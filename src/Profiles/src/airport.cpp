#include "fscs/profiles/airport.hpp"
#include "fscs/profiles/json_util.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <set>
namespace fscs::profiles::airport {
namespace { constexpr double R=6378137.0;
// Stands with usable positions, at most `limit` (0 = all). Gates come first; the rest are chosen by
// farthest-point sampling so a small set still covers the whole apron. Deterministic, in source order.
std::vector<std::size_t> spreadParking(const json& parking,std::size_t limit) {
 std::vector<std::size_t> usable,gates,chosen;
 for(std::size_t i=0;i<parking.size();++i){
  if(!std::isfinite(js::num(parking[i],"north",NAN))||!std::isfinite(js::num(parking[i],"east",NAN)))continue;
  usable.push_back(i);if(js::boolean(parking[i],"gate",false))gates.push_back(i);
 }
 if(limit==0||usable.size()<=limit)return usable;
 auto at=[&](std::size_t i){return core::Vec3{js::num(parking[i],"north",0),js::num(parking[i],"east",0),0};};
 auto pick=[&](const std::vector<std::size_t>& pool){
  core::Vec3 centroid{0,0,0};for(auto i:pool)centroid=centroid+at(i)*(1.0/pool.size());
  while(chosen.size()<limit){
   double best=-1;std::size_t found=parking.size();
   for(auto i:pool){
    if(std::find(chosen.begin(),chosen.end(),i)!=chosen.end())continue;
    double d=std::numeric_limits<double>::infinity();
    for(auto c:chosen)d=std::min(d,(at(i)-at(c)).length());
    if(chosen.empty())d=(at(i)-centroid).length();
    if(d>best){best=d;found=i;}
   }
   if(found==parking.size())return;
   chosen.push_back(found);
  }
 };
 if(gates.size()>=limit)pick(gates);
 else {chosen=gates;pick(usable);}
 std::sort(chosen.begin(),chosen.end());
 return chosen;
}
}
bool Geo::valid() const { return std::isfinite(lat)&&std::isfinite(lon)&&std::isfinite(altitude)&&std::abs(lat)<89.9&&std::abs(lon)<=180&&altitude>-1500&&altitude<30000; }
Geo geo(const json& j) { return {js::num(j,"lat",NAN),js::num(j,"lon",NAN),js::num(j,"altitude",NAN)}; }
json asJson(Geo p) { return {{"lat",p.lat},{"lon",p.lon},{"altitude",p.altitude}}; }
core::Vec3 local(Geo a, Geo b) { return {(b.lat-a.lat)*core::kDegToRad*R,std::remainder(b.lon-a.lon,360.0)*core::kDegToRad*R*std::cos(a.lat*core::kDegToRad),a.altitude-b.altitude}; }
Geo world(Geo a, core::Vec3 v) { return {a.lat+v.x/R/core::kDegToRad,std::remainder(a.lon+v.y/(R*std::cos(a.lat*core::kDegToRad))/core::kDegToRad,360.0),a.altitude-v.z}; }
double distanceNm(Geo a, Geo b) { auto d=local(a,b); return std::hypot(d.x,d.y)/1852.0; }
bool validCode(const std::string& s) { return s.size()>=3&&s.size()<=8&&std::all_of(s.begin(),s.end(),[](char c){return (c>='A'&&c<='Z')||(c>='0'&&c<='9');}); }
std::string stableId(const std::string& key) { char s[37]; auto a=js::crc32(key), b=js::crc32("fscs-airport:"+key); std::snprintf(s,sizeof(s),"%08x-%04x-5%03x-a%03x-%08x%04x",a,b&65535,(b>>16)&4095,a&4095,b,(a>>16)&65535); return s; }
json parkingIdentity(int type, std::uint32_t number, int name, int suffix) {
 const bool gate=(type>=8&&type<=10)||type==15||name==10||(name>=12&&name<=37);
 const char* names[]={"","Parking","North parking","Northeast parking","East parking","Southeast parking","South parking","Southwest parking","West parking","Northwest parking","Gate","Dock"};
 std::string label;
 if(name>=12&&name<=37)label="Gate "+std::string(1,char('A'+name-12));
 else if(name>0&&name<12)label=std::string(names[name])+" ";
 else label=gate?"Gate ":type==11?"Dock ":type==12?"Fuel parking ":"Parking ";
 label+=std::to_string(number);
 if(suffix>=12&&suffix<=37)label+=char('A'+suffix-12);
 else if(suffix>0&&suffix<12)label+=" "+std::string(names[suffix]);
 auto key=std::to_string(name)+":"+std::to_string(number);
 if(suffix!=0)key+=":"+std::to_string(suffix); // keep IDs stable for unsuffixed saved stands
 return {{"name",label},{"key",key},{"gate",gate},{"type",type},{"number",number},{"nameCode",name},{"suffixCode",suffix}};
}
json generate(const json& f,const json& o,std::vector<std::string>& notes) {
 const auto code=js::str(f,"icao"); const auto origin=geo(f);
 if(!validCode(code)||!origin.valid()) {notes.push_back("Airport location is missing or invalid.");return nullptr;}
 auto doc=makeStarterProfile(code+" airport cameras","",""); auto seed=doc["cameras"][0];
 doc["profileId"]=stableId(code);doc["scope"]="airport";doc["airport"]={{"icao",code},{"name",js::str(f,"name",code)},{"origin",asJson(origin)}};
 doc["requiredSchemaFeatures"]={"airport-world-v1"}; doc["cameras"]=json::array();doc["categories"]=json::array();
 const double height=std::clamp(js::num(o,"height",3),0.0,1000.0), offset=std::clamp(js::num(o,"offset",12),3.0,150.0);
 bool towerPending=js::boolean(o,"tower",true);
 std::set<std::string> generatedIds;
 auto add=[&](std::string group,std::string key,std::string name,core::Vec3 pos,core::Vec3 target,double minimumMeters=-1) {
   // Tower is generated last, but must survive large airports reaching the limit.
   if(doc["cameras"].size()>=kMaxGeneratedCameras-(towerPending?1:0)) return;
   const std::string heightCategory=group=="Runways"||group=="Spotting"?"runways":group=="Gates"?"gates":group=="Apron"?"apron":group=="Tower"?"tower":"taxi";
   const double minimum=minimumMeters>=0?minimumMeters:key.find(":approach")!=std::string::npos?15:key.find(":overview")!=std::string::npos?25:0;
   const double baseAltitude=group=="Tower"?origin.altitude:origin.altitude-target.z;
   const bool explicitHeight=js::get(o,"height").is_number()&&!js::boolean(o,"useGlobalHeights",false);
   const double inherited=std::clamp(js::num(js::object(o,"categoryHeights"),heightCategory.c_str(),heightCategory=="tower"?35:3),0.0,1000.0);
   const double selectedHeight=std::max(minimum,explicitHeight?height:inherited);
   pos.z=origin.altitude-baseAltitude-selectedHeight;
   if(std::max({std::abs(pos.x),std::abs(pos.y),std::abs(pos.z)})>core::kMaxPositionMeters) return;
   const auto catId=stableId(code+":"+group), id=stableId(code+":"+key);
   if(!generatedIds.insert(id).second)return;
   const char* iconName=group=="Runways"||group=="Spotting"?"view-runway":group=="Gates"?"view-gate":group=="Apron"?"view-apron":group=="Tower"?"view-tower":"view-taxiway";
   json* cat=nullptr;for(auto& c:doc["categories"])if(js::str(c,"id")==catId)cat=&c;
   if(!cat){doc["categories"].push_back({{"id",catId},{"name",group},{"icon",iconName},{"airportIcao",code},{"cameraIds",json::array()}});cat=&doc["categories"].back();}
   auto delta=target-pos;auto q=core::Quat::fromPbh({-std::atan2(delta.z,std::hypot(delta.x,delta.y))/core::kDegToRad,0,std::atan2(delta.y,delta.x)/core::kDegToRad});
   auto cam=seed;cam.update({{"id",id},{"categoryId",catId},{"name",name.substr(0,60)},{"mode","airport"},{"aircraftTracking","both"},{"viewId",nullptr},{"favorite",true},{"interaction",false},{"requiredCapabilities",json::array()},
      {"airportIcao",code},{"worldOrigin",asJson(origin)},{"generatedKey",key},{"icon",iconName},{"positionMetersFRD",{pos.x,pos.y,pos.z}},{"orientationCameraToAnchorXYZW",{q.x,q.y,q.z,q.w}},
      {"transition",{{"kind","cut"},{"durationSeconds",0.6},{"delaySeconds",0}}}});
   cam["airportHeight"]={{"category",heightCategory},{"baseAltitudeMeters",baseAltitude},{"minimumMeters",minimum}};
   if(explicitHeight)cam["airportOverrides"]["heightMeters"]=selectedHeight;
   doc["cameras"].push_back(cam);(*cat)["cameraIds"].push_back(id);if(!cat->contains("defaultCameraId"))(*cat)["defaultCameraId"]=id;
 };
 if(js::boolean(o,"runways",true))for(const auto& r:js::array(f,"runways")) {
   auto center=local(origin,geo(r));if(!geo(r).valid())continue;double heading=js::num(r,"heading",0)*core::kDegToRad,length=js::num(r,"length",0),width=js::num(r,"width",30);
   if(length<50||length>10000)continue;core::Vec3 axis{std::cos(heading),std::sin(heading),0},right{-axis.y,axis.x,0};
   for(int side=0;side<2;++side){double sign=side?-1:1;auto end=center-axis*(sign*length/2);auto name=js::str(r,side?"secondary":"primary",side?"Reciprocal":"Runway");
     add("Runways","runway:"+name+":threshold",name+" threshold",end+right*(width/2+offset)+core::Vec3{0,0,-height},end+axis*(sign*120));
     add("Runways","runway:"+name+":approach",name+" approach",end-axis*(sign*150)+core::Vec3{0,0,-std::max(15.0,height)},end+axis*(sign*100));}
   add("Runways","runway:"+js::str(r,"primary")+":overview",js::str(r,"primary")+" runway overview",center+right*(width+offset)+core::Vec3{0,0,-std::max(25.0,height)},center);
 }
 // Spotting: defaults to the runway choice, so older callers that turned runways off get no new views.
 if(js::boolean(o,"spotting",js::boolean(o,"runways",true)))for(const auto& r:js::array(f,"runways")) {
   const auto g=geo(r);const double length=js::num(r,"length",0),heading=js::num(r,"heading",NAN)*core::kDegToRad;
   if(!g.valid()||length<50||length>10000||!std::isfinite(heading))continue;
   const auto center=local(origin,g);const core::Vec3 axis{std::cos(heading),std::sin(heading),0};
   // A spotter's eye about 90 m below an aircraft on a 3-degree glidepath, 1 NM out.
   const double finalHeight=std::max(0.0,kApproachDistanceMeters*std::tan(kGlidepathDeg*core::kDegToRad)-kApproachBelowGlidepathMeters);
   for(int side=0;side<2;++side){
     const double sign=side?-1:1;const auto landing=axis*sign,threshold=center-landing*(length/2);const core::Vec3 right{-landing.y,landing.x,0};
     const auto name=js::str(r,side?"secondary":"primary",side?"Reciprocal":"Runway");
     // Fence line: behind the threshold and off to the side, looking down the runway.
     add("Spotting","spot:"+name+":fence",name+" fence line",threshold-landing*kFenceBehindMeters+right*kFenceSideMeters,center);
     // Under the approach: on the extended centerline, looking back at the threshold.
     add("Spotting","spot:"+name+":final",name+" approach, 1 NM",threshold-landing*kApproachDistanceMeters,threshold,finalHeight);
   }
 }
 if(js::boolean(o,"parking",true)){const auto& stands=js::array(f,"parking");for(const auto index:spreadParking(stands,static_cast<std::size_t>(std::clamp<std::int64_t>(js::integer(o,"parkingLimit",0),0,static_cast<std::int64_t>(kMaxGeneratedCameras))))) {
   const auto& p=stands[index];const double n=js::num(p,"north",NAN),e=js::num(p,"east",NAN);
   const double h=js::num(p,"heading",0)*core::kDegToRad;core::Vec3 target{n,e,0};auto name=js::str(p,"name","Parking");
   add(js::boolean(p,"gate",false)?"Gates":"Apron","parking:"+js::str(p,"key",name),name,target+core::Vec3{std::cos(h)*offset,std::sin(h)*offset,-height},target);
 }}
 const auto& signs=js::array(f,"signs");
 if(js::boolean(o,"taxi",true)&&!signs.empty())for(const auto& s:signs){auto g=geo(s);if(!g.valid())continue;auto target=local(origin,g);double h=(js::num(s,"heading",0)+90)*core::kDegToRad;
   add("Taxi signs","sign:"+js::str(s,"key"),js::str(s,"label","Taxi sign"),target+core::Vec3{std::cos(h)*offset,std::sin(h)*offset,-height},target);}
 else if(js::boolean(o,"taxi",true)) {
   notes.push_back("No taxi-sign positions supplied; using offset taxiway intersections.");
   std::map<int,std::set<int>> edges;std::map<int,double> widths;
   for(const auto& path:js::array(f,"paths")){if(js::integer(path,"type",0)!=1)continue;int a=(int)js::integer(path,"start",-1),b=(int)js::integer(path,"end",-1);if(a<0||b<0||a==b)continue;edges[a].insert(b);edges[b].insert(a);double width=js::num(path,"width",20);widths[a]=std::max(widths[a],width);widths[b]=std::max(widths[b],width);}
   const auto& points=js::array(f,"points");std::vector<core::Vec3> used;
   for(const auto& [index, neighbors]:edges){if(neighbors.size()<3||index>=(int)points.size())continue;
     core::Vec3 target{js::num(points[index],"north",NAN),js::num(points[index],"east",NAN),0};if(!std::isfinite(target.x)||!std::isfinite(target.y))continue;
     bool duplicate=false;for(auto v:used)if((v-target).length()<25)duplicate=true;if(duplicate)continue;used.push_back(target);
     auto pos=target+core::Vec3{widths[index]/2+offset,widths[index]/2+offset,-height};
     add("Taxiway intersections","junction:"+std::to_string(index),"Taxiway junction "+std::to_string(index+1),pos,target);
   }
 }
 if(towerPending){
   towerPending=false;
   const auto t=geo(js::get(f,"tower"));auto pos=local(origin,t);pos.z-=height;
   const bool usable=t.valid()&&(t.lat!=0||t.lon!=0)&&std::max({std::abs(pos.x),std::abs(pos.y),std::abs(pos.z)})<=core::kMaxPositionMeters;
   // tower.viewHeightMeters (X-Plane's tower viewpoint height above the field) keeps the camera out of the tower
   // building itself: no lower than the real cab. MSFS facilities do not carry it.
   if(usable)add("Tower","tower","Tower overview",pos,{0,0,0},js::num(js::get(f,"tower"),"viewHeightMeters",-1));
   else {
     // Some scenery omits tower coordinates (or reports zero). Place a clearly labelled virtual
     // tower abeam the longest runway's midpoint, on the side with more parking (left when even
     // or unknown). Its height is the tower category height above airport elevation.
     core::Vec3 target{0,0,0};pos={100,100,0};
     double longest=0;
     for(const auto& r:js::array(f,"runways")){
       const auto g=geo(r);const double length=js::num(r,"length",0),heading=js::num(r,"heading",NAN)*core::kDegToRad;
       if(!g.valid()||length<50||length>10000||length<=longest||!std::isfinite(heading))continue;
       const auto center=local(origin,g);const core::Vec3 right{-std::sin(heading),std::cos(heading),0};
       int rightStands=0,leftStands=0;
       for(const auto& p:js::array(f,"parking")){
         const double n=js::num(p,"north",NAN),e=js::num(p,"east",NAN);if(!std::isfinite(n)||!std::isfinite(e))continue;
         const double across=(n-center.x)*right.x+(e-center.y)*right.y;
         if(across>0)++rightStands;else if(across<0)++leftStands;
       }
       const auto candidate=center+right*(rightStands>leftStands?kVirtualTowerAbeamMeters:-kVirtualTowerAbeamMeters);
       if(std::max({std::abs(candidate.x),std::abs(candidate.y),std::abs(candidate.z)})>core::kMaxPositionMeters)continue;
       longest=length;target=center;pos=candidate;
     }
     add("Tower","tower","Virtual tower",pos,target);
     notes.push_back("No tower position in scenery data; added a virtual tower beside the longest runway. It is not the real control tower.");
   }
 }
 if(doc["cameras"].empty()){notes.push_back("No usable selected scenery features.");return nullptr;}
 doc["defaultCameraId"]=doc["cameras"][0]["id"];
 if(doc["cameras"].size()==kMaxGeneratedCameras)notes.push_back("Limited to 500 cameras; select fewer feature groups for larger airports.");
 auto validation=validateProfile(doc);if(!validation.ok()){notes.push_back(validation.summary());return nullptr;}return doc;
}
json merge(const json& saved,const json& generated,bool replace) {
 if(!saved.is_object())return generated;auto out=saved;
 if(replace) {
  // Explicit regeneration removes old generated views that are no longer in
  // the selected scenery features, while keeping manually created views.
  auto& cameras=out["cameras"];
  cameras.erase(std::remove_if(cameras.begin(),cameras.end(),[&](const json& cam){
   if(js::str(cam,"generatedKey").empty())return false;
   for(const auto& fresh:generated["cameras"])if(fresh["id"]==cam["id"])return false;
   return true;
  }),cameras.end());
 }
 for(const auto& cat:generated["categories"]){bool found=false;for(const auto& c:out["categories"])found=found||c["id"]==cat["id"];if(!found){auto copy=cat;copy["cameraIds"]=json::array();out["categories"].push_back(copy);}}
 for(const auto& cam:generated["cameras"]){bool found=false;for(auto& c:out["cameras"])if(c["id"]==cam["id"]){found=true;if(replace){auto viewId=js::get(c,"viewId");c=cam;if(viewId.is_number_integer())c["viewId"]=viewId;}}if(!found)out["cameras"].push_back(cam);}
 for(auto& cat:out["categories"]){cat["cameraIds"]=json::array();for(const auto& c:out["cameras"])if(c["categoryId"]==cat["id"])cat["cameraIds"].push_back(c["id"]);
  bool defaultExists=false;for(const auto& id:cat["cameraIds"])defaultExists=defaultExists||id==js::get(cat,"defaultCameraId");
  if(!defaultExists){if(cat["cameraIds"].empty())cat.erase("defaultCameraId");else cat["defaultCameraId"]=cat["cameraIds"][0];}
 }
 if(replace)out["categories"].erase(std::remove_if(out["categories"].begin(),out["categories"].end(),[](const json& cat){return cat["cameraIds"].empty();}),out["categories"].end());
 bool defaultExists=false;for(const auto& cam:out["cameras"])defaultExists=defaultExists||cam["id"]==out["defaultCameraId"];
 if(!defaultExists&&!out["cameras"].empty())out["defaultCameraId"]=out["cameras"][0]["id"];
 return out;
}
json nearby(const std::vector<json>& profiles,Geo pos,double radius){json result=json::array();if(!pos.valid())return result;
 for(const auto& p:profiles){auto a=js::object(p,"airport");auto loc=geo(js::get(a,"origin"));if(!loc.valid())continue;double d=distanceNm(pos,loc);if(d<=radius)result.push_back({{"icao",js::str(a,"icao")},{"name",js::str(a,"name")},{"profileId",js::str(p,"profileId")},{"distanceNm",d}});}
 std::sort(result.begin(),result.end(),[](const json& a,const json& b){if(a["distanceNm"]==b["distanceNm"])return a["icao"]<b["icao"];return a["distanceNm"]<b["distanceNm"];});return result;}
std::string choose(const json& list,const std::string& current,const std::string& pin){if(list.empty())return {};for(auto& c:list)if(js::str(c,"icao")==pin)return pin;
 const auto nearest=js::str(list[0],"icao");for(auto& c:list)if(js::str(c,"icao")==current&&js::num(c,"distanceNm",0)<=js::num(list[0],"distanceNm",0)+2.0)return current;return nearest;}
json automaticOptions(const json& categoryHeights) {
 return {{"runways",true},{"spotting",true},{"parking",true},{"parkingLimit",kAutoParkingLimit},{"taxi",false},{"tower",true},
         {"categoryHeights",categoryHeights.is_object()?categoryHeights:json::object()}};
}
json compactFacility(const json& f) {
 json out=json::object();
 for(const char* key:{"icao","name","lat","lon","altitude","tower","runways"})if(js::has(f,key))out[key]=f[key];
 // Large airports list thousands of stands: keep only what generation reads.
 out["parking"]=json::array();
 for(const auto& p:js::array(f,"parking")){json stand=json::object();for(const char* key:{"name","key","gate","north","east","heading"})if(js::has(p,key))stand[key]=p[key];out["parking"].push_back(std::move(stand));}
 return out;
}
}
