#include "fscs/profiles/profile.hpp"

#include "fscs/profiles/json_util.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include <sstream>

#if defined(_WIN32)
#include <windows.h>
#include <bcrypt.h>
#pragma comment(lib, "bcrypt.lib")
#else
#include <unistd.h>
#endif

namespace fscs::profiles {

namespace {

using core::Quat;

const std::set<std::string> kKnownFeatures = {};  // schema 2 defines no optional required features yet

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

bool modeActivatable(CameraMode m) { return m == CameraMode::AircraftCockpit || m == CameraMode::AircraftExterior; }

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
    if (!doc.contains("revision") || !doc["revision"].is_number_integer()) doc["revision"] = 0;
    if (doc["revision"].get<std::int64_t>() < 0) c.error("$.revision", "must not be negative");

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
            const std::int64_t vid = js::integer(cam, "viewId", 0);
            if (vid < 0 || vid > kMaxViewId) c.error(p + ".viewId", "must be 0-20");
            else if (vid > 0 && !viewIds.insert(vid).second) c.error(p + ".viewId", "view ID " + std::to_string(vid) + " is used twice");
            cam["viewId"] = vid;
            const std::pair<const char*, json> defaults[] = {
                {"favorite", false},          {"interaction", true},  {"trackingPolicy", "nativeIfQualified"},
                {"temporaryLookPolicy", "resetOnRecall"}, {"levelHorizon", false}, {"seedDefinitionName", nullptr},
                {"effects", {{"enabled", false}}}, {"requiredCapabilities", json::array()}};
            for (const auto& [k, v] : defaults)
                if (!cam.contains(k)) cam[k] = v;
            const std::string look = js::str(cam, "temporaryLookPolicy");
            if (look != "resetOnRecall" && look != "retain") c.error(p + ".temporaryLookPolicy", "unknown look policy");
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
        e.interaction = js::boolean(cam, "interaction", true);
        e.retainLook = js::str(cam, "temporaryLookPolicy") == "retain";
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
    auto* p = static_cast<unsigned char*>(out);
    while (n > 0) {
        const std::size_t chunk = n > 256 ? 256 : n;  // getentropy's documented limit
        if (getentropy(p, chunk) != 0) break;
        p += chunk;
        n -= chunk;
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
        {"categories", json::array({{{"id", cat}, {"name", "Cockpit"}, {"cameraIds", json::array({cam})}}})},
        {"cameras", json::array({{{"id", cam},
                                  {"categoryId", cat},
                                  {"name", "Pilot view"},
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
            if (cat.contains("cameraIds") && cat["cameraIds"].is_array())
                for (auto& ref : cat["cameraIds"]) mapId(ref);
        }
    if (doc.contains("defaultCameraId")) mapId(doc["defaultCameraId"]);
    return {remap.begin(), remap.end()};
}

}  // namespace fscs::profiles
