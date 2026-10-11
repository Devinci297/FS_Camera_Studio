# Third in-sim test corrections

The previous installed build ran in MSFS but remained in startup. Its diagnostic log recorded connected aircraft
events and repeated rejected profile/keyboard commands with "Camera Studio is still starting" for several
minutes. The local repository's SDK asynchronous reads did not finish. The WASM storage adapter now uses local
libc file IO, closes before completing callbacks, and logs repository readiness. This change is restricted to
persistent `\work` files, not streamed package files. Asobo documents this supported distinction:
<https://devsupport.flightsimulator.com/t/safe-to-use-old-file-functions-in-wasm-work-folder/13266>.

The panels now import the simulator's storage and header templates, bind the native drag handler to blank
editor content, and preserve interactive controls. The mini uses an invisible full-width bottom hover strip,
stays open over the strip or dock, and reports its mouse rectangle to the native host. Native header and resize
grips are hidden for the mini. `panelInvisible` remains controlled by the simulator's visibility lifecycle.

A C172 package gets a new default once: 12 cockpit and 8 exterior views, automatically selected while remaining
in Observing. Existing profiles remain alternatives. Each aircraft gets an importable JSON file on store saves,
with the existing two-slot store retained for recovery. Both toolbar icons were replaced and reviewed at 24 px
and 48 px in `build/review/toolbar-icons.png`.

## Offline results

- Native: 77 tests, 4,478 checks passed, including actual local file startup/reload, C172 default selection,
  template validation and aircraft file grouping.
- UI: 17 math, 6 bridge and 2 panel host regressions passed.
- SDK 1.7.3: WASM and both panel definitions compile; package error report is empty.
- Installed module SHA-256: `61F49934E3F8354236C3224277F81E10D3B081E58EDB3D38AFA422F408EFFA80`.
- Community junction targets `build/package/Packages/fs-camera-studio`; previous package is backed up under
  `build/rollback/before-third-sim-fixes`.

## Live acceptance pending

No new flight has tested these changes. Confirm storage readiness and automatic C172 selection first. Then
check background dragging, hidden mini appearance, full-width hover, stay-open behaviour, cockpit clicks
above/beside the dock, close/reopen and saved simulator panel placement. Complete the axis check and tune all
20 approximate template poses. Readable aircraft files first materialize when the new module saves a
transaction. Existing recovery files were not manually overwritten.

The attempted local browser preview timed out, so it does not count as visual hosting evidence. Panel host
regressions use a native-host mock; SDK compilation and disk hashes do not establish live MSFS acceptance.
