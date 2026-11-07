#pragma once
// The resident module (design 2.0 §5): authoritative for the working draft, commits, aircraft/profile matching,
// navigation selection, panel subscriptions and input routing. Panels are mirrors that send intent
// (doc/protocol.md). Pure logic over interfaces; the WASM entry points in wasm/ feed it events and frames.

#include "backend.hpp"
#include "cinematic.hpp"
#include "coordinator.hpp"
#include "input_router.hpp"
#include "fscs/profiles/airport.hpp"
#include "fscs/profiles/repository.hpp"
#include "fscs/protocol/protocol.hpp"

#include <deque>
#include <cmath>
#include <map>
#include <optional>
#include <set>
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
    // Cinematic mode: whether the playlist runs, its director (tests), and the director's random source (tests inject
    // a seeded one for determinism).
    bool cinematicEnabled() const { return cinematicOn_; }
    const cinematic::Director& cinematicDirector() const { return cinematic_; }
    cinematic::Phase cinematicPhase() const { return cinematicPhase_.phase(); }
    void setCinematicRandom(cinematic::Director::Random random) { cinematic_.setRandom(std::move(random)); }
    // Spotting (spotting.cpp): the traffic feed, SpotTarget / StopSpotting / SetAutoSpot and session-wide auto-spot.
    static constexpr double kTrafficPollSeconds = 1.5;      // SimConnect answers each type request once: re-issued
    static constexpr double kTrafficEmitSeconds = 1.0;      // TrafficChanged at most once a second
    static constexpr double kTrafficMaxRadiusMeters = 200000;  // SimConnect's limit for a type request
    static constexpr double kAutoSpotCheckSeconds = .5;
    static constexpr double kAutoSpotViewSeconds = 5;       // a status-driven view change at most this often
    bool autoSpotEnabled() const { return autoSpot_; }

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
        bool pointerOver = false;
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
    void importProfileCode(const protocol::Command& c, const std::string& code, double now);
    void reply(const protocol::Command& c, protocol::AckStatus s, const std::string& reason = {}, json result = nullptr);
    void send(const json& message);
    void emitState(const std::string& name, json payload);  // coalesced latest-state event
    void emitNow(const std::string& name, json payload, const std::string& to = {});
    void flushState(double now, bool force);
    void broadcastSnapshot(const std::string& to = {});
    json snapshot() const;
    bool anyVisibleLease() const;
    void updateMouseBlock();

    // Coordinator bridge
    void onCoordinatorEvent(const std::string& name, json payload);
    json forward(const protocol::Command& c, double now);

    // Working set
    void loadFromRepository(std::vector<std::string> notes, double now);
    void openProfile(const std::string& profileId, double now);
    void publishWorking(double now, bool bindingsChanged);
    void recomputeDirty();
    bool aircraftDirty() const;
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

    json combinedProfile() const;
    void splitCombined(json profile);
    bool validateCombined(json& profile, std::string& error) const;
    bool handleAirport(const protocol::Command& c, double now);
    bool airportEvent(const BackendEvent& ev, double now);
    void refreshAirports(double now);
    bool setAirportAuto(bool enabled, double now);
    void updateAirportAuto(double now);
    bool airportAuto_=false, autoMouseDown_=false, saveViewsRequested_=false;
    std::string autoCameraId_;
    double lastAutoCheck_=-100, lastAutoSwitch_=-100, positionAt_=-100;
    json airportState() const;
    std::map<std::string,json> airportDrafts_, airportPreviews_;
    struct AirportJob { std::string icao, to; json options; double deadline=0; };
    std::map<std::uint32_t,AirportJob> airportJobs_;
    std::uint32_t airportRequestId_=10000;
    profiles::airport::Geo position_{NAN,NAN,NAN};
    json nearbyAirports_=json::array(), nearbyFacilities_=json::array();
    std::string loadedAirport_, pinnedAirport_;
    double lastAirportRefresh_=-100;
    bool airportDirty() const;
    // Cinematic mode (cinematic.cpp): a session-wide automatic view playlist, like airport Auto switch.
    bool setCinematic(bool enabled, double now, std::string* why = nullptr);
    void stopCinematic(const std::string& reason, double now);
    void updateCinematic(double now, bool manual);
    bool cinematicSwitch(double now, bool acquire);
    void cinematicEvent(const BackendEvent& ev, double now);
    bool handleCinematic(const protocol::Command& c, double now);
    void cinematicPanic(json& payload, double now);
    // Spotting (spotting.cpp). Like Cinematic mode and airport Auto switch, auto-spot is module state, never saved.
    bool handleSpotting(const protocol::Command& c, double now);
    bool spottingEvent(const std::string& name, json& payload);
    void updateSpotting(double now);
    std::string spot(std::uint32_t objectId, traffic::SpotView view, double now,
                     const std::optional<traffic::RunwaySpot>& runway = std::nullopt,
                     const std::optional<traffic::PositionSpot>& place = std::nullopt);
    // SpotTarget / AdjustSpot `spot`: a runway spot (kinds fence, approach, threshold, custom) or a position spot.
    std::string parseSpot(const json& value, std::optional<traffic::RunwaySpot>& runway, std::optional<traffic::PositionSpot>& place) const;
    void updateSpotAirports(double now);
    static constexpr double kSpotAirportsSeconds = 2;
    bool setAutoSpot(bool enabled, double now, std::string* why = nullptr);
    void stopAutoSpot(const std::string& reason);
    void yieldSpotting(const std::string& reason);  // another automatic mode was turned on
    void spotCycle(int delta, double now);
    void updateAutoSpot(double now);
    std::optional<std::uint32_t> autoSpotPick(std::uint32_t exclude, double now) const;
    traffic::SpotView autoSpotView(std::uint32_t objectId, traffic::SpotView fallback, double now) const;
    bool autoSpotShow(std::uint32_t objectId, double now);
    profiles::airport::Geo trafficAirport() const;
    json trafficState() const;
    json spottingState() const;
    void emitSpotting(bool force = false);
    bool autoSpot_ = false, pendingSpotReturn_ = false, pendingAutoSpotToggle_ = false;
    int pendingSpotCycle_ = 0;
    std::string spotReason_, spottingEmitted_;
    double nextTrafficPoll_ = 0, lastTrafficEmit_ = -100, lastAutoSpotCheck_ = -100, autoSpotSince_ = 0, autoSpotViewAt_ = 0;
    std::uint64_t trafficEmitted_ = 0;
    std::uint32_t watchedTarget_ = 0;  // the aircraft with per-frame data (watchTrafficTarget)
    std::string spotAirportsKey_;
    double nextSpotAirports_ = 0;
    json cinematicState() const;
    void emitCinematic(bool force = false);
    cinematic::Director cinematic_;
    cinematic::PhaseTracker cinematicPhase_;
    bool cinematicOn_ = false, cinematicSuspended_ = false;
    std::uint64_t cinematicRecalls_ = 0;
    std::string cinematicReason_, cinematicEmitted_;
    // Temporary airport sets (airports.cpp): generated in memory, never stored until saved.
    bool airportAutoGenerate() const;
    json transientAirportList(json savedNearby, double now);
    void requestAutoAirport(const std::string& code, double now);
    bool autoAirportFacility(const backend_event::AirportFacility& e, double now);
    bool saveTemporaryAirport(const protocol::Command& c, double now);
    struct AutoAirportJob { std::string icao; double deadline=0; };
    std::set<std::string> transientAirports_;
    std::map<std::string,json> airportFacilityCache_;  // per-session, by ICAO; null = no usable scenery data
    std::map<std::uint32_t,AutoAirportJob> autoAirportJobs_;
    std::map<std::string,double> autoAirportRetry_;
    double lastAutoDiscovery_=-1000;
    profiles::airport::Geo autoDiscoveryAt_{NAN,NAN,NAN};
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
    std::set<std::string> defaultProfileAttempts_;
    json lastAircraft_ = json::object();
    std::string captureRequester_;
    std::string testDefinition_;
    double testUntil_ = 0;
    int controllerCaptureDevice_ = -1, controllerCaptureButton_ = -1, controllerCaptureHat_ = -1;
    std::string controllerCaptureDirection_;
    std::uint64_t controllerCaptureGeneration_ = 0;
    std::string controllerCaptureRequester_, controllerCaptureId_;
    double controllerCaptureUntil_ = 0, controllerCapturePoll_ = 0;
    void stopControllerCapture(const std::string& state);
    void completeControllerCapture(const std::string& suffix, const std::string& label, const std::string& pov = {});
    std::vector<std::pair<int, std::string>> controllers_;
    std::deque<std::string> diagnostics_;
    bool diagDirty_ = false, diagWriting_ = false;
    double lastDiagWrite_ = -10;
    void writeDiagnostics(double now);
    double now_ = 0;
};

}  // namespace fscs::host
