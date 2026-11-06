// Cinematic mode: flight phase, phase-weighted view choice and dwell clock (pure), and the ModuleApp glue that runs
// the session-wide playlist (doc/dual-bindings.md#cinematic-mode).
#include "cinematic.hpp"
#include "module_app.hpp"

#include "fscs/profiles/json_util.hpp"

#include <algorithm>
#include <cmath>

namespace fscs::host {

namespace cinematic {

const char* toString(Phase p) {
    switch (p) {
        case Phase::Parked: return "parked";
        case Phase::Taxi: return "taxi";
        case Phase::TakeoffClimb: return "takeoffClimb";
        case Phase::Cruise: return "cruise";
        case Phase::Approach: return "approach";
        case Phase::LandingRoll: return "landingRoll";
    }
    return "parked";
}

Phase PhaseTracker::update(const core::FlightMotion& m, double now) {
    if (!m.finite()) return phase_;
    const bool ground = m.onGround > .5;
    const double speed = std::max(0., m.groundSpeed), climb = m.verticalSpeed;
    if (!known_) {
        known_ = true;
        airborne_ = !ground;
    } else if (airborne_ && ground) {
        airborne_ = false;
        landing_ = true;  // touchdown: the landing roll lasts until the aircraft slows to taxi speed
    } else if (!airborne_ && !ground) {
        airborne_ = true;
        landing_ = false;
        liftoffAt_ = now;
    }
    if (ground) {
        if (landing_ && speed >= kRollExit) phase_ = Phase::LandingRoll;
        else {
            landing_ = false;
            if (speed >= (phase_ == Phase::TakeoffClimb ? kRollExit : kRollEnter)) phase_ = Phase::TakeoffClimb;  // takeoff roll
            else if (speed >= (phase_ == Phase::Parked ? kParkedExit : kParkedEnter)) phase_ = Phase::Taxi;
            else phase_ = Phase::Parked;
        }
    } else if (now - liftoffAt_ < kAfterLiftoffSeconds || climb >= (phase_ == Phase::TakeoffClimb ? kClimbExit : kClimbEnter)) {
        phase_ = Phase::TakeoffClimb;
    } else if (climb <= (phase_ == Phase::Approach ? kDescentExit : kDescentEnter)) {
        phase_ = Phase::Approach;
    } else {
        phase_ = Phase::Cruise;
    }
    return phase_;
}

double typeWeight(Phase phase, profiles::CameraMode mode) {
    using profiles::CameraMode;
    //                               exterior orbit flyby airport mounted
    static constexpr double table[6][5] = {{3.0, 2.0, 1.0, 3.0, 1.0},   // parked
                                           {3.0, 1.0, 1.0, 3.0, 1.5},   // taxi
                                           {3.0, 1.0, 2.0, 2.5, 2.0},   // takeoff roll and climb-out
                                           {3.0, 2.5, 2.5, 0.0, 1.5},   // cruise
                                           {2.5, 1.0, 2.0, 3.0, 1.5},   // descent and approach
                                           {3.0, 0.5, 1.0, 3.0, 2.0}};  // landing roll
    int column = -1;
    switch (mode) {
        case CameraMode::AircraftExterior: column = 0; break;
        case CameraMode::Orbit: column = 1; break;
        case CameraMode::Flyby: column = 2; break;
        case CameraMode::Airport: column = 3; break;
        case CameraMode::Mounted: column = 4; break;
        case CameraMode::AircraftCockpit:
        case CameraMode::Free: return 0;
    }
    return column < 0 ? 0 : table[static_cast<int>(phase)][column];
}

namespace {
bool favoursAirportFeatures(Phase p) { return p != Phase::Cruise && p != Phase::TakeoffClimb; }
}  // namespace

Director::Director(Random random) : random_(std::move(random)) {
    profiles::randomBytes(&state_, sizeof state_);
}

void Director::setRandom(Random random) { random_ = std::move(random); }

double Director::random() {
    if (random_) {
        const double r = random_();
        return std::isfinite(r) ? core::clamp(r, 0., std::nextafter(1., 0.)) : 0.;
    }
    // SplitMix64: deterministic per seed, no platform random device needed in the simulator.
    std::uint64_t z = (state_ += 0x9e3779b97f4a7c15ull);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
    z ^= z >> 31;
    return static_cast<double>(z >> 11) * (1.0 / 9007199254740992.0);
}

std::string Director::pick(const std::vector<Candidate>& candidates, const std::string& current, Phase phase, double cockpitWeight) {
    if (candidates.empty()) return {};
    if (candidates.size() == 1) return candidates.front().id;
    std::vector<const Candidate*> cockpit, others;
    for (const auto& c : candidates) {
        if (c.id == current) continue;  // never the same view twice in a row
        (c.mode == profiles::CameraMode::AircraftCockpit ? cockpit : others).push_back(&c);
    }
    if (cockpit.empty() && others.empty()) return candidates.front().id;
    const auto recent = [&](const std::string& id) { return std::find(history_.begin(), history_.end(), id) != history_.end(); };
    // Phase weights are shares per view type, split between that type's views. Airport tower and runway views
    // count double on the ground and on approach.
    std::vector<double> weights(others.size(), 0.);
    for (std::size_t i = 0; i < others.size(); ++i) {
        const auto* c = others[i];
        const auto feature = [&](const Candidate& v) {
            return v.mode == profiles::CameraMode::Airport && favoursAirportFeatures(phase) &&
                           (v.airportCategory == "tower" || v.airportCategory == "runways")
                       ? kAirportFeatureBoost
                       : 1.;
        };
        double sameType = 0;
        for (const auto* o : others)
            if (o->mode == c->mode) sameType += feature(*o);
        weights[i] = typeWeight(phase, c->mode) * feature(*c) / sameType * (recent(c->id) ? kRecentPenalty : 1.);
    }
    double othersTotal = 0;
    for (double w : weights) othersTotal += w;
    if (othersTotal <= 0 && cockpit.empty())  // nothing suits this phase: any non-cockpit view rather than none
        for (std::size_t i = 0; i < others.size(); ++i) othersTotal += weights[i] = recent(others[i]->id) ? kRecentPenalty : 1.;
    const double share = core::clamp(std::isfinite(cockpitWeight) ? cockpitWeight : 0., 0., 1.);
    const double roll = random();
    const bool useCockpit = !cockpit.empty() && (othersTotal <= 0 || roll < share);
    const auto draw = [&](const std::vector<const Candidate*>& pool, const std::vector<double>& w, double total) {
        double r = random() * total;
        for (std::size_t i = 0; i < pool.size(); ++i) {
            if (w[i] <= 0) continue;
            if (r < w[i]) return pool[i]->id;
            r -= w[i];
        }
        for (std::size_t i = pool.size(); i-- > 0;)  // rounding at the very top of the range
            if (w[i] > 0) return pool[i]->id;
        return pool.front()->id;
    };
    if (useCockpit) {
        std::vector<double> w(cockpit.size());
        double total = 0;
        for (std::size_t i = 0; i < cockpit.size(); ++i) total += w[i] = recent(cockpit[i]->id) ? kRecentPenalty : 1.;
        return draw(cockpit, w, total);
    }
    return draw(others, weights, othersTotal);
}

double Director::drawDwell(double minSeconds, double maxSeconds) {
    const double lo = core::clamp(std::isfinite(minSeconds) ? minSeconds : 8., profiles::kCinematicMinDwellSeconds, profiles::kCinematicMaxDwellSeconds);
    const double hi = core::clamp(std::isfinite(maxSeconds) ? maxSeconds : 20., lo, profiles::kCinematicMaxDwellSeconds);
    return lo + (hi - lo) * random();
}

core::DriftSpec Director::drawDrift(const core::Quat& orientation, double seconds) {
    const int axis = random() < .5 ? 0 : 1;
    const double sign = random() < .5 ? -1. : 1.;
    const double fovSign = random() < .5 ? -1. : 1.;
    core::DriftSpec spec;
    spec.direction = core::driftDirection(orientation, axis, sign);
    spec.seconds = seconds;
    spec.meters = std::min(core::kDriftMaxMeters, core::kDriftSpeedMetersPerSec * seconds);
    spec.fovDeltaRad = fovSign * core::kDriftMaxFovDeg * core::kDegToRad;
    return spec;
}

void Director::shown(const std::string& id, double dwellSeconds, double now) {
    cameraId_ = id;
    dwell_ = dwellSeconds;
    elapsed_ = 0;
    lastTick_ = now;
    history_.erase(std::remove(history_.begin(), history_.end(), id), history_.end());
    history_.push_front(id);
    while (history_.size() > kHistory) history_.pop_back();
}

bool Director::tick(double now, bool suspended) {
    const double dt = lastTick_ < 0 ? 0. : core::clamp(now - lastTick_, 0., .25);
    lastTick_ = now;
    if (cameraId_.empty() || suspended || paused(now)) return false;
    elapsed_ += dt;
    return elapsed_ >= dwell_;
}

void Director::reset() {
    cameraId_.clear();
    history_.clear();
    elapsed_ = dwell_ = 0;
    lastTick_ = pausedUntil_ = -1;
}

}  // namespace cinematic

// ---------------------------------------------------------------------------------------------------------------
// ModuleApp: the session-wide playlist. Like airport Auto switch it is module state, not a saved setting.

json ModuleApp::cinematicState() const {
    const char* state = !cinematicOn_ ? "off"
                        : coord_.ownership() == Ownership::Acquiring ? "starting"
                        : cinematicSuspended_ ? "suspended"
                        : cinematic_.paused(now_) ? "paused"
                                                  : "running";
    return {{"enabled", cinematicOn_},
            {"state", state},
            {"phase", cinematic::toString(cinematicPhase_.phase())},
            {"cameraId", cinematicOn_ && !cinematic_.cameraId().empty() ? json(cinematic_.cameraId()) : json(nullptr)},
            {"dwellSeconds", cinematicOn_ ? cinematic_.dwell() : 0.},
            {"reason", cinematicReason_.empty() ? json(nullptr) : json(cinematicReason_)}};
}

void ModuleApp::emitCinematic(bool force) {
    auto state = cinematicState();
    const std::string key = js::dump({state["enabled"], state["state"], state["phase"], state["cameraId"]});
    if (!force && key == cinematicEmitted_) return;
    cinematicEmitted_ = key;
    emitState("CinematicChanged", std::move(state));
}

bool ModuleApp::cinematicSwitch(double now, bool acquire) {
    const auto* profile = coord_.profile();
    if (!profile) return false;
    const auto settings = profiles::parseHostSettings(working_.settings);
    std::vector<cinematic::Candidate> list;
    for (const auto& cam : profile->cameras)
        if (cam.cinematic && profiles::modeActivatable(cam.mode) && cam.mode != profiles::CameraMode::Free)
            list.push_back({cam.id, cam.mode, cam.airportHeightCategory});
    const std::string current = coord_.activeCameraId();
    while (!list.empty()) {
        const auto id = cinematic_.pick(list, current, cinematicPhase_.phase(), settings.cinematicCockpitWeight);
        const auto* cam = profile->camera(id);
        if (!cam) return false;
        const double dwell = cinematic_.drawDwell(settings.cinematicMinSeconds, settings.cinematicMaxSeconds);
        if (id == current && !acquire) {  // the only view left: keep it for another dwell
            cinematic_.shown(id, dwell, now);
            return true;
        }
        const bool world = cam->mode == profiles::CameraMode::Airport || cam->mode == profiles::CameraMode::Flyby;
        // Commercial drift: exterior and airport views only; other views get an empty drift (a carried offset still fades).
        core::DriftSpec drift;
        drift.seconds = dwell;
        if (settings.cinematicDrift && (cam->mode == profiles::CameraMode::AircraftExterior || cam->mode == profiles::CameraMode::Airport))
            drift = cinematic_.drawDrift(cam->pose.orientation, dwell);
        if (coord_.selectCinematicCamera(id, now, acquire, settings.cinematicCuts && world, drift)) {
            cinematicRecalls_ = coord_.recalls();
            cinematic_.shown(id, dwell, now);
            diag("cinematic: " + std::string(cinematic::toString(cinematicPhase_.phase())) + " -> " + cam->name);
            return true;
        }
        list.erase(std::remove_if(list.begin(), list.end(), [&](const cinematic::Candidate& c) { return c.id == id; }), list.end());
    }
    return false;
}

bool ModuleApp::setCinematic(bool enabled, double now, std::string* why) {
    if (enabled == cinematicOn_) return true;
    if (!enabled) {
        stopCinematic("Cinematic mode was turned off.", now);
        return true;
    }
    const auto fail = [&](const std::string& reason) {
        if (why) *why = reason;
        return false;
    };
    if (!simRunning_) return fail("Load a flight before starting Cinematic mode.");
    if (!coord_.canActivate()) return fail(coord_.blockReason());
    const auto state = coord_.ownership();
    if (state == Ownership::Releasing || state == Ownership::Acquiring)
        return fail("Camera Studio is changing camera control. Try again in a moment.");
    if (airportAuto_) setAirportAuto(false, now);  // one automatic mode at a time
    cinematic_.reset();
    cinematicReason_.clear();
    cinematicSuspended_ = false;
    cinematicOn_ = true;
    if (!cinematicSwitch(now, state != Ownership::Active)) {
        cinematicOn_ = false;
        cinematic_.reset();
        emitCinematic(true);
        const auto* profile = coord_.profile();
        const bool any = profile && std::any_of(profile->cameras.begin(), profile->cameras.end(), [](const profiles::CameraEntry& c) {
            return c.cinematic && profiles::modeActivatable(c.mode) && c.mode != profiles::CameraMode::Free;
        });
        return fail(any ? "Camera Studio could not show a Cinematic mode view right now. Try again in a moment."
                        : "No views are included in Cinematic mode. Turn on Include in Cinematic mode for exterior, airport or cockpit views.");
    }
    diag("cinematic mode on");
    emitCinematic(true);
    return true;
}

void ModuleApp::stopCinematic(const std::string& reason, double) {
    if (!cinematicOn_) return;
    cinematicOn_ = false;
    cinematicSuspended_ = false;
    cinematicReason_ = reason;
    cinematic_.reset();
    coord_.stopCinematic();
    diag("cinematic mode off: " + reason);
    emitCinematic(true);
}

void ModuleApp::updateCinematic(double now, bool manual) {
    if (!cinematicOn_) return;
    if (manual) cinematic_.manualInput(now);
    if (airportAuto_) return stopCinematic("Airport Auto switch was turned on.", now);
    const auto state = coord_.ownership();
    if (state == Ownership::Acquiring) return emitCinematic();  // waiting for the simulator to hand over the camera
    // Release Camera, Panic, a hand-back and any lost control stop the playlist; a simulator pause only pauses it.
    if (state != Ownership::Active && state != Ownership::SimulatorSuspended) return stopCinematic("Camera control was released.", now);
    // Any recall Cinematic mode did not make (a view binding or button, Next camera, peek, walkaround) stops it.
    if (coord_.recalls() != cinematicRecalls_) return stopCinematic("A camera was selected manually.", now);
    const bool glance = !coord_.glanceJson().is_null();
    if (!glance && state == Ownership::Active && coord_.activeCameraId() != cinematic_.cameraId())
        return stopCinematic("The cinematic view is no longer shown.", now);
    // Glance and binoculars hold the playlist on the current view; it continues when they end.
    cinematicSuspended_ = state != Ownership::Active || glance || coord_.binocularsHeld() || paused_ || !simRunning_ || coord_.gameControlled();
    coord_.holdCinematicDrift(cinematicSuspended_ || cinematic_.paused(now));
    if (cinematic_.tick(now, cinematicSuspended_) && !cinematicSwitch(now, false)) {
        const auto settings = profiles::parseHostSettings(working_.settings);
        cinematic_.shown(cinematic_.cameraId(), cinematic_.drawDwell(settings.cinematicMinSeconds, settings.cinematicMaxSeconds), now);
    }
    emitCinematic();
}

void ModuleApp::cinematicEvent(const BackendEvent& ev, double now) {
    if (const auto* m = std::get_if<backend_event::MotionTelemetry>(&ev)) {
        cinematicPhase_.update(m->motion, now);  // tracked while off too, so the first view suits the phase
        return;
    }
    if (const auto* b = std::get_if<backend_event::NativeMouseButton>(&ev)) {
        if (b->down && cinematicOn_) cinematic_.manualInput(now);  // right-drag mouse look
        return;
    }
    if (std::holds_alternative<backend_event::FlightLoaded>(ev) || std::holds_alternative<backend_event::Disconnected>(ev))
        cinematicPhase_.reset();
}

bool ModuleApp::handleCinematic(const protocol::Command& c, double now) {
    if (c.action == "MouseLookDelta") {
        if (cinematicOn_) cinematic_.manualInput(now);  // panel mouse look: forwarded to the coordinator as usual
        return false;
    }
    if (c.action != "SetCinematic") return false;
    std::string why;
    const bool ok = setCinematic(js::boolean(c.payload, "enabled", false), now, &why);
    reply(c, ok ? protocol::AckStatus::Completed : protocol::AckStatus::Rejected, why, {{"cinematic", cinematicState()}});
    return true;
}

void ModuleApp::cinematicPanic(json& payload, double now) {
    payload["cinematicStopped"] = cinematicOn_;
    stopCinematic("Panic release.", now);
}

}  // namespace fscs::host
