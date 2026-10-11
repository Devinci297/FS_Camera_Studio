#pragma once
// ICameraBackend (design §15.1): the only seam through which the coordinator touches the simulator.
// SimConnectBackend implements it for MSFS 2024; FakeBackend (tests) delays, drops and reorders.

#include "adapter.hpp"

#include <cstdint>
#include <functional>
#include <string>
#include <variant>
#include <vector>

namespace fscs::host {

enum class SdkAvailability { NotAcquired, Acquired, AcquiredByOther, UserDisabled };

namespace backend_event {
struct Connected { std::string simVersion; };
struct Disconnected { std::string reason; };
struct CameraStatus { SdkAvailability state = SdkAvailability::NotAcquired; bool gameControlled = false; };
struct CameraData { SdkPose pose; };
struct DefinitionList { std::vector<std::string> names; };
struct AircraftLoaded { std::string aircraftPath; };
struct AircraftTitle { std::string title; };
struct FlightLoaded { std::string flightPath; };
struct SimRunning { bool running = false; };
struct Paused { bool paused = false; };
struct CameraException { std::uint32_t code = 0; std::string message; };
struct CommBusMessage { std::string data; };  // one CommBus payload from a panel (may be a chunk)
struct InputEvent { std::size_t index = 0; bool down = true; std::uint32_t value = 0; bool test = false; };
struct ControllersList { std::vector<std::pair<int, std::string>> controllers; };
}  // namespace backend_event

using BackendEvent = std::variant<backend_event::Connected, backend_event::Disconnected, backend_event::CameraStatus,
                                  backend_event::CameraData, backend_event::DefinitionList, backend_event::AircraftLoaded,
                                  backend_event::AircraftTitle, backend_event::FlightLoaded, backend_event::SimRunning,
                                  backend_event::Paused, backend_event::CameraException,
                                  backend_event::CommBusMessage, backend_event::InputEvent, backend_event::ControllersList>;

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
    virtual bool enumerateDefinitions() = 0;
    virtual bool setUsingDefinition(const std::string& name) = 0;

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
    virtual bool enumerateControllers() = 0;
};

}  // namespace fscs::host
