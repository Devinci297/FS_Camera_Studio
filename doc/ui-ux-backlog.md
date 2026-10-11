# UI/UX follow-up backlog

Saved at the user's request on 2026-10-10. These are planned follow-ups, separate from the current MSFS test build.

## Current status

Version 0.3.0 supersedes the early October 11 diagnostic installations. The release includes airport generation
and shared airport storage, global defaults and individual overrides, bindings and controller listening,
compact category/camera hierarchy, live-preview controls, axis reset, and starred mini-panel cameras.
See [0.3.0 release notes](releases/0.3.0.md) and [current qualification](qualification.md).

The list below records the original design requests. It is a requirements reference, not an assertion that
each item remains unimplemented. Scenery accuracy, live controller behavior and simulator hosting still need
acceptance checks; historical installation evidence remains in `test-evidence/`.

## Editing feedback

- Show a prominent **Live preview** indicator when position or orientation edits are moving the active simulator camera.
- Add a reset control to each axis card. Restore that axis to its saved value without changing the other axes, lens, or bindings.
- Strengthen the active-camera highlight in the mini panel, including icon-only mode. Distinguish the camera shown in the simulator from the camera selected for editing; keep names available in tooltips.

## Airport camera creation tools

Provide an **automated airport camera generator** informed by the requested interaction research and the MSFS camera API. Manual placement refines the generated cameras.

- Create and manage cameras attached to an airport rather than an aircraft: tower, runway ends, gates, ramp, and custom observation positions.
- Offer **Current airport** and **Enter ICAO codes** as the two starting paths. Accept one or more ICAO codes, validate them, and resolve the corresponding airports. For current-airport detection, show the resolved airport and handle nearby-airport ambiguity explicitly.
- Automatically generate named, positioned, and oriented cameras from available airport geometry: runway threshold and approach views, runway overview, taxi signs, tower, gates, and apron views where those features are available. Aim views at their relevant runway or airport feature and account for elevation and sensible offsets. Do not invent missing feature locations.
- Preview the generated list and create/save the selected views together. Allow fine-tuning, duplication, current-view capture where supported, and removal after generation.
- Re-running generation for an airport must offer a clear update/add choice and preserve manual refinements unless the user chooses replacement. Avoid duplicate categories and camera IDs.
- Save an airport's cameras in its own persistent profile file, identified by airport code and a stable location reference. Keep aircraft profiles separate.
- Use one top-level **Airport** category in the editor and mini panel, identified by the loaded airport's ICAO/name. Under it, use expandable subcategories such as **Runways**, **Taxi signs**, **Gates**, **Apron**, **Tower**, and **Custom**, each containing its individual cameras. Show only subcategories with cameras. Preserve existing aircraft categories and their ordering; airport subcategories must not become unrelated top-level categories.
- **Load only one airport camera set at a time:** the current airport when it has cameras, otherwise the nearest airport with saved cameras. Let the user choose a different airport from a distance-sorted list of nearby airports that have saved camera sets. Do not load every generated airport or automatically load a distant destination's cameras.
- The selected airport supplies the subcategories and cameras under **Airport**; switching airports updates its ICAO/name and replaces that subtree without deleting saved files or changing aircraft categories. An explicit nearby-airport choice remains selected while relevant rather than being immediately overridden by nearest-airport detection.
- Define and expose the nearby-airport radius and stable automatic-switching rules during implementation. Outside that radius, unload the airport categories while keeping the saved profiles available. Generation by entered ICAO remains available independently of which airport camera set is loaded.
- Support icons/labels/both, search, bindings, camera editing, and the same active-view feedback as aircraft cameras.
- Qualify world-fixed camera control, coordinate conversion, scenery elevation, persistence, and airport changes in MSFS. Do not label airport views ready while their backend mode is unavailable.

Acceptance: generate cameras for the current airport and for two entered ICAO codes; confirm that each airport gets its own persistent profile. Verify the hierarchy **Airport (ICAO/name) > feature subcategory > camera**, including Runways, Taxi signs, and Gates when feature data is available. Load only the current/nearest eligible airport's subtree, select another airport from the nearby saved-camera list, and verify that the subtree is replaced while aircraft categories remain. Test leaving the nearby radius and automatic-switch hysteresis. Save and reload, recall and edit generated views, regenerate without duplicating categories or overwriting manual refinements, and change aircraft without losing airport views. Invalid ICAOs or missing scenery data must produce a useful per-airport result without blocking valid airports in the same batch.
