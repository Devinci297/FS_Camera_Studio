#pragma once
#include "fscs/core/engine.hpp"

namespace fscs::core {
// Camera-local presentation only. These offsets never enter saved presets or temporary look.
struct EffectsSettings {
    bool enabled = false;
    double intensity = .35, turbulence = .5, groundRoll = .35, touchdown = .5, acceleration = .35, engine = .2;
};
struct FlightMotion {
    double onGround = 1, groundSpeed = 0, verticalSpeed = 0, gForce = 1;
    double forwardAcceleration = 0, lateralAcceleration = 0, engineRpm = 0;
    bool finite() const {
        return std::isfinite(onGround) && std::isfinite(groundSpeed) && std::isfinite(verticalSpeed) &&
               std::isfinite(gForce) && std::isfinite(forwardAcceleration) &&
               std::isfinite(lateralAcceleration) && std::isfinite(engineRpm);
    }
};
class CameraEffects {
public:
    void reset() { *this = CameraEffects{}; }
    void tick(double dt, const FlightMotion& m, const EffectsSettings& settings, bool eligible) {
        if (!eligible || !settings.enabled || !m.finite() || !std::isfinite(dt) || dt < 0 || dt > .1) { reset(); return; }
        if (dt == 0) return;
        if (!seeded_) { ground_ = m.onGround > .5; previousVs_ = m.verticalSpeed; meanG_ = m.gForce; seeded_ = true; }
        const bool ground = m.onGround > .5;
        if (ground && !ground_) impact_ = clamp(-previousVs_ / 4., 0., 1.);
        previousVs_ = m.verticalSpeed; ground_ = ground;
        phase_ += dt;
        impact_ *= std::exp(-dt * 5.);
        meanG_ += (m.gForce - meanG_) * (1. - std::exp(-dt * .7));
        const auto amount = [](double v) { return std::isfinite(v) ? clamp(v, 0., 1.) : 0.; };
        const double master = amount(settings.intensity);
        const double rough = ground ? amount(settings.groundRoll) * clamp((m.groundSpeed - .5) / 25., 0., 1.) : 0.;
        const double air = ground ? 0. : amount(settings.turbulence) * clamp(m.gForce - meanG_, -1., 1.);
        const double motor = amount(settings.engine) * clamp(m.engineRpm / 2500., 0., 1.);
        const double hit = amount(settings.touchdown) * impact_;
        const double inertia = amount(settings.acceleration);
        Vec3 desired{
            -clamp(m.forwardAcceleration / 9.80665, -1., 1.) * inertia * .045,
            -clamp(m.lateralAcceleration / 9.80665, -1., 1.) * inertia * .04,
            air * .04 + rough * .008 * std::sin(phase_ * 43.) + hit * .025 + motor * .0015 * std::sin(phase_ * 67.)
        };
        desired = desired * master;
        offset_ = lerp(offset_, desired, 1. - std::exp(-dt * 18.));
        pitch_ += ((-hit * 1.5 - air * .6) * master - pitch_) * (1. - std::exp(-dt * 18.));
    }
    CameraPose apply(const CameraPose& base) const {
        CameraPose result = base;
        result.positionFRD += offset_;
        result.orientation = (base.orientation * Quat::fromPbh({pitch_, 0, 0})).normalized();
        return result.withinLimits() ? result : base;
    }
    const Vec3& offset() const { return offset_; }
private:
    bool seeded_ = false, ground_ = true;
    double previousVs_ = 0, meanG_ = 1, phase_ = 0, impact_ = 0, pitch_ = 0;
    Vec3 offset_;
};
} // namespace fscs::core
