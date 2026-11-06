# Primary and secondary bindings — 0.3.0

Each camera action and command has independent Primary and Secondary inputs. Either slot accepts a keyboard shortcut, controller button, or supported manual input. A common setup is a keyboard shortcut in Primary and a controller button in Secondary.

Use the two slot buttons in the assignment dialog or the two input cards on the Bindings page. Clearing or replacing one slot preserves the other. When an input conflicts with another action, Replace moves only that input; the other action's remaining binding is kept. Save all changes persists the assignments.

For controllers, choose the device and click **Listen for button or hat**, then press and release a button or move the hat in a direction and return it to center. Click **Assign** to confirm. Listening covers buttons 1–32 and the first POV hat. Diagonals choose one clockwise direction (for example, up-right becomes right). **Manual button or hat switch** remains available for additional supported inputs after testing. Keyboard, mouse, and touch-pointer devices are excluded from the controller list. Listening stops on cancellation, controller disconnect, flight changes, or a 20-second timeout. Capture generations prevent delayed events from a previous listening request from filling a new assignment.

Existing bindings without a slot remain usable. The first two legacy inputs for an action become Primary and Secondary when edited. Additional legacy bindings are retained. Camera shortcut badges show actual assigned inputs rather than view IDs; view IDs remain available in Behavior.

Browser regression coverage: `ui/tests/dual-bindings.test.cjs`, `binding-dialog-layout.test.cjs`, `movement-bindings.test.cjs`, and `key-capture.test.mjs`. Hardware input and simulator control masking still need live verification. In particular, the latest T.16000M hat-release fix has automated coverage but has not yet been confirmed on the physical controller. After assigning a hat, start another listening session and test a normal button to check that capture has fully reset.

Hat capture, manual testing, and saved hat bindings explicitly subscribe to both movement and release. The release event is normalized to center, so returning the hat clears capture and held camera movement even when the simulator supplies a zero release payload. This follows the [SimConnect input-event contract](https://docs.flightsimulator.com/html/Programming_Tools/SimConnect/API_Reference/InputEvents/SimConnect_MapInputEventToClientEvent_EX1.htm). Native registration tests cover the missing-release regression. Rejected listener registrations now report an error and add bounded details to Diagnostics.

## Panic release

**Panic: release camera and restore simulator** (`panicRelease`) is a stronger form of **Release camera control** for when a view misbehaves. It has Primary and Secondary slots like every other command. New installs get **Ctrl + Alt + X**, following the Ctrl + Alt default convention; no other default uses X. Saved bindings are not migrated, so an existing setup has no panic input until one is assigned on the Bindings page.

Panic works with both panels closed (once a flight is loaded), while a panel text field has focus, and in every ownership state. Like every binding, it is ignored while the assignment dialog is listening for a controller button. One press:

* returns the camera exactly as Release Camera does — Releasing, then Observing once the simulator confirms;
* restores MSFS head tracking to the value it had before Camera Studio changed it (a change made in the simulator since then is respected);
* stops walkaround, orbit and fly-by motion and turns airport **Auto switch** off;
* clears the temporary mouse look, including looks remembered for cameras that retain them, and ends any mouse drag;
* discards a camera recall queued behind an earlier release, so nothing re-activates. Use **Enable** or select a view to take control again.

Release Camera already does all of this except two things: it leaves Auto switch on (paused until a camera is active again), and it keeps remembered looks so a retained look returns on the next recall. A panic while already released changes nothing and still succeeds. Each press sends a `PanicReleased` event (`state`, `reason`, `airportAutoStopped`). Both panels show "Camera released - simulator has control" for about 3 seconds after any release: the Release buttons, the Release or Panic binding, and automatic hand-backs (a change from Active, Paused by the simulator or Releasing to Observing). Coverage: `tests/test_coordinator.cpp`, `tests/test_module.cpp` and `ui/tests/panic-release.test.cjs`. Panic on a live simulator still needs verification.

## Binoculars

**Binoculars (hold)** (`binoculars`, under Manual look & move) zooms the active camera while its input is held and returns to the camera's own field of view when released. New installs get **Ctrl + Alt + Z**, following the Ctrl + Alt default convention; no other default uses Z. Saved bindings are not migrated, so an existing setup has no binoculars input until one is assigned on the Bindings page. It is hold-only; there is no toggle variant.

* The zoom eases in and out over about 0.3 seconds, timed by the clock rather than by frame count. Releasing part-way through reverses from the current zoom.
* The zoom level is **Settings → Movement → Binoculars zoom** (`host.binocularsFovDeg`, default 10°, 2–60°). It can go below the editor's 30° limit, because that limit is an editor choice and not a camera API limit (airport auto zoom already submits down to 2°). The binoculars never widen a view whose own field of view is already narrower.
* While zoomed, right-drag mouse look, panel mouse look and keyboard or controller look slow down in proportion to the zoom (zoomed field of view divided by the camera's field of view), so aiming stays controllable.
* It works with every camera type that can be activated. On airport cameras it overrides auto zoom while held. On release it eases back to whatever auto zoom is currently framing.
* The zoom affects only what is shown. Saved poses, captures, **Save adjusted view** and the temporary look never include it. Release Camera, Panic, a camera handed back to the simulator, and a panel text field taking focus all cancel it immediately.
* The mini panel shows a small "Binoculars 10°" badge while the zoom is held. The badge only displays state and never sends anything to the module.
* The Bindings page **Test** button cannot hold an input, so it explains how to try the binding in the simulator instead.

Coverage: `tests/test_coordinator.cpp`, `tests/test_profiles.cpp`, `tests/test_module.cpp` and `ui/tests/binoculars.test.cjs`. Very narrow FOVs, and how the zoom feels on a live simulator, still need verification.
