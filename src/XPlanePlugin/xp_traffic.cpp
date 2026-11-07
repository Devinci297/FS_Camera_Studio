#include "xp_traffic.hpp"

#include "xp_geometry.hpp"

#include <XPLMGraphics.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>

namespace fscs::xplane {

namespace be = host::backend_event;

namespace {

constexpr int kNameBytes = 8;  // flight_id and icao_type: 8 bytes per aircraft

// Grid heading (degrees) of true north at a world point (as xp_camera.cpp).
double convergenceAt(double lat, double lon, double alt) {
    double x0 = 0, y0 = 0, z0 = 0, x1 = 0, y1 = 0, z1 = 0;
    XPLMWorldToLocal(lat, lon, alt, &x0, &y0, &z0);
    XPLMWorldToLocal(lat + (lat < 89 ? 0.01 : -0.01), lon, alt, &x1, &y1, &z1);
    const double sign = lat < 89 ? 1.0 : -1.0;
    return std::atan2(sign * (x1 - x0), -sign * (z1 - z0)) * core::kRadToDeg;
}

std::string text(XPLMDataRef r, int index) {
    if (!r) return {};
    char buffer[kNameBytes + 1] = {};
    const int n = XPLMGetDatab(r, buffer, index * kNameBytes, kNameBytes);
    if (n <= 0) return {};
    std::string s(buffer, strnlen(buffer, static_cast<std::size_t>(n)));
    while (!s.empty() && s.back() == ' ') s.pop_back();
    return s;
}

int vf(XPLMDataRef r, float* out, int n) {
    if (!r) return 0;
    return XPLMGetDatavf(r, out, 0, n);
}

}  // namespace

bool TrafficSource::find() {
    if (searched_) return available_;
    searched_ = true;
    const std::string base = "sim/cockpit2/tcas/targets/position/";
    auto at = [&](const char* name) { return XPLMFindDataRef((base + name).c_str()); };
    num_ = XPLMFindDataRef("sim/cockpit2/tcas/indicators/tcas_num_acf");
    x_ = at("x");
    y_ = at("y");
    z_ = at("z");
    psi_ = at("psi");
    the_ = at("the");
    phi_ = at("phi");
    vx_ = at("vx");
    vy_ = at("vy");
    vz_ = at("vz");
    wheels_ = at("weight_on_wheels");
    id_ = XPLMFindDataRef("sim/cockpit2/tcas/targets/modeS_id");
    flightId_ = XPLMFindDataRef("sim/cockpit2/tcas/targets/flight_id");
    type_ = XPLMFindDataRef("sim/cockpit2/tcas/targets/icao_type");
    available_ = num_ && x_ && y_ && z_;
    return available_;
}

bool TrafficSource::read(Arrays& a) {
    if (!find()) return false;
    a.count = std::clamp(XPLMGetDatai(num_), 0, kMax);
    if (a.count <= 1) return true;
    const int n = a.count;
    std::memset(a.id, 0, sizeof(a.id));
    std::memset(a.wheels, 0, sizeof(a.wheels));
    if (id_) XPLMGetDatavi(id_, a.id, 0, n);
    for (float* v : {a.x, a.y, a.z, a.psi, a.the, a.phi, a.vx, a.vy, a.vz}) std::fill(v, v + kMax, 0.0f);
    vf(x_, a.x, n);
    vf(y_, a.y, n);
    vf(z_, a.z, n);
    vf(psi_, a.psi, n);
    vf(the_, a.the, n);
    vf(phi_, a.phi, n);
    vf(vx_, a.vx, n);
    vf(vy_, a.vy, n);
    vf(vz_, a.vz, n);
    if (wheels_) XPLMGetDatavi(wheels_, a.wheels, 0, n);
    return true;
}

// The mode S id (24 bits) when the target has one, else its array slot moved out of that range.
std::uint32_t TrafficSource::objectId(const Arrays& a, int i) const {
    const auto id = static_cast<std::uint32_t>(a.id[i]) & 0xffffffu;
    return id ? id : 0x1000000u + static_cast<std::uint32_t>(i);
}

double TrafficSource::groundAltitude(float x, float y, float z) {
    if (!probe_) probe_ = XPLMCreateProbe(xplm_ProbeY);
    XPLMProbeInfo_t info{};
    info.structSize = sizeof(info);
    if (!probe_ || XPLMProbeTerrainXYZ(probe_, x, y, z, &info) != xplm_ProbeHitTerrain) return NAN;
    double lat = 0, lon = 0, alt = 0;
    XPLMLocalToWorld(info.locationX, info.locationY, info.locationZ, &lat, &lon, &alt);
    return alt;
}

namespace {
// A target's pose in the module's terms: world position, true heading, velocity north/east/down.
struct Pose {
    double lat = 0, lon = 0, alt = 0, heading = 0, north = 0, east = 0, down = 0;
};
Pose poseOf(float x, float y, float z, float psi, float vx, float vy, float vz) {
    Pose p;
    XPLMLocalToWorld(x, y, z, &p.lat, &p.lon, &p.alt);
    const double c = convergenceAt(p.lat, p.lon, p.alt);
    p.heading = std::fmod(psi - c + 720.0, 360.0);
    // Grid north/east to true north/east: turn back by the convergence.
    const double gn = -vz, ge = vx, r = c * core::kDegToRad;
    p.north = gn * std::cos(r) + ge * std::sin(r);
    p.east = ge * std::cos(r) - gn * std::sin(r);
    p.down = -vy;
    return p;
}
}  // namespace

bool TrafficSource::poll(double radiusMeters) {
    Arrays a;
    if (!read(a)) return false;
    be::Traffic t;
    t.userObjectId = a.count > 0 ? objectId(a, 0) : 0;
    for (int i = 1; i < a.count; ++i) {
        if (a.x[i] == 0 && a.y[i] == 0 && a.z[i] == 0) continue;  // an empty slot
        const double dx = a.x[i] - a.x[0], dz = a.z[i] - a.z[0];
        if (std::hypot(dx, dz) > radiusMeters) continue;
        const Pose p = poseOf(a.x[i], a.y[i], a.z[i], a.psi[i], a.vx[i], a.vy[i], a.vz[i]);
        be::TrafficAircraft ac;
        ac.objectId = objectId(a, i);
        ac.lat = p.lat;
        ac.lon = p.lon;
        ac.altitude = p.alt;
        ac.headingDeg = p.heading;
        ac.pitchDeg = a.the[i];
        ac.bankDeg = a.phi[i];
        ac.groundSpeed = std::hypot(p.north, p.east);
        ac.verticalSpeed = -p.down;
        ac.groundAltitude = groundAltitude(a.x[i], a.y[i], a.z[i]);
        ac.onGround = a.wheels[i] != 0 || (std::isfinite(ac.groundAltitude) && ac.altitude - ac.groundAltitude < 1.0);
        ac.atcId = text(flightId_, i);
        ac.model = text(type_, i);
        t.aircraft.push_back(std::move(ac));
    }
    lastCount_ = t.aircraft.size();
    stage_(std::move(t));
    return true;
}

bool TrafficSource::watch(std::uint32_t objectId) {
    watched_ = objectId;
    return objectId == 0 || find();
}

void TrafficSource::frame() {
    if (!watched_) return;
    Arrays a;
    if (!read(a)) return;
    for (int i = 1; i < a.count; ++i) {
        if (objectId(a, i) != watched_) continue;
        const Pose p = poseOf(a.x[i], a.y[i], a.z[i], a.psi[i], a.vx[i], a.vy[i], a.vz[i]);
        be::TrafficTarget t;
        t.objectId = watched_;
        t.lat = p.lat;
        t.lon = p.lon;
        t.altitude = p.alt;
        t.headingDeg = p.heading;
        t.pitchDeg = a.the[i];
        t.bankDeg = a.phi[i];
        t.velocityNorth = p.north;
        t.velocityEast = p.east;
        t.velocityDown = p.down;
        t.groundAltitude = groundAltitude(a.x[i], a.y[i], a.z[i]);
        t.onGround = a.wheels[i] != 0 || (std::isfinite(t.groundAltitude) && t.altitude - t.groundAltitude < 1.0);
        stage_(t);
        return;
    }
}

void TrafficSource::destroy() {
    if (probe_) XPLMDestroyProbe(probe_);
    probe_ = nullptr;
}

}  // namespace fscs::xplane
