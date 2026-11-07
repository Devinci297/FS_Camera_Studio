// A stand-in X-Plane for the XP Camera Studio plugin (Linux only): implements the XPLM functions the plugin
// imports, loads XPCameraStudio.xpl and plays a browser window, so the panel round trip can be checked without
// X-Plane. Run: xpcs_fake_xplane <path to XPCameraStudio.xpl> <scratch folder> [snapshot.json]
//
// Checks: the editor command opens a browser window with the X-Plane URL; Hello through xplane.fscs gets a Snapshot
// naming the loaded aircraft; a profile created, edited and saved from the panel is in the profiles folder after the
// plugin is disabled and enabled again; TextFocus hands the window X-Plane's keyboard.

#include <XPLMDataAccess.h>
#include <XPLMDefs.h>
#include <XPLMDisplay.h>
#include <XPLMMenus.h>
#include <XPLMPlanes.h>
#include <XPLMPlugin.h>
#include <XPLMProcessing.h>
#include <XPLMUtilities.h>

#include "fscs/protocol/protocol.hpp"

#include <nlohmann/json.hpp>

#include <dlfcn.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <functional>
#include <map>
#include <string>
#include <vector>

using json = nlohmann::json;
namespace fs = std::filesystem;

namespace {

std::string g_root, g_prefs, g_pluginPath;  // X-Plane folder, preferences file, plugin binary
struct FakeWindow {
    XPLMCreateWindow_t params{};
    bool visible = false;
    std::string url, title;
    std::map<std::string, std::pair<XPLMBrowserCallback_f, void*>> functions;
    std::vector<std::string> scripts;
};
std::vector<FakeWindow*> g_windows;
XPLMWindowID g_keyboard = nullptr;
struct Command { std::string name; XPLMCommandCallback_f handler = nullptr; void* refcon = nullptr; };
std::vector<Command*> g_commands;
XPLMFlightLoop_f g_loop = nullptr;
void* g_loopRefcon = nullptr;
std::string g_returned;
int g_paused = 0;
const char* kUiName = "Cessna 172 SP";

void out(char* buffer, const std::string& text) { std::snprintf(buffer, 512, "%s", text.c_str()); }

}  // namespace

extern "C" {

void XPLMDebugString(const char* s) { std::fputs(s, stdout); }
void XPLMEnableFeature(const char*, int) {}
void XPLMGetVersions(int* xp, int* xplm, XPLMHostApplicationID* host) {
    *xp = 12441;
    *xplm = 440;
    *host = xplm_Host_XPlane;
}
XPLMPluginID XPLMGetMyID() { return 7; }
void XPLMGetPluginInfo(XPLMPluginID, char* name, char* path, char* sig, char* desc) {
    if (name) out(name, "XP Camera Studio");
    if (path) out(path, g_pluginPath);
    if (sig) out(sig, "xpcs.camerastudio");
    if (desc) out(desc, "");
}
void XPLMGetSystemPath(char* path) { out(path, g_root + "/"); }
void XPLMGetPrefsPath(char* path) { out(path, g_prefs); }

XPLMCommandRef XPLMCreateCommand(const char* name, const char*) {
    auto* c = new Command{name};
    g_commands.push_back(c);
    return c;
}
void XPLMRegisterCommandHandler(XPLMCommandRef cmd, XPLMCommandCallback_f handler, int, void* refcon) {
    static_cast<Command*>(cmd)->handler = handler;
    static_cast<Command*>(cmd)->refcon = refcon;
}
void XPLMUnregisterCommandHandler(XPLMCommandRef cmd, XPLMCommandCallback_f, int, void*) { static_cast<Command*>(cmd)->handler = nullptr; }

XPLMMenuID XPLMFindPluginsMenu() { return reinterpret_cast<XPLMMenuID>(1); }
int XPLMAppendMenuItem(XPLMMenuID, const char*, void*, int) { return 0; }
XPLMMenuID XPLMCreateMenu(const char*, XPLMMenuID, int, XPLMMenuHandler_f, void*) { return reinterpret_cast<XPLMMenuID>(2); }
int XPLMAppendMenuItemWithCommand(XPLMMenuID, const char*, XPLMCommandRef) { return 0; }
void XPLMDestroyMenu(XPLMMenuID) {}

XPLMFlightLoopID XPLMCreateFlightLoop(XPLMCreateFlightLoop_t* p) {
    g_loop = p->callbackFunc;
    g_loopRefcon = p->refcon;
    return reinterpret_cast<XPLMFlightLoopID>(3);
}
void XPLMScheduleFlightLoop(XPLMFlightLoopID, float, int) {}
void XPLMDestroyFlightLoop(XPLMFlightLoopID) { g_loop = nullptr; }

XPLMDataRef XPLMFindDataRef(const char* name) {
    if (!std::strcmp(name, "sim/time/paused")) return reinterpret_cast<XPLMDataRef>(1);
    if (!std::strcmp(name, "sim/aircraft/view/acf_ui_name")) return reinterpret_cast<XPLMDataRef>(2);
    return nullptr;
}
int XPLMGetDatai(XPLMDataRef) { return g_paused; }
int XPLMGetDatab(XPLMDataRef, void* value, int offset, int max) {
    const int n = static_cast<int>(std::strlen(kUiName));
    if (!value) return n;
    const int count = std::max(0, std::min(max, n - offset));
    std::memcpy(value, kUiName + offset, static_cast<std::size_t>(count));
    return count;
}
void XPLMGetNthAircraftModel(int, char* file, char* path) {
    std::snprintf(file, 256, "%s", "Cessna_172SP.acf");
    std::snprintf(path, 512, "%s", (g_root + "/Aircraft/Laminar Research/Cessna 172 SP/Cessna_172SP.acf").c_str());
}

void XPLMGetScreenBoundsGlobal(int* l, int* t, int* r, int* b) {
    *l = 0;
    *t = 1440;
    *r = 2560;
    *b = 0;
}
XPLMWindowID XPLMCreateWindowEx(XPLMCreateWindow_t* p) {
    auto* w = new FakeWindow();
    w->params = *p;
    w->visible = p->visible != 0;
    g_windows.push_back(w);
    return w;
}
void XPLMDestroyWindow(XPLMWindowID id) {
    for (auto it = g_windows.begin(); it != g_windows.end(); ++it)
        if (*it == id) {
            delete *it;
            g_windows.erase(it);
            break;
        }
    if (g_keyboard == id) g_keyboard = nullptr;
}
void XPLMSetWindowPositioningMode(XPLMWindowID, XPLMWindowPositioningMode, int) {}
void XPLMSetWindowTitle(XPLMWindowID id, const char* title) { static_cast<FakeWindow*>(id)->title = title; }
void XPLMSetWindowResizingLimits(XPLMWindowID, int, int, int, int) {}
void XPLMSetWindowIsVisible(XPLMWindowID id, int visible) { static_cast<FakeWindow*>(id)->visible = visible != 0; }
int XPLMGetWindowIsVisible(XPLMWindowID id) { return static_cast<FakeWindow*>(id)->visible; }
void XPLMBringWindowToFront(XPLMWindowID) {}
void XPLMGetWindowGeometry(XPLMWindowID id, int* l, int* t, int* r, int* b) {
    const auto& p = static_cast<FakeWindow*>(id)->params;
    *l = p.left;
    *t = p.top;
    *r = p.right;
    *b = p.bottom;
}
void XPLMSetWindowGeometry(XPLMWindowID id, int l, int t, int r, int b) {
    auto& p = static_cast<FakeWindow*>(id)->params;
    p.left = l;
    p.top = t;
    p.right = r;
    p.bottom = b;
}
void XPLMTakeKeyboardFocus(XPLMWindowID id) { g_keyboard = id; }
int XPLMHasKeyboardFocus(XPLMWindowID id) { return g_keyboard == id; }
void XPLMWindowAddBrowserFunction(XPLMWindowID id, const char* name, XPLMBrowserCallback_f f, void* refcon) {
    static_cast<FakeWindow*>(id)->functions[name] = {f, refcon};
}
void XPLMWindowSetURL(XPLMWindowID id, const char* url) { static_cast<FakeWindow*>(id)->url = url; }
void XPLMWindowInjectScript(XPLMWindowID id, const char* script) { static_cast<FakeWindow*>(id)->scripts.push_back(script); }
const char* XPLMReturnString(const char* s) {
    g_returned = s;
    return g_returned.c_str();
}

}  // extern "C"

namespace {

int failures = 0;
void check(bool ok, const std::string& what) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what.c_str());
    if (!ok) ++failures;
}

struct Plugin {
    void* lib = nullptr;
    int (*start)(char*, char*, char*) = nullptr;
    void (*stop)() = nullptr;
    int (*enable)() = nullptr;
    void (*disable)() = nullptr;
    void (*message)(XPLMPluginID, int, void*) = nullptr;
    bool load(const std::string& path) {
        lib = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
        if (!lib) {
            std::printf("dlopen failed: %s\n", dlerror());
            return false;
        }
        start = reinterpret_cast<int (*)(char*, char*, char*)>(dlsym(lib, "XPluginStart"));
        stop = reinterpret_cast<void (*)()>(dlsym(lib, "XPluginStop"));
        enable = reinterpret_cast<int (*)()>(dlsym(lib, "XPluginEnable"));
        disable = reinterpret_cast<void (*)()>(dlsym(lib, "XPluginDisable"));
        message = reinterpret_cast<void (*)(XPLMPluginID, int, void*)>(dlsym(lib, "XPluginReceiveMessage"));
        return start && stop && enable && disable && message;
    }
};

void frames(int n) {
    for (int i = 0; i < n; ++i)
        if (g_loop) g_loop(1.0f / 60, 1.0f / 60, i, g_loopRefcon);
}

// The page: frames into xplane.fscs, frames out of the injected FSCXPlaneReceive calls.
struct Page {
    FakeWindow* win = nullptr;
    std::string instance;
    int req = 0;
    std::size_t seen = 0;
    std::string bootId;
    long long epoch = 0;
    long long draftRevision = -1;
    fscs::protocol::Reassembler reassembler;
    double t = 0;

    std::vector<json> drain() {
        std::vector<json> msgs;
        for (; seen < win->scripts.size(); ++seen) {
            const std::string& s = win->scripts[seen];
            const std::string prefix = "window.FSCXPlaneReceive&&window.FSCXPlaneReceive(";
            if (s.rfind(prefix, 0) != 0 || s.size() < prefix.size() + 2) continue;
            const json literal = json::parse(s.substr(prefix.size(), s.size() - prefix.size() - 2), nullptr, false);
            if (!literal.is_string()) continue;
            if (auto whole = reassembler.feed(literal.get<std::string>(), t += 0.01)) {
                json m = json::parse(*whole, nullptr, false);
                if (!m.is_object() || (m.contains("to") && m["to"] != instance)) continue;
                if (m.value("name", "") == "Snapshot" || m.value("name", "") == "DraftChanged") {
                    const json& p = m["payload"];
                    if (p.contains("draftRevision")) draftRevision = p["draftRevision"].get<long long>();
                    if (p.contains("working") && p["working"].contains("draftRevision"))
                        draftRevision = p["working"]["draftRevision"].get<long long>();
                }
                if (m.contains("moduleBootId")) bootId = m["moduleBootId"].get<std::string>();
                if (m.contains("sessionEpoch")) epoch = m["sessionEpoch"].get<long long>();
                msgs.push_back(std::move(m));
            }
        }
        return msgs;
    }
    std::string send(const std::string& action, json payload = json::object()) {
        const std::string id = instance.substr(0, instance.find('-')) + "-" + std::to_string(++req);
        json msg{{"protocolVersion", 2}, {"requestId", id}, {"uiInstanceId", instance}};
        if (action != "Hello" && !bootId.empty()) {
            msg["moduleBootId"] = bootId;
            msg["sessionEpoch"] = epoch;
        }
        if (draftRevision >= 0) msg["draftRevision"] = draftRevision;
        msg["action"] = action;
        msg["payload"] = payload;
        auto [fn, refcon] = win->functions.at("fscs");
        const std::string arg = json(msg.dump()).dump();  // X-Plane passes the argument JSON-encoded
        const std::string reply = fn(win, arg.c_str(), refcon);
        if (reply != "true") std::printf("xplane.fscs answered %s\n", reply.c_str());
        frames(3);
        return id;
    }
    json find(const std::vector<json>& msgs, const std::string& name) {
        json found;
        for (const auto& m : msgs)
            if (m.value("type", "") == "event" && m.value("name", "") == name) found = m;
        return found;
    }
    json ack(const std::vector<json>& msgs, const std::string& requestId) {
        for (const auto& m : msgs)
            if (m.value("type", "") == "ack" && m.value("requestId", "") == requestId) return m;
        return nullptr;
    }
};

FakeWindow* openEditor() {
    for (Command* c : g_commands)
        if (c->name == "xpcs/toggle_editor" && c->handler) c->handler(c, xplm_CommandBegin, c->refcon);
    for (FakeWindow* w : g_windows)
        if (w->url.find("surface=main") != std::string::npos) {
            if (w->params.browserLoadFinishedFunc) w->params.browserLoadFinishedFunc(w, w->url.c_str(), w->params.refcon);
            return w;
        }
    return nullptr;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::printf("usage: %s <XPCameraStudio.xpl> <scratch folder>\n", argv[0]);
        return 2;
    }
    const fs::path scratch = fs::absolute(argv[2]);
    fs::remove_all(scratch);
    g_root = (scratch / "X-Plane 12").string();
    fs::create_directories(fs::path(g_root) / "Output" / "preferences");
    g_prefs = (fs::path(g_root) / "Output" / "preferences" / "X-Plane.prf").string();
    g_pluginPath = (fs::path(g_root) / "Resources" / "plugins" / "XPCameraStudio" / "lin_x64" / "XPCameraStudio.xpl").string();

    Plugin plugin;
    if (!plugin.load(argv[1])) return 1;
    char name[256] = {}, sig[256] = {}, desc[256] = {};
    check(plugin.start(name, sig, desc) == 1, "XPluginStart accepts X-Plane 12.4.4");
    check(std::string(sig) == "xpcs.camerastudio", "signature xpcs.camerastudio");
    check(plugin.enable() == 1 && g_loop, "XPluginEnable starts the flight loop");
    frames(5);

    FakeWindow* win = openEditor();
    check(win && win->visible, "xpcs/toggle_editor opens the editor window");
    if (!win) return 1;
    check(win->params.contentType == xplm_WindowContentTypeBrowser, "it is a browser window");
    check(win->url.rfind("file:///", 0) == 0 && win->url.find("XPCameraStudio/ui/index.html?surface=main&host=xplane") != std::string::npos,
          "it loads the bundled ui/index.html with host=xplane: " + win->url);
    check(win->functions.count("fscs") && win->functions.count("fscsPanel"), "xplane.fscs and xplane.fscsPanel are registered");

    Page page{win, "main-abc123"};
    page.send("Hello", {{"surface", "main"}});
    auto msgs = page.drain();
    const json snap = page.find(msgs, "Snapshot");
    check(snap.is_object(), "Hello gets a Snapshot");
    const std::string snapText = snap.dump();
    check(snapText.find("Cessna 172 SP") != std::string::npos, "the Snapshot names the loaded aircraft (Cessna 172 SP)");
    check(snapText.find("Observing") != std::string::npos, "the camera stays in Observing");

    std::string id = page.send("CreateProfile", {{"name", "XP cameras"}, {"forAircraft", true}});
    msgs = page.drain();
    const json created = page.ack(msgs, id);
    check(created.is_object() && created.value("status", "") != "Rejected", "CreateProfile accepted: " + created.dump());

    page.send("RequestSnapshot");
    msgs = page.drain();
    json working = page.find(msgs, "Snapshot")["payload"]["working"];
    check(working["profile"].is_object() && working["profile"]["cameras"].size() > 0, "the new profile has cameras");
    json cam = working["profile"]["cameras"][0];
    cam["name"] = "XP Captain";
    id = page.send("UpdateDraft", {{"ops", json::array({{{"op", "setCamera"}, {"camera", cam}}})}});
    msgs = page.drain();
    check(page.ack(msgs, id).value("status", "") == "Completed", "UpdateDraft renames a camera");
    page.send("CommitChanges");
    msgs = page.drain();
    const json done = page.find(msgs, "CommitCompleted");
    check(done.is_object() && done["payload"].value("status", "") == "Committed", "CommitChanges saves: " + done.dump());

    const fs::path store = fs::path(g_root) / "Output" / "preferences" / "XPCameraStudio";
    bool storeFile = false;
    if (fs::exists(store))
        for (const auto& e : fs::recursive_directory_iterator(store))
            if (e.is_regular_file() && e.path().filename().string().rfind("fscs-store-", 0) == 0) storeFile = true;
    check(storeFile, "a profile store file is in Output/preferences/XPCameraStudio");

    // The mini window: opened by its command, it fits its height to the page's dock and keeps its bottom edge.
    for (Command* c : g_commands)
        if (c->name == "xpcs/toggle_mini" && c->handler) c->handler(c, xplm_CommandBegin, c->refcon);
    FakeWindow* mini = nullptr;
    for (FakeWindow* w : g_windows)
        if (w->url.find("surface=mini") != std::string::npos) mini = w;
    check(mini && mini->visible, "xpcs/toggle_mini opens the mini window");
    if (mini && mini->functions.count("fscsPanelSize")) {
        const int bottom = mini->params.bottom;
        auto [fn, refcon] = mini->functions.at("fscsPanelSize");
        const std::string reply = fn(mini, R"({"surface":"mini","height":122})", refcon);
        check(reply == "true" && mini->params.top - mini->params.bottom == 122 && mini->params.bottom == bottom,
              "fscsPanelSize fits the mini window to 122 and keeps its bottom edge");
        auto [mainFn, mainRefcon] = win->functions.at("fscsPanelSize");
        const int mainHeight = win->params.top - win->params.bottom;
        mainFn(win, R"({"surface":"main","height":100})", mainRefcon);
        check(win->params.top - win->params.bottom == mainHeight, "the editor window keeps its size");
    } else {
        check(false, "the mini window registers fscsPanelSize");
    }

    page.send("TextFocus", {{"focused", true}});
    check(g_keyboard == win, "TextFocus gives the editor X-Plane's keyboard");
    page.send("TextFocus", {{"focused", false}});
    check(g_keyboard == nullptr, "losing text focus gives the keyboard back");

    // Disable and enable: the profile is read back from the files.
    plugin.disable();
    check(g_windows.empty(), "XPluginDisable closes the windows");
    check(plugin.enable() == 1, "XPluginEnable again");
    frames(5);
    win = openEditor();
    Page again{win, "main-def456"};
    again.send("Hello", {{"surface", "main"}});
    msgs = again.drain();
    const std::string reloaded = again.find(msgs, "Snapshot").dump();
    check(reloaded.find("XP Captain") != std::string::npos, "after a restart the saved profile is loaded with the renamed camera");
    if (argc > 3) {  // optional: keep the reloaded Snapshot, e.g. to render it in the real page
        if (FILE* f = std::fopen(argv[3], "wb")) {
            std::fputs(again.find(msgs, "Snapshot").dump().c_str(), f);
            std::fclose(f);
        }
    }

    plugin.disable();
    plugin.stop();
    std::printf("%s (%d failed)\n", failures ? "FAILED" : "all checks passed", failures);
    return failures ? 1 : 0;
}
