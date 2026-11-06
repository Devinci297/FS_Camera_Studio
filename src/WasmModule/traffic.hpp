#pragma once
// Spotting: the AI and multiplayer traffic picture, and the geometry of the traffic cameras. Pure logic, no SDK.
// SimSession polls SimConnect for aircraft within the spotting radius about every 1.5 s (VATSIM/IVAO traffic arrives
// as AI aircraft). The Model keeps each aircraft with a short history, dead-reckons it between polls and blends every
// new poll in over kBlendSeconds with a C1-continuous smoothstep, so a camera aimed at a target never steps at a poll
// boundary at 60 fps. Aircraft not seen for kStaleSeconds are dropped. The coordinator presents the spotting views
// (coordinator.cpp) from the predicted pose; ModuleApp (spotting.cpp) runs the feed, the commands and auto-spot.

#include "backend.hpp"
#include "fscs/core/math.hpp"
#include "fscs/profiles/airport.hpp"

#include <cstdint>
#include <deque>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace fscs::host::traffic {

using Geo = profiles::airport::Geo;
using Sample = backend_event::TrafficAircraft;
using json = nlohmann::json;

inline constexpr double kKnotsPerMps = 1.0 / 0.514444;
inline constexpr double kFeetPerMeter = 1.0 / 0.3048;
inline constexpr double kFpmPerMps = 60.0 / 0.3048;
inline constexpr double kMetersPerNm = 1852.0;

// What an aircraft is doing (doc/protocol.md, TrafficChanged.status).
enum class Status { Parked, Taxi, Takeoff, Departing, Arriving, Landing, Cruise };
const char* toString(Status s);
inline constexpr double kParkedKt = 1;            // on the ground below this: parked
inline constexpr double kRollKt = 40;             // on the ground at or above this: takeoff or landing roll
inline constexpr double kClimbFpm = 300;          // airborne near the airport: departing above +300, arriving below -300
inline constexpr double kLandingMemorySeconds = 60;  // a roll counts as a landing when airborne this recently
inline constexpr double kAirportRadiusNm = 15;    // "near the airport" for departing / arriving
// on ground & gs < 1 kt: parked; on ground & gs < 40 kt: taxi; on ground & gs >= 40 kt: landing when airborne in the
// last 60 s, else takeoff; airborne within 15 NM of the loaded (or nearest) airport: departing above +300 fpm, arriving
// below -300 fpm; anything else: cruise.
Status classify(bool onGround, double groundSpeedKt, double verticalSpeedFpm, bool airborneRecently, bool nearAirport);

// SimConnect ATC strings can be localisation keys ("TT:ATCCOM.AC_MODEL_A320.0.text"): the readable part, trimmed.
std::string cleanAtcText(const std::string& text);
// Display label, e.g. "DLH4AB · A320": the callsign (airline + flight number, else ATC ID) and the model; the title
// when neither is known.
std::string label(const Sample& s);

// A predicted target: world position, NED velocity (m/s), true heading and FRD attitude in degrees.
struct Pose {
    Geo position{NAN, NAN, NAN};
    core::Vec3 velocity;
    double headingDeg = NAN, pitchDeg = 0, bankDeg = 0;
    double groundAltitude = NAN;  // terrain under the target (meters MSL), NAN when not reported
    bool onGround = false;
    bool valid() const { return position.valid() && std::isfinite(headingDeg); }
    // Horizontal direction of travel (degrees true): the velocity when moving, else the heading.
    double trackDeg() const;
};

class Model {
public:
    static constexpr double kStaleSeconds = 6;
    static constexpr double kBlendSeconds = 1.2;            // a new poll is blended in over this long
    static constexpr double kMaxExtrapolationSeconds = 4;   // dead reckoning stops this long after the last poll
    static constexpr double kSnapMeters = 300;              // a larger correction is a jump (slew, warp): no blend
    static constexpr double kMaxTurnRateDegPerSec = 8;
    static constexpr double kMaxDriftDeg = 30, kDriftSmoothing = .35;  // drift angle: limit and per-poll filter gain
    static constexpr std::size_t kHistory = 6, kMaxAircraft = 256, kMaxListed = 64;

    // One dead-reckoned trajectory from t0: constant speed and climb, turning at headingRate (internal, for tests).
    struct Track {
        double t0 = 0;
        Geo at{NAN, NAN, NAN};
        core::Vec3 velocity;
        double headingDeg = 0, headingRate = 0, pitchDeg = 0, bankDeg = 0;
    };
    struct Entry {
        Sample last;                                  // the latest poll (ATC strings cleaned)
        std::deque<std::pair<double, Geo>> history;   // receive time and position, newest last
        Status status = Status::Parked;
        double lastSeen = -1, lastAirborneAt = -1e9;
        double driftDeg = NAN;                        // smoothed track minus heading while airborne (wind)
        Track from, to;                               // shown = blend(from, to) during kBlendSeconds after a poll
        double blendStart = -1e9;
    };

    // The airport that "near the airport" refers to (the loaded airport set, else the nearest airport).
    void setAirport(const Geo& airport) { airport_ = airport; }
    const Geo& airport() const { return airport_; }
    // A complete poll received at `now`. Excluded: the user aircraft (userObjectId; while that is unknown, an entry at
    // the user's own position `ownship`), object 0 and samples without a usable position or heading.
    void update(const std::vector<Sample>& poll, std::uint32_t userObjectId, const Geo& ownship, double now);
    // Forgets aircraft not seen for kStaleSeconds; true when any was removed.
    bool prune(double now);
    void clear();
    const Entry* find(std::uint32_t id) const;
    bool fresh(std::uint32_t id, double now) const;
    // The smoothed, dead-reckoned pose at `now`; invalid when the aircraft is unknown or stale.
    Pose predict(std::uint32_t id, double now) const;
    // Fresh aircraft, nearest to `from` first (unknown distances last, then by id).
    std::vector<std::uint32_t> byDistance(const Geo& from, double now) const;
    // Horizontal distance from the reference airport in NM; NAN without one.
    double airportDistanceNm(std::uint32_t id, double now) const;
    // TrafficChanged payload: {aircraft: [...] nearest first, at most kMaxListed, at (session time, milliseconds)}.
    json toJson(const Geo& ownship, double now) const;
    std::uint64_t revision() const { return revision_; }
    std::size_t size() const { return entries_.size(); }

private:
    std::map<std::uint32_t, Entry> entries_;
    Geo airport_{NAN, NAN, NAN};
    std::uint64_t revision_ = 0;
};

// ---------------------------------------------------------------------------------------------------------------
// Spotting views

enum class SpotView { Tower, Runway, Chase, Flyby, Track };
const char* toString(SpotView v);
std::optional<SpotView> parseSpotView(const std::string& s);

inline constexpr double kChaseBehindMeters = 60, kChaseAboveMeters = 15;
inline constexpr double kChaseFollowSeconds = .8;   // the chase offset eases round turns with this time constant
inline constexpr double kChaseFovDeg = 60;
inline constexpr double kFlybyAheadMeters = 400, kFlybySideMeters = 30, kFlybyBeyondMeters = 300;
inline constexpr double kFlybyAboveMeters = 3;      // above where the target will be as it passes
inline constexpr double kFlybyMaxLeadSeconds = 30;  // altitude prediction horizon for slow targets
inline constexpr double kTerrainClearanceMeters = 2;

// World look direction (heading true, pitch up > 0, degrees) from `from` at `to`; nullopt when closer than 0.5 m.
std::optional<core::Pbh> aim(const Geo& from, const Geo& to);
// Chase: behind the target along its track and above it, as a north-east-down offset from the target.
core::Vec3 chaseOffset(const Pose& target, double behind = kChaseBehindMeters, double above = kChaseAboveMeters);
// Eases `current` towards `desired` with time constant `seconds` (frame-rate independent).
core::Vec3 follow(const core::Vec3& current, const core::Vec3& desired, double dt, double seconds = kChaseFollowSeconds);
// Fly-by: a stationary point `ahead` meters along the track and `side` meters to its right, at the altitude the
// target will have when it gets there.
Geo flybyPosition(const Pose& target, double ahead = kFlybyAheadMeters, double side = kFlybySideMeters);
// True once the target is `beyond` meters past the camera along its track, or has gone far away another way.
bool flybyPassed(const Geo& camera, const Pose& target, double beyond = kFlybyBeyondMeters);
// The lowest allowed camera altitude: the higher known terrain plus clearance; NAN when no terrain is known.
double terrainFloor(double terrainA, double terrainB, double clearance = kTerrainClearanceMeters);
// Auto zoom: the FOV that frames `frameMeters` at `distance` (clamped), and frame-rate independent easing towards it.
double autoZoomFov(double distance, double frameMeters, double minFovRad, double maxFovRad);
double easeFov(double current, double target, double dt, double responseSeconds);

}  // namespace fscs::host::traffic
