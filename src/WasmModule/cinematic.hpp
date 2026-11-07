#pragma once
// Cinematic mode (automatic view playlist): flight-phase detection from telemetry, phase-weighted view choice with
// an injectable random source, and the dwell clock with its manual-input pause. Pure logic; ModuleApp drives it
// (cinematic.cpp) and the coordinator applies the Commercial drift at submission.

#include "fscs/core/drift.hpp"
#include "fscs/core/effects.hpp"
#include "fscs/profiles/profile.hpp"

#include <cstdint>
#include <deque>
#include <functional>
#include <string>
#include <vector>

namespace fscs::host::cinematic {

enum class Phase { Parked, Taxi, TakeoffClimb, Cruise, Approach, LandingRoll };
const char* toString(Phase p);

// Flight phase from the existing motion telemetry (on ground, ground speed, vertical speed), with hysteresis so a
// phase does not flicker at a threshold.
class PhaseTracker {
public:
    static constexpr double kParkedExit = 1.5, kParkedEnter = 1.0;       // m/s ground speed (about 3 and 2 kt)
    static constexpr double kRollEnter = 20.0, kRollExit = 15.0;         // m/s: takeoff / landing roll (about 40 / 30 kt)
    static constexpr double kClimbEnter = 2.0, kClimbExit = 0.75;        // m/s vertical speed (about 400 / 150 fpm)
    static constexpr double kDescentEnter = -2.0, kDescentExit = -0.75;  // m/s
    static constexpr double kAfterLiftoffSeconds = 45;   // airborne this long after liftoff counts as the climb-out

    Phase update(const core::FlightMotion& m, double now);
    Phase phase() const { return phase_; }
    void reset() { *this = PhaseTracker{}; }

private:
    Phase phase_ = Phase::Parked;
    bool known_ = false, airborne_ = false;
    bool landing_ = false;  // touched down and not yet slowed below kRollExit
    double liftoffAt_ = -1e9;
};

// What the director needs to know about one camera.
struct Candidate {
    std::string id;
    profiles::CameraMode mode = profiles::CameraMode::AircraftExterior;
    std::string airportCategory;  // "tower", "runways", ... for airport views
};

// Relative share of a view type in a phase (shared by all views of that type). 0 = not offered. Cockpit views are
// chosen separately with the cockpit weight; free cameras are never offered.
double typeWeight(Phase phase, profiles::CameraMode mode);
// Tower and runway views are favoured on the ground and on approach.
inline constexpr double kAirportFeatureBoost = 2.0;

struct Settings {
    double minSeconds = 8, maxSeconds = 20;
    double cockpitWeight = .15;  // share of picks that are cockpit views
    bool cuts = true;            // cut to world-referential (airport, fly-by) views; smooth otherwise
    bool drift = true;           // Commercial drift on exterior and airport views
};

class Director {
public:
    using Random = std::function<double()>;  // uniform in [0, 1)
    static constexpr double kManualPauseSeconds = 10;
    static constexpr std::size_t kHistory = 4;         // recently shown views are penalised
    static constexpr double kRecentPenalty = .15;      // weight multiplier for a recent view

    explicit Director(Random random = {});
    void setRandom(Random random);
    double random();

    // Chooses the next view. Never `current` unless it is the only candidate; recent views are penalised. Empty when
    // there are no candidates.
    std::string pick(const std::vector<Candidate>& candidates, const std::string& current, Phase phase, double cockpitWeight);
    double drawDwell(double minSeconds, double maxSeconds);
    // A Commercial drift spec for `orientation` over `seconds`: random axis (forward or right) and direction.
    core::DriftSpec drawDrift(const core::Quat& orientation, double seconds);

    // Dwell clock. `shown` starts a new dwell for a view the director chose.
    void shown(const std::string& id, double dwellSeconds, double now);
    // Manual look/move input pauses the clock for kManualPauseSeconds after the last input.
    void manualInput(double now) { pausedUntil_ = now + kManualPauseSeconds; }
    // Advances the clock unless paused or `suspended`; true when the dwell is over.
    bool tick(double now, bool suspended);
    bool paused(double now) const { return now < pausedUntil_; }
    const std::string& cameraId() const { return cameraId_; }
    double elapsed() const { return elapsed_; }
    double dwell() const { return dwell_; }
    const std::deque<std::string>& history() const { return history_; }
    void reset();

private:
    Random random_;
    std::uint64_t state_ = 0;  // default source: SplitMix64
    std::string cameraId_;
    std::deque<std::string> history_;
    double elapsed_ = 0, dwell_ = 0, lastTick_ = -1, pausedUntil_ = -1;
};

}  // namespace fscs::host::cinematic
