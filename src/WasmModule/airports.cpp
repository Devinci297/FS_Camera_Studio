#include "module_app.hpp"
#include "fscs/profiles/json_util.hpp"
#include <algorithm>
#include <limits>
namespace fscs::host {
using protocol::AckStatus;
namespace { json noRevision(json p){if(p.is_object())p.erase("revision");return p;} }
bool ModuleApp::airportDirty() const {for(auto& [code,p]:airportDrafts_)if(noRevision(p)!=noRevision(committedProfile(js::str(p,"profileId"))))return true;return false;}
json ModuleApp::combinedProfile() const {
 auto out=working_.profile;
 auto it=airportDrafts_.find(loadedAirport_);if(it==airportDrafts_.end())return out;
 if(!out.is_object()) {
  out=it->second;
  for(const char* field:{"categories","cameras"})for(auto& item:out[field])item["airportIcao"]=loadedAirport_;
  return out;
 }
 for(auto cat:js::array(it->second,"categories")){cat["airportIcao"]=loadedAirport_;out["categories"].push_back(cat);}
 for(auto cam:js::array(it->second,"cameras")){cam["airportIcao"]=loadedAirport_;out["cameras"].push_back(cam);}
 return out;
}
void ModuleApp::splitCombined(json doc) {
 auto it=airportDrafts_.find(loadedAirport_);
 if(it!=airportDrafts_.end()){
   auto& airport=it->second;airport["categories"]=json::array();airport["cameras"]=json::array();
   for(const char* field:{"categories","cameras"}){json aircraft=json::array();for(auto item:js::array(doc,field)){if(js::str(item,"airportIcao")==loadedAirport_)airport[field].push_back(item);else aircraft.push_back(item);}doc[field]=aircraft;}
   if(!airport["cameras"].empty()){bool found=false;for(auto& c:airport["cameras"])found=found||c["id"]==airport["defaultCameraId"];if(!found)airport["defaultCameraId"]=airport["cameras"][0]["id"];}
 }
 if(working_.profile.is_object())working_.profile=std::move(doc);
}
bool ModuleApp::validateCombined(json& doc,std::string& error) const {
 auto aircraft=working_.profile.is_object()?doc:json(nullptr);auto it=airportDrafts_.find(loadedAirport_);
 json airport=it==airportDrafts_.end()?json(nullptr):it->second;
 if(airport.is_object()) {
  for(const char* field:{"categories","cameras"}) {
   if(aircraft.is_object())aircraft[field]=json::array();airport[field]=json::array();
   for(auto item:js::array(doc,field)) {
    auto code=js::str(item,"airportIcao");
    if(!code.empty()&&code!=loadedAirport_){error="This item belongs to another airport.";return false;}
    if(code.empty()&&!aircraft.is_object()){error="Create an aircraft profile before adding aircraft cameras.";return false;}
    (code.empty()?aircraft:airport)[field].push_back(item);
   }
  }
  if(!airport["cameras"].empty()) {
   bool found=false;for(auto& c:airport["cameras"])found=found||c["id"]==airport["defaultCameraId"];
   if(!found)airport["defaultCameraId"]=airport["cameras"][0]["id"];
  }
 }
 for(auto* part:{&aircraft,&airport})if(part->is_object()) {
  auto v=profiles::validateProfile(*part);
  if(!v.ok()||v.readOnly){error="That change was not applied: "+(v.readOnly?std::string("profile requires a newer version"):v.summary());return false;}
 }
 // Each saved domain owns its category references. Cross-domain moves cannot be
 // represented in either file, and are rejected by the separate validations above.
 doc=aircraft.is_object()?aircraft:airport;
 if(aircraft.is_object()&&airport.is_object())for(const char* field:{"categories","cameras"})for(auto& item:airport[field])doc[field].push_back(item);
 return true;
}
json ModuleApp::airportState() const {return {{"scope","global"},{"autoSwitch",airportAuto_},{"loaded",loadedAirport_},{"pinned",pinnedAirport_},{"nearby",nearbyAirports_},{"facilities",nearbyFacilities_},{"radiusNm",js::num(js::object(working_.settings,"host"),"airportRadiusNm",25)},{"positionKnown",position_.valid()},{"worldStatus","Preview — world coordinates need in-sim qualification"}};}
bool ModuleApp::setAirportAuto(bool enabled,double now) {
 if(airportAuto_==enabled)return true;
 std::string nearest;
 if(enabled) {
  if(!simRunning_||paused_||!position_.valid()||now-positionAt_>2||!coord_.canActivate())return false;
  const auto* p=coord_.profile();double best=std::numeric_limits<double>::infinity();
  if(p)for(const auto& cam:p->cameras)if(cam.mode==profiles::CameraMode::Airport) {
   const auto world=profiles::airport::world({cam.originLat,cam.originLon,cam.originAltitude},cam.pose.positionFRD);
   const double d=profiles::airport::local(position_,world).length();
   if(d<best){best=d;nearest=cam.id;}
  }
  if(nearest.empty()||!coord_.selectAutomaticCamera(nearest,now,true))return false;
 }
 airportAuto_=enabled;autoCameraId_=nearest;lastAutoSwitch_=now;lastAutoCheck_=now;
 emitState("AirportsChanged",airportState());return true;
}
void ModuleApp::updateAirportAuto(double now) {
 if(!airportAuto_)return;
 // Auto switch is a session-wide mode. Ownership changes only pause it.
 if(coord_.ownership()!=Ownership::Active)return;
 const auto* p=coord_.profile();const auto* cam=p?p->camera(coord_.activeCameraId()):nullptr;
 if(!cam)return;
 // A manual camera/category selection gets a full dwell, without changing the shared toggle.
 if(cam->id!=autoCameraId_){autoCameraId_=cam->id;lastAutoSwitch_=now;return;}
 if(autoMouseDown_){lastAutoSwitch_=now;return;}
 if(!simRunning_||paused_||coord_.gameControlled()||!position_.valid()||now-positionAt_>2||now-lastAutoCheck_<.5||now-lastAutoSwitch_<3)return;
 for(const auto& [id,inst]:instances_)if(!inst.lapsed&&inst.visible&&inst.pointerOver)return;
 lastAutoCheck_=now;
 auto distance=[&](const profiles::CameraEntry& c){return profiles::airport::local(position_,profiles::airport::world({c.originLat,c.originLon,c.originAltitude},c.pose.positionFRD)).length();};
 const bool currentAirport=cam->mode==profiles::CameraMode::Airport;
 const double current=currentAirport?distance(*cam):std::numeric_limits<double>::infinity();
 double best=current;std::string nearest=currentAirport?cam->id:std::string();
 for(const auto& other:p->cameras)if(other.mode==profiles::CameraMode::Airport){const double d=distance(other);if(d<best){best=d;nearest=other.id;}}
 // A margin and minimum dwell prevent rapid switching at camera boundaries.
 if(!nearest.empty() && nearest!=cam->id && (!currentAirport || best+std::max(10.0,current*.15)<current)) {
  if(coord_.selectAutomaticCamera(nearest,now)){autoCameraId_=nearest;lastAutoSwitch_=now;}
 }
}
void ModuleApp::refreshAirports(double now) {
 std::vector<json> saved;
 for(const auto& [id,rec]:repo_.committed().profiles)if(js::str(rec.current,"scope")=="airport"){
  auto code=js::str(js::object(rec.current,"airport"),"icao");saved.push_back(rec.current);if(!airportDrafts_.count(code))airportDrafts_[code]=rec.current;
 }
 auto list=profiles::airport::nearby(saved,position_,js::num(js::object(working_.settings,"host"),"airportRadiusNm",25));
 auto chosen=profiles::airport::choose(list,loadedAirport_,pinnedAirport_);
 bool changed=chosen!=loadedAirport_;nearbyAirports_=list;
 if(changed){loadedAirport_=chosen;recomputeDirty();publishWorking(now,false);if(!coord_.profile()||!coord_.profile()->camera(selection_))selectCamera(selectionFallback());working_.draftRevision++;broadcastSnapshot();}
 else emitState("AirportsChanged",airportState());
}
bool ModuleApp::airportEvent(const BackendEvent& ev,double now){
 if(auto e=std::get_if<backend_event::AirportIdentity>(&ev)){
  // Only the scenery discovery list changes. Saved profile keys stay untouched.
  for(auto& a:nearbyFacilities_)if(js::str(a,"icao")==e->ident && js::str(a,"region")==e->region && profiles::airport::validCode(e->icao)
      && profiles::airport::distanceNm(profiles::airport::geo(a),{e->lat,e->lon,0})<0.1){
   a["facilityIdent"]=e->ident;a["icao"]=e->icao;
  }
  emitNow("AirportsChanged",airportState());return true;
 }
 if(auto e=std::get_if<backend_event::AircraftPosition>(&ev)){position_={e->lat,e->lon,e->altitude};positionAt_=now;if(position_.valid())coord_.setAircraftPosition(e->lat,e->lon,e->altitude,now,e->headingDeg);return true;}
 if(auto e=std::get_if<backend_event::NativeMouseButton>(&ev)){autoMouseDown_=e->down;if(airportAuto_)lastAutoSwitch_=now;}
 if(auto e=std::get_if<backend_event::AirportList>(&ev)){for(const auto& a:e->airports){bool found=false;for(auto& old:nearbyFacilities_)if(js::str(old,"icao")==js::str(a,"icao")){old=a;found=true;}if(!found&&nearbyFacilities_.size()<1000)nearbyFacilities_.push_back(a);}
  if(e->complete){std::sort(nearbyFacilities_.begin(),nearbyFacilities_.end(),[this](const json&a,const json&b){return profiles::airport::distanceNm(position_,profiles::airport::geo(a))<profiles::airport::distanceNm(position_,profiles::airport::geo(b));});services_.resolveAirportIdentities(nearbyFacilities_);emitNow("AirportsChanged",airportState());}return true;}
 if(auto e=std::get_if<backend_event::AirportFacility>(&ev)){
  auto it=airportJobs_.find(e->requestId);if(it==airportJobs_.end())return true;auto job=it->second;airportJobs_.erase(it);
  std::vector<std::string> notes;auto doc=e->error.empty()?profiles::airport::generate(e->facility,job.options,notes):json(nullptr);
  if(doc.is_object())airportPreviews_[job.icao]=doc;
  emitNow("AirportPreview",{{"icao",job.icao},{"profile",doc},{"notes",notes},{"error",e->error}},job.to);return true;
 }
 if(std::holds_alternative<backend_event::FlightLoaded>(ev)||std::holds_alternative<backend_event::Disconnected>(ev)){autoMouseDown_=false;position_={NAN,NAN,NAN};nearbyFacilities_=json::array();pinnedAirport_.clear();refreshAirports(now);}
 return false;
}
bool ModuleApp::handleAirport(const protocol::Command& c,double now){
 const auto& a=c.action;const auto& p=c.payload;
 // Resolve defaults on the module side too, so previews from either UI or imported
 // scenery sources cannot silently fall back to a stale client-side height.
 auto generationOptions=[&](){auto options=js::object(p,"options");
  options["categoryHeights"]=js::object(js::object(working_.settings,"host"),"airportCategoryHeights");
  return options;
 };
 if(a=="SetAirportAuto") {const bool ok=setAirportAuto(js::boolean(p,"enabled",false),now);reply(c,ok?AckStatus::Completed:AckStatus::Rejected,ok?"":"Load or generate nearby airport cameras and resume the flight before enabling Auto switch.");return true;}
 if(a=="DiscoverAirports"){nearbyFacilities_=json::array();bool ok=position_.valid()&&services_.requestNearbyAirports();reply(c,ok?AckStatus::Accepted:AckStatus::Rejected,ok?"":"Load a flight before finding nearby airports.");return true;}
 if(a=="SelectAirport"){
  auto code=js::str(p,"icao");bool valid=code.empty();for(auto& n:nearbyAirports_)valid=valid||js::str(n,"icao")==code;
  if(!valid){reply(c,AckStatus::Rejected,"That airport is outside the nearby radius.");return true;}
  pinnedAirport_=code;if(!code.empty())loadedAirport_.clear();refreshAirports(now);broadcastSnapshot();reply(c,AckStatus::Completed);return true;
 }
 if(a=="PreviewAirports"){
  const auto& codes=js::array(p,"codes");if(codes.empty()||codes.size()>10||airportJobs_.size()+codes.size()>10){reply(c,AckStatus::Rejected,"Enter between one and ten airport codes.");return true;}
  reply(c,AckStatus::Accepted);
  for(const auto& v:codes){auto code=js::str(v);if(!profiles::airport::validCode(code)){emitNow("AirportPreview",{{"icao",code},{"error","Invalid airport code."}},c.uiInstanceId);continue;}
   auto id=++airportRequestId_;if(!services_.requestAirport(code,id)){emitNow("AirportPreview",{{"icao",code},{"error","Airport data request failed."}},c.uiInstanceId);continue;}
   airportJobs_[id]={code,c.uiInstanceId,generationOptions(),now+20};
  }return true;
 }
 if(a=="PreviewAirportSource"){
  std::vector<std::string> notes;auto doc=profiles::airport::generate(js::get(p,"facility"),generationOptions(),notes);
  if(!doc.is_object()){reply(c,AckStatus::Rejected,notes.empty()?"No airport data.":notes.front());return true;}
  auto code=js::str(js::object(doc,"airport"),"icao");airportPreviews_[code]=doc;reply(c,AckStatus::Completed);emitNow("AirportPreview",{{"icao",code},{"profile",doc},{"notes",notes}},c.uiInstanceId);return true;
 }
 if(a=="SaveAirportPreview"){
  const auto code=js::str(p,"icao");auto it=airportPreviews_.find(code);if(it==airportPreviews_.end()){reply(c,AckStatus::Rejected,"Generate a preview first.");return true;}
  const bool dirtyAirport=airportDrafts_.count(code)&&noRevision(airportDrafts_[code])!=noRevision(committedProfile(js::str(airportDrafts_[code],"profileId")));
  const bool replace=js::boolean(p,"replace",false);
  if(dirtyAirport&&!(replace&&js::boolean(p,"discardAirportEdits",false))){reply(c,AckStatus::Rejected,"This airport has unsaved edits. Save them, or confirm Replace generated cameras and discard this airport's edits.");return true;}
  auto doc=it->second;const auto& ids=js::array(p,"cameraIds");
  doc["cameras"].erase(std::remove_if(doc["cameras"].begin(),doc["cameras"].end(),[&](const json& cam){return std::find(ids.begin(),ids.end(),cam["id"])==ids.end();}),doc["cameras"].end());
  if(doc["cameras"].empty()){reply(c,AckStatus::Rejected,"Select at least one camera.");return true;}
  for(auto& cat:doc["categories"]){cat["cameraIds"]=json::array();for(auto& cam:doc["cameras"])if(cam["categoryId"]==cat["id"])cat["cameraIds"].push_back(cam["id"]);if(!cat["cameraIds"].empty())cat["defaultCameraId"]=cat["cameraIds"][0];else cat.erase("defaultCameraId");}
  doc["categories"].erase(std::remove_if(doc["categories"].begin(),doc["categories"].end(),[](const json& cat){return cat["cameraIds"].empty();}),doc["categories"].end());doc["defaultCameraId"]=doc["cameras"][0]["id"];
  doc=profiles::airport::merge(committedProfile(js::str(doc,"profileId")),doc,replace);auto validation=profiles::validateProfile(doc);
  if(!validation.ok()){reply(c,AckStatus::Rejected,validation.summary());return true;}
  profiles::CommitRequest req;req.upsertProfiles.push_back(doc);req.requestId=c.uiInstanceId+":"+c.requestId;reply(c,AckStatus::Pending);
  commit(std::move(req),{c.requestId,"airport",working_.draftRevision,{{"icao",code},{"discardedAirportEdits",dirtyAirport}},{}},now);return true;
 }
 return false;
}
}
