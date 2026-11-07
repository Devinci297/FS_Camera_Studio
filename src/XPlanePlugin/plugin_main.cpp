// XP Camera Studio: X-Plane 12 plugin entry points. Phase 0 skeleton: it only proves the plugin builds against SDK
// 4.4.0 and loads. It reports the X-Plane and XPLM versions to Log.txt and does not touch the camera.

#include "XPLMDefs.h"
#include "XPLMPlugin.h"
#include "XPLMUtilities.h"

#include <cstdio>
#include <cstring>

namespace {

// XPLM 4.4.0 ships with X-Plane 12.4.4: the first version with browser windows for the shared UI.
constexpr int kMinXplmVersion = 440;

void copyOut(char* out, const char* text) {
    std::strncpy(out, text, 255);
    out[255] = '\0';
}

void log(const char* text) {
    char line[512];
    std::snprintf(line, sizeof line, "XP Camera Studio: %s\n", text);
    XPLMDebugString(line);
}

}  // namespace

PLUGIN_API int XPluginStart(char* outName, char* outSig, char* outDesc) {
    copyOut(outName, "XP Camera Studio");
    copyOut(outSig, "xpcs.camerastudio");
    copyOut(outDesc, "Camera manager: aircraft profiles, organised views and smooth transitions.");
    XPLMEnableFeature("XPLM_USE_NATIVE_PATHS", 1);

    int xplaneVersion = 0, xplmVersion = 0;
    XPLMHostApplicationID host = 0;
    XPLMGetVersions(&xplaneVersion, &xplmVersion, &host);
    char text[160];
    std::snprintf(text, sizeof text, "started (X-Plane %d, XPLM %d)", xplaneVersion, xplmVersion);
    log(text);
    if (xplmVersion < kMinXplmVersion) {
        log("needs X-Plane 12.4.4 or newer; not starting");
        return 0;
    }
    return 1;
}

PLUGIN_API void XPluginStop(void) { log("stopped"); }

PLUGIN_API int XPluginEnable(void) { return 1; }

PLUGIN_API void XPluginDisable(void) {}

PLUGIN_API void XPluginReceiveMessage(XPLMPluginID inFrom, int inMessage, void* inParam) {
    (void)inFrom;
    (void)inMessage;
    (void)inParam;
}
