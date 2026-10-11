#include "fscs/core/math.hpp"

#include <algorithm>

namespace fscs::core {

Quat Quat::fromAxisAngle(const Vec3& axis, double angleRad) {
    const double len = axis.length();
    if (len <= 0.0) return identity();
    const double s = std::sin(angleRad * 0.5) / len;
    return {axis.x * s, axis.y * s, axis.z * s, std::cos(angleRad * 0.5)};
}

Quat Quat::fromPbh(const Pbh& a) {
    const Quat qz = fromAxisAngle({0, 0, 1}, a.headingDeg * kDegToRad);
    const Quat qy = fromAxisAngle({0, 1, 0}, a.pitchDeg * kDegToRad);  // +y rotation tilts +x toward -z (up)
    const Quat qx = fromAxisAngle({1, 0, 0}, a.bankDeg * kDegToRad);
    return (qz * qy * qx).normalized();
}

Pbh Quat::toPbh() const {
    // Inverse of Rz(heading) * Ry(pitch) * Rx(bank).
    const Quat q = normalized();
    const double sinTheta = clamp(2.0 * (q.w * q.y - q.z * q.x), -1.0, 1.0);
    const double theta = std::asin(sinTheta);
    double phi = 0, psi = 0;
    if (std::abs(sinTheta) < 0.999999) {
        phi = std::atan2(2.0 * (q.w * q.x + q.y * q.z), 1.0 - 2.0 * (q.x * q.x + q.y * q.y));
        psi = std::atan2(2.0 * (q.w * q.z + q.x * q.y), 1.0 - 2.0 * (q.y * q.y + q.z * q.z));
    } else {
        // Gimbal lock: fold bank into heading.
        psi = std::atan2(-2.0 * (q.x * q.y - q.w * q.z), 1.0 - 2.0 * (q.x * q.x + q.z * q.z));
    }
    return {theta * kRadToDeg, phi * kRadToDeg, psi * kRadToDeg};
}

Quat Quat::normalized() const {
    const double n = norm();
    if (!(n > 0.0) || !std::isfinite(n)) return identity();
    return {x / n, y / n, z / n, w / n};
}

Vec3 Quat::rotate(const Vec3& v) const {
    const Vec3 u{x, y, z};
    const Vec3 t = cross(u, v) * 2.0;
    return v + t * w + cross(u, t);
}

Quat Quat::operator*(const Quat& o) const {
    return {w * o.x + x * o.w + y * o.z - z * o.y,
            w * o.y - x * o.z + y * o.w + z * o.x,
            w * o.z + x * o.y - y * o.x + z * o.w,
            w * o.w - x * o.x - y * o.y - z * o.z};
}

double dot(const Quat& a, const Quat& b) { return a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w; }

Quat slerpShortest(const Quat& a, Quat b, double t) {
    double d = dot(a, b);
    if (d < 0.0) {
        b = {-b.x, -b.y, -b.z, -b.w};
        d = -d;
    }
    if (d > 0.9995) {
        // Nearly parallel: normalised lerp is accurate and avoids division by ~0.
        return Quat{a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t, a.w + (b.w - a.w) * t}
            .normalized();
    }
    const double theta0 = std::acos(d);
    const double theta = theta0 * t;
    const double s0 = std::cos(theta) - d * std::sin(theta) / std::sin(theta0);
    const double s1 = std::sin(theta) / std::sin(theta0);
    return Quat{a.x * s0 + b.x * s1, a.y * s0 + b.y * s1, a.z * s0 + b.z * s1, a.w * s0 + b.w * s1}.normalized();
}

double angleBetween(const Quat& a, const Quat& b) {
    const double d = std::min(1.0, std::abs(dot(a.normalized(), b.normalized())));
    return 2.0 * std::acos(d);
}

double wrapDeg(double deg) {
    double r = std::fmod(deg + 180.0, 360.0);
    if (r <= 0.0) r += 360.0;
    return r - 180.0;
}

}  // namespace fscs::core
