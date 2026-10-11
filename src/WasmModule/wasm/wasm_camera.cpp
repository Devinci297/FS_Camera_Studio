#include "wasm_camera.hpp"

#include <MSFS/MSFS.h>
#include <MSFS/MSFS_Camera.h>
#include <MSFS/MSFS_Vars.h>
#include <cmath>

#include <cstdlib>
#include <cstring>

namespace fscs::host {

namespace {

FsPositionReferential toFs(Referential r) {
    switch (r) {
        case Referential::Eyepoint: return FS_POSITION_REFERENTIAL_EYEPOINT;
        case Referential::SimObject: return FS_POSITION_REFERENTIAL_SIMOBJECT;
        case Referential::Datum: return FS_POSITION_REFERENTIAL_SIMOBJECT_DATUM;
        case Referential::World: return FS_POSITION_REFERENTIAL_WORLD;
    }
    return FS_POSITION_REFERENTIAL_EYEPOINT;
}

Referential fromFs(int r) {
    switch (r) {
        case FS_POSITION_REFERENTIAL_SIMOBJECT: return Referential::SimObject;
        case FS_POSITION_REFERENTIAL_SIMOBJECT_DATUM: return Referential::Datum;
        case FS_POSITION_REFERENTIAL_WORLD: return Referential::World;
        default: return Referential::Eyepoint;
    }
}

SdkAvailability fromFs(FsAcquiredState s) {
    switch (s) {
        case FS_ACQUIRED_STATE_ACQUIRED: return SdkAvailability::Acquired;
        case FS_ACQUIRED_STATE_ACQUIRED_BY_OTHER: return SdkAvailability::AcquiredByOther;
        case FS_ACQUIRED_STATE_USER_DISABLED: return SdkAvailability::UserDisabled;
        default: return SdkAvailability::NotAcquired;
    }
}

void onStatus(FsCameraEvent, const FsAcquiredState state, const bool gameControlled, void* ctx) {
    static_cast<WasmCamera*>(ctx)->stage(backend_event::CameraStatus{fromFs(state), gameControlled});
}

// The user aircraft: object id 0 is the SDK's proxy value for the user's SimObject.
constexpr FsSimObjId kUserObject = 0;

}  // namespace

bool WasmCamera::tryConnect() {
    if (subscribed_) return true;
    subscribed_ = fsCameraSubscribeToStatusUpdate(&onStatus, this);
    interactionKnown_ = false;
    if (subscribed_) requestStatus();
    return subscribed_;
}

void WasmCamera::disconnect() {
    if (!subscribed_) return;
    fsCameraUnsubscribeToStatusUpdate();
    subscribed_ = false;
}

void WasmCamera::dispatch(const std::function<void(BackendEvent)>& sink) {
    auto events = std::move(staged_);
    staged_.clear();
    for (auto& e : events) sink(std::move(e));
}

bool WasmCamera::requestStatus() {
    FsAcquiredState state = FS_ACQUIRED_STATE_NOT_ACQUIRED;
    bool game = false;
    if (!fsCameraGetStatus(&state, &game)) return false;
    stage(backend_event::CameraStatus{fromFs(state), game});
    return true;
}

bool WasmCamera::acquire(const std::string& clientId) { return fsCameraAcquire(clientId.c_str()); }

std::optional<bool> WasmCamera::headTracking() {
    if (!subscribed_) return std::nullopt;
    const auto id = fsVarsGetAVarId("TRACK IR ENABLE");
    const auto unit = fsVarsGetUnitId("Bool");
    double value = NAN;
    if (id < 0 || unit < 0 || fsVarsAVarGet(id, unit, {}, &value) != FS_VAR_ERROR_NONE || !std::isfinite(value)) return std::nullopt;
    return value != 0;
}
bool WasmCamera::setHeadTracking(bool enabled) {
    if (!subscribed_) return false;
    const auto id = fsVarsGetAVarId("TRACK IR ENABLE");
    const auto unit = fsVarsGetUnitId("Bool");
    return id >= 0 && unit >= 0 && fsVarsAVarSet(id, unit, {}, enabled ? 1.0 : 0.0) == FS_VAR_ERROR_NONE;
}

bool WasmCamera::release(const std::string& definitionName) {
    interactionKnown_ = false;
    // An empty definition returns to the previously used native camera (verified per build in qualification).
    return fsCameraRelease(definitionName.c_str());
}

bool WasmCamera::submitPose(const SdkPose& p) {
    FsCameraData d{};
    d.position = {p.x, p.y, p.z};
    d.positionReferential = toFs(p.referential);
    d.positionReferentialObjectId = kUserObject;
    d.pbh = {p.pitchDeg, p.bankDeg, p.headingDeg};
    d.rotationReferential = toFs(p.referential);
    d.rotationReferentialObjectId = kUserObject;
    d.fov = p.fovRad;
    // Explicit rotation only; the TARGETED mask is never combined with these submissions.
    return fsCameraSet(&d, FS_CAMERA_DATA_MASK_ALL_ROTATION);
}

bool WasmCamera::setFlag(CameraFlag flag, bool enabled) {
    const int f = flag == CameraFlag::Interaction ? FS_CAMERA_FLAG_INTERACTION : FS_CAMERA_FLAG_ABOVE_GROUND;
    if (flag == CameraFlag::Interaction) {
        if (interactionKnown_ && interactionOn_ == enabled) return true;
        interactionKnown_ = true;
        interactionOn_ = enabled;
    }
    return enabled ? fsCameraEnableFlag(f) : fsCameraDisableFlag(f);
}

bool WasmCamera::requestCameraData(Referential ref) {
    FsCameraData out{};
    if (!fsCameraGet(toFs(ref), &out)) return false;
    SdkPose p;
    p.x = out.position.x;
    p.y = out.position.y;
    p.z = out.position.z;
    p.pitchDeg = out.pbh.p;
    p.bankDeg = out.pbh.b;
    p.headingDeg = out.pbh.h;
    p.fovRad = out.fov;
    p.referential = fromFs(out.positionReferential);
    stage(backend_event::CameraData{p});  // delivered on the next dispatch, like an asynchronous reply
    return true;
}

bool WasmCamera::enumerateDefinitions() {
    char* list = nullptr;
    int count = 0;
    if (!fsCameraEnumerateCameraDefinitions(&list, &count)) return false;
    backend_event::DefinitionList defs;
    // The SDK allocates `count` consecutive 256-character strings; the caller frees the block.
    for (int i = 0; list && i < count && i < 512; ++i) defs.names.emplace_back(list + i * 256, strnlen(list + i * 256, 256));
    std::free(list);
    stage(std::move(defs));
    return true;
}

bool WasmCamera::setUsingDefinition(const std::string& name) { return fsCameraSetUsingCameraDefinition(name.c_str()); }

}  // namespace fscs::host
