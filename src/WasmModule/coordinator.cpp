#include "coordinator.hpp"
#include "fscs/profiles/airport.hpp"

#include "fscs/profiles/json_util.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <limits>

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
    // Latitude/longitude are degrees: 1e-6 would swallow roughly 11 cm of manual movement.
    const double horizontal = a.referential == Referential::World ? 1e-11 : e;
    return a.trackUserAircraft == b.trackUserAircraft && a.referential == b.referential && std::abs(a.x - b.x) < horizontal && std::abs(a.y - b.y) < horizontal && std::abs(a.z - b.z) < e &&
           std::abs(a.pitchDeg - b.pitchDeg) < e && std::abs(a.bankDeg - b.bankDeg) < e &&
           std::abs(a.headingDeg - b.headingDeg) < e && std::abs(a.fovRad - b.fovRad) < e;
}

// Flight effects strength by view type; 0 = not eligible. World-fixed views
// (airport, fly-by) and free cameras never move with the aircraft. Mounted cameras
// are rigid on the airframe and use their own airframe vibration instead.
double effectsScaleFor(profiles::CameraMode mode) {
    switch (mode) {
        case profiles::CameraMode::AircraftCockpit: return 1.;
        case profiles::CameraMode::AircraftExterior:
        case profiles::CameraMode::Orbit: return core::kExteriorEffectsScale;
        case profiles::CameraMode::Flyby:
        case profiles::CameraMode::Airport:
        case profiles::CameraMode::Free:
        case profiles::CameraMode::Mounted: return 0.;
    }
    return 0.;
}
// The biomechanical head model belongs to the pilot's eye: cockpit views only.
bool headModelFor(profiles::CameraMode mode) { return mode == profiles::CameraMode::AircraftCockpit; }
// Free cameras fly with their own integrator (Coordinator::updateFreeCamera); the engine keeps look and zoom.
core::ManualInput lookOnly(core::ManualInput in) { in.moveForward = in.moveRight = in.moveUp = 0; return in; }
// A generated airport camera's height is measured from this ground altitude (ICameraBackend::setGroundBase).
double groundBaseFor(const profiles::CameraEntry* cam) {
    if (!cam || cam->mode != profiles::CameraMode::Airport || cam->airportHeightCategory.empty()) return NAN;
    return cam->airportHeightBaseAltitudeMeters;
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

// The simulator's name for messages: the same module runs in MSFS 2024 and, as XP Camera Studio, in X-Plane 12
// (whose plugin reports "X-Plane <version>").
static std::string simNameOf(const std::string& simVersion) {
    return simVersion.rfind("X-Plane", 0) == 0 ? "X-Plane" : "Microsoft Flight Simulator 2024";
}

bool Coordinator::epochCommand(const std::string& a) const {
    return a == "ActivateCamera" || a == "BeginPeek" || a == "CycleCamera" || a == "NextCategory" ||
           a == "ReturnToPilot" || a == "CaptureView" || a == "SeedFromDefinition" || a == "BakeTemporaryLook" || a == "MouseLookDelta" || a == "StartWalkaround";
}

std::string Coordinator::blockReason() const {
    if (!simConnected_) return simNameOf(simVersion_) + " is not running or not connected.";
    if (!aircraft_.known()) return "Load a flight to use your cameras.";
    if (!profile_) return "No camera profile is open for this aircraft.";
    if (profile_->modelKey != aircraft_.modelKey) return "The open profile belongs to another aircraft.";
    if (profile_->readOnly) return "This profile needs a newer version of Camera Studio.";
    return spottingBlockReason();
}

std::string Coordinator::spottingBlockReason() const {
    if (!simConnected_) return simNameOf(simVersion_) + " is not running or not connected.";
    if (!aircraft_.known()) return "Load a flight to use your cameras.";
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
    if(cam.mode==profiles::CameraMode::Airport || cam.mode==profiles::CameraMode::Flyby || cam.mode==profiles::CameraMode::Free)return Referential::World;
    return cam.mode == profiles::CameraMode::AircraftExterior || cam.mode == profiles::CameraMode::Orbit || cam.mode == profiles::CameraMode::Mounted ? Referential::SimObject : Referential::Eyepoint;
}

void Coordinator::setState(Ownership s, const std::string& reason) {
    if (s == state_ && reason == stateReason_) return;
    if (s != Ownership::Active) headTracking_.restore();
    state_ = s;
    if (s != Ownership::Active && s != Ownership::Acquiring && s != Ownership::SimulatorSuspended) stopWalkaround();
    if (s != Ownership::Active) cancelMouseDrag();
    if (s != Ownership::Active) cancelBinoculars();
    stateReason_ = reason;
    emit_("OwnershipChanged", {{"state", toString(s)}, {"gameControlled", gameControlled_}, {"reason", reason}});
    emitCapabilities();
}

json Coordinator::capabilitiesJson() const {
    const std::string block = blockReason(), spotBlock = spottingBlockReason();
    json modes = json::object();
    for (auto m : {profiles::CameraMode::AircraftCockpit, profiles::CameraMode::AircraftExterior, profiles::CameraMode::Orbit,
                   profiles::CameraMode::Flyby, profiles::CameraMode::Airport, profiles::CameraMode::Free, profiles::CameraMode::Mounted}) {
        const bool ok = profiles::modeActivatable(m);
        modes[profiles::toString(m)] = {{"available", ok},
                                        {"preview", m == profiles::CameraMode::AircraftExterior || m == profiles::CameraMode::Airport || m == profiles::CameraMode::Mounted || m == profiles::CameraMode::Free},
                                        {"reason", ok ? (m == profiles::CameraMode::AircraftExterior ? json("Preview: exterior views keep cockpit sound in the current simulator build.")
                                                         : m == profiles::CameraMode::Mounted ? json("Preview: mounted views keep cockpit sound and need live checking of the gimbal and vibration.")
                                                         : m == profiles::CameraMode::Free ? json("Preview: free cameras need live checking of world placement and terrain clearance.")
                                                                                              : json(nullptr))
                                                      : json("Not available in this version. Saved with the profile but cannot be activated.")}};
    }
    return {{"headTracking", headTracking_.status()}, {"nativeMouseLook", mouseMapped_}, {"adapterQualified", qual_.adapterQualified},
              {"adapterContract", qual_.contract.id},
              {"captureQualified", qual_.captureQualified && !readsUnreliable_},
              {"allowUnqualifiedAdapter", settings_.allowUnqualifiedAdapter},
              {"canActivate", block.empty()},
              {"blockReason", block.empty() ? json(nullptr) : json(block)},
              {"spottingAvailable", spotBlock.empty()},
              {"spottingBlockReason", spotBlock.empty() ? json(nullptr) : json(spotBlock)},
              {"simVersion", simVersion_.empty() ? json(nullptr) : json(simVersion_)},
              {"fovRangeDeg", {30, 110}},
              {"levelHorizon", true},
              {"followHorizon", true},
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
                                  {"cameraId", activeCameraId_.empty() || (spot_ && spot_->overlay) ? json(nullptr) : json(activeCameraId_)},
                                  {"peeking", peek_.has_value()},
                                  {"transitioning", engine_.transitioning()},
                                  {"glance", glanceJson()}});
    lastTransitioning_ = engine_.transitioning();
}

json Coordinator::walkaroundJson() const {
    return {{"running",walkaround_.has_value()},
        {"categoryId",walkaround_ ? json(walkaround_->categoryId) : json(nullptr)},
        {"intervalSeconds",walkaround_ ? walkaround_->interval : 5.0},
        {"loop",walkaround_ ? walkaround_->loop : true}};
}

void Coordinator::emitWalkaround() {
    emit_("WalkaroundChanged",walkaroundJson());
}

void Coordinator::stopWalkaround() {
    if (walkaround_) { walkaround_.reset(); emitWalkaround(); }
}

core::CameraPose Coordinator::motionPose(const CameraEntry& cam) const {
    auto pose = cam.pose;
    const double radius = core::clamp(cam.motionRadiusMeters,2,5000);
    const double height = core::clamp(cam.motionHeightMeters,1,2000);
    if (cam.mode == profiles::CameraMode::Orbit) {
        const double angle = motionTime_ * 2 * std::numbers::pi / core::clamp(cam.motionDurationSeconds,5,600) * (cam.motionClockwise ? 1 : -1);
        pose.positionFRD = {radius * std::cos(angle),radius * std::sin(angle),-height};
    } else if (cam.mode == profiles::CameraMode::Flyby) {
        // A finite cinematic pass also moves when the aircraft is parked. Its WORLD
        // origin and travel heading remain fixed from recall; tracking follows the
        // actual aircraft independently. Cosine easing stops smoothly at each end.
        const double duration = core::clamp(cam.motionDurationSeconds,5,600);
        const double phase = cam.motionRepeat ? std::fmod(motionTime_ / duration,2.0) : std::min(motionTime_ / duration,1.0);
        const double along = radius * std::cos(phase * std::numbers::pi);
        pose.positionFRD = core::Quat::fromPbh({0,0,flybyHeading_}).rotate({along,core::clamp(cam.motionSideMeters,-5000,5000),-height});
    }
    const auto d = pose.positionFRD * -1;
    if (settings_.globalAircraftTracking != "off") {
        auto angles = pose.orientation.toPbh();
        angles.headingDeg = std::atan2(d.y,d.x)*core::kRadToDeg;
        if (settings_.globalAircraftTracking == "both") angles.pitchDeg = std::atan2(-d.z,std::hypot(d.x,d.y))*core::kRadToDeg;
        pose.orientation = core::Quat::fromPbh(angles);
    }
    return pose;
}

void Coordinator::emitLook(double now, bool force) {
    const auto& l = engine_.look();
    const bool active = l.active() || freeOffset_.length() > 0;  // a flown drone is a temporary adjustment too
    if (!force && !active && !lastLookActive_) return;
    if (!force && now - lastLookEmit_ < 0.1) return;
    lastLookEmit_ = now;
    lastLookActive_ = active;
    emit_("TemporaryLookChanged",
          {{"active", active},
           {"positionMetersFRD", {l.translationHeadingFrame.x + freeOffset_.x, l.translationHeadingFrame.y + freeOffset_.y,
                                  l.translationHeadingFrame.z + freeOffset_.z}},
           {"pbhDeg", {l.pitchDeg, 0.0, l.headingDeg}},
           {"fovDeltaRad", l.fovDeltaRad}});
}

void Coordinator::emitFullState() {
    emitWalkaround();
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
    emitBinoculars();
}

void Coordinator::setProfile(std::optional<profiles::ProfileSnapshot> snapshot, double now) {
    const bool same = profile_ && snapshot && profile_->profileId == snapshot->profileId;
    if (!same) { stopWalkaround(); rememberedLooks_.clear(); lookOwner_.clear(); }
    const auto previousCamera = activeEntry() ? std::optional<CameraEntry>(*activeEntry()) : std::nullopt;
    profile_ = std::move(snapshot);
    emitCapabilities();
    // Chase, fly-by and the computed views need no profile: a spot keeps going without its view underneath.
    const bool keepSpot = spot_ && spot_->overlay && spottingBlockReason().empty();
    if (!profile_) {
        if (keepSpot) {
            detachSpotBase();
        } else if (state_ == Ownership::Active || state_ == Ownership::SimulatorSuspended || state_ == Ownership::Acquiring) {
            release("No camera profile is open.", now);
        }
        return;
    }
    if (keepSpot && state_ == Ownership::Acquiring && !pendingCameraId_.empty() && (!canActivate() || !profile_->camera(pendingCameraId_)))
        pendingCameraId_.clear();  // acquire with nothing underneath
    if (state_ == Ownership::Active && !activeCameraId_.empty()) {
        const auto block = blockReason();
        const CameraEntry* cam = profile_->camera(activeCameraId_);
        if (keepSpot && (!block.empty() || !same || !cam || !profiles::modeActivatable(cam->mode))) {
            detachSpotBase();
        } else if (!block.empty()) {
            release(block, now);
        } else if (!same || !cam || !profiles::modeActivatable(cam->mode)) {
            // The active view vanished (revert, delete, profile switch): release before replacing it and require
            // a deliberate recall (design 2.0 §4.7).
            release(same ? "The active view is no longer in the profile. Select a view to continue."
                         : "A different profile was opened. Select a view to continue.", now);
        } else {
            backend_.setFlag(CameraFlag::Interaction, cam->interaction);
            const auto base = engine_.basePose();
            const auto effectivePose = effectiveAirportPose(*cam);
            const double delta = base ? (base->positionFRD - effectivePose.positionFRD).length() +
                                            core::angleBetween(base->orientation, cam->pose.orientation) +
                                            std::abs(base->fovRad - cam->pose.fovRad)
                                      : 1.0;
            if (cam->mode == profiles::CameraMode::Orbit || cam->mode == profiles::CameraMode::Flyby) {
                if (!previousCamera || previousCamera->mode != cam->mode || previousCamera->motionRadiusMeters != cam->motionRadiusMeters ||
                    previousCamera->motionHeightMeters != cam->motionHeightMeters || previousCamera->motionDurationSeconds != cam->motionDurationSeconds ||
                    previousCamera->motionClockwise != cam->motionClockwise || previousCamera->motionSideMeters != cam->motionSideMeters ||
                    previousCamera->motionRepeat != cam->motionRepeat) startTransition(*cam,true);
                else if (previousCamera->pose.fovRad != cam->pose.fovRad) {
                    auto updated = engine_.basePose().value_or(motionPose(*cam));
                    updated.fovRad = cam->pose.fovRad;
                    engine_.animateBase(updated);
                }
            } else if (delta > 1e-9) engine_.place(effectivePose);  // live preview of the edited draft
        }
    }
    emitActiveCamera();
}

void Coordinator::setQualificationSession(bool on, double now) {
    qualificationSession_ = on;
    emitCapabilities();
    if (!on) enforceBlock(now);
}

void Coordinator::enforceBlock(double now) {
    if (state_ != Ownership::Active && state_ != Ownership::SimulatorSuspended) return;
    const auto block = blockReason();
    if (block.empty()) return;
    const auto spotBlock = spottingBlockReason();
    if (spot_ && spot_->overlay && spotBlock.empty()) {
        detachSpotBase();  // spotting continues without a profile view underneath
        return;
    }
    release(spotBlock.empty() ? block : spotBlock, now);
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
    const auto* camera = activeEntry();
    const auto oldPose = camera ? effectiveAirportPose(*camera) : core::CameraPose{};
    settings_ = s;
    if (camera && camera->mode == profiles::CameraMode::Airport && engine_.hasPose()) {
        if (auto base=engine_.basePose()) {
            const double delta=effectiveAirportPose(*camera).positionFRD.z-oldPose.positionFRD.z;
            if (std::abs(delta)>1e-9) { base->positionFRD.z += delta; engine_.place(*base); }
        }
    }
    if (state_ == Ownership::Active && eligibleToSubmit() && !spot_)
        headTracking_.update("global",settings_.globalHeadTrackingEnabled ? "on" : "off",now);
    engine_.setSpeeds({settings_.lookSpeedDegPerSec, settings_.moveSpeedMetersPerSec * settings_.movementPrecisionScale, 0.5, 6.0});
    emitCapabilities();
    enforceBlock(now);
}

json Coordinator::handle(const protocol::Command& c, double now) {
    if (epochCommand(c.action) && c.sessionEpoch != epoch_) return ack(c, AckStatus::Rejected, "staleEpoch");
    const auto& p = c.payload;

    if (c.action == "StopWalkaround") {
        stopWalkaround();
        return ack(c,AckStatus::Completed);
    }
    if (c.action == "StartWalkaround") {
        if (!profile_) return ack(c,AckStatus::Rejected,"Open a camera profile first.");
        const auto categoryId = js::str(p,"categoryId");
        const double interval = js::num(p,"intervalSeconds",5);
        if (!std::isfinite(interval) || interval < 1 || interval > 120) return ack(c,AckStatus::Rejected,"Walkaround dwell must be 1-120 seconds.");
        const CameraEntry* first = nullptr;
        for (const auto& category : profile_->categories) if (category.id == categoryId) {
            for (const auto& id : category.cameraIds) {
                const auto* entry = profile_->camera(id);
                if (entry && entry->mode == profiles::CameraMode::AircraftExterior) { first = entry; break; }
            }
        }
        if (!first) return ack(c,AckStatus::Rejected,"Choose a category containing exterior cameras for a walkaround.");
        const auto error = activate(first->id,now,false);
        if (!error.empty()) return ack(c,AckStatus::Rejected,error);
        walkaround_ = Walkaround{categoryId,interval,0,js::boolean(p,"loop",true)};
        emitWalkaround();
        return ack(c,AckStatus::Accepted);
    }
    if (c.action == "MouseLookDelta") {
        if (!manualCameraReady()) return ack(c, AckStatus::Rejected, "Select a camera before using mouse look.");
        const double heading = js::num(p, "headingDeg", 0), pitch = js::num(p, "pitchDeg", 0);
        if (!std::isfinite(heading) || !std::isfinite(pitch) || std::abs(heading) > 45 || std::abs(pitch) > 45)
            return ack(c, AckStatus::Rejected, "Invalid mouse movement.");
        const double right = js::num(p, "rightMeters", 0), up = js::num(p, "upMeters", 0), forward = js::num(p, "forwardMeters", 0);
        if (!std::isfinite(right) || !std::isfinite(up) || !std::isfinite(forward) || std::abs(right) > .5 || std::abs(up) > .5 || std::abs(forward) > .5)
            return ack(c, AckStatus::Rejected, "Invalid mouse translation.");
        if (spot_) adjustSpot(heading * binocularsLookScale_, pitch * binocularsLookScale_, {forward, right, -up});
        else {
            engine_.offsetLook(heading * binocularsLookScale_, pitch * binocularsLookScale_);
            engine_.offsetPosition({forward, right, -up});
        }
        emitLook(now);
        return ack(c, AckStatus::Completed);
    }

    if (c.action == "DroneHere") {
        std::string error;
        json result = droneHere(now, error);
        return error.empty() ? protocol::makeAck(c, AckStatus::Completed, {}, std::move(result)) : ack(c, AckStatus::Rejected, error);
    }
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
        if (spot_) { recenterSpot(); return ack(c, AckStatus::Completed); }
        engine_.recenterLook();
        resetFreeMotion();  // a drone returns to its saved place
        emitLook(now, true);
        return ack(c, AckStatus::Completed);
    }
    if (c.action == "BakeTemporaryLook") {
        const CameraEntry* cam = activeEntry();
        if (state_ != Ownership::Active || !cam || !engine_.hasPose())
            return ack(c, AckStatus::Rejected, "Select a camera in the simulator first.");
        const auto s = engine_.sample();
        if (!s.valid) return ack(c, AckStatus::Rejected, "The current view is outside the allowed range.");
        if (cam->mode == profiles::CameraMode::Free) return bakeFreeCamera(c, *cam, s.pose, now);
        json payload = poseJson(s.pose);
        payload["cameraId"] = cam->id;
        engine_.recenterLook();
        engine_.place(s.pose);
        emit_("TemporaryLookBaked", payload);
        emitLook(now, true);
        return ack(c, AckStatus::Completed);
    }
    if (c.action == "CaptureView") {
        const auto targetId = js::str(p, "cameraId");
        const CameraEntry* target = profile_ && !targetId.empty() ? profile_->camera(targetId) : activeEntry();
        if (!targetId.empty() && !target) return ack(c, AckStatus::Rejected, "The selected camera no longer exists.");
        const auto requestedFrame = target ? referentialFor(*target) : Referential::Eyepoint;
        if (state_ == Ownership::Active && engine_.hasPose() && activeEntry() && referentialFor(*activeEntry()) == requestedFrame) {
            const auto s = engine_.sample();
            const CameraEntry* cam = activeEntry();
            auto shown = s.pose;
            if (cam && cam->mode == profiles::CameraMode::Free) shown.positionFRD += freeOffset_;  // about the drone's anchor
            json payload = poseJson(shown);
            payload.update({{"ok", s.valid}, {"frame", referentialName(cam ? referentialFor(*cam) : Referential::Eyepoint)},
                            {"qualified", true}, {"source", "addonCamera"},
                            {"message", "Captured the view Camera Studio is showing."}});
            emit_("ViewCaptured", payload);
            return ack(c, AckStatus::Completed);
        }
        if (!simConnected_) return ack(c, AckStatus::Rejected, simNameOf(simVersion_) + " is not connected.");
        if (readsUnreliable_) {
            emit_("ViewCaptured", {{"ok", false}, {"message", "Reading the simulator's view is unavailable on this connection. "
                                                              "Start from a native camera definition instead."}});
            return ack(c, AckStatus::Completed);
        }
        if (read_ != ReadPurpose::None) return ack(c, AckStatus::Rejected, "Another camera read is in progress.");
        readCameraId_=target ? target->id : std::string();
        requestRead(ReadPurpose::Capture, requestedFrame, now, kReadTimeout);
        return ack(c, AckStatus::Accepted);
    }
    if (c.action == "EnumerateDefinitions") {
        if (!simConnected_ || !backend_.enumerateDefinitions())
            return ack(c, AckStatus::Rejected, simNameOf(simVersion_) + " is not connected.");
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
            engine_.reset(); effects_.reset(); motionReceivedAt_ = -1;
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
    if (cam->mode == profiles::CameraMode::Flyby && (targetAt_ < 0 || now-targetAt_ > 5 || !std::isfinite(targetHeading_)))
        return "Waiting for the aircraft position before starting a fly-by.";
    if (const auto freeError = freeBlock(*cam, now); !freeError.empty()) return freeError;
    if (!spotActivating_) endSpot("A camera was selected manually.", true, false);  // any other recall ends spotting
    ++recalls_;
    glance_.reset(); glanceReturn_.reset();  // recalling any view ends a glance without returning
    if (!walkaroundAdvancing_) stopWalkaround();
    seedName_.clear();
    switch (state_) {
        case Ownership::Active: {
            const auto* old=activeEntry();
            const bool cut=!old || referentialFor(*old)!=referentialFor(*cam) || old->originLat!=cam->originLat || old->originLon!=cam->originLon || old->originAltitude!=cam->originAltitude || old->followAircraft!=cam->followAircraft;
            activeCameraId_ = cameraId;
            startTransition(*cam, cut);
            emitActiveCamera();
            return {};
        }
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
    if (cam && cam->mode != profiles::CameraMode::Airport && cam->mode != profiles::CameraMode::Flyby && cam->mode != profiles::CameraMode::Free && qual_.captureQualified && !readsUnreliable_ && read_ == ReadPurpose::None) {
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
    lastSubmitted_.reset();
    airportTrackingPosition_.reset();
    airportAutoZoomFov_.reset();
    cancelMouseDrag();
    motionTime_ = 0;
    if (cam.mode == profiles::CameraMode::Flyby) {
        flybyLat_ = targetLat_; flybyLon_ = targetLon_; flybyAltitude_ = targetAltitude_; flybyHeading_ = targetHeading_;
    }
    const auto target = cam.mode == profiles::CameraMode::Orbit || cam.mode == profiles::CameraMode::Flyby ? motionPose(cam) : effectiveAirportPose(cam);
    if (engine_.hasPose() && freeOffset_.length() > 0) {
        // Leaving a flown drone: start any transition from where it is on screen (same anchor; otherwise a cut).
        auto shown = engine_.sample().pose;
        shown.positionFRD += freeOffset_;
        if (shown.withinLimits()) engine_.place(shown);
    }
    resetFreeMotion();
    freeCameraId_.clear();  // a recall: the saved pose and follow setting decide
    if (!lookOwner_.empty()) rememberedLooks_[lookOwner_] = engine_.look();
    const auto remembered = rememberedLooks_.find(cam.id);
    const auto look = cam.retainLook && remembered != rememberedLooks_.end() ? remembered->second : core::TemporaryLook{};
    lookOwner_ = cam.id;
    backend_.setFlag(CameraFlag::Interaction, cam.interaction);
    backend_.setFlag(CameraFlag::AboveGround, cam.mode==profiles::CameraMode::Airport || cam.mode==profiles::CameraMode::Free);
    backend_.setGroundBase(groundBaseFor(&cam));
    core::TransitionSpec spec = cam.transition;
    if (cinematicTransition_) spec = *cinematicTransition_;
    if (cut || cam.mode == profiles::CameraMode::Flyby) spec.easing = core::Easing::Cut;
    effects_.reset();
    if (!engine_.hasPose()) engine_.place(target);
    else engine_.select(target, spec, true);
    // Each camera owns its temporary offsets; never inherit another camera's look.
    engine_.offsetLook(look.headingDeg,look.pitchDeg);
    engine_.offsetPosition(look.translationHeadingFrame);
    engine_.offsetFov(look.fovDeltaRad);
}

void Coordinator::cancelMouseDrag() {
    // Pointer/focus cancellation is also a gesture end: do not strand a temporary look.
    if(!spot_ && mouseMoved_ && mouseCameraId_==activeCameraId_) {
        if(const auto* cam=activeEntry();cam&&cam->resetMouseLookOnRelease)engine_.recenterLook();
    }
    mouseHeld_=mouseMoved_=false; mouseHeadingPending_=mousePitchPending_=0;
}

void Coordinator::onAcquired(double now) {
    acquireToken_ = 0;
    lastSubmitted_.reset();
    engine_.reset(); effects_.reset(); motionReceivedAt_ = -1;
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
        const auto* preferred = profile_->camera(cat.defaultCameraId);
        if (preferred && preferred->categoryId == cat.id && profiles::modeActivatable(preferred->mode)) return activate(preferred->id, now, false);
        for (const auto& id : cat.cameraIds) {
            const auto* cam = profile_->camera(id);
            if (cam && profiles::modeActivatable(cam->mode)) return activate(id, now, false);
        }
    }
    return "No other category has a camera that can be shown.";
}

void Coordinator::release(const std::string& reason, double now) {
    if (!spotActivating_) endSpot("Camera control was released.", false, true);
    cancelBinoculars();
    glance_.reset(); glanceReturn_.reset();  // never return from a glance after a release
    stopWalkaround();
    headTracking_.restore();
    if (!lookOwner_.empty()) rememberedLooks_[lookOwner_] = engine_.look();
    lookOwner_.clear();
    peek_.reset();
    pendingCameraId_.clear();
    afterReleaseCameraId_.clear();
    seedName_.clear();
    activeCameraId_.clear();
    engine_.reset(); effects_.reset(); motionReceivedAt_ = -1;
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

void Coordinator::panicRelease(double now) {
    const bool spotting = spot_.has_value() || autoSpot_;
    // release() already stops walkaround/orbit/fly-by motion, restores head tracking, drops a pending
    // re-activation and hands the camera back. A panic also forgets remembered temporary looks so a later
    // Enable starts from the saved pose, and neutralises any mouse gesture even when already Observing.
    release("Panic release: the simulator has control", now);
    rememberedLooks_.clear();
    cancelMouseDrag();
    emitLook(now, true);
    emit_("PanicReleased", {{"state", toString(state_)}, {"reason", stateReason_}, {"spottingStopped", spotting}});
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
    airportAutoZoomFov_.reset(); targetAt_ = -100;
    if (state_ == Ownership::Active || state_ == Ownership::SimulatorSuspended || state_ == Ownership::Acquiring)
        release(reason, now);
    peek_.reset();
    pendingCameraId_.clear();
    engine_.reset(); effects_.reset(); motionReceivedAt_ = -1; motionPaused_ = false;
    emit_("ConnectionChanged", {{"simConnected", simConnected_}, {"sessionEpoch", epoch_},
                                {"simVersion", simVersion_.empty() ? json(nullptr) : json(simVersion_)}, {"reason", reason}});
}

// Sim session lost (simulator quit or module shutting down): nothing to release; start over in Observing.
void Coordinator::resetSession(const std::string& reason, double now) {
    stopWalkaround(); motionPaused_ = false;
    rememberedLooks_.clear(); lookOwner_.clear(); targetAt_=-100; airportAutoZoomFov_.reset();
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
    engine_.reset(); effects_.reset(); motionReceivedAt_ = -1;
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
    engine_.reset(); effects_.reset(); motionReceivedAt_ = -1;
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
            if constexpr (std::is_same_v<T, backend_event::NativeMouseMapping>) {
                mouseMapped_ = e.ready; cancelMouseDrag();
                emit_("MouseInputDiagnostic", {{"mapped", e.ready}});
                emitCapabilities();
            } else if constexpr (std::is_same_v<T, backend_event::NativeMouseButton>) {
                // A repeated down notification is still the same gesture. Resetting its easing
                // accumulator here would drop motion and make a steady drag intermittently stop.
                if (e.down && mouseHeld_) return;
                if (!spot_ && !e.down && mouseMoved_ && mouseCameraId_ == activeCameraId_ && state_ == Ownership::Active && eligibleToSubmit()) {
                    if (const auto* camera = activeEntry(); camera && camera->resetMouseLookOnRelease) {
                        engine_.recenterLook(); emitLook(now, true);
                    }
                }
                mouseHeld_ = e.down && mouseMapped_ && !mouseBlocked_ && manualCameraReady();
                mouseMoved_ = false;
                mouseHeadingPending_ = mousePitchPending_ = 0; // stop immediately on release, without a settling jump
                if (e.down) {
                    mouseSamples_ = mouseFrames_ = mouseLargeSamples_ = 0;
                    mouseMaxSampleGap_ = mouseMaxFrameGap_ = mouseMaxRaw_ = 0;
                    mouseLastSampleAt_ = -1; mouseConcurrentLook_ = false;
                    mouseCameraId_ = activeCameraId_;
                }
                emit_("MouseInputDiagnostic", {{"rightDown", e.down}, {"accepted", mouseHeld_}, {"blockedByPanel", mouseBlocked_},
                    {"samples", mouseSamples_}, {"frames", mouseFrames_}, {"largeSamples", mouseLargeSamples_},
                    {"maxSampleGapMs", mouseMaxSampleGap_ * 1000}, {"maxSimFrameGapMs", mouseMaxFrameGap_ * 1000},
                    {"maxRaw", mouseMaxRaw_}, {"concurrentBoundLook", mouseConcurrentLook_}});
            } else if constexpr (std::is_same_v<T, backend_event::NativeMouseAxis>) {
                // Relative mouse counts, NOT a held joystick rate: never integrate a stale axis sample.
                if (!mouseHeld_ || mouseBlocked_ || mouseCameraId_ != activeCameraId_ || !manualCameraReady() || e.delta == 0) return;
                mouseMaxRaw_ = std::max(mouseMaxRaw_, std::abs(static_cast<double>(e.delta)));
                if (e.delta < -32768 || e.delta > 32767) ++mouseLargeSamples_;
                if (mouseLastSampleAt_ >= 0) mouseMaxSampleGap_ = std::max(mouseMaxSampleGap_, now - mouseLastSampleAt_);
                mouseLastSampleAt_ = now;
                // Relative mouse counts are not limited to the joystick +/-32K range.
                // Live drags reached 126574; rejecting those samples caused movement gaps.
                // Keep the measured small-step scale (642 counts ~= 0.118 degrees),
                // with the existing bounded pending angle protecting against extreme input.
                const double degrees = e.delta / 32768.0 * 6.0 * settings_.mouseSensitivity * binocularsLookScale_;
                double& pending = e.horizontal ? mouseHeadingPending_ : mousePitchPending_;
                pending = std::clamp(pending + (e.horizontal || settings_.mouseInvertY ? degrees : -degrees), -45.0, 45.0);
                mouseMoved_ = true;
                if (mouseSamples_++ < 2) emit_("MouseInputDiagnostic", {{"axis", e.horizontal ? "x" : "y"}, {"raw", e.delta}, {"degrees", degrees}});
                emitLook(now);
            } else if constexpr (std::is_same_v<T, backend_event::MotionTelemetry>) {
                if (e.motion.finite()) { flightMotion_ = e.motion; motionReceivedAt_ = now; }
                aircraftBankDeg_ = e.motion.finite() ? e.bankDeg : NAN;
                aircraftPitchDeg_ = e.motion.finite() ? e.pitchDeg : NAN;
            } else if constexpr (std::is_same_v<T, backend_event::GroundElevation>) {
                if (std::isfinite(e.meters) && e.meters > -1500 && e.meters < 10000) { groundAltitude_ = e.meters; groundAt_ = now; }
            } else if constexpr (std::is_same_v<T, backend_event::Traffic>) {
                traffic_.update(e.aircraft, e.userObjectId, targetAt_ >= 0 ? aircraftGeo() : profiles::airport::Geo{NAN, NAN, NAN}, now);
            } else if constexpr (std::is_same_v<T, backend_event::TrafficTarget>) {
                traffic_.updateTarget(e, now);
            } else if constexpr (std::is_same_v<T, backend_event::Connected>) {
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
                traffic_.clear();
                resetSession(e.reason.empty() ? simNameOf(simVersion_) + " closed the connection." : e.reason, now);
                if (wasConnected || aircraft_.known()) {
                    aircraft_ = {};
                    profile_.reset();
                    emit_("AircraftChanged", {{"title", nullptr}, {"aircraftPath", nullptr}, {"modelKey", nullptr}, {"livery", nullptr}});
                }
                setState(Ownership::Observing, "Waiting for " + simNameOf(simVersion_));
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
                traffic_.clear();
            } else if constexpr (std::is_same_v<T, backend_event::SimRunning>) {
                if (!e.running && (state_ == Ownership::Active || state_ == Ownership::Acquiring || state_ == Ownership::SimulatorSuspended))
                    release("The simulator left the flight.", now);
            } else if constexpr (std::is_same_v<T, backend_event::Paused>) {
                motionPaused_ = e.paused;
                // Ordinary pause keeps camera control; discard an in-flight mouse gesture on either edge.
                cancelMouseDrag();
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
        engine_.reset(); effects_.reset(); motionReceivedAt_ = -1;
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
                    engine_.freeze(); effects_.reset(); motionReceivedAt_ = -1;
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
                    engine_.freeze(); effects_.reset(); motionReceivedAt_ = -1;
                    setState(Ownership::SimulatorSuspended, "Paused by the simulator");
                } else if (!gameControlled_ && state_ == Ownership::SimulatorSuspended && wasGame && spotBare()) {
                    // A spot without a view underneath: its overlay is computed afresh, nothing to read back.
                    lastSubmitted_.reset();
                    setState(Ownership::Active, "Camera active");
                    emitActiveCamera();
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
    core::CameraPose pose = fromSdk(d.pose, qual_.contract);
    const auto* target=purpose==ReadPurpose::PreAcquire && profile_ ? profile_->camera(pendingCameraId_) :
        purpose==ReadPurpose::Capture && profile_ ? profile_->camera(readCameraId_) : activeEntry();
    if(d.pose.referential==Referential::World && target && (target->mode==profiles::CameraMode::Airport || target->mode==profiles::CameraMode::Free)){
        const bool free = target->mode==profiles::CameraMode::Free;
        pose.positionFRD=profiles::airport::local(free ? freeAnchor(*target) : profiles::airport::Geo{target->originLat,target->originLon,target->originAltitude},{d.pose.x,d.pose.y,d.pose.z});
        pose.orientation=core::Quat::fromPbh({-d.pose.pitchDeg,d.pose.bankDeg,d.pose.headingDeg});
        // A drone read back beyond the engine's anchor limits cannot seed a pose: recall from the saved one instead.
        if (free && !pose.withinLimits()) pose.positionFRD = {NAN, NAN, NAN};
        if (free && purpose == ReadPurpose::Resume) resetFreeMotion();  // the read already includes the flight
    }
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
                engine_.reset(); effects_.reset(); motionReceivedAt_ = -1;
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

bool Coordinator::eligibleToSubmit() const { return submitReady() && !activeCameraId_.empty(); }

bool Coordinator::submitReady() const {
    return state_ == Ownership::Active && simConnected_ && sdk_ == SdkAvailability::Acquired && !gameControlled_ && seedName_.empty();
}

core::CameraPose Coordinator::effectiveAirportPose(const CameraEntry& cam) const {
    auto pose=cam.pose;
    if (cam.mode != profiles::CameraMode::Airport || cam.airportHeightCategory.empty()) return pose;
    const auto height=settings_.airportCategoryHeights.find(cam.airportHeightCategory);
    if (height == settings_.airportCategoryHeights.end()) return pose;
    const double meters=cam.airportOverrides.heightMeters.value_or(std::max(height->second,cam.airportHeightMinimumMeters));
    pose.positionFRD.z=cam.originAltitude-cam.airportHeightBaseAltitudeMeters-meters;
    return pose;
}

const std::string& Coordinator::aircraftTrackingFor(const CameraEntry& cam) const {
    static const std::string off = "off";
    if (cam.mode == profiles::CameraMode::Mounted) return off;  // rigid on the airframe: never aims at the aircraft
    if (cam.mode == profiles::CameraMode::Airport && cam.airportOverrides.aircraftTracking)
        return *cam.airportOverrides.aircraftTracking;
    return settings_.globalAircraftTracking;
}

void Coordinator::updateAirportTrackingPosition() {
    airportTrackingPosition_.reset();
    const auto* cam=activeEntry();
    if (!cam || cam->mode!=profiles::CameraMode::Airport || !eligibleToSubmit() ||
        aircraftTrackingFor(*cam)=="off" || mouseHeld_ || !lastSubmitted_ ||
        lastSubmitted_->referential!=Referential::World) return;
    const auto intended=profiles::airport::world({cam->originLat,cam->originLon,cam->originAltitude},engine_.sample().pose.positionFRD);
    // Camera changes reset lastSubmitted_. For small continuous moves, carry
    // the observed terrain floor forward until the next SDK sample instead of
    // snapping back to the uncorrected scenery height while a key is held.
    const profiles::airport::Geo previous{lastSubmitted_->x,lastSubmitted_->y,lastSubmitted_->z};
    const auto movement=profiles::airport::local(previous,intended);
    if (movement.length()>25) return; // discontinuous edit: wait for new readback
    const auto actual=backend_.worldCameraPosition();
    if (!actual || !profiles::airport::Geo{actual->x,actual->y,actual->z}.valid()) return;
    const auto delta=profiles::airport::local(previous,{actual->x,actual->y,actual->z});
    if (std::hypot(delta.x,delta.y)>.5) return; // SDK still transitioning to another position
    airportTrackingPosition_=core::Vec3{intended.lat,intended.lon,
        actual->z>previous.altitude ? std::max(actual->z,intended.altitude) : intended.altitude};
}

void Coordinator::updateAirportAutoZoom(double dt) {
    const auto* cam = activeEntry();
    // Spotting zooms the aimed view (airport or free camera) on the target unless the camera turns auto zoom off.
    const bool spotAim = cam && spotAims(*cam);
    if (!cam || (!spotAim && (cam->mode != profiles::CameraMode::Airport || aircraftTrackingFor(*cam) == "off" || targetAt_ < 0)) || (mouseHeld_ && !spotAim)) {
        airportAutoZoomFov_.reset();
        return;
    }
    const auto& o=cam->airportOverrides;
    if (!o.autoZoomEnabled.value_or(spotAim || settings_.airportAutoZoomEnabled)) { airportAutoZoomFov_.reset(); return; }
    const auto pose=engine_.sample().pose;
    auto w=cam->mode==profiles::CameraMode::Free ? freeWorld(*cam,pose.positionFRD,lastTick_) :
        profiles::airport::world({cam->originLat,cam->originLon,cam->originAltitude},pose.positionFRD);
    if (airportTrackingPosition_) w={airportTrackingPosition_->x,airportTrackingPosition_->y,airportTrackingPosition_->z};
    const profiles::airport::Geo aimAt = spotAim ? spot_->target.position : profiles::airport::Geo{targetLat_,targetLon_,targetAltitude_};
    const auto d=profiles::airport::local(w,aimAt);
    const double distance=d.length();
    if (!d.finite() || !std::isfinite(distance) || distance <= .5 || std::abs(aimAt.lat)>90 || std::abs(aimAt.lon)>180) {
        airportAutoZoomFov_.reset();
        return;
    }
    const double minimum=core::clamp(o.autoZoomMinFovDeg.value_or(settings_.airportAutoZoomMinFovDeg),2.,120.)*core::kDegToRad;
    const double maximum=core::clamp(o.autoZoomMaxFovDeg.value_or(settings_.airportAutoZoomMaxFovDeg),minimum*core::kRadToDeg,120.)*core::kDegToRad;
    const double frame=core::clamp(o.autoZoomFrameMeters.value_or(settings_.airportAutoZoomFrameMeters),5.,200.);
    const double response=core::clamp(o.autoZoomResponseSeconds.value_or(settings_.airportAutoZoomResponseSeconds),.1,5.);
    // Exponential easing is independent of frame rate. Freeze through a sim pause;
    // telemetry gaps keep the last known target, matching aircraft tracking.
    const double target=traffic::autoZoomFov(distance,frame,minimum,maximum);
    double current=core::clamp(airportAutoZoomFov_.value_or(pose.fovRad),minimum,maximum);
    if (!motionPaused_ && dt > 0 && dt <= .1) current=traffic::easeFov(current,target,dt,response);
    airportAutoZoomFov_=core::clamp(current,minimum,maximum);
}

void Coordinator::updateHorizon(double dt, double now) {
    if (motionReceivedAt_ < 0) { horizon_.reset(); return; }  // session reset: nothing to ease from
    if (motionPaused_) return;  // hold through a sim pause
    const CameraEntry* cam = activeEntry();
    const bool fresh = now - motionReceivedAt_ < .5 && std::isfinite(aircraftBankDeg_);
    const bool relative = cam && (cam->mode == profiles::CameraMode::AircraftCockpit ||
                                  cam->mode == profiles::CameraMode::AircraftExterior || cam->mode == profiles::CameraMode::Orbit);
    // Stale attitude or a world camera eases back to the aircraft's own roll rather than holding an old correction.
    horizon_.tick(dt, fresh && relative ? core::horizonCounterRollDeg(aircraftBankDeg_, cam->followHorizon) : 0.0);
}

// Binoculars: while a hold is down the submitted FOV eases to the binoculars FOV, and back to the view's own FOV
// (including airport auto zoom) on release. Presentation only: the engine, saved poses, captures and the temporary
// look never see it. Releasing the camera, Panic and any loss of control cancel it immediately.
std::string Coordinator::setBinoculars(bool pressed, std::uint64_t holdId) {
    if (!pressed) {
        if (binocularsHolds_.erase(holdId) && binocularsHolds_.empty()) emitBinoculars();
        return {};
    }
    if (state_ != Ownership::Active || (activeCameraId_.empty() && !spot_)) return "Select a camera before using binoculars.";
    const bool wasHeld = !binocularsHolds_.empty();
    binocularsHolds_.insert(holdId);
    if (!wasHeld) emitBinoculars();
    return {};
}

void Coordinator::cancelBinoculars() {
    const bool wasHeld = !binocularsHolds_.empty();
    binocularsHolds_.clear();
    binocularsProgress_ = 0;
    binocularsLookScale_ = 1;
    updateBinoculars(0);  // restores the unscaled keyboard look speed
    if (wasHeld) emitBinoculars();
}

void Coordinator::updateBinoculars(double dt) {
    // Constant-rate progress shaped by smoothstep in binocularsFov(): time-based, frame-rate independent, and a
    // release mid-way reverses from the current zoom without a jump.
    const double target = binocularsHolds_.empty() ? 0.0 : 1.0;
    if (dt > 0) {
        const double step = dt / kBinocularsEaseSeconds;
        binocularsProgress_ = target > binocularsProgress_ ? std::min(target, binocularsProgress_ + step)
                                                           : std::max(target, binocularsProgress_ - step);
    }
    if (binocularsProgress_ <= 0) binocularsLookScale_ = 1;
    // Keyboard/controller look slows with the zoom so aiming stays controllable.
    if (binocularsProgress_ > 0 || binocularsAppliedLookScale_ != 1) {
        engine_.setSpeeds({settings_.lookSpeedDegPerSec * binocularsLookScale_,
                           settings_.moveSpeedMetersPerSec * settings_.movementPrecisionScale, 0.5, 6.0});
        binocularsAppliedLookScale_ = binocularsLookScale_;
    }
}

double Coordinator::binocularsFov(double shownFov, double cameraFov) {
    if (binocularsProgress_ <= 0 || !(shownFov > 0) || !(cameraFov > 0)) {
        binocularsLookScale_ = 1;
        return shownFov;
    }
    // Never wider than the camera's own FOV. Airport auto zoom is overridden while held and resumes on release.
    const double zoom = std::min(settings_.binocularsFovDeg * core::kDegToRad, cameraFov);
    const double fov = shownFov + (zoom - shownFov) * core::ease(core::Easing::Smoothstep3, binocularsProgress_);
    binocularsLookScale_ = core::clamp(fov / cameraFov, 0.01, 1.0);
    return fov;
}

void Coordinator::emitBinoculars() {
    emit_("BinocularsChanged", {{"active", !binocularsHolds_.empty()}, {"fovDeg", settings_.binocularsFovDeg}});
}

void Coordinator::updateGimbal(double dt, double now) {
    if (motionReceivedAt_ < 0) { gimbal_.reset(); return; }  // session reset: nothing to ease from
    if (motionPaused_) return;  // hold through a sim pause
    const CameraEntry* cam = activeEntry();
    const bool fresh = now - motionReceivedAt_ < .5 && std::isfinite(aircraftBankDeg_);
    // Older telemetry without pitch levels the roll only. Stale attitude eases back to locked on the airframe.
    const double pitch = std::isfinite(aircraftPitchDeg_) ? aircraftPitchDeg_ : 0.0;
    const bool stabilise = fresh && cam && cam->mode == profiles::CameraMode::Mounted;
    gimbal_.tick(dt, stabilise ? cam->gimbal : core::Gimbal::Locked, fresh ? pitch : NAN, fresh ? aircraftBankDeg_ : NAN);
}

void Coordinator::submit(bool refreshTrackingPosition) {
    const CameraEntry* cam = activeEntry();
    if (!cam || !eligibleToSubmit()) return;
    if (spot_ && spot_->overlay) return submitSpotOverlay();  // chase / fly-by: a computed world pose over the base view
    if (refreshTrackingPosition) updateAirportTrackingPosition();
    auto s = engine_.sample();
    s.pose = cinematicPose(*cam, s.pose);  // Commercial drift: presentation only, never written into the engine
    const auto& aircraftTracking = aircraftTrackingFor(*cam);
    auto submittedPose = effects_.apply(s.pose);
    // Aircraft-relative exterior views aim at the aircraft origin. Interior cameras
    // never inherit aircraft tracking even though the preference is global.
    if ((cam->mode == profiles::CameraMode::AircraftExterior || cam->mode == profiles::CameraMode::Orbit) &&
        aircraftTracking != "off" && !mouseHeld_) {
        const auto target = submittedPose.positionFRD * -1;
        const double horizontal = std::hypot(target.x,target.y);
        if (target.length() > .5) {
            auto angles = submittedPose.orientation.toPbh();
            if (horizontal > .5) angles.headingDeg = std::atan2(target.y,target.x)*core::kRadToDeg;
            if (aircraftTracking == "both") angles.pitchDeg = core::clamp(std::atan2(-target.z,horizontal)*core::kRadToDeg,-89.,89.);
            submittedPose.orientation = core::Quat::fromPbh(angles);
        }
    }
    // Follow horizon: after aiming, so exterior views stay on the aircraft. Never written back to the engine.
    double horizonRoll = 0;
    if (cam->mode == profiles::CameraMode::AircraftCockpit || cam->mode == profiles::CameraMode::AircraftExterior ||
        cam->mode == profiles::CameraMode::Orbit) {
        horizonRoll = horizon_.rollDeg();
        submittedPose = core::counterRoll(submittedPose, horizonRoll, cam->mode != profiles::CameraMode::AircraftCockpit);
    }
    // Mounted cameras stay rigid on the airframe (no tracking, no follow horizon): gimbal, then airframe vibration.
    if (cam->mode == profiles::CameraMode::Mounted) {
        submittedPose.orientation = gimbal_.apply(submittedPose.orientation);
        submittedPose = mountVibration_.apply(submittedPose);
    }
    SdkPose sdk = toSdk(submittedPose, qual_.contract, referentialFor(*cam));
    if(cam->mode==profiles::CameraMode::Airport || cam->mode==profiles::CameraMode::Flyby || cam->mode==profiles::CameraMode::Free){
        // Keep WORLD translation and FOV independent of aircraft tracking. Submit the
        // explicit look-at PBH below rather than the SDK's combined TARGETED mode.
        sdk.trackUserAircraft = false;
        const bool flyby = cam->mode==profiles::CameraMode::Flyby;
        auto w=cam->mode==profiles::CameraMode::Free ? freeWorld(*cam,s.pose.positionFRD,lastTick_) :
            profiles::airport::world(flyby ? profiles::airport::Geo{flybyLat_,flybyLon_,flybyAltitude_} : profiles::airport::Geo{cam->originLat,cam->originLon,cam->originAltitude},s.pose.positionFRD);
        sdk.x=w.lat;sdk.y=w.lon;sdk.z=w.altitude;
        // Keep the requested translation intact; only look-at/zoom use the
        // simulator's terrain-adjusted position. No profile height is rewritten.
        if (!flyby && airportTrackingPosition_) w={airportTrackingPosition_->x,airportTrackingPosition_->y,airportTrackingPosition_->z};
        // World PBH uses local geographic north, not the aircraft's measured translational basis.
        auto p=s.pose.orientation.toPbh();
        // Spotting aims the view at the target aircraft (heading and pitch, no horizon guard) instead of the user's.
        const bool spotAim = spotAims(*cam);
        const profiles::airport::Geo aimAt = spotAim ? spot_->target.position : profiles::airport::Geo{targetLat_,targetLon_,targetAltitude_};
        // Keep the last known target through pauses/telemetry gaps instead of snapping
        // back to the saved direction. Flight changes invalidate targetAt_.
        if((spotAim || (aircraftTracking!="off" && targetAt_>=0)) && !mouseHeld_) {
            auto d=profiles::airport::local({w.lat,w.lon,w.altitude},aimAt);
            const double horizontal=std::hypot(d.x,d.y);
            if(d.finite() && horizontal>.5) {
                p.headingDeg=std::atan2(d.y,d.x)*core::kRadToDeg;
                if(spotAim || aircraftTracking=="both") {
                    p.pitchDeg=core::clamp(std::atan2(-d.z,horizontal)*core::kRadToDeg,-89.0,89.0);
                    if (!spotAim && cam->mode==profiles::CameraMode::Airport && cam->airportOverrides.trackingHorizonGuard.value_or(settings_.airportTrackingHorizonGuard) && d.z>20) {
                        // Across distant terrain the aircraft can be below the visible
                        // horizon. Gradually level the view instead of zooming into dirt.
                        // Preserve exact nearby tracking and all upward tracking.
                        const double distanceBlend=core::clamp((horizontal-500.)/1000.,0.,1.);
                        const double heightBlend=core::clamp((d.z-20.)/20.,0.,1.);
                        const auto smooth=[](double x){return x*x*(3-2*x);};
                        p.pitchDeg*=1-smooth(distanceBlend)*smooth(heightBlend);
                    }
                }
            }
        }
        sdk.pitchDeg=-p.pitchDeg;sdk.bankDeg=p.bankDeg;sdk.headingDeg=p.headingDeg;
    }
    if ((cam->mode==profiles::CameraMode::Airport || spotAims(*cam)) && airportAutoZoomFov_) sdk.fovRad=*airportAutoZoomFov_;
    if (spotAims(*cam)) applySpotAdjustments(sdk);
    sdk.fovRad = binocularsFov(sdk.fovRad, spot_ ? sdk.fovRad : s.pose.fovRad);
    if (lastSubmitted_ && nearlyEqual(*lastSubmitted_, sdk)) return;
    if (backend_.submitPose(sdk)) {
        lastSubmitted_ = sdk;
        submittedHorizonRollDeg_ = horizonRoll;
        ++submissions_;
    }
}

void Coordinator::onAction(const ActionEvent& ev, double now) {
    if (ev.pulse) {
        if (ev.pressed && manualCameraReady() && !mouseBlocked_ &&
            (ev.action == Action::ZoomIn || ev.action == Action::ZoomOut)) {
            const double zoom = (ev.action == Action::ZoomIn ? -2.0 : 2.0) * core::kDegToRad;
            if (spot_) adjustSpot(0, 0, {}, zoom); else engine_.offsetFov(zoom);
            emitLook(now, true);
        }
        return;
    }
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
        case Action::GlanceHold:
            if (!ev.pressed) {
                glanceHoldReleased(ev.holdId, now);
                return;
            }
            result = glanceHold(ev.holdId, now);
            break;
        case Action::GlanceNext: if (ev.pressed) result = glanceNext(now); else return; break;
        case Action::GlanceReturn: if (ev.pressed) result = glanceReturn(now); else return; break;
        case Action::PanicRelease:
            if (!ev.pressed) return;
            panicRelease(now);
            break;
        case Action::Binoculars:
            if (!ev.pressed) { setBinoculars(false, ev.holdId); return; }
            result = setBinoculars(true, ev.holdId);
            break;
        case Action::RecenterLook:
            if (!ev.pressed) return;
            if (spot_) { recenterSpot(); break; }
            engine_.recenterLook();
            resetFreeMotion();
            emitLook(now, true);
            break;
        case Action::ToggleEditor:
            if (!ev.pressed) return;
            emit_("SurfaceRequest", {{"target", "main"}, {"op", "toggle"}});
            break;
        case Action::ToggleMouseLook:
            if (!ev.pressed || !manualCameraReady()) return;
            emit_("SurfaceRequest", {{"target", "mini"}, {"op", "mouseLook"}});
            break;
        case Action::ToggleHeadTracking:
            if (!ev.pressed) return;
            emit_("HeadTrackingToggleRequested", {{"enabled",!settings_.globalHeadTrackingEnabled}});
            break;
        case Action::SaveViews:
            if (!ev.pressed) return;
            emit_("SaveViewsRequested", json::object());
            break;
        case Action::ToggleAirportAuto:
            if (!ev.pressed) return;
            emit_("AirportAutoToggleRequested", json::object());
            break;
        case Action::ToggleMini:
            if (!ev.pressed) return;
            emit_("SurfaceRequest", {{"target", "mini"}, {"op", "toggle"}});
            break;
        case Action::ToggleCinematic:
            if (!ev.pressed) return;
            emit_("CinematicToggleRequested", json::object());
            break;
        case Action::SpotNext:
        case Action::SpotPrevious:
            if (!ev.pressed) return;
            emit_("SpotCycleRequested", {{"delta", ev.action == Action::SpotNext ? 1 : -1}});
            break;
        case Action::SpotReturn:
            if (!ev.pressed) return;
            emit_("SpotReturnRequested", json::object());
            break;
        case Action::ToggleAutoSpot:
            if (!ev.pressed) return;
            emit_("AutoSpotToggleRequested", json::object());
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

    traffic_.prune(now);
    // Spotting needs camera control: a refused or lost claim ends it (Release and recalls end it where they happen).
    if (spot_ && (state_ == Ownership::Observing || state_ == Ownership::Faulted || state_ == Ownership::OtherClient ||
                  state_ == Ownership::UserDisabled))
        endSpot("Camera control was lost.", false, true);
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

    if (spot_ && submitReady()) updateSpotManual(dt);
    if (spotBare() && submitReady()) {
        tickBareSpot(dt, now);
        return;
    }
    if (state_ == Ownership::Active && eligibleToSubmit()) {
        if (spot_) {
            updateSpot(dt, now);  // may return to the earlier view or release (a target lost for good)
            if (state_ != Ownership::Active || !eligibleToSubmit()) return;
            if (spotBare()) return;  // the view underneath went away this frame
        }
        // Simulator head tracking adds its offsets to an add-on camera: off while spotting, so the computed and aimed
        // spotting views stay exactly on the target; the global preference again afterwards.
        if (spot_) headTracking_.update("spot", "off", now);
        else if(activeEntry())headTracking_.update("global",settings_.globalHeadTrackingEnabled ? "on" : "off",now);
        emitCapabilities();
        const auto* effectCamera = activeEntry();
        const double effectsScale = effectCamera ? effectsScaleFor(effectCamera->mode) : 0.;
        effects_.tick(dt, flightMotion_, effectCamera ? effectCamera->effects : core::EffectsSettings{},
                      effectsScale > 0 && motionReceivedAt_ >= 0 && now - motionReceivedAt_ < .5, effectsScale,
                      effectCamera && headModelFor(effectCamera->mode));
        const bool mounted = effectCamera && effectCamera->mode == profiles::CameraMode::Mounted;
        mountVibration_.tick(dt, flightMotion_, mounted ? effectCamera->mountVibration : 0.,
                             mounted && motionReceivedAt_ >= 0 && now - motionReceivedAt_ < .5);
        if (!spot_ && mouseHeld_ && !mouseBlocked_ && mouseCameraId_ == activeCameraId_) {
            ++mouseFrames_;
            mouseMaxFrameGap_ = std::max(mouseMaxFrameGap_, dt);
            mouseConcurrentLook_ = mouseConcurrentLook_ || manual_.lookRight != 0 || manual_.lookUp != 0;
            // Frame-rate independent easing of relative deltas. No persistent axis rate or post-release drift.
            const double blend = settings_.mouseSmoothingMs <= 0 ? 1.0 : -std::expm1(-dt / (settings_.mouseSmoothingMs / 1000.0));
            const double heading = mouseHeadingPending_ * blend, pitch = mousePitchPending_ * blend;
            engine_.offsetLook(heading, pitch);
            mouseHeadingPending_ -= heading; mousePitchPending_ -= pitch;
        }
        std::optional<core::Quat> movement;
        if (effectCamera && aircraftTrackingFor(*effectCamera) != "off" && lastSubmitted_) {
            if (lastSubmitted_->referential == Referential::World)
                movement=core::Quat::fromPbh({-lastSubmitted_->pitchDeg,lastSubmitted_->bankDeg,lastSubmitted_->headingDeg});
            else if (lastSubmitted_->referential == Referential::SimObject)
                // Move in the saved (un-rolled) frame: follow horizon swings the shown pose about the aircraft.
                movement=(core::Quat::fromAxisAngle({1,0,0},-submittedHorizonRollDeg_*core::kDegToRad)*fromSdk(*lastSubmitted_,qual_.contract).orientation).normalized();
        }
        const bool manualActive = mouseHeld_ || manual_.lookRight != 0 || manual_.lookUp != 0 || manual_.moveForward != 0 ||
            manual_.moveRight != 0 || manual_.moveUp != 0 || manual_.zoomIn != 0;
        engine_.tick(dt, spot_ ? core::ManualInput{} : effectCamera && effectCamera->mode == profiles::CameraMode::Free ? lookOnly(manual_) : manual_, movement);
        if (!spot_) updateFreeCamera(dt, now);
        updateGlance(now);
        if (effectCamera && (effectCamera->mode == profiles::CameraMode::Orbit || effectCamera->mode == profiles::CameraMode::Flyby) && !engine_.transitioning() && !spot_ && !manualActive && !motionPaused_ && dt <= .1) {
            motionTime_ += dt;
            engine_.animateBase(motionPose(*effectCamera));
        }
        if (walkaround_ && !engine_.transitioning()) {
            if (manualActive) walkaround_->elapsed = 0;
            else if (!motionPaused_ && dt <= .1) walkaround_->elapsed += dt;
            if (walkaround_->elapsed >= walkaround_->interval) {
                std::vector<std::string> ids;
                if (profile_) for (const auto& category : profile_->categories) if (category.id == walkaround_->categoryId)
                    for (const auto& id : category.cameraIds) if (const auto* entry=profile_->camera(id); entry && entry->mode==profiles::CameraMode::AircraftExterior) ids.push_back(id);
                const auto current = std::find(ids.begin(),ids.end(),activeCameraId_);
                if (ids.empty() || current == ids.end() || (std::next(current) == ids.end() && !walkaround_->loop)) stopWalkaround();
                else {
                    const auto next = std::next(current) == ids.end() ? ids.front() : *std::next(current);
                    walkaround_->elapsed = 0;
                    walkaroundAdvancing_ = true;
                    const auto error = activate(next,now,false);
                    walkaroundAdvancing_ = false;
                    if (!error.empty()) stopWalkaround();
                }
            }
        }
        const auto s = engine_.sample();
        if (!s.valid) {
            fault("invalidPose", "Camera stopped: the computed view was outside the allowed range.", now);
            return;
        }
        updateAirportTrackingPosition();
        updateAirportAutoZoom(dt);
        updateBinoculars(dt);
        cinematicDrift_.tick(dt, cinematicDriftHold_ || manualActive, motionPaused_);
        updateHorizon(dt, now);
        updateGimbal(dt, now);
        submit(false);
        if (engine_.transitioning() != lastTransitioning_) emitActiveCamera();
        emitLook(now);
    }
}

// ---------------------------------------------------------------------------------------------------------------
// Smart views (glance)

namespace {
constexpr const char* kNoGlanceViews =
    "No glance views in this profile. Turn on Glance view for an instrument camera in its Behavior settings.";
core::Vec3 lookDirection(const core::Quat& q) { return q.rotate({1, 0, 0}); }
double angleBetweenDirections(const core::Vec3& a, const core::Vec3& b) {
    const double n = a.length() * b.length();
    return n > 1e-12 ? std::acos(core::clamp(core::dot(a, b) / n, -1.0, 1.0)) : std::numbers::pi;
}
}  // namespace

std::vector<const CameraEntry*> glanceViews(const profiles::ProfileSnapshot& profile, const std::string& exclude) {
    std::vector<const CameraEntry*> out;
    for (const auto& category : profile.categories)
        for (const auto& id : category.cameraIds) {
            const auto* cam = profile.camera(id);
            if (cam && cam->glance && cam->mode == profiles::CameraMode::AircraftCockpit && profiles::modeActivatable(cam->mode) &&
                cam->id != exclude && std::find(out.begin(), out.end(), cam) == out.end())
                out.push_back(cam);
        }
    return out;
}

double glanceAngle(const core::CameraPose& shown, const CameraEntry& camera) {
    const auto aim = lookDirection(camera.pose.orientation);
    auto toward = camera.pose.positionFRD + aim * kGlanceFocusMeters - shown.positionFRD;
    if (toward.length() < 1e-3) toward = aim;
    return angleBetweenDirections(lookDirection(shown.orientation), toward);
}

const CameraEntry* nearestGlanceView(const profiles::ProfileSnapshot& profile, const core::CameraPose& shown, const std::string& exclude) {
    const CameraEntry* best = nullptr;
    double bestAngle = 0, bestAim = 0;
    const auto look = lookDirection(shown.orientation);
    for (const auto* cam : glanceViews(profile, exclude)) {
        const double angle = glanceAngle(shown, *cam);
        const double aim = angleBetweenDirections(look, lookDirection(cam->pose.orientation));
        const bool tie = best && std::abs(angle - bestAngle) <= kGlanceTieRadians;
        if (!best || (tie ? aim < bestAim : angle < bestAngle)) {
            best = cam;
            bestAngle = angle;
            bestAim = aim;
        }
    }
    return best;
}

bool Coordinator::glanceCurrent() const {
    return glance_ && !activeCameraId_.empty() && activeCameraId_ == glance_->cameraId &&
           (state_ == Ownership::Active || state_ == Ownership::SimulatorSuspended);
}

void Coordinator::dropStaleGlance() {
    if (glance_ && !glanceCurrent()) glance_.reset();
    if (glanceReturn_ && (glanceReturn_->cameraId != activeCameraId_ || state_ != Ownership::Active)) glanceReturn_.reset();
}

json Coordinator::glanceJson() const {
    if (!glanceCurrent()) return nullptr;
    return {{"cameraId", glance_->cameraId}, {"returnCameraId", glance_->returnCameraId}, {"held", glance_->holdId != 0}};
}

std::string Coordinator::glanceBlock() const {
    if (const auto block = blockReason(); !block.empty()) return block;
    if (spot_) return "Glance is not available while spotting. Return to your aircraft first.";
    const CameraEntry* cam = activeEntry();
    if (state_ != Ownership::Active || !cam || !engine_.hasPose()) return "Show a cockpit view first: a glance returns to the view you are in.";
    if (cam->mode != profiles::CameraMode::AircraftCockpit) return "Glance views work from cockpit views.";
    return {};
}

std::string Coordinator::glanceHold(std::uint64_t holdId, double) {
    dropStaleGlance();
    if (const auto block = glanceBlock(); !block.empty()) return block;
    // Where the user is looking now, temporary mouse look and any running transition included.
    const auto* target = nearestGlanceView(*profile_, engine_.sample().pose, glance_ ? glance_->returnCameraId : activeCameraId_);
    if (!target) return kNoGlanceViews;
    startGlance(*target);
    glance_->holdId = holdId;
    emitActiveCamera();
    return {};
}

void Coordinator::glanceHoldReleased(std::uint64_t holdId, double now) {
    dropStaleGlance();
    // Ended or superseded (a recall, Release, Panic, Return): nothing to return to.
    if (!glance_ || holdId == 0 || glance_->holdId != holdId) return;
    glanceReturn(now);
}

std::string Coordinator::glanceNext(double) {
    dropStaleGlance();
    if (const auto block = glanceBlock(); !block.empty()) return block;
    const auto views = glanceViews(*profile_, glance_ ? glance_->returnCameraId : activeCameraId_);
    if (views.empty()) return kNoGlanceViews;
    // Step in category order from the view on screen, wrapping.
    std::vector<std::string> order;
    for (const auto& category : profile_->categories) order.insert(order.end(), category.cameraIds.begin(), category.cameraIds.end());
    const CameraEntry* target = views.front();
    const auto anchor = std::find(order.begin(), order.end(), activeCameraId_);
    if (anchor != order.end()) {
        const auto start = static_cast<std::size_t>(anchor - order.begin());
        for (std::size_t k = 1; k <= order.size(); ++k) {
            const auto& id = order[(start + k) % order.size()];
            const auto it = std::find_if(views.begin(), views.end(), [&](const CameraEntry* v) { return v->id == id; });
            if (it != views.end()) {
                target = *it;
                break;
            }
        }
    }
    startGlance(*target);
    emitActiveCamera();
    return {};
}

std::string Coordinator::glanceReturn(double) {
    dropStaleGlance();
    if (!glance_) return "Not glancing: there is no view to return to.";
    const Glance g = *glance_;
    glance_.reset();
    const CameraEntry* cam = profile_ ? profile_->camera(g.returnCameraId) : nullptr;
    if (!cam || !profiles::modeActivatable(cam->mode) || !blockReason().empty()) {
        emitActiveCamera();
        return "The view you glanced from is no longer available.";
    }
    if (state_ == Ownership::SimulatorSuspended) {
        activeCameraId_ = cam->id;  // shown when the simulator hands the camera back, as a recall would be
        emitActiveCamera();
        return {};
    }
    cancelMouseDrag();
    glanceTransition(*cam, core::compose(effectiveAirportPose(*cam), g.returnLook));
    glanceReturn_ = GlanceReturn{cam->id, g.returnLook};
    emitActiveCamera();
    return {};
}

void Coordinator::startGlance(const CameraEntry& target) {
    cancelMouseDrag();
    if (!glance_) {
        Glance g;
        if (glanceReturn_ && glanceReturn_->cameraId == activeCameraId_) {
            g.returnCameraId = glanceReturn_->cameraId;  // still gliding back: keep the exact look it returns to
            g.returnLook = glanceReturn_->look;
        } else {
            g.returnCameraId = activeCameraId_;
            g.returnLook = engine_.look();
        }
        rememberedLooks_[g.returnCameraId] = g.returnLook;  // the view keeps its own look, as on any recall
        glance_ = g;
    }
    glanceReturn_.reset();
    glance_->cameraId = target.id;
    // Glance views open at their saved pose.
    if (target.id != activeCameraId_) glanceTransition(target, target.pose);
}

void Coordinator::glanceTransition(const CameraEntry& cam, const core::CameraPose& target) {
    // Start from what is on screen, temporary look included, so neither direction jumps.
    const auto shown = engine_.sample();
    if (shown.valid) engine_.place(shown.pose);
    lastSubmitted_.reset();
    airportTrackingPosition_.reset();
    airportAutoZoomFov_.reset();
    effects_.reset();
    backend_.setFlag(CameraFlag::Interaction, cam.interaction);
    backend_.setFlag(CameraFlag::AboveGround, false);
    backend_.setGroundBase(NAN);
    engine_.select(target, {core::Easing::Smoothstep3, kGlanceTransitionSeconds, 0.0}, true);
    activeCameraId_ = cam.id;
    lookOwner_ = cam.id;
}

void Coordinator::updateGlance(double now) {
    if (!glanceReturn_ || engine_.transitioning()) return;
    const GlanceReturn r = *glanceReturn_;
    glanceReturn_.reset();
    const CameraEntry* cam = activeEntry();
    if (!cam || cam->id != r.cameraId) return;
    // Landed: give the look back to the temporary look, so the base is the saved pose and the look is exactly
    // what it was. Anything moved during the return stays applied on top.
    auto look = r.look;
    const auto moved = engine_.look();
    look.headingDeg = core::wrapDeg(look.headingDeg + moved.headingDeg);
    look.pitchDeg += moved.pitchDeg;
    look.translationHeadingFrame += moved.translationHeadingFrame;
    look.fovDeltaRad += moved.fovDeltaRad;
    engine_.place(effectiveAirportPose(*cam));
    engine_.setLook(look);
    emitLook(now, true);
}

// ---------------------------------------------------------------------------------------------------------------
// Cinematic mode

bool Coordinator::selectCinematicCamera(const std::string& cameraId, double now, bool acquire, bool cut, const core::DriftSpec& drift) {
    if (acquire ? !canActivate() : !eligibleToSubmit()) return false;
    const CameraEntry* cam = profile_ ? profile_->camera(cameraId) : nullptr;
    if (!cam) return false;
    // A smooth move between views in the same frame keeps the shown drift offset and fades it; a cut drops it.
    const CameraEntry* old = state_ == Ownership::Active ? activeEntry() : nullptr;
    const bool carry = !cut && old && !cinematicCameraId_.empty() && old->id == cinematicCameraId_ && referentialFor(*old) == referentialFor(*cam) &&
                       old->originLat == cam->originLat && old->originLon == cam->originLon && old->originAltitude == cam->originAltitude;
    cinematicTransition_ = cut ? core::TransitionSpec{core::Easing::Cut, 0.0, 0.0}
                               : core::TransitionSpec{core::Easing::Smoothstep5, kCinematicTransitionSeconds, 0.0};
    const auto error = activate(cameraId, now, false);
    cinematicTransition_.reset();
    if (!error.empty()) return false;
    cinematicDrift_.start(drift, carry);
    cinematicCameraId_ = cameraId;
    return true;
}

core::CameraPose Coordinator::cinematicPose(const CameraEntry& cam, const core::CameraPose& pose) const {
    // Only the view Cinematic mode chose; a glance on top of it shows the instrument's own pose.
    if (cinematicCameraId_.empty() || cam.id != cinematicCameraId_ || glanceCurrent()) return pose;
    return cinematicDrift_.apply(pose);
}

// Free cameras (drones)
//
// A free camera is world referential like an airport camera: worldOrigin plus a local NED pose about it. Drop drone
// here and Save adjusted view anchor the origin at the aircraft, so the pose is also the drone's offset from the
// aircraft and Follow aircraft can keep that offset in world axes. Keyboard flight is integrated here, not by the
// engine (whose anchor-relative limit is 10 km): it is unsaved movement on top of the engine pose until a Save.

namespace {
double clampUnitRate(double v) { return std::isfinite(v) ? core::clamp(v, -1.0, 1.0) : 0.0; }
constexpr double kFreeAccelerationPerSec = 6.0;   // as the engine's manual movement
constexpr double kFreeMaxPlacementMeters = 9000;  // keeps a placed pose inside the profile's 10 km limit
constexpr double kDroneDefaultFovRad = 1.0821;
}  // namespace

bool Coordinator::freeCameraShown() const {
    if (state_ != Ownership::Active || spot_ || glanceCurrent()) return false;
    const CameraEntry* cam = activeEntry();
    return cam && cam->mode == profiles::CameraMode::Free;
}

bool Coordinator::aircraftPositionFresh(double now) const {
    return targetAt_ >= 0 && now - targetAt_ <= 5 && aircraftGeo().valid() && std::isfinite(targetHeading_);
}

profiles::airport::Geo Coordinator::freeAnchor(const CameraEntry& cam) const {
    if (cam.followAircraft) {
        // The aircraft, or where it was last seen while its position is unavailable.
        if (targetAt_ >= 0 && aircraftGeo().valid()) return aircraftGeo();
        if (freeCameraId_ == cam.id && freeAnchor_.valid()) return freeAnchor_;
        return {NAN, NAN, NAN};
    }
    if (freeSavedOrigin_ && freeCameraId_ == cam.id) return *freeSavedOrigin_;
    return {cam.originLat, cam.originLon, cam.originAltitude};
}

double Coordinator::freeTerrainAltitude(const profiles::airport::Geo& drone, double now) const {
    // SimConnect reports the terrain under the aircraft only, so it stands in for the terrain under a drone near it.
    if (!(groundAt_ >= 0) || now - groundAt_ > kFreeTerrainFreshSeconds || !std::isfinite(groundAltitude_)) return NAN;
    if (!(targetAt_ >= 0) || !aircraftGeo().valid() || !drone.valid()) return NAN;
    const auto d = profiles::airport::local(aircraftGeo(), drone);
    return std::hypot(d.x, d.y) <= kFreeTerrainRadiusMeters ? groundAltitude_ : NAN;
}

profiles::airport::Geo Coordinator::freeWorld(const CameraEntry& cam, const core::Vec3& local, double now) const {
    auto w = profiles::airport::world(freeAnchor(cam), local + freeOffset_);
    if (cam.followAircraft) {
        // The aircraft can carry a chase drone into the ground (a landing): presentation keeps it above terrain.
        const double terrain = freeTerrainAltitude(w, now);
        if (std::isfinite(terrain)) w.altitude = std::max(w.altitude, terrain + kFreeTerrainClearanceMeters);
    }
    return w;
}

std::string Coordinator::freeBlock(const CameraEntry& cam, double now) const {
    if (cam.mode != profiles::CameraMode::Free) return {};
    if (!cam.worldOriginValid) return "This drone has no position yet. Use Drop drone here to place it.";
    if (cam.followAircraft && !aircraftPositionFresh(now)) return "Waiting for the aircraft position before a drone can follow it.";
    return {};
}

void Coordinator::resetFreeMotion() {
    freeOffset_ = {};
    freeVelocity_ = {};
    freeAccumulator_ = 0;
}

void Coordinator::updateFreeCamera(double dt, double now) {
    const CameraEntry* cam = activeEntry();
    if (!cam || cam->mode != profiles::CameraMode::Free) {
        if (!freeCameraId_.empty()) {
            resetFreeMotion();
            freeCameraId_.clear();
            freeSavedOrigin_.reset();
        }
        return;
    }
    const profiles::airport::Geo origin{cam->originLat, cam->originLon, cam->originAltitude};
    const bool sameCamera = freeCameraId_ == cam->id;
    if (freeSavedOrigin_ && (!sameCamera || (origin.lat == freeSavedOrigin_->lat && origin.lon == freeSavedOrigin_->lon &&
                                             origin.altitude == freeSavedOrigin_->altitude)))
        freeSavedOrigin_.reset();  // the profile carries the saved origin now
    const bool originMoved = !sameCamera || origin.lat != freeOrigin_.lat || origin.lon != freeOrigin_.lon ||
                             origin.altitude != freeOrigin_.altitude;
    const auto anchor = freeAnchor(*cam);
    const auto base = engine_.sample().pose.positionFRD;
    if (originMoved) {
        resetFreeMotion();  // another drone, Drop drone here on this one, or a Save: the saved pose decides
    } else if (freeFollowing_ != cam->followAircraft && freeAnchor_.valid() && anchor.valid()) {
        // Follow aircraft switched in flight: keep the drone where it is, unless that is out of reach of the new anchor.
        const auto offset = profiles::airport::local(anchor, profiles::airport::world(freeAnchor_, base + freeOffset_)) - base;
        const auto total = base + offset;
        const bool reachable = std::hypot(total.x, total.y) <= kFreeRadiusMeters && std::abs(total.z) <= kFreeCeilingMeters;
        freeOffset_ = reachable ? offset : core::Vec3{};
        freeVelocity_ = {};
    }
    freeCameraId_ = cam->id;
    freeOrigin_ = origin;
    freeFollowing_ = cam->followAircraft;
    freeAnchor_ = anchor;
    if (!anchor.valid() || !(dt > 0) || !std::isfinite(dt)) return;
    if (dt > core::ManualMover::kMaxGapSeconds) {  // a stall: reseed from rest rather than catch up
        freeVelocity_ = {};
        freeAccumulator_ = 0;
        return;
    }
    // Held movement in the drone's own frame: forward and right follow where it looks (aircraft tracking included),
    // up is world up. Diagonals are not faster.
    core::Vec3 in{clampUnitRate(manual_.moveForward), clampUnitRate(manual_.moveRight), clampUnitRate(manual_.moveUp)};
    if (in.length() > 1) in = in * (1 / in.length());
    const auto orientation = lastSubmitted_ && lastSubmitted_->referential == Referential::World
                                 ? core::Quat::fromPbh({-lastSubmitted_->pitchDeg, lastSubmitted_->bankDeg, lastSubmitted_->headingDeg})
                                 : engine_.sample().pose.orientation;
    const auto direction = orientation.rotate({in.x, in.y, 0}) + core::Vec3{0, 0, -in.z};
    // Speed grows with height above the ground: terrain when known, otherwise the lower of the anchor and the aircraft.
    const auto drone = profiles::airport::world(anchor, base + freeOffset_);
    const double terrain = freeTerrainAltitude(drone, now);
    const double ground = std::isfinite(terrain)                       ? terrain
                          : targetAt_ >= 0 && aircraftGeo().valid() ? std::min(anchor.altitude, targetAltitude_)
                                                                     : anchor.altitude;
    const double height = std::max(0.0, drone.altitude - ground);
    const double speed = settings_.moveSpeedMetersPerSec * settings_.movementPrecisionScale * (manual_.fast ? 4.0 : 1.0) *
                         core::clamp(1 + height * kFreeSpeedPerMeterAboveGround, 1.0, kFreeMaxSpeedFactor);
    const auto target = direction * speed;
    // Bounds stop movement and never push a saved pose: within kFreeRadiusMeters of the anchor horizontally, at most
    // kFreeCeilingMeters above it, and not below terrain + clearance (terrain known) or the anchor less
    // kFreeFloorMarginMeters (no terrain).
    const double floor = std::isfinite(terrain) ? terrain + kFreeTerrainClearanceMeters : anchor.altitude - kFreeFloorMarginMeters;
    const double lowest = anchor.altitude - floor;  // the largest "down" offset
    const double alpha = -std::expm1(-core::ManualMover::kStep * kFreeAccelerationPerSec);
    freeAccumulator_ += dt;
    while (freeAccumulator_ >= core::ManualMover::kStep) {
        freeAccumulator_ -= core::ManualMover::kStep;
        freeVelocity_ = freeVelocity_ + (target - freeVelocity_) * alpha;
        const auto current = base + freeOffset_;
        auto next = current + freeVelocity_ * core::ManualMover::kStep;
        const double radius = std::hypot(next.x, next.y), reach = std::max(kFreeRadiusMeters, std::hypot(current.x, current.y));
        if (radius > reach) {
            next.x *= reach / radius;
            next.y *= reach / radius;
            freeVelocity_.x = freeVelocity_.y = 0;
        }
        const double down = std::max(lowest, current.z), up = std::min(-kFreeCeilingMeters, current.z);
        if (next.z > down) { next.z = down; freeVelocity_.z = 0; }
        if (next.z < up) { next.z = up; freeVelocity_.z = 0; }
        freeOffset_ = next - base;
    }
    if (target.length() == 0 && freeVelocity_.length() < 1e-4) freeVelocity_ = {};  // a released key fully stops
}

json Coordinator::dronePlacement(const profiles::airport::Geo& drone, const core::Quat& orientation, double fovRad) const {
    profiles::airport::Geo origin = drone;
    core::Vec3 position;
    if (targetAt_ >= 0 && aircraftGeo().valid()) {
        // Anchored at the aircraft: the pose is the drone's offset from it, which Follow aircraft keeps.
        auto offset = profiles::airport::local(aircraftGeo(), drone);
        if (std::max({std::abs(offset.x), std::abs(offset.y), std::abs(offset.z)}) <= kFreeMaxPlacementMeters) {
            origin = aircraftGeo();
            position = offset;
        } else {
            offset = {core::clamp(offset.x, -kFreeMaxPlacementMeters, kFreeMaxPlacementMeters),
                      core::clamp(offset.y, -kFreeMaxPlacementMeters, kFreeMaxPlacementMeters),
                      core::clamp(offset.z, -kFreeMaxPlacementMeters, kFreeMaxPlacementMeters)};
            origin = profiles::airport::world(drone, offset * -1);
            position = profiles::airport::local(origin, drone);
        }
    }
    const auto q = orientation.normalized();
    return {{"worldOrigin", profiles::airport::asJson(origin)},
            {"positionMetersFRD", {position.x, position.y, position.z}},
            {"orientationCameraToAnchorXYZW", {q.x, q.y, q.z, q.w}},
            {"fovRad", core::clamp(std::isfinite(fovRad) ? fovRad : kDroneDefaultFovRad, core::kMinFovRad, core::kMaxFovRad)}};
}

json Coordinator::droneHere(double now, std::string& error) const {
    const bool aircraftKnown = aircraftPositionFresh(now);
    const CameraEntry* cam = activeEntry();
    profiles::airport::Geo drone{NAN, NAN, NAN};
    core::Quat orientation;
    double fov = kDroneDefaultFovRad;
    std::string source, message;
    if (spot_ && spot_->overlay && state_ == Ownership::Active && lastSubmitted_ && lastSubmitted_->referential == Referential::World) {
        // A chase, fly-by, computed or position spot view on screen (with or without a profile view underneath).
        const SdkPose& sdk = *lastSubmitted_;
        drone = {sdk.x, sdk.y, sdk.z};
        orientation = core::Quat::fromPbh({-sdk.pitchDeg, sdk.bankDeg, sdk.headingDeg});
        fov = sdk.fovRad;
        if (const auto actual = backend_.worldCameraPosition()) {
            const profiles::airport::Geo seen{actual->x, actual->y, actual->z};
            if (seen.valid() && drone.valid() && profiles::airport::local(drone, seen).length() < 50) drone = seen;
        }
        source = "spot";
        message = "Drone placed at the spotting view.";
    } else if (eligibleToSubmit() && cam && lastSubmitted_ && engine_.hasPose()) {
        // The view on screen, as last submitted (aircraft tracking, follow horizon and gimbal included).
        const SdkPose& sdk = *lastSubmitted_;
        fov = engine_.sample().pose.fovRad;
        if (sdk.referential == Referential::World) {
            drone = {sdk.x, sdk.y, sdk.z};
            orientation = core::Quat::fromPbh({-sdk.pitchDeg, sdk.bankDeg, sdk.headingDeg});
        } else {
            if (!aircraftKnown) {
                error = "Waiting for the aircraft position before placing a drone.";
                return nullptr;
            }
            // Aircraft-relative view: rotate by the aircraft attitude (FRD body axes to north, east, down).
            const auto attitude = core::Quat::fromPbh({std::isfinite(aircraftPitchDeg_) ? aircraftPitchDeg_ : 0.0,
                                                       std::isfinite(aircraftBankDeg_) ? aircraftBankDeg_ : 0.0, targetHeading_});
            const auto shown = fromSdk(sdk, qual_.contract);
            drone = profiles::airport::world(aircraftGeo(), attitude.rotate(shown.positionFRD));
            orientation = (attitude * shown.orientation).normalized();
        }
        // Prefer where the simulator actually shows the camera (terrain lift, eyepoint offset) when it agrees.
        if (const auto actual = backend_.worldCameraPosition()) {
            const profiles::airport::Geo seen{actual->x, actual->y, actual->z};
            if (seen.valid() && drone.valid() && profiles::airport::local(drone, seen).length() < 50) drone = seen;
        }
        source = "view";
        message = "Drone placed at the current view.";
    } else {
        if (!aircraftKnown) {
            error = "Load a flight and wait for the aircraft position before placing a drone.";
            return nullptr;
        }
        // No Camera Studio view on screen: a chase position behind and above the aircraft, looking at it.
        constexpr double kBehind = 40, kAbove = 12;
        drone = profiles::airport::world(aircraftGeo(), core::Quat::fromPbh({0, 0, targetHeading_}).rotate({-kBehind, 0, -kAbove}));
        orientation = core::Quat::fromPbh({-std::atan2(kAbove, kBehind) * core::kRadToDeg, 0, targetHeading_});
        source = "behindAircraft";
        message = "No Camera Studio view was on screen: drone placed behind and above the aircraft.";
    }
    if (!drone.valid()) {
        error = "The current view has no usable world position.";
        return nullptr;
    }
    json out = dronePlacement(drone, orientation, fov);
    out["world"] = {{"latitude", drone.lat}, {"longitude", drone.lon}, {"altitudeMeters", drone.altitude}};
    out["source"] = source;
    out["message"] = message;
    return out;
}

json Coordinator::bakeFreeCamera(const protocol::Command& c, const CameraEntry& cam, const core::CameraPose& shown, double now) {
    // Save adjusted view: the new world origin and pose of where the drone is now, re-anchored at the aircraft.
    const auto drone = freeWorld(cam, shown.positionFRD, now);
    if (!drone.valid()) return ack(c, AckStatus::Rejected, "Waiting for the aircraft position before saving this drone.");
    const json placed = dronePlacement(drone, shown.orientation, shown.fovRad);
    const auto& position = placed["positionMetersFRD"];
    const core::CameraPose saved{{position[0].get<double>(), position[1].get<double>(), position[2].get<double>()},
                                 shown.orientation.normalized(), placed["fovRad"].get<double>()};
    json payload = poseJson(saved);
    payload["worldOrigin"] = placed["worldOrigin"];
    payload["cameraId"] = cam.id;
    engine_.recenterLook();
    engine_.place(saved);
    resetFreeMotion();
    // Until the profile carries the new origin, a world-fixed drone is already shown about it (no jump).
    if (!cam.followAircraft) freeSavedOrigin_ = profiles::airport::geo(placed["worldOrigin"]);
    emit_("TemporaryLookBaked", payload);
    emitLook(now, true);
    return ack(c, AckStatus::Completed);
}

// ---------------------------------------------------------------------------------------------------------------
// Spotting (traffic cameras)
//
// Tower and runway views recall the loaded airport's own views (saved or temporary) and aim them at the target when
// the profile can be used; otherwise, and wherever the profile has no such view near the target, they are computed
// from scenery data (a runway-end spot, the tower) like chase and fly-by. Track aims the world view on screen. Chase,
// fly-by and the computed views are overlays: a computed world pose submitted instead of the active camera, which
// stays underneath as the base so the return works as for any view; without a usable profile there is no base and
// control is taken with nothing underneath. Every pose is computed each frame from the traffic model, which follows
// the target's per-frame data while it is fresh, so a camera tied to the target moves exactly with it.

namespace {
profiles::airport::Geo airportCameraWorld(const CameraEntry& cam, const core::CameraPose& pose) {
    return profiles::airport::world({cam.originLat, cam.originLon, cam.originAltitude}, pose.positionFRD);
}
// Where a position spot puts the camera: its position, never below the terrain under the target plus clearance.
profiles::airport::Geo positionSpotAt(const traffic::PositionSpot& place, const traffic::Pose& target) {
    auto at = place.position;
    const double floor = traffic::terrainFloor(target.groundAltitude, NAN);
    if (std::isfinite(floor) && at.valid()) at.altitude = std::max(at.altitude, floor);
    return at;
}
bool positionSpotInRange(const traffic::PositionSpot& place, const traffic::Pose& target) {
    const auto d = profiles::airport::local(place.position, target.position);
    return std::hypot(d.x, d.y) <= traffic::kSpotAirportRangeMeters;
}
constexpr const char* kSpotTooFar = "That spot is more than 20 km from this aircraft. Choose a spot near it.";
}  // namespace

bool Coordinator::spotAims(const CameraEntry& cam) const {
    return spot_ && !spot_->overlay && cam.id == spot_->cameraId && spot_->target.valid();
}

const CameraEntry* Coordinator::spotCamera(traffic::SpotView view, const traffic::Pose& target) const {
    if (!profile_ || !target.valid() || (view != traffic::SpotView::Tower && view != traffic::SpotView::Runway)) return nullptr;
    // Generated airport views carry their height category: "tower", and "runways" for runway and spotting views.
    const std::string category = view == traffic::SpotView::Tower ? "tower" : "runways";
    const CameraEntry* best = nullptr;
    double bestDistance = std::numeric_limits<double>::infinity();
    for (const auto& cam : profile_->cameras) {
        if (cam.mode != profiles::CameraMode::Airport || cam.airportHeightCategory != category || !profiles::modeActivatable(cam.mode)) continue;
        const double d = profiles::airport::local(airportCameraWorld(cam, effectiveAirportPose(cam)), target.position).length();
        if (d < bestDistance) { bestDistance = d; best = &cam; }
    }
    return best;
}

// A usable profile airport view of this kind within `range` of the target.
bool Coordinator::profileSpotCamera(traffic::SpotView view, const traffic::Pose& target, double range) const {
    if (!canActivate()) return false;
    const auto* cam = spotCamera(view, target);
    return cam && profiles::airport::local(airportCameraWorld(*cam, effectiveAirportPose(*cam)), target.position).length() <= range;
}

double Coordinator::runwayCameraHeight() const {
    const auto it = settings_.airportCategoryHeights.find("runways");
    return it != settings_.airportCategoryHeights.end() && std::isfinite(it->second) ? core::clamp(it->second, 0.0, 1000.0) : 3.0;
}

double Coordinator::towerCameraHeight() const {
    const auto it = settings_.airportCategoryHeights.find("tower");
    return it != settings_.airportCategoryHeights.end() && std::isfinite(it->second) ? core::clamp(it->second, 0.0, 1000.0) : 35.0;
}

json Coordinator::spotViewsJson() const {
    bool tower = false, runway = false;
    for (const auto& a : spotAirports_) {
        tower = tower || a.tower.valid();
        runway = runway || !a.ends.empty();
    }
    if (canActivate() && profile_)
        for (const auto& cam : profile_->cameras) {
            if (cam.mode != profiles::CameraMode::Airport) continue;
            tower = tower || cam.airportHeightCategory == "tower";
            runway = runway || cam.airportHeightCategory == "runways";
        }
    return {{"tower", tower}, {"runway", runway}};
}

bool Coordinator::spotViewAvailable(traffic::SpotView view, std::uint32_t objectId, double now) const {
    const auto target = traffic_.predict(objectId, now);
    if (!target.valid()) return false;
    switch (view) {
        case traffic::SpotView::Tower:
            return (canActivate() && spotCamera(view, target)) || traffic::pickTower(spotAirports_, target);
        case traffic::SpotView::Runway: {
            const auto* e = traffic_.find(objectId);
            return (canActivate() && spotCamera(view, target)) ||
                   traffic::pickRunwayEnd(spotAirports_, target, e ? e->status : traffic::Status::Cruise);
        }
        case traffic::SpotView::Chase:
        case traffic::SpotView::Flyby: return true;
        case traffic::SpotView::Spot: return false;  // needs a position spot (SpotTarget.spot)
        case traffic::SpotView::Track: {
            if (state_ != Ownership::Active) return false;
            if (spot_ && spot_->overlay) return spot_->position.valid();
            const auto* cam = canActivate() ? activeEntry() : nullptr;
            return cam && (cam->mode == profiles::CameraMode::Airport || cam->mode == profiles::CameraMode::Free);
        }
    }
    return false;
}

std::optional<profiles::airport::Geo> Coordinator::spotCameraPosition() const {
    if (!spot_ || !spot_->overlay || !spot_->position.valid()) return std::nullopt;
    if (lastSubmitted_ && lastSubmitted_->referential == Referential::World)
        return profiles::airport::Geo{lastSubmitted_->x, lastSubmitted_->y, lastSubmitted_->z};
    return spot_->position;
}

std::string Coordinator::spotActivate(const std::string& cameraId, double now) {
    spotActivating_ = true;
    const auto error = activate(cameraId, now, false);
    spotActivating_ = false;
    return error;
}

std::string Coordinator::spotAcquire(double now) {
    switch (state_) {
        case Ownership::Active:
        case Ownership::SimulatorSuspended:
        case Ownership::Acquiring: return {};
        case Ownership::Releasing: return "Camera Studio is changing camera control. Try again in a moment.";
        case Ownership::UserDisabled: return "Camera control by add-ons is disabled in the simulator.";
        case Ownership::Observing:
        case Ownership::Faulted:
        case Ownership::OtherClient:
            if (cleanupPending_) return "Camera Studio is still withdrawing an earlier request. Try again in a moment.";
            ++recalls_;
            glance_.reset(); glanceReturn_.reset();
            stopWalkaround();
            seedName_.clear();
            pendingCameraId_.clear();  // nothing underneath: the overlay is the whole view
            beginAcquire(now);
            return state_ == Ownership::Acquiring ? std::string() : stateReason_;
    }
    return "unexpected state";
}

void Coordinator::detachSpotBase() {
    if (!spot_) return;
    spot_->returnCameraId.clear();
    pendingCameraId_.clear();
    if (activeCameraId_.empty()) return;
    if (!lookOwner_.empty()) rememberedLooks_[lookOwner_] = engine_.look();
    lookOwner_.clear();
    cancelMouseDrag();
    peek_.reset();
    glance_.reset(); glanceReturn_.reset();
    stopWalkaround();
    activeCameraId_.clear();
    engine_.reset(); effects_.reset();
    emitActiveCamera();
}

std::string Coordinator::spotTarget(std::uint32_t objectId, traffic::SpotView view, double now, const std::optional<traffic::RunwaySpot>& runway,
                                    const std::optional<traffic::PositionSpot>& place) {
    if (const auto block = spottingBlockReason(); !block.empty()) return block;
    if (state_ == Ownership::Releasing || cleanupPending_) return "Camera Studio is changing camera control. Try again in a moment.";
    if (!seedName_.empty()) return "Camera Studio is busy; try again in a moment.";
    traffic_.prune(now);
    const auto target = traffic_.predict(objectId, now);
    const auto* entry = traffic_.find(objectId);
    if (!entry || !target.valid()) return "That aircraft is no longer in range.";
    const bool profileOk = canActivate();
    const bool shown = (state_ == Ownership::Active || state_ == Ownership::SimulatorSuspended) && !activeCameraId_.empty();
    Spot next;
    next.targetId = objectId;
    next.view = view;
    next.label = traffic::label(entry->last);
    next.target = target;
    // The view to return to is the one shown before spotting began (kept when the target or view changes); a glance
    // returns to the view it was taken from.
    next.returnCameraId = spot_ ? spot_->returnCameraId : glanceCurrent() ? glance_->returnCameraId : shown ? activeCameraId_ : std::string();
    std::string cameraId;
    // Computed views stand at a fixed place and zoom on the target, like fly-by.
    const auto computed = [&](const profiles::airport::Geo& at) {
        next.overlay = true;
        next.position = at;
        next.fovRad = NAN;
    };
    switch (view) {
        case traffic::SpotView::Tower: {
            const auto* tower = traffic::pickTower(spotAirports_, target);
            if (profileSpotCamera(view, target, traffic::kSpotAirportRangeMeters) || (!tower && profileOk && spotCamera(view, target))) {
                cameraId = spotCamera(view, target)->id;
            } else if (tower) {
                computed(traffic::towerPosition(*tower, towerCameraHeight()));
            } else {
                return "No tower position is known for an airport near this aircraft yet.";
            }
            break;
        }
        case traffic::SpotView::Runway: {
            bool aligned = false;
            const auto* end = runway ? traffic::findRunwayEnd(spotAirports_, runway->icao, runway->runwayEnd)
                                     : traffic::pickRunwayEnd(spotAirports_, target, entry->status, &aligned);
            if (runway && !end) return "That runway end is not known. Wait for the airport's scenery data, or choose another end.";
            if (!runway && (profileSpotCamera(view, target, traffic::kSpotAirportRangeMeters) || (!end && profileOk && spotCamera(view, target)))) {
                cameraId = spotCamera(view, target)->id;
            } else if (end) {
                next.runway = runway ? *runway : traffic::runwaySpot(*end, "fence", runwayCameraHeight());
                next.pinned = runway.has_value();
                computed(traffic::spotPosition(*end, *next.runway));
            } else {
                return "No runway data is known for an airport near this aircraft yet.";
            }
            break;
        }
        case traffic::SpotView::Track: {
            const auto* cam = state_ == Ownership::Active && profileOk ? activeEntry() : nullptr;
            if (spot_ && spot_->overlay && state_ == Ownership::Active && spot_->position.valid()) {
                // Over a chase, fly-by or computed view: stay where that camera is and keep aiming.
                next.overlay = next.fixed = true;
                next.position = spotCameraPosition().value_or(spot_->position);
                next.fovRad = lastSubmitted_ ? lastSubmitted_->fovRad : spot_->fovRad;
            } else if (cam && !(spot_ && spot_->overlay) &&
                       (cam->mode == profiles::CameraMode::Airport || cam->mode == profiles::CameraMode::Free)) {
                cameraId = cam->id;
            } else if (!profileOk) {
                return "Track aims an airport or free camera from your camera profile. Without one, use Chase, Fly-by, Tower or Runway.";
            } else {
                return "Track needs an airport or free camera on screen.";
            }
            break;
        }
        case traffic::SpotView::Spot: {
            // A saved position: stands there and zooms on the target, like the computed runway views.
            if (!place || !place->position.valid()) return "The spot view needs a position spot. Save the current view as a spot first.";
            if (!positionSpotInRange(*place, target)) return kSpotTooFar;
            next.place = place;
            next.pinned = true;
            computed(positionSpotAt(*place, target));
            break;
        }
        case traffic::SpotView::Chase:
        case traffic::SpotView::Flyby: next.overlay = true; break;
    }
    if (next.overlay && spot_ && spot_->overlay) next.aboveGround = spot_->aboveGround;
    if (next.overlay) {
        // The view underneath: the one on screen (or about to be), else the profile's default view to take control,
        // else nothing (no usable profile).
        const bool claimed = shown || spotBare() || state_ == Ownership::Acquiring;
        if (!claimed) {
            std::string error = profileOk && profile_->camera(profile_->defaultCameraId) ? spotActivate(profile_->defaultCameraId, now) : "none";
            if (!error.empty()) error = spotAcquire(now);
            if (!error.empty()) return error;
        }
    } else if (!(shown && activeCameraId_ == cameraId) && !(state_ == Ownership::Acquiring && pendingCameraId_ == cameraId)) {
        if (const auto error = spotActivate(cameraId, now); !error.empty()) return error;
    }
    // A glance, walkaround or peek would recall another view over the target.
    glance_.reset(); glanceReturn_.reset();
    stopWalkaround();
    peek_.reset();
    next.cameraId = cameraId;
    next.cameraSince = next.runwayCheckAt = now;
    if (spot_ && spot_->targetId == objectId && spot_->view == view && view == traffic::SpotView::Chase) {
        next.chaseOffset = spot_->chaseOffset;  // the same chase again: no restart
        next.chaseSmoothed = spot_->chaseSmoothed;
        next.chaseRate = spot_->chaseRate;
    }
    cancelMouseDrag();
    engine_.freeze();
    spot_ = std::move(next);
    spotReason_.clear();
    lastSubmitted_.reset();
    airportAutoZoomFov_.reset();
    if (spot_->overlay && !spot_->fixed) placeSpotOverlay(0, now);
    emitActiveCamera();
    return {};
}

std::string Coordinator::adjustSpot(const std::optional<traffic::RunwaySpot>& runway, const std::optional<traffic::PositionSpot>& place, double now) {
    const char* notPinned = "Not watching from a pinned spot. Choose Watch from here first.";
    if (!spot_ || !spot_->pinned || !spot_->overlay) return notPinned;
    if (spot_->view == traffic::SpotView::Runway && spot_->runway) {
        if (!runway) return "This view shows a runway spot. Adjust it with a runway spot.";
        const auto* end = traffic::findRunwayEnd(spotAirports_, runway->icao, runway->runwayEnd);
        if (!end) return "That runway end is not known. Wait for the airport's scenery data, or choose another end.";
        // The same end moves the camera and keeps the zoom easing; another end is a cut.
        const bool cut = runway->icao != spot_->runway->icao || runway->runwayEnd != spot_->runway->runwayEnd;
        spot_->runway = *runway;
        spot_->position = traffic::spotPosition(*end, *runway);
        if (cut) {
            spot_->fovRad = NAN;
            spot_->cameraSince = now;
        }
        return {};
    }
    if (spot_->view == traffic::SpotView::Spot && spot_->place) {
        if (!place || !place->position.valid()) return "This view shows a position spot. Adjust it with a position spot.";
        if (!positionSpotInRange(*place, spot_->target)) return kSpotTooFar;
        spot_->place = place;
        spot_->position = positionSpotAt(*place, spot_->target);
        return {};
    }
    return notPinned;
}

void Coordinator::stopSpotting(const std::string& reason, double now) {
    if (!spot_) return;
    const std::string back = spot_->returnCameraId;
    endSpot(reason, false, false);
    const bool controlled = state_ == Ownership::Active || state_ == Ownership::SimulatorSuspended || state_ == Ownership::Acquiring;
    if (!controlled) return;
    spotActivating_ = true;  // returning is not a manual recall, and releasing here is not a loss of control
    const std::string error = !back.empty() && profile_ && profile_->camera(back) ? activate(back, now, false) : std::string("none");
    if (!error.empty()) release("Camera returned to the simulator", now);
    spotActivating_ = false;
}

void Coordinator::endSpotting(const std::string& reason, double now) {
    const bool bare = spotBare();
    endSpot(reason, false, false);
    if (!bare || (state_ != Ownership::Active && state_ != Ownership::SimulatorSuspended && state_ != Ownership::Acquiring)) return;
    // Nothing underneath: the simulator's camera, until the other mode recalls a view.
    spotActivating_ = true;
    release("Camera returned to the simulator", now);
    spotActivating_ = false;
}

void Coordinator::endSpot(const std::string& reason, bool manual, bool released) {
    if (!spot_) {
        // Auto-spot waiting for a target: another recall or a loss of control turns it off too.
        if (autoSpot_ && (manual || released)) emit_("SpotStopped", {{"reason", reason}, {"manual", manual}, {"released", released}});
        return;
    }
    const bool aboveGround = spot_->aboveGround;
    cancelMouseDrag();
    spot_.reset();
    spotReason_ = reason;
    lastSubmitted_.reset();  // the view underneath is shown again
    airportAutoZoomFov_.reset();
    if (aboveGround && state_ == Ownership::Active) {
        const auto* cam = activeEntry();
        backend_.setFlag(CameraFlag::AboveGround, cam && (cam->mode == profiles::CameraMode::Airport || cam->mode == profiles::CameraMode::Free));
        backend_.setGroundBase(groundBaseFor(cam));
    }
    emit_("SpotStopped", {{"reason", reason}, {"manual", manual}, {"released", released}});
    emitActiveCamera();
}

void Coordinator::tickBareSpot(double dt, double now) {
    // Control taken for spotting without a camera profile: only the overlay is shown.
    updateSpot(dt, now);  // may release (a target lost for good)
    if (!spot_ || state_ != Ownership::Active) return;
    headTracking_.update("spot", "off", now);
    emitCapabilities();
    updateBinoculars(dt);
    if (spot_->overlay) submitSpotOverlay();
    if (engine_.transitioning() != lastTransitioning_) emitActiveCamera();
}

void Coordinator::updateSpot(double dt, double now) {
    if (!spot_) return;
    // A target that leaves the picture is lost: the view holds where it was aiming, then spotting stops (with auto-spot
    // on, the host picks the next aircraft instead). Coming back in time continues.
    const auto pose = traffic_.predict(spot_->targetId, now);
    if (pose.valid()) {
        spot_->target = pose;
        spot_->lost = false;
    } else if (!spot_->lost) {
        spot_->lost = true;
        spot_->lostAt = now;
    }
    if (spot_->lost && !autoSpot_ && now - spot_->lostAt >= kSpotLostHoldSeconds) {
        stopSpotting("The aircraft is no longer in range.", now);
        return;
    }
    // Runway view: follow the target to the best runway end or runway view, with a margin and a minimum dwell.
    if (spot_->view == traffic::SpotView::Runway && !spot_->lost && !spot_->pinned && !spot_->manuallyAdjusted && now >= spot_->runwayCheckAt) {
        spot_->runwayCheckAt = now + kSpotRunwayRecheckSeconds;
        if (spot_->runway) {
            const auto* current = traffic::findRunwayEnd(spotAirports_, spot_->runway->icao, spot_->runway->runwayEnd);
            const auto* e = traffic_.find(spot_->targetId);
            bool aligned = false;
            const auto* best = traffic::pickRunwayEnd(spotAirports_, spot_->target, e ? e->status : traffic::Status::Cruise, &aligned);
            if (best && best != current && now - spot_->cameraSince >= kSpotRunwayDwellSeconds) {
                auto candidate = traffic::runwaySpot(*best, spot_->runway->kind == "custom" ? "fence" : spot_->runway->kind, runwayCameraHeight());
                const auto at = traffic::spotPosition(*best, candidate);
                const double d = current ? profiles::airport::local(spot_->position, spot_->target.position).length() : INFINITY;
                // The end the aircraft is lined up with wins; the nearest end only by a clear margin.
                if (aligned || !current || profiles::airport::local(at, spot_->target.position).length() + std::max(10.0, d * .15) < d) {
                    spot_->runway = std::move(candidate);
                    spot_->position = at;
                    spot_->fovRad = NAN;  // a cut: frame the target afresh
                    spot_->cameraSince = now;
                    lastSubmitted_.reset();
                }
            }
        } else if (!spot_->overlay) {
            const auto* current = profile_ ? profile_->camera(spot_->cameraId) : nullptr;
            const auto* nearest = spotCamera(traffic::SpotView::Runway, spot_->target);
            if (current && nearest && nearest->id != current->id && now - spot_->cameraSince >= kSpotRunwayDwellSeconds) {
                const auto distance = [&](const CameraEntry& c) {
                    return profiles::airport::local(airportCameraWorld(c, effectiveAirportPose(c)), spot_->target.position).length();
                };
                const double d = distance(*current);
                const std::string id = nearest->id;
                if (distance(*nearest) + std::max(10.0, d * .15) < d && spotActivate(id, now).empty() && spot_) {
                    spot_->cameraId = id;
                    spot_->cameraSince = now;
                }
            }
        }
    }
    if (spot_ && spot_->overlay) placeSpotOverlay(dt, now);
}

void Coordinator::placeSpotOverlay(double dt, double now) {
    (void)now;
    auto& s = *spot_;
    const auto& t = s.target;
    if (!t.valid()) return;
    const bool hold = s.lost || motionPaused_;  // hold the last view
    // Never below the terrain under the target (its own per-frame GROUND ALTITUDE when watched). Terrain near the user
    // aircraft is not used: it changes with the user's position, not the target's, and would move the camera.
    const double floor = traffic::terrainFloor(t.groundAltitude, NAN);
    if (s.view == traffic::SpotView::Chase && !s.fixed) {
        // Behind and above along the track (the heading when slow); the offset eases round turns, so the chase swings
        // rather than snaps, and a critically damped filter keeps the remaining per-frame changes from shaking it. The
        // camera is the target plus that offset, so the target itself stays steady in frame.
        if (!hold || !s.chaseOffset.finite()) {
            s.chaseOffset = traffic::follow(s.chaseOffset, traffic::chaseOffset(t), dt);
            auto wanted = s.chaseOffset;
            if (std::isfinite(floor)) wanted.z = std::min(wanted.z, t.position.altitude - floor);
            s.chaseSmoothed = traffic::smoothDamp(s.chaseSmoothed, s.chaseRate, wanted, dt);
        }
        s.position = profiles::airport::world(t.position, s.chaseSmoothed);
        if (std::isfinite(floor) && s.position.valid()) s.position.altitude = std::max(s.position.altitude, floor);
        s.fovRad = traffic::kChaseFovDeg * core::kDegToRad;
        return;
    }
    if (s.view == traffic::SpotView::Flyby && !s.fixed) {
        // Stationary ahead of the target; placed again once it has passed and is well beyond. The terrain floor applies
        // when it is placed only, so the camera never moves while the target comes past.
        if (!s.position.valid() || (!hold && !s.manuallyAdjusted && traffic::flybyPassed(s.position, t))) {
            s.position = traffic::flybyPosition(t);
            if (std::isfinite(floor) && s.position.valid()) s.position.altitude = std::max(s.position.altitude, floor);
            s.fovRad = NAN;  // a cut: frame the target afresh
        }
    }
    // Fly-by, the computed runway and tower views and a held track zoom on the target with the airport auto zoom settings.
    const double distance = profiles::airport::local(s.position, t.position).length();
    if (!std::isfinite(distance)) return;
    const double minimum = core::clamp(settings_.airportAutoZoomMinFovDeg, 2., 120.) * core::kDegToRad;
    const double maximum = core::clamp(settings_.airportAutoZoomMaxFovDeg, minimum * core::kRadToDeg, 120.) * core::kDegToRad;
    const double frame = core::clamp(settings_.airportAutoZoomFrameMeters, 5., 200.);
    const double response = core::clamp(settings_.airportAutoZoomResponseSeconds, .1, 5.);
    const double wanted = traffic::autoZoomFov(std::max(distance, .5), frame, minimum, maximum);
    if (!std::isfinite(s.fovRad)) s.fovRad = wanted;
    else if (!motionPaused_ && dt > 0) s.fovRad = traffic::easeFov(core::clamp(s.fovRad, minimum, maximum), wanted, std::min(dt, .1), response);
}

bool Coordinator::manualCameraReady() const {
    return submitReady() && (spot_.has_value() || !activeCameraId_.empty());
}

void Coordinator::adjustSpot(double heading, double pitch, const core::Vec3& movement, double zoom) {
    if (!spot_ || !manualCameraReady() || !lastSubmitted_ || lastSubmitted_->referential != Referential::World) return;
    if (!std::isfinite(heading) || !std::isfinite(pitch) || !movement.finite() || !std::isfinite(zoom)) return;
    if (heading == 0 && pitch == 0 && movement.length() == 0 && zoom == 0) return;
    auto& s = *spot_;
    const auto& shown = *lastSubmitted_;
    // Forward/right follow the visible view; vertical movement stays world vertical.
    const auto orientation = core::Quat::fromPbh({-shown.pitchDeg, shown.bankDeg, shown.headingDeg});
    s.manualOffset += orientation.rotate({movement.x, movement.y, 0}) + core::Vec3{0, 0, movement.z};
    const double distance = s.manualOffset.length();
    if (distance > 5000) s.manualOffset = s.manualOffset * (5000 / distance);
    s.manualLook.headingDeg = core::wrapDeg(s.manualLook.headingDeg + heading);
    s.manualLook.pitchDeg += core::clamp(-shown.pitchDeg + pitch, -89., 89.) + shown.pitchDeg;
    s.manualLook.pitchDeg = core::clamp(s.manualLook.pitchDeg, -178., 178.);
    s.manualLook.fovDeltaRad = core::clamp(s.manualLook.fovDeltaRad + zoom, -core::kMaxFovRad, core::kMaxFovRad);
    if (!s.manuallyAdjusted) {
        s.manuallyAdjusted = true;
        emit_("SpotAdjusted", json::object());  // hold the chosen target while the user frames it
    }
}

void Coordinator::updateSpotManual(double dt) {
    if (!spot_ || !submitReady()) return;
    auto& s = *spot_;
    if (mouseBlocked_ || motionPaused_ || !(dt > 0) || dt > core::ManualMover::kMaxGapSeconds) {
        s.manualMover.neutralize();
        mouseHeadingPending_ = mousePitchPending_ = 0;
        return;
    }
    s.manualMover.setSpeeds({settings_.lookSpeedDegPerSec * binocularsLookScale_,
                            settings_.moveSpeedMetersPerSec * settings_.movementPrecisionScale, .5, 6.});
    core::TemporaryLook delta;
    s.manualMover.advance(dt, manual_, delta);
    if (mouseHeld_ && mouseCameraId_ == activeCameraId_) {
        const double blend = settings_.mouseSmoothingMs <= 0 ? 1. : -std::expm1(-dt / (settings_.mouseSmoothingMs / 1000.));
        const double heading = mouseHeadingPending_ * blend, pitch = mousePitchPending_ * blend;
        delta.headingDeg += heading; delta.pitchDeg += pitch;
        mouseHeadingPending_ -= heading; mousePitchPending_ -= pitch;
        ++mouseFrames_;
    }
    adjustSpot(delta.headingDeg, delta.pitchDeg, delta.translationHeadingFrame, delta.fovDeltaRad);
}

void Coordinator::applySpotAdjustments(SdkPose& sdk) {
    if (!spot_ || !spot_->manuallyAdjusted || sdk.referential != Referential::World) return;
    const auto& s = *spot_;
    auto position = profiles::airport::world({sdk.x, sdk.y, sdk.z}, s.manualOffset);
    const double floor = traffic::terrainFloor(s.target.groundAltitude, NAN);
    if (std::isfinite(floor)) position.altitude = std::max(position.altitude, floor);
    sdk.x = position.lat; sdk.y = position.lon; sdk.z = position.altitude;
    if (const auto look = traffic::aim(position, s.target.position)) {
        sdk.headingDeg = look->headingDeg;
        sdk.pitchDeg = -look->pitchDeg;
    }
    sdk.headingDeg = core::wrapDeg(sdk.headingDeg + s.manualLook.headingDeg);
    sdk.pitchDeg = -core::clamp(-sdk.pitchDeg + s.manualLook.pitchDeg, -89., 89.);
    sdk.fovRad = core::clamp(sdk.fovRad + s.manualLook.fovDeltaRad, core::kMinFovRad, core::kMaxFovRad);
}

void Coordinator::recenterSpot() {
    if (!spot_) return;
    cancelMouseDrag();
    spot_->manualOffset = {};
    spot_->manualLook = {};
    spot_->manualMover.neutralize();
    spot_->manuallyAdjusted = false;
}

void Coordinator::submitSpotOverlay() {
    auto& s = *spot_;
    if (!s.position.valid() || !s.target.valid()) return;
    if (!s.aboveGround) {
        backend_.setFlag(CameraFlag::AboveGround, true);
        backend_.setGroundBase(NAN);
        s.aboveGround = true;
    }
    SdkPose sdk;
    sdk.referential = Referential::World;
    sdk.trackUserAircraft = false;
    sdk.x = s.position.lat;
    sdk.y = s.position.lon;
    sdk.z = s.position.altitude;
    // Look at the target; directly on top of it, keep the last direction.
    const auto look = traffic::aim(s.position, s.target.position);
    sdk.headingDeg = look ? look->headingDeg : lastSubmitted_ ? lastSubmitted_->headingDeg : s.target.headingDeg;
    sdk.pitchDeg = look ? -look->pitchDeg : lastSubmitted_ ? lastSubmitted_->pitchDeg : 0.0;
    sdk.bankDeg = 0;
    const double fov = std::isfinite(s.fovRad) ? s.fovRad : traffic::kChaseFovDeg * core::kDegToRad;
    sdk.fovRad = fov;
    applySpotAdjustments(sdk);
    sdk.fovRad = binocularsFov(sdk.fovRad, sdk.fovRad);
    if (lastSubmitted_ && nearlyEqual(*lastSubmitted_, sdk)) return;
    if (backend_.submitPose(sdk)) {
        lastSubmitted_ = sdk;
        submittedHorizonRollDeg_ = 0;
        ++submissions_;
    }
}

}  // namespace fscs::host
