#include "fscs/profiles/airport.hpp"
#include "fscs/profiles/json_util.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <set>
namespace fscs::profiles::airport {
namespace { constexpr double R=6378137.0; }
bool Geo::valid() const { return std::isfinite(lat)&&std::isfinite(lon)&&std::isfinite(altitude)&&std::abs(lat)<89.9&&std::abs(lon)<=180&&altitude>-1500&&altitude<30000; }
Geo geo(const json& j) { return {js::num(j,"lat",NAN),js::num(j,"lon",NAN),js::num(j,"altitude",NAN)}; }
json asJson(Geo p) { return {{"lat",p.lat},{"lon",p.lon},{"altitude",p.altitude}}; }
core::Vec3 local(Geo a, Geo b) { return {(b.lat-a.lat)*core::kDegToRad*R,std::remainder(b.lon-a.lon,360.0)*core::kDegToRad*R*std::cos(a.lat*core::kDegToRad),a.altitude-b.altitude}; }
Geo world(Geo a, core::Vec3 v) { return {a.lat+v.x/R/core::kDegToRad,std::remainder(a.lon+v.y/(R*std::cos(a.lat*core::kDegToRad))/core::kDegToRad,360.0),a.altitude-v.z}; }
double distanceNm(Geo a, Geo b) { auto d=local(a,b); return std::hypot(d.x,d.y)/1852.0; }
bool validCode(const std::string& s) { return s.size()>=3&&s.size()<=8&&std::all_of(s.begin(),s.end(),[](char c){return (c>='A'&&c<='Z')||(c>='0'&&c<='9');}); }
std::string stableId(const std::string& key) { char s[37]; auto a=js::crc32(key), b=js::crc32("fscs-airport:"+key); std::snprintf(s,sizeof(s),"%08x-%04x-5%03x-a%03x-%08x%04x",a,b&65535,(b>>16)&4095,a&4095,b,(a>>16)&65535); return s; }
json generate(const json& f,const json& o,std::vector<std::string>& notes) {
 const auto code=js::str(f,"icao"); const auto origin=geo(f);
 if(!validCode(code)||!origin.valid()) {notes.push_back("Airport location is missing or invalid.");return nullptr;}
 auto doc=makeStarterProfile(code+" airport cameras","",""); auto seed=doc["cameras"][0];
 doc["profileId"]=stableId(code);doc["scope"]="airport";doc["airport"]={{"icao",code},{"name",js::str(f,"name",code)},{"origin",asJson(origin)}};
 doc["requiredSchemaFeatures"]={"airport-world-v1"}; doc["cameras"]=json::array();doc["categories"]=json::array();
 const double height=std::clamp(js::num(o,"height",3),1.0,100.0), offset=std::clamp(js::num(o,"offset",12),3.0,150.0);
 std::set<std::string> generatedIds;
 auto add=[&](std::string group,std::string key,std::string name,core::Vec3 pos,core::Vec3 target) {
   if(doc["cameras"].size()>=kMaxCameras) return;
   if(std::max({std::abs(pos.x),std::abs(pos.y),std::abs(pos.z)})>core::kMaxPositionMeters) return;
   const auto catId=stableId(code+":"+group), id=stableId(code+":"+key);
   if(!generatedIds.insert(id).second)return;
   json* cat=nullptr;for(auto& c:doc["categories"])if(js::str(c,"id")==catId)cat=&c;
   if(!cat){doc["categories"].push_back({{"id",catId},{"name",group},{"icon","map-pin"},{"airportIcao",code},{"cameraIds",json::array()}});cat=&doc["categories"].back();}
   auto delta=target-pos;auto q=core::Quat::fromPbh({-std::atan2(delta.z,std::hypot(delta.x,delta.y))/core::kDegToRad,0,std::atan2(delta.y,delta.x)/core::kDegToRad});
   auto cam=seed;cam.update({{"id",id},{"categoryId",catId},{"name",name.substr(0,60)},{"mode","airport"},{"viewId",nullptr},{"favorite",false},{"interaction",false},{"requiredCapabilities",json::array()},
      {"airportIcao",code},{"worldOrigin",asJson(origin)},{"generatedKey",key},{"icon","camera"},{"positionMetersFRD",{pos.x,pos.y,pos.z}},{"orientationCameraToAnchorXYZW",{q.x,q.y,q.z,q.w}},
      {"transition",{{"kind","cut"},{"durationSeconds",0.6},{"delaySeconds",0}}}});
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
 if(js::boolean(o,"parking",true))for(const auto& p:js::array(f,"parking")) {
   const double n=js::num(p,"north",NAN),e=js::num(p,"east",NAN);if(!std::isfinite(n)||!std::isfinite(e))continue;
   const double h=js::num(p,"heading",0)*core::kDegToRad;core::Vec3 target{n,e,0};auto name=js::str(p,"name","Parking");
   add(js::boolean(p,"gate",false)?"Gates":"Apron","parking:"+js::str(p,"key",name),name,target+core::Vec3{std::cos(h)*offset,std::sin(h)*offset,-height},target);
 }
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
 if(js::boolean(o,"tower",true)){auto t=geo(js::get(f,"tower"));if(t.valid()&&(t.lat!=0||t.lon!=0)){auto p=local(origin,t);p.z-=height;add("Tower","tower","Tower overview",p,{0,0,0});}else notes.push_back("No tower position in scenery data.");}
 if(doc["cameras"].empty()){notes.push_back("No usable selected scenery features.");return nullptr;}
 doc["defaultCameraId"]=doc["cameras"][0]["id"];
 if(doc["cameras"].size()==kMaxCameras)notes.push_back("Limited to 500 cameras; select fewer feature groups for larger airports.");
 auto validation=validateProfile(doc);if(!validation.ok()){notes.push_back(validation.summary());return nullptr;}return doc;
}
json merge(const json& saved,const json& generated,bool replace) {
 if(!saved.is_object())return generated;auto out=saved;
 for(const auto& cat:generated["categories"]){bool found=false;for(const auto& c:out["categories"])found=found||c["id"]==cat["id"];if(!found){auto copy=cat;copy["cameraIds"]=json::array();out["categories"].push_back(copy);}}
 for(const auto& cam:generated["cameras"]){bool found=false;for(auto& c:out["cameras"])if(c["id"]==cam["id"]){found=true;if(replace)c=cam;}if(!found)out["cameras"].push_back(cam);}
 for(auto& cat:out["categories"]){cat["cameraIds"]=json::array();for(const auto& c:out["cameras"])if(c["categoryId"]==cat["id"])cat["cameraIds"].push_back(c["id"]);}
 return out;
}
json nearby(const std::vector<json>& profiles,Geo pos,double radius){json result=json::array();if(!pos.valid())return result;
 for(const auto& p:profiles){auto a=js::object(p,"airport");auto loc=geo(js::get(a,"origin"));if(!loc.valid())continue;double d=distanceNm(pos,loc);if(d<=radius)result.push_back({{"icao",js::str(a,"icao")},{"name",js::str(a,"name")},{"profileId",js::str(p,"profileId")},{"distanceNm",d}});}
 std::sort(result.begin(),result.end(),[](const json& a,const json& b){if(a["distanceNm"]==b["distanceNm"])return a["icao"]<b["icao"];return a["distanceNm"]<b["distanceNm"];});return result;}
std::string choose(const json& list,const std::string& current,const std::string& pin){if(list.empty())return {};for(auto& c:list)if(js::str(c,"icao")==pin)return pin;
 const auto nearest=js::str(list[0],"icao");for(auto& c:list)if(js::str(c,"icao")==current&&js::num(c,"distanceNm",0)<=js::num(list[0],"distanceNm",0)+2.0)return current;return nearest;}
}
