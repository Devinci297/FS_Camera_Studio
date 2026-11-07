// Gamepad sticks for drones: settings normalization, axis shaping, Settings > Gamepad listening, and the module mapping
// the axes only while a drone is shown and flying it with them.
#include "test.hpp"

#include "fake_backend.hpp"
#include "gamepad.hpp"
#include "module_app.hpp"
#include "fscs/profiles/airport.hpp"
#include "fscs/profiles/bindings.hpp"
#include "fscs/profiles/json_util.hpp"

#include <cmath>

using namespace fscs::host;
using fscs::profiles::GamepadAxis;
using fscs::profiles::GamepadFunction;
using fscs::profiles::newUuid;
using fscs::protocol::json;
namespace core = fscs::core;
namespace geo = fscs::profiles::airport;
namespace js = fscs::js;
namespace be = backend_event;
namespace profiles = fscs::profiles;

namespace {

GamepadAxis axis(GamepadFunction f, const char* definition, double rest, double full) {
    GamepadAxis a;
    a.function = f;
    a.definition = definition;
    a.rest = rest;
    a.full = full;
    return a;
}

bool near(double a, double b, double eps = 1e-9) { return std::abs(a - b) <= eps; }

// The left stick, the right stick and both triggers on one combined Z axis, as a gamepad usually reports them.
std::vector<GamepadAxis> gamepadLayout() {
    return {axis(GamepadFunction::Forward, "joystick:3:YAxis", 0, -32767), axis(GamepadFunction::Right, "joystick:3:XAxis", 0, 32767),
            axis(GamepadFunction::Climb, "joystick:3:ZAxis", 0, -32767), axis(GamepadFunction::Descend, "joystick:3:ZAxis", 0, 32767),
            axis(GamepadFunction::Yaw, "joystick:3:RxAxis", 0, 32767), axis(GamepadFunction::Pitch, "joystick:3:RyAxis", 0, -32767)};
}

json layoutJson() {
    json out = json::array();
    for (const auto& a : gamepadLayout())
        out.push_back({{"function", profiles::toString(a.function)}, {"definition", a.definition}, {"label", a.definition}, {"rest", a.rest}, {"full", a.full}});
    return out;
}

}  // namespace

TEST_CASE("gamepad: settings keep one valid axis per motion and clamp the rest") {
    const json doc{{"host", {{"gamepadDeadzone", 0.9}, {"gamepadBlocksAircraft", false},
        {"gamepadAxes", json::array({
            {{"function", "forward"}, {"definition", "joystick:1:YAxis"}, {"rest", 0}, {"full", -40000}, {"label", "Pad - Y axis"}},
            {{"function", "forward"}, {"definition", "joystick:1:XAxis"}, {"rest", 0}, {"full", 32767}},   // second forward: dropped
            {{"function", "right"}, {"definition", "joystick:1:button:3"}, {"rest", 0}, {"full", 32767}},  // not an axis
            {{"function", "roll"}, {"definition", "joystick:1:ZAxis"}, {"rest", 0}, {"full", 32767}},      // unknown motion
            {{"function", "yaw"}, {"definition", "joystick:1:RxAxis"}, {"rest", 100}, {"full", 900}},      // too small a span
            {{"function", "climb"}, {"definition", "joystick:1:Slider"}, {"rest", -32768}, {"full", 32767}},
            {{"function", "pitch"}, {"definition", "joystick:1:RyAxis"}, {"full", -32767}}})}}}};
    const json out = profiles::normalizeSettings(doc);
    const auto& host = out["host"];
    CHECK(near(host["gamepadDeadzone"].get<double>(), profiles::kGamepadMaxDeadzone));
    CHECK_EQ(host["gamepadBlocksAircraft"].get<bool>(), false);
    CHECK_EQ(host["gamepadSticksEnabled"].get<bool>(), true);
    const auto& axes = host["gamepadAxes"];
    CHECK_EQ(axes.size(), std::size_t(3));
    CHECK_EQ(js::str(axes[0], "function"), std::string("forward"));
    CHECK_EQ(js::str(axes[0], "definition"), std::string("joystick:1:YAxis"));
    CHECK(near(axes[0]["full"].get<double>(), -profiles::kGamepadAxisLimit));
    CHECK_EQ(js::str(axes[0], "label"), std::string("Pad - Y axis"));
    CHECK_EQ(js::str(axes[1], "function"), std::string("climb"));
    CHECK_EQ(js::str(axes[2], "function"), std::string("pitch"));
    CHECK(near(axes[2]["rest"].get<double>(), 0));  // missing rest: centred
    CHECK_EQ(js::str(axes[2], "label"), std::string("joystick:1:RyAxis"));
    const json defaults = profiles::defaultSettingsDocument();
    CHECK(defaults["host"]["gamepadAxes"].is_array() && defaults["host"]["gamepadAxes"].empty());
    CHECK(near(profiles::parseHostSettings(defaults).gamepadDeadzone, 0.15));
    CHECK(profiles::validAxisDefinition("joystick:0:XAxis"));
    CHECK(profiles::validAxisDefinition("joystick:12:Slider"));
    CHECK(!profiles::validAxisDefinition("joystick::XAxis"));
    CHECK(!profiles::validAxisDefinition("joystick:0:POV:0"));
    CHECK(!profiles::validAxisDefinition("mouse:0:XAxis"));
}

TEST_CASE("gamepad: sticks are bipolar, triggers 0..1, with a dead zone and a fine response near the centre") {
    GamepadSticks g;
    const auto& defs = g.configure(gamepadLayout(), 0.1);
    CHECK_EQ(defs.size(), std::size_t(5));  // both triggers share the combined Z axis
    CHECK(!g.active());
    const auto index = [&](const char* d) { return static_cast<std::size_t>(std::find(defs.begin(), defs.end(), d) - defs.begin()); };
    // Left stick pushed fully forward (Y negative) and a little right, inside the dead zone.
    g.onAxis(index("joystick:3:YAxis"), -32767);
    g.onAxis(index("joystick:3:XAxis"), 2000);
    auto m = g.input();
    CHECK(near(m.moveForward, 1));
    CHECK(near(m.moveRight, 0));
    // Pulled back halfway: the response curve is gentler than linear but keeps the direction.
    g.onAxis(index("joystick:3:YAxis"), 16384);
    m = g.input();
    CHECK(m.moveForward < 0 && m.moveForward > -0.5);
    // The right trigger alone (combined Z negative) climbs; the left trigger alone descends.
    g.onAxis(index("joystick:3:ZAxis"), -32767);
    CHECK(near(g.input().moveUp, 1));
    g.onAxis(index("joystick:3:ZAxis"), 32767);
    CHECK(near(g.input().moveUp, -1));
    // Right stick: look right and up.
    g.onAxis(index("joystick:3:RxAxis"), 32767);
    g.onAxis(index("joystick:3:RyAxis"), -32767);
    m = g.input();
    CHECK(near(m.lookRight, 1) && near(m.lookUp, 1));
    g.neutral();
    CHECK(!g.active());

    // Separate triggers resting at one end (-32768) and travelling to the other: 0..1 each, climb less descend.
    GamepadSticks t;
    const auto& split = t.configure({axis(GamepadFunction::Climb, "joystick:2:Slider", -32768, 32767),
                                     axis(GamepadFunction::Descend, "joystick:2:RzAxis", -32768, 32767)}, 0.05);
    t.onAxis(0, -32768);
    t.onAxis(1, -32768);
    CHECK(near(t.input().moveUp, 0));
    t.onAxis(0, 32767);
    CHECK(near(t.input().moveUp, 1));
    t.onAxis(1, 32767);
    CHECK(near(t.input().moveUp, 0));  // both pressed cancel
    CHECK_EQ(split.size(), std::size_t(2));
    // Reconfiguring with the same axes keeps the held samples (an unrelated settings edit); new axes forget them.
    t.configure({axis(GamepadFunction::Climb, "joystick:2:Slider", -32768, 32767),
                 axis(GamepadFunction::Descend, "joystick:2:RzAxis", -32768, 32767)}, 0.2);
    CHECK(t.active() == false);  // climb and descend still cancel, but both samples are kept
    t.onAxis(1, -32768);
    CHECK(near(t.input().moveUp, 1));
    t.configure({axis(GamepadFunction::Climb, "joystick:2:Slider", -32768, 32767)}, 0.2);
    CHECK(!t.active());

    CHECK(near(GamepadSticks::shape(0.05, 0.1), 0));
    CHECK(near(GamepadSticks::shape(-1, 0.1), -1));
    CHECK(near(GamepadSticks::shape(2, 0), 1));
    CHECK(GamepadSticks::shape(0.3, 0.1) > 0 && GamepadSticks::shape(0.3, 0.1) < 0.2);
    CHECK(near(GamepadSticks::shape(NAN, 0.1), 0));
}

TEST_CASE("gamepad: listening takes the first axis moved well away from where it rested") {
    AxisListener l;
    l.start();
    CHECK(!l.onSample(1, 120));      // Y axis rests near the centre
    CHECK(!l.onSample(2, -32768));   // a trigger resting at one end
    CHECK(!l.onSample(1, -9000));    // a small push is not enough
    CHECK(!l.onSample(9, 0));        // not an axis
    auto r = l.onSample(1, -30000);  // pushed forward
    CHECK(r.has_value());
    CHECK_EQ(r->axis, 1);
    CHECK(near(r->rest, 120));
    CHECK(near(r->full, -32767));
    r = l.onSample(2, 20000);        // the trigger pressed: travels to the far end
    CHECK(r.has_value());
    CHECK(near(r->rest, -32768) && near(r->full, 32767));
    l.start();
    CHECK(!l.onSample(1, -30000));   // a new listen: the first sample is the rest again
}

namespace {

struct PadRig {
    FakeBackend camera;
    FakeServices services;
    FakeStorage storage;
    ModuleApp app{camera, services, storage, "boot-0123456789", "0.1.0-test"};
    double t = 0;
    int req = 0;
    std::size_t seen = 0;
    std::vector<json> received;
    std::string drone, exterior;
    PadRig() { app.start(t); }
    std::vector<json> drain() {
        fscs::protocol::Reassembler r;
        std::vector<json> out;
        for (; seen < services.sent.size(); ++seen)
            if (auto whole = r.feed(services.sent[seen], t)) out.push_back(js::parse(*whole));
        received.insert(received.end(), out.begin(), out.end());
        return out;
    }
    json ack(const std::string& action, json payload = json::object()) {
        const std::string id = "main-" + std::to_string(++req);
        json msg{{"protocolVersion", 2}, {"requestId", id}, {"uiInstanceId", "main-1"}, {"moduleBootId", app.bootId()},
                 {"sessionEpoch", app.coordinator().epoch()}, {"draftRevision", app.draftRevision()}, {"action", action}, {"payload", payload}};
        app.onEvent(be::CommBusMessage{msg.dump()}, t);
        for (auto& m : drain())
            if (js::str(m, "type") == "ack" && js::str(m, "requestId") == id) return m;
        return nullptr;
    }
    json lastEvent(const std::string& name) {
        drain();
        for (auto it = received.rbegin(); it != received.rend(); ++it)
            if (js::str(*it, "type") == "event" && js::str(*it, "name") == name) return *it;
        return nullptr;
    }
    void frame(int n = 1) {
        for (int i = 0; i < n; ++i) {
            t += 1.0 / 60;
            app.onEvent(be::AircraftPosition{47, 8, 500, 0}, t);
            app.update(t);
        }
    }
    geo::Geo shown() const { return {camera.submitted.back().x, camera.submitted.back().y, camera.submitted.back().z}; }
    void setUp(const json& host) {
        frame();
        app.onEvent(be::Connected{"1.8.16.0"}, t);
        app.onEvent(be::AircraftLoaded{"SimObjects\\Airplanes\\Asobo-C152\\aircraft.cfg"}, t);
        app.onEvent(be::CameraStatus{SdkAvailability::NotAcquired, false}, t);
        frame();
        ack("Hello", {{"surface", "main"}});
        ack("CreateProfile", {{"name", "Drones"}, {"forAircraft", true}});
        frame(3);
        const auto here = ack("DroneHere");
        CHECK_EQ(js::str(here, "status"), std::string("Completed"));
        const auto& placed = here["result"];
        json profile = app.workingProfile();
        exterior = profile["cameras"][0]["id"];
        json d = profile["cameras"][0];
        d["id"] = drone = newUuid();
        d["name"] = "Drone 1";
        d["mode"] = "free";
        d["viewId"] = 0;
        d["worldOrigin"] = placed["worldOrigin"];
        d["positionMetersFRD"] = placed["positionMetersFRD"];
        d["orientationCameraToAnchorXYZW"] = placed["orientationCameraToAnchorXYZW"];
        d["followAircraft"] = false;
        auto categories = profile["categories"];
        categories[0]["cameraIds"].push_back(drone);
        json settings = profiles::defaultSettingsDocument();
        settings["host"].update(host);
        CHECK_EQ(js::str(ack("UpdateDraft", {{"ops", {{{"op", "setCamera"}, {"camera", d}}, {{"op", "setCategories"}, {"categories", categories}},
                                                     {{"op", "setSettings"}, {"settings", settings}}}}}), "status"),
                 std::string("Completed"));
    }
    void show(const std::string& id) {
        CHECK_EQ(js::str(ack("ActivateCamera", {{"cameraId", id}}), "status"), std::string("Accepted"));
        app.onEvent(be::CameraStatus{SdkAvailability::Acquired, false}, t);
        frame(40);
        CHECK_EQ(app.coordinator().activeCameraId(), id);
    }
    std::size_t axisIndex(const std::string& definition) const {
        return static_cast<std::size_t>(std::find(services.axes.begin(), services.axes.end(), definition) - services.axes.begin());
    }
};

}  // namespace

TEST_CASE("gamepad: the module maps the axes once, turns them on only while a drone is shown, and the sticks fly it") {
    PadRig m;
    m.setUp({{"gamepadAxes", layoutJson()}});
    CHECK_EQ(m.services.axes.size(), std::size_t(5));
    CHECK(m.services.axesMask);  // masked from the aircraft by default
    const int maps = m.services.axisMaps;
    CHECK(!m.services.axesEnabled);
    // An exterior view: the sticks stay the simulator's.
    m.show(m.exterior);
    CHECK(!m.services.axesEnabled);
    m.show(m.drone);
    CHECK(m.services.axesEnabled);
    const auto start = m.shown();
    // Left stick fully forward for two seconds, sampled about six times a second as SimConnect reports it.
    for (int i = 0; i < 120; ++i) {
        if (i % 10 == 0) m.app.onEvent(be::AxisInput{m.axisIndex("joystick:3:YAxis"), -32767}, m.t);
        m.frame();
    }
    const auto moved = m.shown();
    CHECK(geo::local(start, moved).length() > 0.5);
    // Released: the drone comes to rest and stays there.
    m.app.onEvent(be::AxisInput{m.axisIndex("joystick:3:YAxis"), 0}, m.t);
    m.frame(240);
    const auto rested = m.shown();
    m.frame(60);
    CHECK(geo::local(rested, m.shown()).length() < 1e-3);
    // Leaving the drone turns the axes off and forgets the samples: a stick held meanwhile does not fly the next drone.
    m.app.onEvent(be::AxisInput{m.axisIndex("joystick:3:YAxis"), -32767}, m.t);
    m.show(m.exterior);
    CHECK(!m.services.axesEnabled);
    const auto exteriorPose = m.shown();
    m.app.onEvent(be::AxisInput{m.axisIndex("joystick:3:YAxis"), -32767}, m.t);  // ignored while off
    m.frame(60);
    CHECK(geo::local(exteriorPose, m.shown()).length() < 1e-6);
    // Settings edits that do not touch the gamepad do not remap it.
    json settings = profiles::defaultSettingsDocument();
    settings["host"]["gamepadAxes"] = layoutJson();
    settings["host"]["lookSpeedDegPerSec"] = 90;
    CHECK_EQ(js::str(m.ack("UpdateDraft", {{"ops", {{{"op", "setSettings"}, {"settings", settings}}}}}), "status"), std::string("Completed"));
    CHECK_EQ(m.services.axisMaps, maps);
    // Turning the mask off remaps without it; turning the sticks off keeps the drone's axes off.
    settings["host"]["gamepadBlocksAircraft"] = false;
    m.ack("UpdateDraft", {{"ops", {{{"op", "setSettings"}, {"settings", settings}}}}});
    CHECK_EQ(m.services.axisMaps, maps + 1);
    CHECK(!m.services.axesMask);
    settings["host"]["gamepadSticksEnabled"] = false;
    m.ack("UpdateDraft", {{"ops", {{{"op", "setSettings"}, {"settings", settings}}}}});
    m.show(m.drone);
    CHECK(!m.services.axesEnabled);
}

TEST_CASE("gamepad: no axes assigned maps nothing and never turns the group on") {
    PadRig m;
    m.setUp(json::object());
    CHECK(m.services.axes.empty());
    m.show(m.drone);
    CHECK(!m.services.axesEnabled);
    CHECK(m.services.axisEnableChanges.empty());
}

TEST_CASE("gamepad: Listen reports the moved axis with its rest and full values, then stops") {
    PadRig m;
    m.setUp({{"gamepadAxes", layoutJson()}});
    m.show(m.drone);
    CHECK(m.services.axesEnabled);
    CHECK_EQ(js::str(m.ack("StartAxisCapture", {{"deviceId", 3}, {"function", "forward"}, {"captureId", "c1"}}), "status"), std::string("Rejected"));
    m.app.onEvent(be::ControllersList{{{3, "Xbox Controller"}}}, m.t);
    CHECK_EQ(js::str(m.ack("StartAxisCapture", {{"deviceId", 3}, {"function", "sideways"}, {"captureId", "c1"}}), "status"), std::string("Rejected"));
    CHECK_EQ(js::str(m.ack("StartAxisCapture", {{"deviceId", 3}, {"function", "climb"}, {"captureId", "c1"}}), "status"), std::string("Accepted"));
    CHECK_EQ(m.services.axisCaptureDevice, 3);
    m.frame();
    CHECK(!m.services.axesEnabled);  // listening: the sticks do not fly the drone meanwhile
    const auto generation = m.services.axisCaptureGeneration;
    m.app.onEvent(be::ControllerAxis{3, 2, 0, generation - 1}, m.t);  // an earlier request's sample: ignored
    m.app.onEvent(be::ControllerAxis{3, 2, 30000, generation - 1}, m.t);
    CHECK(m.lastEvent("ControllerAxisCaptured").is_null());
    m.app.onEvent(be::ControllerAxis{3, 2, 0, generation}, m.t);
    m.app.onEvent(be::ControllerAxis{3, 0, 0, generation}, m.t);
    m.app.onEvent(be::ControllerAxis{3, 2, -31000, generation}, m.t);  // right trigger on the combined Z axis
    const auto captured = m.lastEvent("ControllerAxisCaptured");
    CHECK(!captured.is_null());
    const auto& a = captured["payload"]["axis"];
    CHECK_EQ(js::str(captured["payload"], "captureId"), std::string("c1"));
    CHECK_EQ(js::str(a, "function"), std::string("climb"));
    CHECK_EQ(js::str(a, "definition"), std::string("joystick:3:ZAxis"));
    CHECK_EQ(js::str(a, "label"), std::string("Xbox Controller - Z axis"));
    CHECK(near(a["rest"].get<double>(), 0) && near(a["full"].get<double>(), -32767));
    CHECK_EQ(m.services.axisCaptureDevice, -1);
    CHECK_EQ(js::str(m.lastEvent("AxisCaptureStatus")["payload"], "state"), std::string("stopped"));
    m.frame();
    CHECK(m.services.axesEnabled);  // back on the drone
    // A listen that hears nothing times out.
    m.ack("StartAxisCapture", {{"deviceId", 3}, {"function", "yaw"}, {"captureId", "c2"}});
    for (int i = 0; i < 16 * 60; ++i) {
        if (i % 60 == 0) m.ack("Hello", {{"surface", "main"}});  // the panel's lease stays fresh
        m.frame();
    }
    CHECK_EQ(m.services.axisCaptureDevice, -1);
    CHECK_EQ(js::str(m.lastEvent("AxisCaptureStatus")["payload"], "state"), std::string("timeout"));
}
