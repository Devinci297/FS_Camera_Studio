# Aircraft profiles and storage

MSFS gives the WASM module a persistent virtual `\work` directory. On this Steam MSFS 2024 installation it is:

```text
C:\Users\Devinci\AppData\Roaming\Microsoft Flight Simulator 2024\WASM\MSFS2024\fs-camera-studio\work
```

Every successful store save writes one readable JSON file per aircraft package:
`fscs-profile-{aircraft-package-slug}-{package-key-crc32}.json`. Liveries sharing a package share this file.
The `profile` field holds the preferred profile, and `alternateProfiles` holds other profiles for that aircraft.
Files can be imported using the Profiles page. Editing one on disk does not automatically replace the running
profile: import it to apply changes. Aircraft files first appear when the updated module saves a transaction.

The checksummed `fscs-store-a.json` and `fscs-store-b.json` remain the recovery source used at startup. They hold
profiles, five backups per profile, settings, bindings and aircraft selection together. Readable aircraft files
are written before a store transaction and may reflect a save that failed later; startup trusts the verified
store slots. Profiles without an aircraft match get individual `unassigned` files until assigned to an aircraft.

## Cessna 172 default

`profiles/Cessna_172_G1000.json` is an importable copy of the built-in template. It contains 12 cockpit views
(pilot, landing, wide cockpit, copilot, PFD, MFD, full panel, standby instruments, throttle/trim, electrical,
overhead and rear seats) and 8 exterior views (front quarters, wings, chase, head-on, overhead and rear quarter).

The module creates and selects this template once per C172 aircraft package, replacing the old default
selection. Existing profiles remain available as alternatives. Later edits and profile choices are preserved;
the template is not reapplied on every startup. Automatic selection leaves camera ownership in Observing.

These are starting poses, not measured in-sim presets. Complete the axis check, test each view, and tune positions
for the aircraft variant. Exterior views remain marked Preview, with the camera API's cockpit-sound limitation.

To regenerate the importable copy after changing the template, build the native tests and run:

```powershell
build\tests\bin\Debug\fscs_export_c172.exe profiles\Cessna_172_G1000.json
```
