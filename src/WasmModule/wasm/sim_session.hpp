#pragma once
// In-module SimConnect session (design 2.0 §5, §9.1): aircraft identity, flight/pause events, input mapping through
// SimConnect_MapInputEventToClientEvent_EX1 and controller enumeration — inside the simulator, no external process.
// Also sends CommBus messages to the panels. SimConnect stays asynchronous inside WASM [S34]: results are staged
// as BackendEvents and handed to the module on the next frame.

#include "backend.hpp"

#include <string>
#include <vector>
#include <map>

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
    bool enumerateControllers() override;
    void setNativeMouseEnabled(bool enabled) override;

    bool requestNearbyAirports() override;
    bool requestAirport(const std::string&, std::uint32_t) override;
    void resolveAirportIdentities(const nlohmann::json&) override;
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
    std::uint32_t airportIdentityRequest_=700000;
    std::map<std::uint32_t,nlohmann::json> airportIdentityRequests_;
    std::map<std::uint32_t,nlohmann::json> airportRequests_;
    unsigned long long handle_ = 0;  // SimConnect HANDLE (an integer handle under WASM)
    std::vector<BackendEvent> staged_;
    std::vector<std::string> mapped_;
    std::vector<bool> isPov_;
    std::size_t clientEventsDefined_ = 0;
    std::vector<unsigned long> mouseMappingPackets_;
    bool mouseRegistered_ = false;
    bool mouseEnabled_ = false;
    bool mouseAxesEnabled_ = false;
    bool mouseFailed_ = false;
    bool testMapped_ = false;
    bool testIsPov_ = false;
    bool zoomRegistered_ = false;
};

}  // namespace fscs::host
