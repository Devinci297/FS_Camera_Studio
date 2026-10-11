#pragma once
// Basis conversion between the canonical FRD frame and the simulator camera API (design §7.1). Axis order and
// angle signs are an explicit, versioned contract that the conformance tool qualifies per simulator build —
// never a hard-coded swap.

#include "fscs/core/engine.hpp"

#include <nlohmann/json.hpp>

#include <array>
#include <optional>
#include <string>

namespace fscs::host {

enum class Referential { Eyepoint, SimObject, Datum, World };

// Backend-neutral camera data as the SDK sees it: meters, PBH degrees, FOV radians.
struct SdkPose {
    double x = 0, y = 0, z = 0;
    double pitchDeg = 0, bankDeg = 0, headingDeg = 0;
    double fovRad = 1.0;
    Referential referential = Referential::Eyepoint;
    bool trackUserAircraft = false;
};

struct AdapterContract {
    std::string id = "msfs2024-eyepoint-default-v1";
    // For each canonical axis (forward, right, down) the SDK component index (0=x, 1=y, 2=z) and sign.
    std::array<int, 3> axisIndex{2, 0, 1};
    std::array<double, 3> axisSign{1.0, 1.0, -1.0};
    double pitchSign = -1.0, bankSign = 1.0, headingSign = 1.0;

    bool valid() const;
    nlohmann::json toJson() const;
    static std::optional<AdapterContract> fromJson(const nlohmann::json& j);
};

SdkPose toSdk(const core::CameraPose& pose, const AdapterContract& c, Referential ref);
core::CameraPose fromSdk(const SdkPose& sdk, const AdapterContract& c);

// One qualification record per simulator build, written by the in-sim qualification checks (doc/qualification.md).
struct Qualification {
    bool adapterQualified = false;
    bool captureQualified = false;
    AdapterContract contract;
    std::string simVersion;
};

// Picks the record for `simVersion` from the stored qualification document ({"records": [...]});
// unqualified defaults otherwise.
Qualification qualificationFor(const nlohmann::json& doc, const std::string& simVersion);

}  // namespace fscs::host
