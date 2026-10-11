#pragma once
// Bindings (doc/protocol.md §2.2) and settings (§2.3), schema 2. Inputs are MSFS input definition strings mapped
// in-module with SimConnect_MapInputEventToClientEvent_EX1 (design 2.0 §9.1). Bindings never travel in a profile.

#include <nlohmann/json.hpp>

#include <optional>
#include <string>
#include <vector>

namespace fscs::profiles {

using json = nlohmann::json;

enum class Action {
    SelectView, PeekView, NextCamera, PreviousCamera, NextCategory, ReturnToPilot, ReleaseCamera, RecenterLook,
    ToggleEditor, ToggleMini, ToggleMouseLook, LookLeft, LookRight, LookUp, LookDown, MoveForward, MoveBack, MoveLeft, MoveRight,
    MoveUp, MoveDown, ZoomIn, ZoomOut, MoveFast, ToggleHeadTracking, ToggleAirportAuto, SaveViews
};
const char* toString(Action a);
std::optional<Action> parseAction(const std::string& s);
bool actionTakesViewId(Action a);
// Held actions drive continuous motion; the rest fire on the down edge.
bool actionIsContinuous(Action a);

enum class InputKind { Key, Joystick, SimZoom };
enum class PovDirection { None, Up, Right, Down, Left };

struct InputSpec {
    InputKind kind = InputKind::Key;
    std::string definition;   // e.g. "VK_LCONTROL+VK_LMENU+1", "joystick:1:button:4", "joystick:1:POV:0"
    std::string label;        // display only
    int deviceId = -1;        // joystick only
    std::string deviceName;   // joystick only, display
    PovDirection pov = PovDirection::None;  // set for POV (hat) inputs

    bool isPov() const { return pov != PovDirection::None; }
    // Two specs collide when they would be indistinguishable to the simulator.
    bool sameInput(const InputSpec& o) const { return definition == o.definition && pov == o.pov; }
    json toJson() const;
    static std::optional<InputSpec> fromJson(const json& j);
};

// True for definitions the simulator accepts: up to two VK_ modifiers and up to two keys, or joystick forms.
bool validDefinition(const std::string& definition);

struct Binding {
    Action action = Action::NextCamera;
    int viewId = 0;
    InputSpec input;
};

struct BindingSet {
    std::vector<Binding> bindings;
    std::vector<std::string> problems;  // dropped entries, in plain language
};

constexpr int kBindingsSchemaVersion = 2;
BindingSet parseBindings(const json& doc);
json defaultBindingsDocument();
// One-time migration: expose the old runtime defaults without overwriting custom assignments.
json upgradeMovementBindings(json doc);

struct HostSettings {
    bool allowUnqualifiedAdapter = false;
    double lookSpeedDegPerSec = 60;
    double airportRadiusNm = 25;
    double mouseSensitivity = 1.0;
    double mouseSmoothingMs = 55.0;
    bool mouseLookEnabled = true;
    bool mouseInvertY = false;
    double moveSpeedMetersPerSec = 0.5;
    double movementPrecisionScale = 1.0;
};

constexpr int kSettingsSchemaVersion = 2;
HostSettings parseHostSettings(const json& settingsDoc);
// Fills missing keys with defaults and clamps values. Unknown keys are dropped.
json normalizeSettings(const json& doc);
json defaultSettingsDocument();

}  // namespace fscs::profiles
