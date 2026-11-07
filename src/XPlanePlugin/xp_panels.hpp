#pragma once
// XP Camera Studio: the shared HTML UI (ui/index.html) in X-Plane 12.4.4 browser windows, one per surface:
// "main" (the editor) and "mini" (the dock). The page talks to the module with the same protocol as the MSFS
// CommBus (doc/protocol.md):
//   page -> plugin   xplane.fscs(frameText)            browser function, queued for the next flight-loop pass
//   plugin -> page   FSCXPlaneReceive(frameText)       injected script, every loaded window
//   page -> plugin   xplane.fscsPanel({surface, open})  the UI's own open/close buttons
// Windows are created the first time they are shown.

#include <XPLMDisplay.h>

#include <deque>
#include <functional>
#include <string>

namespace fscs::xplane {

class PanelWindows {
public:
    enum Surface { Main = 0, Mini = 1 };
    // `uiIndexUrl`: file URL of ui/index.html, without a query.
    // `inbound(surface, frame)`: a frame from a page, called inside the X-Plane callback; the host queues it.
    void init(std::string uiIndexUrl, std::function<void(Surface, std::string)> inbound);
    void show(Surface s, bool visible);
    void toggle(Surface s);
    bool visible(Surface s) const;
    // The page in `s` has a text field focused: give that window the keyboard, or hand it back to X-Plane.
    void keyboard(Surface s, bool focused);
    void post(const std::string& frame);  // a module frame for every page, delivered by flush()
    void flush();
    void destroy();

private:
    struct Win {
        XPLMWindowID id = nullptr;
        bool loaded = false;
        PanelWindows* owner = nullptr;
        Surface surface = Main;
    };
    void create(Surface s);
    static const char* onFrame(XPLMWindowID window, const char* json, void* refcon);
    static const char* onPanel(XPLMWindowID window, const char* json, void* refcon);
    static void onLoaded(XPLMWindowID window, const char* url, void* refcon);
    static void onLoadError(XPLMWindowID window, const char* url, const char* error, void* refcon);

    std::string url_;
    std::function<void(Surface, std::string)> inbound_;
    Win wins_[2];
    std::deque<std::string> outbox_;
};

// "file:///" URL for a UTF-8 native path, percent-encoding everything outside the unreserved set and "/:".
std::string fileUrl(const std::string& utf8Path);

}  // namespace fscs::xplane
