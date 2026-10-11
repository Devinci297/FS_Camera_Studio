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

TEST_CASE("coordinator: unsupported free mode stays visible but cannot activate") {
    Rig r;
    r.profile["cameras"][1]["mode"] = "free";
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
        if (std::string(def) == "Q") CHECK(r.manualInput().moveUp > 0);
        if (std::string(def) == "E") CHECK(r.manualInput().moveUp < 0);
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

TEST_CASE("input: movement letter aliases and both Shift chords release cleanly without double speed") {
    auto r = routerWithDefaults();
    for (const auto* definition : {"q", "e", "VK_LSHIFT+q", "VK_LSHIFT+E", "VK_RSHIFT+Q", "VK_RSHIFT+e"}) {
        const std::string def = definition;
        const auto index = indexOf(r, def);
        CHECK(index < r.definitions().size());
        r.onInput(index, true, 0);
        CHECK_NEAR(r.manualInput().moveUp, def.back() == 'q' || def.back() == 'Q' ? 1 : -1, 1e-9);
        CHECK(r.manualInput().fast == (def.find('+') != std::string::npos));
        r.onInput(index, false, 0);
        CHECK_NEAR(r.manualInput().moveUp, 0, 1e-9);
        CHECK(!r.manualInput().fast);
    }
    r.onInput(indexOf(r, "E"), true, 0);
    r.onInput(indexOf(r, "e"), true, 0);
    r.onInput(indexOf(r, "VK_RSHIFT+e"), true, 0);
    CHECK_NEAR(r.manualInput().moveUp, -1, 1e-9);
    CHECK(r.manualInput().fast);
    r.onInput(indexOf(r, "VK_RSHIFT+e"), false, 0);
    CHECK_NEAR(r.manualInput().moveUp, 0, 1e-9);
    CHECK(!r.manualInput().fast);
    r.onInput(indexOf(r, "VK_LSHIFT+e"), true, 0);
    r.setTextFocus(true);
    CHECK_NEAR(r.manualInput().moveUp, 0, 1e-9);
    CHECK(!r.manualInput().fast);
    r.setTextFocus(false);
    CHECK(r.onInput(indexOf(r, "VK_LSHIFT+e"), true, 0).empty());
    r.onInput(indexOf(r, "VK_LSHIFT+e"), false, 0);
    r.onInput(indexOf(r, "VK_LSHIFT+e"), true, 0);
    CHECK_NEAR(r.manualInput().moveUp, -1, 1e-9);
    CHECK(r.manualInput().fast);
    r.releaseAll();
    CHECK_NEAR(r.manualInput().moveUp, 0, 1e-9);
    CHECK(!r.manualInput().fast);
}

TEST_CASE("input: boost aliases preserve custom bindings and follow rebound movement and boost") {
    auto doc = fscs::profiles::defaultBindingsDocument();
    doc["bindings"].push_back({{"action", "nextCamera"}, {"input", {{"kind", "key"}, {"definition", "VK_LSHIFT+Q"}}}});
    auto parsed = fscs::profiles::parseBindings(doc);
    CHECK(parsed.problems.empty());
    InputRouter r;
    r.setBindings(parsed.bindings);
    const auto event = r.onInput(indexOf(r, "VK_LSHIFT+Q"), true, 0);
    CHECK_EQ(event.size(), std::size_t{1});
    if (!event.empty()) CHECK(event.front().action == Action::NextCamera);
    CHECK_NEAR(r.manualInput().moveUp, 0, 1e-9);
    CHECK(!r.manualInput().fast);
    for (auto& binding : parsed.bindings) {
        if (binding.action == Action::MoveUp) binding.input.definition = "U";
        if (binding.action == Action::MoveFast) binding.input.definition = "VK_RSHIFT";
    }
    r.setBindings(parsed.bindings);
    CHECK(indexOf(r, "q") == r.definitions().size());
    CHECK(indexOf(r, "VK_LSHIFT+u") == r.definitions().size());
    r.onInput(indexOf(r, "VK_RSHIFT+u"), true, 0);
    CHECK_NEAR(r.manualInput().moveUp, 1, 1e-9);
    CHECK(r.manualInput().fast);
    r.setBindings({});
    CHECK(r.definitions().empty());
    CHECK(!r.manualInput().fast);
}

TEST_CASE("airports: Q E and Shift chords move world altitude with tracking and coarse speed") {
    auto run = [](const char* definition, const char* precision) {
        Rig r;
        std::vector<std::string> notes;
        const auto airport = fscs::profiles::airport::generate(airportFixture(), json::object(), notes);
        auto cam = airport["cameras"][0];
        cam["positionMetersFRD"] = {0, 0, -3};
        cam["orientationCameraToAnchorXYZW"] = {0, 0, 0, 1};
        cam["aircraftTracking"] = "both";
        cam["transition"]["kind"] = "cut";
        auto cat = airport["categories"][0];
        cat["cameraIds"] = json::array({cam["id"]});
        cat["defaultCameraId"] = cam["id"];
        r.profile["categories"].push_back(cat);
        r.profile["cameras"].push_back(cam);
        r.connect();
        auto settings = fscs::profiles::defaultSettingsDocument();
        settings["ui"]["precision"] = precision;
        r.c.setSettings(fscs::profiles::parseHostSettings(settings), r.t);
        r.c.setAircraftPosition(40.001, -89.999, 150, r.t);
        r.activate(cam["id"]); r.grant(); r.advance(.02);
        const auto before = r.backend.submitted.back();
        auto input = routerWithDefaults();
        const auto index = indexOf(input, definition);
        CHECK(index < input.definitions().size());
        input.onInput(index, true, 0);
        r.c.setManualInput(input.manualInput());
        r.advance(1.0 / 60, 120);
        const auto after = r.backend.submitted.back();
        CHECK(after.referential == Referential::World);
        CHECK(!after.trackUserAircraft);
        CHECK_NEAR(after.x, before.x, 1e-10);
        CHECK_NEAR(after.y, before.y, 1e-10);
        input.onInput(index, false, 0);
        r.c.setManualInput(input.manualInput());
        r.advance(1.0 / 60, 240); // Let the normal movement deceleration settle.
        const double stopped = r.backend.submitted.back().z;
        r.advance(1.0 / 60, 60);
        CHECK_NEAR(r.backend.submitted.back().z, stopped, 1e-6);
        return after.z - before.z;
    };
    const auto up = run("q", "normal"), down = run("e", "normal");
    const auto coarse = run("q", "coarse");
    CHECK(up > .5);
    CHECK_NEAR(down, -up, 1e-8);
    CHECK_NEAR(coarse, up * 10, 1e-8);
    CHECK_NEAR(run("VK_LSHIFT+Q", "coarse"), coarse * 4, 1e-8);
    CHECK_NEAR(run("VK_RSHIFT+e", "coarse"), -coarse * 4, 1e-8);
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

TEST_CASE("tracking: global simulator switch ignores camera policy and restores ownership") {
    Rig r; r.profile["cameras"][0]["trackingPolicy"]="on";r.profile["cameras"][1]["trackingPolicy"]="off";
    r.connect();r.advance(.02);CHECK_EQ(r.backend.count("tracking:read"),std::size_t{0});
    r.activate(r.pilot);r.grant();r.advance(.02);CHECK(r.backend.tracking==false);
    const auto writes=r.backend.count("tracking:off");
    r.activate(r.pfd);r.advance(.02);CHECK(r.backend.tracking==false);CHECK_EQ(r.backend.count("tracking:off"),writes);
    fscs::profiles::HostSettings settings;settings.globalHeadTrackingEnabled=true;r.c.setSettings(settings,r.t);r.advance(.02);
    CHECK(r.backend.tracking==true);
    r.activate(r.pilot);r.advance(.02);CHECK(r.backend.tracking==true);
    r.send("ReleaseCamera");CHECK(r.backend.tracking==true);
    r.deliver(be::CameraStatus{SdkAvailability::NotAcquired,false});r.backend.tracking=false;
    r.activate(r.pfd);r.grant();r.advance(.02);CHECK(r.backend.tracking==true);
    r.backend.tracking=false; // External change wins over release restoration.
    const auto offWrites=r.backend.count("tracking:off");r.send("ReleaseCamera");CHECK_EQ(r.backend.count("tracking:off"),offWrites);
}
TEST_CASE("tracking: unavailable and rejected controls report failure without repeated writes") {
    Rig r;r.profile["cameras"][0]["trackingPolicy"]="off";r.backend.trackingWriteOk=false;
    r.connect();r.activate(r.pilot);r.grant();r.advance(.02,100);
    CHECK_EQ(r.backend.count("tracking:off"),std::size_t{1});
    CHECK(!r.c.capabilitiesJson()["headTracking"]["error"].get<std::string>().empty());
    r.backend.tracking.reset();r.activate(r.pfd);r.advance(.02,20);
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
    CHECK(!r.backend.submitted.back().trackUserAircraft);
    const auto fov=r.backend.submitted.back().fovRad;
    r.c.onAction({fscs::profiles::Action::ZoomIn,0,true,0,true},r.t);r.advance(.02);
    CHECK(r.backend.submitted.back().fovRad<fov);
    CHECK(!r.backend.submitted.back().trackUserAircraft);
    // Explicit world look-at keeps the last target through telemetry pauses; right-drag takes over.
    r.advance(.02,60);CHECK(!r.backend.submitted.back().trackUserAircraft);
    r.deliver(be::NativeMouseMapping{true});r.deliver(be::NativeMouseButton{true});r.advance(.02);
    CHECK(!r.backend.submitted.back().trackUserAircraft);
    r.deliver(be::NativeMouseButton{false});r.advance(.02);
    CHECK(!r.backend.submitted.back().trackUserAircraft);
    const auto trackingPosition=r.backend.submitted.back();r.c.setAircraftPosition(40.001,-90,150,r.t);r.advance(.02);
    CHECK_NEAR(r.backend.submitted.back().headingDeg,0,0.1);CHECK_NEAR(r.backend.submitted.back().x,trackingPosition.x,1e-12);
    fscs::profiles::HostSettings fixed;fixed.globalAircraftTracking="off";r.c.setSettings(fixed,r.t);r.advance(.02);CHECK_NEAR(r.backend.submitted.back().headingDeg,0,1e-9);
    CHECK(!r.backend.submitted.back().trackUserAircraft);
    fscs::core::ManualInput input;input.moveForward=1;r.c.setManualInput(input);r.advance(.02,50);CHECK(r.backend.submitted.back().x>trackingPosition.x);
}

TEST_CASE("input: releasing another case or modifier alias neutralizes movement without affecting other keys") {
    for (const auto& pair : std::vector<std::pair<const char*, const char*>>{
             {"Q", "q"}, {"e", "E"}, {"VK_LSHIFT+Q", "q"}, {"VK_RSHIFT+e", "E"}}) {
        auto r = routerWithDefaults();
        r.onInput(indexOf(r, pair.first), true, 0);
        CHECK(std::abs(r.manualInput().moveUp) == 1);
        r.onInput(indexOf(r, pair.second), false, 0);
        CHECK_NEAR(r.manualInput().moveUp, 0, 1e-9);
        CHECK(!r.manualInput().fast);
        // A wheel pulse after key release must not revive a stale translation hold.
        r.onInput(indexOf(r, "sim:ZOOM_IN"), true, 1);
        CHECK_NEAR(r.manualInput().moveUp, 0, 1e-9);
        CHECK_EQ(r.onInput(indexOf(r, pair.first), true, 0).size(), std::size_t{1});
    }
    for (const auto* modifier : {"VK_LSHIFT", "VK_RSHIFT", "VK_SHIFT"}) {
        for (bool modifierFirst : {false, true}) {
            auto r = routerWithDefaults();
            r.onInput(indexOf(r, modifier), true, 0);
            r.onInput(indexOf(r, "VK_LSHIFT+q"), true, 0);
            r.onInput(indexOf(r, "w"), true, 0);
            if (modifierFirst) {
                // The generic Shift release also clears either generated side's chord.
                r.onInput(indexOf(r, "VK_SHIFT"), false, 0);
                CHECK_NEAR(r.manualInput().moveUp, 0, 1e-9);
                r.onInput(indexOf(r, "Q"), false, 0);
            } else {
                r.onInput(indexOf(r, "Q"), false, 0);
                r.onInput(indexOf(r, "VK_SHIFT"), false, 0);
            }
            CHECK_NEAR(r.manualInput().moveUp, 0, 1e-9);
            CHECK(!r.manualInput().fast);
            CHECK_NEAR(r.manualInput().moveForward, 1, 1e-9);
            r.onInput(indexOf(r, "W"), false, 0);
            CHECK_NEAR(r.manualInput().moveForward, 0, 1e-9);
        }
    }
    auto doc = fscs::profiles::defaultBindingsDocument();
    doc["bindings"].push_back({{"action", "nextCamera"}, {"input", {{"kind", "key"}, {"definition", "VK_LSHIFT+Q"}}}});
    InputRouter custom; custom.setBindings(fscs::profiles::parseBindings(doc).bindings);
    const auto pressed = custom.onInput(indexOf(custom, "VK_LSHIFT+Q"), true, 0);
    CHECK_EQ(pressed.size(), std::size_t{1});
    custom.onInput(indexOf(custom, "q"), false, 0);
    custom.onInput(indexOf(custom, "VK_LSHIFT"), false, 0);
    const auto released = custom.onInput(indexOf(custom, "VK_LSHIFT+Q"), false, 0);
    CHECK_EQ(released.size(), std::size_t{1});
    if (!pressed.empty() && !released.empty()) CHECK_EQ(pressed[0].holdId, released[0].holdId);
}

TEST_CASE("airports: fixed and tracked views accept vertical keys and live altitude edits independently") {
    std::vector<double> raised;
    for (const auto* tracking : {"off", "heading", "both"}) {
        Rig r; std::vector<std::string> notes;
        const auto airport = fscs::profiles::airport::generate(airportFixture(), json::object(), notes);
        auto cam = airport["cameras"][0];
        cam.erase("airportHeight"); // Legacy explicitly positioned camera; generated-height behavior is tested below.
        cam["positionMetersFRD"] = {0, 0, -3};
        cam["orientationCameraToAnchorXYZW"] = {0, 0, 0, 1};
        cam["aircraftTracking"] = tracking;
        cam["transition"]["kind"] = "cut";
        auto cat = airport["categories"][0]; cat["cameraIds"] = json::array({cam["id"]}); cat["defaultCameraId"] = cam["id"];
        r.profile["categories"].push_back(cat); r.profile["cameras"].push_back(cam);
        r.connect(); fscs::profiles::HostSettings settings;settings.globalAircraftTracking=tracking;r.c.setSettings(settings,r.t);r.c.setAircraftPosition(40.001, -89.999, 150, r.t);
        r.activate(cam["id"]); r.grant(); r.advance(.02);
        const auto first = r.backend.submitted.back();
        auto input = routerWithDefaults();
        input.onInput(indexOf(input, "Q"), true, 0); r.c.setManualInput(input.manualInput()); r.advance(.02, 50);
        const auto up = r.backend.submitted.back();
        CHECK(up.z > first.z); raised.push_back(up.z-first.z);
        CHECK_NEAR(up.x, first.x, 1e-10); CHECK_NEAR(up.y, first.y, 1e-10);
        CHECK(!up.trackUserAircraft);
        input.onInput(indexOf(input, "Q"), false, 0);
        input.onInput(indexOf(input, "E"), true, 0); r.c.setManualInput(input.manualInput()); r.advance(.02, 100);
        CHECK(r.backend.submitted.back().z < up.z);
        input.onInput(indexOf(input, "E"), false, 0); r.c.setManualInput(input.manualInput());
        r.send("RecenterLook");
        r.profile["cameras"].back()["positionMetersFRD"] = {0, 0, -30};
        r.setProfile(); r.advance(.02);
        const auto edited = r.backend.submitted.back();
        CHECK_NEAR(edited.z, 130, 1e-6);
        CHECK(!edited.trackUserAircraft);
        CHECK(r.backend.count("flag:ground:on") > 0);
        if (std::string(tracking) == "both") {
            CHECK(edited.pitchDeg < 0);
            CHECK(edited.pitchDeg > first.pitchDeg);
            r.c.setAircraftPosition(40.001, -90, 160, r.t); r.advance(.02);
            const auto followed = r.backend.submitted.back();
            CHECK_NEAR(followed.headingDeg, 0, .1);
            CHECK_NEAR(followed.z, edited.z, 1e-6);
            CHECK(std::abs(followed.pitchDeg-edited.pitchDeg) > .1);
        }
    }
    CHECK_NEAR(raised[0], raised[1], 1e-9);
    CHECK_NEAR(raised[0], raised[2], 1e-9);
}

TEST_CASE("motion views: orbit circles aircraft with stable radius and opposite directions") {
    for (const bool clockwise : {true,false}) {
        Rig r;
        auto& cam=r.profile["cameras"][0]; cam["mode"]="orbit"; cam["transition"]["kind"]="cut";
        cam["motion"]={{"radiusMeters",40},{"heightMeters",12},{"durationSeconds",8},{"clockwise",clockwise}};
        r.connect();r.activate(r.pilot);r.grant();r.advance(.02,100);
        const auto pose=r.c.engine().sample().pose;
        CHECK_NEAR(std::hypot(pose.positionFRD.x,pose.positionFRD.y),40,1e-7);
        CHECK_NEAR(pose.positionFRD.z,-12,1e-7);
        CHECK(std::abs(pose.positionFRD.x)<2);
        CHECK(clockwise ? pose.positionFRD.y>39 : pose.positionFRD.y< -39);
        CHECK(r.backend.submitted.back().referential==Referential::SimObject);
        auto forward=pose.orientation.rotate({1,0,0});
        CHECK(fscs::core::dot(forward,pose.positionFRD)<-40);
        const auto before=pose.positionFRD;
        r.deliver(be::Paused{true});r.advance(.02,100);
        CHECK_NEAR((r.c.engine().sample().pose.positionFRD-before).length(),0,1e-9);
    }
}

TEST_CASE("motion views: paused flyby remains world stationary and tracks a moving aircraft") {
    Rig r;auto& cam=r.profile["cameras"][0];cam["mode"]="flyby";cam["transition"]["kind"]="cut";
    cam["motion"]={{"radiusMeters",100},{"heightMeters",10},{"durationSeconds",30},{"clockwise",true}};
    r.connect();
    CHECK_EQ(r.send("ActivateCamera",{{"cameraId",r.pilot}})["status"].get<std::string>(),std::string("Rejected"));
    r.c.setAircraftPosition(40,-90,1000,r.t,90);
    r.activate(r.pilot);r.grant();r.deliver(be::Paused{true});r.advance(.02);
    const auto first=r.backend.submitted.back();
    CHECK(first.referential==Referential::World);
    CHECK(first.y> -90); // ahead of an eastbound aircraft
    CHECK_NEAR(first.z,1010,1e-5);
    r.c.setAircraftPosition(40,-89.998,1000,r.t,90);r.advance(.02);
    const auto next=r.backend.submitted.back();
    CHECK_NEAR(next.x,first.x,1e-10);CHECK_NEAR(next.y,first.y,1e-10);CHECK_NEAR(next.z,first.z,1e-10);
    CHECK(std::abs(fscs::core::wrapDeg(next.headingDeg-first.headingDeg))>45);
    CHECK(!next.trackUserAircraft);
}

TEST_CASE("walkaround: ordered exterior views auto advance dwell after transition and stop at end") {
    Rig r;for(auto& cam:r.profile["cameras"]) {cam["mode"]="aircraftExterior";cam["transition"]["kind"]="cut";}
    r.connect();const std::string category=r.profile["categories"][0]["id"];
    CHECK_EQ(r.send("StartWalkaround",{{"categoryId",category},{"intervalSeconds",1},{"loop",false}})["status"].get<std::string>(),std::string("Accepted"));
    r.grant();r.advance(.02,40);CHECK_EQ(r.c.activeCameraId(),r.pilot);
    r.advance(.02,12);CHECK_EQ(r.c.activeCameraId(),r.pfd);
    r.advance(.02,55);CHECK_EQ(r.c.activeCameraId(),r.pfd);
    bool stopped=false;for(const auto& [name,p]:r.events)if(name=="WalkaroundChanged")stopped=!p["running"].get<bool>();
    CHECK(stopped);
}

TEST_CASE("walkaround: manual motion resets dwell and explicit recall cancels sequence") {
    Rig r;for(auto& cam:r.profile["cameras"]) {cam["mode"]="aircraftExterior";cam["transition"]["kind"]="cut";}
    r.connect();const std::string category=r.profile["categories"][0]["id"];
    r.send("StartWalkaround",{{"categoryId",category},{"intervalSeconds",1},{"loop",true}});r.grant();
    fscs::core::ManualInput input;input.moveForward=1;r.c.setManualInput(input);r.advance(.02,110);
    CHECK_EQ(r.c.activeCameraId(),r.pilot);
    r.c.setManualInput({});r.advance(.02,30);CHECK_EQ(r.c.activeCameraId(),r.pilot);
    r.advance(.02,25);CHECK_EQ(r.c.activeCameraId(),r.pfd);
    r.activate(r.pilot);r.advance(.02,100);CHECK_EQ(r.c.activeCameraId(),r.pilot);
}

TEST_CASE("walkaround: loops and ignores background airport recall until stopped") {
    Rig r;for(auto& cam:r.profile["cameras"]) {cam["mode"]="aircraftExterior";cam["transition"]["kind"]="cut";}
    r.connect();const std::string category=r.profile["categories"][0]["id"];
    r.send("StartWalkaround",{{"categoryId",category},{"intervalSeconds",1},{"loop",true}});r.grant();
    CHECK(!r.c.selectAutomaticCamera(r.pfd,r.t));
    r.advance(.02,103);CHECK_EQ(r.c.activeCameraId(),r.pilot);
    CHECK_EQ(r.send("StopWalkaround")["status"].get<std::string>(),std::string("Completed"));
    r.advance(.02,100);CHECK_EQ(r.c.activeCameraId(),r.pilot);
    CHECK(r.c.selectAutomaticCamera(r.pfd,r.t));
}

TEST_CASE("motion views: orbit zoom survives animation and unrelated profile updates") {
    Rig r;r.profile["cameras"][0]["mode"]="orbit";r.profile["cameras"][0]["transition"]["kind"]="cut";
    r.connect();r.activate(r.pilot);r.grant();r.advance(.02,30);
    const auto before=r.c.engine().sample().pose;
    fscs::core::ManualInput input;input.zoomIn=1;r.c.setManualInput(input);r.advance(.02,10);r.c.setManualInput({});
    const auto zoomed=r.c.engine().sample().pose;
    r.profile["name"]="Renamed profile";r.setProfile();r.advance(.02,20);
    const auto after=r.c.engine().sample().pose;
    CHECK(after.fovRad < before.fovRad);
    CHECK(after.fovRad <= zoomed.fovRad);
    CHECK((after.positionFRD-before.positionFRD).length()>1);
}

TEST_CASE("tracking: shared aircraft preference applies to exterior views and never cockpit") {
    Rig r;for(auto& cam:r.profile["cameras"]) {cam["mode"]="aircraftExterior";cam["transition"]["kind"]="cut";cam["positionMetersFRD"]={20,20,-10};cam["orientationCameraToAnchorXYZW"]={0,0,0,1};}
    r.connect();r.activate(r.pilot);r.grant();r.advance(.02);
    auto tracked=fromSdk(r.backend.submitted.back(),r.qual.contract).orientation.toPbh();
    CHECK_NEAR(tracked.headingDeg,-135,1e-7);CHECK(tracked.pitchDeg< -10);
    fscs::profiles::HostSettings settings;settings.globalAircraftTracking="off";r.c.setSettings(settings,r.t);r.advance(.02);
    auto fixed=fromSdk(r.backend.submitted.back(),r.qual.contract).orientation.toPbh();CHECK_NEAR(fixed.headingDeg,0,1e-7);CHECK_NEAR(fixed.pitchDeg,0,1e-7);
    r.activate(r.pfd);r.advance(.02);fixed=fromSdk(r.backend.submitted.back(),r.qual.contract).orientation.toPbh();CHECK_NEAR(fixed.headingDeg,0,1e-7);
    settings.globalAircraftTracking="heading";r.c.setSettings(settings,r.t);r.advance(.02);
    auto heading=fromSdk(r.backend.submitted.back(),r.qual.contract).orientation.toPbh();CHECK_NEAR(heading.headingDeg,-135,1e-7);CHECK_NEAR(heading.pitchDeg,0,1e-7);
    r.profile["cameras"][1]["mode"]="aircraftCockpit";r.setProfile();r.advance(.02);
    settings.globalAircraftTracking="both";r.c.setSettings(settings,r.t);r.advance(.02);
    const auto cockpit=fromSdk(r.backend.submitted.back(),r.qual.contract).orientation.toPbh();CHECK_NEAR(cockpit.headingDeg,0,1e-7);CHECK_NEAR(cockpit.pitchDeg,0,1e-7);
}

TEST_CASE("tracking: airport and flyby use shared mode despite legacy camera overrides") {
    Rig r;std::vector<std::string> notes;auto airport=fscs::profiles::airport::generate(airportFixture(),json::object(),notes);
    auto cam=airport["cameras"][0];cam["positionMetersFRD"]={0,0,-3};cam["orientationCameraToAnchorXYZW"]={0,0,0,1};cam["transition"]["kind"]="cut";cam["aircraftTracking"]="off";
    auto cat=airport["categories"][0];cat["cameraIds"]=json::array({cam["id"]});cat["defaultCameraId"]=cam["id"];r.profile["categories"].push_back(cat);r.profile["cameras"].push_back(cam);
    r.connect();r.c.setAircraftPosition(40,-89.999,150,r.t);r.activate(cam["id"]);r.grant();r.advance(.02);
    CHECK_NEAR(r.backend.submitted.back().headingDeg,90,.1);CHECK(r.backend.submitted.back().pitchDeg<0);
    fscs::profiles::HostSettings settings;settings.globalAircraftTracking="off";r.c.setSettings(settings,r.t);r.advance(.02);
    CHECK_NEAR(r.backend.submitted.back().headingDeg,0,1e-7);CHECK_NEAR(r.backend.submitted.back().pitchDeg,0,1e-7);
    r.profile["cameras"][0]["mode"]="flyby";r.profile["cameras"][0]["transition"]["kind"]="cut";r.setProfile();r.activate(r.pilot);r.advance(.02);
    const auto before=r.backend.submitted.back();r.c.setAircraftPosition(40,-89.997,160,r.t);r.advance(.02);
    CHECK_NEAR(r.backend.submitted.back().headingDeg,before.headingDeg,1e-7);CHECK_NEAR(r.backend.submitted.back().pitchDeg,before.pitchDeg,1e-7);
    settings.globalAircraftTracking="both";r.c.setSettings(settings,r.t);r.advance(.02);
    CHECK(std::abs(r.backend.submitted.back().headingDeg-before.headingDeg)>1);
}

TEST_CASE("motion views: flyby animates parked pass with side height duration and finite end") {
    for (const double duration : {5.,10.}) {
        Rig r;auto& cam=r.profile["cameras"][0];cam["mode"]="flyby";cam["transition"]["kind"]="cut";
        cam["motion"]={{"radiusMeters",100},{"heightMeters",20},{"durationSeconds",duration},{"sideMeters",-30},{"repeat",false}};
        r.connect();r.c.setAircraftPosition(40,-90,1000,r.t,0);r.activate(r.pilot);r.grant();
        r.advance(.02,static_cast<int>(duration/.04));
        auto middle=r.c.engine().sample().pose.positionFRD;
        CHECK_NEAR(middle.x,0,1e-7);CHECK_NEAR(middle.y,-30,1e-7);CHECK_NEAR(middle.z,-20,1e-7);
        const auto world=r.backend.submitted.back();CHECK(world.referential==Referential::World);CHECK_NEAR(world.z,1020,1e-5);
        CHECK(std::abs(world.headingDeg)>80); // aim back at stationary aircraft from closest approach
        r.advance(.02,static_cast<int>(duration/.04)+100);
        CHECK_NEAR(r.c.engine().sample().pose.positionFRD.x,-100,1e-7);
        const auto end=r.c.engine().sample().pose.positionFRD;r.advance(.02,200);
        CHECK_NEAR((r.c.engine().sample().pose.positionFRD-end).length(),0,1e-9);
        r.c.setAircraftPosition(40,-90,1000,r.t,0);r.activate(r.pilot);r.advance(.02);
        CHECK(r.c.engine().sample().pose.positionFRD.x>99.9); // recall starts another pass
    }
}

TEST_CASE("motion views: repeated flyby reverses continuously and pauses for manual input or simulation pause") {
    Rig r;auto& cam=r.profile["cameras"][0];cam["mode"]="flyby";cam["transition"]["kind"]="cut";
    cam["motion"]={{"radiusMeters",50},{"heightMeters",10},{"durationSeconds",5},{"sideMeters",20},{"repeat",true}};
    r.connect();r.c.setAircraftPosition(40,-90,1000,r.t,0);r.activate(r.pilot);r.grant();r.advance(.02,250);
    const auto end=r.c.engine().basePose()->positionFRD;CHECK_NEAR(end.x,-50,1e-7);
    r.advance(.02);const auto reverse=r.c.engine().basePose()->positionFRD;
    CHECK((reverse-end).length()<.01);CHECK(reverse.x>end.x);
    r.deliver(be::Paused{true});r.advance(.02,40);CHECK_NEAR((r.c.engine().basePose()->positionFRD-reverse).length(),0,1e-9);
    r.deliver(be::Paused{false});fscs::core::ManualInput input;input.lookRight=1;r.c.setManualInput(input);r.advance(.02,40);
    CHECK_NEAR((r.c.engine().basePose()->positionFRD-reverse).length(),0,1e-9);
    input={};input.moveUp=1;r.c.setManualInput(input);r.advance(.02,40);
    CHECK_NEAR((r.c.engine().basePose()->positionFRD-reverse).length(),0,1e-9);
    r.c.setManualInput({});r.advance(.02,249);CHECK_NEAR(r.c.engine().basePose()->positionFRD.x,50,1e-7);
}

namespace {
std::string addAutoZoomAirport(Rig& r) {
    std::vector<std::string> notes;
    auto airport=fscs::profiles::airport::generate(airportFixture(),json::object(),notes);
    auto camera=airport["cameras"][0];
    camera["positionMetersFRD"]={0,0,0}; camera.erase("airportHeight");
    camera["orientationCameraToAnchorXYZW"]={0,0,0,1};camera["transition"]["kind"]="cut";
    auto category=airport["categories"][0];category["cameraIds"]=json::array({camera["id"]});category["defaultCameraId"]=camera["id"];
    r.profile["categories"].push_back(category);r.profile["cameras"].push_back(camera);
    return camera["id"].get<std::string>();
}
void autoZoomTarget(Rig& r,double distance) {
    const auto target=fscs::profiles::airport::world({40,-90,100},{distance,0,0});
    r.c.setAircraftPosition(target.lat,target.lon,target.altitude,r.t);
}
}
TEST_CASE("airports: autozoom follows distance smoothly without modifying saved or manual FOV") {
    Rig r;const auto id=addAutoZoomAirport(r);r.connect();
    fscs::profiles::HostSettings settings;settings.airportAutoZoomEnabled=true;settings.airportAutoZoomResponseSeconds=.1;
    r.c.setSettings(settings,r.t);autoZoomTarget(r,100);r.activate(id);r.grant();
    const auto manual=r.c.engine().sample().pose.fovRad;
    r.advance(.02);const double first=r.backend.submitted.back().fovRad;
    const double target=2*std::atan2(40.,200.);
    CHECK(first<manual);CHECK(first>target);r.advance(.02,100);
    CHECK_NEAR(r.backend.submitted.back().fovRad,target,1e-5);
    CHECK_NEAR(r.c.engine().sample().pose.fovRad,manual,1e-12);
    CHECK_NEAR(r.c.profile()->camera(id)->pose.fovRad,manual,1e-12);
    autoZoomTarget(r,10000);r.advance(.02,100);CHECK_NEAR(r.backend.submitted.back().fovRad,10*fscs::core::kDegToRad,1e-6);
    autoZoomTarget(r,1);r.advance(.02,100);CHECK_NEAR(r.backend.submitted.back().fovRad,90*fscs::core::kDegToRad,1e-6);
    settings.airportAutoZoomEnabled=false;r.c.setSettings(settings,r.t);r.advance(.02);
    CHECK_NEAR(r.backend.submitted.back().fovRad,manual,1e-12);
}
TEST_CASE("airports: autozoom respects pause mouse override tracking and camera activation") {
    Rig r;const auto id=addAutoZoomAirport(r);r.connect();fscs::profiles::HostSettings settings;settings.airportAutoZoomEnabled=true;
    r.c.setSettings(settings,r.t);autoZoomTarget(r,100);r.activate(id);r.grant();r.advance(.02,50);
    const auto held=r.backend.submitted.back().fovRad;r.deliver(be::Paused{true});autoZoomTarget(r,1000);r.advance(.02,50);
    CHECK_NEAR(r.backend.submitted.back().fovRad,held,1e-12);r.deliver(be::Paused{false});r.advance(.02);CHECK(r.backend.submitted.back().fovRad<held);
    r.deliver(be::NativeMouseMapping{true});r.deliver(be::NativeMouseButton{true});r.advance(.02);
    CHECK_NEAR(r.backend.submitted.back().fovRad,r.c.engine().sample().pose.fovRad,1e-12);
    r.deliver(be::NativeMouseButton{false});r.advance(.02);CHECK(r.backend.submitted.back().fovRad<r.c.engine().sample().pose.fovRad);
    settings.globalAircraftTracking="off";r.c.setSettings(settings,r.t);r.advance(.02);
    CHECK_NEAR(r.backend.submitted.back().fovRad,r.c.engine().sample().pose.fovRad,1e-12);
    settings.globalAircraftTracking="both";r.c.setSettings(settings,r.t);r.activate(r.pilot);r.advance(.02);
    CHECK_NEAR(r.backend.submitted.back().fovRad,r.c.engine().sample().pose.fovRad,1e-12);
    r.activate(id);r.c.setAircraftPosition(NAN,-90,100,r.t);r.advance(.02);
    CHECK_NEAR(r.backend.submitted.back().fovRad,r.c.engine().sample().pose.fovRad,1e-12);
}
TEST_CASE("airports: tracking and zoom use terrain-adjusted camera altitude without saving it") {
    Rig r;const auto id=addAutoZoomAirport(r);r.connect();
    fscs::profiles::HostSettings settings;settings.airportTrackingHorizonGuard=false;
    settings.airportAutoZoomEnabled=true;settings.airportAutoZoomResponseSeconds=.1;
    r.c.setSettings(settings,r.t);autoZoomTarget(r,100);r.activate(id);r.grant();
    // Requested MSL 100, but terrain/building protection puts the camera at 180.
    r.backend.actualWorldPosition=fscs::core::Vec3{40,-90,180};r.advance(.02,100);
    CHECK_NEAR(r.backend.submitted.back().pitchDeg,std::atan2(80.,100.)*fscs::core::kRadToDeg,1e-6);
    CHECK_NEAR(r.backend.submitted.back().fovRad,2*std::atan2(40.,2*std::hypot(100.,80.)),1e-5);
    CHECK_NEAR(r.backend.submitted.back().z,100,1e-9);
    CHECK_NEAR(r.c.engine().sample().pose.positionFRD.z,0,1e-9);
    // Readback from another location or a failed SDK read must not contaminate aiming.
    r.backend.actualWorldPosition=fscs::core::Vec3{41,-90,180};r.advance(.02);
    CHECK_NEAR(r.backend.submitted.back().pitchDeg,0,1e-9);
    r.backend.actualWorldPosition=fscs::core::Vec3{40,-90,NAN};r.advance(.02);
    CHECK_NEAR(r.backend.submitted.back().pitchDeg,0,1e-9);
    r.backend.actualWorldPosition.reset();r.advance(.02);
    CHECK_NEAR(r.backend.submitted.back().pitchDeg,0,1e-9);
}
TEST_CASE("airports: distant lower targets keep heading and level pitch with an optional override") {
    Rig r;const auto id=addAutoZoomAirport(r);r.connect();
    autoZoomTarget(r,2000);r.activate(id);r.grant();
    r.backend.actualWorldPosition=fscs::core::Vec3{40,-90,180};r.advance(.02);
    CHECK_NEAR(r.backend.submitted.back().pitchDeg,0,1e-9);
    CHECK_NEAR(r.backend.submitted.back().headingDeg,0,1e-9);
    autoZoomTarget(r,250);r.advance(.02);
    CHECK_NEAR(r.backend.submitted.back().pitchDeg,std::atan2(80.,250.)*fscs::core::kRadToDeg,1e-6);
    autoZoomTarget(r,1000);r.advance(.02);
    CHECK_NEAR(r.backend.submitted.back().pitchDeg,.5*std::atan2(80.,1000.)*fscs::core::kRadToDeg,1e-6);
    autoZoomTarget(r,2000);
    r.profile["cameras"].back()["airportOverrides"]["trackingHorizonGuard"]=false;
    r.setProfile();r.advance(.02);
    CHECK_NEAR(r.backend.submitted.back().pitchDeg,std::atan2(80.,2000.)*fscs::core::kRadToDeg,1e-6);
    // Upward aim to an aircraft above the camera is never flattened.
    r.profile["cameras"].back()["airportOverrides"]["trackingHorizonGuard"]=true;r.setProfile();
    const auto target=fscs::profiles::airport::world({40,-90,100},{2000,0,-160});
    r.c.setAircraftPosition(target.lat,target.lon,target.altitude,r.t);r.advance(.02);
    CHECK_NEAR(r.backend.submitted.back().pitchDeg,-std::atan2(80.,2000.)*fscs::core::kRadToDeg,1e-6);
}
TEST_CASE("airports: terrain tracking stays continuous while moving and resets across camera changes") {
    Rig r;const auto id=addAutoZoomAirport(r);r.connect();autoZoomTarget(r,100);r.activate(id);r.grant();r.advance(.02);
    auto input=routerWithDefaults();
    input.onInput(indexOf(input,"Q"),true,0);r.c.setManualInput(input.manualInput());
    for (int i=0;i<25;++i) {
        const auto previous=r.backend.submitted.back();
        r.backend.actualWorldPosition=fscs::core::Vec3{previous.x,previous.y,180};
        r.advance(.02);
        CHECK_NEAR(r.backend.submitted.back().pitchDeg,std::atan2(80.,100.)*fscs::core::kRadToDeg,1e-6);
        CHECK(r.backend.submitted.back().z>previous.z);
    }
    input.onInput(indexOf(input,"Q"),false,0);
    input.onInput(indexOf(input,"W"),true,0);r.c.setManualInput(input.manualInput());
    for (int i=0;i<25;++i) {
        const auto previous=r.backend.submitted.back();
        r.backend.actualWorldPosition=fscs::core::Vec3{previous.x,previous.y,180};
        r.advance(.02);CHECK(r.backend.submitted.back().pitchDeg>38);
    }
    r.c.setManualInput({});r.activate(r.pilot);r.advance(.02);
    CHECK(r.backend.submitted.back().referential!=fscs::host::Referential::World);
    r.backend.actualWorldPosition=fscs::core::Vec3{41,-90,500};
    r.activate(id);r.advance(.02);
    CHECK(std::abs(r.backend.submitted.back().pitchDeg)<1);
}
TEST_CASE("airports: per-camera tracking and autozoom overrides inherit independently") {
    Rig r;const auto id=addAutoZoomAirport(r);auto& camera=r.profile["cameras"].back();
    camera["airportOverrides"]={{"aircraftTracking","both"},{"autoZoomEnabled",true},{"autoZoomFrameMeters",20},{"autoZoomMinFovDeg",20},{"autoZoomMaxFovDeg",30},{"autoZoomResponseSeconds",.1}};
    r.connect();fscs::profiles::HostSettings settings;settings.globalAircraftTracking="off";r.c.setSettings(settings,r.t);
    r.c.setAircraftPosition(40,-89.999,120,r.t);r.activate(id);r.grant();r.advance(.02,100);
    CHECK_NEAR(r.backend.submitted.back().headingDeg,90,.1);CHECK(r.backend.submitted.back().pitchDeg<0);
    CHECK_NEAR(r.backend.submitted.back().fovRad,20*fscs::core::kDegToRad,1e-6);
    camera["airportOverrides"]["autoZoomEnabled"]=false;r.setProfile();r.advance(.02);
    CHECK_NEAR(r.backend.submitted.back().fovRad,r.c.engine().sample().pose.fovRad,1e-12);
    camera["airportOverrides"]["aircraftTracking"]="off";settings.globalAircraftTracking="both";settings.airportAutoZoomEnabled=true;r.c.setSettings(settings,r.t);r.setProfile();r.advance(.02);
    CHECK_NEAR(r.backend.submitted.back().headingDeg,0,1e-9);CHECK_NEAR(r.backend.submitted.back().pitchDeg,0,1e-9);
    camera.erase("airportOverrides");r.setProfile();r.advance(.02);
    CHECK_NEAR(r.backend.submitted.back().headingDeg,90,.1);CHECK(r.backend.submitted.back().fovRad<r.c.engine().sample().pose.fovRad);
}
TEST_CASE("airports: global category heights preserve position offsets and allow exact camera override") {
    Rig r;const auto id=addAutoZoomAirport(r);auto& camera=r.profile["cameras"].back();
    camera["airportHeight"]={{"category","runways"},{"baseAltitudeMeters",100},{"minimumMeters",15}};
    r.connect();r.activate(id);r.grant();r.advance(.02);CHECK_NEAR(r.backend.submitted.back().z,115,1e-9);
    fscs::profiles::HostSettings settings;settings.airportCategoryHeights["runways"]=40;r.c.setSettings(settings,r.t);r.advance(.02);
    CHECK_NEAR(r.backend.submitted.back().z,140,1e-9);
    camera["airportOverrides"]["heightMeters"]=5;r.setProfile();r.advance(.02);CHECK_NEAR(r.backend.submitted.back().z,105,1e-9);
    settings.airportCategoryHeights["runways"]=80;r.c.setSettings(settings,r.t);r.advance(.02);CHECK_NEAR(r.backend.submitted.back().z,105,1e-9);
    camera["airportOverrides"].erase("heightMeters");r.setProfile();r.advance(.02);CHECK_NEAR(r.backend.submitted.back().z,180,1e-9);
}
