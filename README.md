# FS Camera Studio

A camera manager for **Microsoft Flight Simulator 2024** that lives entirely inside the simulator: aircraft
profiles, organised views, precise editing, smooth transitions and shortcuts — opened from the sim's toolbar, with a
mini control panel at the bottom of the screen.

**Status: in-sim testing, with a new build awaiting live verification.** The module and panels have run in MSFS;
the airport-camera test build passes 102 native tests and focused browser checks and compiles with SDK 1.7.3.
Camera axes, template poses and hosting behaviour still require live qualification — see
[`doc/qualification.md`](doc/qualification.md).

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
  Each aircraft also gets a readable, importable JSON file alongside these recovery slots; see
  [`doc/profile-storage.md`](doc/profile-storage.md). The C172 automatically receives a 20-view starter profile
  (12 cockpit, 8 exterior) the first time this template is used. It does not move the camera on load.
* **Bindings** use the simulator's own input mapping (keyboard chords, joystick buttons, hats) and work with the
  editor closed.
* **Axis check:** the camera API's axis signs are measured per simulator build with an in-panel check before live
  control is enabled.
* **Airport cameras (test build):** open **Profiles → Airport cameras**, choose a nearby airport or enter ICAOs,
  preview generated views and save the selected ones. Runway, gate/apron, tower and offset taxiway-intersection
  views use available scenery facility data. Optional airport XML supplies actual taxi-sign positions.
  Only one nearby saved airport loads under the Airport category. See
  [`the airport test guide`](doc/test-evidence/2026-10-11-airport-cameras.md) for scope and live checks.

## Build

The tester release and illustrated manual are available from
[GitHub Releases](https://github.com/Devinci297/FS_Camera_Studio/releases).
The app bar displays the version; click it for update detection and the installer download.
Close MSFS before installing an update. Airport cameras that have been generated **and saved**
load automatically within the configured radius (25 NM by default); Auto switch is a separate viewpoint control.

### Reproduce the 0.1.1 tester release

```powershell
tools\build.ps1 -ModuleOnly -OutputDirectory build/wasm/release-0.1.1
python tools/diagnostics/stage-release.py
python tools/build-manual.py
python tools/build-installer.py
```

Install ReportLab for manual generation and obtain Inno Setup with `tools/get-installer-compiler.ps1`.
`package/validated-host` freezes the previously working toolbar host, including compiled panel definitions;
its receipt is `package/validated-host.json`. The release stage applies only the update UI and rebuilt module.
This intentionally preserves the known-loading host while broader `ui/` host changes still need flight-loading
qualification. Do not replace the release host wholesale with the development panel. The generated receipt,
installer and PDF are under `build/release`, `dist` and `output/pdf` respectively. Nothing is installed by these commands.

For a shareable Windows tester setup, see [`installer/README.md`](installer/README.md).
The installer packages an explicitly verified build, detects the MSFS Community folder, supports upgrades and
uninstall, and preserves saved camera profiles. Testers do not need the SDK or developer tools.

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

UI tests and browser preview: `node ui/tests/math.test.mjs`, `node ui/tests/bridge.test.mjs`,
`node ui/tests/panel.test.mjs`, and open
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
