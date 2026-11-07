# Traffic viewpoints - local candidate, 2026-11-07

## Behavior

- Auto nearest chooses the closest loaded airport camera, saved spotting place, or scenery tower/runway fence to the selected aircraft. It checks once per second, holds a camera for six seconds, and requires a 15 percent or 10 meter improvement before changing to another existing candidate. Candidates must be 20 meters to 20 kilometers away. No candidate falls back to Chase; losing a candidate permits immediate reselection.
- External opens Front, Rear, Left side, Right side, Overhead and Orbit in both the main editor and mini toolbar. Heading-relative angles follow the target; Orbit circles at eight degrees per second.
- Movement, mouse look and zoom remain available. Manual adjustments stop automatic nearest-place changes and orbit rotation until Recenter. Simulator pause holds automatic switching/rotation. Track can hold any shown world traffic camera.
- The status includes the chosen viewpoint and, for Auto nearest, the selected camera name. The combined build also includes restored Primary/Secondary X-Plane assignments and saved-aircraft controls-profile detection.

## Validation

- Native Release: **329 shared tests / 76,321 checks**, plus **11 keyboard tests / 53 checks**. All pass (340 tests, 76,374 checks).
- Added native coverage for heading-relative placement, aim, terrain bounds, Orbit advance/pause/manual hold/recenter, nearest selection/dwell/margin, manual hold, fallback/recovery, scenery and profile candidates, and actual panel protocol/state replies. Existing manual traffic control tests now cover every static external angle and nearest fallback.
- Six headless suites pass: traffic-viewpoints, spotting, mini-traffic, spots, traffic-camera-controls, and xplane-camera-bindings (main and mini against the real C++ service/helper in disposable preferences).
- New UI coverage checks all six angles, target retention, nearest status, Track and menus at 360/560/900/1440 pixels. Main and mini renders were inspected, including the 560-pixel mini menu.
- MSFS WASM compiles successfully. This verifies the shared build, not live simulator behavior.

## Installed state

The 15-file payload was installed into the Steam X-Plane 12 XPCameraStudio plugin while X-Plane was closed. Every installed file matches the verified candidate. All 102 files under Output/preferences were hash-checked unchanged. The previous plugin was backed up before replacement.

- Candidate: `build/traffic-views/XPCS-traffic-viewpoints-local.zip`
- Build and source hashes: `build/traffic-views/verified-build.json`
- Native result: `build/traffic-views/native-tests.log`
- Install hashes and backup location: `build/traffic-views/installation.json`
- UI renders: `build/traffic-views/ui/`

This is a local candidate based on 0.5.3; no new release was published. X-Plane has not been launched by the agent. Loaded identity and live acceptance remain pending the user's next simulator session. MSFS was built but not installed. No computer-use tools were used.
