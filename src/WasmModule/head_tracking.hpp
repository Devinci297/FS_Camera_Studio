#pragma once
#include "backend.hpp"
#include <optional>

namespace fscs::host {
// TRACK IR ENABLE is global. Lease it while Camera Studio owns the view, using the shared host preference.
// Never synthesize head poses: MSFS adds the native tracking offset to the add-on camera.
class HeadTracking {
public:
    explicit HeadTracking(ICameraBackend& backend) : backend_(backend) {}
    void restore() {
        if (original_ && written_ && backend_.isOpen()) {
            auto current = backend_.headTracking();
            // Respect a change made externally since our last write.
            if (current && *current == *written_ && *current != *original_)
                if (!backend_.setHeadTracking(*original_)) error_ = "Could not restore simulator head tracking.";
        }
        original_.reset(); written_.reset(); observed_.reset(); key_.clear();
    }
    void update(const std::string& camera, const std::string& policy, double now) {
        const auto key = camera + ":" + policy;
        if (key != key_) {
            if (policy == "nativeIfQualified") restore();
            key_ = key; error_.clear();
            observed_ = backend_.headTracking();
            if (policy != "nativeIfQualified") {
                if (!observed_) error_ = "Simulator head tracking is unavailable.";
                else {
                    if (!original_) original_ = observed_;
                    const bool enabled = policy == "on";
                    if (*observed_ == enabled || backend_.setHeadTracking(enabled)) written_ = enabled;
                    else error_ = "The simulator rejected the head-tracking change.";
                }
            }
            nextRead_ = now; deadline_ = now + 1;
        }
        if (now >= nextRead_) {
            observed_ = backend_.headTracking(); nextRead_ = now + .25;
            if (error_.empty() && now >= deadline_ && written_ && observed_ != written_)
                error_ = "Simulator head tracking differs from the global preference.";
        }
    }
    bool nextEnabled() const { return !observed_.value_or(false); }
    nlohmann::json status() const {
        return {{"enabled", observed_ ? nlohmann::json(*observed_) : nlohmann::json(nullptr)},
                {"error", error_}, {"managed", original_.has_value()}};
    }
private:
    ICameraBackend& backend_;
    std::optional<bool> original_, written_, observed_;
    std::string key_, error_;
    double nextRead_ = 0, deadline_ = 0;
};
}
