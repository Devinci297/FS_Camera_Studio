#include "test.hpp"
#include "airport_fixture.hpp"
#include "fscs/profiles/json_util.hpp"
using namespace fscs::profiles;
namespace ap=fscs::profiles::airport;
TEST_CASE("airports: geometry generates valid deterministic world cameras and junction fallback") {
 std::vector<std::string> notes;auto p=ap::generate(airportFixture(),json::object(),notes);
 CHECK(p.is_object());CHECK(validateProfile(p).ok());CHECK_EQ(p["cameras"].size(),std::size_t{8});
 auto again=ap::generate(airportFixture(),json::object(),notes);CHECK_EQ(p["profileId"],again["profileId"]);
 bool junction=false;
 for(auto& cat:p["categories"])if(cat["name"]=="Taxiway intersections")junction=true;
 CHECK(junction);
 for(auto& cam:p["cameras"]){CHECK_EQ(cam["mode"],json("airport"));CHECK_EQ(cam["aircraftTracking"],json("both"));CHECK_EQ(cam["viewId"],json(0));CHECK_EQ(cam["worldOrigin"]["altitude"],json(100));}
 auto signs=airportFixture();signs["signs"]={{{"key","A"},{"label","A / B"},{"lat",40},{"lon",-90},{"altitude",100},{"heading",0}}};
 auto s=ap::generate(signs,json::object(),notes);bool actual=false;junction=false;
 for(auto& cat:s["categories"]){actual=actual||cat["name"]=="Taxi signs";junction=junction||cat["name"]=="Taxiway intersections";}
 CHECK(actual);CHECK(!junction);
}
TEST_CASE("airports: regenerate preserves refinements unless replacement requested") {
 std::vector<std::string> notes;auto p=ap::generate(airportFixture(),json::object(),notes);auto edited=p;
 edited["cameras"][0]["aircraftTracking"]="off";edited["cameras"][0]["name"]="My runway";edited["cameras"][0]["positionMetersFRD"][0]=123;
 auto preserved=ap::merge(edited,p,false);CHECK(validateProfile(preserved).ok());CHECK_EQ(preserved["cameras"][0]["name"],json("My runway"));
 CHECK_EQ(preserved["cameras"].size(),p["cameras"].size());CHECK_EQ(preserved["cameras"][0]["aircraftTracking"],json("off"));
 auto replaced=ap::merge(edited,p,true);CHECK(validateProfile(replaced).ok());CHECK_EQ(replaced["cameras"][0]["name"],p["cameras"][0]["name"]);
}
TEST_CASE("airports: roundtrip world coordinates and nearby selection with radius and hysteresis") {
 ap::Geo origin{40,179.999,100};fscs::core::Vec3 delta{123,1000,-30};auto w=ap::world(origin,delta);auto v=ap::local(origin,w);
 CHECK(w.lon<0);CHECK_NEAR(v.x,delta.x,1e-6);CHECK_NEAR(v.y,delta.y,1e-6);CHECK_NEAR(v.z,delta.z,1e-6);
 std::vector<std::string> notes;auto a=ap::generate(airportFixture(),json::object(),notes),b=ap::generate(airportFixture("KBBB",40.1),json::object(),notes);
 auto n=ap::nearby({a,b},{40,-90,100},25);CHECK_EQ(n.size(),std::size_t{2});CHECK_EQ(ap::choose(n,"",""),std::string("KAAA"));CHECK_EQ(ap::choose(n,"","KBBB"),std::string("KBBB"));
 n=ap::nearby({a,b},{40.051,-90,100},25);CHECK_EQ(ap::choose(n,"KAAA",""),std::string("KAAA"));
 n=ap::nearby({a,b},{42,-90,100},25);CHECK(n.empty());CHECK(ap::choose(n,"KAAA","KBBB").empty());
}
TEST_CASE("airports: invalid geometry and empty selected features produce no cameras") {
 std::vector<std::string> notes;auto f=airportFixture();f["lat"]=999;CHECK(ap::generate(f,json::object(),notes).is_null());
 CHECK(ap::generate(airportFixture(),{{"runways",false},{"parking",false},{"taxi",false},{"tower",false}},notes).is_null());
 CHECK(!notes.empty());
}
TEST_CASE("airports: scenery stand names preserve direction letters suffixes and distinct cameras") {
 CHECK(ap::parkingIdentity(9,12,12,0)["name"]==json("Gate A12"));
 CHECK(ap::parkingIdentity(9,12,10,13)["name"]==json("Gate 12B"));
 CHECK(ap::parkingIdentity(3,5,3,0)["name"]==json("Northeast parking 5"));
 CHECK(ap::parkingIdentity(11,2,11,0)["name"]==json("Dock 2"));
 CHECK(ap::parkingIdentity(3,9,12,0)["gate"]==json(true));
 CHECK(ap::parkingIdentity(9,12,12,0)["key"]==json("12:12"));
 auto f=airportFixture();f["parking"]=json::array();
 for(int suffix:{0,12,13}) {auto p=ap::parkingIdentity(9,12,10,suffix);p.update({{"north",100},{"east",100},{"heading",180}});f["parking"].push_back(p);}
 std::vector<std::string> notes;auto p=ap::generate(f,{{"runways",false},{"taxi",false},{"tower",false}},notes);
 CHECK_EQ(p["cameras"].size(),std::size_t{3});CHECK_EQ(p["categories"][0]["name"],json("Gates"));
 CHECK(p["cameras"][0]["id"]!=p["cameras"][1]["id"]);CHECK(p["cameras"][1]["id"]!=p["cameras"][2]["id"]);
}
