# Aircraft inspection walkaround

Use **Library + → External motion views** or **Walkaround → Study aircraft / add inspection**. Review the footprint, numbered stops, clearance and eye height, then add and save the views. Walkaround controls set dwell time, average walking speed, head-motion strength, look-around angle and looping.

The shared planner and camera engine are used by both simulator adapters. Inspection cameras aim at actual component locations. Travel follows the outline with rounded turns, eases into and out of stops, looks around during the dwell, and adds distance-driven footsteps and subtle head roll. Generated stops retain their footprint and aircraft variant in the profile. Arbitrary entry from a cockpit is a cut to the first exterior stop; subsequent stops use the perimeter. Manual control stops the sequence. Simulator pause freezes travel and scanning. Fresh telemetry must report stationary, on-ground and parking brake set; brake release or motion stops the inspection.

When vertical component bounds are available (automatic ACF studies, or an imported study with `obstacles`), gear stops use checked access branches beneath elevated parts. The camera returns along the branch before continuing around the outline. Low bodies, nacelles, wheels and rotor disks remain obstacles. If those bounds are unavailable, inspection stays outside the full configuration envelope.

## Geometry sources

X-Plane reads the loaded ACF on a file-only worker, without calling XPLM off the flight-loop thread or editing the aircraft. It extracts body meshes, already-positioned wing meshes, body transforms, engine locations, extended gear legs, propeller/rotor disks, nominal CG and parked height/pitch. Geometry is converted from Plane Maker feet to meters FRD relative to nominal CG. A result for an aircraft that is no longer loaded is discarded. Distinct ACF files in the same folder have distinct inspection identities.

MSFS uses readable `flight_model.cfg` and `engines.cfg`: fuselage dimensions/location, wing sweep and quarter-MAC location, stabilizer, wheel contacts, engine positions, datum, CG and parked attitude. It accepts legacy and named `Name: ... #Properties: ...` wheel tuples. Obsolete `wing_pos_apex_lon` is not used. The result is a conservative configuration envelope, not the visual mesh.

During local MSFS package staging, `tools/package.ps1` runs the scanner when Node is available. Public releases use `-SkipAircraftStudy` and contain no machine-specific catalog; MSFS users can import their aircraft configuration through the panel. To refresh a separately staged local package:

```powershell
node tools/study-aircraft.cjs --output <package>/html_ui/InGamePanels/FSCameraStudio/aircraft-footprints.js <installed-package-roots...>
```

Omit package roots to discover the configured MSFS 2024 packages directory. A `.report.json` alongside the catalog lists unavailable configurations. Do not distribute a machine-specific catalog as universal aircraft data. A package's layout manifest must be regenerated if updating its catalog after compilation.

**Advanced geometry import → Import aircraft source** also accepts pasted ACF, merged MSFS flight-model/engine configuration, or a study JSON. If automatic geometry is unavailable, **Import aircraft source** appears directly. Imported geometry is previewed before camera generation; generated cameras persist independently of the import. Use the configuration for the currently loaded aircraft and resolve inheritance before pasting.

The Walkaround control plays only studied inspection routes. When a profile has inspection views, its old fixed-view category named Walkaround is retired automatically; unrelated camera categories are retained. The standalone legacy tour no longer starts. A profile with only an old route retains those saved views until an inspection replaces it.

Walking effects are in the Walkaround controls, from 0 (off) to 1. Try 0.1 for light movement. The view looks ahead into bends, slows for rounded corners and turns smoothly toward the component on arrival. Nearby views sharing a gear approach avoid walking out and back unnecessarily.

## Limits

Automatic analysis cannot promise correct visual geometry for every add-on. ACF physics geometry may differ from attached OBJ meshes. MSFS protected/streamed packages, unresolved inheritance, modular attachments and incomplete geometry require an imported study; a generic airliner is never silently substituted. Scenery, moving control surfaces, changing wing sweep, ground slope/roll, and visual-model-only details are not collision tested. Re-study after geometry changes. Aircraft without a usable parking-brake signal cannot start the automatic inspection.

The import schema is simulator-independent: `version: 1`, `polygons` (convex component polygons of `[forward,right,down]` meter points), `landmarks` (`name`, `kind`, `point`, outward horizontal `direction`), `groundDown`, `groundSlope`, `lengthMeters`, and `spanMeters`. Ground height in the aircraft frame is `groundDown + forward * groundSlope`. The planner reserves 25 cm beyond chosen clearance for turns limited to 20 cm.

Optional `obstacles` contain a convex `polygon` and `maxDown` (the component's lowest point in FRD). Supply complete obstruction geometry before enabling approaches; omitted obstacles are not collision tested. Profiles store each stop's `walkaroundEntry` on the outer loop along with its inspection position.

## References

- [X-Plane ACF guidance](https://developer.x-plane.com/article/guidelines-for-working-with-text-based-acf-file-formats/)
- [X-Plane coordinate origin](https://developer.x-plane.com/article/movingtheplane/)
- [MSFS aircraft geometry](https://docs.flightsimulator.com/html/Content_Configuration/SimObjects/Aircraft_SimO/flight_model/geometry.htm)
- [MSFS engine positions](https://docs.flightsimulator.com/html/Content_Configuration/SimObjects/Aircraft_SimO/engines_cfg.htm)
- [MSFS WASM file access](https://docs.flightsimulator.com/msfs2024/flighting/programming-apis/wasm/io-api/fsioopen/)

## Validation

`node ui/tests/walkaround.test.cjs [optional .acf paths...]` checks geometry generation and MSFS datum/contact conventions. Native tests cover perimeter travel, finite smooth poses, parking telemetry, scan, pause and brake release. The B2707-300 ACF produces 23 stops including five gear locations and four engines. Hidden-browser inspection covers the numbered preview and camera creation. Live simulator visual acceptance remains necessary, especially for add-ons whose physics and visual models differ.
