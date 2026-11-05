#pragma once
// Follow horizon (per camera, 0..1): presentation-only counter-roll against the aircraft's bank. Never enters saved
// poses or the temporary look.
//
// Aircraft-relative anchors (pilot eyepoint, SimObject) carry the aircraft's attitude A = Rz(H)*Ry(P)*Rx(B), so the
// camera's world orientation is A*q. Pre-multiplying the camera->anchor orientation by Rx(-B*(1-follow)) gives
// Rz(H)*Ry(P)*Rx(B*follow)*q: the view inherits only `follow` of the bank, and at 0 a forward view's horizon is
// exactly level whatever the pitch. The roll is about the aircraft's longitudinal axis (anchor +x), not the
// camera's own axis, so side-facing views level too instead of turning into a pitch.

#include "fscs/core/engine.hpp"

namespace fscs::core {

constexpr double kHorizonSmoothingSeconds = 0.15;

// Counter-roll in degrees (FRD: positive bank = right wing down) to apply about the anchor's forward axis.
inline double horizonCounterRollDeg(double aircraftBankDeg, double followHorizon) {
    if (!std::isfinite(aircraftBankDeg) || !std::isfinite(followHorizon)) return 0;
    return -wrapDeg(aircraftBankDeg) * (1 - clamp(followHorizon, 0., 1.));
}

// Rotates the pose about the anchor's forward axis. `aboutAnchor` also swings the position about the anchor origin:
// exterior views orbit the aircraft and keep it framed; cockpit views keep the eye where it is.
inline CameraPose counterRoll(const CameraPose& pose, double rollDeg, bool aboutAnchor) {
    if (!std::isfinite(rollDeg) || rollDeg == 0) return pose;
    const Quat r = Quat::fromAxisAngle({1, 0, 0}, rollDeg * kDegToRad);
    CameraPose out = pose;
    out.orientation = (r * pose.orientation).normalized();
    if (aboutAnchor) out.positionFRD = r.rotate(pose.positionFRD);
    return out.finite() && out.withinLimits() ? out : pose;
}

// Frame-rate independent exponential easing of the applied counter-roll, so recalls and telemetry gaps never snap.
class HorizonFilter {
public:
    void reset() { rollDeg_ = 0; }
    double tick(double dt, double targetDeg) {
        if (!std::isfinite(targetDeg)) targetDeg = 0;
        if (std::isfinite(dt) && dt > 0) rollDeg_ += (targetDeg - rollDeg_) * -std::expm1(-dt / kHorizonSmoothingSeconds);
        return rollDeg_;
    }
    double rollDeg() const { return rollDeg_; }

private:
    double rollDeg_ = 0;
};

}  // namespace fscs::core
