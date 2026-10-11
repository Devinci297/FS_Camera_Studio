# Tester setup validation — October 11, 2026

Release: **0.1.0-test.20261011.1**. Shipping artifacts are in `dist/0.1.0-test.20261011.1/`.
Compiler: official Inno Setup 6.7.3, valid Pyrsys B.V. signature. Compiler download SHA-256:
`9c73c3bae7ed48d44112a0f48e66742c00090bdb5bef71d9d3c056c66e97b732`.
The produced tester setup is unsigned.

The payload contains exactly the 20 package files in `build/review/airport-controls-20261011/installed.json`.
All package hashes match. The 18 layout entries match their files and sizes. No fresh SDK build was performed;
the known-loading panel host and the airport-controls module from that receipt were preserved.

The actual shipping installer completed a fresh install in a workspace Community fixture. An initial upgrade
test caught an invalid date-separator argument in backup naming; this was fixed before distribution. MSFS started
during the checks, so subsequent lifecycle testing used the same Inno script with only its compiled process-name
guard changed to `FSCS-Installer-Test-Sim.exe`. This internal harness is emitted under build/, never dist/.
The harness's fresh-install, backup/repair and uninstall all succeeded. The uninstall helper deletes itself shortly
after its first process exits; tests wait for that completion before verifying cleanup.

Nine passing harness scenarios:

1. Fresh installation: all 20 installed hashes match the receipt.
2. Upgrade/repair: modified shipped file repaired; complete previous package backup hash-verified.
3. Uninstall: installer-owned files removed; additional package file, neighboring add-on and profile fixture preserved.
4. Existing unrecognized folder rejected without changes.
5. Wrong destination folder rejected.
6. Live configuration autodetection finds and refuses the developer's package junction.
7. Explicit package junction refused without touching its target.
8. Duplicate copy in sibling Community2024 refused.
9. Named test simulator process blocks installation before package creation.

Harness evidence: `build/installer-tests/cde89354-8830-4b6a-a6d5-8292f2b88bbc/results.json` and adjacent logs.
Afterward its registry/uninstaller entries were removed; remaining test backups were moved into that evidence folder.
The real Community package inventory stayed identical throughout.

The **final shipping EXE** was separately run against an empty fixture while the actual MSFS process was running.
It correctly exited with code 7, created no package/uninstaller, and left all real package hashes unchanged.
Evidence: `build/installer-tests/shipping-process-guard/results.json` and `shipping-blocked.log`.

The ZIP was read back, its CRC checked, and its exact files/checksums validated. BUILD-INFO records
`testHarness: false` and `liveSimulatorVerified: false`. Distribution includes an EXE, tester guide, notices,
BUILD-INFO and checksums; the ZIP has a separate SHA-256 sidecar. No tester data, profiles or developer config files
are included. Native/app flight acceptance remains as documented in the airport-controls test evidence.
