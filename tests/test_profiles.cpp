#include "test.hpp"
#include <set>
#include <iomanip>
#include <sstream>
#include "airport_fixture.hpp"

#include "fake_backend.hpp"
#include "fscs/profiles/bindings.hpp"
#include "fscs/profiles/json_util.hpp"
#include "fscs/profiles/repository.hpp"
#include "fscs/protocol/protocol.hpp"

using namespace fscs::profiles;
using fscs::host::FakeStorage;

TEST_CASE("profiles: legacy mini favorites initialize once across aircraft and airport libraries") {
    StoreState old; old.txn = 1;
    std::vector<std::string> notes;
    auto aircraft = makeC172Profile("c172", "C172");
    auto airport = fscs::profiles::airport::generate(airportFixture(), json::object(), notes);
    for (auto doc : {aircraft, airport}) {
        for (auto& camera : doc["cameras"]) camera["favorite"] = false;
        const auto id = doc["profileId"].get<std::string>();
        old.profiles[id] = {doc, {doc}};
    }
    auto payload = json::parse(Repository::serialize(old, {}))["payload"];
    payload.erase("miniFavoritesInitialized");
    const auto body = fscs::js::dump(payload);
    std::ostringstream crc;
    crc << std::hex << std::setfill('0') << std::setw(8) << fscs::js::crc32(body);
    FakeStorage storage;
    storage.files["a"] = "{\"format\":\"fs-camera-studio-store\",\"version\":1,\"crc\":\"" + crc.str() + "\",\"payload\":" + body + "}";
    Repository loaded(storage, "a", "b"); loaded.load([](auto) {});
    CHECK_EQ(loaded.committed().profiles.size(), std::size_t{2});
    CommitRequest change;
    for (const auto& [id, record] : loaded.committed().profiles) {
        for (const auto& camera : record.current["cameras"]) CHECK_EQ(camera["favorite"], json(true));
        CHECK_EQ(record.backups[0]["cameras"][0]["favorite"], json(false));
        auto doc = record.current;
        doc["cameras"][0]["favorite"] = false;
        change.upsertProfiles.push_back(doc);
    }
    loaded.commit(change, [](const auto& result) { CHECK(result.status == CommitStatus::Committed); });
    Repository reloaded(storage, "a", "b"); reloaded.load([](auto) {});
    for (const auto& [id, record] : reloaded.committed().profiles) {
        CHECK_EQ(record.current["cameras"][0]["favorite"], json(false));
        CHECK_EQ(record.current["cameras"][1]["favorite"], json(true));
    }
    const auto savedEnvelope = json::parse(Repository::serialize(reloaded.committed(), {}));
    CHECK_EQ(savedEnvelope["payload"]["miniFavoritesInitialized"], json(true));
}

TEST_CASE("profiles: builtin views start starred and explicit imported favorites are preserved") {
    for (const auto& doc : {makeStarterProfile("Starter", "test", "Test"), makeC172Profile("c172", "C172"), makeA320Profile("a320", "A320")})
        for (const auto& camera : doc["cameras"]) CHECK_EQ(camera["favorite"], json(true));
    auto doc = makeC172Profile("c172", "C172");
    doc["cameras"][0]["favorite"] = false;
    std::string error;
    auto imported = importProfileCode(exportProfileCode(doc, false), &error);
    CHECK(imported.has_value());
    if (!imported) return;
    CHECK_EQ(imported->profile["cameras"][0]["favorite"], json(false));
    CHECK_EQ(imported->profile["cameras"][1]["favorite"], json(true));
}

namespace {

json twoCameraProfile() {
    json p = makeStarterProfile("Test", "asobo-c172", "C172");
    json cam = p["cameras"][0];
    cam["id"] = newUuid();
    cam["name"] = "PFD";
    cam["viewId"] = 2;
    cam["positionMetersFRD"] = {0.3, 0.0, 0.2};
    p["cameras"].push_back(cam);
    p["categories"][0]["cameraIds"].push_back(cam["id"]);
    return p;
}

struct Store {
    FakeStorage storage;
    Repository repo{storage, "a", "b"};
    std::vector<std::string> notes;
    Store() { repo.load([this](std::vector<std::string> n) { notes = std::move(n); }); }
    CommitResult commit(CommitRequest r) {
        CommitResult out;
        repo.commit(std::move(r), [&](const CommitResult& res) { out = res; });
        return out;
    }
};

}  // namespace

TEST_CASE("profile: the starter profile validates and the design example shape is accepted") {
    json p = makeStarterProfile("My cameras", "asobo-c172", "C172");
    auto r = validateProfile(p);
    CHECK(r.ok());
    auto snap = toSnapshot(p);
    CHECK(snap.cameraByViewId(1) != nullptr);
    CHECK(snap.cameras[0].transition.easing == fscs::core::Easing::Smoothstep3);
    CHECK_NEAR(snap.cameras[0].transition.durationSeconds, 0.65, 1e-12);
}

TEST_CASE("profile: structural errors are rejected") {
    json p = twoCameraProfile();
    p["cameras"][1]["viewId"] = 1;  // duplicate view ID
    CHECK(!validateProfile(p).ok());

    p = twoCameraProfile();
    p["categories"][0]["cameraIds"].push_back(p["cameras"][0]["id"]);
    CHECK(!validateProfile(p).ok());

    p = twoCameraProfile();
    p["defaultCameraId"] = newUuid();
    CHECK(!validateProfile(p).ok());

    p = twoCameraProfile();
    p["cameras"][0]["orientationCameraToAnchorXYZW"] = {0, 0, 0, 0};
    CHECK(validateProfile(p).summary().find("zero quaternion") != std::string::npos);

    p = twoCameraProfile();
    p["cameras"][0]["projection"]["value"] = 3.5;
    CHECK(!validateProfile(p).ok());

    p = twoCameraProfile();
    p["cameras"][0]["transition"]["durationSeconds"] = 20;  // above 15 s
    CHECK(!validateProfile(p).ok());

    p = twoCameraProfile();
    p["cameras"][0]["name"] = 42;  // wrong type must not abort, just fail
    CHECK(!validateProfile(p).ok());

    p = twoCameraProfile();
    p["schemaVersion"] = 3;
    CHECK(!validateProfile(p).ok());
}

TEST_CASE("profile: schema 1 documents migrate to schema 2") {
    json p = twoCameraProfile();
    p["schemaVersion"] = 1;
    for (auto& cam : p["cameras"]) {
        cam["shortcutSlot"] = cam["viewId"];
        cam.erase("viewId");
        cam["transition"]["kind"] = "smoothstep7";
    }
    auto r = validateProfile(p);
    CHECK(r.ok());
    CHECK(r.migrated);
    CHECK_EQ(p["schemaVersion"].get<int>(), 2);
    CHECK_EQ(p["cameras"][1]["viewId"].get<int>(), 2);
    CHECK_EQ(p["cameras"][0]["transition"]["kind"].get<std::string>(), std::string("smoothstep5"));
}

TEST_CASE("profile: unknown required feature opens read-only") {
    json p = twoCameraProfile();
    p["requiredSchemaFeatures"] = {"worldCameras2"};
    auto r = validateProfile(p);
    CHECK(r.ok());
    CHECK(r.readOnly);
}

TEST_CASE("profile: reassignIds remaps every reference including the default view") {
    json p = twoCameraProfile();
    const std::string oldDefault = p["defaultCameraId"];
    const auto remap = reassignIds(p);
    CHECK(validateProfile(p).ok());
    CHECK(p["defaultCameraId"].get<std::string>() != oldDefault);
    bool found = false;
    for (const auto& [from, to] : remap) found = found || (from == oldDefault && to == p["defaultCameraId"].get<std::string>());
    CHECK(found);
}

TEST_CASE("json: UTF-8 validation and CRC") {
    CHECK(fscs::js::validUtf8("plain"));
    CHECK(fscs::js::validUtf8("caf\xC3\xA9"));
    CHECK(!fscs::js::validUtf8("\xC3"));
    CHECK(!fscs::js::validUtf8("\xED\xA0\x80"));  // surrogate
    CHECK_EQ(fscs::js::crc32("123456789"), 0xCBF43926u);
}

TEST_CASE("repository: commits alternate slots and the newest valid transaction wins on load") {
    Store s;
    json p = twoCameraProfile();
    CommitRequest r;
    r.upsertProfiles.push_back(p);
    CHECK(s.commit(r).status == CommitStatus::Committed);
    CHECK(s.storage.files.count("a") == 1);
    p["name"] = "Second";
    CommitRequest r2;
    r2.upsertProfiles.push_back(p);
    CHECK(s.commit(r2).status == CommitStatus::Committed);
    CHECK(s.storage.files.count("b") == 1);

    Repository reloaded(s.storage, "a", "b");
    reloaded.load([](std::vector<std::string>) {});
    CHECK_EQ(reloaded.committed().txn, std::int64_t{2});
    const auto& rec = reloaded.committed().profiles.at(p["profileId"].get<std::string>());
    CHECK_EQ(rec.current["name"].get<std::string>(), std::string("Second"));
    CHECK_EQ(rec.current["revision"].get<int>(), 2);
    CHECK_EQ(rec.backups.size(), std::size_t{1});
}

TEST_CASE("repository: a torn write is detected by readback and never replaces the last save") {
    Store s;
    json p = twoCameraProfile();
    CommitRequest r;
    r.upsertProfiles.push_back(p);
    CHECK(s.commit(r).status == CommitStatus::Committed);
    p["name"] = "Torn";
    s.storage.tearNextWrite = true;
    CommitRequest r2;
    r2.upsertProfiles.push_back(p);
    const auto res = s.commit(r2);
    CHECK(res.status == CommitStatus::Failed);
    CHECK_EQ(s.repo.committed().profiles.begin()->second.current["name"].get<std::string>(), std::string("Test"));

    Repository reloaded(s.storage, "a", "b");
    std::vector<std::string> notes;
    reloaded.load([&](std::vector<std::string> n) { notes = std::move(n); });
    CHECK_EQ(reloaded.committed().profiles.begin()->second.current["name"].get<std::string>(), std::string("Test"));
    CHECK(!notes.empty());  // the interrupted save is reported
}

TEST_CASE("repository: a failed write keeps the committed state; corrupt slots are ignored") {
    Store s;
    CommitRequest r;
    r.upsertProfiles.push_back(twoCameraProfile());
    s.storage.failNextWrite = true;
    CHECK(s.commit(r).status == CommitStatus::Failed);
    CHECK(s.repo.committed().profiles.empty());
    CHECK(s.commit(r).status == CommitStatus::Committed);
    s.storage.files["b"] = "{ garbage";
    Repository reloaded(s.storage, "a", "b");
    reloaded.load([](std::vector<std::string>) {});
    CHECK_EQ(reloaded.committed().profiles.size(), std::size_t{1});
    // Flip a byte inside the payload: the checksum must reject it.
    std::string bad = s.storage.files["a"];
    bad[bad.size() / 2] = bad[bad.size() / 2] == 'x' ? 'y' : 'x';
    s.storage.files["a"] = bad;
    Repository corrupt(s.storage, "a", "b");
    std::vector<std::string> notes;
    corrupt.load([&](std::vector<std::string> n) { notes = std::move(n); });
    CHECK(corrupt.committed().profiles.empty());
    CHECK(!notes.empty());
}

TEST_CASE("repository: profile, settings and bindings commit as one transaction; retries are idempotent") {
    Store s;
    CommitRequest r;
    r.upsertProfiles.push_back(twoCameraProfile());
    json settings = defaultSettingsDocument();
    settings["ui"]["theme"] = "light";
    r.settings = settings;
    r.bindings = defaultBindingsDocument();
    r.requestId = "main-1";
    CHECK(s.commit(r).status == CommitStatus::Committed);
    CHECK_EQ(s.repo.committed().settings["ui"]["theme"].get<std::string>(), std::string("light"));
    const auto txn = s.repo.committed().txn;
    const auto again = s.commit(r);
    CHECK(again.duplicate);
    CHECK_EQ(s.repo.committed().txn, txn);
    // Invalid bindings fail the whole transaction.
    CommitRequest bad;
    bad.upsertProfiles.push_back(twoCameraProfile());
    bad.bindings = json{{"schemaVersion", 2}, {"bindings", {{{"action", "nope"}}}}};
    CHECK(s.commit(bad).status == CommitStatus::Failed);
    CHECK_EQ(s.repo.committed().profiles.size(), std::size_t{1});
}

TEST_CASE("repository: keeps at most five backups per profile") {
    Store s;
    json p = twoCameraProfile();
    for (int i = 0; i < 8; ++i) {
        p["name"] = "Rev " + std::to_string(i);
        CommitRequest r;
        r.upsertProfiles.push_back(p);
        CHECK(s.commit(r).status == CommitStatus::Committed);
    }
    const auto& rec = s.repo.committed().profiles.begin()->second;
    CHECK_EQ(rec.backups.size(), kBackupsPerProfile);
    CHECK_EQ(rec.current["revision"].get<int>(), 8);
    CHECK_EQ(rec.backups.front()["revision"].get<int>(), 7);
}

TEST_CASE("repository: asynchronous IO completes in order") {
    FakeStorage storage;
    storage.deferred = true;
    Repository repo(storage, "a", "b");
    bool loaded = false;
    repo.load([&](std::vector<std::string>) { loaded = true; });
    CHECK(!loaded);
    storage.runDeferred();
    CHECK(loaded);
    int done = 0;
    for (int i = 0; i < 3; ++i) {
        CommitRequest r;
        r.upsertProfiles.push_back(twoCameraProfile());
        repo.commit(r, [&](const CommitResult& res) { done += res.status == CommitStatus::Committed; });
    }
    CHECK(repo.busy());
    storage.runDeferred();
    CHECK_EQ(done, 3);
    CHECK_EQ(repo.committed().profiles.size(), std::size_t{3});
}

TEST_CASE("exchange: export/import round trip remaps ids; bad codes are refused") {
    json p = twoCameraProfile();
    p["cameras"][1]["mode"] = "free";
    std::string err;
    auto rep = importProfileCode(exportProfileCode(p, true), &err);
    CHECK(rep.has_value());
    CHECK(rep->profile["profileId"] != p["profileId"]);
    CHECK_EQ(rep->unsupportedCount, std::size_t{1});
    CHECK(!importProfileCode("not json", &err));
    CHECK(!importProfileCode(R"({"schemaVersion": 9})", &err));
    CHECK(!importProfileCode(R"({"format":"fs-camera-studio-profile","profile":[1,2]})", &err));
}

TEST_CASE("bindings: defaults parse; definitions are validated; duplicates dropped") {
    auto set = parseBindings(defaultBindingsDocument());
    CHECK(set.problems.empty());
    CHECK(set.bindings.size() > 15);
    CHECK(validDefinition("VK_LCONTROL+VK_LMENU+1"));
    CHECK(validDefinition("joystick:1:button:4"));
    CHECK(validDefinition("joystick:0:POV:0"));
    CHECK(!validDefinition("VK_LCONTROL+VK_LSHIFT+VK_LMENU+A"));  // three modifiers
    CHECK(!validDefinition("A+VK_LCONTROL"));                    // modifier after key
    CHECK(!validDefinition("joystick:x:button:1"));
    CHECK(!validDefinition("rm -rf"));
    json doc = defaultBindingsDocument();
    const json first = doc["bindings"][0];
    doc["bindings"].push_back(first);
    CHECK_EQ(parseBindings(doc).problems.size(), std::size_t{1});
}

TEST_CASE("settings: normalisation clamps and fills defaults") {
    json s{{"ui", {{"theme", "neon"}, {"precision", "coarse"}}}, {"host", {{"lookSpeedDegPerSec", 9999}}}};
    json n = normalizeSettings(s);
    CHECK_EQ(n["ui"]["theme"].get<std::string>(), std::string("dark"));
    CHECK_EQ(n["ui"]["precision"].get<std::string>(), std::string("coarse"));
    CHECK_NEAR(n["host"]["lookSpeedDegPerSec"].get<double>(), 360.0, 1e-9);
    CHECK(n["ui"]["miniEnabled"].get<bool>());
    CHECK_EQ(n["ui"]["nameDisplay"], "both");
    CHECK_NEAR(n["ui"]["miniOpacity"].get<double>(), 0.65, 1e-9);
    CHECK_NEAR(n["ui"]["mainOpacity"].get<double>(), 1.0, 1e-9);
    s["ui"]["mainOpacity"] = 0.35;
    CHECK_NEAR(normalizeSettings(s)["ui"]["mainOpacity"].get<double>(), 0.35, 1e-9);
    s["ui"]["mainOpacity"] = -1;
    CHECK_NEAR(normalizeSettings(s)["ui"]["mainOpacity"].get<double>(), 0.0, 1e-9);
    s["ui"]["mainOpacity"] = 3;
    CHECK_NEAR(normalizeSettings(s)["ui"]["mainOpacity"].get<double>(), 1.0, 1e-9);
    s["ui"]["nameDisplay"] = "icons"; s["ui"]["miniOpacity"] = 3;
    n = normalizeSettings(s);
    CHECK_EQ(n["ui"]["nameDisplay"], "icons");
    CHECK_NEAR(n["ui"]["miniOpacity"].get<double>(), 1, 1e-9);
    s["ui"]["nameDisplay"] = "invalid"; s["ui"]["miniOpacity"] = -1;
    n = normalizeSettings(s);
    CHECK_EQ(n["ui"]["nameDisplay"], "both");
    CHECK_NEAR(n["ui"]["miniOpacity"].get<double>(), 0.0, 1e-9);
}

TEST_CASE("bindings: movement migration preserves custom assignments and persists deliberate clearing") {
    json legacy{{"schemaVersion", 2}, {"bindings", json::array({
        {{"action", "moveForward"}, {"input", {{"kind", "key"}, {"definition", "I"}, {"label", "I"}}}},
        {{"action", "nextCamera"}, {"input", {{"kind", "key"}, {"definition", "E"}, {"label", "E"}}}}
    })}};
    const auto upgraded = upgradeMovementBindings(legacy);
    CHECK_EQ(upgraded["bindings"][0], legacy["bindings"][0]);
    CHECK_EQ(upgraded["bindings"][1], legacy["bindings"][1]);
    const auto parsed = parseBindings(upgraded); CHECK(parsed.problems.empty());
    for (const auto& b : parsed.bindings) {
        CHECK(b.action != Action::MoveDown); // E belongs to another action.
        CHECK(b.input.definition != "W"); // Existing custom forward binding wins.
    }
    CHECK_EQ(upgradeMovementBindings(upgraded), upgraded);
    StoreState state; state.txn = 1; state.settings = defaultSettingsDocument(); state.bindings = upgraded;
    state.bindings["bindings"] = json::array(); // Clear all, then save/reload.
    const auto loaded = Repository::deserialize(Repository::serialize(state, {}), nullptr, nullptr);
    CHECK(loaded.has_value());
    if (loaded) { CHECK(loaded->bindings["bindings"].empty()); CHECK_EQ(loaded->bindings["movementDefaultsVersion"], 2); }
    auto wheel = InputSpec::fromJson({{"kind","simZoom"},{"definition","sim:ZOOM_IN"}});
    CHECK(wheel.has_value());
    if (wheel) { const auto encoded = wheel->toJson(); CHECK_EQ(encoded["kind"], "simZoom"); }
    CHECK(!InputSpec::fromJson({{"kind","key"},{"definition","sim:ZOOM_IN"}}));
    CHECK(!InputSpec::fromJson({{"kind","simZoom"},{"definition","W"}}));
    CHECK(!validDefinition("sim:ENGINE_AUTO_START"));
    CHECK(!parseBindings({{"schemaVersion",2},{"bindings",json::array({{{"action","moveFast"},{"input",{{"kind","simZoom"},{"definition","sim:ZOOM_IN"}}}}})}}).problems.empty());
}

TEST_CASE("protocol: chunking round-trips, keeps UTF-8 intact, and rejects junk") {
    using namespace fscs::protocol;
    std::string big;
    for (int i = 0; i < 3000; ++i) big += "caf\xC3\xA9 ";
    const std::string text = json{{"blob", big}}.dump();
    auto pieces = chunk(text, "m1");
    CHECK(pieces.size() > 1);
    CHECK(pieces[0].compare(0, 8, "{\"part\":") == 0);
    Reassembler r;
    std::optional<std::string> out;
    for (auto it = pieces.rbegin(); it != pieces.rend(); ++it) out = r.feed(*it, 0.0);  // out of order
    CHECK(out.has_value());
    CHECK(*out == text);
    CHECK(r.feed(R"({"a":1})", 0.0).value() == R"({"a":1})");
    CHECK(!r.feed(R"({"part":5,"of":2,"msgId":"x","chunk":"y"})", 0.0));
    std::string err;
    CHECK(!Command::fromJson({{"protocolVersion", 1}, {"action", "X"}}, &err));
    CHECK(Command::fromJson({{"protocolVersion", 2}, {"action", "X"}, {"requestId", "main-4"}}, &err).has_value());
    CHECK(!Command::fromJson({{"protocolVersion", 2}, {"action", "X"}, {"payload", 7}}, &err));
}

TEST_CASE("profiles: C172 defaults have 12 cockpit and 8 exterior views with a pilot default") {
    json doc = makeC172Profile("asobo_c172sp_g1000", "C172SP G1000 Passengers");
    CHECK(validateProfile(doc).ok());
    CHECK_EQ(doc["cameras"].size(), std::size_t{20});
    CHECK_EQ(doc["categories"].size(), std::size_t{5});
    auto snap = toSnapshot(doc);
    CHECK(snap.camera(snap.defaultCameraId));
    CHECK_EQ(snap.camera(snap.defaultCameraId)->name, std::string("Pilot - normal"));
    int exterior = 0;
    std::set<int> numbers;
    for (const auto& view : doc["cameras"]) {
        CHECK(numbers.insert(view["viewId"].get<int>()).second);
        if (view["mode"] == "aircraftExterior") { ++exterior; CHECK(!view["interaction"].get<bool>()); }
    }
    CHECK_EQ(exterior, 8);
}

TEST_CASE("profiles: A320 starter anchors every variant at its own pilot eyepoint") {
    std::set<std::string> ids;
    for (const auto& model : {"inibuilds-aircraft-a320neo", "FlyByWire_A32NX", "FNX_320_CFM"}) {
        CHECK(isA320(model, ""));
        auto doc = makeA320Profile(model, "Airbus");
        CHECK(validateProfile(doc).ok());
        CHECK_EQ(doc["aircraftMatch"]["modelKey"], json(model));
        CHECK_EQ(doc["cameras"].size(), std::size_t{12});
        CHECK_EQ(doc["categories"].size(), std::size_t{3});
        CHECK(ids.insert(doc["profileId"].get<std::string>()).second);
        std::set<int> viewIds;
        for (const auto& cam : doc["cameras"]) {
            CHECK_EQ(cam["mode"], json("aircraftCockpit"));
            CHECK(cam["positionMetersFRD"] == json::array({0.0, 0.0, 0.0}));
            CHECK(cam["interaction"].get<bool>());
            CHECK(viewIds.insert(cam["viewId"].get<int>()).second);
        }
        const auto snap = toSnapshot(doc);
        CHECK_EQ(snap.camera(snap.defaultCameraId)->name, std::string("Pilot - normal"));
        for (const auto& cat : doc["categories"])
            CHECK_EQ(cat["defaultCameraId"], cat["cameraIds"][0]);
    }
    CHECK(isA320("asobo-aircraft", "Airbus A320 NEO"));
    CHECK(!isA320("Fenix_A319", "Airbus A319"));
    CHECK(!isA320("FNX_321", "Fenix A320 family A321"));
    CHECK(!isA320("Asobo_C172", "Cessna 172"));
}

TEST_CASE("profiles: aircraft files separate models, keep alternates and import the preferred profile") {
    json cessna = makeC172Profile("asobo_c172sp_g1000", "C172");
    json alternate = makeStarterProfile("Other C172 views", "asobo_c172sp_g1000", "C172");
    json jet = makeStarterProfile("Jet", "some-aircraft", "Jet");
    json payload{{"txn", 2}, {"navigation", {{"lastByModel", {{"asobo_c172sp_g1000", cessna["profileId"]}}}}},
                 {"profiles", {{{"current", cessna}}, {{"current", alternate}}, {{"current", jet}}}}};
    const auto files = aircraftProfileFiles({{"payload", payload}});
    CHECK_EQ(files.size(), std::size_t{2});
    const auto name = aircraftProfileFile("asobo_c172sp_g1000");
    CHECK(files.count(name));
    auto code = fscs::js::parse(files.at(name));
    CHECK_EQ(code["profile"]["profileId"], cessna["profileId"]);
    CHECK_EQ(code["alternateProfiles"].size(), std::size_t{1});
    CHECK(importProfileCode(files.at(name), nullptr).has_value());
    CHECK(aircraftProfileFile("model/a") != aircraftProfileFile("model-a"));
    CHECK(aircraftProfileFile("../../model").find("..") == std::string::npos);
}

TEST_CASE("profiles: camera effects round trip and reject invalid strengths") {
    auto doc = makeStarterProfile("FX", "test", "Test");
    doc["cameras"][0]["effects"] = {{"enabled",true},{"intensity",.6}};
    CHECK(validateProfile(doc).ok());
    const auto profile = toSnapshot(doc);
    CHECK(profile.cameras[0].effects.enabled);
    CHECK_NEAR(profile.cameras[0].effects.intensity,.6,1e-9);
    CHECK_NEAR(profile.cameras[0].effects.turbulence,.5,1e-9);
    doc["cameras"][0]["effects"]["groundRoll"]=2.; CHECK(!validateProfile(doc).ok());
    doc["cameras"][0]["effects"]="invalid"; CHECK(!validateProfile(doc).ok());
}
TEST_CASE("profiles: corrected lateral calibration preserves saved physical poses once") {
    StoreState state; state.txn=1;
    auto doc = makeStarterProfile("Saved", "test", "Test");
    doc["cameras"][0]["positionMetersFRD"]={.2,.08,-.1};
    const auto id = doc["profileId"].get<std::string>();
    state.profiles[id].current=doc; state.profiles[id].backups.push_back(doc);
    fscs::host::AdapterContract old;
    old.id="msfs2024-wasm-measured-12.2.282174.999";
    state.navigation={{"qualification",{{"records",json::array({{{"simVersion","12.2.282174.999"},{"contract",old.toJson()},{"gates",{{"G01","pass"},{"G03","pass"}}}}})}}}};
    const auto oldPose=toSnapshot(doc).cameras[0].pose;
    const auto oldSdk=fscs::host::toSdk(oldPose,old,fscs::host::Referential::Eyepoint);
    auto loaded=Repository::deserialize(Repository::serialize(state,{}),nullptr,nullptr); CHECK(loaded.has_value()); if (!loaded) return;
    const auto q=fscs::host::qualificationFor(loaded->navigation["qualification"],"12.2.282174.999");
    CHECK_NEAR(q.contract.axisSign[1],-1.,1e-9);
    CHECK_NEAR(loaded->profiles[id].current["cameras"][0]["positionMetersFRD"][1].get<double>(),-.08,1e-9);
    CHECK_NEAR(loaded->profiles[id].backups[0]["cameras"][0]["positionMetersFRD"][1].get<double>(),-.08,1e-9);
    const auto current=toSnapshot(loaded->profiles[id].current).cameras[0].pose;
    CHECK_NEAR(fscs::host::toSdk(current,q.contract,fscs::host::Referential::Eyepoint).x,oldSdk.x,1e-9);
    auto again=Repository::deserialize(Repository::serialize(*loaded,{}),nullptr,nullptr); CHECK(again.has_value()); if (!again) return;
    CHECK_NEAR(again->profiles[id].current["cameras"][0]["positionMetersFRD"][1].get<double>(),-.08,1e-9);
}

TEST_CASE("profile: category defaults and colors validate and remap together") {
    auto doc = fscs::profiles::makeStarterProfile("Colors", "c172", "C172");
    doc["categories"][0]["defaultCameraId"] = doc["cameras"][0]["id"];
    doc["categories"][0]["color"] = "#75baff";
    doc["cameras"][0]["color"] = "#ffd27a";
    CHECK(fscs::profiles::validateProfile(doc).ok());
    const auto snapshot = fscs::profiles::toSnapshot(doc);
    CHECK_EQ(snapshot.categories[0].defaultCameraId, doc["cameras"][0]["id"].get<std::string>());
    fscs::profiles::reassignIds(doc);
    CHECK_EQ(doc["categories"][0]["defaultCameraId"], doc["cameras"][0]["id"]);
    CHECK(fscs::profiles::validateProfile(doc).ok());
    auto bad = doc; bad["categories"][0]["defaultCameraId"] = fscs::profiles::newUuid();
    CHECK(!fscs::profiles::validateProfile(bad).ok());
    bad = doc; bad["cameras"][0]["color"] = "url(invalid)";
    CHECK(!fscs::profiles::validateProfile(bad).ok());
}

TEST_CASE("profiles: per-camera mouse release behavior defaults to retain and validates") {
    auto doc = makeStarterProfile("Mouse", "test", "Test");
    CHECK(!toSnapshot(doc).cameras[0].resetMouseLookOnRelease);
    doc["cameras"][0]["mouseLookOnRelease"] = "reset";
    CHECK(validateProfile(doc).ok());
    CHECK(toSnapshot(doc).cameras[0].resetMouseLookOnRelease);
    doc["cameras"][0]["mouseLookOnRelease"] = "retain";
    CHECK(validateProfile(doc).ok());
    CHECK(!toSnapshot(doc).cameras[0].resetMouseLookOnRelease);
    doc["cameras"][0]["mouseLookOnRelease"] = "unknown";
    CHECK(!validateProfile(doc).ok());
    doc["cameras"][0]["mouseLookOnRelease"] = 1;
    CHECK(!validateProfile(doc).ok());
}

TEST_CASE("bindings: high view IDs and reassignable save shortcut survive reload") {
    auto profile=makeStarterProfile("Slots","test","Test");profile["cameras"][0]["viewId"]=12345;
    CHECK(validateProfile(profile).ok());CHECK(toSnapshot(profile).cameraByViewId(12345)!=nullptr);
    json doc{{"schemaVersion",2},{"bindings",json::array({{{"action","selectView"},{"viewId",12345},{"input",{{"kind","key"},{"definition","F"}}}}})}};
    CHECK(parseBindings(doc).problems.empty());doc=upgradeMovementBindings(doc);
    bool save=false;for(auto& b:parseBindings(doc).bindings)if(b.action==Action::SaveViews){save=true;CHECK_EQ(b.input.definition,std::string("VK_LCONTROL+S"));}CHECK(save);
    doc["bindings"]=json::array();CHECK(upgradeMovementBindings(doc)["bindings"].empty());
}

TEST_CASE("profiles: aircraft tracking stays airport-only through validation import and reload") {
    for (const auto* mode : {"aircraftCockpit", "aircraftExterior", "orbit", "flyby", "free"}) {
        auto p = makeStarterProfile("Tracking", "test", "Test");
        auto& cam = p["cameras"][0];
        cam["mode"] = mode;
        cam["aircraftTracking"] = "both";
        cam["trackingPolicy"] = "on"; // TrackIR/head tracking is a separate cockpit feature.
        const auto beforeValidation = toSnapshot(p);
        CHECK_EQ(beforeValidation.cameras[0].aircraftTracking, std::string("off"));
        auto imported = importProfileCode(exportProfileCode(p, false), nullptr);
        CHECK(imported.has_value());
        if (imported) CHECK_EQ(imported->profile["cameras"][0]["aircraftTracking"], json("off"));
        StoreState state; state.txn = 1;
        state.settings = defaultSettingsDocument();
        state.bindings = defaultBindingsDocument();
        state.profiles[p["profileId"].get<std::string>()].current = p;
        auto loaded = Repository::deserialize(Repository::serialize(state, {}), nullptr, nullptr);
        CHECK(loaded.has_value());
        if (loaded) CHECK_EQ(loaded->profiles.begin()->second.current["cameras"][0]["aircraftTracking"], json("off"));
        const auto result = validateProfile(p);
        CHECK(result.ok());
        CHECK(!result.warnings.empty());
        CHECK_EQ(cam["aircraftTracking"], json("off"));
        CHECK_EQ(cam["trackingPolicy"], json("on"));
        const auto normalized = toSnapshot(p);
        CHECK_EQ(normalized.cameras[0].trackingPolicy, std::string("on"));
    }
    auto airport = makeStarterProfile("Airport", "test", "Test");
    airport["cameras"][0].update({{"mode", "airport"}, {"aircraftTracking", "both"},
                                  {"worldOrigin", {{"lat", 40.}, {"lon", -90.}, {"altitude", 100.}}}});
    CHECK(validateProfile(airport).ok());
    const auto airportSnapshot = toSnapshot(airport);
    CHECK_EQ(airportSnapshot.cameras[0].aircraftTracking, std::string("both"));
}

TEST_CASE("bindings: Q raises and E lowers with stock-only migration and custom preservation") {
    const auto fresh = defaultBindingsDocument();
    auto definition = [](const json& doc, const std::string& action) {
        for (const auto& binding : doc["bindings"])
            if (binding["action"] == action) return binding["input"]["definition"].get<std::string>();
        return std::string();
    };
    CHECK_EQ(fresh["movementDefaultsVersion"], json(2));
    CHECK_EQ(definition(fresh, "moveUp"), std::string("Q"));
    CHECK_EQ(definition(fresh, "moveDown"), std::string("E"));
    auto legacy = fresh;
    legacy["movementDefaultsVersion"] = 1;
    for (auto& binding : legacy["bindings"]) {
        if (binding["action"] == "moveUp") binding["action"] = "moveDown";
        else if (binding["action"] == "moveDown") binding["action"] = "moveUp";
    }
    CHECK(upgradeMovementBindings(legacy) == fresh);
    CHECK(upgradeMovementBindings(fresh) == fresh);
    for (const auto* customization : {"key", "label", "extra", "removed"}) {
        auto custom = legacy;
        if (std::string(customization) == "extra") {
            custom["bindings"].push_back({{"action", "moveUp"}, {"input", {{"kind", "key"}, {"definition", "U"}, {"label", "U"}}}});
        } else {
            for (auto it = custom["bindings"].begin(); it != custom["bindings"].end(); ++it) {
                if ((*it)["action"] != "moveUp") continue;
                if (std::string(customization) == "key") {
                    (*it)["input"]["definition"] = "U"; (*it)["input"]["label"] = "U";
                } else if (std::string(customization) == "label") (*it)["input"]["label"] = "My preferred raise key";
                else custom["bindings"].erase(it);
                break;
            }
        }
        const auto before = custom["bindings"];
        const auto migrated = upgradeMovementBindings(custom);
        CHECK(migrated["bindings"] == before);
        CHECK_EQ(migrated["movementDefaultsVersion"], json(2));
        CHECK(parseBindings(migrated).problems.empty());
    }
}

TEST_CASE("profiles: builtin cameras and categories carry semantic icons through export") {
    const std::set<std::string> expected = {"view-cockpit", "view-instrument", "view-controls", "view-seat", "view-window", "view-front", "view-wing", "view-tail", "plane"};
    for (auto doc : {makeStarterProfile("Starter", "test", "Test"), makeC172Profile("c172", "C172"), makeA320Profile("a320", "A320")}) {
        CHECK(validateProfile(doc).ok());
        const auto imported = json::parse(doc.dump());
        CHECK_EQ(imported["cameras"], doc["cameras"]);
        CHECK_EQ(imported["categories"], doc["categories"]);
        for (const auto* key : {"categories", "cameras"})
            for (const auto& entry : doc[key])
                CHECK(expected.count(entry["icon"].get<std::string>()) != 0);
    }
}

TEST_CASE("settings: tracking defaults are global and invalid values normalize safely") {
    auto settings=fscs::profiles::defaultSettingsDocument();
    CHECK_EQ(settings["host"]["globalHeadTrackingEnabled"],json(false));CHECK_EQ(settings["host"]["globalAircraftTracking"],json("both"));
    settings["host"]["globalHeadTrackingEnabled"]=true;settings["host"]["globalAircraftTracking"]="heading";
    auto parsed=fscs::profiles::parseHostSettings(settings);CHECK(parsed.globalHeadTrackingEnabled);CHECK_EQ(parsed.globalAircraftTracking,std::string("heading"));
    auto normalized=fscs::profiles::normalizeSettings(settings);CHECK_EQ(normalized["host"]["globalHeadTrackingEnabled"],json(true));CHECK_EQ(normalized["host"]["globalAircraftTracking"],json("heading"));
    settings["host"]["globalHeadTrackingEnabled"]="on";settings["host"]["globalAircraftTracking"]="invalid";normalized=fscs::profiles::normalizeSettings(settings);
    CHECK_EQ(normalized["host"]["globalHeadTrackingEnabled"],json(false));CHECK_EQ(normalized["host"]["globalAircraftTracking"],json("both"));
}

TEST_CASE("profiles: flyby pass options validate and survive import normalization") {
    auto doc=makeStarterProfile("Flyby","test","Test");auto& camera=doc["cameras"][0];camera["mode"]="flyby";
    camera["motion"]={{"radiusMeters",150},{"heightMeters",15},{"durationSeconds",12},{"sideMeters",-40},{"repeat",true}};
    CHECK(validateProfile(doc).ok());const auto snapshot=toSnapshot(doc);const auto& cam=snapshot.cameras[0];
    CHECK_NEAR(cam.motionSideMeters,-40,1e-9);CHECK(cam.motionRepeat);CHECK_NEAR(cam.motionDurationSeconds,12,1e-9);
    for (const auto& pair : {std::pair<const char*,json>{"sideMeters",5001},{"repeat","yes"},{"durationSeconds",0}}) {
        auto invalid=doc;invalid["cameras"][0]["motion"][pair.first]=pair.second;CHECK(!validateProfile(invalid).ok());
    }
}

TEST_CASE("settings: airport zoom and category heights normalize and roundtrip") {
    auto settings=defaultSettingsDocument();auto& h=settings["host"];
    CHECK_EQ(h["airportAutoZoomEnabled"],json(false));CHECK_EQ(h["airportAutoZoomFrameMeters"],json(40));
    h["airportAutoZoomEnabled"]=true;h["airportAutoZoomFrameMeters"]=999;h["airportAutoZoomMinFovDeg"]=130;
    h["airportAutoZoomMaxFovDeg"]=1;h["airportAutoZoomResponseSeconds"]=-1;h["airportCategoryHeights"]["tower"]=2000;
    const auto normalized=normalizeSettings(settings);const auto parsed=parseHostSettings(json::parse(normalized.dump()));
    CHECK(parsed.airportAutoZoomEnabled);CHECK_NEAR(parsed.airportAutoZoomFrameMeters,200,1e-9);
    CHECK_NEAR(parsed.airportAutoZoomMinFovDeg,120,1e-9);CHECK_NEAR(parsed.airportAutoZoomMaxFovDeg,120,1e-9);
    CHECK_NEAR(parsed.airportAutoZoomResponseSeconds,.1,1e-9);CHECK_NEAR(parsed.airportCategoryHeights.at("tower"),1000,1e-9);
    CHECK_NEAR(parsed.airportCategoryHeights.at("runways"),3,1e-9);
}
TEST_CASE("profiles: sparse airport overrides normalize without replacing inheritance") {
    auto doc=makeStarterProfile("Airport","test","Test");auto& camera=doc["cameras"][0];camera["mode"]="airport";
    camera["worldOrigin"]={{"lat",40},{"lon",-90},{"altitude",100}};
    camera["airportOverrides"]={{"aircraftTracking","off"},{"autoZoomEnabled",false},{"autoZoomMinFovDeg",40},{"autoZoomMaxFovDeg",10},{"heightMeters",5000}};
    CHECK(validateProfile(doc).ok());const auto imported=json::parse(doc.dump());const auto snapshot=toSnapshot(imported);
    const auto& o=snapshot.cameras[0].airportOverrides;CHECK(o.aircraftTracking && *o.aircraftTracking=="off");CHECK(o.autoZoomEnabled && !*o.autoZoomEnabled);
    CHECK(!o.autoZoomFrameMeters);CHECK_NEAR(*o.autoZoomMinFovDeg,40,1e-9);CHECK_NEAR(*o.autoZoomMaxFovDeg,40,1e-9);CHECK_NEAR(*o.heightMeters,1000,1e-9);
    camera["airportOverrides"]={{"aircraftTracking","bad"},{"autoZoomEnabled","yes"},{"autoZoomResponseSeconds","invalid"}};
    CHECK(validateProfile(doc).ok());CHECK(camera["airportOverrides"].empty());
}

TEST_CASE("repository: global airport preferences and camera overrides survive saved reload") {
    Store store;auto settings=defaultSettingsDocument();settings["host"]["airportAutoZoomEnabled"]=true;
    settings["host"]["airportAutoZoomFrameMeters"]=55;settings["host"]["airportCategoryHeights"]["gates"]=12;
    auto profile=makeStarterProfile("Airport","test","Test");auto& camera=profile["cameras"][0];camera["mode"]="airport";
    camera["worldOrigin"]={{"lat",40},{"lon",-90},{"altitude",100}};
    camera["airportOverrides"]={{"autoZoomEnabled",false},{"aircraftTracking","heading"},{"heightMeters",8}};
    camera["airportHeight"]={{"category","gates"},{"baseAltitudeMeters",100},{"minimumMeters",0}};
    CommitRequest request;request.settings=settings;request.upsertProfiles.push_back(profile);
    CHECK(store.commit(request).status==CommitStatus::Committed);
    Repository reloaded(store.storage,"a","b");reloaded.load([](std::vector<std::string>){});
    const auto parsed=parseHostSettings(reloaded.committed().settings);CHECK(parsed.airportAutoZoomEnabled);
    CHECK_NEAR(parsed.airportAutoZoomFrameMeters,55,1e-9);CHECK_NEAR(parsed.airportCategoryHeights.at("gates"),12,1e-9);
    const auto snapshot=toSnapshot(reloaded.committed().profiles.at(profile["profileId"].get<std::string>()).current);
    const auto& saved=snapshot.cameras[0];CHECK_EQ(saved.airportHeightCategory,std::string("gates"));
    CHECK(saved.airportOverrides.autoZoomEnabled && !*saved.airportOverrides.autoZoomEnabled);
    CHECK_EQ(*saved.airportOverrides.aircraftTracking,std::string("heading"));CHECK_NEAR(*saved.airportOverrides.heightMeters,8,1e-9);
}

TEST_CASE("profiles: airport horizon protection defaults and explicit overrides survive reload") {
    auto settings=defaultSettingsDocument();
    CHECK(parseHostSettings(json::object()).airportTrackingHorizonGuard);
    CHECK_EQ(settings["host"]["airportTrackingHorizonGuard"],json(true));
    settings["host"]["airportTrackingHorizonGuard"]=false;
    CHECK(!parseHostSettings(normalizeSettings(settings)).airportTrackingHorizonGuard);
    auto invalid=settings;invalid["host"]["airportTrackingHorizonGuard"]="false";
    CHECK(parseHostSettings(normalizeSettings(invalid)).airportTrackingHorizonGuard);
    auto profile=makeStarterProfile("Horizon","test","Test");auto& camera=profile["cameras"][0];
    camera["mode"]="airport";camera["worldOrigin"]={{"lat",40},{"lon",-90},{"altitude",100}};
    CHECK(validateProfile(profile).ok());CHECK(!toSnapshot(profile).cameras[0].airportOverrides.trackingHorizonGuard);
    camera["airportOverrides"]={{"trackingHorizonGuard",false}};
    Store store;CommitRequest request;request.settings=settings;request.upsertProfiles.push_back(profile);
    CHECK(store.commit(request).status==CommitStatus::Committed);
    Repository reloaded(store.storage,"a","b");reloaded.load([](std::vector<std::string>){});
    CHECK(!parseHostSettings(reloaded.committed().settings).airportTrackingHorizonGuard);
    const auto snapshot=toSnapshot(reloaded.committed().profiles.at(profile["profileId"].get<std::string>()).current);
    CHECK(snapshot.cameras[0].airportOverrides.trackingHorizonGuard.has_value());
    CHECK(!*snapshot.cameras[0].airportOverrides.trackingHorizonGuard);
    camera["airportOverrides"]["trackingHorizonGuard"]=true;
    CHECK(validateProfile(profile).ok());CHECK(*toSnapshot(profile).cameras[0].airportOverrides.trackingHorizonGuard);
    camera["airportOverrides"]["trackingHorizonGuard"]="off";
    CHECK(validateProfile(profile).ok());CHECK(!toSnapshot(profile).cameras[0].airportOverrides.trackingHorizonGuard);
    camera["airportOverrides"]["trackingHorizonGuard"]=true;camera["mode"]="aircraftCockpit";
    CHECK(validateProfile(profile).ok());CHECK(!camera.contains("airportOverrides"));
}
