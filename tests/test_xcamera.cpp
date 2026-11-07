#include "test.hpp"

#include "fscs/profiles/json_util.hpp"
#include "fscs/profiles/repository.hpp"
#include "fscs/profiles/xcamera.hpp"

#include <cmath>
#include <map>
#include <string>
#include <vector>

using namespace fscs::profiles;
namespace js = fscs::js;
using fscs::core::Quat;

namespace {

// X-Camera 2.4's header (72 columns, with its own spacing).
const char* const kHeader =
    "Category Name,Camera Name,View ID,X,Y,Z,heading,pitch,TrackIR Enabled,Smooth Transition,HeadShake,RetainMouseLookPosition, "
    "Enable Temporary Positioning,Enable Linear Transition,Linear Transition Frames,Enable Auto Advance,Limit TrackIR, Max TrackIR X,"
    "Max TrackIR Y,Max TrackIR Z,Min TrackIR X,Min TrackIR Y,Min TrackIR Z,Zoom,FOV,Enable FOV,Smooth Camera Joins, Enable Track "
    "Heading, Enable Track Pitch, CGY Offset, CGZ Offset, Enable Level Camera, Enable View Axis, Enable External Camera, Enable "
    "Cineflex Camera, Enable Free Camera, Initial Movement Range, Enable Bezier Transition, Run Code,Transition Seconds,Hidden "
    "Camera,X-Camera Version,Default FOV,Aircraft,Author,Camera File Version,Category Index,Manual Enable, Latitude,Longitude, "
    "Elevation, X-Plane 9 Chase View, roll, Target Following Camera, Scenic Flyer GA Panel, Scenic Flyer Glass Panel, Cinema "
    "Verite, CGX Offset, Hide Yoke, Enable Camera Axis, Enable XPRealistic, Enable RealView, Disable 3D Cockpit Zoom, AGL Camera, "
    "Height ABove AGL (Meters),Circle View, Transition Delay, Script ID, Camera Origin,Target Reciprocal, G-Loaded Camera, Passive "
    "Camera ";

std::vector<std::string> headerNames() {
    std::vector<std::string> out;
    std::string cur;
    for (const char* p = kHeader;; ++p) {
        if (*p == ',' || !*p) {
            std::size_t a = cur.find_first_not_of(' '), b = cur.find_last_not_of(' ');
            out.push_back(a == std::string::npos ? "" : cur.substr(a, b - a + 1));
            cur.clear();
            if (!*p) break;
        } else {
            cur.push_back(*p);
        }
    }
    return out;
}

// One row: every column FALSE/0 unless given.
std::string row(const std::map<std::string, std::string>& v) {
    std::string out;
    const auto names = headerNames();
    for (std::size_t i = 0; i < names.size(); ++i) {
        const auto it = v.find(names[i]);
        out += (i ? "," : "") + (it != v.end() ? it->second : names[i] == "Zoom" ? "1.000000" : names[i] == "Default FOV" ? "65.0" : "FALSE");
    }
    return out;
}

std::string cam(const char* category, const char* name, int viewId, double x, double y, double z, double heading, double pitch,
                std::map<std::string, std::string> extra = {}) {
    extra["Category Name"] = category;
    extra["Camera Name"] = name;
    extra["View ID"] = std::to_string(viewId);
    extra["X"] = std::to_string(x);
    extra["Y"] = std::to_string(y);
    extra["Z"] = std::to_string(z);
    extra["heading"] = std::to_string(heading);
    extra["pitch"] = std::to_string(pitch);
    return row(extra);
}

const json* cameraNamed(const json& doc, const std::string& name) {
    for (const auto& c : js::array(doc, "cameras"))
        if (js::str(c, "name") == name) return &c;
    return nullptr;
}

fscs::core::Vec3 position(const json& c) {
    const auto& p = c["positionMetersFRD"];
    return {p[0].get<double>(), p[1].get<double>(), p[2].get<double>()};
}

fscs::core::Pbh angles(const json& c) {
    const auto& q = c["orientationCameraToAnchorXYZW"];
    return Quat{q[0].get<double>(), q[1].get<double>(), q[2].get<double>(), q[3].get<double>()}.toPbh();
}

xcamera::Source source(std::string csv) {
    xcamera::Source s;
    s.csv = std::move(csv);
    s.eyepointBody = fscs::core::Vec3{-0.5, 2.0, -12.0};  // x right, y up, z aft
    s.modelKey = "toliss a319";
    s.displayName = "ToLiss A319";
    s.fileName = "X-Camera_a319.csv";
    return s;
}

}  // namespace

TEST_CASE("xcamera: the eyepoint comes from the .acf in meters from the reference point") {
    xcamera::AcfEyepoint eye;
    CHECK(!eye.feed("I"));
    CHECK(!eye.feed("P acf/_cgY 0.300000012"));
    CHECK(!eye.feed("P acf/_cgZ 2.900000095"));
    CHECK(!eye.feed("P acf/_cgZ_aft 3.899999857"));  // a longer key with the same start is not the one
    CHECK(!eye.feed("P acf/_pe_xyz/0 -0.800000012"));
    CHECK(!eye.feed("P acf/_pe_xyz/1 1.509999990"));
    CHECK(eye.feed("P acf/_pe_xyz/2 3.500000000"));
    const auto e = eye.result();
    CHECK(e.has_value());
    // X-Plane 12's C172: X-Camera's own pilot view sits exactly here (-0.243840, 0.368808, 0.182880).
    CHECK_NEAR(e->x, -0.24384, 1e-5);
    CHECK_NEAR(e->y, 0.368808, 1e-5);
    CHECK_NEAR(e->z, 0.18288, 1e-5);
}

TEST_CASE("xcamera: cockpit cameras are measured from the eyepoint, exterior ones from the aircraft") {
    std::string csv = std::string(kHeader) + "\r\n" +
                      cam("Cockpit", "Pilot View", 1, -0.5, 2.0, -12.0, 359.0, 351.0, {{"Smooth Transition", "TRUE"}, {"Transition Seconds", "1.5"}}) + "\r\n" +
                      cam("Cockpit", "FMS", 4, -0.2, 1.6, -12.7, 6.0, 300.0, {{"Enable FOV", "TRUE"}, {"FOV", "50.0"}, {"RetainMouseLookPosition", "TRUE"}}) + "\r\n" +
                      cam("Outside", "Left wing", 2, -20.0, 3.0, 2.0, 90.0, 350.0, {{"Enable External Camera", "TRUE"}}) + "\r\n";
    std::string error;
    auto report = importXCamera(source(csv), &error);
    CHECK(report.has_value());
    if (!report) return;
    const json& doc = report->profile;
    CHECK_EQ(report->cameraCount, std::size_t{3});
    CHECK_EQ(js::str(doc, "name"), std::string("ToLiss A319 - X-Camera"));
    CHECK_EQ(js::str(js::object(doc, "aircraftMatch"), "modelKey"), std::string("toliss a319"));
    CHECK_EQ(doc["categories"].size(), std::size_t{2});

    const json* pilot = cameraNamed(doc, "Pilot View");
    CHECK(pilot != nullptr);
    CHECK_EQ(js::str(*pilot, "mode"), std::string("aircraftCockpit"));
    CHECK_EQ(js::str(doc, "defaultCameraId"), js::str(*pilot, "id"));
    CHECK_NEAR(position(*pilot).length(), 0.0, 1e-9);
    CHECK_NEAR(angles(*pilot).headingDeg, -1.0, 1e-6);
    CHECK_NEAR(angles(*pilot).pitchDeg, -9.0, 1e-6);
    CHECK_EQ(js::str(pilot->at("transition"), "kind"), std::string("smoothstep3"));
    CHECK_NEAR(pilot->at("transition")["durationSeconds"].get<double>(), 1.5, 1e-9);
    CHECK_NEAR(pilot->at("projection")["value"].get<double>(), 65.0 * fscs::core::kDegToRad, 1e-9);  // X-Camera's default FOV

    const json* fms = cameraNamed(doc, "FMS");
    const auto f = position(*fms);
    CHECK_NEAR(f.x, 0.7, 1e-9);   // 0.7 m ahead of the pilot
    CHECK_NEAR(f.y, 0.3, 1e-9);   // 0.3 m right
    CHECK_NEAR(f.z, 0.4, 1e-9);   // 0.4 m lower
    CHECK_NEAR(fms->at("projection")["value"].get<double>(), 50.0 * fscs::core::kDegToRad, 1e-9);
    CHECK_EQ(js::str(*fms, "temporaryLookPolicy"), std::string("retain"));
    CHECK_EQ(js::str(fms->at("transition"), "kind"), std::string("cut"));
    CHECK_EQ(js::integer(*fms, "viewId", 0), std::int64_t{4});

    const json* wing = cameraNamed(doc, "Left wing");
    CHECK_EQ(js::str(*wing, "mode"), std::string("aircraftExterior"));
    const auto w = position(*wing);
    CHECK_NEAR(w.x, -2.0, 1e-9);
    CHECK_NEAR(w.y, -20.0, 1e-9);
    CHECK_NEAR(w.z, -3.0, 1e-9);
    CHECK_NEAR(angles(*wing).headingDeg, 90.0, 1e-6);
    CHECK(!js::boolean(*wing, "interaction", true));
}

TEST_CASE("xcamera: paths become orbits and fly-bys, other paths keep their first point") {
    std::string csv = std::string(kHeader) + "\n" + cam("Cockpit", "Pilot View", 1, -0.5, 2.0, -12.0, 0, 0) + "\n";
    // A clockwise lap of 50 m at 5 m, 36 points 0.8 s apart, starting on the right.
    for (int i = 0; i <= 36; ++i) {
        const double a = (90 + i * 10) * fscs::core::kDegToRad;  // angle from the nose, clockwise
        const double fwd = 50 * std::cos(a), right = 50 * std::sin(a);
        csv += cam("Orbit 1", ("Orbit 1 - " + std::to_string(i)).c_str(), 0, right, 5.0, -fwd, 0, 0,
                   {{"Enable External Camera", "TRUE"}, {"Hidden Camera", i ? "TRUE" : "FALSE"}, {"Transition Seconds", i ? "0.8" : "0.0"},
                    {"Enable Track Heading", "TRUE"}}) + "\n";
    }
    // From 500 m ahead to 480 m behind, 30 m to the left and 20 m up, in 30 s.
    csv += cam("FlyBy_1", "FlyBy_1 - 0", 10, -30.0, 20.0, -500.0, 180, 0, {{"Enable External Camera", "TRUE"}}) + "\n";
    csv += cam("FlyBy_1", "FlyBy_1 - 1", 0, -30.0, 20.0, 480.0, 0, 0, {{"Hidden Camera", "TRUE"}, {"Transition Seconds", "30.0"}}) + "\n";
    // A walkaround: three points along the left side.
    csv += cam("Tour", "Start", 0, -10.0, 1.0, -20.0, 90, 0, {{"Enable External Camera", "TRUE"}}) + "\n";
    csv += cam("Tour", "Start", 0, -12.0, 1.0, 0.0, 90, 0, {{"Hidden Camera", "TRUE"}}) + "\n";
    csv += cam("Tour", "Start", 0, -10.0, 1.0, 20.0, 90, 0, {{"Hidden Camera", "TRUE"}}) + "\n";
    std::string error;
    auto report = importXCamera(source(csv), &error);
    CHECK(report.has_value());
    if (!report) return;
    const json& doc = report->profile;
    CHECK_EQ(report->cameraCount, std::size_t{4});

    const json* orbit = cameraNamed(doc, "Orbit 1");
    CHECK(orbit != nullptr);
    CHECK_EQ(js::str(*orbit, "mode"), std::string("orbit"));
    CHECK_NEAR(orbit->at("motion")["radiusMeters"].get<double>(), 50.0, 1e-3);
    CHECK_NEAR(orbit->at("motion")["heightMeters"].get<double>(), 5.0, 1e-3);
    CHECK_NEAR(orbit->at("motion")["durationSeconds"].get<double>(), 28.8, 1e-3);
    CHECK(orbit->at("motion")["clockwise"].get<bool>());
    CHECK(js::boolean(*orbit, "favorite", false));

    const json* flyby = cameraNamed(doc, "FlyBy_1");
    CHECK(flyby != nullptr);
    CHECK_EQ(js::str(*flyby, "mode"), std::string("flyby"));
    CHECK_NEAR(flyby->at("motion")["radiusMeters"].get<double>(), 490.0, 1e-3);
    CHECK_NEAR(flyby->at("motion")["sideMeters"].get<double>(), -30.0, 1e-3);
    CHECK_NEAR(flyby->at("motion")["heightMeters"].get<double>(), 20.0, 1e-3);
    CHECK_NEAR(flyby->at("motion")["durationSeconds"].get<double>(), 30.0, 1e-3);
    CHECK_EQ(js::integer(*flyby, "viewId", 0), std::int64_t{10});

    const json* tour = cameraNamed(doc, "Start");
    CHECK(tour != nullptr);
    CHECK_EQ(js::str(*tour, "mode"), std::string("aircraftExterior"));
    CHECK_NEAR(position(*tour).x, 20.0, 1e-9);
    CHECK_EQ(report->notes.size(), std::size_t{3});
}

TEST_CASE("xcamera: older files, Latin-1 names, commas, duplicate IDs and world cameras") {
    // X-Camera 2.3 wrote 69 columns: no Target Reciprocal, G-Loaded or Passive Camera, and no roll after it.
    std::string header = kHeader;
    header = header.substr(0, header.find(",Target Reciprocal"));
    std::string csv = header + "\n";
    auto trimmed = [](std::string r) {
        for (int i = 0; i < 3; ++i) r = r.substr(0, r.rfind(','));
        return r;
    };
    csv += trimmed(cam("Cockpit", "Pilot View", 1, -0.5, 2.0, -12.0, 0, 0)) + "\n";
    csv += trimmed(cam("Cabine", "Si\xE8ge 3A, fen\xEAtre", 1, -1.5, 2.0, 0.0, 270, 0)) + "\n";
    csv += trimmed(cam("Tower", "Tower", 0, 0, 0, 0, 0, 0, {{"Latitude", "47.45"}, {"Longitude", "8.55"}})) + "\n";
    csv += "garbage,row\n";
    std::string error;
    auto report = importXCamera(source(csv), &error);
    CHECK(report.has_value());
    if (!report) return;
    const json& doc = report->profile;
    CHECK_EQ(report->cameraCount, std::size_t{2});
    const json* seat = cameraNamed(doc, "Si\xC3\xA8ge 3A, fen\xC3\xAAtre");
    CHECK(seat != nullptr);
    CHECK_EQ(js::integer(*seat, "viewId", -1), std::int64_t{0});  // view ID 1 belongs to the pilot view
    CHECK_EQ(js::str(*seat, "mode"), std::string("aircraftCockpit"));
    CHECK_EQ(report->notes.size(), std::size_t{3});  // duplicate ID, world camera, unreadable row
}

TEST_CASE("xcamera: without an eyepoint, cockpit views are measured from X-Camera's pilot view") {
    std::string csv = std::string(kHeader) + "\n" + cam("Cockpit", "Overhead", 0, 0.0, 2.2, -12.2, 0, 50) + "\n" +
                      cam("Cockpit", "Pilot View", 1, -0.5, 2.0, -12.0, 0, 0) + "\n" +
                      cam("Cockpit", "Way out", 2, 0.0, 2.0, 40.0, 0, 0) + "\n";
    auto s = source(csv);
    s.eyepointBody.reset();
    std::string error;
    auto report = importXCamera(s, &error);
    CHECK(report.has_value());
    if (!report) return;
    CHECK_NEAR(position(*cameraNamed(report->profile, "Pilot View")).length(), 0.0, 1e-9);
    CHECK_NEAR(position(*cameraNamed(report->profile, "Overhead")).x, 0.2, 1e-9);
    CHECK_EQ(js::str(*cameraNamed(report->profile, "Way out"), "mode"), std::string("aircraftExterior"));
    CHECK(report->notes.front().find("\"Pilot View\"") != std::string::npos);
}

TEST_CASE("xcamera: files that are not X-Camera cameras are refused") {
    std::string error;
    CHECK(!importXCamera(source(""), &error));
    CHECK(!importXCamera(source("{\"format\":\"fs-camera-studio-profile\"}"), &error));
    CHECK(error.find("not an X-Camera") != std::string::npos);
    CHECK(!importXCamera(source(std::string(kHeader) + "\n"), &error));
    CHECK(!importXCamera(source(std::string(xcamera::kMaxCsvBytes + 1, 'x')), &error));
}
