#pragma once
// File-only worker input: no XPLM calls off the simulator thread. Only numeric
// geometry is sent to the planner; never forward arbitrary aircraft-file text.
#include <nlohmann/json.hpp>
#include <filesystem>
#include <fstream>
#include <string>
#include <cmath>

namespace fscs::xplane {
inline nlohmann::json readAircraftGeometry(const std::filesystem::path& path) {
    std::ifstream file(path);
    if (!file) return {{"error", "The loaded aircraft ACF could not be read."}};
    nlohmann::json properties = nlohmann::json::object();
    std::string line;
    std::size_t bytes = 0;
    while (std::getline(file,line)) {
        bytes += line.size();
        if (bytes > 32*1024*1024) return {{"error", "The aircraft ACF exceeds the study size limit."}};
        if (!line.starts_with("P ")) continue;
        const auto space=line.find(' ',2); if(space==std::string::npos)continue;
        const auto key=line.substr(2,space-2);
        const bool component=key.starts_with("_body/")||key.starts_with("_wing/");
        if (!(component && (key.find("/_geo_xyz/")!=std::string::npos||key.find("/_part_")!=std::string::npos||key.ends_with("/_s_dim")||key.ends_with("/_r_dim"))) &&
            !key.starts_with("_gear/") && !(key.starts_with("_engn/")&&key.find("/_part_")!=std::string::npos) &&
            !(key.starts_with("_blad/")&&(key.find("/_part_")!=std::string::npos||key.ends_with("/_semilen_SEG"))) &&
            !key.starts_with("acf/_cg") && key!="acf/_h_eqlbm" && key!="acf/_the_eqlbm" && key!="acf/_num_engn" && key!="acf/_num_prop") continue;
        try {
            std::size_t used=0;const auto text=line.substr(space+1);const double value=std::stod(text,&used);
            if(std::isfinite(value)&&text.find_first_not_of(" \t\r",used)==std::string::npos)properties[key]=value;
        } catch(const std::exception&) { /* descriptive fields are not geometry */ }
    }
    if(file.bad())return {{"error", "The aircraft ACF could not be read completely."}};
    return {{"acfProperties",std::move(properties)}};
}
}
