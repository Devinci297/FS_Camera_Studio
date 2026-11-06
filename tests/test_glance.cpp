// Smart views (glance): hold to glance at the nearest instrument view, step through glance views, and return to
// the exact previous view and temporary look. Release Camera and Panic cancel without a return.
#include "test.hpp"

#include "coordinator.hpp"
#include "fake_backend.hpp"
#include "input_router.hpp"
#include "module_app.hpp"
#include "fscs/profiles/json_util.hpp"

#include <algorithm>
#include <map>
#include <set>
#include <stdexcept>

using namespace fscs::host;
using fscs::profiles::Action;
using fscs::profiles::defaultGlance;
using fscs::profiles::makeStarterProfile;
using fscs::profiles::newUuid;
using fscs::profiles::toSnapshot;
using fscs::profiles::validateProfile;
using fscs::protocol::Command;
using fscs::protocol::json;
namespace core = fscs::core;
namespace be = backend_event;

namespace {

json quat(double pitch, double heading) {
    const auto q = core::Quat::fromPbh({pitch, 0, heading});
    return {q.x, q.y, q.z, q.w};
}

struct GlanceRig {
    FakeBackend backend;
    std::vector<std::pair<std::string, json>> events;
    Qualification qual;
    Coordinator c{backend, [this](const std::string& n, json p) { events.emplace_back(n, std::move(p)); },
                  [this](const std::string&) { return qual; }};
    double t = 0;
    int req = 0;
    json profile;
    std::string pilot, side, pfd, mfd, standby, radios, exterior, instrumentCat;

    GlanceRig() {
        qual.adapterQualified = true;
        profile = makeStarterProfile("Glance", "asobo-c172", "C172");
        pilot = profile["cameras"][0]["id"];
        side = add(profile["categories"][0]["id"], "Pilot - side", {0, 0, 0}, 0, -60);
        instrumentCat = newUuid();
        profile["categories"].push_back({{"id", instrumentCat}, {"name", "Instruments"}, {"cameraIds", json::array()}});
        pfd = add(instrumentCat, "G1000 - PFD", {0.38, 0.02, 0.08}, -12, 0);
        mfd = add(instrumentCat, "G1000 - MFD", {0.38, 0.38, 0.08}, -12, 0);
        standby = add(instrumentCat, "Standby instruments", {0.30, 0.16, 0.18}, -22, 0);
        radios = add(instrumentCat, "Radios", {0.3, 0.3, 0.1}, -20, 10);
        cam(radios)["glance"] = false;  // switched off by the user
        exterior = add(instrumentCat, "Exterior instrument shot", {3, 0, -1}, 0, 180);
        cam(exterior)["mode"] = "aircraftExterior";
        cam(exterior)["glance"] = true;  // never offered: not a cockpit view
    }
    json& cam(const std::string& id) {
        for (auto& v : profile["cameras"])
            if (v["id"] == id) return v;
        throw std::runtime_error("no camera");
    }
    std::string add(const std::string& category, const char* name, json position, double pitch, double heading) {
        json v = profile["cameras"][0];
        v["id"] = newUuid();
        v["name"] = name;
        v.erase("glance");
        v.erase("icon");  // automatic icon: the name decides
        v["viewId"] = static_cast<int>(profile["cameras"].size()) + 1;
        v["categoryId"] = category;
        v["positionMetersFRD"] = position;
        v["orientationCameraToAnchorXYZW"] = quat(pitch, heading);
        profile["cameras"].push_back(v);
        for (auto& g : profile["categories"])
            if (g["id"] == category) g["cameraIds"].push_back(v["id"]);
        return v["id"];
    }
    json send(const std::string& action, json payload = json::object()) {
        Command cmd;
        cmd.requestId = "g-" + std::to_string(++req);
        cmd.sessionEpoch = c.epoch();
        cmd.action = action;
        cmd.payload = std::move(payload);
        return c.handle(cmd, t);
    }
    void advance(double seconds) {
        const int steps = static_cast<int>(std::lround(seconds * 100));
        for (int i = 0; i < steps; ++i) {
            t += 0.01;
            c.tick(t);
        }
    }
    void setProfile() {
        json copy = profile;
        const auto v = validateProfile(copy);
        if (!v.ok()) std::printf("Validation: %s\n", v.summary().c_str());
        CHECK(v.ok());
        c.setProfile(toSnapshot(copy), t);
    }
    void connect() {
        c.tick(t);
        c.onBackendEvent(be::Connected{"1.8.16.0"}, t);
        c.onBackendEvent(be::AircraftLoaded{"SimObjects\\Airplanes\\Asobo-C172\\presets\\classic\\aircraft.cfg"}, t);
        c.onBackendEvent(be::CameraStatus{SdkAvailability::NotAcquired, false}, t);
        setProfile();
    }
    void show(const std::string& id) {
        CHECK_EQ(send("ActivateCamera", {{"cameraId", id}})["status"].get<std::string>(), std::string("Accepted"));
        if (c.ownership() != Ownership::Active) c.onBackendEvent(be::CameraStatus{SdkAvailability::Acquired, false}, t);
        advance(2);
        CHECK(c.ownership() == Ownership::Active);
        CHECK_EQ(c.activeCameraId(), id);
    }
    void action(Action a, bool pressed, std::uint64_t hold = 1) { c.onAction({a, 0, pressed, hold}, t); }
    std::string lastResult() const {
        for (auto it = events.rbegin(); it != events.rend(); ++it)
            if (it->first == "BindingTriggered") return it->second["result"].is_string() ? it->second["result"].get<std::string>() : "";
        return "<none>";
    }
    bool emitted(const std::string& name) const {
        for (const auto& e : events)
            if (e.first == name) return true;
        return false;
    }
    const fscs::profiles::CameraEntry& entry(const std::string& id) const { return *c.profile()->camera(id); }
};

void checkPose(const core::CameraPose& a, const core::CameraPose& b, double eps = 1e-9) {
    CHECK_NEAR(a.positionFRD.x, b.positionFRD.x, eps);
    CHECK_NEAR(a.positionFRD.y, b.positionFRD.y, eps);
    CHECK_NEAR(a.positionFRD.z, b.positionFRD.z, eps);
    CHECK_NEAR(core::angleBetween(a.orientation, b.orientation), 0.0, 1e-6);
    CHECK_NEAR(a.fovRad, b.fovRad, eps);
}

core::CameraPose eyeLooking(double pitch, double heading) {
    core::CameraPose p;
    p.orientation = core::Quat::fromPbh({pitch, 0, heading});
    return p;
}

}  // namespace

TEST_CASE("glance: the default flag comes from the instrument icon or, for an automatic icon, the name") {
    const auto make = [](const char* name, const char* icon = nullptr, const char* mode = "aircraftCockpit") {
        json c{{"name", name}, {"mode", mode}};
        if (icon) c["icon"] = icon;
        return c;
    };
    CHECK(defaultGlance(make("G1000 - PFD")));
    CHECK(defaultGlance(make("Autopilot panel")));
    CHECK(defaultGlance(make("Radio stack")));
    CHECK(defaultGlance(make("Camera 3", "view-instrument")));
    CHECK(!defaultGlance(make("PFD", "camera")));  // a chosen icon decides
    CHECK(!defaultGlance(make("Pilot view", "view-cockpit")));
    CHECK(!defaultGlance(make("Pilot view")));
    CHECK(!defaultGlance(make("Tower radio")));  // airport words come first, as in the editor
    CHECK(!defaultGlance(make("Instrument close-up", "view-instrument", "aircraftExterior")));

    // Older profiles have no flag: validation derives it and the snapshot follows; explicit values are kept.
    GlanceRig r;
    r.cam(r.mfd)["glance"] = false;
    json doc = r.profile;
    const auto v = validateProfile(doc);
    CHECK(v.ok());
    CHECK(!v.migrated);
    CHECK_EQ(doc["schemaVersion"].get<int>(), fscs::profiles::kSchemaVersion);
    for (const auto& c : doc["cameras"]) {
        CHECK(c["glance"].is_boolean());
        const auto id = c["id"].get<std::string>();
        const bool expected = id == r.pfd || id == r.standby || id == r.exterior;
        CHECK_EQ(c["glance"].get<bool>(), expected);
    }
    const auto snapshot = toSnapshot(doc);
    CHECK(snapshot.camera(r.pfd)->glance);
    CHECK(!snapshot.camera(r.mfd)->glance);
    CHECK(!snapshot.camera(r.exterior)->glance);  // stored, but only cockpit views can be glanced at
    CHECK(!snapshot.camera(r.pilot)->glance);

    json bad = r.profile;
    bad["cameras"][1]["glance"] = "yes";
    CHECK(!validateProfile(bad).ok());

    // The C172 starter's instrument views become glance views; flight deck, controls and exterior do not.
    json c172 = fscs::profiles::makeC172Profile("asobo-c172", "C172");
    CHECK(validateProfile(c172).ok());
    int glances = 0;
    for (const auto& c : c172["cameras"]) {
        const bool g = c["glance"].get<bool>();
        glances += g;
        if (c["name"] == "G1000 - PFD" || c["name"] == "Standby instruments") CHECK(g);
        if (c["name"] == "Pilot - normal" || c["name"] == "Throttle, mixture and trim" || c["name"] == "Left wing") CHECK(!g);
    }
    CHECK_EQ(glances, 4);
}

TEST_CASE("glance: nearest view follows the look direction, excluding non-cockpit and switched-off views") {
    GlanceRig r;
    json doc = r.profile;
    CHECK(validateProfile(doc).ok());
    const auto p = toSnapshot(doc);
    const auto views = glanceViews(p, r.pfd);
    CHECK_EQ(views.size(), std::size_t{2});  // MFD and standby; PFD excluded, radios off, exterior not cockpit
    CHECK_EQ(views[0]->id, r.mfd);
    CHECK_EQ(views[1]->id, r.standby);

    CHECK_EQ(nearestGlanceView(p, eyeLooking(0, 0), r.pilot)->id, r.pfd);
    CHECK_EQ(nearestGlanceView(p, eyeLooking(-12, 22), r.pilot)->id, r.mfd);
    CHECK_EQ(nearestGlanceView(p, eyeLooking(-40, 9), r.pilot)->id, r.standby);
    // Looking straight at the exterior camera's subject still never picks it.
    CHECK(nearestGlanceView(p, eyeLooking(24, 0), r.pilot)->id != r.exterior);
    // The glanced-from view is never offered, even when it is the nearest.
    CHECK_EQ(nearestGlanceView(p, eyeLooking(0, 0), r.pfd)->id, r.mfd);
    // The angle is measured from the eyepoint actually shown, temporary translation included.
    core::CameraPose leaning = eyeLooking(-12, 0);
    leaning.positionFRD = {0, 0.38, 0};
    CHECK_EQ(nearestGlanceView(p, leaning, r.pilot)->id, r.mfd);
    CHECK_NEAR(glanceAngle(leaning, *p.camera(r.mfd)), 0.0, 0.01);

    // Tie on the instrument direction: the camera that itself points closest to the look wins, then category order.
    GlanceRig t;
    const auto ray = core::Quat::fromPbh({-10, 0, 0}).rotate({1, 0, 0});
    const auto steepAim = core::Quat::fromPbh({-30, 0, 0}).rotate({1, 0, 0});
    const auto steepPos = ray * 1.0 - steepAim * kGlanceFocusMeters;
    const auto steep = t.add(t.instrumentCat, "Steep panel view", {steepPos.x, steepPos.y, steepPos.z}, -30, 0);
    const auto level = t.add(t.instrumentCat, "Level panel view", {0, 0, 0}, -10, 0);
    const auto again = t.add(t.instrumentCat, "Level panel view 2", {0, 0, 0}, -10, 0);
    for (const auto& id : {t.pfd, t.mfd, t.standby}) t.cam(id)["glance"] = false;
    for (const auto& id : {steep, level, again}) t.cam(id)["glance"] = true;
    json tied = t.profile;
    CHECK(validateProfile(tied).ok());
    const auto tp = toSnapshot(tied);
    CHECK_NEAR(glanceAngle(eyeLooking(0, 0), *tp.camera(steep)), glanceAngle(eyeLooking(0, 0), *tp.camera(level)), 1e-9);
    CHECK_EQ(nearestGlanceView(tp, eyeLooking(0, 0), t.pilot)->id, level);
    CHECK(nearestGlanceView(tp, eyeLooking(0, 0), t.side) != nullptr);
}

TEST_CASE("glance: hold glances at the nearest instrument and release returns to the exact view and look") {
    GlanceRig r;
    r.connect();
    r.show(r.pilot);
    // Temporary mouse look toward the MFD.
    CHECK_EQ(r.send("MouseLookDelta", {{"headingDeg", 22}, {"pitchDeg", -9}})["status"].get<std::string>(), std::string("Completed"));
    r.advance(0.1);
    const auto look = r.c.engine().look();
    const auto before = r.c.engine().sample().pose;
    const auto saved = r.entry(r.pilot).pose;
    r.events.clear();
    r.action(Action::GlanceHold, true, 7);
    CHECK_EQ(r.c.activeCameraId(), r.mfd);
    CHECK(r.c.glanceJson()["held"].get<bool>());
    CHECK_EQ(r.c.glanceJson()["returnCameraId"].get<std::string>(), r.pilot);
    CHECK(r.c.engine().transitioning());
    checkPose(r.c.engine().sample().pose, before);  // starts from what is on screen: no jump
    r.advance(0.3);
    CHECK(r.c.engine().transitioning());
    r.advance(0.06);
    CHECK(!r.c.engine().transitioning());  // quick: 0.35 s
    checkPose(*r.c.engine().basePose(), r.entry(r.mfd).pose);
    CHECK(!r.c.engine().look().active());  // glance views open at their saved pose
    const auto glanced = r.c.engine().sample().pose;

    r.action(Action::GlanceHold, false, 7);
    CHECK_EQ(r.c.activeCameraId(), r.pilot);
    CHECK(r.c.glanceJson().is_null());
    checkPose(r.c.engine().sample().pose, glanced);  // returns smoothly from the glance view
    r.advance(0.2);
    CHECK(r.c.engine().transitioning());
    r.advance(0.2);
    CHECK(!r.c.engine().transitioning());
    // Exactly the previous view: the saved pose as base and the same temporary look.
    checkPose(*r.c.engine().basePose(), saved);
    CHECK_NEAR(r.c.engine().look().headingDeg, look.headingDeg, 1e-12);
    CHECK_NEAR(r.c.engine().look().pitchDeg, look.pitchDeg, 1e-12);
    checkPose(r.c.engine().sample().pose, before);
    // Saved poses never change.
    checkPose(r.entry(r.pilot).pose, saved);
    // Recentering afterwards returns to the saved pose, so the look really is the temporary look again.
    r.send("RecenterLook");
    checkPose(r.c.engine().sample().pose, saved);
    CHECK(r.emitted("ActiveCameraChanged"));

    // Without mouse look, the default forward view glances at the PFD.
    r.action(Action::GlanceHold, true, 8);
    CHECK_EQ(r.c.activeCameraId(), r.pfd);
    r.advance(0.1);
    r.action(Action::GlanceHold, false, 8);  // released mid-transition
    r.advance(0.5);
    CHECK_EQ(r.c.activeCameraId(), r.pilot);
    checkPose(r.c.engine().sample().pose, saved);
}

TEST_CASE("glance: next steps through glance views in category order and return or a recall ends it") {
    GlanceRig r;
    r.connect();
    r.show(r.pilot);
    r.send("MouseLookDelta", {{"headingDeg", -10}, {"pitchDeg", 4}});
    const auto before = r.c.engine().sample().pose;
    r.action(Action::GlanceNext, true);
    CHECK_EQ(r.c.activeCameraId(), r.pfd);
    CHECK(!r.c.glanceJson()["held"].get<bool>());
    r.action(Action::GlanceNext, true);
    CHECK_EQ(r.c.activeCameraId(), r.mfd);
    r.action(Action::GlanceNext, true);
    CHECK_EQ(r.c.activeCameraId(), r.standby);  // radios (off) and the exterior view are skipped
    r.action(Action::GlanceNext, true);
    CHECK_EQ(r.c.activeCameraId(), r.pfd);  // wraps
    CHECK_EQ(r.c.glanceJson()["returnCameraId"].get<std::string>(), r.pilot);
    r.advance(0.5);
    r.action(Action::GlanceReturn, true);
    CHECK_EQ(r.c.activeCameraId(), r.pilot);
    r.advance(0.5);
    checkPose(r.c.engine().sample().pose, before);
    r.action(Action::GlanceReturn, true);
    CHECK(r.lastResult().find("Not glancing") != std::string::npos);
    CHECK_EQ(r.c.activeCameraId(), r.pilot);

    // From a glance view shown as a normal view, cycling starts after it and never offers it.
    r.show(r.mfd);
    r.action(Action::GlanceNext, true);
    CHECK_EQ(r.c.activeCameraId(), r.standby);
    r.action(Action::GlanceNext, true);
    CHECK_EQ(r.c.activeCameraId(), r.pfd);
    r.action(Action::GlanceNext, true);
    CHECK_EQ(r.c.activeCameraId(), r.standby);

    // Recalling any normal view ends glance mode without a return.
    r.show(r.side);
    CHECK(r.c.glanceJson().is_null());
    r.action(Action::GlanceReturn, true);
    CHECK_EQ(r.c.activeCameraId(), r.side);

    // A hold that starts while cycling makes its release return; cycling during a hold keeps the hold.
    r.action(Action::GlanceNext, true);
    r.action(Action::GlanceHold, true, 11);
    r.action(Action::GlanceNext, true);
    CHECK(r.c.glanceJson()["held"].get<bool>());
    r.action(Action::GlanceHold, false, 11);
    r.advance(0.5);
    CHECK_EQ(r.c.activeCameraId(), r.side);
    // A stale release (a different hold) changes nothing.
    r.action(Action::GlanceHold, true, 12);
    r.action(Action::GlanceHold, false, 99);
    CHECK(!r.c.glanceJson().is_null());
}

TEST_CASE("glance: Release Camera and Panic cancel a glance with no return afterwards") {
    for (const bool panic : {false, true}) {
        GlanceRig r;
        r.connect();
        r.show(r.pilot);
        r.action(Action::GlanceHold, true, 3);
        r.advance(0.1);
        CHECK_EQ(r.c.activeCameraId(), r.pfd);
        if (panic) r.action(Action::PanicRelease, true, 4);
        else r.send("ReleaseCamera");
        CHECK(r.c.ownership() == Ownership::Releasing);
        CHECK(r.c.glanceJson().is_null());
        r.action(Action::GlanceHold, false, 3);  // key comes up after the release
        r.c.onBackendEvent(be::CameraStatus{SdkAvailability::NotAcquired, false}, r.t);
        r.advance(1);
        CHECK(r.c.ownership() == Ownership::Observing);
        CHECK(r.c.activeCameraId().empty());
        CHECK_EQ(r.backend.count("acquire"), std::size_t{1});

        // Showing the glance view again afterwards is a normal view: an old hold or Return never moves it.
        r.show(r.pfd);
        r.action(Action::GlanceHold, false, 3);
        r.action(Action::GlanceReturn, true);
        r.advance(1);
        CHECK_EQ(r.c.activeCameraId(), r.pfd);
        CHECK(r.c.glanceJson().is_null());
    }
    // Releasing during the return transition: nothing lands afterwards either.
    GlanceRig r;
    r.connect();
    r.show(r.pilot);
    r.action(Action::GlanceNext, true);
    r.advance(0.5);
    r.action(Action::GlanceReturn, true);
    r.advance(0.1);
    r.action(Action::PanicRelease, true, 5);
    r.c.onBackendEvent(be::CameraStatus{SdkAvailability::NotAcquired, false}, r.t);
    r.advance(1);
    CHECK(r.c.ownership() == Ownership::Observing);
    CHECK_EQ(r.backend.count("acquire"), std::size_t{1});
}

TEST_CASE("glance: needs an active cockpit view and a glance view, and reports why not") {
    GlanceRig r;
    r.connect();
    r.action(Action::GlanceHold, true, 1);
    CHECK(r.lastResult().find("cockpit view") != std::string::npos);
    r.action(Action::GlanceHold, false, 1);
    CHECK(r.c.ownership() == Ownership::Observing);
    CHECK_EQ(r.backend.count("acquire"), std::size_t{0});
    r.show(r.exterior);
    r.action(Action::GlanceNext, true);
    CHECK(r.lastResult().find("cockpit views") != std::string::npos);
    CHECK_EQ(r.c.activeCameraId(), r.exterior);

    for (const auto& id : {r.pfd, r.mfd, r.standby}) r.cam(id)["glance"] = false;
    r.setProfile();
    r.show(r.pilot);
    r.action(Action::GlanceHold, true, 2);
    CHECK(r.lastResult().find("No glance views") != std::string::npos);
    CHECK_EQ(r.c.activeCameraId(), r.pilot);
}

TEST_CASE("glance: defaults use free Ctrl+Alt letters and alternate inputs share one hold") {
    const auto doc = fscs::profiles::defaultBindingsDocument();
    std::map<std::string, std::string> byAction;
    std::set<std::string> definitions;
    for (const auto& b : doc["bindings"]) {
        const auto def = b["input"]["definition"].get<std::string>();
        CHECK(definitions.insert(def).second);  // no default input is used twice
        if (!b.contains("viewId")) byAction[b["action"].get<std::string>()] = def;
    }
    CHECK_EQ(byAction["glanceHold"], std::string("VK_LCONTROL+VK_LMENU+G"));
    CHECK_EQ(byAction["glanceNext"], std::string("VK_LCONTROL+VK_LMENU+I"));
    CHECK_EQ(byAction["glanceReturn"], std::string("VK_LCONTROL+VK_LMENU+U"));
    CHECK(fscs::profiles::parseAction("glanceHold") == Action::GlanceHold);
    CHECK_EQ(std::string(fscs::profiles::toString(Action::GlanceReturn)), std::string("glanceReturn"));

    InputRouter router;
    std::vector<fscs::profiles::Binding> list;
    for (const char* def : {"VK_LCONTROL+VK_LMENU+G", "joystick:1:button:3"}) {
        fscs::profiles::Binding b;
        b.action = Action::GlanceHold;
        b.input.definition = def;
        list.push_back(b);
    }
    router.setBindings(list);
    auto e = router.onInput(0, true, 0);
    CHECK_EQ(e.size(), std::size_t{1});
    CHECK(router.onInput(1, true, 0).empty());  // the second input joins the same hold
    CHECK(router.onInput(0, false, 0).empty());  // still held on the other input
    e = router.onInput(1, false, 0);
    CHECK_EQ(e.size(), std::size_t{1});
    CHECK(!e[0].pressed);
    // Text focus ends the hold (and so returns from the glance).
    router.onInput(0, true, 0);
    e = router.setTextFocus(true);
    CHECK_EQ(e.size(), std::size_t{1});
    CHECK(e[0].action == Action::GlanceHold);
    CHECK(!e[0].pressed);
}

// ---------------------------------------------------------------------------------------------------------------
// End to end through the module: default keys with both panels closed, the ActiveCameraChanged/Snapshot glance
// field and the Bindings page's Test button.

namespace {
namespace js = fscs::js;

struct GlanceModule {
    FakeBackend camera;
    FakeServices services;
    FakeStorage storage;
    ModuleApp app{camera, services, storage, "boot-glance", "0.1.0-test"};
    double t = 0;
    int req = 0;
    std::size_t seen = 0;
    GlanceModule() { app.start(t); }
    std::vector<json> drain() {
        fscs::protocol::Reassembler r;
        std::vector<json> out;
        for (; seen < services.sent.size(); ++seen)
            if (auto whole = r.feed(services.sent[seen], t)) out.push_back(js::parse(*whole));
        return out;
    }
    json lastEvent(const std::string& name) {
        json found;
        for (auto& m : drain())
            if (js::str(m, "type") == "event" && js::str(m, "name") == name) found = m;
        return found;
    }
    void panel(const std::string& action, json payload = json::object()) {
        json msg{{"protocolVersion", 2}, {"requestId", "main-" + std::to_string(++req)}, {"uiInstanceId", "main-1"},
                 {"moduleBootId", app.bootId()}, {"sessionEpoch", app.coordinator().epoch()}, {"action", action}, {"payload", payload}};
        app.onEvent(be::CommBusMessage{msg.dump()}, t);
    }
    void frame(int n = 1) {
        for (int i = 0; i < n; ++i) {
            t += 1.0 / 60;
            app.update(t);
        }
    }
    std::size_t key(const std::string& definition) const {
        return static_cast<std::size_t>(std::find(services.mapped.begin(), services.mapped.end(), definition) - services.mapped.begin());
    }
    void press(const std::string& definition, bool down) { app.onEvent(be::InputEvent{key(definition), down}, t); }
    std::string cameraNamed(const std::string& name) const {
        for (const auto& c : app.workingProfile()["cameras"])
            if (c["name"] == name) return c["id"];
        return {};
    }
};
}  // namespace

TEST_CASE("module: glance keys work with panels closed and report the glanced view") {
    GlanceModule m;
    m.frame();
    m.app.onEvent(be::Connected{"1.8.16.0"}, m.t);
    m.app.onEvent(be::AircraftLoaded{"SimObjects\\Airplanes\\asobo_c172sp_g1000\\aircraft.cfg"}, m.t);
    m.app.onEvent(be::CameraStatus{SdkAvailability::NotAcquired, false}, m.t);
    m.frame(2);
    m.panel("Hello", {{"surface", "main"}});
    m.panel("QualificationStart");
    const auto pilot = js::str(m.app.workingProfile(), "defaultCameraId");
    const auto pfd = m.cameraNamed("G1000 - PFD");
    CHECK(!pfd.empty());
    m.panel("ActivateCamera", {{"cameraId", pilot}});
    m.app.onEvent(be::CameraStatus{SdkAvailability::Acquired, false}, m.t);
    m.frame(90);
    CHECK(m.app.coordinator().ownership() == Ownership::Active);
    for (const char* def : {"VK_LCONTROL+VK_LMENU+G", "VK_LCONTROL+VK_LMENU+I", "VK_LCONTROL+VK_LMENU+U"}) CHECK(m.key(def) < m.services.mapped.size());
    m.drain();

    m.press("VK_LCONTROL+VK_LMENU+G", true);
    m.frame(30);
    CHECK_EQ(m.app.coordinator().activeCameraId(), pfd);
    auto active = m.lastEvent("ActiveCameraChanged");
    CHECK_EQ(active["payload"]["glance"]["cameraId"].get<std::string>(), pfd);
    CHECK_EQ(active["payload"]["glance"]["returnCameraId"].get<std::string>(), pilot);
    m.panel("RequestSnapshot");
    CHECK_EQ(m.lastEvent("Snapshot")["payload"]["activeCamera"]["glance"]["cameraId"].get<std::string>(), pfd);
    m.press("VK_LCONTROL+VK_LMENU+G", false);
    m.frame(30);
    CHECK_EQ(m.app.coordinator().activeCameraId(), pilot);
    CHECK(m.lastEvent("ActiveCameraChanged")["payload"]["glance"].is_null());

    // Next and Return keys.
    m.press("VK_LCONTROL+VK_LMENU+I", true); m.press("VK_LCONTROL+VK_LMENU+I", false);
    m.press("VK_LCONTROL+VK_LMENU+I", true); m.press("VK_LCONTROL+VK_LMENU+I", false);
    CHECK_EQ(m.app.coordinator().activeCameraId(), m.cameraNamed("G1000 - MFD"));
    m.press("VK_LCONTROL+VK_LMENU+U", true); m.press("VK_LCONTROL+VK_LMENU+U", false);
    m.frame(30);
    CHECK_EQ(m.app.coordinator().activeCameraId(), pilot);

    // Test on the Bindings page: a held glance stays on the instrument (a release would return at once).
    m.panel("TestBinding", {{"binding", {{"action", "glanceHold"}}}});
    m.frame(30);
    CHECK_EQ(m.app.coordinator().activeCameraId(), pfd);
    m.panel("TestBinding", {{"binding", {{"action", "glanceReturn"}}}});
    m.frame(30);
    CHECK_EQ(m.app.coordinator().activeCameraId(), pilot);
}
