#include "wasm_storage.hpp"

#include "fscs/profiles/json_util.hpp"

#include <cstdio>

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
void WasmStorage::writeFile(const std::string& path, std::string bytes, std::function<void(bool, std::string)> done) {
    // Publish readable aircraft files with each store save. The checksummed two-slot store remains the recovery
    // authority across profiles, settings and bindings; the individual files can also be imported as profile codes.
    const bool store = path == "\\work\\fscs-store-a.json" || path == "\\work\\fscs-store-b.json";
    if (store) {
        const auto envelope = js::parse(bytes);
        for (const auto& [profilePath, profileBytes] : profiles::aircraftProfileFiles(envelope)) {
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
