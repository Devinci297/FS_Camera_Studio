#include "fscs/profiles/xcamera.hpp"

#include "fscs/core/engine.hpp"
#include "fscs/profiles/json_util.hpp"
#include "fscs/profiles/profile.hpp"

#include <algorithm>
#include <cmath>
#include <locale>
#include <map>
#include <set>
#include <sstream>

namespace fscs::profiles::xcamera {

namespace {

using core::Quat;
using core::Vec3;

constexpr double kFeetToMeters = 0.3048;
// A camera X-Camera shows from the cockpit but further than this from the pilot becomes an exterior view.
constexpr double kCockpitReachMeters = 20;

std::string trim(std::string_view s) {
    std::size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r' || s[a] == '\n')) ++a;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r' || s[b - 1] == '\n')) --b;
    return std::string(s.substr(a, b - a));
}

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::vector<std::string> split(std::string_view line) {
    std::vector<std::string> out;
    std::size_t start = 0;
    for (std::size_t i = 0; i <= line.size(); ++i)
        if (i == line.size() || line[i] == ',') {
            out.emplace_back(line.substr(start, i - start));
            start = i + 1;
        }
    return out;
}

// Locale-independent: X-Camera always writes '.' decimals.
std::optional<double> number(const std::string& text) {
    const std::string t = trim(text);
    if (t.empty()) return std::nullopt;
    std::istringstream in(t);
    in.imbue(std::locale::classic());
    double v = 0;
    in >> v;
    if (in.fail() || !std::isfinite(v)) return std::nullopt;
    return v;
}

// Older X-Camera files and hand-edited ones are Windows-1252 text; Latin-1 covers the names they use.
std::string toUtf8(const std::string& s) {
    if (js::validUtf8(s)) return s;
    std::string out;
    out.reserve(s.size() + s.size() / 8);
    for (unsigned char c : s) {
        if (c < 0x80) out.push_back(static_cast<char>(c));
        else {
            out.push_back(static_cast<char>(0xC0 | (c >> 6)));
            out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
        }
    }
    return out;
}

// At most `limit` bytes without splitting a UTF-8 sequence.
std::string clip(std::string s, std::size_t limit) {
    if (s.size() <= limit) return s;
    std::size_t n = limit;
    while (n > 0 && (static_cast<unsigned char>(s[n]) & 0xC0) == 0x80) --n;
    s.resize(n);
    return trim(s);
}

double wrap(double deg) {
    double d = std::fmod(deg, 360.0);
    if (d > 180) d -= 360;
    if (d <= -180) d += 360;
    return d;
}

constexpr Vec3 bodyToFrd(const Vec3& b) { return {-b.z, b.x, -b.y}; }

// Heading and pitch (degrees, FRD) looking along `d`.
void aimAlong(const Vec3& d, double& headingDeg, double& pitchDeg) {
    headingDeg = std::atan2(d.y, d.x) * core::kRadToDeg;
    pitchDeg = std::atan2(-d.z, std::hypot(d.x, d.y)) * core::kRadToDeg;
}

json quat(const Quat& q) { return {q.x, q.y, q.z, q.w}; }

class Table {
public:
    explicit Table(const std::vector<std::string>& header) {
        for (std::size_t i = 0; i < header.size(); ++i) columns_[lower(trim(header[i]))] = i;
    }
    bool has(const char* name) const { return columns_.count(name) != 0; }
    std::size_t size() const { return columns_.size(); }
    std::string text(const std::vector<std::string>& row, const char* name) const {
        const auto it = columns_.find(name);
        return it == columns_.end() || it->second >= row.size() ? std::string() : trim(row[it->second]);
    }
    double num(const std::vector<std::string>& row, const char* name, double fallback) const {
        return number(text(row, name)).value_or(fallback);
    }
    bool flag(const std::vector<std::string>& row, const char* name) const {
        const std::string t = lower(text(row, name));
        return t == "true" || t == "1";
    }

private:
    std::map<std::string, std::size_t> columns_;
};

struct Point {
    Vec3 body;  // X-Camera axes: x right, y up, z aft
    double headingDeg = 0, pitchDeg = 0, rollDeg = 0;
    double seconds = 0;
};

struct Camera {
    std::string category, name;
    long long viewId = 0;
    bool external = false, smooth = false, retainLook = false, headShake = false, level = false;
    bool trackHeading = false, trackPitch = false, worldFixed = false;
    Vec3 target;  // what a tracking camera looks at (CG offsets), body axes
    double fovDeg = 60, zoom = 1;
    std::vector<Point> path;
};

// "Orbit 1 - 0" -> "Orbit 1": X-Camera numbers the points of a path.
std::string pathName(const std::string& name) {
    const auto dash = name.rfind(" - ");
    if (dash == std::string::npos || dash + 3 >= name.size()) return name;
    for (std::size_t i = dash + 3; i < name.size(); ++i)
        if (!std::isdigit(static_cast<unsigned char>(name[i]))) return name;
    return trim(name.substr(0, dash));
}

struct Orbit {
    double radius = 0, height = 0, duration = 0;
    bool clockwise = true;
};

// A path that goes (almost) all the way round the aircraft at a steady distance.
std::optional<Orbit> asOrbit(const std::vector<Point>& path) {
    if (path.size() < 6) return std::nullopt;
    std::vector<double> radii;
    for (const auto& p : path) radii.push_back(std::hypot(p.body.x, p.body.z));
    std::vector<double> sorted = radii;
    std::sort(sorted.begin(), sorted.end());
    const double r = sorted[sorted.size() / 2];
    if (r < 2) return std::nullopt;
    std::size_t steady = 0;
    for (double v : radii) steady += std::abs(v - r) <= 0.2 * r;
    if (steady * 10 < path.size() * 8) return std::nullopt;
    double total = 0, seconds = 0;
    int forward = 0, backward = 0;
    for (std::size_t i = 1; i < path.size(); ++i) {
        const double a0 = std::atan2(path[i - 1].body.x, -path[i - 1].body.z) * core::kRadToDeg;
        const double a1 = std::atan2(path[i].body.x, -path[i].body.z) * core::kRadToDeg;
        const double d = wrap(a1 - a0);
        total += d;
        if (d > 0.01) ++forward;
        if (d < -0.01) ++backward;
        seconds += std::max(0.0, path[i].seconds);
    }
    if (std::abs(total) < 270) return std::nullopt;
    if (std::min(forward, backward) * 5 > std::max(forward, backward)) return std::nullopt;
    double height = 0;
    for (const auto& p : path) height += p.body.y;
    height /= static_cast<double>(path.size());
    Orbit o;
    o.radius = core::clamp(r, 2, 5000);
    o.height = core::clamp(height, 1, 2000);
    o.duration = core::clamp(seconds > 0 ? seconds * 360 / std::abs(total) : 30, 5, 600);
    o.clockwise = total > 0;
    return o;
}

struct Flyby {
    double radius = 0, side = 0, height = 0, duration = 0;
};

// A path that starts well ahead of the aircraft and ends behind it, mostly along its track.
std::optional<Flyby> asFlyby(const std::vector<Point>& path) {
    if (path.size() < 2 || path.size() > 6) return std::nullopt;
    const Vec3 a = bodyToFrd(path.front().body), b = bodyToFrd(path.back().body);
    const double along = b.x - a.x, across = b.y - a.y;
    if (along > -50 || std::abs(along) < 2 * std::abs(across)) return std::nullopt;
    double height = 0, seconds = 0;
    for (const auto& p : path) height += p.body.y;
    for (std::size_t i = 1; i < path.size(); ++i) seconds += std::max(0.0, path[i].seconds);
    Flyby f;
    f.radius = core::clamp(std::abs(along) / 2, 2, 5000);
    f.side = core::clamp((a.y + b.y) / 2, -5000, 5000);
    f.height = core::clamp(height / static_cast<double>(path.size()), 1, 2000);
    f.duration = core::clamp(seconds > 0 ? seconds : 30, 5, 600);
    return f;
}

std::string plural(std::size_t n, const char* one, const char* many) {
    return std::to_string(n) + " " + (n == 1 ? one : many);
}

}  // namespace

bool AcfEyepoint::feed(std::string_view line) {
    auto value = [&](std::string_view key, double& out) {
        if (line.size() <= key.size() || line.compare(0, key.size(), key) != 0 || line[key.size()] != ' ') return;
        if (auto v = number(std::string(line.substr(key.size() + 1)))) out = *v;
    };
    if (line.size() > 2 && line[0] == 'P' && line[1] == ' ') {
        value("P acf/_pe_xyz/0", pe_[0]);
        value("P acf/_pe_xyz/1", pe_[1]);
        value("P acf/_pe_xyz/2", pe_[2]);
        value("P acf/_cgY", cgY_);
        value("P acf/_cgZ", cgZ_);
    }
    return result().has_value();
}

std::optional<core::Vec3> AcfEyepoint::result() const {
    for (double v : {pe_[0], pe_[1], pe_[2], cgY_, cgZ_})
        if (!std::isfinite(v)) return std::nullopt;
    return Vec3{pe_[0] * kFeetToMeters, (pe_[1] - cgY_) * kFeetToMeters, (pe_[2] - cgZ_) * kFeetToMeters};
}

std::optional<Result> convert(const Source& source, std::string* error) {
    auto fail = [&](const std::string& why) -> std::optional<Result> {
        if (error) *error = why;
        return std::nullopt;
    };
    if (source.csv.size() > kMaxCsvBytes) return fail("This X-Camera file is larger than 4 MiB.");
    std::string text = toUtf8(source.csv);
    if (text.size() >= 3 && text.compare(0, 3, "\xEF\xBB\xBF") == 0) text.erase(0, 3);

    std::vector<std::string> lines;
    {
        std::size_t start = 0;
        for (std::size_t i = 0; i <= text.size(); ++i)
            if (i == text.size() || text[i] == '\n') {
                std::string line = trim(std::string_view(text).substr(start, i - start));
                if (!line.empty()) lines.push_back(std::move(line));
                start = i + 1;
            }
    }
    if (lines.empty()) return fail("This X-Camera file is empty.");
    const auto header = split(lines[0]);
    const Table t(header);
    for (const char* need : {"category name", "camera name", "x", "y", "z", "heading", "pitch"})
        if (!t.has(need)) return fail("This is not an X-Camera camera file (its first line has no \"" + std::string(need) + "\" column).");
    const std::size_t nameColumn = [&] {
        for (std::size_t i = 0; i < header.size(); ++i)
            if (lower(trim(header[i])) == "camera name") return i;
        return std::size_t{1};
    }();

    // Rows, grouped into cameras: the hidden rows after a camera are the rest of its path.
    std::vector<Camera> cameras;
    std::size_t unreadable = 0;
    for (std::size_t li = 1; li < lines.size(); ++li) {
        auto row = split(lines[li]);
        // X-Camera does not quote: a comma in a camera name adds fields, which belong to the name.
        if (row.size() > header.size() && nameColumn + 1 < row.size()) {
            const std::size_t extra = row.size() - header.size();
            for (std::size_t k = 0; k < extra; ++k) row[nameColumn] += "," + row[nameColumn + 1 + k];
            row.erase(row.begin() + static_cast<std::ptrdiff_t>(nameColumn + 1), row.begin() + static_cast<std::ptrdiff_t>(nameColumn + 1 + extra));
        }
        const auto x = number(t.text(row, "x")), y = number(t.text(row, "y")), z = number(t.text(row, "z"));
        const auto heading = number(t.text(row, "heading")), pitch = number(t.text(row, "pitch"));
        if (!x || !y || !z || !heading || !pitch || std::abs(*x) > core::kMaxPositionMeters ||
            std::abs(*y) > core::kMaxPositionMeters || std::abs(*z) > core::kMaxPositionMeters) {
            ++unreadable;
            continue;
        }
        Point p{{*x, *y, *z}, *heading, *pitch, t.num(row, "roll", 0), t.num(row, "transition seconds", 0)};
        if (t.flag(row, "hidden camera") && !cameras.empty()) {
            cameras.back().path.push_back(p);
            continue;
        }
        Camera c;
        c.category = t.text(row, "category name");
        c.name = t.text(row, "camera name");
        const double vid = t.num(row, "view id", 0);
        c.viewId = vid >= 1 && vid <= profiles::kMaxViewId ? static_cast<long long>(vid) : 0;
        c.external = t.flag(row, "enable external camera");
        c.smooth = t.flag(row, "smooth transition");
        c.retainLook = t.flag(row, "retainmouselookposition");
        c.headShake = t.flag(row, "headshake");
        c.level = t.flag(row, "enable level camera");
        c.trackHeading = t.flag(row, "enable track heading");
        c.trackPitch = t.flag(row, "enable track pitch");
        c.target = {t.num(row, "cgx offset", 0), t.num(row, "cgy offset", 0), t.num(row, "cgz offset", 0)};
        c.worldFixed = std::abs(t.num(row, "latitude", 0)) > 1e-9 || std::abs(t.num(row, "longitude", 0)) > 1e-9 ||
                       t.num(row, "camera origin", 0) != 0;
        const double defaultFov = t.num(row, "default fov", 0);
        const double fov = t.num(row, "fov", 0);
        c.fovDeg = t.flag(row, "enable fov") && fov > 1 && fov < 170 ? fov : defaultFov > 1 && defaultFov < 170 ? defaultFov : 60;
        const double zoom = t.num(row, "zoom", 1);
        c.zoom = zoom > 0.05 && zoom < 50 ? zoom : 1;
        c.path.push_back(p);
        cameras.push_back(std::move(c));
    }
    if (cameras.empty()) return fail("This X-Camera file has no cameras Camera Studio can read.");

    // The eyepoint cockpit views are measured from: the aircraft's, else X-Camera's own pilot view.
    std::optional<Vec3> eye = source.eyepointBody;
    std::string eyeFrom;
    if (!eye) {
        const Camera* pilot = nullptr;
        for (const auto& c : cameras)
            if (!c.external && !c.worldFixed && (!pilot || (c.viewId == 1 && pilot->viewId != 1))) pilot = &c;
        if (pilot) {
            eye = pilot->path.front().body;
            eyeFrom = pilot->name.empty() ? "its first cockpit camera" : "\"" + pilot->name + "\"";
        }
    }

    json doc = makeStarterProfile("", source.modelKey, source.displayName);
    std::string title = source.displayName;
    if (title.empty()) {
        title = source.fileName;
        const auto dot = title.rfind('.');
        if (dot != std::string::npos) title.resize(dot);
    }
    doc["name"] = clip(clip(title.empty() ? "Aircraft" : title, 47) + " - X-Camera", 60);
    doc["categories"] = json::array();
    doc["cameras"] = json::array();
    const json cameraTemplate = makeStarterProfile("", "", "")["cameras"][0];

    std::map<std::string, std::size_t> categoryIndex;
    std::set<long long> usedViewIds;
    std::size_t orbits = 0, flybys = 0, stillPaths = 0, worldFixed = 0, duplicateIds = 0, movedOutside = 0, overflow = 0;
    std::string defaultId, firstCockpitId, firstId;
    for (const auto& c : cameras) {
        if (c.worldFixed) {
            ++worldFixed;
            continue;
        }
        if (doc["cameras"].size() >= kMaxCameras) {
            ++overflow;
            continue;
        }
        json cam = cameraTemplate;
        const std::string id = newUuid();
        std::string name = trim(c.path.size() > 1 ? pathName(c.name) : c.name);
        if (name.empty()) name = "Camera " + std::to_string(doc["cameras"].size() + 1);
        cam["id"] = id;
        cam["name"] = clip(name, 60);
        cam.erase("icon");  // the editor picks one from the name and mode
        long long viewId = c.viewId;
        if (viewId > 0 && !usedViewIds.insert(viewId).second) {
            ++duplicateIds;
            viewId = 0;
        }
        cam["viewId"] = viewId;
        const double fovRad = core::clamp(2 * std::atan(std::tan(c.fovDeg * core::kDegToRad / 2) / c.zoom), core::kMinFovRad, core::kMaxFovRad);
        cam["projection"]["value"] = fovRad;
        cam["temporaryLookPolicy"] = c.retainLook ? "retain" : "resetOnRecall";
        cam["followHorizon"] = c.level ? 0.0 : 1.0;
        cam["effects"] = {{"enabled", c.headShake}};
        const double seconds = c.path.front().seconds;
        cam["transition"] = c.smooth ? json{{"kind", "smoothstep3"},
                                            {"durationSeconds", core::clamp(seconds > 0 ? seconds : core::kDefaultTransitionSeconds,
                                                                            core::kMinTransitionSeconds, core::kMaxTransitionSeconds)},
                                            {"delaySeconds", 0.0}}
                                     : json{{"kind", "cut"}, {"durationSeconds", 0.0}, {"delaySeconds", 0.0}};

        std::string mode;
        bool motion = false;
        if (const auto orbit = c.path.size() > 1 ? asOrbit(c.path) : std::nullopt) {
            mode = "orbit";
            motion = true;
            ++orbits;
            cam["motion"] = {{"radiusMeters", orbit->radius}, {"heightMeters", orbit->height}, {"durationSeconds", orbit->duration},
                             {"sideMeters", 35.0}, {"clockwise", orbit->clockwise}, {"repeat", false}};
            const Vec3 start{orbit->radius, 0, -orbit->height};
            double h = 0, p = 0;
            aimAlong(start * -1, h, p);
            cam["positionMetersFRD"] = {start.x, start.y, start.z};
            cam["orientationCameraToAnchorXYZW"] = quat(Quat::fromPbh({p, 0, h}));
        } else if (const auto flyby = c.path.size() > 1 ? asFlyby(c.path) : std::nullopt) {
            mode = "flyby";
            motion = true;
            ++flybys;
            cam["motion"] = {{"radiusMeters", flyby->radius}, {"heightMeters", flyby->height}, {"durationSeconds", flyby->duration},
                             {"sideMeters", flyby->side}, {"clockwise", true}, {"repeat", false}};
            const Vec3 start{flyby->radius, flyby->side, -flyby->height};
            double h = 0, p = 0;
            aimAlong(start * -1, h, p);
            cam["positionMetersFRD"] = {start.x, start.y, start.z};
            cam["orientationCameraToAnchorXYZW"] = quat(Quat::fromPbh({p, 0, h}));
        } else {
            if (c.path.size() > 1) ++stillPaths;
            const Point& pt = c.path.front();
            bool exterior = c.external || !eye;
            if (!exterior && (pt.body - *eye).length() > kCockpitReachMeters) {
                exterior = true;
                ++movedOutside;
            }
            const Vec3 frd = exterior ? bodyToFrd(pt.body) : bodyToFrd(pt.body - *eye);
            double heading = wrap(pt.headingDeg), pitch = wrap(pt.pitchDeg);
            if (exterior && (c.trackHeading || c.trackPitch)) {
                const Vec3 look = bodyToFrd(c.target) - bodyToFrd(pt.body);
                if (look.length() > 0.5) {
                    double h = 0, p = 0;
                    aimAlong(look, h, p);
                    if (c.trackHeading) heading = h;
                    if (c.trackPitch) pitch = p;
                }
            }
            mode = exterior ? "aircraftExterior" : "aircraftCockpit";
            cam["positionMetersFRD"] = {frd.x, frd.y, frd.z};
            cam["orientationCameraToAnchorXYZW"] = quat(Quat::fromPbh({pitch, wrap(pt.rollDeg), heading}));
        }
        cam["mode"] = mode;
        const bool cockpit = mode == "aircraftCockpit";
        cam["interaction"] = cockpit;
        cam["requiredCapabilities"] = cockpit ? json({"cockpitPose", "interaction", "fov"}) : motion ? json::array() : json({"fov"});
        cam["favorite"] = viewId > 0 || motion;

        std::string category = clip(trim(c.category), 60);
        if (category.empty()) category = "X-Camera";
        std::size_t catIndex = doc["categories"].size() - (doc["categories"].empty() ? 0 : 1);
        if (const auto found = categoryIndex.find(category); found != categoryIndex.end()) {
            catIndex = found->second;
        } else if (doc["categories"].size() < kMaxCategories) {  // beyond the limit, the rest join the last one
            catIndex = doc["categories"].size();
            categoryIndex.emplace(category, catIndex);
            doc["categories"].push_back({{"id", newUuid()}, {"name", category}, {"cameraIds", json::array()}});
        }
        auto& cat = doc["categories"][catIndex];
        cam["categoryId"] = cat["id"];
        cat["cameraIds"].push_back(id);
        if (cockpit && !cat.contains("icon")) cat["icon"] = "view-cockpit";
        doc["cameras"].push_back(std::move(cam));

        if (firstId.empty()) firstId = id;
        if (cockpit && firstCockpitId.empty()) firstCockpitId = id;
        if (cockpit && viewId == 1 && defaultId.empty()) defaultId = id;
    }
    if (doc["cameras"].empty()) return fail("Every camera in this X-Camera file is placed at a fixed spot in the world, which Camera Studio cannot import yet.");
    if (defaultId.empty()) defaultId = firstCockpitId.empty() ? firstId : firstCockpitId;
    doc["defaultCameraId"] = defaultId;
    for (auto& cam : doc["cameras"])
        if (cam["id"] == defaultId) cam["favorite"] = true;

    Result r;
    r.cameraCount = doc["cameras"].size();
    if (!eyeFrom.empty())
        r.notes.push_back("The aircraft's eyepoint was not found, so cockpit views are measured from " + eyeFrom + ".");
    if (orbits)
        r.notes.push_back(plural(orbits, "camera path that circles the aircraft became an orbit view. It keeps",
                                 "camera paths that circle the aircraft became orbit views. They keep") +
                          " the path's distance, height and lap time and start in front of the aircraft.");
    if (flybys)
        r.notes.push_back(plural(flybys, "fly-by path became a fly-by view", "fly-by paths became fly-by views") +
                          ": the camera waits ahead of the aircraft and watches it pass.");
    if (stillPaths)
        r.notes.push_back(plural(stillPaths, "other camera path (a walkaround or swoop) kept only its first point, as a still view.",
                                 "other camera paths (walkarounds and swoops) kept only their first point, as still views."));
    if (movedOutside)
        r.notes.push_back(plural(movedOutside, "cockpit camera sits more than 20 m from the pilot and became an exterior view.",
                                 "cockpit cameras sit more than 20 m from the pilot and became exterior views."));
    if (duplicateIds)
        r.notes.push_back(plural(duplicateIds, "view ID was used twice; the later camera has none.", "view IDs were used twice; the later cameras have none."));
    if (worldFixed) r.notes.push_back(plural(worldFixed, "camera fixed to a spot in the world was", "cameras fixed to a spot in the world were") + " left out.");
    if (overflow) r.notes.push_back(plural(overflow, "camera was", "cameras were") + " left out: a profile holds at most 500.");
    if (unreadable) r.notes.push_back(plural(unreadable, "row could not be read and was", "rows could not be read and were") + " skipped.");
    r.profile = std::move(doc);
    return r;
}

}  // namespace fscs::profiles::xcamera
