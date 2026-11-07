#pragma once
// XP Camera Studio: mouse look, as in MSFS: while the module shows one of its cameras, dragging with the right mouse
// button turns the view. A transparent full-screen window under the panels (the flight overlay layer) takes only
// right-button drags; left clicks, the wheel and the cursor pass through to the 3D cockpit and other windows. The
// drags become the module's native mouse events (button, then relative axis counts).

#include "backend.hpp"

#include <XPLMDisplay.h>

#include <functional>

namespace fscs::xplane {

class MouseLook {
public:
    // Axis counts per boxel: the module turns 6 deg per 32768 counts at sensitivity 1, so this is about 0.15 deg.
    static constexpr double kCountsPerBoxel = 820;

    explicit MouseLook(std::function<void(host::BackendEvent)> sink) : sink_(std::move(sink)) {}
    ~MouseLook() { destroy(); }
    void setEnabled(bool enabled);
    bool enabled() const { return enabled_; }
    void destroy();

private:
    static int onClick(XPLMWindowID, int x, int y, XPLMMouseStatus status, void* refcon);
    static int onRightClick(XPLMWindowID, int x, int y, XPLMMouseStatus status, void* refcon);
    static int onWheel(XPLMWindowID, int, int, int, int, void*) { return 0; }
    static XPLMCursorStatus onCursor(XPLMWindowID, int, int, void*) { return xplm_CursorDefault; }
    static void onKey(XPLMWindowID, char, XPLMKeyFlags, char, void*, int) {}
    static void onDraw(XPLMWindowID, void*) {}
    void fitScreen();

    std::function<void(host::BackendEvent)> sink_;
    XPLMWindowID window_ = nullptr;
    bool enabled_ = false, held_ = false;
    int lastX_ = 0, lastY_ = 0;
};

}  // namespace fscs::xplane
