#include "test.hpp"
#include "wasm/wasm_storage.hpp"
#include "fscs/profiles/json_util.hpp"

#include <filesystem>

using namespace fscs;

namespace {
struct LocalFiles {
    std::filesystem::path root = std::filesystem::temp_directory_path() / ("fscs-storage-" + profiles::newUuid());
    LocalFiles() { std::filesystem::create_directory(root); }
    ~LocalFiles() { for (const auto& file : std::filesystem::directory_iterator(root)) std::filesystem::remove(file.path()); std::filesystem::remove(root); }
    std::string path(const char* name) const { return (root / name).string(); }
};
}

TEST_CASE("storage: existing local files complete repository startup and preserve saved profiles") {
    LocalFiles files;
    host::WasmStorage driver;
    profiles::Repository first(driver, files.path("a.json"), files.path("b.json"));
    bool loaded = false;
    first.load([&](auto) { loaded = true; });
    CHECK(loaded);
    const auto doc = profiles::makeC172Profile("asobo_c172sp_g1000", "C172 G1000");
    profiles::CommitRequest req;
    req.upsertProfiles.push_back(doc);
    bool saved = false;
    first.commit(req, [&](const auto& result) { saved = result.status == profiles::CommitStatus::Committed; });
    CHECK(saved);
    profiles::Repository second(driver, files.path("a.json"), files.path("b.json"));
    loaded = false;
    second.load([&](auto) { loaded = true; });
    CHECK(loaded);
    CHECK_EQ(second.committed().profiles.size(), std::size_t{1});
    CHECK_EQ(second.committed().profiles.begin()->second.current["cameras"].size(), std::size_t{20});
}

TEST_CASE("storage: empty, missing and failed writes always complete their callbacks") {
    LocalFiles files;
    host::WasmStorage driver;
    int callbacks = 0;
    driver.readFile(files.path("missing.json"), [&](auto bytes) { ++callbacks; CHECK(!bytes); });
    driver.writeFile(files.path("empty.json"), "", [&](bool ok, auto) { ++callbacks; CHECK(ok); });
    driver.readFile(files.path("empty.json"), [&](auto bytes) { ++callbacks; CHECK(bytes && bytes->empty()); });
    driver.writeFile(files.path("missing/failed.json"), "test", [&](bool ok, auto error) { ++callbacks; CHECK(!ok); CHECK(!error.empty()); });
    CHECK_EQ(callbacks, 4);
}
