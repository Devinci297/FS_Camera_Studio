# Mouse sample range correction

User confirmed the airport-era module with old UI loads, but right-drag remains jerky. This narrows the loading
regression to the newer panel set or its interaction with the module; airport controls remain absent in this UI.
Read-only flight evidence: `build/review/mouse-range-20261011/live-before.log`.

One drag recorded 103 accepted samples and 47 rejected samples, maximum absolute raw value 126574. Another
recorded 179 accepted and 53 rejected, maximum 100874. Both reported no panel blocking or competing bound-look
input. Code rejected any sample outside [-32768,32767], applying joystick limits to relative mouse counts.
This is a demonstrated source of discontinuity, not proof that all possible causes of jerkiness are resolved.

Removed that rejection. All signed 32-bit mouse counts follow the existing measured conversion and 55 ms
default easing. The existing 45-degree pending-angle cap bounds extreme input. Release/cancel clears pending
motion as before; sensitivity, input mapping, masking and UI JavaScript remain unchanged from the baseline.
Diagnostics now count `largeSamples` rather than claiming those samples were rejected.

Native suite: 103 passed, zero failed, 5185 checks. Includes positive/negative values across the former cutoff,
observed large values, both int32 extrema, 30/120 Hz easing, and no movement after release.
SDK WASM compilation passed with existing SDK structure and aggregate warnings.

User additionally requested removal of the highlighted mini category/camera dropdown. Source and staged
candidate hide that redundant picker with one CSS rule, preserving previous/next, category and camera buttons.
Browser verification used the actual staged mini.html and saved
`build/review/mouse-range-20261011/mini-without-picker.png`.

Candidate: `build/diagnostic/mouse-range/fs-camera-studio`; receipt:
`build/review/mouse-range-20261011/staged.json`. Exactly three files differ from the confirmed-loading build:
WASM, app.css (one appended rule), layout.json.

Installed after the user closed MSFS. All 20 package hashes and 18 layout entries verified; receipt:
`build/review/mouse-range-20261011/installed.json`. The prior package is backed up at
`build/rollback/before-mouse-range-20261011-20261011-052902/fs-camera-studio`.
Live smoothness acceptance is pending. The airport backend remains in this module, but airport generation
controls are absent because the installed panels retain the confirmed-loading UI.

User feedback after installation: "so far the mouse look is good". Record this as a provisional live pass
for right-drag smoothness in the mouse-range build, not prolonged-session or all-camera acceptance. Preserve
this installed module and the loading UI as the next comparison baseline while isolating the airport-panel
startup regression. The airport generation UI remains unavailable in this installed build.

The [SDK input documentation](https://docs.flightsimulator.com/html/Programming_Tools/SimConnect/API_Reference/InputEvents/SimConnect_MapInputEventToClientEvent.htm)
describes the +/-32K limits specifically for joystick axes. The mouse range correction is supported by this
flight's measured data; the scaling factor remains empirical rather than a claimed SDK mouse-unit contract.
