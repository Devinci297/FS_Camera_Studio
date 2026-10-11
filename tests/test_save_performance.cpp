#include "test.hpp"
#include "fake_backend.hpp"
#include "fscs/profiles/repository.hpp"
#include "fscs/profiles/json_util.hpp"

using namespace fscs::profiles;
namespace js = fscs::js;
namespace {
struct SavePerfRig {
    fscs::host::FakeStorage driver;
    Repository repo{driver, "a", "b"};
    SavePerfRig() { repo.load([](auto) {}); }
    CommitResult save(CommitRequest request) {
        CommitResult result;
        repo.commit(std::move(request), [&](const auto& r) { result = r; });
        return result;
    }
};
}

TEST_CASE("save: unchanged shortcut data does not rewrite the library or grow backups") {
    SavePerfRig r;
    auto profile = makeStarterProfile("First", "model-first", "First");
    CommitRequest initial; initial.upsertProfiles.push_back(profile);
    CHECK(r.save(initial).status == CommitStatus::Committed);
    const int writes = r.driver.writes;
    const auto txn = r.repo.committed().txn;
    CommitRequest clean;
    clean.settings = r.repo.committed().settings;
    clean.bindings = r.repo.committed().bindings;
    clean.navigation = r.repo.committed().navigation;
    clean.requestId = "save-clean";
    CHECK(r.save(clean).status == CommitStatus::Committed);
    CHECK(r.save(clean).duplicate);
    CHECK_EQ(r.driver.writes, writes);
    CHECK_EQ(r.repo.committed().txn, txn);
    CommitRequest identical;
    identical.upsertProfiles.push_back(r.repo.committed().profiles.begin()->second.current);
    CHECK(r.save(identical).status == CommitStatus::Committed);
    CHECK_EQ(r.driver.writes, writes);
    CHECK(r.repo.committed().profiles.begin()->second.backups.empty());
}

TEST_CASE("save: only changed aircraft mirrors are generated with all same-aircraft alternatives") {
    SavePerfRig r;
    auto a = makeStarterProfile("A", "model-a", "Aircraft A");
    auto alternate = makeStarterProfile("Alternate A", "model-a", "Aircraft A");
    auto b = makeStarterProfile("B", "model-b", "Aircraft B");
    CommitRequest initial; initial.upsertProfiles = {a, alternate, b};
    CHECK(r.save(initial).status == CommitStatus::Committed);
    CHECK_EQ(r.driver.preparedExports.size(), std::size_t{2});
    a["name"] = "Adjusted A";
    CommitRequest edit; edit.upsertProfiles = {a};
    CHECK(r.save(edit).status == CommitStatus::Committed);
    CHECK_EQ(r.driver.preparedExports.size(), std::size_t{1});
    CHECK(r.driver.preparedExports.count(aircraftProfileFile("model-a")) == 1);
    const auto mirror = js::parse(r.driver.preparedExports.begin()->second);
    CHECK_EQ(mirror["alternateProfiles"].size(), std::size_t{1});
    CHECK(importProfileCode(r.driver.preparedExports.begin()->second, nullptr).has_value());
    CommitRequest preference; auto settings = r.repo.committed().settings;
    settings["ui"]["theme"] = "light"; preference.settings = settings;
    CHECK(r.save(preference).status == CommitStatus::Committed);
    CHECK(r.driver.preparedExports.empty());
    CommitRequest selection;
    selection.navigation = json{{"lastByModel", {{"model-a", alternate["profileId"]}}}};
    CHECK(r.save(selection).status == CommitStatus::Committed);
    CHECK_EQ(r.driver.preparedExports.size(), std::size_t{1});
    const auto preferredMirror = js::parse(r.driver.preparedExports.begin()->second);
    CHECK_EQ(preferredMirror["profile"]["profileId"], alternate["profileId"]);
}

TEST_CASE("save: exact readback still rejects corruption and preserves committed state") {
    SavePerfRig r;
    auto profile = makeStarterProfile("First", "model-first", "First");
    CommitRequest initial; initial.upsertProfiles.push_back(profile);
    CHECK(r.save(initial).status == CommitStatus::Committed);
    const auto committed = r.repo.committed().profiles.begin()->second.current;
    const auto txn = r.repo.committed().txn;
    profile["name"] = "Failed edit";
    CommitRequest edit; edit.upsertProfiles.push_back(profile);
    r.driver.tearNextWrite = true;
    CHECK(r.save(edit).status == CommitStatus::Failed);
    CHECK_EQ(r.repo.committed().profiles.begin()->second.current, committed);
    CHECK_EQ(r.repo.committed().txn, txn);
    CHECK(r.save(edit).status == CommitStatus::Committed);
    Repository reload(r.driver, "a", "b"); reload.load([](auto) {});
    CHECK_EQ(reload.committed().profiles.begin()->second.current["name"], "Failed edit");
}
