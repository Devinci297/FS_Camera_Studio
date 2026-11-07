#pragma once
// XP Camera Studio: frames between the module's canonical camera poses and X-Plane (no XPLM calls, so it is tested
// on its own).
//
//   OpenGL local   x east, y up, z south (meters)              X-Plane world rendering, XPLMControlCamera
//   grid NED       x north (-z), y east (x), z down (-y)       aircraft attitude psi/theta/phi, camera headings
//   aircraft body  x right, y up, z aft (meters)               pilots_head_x/y/z, acf_peX/Y/Z
//   FRD            x forward, y right, z down                  the module (fscs::core), relative to the aircraft
//
// X-Plane headings are measured from the OpenGL -z axis ("grid north"), which differs from true north by the grid
// convergence at the point; world poses from the module use true north.

#include "fscs/core/math.hpp"

namespace fscs::xplane::geo {

using core::Quat;
using core::Vec3;

constexpr Vec3 nedToGl(const Vec3& n) { return {n.y, -n.z, -n.x}; }
constexpr Vec3 glToNed(const Vec3& g) { return {-g.z, g.x, -g.y}; }
constexpr Vec3 bodyToFrd(const Vec3& b) { return {-b.z, b.x, -b.y}; }
constexpr Vec3 frdToBody(const Vec3& f) { return {f.y, -f.z, -f.x}; }

// The aircraft in the OpenGL frame: its reference point (local_x/y/z) and body (FRD) -> grid NED attitude.
struct Aircraft {
    Vec3 gl;
    Quat attitude;
};
// X-Plane's psi (heading from grid north), theta (pitch up) and phi (roll right), degrees.
inline Quat attitude(double thetaDeg, double phiDeg, double psiDeg) { return Quat::fromPbh({thetaDeg, phiDeg, psiDeg}); }

// A camera as XPLMCameraPosition_t wants it: OpenGL position, degrees from a camera facing grid north, level.
struct GlCamera {
    Vec3 gl;
    double pitchDeg = 0, headingDeg = 0, rollDeg = 0;
};

inline GlCamera fromGrid(const Vec3& gl, const Quat& cameraToGrid) {
    const auto a = cameraToGrid.toPbh();
    return {gl, a.pitchDeg, a.headingDeg, a.bankDeg};
}
inline Quat gridOrientation(const GlCamera& c) { return Quat::fromPbh({c.pitchDeg, c.rollDeg, c.headingDeg}); }

// Aircraft-relative camera (FRD offset from the reference point, camera -> body) to the OpenGL camera.
inline GlCamera relativeToGl(const Aircraft& a, const Vec3& offsetFrd, const Quat& cameraToBody) {
    return fromGrid(a.gl + nedToGl(a.attitude.rotate(offsetFrd)), (a.attitude * cameraToBody).normalized());
}
// The reverse: where an OpenGL camera is relative to the aircraft.
inline void glToRelative(const Aircraft& a, const GlCamera& c, Vec3& offsetFrd, Quat& cameraToBody) {
    const Quat inverse = a.attitude.conjugate();
    offsetFrd = inverse.rotate(glToNed(c.gl - a.gl));
    cameraToBody = (inverse * gridOrientation(c)).normalized();
}

// True-north orientation to grid north, given the grid heading (degrees) of true north at that point.
inline Quat trueToGrid(const Quat& cameraToTrue, double convergenceDeg) {
    return (Quat::fromAxisAngle({0, 0, 1}, convergenceDeg * core::kDegToRad) * cameraToTrue).normalized();
}
inline Quat gridToTrue(const Quat& cameraToGrid, double convergenceDeg) { return trueToGrid(cameraToGrid, -convergenceDeg); }

// Looking from `from` at `to` (OpenGL), level horizon. Nothing to look at when they coincide.
inline bool aim(const Vec3& fromGl, const Vec3& toGl, double& pitchDeg, double& headingDeg) {
    const Vec3 d = glToNed(toGl - fromGl);
    const double horizontal = std::hypot(d.x, d.y);
    if (horizontal < 1e-6 && std::abs(d.z) < 1e-6) return false;
    headingDeg = std::atan2(d.y, d.x) * core::kRadToDeg;
    pitchDeg = std::atan2(-d.z, horizontal) * core::kRadToDeg;
    return true;
}

// XPLMCameraPosition_t zoom for a horizontal field of view, given the view's own horizontal field of view.
inline double zoomFor(double fovRad, double baseFovDeg) {
    const double base = core::clamp(baseFovDeg, 5., 170.) * core::kDegToRad / 2;
    const double fov = core::clamp(fovRad, 1. * core::kDegToRad, 170. * core::kDegToRad) / 2;
    return std::tan(base) / std::tan(fov);
}
inline double fovFor(double zoom, double baseFovDeg) {
    const double base = core::clamp(baseFovDeg, 5., 170.) * core::kDegToRad / 2;
    return 2 * std::atan(std::tan(base) / (zoom > 0.01 ? zoom : 1.0));
}

}  // namespace fscs::xplane::geo
