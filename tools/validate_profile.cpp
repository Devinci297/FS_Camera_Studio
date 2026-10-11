#include "fscs/profiles/repository.hpp"
#include <fstream>
#include <iostream>
#include <iterator>

// Exercise the same parser, migration and schema validation used by in-panel imports.
int main(int argc, char** argv) {
    if (argc < 2) { std::cerr << "Usage: fscs_validate_profile <profile.json> [...]\n"; return 2; }
    bool valid = true;
    for (int i = 1; i < argc; ++i) {
        std::ifstream input(argv[i], std::ios::binary);
        if (!input) { std::cerr << argv[i] << ": cannot open\n"; valid = false; continue; }
        const std::string code{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
        std::string error;
        const auto report = fscs::profiles::importProfileCode(code, &error);
        if (!report) { std::cerr << argv[i] << ": " << error << '\n'; valid = false; continue; }
        std::cout << argv[i] << ": valid (" << report->profile["cameras"].size() << " cameras, "
                  << report->profile["categories"].size() << " categories)\n";
    }
    return valid ? 0 : 1;
}
