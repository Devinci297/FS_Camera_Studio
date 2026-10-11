# FS Camera Studio — data and protocol contract (v2)

Single contract between the in-sim panels (`ui/`) and the camera module (`src/WasmModule`). It implements design
document 2.0 §4.7, §5, §6, §8, §9 and §15.2. Change this file first, then the code.

```text
 MSFS toolbar panel "Camera Studio"        MSFS panel "Camera Studio Mini" (bottom strip)
   ui/index.html?surface=main                ui/index.html?surface=mini
            │  CommBus  FSCS_UI_TO_HOST  (listener.callWasm)            ▲
            ▼                                                           │ CommBus FSCS_HOST_TO_UI (listener.on)
 FSCameraStudio.wasm  — standalone module in the package `modules` folder
   coordinator · motion engine · working drafts · repository (\work) · input adapter (in-module SimConnect EX1)
            │
   fsCamera* (the only camera writer)
```

There is no desktop application, tray app or external process.

## 1. Conventions

* **Canonical frame (FRD):** right-handed, x forward, y right, z down. A camera looks along its local +x; its up is −z.
* **Orientation:** unit quaternion `[x, y, z, w]`, camera → anchor, `q = Rz(heading)·Ry(pitch)·Rx(bank)`.
  heading > 0 turns right, pitch > 0 looks up, bank > 0 rolls right.
* **Authoring (screen) axes:** X = right, Y = up, Z = forward, meters. FRD = `[Z, X, −Y]` (design §4.2).
  The UI converts authoring ⇄ FRD; only the module converts FRD ⇄ SDK (two different conversions, two test sets).
* **Projection:** stored as simulator FOV radians (`simulatorFovRadians`); the UI shows degrees 30–110
  (presets 40/62/85), intersected with the qualified range from `CapabilitiesChanged`.
* **IDs:** lower-case RFC 4122 UUID strings. `viewId` 1–20 is an optional recall slot (0 = none), unique per profile,
  not a camera-count limit.

## 2. Documents (module-owned, stored in `\work`)

### 2.1 Profile — schemaVersion 2

```json
{
  "schemaVersion": 2,
  "profileId": "b8cfa6d9-2fe2-48d1-8bd2-f4e3df83722a",
  "name": "My cameras",
  "aircraftMatch": { "strategy": "aircraftPath", "modelKey": "asobo-c172sp-classic", "variantKey": null,
                     "displayName": "C172 Skyhawk", "confirmed": false },
  "calibration": {
    "frameContract": "FRD-camera-to-anchor-xyzw-v1",
    "anchor": { "kind": "sdkPilotEyepoint", "seedDefinitionName": null,
                "correctionMetersFRD": [0, 0, 0], "correctionRotationXYZW": [0, 0, 0, 1] },
    "adapterContract": "pending-qualification",
    "simulatorBuild": null
  },
  "requiredSchemaFeatures": [],
  "defaultCameraId": "c212581c-…",
  "categories": [ { "id": "77cbb884-…", "name": "Cockpit", "cameraIds": ["c212581c-…"] } ],
  "cameras": [ {
    "id": "c212581c-…", "categoryId": "77cbb884-…", "name": "Pilot view",
    "mode": "aircraftCockpit", "requiredCapabilities": ["cockpitPose", "interaction", "fov"],
    "viewId": 1, "favorite": true,
    "positionMetersFRD": [0, 0, 0], "orientationCameraToAnchorXYZW": [0, 0, 0, 1],
    "projection": { "kind": "simulatorFovRadians", "value": 1.0821, "axis": "adapterDefined" },
    "transition": { "kind": "smoothstep3", "durationSeconds": 0.65, "delaySeconds": 0 },
    "interaction": true, "trackingPolicy": "nativeIfQualified", "effects": { "enabled": false },
    "temporaryLookPolicy": "resetOnRecall", "levelHorizon": false, "seedDefinitionName": null
  } ],
  "sequences": []
}
```

* `aircraftMatch.strategy`: `aircraftPath` | `userConfirmedModelVariant` | `none`.
* `mode`: `aircraftCockpit` (MVP); `aircraftExterior` (Preview — cockpit audio limitation);
  `airport` (Preview — world-fixed coordinates, pending live qualification). `orbit`, `flyby`, and
  `free` are stored and editable but **cannot activate** (reason shown beside the control).
* Airport profiles have `scope: "airport"`, `requiredSchemaFeatures: ["airport-world-v1"]`, and
  `airport: {icao, name, origin: {lat, lon, altitude}}`. Cameras retain a `worldOrigin`, `airportIcao`,
  and stable `generatedKey`. Position is local FRD about that geodetic origin; the SDK adapter converts
  it to world latitude/longitude/altitude. Only the currently loaded airport is combined with the aircraft
  profile for display. Each domain is validated and saved separately; cross-domain category moves are rejected.
* `transition.kind`: `cut` (Instant), `smoothstep3` (Smooth, default), `smoothstep5` (Cinematic).
  `durationSeconds` 0.1–15 (ignored for `cut`); `delaySeconds` 0–30 = **start delay** before movement.
* `mouseLookOnRelease`: optional `retain` (default) | `reset`. Per camera: keep temporary native right-drag look, or recenter it on button release. Does not overwrite the saved pose.
* `temporaryLookPolicy`: `resetOnRecall` | `retain`. `levelHorizon` is capability-gated (disabled until qualified).
* Validation: ≤ 10 MiB, finite numbers, |position| ≤ 10 000 m, FOV 0.1–2.8 rad, quaternion norm within 1e-3
  (normalised; zero rejected), unique UUIDs, every camera in exactly one category matching its `categoryId`,
  existing `defaultCameraId`, unique non-zero `viewId`, ≤ 500 cameras, ≤ 100 categories, names 1–60 chars.
  Unknown `requiredSchemaFeatures` → read-only, cannot activate. Schema 1 documents migrate (`shortcutSlot` →
  `viewId`, `smoothstep5` → `smoothstep3`, `smoothstep7` → `smoothstep5`) into a new revision; the input is retained.

### 2.2 Bindings — schemaVersion 2

```json
{ "schemaVersion": 2, "bindings": [
  { "action": "selectView", "viewId": 1, "input": { "kind": "key", "definition": "VK_LCONTROL+VK_LMENU+VK_1", "label": "Ctrl + Alt + 1" } },
  { "action": "peekView", "viewId": 2, "input": { "kind": "joystick", "deviceId": 1, "deviceName": "T.16000M", "definition": "joystick:1:button:4", "label": "T.16000M · Button 4" } },
  { "action": "releaseCamera", "input": { "kind": "key", "definition": "VK_LCONTROL+VK_LMENU+VK_R", "label": "Ctrl + Alt + R" } }
] }
```

Actions: `selectView` (+`viewId`), `peekView` (+`viewId`), `nextCamera`, `previousCamera`, `nextCategory`,
`returnToPilot`, `releaseCamera`, `recenterLook`, `toggleEditor`, `toggleMini`, `lookLeft`, `lookRight`, `lookUp`,
`lookDown`, `moveForward`, `moveBack`, `moveLeft`, `moveRight`, `moveUp`, `moveDown`, `moveFast`, `zoomIn`, `zoomOut`.
An input definition maps to at most one action. Keys are captured by the panel (keydown while the Assign dialog
has focus) and stored as MSFS input definition strings; controller inputs are chosen from `ControllersListed`
plus a button/hat number and confirmed with a live down/up test. Inputs are mapped in-module through
`SimConnect_MapInputEventToClientEvent_EX1`, so they work with the editor closed (flight loaded only).

Movement defaults are stored and editable: W/S forward/back, A/D left/right, Q/E up/down, Shift faster movement.
`movementDefaultsVersion: 1` records the one-time migration of the previous invisible runtime defaults. Missing
defaults are added only if neither the action nor the input is occupied. Clearing a migrated assignment stays
cleared across save/reload; runtime mapping never supplies hidden fallbacks.

Zoom also accepts `{ "kind": "simZoom", "definition": "sim:ZOOM_IN", "label": "Mouse wheel up / MSFS zoom in" }`
and `sim:ZOOM_OUT`. Only `zoomIn` / `zoomOut` may use these inputs. They observe the corresponding MSFS zoom
events (including ZOOM_PLUS / ZOOM_MINUS aliases) without masking them; each event is a bounded 2-degree FOV
step, not a held input. Wheel operation depends on MSFS's zoom assignment and delivery of these events, pending
live verification. This does not install a full-screen pointer surface. Assign supports keys, controllers, or either
wheel direction; replacing a conflict clears the old action atomically. Text focus, pause, release and simulator
camera takeover suppress these pulses.

### 2.3 Settings — schemaVersion 2

```json
{ "schemaVersion": 2,
  "ui": { "shortcuts": true, "contrast": false, "backups": true, "remember": true, "precision": "normal",
          "theme": "dark", "reducedMotion": false, "miniEnabled": true, "miniHover": true, "miniPinned": false },
  "host": { "allowUnqualifiedAdapter": false, "lookSpeedDegPerSec": 60, "moveSpeedMetersPerSec": 0.5 } }
```

`remember` keeps the last selected view per profile as navigation metadata (never dirties camera content).
`ui.precision` also scales held-key translation: Fine 0.1x, Normal 1x, Coarse 10x of
`host.moveSpeedMetersPerSec`; `moveFast` multiplies that by four while held. Mouse-look sensitivity is independent.
`ui.miniPinned` is a global preference: changing it through `UpdateDraft` saves it immediately on its own and never
marks settings as unsaved.

## 3. Transport

CommBus events: UI → module `FSCS_UI_TO_HOST` (`listener.callWasm`), module → UI `FSCS_HOST_TO_UI`
(`fsCommBusCall(..., FsCommBusBroadcast_JS)`). Payloads are UTF-8 JSON text.

**Chunking (both directions):** a message whose JSON exceeds 4000 characters is sent as consecutive
`{"part": i, "of": n, "msgId": "<sender-unique>", "chunk": "<substring>"}` envelopes (i from 0). The receiver
reassembles by `msgId`, discards incomplete sets after 5 s, and rejects totals above 2 MiB.

**Paused simulator:** JS does not process module→UI events while the sim is paused. The module therefore keeps
only the latest state per event name for each surface (coalescing) and never builds a per-frame backlog.

## 4. Envelopes

UI → module:
```json
{ "protocolVersion": 2, "requestId": "main-41", "uiInstanceId": "main-7f3a…", "moduleBootId": "…",
  "sessionEpoch": 4, "draftRevision": 12, "action": "ActivateCamera", "payload": { … } }
```
* `uiInstanceId` is new on every panel load (`<surface>-<random>`); `requestId` is unique per instance.
* `moduleBootId` / `sessionEpoch` are echoed from the last snapshot; omit both only in `Hello`.

Module → UI:
* Ack: `{ "type": "ack", "to": "<uiInstanceId>", "requestId": "main-41", "status": "Accepted|Completed|Rejected|Cancelled|TimedOut|Pending", "reason": null, "result": { … } }`
* Event: `{ "type": "event", "name": "OwnershipChanged", "seq": 812, "moduleBootId": "…", "sessionEpoch": 4, "payload": { … } }`
  (`to` present only for instance-directed events such as `ViewCaptured`).

Rules:
* Messages with another `moduleBootId` are rejected (`staleBoot`) except `Hello`, `RequestSnapshot`, `ReleaseCamera`.
* Camera-motion actions with a stale `sessionEpoch` are rejected (`staleEpoch`): `ActivateCamera`, `EnableCamera`,
  `BeginPeek`, `CycleCamera`, `NextCategory`, `ReturnToPilot`, `CaptureView`, `SeedFromDefinition`, `BakeTemporaryLook`.
* `UpdateDraft` with a stale `draftRevision` is rejected (`staleDraft`) and the UI re-requests a snapshot.
* `ReleaseCamera` is a priority action, always processed, never queued behind other work.
* Accepted never means a view moved or a save became durable.

## 5. Actions (UI → module)

| Action | Payload | Result / notes |
| --- | --- | --- |
| `Hello` | `surface` (`main`\|`mini`) | Ack + `Snapshot` event addressed to the instance; starts a subscription lease |
| `RenewLease` | `visible` (bool) | Every 2 s while visible. A lease lapses after 6 s; lapsed/hidden instances get no streaming (delivery only — never releases the camera) |
| `RequestSnapshot` | — | Fresh `Snapshot` to this instance |
| `SelectCamera` | `cameraId` | Navigation selection shared by both surfaces (`SelectionChanged`); never activates |
| `ActivateCamera` | `cameraId` | Recall from the current working draft; acquires if needed. Last command wins |
| `EnableCamera` | — | Activate the current selection (Release ⇄ Enable button) |
| `ReleaseCamera` | — | Yield to the simulator |
| `BeginPeek` / `EndPeek` | `cameraId`, `peekToken` / `peekToken` | Direct selection invalidates a peek |
| `CycleCamera` | `delta` ±1 | Wraps within the current category |
| `NextCategory`, `ReturnToPilot`, `RecenterLook` | — | |
| `BeginGesture` / `RenewGesture` / `EndGesture` | `gestureToken`, `input` (ManualInput rates) | Continuous UI motion; 150 ms lease renewed every 50 ms; expiry neutralises motion only |
| `UpdateDraft` | `ops` (§5.1) | Applies to the working draft; broadcasts `DraftChanged`. Previews the active camera live if authorised |
| `RevertChanges` | — | Restores committed profile/settings/bindings; unsaved new cameras vanish; releases first if the active view becomes invalid |
| `CommitChanges` | `backup` (bool) | Ack `Pending` with `result.revisions` tuple; later `CommitCompleted`. Idempotent by requestId |
| `OpenProfile` | `profileId`, `discard` (bool) | Rejected `unsavedChanges` when the draft is dirty and `discard` is false |
| `CreateProfile` | `name`, `forAircraft` (bool), `copyOf` (profileId\|null) | New committed profile; becomes the working profile |
| `DeleteProfile` | `profileId` | Not allowed for the profile in use |
| `BindProfileToAircraft` | `profileId` | Sets `userConfirmedModelVariant` for the loaded aircraft (committed) |
| `ListBackups` / `LoadBackup` | — / `revision` | Loads an older revision as the working draft (must be saved) |
| `ExportProfile` | `profileId`, `working` (bool) | `result.code` (profile code JSON text), `result.label` ("working copy"/"saved") |
| `ImportProfile` | `code` | Imports as a copy with all UUIDs remapped (default/last view too); `result.report` |
| `CaptureView` | — | `ViewCaptured` (to instance) — read-and-review, never saved implicitly |
| `EnumerateDefinitions` | — | `DefinitionsListed` |
| `SeedFromDefinition` | `name` | Acquire, apply native definition, read back → `ViewCaptured` |
| `BakeTemporaryLook` | — | Composes look into the active camera's draft (`DraftChanged`) and clears the look |
| `ListControllers` | — | `ControllersListed` |
| `TestInput` | `input` | Temporarily maps the input; reports `InputTest` down/up events for 15 s |
| `TestBinding` | `binding` | Dispatches the action once; reply distinguishes dispatch from visible recall |
| `RelaySurface` | `target` (`main`\|`mini`), `op` (`expand`\|`compact`\|`show`) | Broadcast `SurfaceRequest`; e.g. mini "Edit" → main expands on Cameras |
| `GetDiagnostics` | — | `result.text` preview for in-panel export |
| `TextFocus` | `focused` (bool) | A panel text field has focus: bindings other than Release Camera are suppressed |
| `QualificationStart` | — | Begins the in-panel axis check; live control is allowed on an unqualified build while it runs. `result`: `simVersion`, `contract`, `axes` |
| `QualificationProbe` | `axis` (`forward\|right\|up\|pitch\|heading\|bank`) | Offsets the active view by 30 cm / 15°; `result.expect` describes what should be seen |
| `QualificationRestore` | — | Returns the active view to its draft pose |
| `QualificationRecord` | `answers` {axis: `correct\|reversed\|wrong`}, `recall` (`pass\|fail`), `notes` | Flips reversed signs, records G01/G03 for this simulator build (Pending → `CommitCompleted` kind `qualification`) |
| `QualificationEnd` | — | Ends the session; releases if the build is still unqualified and testing is not allowed |

### 5.1 Draft operations

`ops` is an ordered array; the whole array applies atomically or not at all:

| op | fields |
| --- | --- |
| `setCamera` | `camera` (full camera object; adds when new) |
| `removeCamera` | `cameraId` |
| `setCategories` | `categories` (full ordered list with `cameraIds`) |
| `setProfileFields` | any of `name`, `defaultCameraId`, `aircraftMatch` |
| `setSettings` | `settings` (§2.3 document) |
| `setBindings` | `bindings` (§2.2 document) |

Coalesce rapid edits (typing, steppers, slider) to at most ~10 `UpdateDraft` per second; send only the camera
being edited.

## 6. Events (module → UI)

| Name | Payload |
| --- | --- |
| `Snapshot` | everything below in one object: `moduleBootId`, `sessionEpoch`, `session`, `aircraft`, `ownership`, `capabilities`, `activeCamera`, `selection`, `profiles`, `working` {`profile`, `settings`, `bindings`, `draftRevision`, `dirty` {`profile`,`settings`,`bindings`}, `committedRevision`, `hasCommittedVersion` {cameraId: bool}}, `pendingCommit`, `look` |
| `SessionChanged` | `moduleBootId`, `sessionEpoch`, `session` (`Initializing\|Discovering\|Ready\|Suspended\|Faulted`), `reason` |
| `AircraftChanged` | `title`, `modelKey`, `matchProfileId`, `matchReason` (plain text, e.g. "Matched by aircraft package") |
| `OwnershipChanged` | `state` (`Observing, Acquiring, Active, SimulatorSuspended, OtherClient, UserDisabled, Releasing, Faulted`), `gameControlled`, `reason` |
| `CapabilitiesChanged` | `adapterQualified`, `adapterContract`, `captureQualified`, `allowUnqualifiedAdapter`, `canActivate`, `blockReason`, `simVersion`, `fovRangeDeg` [min,max], `levelHorizon` (bool), `modes` {mode: {available, preview, reason}} |
| `ActiveCameraChanged` | `profileId`, `cameraId`, `peeking`, `transitioning`, `fromDraft` |
| `SelectionChanged` | `profileId`, `cameraId` |
| `DraftChanged` | `draftRevision`, `dirty`, `ops` (as applied, so mirrors update without a snapshot), `origin` (uiInstanceId) |
| `CommitCompleted` | `requestId`, `kind`, `status` (`Committed\|Failed`), `revisions`, `message`, `report`; kind `qualification` adds `g01`, `g03` (bool) |
| `ProfilesChanged` | `profiles` [{`profileId`,`name`,`displayName`,`modelKey`,`matchStrategy`,`cameraCount`,`revision`,`readOnly`}] |
| `TemporaryLookChanged` | `active`, `positionMetersFRD`, `pbhDeg`, `fovDeltaRad` |
| `ViewCaptured` | `ok`, `positionMetersFRD`, `orientationCameraToAnchorXYZW`, `fovRad`, `frame`, `qualified`, `source`, `message` |
| `DefinitionsListed` | `names` |
| `ControllersListed` | `controllers` [{`deviceId`,`name`}] |
| `InputTest` | `definition`, `phase` (`down`\|`up`) |
| `BindingTriggered` | `action`, `viewId`, `result` |
| `SurfaceRequest` | `target`, `op` |
| `FaultRaised` | `code`, `message` |

## 7. Plain-language status

| Condition | Text |
| --- | --- |
| no snapshot yet | "Starting Camera Studio…" (after 10 s: "Camera Studio's camera module did not start — reload the panel or restart the flight.") |
| no flight / aircraft | "Load a flight to use your cameras" |
| Observing | "Simulator in control" |
| Acquiring | "Taking camera control…" |
| Active | "Camera active" |
| SimulatorSuspended | "Paused by the simulator" |
| OtherClient | "Another add-on has control" |
| UserDisabled | "Camera control by add-ons is disabled in the simulator" |
| Releasing | "Returning camera to the simulator…" |
| released by user | "Camera released" |
| Faulted | "Camera stopped after an error — use the simulator's camera controls, then Enable" |
