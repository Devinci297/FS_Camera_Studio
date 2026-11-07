#pragma once
#include "fscs/core/engine.hpp"

namespace fscs::core {
// Camera-local presentation only. These offsets never enter saved presets or temporary look.
// Defaults are the editor's "Realistic" preset values (ui/app.js EFFECT_DEFAULTS) with enabled off. Keep them, the
// validateProfile fallbacks (src/Profiles/src/profile.cpp) and EFFECT_DEFAULTS in sync.
struct EffectsSettings {
    bool enabled = false;
    double intensity = .35, turbulence = .5, groundRoll = .35, touchdown = .5, acceleration = .35, engine = .2;
    // Biomechanical head model (cockpit views only). Idle sway is optional and off by default.
    double headLag = .5, turnLean = .5, taxiBob = .5, idleSway = 0;
};
struct FlightMotion {
    double onGround = 1, groundSpeed = 0, verticalSpeed = 0, gForce = 1;
    double forwardAcceleration = 0, lateralAcceleration = 0, engineRpm = 0;
    double parkingBrake = NAN;
    bool finite() const {
        return std::isfinite(onGround) && std::isfinite(groundSpeed) && std::isfinite(verticalSpeed) &&
               std::isfinite(gForce) && std::isfinite(forwardAcceleration) &&
               std::isfinite(lateralAcceleration) && std::isfinite(engineRpm);
    }
};
// Aircraft-relative exterior views (chase, orbit) move at reduced strength: a camera
// outside the aircraft should not shake like the pilot's head.
inline constexpr double kExteriorEffectsScale = .5;

// Pilot's head relative to the seat, in the aircraft frame: eye offset (m) plus pitch/roll (degrees, about the
// aircraft's lateral and longitudinal axes, so a side-facing view leans with the torso rather than turning).
struct HeadPose {
    Vec3 offset;
    double pitchDeg = 0, rollDeg = 0;
};
// Natural head movement for cockpit views. Every term is driven only by FlightMotion and elapsed time. Strengths are
// the per-camera settings (0..1) times `master` (intensity x view scale); constants give the full-strength response.
class HeadModel {
public:
    // Neck spring-damper: ~1 Hz natural frequency, slightly under-damped (~5% overshoot, ~0.9 s 2% settle).
    static constexpr double kNeckOmega = 6.5, kNeckDamping = .7;
    // Sub-step limit for the spring: frames up to 0.1 s integrate stably and match small frames closely.
    static constexpr double kMaxSubstep = 1. / 120.;
    // Head lag per g of excitation: back under forward acceleration, outward in turns, down under extra G.
    static constexpr double kLagForwardM = .10, kLagLateralM = .08, kLagVerticalM = .06;
    static constexpr double kLagPitchDegPerForwardG = 4., kLagPitchDegPerG = -5., kLagRollDeg = -3.;
    // Lean into turns: slow (sustained) response, soft-limited to a few degrees, with a small inward shift.
    static constexpr double kLeanSeconds = .6, kLeanDegPerG = 20., kMaxLeanDeg = 4., kLeanShiftMPerDeg = .003;
    // Taxi bob: low-frequency vertical/pitch bob from ground speed (the ground-roll effect is the fast vibration).
    static constexpr double kBobM = .04, kBobPitchDeg = 1.5, kBobFadeSeconds = .4;
    // Idle sway while parked: breathing-rate vertical motion and slow postural drift.
    static constexpr double kSwayFadeSeconds = 1.5;
    // Final bounds, far inside CameraPose::withinLimits.
    static constexpr double kMaxOffsetM = .15, kMaxAngleDeg = 6.;
    static constexpr double kTau = 6.283185307179586;

    void reset() { *this = HeadModel{}; }
    void tick(double dt, const FlightMotion& m, const EffectsSettings& settings, double master) {
        if (!std::isfinite(dt) || dt > .1 || !m.finite()) { reset(); return; }
        if (dt <= 0) return;
        constexpr double g = 9.80665;
        const auto amount = [](double v) { return std::isfinite(v) ? clamp(v, 0., 1.) : 0.; };
        const auto blend = [dt](double seconds) { return 1. - std::exp(-dt / seconds); };
        master = amount(master);
        const bool ground = m.onGround > .5;
        const double speed = m.groundSpeed > 0 ? m.groundSpeed : 0.;
        // 1. Neck spring-damper towards the sustained inertial load (unit gain, in g).
        const double lag = amount(settings.headLag) * master;
        const Vec3 load{clamp(m.forwardAcceleration / g, -1., 1.) * lag, clamp(m.lateralAcceleration / g, -1., 1.) * lag,
                        clamp(m.gForce - 1., -1., 2.) * lag};
        const int steps = static_cast<int>(std::ceil(dt / kMaxSubstep));
        const double h = dt / steps;
        for (int i = 0; i < steps; ++i) {  // semi-implicit Euler
            neckVelocity_ += ((load - neck_) * (kNeckOmega * kNeckOmega) - neckVelocity_ * (2. * kNeckDamping * kNeckOmega)) * h;
            neck_ += neckVelocity_ * h;
        }
        // 2. Lean into sustained lateral acceleration.
        lean_ += (clamp(m.lateralAcceleration / g, -1., 1.) * amount(settings.turnLean) * master - lean_) * blend(kLeanSeconds);
        // 3. Taxi bob: fades in from walking pace, out towards rotation speed; frequency rises gently with speed.
        const double taxi = ground ? clamp((speed - .5) / 6., 0., 1.) * (1. - clamp((speed - 30.) / 15., 0., 1.)) : 0.;
        bob_ += (taxi * amount(settings.taxiBob) * master - bob_) * blend(kBobFadeSeconds);
        bobPhase_ = std::fmod(bobPhase_ + kTau * clamp(.9 + .05 * speed, .9, 1.8) * dt, kTau);
        // 4. Idle sway only while parked.
        const double parked = ground && speed < .5 ? 1. : 0.;
        sway_ += (parked * amount(settings.idleSway) * master - sway_) * blend(kSwayFadeSeconds);
        swayTime_ = std::fmod(swayTime_ + dt, 1000.);  // all sway periods divide 1000 s

        const double leanDeg = kMaxLeanDeg * std::tanh(lean_ * kLeanDegPerG / kMaxLeanDeg);
        const double bobWave = (std::sin(bobPhase_) + .35 * std::sin(1.7 * bobPhase_ + 1.1)) / 1.35;
        const auto wave = [this](double hz, double phase) { return std::sin(kTau * hz * swayTime_ + phase); };
        Vec3 offset{-kLagForwardM * neck_.x, -kLagLateralM * neck_.y + kLeanShiftMPerDeg * leanDeg,
                    kLagVerticalM * neck_.z + kBobM * bob_ * bobWave};
        offset += Vec3{.003 * wave(.07, 0), .006 * wave(.11, 1.), .004 * wave(.25, 2.)} * sway_;
        const double pitch = kLagPitchDegPerForwardG * neck_.x + kLagPitchDegPerG * neck_.z +
                             kBobPitchDeg * bob_ * std::sin(bobPhase_ - .6) + .25 * sway_ * wave(.13, 2.5);
        const double roll = kLagRollDeg * neck_.y + leanDeg + .4 * sway_ * wave(.09, .5);
        pose_.offset = {clamp(offset.x, -kMaxOffsetM, kMaxOffsetM), clamp(offset.y, -kMaxOffsetM, kMaxOffsetM),
                        clamp(offset.z, -kMaxOffsetM, kMaxOffsetM)};
        pose_.pitchDeg = clamp(pitch, -kMaxAngleDeg, kMaxAngleDeg);
        pose_.rollDeg = clamp(roll, -kMaxAngleDeg, kMaxAngleDeg);
    }
    const HeadPose& pose() const { return pose_; }
private:
    Vec3 neck_, neckVelocity_;
    double lean_ = 0, bob_ = 0, bobPhase_ = 0, sway_ = 0, swayTime_ = 0;
    HeadPose pose_;
};

class CameraEffects {
public:
    void reset() { *this = CameraEffects{}; }
    // scale (0..1) multiplies every output (offsets and pitch) on top of settings.intensity.
    // head enables the biomechanical head model (cockpit views); otherwise it stays at rest.
    void tick(double dt, const FlightMotion& m, const EffectsSettings& settings, bool eligible, double scale = 1.,
              bool head = false) {
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
        const double master = amount(settings.intensity) * amount(scale);
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
        if (head) head_.tick(dt, m, settings, master); else head_.reset();
    }
    CameraPose apply(const CameraPose& base) const {
        CameraPose result = base;
        const HeadPose& head = head_.pose();
        result.positionFRD += offset_ + head.offset;
        // The head turns about the aircraft's axes (pre-multiplied); the existing dip stays camera-local.
        result.orientation = (Quat::fromPbh({head.pitchDeg, head.rollDeg, 0}) * base.orientation *
                              Quat::fromPbh({pitch_, 0, 0})).normalized();
        return result.withinLimits() ? result : base;
    }
    const Vec3& offset() const { return offset_; }  // vehicle effects only; the head model is head()
    const HeadPose& head() const { return head_.pose(); }
private:
    bool seeded_ = false, ground_ = true;
    double previousVs_ = 0, meanG_ = 1, phase_ = 0, impact_ = 0, pitch_ = 0;
    Vec3 offset_;
    HeadModel head_;
};
} // namespace fscs::core
