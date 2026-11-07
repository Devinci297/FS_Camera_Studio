// XP Camera Studio: X-Plane 12 plugin entry points. The same module logic as the MSFS WASM module (ModuleApp),
// driven by an X-Plane flight loop, with profiles stored as files, the shared HTML UI in X-Plane browser windows and
// the camera through the pilot's head datarefs and XPLMControlCamera (xp_camera.hpp).

#include "module_app.hpp"
#include "xp_airports.hpp"
#include "xp_backends.hpp"
#include "xp_camera.hpp"
#include "xp_geometry.hpp"
#include "xp_mouse.hpp"
#include "xp_panels.hpp"
#include "xp_storage.hpp"
#include "xp_traffic.hpp"
#include "fscs/profiles/profile.hpp"

#include <XPLMDataAccess.h>
#include <XPLMDefs.h>
#include <XPLMMenus.h>
#include <XPLMPlanes.h>
#include <XPLMPlugin.h>
#include <XPLMProcessing.h>
#include <XPLMUtilities.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdio>
#include <cstring>
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
constexpr double kGroundCheckSeconds = 0.25;

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
    std::unique_ptr<xplane::MouseLook> mouse;
    xplane::apt::Database airports;
    bool airportsLogged = false;
    std::unique_ptr<xplane::TrafficSource> traffic;
    std::unique_ptr<host::ModuleApp> app;
    std::deque<std::pair<xplane::PanelWindows::Surface, std::string>> inbound;
    fs::path xplaneRoot;
    double now = 0;
    double nextAircraftCheck = 0;
    bool aircraftDirty = true;
    std::string aircraftPath, aircraftTitle;
    int paused = -1;
    XPLMDataRef pausedRef = nullptr, uiNameRef = nullptr;
    double nextGroundCheck = 0;
    // Telemetry for camera effects, free cameras and spotting (the MSFS module reads the same from SimConnect).
    struct Telemetry {
        XPLMDataRef lat = nullptr, lon = nullptr, elevation = nullptr, truePsi = nullptr, theta = nullptr, phi = nullptr;
        XPLMDataRef onGround = nullptr, groundSpeed = nullptr, verticalSpeed = nullptr, agl = nullptr;
        XPLMDataRef gNormal = nullptr, rpm = nullptr, psi = nullptr;
        XPLMDataRef accel[3] = {};  // local_ax/ay/az: the aircraft's acceleration in OpenGL axes, m/s^2
    } tel;
};

Host* g = nullptr;
XPLMFlightLoopID g_loop = nullptr;
XPLMCommandRef g_toggleEditor = nullptr, g_toggleMini = nullptr;
XPLMMenuID g_menu = nullptr;

// xpcs/ commands: every binding action, so X-Plane's keyboard and joystick settings can drive the module. View
// commands take the view ID in their name (xpcs/view_3 selects view 3, xpcs/peek_3 shows it while held).
using profiles::Action;
struct CommandSpec {
    Action action;
    const char* name;
    const char* description;
};
constexpr CommandSpec kCommands[] = {
    {Action::NextCamera, "next_camera", "Next camera"},
    {Action::PreviousCamera, "previous_camera", "Previous camera"},
    {Action::NextCategory, "next_category", "Next category"},
    {Action::ReturnToPilot, "return_to_pilot", "Return to the pilot's view"},
    {Action::ReleaseCamera, "release_camera", "Give the camera back to X-Plane"},
    {Action::PanicRelease, "panic_release", "Stop everything and give the camera back to X-Plane"},
    {Action::RecenterLook, "recenter_look", "Recenter the look"},
    {Action::ToggleMouseLook, "toggle_mouse_look", "Toggle mouse look"},
    {Action::LookLeft, "look_left", "Look left (hold)"},
    {Action::LookRight, "look_right", "Look right (hold)"},
    {Action::LookUp, "look_up", "Look up (hold)"},
    {Action::LookDown, "look_down", "Look down (hold)"},
    {Action::MoveForward, "move_forward", "Move forward (hold)"},
    {Action::MoveBack, "move_back", "Move back (hold)"},
    {Action::MoveLeft, "move_left", "Move left (hold)"},
    {Action::MoveRight, "move_right", "Move right (hold)"},
    {Action::MoveUp, "move_up", "Move up (hold)"},
    {Action::MoveDown, "move_down", "Move down (hold)"},
    {Action::MoveFast, "move_fast", "Move faster (hold)"},
    {Action::ZoomIn, "zoom_in", "Zoom in (hold)"},
    {Action::ZoomOut, "zoom_out", "Zoom out (hold)"},
    {Action::Binoculars, "binoculars", "Binoculars (hold)"},
    {Action::GlanceHold, "glance_hold", "Glance at an instrument (hold)"},
    {Action::GlanceNext, "glance_next", "Glance at the next instrument"},
    {Action::GlanceReturn, "glance_return", "Return from a glance"},
    {Action::ToggleHeadTracking, "toggle_head_tracking", "Toggle head tracking"},
    {Action::ToggleAirportAuto, "toggle_airport_auto", "Toggle automatic airport cameras"},
    {Action::ToggleCinematic, "toggle_cinematic", "Toggle cinematic mode"},
    {Action::SpotNext, "spot_next", "Spot the next aircraft"},
    {Action::SpotPrevious, "spot_previous", "Spot the previous aircraft"},
    {Action::SpotReturn, "spot_return", "Stop spotting"},
    {Action::ToggleAutoSpot, "toggle_auto_spot", "Toggle automatic spotting"},
    {Action::SaveViews, "save_views", "Save all changes to the views"},
};
constexpr int kViewCommands = 30;  // xpcs/view_1..30 and xpcs/peek_1..30

struct BoundCommand {
    XPLMCommandRef ref = nullptr;
    Action action = Action::NextCamera;
    int viewId = 0;
};
std::deque<BoundCommand> g_commands;  // stable addresses: each is its handler's refcon

int onActionCommand(XPLMCommandRef, XPLMCommandPhase phase, void* refcon) {
    const auto* c = static_cast<const BoundCommand*>(refcon);
    if (!g || !g->app || phase == xplm_CommandContinue) return 0;
    g->app->hostAction(c->action, c->viewId, phase == xplm_CommandBegin, g->now);
    return 0;
}

void addCommand(Action action, int viewId, const std::string& name, const std::string& description) {
    BoundCommand& c = g_commands.emplace_back();
    c.ref = XPLMCreateCommand(("xpcs/" + name).c_str(), ("XP Camera Studio: " + description).c_str());
    c.action = action;
    c.viewId = viewId;
    XPLMRegisterCommandHandler(c.ref, &onActionCommand, 1, &c);
}

void registerCommands() {
    for (const auto& s : kCommands) addCommand(s.action, 0, s.name, s.description);
    for (int v = 1; v <= kViewCommands; ++v) {
        const std::string n = std::to_string(v);
        addCommand(Action::SelectView, v, "view_" + n, "Select view " + n);
        addCommand(Action::PeekView, v, "peek_" + n, "Show view " + n + " while held");
    }
}

void unregisterCommands() {
    for (auto& c : g_commands) XPLMUnregisterCommandHandler(c.ref, &onActionCommand, 1, &c);
    g_commands.clear();
}

// Published xpcs/ datarefs (read only), for cockpit builders, FlyWithLua and the like.
enum class Published { CameraActive, ActiveView, ActiveCamera, FovDeg, Cinematic, AutoSpot, EditorOpen, MiniOpen };
struct PublishedRef {
    Published what;
    const char* name;
    XPLMDataTypeID type;
    XPLMDataRef ref = nullptr;
};
PublishedRef g_published[] = {
    {Published::CameraActive, "xpcs/camera_active", xplmType_Int},
    {Published::ActiveView, "xpcs/active_view", xplmType_Int},
    {Published::ActiveCamera, "xpcs/active_camera", xplmType_Data},
    {Published::FovDeg, "xpcs/fov_deg", xplmType_Float},
    {Published::Cinematic, "xpcs/cinematic", xplmType_Int},
    {Published::AutoSpot, "xpcs/auto_spot", xplmType_Int},
    {Published::EditorOpen, "xpcs/editor_open", xplmType_Int},
    {Published::MiniOpen, "xpcs/mini_open", xplmType_Int},
};

// The camera the module shows now, or none while X-Plane has the camera.
const profiles::CameraEntry* activeCamera() {
    if (!g || !g->app) return nullptr;
    const auto& c = g->app->coordinator();
    if (c.ownership() != host::Ownership::Active || !c.profile()) return nullptr;
    return c.profile()->camera(c.activeCameraId());
}

int readInt(void* refcon) {
    if (!g || !g->app) return 0;
    switch (static_cast<const PublishedRef*>(refcon)->what) {
        case Published::CameraActive: return activeCamera() ? 1 : 0;
        case Published::ActiveView: {
            const auto* cam = activeCamera();
            return cam ? cam->viewId : 0;
        }
        case Published::Cinematic: return g->app->cinematicEnabled() ? 1 : 0;
        case Published::AutoSpot: return g->app->autoSpotEnabled() ? 1 : 0;
        case Published::EditorOpen: return g->panels.visible(xplane::PanelWindows::Main) ? 1 : 0;
        case Published::MiniOpen: return g->panels.visible(xplane::PanelWindows::Mini) ? 1 : 0;
        default: return 0;
    }
}

float readFloat(void*) { return g ? static_cast<float>(g->camera.fovDeg()) : 0.0f; }

// The active camera's name, UTF-8 without a terminator; a null buffer asks for the length.
int readName(void*, void* out, int offset, int max) {
    const auto* cam = activeCamera();
    const std::string name = cam ? cam->name : std::string();
    const int size = static_cast<int>(name.size());
    if (!out) return size;
    if (offset < 0 || offset >= size || max <= 0) return 0;
    const int n = std::min(max, size - offset);
    std::memcpy(out, name.data() + offset, static_cast<std::size_t>(n));
    return n;
}

void registerDatarefs() {
    for (auto& p : g_published) {
        const bool isInt = p.type == xplmType_Int, isFloat = p.type == xplmType_Float, isData = p.type == xplmType_Data;
        p.ref = XPLMRegisterDataAccessor(p.name, p.type, 0,                         // read only
                                         isInt ? &readInt : nullptr, nullptr,          // int
                                         isFloat ? &readFloat : nullptr, nullptr,      // float
                                         nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,  // double, arrays
                                         isData ? &readName : nullptr, nullptr, &p, &p);
    }
}

void unregisterDatarefs() {
    for (auto& p : g_published)
        if (p.ref) XPLMUnregisterDataAccessor(p.ref);
    for (auto& p : g_published) p.ref = nullptr;
}
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
        g->camera.aircraftChanged();
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

void findTelemetry(Host::Telemetry& t) {
    t.lat = XPLMFindDataRef("sim/flightmodel/position/latitude");
    t.lon = XPLMFindDataRef("sim/flightmodel/position/longitude");
    t.elevation = XPLMFindDataRef("sim/flightmodel/position/elevation");
    t.truePsi = XPLMFindDataRef("sim/flightmodel/position/true_psi");
    t.theta = XPLMFindDataRef("sim/flightmodel/position/theta");
    t.phi = XPLMFindDataRef("sim/flightmodel/position/phi");
    t.onGround = XPLMFindDataRef("sim/flightmodel/failures/onground_any");
    t.groundSpeed = XPLMFindDataRef("sim/flightmodel/position/groundspeed");
    t.verticalSpeed = XPLMFindDataRef("sim/flightmodel/position/vh_ind");
    t.agl = XPLMFindDataRef("sim/flightmodel/position/y_agl");
    t.gNormal = XPLMFindDataRef("sim/flightmodel/forces/g_nrml");
    t.psi = XPLMFindDataRef("sim/flightmodel/position/psi");
    t.accel[0] = XPLMFindDataRef("sim/flightmodel/position/local_ax");
    t.accel[1] = XPLMFindDataRef("sim/flightmodel/position/local_ay");
    t.accel[2] = XPLMFindDataRef("sim/flightmodel/position/local_az");
    t.rpm = XPLMFindDataRef("sim/cockpit2/engine/indicators/engine_speed_rpm");
}

float f(XPLMDataRef r, float fallback = 0) { return r ? XPLMGetDataf(r) : fallback; }
double d(XPLMDataRef r, double fallback = 0) { return r ? XPLMGetDatad(r) : fallback; }

// Every frame: where the aircraft is and how it moves. Signs as the module expects: pitch nose up, bank right wing
// down, accelerations forward and right in m/s^2.
void publishTelemetry() {
    const auto& t = g->tel;
    if (!t.lat || !t.lon) return;
    const double lat = d(t.lat), lon = d(t.lon), elevation = d(t.elevation);
    deliver(be::AircraftPosition{lat, lon, elevation, f(t.truePsi)});
    g->services.setPosition(lat, lon);
    host::backend_event::MotionTelemetry m;
    m.motion.onGround = t.onGround && XPLMGetDatai(t.onGround) != 0 ? 1 : 0;
    m.motion.groundSpeed = f(t.groundSpeed);
    m.motion.verticalSpeed = f(t.verticalSpeed);
    m.motion.gForce = f(t.gNormal, 1);
    // As MSFS's ACCELERATION BODY Z/X: the aircraft's own acceleration along its nose and right wing (no gravity).
    const core::Vec3 accelNed = xplane::geo::glToNed({f(t.accel[0]), f(t.accel[1]), f(t.accel[2])});
    const core::Vec3 accelBody = xplane::geo::attitude(f(t.theta), f(t.phi), f(t.psi)).conjugate().rotate(accelNed);
    m.motion.forwardAcceleration = accelBody.x;
    m.motion.lateralAcceleration = accelBody.y;
    float rpm = 0;
    if (t.rpm) XPLMGetDatavf(t.rpm, &rpm, 0, 1);
    m.motion.engineRpm = rpm;
    m.pitchDeg = f(t.theta);
    m.bankDeg = f(t.phi);
    if (m.motion.finite()) deliver(m);
    if (t.agl && t.elevation && g->now >= g->nextGroundCheck) {
        g->nextGroundCheck = g->now + kGroundCheckSeconds;
        deliver(be::GroundElevation{elevation - f(t.agl)});
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
    publishTelemetry();
    if (!g->airportsLogged && g->airports.ready()) {
        g->airportsLogged = true;
        char took[32];
        std::snprintf(took, sizeof(took), "%.1f", g->airports.seconds());
        log("airports: " + std::to_string(g->airports.airports()) + " from " + std::to_string(g->airports.files()) +
            " apt.dat files in " + took + " s");
    }
    g->traffic->frame();
    std::vector<BackendEvent> events;
    g->services.dispatch([&](BackendEvent e) { events.push_back(std::move(e)); });
    g->camera.dispatch([&](BackendEvent e) { events.push_back(std::move(e)); });
    for (const auto& e : events) deliver(e);
    g->app->update(g->now);
    g->camera.frame();
    g->panels.update(g->now);
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
    g->storage->setXPlane(g->xplaneRoot, [] { return g ? g->aircraftPath : std::string(); });
    log("profiles folder " + toUtf8(storageRoot));

    g->pausedRef = XPLMFindDataRef("sim/time/paused");
    g->uiNameRef = XPLMFindDataRef("sim/aircraft/view/acf_ui_name");
    findTelemetry(g->tel);

    const std::string index = toUtf8(xplane::pathFromUtf8(g_pluginFolder) / "ui" / "index.html");
    g->panels.init(xplane::fileUrl(index), &onInbound);
    g->services.toPanels = [](const std::string& text) { if (g) g->panels.post(text); };
    g->mouse = std::make_unique<xplane::MouseLook>([](BackendEvent e) { if (g) g->services.stage(std::move(e)); });
    g->services.onNativeMouse = [](bool on) { if (g && g->mouse) g->mouse->setEnabled(on); };
    // apt.dat is indexed in the background: nearby airports are answered once it is done.
    g->airports.start(g->xplaneRoot);
    g->services.airports = &g->airports;
    g->services.log = [](const std::string& text) { log(text); };
    g->traffic = std::make_unique<xplane::TrafficSource>([](BackendEvent e) { if (g) g->services.stage(std::move(e)); });
    g->services.traffic = g->traffic.get();

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
    g->services.onNativeMouse = nullptr;
    g->services.airports = nullptr;
    g->services.traffic = nullptr;
    g->airports.stop();
    if (g->traffic) g->traffic->destroy();
    if (g->mouse) g->mouse->destroy();
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
    registerCommands();
    registerDatarefs();

    const int item = XPLMAppendMenuItem(XPLMFindPluginsMenu(), "XP Camera Studio", nullptr, 0);
    g_menu = XPLMCreateMenu("XP Camera Studio", XPLMFindPluginsMenu(), item, nullptr, nullptr);
    XPLMAppendMenuItemWithCommand(g_menu, "Editor", g_toggleEditor);
    XPLMAppendMenuItemWithCommand(g_menu, "Mini panel", g_toggleMini);
    return 1;
}

PLUGIN_API void XPluginStop(void) {
    if (g_toggleEditor) XPLMUnregisterCommandHandler(g_toggleEditor, &onCommand, 1, nullptr);
    if (g_toggleMini) XPLMUnregisterCommandHandler(g_toggleMini, &onCommand, 1, nullptr);
    unregisterCommands();
    unregisterDatarefs();
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
        g->aircraftDirty = true, g->camera.aircraftChanged();
    else if (inMessage == XPLM_MSG_AIRPORT_LOADED)
        deliver(be::FlightLoaded{});
}
