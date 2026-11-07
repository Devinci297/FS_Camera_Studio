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
- Profiles are saved in `X-Plane 12/Output/preferences/XPCameraStudio`. Log lines start with `XP Camera Studio:`
  in `X-Plane 12/Log.txt`.

## How the UI connects

The plugin opens `ui/index.html?surface=main|mini&host=xplane`. `ui/bridge.js` sees `host=xplane` and sends frames
through the browser function `xplane.fscs(frame)`; the plugin answers by injecting `FSCXPlaneReceive(frame)`. Frames
are the same protocol v2 JSON as the MSFS CommBus. `xplane.fscsPanel({surface, open})` shows or hides a window.

## Tests (Linux)

`xpcs_fake_xplane` (`tests/fake_xplane.cpp`) is a stand-in X-Plane: it implements the XPLM calls the plugin uses,
loads the built `.xpl` and plays a browser window (Hello and Snapshot, create, edit and save a profile, reload after
disable and enable, keyboard focus). It runs with `ctest` in a Linux build with `-DFSCS_BUILD_XPLANE=ON`.
`node ui/tests/xplane-bridge.test.mjs` checks the UI side of the connection.
