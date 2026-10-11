#include "fscs/core/engine.hpp"

#include <algorithm>

namespace fscs::core {

bool CameraPose::withinLimits() const {
    return finite() && std::abs(positionFRD.x) <= kMaxPositionMeters && std::abs(positionFRD.y) <= kMaxPositionMeters &&
           std::abs(positionFRD.z) <= kMaxPositionMeters && fovRad >= kMinFovRad && fovRad <= kMaxFovRad &&
           std::abs(orientation.norm() - 1.0) < 1e-6;
}

double ease(Easing kind, double u) {
    u = clamp(u, 0.0, 1.0);
    switch (kind) {
        case Easing::Cut: return 1.0;
        case Easing::Smoothstep3: return u * u * (3.0 - 2.0 * u);
        case Easing::Smoothstep5: return u * u * u * (u * (u * 6.0 - 15.0) + 10.0);
    }
    return 1.0;
}

Transition::Transition(const CameraPose& from, const CameraPose& to, const TransitionSpec& spec)
    : from_(from), to_(to), spec_(spec) {
    spec_.delaySeconds = clamp(spec_.delaySeconds, 0.0, kMaxDelaySeconds);
    if (spec_.easing == Easing::Cut) {
        spec_.durationSeconds = 0.0;
    } else {
        spec_.durationSeconds = clamp(spec_.durationSeconds, kMinTransitionSeconds, kMaxTransitionSeconds);
    }
}

void Transition::advance(double dt) {
    if (dt > 0.0 && std::isfinite(dt)) elapsed_ += dt;
}

CameraPose Transition::sample() const {
    const double moving = elapsed_ - spec_.delaySeconds;
    if (moving < 0.0) return from_;
    if (spec_.durationSeconds <= 0.0 || moving >= spec_.durationSeconds) return to_;
    const double e = ease(spec_.easing, moving / spec_.durationSeconds);
    return {lerp(from_.positionFRD, to_.positionFRD, e), slerpShortest(from_.orientation, to_.orientation, e),
            from_.fovRad + (to_.fovRad - from_.fovRad) * e};
}

bool Transition::finished() const { return elapsed_ >= spec_.delaySeconds + spec_.durationSeconds; }

bool TemporaryLook::active() const {
    return headingDeg != 0.0 || pitchDeg != 0.0 || translationHeadingFrame.length() > 0.0 || fovDeltaRad != 0.0;
}

CameraPose compose(const CameraPose& base, const TemporaryLook& look) {
    if (!look.active()) return base;
    Pbh angles = base.orientation.toPbh();
    const Quat headingFrame = Quat::fromPbh({0, 0, angles.headingDeg});
    angles.headingDeg = wrapDeg(angles.headingDeg + look.headingDeg);
    angles.pitchDeg = clamp(angles.pitchDeg + look.pitchDeg, -89.0, 89.0);
    CameraPose out;
    out.orientation = Quat::fromPbh(angles);
    out.positionFRD = base.positionFRD + headingFrame.rotate(look.translationHeadingFrame);
    out.fovRad = clamp(base.fovRad + look.fovDeltaRad, kMinFovRad, kMaxFovRad);
    return out;
}

namespace {
double approach(double current, double target, double alpha) { return current + (target - current) * alpha; }
double clampUnit(double v) { return std::isfinite(v) ? clamp(v, -1.0, 1.0) : 0.0; }
}  // namespace

void ManualMover::advance(double dt, const ManualInput& in, TemporaryLook& look) {
    if (!(dt > 0.0) || !std::isfinite(dt)) return;
    if (dt > kMaxGapSeconds) {
        // Do not integrate obsolete movement after a stall; reseed from rest.
        neutralize();
        return;
    }
    ManualInput target{clampUnit(in.lookRight), clampUnit(in.lookUp),    clampUnit(in.moveForward),
                       clampUnit(in.moveRight), clampUnit(in.moveUp),    clampUnit(in.zoomIn)};
    // Normalise multi-axis translation so diagonals are not faster.
    const double moveLen = Vec3{target.moveForward, target.moveRight, target.moveUp}.length();
    if (moveLen > 1.0) {
        target.moveForward /= moveLen;
        target.moveRight /= moveLen;
        target.moveUp /= moveLen;
    }
    const double alpha = -std::expm1(-kStep * speeds_.accelerationPerSec);
    accumulator_ += dt;
    while (accumulator_ >= kStep) {
        accumulator_ -= kStep;
        rate_.lookRight = approach(rate_.lookRight, target.lookRight, alpha);
        rate_.lookUp = approach(rate_.lookUp, target.lookUp, alpha);
        rate_.moveForward = approach(rate_.moveForward, target.moveForward, alpha);
        rate_.moveRight = approach(rate_.moveRight, target.moveRight, alpha);
        rate_.moveUp = approach(rate_.moveUp, target.moveUp, alpha);
        rate_.zoomIn = approach(rate_.zoomIn, target.zoomIn, alpha);

        look.headingDeg = wrapDeg(look.headingDeg + rate_.lookRight * speeds_.lookDegPerSec * kStep);
        look.pitchDeg = clamp(look.pitchDeg + rate_.lookUp * speeds_.lookDegPerSec * kStep, -178.0, 178.0);
        look.translationHeadingFrame +=
            Vec3{rate_.moveForward, rate_.moveRight, -rate_.moveUp} * (speeds_.moveMetersPerSec * (in.fast ? 4.0 : 1.0) * kStep);
        look.fovDeltaRad = clamp(look.fovDeltaRad - rate_.zoomIn * speeds_.zoomRadPerSec * kStep, -kMaxFovRad, kMaxFovRad);
    }
    // Snap tiny residual rates to zero so a released input fully stops.
    auto settle = [](double& r) { if (std::abs(r) < 1e-4) r = 0; };
    settle(rate_.lookRight); settle(rate_.lookUp); settle(rate_.moveForward);
    settle(rate_.moveRight); settle(rate_.moveUp); settle(rate_.zoomIn);
}

void CameraEngine::select(const CameraPose& target, const TransitionSpec& spec, bool resetLook) {
    if (resetLook) recenterLook();
    if (!base_) {
        base_ = target;
        transition_.reset();
        return;
    }
    const CameraPose from = transition_ ? transition_->sample() : *base_;
    transition_.emplace(from, target, spec);
    base_ = target;
    if (transition_->finished()) transition_.reset();
}

void CameraEngine::place(const CameraPose& pose) {
    base_ = pose;
    transition_.reset();
}

void CameraEngine::tick(double dt, const ManualInput& input, std::optional<Quat> movementOrientation) {
    if (transition_) {
        transition_->advance(dt);
        if (transition_->finished()) transition_.reset();
    }
    auto relative = input;
    if (const auto base = basePose()) {
        const auto orientation = movementOrientation.value_or(sample().pose.orientation);
        auto angles = orientation.toPbh();
        angles.headingDeg = wrapDeg(angles.headingDeg - base->orientation.toPbh().headingDeg);
        const auto direction = Quat::fromPbh(angles).rotate({input.moveForward,input.moveRight,0});
        relative.moveForward=direction.x; relative.moveRight=direction.y; relative.moveUp=input.moveUp-direction.z;
    }
    mover_.advance(dt, relative, look_);
}

void CameraEngine::freeze() { mover_.neutralize(); }

void CameraEngine::offsetPosition(const Vec3& delta) {
    if (!base_ || !delta.finite()) return;
    const auto old = look_.translationHeadingFrame;
    look_.translationHeadingFrame += delta;
    if (!sample().valid) look_.translationHeadingFrame = old;
}

void CameraEngine::offsetFov(double deltaRad) {
    const auto base = basePose();
    if (!base || !std::isfinite(deltaRad)) return;
    look_.fovDeltaRad = clamp(base->fovRad + look_.fovDeltaRad + deltaRad, kMinFovRad, kMaxFovRad) - base->fovRad;
}

void CameraEngine::offsetLook(double headingDeg, double pitchDeg) {
    if (!base_ || !std::isfinite(headingDeg) || !std::isfinite(pitchDeg)) return;
    look_.headingDeg = wrapDeg(look_.headingDeg + headingDeg);
    const auto base = basePose();
    const double pitch = base ? base->orientation.toPbh().pitchDeg : 0;
    look_.pitchDeg = clamp(pitch + look_.pitchDeg + pitchDeg, -89.0, 89.0) - pitch;
}

std::optional<CameraPose> CameraEngine::basePose() const {
    if (!base_) return std::nullopt;
    return transition_ ? transition_->sample() : *base_;
}

EngineSample CameraEngine::sample() const {
    const auto b = basePose();
    if (!b) return {};
    EngineSample s;
    s.pose = compose(*b, look_);
    s.pose.orientation = s.pose.orientation.normalized();
    s.valid = s.pose.withinLimits();
    return s;
}

void CameraEngine::reset() {
    base_.reset();
    transition_.reset();
    recenterLook();
}

}  // namespace fscs::core
