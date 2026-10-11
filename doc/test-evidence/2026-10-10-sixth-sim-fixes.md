# Sixth simulator feedback pass

## Observed and changed

- The flight diagnostics reported a 1342 x 1342 mini viewport, explaining its right-offset placement and limited hover coverage. Removed explicit default width/height from the full-screen panel preset and changed its native ID to `PANEL_FSCS_CAMERA_MINI_FULLSCREEN` to avoid reusing the saved square rectangle. Asobo's guidance confirms the dimension attributes must be omitted for a full-screen viewport: https://devsupport.flightsimulator.com/t/su12-beta-in-game-panels-can-now-be-wider-than-the-simulation-viewport/5282 . Native panel positioning still needs flight acceptance.
- Mouse look used the `inset` shorthand and listened only on its overlay. It now has explicit bounds, handles retargeted document mouse events, excludes the top 12% (at least 96px in native hit testing) from capture, hides the editor when starting, and stops when the editor reopens. Escape is handled at document capture level; click and blur also stop capture. The upper toolbar remains outside the native mouse-look rectangle.
- Reordering uses ordinary mouse events on both rows and grips, capture-phase move/up handlers, geometric target lookup, Escape/blur cancellation, and scoped click suppression. Right-button mouseup and keyboard context actions supplement contextmenu events in both surfaces.
- Camera names occupy their own flex line; Live and view-ID badges sit below, with explicit icon margins for Coherent. Direction buttons no longer cover illustration arrowheads.
- Categories may store `defaultCameraId`. The mini category button recalls it, with first-camera fallback for older profiles. The native Next Category action also prefers it. Moving/deleting that camera clears the stale category reference; profile ID reassignment remaps it.
- Camera/category names and icons support palette colors. Cameras inherit category colors unless individually overridden. Hex colors and category defaults are validated and retained in profile export. The editor has Color and Make category default buttons; category context actions include Choose color.

## Verification and delivery

- Native: 86 tests, 4547 checks passed, including category default validation/remapping and color validation.
- JavaScript math, bridge, and key-capture suites passed; 4 host regressions passed, including toolbar exclusion and capture release when the editor reopens.
- Headless Edge editor, general interaction, and library suites passed. Added coverage for mouseup-only context actions, inherited/overridden color export, stale-default cleanup during drag, category-default recall, explicit overlay dimensions, and retargeted mouse motion. Compact editor fit checked at 1100x1000 and 800x950.
- WASM built with SDK 1.7.3. Warnings: default string aggregate initializer omitted (value-initialized), and existing SimConnect non-standard-layout offsetof warning.
- Packaging initially stopped before mutation because MSFS was running. After the user confirmed closing it, package compilation succeeded and updated the existing Community junction target.
- Verified six UI assets and WASM hashes, the new mini host ID, and all 18 layout entry sizes. Receipt: `build/review/sixth-installed-hashes.json`.
- WASM SHA-256: `865864FDC866268114D43D15CB74833EB0CA17BD615C35215633416DA7B9BA49`.
- Rollback package: `build/rollback/before-sixth-sim-fixes-20261010-225521/fs-camera-studio`.

Next flight: confirm actual full-width hover/centering, mouse motion, toolbar/editor click recovery, row reordering, and right-click menus in Coherent. Browser and installation checks do not establish live simulator acceptance.
