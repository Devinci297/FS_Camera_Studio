// Converts the X-Camera files XP Camera Studio finds in an X-Plane folder into Camera Studio profile files, with the
// same finder (xp_storage.cpp) and converter (xcamera.cpp) as Profiles > Import. X-Camera's files are only read.
//
//   xpcs_xcamera_convert <X-Plane folder> <output folder> [loaded aircraft .acf, relative to the X-Plane folder]
//
// Each profile is written as "<aircraft> - <file>.json"; copy them into
// <X-Plane>/Output/preferences/XPCameraStudio/imports to import them as files.
#include "fscs/profiles/repository.hpp"
#include "xp_storage.hpp"

#include <fstream>
#include <iostream>

namespace fs = std::filesystem;
using fscs::xplane::pathFromUtf8;

int main(int argc, char** argv) {
    if (argc < 3) {
        std::cerr << "Usage: xpcs_xcamera_convert <X-Plane folder> <output folder> [loaded aircraft .acf]\n";
        return 2;
    }
    const fs::path xplane = pathFromUtf8(argv[1]), out = pathFromUtf8(argv[2]);
    const std::string loaded = argc > 3 ? argv[3] : "";
    fscs::xplane::XPStorage storage(xplane / "Output" / "preferences" / "XPCameraStudio");
    storage.setXPlane(xplane, [loaded] { return loaded; });
    std::error_code ec;
    fs::create_directories(out, ec);
    int failures = 0, written = 0;
    storage.listXCameraFiles([&](std::vector<fscs::profiles::xcamera::File> files) {
        for (const auto& f : files) {
            storage.readXCameraFile(f.id, [&](std::optional<fscs::profiles::xcamera::Source> source, std::string error) {
                std::cout << f.aircraft << " / " << f.label << " (" << f.source << (f.loadedAircraft ? ", loaded" : "") << "): ";
                if (!source) {
                    std::cout << "FAILED " << error << "\n";
                    ++failures;
                    return;
                }
                std::string err;
                const auto report = fscs::profiles::importXCamera(*source, &err);
                if (!report) {
                    std::cout << "FAILED " << err << "\n";
                    ++failures;
                    return;
                }
                std::cout << report->cameraCount << " cameras, eyepoint " << (source->eyepointBody ? "from the .acf" : "not found") << "\n";
                for (const auto& note : report->notes) std::cout << "    " << note << "\n";
                std::string name = f.aircraft + " - " + f.label;
                for (char& c : name)
                    if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|') c = '_';
                name = name.substr(0, name.size() - 4) + ".json";
                std::ofstream file(out / pathFromUtf8(name), std::ios::binary | std::ios::trunc);
                file << fscs::profiles::exportProfileCode(report->profile, false);
                if (!file) {
                    std::cout << "    could not write " << name << "\n";
                    ++failures;
                } else {
                    ++written;
                }
            });
        }
    });
    std::cout << written << " profiles written, " << failures << " failed\n";
    return failures ? 1 : 0;
}
