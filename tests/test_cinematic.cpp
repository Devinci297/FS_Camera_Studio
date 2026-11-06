// Cinematic mode: flight-phase detection, phase-weighted view choice with a seeded random source, dwell timing, the
// manual-input pause, stopping on release / panic / manual recall, and the presentation-only Commercial drift.
#include "test.hpp"

#include "cinematic.hpp"
#include "coordinator.hpp"
#include "fake_backend.hpp"
#include "module_app.hpp"
#include "fscs/profiles/airport.hpp"
#include "fscs/profiles/json_util.hpp"
#include "airport_fixture.hpp"

#include <algorithm>
#include <map>
#include <set>

using namespace fscs::host;
using fscs::profiles::CameraMode;
using fscs::protocol::Command;
using fscs::protocol::json;
namespace core = fscs::core;
namespace js = fscs::js;
namespace be = backend_event;
namespace cin = fscs::host::cinematic;

namespace {

// Deterministic uniform [0, 1) source for the director (SplitMix64).
struct Seeded {
    std::uint64_t state;
    double operator()() {
        std::uint64_t z = (state += 0x9e3779b97f4a7c15ull);
        z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
        z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
        z ^= z >> 31;
        return static_cast<double>(z >> 11) * (1.0 / 9007199254740992.0);
    }
};

core::FlightMotion motion(bool ground, double speed, double climb) {
    core::FlightMotion m;
    m.onGround = ground ? 1 : 0;
    m.groundSpeed = speed;
    m.verticalSpeed = climb;
    return m;
}

}  // namespace

TEST_CASE("cinematic: flight phase from on-ground, ground speed and vertical speed, with hysteresis") {
    cin::PhaseTracker p;
    double t = 0;
    const auto at = [&](bool ground, double speed, double climb) { t += 1; return p.update(motion(ground, speed, climb), t); };
    CHECK(at(true, 0, 0) == cin::Phase::Parked);
    CHECK(at(true, 1.2, 0) == cin::Phase::Parked);  // below the 1.5 m/s exit threshold
    CHECK(at(true, 6, 0) == cin::Phase::Taxi);
    CHECK(at(true, 1.2, 0) == cin::Phase::Taxi);    // above the 1.0 m/s re-entry threshold
    CHECK(at(true, 0.5, 0) == cin::Phase::Parked);
    CHECK(at(true, 12, 0) == cin::Phase::Taxi);
    CHECK(at(true, 25, 0) == cin::Phase::TakeoffClimb);  // takeoff roll
    CHECK(at(true, 17, 0) == cin::Phase::TakeoffClimb);  // hysteresis: still rolling
    CHECK(at(false, 35, 0.2) == cin::Phase::TakeoffClimb);  // liftoff: the climb-out window counts even when level
    t += cin::PhaseTracker::kAfterLiftoffSeconds;
    CHECK(at(false, 60, 5) == cin::Phase::TakeoffClimb);
    CHECK(at(false, 60, 1.0) == cin::Phase::TakeoffClimb);  // above the 0.75 m/s exit threshold
    CHECK(at(false, 60, 0.3) == cin::Phase::Cruise);
    CHECK(at(false, 60, 1.5) == cin::Phase::Cruise);        // below the 2 m/s climb entry threshold
    CHECK(at(false, 60, -1.5) == cin::Phase::Cruise);
    CHECK(at(false, 55, -3) == cin::Phase::Approach);
    CHECK(at(false, 50, -1.0) == cin::Phase::Approach);     // hysteresis
    CHECK(at(false, 45, -0.5) == cin::Phase::Cruise);
    CHECK(at(false, 40, -3.5) == cin::Phase::Approach);
    CHECK(at(true, 38, -1) == cin::Phase::LandingRoll);     // touchdown
    CHECK(at(true, 20, 0) == cin::Phase::LandingRoll);
    CHECK(at(true, 14, 0) == cin::Phase::Taxi);             // slowed below 15 m/s
    CHECK(at(true, 18, 0) == cin::Phase::Taxi);             // not a landing roll again, and below the 20 m/s roll entry
    CHECK(at(true, 0, 0) == cin::Phase::Parked);
    // Non-finite telemetry changes nothing; a reset starts from the next sample.
    core::FlightMotion bad = motion(false, NAN, 0);
    CHECK(p.update(bad, t) == cin::Phase::Parked);
    p.reset();
    CHECK(p.update(motion(false, 120, 0), t + 1) == cin::Phase::Cruise);  // first sample already airborne: no climb-out window
    CHECK_EQ(std::string(cin::toString(cin::Phase::Approach)), std::string("approach"));
}

TEST_CASE("cinematic: phase weights, cockpit share and no immediate repeats with a seeded random source") {
    std::vector<cin::Candidate> list;
    for (int i = 0; i < 4; ++i) list.push_back({"ext" + std::to_string(i), CameraMode::AircraftExterior, ""});
    list.push_back({"orbit", CameraMode::Orbit, ""});
    list.push_back({"flyby", CameraMode::Flyby, ""});
    list.push_back({"mount", CameraMode::Mounted, ""});
    list.push_back({"tower", CameraMode::Airport, "tower"});
    list.push_back({"gate", CameraMode::Airport, "gates"});
    list.push_back({"pilot", CameraMode::AircraftCockpit, ""});
    const auto modeOf = [&](const std::string& id) {
        for (const auto& c : list) if (c.id == id) return c.mode;
        return CameraMode::Free;
    };
    const auto run = [&](cin::Phase phase, int picks, double cockpitWeight, std::uint64_t seed) {
        cin::Director d(Seeded{seed});
        std::map<std::string, int> count;
        std::string current = "ext0";
        for (int i = 0; i < picks; ++i) {
            const auto id = d.pick(list, current, phase, cockpitWeight);
            CHECK(id != current);  // never the same view twice in a row
            ++count[id];
            d.shown(id, 10, i * 10.0);
            current = id;
        }
        return count;
    };
    const int n = 6000;
    // Cruise: no airport views; exterior, orbit and fly-by dominate; cockpit about 15 %.
    auto cruise = run(cin::Phase::Cruise, n, .15, 1);
    CHECK_EQ(cruise["tower"] + cruise["gate"], 0);
    CHECK_NEAR(cruise["pilot"] / double(n), .15, .03);
    int exterior = 0;
    for (int i = 0; i < 4; ++i) exterior += cruise["ext" + std::to_string(i)];
    CHECK(exterior > cruise["mount"]);
    CHECK(cruise["orbit"] > cruise["mount"]);
    CHECK(cruise["flyby"] > cruise["mount"]);
    // Parked: airport views appear, tower favoured over a gate view.
    auto parked = run(cin::Phase::Parked, n, .15, 2);
    CHECK(parked["tower"] + parked["gate"] > n / 6);
    CHECK(parked["tower"] > parked["gate"] * 1.2);  // double weight, softened by the recent-view penalty
    CHECK(parked["tower"] > parked["ext1"]);
    // Approach also favours airport views over orbit.
    auto approach = run(cin::Phase::Approach, n, .15, 3);
    CHECK(approach["tower"] > approach["orbit"]);
    // Cockpit weight 0: never chosen while other views exist; 1: always when not already showing it.
    auto noCockpit = run(cin::Phase::Cruise, 500, 0, 4);  // keep the map alive: CHECK_EQ binds a reference
    CHECK_EQ(noCockpit["pilot"], 0);
    {
        cin::Director d(Seeded{5});
        CHECK_EQ(d.pick(list, "ext1", cin::Phase::Cruise, 1), std::string("pilot"));
    }
    // The same seed gives the same sequence (deterministic); another seed differs.
    CHECK(run(cin::Phase::Taxi, 200, .15, 9) == run(cin::Phase::Taxi, 200, .15, 9));
    CHECK(run(cin::Phase::Taxi, 200, .15, 9) != run(cin::Phase::Taxi, 200, .15, 10));
    // Recent views are penalised: with five equal views, a view rarely returns within two picks.
    {
        std::vector<cin::Candidate> five;
        for (int i = 0; i < 5; ++i) five.push_back({"v" + std::to_string(i), CameraMode::AircraftExterior, ""});
        cin::Director d(Seeded{11});
        std::vector<std::string> seq{"v0"};
        d.shown("v0", 10, 0);
        for (int i = 1; i < 2000; ++i) {
            seq.push_back(d.pick(five, seq.back(), cin::Phase::Cruise, 0));
            d.shown(seq.back(), 10, i * 10.0);
        }
        int quickRepeats = 0;
        for (std::size_t i = 2; i < seq.size(); ++i) quickRepeats += seq[i] == seq[i - 2];
        CHECK(quickRepeats < 2000 / 10);  // uniform without the penalty would be about 1/4
        CHECK_EQ(d.history().size(), cin::Director::kHistory);
    }
    // Only one candidate: it is kept; no candidates: nothing; only views unsuited to the phase: still a view.
    cin::Director d(Seeded{12});
    CHECK_EQ(d.pick({{"solo", CameraMode::AircraftExterior, ""}}, "solo", cin::Phase::Cruise, .15), std::string("solo"));
    CHECK(d.pick({}, "", cin::Phase::Cruise, .15).empty());
    const auto airportOnly = d.pick({{"a", CameraMode::Airport, "tower"}, {"b", CameraMode::Airport, "gates"}}, "a", cin::Phase::Cruise, .15);
    CHECK_EQ(airportOnly, std::string("b"));
    CHECK(modeOf("pilot") == CameraMode::AircraftCockpit);
    CHECK_EQ(cin::typeWeight(cin::Phase::Cruise, CameraMode::Free), 0.0);
    CHECK_EQ(cin::typeWeight(cin::Phase::Cruise, CameraMode::AircraftCockpit), 0.0);
}

TEST_CASE("cinematic: dwell is drawn within the settings and the clock pauses on manual input and suspension") {
    cin::Director d(Seeded{21});
    for (int i = 0; i < 500; ++i) {
        const double dwell = d.drawDwell(8, 20);
        CHECK(dwell >= 8 && dwell <= 20);
    }
    CHECK_NEAR(d.drawDwell(12, 12), 12.0, 1e-12);
    CHECK_NEAR(d.drawDwell(1, 2), 3.0, 1e-12);       // clamped to the 3 s floor
    CHECK(d.drawDwell(30, 10) >= 30);                 // max below min uses min
    // Injected values drive the draw exactly.
    cin::Director fixed([] { return .5; });
    CHECK_NEAR(fixed.drawDwell(8, 20), 14.0, 1e-12);

    double t = 100;
    d.shown("a", 5, t);
    const auto step = [&](double seconds, bool suspended = false) {
        bool due = false;
        for (int i = 0; i < static_cast<int>(std::lround(seconds * 10)); ++i) { t += .1; due = d.tick(t, suspended) || due; }
        return due;
    };
    CHECK(!step(4.8));
    CHECK(step(.4));                                  // due at 5 s
    d.shown("b", 5, t);
    CHECK(!step(2));
    d.manualInput(t);                                 // pauses for 10 s after the last input
    CHECK(d.paused(t + 9.9));
    CHECK(!step(9.9));
    CHECK_NEAR(d.elapsed(), 2.0, 1e-9);
    CHECK(!step(2.8));                                // resumed: 2 + 2.7 s
    CHECK(step(.5));
    d.shown("c", 5, t);
    CHECK(!step(20, true));                           // suspended (glance, binoculars): the clock stands still
    CHECK_NEAR(d.elapsed(), 0.0, 1e-12);
    CHECK(step(5.2));
    d.reset();
    CHECK(d.cameraId().empty());
    CHECK(!step(30));
}

TEST_CASE("cinematic: Commercial drift is level, bounded and fades a carried offset") {
    cin::Director d(Seeded{31});
    core::CameraPose pose;
    pose.orientation = core::Quat::fromPbh({-25, 0, 140});  // pitched down: the dolly still stays level
    pose.fovRad = 1.0;
    for (int i = 0; i < 200; ++i) {
        const double seconds = d.drawDwell(3, 300);
        const auto spec = d.drawDrift(pose.orientation, seconds);
        CHECK_NEAR(spec.direction.z, 0.0, 1e-12);
        CHECK_NEAR(spec.direction.length(), 1.0, 1e-9);
        CHECK(spec.meters <= core::kDriftMaxMeters + 1e-12);
        core::CommercialDrift drift;
        drift.start(spec, false);
        for (int k = 0; k <= 40; ++k) {
            const auto shown = drift.apply(pose);
            CHECK((shown.positionFRD - pose.positionFRD).length() <= core::kDriftMaxMeters + 1e-9);
            CHECK(std::abs(shown.fovRad - pose.fovRad) <= core::kDriftMaxFovDeg * core::kDegToRad * 1.0001);
            CHECK(std::abs(shown.fovRad - pose.fovRad) <= pose.fovRad * core::kDriftMaxFovFraction + 1e-12);
            drift.tick(seconds / 40, false);
        }
        CHECK_NEAR(drift.progress(), 1.0, 1e-9);
    }
    // Straight down: no level forward axis, so the right axis is used.
    const auto down = core::driftDirection(core::Quat::fromPbh({-90, 0, 0}), 0, 1);
    CHECK_NEAR(down.length(), 1.0, 1e-9);
    CHECK_NEAR(down.z, 0.0, 1e-12);
    // Travel follows smoothstep over the dwell; a hold freezes it; the full travel is speed x dwell, capped.
    core::CommercialDrift drift;
    drift.start({{1, 0, 0}, 1.2, -2 * core::kDegToRad, 10}, false);
    drift.tick(5, false);
    CHECK_NEAR(drift.offset().x, 0.6, 1e-9);
    drift.tick(3, true);
    CHECK_NEAR(drift.offset().x, 0.6, 1e-9);
    drift.tick(3, false, true);  // simulator pause
    CHECK_NEAR(drift.offset().x, 0.6, 1e-9);
    CHECK_NEAR(d.drawDrift(pose.orientation, 10).meters, 1.2, 1e-12);
    CHECK_NEAR(d.drawDrift(pose.orientation, 200).meters, core::kDriftMaxMeters, 1e-12);
    // A smooth move to the next view carries the offset and fades it; a cut drops it.
    drift.tick(10, false);
    CHECK_NEAR(drift.offset().x, 1.2, 1e-9);
    drift.start({{0, 1, 0}, 1.0, 0, 10}, true);
    CHECK_NEAR(drift.offset().x, 1.2, 1e-9);  // no jump
    drift.tick(core::kDriftCarrySeconds, false);
    CHECK_NEAR(drift.offset().x, 1.2 * std::exp(-1.0), 1e-9);
    drift.tick(10, false);
    CHECK(std::abs(drift.offset().x) < 1e-4);
    drift.start({{0, 1, 0}, 1.0, 0, 10}, false);
    CHECK_NEAR(drift.offset().length(), 0.0, 1e-12);
    drift.stop();
    CHECK(!drift.active());
    CHECK_NEAR(drift.apply(pose).fovRad, pose.fovRad, 1e-12);
}

TEST_CASE("cinematic: per-camera flag defaults, settings defaults and the Ctrl+Alt+K binding") {
    json c172 = fscs::profiles::makeC172Profile("asobo-c172", "C172");
    CHECK(fscs::profiles::validateProfile(c172).ok());
    const auto pilot = js::str(c172, "defaultCameraId");
    int included = 0;
    for (const auto& c : c172["cameras"]) {
        CHECK(c["cinematic"].is_boolean());
        const bool expected = c["mode"] != "aircraftCockpit" || c["id"] == pilot;
        CHECK_EQ(c["cinematic"].get<bool>(), expected);
        included += c["cinematic"].get<bool>();
    }
    CHECK_EQ(included, 9);  // 8 exterior views and the default pilot view
    const auto snapshot = fscs::profiles::toSnapshot(c172);
    CHECK(snapshot.camera(pilot)->cinematic);
    // An explicit choice is kept; a non-boolean is rejected. No schema bump.
    json doc = c172;
    doc["cameras"][1]["cinematic"] = true;
    doc["cameras"][12]["cinematic"] = false;
    CHECK(fscs::profiles::validateProfile(doc).ok());
    CHECK(doc["cameras"][1]["cinematic"].get<bool>());
    CHECK(!doc["cameras"][12]["cinematic"].get<bool>());
    CHECK_EQ(doc["schemaVersion"].get<int>(), fscs::profiles::kSchemaVersion);
    doc["cameras"][2]["cinematic"] = "yes";
    CHECK(!fscs::profiles::validateProfile(doc).ok());
    CHECK(!fscs::profiles::defaultCinematic({{"mode", "free"}, {"id", "x"}}, "x"));
    CHECK(fscs::profiles::defaultCinematic({{"mode", "mounted"}, {"id", "x"}}, ""));
    CHECK(fscs::profiles::defaultCinematic({{"mode", "airport"}}, ""));

    // Settings: defaults 8 / 20 s, 15 % cockpit, cuts and drift on; values are clamped.
    const auto defaults = fscs::profiles::parseHostSettings(fscs::profiles::defaultSettingsDocument());
    CHECK_EQ(defaults.cinematicMinSeconds, 8.0);
    CHECK_EQ(defaults.cinematicMaxSeconds, 20.0);
    CHECK_EQ(defaults.cinematicCockpitWeight, .15);
    CHECK(defaults.cinematicCuts && defaults.cinematicDrift);
    const auto host = fscs::profiles::defaultSettingsDocument()["host"];
    for (const char* key : {"cinematicMinSeconds", "cinematicMaxSeconds", "cinematicCockpitWeight", "cinematicCuts", "cinematicDrift"}) CHECK(host.contains(key));
    json s = fscs::profiles::defaultSettingsDocument();
    s["host"].update({{"cinematicMinSeconds", 1}, {"cinematicMaxSeconds", 2}, {"cinematicCockpitWeight", 4}, {"cinematicCuts", false}, {"cinematicDrift", false}});
    const auto clamped = fscs::profiles::parseHostSettings(s);
    CHECK_EQ(clamped.cinematicMinSeconds, 3.0);
    CHECK_EQ(clamped.cinematicMaxSeconds, 3.0);
    CHECK_EQ(clamped.cinematicCockpitWeight, 1.0);
    CHECK(!clamped.cinematicCuts && !clamped.cinematicDrift);
    const auto normalized = fscs::profiles::normalizeSettings(s)["host"];
    CHECK_EQ(normalized["cinematicMaxSeconds"].get<double>(), 3.0);
    CHECK(!normalized["cinematicDrift"].get<bool>());

    // Binding: appended action, Ctrl + Alt + K by default, no clash with any other default.
    CHECK(fscs::profiles::parseAction("toggleCinematic") == fscs::profiles::Action::ToggleCinematic);
    CHECK_EQ(std::string(fscs::profiles::toString(fscs::profiles::Action::ToggleCinematic)), std::string("toggleCinematic"));
    const auto bindings = fscs::profiles::defaultBindingsDocument();
    CHECK(fscs::profiles::parseBindings(bindings).problems.empty());
    int found = 0;
    std::set<std::string> definitions;
    for (const auto& b : bindings["bindings"]) {
        const auto def = js::str(js::object(b, "input"), "definition");
        CHECK(definitions.insert(def).second);
        if (js::str(b, "action") == "toggleCinematic") {
            ++found;
            CHECK_EQ(def, std::string("VK_LCONTROL+VK_LMENU+K"));
            CHECK_EQ(js::str(js::object(b, "input"), "label"), std::string("Ctrl + Alt + K"));
        }
    }
    CHECK_EQ(found, 1);
}

// ---------------------------------------------------------------------------------------------------------------
// Coordinator: cinematic transitions and the presentation-only drift.

namespace {
struct CineRig {
    FakeBackend backend;
    std::vector<std::pair<std::string, json>> events;
    Qualification qual;
    Coordinator c{backend, [this](const std::string& n, json p) { events.emplace_back(n, std::move(p)); },
                  [this](const std::string&) { return qual; }};
    double t = 0;
    int req = 0;
    json profile;
    std::vector<std::string> exterior, airport;
    std::string pilot;
    CineRig() {
        qual.adapterQualified = true;
        qual.captureQualified = true;
        profile = fscs::profiles::makeC172Profile("asobo-c172", "C172");
        CHECK(fscs::profiles::validateProfile(profile).ok());
        pilot = js::str(profile, "defaultCameraId");
        for (const auto& cam : profile["cameras"]) if (cam["mode"] == "aircraftExterior") exterior.push_back(cam["id"]);
        // Two world-fixed airport views of one airport, appended as the module combines a loaded airport.
        std::vector<std::string> notes;
        auto doc = fscs::profiles::airport::generate(airportFixture(), json::object(), notes);
        CHECK(doc.is_object());
        auto category = doc["categories"][0];
        category["cameraIds"] = json::array();
        for (int i = 0; i < 2; ++i) {
            auto cam = doc["cameras"][i];
            cam["categoryId"] = category["id"];
            category["cameraIds"].push_back(cam["id"]);
            airport.push_back(cam["id"]);
            profile["cameras"].push_back(cam);
        }
        profile["categories"].push_back(category);
        connect();
    }
    json send(const std::string& action, json payload = json::object()) {
        Command cmd;
        cmd.requestId = "c-" + std::to_string(++req);
        cmd.sessionEpoch = c.epoch();
        cmd.action = action;
        cmd.payload = std::move(payload);
        return c.handle(cmd, t);
    }
    void advance(double seconds) {
        for (int i = 0; i < static_cast<int>(std::lround(seconds * 100)); ++i) { t += .01; c.tick(t); }
    }
    void connect() {
        c.tick(t);
        c.onBackendEvent(be::Connected{"1.8.16.0"}, t);
        c.onBackendEvent(be::AircraftLoaded{"SimObjects\\Airplanes\\asobo-c172\\aircraft.cfg"}, t);
        c.onBackendEvent(be::CameraStatus{SdkAvailability::NotAcquired, false}, t);
        c.setProfile(fscs::profiles::toSnapshot(profile), t);
    }
    const fscs::profiles::CameraEntry& entry(const std::string& id) const { return *c.profile()->camera(id); }
};
}  // namespace

TEST_CASE("cinematic coordinator: smooth quintic moves between aircraft views, cuts where asked, any recall counts") {
    CineRig r;
    core::DriftSpec none;
    CHECK(!r.c.selectCinematicCamera(r.exterior[0], r.t, false, false, none));  // not active and no acquire
    const auto recalls0 = r.c.recalls();
    CHECK(r.c.selectCinematicCamera(r.exterior[0], r.t, true, false, none));    // acquire
    CHECK(r.c.ownership() == Ownership::Acquiring);
    r.c.onBackendEvent(be::CameraStatus{SdkAvailability::Acquired, false}, r.t);
    r.advance(2);
    CHECK(r.c.ownership() == Ownership::Active);
    CHECK_EQ(r.c.activeCameraId(), r.exterior[0]);
    CHECK_EQ(r.c.recalls(), recalls0 + 1);
    // Exterior to exterior: smooth, 2.5 s, whatever the camera's own transition says.
    CHECK(r.c.selectCinematicCamera(r.exterior[1], r.t, false, false, none));
    CHECK(r.c.engine().transitioning());
    r.advance(Coordinator::kCinematicTransitionSeconds - .2);
    CHECK(r.c.engine().transitioning());
    r.advance(.4);
    CHECK(!r.c.engine().transitioning());
    // To a world-referential airport view: a cut (a frame change is always a cut).
    CHECK(r.c.selectCinematicCamera(r.airport[0], r.t, false, true, none));
    CHECK(!r.c.engine().transitioning());
    // Airport to airport of the same origin: cut when asked, smooth otherwise.
    CHECK(r.c.selectCinematicCamera(r.airport[1], r.t, false, true, none));
    CHECK(!r.c.engine().transitioning());
    CHECK(r.c.selectCinematicCamera(r.airport[0], r.t, false, false, none));
    CHECK(r.c.engine().transitioning());
    r.advance(3);
    // A manual recall is counted too: the host uses this to stop Cinematic mode.
    const auto before = r.c.recalls();
    CHECK_EQ(js::str(r.send("ActivateCamera", {{"cameraId", r.pilot}}), "status"), std::string("Accepted"));
    CHECK_EQ(r.c.recalls(), before + 1);
    // The camera's own transition applies again to an ordinary recall.
    CHECK_EQ(js::str(r.send("ActivateCamera", {{"cameraId", r.exterior[2]}}), "status"), std::string("Accepted"));
    CHECK(!r.c.engine().transitioning());  // cockpit to exterior changes frame: a cut
    CHECK_EQ(js::str(r.send("ActivateCamera", {{"cameraId", r.exterior[3]}}), "status"), std::string("Accepted"));
    r.advance(r.entry(r.exterior[3]).transition.durationSeconds + .05);
    CHECK(!r.c.engine().transitioning());
}

TEST_CASE("cinematic coordinator: Commercial drift moves the submitted view only, never the engine, captures or poses") {
    CineRig r;
    const auto id = r.exterior[0];
    core::DriftSpec spec{core::driftDirection(r.entry(id).pose.orientation, 0, 1), 2.4, -3 * core::kDegToRad, 20};
    CHECK(r.c.selectCinematicCamera(id, r.t, true, true, spec));
    r.c.onBackendEvent(be::CameraStatus{SdkAvailability::Acquired, false}, r.t);
    r.advance(.5);
    CHECK(r.c.ownership() == Ownership::Active);
    const auto saved = r.entry(id).pose;
    const auto shownPosition = [&] { return fromSdk(r.backend.submitted.back(), r.qual.contract).positionFRD; };
    // Half way: about half the travel (smoothstep), engine untouched.
    r.advance(9.5);
    CHECK_NEAR((shownPosition() - saved.positionFRD).length(), 1.2, .02);
    // Held (manual-input pause, glance, binoculars): the drift stands still.
    r.c.holdCinematicDrift(true);
    const auto held = shownPosition();
    r.advance(5);
    CHECK_NEAR((shownPosition() - held).length(), 0.0, 1e-6);
    r.c.holdCinematicDrift(false);
    r.advance(11);
    const auto end = shownPosition();
    CHECK_NEAR((end - saved.positionFRD).length(), 2.4, 1e-6);
    CHECK_NEAR(end.z, saved.positionFRD.z, 1e-6);  // level dolly
    CHECK_NEAR(r.backend.submitted.back().fovRad, saved.fovRad - std::min(3 * core::kDegToRad, saved.fovRad * core::kDriftMaxFovFraction), 1e-6);
    // Never in the engine, the temporary look, a capture or a saved pose.
    const auto engine = r.c.engine().sample().pose;
    CHECK_NEAR((engine.positionFRD - saved.positionFRD).length(), 0.0, 1e-9);
    CHECK_NEAR(engine.fovRad, saved.fovRad, 1e-12);
    CHECK(!r.c.engine().look().active());
    r.events.clear();
    r.send("CaptureView");
    json captured;
    for (const auto& [n, p] : r.events) if (n == "ViewCaptured") captured = p;
    CHECK(captured.is_object());
    CHECK_NEAR(captured["positionMetersFRD"][0].get<double>(), saved.positionFRD.x, 1e-9);
    CHECK_NEAR(captured["fovRad"].get<double>(), saved.fovRad, 1e-12);
    // Stopped: the next submission is the saved pose again.
    r.c.stopCinematic();
    r.advance(.1);
    CHECK_NEAR((shownPosition() - saved.positionFRD).length(), 0.0, 1e-6);
    // A manual recall of another view shows it without any drift.
    CHECK(r.c.selectCinematicCamera(id, r.t, false, true, spec));
    r.advance(5);
    CHECK_EQ(js::str(r.send("ActivateCamera", {{"cameraId", r.exterior[1]}}), "status"), std::string("Accepted"));
    r.advance(3);
    CHECK_NEAR((shownPosition() - r.entry(r.exterior[1]).pose.positionFRD).length(), 0.0, 1e-6);
}

// ---------------------------------------------------------------------------------------------------------------
// Module: the session-wide playlist end to end, with panels closed where it matters.

namespace {
struct CineModule {
    FakeBackend camera;
    FakeServices services;
    FakeStorage storage;
    ModuleApp app{camera, services, storage, "boot-cinematic", "0.1.0-test"};
    double t = 0;
    int req = 0;
    std::size_t seen = 0;
    CineModule() {
        app.setCinematicRandom(Seeded{77});
        app.start(t);
        frame();
        app.onEvent(be::Connected{"1.8.16.0"}, t);
        app.onEvent(be::AircraftLoaded{"SimObjects\\Airplanes\\asobo_c172sp_g1000\\aircraft.cfg"}, t);
        app.onEvent(be::CameraStatus{SdkAvailability::NotAcquired, false}, t);
        frame(2);
        panel("Hello", {{"surface", "main"}});
        panel("QualificationStart");
    }
    std::vector<json> drain() {
        fscs::protocol::Reassembler r;
        std::vector<json> out;
        for (; seen < services.sent.size(); ++seen)
            if (auto whole = r.feed(services.sent[seen], t)) out.push_back(js::parse(*whole));
        return out;
    }
    json lastEvent(const std::string& name) {
        json found;
        for (auto& m : drain())
            if (js::str(m, "type") == "event" && js::str(m, "name") == name) found = m;
        return found;
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
    void frame(int n = 1) {
        for (int i = 0; i < n; ++i) {
            t += .1;
            app.onEvent(be::MotionTelemetry{flight, 0, 0}, t);
            app.update(t);
        }
    }
    void seconds(double s) { frame(static_cast<int>(std::lround(s * 10))); }
    std::size_t key(const std::string& definition) const {
        return static_cast<std::size_t>(std::find(services.mapped.begin(), services.mapped.end(), definition) - services.mapped.begin());
    }
    void press(const std::string& definition, bool down) { app.onEvent(be::InputEvent{key(definition), down}, t); }
    void tap(const std::string& definition) { press(definition, true); press(definition, false); }
    json snapshot() {
        const std::string id = "main-" + std::to_string(++req);
        json msg{{"protocolVersion", 2}, {"requestId", id}, {"uiInstanceId", "main-1"}, {"moduleBootId", app.bootId()},
                 {"sessionEpoch", app.coordinator().epoch()}, {"action", "RequestSnapshot"}, {"payload", json::object()}};
        app.onEvent(be::CommBusMessage{msg.dump()}, t);
        return lastEvent("Snapshot")["payload"];
    }
    void enable() {
        const auto ack = panel("SetCinematic", {{"enabled", true}});
        CHECK_EQ(js::str(ack, "status"), std::string("Completed"));
        if (app.coordinator().ownership() == Ownership::Acquiring) app.onEvent(be::CameraStatus{SdkAvailability::Acquired, false}, t);
        frame(5);
        CHECK(app.coordinator().ownership() == Ownership::Active);
        CHECK(app.cinematicEnabled());
    }
    bool included(const std::string& id) const {
        for (const auto& c : app.workingProfile()["cameras"])
            if (c["id"] == id) return c["cinematic"].get<bool>();
        return false;
    }
    std::string mode(const std::string& id) const {
        for (const auto& c : app.workingProfile()["cameras"])
            if (c["id"] == id) return c["mode"];
        return {};
    }
    core::FlightMotion flight = motion(true, 0, 0);
};
}  // namespace

TEST_CASE("module: Cinematic mode cycles eligible views on a randomised dwell, never repeating a view") {
    CineModule m;
    auto settings = m.snapshot()["working"]["settings"];
    settings["host"]["cinematicMinSeconds"] = 4;
    settings["host"]["cinematicMaxSeconds"] = 6;
    CHECK_EQ(js::str(m.panel("UpdateDraft", {{"ops", json::array({{{"op", "setSettings"}, {"settings", settings}}})}}), "status"), std::string("Completed"));
    // Observing: enabling takes control with the first cinematic view.
    CHECK(m.app.coordinator().ownership() == Ownership::Observing);
    m.enable();
    CHECK(m.lastEvent("CinematicChanged").is_object());
    m.frame(2);
    auto event = m.snapshot()["cinematic"];
    CHECK(event["enabled"].get<bool>());
    CHECK_EQ(event["state"].get<std::string>(), std::string("running"));
    CHECK_EQ(event["phase"].get<std::string>(), std::string("parked"));
    CHECK_EQ(event["cameraId"].get<std::string>(), m.app.coordinator().activeCameraId());
    std::vector<std::string> shown{m.app.coordinator().activeCameraId()};
    std::vector<double> switchedAt{m.t};
    int cockpit = 0;
    while (shown.size() < 40 && m.t < 2000) {
        m.frame();
        const auto now = m.app.coordinator().activeCameraId();
        if (now != shown.back()) {
            shown.push_back(now);
            switchedAt.push_back(m.t);
        }
    }
    CHECK_EQ(shown.size(), std::size_t{40});
    for (std::size_t i = 0; i < shown.size(); ++i) {
        CHECK(m.included(shown[i]));
        cockpit += m.mode(shown[i]) == "aircraftCockpit";
        if (i) {
            CHECK(shown[i] != shown[i - 1]);
            const double dwell = switchedAt[i] - switchedAt[i - 1];
            CHECK(dwell >= 4 - .15 && dwell <= 6 + .15);
        }
    }
    CHECK(cockpit < 15);   // about 15 % of picks
    CHECK(m.app.cinematicEnabled());
    CHECK(m.app.coordinator().ownership() == Ownership::Active);
    // In flight the phase changes and is reported.
    m.flight = motion(false, 60, 0);
    m.seconds(50);
    CHECK_EQ(m.snapshot()["cinematic"]["phase"].get<std::string>(), std::string("cruise"));
    CHECK(m.app.cinematicPhase() == cin::Phase::Cruise);
}

TEST_CASE("module: manual look or move pauses Cinematic mode for 10 s; glance and binoculars suspend it") {
    CineModule m;
    auto settings = m.snapshot()["working"]["settings"];
    settings["host"]["cinematicMinSeconds"] = 5;
    settings["host"]["cinematicMaxSeconds"] = 5;
    settings["host"]["cinematicCockpitWeight"] = 1;  // pilot view whenever another view is shown: glance needs a cockpit view
    CHECK_EQ(js::str(m.panel("UpdateDraft", {{"ops", json::array({{{"op", "setSettings"}, {"settings", settings}}})}}), "status"), std::string("Completed"));
    m.enable();
    const auto first = m.app.coordinator().activeCameraId();
    CHECK_EQ(m.mode(first), std::string("aircraftCockpit"));
    m.seconds(2);
    // Panel mouse look (MouseLookDelta) and a held look key both count as manual input.
    m.panel("MouseLookDelta", {{"headingDeg", 2.0}, {"pitchDeg", 0.0}});
    m.seconds(9.5);
    CHECK_EQ(m.app.coordinator().activeCameraId(), first);
    CHECK_EQ(m.snapshot()["cinematic"]["state"].get<std::string>(), std::string("paused"));
    m.seconds(2.8);   // resumed after 10 s: 2 + 2.3 s of the 5 s dwell
    CHECK_EQ(m.app.coordinator().activeCameraId(), first);
    m.seconds(1.2);
    const auto second = m.app.coordinator().activeCameraId();
    CHECK(second != first);
    CHECK(m.app.cinematicEnabled());
    // A held look key pauses too, for 10 s after it is released.
    m.press("VK_LCONTROL+VK_LMENU+VK_LEFT", true);
    m.seconds(8);
    m.press("VK_LCONTROL+VK_LMENU+VK_LEFT", false);
    m.seconds(9.5);
    CHECK_EQ(m.app.coordinator().activeCameraId(), second);
    m.seconds(6);
    const auto pilot = m.app.coordinator().activeCameraId();
    CHECK(pilot != second);
    CHECK_EQ(pilot, first);  // cockpit weight 1: back to the pilot view
    // Glance from the cinematic pilot view: the playlist holds, then continues after the return.
    m.seconds(1);
    m.press("VK_LCONTROL+VK_LMENU+G", true);
    m.seconds(1);
    CHECK(!m.app.coordinator().glanceJson().is_null());
    m.seconds(20);
    CHECK(m.app.cinematicEnabled());
    CHECK_EQ(m.snapshot()["cinematic"]["state"].get<std::string>(), std::string("suspended"));
    CHECK(!m.app.coordinator().glanceJson().is_null());
    m.press("VK_LCONTROL+VK_LMENU+G", false);
    m.seconds(1);
    CHECK_EQ(m.app.coordinator().activeCameraId(), pilot);
    CHECK(m.app.cinematicEnabled());
    m.seconds(3.5);
    CHECK(m.app.coordinator().activeCameraId() != pilot);  // 1 + 1 + 3 s of the dwell
    // Binoculars hold the current view the same way.
    const auto viewing = m.app.coordinator().activeCameraId();
    m.press("VK_LCONTROL+VK_LMENU+Z", true);
    m.seconds(20);
    CHECK_EQ(m.app.coordinator().activeCameraId(), viewing);
    CHECK(m.app.cinematicEnabled());
    m.press("VK_LCONTROL+VK_LMENU+Z", false);
    m.seconds(5.5);
    CHECK(m.app.coordinator().activeCameraId() != viewing);
}

TEST_CASE("module: Cinematic mode stops on Release, Panic, a manual recall and its own binding") {
    CineModule m;
    // The binding toggles it with both panels closed.
    CHECK(m.key("VK_LCONTROL+VK_LMENU+K") < m.services.mapped.size());
    m.tap("VK_LCONTROL+VK_LMENU+K");
    m.app.onEvent(be::CameraStatus{SdkAvailability::Acquired, false}, m.t);
    m.frame(5);
    CHECK(m.app.cinematicEnabled());
    CHECK(m.app.coordinator().ownership() == Ownership::Active);
    m.tap("VK_LCONTROL+VK_LMENU+K");
    m.frame();
    CHECK(!m.app.cinematicEnabled());
    CHECK(m.app.coordinator().ownership() == Ownership::Active);  // turning it off keeps the view
    auto state = m.snapshot()["cinematic"];
    CHECK(!state["enabled"].get<bool>());
    CHECK_EQ(state["reason"].get<std::string>(), std::string("Cinematic mode was turned off."));

    // Release Camera stops it (not a pause): nothing resumes after a new Enable.
    m.enable();
    m.panel("ReleaseCamera");
    m.frame();
    CHECK(!m.app.cinematicEnabled());
    m.app.onEvent(be::CameraStatus{SdkAvailability::NotAcquired, false}, m.t);
    m.frame(3);
    CHECK(m.app.coordinator().ownership() == Ownership::Observing);
    m.panel("EnableCamera");
    m.app.onEvent(be::CameraStatus{SdkAvailability::Acquired, false}, m.t);
    m.frame(5);
    const auto enabled = m.app.coordinator().activeCameraId();
    m.seconds(30);
    CHECK_EQ(m.app.coordinator().activeCameraId(), enabled);
    CHECK(!m.app.cinematicEnabled());

    // A manual recall (a view binding) stops it and keeps the recalled view.
    m.enable();
    m.tap("VK_LCONTROL+VK_LMENU+1");
    m.frame();
    CHECK(!m.app.cinematicEnabled());
    CHECK_EQ(m.snapshot()["cinematic"]["reason"].get<std::string>(), std::string("A camera was selected manually."));
    const auto recalled = m.app.coordinator().activeCameraId();
    m.seconds(30);
    CHECK_EQ(m.app.coordinator().activeCameraId(), recalled);
    // So does a panel recall, and Next camera.
    m.enable();
    m.panel("ActivateCamera", {{"cameraId", recalled}});
    m.frame();
    CHECK(!m.app.cinematicEnabled());
    m.enable();
    m.tap("VK_LCONTROL+VK_LMENU+N");
    m.frame();
    CHECK(!m.app.cinematicEnabled());

    // Panic stops it and says so.
    m.enable();
    m.drain();
    m.tap("VK_LCONTROL+VK_LMENU+X");
    const auto panic = m.lastEvent("PanicReleased");
    CHECK(panic["payload"]["cinematicStopped"].get<bool>());
    CHECK(!m.app.cinematicEnabled());
    m.app.onEvent(be::CameraStatus{SdkAvailability::NotAcquired, false}, m.t);
    m.seconds(30);
    CHECK(m.app.coordinator().ownership() == Ownership::Observing);
    m.tap("VK_LCONTROL+VK_LMENU+X");
    CHECK(!m.lastEvent("PanicReleased")["payload"]["cinematicStopped"].get<bool>());

    // Off again through the panel; and nothing to show is refused.
    m.enable();
    CHECK_EQ(js::str(m.panel("SetCinematic", {{"enabled", false}}), "status"), std::string("Completed"));
    CHECK(!m.app.cinematicEnabled());
    json ops = json::array();
    for (auto cam : m.app.workingProfile()["cameras"]) {
        cam["cinematic"] = false;
        ops.push_back({{"op", "setCamera"}, {"camera", cam}});
    }
    CHECK_EQ(js::str(m.panel("UpdateDraft", {{"ops", ops}}), "status"), std::string("Completed"));
    const auto refused = m.panel("SetCinematic", {{"enabled", true}});
    CHECK_EQ(js::str(refused, "status"), std::string("Rejected"));
    CHECK(js::str(refused, "reason").find("No views are included") != std::string::npos);
    CHECK(!m.app.cinematicEnabled());
}
