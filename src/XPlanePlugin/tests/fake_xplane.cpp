// A stand-in X-Plane for the XP Camera Studio plugin (Linux only): implements the XPLM functions the plugin
// imports, loads XPCameraStudio.xpl and plays a browser window, so the panel round trip can be checked without
// X-Plane. Run: xpcs_fake_xplane <path to XPCameraStudio.xpl> <scratch folder> [snapshot.json]
//
// Checks: the editor command opens a browser window with the X-Plane URL; Hello through xplane.fscs gets a Snapshot
// naming the loaded aircraft; a profile created, edited and saved from the panel is in the profiles folder after the
// plugin is disabled and enabled again; TextFocus hands the window X-Plane's keyboard.

#include <XPLMCamera.h>
#include <XPLMDataAccess.h>
#include <XPLMDefs.h>
#include <XPLMDisplay.h>
#include <XPLMMenus.h>
#include <XPLMPlanes.h>
#include <XPLMPlugin.h>
#include <XPLMProcessing.h>
#include <XPLMScenery.h>
#include <XPLMUtilities.h>

#include "fscs/protocol/protocol.hpp"

#include <nlohmann/json.hpp>

#include <dlfcn.h>

#include <algorithm>
#include <cmath>
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
    XPLMWindowPositioningMode mode = xplm_WindowPositionFree;
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

// Datarefs by name; the plugin finds what it needs and the test reads and sets values.
struct Ref { std::string name; std::vector<double> v = std::vector<double>(1, 0.0); };
std::map<std::string, Ref*> g_refs;
Ref* ref(const std::string& name) {
    auto& r = g_refs[name];
    if (!r) r = new Ref{name};
    return r;
}
double& dr(const std::string& name) { return ref(name)->v[0]; }

// Datarefs the plugin publishes, read the way another plugin would.
struct Published {
    std::string name;
    XPLMDataTypeID type;
    XPLMGetDatai_f geti;
    XPLMGetDataf_f getf;
    XPLMGetDatab_f getb;
    void* refcon;
};
std::map<std::string, Published*> g_published;
int pubi(const std::string& name) {
    const auto it = g_published.find(name);
    return it != g_published.end() && it->second->geti ? it->second->geti(it->second->refcon) : -999;
}
float pubf(const std::string& name) {
    const auto it = g_published.find(name);
    return it != g_published.end() && it->second->getf ? it->second->getf(it->second->refcon) : -999.0f;
}
std::string pubs(const std::string& name) {
    const auto it = g_published.find(name);
    if (it == g_published.end() || !it->second->getb) return "(none)";
    const int n = it->second->getb(it->second->refcon, nullptr, 0, 0);
    std::string s(static_cast<std::size_t>(std::max(n, 0)), '\0');
    if (n > 0) it->second->getb(it->second->refcon, s.data(), 0, n);
    return s;
}

// The world: a flat patch around 47 N 8 E (no grid convergence), terrain at 400 m.
constexpr double kLat0 = 47, kLon0 = 8, kTerrain = 400;
constexpr double kMetersPerDegLat = 111000, kMetersPerDegLon = 75700;

// Views: 1026 the 3D cockpit, 1017 chase. The camera: a plugin's control callback, or the pilot's head.
XPLMCameraControl_f g_camera = nullptr;
void* g_cameraRefcon = nullptr;
int g_commandsRun = 0;

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

XPLMDataRef XPLMRegisterDataAccessor(const char* name, XPLMDataTypeID type, int, XPLMGetDatai_f geti, XPLMSetDatai_f,
                                     XPLMGetDataf_f getf, XPLMSetDataf_f, XPLMGetDatad_f, XPLMSetDatad_f,
                                     XPLMGetDatavi_f, XPLMSetDatavi_f, XPLMGetDatavf_f, XPLMSetDatavf_f,
                                     XPLMGetDatab_f getb, XPLMSetDatab_f, void* readRefcon, void*) {
    auto* p = new Published{name, type, geti, getf, getb, readRefcon};
    g_published[name] = p;
    return p;
}
void XPLMUnregisterDataAccessor(XPLMDataRef r) {
    for (auto it = g_published.begin(); it != g_published.end(); ++it)
        if (it->second == r) {
            delete it->second;
            g_published.erase(it);
            return;
        }
}

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

XPLMDataRef XPLMFindDataRef(const char* name) { return ref(name); }
int XPLMGetDatai(XPLMDataRef r) {
    if (static_cast<Ref*>(r)->name == "sim/time/paused") return g_paused;
    return static_cast<int>(static_cast<Ref*>(r)->v[0]);
}
float XPLMGetDataf(XPLMDataRef r) { return static_cast<float>(static_cast<Ref*>(r)->v[0]); }
double XPLMGetDatad(XPLMDataRef r) { return static_cast<Ref*>(r)->v[0]; }
void XPLMSetDataf(XPLMDataRef r, float value) {
    auto* ref = static_cast<Ref*>(r);
    ref->v[0] = value;
    // As in X-Plane 12, the aircraft's eyepoint follows the pilot's head.
    static const std::map<std::string, std::string> follows{{"sim/graphics/view/pilots_head_x", "sim/aircraft/view/acf_peX"},
                                                            {"sim/graphics/view/pilots_head_y", "sim/aircraft/view/acf_peY"},
                                                            {"sim/graphics/view/pilots_head_z", "sim/aircraft/view/acf_peZ"}};
    if (const auto it = follows.find(ref->name); it != follows.end()) dr(it->second) = value;
}
int XPLMGetDatavf(XPLMDataRef r, float* values, int offset, int max) {
    const auto& v = static_cast<Ref*>(r)->v;
    if (!values) return static_cast<int>(v.size());
    int n = 0;
    for (int i = offset; i < static_cast<int>(v.size()) && n < max; ++i) values[n++] = static_cast<float>(v[i]);
    return n;
}
XPLMCommandRef XPLMFindCommand(const char* name) { return XPLMCreateCommand(name, ""); }
void XPLMCommandOnce(XPLMCommandRef cmd) {
    const std::string name = static_cast<Command*>(cmd)->name;
    ++g_commandsRun;
    if (name == "sim/view/chase") {
        dr("sim/graphics/view/view_type") = 1017;
        dr("sim/graphics/view/view_is_external") = 1;
    } else if (name == "sim/view/3d_cockpit_cmnd_look") {
        dr("sim/graphics/view/view_type") = 1026;
        dr("sim/graphics/view/view_is_external") = 0;
    }
}
void XPLMWorldToLocal(double lat, double lon, double alt, double* x, double* y, double* z) {
    *x = (lon - kLon0) * kMetersPerDegLon;
    *y = alt;
    *z = -(lat - kLat0) * kMetersPerDegLat;
}
void XPLMLocalToWorld(double x, double y, double z, double* lat, double* lon, double* alt) {
    *lat = kLat0 - z / kMetersPerDegLat;
    *lon = kLon0 + x / kMetersPerDegLon;
    *alt = y;
}
void XPLMControlCamera(XPLMCameraControlDuration, XPLMCameraControl_f f, void* refcon) {
    g_camera = f;
    g_cameraRefcon = refcon;
}
void XPLMDontControlCamera() { g_camera = nullptr; }
int XPLMIsCameraBeingControlled(XPLMCameraControlDuration*) { return g_camera != nullptr; }
// The aircraft is level and heads grid north in this test, so body axes are OpenGL axes (x right, y up, z aft).
void XPLMReadCameraPosition(XPLMCameraPosition_t* p) {
    if (g_camera && g_camera(p, 0, g_cameraRefcon)) return;
    p->x = static_cast<float>(dr("sim/flightmodel/position/local_x") + dr("sim/graphics/view/pilots_head_x"));
    p->y = static_cast<float>(dr("sim/flightmodel/position/local_y") + dr("sim/graphics/view/pilots_head_y"));
    p->z = static_cast<float>(dr("sim/flightmodel/position/local_z") + dr("sim/graphics/view/pilots_head_z"));
    p->heading = static_cast<float>(dr("sim/graphics/view/pilots_head_psi"));
    p->pitch = static_cast<float>(dr("sim/graphics/view/pilots_head_the"));
    p->roll = static_cast<float>(dr("sim/graphics/view/pilots_head_phi"));
    p->zoom = 1;
}
XPLMProbeRef XPLMCreateProbe(XPLMProbeType) { return reinterpret_cast<XPLMProbeRef>(5); }
void XPLMDestroyProbe(XPLMProbeRef) {}
XPLMProbeResult XPLMProbeTerrainXYZ(XPLMProbeRef, float x, float, float z, XPLMProbeInfo_t* info) {
    info->locationX = x;
    info->locationY = static_cast<float>(kTerrain);
    info->locationZ = z;
    return xplm_ProbeHitTerrain;
}
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
void XPLMSetWindowPositioningMode(XPLMWindowID id, XPLMWindowPositioningMode mode, int) { static_cast<FakeWindow*>(id)->mode = mode; }
int XPLMWindowIsPoppedOut(XPLMWindowID id) { return static_cast<FakeWindow*>(id)->mode == xplm_WindowPopOut; }
void XPLMSetWindowTitle(XPLMWindowID id, const char* title) { static_cast<FakeWindow*>(id)->title = title; }
void XPLMSetWindowResizingLimits(XPLMWindowID, int, int, int, int) {}
void XPLMSetWindowIsVisible(XPLMWindowID id, int visible) { static_cast<FakeWindow*>(id)->visible = visible != 0; }
int XPLMGetWindowIsVisible(XPLMWindowID id) { return static_cast<FakeWindow*>(id)->visible; }
void XPLMBringWindowToFront(XPLMWindowID) {}
int g_mouseX = 500, g_mouseY = 500;
void XPLMGetMouseLocationGlobal(int* x, int* y) {
    *x = g_mouseX;
    *y = g_mouseY;
}
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

// A command from X-Plane's keyboard or joystick settings: down, then (unless held) up.
bool command(const std::string& name, XPLMCommandPhase phase) {
    for (Command* c : g_commands)
        if (c->name == name && c->handler) {
            c->handler(c, phase, c->refcon);
            return true;
        }
    return false;
}
bool press(const std::string& name) { return command(name, xplm_CommandBegin) && command(name, xplm_CommandEnd); }

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

    // A Cessna standing level at 47 N 8 E, heading grid north, in the 3D cockpit.
    dr("sim/graphics/view/view_type") = 1026;
    dr("sim/graphics/view/view_is_external") = 0;
    dr("sim/graphics/view/field_of_view_deg") = 70;
    dr("sim/flightmodel/position/local_y") = kTerrain + 1;
    dr("sim/flightmodel/position/latitude") = kLat0;
    dr("sim/flightmodel/position/longitude") = kLon0;
    dr("sim/flightmodel/position/elevation") = kTerrain + 1;
    dr("sim/flightmodel/position/y_agl") = 1;
    dr("sim/flightmodel/failures/onground_any") = 1;
    dr("sim/aircraft/view/acf_peX") = -0.25;
    dr("sim/aircraft/view/acf_peY") = 0.9;
    dr("sim/aircraft/view/acf_peZ") = -0.6;
    dr("sim/graphics/view/pilots_head_x") = -0.25;
    dr("sim/graphics/view/pilots_head_y") = 0.9;
    dr("sim/graphics/view/pilots_head_z") = -0.6;

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
    check(win->params.decorateAsFloatingWindow == xplm_WindowDecorationSelfDecoratedResizable,
          "the page draws its own frame (self-decorated, resizable)");
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

    // Dragging the header: the window follows the mouse until the page ends the drag.
    {
        auto [dragFn, dragRefcon] = win->functions.at("fscsPanelDrag");
        const int left = win->params.left, top = win->params.top;
        dragFn(win, R"({"surface":"main","phase":"start"})", dragRefcon);
        g_mouseX += 40;
        g_mouseY -= 25;
        frames(1);
        check(win->params.left == left + 40 && win->params.top == top - 25, "a header drag moves the editor with the mouse");
        dragFn(win, R"({"surface":"main","phase":"end"})", dragRefcon);
        g_mouseX += 100;
        frames(1);
        check(win->params.left == left + 40, "the window stays put after the drag ends");
    }

    // Pop out: the editor moves into its own desktop window and docks back where it was.
    if (win->functions.count("fscsPanelPopOut")) {
        auto [popFn, popRefcon] = win->functions.at("fscsPanelPopOut");
        const int left = win->params.left, top = win->params.top;
        check(std::string(popFn(win, R"({"surface":"main"})", popRefcon)) == "out" && win->mode == xplm_WindowPopOut, "fscsPanelPopOut pops the editor out");
        check(std::string(popFn(win, R"({"surface":"main"})", popRefcon)) == "in" && win->mode == xplm_WindowPositionFree &&
                  win->params.left == left && win->params.top == top,
              "and docks it back where it was");
    } else {
        check(false, "the editor registers fscsPanelPopOut");
    }

    // Hover mode: the page asks for it, the mini hides until the mouse reaches the bottom edge below it.
    if (mini && mini->functions.count("fscsPanelHover")) {
        auto [hoverFn, hoverRefcon] = mini->functions.at("fscsPanelHover");
        const int midX = (mini->params.left + mini->params.right) / 2;
        g_mouseX = midX;
        g_mouseY = 900;  // well above the dock
        check(std::string(hoverFn(mini, R"({"surface":"mini","autoHide":true})", hoverRefcon)) == "true", "fscsPanelHover accepts hover mode");
        frames(60);
        check(!mini->visible && pubi("xpcs/mini_open") == 1, "a hover mini hides when the mouse is elsewhere, and still counts as open");
        g_mouseY = 2;
        frames(1);
        check(mini->visible, "the mouse at the bottom edge below it brings it up");
        g_mouseY = (mini->params.top + mini->params.bottom) / 2;
        frames(90);
        check(mini->visible, "it stays up while the mouse is over it");
        g_mouseX = 100;  // left of the window
        g_mouseY = 2;
        frames(60);
        check(!mini->visible, "the bottom edge away from it does not bring it up, and it hides after the mouse leaves");
        hoverFn(mini, R"({"surface":"mini","autoHide":false})", hoverRefcon);
        frames(1);
        check(mini->visible, "pinned (autoHide false) it is shown again");
        g_mouseX = 500;
        g_mouseY = 500;
    } else {
        check(false, "the mini window registers fscsPanelHover");
    }

    // Cameras (phase 2). The cockpit camera sits on the default eyepoint: the pilot's head goes there, with its FOV.
    auto state = [&]() {
        page.send("RequestSnapshot");
        return page.find(page.drain(), "Snapshot")["payload"]["ownership"].value("state", "");
    };
    const auto near = [](double a, double b) { return std::abs(a - b) < 1e-3; };
    {
        const std::string cockpitId = cam.value("id", "");
        page.send("ActivateCamera", {{"cameraId", cockpitId}});
        frames(90);
        check(state() == "Active", "activating the cockpit camera takes the camera");
        check(near(dr("sim/graphics/view/pilots_head_x"), -0.25) && near(dr("sim/graphics/view/pilots_head_y"), 0.9) &&
                  near(dr("sim/graphics/view/pilots_head_z"), -0.6) && near(dr("sim/graphics/view/pilots_head_psi"), 0),
              "the cockpit view puts the pilot's head on the eyepoint");
        check(std::abs(dr("sim/graphics/view/field_of_view_deg") - 62.0) < 0.05, "and sets its field of view (62 deg)");
        check(!g_camera && dr("sim/graphics/view/view_type") == 1026, "in X-Plane's 3D cockpit, without camera control");

        // An exterior camera 20 m behind and 3 m above the reference point: X-Plane's chase view, drawn by the plugin.
        json chase = cam;
        chase["id"] = "1b4e28ba-2fa1-41d2-883f-0016d3cca427";
        chase["name"] = "XP chase";
        chase["viewId"] = 2;
        chase["mode"] = "aircraftExterior";
        chase["icon"] = "view-exterior";
        chase["positionMetersFRD"] = json::array({-20.0, 0.0, -3.0});
        chase["requiredCapabilities"] = json::array({"exteriorPose", "fov"});
        const std::string added = page.send("UpdateDraft", {{"ops", json::array({{{"op", "setCamera"}, {"camera", chase}}})}});
        { const json a = page.ack(page.drain(), added); check(a.value("status", "") == "Completed", "an exterior camera is added: " + a.dump()); }
        page.send("ActivateCamera", {{"cameraId", chase["id"]}});
        frames(120);
        XPLMCameraPosition_t cp{};
        XPLMReadCameraPosition(&cp);
        check(g_camera && dr("sim/graphics/view/view_type") == 1017, "the exterior camera switches to an external view and controls the camera");
        check(near(cp.x, 0) && near(cp.y, kTerrain + 1 + 3) && near(cp.z, 20) && near(cp.heading, 0) &&
                  std::abs(cp.pitch + std::atan2(3.0, 20.0) * 180 / M_PI) < 0.01 && near(cp.roll, 0),
              "the camera is 20 m behind and 3 m above the aircraft, looking down at it: " + std::to_string(cp.x) + " " +
                  std::to_string(cp.y) + " " + std::to_string(cp.z) + " hdg " + std::to_string(cp.heading) + " pitch " + std::to_string(cp.pitch) + " roll " + std::to_string(cp.roll));
        check(std::abs(cp.zoom - std::tan(35 * M_PI / 180) / std::tan(31 * M_PI / 180)) < 1e-3, "62 deg through zoom on a 70 deg view");

        // The aircraft turns right 90 deg: the camera stays behind it.
        dr("sim/flightmodel/position/psi") = 90;
        XPLMReadCameraPosition(&cp);
        check(near(cp.x, -20) && near(cp.z, 0) && near(cp.heading, 90), "turning with the aircraft (now 20 m west, heading east)");
        dr("sim/flightmodel/position/psi") = 0;

        // A cockpit view away from the eyepoint (X-Plane 12 moves acf_pe* with the head): the head goes to the
        // eyepoint plus the offset and stays there.
        json side = cam;
        side["id"] = "6f1d2c3b-4a5e-4f60-8172-93a4b5c6d7e8";
        side["name"] = "XP right seat";
        side["viewId"] = 3;
        side["positionMetersFRD"] = json::array({0.3, 0.5, 0.1});
        const std::string sideAdded = page.send("UpdateDraft", {{"ops", json::array({{{"op", "setCamera"}, {"camera", side}}})}});
        { const json a = page.ack(page.drain(), sideAdded); check(a.value("status", "") == "Completed", "a cockpit camera with an offset is added: " + a.dump()); }
        page.send("ActivateCamera", {{"cameraId", side["id"]}});
        frames(120);
        const auto headAt = [&](double x, double y, double z) {
            return near(dr("sim/graphics/view/pilots_head_x"), x) && near(dr("sim/graphics/view/pilots_head_y"), y) &&
                   near(dr("sim/graphics/view/pilots_head_z"), z);
        };
        const std::string headText = std::to_string(dr("sim/graphics/view/pilots_head_x")) + " " +
                                     std::to_string(dr("sim/graphics/view/pilots_head_y")) + " " +
                                     std::to_string(dr("sim/graphics/view/pilots_head_z"));
        check(headAt(0.25, 0.8, -0.9), "an offset cockpit view puts the head 0.5 m right, 0.1 m down, 0.3 m forward: " + headText);
        frames(120);
        check(headAt(0.25, 0.8, -0.9), "and the head stays there (no drift)");

        // Mouse look: a right-button drag on the transparent overlay turns the head; left clicks pass through.
        FakeWindow* overlay = nullptr;
        for (FakeWindow* w : g_windows)
            if (w->params.layer == xplm_WindowLayerFlightOverlay) overlay = w;
        check(overlay && overlay->visible && overlay->params.decorateAsFloatingWindow == xplm_WindowDecorationNone,
              "with a camera active, the mouse-look overlay is up");
        if (overlay) {
            auto right = overlay->params.handleRightClickFunc;
            auto left = overlay->params.handleMouseClickFunc;
            check(left && left(overlay, 500, 500, xplm_MouseDown, overlay->params.refcon) == 0, "left clicks pass through it");
            const double psi0 = dr("sim/graphics/view/pilots_head_psi"), the0 = dr("sim/graphics/view/pilots_head_the");
            check(right && right(overlay, 500, 500, xplm_MouseDown, overlay->params.refcon) == 1, "it takes the right button");
            frames(2);
            for (int i = 1; i <= 10; ++i) {
                right(overlay, 500 + 10 * i, 500 - 5 * i, xplm_MouseDrag, overlay->params.refcon);
                frames(1);
            }
            right(overlay, 600, 450, xplm_MouseUp, overlay->params.refcon);
            frames(30);
            const double psi1 = dr("sim/graphics/view/pilots_head_psi"), the1 = dr("sim/graphics/view/pilots_head_the");
            check(psi1 > psi0 + 3 && the1 < the0 - 1, "dragging right and down looks right and down: psi " + std::to_string(psi0) + " -> " +
                      std::to_string(psi1) + ", pitch " + std::to_string(the0) + " -> " + std::to_string(the1));
            page.send("RecenterLook");
            frames(30);
        }
        page.send("ActivateCamera", {{"cameraId", chase["id"]}});
        frames(120);

        // X-Plane commands and the published datarefs.
        check(pubi("xpcs/camera_active") == 1 && pubi("xpcs/active_view") == 2 && pubs("xpcs/active_camera") == "XP chase" &&
                  std::abs(pubf("xpcs/fov_deg") - 62) < 0.05,
              "xpcs/camera_active, active_view, active_camera and fov_deg describe the chase camera: " +
                  std::to_string(pubi("xpcs/camera_active")) + " " + std::to_string(pubi("xpcs/active_view")) + " '" +
                  pubs("xpcs/active_camera") + "' " + std::to_string(pubf("xpcs/fov_deg")));
        check(pubi("xpcs/editor_open") == 1 && pubi("xpcs/mini_open") == 1 && pubi("xpcs/cinematic") == 0,
              "xpcs/editor_open, mini_open and cinematic");
        const int cockpitView = cam.value("viewId", 0);
        check(cockpitView > 0 && press("xpcs/view_" + std::to_string(cockpitView)), "xpcs/view_" + std::to_string(cockpitView) + " exists");
        frames(120);
        check(pubi("xpcs/active_view") == cockpitView && !g_camera && near(dr("sim/graphics/view/pilots_head_x"), -0.25),
              "xpcs/view_N selects the cockpit view");
        const double psi0 = dr("sim/graphics/view/pilots_head_psi");
        command("xpcs/look_right", xplm_CommandBegin);
        frames(30);
        const double psiHeld = dr("sim/graphics/view/pilots_head_psi");
        command("xpcs/look_right", xplm_CommandEnd);
        frames(60);
        const double psiReleased = dr("sim/graphics/view/pilots_head_psi");
        frames(30);
        check(psiHeld > psi0 + 1 && std::abs(dr("sim/graphics/view/pilots_head_psi") - psiReleased) < 0.5,
              "holding xpcs/look_right turns the head, letting go stops it: " + std::to_string(psi0) + " -> " +
                  std::to_string(psiHeld) + " -> " + std::to_string(psiReleased));
        command("xpcs/peek_2", xplm_CommandBegin);
        frames(120);
        check(pubi("xpcs/active_view") == 2 && g_camera, "holding xpcs/peek_2 shows the chase view");
        command("xpcs/peek_2", xplm_CommandEnd);
        frames(120);
        check(pubi("xpcs/active_view") == cockpitView, "letting go of xpcs/peek_2 returns to the cockpit");
        press("xpcs/view_2");
        frames(120);
        check(pubi("xpcs/active_view") == 2 && g_camera, "xpcs/view_2 selects the chase view again");

        // The user picks X-Plane's 3D cockpit: control goes back to X-Plane and the module observes.
        XPLMCommandOnce(XPLMFindCommand("sim/view/3d_cockpit_cmnd_look"));
        frames(10);
        check(!g_camera && state() == "Observing", "picking another X-Plane view hands the camera back");

        // Back to the cockpit camera, then Release: the pilot's head and FOV return to where they were.
        dr("sim/graphics/view/pilots_head_x") = 0.1;
        page.send("ActivateCamera", {{"cameraId", cockpitId}});
        frames(90);
        page.send("ReleaseCamera");
        frames(30);
        check(state() == "Observing" && near(dr("sim/graphics/view/pilots_head_x"), 0.1) &&
                  std::abs(dr("sim/graphics/view/field_of_view_deg") - 70) < 0.01,
              "Release puts the pilot's head and FOV back");
        check(pubi("xpcs/camera_active") == 0 && pubi("xpcs/active_view") == 0 && pubs("xpcs/active_camera").empty() &&
                  std::abs(pubf("xpcs/fov_deg") - 70) < 0.01,
              "after Release the datarefs show X-Plane's own camera");
        press("xpcs/next_camera");
        frames(120);
        check(pubi("xpcs/camera_active") == 1, "xpcs/next_camera takes the camera again");
        press("xpcs/release_camera");
        frames(30);
        check(state() == "Observing" && pubi("xpcs/camera_active") == 0, "xpcs/release_camera gives it back");
        bool overlayUp = false;
        for (FakeWindow* w : g_windows)
            if (w->params.layer == xplm_WindowLayerFlightOverlay && w->visible) overlayUp = true;
        check(!overlayUp, "and the mouse-look overlay goes away");
        int viewCommands = 0;
        for (Command* c : g_commands)
            if (c->handler && (c->name.rfind("xpcs/view_", 0) == 0 || c->name.rfind("xpcs/peek_", 0) == 0)) ++viewCommands;
        check(viewCommands == 60, "xpcs/view_1..30 and xpcs/peek_1..30 are registered");
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
