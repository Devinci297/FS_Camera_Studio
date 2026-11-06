# Airport camera defaults and overrides

Settings > Tracking selects the shared tracking mode; Settings > Airport defaults configures airport camera behavior across aircraft profiles.

- Aircraft tracking: Off, Heading only, or Heading + pitch (default).
- Keep distant views near the horizon: on by default. In Heading + pitch mode, lower targets progressively level the camera between 500 and 1,500 m horizontal distance. Protection blends in between 20 and 40 m of height difference; above 1,500 m with at least 40 m difference the camera looks level while retaining the aircraft's heading. Nearby and upward tracking stay exact. Turn this off globally or in a camera override for strict line-of-sight pitch.
- Auto zoom: off by default. When enabled and tracking, it adjusts the submitted camera FOV from aircraft distance, with configurable framing size, minimum/maximum FOV, and smoothing time. Smaller framing sizes give closer views. It uses a fixed size in meters rather than detecting the aircraft model's dimensions.
- Category heights: runways, gates, apron/parking, taxi signs/intersections, and tower. Defaults are 3 m for the first four categories and 35 m for towers. Heights use scenery feature elevation; tower uses airport elevation. Runway approach and overview inherited heights have 15 m and 25 m minimums respectively.

Behavior > Airport camera settings (also available in the camera menu and tracking popup) overrides each setting independently. Missing override values inherit global defaults. “Use global settings for everything” clears these overrides. An explicit height override bypasses the runway minimums.

New generated cameras carry their elevation reference and inherit category heights. Older cameras retain their saved altitude until the user opts into a height setting; their reference then uses airport elevation. This avoids changing previously refined views. Manual vertical edits produce a height override. Ordinary saving preserves inheritance when there was no vertical adjustment.

Auto zoom affects airport cameras only. It does not alter saved/manual FOV. Turning it off, disabling aircraft tracking, or holding right-drag restores manual FOV; automatic zoom resumes after right-drag release. Simulation pause freezes the zoom response. Framing uses `2 * atan(frameMeters / (2 * distanceMeters))`, clamped to the selected FOV limits, with exponential smoothing. Holding [Binoculars](dual-bindings.md#binoculars) overrides auto zoom; on release the view eases back to the current auto-zoom framing.

MSFS can lift an airport camera above terrain or buildings through [`FS_CAMERA_FLAG_ABOVE_GROUND`](https://docs.flightsimulator.com/msfs2024/html/6_Programming_APIs/WASM/Camera_API/FsCameraFlag.htm). Tracking and zoom read the current world camera position after a submitted move has settled, so their height calculations use the reported altitude rather than the requested scenery elevation. Readback from another location, invalid data, and unavailable reads are ignored. This does not rewrite saved camera heights. Horizon protection is a framing preference, not a terrain visibility test; it cannot make an aircraft visible through a hill or building.

## Temporary airport cameras

Settings > Airport defaults > **Temporary airport cameras** (on by default). Near an airport that has no saved camera set, Camera Studio creates its views in memory from the installed scenery and shows them under **Airport** like a saved set, marked **Temporary — Save to keep** in the library. They are not written to disk and are not part of Save all changes. Choose **Save** on that notice to keep them: they become a normal saved airport set with the same camera IDs, so the active view and view bindings carry over. Any saved set always wins: a saved set for the same airport replaces the temporary one, and while a saved airport is within the nearby radius no temporary set is created. Turning the setting off unloads the temporary set.

Automatic sets are kept small: every runway end (threshold, approach and overview views), two spotting views per runway end, at most four gates or stands spread across the apron (gates first), and the tower. They do not include taxi intersections. Scenery data for each airport is read once per session.

- **Tower:** the scenery tower position at the tower category height above airport elevation. When scenery has no tower (missing or zero coordinates) the view is a **Virtual tower**, 300 m abeam the midpoint of the longest runway on the side with more parking stands (the left of the runway heading when even or unknown), looking at that midpoint. It is labelled as virtual and is not the real control tower.
- **Spotting** subcategory, for each runway end: a **fence line** view 150 m behind the threshold and 80 m to the right of the landing direction, at the runway category height, looking down the runway at its midpoint; and an **approach, 1 NM** view on the extended centerline 1,852 m before the threshold, about 90 m below the 3° glidepath (about 7 m above threshold elevation, used as a minimum height), looking back at the threshold.

The manual generator (Create airport cameras) offers the same Spotting choice. Gates and taxi signs are only created where scenery data provides them. MSFS keeps cameras above terrain, so an approach view over rising ground may be lifted.

Head tracking stays a separate global setting, off by default. Exterior cameras share the global aircraft-tracking mode, but airport height and autozoom settings do not affect them. Cockpit cameras never track the aircraft.

Changes apply immediately; Save changes or Ctrl+S persists them. Native regression and browser checks cover these paths; in-simulator visual acceptance remains a separate check.
