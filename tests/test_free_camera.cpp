// Free cameras (drones): world-referential activation, Drop drone here, keyboard flight in the drone's own frame with
// height-scaled speed and bounds, Follow aircraft in world axes, aircraft tracking, Save adjusted view and the
// features that do not apply.
#include "test.hpp"

#include "coordinator.hpp"
#include "fake_backend.hpp"
#include "input_router.hpp"
#include "module_app.hpp"
#include "fscs/profiles/airport.hpp"
#include "fscs/profiles/json_util.hpp"

#include <stdexcept>

using namespace fscs::host;
using fscs::profiles::Action;
using fscs::profiles::makeStarterProfile;
using fscs::profiles::newUuid;
using fscs::profiles::toSnapshot;
using fscs::profiles::validateProfile;
using fscs::protocol::Command;
using fscs::protocol::json;
namespace core = fscs::core;
namespace geo = fscs::profiles::airport;
namespace js = fscs::js;
namespace be = backend_event;

namespace {

json quatJson(const core::Quat& q) { return {q.x, q.y, q.z, q.w}; }
geo::Geo submittedGeo(const SdkPose& p) { return {p.x, p.y, p.z}; }
core::Quat submittedOrientation(const SdkPose& p) { return core::Quat::fromPbh({-p.pitchDeg, p.bankDeg, p.headingDeg}); }
double metersBetween(const geo::Geo& a, const geo::Geo& b) { return geo::local(a, b).length(); }

struct FreeRig {
    FakeBackend backend;
    std::vector<std::pair<std::string, json>> events;
    Qualification qual;
    Coordinator c{backend, [this](const std::string& n, json p) { events.emplace_back(n, std::move(p)); },
                  [this](const std::string&) { return qual; }};
    double t = 0;
    int req = 0;
    json profile;
    std::string pilot, exterior, drone;
    fscs::profiles::HostSettings settings;
    std::optional<geo::Geo> aircraft;  // re-sent every frame while set, like the sim-frame telemetry
    double aircraftHeading = 0;
    std::optional<double> terrain;

    FreeRig() {
        qual.adapterQualified = true;
        settings.globalAircraftTracking = "off";
        profile = makeStarterProfile("Free", "asobo-c172", "C172");
        pilot = profile["cameras"][0]["id"];
        exterior = add("Chase", "aircraftExterior", {-20, 3, -5}, core::Quat::fromPbh({-10, 0, 30}));
        drone = add("Drone", "free", {0, 0, -10}, core::Quat::fromPbh({0, 0, 90}));
        cam(drone)["worldOrigin"] = geo::asJson({47, 8, 400});
    }
    std::string add(const char* name, const char* mode, json position, const core::Quat& q) {
        json v = profile["cameras"][0];
        v["id"] = newUuid();
        v["name"] = name;
        v["mode"] = mode;
        v.erase("glance");
        v["viewId"] = 0;
        v["interaction"] = false;
        v["requiredCapabilities"] = json::array();
        v["positionMetersFRD"] = position;
        v["orientationCameraToAnchorXYZW"] = quatJson(q);
        v["transition"] = {{"kind", "smoothstep3"}, {"durationSeconds", 0.5}, {"delaySeconds", 0}};
        profile["cameras"].push_back(v);
        profile["categories"][0]["cameraIds"].push_back(v["id"]);
        return v["id"];
    }
    json& cam(const std::string& id) {
        for (auto& v : profile["cameras"])
            if (v["id"] == id) return v;
        throw std::runtime_error("no camera");
    }
    void deliver(BackendEvent e) { c.onBackendEvent(e, t); }
    json send(const std::string& action, json payload = json::object()) {
        Command cmd;
        cmd.requestId = "t-" + std::to_string(++req);
        cmd.sessionEpoch = c.epoch();
        cmd.action = action;
        cmd.payload = std::move(payload);
        return c.handle(cmd, t);
    }
    void feed() {
        if (aircraft) c.setAircraftPosition(aircraft->lat, aircraft->lon, aircraft->altitude, t, aircraftHeading);
        if (terrain) deliver(be::GroundElevation{*terrain});
    }
    void advance(double dt, int steps = 1) {
        for (int i = 0; i < steps; ++i) {
            t += dt;
            feed();
            c.tick(t);
        }
    }
    void fly(const core::ManualInput& in, double seconds) {
        c.setManualInput(in);
        advance(1.0 / 60, static_cast<int>(std::lround(seconds * 60)));
        c.setManualInput({});
        advance(1.0 / 60, 120);  // let the smoothed rate settle to rest
    }
    void setProfile() {
        json copy = profile;
        const auto validation = validateProfile(copy);
        if (!validation.ok()) std::printf("Validation: %s\n", validation.summary().c_str());
        CHECK(validation.ok());
        c.setProfile(toSnapshot(copy), t);
    }
    void connect() {
        c.tick(t);
        deliver(be::Connected{"1.8.16.0"});
        deliver(be::AircraftLoaded{"SimObjects\\Airplanes\\Asobo-C172\\presets\\classic\\aircraft.cfg"});
        deliver(be::CameraStatus{SdkAvailability::NotAcquired, false});
        c.setSettings(settings, t);
        feed();
        setProfile();
    }
    void applySettings() { c.setSettings(settings, t); }
    void show(const std::string& id) {
        const auto ack = send("ActivateCamera", {{"cameraId", id}});
        CHECK_EQ(ack["status"].get<std::string>(), std::string("Accepted"));
        if (c.ownership() != Ownership::Active) deliver(be::CameraStatus{SdkAvailability::Acquired, false});
        advance(1.0 / 60, 40);  // past any transition
        CHECK(c.ownership() == Ownership::Active);
        CHECK_EQ(c.activeCameraId(), id);
    }
    const SdkPose& last() const { return backend.submitted.back(); }
    std::size_t count(const std::string& name) const {
        std::size_t n = 0;
        for (const auto& e : events) n += e.first == name;
        return n;
    }
    json lastEvent(const std::string& name) const {
        for (auto it = events.rbegin(); it != events.rend(); ++it)
            if (it->first == name) return it->second;
        return nullptr;
    }
    // A free camera from a DroneHere result, added to the profile.
    std::string addDrone(const json& placed, bool follow = false) {
        const auto id = add("Dropped drone", "free", placed["positionMetersFRD"], core::Quat::fromPbh({}));
        cam(id)["orientationCameraToAnchorXYZW"] = placed["orientationCameraToAnchorXYZW"];
        cam(id)["projection"]["value"] = placed["fovRad"];
        cam(id)["worldOrigin"] = placed["worldOrigin"];
        cam(id)["followAircraft"] = follow;
        return id;
    }
};

}  // namespace

TEST_CASE("free: activates as a world-referential preview mode at its saved world origin") {
    CHECK(fscs::profiles::modeActivatable(fscs::profiles::CameraMode::Free));
    FreeRig r;
    r.connect();
    const auto modes = r.c.capabilitiesJson()["modes"]["free"];
    CHECK_EQ(modes["available"], json(true));
    CHECK_EQ(modes["preview"], json(true));
    r.show(r.drone);
    CHECK(r.last().referential == Referential::World);
    CHECK(!r.last().trackUserAircraft);
    CHECK(metersBetween(submittedGeo(r.last()), {47, 8, 410}) < 1e-6);
    CHECK_NEAR(r.last().headingDeg, 90, 1e-9);
    CHECK_NEAR(r.last().pitchDeg, 0, 1e-9);
    CHECK_EQ(r.backend.count("flag:ground:on"), std::size_t{1});  // MSFS keeps the drone out of terrain and buildings
    CHECK_EQ(r.backend.count("get"), std::size_t{0});  // no aircraft-relative pre-read for a world camera
    // The schema field reads back; a missing world origin is a warning, not a broken profile.
    auto doc = r.profile;
    doc["cameras"][2].erase("worldOrigin");
    auto result = validateProfile(doc);
    CHECK(result.ok());
    CHECK(!result.warnings.empty());
    CHECK(!toSnapshot(doc).cameras[2].worldOriginValid);
    CHECK_EQ(doc["cameras"][2]["followAircraft"], json(false));
    doc["cameras"][2]["worldOrigin"] = {{"lat", 95}, {"lon", 8}, {"altitude", 0}};
    CHECK(!validateProfile(doc).ok());
    doc["cameras"][2]["worldOrigin"] = geo::asJson({47, 8, 400});
    doc["cameras"][2]["followAircraft"] = "yes";
    CHECK(!validateProfile(doc).ok());
}

TEST_CASE("free: a free camera without a world position or a follow target cannot activate") {
    FreeRig r;
    r.cam(r.drone).erase("worldOrigin");
    r.connect();
    auto ack = r.send("ActivateCamera", {{"cameraId", r.drone}});
    CHECK_EQ(ack["status"].get<std::string>(), std::string("Rejected"));
    CHECK(js::str(ack, "reason").find("Drop drone here") != std::string::npos);
    r.cam(r.drone)["worldOrigin"] = geo::asJson({47, 8, 400});
    r.cam(r.drone)["followAircraft"] = true;
    r.setProfile();
    ack = r.send("ActivateCamera", {{"cameraId", r.drone}});
    CHECK_EQ(ack["status"].get<std::string>(), std::string("Rejected"));
    CHECK(js::str(ack, "reason").find("aircraft position") != std::string::npos);
    CHECK(r.c.ownership() == Ownership::Observing);
}

TEST_CASE("free: Drop drone here converts an exterior view to the same world pose and back") {
    FreeRig r;
    r.aircraft = geo::Geo{47, 8, 500};
    r.aircraftHeading = 90;
    r.connect();
    r.deliver(be::MotionTelemetry{{}, 0, 0});
    r.show(r.exterior);
    // Level, heading east: 20 m behind = west, 3 m right = south, 5 m up.
    auto ack = r.send("DroneHere");
    CHECK_EQ(ack["status"].get<std::string>(), std::string("Completed"));
    auto placed = ack["result"];
    CHECK_EQ(js::str(placed, "source"), std::string("view"));
    CHECK(metersBetween(geo::geo(placed["worldOrigin"]), {47, 8, 500}) < 1e-9);  // anchored at the aircraft
    CHECK_NEAR(placed["positionMetersFRD"][0].get<double>(), -3, 1e-9);
    CHECK_NEAR(placed["positionMetersFRD"][1].get<double>(), -20, 1e-9);
    CHECK_NEAR(placed["positionMetersFRD"][2].get<double>(), -5, 1e-9);

    // Pitched and banked: the drone appears exactly where the exterior view was, looking the same way.
    r.deliver(be::MotionTelemetry{{}, 10, 5});
    r.advance(1.0 / 60, 30);
    ack = r.send("DroneHere");
    placed = ack["result"];
    const auto attitude = core::Quat::fromPbh({5, 10, 90});
    const auto expected = geo::world({47, 8, 500}, attitude.rotate({-20, 3, -5}));
    const auto expectedOrientation = attitude * core::Quat::fromPbh({-10, 0, 30});
    const auto id = r.addDrone(placed);
    r.setProfile();
    r.show(id);
    CHECK(metersBetween(submittedGeo(r.last()), expected) < 1e-6);
    CHECK(core::angleBetween(submittedOrientation(r.last()), expectedOrientation) < 1e-6);
    CHECK_NEAR(r.last().fovRad, r.cam(r.exterior)["projection"]["value"].get<double>(), 1e-12);
    // And from the drone itself: the same world pose again.
    const auto again = r.send("DroneHere")["result"];
    const auto back = geo::world(geo::geo(again["worldOrigin"]), {again["positionMetersFRD"][0].get<double>(),
                                                                   again["positionMetersFRD"][1].get<double>(),
                                                                   again["positionMetersFRD"][2].get<double>()});
    CHECK(metersBetween(back, expected) < 1e-6);
    const auto& q = again["orientationCameraToAnchorXYZW"];
    CHECK(core::angleBetween({q[0].get<double>(), q[1].get<double>(), q[2].get<double>(), q[3].get<double>()}, expectedOrientation) < 1e-6);
}

TEST_CASE("free: Drop drone here uses the simulator's camera position when it agrees, and a chase spot otherwise") {
    FreeRig r;
    r.aircraft = geo::Geo{47, 8, 500};
    r.aircraftHeading = 0;
    r.connect();
    // Observing: no view of ours on screen, so the drone goes 40 m behind and 12 m above, looking at the aircraft.
    auto placed = r.send("DroneHere")["result"];
    CHECK_EQ(js::str(placed, "source"), std::string("behindAircraft"));
    CHECK_NEAR(placed["positionMetersFRD"][0].get<double>(), -40, 1e-9);
    CHECK_NEAR(placed["positionMetersFRD"][2].get<double>(), -12, 1e-9);
    const auto& q = placed["orientationCameraToAnchorXYZW"];
    const auto pbh = core::Quat{q[0].get<double>(), q[1].get<double>(), q[2].get<double>(), q[3].get<double>()}.toPbh();
    CHECK(pbh.pitchDeg < -16 && pbh.pitchDeg > -17);
    // The cockpit eyepoint is offset from the aircraft origin: the simulator's readback position wins nearby.
    r.show(r.pilot);
    const auto eye = geo::world({47, 8, 500}, {1.2, -0.4, -1.1});
    r.backend.actualWorldPosition = core::Vec3{eye.lat, eye.lon, eye.altitude};
    placed = r.send("DroneHere")["result"];
    const auto& p = placed["positionMetersFRD"];
    CHECK(metersBetween(geo::world(geo::geo(placed["worldOrigin"]), {p[0].get<double>(), p[1].get<double>(), p[2].get<double>()}), eye) < 1e-6);
    // Without an aircraft position there is nothing to convert from.
    FreeRig none;
    none.connect();
    CHECK_EQ(none.send("DroneHere")["status"].get<std::string>(), std::string("Rejected"));
}

TEST_CASE("free: keyboard flight moves in the drone's own frame, faster higher above the ground") {
    auto distance = [](double heightAboveTerrain, bool fast, core::ManualInput in, double pitch = 0) {
        FreeRig r;
        r.aircraft = geo::Geo{47, 8, 400};
        r.terrain = 400;
        r.cam(r.drone)["positionMetersFRD"] = {0, 0, -heightAboveTerrain};
        r.cam(r.drone)["orientationCameraToAnchorXYZW"] = quatJson(core::Quat::fromPbh({pitch, 0, 90}));
        r.connect();
        r.show(r.drone);
        const auto start = submittedGeo(r.last());
        in.fast = fast;
        r.fly(in, 2);
        CHECK(r.c.ownership() == Ownership::Active);
        return geo::local(start, submittedGeo(r.last()));
    };
    core::ManualInput forward;
    forward.moveForward = 1;
    // Facing east at 10 m above terrain: factor 1 + 10 * 0.5 = 6 on 0.5 m/s, for about two seconds.
    const auto low = distance(10, false, forward);
    CHECK_NEAR(low.y, 6.0, 0.3);
    CHECK_NEAR(low.x, 0, 1e-6);
    CHECK_NEAR(low.z, 0, 1e-6);
    const auto high = distance(100, false, forward);  // factor 51
    CHECK_NEAR(high.y / low.y, 51.0 / 6.0, 0.05);
    const auto fast = distance(10, true, forward);  // Shift: four times
    CHECK_NEAR(fast.y / low.y, 4.0, 0.02);
    core::ManualInput right;
    right.moveRight = 1;
    const auto side = distance(10, false, right);  // facing east, right is south
    CHECK(side.x < -5);
    CHECK_NEAR(side.y, 0, 1e-6);
    const auto dive = distance(100, false, forward, -30);  // looking down: forward also descends
    CHECK(dive.z > 0);
    CHECK_NEAR(dive.z / dive.y, std::tan(30 * core::kDegToRad), 0.05);
    core::ManualInput up;
    up.moveUp = 1;
    const auto climb = distance(10, false, up);
    CHECK(climb.z < -5);
    CHECK_NEAR(climb.y, 0, 1e-6);
}

TEST_CASE("free: flight is bounded by reach, ceiling and terrain or the origin floor, and never faults") {
    {  // reach: 20 km from the anchor, far beyond the 10 km anchor limit of aircraft-relative views
        FreeRig r;
        r.aircraft = geo::Geo{47, 8, 400};
        r.terrain = 400;
        r.settings.moveSpeedMetersPerSec = 500;
        r.connect();
        r.show(r.drone);
        core::ManualInput in;
        in.moveForward = 1;
        r.fly(in, 12);
        CHECK(r.c.ownership() == Ownership::Active);
        const auto d = geo::local({47, 8, 400}, submittedGeo(r.last()));
        CHECK(std::hypot(d.x, d.y) <= Coordinator::kFreeRadiusMeters + 1e-6);
        CHECK(std::hypot(d.x, d.y) > Coordinator::kFreeRadiusMeters - 1);
        // The ceiling: 10 km above the anchor.
        in = {};
        in.moveUp = 1;
        r.fly(in, 30);
        CHECK_NEAR(submittedGeo(r.last()).altitude, 400 + Coordinator::kFreeCeilingMeters, 1e-6);
    }
    {  // terrain known (under the aircraft, within 2 km): never below terrain + 2 m
        FreeRig r;
        r.aircraft = geo::Geo{47, 8, 400};
        r.terrain = 400;
        r.connect();
        r.show(r.drone);
        core::ManualInput in;
        in.moveUp = -1;
        r.fly(in, 10);
        CHECK_NEAR(submittedGeo(r.last()).altitude, 400 + Coordinator::kFreeTerrainClearanceMeters, 1e-6);
    }
    {  // no terrain: not more than 300 m below the origin
        FreeRig r;
        r.cam(r.drone)["worldOrigin"] = geo::asJson({47, 8, 1000});
        r.settings.moveSpeedMetersPerSec = 500;
        r.connect();
        r.show(r.drone);
        core::ManualInput in;
        in.moveUp = -1;
        r.fly(in, 5);
        CHECK_NEAR(submittedGeo(r.last()).altitude, 1000 - Coordinator::kFreeFloorMarginMeters, 1e-6);
    }
    {  // a saved pose below the floor is kept, never pushed: it can climb but not descend further
        FreeRig r;
        r.aircraft = geo::Geo{47, 8, 400};
        r.terrain = 400;
        r.cam(r.drone)["positionMetersFRD"] = {0, 0, 5};
        r.connect();
        r.show(r.drone);
        CHECK_NEAR(submittedGeo(r.last()).altitude, 395, 1e-6);
        core::ManualInput in;
        in.moveUp = -1;
        r.fly(in, 2);
        CHECK_NEAR(submittedGeo(r.last()).altitude, 395, 1e-6);
        in.moveUp = 1;
        r.fly(in, 1);
        CHECK(submittedGeo(r.last()).altitude > 395.3);
    }
}

TEST_CASE("free: Follow aircraft keeps the drone's offset in world axes; switching it keeps the drone in place") {
    FreeRig r;
    const geo::Geo a0{47, 8, 500};
    r.aircraft = a0;
    r.aircraftHeading = 0;
    r.cam(r.drone)["worldOrigin"] = geo::asJson(a0);
    r.cam(r.drone)["positionMetersFRD"] = {-30, 0, -10};
    r.cam(r.drone)["followAircraft"] = true;
    r.connect();
    r.show(r.drone);
    CHECK(metersBetween(submittedGeo(r.last()), geo::world(a0, {-30, 0, -10})) < 1e-6);
    const auto heading = r.last().headingDeg;
    // The aircraft flies 100 m north, turns east and climbs: the drone moves with it without rotating.
    const auto a1 = geo::world(a0, {100, 0, -20});
    r.aircraft = a1;
    r.aircraftHeading = 90;
    r.advance(1.0 / 60, 5);
    CHECK(metersBetween(submittedGeo(r.last()), geo::world(a1, {-30, 0, -10})) < 1e-6);
    CHECK_NEAR(r.last().headingDeg, heading, 1e-9);
    // Following, keyboard flight changes the offset, which then stays.
    core::ManualInput in;
    in.moveUp = 1;
    r.fly(in, 1);
    const auto offset = geo::local(a1, submittedGeo(r.last()));
    CHECK(offset.z < -10.5);
    const auto a2 = geo::world(a1, {0, 250, 0});
    r.aircraft = a2;
    r.advance(1.0 / 60, 2);
    CHECK(geo::local(geo::world(a2, offset), submittedGeo(r.last())).length() < 1e-6);
    // Landing: a chase drone is kept above the terrain under the aircraft.
    r.terrain = a2.altitude + 15;
    r.advance(1.0 / 60, 2);
    CHECK_NEAR(submittedGeo(r.last()).altitude, a2.altitude + 15 + Coordinator::kFreeTerrainClearanceMeters, 1e-6);
    r.terrain.reset();
    r.advance(1.0 / 60, 400);  // terrain reading goes stale
    // Switched off in flight: the drone stays where it is and no longer follows.
    const auto here = submittedGeo(r.last());
    r.cam(r.drone)["followAircraft"] = false;
    r.setProfile();
    r.advance(1.0 / 60, 2);
    CHECK(metersBetween(submittedGeo(r.last()), here) < 1e-6);
    r.aircraft = geo::world(a2, {500, 0, 0});
    r.advance(1.0 / 60, 5);
    CHECK(metersBetween(submittedGeo(r.last()), here) < 1e-6);
    // And on again: following from where it is now.
    r.cam(r.drone)["followAircraft"] = true;
    r.setProfile();
    r.advance(1.0 / 60, 2);
    CHECK(metersBetween(submittedGeo(r.last()), here) < 1e-6);
    const auto a3 = geo::world(*r.aircraft, {0, -40, 0});
    const auto kept = geo::local(*r.aircraft, here);
    r.aircraft = a3;
    r.advance(1.0 / 60, 2);
    CHECK(metersBetween(submittedGeo(r.last()), geo::world(a3, kept)) < 1e-6);
    CHECK_EQ(r.count("TemporaryLookBaked"), std::size_t{0});  // nothing is written into the saved pose in flight
}

TEST_CASE("free: global aircraft tracking aims the drone at the aircraft") {
    FreeRig r;
    r.settings.globalAircraftTracking = "both";
    r.cam(r.drone)["positionMetersFRD"] = {0, 0, 0};
    r.cam(r.drone)["orientationCameraToAnchorXYZW"] = quatJson(core::Quat::fromPbh({0, 0, 0}));
    r.aircraft = geo::world({47, 8, 400}, {0, 100, 20});  // 100 m east, 20 m below
    r.connect();
    r.show(r.drone);
    CHECK_NEAR(r.last().headingDeg, 90, 1e-6);
    CHECK_NEAR(-r.last().pitchDeg, -std::atan2(20, 100) * core::kRadToDeg, 1e-6);  // looks down at it
    CHECK(metersBetween(submittedGeo(r.last()), {47, 8, 400}) < 1e-6);  // aiming never moves the drone
    // Forward flies toward where it looks: toward the aircraft.
    core::ManualInput in;
    in.moveForward = 1;
    r.fly(in, 1);
    const auto moved = geo::local({47, 8, 400}, submittedGeo(r.last()));
    CHECK(moved.y > 0.5);
    CHECK(moved.z > 0);
    CHECK_NEAR(moved.x, 0, 1e-6);
    r.settings.globalAircraftTracking = "heading";
    r.applySettings();
    r.advance(1.0 / 60, 2);
    CHECK_NEAR(r.last().pitchDeg, 0, 1e-9);  // heading only keeps the saved pitch
    CHECK(r.last().headingDeg > 80);
    r.settings.globalAircraftTracking = "off";
    r.applySettings();
    r.advance(1.0 / 60, 2);
    CHECK_NEAR(r.last().headingDeg, 0, 1e-9);
}

TEST_CASE("free: Save adjusted view stores the new world origin and pose without moving the drone") {
    FreeRig r;
    const geo::Geo aircraft{47.001, 8, 480};
    r.aircraft = aircraft;
    r.terrain = 400;
    r.connect();
    r.show(r.drone);
    core::ManualInput in;
    in.moveForward = 1;
    in.moveUp = 0.5;
    r.fly(in, 3);
    in = {};
    in.lookRight = 1;
    r.fly(in, 0.5);
    const auto before = r.last();
    const auto world = submittedGeo(before);
    CHECK(metersBetween(world, {47, 8, 410}) > 5);
    CHECK_EQ(r.count("TemporaryLookBaked"), std::size_t{0});
    CHECK_EQ(r.send("BakeTemporaryLook")["status"].get<std::string>(), std::string("Completed"));
    const auto baked = r.lastEvent("TemporaryLookBaked");
    CHECK_EQ(js::str(baked, "cameraId"), r.drone);
    CHECK(metersBetween(geo::geo(baked["worldOrigin"]), aircraft) < 1e-9);  // re-anchored at the aircraft
    const auto& p = baked["positionMetersFRD"];
    const core::Vec3 position{p[0].get<double>(), p[1].get<double>(), p[2].get<double>()};
    CHECK(metersBetween(geo::world(geo::geo(baked["worldOrigin"]), position), world) < 1e-6);
    CHECK(r.c.freeOffset().length() == 0);
    CHECK(!r.c.engine().look().active());
    // Shown at the same place before and after the profile carries the new origin.
    r.advance(1.0 / 60, 3);
    CHECK(metersBetween(submittedGeo(r.last()), world) < 1e-6);
    CHECK_NEAR(r.last().headingDeg, before.headingDeg, 1e-6);
    r.cam(r.drone)["worldOrigin"] = baked["worldOrigin"];
    r.cam(r.drone)["positionMetersFRD"] = baked["positionMetersFRD"];
    r.cam(r.drone)["orientationCameraToAnchorXYZW"] = baked["orientationCameraToAnchorXYZW"];
    r.cam(r.drone)["projection"]["value"] = baked["fovRad"];
    r.setProfile();
    r.advance(1.0 / 60, 3);
    CHECK(metersBetween(submittedGeo(r.last()), world) < 1e-6);
    CHECK_NEAR(r.last().headingDeg, before.headingDeg, 1e-6);
    // Recalling it later starts from the saved place.
    r.show(r.pilot);
    r.show(r.drone);
    CHECK(metersBetween(submittedGeo(r.last()), world) < 1e-6);
}

TEST_CASE("free: flight effects, follow horizon and the gimbal do not apply; binoculars does, glance is not offered") {
    FreeRig r;
    r.cam(r.drone)["effects"] = {{"enabled", true}, {"intensity", 1}};
    r.cam(r.drone)["followHorizon"] = 0;
    r.cam(r.drone)["gimbal"] = "horizon";
    r.connect();
    r.show(r.drone);
    const auto first = r.last();
    const auto submissions = r.backend.submitted.size();
    for (int i = 0; i < 120; ++i) {
        core::FlightMotion motion;
        motion.onGround = i % 2;
        motion.groundSpeed = 30;
        motion.verticalSpeed = (i % 7) - 3;
        motion.gForce = 1 + 0.4 * std::sin(i * 0.3);
        motion.forwardAcceleration = 2;
        motion.engineRpm = 2400;
        r.deliver(be::MotionTelemetry{motion, 30, 10});
        r.advance(1.0 / 60);
    }
    CHECK_EQ(r.backend.submitted.size(), submissions);  // nothing moved the view
    CHECK_NEAR(r.last().bankDeg, first.bankDeg, 1e-12);
    // Glance views work from cockpit views only.
    r.c.onAction({Action::GlanceHold, 0, true, 5}, r.t);
    CHECK(js::str(r.lastEvent("BindingTriggered"), "result").find("cockpit") != std::string::npos);
    CHECK_EQ(r.c.activeCameraId(), r.drone);
    // Binoculars zooms the drone.
    r.c.onAction({Action::Binoculars, 0, true, 6}, r.t);
    r.advance(1.0 / 60, 30);
    CHECK_NEAR(r.last().fovRad, r.settings.binocularsFovDeg * core::kDegToRad, 1e-9);
    r.c.onAction({Action::Binoculars, 0, false, 6}, r.t);
    r.advance(1.0 / 60, 30);
    CHECK_NEAR(r.last().fovRad, first.fovRad, 1e-9);
}

namespace {
struct DroneModuleRig {
    FakeBackend camera;
    FakeServices services;
    FakeStorage storage;
    ModuleApp app{camera, services, storage, "boot-0123456789", "0.1.0-test"};
    double t = 0;
    int req = 0;
    std::size_t seen = 0;
    DroneModuleRig() { app.start(t); }
    std::vector<json> drain() {
        fscs::protocol::Reassembler r;
        std::vector<json> out;
        for (; seen < services.sent.size(); ++seen)
            if (auto whole = r.feed(services.sent[seen], t)) out.push_back(js::parse(*whole));
        return out;
    }
    json ack(const std::string& action, json payload = json::object()) {
        const std::string id = "main-" + std::to_string(++req);
        json msg{{"protocolVersion", 2}, {"requestId", id}, {"uiInstanceId", "main-1"}, {"moduleBootId", app.bootId()},
                 {"sessionEpoch", app.coordinator().epoch()}, {"draftRevision", app.draftRevision()}, {"action", action}, {"payload", payload}};
        app.onEvent(be::CommBusMessage{msg.dump()}, t);
        for (auto& m : drain())
            if (js::str(m, "type") == "ack" && js::str(m, "requestId") == id) return m;
        return nullptr;
    }
    void frame(int n = 1) {
        for (int i = 0; i < n; ++i) {
            t += 1.0 / 60;
            app.onEvent(be::AircraftPosition{47, 8, 500, 0}, t);
            app.update(t);
        }
    }
};
}  // namespace

TEST_CASE("free: the module answers DroneHere and Save adjusted view stores the drone's origin in the aircraft profile") {
    DroneModuleRig m;
    m.frame();
    m.app.onEvent(be::Connected{"1.8.16.0"}, m.t);
    m.app.onEvent(be::AircraftLoaded{"SimObjects\\Airplanes\\Asobo-C152\\aircraft.cfg"}, m.t);
    m.app.onEvent(be::CameraStatus{SdkAvailability::NotAcquired, false}, m.t);
    m.frame();
    m.ack("Hello", {{"surface", "main"}});
    m.ack("CreateProfile", {{"name", "Drones"}, {"forAircraft", true}});
    m.frame(3);
    const auto here = m.ack("DroneHere");
    CHECK_EQ(js::str(here, "status"), std::string("Completed"));
    const auto& placed = here["result"];
    json profile = m.app.workingProfile();
    json drone = profile["cameras"][0];
    drone["id"] = newUuid();
    drone["name"] = "Drone 1";
    drone["mode"] = "free";
    drone["viewId"] = 0;
    drone["worldOrigin"] = placed["worldOrigin"];
    drone["positionMetersFRD"] = placed["positionMetersFRD"];
    drone["orientationCameraToAnchorXYZW"] = placed["orientationCameraToAnchorXYZW"];
    drone["followAircraft"] = false;
    auto categories = profile["categories"];
    categories[0]["cameraIds"].push_back(drone["id"]);
    CHECK_EQ(js::str(m.ack("UpdateDraft", {{"ops", {{{"op", "setCamera"}, {"camera", drone}}, {{"op", "setCategories"}, {"categories", categories}}}}}), "status"),
             std::string("Completed"));
    CHECK_EQ(js::str(m.ack("ActivateCamera", {{"cameraId", drone["id"]}}), "status"), std::string("Accepted"));
    m.app.onEvent(be::CameraStatus{SdkAvailability::Acquired, false}, m.t);
    m.frame(5);
    CHECK_EQ(m.app.coordinator().activeCameraId(), drone["id"].get<std::string>());
    CHECK(m.camera.submitted.back().referential == Referential::World);
    const auto start = geo::Geo{m.camera.submitted.back().x, m.camera.submitted.back().y, m.camera.submitted.back().z};
    // A held panel gesture, renewed like the UI does, flies the drone forward for a second.
    for (int i = 0; i < 20; ++i) {
        m.ack(i ? "RenewGesture" : "BeginGesture", {{"gestureToken", "fly"}, {"input", {{"moveForward", 1}}}});
        m.frame(3);
    }
    m.ack("EndGesture", {{"gestureToken", "fly"}});
    m.frame(60);
    const auto moved = geo::Geo{m.camera.submitted.back().x, m.camera.submitted.back().y, m.camera.submitted.back().z};
    CHECK(metersBetween(start, moved) > 0.3);
    CHECK_EQ(js::str(m.ack("BakeTemporaryLook"), "status"), std::string("Completed"));
    json saved;
    for (const auto& c : m.app.workingProfile()["cameras"])
        if (c["id"] == drone["id"]) saved = c;
    CHECK(saved.is_object());
    CHECK_EQ(js::str(saved, "mode"), std::string("free"));
    const auto& p = saved["positionMetersFRD"];
    CHECK(metersBetween(geo::world(geo::geo(saved["worldOrigin"]), {p[0].get<double>(), p[1].get<double>(), p[2].get<double>()}), moved) < 1e-6);
    CHECK(js::str(m.app.workingProfile(), "scope") != "airport");  // aircraft profile, not the airport store
    m.frame(3);
    const auto after = geo::Geo{m.camera.submitted.back().x, m.camera.submitted.back().y, m.camera.submitted.back().z};
    CHECK(metersBetween(after, moved) < 1e-6);
}

TEST_CASE("free: drone flight is a temporary adjustment: shown as one, cleared by Recenter and by a recall") {
    FreeRig r;
    r.aircraft = geo::Geo{47, 8, 400};
    r.terrain = 400;
    r.connect();
    r.show(r.drone);
    const auto saved = submittedGeo(r.last());
    core::ManualInput in;
    in.moveForward = 1;
    r.fly(in, 1);
    CHECK(metersBetween(submittedGeo(r.last()), saved) > 1);
    CHECK(r.lastEvent("TemporaryLookChanged").value("active", false));
    CHECK_EQ(r.send("RecenterLook")["status"].get<std::string>(), std::string("Completed"));
    r.advance(1.0 / 60, 2);
    CHECK(metersBetween(submittedGeo(r.last()), saved) < 1e-6);
    CHECK(!r.lastEvent("TemporaryLookChanged").value("active", true));
    // Flown, then Follow aircraft switched while another view is shown: the recall starts from the saved pose.
    r.fly(in, 1);
    r.show(r.pilot);
    r.cam(r.drone)["followAircraft"] = true;
    r.setProfile();
    r.aircraft = geo::world({47, 8, 400}, {0, 0, 0});
    r.show(r.drone);
    CHECK(metersBetween(submittedGeo(r.last()), geo::world(*r.aircraft, {0, 0, -10})) < 1e-6);
}
