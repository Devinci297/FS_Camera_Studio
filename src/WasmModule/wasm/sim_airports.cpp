#include "sim_session.hpp"
#include "fscs/profiles/airport.hpp"
#include "fscs/profiles/json_util.hpp"
#include <MSFS/MSFS.h>
#include <MSFS/MSFS_WindowsTypes.h>
#include <SimConnect.h>
#include <cstring>
#include <cstdio>
namespace fscs::host {
using nlohmann::json;
namespace {
constexpr DWORD kAirportDef=70,kAirportIdentityDef=71,kNearby=80;
#pragma pack(push,1)
struct AirportData {double lat,lon,alt;char name[64];double towerLat,towerLon,towerAlt;};
struct AirportIdentityData {char icao[8];double lat,lon;};
struct RunwayData {double lat,lon,alt;float heading,length,width;int primary,primaryDesignator,secondary,secondaryDesignator;};
struct ParkingData {int type,number,name;float heading,radius,east,north;};
struct PointData {float east,north;};
struct PathData {int type,start,end;float width;};
#pragma pack(pop)
std::string runwayName(int n,int d){char buf[16];const char* suffix[]={"","L","R","C","W","A","B"};std::snprintf(buf,sizeof(buf),"%02d%s",n,d>=0&&d<7?suffix[d]:"");return buf;}
}
bool SimSession::requestNearbyAirports(){return handle_&&SUCCEEDED(SimConnect_RequestFacilitiesList_EX1(static_cast<HANDLE>(handle_),SIMCONNECT_FACILITY_LIST_TYPE_AIRPORT,kNearby));}
void SimSession::resolveAirportIdentities(const json& airports){
 // Facility lists can expose a short ident. Ask scenery for its ICAO rather than
 // guessing a country prefix. Only resolve the eight displayed nearby choices.
 airportIdentityRequests_.clear();if(!handle_)return;
 const auto h=static_cast<HANDLE>(handle_);
 if(!airportIdentityDefinitionReady_){
  for(const auto* field:{"OPEN AIRPORT","ICAO","LATITUDE","LONGITUDE","CLOSE AIRPORT"})
   if(FAILED(SimConnect_AddToFacilityDefinition(h,kAirportIdentityDef,field)))return;
  airportIdentityDefinitionReady_=true;
 }
 for(const auto& a:airports){
  if(airportIdentityRequests_.size()>=8)break;
  auto ident=js::str(a,"icao"),region=js::str(a,"region");
  if(!profiles::airport::validCode(ident))continue;
  auto id=++airportIdentityRequest_;
  if(SUCCEEDED(SimConnect_RequestFacilityData(h,kAirportIdentityDef,id,ident.c_str(),region.c_str())))airportIdentityRequests_[id]=a;
 }
}
bool SimSession::requestAirport(const std::string& code,std::uint32_t id){
 if(!handle_||!profiles::airport::validCode(code))return false;
 if(airportRequests_.size()>=10)airportRequests_.erase(airportRequests_.begin());
 auto h=static_cast<HANDLE>(handle_);
 if(!airportDefinitionReady_){
  const char* fields[]={"OPEN AIRPORT","LATITUDE","LONGITUDE","ALTITUDE","NAME64","TOWER_LATITUDE","TOWER_LONGITUDE","TOWER_ALTITUDE",
    "OPEN RUNWAY","LATITUDE","LONGITUDE","ALTITUDE","HEADING","LENGTH","WIDTH","PRIMARY_NUMBER","PRIMARY_DESIGNATOR","SECONDARY_NUMBER","SECONDARY_DESIGNATOR","CLOSE RUNWAY",
    "OPEN TAXI_PARKING","TYPE","NUMBER","NAME","HEADING","RADIUS","BIAS_X","BIAS_Z","CLOSE TAXI_PARKING",
    "OPEN TAXI_POINT","BIAS_X","BIAS_Z","CLOSE TAXI_POINT",
    "OPEN TAXI_PATH","TYPE","START","END","WIDTH","CLOSE TAXI_PATH","CLOSE AIRPORT"};
  for(auto field:fields)if(FAILED(SimConnect_AddToFacilityDefinition(h,kAirportDef,field)))return false;
  airportDefinitionReady_=true;
 }
 if(FAILED(SimConnect_RequestFacilityData(h,kAirportDef,id,code.c_str())))return false;
 airportRequests_[id]={{"icao",code},{"runways",json::array()},{"parking",json::array()},{"points",json::array()},{"paths",json::array()}};return true;
}
bool SimSession::onAirportRecv(const void* raw,unsigned long size){
 if(size<sizeof(SIMCONNECT_RECV))return false;auto base=static_cast<const SIMCONNECT_RECV*>(raw);
 if(base->dwID==SIMCONNECT_RECV_ID_AIRPORT_LIST){
  if(size<offsetof(SIMCONNECT_RECV_AIRPORT_LIST,rgData))return true;
  auto list=static_cast<const SIMCONNECT_RECV_AIRPORT_LIST*>(raw);if(list->dwRequestID!=kNearby)return false;
  json airports=json::array();
  for(DWORD i=0;i<list->dwArraySize && i<1000 && offsetof(SIMCONNECT_RECV_AIRPORT_LIST,rgData)+(i+1)*sizeof(SIMCONNECT_DATA_FACILITY_AIRPORT)<=size;++i){const auto& a=list->rgData[i];airports.push_back({{"icao",std::string(a.Ident,strnlen(a.Ident,9))},{"region",std::string(a.Region,strnlen(a.Region,3))},{"lat",a.Latitude},{"lon",a.Longitude},{"altitude",a.Altitude}});}
  staged_.push_back(backend_event::AirportList{airports,list->dwEntryNumber+1>=list->dwOutOf});return true;
 }
 if(base->dwID==SIMCONNECT_RECV_ID_FACILITY_DATA_END){if(size<sizeof(SIMCONNECT_RECV_FACILITY_DATA_END))return true;auto end=static_cast<const SIMCONNECT_RECV_FACILITY_DATA_END*>(raw);auto it=airportRequests_.find(end->RequestId);if(it==airportRequests_.end())return true;
  staged_.push_back(backend_event::AirportFacility{end->RequestId,it->second,it->second.contains("lat")?"":"Airport was not found in installed scenery."});airportRequests_.erase(it);return true;}
 if(base->dwID!=SIMCONNECT_RECV_ID_FACILITY_DATA)return false;
 if(size<offsetof(SIMCONNECT_RECV_FACILITY_DATA,Data))return true;auto d=static_cast<const SIMCONNECT_RECV_FACILITY_DATA*>(raw);
 auto identity=airportIdentityRequests_.find(d->UserRequestId);
 if(identity!=airportIdentityRequests_.end()){
  if(d->Type==SIMCONNECT_FACILITY_DATA_AIRPORT && size>=offsetof(SIMCONNECT_RECV_FACILITY_DATA,Data)+sizeof(AirportIdentityData)){
   AirportIdentityData a;std::memcpy(&a,reinterpret_cast<const char*>(raw)+offsetof(SIMCONNECT_RECV_FACILITY_DATA,Data),sizeof(a));
   auto code=std::string(a.icao,strnlen(a.icao,8));
   if(profiles::airport::validCode(code))staged_.push_back(backend_event::AirportIdentity{js::str(identity->second,"icao"),js::str(identity->second,"region"),code,a.lat,a.lon});
  }
  airportIdentityRequests_.erase(identity);return true;
 }
 auto it=airportRequests_.find(d->UserRequestId);if(it==airportRequests_.end())return true;
 auto& f=it->second;const auto count=size - offsetof(SIMCONNECT_RECV_FACILITY_DATA,Data);
 const auto* bytes=reinterpret_cast<const char*>(raw)+offsetof(SIMCONNECT_RECV_FACILITY_DATA,Data);
 if(d->Type==SIMCONNECT_FACILITY_DATA_AIRPORT && count>=sizeof(AirportData)){AirportData a;std::memcpy(&a,bytes,sizeof(a));f.update({{"lat",a.lat},{"lon",a.lon},{"altitude",a.alt},{"name",std::string(a.name,strnlen(a.name,64))},{"tower",{{"lat",a.towerLat},{"lon",a.towerLon},{"altitude",a.towerAlt}}}});}
 else if(d->Type==SIMCONNECT_FACILITY_DATA_RUNWAY && count>=sizeof(RunwayData)&&f["runways"].size()<100){RunwayData r;std::memcpy(&r,bytes,sizeof(r));f["runways"].push_back({{"lat",r.lat},{"lon",r.lon},{"altitude",r.alt},{"heading",r.heading},{"length",r.length},{"width",r.width},{"primary",runwayName(r.primary,r.primaryDesignator)},{"secondary",runwayName(r.secondary,r.secondaryDesignator)}});}
 else if(d->Type==SIMCONNECT_FACILITY_DATA_TAXI_PARKING && count>=sizeof(ParkingData)&&f["parking"].size()<3000){ParkingData p;std::memcpy(&p,bytes,sizeof(p));bool gate=(p.type>=8&&p.type<=10)||p.type==15;std::string prefix=gate?"Gate ":"Parking ";if(p.name>=12&&p.name<=37)prefix+=char('A'+p.name-12);
  f["parking"].push_back({{"type",p.type},{"name",prefix+std::to_string(p.number)},{"key",std::to_string(p.name)+":"+std::to_string(p.number)},{"gate",gate},{"heading",p.heading},{"radius",p.radius},{"east",p.east},{"north",p.north}});}
 else if(d->Type==SIMCONNECT_FACILITY_DATA_TAXI_POINT && count>=sizeof(PointData)&&d->ItemIndex<10000){PointData p;std::memcpy(&p,bytes,sizeof(p));while(f["points"].size()<=d->ItemIndex)f["points"].push_back(nullptr);f["points"][d->ItemIndex]={{"east",p.east},{"north",p.north}};}
 else if(d->Type==SIMCONNECT_FACILITY_DATA_TAXI_PATH && count>=sizeof(PathData)&&f["paths"].size()<20000){PathData p;std::memcpy(&p,bytes,sizeof(p));f["paths"].push_back({{"type",p.type},{"start",p.start},{"end",p.end},{"width",p.width}});}
 return true;
}
}
