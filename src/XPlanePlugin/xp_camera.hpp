#pragma once
// XP Camera Studio: the module's camera backend over X-Plane 12 (phase 2). The coordinator submits poses in the MSFS
// SDK's conventions (SdkPose, default adapter contract); this class puts them on screen:
//   Eyepoint  (cockpit views)   the pilot's head datarefs in X-Plane's 3D cockpit, so the cockpit stays clickable;
//                               the field of view through sim/graphics/view/field_of_view_deg
//   SimObject (exterior, orbit, mounted) and World (fly-by, airport, free, spotting)
//                               XPLMControlCamera in an external view, computed at draw time from the live aircraft
// The user picking another X-Plane view, or another plugin taking the camera, hands control back to X-Plane and is
// reported to the coordinator as the simulator taking the camera.

#include "backend.hpp"

#include <XPLMCamera.h>
#include <XPLMDataAccess.h>
#include <XPLMScenery.h>
#include <XPLMUtilities.h>

#include <cmath>
#include <optional>
#include <vector>

namespace fscs::xplane {

class XPCamera final : public host::ICameraBackend {
public:
    XPCamera() = default;
    ~XPCamera() override { disconnect(); }

    bool tryConnect() override;
    void disconnect() override;
    bool isOpen() const override { return open_; }
    void dispatch(const std::function<void(host::BackendEvent)>& sink) override;

    bool requestStatus() override;
    bool acquire(const std::string& clientId) override;
    bool release(const std::string& definitionName) override;
    bool submitPose(const host::SdkPose& pose) override;
    bool setFlag(host::CameraFlag flag, bool enabled) override;
    void setGroundBase(double baseAltitudeMeters) override { groundBase_ = baseAltitudeMeters; }
    bool requestCameraData(host::Referential ref) override;
    bool enumerateDefinitions() override;
    bool setUsingDefinition(const std::string&) override { return false; }

    // Every flight loop, after the module's update: keeps the pilot's head where the cockpit view wants it and
    // notices the user choosing another view.
    void frame();
    // The camera X-Plane is drawing now, relative to the aircraft or in the world, as the coordinator reads it.
    std::optional<host::SdkPose> currentPose(host::Referential ref);
    bool controlling() const { return mode_ == Mode::Control; }
    // A new user aircraft: its eyepoint is read again before the next cockpit view.
    void aircraftChanged() { eyeValid_ = false; }
    // The horizontal field of view on screen, degrees: the module's while it drives the camera, else X-Plane's.
    double fovDeg() const;
    bool acquired() const { return acquired_; }

    void stage(host::BackendEvent ev) { staged_.push_back(std::move(ev)); }

private:
    enum class Mode { None, Head, Control };
    struct Saved {
        float head[6] = {0, 0, 0, 0, 0, 0};
        float fovDeg = 60;
        bool external = false;
        bool valid = false;
    };

    void findRefs();
    core::Vec3 eyepoint();
    void enterHead();
    void enterControl();
    void leaveControl();
    void writeHead();
    void command(XPLMCommandRef cmd, bool settle = true);
    void lose(host::SdkAvailability why);
    bool cameraFor(XPLMCameraPosition_t& out);
    static int onCamera(XPLMCameraPosition_t* out, int losing, void* refcon);

    bool open_ = false;
    bool acquired_ = false;
    Mode mode_ = Mode::None;
    Saved saved_;
    std::optional<host::SdkPose> pose_;  // the last submitted pose
    bool aboveGround_ = false;
    // Airport cameras: their height's ground altitude, and how far the terrain under the camera lifts it above that.
    double groundBase_ = NAN, lift_ = 0;
    int viewType_ = 0, settleFrames_ = 0;
    bool releasingControl_ = false;
    std::vector<host::BackendEvent> staged_;

    XPLMDataRef head_[6] = {};  // pilots_head_x, _y, _z, _psi, _the, _phi
    XPLMDataRef eye_[3] = {};   // acf_peX, _Y, _Z: they move with the pilot's head in X-Plane 12
    core::Vec3 eye_body_;       // the eyepoint (body), read once per aircraft before the module moves the head
    bool eyeValid_ = false;
    bool wroteHead_ = false;    // the module has moved the head since it last gave it back
    XPLMDataRef fov_ = nullptr, viewType_ref_ = nullptr, external_ = nullptr;
    XPLMDataRef local_[3] = {}, attitude_[3] = {};  // local_x/y/z, theta/phi/psi
    XPLMCommandRef cockpitCmd_ = nullptr, chaseCmd_ = nullptr;
    XPLMProbeRef probe_ = nullptr;
};

}  // namespace fscs::xplane
