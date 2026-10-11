# Fifth simulator feedback pass

Built and installed on October 10, 2026. MSFS was closed before packaging. The existing Community junction points to `build/package/Packages/fs-camera-studio`.

## Changes

- Position controls use directional buttons: left/right horizontally, up/down and forward/back vertically. The six straight pose sliders are removed. Pitch uses a semicircular handle; yaw and roll use circular handles with actual degree geometry, a shaded angle sector, typed values, keyboard adjustment, and per-axis reset.
- The editor fits without inspector scrolling at 1100x1000 and 800x950. Inline camera naming, icon selection beside the name, direct shortcut assignment, and conflict replacement/clearing are implemented.
- Camera/category reordering and icon persistence are covered in browser regressions. Mini-panel hover tolerates Coherent retargeting; native visibility changes refresh panel geometry. Startup rebinds the runtime profile when its UI selection survived an aircraft-load reset.
- Opt-in flight effects are under Motion: turbulence, ground roll, touchdown, acceleration, and engine vibration. Telemetry drives bounded transient offsets rather than editing saved camera poses.
- Mouse look can retain custom-camera ownership, with pan and forward/back movement. A configurable action and native mouse-look-toggle event route to the mini surface. Global native mouse event behavior remains unverified in MSFS.
- The specifically identified old lateral calibration is migrated once to right-positive, preserving existing physical camera positions by negating stored lateral coordinates. Future qualification records use a new identifier.

## Verification

- Native: 85 tests passed, 4541 checks.
- JavaScript: math 17, bridge 6, key capture 9, panel host 3 passed.
- Headless Edge: general UI interactions/layout; icon persistence, reordering and retargeted hover; editor fit, directional button geometry, arc dragging and keyboard operation, axis reset, inline rename, conflict replacement/clear and effects export all passed.
- Preview: `build/review/directional-controls.png`; size checks: `build/review/pose-1100.png`, `build/review/pose-800.png`.
- WASM built with SDK 1.7.3. Compiler reported one non-standard-layout `offsetof` warning for the SimConnect telemetry packet size guard.
- Package compiled successfully. All 18 layout entries exist with matching sizes. Six installed JavaScript/CSS assets and the WASM match source/build hashes; receipt: `build/review/fifth-installed-hashes.json`.
- Installed WASM SHA-256: `B5611EB0912E947912E219AC69509B47ADC1B6D5045252FC36DAF5E819D918D3`.
- Rollback: `build/rollback/before-fifth-sim-fixes-20261010-222649/fs-camera-studio`.

## Flight acceptance still required

Verify mini auto-start and full-width hover after flight loading, runtime profile availability, positive X moving right, Coherent arc drag/keyboard capture, and mouse look while the custom camera remains selected. Test opt-in effects during taxi, flight and touchdown. Browser tests and installed hashes do not establish those live outcomes.
