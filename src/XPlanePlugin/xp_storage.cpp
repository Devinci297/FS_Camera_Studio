#include "xp_storage.hpp"

#include <algorithm>
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
