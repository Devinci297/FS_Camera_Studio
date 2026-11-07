#pragma once
// XP Camera Studio: the shared HTML UI (ui/index.html) in X-Plane 12.4.4 browser windows, one per surface:
// "main" (the editor) and "mini" (the dock). The page talks to the module with the same protocol as the MSFS
// CommBus (doc/protocol.md):
//   page -> plugin   xplane.fscs(frameText)            browser function, queued for the next flight-loop pass
//   plugin -> page   FSCXPlaneReceive(frameText)       injected script, every loaded window
//   page -> plugin   xplane.fscsPanel({surface, open})  the UI's own open/close buttons
//   page -> plugin   xplane.fscsPanelSize({surface, height})  the mini fits its window to the dock
//   page -> plugin   xplane.fscsPanelDrag({surface, phase})   "start"/"end": the window follows the mouse between
//   page -> plugin   xplane.fscsPanelHover({surface, autoHide})  the mini hides until the mouse reaches the bottom edge
//   page -> plugin   xplane.fscsPanelPopOut({surface})  the editor moves into its own desktop window and back; answers
//                                                       "out" or "in"
// The windows are self-decorated: the page draws its own frame, header and close button, as in MSFS.
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
    // Open again at startup, as it was when X-Plane last closed: a mini starts hidden, in hover mode, until its page
    // says otherwise (pinned, or hover turned off), so the bottom edge reveals it without opening it each session.
    void restore(Surface s);
    void toggle(Surface s);
    // Open as far as the user is concerned: a hover-hidden mini still counts as open.
    bool visible(Surface s) const;
    // Drawn on screen now.
    bool shown(Surface s) const;
    // The page in `s` has a text field focused: give that window the keyboard, or hand it back to X-Plane.
    void keyboard(Surface s, bool focused);
    // Sets a window's content height in boxels, keeping its bottom edge (the mini dock grows upwards).
    void fitHeight(Surface s, int height);
    // Every frame: moves a window that is being dragged by its page, and hides or reveals a hover mini.
    void update(double now);
    static constexpr int kRevealBand = 10;          // boxels above the screen's bottom edge that reveal the mini
    static constexpr int kHoverMargin = 16;         // around the window, still counted as over it
    static constexpr double kHideDelaySeconds = 0.8;
    void post(const std::string& frame);  // a module frame for every page, delivered by flush()
    void flush();
    void destroy();

private:
    struct Win {
        XPLMWindowID id = nullptr;
        bool loaded = false;
        bool heard = false;  // a frame arrived since the page loaded
        PanelWindows* owner = nullptr;
        Surface surface = Main;
        bool dragging = false;
        int mouseX = 0, mouseY = 0, left = 0, top = 0, right = 0, bottom = 0;  // at the start of the drag
        bool wanted = false;    // open (menu, command or the page's own buttons)
        bool autoHide = false;  // hover mode, set by the page
        bool revealed = false;  // hover mode: the mouse brought it up
        double leftAt = -1;     // hover mode: when the mouse left it
        int pendingPop = 0;     // the page asked to pop out (1) or dock (-1): done in update(), never in its call
    };
    void applyPop(Win& w);
    void apply(Win& w);
    void updateHover(Win& w, double now);
    static const char* onPanelHover(XPLMWindowID window, const char* json, void* refcon);
    static const char* onPanelPopOut(XPLMWindowID window, const char* json, void* refcon);
    void create(Surface s);
    static const char* onFrame(XPLMWindowID window, const char* json, void* refcon);
    static const char* onPanel(XPLMWindowID window, const char* json, void* refcon);
    static const char* onPanelSize(XPLMWindowID window, const char* json, void* refcon);
    static const char* onPanelDrag(XPLMWindowID window, const char* json, void* refcon);
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
