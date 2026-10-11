#pragma once
// Canonical math for FS Camera Studio. Frame convention (doc/protocol.md §1): right-handed FRD —
// x forward, y right, z down. Quaternions are [x, y, z, w] and rotate camera coordinates into the anchor frame.

#include <cmath>
#include <numbers>

namespace fscs::core {

constexpr double kDegToRad = std::numbers::pi / 180.0;
constexpr double kRadToDeg = 180.0 / std::numbers::pi;

struct Vec3 {
    double x = 0, y = 0, z = 0;

    constexpr Vec3 operator+(const Vec3& o) const { return {x + o.x, y + o.y, z + o.z}; }
    constexpr Vec3 operator-(const Vec3& o) const { return {x - o.x, y - o.y, z - o.z}; }
    constexpr Vec3 operator*(double s) const { return {x * s, y * s, z * s}; }
    constexpr Vec3& operator+=(const Vec3& o) { x += o.x; y += o.y; z += o.z; return *this; }
    double length() const { return std::sqrt(x * x + y * y + z * z); }
    bool finite() const { return std::isfinite(x) && std::isfinite(y) && std::isfinite(z); }
};

constexpr double dot(const Vec3& a, const Vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
constexpr Vec3 cross(const Vec3& a, const Vec3& b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
constexpr Vec3 lerp(const Vec3& a, const Vec3& b, double t) { return a + (b - a) * t; }

// Aerospace angles in degrees: heading > 0 turns right, pitch > 0 looks up, bank > 0 rolls right.
struct Pbh {
    double pitchDeg = 0, bankDeg = 0, headingDeg = 0;
};

struct Quat {
    double x = 0, y = 0, z = 0, w = 1;

    static constexpr Quat identity() { return {}; }
    static Quat fromAxisAngle(const Vec3& axis, double angleRad);
    // q = Rz(heading) * Ry(pitch) * Rx(bank) in FRD; positive pitch looks up.
    static Quat fromPbh(const Pbh& angles);

    Pbh toPbh() const;
    double norm() const { return std::sqrt(x * x + y * y + z * z + w * w); }
    Quat normalized() const;
    Quat conjugate() const { return {-x, -y, -z, w}; }
    Vec3 rotate(const Vec3& v) const;
    bool finite() const { return std::isfinite(x) && std::isfinite(y) && std::isfinite(z) && std::isfinite(w); }

    Quat operator*(const Quat& o) const;
};

double dot(const Quat& a, const Quat& b);
// Shortest-arc spherical interpolation. Inputs need not share a hemisphere.
Quat slerpShortest(const Quat& a, Quat b, double t);
// Angle between two orientations in radians (0..pi).
double angleBetween(const Quat& a, const Quat& b);

constexpr double clamp(double v, double lo, double hi) { return v < lo ? lo : (v > hi ? hi : v); }
// Wrap an angle in degrees to (-180, 180].
double wrapDeg(double deg);

}  // namespace fscs::core
