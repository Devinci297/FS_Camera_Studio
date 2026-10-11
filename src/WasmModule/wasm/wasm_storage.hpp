#pragma once
// IStorageDriver over libc for local persistent \work files (never streamed package assets).

#include "fscs/profiles/repository.hpp"

namespace fscs::host {

class WasmStorage final : public profiles::IStorageDriver {
public:
    void prepareProfileExports(std::map<std::string, std::string> files) override { profileExports_ = std::move(files); }
    void listImportFiles(std::function<void(std::vector<std::string>, std::string)> done) override;
    void writeFile(const std::string& path, std::string bytes, std::function<void(bool, std::string)> done) override;
    void readFile(const std::string& path, std::function<void(std::optional<std::string>)> done) override;
private:
    std::map<std::string, std::string> profileExports_;
};

}  // namespace fscs::host
