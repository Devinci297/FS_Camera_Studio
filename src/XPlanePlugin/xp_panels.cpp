#include "xp_panels.hpp"

#include <XPLMUtilities.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdio>

namespace fscs::xplane {

using json = nlohmann::json;

std::string fileUrl(const std::string& utf8Path) {
    static const char* hex = "0123456789ABCDEF";
    std::string out = "file:///";
    std::size_t start = 0;
    while (start < utf8Path.size() && (utf8Path[start] == '/' || utf8Path[start] == '\\')) ++start;
    for (std::size_t i = start; i < utf8Path.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(utf8Path[i]);
        if (c == '\\') out.push_back('/');
        else if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_' ||
                 c == '.' || c == '~' || c == '/' || c == ':')
            out.push_back(static_cast<char>(c));
        else {
            out.push_back('%');
            out.push_back(hex[c >> 4]);
            out.push_back(hex[c & 15]);
        }
    }
    return out;
}

namespace {
void log(const std::string& text) { XPLMDebugString(("XP Camera Studio: " + text + "\n").c_str()); }
const char* kSurfaceName[2] = {"main", "mini"};
}  // namespace

void PanelWindows::init(std::string uiIndexUrl, std::function<void(Surface, std::string)> inbound) {
    url_ = std::move(uiIndexUrl);
    inbound_ = std::move(inbound);
    for (int i = 0; i < 2; ++i) {
        wins_[i].owner = this;
        wins_[i].surface = static_cast<Surface>(i);
    }
}

void PanelWindows::create(Surface s) {
    Win& w = wins_[s];
    if (w.id) return;
    int left = 0, top = 0, right = 0, bottom = 0;
    XPLMGetScreenBoundsGlobal(&left, &top, &right, &bottom);
    const int width = right - left, height = top - bottom;

    XPLMCreateWindow_t p{};
    p.structSize = sizeof(p);
    if (s == Main) {
        // The MSFS editor panel's default: 78% x 86% of the screen, 7% from the top, 11% from the right.
        p.right = right - width * 11 / 100;
        p.left = p.right - width * 78 / 100;
        p.top = top - height * 7 / 100;
        p.bottom = p.top - height * 86 / 100;
    } else {
        // The dock: a wide strip centred above the bottom edge.
        const int w2 = width * 80 / 100;
        p.left = left + (width - w2) / 2;
        p.right = p.left + w2;
        p.bottom = bottom + height * 4 / 100;
        p.top = p.bottom + 140;  // the page then fits it to the dock (fscsPanelSize)
    }
    p.visible = 0;
    p.drawWindowFunc = nullptr;  // browser content draws itself
    p.refcon = &w;
    // The page draws its own frame (rounded editor panel, transparent dock), so X-Plane draws none; the editor keeps
    // X-Plane's resize handles.
    p.decorateAsFloatingWindow = s == Main ? xplm_WindowDecorationSelfDecoratedResizable : xplm_WindowDecorationSelfDecorated;
    p.layer = xplm_WindowLayerFloatingWindows;
    p.contentType = xplm_WindowContentTypeBrowser;
    p.browserLoadFinishedFunc = &PanelWindows::onLoaded;
    p.browserLoadErrorFunc = &PanelWindows::onLoadError;
    w.id = XPLMCreateWindowEx(&p);
    if (!w.id) {
        log(std::string("could not create the ") + kSurfaceName[s] + " window");
        return;
    }
    XPLMSetWindowPositioningMode(w.id, xplm_WindowPositionFree, -1);
    XPLMSetWindowTitle(w.id, s == Main ? "XP Camera Studio" : "XP Camera Studio Mini");
    if (s == Main) XPLMSetWindowResizingLimits(w.id, width * 58 / 100, height * 45 / 100, width, height);
    else XPLMSetWindowResizingLimits(w.id, 320, 120, width, height);
    XPLMWindowAddBrowserFunction(w.id, "fscsKeys", &PanelWindows::onKeys, &w);
    XPLMWindowAddBrowserFunction(w.id, "fscs", &PanelWindows::onFrame, &w);
    XPLMWindowAddBrowserFunction(w.id, "fscsPanel", &PanelWindows::onPanel, &w);
    XPLMWindowAddBrowserFunction(w.id, "fscsPanelSize", &PanelWindows::onPanelSize, &w);
    XPLMWindowAddBrowserFunction(w.id, "fscsPanelDrag", &PanelWindows::onPanelDrag, &w);
    XPLMWindowAddBrowserFunction(w.id, "fscsPanelHover", &PanelWindows::onPanelHover, &w);
    XPLMWindowAddBrowserFunction(w.id, "fscsPanelPopOut", &PanelWindows::onPanelPopOut, &w);
    XPLMWindowSetURL(w.id, (url_ + "?surface=" + kSurfaceName[s] + "&host=xplane").c_str());
}

void PanelWindows::show(Surface s, bool visible) {
    if (visible) create(s);
    Win& w = wins_[s];
    if (!w.id) return;
    w.wanted = visible;
    if (visible) {
        w.revealed = true;  // opening it shows it, even in hover mode, until the mouse has been and gone
        w.leftAt = -1;
    }
    apply(w);
    if (visible) XPLMBringWindowToFront(w.id);
}

void PanelWindows::restore(Surface s) {
    create(s);
    Win& w = wins_[s];
    if (!w.id) return;
    w.wanted = true;
    w.autoHide = s == Mini;
    w.revealed = false;
    w.leftAt = -1;
    apply(w);
}

// Shown when open, unless a hover mini is waiting for the mouse.
void PanelWindows::apply(Win& w) {
    if (!w.id) return;
    const bool on = w.wanted && (!w.autoHide || w.revealed || w.dragging);
    if ((XPLMGetWindowIsVisible(w.id) != 0) == on) return;
    XPLMSetWindowIsVisible(w.id, on ? 1 : 0);
    if (!on && XPLMHasKeyboardFocus(w.id)) XPLMTakeKeyboardFocus(nullptr);
}

void PanelWindows::toggle(Surface s) { show(s, !visible(s)); }

bool PanelWindows::visible(Surface s) const { return wins_[s].id && wins_[s].wanted; }

bool PanelWindows::shown(Surface s) const { return wins_[s].id && XPLMGetWindowIsVisible(wins_[s].id); }

void PanelWindows::keyboard(Surface s, bool focused) {
    const XPLMWindowID id = wins_[s].id;
    if (!id) return;
    if (focused) XPLMTakeKeyboardFocus(id);
    else if (XPLMHasKeyboardFocus(id)) XPLMTakeKeyboardFocus(nullptr);
}

void PanelWindows::fitHeight(Surface s, int height) {
    const XPLMWindowID id = wins_[s].id;
    if (!id) return;
    int sl = 0, st = 0, sr = 0, sb = 0;
    XPLMGetScreenBoundsGlobal(&sl, &st, &sr, &sb);
    const int maxHeight = (st - sb) * 9 / 10;
    if (height < 40) height = 40;
    if (height > maxHeight) height = maxHeight;
    int left = 0, top = 0, right = 0, bottom = 0;
    XPLMGetWindowGeometry(id, &left, &top, &right, &bottom);
    if (top - bottom == height) return;
    XPLMSetWindowGeometry(id, left, bottom + height, right, bottom);
}

// Hover mode: the mouse at the bottom edge of the screen, below the window, brings it up; it goes away once the
// mouse has been off it (and off the band below it) for a moment.
void PanelWindows::updateHover(Win& w, double now) {
    if (!w.id || !w.wanted || !w.autoHide || w.dragging) return;
    int x = 0, y = 0, left = 0, top = 0, right = 0, bottom = 0, sl = 0, st = 0, sr = 0, sb = 0;
    XPLMGetMouseLocationGlobal(&x, &y);
    XPLMGetWindowGeometry(w.id, &left, &top, &right, &bottom);
    XPLMGetScreenBoundsGlobal(&sl, &st, &sr, &sb);
    const bool overColumn = x >= left - kHoverMargin && x <= right + kHoverMargin;
    const bool atEdge = overColumn && y <= sb + kRevealBand;
    const bool over = overColumn && y <= top + kHoverMargin && y >= std::min(bottom, sb) - kHoverMargin;
    if (atEdge || (w.revealed && over) || (w.revealed && XPLMHasKeyboardFocus(w.id))) {
        w.revealed = true;
        w.leftAt = -1;
    } else if (w.revealed) {
        if (w.leftAt < 0) w.leftAt = now;
        if (now - w.leftAt >= kHideDelaySeconds) w.revealed = false;
    }
    apply(w);
}

// Popping out moves the browser into a desktop window of its own. X-Plane hung doing that inside the page's own
// xplane.fscsPanelPopOut call (the page waits for the answer while its view is rebuilt), so it happens here, in the
// flight loop, after the page has its answer.
void PanelWindows::applyPop(Win& w) {
    const int pop = w.pendingPop;
    w.pendingPop = 0;
    if (!w.id || !pop) return;
    const bool out = pop > 0;
    if (out == (XPLMWindowIsPoppedOut(w.id) != 0)) return;
    if (out) {
        XPLMGetWindowGeometry(w.id, &w.left, &w.top, &w.right, &w.bottom);  // to dock it back there
        XPLMSetWindowPositioningMode(w.id, xplm_WindowPopOut, -1);
    } else {
        // Back inside X-Plane, where it was before it popped out.
        XPLMSetWindowPositioningMode(w.id, xplm_WindowPositionFree, -1);
        if (w.left < w.right && w.bottom < w.top) XPLMSetWindowGeometry(w.id, w.left, w.top, w.right, w.bottom);
    }
    log(std::string("editor ") + (out ? "popped out" : "docked"));
}

void PanelWindows::update(double now) {
    for (Win& w : wins_) applyPop(w);
    for (Win& w : wins_) updateHover(w, now);
    for (Win& w : wins_) {
        if (!w.dragging) continue;
        if (!w.id || !XPLMGetWindowIsVisible(w.id) || XPLMWindowIsPoppedOut(w.id)) {
            w.dragging = false;  // a popped-out window moves with its own desktop frame
            continue;
        }
        int x = 0, y = 0;
        XPLMGetMouseLocationGlobal(&x, &y);
        const int dx = x - w.mouseX, dy = y - w.mouseY;
        XPLMSetWindowGeometry(w.id, w.left + dx, w.top + dy, w.right + dx, w.bottom + dy);
    }
}

void PanelWindows::post(const std::string& frame) { outbox_.push_back(frame); }

void PanelWindows::flush() {
    while (!outbox_.empty()) {
        // A JSON string literal is a valid JavaScript string literal (ASCII-only, so U+2028/2029 are escaped too).
        const std::string script = "window.FSCXPlaneReceive&&window.FSCXPlaneReceive(" +
                                   json(outbox_.front()).dump(-1, ' ', true, json::error_handler_t::replace) + ");";
        outbox_.pop_front();
        for (const Win& w : wins_)
            if (w.id && w.loaded) XPLMWindowInjectScript(w.id, script.c_str());
    }
}

void PanelWindows::destroy() {
    for (Win& w : wins_) {
        if (w.id) XPLMDestroyWindow(w.id);
        w.id = nullptr;
        w.loaded = false;
        w.dragging = false;
        w.wanted = w.revealed = w.autoHide = false;
        w.leftAt = -1;
        w.pendingPop = 0;
    }
    outbox_.clear();
}

const char* PanelWindows::onFrame(XPLMWindowID, const char* text, void* refcon) {
    auto* w = static_cast<Win*>(refcon);
    // The page passes the frame text; X-Plane hands it over JSON-encoded (a JSON string). Should a build pass the
    // text through as it is, the frame itself is a JSON object: accept that too.
    const std::string raw = text ? text : "";
    const json arg = json::parse(raw, nullptr, false);
    const bool ok = arg.is_string() || arg.is_object();
    if (ok && w && !w->heard) {
        w->heard = true;  // once per page load: proves the page reached the plugin
        w->loaded = true;  // its scripts run before the load-finished callback: answer its Hello at once
        log(std::string(kSurfaceName[w->surface]) + " page connected");
    }
    if (ok && w && w->owner && w->owner->inbound_) w->owner->inbound_(w->surface, arg.is_string() ? arg.get<std::string>() : raw);
    return XPLMReturnString(ok ? "true" : "false");
}

const char* PanelWindows::onKeys(XPLMWindowID, const char* text, void* refcon) {
    auto* w=static_cast<Win*>(refcon);
    if(!w || !w->owner || !w->owner->nativeKeys)
        return XPLMReturnString("{\"ok\":false,\"error\":\"Native key service is unavailable.\"}");
    const auto result=w->owner->nativeKeys(text?text:"");
    return XPLMReturnString(result.c_str());
}

const char* PanelWindows::onPanel(XPLMWindowID, const char* text, void* refcon) {
    auto* w = static_cast<Win*>(refcon);
    const json arg = json::parse(text ? text : "", nullptr, false);
    if (!w || !w->owner || !arg.is_object()) return XPLMReturnString("false");
    const std::string surface = arg.value("surface", std::string());
    const bool open = arg.value("open", false);
    if (surface != "main" && surface != "mini") return XPLMReturnString("false");
    w->owner->show(surface == "main" ? Main : Mini, open);
    return XPLMReturnString("true");
}

const char* PanelWindows::onPanelSize(XPLMWindowID, const char* text, void* refcon) {
    auto* w = static_cast<Win*>(refcon);
    const json arg = json::parse(text ? text : "", nullptr, false);
    if (!w || !w->owner || !arg.is_object() || !arg.contains("height") || !arg["height"].is_number()) return XPLMReturnString("false");
    // Only the mini follows its content; the editor keeps the size the user gives it.
    if (w->surface != Mini) return XPLMReturnString("false");
    w->owner->fitHeight(Mini, static_cast<int>(arg["height"].get<double>()));
    return XPLMReturnString("true");
}

const char* PanelWindows::onPanelHover(XPLMWindowID, const char* text, void* refcon) {
    auto* w = static_cast<Win*>(refcon);
    const json arg = json::parse(text ? text : "", nullptr, false);
    if (!w || !w->id || !arg.is_object() || w->surface != Mini) return XPLMReturnString("false");
    const bool autoHide = arg.value("autoHide", false);
    if (autoHide != w->autoHide) {
        w->autoHide = autoHide;
        w->leftAt = -1;  // the hide delay starts again from now
        w->owner->apply(*w);
    }
    return XPLMReturnString("true");
}

const char* PanelWindows::onPanelPopOut(XPLMWindowID, const char* text, void* refcon) {
    auto* w = static_cast<Win*>(refcon);
    if (!w || !w->id || w->surface != Main) return XPLMReturnString("in");
    // The state it will have once update() has moved it (a second click before that undoes the first).
    const bool now = w->pendingPop ? w->pendingPop > 0 : XPLMWindowIsPoppedOut(w->id) != 0;
    const bool out = !now;
    w->dragging = false;
    w->pendingPop = out ? 1 : -1;
    (void)text;
    return XPLMReturnString(out ? "out" : "in");
}

const char* PanelWindows::onPanelDrag(XPLMWindowID, const char* text, void* refcon) {
    auto* w = static_cast<Win*>(refcon);
    const json arg = json::parse(text ? text : "", nullptr, false);
    if (!w || !w->id || !arg.is_object()) return XPLMReturnString("false");
    if (arg.value("phase", std::string()) == "start") {
        XPLMGetMouseLocationGlobal(&w->mouseX, &w->mouseY);
        XPLMGetWindowGeometry(w->id, &w->left, &w->top, &w->right, &w->bottom);
        w->dragging = true;
    } else {
        w->dragging = false;
    }
    return XPLMReturnString("true");
}

void PanelWindows::onLoaded(XPLMWindowID, const char* url, void* refcon) {
    auto* w = static_cast<Win*>(refcon);
    if (!w) return;
    w->loaded = true;
    w->heard = false;
    log(std::string(kSurfaceName[w->surface]) + " page loaded" + (url ? std::string(": ") + url : std::string()));
}

void PanelWindows::onLoadError(XPLMWindowID, const char* url, const char* error, void* refcon) {
    auto* w = static_cast<Win*>(refcon);
    if (w) w->loaded = false;
    log(std::string("could not load ") + (url ? url : "?") + ": " + (error ? error : "unknown error"));
}

}  // namespace fscs::xplane
