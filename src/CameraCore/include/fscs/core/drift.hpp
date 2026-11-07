#pragma once
// Commercial drift (Cinematic mode): a slow, level dolly along a view's own forward or right axis and a small FOV
// ease over one dwell. Presentation only, like follow horizon and binoculars: it is added to the pose at submission
// and never enters the engine, saved poses, captures or the temporary look.

#include "fscs/core/engine.hpp"

#include <algorithm>
#include <cmath>

namespace fscs::core {

// Dolly speed and bounds: the travel is speed x dwell, never more than kDriftMaxMeters; the FOV changes by at most
// kDriftMaxFovDeg (and at most kDriftMaxFovFraction of the view's FOV).
inline constexpr double kDriftSpeedMetersPerSec = 0.12;
inline constexpr double kDriftMaxMeters = 2.5;
inline constexpr double kDriftMaxFovDeg = 3.0;
inline constexpr double kDriftMaxFovFraction = 0.05;
// A smooth cinematic transition keeps the previous view's offset and lets it fade with this time constant.
inline constexpr double kDriftCarrySeconds = 0.6;

struct DriftSpec {
    Vec3 direction;            // anchor frame, level (z = 0) and unit length
    double meters = 0;         // total travel over the dwell, 0..kDriftMaxMeters
    double fovDeltaRad = 0;    // FOV change at the end of the dwell; negative narrows
    double seconds = 10;       // dwell the drift is spread over
};

// The level unit direction of a view's forward (axis 0) or right (axis 1) vector. A view looking straight up or
// down has no level forward direction, so it uses its right axis.
inline Vec3 driftDirection(const Quat& orientation, int axis, double sign) {
    const auto flat = [](Vec3 v) {
        v.z = 0;
        const double n = v.length();
        return n > 1e-6 ? v * (1. / n) : Vec3{};
    };
    Vec3 d = flat(orientation.rotate(axis == 0 ? Vec3{1, 0, 0} : Vec3{0, 1, 0}));
    if (d.length() < .5) d = flat(orientation.rotate({0, 1, 0}));
    return d * (sign < 0 ? -1. : 1.);
}

class CommercialDrift {
public:
    // Starts a drift for a new view. `carry` keeps the current offset and fades it out (a smooth transition between
    // views in the same frame); otherwise the previous offset is dropped (a cut).
    void start(const DriftSpec& spec, bool carry) {
        if (carry) {
            carryOffset_ = offset();
            carryFovRad_ = fovDelta();
        } else {
            carryOffset_ = {};
            carryFovRad_ = 0;
        }
        spec_ = spec;
        spec_.meters = clamp(std::isfinite(spec.meters) ? spec.meters : 0., 0., kDriftMaxMeters);
        const double maxFov = kDriftMaxFovDeg * kDegToRad;
        spec_.fovDeltaRad = clamp(std::isfinite(spec.fovDeltaRad) ? spec.fovDeltaRad : 0., -maxFov, maxFov);
        spec_.seconds = std::isfinite(spec.seconds) && spec.seconds > 1 ? spec.seconds : 1.;
        const double n = spec.direction.length();
        spec_.direction = n > 1e-6 && std::isfinite(n) ? spec.direction * (1. / n) : Vec3{};
        elapsed_ = 0;
        active_ = true;
    }
    // Stops without a fade (release, panic, a manual recall, Cinematic mode off).
    void stop() { *this = CommercialDrift{}; }
    // Advances the dolly unless held (a manual-input pause, glance, binoculars or a simulator pause); a carried
    // offset fades regardless, except while the simulator is paused (`frozen`).
    void tick(double dt, bool hold, bool frozen = false) {
        if (!std::isfinite(dt) || dt <= 0 || frozen) return;
        if (active_ && !hold) elapsed_ = std::min(spec_.seconds, elapsed_ + dt);
        const double keep = std::exp(-dt / kDriftCarrySeconds);
        carryOffset_ = carryOffset_ * keep;
        carryFovRad_ *= keep;
        if (carryOffset_.length() < 1e-5 && std::abs(carryFovRad_) < 1e-7) {
            carryOffset_ = {};
            carryFovRad_ = 0;
        }
    }
    bool active() const { return active_; }
    double progress() const { return active_ ? clamp(elapsed_ / spec_.seconds, 0., 1.) : 0.; }
    const DriftSpec& spec() const { return spec_; }
    // Current offset (anchor frame) and FOV change, carry included.
    Vec3 offset() const {
        return (active_ ? spec_.direction * (spec_.meters * ease(Easing::Smoothstep3, progress())) : Vec3{}) + carryOffset_;
    }
    double fovDelta() const { return (active_ ? spec_.fovDeltaRad * ease(Easing::Smoothstep3, progress()) : 0.) + carryFovRad_; }
    // The shown pose. The FOV change is limited to a fraction of the view's own FOV and stays within the camera limits.
    CameraPose apply(const CameraPose& pose) const {
        CameraPose out = pose;
        const Vec3 o = offset();
        if (o.finite()) out.positionFRD += o;
        const double limit = std::abs(pose.fovRad) * kDriftMaxFovFraction;
        const double d = clamp(fovDelta(), -limit, limit);
        if (std::isfinite(d) && d != 0) out.fovRad = clamp(pose.fovRad + d, kMinFovRad, kMaxFovRad);
        return out;
    }

private:
    DriftSpec spec_;
    double elapsed_ = 0;
    bool active_ = false;
    Vec3 carryOffset_;
    double carryFovRad_ = 0;
};

}  // namespace fscs::core
