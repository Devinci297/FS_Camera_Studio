# Automatic checks and click-to-update

Install the 0.5.2 Windows setup once. It includes the updater and its per-user Windows link handler.
Normal camera operation does not require a running desktop app or service.

- Checks run when an in-sim panel becomes ready, then at most every six hours while a panel is open. They work
  from a mini-only session. Failed checks back off; manual checks remain available.
- The editor shows an update notice and version badge; the mini gets a dedicated update row. Later dismisses
  that version's editor notice. Cached metadata, last check and preference survive panel reopening.
- Automatic checks are enabled by default and can be disabled in Settings > Saving & updates.
- Installation always requires **Update now**. Save your views first. The Windows updater downloads the official
  installer, checks its exact size and SHA-256, and waits for MSFS, X-Plane and the XPCS key helper to exit.
  It then runs setup, preserving the existing installer-managed simulator choices and folders.
- Cancel stops downloading/waiting. Once setup starts the updater waits for completion and verifies the resulting
  installed version. It never kills a simulator, clicks controls, or updates the loaded plugin in place.
- XPCS launches the installed helper directly. MSFS requests the registered `fscamerastudio://update/<version>`
  link through OpenBrowser. That real simulator handoff still needs live acceptance; the UI does not claim an
  acknowledged launch. If it does not open, use **Download installer**, or the Start-menu **Check for updates**.
- The updater accepts only published normal releases from this repository. It refuses downgrade, malformed
  URLs/versions, absent hashes, corrupt/oversize downloads and unapproved redirect hosts. A per-user mutex prevents
  concurrent updaters. It locks the verified installer through setup execution. No arbitrary URL is accepted from
  the panel or protocol handler.

The helper uses Windows' .NET Framework and is packaged in a version-specific directory, allowing a running
updater to be replaced by a newer version without overwriting its executable. The isolated installer harness
uses a different protocol name and support folder. Tests never run the downloaded production installer.
