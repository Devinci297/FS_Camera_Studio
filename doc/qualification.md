# In-sim qualification (Phase 0 gates)

Design 2.0 §13.1 makes camera correctness and in-sim hosting co-equal release gates. Initial in-sim tests exposed
startup and hosting failures; the latest corrections still need a live retest. Record each result
(pass / fail / notes, simulator build, aircraft) in `doc/test-evidence/` as you go.

Baseline to record: simulator build (shown in Settings → Simulator), SDK 1.7.3, package version (`VERSION`),
aircraft and livery, Steam or Microsoft Store edition.

## Setup

1. `tools\build.ps1` — builds the WASM module and stages the MSFS project.
2. `tools\package.ps1 -Compile` — compiles the toolbar panels with `fspackagetool` (uses the simulator's build mode).
3. Close MSFS, then `tools\install.ps1` — links the package into your Community folder.
4. Start a Free Flight, parked, in a small cockpit aircraft.

## Hosting gates

| Gate | Check | Pass when |
| --- | --- | --- |
| H01-a | Toolbar shows **Camera Studio** and **Camera Studio Mini** buttons | Both open their panels |
| H01-b | Main panel looks like `Docs/MSFS_Camera_UI.html` (editor) | Layout, colours and fonts match; nothing hidden by the sim frame |
| H01-c | Open Mini from the toolbar; hover anywhere in the invisible bottom strip to reveal the dock; stay in the strip or dock; leave both | No visible handle, text, frame or resize grips while hidden; stays visible inside the area, hides after ~0.18 s outside; Pin keeps it |
| H01-f | Drag any blank editor background; click buttons, fields and camera rows | Background moves the native window; controls retain their normal actions |
| H01-d | Click switches/knobs right next to (not under) the mini panel and around the closed editor | Cockpit clicks outside the panels always work |
| H01-e | Close and reopen each panel from the toolbar; switch aircraft | State comes back (same profile, selection, unsaved edits) |
| H02 | Bindings: assign Ctrl+Alt+1, a joystick button and a hat direction; test each with the editor **closed**; unplug/replug the controller | Down/up work, peek returns, Release always works, hot-plug recovers |
| H03 | Save, restart the flight, restart MSFS; export a profile code and import it back | Everything persists; import makes a copy |
| H04 | Pause, active pause, menus, aircraft change, flight reload | No stale camera movement, no unexpected acquisition |

If a toolbar panel doesn't appear or shows no content, inspect `ui/msfs-panel.js` and the native template imports.
The initial `hide` workaround leaves the simulator's `panelInvisible` visibility lifecycle intact.

## Camera gates

| Gate | Check | Pass when |
| --- | --- | --- |
| G01 | Settings → Simulator → **Run axis check** | Each step moves as described (the check fixes reversed signs and records the result) |
| G02 | Look around with the native camera, then Capture current view in the editor | Captured values match the view; note if head tracking is included |
| G03 | Part of the axis check: recall a view after moving | Same view every time |
| G04 | FOV slider from 30° to 110°, in windowed and ultrawide | Range and look are sensible; note limits |
| G05 | With a Camera Studio view active, click switches, turn knobs, use screens | Cockpit stays operable |
| G06 | Release, disable add-on cameras in MSFS settings, let another camera add-on take over | Native camera always recoverable without restarting |
| G07 | Smooth/Cinematic transitions at 30, 60 and 120 FPS | Smooth, correct duration, no stutter |
| G08 | Exterior (Preview) views | Visibility and the known cockpit-sound limitation recorded |

Until G01 passes, live control stays off unless **Settings → Simulator → Allow live control before axis
qualification** is switched on (for testing only).
