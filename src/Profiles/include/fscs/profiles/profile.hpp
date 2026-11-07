#pragma once
// Profile schema v4 (doc/protocol.md §2.1, design 2.0 §8): validation, migration and an immutable typed snapshot
// for the coordinator. Portable: no Windows API, no exceptions.

#include "fscs/core/engine.hpp"
#include "fscs/core/effects.hpp"
#include "fscs/core/mount.hpp"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace fscs::profiles {

using json = nlohmann::json;

constexpr int kSchemaVersion = 4;
constexpr std::size_t kMaxProfileBytes = 10u << 20;
constexpr std::size_t kMaxCameras = 2000;
constexpr std::size_t kMaxCategories = 100;
constexpr int kMaxViewId = 2147483647;

enum class CameraMode { AircraftCockpit, AircraftExterior, Orbit, Flyby, Airport, Free, Mounted };
const char* toString(CameraMode m);
std::optional<CameraMode> parseMode(const std::string& s);
// Modes this build can drive live. Others stay visible but cannot activate (design 2.0 §4.8).
bool modeActivatable(CameraMode m);

struct Issue {
    std::string path;
    std::string message;
};

struct ValidationResult {
    std::vector<Issue> errors;
    std::vector<Issue> warnings;
    bool readOnly = false;  // unknown required feature: open, never activate
    bool migrated = false;  // an older schema was upgraded in place
    bool ok() const { return errors.empty(); }
    std::string summary() const;
};

// Upgrades older schemas, then validates and normalises `doc` in place.
ValidationResult validateProfile(json& doc);

struct AirportCameraOverrides {
    std::optional<std::string> aircraftTracking;
    std::optional<bool> trackingHorizonGuard;
    std::optional<bool> autoZoomEnabled;
    std::optional<double> autoZoomFrameMeters, autoZoomMinFovDeg, autoZoomMaxFovDeg, autoZoomResponseSeconds;
    std::optional<double> heightMeters;
};

struct CameraEntry {
    std::string id;
    std::string categoryId;
    std::string name;
    CameraMode mode = CameraMode::AircraftCockpit;
    int viewId = 0;
    bool favorite = false;
    core::CameraPose pose;
    double originLat=0, originLon=0, originAltitude=0;
    core::TransitionSpec transition;
    double motionRadiusMeters = 35, motionHeightMeters = 8, motionDurationSeconds = 30, motionSideMeters = 35;
    bool motionClockwise = true, motionRepeat = false;
    bool interaction = true;
    std::string trackingPolicy = "nativeIfQualified";
    std::string aircraftTracking = "off";
    AirportCameraOverrides airportOverrides;
    std::string airportHeightCategory;
    double airportHeightBaseAltitudeMeters = 0, airportHeightMinimumMeters = 0;
    bool retainLook = false;
    bool resetMouseLookOnRelease = false;
    core::EffectsSettings effects;
    double followHorizon = 1;  // 1 = banks with the aircraft, 0 = horizon stays level (presentation only)
    core::Gimbal gimbal = core::Gimbal::Locked;            // mounted cameras only
    double mountVibration = core::kDefaultMountVibration;  // mounted cameras only, 0..1
    std::string seedDefinitionName;
    bool glance = false;  // offered by the Glance bindings (instrument close-up); cockpit views only
    bool cinematic = true;  // offered by Cinematic mode (automatic view playlist)
    bool followAircraft = false;    // free cameras only: keep the offset from the aircraft in world axes
    bool worldOriginValid = false;  // worldOrigin is present and a valid geodetic point
};

struct CategoryEntry {
    std::string id;
    std::string name;
    std::vector<std::string> cameraIds;
    std::string defaultCameraId;
};

struct ProfileSnapshot {
    std::string profileId;
    std::int64_t revision = 0;
    std::string name;
    std::string matchStrategy;
    std::string modelKey;
    std::string adapterContract;
    std::string defaultCameraId;
    bool readOnly = false;
    std::vector<CategoryEntry> categories;
    std::vector<CameraEntry> cameras;

    const CameraEntry* camera(const std::string& id) const;
    const CameraEntry* cameraByViewId(int viewId) const;
    const CategoryEntry* categoryOf(const std::string& cameraId) const;
};

// Default for a camera without a `glance` flag: cockpit cameras whose icon, or for an automatic icon whose name,
// marks them as an instrument view (the editor's "view-instrument" icon).
bool defaultGlance(const json& camera);
// Default for a camera without a `cinematic` flag: every exterior, orbit, fly-by, airport and mounted view; of the
// cockpit views only the profile's default (pilot) view. Free cameras are flown by the user and never included.
bool defaultCinematic(const json& camera, const std::string& defaultCameraId);

// Requires a document that passed validateProfile.
ProfileSnapshot toSnapshot(const json& doc);

// A fresh profile with one Cockpit category and a Pilot view at the eyepoint.
json makeStarterProfile(const std::string& name, const std::string& modelKey, const std::string& displayName);
bool isC172(const std::string& modelKey, const std::string& displayName);
// The C172 starter: MSFS's G1000 views, or (xplane) views framed in X-Plane 12's Laminar C172.
constexpr const char* kC172Template = "c172-complete-v1";
constexpr const char* kC172XPlaneTemplate = "c172-xplane-v1";
json makeC172Profile(const std::string& modelKey, const std::string& displayName, bool xplane = false);
bool isA320(const std::string& modelKey, const std::string& displayName);
// Pilot-eyepoint starter views, independently saved against the loaded aircraft package.
json makeA320Profile(const std::string& modelKey, const std::string& displayName);
// Replaces every ID with a new UUID, keeping references (default view included) consistent. Returns old->new map.
std::vector<std::pair<std::string, std::string>> reassignIds(json& doc);

std::string newUuid();
bool isUuid(const std::string& s);
// Cryptographic-quality random bytes from the platform (BCrypt on Windows, getentropy on WASI).
void randomBytes(void* out, std::size_t n);

}  // namespace fscs::profiles
