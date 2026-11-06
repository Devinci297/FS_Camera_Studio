// Spotting: the traffic model (classification, smooth dead-reckoned prediction, stale removal, exclusion), the
// spotting view geometry (tower, runway, chase, fly-by, track; aim, terrain floor, auto zoom), the coordinator's lost /
// release / glance rules and, in the module, the traffic feed, commands, bindings, auto-spot and mutual exclusion.
#include "test.hpp"

#include "coordinator.hpp"
#include "fake_backend.hpp"
#include "module_app.hpp"
#include "traffic.hpp"
#include "fscs/profiles/airport.hpp"
#include "fscs/profiles/json_util.hpp"
#include "airport_fixture.hpp"

#include <algorithm>
#include <functional>
#include <set>

using namespace fscs::host;
using fscs::protocol::Command;
using fscs::protocol::json;
namespace core = fscs::core;
namespace js = fscs::js;
namespace be = backend_event;
namespace ap = fscs::profiles::airport;
namespace tr = fscs::host::traffic;

namespace {

const ap::Geo kAirport{40, -90, 100};  // airportFixture(): runway 09/27 through the origin, 2000 m, tower to the north
constexpr std::uint32_t kUser = 1;

// One aircraft `north`/`east` meters from the airport origin, `height` above its elevation.
be::TrafficAircraft plane(std::uint32_t id, double north, double east, double height, double heading, double speed,
                          double climb = 0, bool ground = false) {
    const auto g = ap::world(kAirport, {north, east, -height});
    be::TrafficAircraft a;
    a.objectId = id;
    a.lat = g.lat;
    a.lon = g.lon;
    a.altitude = g.altitude;
    a.headingDeg = heading;
    a.groundSpeed = speed;
    a.verticalSpeed = climb;
    a.onGround = ground;
    a.groundAltitude = kAirport.altitude;
    a.title = "Airbus A320neo Lufthansa";
    a.airline = "DLH";
    a.flightNumber = std::to_string(id) + "AB";
    a.atcId = "D-AI" + std::to_string(id);
    a.model = "TT:ATCCOM.AC_MODEL_A320.0.text";
    return a;
}

core::Vec3 at(const ap::Geo& g) { return ap::local(kAirport, g); }

}  // namespace

// ---------------------------------------------------------------------------------------------------------------
// Traffic model

TEST_CASE("spotting: status classification follows the contract rules, with landing memory and the airport radius") {
    using tr::Status;
    CHECK(tr::classify(true, 0, 0, false, true) == Status::Parked);
    CHECK(tr::classify(true, .99, 0, false, true) == Status::Parked);
    CHECK(tr::classify(true, 1, 0, false, true) == Status::Taxi);
    CHECK(tr::classify(true, 39.9, 0, true, true) == Status::Taxi);
    CHECK(tr::classify(true, 40, 0, false, true) == Status::Takeoff);
    CHECK(tr::classify(true, 40, 0, true, true) == Status::Landing);
    CHECK(tr::classify(false, 150, 301, false, true) == Status::Departing);
    CHECK(tr::classify(false, 150, 300, false, true) == Status::Cruise);
    CHECK(tr::classify(false, 150, -301, false, true) == Status::Arriving);
    CHECK(tr::classify(false, 150, -1500, false, false) == Status::Cruise);  // not near the airport
    CHECK(tr::classify(false, 150, 1500, false, false) == Status::Cruise);
    CHECK_EQ(std::string(tr::toString(Status::Landing)), std::string("landing"));

    tr::Model m;
    m.setAirport(kAirport);
    double t = 10;
    const auto statusOf = [&](std::uint32_t id) { return m.find(id)->status; };
    m.update({plane(2, 0, 0, 0, 90, .3, 0, true), plane(3, 0, 300, 0, 90, 8, 0, true), plane(4, 0, -800, 0, 90, 30, 0, true),
              plane(5, 0, -3000, 160, 90, 70, -4), plane(6, 0, 3000, 300, 90, 80, 6), plane(7, 0, 20 * 1852., 900, 90, 80, 6),
              plane(8, 0, -6000, 300, 90, 70, -4)}, kUser, kAirport, t);
    CHECK(statusOf(2) == Status::Parked);
    CHECK(statusOf(3) == Status::Taxi);
    CHECK(statusOf(4) == Status::Takeoff);   // never seen airborne
    CHECK(statusOf(5) == Status::Arriving);
    CHECK(statusOf(6) == Status::Departing);
    CHECK(statusOf(7) == Status::Cruise);    // 20 NM out
    // Touchdown: airborne in the last poll, on the runway at speed now -> landing; slows to taxi; 60 s later a fast
    // roll is a takeoff again.
    t += 1.5;
    m.update({plane(8, 0, -1000, 0, 90, 65, -1, true)}, kUser, kAirport, t);
    CHECK(statusOf(8) == Status::Landing);
    t += 1.5;
    m.update({plane(8, 0, -500, 0, 90, 15, 0, true)}, kUser, kAirport, t);
    CHECK(statusOf(8) == Status::Taxi);
    for (int i = 0; i < 41; ++i) { t += 1.5; m.update({plane(8, 0, -500, 0, 90, 5, 0, true)}, kUser, kAirport, t); }
    m.update({plane(8, 0, -500, 0, 90, 30, 0, true)}, kUser, kAirport, t + 1.5);
    CHECK(statusOf(8) == Status::Takeoff);
    // Without an airport nothing airborne is departing or arriving.
    tr::Model none;
    none.update({plane(5, 0, -3000, 160, 90, 70, -4)}, kUser, kAirport, 0);
    CHECK(none.find(5)->status == Status::Cruise);
    CHECK(std::isnan(none.airportDistanceNm(5, 0)));
}

TEST_CASE("spotting: prediction dead-reckons between polls and blends each poll in without a step at 60 fps") {
    // Straight and level at 70 m/s: the prediction matches the truth between polls and stops 4 s after the last one.
    {
        tr::Model m;
        const auto truth = [](double t) { return plane(9, 0, -5000 + 70 * t, 300, 90, 70); };
        for (double t = 0; t <= 6; t += 1.5) m.update({truth(t)}, kUser, kAirport, t);
        for (double t = 6; t < 7.5; t += .1) {
            const auto p = m.predict(9, t);
            CHECK(p.valid());
            CHECK_NEAR((at(p.position) - at({truth(t).lat, truth(t).lon, truth(t).altitude})).length(), 0.0, .3);
            CHECK_NEAR(p.velocity.y, 70, .5);
        }
        CHECK_NEAR((at(m.predict(9, 10).position) - at(m.predict(9, 12).position)).length(), 0.0, 1e-6);
        CHECK(m.predict(9, 12.01).valid() == false);  // stale after 6 s
    }
    // A turning, climbing target, polled every 1.5 s and drawn at 60 fps: no step at a poll boundary, and it stays on
    // the curve. Reports either exact or jittered (+-1.5 m, +-0.5 m vertical, +-0.25 deg: about a frame of timing).
    std::uint64_t seed = 12345;
    const auto noise = [&] {
        seed = seed * 6364136223846793005ull + 1442695040888963407ull;
        return static_cast<double>(seed >> 11) / 9007199254740992.0 * 2 - 1;
    };
    const auto truth = [&](double t, double jitter) {
        const double rate = 1.5;  // degrees per second, turning right from east
        const double r = 70 / (rate * core::kDegToRad), a = rate * t * core::kDegToRad;
        auto p = plane(11, -(r - r * std::cos(a)), r * std::sin(a), 300 + 3 * t, 90 + rate * t, 70, 3);
        if (jitter > 0) {
            const auto g = ap::world({p.lat, p.lon, p.altitude}, {jitter * noise(), jitter * noise(), jitter / 3 * noise()});
            p.lat = g.lat; p.lon = g.lon; p.altitude = g.altitude;
            p.headingDeg += jitter / 6 * noise();
        }
        return p;
    };
    struct Run { double maxAccel = 0, maxStep = 0, maxTurn = 0, maxError = 0; };
    const double dt = 1.0 / 60;
    const auto fly = [&](double jitter) {
        tr::Model m;
        Run out;
        double nextPoll = 0;
        std::vector<core::Vec3> path;
        std::vector<double> headings;
        for (int frame = 0; frame < 60 * 40; ++frame) {
            const double t = frame * dt;
            if (t >= nextPoll - 1e-9) { m.update({truth(t, jitter)}, kUser, kAirport, t); nextPoll += 1.5; }
            const auto p = m.predict(11, t);
            CHECK(p.valid());
            path.push_back(at(p.position));
            headings.push_back(p.headingDeg);
            const auto real = truth(t, 0);
            if (t > 3) out.maxError = std::max(out.maxError, (path.back() - at({real.lat, real.lon, real.altitude})).length());
        }
        for (std::size_t i = 2; i < path.size(); ++i) {
            const auto accel = (path[i] - path[i - 1] * 2 + path[i - 2]) * (1 / (dt * dt));
            if (i > 180) out.maxAccel = std::max(out.maxAccel, accel.length());  // once the turn rate is known
            out.maxStep = std::max(out.maxStep, (path[i] - path[i - 1]).length());
            out.maxTurn = std::max(out.maxTurn, std::abs(core::wrapDeg(headings[i] - headings[i - 1])));
        }
        return out;
    };
    // Exact reports: the arc dead reckoning follows the turn (1.8 m/s^2 centripetal) with almost nothing to correct.
    const auto exact = fly(0);
    CHECK(exact.maxAccel < 3);
    CHECK(exact.maxError < 1);
    // Jittered: an unblended 1.5 m correction would be a 5,000 m/s^2 spike in one frame; the blend eases it in.
    const auto jittered = fly(1.5);
    CHECK(jittered.maxAccel < 25);
    CHECK(jittered.maxStep < 70 * dt * 1.2);
    CHECK(jittered.maxTurn < .25);  // degrees per frame, 15 deg/s at most
    CHECK(jittered.maxError < 5);
    // A jump (slew, warp) snaps instead of sweeping the camera across the sky.
    tr::Model j;
    j.update({plane(12, 0, 0, 300, 90, 70)}, kUser, kAirport, 0);
    j.update({plane(12, 0, 5000, 300, 90, 70)}, kUser, kAirport, 1.5);
    CHECK_NEAR(at(j.predict(12, 1.5).position).y, 5000, 1e-6);
}

TEST_CASE("spotting: stale aircraft are dropped and the user aircraft, object 0 and bad samples are excluded") {
    tr::Model m;
    auto user = plane(kUser, 0, 0, 0, 0, 0, 0, true);
    auto zero = plane(0, 0, 100, 0, 0, 0, 0, true);
    auto bad = plane(20, 0, 200, 0, 0, 0, 0, true);
    bad.lat = NAN;
    auto noHeading = plane(21, 0, 300, 0, NAN, 0, 0, true);
    m.update({user, zero, bad, noHeading, plane(22, 0, 400, 0, 0, 0, 0, true)}, kUser, kAirport, 0);
    CHECK_EQ(m.size(), std::size_t{1});
    CHECK(m.find(22) != nullptr);
    // While the user's id is unknown, an entry at the user's own position is the user.
    const ap::Geo ownship{user.lat, user.lon, user.altitude};
    m.update({user, plane(23, 0, 2, 0, 0, 0, 0, true)}, 0, ownship, 1);
    CHECK(m.find(kUser) == nullptr);
    CHECK(m.find(23) != nullptr);
    // Not seen for more than 6 s: gone (missing from a poll or two is fine).
    const auto revision = m.revision();
    m.update({plane(23, 0, 2, 0, 0, 0, 0, true)}, 0, ownship, 6);
    CHECK(!m.prune(6));
    CHECK(m.find(22) != nullptr);
    CHECK(m.prune(6.1));
    CHECK(m.find(22) == nullptr);
    CHECK(m.find(23) != nullptr);
    CHECK(m.revision() > revision);
    CHECK(!m.predict(22, 7).valid());
    m.clear();
    CHECK_EQ(m.size(), std::size_t{0});
    // ATC strings: localisation keys become readable; control bytes are dropped; labels read like a callsign.
    CHECK_EQ(tr::cleanAtcText("TT:ATCCOM.AC_MODEL_A320.0.text"), std::string("A320"));
    CHECK_EQ(tr::cleanAtcText("ATCCOM.AC_MODEL A20N.0.text"), std::string("A20N"));
    CHECK_EQ(tr::cleanAtcText("TT:ATCCOM.ATC_NAME DELTA.0.text"), std::string("DELTA"));
    CHECK_EQ(tr::cleanAtcText("  N172SP \t"), std::string("N172SP"));
    be::TrafficAircraft s;
    s.objectId = 5;
    s.airline = "DLH"; s.flightNumber = "4AB"; s.model = "A320";
    CHECK_EQ(tr::label(s), std::string("DLH4AB - A320"));
    s.airline = "Lufthansa";
    CHECK_EQ(tr::label(s), std::string("Lufthansa 4AB - A320"));
    s.airline.clear(); s.atcId = "N123AB"; s.model.clear();
    CHECK_EQ(tr::label(s), std::string("N123AB"));
    s.atcId.clear(); s.title = "Cessna 172";
    CHECK_EQ(tr::label(s), std::string("Cessna 172"));
    // The list: nearest first, user excluded, contract fields and units.
    tr::Model list;
    list.setAirport(kAirport);
    list.update({plane(30, 0, 5000, 300, 270, 70, -4), plane(31, 0, 1852, 0, 90, 0, 0, true), user}, kUser, kAirport, 3);
    const auto doc = list.toJson(kAirport, 3.5);
    const auto& aircraft = doc["aircraft"];
    CHECK_EQ(aircraft.size(), std::size_t{2});
    CHECK_EQ(aircraft[0]["objectId"].get<std::uint32_t>(), 31u);
    CHECK_NEAR(aircraft[0]["distanceNm"].get<double>(), 1.0, .01);
    CHECK_NEAR(aircraft[0]["bearingDeg"].get<double>(), 90, 1);
    CHECK_EQ(aircraft[0]["status"].get<std::string>(), std::string("parked"));
    CHECK_EQ(aircraft[1]["status"].get<std::string>(), std::string("arriving"));
    CHECK_NEAR(aircraft[1]["altitudeFt"].get<double>(), (400 - 4 * .5) / .3048, 1);  // predicted to `now`
    CHECK_NEAR(aircraft[1]["groundSpeedKt"].get<double>(), 136, 1);
    CHECK_NEAR(aircraft[1]["verticalSpeedFpm"].get<double>(), -790, 10);
    CHECK_NEAR(aircraft[1]["headingDeg"].get<double>(), 270, .5);
    CHECK_EQ(aircraft[1]["model"].get<std::string>(), std::string("A320"));
    for (const char* key : {"objectId", "title", "atcId", "airline", "flightNumber", "model", "latitude", "longitude", "altitudeFt",
                            "groundSpeedKt", "verticalSpeedFpm", "headingDeg", "onGround", "distanceNm", "bearingDeg", "status"})
        CHECK(aircraft[1].contains(key));
    CHECK_EQ(doc["at"].get<std::int64_t>(), std::int64_t{3500});  // session milliseconds
}

TEST_CASE("spotting: view geometry - aim, chase offset, fly-by placement and passing, terrain floor, auto zoom") {
    // Aim: north and 100 m below is heading 0, pitch -45.
    const auto look = tr::aim(kAirport, ap::world(kAirport, {100, 0, 100}));
    CHECK(look.has_value());
    CHECK_NEAR(look->headingDeg, 0, 1e-6);
    CHECK_NEAR(look->pitchDeg, -45, 1e-6);
    CHECK(!tr::aim(kAirport, ap::world(kAirport, {.1, .1, 0})).has_value());
    // Chase: 60 m behind along the track, 15 m above.
    tr::Pose p;
    p.position = ap::world(kAirport, {0, 0, -300});
    p.headingDeg = 0;
    p.velocity = {0, 70, 0};  // flying east whatever the nose says (crab)
    const auto chase = tr::chaseOffset(p);
    CHECK_NEAR(chase.x, 0, 1e-9);
    CHECK_NEAR(chase.y, -60, 1e-9);
    CHECK_NEAR(chase.z, -15, 1e-9);
    p.velocity = {};
    CHECK_NEAR(tr::chaseOffset(p).x, -60, 1e-9);  // parked: behind the nose
    // Follow eases (frame-rate independent) and starts at the target when unseeded.
    const core::Vec3 nan{NAN, NAN, NAN};
    CHECK_NEAR(tr::follow(nan, {1, 2, 3}, .01).y, 2, 1e-12);
    const auto once = tr::follow({0, 0, 0}, {10, 0, 0}, .2), twice = tr::follow(tr::follow({0, 0, 0}, {10, 0, 0}, .1), {10, 0, 0}, .1);
    CHECK_NEAR(once.x, twice.x, 1e-9);
    // Fly-by: 400 m ahead, 30 m right, at the altitude the target will have when it gets there (+3 m).
    p.velocity = {0, 80, -8};  // east, climbing 8 m/s
    p.headingDeg = 90;
    const auto flyby = at(tr::flybyPosition(p));
    CHECK_NEAR(flyby.y, 400, 1e-6);
    CHECK_NEAR(flyby.x, -30, 1e-6);  // right of an eastbound track is south
    CHECK_NEAR(-flyby.z, 300 + 8 * 5 + 3, 1e-6);
    const auto camera = tr::flybyPosition(p);
    CHECK(!tr::flybyPassed(camera, p));
    tr::Pose later = p;
    later.position = ap::world(kAirport, {0, 699, -340});
    CHECK(!tr::flybyPassed(camera, later));
    later.position = ap::world(kAirport, {0, 701, -340});
    CHECK(tr::flybyPassed(camera, later));
    later.position = ap::world(kAirport, {1500, 0, -340});  // turned away
    CHECK(tr::flybyPassed(camera, later));
    // Terrain floor: the higher known terrain plus 2 m.
    CHECK_NEAR(tr::terrainFloor(100, NAN), 102, 1e-12);
    CHECK_NEAR(tr::terrainFloor(100, 140), 142, 1e-12);
    CHECK(std::isnan(tr::terrainFloor(NAN, NAN)));
    // Auto zoom frames 40 m at 1 km in 2.29 degrees, clamped.
    CHECK_NEAR(tr::autoZoomFov(1000, 40, 0, 2), 2 * std::atan(.02), 1e-12);
    CHECK_NEAR(tr::autoZoomFov(1000, 40, .2, 2), .2, 1e-12);
    CHECK_NEAR(tr::easeFov(1, 0, 1, 1), std::exp(-1), 1e-12);
}

// ---------------------------------------------------------------------------------------------------------------
// Coordinator: the views

namespace {
struct SpotRig {
    FakeBackend backend;
    std::vector<std::pair<std::string, json>> events;
    Qualification qual;
    Coordinator c{backend, [this](const std::string& n, json p) { events.emplace_back(n, std::move(p)); },
                  [this](const std::string&) { return qual; }};
    double t = 0, nextPoll = 0;
    int req = 0;
    json profile;
    std::string pilot, tower;
    std::function<std::vector<be::TrafficAircraft>(double)> truth;
    std::function<std::optional<be::TrafficTarget>(double)> frames;  // per-frame data for the watched target
    explicit SpotRig(bool airport = true, bool withProfile = true) {
        qual.adapterQualified = true;
        qual.captureQualified = true;
        profile = fscs::profiles::makeC172Profile("asobo-c172", "C172");
        pilot = js::str(profile, "defaultCameraId");
        if (airport) {
            // The loaded airport set, combined into the runtime profile as the module does.
            std::vector<std::string> notes;
            const auto doc = ap::generate(airportFixture(), ap::automaticOptions(json::object()), notes);
            CHECK(doc.is_object());
            for (const auto& cat : doc["categories"]) profile["categories"].push_back(cat);
            for (const auto& cam : doc["cameras"]) {
                profile["cameras"].push_back(cam);
                if (js::str(js::object(cam, "airportHeight"), "category") == "tower") tower = js::str(cam, "id");
            }
            CHECK(!tower.empty());
        }
        c.tick(t);
        c.onBackendEvent(be::Connected{"1.8.16.0"}, t);
        c.onBackendEvent(be::AircraftLoaded{"SimObjects\\Airplanes\\asobo-c172\\aircraft.cfg"}, t);
        c.onBackendEvent(be::CameraStatus{SdkAvailability::NotAcquired, false}, t);
        if (withProfile) c.setProfile(fscs::profiles::toSnapshot(profile), t);
        own();
    }
    void own() { const auto g = ap::world(kAirport, {-300, 0, 0}); c.setAircraftPosition(g.lat, g.lon, g.altitude, t, 90); }
    json send(const std::string& action, json payload = json::object()) {
        Command cmd;
        cmd.requestId = "c-" + std::to_string(++req);
        cmd.sessionEpoch = c.epoch();
        cmd.action = action;
        cmd.payload = std::move(payload);
        return c.handle(cmd, t);
    }
    void poll() { c.onBackendEvent(be::Traffic{truth(t), kUser}, t); }
    // Advances in 10 ms frames, polling the truth every 1.5 s (when there is one).
    void advance(double seconds) {
        for (int i = 0; i < static_cast<int>(std::lround(seconds * 100)); ++i) {
            t += .01;
            if (truth && t >= nextPoll) { poll(); nextPoll = t + 1.5; }
            if (frames) if (const auto f = frames(t)) c.onBackendEvent(*f, t);
            own();
            c.tick(t);
        }
    }
    void acquire() {
        if (c.ownership() == Ownership::Acquiring) c.onBackendEvent(be::CameraStatus{SdkAvailability::Acquired, false}, t);
        advance(.05);
        CHECK(c.ownership() == Ownership::Active);
    }
    const SdkPose& shown() const { return backend.submitted.back(); }
    ap::Geo camera() const { return {shown().x, shown().y, shown().z}; }
    // Where the submitted view looks, as a world heading / pitch (pitch up > 0).
    core::Pbh looking() const { return {-shown().pitchDeg, 0, shown().headingDeg}; }
    const fscs::profiles::CameraEntry* entry(const std::string& id) const { return c.profile()->camera(id); }
    std::vector<std::string> runwayViews() const {
        std::vector<std::string> out;
        for (const auto& cam : c.profile()->cameras) if (cam.airportHeightCategory == "runways") out.push_back(cam.id);
        return out;
    }
    std::size_t count(const std::string& name) const {
        return static_cast<std::size_t>(std::count_if(events.begin(), events.end(), [&](const auto& e) { return e.first == name; }));
    }
    void checkAimed() const {
        const auto expected = tr::aim(camera(), c.spotPose().position);
        CHECK(expected.has_value());
        CHECK_NEAR(core::wrapDeg(looking().headingDeg - expected->headingDeg), 0, .01);
        CHECK_NEAR(looking().pitchDeg, expected->pitchDeg, .01);
    }
};
}  // namespace

TEST_CASE("spotting coordinator: chase follows 60 m behind and 15 m above, aimed every frame, never below terrain") {
    SpotRig r;
    r.truth = [](double t) { return std::vector<be::TrafficAircraft>{plane(7, 0, -6000 + 70 * t, 300, 90, 70)}; };
    r.advance(.1);
    CHECK(r.c.ownership() == Ownership::Observing);
    // Takes control with the default view underneath; the shown pose is the computed world pose.
    CHECK_EQ(r.c.spotTarget(7, tr::SpotView::Chase, r.t), std::string());
    CHECK(r.c.spotting());
    CHECK(r.c.ownership() == Ownership::Acquiring);
    r.acquire();
    r.events.clear();
    r.advance(3);
    CHECK_EQ(r.c.activeCameraId(), r.pilot);  // the base view underneath
    CHECK(r.c.spotOverlay());
    CHECK(r.shown().referential == Referential::World);
    CHECK(!r.shown().trackUserAircraft);
    const auto offset = ap::local(r.c.spotPose().position, r.camera());
    CHECK_NEAR(offset.x, 0, .5);
    CHECK_NEAR(offset.y, -60, .5);
    CHECK_NEAR(offset.z, -15, .1);
    r.checkAimed();
    CHECK_NEAR(r.looking().pitchDeg, -std::atan2(15, 60) * core::kRadToDeg, .5);
    CHECK_NEAR(r.shown().fovRad, tr::kChaseFovDeg * core::kDegToRad, 1e-9);
    CHECK(std::find(r.backend.calls.begin(), r.backend.calls.end(), "flag:ground:on") != r.backend.calls.end());
    // Smooth at 100 fps across poll boundaries: the camera's steps stay even (no jitter).
    double maxJerk = 0;
    ap::Geo prev2{NAN, NAN, NAN}, prev{NAN, NAN, NAN};
    for (int i = 0; i < 400; ++i) {
        r.advance(.01);
        const auto now = r.camera();
        if (prev2.valid()) maxJerk = std::max(maxJerk, (at(now) - at(prev) * 2 + at(prev2)).length());
        prev2 = prev;
        prev = now;
    }
    CHECK(maxJerk < .01);  // meters of second difference per 10 ms frame
    // A turn swings the chase round instead of snapping it.
    r.truth = [](double t) { return std::vector<be::TrafficAircraft>{plane(7, 70 * (t - 7.15), 0, 300, 0, 70)}; };
    r.nextPoll = 0;
    r.advance(.02);
    const auto swung = ap::local(r.c.spotPose().position, r.camera());
    CHECK(swung.y < -40);  // still mostly to the west
    r.advance(6);
    const auto settled = ap::local(r.c.spotPose().position, r.camera());
    CHECK_NEAR(settled.x, -60, 1);
    CHECK_NEAR(settled.y, 0, 1);
    // Terrain floor: a taxiing target below a rise: the chase stays 2 m above the terrain under the target.
    r.truth = [](double t) {
        auto a = plane(7, 0, 50 * (t - 13), 0, 90, 8, 0, true);
        a.groundAltitude = kAirport.altitude + 30;  // the camera behind would be inside the hill
        return std::vector<be::TrafficAircraft>{a};
    };
    r.nextPoll = 0;
    r.advance(4);
    CHECK(r.camera().altitude >= kAirport.altitude + 30 + tr::kTerrainClearanceMeters - 1e-6);
    CHECK_NEAR(r.camera().altitude, kAirport.altitude + 32, 1e-6);
    r.checkAimed();
}

TEST_CASE("spotting coordinator: fly-by stands still ahead of the target and is placed again once it has passed") {
    SpotRig r;
    r.truth = [](double t) { return std::vector<be::TrafficAircraft>{plane(7, 0, -6000 + 80 * t, 300, 90, 80)}; };
    r.send("ActivateCamera", {{"cameraId", r.pilot}});
    r.acquire();
    r.advance(1);
    CHECK_EQ(r.c.spotTarget(7, tr::SpotView::Flyby, r.t), std::string());
    r.advance(.05);
    const auto placed = r.camera();
    const auto rel = ap::local(r.c.spotPose().position, placed);
    CHECK_NEAR(rel.y, 400, 5);
    CHECK_NEAR(rel.x, -30, 1);
    CHECK_NEAR(-rel.z, 3, 1);
    r.checkAimed();
    // Stationary while it comes past; the zoom widens as it closes (airport auto zoom settings).
    const double farFov = r.shown().fovRad;
    r.advance(4.5);
    CHECK_NEAR((at(r.camera()) - at(placed)).length(), 0, 1e-6);
    CHECK(r.shown().fovRad > farFov);
    r.checkAimed();
    // Past and 300 m beyond (700 m of travel, under 9 s): placed again ahead.
    r.advance(4.5);
    const auto again = r.camera();
    CHECK((at(again) - at(placed)).length() > 500);
    CHECK_NEAR(ap::local(r.c.spotPose().position, again).y, 400, 30);
    CHECK_EQ(r.c.activeCameraId(), r.pilot);
}

TEST_CASE("spotting coordinator: tower and runway views aim the loaded airport's views at the target and zoom on it") {
    SpotRig r;
    r.truth = [](double t) { return std::vector<be::TrafficAircraft>{plane(7, 0, -1000 + 60 * t, 0, 90, 60, 0, true)}; };
    r.send("ActivateCamera", {{"cameraId", r.pilot}});
    r.acquire();
    r.advance(.2);
    CHECK_EQ(r.c.spotTarget(7, tr::SpotView::Tower, r.t), std::string());
    CHECK_EQ(r.c.activeCameraId(), r.tower);
    CHECK(!r.c.spotOverlay());
    r.advance(4);
    r.checkAimed();
    // Spotting zooms on the target: the default 40 m frame at the target's distance (10 to 90 degrees).
    const double towerDistance = (at(r.camera()) - at(r.c.spotPose().position)).length();
    CHECK_NEAR(r.shown().fovRad, tr::autoZoomFov(towerDistance, 40, 10 * core::kDegToRad, 90 * core::kDegToRad), .02);
    CHECK(r.shown().fovRad < 30 * core::kDegToRad);
    // Runway: the runway or spotting view nearest the target, then the nearer one as it rolls (6 s dwell, margin).
    CHECK_EQ(r.c.spotTarget(7, tr::SpotView::Runway, r.t), std::string());
    const auto nearestTo = [&](const ap::Geo& target) {
        std::string best;
        double d = 1e18;
        for (const auto& id : r.runwayViews()) {
            const auto* cam = r.entry(id);
            const auto pose = cam->pose;
            const double x = ap::local(ap::world({cam->originLat, cam->originLon, cam->originAltitude}, pose.positionFRD), target).length();
            if (x < d) { d = x; best = id; }
        }
        return best;
    };
    CHECK_EQ(r.c.activeCameraId(), nearestTo(r.c.spotPose().position));
    const auto first = r.c.activeCameraId();
    r.advance(1);
    r.checkAimed();
    r.advance(25);  // rolled 1.5 km east
    CHECK(r.c.activeCameraId() != first);
    CHECK_EQ(r.c.activeCameraId(), nearestTo(r.c.spotPose().position));
    r.checkAimed();
    // Track keeps the airport view and only aims it.
    const auto runway = r.c.activeCameraId();
    CHECK_EQ(r.c.spotTarget(7, tr::SpotView::Track, r.t), std::string());
    r.advance(10);
    CHECK_EQ(r.c.activeCameraId(), runway);
    r.checkAimed();
    // Over a chase, track holds the camera where it is and keeps aiming.
    CHECK_EQ(r.c.spotTarget(7, tr::SpotView::Chase, r.t), std::string());
    r.advance(2);
    const auto held = r.camera();
    CHECK_EQ(r.c.spotTarget(7, tr::SpotView::Track, r.t), std::string());
    r.advance(3);
    CHECK_NEAR((at(r.camera()) - at(held)).length(), 0, 1e-6);
    r.checkAimed();
    // StopSpotting returns to the view shown before spotting began.
    r.c.stopSpotting("Spotting stopped.", r.t);
    CHECK(!r.c.spotting());
    CHECK_EQ(r.c.activeCameraId(), r.pilot);
    CHECK(r.c.ownership() == Ownership::Active);
}

TEST_CASE("spotting coordinator: rejections - no airport views, track without a world view, unknown target") {
    SpotRig r(false);
    r.truth = [](double t) { return std::vector<be::TrafficAircraft>{plane(7, 0, -1000 + 60 * t, 0, 90, 60, 0, true)}; };
    r.advance(.1);
    CHECK_EQ(r.c.spotTarget(7, tr::SpotView::Tower, r.t), std::string("No tower position is known for an airport near this aircraft yet."));
    CHECK_EQ(r.c.spotTarget(7, tr::SpotView::Runway, r.t), std::string("No runway data is known for an airport near this aircraft yet."));
    CHECK_EQ(r.c.spotTarget(99, tr::SpotView::Chase, r.t), std::string("That aircraft is no longer in range."));
    r.send("ActivateCamera", {{"cameraId", r.pilot}});
    r.acquire();
    CHECK_EQ(r.c.spotTarget(7, tr::SpotView::Track, r.t), std::string("Track needs an airport or free camera on screen."));
    CHECK(!r.c.spotting());
    CHECK(!r.c.spotViewAvailable(tr::SpotView::Tower, 7, r.t));
    CHECK(r.c.spotViewAvailable(tr::SpotView::Chase, 7, r.t));
    CHECK(!r.c.spotViewAvailable(tr::SpotView::Track, 7, r.t));
    CHECK(tr::parseSpotView("flyby") == tr::SpotView::Flyby);
    CHECK(!tr::parseSpotView("fly-by").has_value());
}

TEST_CASE("spotting coordinator: a lost target holds the view 5 s then returns; recalls, release, panic and glance") {
    SpotRig r;
    r.truth = [](double t) { return std::vector<be::TrafficAircraft>{plane(7, 0, -6000 + 70 * t, 300, 90, 70)}; };
    r.send("ActivateCamera", {{"cameraId", r.pilot}});
    r.acquire();
    r.advance(1);
    CHECK_EQ(r.c.spotTarget(7, tr::SpotView::Chase, r.t), std::string());
    r.advance(2);
    // ActiveCameraChanged reports no camera for chase and fly-by.
    json active;
    for (const auto& [n, p] : r.events) if (n == "ActiveCameraChanged") active = p;
    CHECK(active["cameraId"].is_null());
    // Glance is refused while spotting; binoculars still zoom the chase.
    r.events.clear();
    r.c.onAction({fscs::profiles::Action::GlanceNext, 0, true}, r.t);
    json triggered;
    for (const auto& [n, p] : r.events) if (n == "BindingTriggered") triggered = p;
    CHECK(js::str(triggered, "result").find("while spotting") != std::string::npos);
    r.c.onAction({fscs::profiles::Action::Binoculars, 0, true, 5}, r.t);
    r.advance(1);
    CHECK(r.shown().fovRad < tr::kChaseFovDeg * core::kDegToRad * .5);
    r.c.onAction({fscs::profiles::Action::Binoculars, 0, false, 5}, r.t);
    r.advance(1);
    // Lost: no more polls. Dropped after 6 s, then the view holds for 5 s, then back to the earlier view.
    r.truth = nullptr;
    r.advance(6.2);
    CHECK(r.c.spotLost());
    const auto heldAt = r.camera();
    const auto submissions = r.c.submissions();
    r.advance(Coordinator::kSpotLostHoldSeconds - .3 - r.c.spotLostSeconds(r.t));  // lost since the 6 s stale point
    CHECK(r.c.spotting());
    CHECK_NEAR((at(r.camera()) - at(heldAt)).length(), 0, 1e-9);
    CHECK_EQ(r.c.submissions(), submissions);  // nothing moves while held
    r.advance(1);
    CHECK(!r.c.spotting());
    CHECK_EQ(r.c.spotReason(), std::string("The aircraft is no longer in range."));
    CHECK_EQ(r.c.activeCameraId(), r.pilot);
    CHECK(r.c.ownership() == Ownership::Active);
    // From Observing (nothing to return to), stopping releases.
    r.truth = [](double t) { return std::vector<be::TrafficAircraft>{plane(7, 0, -6000 + 70 * t, 300, 90, 70)}; };
    r.nextPoll = 0;
    r.send("ReleaseCamera");
    r.c.onBackendEvent(be::CameraStatus{SdkAvailability::NotAcquired, false}, r.t);
    r.advance(.5);
    CHECK(r.c.ownership() == Ownership::Observing);
    CHECK_EQ(r.c.spotTarget(7, tr::SpotView::Chase, r.t), std::string());
    r.acquire();
    r.c.stopSpotting("Spotting stopped.", r.t);
    CHECK(r.c.ownership() == Ownership::Releasing);
    r.c.onBackendEvent(be::CameraStatus{SdkAvailability::NotAcquired, false}, r.t);
    r.advance(.1);
    // Any other recall ends spotting (manual), and so do Release and Panic (released).
    CHECK_EQ(r.c.spotTarget(7, tr::SpotView::Tower, r.t), std::string());
    r.acquire();
    r.events.clear();
    r.send("ActivateCamera", {{"cameraId", r.pilot}});
    CHECK(!r.c.spotting());
    json stopped;
    for (const auto& [n, p] : r.events) if (n == "SpotStopped") stopped = p;
    CHECK(stopped["manual"].get<bool>());
    CHECK_EQ(r.c.activeCameraId(), r.pilot);
    CHECK_EQ(r.c.spotTarget(7, tr::SpotView::Chase, r.t), std::string());
    r.events.clear();
    r.c.release("Camera returned to the simulator", r.t);
    CHECK(!r.c.spotting());
    for (const auto& [n, p] : r.events) if (n == "SpotStopped") stopped = p;
    CHECK(stopped["released"].get<bool>());
    r.c.onBackendEvent(be::CameraStatus{SdkAvailability::NotAcquired, false}, r.t);
    r.advance(.1);
    CHECK_EQ(r.c.spotTarget(7, tr::SpotView::Chase, r.t), std::string());
    r.acquire();
    r.events.clear();
    r.c.panicRelease(r.t);
    CHECK(!r.c.spotting());
    json panic;
    for (const auto& [n, p] : r.events) if (n == "PanicReleased") panic = p;
    CHECK(panic["spottingStopped"].get<bool>());
    // A lost claim (another add-on takes the camera) ends it too.
    r.c.onBackendEvent(be::CameraStatus{SdkAvailability::NotAcquired, false}, r.t);
    r.advance(.1);
    CHECK_EQ(r.c.spotTarget(7, tr::SpotView::Chase, r.t), std::string());
    r.acquire();
    r.c.onBackendEvent(be::CameraStatus{SdkAvailability::AcquiredByOther, false}, r.t);
    r.advance(.1);
    CHECK(!r.c.spotting());
}

// ---------------------------------------------------------------------------------------------------------------
// Module: feed, commands, bindings, auto-spot

namespace {
struct SpotModule {
    FakeBackend camera;
    FakeServices services;
    FakeStorage storage;
    ModuleApp app{camera, services, storage, "boot-spotting", "0.1.0-test"};
    double t = 0, step = .05;
    int req = 0;
    std::size_t seen = 0, polls = 0;
    std::function<std::vector<be::TrafficAircraft>(double)> truth;
    std::vector<json> log;  // every event since the last drain
    bool leased = false, visible = true;  // the main panel renews its lease every 2 s while open
    double renewAt = 0;
    explicit SpotModule(bool airport = true, const std::string& aircraft = "SimObjects\\Airplanes\\asobo_c172sp_g1000\\aircraft.cfg") {
        app.start(t);
        frame();
        app.onEvent(be::Connected{"1.8.16.0"}, t);
        app.onEvent(be::AircraftLoaded{aircraft}, t);
        app.onEvent(be::CameraStatus{SdkAvailability::NotAcquired, false}, t);
        frame(2);
        panel("Hello", {{"surface", "main"}});
        panel("QualificationStart");
        leased = true;
        if (airport) {
            frame(60);
            app.onEvent(be::AirportList{json::array({{{"icao", "KAAA"}, {"region", "K1"}, {"lat", 40}, {"lon", -90}, {"altitude", 100}}}), true}, t);
            frame(60);
            bool answered = false;
            for (auto it = services.airportRequests.rbegin(); it != services.airportRequests.rend() && !answered; ++it)
                if (it->first == "KAAA") { app.onEvent(be::AirportFacility{it->second, airportFixture(), ""}, t); answered = true; }
            CHECK(answered);
            frame(60);
            CHECK_EQ(js::str(snapshot()["airports"], "loaded"), std::string("KAAA"));
        }
    }
    std::vector<json> drain() {
        fscs::protocol::Reassembler r;
        std::vector<json> out;
        for (; seen < services.sent.size(); ++seen)
            if (auto whole = r.feed(services.sent[seen], t)) out.push_back(js::parse(*whole));
        for (const auto& m : out) log.push_back(m);
        return out;
    }
    json lastEvent(const std::string& name) {
        drain();
        json found;
        for (auto& m : log)
            if (js::str(m, "type") == "event" && js::str(m, "name") == name) found = m;
        return found;
    }
    std::size_t eventCount(const std::string& name) {
        drain();
        return static_cast<std::size_t>(std::count_if(log.begin(), log.end(), [&](const json& m) {
            return js::str(m, "type") == "event" && js::str(m, "name") == name;
        }));
    }
    json panel(const std::string& action, json payload = json::object()) {
        const std::string id = "main-" + std::to_string(++req);
        json msg{{"protocolVersion", 2}, {"requestId", id}, {"uiInstanceId", "main-1"}, {"moduleBootId", app.bootId()},
                 {"sessionEpoch", app.coordinator().epoch()}, {"draftRevision", app.draftRevision()}, {"action", action}, {"payload", payload}};
        app.onEvent(be::CommBusMessage{msg.dump()}, t);
        json ack;
        for (auto& m : drain())
            if (js::str(m, "type") == "ack" && js::str(m, "requestId") == id) ack = m;
        return ack;
    }
    json snapshot() {
        const std::string id = "main-" + std::to_string(++req);
        json msg{{"protocolVersion", 2}, {"requestId", id}, {"uiInstanceId", "main-1"}, {"moduleBootId", app.bootId()},
                 {"sessionEpoch", app.coordinator().epoch()}, {"action", "RequestSnapshot"}, {"payload", json::object()}};
        app.onEvent(be::CommBusMessage{msg.dump()}, t);
        return lastEvent("Snapshot")["payload"];
    }
    // One frame: the user's own position and motion, the answer to any new traffic poll, then the module update.
    void frame(int n = 1) {
        for (int i = 0; i < n; ++i) {
            t += step;
            const auto own = ap::world(kAirport, {-300, 0, 0});
            app.onEvent(be::AircraftPosition{own.lat, own.lon, own.altitude, 90}, t);
            core::FlightMotion m;
            m.onGround = 1;
            app.onEvent(be::MotionTelemetry{m, 0, 0}, t);
            if (truth && services.trafficRequests.size() > polls) {
                polls = services.trafficRequests.size();
                auto list = truth(t);
                list.push_back(plane(kUser, -300, 0, 0, 90, 0, 0, true));  // the simulator lists the user too
                app.onEvent(be::Traffic{list, kUser}, t);
            }
            if (leased && t >= renewAt) {
                renewAt = t + 2;
                panel("RenewLease", {{"visible", visible}});
            }
            app.update(t);
            if (app.coordinator().ownership() == Ownership::Acquiring) app.onEvent(be::CameraStatus{SdkAvailability::Acquired, false}, t);
        }
    }
    void seconds(double s) { frame(static_cast<int>(std::lround(s / step))); }
    std::size_t key(const std::string& definition) const {
        return static_cast<std::size_t>(std::find(services.mapped.begin(), services.mapped.end(), definition) - services.mapped.begin());
    }
    void tap(const std::string& definition) {
        app.onEvent(be::InputEvent{key(definition), true}, t);
        app.onEvent(be::InputEvent{key(definition), false}, t);
    }
    json setting(const char* name, json value) {
        auto settings = snapshot()["working"]["settings"];
        settings["host"][name] = std::move(value);
        return panel("UpdateDraft", {{"ops", json::array({{{"op", "setSettings"}, {"settings", settings}}})}});
    }
    std::string towerId() const {
        for (const auto& cam : app.coordinator().profile()->cameras) if (cam.airportHeightCategory == "tower") return cam.id;
        return {};
    }
};

std::string status(const json& ack) { return js::str(ack, "status"); }
}  // namespace

TEST_CASE("module: traffic feed polls every 1.5 s within the spotting radius; TrafficChanged at most 1 Hz; Snapshot fields") {
    SpotModule m(false);
    m.truth = [](double t) {
        return std::vector<be::TrafficAircraft>{plane(7, 0, -6000 + 70 * t, 300, 90, 70, -4), plane(8, 0, 500, 0, 270, 0, 0, true)};
    };
    const auto before = m.services.trafficRequests.size();
    m.seconds(6);
    const auto polls = m.services.trafficRequests.size() - before;
    CHECK(polls >= 4 && polls <= 5);
    CHECK_NEAR(m.services.trafficRequests.back(), 20 * 1852., 1e-6);  // default 20 NM, in meters
    // TrafficChanged: coalesced, never more than once a second even with polls every frame.
    m.drain();
    m.log.clear();
    for (int i = 0; i < 100; ++i) {
        m.app.onEvent(be::Traffic{m.truth(m.t), kUser}, m.t);
        m.frame();
    }
    const auto changes = m.eventCount("TrafficChanged");
    CHECK(changes >= 4 && changes <= 6);  // 5 s
    const auto event = m.lastEvent("TrafficChanged")["payload"];
    CHECK_EQ(event["aircraft"].size(), std::size_t{2});
    CHECK_EQ(event["aircraft"][0]["objectId"].get<std::uint32_t>(), 8u);  // nearest first, the user excluded
    CHECK_EQ(event["aircraft"][0]["status"].get<std::string>(), std::string("parked"));
    CHECK(event["at"].is_number_integer());
    const auto snap = m.snapshot();
    CHECK_EQ(snap["traffic"]["aircraft"].size(), std::size_t{2});
    CHECK_EQ(snap["spotting"]["state"].get<std::string>(), std::string("off"));
    CHECK(!snap["spotting"]["autoSpot"].get<bool>());
    CHECK(snap["spotting"]["targetId"].is_null());
    for (const char* k : {"targetId", "view", "autoSpot", "state", "targetLabel", "reason"}) CHECK(snap["spotting"].contains(k));
    // The radius setting is clamped 1-100 NM; the hold 10-300 s; both have defaults and no schema bump.
    CHECK_EQ(status(m.setting("spottingRadiusNm", 150)), std::string("Completed"));
    m.seconds(2);
    CHECK_NEAR(m.services.trafficRequests.back(), 100 * 1852., 1e-6);
    const auto defaults = fscs::profiles::parseHostSettings(fscs::profiles::defaultSettingsDocument());
    CHECK_EQ(defaults.spottingRadiusNm, 20.0);
    CHECK_EQ(defaults.autoSpotHoldSeconds, 30.0);
    json s = fscs::profiles::defaultSettingsDocument();
    CHECK(s["host"].contains("spottingRadiusNm"));
    CHECK(s["host"].contains("autoSpotHoldSeconds"));
    s["host"]["spottingRadiusNm"] = .2;
    s["host"]["autoSpotHoldSeconds"] = 1000;
    const auto clamped = fscs::profiles::parseHostSettings(s);
    CHECK_EQ(clamped.spottingRadiusNm, 1.0);
    CHECK_EQ(clamped.autoSpotHoldSeconds, 300.0);
    const auto normalized = fscs::profiles::normalizeSettings(s)["host"];
    CHECK_EQ(normalized["autoSpotHoldSeconds"].get<double>(), 300.0);
    CHECK_EQ(fscs::profiles::normalizeSettings(s)["schemaVersion"].get<int>(), fscs::profiles::kSettingsSchemaVersion);
    // No panel visible and nothing spotted: the feed stops.
    m.visible = false;
    m.panel("RenewLease", {{"visible", false}});
    const auto hidden = m.services.trafficRequests.size();
    m.seconds(5);
    CHECK_EQ(m.services.trafficRequests.size(), hidden);
}

TEST_CASE("module: SpotTarget, StopSpotting and SetAutoSpot commands, with result.spotting and SpottingChanged") {
    SpotModule m;
    m.truth = [](double t) { return std::vector<be::TrafficAircraft>{plane(7, 0, -6000 + 70 * t, 300, 90, 70, -4)}; };
    m.seconds(2);
    const auto pilot = m.app.coordinator().activeCameraId();
    CHECK_EQ(status(m.panel("SpotTarget", {{"objectId", 7}, {"view", "sideways"}})), std::string("Rejected"));
    CHECK_EQ(status(m.panel("SpotTarget", {{"objectId", 99}, {"view", "chase"}})), std::string("Rejected"));
    const auto ack = m.panel("SpotTarget", {{"objectId", 7}, {"view", "chase"}});
    CHECK_EQ(status(ack), std::string("Completed"));
    const auto& result = ack["result"]["spotting"];
    CHECK_EQ(result["state"].get<std::string>(), std::string("tracking"));
    CHECK_EQ(result["view"].get<std::string>(), std::string("chase"));
    CHECK_EQ(result["targetId"].get<std::uint32_t>(), 7u);
    CHECK_EQ(result["targetLabel"].get<std::string>(), std::string("DLH7AB - A320"));
    m.seconds(1);
    CHECK(m.app.coordinator().ownership() == Ownership::Active);
    auto snap = m.snapshot();
    CHECK(snap["activeCamera"]["cameraId"].is_null());  // chase shows no profile camera
    CHECK_EQ(snap["spotting"]["state"].get<std::string>(), std::string("tracking"));
    CHECK_EQ(m.lastEvent("SpottingChanged")["payload"]["view"].get<std::string>(), std::string("chase"));
    // Tower: the loaded airport's tower view, reported as the active camera.
    CHECK_EQ(status(m.panel("SpotTarget", {{"objectId", 7}, {"view", "tower"}})), std::string("Completed"));
    m.seconds(.5);
    CHECK_EQ(m.snapshot()["activeCamera"]["cameraId"].get<std::string>(), m.towerId());
    // StopSpotting goes back (here: nothing was shown before, so the simulator gets the camera back).
    const auto stop = m.panel("StopSpotting");
    CHECK_EQ(status(stop), std::string("Completed"));
    CHECK_EQ(stop["result"]["spotting"]["state"].get<std::string>(), std::string("off"));
    CHECK(m.app.coordinator().ownership() == Ownership::Releasing);
    (void)pilot;
    m.app.onEvent(be::CameraStatus{SdkAvailability::NotAcquired, false}, m.t);
    m.seconds(.5);
    // Auto-spot: Completed and waiting with nothing to show; StopSpotting and a manual SpotTarget turn it off.
    CHECK_EQ(status(m.panel("SetAutoSpot", {{"enabled", true}})), std::string("Completed"));
    m.seconds(1);
    snap = m.snapshot();
    CHECK(snap["spotting"]["autoSpot"].get<bool>());
    // Plane 7 is descending 3 NM out, so it is arriving: auto-spot shows it at once.
    CHECK_EQ(snap["spotting"]["state"].get<std::string>(), std::string("tracking"));
    CHECK_EQ(snap["spotting"]["targetId"].get<std::uint32_t>(), 7u);
    CHECK_EQ(status(m.panel("SpotTarget", {{"objectId", 7}, {"view", "flyby"}})), std::string("Completed"));
    CHECK(!m.app.autoSpotEnabled());
    CHECK_EQ(status(m.panel("SetAutoSpot", {{"enabled", true}})), std::string("Completed"));
    CHECK_EQ(status(m.panel("StopSpotting")), std::string("Completed"));
    CHECK(!m.app.autoSpotEnabled());
    CHECK(!m.app.coordinator().spotting());
}

TEST_CASE("module: Release and Panic stop spotting and auto-spot; PanicReleased.spottingStopped") {
    SpotModule m;
    m.truth = [](double t) { return std::vector<be::TrafficAircraft>{plane(7, 0, -6000 + 70 * t, 300, 90, 70)}; };
    m.seconds(2);
    CHECK_EQ(status(m.panel("SpotTarget", {{"objectId", 7}, {"view", "chase"}})), std::string("Completed"));
    CHECK_EQ(status(m.panel("SetAutoSpot", {{"enabled", true}})), std::string("Completed"));
    m.seconds(1);
    m.panel("ReleaseCamera");
    m.seconds(.1);
    CHECK(!m.app.coordinator().spotting());
    CHECK(!m.app.autoSpotEnabled());
    CHECK_EQ(m.snapshot()["spotting"]["state"].get<std::string>(), std::string("off"));
    m.app.onEvent(be::CameraStatus{SdkAvailability::NotAcquired, false}, m.t);
    m.seconds(.5);
    // Panic, spotting: stopped and said so.
    CHECK_EQ(status(m.panel("SpotTarget", {{"objectId", 7}, {"view", "chase"}})), std::string("Completed"));
    m.seconds(1);
    m.drain();
    m.log.clear();
    m.tap("VK_LCONTROL+VK_LMENU+X");
    CHECK(m.lastEvent("PanicReleased")["payload"]["spottingStopped"].get<bool>());
    CHECK(!m.app.coordinator().spotting());
    m.app.onEvent(be::CameraStatus{SdkAvailability::NotAcquired, false}, m.t);
    m.seconds(.5);
    // Panic while auto-spot only waits: stopped too.
    CHECK_EQ(status(m.panel("SetAutoSpot", {{"enabled", true}})), std::string("Completed"));
    m.seconds(1);
    CHECK(!m.app.coordinator().spotting());
    m.log.clear();
    m.tap("VK_LCONTROL+VK_LMENU+X");
    CHECK(m.lastEvent("PanicReleased")["payload"]["spottingStopped"].get<bool>());
    CHECK(!m.app.autoSpotEnabled());
    m.log.clear();
    m.tap("VK_LCONTROL+VK_LMENU+X");
    CHECK(!m.lastEvent("PanicReleased")["payload"]["spottingStopped"].get<bool>());
}

TEST_CASE("module: spotting, Cinematic mode and airport Auto switch are mutually exclusive") {
    SpotModule m;
    m.truth = [](double t) { return std::vector<be::TrafficAircraft>{plane(7, 0, -6000 + 70 * t, 300, 90, 70)}; };
    m.seconds(2);
    CHECK_EQ(status(m.panel("SetCinematic", {{"enabled", true}})), std::string("Completed"));
    m.seconds(1);
    CHECK(m.app.cinematicEnabled());
    CHECK_EQ(status(m.panel("SpotTarget", {{"objectId", 7}, {"view", "chase"}})), std::string("Completed"));
    m.seconds(.5);
    CHECK(!m.app.cinematicEnabled());
    CHECK_EQ(m.snapshot()["cinematic"]["reason"].get<std::string>(), std::string("Spotting started."));
    m.seconds(30);  // nothing switches the chase away
    CHECK(m.app.coordinator().spotting());
    CHECK_EQ(status(m.panel("SetAutoSpot", {{"enabled", true}})), std::string("Completed"));
    CHECK_EQ(status(m.panel("SetCinematic", {{"enabled", true}})), std::string("Completed"));
    m.seconds(.5);
    CHECK(m.app.cinematicEnabled());
    CHECK(!m.app.coordinator().spotting());
    CHECK(!m.app.autoSpotEnabled());
    CHECK_EQ(m.snapshot()["spotting"]["reason"].get<std::string>(), std::string("Cinematic mode was turned on."));
    // Auto-spot on stops Cinematic mode; airport Auto switch on stops spotting.
    CHECK_EQ(status(m.panel("SetAutoSpot", {{"enabled", true}})), std::string("Completed"));
    CHECK(!m.app.cinematicEnabled());
    CHECK_EQ(status(m.panel("SpotTarget", {{"objectId", 7}, {"view", "chase"}})), std::string("Completed"));
    CHECK_EQ(status(m.panel("SetAirportAuto", {{"enabled", true}})), std::string("Completed"));
    m.seconds(.5);
    CHECK(!m.app.coordinator().spotting());
    CHECK_EQ(m.snapshot()["spotting"]["reason"].get<std::string>(), std::string("Airport Auto switch was turned on."));
    CHECK(m.snapshot()["airports"]["autoSwitch"].get<bool>());
    CHECK_EQ(status(m.panel("SpotTarget", {{"objectId", 7}, {"view", "chase"}})), std::string("Completed"));
    CHECK(!m.snapshot()["airports"]["autoSwitch"].get<bool>());
}

TEST_CASE("module: auto-spot picks landings, arrivals and departures at the airport, holds them and picks views by status") {
    SpotModule m;
    CHECK_EQ(status(m.setting("autoSpotHoldSeconds", 10)), std::string("Completed"));
    // 7 arrives on final for runway 09; 8 departs; 9 cruises overhead; 10 is parked.
    m.truth = [](double t) {
        return std::vector<be::TrafficAircraft>{plane(7, 0, -9000 + 70 * t, 470 - 3.7 * t, 90, 70, -3.7),
                                                plane(8, 0, 3000 + 80 * t, 300 + 8 * t, 90, 80, 8),
                                                plane(9, 0, 0, 10000, 0, 220), plane(10, 300, 300, 0, 0, 0, 0, true)};
    };
    m.seconds(2);
    CHECK_EQ(status(m.panel("SetAutoSpot", {{"enabled", true}})), std::string("Completed"));
    m.seconds(2);
    // The arrival first (an arrival outranks a departure), in the runway view.
    CHECK(m.app.coordinator().spotting());
    CHECK_EQ(m.app.coordinator().spotTargetId(), 7u);
    CHECK(m.app.coordinator().spotView() == tr::SpotView::Runway);
    auto state = m.snapshot()["spotting"];
    CHECK(state["autoSpot"].get<bool>());
    CHECK_EQ(state["state"].get<std::string>(), std::string("tracking"));
    CHECK_EQ(state["view"].get<std::string>(), std::string("runway"));
    // Held for the hold time, then the next one: the departure, in the chase view.
    m.seconds(7);
    CHECK_EQ(m.app.coordinator().spotTargetId(), 7u);
    m.seconds(4);
    CHECK_EQ(m.app.coordinator().spotTargetId(), 8u);
    CHECK(m.app.coordinator().spotView() == tr::SpotView::Chase);
    // When nothing qualifies any more: waiting, back to the earlier view (here the simulator's).
    m.truth = [](double t) {
        return std::vector<be::TrafficAircraft>{plane(9, 0, 0, 10000, 0, 220), plane(10, 300, 300 + 0 * t, 0, 0, 0, 0, true)};
    };
    m.seconds(20);
    CHECK(!m.app.coordinator().spotting());
    state = m.snapshot()["spotting"];
    CHECK_EQ(state["state"].get<std::string>(), std::string("waiting"));
    CHECK(state["autoSpot"].get<bool>());
    // A landing roll appears: shown at once, in the runway view; once it slows to taxi the view becomes the tower.
    m.truth = [](double t) {
        const double rolled = std::min(t - 46.0, 20.0);
        const double east = -900 + 40 * std::min(rolled, 15.0) + 8 * std::max(0.0, rolled - 15);
        return std::vector<be::TrafficAircraft>{plane(11, 0, east, 0, 90, rolled < 15 ? 40 : 8, 0, true)};
    };
    // It was seen airborne just before touchdown.
    m.app.onEvent(be::Traffic{{plane(11, 0, -1100, 15, 90, 60, -3)}, kUser}, m.t);
    m.seconds(2);
    CHECK(m.app.coordinator().spotting());
    CHECK_EQ(m.app.coordinator().spotTargetId(), 11u);
    CHECK(m.app.coordinator().spotView() == tr::SpotView::Runway);
    m.seconds(18);
    CHECK_EQ(m.app.coordinator().spotTargetId(), 11u);
    CHECK(m.app.coordinator().spotView() == tr::SpotView::Tower);
}

TEST_CASE("module: spotting bindings - appended, Ctrl+Alt T/Y/O/J, collision-free; cycle by distance keeping the view") {
    using fscs::profiles::Action;
    CHECK(fscs::profiles::parseAction("spotNext") == Action::SpotNext);
    CHECK(fscs::profiles::parseAction("spotPrevious") == Action::SpotPrevious);
    CHECK(fscs::profiles::parseAction("spotReturn") == Action::SpotReturn);
    CHECK(fscs::profiles::parseAction("toggleAutoSpot") == Action::ToggleAutoSpot);
    CHECK(static_cast<int>(Action::SpotNext) == static_cast<int>(Action::ToggleCinematic) + 1);  // appended
    const auto bindings = fscs::profiles::defaultBindingsDocument();
    CHECK(fscs::profiles::parseBindings(bindings).problems.empty());
    std::set<std::string> definitions;
    std::map<std::string, std::string> spotting;
    for (const auto& b : bindings["bindings"]) {
        const auto def = js::str(js::object(b, "input"), "definition");
        CHECK(definitions.insert(def).second);  // no two defaults share an input
        const auto action = js::str(b, "action");
        if (action.rfind("spot", 0) == 0 || action == "toggleAutoSpot") spotting[action] = def;
    }
    CHECK_EQ(spotting.size(), std::size_t{4});
    CHECK_EQ(spotting["spotNext"], std::string("VK_LCONTROL+VK_LMENU+T"));
    CHECK_EQ(spotting["spotPrevious"], std::string("VK_LCONTROL+VK_LMENU+Y"));
    CHECK_EQ(spotting["spotReturn"], std::string("VK_LCONTROL+VK_LMENU+O"));
    CHECK_EQ(spotting["toggleAutoSpot"], std::string("VK_LCONTROL+VK_LMENU+J"));
    // Listed straight after toggleCinematic (the movement defaults are appended after them by the upgrade).
    std::vector<std::string> order;
    for (const auto& b : bindings["bindings"]) order.push_back(js::str(b, "action"));
    const auto cinematic = std::find(order.begin(), order.end(), "toggleCinematic");
    CHECK(cinematic != order.end() && cinematic + 4 < order.end());
    if (cinematic != order.end() && cinematic + 4 < order.end()) {
        CHECK_EQ(cinematic[1], std::string("spotNext"));
        CHECK_EQ(cinematic[4], std::string("toggleAutoSpot"));
    }

    SpotModule m;
    m.truth = [](double t) {
        return std::vector<be::TrafficAircraft>{plane(7, 0, 3000 + 0 * t, 300, 90, 0), plane(8, 0, 1000, 0, 90, 0, 0, true),
                                                plane(9, 0, 2000, 0, 90, 0, 0, true)};
    };
    m.seconds(2);
    CHECK(m.key("VK_LCONTROL+VK_LMENU+T") < m.services.mapped.size());
    m.tap("VK_LCONTROL+VK_LMENU+T");
    m.seconds(.5);
    CHECK_EQ(m.app.coordinator().spotTargetId(), 8u);  // nearest first, chase to start with
    CHECK(m.app.coordinator().spotView() == tr::SpotView::Chase);
    m.tap("VK_LCONTROL+VK_LMENU+T");
    m.seconds(.2);
    CHECK_EQ(m.app.coordinator().spotTargetId(), 9u);
    // The view type is kept: tower stays tower for the next target.
    CHECK_EQ(status(m.panel("SpotTarget", {{"objectId", 9}, {"view", "tower"}})), std::string("Completed"));
    m.tap("VK_LCONTROL+VK_LMENU+T");
    m.seconds(.2);
    CHECK_EQ(m.app.coordinator().spotTargetId(), 7u);
    CHECK(m.app.coordinator().spotView() == tr::SpotView::Tower);
    m.tap("VK_LCONTROL+VK_LMENU+T");  // wraps
    m.seconds(.2);
    CHECK_EQ(m.app.coordinator().spotTargetId(), 8u);
    m.tap("VK_LCONTROL+VK_LMENU+Y");
    m.seconds(.2);
    CHECK_EQ(m.app.coordinator().spotTargetId(), 7u);
    // Auto-spot toggles; Return stops spotting and auto-spot.
    m.tap("VK_LCONTROL+VK_LMENU+J");
    m.seconds(.2);
    CHECK(m.app.autoSpotEnabled());
    m.tap("VK_LCONTROL+VK_LMENU+O");
    m.seconds(.2);
    CHECK(!m.app.coordinator().spotting());
    CHECK(!m.app.autoSpotEnabled());
    // Nothing was shown before spotting, so Return gave the camera back to the simulator.
    CHECK(m.app.coordinator().ownership() == Ownership::Releasing);
    m.app.onEvent(be::CameraStatus{SdkAvailability::NotAcquired, false}, m.t);
    m.seconds(.2);
    // A manual recall stops spotting.
    m.tap("VK_LCONTROL+VK_LMENU+T");
    m.seconds(.5);
    CHECK(m.app.coordinator().spotting());
    m.tap("VK_LCONTROL+VK_LMENU+1");
    m.seconds(.2);
    CHECK(!m.app.coordinator().spotting());
    CHECK_EQ(m.snapshot()["spotting"]["reason"].get<std::string>(), std::string("A camera was selected manually."));
}

// ---------------------------------------------------------------------------------------------------------------
// Smoothness: per-frame target data, injected traffic without speeds, parked targets

namespace {
// The per-frame sample of `a`, with world velocity (north, east, down) as SimConnect VELOCITY WORLD would give it.
be::TrafficTarget frameOf(const be::TrafficAircraft& a, core::Vec3 velocity) {
    be::TrafficTarget f;
    f.objectId = a.objectId;
    f.lat = a.lat; f.lon = a.lon; f.altitude = a.altitude;
    f.headingDeg = a.headingDeg; f.pitchDeg = a.pitchDeg; f.bankDeg = a.bankDeg;
    f.velocityNorth = velocity.x; f.velocityEast = velocity.y; f.velocityDown = velocity.z;
    f.onGround = a.onGround;
    f.groundAltitude = a.groundAltitude;
    return f;
}
double gap(const ap::Geo& a, const ap::Geo& b) { return ap::local(a, b).length(); }
}  // namespace

TEST_CASE("spotting: ATC text - $$: and TT: prefixes are stripped; labels are plain ASCII") {
    CHECK_EQ(tr::cleanAtcText("$$:E190"), std::string("E190"));
    CHECK_EQ(tr::cleanAtcText("$$E190"), std::string("E190"));
    CHECK_EQ(tr::cleanAtcText("TT:$$:A20N"), std::string("A20N"));
    CHECK_EQ(tr::cleanAtcText("$$:TT:ATCCOM.AC_MODEL_B738.0.text"), std::string("B738"));
    CHECK_EQ(tr::cleanAtcText("tt:ATCCOM.AC_MODEL_A320.0.text"), std::string("A320"));
    be::TrafficAircraft s;
    s.airline = "CVY"; s.flightNumber = "44N"; s.model = "$$:E190";
    tr::Model m;
    auto p = plane(5, 0, 0, 300, 90, 70);
    p.airline = s.airline; p.flightNumber = s.flightNumber; p.model = s.model;
    m.update({p}, kUser, kAirport, 0);
    const auto label = tr::label(m.find(5)->last);
    CHECK_EQ(label, std::string("CVY44N - E190"));
    CHECK(std::all_of(label.begin(), label.end(), [](char c) { return static_cast<unsigned char>(c) < 128; }));
}

TEST_CASE("spotting: injected traffic with zero reported speeds - speeds fitted to positions, smooth, not parked") {
    // FSLTL-style: GROUND VELOCITY and VERTICAL SPEED read 0 while the aircraft flies east at 70 m/s, descending 4 m/s.
    tr::Model m;
    m.setAirport(kAirport);
    const auto truth = [](double t) {
        auto a = plane(9, 0, -8000 + 70 * t, 600 - 4 * t, 90, 0, 0);
        return a;
    };
    const double dt = 1.0 / 60;
    double nextPoll = 0, maxAccel = 0, maxError = 0;
    std::vector<core::Vec3> path;
    for (int frame = 0; frame < 60 * 20; ++frame) {
        const double t = frame * dt;
        if (t >= nextPoll - 1e-9) { m.update({truth(t)}, kUser, kAirport, t); nextPoll += 1.5; }
        const auto p = m.predict(9, t);
        CHECK(p.valid());
        path.push_back(at(p.position));
        const auto real = truth(t);
        if (t > 6) maxError = std::max(maxError, (path.back() - at({real.lat, real.lon, real.altitude})).length());
    }
    for (std::size_t i = 60 * 6; i < path.size(); ++i)
        maxAccel = std::max(maxAccel, ((path[i] - path[i - 1] * 2 + path[i - 2]) * (1 / (dt * dt))).length());
    CHECK(maxAccel < 3);     // no surge and stop between polls
    CHECK(maxError < 1);
    const auto* e = m.find(9);
    CHECK(e->status == tr::Status::Arriving);  // not parked: within 15 NM, descending
    const auto doc = m.toJson(kAirport, 20);
    const auto& listed = doc["aircraft"][0];
    CHECK_NEAR(listed["groundSpeedKt"].get<double>(), 70 * tr::kKnotsPerMps, 2);
    CHECK_NEAR(listed["verticalSpeedFpm"].get<double>(), -4 * tr::kFpmPerMps, 20);
    // On the ground at 20 m/s with a zero report: a takeoff roll, not parked.
    tr::Model g;
    g.setAirport(kAirport);
    for (double t = 0; t <= 4.5; t += 1.5) g.update({plane(10, 0, -900 + 25 * t, 0, 90, 0, 0, true)}, kUser, kAirport, t);
    CHECK(g.find(10)->status == tr::Status::Takeoff);  // 25 m/s = 48.6 kt: a roll
    CHECK_NEAR(g.find(10)->last.groundSpeed, 25, .5);
    for (double t = 6; t <= 12; t += 1.5) g.update({plane(10, 0, -900 + 25 * 4.5 + 10 * (t - 4.5), 0, 90, 0, 0, true)}, kUser, kAirport, t);
    CHECK(g.find(10)->status == tr::Status::Taxi);  // 10 m/s: taxiing
    CHECK_NEAR(g.find(10)->last.groundSpeed, 10, .5);
    // A plausible report is kept, and a fit across a jump is never a speed.
    tr::Model j;
    j.update({plane(11, 0, 0, 0, 90, 15, 0, true)}, kUser, kAirport, 0);
    j.update({plane(11, 0, 500, 0, 90, 15, 0, true)}, kUser, kAirport, 1.5);
    CHECK_NEAR(j.find(11)->last.groundSpeed, 15, 1e-9);
}

TEST_CASE("spotting: per-frame target data is shown exactly, the poll never disturbs it, and losing it blends back") {
    tr::Model m;
    const auto truth = [](double t) { return plane(7, 0, -6000 + 70 * t, 300 + 2 * t, 90, 70, 2); };
    // The poll lags and is off by 20 m: per-frame data wins.
    for (double t = 0; t <= 3; t += 1.5) m.update({plane(7, 20, -6000 + 70 * t, 300, 90, 70)}, kUser, kAirport, t);
    const double dt = 1.0 / 60;
    double t = 3;
    for (int i = 0; i < 120; ++i, t += dt) {
        m.updateTarget(frameOf(truth(t), {0, 70, -2}), t);
        if (i == 60) m.update({plane(7, 20, -6000 + 70 * t, 300, 90, 70)}, kUser, kAirport, t);  // a poll mid-way
        const auto p = m.predict(7, t);
        const auto real = truth(t);
        CHECK_NEAR(gap(p.position, {real.lat, real.lon, real.altitude}), 0, 1e-9);  // zero error at the sample time
        CHECK_NEAR(p.headingDeg, 90, 1e-9);
        CHECK(m.frameFresh(7, t));
    }
    t -= dt;
    const auto last = truth(t);
    // Within 0.1 s: extrapolated with the sample's velocity; beyond that it continues and blends into the poll prediction.
    CHECK_NEAR(at(m.predict(7, t + .05).position).y - at({last.lat, last.lon, last.altitude}).y, 3.5, 1e-6);
    CHECK_NEAR(at(m.predict(7, t + .3).position).y - at({last.lat, last.lon, last.altitude}).y, 21, .5);
    // Samples stop: after 0.5 s the pose blends into the poll prediction with no step at 60 fps.
    double maxStep = 0;
    ap::Geo prev = m.predict(7, t).position;
    for (int i = 1; i < 180; ++i) {
        const auto now = m.predict(7, t + i * dt).position;
        maxStep = std::max(maxStep, gap(prev, now));
        prev = now;
    }
    CHECK(maxStep < 70 * dt * 2);  // never more than about two frames of travel
    CHECK(!m.frameFresh(7, t + 1));
    // Velocity is checked against the positions: a zero or wrongly mapped VELOCITY WORLD is replaced by a fit.
    tr::Model z;
    z.update({plane(8, 0, 0, 300, 90, 70)}, kUser, kAirport, 0);
    double u = 0;
    for (int i = 0; i < 40; ++i, u += dt) z.updateTarget(frameOf(plane(8, 0, 70 * u, 300, 90, 0), {0, 0, 0}), u);
    CHECK_NEAR(z.predict(8, u - dt).velocity.y, 70, .5);
    CHECK_NEAR(z.predict(8, u - dt).velocity.x, 0, .5);
    tr::Model w;
    w.update({plane(8, 0, 0, 300, 90, 70)}, kUser, kAirport, 0);
    u = 0;
    for (int i = 0; i < 40; ++i, u += dt) w.updateTarget(frameOf(plane(8, 0, 70 * u, 300, 90, 0), {0, -70, 0}), u);  // reversed
    CHECK_NEAR(w.predict(8, u - dt).velocity.y, 70, .5);
    CHECK(tr::chaseDirectionDeg(w.predict(8, u - dt)) > 80);
    // Unknown objects are ignored.
    w.updateTarget(frameOf(plane(99, 0, 0, 300, 90, 0), {}), u);
    CHECK(w.find(99) == nullptr);
}

TEST_CASE("spotting: a parked target is perfectly still despite noisy reports, polled or per frame") {
    std::uint64_t seed = 99;
    const auto noise = [&] {
        seed = seed * 6364136223846793005ull + 1442695040888963407ull;
        return static_cast<double>(seed >> 11) / 9007199254740992.0 * 2 - 1;
    };
    const auto noisy = [&](double meters, double degrees) {
        auto a = plane(5, 100, 200, 0, 45, .05 * std::abs(noise()), .02 * noise(), true);
        const auto g = ap::world({a.lat, a.lon, a.altitude}, {meters * noise(), meters * noise(), meters / 2 * noise()});
        a.lat = g.lat; a.lon = g.lon; a.altitude = g.altitude;
        a.headingDeg += degrees * noise();
        return a;
    };
    tr::Model m;
    m.update({noisy(.2, .3)}, kUser, kAirport, 0);
    const auto first = m.predict(5, 0);
    for (double t = 1.5; t < 30; t += 1.5) {
        m.update({noisy(.2, .3)}, kUser, kAirport, t);
        for (double f = 0; f < 1.5; f += .1) {
            const auto p = m.predict(5, t + f);
            CHECK_NEAR(gap(p.position, first.position), 0, 1e-9);
            CHECK_NEAR(p.headingDeg, first.headingDeg, 1e-9);
            CHECK_NEAR(p.velocity.length(), 0, 1e-12);
        }
    }
    CHECK(m.find(5)->status == tr::Status::Parked);
    // Per frame: 5 cm of jitter and a few mm/s of velocity noise never move it either.
    double t = 30;
    m.updateTarget(frameOf(noisy(.05, .1), {.001, -.002, 0}), t);
    const auto held = m.predict(5, t);
    for (int i = 0; i < 300; ++i) {
        t += 1.0 / 60;
        m.updateTarget(frameOf(noisy(.05, .1), {.003 * noise(), .003 * noise(), 0}), t);
        CHECK_NEAR(gap(m.predict(5, t).position, held.position), 0, 1e-9);
        CHECK_NEAR(m.predict(5, t).headingDeg, held.headingDeg, 1e-9);
    }
    // Moving off is followed at once.
    for (int i = 0; i < 30; ++i) {
        t += 1.0 / 60;
        const auto a = plane(5, 100, 200 + 3 * (i + 1) / 60.0 + .5, 0, 45, 3, 0, true);
        m.updateTarget(frameOf(a, {0, 3, 0}), t);
    }
    CHECK(gap(m.predict(5, t).position, held.position) > .5);
}

TEST_CASE("spotting: chase direction - heading when slow, track when fast; smoothDamp is continuous") {
    tr::Pose p;
    p.position = ap::world(kAirport, {0, 0, -100});
    p.headingDeg = 0;
    p.velocity = {0, 5, 0};  // sliding east at 5 m/s, nose north (a slow, noisy track)
    CHECK_NEAR(tr::chaseDirectionDeg(p), 0, 1e-9);
    CHECK_NEAR(tr::chaseOffset(p).x, -60, 1e-9);
    p.velocity = {0, 12, 0};
    CHECK_NEAR(tr::chaseDirectionDeg(p), 90, 1e-9);
    p.velocity = {0, 8, 0};
    const double mid = tr::chaseDirectionDeg(p);
    CHECK(mid > 10 && mid < 80);
    core::Vec3 rate, x{NAN, NAN, NAN};
    x = tr::smoothDamp(x, rate, {10, 0, 0}, .01);
    CHECK_NEAR(x.x, 10, 1e-12);
    double prevStep = 0, maxChange = 0, maxStep = 0;
    for (int i = 0; i < 300; ++i) {
        const auto before = x;
        x = tr::smoothDamp(x, rate, {20, 0, 0}, .01);
        const double step = x.x - before.x;
        maxStep = std::max(maxStep, step);
        if (i > 0) maxChange = std::max(maxChange, std::abs(step - prevStep));
        prevStep = step;
    }
    CHECK_NEAR(x.x, 20, 1e-3);
    CHECK(maxStep < .4);     // a 10 m change of target is eased in, never jumped to
    CHECK(maxChange < .07);  // the rate changes gradually: no kink
}

TEST_CASE("spotting: runway ends and spots from scenery data - geometry, end selection and SpotTarget.spot parsing") {
    const std::vector<tr::SpotAirport> airports{tr::spotAirport(airportFixture())};
    const auto& a = airports.front();
    CHECK_EQ(a.icao, std::string("KAAA"));
    CHECK_EQ(a.ends.size(), std::size_t{2});
    const auto* e09 = tr::findRunwayEnd(airports, "KAAA", "09");
    const auto* e27 = tr::findRunwayEnd(airports, "KAAA", "27");
    CHECK(e09 && e27);
    if (!e09 || !e27) return;
    CHECK_NEAR(e09->headingDeg, 90, 1e-9);
    CHECK_NEAR(at(e09->threshold).y, -1000, .01);
    CHECK_NEAR(at(e27->threshold).y, 1000, .01);
    // Fence line: 150 m behind the threshold, 80 m to the right of the landing direction, 3 m up.
    const auto fence = tr::runwaySpot(*e09, "fence", 3);
    CHECK_EQ(fence.kind, std::string("fence"));
    const auto f = at(tr::spotPosition(*e09, fence));
    CHECK_NEAR(f.y, -1150, .01);
    CHECK_NEAR(f.x, -80, .01);  // right of an eastbound landing is south
    CHECK_NEAR(-f.z, 3, .01);
    const auto f27 = at(tr::spotPosition(*e27, tr::runwaySpot(*e27, "fence", 3)));
    CHECK_NEAR(f27.y, 1150, .01);
    CHECK_NEAR(f27.x, 80, .01);
    const auto approach = at(tr::spotPosition(*e09, tr::runwaySpot(*e09, "approach", 3)));
    CHECK_NEAR(approach.y, -1000 - 1852, .01);
    CHECK_NEAR(approach.x, 0, .01);
    CHECK_NEAR(-approach.z, 1852 * std::tan(3 * core::kDegToRad) - 90, .01);
    CHECK_NEAR(at(tr::spotPosition(*e09, tr::runwaySpot(*e09, "threshold", 3))).x, -32, .01);
    // Tower: the scenery tower at airport elevation; the generator's virtual tower when scenery has none.
    CHECK_NEAR(at(a.tower).x, 111, 1);
    CHECK(!a.virtualTower);
    CHECK_NEAR(at(tr::towerPosition(a, 35)).z, -35, .01);
    auto noTower = airportFixture();
    noTower["tower"] = {{"lat", 0}, {"lon", 0}, {"altitude", 0}};
    const auto v = tr::spotAirport(noTower);
    CHECK(v.virtualTower);
    CHECK_NEAR(std::abs(at(v.tower).x), 300, .5);  // abeam the runway midpoint
    // End selection.
    const auto pose = [](double north, double east, double height, double heading, double speed) {
        tr::Pose p;
        p.position = ap::world(kAirport, {north, east, -height});
        p.headingDeg = heading;
        p.velocity = {speed * std::cos(heading * core::kDegToRad), speed * std::sin(heading * core::kDegToRad), 0};
        return p;
    };
    bool aligned = false;
    // Arriving from the west on final for 09 (5 km out, 60 m left of the centerline): 09.
    auto* pick = tr::pickRunwayEnd(airports, pose(60, -6000, 250, 92, 70), tr::Status::Arriving, &aligned);
    CHECK(pick == e09);
    CHECK(aligned);
    // Landing roll on 09 past the middle, nearer the 27 threshold: still 09.
    CHECK(tr::pickRunwayEnd(airports, pose(0, 600, 0, 90, 40), tr::Status::Landing) == e09);
    // Taking off on 09 from its threshold: the far end, 27 (it lifts off over 27's approach side).
    CHECK(tr::pickRunwayEnd(airports, pose(0, -900, 0, 90, 50), tr::Status::Takeoff, &aligned) == e27);
    CHECK(aligned);
    CHECK(tr::pickRunwayEnd(airports, pose(0, 3000, 300, 90, 80), tr::Status::Departing) == e27);
    // Parked or not lined up: the nearest end.
    CHECK(tr::pickRunwayEnd(airports, pose(200, 800, 0, 0, 0), tr::Status::Parked, &aligned) == e27);
    CHECK(!aligned);
    CHECK(tr::pickRunwayEnd(airports, pose(3000, -900, 300, 0, 70), tr::Status::Arriving) == e09);  // crossing: nearest
    CHECK(tr::pickRunwayEnd(airports, pose(0, 40000, 300, 270, 70), tr::Status::Arriving) == nullptr);  // too far away
    // SpotTarget.spot.
    std::string error;
    auto parsed = tr::parseRunwaySpot({{"icao", "KAAA"}, {"runwayEnd", "27"}, {"kind", "custom"}, {"behindMeters", 40}, {"sideMeters", -25}, {"heightMeters", 10}},
                                      airports, 3, error);
    CHECK(parsed.has_value());
    if (parsed) {
        CHECK_EQ(parsed->kind, std::string("custom"));
        CHECK_NEAR(parsed->sideMeters, -25, 1e-12);
        const auto c = at(tr::spotPosition(*e27, *parsed));
        CHECK_NEAR(c.y, 1040, .01);
        CHECK_NEAR(c.x, -25, .01);
        CHECK_NEAR(-c.z, 10, .01);
    }
    parsed = tr::parseRunwaySpot({{"icao", "KAAA"}, {"runwayEnd", "09"}}, airports, 3, error);
    CHECK(parsed.has_value() && parsed->kind == "fence" && parsed->behindMeters == 150);
    CHECK(!tr::parseRunwaySpot({{"icao", "KAAA"}, {"runwayEnd", "18"}}, airports, 3, error).has_value());
    CHECK(error.find("not known") != std::string::npos);
    CHECK(!tr::parseRunwaySpot({{"icao", "KAAA"}, {"runwayEnd", "09"}, {"kind", "roof"}}, airports, 3, error).has_value());
    CHECK(!tr::parseRunwaySpot({{"icao", "KAAA"}, {"runwayEnd", "09"}, {"heightMeters", -1}}, airports, 3, error).has_value());
    const auto j = tr::toJson(tr::runwaySpot(*e09, "fence", 3));
    for (const char* k : {"icao", "runwayEnd", "kind", "behindMeters", "sideMeters", "heightMeters"}) CHECK(j.contains(k));
}

TEST_CASE("spotting coordinator: chase on per-frame data - exact aim, rigid offset, fallback without a step, target terrain only") {
    SpotRig r;
    r.truth = [](double t) { return std::vector<be::TrafficAircraft>{plane(7, 0, -6000 + 70 * t, 300 + 3 * std::sin(t), 90 + 5 * std::sin(t / 3), 70)}; };
    // The aircraft drawn wanders off any straight-line prediction; the per-frame data is what is drawn.
    r.frames = [&r](double t) {
        const auto a = r.truth(t)[0];
        return std::optional<be::TrafficTarget>(frameOf(a, {0, 70, -3 * std::cos(t)}));
    };
    r.send("ActivateCamera", {{"cameraId", r.pilot}});
    r.acquire();
    r.advance(.5);
    CHECK_EQ(r.c.spotTarget(7, tr::SpotView::Chase, r.t), std::string());
    r.advance(4);
    double maxAimError = 0, maxTargetError = 0, maxOffsetJerk = 0;
    core::Vec3 prev2{NAN, NAN, NAN}, prev{NAN, NAN, NAN};
    for (int i = 0; i < 300; ++i) {
        r.advance(.01);
        const auto real = r.truth(r.t)[0];
        const ap::Geo drawn{real.lat, real.lon, real.altitude};
        maxTargetError = std::max(maxTargetError, gap(r.c.spotPose().position, drawn));
        const auto expected = tr::aim(r.camera(), drawn);
        maxAimError = std::max({maxAimError, std::abs(core::wrapDeg(r.looking().headingDeg - expected->headingDeg)),
                                std::abs(r.looking().pitchDeg - expected->pitchDeg)});
        const auto offset = ap::local(drawn, r.camera());
        if (prev2.finite()) maxOffsetJerk = std::max(maxOffsetJerk, (offset - prev * 2 + prev2).length());
        prev2 = prev;
        prev = offset;
    }
    CHECK(maxTargetError < 1e-6);  // the pose is the simulator's own
    CHECK(maxAimError < 1e-6);     // degrees: the target stays exactly centred
    CHECK(maxOffsetJerk < .005);   // the camera rides with the target; its offset changes smoothly
    // Terrain under the user aircraft never moves the chase (only the target's own ground altitude counts).
    const auto before = r.camera().altitude - r.truth(r.t)[0].altitude;
    r.c.onBackendEvent(be::GroundElevation{kAirport.altitude + 500}, r.t);
    r.advance(.5);
    CHECK_NEAR(r.camera().altitude - r.truth(r.t)[0].altitude, before, .05);
    // Per-frame data stops: the camera continues without a step.
    r.frames = nullptr;
    double maxStep = 0;
    ap::Geo last = r.camera();
    for (int i = 0; i < 300; ++i) {
        r.advance(.01);
        maxStep = std::max(maxStep, gap(last, r.camera()));
        last = r.camera();
    }
    CHECK(maxStep < 70 * .01 * 1.6);
}

TEST_CASE("spotting coordinator: a parked target gives a perfectly still chase and fly-by") {
    SpotRig r;
    std::uint64_t seed = 7;
    const auto noise = [&seed] {
        seed = seed * 6364136223846793005ull + 1442695040888963407ull;
        return static_cast<double>(seed >> 11) / 9007199254740992.0 * 2 - 1;
    };
    r.truth = [&noise](double) {
        auto a = plane(7, 200, 300, 0, 120, 0, 0, true);
        const auto g = ap::world({a.lat, a.lon, a.altitude}, {.2 * noise(), .2 * noise(), .1 * noise()});
        a.lat = g.lat; a.lon = g.lon; a.altitude = g.altitude;
        a.headingDeg += .3 * noise();
        a.groundAltitude += .2 * noise();
        return std::vector<be::TrafficAircraft>{a};
    };
    r.send("ActivateCamera", {{"cameraId", r.pilot}});
    r.acquire();
    r.advance(.5);
    for (const auto view : {tr::SpotView::Chase, tr::SpotView::Flyby}) {
        CHECK_EQ(r.c.spotTarget(7, view, r.t), std::string());
        r.advance(8);  // settled
        const auto settled = r.shown();
        const auto submissions = r.c.submissions();
        r.advance(10);  // several noisy polls
        CHECK_EQ(r.c.submissions(), submissions);  // nothing new to submit: perfectly still
        CHECK_NEAR(r.shown().x, settled.x, 0);
        CHECK_NEAR(r.shown().headingDeg, settled.headingDeg, 0);
        CHECK_NEAR(r.shown().fovRad, settled.fovRad, 0);
    }
    // Per-frame noise (a few cm) does not shake it either.
    r.frames = [&noise](double) {
        auto a = plane(7, 200, 300, 0, 120, 0, 0, true);
        const auto g = ap::world({a.lat, a.lon, a.altitude}, {.04 * noise(), .04 * noise(), .02 * noise()});
        a.lat = g.lat; a.lon = g.lon; a.altitude = g.altitude;
        a.headingDeg += .1 * noise();
        return std::optional<be::TrafficTarget>(frameOf(a, {.002 * noise(), .002 * noise(), 0}));
    };
    CHECK_EQ(r.c.spotTarget(7, tr::SpotView::Chase, r.t), std::string());
    r.advance(8);
    const auto submissions = r.c.submissions();
    r.advance(5);
    CHECK(r.c.submissions() <= submissions + 1);
}

TEST_CASE("spotting coordinator: slow targets are chased from behind the nose, not along a noisy track") {
    SpotRig r;
    // Taxiing north at 4 m/s while the reported track wobbles 40 degrees either side.
    r.truth = [](double t) { return std::vector<be::TrafficAircraft>{plane(7, -500 + 4 * t, 300, 0, 0, 4, 0, true)}; };
    r.frames = [&r](double t) {
        const double wobble = 40 * core::kDegToRad * std::sin(t * 3);
        return std::optional<be::TrafficTarget>(frameOf(r.truth(t)[0], {4 * std::cos(wobble), 4 * std::sin(wobble), 0}));
    };
    r.send("ActivateCamera", {{"cameraId", r.pilot}});
    r.acquire();
    r.advance(.2);
    CHECK_EQ(r.c.spotTarget(7, tr::SpotView::Chase, r.t), std::string());
    double maxSide = 0;
    r.advance(3);
    for (int i = 0; i < 300; ++i) {
        r.advance(.01);
        const auto offset = ap::local(r.c.spotPose().position, r.camera());
        CHECK_NEAR(offset.x, -60, .5);
        maxSide = std::max(maxSide, std::abs(offset.y));
    }
    CHECK(maxSide < .5);
}

TEST_CASE("spotting coordinator: without a camera profile - chase takes control with nothing underneath, StopSpotting releases") {
    SpotRig r(false, false);
    r.truth = [](double t) { return std::vector<be::TrafficAircraft>{plane(7, 0, -6000 + 70 * t, 300, 90, 70)}; };
    r.advance(.1);
    CHECK(!r.c.canActivate());
    CHECK(r.c.spottingAvailable());
    const auto caps = r.c.capabilitiesJson();
    CHECK(!caps["canActivate"].get<bool>());
    CHECK(caps["spottingAvailable"].get<bool>());
    CHECK(caps["spottingBlockReason"].is_null());
    CHECK_EQ(js::str(caps, "blockReason"), std::string("No camera profile is open for this aircraft."));
    CHECK_EQ(r.c.spotTarget(7, tr::SpotView::Chase, r.t), std::string());
    CHECK(r.c.ownership() == Ownership::Acquiring);
    CHECK(r.backend.count("acquire") == 1);
    r.acquire();
    r.advance(1);
    CHECK(r.c.ownership() == Ownership::Active);
    CHECK(r.c.activeCameraId().empty());
    CHECK(r.c.spotOverlay());
    CHECK(!r.backend.submitted.empty());
    CHECK(r.shown().referential == Referential::World);
    r.checkAimed();
    const auto offset = ap::local(r.c.spotPose().position, r.camera());
    CHECK_NEAR(offset.y, -60, 1);
    json active;
    for (const auto& [n, p] : r.events) if (n == "ActiveCameraChanged") active = p;
    CHECK(active["cameraId"].is_null());
    // Fly-by and Next target work the same way; binoculars still zoom; glance is refused; mouse look is harmless.
    CHECK_EQ(r.c.spotTarget(7, tr::SpotView::Flyby, r.t), std::string());
    r.advance(1);
    CHECK(r.c.spotOverlay());
    CHECK_EQ(r.c.spotTarget(7, tr::SpotView::Chase, r.t), std::string());
    r.advance(1);
    r.events.clear();
    r.c.onAction({fscs::profiles::Action::Binoculars, 0, true, 5}, r.t);
    r.advance(1);
    CHECK(r.shown().fovRad < tr::kChaseFovDeg * core::kDegToRad * .5);
    r.c.onAction({fscs::profiles::Action::Binoculars, 0, false, 5}, r.t);
    r.c.onAction({fscs::profiles::Action::GlanceNext, 0, true}, r.t);
    json triggered;
    for (const auto& [n, p] : r.events) if (n == "BindingTriggered") triggered = p;
    CHECK(!js::str(triggered, "result").empty());
    CHECK_EQ(status(r.send("MouseLookDelta", {{"headingDeg", 2}, {"pitchDeg", 1}})), std::string("Rejected"));
    r.c.onBackendEvent(be::NativeMouseButton{true}, r.t);
    r.c.onBackendEvent(be::NativeMouseAxis{true, 4000}, r.t);
    r.advance(.5);
    CHECK(r.c.spotting());
    r.checkAimed();
    // Track holds over the chase; tower and runway without data are refused plainly.
    CHECK_EQ(r.c.spotTarget(7, tr::SpotView::Tower, r.t), std::string("No tower position is known for an airport near this aircraft yet."));
    CHECK(r.c.spotting());
    // Opening a profile, closing it again and a profile for another aircraft never release the spot.
    r.c.setProfile(fscs::profiles::toSnapshot(r.profile), r.t);
    r.advance(.5);
    CHECK(r.c.spotting());
    CHECK(r.c.ownership() == Ownership::Active);
    auto other = fscs::profiles::toSnapshot(r.profile);
    other.modelKey = "another-aircraft";
    r.c.setProfile(other, r.t);
    r.c.setProfile(std::nullopt, r.t);
    r.c.setSettings(fscs::profiles::HostSettings{}, r.t);
    r.advance(.5);
    CHECK(r.c.spotting());
    CHECK(r.c.ownership() == Ownership::Active);
    // ... unless spotting itself becomes unavailable.
    r.c.onBackendEvent(be::CameraStatus{SdkAvailability::Acquired, true}, r.t);  // the simulator pauses the camera
    r.advance(.2);
    r.c.onBackendEvent(be::CameraStatus{SdkAvailability::Acquired, false}, r.t);
    r.advance(.2);
    CHECK(r.c.ownership() == Ownership::Active);
    CHECK(r.c.spotting());
    // StopSpotting: nothing to go back to, so the simulator gets the camera back.
    r.c.stopSpotting("Spotting stopped.", r.t);
    CHECK(!r.c.spotting());
    CHECK(r.c.ownership() == Ownership::Releasing);
    r.c.onBackendEvent(be::CameraStatus{SdkAvailability::NotAcquired, false}, r.t);
    r.advance(.1);
    CHECK(r.c.ownership() == Ownership::Observing);
    // Release and Panic as before.
    CHECK_EQ(r.c.spotTarget(7, tr::SpotView::Chase, r.t), std::string());
    r.acquire();
    r.c.panicRelease(r.t);
    CHECK(!r.c.spotting());
    r.c.onBackendEvent(be::CameraStatus{SdkAvailability::NotAcquired, false}, r.t);
    r.advance(.1);
    // Spotting unavailable (axis check pending): refused with that reason.
    r.qual.adapterQualified = false;
    r.c.refreshQualification();
    CHECK(!r.c.spottingAvailable());
    CHECK(r.c.capabilitiesJson()["spottingBlockReason"].is_string());
    CHECK(r.c.spotTarget(7, tr::SpotView::Chase, r.t).find("Axis mapping") != std::string::npos);
}

TEST_CASE("spotting coordinator: a profile going away under a chase keeps the spot without its base view") {
    SpotRig r;
    r.truth = [](double t) { return std::vector<be::TrafficAircraft>{plane(7, 0, -6000 + 70 * t, 300, 90, 70)}; };
    r.send("ActivateCamera", {{"cameraId", r.pilot}});
    r.acquire();
    r.advance(.5);
    CHECK_EQ(r.c.spotTarget(7, tr::SpotView::Chase, r.t), std::string());
    r.advance(1);
    CHECK_EQ(r.c.activeCameraId(), r.pilot);
    r.c.setProfile(std::nullopt, r.t);
    r.advance(1);
    CHECK(r.c.spotting());
    CHECK(r.c.ownership() == Ownership::Active);
    CHECK(r.c.activeCameraId().empty());
    const auto n = r.c.submissions();
    r.advance(.5);
    CHECK(r.c.submissions() > n);
    r.checkAimed();
    r.c.stopSpotting("Spotting stopped.", r.t);
    CHECK(r.c.ownership() == Ownership::Releasing);
    // A profile tower view (not an overlay) still releases when the profile goes.
    r.c.onBackendEvent(be::CameraStatus{SdkAvailability::NotAcquired, false}, r.t);
    r.c.setProfile(fscs::profiles::toSnapshot(r.profile), r.t);
    r.advance(.2);
    CHECK_EQ(r.c.spotTarget(7, tr::SpotView::Tower, r.t), std::string());
    r.acquire();
    CHECK_EQ(r.c.activeCameraId(), r.tower);
    r.c.setProfile(std::nullopt, r.t);
    CHECK(!r.c.spotting());
}

TEST_CASE("spotting coordinator: computed runway and tower views from scenery data, with or without a profile") {
    SpotRig r(false, false);
    r.c.setSpotAirports({tr::spotAirport(airportFixture())});
    // On final for 09, descending.
    r.truth = [](double t) { return std::vector<be::TrafficAircraft>{plane(7, 0, -7000 + 70 * t, 370 - 3.7 * t, 90, 70, -3.7)}; };
    r.advance(.1);
    const auto views = r.c.spotViewsJson();
    CHECK(views["runway"].get<bool>());
    CHECK(views["tower"].get<bool>());
    CHECK(r.c.spotViewAvailable(tr::SpotView::Runway, 7, r.t));
    CHECK_EQ(r.c.spotTarget(7, tr::SpotView::Runway, r.t), std::string());
    r.acquire();
    r.advance(1);
    CHECK(r.c.spotOverlay());
    const auto spot = r.c.spotRunway();
    CHECK(spot.has_value());
    if (spot) {
        CHECK_EQ(spot->runwayEnd, std::string("09"));
        CHECK_EQ(spot->kind, std::string("fence"));
    }
    const auto place = at(r.camera());
    CHECK_NEAR(place.y, -1150, .5);
    CHECK_NEAR(place.x, -80, .5);
    CHECK_NEAR(-place.z, 3, .1);
    r.checkAimed();
    // Zooms on the target like fly-by as it comes close; stays put.
    const double far = r.shown().fovRad;
    r.advance(80);
    CHECK_NEAR(gap(r.camera(), ap::world(kAirport, place)), 0, 1e-6);
    CHECK(r.shown().fovRad > far);
    r.checkAimed();
    // A pinned spot is kept: another end, a custom offset.
    const tr::RunwaySpot pinned{"KAAA", "27", "custom", 50, 20, 8};
    CHECK_EQ(r.c.spotTarget(7, tr::SpotView::Runway, r.t, pinned), std::string());
    r.advance(10);
    const auto custom = at(r.camera());
    CHECK_NEAR(custom.y, 1050, .5);
    CHECK_NEAR(custom.x, 20, .5);
    CHECK_NEAR(-custom.z, 8, .1);
    CHECK(r.c.spotTarget(7, tr::SpotView::Runway, r.t, tr::RunwaySpot{"KAAA", "36", "fence", 0, 0, 0}).find("not known") != std::string::npos);
    // Tower: the scenery tower position at the tower camera height.
    CHECK_EQ(r.c.spotTarget(7, tr::SpotView::Tower, r.t), std::string());
    r.advance(.5);
    CHECK_NEAR(at(r.camera()).x, 111, 1);
    CHECK_NEAR(-at(r.camera()).z, 35, .1);
    r.checkAimed();
    // Without runway data, refused plainly.
    r.c.setSpotAirports({});
    CHECK_EQ(r.c.spotTarget(7, tr::SpotView::Runway, r.t), std::string("No runway data is known for an airport near this aircraft yet."));
}

TEST_CASE("spotting coordinator: computed runway view re-picks the end as the target goes round, with the dwell") {
    SpotRig r(false, false);
    r.c.setSpotAirports({tr::spotAirport(airportFixture())});
    // Parked near the 27 end, then taxiing west along the runway towards 09.
    r.truth = [](double t) {
        const double east = t < 5 ? 900 : std::max(-900.0, 900 - 15 * (t - 5));
        return std::vector<be::TrafficAircraft>{plane(7, 50, east, 0, 270, t < 5 ? 0 : 15, 0, true)};
    };
    r.advance(.2);
    CHECK_EQ(r.c.spotTarget(7, tr::SpotView::Runway, r.t), std::string());
    r.acquire();
    r.advance(1);
    const auto end = [&r] { const auto spot = r.c.spotRunway(); return spot ? spot->runwayEnd : std::string(); };
    CHECK_EQ(end(), std::string("27"));
    r.advance(130);  // taxied to the far end
    CHECK_EQ(end(), std::string("09"));
    r.checkAimed();
}

TEST_CASE("module: per-frame watch follows spotting; runway spots from cached scenery data without a profile; auto-spot prefers runway") {
    // An aircraft without any camera profile, and no temporary airport sets.
    SpotModule m(false, "SimObjects\\Airplanes\\test_jet\\aircraft.cfg");
    CHECK_EQ(status(m.setting("airportAutoGenerate", false)), std::string("Completed"));
    m.truth = [](double t) {
        return std::vector<be::TrafficAircraft>{plane(7, 0, -9000 + 70 * t, 470 - 3.7 * t, 90, 70, -3.7), plane(8, 0, 900, 0, 270, 0, 0, true)};
    };
    m.seconds(2);
    CHECK(!m.app.coordinator().canActivate());
    auto snap = m.snapshot();
    CHECK(snap["capabilities"]["spottingAvailable"].get<bool>());
    CHECK(!snap["capabilities"]["canActivate"].get<bool>());
    CHECK(!snap["spotting"]["views"]["runway"].get<bool>());
    // Chase without a profile; the watch starts, changes with the target and stops with spotting.
    CHECK_EQ(status(m.panel("SpotTarget", {{"objectId", 8}, {"view", "chase"}})), std::string("Completed"));
    m.seconds(.5);
    CHECK(m.app.coordinator().ownership() == Ownership::Active);
    CHECK(!m.services.watchRequests.empty());
    CHECK_EQ(m.services.watchRequests.back(), 8u);
    CHECK_EQ(status(m.panel("SpotTarget", {{"objectId", 7}, {"view", "flyby"}})), std::string("Completed"));
    m.seconds(.2);
    CHECK_EQ(m.services.watchRequests.back(), 7u);
    m.app.onEvent(be::TrafficTarget{frameOf(m.truth(m.t)[0], {0, 70, 3.7})}, m.t);
    m.frame();
    CHECK(m.app.coordinator().traffic().frameFresh(7, m.t));
    CHECK_EQ(status(m.panel("StopSpotting")), std::string("Completed"));
    m.seconds(.2);
    CHECK_EQ(m.services.watchRequests.back(), 0u);
    CHECK(m.app.coordinator().ownership() == Ownership::Releasing);
    m.app.onEvent(be::CameraStatus{SdkAvailability::NotAcquired, false}, m.t);
    m.seconds(.2);
    // Scenery knows a nearby airport; auto-spot asks for its facility data and then shows the arrival from the 09 fence.
    m.app.onEvent(be::AirportList{json::array({{{"icao", "KAAA"}, {"region", "K1"}, {"lat", 40}, {"lon", -90}, {"altitude", 100}}}), true}, m.t);
    CHECK_EQ(status(m.panel("SetAutoSpot", {{"enabled", true}})), std::string("Completed"));
    m.seconds(3);
    bool answered = false;
    for (auto it = m.services.airportRequests.rbegin(); it != m.services.airportRequests.rend() && !answered; ++it)
        if (it->first == "KAAA") { m.app.onEvent(be::AirportFacility{it->second, airportFixture(), ""}, m.t); answered = true; }
    CHECK(answered);
    m.seconds(8);
    CHECK(m.app.coordinator().spotting());
    CHECK_EQ(m.app.coordinator().spotTargetId(), 7u);
    CHECK(m.app.coordinator().spotView() == tr::SpotView::Runway);
    CHECK(m.app.coordinator().spotOverlay());
    snap = m.snapshot();
    CHECK(snap["spotting"]["views"]["runway"].get<bool>());
    CHECK_EQ(snap["spotting"]["runways"].size(), std::size_t{2});
    CHECK_EQ(js::str(snap["spotting"]["spot"], "runwayEnd"), std::string("09"));
    CHECK_EQ(js::str(snap["spotting"]["spot"], "kind"), std::string("fence"));
    CHECK(snap["activeCamera"]["cameraId"].is_null());
    // SpotTarget.spot pins a runway end; it needs the runway view and a known end.
    auto ack = m.panel("SpotTarget", {{"objectId", 7}, {"view", "runway"}, {"spot", {{"icao", "KAAA"}, {"runwayEnd", "27"}, {"kind", "approach"}}}});
    CHECK_EQ(status(ack), std::string("Completed"));
    CHECK_EQ(js::str(ack["result"]["spotting"]["spot"], "runwayEnd"), std::string("27"));
    CHECK_EQ(js::str(ack["result"]["spotting"]["spot"], "kind"), std::string("approach"));
    CHECK_EQ(status(m.panel("SpotTarget", {{"objectId", 7}, {"view", "chase"}, {"spot", {{"icao", "KAAA"}, {"runwayEnd", "27"}}}})), std::string("Rejected"));
    CHECK_EQ(status(m.panel("SpotTarget", {{"objectId", 7}, {"view", "runway"}, {"spot", {{"icao", "KAAA"}, {"runwayEnd", "05"}}}})), std::string("Rejected"));
    m.panel("ReleaseCamera");
    m.seconds(.2);
    CHECK_EQ(m.services.watchRequests.back(), 0u);
}
