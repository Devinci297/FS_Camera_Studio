#include "xp_camera.hpp"

#include "xp_geometry.hpp"

#include <XPLMGraphics.h>

#include <cmath>

namespace fscs::xplane {

using host::BackendEvent;
using host::Referential;
using host::SdkAvailability;
using host::SdkPose;
namespace be = host::backend_event;

namespace {

// The module's poses use the shipped MSFS SDK mapping (host::qualificationFor); invert exactly that.
const host::AdapterContract kContract{};

// Frames to wait after switching X-Plane's view ourselves before a view change counts as the user's.
constexpr int kSettleFrames = 5;
// Lowest a world camera goes above the terrain under it while the above-ground flag is set (meters).
constexpr double kGroundClearance = 1.0;

const char* const kHeadRefs[6] = {"sim/graphics/view/pilots_head_x",   "sim/graphics/view/pilots_head_y",
                                  "sim/graphics/view/pilots_head_z",   "sim/graphics/view/pilots_head_psi",
                                  "sim/graphics/view/pilots_head_the", "sim/graphics/view/pilots_head_phi"};
const char* const kEyeRefs[3] = {"sim/aircraft/view/acf_peX", "sim/aircraft/view/acf_peY", "sim/aircraft/view/acf_peZ"};
const char* const kLocalRefs[3] = {"sim/flightmodel/position/local_x", "sim/flightmodel/position/local_y",
                                   "sim/flightmodel/position/local_z"};
const char* const kAttitudeRefs[3] = {"sim/flightmodel/position/theta", "sim/flightmodel/position/phi",
                                      "sim/flightmodel/position/psi"};

float getf(XPLMDataRef r, float fallback = 0) { return r ? XPLMGetDataf(r) : fallback; }
double getd(XPLMDataRef r) { return r ? XPLMGetDatad(r) : 0.0; }

// Grid heading (degrees) of true north at a world point: X-Plane headings are from the OpenGL -z axis.
double convergenceAt(double lat, double lon, double alt) {
    double x0 = 0, y0 = 0, z0 = 0, x1 = 0, y1 = 0, z1 = 0;
    XPLMWorldToLocal(lat, lon, alt, &x0, &y0, &z0);
    XPLMWorldToLocal(lat + (lat < 89 ? 0.01 : -0.01), lon, alt, &x1, &y1, &z1);
    const double sign = lat < 89 ? 1.0 : -1.0;
    return std::atan2(sign * (x1 - x0), -sign * (z1 - z0)) * core::kRadToDeg;
}

}  // namespace

void XPCamera::findRefs() {
    for (int i = 0; i < 6; ++i) head_[i] = XPLMFindDataRef(kHeadRefs[i]);
    for (int i = 0; i < 3; ++i) {
        eye_[i] = XPLMFindDataRef(kEyeRefs[i]);
        local_[i] = XPLMFindDataRef(kLocalRefs[i]);
        attitude_[i] = XPLMFindDataRef(kAttitudeRefs[i]);
    }
    fov_ = XPLMFindDataRef("sim/graphics/view/field_of_view_deg");
    viewType_ref_ = XPLMFindDataRef("sim/graphics/view/view_type");
    external_ = XPLMFindDataRef("sim/graphics/view/view_is_external");
    cockpitCmd_ = XPLMFindCommand("sim/view/3d_cockpit_cmnd_look");
    chaseCmd_ = XPLMFindCommand("sim/view/chase");
    if (!probe_) probe_ = XPLMCreateProbe(xplm_ProbeY);
}

bool XPCamera::tryConnect() {
    if (open_) return true;
    findRefs();
    open_ = head_[0] && local_[0] && attitude_[0];
    if (open_) requestStatus();
    return open_;
}

void XPCamera::disconnect() {
    if (acquired_) release("");
    if (probe_) XPLMDestroyProbe(probe_);
    probe_ = nullptr;
    open_ = false;
}

void XPCamera::dispatch(const std::function<void(BackendEvent)>& sink) {
    auto events = std::move(staged_);
    staged_.clear();
    for (auto& e : events) sink(std::move(e));
}

bool XPCamera::requestStatus() {
    stage(be::CameraStatus{acquired_ ? SdkAvailability::Acquired : SdkAvailability::NotAcquired, false});
    return true;
}

bool XPCamera::acquire(const std::string&) {
    if (!open_) return false;
    if (acquired_) {
        stage(be::CameraStatus{SdkAvailability::Acquired, false});
        return true;
    }
    // Another plugin (X-Camera, a tour) drawing the camera: never fight for it.
    if (XPLMIsCameraBeingControlled(nullptr)) {
        stage(be::CameraStatus{SdkAvailability::AcquiredByOther, false});
        return true;
    }
    for (int i = 0; i < 6; ++i) saved_.head[i] = getf(head_[i]);
    saved_.fovDeg = getf(fov_, 60);
    saved_.external = external_ && XPLMGetDatai(external_) != 0;
    saved_.valid = true;
    viewType_ = viewType_ref_ ? XPLMGetDatai(viewType_ref_) : 0;
    settleFrames_ = 0;
    acquired_ = true;
    mode_ = Mode::None;
    pose_.reset();
    stage(be::CameraStatus{SdkAvailability::Acquired, false});
    return true;
}

bool XPCamera::release(const std::string&) {
    if (!acquired_) {
        stage(be::CameraStatus{SdkAvailability::NotAcquired, false});
        return true;
    }
    const bool wasControl = mode_ == Mode::Control;
    leaveControl();
    if (saved_.valid) {
        // Back to the view the user had: the cockpit with the head where it was, or the external view we used.
        if (!saved_.external && wasControl && cockpitCmd_) XPLMCommandOnce(cockpitCmd_);
        for (int i = 0; i < 6; ++i)
            if (head_[i]) XPLMSetDataf(head_[i], saved_.head[i]);
        if (fov_) XPLMSetDataf(fov_, saved_.fovDeg);
    }
    mode_ = Mode::None;
    acquired_ = false;
    pose_.reset();
    stage(be::CameraStatus{SdkAvailability::NotAcquired, false});
    return true;
}

void XPCamera::lose(SdkAvailability why) {
    leaveControl();
    if (saved_.valid && fov_) XPLMSetDataf(fov_, saved_.fovDeg);  // the user's own view keeps their zoom
    mode_ = Mode::None;
    acquired_ = false;
    pose_.reset();
    stage(be::CameraStatus{why, false});
}

void XPCamera::command(XPLMCommandRef cmd, bool settle) {
    if (!cmd) return;
    XPLMCommandOnce(cmd);
    if (settle) settleFrames_ = kSettleFrames;
}

void XPCamera::enterHead() {
    leaveControl();
    if (external_ && XPLMGetDatai(external_) != 0) command(cockpitCmd_);
    mode_ = Mode::Head;
}

void XPCamera::enterControl() {
    // X-Plane's own exterior view gives exterior sound and hides the 2-D panel; the plugin then draws the camera.
    if (!external_ || XPLMGetDatai(external_) == 0) command(chaseCmd_);
    if (saved_.valid && fov_) XPLMSetDataf(fov_, saved_.fovDeg);  // zoom is relative to the user's own FOV
    XPLMControlCamera(xplm_ControlCameraForever, &XPCamera::onCamera, this);
    mode_ = Mode::Control;
}

void XPCamera::leaveControl() {
    if (mode_ != Mode::Control) return;
    releasingControl_ = true;
    XPLMDontControlCamera();
    releasingControl_ = false;
    mode_ = Mode::None;
}

bool XPCamera::submitPose(const SdkPose& pose) {
    if (!acquired_) return false;
    pose_ = pose;
    if (pose.referential == Referential::Eyepoint) {
        if (mode_ != Mode::Head) enterHead();
        writeHead();
    } else if (mode_ != Mode::Control) {
        enterControl();
    }
    return true;
}

void XPCamera::writeHead() {
    if (!pose_ || pose_->referential != Referential::Eyepoint) return;
    const core::CameraPose p = host::fromSdk(*pose_, kContract);
    const core::Vec3 eye{getf(eye_[0]), getf(eye_[1]), getf(eye_[2])};
    const core::Vec3 body = eye + geo::frdToBody(p.positionFRD);
    const auto a = p.orientation.toPbh();
    const float values[6] = {static_cast<float>(body.x), static_cast<float>(body.y), static_cast<float>(body.z),
                             static_cast<float>(a.headingDeg), static_cast<float>(a.pitchDeg), static_cast<float>(a.bankDeg)};
    for (int i = 0; i < 6; ++i)
        if (head_[i] && std::isfinite(values[i])) XPLMSetDataf(head_[i], values[i]);
    const double fovDeg = p.fovRad * core::kRadToDeg;
    if (fov_ && std::isfinite(fovDeg) && fovDeg > 1 && fovDeg < 170 && std::abs(getf(fov_) - fovDeg) > 0.01)
        XPLMSetDataf(fov_, static_cast<float>(fovDeg));
}

void XPCamera::frame() {
    if (!acquired_) return;
    const int view = viewType_ref_ ? XPLMGetDatai(viewType_ref_) : 0;
    if (settleFrames_ > 0) {
        --settleFrames_;
        viewType_ = view;
    } else if (view != viewType_) {
        lose(SdkAvailability::NotAcquired);  // the user picked another X-Plane view
        return;
    }
    if (mode_ == Mode::Head) writeHead();
}

bool XPCamera::setFlag(host::CameraFlag flag, bool enabled) {
    if (flag == host::CameraFlag::AboveGround) aboveGround_ = enabled;
    return true;  // Interaction: the 3D cockpit stays clickable in head mode anyway
}

bool XPCamera::cameraFor(XPLMCameraPosition_t& out) {
    if (!pose_) return false;
    const geo::Aircraft a{{getd(local_[0]), getd(local_[1]), getd(local_[2])},
                          geo::attitude(getf(attitude_[0]), getf(attitude_[1]), getf(attitude_[2]))};
    const core::CameraPose p = host::fromSdk(*pose_, kContract);
    geo::GlCamera c;
    if (pose_->referential == Referential::World) {
        double x = 0, y = 0, z = 0;
        XPLMWorldToLocal(pose_->x, pose_->y, pose_->z, &x, &y, &z);
        c.gl = {x, y, z};
        double pitch = 0, heading = 0;
        if (pose_->trackUserAircraft && geo::aim(c.gl, a.gl, pitch, heading)) {
            c.pitchDeg = pitch;
            c.headingDeg = heading;
        } else {
            c = geo::fromGrid(c.gl, geo::trueToGrid(p.orientation, convergenceAt(pose_->x, pose_->y, pose_->z)));
        }
    } else {
        core::Vec3 offset = p.positionFRD;
        if (pose_->referential == Referential::Eyepoint)
            offset = offset + geo::bodyToFrd({getf(eye_[0]), getf(eye_[1]), getf(eye_[2])});
        c = geo::relativeToGl(a, offset, p.orientation);
    }
    if (aboveGround_ && probe_) {
        XPLMProbeInfo_t info{};
        info.structSize = sizeof(info);
        if (XPLMProbeTerrainXYZ(probe_, static_cast<float>(c.gl.x), static_cast<float>(c.gl.y), static_cast<float>(c.gl.z), &info) ==
                xplm_ProbeHitTerrain &&
            c.gl.y < info.locationY + kGroundClearance)
            c.gl.y = info.locationY + kGroundClearance;
    }
    if (!c.gl.finite() || !std::isfinite(c.pitchDeg) || !std::isfinite(c.headingDeg) || !std::isfinite(c.rollDeg)) return false;
    out.x = static_cast<float>(c.gl.x);
    out.y = static_cast<float>(c.gl.y);
    out.z = static_cast<float>(c.gl.z);
    out.pitch = static_cast<float>(c.pitchDeg);
    out.heading = static_cast<float>(c.headingDeg);
    out.roll = static_cast<float>(c.rollDeg);
    out.zoom = static_cast<float>(geo::zoomFor(p.fovRad, saved_.valid ? saved_.fovDeg : getf(fov_, 60)));
    return true;
}

int XPCamera::onCamera(XPLMCameraPosition_t* out, int losing, void* refcon) {
    auto* self = static_cast<XPCamera*>(refcon);
    if (losing) {
        // Another plugin forcibly took the camera (our own XPLMDontControlCamera does not call back).
        if (self && !self->releasingControl_ && self->mode_ == Mode::Control) {
            self->mode_ = Mode::None;
            if (self->acquired_) self->lose(SdkAvailability::AcquiredByOther);
        }
        return 0;
    }
    if (!self || !out) return 0;
    if (self->cameraFor(*out)) return 1;
    XPLMReadCameraPosition(out);  // nothing new to show: keep the current picture
    return 1;
}

std::optional<SdkPose> XPCamera::currentPose(Referential ref) {
    XPLMCameraPosition_t cp{};
    XPLMReadCameraPosition(&cp);
    const geo::GlCamera c{{cp.x, cp.y, cp.z}, cp.pitch, cp.heading, cp.roll};
    if (!c.gl.finite()) return std::nullopt;
    const double fov = geo::fovFor(cp.zoom, getf(fov_, 60));
    core::CameraPose pose;
    pose.fovRad = fov;
    SdkPose sdk;
    if (ref == Referential::World) {
        double lat = 0, lon = 0, alt = 0;
        XPLMLocalToWorld(c.gl.x, c.gl.y, c.gl.z, &lat, &lon, &alt);
        pose.orientation = geo::gridToTrue(geo::gridOrientation(c), convergenceAt(lat, lon, alt));
        sdk = host::toSdk(pose, kContract, ref);
        sdk.x = lat;
        sdk.y = lon;
        sdk.z = alt;
        return sdk;
    }
    const geo::Aircraft a{{getd(local_[0]), getd(local_[1]), getd(local_[2])},
                          geo::attitude(getf(attitude_[0]), getf(attitude_[1]), getf(attitude_[2]))};
    geo::glToRelative(a, c, pose.positionFRD, pose.orientation);
    if (ref == Referential::Eyepoint)
        pose.positionFRD = pose.positionFRD - geo::bodyToFrd({getf(eye_[0]), getf(eye_[1]), getf(eye_[2])});
    return host::toSdk(pose, kContract, ref);
}

bool XPCamera::requestCameraData(Referential ref) {
    const auto pose = currentPose(ref);
    if (!pose) return false;
    stage(be::CameraData{*pose});  // delivered on the next dispatch, like an asynchronous reply
    return true;
}

bool XPCamera::enumerateDefinitions() {
    stage(be::DefinitionList{});  // X-Plane has no named camera definitions to seed from
    return true;
}

}  // namespace fscs::xplane
