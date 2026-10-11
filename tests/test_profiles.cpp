#include "test.hpp"

#include "fake_backend.hpp"
#include "fscs/profiles/bindings.hpp"
#include "fscs/profiles/json_util.hpp"
#include "fscs/profiles/repository.hpp"
#include "fscs/protocol/protocol.hpp"

using namespace fscs::profiles;
using fscs::host::FakeStorage;

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
    p["cameras"][1]["mode"] = "orbit";
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
