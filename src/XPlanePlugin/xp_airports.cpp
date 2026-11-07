#include "xp_airports.hpp"

#include "xp_storage.hpp"
#include "fscs/profiles/airport.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <set>

namespace fscs::xplane::apt {

using nlohmann::json;
namespace fs = std::filesystem;
namespace airport = profiles::airport;

namespace {

constexpr double kFeet = 0.3048;
constexpr double kEarthRadius = 6371008.8;
constexpr double kDegToRad = 3.14159265358979323846 / 180.0;
constexpr std::size_t kMaxRunways = 100, kMaxParking = 3000, kMaxPoints = 10000, kMaxPaths = 20000, kMaxSigns = 2000;

std::FILE* openRead(const fs::path& path) {
#ifdef _WIN32
    return _wfopen(path.c_str(), L"rb");
#else
    return std::fopen(path.c_str(), "rb");
#endif
}

// Whitespace-separated fields of one line.
std::vector<std::string_view> fields(std::string_view line, std::size_t max = 64) {
    std::vector<std::string_view> out;
    std::size_t i = 0;
    while (i < line.size() && out.size() < max) {
        while (i < line.size() && std::isspace(static_cast<unsigned char>(line[i]))) ++i;
        if (i >= line.size()) break;
        const std::size_t start = i;
        while (i < line.size() && !std::isspace(static_cast<unsigned char>(line[i]))) ++i;
        out.push_back(line.substr(start, i - start));
    }
    return out;
}
// The text from field `from` to the end of the line (names have spaces).
std::string rest(std::string_view line, std::size_t from) {
    std::size_t i = 0;
    for (std::size_t n = 0; n < from; ++n) {
        while (i < line.size() && std::isspace(static_cast<unsigned char>(line[i]))) ++i;
        while (i < line.size() && !std::isspace(static_cast<unsigned char>(line[i]))) ++i;
    }
    while (i < line.size() && std::isspace(static_cast<unsigned char>(line[i]))) ++i;
    std::size_t end = line.size();
    while (end > i && std::isspace(static_cast<unsigned char>(line[end - 1]))) --end;
    return std::string(line.substr(i, end - i));
}
int rowCode(std::string_view line) {
    std::size_t i = 0;
    while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) ++i;
    int code = -1;
    const auto r = std::from_chars(line.data() + i, line.data() + line.size(), code);
    if (r.ec != std::errc() || (r.ptr != line.data() + line.size() && !std::isspace(static_cast<unsigned char>(*r.ptr)))) return -1;
    return code;
}
// from_chars: never the user's locale (strtod would read "47.5" as 47 under a comma-decimal locale).
double num(std::string_view s, double fallback = NAN) {
    if (!s.empty() && s.front() == '+') s.remove_prefix(1);
    double v = 0;
    const auto r = std::from_chars(s.data(), s.data() + s.size(), v);
    return r.ec == std::errc() ? v : fallback;
}
bool header(int code) { return code == 1 || code == 16 || code == 17; }
std::string upper(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}
std::string trim(std::string s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.pop_back();
    std::size_t i = 0;
    while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i;
    return s.substr(i);
}

double distanceMeters(double lat1, double lon1, double lat2, double lon2) {
    const double p1 = lat1 * kDegToRad, p2 = lat2 * kDegToRad, dp = p2 - p1, dl = (lon2 - lon1) * kDegToRad;
    const double a = std::sin(dp / 2) * std::sin(dp / 2) + std::cos(p1) * std::cos(p2) * std::sin(dl / 2) * std::sin(dl / 2);
    return 2 * kEarthRadius * std::atan2(std::sqrt(a), std::sqrt(1 - a));
}
double bearingDeg(double lat1, double lon1, double lat2, double lon2) {
    const double p1 = lat1 * kDegToRad, p2 = lat2 * kDegToRad, dl = (lon2 - lon1) * kDegToRad;
    const double b = std::atan2(std::sin(dl) * std::cos(p2), std::cos(p1) * std::sin(p2) - std::sin(p1) * std::cos(p2) * std::cos(dl));
    return std::fmod(b / kDegToRad + 360.0, 360.0);
}
bool validLatLon(double lat, double lon) { return std::isfinite(lat) && std::isfinite(lon) && std::abs(lat) <= 90 && std::abs(lon) <= 180; }

// The header fields of the airport being indexed, finished at the next header.
struct Pending {
    Entry e;
    bool open = false, datumLat = false, datumLon = false;
    double sumLat = 0, sumLon = 0;
    int points = 0;
    void add(double lat, double lon) {
        if (!validLatLon(lat, lon)) return;
        sumLat += lat;
        sumLon += lon;
        ++points;
    }
};
void finish(Pending& p, std::vector<Entry>& out) {
    if (!p.open) return;
    p.open = false;
    if (!(p.datumLat && p.datumLon)) {
        if (p.points == 0) return;  // nowhere to put it
        p.e.lat = p.sumLat / p.points;
        p.e.lon = p.sumLon / p.points;
    }
    if (p.e.id.empty() || !validLatLon(p.e.lat, p.e.lon)) return;
    out.push_back(std::move(p.e));
}

// ICAO aerodrome reference code (1301 width code) to a stand radius, meters: half the largest wingspan.
double radiusForCode(char code) {
    switch (code) {
    case 'A': return 7.5;
    case 'B': return 12;
    case 'C': return 18;
    case 'D': return 26;
    case 'E': return 32.5;
    case 'F': return 40;
    default: return NAN;
    }
}
double taxiwayWidth(std::string_view type) {
    // taxiway_A .. taxiway_F: ICAO taxiway widths per code letter; plain "taxiway" is a medium one.
    if (type.size() == 9 && type.substr(0, 8) == "taxiway_") {
        switch (type[8]) {
        case 'A': return 7.5;
        case 'B': return 10.5;
        case 'C': return 15;
        case 'D': return 18;
        case 'E': return 23;
        case 'F': return 25;
        default: break;
        }
    }
    return 15;
}

}  // namespace

// ---- LineReader ------------------------------------------------------------------------------------------------

LineReader::LineReader(const fs::path& path, std::uint64_t offset) : buffer_(1 << 20) {
    file_ = openRead(path);
    if (!file_) return;
#ifdef _WIN32
    if (offset && _fseeki64(file_, static_cast<long long>(offset), SEEK_SET) != 0) {
#else
    if (offset && fseeko(file_, static_cast<off_t>(offset), SEEK_SET) != 0) {
#endif
        std::fclose(file_);
        file_ = nullptr;
        return;
    }
    bufferOffset_ = offset;
}
LineReader::~LineReader() {
    if (file_) std::fclose(file_);
}
bool LineReader::next(std::string_view& line, std::uint64_t& offset) {
    if (!file_) return false;
    carry_.clear();
    std::uint64_t start = bufferOffset_ + begin_;
    for (;;) {
        if (begin_ < end_) {
            const char* data = buffer_.data();
            const void* nl = std::memchr(data + begin_, '\n', end_ - begin_);
            if (nl) {
                const std::size_t at = static_cast<const char*>(nl) - data;
                offset = start;
                if (carry_.empty()) {
                    line = std::string_view(data + begin_, at - begin_);
                } else {
                    carry_.append(data + begin_, at - begin_);
                    line = carry_;
                }
                begin_ = at + 1;
                if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
                return true;
            }
            carry_.append(buffer_.data() + begin_, end_ - begin_);
        }
        if (eof_) {
            begin_ = end_;
            if (carry_.empty()) return false;
            offset = start;
            line = carry_;
            if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
            eof_ = true;
            return true;
        }
        bufferOffset_ += end_;
        begin_ = 0;
        end_ = std::fread(buffer_.data(), 1, buffer_.size(), file_);
        if (end_ < buffer_.size()) eof_ = true;
    }
}

// ---- Files -----------------------------------------------------------------------------------------------------

std::vector<fs::path> sceneryFiles(const fs::path& root) {
    std::vector<fs::path> out;
    std::error_code ec;
    const fs::path global = root / "Global Scenery" / "Global Airports" / "Earth nav data" / "apt.dat";
    bool globalListed = false;
    auto addPack = [&](const fs::path& pack) {
        const fs::path file = pack / "Earth nav data" / "apt.dat";
        if (fs::is_regular_file(file, ec) && std::find(out.begin(), out.end(), file) == out.end()) out.push_back(file);
    };
    std::ifstream ini(root / "Custom Scenery" / "scenery_packs.ini", std::ios::binary);
    if (ini) {
        std::string line;
        while (std::getline(ini, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            static const std::string kPack = "SCENERY_PACK ";
            if (line.rfind(kPack, 0) != 0) continue;  // SCENERY_PACK_DISABLED and the header
            std::string where = trim(line.substr(kPack.size()));
            if (where == "*GLOBAL_AIRPORTS*") {
                if (!globalListed && fs::is_regular_file(global, ec)) out.push_back(global);
                globalListed = true;
                continue;
            }
            if (where.empty() || where.front() == '*') continue;
            fs::path pack = pathFromUtf8(where);
            if (pack.is_relative()) pack = root / pack;
            addPack(pack);
        }
    } else {
        // No list yet (a fresh install): the folders as X-Plane would first sort them.
        std::vector<fs::path> packs;
        for (fs::directory_iterator it(root / "Custom Scenery", ec), end; !ec && it != end; it.increment(ec))
            if (it->is_directory(ec)) packs.push_back(it->path());
        std::sort(packs.begin(), packs.end());
        for (const auto& p : packs) addPack(p);
    }
    if (!globalListed && fs::is_regular_file(global, ec)) out.push_back(global);
    return out;
}

void index(const fs::path& path, std::uint32_t file, std::vector<Entry>& out) {
    LineReader reader(path);
    if (!reader.ok()) return;
    Pending p;
    std::string_view line;
    std::uint64_t offset = 0;
    while (reader.next(line, offset)) {
        const int code = rowCode(line);
        if (code < 0) continue;
        if (header(code)) {
            finish(p, out);
            const auto f = fields(line, 5);
            if (f.size() < 5) continue;
            p = Pending{};
            p.open = true;
            p.e.id = upper(std::string(f[4]));
            p.e.name = rest(line, 5);
            p.e.elevation = num(f[1], 0) * kFeet;
            p.e.kind = code;
            p.e.file = file;
            p.e.offset = offset;
            continue;
        }
        if (code == 99) break;
        if (!p.open) continue;
        if (code == 1302) {
            const auto f = fields(line, 3);
            if (f.size() < 3) continue;
            if (f[1] == "datum_lat") p.datumLat = validLatLon(p.e.lat = num(f[2]), 0);
            else if (f[1] == "datum_lon") p.datumLon = validLatLon(0, p.e.lon = num(f[2]));
            else if (f[1] == "region_code") p.e.region = upper(std::string(f[2]));
        } else if (code == 100) {
            const auto f = fields(line, 20);
            if (f.size() >= 20) p.add(num(f[9]), num(f[10])), p.add(num(f[18]), num(f[19]));
        } else if (code == 101) {
            const auto f = fields(line, 9);
            if (f.size() >= 9) p.add(num(f[4]), num(f[5])), p.add(num(f[7]), num(f[8]));
        } else if (code == 102) {
            const auto f = fields(line, 4);
            if (f.size() >= 4) p.add(num(f[2]), num(f[3]));
        }
    }
    finish(p, out);
}

std::string signLabel(std::string_view text) {
    // The front of the sign only ("front|back"); {..} are colour and glyph instructions, the rest is printed.
    const auto bar = text.find('|');
    if (bar != std::string_view::npos) text = text.substr(0, bar);
    std::string out;
    bool inBrace = false;
    for (const char c : text) {
        if (c == '{') {
            inBrace = true;
            if (!out.empty() && out.back() != ' ') out.push_back(' ');
        } else if (c == '}') {
            inBrace = false;
        } else if (!inBrace) {
            out.push_back(c);
        }
    }
    std::string squeezed;
    for (const char c : out)
        if (!(c == ' ' && (squeezed.empty() || squeezed.back() == ' '))) squeezed.push_back(c);
    return trim(squeezed);
}

json facility(const fs::path& path, const Entry& entry, bool summary) {
    LineReader reader(path, entry.offset);
    if (!reader.ok()) return nullptr;
    const airport::Geo origin{entry.lat, entry.lon, entry.elevation};
    auto local = [&](double lat, double lon) { return airport::local(origin, {lat, lon, entry.elevation}); };
    json f = {{"icao", entry.id}, {"lat", entry.lat}, {"lon", entry.lon}, {"altitude", entry.elevation},
              {"name", entry.name.empty() ? entry.id : entry.name}, {"runways", json::array()}, {"parking", json::array()}};
    if (!summary) {
        f["points"] = json::array();
        f["paths"] = json::array();
        f["signs"] = json::array();
    }
    std::map<std::string, std::size_t> nodes;  // 1201 node id -> points index
    std::set<std::string> keys;
    bool haveStartup = false;
    std::vector<json> oldStartup;  // row 15, used only when there is no 1300
    auto addStand = [&](double lat, double lon, double heading, const std::string& xpType, const std::string& types,
                        std::string name, std::vector<json>& into) {
        if (!validLatLon(lat, lon) || into.size() >= kMaxParking) return;
        const bool gate = xpType == "gate";
        const bool heavy = types.find("heavy") != std::string::npos, jets = types.find("jets") != std::string::npos;
        // MSFS parking types the module knows: 8-10 gates by size, 1 a GA ramp, 2 a small GA ramp (tie-down).
        const int type = gate ? (heavy ? 10 : jets ? 9 : 8) : xpType == "tie_down" ? 2 : 1;
        const std::uint32_t number = static_cast<std::uint32_t>(into.size() + 1);
        name = trim(name);
        if (name.empty()) name = (gate ? "Gate " : "Parking ") + std::to_string(number);
        std::string key = "xp:" + name;
        for (int n = 2; keys.count(key); ++n) key = "xp:" + name + "#" + std::to_string(n);
        keys.insert(key);
        const auto at = local(lat, lon);
        json stand = {{"name", name.substr(0, 60)}, {"key", key}, {"gate", gate}, {"type", type}, {"number", number},
                      {"nameCode", 0}, {"suffixCode", 0}, {"heading", std::isfinite(heading) ? heading : 0.0},
                      {"radius", heavy ? 32.5 : jets ? 18.0 : 9.0}, {"east", at.y}, {"north", at.x}};
        into.push_back(std::move(stand));
    };
    std::vector<json> parking;
    std::string_view line;
    std::uint64_t offset = 0;
    bool first = true;
    while (reader.next(line, offset)) {
        const int code = rowCode(line);
        if (first) {
            first = false;
            continue;  // the airport's own header
        }
        if (header(code) || code == 99) break;
        if (code == 100 && f["runways"].size() < kMaxRunways) {
            const auto r = fields(line, 20);
            if (r.size() < 20) continue;
            const double lat1 = num(r[9]), lon1 = num(r[10]), lat2 = num(r[18]), lon2 = num(r[19]);
            if (!validLatLon(lat1, lon1) || !validLatLon(lat2, lon2)) continue;
            f["runways"].push_back({{"lat", (lat1 + lat2) / 2}, {"lon", (lon1 + lon2) / 2}, {"altitude", entry.elevation},
                                    {"heading", bearingDeg(lat1, lon1, lat2, lon2)}, {"length", distanceMeters(lat1, lon1, lat2, lon2)},
                                    {"width", num(r[1], 30)}, {"primary", std::string(r[8])}, {"secondary", std::string(r[17])}});
        } else if (code == 101 && f["runways"].size() < kMaxRunways) {
            const auto r = fields(line, 9);
            if (r.size() < 9) continue;
            const double lat1 = num(r[4]), lon1 = num(r[5]), lat2 = num(r[7]), lon2 = num(r[8]);
            if (!validLatLon(lat1, lon1) || !validLatLon(lat2, lon2)) continue;
            f["runways"].push_back({{"lat", (lat1 + lat2) / 2}, {"lon", (lon1 + lon2) / 2}, {"altitude", entry.elevation},
                                    {"heading", bearingDeg(lat1, lon1, lat2, lon2)}, {"length", distanceMeters(lat1, lon1, lat2, lon2)},
                                    {"width", num(r[1], 30)}, {"primary", std::string(r[3])}, {"secondary", std::string(r[6])}});
        } else if (code == 14) {
            // The tower viewpoint: height above the field, feet.
            const auto r = fields(line, 4);
            if (r.size() < 4 || f.contains("tower")) continue;
            const double lat = num(r[1]), lon = num(r[2]), height = num(r[3], 0);
            if (validLatLon(lat, lon) && (lat != 0 || lon != 0))
                f["tower"] = {{"lat", lat}, {"lon", lon}, {"altitude", entry.elevation + height * kFeet}};
        } else if (code == 1300) {
            const auto r = fields(line, 6);
            if (r.size() < 6) continue;
            haveStartup = true;
            addStand(num(r[1]), num(r[2]), num(r[3]), std::string(r[4]), std::string(r[5]), rest(line, 6), parking);
        } else if (code == 1301) {
            // Metadata for the stand before it: its ICAO size code sets the radius.
            const auto r = fields(line, 2);
            if (r.size() >= 2 && r[1].size() == 1 && !parking.empty()) {
                const double radius = radiusForCode(r[1][0]);
                if (std::isfinite(radius)) parking.back()["radius"] = radius;
            }
        } else if (code == 15) {
            const auto r = fields(line, 4);
            if (r.size() >= 4) addStand(num(r[1]), num(r[2]), num(r[3]), "misc", "", rest(line, 4), oldStartup);
        } else if (summary) {
            continue;
        } else if (code == 1201 && f["points"].size() < kMaxPoints) {
            const auto r = fields(line, 5);
            if (r.size() < 5) continue;
            const double lat = num(r[1]), lon = num(r[2]);
            if (!validLatLon(lat, lon) || nodes.count(std::string(r[4]))) continue;
            const auto at = local(lat, lon);
            nodes[std::string(r[4])] = f["points"].size();
            f["points"].push_back({{"east", at.y}, {"north", at.x}});
        } else if (code == 1202 && f["paths"].size() < kMaxPaths) {
            const auto r = fields(line, 5);
            if (r.size() < 5) continue;
            const auto a = nodes.find(std::string(r[1])), b = nodes.find(std::string(r[2]));
            if (a == nodes.end() || b == nodes.end()) continue;
            // MSFS path types: 1 taxi, 2 runway.
            const bool runway = r[4] == "runway";
            const bool taxi = r[4].substr(0, 7) == "taxiway";
            if (!runway && !taxi) continue;
            f["paths"].push_back({{"type", runway ? 2 : 1}, {"start", a->second}, {"end", b->second},
                                  {"width", runway ? 45.0 : taxiwayWidth(r[4])}});
        } else if (code == 20 && f["signs"].size() < kMaxSigns) {
            const auto r = fields(line, 7);
            if (r.size() < 7) continue;
            const double lat = num(r[1]), lon = num(r[2]), heading = num(r[3], 0);
            const std::string label = signLabel(rest(line, 6));
            if (!validLatLon(lat, lon) || label.empty()) continue;
            char key[48];
            std::snprintf(key, sizeof(key), "xp:%.6f,%.6f", lat, lon);
            // X-Plane gives the heading of someone reading the sign; the generator stands 90 degrees off its value,
            // so this puts the camera in front of the sign, looking at it.
            f["signs"].push_back({{"lat", lat}, {"lon", lon}, {"altitude", entry.elevation},
                                  {"heading", std::fmod(heading + 90.0, 360.0)}, {"key", key}, {"label", "Sign " + label}});
        }
    }
    // Older scenery: row-15 startup locations, when there are no 1300 rows.
    f["parking"] = haveStartup ? json(parking) : json(oldStartup);
    return f;
}

// ---- Database --------------------------------------------------------------------------------------------------

void Database::start(fs::path root) {
    stop();
    stopping_ = false;
    ready_ = false;
    files_.clear();
    entries_.clear();
    byId_.clear();
    worker_ = std::thread([this, root = std::move(root)] { run(root); });
}

void Database::stop() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopping_ = true;
    }
    wake_.notify_all();
    if (worker_.joinable()) worker_.join();
}

void Database::run(fs::path root) {
    files_ = sceneryFiles(root);
    for (std::uint32_t i = 0; i < files_.size() && !stopping_; ++i) index(files_[i], i, entries_);
    // The first file to list an airport wins: Custom Scenery before Global Airports.
    for (std::size_t i = 0; i < entries_.size(); ++i) byId_.emplace(entries_[i].id, i);
    ready_ = true;
    for (;;) {
        Job job;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            wake_.wait(lock, [this] { return stopping_ || !jobs_.empty(); });
            if (stopping_) return;
            job = std::move(jobs_.front());
            jobs_.pop_front();
        }
        Result r;
        r.requestId = job.requestId;
        const auto it = byId_.find(upper(job.code));
        if (it == byId_.end()) {
            r.error = "No airport " + job.code + " in X-Plane's scenery.";
        } else {
            const Entry& e = entries_[it->second];
            r.facility = facility(files_[e.file], e, job.summary);
            if (!r.facility.is_object()) r.error = "Could not read the scenery data for " + job.code + ".";
        }
        std::lock_guard<std::mutex> lock(mutex_);
        results_.push_back(std::move(r));
    }
}

json Database::nearby(double lat, double lon, double radiusNm, std::size_t limit) const {
    json out = json::array();
    if (!ready() || !validLatLon(lat, lon)) return out;
    std::vector<std::pair<double, std::size_t>> found;
    const double radius = radiusNm * 1852.0;
    // A quick latitude cut before the distance.
    const double latSpan = radius / 111000.0 + 0.1;
    for (const auto& [id, i] : byId_) {
        const Entry& e = entries_[i];
        if (e.kind == 17 || std::abs(e.lat - lat) > latSpan) continue;
        const double d = distanceMeters(lat, lon, e.lat, e.lon);
        if (d <= radius) found.emplace_back(d, i);
    }
    std::sort(found.begin(), found.end());
    if (found.size() > limit) found.resize(limit);
    for (const auto& [d, i] : found) {
        const Entry& e = entries_[i];
        out.push_back({{"icao", e.id}, {"region", e.region}, {"lat", e.lat}, {"lon", e.lon}, {"altitude", e.elevation},
                       {"name", e.name}, {"facilityIdent", e.id}});
    }
    return out;
}

void Database::request(std::string code, std::uint32_t requestId, bool summary) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        jobs_.push_back({std::move(code), requestId, summary});
    }
    wake_.notify_one();
}

std::vector<Database::Result> Database::take() {
    std::lock_guard<std::mutex> lock(mutex_);
    auto out = std::move(results_);
    results_.clear();
    return out;
}

}  // namespace fscs::xplane::apt
