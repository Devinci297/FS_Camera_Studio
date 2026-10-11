#include "test.hpp"

#include "coordinator.hpp"
#include "fake_backend.hpp"
#include "input_router.hpp"

using namespace fscs::host;
using fscs::profiles::Action;
using fscs::profiles::makeStarterProfile;
using fscs::profiles::newUuid;
using fscs::profiles::toSnapshot;
using fscs::protocol::Command;
using fscs::protocol::json;
namespace be = backend_event;

namespace {

struct Rig {
    FakeBackend backend;
    std::vector<std::pair<std::string, json>> events;
    Qualification qual;
    Coordinator c{backend, [this](const std::string& n, json p) { events.emplace_back(n, std::move(p)); },
                  [this](const std::string&) { return qual; }};
    double t = 0;
    int req = 0;
    json profile;
    std::string pilot, pfd;

    Rig() {
        qual.adapterQualified = true;
        profile = makeStarterProfile("Test", "asobo-c172", "C172");
        pilot = profile["cameras"][0]["id"];
        json cam = profile["cameras"][0];
        cam["id"] = pfd = newUuid();
        cam["name"] = "PFD";
        cam["viewId"] = 2;
        cam["positionMetersFRD"] = {0.3, 0.1, 0.2};
        cam["transition"] = {{"kind", "smoothstep3"}, {"durationSeconds", 0.5}, {"delaySeconds", 0}};
        profile["cameras"].push_back(cam);
        profile["categories"][0]["cameraIds"].push_back(pfd);
    }
    void deliver(BackendEvent e) { c.onBackendEvent(e, t); }
    json send(const std::string& action, json payload = json::object(), std::optional<std::uint64_t> epoch = {}) {
        Command cmd;
        cmd.requestId = "t-" + std::to_string(++req);
        cmd.sessionEpoch = epoch.value_or(c.epoch());
        cmd.action = action;
        cmd.payload = std::move(payload);
        return c.handle(cmd, t);
    }
    void advance(double dt, int steps = 1) {
        for (int i = 0; i < steps; ++i) {
            t += dt;
            c.tick(t);
        }
    }
    void setProfile() {
        json copy = profile;
        CHECK(fscs::profiles::validateProfile(copy).ok());
        c.setProfile(toSnapshot(copy), t);
    }
    void connect() {
        c.tick(t);
        deliver(be::Connected{"1.8.16.0"});
        deliver(be::AircraftLoaded{"SimObjects\\Airplanes\\Asobo-C172\\presets\\classic\\aircraft.cfg"});
        deliver(be::CameraStatus{SdkAvailability::NotAcquired, false});
        setProfile();
    }
    void activate(const std::string& id) {
        CHECK_EQ(send("ActivateCamera", {{"cameraId", id}})["status"].get<std::string>(), std::string("Accepted"));
    }
    void grant() { deliver(be::CameraStatus{SdkAvailability::Acquired, false}); }
    bool emitted(const std::string& name) const {
        for (const auto& e : events)
            if (e.first == name) return true;
        return false;
    }
};

}  // namespace

TEST_CASE("coordinator: aircraft path gives a model key that survives presets and liveries") {
    const auto a = aircraftFromPath("SimObjects\\Airplanes\\Asobo-C172\\presets\\classic\\config\\aircraft.cfg");
    CHECK_EQ(a.modelKey, std::string("asobo-c172"));
    CHECK_EQ(a.variantKey, std::string("presets/classic/config"));
    CHECK_EQ(aircraftFromPath("SimObjects/Airplanes/FBW_A32NX/aircraft.cfg").modelKey, std::string("fbw_a32nx"));
}

TEST_CASE("coordinator: starts observing and never moves the view on its own") {
    Rig r;
    r.connect();
    r.advance(1.0 / 60, 120);
    CHECK(r.c.ownership() == Ownership::Observing);
    CHECK_EQ(r.backend.count("acquire"), std::size_t{0});
    CHECK_EQ(r.backend.count("submit"), std::size_t{0});
    CHECK(r.c.canActivate());
}

TEST_CASE("coordinator: activation acquires, waits for owned status, then writes") {
    Rig r;
    r.connect();
    r.activate(r.pilot);
    CHECK(r.c.ownership() == Ownership::Acquiring);
    r.advance(1.0 / 60, 5);
    CHECK_EQ(r.backend.count("submit"), std::size_t{0});
    r.grant();
    CHECK(r.c.ownership() == Ownership::Active);
    r.advance(1.0 / 60);
    CHECK_EQ(r.backend.count("submit"), std::size_t{1});
    CHECK_EQ(r.backend.count("flag:interaction:on"), std::size_t{1});
    r.advance(1.0 / 60, 10);
    CHECK_EQ(r.backend.count("submit"), std::size_t{1});  // unchanged static pose is not rewritten
}

TEST_CASE("coordinator: transition to a second camera is written over its duration") {
    Rig r;
    r.connect();
    r.activate(r.pilot);
    r.grant();
    r.advance(1.0 / 60);
    r.activate(r.pfd);
    r.advance(1.0 / 60, 40);
    const auto& last = r.backend.submitted.back();
    CHECK_NEAR(last.z, 0.3, 1e-9);
    CHECK_NEAR(last.x, 0.1, 1e-9);
    CHECK_NEAR(last.y, -0.2, 1e-9);
    CHECK(r.backend.count("submit") > 20);
}

TEST_CASE("coordinator: editing the active camera's draft previews it live") {
    Rig r;
    r.connect();
    r.activate(r.pilot);
    r.grant();
    r.advance(1.0 / 60);
    r.profile["cameras"][0]["positionMetersFRD"] = {0.05, 0.0, 0.0};
    r.setProfile();
    r.advance(1.0 / 60);
    CHECK_NEAR(r.backend.submitted.back().z, 0.05, 1e-9);
}

TEST_CASE("coordinator: removing the active view releases and requires a deliberate recall") {
    Rig r;
    r.connect();
    r.activate(r.pfd);
    r.grant();
    r.profile["cameras"].erase(1);
    r.profile["categories"][0]["cameraIds"].erase(1);
    r.setProfile();
    CHECK(r.c.ownership() == Ownership::Releasing);
}

TEST_CASE("coordinator: acquisition timeout withdraws the claim and ignores a late grant") {
    Rig r;
    r.connect();
    r.activate(r.pilot);
    r.advance(0.1, 25);
    CHECK(r.c.ownership() == Ownership::Observing);
    CHECK_EQ(r.backend.count("release"), std::size_t{1});
    r.grant();  // late
    r.advance(1.0 / 60, 10);
    CHECK_EQ(r.backend.count("submit"), std::size_t{0});
    CHECK_EQ(r.backend.count("release"), std::size_t{2});
}

TEST_CASE("coordinator: an unconfirmed release enters Faulted instead of resetting a connection") {
    Rig r;
    r.connect();
    r.activate(r.pilot);
    r.grant();
    r.send("ReleaseCamera");
    CHECK(r.c.ownership() == Ownership::Releasing);
    r.advance(0.1, 20);  // no status confirmation arrives
    CHECK(r.c.ownership() == Ownership::Faulted);
    CHECK_EQ(r.backend.count("disconnect"), std::size_t{0});
    CHECK(r.emitted("FaultRaised"));
    // Explicit retry after native recovery is allowed.
    r.deliver(be::CameraStatus{SdkAvailability::NotAcquired, false});
    r.activate(r.pilot);
    CHECK(r.c.ownership() == Ownership::Acquiring);
}

TEST_CASE("coordinator: another client taking the camera stops all writes immediately") {
    Rig r;
    r.connect();
    r.activate(r.pilot);
    r.grant();
    r.advance(1.0 / 60);
    r.activate(r.pfd);
    r.deliver(be::CameraStatus{SdkAvailability::AcquiredByOther, false});
    const auto before = r.backend.count("submit");
    r.advance(1.0 / 60, 30);
    CHECK_EQ(r.backend.count("submit"), before);
    CHECK(r.c.ownership() == Ownership::OtherClient);
    CHECK_EQ(r.backend.count("acquire"), std::size_t{1});
}

TEST_CASE("coordinator: stale-epoch motion commands are rejected, release always works") {
    Rig r;
    r.connect();
    const auto old = r.c.epoch();
    r.activate(r.pilot);
    r.grant();
    r.deliver(be::FlightLoaded{"x.flt"});
    CHECK(r.c.epoch() > old);
    CHECK_EQ(r.send("ActivateCamera", {{"cameraId", r.pfd}}, old)["reason"].get<std::string>(), std::string("staleEpoch"));
    CHECK_EQ(r.send("ReleaseCamera", json::object(), old)["status"].get<std::string>(), std::string("Completed"));
}

TEST_CASE("coordinator: aircraft change releases and requires a matching profile") {
    Rig r;
    r.connect();
    r.activate(r.pilot);
    r.grant();
    r.deliver(be::AircraftLoaded{"SimObjects\\Airplanes\\Other-Jet\\aircraft.cfg"});
    CHECK(r.c.ownership() == Ownership::Releasing);
    r.deliver(be::CameraStatus{SdkAvailability::NotAcquired, false});
    CHECK(r.c.ownership() == Ownership::Observing);
    CHECK(r.c.blockReason().find("No camera profile") != std::string::npos);
    r.setProfile();
    CHECK(r.c.blockReason().find("another aircraft") != std::string::npos);
}

TEST_CASE("coordinator: a livery change keeps the profile") {
    Rig r;
    r.connect();
    r.deliver(be::AircraftTitle{"Cessna 172 Red"});
    r.deliver(be::AircraftTitle{"Cessna 172 Blue"});
    CHECK(r.c.canActivate());
}

TEST_CASE("coordinator: unqualified adapter blocks activation unless testing is allowed") {
    Rig r;
    r.qual.adapterQualified = false;
    r.connect();
    CHECK(!r.c.canActivate());
    CHECK_EQ(r.send("ActivateCamera", {{"cameraId", r.pilot}})["status"].get<std::string>(), std::string("Rejected"));
    fscs::profiles::HostSettings s;
    s.allowUnqualifiedAdapter = true;
    r.c.setSettings(s, r.t);
    CHECK(r.c.canActivate());
}

TEST_CASE("coordinator: simulator takeover freezes motion and reseeds from a readback on return") {
    Rig r;
    r.connect();
    r.activate(r.pilot);
    r.grant();
    r.advance(1.0 / 60);
    r.activate(r.pfd);
    r.advance(1.0 / 60, 5);
    r.deliver(be::CameraStatus{SdkAvailability::Acquired, true});
    CHECK(r.c.ownership() == Ownership::SimulatorSuspended);
    const auto before = r.backend.count("submit");
    r.advance(1.0 / 60, 60);
    CHECK_EQ(r.backend.count("submit"), before);
    r.deliver(be::CameraStatus{SdkAvailability::Acquired, false});
    CHECK_EQ(r.backend.count("get"), std::size_t{1});
    SdkPose read;
    read.z = 1.0;
    r.deliver(be::CameraData{read});
    CHECK(r.c.ownership() == Ownership::Active);
    r.advance(1.0 / 60);
    CHECK_NEAR(r.backend.submitted.back().z, 1.0, 0.05);
    r.advance(1.0 / 60, 60);
    CHECK_NEAR(r.backend.submitted.back().z, 0.3, 1e-9);
}

TEST_CASE("coordinator: a capture that never answers quarantines reads without touching ownership") {
    Rig r;
    r.connect();
    r.send("CaptureView");
    r.advance(0.1, 15);
    CHECK(r.emitted("ViewCaptured"));
    CHECK(r.c.ownership() == Ownership::Observing);
    CHECK_EQ(r.backend.count("disconnect"), std::size_t{0});
    CHECK(!r.c.capabilitiesJson()["captureQualified"].get<bool>());
}

TEST_CASE("coordinator: peek returns to the previous camera; a direct selection cancels it") {
    Rig r;
    r.connect();
    r.activate(r.pilot);
    r.grant();
    CHECK_EQ(r.send("BeginPeek", {{"cameraId", r.pfd}, {"peekToken", 7}})["status"].get<std::string>(), std::string("Accepted"));
    CHECK_EQ(r.c.activeCameraId(), r.pfd);
    r.send("EndPeek", {{"peekToken", 7}});
    CHECK_EQ(r.c.activeCameraId(), r.pilot);
    r.send("BeginPeek", {{"cameraId", r.pfd}, {"peekToken", 8}});
    r.activate(r.pilot);
    r.activate(r.pfd);
    r.send("EndPeek", {{"peekToken", 8}});
    CHECK_EQ(r.c.activeCameraId(), r.pfd);
}

TEST_CASE("coordinator: peek from the simulator's own view releases on end") {
    Rig r;
    r.connect();
    r.send("BeginPeek", {{"cameraId", r.pfd}, {"peekToken", 3}});
    r.grant();
    CHECK(r.c.ownership() == Ownership::Active);
    r.send("EndPeek", {{"peekToken", 3}});
    CHECK(r.c.ownership() == Ownership::Releasing);
}

TEST_CASE("coordinator: view-ID bindings select through the open profile; toggles reach the panels") {
    Rig r;
    r.connect();
    r.c.onAction({Action::SelectView, 2, true, 1}, r.t);
    CHECK(r.c.ownership() == Ownership::Acquiring);
    r.grant();
    CHECK_EQ(r.c.activeCameraId(), r.pfd);
    r.c.onAction({Action::SelectView, 17, true, 2}, r.t);  // view 17 is valid but unassigned
    CHECK_EQ(r.c.activeCameraId(), r.pfd);
    r.c.onAction({Action::ToggleMini, 0, true, 3}, r.t);
    CHECK(r.emitted("SurfaceRequest"));
    r.c.onAction({Action::ReleaseCamera, 0, true, 4}, r.t);
    CHECK(r.c.ownership() == Ownership::Releasing);
}

TEST_CASE("coordinator: non-cockpit modes stay visible but cannot activate") {
    Rig r;
    r.profile["cameras"][1]["mode"] = "orbit";
    r.connect();
    CHECK_EQ(r.send("ActivateCamera", {{"cameraId", r.pfd}})["status"].get<std::string>(), std::string("Rejected"));
}

TEST_CASE("coordinator: bake temporary look keeps the visible pose and clears the offset") {
    Rig r;
    r.connect();
    r.activate(r.pilot);
    r.grant();
    fscs::core::ManualInput in;
    in.lookRight = 1;
    r.c.setManualInput(in);
    r.advance(1.0 / 60, 30);
    r.c.setManualInput({});
    r.advance(1.0 / 60, 30);
    const auto heading = r.backend.submitted.back().headingDeg;
    CHECK(heading > 5);
    CHECK_EQ(r.send("BakeTemporaryLook")["status"].get<std::string>(), std::string("Completed"));
    CHECK(!r.c.engine().look().active());
    CHECK(r.emitted("TemporaryLookBaked"));
    r.advance(1.0 / 60, 5);
    CHECK_NEAR(r.backend.submitted.back().headingDeg, heading, 1e-3);
}

// ---- Input router -------------------------------------------------------------------------------------------

namespace {
InputRouter routerWithDefaults() {
    InputRouter r;
    r.setBindings(fscs::profiles::parseBindings(fscs::profiles::defaultBindingsDocument()).bindings);
    return r;
}
std::size_t indexOf(const InputRouter& r, const std::string& def) {
    const auto& d = r.definitions();
    return static_cast<std::size_t>(std::find(d.begin(), d.end(), def) - d.begin());
}
}  // namespace

TEST_CASE("input: down/up edges pair, repeats are ignored") {
    auto r = routerWithDefaults();
    const auto i = indexOf(r, "VK_LCONTROL+VK_LMENU+2");
    auto e = r.onInput(i, true, 0);
    CHECK_EQ(e.size(), std::size_t{1});
    CHECK(e[0].action == Action::SelectView && e[0].viewId == 2 && e[0].pressed);
    CHECK(r.onInput(i, true, 0).empty());
    auto rel = r.onInput(i, false, 0);
    CHECK_EQ(rel.size(), std::size_t{1});
    CHECK_EQ(rel[0].holdId, e[0].holdId);
}

TEST_CASE("input: context loss releases holds and requires neutral before a new hold") {
    auto r = routerWithDefaults();
    const auto i = indexOf(r, "VK_LCONTROL+VK_LMENU+VK_LEFT");
    CHECK_EQ(r.onInput(i, true, 0).size(), std::size_t{1});
    CHECK(r.manualInput().lookRight < 0);
    CHECK_EQ(r.releaseAll().size(), std::size_t{1});
    CHECK(r.manualInput().lookRight == 0);
    CHECK(r.onInput(i, true, 0).empty());  // still held physically
    r.onInput(i, false, 0);
    CHECK_EQ(r.onInput(i, true, 0).size(), std::size_t{1});
}

TEST_CASE("input: text focus suppresses everything except Release Camera") {
    auto r = routerWithDefaults();
    r.setTextFocus(true);
    CHECK(r.onInput(indexOf(r, "VK_LCONTROL+VK_LMENU+1"), true, 0).empty());
    auto e = r.onInput(indexOf(r, "VK_LCONTROL+VK_LMENU+R"), true, 0);
    CHECK_EQ(e.size(), std::size_t{1});
    CHECK(e[0].action == Action::ReleaseCamera);
}

TEST_CASE("input: POV angles map to directions with diagonals and centring") {
    InputRouter r;
    auto pov = [](const char* action, const char* dir) {
        return json{{"action", action},
                    {"input", {{"kind", "joystick"}, {"definition", "joystick:1:POV:0"}, {"deviceId", 1}, {"pov", dir}}}};
    };
    json doc{{"schemaVersion", 2}, {"bindings", json::array({pov("nextCamera", "right"), pov("lookUp", "up")})}};
    auto set = fscs::profiles::parseBindings(doc);
    CHECK(set.problems.empty());
    r.setBindings(set.bindings);
    CHECK_EQ(r.definitions().size(), std::size_t{1});  // one mapping serves both directions
    auto e = r.onInput(0, true, 4500);                    // up-right
    CHECK_EQ(e.size(), std::size_t{2});
    CHECK(r.manualInput().lookUp > 0);
    e = r.onInput(0, true, 9000);  // right only: releases up
    CHECK_EQ(e.size(), std::size_t{1});
    CHECK(!e[0].pressed);
    e = r.onInput(0, true, 0xFFFF);  // centred
    CHECK_EQ(e.size(), std::size_t{1});
    CHECK(povDirections(0xFFFFFFFFu).empty());
    CHECK(povDirections(35000).count(fscs::profiles::PovDirection::Up) == 1);
}

TEST_CASE("coordinator: an aircraft title that arrives before the aircraft is kept (seen in-sim)") {
    Rig r;
    r.c.tick(r.t);
    r.deliver(be::Connected{"1.8.16.0"});
    r.deliver(be::AircraftTitle{"Cessna Skyhawk G1000 Asobo"});
    r.deliver(be::AircraftLoaded{"SimObjects/Airplanes/Asobo_C172sp_G1000/aircraft.cfg"});
    CHECK_EQ(r.c.aircraft().title, std::string("Cessna Skyhawk G1000 Asobo"));
    CHECK_EQ(r.c.aircraft().modelKey, std::string("asobo_c172sp_g1000"));
}
