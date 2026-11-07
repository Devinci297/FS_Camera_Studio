#pragma once
// XP Camera Studio: the module's non-camera seam for X-Plane 12 (the camera is xp_camera.hpp).
//  XPServices IModuleServices: panel messages go to the browser windows, airports come from apt.dat
//             (xp_airports.hpp) and spotting traffic from the TCAS targets (xp_traffic.hpp). Bindings are X-Plane
//             commands (plugin_main.cpp), so input mapping is accepted and does nothing here.
// It stages its answers as BackendEvents for the next flight-loop pass, as the MSFS adapters do, so the module
// never runs inside an X-Plane callback.

#include "backend.hpp"
#include "xp_airports.hpp"
#include "xp_traffic.hpp"

#include <cmath>
#include <functional>
#include <memory>
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
    std::function<void(const std::string&)> log;  // to X-Plane's Log.txt

    void stage(BackendEvent ev) { queue_.stage(std::move(ev)); }
    void dispatch(const std::function<void(BackendEvent)>& sink) {
        if (airports) {
            if (nearbyPending_ && airports->ready()) {
                nearbyPending_ = false;
                queue_.stage(host::backend_event::AirportList{airports->nearby(lat_, lon_, kNearbyRadiusNm, kNearbyLimit), true});
            }
            for (auto& r : airports->take()) {
                if (log && r.error.empty() && r.facility.is_object())
                    log("airport " + r.facility.value("icao", std::string()) + " from " + r.source);
                queue_.stage(host::backend_event::AirportFacility{r.requestId, std::move(r.facility), std::move(r.error)});
            }
        }
        queue_.drain(sink);
    }
    // The user aircraft, every frame: nearby airports are around it.
    void setPosition(double lat, double lon) {
        lat_ = lat;
        lon_ = lon;
    }

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
    // Mouse look (xp_mouse.hpp): the host turns its right-drag window on and off.
    std::function<void(bool)> onNativeMouse;
    void setNativeMouseEnabled(bool enabled) override {
        if (onNativeMouse) onNativeMouse(enabled);
    }
    bool enumerateControllers() override {
        queue_.stage(host::backend_event::ControllersList{});
        return true;
    }

    // Airports: the list waits for the index (built once, in the background, after the plugin starts).
    apt::Database* airports = nullptr;
    bool requestNearbyAirports() override {
        if (!airports || !std::isfinite(lat_)) return false;
        nearbyPending_ = true;
        return true;
    }
    bool requestAirport(const std::string& code, std::uint32_t id) override {
        if (!airports) return false;
        airports->request(code, id, false);
        return true;
    }
    bool requestAirportSummary(const std::string& code, std::uint32_t id) override {
        if (!airports) return false;
        airports->request(code, id, true);
        return true;
    }
    // Every entry carries its facilityIdent already: nothing to resolve.
    void resolveAirportIdentities(const nlohmann::json&) override {}

    // Spotting.
    TrafficSource* traffic = nullptr;
    bool requestTraffic(double radiusMeters) override { return traffic && traffic->poll(radiusMeters); }
    bool watchTrafficTarget(std::uint32_t objectId) override { return traffic && traffic->watch(objectId); }

private:
    static constexpr double kNearbyRadiusNm = 60;
    static constexpr std::size_t kNearbyLimit = 200;
    EventQueue queue_;
    bool nearbyPending_ = false;
    double lat_ = NAN, lon_ = NAN;
};

}  // namespace fscs::xplane
