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

## Smart views (glance)

MSFS does not list cockpit instruments or their positions, so smart views work from your own instrument close-ups. Turn on **Glance view** in a cockpit camera's Behavior settings (next to **Show in quick access**). Cockpit cameras whose icon is the instrument icon, or whose name gives that automatic icon (PFD, MFD, instrument, G1000, radio, avionics, autopilot), are glance views by default, so the C172 starter's four Instruments views work at once. Older profiles get the same default when they load; nothing is migrated. Exterior, orbit, fly-by and airport views are never glance views.

| Binding | Default (new installs) | What it does |
| --- | --- | --- |
| **Glance at instrument (hold)** (`glanceHold`) | Ctrl + Alt + G | While held, shows the glance view nearest to where you are looking. Release returns to the exact view and look direction you left. |
| **Next glance view** (`glanceNext`) | Ctrl + Alt + I | Steps through glance views in category order, wrapping. |
| **Return from glance** (`glanceReturn`) | Ctrl + Alt + U | Goes back to the view and look direction you glanced from. |

G, I and U were unused by every other default (1–9, P, N, B, R, X, E, M, C, the arrows, W/A/S/D/Q/E, Shift and Ctrl + S). Saved bindings are not migrated, so an existing setup has no glance inputs until they are assigned on the Bindings page, under **Glance - instrument close-ups**.

* **Nearest view.** The current look direction includes temporary mouse look and keyboard movement. Each glance view is compared by the direction from the current eyepoint to the point 0.75 m in front of that camera (roughly the instrument it shows), so a close-up placed at the eyepoint compares by its own direction. The smallest angle wins; within half a degree, the camera that itself points closest to your look wins, then category order. The view you glanced from is never offered.
* **Return.** Both directions use the smooth transition and take 0.35 s. A glance view opens at its saved pose; on return the saved pose of the original view and its temporary look are restored exactly. Saved poses are never changed.
* **Ending a glance.** Release of the hold, **Return from glance**, recalling any view (a view binding, Next/Previous camera, a panel button, peek, automatic airport switching) ends glance mode; only release and Return go back. **Release camera control** and **Panic** cancel it: nothing returns afterwards, even if the glance key comes up later.
* Glancing needs an active cockpit view. Text focus in a panel ends a held glance, like every held binding. Alternate inputs for **Glance at instrument (hold)** share one hold. The Bindings page's Test button glances and stays; test **Return from glance** to go back.
* The mini panel shows **Glancing - release to return** above the instrument's name while a glance view is shown.

Coverage: `tests/test_glance.cpp` (defaults, nearest-view selection, hold/release exactness, cycling, release and panic, the module path) and `ui/tests/glance-views.test.cjs`. Glancing in a live simulator still needs verification.

## Cinematic mode

**Cinematic** (next to **Auto switch** in both panels) and **Toggle Cinematic mode** (`toggleCinematic`) start an automatic playlist of views, like a broadcast director. New installs get **Ctrl + Alt + K**; K was unused by every other default (1–9, P, N, B, R, E, M, C, X, Z, G, I, U, the arrows, W/A/S/D/Q/E, Shift and Ctrl + S). Saved bindings are not migrated, so an existing setup has no Cinematic input until one is assigned on the Bindings page. Like Auto switch it is a session-wide mode, not a saved setting, and it works with both panels closed.

* **Dwell.** Each view is shown for a random time between **Shortest** and **Longest time on a view** (Settings → Cinematic mode, `host.cinematicMinSeconds` / `cinematicMaxSeconds`, defaults 8 and 20 s). The same view is never shown twice in a row, and the last four views are much less likely to come back.
* **Flight phase.** The phase comes from the aircraft's on-ground state, ground speed and vertical speed: parked (below 1.5 m/s), taxi, takeoff roll and climb-out (on the ground above 20 m/s, the first 45 s after liftoff, or climbing faster than 2 m/s), cruise, descent and approach (descending faster than 2 m/s), and landing roll (from touchdown until below 15 m/s). Each threshold has hysteresis so the phase does not flicker. On the ground and on approach airport views (tower and runway views count double) and exterior views are favoured; in cruise exterior, orbit and fly-by views are favoured and airport views are not shown. Airport views are only available while a saved airport is loaded nearby.
* **Cockpit views** appear occasionally: **Cockpit views** in Settings is their share of picks (`cinematicCockpitWeight`, default 15 %). At 0 they appear only when no other view takes part.
* **Which views.** **Include in Cinematic mode** in each camera's Behavior settings (`cinematic`). It is on by default for exterior, orbit, fly-by, airport and mounted views, and of the cockpit views only for the profile's default (pilot) view. Free cameras are never included.
* **Transitions.** With **Cut to airport and fly-by views** on (`cinematicCuts`, default) the playlist cuts to world-fixed views and glides to the others with a 2.5 s quintic (Cinematic) move. Off, it glides wherever the views allow; a change between cockpit, aircraft-relative and world-fixed views is always a cut.
* **Commercial drift** (`cinematicDrift`, default on) adds a slow, level dolly along the view's forward or right axis (0.12 m/s, at most 2.5 m over the dwell) and a small zoom (at most 3°, and at most 5 % of the view's field of view) on exterior and airport views. Cockpit views never drift. Like Binoculars it is presentation only: saved poses, captures, **Save adjusted view** and the temporary look never include it. A smooth move to the next view lets the previous drift fade out instead of jumping.
* **Pausing.** Manual look or move input (keys, controller, right-drag or panel mouse look, zoom) pauses the playlist for 10 seconds after the last input, then it continues. Glance and Binoculars hold it on the current view until they end; a simulator pause holds it too.
* **Stopping.** Release Camera, Panic, any manual camera recall (a view binding or button, Next/Previous camera, Return to default view, peek, walkaround), turning on Auto switch, and pressing the toggle again stop it; it does not come back on its own. Turning it on turns Auto switch off.

Each change is reported as a `CinematicChanged` event (`enabled`, `state`, `phase`, `cameraId`, `dwellSeconds`, `reason`); `PanicReleased` carries `cinematicStopped`. Coverage: `tests/test_cinematic.cpp` (phase detection, weighting with a seeded random source, dwell timing, no immediate repeats, pause and resume, stopping on release, panic and manual recall, drift bounds and presentation only) and `ui/tests/cinematic-mode.test.cjs`. How the playlist, transitions and drift look in a live simulator still needs verification.

## Spotting

The traffic list shows AI and multiplayer aircraft within **Spotting radius** (Settings, `host.spottingRadiusNm`, default 20 NM, 1–100). VATSIM and IVAO traffic arrives in the simulator as AI aircraft, so it is listed too. The module polls SimConnect about every 1.5 s while a panel is visible or spotting is on. It never lists the user's own aircraft, and it drops an aircraft 6 s after it was last reported. Between polls each aircraft is dead-reckoned along its turn. Each new report is blended in over 1.2 s, so a camera aimed at the aircraft does not jump when the report arrives. A correction over 300 m (slew, warp) is applied at once.

Each aircraft gets a status:

* **parked**: on the ground below 1 kt.
* **taxi**: on the ground below 40 kt.
* **takeoff** or **landing**: rolling at 40 kt or more. It counts as landing when the aircraft was airborne in the last 60 s.
* **departing** or **arriving**: airborne within 15 NM of the loaded or nearest airport, climbing above 300 fpm or descending faster than 300 fpm.
* **cruise**: anything else.

* **Views.**
  * **Tower** aims the loaded airport set's tower view at the target.
  * **Runway** aims the runway or spotting view nearest the target. As the target rolls it switches to a nearer view when one is clearly nearer, at most every 6 s.
  * Tower and Runway need a loaded airport set with such views. Both zoom to frame the target with the Airport auto zoom frame size and limits (`host.airportAutoZoom*`).
  * **Chase** follows 60 m behind and 15 m above the target, along its track, and eases round turns.
  * **Fly-by** places a still camera 400 m ahead of the target and 30 m to the right of its track, at the height the target will have when it passes. Once the target is 300 m past, the camera moves ahead again.
  * **Track** keeps the camera where it is and only aims it: an airport or free camera, or wherever a chase or fly-by left it.
  * Chase and fly-by stay at least 2 m above the terrain under the target, and above the terrain under the user's aircraft when the camera is near it.
  * Binoculars work while spotting. Glance does not.
* **Bindings.** New installs get these defaults. T, Y, O and J were not used by any other default. Saved bindings are not migrated.
  * **Ctrl + Alt + T** (`spotNext`) and **Ctrl + Alt + Y** (`spotPrevious`) spot the next or previous aircraft by distance and keep the current view. They use chase when spotting is off or the current view is not available for the new target.
  * **Ctrl + Alt + O** (`spotReturn`) stops spotting.
  * **Ctrl + Alt + J** (`toggleAutoSpot`) turns auto-spot on or off.
* **Mini panel Traffic tab.** A fixed bar above the aircraft list holds the view choice (Tower, Runway, Chase, Fly-by, Track), status filters with counts (**All**, **Departures** = takeoff or departing, **Arrivals** = arriving or landing, **On ground**, **Cruise**), a search box and Stop. The bar does not scroll with the list. Search matches airline, flight number, callsign or aircraft type, ignoring spaces and dashes (`a32` finds A320 and A321, `hbjca` finds HB-JCA). **Enter** follows the first match in the chosen view; **Esc** clears the search. The editor's Traffic page has the same Cruise filter and type search.
* **Lost target.** When the target is no longer reported, the view holds for 5 s (`state: lost`). Spotting then returns to the view shown before it began, or gives the camera back to the simulator when there was none. With auto-spot on, the next target is picked instead.
* **Auto-spot.** A session-wide mode, not saved.
  * It picks traffic within 15 NM of the airport in this order: landing, takeoff, arriving, departing. Among aircraft with the same status, the one nearest the airport goes first.
  * Arriving, landing and takeoff traffic gets the runway view, taxiing traffic the tower view, and departing traffic the chase. When a view is missing it uses the other airport view, then chase.
  * It stays on a target for **Auto-spot hold** (`host.autoSpotHoldSeconds`, default 30 s, 10–300). It changes the view when the status changes, at most every 5 s.
  * When nothing qualifies it waits (`state: waiting`).
* **Stopping.**
  * Spotting stops on: Stop spotting, the Return binding, any manual camera recall, Release, Panic, a lost camera claim and a new flight.
  * Auto-spot turns off on: a manual target choice, Stop spotting, Release, Panic and a new flight.
  * Spotting stops Cinematic mode and Auto switch, and turning either of them on stops spotting.

Changes are reported as `TrafficChanged` (at most once a second) and `SpottingChanged`; `PanicReleased` carries `spottingStopped`.

Coverage: `tests/test_spotting.cpp` covers classification, smooth prediction in a turn at 60 fps, stale removal and exclusion, view geometry and the terrain floor, the lost hold, the commands, bindings, auto-spot and mutual exclusion. `ui/tests/spotting.test.cjs` covers the panel side.

Not yet verified in a live simulator: the traffic data layout, the ATC strings and how the views look.
