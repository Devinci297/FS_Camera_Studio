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
    // Localisation prefixes, in either order: "TT:" (a translation key) and "$$:" (a literal, e.g. "$$:E190").
    for (bool again = true; again;) {
        const auto before = s.size();
        for (const char* prefix : {"TT:", "$$:", "$$"}) strip(prefix);
        again = s.size() != before;
    }
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
    return s.model.empty() ? callsign : callsign + " - " + s.model;
}

double Pose::trackDeg() const {
    return std::hypot(velocity.x, velocity.y) > 2 ? std::atan2(velocity.y, velocity.x) * core::kRadToDeg : headingDeg;
}

double chaseDirectionDeg(const Pose& target) {
    const double speed = std::hypot(target.velocity.x, target.velocity.y);
    const double u = core::clamp((speed - kChaseHeadingBelowMps) / (kChaseTrackAboveMps - kChaseHeadingBelowMps), 0.0, 1.0);
    if (u <= 0 || !std::isfinite(speed)) return target.headingDeg;
    const double track = std::atan2(target.velocity.y, target.velocity.x) * core::kRadToDeg;
    return core::wrapDeg(target.headingDeg + core::wrapDeg(track - target.headingDeg) * u * u * (3 - 2 * u));
}

std::optional<core::Vec3> fitVelocity(const std::deque<std::pair<double, Geo>>& samples, double minSpan, double* mean,
                                      std::size_t minSamples) {
    if (samples.size() < std::max<std::size_t>(minSamples, 2) || samples.back().first - samples.front().first < minSpan) return std::nullopt;
    const Geo& ref = samples.back().second;
    double tm = 0;
    core::Vec3 xm;
    for (const auto& [t, g] : samples) { tm += t; xm += local(ref, g); }
    const double n = static_cast<double>(samples.size());
    tm /= n;
    xm = xm * (1 / n);
    double stt = 0;
    core::Vec3 stx;
    for (const auto& [t, g] : samples) {
        const double dt = t - tm;
        stt += dt * dt;
        stx += (local(ref, g) - xm) * dt;
    }
    if (!(stt > 1e-9)) return std::nullopt;
    const auto v = stx * (1 / stt);
    if (!v.finite()) return std::nullopt;
    if (mean) *mean = tm;
    return v;
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

// The per-frame sample at `t`: exact at its receive time, extrapolated at most kFrameLeadSeconds.
Shown frameShown(const Model::Entry::Frame& f, double t) {
    const double lead = core::clamp(t - f.at, 0.0, Model::kFrameLeadSeconds);
    return {world(f.position, f.velocity * lead), f.velocity, f.headingDeg, 0.0, f.pitchDeg, f.bankDeg};
}

bool hasFrame(const Model::Entry& e) { return e.frame.position.valid() && std::isfinite(e.frame.headingDeg); }

// What is shown at `t`: per-frame data as it is (extrapolated at most kFrameLeadSeconds); once it stops, a smoothstep
// blend from the last sample dead-reckoned on into the poll prediction (no step in position or velocity); otherwise
// the poll prediction.
Shown display(const Model::Entry& e, double t) {
    if (!hasFrame(e)) return shown(e, t);
    const double age = t - e.frame.at;
    if (age <= Model::kFrameLeadSeconds) return frameShown(e.frame, t);
    const double u = (age - Model::kFrameLeadSeconds) / Model::kBlendSeconds;
    if (!(u < 1)) return shown(e, t);
    const double s = u * u * (3 - 2 * u), ds = 6 * u * (1 - u) / Model::kBlendSeconds;
    Shown a = frameShown(e.frame, t);
    a.position = world(e.frame.position, e.frame.velocity * std::min(age, Model::kMaxExtrapolationSeconds));
    const auto b = shown(e, t);
    const auto d = local(a.position, b.position);
    const double dh = core::wrapDeg(b.headingDeg - a.headingDeg);
    return {world(a.position, d * s), a.velocity * (1 - s) + b.velocity * s + d * ds, core::wrapDeg(a.headingDeg + dh * s),
            b.headingRate * s + dh * ds, a.pitchDeg + (b.pitchDeg - a.pitchDeg) * s,
            a.bankDeg + core::wrapDeg(b.bankDeg - a.bankDeg) * s};
}

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
        // Per-frame data arriving: the poll must not disturb the pose (it arrives every frame, so within the lead time).
        const bool framed = hasFrame(e) && now - e.frame.at <= kFrameLeadSeconds;
        Sample s = raw;
        s.headingDeg = core::wrapDeg(s.headingDeg);
        s.groundSpeed = std::max(0.0, finiteOr(s.groundSpeed, 0));
        s.verticalSpeed = finiteOr(s.verticalSpeed, 0);
        s.pitchDeg = core::clamp(finiteOr(s.pitchDeg, 0), -90., 90.);
        s.bankDeg = core::wrapDeg(finiteOr(s.bankDeg, 0));
        if (!(std::isfinite(s.groundAltitude) && s.groundAltitude > -1500 && s.groundAltitude < 10000)) s.groundAltitude = NAN;
        for (auto* text : {&s.title, &s.atcId, &s.airline, &s.flightNumber, &s.model}) *text = cleanAtcText(*text);
        const bool known = e.lastSeen >= 0;
        if (known && now - e.lastSeen > kStaleSeconds) e.history.clear();
        // A jump (slew, warp, a new flight) starts the position history, drift and turn rate afresh.
        bool jumped = false;
        if (known) {
            const auto shownNow = display(e, now);
            jumped = !shownNow.position.valid() || local(shownNow.position, g).length() > kSnapMeters + 300 * (now - e.lastSeen);
            if (jumped) { e.history.clear(); e.driftDeg = NAN; }
        }

        // Injected traffic can report zero (or wrong) speeds while it moves: fit the speed to the recent positions.
        std::deque<std::pair<double, Geo>> recent;
        for (const auto& h : e.history) if (now - h.first <= kPollFitSeconds && now - h.first > 1e-6) recent.push_back(h);
        recent.emplace_back(now, g);
        double fittedAt = now;
        const auto fitted = fitVelocity(recent, 1.0, &fittedAt, 2);
        std::optional<double> fittedTrackDeg;
        // Plausible speeds only: a fit across a jump is not a velocity.
        if (fitted && std::hypot(fitted->x, fitted->y) <= (s.onGround ? kMaxGroundSpeedMps : kMaxAirSpeedMps)) {
            const double fs = std::hypot(fitted->x, fitted->y);
            if (fs > 1 && (s.groundSpeed < .5 || s.groundSpeed < .25 * fs || s.groundSpeed > 4 * fs + 2)) {
                s.groundSpeed = fs;
                fittedTrackDeg = std::atan2(fitted->y, fitted->x) * core::kRadToDeg;
            }
            if (std::abs(s.verticalSpeed) < .05 && std::abs(fitted->z) > .3 && !s.onGround) s.verticalSpeed = -fitted->z;
        }

        // Direction of travel: the heading plus the drift (wind) angle when airborne and moving, else the heading. The
        // drift is the measured track between the last two polls against the heading half-way between them, smoothed
        // over polls since it changes slowly and a single chord is noisy. Turn rate from the last two polls.
        double turn = 0;
        if (known && !jumped) {
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
        // Direction of travel: a fitted track (turned on to now by the turn rate), else heading plus drift.
        const double t = (fittedTrackDeg ? *fittedTrackDeg + turn * (now - fittedAt)
                                         : s.headingDeg + (std::isfinite(e.driftDeg) ? e.driftDeg : 0.0)) * core::kDegToRad;
        const bool parked = s.onGround && s.groundSpeed < kParkedSpeedMps;
        Track next{now, g, {s.groundSpeed * std::cos(t), s.groundSpeed * std::sin(t), s.onGround ? 0.0 : -s.verticalSpeed},
                   s.headingDeg, turn, s.pitchDeg, s.bankDeg};
        if (parked) { next.velocity = {}; next.headingRate = 0; }
        if (!known) {
            e.from = e.to = next;
            e.blendStart = -1e9;
        } else if (framed) {
            // Fresh per-frame data shows the aircraft: the poll only updates the fallback trajectory, never the pose.
            e.from = e.to = next;
            e.blendStart = -1e9;
        } else {
            // Continue from what is shown now (position, velocity and heading rate) into the new trajectory.
            const auto cur = display(e, now);
            e.frame.at = -1e9;  // a blend back from per-frame data is folded into this one
            const auto moved = cur.position.valid() ? local(cur.position, g) : core::Vec3{NAN, NAN, NAN};
            if (parked && e.last.onGround && moved.finite() && moved.length() < kParkedDeadbandMeters &&
                std::abs(core::wrapDeg(s.headingDeg - cur.headingDeg)) < kParkedDeadbandDeg) {
                // Parked: report noise never moves the pose; it stays exactly where it is shown.
                e.from = e.to = {now, cur.position, {}, cur.headingDeg, 0, cur.pitchDeg, cur.bankDeg};
                e.blendStart = -1e9;
            } else {
                e.from = {now, cur.position, cur.velocity, cur.headingDeg, cur.headingRate, cur.pitchDeg, cur.bankDeg};
                e.to = next;
                const bool jump = !moved.finite() || moved.length() > kSnapMeters;
                e.blendStart = jump ? -1e9 : now;
            }
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

void Model::updateTarget(const FrameSample& raw, double now) {
    const auto it = entries_.find(raw.objectId);
    if (it == entries_.end()) return;
    Entry& e = it->second;
    const Geo g{raw.lat, raw.lon, raw.altitude};
    if (!g.valid() || !std::isfinite(raw.headingDeg)) return;
    e.frames.emplace_back(now, g);
    while (!e.frames.empty() && (now - e.frames.front().first > kFrameFitSeconds || e.frames.size() > 64)) e.frames.pop_front();
    Entry::Frame f;
    f.at = now;
    f.position = g;
    f.headingDeg = core::wrapDeg(raw.headingDeg);
    f.pitchDeg = core::clamp(finiteOr(raw.pitchDeg, 0), -90., 90.);
    f.bankDeg = core::wrapDeg(finiteOr(raw.bankDeg, 0));
    f.onGround = raw.onGround;
    f.groundAltitude = std::isfinite(raw.groundAltitude) && raw.groundAltitude > -1500 && raw.groundAltitude < 10000 ? raw.groundAltitude : NAN;
    // VELOCITY WORLD, checked against the positions: injected traffic may report zero, and a wrong axis would point
    // the chase the wrong way. A least-squares fit over the last 0.6 s replaces a reported velocity that disagrees.
    core::Vec3 v{finiteOr(raw.velocityNorth, 0), finiteOr(raw.velocityEast, 0), finiteOr(raw.velocityDown, 0)};
    if (const auto fit = fitVelocity(e.frames, .15)) {
        const double rs = std::hypot(v.x, v.y), fs = std::hypot(fit->x, fit->y);
        if (fs > 1.5 && (rs < .5 || rs < .5 * fs || rs > 2 * fs + 2 || v.x * fit->x + v.y * fit->y < .5 * rs * fs)) {
            v.x = fit->x;
            v.y = fit->y;
        }
        if (std::abs(v.z) < .05 && std::abs(fit->z) > .5) v.z = fit->z;
    }
    if (f.onGround) v.z = 0;
    const bool parked = f.onGround && std::hypot(v.x, v.y) < kFrameParkedSpeedMps;
    if (parked) v = {};
    f.velocity = v;
    if (parked && hasFrame(e) && e.frame.onGround && now - e.frame.at <= kFrameFreshSeconds &&
        local(e.frame.position, g).length() < kFrameParkedDeadbandMeters &&
        std::abs(core::wrapDeg(f.headingDeg - e.frame.headingDeg)) < kFrameParkedDeadbandDeg) {
        // Parked: the pose stays exactly where it is (a perfectly still camera); only the time moves on.
        f.position = e.frame.position;
        f.headingDeg = e.frame.headingDeg;
        f.pitchDeg = e.frame.pitchDeg;
        f.bankDeg = e.frame.bankDeg;
    }
    e.frame = f;
}

bool Model::frameFresh(std::uint32_t id, double now) const {
    const auto* e = find(id);
    return e && hasFrame(*e) && now - e->frame.at <= kFrameFreshSeconds && now - e->frame.at >= -1e-9;
}

bool Model::prune(double now) {
    bool removed = false;
    for (auto it = entries_.begin(); it != entries_.end();) {
        if (now - it->second.seen() > kStaleSeconds) {
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
    return e && now - e->seen() <= kStaleSeconds;
}

Pose Model::predict(std::uint32_t id, double now) const {
    Pose p;
    const auto* e = find(id);
    if (!e || now - e->seen() > kStaleSeconds) return p;
    const auto s = display(*e, now);
    p.position = s.position;
    p.velocity = s.velocity;
    p.headingDeg = s.headingDeg;
    p.pitchDeg = s.pitchDeg;
    p.bankDeg = s.bankDeg;
    const bool framed = hasFrame(*e) && now - e->frame.at <= kFrameLeadSeconds + kBlendSeconds;
    p.groundAltitude = framed && std::isfinite(e->frame.groundAltitude) ? e->frame.groundAltitude : e->last.groundAltitude;
    p.onGround = framed ? e->frame.onGround : e->last.onGround;
    return p;
}

std::vector<std::uint32_t> Model::byDistance(const Geo& from, double now) const {
    std::vector<std::pair<double, std::uint32_t>> list;
    for (const auto& [id, e] : entries_) {
        if (now - e.seen() > kStaleSeconds) continue;
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
        case SpotView::Spot: return "spot";
        case SpotView::Nearest: return "nearest";
        case SpotView::Front: return "front";
        case SpotView::Rear: return "rear";
        case SpotView::Left: return "left";
        case SpotView::Right: return "right";
        case SpotView::Overhead: return "overhead";
        case SpotView::Orbit: return "orbit";
    }
    return "chase";
}

std::optional<SpotView> parseSpotView(const std::string& s) {
    for (auto v : {SpotView::Tower, SpotView::Runway, SpotView::Chase, SpotView::Flyby, SpotView::Track, SpotView::Spot, SpotView::Nearest, SpotView::Front, SpotView::Rear, SpotView::Left, SpotView::Right, SpotView::Overhead, SpotView::Orbit})
        if (s == toString(v)) return v;
    return std::nullopt;
}

bool externalSpotView(SpotView view) {
    return view==SpotView::Chase || view==SpotView::Front || view==SpotView::Rear || view==SpotView::Left ||
        view==SpotView::Right || view==SpotView::Overhead || view==SpotView::Orbit;
}
core::Vec3 externalOffset(const Pose& target, SpotView view, double orbitDegrees) {
    if(view==SpotView::Chase)return chaseOffset(target);
    const double heading=target.headingDeg*core::kDegToRad;
    const core::Vec3 forward{std::cos(heading),std::sin(heading),0},right{-std::sin(heading),std::cos(heading),0};
    switch(view) {
        case SpotView::Front:return forward*80+core::Vec3{0,0,-8};
        case SpotView::Rear:return forward*(-80)+core::Vec3{0,0,-8};
        case SpotView::Left:return right*(-65)+core::Vec3{0,0,-12};
        case SpotView::Right:return right*65+core::Vec3{0,0,-12};
        case SpotView::Overhead:return forward*(-5)+core::Vec3{0,0,-90};
        case SpotView::Orbit:{const double angle=orbitDegrees*core::kDegToRad;return forward*(-90*std::cos(angle))+right*(90*std::sin(angle))+core::Vec3{0,0,-25};}
        default:return chaseOffset(target);
    }
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
    const double t = chaseDirectionDeg(target) * core::kDegToRad;
    return {-std::cos(t) * behind, -std::sin(t) * behind, -above};
}

core::Vec3 follow(const core::Vec3& current, const core::Vec3& desired, double dt, double seconds) {
    if (!current.finite()) return desired;
    if (!(dt > 0) || !std::isfinite(dt)) return current;
    if ((desired - current).length() < kSettledMeters) return desired;  // settled: exactly still, not creeping
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

core::Vec3 smoothDamp(const core::Vec3& current, core::Vec3& velocity, const core::Vec3& target, double dt, double seconds) {
    if (!current.finite() || !velocity.finite()) { velocity = {}; return target; }
    if (!(dt > 0) || !std::isfinite(dt)) return current;
    // Critically damped spring (Unity's SmoothDamp approximation of exp(-x) is stable at any frame time).
    const double omega = 2 / std::max(seconds, 1e-3), x = omega * dt;
    const double decay = 1 / (1 + x + .48 * x * x + .235 * x * x * x);
    const auto change = current - target;
    if (change.length() < kSettledMeters && velocity.length() < kSettledMeters) { velocity = {}; return target; }
    const auto temp = (velocity + change * omega) * dt;
    velocity = (velocity - temp * omega) * decay;
    return target + (change + temp) * decay;
}

double autoZoomFov(double distance, double frameMeters, double minFovRad, double maxFovRad) {
    return core::clamp(2 * std::atan2(frameMeters, 2 * distance), minFovRad, maxFovRad);
}

double easeFov(double current, double target, double dt, double responseSeconds) {
    if (std::abs(target - current) < 1e-7) return target;
    return current + (target - current) * (-std::expm1(-dt / responseSeconds));
}

// ---------------------------------------------------------------------------------------------------------------
// Computed airport views

namespace {
double horizontalDistance(const Geo& a, const Geo& b) {
    const auto d = local(a, b);
    return std::hypot(d.x, d.y);
}
}  // namespace

SpotAirport spotAirport(const json& f) {
    SpotAirport a;
    a.icao = js::str(f, "icao");
    a.origin = profiles::airport::geo(f);
    if (!a.origin.valid()) return a;
    // The generator's runway geometry: thresholds at the runway ends, primary landing along the heading.
    const json* longest = nullptr;
    double longestLength = 0;
    for (const auto& r : js::array(f, "runways")) {
        const auto center = profiles::airport::geo(r);
        const double length = js::num(r, "length", 0), heading = js::num(r, "heading", NAN), width = js::num(r, "width", 30);
        if (!center.valid() || length < 50 || length > 10000 || !std::isfinite(heading)) continue;
        if (length > longestLength) { longestLength = length; longest = &r; }
        for (int side = 0; side < 2; ++side) {
            RunwayEnd end;
            end.icao = a.icao;
            end.name = js::str(r, side ? "secondary" : "primary", side ? "Reciprocal" : "Runway");
            end.headingDeg = core::wrapDeg(heading + (side ? 180 : 0));
            const double t = end.headingDeg * core::kDegToRad;
            end.threshold = world(center, core::Vec3{std::cos(t), std::sin(t), 0} * (-length / 2));
            end.threshold.altitude = center.altitude;
            end.lengthMeters = length;
            end.widthMeters = std::isfinite(width) ? width : 30;
            if (end.threshold.valid()) a.ends.push_back(std::move(end));
        }
    }
    const auto tower = profiles::airport::geo(js::get(f, "tower"));
    if (tower.valid() && (tower.lat != 0 || tower.lon != 0) && horizontalDistance(a.origin, tower) < 10000) {
        a.tower = {tower.lat, tower.lon, a.origin.altitude};
    } else if (longest) {
        // As the generator: a virtual tower abeam the longest runway's midpoint, on the side with more parking.
        const auto center = profiles::airport::geo(*longest);
        const double h = js::num(*longest, "heading", 0) * core::kDegToRad;
        const core::Vec3 right{-std::sin(h), std::cos(h), 0};
        const auto c = local(a.origin, center);
        int rightStands = 0, leftStands = 0;
        for (const auto& p : js::array(f, "parking")) {
            const double n = js::num(p, "north", NAN), e = js::num(p, "east", NAN);
            if (!std::isfinite(n) || !std::isfinite(e)) continue;
            const double across = (n - c.x) * right.x + (e - c.y) * right.y;
            if (across > 0) ++rightStands; else if (across < 0) ++leftStands;
        }
        a.tower = world(a.origin, c + right * (rightStands > leftStands ? profiles::airport::kVirtualTowerAbeamMeters
                                                                        : -profiles::airport::kVirtualTowerAbeamMeters));
        a.tower.altitude = a.origin.altitude;
        a.virtualTower = true;
    }
    return a;
}

const std::vector<std::string>& runwaySpotKinds() {
    static const std::vector<std::string> kinds{"fence", "approach", "threshold", "custom"};
    return kinds;
}

RunwaySpot runwaySpot(const RunwayEnd& end, const std::string& kind, double runwayHeight) {
    RunwaySpot s;
    s.icao = end.icao;
    s.runwayEnd = end.name;
    s.kind = kind;
    const double height = std::isfinite(runwayHeight) ? core::clamp(runwayHeight, 0.0, 1000.0) : 3.0;
    if (kind == "approach") {
        // The generator's 1 NM spot: about 90 m below a 3-degree glidepath.
        s.behindMeters = profiles::airport::kApproachDistanceMeters;
        s.heightMeters = std::max(0.0, profiles::airport::kApproachDistanceMeters * std::tan(profiles::airport::kGlidepathDeg * core::kDegToRad) -
                                           profiles::airport::kApproachBelowGlidepathMeters);
    } else if (kind == "threshold") {
        s.sideMeters = end.widthMeters / 2 + 12;  // the generator's threshold view offset
        s.heightMeters = height;
    } else {
        s.behindMeters = profiles::airport::kFenceBehindMeters;
        s.sideMeters = profiles::airport::kFenceSideMeters;
        s.heightMeters = height;
    }
    return s;
}

Geo spotPosition(const RunwayEnd& end, const RunwaySpot& spot) {
    const double t = end.headingDeg * core::kDegToRad;
    const core::Vec3 landing{std::cos(t), std::sin(t), 0}, right{-std::sin(t), std::cos(t), 0};
    return world(end.threshold, landing * -spot.behindMeters + right * spot.sideMeters + core::Vec3{0, 0, -spot.heightMeters});
}

json toJson(const RunwaySpot& s) {
    json j{{"icao", s.icao}, {"runwayEnd", s.runwayEnd}, {"kind", s.kind},
           {"behindMeters", s.behindMeters}, {"sideMeters", s.sideMeters}, {"heightMeters", s.heightMeters}};
    if (!s.name.empty()) j["name"] = s.name;
    return j;
}

std::string spotName(const json& v) {
    std::string name;
    for (const char c : js::str(v, "name"))
        if (static_cast<unsigned char>(c) >= 32 && c != 127) name.push_back(c);
    const auto first = name.find_first_not_of(' ');
    if (first == std::string::npos) return {};
    name = name.substr(first, name.find_last_not_of(' ') - first + 1);
    if (name.size() > kSpotNameMax) {
        // Never split a UTF-8 sequence: cut before the last character when it does not fit whole.
        std::size_t lead = kSpotNameMax;
        while (lead > 0 && (static_cast<unsigned char>(name[lead]) & 0xC0) == 0x80) --lead;
        name.resize(lead);
    }
    return name;
}

std::optional<PositionSpot> parsePositionSpot(const json& v, std::string& error) {
    if (!v.is_object()) { error = "A position spot needs a latitude, a longitude and an altitude."; return std::nullopt; }
    const auto number = [&](const char* key, double low, double high, double& out) {
        const auto& x = js::get(v, key);
        if (!x.is_number() || !std::isfinite(x.get<double>()) || x.get<double>() < low || x.get<double>() > high) return false;
        out = x.get<double>();
        return true;
    };
    PositionSpot spot;
    if (!number("latitude", -90, 90, spot.position.lat) || !number("longitude", -180, 180, spot.position.lon)) {
        error = "The spot's position is not valid.";
        return std::nullopt;
    }
    if (!number("altitudeMeters", kPositionSpotMinAltitude, kPositionSpotMaxAltitude, spot.position.altitude)) {
        error = "The spot's altitudeMeters is out of range.";
        return std::nullopt;
    }
    spot.name = spotName(v);
    return spot;
}

json toJson(const PositionSpot& s) {
    json j{{"kind", "position"}, {"latitude", s.position.lat}, {"longitude", s.position.lon}, {"altitudeMeters", s.position.altitude}};
    if (!s.name.empty()) j["name"] = s.name;
    return j;
}

const RunwayEnd* findRunwayEnd(const std::vector<SpotAirport>& airports, const std::string& icao, const std::string& end) {
    for (const auto& a : airports)
        if (a.icao == icao)
            for (const auto& e : a.ends) if (e.name == end) return &e;
    return nullptr;
}

std::optional<RunwaySpot> parseRunwaySpot(const json& v, const std::vector<SpotAirport>& airports, double runwayHeight, std::string& error) {
    if (!v.is_object()) { error = "A runway spot needs an airport and a runway end."; return std::nullopt; }
    const auto* end = findRunwayEnd(airports, js::str(v, "icao"), js::str(v, "runwayEnd"));
    if (!end) { error = "That runway end is not known. Wait for the airport's scenery data, or choose another end."; return std::nullopt; }
    const auto kind = js::str(v, "kind", "fence");
    const auto& kinds = runwaySpotKinds();
    if (std::find(kinds.begin(), kinds.end(), kind) == kinds.end()) { error = "Choose a spot type: fence, approach, threshold or custom."; return std::nullopt; }
    auto spot = runwaySpot(*end, kind == "custom" ? "fence" : kind, runwayHeight);
    spot.kind = kind;
    const auto offset = [&](const char* key, double& out, double low, double high) {
        const auto& x = js::get(v, key);
        if (x.is_null()) return true;
        if (!x.is_number() || !std::isfinite(x.get<double>()) || x.get<double>() < low || x.get<double>() > high) {
            error = std::string("The spot's ") + key + " is out of range.";
            return false;
        }
        out = x.get<double>();
        return true;
    };
    if (!offset("behindMeters", spot.behindMeters, -5000, 5000) || !offset("sideMeters", spot.sideMeters, -5000, 5000) ||
        !offset("heightMeters", spot.heightMeters, 0, 1000))
        return std::nullopt;
    spot.name = spotName(v);
    return spot;
}

const RunwayEnd* pickRunwayEnd(const std::vector<SpotAirport>& airports, const Pose& target, Status status, bool* aligned) {
    if (aligned) *aligned = false;
    if (!target.valid()) return nullptr;
    const bool arriving = status == Status::Arriving || status == Status::Landing;
    const bool departing = status == Status::Takeoff || status == Status::Departing;
    const double track = target.trackDeg();
    const RunwayEnd *best = nullptr, *nearest = nullptr;
    double bestScore = std::numeric_limits<double>::infinity(), nearestDistance = bestScore;
    for (const auto& a : airports)
        for (const auto& e : a.ends) {
            const auto rel = local(e.threshold, target.position);
            const double distance = std::hypot(rel.x, rel.y);
            if (!(distance <= kSpotAirportRangeMeters)) continue;
            if (distance < nearestDistance) { nearestDistance = distance; nearest = &e; }
            if (!arriving && !departing) continue;
            // Arrivals land over this end in its landing direction; departures lift off over the reciprocal end's
            // threshold, flying against that end's landing direction.
            const double wanted = arriving ? e.headingDeg : core::wrapDeg(e.headingDeg + 180);
            if (std::abs(core::wrapDeg(track - wanted)) > kRunwayAlignDeg) continue;
            const double t = e.headingDeg * core::kDegToRad;
            const double along = rel.x * std::cos(t) + rel.y * std::sin(t);  // < 0: on this end's approach side
            const double lateral = std::abs(-rel.x * std::sin(t) + rel.y * std::cos(t));
            // Up to the far end of the runway, and inside a cone round the extended centerline.
            if (along > e.lengthMeters + 200 || lateral > 150 + .15 * std::max(0.0, -along)) continue;
            const double score = lateral + .02 * std::abs(along);
            if (score < bestScore) { bestScore = score; best = &e; }
        }
    if (best) {
        if (aligned) *aligned = true;
        return best;
    }
    return nearest;
}

const SpotAirport* pickTower(const std::vector<SpotAirport>& airports, const Pose& target) {
    const SpotAirport* best = nullptr;
    double bestDistance = kSpotAirportRangeMeters;
    if (!target.valid()) return nullptr;
    for (const auto& a : airports) {
        if (!a.tower.valid()) continue;
        const double d = horizontalDistance(a.tower, target.position);
        if (d <= bestDistance) { bestDistance = d; best = &a; }
    }
    return best;
}

Geo towerPosition(const SpotAirport& a, double towerHeight) {
    auto g = a.tower;
    g.altitude += std::isfinite(towerHeight) ? core::clamp(towerHeight, 0.0, 1000.0) : 35.0;
    return g;
}

}  // namespace fscs::host::traffic
