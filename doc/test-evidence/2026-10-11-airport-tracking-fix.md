# Airport controls candidate — 2026-10-11

Installed local test build: `0.1.1-test.20261011.2`. No GitHub release was changed.

Changes:
- Full aircraft tracking uses the SDK target mode with a fixed WORLD position and the user aircraft as the rotation target. Heading-only tracking retains the latest aircraft position through telemetry gaps. Right-drag temporarily uses explicit rotation.
- Stationary WORLD cameras no longer resubmit their position on every orientation/FOV change.
- Subscribe to canonical ZOOM_PLUS and ZOOM_MINUS only. The documented legacy ZOOM_IN/ZOOM_OUT aliases have opposite meanings to their names; subscribing to both alias pairs could cancel/double wheel input.
- Tracking opens above the mini dock with visible selected state and descriptive choices. Transparent outside the dialog; mini hit bounds use the actual dialog, not the viewport.
- Mini dock height reduced from 164 to 120 px; category and camera lists scroll independently.
- Shipped direction mapping replaces subjective visual calibration and accumulated sign flips. The known lateral correction for simulator build 12.2.282174.999 remains selected automatically. Old panels cannot run probes or overwrite it. This selects a built-in mapping; it does not pretend to visually measure the simulator.
- Parking names now include scenery NAME, NUMBER and SUFFIX, directional parking names, docks and gate letters. Nonzero suffixes participate in generated IDs so distinct stands are not merged. XML import follows the same naming. NONE/VEHICLE positions are excluded from aircraft stands.

Verification:
- Native: 119 tests passed, 5,427 checks.
- SDK Release WASM compilation passed (existing missing-initializer/SDK offsetof warnings).
- Packaged flight-controls browser suite passed, including compact menu bounds and 100-camera scrolling at 1100 and 750 px.
- Packaged airport UI suite passed, including hidden calibration controls, XML gate/suffix names, preview and selective save.
- Inspected screenshots in build/review/airport-tracking-menu.png and airport-controls-mini-detail.png.
- MSFS process absent before installation. Existing Community junction and prior installed hashes verified. Backup created; all 22 installed file hashes match the staged receipt.
- Receipt: build/review/airport-tracking-fix/installed.json.

Live simulator behavior remains to be tested. Existing saved airport views are not silently renamed or replaced; regenerate a preview to get the new scenery names. Preserving existing cameras keeps their custom names/poses. Choosing replacement in the generator replaces generated views. SDK facility data may differ from painted terminal signage or third-party scenery metadata; this code cannot infer absent names.

SDK references:
- https://docs.flightsimulator.com/msfs2024/retail/programming-apis/wasm/camera-api/fscameradata/
- https://docs.flightsimulator.com/msfs2024/retail/programming-apis/key-events/view-and-camera-events/
- https://docs.flightsimulator.com/msfs2024/html/6_Programming_APIs/SimConnect/API_Reference/Facilities/SimConnect_AddToFacilityDefinition.htm
