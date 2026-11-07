#pragma once
// XP Camera Studio: IStorageDriver over plain files. The repository addresses the MSFS work folder ("\work\...");
// here that folder is <X-Plane>/Output/preferences/XPCameraStudio, which survives plugin updates. Paths are UTF-8.
//
// It also finds X-Camera's camera files for Profiles > Import: "X-Camera_<acf>.csv" beside each aircraft in
// Aircraft/ (two folder levels deep), community sets in X-Camera's CommunityAircraftCameras folder named for the
// loaded aircraft's .acf, and .csv files in the imports folder (for the loaded aircraft). X-Camera's files are only
// read, never changed.

#include "fscs/profiles/repository.hpp"

#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <string>

namespace fscs::xplane {

class XPStorage final : public profiles::IStorageDriver {
public:
    explicit XPStorage(std::filesystem::path root) : root_(std::move(root)) {}

    void prepareProfileExports(std::map<std::string, std::string> files) override { exports_ = std::move(files); }
    void listImportFiles(std::function<void(std::vector<std::string>, std::string)> done) override;
    void writeFile(const std::string& path, std::string bytes, std::function<void(bool, std::string)> done) override;
    void readFile(const std::string& path, std::function<void(std::optional<std::string>)> done) override;

    std::string importFolderLabel() const override;
    void listXCameraFiles(std::function<void(std::vector<profiles::xcamera::File>)> done) override;
    void readXCameraFile(const std::string& id,
                         std::function<void(std::optional<profiles::xcamera::Source>, std::string)> done) override;
    // The X-Plane folder, and the user aircraft's .acf (as the plugin reports it: relative to that folder).
    void setXPlane(std::filesystem::path xplaneRoot, std::function<std::string()> loadedAircraft) {
        xplaneRoot_ = std::move(xplaneRoot);
        loadedAircraft_ = std::move(loadedAircraft);
    }

    const std::filesystem::path& root() const { return root_; }
    // "\work\fscs-store-a.json" -> <root>/fscs-store-a.json. Empty for a path outside the work folder or one that
    // tries to leave it.
    std::optional<std::filesystem::path> resolve(const std::string& workPath) const;

private:
    struct XCameraEntry {
        std::filesystem::path csv, acf;
        std::string modelKey, aircraft;
    };

    std::filesystem::path root_;
    std::map<std::string, std::string> exports_;
    std::filesystem::path xplaneRoot_;
    std::function<std::string()> loadedAircraft_;
    std::map<std::string, XCameraEntry> xcamera_;  // the latest listing, by id
};

std::filesystem::path pathFromUtf8(const std::string& utf8);
std::string utf8Of(const std::filesystem::path& path);

}  // namespace fscs::xplane
