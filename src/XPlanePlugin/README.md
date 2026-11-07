# XP Camera Studio (X-Plane 12 plugin)

The X-Plane 12 version of FS Camera Studio. It runs the same module logic (`ModuleApp`, profiles, protocol) as the
MSFS WASM module and shows the same HTML UI (`ui/`) in X-Plane's built-in browser windows. Requires X-Plane 12.4.4
or newer (SDK 4.4.0, vendored in `third_party/XPSDK`).

Phase 1 (this folder today): the plugin loads, hosts the module from a flight loop, stores profiles as files and opens
the editor and the mini panel. It never moves the camera yet: the module stays in Observing.

## Build (Windows, Visual Studio 2022 x64)

    cmake -S . -B build-xp -G "Visual Studio 17 2022" -A x64 -DFSCS_BUILD_XPLANE=ON
    cmake --build build-xp --config Release --target xpcs_plugin

The result is `build-xp/XPCameraStudio/` with `win_x64/XPCameraStudio.xpl` and `ui/`. Copy that whole folder to
`X-Plane 12/Resources/plugins/XPCameraStudio`.

## Use

- Plugins > XP Camera Studio > Editor / Mini panel, or the commands `xpcs/toggle_editor` and `xpcs/toggle_mini`
  (bind them in X-Plane's keyboard or joystick settings).
- Cameras: cockpit views move the pilot's head (`sim/graphics/view/pilots_head_*`, so the 3D cockpit stays
  clickable) and set `field_of_view_deg`; exterior, orbit, mounted, fly-by and airport views switch to an external
  view and draw it through `XPLMControlCamera`. Picking another X-Plane view hands the camera back.
- Profiles are saved in `X-Plane 12/Output/preferences/XPCameraStudio`. Log lines start with `XP Camera Studio:`
  in `X-Plane 12/Log.txt`.

## Commands

Every binding action is an X-Plane command, for X-Plane's keyboard and joystick settings. Held actions last while
the key or button is down.

| Command | Action |
| --- | --- |
| `xpcs/toggle_editor`, `xpcs/toggle_mini` | show or hide the editor / mini panel |
| `xpcs/view_1` .. `xpcs/view_30` | select the camera with that view ID |
| `xpcs/peek_1` .. `xpcs/peek_30` | show that view while held |
| `xpcs/next_camera`, `previous_camera`, `next_category`, `return_to_pilot` | cycle cameras |
| `xpcs/release_camera`, `panic_release` | give the camera back to X-Plane |
| `xpcs/look_left/right/up/down`, `move_forward/back/left/right/up/down`, `move_fast`, `zoom_in/out` | held motion |
| `xpcs/recenter_look`, `toggle_mouse_look`, `binoculars`, `glance_hold`, `glance_next`, `glance_return` | look |
| `xpcs/toggle_cinematic`, `toggle_airport_auto`, `toggle_head_tracking`, `toggle_auto_spot` | modes |
| `xpcs/spot_next`, `spot_previous`, `spot_return`, `save_views` | spotting, save all changes |

## Datarefs (read only)

| Dataref | Type | Value |
| --- | --- | --- |
| `xpcs/camera_active` | int | 1 while XP Camera Studio shows one of its cameras |
| `xpcs/active_view` | int | that camera's view ID (0 for none) |
| `xpcs/active_camera` | data | that camera's name (UTF-8) |
| `xpcs/fov_deg` | float | the horizontal field of view on screen |
| `xpcs/cinematic`, `xpcs/auto_spot` | int | 1 while cinematic mode / automatic spotting runs |
| `xpcs/editor_open`, `xpcs/mini_open` | int | 1 while that window is shown |

## How the UI connects

The plugin opens `ui/index.html?surface=main|mini&host=xplane`. `ui/bridge.js` sees `host=xplane` and sends frames
through the browser function `xplane.fscs(frame)`; the plugin answers by injecting `FSCXPlaneReceive(frame)`. Frames
are the same protocol v2 JSON as the MSFS CommBus. `xplane.fscsPanel({surface, open})` shows or hides a window.

## Tests (Linux)

`xpcs_fake_xplane` (`tests/fake_xplane.cpp`) is a stand-in X-Plane: it implements the XPLM calls the plugin uses,
loads the built `.xpl` and plays a browser window (Hello and Snapshot, create, edit and save a profile, reload after
disable and enable, keyboard focus, window fit and drag, cockpit and exterior cameras, commands and datarefs). It runs with `ctest` in a Linux build with `-DFSCS_BUILD_XPLANE=ON`.
`node ui/tests/xplane-bridge.test.mjs` checks the UI side of the connection.
