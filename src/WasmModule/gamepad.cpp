#include "gamepad.hpp"

#include <algorithm>
#include <cmath>

namespace fscs::host {

using profiles::GamepadFunction;

const std::vector<std::string>& GamepadSticks::configure(const std::vector<profiles::GamepadAxis>& axes, double deadzone) {
    auto previous = std::move(definitions_);
    auto samples = std::move(raw_);
    definitions_.clear();
    uses_.clear();
    deadzone_ = std::clamp(std::isfinite(deadzone) ? deadzone : 0.15, 0.0, profiles::kGamepadMaxDeadzone);
    for (const auto& a : axes) {
        if (!profiles::validAxisDefinition(a.definition) || !(std::abs(a.full - a.rest) >= profiles::kGamepadMinSpan)) continue;
        auto it = std::find(definitions_.begin(), definitions_.end(), a.definition);
        const auto index = static_cast<std::size_t>(it - definitions_.begin());
        if (it == definitions_.end()) {
            definitions_.push_back(a.definition);
            uses_.emplace_back();
        }
        uses_[index].push_back({a.function, a.rest, a.full});
    }
    // Unchanged axes keep their samples (a settings edit elsewhere must not drop a held stick for a moment).
    if (previous == definitions_) raw_ = std::move(samples);
    else raw_.assign(definitions_.size(), std::nullopt);
    return definitions_;
}

void GamepadSticks::onAxis(std::size_t index, std::int32_t raw) {
    if (index < raw_.size()) raw_[index] = raw;
}

void GamepadSticks::neutral() { std::fill(raw_.begin(), raw_.end(), std::nullopt); }

double GamepadSticks::shape(double value, double deadzone) {
    if (!std::isfinite(value)) return 0;
    const double v = std::clamp(value, -1.0, 1.0), magnitude = std::abs(v);
    if (magnitude <= deadzone) return 0;
    const double t = (magnitude - deadzone) / (1 - deadzone);
    // Mostly cubic: a gentle push creeps for framing, full deflection is the full keyboard speed.
    return std::copysign(0.25 * t + 0.75 * t * t * t, v);
}

core::ManualInput GamepadSticks::input() const {
    core::ManualInput m;
    for (std::size_t i = 0; i < raw_.size(); ++i) {
        if (!raw_[i]) continue;
        for (const auto& u : uses_[i]) {
            const double value = shape((static_cast<double>(*raw_[i]) - u.rest) / (u.full - u.rest), deadzone_);
            switch (u.function) {
                case GamepadFunction::Forward: m.moveForward += value; break;
                case GamepadFunction::Right: m.moveRight += value; break;
                case GamepadFunction::Climb: m.moveUp += value; break;
                case GamepadFunction::Descend: m.moveUp -= value; break;
                case GamepadFunction::Yaw: m.lookRight += value; break;
                case GamepadFunction::Pitch: m.lookUp += value; break;
            }
        }
    }
    const auto clamp = [](double& v) { v = std::clamp(v, -1.0, 1.0); };
    clamp(m.moveForward); clamp(m.moveRight); clamp(m.moveUp); clamp(m.lookRight); clamp(m.lookUp);
    return m;
}

bool GamepadSticks::active() const {
    const auto m = input();
    return m.moveForward != 0 || m.moveRight != 0 || m.moveUp != 0 || m.lookRight != 0 || m.lookUp != 0;
}

std::optional<AxisListener::Result> AxisListener::onSample(int axis, std::int32_t raw) {
    if (axis < 0 || axis >= static_cast<int>(rest_.size())) return std::nullopt;
    auto& rest = rest_[static_cast<std::size_t>(axis)];
    if (!rest) {
        rest = raw;
        return std::nullopt;
    }
    const double delta = static_cast<double>(raw) - *rest;
    if (std::abs(delta) < kMoveThreshold) return std::nullopt;
    Result r;
    r.axis = axis;
    r.rest = *rest;
    const double end = profiles::kGamepadAxisLimit - 1;
    // A trigger at rest sits near one end and travels to the other; a centred stick goes to the end it was pushed to.
    r.full = std::abs(r.rest) >= kTriggerRest ? -std::copysign(end, r.rest) : std::copysign(end, delta);
    if (std::abs(r.full - r.rest) < profiles::kGamepadMinSpan) return std::nullopt;
    return r;
}

}  // namespace fscs::host
