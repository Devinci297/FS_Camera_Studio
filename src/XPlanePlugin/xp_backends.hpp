#pragma once
// XP Camera Studio: the module's simulator seams for X-Plane 12 (phase 1).
//  XPCamera   ICameraBackend. Phase 1 stays in Observing: it never acquires or writes the camera, so the coordinator
//             reports "The simulator did not accept the camera request." if a view is activated. Phase 2 drives
//             XPLMControlCamera and the pilot-head datarefs here.
//  XPServices IModuleServices: panel messages go to the browser windows; inputs, controllers, airports and traffic
//             are phase 2-4 (X-Plane commands, TCAS targets, apt.dat).
// Both stage their answers as BackendEvents for the next flight-loop pass, as the MSFS adapters do, so the module
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

class XPCamera final : public host::ICameraBackend {
public:
    bool tryConnect() override { open_ = true; return true; }
    void disconnect() override { open_ = false; }
    bool isOpen() const override { return open_; }
    void dispatch(const std::function<void(BackendEvent)>& sink) override { queue_.drain(sink); }

    bool requestStatus() override {
        queue_.stage(host::backend_event::CameraStatus{host::SdkAvailability::NotAcquired, false});
        return open_;
    }
    bool acquire(const std::string&) override { return false; }  // phase 2
    bool release(const std::string&) override { return open_; }
    bool submitPose(const host::SdkPose&) override { return false; }
    bool setFlag(host::CameraFlag, bool) override { return false; }
    bool requestCameraData(host::Referential) override { return false; }
    bool enumerateDefinitions() override {
        queue_.stage(host::backend_event::DefinitionList{});
        return open_;
    }
    bool setUsingDefinition(const std::string&) override { return false; }

private:
    bool open_ = false;
    EventQueue queue_;
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
