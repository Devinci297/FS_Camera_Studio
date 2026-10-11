#include "test.hpp"

#include "coordinator.hpp"
#include "fake_backend.hpp"
#include "input_router.hpp"
#include "airport_fixture.hpp"

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
        const auto validation=fscs::profiles::validateProfile(copy); if(!validation.ok())std::printf("Validation: %s\n",validation.summary().c_str()); CHECK(validation.ok());
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

TEST_CASE("input: visible movement defaults, boost and zoom pulses can be removed") {
    auto r = routerWithDefaults();
    for (const auto* def : {"W", "S", "A", "D", "Q", "E", "VK_SHIFT"}) {
        const auto i = indexOf(r, def);
        CHECK(i < r.definitions().size());
        CHECK_EQ(r.onInput(i, true, 1).size(), std::size_t{1});
        if (std::string(def) == "VK_SHIFT") CHECK(r.manualInput().fast);
        if (std::string(def) == "Q") CHECK(r.manualInput().moveUp < 0);
        if (std::string(def) == "E") CHECK(r.manualInput().moveUp > 0);
        CHECK_EQ(r.onInput(i, false, 0).size(), std::size_t{1});
    }
    const auto zoom = indexOf(r, "sim:ZOOM_IN");
    for (int i = 0; i < 3; ++i) {
        const auto ev = r.onInput(zoom, true, 1);
        CHECK_EQ(ev.size(), std::size_t{1});
        if (!ev.empty()) CHECK(ev.front().pulse);
    }
    CHECK_NEAR(r.manualInput().zoomIn, 0, 1e-9); // No held state, even without key-up.
    r.setTextFocus(true); CHECK(r.onInput(zoom, true, 1).empty());
    r.setTextFocus(false); r.setBindings({});
    CHECK(r.onInput(zoom, true, 1).empty());
    CHECK(r.definitions().empty());
}

TEST_CASE("coordinator: precision scales keyboard translation and boost independently of look") {
    auto run = [](const char* precision, bool fast) {
        Rig r; r.connect(); r.activate(r.pilot); r.grant(); r.advance(.01);
        auto settings = fscs::profiles::defaultSettingsDocument(); settings["ui"]["precision"] = precision;
        r.c.setSettings(fscs::profiles::parseHostSettings(settings), r.t);
        fscs::core::ManualInput in; in.moveUp = 1; in.fast = fast; in.lookRight = 1;
        r.c.setManualInput(in); r.advance(1.0/60, 120);
        return r.c.engine().look();
    };
    const auto normal = run("normal", false), fine = run("fine", false), coarse = run("coarse", false), fast = run("coarse", true);
    CHECK(normal.translationHeadingFrame.z < -.5);
    CHECK_NEAR(fine.translationHeadingFrame.z, normal.translationHeadingFrame.z * .1, 1e-8);
    CHECK_NEAR(coarse.translationHeadingFrame.z, normal.translationHeadingFrame.z * 10, 1e-8);
    CHECK_NEAR(fast.translationHeadingFrame.z, coarse.translationHeadingFrame.z * 4, 1e-8);
    CHECK_NEAR(coarse.headingDeg, normal.headingDeg, 1e-8);
}

TEST_CASE("coordinator: wheel zoom clamps without windup and ignores blocked or released input") {
    Rig r; r.connect(); r.activate(r.pilot); r.grant(); r.advance(.02);
    const auto saved = r.c.engine().sample().pose.fovRad;
    const ActionEvent in{Action::ZoomIn,0,true,0,true}, out{Action::ZoomOut,0,true,0,true};
    r.c.onAction(in, r.t);
    CHECK_NEAR(r.c.engine().sample().pose.fovRad, saved-2*fscs::core::kDegToRad, 1e-9);
    r.c.setMouseInputBlocked(true); r.c.onAction(in,r.t);
    CHECK_NEAR(r.c.engine().sample().pose.fovRad, saved-2*fscs::core::kDegToRad, 1e-9);
    r.c.setMouseInputBlocked(false);
    for(int i=0;i<200;++i) r.c.onAction(in,r.t);
    CHECK_NEAR(r.c.engine().sample().pose.fovRad, fscs::core::kMinFovRad, 1e-9);
    r.c.onAction(out,r.t);
    CHECK_NEAR(r.c.engine().sample().pose.fovRad, fscs::core::kMinFovRad+2*fscs::core::kDegToRad, 1e-9);
    for(int i=0;i<200;++i) r.c.onAction(out,r.t);
    CHECK_NEAR(r.c.engine().sample().pose.fovRad, fscs::core::kMaxFovRad, 1e-9);
    r.send("ReleaseCamera"); const double held = r.c.engine().look().fovDeltaRad;
    r.c.onAction(in,r.t); CHECK_NEAR(r.c.engine().look().fovDeltaRad,held,1e-9);
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
TEST_CASE("coordinator: mouse look requires current session and active camera") {
    Rig r;
    r.connect();
    CHECK_EQ(r.send("MouseLookDelta", {{"headingDeg", 5}})["status"].get<std::string>(), "Rejected");
    r.activate(r.pilot); r.grant();
    CHECK_EQ(r.send("MouseLookDelta", {{"headingDeg", 5}, {"pitchDeg", -3}})["status"].get<std::string>(), "Completed");
    CHECK_EQ(r.send("MouseLookDelta", {{"headingDeg", 5}}, r.c.epoch() + 1)["reason"].get<std::string>(), "staleEpoch");
    CHECK_EQ(r.send("MouseLookDelta", {{"headingDeg", 50}})["status"].get<std::string>(), "Rejected");
    CHECK_EQ(r.send("MouseLookDelta", {{"rightMeters", .1}, {"upMeters", .2}, {"forwardMeters", .3}})["status"].get<std::string>(), "Completed");
    CHECK_NEAR(r.c.engine().look().translationHeadingFrame.x,.3,1e-9);
    CHECK_NEAR(r.c.engine().look().translationHeadingFrame.y,.1,1e-9);
    CHECK_NEAR(r.c.engine().look().translationHeadingFrame.z,-.2,1e-9);
    CHECK_EQ(r.send("MouseLookDelta", {{"rightMeters", 1}})["status"].get<std::string>(), "Rejected");
    r.send("ReleaseCamera");
    CHECK_EQ(r.send("MouseLookDelta", {{"headingDeg", 5}})["status"].get<std::string>(), "Rejected");
}
TEST_CASE("coordinator: capture uses the selected camera reference frame") {
    Rig r;
    r.profile["cameras"][1]["mode"] = "aircraftExterior";
    r.connect();
    CHECK_EQ(r.send("CaptureView", {{"cameraId", r.pfd}})["status"].get<std::string>(), "Accepted");
    CHECK(r.backend.readFrames.back() == Referential::SimObject);
    CHECK_EQ(r.send("CaptureView", {{"cameraId", "missing"}})["status"].get<std::string>(), "Rejected");
}
TEST_CASE("coordinator: native right drag adds temporary look without leaving the selected camera") {
    for (const auto* mode : {"aircraftCockpit", "aircraftExterior"}) {
        Rig r; r.profile["cameras"][0]["mode"] = mode; r.connect();
        r.deliver(be::NativeMouseMapping{true});
        r.deliver(be::NativeMouseButton{true}); r.deliver(be::NativeMouseAxis{true, 8192}); r.advance(.01, 150);
        CHECK_NEAR(r.c.engine().look().headingDeg, 0, 1e-9);
        r.activate(r.pilot); r.grant();
        r.deliver(be::NativeMouseAxis{true, 8192}); r.advance(.01, 150); // fresh press required after acquire
        CHECK_NEAR(r.c.engine().look().headingDeg, 0, 1e-9);
        r.deliver(be::NativeMouseButton{true});
        r.deliver(be::NativeMouseAxis{true, 8192}); r.advance(.01, 150); r.deliver(be::NativeMouseAxis{false, -16384}); r.advance(.01, 150);
        CHECK_NEAR(r.c.engine().look().headingDeg, 1.5, 1e-9);
        CHECK_NEAR(r.c.engine().look().pitchDeg, 3, 1e-9);
        r.deliver(be::NativeMouseButton{false}); r.deliver(be::NativeMouseAxis{true, 100}); r.advance(.01, 150);
        r.advance(.02, 8);
        CHECK_NEAR(r.c.engine().look().headingDeg, 1.5, 1e-9);
        CHECK(r.c.ownership() == Ownership::Active);
        CHECK_EQ(r.c.activeCameraId(), r.pilot);
        for (const auto& call : r.backend.calls) CHECK(call != "release");
        CHECK(!r.backend.submitted.empty());
        // Saved preset remains unchanged; only the engine's transient look was modified.
        const auto saved = toSnapshot(r.profile);
        CHECK_NEAR(r.c.profile()->camera(r.pilot)->pose.orientation.w, saved.camera(r.pilot)->pose.orientation.w, 1e-9);
    }
}
TEST_CASE("coordinator: native mouse respects UI blocking, pause, release, mapping failure and signed deltas") {
    Rig r; r.connect(); r.activate(r.pilot); r.grant(); r.deliver(be::NativeMouseMapping{true});
    r.c.setMouseInputBlocked(true); r.deliver(be::NativeMouseButton{true}); r.deliver(be::NativeMouseAxis{true, 8192}); r.advance(.01, 150);
    r.c.setMouseInputBlocked(false); r.deliver(be::NativeMouseAxis{true, 8192}); r.advance(.01, 150);
    CHECK_NEAR(r.c.engine().look().headingDeg, 0, 1e-9);
    r.deliver(be::NativeMouseButton{false}); r.deliver(be::NativeMouseButton{true}); r.deliver(be::NativeMouseAxis{true, -8192}); r.advance(.01, 150);
    CHECK_NEAR(r.c.engine().look().headingDeg, -1.5, 1e-9);
    r.deliver(be::NativeMouseAxis{true, 100000}); r.advance(.01, 150);
    const double moved = -1.5 + 100000.0 / 32768 * 6;
    CHECK_NEAR(r.c.engine().look().headingDeg, moved, 1e-9);
    r.c.cancelMouseDrag(); r.deliver(be::NativeMouseAxis{true, 8192}); r.advance(.01, 150);
    CHECK_NEAR(r.c.engine().look().headingDeg, moved, 1e-9);
    r.deliver(be::NativeMouseButton{true}); r.deliver(be::Paused{true});
    const auto suspended = r.c.engine().look().headingDeg;
    r.deliver(be::NativeMouseAxis{true, 8192}); r.advance(.01, 150);
    CHECK_NEAR(r.c.engine().look().headingDeg, suspended, 1e-9);
    r.deliver(be::Paused{false}); const auto resumed = r.c.engine().look().headingDeg;
    r.deliver(be::NativeMouseAxis{true, 8192}); r.advance(.01, 150);
    CHECK_NEAR(r.c.engine().look().headingDeg, resumed, 1e-9);
    r.deliver(be::NativeMouseMapping{false}); r.deliver(be::NativeMouseButton{true}); r.deliver(be::NativeMouseAxis{true, 8192}); r.advance(.01, 150);
    CHECK_NEAR(r.c.engine().look().headingDeg, resumed, 1e-9);
    r.send("ReleaseCamera"); const auto after = r.c.engine().look().headingDeg;
    r.deliver(be::NativeMouseMapping{true}); r.deliver(be::NativeMouseButton{true}); r.deliver(be::NativeMouseAxis{true, 8192}); r.advance(.01, 150);
    CHECK_NEAR(r.c.engine().look().headingDeg, after, 1e-9);
}

TEST_CASE("coordinator: reset-on-release returns look without changing the camera preset") {
    Rig r; r.profile["cameras"][0]["mouseLookOnRelease"] = "reset";
    r.connect(); r.activate(r.pilot); r.grant(); r.deliver(be::NativeMouseMapping{true});
    const auto saved = r.c.profile()->camera(r.pilot)->pose;
    r.deliver(be::NativeMouseButton{true}); r.deliver(be::NativeMouseAxis{true, 16384}); r.advance(.01, 150);
    r.deliver(be::NativeMouseAxis{false, -8192}); r.advance(.01, 150);
    CHECK_NEAR(r.c.engine().look().headingDeg, 3, 1e-9);
    r.deliver(be::NativeMouseButton{false});
    CHECK_NEAR(r.c.engine().look().headingDeg, 0, 1e-9);
    CHECK_NEAR(r.c.engine().look().pitchDeg, 0, 1e-9);
    CHECK(r.c.ownership() == Ownership::Active);
    CHECK_EQ(r.c.activeCameraId(), r.pilot);
    CHECK_NEAR(r.c.profile()->camera(r.pilot)->pose.orientation.w, saved.orientation.w, 1e-9);
    r.send("MouseLookDelta", {{"headingDeg", 4}});
    r.deliver(be::NativeMouseButton{true}); r.deliver(be::NativeMouseButton{false});
    CHECK_NEAR(r.c.engine().look().headingDeg, 4, 1e-9); // a stationary/context click is not a drag
    r.deliver(be::NativeMouseButton{true}); r.deliver(be::NativeMouseAxis{true, 8192}); r.advance(.01, 150);
    r.activate(r.pfd); const auto switched = r.c.engine().look().headingDeg;
    r.deliver(be::NativeMouseAxis{true, 8192}); r.advance(.01, 150); r.deliver(be::NativeMouseButton{false});
    CHECK_NEAR(r.c.engine().look().headingDeg, switched, 1e-9);
    CHECK_EQ(r.c.activeCameraId(), r.pfd);
}

TEST_CASE("coordinator: observed 642-count mouse step is small, smoothed and frame-rate independent") {
    const auto sample = [](double step, int frames) {
        Rig r; r.connect(); r.activate(r.pilot); r.grant(); r.deliver(be::NativeMouseMapping{true});
        r.deliver(be::NativeMouseButton{true}); r.deliver(be::NativeMouseAxis{true, 642});
        CHECK_NEAR(r.c.engine().look().headingDeg, 0, 1e-12); // input callback never jumps the view
        r.advance(step);
        CHECK(r.c.engine().look().headingDeg > 0);
        CHECK(r.c.engine().look().headingDeg < 642.0 / 32768 * 6);
        r.advance(step, frames - 1);
        const double result = r.c.engine().look().headingDeg;
        CHECK_NEAR(result, 642.0 / 32768 * 6, 1e-8);
        r.deliver(be::NativeMouseAxis{true, 32767}); r.advance(step);
        const double stopped = r.c.engine().look().headingDeg;
        r.deliver(be::NativeMouseButton{false}); r.advance(.01, 100);
        CHECK_NEAR(r.c.engine().look().headingDeg, stopped, 1e-12);
        return result;
    };
    CHECK_NEAR(sample(1.0 / 30, 30), sample(1.0 / 120, 120), 1e-9);
}

TEST_CASE("coordinator: repeated right-down does not restart smoothing or lose accumulated mouse movement") {
    Rig r; r.connect(); r.activate(r.pilot); r.grant(); r.deliver(be::NativeMouseMapping{true});
    r.deliver(be::NativeMouseButton{true});
    r.deliver(be::NativeMouseAxis{true, 8192});
    r.advance(.01);
    const double first = r.c.engine().look().headingDeg;
    r.deliver(be::NativeMouseButton{true}); // duplicate notification during the same physical drag
    r.advance(.01, 150);
    CHECK(r.c.engine().look().headingDeg > first);
    CHECK_NEAR(r.c.engine().look().headingDeg, 1.5, 1e-9);
    r.deliver(be::NativeMouseButton{false});
    r.advance(.05, 10);
    CHECK_NEAR(r.c.engine().look().headingDeg, 1.5, 1e-9);
}

TEST_CASE("coordinator: airport cameras use geographic coordinates and cut across reference frames") {
    Rig r;std::vector<std::string> notes;auto airport=fscs::profiles::airport::generate(airportFixture(),json::object(),notes);
    for(auto& cat:airport["categories"])r.profile["categories"].push_back(cat);
    for(auto& cam:airport["cameras"])r.profile["cameras"].push_back(cam);
    r.connect();r.activate(r.pilot);r.grant();r.advance(.02);
    const auto cam=airport["cameras"][0];r.activate(cam["id"].get<std::string>());r.advance(.02);
    CHECK(r.backend.submitted.back().referential==Referential::World);
    CHECK(!r.c.engine().transitioning());
    auto pos=cam["positionMetersFRD"];const auto expected=fscs::profiles::airport::world({40,-90,100},{pos[0],pos[1],pos[2]});
    CHECK_NEAR(r.backend.submitted.back().x,expected.lat,1e-8);CHECK_NEAR(r.backend.submitted.back().y,expected.lon,1e-8);CHECK_NEAR(r.backend.submitted.back().z,expected.altitude,1e-8);
    r.activate(r.pilot);r.advance(.02);CHECK(r.backend.submitted.back().referential==Referential::Eyepoint);CHECK(!r.c.engine().transitioning());
    // Capture for an inactive airport camera must convert against that airport, not the active cockpit.
    r.send("ReleaseCamera");r.deliver(be::CameraStatus{SdkAvailability::NotAcquired,false});
    r.send("CaptureView",{{"cameraId",cam["id"]}});
    SdkPose sdk;sdk.referential=Referential::World;sdk.x=40;sdk.y=-90;sdk.z=110;sdk.fovRad=1;
    r.deliver(be::CameraData{sdk});
    for(auto it=r.events.rbegin();it!=r.events.rend();++it)if(it->first=="ViewCaptured"){
        CHECK_NEAR(it->second["positionMetersFRD"][2].get<double>(),-10,1e-8);break;
    }
}

TEST_CASE("coordinator: drag diagnostics report timing gaps and competing bound look only after release") {
    Rig r; r.connect(); r.activate(r.pilot); r.grant(); r.deliver(be::NativeMouseMapping{true});
    r.deliver(be::NativeMouseButton{true}); r.deliver(be::NativeMouseAxis{true, 642});
    r.advance(.01);
    r.deliver(be::NativeMouseAxis{false, -1285});
    r.c.setManualInput({1, 0, 0, 0, 0, 0});
    r.advance(.08);
    r.deliver(be::NativeMouseAxis{true, 100000});
    r.deliver(be::NativeMouseButton{false});
    const auto& report = r.events.back().second;
    CHECK_EQ(r.events.back().first, std::string("MouseInputDiagnostic"));
    CHECK_EQ(report["samples"].get<unsigned>(), 3u);
    CHECK_EQ(report["largeSamples"].get<unsigned>(), 1u);
    CHECK_EQ(report["frames"].get<unsigned>(), 2u);
    CHECK_NEAR(report["maxSimFrameGapMs"].get<double>(), 80, 1e-7);
    CHECK_NEAR(report["maxSampleGapMs"].get<double>(), 80, 1e-7);
    CHECK(report["concurrentBoundLook"].get<bool>());
    CHECK_NEAR(report["maxRaw"].get<double>(), 100000, 1e-7);
}

TEST_CASE("coordinator: mouse motion remains continuous beyond joystick limits and bounded at int32 extremes") {
    for (const auto raw : {32767, 32768, -32768, -32769, 78386, -81598, 100874, -126574,
                           std::numeric_limits<std::int32_t>::min(), std::numeric_limits<std::int32_t>::max()}) {
        for (const double step : {1.0 / 30, 1.0 / 120}) {
            Rig r; r.connect(); r.activate(r.pilot); r.grant(); r.deliver(be::NativeMouseMapping{true});
            r.deliver(be::NativeMouseButton{true});
            r.deliver(be::NativeMouseAxis{true, raw});
            const double expected = std::clamp(raw / 32768.0 * 6, -45.0, 45.0);
            CHECK_NEAR(r.c.engine().look().headingDeg, 0, 1e-12);
            r.advance(step);
            const double first = r.c.engine().look().headingDeg;
            CHECK(first * expected > 0);
            CHECK(std::abs(first) < std::abs(expected));
            r.advance(step, static_cast<int>(2 / step));
            CHECK_NEAR(r.c.engine().look().headingDeg, expected, 1e-8);
            r.deliver(be::NativeMouseAxis{true, -642}); r.advance(step);
            const double stopped = r.c.engine().look().headingDeg;
            r.deliver(be::NativeMouseButton{false}); r.advance(step, 60);
            CHECK_NEAR(r.c.engine().look().headingDeg, stopped, 1e-12);
        }
    }
}

TEST_CASE("tracking: per-camera simulator switch restores ownership and respects external changes") {
    Rig r; r.profile["cameras"][0]["trackingPolicy"]="off";r.profile["cameras"][1]["trackingPolicy"]="on";
    r.connect();r.advance(.02);CHECK_EQ(r.backend.count("tracking:read"),std::size_t{0});
    r.activate(r.pilot);r.grant();r.advance(.02);CHECK(r.backend.tracking==false);
    r.activate(r.pfd);r.advance(.02);CHECK(r.backend.tracking==true);
    r.activate(r.pilot);r.advance(.02);CHECK(r.backend.tracking==false);
    r.send("ReleaseCamera");CHECK(r.backend.tracking==true);
    r.deliver(be::CameraStatus{SdkAvailability::NotAcquired,false});r.backend.tracking=false;
    r.activate(r.pfd);r.grant();r.advance(.02);CHECK(r.backend.tracking==true);
    r.backend.tracking=false; // external user preference wins over release restoration
    const auto writes=r.backend.count("tracking:off");r.send("ReleaseCamera");CHECK_EQ(r.backend.count("tracking:off"),writes);
}
TEST_CASE("tracking: unavailable and rejected controls report failure without repeated writes") {
    Rig r;r.profile["cameras"][0]["trackingPolicy"]="off";r.backend.trackingWriteOk=false;
    r.connect();r.activate(r.pilot);r.grant();r.advance(.02,100);
    CHECK_EQ(r.backend.count("tracking:off"),std::size_t{1});
    CHECK(!r.c.capabilitiesJson()["headTracking"]["error"].get<std::string>().empty());
    r.backend.tracking.reset();r.activate(r.pfd);r.advance(.02);
    CHECK(r.c.capabilitiesJson()["headTracking"]["enabled"].is_null());
}
TEST_CASE("look: temporary offsets belong to each camera and return on release or panel cancellation") {
    Rig r;r.profile["cameras"][0]["temporaryLookPolicy"]="retain";r.connect();r.activate(r.pilot);r.grant();
    r.deliver(be::NativeMouseMapping{true});r.deliver(be::NativeMouseButton{true});r.deliver(be::NativeMouseAxis{true,32768});r.advance(.02,100);r.deliver(be::NativeMouseButton{false});
    const auto look=r.c.engine().look().headingDeg;CHECK(look>5);
    r.activate(r.pfd);r.advance(.02,50);CHECK_NEAR(r.c.engine().look().headingDeg,0,1e-9);
    r.activate(r.pilot);r.advance(.02,50);CHECK_NEAR(r.c.engine().look().headingDeg,look,1e-9);
    r.profile["cameras"][0]["mouseLookOnRelease"]="reset";r.setProfile();
    r.deliver(be::NativeMouseButton{true});r.deliver(be::NativeMouseAxis{true,32768});r.advance(.02,20);r.c.setMouseInputBlocked(true);
    CHECK_NEAR(r.c.engine().look().headingDeg,0,1e-9);
    r.c.setMouseInputBlocked(false);r.deliver(be::NativeMouseButton{false});r.deliver(be::NativeMouseButton{true});r.deliver(be::NativeMouseAxis{true,32768});r.advance(.02,20);r.deliver(be::NativeMouseButton{false});
    CHECK_NEAR(r.c.engine().look().headingDeg,0,1e-9);
}
TEST_CASE("airports: sub-centimeter edits reach world SDK and heading/pitch tracking keeps position") {
    Rig r;std::vector<std::string> notes;auto airport=fscs::profiles::airport::generate(airportFixture(),json::object(),notes);
    auto cam=airport["cameras"][0];cam["positionMetersFRD"]={0,0,-3};cam["orientationCameraToAnchorXYZW"]={0,0,0,1};cam["transition"]["kind"]="cut";
    auto cat=airport["categories"][0];cat["cameraIds"]=json::array({cam["id"]});cat["defaultCameraId"]=cam["id"];r.profile["categories"].push_back(cat);r.profile["cameras"].push_back(cam);
    r.connect();r.activate(cam["id"]);r.grant();r.advance(.02);const auto first=r.backend.submitted.back();
    r.profile["cameras"].back()["positionMetersFRD"][0]=.001;r.setProfile();r.advance(.02);
    CHECK(r.backend.submitted.back().x>first.x);CHECK(r.backend.submitted.back().x-first.x<1e-7);
    r.profile["cameras"].back()["aircraftTracking"]="both";r.setProfile();r.c.setAircraftPosition(40,-89.999,150,r.t);r.advance(.02);
    CHECK_NEAR(r.backend.submitted.back().headingDeg,90,0.1);CHECK(r.backend.submitted.back().pitchDeg<0);
    const auto trackingPosition=r.backend.submitted.back();r.c.setAircraftPosition(40.001,-90,150,r.t);r.advance(.02);
    CHECK_NEAR(r.backend.submitted.back().headingDeg,0,0.1);CHECK_NEAR(r.backend.submitted.back().x,trackingPosition.x,1e-12);
    r.profile["cameras"].back()["aircraftTracking"]="off";r.setProfile();r.advance(.02);CHECK_NEAR(r.backend.submitted.back().headingDeg,0,1e-9);
    fscs::core::ManualInput input;input.moveForward=1;r.c.setManualInput(input);r.advance(.02,50);CHECK(r.backend.submitted.back().x>trackingPosition.x);
}
