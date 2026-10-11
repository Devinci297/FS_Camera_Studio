#pragma once
// Camera engine: pure calculations, independent of the SDK, files and UI (design §5, §7).
// Saved preset, transient manual look and the transition are kept separate; the engine only composes them.

#include "fscs/core/math.hpp"

#include <cstdint>
#include <optional>

namespace fscs::core {

constexpr double kMaxPositionMeters = 10000.0;
constexpr double kMinFovRad = 0.1;
constexpr double kMaxFovRad = 2.8;
constexpr double kMinTransitionSeconds = 0.1;   // design 2.0 §4.2: 0.1-15 s, default 0.65 s
constexpr double kMaxTransitionSeconds = 15.0;
constexpr double kDefaultTransitionSeconds = 0.65;
constexpr double kMaxDelaySeconds = 30.0;

struct CameraPose {
    Vec3 positionFRD;   // meters, anchor frame
    Quat orientation;   // camera -> anchor
    double fovRad = 1.0;

    bool finite() const { return positionFRD.finite() && orientation.finite() && std::isfinite(fovRad); }
    bool withinLimits() const;
};

// Instant, Smooth (cubic smoothstep) and Cinematic (quintic smoothstep), design 2.0 §7.3.
enum class Easing { Cut, Smoothstep3, Smoothstep5 };

double ease(Easing kind, double u);

struct TransitionSpec {
    Easing easing = Easing::Smoothstep3;
    double durationSeconds = kDefaultTransitionSeconds;
    double delaySeconds = 0.0;
};

// A timed move between two poses, driven by an active-time clock that only advances while the camera may be
// controlled. Timing comes from elapsed time, never from an estimated frame count.
class Transition {
public:
    Transition(const CameraPose& from, const CameraPose& to, const TransitionSpec& spec);

    void advance(double dtSeconds);
    CameraPose sample() const;
    bool finished() const;
    const CameraPose& target() const { return to_; }
    double elapsed() const { return elapsed_; }

private:
    CameraPose from_, to_;
    TransitionSpec spec_;
    double elapsed_ = 0.0;
};

// Rates in [-1, 1] from held inputs.
struct ManualInput {
    double lookRight = 0, lookUp = 0;
    double moveForward = 0, moveRight = 0, moveUp = 0;
    double zoomIn = 0;
    bool fast = false;
};

struct ManualSpeeds {
    double lookDegPerSec = 60.0;
    double moveMetersPerSec = 0.5;
    double zoomRadPerSec = 0.5;
    double accelerationPerSec = 6.0;  // how fast the commanded rate approaches the target (1/s)
};

// Temporary look offset relative to the saved preset (FR08: never written into the preset implicitly).
// Translation is expressed in the preset's heading frame: forward/right along the view's heading, down = anchor down.
struct TemporaryLook {
    double headingDeg = 0, pitchDeg = 0;
    Vec3 translationHeadingFrame;
    double fovDeltaRad = 0;

    bool active() const;
};

CameraPose compose(const CameraPose& base, const TemporaryLook& look);

// Fixed 240 Hz integration of manual look/move, independent of the output rate (design §7.3).
class ManualMover {
public:
    static constexpr double kStep = 1.0 / 240.0;
    static constexpr double kMaxGapSeconds = 0.1;

    void setSpeeds(const ManualSpeeds& s) { speeds_ = s; }
    // Advances by dt. A gap above kMaxGapSeconds suspends and reseeds instead of catching up.
    void advance(double dtSeconds, const ManualInput& input, TemporaryLook& look);
    void neutralize() { rate_ = {}; accumulator_ = 0; }

private:
    ManualSpeeds speeds_;
    ManualInput rate_;  // smoothed commanded rates
    double accumulator_ = 0;
};

struct EngineSample {
    CameraPose pose;
    bool valid = false;
};

class CameraEngine {
public:
    // Starts a transition to `target`. Interrupting begins from the last output, never from the old target.
    void select(const CameraPose& target, const TransitionSpec& spec, bool resetLook);
    // Places the camera immediately (used for initial seeding and live draft previews).
    void place(const CameraPose& pose);
    void tick(double dtSeconds, const ManualInput& input, std::optional<Quat> movementOrientation = {});
    void offsetLook(double headingDeg, double pitchDeg);
    void offsetPosition(const Vec3& delta);
    void offsetFov(double deltaRad);
    void animateBase(const CameraPose& pose) { if (pose.withinLimits() && !transition_) base_ = pose; }
    void freeze();  // simulator suspension: stop time and held motion
    void recenterLook() { look_ = {}; mover_.neutralize(); }
    void setSpeeds(const ManualSpeeds& s) { mover_.setSpeeds(s); }

    EngineSample sample() const;
    bool hasPose() const { return base_.has_value(); }
    bool transitioning() const { return transition_.has_value(); }
    const TemporaryLook& look() const { return look_; }
    std::optional<CameraPose> basePose() const;  // saved/transition pose without temporary look
    void reset();

private:
    std::optional<CameraPose> base_;
    std::optional<Transition> transition_;
    TemporaryLook look_;
    ManualMover mover_;
};

}  // namespace fscs::core
