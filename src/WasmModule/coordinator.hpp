#pragma once
// Camera coordinator (design §5, §6): sole authority that permits a camera submission. Owns ownership state,
// session epoch, acquisition tokens, peek tokens and the motion engine. Single-threaded: every call comes from
// the host's SimConnect thread. Time is injected so tests can drive it deterministically.

#include "backend.hpp"
#include "head_tracking.hpp"
#include "input_router.hpp"
#include "traffic.hpp"
#include "fscs/core/drift.hpp"
#include "fscs/core/engine.hpp"
#include "fscs/core/horizon.hpp"
#include "fscs/core/mount.hpp"
#include "fscs/profiles/airport.hpp"
#include "fscs/profiles/bindings.hpp"
#include "fscs/profiles/profile.hpp"
#include "fscs/protocol/protocol.hpp"

#include <functional>
#include <optional>
#include <set>
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

// Smart views (glance). Glance views are the open profile's cockpit cameras with `glance` set, in category order;
// `exclude` (the view being glanced from) is never offered.
std::vector<const profiles::CameraEntry*> glanceViews(const profiles::ProfileSnapshot& profile, const std::string& exclude);
// Angle (radians) between where `shown` looks and the glance camera's instrument: the point kGlanceFocusMeters ahead
// of that camera, seen from the shown eyepoint. A camera at the eyepoint compares by its own direction.
constexpr double kGlanceFocusMeters = 0.75;
double glanceAngle(const core::CameraPose& shown, const profiles::CameraEntry& camera);
// The glance view nearest to the shown look direction. Within kGlanceTieRadians, the camera that itself points
// closest to the look direction wins, then category order. nullptr when there is none.
constexpr double kGlanceTieRadians = 0.5 * core::kDegToRad;
const profiles::CameraEntry* nearestGlanceView(const profiles::ProfileSnapshot& profile, const core::CameraPose& shown,
                                               const std::string& exclude);

class Coordinator {
public:
    using EmitFn = std::function<void(const std::string& name, json payload)>;
    using QualificationFn = std::function<Qualification(const std::string& simVersion)>;

    static constexpr double kAcquireTimeout = 2.0;
    static constexpr double kReleaseTimeout = 1.5;
    static constexpr double kReadTimeout = 1.0;
    static constexpr double kPreAcquireReadTimeout = 0.3;
    static constexpr double kMaxReconnectBackoff = 10.0;
    static constexpr double kGlanceTransitionSeconds = 0.35;  // both directions, smooth easing
    static constexpr double kCinematicTransitionSeconds = 2.5;  // smooth Cinematic mode moves, quintic easing

    Coordinator(ICameraBackend& backend, EmitFn emit, QualificationFn qualify);

    // Protocol command (Hello/Heartbeat/binding capture are handled by the host app). Returns the ack.
    json handle(const protocol::Command& cmd, double now);
    // Fast path: never queued behind other work.
    void release(const std::string& reason, double now);
    // Panic binding: release() plus forgetting every remembered temporary look. Valid in every ownership state and
    // idempotent; never re-activates afterwards. Emits PanicReleased so the host stops automatic switching.
    void panicRelease(double now);
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
    json walkaroundJson() const;

    // In-panel axis check (gate G01/G03): allows live control on an unqualified build while it runs, and shows
    // probe poses for the active camera through the current adapter contract.
    void setQualificationSession(bool on, double now);
    bool qualificationSession() const { return qualificationSession_; }
    bool previewPose(const core::CameraPose& pose);
    void refreshQualification();
    const Qualification& qualificationState() const { return qual_; }
    void setManualInput(const core::ManualInput& in) { manual_ = in; }
    void setMouseInputBlocked(bool blocked) { if (blocked) cancelMouseDrag(); mouseBlocked_ = blocked; }
    void cancelMouseDrag();
    void tick(double now);
    void setAircraftPosition(double lat, double lon, double altitude, double now, double headingDeg = 0) { targetLat_=lat; targetLon_=lon; targetAltitude_=altitude; targetHeading_=headingDeg; targetAt_=now; }
    bool selectAutomaticCamera(const std::string& id, double now, bool acquire = false) { return (!walkaround_ || acquire) && (acquire ? canActivate() : eligibleToSubmit()) && activate(id,now,false).empty(); }

    // State the host app re-sends to a newly connected UI.
    void emitFullState();

    Ownership ownership() const { return state_; }
    // A free camera (drone) is on screen and flown by manual input: gamepad sticks map only then.
    bool freeCameraShown() const;
    std::uint64_t epoch() const { return epoch_; }
    const std::string& activeCameraId() const { return activeCameraId_; }
    bool canActivate() const { return blockReason().empty(); }
    std::string blockReason() const;
    // Spotting needs no camera profile: only the simulator, a loaded flight, the axis check and the simulator's
    // permission. Chase, fly-by and the computed runway and tower views work without a profile; views that recall
    // profile cameras still need canActivate().
    std::string spottingBlockReason() const;
    bool spottingAvailable() const { return spottingBlockReason().empty(); }
    const core::CameraEngine& engine() const { return engine_; }
    const core::CameraEffects& effects() const { return effects_; }
    const core::MountVibration& mountVibration() const { return mountVibration_; }
    const Aircraft& aircraft() const { return aircraft_; }
    std::uint64_t submissions() const { return submissions_; }
    // Binoculars (hold-to-zoom): held while any binding hold is down; progress 0 = camera FOV, 1 = fully zoomed.
    bool binocularsHeld() const { return !binocularsHolds_.empty(); }
    double binocularsProgress() const { return binocularsProgress_; }
    double binocularsLookScale() const { return binocularsLookScale_; }
    // Time to ease fully in or out; reversing mid-way continues from the current zoom.
    static constexpr double kBinocularsEaseSeconds = 0.3;
    // Current glance (null when not glancing): {cameraId, returnCameraId, held}. Sent with ActiveCameraChanged.
    json glanceJson() const;

    // Cinematic mode (driven by the host): recalls `cameraId` with a cinematic transition (a cut, or a smooth quintic
    // move where the frames allow it) and starts its Commercial drift (an empty spec for views without one). A smooth
    // move keeps the previous drift offset and fades it out. False if the view cannot be shown now.
    bool selectCinematicCamera(const std::string& cameraId, double now, bool acquire, bool cut, const core::DriftSpec& drift);
    // Ends the drift at once (Cinematic mode off, release, panic, a manual recall).
    void stopCinematic() { cinematicDrift_.stop(); cinematicCameraId_.clear(); }
    // Holds the drift where it is (manual-input pause, glance, binoculars); a carried offset still fades.
    void holdCinematicDrift(bool hold) { cinematicDriftHold_ = hold; }
    const core::CommercialDrift& cinematicDrift() const { return cinematicDrift_; }
    // Accepted recalls of any kind; the host compares it to spot a recall Cinematic mode did not make.
    std::uint64_t recalls() const { return recalls_; }
    // Free cameras (drones), world-fixed or following the aircraft in world axes. Keyboard flight is unsaved
    // drone movement in meters (north, east, down) about the anchor until Save adjusted view folds it into the pose.
    static constexpr double kFreeRadiusMeters = 20000;          // horizontal reach from the anchor
    static constexpr double kFreeCeilingMeters = 10000;         // highest point above the anchor
    static constexpr double kFreeFloorMarginMeters = 300;       // without terrain: lowest point below the anchor
    static constexpr double kFreeTerrainClearanceMeters = 2;    // with terrain: lowest point above it
    static constexpr double kFreeTerrainRadiusMeters = 2000;    // terrain under the aircraft counts this far from it
    static constexpr double kFreeTerrainFreshSeconds = 5;
    static constexpr double kFreeSpeedPerMeterAboveGround = 0.5;  // speed factor 1 + h * this, h = height above ground
    static constexpr double kFreeMaxSpeedFactor = 400;
    const core::Vec3& freeOffset() const { return freeOffset_; }
    // Terrain height used for the active free camera at `drone` (meters MSL); NAN when none is available.
    double freeTerrainAltitude(const profiles::airport::Geo& drone, double now) const;

    // Spotting (traffic cameras). The traffic picture comes from SimConnect polls (Traffic events). spotTarget() shows
    // one aircraft in a tower, runway, chase, fly-by, track or position spot view, presented every frame from its
    // predicted pose, and takes control if needed; it returns why it cannot ("" when shown, or shown once the
    // simulator hands over).
    // Tower and runway views recall the loaded airport's own views and aim them at the target; track aims the world
    // view on screen; chase and fly-by are computed world poses over the view that was on screen. Any other recall,
    // Release, Panic and a loss of control end spotting.
    static constexpr double kSpotLostHoldSeconds = 5;      // a lost target holds the last view this long
    static constexpr double kSpotRunwayRecheckSeconds = 1;  // runway view: the nearest runway view is re-checked
    static constexpr double kSpotRunwayDwellSeconds = 6;    // ... no more often than this, with a 15 % margin
    // `runway` pins a runway view to that runway spot (SpotTarget.spot); without it the end is picked automatically.
    // `place` is the position spot of the spot view (required for it, ignored by the others).
    std::string spotTarget(std::uint32_t objectId, traffic::SpotView view, double now,
                           const std::optional<traffic::RunwaySpot>& runway = std::nullopt,
                           const std::optional<traffic::PositionSpot>& place = std::nullopt);
    // AdjustSpot: moves the pinned runway spot or the position spot being shown, keeping the target, the view and the
    // zoom (another runway end is a cut). Returns why it cannot: not spotting from a pinned spot, or the other kind.
    std::string adjustSpot(const std::optional<traffic::RunwaySpot>& runway, const std::optional<traffic::PositionSpot>& place, double now);
    // StopSpotting: back to the view shown before spotting began, or release when there was none.
    void stopSpotting(const std::string& reason, double now);
    // Ends spotting and keeps whatever world or aircraft view is underneath (another automatic mode takes over).
    // Without a view underneath (a spot taken without a camera profile) the camera goes back to the simulator.
    void endSpotting(const std::string& reason, double now);
    bool spotting() const { return spot_.has_value(); }
    // Chase and fly-by show no profile camera: ActiveCameraChanged / Snapshot report cameraId null meanwhile.
    bool spotOverlay() const { return spot_ && spot_->overlay; }
    std::uint32_t spotTargetId() const { return spot_ ? spot_->targetId : 0; }
    std::optional<traffic::SpotView> spotView() const { return spot_ ? std::optional<traffic::SpotView>(spot_->view) : std::nullopt; }
    bool spotLost() const { return spot_ && spot_->lost; }
    double spotLostSeconds(double now) const { return spot_ && spot_->lost ? now - spot_->lostAt : 0.0; }
    const std::string& spotLabel() const { static const std::string none; return spot_ ? spot_->label : none; }
    const std::string& spotReason() const { return spotReason_; }
    // The target pose the view aims at (held while lost) and, for chase / fly-by / held track, the camera position.
    traffic::Pose spotPose() const { return spot_ ? spot_->target : traffic::Pose{}; }
    std::optional<profiles::airport::Geo> spotCameraPosition() const;
    // Whether a view type can be shown for `objectId` now (tower and runway need the loaded airport's views).
    bool spotViewAvailable(traffic::SpotView view, std::uint32_t objectId, double now) const;
    // Auto-spot (driven by the host): a lost target then waits for the host to pick the next one.
    void setAutoSpot(bool on) { autoSpot_ = on; }
    void setTrafficAirport(const profiles::airport::Geo& airport) { traffic_.setAirport(airport); }
    // Scenery data for the computed runway-end and tower views (the loaded or nearest airport, and the one nearest
    // the target); set by the host from its facility cache.
    void setSpotAirports(std::vector<traffic::SpotAirport> airports) { spotAirports_ = std::move(airports); }
    const std::vector<traffic::SpotAirport>& spotAirports() const { return spotAirports_; }
    // The runway spot a computed runway view shows (nullopt otherwise), and whether it was pinned by SpotTarget.spot.
    std::optional<traffic::RunwaySpot> spotRunway() const { return spot_ ? spot_->runway : std::nullopt; }
    bool spotPinned() const { return spot_ && spot_->pinned; }
    // The position spot the spot view shows (nullopt otherwise).
    std::optional<traffic::PositionSpot> spotPlace() const { return spot_ ? spot_->place : std::nullopt; }
    // Whether tower and runway views can be shown at all: profile airport cameras (with control available) or
    // computed views from scenery data. SpottingChanged.views.
    json spotViewsJson() const;
    // Camera category heights for computed views (settings, else the generator's 3 m and 35 m).
    double runwayCameraHeight() const;
    double towerCameraHeight() const;
    const traffic::Model& traffic() const { return traffic_; }

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
    // eligibleToSubmit() without the active camera: a spot taken without a camera profile submits its overlay alone.
    bool submitReady() const;
    bool spotBare() const { return spot_ && activeCameraId_.empty() && pendingCameraId_.empty(); }
    void submit(bool refreshTrackingPosition = true);
    const std::string& aircraftTrackingFor(const profiles::CameraEntry& cam) const;
    void updateAirportAutoZoom(double dt);
    void updateAirportTrackingPosition();
    void updateHorizon(double dt, double now);
    std::string setBinoculars(bool pressed, std::uint64_t holdId);
    void cancelBinoculars();
    void updateBinoculars(double dt);
    double binocularsFov(double shownFov, double cameraFov);
    void emitBinoculars();
    void updateGimbal(double dt, double now);
    core::CameraPose effectiveAirportPose(const profiles::CameraEntry& cam) const;
    void emitCapabilities(bool force = false);
    void emitActiveCamera();
    void emitWalkaround();
    void stopWalkaround();
    core::CameraPose motionPose(const profiles::CameraEntry& cam) const;
    void emitLook(double now, bool force = false);
    void endPeek(std::uint64_t token, double now);
    std::string cycle(int delta, double now);
    std::string nextCategory(double now);
    const profiles::CameraEntry* activeEntry() const;
    Referential referentialFor(const profiles::CameraEntry& cam) const;
    void requestRead(ReadPurpose p, Referential ref, double now, double timeout);

    ICameraBackend& backend_;
    HeadTracking headTracking_{backend_};
    double targetLat_=0, targetLon_=0, targetAltitude_=0, targetHeading_=0, targetAt_=-100;
    double flybyLat_=0, flybyLon_=0, flybyAltitude_=0, flybyHeading_=0, motionTime_=0;
    struct Walkaround { std::string categoryId; double interval=5, elapsed=0; bool loop=true; };
    std::optional<Walkaround> walkaround_;
    bool walkaroundAdvancing_ = false, motionPaused_ = false;
    std::map<std::string, core::TemporaryLook> rememberedLooks_;
    std::string lookOwner_;
    EmitFn emit_;
    QualificationFn qualify_;

    // Connection / session
    bool simConnected_ = false;
    std::string simVersion_;
    std::uint64_t epoch_ = 1;
    double nextConnectAt_ = 0, backoff_ = 1.0;
    Aircraft aircraft_;
    std::string pendingTitle_;
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
    core::CameraEffects effects_;
    core::FlightMotion flightMotion_;
    double motionReceivedAt_ = -1;
    double aircraftBankDeg_ = NAN;  // FRD, right wing down > 0; NAN while unknown
    core::HorizonFilter horizon_;  // follow-horizon counter-roll, applied at submission only
    double submittedHorizonRollDeg_ = 0;  // the counter-roll inside lastSubmitted_
    double aircraftPitchDeg_ = NAN;  // FRD, nose up > 0; NAN while unknown
    core::GimbalFilter gimbal_;  // mounted cameras: gimbal stabilisation, applied at submission only
    core::MountVibration mountVibration_;  // mounted cameras: airframe vibration, applied at submission only
    core::ManualInput manual_;
    bool mouseMapped_ = false, mouseHeld_ = false, mouseBlocked_ = false, mouseMoved_ = false;
    std::string mouseCameraId_;
    unsigned mouseSamples_ = 0;
    unsigned mouseFrames_ = 0, mouseLargeSamples_ = 0;
    double mouseLastSampleAt_ = -1, mouseMaxSampleGap_ = 0, mouseMaxFrameGap_ = 0;
    double mouseMaxRaw_ = 0;
    bool mouseConcurrentLook_ = false;
    double mouseHeadingPending_ = 0, mousePitchPending_ = 0;
    double lastTick_ = -1;
    std::optional<SdkPose> lastSubmitted_;
    // Submission-only zoom: never changes the saved pose or the user's manual FOV.
    std::optional<double> airportAutoZoomFov_;
    std::optional<core::Vec3> airportTrackingPosition_; // lat, lon, actual MSL meters; never saved
    // Binoculars: submission-only FOV override, like airport auto zoom. Never written into the engine or a pose.
    std::set<std::uint64_t> binocularsHolds_;
    double binocularsProgress_ = 0;
    double binocularsLookScale_ = 1;         // shown FOV / camera FOV (at most 1), from the last submission
    double binocularsAppliedLookScale_ = 1;  // scale already applied to the keyboard look speed
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

    // Smart views: a glance shows an instrument close-up and returns to the exact view and temporary look it left.
    // Any other recall, Release Camera and Panic end it without a return.
    struct Glance {
        std::string returnCameraId;
        core::TemporaryLook returnLook;
        std::string cameraId;      // the glance view on screen
        std::uint64_t holdId = 0;  // non-zero while a held Glance binding owns it: its release returns
    };
    std::optional<Glance> glance_;
    struct GlanceReturn {
        std::string cameraId;
        core::TemporaryLook look;
    };
    std::optional<GlanceReturn> glanceReturn_;  // returning: re-applies the exact look once the transition lands

    // Cinematic mode: a transition override for the recall it makes, and the presentation-only Commercial drift.
    core::CameraPose cinematicPose(const profiles::CameraEntry& cam, const core::CameraPose& pose) const;
    std::optional<core::TransitionSpec> cinematicTransition_;
    core::CommercialDrift cinematicDrift_;
    std::string cinematicCameraId_;
    bool cinematicDriftHold_ = false;
    std::uint64_t recalls_ = 0;
    bool glanceCurrent() const;
    void dropStaleGlance();
    std::string glanceBlock() const;
    std::string glanceHold(std::uint64_t holdId, double now);
    void glanceHoldReleased(std::uint64_t holdId, double now);
    std::string glanceNext(double now);
    std::string glanceReturn(double now);
    void startGlance(const profiles::CameraEntry& target);
    void glanceTransition(const profiles::CameraEntry& cam, const core::CameraPose& target);
    void updateGlance(double now);

    // Reads (CameraGet has no correlation id: one outstanding read, quarantined on timeout)
    ReadPurpose read_ = ReadPurpose::None;
    std::string readCameraId_;
    double readDeadline_ = 0;
    std::optional<core::CameraPose> preAcquirePose_;
    bool readsUnreliable_ = false;
    bool qualificationSession_ = false;  // set after a read timeout: a late reply could be misread, so reads stay off
    std::string seedName_;

    json lastCapabilities_;

    // Free cameras (drones). The world pose is world(anchor, engine pose + freeOffset_); the anchor is the saved world
    // origin, or the aircraft position while following it.
    core::Vec3 freeOffset_, freeVelocity_;
    double freeAccumulator_ = 0;
    std::string freeCameraId_;
    profiles::airport::Geo freeOrigin_{NAN, NAN, NAN}, freeAnchor_{NAN, NAN, NAN};
    bool freeFollowing_ = false;
    std::optional<profiles::airport::Geo> freeSavedOrigin_;  // a Save's new origin until the profile carries it
    double groundAltitude_ = NAN, groundAt_ = -100;
    profiles::airport::Geo aircraftGeo() const { return {targetLat_, targetLon_, targetAltitude_}; }
    bool aircraftPositionFresh(double now) const;
    profiles::airport::Geo freeAnchor(const profiles::CameraEntry& cam) const;
    profiles::airport::Geo freeWorld(const profiles::CameraEntry& cam, const core::Vec3& local, double now) const;
    std::string freeBlock(const profiles::CameraEntry& cam, double now) const;
    void updateFreeCamera(double dt, double now);
    void resetFreeMotion();
    json dronePlacement(const profiles::airport::Geo& drone, const core::Quat& orientation, double fovRad) const;
    json droneHere(double now, std::string& error) const;
    json bakeFreeCamera(const protocol::Command& c, const profiles::CameraEntry& cam, const core::CameraPose& shown, double now);

    // Spotting. Overlay views (chase, fly-by, a track held where an overlay was) submit a computed world pose instead of
    // the active camera, which stays underneath as the base; tower, runway and track aim a world camera at the target.
    struct Spot {
        std::uint32_t targetId = 0;
        traffic::SpotView view = traffic::SpotView::Chase;
        std::string label;
        traffic::Pose target;          // predicted pose, held while lost
        bool lost = false;
        double lostAt = 0;
        std::string returnCameraId;    // shown before spotting began; empty: release on stop
        std::string cameraId;          // tower / runway / track: the world camera aimed at the target
        bool overlay = false, fixed = false, aboveGround = false;
        profiles::airport::Geo position{NAN, NAN, NAN};  // overlay camera
        core::Vec3 chaseOffset{NAN, NAN, NAN};
        core::Vec3 chaseSmoothed{NAN, NAN, NAN}, chaseRate;  // critically damped chase offset and its rate
        double fovRad = NAN;
        double runwayCheckAt = 0, cameraSince = 0;
        std::optional<traffic::RunwaySpot> runway;  // computed runway view: the runway spot shown
        bool pinned = false;                        // ... chosen by SpotTarget.spot: never re-picked
        std::optional<traffic::PositionSpot> place; // spot view: the position spot shown
    };
    std::optional<Spot> spot_;
    bool spotActivating_ = false, autoSpot_ = false;
    std::string spotReason_;
    traffic::Model traffic_;
    std::vector<traffic::SpotAirport> spotAirports_;
    bool spotAims(const profiles::CameraEntry& cam) const;
    const profiles::CameraEntry* spotCamera(traffic::SpotView view, const traffic::Pose& target) const;
    std::string spotActivate(const std::string& cameraId, double now);
    std::string spotAcquire(double now);  // takes control with no camera underneath
    void detachSpotBase();                // the profile view under an overlay went away: keep spotting without it
    void enforceBlock(double now);        // releases (or detaches a spot's base view) when control is blocked
    bool profileSpotCamera(traffic::SpotView view, const traffic::Pose& target, double range) const;
    void tickBareSpot(double dt, double now);
    void updateSpot(double dt, double now);
    void placeSpotOverlay(double dt, double now);
    void submitSpotOverlay();
    // Drops spotting and tells the host (SpotStopped): `manual` for another recall, `released` for a loss of control.
    void endSpot(const std::string& reason, bool manual, bool released);
};

}  // namespace fscs::host
