# XP Camera Studio (X-Plane 12 plugin)

The X-Plane 12 version of FS Camera Studio. It runs the same module logic (`ModuleApp`, profiles, protocol) as the
MSFS WASM module and shows the same HTML UI (`ui/`) in X-Plane's built-in browser windows. Requires X-Plane 12.4.4
or newer (SDK 4.4.0, vendored in `third_party/XPSDK`).

The plugin hosts the module from a flight loop, stores profiles as files, opens the editor and the mini panel in
browser windows, flies cockpit, exterior, airport and spotting cameras, and builds airport cameras from apt.dat. From
the next release it ships in the same Windows installer as the MSFS version, with the same version number.

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

## Airports and spotting

Airport cameras come from X-Plane's own scenery files. At start the plugin indexes every `Earth nav data/apt.dat` of
the enabled packs in `Custom Scenery/scenery_packs.ini` plus `Global Scenery/Global Airports` (where
`*GLOBAL_AIRPORTS*` is listed), on a worker thread; the first pack to list an airport wins, as in X-Plane. An airport
gives runways (rows 100 and 101), the tower viewpoint (14), startup locations as gates and parking (1300 and 1301, or
older 15 rows), the taxi network (1201 and 1202) and taxi signs (20, labelled by their text). The log says
`airports: N from M apt.dat files` when the index is ready.

Spotting traffic is read from X-Plane's TCAS targets (`sim/cockpit2/tcas/targets/...`), where X-Plane puts its AI
aircraft and multiplayer or traffic plugins put theirs. Each aircraft is known by its mode S id; its callsign is the
`flight_id` and its type the `icao_type`.

## Importing X-Camera cameras

Profiles > Import lists X-Camera's camera files under **X-Camera views**: each aircraft's `X-Camera_<acf>.csv` (in
`Aircraft/`, two folder levels deep), community sets in `Resources/plugins/X-Camera/CommunityAircraftCameras` named
for the loaded aircraft's .acf, and `.csv` files copied into the imports folder (taken as the loaded aircraft's).
Import converts the chosen file into a new profile for that aircraft; X-Camera's files are only read.

- X, Y, Z are aircraft body axes from the reference point (`pilots_head_x/y/z`). Cockpit cameras become cockpit views
  relative to the eyepoint, read from the .acf (`acf/_pe_xyz` less `acf/_cgY` and `_cgZ`, in feet); without one they
  are measured from X-Camera's pilot view. Exterior cameras become exterior views relative to the aircraft.
- A camera path (the hidden rows after a camera) that circles the aircraft becomes an orbit (distance, height, lap
  time); one that runs from ahead to behind becomes a fly-by; others keep their first point as a still view.
- Kept: names, categories, view IDs (the second use of an ID is dropped), FOV and zoom, smooth or cut transitions,
  head shake (motion effects), level camera (horizon) and retained mouse look. Cameras fixed in the world are left out.

`xpcs_xcamera_convert <X-Plane folder> <output folder>` writes the same conversions as profile files.

## How the UI connects

The plugin opens `ui/index.html?surface=main|mini&host=xplane`. `ui/bridge.js` sees `host=xplane` and sends frames
through the browser function `xplane.fscs(frame)`; the plugin answers by injecting `FSCXPlaneReceive(frame)`. Frames
are the same protocol v2 JSON as the MSFS CommBus. `xplane.fscsPanel({surface, open})` shows or hides a window.

## Tests (Linux)

`xpcs_fake_xplane` (`tests/fake_xplane.cpp`) is a stand-in X-Plane: it implements the XPLM calls the plugin uses,
loads the built `.xpl` and plays a browser window (Hello and Snapshot, create, edit and save a profile, reload after
disable and enable, keyboard focus, window fit and drag, cockpit and exterior cameras, commands and datarefs,
nearby airports and an airport preview from test scenery, TCAS traffic and spotting it). It runs with `ctest` in a Linux build with `-DFSCS_BUILD_XPLANE=ON`.
`node ui/tests/xplane-bridge.test.mjs` checks the UI side of the connection.
