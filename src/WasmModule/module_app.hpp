#pragma once
// The resident module (design 2.0 §5): authoritative for the working draft, commits, aircraft/profile matching,
// navigation selection, panel subscriptions and input routing. Panels are mirrors that send intent
// (doc/protocol.md). Pure logic over interfaces; the WASM entry points in wasm/ feed it events and frames.

#include "backend.hpp"
#include "coordinator.hpp"
#include "input_router.hpp"
#include "fscs/profiles/repository.hpp"
#include "fscs/protocol/protocol.hpp"

#include <deque>
#include <map>
#include <optional>
#include <string>

namespace fscs::host {

class ModuleApp {
public:
    static constexpr double kLeaseSeconds = 6.0;
    static constexpr double kGestureLeaseSeconds = 0.15;
    static constexpr double kStateFlushSeconds = 0.1;
    static constexpr double kInputTestSeconds = 15.0;

    ModuleApp(ICameraBackend& camera, IModuleServices& services, profiles::IStorageDriver& storage, std::string bootId,
              std::string appVersion);

    void start(double now);
    void onEvent(const BackendEvent& ev, double now);
    void update(double now);
    void shutdown(double now);

    const Coordinator& coordinator() const { return coord_; }
    const profiles::Repository& repository() const { return repo_; }
    bool ready() const { return repo_.loaded(); }
    const json& workingProfile() const { return working_.profile; }
    std::int64_t draftRevision() const { return working_.draftRevision; }
    bool dirty() const { return working_.dirtyProfile || working_.dirtySettings || working_.dirtyBindings; }
    const std::string& bootId() const { return bootId_; }

private:
    struct Working {
        bool open = false;
        json profile;   // working draft (may be null when no profile exists yet)
        json settings;
        json bindings;
        std::int64_t draftRevision = 0;
        bool dirtyProfile = false, dirtySettings = false, dirtyBindings = false;
    };
    struct Instance {
        std::string surface;
        double lastRenew = 0;
        bool visible = true;
        bool lapsed = false;
    };
    struct Gesture {
        core::ManualInput input;
        double expires = 0;
    };
    struct PendingCommit {
        std::string requestId;  // panel request id (for CommitCompleted)
        std::string kind;       // "save", "create", "delete", "bind", "import", "navigation"
        std::int64_t draftRevision = 0;
        json report;
        std::string openAfter;  // profile to open once committed
    };

    // Panel messages
    void onPanelText(const std::string& text, double now);
    void handle(const protocol::Command& c, double now);
    void reply(const protocol::Command& c, protocol::AckStatus s, const std::string& reason = {}, json result = nullptr);
    void send(const json& message);
    void emitState(const std::string& name, json payload);  // coalesced latest-state event
    void emitNow(const std::string& name, json payload, const std::string& to = {});
    void flushState(double now, bool force);
    void broadcastSnapshot(const std::string& to = {});
    json snapshot() const;
    bool anyVisibleLease() const;

    // Coordinator bridge
    void onCoordinatorEvent(const std::string& name, json payload);
    json forward(const protocol::Command& c, double now);

    // Working set
    void loadFromRepository(std::vector<std::string> notes, double now);
    void openProfile(const std::string& profileId, double now);
    void publishWorking(double now, bool bindingsChanged);
    void recomputeDirty();
    bool applyOps(const json& ops, json& applied, std::string& error);
    json committedProfile(const std::string& profileId) const;
    void resolveAircraftProfile(double now);
    json profilesList() const;
    json navigation() const;
    void commit(profiles::CommitRequest req, PendingCommit pending, double now);
    void selectCamera(const std::string& cameraId);
    std::string selectionFallback() const;
    Qualification qualification(const std::string& simVersion) const;
    void diag(const std::string& line);

    IModuleServices& services_;
    profiles::IStorageDriver& storage_;
    profiles::Repository repo_;
    Coordinator coord_;
    InputRouter router_;
    std::string bootId_, appVersion_;

    Working working_;
    std::string selection_;
    json lastCamera_ = json::object();  // profileId -> cameraId (navigation metadata)
    std::optional<PendingCommit> pendingCommit_;
    std::deque<std::pair<profiles::CommitRequest, PendingCommit>> commitQueue_;

    std::map<std::string, Instance> instances_;
    std::map<std::string, Gesture> gestures_;
    std::map<std::string, json> pendingState_;
    double lastFlush_ = 0;
    std::uint64_t seq_ = 0, msgCounter_ = 0;
    protocol::Reassembler reassembler_;
    bool paused_ = false;
    bool simRunning_ = true;

    std::string aircraftModelKey_, aircraftTitle_;
    bool resolveAircraft_ = false;
    json lastAircraft_ = json::object();
    std::string captureRequester_;
    std::string testDefinition_;
    double testUntil_ = 0;
    std::vector<std::pair<int, std::string>> controllers_;
    std::deque<std::string> diagnostics_;
    bool diagDirty_ = false, diagWriting_ = false;
    double lastDiagWrite_ = -10;
    void writeDiagnostics(double now);
    double now_ = 0;
};

}  // namespace fscs::host
