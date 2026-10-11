# FS Camera Studio

A camera manager for **Microsoft Flight Simulator 2024** that lives entirely inside the simulator: aircraft
profiles, organised views, precise editing, smooth transitions and shortcuts — opened from the sim's toolbar, with a
mini control panel at the bottom of the screen.

**Status: built, not yet run in the simulator.** The WASM module compiles with the MSFS 2024 SDK toolchain, the
panels run in a browser preview, and 71 native + 22 UI tests pass. Every in-sim behaviour still has to be qualified
on a real simulator build — see [`doc/qualification.md`](doc/qualification.md).

## How it works

```text
MSFS toolbar ── "Camera Studio" panel (full editor)      ┐  HTML/CSS/JS (ui/)
             └─ "Camera Studio Mini" panel (bottom dock) ┘
                         │  CommBus (FSCS_UI_TO_HOST / FSCS_HOST_TO_UI)
                         ▼
FSCameraStudio.wasm — standalone module loaded by MSFS from the package
   working drafts · profiles (\work storage) · input bindings · coordinator · motion engine
                         │
                  WASM Camera API (fsCamera*) — the only camera writer
```

There is no desktop app, background service or external process. Both panels are mirrors of one authoritative
state held in the module, so the editor and the mini panel always agree.

* **Safety first:** the module starts in *Observing* and never moves the view on its own. Release Camera is always
  available (both panels and a binding). If anything goes wrong — another add-on takes the camera, the sim takes
  over, a release can't be confirmed — it stops writing and hands control back.
* **Profiles** are matched to the loaded aircraft by its package (liveries keep the same profile). Saves are one
  transaction (profile + settings + bindings), written to one of two checksummed slots and verified by readback, so
  an interrupted save can never damage the last good one. Five backups per profile.
* **Bindings** use the simulator's own input mapping (keyboard chords, joystick buttons, hats) and work with the
  editor closed.
* **Axis check:** the camera API's axis signs are measured per simulator build with an in-panel check before live
  control is enabled.

## Build

Requirements: Windows 11 x64, the MSFS 2024 SDK (`MSFS2024_SDK` set; 1.7.3 is the baseline), PowerShell. For the
native tests: Visual Studio 2022 Build Tools and CMake 3.25+.

```powershell
tools\build.ps1                 # WASM module + staged MSFS project in build\project
tools\package.ps1 -Compile      # compile the toolbar panels with fspackagetool -> build\package
tools\install.ps1               # link the package into your Community folder (close MSFS first)
```

Native tests (pure engine, profiles, protocol, coordinator and module logic against fake adapters):

```powershell
cmake --preset tests
cmake --build --preset tests
ctest --preset tests
```

UI tests and browser preview: `node ui/tests/math.test.mjs`, `node ui/tests/bridge.test.mjs`, and open
`ui/index.html` in a browser (demo mode with an in-memory module).

## Repository layout

| Path | Contents |
| --- | --- |
| `src/CameraCore` | Pure math: frames, quaternions, transitions, manual movement |
| `src/Profiles` | Profile schema v2, validation, bindings/settings, the two-slot repository, profile codes |
| `src/Protocol` | Panel ⇄ module envelopes and CommBus chunking |
| `src/WasmModule` | Coordinator, input router and module app (pure); `wasm/` holds the SDK adapters and entry points |
| `ui/` | The in-sim panels (one codebase, `main` and `mini` surfaces) |
| `package/` | MSFS project, package definition, toolbar panel definitions and icons |
| `tests/` | Native test suite with fake camera, services and storage |
| `doc/` | [`protocol.md`](doc/protocol.md) (the contract) and [`qualification.md`](doc/qualification.md) (in-sim gates) |
| `tools/` | Build, package and install scripts |
