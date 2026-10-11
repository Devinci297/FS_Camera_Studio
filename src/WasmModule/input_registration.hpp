#pragma once
// Shared by the SimConnect adapter and its native registration regression tests.
#include <cstdint>
#include <string>
#include <utility>

namespace fscs::host {

constexpr std::uint32_t kPovCentered = 0xffffffffu;

// The SDK only promises a second hat event when an UpEventID is registered.
// Keep the down payload untouched (it carries the angle), and use a distinct
// release event so the neutral state never depends on an SDK-provided UpValue.
template<class MapInput>
auto registerPovInput(MapInput&& mapInput, const std::string& definition,
                      std::uint32_t downEvent, std::uint32_t releaseEvent) {
    return std::forward<MapInput>(mapInput)(definition, downEvent, 0u, releaseEvent, 0u);
}

inline std::uint32_t povInputValue(bool isRelease, std::uint32_t rawValue) {
    return isRelease ? kPovCentered : rawValue;
}

}  // namespace fscs::host
