#pragma once
#include "fscs/profiles/airport.hpp"
inline nlohmann::json airportFixture(const std::string& code="KAAA",double lat=40) {
 return {{"icao",code},{"name","Test airport"},{"lat",lat},{"lon",-90},{"altitude",100},
 {"runways",{{{"lat",lat},{"lon",-90},{"altitude",100},{"heading",90},{"length",2000},{"width",40},{"primary","09"},{"secondary","27"}}}},
 {"parking",{{{"name","Gate A1"},{"key","A1"},{"gate",true},{"north",100},{"east",100},{"heading",180}}}},
 {"points",{{{"north",0},{"east",0}},{{"north",100},{"east",0}},{{"north",0},{"east",100}},{{"north",-100},{"east",0}}}},
 {"paths",{{{"type",1},{"start",0},{"end",1},{"width",20}},{{"type",1},{"start",0},{"end",2},{"width",20}},{{"type",1},{"start",0},{"end",3},{"width",20}}}},
 {"tower",{{"lat",lat+.001},{"lon",-90},{"altitude",125}}}};
}
