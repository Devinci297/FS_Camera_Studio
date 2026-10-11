# Airport creation entry point — October 11, 2026

The camera-library **+** menu now offers **New camera**, **New category**, and **Create airport cameras**.
The bottom Add camera button still opens the existing single-camera form.

The generator was previously available under Profiles in source, but excluded from the loading-diagnostic UI.
This candidate exposes only the on-demand airport dialog, airport snapshot/event handling and its styles on top
of the verified movement-bindings package. It keeps the module, bridge, panel host, mouse controls and all other
package assets byte-identical. Airport discovery starts only when the user opens the generator.

The dialog supports nearby/current airport selection, ICAO entry, feature options, optional airport XML,
preview and selective save through the existing native airport backend.

Verification: browser tests passed against source and the exact staged main.html. They cover all three menu
actions, no discovery on opening the + menu, discovery when opening the generator, XML parsing, feature preview,
selective save and batch ICAOs. Browser airport data and backend responses are fixtures; actual MSFS scenery
delivery, generation, world-fixed camera operation and loading stability still require a live flight. Dialog
screenshot inspected at 1100x1000. No native changes in this iteration.

Candidate: `build/diagnostic/airport-menu/fs-camera-studio`.
Receipts: `build/review/airport-menu-20261011/staged.json` and `installed.json` after installation.
