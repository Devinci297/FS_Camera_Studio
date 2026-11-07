// Spotting, the module side: the traffic feed (a SimConnect poll about every 1.5 s while a panel is visible or
// spotting is on, plus per-frame data for the target), TrafficChanged at most once a second, the scenery data for the
// computed runway and tower views, the SpotTarget / StopSpotting / SetAutoSpot commands, the spotting bindings and
// session-wide auto-spot. The coordinator presents the views (coordinator.cpp).

#include "module_app.hpp"

#include "fscs/profiles/json_util.hpp"

#include <algorithm>
#include <limits>

namespace fscs::host {

using protocol::AckStatus;
using traffic::SpotView;
using traffic::Status;

namespace {

// Auto-spot: what is worth showing at the airport, best first; -1 = not a candidate.
int autoSpotRank(Status s) {
    switch (s) {
        case Status::Landing: return 0;
        case Status::Takeoff: return 1;
        case Status::Arriving: return 2;
        case Status::Departing: return 3;
        default: return -1;
    }
}

}  // namespace

profiles::airport::Geo ModuleApp::trafficAirport() const {
    // The loaded airport set (saved or temporary), else the nearest airport scenery reported.
    const auto it = airportDrafts_.find(loadedAirport_);
    if (it != airportDrafts_.end()) {
        const auto g = profiles::airport::geo(js::get(js::object(it->second, "airport"), "origin"));
        if (g.valid()) return g;
    }
    for (const auto& a : nearbyFacilities_) {
        auto g = profiles::airport::geo(a);
        if (!std::isfinite(g.altitude)) g.altitude = 0;
        if (g.valid()) return g;
    }
    return {NAN, NAN, NAN};
}

json ModuleApp::trafficState() const { return coord_.traffic().toJson(position_, now_); }

void ModuleApp::updateSpotAirports(double now) {
    if (now < nextSpotAirports_) return;
    nextSpotAirports_ = now + kSpotAirportsSeconds;
    // The loaded (else the nearest) airport, and the scenery airport nearest the target: their runway ends and towers.
    std::vector<std::string> codes;
    const auto add = [&](const std::string& code) {
        if (profiles::airport::validCode(code) && std::find(codes.begin(), codes.end(), code) == codes.end()) codes.push_back(code);
    };
    add(loadedAirport_);
    if (loadedAirport_.empty() && !nearbyFacilities_.empty()) add(js::str(nearbyFacilities_[0], "icao"));
    const auto target = coord_.spotPose();
    if (coord_.spotting() && target.valid()) {
        std::string nearest;
        double best = traffic::kSpotAirportRangeMeters / traffic::kMetersPerNm;
        for (const auto& a : nearbyFacilities_) {
            const double d = profiles::airport::distanceNm(profiles::airport::geo(a), target.position);
            if (d < best) { best = d; nearest = js::str(a, "icao"); }
        }
        add(nearest);
    }
    std::string key;
    std::vector<traffic::SpotAirport> airports;
    for (const auto& code : codes) {
        const auto cached = airportFacilityCache_.find(code);
        if (cached == airportFacilityCache_.end()) {
            // Rate limited and cached for the session. With temporary airport sets turned off, only while spotting.
            if (airportAutoGenerate() || autoSpot_ || coord_.spotting()) requestAutoAirport(code, now);
            key += code + "?;";
        } else if (cached->second.is_object()) {
            airports.push_back(traffic::spotAirport(cached->second));
            key += code + ";";
        }
    }
    if (key == spotAirportsKey_) return;
    spotAirportsKey_ = key;
    coord_.setSpotAirports(std::move(airports));
}

json ModuleApp::spottingState() const {
    const bool on = coord_.spotting();
    const char* state = on ? (coord_.spotLost() ? "lost" : "tracking") : autoSpot_ ? "waiting" : "off";
    json runways = json::array();
    for (const auto& a : coord_.spotAirports())
        for (const auto& e : a.ends)
            runways.push_back({{"icao", e.icao}, {"end", e.name}, {"headingDeg", std::round(e.headingDeg * 10) / 10},
                               {"lengthMeters", std::round(e.lengthMeters)}, {"widthMeters", std::round(e.widthMeters * 10) / 10}});
    // The runway spot of a computed runway view, or the position spot of the spot view; `pinned`: chosen by the panel.
    json spot = nullptr;
    if (const auto place = coord_.spotPlace()) spot = traffic::toJson(*place);
    else if (const auto runway = coord_.spotRunway()) spot = traffic::toJson(*runway);
    if (spot.is_object()) spot["pinned"] = coord_.spotPinned();
    return {{"targetId", on ? json(coord_.spotTargetId()) : json(nullptr)},
            {"view", on && coord_.spotView() ? json(traffic::toString(*coord_.spotView())) : json(nullptr)},
            {"autoSpot", autoSpot_},
            {"state", state},
            {"targetLabel", on ? json(coord_.spotLabel()) : json(nullptr)},
            {"reason", spotReason_.empty() ? json(nullptr) : json(spotReason_)},
            {"views", coord_.spotViewsJson()},
            {"cameraName", coord_.spotCameraName()},
            {"runways", runways},
            {"spot", spot}};
}

void ModuleApp::emitSpotting(bool force) {
    auto state = spottingState();
    const std::string key = js::dump(state);
    if (!force && key == spottingEmitted_) return;
    spottingEmitted_ = key;
    emitState("SpottingChanged", std::move(state));
}

std::string ModuleApp::spot(std::uint32_t objectId, SpotView view, double now, const std::optional<traffic::RunwaySpot>& runway,
                            const std::optional<traffic::PositionSpot>& place) {
    if (!simRunning_) return "Load a flight before spotting traffic.";
    const auto error = coord_.spotTarget(objectId, view, now, runway, place);
    if (!error.empty()) return error;
    // One automatic mode at a time: spotting stops Cinematic mode and airport Auto switch.
    if (cinematicOn_) stopCinematic("Spotting started.", now);
    if (airportAuto_) setAirportAuto(false, now);
    spotReason_.clear();
    diag("spotting: " + coord_.spotLabel() + " (" + traffic::toString(view) + ")");
    emitSpotting();
    return {};
}

void ModuleApp::stopAutoSpot(const std::string& reason) {
    if (!autoSpot_) return;
    autoSpot_ = false;
    coord_.setAutoSpot(false);
    spotReason_ = reason;
    diag("auto-spot off: " + reason);
}

void ModuleApp::yieldSpotting(const std::string& reason) {
    if (!autoSpot_ && !coord_.spotting()) return;
    stopAutoSpot(reason);
    coord_.endSpotting(reason, now_);  // the other mode takes over from the view underneath
    spotReason_ = reason;
    emitSpotting();
}

bool ModuleApp::setAutoSpot(bool enabled, double now, std::string* why) {
    if (enabled == autoSpot_) return true;
    if (!enabled) {
        stopAutoSpot("Auto-spot was turned off.");
        emitSpotting();
        return true;
    }
    const auto fail = [&](const std::string& reason) {
        if (why) *why = reason;
        return false;
    };
    if (!simRunning_) return fail("Load a flight before turning on auto-spot.");
    // No camera profile needed: chase, fly-by and the computed runway and tower views work without one.
    if (!coord_.spottingAvailable()) return fail(coord_.spottingBlockReason());
    if (cinematicOn_) stopCinematic("Auto-spot was turned on.", now);
    if (airportAuto_) setAirportAuto(false, now);
    autoSpot_ = true;
    coord_.setAutoSpot(true);
    spotReason_.clear();
    lastAutoSpotCheck_ = -100;  // choose on the next frame, after a fresh poll
    nextTrafficPoll_ = std::min(nextTrafficPoll_, now);
    autoSpotSince_ = now;  // a target already spotted is held first
    autoSpotViewAt_ = -1e9;
    diag("auto-spot on");
    emitSpotting();
    return true;
}

void ModuleApp::spotCycle(int delta, double now) {
    const auto ids = coord_.traffic().byDistance(position_, now);
    if (ids.empty()) {
        spotReason_ = "No traffic in range.";
        emitSpotting();
        return;
    }
    // By distance from the user aircraft, keeping the view type (chase to start with).
    const auto current = std::find(ids.begin(), ids.end(), coord_.spotTargetId());
    const auto n = static_cast<long>(ids.size());
    const long i = !coord_.spotting() || current == ids.end() ? (delta > 0 ? 0 : n - 1)
                                                              : (((current - ids.begin()) + delta) % n + n) % n;
    const auto id = ids[static_cast<std::size_t>(i)];
    const SpotView view = coord_.spotView().value_or(SpotView::Chase);
    // A pinned runway spot or a position spot stays where it is for the next aircraft.
    const auto runway = coord_.spotPinned() && view == SpotView::Runway ? coord_.spotRunway() : std::nullopt;
    const auto place = view == SpotView::Spot ? coord_.spotPlace() : std::nullopt;
    stopAutoSpot("A target was chosen manually.");
    auto error = spot(id, view, now, runway, place);
    if (!error.empty() && view != SpotView::Chase) error = spot(id, SpotView::Chase, now);
    if (!error.empty()) spotReason_ = error;
    emitSpotting();
}

SpotView ModuleApp::autoSpotView(std::uint32_t objectId, SpotView fallback, double now) const {
    const auto* e = coord_.traffic().find(objectId);
    if (!e) return fallback;
    SpotView wanted;
    switch (e->status) {
        case Status::Arriving:
        case Status::Landing:
        case Status::Takeoff: wanted = SpotView::Runway; break;
        case Status::Taxi: wanted = SpotView::Tower; break;
        case Status::Departing: return SpotView::Chase;
        default: return fallback;
    }
    // Where the airport has no such view: the other airport view, then the chase view.
    const SpotView other = wanted == SpotView::Runway ? SpotView::Tower : SpotView::Runway;
    if (coord_.spotViewAvailable(wanted, objectId, now)) return wanted;
    if (coord_.spotViewAvailable(other, objectId, now)) return other;
    return SpotView::Chase;
}

std::optional<std::uint32_t> ModuleApp::autoSpotPick(std::uint32_t exclude, double now) const {
    // Landing and takeoff rolls first, then the arrival closest to touchdown, then the nearest departure: aircraft
    // within 15 NM of the loaded (or nearest) airport only.
    const auto& model = coord_.traffic();
    std::optional<std::uint32_t> best;
    int bestRank = std::numeric_limits<int>::max();
    double bestDistance = std::numeric_limits<double>::infinity();
    for (const auto id : model.byDistance(position_, now)) {
        const auto* e = model.find(id);
        const int rank = e ? autoSpotRank(e->status) : -1;
        if (id == exclude || rank < 0) continue;
        const double d = model.airportDistanceNm(id, now);
        if (!(d <= traffic::kAirportRadiusNm)) continue;
        if (rank < bestRank || (rank == bestRank && d < bestDistance)) {
            best = id;
            bestRank = rank;
            bestDistance = d;
        }
    }
    return best;
}

bool ModuleApp::autoSpotShow(std::uint32_t objectId, double now) {
    const SpotView view = autoSpotView(objectId, SpotView::Chase, now);
    auto error = spot(objectId, view, now);
    if (!error.empty() && view != SpotView::Chase) error = spot(objectId, SpotView::Chase, now);
    if (!error.empty()) {
        diag("auto-spot: " + error);
        return false;
    }
    autoSpotSince_ = autoSpotViewAt_ = now;
    return true;
}

void ModuleApp::updateAutoSpot(double now) {
    if (!autoSpot_ || now - lastAutoSpotCheck_ < kAutoSpotCheckSeconds) return;
    lastAutoSpotCheck_ = now;
    if (!simRunning_ || paused_ || coord_.gameControlled()) return;
    const auto state = coord_.ownership();
    if (state == Ownership::Releasing || state == Ownership::Acquiring) return;
    const double hold = std::clamp(js::num(js::object(working_.settings, "host"), "autoSpotHoldSeconds", 30.0),
                                   profiles::kAutoSpotMinHoldSeconds, profiles::kAutoSpotMaxHoldSeconds);
    if (coord_.spotting()) {
        const auto current = coord_.spotTargetId();
        const bool gone = coord_.spotLost() && coord_.spotLostSeconds(now) >= Coordinator::kSpotLostHoldSeconds;
        if (!gone && now - autoSpotSince_ < hold) {
            // While held the view follows what the aircraft does: the runway for a landing, the tower once it taxis.
            const auto view = coord_.spotView().value_or(SpotView::Chase);
            if (!coord_.spotLost() && now - autoSpotViewAt_ >= kAutoSpotViewSeconds) {
                const auto wanted = autoSpotView(current, view, now);
                if (wanted != view && spot(current, wanted, now).empty()) autoSpotViewAt_ = now;
            }
            return;
        }
        if (const auto next = autoSpotPick(current, now); next && autoSpotShow(*next, now)) return;
        const auto* e = coord_.traffic().find(current);
        if (!gone && e && autoSpotRank(e->status) >= 0) {  // still the best there is: hold it again
            autoSpotSince_ = now;
            return;
        }
        coord_.stopSpotting(gone ? "The aircraft is no longer in range." : "Waiting for arriving or departing traffic.", now);
        spotReason_ = "Waiting for arriving or departing traffic.";
        return;
    }
    if (!coord_.spottingAvailable()) return;
    if (const auto next = autoSpotPick(0, now)) autoSpotShow(*next, now);
}

void ModuleApp::updateSpotting(double now) {
    coord_.setAutoSpot(autoSpot_);
    // Bindings arrive inside the coordinator's own callback: handled here, never re-entering it from there.
    if (pendingAutoSpotToggle_) {
        pendingAutoSpotToggle_ = false;
        std::string why;
        if (!setAutoSpot(!autoSpot_, now, &why)) spotReason_ = why;
    }
    if (pendingSpotReturn_) {
        pendingSpotReturn_ = false;
        stopAutoSpot("Back to your aircraft.");
        coord_.stopSpotting("Back to your aircraft.", now);
    }
    if (pendingSpotCycle_) {
        const int delta = pendingSpotCycle_;
        pendingSpotCycle_ = 0;
        spotCycle(delta, now);
    }
    // The feed runs while someone can see it or spotting needs it.
    if (simRunning_ && (autoSpot_ || coord_.spotting() || anyVisibleLease()) && now >= nextTrafficPoll_) {
        nextTrafficPoll_ = now + kTrafficPollSeconds;
        coord_.setTrafficAirport(trafficAirport());
        const double radiusNm = std::clamp(js::num(js::object(working_.settings, "host"), "spottingRadiusNm", 20.0),
                                           profiles::kSpottingMinRadiusNm, profiles::kSpottingMaxRadiusNm);
        services_.requestTraffic(std::min(radiusNm * traffic::kMetersPerNm, kTrafficMaxRadiusMeters));
    }
    if (simRunning_ && (autoSpot_ || coord_.spotting() || anyVisibleLease())) updateSpotAirports(now);
    if (anyVisibleLease() && coord_.traffic().revision() != trafficEmitted_ && now - lastTrafficEmit_ >= kTrafficEmitSeconds) {
        trafficEmitted_ = coord_.traffic().revision();
        lastTrafficEmit_ = now;
        emitState("TrafficChanged", trafficState());
    }
    updateAutoSpot(now);
    // Per-frame data for the target only: started, changed and stopped with spotting (release, panic, stop, lost).
    const std::uint32_t watch = coord_.spotting() ? coord_.spotTargetId() : 0;
    if (watch != watchedTarget_) {
        watchedTarget_ = watch;
        services_.watchTrafficTarget(watch);
        diag(watch ? "spotting: per-frame data for object " + std::to_string(watch) : std::string("spotting: per-frame data stopped"));
    }
    emitSpotting();
}

bool ModuleApp::spottingEvent(const std::string& name, json& payload) {
    if (name == "SpotAdjusted") {
        stopAutoSpot("The spotting camera was adjusted manually.");
        emitSpotting();
        return true;
    }
    if (name == "SpotStopped") {
        spotReason_ = js::str(payload, "reason");
        // Another recall, Release, Panic and a loss of control end auto-spot too.
        if (js::boolean(payload, "manual", false) || js::boolean(payload, "released", false)) stopAutoSpot(spotReason_);
        return true;  // reported through SpottingChanged
    }
    if (name == "SpotCycleRequested") { pendingSpotCycle_ = js::integer(payload, "delta", 1) < 0 ? -1 : 1; return true; }
    if (name == "SpotReturnRequested") { pendingSpotReturn_ = true; return true; }
    if (name == "AutoSpotToggleRequested") { pendingAutoSpotToggle_ = true; return true; }
    if (name == "PanicReleased") {
        payload["spottingStopped"] = js::boolean(payload, "spottingStopped", false) || autoSpot_;
        if (js::boolean(payload, "spottingStopped", false)) spotReason_ = "Panic release.";
        stopAutoSpot("Panic release.");
    }
    return false;
}

std::string ModuleApp::parseSpot(const json& value, std::optional<traffic::RunwaySpot>& runway, std::optional<traffic::PositionSpot>& place) const {
    std::string error;
    if (js::str(value, "kind") == "position") place = traffic::parsePositionSpot(value, error);
    else runway = traffic::parseRunwaySpot(value, coord_.spotAirports(), coord_.runwayCameraHeight(), error);
    return error;
}

bool ModuleApp::handleSpotting(const protocol::Command& c, double now) {
    const auto& a = c.action;
    const auto& p = c.payload;
    if (a == "SpotTarget") {
        const auto id = js::integer(p, "objectId", 0);
        const auto view = traffic::parseSpotView(js::str(p, "view"));
        const auto& value = js::get(p, "spot");
        const bool position = js::str(value, "kind") == "position";
        std::string error;
        std::optional<traffic::RunwaySpot> runway;
        std::optional<traffic::PositionSpot> place;
        if (id <= 0 || id > 0xffffffffLL) error = "Choose an aircraft from the traffic list.";
        else if (!view) error = "Choose a valid traffic viewpoint.";
        else if (*view == SpotView::Spot && !position) error = "The spot view needs a position spot.";
        else if (!value.is_null() && position && *view != SpotView::Spot) error = "A position spot can only be used with the spot view.";
        else if (!value.is_null() && !position && *view != SpotView::Runway) error = "A runway spot can only be used with the runway view.";
        else if (!value.is_null() && !(error = parseSpot(value, runway, place)).empty()) {}
        else error = spot(static_cast<std::uint32_t>(id), *view, now, runway, place);
        if (error.empty()) stopAutoSpot("A target was chosen manually.");  // a manual choice ends auto-spot
        emitSpotting();
        reply(c, error.empty() ? AckStatus::Completed : AckStatus::Rejected, error, {{"spotting", spottingState()}});
        return true;
    }
    if (a == "AdjustSpot") {
        // Live edits of the pinned spot being shown: same target, same view, no restart.
        std::optional<traffic::RunwaySpot> runway;
        std::optional<traffic::PositionSpot> place;
        std::string error = js::get(p, "spot").is_object() ? parseSpot(js::get(p, "spot"), runway, place) : "Send the spot to adjust.";
        if (error.empty()) error = coord_.adjustSpot(runway, place, now);
        emitSpotting();
        reply(c, error.empty() ? AckStatus::Completed : AckStatus::Rejected, error, {{"spotting", spottingState()}});
        return true;
    }
    if (a == "StopSpotting") {
        stopAutoSpot("Spotting stopped.");
        coord_.stopSpotting("Spotting stopped.", now);
        emitSpotting();
        reply(c, AckStatus::Completed, {}, {{"spotting", spottingState()}});
        return true;
    }
    if (a == "SetAutoSpot") {
        std::string why;
        const bool ok = setAutoSpot(js::boolean(p, "enabled", false), now, &why);
        reply(c, ok ? AckStatus::Completed : AckStatus::Rejected, why, {{"spotting", spottingState()}});
        return true;
    }
    return false;
}

}  // namespace fscs::host
