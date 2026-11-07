#pragma once
// XP Camera Studio: the module's non-camera seam for X-Plane 12 (the camera is xp_camera.hpp).
//  XPServices IModuleServices: panel messages go to the browser windows; inputs, controllers, airports and traffic
//             are phase 2-4 (X-Plane commands, TCAS targets, apt.dat).
// It stages its answers as BackendEvents for the next flight-loop pass, as the MSFS adapters do, so the module
// never runs inside an X-Plane callback.

#include "backend.hpp"

#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace fscs::xplane {

using host::BackendEvent;

class EventQueue {
public:
    void stage(BackendEvent ev) { staged_.push_back(std::move(ev)); }
    void drain(const std::function<void(BackendEvent)>& sink) {
        auto events = std::move(staged_);
        staged_.clear();
        for (auto& ev : events) sink(std::move(ev));
    }

private:
    std::vector<BackendEvent> staged_;
};

class XPServices final : public host::IModuleServices {
public:
    std::function<void(const std::string&)> toPanels;

    void stage(BackendEvent ev) { queue_.stage(std::move(ev)); }
    void dispatch(const std::function<void(BackendEvent)>& sink) { queue_.drain(sink); }

    bool sendToPanels(const std::string& text) override {
        if (toPanels) toPanels(text);
        return true;
    }
    // Phase 2 turns bindings into X-Plane commands; until then the mapping is accepted and does nothing.
    bool mapInputs(const std::vector<std::string>& definitions) override {
        queue_.stage(host::backend_event::InputMappingStatus{true, definitions.size(), 0, 0});
        return true;
    }
    bool mapTestInput(const std::string&) override { return true; }
    void setNativeMouseEnabled(bool) override {}
    bool enumerateControllers() override {
        queue_.stage(host::backend_event::ControllersList{});
        return true;
    }

private:
    EventQueue queue_;
};

}  // namespace fscs::xplane
