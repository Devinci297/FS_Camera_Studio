// XP Camera Studio: X-Plane 12 plugin entry points (phase 1). The same module logic as the MSFS WASM module
// (ModuleApp), driven by an X-Plane flight loop, with profiles stored as files and the shared HTML UI in X-Plane
// browser windows. Phase 1 never moves the camera: the coordinator stays in Observing.

#include "module_app.hpp"
#include "xp_backends.hpp"
#include "xp_panels.hpp"
#include "xp_storage.hpp"
#include "fscs/profiles/profile.hpp"

#include <XPLMDataAccess.h>
#include <XPLMDefs.h>
#include <XPLMMenus.h>
#include <XPLMPlanes.h>
#include <XPLMPlugin.h>
#include <XPLMProcessing.h>
#include <XPLMUtilities.h>

#include <nlohmann/json.hpp>

#include <cstdio>
#include <deque>
#include <filesystem>
#include <memory>
#include <string>
#include <utility>

#ifndef XPCS_VERSION
#define XPCS_VERSION "0.0.0"
#endif

namespace {

using namespace fscs;
using host::BackendEvent;
namespace be = host::backend_event;
namespace fs = std::filesystem;

// XPLM 4.4.0 ships with X-Plane 12.4.4: the first version with browser windows for the shared UI.
constexpr int kMinXplmVersion = 440;
constexpr double kAircraftCheckSeconds = 1.0;

void log(const std::string& text) { XPLMDebugString(("XP Camera Studio: " + text + "\n").c_str()); }

void copyOut(char* out, const char* text) {
    std::snprintf(out, 256, "%s", text);  // X-Plane passes 256-byte buffers
}

std::string bootId() {
    unsigned char b[12] = {};
    profiles::randomBytes(b, sizeof(b));
    static const char* hex = "0123456789abcdef";
    std::string s;
    for (unsigned char c : b) {
        s.push_back(hex[c >> 4]);
        s.push_back(hex[c & 15]);
    }
    return s;
}

std::string toUtf8(const fs::path& p) {
    const std::u8string s = p.u8string();
    return std::string(s.begin(), s.end());
}

// The folder of a file path X-Plane reports (native, UTF-8).
fs::path folderOf(const char* path) { return xplane::pathFromUtf8(path).parent_path(); }

struct Host {
    xplane::XPCamera camera;
    xplane::XPServices services;
    std::unique_ptr<xplane::XPStorage> storage;
    xplane::PanelWindows panels;
    std::unique_ptr<host::ModuleApp> app;
    std::deque<std::pair<xplane::PanelWindows::Surface, std::string>> inbound;
    fs::path xplaneRoot;
    double now = 0;
    double nextAircraftCheck = 0;
    bool aircraftDirty = true;
    std::string aircraftPath, aircraftTitle;
    int paused = -1;
    XPLMDataRef pausedRef = nullptr, uiNameRef = nullptr;
};

Host* g = nullptr;
XPLMFlightLoopID g_loop = nullptr;
XPLMCommandRef g_toggleEditor = nullptr, g_toggleMini = nullptr;
XPLMMenuID g_menu = nullptr;
std::string g_pluginFolder;  // the XPCameraStudio folder (UTF-8), holding win_x64/ and ui/

void deliver(const BackendEvent& ev) {
    if (g && g->app) g->app->onEvent(ev, g->now);
}

// The user aircraft, as a path relative to the X-Plane folder with forward slashes ("Aircraft/Laminar Research/
// Cessna 172 SP/Cessna_172SP.acf"): the coordinator keys profiles by its folder, so liveries share one profile.
void checkAircraft() {
    char file[512] = {}, path[1024] = {};
    XPLMGetNthAircraftModel(XPLM_USER_AIRCRAFT, file, path);
    std::string rel = toUtf8(xplane::pathFromUtf8(path).lexically_relative(g->xplaneRoot));
    if (rel.empty() || rel.rfind("..", 0) == 0) rel = path;
    for (char& c : rel)
        if (c == '\\') c = '/';
    if (rel.empty()) return;
    if (rel != g->aircraftPath) {
        g->aircraftPath = rel;
        deliver(be::AircraftLoaded{rel});
    }
    std::string title;
    if (g->uiNameRef) {
        char name[260] = {};
        const int n = XPLMGetDatab(g->uiNameRef, name, 0, sizeof(name) - 1);
        if (n > 0) title.assign(name, static_cast<std::size_t>(n));
        title = title.c_str();  // stop at the first NUL
    }
    if (title.empty()) {
        title = file;
        if (title.size() > 4 && title.compare(title.size() - 4, 4, ".acf") == 0) title.resize(title.size() - 4);
    }
    if (!title.empty() && title != g->aircraftTitle) {
        g->aircraftTitle = title;
        deliver(be::AircraftTitle{title});
    }
}

float flightLoop(float elapsedSinceLastCall, float, int, void*) {
    if (!g || !g->app) return -1.0f;
    if (elapsedSinceLastCall > 0 && elapsedSinceLastCall < 1.0f) g->now += elapsedSinceLastCall;
    else g->now += 1.0 / 60.0;  // first frame or a stall: never fast-forward

    while (!g->inbound.empty()) {
        auto [surface, frame] = std::move(g->inbound.front());
        g->inbound.pop_front();
        deliver(be::CommBusMessage{frame});
    }
    if (g->aircraftDirty || g->now >= g->nextAircraftCheck) {
        g->aircraftDirty = false;
        g->nextAircraftCheck = g->now + kAircraftCheckSeconds;
        checkAircraft();
    }
    if (g->pausedRef) {
        const int p = XPLMGetDatai(g->pausedRef) != 0;
        if (p != g->paused) {
            g->paused = p;
            deliver(be::Paused{p != 0});
        }
    }
    std::vector<BackendEvent> events;
    g->services.dispatch([&](BackendEvent e) { events.push_back(std::move(e)); });
    g->camera.dispatch([&](BackendEvent e) { events.push_back(std::move(e)); });
    for (const auto& e : events) deliver(e);
    g->app->update(g->now);
    g->panels.flush();
    return -1.0f;  // every frame
}

// TextFocus is the page telling the module a text field has focus: also give that window X-Plane's keyboard.
void onInbound(xplane::PanelWindows::Surface surface, std::string frame) {
    if (!g) return;
    if (frame.size() < 2000 && frame.find("\"TextFocus\"") != std::string::npos) {
        const auto msg = nlohmann::json::parse(frame, nullptr, false);
        if (msg.is_object() && msg.value("action", std::string()) == "TextFocus") {
            const auto& payload = msg.contains("payload") ? msg["payload"] : nlohmann::json();
            g->panels.keyboard(surface, payload.is_object() && payload.value("focused", false));
        }
    }
    g->inbound.emplace_back(surface, std::move(frame));
}

int onCommand(XPLMCommandRef cmd, XPLMCommandPhase phase, void*) {
    if (phase != xplm_CommandBegin || !g) return 0;
    if (cmd == g_toggleEditor) g->panels.toggle(xplane::PanelWindows::Main);
    else if (cmd == g_toggleMini) g->panels.toggle(xplane::PanelWindows::Mini);
    return 0;
}

bool startHost() {
    g = new Host();
    char systemPath[1024] = {}, prefsPath[1024] = {};
    XPLMGetSystemPath(systemPath);
    XPLMGetPrefsPath(prefsPath);
    g->xplaneRoot = xplane::pathFromUtf8(systemPath);
    const fs::path storageRoot = folderOf(prefsPath) / "XPCameraStudio";
    g->storage = std::make_unique<xplane::XPStorage>(storageRoot);
    log("profiles folder " + toUtf8(storageRoot));

    g->pausedRef = XPLMFindDataRef("sim/time/paused");
    g->uiNameRef = XPLMFindDataRef("sim/aircraft/view/acf_ui_name");

    const std::string index = toUtf8(xplane::pathFromUtf8(g_pluginFolder) / "ui" / "index.html");
    g->panels.init(xplane::fileUrl(index), &onInbound);
    g->services.toPanels = [](const std::string& text) { if (g) g->panels.post(text); };

    g->app = std::make_unique<host::ModuleApp>(g->camera, g->services, *g->storage, bootId(), XPCS_VERSION);
    g->app->start(0.0);
    int xplaneVersion = 0, xplmVersion = 0;
    XPLMHostApplicationID hostId = 0;
    XPLMGetVersions(&xplaneVersion, &xplmVersion, &hostId);
    deliver(be::Connected{"X-Plane " + std::to_string(xplaneVersion)});
    deliver(be::SimRunning{true});
    return true;
}

void stopHost() {
    if (!g) return;
    if (g->app) g->app->shutdown(g->now);
    g->panels.destroy();
    delete g;
    g = nullptr;
}

}  // namespace

PLUGIN_API int XPluginStart(char* outName, char* outSig, char* outDesc) {
    copyOut(outName, "XP Camera Studio");
    copyOut(outSig, "xpcs.camerastudio");
    copyOut(outDesc, "Camera manager: aircraft profiles, organised views and smooth transitions.");
    XPLMEnableFeature("XPLM_USE_NATIVE_PATHS", 1);

    int xplaneVersion = 0, xplmVersion = 0;
    XPLMHostApplicationID hostId = 0;
    XPLMGetVersions(&xplaneVersion, &xplmVersion, &hostId);
    log("started (X-Plane " + std::to_string(xplaneVersion) + ", XPLM " + std::to_string(xplmVersion) + ", XPCS " +
        XPCS_VERSION + ")");
    if (xplmVersion < kMinXplmVersion) {
        log("needs X-Plane 12.4.4 or newer; not starting");
        return 0;
    }

    char pluginPath[1024] = {};
    XPLMGetPluginInfo(XPLMGetMyID(), nullptr, pluginPath, nullptr, nullptr);
    g_pluginFolder = toUtf8(folderOf(pluginPath).parent_path());  // .../XPCameraStudio/win_x64/x.xpl -> XPCameraStudio

    g_toggleEditor = XPLMCreateCommand("xpcs/toggle_editor", "XP Camera Studio: show or hide the editor");
    g_toggleMini = XPLMCreateCommand("xpcs/toggle_mini", "XP Camera Studio: show or hide the mini panel");
    XPLMRegisterCommandHandler(g_toggleEditor, &onCommand, 1, nullptr);
    XPLMRegisterCommandHandler(g_toggleMini, &onCommand, 1, nullptr);

    const int item = XPLMAppendMenuItem(XPLMFindPluginsMenu(), "XP Camera Studio", nullptr, 0);
    g_menu = XPLMCreateMenu("XP Camera Studio", XPLMFindPluginsMenu(), item, nullptr, nullptr);
    XPLMAppendMenuItemWithCommand(g_menu, "Editor", g_toggleEditor);
    XPLMAppendMenuItemWithCommand(g_menu, "Mini panel", g_toggleMini);
    return 1;
}

PLUGIN_API void XPluginStop(void) {
    if (g_toggleEditor) XPLMUnregisterCommandHandler(g_toggleEditor, &onCommand, 1, nullptr);
    if (g_toggleMini) XPLMUnregisterCommandHandler(g_toggleMini, &onCommand, 1, nullptr);
    if (g_menu) XPLMDestroyMenu(g_menu);
    g_menu = nullptr;
    log("stopped");
}

PLUGIN_API int XPluginEnable(void) {
    if (!startHost()) return 0;
    XPLMCreateFlightLoop_t loop{};
    loop.structSize = sizeof(loop);
    loop.phase = xplm_FlightLoop_Phase_AfterFlightModel;
    loop.callbackFunc = &flightLoop;
    g_loop = XPLMCreateFlightLoop(&loop);
    XPLMScheduleFlightLoop(g_loop, -1.0f, 1);
    return 1;
}

PLUGIN_API void XPluginDisable(void) {
    if (g_loop) XPLMDestroyFlightLoop(g_loop);
    g_loop = nullptr;
    stopHost();
}

PLUGIN_API void XPluginReceiveMessage(XPLMPluginID, int inMessage, void* inParam) {
    if (!g) return;
    if (inMessage == XPLM_MSG_PLANE_LOADED && reinterpret_cast<intptr_t>(inParam) == XPLM_USER_AIRCRAFT)
        g->aircraftDirty = true;
    else if (inMessage == XPLM_MSG_AIRPORT_LOADED)
        deliver(be::FlightLoaded{});
}
