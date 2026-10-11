#pragma once
// Recoverable in-sim persistence (design 2.0 §8.2). The WASM IO API offers open/read/write/close only — no rename,
// delete needed by this protocol — so the store is two fixed slot files, each a complete checksummed snapshot of every profile
// (with up to five backups), settings, bindings and navigation metadata. A commit overwrites only the OLDER slot,
// reads it back and verifies it before it counts; loading picks the highest valid transaction. One commit carries
// profile, settings and bindings together, so a partial write can never publish half a transaction.

#include "fscs/profiles/profile.hpp"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace fscs::profiles {

using json = nlohmann::json;

// Asynchronous whole-file storage. Callbacks may run later (WASM IO) or immediately (tests).
class IStorageDriver {
public:
    virtual ~IStorageDriver() = default;
    // Readable mirrors for this transaction only; the checksummed slot remains authoritative.
    virtual void prepareProfileExports(std::map<std::string, std::string>) {}
    // Nonrecursive JSON candidates from the dedicated persistent import folder.
    virtual void listImportFiles(std::function<void(std::vector<std::string>, std::string)> done) {
        done({}, "Folder listing is unavailable in this host.");
    }
    // Creates/truncates and writes the whole file; `done(ok, error)` after the file is written and closed.
    virtual void writeFile(const std::string& path, std::string bytes, std::function<void(bool, std::string)> done) = 0;
    // Reads the whole file; `done(nullopt)` when missing or unreadable.
    virtual void readFile(const std::string& path, std::function<void(std::optional<std::string>)> done) = 0;
};

inline constexpr const char* kProfileImportFolder = "\\work\\imports\\";
bool validProfileImportFilename(const std::string& name);

constexpr std::size_t kMaxStoreBytes = 24u << 20;
constexpr std::size_t kBackupsPerProfile = 5;

// One human-readable current-profile file per aircraft model; copies share that aircraft file.
std::string aircraftProfileFile(const std::string& modelKey);
std::map<std::string, std::string> aircraftProfileFiles(const json& storeEnvelope);

struct ProfileRecord {
    json current;               // committed revision
    std::vector<json> backups;  // previous committed revisions, newest first
};

struct StoreState {
    std::int64_t txn = 0;
    std::map<std::string, ProfileRecord> profiles;
    json settings;
    json bindings;
    json navigation = json::object();  // {"lastProfileId": ..., "lastCamera": {profileId: cameraId}}
};

struct CommitRequest {
    std::vector<json> upsertProfiles;     // validated schema-2 documents
    std::vector<std::string> removeProfiles;
    std::optional<json> settings;
    std::optional<json> bindings;
    std::optional<json> navigation;
    bool backup = true;
    std::string requestId;                // recorded so a retried request cannot commit twice
};

enum class CommitStatus { Committed, Failed };

struct CommitResult {
    CommitStatus status = CommitStatus::Failed;
    std::string message;
    std::int64_t txn = 0;
    bool duplicate = false;  // the requestId was already committed
};

class Repository {
public:
    using LoadDone = std::function<void(std::vector<std::string> notes)>;
    using CommitDone = std::function<void(const CommitResult&)>;

    Repository(IStorageDriver& driver, std::string slotA, std::string slotB);

    void load(LoadDone done);
    void commit(CommitRequest req, CommitDone done);

    bool loaded() const { return loaded_; }
    bool busy() const { return active_ || !queue_.empty(); }
    const StoreState& committed() const { return state_; }

    // Exposed for tests and diagnostics.
    static std::string serialize(const StoreState& s, const std::vector<std::string>& recentRequestIds);
    static std::optional<StoreState> deserialize(const std::string& bytes, std::vector<std::string>* notes,
                                                 std::vector<std::string>* recentRequestIds);

private:
    void startNext();
    void finish(CommitResult r);

    IStorageDriver& driver_;
    std::string slots_[2];
    int currentSlot_ = -1;  // slot holding state_, -1 when nothing valid is on disk
    bool loaded_ = false;
    StoreState state_;
    std::vector<std::string> recentRequestIds_;  // bounded, persisted with the state

    struct Pending {
        CommitRequest req;
        CommitDone done;
    };
    std::deque<Pending> queue_;
    bool active_ = false;
};

// Profile code exchange (design 2.0 §4.7, §8.2): bounded, validated data — never executed.
std::string exportProfileCode(const json& profile, bool workingCopy);

struct ImportReport {
    json profile;  // validated, all UUIDs remapped, revision 0
    std::size_t cameraCount = 0;
    std::size_t unsupportedCount = 0;
    std::vector<std::string> notes;
};
// Returns nullopt with a plain-language `error` when the code is not importable.
std::optional<ImportReport> importProfileCode(const std::string& code, std::string* error);

}  // namespace fscs::profiles
