#pragma once
// IStorageDriver over the MSFS 2024 asynchronous IO API (MSFS_IO.h) for the package's persistent \work folder.

#include "fscs/profiles/repository.hpp"

namespace fscs::host {

class WasmStorage final : public profiles::IStorageDriver {
public:
    void writeFile(const std::string& path, std::string bytes, std::function<void(bool, std::string)> done) override;
    void readFile(const std::string& path, std::function<void(std::optional<std::string>)> done) override;
};

}  // namespace fscs::host
