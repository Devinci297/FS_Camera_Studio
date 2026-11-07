#include "xp_key_bindings.hpp"
#include <algorithm>
#include <chrono>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

namespace fscs::xplane::keys {
namespace {
[[noreturn]] void fail(const std::string& s) { throw std::runtime_error(s); }
std::string utf8(const fs::path& p) { auto s=p.u8string(); return {s.begin(),s.end()}; }
fs::path native(const std::string& s) { return fs::path(std::u8string(s.begin(),s.end())); }
fs::path storage(const fs::path& p) { return p/"XPCameraStudio"; }
fs::path queueFile(const fs::path& p) { return storage(p)/"native-key-queue.json"; }
std::string stamp() { return std::to_string(std::chrono::system_clock::now().time_since_epoch().count()); }
void commandCheck(const std::string& c) {
    if(c.rfind("xpcs/view_",0)!=0) fail("Only XPCS camera-view commands can be assigned here.");
    const auto n=c.substr(10);
    int id=0;
    try { id=std::stoi(n); } catch(...) { fail("Invalid camera view command."); }
    if(id<1 || id>30 || std::to_string(id)!=n) fail("Use a camera view ID from 1 to 30.");
}
std::string modifiers(const std::string& text) {
    if(text=="<NONE>") return text;
    std::set<std::string> parts;
    std::istringstream in(text); std::string s;
    while(std::getline(in,s,'+')) {
        if(s!="SHIFT" && s!="ALT" && s!="CTRL") fail("Unknown native key modifier: "+s);
        if(!parts.insert(s).second) fail("Duplicate native key modifier.");
    }
    std::string result;
    for(const auto& m:{"SHIFT","ALT","CTRL"}) if(parts.count(m)) { if(!result.empty())result+="+"; result+=m; }
    if(result.empty())fail("Empty key modifier.");
    return result;
}
Chord savedChord(const json& j) {
    Chord c{j.at("key").get<std::string>(),modifiers(j.at("modifiers").get<std::string>())};
    // Accept only tokens produced by chord(); queue files cannot inject lines or arbitrary preference syntax.
    bool valid=false;
    for(char ch='A';ch<='Z';++ch) valid|=c.key==std::string(1,ch);
    for(int n=0;n<=9;++n) valid|=c.key==std::to_string(n)||c.key=="Numpad-"+std::to_string(n);
    for(int n=1;n<=24;++n) valid|=c.key=="F"+std::to_string(n);
    for(const auto& key:{"Left","Right","Up","Down","Space","Return","Escape",",",".","/",";","[","]","-","=","`"})
        valid|=c.key==key;
    if(!valid) fail("Unsupported native key token.");
    return c;
}
json asJson(const Chord& c) { return {{"key",c.key},{"modifiers",c.modifiers}}; }
bool same(const Chord& a,const Chord& b) { return a.key==b.key && a.modifiers==b.modifiers; }
struct Line { std::string raw, command; Chord key; bool binding=false; };
std::vector<Line> lines(const std::string& text) {
    if(text.size()>4*1024*1024)fail("Keyboard preferences are unexpectedly large.");
    std::vector<Line> result;
    std::size_t start=0;
    while(start<text.size()) {
        auto end=text.find('\n',start); if(end==std::string::npos)end=text.size()-1;
        auto raw=text.substr(start,end-start+1); start=end+1;
        result.push_back({raw,{}, {},false});
    }
    if(result.size()<2)fail("Keyboard preferences have no version header.");
    auto clean=[](std::string s){while(!s.empty()&&(s.back()=='\r'||s.back()=='\n'))s.pop_back();return s;};
    auto first=clean(result[0].raw);
    if(first.rfind("\xEF\xBB\xBF",0)==0)first.erase(0,3);
    if((first!="I" && first!="A")||clean(result[1].raw)!="1005 Version")fail("Unsupported X-Plane keyboard preference version.");
    for(std::size_t i=2;i<result.size();++i) {
        auto& l=result[i]; auto textLine=clean(l.raw);
        if(textLine.empty() || textLine[0]=='#')continue;
        std::istringstream in(textLine); std::string extra;
        if(!(in>>l.key.key>>l.key.modifiers>>l.command)||(in>>extra))fail("Unrecognized keyboard preference line; no changes made.");
        l.key.modifiers=modifiers(l.key.modifiers); l.binding=true;
    }
    return result;
}
json owners(const std::vector<Line>& rows,const Chord& c) {
    std::vector<std::string> result;
    for(const auto& row:rows)if(row.binding&&same(row.key,c))result.push_back(row.command);
    std::sort(result.begin(),result.end());return result;
}
fs::path profilePath(const fs::path& prefs,const std::string& id) {
    fs::path rel=native(id);
    if(id=="X-Plane Keys.prf") {}
    else if(rel.parent_path()!=fs::path("control profiles") || rel.filename().empty() ||
            !id.ends_with(" - Keys.prf") || id.find('\\')!=std::string::npos ||
            id.find(':')!=std::string::npos || id.find("..")!=std::string::npos)
        fail("Select an existing X-Plane keyboard profile.");
    auto path=prefs/rel; safePath(path);
    if(!fs::is_regular_file(path))fail("The selected keyboard profile no longer exists.");
    return path;
}
json loadQueue(const fs::path& p) {
    const auto path=queueFile(p);
    if(!fs::exists(path))return {{"version",1},{"revision","0"},{"operations",json::array()}};
    auto j=json::parse(read(path));
    if(j.at("version")!=1 || !j.at("operations").is_array() || j["operations"].size()>256)fail("Invalid pending keyboard assignments.");
    if(j.contains("transaction")) {
        const auto tx=j.at("transaction");const auto profile=tx.at("profile").get<std::string>();
        const auto current=read(profilePath(p,profile));
        if(current==tx.at("after").get<std::string>()) {
            json remaining=json::array();for(const auto& op:j["operations"])if(op["profile"]!=profile)remaining.push_back(op);
            j["operations"]=remaining;
        } else if(current!=tx.at("before").get<std::string>())fail("An interrupted native-key transaction needs review; its backup is in XPCameraStudio/keyboard-backups.");
        j.erase("transaction");j["revision"]=stamp();writeAtomic(path,j.dump(2));
        if(current==tx.at("after").get<std::string>())writeAtomic(storage(p)/"native-key-result.json",
            json{{"status","applied"},{"errors",json::array()},{"applied",json::array({{{"profile",profile},{"backup",tx.value("backup",std::string())},{"recovered",true}}})}}.dump(2));
    }
    return j;
}
std::string projected(const fs::path& prefs,const json& queue,const std::string& profile) {
    auto text=read(profilePath(prefs,profile));
    for(const auto& op:queue["operations"])if(op.at("profile")==profile)text=edit(text,op);
    return text;
}
// Shared by the plugin and the helper, preventing two plugin instances/workers from racing.
struct Lock {
#ifdef _WIN32
    HANDLE h=INVALID_HANDLE_VALUE;
    explicit Lock(const fs::path& path) {
        safePath(path);
        h=CreateFileW(path.c_str(),GENERIC_READ|GENERIC_WRITE,0,nullptr,OPEN_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
        if(h==INVALID_HANDLE_VALUE)fail("Keyboard assignments are busy. Try again.");
    }
    ~Lock(){CloseHandle(h);}
#else
    int h=-1;
    explicit Lock(const fs::path& path) {
        safePath(path);h=open(path.c_str(),O_CREAT|O_RDWR,0600);
        if(h<0||flock(h,LOCK_EX|LOCK_NB)!=0){if(h>=0)close(h);fail("Keyboard assignments are busy.");}
    }
    ~Lock(){flock(h,LOCK_UN);close(h);}
#endif
};
void prepare(const fs::path& prefs) {
    safePath(prefs);safePath(storage(prefs));fs::create_directories(storage(prefs));
}
json replyStatus(const fs::path& prefs,const json& q) {
    json j={{"ok",true},{"pendingCount",q["operations"].size()},{"revision",q["revision"]}};
    const auto receipt=storage(prefs)/"native-key-result.json";
    if(fs::exists(receipt))j["lastResult"]=json::parse(read(receipt));
    return j;
}
}
Chord chord(const json& input) {
    const auto code=input.at("code").get<std::string>();
    std::string key;
    if(code.size()==4 && code.rfind("Key",0)==0 && code[3]>='A' && code[3]<='Z') key=code.substr(3);
    else if(code.size()==6 && code.rfind("Digit",0)==0 && code[5]>='0' && code[5]<='9')key=code.substr(5);
    else if(code.size()==7 && code.rfind("Numpad",0)==0 && code[6]>='0' && code[6]<='9')key="Numpad-"+code.substr(6);
    else if(code.size()>=2 && code[0]=='F') {
        for(int n=1;n<=24;++n)if(code=="F"+std::to_string(n))key=code;
    } else {
        const std::map<std::string,std::string> table={{"ArrowLeft","Left"},{"ArrowRight","Right"},{"ArrowUp","Up"},
            {"ArrowDown","Down"},{"Space","Space"},{"Enter","Return"},{"Escape","Escape"},{"Comma",","},{"Period","."},
            {"Slash","/"},{"Semicolon",";"},{"BracketLeft","["},{"BracketRight","]"},{"Minus","-"},{"Equal","="},{"Backquote","`"}};
        if(auto it=table.find(code);it!=table.end())key=it->second;
    }
    if(key.empty()||input.value("meta",false))fail("Use a letter, number, numpad digit, F-key, arrow, space, Enter, or supported punctuation; Windows keys are not supported.");
    std::string mods;
    for(const auto& pair:{std::pair{"shift","SHIFT"},std::pair{"alt","ALT"},std::pair{"ctrl","CTRL"}})
        if(input.value(pair.first,false)){if(!mods.empty())mods+="+";mods+=pair.second;}
    return {key,mods.empty()?"<NONE>":mods};
}
json inspect(const std::string& text,const std::string& command,const Chord* key) {
    commandCheck(command);auto rows=lines(text);
    json result={{"bindings",json::array()}};
    for(const auto& row:rows)if(row.binding&&row.command==command)result["bindings"].push_back(asJson(row.key));
    if(key){result["key"]=asJson(*key);result["owners"]=owners(rows,*key);}
    return result;
}
std::string edit(const std::string& text,const json& op) {
    const auto command=op.at("command").get<std::string>();commandCheck(command);
    const auto key=savedChord(op.at("key"));
    const auto mode=op.at("mode").get<std::string>(); if(mode!="assign"&&mode!="remove")fail("Invalid key operation.");
    const auto rows=lines(text);
    const auto actual=owners(rows,key);
    const bool assign=mode=="assign";
    // Idempotency after an interrupted helper: exact final ownership means this operation already applied.
    const json wanted=assign?json::array({command}):op.at("expected");
    json afterRemove=json::array();
    if(!assign)for(const auto& owner:op.at("expected"))if(owner!=command)afterRemove.push_back(owner);
    const bool already=actual==(assign?wanted:afterRemove);
    if(actual!=op.at("expected")&&!already)fail("A shortcut changed in X-Plane after it was reviewed. Reopen its XPCS assignment to review it again.");
    const bool replace=op.contains("replace")&&!op["replace"].is_null();
    Chord old;
    if(replace) {
        old=savedChord(op["replace"]);
        auto oldOwners=owners(rows,old);
        auto expected=op.at("replaceExpected");
        json remaining=json::array();
        for(const auto& o:expected)if(o!=command)remaining.push_back(o);
        if(oldOwners!=expected && oldOwners!=remaining)fail("The shortcut being replaced changed in X-Plane. Review it again.");
    }
    std::string out;
    for(const auto& row:rows) {
        if(row.binding && same(row.key,key) && (assign||row.command==command))continue;
        if(row.binding && replace && same(row.key,old) && row.command==command)continue;
        out+=row.raw;
    }
    if(assign) {
        const std::string nl=text.find("\r\n")!=std::string::npos?"\r\n":"\n";
        if(!out.empty()&&out.back()!='\n')out+=nl;
        out+=key.key+" "+key.modifiers+" "+command+nl;
    }
    return out;
}
void safePath(const fs::path& path) {
    // Fail closed on linked files/directories, including Windows junctions.
    fs::path current;
    for(const auto& part:fs::absolute(path)) {
        current/=part;
#ifdef _WIN32
        const auto attr=GetFileAttributesW(current.c_str());
        if(attr!=INVALID_FILE_ATTRIBUTES && (attr&FILE_ATTRIBUTE_REPARSE_POINT))fail("Linked keyboard preference paths are not supported.");
#else
        std::error_code ec;if(fs::is_symlink(fs::symlink_status(current,ec)))fail("Linked keyboard preference paths are not supported.");
#endif
    }
}
std::string read(const fs::path& path) {
    safePath(path);
    if(fs::file_size(path)>4*1024*1024)fail("Keyboard data is unexpectedly large.");
    std::ifstream in(path,std::ios::binary);
    if(!in)fail("Cannot read keyboard file: "+utf8(path.filename()));
    std::string out((std::istreambuf_iterator<char>(in)),{});
    if(in.bad())fail("Keyboard file read failed.");return out;
}
void writeAtomic(const fs::path& path,const std::string& bytes) {
    safePath(path); const auto tmp=path.parent_path()/(path.filename().wstring()+L".xpcs-"+std::to_wstring(std::chrono::system_clock::now().time_since_epoch().count())+L".tmp");
    safePath(tmp);
    { std::ofstream out(tmp,std::ios::binary|std::ios::trunc);out.write(bytes.data(),static_cast<std::streamsize>(bytes.size()));out.flush();
      if(!out){std::error_code ec;fs::remove(tmp,ec);fail("Cannot save keyboard changes.");} }
#ifdef _WIN32
    if(!MoveFileExW(tmp.c_str(),path.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)){
        std::error_code ec;fs::remove(tmp,ec);fail("Cannot replace keyboard file; original preserved.");
    }
#else
    fs::rename(tmp,path);
#endif
}
void applyPending(const fs::path& prefs,const std::function<bool()>& simulatorRunning) {
    prepare(prefs);Lock lock(storage(prefs)/"native-key.lock");
    auto q=loadQueue(prefs);
    if(q["operations"].empty())return;
    json report={{"status","applied"},{"applied",json::array()},{"errors",json::array()}};
    std::set<std::string> profiles;
    for(const auto& op:q["operations"])profiles.insert(op.at("profile").get<std::string>());
    for(const auto& profile:profiles) {
        try {
            if(simulatorRunning())fail("X-Plane is running again. Pending changes were not applied.");
            const auto path=profilePath(prefs,profile);
            const auto before=read(path);auto after=before;
            for(const auto& op:q["operations"])if(op.at("profile")==profile)after=edit(after,op);
            const auto backupDir=storage(prefs)/"keyboard-backups";safePath(backupDir);fs::create_directories(backupDir);
            const auto backup=backupDir/(stamp()+"-"+utf8(path.filename()));
            writeAtomic(backup,before);
            if(read(backup)!=before)fail("Keyboard backup verification failed.");
            if(simulatorRunning()||read(path)!=before)fail("Keyboard preferences changed while applying. Original retained.");
            q["transaction"]={{"profile",profile},{"before",before},{"after",after},{"backup",utf8(backup)}};
            writeAtomic(queueFile(prefs),q.dump(2));
            writeAtomic(path,after);
            if(read(path)!=after)fail("Native keyboard verification failed.");
            report["applied"].push_back({{"profile",profile},{"backup",utf8(backup)},{"changed",before!=after}});
            json remaining=json::array();
            for(const auto& op:q["operations"])if(op.at("profile")!=profile)remaining.push_back(op);
            q.erase("transaction");q["operations"]=remaining;q["revision"]=stamp();writeAtomic(queueFile(prefs),q.dump(2));
        } catch(const std::exception& e) {
            report["status"]="error";report["errors"].push_back({{"profile",profile},{"message",e.what()}});
            if(q.contains("transaction"))break; // keep recovery metadata until this write is resolved
        }
    }
    writeAtomic(storage(prefs)/"native-key-result.json",report.dump(2));
}

Service::~Service() {
#ifdef _WIN32
    if(worker_)CloseHandle(static_cast<HANDLE>(worker_));
#endif
}
void Service::ensureWorker() {
#ifdef _WIN32
    if(worker_ && WaitForSingleObject(static_cast<HANDLE>(worker_),0)==WAIT_TIMEOUT)return;
    if(worker_){CloseHandle(static_cast<HANDLE>(worker_));worker_=nullptr;}
    safePath(helper_);if(!fs::is_regular_file(helper_))fail("The native-key helper is missing. Reinstall the complete XPCS plugin.");
    HANDLE parent=nullptr;
    if(!DuplicateHandle(GetCurrentProcess(),GetCurrentProcess(),GetCurrentProcess(),&parent,SYNCHRONIZE,TRUE,0))
        fail("Cannot prepare native-key helper.");
    SIZE_T size=0;InitializeProcThreadAttributeList(nullptr,1,0,&size);
    std::vector<unsigned char> attributes(size);
    STARTUPINFOEXW start{};start.StartupInfo.cb=sizeof(start);
    start.lpAttributeList=reinterpret_cast<PPROC_THREAD_ATTRIBUTE_LIST>(attributes.data());
    if(!InitializeProcThreadAttributeList(start.lpAttributeList,1,0,&size)){CloseHandle(parent);fail("Cannot initialize native-key helper.");}
    const auto quote=[](const std::wstring& s) { // prefs is a directory without trailing separators; reject quotes.
        if(s.find(L'"')!=std::wstring::npos)fail("Invalid helper path.");
        return L"\""+s+L"\"";
    };
    wchar_t exe[32768]={};GetModuleFileNameW(nullptr,exe,32768);
    std::wstring cmd=quote(helper_.wstring())+L" --wait-handle "+std::to_wstring(reinterpret_cast<uintptr_t>(parent))+
        L" --prefs "+quote(prefs_.wstring())+L" --sim-exe "+quote(exe);
    PROCESS_INFORMATION process{};
    const bool attributesOK=UpdateProcThreadAttribute(start.lpAttributeList,0,PROC_THREAD_ATTRIBUTE_HANDLE_LIST,&parent,sizeof(parent),nullptr,nullptr)!=0;
    const bool ok=attributesOK && CreateProcessW(helper_.c_str(),cmd.data(),nullptr,nullptr,TRUE,
        EXTENDED_STARTUPINFO_PRESENT|CREATE_NO_WINDOW,nullptr,nullptr,&start.StartupInfo,&process);
    DeleteProcThreadAttributeList(start.lpAttributeList);CloseHandle(parent);
    if(!ok)fail("Could not start native-key helper. No assignment queued.");
    CloseHandle(process.hThread);worker_=process.hProcess;
#else
    fail("Native key assignment currently requires the Windows XPCS build.");
#endif
}
std::string Service::request(const std::string& text) {
    try {
        if(text.size()>16384)fail("Keyboard request too large.");
        const auto input=json::parse(text);const auto action=input.at("op").get<std::string>();
        prepare(prefs_);Lock lock(storage(prefs_)/"native-key.lock");auto q=loadQueue(prefs_);
        auto response=replyStatus(prefs_,q);
        if(action=="profiles") {
            if(!q["operations"].empty())ensureWorker();
            response["profiles"]=json::array();
            if(fs::exists(prefs_/"X-Plane Keys.prf"))response["profiles"].push_back({{"id","X-Plane Keys.prf"},{"name","Global / default"}});
            auto directory=prefs_/"control profiles";safePath(directory);
            if(fs::exists(directory))for(const auto& e:fs::directory_iterator(directory)) {
                const auto name=utf8(e.path().filename());
                if(e.is_regular_file()&&name.ends_with(" - Keys.prf")) {
                    safePath(e.path());response["profiles"].push_back({{"id","control profiles/"+name},{"name",name.substr(0,name.size()-11)}});
                }
            }
            return response.dump();
        }
        const auto profile=input.at("profile").get<std::string>();
        profilePath(prefs_,profile);
        if(action=="cancel") {
            if(input.at("revision")!=q["revision"])fail("Assignments changed in another panel. Refresh and try again.");
            json remaining=json::array();for(const auto& op:q["operations"])if(op["profile"]!=profile)remaining.push_back(op);
            q["operations"]=remaining;q["revision"]=stamp();writeAtomic(queueFile(prefs_),q.dump(2));
            return replyStatus(prefs_,q).dump();
        }
        const auto command=input.at("command").get<std::string>();commandCheck(command);
        const auto current=projected(prefs_,q,profile);
        if(action=="inspect") {
            response.update(inspect(current,command));
            response["pending"]=std::any_of(q["operations"].begin(),q["operations"].end(),[&](const auto& op){return op["profile"]==profile;});
            return response.dump();
        }
        if(action!="preview" && action!="assign" && action!="remove")fail("Unknown keyboard operation.");
        const auto key=action=="remove"?savedChord(input.at("key")):chord(input.at("input"));
        auto detail=inspect(current,command,&key);
        json operation={{"profile",profile},{"command",command},{"mode",action=="remove"?"remove":"assign"},{"key",asJson(key)},{"expected",detail["owners"]}};
        if(input.contains("replace")&&!input["replace"].is_null()) {
            const auto old=savedChord(input["replace"]);
            if(!same(old,key)) {
                auto previous=owners(lines(current),old);
                if(std::find(previous.begin(),previous.end(),command)==previous.end())fail("The key being replaced is no longer assigned to this camera.");
                operation["replace"]=asJson(old);operation["replaceExpected"]=previous;
            }
        }
        // The UI returns the whole reviewed operation: any concurrent change forces a fresh preview.
        if(action=="preview"){response.update(detail);response["review"]=operation;return response.dump();}
        if(action=="assign" && input.at("review")!=operation)fail("Shortcut conflicts changed. Press the key again to review them.");
        if(action=="remove" && input.at("revision")!=q["revision"])fail("Assignments changed. Refresh and try again.");
        if(action=="remove" && std::find(detail["owners"].begin(),detail["owners"].end(),command)==detail["owners"].end())fail("This shortcut is no longer assigned to the camera.");
        (void)edit(current,operation);
        if(q["operations"].size()>=256)fail("Too many pending shortcuts. Restart X-Plane to apply them first.");
        ensureWorker();
        q["operations"].push_back(operation);q["revision"]=stamp();writeAtomic(queueFile(prefs_),q.dump(2));
        response=replyStatus(prefs_,q);response["pending"]=true;return response.dump();
    } catch(const std::exception& e) {return json{{"ok",false},{"error",e.what()}}.dump();}
}
} // namespace fscs::xplane::keys
