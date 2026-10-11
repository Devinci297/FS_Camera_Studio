# Airport camera defaults and overrides

Settings > Tracking selects the shared tracking mode; Settings > Airport defaults configures airport camera behavior across aircraft profiles.

- Aircraft tracking: Off, Heading only, or Heading + pitch (default).
- Keep distant views near the horizon: on by default. In Heading + pitch mode, lower targets progressively level the camera between 500 and 1,500 m horizontal distance. Protection blends in between 20 and 40 m of height difference; above 1,500 m with at least 40 m difference the camera looks level while retaining the aircraft's heading. Nearby and upward tracking stay exact. Turn this off globally or in a camera override for strict line-of-sight pitch.
- Auto zoom: off by default. When enabled and tracking, it adjusts the submitted camera FOV from aircraft distance, with configurable framing size, minimum/maximum FOV, and smoothing time. Smaller framing sizes give closer views. It uses a fixed size in meters rather than detecting the aircraft model's dimensions.
- Category heights: runways, gates, apron/parking, taxi signs/intersections, and tower. Defaults are 3 m for the first four categories and 35 m for towers. Heights use scenery feature elevation; tower uses airport elevation. Runway approach and overview inherited heights have 15 m and 25 m minimums respectively.

Behavior > Airport camera settings (also available in the camera menu and tracking popup) overrides each setting independently. Missing override values inherit global defaults. “Use global settings for everything” clears these overrides. An explicit height override bypasses the runway minimums.

New generated cameras carry their elevation reference and inherit category heights. Older cameras retain their saved altitude until the user opts into a height setting; their reference then uses airport elevation. This avoids changing previously refined views. Manual vertical edits produce a height override. Ordinary saving preserves inheritance when there was no vertical adjustment.

Auto zoom affects airport cameras only. It does not alter saved/manual FOV. Turning it off, disabling aircraft tracking, or holding right-drag restores manual FOV; automatic zoom resumes after right-drag release. Simulation pause freezes the zoom response. Framing uses `2 * atan(frameMeters / (2 * distanceMeters))`, clamped to the selected FOV limits, with exponential smoothing.

MSFS can lift an airport camera above terrain or buildings through [`FS_CAMERA_FLAG_ABOVE_GROUND`](https://docs.flightsimulator.com/msfs2024/html/6_Programming_APIs/WASM/Camera_API/FsCameraFlag.htm). Tracking and zoom read the current world camera position after a submitted move has settled, so their height calculations use the reported altitude rather than the requested scenery elevation. Readback from another location, invalid data, and unavailable reads are ignored. This does not rewrite saved camera heights. Horizon protection is a framing preference, not a terrain visibility test; it cannot make an aircraft visible through a hill or building.

Head tracking stays a separate global setting, off by default. Exterior cameras share the global aircraft-tracking mode, but airport height and autozoom settings do not affect them. Cockpit cameras never track the aircraft.

Changes apply immediately; Save changes or Ctrl+S persists them. Native regression and browser checks cover these paths; in-simulator visual acceptance remains a separate check.
