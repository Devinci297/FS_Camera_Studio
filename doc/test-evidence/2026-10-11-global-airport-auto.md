# Wider panels and global airport auto switching

Local candidate: `0.1.1-test.20261011.3`. Not published to GitHub.

- Mini dock uses the available viewport width with 16px side margins, remains centered and 120px high, and retains independently scrolling lists.
- Icons-only mode retains names on airport camera buttons in both the library and dock; category icons and aircraft-view preferences remain supported.
- Auto switch is in the main app header on every tab and always present in the mini tools. Enabling selects the closest camera in the currently loaded airport set, even from a cockpit view or released camera control. Continued switching retains the existing three-second dwell and distance margin. Manual movement, right-drag and manual camera selection still stop automatic switching.
- No nearby camera set or stale aircraft position rejects activation without moving the camera. The toggle is disabled in the UI when no airport cameras/position are available, with explanatory text.
- Main toolbar definition increased from 56% to 78% default width, 40% to 58% minimum width, and 70% to 86% default height. The compiled main SPB is included. Actual simulator window sizing and saved-window behavior remain live-test items.

Validation:
- 120 native tests, 5,435 checks passed, including nearest selection from cockpit/released control, acquisition in progress, stale position rejection and existing switch dwell/manual override.
- SDK Release WASM build and isolated SDK package compilation passed (existing SDK warnings).
- Packaged global-auto UI checks passed: every editor tab, activation from cockpit, airport labels in icons-only mode, centered compact dock at 750/1200/1920px.
- Packaged flight-controls and airport-generator browser suites passed. Flight-controls fixture now lets queued demo save snapshots finish before injecting synthetic camera data.
- Reviewed `build/review/global-airport-main.png` and `global-airport-mini-750.png` / `global-airport-mini-1920.png`.
- Preserved the verified .2 mini host; staged only scoped app/CSS changes, the new main SPB, version and WASM. Staging script verifies the .2 baseline inventory.
- MSFS absent before installation. Existing Community junction target and prior hashes verified. All 22 installed files match `build/review/global-airport-auto/installed.json`.
- Backup: `build/rollback/before-global-airport-auto-20261011-100150/fs-camera-studio`.

Live simulator acceptance is pending. A browser layout check does not verify MSFS's restored window geometry.
