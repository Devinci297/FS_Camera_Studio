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
}
