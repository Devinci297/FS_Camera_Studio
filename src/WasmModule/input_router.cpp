#include "input_router.hpp"

#include <algorithm>

namespace fscs::host {

using profiles::Action;
using profiles::PovDirection;

std::set<PovDirection> povDirections(std::uint32_t angle) {
    if (angle > 35999) return {};  // centred
    // Eight sectors of 45 degrees centred on the cardinal and diagonal directions.
    const int sector = static_cast<int>(((angle + 2250) % 36000) / 4500);
    switch (sector) {
        case 0: return {PovDirection::Up};
        case 1: return {PovDirection::Up, PovDirection::Right};
        case 2: return {PovDirection::Right};
        case 3: return {PovDirection::Right, PovDirection::Down};
        case 4: return {PovDirection::Down};
        case 5: return {PovDirection::Down, PovDirection::Left};
        case 6: return {PovDirection::Left};
        default: return {PovDirection::Left, PovDirection::Up};
    }
}

const std::vector<std::string>& InputRouter::setBindings(std::vector<profiles::Binding> bindings) {
    releaseAll();
    bindings_ = std::move(bindings);
    definitions_.clear();
    bindingsForDefinition_.clear();
    povState_.clear();
    physicallyDown_.clear();
    mustRelease_.clear();
    for (std::size_t b = 0; b < bindings_.size(); ++b) {
        const auto& def = bindings_[b].input.definition;
        auto it = std::find(definitions_.begin(), definitions_.end(), def);
        std::size_t index = static_cast<std::size_t>(it - definitions_.begin());
        if (it == definitions_.end()) definitions_.push_back(def);
        bindingsForDefinition_[index].push_back(b);
    }
    return definitions_;
}

std::vector<ActionEvent> InputRouter::press(std::size_t b, const std::string& physical) {
    if (mustRelease_.count(physical)) return {};
    const auto& binding = bindings_[b];
    if (textFocus_ && binding.action != Action::ReleaseCamera) return {};
    if (held_.count(physical)) return {};
    const std::uint64_t id = nextHoldId_++;
    held_[physical] = {b, id};
    return {{binding.action, binding.viewId, true, id}};
}

std::vector<ActionEvent> InputRouter::release(const std::string& physical) {
    mustRelease_.erase(physical);
    auto it = held_.find(physical);
    if (it == held_.end()) return {};
    const auto& binding = bindings_[it->second.binding];
    ActionEvent e{binding.action, binding.viewId, false, it->second.holdId};
    held_.erase(it);
    return {e};
}

std::vector<ActionEvent> InputRouter::onInput(std::size_t index, bool down, std::uint32_t value) {
    auto map = bindingsForDefinition_.find(index);
    if (map == bindingsForDefinition_.end()) return {};
    const auto& candidates = map->second;
    const bool pov = bindings_[candidates.front()].input.isPov();
    std::vector<ActionEvent> out;
    if (!pov) {
        const std::string physical = "d" + std::to_string(index);
        if (down) {
            if (!physicallyDown_.insert(physical).second) return {};  // repeat
            out = press(candidates.front(), physical);  // one action per input
        } else {
            physicallyDown_.erase(physical);
            out = release(physical);
        }
        return out;
    }
    const auto now = povDirections(value);
    auto& prev = povState_[index];
    for (auto d : prev)
        if (!now.count(d)) {
            const std::string physical = "p" + std::to_string(index) + ":" + std::to_string(static_cast<int>(d));
            physicallyDown_.erase(physical);
            auto e = release(physical);
            out.insert(out.end(), e.begin(), e.end());
        }
    for (auto d : now)
        if (!prev.count(d)) {
            const std::string physical = "p" + std::to_string(index) + ":" + std::to_string(static_cast<int>(d));
            physicallyDown_.insert(physical);
            for (std::size_t b : candidates)
                if (bindings_[b].input.pov == d) {
                    auto e = press(b, physical);
                    out.insert(out.end(), e.begin(), e.end());
                    break;
                }
        }
    prev = now;
    return out;
}

std::vector<ActionEvent> InputRouter::setTextFocus(bool focused) {
    if (focused == textFocus_) return {};
    textFocus_ = focused;
    if (!focused) return {};
    std::vector<ActionEvent> out;
    for (auto it = held_.begin(); it != held_.end();) {
        const auto& b = bindings_[it->second.binding];
        if (b.action != Action::ReleaseCamera) {
            out.push_back({b.action, b.viewId, false, it->second.holdId});
            mustRelease_.insert(it->first);
            it = held_.erase(it);
        } else {
            ++it;
        }
    }
    return out;
}

std::vector<ActionEvent> InputRouter::releaseAll() {
    std::vector<ActionEvent> out;
    for (const auto& [physical, h] : held_) {
        const auto& b = bindings_[h.binding];
        out.push_back({b.action, b.viewId, false, h.holdId});
    }
    held_.clear();
    mustRelease_ = physicallyDown_;  // anything still held must come back to neutral first
    return out;
}

core::ManualInput InputRouter::manualInput() const {
    core::ManualInput m;
    for (const auto& [physical, h] : held_) {
        switch (bindings_[h.binding].action) {
            case Action::LookLeft: m.lookRight -= 1; break;
            case Action::LookRight: m.lookRight += 1; break;
            case Action::LookUp: m.lookUp += 1; break;
            case Action::LookDown: m.lookUp -= 1; break;
            case Action::MoveForward: m.moveForward += 1; break;
            case Action::MoveBack: m.moveForward -= 1; break;
            case Action::MoveRight: m.moveRight += 1; break;
            case Action::MoveLeft: m.moveRight -= 1; break;
            case Action::MoveUp: m.moveUp += 1; break;
            case Action::MoveDown: m.moveUp -= 1; break;
            case Action::ZoomIn: m.zoomIn += 1; break;
            case Action::ZoomOut: m.zoomIn -= 1; break;
            default: break;
        }
    }
    return m;
}

}  // namespace fscs::host
