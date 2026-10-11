# Tester installer

The Windows installer deploys the **verified packaged build**, not a fresh compile of the working tree.
This preserves the reviewed simulator host and its native pointer routing.
The package's 26 files remain byte-identical to the selected release receipt. No user profiles or development
configuration are included. Setup works offline, per user, without the MSFS SDK or a background service.

## Build

```powershell
tools\get-installer-compiler.ps1
python tools/diagnostics/stage-v0.3.py --freeze
tools\build.ps1 -ModuleOnly -OutputDirectory build/wasm/release-0.3.0
python tools/diagnostics/stage-v0.3.py
python tools/build-manual.py
python tools/build-installer.py --version 0.3.0 --package build/release/0.3.0/fs-camera-studio --receipt build/release/0.3.0/staged.json
```

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

Silent deployment: `Setup.exe /VERYSILENT /SUPPRESSMSGBOXES /NORESTART /COMMUNITY="D:\MSFS\Community" /LOG="setup.log"`.
No install target is accepted without validation. This release is unsigned; signing needs a publisher certificate.
Sign the EXE before regenerating distribution checksums/ZIP for any future signed release.

## Regression checks

`python tools/test-installer.py` tests the shipping EXE in an isolated Community fixture when MSFS is closed and
no installer-managed FS Camera Studio support directory exists. It never writes the real Community package.
It verifies fresh install, repair/backup, uninstall/unknown-file preservation, wrong/unrecognized targets,
autodetection's refusal to follow the developer junction, duplicates, and a named sleeper process guard.

For lifecycle testing while MSFS is running:

```powershell
python tools/build-installer.py --test-harness
python tools/test-installer.py --test-harness
```

The harness changes only the compiled process name checked by the guard to a test sleeper name. It is emitted
under **build/**, never dist/, and its BUILD-INFO has `testHarness: true`. Do not distribute that harness.
Use the shipping EXE separately to verify the real running-simulator refusal. Both tests preserve evidence
under `build/installer-tests/`. A failed test intentionally leaves evidence for inspection; inspect the exact
registered fixture path before cleanup. Never point the test harness at the real Community folder.

## References

- [MSFS virtual file system](https://docs.flightsimulator.com/msfs2024/retail/devmode/menus/tools_info/virtual-file-system/virtual-file-system-information/): installer discovery through InstalledPackagesPath and Community2024.
- [Inno Setup downloads](https://jrsoftware.org/isdl.php), [script events](https://jrsoftware.org/ishelp/topic_scriptevents.htm), [SHA-256 verification](https://jrsoftware.org/ishelp/topic_isxfunc_getsha256offile.htm).

Installer tests do not replace live MSFS acceptance. Version 0.3.0 includes global airport defaults and overrides,
cross-aircraft profile copying, compact library navigation, dual bindings and controller hat-release handling.
The latest T.16000M fix still needs live hardware verification; see
`doc/test-evidence/2026-10-11-v0.3.0.md`.
