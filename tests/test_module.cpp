#include "test.hpp"

#include "fake_backend.hpp"
#include "module_app.hpp"
#include "fscs/profiles/json_util.hpp"

using namespace fscs::host;
using fscs::protocol::json;
namespace be = backend_event;
namespace js = fscs::js;

namespace {

struct ModuleRig {
    FakeBackend camera;
    FakeServices services;
    FakeStorage storage;
    ModuleApp app{camera, services, storage, "boot-0123456789", "0.1.0-test"};
    double t = 0;
    int req = 0;
    std::size_t seen = 0;

    ModuleRig() { app.start(t); }

    // Messages to panels since the last call, reassembled.
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
    std::string panel(const std::string& action, json payload = json::object(), const std::string& instance = "main-1",
                      std::optional<std::int64_t> draftRevision = {}) {
        const std::string id = instance.substr(0, instance.find('-')) + "-" + std::to_string(++req);
        json msg{{"protocolVersion", 2},       {"requestId", id},     {"uiInstanceId", instance}, {"moduleBootId", app.bootId()},
                 {"sessionEpoch", app.coordinator().epoch()}, {"action", action}, {"payload", payload}};
        if (draftRevision) msg["draftRevision"] = *draftRevision;
        app.onEvent(be::CommBusMessage{msg.dump()}, t);
        return id;
    }
    json ackFor(const std::string& requestId) {
        for (auto& m : drain())
            if (js::str(m, "type") == "ack" && js::str(m, "requestId") == requestId) return m;
        return nullptr;
    }
    void frame(int n = 1) {
        for (int i = 0; i < n; ++i) {
            t += 1.0 / 60;
            app.update(t);
        }
    }
    void flight(const std::string& aircraftPath = "SimObjects\\Airplanes\\Asobo-C172\\aircraft.cfg") {
        frame();
        app.onEvent(be::Connected{"1.8.16.0"}, t);
        app.onEvent(be::AircraftLoaded{aircraftPath}, t);
        app.onEvent(be::CameraStatus{SdkAvailability::NotAcquired, false}, t);
        frame();
    }
};

}  // namespace

TEST_CASE("module: Hello returns an addressed snapshot; unknown boot ids are rejected") {
    ModuleRig m;
    m.drain();
    m.panel("Hello", {{"surface", "main"}});
    auto msgs = m.drain();
    bool snapshot = false;
    for (const auto& x : msgs)
        if (js::str(x, "name") == "Snapshot") {
            snapshot = true;
            CHECK_EQ(js::str(x, "to"), std::string("main-1"));
            CHECK_EQ(js::str(x["payload"], "moduleBootId"), std::string("boot-0123456789"));
            CHECK_EQ(js::str(x["payload"]["ownership"], "state"), std::string("Observing"));
        }
    CHECK(snapshot);
    json stale{{"protocolVersion", 2}, {"requestId", "main-9"}, {"uiInstanceId", "main-1"}, {"moduleBootId", "old-boot"},
               {"action", "SelectCamera"}, {"payload", json::object()}};
    m.app.onEvent(be::CommBusMessage{stale.dump()}, m.t);
    CHECK_EQ(js::str(m.ackFor("main-9"), "reason"), std::string("staleBoot"));
}

TEST_CASE("module: creating a profile for the loaded aircraft opens it and matches it") {
    ModuleRig m;
    m.flight();
    m.panel("Hello", {{"surface", "main"}});
    const auto id = m.panel("CreateProfile", {{"name", "My cameras"}, {"forAircraft", true}});
    CHECK_EQ(js::str(m.ackFor(id), "status"), std::string("Pending"));
    CHECK_EQ(m.app.repository().committed().profiles.size(), std::size_t{1});
    CHECK(m.app.workingProfile().is_object());
    CHECK(m.app.coordinator().canActivate() == false);  // adapter not qualified by default
    CHECK(m.app.coordinator().blockReason().find("axis check") != std::string::npos);
}

TEST_CASE("module: drafts are module-owned, mirrored to every surface, and saved as one transaction") {
    ModuleRig m;
    m.flight();
    m.panel("Hello", {{"surface", "main"}});
    m.panel("Hello", {{"surface", "mini"}}, "mini-1");
    m.panel("CreateProfile", {{"name", "Cams"}, {"forAircraft", true}});
    m.drain();
    json cam = m.app.workingProfile()["cameras"][0];
    cam["name"] = "Captain";
    const auto rev = m.app.draftRevision();
    const auto upd = m.panel("UpdateDraft", {{"ops", {{{"op", "setCamera"}, {"camera", cam}}}}}, "main-1", rev);
    auto msgs = m.drain();
    bool ack = false, mirrored = false;
    for (const auto& x : msgs) {
        if (js::str(x, "requestId") == upd) ack = js::str(x, "status") == "Completed";
        if (js::str(x, "name") == "DraftChanged") mirrored = js::boolean(x["payload"]["dirty"], "profile", false);
    }
    CHECK(ack);
    CHECK(mirrored);  // broadcast: the mini surface receives the same message
    CHECK(m.app.dirty());

    // A stale draft revision is refused.
    CHECK_EQ(js::str(m.ackFor(m.panel("UpdateDraft", {{"ops", {{{"op", "setCamera"}, {"camera", cam}}}}}, "mini-1", rev)), "reason"),
             std::string("staleDraft"));

    const auto save = m.panel("CommitChanges", {{"backup", true}});
    (void)save;
    const json done = m.lastEvent("CommitCompleted");
    CHECK_EQ(js::str(done["payload"], "status"), std::string("Committed"));
    CHECK(!m.app.dirty());
    CHECK_EQ(js::str(m.app.repository().committed().profiles.begin()->second.current["cameras"][0], "name"), std::string("Captain"));
}

TEST_CASE("module: a failed save keeps the draft and reports why") {
    ModuleRig m;
    m.flight();
    m.panel("CreateProfile", {{"name", "Cams"}, {"forAircraft", true}});
    json cam = m.app.workingProfile()["cameras"][0];
    cam["name"] = "Changed";
    m.panel("UpdateDraft", {{"ops", {{{"op", "setCamera"}, {"camera", cam}}}}}, "main-1", m.app.draftRevision());
    m.drain();
    m.storage.failNextWrite = true;
    m.panel("CommitChanges");
    const json done = m.lastEvent("CommitCompleted");
    CHECK_EQ(js::str(done["payload"], "status"), std::string("Failed"));
    CHECK(m.app.dirty());
    CHECK_EQ(js::str(m.app.workingProfile()["cameras"][0], "name"), std::string("Changed"));
}

TEST_CASE("module: invalid draft operations are rejected before they reach the camera") {
    ModuleRig m;
    m.flight();
    m.panel("CreateProfile", {{"name", "Cams"}, {"forAircraft", true}});
    json cam = m.app.workingProfile()["cameras"][0];
    cam["projection"]["value"] = 9.0;
    const auto id = m.panel("UpdateDraft", {{"ops", {{{"op", "setCamera"}, {"camera", cam}}}}}, "main-1", m.app.draftRevision());
    const json ack = m.ackFor(id);
    CHECK_EQ(js::str(ack, "status"), std::string("Rejected"));
    CHECK(!m.app.dirty());
}

TEST_CASE("module: switching aircraft opens the matching profile when the draft is clean") {
    ModuleRig m;
    m.flight();
    m.panel("CreateProfile", {{"name", "C172"}, {"forAircraft", true}});
    const std::string c172 = js::str(m.app.workingProfile(), "profileId");
    m.flight("SimObjects\\Airplanes\\Other-Jet\\aircraft.cfg");
    m.panel("CreateProfile", {{"name", "Jet"}, {"forAircraft", true}});
    CHECK(js::str(m.app.workingProfile(), "profileId") != c172);
    m.flight("SimObjects\\Airplanes\\Asobo-C172\\aircraft.cfg");
    m.frame(2);
    CHECK_EQ(js::str(m.app.workingProfile(), "profileId"), c172);
}

TEST_CASE("module: OpenProfile refuses to drop unsaved changes unless asked") {
    ModuleRig m;
    m.flight();
    m.panel("CreateProfile", {{"name", "A"}, {"forAircraft", true}});
    const std::string a = js::str(m.app.workingProfile(), "profileId");
    m.panel("CreateProfile", {{"name", "B"}, {"forAircraft", true}});
    json cam = m.app.workingProfile()["cameras"][0];
    cam["name"] = "Edited";
    m.panel("UpdateDraft", {{"ops", {{{"op", "setCamera"}, {"camera", cam}}}}}, "main-1", m.app.draftRevision());
    m.drain();
    CHECK_EQ(js::str(m.ackFor(m.panel("OpenProfile", {{"profileId", a}})), "reason"), std::string("unsavedChanges"));
    CHECK_EQ(js::str(m.ackFor(m.panel("OpenProfile", {{"profileId", a}, {"discard", true}})), "status"), std::string("Completed"));
    CHECK_EQ(js::str(m.app.workingProfile(), "profileId"), a);
}

TEST_CASE("module: export and import round-trip inside the module") {
    ModuleRig m;
    m.flight();
    m.panel("CreateProfile", {{"name", "Source"}, {"forAircraft", true}});
    m.drain();
    const std::string src = js::str(m.app.workingProfile(), "profileId");
    const json exp = m.ackFor(m.panel("ExportProfile", {{"profileId", src}}));
    const std::string code = js::str(exp["result"], "code");
    CHECK(!code.empty());
    m.panel("ImportProfile", {{"code", code}});
    const json done = m.lastEvent("CommitCompleted");
    CHECK_EQ(js::str(done["payload"], "status"), std::string("Committed"));
    CHECK_EQ(m.app.repository().committed().profiles.size(), std::size_t{2});
}

TEST_CASE("module: chunked panel messages are reassembled; release works even before a snapshot") {
    ModuleRig m;
    m.flight();
    json big{{"protocolVersion", 2}, {"requestId", "main-77"}, {"uiInstanceId", "main-1"}, {"action", "ImportProfile"},
             {"payload", {{"code", std::string(9000, ' ') + "{}"}}}};
    for (const auto& piece : fscs::protocol::chunk(big.dump(), "ui-1")) m.app.onEvent(be::CommBusMessage{piece}, m.t);
    const json ack = m.ackFor("main-77");
    CHECK_EQ(js::str(ack, "status"), std::string("Rejected"));  // reached the handler intact
    json rel{{"protocolVersion", 2}, {"requestId", "mini-5"}, {"uiInstanceId", "mini-x"}, {"moduleBootId", "anything"}, {"action", "ReleaseCamera"}};
    m.app.onEvent(be::CommBusMessage{rel.dump()}, m.t);
    CHECK_EQ(js::str(m.ackFor("mini-5"), "status"), std::string("Completed"));
}

TEST_CASE("module: bindings map through the simulator and drive actions with the editor closed") {
    ModuleRig m;
    m.flight();
    m.panel("CreateProfile", {{"name", "Cams"}, {"forAircraft", true}});
    CHECK(!m.services.mapped.empty());
    const auto& defs = m.services.mapped;
    const auto i = static_cast<std::size_t>(std::find(defs.begin(), defs.end(), "VK_LCONTROL+VK_LMENU+R") - defs.begin());
    CHECK(i < defs.size());
    m.app.onEvent(be::InputEvent{i, true, 0, false}, m.t);  // no panel needed
    CHECK(m.app.coordinator().ownership() == Ownership::Observing);
}

TEST_CASE("module: state events are coalesced and paused simulators get one fresh snapshot on resume") {
    ModuleRig m;
    m.flight();
    m.panel("Hello", {{"surface", "main"}});
    m.drain();
    m.app.onEvent(be::Paused{true}, m.t);
    for (int i = 0; i < 20; ++i) m.app.onEvent(be::AircraftTitle{"Livery " + std::to_string(i)}, m.t);
    m.frame(10);
    auto during = m.drain();
    CHECK(during.empty());  // nothing streamed while JS is paused
    m.app.onEvent(be::Paused{false}, m.t);
    auto after = m.drain();
    int snapshots = 0;
    for (const auto& x : after) snapshots += js::str(x, "name") == "Snapshot";
    CHECK_EQ(snapshots, 1);
}

TEST_CASE("module: pinning the mini panel is saved as a preference and never marks edits unsaved") {
    ModuleRig m;
    m.flight();
    m.panel("CreateProfile", {{"name", "Cams"}, {"forAircraft", true}});
    json settings = m.app.repository().committed().settings;
    settings["ui"]["miniPinned"] = true;
    m.panel("UpdateDraft", {{"ops", {{{"op", "setSettings"}, {"settings", settings}}}}}, "mini-1", m.app.draftRevision());
    CHECK(!m.app.dirty());
    CHECK(m.app.repository().committed().settings["ui"]["miniPinned"].get<bool>());
}

TEST_CASE("module: text focus in a panel suppresses bindings except Release Camera") {
    ModuleRig m;
    m.flight();
    m.panel("CreateProfile", {{"name", "Cams"}, {"forAircraft", true}});
    m.panel("TextFocus", {{"focused", true}});
    const auto& defs = m.services.mapped;
    const auto i = static_cast<std::size_t>(std::find(defs.begin(), defs.end(), "VK_LCONTROL+VK_LMENU+1") - defs.begin());
    m.app.onEvent(be::InputEvent{i, true, 0, false}, m.t);
    CHECK(m.app.coordinator().ownership() == Ownership::Observing);  // the view-1 key was ignored
}

TEST_CASE("module: the in-panel axis check qualifies the build and corrects a reversed sign") {
    ModuleRig m;
    m.flight();
    m.panel("CreateProfile", {{"name", "Cams"}, {"forAircraft", true}});
    CHECK(!m.app.coordinator().canActivate());  // unqualified build
    m.panel("QualificationStart");
    CHECK(m.app.coordinator().canActivate());
    const std::string pilot = js::str(m.app.workingProfile(), "defaultCameraId");
    m.panel("ActivateCamera", {{"cameraId", pilot}});
    m.app.onEvent(be::CameraStatus{SdkAvailability::Acquired, false}, m.t);
    m.frame();
    m.drain();
    const auto probe = m.panel("QualificationProbe", {{"axis", "forward"}});
    CHECK_EQ(js::str(m.ackFor(probe), "status"), std::string("Completed"));
    m.frame();
    CHECK_NEAR(m.camera.submitted.back().z, 0.3, 1e-9);  // default contract: forward -> SDK z
    const double pitchSignBefore = m.app.coordinator().qualificationState().contract.pitchSign;
    m.panel("QualificationRecord", {{"answers", {{"forward", "correct"}, {"right", "correct"}, {"up", "correct"},
                                                 {"pitch", "reversed"}, {"heading", "correct"}, {"bank", "correct"}}},
                                    {"recall", "pass"}});
    m.panel("QualificationEnd");
    CHECK(m.app.coordinator().qualificationState().adapterQualified);
    CHECK_NEAR(m.app.coordinator().qualificationState().contract.pitchSign, -pitchSignBefore, 1e-12);
    CHECK(m.app.coordinator().canActivate());
}
