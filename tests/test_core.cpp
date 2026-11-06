#include "test.hpp"

#include <algorithm>
#include "adapter.hpp"
#include "fscs/core/engine.hpp"
#include "fscs/core/effects.hpp"
#include "fscs/core/horizon.hpp"

using namespace fscs::core;
using fscs::host::AdapterContract;
using fscs::host::fromSdk;
using fscs::host::Referential;
using fscs::host::toSdk;

namespace {
constexpr double kEps = 1e-9;
CameraPose pose(Vec3 p, Pbh a, double fov = 1.0) { return {p, Quat::fromPbh(a), fov}; }
}  // namespace

TEST_CASE("horizon: counter-roll leaves the aircraft's bank times follow horizon at 0, 0.5 and 1") {
    // Aircraft attitude in the local level frame, FRD: banked 30 degrees right, nose up 10, heading 40.
    const Quat aircraft = Quat::fromPbh({10, 30, 40});
    CHECK_NEAR(horizonCounterRollDeg(30, 0), -30, kEps);
    CHECK_NEAR(horizonCounterRollDeg(30, .5), -15, kEps);
    CHECK_NEAR(horizonCounterRollDeg(30, 1), 0, kEps);
    CHECK_NEAR(horizonCounterRollDeg(-20, 0), 20, kEps);
    CHECK_NEAR(horizonCounterRollDeg(30, 7), 0, kEps);  // clamped
    CHECK_NEAR(horizonCounterRollDeg(NAN, 0), 0, kEps);
    for (const double follow : {0., .5, 1.}) {
        // Forward view: the world bank is exactly bank * follow, and pitch/heading are untouched.
        const auto forward = counterRoll(pose({}, {}), horizonCounterRollDeg(30, follow), false);
        const auto world = (aircraft * forward.orientation).toPbh();
        CHECK_NEAR(world.bankDeg, 30 * follow, 1e-7);
        CHECK_NEAR(world.pitchDeg, 10, 1e-7);
        CHECK_NEAR(world.headingDeg, 40, 1e-7);
        // Cockpit views keep the eye where it is.
        const auto eye = counterRoll(pose({.3, .7, .1}, {-12, 0, 0}), horizonCounterRollDeg(30, follow), false);
        CHECK_NEAR(eye.positionFRD.y, .7, kEps);
        if (follow == 0) CHECK_NEAR((aircraft * eye.orientation).toPbh().bankDeg, 0, 1e-7);
    }
    // A side-facing view in a level-pitch bank sees a level horizon instead of a pitched one.
    const Quat banked = Quat::fromPbh({0, 30, 0});
    const auto side = counterRoll(pose({}, {0, 0, 90}), horizonCounterRollDeg(30, 0), false);
    const auto sideWorld = (banked * side.orientation).toPbh();
    CHECK_NEAR(sideWorld.pitchDeg, 0, 1e-7);
    CHECK_NEAR(sideWorld.bankDeg, 0, 1e-7);
    CHECK(std::abs((banked * Quat::fromPbh({0, 0, 90})).toPbh().pitchDeg) > 20);
    // Exterior views swing about the aircraft and stay aimed at it.
    const Vec3 at{0, 10, -2};
    const CameraPose wing{at, Quat::fromPbh({std::atan2(-2., 10.) * -kRadToDeg, 0, -90}), 1.};
    const auto swung = counterRoll(wing, -30, true);
    CHECK_NEAR(swung.positionFRD.length(), at.length(), 1e-9);
    const Vec3 aim = swung.orientation.rotate({1, 0, 0}), toAircraft = swung.positionFRD * (-1 / swung.positionFRD.length());
    const Vec3 aimBefore = wing.orientation.rotate({1, 0, 0}), toBefore = at * (-1 / at.length());
    CHECK_NEAR(dot(aim, toAircraft), dot(aimBefore, toBefore), 1e-9);
    CHECK_NEAR(swung.fovRad, 1., kEps);
    // Zero roll is the identity.
    const auto same = counterRoll(wing, 0, true);
    CHECK_NEAR(angleBetween(same.orientation, wing.orientation), 0, kEps);
}

TEST_CASE("horizon: the filter eases with a 0.15 s time constant independent of frame rate") {
    HorizonFilter a, b;
    a.tick(.15, 10);
    CHECK_NEAR(a.rollDeg(), 10 * (1 - std::exp(-1.)), 1e-9);
    for (int i = 0; i < 15; ++i) b.tick(.01, 10);
    CHECK_NEAR(b.rollDeg(), a.rollDeg(), 1e-9);
    for (int i = 0; i < 200; ++i) b.tick(.01, 10);
    CHECK_NEAR(b.rollDeg(), 10, 1e-4);
    b.tick(0, -50);
    CHECK_NEAR(b.rollDeg(), 10, 1e-4);
    b.tick(.01, NAN);  // invalid target eases toward 0
    CHECK(b.rollDeg() < 10);
    b.reset();
    CHECK_NEAR(b.rollDeg(), 0, kEps);
}

TEST_CASE("movement: WASD follows yaw and pitch while QE remains vertical") {
    CameraEngine e; e.place(pose({},{}));e.offsetLook(90,30);
    ManualInput input;input.moveForward=1;
    for(int i=0;i<120;++i)e.tick(1.0/60,input);
    const auto moved=e.sample().pose.positionFRD;
    CHECK_NEAR(moved.x,0,1e-8);CHECK(moved.y>.5);CHECK(moved.z<-.3);
    CHECK_NEAR(-moved.z/moved.y,std::tan(30*kDegToRad),1e-6);
    e.freeze();const auto before=e.sample().pose.positionFRD;input={};input.moveUp=1;
    for(int i=0;i<60;++i)e.tick(1.0/60,input);
    const auto after=e.sample().pose.positionFRD;
    CHECK_NEAR(before.x,after.x,1e-8);CHECK_NEAR(before.y,after.y,1e-8);CHECK(after.z<before.z);
}

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
TEST_CASE("engine: mouse look keeps preset unchanged and recenters") {
    CameraEngine e;
    const auto saved = pose({1, 2, 3}, {5, 0, 10});
    e.place(saved);
    e.offsetLook(20, 12);
    CHECK_NEAR(e.look().headingDeg, 20, kEps);
    CHECK_NEAR(e.look().pitchDeg, 12, kEps);
    CHECK_NEAR(angleBetween(e.basePose()->orientation, saved.orientation), 0, 1e-7);
    e.offsetLook(0, 200);
    CHECK_NEAR(e.look().pitchDeg + saved.orientation.toPbh().pitchDeg, 89, kEps);
    e.offsetLook(NAN, 0);
    CHECK(e.sample().valid);
    e.recenterLook();
    CHECK(!e.look().active());
    CHECK_NEAR(angleBetween(e.sample().pose.orientation, saved.orientation), 0, 1e-7);
}

TEST_CASE("effects: only valid enabled telemetry moves the view and never changes the base") {
    CameraEffects fx; EffectsSettings settings; FlightMotion motion; CameraPose base = pose({1,2,3}, {}, 1.);
    motion.forwardAcceleration = 5.;
    fx.tick(.02, motion, settings, true); CHECK_NEAR(fx.offset().length(), 0., 1e-12);
    settings.enabled = true;
    for (int i=0;i<50;++i) fx.tick(.02,motion,settings,true);
    CHECK(fx.offset().x < 0); CHECK(fx.offset().length() < .1);
    const auto output = fx.apply(base); CHECK(output.positionFRD.x < base.positionFRD.x); CHECK_NEAR(base.positionFRD.x,1.,1e-12);
    CHECK_NEAR(output.fovRad,base.fovRad,1e-12);
    fx.tick(.02,motion,settings,false); CHECK_NEAR(fx.offset().length(),0.,1e-12);
    fx.tick(.5,motion,settings,true); CHECK_NEAR(fx.offset().length(),0.,1e-12);
    motion.gForce = std::numeric_limits<double>::quiet_NaN();
    fx.tick(.02,motion,settings,true); CHECK_NEAR(fx.offset().length(),0.,1e-12);
}
TEST_CASE("effects: stationary aircraft is still and touchdown does not trigger on startup") {
    CameraEffects fx; EffectsSettings settings; settings.enabled=true; FlightMotion motion;
    for (int i=0;i<50;++i) fx.tick(.02,motion,settings,true);
    CHECK_NEAR(fx.offset().length(),0.,1e-12);
    motion.onGround=0; motion.verticalSpeed=-3.; fx.tick(.02,motion,settings,true);
    motion.onGround=1; motion.verticalSpeed=0.; fx.tick(.02,motion,settings,true);
    CHECK(fx.offset().z > 0);
    for (int i=0;i<500;++i) fx.tick(.02,motion,settings,true);
    CHECK_NEAR(fx.offset().length(),0.,1e-9);
    motion.groundSpeed=20;
    for (int i=0;i<30;++i) fx.tick(.02,motion,settings,true);
    CHECK(fx.offset().length()>1e-7);
    settings.intensity=0;
    for (int i=0;i<100;++i) fx.tick(.02,motion,settings,true);
    CHECK_NEAR(fx.offset().length(),0.,1e-9);
}
TEST_CASE("effects: scale reduces offsets and pitch proportionally for exterior views") {
    EffectsSettings settings; settings.enabled=true; FlightMotion motion; motion.forwardAcceleration=5.;
    const auto run=[&](double scale) {
        CameraEffects fx; FlightMotion m=motion;
        for (int i=0;i<50;++i) fx.tick(.02,m,settings,true,scale);
        m.onGround=0; m.verticalSpeed=-3.; fx.tick(.02,m,settings,true,scale);
        m.onGround=1; m.verticalSpeed=0.; for (int i=0;i<5;++i) fx.tick(.02,m,settings,true,scale);
        return fx;
    };
    const auto full=run(1.), exterior=run(kExteriorEffectsScale);
    CHECK(full.offset().length()>1e-4);
    CHECK_NEAR(exterior.offset().x,full.offset().x*kExteriorEffectsScale,1e-12);
    CHECK_NEAR(exterior.offset().z,full.offset().z*kExteriorEffectsScale,1e-12);
    const CameraPose base=pose({1,2,3},{},1.);
    const double fullPitch=full.apply(base).orientation.toPbh().pitchDeg, exteriorPitch=exterior.apply(base).orientation.toPbh().pitchDeg;
    CHECK(std::abs(fullPitch)>1e-6);
    CHECK_NEAR(exteriorPitch,fullPitch*kExteriorEffectsScale,1e-6);
    CHECK_NEAR(run(0.).offset().length(),0.,1e-12);
    CHECK_NEAR(run(std::numeric_limits<double>::quiet_NaN()).offset().length(),0.,1e-12);
    CHECK_NEAR(run(4.).offset().x,full.offset().x,1e-12);
}
namespace {
// Head-model settings with a single term at full strength.
EffectsSettings headOnly(double headLag, double turnLean, double taxiBob, double idleSway) {
    EffectsSettings s; s.enabled = true; s.intensity = 1;
    s.headLag = headLag; s.turnLean = turnLean; s.taxiBob = taxiBob; s.idleSway = idleSway;
    return s;
}
}  // namespace
TEST_CASE("head model: neck spring lags a sustained G step, settles in about a second and returns to rest") {
    HeadModel head; const auto settings = headOnly(1, 0, 0, 0); FlightMotion motion; motion.onGround = 0;
    const double dt = .005, steady = HeadModel::kLagVerticalM;  // 2 G: one g of extra load
    motion.gForce = 2;
    double peak = 0, settled = -1;
    for (int i = 1; i * dt <= 2.5; ++i) {
        head.tick(dt, motion, settings, 1);
        const double z = head.pose().offset.z;
        if (i * dt <= .1 + 1e-9) CHECK(z < .5 * steady);  // the head lags behind the load
        peak = std::max(peak, z);
        if (std::abs(z - steady) > .02 * steady) settled = -1; else if (settled < 0) settled = i * dt;
    }
    CHECK(settled > .6); CHECK(settled < 1.2);
    CHECK(peak > steady); CHECK(peak < 1.1 * steady);  // slightly under-damped
    CHECK_NEAR(head.pose().offset.z, steady, 1e-4);
    CHECK(head.pose().pitchDeg < 0);  // extra G nods the head down
    motion.gForce = 1;
    for (int i = 0; i < 240; ++i) head.tick(dt, motion, settings, 1);
    CHECK(std::abs(head.pose().offset.z) < .02 * steady);
    for (int i = 0; i < 1000; ++i) head.tick(dt, motion, settings, 1);
    CHECK_NEAR(head.pose().offset.length(), 0, 1e-7); CHECK_NEAR(head.pose().pitchDeg, 0, 1e-5);
}
TEST_CASE("head model: lean into turns, low-frequency taxi bob and optional idle sway") {
    FlightMotion turn; turn.onGround = 0; turn.lateralAcceleration = 2;  // right turn, ~0.2 g
    HeadModel lean;
    for (int i = 0; i < 300; ++i) lean.tick(.01, turn, headOnly(0, 1, 0, 0), 1);
    CHECK(lean.pose().rollDeg > 2); CHECK(lean.pose().rollDeg < HeadModel::kMaxLeanDeg);  // rolls right, a few degrees
    CHECK(lean.pose().offset.y > 0);
    turn.lateralAcceleration = 50;
    for (int i = 0; i < 300; ++i) lean.tick(.01, turn, headOnly(0, 1, 0, 0), 1);
    CHECK(lean.pose().rollDeg <= HeadModel::kMaxLeanDeg);
    // Taxi bob: on the ground, ~1-2 Hz, absent when stopped or airborne.
    const auto crossings = [](FlightMotion m, const EffectsSettings& s, double* amplitude) {
        HeadModel head; int count = 0; double last = 0, peak = 0;
        for (int i = 0; i < 100; ++i) head.tick(.01, m, s, 1);  // fade in
        for (int i = 0; i < 500; ++i) {
            head.tick(.01, m, s, 1);
            const double z = head.pose().offset.z;
            if ((z > 0) != (last > 0) && i > 0) ++count;
            last = z; peak = std::max(peak, std::abs(z));
        }
        *amplitude = peak; return count;
    };
    FlightMotion taxi; taxi.groundSpeed = 10; double amplitude = 0;
    const int count = crossings(taxi, headOnly(0, 0, 1, 0), &amplitude);
    CHECK(count >= 2 * 5 * 1); CHECK(count <= 2 * 5 * 2 + 2);  // 1..2 Hz over 5 s
    CHECK(amplitude > .01); CHECK(amplitude <= HeadModel::kBobM + 1e-9);
    taxi.groundSpeed = 0; crossings(taxi, headOnly(0, 0, 1, 0), &amplitude); CHECK_NEAR(amplitude, 0, 1e-12);
    taxi.groundSpeed = 10; taxi.onGround = 0; crossings(taxi, headOnly(0, 0, 1, 0), &amplitude); CHECK_NEAR(amplitude, 0, 1e-12);
    // Idle sway is off by default, very small when enabled while parked, and fades once rolling.
    FlightMotion parked; HeadModel sway; EffectsSettings defaults; defaults.intensity = 1;
    CHECK_NEAR(defaults.idleSway, 0, 1e-12);
    for (int i = 0; i < 500; ++i) sway.tick(.02, parked, defaults, 1);
    CHECK_NEAR(sway.pose().offset.length(), 0, 1e-12);
    double peak = 0;
    for (int i = 0; i < 1000; ++i) { sway.tick(.02, parked, headOnly(0, 0, 0, 1), 1); peak = std::max(peak, sway.pose().offset.length()); }
    CHECK(peak > 1e-3); CHECK(peak < .01);
    CHECK(std::abs(sway.pose().rollDeg) < .5);
    parked.groundSpeed = 40;  // above taxi-bob range: only the fading sway remains
    for (int i = 0; i < 500; ++i) sway.tick(.02, parked, headOnly(0, 0, 0, 1), 1);
    CHECK(sway.pose().offset.length() < 1e-5);
}
TEST_CASE("head model: output stays bounded and within pose limits for extreme input") {
    CameraEffects fx; EffectsSettings settings = headOnly(1, 1, 1, 1); settings.acceleration = 1; settings.turbulence = 1;
    const CameraPose base = pose({.4, -.3, -.8}, {5, 0, 30}, 1.);
    double maxOffset = 0, maxAngle = 0;
    for (int i = 0; i < 4000; ++i) {
        FlightMotion m; const double s = std::sin(i * .37), c = std::cos(i * .91);
        m.onGround = (i / 300) % 2; m.groundSpeed = 60 * std::abs(s); m.gForce = 1 + 20 * c;
        m.forwardAcceleration = 400 * s; m.lateralAcceleration = -300 * c; m.verticalSpeed = -50 * s; m.engineRpm = 3000;
        fx.tick(.001 + .099 * std::abs(std::sin(i * 1.3)), m, settings, true, 1, true);
        const auto& h = fx.head();
        maxOffset = std::max({maxOffset, std::abs(h.offset.x), std::abs(h.offset.y), std::abs(h.offset.z)});
        maxAngle = std::max({maxAngle, std::abs(h.pitchDeg), std::abs(h.rollDeg)});
        const auto out = fx.apply(base);
        CHECK(out.withinLimits());
        CHECK((out.positionFRD - (base.positionFRD + fx.offset() + h.offset)).length() < 1e-12);  // not the fallback
        CHECK_NEAR(out.fovRad, base.fovRad, 1e-12);
    }
    CHECK(maxOffset > .01); CHECK(maxOffset <= HeadModel::kMaxOffsetM);
    CHECK(maxAngle > 1); CHECK(maxAngle <= HeadModel::kMaxAngleDeg);
    CHECK_NEAR(base.positionFRD.x, .4, 1e-12);
}
TEST_CASE("head model: frame rate independent within tolerance for frames up to 0.1 s") {
    const auto run = [](double dt) {
        HeadModel head; const auto settings = headOnly(1, 1, 1, 0);
        FlightMotion m; m.groundSpeed = 8; m.gForce = 1.4; m.forwardAcceleration = 2; m.lateralAcceleration = 3;
        const int steps = static_cast<int>(std::lround(.7 / dt));  // mid-transient
        for (int i = 0; i < steps; ++i) head.tick(dt, m, settings, 1);
        return head.pose();
    };
    const auto reference = run(.001);
    for (const double dt : {1. / 144, 1. / 60, 1. / 30, .07, .1}) {
        const auto p = run(dt);
        CHECK((p.offset - reference.offset).length() < .02 * reference.offset.length() + 2e-4);
        CHECK_NEAR(p.pitchDeg, reference.pitchDeg, .05);
        CHECK_NEAR(p.rollDeg, reference.rollDeg, .05);
    }
}
TEST_CASE("head model: runs only when requested and resets on ineligible frames") {
    CameraEffects fx; EffectsSettings settings; settings.enabled = true;
    FlightMotion motion; motion.onGround = 0; motion.lateralAcceleration = 3; motion.gForce = 1.5;
    const auto moved = [&] { return fx.head().offset.length() > 1e-4 && std::abs(fx.head().rollDeg) > 1e-3; };
    const auto still = [&] { return fx.head().offset.length() == 0 && fx.head().rollDeg == 0 && fx.head().pitchDeg == 0; };
    const auto run = [&](int n, bool eligible, bool head) { for (int i = 0; i < n; ++i) fx.tick(.02, motion, settings, eligible, 1, head); };
    run(50, true, false); CHECK(still());  // exterior/orbit: vehicle effects only
    CHECK(fx.offset().length() > 1e-4);
    run(50, true, true); CHECK(moved());
    run(1, true, false); CHECK(still());
    run(50, true, true); CHECK(moved()); run(1, false, true); CHECK(still());
    run(50, true, true); fx.tick(.5, motion, settings, true, 1, true); CHECK(still());
    run(50, true, true); settings.enabled = false; run(1, true, true); CHECK(still()); settings.enabled = true;
    run(50, true, true); motion.gForce = NAN; run(1, true, true); CHECK(still()); motion.gForce = 1.5;
    run(50, true, true); fx.reset(); CHECK(still());
    // With every head strength at 0 the head stays at rest.
    settings.headLag = settings.turnLean = settings.taxiBob = settings.idleSway = 0;
    run(100, true, true); CHECK(still());
}
TEST_CASE("adapter: automatic mapping ignores accumulated visual wizard corrections") {
    fscs::host::AdapterContract wrong;wrong.pitchSign=1;wrong.headingSign=-1;wrong.axisSign={-1,1,1};
    const nlohmann::json saved={{"records",nlohmann::json::array({{{"simVersion","12.2.282174.999"},{"contract",wrong.toJson()},{"gates",{{"G01","pass"},{"G03","pass"},{"G02","pass"}}}}})}};
    const auto q=fscs::host::qualificationFor(saved,"12.2.282174.999");
    CHECK(q.adapterQualified);CHECK(!q.captureQualified);
    CHECK_NEAR(q.contract.axisSign[0],1,1e-9);CHECK_NEAR(q.contract.axisSign[1],-1,1e-9);CHECK_NEAR(q.contract.axisSign[2],-1,1e-9);
    CHECK_NEAR(q.contract.pitchSign,-1,1e-9);CHECK_NEAR(q.contract.headingSign,1,1e-9);
}
