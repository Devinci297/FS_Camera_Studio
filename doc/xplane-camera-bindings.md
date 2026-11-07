# Native X-Plane camera keys and larger profiles (local development build)

The camera assignment dialog now captures keys and writes real X-Plane keyboard assignments through a bundled
Windows helper. The dialog detects the saved controls profile associated with the loaded aircraft when available.
Otherwise, choose it using the in-panel profile buttons. Select **Primary** or **Secondary**, press a shortcut,
review conflicting commands and choose **Assign**. X-Plane keeps its current in-memory assignments until exit;
the helper waits on the actual simulator process handle before merging the changes into its final saved preferences.
On the next launch, the selected controls profile contains the `xpcs/view_N` binding and no old owners of that chord.

**Primary** and **Secondary** work independently. Capturing a new shortcut replaces only the selected slot;
**Clear assignment** clears that slot and preserves the other. Slot identities survive reopening and restart,
including an empty Primary with an assigned Secondary. Other existing native shortcuts remain available with
**Change** and **Remove**. Pending changes for
the selected controls profile can be cancelled. New camera/view-ID edits still need the normal camera Save action.
Existing view command IDs remain 1-30; this is separate from the total camera count.

The helper backs up each affected preferences file before an atomic replacement. It checks reviewed chord owners
again after exit, merges unrelated final-save changes, and refuses stale conflicts, unknown formats, linked paths,
or writes while another instance of the simulator executable is running. An interrupted write has recovery metadata.
Failed work remains pending with an error result. The next session can cancel and review it again.
The keyboard queue, backups, and result are in `Output/preferences/XPCameraStudio/`.

The total profile capacity is now **2,000 cameras**, including imported and personal cameras. Airport generation
retains a separate 500-camera budget so it does not fill the new capacity. Existing profiles need no migration.
Panel messages have a bounded 8 MiB allowance and up to 8,192 chunks. Native frame sizing includes JSON escaping
and the envelope; individual frames remain within 4,000 bytes. Category, profile-byte, and store-byte guards remain.

## Validation

- 323 shared native tests, 76,109 checks: all passed, including 2,000-camera save/reload, backups, import, rejection
  beyond the cap, full-profile transport, and a 5 MiB message spanning more than 1,024 frames.
- 8 native keyboard tests, 42 checks: all passed.
- Real-helper lifecycle checks passed for missing helper, stale preview, native conflict changed on exit,
  cancel/retry, another process running, read-only write recovery, and Unicode paths.
- Six headless UI suites passed: native camera bindings, X-Plane bindings, MSFS dual bindings, traffic camera
  controls, free cameras, and capacity. The camera-binding UI calls the actual C++ service and helper against
  temporary native preferences; both main and mini flows verify final native assignments and backup bytes.
- UI capacity checks create camera 501 and camera 2,000, save/reload, and reject an additional drone with an explicit
  limit message. Dialog screenshots were reviewed at normal sizes; the footer remains visible at 360 px height.
- Windows X-Plane Release and MSFS WASM builds succeeded. Existing SDK warnings remain.

## Artifacts and state

`build/traffic-controls/xplane/XPCameraStudio` contains the plugin, UI, and `win_x64/XPCSKeyHelper.exe`.
`build/traffic-controls/XPCS-native-keys-2000-cameras-test.zip` is the verified complete X-Plane candidate.
`build/traffic-controls/fs-camera-studio` is the staged MSFS candidate; it uses the reviewed 0.5.0 toolbar host.
`build/traffic-controls/verified-build.json` records all payload hashes and validation counts.

The development X-Plane build was installed locally with a verified rollback backup. Automated tests are
separate from live simulator acceptance; these changes still need the user's live check.
See `build/traffic-controls/installation.json` for the installed state and rollback backup, when present.

## Release 0.5.1

These changes are included in 0.5.1. The development-state notes above describe the earlier local build.
See [0.5.1 release notes](releases/0.5.1.md) and [release verification](test-evidence/2026-11-07-v0.5.1.md).

## Assignment regression fix after 0.5.3

The earlier parser incorrectly rejected X-Plane key lines with optional human-readable command descriptions.
The fix reads the first three tokens and preserves complete untouched lines, including descriptions and
unknown key tokens. A copied set of all 28 user keyboard profiles now parses, and the saved B2707 mapping is detected.
The dialog no longer depends on a native HTML select popup or requires manual profile selection before capture
when the aircraft mapping is known. Its key field receives focus as soon as the profile is read.

Primary/Secondary identity is stored as hints in the native key queue; only chords actually present in the
projected native bindings appear assigned. Cancellation restores the original hints, and successful apply or
transaction recovery keeps the new hints. See the [regression verification](test-evidence/2026-11-07-xplane-assignment-regression.md).
