#pragma once
// X-Camera camera files to Camera Studio profiles (X-Plane 12). X-Camera keeps one CSV per aircraft, next to the
// .acf ("X-Camera_<acf name>.csv"), plus community sets in its plugin folder. A row is one camera:
//
//   X, Y, Z        aircraft body axes (x right, y up, z aft), meters from the reference point: pilots_head_x/y/z
//   heading, pitch degrees 0..360 relative to the aircraft (pilots_head_psi/the), roll in newer files
//   Hidden Camera  TRUE for the second and later points of a camera path (orbits, fly-bys, swoops)
//
// Cockpit cameras become eyepoint-relative cockpit views (the eyepoint comes from the aircraft's .acf), exterior
// cameras aircraft-relative exterior views. A path that circles the aircraft becomes an orbit, one that passes it
// from ahead to behind a fly-by; any other path keeps its first point as a still view. Portable: no file access.

#include "fscs/core/math.hpp"

#include <nlohmann/json.hpp>

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace fscs::profiles::xcamera {

using json = nlohmann::json;

constexpr std::size_t kMaxCsvBytes = 4u << 20;

// One X-Camera file the host offers for import.
struct File {
    std::string id;        // opaque, from the host's latest listing
    std::string label;     // file name
    std::string aircraft;  // aircraft folder (or the loaded aircraft for community and imported files)
    std::string source;    // "aircraft", "community" or "imports"
    bool loadedAircraft = false;
};

// What the converter needs: the file text and, for cockpit views, the eyepoint it is measured against.
struct Source {
    std::string csv;
    std::optional<core::Vec3> eyepointBody;  // acf_peX/Y/Z: body axes, meters from the reference point
    std::string modelKey;                    // aircraft folder, lower case (Coordinator's aircraftFromPath)
    std::string displayName;
    std::string fileName;
};

struct Result {
    json profile;  // schema v4, not yet validated
    std::size_t cameraCount = 0;
    std::vector<std::string> notes;
};

// Nullopt with a plain-language `error` when the text is not an X-Camera camera file.
std::optional<Result> convert(const Source& source, std::string* error);

// The pilot's eyepoint from an .acf, fed one line at a time: "P acf/_pe_xyz/0..2" and the reference point
// "P acf/_cgY", "P acf/_cgZ", all in feet from the designer's datum. `feed` returns true once everything is read.
class AcfEyepoint {
public:
    bool feed(std::string_view line);
    std::optional<core::Vec3> result() const;  // body axes, meters from the reference point

private:
    double pe_[3] = {NAN, NAN, NAN};
    double cgY_ = NAN, cgZ_ = NAN;
};

}  // namespace fscs::profiles::xcamera
