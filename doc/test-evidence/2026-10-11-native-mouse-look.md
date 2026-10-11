# Native right-drag mouse look and release behavior

## Implementation

- Uses the SU6 native SimConnect mappings `mouse:0:button:1`, `mouse:0:XAxis`, and `mouse:0:YAxis` in a separate, nonmasking input group. This does not depend on the mini panel opening or covering the viewport. Source: [Asobo support announcement](https://devsupport.flightsimulator.com/t/mapinputeventtoclientevent-mouse/9489/6), August 20, 2026. The input syntax table is preserved in `build/review/asobo-su6-mouse-input.png`.
- Native button and signed axis events feed temporary look offsets only while the right button is held and the custom camera is active. Camera ownership and the saved pose remain unchanged. Axis samples are applied once, not integrated as a persistent rate. Settings > Look speed scales sensitivity.
- Camera > Behavior > After right-drag mouse look offers **Keep look** (default) and **Return to saved**. Optional profile field `mouseLookOnRelease` is `retain` or `reset`. Reset recenters temporary look on release after an actual drag. A stationary right click does not recenter unrelated temporary look. Saving the current view remains explicit.
- Recall, pause edges, camera ownership loss, and panel focus cancellation discard a held gesture. UI pointer blocking aggregates visible panel leases, allowing camera/category right-click menus without moving the camera. Hidden or expired panels cannot hold a permanent input block.
- The Mouse look button now explains the native gesture. The old HTML overlay never activates or expands the native panel hit rectangle. No global OS input helper is used.
- Mapping failures disable native mouse input and are logged. Diagnostics include mapping readiness, right-button acceptance/UI blocking, and the first two axis samples per gesture. Runtime axis direction, scale, and event delivery still require live verification.

## Offline validation

- Native build/tests: **91 passed, 0 failed, 4,613 checks**. Covers cockpit/exterior, inactive input, signed deltas, button release, no stale motion, panel blocking, lease expiry, pause, mapping failure, preset preservation, and per-camera retain/reset semantics.
- Headless Edge: app, editor, library, settings/library suites passed. New Behavior choice and exported profile value verified; screenshot `build/review/mouse-release-behavior.png` inspected.
- Math (17), bridge (6), key capture (9), and panel host (5) checks passed. Native mouse look does not create a scene-wide HTML hit region.
- WASM SDK 1.7.3 compile succeeded. Existing aggregate-initializer and SDK-layout warnings remain.

## Live acceptance

Select a custom view with **View in sim**. With the pointer over the simulator scene, hold the right mouse button and drag horizontally and vertically; release it. The selected custom camera should remain active. Test both Behavior choices, and save/reopen the profile to check persistence. Repeat with the mini panel hidden and with the editor closed. Right-clicking Camera Studio camera/category entries should open their menus without moving the view.

Native event delivery and direction are not established by offline tests. If the gesture fails, inspect the `mouse` lines in the simulator package work folder's `fscs-diagnostics.log`; these distinguish unsupported mappings, UI suppression, and missing axis samples. The existing mini-hover geometry issue is separate and is not claimed fixed by this change.

## Installation

Package compilation completed with a clean report while MSFS was closed. The existing Community junction serves the new build. Verified 11 shared/generated UI files plus the WASM against staged/source files and all 18 layout file sizes. Receipt: `build/review/native-mouse-installed-hashes.json`. WASM SHA-256: `b31b0da5964e0c78433b6c5e7594bfa911fe95c3960bbefd920b78b3a2494142`. Rollback: `build/rollback/before-native-mouse-20261010-234656/fs-camera-studio`. Live flight testing remains pending.


## Loading crash and rollback

User reported a crash during loading immediately after this installation. Windows Event 1000 and AsoboReport-Crash record an access violation (0xC0000005) in CoherentUIGT.dll at offset 0x9b99a, at 2026-10-10 23:52 America/Chicago (2026-10-11T04:52Z). The stack contains CoherentUIGT and CoherentGTJS frames. FSCameraStudio.wasm was Ready/Idle; diagnostics show mapping registration succeeded, the profile resolved, and ownership remained Observing. No right-button/axis or camera-acquisition events were recorded. This narrows timing but does not establish addon responsibility or exclude its startup path.

Preserved crash report, WER report, and module diagnostics under `build/review/loading-crash-20261010`. Restored the full previous package from `build/rollback/before-native-mouse-20261010-234656/fs-camera-studio`, verifying all 20 files. The native-mouse package is preserved at `build/rollback/native-mouse-loading-crash-20261010`. Profile stores were not changed. Receipt: `build/review/loading-crash-20261010/rollback-receipt.json`.

The native mouse/release-choice sources remain in the workspace but are no longer installed. The previous package must be tested by loading the same flight before attributing the crash or deploying another candidate. No speculative startup changes were deployed as a claimed fix.


## Successful rollback load and isolated candidate

The user confirmed the restored package loaded successfully. Read-only diagnostics also show camera acquisition and use in that session. This implicates changes in the new build but does not prove which change caused the Coherent access violation.

Prepared a narrower candidate: native mouse mappings are no longer created in SimSession::setup. ModuleApp requests them only after camera ownership becomes Active, with simulation running and not paused. Pause, simulator takeover, camera release, and shutdown turn the native group off. Mapping rejection suppresses further retries for that connection. The UI and retain/reset behavior are unchanged from the failed candidate so this trial isolates input-registration timing.

Native tests: 92 passed, 0 failed, 4,624 checks. New regression proves no mouse enable during loading or pending acquisition and no repeated enable per frame; covers pause/resume, takeover, release, another flight, and shutdown. The working installed package stays on the rollback while the user's flight is active. This candidate is not a confirmed crash fix.

The user subsequently closed MSFS and authorized installation. The deferred-mapping candidate compiled and installed with a clean SDK report. Verified 12 UI/WASM files and 18 layout sizes through the Community junction. Receipt: `build/review/native-mouse-lazy-installed-hashes.json`. WASM SHA-256 `55ceb9f778e3738ae9551def553317d8303ff423cf6071e1bf57204ece29031e`. Live loading, acquisition, and right-drag checks are pending.


## Second crash: input timing hypothesis rejected; module-only isolation installed

The deferred-registration candidate also crashed late in flight loading, before camera activation. Asobo crash timestamp 2026-10-11T05:06:14Z (00:06:14 America/Chicago). All nine reported CoherentUIGT/CoherentGTJS stack entries match the first crash, including fault offset 0x9b99a. Diagnostics confirm ownership Observing and no native mouse mapping initialization. This excludes startup registration as the necessary trigger; root cause is still unknown.

Saved the second crash report and diagnostics in `build/review/loading-crash-20261011`. Restored and hash-verified all 20 files of the successful package, preserving the failed candidate at `build/rollback/native-mouse-lazy-loading-crash-20261011`.

Prepared and installed a controlled diagnostic composition: the successful package with ONLY `modules/FSCameraStudio.wasm` replaced by the deferred-input module (SHA-256 55ceb9f778e3738ae9551def553317d8303ff423cf6071e1bf57204ece29031e), plus its size/date in layout.json. Every other file, including generated HTML, JavaScript, CSS, SPB definitions, icons, and manifest, is byte-identical to the package the user confirmed loads. MSFS was closed; all 20 staged/installed file hashes verified through the Community junction. No SDK recompilation needed because the compiled panel definitions are unchanged. Receipt: `build/review/loading-crash-20261011/isolation-installed.json`.

This is an isolation build, not a claimed fix. Its old UI intentionally does not show the new release-behavior choice or send PointerOverPanel messages. First acceptance step is loading the same flight, without activating mouse look. If successful, investigate the new panel UI/native callback changes; if it still fails, narrow the native module changes. Profiles and user settings are untouched.


## Module-only live result and measured input correction

User confirmed native right-drag works with the byte-identical old UI plus the new module, but reported excessive speed/jerk and unreliable MSFS top-toolbar hover. The successful live log is preserved at `build/review/mouse-smoothing-20261011/live-before.log`. It shows repeated mouse-button press/release pairs with accepted axis events; small movements arrive as 642-unit steps (also 1285, 1927, etc.), not single-pixel counts. The old conversion saturated these at 20 degrees per sample. This strongly narrows the loading regression to new UI/native-callback changes, though its exact trigger is still unproven.

Corrected conversion: normalize signed input by 32768 and scale to six degrees at full range at the default Look speed of 60. A 642-unit step targets 0.11755 degrees. Frame-rate independent exponential easing uses a 55 ms time constant. Pending deltas clear on release, cancellation, UI blocking, camera switch and pause so there is no post-release drift or catch-up jump. Keep/reset behavior remains in the native model.

Split mouse axes into their own input group. Axis input stays disabled during ordinary hovering and is enabled only while the right button is held; release/pause/ownership loss disable it. This removes our always-on axis subscription as a possible source of unreliable native toolbar hover. Toolbar recovery is still a live acceptance item, not a verified fix.

Native tests: **93 passed, 0 failed, 4,939 checks**, including the measured input scale, smooth first frame, equal motion at 30/120 Hz, no release drift, existing reset/retain and ownership cases.

## Cockpit clicks and toolbar hover remain blocked after release

The user confirmed that neither Release camera nor switching off Mini control panel restores cockpit clicks or top-toolbar hover. Diagnostics show camera ownership returned to Observing and the mini hit rectangle became zero. These observations make an active camera or visible mini controls insufficient explanations.

Inspection of the installed stock ingame-ui.js and ToolBarPanels.js shows a separate global panel-hover claim through pushPanelMouseOver/pushPanelMouseOut, independent of UPDATE_PANEL_RECT. The full-size mini root remained pointer-targetable even when its children were transparent. Prepared two minimal host changes on the proven-loading UI: disable pointer hits on the empty mini root and its content wrapper while retaining real mini/dialog controls, and explicitly release the global hover claim once when the panel becomes inactive, invisible, or mini mode is off. This is a testable hypothesis, not a confirmed live fix.

The staged package retains the proven-loading UI except for those two host changes. It does not include the newer PointerOverPanel startup changes implicated in the crash trial. Four files differ from the last installed diagnostic: WASM, msfs-panel.js, panel-host.css, and layout.json. WASM SHA-256: f1240a2ac04908eaed94d13463d19547179dbb190180602455bd0d3a97ebd89c. Receipt: build/review/mouse-smoothing-20261011/staged.json.

Five panel regression tests pass against both the source host and the actual patched legacy host. A headless Edge test confirms top/scene clicks pass through the transparent host while mini controls remain clickable, including after hiding the mini. Whitespace checks pass. The staged candidate has not yet been installed; live recovery, loading stability, and mouse sensitivity remain pending.

MSFS was subsequently confirmed closed by process inventory. Installed the four changed files after verifying the prior package against the successful diagnostic baseline and backing it up at build/rollback/before-mouse-smoothing-20261011-003058/fs-camera-studio. All 20 installed file hashes match the staged receipt, all 18 layout sizes match, and the Community junction resolves to the verified installation. Receipt: build/review/mouse-smoothing-20261011/installed.json. User settings and profiles were not changed. Live acceptance remains pending: load a flight, test cockpit clicks/top-toolbar hover with the mini off and on, then test right-drag movement and clicks after release.


## Live acceptance of smoothing and pointer fixes

User confirmed on October 11 that cockpit clicks and toolbar hover work with the mini both on and off, and mouse look is smooth. This verifies the installed diagnostic composition. Future UI changes must retain the proven host handling.

## Intermittent rough drag returns; isolated dispatch-registration trial

The user subsequently reported rough right-drag even with both panels hidden and the pointer over the scene. This supersedes the earlier smoothness acceptance: cockpit/toolbar recovery remains reported, but consistent mouse smoothness is not accepted. Read-only log capture is `build/review/mouse-dispatch-once-20261011/live-before.log`. The captured gestures remain accepted with `blockedByPanel:false`, without repeated camera ownership changes. The old log has no per-frame timing or rejected-axis counters, so it cannot establish whether native camera competition or input timing caused the roughness.

Found a definite WASM lifecycle mismatch: `SimSession::dispatch` called `SimConnect_CallDispatch` every frame. The [current SDK communication guidance](https://docs.flightsimulator.com/msfs2024/retail/programming-apis/simconnect/api-reference/communication/communication-api/) specifies that WASM callbacks are invoked automatically; looping the registration is unnecessary. An [Asobo engineering response](https://devsupport.flightsimulator.com/t/total-execution-time-grows-slowly-when-using-simconnect-in-wasm/7320) documented increasing execution time from repeated registration in an older build. That historical defect is evidence for a targeted trial, not proof that the same simulator defect remains in this build.

Production source now registers once per successful connection, handles registration failure, and drains only staged events each frame. Source also ignores repeated right-down events during an accepted gesture instead of resetting its smoothing accumulator, and records per-gesture frame/sample gaps, rejected samples, maximum raw input, and concurrent bound look. These extra source changes are **not** in the isolated installed trial.

To avoid combining unfinished airport/UI work with this investigation, relinked all preserved `mouse-smooth-review` object files in their original order. The unmodified relink reproduces the installed baseline byte-for-byte: SHA-256 `f1240a2ac04908eaed94d13463d19547179dbb190180602455bd0d3a97ebd89c`. A diagnostic-only linker wrapper suppresses identical repeated callback registrations until the connection closes. It preserves registration failure retries, changed callback contexts, and handle reuse after reconnect. No mouse masking, sensitivity, smoothing, UI, or camera behavior was changed in this trial.

Validation: executed the actual compiled WASM wrapper with mocked SDK imports for 216,000 frame calls (one hour at 60 FPS); exactly one registration, with reconnect/context/failure cases passing. Native suite: **95 passed, 0 failed, 4,956 checks**, including repeat-down and diagnostic-counter regressions in source. Production SDK registration and live smoothness still require simulator acceptance.

Installed with MSFS closed, checked twice by process inventory, after hash-verifying and backing up the complete previous package. Only `modules/FSCameraStudio.wasm` and `layout.json` differ. All 20 file hashes match through the Community junction and all 18 layout sizes match. Receipt and rollback path: `build/review/mouse-dispatch-once-20261011/installed-awaiting-live-test.json`. Installed trial SHA-256: `cf3c0c25d7c40e476cbcf9b94059d225e6261739518ec9f39801348f1b62b8b4`.

Next live acceptance: load the same flight; compare short right-drags immediately and again after several minutes, with both panels hidden. Verify cockpit clicks and top-toolbar hover after release. If roughness persists, registration alone is insufficient; use the pending timing diagnostics to investigate dropped/out-of-range samples and simultaneous camera input rather than layering speculative masking changes.
