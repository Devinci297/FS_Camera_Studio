#pragma once
// Gamepad sticks for free cameras (drones), like the simulator's own drone camera: left stick moves, right stick
// turns, triggers climb and descend (whatever the user assigned in Settings > Gamepad). Axes are mapped in-module with
// SimConnect joystick axis definitions; SimConnect reports each axis about six times a second as a raw value of about
// -32K..32K, so the latest sample is held as the stick position until the next one arrives.

#include "fscs/core/engine.hpp"
#include "fscs/profiles/bindings.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace fscs::host {

// The axis names SimConnect accepts after "joystick:<device>:", and their display names.
constexpr std::array<const char*, 7> kGamepadAxisNames{"XAxis", "YAxis", "ZAxis", "RxAxis", "RyAxis", "RzAxis", "Slider"};
constexpr std::array<const char*, 7> kGamepadAxisLabels{"X axis", "Y axis", "Z axis", "X rotation", "Y rotation", "Z rotation", "Slider"};

class GamepadSticks {
public:
    // Returns the unique definitions to map, in index order (AxisInput.index refers to a position in this list).
    const std::vector<std::string>& configure(const std::vector<profiles::GamepadAxis>& axes, double deadzone);
    // The standard layout on one controller, as the simulator's drone camera: left stick (X, Y) moves, right stick
    // (X and Y rotation) turns and looks, triggers climb (right) and descend (left). Each axis is calibrated from its
    // first sample after the sticks turn on: near the centre it is a stick (or both triggers on one Z axis); near one
    // end it is a trigger resting there (Z: left trigger, Z rotation: right trigger). -1 maps nothing.
    const std::vector<std::string>& configureStandard(int deviceId, double deadzone);
    bool standard() const { return standard_; }
    const std::vector<std::string>& definitions() const { return definitions_; }
    bool empty() const { return definitions_.empty(); }
    void onAxis(std::size_t index, std::int32_t raw);
    // Forget every sample: the sticks read as released until new samples arrive (mapping off, flight change).
    void neutral();
    // Unit rates for the drone: forward, right, up (climb less descend), look right and look up.
    core::ManualInput input() const;
    bool active() const;
    // One axis value after the dead zone and the response curve: fine near the centre, full speed at full deflection.
    static double shape(double value, double deadzone);

private:
    struct Use { profiles::GamepadFunction function; double rest, full; };
    // Standard layout: what the first sample of each axis showed (a centred stick or a trigger resting at one end).
    struct Calibration { bool trigger = false; double rest = 0; };
    std::vector<Use> standardUses(std::size_t index) const;
    bool standard_ = false;
    std::vector<std::optional<Calibration>> calibration_;
    std::vector<std::string> definitions_;
    std::vector<std::vector<Use>> uses_;
    std::vector<std::optional<std::int32_t>> raw_;
    double deadzone_ = 0.15;
};

// Settings > Gamepad > Listen: watches every axis of one controller, takes each axis's first sample as its rest
// value, and picks the first axis moved well away from it. The result's `full` is the end of travel in the direction
// moved: the far end for a trigger (rest near one end), otherwise that end of a centred axis.
class AxisListener {
public:
    struct Result { int axis = -1; double rest = 0, full = 0; };
    void start() { rest_.fill(std::nullopt); }
    std::optional<Result> onSample(int axis, std::int32_t raw);
    static constexpr double kMoveThreshold = 16384;  // half of one side's travel
    static constexpr double kTriggerRest = 16384;    // a rest this far off centre is a trigger (or pedal) at one end

private:
    std::array<std::optional<std::int32_t>, kGamepadAxisNames.size()> rest_{};
};

}  // namespace fscs::host
