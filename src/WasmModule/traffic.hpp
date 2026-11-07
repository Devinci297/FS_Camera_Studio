#pragma once
// Spotting: the AI and multiplayer traffic picture, and the geometry of the traffic cameras. Pure logic, no SDK.
// SimSession polls SimConnect for aircraft within the spotting radius about every 1.5 s (VATSIM/IVAO traffic arrives
// as AI aircraft). The Model keeps each aircraft with a short history, dead-reckons it between polls and blends every
// new poll in over kBlendSeconds with a C1-continuous smoothstep, so a camera aimed at a target never steps at a poll
// boundary at 60 fps. Aircraft not seen for kStaleSeconds are dropped. The spotting target also gets SimConnect data
// every simulator frame (updateTarget): while that is fresh the pose is the simulator's own, so a camera fixed to it
// moves exactly with the aircraft drawn; when it stops the pose blends back into the poll prediction. Injected traffic
// (FSLTL and similar) can report zero speeds while it moves, so velocities are also fitted to the positions. The
// coordinator presents the spotting views (coordinator.cpp) from the pose; ModuleApp (spotting.cpp) runs the feed, the
// commands and auto-spot.

#include "backend.hpp"
#include "fscs/core/math.hpp"
#include "fscs/profiles/airport.hpp"

#include <algorithm>
#include <cstdint>
#include <deque>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace fscs::host::traffic {

using Geo = profiles::airport::Geo;
using Sample = backend_event::TrafficAircraft;
using FrameSample = backend_event::TrafficTarget;
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

// SimConnect ATC strings can be localisation keys ("TT:ATCCOM.AC_MODEL_A320.0.text", "$$:E190"): the readable part,
// trimmed.
std::string cleanAtcText(const std::string& text);
// Display label, e.g. "DLH4AB - A320" (ASCII: the simulator's panel font has no middle dot): the callsign (airline + flight number, else ATC ID) and the model; the title
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
// The chase direction (degrees true): the heading below kChaseHeadingBelowMps, the track above kChaseTrackAboveMps
// and a smooth blend between, so a taxiing or parked aircraft's small velocity noise never swings the chase.
inline constexpr double kChaseHeadingBelowMps = 6, kChaseTrackAboveMps = 10;
double chaseDirectionDeg(const Pose& target);
// Least-squares velocity (north, east, down m/s) through timed positions, at their mean time; nullopt with fewer
// than `minSamples` samples or a span under `minSpan` seconds. `mean` receives the mean time.
std::optional<core::Vec3> fitVelocity(const std::deque<std::pair<double, Geo>>& samples, double minSpan, double* mean = nullptr,
                                      std::size_t minSamples = 3);

class Model {
public:
    static constexpr double kStaleSeconds = 6;
    static constexpr double kBlendSeconds = 1.2;            // a new poll is blended in over this long
    static constexpr double kMaxExtrapolationSeconds = 4;   // dead reckoning stops this long after the last poll
    static constexpr double kSnapMeters = 300;              // a larger correction is a jump (slew, warp): no blend
    static constexpr double kMaxTurnRateDegPerSec = 8;
    static constexpr double kMaxDriftDeg = 30, kDriftSmoothing = .35;  // drift angle: limit and per-poll filter gain
    static constexpr std::size_t kHistory = 6, kMaxAircraft = 256, kMaxListed = 64;
    // Speeds fitted to the positions replace reported ones that are near zero or far off (injected traffic).
    static constexpr double kPollFitSeconds = 3.1, kFrameFitSeconds = .6;
    static constexpr double kMaxGroundSpeedMps = 100, kMaxAirSpeedMps = 400;  // a faster fit is a jump, not a speed
    // Parked (on the ground, below this speed): reports that move less than the deadband keep the pose still.
    static constexpr double kParkedSpeedMps = .5, kParkedDeadbandMeters = .5, kParkedDeadbandDeg = 1;
    static constexpr double kFrameParkedSpeedMps = .3, kFrameParkedDeadbandMeters = .15, kFrameParkedDeadbandDeg = .3;
    // Per-frame data: extrapolated at most kFrameLeadSeconds past its receive time; after that (samples stopped) the pose
    // blends from it, dead-reckoned on, back into the poll prediction over kBlendSeconds. Fresh: within kFrameFreshSeconds.
    static constexpr double kFrameLeadSeconds = .1, kFrameFreshSeconds = .5;

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
        // Per-frame data (the watched spotting target): the latest sample, velocity checked against `frames`.
        struct Frame {
            double at = -1e9;
            Geo position{NAN, NAN, NAN};
            core::Vec3 velocity;
            double headingDeg = NAN, pitchDeg = 0, bankDeg = 0, groundAltitude = NAN;
            bool onGround = false;
        } frame;
        std::deque<std::pair<double, Geo>> frames;   // receive time and position of recent per-frame samples
        double seen() const { return std::max(lastSeen, frame.at); }
    };

    // The airport that "near the airport" refers to (the loaded airport set, else the nearest airport).
    void setAirport(const Geo& airport) { airport_ = airport; }
    const Geo& airport() const { return airport_; }
    // A complete poll received at `now`. Excluded: the user aircraft (userObjectId; while that is unknown, an entry at
    // the user's own position `ownship`), object 0 and samples without a usable position or heading.
    void update(const std::vector<Sample>& poll, std::uint32_t userObjectId, const Geo& ownship, double now);
    // One per-frame sample of a listed aircraft received at `now` (unknown ids and unusable samples are ignored).
    void updateTarget(const FrameSample& sample, double now);
    // Whether per-frame data for `id` is fresh at `now` (the pose is the simulator's own).
    bool frameFresh(std::uint32_t id, double now) const;
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

// Spot: a fixed position spot (SpotTarget.spot kind "position"), aimed at the target with auto zoom.
enum class SpotView { Tower, Runway, Chase, Flyby, Track, Spot };
const char* toString(SpotView v);
std::optional<SpotView> parseSpotView(const std::string& s);

inline constexpr double kChaseBehindMeters = 60, kChaseAboveMeters = 15;
inline constexpr double kChaseFollowSeconds = .8;   // the chase offset eases round turns with this time constant
inline constexpr double kChaseFovDeg = 60;
inline constexpr double kFlybyAheadMeters = 400, kFlybySideMeters = 30, kFlybyBeyondMeters = 300;
inline constexpr double kFlybyAboveMeters = 3;      // above where the target will be as it passes
inline constexpr double kFlybyMaxLeadSeconds = 30;  // altitude prediction horizon for slow targets
inline constexpr double kTerrainClearanceMeters = 2;
inline constexpr double kSettledMeters = 1e-4;     // follow / smoothDamp stop exactly once this close

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
// Frame-rate independent critically damped smoothing of `current` towards `target` (time constant `seconds`), with
// its rate in `velocity`: continuous in position and rate, so small per-frame changes never shake the view.
inline constexpr double kChaseSmoothSeconds = .25;
core::Vec3 smoothDamp(const core::Vec3& current, core::Vec3& velocity, const core::Vec3& target, double dt,
                      double seconds = kChaseSmoothSeconds);
// Auto zoom: the FOV that frames `frameMeters` at `distance` (clamped), and frame-rate independent easing towards it.
double autoZoomFov(double distance, double frameMeters, double minFovRad, double maxFovRad);
double easeFov(double current, double target, double dt, double responseSeconds);

// ---------------------------------------------------------------------------------------------------------------
// Computed airport views: runway-end spots and the tower, from scenery facility data, so the runway and tower views
// work without saved or generated airport cameras (and without a camera profile).

// One runway end: its threshold and the landing direction over it.
struct RunwayEnd {
    std::string icao, name;                // e.g. "KAAA", "27L"
    Geo threshold{NAN, NAN, NAN};          // runway elevation
    double headingDeg = NAN;               // landing direction, degrees true
    double lengthMeters = 0, widthMeters = 0;
};
struct SpotAirport {
    std::string icao;
    Geo origin{NAN, NAN, NAN};
    Geo tower{NAN, NAN, NAN};              // scenery tower, else the generator's virtual tower; altitude = elevation
    bool virtualTower = false;
    std::vector<RunwayEnd> ends;
};
// From a facility document (airport::compactFacility shape): both ends of every usable runway, and the tower position.
SpotAirport spotAirport(const json& facility);
// A runway spot: where at which runway end the camera stands. kind: "fence" (150 m behind the threshold, 80 m to the
// right of the landing direction, runway camera height), "approach" (1 NM out on the extended centerline, about 90 m
// below the glidepath), "threshold" (beside the threshold) or "custom". Offsets in meters: behind the threshold
// (negative: down the runway), right of the landing direction (negative: left), above the threshold elevation.
struct RunwaySpot {
    std::string icao, runwayEnd, kind = "fence";
    double behindMeters = 0, sideMeters = 0, heightMeters = 0;
    std::string name;  // the user's name for a saved spot (shown in the status line), may be empty
};
inline constexpr std::size_t kSpotNameMax = 60;
// A spot's optional `name`: trimmed, control characters dropped, at most kSpotNameMax bytes.
std::string spotName(const json& value);
const std::vector<std::string>& runwaySpotKinds();
// The kind's default offsets at `end`; runwayHeight is the runway camera category height (fence and threshold).
RunwaySpot runwaySpot(const RunwayEnd& end, const std::string& kind, double runwayHeight);
Geo spotPosition(const RunwayEnd& end, const RunwaySpot& spot);
json toJson(const RunwaySpot& spot);
const RunwayEnd* findRunwayEnd(const std::vector<SpotAirport>& airports, const std::string& icao, const std::string& end);
// SpotTarget's optional `spot`: a known runway end, a kind (default fence) and optional offsets (behind and side
// within +-5 km, height 0-1000 m). nullopt with `error` set when it cannot be used.
std::optional<RunwaySpot> parseRunwaySpot(const json& value, const std::vector<SpotAirport>& airports, double runwayHeight,
                                          std::string& error);
// A position spot (SpotTarget.spot kind "position", view "spot"): a fixed world position (altitude meters MSL) the
// camera stands at, aimed at the target with auto zoom. Placed no lower than the terrain under the target (its ground
// altitude when the spot is placed or adjusted) plus kTerrainClearanceMeters.
struct PositionSpot {
    std::string name;
    Geo position{NAN, NAN, NAN};
};
inline constexpr double kPositionSpotMinAltitude = -500, kPositionSpotMaxAltitude = 10000;
// latitude -90..90, longitude -180..180, altitudeMeters -500..10000 and an optional name; nullopt with `error` set.
std::optional<PositionSpot> parsePositionSpot(const json& value, std::string& error);
json toJson(const PositionSpot& spot);
// The runway end for a runway view of `target`: arriving / landing aircraft: the end it is lined up to land on
// (landing direction within kRunwayAlignDeg of its track, threshold ahead or under it); takeoff / departing: the far
// end it lifts off over (the reciprocal end, whose approach side it climbs out over); otherwise, or when nothing lines
// up, the end nearest the target. Only ends within kSpotAirportRangeMeters. `aligned` tells which rule chose it.
inline constexpr double kRunwayAlignDeg = 30;
inline constexpr double kSpotAirportRangeMeters = 20000;
const RunwayEnd* pickRunwayEnd(const std::vector<SpotAirport>& airports, const Pose& target, Status status, bool* aligned = nullptr);
// The tower nearest the target within kSpotAirportRangeMeters (nullptr: none); towerHeight above the elevation.
const SpotAirport* pickTower(const std::vector<SpotAirport>& airports, const Pose& target);
Geo towerPosition(const SpotAirport& airport, double towerHeight);

}  // namespace fscs::host::traffic
