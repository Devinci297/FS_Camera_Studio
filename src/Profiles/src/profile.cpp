#include "fscs/profiles/profile.hpp"
#include "fscs/profiles/airport.hpp"

#include "fscs/profiles/json_util.hpp"

#include <algorithm>
#include <cmath>
#include <cctype>
#include <map>
#include <set>
#include <sstream>

#if defined(_WIN32)
#include <windows.h>
#include <bcrypt.h>
#pragma comment(lib, "bcrypt.lib")
#else
#include <time.h>
#include <unistd.h>
#endif

namespace fscs::profiles {

namespace {

using core::Quat;

const std::set<std::string> kKnownFeatures = {"airport-world-v1"};  // schema 2 defines no optional required features yet

json starterCalibration() {
    return {{"frameContract", "FRD-camera-to-anchor-xyzw-v1"},
            {"anchor", {{"kind", "sdkPilotEyepoint"}, {"seedDefinitionName", nullptr},
                        {"correctionMetersFRD", {0.0, 0.0, 0.0}}, {"correctionRotationXYZW", {0.0, 0.0, 0.0, 1.0}}}},
            {"adapterContract", "pending-qualification"},
            {"simulatorBuild", nullptr}};
}

struct Checker {
    ValidationResult& r;
    void error(std::string path, std::string msg) { r.errors.push_back({std::move(path), std::move(msg)}); }
    void warn(std::string path, std::string msg) { r.warnings.push_back({std::move(path), std::move(msg)}); }

    bool text(const json& o, const char* key, const std::string& path, std::size_t minLen, std::size_t maxLen) {
        const json& v = js::get(o, key);
        if (!v.is_string()) {
            error(path + "." + key, "must be text");
            return false;
        }
        const auto& s = v.get_ref<const std::string&>();
        if (s.size() < minLen || s.size() > maxLen || !js::validUtf8(s)) {
            error(path + "." + key, "must be " + std::to_string(minLen) + "-" + std::to_string(maxLen) + " characters of text");
            return false;
        }
        return true;
    }
    bool numbers(const json& o, const char* key, std::size_t n, const std::string& path) {
        const json& v = js::get(o, key);
        if (!v.is_array() || v.size() != n) {
            error(path + "." + key, "must be a list of " + std::to_string(n) + " numbers");
            return false;
        }
        for (const auto& x : v)
            if (!x.is_number() || !std::isfinite(x.get<double>())) {
                error(path + "." + key, "contains a value that is not a finite number");
                return false;
            }
        return true;
    }
};

// Normalises a quaternion array in place; rejects a zero or badly denormalised value.
bool fixQuaternion(json& arr, Checker& c, const std::string& path) {
    Quat q{arr[0].get<double>(), arr[1].get<double>(), arr[2].get<double>(), arr[3].get<double>()};
    const double n = q.norm();
    if (!(n > 1e-9)) {
        c.error(path, "orientation is a zero quaternion");
        return false;
    }
    if (std::abs(n - 1.0) > 1e-3) {
        c.error(path, "orientation quaternion is not normalised");
        return false;
    }
    q = q.normalized();
    arr = json::array({q.x, q.y, q.z, q.w});
    return true;
}

// Schema 1 -> 2: shortcutSlot became viewId; easing names shifted (v1 Smooth was quintic, Cinematic septic).
void migrateFrom1(json& doc) {
    doc["schemaVersion"] = 2;
    if (!doc.contains("cameras") || !doc["cameras"].is_array()) return;
    for (auto& cam : doc["cameras"]) {
        if (!cam.is_object()) continue;
        if (cam.contains("shortcutSlot")) {
            cam["viewId"] = js::integer(cam["shortcutSlot"], 0);
            cam.erase("shortcutSlot");
        }
        if (cam.contains("transition") && cam["transition"].is_object()) {
            auto& t = cam["transition"];
            const std::string kind = js::str(t, "kind");
            if (kind == "smoothstep5") t["kind"] = "smoothstep3";
            else if (kind == "smoothstep7") t["kind"] = "smoothstep5";
        }
    }
}

}  // namespace

const char* toString(CameraMode m) {
    switch (m) {
        case CameraMode::AircraftCockpit: return "aircraftCockpit";
        case CameraMode::AircraftExterior: return "aircraftExterior";
        case CameraMode::Orbit: return "orbit";
        case CameraMode::Flyby: return "flyby";
        case CameraMode::Airport: return "airport";
        case CameraMode::Free: return "free";
    }
    return "aircraftCockpit";
}

std::optional<CameraMode> parseMode(const std::string& s) {
    static const std::map<std::string, CameraMode> m = {
        {"aircraftCockpit", CameraMode::AircraftCockpit}, {"aircraftExterior", CameraMode::AircraftExterior},
        {"orbit", CameraMode::Orbit},                     {"flyby", CameraMode::Flyby},
        {"airport", CameraMode::Airport},                 {"free", CameraMode::Free}};
    auto it = m.find(s);
    if (it == m.end()) return std::nullopt;
    return it->second;
}

bool modeActivatable(CameraMode m) { return m == CameraMode::AircraftCockpit || m == CameraMode::AircraftExterior || m == CameraMode::Airport || m == CameraMode::Orbit || m == CameraMode::Flyby; }

std::string ValidationResult::summary() const {
    if (errors.empty()) return "valid";
    std::ostringstream s;
    const std::size_t shown = std::min<std::size_t>(errors.size(), 5);
    for (std::size_t i = 0; i < shown; ++i) s << (i ? "; " : "") << errors[i].path << ": " << errors[i].message;
    if (errors.size() > shown) s << " (+" << errors.size() - shown << " more)";
    return s.str();
}

ValidationResult validateProfile(json& doc) {
    ValidationResult r;
    Checker c{r};
    if (!doc.is_object()) {
        c.error("$", "profile must be a JSON object");
        return r;
    }
    const auto version = js::integer(doc, "schemaVersion", -1);
    if (version == 1) {
        migrateFrom1(doc);
        r.migrated = true;
    } else if (version != kSchemaVersion) {
        c.error("$.schemaVersion", "unsupported schema version (this build reads versions 1 and 2)");
        return r;
    }
    if (c.text(doc, "profileId", "$", 36, 36) && !isUuid(doc["profileId"].get<std::string>())) c.error("$.profileId", "must be a UUID");
    c.text(doc, "name", "$", 1, 60);
    const bool airportProfile=js::str(doc,"scope")=="airport";
    const auto airportCode=js::str(js::object(doc,"airport"),"icao");
    if(airportProfile && (!airport::validCode(airportCode)||!airport::geo(js::get(js::object(doc,"airport"),"origin")).valid()))
        c.error("$.airport","Airport profiles need a valid ICAO and geographic origin.");
    if (!doc.contains("revision") || !doc["revision"].is_number_integer()) doc["revision"] = 0;
    if (doc["revision"].get<std::int64_t>() < 0) c.error("$.revision", "must not be negative");

    // Airport libraries are global regardless of legacy aircraft-match metadata.
    if(airportProfile)doc["aircraftMatch"]={{"strategy","none"},{"modelKey",nullptr},{"variantKey",nullptr},{"displayName",nullptr},{"confirmed",false}};
    // Aircraft match.
    if (!doc.contains("aircraftMatch") || !doc["aircraftMatch"].is_object()) doc["aircraftMatch"] = {{"strategy", "none"}};
    {
        auto& m = doc["aircraftMatch"];
        const std::string strategy = js::str(m, "strategy", "none");
        if (strategy != "aircraftPath" && strategy != "userConfirmedModelVariant" && strategy != "none")
            c.error("$.aircraftMatch.strategy", "unknown match strategy");
        if (strategy != "none" && js::str(m, "modelKey").empty()) c.error("$.aircraftMatch.modelKey", "is required for an aircraft profile");
        for (const char* k : {"modelKey", "variantKey", "displayName"})
            if (!m.contains(k) || !(m[k].is_string() || m[k].is_null())) m[k] = nullptr;
        if (!m.contains("confirmed") || !m["confirmed"].is_boolean()) m["confirmed"] = strategy == "userConfirmedModelVariant";
    }

    // Calibration metadata.
    if (!doc.contains("calibration") || !doc["calibration"].is_object()) {
        doc["calibration"] = starterCalibration();
    } else {
        auto& cal = doc["calibration"];
        if (js::str(cal, "frameContract") != "FRD-camera-to-anchor-xyzw-v1") c.error("$.calibration.frameContract", "unknown frame contract");
        if (!cal.contains("anchor") || !cal["anchor"].is_object()) {
            c.error("$.calibration.anchor", "is required");
        } else {
            auto& a = cal["anchor"];
            const std::string kind = js::str(a, "kind");
            if (kind != "sdkPilotEyepoint" && kind != "sdkDatum" && kind != "applicationCalibrated")
                c.error("$.calibration.anchor.kind", "unknown anchor kind");
            if (c.numbers(a, "correctionMetersFRD", 3, "$.calibration.anchor") &&
                c.numbers(a, "correctionRotationXYZW", 4, "$.calibration.anchor"))
                fixQuaternion(a["correctionRotationXYZW"], c, "$.calibration.anchor.correctionRotationXYZW");
        }
        if (!js::get(cal, "adapterContract").is_string()) cal["adapterContract"] = "pending-qualification";
    }

    if (!doc.contains("requiredSchemaFeatures") || !doc["requiredSchemaFeatures"].is_array()) doc["requiredSchemaFeatures"] = json::array();
    for (const auto& f : doc["requiredSchemaFeatures"])
        if (!f.is_string() || !kKnownFeatures.count(f.get<std::string>())) {
            r.readOnly = true;
            c.warn("$.requiredSchemaFeatures", "needs a newer version of Camera Studio; opened read-only");
        }
    if (!doc.contains("sequences") || !doc["sequences"].is_array()) doc["sequences"] = json::array();

    // Cameras.
    std::map<std::string, std::string> cameraCategory;
    std::set<std::int64_t> viewIds;
    if (!doc.contains("cameras") || !doc["cameras"].is_array() || doc["cameras"].empty()) {
        c.error("$.cameras", "a profile needs at least one camera");
    } else if (doc["cameras"].size() > kMaxCameras) {
        c.error("$.cameras", "more than " + std::to_string(kMaxCameras) + " cameras");
    } else {
        std::size_t i = 0;
        for (auto& cam : doc["cameras"]) {
            const std::string p = "$.cameras[" + std::to_string(i++) + "]";
            if (!cam.is_object()) {
                c.error(p, "must be an object");
                continue;
            }
            if (!c.text(cam, "id", p, 36, 36) || !isUuid(cam["id"].get<std::string>())) {
                c.error(p + ".id", "must be a UUID");
                continue;
            }
            const std::string id = cam["id"].get<std::string>();
            if (cameraCategory.count(id)) c.error(p + ".id", "duplicate camera id");
            c.text(cam, "name", p, 1, 60);
            if (c.text(cam, "categoryId", p, 36, 36)) cameraCategory[id] = cam["categoryId"].get<std::string>();
            else cameraCategory[id] = "";
            if (!parseMode(js::str(cam, "mode"))) c.error(p + ".mode", "unknown camera mode");
            if(airportProfile && (js::str(cam,"mode")!="airport" || js::str(cam,"airportIcao")!=airportCode))
                c.error(p,"This camera must belong to the same airport as its profile.");
            if (js::str(cam, "mode") == "airport") {
                const auto& origin = js::object(cam, "worldOrigin");
                double lat=js::num(origin,"lat",NAN), lon=js::num(origin,"lon",NAN), alt=js::num(origin,"altitude",NAN);
                if(!std::isfinite(lat)||std::abs(lat)>=89.9||!std::isfinite(lon)||std::abs(lon)>180||!std::isfinite(alt)||alt< -1500||alt>30000)
                    c.error(p+".worldOrigin", "Airport camera needs a valid world origin in degrees/meters.");
            }
            if (c.numbers(cam, "positionMetersFRD", 3, p))
                for (const auto& v : cam["positionMetersFRD"])
                    if (std::abs(v.get<double>()) > core::kMaxPositionMeters) c.error(p + ".positionMetersFRD", "is more than 10 km from the anchor");
            if (c.numbers(cam, "orientationCameraToAnchorXYZW", 4, p))
                fixQuaternion(cam["orientationCameraToAnchorXYZW"], c, p + ".orientationCameraToAnchorXYZW");
            {
                const json& pr = js::get(cam, "projection");
                const double v = js::num(pr, "value", NAN);
                if (!pr.is_object() || js::str(pr, "kind") != "simulatorFovRadians") c.error(p + ".projection", "unknown projection");
                else if (!std::isfinite(v) || v < core::kMinFovRad || v > core::kMaxFovRad)
                    c.error(p + ".projection.value", "field of view must be 0.1-2.8 radians");
            }
            if (!cam.contains("transition") || !cam["transition"].is_object())
                cam["transition"] = {{"kind", "smoothstep3"}, {"durationSeconds", core::kDefaultTransitionSeconds}, {"delaySeconds", 0.0}};
            {
                auto& t = cam["transition"];
                const std::string kind = js::str(t, "kind");
                if (kind != "cut" && kind != "smoothstep3" && kind != "smoothstep5") c.error(p + ".transition.kind", "unknown transition");
                if (!t.contains("durationSeconds")) t["durationSeconds"] = core::kDefaultTransitionSeconds;
                if (!t.contains("delaySeconds")) t["delaySeconds"] = 0.0;
                const double d = js::num(t, "durationSeconds", NAN), delay = js::num(t, "delaySeconds", NAN);
                if (!std::isfinite(d) || (kind != "cut" && (d < core::kMinTransitionSeconds || d > core::kMaxTransitionSeconds)))
                    c.error(p + ".transition.durationSeconds", "must be 0.1-15 seconds");
                if (!std::isfinite(delay) || delay < 0 || delay > core::kMaxDelaySeconds) c.error(p + ".transition.delaySeconds", "must be 0-30 seconds");
            }
            if (cam.contains("color")) {
                const auto color = js::str(cam, "color");
                if (color.size() != 7 || color[0] != '#' || color.find_first_not_of("0123456789abcdefABCDEF", 1) != std::string::npos)
                    c.error(p + ".color", "must be a six-digit hex color");
            }
            const std::int64_t vid = js::integer(cam, "viewId", 0);
            if (vid < 0 || vid > kMaxViewId) c.error(p + ".viewId", "must be a non-negative 32-bit integer");
            else if (vid > 0 && !viewIds.insert(vid).second) c.error(p + ".viewId", "view ID " + std::to_string(vid) + " is used twice");
            cam["viewId"] = vid;
            const std::pair<const char*, json> defaults[] = {
                {"favorite", false},          {"interaction", true},  {"trackingPolicy", "nativeIfQualified"},
                {"temporaryLookPolicy", "resetOnRecall"}, {"levelHorizon", false}, {"seedDefinitionName", nullptr},
                {"effects", {{"enabled", false}}}, {"requiredCapabilities", json::array()}};
            for (const auto& [k, v] : defaults)
                if (!cam.contains(k)) cam[k] = v;
            if (cam.contains("motion") && !cam["motion"].is_object()) c.error(p + ".motion", "must be an object");
            if (!cam.contains("motion")) cam["motion"] = json::object();
            if (cam["motion"].is_object()) {
                auto& motion = cam["motion"];
                struct Range { const char* key; double value, lo, hi; };
                for (const auto& v : {Range{"radiusMeters",35,2,5000}, Range{"heightMeters",8,1,2000}, Range{"durationSeconds",30,5,600}, Range{"sideMeters",35,-5000,5000}}) {
                    if (!motion.contains(v.key)) motion[v.key] = v.value;
                    const double value = js::num(motion,v.key,-1);
                    if (!motion[v.key].is_number() || !std::isfinite(value) || value<v.lo || value>v.hi)
                        c.error(p + ".motion." + v.key, "outside allowed motion range");
                }
                if (!motion.contains("clockwise")) motion["clockwise"] = true;
                if (!motion["clockwise"].is_boolean()) c.error(p + ".motion.clockwise", "must be true or false");
                if (!motion.contains("repeat")) motion["repeat"] = false;
                if (!motion["repeat"].is_boolean()) c.error(p + ".motion.repeat", "must be true or false");
            }
            auto& fx = cam["effects"];
            const auto tracking = js::str(cam, "trackingPolicy");
            if (tracking != "nativeIfQualified" && tracking != "on" && tracking != "off") c.error(p + ".trackingPolicy", "must be nativeIfQualified, on or off");
            if (!cam.contains("aircraftTracking")) cam["aircraftTracking"] = "off";
            const auto target = js::str(cam, "aircraftTracking");
            if (target != "off" && target != "heading" && target != "both") c.error(p + ".aircraftTracking", "must be off, heading or both");
            else if (js::str(cam, "mode") != "airport" && target != "off") {
                cam["aircraftTracking"] = "off";
                c.warn(p + ".aircraftTracking", "Aircraft tracking is only available for airport cameras; disabled for this view.");
            }
            if (!fx.is_object()) c.error(p + ".effects", "must be an object");
            else {
                if (!fx.contains("enabled")) fx["enabled"] = false;
                if (!fx["enabled"].is_boolean()) c.error(p + ".effects.enabled", "must be true or false");
                const std::pair<const char*, double> amounts[] = {{"intensity", .35}, {"turbulence", .5}, {"groundRoll", .35}, {"touchdown", .5}, {"acceleration", .35}, {"engine", .2}};
                for (const auto& [key, fallback] : amounts) {
                    if (!fx.contains(key)) fx[key] = fallback;
                    const double value = js::num(fx, key, -1.);
                    if (!fx[key].is_number() || !std::isfinite(value) || value < 0 || value > 1)
                        c.error(p + ".effects." + key, "must be a number between 0 and 1");
                }
            }
            const std::string look = js::str(cam, "temporaryLookPolicy");
            if (look != "resetOnRecall" && look != "retain") c.error(p + ".temporaryLookPolicy", "unknown look policy");
            const std::string mouseRelease = js::str(cam, "mouseLookOnRelease", "retain");
            if ((cam.contains("mouseLookOnRelease") && !cam["mouseLookOnRelease"].is_string()) || (mouseRelease != "retain" && mouseRelease != "reset")) c.error(p + ".mouseLookOnRelease", "must be retain or reset");
            if (!cam["favorite"].is_boolean() || !cam["interaction"].is_boolean() || !cam["levelHorizon"].is_boolean())
                c.error(p, "favorite, interaction and levelHorizon must be true or false");
            if (!(cam["seedDefinitionName"].is_null() || cam["seedDefinitionName"].is_string())) cam["seedDefinitionName"] = nullptr;
        }
    }

    // Categories.
    std::map<std::string, int> listed;
    std::set<std::string> categoryIds;
    if (!doc.contains("categories") || !doc["categories"].is_array() || doc["categories"].empty()) {
        c.error("$.categories", "a profile needs at least one category");
    } else if (doc["categories"].size() > kMaxCategories) {
        c.error("$.categories", "more than " + std::to_string(kMaxCategories) + " categories");
    } else {
        std::size_t i = 0;
        for (const auto& cat : doc["categories"]) {
            const std::string p = "$.categories[" + std::to_string(i++) + "]";
            if (!cat.is_object()) {
                c.error(p, "must be an object");
                continue;
            }
            if (!c.text(cat, "id", p, 36, 36) || !isUuid(cat["id"].get<std::string>())) {
                c.error(p + ".id", "must be a UUID");
                continue;
            }
            const std::string cid = cat["id"].get<std::string>();
            if (!categoryIds.insert(cid).second) c.error(p + ".id", "duplicate category id");
            c.text(cat, "name", p, 1, 60);
            const json& ids = js::get(cat, "cameraIds");
            if (!ids.is_array()) {
                c.error(p + ".cameraIds", "must be a list");
                continue;
            }
            if (cat.contains("defaultCameraId") && (!cat["defaultCameraId"].is_string() || std::find(ids.begin(), ids.end(), cat["defaultCameraId"]) == ids.end()))
                c.error(p + ".defaultCameraId", "must name a camera in this category");
            if (cat.contains("color")) {
                const auto color = js::str(cat, "color");
                if (color.size() != 7 || color[0] != '#' || color.find_first_not_of("0123456789abcdefABCDEF", 1) != std::string::npos)
                    c.error(p + ".color", "must be a six-digit hex color");
            }
            for (const auto& ref : ids) {
                const std::string id = js::str(ref);
                auto it = cameraCategory.find(id);
                if (it == cameraCategory.end()) c.error(p + ".cameraIds", "references a missing camera");
                else if (it->second != cid) c.error(p + ".cameraIds", "lists a camera that belongs to another category");
                listed[id]++;
            }
        }
    }
    for (const auto& [id, cat] : cameraCategory) {
        const int n = listed.count(id) ? listed[id] : 0;
        if (n != 1) c.error("$.categories", n == 0 ? "a camera is not listed in any category" : "a camera is listed more than once");
        if (!categoryIds.count(cat)) c.error("$.cameras", "a camera names a missing category");
    }
    if (!cameraCategory.count(js::str(doc, "defaultCameraId"))) c.error("$.defaultCameraId", "must name one of the profile's cameras");
    return r;
}

const CameraEntry* ProfileSnapshot::camera(const std::string& id) const {
    for (const auto& c : cameras)
        if (c.id == id) return &c;
    return nullptr;
}

const CameraEntry* ProfileSnapshot::cameraByViewId(int viewId) const {
    if (viewId <= 0) return nullptr;
    for (const auto& c : cameras)
        if (c.viewId == viewId) return &c;
    return nullptr;
}

const CategoryEntry* ProfileSnapshot::categoryOf(const std::string& cameraId) const {
    for (const auto& cat : categories)
        if (std::find(cat.cameraIds.begin(), cat.cameraIds.end(), cameraId) != cat.cameraIds.end()) return &cat;
    return nullptr;
}

ProfileSnapshot toSnapshot(const json& doc) {
    ProfileSnapshot s;
    s.profileId = js::str(doc, "profileId");
    s.revision = js::integer(doc, "revision", 0);
    s.name = js::str(doc, "name");
    const json& m = js::object(doc, "aircraftMatch");
    s.matchStrategy = js::str(m, "strategy", "none");
    s.modelKey = js::str(m, "modelKey");
    s.adapterContract = js::str(js::object(doc, "calibration"), "adapterContract", "pending-qualification");
    s.defaultCameraId = js::str(doc, "defaultCameraId");
    for (const auto& f : js::array(doc, "requiredSchemaFeatures"))
        if (!f.is_string() || !kKnownFeatures.count(f.get<std::string>())) s.readOnly = true;
    for (const auto& cat : js::array(doc, "categories")) {
        CategoryEntry e{js::str(cat, "id"), js::str(cat, "name"), {}};
        for (const auto& id : js::array(cat, "cameraIds")) e.cameraIds.push_back(js::str(id));
        e.defaultCameraId = js::str(cat, "defaultCameraId");
        s.categories.push_back(std::move(e));
    }
    for (const auto& cam : js::array(doc, "cameras")) {
        CameraEntry e;
        e.id = js::str(cam, "id");
        e.categoryId = js::str(cam, "categoryId");
        e.name = js::str(cam, "name");
        e.mode = parseMode(js::str(cam, "mode")).value_or(CameraMode::AircraftCockpit);
        e.viewId = static_cast<int>(js::integer(cam, "viewId", 0));
        e.favorite = js::boolean(cam, "favorite", false);
        const json& p = js::get(cam, "positionMetersFRD");
        const json& q = js::get(cam, "orientationCameraToAnchorXYZW");
        if (p.is_array() && p.size() == 3) e.pose.positionFRD = {js::num(p[0], 0), js::num(p[1], 0), js::num(p[2], 0)};
        if (q.is_array() && q.size() == 4)
            e.pose.orientation = core::Quat{js::num(q[0], 0), js::num(q[1], 0), js::num(q[2], 0), js::num(q[3], 1)}.normalized();
        e.pose.fovRad = js::num(js::object(cam, "projection"), "value", 1.0);
        const json& t = js::object(cam, "transition");
        const std::string kind = js::str(t, "kind", "smoothstep3");
        e.transition.easing = kind == "cut" ? core::Easing::Cut : kind == "smoothstep5" ? core::Easing::Smoothstep5 : core::Easing::Smoothstep3;
        e.transition.durationSeconds = js::num(t, "durationSeconds", core::kDefaultTransitionSeconds);
        e.transition.delaySeconds = js::num(t, "delaySeconds", 0.0);
        const auto& motion = js::object(cam,"motion");
        e.motionRadiusMeters = js::num(motion,"radiusMeters",35);
        e.motionHeightMeters = js::num(motion,"heightMeters",8);
        e.motionDurationSeconds = js::num(motion,"durationSeconds",30);
        e.motionClockwise = js::boolean(motion,"clockwise",true);
        e.motionSideMeters = js::num(motion,"sideMeters",35);
        e.motionRepeat = js::boolean(motion,"repeat",false);
        e.interaction = js::boolean(cam, "interaction", true);
        e.trackingPolicy = js::str(cam, "trackingPolicy", "nativeIfQualified");
        e.aircraftTracking = e.mode == CameraMode::Airport ? js::str(cam, "aircraftTracking", "off") : "off";
        e.retainLook = js::str(cam, "temporaryLookPolicy") == "retain";
        e.originLat=js::num(js::object(cam,"worldOrigin"),"lat",0); e.originLon=js::num(js::object(cam,"worldOrigin"),"lon",0); e.originAltitude=js::num(js::object(cam,"worldOrigin"),"altitude",0);
        e.resetMouseLookOnRelease = js::str(cam, "mouseLookOnRelease", "retain") == "reset";
        const auto& fx = js::object(cam, "effects");
        e.effects = {js::boolean(fx, "enabled", false), js::num(fx, "intensity", .35), js::num(fx, "turbulence", .5),
                     js::num(fx, "groundRoll", .35), js::num(fx, "touchdown", .5), js::num(fx, "acceleration", .35), js::num(fx, "engine", .2)};
        e.levelHorizon = js::boolean(cam, "levelHorizon", false);
        e.seedDefinitionName = js::str(cam, "seedDefinitionName");
        s.cameras.push_back(std::move(e));
    }
    return s;
}

void randomBytes(void* out, std::size_t n) {
#if defined(_WIN32)
    BCryptGenRandom(nullptr, static_cast<PUCHAR>(out), static_cast<ULONG>(n), BCRYPT_USE_SYSTEM_PREFERRED_RNG);
#else
    // In-sim getentropy() was observed to fill only the first few bytes, so it only seeds and stirs a SplitMix64
    // stream together with the clock and a counter. IDs need uniqueness, not cryptographic strength.
    static std::uint64_t state = 0;
    static std::uint64_t counter = 0;
    std::uint64_t fresh = 0;
    getentropy(&fresh, sizeof(fresh));
    timespec ts{};
    clock_gettime(CLOCK_REALTIME, &ts);
    state ^= fresh ^ (static_cast<std::uint64_t>(ts.tv_sec) * 0x9E3779B97F4A7C15ull) ^ static_cast<std::uint64_t>(ts.tv_nsec) ^
             (++counter << 32) ^ reinterpret_cast<std::uintptr_t>(out);
    auto* p = static_cast<unsigned char*>(out);
    while (n > 0) {
        std::uint64_t z = (state += 0x9E3779B97F4A7C15ull);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        z ^= z >> 31;
        for (int i = 0; i < 8 && n > 0; ++i, --n) *p++ = static_cast<unsigned char>(z >> (8 * i));
    }
#endif
}

std::string newUuid() {
    unsigned char b[16] = {};
    randomBytes(b, sizeof(b));
    b[6] = static_cast<unsigned char>((b[6] & 0x0F) | 0x40);
    b[8] = static_cast<unsigned char>((b[8] & 0x3F) | 0x80);
    static const char* hex = "0123456789abcdef";
    std::string s;
    for (int i = 0; i < 16; ++i) {
        if (i == 4 || i == 6 || i == 8 || i == 10) s.push_back('-');
        s.push_back(hex[b[i] >> 4]);
        s.push_back(hex[b[i] & 15]);
    }
    return s;
}

bool isUuid(const std::string& s) {
    if (s.size() != 36) return false;
    for (std::size_t i = 0; i < s.size(); ++i) {
        const char ch = s[i];
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (ch != '-') return false;
        } else if (!((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f') || (ch >= 'A' && ch <= 'F'))) {
            return false;
        }
    }
    return true;
}

namespace {
// Explicit defaults remain recognizable when users rename their cameras.
const char* builtinViewIcon(const std::string& name) {
    static const std::map<std::string, const char*> icons = {
        {"Cockpit", "view-cockpit"},
        {"Flight deck", "view-cockpit"},
        {"Instruments", "view-instrument"},
        {"Controls", "view-controls"},
        {"Cabin", "view-seat"},
        {"Exterior (Preview)", "plane"},
        {"External preview", "plane"},
        {"Pilot view", "view-cockpit"},
        {"Pilot - normal", "view-cockpit"},
        {"Pilot - landing", "view-cockpit"},
        {"Pilot - wide cockpit", "view-cockpit"},
        {"Pilot - wide", "view-cockpit"},
        {"Copilot", "view-seat"},
        {"G1000 - PFD", "view-instrument"},
        {"G1000 - MFD", "view-instrument"},
        {"G1000 - full panel", "view-instrument"},
        {"Standby instruments", "view-instrument"},
        {"Throttle, mixture and trim", "view-controls"},
        {"Electrical and lighting", "view-controls"},
        {"Overhead and cabin lighting", "view-controls"},
        {"Rear seats - forward", "view-seat"},
        {"Front left quarter", "view-front"},
        {"Front right quarter", "view-front"},
        {"Left wing", "view-wing"},
        {"Right wing", "view-wing"},
        {"Tail - chase", "view-tail"},
        {"Tail chase", "view-tail"},
        {"Nose - head on", "view-front"},
        {"Above - overview", "plane"},
        {"Rear left quarter", "view-tail"},
        {"Left window", "view-window"},
        {"Right cockpit", "view-cockpit"},
        {"Captain displays", "view-instrument"},
        {"Center displays", "view-instrument"},
        {"Main panel overview", "view-instrument"},
        {"Glareshield", "view-controls"},
        {"Overhead", "view-controls"},
        {"Pedestal", "view-controls"},
        {"Left side controls", "view-controls"},
        {"Flight instruments", "view-instrument"},
        {"Radios and navigation", "view-instrument"},
        {"Lower controls", "view-controls"},
    };
    const auto it = icons.find(name);
    return it == icons.end() ? "camera" : it->second;
}
}  // namespace

json makeStarterProfile(const std::string& name, const std::string& modelKey, const std::string& displayName) {
    const std::string cat = newUuid(), cam = newUuid();
    return {
        {"schemaVersion", kSchemaVersion},
        {"profileId", newUuid()},
        {"revision", 0},
        {"name", name},
        {"aircraftMatch", {{"strategy", modelKey.empty() ? "none" : "aircraftPath"},
                           {"modelKey", modelKey.empty() ? json(nullptr) : json(modelKey)},
                           {"variantKey", nullptr},
                           {"displayName", displayName.empty() ? json(nullptr) : json(displayName)},
                           {"confirmed", false}}},
        {"calibration", starterCalibration()},
        {"requiredSchemaFeatures", json::array()},
        {"defaultCameraId", cam},
        {"categories", json::array({{{"id", cat}, {"name", "Cockpit"}, {"icon", "view-cockpit"}, {"cameraIds", json::array({cam})}}})},
        {"cameras", json::array({{{"id", cam},
                                  {"categoryId", cat},
                                  {"name", "Pilot view"},
                                  {"icon", "view-cockpit"},
                                  {"mode", "aircraftCockpit"},
                                  {"requiredCapabilities", {"cockpitPose", "interaction", "fov"}},
                                  {"viewId", 1},
                                  {"favorite", true},
                                  {"positionMetersFRD", {0.0, 0.0, 0.0}},
                                  {"orientationCameraToAnchorXYZW", {0.0, 0.0, 0.0, 1.0}},
                                  {"projection", {{"kind", "simulatorFovRadians"}, {"value", 62.0 * core::kDegToRad}, {"axis", "adapterDefined"}}},
                                  {"transition", {{"kind", "smoothstep3"}, {"durationSeconds", core::kDefaultTransitionSeconds}, {"delaySeconds", 0.0}}},
                                  {"interaction", true},
                                  {"trackingPolicy", "nativeIfQualified"},
                                  {"effects", {{"enabled", false}}},
                                  {"temporaryLookPolicy", "resetOnRecall"},
                                  {"levelHorizon", false},
                                  {"seedDefinitionName", nullptr}}})},
        {"sequences", json::array()}};
}

bool isC172(const std::string& modelKey, const std::string& displayName) {
    std::string text = modelKey + " " + displayName;
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text.find("c172") != std::string::npos || text.find("c-172") != std::string::npos ||
           text.find("cessna 172") != std::string::npos;
}

json makeC172Profile(const std::string& modelKey, const std::string& displayName) {
    json doc = makeStarterProfile("Cessna 172 - Complete views", modelKey, displayName);
    doc["builtinTemplate"] = "c172-complete-v1";
    doc["categories"] = json::array();
    doc["cameras"] = json::array();
    const json cameraTemplate = makeStarterProfile("", "", "")["cameras"][0];
    int viewId = 0;
    auto category = [&](const std::string& name) {
        std::string id = newUuid();
        doc["categories"].push_back({{"id", id}, {"name", name}, {"icon", builtinViewIcon(name)}, {"cameraIds", json::array()}});
        return id;
    };
    auto view = [&](const std::string& group, const char* name, double f, double r, double d, double pitch,
                    double heading, double fov, bool favorite = false, bool exterior = false) {
        json cam = cameraTemplate;
        const std::string id = newUuid();
        const auto q = core::Quat::fromPbh({pitch, 0, heading});
        cam["id"] = id;
        cam["categoryId"] = group;
        cam["name"] = name;
        cam["icon"] = builtinViewIcon(name);
        cam["viewId"] = ++viewId;
        cam["favorite"] = favorite;
        cam["positionMetersFRD"] = {f, r, d};
        cam["orientationCameraToAnchorXYZW"] = {q.x, q.y, q.z, q.w};
        cam["projection"]["value"] = fov * core::kDegToRad;
        cam["transition"]["durationSeconds"] = exterior ? 1.2 : 0.55;
        cam["mode"] = exterior ? "aircraftExterior" : "aircraftCockpit";
        cam["interaction"] = !exterior;
        cam["requiredCapabilities"] = exterior ? json({"fov"}) : json({"cockpitPose", "interaction", "fov"});
        doc["cameras"].push_back(cam);
        for (auto& cat : doc["categories"]) if (cat["id"] == group) cat["cameraIds"].push_back(id);
        if (viewId == 1) doc["defaultCameraId"] = id;
    };
    auto group = category("Flight deck");
    view(group, "Pilot - normal", 0, 0, 0, 0, 0, 62, true);
    view(group, "Pilot - landing", 0.08, 0, -0.08, -3, 0, 58, true);
    view(group, "Pilot - wide cockpit", -0.06, 0.12, -0.04, -8, 0, 82);
    view(group, "Copilot", 0, 0.74, 0, 0, 0, 62);
    group = category("Instruments");
    view(group, "G1000 - PFD", 0.38, 0.02, 0.08, -12, 0, 35, true);
    view(group, "G1000 - MFD", 0.38, 0.38, 0.08, -12, 0, 35, true);
    view(group, "G1000 - full panel", 0.16, 0.30, 0.10, -14, 0, 68);
    view(group, "Standby instruments", 0.30, 0.16, 0.18, -22, 0, 40);
    group = category("Controls");
    view(group, "Throttle, mixture and trim", 0.18, 0.34, 0.30, -48, 0, 48, true);
    view(group, "Electrical and lighting", 0.14, -0.12, 0.24, -38, -15, 46);
    view(group, "Overhead and cabin lighting", -0.05, 0.30, -0.18, 40, 0, 66);
    group = category("Cabin");
    view(group, "Rear seats - forward", -0.95, 0.35, -0.02, -5, 0, 80);
    group = category("Exterior (Preview)");
    auto outside = [&](const char* name, double f, double r, double d, double fov, bool favorite = false) {
        const double heading = std::atan2(-r, -f) * core::kRadToDeg;
        const double pitch = std::atan2(d, std::hypot(f, r)) * core::kRadToDeg;
        view(group, name, f, r, d, pitch, heading, fov, favorite, true);
    };
    outside("Front left quarter", 7, -7, -2.8, 55, true);
    outside("Front right quarter", 7, 7, -2.8, 55);
    outside("Left wing", 0, -10, -2.2, 55);
    outside("Right wing", 0, 10, -2.2, 55);
    outside("Tail - chase", -11, 0, -3, 55, true);
    outside("Nose - head on", 10, 0, -1.8, 50);
    outside("Above - overview", -4, 0, -13, 60);
    outside("Rear left quarter", -8, -7, -3, 55);
    return doc;
}

bool isA320(const std::string& modelKey, const std::string& displayName) {
    // Package paths and titles vary (A320_NEO, A32NX, FNX_320); punctuation is not identity.
    std::string text;
    for (unsigned char c : modelKey + " " + displayName)
        if (std::isalnum(c)) text.push_back(static_cast<char>(std::tolower(c)));
    for (const char* other : {"a318", "a319", "a321", "fnx319", "fnx321"})
        if (text.find(other) != std::string::npos) return false;
    return text.find("a320") != std::string::npos || text.find("a32nx") != std::string::npos ||
           text.find("fnx320") != std::string::npos;
}

json makeA320Profile(const std::string& modelKey, const std::string& displayName) {
    json doc = makeStarterProfile("Airbus A320 - Starter views", modelKey, displayName);
    doc["builtinTemplate"] = "a320-eyepoint-v1";
    doc["categories"] = json::array();
    doc["cameras"] = json::array();
    const json cameraTemplate = makeStarterProfile("", "", "")["cameras"][0];
    int viewId = 0;
    auto category = [&](const char* name, const char* color) {
        const auto id = newUuid();
        doc["categories"].push_back({{"id", id}, {"name", name}, {"icon", builtinViewIcon(name)}, {"color", color}, {"cameraIds", json::array()}});
        return id;
    };
    auto view = [&](const std::string& group, const char* name, double pitch, double heading, double fov, bool favorite = false) {
        json cam = cameraTemplate;
        const auto id = newUuid();
        const auto q = core::Quat::fromPbh({pitch, 0, heading});
        cam["id"] = id;
        cam["categoryId"] = group;
        cam["name"] = name;
        cam["icon"] = builtinViewIcon(name);
        cam["viewId"] = ++viewId;
        cam["favorite"] = favorite;
        // No cross-model cockpit coordinates: all views start at this model's pilot eyepoint.
        cam["positionMetersFRD"] = {0.0, 0.0, 0.0};
        cam["orientationCameraToAnchorXYZW"] = {q.x, q.y, q.z, q.w};
        cam["projection"]["value"] = fov * core::kDegToRad;
        cam["transition"]["durationSeconds"] = 0.55;
        doc["cameras"].push_back(cam);
        for (auto& cat : doc["categories"]) if (cat["id"] == group) {
            if (cat["cameraIds"].empty()) cat["defaultCameraId"] = id;
            cat["cameraIds"].push_back(id);
        }
        if (viewId == 1) doc["defaultCameraId"] = id;
    };
    auto group = category("Flight deck", "#75baff");
    view(group, "Pilot - normal", 0, 0, 62, true);
    view(group, "Pilot - landing", -3, 0, 52, true);
    view(group, "Pilot - wide cockpit", -8, 0, 85);
    view(group, "Left window", 0, -70, 62);
    view(group, "Right cockpit", -5, 65, 72);
    group = category("Instruments", "#f5a56b");
    view(group, "Captain displays", -24, 0, 48, true);
    view(group, "Center displays", -23, 28, 48, true);
    view(group, "Main panel overview", -20, 15, 80);
    group = category("Controls", "#87d5a0");
    view(group, "Glareshield", -8, 23, 55, true);
    view(group, "Overhead", 55, 25, 75);
    view(group, "Pedestal", -58, 38, 65);
    view(group, "Left side controls", -40, -60, 60);
    return doc;
}

std::vector<std::pair<std::string, std::string>> reassignIds(json& doc) {
    std::map<std::string, std::string> remap;
    auto mapId = [&](json& v) {
        if (!v.is_string()) return;
        auto& slot = remap[v.get<std::string>()];
        if (slot.empty()) slot = newUuid();
        v = slot;
    };
    if (!doc.is_object()) return {};
    doc["profileId"] = newUuid();
    doc["revision"] = 0;
    if (doc.contains("cameras") && doc["cameras"].is_array())
        for (auto& cam : doc["cameras"]) {
            if (!cam.is_object()) continue;
            if (cam.contains("id")) mapId(cam["id"]);
            if (cam.contains("categoryId")) mapId(cam["categoryId"]);
        }
    if (doc.contains("categories") && doc["categories"].is_array())
        for (auto& cat : doc["categories"]) {
            if (!cat.is_object()) continue;
            if (cat.contains("id")) mapId(cat["id"]);
            if (cat.contains("defaultCameraId")) mapId(cat["defaultCameraId"]);
            if (cat.contains("cameraIds") && cat["cameraIds"].is_array())
                for (auto& ref : cat["cameraIds"]) mapId(ref);
        }
    if (doc.contains("defaultCameraId")) mapId(doc["defaultCameraId"]);
    return {remap.begin(), remap.end()};
}

}  // namespace fscs::profiles
