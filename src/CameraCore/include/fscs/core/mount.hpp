#pragma once
// Mounted action cam (camera mode "mounted"): a camera rigidly fixed to the airframe on the SimObject anchor, with an
// optional gimbal and its own airframe vibration. Presentation only: neither ever enters saved poses, captures or
// the temporary look.
//
// The SimObject anchor carries the aircraft's attitude A = Rz(H)*Ry(P)*Rx(B) (see horizon.hpp), so the camera's world
// orientation is A*q. The gimbal replaces q by a q' that holds the wanted world attitude:
//   locked  - q' = q: the camera moves with the airframe.
//   horizon - keeps the world heading and pitch the mount gives the lens, but replaces its world roll with the
//             authored roll (normally 0), so the horizon stays level in a bank for any lens direction. For a
//             forward-facing lens this equals the follow-horizon counter-roll at followHorizon 0.
//   yaw     - A*q' = Rz(H)*q: level in pitch and bank, follows the aircraft's heading only.

#include "fscs/core/effects.hpp"
#include "fscs/core/horizon.hpp"

#include <numbers>
#include <optional>
#include <string>

namespace fscs::core {

enum class Gimbal { Locked, Horizon, Yaw };

inline const char* toString(Gimbal g) {
    switch (g) {
        case Gimbal::Locked: return "locked";
        case Gimbal::Horizon: return "horizon";
        case Gimbal::Yaw: return "yaw";
    }
    return "locked";
}

inline std::optional<Gimbal> parseGimbal(const std::string& s) {
    if (s == "locked") return Gimbal::Locked;
    if (s == "horizon") return Gimbal::Horizon;
    if (s == "yaw") return Gimbal::Yaw;
    return std::nullopt;
}

inline constexpr double kDefaultMountVibration = .3;

// The stabilised camera->anchor orientation. pitchDeg/bankDeg: the aircraft's attitude in FRD degrees (nose up > 0,
// right wing down > 0). Heading cancels out: it pre-multiplies both sides.
inline Quat gimbalOrientation(const Quat& q, Gimbal g, double pitchDeg, double bankDeg) {
    if (g == Gimbal::Locked || !std::isfinite(pitchDeg) || !std::isfinite(bankDeg)) return q;
    const Quat tilt = Quat::fromPbh({wrapDeg(pitchDeg), wrapDeg(bankDeg), 0});
    if (g == Gimbal::Yaw) return (tilt.conjugate() * q).normalized();
    const Pbh world = (tilt * q).normalized().toPbh();  // heading relative to the aircraft's heading
    const Quat level = Quat::fromPbh({world.pitchDeg, q.toPbh().bankDeg, world.headingDeg});
    return (tilt.conjugate() * level).normalized();
}

// Eases the gimbal in and out (same time constant as follow horizon) and keeps the last known attitude, so a recall,
// a gimbal change to or from locked, and a telemetry gap never snap the view.
class GimbalFilter {
public:
    void reset() { *this = GimbalFilter{}; }
    // wanted: the gimbal to ease towards; Locked eases the last stabilising mode out. A non-finite attitude keeps the
    // previous one.
    void tick(double dt, Gimbal wanted, double pitchDeg, double bankDeg) {
        if (std::isfinite(pitchDeg) && std::isfinite(bankDeg)) { pitchDeg_ = pitchDeg; bankDeg_ = bankDeg; }
        if (wanted != Gimbal::Locked) mode_ = wanted;
        const double target = wanted == Gimbal::Locked ? 0. : 1.;
        if (std::isfinite(dt) && dt > 0) weight_ += (target - weight_) * -std::expm1(-dt / kHorizonSmoothingSeconds);
    }
    Quat apply(const Quat& q) const {
        if (mode_ == Gimbal::Locked || weight_ <= 0) return q;
        const Quat stabilised = gimbalOrientation(q, mode_, pitchDeg_, bankDeg_);
        return weight_ >= 1 - 1e-12 ? stabilised : slerpShortest(q, stabilised, weight_).normalized();
    }
    double weight() const { return weight_; }

private:
    Gimbal mode_ = Gimbal::Locked;
    double weight_ = 0, pitchDeg_ = 0, bankDeg_ = 0;
};

// Airframe vibration for mounted cameras: small, fast and stiff, unlike the head-like CameraEffects. Engine RPM drives
// a fine buzz whose frequency rises with RPM; ground roll adds a coarser shudder that grows with ground speed. Each
// position axis stays within amount * kMountVibrationMaxMeters and each angle within amount * kMountVibrationMaxDeg.
inline constexpr double kMountVibrationMaxMeters = .006;
inline constexpr double kMountVibrationMaxDeg = .3;

class MountVibration {
public:
    void reset() { *this = MountVibration{}; }
    void tick(double dt, const FlightMotion& m, double amount, bool eligible) {
        if (!eligible || !m.finite() || !std::isfinite(amount) || amount <= 0 || !std::isfinite(dt) || dt < 0 || dt > .1) {
            reset();
            return;
        }
        if (dt == 0) return;
        const double a = clamp(amount, 0., 1.);
        const double rpm = clamp(m.engineRpm / 2500., 0., 1.);
        const double roll = m.onGround > .5 ? clamp((m.groundSpeed - .5) / 20., 0., 1.) : 0.;
        // Amplitudes ease in and out so a recall or an engine start never steps; their sum never exceeds `a`.
        const double ease = 1. - std::exp(-dt / .2);
        engine_ += (a * .35 * rpm - engine_) * ease;
        ground_ += (a * .65 * roll - ground_) * ease;
        constexpr double tau = 2 * std::numbers::pi;
        enginePhase_ = std::fmod(enginePhase_ + dt * tau * (12. + 10. * rpm), tau * 1000.);
        groundPhase_ = std::fmod(groundPhase_ + dt * tau * (5. + 5. * roll), tau * 1000.);
        // Sums of incommensurate sines, each bounded by 1, one blend per axis.
        const auto wave = [](double phase, double k) {
            return .5 * std::sin(phase * k) + .3 * std::sin(phase * k * 1.618 + 1.1) + .2 * std::sin(phase * k * 2.414 + 2.3);
        };
        const auto signal = [&](double k) { return engine_ * wave(enginePhase_, k) + ground_ * wave(groundPhase_, k * .93); };
        offset_ = Vec3{signal(1.07) * .4, signal(.89) * .7, signal(1.0)} * kMountVibrationMaxMeters;
        jitter_ = Pbh{signal(1.13) * kMountVibrationMaxDeg, signal(.97) * kMountVibrationMaxDeg * .8, signal(1.21) * kMountVibrationMaxDeg * .5};
    }
    // Position shakes in the anchor frame (the airframe moves the mount); the jitter rotates the lens about itself.
    CameraPose apply(const CameraPose& base) const {
        CameraPose result = base;
        result.positionFRD += offset_;
        result.orientation = (base.orientation * Quat::fromPbh(jitter_)).normalized();
        return result.finite() && result.withinLimits() ? result : base;
    }
    const Vec3& offset() const { return offset_; }
    const Pbh& jitterDeg() const { return jitter_; }

private:
    double engine_ = 0, ground_ = 0, enginePhase_ = 0, groundPhase_ = 0;
    Vec3 offset_;
    Pbh jitter_;
};

}  // namespace fscs::core
