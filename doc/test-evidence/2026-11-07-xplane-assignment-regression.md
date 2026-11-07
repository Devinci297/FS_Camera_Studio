# X-Plane camera assignment regression fix

November 7, 2026. Local candidate based on release 0.5.3; not a new published release.

## Reproduced failure

The 0.5.3 service rejects the user's copied Devinci B2707 keyboard file with `Unrecognized keyboard preference line; no changes made.` Valid X-Plane command descriptions after the three binding tokens trigger this error. The previous UI also required an explicit selection in an HTML select popup before enabling capture and had removed the familiar Primary/Secondary slot controls.

## Changes

- Read valid trailing command descriptions while preserving all unchanged line bytes.
- Match the loaded aircraft title to the saved `acf_profile` association in Miscellaneous.prf; only select a profile that actually exists. An unmatched multi-profile setup retains a visible in-panel picker.
- Replace the browser select popup with in-panel buttons, automatically focus capture, and restore independent Primary/Secondary controls and per-slot clearing.
- Preserve slot identities across native file writes, reopening, cancellation and restart. Slot hints never invent assignments absent from the native projected file.
- Preserve conflict review, deferred writes, exact-owner validation, backups and atomic replacement.

## Validation

- **323 shared tests / 76,109 checks** and **11 native keyboard tests / 53 checks** passed.
- All **28 copied real keyboard profiles** parse successfully; the B2707 mapping selects `Devinci B2707`. Real preferences were only read, never edited by these tests.
- Main and mini headless tests use the actual C++ service and helper in disposable folders. They capture keys without manually focusing the field, verify TextFocus delivery, assign both slots, clear Primary while preserving Secondary, reopen/cancel, change profiles, and verify native conflict removal and stable slot identity after process exit. Short-height footer checks pass. Rendered main/mini dialogs were inspected.
- The fixture's former use of the browser-reserved `window.frames` property was corrected so keyboard focus requests are now actually observed.
- Five additional UI suites passed: MSFS dual bindings, short assignment dialog layout, key capture (9 regressions), X-Plane bindings, and movement/zoom bindings.
- Real-helper lifecycle checks passed: missing helper, stale previews, final-save conflict changes, cancel/retry, another process running, read-only recovery and Unicode paths.

## Artifact and acceptance

Candidate: `build/assignment-fix/XPCS-assignment-fix.zip`. Payload and source hashes: `build/assignment-fix/verified-build.json`. The payload retains the 0.5.3 base version and updater path; the local candidate is identified by its receipt and hashes.

Live acceptance remains pending. No computer-use tools were used, no live simulator was closed by the agent, and test fixtures did not alter user camera profiles or keyboard settings. Installation state is recorded separately in `build/assignment-fix/installation.json` when installation is possible.
