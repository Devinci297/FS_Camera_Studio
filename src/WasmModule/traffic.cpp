#include "traffic.hpp"

#include "fscs/profiles/json_util.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <limits>

namespace fscs::host::traffic {

using profiles::airport::local;
using profiles::airport::world;

const char* toString(Status s) {
    switch (s) {
        case Status::Parked: return "parked";
        case Status::Taxi: return "taxi";
        case Status::Takeoff: return "takeoff";
        case Status::Departing: return "departing";
        case Status::Arriving: return "arriving";
        case Status::Landing: return "landing";
        case Status::Cruise: return "cruise";
    }
    return "cruise";
}

Status classify(bool onGround, double groundSpeedKt, double verticalSpeedFpm, bool airborneRecently, bool nearAirport) {
    if (onGround) {
        if (!(groundSpeedKt >= kParkedKt)) return Status::Parked;
        if (groundSpeedKt < kRollKt) return Status::Taxi;
        return airborneRecently ? Status::Landing : Status::Takeoff;
    }
    if (nearAirport && verticalSpeedFpm > kClimbFpm) return Status::Departing;
    if (nearAirport && verticalSpeedFpm < -kClimbFpm) return Status::Arriving;
    return Status::Cruise;
}

std::string cleanAtcText(const std::string& text) {
    std::string s = text;
    const auto strip = [&](const std::string& prefix) {
        if (s.size() >= prefix.size() && std::equal(prefix.begin(), prefix.end(), s.begin(), [](char a, char b) {
                return std::toupper(static_cast<unsigned char>(a)) == std::toupper(static_cast<unsigned char>(b));
            })) s.erase(0, prefix.size());
    };
    strip("TT:");
    const bool key = s.rfind("ATCCOM.", 0) == 0;
    if (key) {
        s.erase(0, 7);
        // "...<name>.0.text": drop the variant number and the ".text" suffix.
        if (s.size() > 5 && s.compare(s.size() - 5, 5, ".text") == 0) s.erase(s.size() - 5);
        const auto dot = s.find_last_of('.');
        if (dot != std::string::npos && dot + 1 < s.size() &&
            std::all_of(s.begin() + static_cast<long>(dot) + 1, s.end(), [](char c) { return std::isdigit(static_cast<unsigned char>(c)); }))
            s.erase(dot);
        for (const char* prefix : {"AC_MODEL_", "AC_MODEL ", "ATC_NAME_", "ATC_NAME "}) strip(prefix);
        std::replace(s.begin(), s.end(), '_', ' ');
    }
    std::string out;
    for (char c : s) {
        const auto u = static_cast<unsigned char>(c);
        if (u < 32) continue;  // fixed-size SimConnect fields: never carry control bytes into the panel JSON
        if (c == ' ' && (out.empty() || out.back() == ' ')) continue;
        out.push_back(c);
    }
    while (!out.empty() && out.back() == ' ') out.pop_back();
    if (out.size() > 64) out.resize(64);
    while (!out.empty() && !js::validUtf8(out)) out.pop_back();  // never split a UTF-8 sequence
    return out;
}

std::string label(const Sample& s) {
    std::string callsign;
    if (!s.airline.empty() && !s.flightNumber.empty()) {
        // An ICAO airline code joins the number ("DLH4AB"); a written-out name keeps a space ("Lufthansa 4AB").
        const bool code = s.airline.size() <= 3 && std::all_of(s.airline.begin(), s.airline.end(), [](char c) { return std::isupper(static_cast<unsigned char>(c)); });
        callsign = s.airline + (code ? "" : " ") + s.flightNumber;
    } else {
        callsign = s.atcId;
    }
    if (callsign.empty()) return !s.title.empty() ? s.title : s.model.empty() ? "Aircraft " + std::to_string(s.objectId) : s.model;
    return s.model.empty() ? callsign : callsign + " \xC2\xB7 " + s.model;
}

double Pose::trackDeg() const {
    return std::hypot(velocity.x, velocity.y) > 2 ? std::atan2(velocity.y, velocity.x) * core::kRadToDeg : headingDeg;
}

// ---------------------------------------------------------------------------------------------------------------
// Model

namespace {

double elapsed(const Model::Track& k, double t) { return core::clamp(t - k.t0, 0.0, Model::kMaxExtrapolationSeconds); }
bool moving(const Model::Track& k, double t) { return t - k.t0 >= 0 && t - k.t0 <= Model::kMaxExtrapolationSeconds; }

struct Shown {
    Geo position;
    core::Vec3 velocity;
    double headingDeg, headingRate, pitchDeg, bankDeg;
};

// Dead reckoning along an arc: the horizontal velocity turns with the heading rate, so a turning target stays on its
// curve between polls instead of drifting outside it.
Shown along(const Model::Track& k, double t) {
    const double dt = elapsed(k, t);
    const bool m = moving(k, t);
    const double w = k.headingRate * core::kDegToRad, a = w * dt;
    const double sinTerm = std::abs(w) > 1e-6 ? std::sin(a) / w : dt, cosTerm = std::abs(w) > 1e-6 ? (1 - std::cos(a)) / w : 0.0;
    const auto& v = k.velocity;
    const core::Vec3 moved{v.x * sinTerm - v.y * cosTerm, v.x * cosTerm + v.y * sinTerm, v.z * dt};
    const core::Vec3 now{v.x * std::cos(a) - v.y * std::sin(a), v.x * std::sin(a) + v.y * std::cos(a), v.z};
    return {world(k.at, moved), m ? now : core::Vec3{}, core::wrapDeg(k.headingDeg + k.headingRate * dt), m ? k.headingRate : 0.0,
            k.pitchDeg, k.bankDeg};
}

// The shown trajectory: smoothstep blend from the display state at the poll into the new poll's trajectory. Position,
// velocity and heading are continuous across the poll, and the blend's derivative is zero at both ends.
Shown shown(const Model::Entry& e, double t) {
    const double u = (t - e.blendStart) / Model::kBlendSeconds;
    if (!(u < 1)) return along(e.to, t);
    const double v = core::clamp(u, 0.0, 1.0);
    const double s = v * v * (3 - 2 * v), ds = u < 0 ? 0.0 : 6 * v * (1 - v) / Model::kBlendSeconds;
    const auto a = along(e.from, t), b = along(e.to, t);
    const auto d = local(a.position, b.position);
    const double dh = core::wrapDeg(b.headingDeg - a.headingDeg);
    return {world(a.position, d * s), a.velocity * (1 - s) + b.velocity * s + d * ds,
            core::wrapDeg(a.headingDeg + dh * s), a.headingRate * (1 - s) + b.headingRate * s + dh * ds,
            a.pitchDeg + (b.pitchDeg - a.pitchDeg) * s, a.bankDeg + core::wrapDeg(b.bankDeg - a.bankDeg) * s};
}

double finiteOr(double v, double fallback) { return std::isfinite(v) ? v : fallback; }

}  // namespace

void Model::update(const std::vector<Sample>& poll, std::uint32_t userObjectId, const Geo& ownship, double now) {
    for (const auto& raw : poll) {
        if (raw.objectId == 0 || raw.objectId == userObjectId) continue;
        const Geo g{raw.lat, raw.lon, raw.altitude};
        if (!g.valid() || !std::isfinite(raw.headingDeg)) continue;
        if (userObjectId == 0 && ownship.valid()) {
            const auto d = local(ownship, g);
            if (std::hypot(d.x, d.y) < 1 && std::abs(d.z) < 3) continue;  // the user aircraft, while its id is unknown
        }
        auto it = entries_.find(raw.objectId);
        if (it == entries_.end()) {
            if (entries_.size() >= kMaxAircraft) continue;
            it = entries_.emplace(raw.objectId, Entry{}).first;
        }
        Entry& e = it->second;
        Sample s = raw;
        s.headingDeg = core::wrapDeg(s.headingDeg);
        s.groundSpeed = std::max(0.0, finiteOr(s.groundSpeed, 0));
        s.verticalSpeed = finiteOr(s.verticalSpeed, 0);
        s.pitchDeg = core::clamp(finiteOr(s.pitchDeg, 0), -90., 90.);
        s.bankDeg = core::wrapDeg(finiteOr(s.bankDeg, 0));
        if (!(std::isfinite(s.groundAltitude) && s.groundAltitude > -1500 && s.groundAltitude < 10000)) s.groundAltitude = NAN;
        for (auto* text : {&s.title, &s.atcId, &s.airline, &s.flightNumber, &s.model}) *text = cleanAtcText(*text);

        // Direction of travel: the heading plus the drift (wind) angle when airborne and moving, else the heading. The
        // drift is the measured track between the last two polls against the heading half-way between them, smoothed
        // over polls since it changes slowly and a single chord is noisy. Turn rate from the last two polls.
        double turn = 0;
        const bool known = e.lastSeen >= 0;
        if (known) {
            const double dt = now - e.lastSeen;
            if (dt >= .3 && dt <= 6) {
                const double dh = core::wrapDeg(s.headingDeg - e.last.headingDeg);
                const auto d = local({e.last.lat, e.last.lon, e.last.altitude}, g);
                if (!s.onGround && s.groundSpeed >= 5 && std::hypot(d.x, d.y) >= 2) {
                    const double drift = core::clamp(core::wrapDeg(std::atan2(d.y, d.x) * core::kRadToDeg - (e.last.headingDeg + dh / 2)),
                                                     -kMaxDriftDeg, kMaxDriftDeg);
                    e.driftDeg = std::isfinite(e.driftDeg) ? e.driftDeg + (drift - e.driftDeg) * kDriftSmoothing : drift;
                }
                turn = core::clamp(dh / dt, -kMaxTurnRateDegPerSec, kMaxTurnRateDegPerSec);
            }
        }
        if (s.onGround) e.driftDeg = NAN;
        const double t = (s.headingDeg + (std::isfinite(e.driftDeg) ? e.driftDeg : 0.0)) * core::kDegToRad;
        const Track next{now, g, {s.groundSpeed * std::cos(t), s.groundSpeed * std::sin(t), s.onGround ? 0.0 : -s.verticalSpeed},
                         s.headingDeg, turn, s.pitchDeg, s.bankDeg};
        if (!known) {
            e.from = e.to = next;
            e.blendStart = -1e9;
        } else {
            // Continue from what is shown now (position, velocity and heading rate) into the new trajectory.
            const auto cur = shown(e, now);
            e.from = {now, cur.position, cur.velocity, cur.headingDeg, cur.headingRate, cur.pitchDeg, cur.bankDeg};
            e.to = next;
            const bool jump = !cur.position.valid() || local(cur.position, g).length() > kSnapMeters;
            e.blendStart = jump ? -1e9 : now;
        }
        const bool airborneRecently = now - e.lastAirborneAt <= kLandingMemorySeconds;
        if (!s.onGround) e.lastAirborneAt = now;
        const bool near = airport_.valid() && profiles::airport::distanceNm(airport_, g) <= kAirportRadiusNm;
        e.status = classify(s.onGround, s.groundSpeed * kKnotsPerMps, s.verticalSpeed * kFpmPerMps, airborneRecently, near);
        e.last = std::move(s);
        e.history.emplace_back(now, g);
        while (e.history.size() > kHistory) e.history.pop_front();
        e.lastSeen = now;
    }
    ++revision_;
    prune(now);
}

bool Model::prune(double now) {
    bool removed = false;
    for (auto it = entries_.begin(); it != entries_.end();) {
        if (now - it->second.lastSeen > kStaleSeconds) {
            it = entries_.erase(it);
            removed = true;
        } else {
            ++it;
        }
    }
    if (removed) ++revision_;
    return removed;
}

void Model::clear() {
    if (!entries_.empty()) ++revision_;
    entries_.clear();
}

const Model::Entry* Model::find(std::uint32_t id) const {
    const auto it = entries_.find(id);
    return it == entries_.end() ? nullptr : &it->second;
}

bool Model::fresh(std::uint32_t id, double now) const {
    const auto* e = find(id);
    return e && now - e->lastSeen <= kStaleSeconds;
}

Pose Model::predict(std::uint32_t id, double now) const {
    Pose p;
    const auto* e = find(id);
    if (!e || now - e->lastSeen > kStaleSeconds) return p;
    const auto s = shown(*e, now);
    p.position = s.position;
    p.velocity = s.velocity;
    p.headingDeg = s.headingDeg;
    p.pitchDeg = s.pitchDeg;
    p.bankDeg = s.bankDeg;
    p.groundAltitude = e->last.groundAltitude;
    p.onGround = e->last.onGround;
    return p;
}

std::vector<std::uint32_t> Model::byDistance(const Geo& from, double now) const {
    std::vector<std::pair<double, std::uint32_t>> list;
    for (const auto& [id, e] : entries_) {
        if (now - e.lastSeen > kStaleSeconds) continue;
        const auto p = predict(id, now);
        double d = from.valid() && p.valid() ? std::hypot(local(from, p.position).x, local(from, p.position).y) : NAN;
        list.emplace_back(std::isfinite(d) ? d : std::numeric_limits<double>::infinity(), id);
    }
    std::sort(list.begin(), list.end());
    std::vector<std::uint32_t> out;
    for (const auto& [d, id] : list) out.push_back(id);
    return out;
}

double Model::airportDistanceNm(std::uint32_t id, double now) const {
    const auto p = predict(id, now);
    return airport_.valid() && p.valid() ? profiles::airport::distanceNm(airport_, p.position) : NAN;
}

json Model::toJson(const Geo& ownship, double now) const {
    const auto round = [](double v, double step) { return std::isfinite(v) ? std::round(v / step) * step : v; };
    json list = json::array();
    for (const auto id : byDistance(ownship, now)) {
        if (list.size() >= kMaxListed) break;
        const auto& e = *find(id);
        const auto p = predict(id, now);
        json distance = nullptr, bearing = nullptr;
        if (ownship.valid() && p.valid()) {
            const auto d = local(ownship, p.position);
            distance = round(std::hypot(d.x, d.y) / kMetersPerNm, .01);
            double b = std::atan2(d.y, d.x) * core::kRadToDeg;
            if (b < 0) b += 360;
            bearing = std::fmod(std::round(b), 360.0);
        }
        double heading = std::round(p.headingDeg);
        if (heading < 0) heading += 360;
        list.push_back({{"objectId", id}, {"title", e.last.title}, {"atcId", e.last.atcId}, {"airline", e.last.airline},
                        {"flightNumber", e.last.flightNumber}, {"model", e.last.model},
                        {"latitude", round(p.position.lat, 1e-6)}, {"longitude", round(p.position.lon, 1e-6)},
                        {"altitudeFt", std::round(p.position.altitude * kFeetPerMeter)},
                        {"groundSpeedKt", std::round(e.last.groundSpeed * kKnotsPerMps)},
                        {"verticalSpeedFpm", round(e.last.verticalSpeed * kFpmPerMps, 10)},
                        {"headingDeg", std::fmod(heading, 360.0)}, {"onGround", e.last.onGround},
                        {"distanceNm", distance}, {"bearingDeg", bearing}, {"status", toString(e.status)}});
    }
    return {{"aircraft", list}, {"at", std::llround(now * 1000)}};  // session time, milliseconds
}

// ---------------------------------------------------------------------------------------------------------------
// Spotting views

const char* toString(SpotView v) {
    switch (v) {
        case SpotView::Tower: return "tower";
        case SpotView::Runway: return "runway";
        case SpotView::Chase: return "chase";
        case SpotView::Flyby: return "flyby";
        case SpotView::Track: return "track";
    }
    return "chase";
}

std::optional<SpotView> parseSpotView(const std::string& s) {
    for (auto v : {SpotView::Tower, SpotView::Runway, SpotView::Chase, SpotView::Flyby, SpotView::Track})
        if (s == toString(v)) return v;
    return std::nullopt;
}

std::optional<core::Pbh> aim(const Geo& from, const Geo& to) {
    const auto d = local(from, to);
    const double horizontal = std::hypot(d.x, d.y);
    if (!d.finite() || d.length() < .5) return std::nullopt;
    core::Pbh p;
    p.headingDeg = horizontal > 1e-6 ? std::atan2(d.y, d.x) * core::kRadToDeg : 0;
    p.pitchDeg = core::clamp(std::atan2(-d.z, horizontal) * core::kRadToDeg, -89., 89.);
    return p;
}

core::Vec3 chaseOffset(const Pose& target, double behind, double above) {
    const double t = target.trackDeg() * core::kDegToRad;
    return {-std::cos(t) * behind, -std::sin(t) * behind, -above};
}

core::Vec3 follow(const core::Vec3& current, const core::Vec3& desired, double dt, double seconds) {
    if (!current.finite()) return desired;
    if (!(dt > 0) || !std::isfinite(dt)) return current;
    return current + (desired - current) * (-std::expm1(-dt / std::max(seconds, 1e-3)));
}

Geo flybyPosition(const Pose& target, double ahead, double side) {
    const double t = target.trackDeg() * core::kDegToRad;
    const core::Vec3 forward{std::cos(t), std::sin(t), 0}, right{-std::sin(t), std::cos(t), 0};
    const double speed = std::hypot(target.velocity.x, target.velocity.y);
    // Where the target will be: the climb or descent over the time it takes to get there.
    const double lead = speed > 1 ? std::min(ahead / speed, kFlybyMaxLeadSeconds) : 0.0;
    const double down = target.onGround ? 0.0 : target.velocity.z * lead;
    return world(target.position, forward * ahead + right * side + core::Vec3{0, 0, down - kFlybyAboveMeters});
}

bool flybyPassed(const Geo& camera, const Pose& target, double beyond) {
    const auto d = local(camera, target.position);
    const double t = target.trackDeg() * core::kDegToRad;
    const double alongTrack = d.x * std::cos(t) + d.y * std::sin(t);
    return alongTrack > beyond || std::hypot(d.x, d.y) > 3 * kFlybyAheadMeters;
}

double terrainFloor(double terrainA, double terrainB, double clearance) {
    const bool a = std::isfinite(terrainA), b = std::isfinite(terrainB);
    if (!a && !b) return NAN;
    return (a && b ? std::max(terrainA, terrainB) : a ? terrainA : terrainB) + clearance;
}

double autoZoomFov(double distance, double frameMeters, double minFovRad, double maxFovRad) {
    return core::clamp(2 * std::atan2(frameMeters, 2 * distance), minFovRad, maxFovRad);
}

double easeFov(double current, double target, double dt, double responseSeconds) {
    return current + (target - current) * (-std::expm1(-dt / responseSeconds));
}

}  // namespace fscs::host::traffic
