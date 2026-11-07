#pragma once
// ICameraBackend (design §15.1): the only seam through which the coordinator touches the simulator.
// SimConnectBackend implements it for MSFS 2024; FakeBackend (tests) delays, drops and reorders.

#include "adapter.hpp"
#include "fscs/core/effects.hpp"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace fscs::host {

enum class SdkAvailability { NotAcquired, Acquired, AcquiredByOther, UserDisabled };

namespace backend_event {
struct NativeMouseButton { bool down = false; };
struct NativeMouseAxis { bool horizontal = true; std::int32_t delta = 0; };
struct NativeMouseMapping { bool ready = false; };
struct AircraftPosition { double lat=0, lon=0, altitude=0, headingDeg=0; };
struct AirportList { nlohmann::json airports; bool complete=false; };
struct AirportIdentity { std::string ident, region, icao; double lat=0, lon=0; };
struct AirportFacility { std::uint32_t requestId=0; nlohmann::json facility; std::string error; };
// bankDeg: aircraft bank in FRD degrees (right wing down > 0) for follow horizon; NAN when not reported.
// pitchDeg: aircraft pitch in FRD degrees (nose up > 0) for mounted-camera gimbals; NAN when not reported.
struct MotionTelemetry { core::FlightMotion motion; double bankDeg = NAN; double pitchDeg = NAN; };
// Terrain elevation under the user aircraft (SimConnect GROUND ALTITUDE, meters MSL): free-camera altitude floor and
// height-above-ground speed scaling near the aircraft.
struct GroundElevation { double meters = NAN; };
// Spotting: AI and multiplayer aircraft within the spotting radius, one SimConnect poll (SIMOBJECT_TYPE_AIRCRAFT).
// Meters, degrees true and m/s; pitch/bank FRD (nose up, right wing down > 0); groundAltitude is the terrain under it.
// userObjectId: the user aircraft's object id from its own data requests (0 while unknown), never a target.
struct TrafficAircraft {
    std::uint32_t objectId = 0;
    double lat = NAN, lon = NAN, altitude = NAN, headingDeg = NAN, pitchDeg = 0, bankDeg = 0;
    double groundSpeed = 0, verticalSpeed = 0, groundAltitude = NAN;
    bool onGround = false;
    std::string title, atcId, airline, flightNumber, model;
};
struct Traffic { std::vector<TrafficAircraft> aircraft; std::uint32_t userObjectId = 0; };
// Spotting: the watched target (IModuleServices::watchTrafficTarget), every simulator frame. Meters, degrees true,
// pitch/bank FRD as TrafficAircraft; velocity in world north, east, down m/s (SimConnect VELOCITY WORLD Z, X, -Y).
struct TrafficTarget {
    std::uint32_t objectId = 0;
    double lat = NAN, lon = NAN, altitude = NAN, headingDeg = NAN, pitchDeg = 0, bankDeg = 0;
    double velocityNorth = 0, velocityEast = 0, velocityDown = 0, groundAltitude = NAN;
    bool onGround = false;
};
struct Connected { std::string simVersion; };
struct Disconnected { std::string reason; };
struct CameraStatus { SdkAvailability state = SdkAvailability::NotAcquired; bool gameControlled = false; };
struct CameraData { SdkPose pose; };
struct DefinitionList { std::vector<std::string> names; };
struct AircraftGeometry { std::string aircraftPath; nlohmann::json geometry; };
struct AircraftLoaded { std::string aircraftPath; };
struct AircraftTitle { std::string title; };
struct FlightLoaded { std::string flightPath; };
struct SimRunning { bool running = false; };
struct Paused { bool paused = false; };
struct CameraException { std::uint32_t code = 0; std::string message; };
struct CommBusMessage { std::string data; };  // one CommBus payload from a panel (may be a chunk)
struct InputMappingStatus { bool ready = false; std::size_t definitions = 0; std::size_t failedCalls = 0; std::uint32_t code = 0; };
struct InputEvent { std::size_t index = 0; bool down = true; std::uint32_t value = 0; bool test = false; };
struct ControllersList { std::vector<std::pair<int, std::string>> controllers; };
struct ControllerButton { int deviceId = -1; int button = -1; bool down = false; std::uint64_t captureGeneration = 0; };
struct ControllerHat { int deviceId = -1; int hat = 0; std::uint32_t angle = 0xffffffffu; std::uint64_t captureGeneration = 0; };
// Gamepad sticks: a mapped axis (IModuleServices::mapAxes) reported its raw position (about -32K..32K).
struct AxisInput { std::size_t index = 0; std::int32_t value = 0; };
// Settings > Gamepad > Listen: one axis (kGamepadAxisNames index) of the controller being listened to.
struct ControllerAxis { int deviceId = -1; int axis = -1; std::int32_t value = 0; std::uint64_t captureGeneration = 0; };
struct ControllerCaptureMappingStatus {
    int deviceId = -1;
    std::uint64_t generation = 0;
    bool ready = false;
    std::string definition;
    std::string stage;
    std::uint32_t code = 0;
};
}  // namespace backend_event

using BackendEvent = std::variant<backend_event::Connected, backend_event::Disconnected, backend_event::CameraStatus,
                                  backend_event::CameraData, backend_event::DefinitionList, backend_event::AircraftLoaded,
                                  backend_event::AircraftGeometry, backend_event::AircraftTitle, backend_event::FlightLoaded, backend_event::SimRunning,
                                  backend_event::Paused, backend_event::CameraException,
                                  backend_event::CommBusMessage, backend_event::InputEvent, backend_event::InputMappingStatus, backend_event::ControllersList, backend_event::ControllerButton, backend_event::ControllerHat, backend_event::ControllerCaptureMappingStatus, backend_event::MotionTelemetry, backend_event::NativeMouseButton, backend_event::NativeMouseAxis, backend_event::NativeMouseMapping, backend_event::AircraftPosition, backend_event::AirportList, backend_event::AirportFacility, backend_event::AirportIdentity, backend_event::GroundElevation, backend_event::Traffic, backend_event::TrafficTarget, backend_event::AxisInput, backend_event::ControllerAxis>;

enum class CameraFlag { Interaction, AboveGround };

inline constexpr const char* kUiToHostEvent = "FSCS_UI_TO_HOST";
inline constexpr const char* kHostToUiEvent = "FSCS_HOST_TO_UI";

class ICameraBackend {
public:
    virtual ~ICameraBackend() = default;

    // Connection lifecycle. `tryConnect` is non-blocking; success is reported as a Connected event.
    virtual bool tryConnect() = 0;
    virtual void disconnect() = 0;
    virtual bool isOpen() const = 0;
    // Drains pending simulator messages, delivering them through `sink` on the calling thread.
    virtual void dispatch(const std::function<void(BackendEvent)>& sink) = 0;

    virtual bool requestStatus() = 0;
    virtual bool acquire(const std::string& clientId) = 0;
    // Empty definition: return to the previously used native camera.
    virtual bool release(const std::string& definitionName) = 0;
    virtual bool submitPose(const SdkPose& pose) = 0;
    virtual bool setFlag(CameraFlag flag, bool enabled) = 0;
    virtual bool requestCameraData(Referential ref) = 0;
    // Read-only current world position, separate from capture/seed replies.
    // MSFS may lift airport cameras above terrain or buildings after submission.
    virtual std::optional<core::Vec3> worldCameraPosition() { return std::nullopt; }
    // Airport cameras: the ground altitude (meters MSL) their height was measured from, NAN for any other camera. A
    // simulator that does not lift airport cameras over its terrain itself (X-Plane) raises a world camera by how much
    // the terrain under it is above this, so the height holds on sloping ground.
    virtual void setGroundBase(double baseAltitudeMeters) { (void)baseAltitudeMeters; }
    virtual bool enumerateDefinitions() = 0;
    virtual bool setUsingDefinition(const std::string& name) = 0;
    virtual std::optional<bool> headTracking() { return std::nullopt; }
    virtual bool setHeadTracking(bool) { return false; }

};

// Everything the module needs from the simulator besides the camera: panel messaging (CommBus), input mapping and
// controller enumeration through the in-module SimConnect session. Results arrive as BackendEvents.
class IModuleServices {
public:
    virtual ~IModuleServices() = default;
    virtual bool sendToPanels(const std::string& text) = 0;
    // Replaces the mapped input group: InputEvent.index refers to a position in `definitions`.
    virtual bool mapInputs(const std::vector<std::string>& definitions) = 0;
    // A separate group for the Bindings test dialog (InputEvent.test = true); empty string clears it.
    virtual bool mapTestInput(const std::string& definition) = 0;
    // Temporary selected-controller discovery; -1 clears the group. Buttons 0..31 and POV 0.
    // Generation tags reject delayed events from a previous listening request.
    virtual bool captureControllerInputs(int deviceId, std::uint64_t generation = 0) { return deviceId < 0; }
    // Gamepad sticks: replaces the mapped axis group (AxisInput.index refers to a position in `definitions`), left
    // off until setAxesEnabled(true). `mask` keeps the mapped axes from the simulator (the aircraft) while enabled.
    virtual bool mapAxes(const std::vector<std::string>& definitions, bool mask) { (void)mask; return definitions.empty(); }
    virtual void setAxesEnabled(bool enabled) { (void)enabled; }
    // Settings > Gamepad > Listen: every axis of one controller as ControllerAxis events; -1 clears the group.
    virtual bool captureControllerAxes(int deviceId, std::uint64_t generation = 0) { (void)generation; return deviceId < 0; }
    // Native mouse registration is lazy: never subscribe during startup/flight loading.
    virtual void setNativeMouseEnabled(bool enabled) = 0;
    virtual bool enumerateControllers() = 0;
    virtual bool requestNearbyAirports() { return false; }
    virtual bool requestAirport(const std::string&, std::uint32_t) { return false; }
    virtual void resolveAirportIdentities(const nlohmann::json&) {}
    // Airport, runways and parking only, without the taxi network: for automatic temporary airport sets.
    virtual bool requestAirportSummary(const std::string& code, std::uint32_t id) { return requestAirport(code, id); }
    // Spotting: one poll for aircraft within `radiusMeters` of the user aircraft (SimConnect caps it at 200 km). The
    // answer arrives as a Traffic event; re-issued by the host about every 1.5 s.
    virtual bool requestTraffic(double radiusMeters) { (void)radiusMeters; return false; }
    // Spotting: per-frame data for one aircraft (the spotting target), as TrafficTarget events; 0 stops it. A new id
    // replaces the previous one. Re-issued by the session after a reconnect.
    virtual bool watchTrafficTarget(std::uint32_t objectId) { (void)objectId; return false; }
};

}  // namespace fscs::host
