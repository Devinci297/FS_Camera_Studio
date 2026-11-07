#pragma once
// In-module SimConnect session (design 2.0 §5, §9.1): aircraft identity, flight/pause events, input mapping through
// SimConnect_MapInputEventToClientEvent_EX1 and controller enumeration — inside the simulator, no external process.
// Also sends CommBus messages to the panels. SimConnect stays asynchronous inside WASM [S34]: results are staged
// as BackendEvents and handed to the module on the next frame.

#include "backend.hpp"
#include "gamepad.hpp"

#include <string>
#include <vector>
#include <map>
#include <set>

namespace fscs::host {

class SimSession final : public IModuleServices {
public:
    bool open();
    void close();
    bool isOpen() const { return handle_ != 0; }
    // Drains events from the persistent WASM callback; does not register or poll SimConnect.
    void dispatch(std::vector<BackendEvent>& out);

    bool sendToPanels(const std::string& text) override;
    bool mapInputs(const std::vector<std::string>& definitions) override;
    bool mapTestInput(const std::string& definition) override;
    bool captureControllerInputs(int deviceId, std::uint64_t generation = 0) override;
    bool enumerateControllers() override;
    void setNativeMouseEnabled(bool enabled) override;
    bool mapAxes(const std::vector<std::string>& definitions, bool mask) override;
    void setAxesEnabled(bool enabled) override;
    bool captureControllerAxes(int deviceId, std::uint64_t generation = 0) override;

    bool requestNearbyAirports() override;
    bool requestAirport(const std::string&, std::uint32_t) override;
    void resolveAirportIdentities(const nlohmann::json&) override;
    bool requestAirportSummary(const std::string&, std::uint32_t) override;
    bool requestTraffic(double radiusMeters) override;
    bool watchTrafficTarget(std::uint32_t objectId) override;
    bool onAirportRecv(const void*, unsigned long);
    void onRecv(const void* data, unsigned long size);

private:
    void setup();
    bool ensureClientEvents(std::size_t count);
    bool ensureZoomEvents();
    void requestTitle();
    void setMouseAxesEnabled(bool enabled);

    bool airportDefinitionReady_=false;
    bool airportIdentityDefinitionReady_=false;
    bool airportSummaryDefinitionReady_=false;
    std::uint32_t airportIdentityRequest_=700000;
    std::map<std::uint32_t,nlohmann::json> airportIdentityRequests_;
    std::map<std::uint32_t,nlohmann::json> airportRequests_;
    unsigned long long handle_ = 0;  // SimConnect HANDLE (an integer handle under WASM)
    std::vector<BackendEvent> staged_;
    std::vector<std::string> mapped_;
    std::vector<bool> isPov_;
    std::size_t clientEventsDefined_ = 0;
    std::set<unsigned long> povReleaseEventsDefined_;
    std::vector<unsigned long> inputMappingPackets_;
    std::vector<unsigned long> mouseMappingPackets_;
    bool mouseRegistered_ = false;
    bool mouseEnabled_ = false;
    bool mouseAxesEnabled_ = false;
    bool mouseFailed_ = false;
    bool testMapped_ = false;
    bool testIsPov_ = false;
    bool zoomRegistered_ = false;
    int captureDevice_ = -1;
    unsigned long captureEventBase_ = 10000, nextCaptureEventBase_ = 10000;
    std::uint64_t captureGeneration_ = 0;
    struct CapturePacket { int input = -1; std::string stage; };
    std::map<unsigned long, CapturePacket> capturePackets_;
    std::vector<bool> captureInputsValid_;
    // Gamepad sticks: mapped axes (kAxisGroup, off until enabled) and Settings > Gamepad listening (kAxisCaptureGroup).
    std::vector<std::string> axesMapped_;
    std::size_t axisEventsDefined_ = 0;
    bool axesMask_ = false, axesEnabled_ = false;
    int axisCaptureDevice_ = -1;
    unsigned long axisCaptureBase_ = 0, nextAxisCaptureBase_ = 0;
    std::uint64_t axisCaptureGeneration_ = 0;
    std::vector<backend_event::TrafficAircraft> trafficPending_;  // the poll being received
    std::uint32_t userObjectId_ = 0;                               // from the user aircraft's own data requests
    unsigned long watchedTarget_ = 0;                              // spotting target with per-frame data (0: none)
};

}  // namespace fscs::host
