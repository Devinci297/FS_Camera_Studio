#include "coordinator.hpp"

#include "fscs/profiles/json_util.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>

namespace fscs::host {

using profiles::Action;
using profiles::CameraEntry;
using protocol::AckStatus;

namespace {

json poseJson(const core::CameraPose& p) {
    return {{"positionMetersFRD", {p.positionFRD.x, p.positionFRD.y, p.positionFRD.z}},
            {"orientationCameraToAnchorXYZW", {p.orientation.x, p.orientation.y, p.orientation.z, p.orientation.w}},
            {"fovRad", p.fovRad}};
}

const char* referentialName(Referential r) {
    switch (r) {
        case Referential::Eyepoint: return "eyepoint";
        case Referential::SimObject: return "simObject";
        case Referential::Datum: return "simObjectDatum";
        case Referential::World: return "world";
    }
    return "eyepoint";
}

bool nearlyEqual(const SdkPose& a, const SdkPose& b) {
    constexpr double e = 1e-6;
    return a.referential == b.referential && std::abs(a.x - b.x) < e && std::abs(a.y - b.y) < e && std::abs(a.z - b.z) < e &&
           std::abs(a.pitchDeg - b.pitchDeg) < e && std::abs(a.bankDeg - b.bankDeg) < e &&
           std::abs(a.headingDeg - b.headingDeg) < e && std::abs(a.fovRad - b.fovRad) < e;
}


}  // namespace

const char* toString(Ownership o) {
    switch (o) {
        case Ownership::Observing: return "Observing";
        case Ownership::Acquiring: return "Acquiring";
        case Ownership::Active: return "Active";
        case Ownership::SimulatorSuspended: return "SimulatorSuspended";
        case Ownership::OtherClient: return "OtherClient";
        case Ownership::UserDisabled: return "UserDisabled";
        case Ownership::Releasing: return "Releasing";
        case Ownership::Faulted: return "Faulted";
    }
    return "Observing";
}

Aircraft aircraftFromPath(const std::string& path) {
    Aircraft a;
    a.aircraftPath = path;
    std::vector<std::string> parts;
    std::string cur;
    for (char ch : path) {
        if (ch == '\\' || ch == '/') {
            if (!cur.empty()) parts.push_back(cur);
            cur.clear();
        } else {
            cur.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
        }
    }
    if (!cur.empty()) parts.push_back(cur);
    if (parts.empty()) return a;
    std::size_t model = std::string::npos;
    for (std::size_t i = 0; i + 2 < parts.size(); ++i)
        if (parts[i] == "simobjects") {
            model = i + 2;
            break;
        }
    if (model == std::string::npos) model = parts.size() >= 2 ? parts.size() - 2 : 0;
    a.modelKey = parts[model];
    for (std::size_t i = model + 1; i + 1 < parts.size(); ++i) a.variantKey += (a.variantKey.empty() ? "" : "/") + parts[i];
    return a;
}

Coordinator::Coordinator(ICameraBackend& backend, EmitFn emit, QualificationFn qualify)
    : backend_(backend), emit_(std::move(emit)), qualify_(std::move(qualify)) {}

json Coordinator::ack(const protocol::Command& c, AckStatus s, const std::string& reason) {
    return protocol::makeAck(c, s, reason);
}

bool Coordinator::epochCommand(const std::string& a) const {
    return a == "ActivateCamera" || a == "BeginPeek" || a == "CycleCamera" || a == "NextCategory" ||
           a == "ReturnToPilot" || a == "CaptureView" || a == "SeedFromDefinition" || a == "BakeTemporaryLook";
}

std::string Coordinator::blockReason() const {
    if (!simConnected_) return "Microsoft Flight Simulator 2024 is not running or not connected.";
    if (!aircraft_.known()) return "Load a flight to use your cameras.";
    if (!profile_) return "No camera profile is open for this aircraft.";
    if (profile_->modelKey != aircraft_.modelKey) return "The open profile belongs to another aircraft.";
    if (profile_->readOnly) return "This profile needs a newer version of Camera Studio.";
    if (!qual_.adapterQualified && !settings_.allowUnqualifiedAdapter && !qualificationSession_)
        return "Axis mapping for simulator build " + (simVersion_.empty() ? std::string("(unknown)") : simVersion_) +
               " is not checked yet. Run the axis check in Settings > Simulator.";
    if (state_ == Ownership::UserDisabled) return "Camera control by add-ons is disabled in the simulator.";
    return {};
}

const CameraEntry* Coordinator::activeEntry() const {
    return profile_ && !activeCameraId_.empty() ? profile_->camera(activeCameraId_) : nullptr;
}

Referential Coordinator::referentialFor(const CameraEntry& cam) const {
    return cam.mode == profiles::CameraMode::AircraftExterior ? Referential::SimObject : Referential::Eyepoint;
}

void Coordinator::setState(Ownership s, const std::string& reason) {
    if (s == state_ && reason == stateReason_) return;
    state_ = s;
    stateReason_ = reason;
    emit_("OwnershipChanged", {{"state", toString(s)}, {"gameControlled", gameControlled_}, {"reason", reason}});
    emitCapabilities();
}

json Coordinator::capabilitiesJson() const {
    const std::string block = blockReason();
    json modes = json::object();
    for (auto m : {profiles::CameraMode::AircraftCockpit, profiles::CameraMode::AircraftExterior, profiles::CameraMode::Orbit,
                   profiles::CameraMode::Flyby, profiles::CameraMode::Airport, profiles::CameraMode::Free}) {
        const bool ok = profiles::modeActivatable(m);
        modes[profiles::toString(m)] = {{"available", ok},
                                        {"preview", m == profiles::CameraMode::AircraftExterior},
                                        {"reason", ok ? (m == profiles::CameraMode::AircraftExterior ? json("Preview: exterior views keep cockpit sound in the current simulator build.") : json(nullptr))
                                                      : json("Not available in this version. Saved with the profile but cannot be activated.")}};
    }
    return {{"adapterQualified", qual_.adapterQualified},
              {"adapterContract", qual_.contract.id},
              {"captureQualified", qual_.captureQualified && !readsUnreliable_},
              {"allowUnqualifiedAdapter", settings_.allowUnqualifiedAdapter},
              {"canActivate", block.empty()},
              {"blockReason", block.empty() ? json(nullptr) : json(block)},
              {"simVersion", simVersion_.empty() ? json(nullptr) : json(simVersion_)},
              {"fovRangeDeg", {30, 110}},
              {"levelHorizon", false},
              {"modes", modes}};
}

void Coordinator::emitCapabilities(bool force) {
    json caps = capabilitiesJson();
    if (!force && caps == lastCapabilities_) return;
    lastCapabilities_ = caps;
    emit_("CapabilitiesChanged", caps);
}

void Coordinator::emitActiveCamera() {
    emit_("ActiveCameraChanged", {{"profileId", profile_ ? json(profile_->profileId) : json(nullptr)},
                                  {"cameraId", activeCameraId_.empty() ? json(nullptr) : json(activeCameraId_)},
                                  {"peeking", peek_.has_value()},
                                  {"transitioning", engine_.transitioning()}});
    lastTransitioning_ = engine_.transitioning();
}

void Coordinator::emitLook(double now, bool force) {
    const auto& l = engine_.look();
    const bool active = l.active();
    if (!force && !active && !lastLookActive_) return;
    if (!force && now - lastLookEmit_ < 0.1) return;
    lastLookEmit_ = now;
    lastLookActive_ = active;
    emit_("TemporaryLookChanged",
          {{"active", active},
           {"positionMetersFRD", {l.translationHeadingFrame.x, l.translationHeadingFrame.y, l.translationHeadingFrame.z}},
           {"pbhDeg", {l.pitchDeg, 0.0, l.headingDeg}},
           {"fovDeltaRad", l.fovDeltaRad}});
}

void Coordinator::emitFullState() {
    emit_("ConnectionChanged", {{"simConnected", simConnected_}, {"sessionEpoch", epoch_},
                                {"simVersion", simVersion_.empty() ? json(nullptr) : json(simVersion_)}, {"reason", nullptr}});
    emit_("AircraftChanged", aircraft_.known()
                                 ? json{{"title", aircraft_.title}, {"aircraftPath", aircraft_.aircraftPath},
                                        {"modelKey", aircraft_.modelKey}, {"livery", aircraft_.title}}
                                 : json{{"title", nullptr}, {"aircraftPath", nullptr}, {"modelKey", nullptr}, {"livery", nullptr}});
    emit_("OwnershipChanged", {{"state", toString(state_)}, {"gameControlled", gameControlled_}, {"reason", stateReason_}});
    emitCapabilities(true);
    emitActiveCamera();
    emitLook(0, true);
}

void Coordinator::setProfile(std::optional<profiles::ProfileSnapshot> snapshot, double now) {
    const bool same = profile_ && snapshot && profile_->profileId == snapshot->profileId;
    profile_ = std::move(snapshot);
    emitCapabilities();
    if (!profile_) {
        if (state_ == Ownership::Active || state_ == Ownership::SimulatorSuspended || state_ == Ownership::Acquiring)
            release("No camera profile is open.", now);
        return;
    }
    if (state_ == Ownership::Active && !activeCameraId_.empty()) {
        const auto block = blockReason();
        const CameraEntry* cam = profile_->camera(activeCameraId_);
        if (!block.empty()) {
            release(block, now);
        } else if (!same || !cam || !profiles::modeActivatable(cam->mode)) {
            // The active view vanished (revert, delete, profile switch): release before replacing it and require
            // a deliberate recall (design 2.0 §4.7).
            release(same ? "The active view is no longer in the profile. Select a view to continue."
                         : "A different profile was opened. Select a view to continue.", now);
        } else {
            backend_.setFlag(CameraFlag::Interaction, cam->interaction);
            const auto base = engine_.basePose();
            const double delta = base ? (base->positionFRD - cam->pose.positionFRD).length() +
                                            core::angleBetween(base->orientation, cam->pose.orientation) +
                                            std::abs(base->fovRad - cam->pose.fovRad)
                                      : 1.0;
            if (delta > 1e-9) engine_.place(cam->pose);  // live preview of the edited draft
        }
    }
    emitActiveCamera();
}

void Coordinator::setQualificationSession(bool on, double now) {
    qualificationSession_ = on;
    emitCapabilities();
    if (!on && (state_ == Ownership::Active || state_ == Ownership::SimulatorSuspended) && !blockReason().empty())
        release(blockReason(), now);
}

bool Coordinator::previewPose(const core::CameraPose& pose) {
    if (state_ != Ownership::Active || activeCameraId_.empty() || !pose.withinLimits()) return false;
    engine_.place(pose);
    return true;
}

void Coordinator::refreshQualification() {
    qual_ = qualify_(simVersion_);
    lastSubmitted_.reset();  // the contract may have changed: resend the current pose
    emitCapabilities();
}

void Coordinator::setSettings(const profiles::HostSettings& s, double now) {
    settings_ = s;
    engine_.setSpeeds({settings_.lookSpeedDegPerSec, settings_.moveSpeedMetersPerSec, 0.5, 6.0});
    emitCapabilities();
    if ((state_ == Ownership::Active || state_ == Ownership::SimulatorSuspended) && !blockReason().empty())
        release(blockReason(), now);
}

json Coordinator::handle(const protocol::Command& c, double now) {
    if (epochCommand(c.action) && c.sessionEpoch != epoch_) return ack(c, AckStatus::Rejected, "staleEpoch");
    const auto& p = c.payload;

    if (c.action == "ActivateCamera") {
        const auto r = activate(js::str(p, "cameraId"), now, false);
        return r.empty() ? ack(c, AckStatus::Accepted) : ack(c, AckStatus::Rejected, r);
    }
    if (c.action == "ReturnToPilot") {
        if (!profile_) return ack(c, AckStatus::Rejected, "No camera profile is open.");
        const auto r = activate(profile_->defaultCameraId, now, false);
        return r.empty() ? ack(c, AckStatus::Accepted) : ack(c, AckStatus::Rejected, r);
    }
    if (c.action == "CycleCamera") {
        const auto r = cycle(js::integer(p, "delta", 1) < 0 ? -1 : 1, now);
        return r.empty() ? ack(c, AckStatus::Accepted) : ack(c, AckStatus::Rejected, r);
    }
    if (c.action == "NextCategory") {
        const auto r = nextCategory(now);
        return r.empty() ? ack(c, AckStatus::Accepted) : ack(c, AckStatus::Rejected, r);
    }
    if (c.action == "BeginPeek") {
        if (peek_) return ack(c, AckStatus::Rejected, "Already peeking.");
        const std::uint64_t token = static_cast<std::uint64_t>(std::max<std::int64_t>(0, js::integer(p, "peekToken", 0)));
        if (token == 0) return ack(c, AckStatus::Rejected, "missing peekToken");
        Peek pk{token, state_ == Ownership::Active ? activeCameraId_ : std::string(), state_ != Ownership::Active};
        const auto r = activate(js::str(p, "cameraId"), now, true);
        if (!r.empty()) return ack(c, AckStatus::Rejected, r);
        peek_ = pk;
        emitActiveCamera();
        return ack(c, AckStatus::Accepted);
    }
    if (c.action == "EndPeek") {
        endPeek(static_cast<std::uint64_t>(std::max<std::int64_t>(0, js::integer(p, "peekToken", 0))), now);
        return ack(c, AckStatus::Completed);
    }
    if (c.action == "ReleaseCamera") {
        release("Camera returned to the simulator", now);
        return ack(c, AckStatus::Completed);
    }
    if (c.action == "CancelMotion") {
        if (auto b = engine_.basePose()) engine_.place(*b);
        emitActiveCamera();
        return ack(c, AckStatus::Completed);
    }
    if (c.action == "RecenterLook") {
        engine_.recenterLook();
        emitLook(now, true);
        return ack(c, AckStatus::Completed);
    }
    if (c.action == "BakeTemporaryLook") {
        const CameraEntry* cam = activeEntry();
        if (state_ != Ownership::Active || !cam || !engine_.hasPose())
            return ack(c, AckStatus::Rejected, "Select a camera in the simulator first.");
        const auto s = engine_.sample();
        if (!s.valid) return ack(c, AckStatus::Rejected, "The current view is outside the allowed range.");
        json payload = poseJson(s.pose);
        payload["cameraId"] = cam->id;
        engine_.recenterLook();
        engine_.place(s.pose);
        emit_("TemporaryLookBaked", payload);
        emitLook(now, true);
        return ack(c, AckStatus::Completed);
    }
    if (c.action == "CaptureView") {
        if (state_ == Ownership::Active && engine_.hasPose()) {
            const auto s = engine_.sample();
            const CameraEntry* cam = activeEntry();
            json payload = poseJson(s.pose);
            payload.update({{"ok", s.valid}, {"frame", referentialName(cam ? referentialFor(*cam) : Referential::Eyepoint)},
                            {"qualified", true}, {"source", "addonCamera"},
                            {"message", "Captured the view Camera Studio is showing."}});
            emit_("ViewCaptured", payload);
            return ack(c, AckStatus::Completed);
        }
        if (!simConnected_) return ack(c, AckStatus::Rejected, "Microsoft Flight Simulator 2024 is not connected.");
        if (readsUnreliable_) {
            emit_("ViewCaptured", {{"ok", false}, {"message", "Reading the simulator's view is unavailable on this connection. "
                                                              "Start from a native camera definition instead."}});
            return ack(c, AckStatus::Completed);
        }
        if (read_ != ReadPurpose::None) return ack(c, AckStatus::Rejected, "Another camera read is in progress.");
        requestRead(ReadPurpose::Capture, Referential::Eyepoint, now, kReadTimeout);
        return ack(c, AckStatus::Accepted);
    }
    if (c.action == "EnumerateDefinitions") {
        if (!simConnected_ || !backend_.enumerateDefinitions())
            return ack(c, AckStatus::Rejected, "Microsoft Flight Simulator 2024 is not connected.");
        return ack(c, AckStatus::Accepted);
    }
    if (c.action == "SeedFromDefinition") {
        const std::string name = js::str(p, "name");
        if (name.empty()) return ack(c, AckStatus::Rejected, "missing name");
        if (const auto block = blockReason(); !block.empty()) return ack(c, AckStatus::Rejected, block);
        if (readsUnreliable_) return ack(c, AckStatus::Rejected, "Reading views back is unavailable on this connection.");
        peek_.reset();
        seedName_ = name;
        if (state_ == Ownership::Active) {
            activeCameraId_.clear();
            engine_.reset();
            lastSubmitted_.reset();
            backend_.setUsingDefinition(name);
            requestRead(ReadPurpose::Seed, Referential::Eyepoint, now, kReadTimeout);
            emitActiveCamera();
            return ack(c, AckStatus::Accepted);
        }
        if (state_ == Ownership::Observing || state_ == Ownership::Faulted || state_ == Ownership::OtherClient) {
            pendingCameraId_.clear();
            beginAcquire(now);
            return ack(c, AckStatus::Accepted);
        }
        seedName_.clear();
        return ack(c, AckStatus::Rejected, "Camera Studio is busy; try again in a moment.");
    }
    return ack(c, AckStatus::Rejected, "unknownAction");
}

std::string Coordinator::activate(const std::string& cameraId, double now, bool fromPeek) {
    if (!fromPeek) peek_.reset();
    if (!profile_) return "No camera profile is open.";
    const CameraEntry* cam = profile_->camera(cameraId);
    if (!cam) return "That camera is not in the open profile.";
    if (!profiles::modeActivatable(cam->mode)) return "This view type is not available in this version.";
    if (const auto block = blockReason(); !block.empty()) return block;
    seedName_.clear();
    switch (state_) {
        case Ownership::Active:
            activeCameraId_ = cameraId;
            startTransition(*cam, false);
            emitActiveCamera();
            return {};
        case Ownership::Acquiring:
            pendingCameraId_ = cameraId;
            return {};
        case Ownership::SimulatorSuspended:
            activeCameraId_ = cameraId;  // applied when the simulator hands the camera back
            return {};
        case Ownership::Releasing:
            afterReleaseCameraId_ = cameraId;
            return {};
        case Ownership::UserDisabled:
            return "Camera control by add-ons is disabled in the simulator.";
        case Ownership::Observing:
        case Ownership::Faulted:
        case Ownership::OtherClient:
            if (cleanupPending_) return "Camera Studio is still withdrawing an earlier request. Try again in a moment.";
            pendingCameraId_ = cameraId;
            beginAcquire(now);
            return {};
    }
    return "unexpected state";
}

void Coordinator::requestRead(ReadPurpose p, Referential ref, double now, double timeout) {
    if (read_ != ReadPurpose::None) return;
    if (!backend_.requestCameraData(ref)) {
        if (p == ReadPurpose::Capture || p == ReadPurpose::Seed)
            emit_("ViewCaptured", {{"ok", false}, {"message", "The simulator did not accept the camera read."}});
        return;
    }
    read_ = p;
    readDeadline_ = now + timeout;
}

void Coordinator::beginAcquire(double now) {
    acquireToken_ = ++tokenCounter_;
    acquireDeadline_ = now + kAcquireTimeout;
    acquireSent_ = false;
    preAcquirePose_.reset();
    setState(Ownership::Acquiring, "Taking camera control…");
    const CameraEntry* cam = profile_ ? profile_->camera(pendingCameraId_) : nullptr;
    if (cam && qual_.captureQualified && !readsUnreliable_ && read_ == ReadPurpose::None) {
        requestRead(ReadPurpose::PreAcquire, referentialFor(*cam), now, kPreAcquireReadTimeout);
        if (read_ == ReadPurpose::PreAcquire) return;  // acquire once the read answers
    }
    acquireSent_ = backend_.acquire("FS Camera Studio");
    if (!acquireSent_) {
        acquireToken_ = 0;
        setState(Ownership::Observing, "The simulator did not accept the camera request.");
    }
}

void Coordinator::startTransition(const CameraEntry& cam, bool cut) {
    backend_.setFlag(CameraFlag::Interaction, cam.interaction);
    core::TransitionSpec spec = cam.transition;
    if (cut) spec.easing = core::Easing::Cut;
    if (!engine_.hasPose()) engine_.place(cam.pose);
    else engine_.select(cam.pose, spec, !cam.retainLook);
}

void Coordinator::onAcquired(double now) {
    acquireToken_ = 0;
    lastSubmitted_.reset();
    engine_.reset();
    setState(Ownership::Active, "Camera active");
    if (!seedName_.empty()) {
        activeCameraId_.clear();
        backend_.setUsingDefinition(seedName_);
        requestRead(ReadPurpose::Seed, Referential::Eyepoint, now, kReadTimeout);
        emitActiveCamera();
        return;
    }
    activeCameraId_ = pendingCameraId_;
    pendingCameraId_.clear();
    const CameraEntry* cam = activeEntry();
    if (!cam) {
        activeCameraId_.clear();
        emitActiveCamera();
        return;
    }
    if (preAcquirePose_) {
        engine_.place(*preAcquirePose_);  // glide from the native view
        startTransition(*cam, false);
    } else {
        startTransition(*cam, true);
    }
    preAcquirePose_.reset();
    emitActiveCamera();
}

void Coordinator::endPeek(std::uint64_t token, double now) {
    if (!peek_ || peek_->token != token) return;  // stale release (a newer selection superseded it)
    const Peek p = *peek_;
    peek_.reset();
    if (p.releaseOnEnd) release("Camera returned to the simulator", now);
    else if (!p.previousCameraId.empty()) activate(p.previousCameraId, now, true);
    emitActiveCamera();
}

std::string Coordinator::cycle(int delta, double now) {
    if (!profile_) return "No camera profile is open.";
    const std::string from = activeCameraId_.empty() ? profile_->defaultCameraId : activeCameraId_;
    const auto* cat = profile_->categoryOf(from);
    if (!cat || cat->cameraIds.empty()) return "No cameras to cycle through.";
    const auto& ids = cat->cameraIds;
    const auto n = static_cast<int>(ids.size());
    int i = static_cast<int>(std::find(ids.begin(), ids.end(), from) - ids.begin());
    for (int step = 0; step < n; ++step) {
        i = ((i + delta) % n + n) % n;
        const auto* cam = profile_->camera(ids[i]);
        if (cam && profiles::modeActivatable(cam->mode) && (cam->id != from || n == 1)) return activate(cam->id, now, false);
    }
    return "No other camera in this category can be shown.";
}

std::string Coordinator::nextCategory(double now) {
    if (!profile_ || profile_->categories.empty()) return "No camera profile is open.";
    const std::string from = activeCameraId_.empty() ? profile_->defaultCameraId : activeCameraId_;
    const auto& cats = profile_->categories;
    const auto n = static_cast<int>(cats.size());
    int i = 0;
    for (int k = 0; k < n; ++k)
        if (std::find(cats[k].cameraIds.begin(), cats[k].cameraIds.end(), from) != cats[k].cameraIds.end()) i = k;
    for (int step = 1; step <= n; ++step) {
        const auto& cat = cats[(i + step) % n];
        for (const auto& id : cat.cameraIds) {
            const auto* cam = profile_->camera(id);
            if (cam && profiles::modeActivatable(cam->mode)) return activate(id, now, false);
        }
    }
    return "No other category has a camera that can be shown.";
}

void Coordinator::release(const std::string& reason, double now) {
    peek_.reset();
    pendingCameraId_.clear();
    afterReleaseCameraId_.clear();
    seedName_.clear();
    activeCameraId_.clear();
    engine_.reset();
    lastSubmitted_.reset();
    const bool claimed = state_ == Ownership::Active || state_ == Ownership::SimulatorSuspended ||
                         (state_ == Ownership::Acquiring && acquireSent_);
    acquireToken_ = 0;
    if (state_ == Ownership::Releasing) {
        // Already returning control; a second request changes nothing.
    } else if (claimed && simConnected_) {
        backend_.setFlag(CameraFlag::Interaction, false);
        backend_.release("");
        releaseDeadline_ = now + kReleaseTimeout;
        setState(Ownership::Releasing, "Returning camera to the simulator…");
    } else if (state_ != Ownership::OtherClient && state_ != Ownership::UserDisabled) {
        setState(Ownership::Observing, reason);
    }
    emitActiveCamera();
}

void Coordinator::fault(const std::string& code, const std::string& message, double now) {
    emit_("FaultRaised", {{"code", code}, {"message", message}});
    release(message, now);
    if (state_ == Ownership::Releasing) {
        faultAfterRelease_ = true;
        stateReason_ = message;
    } else {
        setState(Ownership::Faulted, message);
    }
}

void Coordinator::invalidateSession(const std::string& reason, double now) {
    ++epoch_;
    if (state_ == Ownership::Active || state_ == Ownership::SimulatorSuspended || state_ == Ownership::Acquiring)
        release(reason, now);
    peek_.reset();
    pendingCameraId_.clear();
    engine_.reset();
    emit_("ConnectionChanged", {{"simConnected", simConnected_}, {"sessionEpoch", epoch_},
                                {"simVersion", simVersion_.empty() ? json(nullptr) : json(simVersion_)}, {"reason", reason}});
}

// Sim session lost (simulator quit or module shutting down): nothing to release; start over in Observing.
void Coordinator::resetSession(const std::string& reason, double now) {
    (void)now;
    simConnected_ = false;
    sdk_ = SdkAvailability::NotAcquired;
    gameControlled_ = false;
    read_ = ReadPurpose::None;
    cleanupPending_ = false;
    acquireToken_ = 0;
    peek_.reset();
    pendingCameraId_.clear();
    afterReleaseCameraId_.clear();
    activeCameraId_.clear();
    engine_.reset();
    lastSubmitted_.reset();
    faultAfterRelease_ = false;
    ++epoch_;
    emit_("ConnectionChanged", {{"simConnected", false}, {"sessionEpoch", epoch_},
                                {"simVersion", simVersion_.empty() ? json(nullptr) : json(simVersion_)}, {"reason", reason}});
    setState(Ownership::Observing, reason);
    emitActiveCamera();
}

// The WASM camera API has no per-request connection to reset (design 2.0 §6.3): when a release or a withdrawn
// claim cannot be confirmed, stop all writes, enter Faulted and point the user at the native camera controls.
void Coordinator::failSafe(const std::string& reason, double now) {
    (void)now;
    read_ = ReadPurpose::None;
    cleanupPending_ = false;
    acquireToken_ = 0;
    peek_.reset();
    pendingCameraId_.clear();
    afterReleaseCameraId_.clear();
    seedName_.clear();
    activeCameraId_.clear();
    engine_.reset();
    lastSubmitted_.reset();
    faultAfterRelease_ = false;
    emit_("FaultRaised", {{"code", "unconfirmed"}, {"message", reason}});
    setState(Ownership::Faulted, reason + " Use the simulator's camera controls, then Enable.");
    emitActiveCamera();
}

void Coordinator::onBackendEvent(const BackendEvent& ev, double now) {
    std::visit(
        [&](const auto& e) {
            using T = std::decay_t<decltype(e)>;
            if constexpr (std::is_same_v<T, backend_event::Connected>) {
                simConnected_ = true;
                simVersion_ = e.simVersion;
                backoff_ = 1.0;
                qual_ = qualify_(simVersion_);
                ++epoch_;
                backend_.requestStatus();
                emit_("ConnectionChanged", {{"simConnected", true}, {"sessionEpoch", epoch_}, {"simVersion", simVersion_}, {"reason", nullptr}});
                setState(Ownership::Observing, "Simulator in control");
                emitCapabilities(true);
            } else if constexpr (std::is_same_v<T, backend_event::Disconnected>) {
                const bool wasConnected = simConnected_;
                resetSession(e.reason.empty() ? "Microsoft Flight Simulator 2024 closed the connection." : e.reason, now);
                if (wasConnected || aircraft_.known()) {
                    aircraft_ = {};
                    profile_.reset();
                    emit_("AircraftChanged", {{"title", nullptr}, {"aircraftPath", nullptr}, {"modelKey", nullptr}, {"livery", nullptr}});
                }
                setState(Ownership::Observing, "Waiting for Microsoft Flight Simulator 2024");
                emitCapabilities();
            } else if constexpr (std::is_same_v<T, backend_event::CameraStatus>) {
                onStatus(e, now);
            } else if constexpr (std::is_same_v<T, backend_event::CameraData>) {
                onCameraData(e, now);
            } else if constexpr (std::is_same_v<T, backend_event::DefinitionList>) {
                emit_("DefinitionsListed", {{"names", e.names}});
            } else if constexpr (std::is_same_v<T, backend_event::AircraftLoaded>) {
                Aircraft a = aircraftFromPath(e.aircraftPath);
                if (a.aircraftPath == aircraft_.aircraftPath && a.modelKey == aircraft_.modelKey) return;
                const bool modelChanged = a.modelKey != aircraft_.modelKey;
                // The title can arrive before the aircraft-loaded notification; keep it rather than drop it.
                a.title = !pendingTitle_.empty() ? pendingTitle_ : modelChanged ? std::string() : aircraft_.title;
                pendingTitle_.clear();
                aircraft_ = a;
                if (modelChanged) profile_.reset();
                invalidateSession("The aircraft changed.", now);
                emit_("AircraftChanged", {{"title", aircraft_.title.empty() ? json(nullptr) : json(aircraft_.title)},
                                          {"aircraftPath", aircraft_.aircraftPath}, {"modelKey", aircraft_.modelKey},
                                          {"livery", aircraft_.title.empty() ? json(nullptr) : json(aircraft_.title)}});
                emitCapabilities();
            } else if constexpr (std::is_same_v<T, backend_event::AircraftTitle>) {
                if (!aircraft_.known()) {
                    pendingTitle_ = e.title;
                    return;
                }
                if (e.title == aircraft_.title) return;
                aircraft_.title = e.title;  // a livery change keeps the model, so the profile stays compatible
                emit_("AircraftChanged", {{"title", aircraft_.title}, {"aircraftPath", aircraft_.aircraftPath},
                                          {"modelKey", aircraft_.modelKey}, {"livery", aircraft_.title}});
            } else if constexpr (std::is_same_v<T, backend_event::FlightLoaded>) {
                invalidateSession("A flight was loaded.", now);
            } else if constexpr (std::is_same_v<T, backend_event::SimRunning>) {
                if (!e.running && (state_ == Ownership::Active || state_ == Ownership::Acquiring || state_ == Ownership::SimulatorSuspended))
                    release("The simulator left the flight.", now);
            } else if constexpr (std::is_same_v<T, backend_event::Paused>) {
                // Ordinary pause keeps camera control; takeovers arrive as bGameControlled.
            } else if constexpr (std::is_same_v<T, backend_event::CameraException>) {
                if (state_ == Ownership::Acquiring) {
                    acquireToken_ = 0;
                    pendingCameraId_.clear();
                    seedName_.clear();
                    setState(Ownership::Observing, "The simulator refused camera control: " + e.message);
                } else {
                    emit_("FaultRaised", {{"code", "cameraApi"}, {"message", "The simulator reported a camera error: " + e.message}});
                }
            }
        },
        ev);
}

void Coordinator::onStatus(const backend_event::CameraStatus& st, double now) {
    sdk_ = st.state;
    const bool wasGame = gameControlled_;
    gameControlled_ = st.gameControlled;
    if (cleanupPending_ && st.state != SdkAvailability::Acquired) cleanupPending_ = false;

    auto lostCamera = [&](Ownership next, const std::string& why) {
        engine_.reset();
        activeCameraId_.clear();
        peek_.reset();
        lastSubmitted_.reset();
        setState(next, why);
        emitActiveCamera();
    };

    switch (state_) {
        case Ownership::Acquiring:
            if (st.state == SdkAvailability::Acquired) {
                if (acquireToken_ == 0) {
                    backend_.release("");  // late grant of an abandoned request: release without writing
                    break;
                }
                onAcquired(now);
                if (gameControlled_) {
                    engine_.freeze();
                    setState(Ownership::SimulatorSuspended, "Paused by the simulator");
                }
            } else if (st.state == SdkAvailability::AcquiredByOther) {
                acquireToken_ = 0;
                pendingCameraId_.clear();
                seedName_.clear();
                backend_.release("");  // yield our queued claim, never fight for the camera
                cleanupPending_ = true;
                cleanupDeadline_ = now + kReleaseTimeout;
                setState(Ownership::OtherClient, "Another add-on has the camera");
            } else if (st.state == SdkAvailability::UserDisabled) {
                acquireToken_ = 0;
                pendingCameraId_.clear();
                setState(Ownership::UserDisabled, "Camera control by add-ons is disabled in the simulator");
            }
            break;
        case Ownership::Active:
        case Ownership::SimulatorSuspended:
            if (st.state == SdkAvailability::Acquired) {
                if (gameControlled_ && state_ == Ownership::Active) {
                    engine_.freeze();
                    setState(Ownership::SimulatorSuspended, "Paused by the simulator");
                } else if (!gameControlled_ && state_ == Ownership::SimulatorSuspended && wasGame) {
                    // The simulator may have changed the visible view: reseed from a readback, or yield.
                    const CameraEntry* cam = activeEntry();
                    if (!readsUnreliable_ && cam) requestRead(ReadPurpose::Resume, referentialFor(*cam), now, kReadTimeout);
                    if (read_ != ReadPurpose::Resume)
                        release("The simulator took over the view. Select a view to continue.", now);
                }
            } else if (st.state == SdkAvailability::AcquiredByOther) {
                lostCamera(Ownership::OtherClient, "Another add-on took the camera");
            } else if (st.state == SdkAvailability::UserDisabled) {
                lostCamera(Ownership::UserDisabled, "Camera control by add-ons is disabled in the simulator");
            } else {
                lostCamera(Ownership::Observing, "The simulator took the camera back. Select a view to continue.");
            }
            break;
        case Ownership::Releasing:
            if (st.state != SdkAvailability::Acquired) {
                const std::string next = afterReleaseCameraId_;
                afterReleaseCameraId_.clear();
                if (faultAfterRelease_) {
                    faultAfterRelease_ = false;
                    setState(Ownership::Faulted, stateReason_);
                } else {
                    setState(Ownership::Observing, "Camera returned to the simulator");
                }
                if (!next.empty()) activate(next, now, false);
            }
            break;
        case Ownership::OtherClient:
            if (st.state == SdkAvailability::NotAcquired) setState(Ownership::Observing, "Simulator in control");
            else if (st.state == SdkAvailability::Acquired) backend_.release("");
            break;
        case Ownership::UserDisabled:
            if (st.state == SdkAvailability::NotAcquired) setState(Ownership::Observing, "Simulator in control");
            break;
        case Ownership::Observing:
        case Ownership::Faulted:
            if (st.state == SdkAvailability::Acquired) backend_.release("");  // stale claim with no valid token
            else if (st.state == SdkAvailability::UserDisabled)
                setState(Ownership::UserDisabled, "Camera control by add-ons is disabled in the simulator");
            break;
    }
    emitCapabilities();
}

void Coordinator::onCameraData(const backend_event::CameraData& d, double) {
    const ReadPurpose purpose = read_;
    if (purpose == ReadPurpose::None) return;  // unsolicited; reads are serialised so nothing to match
    read_ = ReadPurpose::None;
    const core::CameraPose pose = fromSdk(d.pose, qual_.contract);
    const bool finite = pose.finite() && pose.fovRad > 0;
    switch (purpose) {
        case ReadPurpose::PreAcquire:
            if (state_ == Ownership::Acquiring && !acquireSent_) {
                if (finite) preAcquirePose_ = pose;
                acquireSent_ = backend_.acquire("FS Camera Studio");
            }
            break;
        case ReadPurpose::Capture:
        case ReadPurpose::Seed: {
            json payload = poseJson(pose);
            const bool adapterOk = qual_.adapterQualified;
            const bool captureOk = purpose == ReadPurpose::Seed || qual_.captureQualified;
            payload.update({{"ok", finite},
                            {"frame", referentialName(d.pose.referential)},
                            {"qualified", adapterOk && captureOk},
                            {"source", purpose == ReadPurpose::Seed ? "definition:" + seedName_ : std::string("nativeView")},
                            {"message", !finite ? "The simulator returned an unusable view."
                                        : adapterOk && captureOk ? "Captured from the simulator."
                                                                 : "Captured, but this simulator build is not qualified yet — check the "
                                                                   "preview before saving. Head-tracking offsets may be included."}});
            if (purpose == ReadPurpose::Seed) seedName_.clear();
            emit_("ViewCaptured", payload);
            break;
        }
        case ReadPurpose::Resume:
            if (state_ == Ownership::SimulatorSuspended && sdk_ == SdkAvailability::Acquired && !gameControlled_) {
                const CameraEntry* cam = activeEntry();
                engine_.reset();
                lastSubmitted_.reset();
                if (finite) engine_.place(pose);
                if (cam) startTransition(*cam, !finite);
                setState(Ownership::Active, "Camera active");
                emitActiveCamera();
            }
            break;
        case ReadPurpose::None: break;
    }
}

bool Coordinator::eligibleToSubmit() const {
    return state_ == Ownership::Active && simConnected_ && sdk_ == SdkAvailability::Acquired && !gameControlled_ &&
           !activeCameraId_.empty() && seedName_.empty();
}

void Coordinator::submit() {
    const CameraEntry* cam = activeEntry();
    if (!cam || !eligibleToSubmit()) return;
    const auto s = engine_.sample();
    const SdkPose sdk = toSdk(s.pose, qual_.contract, referentialFor(*cam));
    if (lastSubmitted_ && nearlyEqual(*lastSubmitted_, sdk)) return;
    if (backend_.submitPose(sdk)) {
        lastSubmitted_ = sdk;
        ++submissions_;
    }
}

void Coordinator::onAction(const ActionEvent& ev, double now) {
    std::string result;
    switch (ev.action) {
        case Action::SelectView:
            if (!ev.pressed) return;
            if (const auto* cam = profile_ ? profile_->cameraByViewId(ev.viewId) : nullptr) result = activate(cam->id, now, false);
            else result = "No camera uses view ID " + std::to_string(ev.viewId) + ".";
            break;
        case Action::PeekView: {
            constexpr std::uint64_t kHostTokenBit = 1ull << 62;  // keeps host tokens apart from UI tokens
            if (!ev.pressed) {
                endPeek(ev.holdId | kHostTokenBit, now);
                return;
            }
            const auto* cam = profile_ ? profile_->cameraByViewId(ev.viewId) : nullptr;
            if (!cam) {
                result = "No camera uses view ID " + std::to_string(ev.viewId) + ".";
                break;
            }
            if (peek_) return;
            Peek pk{ev.holdId | kHostTokenBit, state_ == Ownership::Active ? activeCameraId_ : std::string(), state_ != Ownership::Active};
            result = activate(cam->id, now, true);
            if (result.empty()) {
                peek_ = pk;
                emitActiveCamera();
            }
            break;
        }
        case Action::NextCamera: if (ev.pressed) result = cycle(1, now); else return; break;
        case Action::PreviousCamera: if (ev.pressed) result = cycle(-1, now); else return; break;
        case Action::NextCategory: if (ev.pressed) result = nextCategory(now); else return; break;
        case Action::ReturnToPilot:
            if (!ev.pressed) return;
            result = profile_ ? activate(profile_->defaultCameraId, now, false) : "No camera profile is open.";
            break;
        case Action::ReleaseCamera:
            if (!ev.pressed) return;
            release("Camera returned to the simulator", now);
            break;
        case Action::RecenterLook:
            if (!ev.pressed) return;
            engine_.recenterLook();
            emitLook(now, true);
            break;
        case Action::ToggleEditor:
            if (!ev.pressed) return;
            emit_("SurfaceRequest", {{"target", "main"}, {"op", "toggle"}});
            break;
        case Action::ToggleMini:
            if (!ev.pressed) return;
            emit_("SurfaceRequest", {{"target", "mini"}, {"op", "toggle"}});
            break;
        default: return;  // continuous actions arrive through setManualInput
    }
    emit_("BindingTriggered", {{"action", profiles::toString(ev.action)}, {"viewId", ev.viewId},
                               {"result", result.empty() ? json(nullptr) : json(result)}});
}

void Coordinator::tick(double now) {
    const double dt = lastTick_ < 0 ? 0.0 : core::clamp(now - lastTick_, 0.0, 0.25);
    lastTick_ = now;

    if (!backend_.isOpen()) {
        if (now >= nextConnectAt_) {
            if (!backend_.tryConnect()) backoff_ = std::min(backoff_ * 2.0, kMaxReconnectBackoff);
            nextConnectAt_ = now + backoff_;
        }
        return;
    }

    if (state_ == Ownership::Acquiring && now > acquireDeadline_) {
        acquireToken_ = 0;
        read_ = read_ == ReadPurpose::PreAcquire ? ReadPurpose::None : read_;
        if (acquireSent_) {
            backend_.release("");  // withdraw the queued claim
            cleanupPending_ = true;
            cleanupDeadline_ = now + kReleaseTimeout;
        }
        pendingCameraId_.clear();
        seedName_.clear();
        setState(Ownership::Observing, "The simulator did not hand over the camera in time. Try again.");
    }
    if (cleanupPending_ && now > cleanupDeadline_) {
        failSafe("Camera Studio could not confirm that a cancelled camera request was withdrawn.", now);
        return;
    }
    if (state_ == Ownership::Releasing && now > releaseDeadline_) {
        failSafe("The simulator did not confirm that the camera was returned.", now);
        return;
    }
    if (read_ != ReadPurpose::None && now > readDeadline_) {
        const ReadPurpose purpose = read_;
        read_ = ReadPurpose::None;
        readsUnreliable_ = true;  // a late reply could be mistaken for a later read: quarantine the lane
        if (purpose == ReadPurpose::Capture || purpose == ReadPurpose::Seed)
            emit_("ViewCaptured", {{"ok", false}, {"message", "The simulator did not report the current view in time."}});
        if (purpose == ReadPurpose::PreAcquire && state_ == Ownership::Acquiring && !acquireSent_) {
            // Nothing was claimed yet; still start a fresh connection so a late reply cannot be misread.
            pendingCameraId_.clear();
            acquireToken_ = 0;
        }
        if (state_ == Ownership::SimulatorSuspended) release("The simulator took over the view. Select a view to continue.", now);
        return;
    }

    if (state_ == Ownership::Active && eligibleToSubmit()) {
        engine_.tick(dt, manual_);
        const auto s = engine_.sample();
        if (!s.valid) {
            fault("invalidPose", "Camera stopped: the computed view was outside the allowed range.", now);
            return;
        }
        submit();
        if (engine_.transitioning() != lastTransitioning_) emitActiveCamera();
        emitLook(now);
    }
}

}  // namespace fscs::host
