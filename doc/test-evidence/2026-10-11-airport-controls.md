# Airport controls and view-relative movement — October 11, 2026

## Changes

- WASD follows the camera's current orientation, including pitch and temporary right-drag look. Q/E remains world vertical; Shift boosts the configured movement speed. Airport tracking supplies the rendered orientation as the movement basis.
- Airport position comparisons use a geographic epsilon small enough to accept millimetre-scale movement. Position edits and manual movement retain world-fixed camera placement.
- Each airport view supports fixed direction, aircraft heading only, or heading and pitch. The Track button is available beside Mouse look in the editor and mini panel.
- Auto switch chooses the nearest camera in the loaded airport. It uses a 3-second dwell and a 10 m / 15% improvement margin. Manual camera selection, movement, right-drag, leaving airport control, or losing active control stops it. This is a session toggle, not an automatic flight-start action.
- The mini toolbar has Auto switch and Save. Camera lists and categories scroll inside a fixed-height dock; the toolbar scrolls horizontally when narrow. Airport subcategories appear under an Airport parent.
- Ctrl+S defaults to Save adjusted view and all changes, with one-time conflict-aware migration. The action is editable in Bindings. Saving bakes temporary position/look/zoom before committing pending profile, airport, settings and binding edits.
- The 20 View ID ceiling is removed throughout native validation, binding parsing and UI allocation. IDs are nonnegative signed 32-bit integers; 0 remains unassigned. This removes the legacy binding-slot ceiling, not the profile's existing camera-count limit.
- Per-camera temporary look is isolated by camera identity. Mouse look opens release/recall options. Return to saved view on release also handles panel/focus cancellation.
- Head tracking has per-camera simulator default/on/off, an editor/mini toggle and an assignable command. MSFS TRACK IR ENABLE is read and written, acknowledged through readback, and the original global setting is restored on release unless changed externally. Native head poses remain MSFS-owned.
- Airport creation uses explicit selectable camera-type cards, labeled metre fields, nearby/ICAO selection, previews and selective save.

## Build and checks

- Native Debug suite: **116 passed, 0 failed, 5397 checks**.
- Isolated WASM build: `build/wasm/airport-controls-20261011/FSCameraStudio.wasm`.
- Browser flight-controls regression passed against the exact staged main/mini HTML: head preference, temporary-look options, Ctrl+S commit, 21st binding slot, auto-switch command, Save visibility, aircraft tracking, 100-camera scrolling, and last-camera reachability at 1100x1000 and 750x900.
- Airport dialog regression passed against staged HTML: three library creation actions, on-demand discovery, XML parsing, batch ICAOs, feature preview and selective save.
- Existing movement-bindings regression passed against staged HTML: defaults, precision persistence, key/wheel reassignment, conflict replacement, clearing and saved snapshots.
- JavaScript syntax checks and Git whitespace checks passed.
- Visual inspection: `build/review/airport-generator.png`, `airport-controls-mini.png`, `airport-controls-mini-narrow.png`.

Candidate: `build/diagnostic/airport-controls/fs-camera-studio`.
Receipts: `build/review/airport-controls-20261011/staged.json`; guarded installation writes `installed.json` with backup and verified inventories.
The candidate preserves the known-loading host, panel HTML and remaining assets. Only app.js, app.css, bridge.js, WASM and layout change.

## Live acceptance still required

Offline tests do not establish flight-loading stability, physical TrackIR response, MSFS scenery delivery, world-camera movement direction or live mouse smoothness. Test flight loading first, then right-drag and pitched WASD, Q/E, temporary reset/recall, Ctrl+S followed by camera recall, generated airport movement/tracking/auto-switch, and head-tracking on/off with hardware. Verify cockpit clicks and stock toolbar remain available.

## References consulted

- [MSFS miscellaneous variables](https://docs.flightsimulator.com/msfs2024/retail/programming-apis/simvars/miscellaneous-variables/): TRACK IR ENABLE is writable; verified the page's checkmark column and local SDK Vars API declarations.
- [MSFS camera data](https://docs.flightsimulator.com/msfs2024/html/6_Programming_APIs/WASM/Camera_API/FsCameraData.htm): world-position camera representation.
- Comparative airport tracking and nearest-view references are retained in the local research notes.
  Camera Studio's dwell and distance-margin rules are independent implementation choices.
