#include "xp_panels.hpp"

#include <XPLMUtilities.h>

#include <nlohmann/json.hpp>

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
        // The dock: a strip centred above the bottom edge.
        const int w2 = width * 60 / 100;
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
    XPLMWindowAddBrowserFunction(w.id, "fscs", &PanelWindows::onFrame, &w);
    XPLMWindowAddBrowserFunction(w.id, "fscsPanel", &PanelWindows::onPanel, &w);
    XPLMWindowAddBrowserFunction(w.id, "fscsPanelSize", &PanelWindows::onPanelSize, &w);
    XPLMWindowAddBrowserFunction(w.id, "fscsPanelDrag", &PanelWindows::onPanelDrag, &w);
    XPLMWindowSetURL(w.id, (url_ + "?surface=" + kSurfaceName[s] + "&host=xplane").c_str());
}

void PanelWindows::show(Surface s, bool visible) {
    if (visible) create(s);
    Win& w = wins_[s];
    if (!w.id) return;
    XPLMSetWindowIsVisible(w.id, visible ? 1 : 0);
    if (visible) XPLMBringWindowToFront(w.id);
    else if (XPLMHasKeyboardFocus(w.id)) XPLMTakeKeyboardFocus(nullptr);
}

void PanelWindows::toggle(Surface s) { show(s, !visible(s)); }

bool PanelWindows::visible(Surface s) const { return wins_[s].id && XPLMGetWindowIsVisible(wins_[s].id); }

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

void PanelWindows::updateDrag() {
    for (Win& w : wins_) {
        if (!w.dragging) continue;
        if (!w.id || !XPLMGetWindowIsVisible(w.id)) {
            w.dragging = false;
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
