#pragma once
// In-module input adapter logic (design 2.0 §9.1). Bindings are mapped through the simulator
// (SimConnect_MapInputEventToClientEvent_EX1); the simulator reports down/up per mapped definition, and POV events
// carry the hat angle. This class turns those into logical actions with deterministic edges:
//  * one down edge per press, every hold has a matching release (paired by holdId, used as the peek token);
//  * text entry in a panel suppresses everything except Release Camera;
//  * context loss releases all holds, and inputs still held afterwards must return to neutral first.

#include "fscs/core/engine.hpp"
#include "fscs/profiles/bindings.hpp"

#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace fscs::host {

struct ActionEvent {
    profiles::Action action;
    int viewId = 0;
    bool pressed = true;
    std::uint64_t holdId = 0;
    bool pulse = false; // Simulator zoom commands have no matching release edge.
};

class InputRouter {
public:
    // Returns the unique definitions to map, in index order (index = position in this list).
    const std::vector<std::string>& setBindings(std::vector<profiles::Binding> bindings);
    const std::vector<std::string>& definitions() const { return definitions_; }

    // A mapped definition went down/up. For POV definitions `value` is the angle in hundredths of a degree
    // (0 = forward, 9000 = right; anything above 35999 = centred) and `down` is ignored.
    std::vector<ActionEvent> onInput(std::size_t index, bool down, std::uint32_t value);
    std::vector<ActionEvent> setTextFocus(bool focused);
    // Flight/session change, device loss or module suspension.
    std::vector<ActionEvent> releaseAll();

    core::ManualInput manualInput() const;

private:
    struct Held {
        std::size_t binding;
        std::uint64_t holdId;
    };
    std::vector<ActionEvent> press(std::size_t bindingIndex, const std::string& physical);
    std::vector<ActionEvent> release(const std::string& physical);

    std::vector<profiles::Binding> bindings_;
    std::set<std::size_t> fastBindings_; // Runtime modifier chords, never saved over user assignments.
    std::vector<std::string> definitions_;
    std::map<std::size_t, std::vector<std::size_t>> bindingsForDefinition_;
    std::set<std::string> physicallyDown_;
    std::set<std::string> mustRelease_;  // held through a context loss: ignore until released
    std::map<std::string, Held> held_;
    std::map<std::size_t, std::set<profiles::PovDirection>> povState_;
    bool textFocus_ = false;
    std::uint64_t nextHoldId_ = 1;
};

std::set<profiles::PovDirection> povDirections(std::uint32_t angle);

}  // namespace fscs::host
