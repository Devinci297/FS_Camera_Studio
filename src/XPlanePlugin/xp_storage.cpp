#include "xp_storage.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <fstream>
#include <system_error>

namespace fscs::xplane {

namespace fs = std::filesystem;

fs::path pathFromUtf8(const std::string& utf8) {
    return fs::path(std::u8string(reinterpret_cast<const char8_t*>(utf8.data()), utf8.size()));
}

std::optional<fs::path> XPStorage::resolve(const std::string& workPath) const {
    std::string rest;
    for (const char* prefix : {"\\work\\", "/work/"}) {
        const std::string p = prefix;
        if (workPath.rfind(p, 0) == 0) rest = workPath.substr(p.size());
    }
    if (rest.empty()) return std::nullopt;
    fs::path out = root_;
    std::string part;
    auto push = [&]() -> bool {
        if (part.empty()) return true;
        if (part == "." || part == ".." || part.find(':') != std::string::npos) return false;
        out /= pathFromUtf8(part);
        part.clear();
        return true;
    };
    for (char c : rest) {
        if (c == '\\' || c == '/') {
            if (!push()) return std::nullopt;
        } else {
            part.push_back(c);
        }
    }
    if (!push() || out == root_) return std::nullopt;
    return out;
}

namespace {

bool writeWhole(const fs::path& path, const std::string& bytes) {
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) return false;
    file.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    file.flush();
    const bool ok = static_cast<bool>(file);
    file.close();
    return ok && !file.fail();
}

}  // namespace

void XPStorage::listImportFiles(std::function<void(std::vector<std::string>, std::string)> done) {
    const auto folder = resolve(profiles::kProfileImportFolder);
    std::error_code ec;
    if (!folder || (!fs::create_directories(*folder, ec) && ec)) {
        done({}, "Could not create the imports folder. Create it at the displayed location, then Refresh.");
        return;
    }
    std::vector<std::string> names;
    std::string error;
    for (fs::directory_iterator it(*folder, ec), end; !ec && it != end; it.increment(ec)) {
        if (!it->is_regular_file(ec)) continue;
        const std::u8string name8 = it->path().filename().u8string();
        const std::string name(name8.begin(), name8.end());
        if (profiles::validProfileImportFilename(name)) names.push_back(name);
        if (names.size() >= 1000) { error = "Showing the first 1,000 profile files. Remove unused files to see more."; break; }
    }
    if (ec && error.empty()) error = "X-Plane could not finish reading the imports folder. Refresh to try again.";
    std::sort(names.begin(), names.end());
    done(std::move(names), std::move(error));
}

std::string utf8Of(const fs::path& path) {
    const std::u8string s = path.u8string();
    return std::string(s.begin(), s.end());
}

namespace {

std::string lowerText(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}
std::string lowerName(const fs::path& p) { return lowerText(utf8Of(p.filename())); }
bool endsWith(const std::string& s, const std::string& tail) {
    return s.size() >= tail.size() && s.compare(s.size() - tail.size(), tail.size(), tail) == 0;
}
bool startsWith(const std::string& s, const std::string& head) { return s.rfind(head, 0) == 0; }

std::vector<fs::path> entries(const fs::path& dir, bool directories) {
    std::vector<fs::path> out;
    std::error_code ec;
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        std::error_code tc;
        if (directories ? it->is_directory(tc) : it->is_regular_file(tc)) out.push_back(it->path());
        if (out.size() >= 20000) break;
    }
    std::sort(out.begin(), out.end());
    return out;
}

constexpr std::size_t kMaxXCameraFiles = 500;
constexpr std::uintmax_t kMaxAcfBytes = 256u << 20;

}  // namespace

std::string XPStorage::importFolderLabel() const {
    if (const auto folder = resolve(profiles::kProfileImportFolder)) return utf8Of(fs::path(*folder).make_preferred());
    return IStorageDriver::importFolderLabel();
}

void XPStorage::listXCameraFiles(std::function<void(std::vector<profiles::xcamera::File>)> done) {
    xcamera_.clear();
    if (xplaneRoot_.empty()) {
        done({});
        return;
    }
    // The loaded aircraft: the plugin reports its .acf relative to the X-Plane folder.
    fs::path loadedAcf;
    if (loadedAircraft_) {
        const std::string rel = loadedAircraft_();
        if (!rel.empty()) {
            const fs::path p = pathFromUtf8(rel);
            loadedAcf = (p.is_absolute() ? p : xplaneRoot_ / p).lexically_normal();
        }
    }
    const std::string loadedFolder = loadedAcf.empty() ? std::string() : utf8Of(loadedAcf.parent_path().filename());
    const std::string loadedStem = loadedAcf.empty() ? std::string() : lowerText(utf8Of(loadedAcf.stem()));
    const std::string loadedDir = loadedAcf.empty() ? std::string() : lowerText(utf8Of(loadedAcf.parent_path()));

    struct Found {
        profiles::xcamera::File file;
        XCameraEntry entry;
    };
    std::vector<Found> found;
    auto add = [&](const fs::path& csv, const fs::path& acf, const char* source, const std::string& aircraft, bool loaded) {
        if (found.size() >= kMaxXCameraFiles) return;
        Found f;
        f.file.label = utf8Of(csv.filename());
        f.file.aircraft = aircraft;
        f.file.source = source;
        f.file.loadedAircraft = loaded;
        f.entry.csv = csv;
        f.entry.acf = acf;
        f.entry.aircraft = aircraft;
        f.entry.modelKey = lowerText(aircraft);  // the folder of the .acf, as Coordinator's aircraftFromPath keys it
        found.push_back(std::move(f));
    };

    // Each aircraft's own X-Camera file, beside its .acf.
    auto scan = [&](const fs::path& dir) {
        const std::string name = utf8Of(dir.filename());
        if (name.empty() || name[0] == '.') return;
        std::vector<fs::path> acfs, csvs;
        for (const auto& file : entries(dir, false)) {
            const std::string n = lowerName(file);
            if (endsWith(n, ".acf")) acfs.push_back(file);
            else if (startsWith(n, "x-camera_") && endsWith(n, ".csv")) csvs.push_back(file);
        }
        for (const auto& csv : csvs) {
            const std::string n = lowerName(csv);
            const std::string stem = n.substr(9, n.size() - 9 - 4);
            fs::path acf;
            for (const auto& a : acfs)
                if (lowerText(utf8Of(a.stem())) == stem) acf = a;
            if (acf.empty() && !acfs.empty()) acf = acfs.front();
            const bool loaded = !loadedDir.empty() && lowerText(utf8Of(dir.lexically_normal())) == loadedDir &&
                                (acf.empty() || lowerText(utf8Of(acf.stem())) == loadedStem || acfs.size() == 1);
            add(csv, acf, "aircraft", name, loaded);
        }
    };
    for (const auto& d1 : entries(xplaneRoot_ / "Aircraft", true)) {
        const std::string n1 = utf8Of(d1.filename());
        if (n1.empty() || n1[0] == '.') continue;
        scan(d1);
        for (const auto& d2 : entries(d1, true)) scan(d2);
    }
    if (!loadedAcf.empty()) {
        // Community sets for the loaded aircraft: "<id>_X-Camera_<acf name>.csv".
        const std::string tail = "x-camera_" + loadedStem + ".csv";
        for (const auto& csv : entries(xplaneRoot_ / "Resources" / "plugins" / "X-Camera" / "CommunityAircraftCameras", false))
            if (endsWith(lowerName(csv), tail)) add(csv, loadedAcf, "community", loadedFolder, true);
    }
    // .csv files the user copied into the imports folder: for the loaded aircraft.
    if (const auto folder = resolve(profiles::kProfileImportFolder))
        for (const auto& csv : entries(*folder, false))
            if (endsWith(lowerName(csv), ".csv")) add(csv, loadedAcf, "imports", loadedFolder, !loadedAcf.empty());

    std::stable_sort(found.begin(), found.end(), [](const Found& a, const Found& b) {
        if (a.file.loadedAircraft != b.file.loadedAircraft) return a.file.loadedAircraft;
        return lowerText(a.file.aircraft) < lowerText(b.file.aircraft);
    });
    std::vector<profiles::xcamera::File> out;
    for (auto& f : found) {
        f.file.id = "x" + std::to_string(out.size() + 1);
        xcamera_[f.file.id] = std::move(f.entry);
        out.push_back(std::move(f.file));
    }
    done(std::move(out));
}

void XPStorage::readXCameraFile(const std::string& id,
                                std::function<void(std::optional<profiles::xcamera::Source>, std::string)> done) {
    const auto it = xcamera_.find(id);
    if (it == xcamera_.end()) {
        done(std::nullopt, "That X-Camera file is no longer listed. Refresh and choose it again.");
        return;
    }
    const XCameraEntry& e = it->second;
    std::error_code ec;
    const auto size = fs::file_size(e.csv, ec);
    if (ec || size > profiles::xcamera::kMaxCsvBytes) {
        done(std::nullopt, ec ? "That X-Camera file is missing. Refresh and choose it again." : "That X-Camera file is larger than 4 MiB.");
        return;
    }
    profiles::xcamera::Source source;
    {
        std::ifstream file(e.csv, std::ios::binary);
        source.csv.assign(static_cast<std::size_t>(size), '\0');
        file.read(source.csv.data(), static_cast<std::streamsize>(source.csv.size()));
        if (!file || file.gcount() != static_cast<std::streamsize>(source.csv.size())) {
            done(std::nullopt, "X-Plane could not read that X-Camera file.");
            return;
        }
    }
    // The eyepoint cockpit views are measured against, from the aircraft's .acf (text: one property per line).
    if (!e.acf.empty() && fs::file_size(e.acf, ec) <= kMaxAcfBytes && !ec) {
        std::ifstream acf(e.acf, std::ios::binary);
        profiles::xcamera::AcfEyepoint eye;
        std::string line;
        while (std::getline(acf, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (eye.feed(line)) break;
        }
        source.eyepointBody = eye.result();
    }
    source.modelKey = e.modelKey;
    source.displayName = e.aircraft;
    source.fileName = utf8Of(e.csv.filename());
    done(std::move(source), {});
}

void XPStorage::writeFile(const std::string& path, std::string bytes, std::function<void(bool, std::string)> done) {
    // Readable per-aircraft mirrors ride along with a store slot write, as in the MSFS module.
    const bool store = path == "\\work\\fscs-store-a.json" || path == "\\work\\fscs-store-b.json";
    if (store) {
        auto exports = std::move(exports_);
        exports_.clear();
        for (const auto& [profilePath, profileBytes] : exports) {
            const auto target = resolve(profilePath);
            if (!target || !writeWhole(*target, profileBytes)) {
                done(false, "X-Plane could not write the aircraft's profile file.");
                return;
            }
        }
    }
    const auto target = resolve(path);
    if (!target) {
        done(false, "Camera Studio tried to write outside its folder.");
        return;
    }
    const bool ok = writeWhole(*target, bytes);
    done(ok, ok ? std::string() : "X-Plane could not write Camera Studio's save file.");
}

void XPStorage::readFile(const std::string& path, std::function<void(std::optional<std::string>)> done) {
    const auto target = resolve(path);
    std::error_code ec;
    if (!target || !fs::is_regular_file(*target, ec)) {
        done(std::nullopt);  // a missing slot is normal on first run
        return;
    }
    const auto size = fs::file_size(*target, ec);
    if (ec || size > profiles::kMaxStoreBytes) {
        done(std::nullopt);
        return;
    }
    std::ifstream file(*target, std::ios::binary);
    std::string bytes(static_cast<std::size_t>(size), '\0');
    file.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    if (!file || file.gcount() != static_cast<std::streamsize>(bytes.size())) {
        done(std::nullopt);
        return;
    }
    done(std::move(bytes));
}

}  // namespace fscs::xplane
