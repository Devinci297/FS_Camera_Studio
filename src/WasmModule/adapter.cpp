#include "adapter.hpp"

#include "fscs/profiles/json_util.hpp"

#include <set>

namespace fscs::host {

using core::Pbh;
using core::Quat;
using core::Vec3;
using nlohmann::json;
namespace js = fscs::js;

bool AdapterContract::valid() const {
    const std::set<int> idx(axisIndex.begin(), axisIndex.end());
    if (idx != std::set<int>{0, 1, 2}) return false;
    for (double s : axisSign)
        if (s != 1.0 && s != -1.0) return false;
    for (double s : {pitchSign, bankSign, headingSign})
        if (s != 1.0 && s != -1.0) return false;
    return !id.empty();
}

json AdapterContract::toJson() const {
    return {{"id", id},
            {"axisIndex", axisIndex},
            {"axisSign", axisSign},
            {"pitchSign", pitchSign},
            {"bankSign", bankSign},
            {"headingSign", headingSign}};
}

std::optional<AdapterContract> AdapterContract::fromJson(const json& j) {
    const json& idx = js::get(j, "axisIndex");
    const json& sign = js::get(j, "axisSign");
    if (!idx.is_array() || idx.size() != 3 || !sign.is_array() || sign.size() != 3) return std::nullopt;
    AdapterContract c;
    c.id = js::str(j, "id");
    for (int i = 0; i < 3; ++i) {
        c.axisIndex[i] = static_cast<int>(js::integer(idx[i], -1));
        c.axisSign[i] = js::num(sign[i], 0.0);
    }
    c.pitchSign = js::num(j, "pitchSign", 0.0);
    c.bankSign = js::num(j, "bankSign", 0.0);
    c.headingSign = js::num(j, "headingSign", 0.0);
    if (!c.valid()) return std::nullopt;
    return c;
}

SdkPose toSdk(const core::CameraPose& pose, const AdapterContract& c, Referential ref) {
    SdkPose s;
    const double frd[3] = {pose.positionFRD.x, pose.positionFRD.y, pose.positionFRD.z};
    double out[3] = {0, 0, 0};
    for (int i = 0; i < 3; ++i) out[c.axisIndex[i]] = frd[i] * c.axisSign[i];
    s.x = out[0];
    s.y = out[1];
    s.z = out[2];
    const Pbh a = pose.orientation.toPbh();
    s.pitchDeg = a.pitchDeg * c.pitchSign;
    s.bankDeg = a.bankDeg * c.bankSign;
    s.headingDeg = a.headingDeg * c.headingSign;
    s.fovRad = pose.fovRad;
    s.referential = ref;
    return s;
}

core::CameraPose fromSdk(const SdkPose& s, const AdapterContract& c) {
    core::CameraPose p;
    const double in[3] = {s.x, s.y, s.z};
    double frd[3];
    for (int i = 0; i < 3; ++i) frd[i] = in[c.axisIndex[i]] * c.axisSign[i];
    p.positionFRD = {frd[0], frd[1], frd[2]};
    p.orientation = Quat::fromPbh({s.pitchDeg * c.pitchSign, s.bankDeg * c.bankSign, s.headingDeg * c.headingSign});
    p.fovRad = s.fovRad;
    return p;
}

Qualification qualificationFor(const json& doc, const std::string& simVersion) {
    Qualification q;
    q.simVersion = simVersion;
    for (const auto& r : js::array(doc, "records")) {
        if (js::str(r, "simVersion") != simVersion) continue;
        auto contract = AdapterContract::fromJson(js::get(r, "contract"));
        if (!contract) continue;
        const json& gates = js::object(r, "gates");
        q.contract = *contract;
        q.adapterQualified = js::str(gates, "G01") == "pass" && js::str(gates, "G03") == "pass";
        q.captureQualified = js::str(gates, "G02") == "pass";
    }
    return q;
}

}  // namespace fscs::host
