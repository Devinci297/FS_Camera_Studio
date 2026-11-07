#pragma once
// Native keyboard preferences are changed only by the helper after the simulator exits.
#include <filesystem>
#include <functional>
#include <string>
#include <nlohmann/json.hpp>

namespace fscs::xplane::keys {
using json = nlohmann::json;
namespace fs = std::filesystem;
struct Chord { std::string key, modifiers; };
Chord chord(const json& input);
json inspect(const std::string& text, const std::string& command, const Chord* key = nullptr);
std::string edit(const std::string& text, const json& operation);
std::string read(const fs::path& path);
void writeAtomic(const fs::path& path, const std::string& bytes);
void safePath(const fs::path& path);
void applyPending(const fs::path& prefs, const std::function<bool()>& simulatorRunning);

class Service {
public:
    Service(fs::path prefs, fs::path helper, std::function<std::string()> aircraftName = {})
        : prefs_(std::move(prefs)), helper_(std::move(helper)), aircraftName_(std::move(aircraftName)) {}
    ~Service();
    std::string request(const std::string& request);
private:
    void ensureWorker();
    fs::path prefs_, helper_;
    std::function<std::string()> aircraftName_;
    void* worker_ = nullptr;
};
} // namespace fscs::xplane::keys
