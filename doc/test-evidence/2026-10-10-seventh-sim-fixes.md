# Seventh simulator feedback pass

## Implemented

- Replaced the Appearance and Category/camera names native selects with direct Dark/Light and Icons/Labels/Both buttons. Choices retain the existing settings fields and selected-state feedback.
- Added the name/icon color palette directly to camera and category action dialogs, including right-button and keyboard context entry points. The editor's Color dialog remains available.
- Camera rows and category headings use explicit block widths and reserved control gutters instead of nested wrapping flex layouts. Live/view-ID badges occupy a separate line; category labels truncate instead of breaking into single characters.
- The camera tree owns the sidebar's scrollable area. Add camera stays at the bottom; reordering auto-scroll targets the tree. Re-rendering restores the scroll position.
- Remote selection notifications no longer expand categories. Deletion retains a valid selection and preserves collapsed groups even when a fallback selection is needed.
- Native hit rectangles are clamped to the actual viewport, including tiny loading windows, preventing top > bottom.

## Mini panel regression and unresolved mouse input

The installed sixth-build log reports `viewport=67x67`. Its preset-only registration did not establish a full-screen native viewport. Restored explicit 100/100 dimensions as a fallback and moved the horizontal center anchor onto the panel definition. A fresh mini native ID avoids reusing the 67px saved rectangle. This is a recovery candidate, not evidence of full-window coverage. The older 100/100 build also had limited coverage; full-width positioning remains unresolved and requires native-host verification.

Default right-button hold/drag mouse look is NOT fixed in this pass. The existing MOUSE_LOOK_TOGGLE notification and HTML overlay do not supply native drag movement outside the panel. SDK inspection found only position/rotation/FOV submission and interaction/above-ground flags in MSFS_Camera.h; the documented AXIS_PAN events set camera-axis angles and cannot safely be assumed to be raw mouse deltas. No speculative native camera writes or new global input hooks were introduced.

## Verification

- Headless Edge general UI, editor, library, and settings/library suites passed. New coverage exercises direct settings choices, save/snapshot retention, Add camera placement, minimum label width, and deletion of both the selected and another category while all groups are collapsed.
- Math (17), bridge (6), key capture (9) and panel host (5) checks passed. Host regression includes a 67px viewport with a valid bounded hit rectangle.
- No native source changes in this pass; the previously built sixth-pass WASM is reused.
- Initial installation stopped before mutation because MSFS was running. After the user confirmed closure, package compilation completed with a clean compiler report. The existing Community junction now serves this build. Verified six shared UI files, four generated host files, and the unchanged WASM against their sources; all 18 layout entry sizes match. Receipt: `build/review/seventh-installed-hashes.json`. Rollback: `build/rollback/before-seventh-sim-fixes-20261010-233006/fs-camera-studio`.

Live acceptance is still required for Coherent rendering, mini hover geometry, native input, and category state during cross-panel events.
