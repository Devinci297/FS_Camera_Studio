#pragma once
// ICameraBackend over the MSFS 2024 WASM Camera API (MSFS_Camera.h). The only component that writes a camera pose.
// Status callbacks and synchronous reads are staged as BackendEvents and delivered on the next dispatch(), so the
// coordinator never runs inside an SDK callback.

#include "backend.hpp"

#include <vector>

namespace fscs::host {

class WasmCamera final : public ICameraBackend {
public:
    WasmCamera() = default;
    ~WasmCamera() override { disconnect(); }

    bool tryConnect() override;
    void disconnect() override;
    bool isOpen() const override { return subscribed_; }
    void dispatch(const std::function<void(BackendEvent)>& sink) override;

    bool requestStatus() override;
    bool acquire(const std::string& clientId) override;
    bool release(const std::string& definitionName) override;
    bool submitPose(const SdkPose& pose) override;
    bool setFlag(CameraFlag flag, bool enabled) override;
    bool requestCameraData(Referential ref) override;
    bool enumerateDefinitions() override;
    bool setUsingDefinition(const std::string& name) override;
    std::optional<bool> headTracking() override;
    bool setHeadTracking(bool enabled) override;

    void stage(BackendEvent ev) { staged_.push_back(std::move(ev)); }

private:
    bool subscribed_ = false;
    std::optional<SdkPose> submitted_;
    bool interactionKnown_ = false, interactionOn_ = false;
    std::vector<BackendEvent> staged_;
};

}  // namespace fscs::host
