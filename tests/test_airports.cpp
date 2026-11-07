#include "test.hpp"
#include "airport_fixture.hpp"
#include "fscs/profiles/json_util.hpp"
using namespace fscs::profiles;
namespace ap=fscs::profiles::airport;
TEST_CASE("airports: geometry generates valid deterministic world cameras and junction fallback") {
 std::vector<std::string> notes;auto p=ap::generate(airportFixture(),json::object(),notes);
 CHECK(p.is_object());CHECK(validateProfile(p).ok());CHECK_EQ(p["cameras"].size(),std::size_t{12});
 auto again=ap::generate(airportFixture(),json::object(),notes);CHECK_EQ(p["profileId"],again["profileId"]);
 bool junction=false;
 for(auto& cat:p["categories"])if(cat["name"]=="Taxiway intersections")junction=true;
 CHECK(junction);
 for(auto& cam:p["cameras"]){CHECK_EQ(cam["favorite"],json(true));CHECK_EQ(cam["mode"],json("airport"));CHECK_EQ(cam["aircraftTracking"],json("both"));CHECK_EQ(cam["viewId"],json(0));CHECK_EQ(cam["worldOrigin"]["altitude"],json(100));}
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

TEST_CASE("airports: tower preserves scenery position or provides a labeled stable fallback") {
 const json options={{"runways",false},{"parking",false},{"taxi",false},{"tower",true}};
 std::vector<std::string> notes;auto f=airportFixture();
 auto actual=ap::generate(f,options,notes);CHECK(validateProfile(actual).ok());
 CHECK_EQ(actual["cameras"].size(),std::size_t{1});
 CHECK_EQ(actual["cameras"][0]["name"],json("Tower overview"));
 const auto expected=ap::local(ap::geo(f),ap::geo(f["tower"]));
 CHECK_NEAR(actual["cameras"][0]["positionMetersFRD"][0].get<double>(),expected.x,1e-6);
 CHECK_NEAR(actual["cameras"][0]["positionMetersFRD"][1].get<double>(),expected.y,1e-6);
 CHECK_NEAR(actual["cameras"][0]["positionMetersFRD"][2].get<double>(),-35,1e-6);
 CHECK(notes.empty());
 const auto id=actual["cameras"][0]["id"];
 for(const auto& tower:std::vector<json>{nullptr,{{"lat",0},{"lon",0},{"altitude",0}},{{"lat",95},{"lon",-90},{"altitude",100}},{{"lat",41},{"lon",-90},{"altitude",100}}}){
   f["tower"]=tower;notes.clear();auto p=ap::generate(f,options,notes);
   CHECK(validateProfile(p).ok());CHECK_EQ(p["cameras"].size(),std::size_t{1});
   CHECK_EQ(p["cameras"][0]["id"],id);CHECK_EQ(p["cameras"][0]["name"],json("Virtual tower"));
   CHECK_EQ(p["categories"][0]["name"],json("Tower"));CHECK_EQ(notes.size(),std::size_t{1});
   // Longest runway center is at the airport origin, heading east, and the only stand is north of it:
   // the virtual tower is 300 m north (left of the runway heading) and 35 m above airport elevation.
   CHECK_NEAR(p["cameras"][0]["positionMetersFRD"][0].get<double>(),300,1e-6);
   CHECK_NEAR(p["cameras"][0]["positionMetersFRD"][1].get<double>(),0,1e-6);
   CHECK_NEAR(p["cameras"][0]["positionMetersFRD"][2].get<double>(),-35,1e-6);
 }
 f.erase("tower");f["runways"]=json::array();notes.clear();auto noRunway=ap::generate(f,options,notes);
 CHECK(validateProfile(noRunway).ok());CHECK_EQ(noRunway["cameras"][0]["positionMetersFRD"],json({100,100,-35}));CHECK_EQ(noRunway["cameras"][0]["name"],json("Virtual tower"));
 CHECK(ap::generate(f,{{"runways",false},{"parking",false},{"taxi",false},{"tower",false}},notes).is_null());
}

TEST_CASE("airports: large airports reserve a tower slot and regeneration adds it without replacing views") {
 auto f=airportFixture();f["parking"]=json::array();f.erase("tower");
 for(std::size_t n=0;n<ap::kMaxGeneratedCameras+10;++n)f["parking"].push_back({{"name","Parking "+std::to_string(n)},{"key",std::to_string(n)},{"north",100},{"east",100},{"heading",0}});
 std::vector<std::string> notes;const json opts={{"runways",false},{"taxi",false}};
 auto p=ap::generate(f,opts,notes);CHECK(validateProfile(p).ok());CHECK_EQ(p["cameras"].size(),ap::kMaxGeneratedCameras);CHECK(p["cameras"].size()<kMaxCameras);
 CHECK_EQ(p["cameras"].back()["generatedKey"],json("tower"));
 f=airportFixture();f.erase("tower");auto withoutTower=ap::generate(f,{{"tower",false}},notes);
 withoutTower["cameras"][0]["name"]="My edited view";
 auto regenerated=ap::generate(f,json::object(),notes);auto merged=ap::merge(withoutTower,regenerated,false);
 CHECK(validateProfile(merged).ok());CHECK_EQ(merged["cameras"].size(),withoutTower["cameras"].size()+1);
 CHECK_EQ(merged["cameras"][0]["name"],json("My edited view"));CHECK_EQ(merged["cameras"].back()["generatedKey"],json("tower"));
}

TEST_CASE("airports: generated cameras and categories use feature icons") {
 std::vector<std::string> notes;auto p=ap::generate(airportFixture(),json::object(),notes);
 for(const auto& cat:p["categories"]){
   const std::string name=cat["name"].get<std::string>();
   const std::string expected=name=="Runways"||name=="Spotting"?"view-runway":name=="Gates"?"view-gate":name=="Apron"?"view-apron":name=="Tower"?"view-tower":"view-taxiway";
   CHECK_EQ(cat["icon"],json(expected));
   for(const auto& cam:p["cameras"])if(cam["categoryId"]==cat["id"])CHECK_EQ(cam["icon"],cat["icon"]);
 }
}

TEST_CASE("airports: explicit replacement removes old generated views but preserves manual views and surviving bindings") {
 std::vector<std::string> notes;auto old=ap::generate(airportFixture(),json::object(),notes);
 auto manual=old["cameras"][0];manual["id"]=newUuid();manual["name"]="Custom stand";manual.erase("generatedKey");manual["viewId"]=99;old["cameras"].push_back(manual);old["categories"][0]["cameraIds"].push_back(manual["id"]);
 for(auto& cam:old["cameras"])if(fscs::js::str(cam,"generatedKey")=="tower")cam["viewId"]=88;
 auto tower=ap::generate(airportFixture(),{{"runways",false},{"parking",false},{"taxi",false},{"tower",true}},notes);
 auto replacement=ap::merge(old,tower,true);CHECK(validateProfile(replacement).ok());CHECK_EQ(replacement["cameras"].size(),std::size_t{2});
 bool kept=false,bound=false;for(const auto& cam:replacement["cameras"]){kept=kept||(cam["id"]==manual["id"]&&cam["viewId"]==99);bound=bound||(fscs::js::str(cam,"generatedKey")=="tower"&&cam["viewId"]==88);}
 CHECK(kept);CHECK(bound);
 replacement["aircraftMatch"]={{"strategy","aircraftPath"},{"modelKey","old-aircraft"}};CHECK(validateProfile(replacement).ok());CHECK_EQ(replacement["aircraftMatch"]["strategy"],json("none"));CHECK(replacement["aircraftMatch"]["modelKey"].is_null());
}

TEST_CASE("airports: category heights preserve scenery references and explicit generation refinements") {
 auto facility=airportFixture();facility["runways"][0]["altitude"]=110;
 facility["parking"].push_back({{"name","Parking 1"},{"key","1"},{"gate",false},{"north",200},{"east",100},{"heading",0}});
 std::vector<std::string> notes;
 const json heights={{"runways",8},{"gates",12},{"apron",6},{"taxi",9},{"tower",50}};
 auto generated=ap::generate(facility,{{"categoryHeights",heights}},notes);
 CHECK(validateProfile(generated).ok());
 for(const auto& cam:generated["cameras"]) {
  const auto meta=cam["airportHeight"];const auto category=meta["category"].get<std::string>();
  CHECK(!cam.contains("airportOverrides")||!cam["airportOverrides"].contains("heightMeters"));
  const double baseline=category=="runways"?110:100;
  CHECK_NEAR(meta["baseAltitudeMeters"].get<double>(),baseline,1e-6);
  const double expected=std::max(heights[category].get<double>(),meta["minimumMeters"].get<double>());
  CHECK_NEAR(cam["positionMetersFRD"][2].get<double>(),100-baseline-expected,1e-6);
 }
 auto explicitHeight=ap::generate(facility,{{"height",70},{"categoryHeights",heights}},notes);
 for(const auto& cam:explicitHeight["cameras"])CHECK_NEAR(cam["airportOverrides"]["heightMeters"].get<double>(),70,1e-6);
 auto inherited=ap::generate(facility,{{"height",70},{"useGlobalHeights",true},{"categoryHeights",heights}},notes);
 CHECK_EQ(inherited["cameras"][0]["positionMetersFRD"],generated["cameras"][0]["positionMetersFRD"]);
 CHECK(!inherited["cameras"][0].contains("airportOverrides")||!inherited["cameras"][0]["airportOverrides"].contains("heightMeters"));
}

// ---- Temporary airport sets, virtual tower and spotting views ----------------------------------------------
namespace {
fscs::core::Vec3 at(const json& cam){return {cam["positionMetersFRD"][0].get<double>(),cam["positionMetersFRD"][1].get<double>(),cam["positionMetersFRD"][2].get<double>()};}
fscs::core::Vec3 aim(const json& cam){const auto& q=cam["orientationCameraToAnchorXYZW"];return fscs::core::Quat{q[0].get<double>(),q[1].get<double>(),q[2].get<double>(),q[3].get<double>()}.rotate({1,0,0});}
// The camera looks straight at `target` (local NED meters).
bool looksAt(const json& cam,fscs::core::Vec3 target){const auto d=target-at(cam);return fscs::core::dot(aim(cam),d*(1/d.length()))>1-1e-9;}
const json* byKey(const json& doc,const std::string& key){for(const auto& c:doc["cameras"])if(fscs::js::str(c,"generatedKey")==key)return &c;return nullptr;}
}

TEST_CASE("airports: virtual tower stands 300 m abeam the longest runway on the side with more stands") {
 auto f=airportFixture();f.erase("tower");
 // A short crossing runway must not win; the long one runs north from a center 500 m east of the origin.
 const auto longCenter=ap::world(ap::geo(f),{0,500,0});
 f["runways"]=json::array({{{"lat",40},{"lon",-90},{"altitude",100},{"heading",90},{"length",800},{"width",30},{"primary","09"},{"secondary","27"}},
   {{"lat",longCenter.lat},{"lon",longCenter.lon},{"altitude",100},{"heading",0},{"length",3000},{"width",45},{"primary","36"},{"secondary","18"}}});
 // Heading north, right is east: two stands east of the runway, one west.
 f["parking"]=json::array({{{"name","Gate A1"},{"key","A1"},{"gate",true},{"north",0},{"east",800},{"heading",0}},
   {{"name","Gate A2"},{"key","A2"},{"gate",true},{"north",100},{"east",800},{"heading",0}},
   {{"name","Stand 9"},{"key","9"},{"gate",false},{"north",0},{"east",200},{"heading",0}}});
 const json towerOnly={{"runways",false},{"spotting",false},{"parking",false},{"taxi",false},{"tower",true}};
 std::vector<std::string> notes;auto p=ap::generate(f,towerOnly,notes);
 CHECK(validateProfile(p).ok());CHECK_EQ(p["cameras"].size(),std::size_t{1});
 const auto& tower=p["cameras"][0];CHECK_EQ(tower["name"],json("Virtual tower"));CHECK_EQ(tower["generatedKey"],json("tower"));
 CHECK_EQ(notes.size(),std::size_t{1});CHECK(notes[0].find("not the real control tower")!=std::string::npos);
 const auto center=ap::local(ap::geo(f),longCenter);
 CHECK_NEAR(at(tower).x,center.x,1e-6);CHECK_NEAR(at(tower).y,center.y+fscs::profiles::airport::kVirtualTowerAbeamMeters,1e-6);
 CHECK_NEAR(at(tower).z,-35,1e-6);CHECK(looksAt(tower,center));
 CHECK_EQ(tower["airportHeight"]["category"],json("tower"));
 // Tower category height follows the setting.
 auto higher=ap::generate(f,{{"runways",false},{"spotting",false},{"parking",false},{"taxi",false},{"categoryHeights",{{"tower",60}}}},notes);
 CHECK_NEAR(at(higher["cameras"][0]).z,-60,1e-6);
 // More stands west, or none at all, put it on the left (west for a north heading).
 f["parking"][2]["east"]=-200;f["parking"].push_back({{"name","Stand 10"},{"key","10"},{"north",0},{"east",-300}});f["parking"].push_back({{"name","Stand 11"},{"key","11"},{"north",0},{"east",-400}});
 auto west=ap::generate(f,towerOnly,notes);CHECK_NEAR(at(west["cameras"][0]).y,center.y-300,1e-6);
 f["parking"]=json::array();auto unknown=ap::generate(f,towerOnly,notes);CHECK_NEAR(at(unknown["cameras"][0]).y,center.y-300,1e-6);
 // Zero tower data from scenery is treated as missing; a real position is used as is.
 f["tower"]={{"lat",0},{"lon",0},{"altitude",0}};const auto zero=ap::generate(f,towerOnly,notes);CHECK_EQ(zero["cameras"][0]["name"],json("Virtual tower"));
 f["tower"]={{"lat",40.001},{"lon",-90},{"altitude",125}};notes.clear();auto real=ap::generate(f,towerOnly,notes);
 CHECK_EQ(real["cameras"][0]["name"],json("Tower overview"));CHECK(notes.empty());
}

TEST_CASE("airports: spotting views sit at the fence line and under the approach of every runway end") {
 namespace a=fscs::profiles::airport;
 std::vector<std::string> notes;auto p=ap::generate(airportFixture(),json::object(),notes);
 CHECK(validateProfile(p).ok());
 json spotting;for(const auto& cat:p["categories"])if(cat["name"]=="Spotting")spotting=cat;
 CHECK(spotting.is_object());CHECK_EQ(spotting["cameraIds"].size(),std::size_t{4});CHECK_EQ(spotting["icon"],json("view-runway"));
 // Runway 09/27: center at the origin, heading east, 2000 m long. 09 lands east from x=-1000 east.
 const double finalHeight=a::kApproachDistanceMeters*std::tan(a::kGlidepathDeg*fscs::core::kDegToRad)-a::kApproachBelowGlidepathMeters;
 CHECK_NEAR(finalHeight,7.06,0.01);
 struct End{const char* name;double sign;};
 for(const End end:{End{"09",1},End{"27",-1}}){
  const std::string name=end.name;const fscs::core::Vec3 threshold{0,-1000*end.sign,0};
  const auto* fence=byKey(p,"spot:"+name+":fence");const auto* final=byKey(p,"spot:"+name+":final");
  CHECK(fence!=nullptr);CHECK(final!=nullptr);if(!fence||!final)continue;
  CHECK_EQ((*fence)["categoryId"],spotting["id"]);CHECK_EQ((*fence)["name"],json(name+" fence line"));CHECK_EQ((*final)["name"],json(name+" approach, 1 NM"));
  // 150 m behind the threshold, 80 m right of the landing direction (south for 09, north for 27), 3 m runway height.
  CHECK_NEAR(at(*fence).x,-a::kFenceSideMeters*end.sign,1e-6);CHECK_NEAR(at(*fence).y,(-1000-a::kFenceBehindMeters)*end.sign,1e-6);CHECK_NEAR(at(*fence).z,-3,1e-6);
  CHECK(looksAt(*fence,{0,0,0}));
  // 1 NM out on the extended centerline, about 90 m below the 3-degree glidepath, looking back at the threshold.
  CHECK_NEAR(at(*final).x,0,1e-6);CHECK_NEAR(at(*final).y,(-1000-a::kApproachDistanceMeters)*end.sign,1e-6);CHECK_NEAR(at(*final).z,-finalHeight,1e-6);
  CHECK(looksAt(*final,threshold));
  CHECK_EQ((*final)["airportHeight"]["category"],json("runways"));CHECK_NEAR((*final)["airportHeight"]["minimumMeters"].get<double>(),finalHeight,1e-9);
 }
 // A higher runway category height lifts the fence view but never lowers the approach view below its minimum.
 auto raised=ap::generate(airportFixture(),{{"categoryHeights",{{"runways",5}}}},notes);
 CHECK_NEAR(at(*byKey(raised,"spot:09:fence")).z,-5,1e-6);CHECK_NEAR(at(*byKey(raised,"spot:09:final")).z,-finalHeight,1e-6);
 // Spotting follows the runway choice unless set explicitly.
 CHECK(byKey(ap::generate(airportFixture(),{{"runways",false}},notes),"spot:09:fence")==nullptr);
 auto only=ap::generate(airportFixture(),{{"runways",false},{"spotting",true},{"parking",false},{"taxi",false},{"tower",false}},notes);
 CHECK_EQ(only["cameras"].size(),std::size_t{4});
}

TEST_CASE("airports: automatic options cap stands by spread and skip taxi intersections") {
 auto f=airportFixture();f["parking"]=json::array();
 // A 10 x 3 grid of gates plus stands; the cap keeps four gates spread across the apron.
 for(int row=0;row<3;++row)for(int col=0;col<10;++col)f["parking"].push_back({{"name","Gate "+std::to_string(row*10+col)},{"key","g"+std::to_string(row*10+col)},{"gate",true},{"north",200+row*40},{"east",-450+col*100},{"heading",180}});
 for(int n=0;n<20;++n)f["parking"].push_back({{"name","Stand "+std::to_string(n)},{"key","s"+std::to_string(n)},{"gate",false},{"north",-300},{"east",n*10},{"heading",0}});
 std::vector<std::string> notes;auto p=ap::generate(f,ap::automaticOptions(json::object()),notes);
 CHECK(validateProfile(p).ok());CHECK(notes.empty());
 std::size_t gates=0,apron=0,runways=0,spotting=0,tower=0,taxi=0;double minX=1e9,maxX=-1e9,minY=1e9,maxY=-1e9;
 for(const auto& cat:p["categories"]){
  const auto name=cat["name"].get<std::string>();const auto n=cat["cameraIds"].size();
  if(name=="Gates")gates=n;else if(name=="Apron")apron=n;else if(name=="Runways")runways=n;else if(name=="Spotting")spotting=n;else if(name=="Tower")tower=n;else taxi+=n;
 }
 CHECK_EQ(gates,std::size_t{4});CHECK_EQ(apron,std::size_t{0});CHECK_EQ(taxi,std::size_t{0});
 CHECK_EQ(runways,std::size_t{5});CHECK_EQ(spotting,std::size_t{4});CHECK_EQ(tower,std::size_t{1});
 for(const auto& cam:p["cameras"])if(fscs::js::str(cam,"icon")=="view-gate"){const auto t=at(cam);minX=std::min(minX,t.x);maxX=std::max(maxX,t.x);minY=std::min(minY,t.y);maxY=std::max(maxY,t.y);}
 // Spread: the chosen gates reach both ends of the 900 m row and more than one row.
 CHECK(maxY-minY>800);CHECK(maxX-minX>40);
 // Deterministic, so the same airport always gets the same IDs.
 CHECK_EQ(ap::generate(f,ap::automaticOptions(json::object()),notes),p);
 // Few gates: every gate is kept and stands fill the remaining slots.
 f["parking"]=json::array({{{"name","Gate 1"},{"key","1"},{"gate",true},{"north",100},{"east",0}},{{"name","Stand 2"},{"key","2"},{"north",-100},{"east",0}},{{"name","Stand 3"},{"key","3"},{"north",-100},{"east",500}},{{"name","Stand 4"},{"key","4"},{"north",-100},{"east",-500}},{{"name","Stand 5"},{"key","5"},{"north",-100},{"east",10}}});
 auto few=ap::generate(f,ap::automaticOptions(json::object()),notes);
 CHECK(byKey(few,"parking:1")!=nullptr);CHECK(byKey(few,"parking:3")!=nullptr);CHECK(byKey(few,"parking:4")!=nullptr);
 std::size_t stands=0;for(const auto& cam:few["cameras"])stands+=fscs::js::str(cam,"generatedKey").rfind("parking:",0)==0;
 CHECK_EQ(stands,std::size_t{4});
 // The cache copy keeps what generation reads and drops the taxi network.
 auto compact=ap::compactFacility(airportFixture());CHECK(!compact.contains("points"));CHECK(!compact.contains("paths"));CHECK(compact.contains("tower"));
 const auto fromCache=ap::generate(compact,ap::automaticOptions(json::object()),notes);CHECK_EQ(fromCache,ap::generate(airportFixture(),ap::automaticOptions(json::object()),notes));
}

#include "fake_backend.hpp"
#include "module_app.hpp"
namespace {
namespace be=fscs::host::backend_event;
struct AirportRig {
 fscs::host::FakeBackend camera;fscs::host::FakeServices services;fscs::host::FakeStorage storage;
 fscs::host::ModuleApp app{camera,services,storage,"boot-airport-0001","0.1.0-test"};
 double t=0;int req=0;std::size_t seen=0;
 AirportRig(){app.start(t);frame();app.onEvent(be::Connected{"1.8.16.0"},t);app.onEvent(be::AircraftLoaded{"SimObjects\\Airplanes\\Asobo-C152\\aircraft.cfg"},t);
  app.onEvent(be::CameraStatus{fscs::host::SdkAvailability::NotAcquired,false},t);frame();panel("Hello",{{"surface","main"}});}
 std::vector<json> drain(){fscs::protocol::Reassembler r;std::vector<json> out;for(;seen<services.sent.size();++seen)if(auto whole=r.feed(services.sent[seen],t))out.push_back(fscs::js::parse(*whole));return out;}
 json lastEvent(const std::string& name){json found;for(auto& m:drain())if(fscs::js::str(m,"type")=="event"&&fscs::js::str(m,"name")==name)found=m;return found;}
 json snapshot(){panel("RequestSnapshot");return lastEvent("Snapshot")["payload"];}
 std::string loaded(){const auto snap=snapshot();return fscs::js::str(snap["airports"],"loaded");}
 std::string panel(const std::string& action,json payload=json::object(),std::optional<std::int64_t> revision={}){
  const std::string id="main-"+std::to_string(++req);
  json msg{{"protocolVersion",2},{"requestId",id},{"uiInstanceId","main-1"},{"moduleBootId",app.bootId()},{"sessionEpoch",app.coordinator().epoch()},{"action",action},{"payload",payload}};
  if(revision)msg["draftRevision"]=*revision;app.onEvent(be::CommBusMessage{msg.dump()},t);return id;
 }
 json ackFor(const std::string& id){for(auto& m:drain())if(fscs::js::str(m,"type")=="ack"&&fscs::js::str(m,"requestId")==id)return m;return nullptr;}
 void frame(int n=1){for(int i=0;i<n;++i){t+=1.0/60;app.update(t);}}
 void at(double lat,double lon){app.onEvent(be::AircraftPosition{lat,lon,100},t);}
 void sceneryList(json airports){app.onEvent(be::AirportList{std::move(airports),true},t);}
 // Answers the latest automatic facility request for `code`, as the simulator would.
 bool answer(const std::string& code,const json& facility,const std::string& error=""){
  for(auto it=services.airportRequests.rbegin();it!=services.airportRequests.rend();++it)if(it->first==code){app.onEvent(be::AirportFacility{it->second,facility,error},t);return true;}
  return false;
 }
 std::size_t requestsFor(const std::string& code)const{std::size_t n=0;for(const auto& r:services.airportRequests)n+=r.first==code;return n;}
 bool storedAirport(const std::string& code)const{for(const auto& [id,rec]:app.repository().committed().profiles)if(fscs::js::str(fscs::js::object(rec.current,"airport"),"icao")==code)return true;return false;}
};
json sceneryAirport(const std::string& code,double lat){return {{"icao",code},{"region","K1"},{"lat",lat},{"lon",-90},{"altitude",100}};}
// Flies to KAAA, lets the module discover it and answers its facility request.
void arriveAtKaaa(AirportRig& m){
 m.at(40,-90);m.frame(130);CHECK_EQ(m.services.nearbyRequests,1);
 m.sceneryList(json::array({sceneryAirport("KAAA",40),sceneryAirport("KFAR",41)}));m.frame(130);
 CHECK_EQ(m.requestsFor("KAAA"),std::size_t{1});CHECK_EQ(m.requestsFor("KFAR"),std::size_t{0});
 CHECK(m.answer("KAAA",airportFixture()));
}
}

TEST_CASE("airports: temporary set loads within radius, is never stored or dirty, and unloads outside") {
 AirportRig m;arriveAtKaaa(m);
 auto snap=m.snapshot();
 CHECK_EQ(snap["airports"]["loaded"],json("KAAA"));CHECK(snap["airports"]["transient"].get<bool>());CHECK(snap["airports"]["autoGenerate"].get<bool>());
 CHECK_EQ(snap["airports"]["loadedName"],json("Test airport"));
 CHECK_EQ(snap["airports"]["nearby"].size(),std::size_t{1});CHECK(snap["airports"]["nearby"][0]["transient"].get<bool>());
 // Loaded under Airport exactly like a saved set, with the same generator (here without an aircraft profile).
 std::vector<std::string> notes;const auto expected=ap::generate(airportFixture(),ap::automaticOptions(snap["working"]["settings"]["host"]["airportCategoryHeights"]),notes);
 CHECK_EQ(snap["working"]["profile"]["cameras"].size(),expected["cameras"].size());
 bool tower=false,spotting=false;for(const auto& cat:snap["working"]["profile"]["categories"]){CHECK_EQ(cat["airportIcao"],json("KAAA"));tower=tower||cat["name"]=="Tower";spotting=spotting||cat["name"]=="Spotting";}
 CHECK(tower);CHECK(spotting);
 for(const auto& cam:snap["working"]["profile"]["cameras"])CHECK(!snap["working"]["hasCommittedVersion"][cam["id"].get<std::string>()].get<bool>());
 CHECK(!m.app.dirty());CHECK(!snap["working"]["dirty"]["airport"].get<bool>());CHECK(!m.storedAirport("KAAA"));
 CHECK(m.app.coordinator().profile()!=nullptr);
 // Edits stay in memory; an ordinary save neither stores nor needs to store them.
 auto cam=snap["working"]["profile"]["cameras"][0];cam["name"]="Edited temporary view";
 CHECK_EQ(fscs::js::str(m.ackFor(m.panel("UpdateDraft",{{"ops",{{{"op","setCamera"},{"camera",cam}}}}},m.app.draftRevision())),"status"),std::string("Completed"));
 CHECK(!m.app.dirty());
 const auto writes=m.storage.writes;m.panel("CommitChanges");CHECK(!m.storedAirport("KAAA"));
 bool airportFile=false;for(const auto& [file,bytes]:m.storage.files)airportFile=airportFile||bytes.find("Edited temporary view")!=std::string::npos;
 CHECK(!airportFile);(void)writes;
 snap=m.snapshot();CHECK_EQ(snap["working"]["profile"]["cameras"][0]["name"],json("Edited temporary view"));
 // Outside the radius it unloads like a saved set.
 m.at(42,-90);m.frame(130);snap=m.snapshot();
 CHECK_EQ(snap["airports"]["loaded"],json(""));CHECK(!snap["airports"]["transient"].get<bool>());CHECK(!snap["working"]["profile"].is_object());
}

TEST_CASE("airports: temporary facility data is cached for the session and discovery is rate limited") {
 AirportRig m;arriveAtKaaa(m);
 m.frame(600);CHECK_EQ(m.requestsFor("KAAA"),std::size_t{1});CHECK_EQ(m.services.nearbyRequests,1);
 // Leaving and coming back regenerates from the cache without asking scenery again.
 m.at(42,-90);m.frame(130);CHECK_EQ(m.loaded(),std::string(""));
 m.at(40,-90);m.frame(130);
 CHECK_EQ(m.loaded(),std::string("KAAA"));CHECK_EQ(m.requestsFor("KAAA"),std::size_t{1});
 // Discovery is rate limited: at most every 30 s, and only after 3 NM of movement.
 CHECK_EQ(m.services.nearbyRequests,1);
 m.frame(2000);CHECK_EQ(m.services.nearbyRequests,1);
 m.at(40.06,-90);m.frame(130);CHECK_EQ(m.services.nearbyRequests,2);
 // An airport without usable scenery data is asked once, then skipped for the next nearest.
 m.sceneryList(json::array({sceneryAirport("KBAD",40.05),sceneryAirport("KBBB",40.1)}));
 m.at(40.05,-90);m.frame(130);CHECK_EQ(m.requestsFor("KBAD"),std::size_t{1});
 CHECK_EQ(m.loaded(),std::string("KAAA")); // kept while the next airport's data is pending
 CHECK(m.answer("KBAD",json::object(),"Airport was not found in installed scenery."));
 m.frame(130);CHECK_EQ(m.requestsFor("KBBB"),std::size_t{0}); // KAAA is still loaded within the 2 NM hysteresis
 m.frame(600);CHECK_EQ(m.requestsFor("KBAD"),std::size_t{1});
 // An unanswered request times out and is retried a minute later, not on every check.
 m.at(40.1,-90);m.frame(130);CHECK_EQ(m.requestsFor("KBBB"),std::size_t{1});
 m.frame(60*30);CHECK_EQ(m.requestsFor("KBBB"),std::size_t{1});
 m.frame(60*60);CHECK_EQ(m.requestsFor("KBBB"),std::size_t{2});
 CHECK(m.answer("KBBB",airportFixture("KBBB",40.1)));CHECK_EQ(m.loaded(),std::string("KBBB"));
}

TEST_CASE("airports: a saved set always wins over a temporary one") {
 AirportRig m;
 // A saved KBBB about 6 NM away is within the 25 NM radius: no temporary set is generated for KAAA.
 m.at(40,-90);
 m.panel("PreviewAirportSource",{{"facility",airportFixture("KBBB",40.1)}});auto preview=m.lastEvent("AirportPreview")["payload"]["profile"];
 json ids=json::array();for(const auto& c:preview["cameras"])ids.push_back(c["id"]);m.panel("SaveAirportPreview",{{"icao","KBBB"},{"cameraIds",ids}});
 m.frame(130);m.sceneryList(json::array({sceneryAirport("KAAA",40)}));m.frame(130);
 json snap=m.snapshot();CHECK_EQ(snap["airports"]["loaded"],json("KBBB"));CHECK(!snap["airports"]["transient"].get<bool>());
 CHECK_EQ(m.requestsFor("KAAA"),std::size_t{0});CHECK_EQ(m.services.nearbyRequests,0);
 // A temporary set gives way as soon as a saved set for the same airport exists.
 AirportRig n;arriveAtKaaa(n);snap=n.snapshot();CHECK(snap["airports"]["transient"].get<bool>());
 n.panel("PreviewAirportSource",{{"facility",airportFixture()},{"options",{{"runways",false},{"spotting",false},{"parking",false},{"taxi",false}}}});
 auto towerOnly=n.lastEvent("AirportPreview")["payload"]["profile"];
 auto saved=n.panel("SaveAirportPreview",{{"icao","KAAA"},{"cameraIds",{towerOnly["cameras"][0]["id"]}}});
 CHECK_EQ(fscs::js::str(n.ackFor(saved),"status"),std::string("Pending"));
 n.frame(130);snap=n.snapshot();
 CHECK_EQ(snap["airports"]["loaded"],json("KAAA"));CHECK(!snap["airports"]["transient"].get<bool>());
 CHECK_EQ(snap["working"]["profile"]["cameras"].size(),std::size_t{1});CHECK(n.storedAirport("KAAA"));CHECK(!n.app.dirty());
}

TEST_CASE("airports: saving a temporary set keeps its IDs, active view and view bindings") {
 AirportRig m;arriveAtKaaa(m);
 auto snap=m.snapshot();json tower;for(const auto& c:snap["working"]["profile"]["cameras"])if(c["generatedKey"]=="tower")tower=c;
 CHECK(tower.is_object());
 // Bind the tower to view 7 and show it before saving.
 tower["viewId"]=7;CHECK_EQ(fscs::js::str(m.ackFor(m.panel("UpdateDraft",{{"ops",{{{"op","setCamera"},{"camera",tower}}}}},m.app.draftRevision())),"status"),std::string("Completed"));
 m.panel("QualificationStart");m.panel("ActivateCamera",{{"cameraId",tower["id"]}});
 m.app.onEvent(be::CameraStatus{fscs::host::SdkAvailability::Acquired,false},m.t);m.frame();
 CHECK_EQ(m.app.coordinator().activeCameraId(),tower["id"].get<std::string>());
 const auto before=m.snapshot();std::vector<std::string> ids;for(const auto& c:before["working"]["profile"]["cameras"])ids.push_back(c["id"]);
 auto request=m.panel("SaveTemporaryAirport",{{"icao","KAAA"}});json ack,completed;
 for(const auto& msg:m.drain()){if(fscs::js::str(msg,"requestId")==request)ack=msg;if(fscs::js::str(msg,"name")=="CommitCompleted")completed=msg["payload"];}
 CHECK_EQ(fscs::js::str(ack,"status"),std::string("Pending"));CHECK_EQ(completed["status"],json("Committed"));CHECK_EQ(completed["kind"],json("airport"));
 CHECK(m.storedAirport("KAAA"));
 snap=m.snapshot();CHECK(!snap["airports"]["transient"].get<bool>());CHECK_EQ(snap["airports"]["loaded"],json("KAAA"));CHECK(!m.app.dirty());
 std::vector<std::string> after;for(const auto& c:snap["working"]["profile"]["cameras"]){after.push_back(c["id"]);CHECK(snap["working"]["hasCommittedVersion"][c["id"].get<std::string>()].get<bool>());}
 CHECK(after==ids);
 CHECK_EQ(m.app.coordinator().activeCameraId(),tower["id"].get<std::string>());CHECK(m.app.coordinator().ownership()==fscs::host::Ownership::Active);
 bool bound=false;for(const auto& [id,rec]:m.app.repository().committed().profiles)for(const auto& c:fscs::js::array(rec.current,"cameras"))bound=bound||(c["id"]==tower["id"]&&c["viewId"]==7);
 CHECK(bound);
 // Saved now: a second temporary save is refused, and later edits are ordinary unsaved airport edits.
 CHECK_EQ(fscs::js::str(m.ackFor(m.panel("SaveTemporaryAirport",{{"icao","KAAA"}})),"status"),std::string("Rejected"));
 tower["name"]="Tower, refined";m.panel("UpdateDraft",{{"ops",{{{"op","setCamera"},{"camera",tower}}}}},m.app.draftRevision());CHECK(m.app.dirty());
}

TEST_CASE("airports: turning airportAutoGenerate off unloads and stops temporary generation") {
 AirportRig m;
 const auto first=m.snapshot();auto settings=first["working"]["settings"];CHECK(settings["host"]["airportAutoGenerate"].get<bool>());
 settings["host"]["airportAutoGenerate"]=false;
 m.panel("UpdateDraft",{{"ops",{{{"op","setSettings"},{"settings",settings}}}}},m.app.draftRevision());
 m.at(40,-90);m.frame(130);m.sceneryList(json::array({sceneryAirport("KAAA",40)}));m.frame(130);
 auto snap=m.snapshot();CHECK_EQ(snap["airports"]["loaded"],json(""));CHECK(!snap["airports"]["autoGenerate"].get<bool>());
 CHECK_EQ(m.services.nearbyRequests,0);CHECK_EQ(m.requestsFor("KAAA"),std::size_t{0});
 CHECK(!snap["working"]["settings"]["host"]["airportAutoGenerate"].get<bool>());
 CHECK(!fscs::profiles::parseHostSettings(snap["working"]["settings"]).airportAutoGenerate);
 // On again: the temporary set appears; off again: it unloads.
 settings["host"]["airportAutoGenerate"]=true;m.panel("UpdateDraft",{{"ops",{{{"op","setSettings"},{"settings",settings}}}}},m.app.draftRevision());
 m.frame(130);CHECK(m.answer("KAAA",airportFixture()));CHECK_EQ(m.loaded(),std::string("KAAA"));
 settings["host"]["airportAutoGenerate"]=false;m.panel("UpdateDraft",{{"ops",{{{"op","setSettings"},{"settings",settings}}}}},m.app.draftRevision());
 m.frame(130);CHECK_EQ(m.loaded(),std::string(""));
}
