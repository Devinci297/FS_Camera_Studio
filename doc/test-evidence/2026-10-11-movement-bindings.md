# Movement and zoom bindings — October 11, 2026

Candidate: `build/diagnostic/movement-bindings/fs-camera-studio`.
Baseline: verified installed mouse-range candidate, accepted by the user as smooth so far.
Only the module, app.js, demo bridge.js and layout.json change. Panel host files, loading behavior, styling,
mini picker removal and the corrected native mouse counts are retained. The newer full airport UI is excluded.

## Behavior

- Bindings → Manual look & move shows editable W/S, A/D, Q/E, Shift and zoom defaults.
- Assign/Replace/Clear use the existing transaction, persistence and conflict checks.
- Shift alone can be captured for Faster movement; another key or controller can replace it.
- Fine/Normal/Coarse now persist from the editor buttons and scale keyboard speed by 0.1/1/10.
  With the default 0.5 m/s base, Coarse is 5 m/s and Coarse plus boost is 20 m/s.
- Zoom defaults observe MSFS zoom commands without masking simulator input. Assign can select either wheel
  direction, keyboard or controller. Default wheel delivery is **not yet verified in MSFS**.
- Native zoom pulses change FOV by 2 degrees, clamp to limits without accumulating hidden excess, and ignore
  text focus, pause, released cameras and simulator takeover. No pointer overlay is added.

Source for the zoom event names: [MSFS View/Camera Events](https://docs.flightsimulator.com/html/Programming_Tools/Event_IDs/View_Camera_Events.htm).
The SDK mouse input table does not document a raw wheel axis, so no guessed raw-wheel mapping is used.

## Automated verification

- Native suite: 108 passed, 0 failed, 5279 checks.
- Browser regression: default labels, precision persistence, key and wheel reassignment, Shift-only capture,
  atomic conflict replacement, clearing and saved snapshots. Run against source and staged baseline UI.
- Legacy key capture regression remains passing.
- SDK module build and installed-file inventory are recorded in the review receipts.

## Live checks still required

1. Load a flight; confirm cockpit clicks, top toolbar hover, mini panel and smooth right-drag remain working.
2. Activate a custom camera. Compare Normal and Coarse with W/A/S/D and Q/E; hold Shift for boost.
3. Reassign Move forward to I and Clear Move down. Save and restart; W and Q must no longer move the custom camera.
4. Scroll over the scene to zoom. If it does not respond, verify MSFS's wheel zoom assignment and report it;
   the native event delivery path remains unqualified until this passes.
5. Scroll cockpit knobs and panel lists. Confirm only the intended knob/list responds.
6. Assign Zoom in to a key, clear Zoom out, and verify the previous wheel directions stop affecting Camera Studio.

Installed after verifying MSFS was closed. All 20 package hashes and 18 layout entries matched the candidate.
Receipt: `build/review/movement-bindings-20261011/installed.json`.
Rollback: `build/rollback/before-movement-bindings-20261011-20261011-055402/fs-camera-studio`.
Source and staged main.html browser regressions both passed. Live simulator behavior remains unverified.
