# Airport camera test build — October 11, 2026

**Flight-loading failure and rollback:** the user reported another crash after installation. The report at
2026-10-11T06:36:01Z has access violation `0xC0000005` and the same nine CoherentUIGT/CoherentGTJS frames as
the preceding loading crash (first offset 637338 / `0x9b99a`). Diagnostics show ownership Observing, no native
mouse gesture/acquisition, and the last event "A flight was loaded". This rejects the deferred incoming
CommBus handling change as a sufficient loading fix; it does not establish a root cause or prove the airport
generator itself caused the crash. The module is reported Ready/Idle in the simulator report.

Restored all 20 files from the package preceding this airport trial, with MSFS stopped, and verified every
hash through the Community junction. Evidence, the failed package and rollback receipt are in
`build/review/airport-loading-crash-20261011/`. Airport generation is therefore **not in the current installed
package**. The full source and candidate remain preserved; do not reinstall them as a loading fix without
further isolation and live acceptance. The prior callback-registration diagnostic is restored, not newly
live-qualified by this rollback.

The user subsequently confirmed the restored package loads. This qualifies that rollback's loading only;
it does not yet qualify the full airport module or the new panels. Prepared the next controlled candidate at
`build/diagnostic/airport-module-only/fs-camera-studio`: unchanged loading UI, SPBs and icons, with only the
failed trial's WASM module and its layout metadata replaced. The receipt at
`build/review/airport-module-only-20261011/staged.json` verifies that only those two files differ.
This candidate is not installed, and intentionally has no airport-generator controls in its old UI.
If it loads, the next isolation target is the panel changes; if it fails, inspect the native module changes.

Subsequent installation: after the user closed MSFS, installed the module-only candidate at 05:12 local time
on October 11. Only `modules/FSCameraStudio.wasm` and `layout.json` changed. All 20 package hashes and 18 layout
sizes verified through the Community junction; all UI assets remain byte-identical to the confirmed-loading
baseline. Receipt: `build/review/airport-module-only-20261011/installed.json`. Rollback backup:
`build/rollback/before-airport-module-only-20261011-051222/fs-camera-studio`. Flight-loading acceptance pending.

The user subsequently confirmed this module-only candidate reaches the cockpit. Mouse smoothness failed
acceptance; investigation found discarded large mouse samples. See `2026-10-11-mouse-range.md`.
Airport generation UI and actual world-camera use remain unqualified.

Transport research: the [official fsCommBusCall documentation](https://docs.flightsimulator.com/msfs2024/retail/programming-apis/wasm/communication-api/fscommbuscall/)
states that bus delivery is delayed/asynchronous. Do not assert synchronous cross-language reentry as the
crash cause merely from JS handlers sending replies; the crash stack alone does not identify that mechanism.

Requested by the user for testing alongside the mouse-look changes. This is a full UI/module candidate,
not the previous isolated callback-registration diagnostic. Installed identity is recorded separately in
`build/review/airport-test-20261011/installed.json` after package verification. Live acceptance is pending.

Installed with MSFS closed at 01:29 local time. All 20 installed file hashes and 18 layout sizes verified through
the Community junction. WASM SHA-256: `af0f71b51adb8cb2a380a9be7abaa582415aba9799b3c6ebd6509d92723ccd41`.
Rollback: `build/rollback/before-airport-test-20261011-012924/fs-camera-studio`.
The package compiler exited 1 without output; composition reused the previous compiled SPB files after checking
that their XML sources were unchanged and their hashes matched the previous installed receipt. Fresh staged
HTML/CSS/JS and the SDK-built WASM module were copied into that verified package, with a regenerated layout.

## Test workflow

1. Start a flight, open Camera Studio, then **Profiles → Airport cameras**.
2. Choose a current/nearby scenery airport, or enter one or more ICAOs (maximum ten per batch).
3. Select feature types and height/offset, then **Preview cameras**. Preview generation does not move the camera.
4. Expand the feature groups, choose views, and **Save selected views**. The saved result appears only after
   storage confirms the commit.
5. Open **Airport · ICAO** in the camera library or mini panel. Select a feature category and a camera.
6. Refine a view, save, restart and recall it. Regenerate with replacement off to preserve refinements.
7. Generate a second nearby airport and select it from the distance list. Confirm it replaces the airport
   subtree while aircraft categories remain. Automatic selection has a 25 NM default radius and 2 NM
   switch hysteresis; a manual choice remains selected while inside the radius.

## Implemented scope

- Runway threshold, approach and overview views; gate/apron views; tower where its position is available.
- Taxiway junction views use a slight offset when actual sign positions are unavailable. Optional XML source
  import reads taxi signs; there is no direct BGL parser in this build.
- Stable feature IDs, selective save, preserve-refinements regeneration and explicit replacement.
- Separate persistent airport profiles and recovery transactions; only one nearby airport loaded at a time.
- Airport coordinates use a fixed world origin; switching between aircraft and world frames uses a cut.
- Existing right-drag smoothing and callback registration once per connection are retained. Settings exposes
  mouse sensitivity/smoothing/invert; Behavior exposes retain/reset on release. WASD movement, Q down/E up,
  and Shift faster movement are present as fallback controls when those keys have no existing assignment.

## Verification and remaining acceptance

- Native suite: **102 passed, 0 failed, 5045 checks**, including generation, geodetic conversion, deterministic
  IDs, regeneration, nearby selection, separate storage/reload, scope validation and world-camera capture.
- Browser: airport XML parsing, batch input, feature preview, selective save and compact context menu passed.
  Settings/library tests, panel tests and mini pointer pass-through tests passed. Screenshot reviewed at 1100×1000.
- SDK WASM compilation passed; SDK structure/aggregate warnings remain. Package compiler result and installed
  hashes are recorded in the installation receipt.
- Actual MSFS facility delivery, terrain clearance, world axes and saved-view recall still require a flight.
  Missing scenery features are skipped instead of inventing coordinates. Generated views start with View ID 0;
  manual bindings still share the aircraft View ID namespace, so choose unused IDs.
- Earlier full UI builds crashed during loading. Incoming CommBus messages now defer handling outside the native
  callback, but this does not establish the crash cause or prove it fixed. The preceding installed package is
  backed up before replacement. Cockpit clicks, top-toolbar hover and right-drag smoothness need live retesting.

## Research references

- Comparative interaction references for airport generation, feature offsets, preview/refinement and nearby sets
  are retained in the local research notes, outside the published product documentation.
- [MSFS facility definitions](https://docs.flightsimulator.com/msfs2024/html/6_Programming_APIs/SimConnect/API_Reference/Facilities/SimConnect_AddToFacilityDefinition.htm):
  runway, parking, taxiway geometry and tower data.
- [MSFS camera data](https://docs.flightsimulator.com/msfs2024/retail/programming-apis/wasm/camera-api/fscameradata/): world camera coordinates.
- [Taxiway XML](https://docs.flightsimulator.com/msfs2024/html/5_Content_Configuration/Environment/Airports_And_Facilities/Taxiway_XML_Properties.htm): actual taxi-sign input.
