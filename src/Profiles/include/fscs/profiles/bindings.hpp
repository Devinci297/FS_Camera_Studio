#pragma once
// Bindings (doc/protocol.md §2.2) and settings (§2.3), schema 2. Inputs are MSFS input definition strings mapped
// in-module with SimConnect_MapInputEventToClientEvent_EX1 (design 2.0 §9.1). Bindings never travel in a profile.

#include <nlohmann/json.hpp>

#include <optional>
#include <map>
#include <string>
#include <vector>

namespace fscs::profiles {

using json = nlohmann::json;

enum class Action {
    SelectView, PeekView, NextCamera, PreviousCamera, NextCategory, ReturnToPilot, ReleaseCamera, RecenterLook,
    ToggleEditor, ToggleMini, ToggleMouseLook, LookLeft, LookRight, LookUp, LookDown, MoveForward, MoveBack, MoveLeft, MoveRight,
    MoveUp, MoveDown, ZoomIn, ZoomOut, MoveFast, ToggleHeadTracking, ToggleAirportAuto, SaveViews, PanicRelease,
    Binoculars,
    GlanceHold, GlanceNext, GlanceReturn,
    ToggleCinematic,
    SpotNext, SpotPrevious, SpotReturn, ToggleAutoSpot
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

// Gamepad sticks for free cameras (drones), like the simulator's own drone camera: an analog joystick axis per drone
// motion, mapped in-module only while a drone view is shown. `rest` is the raw value with the control released and
// `full` the value at full deflection in the motion's positive direction (SimConnect axes report about -32K..32K), so
// value = (raw - rest) / (full - rest). A centred stick is bipolar; a trigger resting at one end gives 0..1.
enum class GamepadFunction { Forward, Right, Climb, Descend, Yaw, Pitch };
const char* toString(GamepadFunction f);
std::optional<GamepadFunction> parseGamepadFunction(const std::string& s);
struct GamepadAxis {
    GamepadFunction function = GamepadFunction::Forward;
    std::string definition;  // "joystick:<device>:<XAxis|YAxis|ZAxis|RxAxis|RyAxis|RzAxis|Slider>"
    std::string label;       // display only
    double rest = 0, full = 32767;
};
constexpr std::size_t kGamepadFunctionCount = 6;
constexpr double kGamepadAxisLimit = 32768;
constexpr double kGamepadMinSpan = 4096;  // a smaller rest-to-full distance is noise, not a control
constexpr double kGamepadMaxDeadzone = 0.5;
// True for a joystick axis definition the module maps for gamepad sticks.
bool validAxisDefinition(const std::string& definition);
// At most one axis per function (the first), valid definitions only, rest/full clamped and at least kGamepadMinSpan apart.
json normalizeGamepadAxes(const json& axes);
std::vector<GamepadAxis> parseGamepadAxes(const json& axes);

struct HostSettings {
    bool allowUnqualifiedAdapter = false;
    double lookSpeedDegPerSec = 60;
    double airportRadiusNm = 25;
    double mouseSensitivity = 1.0;
    double mouseSmoothingMs = 55.0;
    bool mouseLookEnabled = true;
    bool globalHeadTrackingEnabled = false;
    std::string globalAircraftTracking = "both";
    bool airportTrackingHorizonGuard = true;
    bool airportAutoZoomEnabled = false;
    double airportAutoZoomFrameMeters = 40;
    double airportAutoZoomMinFovDeg = 10;
    double airportAutoZoomMaxFovDeg = 90;
    double airportAutoZoomResponseSeconds = 1;
    std::map<std::string,double> airportCategoryHeights{{"runways",3},{"gates",3},{"apron",3},{"taxi",3},{"tower",35}};
    // Temporary airport sets: generate tower/runway/spotting views in memory near airports without saved cameras.
    bool airportAutoGenerate = true;
    bool mouseInvertY = false;
    double moveSpeedMetersPerSec = 0.5;
    double movementPrecisionScale = 1.0;
    // Hold-to-zoom FOV (presentation only, never saved into a pose). May go below the editor's 30-degree floor.
    double binocularsFovDeg = 10;
    // Cinematic mode (automatic view playlist): dwell range, share of cockpit views, cuts to world-referential views
    // and the Commercial drift.
    double cinematicMinSeconds = 8, cinematicMaxSeconds = 20;
    double cinematicCockpitWeight = .15;
    bool cinematicCuts = true, cinematicDrift = true;
    // Spotting: traffic within this radius of the user aircraft; auto-spot shows each aircraft at least this long.
    double spottingRadiusNm = 20;
    double autoSpotHoldSeconds = 30;
    // Gamepad sticks fly drones (free cameras); blocksAircraft masks the mapped axes from the aircraft meanwhile.
    bool gamepadSticksEnabled = true;
    bool gamepadBlocksAircraft = true;
    double gamepadDeadzone = 0.15;
    std::vector<GamepadAxis> gamepadAxes;
};
constexpr double kBinocularsMinFovDeg = 2;
constexpr double kBinocularsMaxFovDeg = 60;
constexpr double kCinematicMinDwellSeconds = 3;
constexpr double kCinematicMaxDwellSeconds = 300;
constexpr double kSpottingMinRadiusNm = 1;
constexpr double kSpottingMaxRadiusNm = 100;
constexpr double kAutoSpotMinHoldSeconds = 10;
constexpr double kAutoSpotMaxHoldSeconds = 300;

// host.spottingSpots: the user's saved spotting spots (runway spots and position spots), kept without a camera
// profile. At most kSpottingSpotsMax; names at most kSpottingSpotNameMax bytes.
constexpr std::size_t kSpottingSpotsMax = 200;
constexpr std::size_t kSpottingSpotNameMax = 60;
constexpr double kSpottingSpotOffsetMaxMeters = 5000, kSpottingSpotHeightMaxMeters = 1000;
constexpr double kSpottingSpotMinAltitude = -500, kSpottingSpotMaxAltitude = 10000;
// Each entry {id, name, icao, kind (fence, approach, threshold, custom, position), runwayEnd + behindMeters /
// sideMeters / heightMeters (runway kinds) or latitude / longitude / altitudeMeters (position)}: unknown kinds,
// runway spots without an end and position spots without a usable position are dropped; offsets and the altitude are
// clamped; a missing or repeated id gets a new one; an empty name becomes "Spot".
json normalizeSpottingSpots(const json& spots);

constexpr int kSettingsSchemaVersion = 2;
HostSettings parseHostSettings(const json& settingsDoc);
// Fills missing keys with defaults and clamps values. Unknown keys are dropped.
json normalizeSettings(const json& doc);
json defaultSettingsDocument();

}  // namespace fscs::profiles
