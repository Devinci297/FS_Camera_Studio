# Traffic camera adjustments

While spotting AI traffic, camera movement, rotation and zoom apply to the visible traffic camera. The target stays
selected and chase views continue following it. Saved aircraft/airport camera poses remain unchanged.

- Hold right-click over the simulator scene and drag to rotate.
- Use movement/look/zoom bindings. MSFS defaults: W/A/S/D, Q/E for height, Shift for faster movement.
- In X-Plane, assign the `xpcs/move_*`, `xpcs/look_*`, `xpcs/zoom_in`, `xpcs/zoom_out`, and `xpcs/recenter_look`
  commands in Keyboard/Joystick settings.
- Click **Mouse look** while spotting (or **More > Mouse look** in a narrow mini panel) to open **Traffic camera
  controls**. Its buttons move in 0.5 m increments and rotate in 5 degree increments.
- **Recenter** restores the traffic framing. Selecting another target/view clears temporary adjustments.
- Manual adjustment disables Auto-spot switching, keeps the current target, and prevents automatic runway/fly-by
  repositioning from replacing your framing. Recenter allows the view's automatic repositioning again.
- Adjustments work without an aircraft profile. Stale dialogs cannot adjust your aircraft camera after spotting ends.

## Validation

321 native tests / 75,390 checks passed. New regression coverage includes profile-free chase/fly-by, a saved tower
view, native mouse and panel deltas, input blocking, recentering, unchanged underlying poses, a moving AI target,
frame stalls, and Auto-spot cancellation. The traffic-controls, spotting, mini-traffic, spots, movement-bindings and
X-Plane-bindings UI suites pass. Main and mini dialog screenshots were inspected.

Both MSFS WASM and Windows X-Plane Release builds succeeded. Existing SDK/header warnings remain.
Local payloads, separate from the published 0.5.0 artifacts:

- `build/traffic-controls/fs-camera-studio` (complete MSFS package using the reviewed 0.5.0 host).
- `build/traffic-controls/xplane/XPCameraStudio` (X-Plane plugin and current UI).
- `build/traffic-controls/verified-build.json` (payload SHA-256 inventories).

The development X-Plane build was installed locally with a verified rollback backup. The MSFS candidate was
not installed. Live simulator acceptance of these changes remains pending.

## Release 0.5.1

These changes are included in 0.5.1. The development-state notes above describe the earlier local build.
See [0.5.1 release notes](releases/0.5.1.md) and [release verification](test-evidence/2026-11-07-v0.5.1.md).
