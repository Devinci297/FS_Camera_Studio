#include "test.hpp"
#include "xp_key_bindings.hpp"
#include <chrono>
#include <fstream>
using namespace fscs::xplane::keys;
namespace {
bool throws(const std::function<void()>& f) {try{f();return false;}catch(const std::exception&){return true;}}
const std::string fixture="I\r\n1005 Version\r\n# keep this comment\r\nNumpad-1 <NONE> sim/view/quick_look_1\r\n"
    "Numpad-1 <NONE> other/plugin\r\nNumpad-1 CTRL sim/view/quick_look_1_mem\r\n2 ALT+SHIFT xpcs/view_1\r\n"
    "9 <NONE> xpcs/view_1\r\nW <NONE> sim/view/forward_with_panel\r\n";
json op(const std::string& text,const Chord& key,const std::string& command="xpcs/view_1") {
    return {{"profile","X-Plane Keys.prf"},{"command",command},{"mode","assign"},{"key",{{"key",key.key},{"modifiers",key.modifiers}}},
        {"expected",inspect(text,command,&key)["owners"]}};
}
struct Folder {
    fs::path path=fs::temp_directory_path()/("xpcs-key-test-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    Folder(){fs::create_directories(path/"XPCameraStudio");}
    ~Folder(){std::error_code ec;fs::remove_all(path,ec);}
};
}
TEST_CASE("xplane keys canonical chords distinguish numpad and modifiers") {
    auto a=chord({{"code","Numpad1"},{"ctrl",true},{"shift",true}});
    CHECK_EQ(a.key,"Numpad-1");CHECK_EQ(a.modifiers,"SHIFT+CTRL");
    CHECK_EQ(chord({{"code","Digit1"}}).key,"1");
    CHECK_EQ(chord({{"code","ArrowRight"},{"alt",true}}).key,"Right");
    CHECK(throws([]{chord({{"code","ShiftLeft"}});}));
    CHECK(throws([]{chord({{"code","KeyA"},{"meta",true}});}));
    CHECK(throws([]{chord({{"code","KeyA\nB"}});}));
}
TEST_CASE("xplane keys reassign all exact conflicts and preserve unrelated bytes") {
    const Chord c{"Numpad-1","<NONE>"};const auto operation=op(fixture,c);const auto changed=edit(fixture,operation);
    CHECK(changed.find("sim/view/quick_look_1\r\n")==std::string::npos);
    CHECK(changed.find("other/plugin")==std::string::npos);
    CHECK(changed.find("Numpad-1 CTRL sim/view/quick_look_1_mem\r\n")!=std::string::npos);
    CHECK(changed.find("# keep this comment\r\n")!=std::string::npos);
    CHECK(changed.find("W <NONE> sim/view/forward_with_panel\r\n")!=std::string::npos);
    CHECK_EQ(inspect(changed,"xpcs/view_1",&c)["owners"].dump(),"[\"xpcs/view_1\"]");
    CHECK_EQ(inspect(changed,"xpcs/view_1")["bindings"].size(),3u); // secondary keys survive add
    CHECK_EQ(edit(changed,operation),changed);
}
TEST_CASE("xplane keys replacement and clear affect only the selected shortcut") {
    const Chord c{"Numpad-1","<NONE>"};auto operation=op(fixture,c);
    operation["replace"]={{"key","2"},{"modifiers","SHIFT+ALT"}};operation["replaceExpected"]={"xpcs/view_1"};
    auto changed=edit(fixture,operation);
    CHECK(changed.find("2 ALT+SHIFT")==std::string::npos);
    CHECK(changed.find("9 <NONE> xpcs/view_1")!=std::string::npos);
    CHECK_EQ(edit(changed,operation),changed);
    auto remove=op(changed,c);remove["mode"]="remove";changed=edit(changed,remove);
    CHECK(inspect(changed,"xpcs/view_1",&c)["owners"].empty());
    CHECK_EQ(inspect(changed,"xpcs/view_1")["bindings"].size(),1u);
}
TEST_CASE("xplane keys reject stale conflict previews and malformed data") {
    const Chord c{"Numpad-1","<NONE>"};const auto operation=op(fixture,c);
    CHECK(throws([&]{edit(fixture+"Numpad-1 <NONE> sim/new_action\n",operation);}));
    CHECK(throws([&]{edit("I\n9999 Version\n",operation);}));
    CHECK(throws([&]{edit(fixture+"unexpected line\n",operation);}));
    auto bad=operation;bad["command"]="sim/operation/quit";CHECK(throws([&]{edit(fixture,bad);}));
    bad=operation;bad["command"]="xpcs/view_31";CHECK(throws([&]{edit(fixture,bad);}));
    bad=operation;bad["key"]["key"]="A\nW";CHECK(throws([&]{edit(fixture,bad);}));
}
TEST_CASE("xplane keys deferred apply refuses live sim then backs up and merges final save") {
    Folder f;const auto path=f.path/"X-Plane Keys.prf";
    writeAtomic(path,fixture);
    const auto operation=op(fixture,{"Numpad-1","<NONE>"});
    const auto queue=f.path/"XPCameraStudio/native-key-queue.json";
    writeAtomic(queue,json{{"version",1},{"revision","1"},{"operations",json::array({operation})}}.dump());
    applyPending(f.path,[]{return true;});
    CHECK_EQ(read(path),fixture);
    CHECK_EQ(json::parse(read(queue))["operations"].size(),1u);
    const auto finalSave=fixture+"Z CTRL sim/late_unrelated_change\r\n";writeAtomic(path,finalSave);
    applyPending(f.path,[]{return false;});
    CHECK_EQ(read(path),edit(finalSave,operation));
    CHECK(json::parse(read(queue))["operations"].empty());
    const auto receipt=json::parse(read(f.path/"XPCameraStudio/native-key-result.json"));
    CHECK_EQ(receipt["status"].get<std::string>(),"applied");
    CHECK_EQ(read(fs::u8path(receipt["applied"][0]["backup"].get<std::string>())),finalSave);
}
TEST_CASE("xplane keys failed profile is untouched and remains pending") {
    Folder f;const auto path=f.path/"X-Plane Keys.prf";
    const auto operation=op(fixture,{"Numpad-1","<NONE>"});
    writeAtomic(path,fixture+"Numpad-1 <NONE> sim/new_action\n");
    const auto queue=f.path/"XPCameraStudio/native-key-queue.json";
    writeAtomic(queue,json{{"version",1},{"revision","1"},{"operations",json::array({operation})}}.dump());
    const auto before=read(path);applyPending(f.path,[]{return false;});
    CHECK_EQ(read(path),before);CHECK_EQ(json::parse(read(queue))["operations"].size(),1u);
    CHECK_EQ(json::parse(read(f.path/"XPCameraStudio/native-key-result.json"))["status"].get<std::string>(),"error");
}
TEST_CASE("xplane keys recover committed transaction without replaying chained assignments") {
    Folder f;const auto path=f.path/"X-Plane Keys.prf";auto first=op(fixture,{"Numpad-1","<NONE>"});
    auto second=op(edit(fixture,first),{"Numpad-1","<NONE>"},"xpcs/view_2");
    const auto after=edit(edit(fixture,first),second);writeAtomic(path,after);
    const auto queue=f.path/"XPCameraStudio/native-key-queue.json";
    writeAtomic(queue,json{{"version",1},{"revision","1"},{"operations",json::array({first,second})},
        {"transaction",{{"profile","X-Plane Keys.prf"},{"before",fixture},{"after",after}}}}.dump());
    applyPending(f.path,[]{return false;});
    CHECK_EQ(read(path),after);CHECK(json::parse(read(queue))["operations"].empty());
    CHECK_EQ(json::parse(read(f.path/"XPCameraStudio/native-key-result.json"))["status"].get<std::string>(),"applied");
}
TEST_CASE("xplane keys service rejects profile traversal and leaves preferences unchanged") {
    Folder f;writeAtomic(f.path/"X-Plane Keys.prf",fixture);
    Service service(f.path, f.path/"missing.exe");
    for(const auto& name:{"../X-Plane Keys.prf","control profiles/../../X-Plane Keys.prf","C:/outside - Keys.prf"}) {
        const auto reply=json::parse(service.request(json{{"op","inspect"},{"profile",name},{"command","xpcs/view_1"}}.dump()));
        CHECK(!reply["ok"].get<bool>());
    }
    CHECK_EQ(read(f.path/"X-Plane Keys.prf"),fixture);
}
