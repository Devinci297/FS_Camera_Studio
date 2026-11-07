# Tester installer

One setup for both simulators. The components page offers **Microsoft Flight Simulator 2024** and **X-Plane 12**
(XP Camera Studio); the user picks one or both, and each choice gets its own folder page. Both share one version and
one update check. On a first install the simulators found on the PC are preselected; an upgrade keeps the earlier
choice.

The Windows installer deploys the **verified packaged build**, not a fresh compile of the working tree.
This preserves the reviewed simulator host and its native pointer routing.
The package files remain byte-identical to the selected release receipt. No user profiles or development
configuration are included. Setup works offline, per user, without the MSFS SDK or a background service.

## Build

```powershell
tools\get-installer-compiler.ps1
python tools/diagnostics/stage-v0.5.2.py --freeze
tools\build.ps1 -ModuleOnly -OutputDirectory build/wasm/release-0.5.2
tools\package.ps1 -Compile -Wasm build/wasm/release-0.5.2/FSCameraStudio.wasm -OutputDirectory build/release-0.5.2-compile
python tools/diagnostics/stage-v0.5.2.py --host build/release-0.5.2-compile/Packages/fs-camera-studio
python tools/diagnostics/stage-v0.5.2.py
cmake -S . -B build-xp -G "Visual Studio 17 2022" -A x64 -DFSCS_BUILD_XPLANE=ON
cmake --build build-xp --config Release --target xpcs_plugin
node tools/capture-manual.cjs
python tools/build-manual.py
python tools/build-installer.py --version 0.5.2 --package build/release/0.5.2/fs-camera-studio --receipt build/release/0.5.2/staged.json --xplane build-xp/XPCameraStudio
```

Both simulator payloads must be built at the same version. The plugin takes its version from `ui/version.js`, and
the installer build refuses a mismatch.

`--xplane` defaults to `build-xp/XPCameraStudio`. It must hold `win_x64/XPCameraStudio.xpl` and `ui/` only;
`BUILD-INFO.json` records every plugin file hash next to the package hashes, and setup verifies both after install.

The build dependency is Inno Setup 6.7.3, downloaded from its official GitHub release and checked for a valid
Pyrsys B.V. Authenticode signature before execution. An existing compiler can be supplied with `--compiler`.
Testers only need the resulting setup EXE. The release ID has the form `major.minor.patch or major.minor.patch-test.YYYYMMDD.iteration`;
increase it for every distributed build. `BUILD-INFO.json` records the tester ID and every shipped package hash.
The add-on manifest version is preserved from the tested package rather than relabeling its payload.

Outputs are under `dist/<release ID>/`:

- `FS-Camera-Studio-<ID>-Setup.exe`: standalone installer.
- `FS-Camera-Studio-<ID>-Testers.zip`: installer, illustrated PDF manual, guide, C152 import-test profile, notices, build info and SHA-256 checksums.
- ZIP `.sha256` sidecar: publish separately from the archive when sharing it.

## Behavior

- Detects Steam and Microsoft Store MSFS 2024 `UserCfg.opt` package locations. Preserves an existing install's
  Community location, otherwise prefers an existing Community2024 folder. The tester confirms/browses the folder.
- Upgrades the same location; blocks a second copy in the sibling Community/Community2024 folder.
- Requires MSFS stopped, checks again immediately before writing, never kills or restarts the simulator.
- Refuses package junctions/symlinks (including nested ones). It cannot overwrite an Addons Linker/development target.
- Copies and hash-verifies an existing package into `%LOCALAPPDATA%\FS Camera Studio\Backups` before replacing files.
- Verifies every installed package file. Repair restores the same shipped files; unknown added files remain untouched.
- Registers a per-user Windows Apps uninstaller and Start-menu guide. Uninstall removes only installer-owned files;
  MSFS `\work` camera profiles, airport data, settings, bindings, package backups and unrelated files are preserved.
- Uses the current user's permissions. Protected Community folders need an appropriate writable package location.

X-Plane 12:

- Finds X-Plane 12 from `%LOCALAPPDATA%\x-plane_install_12.txt` (written by X-Plane), the default Steam library or
  one listed in Steam's `libraryfolders.vdf`; the user confirms or browses to the folder with `X-Plane.exe`.
- Installs to `Resources\plugins\XPCameraStudio` (`win_x64\XPCameraStudio.xpl` and `ui\`). An existing plugin folder
  is backed up and hash-verified under `%LOCALAPPDATA%\FS Camera Studio\Backups\<time>\XPCameraStudio`, then replaced
  by a clean copy, so no stale files remain. A folder of that name without the plugin is refused, as are links.
- Requires X-Plane stopped (`X-Plane.exe`). Profiles in `Output\preferences\XPCameraStudio` are never touched;
  uninstall removes only the plugin folder and leaves other plugins alone.
- With X-Plane only, nothing is written to a Community folder.

Silent deployment: `Setup.exe /VERYSILENT /SUPPRESSMSGBOXES /NORESTART /COMMUNITY="D:\MSFS\Community" /LOG="setup.log"`.
For X-Plane add `/COMPONENTS=msfs,xplane /XPLANE="D:\X-Plane 12"` (or `/COMPONENTS=xplane` for X-Plane only). A
silent setup never selects X-Plane on its own.
No install target is accepted without validation. This release is unsigned; signing needs a publisher certificate.
Sign the EXE before regenerating distribution checksums/ZIP for any future signed release.

## Regression checks

`python tools/test-installer.py` tests the shipping EXE in an isolated Community fixture when MSFS is closed and
no installer-managed FS Camera Studio support directory exists. It never writes the real Community package.
It verifies fresh install, repair/backup, uninstall/unknown-file preservation, wrong/unrecognized targets,
autodetection's refusal to follow the developer junction, duplicates, and a named sleeper process guard. For X-Plane
it uses a fake X-Plane folder (an empty `X-Plane.exe` and `Resources\plugins` with another plugin): X-Plane-only
install, clean upgrade with backup, uninstall, wrong and unrecognized folders, both simulators in one run, and the
running-X-Plane guard. It never writes the real X-Plane folder.

For lifecycle testing while MSFS is running:

```powershell
python tools/build-installer.py --test-harness
python tools/test-installer.py --test-harness
```

The harness changes only the compiled process names checked by the guards to test sleeper names. It is emitted
under **build/**, never dist/, and its BUILD-INFO has `testHarness: true`. Do not distribute that harness.
Use the shipping EXE separately to verify the real running-simulator refusal. Both tests preserve evidence
under `build/installer-tests/`. A failed test intentionally leaves evidence for inspection; inspect the exact
registered fixture path before cleanup. Never point the test harness at the real Community folder.

## References

- [MSFS virtual file system](https://docs.flightsimulator.com/msfs2024/retail/devmode/menus/tools_info/virtual-file-system/virtual-file-system-information/): installer discovery through InstalledPackagesPath and Community2024.
- [Inno Setup downloads](https://jrsoftware.org/isdl.php), [script events](https://jrsoftware.org/ishelp/topic_scriptevents.htm), [SHA-256 verification](https://jrsoftware.org/ishelp/topic_isxfunc_getsha256offile.htm).

Installer tests do not replace live MSFS acceptance. Version 0.4.0 adds camera feel (follow horizon, head
movement, mounted cam, binoculars, glance views), cinematic mode, temporary airport vantage points, free drone
cameras with gamepad control, traffic spotting, Pop out and the mini Traffic tab. Profiles saved by 0.4.0 use
schema 4 and cannot be read by 0.3.0. See `doc/test-evidence/2026-11-07-v0.4.0.md`.

For 0.5.2, the isolated harness passed all 17 scenarios; see the
[release validation record and live checklist](../doc/test-evidence/2026-11-07-v0.5.2.md).
The user installed and tested 0.5.2 successfully in both simulators and approved publication on November 7, 2026.

## Windows updater

Setup includes a versioned `FSCSUpdater.exe` under its support folder and registers the per-user
`fscamerastudio://update/<version>` handler. The test harness uses its own protocol and support folder.
The build script compiles the updater with the Windows .NET Framework compiler and reuses a hash-verified
binary when sources match; shipping and test installers therefore contain the same updater.
`BUILD-INFO.json` records its SHA-256 and setup verifies it after installation. Uninstall removes its handler.
