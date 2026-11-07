#pragma once
#include "fscs/profiles/profile.hpp"
namespace fscs::profiles::airport {
struct Geo { double lat=0, lon=0, altitude=0; bool valid() const; };
Geo geo(const json& value);
json asJson(Geo p);
core::Vec3 local(Geo origin, Geo point); // north, east, down, meters
Geo world(Geo origin, core::Vec3 ned);
double distanceNm(Geo a, Geo b);
bool validCode(const std::string& code);
std::string stableId(const std::string& key);
json parkingIdentity(int type, std::uint32_t number, int name, int suffix);
// Facility document is adapter-neutral, in degrees/meters. Missing features remain missing.
json generate(const json& facility, const json& options, std::vector<std::string>& notes);
json merge(const json& saved, const json& generated, bool replaceGenerated);
json nearby(const std::vector<json>& profiles, Geo position, double radiusNm);
std::string choose(const json& candidates, const std::string& current, const std::string& pinned);
// Generated spotting views and the virtual tower fallback, in meters.
constexpr double kFenceBehindMeters = 150;          // behind the threshold, on the extended centerline side
constexpr double kFenceSideMeters = 80;             // to the right of the landing direction
constexpr std::size_t kMaxGeneratedCameras = 500; // leave capacity for personal views
constexpr double kApproachDistanceMeters = 1852;    // 1 NM out on the extended centerline
constexpr double kGlidepathDeg = 3;
constexpr double kApproachBelowGlidepathMeters = 90;
constexpr double kVirtualTowerAbeamMeters = 300;    // beside the longest runway's midpoint
constexpr std::int64_t kAutoParkingLimit = 4;       // gates/stands in an automatic set, chosen by spread
// Generation options for automatic (temporary) airport sets: tower, every runway end with spotting views
// and a few spread-out stands. No taxi intersections, so it stays cheap enough to run unprompted.
json automaticOptions(const json& categoryHeights);
// The facility fields automatic generation reads, for a per-session cache (drops taxi points and paths).
json compactFacility(const json& facility);
}
