#include "fscs/profiles/profile.hpp"
#include "fscs/profiles/repository.hpp"
#include <fstream>

int main(int argc, char** argv) {
    if (argc != 2) return 2;
    const auto doc = fscs::profiles::makeC172Profile("asobo_c172sp_g1000", "C172SP G1000 Passengers");
    std::ofstream file(argv[1], std::ios::binary);
    file << fscs::profiles::exportProfileCode(doc, false) << '\n';
    return file ? 0 : 1;
}
