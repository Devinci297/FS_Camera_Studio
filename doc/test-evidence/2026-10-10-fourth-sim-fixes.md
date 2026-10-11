# Fourth MSFS test build

## Implemented

- Main editor drag surface restricted to the custom header; interactive header controls excluded. Native duplicate header hidden. X uses the native panel close API; Compact opens the separate mini surface. Mini Edit opens the main toolbar panel and relays the selected camera to Position & Lens.
- Mini definition uses the installed simulator schema's `Preset PanelLayout="FULL_SCREEN"`, `resizeDirections="None"`, and `ShowOnEvent` for `CHAIN_MESSAGE_FLIGHT_END_LOADING`. The invisible bottom hit strip spans the drawing viewport; the dock stays open over its own controls and the strip. Full-window behavior and automatic opening remain live qualification items.
- Compact centered mini layout, category buttons at the left, category camera buttons at the right, 65% default background opacity and configurable opacity. Category/camera SVG icons with Icons / Labels / Both settings and tooltips.
- Bindings registered on repository startup even when reopening an existing profile. Assignment capture accepts modern physical codes and legacy keyCode/which/key events. Compact searchable/filterable bindings, expandable sections, and direct camera assignment button. Expand/collapse all in both library and Bindings.
- Illustrated axis cards, numeric controls, units, directional slider endpoints, and sliders for position/orientation. Current-view capture can save to the selected camera and requests that camera's reference frame, including exterior frames.
- Explicit mini **Mouse look** mode: visible crosshair, temporary camera offsets, click/Escape/window-blur exit, and ownership/session cancellation. This is an addon mode, not proof that native MSFS right-drag mouse look or intermittent native cursor disappearance is resolved.
- Filled toolbar SVG paths replace stroke-only artwork to avoid native icon recoloring filling the entire silhouette.

## Verification

- Native build: **80 tests passed, 0 failed, 4498 checks**. Includes saved-profile binding registration, temporary mouse offsets/recenter, stale session rejection, capture reference frames, and settings normalization.
- JavaScript: **17 math, 6 bridge, 9 legacy key capture, 2 panel host tests passed**.
- `ui/tests/app.test.cjs`: isolated headless Edge checked category and binding expansion, search/filtering, legacy key assignment, direct camera assignment, six pose sliders, name-display/opacity settings, mini centering and compact height, and mouse-look start/movement/Escape exit. No page errors. Browser demo backend, not Coherent or an active simulator.
- Review images: `build/review/editor-sliders.png`, `bindings-compact.png`, `mini-categories.png`, `toolbar-icons.png`.
- SDK 1.7.3 WASM compilation and package compiler completed, including both SPB definitions.
- Installed Community junction points to `build/package/Packages/fs-camera-studio`. Module and all five UI file hashes match source/build output; all **18 layout entries** exist with matching sizes.
- WASM SHA-256: `91676AB9480ED184D03AA56FC19C15E1F2CC296B2DCC57B52D0F20E2B90D3B7F`.
- Rollback: `build/rollback/before-fourth-sim-fixes-20261010-212613`.
- `git diff --check` passed. Existing uncommitted third-round changes retained; no commit or push made in this round.

## Live test still required

Start a fresh flight; check automatic mini activation, hover coverage at both bottom corners, centering and cockpit click-through. Drag only the custom main header; test X and mini Edit. Assign a key, save, reopen/restart and recall it with the editor closed. Test sliders, current-view capture, explicit Mouse look and cursor recovery. Check icons before/after hover and persistence of opacity/name settings.

The installed simulator schema supplied FULL_SCREEN/ShowOnEvent; native `ToolBarPanels.js` supplied the open/close API. Related Asobo discussion: https://devsupport.flightsimulator.com/t/custom-made-panels-not-generated/13134 . Compilation validates syntax; it does not establish native behavior in a loaded flight.

Future UX suggestions and automatic airport camera generation/loading requirements are saved separately in `doc/ui-ux-backlog.md`; airport camera tools are not implemented in this build.
