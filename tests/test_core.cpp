#include "test.hpp"

#include "adapter.hpp"
#include "fscs/core/engine.hpp"

using namespace fscs::core;
using fscs::host::AdapterContract;
using fscs::host::fromSdk;
using fscs::host::Referential;
using fscs::host::toSdk;

namespace {
constexpr double kEps = 1e-9;
CameraPose pose(Vec3 p, Pbh a, double fov = 1.0) { return {p, Quat::fromPbh(a), fov}; }
}  // namespace

TEST_CASE("math: heading 90 turns the view to the right") {
    const Vec3 f = Quat::fromPbh({0, 0, 90}).rotate({1, 0, 0});
    CHECK_NEAR(f.x, 0, kEps);
    CHECK_NEAR(f.y, 1, kEps);
    CHECK_NEAR(f.z, 0, kEps);
}

TEST_CASE("math: positive pitch looks up (negative z in FRD)") {
    const Vec3 f = Quat::fromPbh({30, 0, 0}).rotate({1, 0, 0});
    CHECK_NEAR(f.x, std::cos(30 * kDegToRad), kEps);
    CHECK_NEAR(f.z, -std::sin(30 * kDegToRad), kEps);
}

TEST_CASE("math: positive bank rolls right (camera up tilts to +y)") {
    const Vec3 up = Quat::fromPbh({0, 20, 0}).rotate({0, 0, -1});
    CHECK(up.y > 0.3);
}

TEST_CASE("math: PBH round trip over a grid") {
    for (double p = -85; p <= 85; p += 17)
        for (double b = -170; b <= 170; b += 34)
            for (double h = -175; h <= 175; h += 35) {
                const Pbh back = Quat::fromPbh({p, b, h}).toPbh();
                CHECK_NEAR(back.pitchDeg, p, 1e-7);
                CHECK_NEAR(back.bankDeg, b, 1e-7);
                CHECK_NEAR(back.headingDeg, h, 1e-7);
            }
}

TEST_CASE("math: slerp takes the shortest arc across the sign flip") {
    const Quat a = Quat::fromPbh({0, 0, 170});
    Quat b = Quat::fromPbh({0, 0, -170});
    b = {-b.x, -b.y, -b.z, -b.w};  // same rotation, opposite hemisphere
    const Pbh mid = slerpShortest(a, b, 0.5).toPbh();
    CHECK_NEAR(std::abs(mid.headingDeg), 180, 1e-6);
}

TEST_CASE("math: wrapDeg") {
    CHECK_NEAR(wrapDeg(190), -170, kEps);
    CHECK_NEAR(wrapDeg(-190), 170, kEps);
    CHECK_NEAR(wrapDeg(180), 180, kEps);
    CHECK_NEAR(wrapDeg(540), 180, kEps);
}

TEST_CASE("easing: endpoints and symmetry") {
    for (auto e : {Easing::Smoothstep3, Easing::Smoothstep5}) {
        CHECK_NEAR(ease(e, 0), 0, kEps);
        CHECK_NEAR(ease(e, 1), 1, kEps);
        CHECK_NEAR(ease(e, 0.5), 0.5, kEps);
        CHECK_NEAR(ease(e, 0.25) + ease(e, 0.75), 1, kEps);
    }
    CHECK_NEAR(ease(Easing::Cut, 0), 1, kEps);
}

TEST_CASE("transition: endpoint reached exactly at the active duration, independent of step size") {
    const CameraPose a = pose({0, 0, 0}, {0, 0, 0}, 1.0), b = pose({1, -2, 0.5}, {10, 0, 45}, 0.8);
    for (double hz : {30.0, 60.0, 120.0, 37.0}) {
        Transition t(a, b, {Easing::Smoothstep5, 0.35, 0});
        double elapsed = 0;
        while (elapsed + 1.0 / hz < 0.35 - 1e-12) {
            t.advance(1.0 / hz);
            elapsed += 1.0 / hz;
            CHECK(!t.finished());
        }
        t.advance(0.35 - elapsed);
        CHECK(t.finished());
        const auto s = t.sample();
        CHECK_NEAR((s.positionFRD - b.positionFRD).length(), 0, 1e-12);
        CHECK_NEAR(angleBetween(s.orientation, b.orientation), 0, 1e-7);
        CHECK_NEAR(s.fovRad, 0.8, 1e-12);
    }
}

TEST_CASE("transition: analytic sample matches at the same time for any rate") {
    const CameraPose a = pose({0, 0, 0}, {0, 0, 0}), b = pose({2, 0, 0}, {0, 0, 90});
    Transition coarse(a, b, {Easing::Smoothstep5, 1.0, 0}), fine(a, b, {Easing::Smoothstep5, 1.0, 0});
    for (int i = 0; i < 9; ++i) coarse.advance(1.0 / 30.0);
    for (int i = 0; i < 36; ++i) fine.advance(1.0 / 120.0);
    CHECK_NEAR((coarse.sample().positionFRD - fine.sample().positionFRD).length(), 0, 1e-9);
    CHECK_NEAR(angleBetween(coarse.sample().orientation, fine.sample().orientation), 0, 1e-7);
}

TEST_CASE("transition: delay holds the start pose, cut has zero duration") {
    const CameraPose a = pose({0, 0, 0}, {}), b = pose({1, 0, 0}, {});
    Transition delayed(a, b, {Easing::Smoothstep5, 0.5, 1.0});
    delayed.advance(0.9);
    CHECK_NEAR(delayed.sample().positionFRD.x, 0, kEps);
    Transition cut(a, b, {Easing::Cut, 5.0, 0});
    CHECK(cut.finished());
    CHECK_NEAR(cut.sample().positionFRD.x, 1, kEps);
}

TEST_CASE("engine: interrupting a transition starts from the current output, not the old target") {
    CameraEngine e;
    e.place(pose({0, 0, 0}, {}));
    e.select(pose({10, 0, 0}, {}), {Easing::Smoothstep5, 1.0, 0}, true);
    e.tick(0.5, {});
    const double mid = e.sample().pose.positionFRD.x;
    CHECK_NEAR(mid, 5, 1e-9);
    e.select(pose({0, 4, 0}, {}), {Easing::Smoothstep5, 1.0, 0}, true);
    CHECK_NEAR(e.sample().pose.positionFRD.x, mid, 1e-9);  // no jump
    e.tick(1.0, {});
    CHECK_NEAR(e.sample().pose.positionFRD.x, 0, 1e-9);
    CHECK_NEAR(e.sample().pose.positionFRD.y, 4, 1e-9);
    CHECK(!e.transitioning());
}

TEST_CASE("engine: temporary look never alters the saved base pose") {
    CameraEngine e;
    const CameraPose base = pose({0.2, -0.3, 0.1}, {5, 0, 10});
    e.place(base);
    ManualInput in;
    in.lookRight = 1;
    in.moveForward = 1;
    for (int i = 0; i < 30; ++i) e.tick(1.0 / 60.0, in);
    CHECK(e.look().active());
    CHECK(e.sample().pose.orientation.toPbh().headingDeg > 10.5);
    CHECK_NEAR((e.basePose()->positionFRD - base.positionFRD).length(), 0, kEps);
    e.recenterLook();
    CHECK_NEAR(angleBetween(e.sample().pose.orientation, base.orientation), 0, 1e-9);
}

TEST_CASE("manual: fixed-step integration gives the same result at 30/60/120 Hz") {
    auto run = [](double hz) {
        ManualMover m;
        TemporaryLook look;
        ManualInput in;
        in.lookRight = 1;
        in.moveForward = 0.5;
        const int steps = static_cast<int>(10.0 * hz);
        for (int i = 0; i < steps; ++i) m.advance(1.0 / hz, in, look);
        return look;
    };
    const auto a = run(30), b = run(60), c = run(120);
    CHECK_NEAR(a.headingDeg, b.headingDeg, 0.1);
    CHECK_NEAR(b.headingDeg, c.headingDeg, 0.1);
    CHECK_NEAR((a.translationHeadingFrame - c.translationHeadingFrame).length(), 0, 0.005);
}

TEST_CASE("manual: a stall longer than 100 ms does not catch up") {
    ManualMover m;
    TemporaryLook look;
    ManualInput in;
    in.lookRight = 1;
    m.advance(1.0 / 60, in, look);
    const double before = look.headingDeg;
    m.advance(2.0, in, look);  // stall
    CHECK_NEAR(look.headingDeg, before, kEps);
}

TEST_CASE("engine: invalid values never produce a valid sample") {
    CameraEngine e;
    CameraPose bad = pose({0, 0, 0}, {});
    bad.fovRad = 5.0;
    e.place(bad);
    CHECK(!e.sample().valid);
    bad.fovRad = 1.0;
    bad.positionFRD.x = NAN;
    e.place(bad);
    CHECK(!e.sample().valid);
}

TEST_CASE("adapter: default contract maps FRD to right/up/forward and round-trips") {
    AdapterContract c;
    CHECK(c.valid());
    const CameraPose p = pose({1.5, -0.25, -0.75}, {12, -4, 33}, 0.9);
    const auto s = toSdk(p, c, Referential::Eyepoint);
    CHECK_NEAR(s.z, 1.5, kEps);    // forward
    CHECK_NEAR(s.x, -0.25, kEps);  // right
    CHECK_NEAR(s.y, 0.75, kEps);   // up
    CHECK_NEAR(s.pitchDeg, -12, 1e-7);
    const auto back = fromSdk(s, c);
    CHECK_NEAR((back.positionFRD - p.positionFRD).length(), 0, kEps);
    CHECK_NEAR(angleBetween(back.orientation, p.orientation), 0, 1e-7);
}

TEST_CASE("adapter: contract JSON rejects permutations that are not bijective") {
    AdapterContract c;
    auto j = c.toJson();
    CHECK(AdapterContract::fromJson(j).has_value());
    j["axisIndex"] = {0, 0, 1};
    CHECK(!AdapterContract::fromJson(j).has_value());
    j = c.toJson();
    j["pitchSign"] = 2.0;
    CHECK(!AdapterContract::fromJson(j).has_value());
}
