# Panel polish, A320 starters, and profile transfer

Tester release candidate `0.1.1-test.20261011.4`.

- Aligned compact header with version badge and ellipsized aircraft/profile selector.
- Mini spans the full viewport width, starts at 84px high, and uses adaptive columns/rows to keep categories visible. The 19-category stress fixture is 102px high at 1200px width. Camera lists retain their own scrolling. Category display modes no longer hide any camera names or icons.
- Hover and keyboard-focus help covers main/mini controls and dynamically created dialogs; tooltips never intercept pointer input.
- Mini background alpha is applied as a concrete RGBA value for Coherent compatibility; host wrapper backgrounds and dock shadow removed. Text and buttons remain readable at low background opacity.
- Binding groups start collapsed; the expand/collapse action reflects actual visible groups, including filtered results.
- New airport cameras default to tracking heading and pitch. Existing edited cameras retain their selected tracking mode.
- Runtime input aliases cover letter case and left/right Shift chords while preserving explicit user assignments. Duplicate alias delivery does not double movement.
- A320 starter views use each loaded aircraft's pilot eyepoint and separate model profile. Existing user profiles remain intact. Supported recognition includes iniBuilds/default A320neo, FlyByWire A32NX and Fenix A320; these are adaptable starter framings, not calibrated instrument closeups.
- Export now lists saved profiles and distinguishes saved exports from the current unsaved draft. Import waits for the native asynchronous commit, correctly reads the native report, and adds a distinct copy.

Validation:

- Native build: 128 tests / 5,774 checks passed. Includes airport altitude with tracking, Coarse/Shift multipliers, A320 matching and persistence, arbitrary-profile export, import persistence and invalid/failed-import preservation.
- SDK Release WASM compiled successfully; existing SDK initializer/offsetof warnings remain.
- Packaged UI regression tests cover header bounds, category visibility, collapsed bindings, tooltips, category display modes, opacity, profile transfer, flight controls, airport generation and updater behavior.
- The native importer returns Pending followed by CommitCompleted; the browser transfer test explicitly exercises that contract rather than only the demo's immediate-success path.
- Candidate stages over the verified .3 package, keeping its simulator host and native mouse path. Scoped function/CSS transplants are recorded in `tools/diagnostics/stage-panel-polish.py`.
- Shipping .4 installer passed all nine isolated lifecycle scenarios (fresh install, upgrade/repair, backup, uninstall preservation and unsafe-target/running-simulator guards). Evidence: `build/installer-tests/8e3ed1ed-1e69-4977-9d28-4225331ca75f/results.json`. The actual installed package stayed unchanged.
- Release installer payload matches all 22 files in the installed receipt. Tester ZIP, standalone PDF, build metadata and checksums accompany the installer.

Installation completed with MSFS closed. All 22 installed package files match the staged inventory. Receipt: `build/review/panel-polish-a320/installed.json`. Backup: `build/rollback/before-panel-polish-20261011-104454/fs-camera-studio`.

Browser/native checks do not prove live simulator rendering or input delivery. Recheck actual Q/E/Shift, mini transparency and hover, and A320 framing in MSFS.
