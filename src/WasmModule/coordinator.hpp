#pragma once
// Camera coordinator (design §5, §6): sole authority that permits a camera submission. Owns ownership state,
// session epoch, acquisition tokens, peek tokens and the motion engine. Single-threaded: every call comes from
// the host's SimConnect thread. Time is injected so tests can drive it deterministically.

#include "backend.hpp"
#include "input_router.hpp"
#include "fscs/core/engine.hpp"
#include "fscs/profiles/bindings.hpp"
#include "fscs/profiles/profile.hpp"
#include "fscs/protocol/protocol.hpp"

#include <functional>
#include <optional>
#include <string>

namespace fscs::host {

using protocol::json;

enum class Ownership { Observing, Acquiring, Active, SimulatorSuspended, OtherClient, UserDisabled, Releasing, Faulted };
const char* toString(Ownership o);

struct Aircraft {
    std::string aircraftPath;
    std::string title;
    std::string modelKey;
    std::string variantKey;
    bool known() const { return !modelKey.empty(); }
};

// "SimObjects\Airplanes\<model>\presets\...\aircraft.cfg" -> model "<model>", variant "presets/..."
Aircraft aircraftFromPath(const std::string& path);

class Coordinator {
public:
    using EmitFn = std::function<void(const std::string& name, json payload)>;
    using QualificationFn = std::function<Qualification(const std::string& simVersion)>;

    static constexpr double kAcquireTimeout = 2.0;
    static constexpr double kReleaseTimeout = 1.5;
    static constexpr double kReadTimeout = 1.0;
    static constexpr double kPreAcquireReadTimeout = 0.3;
    static constexpr double kMaxReconnectBackoff = 10.0;

    Coordinator(ICameraBackend& backend, EmitFn emit, QualificationFn qualify);

    // Protocol command (Hello/Heartbeat/binding capture are handled by the host app). Returns the ack.
    json handle(const protocol::Command& cmd, double now);
    // Fast path: never queued behind other work.
    void release(const std::string& reason, double now);
    void onBackendEvent(const BackendEvent& ev, double now);
    void onAction(const ActionEvent& ev, double now);
    // The working profile (drafts included) the coordinator recalls from. Each call is an immutable revision;
    // nullopt when no profile is open. A change to the active camera previews live while control is authorised.
    void setProfile(std::optional<profiles::ProfileSnapshot> snapshot, double now);
    void setSettings(const profiles::HostSettings& s, double now);
    const profiles::ProfileSnapshot* profile() const { return profile_ ? &*profile_ : nullptr; }
    const std::string& stateReason() const { return stateReason_; }
    bool gameControlled() const { return gameControlled_; }
    const std::string& simVersion() const { return simVersion_; }
    json capabilitiesJson() const;

    // In-panel axis check (gate G01/G03): allows live control on an unqualified build while it runs, and shows
    // probe poses for the active camera through the current adapter contract.
    void setQualificationSession(bool on, double now);
    bool qualificationSession() const { return qualificationSession_; }
    bool previewPose(const core::CameraPose& pose);
    void refreshQualification();
    const Qualification& qualificationState() const { return qual_; }
    void setManualInput(const core::ManualInput& in) { manual_ = in; }
    void tick(double now);

    // State the host app re-sends to a newly connected UI.
    void emitFullState();

    Ownership ownership() const { return state_; }
    std::uint64_t epoch() const { return epoch_; }
    const std::string& activeCameraId() const { return activeCameraId_; }
    bool canActivate() const { return blockReason().empty(); }
    std::string blockReason() const;
    const core::CameraEngine& engine() const { return engine_; }
    const Aircraft& aircraft() const { return aircraft_; }
    std::uint64_t submissions() const { return submissions_; }

private:
    enum class ReadPurpose { None, PreAcquire, Capture, Resume, Seed };

    json ack(const protocol::Command& c, protocol::AckStatus s, const std::string& reason = {});
    bool epochCommand(const std::string& action) const;
    std::string activate(const std::string& cameraId, double now, bool fromPeek);
    void beginAcquire(double now);
    void onAcquired(double now);
    void startTransition(const profiles::CameraEntry& cam, bool cut);
    void setState(Ownership s, const std::string& reason);
    void invalidateSession(const std::string& reason, double now);
    void resetSession(const std::string& reason, double now);
    void failSafe(const std::string& reason, double now);
    void fault(const std::string& code, const std::string& message, double now);
    void onStatus(const backend_event::CameraStatus& st, double now);
    void onCameraData(const backend_event::CameraData& d, double now);
    bool eligibleToSubmit() const;
    void submit();
    void emitCapabilities(bool force = false);
    void emitActiveCamera();
    void emitLook(double now, bool force = false);
    void endPeek(std::uint64_t token, double now);
    std::string cycle(int delta, double now);
    std::string nextCategory(double now);
    const profiles::CameraEntry* activeEntry() const;
    Referential referentialFor(const profiles::CameraEntry& cam) const;
    void requestRead(ReadPurpose p, Referential ref, double now, double timeout);

    ICameraBackend& backend_;
    EmitFn emit_;
    QualificationFn qualify_;

    // Connection / session
    bool simConnected_ = false;
    std::string simVersion_;
    std::uint64_t epoch_ = 1;
    double nextConnectAt_ = 0, backoff_ = 1.0;
    Aircraft aircraft_;
    std::optional<profiles::ProfileSnapshot> profile_;
    profiles::HostSettings settings_;
    Qualification qual_;

    // Ownership
    Ownership state_ = Ownership::Observing;
    std::string stateReason_ = "Simulator in control";
    SdkAvailability sdk_ = SdkAvailability::NotAcquired;
    bool gameControlled_ = false;
    std::uint64_t acquireToken_ = 0, tokenCounter_ = 0;
    double acquireDeadline_ = 0;
    bool acquireSent_ = false;
    bool cleanupPending_ = false;
    double cleanupDeadline_ = 0;
    double releaseDeadline_ = 0;
    bool faultAfterRelease_ = false;
    std::string pendingCameraId_;
    std::string afterReleaseCameraId_;

    // Motion
    std::string activeCameraId_;
    core::CameraEngine engine_;
    core::ManualInput manual_;
    double lastTick_ = -1;
    std::optional<SdkPose> lastSubmitted_;
    std::uint64_t submissions_ = 0;
    bool lastTransitioning_ = false;
    double lastLookEmit_ = 0;
    bool lastLookActive_ = false;

    struct Peek {
        std::uint64_t token;
        std::string previousCameraId;
        bool releaseOnEnd;
    };
    std::optional<Peek> peek_;

    // Reads (CameraGet has no correlation id: one outstanding read, quarantined on timeout)
    ReadPurpose read_ = ReadPurpose::None;
    double readDeadline_ = 0;
    std::optional<core::CameraPose> preAcquirePose_;
    bool readsUnreliable_ = false;
    bool qualificationSession_ = false;  // set after a read timeout: a late reply could be misread, so reads stay off
    std::string seedName_;

    json lastCapabilities_;
};

}  // namespace fscs::host
