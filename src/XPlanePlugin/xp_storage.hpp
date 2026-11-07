#pragma once
// XP Camera Studio: IStorageDriver over plain files. The repository addresses the MSFS work folder ("\work\...");
// here that folder is <X-Plane>/Output/preferences/XPCameraStudio, which survives plugin updates. Paths are UTF-8.

#include "fscs/profiles/repository.hpp"

#include <filesystem>
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

    const std::filesystem::path& root() const { return root_; }
    // "\work\fscs-store-a.json" -> <root>/fscs-store-a.json. Empty for a path outside the work folder or one that
    // tries to leave it.
    std::optional<std::filesystem::path> resolve(const std::string& workPath) const;

private:
    std::filesystem::path root_;
    std::map<std::string, std::string> exports_;
};

std::filesystem::path pathFromUtf8(const std::string& utf8);

}  // namespace fscs::xplane
