#include "wasm_storage.hpp"

#include "fscs/profiles/json_util.hpp"

#include <cstdio>
#include <cerrno>
#if !defined(_WIN32)
#include <dirent.h>
#include <sys/stat.h>
#endif
#include <algorithm>

namespace fscs::host {

namespace {
bool writeLocal(const std::string& path, const std::string& bytes) {
    FILE* file = std::fopen(path.c_str(), "wb");
    if (!file) return false;
    const bool written = std::fwrite(bytes.data(), 1, bytes.size(), file) == bytes.size();
    const bool flushed = std::fflush(file) == 0;
    const bool closed = std::fclose(file) == 0;
    return written && flushed && closed;
}
}

// Local persistent \work files, never streamed package assets. Asobo supports libc IO here.
// Close before invoking the callback: the repository may immediately read a written slot back.
void WasmStorage::listImportFiles(std::function<void(std::vector<std::string>, std::string)> done) {
#if defined(_WIN32)
    // Native unit-test builds have no simulator VFS. Module tests provide an explicit FakeStorage folder.
    done({}, "The simulator import folder is only available inside MSFS.");
#else
    // fd_readdir is supported by MSFS 2024. Never enumerate outside our own persistent work directory.
    const char* folder = "/work/imports";
    if (mkdir(folder, 0777) != 0 && errno != EEXIST) {
        done({}, "Could not create the imports folder. Create it at the displayed location, then Refresh.");
        return;
    }
    DIR* dir = opendir(folder);
    if (!dir) {
        done({}, "The simulator could not list the imports folder. Check the folder and refresh.");
        return;
    }
    std::vector<std::string> names;
    std::string error;
    errno = 0;
    while (auto* entry = readdir(dir)) {
        const std::string name = entry->d_name;
        if (profiles::validProfileImportFilename(name)) names.push_back(name);
        if (names.size() >= 1000) { error = "Showing the first 1,000 profile files. Remove unused files to see more."; break; }
        errno = 0;
    }
    if (errno != 0 && error.empty()) error = "The simulator could not finish reading the imports folder. Refresh to try again.";
    closedir(dir);
    std::sort(names.begin(), names.end());
    done(std::move(names), std::move(error));
#endif
}

void WasmStorage::writeFile(const std::string& path, std::string bytes, std::function<void(bool, std::string)> done) {
    // The repository prepares only affected aircraft/airport mirrors. Never parse the
    // full library and rewrite every profile during the simulator's frame callback.
    const bool store = path == "\\work\\fscs-store-a.json" || path == "\\work\\fscs-store-b.json";
    if (store) {
        auto exports = std::move(profileExports_);
        profileExports_.clear();
        for (const auto& [profilePath, profileBytes] : exports) {
            if (!writeLocal(profilePath, profileBytes)) {
                done(false, "The simulator could not write the aircraft's profile file.");
                return;
            }
        }
    }
    FILE* file = std::fopen(path.c_str(), "wb");
    if (!file) {
        done(false, "The simulator could not open Camera Studio's save file.");
        return;
    }
    const bool written = std::fwrite(bytes.data(), 1, bytes.size(), file) == bytes.size();
    const bool flushed = std::fflush(file) == 0;
    const bool closed = std::fclose(file) == 0;
    const bool ok = written && flushed && closed;
    done(ok, ok ? std::string() : "The simulator could not write Camera Studio's save file.");
}

void WasmStorage::readFile(const std::string& path, std::function<void(std::optional<std::string>)> done) {
    FILE* file = std::fopen(path.c_str(), "rb");
    if (!file) {
        done(std::nullopt);  // A missing slot is normal on first run.
        return;
    }
    std::optional<std::string> bytes;
    if (std::fseek(file, 0, SEEK_END) == 0) {
        const long size = std::ftell(file);
        if (size >= 0 && static_cast<unsigned long>(size) <= profiles::kMaxStoreBytes && std::fseek(file, 0, SEEK_SET) == 0) {
            bytes.emplace(static_cast<std::size_t>(size), '\0');
            if (std::fread(bytes->data(), 1, bytes->size(), file) != bytes->size() || std::ferror(file)) bytes.reset();
        }
    }
    if (std::fclose(file) != 0) bytes.reset();
    done(std::move(bytes));
}

}  // namespace fscs::host
