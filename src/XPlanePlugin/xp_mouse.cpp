#include "xp_mouse.hpp"

#include <cmath>

namespace fscs::xplane {

namespace be = host::backend_event;

void MouseLook::fitScreen() {
    int left = 0, top = 0, right = 0, bottom = 0;
    XPLMGetScreenBoundsGlobal(&left, &top, &right, &bottom);
    XPLMSetWindowGeometry(window_, left, top, right, bottom);
}

void MouseLook::setEnabled(bool enabled) {
    if (enabled == enabled_) return;
    enabled_ = enabled;
    if (enabled && !window_) {
        XPLMCreateWindow_t p{};
        p.structSize = sizeof(p);
        XPLMGetScreenBoundsGlobal(&p.left, &p.top, &p.right, &p.bottom);
        p.visible = 0;
        p.drawWindowFunc = &MouseLook::onDraw;
        p.handleMouseClickFunc = &MouseLook::onClick;
        p.handleRightClickFunc = &MouseLook::onRightClick;
        p.handleMouseWheelFunc = &MouseLook::onWheel;
        p.handleCursorFunc = &MouseLook::onCursor;
        p.handleKeyFunc = &MouseLook::onKey;
        p.refcon = this;
        p.decorateAsFloatingWindow = xplm_WindowDecorationNone;
        p.layer = xplm_WindowLayerFlightOverlay;  // below the editor and the mini
        window_ = XPLMCreateWindowEx(&p);
    }
    if (!window_) {
        enabled_ = false;
        sink_(be::NativeMouseMapping{false});
        return;
    }
    if (enabled) fitScreen();
    if (!enabled && held_) {
        held_ = false;
        sink_(be::NativeMouseButton{false});
    }
    XPLMSetWindowIsVisible(window_, enabled ? 1 : 0);
    sink_(be::NativeMouseMapping{enabled});
}

void MouseLook::destroy() {
    if (window_) XPLMDestroyWindow(window_);
    window_ = nullptr;
    enabled_ = held_ = false;
}

int MouseLook::onClick(XPLMWindowID, int, int, XPLMMouseStatus, void*) { return 0; }  // left clicks pass through

int MouseLook::onRightClick(XPLMWindowID, int x, int y, XPLMMouseStatus status, void* refcon) {
    auto* self = static_cast<MouseLook*>(refcon);
    if (!self || !self->enabled_) return 0;
    if (status == xplm_MouseDown) {
        self->held_ = true;
        self->lastX_ = x;
        self->lastY_ = y;
        self->sink_(be::NativeMouseButton{true});
        return 1;
    }
    if (!self->held_) return 0;
    const int dx = x - self->lastX_, dy = y - self->lastY_;
    self->lastX_ = x;
    self->lastY_ = y;
    // Screen y grows upwards in X-Plane; the module's mouse y, as in MSFS, grows downwards.
    if (dx) self->sink_(be::NativeMouseAxis{true, static_cast<std::int32_t>(std::lround(dx * kCountsPerBoxel))});
    if (dy) self->sink_(be::NativeMouseAxis{false, static_cast<std::int32_t>(std::lround(-dy * kCountsPerBoxel))});
    if (status == xplm_MouseUp) {
        self->held_ = false;
        self->sink_(be::NativeMouseButton{false});
    }
    return 1;
}

}  // namespace fscs::xplane
