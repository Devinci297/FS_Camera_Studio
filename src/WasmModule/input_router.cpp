#include "input_router.hpp"

#include <algorithm>
#include <cctype>

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
    fastBindings_.clear();
    movementFamilies_.clear();
    fastModifiers_.clear();
    const auto configured = bindings_;
    std::set<std::string> modifiers;
    for (const auto& b : configured) {
        if (b.action != Action::MoveFast || b.input.kind != profiles::InputKind::Key) continue;
        const auto& key = b.input.definition;
        if (key == "VK_SHIFT") {
            modifiers.insert("VK_LSHIFT");
            modifiers.insert("VK_RSHIFT");
        } else if (key == "VK_LSHIFT" || key == "VK_RSHIFT" || key == "VK_LCONTROL" ||
                   key == "VK_RCONTROL" || key == "VK_LMENU" || key == "VK_RMENU") {
            modifiers.insert(key);
        }
    }
    const auto alias = [&](profiles::Binding b, const std::string& definition, const std::string& family, const std::string& modifier) {
        if (std::any_of(bindings_.begin(), bindings_.end(), [&](const auto& existing) {
                return existing.input.definition == definition;
            })) return;
        b.input.definition = definition;
        const auto index = bindings_.size();
        if (!family.empty()) movementFamilies_[index] = family;
        if (!modifier.empty()) {
            fastBindings_.insert(index);
            fastModifiers_[index] = modifier;
        }
        bindings_.push_back(std::move(b));
    };
    for (std::size_t bindingIndex = 0; bindingIndex < configured.size(); ++bindingIndex) {
        const auto& b = configured[bindingIndex];
        if (b.action == Action::MoveFast && b.input.kind == profiles::InputKind::Key &&
            b.input.definition == "VK_SHIFT") {
            alias(b, "VK_LSHIFT", {}, {});
            alias(b, "VK_RSHIFT", {}, {});
        }
        const bool movement = b.action == Action::MoveForward || b.action == Action::MoveBack ||
                              b.action == Action::MoveLeft || b.action == Action::MoveRight ||
                              b.action == Action::MoveUp || b.action == Action::MoveDown;
        if (!movement || b.input.kind != profiles::InputKind::Key || b.input.definition.size() != 1 ||
            !std::isalpha(static_cast<unsigned char>(b.input.definition[0]))) continue;
        // SimConnect key definitions are case-sensitive and modifiers are distinct chords.
        // Include both letter cases and the configured speed modifier without stealing explicit bindings.
        const auto key = static_cast<unsigned char>(b.input.definition[0]);
        const std::string lower(1, static_cast<char>(std::tolower(key)));
        const std::string upper(1, static_cast<char>(std::toupper(key)));
        movementFamilies_[bindingIndex] = lower;
        alias(b, lower, lower, {});
        alias(b, upper, lower, {});
        for (const auto& modifier : modifiers)
            for (const auto& letter : {lower, upper}) alias(b, modifier + "+" + letter, lower, modifier);
    }
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
    // Alternate bindings share one logical hold (particularly Peek). Releasing either
    // physical input must not restore the view while the other is still pressed.
    if (profiles::actionIsContinuous(binding.action) || binding.action == Action::PeekView) {
        for (const auto& [key, active] : held_) {
            const auto& other = bindings_[active.binding];
            if (other.action == binding.action && other.viewId == binding.viewId) {
                held_[physical] = {b, active.holdId};
                return {};
            }
        }
    }
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
    for (const auto& [key, active] : held_) if (active.holdId == e.holdId) return {};
    return {e};
}

std::vector<ActionEvent> InputRouter::onInput(std::size_t index, bool down, std::uint32_t value) {
    auto map = bindingsForDefinition_.find(index);
    if (map == bindingsForDefinition_.end()) return {};
    const auto& candidates = map->second;
    const auto& first = bindings_[candidates.front()];
    if (first.input.kind == profiles::InputKind::SimZoom) {
        if (!down || textFocus_) return {};
        return {{first.action, first.viewId, true, 0, true}};
    }
    const bool pov = bindings_[candidates.front()].input.isPov();
    std::vector<ActionEvent> out;
    if (!pov) {
        const std::string physical = "d" + std::to_string(index);
        if (down) {
            if (!physicallyDown_.insert(physical).second) return {};  // repeat
            out = press(candidates.front(), physical);  // one action per input
        } else {
            // A key's release may arrive under a different case/chord after Shift changes.
            // Reconcile only our movement aliases; unrelated configured shortcuts stay independent.
            const auto family = movementFamilies_.find(candidates.front());
            const auto& releasedDefinition = first.input.definition;
            const auto isShift = [](const std::string& def) {
                return def == "VK_SHIFT" || def == "VK_LSHIFT" || def == "VK_RSHIFT";
            };
            for (const auto& [otherIndex, otherCandidates] : bindingsForDefinition_) {
                const auto other = otherCandidates.front();
                const auto otherFamily = movementFamilies_.find(other);
                const auto modifier = fastModifiers_.find(other);
                const bool sameFamily = family != movementFamilies_.end() &&
                    otherFamily != movementFamilies_.end() && family->second == otherFamily->second;
                const bool releasedModifier = modifier != fastModifiers_.end() &&
                    (modifier->second == releasedDefinition || (releasedDefinition == "VK_SHIFT" && isShift(modifier->second)));
                const auto& otherBinding = bindings_[other];
                const bool shiftAlias = isShift(releasedDefinition) && otherBinding.action == Action::MoveFast &&
                    isShift(otherBinding.input.definition) &&
                    (releasedDefinition == "VK_SHIFT" || otherBinding.input.definition == "VK_SHIFT");
                if (otherIndex != index && !sameFamily && !releasedModifier && !shiftAlias) continue;
                const std::string otherPhysical = "d" + std::to_string(otherIndex);
                physicallyDown_.erase(otherPhysical);
                auto events = release(otherPhysical);
                out.insert(out.end(), events.begin(), events.end());
            }
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
    std::set<std::uint64_t> released;
    for (auto it = held_.begin(); it != held_.end();) {
        const auto& b = bindings_[it->second.binding];
        if (b.action != Action::ReleaseCamera) {
            if (released.insert(it->second.holdId).second) out.push_back({b.action, b.viewId, false, it->second.holdId});
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
    std::set<std::uint64_t> released;
    for (const auto& [physical, h] : held_) {
        const auto& b = bindings_[h.binding];
        if (released.insert(h.holdId).second) out.push_back({b.action, b.viewId, false, h.holdId});
    }
    held_.clear();
    mustRelease_ = physicallyDown_;  // anything still held must come back to neutral first
    return out;
}

core::ManualInput InputRouter::manualInput() const {
    core::ManualInput m;
    std::set<Action> applied;
    for (const auto& [physical, h] : held_) {
        m.fast = m.fast || fastBindings_.count(h.binding) > 0;
        if (!applied.insert(bindings_[h.binding].action).second) continue;
        switch (bindings_[h.binding].action) {
            case Action::LookLeft: m.lookRight -= 1; break;
            case Action::LookRight: m.lookRight += 1; break;
            case Action::LookUp: m.lookUp += 1; break;
            case Action::LookDown: m.lookUp -= 1; break;
            case Action::MoveFast: m.fast = true; break;
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
