#include "test.hpp"

#include "fake_backend.hpp"
#include "module_app.hpp"
#include "airport_fixture.hpp"
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
    void flight(const std::string& aircraftPath = "SimObjects\\Airplanes\\Asobo-C152\\aircraft.cfg") {
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
    m.flight("SimObjects\\Airplanes\\Asobo-C152\\aircraft.cfg");
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

TEST_CASE("module: saved-profile startup registers bindings without opening a panel") {
    ModuleRig first;
    first.flight();
    first.panel("CreateProfile", {{"name", "Saved cameras"}, {"forAircraft", true}});
    FakeBackend camera;
    FakeServices services;
    ModuleApp restarted(camera, services, first.storage, "restart-boot", "test");
    restarted.start(0);
    CHECK(restarted.workingProfile().is_object());
    CHECK(!services.mapped.empty());
    CHECK(std::find(services.mapped.begin(), services.mapped.end(), "VK_LCONTROL+VK_LMENU+1") != services.mapped.end());
    const auto savedId = js::str(restarted.workingProfile(), "profileId");
    restarted.onEvent(be::Connected{"1.8.16.0"}, 1);
    restarted.onEvent(be::AircraftLoaded{"SimObjects\\Airplanes\\Asobo-C152\\aircraft.cfg"}, 1);
    CHECK(restarted.coordinator().profile() == nullptr);
    restarted.update(2);
    CHECK(restarted.coordinator().profile() != nullptr);
    CHECK_EQ(js::str(restarted.workingProfile(), "profileId"), savedId);
    CHECK(restarted.coordinator().ownership() == Ownership::Observing);
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

TEST_CASE("module: C172 complete profile installs once, opens by default and never acquires the camera") {
    ModuleRig m;
    m.flight("SimObjects\\Airplanes\\asobo_c172sp_g1000\\aircraft.cfg");
    m.frame(2);
    CHECK_EQ(js::str(m.app.workingProfile(), "builtinTemplate"), std::string("c172-complete-v1"));
    CHECK_EQ(m.app.workingProfile()["cameras"].size(), std::size_t{20});
    CHECK(m.app.coordinator().ownership() == Ownership::Observing);
    const auto id = js::str(m.app.workingProfile(), "profileId");
    const auto txn = m.app.repository().committed().txn;
    m.flight("SimObjects\\Airplanes\\Other-Jet\\aircraft.cfg");
    m.flight("SimObjects\\Airplanes\\asobo_c172sp_g1000\\aircraft.cfg");
    m.frame(2);
    CHECK_EQ(js::str(m.app.workingProfile(), "profileId"), id);
    CHECK_EQ(m.app.repository().committed().txn, txn);
    CHECK_EQ(m.app.repository().committed().profiles.size(), std::size_t{1});
}

TEST_CASE("module: native mouse blocking aggregates panels and expires with visibility or leases") {
    ModuleRig m; m.flight();
    m.panel("Hello", {{"surface", "main"}});
    m.panel("Hello", {{"surface", "mini"}}, "mini-1");
    m.panel("CreateProfile", {{"name", "Mouse"}, {"forAircraft", true}});
    m.panel("QualificationStart");
    const auto pilot = js::str(m.app.workingProfile(), "defaultCameraId");
    m.panel("ActivateCamera", {{"cameraId", pilot}});
    m.app.onEvent(be::CameraStatus{SdkAvailability::Acquired, false}, m.t); m.frame();
    m.app.onEvent(be::NativeMouseMapping{true}, m.t);
    const auto drag = [&] {
        m.app.onEvent(be::NativeMouseButton{true}, m.t);
        m.app.onEvent(be::NativeMouseAxis{true, 8192}, m.t);
        m.frame(90);
        m.app.onEvent(be::NativeMouseButton{false}, m.t);
    };
    m.panel("PointerOverPanel", {{"over", true}});
    m.panel("PointerOverPanel", {{"over", false}}, "mini-1");
    drag(); CHECK_NEAR(m.app.coordinator().engine().look().headingDeg, 0, 1e-9);
    m.panel("RenewLease", {{"visible", false}}); m.frame();
    drag(); CHECK_NEAR(m.app.coordinator().engine().look().headingDeg, 1.5, 1e-9);
    m.panel("PointerOverPanel", {{"over", true}}, "mini-1");
    drag(); CHECK_NEAR(m.app.coordinator().engine().look().headingDeg, 1.5, 1e-9);
    m.frame(400); // no live UI lease can leave native mouse permanently blocked
    drag(); CHECK_NEAR(m.app.coordinator().engine().look().headingDeg, 3, 1e-9);
    CHECK(m.app.coordinator().ownership() == Ownership::Active);
}

TEST_CASE("module: native mouse stays dormant through loading and acquisition, suspends and releases") {
    ModuleRig m;
    m.flight("SimObjects\\Airplanes\\asobo_c172sp_g1000\\aircraft.cfg");
    m.panel("Hello", {{"surface", "main"}});
    m.app.onEvent(be::FlightLoaded{"test.flt"}, m.t); m.frame(180);
    CHECK(m.services.mouseEnableChanges.empty());
    m.panel("QualificationStart");
    m.panel("ActivateCamera", {{"cameraId", js::str(m.app.workingProfile(), "defaultCameraId")}}); m.frame();
    CHECK(m.services.mouseEnableChanges.empty()); // pending acquire is not enough
    m.app.onEvent(be::CameraStatus{SdkAvailability::Acquired, false}, m.t); m.frame();
    CHECK(m.services.nativeMouseEnabled);
    CHECK_EQ(m.services.mouseEnableChanges.size(), std::size_t{1});
    m.frame(120); CHECK_EQ(m.services.mouseEnableChanges.size(), std::size_t{1});
    m.app.onEvent(be::Paused{true}, m.t); m.frame(); CHECK(!m.services.nativeMouseEnabled);
    m.app.onEvent(be::Paused{false}, m.t); m.frame(); CHECK(m.services.nativeMouseEnabled);
    m.app.onEvent(be::CameraStatus{SdkAvailability::Acquired, true}, m.t); m.frame();
    CHECK(!m.services.nativeMouseEnabled); // native simulator takeover
    m.app.onEvent(be::CameraStatus{SdkAvailability::Acquired, false}, m.t); m.frame();
    m.panel("ReleaseCamera"); m.frame(); CHECK(!m.services.nativeMouseEnabled);
    m.app.onEvent(be::FlightLoaded{"another.flt"}, m.t); m.frame(); CHECK(!m.services.nativeMouseEnabled);
    m.app.shutdown(m.t); CHECK(!m.services.nativeMouseEnabled);
}

TEST_CASE("module: wheel bindings respect pause, text focus and cleared mappings across reload") {
    ModuleRig m; m.flight(); m.panel("Hello", {{"surface", "main"}});
    m.panel("CreateProfile", {{"name", "Movement"}, {"forAircraft", true}});
    m.panel("QualificationStart");
    m.panel("ActivateCamera", {{"cameraId", js::str(m.app.workingProfile(), "defaultCameraId")}});
    m.app.onEvent(be::CameraStatus{SdkAvailability::Acquired, false}, m.t); m.frame();
    const auto zoom = static_cast<std::size_t>(std::find(m.services.mapped.begin(),m.services.mapped.end(),"sim:ZOOM_IN")-m.services.mapped.begin());
    CHECK(zoom < m.services.mapped.size());
    const auto pulse = [&] { m.app.onEvent(be::InputEvent{zoom,true,1,false},m.t); };
    pulse(); CHECK(m.app.coordinator().engine().look().fovDeltaRad < 0);
    const double offset=m.app.coordinator().engine().look().fovDeltaRad;
    m.app.onEvent(be::Paused{true},m.t); pulse();
    CHECK_NEAR(m.app.coordinator().engine().look().fovDeltaRad,offset,1e-9);
    m.app.onEvent(be::Paused{false},m.t);
    m.panel("TextFocus",{{"focused",true}}); pulse();
    CHECK_NEAR(m.app.coordinator().engine().look().fovDeltaRad,offset,1e-9);
    m.panel("TextFocus",{{"focused",false}});
    auto bindings=fscs::profiles::defaultBindingsDocument(); bindings["bindings"]=json::array();
    m.panel("UpdateDraft",{{"ops",{{{"op","setBindings"},{"bindings",bindings}}}}},"main-1",m.app.draftRevision());
    CHECK(m.services.mapped.empty()); // No hidden WASD fallback.
    m.panel("CommitChanges");
    CHECK(m.app.repository().committed().bindings["bindings"].empty());
    FakeBackend camera; FakeServices services;
    ModuleApp reloaded{camera,services,m.storage,"boot-reloaded","0.1.0-test"}; reloaded.start(0);
    CHECK(services.mapped.empty());
    CHECK(reloaded.repository().committed().bindings["bindings"].empty());
}

TEST_CASE("module: airport preview saves separate files, loads one nearby tree and preserves aircraft edits") {
    ModuleRig m; m.flight();m.panel("Hello",{{"surface","main"}});
    m.panel("CreateProfile",{{"name","Aircraft"},{"forAircraft",true}});m.drain();
    const auto aircraft=m.app.workingProfile();
    m.app.onEvent(be::AircraftPosition{40,-90,100},m.t);
    for(const auto& code:{std::string("KAAA"),std::string("KBBB")}) {
        m.panel("PreviewAirportSource",{{"facility",airportFixture(code,code=="KAAA"?40:40.1)}});
        auto preview=m.lastEvent("AirportPreview")["payload"]["profile"];CHECK(preview.is_object());
        json ids=json::array();for(auto& c:preview["cameras"])ids.push_back(c["id"]);
        auto request=m.panel("SaveAirportPreview",{{"icao",code},{"cameraIds",ids}});
        CHECK_EQ(js::str(m.ackFor(request),"status"),std::string("Pending"));
    }
    CHECK_EQ(m.app.repository().committed().profiles.size(),std::size_t{3});
    CHECK_EQ(m.app.workingProfile(),aircraft);
    bool aFile=false,bFile=false;for(auto& [file,bytes]:m.storage.files)for(auto& [mirror,doc]:fscs::profiles::aircraftProfileFiles(js::parse(bytes))){aFile=aFile||mirror.find("fscs-airport-KAAA.json")!=std::string::npos;bFile=bFile||mirror.find("fscs-airport-KBBB.json")!=std::string::npos;}
    CHECK(aFile);CHECK(bFile);
    m.panel("RequestSnapshot");auto snap=m.lastEvent("Snapshot")["payload"];
    CHECK_EQ(snap["airports"]["loaded"],json("KAAA"));CHECK_EQ(snap["airports"]["nearby"].size(),std::size_t{2});
    CHECK_EQ(snap["working"]["profile"]["cameras"].size(),aircraft["cameras"].size()+8);
    auto cam=snap["working"]["profile"]["cameras"].back();CHECK(snap["working"]["hasCommittedVersion"][js::str(cam,"id")].get<bool>());
    cam["name"]="My tower";
    auto edit=m.panel("UpdateDraft",{{"ops",{{{"op","setCamera"},{"camera",cam}}}}},"main-1",m.app.draftRevision());
    CHECK_EQ(js::str(m.ackFor(edit),"status"),std::string("Completed"));CHECK(m.app.dirty());
    CHECK_EQ(m.app.workingProfile(),aircraft);
    m.panel("SelectAirport",{{"icao","KBBB"}});m.drain();
    m.panel("CommitChanges");CHECK_EQ(js::str(m.lastEvent("CommitCompleted")["payload"],"status"),std::string("Committed"));
    CHECK(!m.app.dirty());
    m.panel("SelectAirport",{{"icao","KAAA"}});m.drain();m.panel("RequestSnapshot");snap=m.lastEvent("Snapshot")["payload"];
    CHECK_EQ(snap["working"]["profile"]["cameras"].back()["name"],json("My tower"));
    // Crossing from world scope into aircraft scope must fail atomically.
    auto cats=snap["working"]["profile"]["categories"];auto airportCat=cats.back();cats.back()["cameraIds"]=json::array();cats[0]["cameraIds"].push_back(cam["id"]);
    auto move=m.panel("UpdateDraft",{{"ops",{{{"op","setCategories"},{"categories",cats}}}}},"main-1",m.app.draftRevision());
    CHECK_EQ(js::str(m.ackFor(move),"status"),std::string("Rejected"));CHECK(!m.app.dirty());
    m.app.onEvent(be::AircraftPosition{42,-90,100},m.t);m.frame(121);m.panel("RequestSnapshot");snap=m.lastEvent("Snapshot")["payload"];
    CHECK_EQ(snap["airports"]["loaded"],json(""));CHECK_EQ(snap["working"]["profile"]["cameras"].size(),aircraft["cameras"].size());
}

TEST_CASE("module: airport facility requests complete independently and time out without camera writes") {
    ModuleRig m;m.flight();m.panel("Hello",{{"surface","main"}});m.drain();
    auto req=m.panel("PreviewAirports",{{"codes",{"KAAA","KBBB","!"}}});CHECK_EQ(js::str(m.ackFor(req),"status"),std::string("Accepted"));
    CHECK_EQ(m.services.airportRequests.size(),std::size_t{2});
    m.app.onEvent(be::AirportFacility{m.services.airportRequests[0].second,airportFixture(),""},m.t);
    CHECK(m.lastEvent("AirportPreview")["payload"]["profile"].is_object());
    m.frame(1202);auto timeout=m.lastEvent("AirportPreview")["payload"];
    CHECK_EQ(timeout["icao"],json("KBBB"));CHECK(!js::str(timeout,"error").empty());
    CHECK_EQ(m.camera.count("submit"),std::size_t{0});
}

TEST_CASE("module: nearby airport uses scenery ICAO without guessing prefixes or changing profiles") {
    ModuleRig m;m.flight();m.panel("Hello",{{"surface","main"}});m.panel("RequestSnapshot");
    const auto before=m.lastEvent("Snapshot")["payload"]["working"];
    m.app.onEvent(be::AircraftPosition{28.4294,-81.309,30},m.t);
    m.app.onEvent(be::AirportList{json::array({{{"icao","MCO"},{"region","K7"},{"lat",28.4294},{"lon",-81.309},{"altitude",30}},
       {{"icao","LFPG"},{"region","LF"},{"lat",49.01},{"lon",2.55},{"altitude",100}},
       {{"icao","29FL"},{"region","K7"},{"lat",28.5},{"lon",-81.4},{"altitude",30}}}),true},m.t);
    m.app.onEvent(be::AirportIdentity{"MCO","K7","KMCO",28.4294,-81.309},m.t);
    m.panel("RequestSnapshot");auto snap=m.lastEvent("Snapshot")["payload"];
    CHECK_EQ(snap["airports"]["facilities"][0]["icao"],json("KMCO"));
    CHECK_EQ(snap["working"],before);
    // A different facility with the same short identifier cannot rename this one.
    m.app.onEvent(be::AirportIdentity{"29FL","K7","K29FL",40,-90},m.t);
    m.panel("RequestSnapshot");snap=m.lastEvent("Snapshot")["payload"];
    bool local=false,international=false;for(const auto& a:snap["airports"]["facilities"]){local=local||a["icao"]=="29FL";international=international||a["icao"]=="LFPG";}
    CHECK(local);CHECK(international);
}

TEST_CASE("module: Ctrl S bakes manual movement and commits all changes once per key press") {
    ModuleRig m;m.flight();m.panel("Hello",{{"surface","main"}});m.panel("CreateProfile",{{"name","Save views"},{"forAircraft",true}});
    m.panel("QualificationStart");const auto id=js::str(m.app.workingProfile(),"defaultCameraId");m.panel("ActivateCamera",{{"cameraId",id}});m.app.onEvent(be::CameraStatus{SdkAvailability::Acquired,false},m.t);m.frame();
    const auto index=[&](const std::string& key){return static_cast<std::size_t>(std::find(m.services.mapped.begin(),m.services.mapped.end(),key)-m.services.mapped.begin());};
    const auto move=index("W"),save=index("VK_LCONTROL+S");CHECK(save<m.services.mapped.size());
    m.app.onEvent(be::InputEvent{move,true},m.t);m.frame(60);m.app.onEvent(be::InputEvent{move,false},m.t);m.frame(90);
    const auto position=m.app.coordinator().engine().sample().pose.positionFRD;
    CHECK(position.x>.1);m.app.onEvent(be::InputEvent{save,true},m.t);m.frame();
    CHECK_NEAR(m.app.workingProfile()["cameras"][0]["positionMetersFRD"][0].get<double>(),position.x,.001);CHECK(!m.app.dirty());
    CHECK_NEAR(m.app.coordinator().engine().look().translationHeadingFrame.length(),0,1e-9);
    const auto transaction=m.app.repository().committed().txn;
    m.app.onEvent(be::InputEvent{save,true},m.t);m.frame();CHECK_EQ(m.app.repository().committed().txn,transaction);
    m.app.onEvent(be::InputEvent{save,false},m.t);
    auto bindings=fscs::profiles::defaultBindingsDocument();bindings["bindings"].push_back({{"action","toggleHeadTracking"},{"input",{{"kind","key"},{"definition","T"}}}});
    m.panel("UpdateDraft",{{"ops",{{{"op","setBindings"},{"bindings",bindings}}}}},"main-1",m.app.draftRevision());
    m.app.onEvent(be::InputEvent{index("T"),true},m.t);m.frame();CHECK_EQ(m.app.workingProfile()["cameras"][0]["trackingPolicy"],json("off"));CHECK(m.camera.tracking==false);
}
TEST_CASE("module: airport nearest auto switch has a dwell and manual movement turns it off") {
    ModuleRig m;m.flight();m.panel("Hello",{{"surface","main"}});m.panel("CreateProfile",{{"name","Airport switching"},{"forAircraft",true}});
    m.app.onEvent(be::AircraftPosition{40,-90,100},m.t);
    m.panel("PreviewAirportSource",{{"facility",airportFixture()}});auto preview=m.lastEvent("AirportPreview")["payload"]["profile"];
    json ids=json::array();for(auto& cam:preview["cameras"])ids.push_back(cam["id"]);
    m.panel("SaveAirportPreview",{{"icao","KAAA"},{"cameraIds",ids}});m.panel("QualificationStart");
    auto first=preview["cameras"][0],second=preview["cameras"][1];
    m.panel("ActivateCamera",{{"cameraId",first["id"]}});m.app.onEvent(be::CameraStatus{SdkAvailability::Acquired,false},m.t);m.frame();
    CHECK_EQ(js::str(m.ackFor(m.panel("SetAirportAuto",{{"enabled",true}})),"status"),std::string("Completed"));
    auto p=second["positionMetersFRD"];const auto near=fscs::profiles::airport::world({40,-90,100},{p[0],p[1],p[2]});
    for(int i=0;i<120;++i){m.app.onEvent(be::AircraftPosition{near.lat,near.lon,near.altitude},m.t);m.frame();}
    CHECK_EQ(m.app.coordinator().activeCameraId(),first["id"].get<std::string>());
    for(int i=0;i<100;++i){m.app.onEvent(be::AircraftPosition{near.lat,near.lon,near.altitude},m.t);m.frame();}
    CHECK_EQ(m.app.coordinator().activeCameraId(),second["id"].get<std::string>());
    auto move=static_cast<std::size_t>(std::find(m.services.mapped.begin(),m.services.mapped.end(),"W")-m.services.mapped.begin());m.app.onEvent(be::InputEvent{move,true},m.t);m.frame();
    m.panel("RequestSnapshot");CHECK(!m.lastEvent("Snapshot")["payload"]["airports"]["autoSwitch"].get<bool>());
}
