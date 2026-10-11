# In-simulator qualification — 0.3.0

Automated native and browser tests do not prove live simulator behavior. Record pass/fail, steps, MSFS build,
aircraft, controller, scenery, package version and screenshots in `doc/test-evidence/`. The latest T.16000M
hat-release fix has automated coverage and still needs physical-controller confirmation.

## Setup

Install the release setup with MSFS closed, start a parked Free Flight, and open both toolbar panels. Confirm
**0.3.0** in the app bar. Developer reproduction and package receipts are described in the
[README](../README.md) and [release evidence](test-evidence/2026-10-11-v0.3.0.md).
Camera directions are configured automatically; there is no user axis-calibration step.

## Hosting and navigation

| Check | Expected result |
| --- | --- |
| Open, close and reopen main and mini panels | Both load; selection and pending edits remain consistent |
| Expand categories and airport group; search and clear search | Category headers and indented cameras are distinct; labels and counts remain readable |
| Compact dock on narrow and wide screens | Bottom placement, visible categories, scrollable crowded camera list, accessible global actions |
| Toggle a camera star; change mini category display | Only starred cameras appear in mini; main always shows icons and names |
| Open assignment, tracking, walkaround and confirmation dialogs | Content scrolls above visible action buttons; no unwanted full-screen opaque panel |
| Test opacity, reopen and resize | Main and mini alpha settings are independent; scene is visible without native blur |
| Click cockpit controls around visible and hidden panels | No invisible hit regions block simulator controls |

## Input and camera behavior

| Check | Expected result |
| --- | --- |
| Keyboard Primary and controller Secondary for the same camera | Either recalls it; replacing or clearing one preserves the other |
| Select T.16000M, Listen, press/release a normal button, Assign | Correct selected-device button recorded; no camera action during capture |
| Listen, move each cardinal hat direction and return to center | One hat direction captured and listening ends; Assign remains visible |
| Immediately listen for a button after capturing a hat | Button capture still works; no stuck capture or held movement |
| Cancel listening, change devices, unplug/replug, wait for timeout | Capture stops safely; late events do not fill a later dialog |
| Bind a hat direction to movement, then center it | Movement stops; no continued drift |
| W/S, A/D, Q/E and Shift with a pitched view | View-relative forward/side movement; Q up, E down; Shift 4x faster |
| Hold/release editor adjustment buttons, then lose focus | Repeats while held and stops promptly on release/focus loss |
| Right-drag, release, change camera and return | Per-camera temporary-look release/recall settings honored |
| Wheel zoom, pause, active pause, menus and Release camera | No latched translation or zoom; simulator can recover camera control |
| Capture current view, review, save and recall | Reviewed pose matches the scene and remains after restart |
| Orbit and animated fly-by while parked; timed walkaround | Correct mode controls, animation, pause/stop and sequence looping |

## Airport cameras

Generate and save cameras for two airports. Confirm scenery names and ICAO, tower fallback labeling, separate
shared storage, nearby-radius loading and explicit airport selection. Switch aircraft: the same nearby airport
views should remain available. Regenerate with replacement after acknowledging the warning; other airports
and aircraft profiles must survive.

Test global heading + pitch tracking with head tracking off, then each per-camera override. At airports with
uneven terrain, compare near/distant targets and higher/lower elevations. Horizon protection should level distant
lower targets while retaining heading; disabling it restores strict pitch. Check reported world height after
terrain-floor correction. Terrain/building occlusion cannot be eliminated by tracking.

Test category heights, explicit height overrides, auto-zoom framing/FOV limits/response, manual zoom restoration
when auto zoom is disabled, and save/reload inheritance. Auto switch should select and highlight the nearest
eligible airport camera while control is active and retain its global setting across camera changes.

## Profiles, saves and updates

Import JSON through the imports folder and pasted code; export a selected saved profile and the active draft.
Copy a profile to a different loaded aircraft, including a non-Cessna model; verify distinct IDs/association,
source preservation and expected need to adjust cockpit positions. Save and restart MSFS to check persistence.

Measure Ctrl+S responsiveness on a large library, verify one completion notification, and listen for the native
confirmation chime (including panels hidden). A successful sound request is not proof of audible output.
Test update checks and installer upgrades separately; installation must refuse a running simulator and preserve
profiles, shared airport libraries, settings and bindings.

Exterior views can retain cockpit audio. Starter poses and scenery-generated cameras require adjustment for
the selected aircraft and scenery. Report unresolved limitations alongside passing checks.
