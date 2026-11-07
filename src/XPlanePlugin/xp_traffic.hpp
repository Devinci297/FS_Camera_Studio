#pragma once
// XP Camera Studio: other aircraft for spotting, from X-Plane's TCAS target arrays (sim/cockpit2/tcas/targets/...).
// X-Plane fills them with its own AI traffic, and multiplayer and traffic plugins write their aircraft there too.
// Index 0 is the user's aircraft. Targets are keyed by their mode S id, which stays with an aircraft while the
// array index can change as aircraft come and go.

#include "backend.hpp"

#include <XPLMDataAccess.h>
#include <XPLMScenery.h>

#include <cstdint>
#include <functional>
#include <utility>
#include <vector>

namespace fscs::xplane {

class TrafficSource {
public:
    using Stage = std::function<void(host::BackendEvent)>;
    explicit TrafficSource(Stage stage) : stage_(std::move(stage)) {}
    ~TrafficSource() { destroy(); }
    TrafficSource(const TrafficSource&) = delete;
    TrafficSource& operator=(const TrafficSource&) = delete;

    // One Traffic event with every target within the radius of the user's aircraft. False without TCAS datarefs.
    bool poll(double radiusMeters);
    // Per-frame TrafficTarget events for this aircraft (0 stops them).
    bool watch(std::uint32_t objectId);
    void frame();
    void destroy();
    std::size_t lastCount() const { return lastCount_; }

private:
    static constexpr int kMax = 64;
    struct Arrays {
        float x[kMax], y[kMax], z[kMax], psi[kMax], the[kMax], phi[kMax], vx[kMax], vy[kMax], vz[kMax];
        int id[kMax], wheels[kMax];
        int count = 0;
    };
    bool find();
    bool read(Arrays& a);
    std::uint32_t objectId(const Arrays& a, int i) const;
    double groundAltitude(float x, float y, float z);

    Stage stage_;
    bool searched_ = false, available_ = false;
    XPLMDataRef num_ = nullptr, x_ = nullptr, y_ = nullptr, z_ = nullptr, psi_ = nullptr, the_ = nullptr, phi_ = nullptr;
    XPLMDataRef vx_ = nullptr, vy_ = nullptr, vz_ = nullptr, id_ = nullptr, wheels_ = nullptr, flightId_ = nullptr,
                type_ = nullptr;
    XPLMProbeRef probe_ = nullptr;
    std::uint32_t watched_ = 0;
    std::size_t lastCount_ = 0;
};

}  // namespace fscs::xplane
