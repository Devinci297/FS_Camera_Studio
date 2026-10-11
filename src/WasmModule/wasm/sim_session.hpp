#pragma once
// In-module SimConnect session (design 2.0 §5, §9.1): aircraft identity, flight/pause events, input mapping through
// SimConnect_MapInputEventToClientEvent_EX1 and controller enumeration — inside the simulator, no external process.
// Also sends CommBus messages to the panels. SimConnect stays asynchronous inside WASM [S34]: results are staged
// as BackendEvents and handed to the module on the next frame.

#include "backend.hpp"

#include <string>
#include <vector>

namespace fscs::host {

class SimSession final : public IModuleServices {
public:
    bool open();
    void close();
    bool isOpen() const { return handle_ != 0; }
    // Pumps SimConnect; staged events are appended to `out`.
    void dispatch(std::vector<BackendEvent>& out);

    bool sendToPanels(const std::string& text) override;
    bool mapInputs(const std::vector<std::string>& definitions) override;
    bool mapTestInput(const std::string& definition) override;
    bool enumerateControllers() override;

    void onRecv(const void* data, unsigned long size);

private:
    void setup();
    bool ensureClientEvents(std::size_t count);

    unsigned long long handle_ = 0;  // SimConnect HANDLE (an integer handle under WASM)
    std::vector<BackendEvent> staged_;
    std::vector<std::string> mapped_;
    std::vector<bool> isPov_;
    std::size_t clientEventsDefined_ = 0;
    bool testMapped_ = false;
    bool testIsPov_ = false;
};

}  // namespace fscs::host
