/*
 * FS Camera Studio - panel <-> camera module bridge (doc/protocol.md v2 sec. 3-sec. 6).
 *
 * The UI runs as MSFS 2024 in-game toolbar panels (Coherent GT). Its only peer is the Camera Studio WASM module:
 *   panel > module   listener.callWasm("FSCS_UI_TO_HOST", jsonText)
 *   module > panel   listener.on("FSCS_HOST_TO_UI", jsonText)
 * Messages longer than 4000 characters travel as { part, of, msgId, chunk } frames in both directions.
 *
 * Demo mode: when RegisterCommBusListener is undefined (plain browser) an in-memory module that follows
 * protocol v2 is used, so the preview exercises the real message flow. Nothing touches disk or network.
 *
 * API - FSCBridge.create({ surface }):
 *   bridge.mode / surface / uiInstanceId
 *   bridge.context                     { moduleBootId, sessionEpoch, draftRevision } - echoed in envelopes
 *   bridge.on('event' | 'ack', fn)
 *   bridge.start()                     Hello (retried every 2 s until a Snapshot), then RenewLease every 2 s
 *   bridge.send(action, payload)       > requestId
 *   bridge.request(action, payload)    > Promise<ack>
 *   bridge.demo                        demo controls (demo mode only)
 */
(function (root) {
  'use strict';

  const M = root.FSCMath;
  const clone = value => (value === undefined ? undefined : JSON.parse(JSON.stringify(value)));
  const same = (a, b) => JSON.stringify(a) === JSON.stringify(b);
  const CHUNK_TRIGGER = 4000;
  const CHUNK_TTL_MS = 5000;
  const CHUNK_MAX_TOTAL = 2 * 1024 * 1024;
  const HELLO_RETRY_MS = 2000;
  const LEASE_MS = 2000;
  const ACK_TIMEOUT_MS = 15000;
  const randomHex = n => { let s = ''; for (let i = 0; i < n; i++) s += Math.floor(Math.random() * 16).toString(16); return s; };

  /* ------------- Chunking (sec. 3) ------------- */
  function encodeFrames(message, msgId) {
    const text = JSON.stringify(message);
    if (text.length <= CHUNK_TRIGGER) return [text];
    const frames = [];
    let size = 3000;
    // Shrink the slice size until every wrapped frame (with JSON escaping) fits the frame limit.
    for (;;) {
      frames.length = 0;
      const of = Math.ceil(text.length / size);
      let fits = true;
      for (let i = 0; i < of; i++) {
        const frame = JSON.stringify({ part: i, of, msgId, chunk: text.slice(i * size, (i + 1) * size) });
        if (frame.length > CHUNK_TRIGGER) { fits = false; break; }
        frames.push(frame);
      }
      if (fits) return frames;
      size = Math.floor(size * 0.8);
    }
  }
  class Assembler {
    constructor() { this.sets = new Map(); }
    /** Returns a complete message object, or null while parts are missing / on error. */
    push(data) {
      let message = data;
      if (typeof message === 'string') {
        try { message = JSON.parse(message); } catch (error) { console.warn('Unreadable CommBus message'); return null; }
      }
      if (!message || typeof message !== 'object') return null;
      if (typeof message.part !== 'number' || message.msgId === undefined) return message;
      const now = Date.now();
      this.sets.forEach((entry, key) => { if (now - entry.at > CHUNK_TTL_MS) this.sets.delete(key); });
      const key = String(message.msgId);
      const entry = this.sets.get(key) || { of: message.of, chunks: {}, count: 0, size: 0, at: now };
      if (!Object.prototype.hasOwnProperty.call(entry.chunks, message.part)) {
        entry.count++;
        entry.size += String(message.chunk || '').length;
      }
      entry.chunks[message.part] = String(message.chunk || '');
      entry.at = now;
      if (entry.size > CHUNK_MAX_TOTAL) { this.sets.delete(key); console.warn('Chunked message too large; dropped'); return null; }
      this.sets.set(key, entry);
      if (entry.count < entry.of) return null;
      this.sets.delete(key);
      const text = Object.keys(entry.chunks).map(Number).sort((a, b) => a - b).map(k => entry.chunks[k]).join('');
      try { return JSON.parse(text); } catch (error) { console.warn('Unreadable chunked CommBus message'); return null; }
    }
  }

  /* ------------- Transports ------------- */
  /** The only place that knows about the MSFS CommBus. */
  class CommBusTransport {
    constructor() { this.listener = null; this.handlers = []; this.isOpen = false; }
    static available() { return typeof root.RegisterCommBusListener === 'function'; }
    start(onOpen) {
      try {
        this.listener = root.RegisterCommBusListener(() => {
          this.isOpen = true;
          if (onOpen) onOpen();
        });
        this.listener.on('FSCS_HOST_TO_UI', data => this.handlers.forEach(cb => cb(data)));
      } catch (error) {
        console.error('CommBus listener could not be created', error);
      }
    }
    send(frame) {
      if (!this.listener || !this.isOpen) return false;
      try { this.listener.callWasm('FSCS_UI_TO_HOST', frame); return true; } catch (error) { console.warn('CommBus send failed', error); return false; }
    }
    onMessage(cb) { this.handlers.push(cb); }
  }

  /** In-browser stand-in with the same shape; frames are real JSON text so chunking is exercised too. */
  class DemoTransport {
    constructor() {
      this.handlers = [];
      this.module = createDemoModule(frame => setTimeout(() => this.handlers.forEach(cb => cb(frame)), 0));
    }
    start(onOpen) { setTimeout(onOpen, 0); }
    send(frame) { setTimeout(() => this.module.receiveFrame(frame), 0); return true; }
    onMessage(cb) { this.handlers.push(cb); }
  }

  /* ------------- Bridge ------------- */
  function create(options) {
    const surface = (options && options.surface) || 'main';
    const transport = CommBusTransport.available() ? new CommBusTransport() : new DemoTransport();
    const uiInstanceId = surface + '-' + randomHex(12);
    const context = { moduleBootId: null, sessionEpoch: null, draftRevision: null };
    const listeners = { event: [], ack: [] };
    const pending = new Map();
    const assembler = new Assembler();
    let seq = 0, frameSeq = 0, snapshotSeen = false, helloTimer = 0, leaseTimer = 0;

    const emit = (kind, message) => listeners[kind].forEach(fn => { try { fn(message); } catch (error) { console.error('Bridge listener failed', error); } });

    transport.onMessage(data => {
      const message = assembler.push(data);
      if (!message) return;
      if (message.type === 'ack') {
        if (message.to && message.to !== uiInstanceId) return;
        const entry = pending.get(message.requestId);
        if (entry) { pending.delete(message.requestId); clearTimeout(entry.timer); entry.resolve(message); }
        emit('ack', message);
      } else if (message.type === 'event') {
        if (message.to && message.to !== uiInstanceId) return;
        if (message.name === 'Snapshot') snapshotSeen = true;
        // Every event envelope carries the module's boot id and session epoch: keep the echo fields current.
        if (message.moduleBootId) {
          const rebooted = context.moduleBootId !== null && message.moduleBootId !== context.moduleBootId;
          context.moduleBootId = message.moduleBootId;
          if (rebooted && message.name !== 'Snapshot') post('RequestSnapshot', {});
        }
        if (typeof message.sessionEpoch === 'number') context.sessionEpoch = message.sessionEpoch;
        emit('event', message);
      }
    });

    function post(action, payload) {
      const requestId = surface + '-' + (++seq);
      const envelope = { protocolVersion: 2, requestId, uiInstanceId };
      if (action !== 'Hello') {
        if (context.moduleBootId !== null) envelope.moduleBootId = context.moduleBootId;
        if (context.sessionEpoch !== null) envelope.sessionEpoch = context.sessionEpoch;
      }
      if (context.draftRevision !== null) envelope.draftRevision = context.draftRevision;
      envelope.action = action;
      envelope.payload = payload || {};
      const frames = encodeFrames(envelope, uiInstanceId + ':' + (++frameSeq));
      let ok = true;
      frames.forEach(frame => { if (!transport.send(frame)) ok = false; });
      return { requestId, ok };
    }

    return {
      mode: transport instanceof CommBusTransport ? 'commbus' : 'demo',
      surface,
      uiInstanceId,
      context,
      demo: transport instanceof DemoTransport ? transport.module.controls : null,
      on(kind, fn) { listeners[kind].push(fn); },
      send(action, payload) { return post(action, payload).requestId; },
      request(action, payload) {
        return new Promise(resolve => {
          const sent = post(action, payload);
          if (!sent.ok) { resolve({ type: 'ack', requestId: sent.requestId, status: 'Rejected', reason: 'notConnected', result: null }); return; }
          const timer = setTimeout(() => { pending.delete(sent.requestId); resolve({ type: 'ack', requestId: sent.requestId, status: 'TimedOut', reason: 'timeout', result: null }); }, ACK_TIMEOUT_MS);
          pending.set(sent.requestId, { resolve, timer });
        });
      },
      start() {
        transport.start(() => {
          const hello = () => { if (!snapshotSeen) post('Hello', { surface }); };
          hello();
          helloTimer = setInterval(() => { if (snapshotSeen) clearInterval(helloTimer); else hello(); }, HELLO_RETRY_MS);
          leaseTimer = setInterval(() => {
            const visible = document.hidden !== true;
            if (snapshotSeen && visible) post('RenewLease', { visible });
          }, LEASE_MS);
        });
      },
      stop() { clearInterval(helloTimer); clearInterval(leaseTimer); },
    };
  }

  /* ----------------------------- Demo camera module (protocol v2) ----------------------------- */

  const FRAME_CONTRACT = 'FRD-camera-to-anchor-xyzw-v1';
  const C172 = { title: 'C172 Skyhawk', modelKey: 'asobo-c172sp-classic' };
  const MODE_FROM_TYPE = { cockpit: 'aircraftCockpit', external: 'aircraftExterior', orbit: 'orbit', flyby: 'flyby', airport: 'airport', free: 'free' };
  const NOT_AVAILABLE = 'Not available in this version - saved with the profile but cannot be activated.';
  const MODE_CAPS = {
    aircraftCockpit: { available: true, reason: null },
    aircraftExterior: { available: true, reason: 'Preview - exterior views keep cockpit sound in the current simulator build.' },
    orbit: { available: true, reason: null }, flyby: { available: true, reason: null },
    airport: { available: false, reason: NOT_AVAILABLE }, free: { available: false, reason: NOT_AVAILABLE },
  };
  const MOTION_ACTIONS = new Set(['ActivateCamera', 'EnableCamera', 'BeginPeek', 'CycleCamera', 'NextCategory', 'ReturnToPilot', 'CaptureView', 'SeedFromDefinition', 'BakeTemporaryLook']);
  const NEUTRAL_LOOK = () => ({ active: false, positionMetersFRD: [0, 0, 0], pbhDeg: [0, 0, 0], fovDeltaRad: 0 });
  const round = (value, digits) => { const r = Number(value.toFixed(digits)); return Object.is(r, -0) ? 0 : r; };

  // Seed poses are copied verbatim from the approved mockup (authoring axes X right, Y up, Z forward).
  function seedCamera(categoryId, name, viewId, pose, extras) {
    const p = Object.assign({ x: -0.245, y: 0.369, z: 0.183, pitch: 0, yaw: 0, roll: 0, fov: 62 }, pose || {});
    const x = extras || {};
    const mode = MODE_FROM_TYPE[x.type || 'cockpit'];
    return {
      id: M.uuid(), categoryId, name, mode,
      requiredCapabilities: mode === 'aircraftCockpit' ? ['cockpitPose', 'interaction', 'fov'] : ['fov'],
      viewId, favorite: Boolean(x.favorite),
      positionMetersFRD: [round(p.z, 4), round(p.x, 4), round(-p.y, 4)],
      orientationCameraToAnchorXYZW: M.quatFromHpb(p.yaw, p.pitch, p.roll).map(v => round(v, 7)),
      projection: { kind: 'simulatorFovRadians', value: round(M.degToRad(p.fov), 4), axis: 'adapterDefined' },
      transition: x.transition || { kind: 'smoothstep3', durationSeconds: 0.65, delaySeconds: 0 },
      interaction: mode === 'aircraftCockpit', trackingPolicy: 'nativeIfQualified', effects: { enabled: false },
      temporaryLookPolicy: x.temporaryLookPolicy || 'retain', levelHorizon: false, seedDefinitionName: null,
    };
  }
  function calibration() {
    return { frameContract: FRAME_CONTRACT, anchor: { kind: 'sdkPilotEyepoint', seedDefinitionName: null, correctionMetersFRD: [0, 0, 0], correctionRotationXYZW: [0, 0, 0, 1] }, adapterContract: 'pending-qualification', simulatorBuild: null };
  }
  function makeProfile(spec) {
    const profile = { schemaVersion: 2, profileId: M.uuid(), name: spec.name, aircraftMatch: spec.aircraftMatch, calibration: calibration(), requiredSchemaFeatures: [], defaultCameraId: null, categories: [], cameras: [], sequences: [] };
    spec.categories.forEach(group => {
      const category = { id: M.uuid(), name: group.name, cameraIds: [] };
      group.cameras.forEach(entry => {
        const camera = seedCamera(category.id, entry[0], entry[1], entry[2], entry[3]);
        category.cameraIds.push(camera.id);
        profile.cameras.push(camera);
        if (entry[0] === spec.defaultName) profile.defaultCameraId = camera.id;
      });
      profile.categories.push(category);
    });
    if (!profile.defaultCameraId && profile.cameras.length) profile.defaultCameraId = profile.cameras[0].id;
    return profile;
  }
  const SEED_GROUPS = {
    cockpit: { name: 'Cockpit', cameras: [
      ['Pilot view', 1, {}, { favorite: true, temporaryLookPolicy: 'resetOnRecall' }],
      ['PFD', 2, { z: -0.10, y: 0.31, pitch: -16, fov: 45 }, { favorite: true }],
      ['Autopilot & radios', 3, { x: 0.09, z: -0.14, y: 0.28, pitch: -24, yaw: 18 }],
      ['Throttles', 4, { x: 0.08, y: 0.11, z: 0.02, pitch: -54, fov: 45 }, { favorite: true }],
      ['Overhead & lights', 0, { y: 0.68, pitch: 42 }],
      ['Copilot', 0, { x: 0.46 }],
      ['Right window', 0, { x: 0.49, yaw: 90 }],
    ] },
    external: { name: 'External', cameras: [
      ['Tail', 5, { x: 0, y: 3, z: 14, pitch: -7 }, { type: 'external', favorite: true, transition: { kind: 'smoothstep5', durationSeconds: 1.2, delaySeconds: 0 } }],
      ['Left wing', 6, { x: -5, y: 0.5, z: 1, yaw: 90 }, { type: 'external' }],
      ['Nose', 0, { x: 0, y: 1, z: -7, yaw: 180 }, { type: 'external' }],
    ] },
    cabin: { name: 'Cabin', cameras: [['Front cabin', 0, { x: 0, y: 0.3, z: 1.4, yaw: 180 }]] },
    gear: { name: 'Landing gear', cameras: [['Nose gear', 0, { x: 1, y: -1, z: -2, yaw: -90 }, { type: 'external' }]] },
    walk: { name: 'Walkaround', cameras: [['Walkaround start', 0, {}, { type: 'free' }], ['Walkaround end', 0, { x: 5, y: 1, z: 4 }, { type: 'free' }]] },
    orbit: { name: 'Orbit', cameras: [['Orbit start', 0, { x: 0, y: 4, z: 12 }, { type: 'orbit' }]] },
    flyby: { name: 'Fly-by', cameras: [['Fly-by start', 0, { x: 15, y: 4, z: -15, yaw: -45 }, { type: 'flyby' }]] },
  };
  /** Default bindings the module seeds on first run. */
  function seedBindings() {
    const ctrlAlt = (key, label) => ({ kind: 'key', definition: 'VK_LCONTROL+VK_LMENU+' + key, label: 'Ctrl + Alt + ' + label });
    const list = [];
    for (let n = 1; n <= 9; n++) list.push({ action: 'selectView', viewId: n, input: ctrlAlt(String(n), String(n)) });
    list.push({ action: 'returnToPilot', input: ctrlAlt('P', 'P') });
    list.push({ action: 'nextCamera', input: ctrlAlt('N', 'N') });
    list.push({ action: 'previousCamera', input: ctrlAlt('B', 'B') });
    list.push({ action: 'releaseCamera', input: ctrlAlt('R', 'R') });
    list.push({ action: 'toggleEditor', input: ctrlAlt('E', 'E') });
    list.push({ action: 'toggleMini', input: ctrlAlt('M', 'M') });
    list.push({ action: 'recenterLook', input: ctrlAlt('C', 'C') });
    list.push({ action: 'lookLeft', input: ctrlAlt('VK_LEFT', 'Left arrow') });
    list.push({ action: 'lookRight', input: ctrlAlt('VK_RIGHT', 'Right arrow') });
    list.push({ action: 'lookUp', input: ctrlAlt('VK_UP', 'Up arrow') });
    list.push({ action: 'lookDown', input: ctrlAlt('VK_DOWN', 'Down arrow') });
    [['moveForward','W'],['moveBack','S'],['moveLeft','A'],['moveRight','D'],['moveUp','Q'],['moveDown','E'],['moveFast','VK_SHIFT']].forEach(entry => list.push({action:entry[0],input:{kind:'key',definition:entry[1],label:entry[1]==='VK_SHIFT'?'Shift':entry[1]}}));
    [['zoomIn','sim:ZOOM_IN','Mouse wheel up / MSFS zoom in'],['zoomOut','sim:ZOOM_OUT','Mouse wheel down / MSFS zoom out']].forEach(entry => list.push({action:entry[0],input:{kind:'simZoom',definition:entry[1],label:entry[2]}}));
    list.push({action:'saveViews',input:{kind:'key',definition:'VK_LCONTROL+S',label:'Ctrl + S'}});
    return { schemaVersion: 2, movementDefaultsVersion: 2, saveViewsDefaultsVersion:1, bindings: list };
  }

  function validateProfile(p) {
    if (!p || p.schemaVersion !== 2) return 'This profile uses an unknown format.';
    const nameOk = n => typeof n === 'string' && n.trim().length >= 1 && n.length <= 60;
    if (!nameOk(p.name)) return 'Profile names need 1-60 characters.';
    if (!Array.isArray(p.categories) || !Array.isArray(p.cameras)) return 'The profile is incomplete.';
    if (p.cameras.length > 500) return 'A profile can hold up to 500 cameras.';
    if (p.categories.length > 100) return 'A profile can hold up to 100 categories.';
    const ids = new Set(), seen = new Map(), views = new Set();
    for (const category of p.categories) {
      if (ids.has(category.id)) return 'Two items share the same ID.';
      ids.add(category.id);
      if (!nameOk(category.name)) return 'Category names need 1-60 characters.';
      for (const id of category.cameraIds) { if (seen.has(id)) return 'A camera appears in two categories.'; seen.set(id, category.id); }
    }
    for (const c of p.cameras) {
      if (ids.has(c.id)) return 'Two items share the same ID.';
      ids.add(c.id);
      if (!nameOk(c.name)) return 'Camera names need 1-60 characters.';
      if (seen.get(c.id) !== c.categoryId) return '"' + c.name + '" is not listed in its category.';
      const values = c.positionMetersFRD.concat(c.orientationCameraToAnchorXYZW, [c.projection.value]);
      if (!values.every(Number.isFinite)) return '"' + c.name + '" has an invalid number.';
      if (c.positionMetersFRD.some(v => Math.abs(v) > 10000)) return '"' + c.name + '" is more than 10 km away.';
      if (c.projection.value < 0.1 || c.projection.value > 2.8) return '"' + c.name + '" has a field of view outside the supported range.';
      const n = Math.hypot.apply(null, c.orientationCameraToAnchorXYZW);
      if (!(n > 0) || Math.abs(n - 1) > 1e-3) return '"' + c.name + '" has an invalid orientation.';
      if (c.viewId) { if (views.has(c.viewId)) return 'View ID ' + c.viewId + ' is used twice.'; views.add(c.viewId); }
    }
    if (seen.size !== p.cameras.length) return 'A category lists a camera that does not exist.';
    if (!p.cameras.some(c => c.id === p.defaultCameraId)) return 'Choose a default camera.';
    return null;
  }
  function remapIds(profile) {
    const map = new Map();
    const remap = id => { if (!id) return id; if (!map.has(id)) map.set(id, M.uuid()); return map.get(id); };
    profile.profileId = M.uuid();
    profile.categories.forEach(c => { c.id = remap(c.id); c.cameraIds = c.cameraIds.map(remap); });
    profile.cameras.forEach(c => { c.id = remap(c.id); c.categoryId = remap(c.categoryId); });
    profile.defaultCameraId = remap(profile.defaultCameraId);
    return profile;
  }
  /** Schema 1 > 2 migration (sec. 2.1). */
  function migrate(input) {
    const p = clone(input);
    if (p && p.schemaVersion === 1) {
      p.schemaVersion = 2;
      delete p.revision;
      (p.cameras || []).forEach(c => {
        c.viewId = c.shortcutSlot || 0;
        delete c.shortcutSlot;
        const kind = c.transition && c.transition.kind;
        if (kind === 'smoothstep5') c.transition.kind = 'smoothstep3';
        else if (kind === 'smoothstep7') c.transition.kind = 'smoothstep5';
        if (c.levelHorizon === undefined) c.levelHorizon = false;
      });
      if (p.aircraftMatch && p.aircraftMatch.confirmed === undefined) p.aircraftMatch.confirmed = p.aircraftMatch.strategy === 'userConfirmedModelVariant';
    }
    return p;
  }
  function applyOps(target, ops) {
    const doc = clone(target);
    for (const op of ops) {
      switch (op.op) {
        case 'setCamera': {
          const index = doc.profile.cameras.findIndex(c => c.id === op.camera.id);
          if (index >= 0) doc.profile.cameras[index] = clone(op.camera); else doc.profile.cameras.push(clone(op.camera));
          break;
        }
        case 'removeCamera':
          doc.profile.cameras = doc.profile.cameras.filter(c => c.id !== op.cameraId);
          doc.profile.categories.forEach(g => { g.cameraIds = g.cameraIds.filter(id => id !== op.cameraId); });
          break;
        case 'setCategories':
          doc.profile.categories = clone(op.categories);
          doc.profile.categories.forEach(g => g.cameraIds.forEach(id => { const camera = doc.profile.cameras.find(c => c.id === id); if (camera) camera.categoryId = g.id; }));
          break;
        case 'setProfileFields':
          ['name', 'defaultCameraId', 'aircraftMatch'].forEach(k => { if (op[k] !== undefined) doc.profile[k] = clone(op[k]); });
          break;
        case 'setSettings': doc.settings = clone(op.settings); break;
        case 'setBindings': doc.bindings = clone(op.bindings); break;
        default: throw { reason: 'unknownOp' };
      }
    }
    return doc;
  }

  function createDemoModule(sendFrame) {
    const g = SEED_GROUPS;
    const mine = makeProfile({ name: 'My cameras', defaultName: 'Pilot view', aircraftMatch: { strategy: 'aircraftPath', modelKey: C172.modelKey, variantKey: null, displayName: C172.title, confirmed: false }, categories: [g.cockpit, g.external, g.cabin, g.gear, g.walk, g.orbit, g.flyby] });
    const cinematic = makeProfile({ name: 'Cinematic collection', defaultName: 'Tail', aircraftMatch: { strategy: 'userConfirmedModelVariant', modelKey: C172.modelKey, variantKey: null, displayName: C172.title, confirmed: true }, categories: [g.external, g.orbit, g.flyby] });
    const airport = makeProfile({ name: 'Airport starter', defaultName: 'Tower', aircraftMatch: { strategy: 'none', modelKey: null, variantKey: null, displayName: 'Airport cameras', confirmed: false }, categories: [{ name: 'Airport', cameras: [
      ['Tower', 1, { x: 50, y: 25, z: 70 }, { type: 'airport', favorite: true }],
      ['Runway', 2, { x: 0, y: 2, z: -300 }, { type: 'airport', favorite: true }],
      ['Gate', 3, { x: 30, y: 2, z: 20 }, { type: 'airport' }],
    ] }] });
    const now = Date.now();
    const repo = { profiles: new Map(), settings: null, bindings: null };
    const addRepo = (profile, revision, history) => repo.profiles.set(profile.profileId, { committed: profile, revision, history: history || [], lastCameraId: null });
    const r2 = clone(mine); r2.cameras[0].projection.value = round(M.degToRad(70), 4);
    const r1 = clone(r2); r1.cameras[0].name = 'Pilot view (original)';
    addRepo(mine, 3, [{ revision: 1, savedAt: new Date(now - 3 * 86400e3).toISOString(), profile: r1 }, { revision: 2, savedAt: new Date(now - 86400e3 - 5 * 3600e3).toISOString(), profile: r2 }]);
    addRepo(cinematic, 1);
    addRepo(airport, 1);
    repo.settings = { schemaVersion: 2, ui: { shortcuts: true, contrast: false, backups: true, remember: true, precision: 'normal', theme: 'dark', reducedMotion: false, miniEnabled: true, miniHover: true, miniPinned: false, miniOpacity: 0.65, nameDisplay: 'both' }, host: { allowUnqualifiedAdapter: false, lookSpeedDegPerSec: 60, moveSpeedMetersPerSec: 0.5 } };
    repo.bindings = seedBindings();

    const m = {
      bootId: 'demo-' + randomHex(8), epoch: 1, seq: 0, frameSeq: 0,
      session: 'Ready',
      aircraft: { title: C172.title, modelKey: C172.modelKey },
      ownership: { state: 'Observing', gameControlled: true, reason: null },
      forcedBlock: null, qualified: true, captureQualified: false, simVersion: '1.8.16.0', qualification: null,
      active: { profileId: null, cameraId: null, peeking: false, transitioning: false, fromDraft: false },
      live: null, look: NEUTRAL_LOOK(), peek: null, pendingCommit: null, instances: new Map(), timers: new Set(),
      working: null, selection: { profileId: null, cameraId: null },
    };
    const assembler = new Assembler();

    function openWorking(profileId, keepGlobals) {
      const entry = repo.profiles.get(profileId);
      const settings = keepGlobals && m.working ? m.working.settings : clone(repo.settings);
      const bindings = keepGlobals && m.working ? m.working.bindings : clone(repo.bindings);
      const draftRevision = m.working ? m.working.draftRevision + 1 : 1;
      m.working = { profile: clone(entry.committed), settings, bindings, draftRevision };
      const sel = entry.lastCameraId && entry.committed.cameras.some(c => c.id === entry.lastCameraId) ? entry.lastCameraId : entry.committed.defaultCameraId;
      m.selection = { profileId, cameraId: sel };
    }
    const workingEntry = () => repo.profiles.get(m.working.profile.profileId);
    const committedProfile = () => (workingEntry() ? workingEntry().committed : null);
    function dirty() {
      return { profile: !same(m.working.profile, committedProfile()), settings: !same(m.working.settings, repo.settings), bindings: !same(m.working.bindings, repo.bindings) };
    }
    function hasCommittedVersion() {
      const committed = committedProfile();
      const result = {};
      m.working.profile.cameras.forEach(c => { result[c.id] = Boolean(committed && committed.cameras.some(k => k.id === c.id)); });
      return result;
    }
    function matchFor(aircraft) {
      if (!aircraft || !aircraft.modelKey) return { matchProfileId: null, matchReason: null };
      const list = Array.from(repo.profiles.values()).map(e => e.committed).filter(p => p.aircraftMatch && p.aircraftMatch.strategy !== 'none' && p.aircraftMatch.modelKey === aircraft.modelKey);
      const preferred = list.find(p => m.working && p.profileId === m.working.profile.profileId) || list.find(p => p.aircraftMatch.confirmed) || list[0];
      if (!preferred) return { matchProfileId: null, matchReason: null };
      return { matchProfileId: preferred.profileId, matchReason: preferred.aircraftMatch.strategy === 'userConfirmedModelVariant' ? 'You linked this profile to this aircraft' : 'Matched by aircraft package' };
    }
    const aircraftPayload = () => Object.assign({ title: m.aircraft ? m.aircraft.title : null, modelKey: m.aircraft ? m.aircraft.modelKey : null }, matchFor(m.aircraft));
    function capabilities() {
      const allow = Boolean(m.working.settings.host.allowUnqualifiedAdapter);
      let blockReason = null;
      if (m.forcedBlock) blockReason = m.forcedBlock;
      else if (m.session !== 'Ready') blockReason = 'Camera Studio is not ready yet.';
      else if (!m.aircraft || !m.aircraft.modelKey) blockReason = 'Load a flight to use your cameras.';
      else if (!m.qualified && !allow && !m.qualification) blockReason = 'This simulator build has not passed the axis check yet. Run it from Settings > Simulator.';
      return { adapterQualified: m.qualified, adapterContract: 'camera-api-v1', captureQualified: m.captureQualified, allowUnqualifiedAdapter: allow, canActivate: !blockReason, blockReason, simVersion: m.simVersion, fovRangeDeg: [20, 120], levelHorizon: false, modes: clone(MODE_CAPS) };
    }
    function summaries() {
      return Array.from(repo.profiles.values()).map(e => {
        const p = e.committed;
        return { profileId: p.profileId, name: p.name, displayName: p.aircraftMatch ? p.aircraftMatch.displayName : null, modelKey: p.aircraftMatch ? p.aircraftMatch.modelKey : null, matchStrategy: p.aircraftMatch ? p.aircraftMatch.strategy : 'none', cameraCount: p.cameras.length, revision: e.revision, readOnly: false };
      });
    }
    function snapshot() {
      return {
        moduleBootId: m.bootId, sessionEpoch: m.epoch, session: m.session, aircraft: aircraftPayload(),
        ownership: clone(m.ownership), capabilities: capabilities(), activeCamera: clone(m.active), selection: clone(m.selection),
        profiles: summaries(),
        working: { profile: clone(m.working.profile), settings: clone(m.working.settings), bindings: clone(m.working.bindings), draftRevision: m.working.draftRevision, dirty: dirty(), committedRevision: workingEntry().revision, hasCommittedVersion: hasCommittedVersion() },
        pendingCommit: m.pendingCommit ? { requestId: m.pendingCommit.requestId, revisions: m.pendingCommit.revisions } : null,
        look: clone(m.look),
      };
    }

    function out(message) { encodeFrames(message, 'module:' + (++m.frameSeq)).forEach(frame => sendFrame(frame)); }
    function later(fn, ms, keep) {
      const t = setTimeout(() => { m.timers.delete(t); fn(); }, ms);
      if (!keep) m.timers.add(t);
      return t;
    }
    function event(name, payload, to, delay) {
      const message = { type: 'event', name, seq: ++m.seq, moduleBootId: m.bootId, sessionEpoch: m.epoch, payload: clone(payload) };
      if (to) message.to = to;
      if (delay) later(() => out(message), delay, true); else out(message);
    }
    function ack(env, status, reason, result) {
      out({ type: 'ack', to: env.uiInstanceId, requestId: env.requestId, status, reason: reason || null, result: result === undefined ? null : clone(result) });
    }
    const clearMotion = () => { m.timers.forEach(clearTimeout); m.timers.clear(); };
    const broadcastSnapshot = () => event('Snapshot', snapshot());
    const broadcastProfiles = () => event('ProfilesChanged', { profiles: summaries() });
    const setOwnership = (state, reason) => { m.ownership = { state, gameControlled: state !== 'Active', reason: reason || null }; event('OwnershipChanged', m.ownership); };
    const sendActive = () => event('ActiveCameraChanged', m.active);
    const workingCamera = id => m.working.profile.cameras.find(c => c.id === id);
    const categoryOf = id => m.working.profile.categories.find(c => c.cameraIds.indexOf(id) >= 0);
    const activatable = camera => Boolean(camera && MODE_CAPS[camera.mode] && MODE_CAPS[camera.mode].available);
    const internal = (env, tag) => ({ uiInstanceId: env.uiInstanceId, requestId: env.requestId + ':' + tag });
    function select(cameraId) {
      m.selection = { profileId: m.working.profile.profileId, cameraId };
      const entry = workingEntry();
      if (entry && m.working.settings.ui.remember) entry.lastCameraId = cameraId;
      event('SelectionChanged', m.selection);
    }

    function show(camera, peeking) {
      const committed = committedProfile() && committedProfile().cameras.find(c => c.id === camera.id);
      m.live = clone(camera);
      m.look = NEUTRAL_LOOK();
      m.active = { profileId: m.working.profile.profileId, cameraId: camera.id, peeking: Boolean(peeking), transitioning: camera.transition.kind !== 'cut', fromDraft: !same(committed, camera) };
      sendActive();
      const t = camera.transition;
      const ms = (t.delaySeconds || 0) * 1000 + (t.kind === 'cut' ? 0 : (t.durationSeconds || 0) * 1000);
      later(() => {
        m.active = Object.assign({}, m.active, { transitioning: false });
        sendActive();
        if (!peeking) later(() => {
          if (m.ownership.state !== 'Active' || m.active.cameraId !== camera.id) return;
          m.look = { active: true, positionMetersFRD: [0.02, 0, 0], pbhDeg: [-3, 0, 6], fovDeltaRad: 0 };
          event('TemporaryLookChanged', m.look);
        }, 2500);
      }, ms);
    }
    function activate(env, cameraId, options) {
      const opts = options || {};
      const camera = workingCamera(cameraId);
      const caps = capabilities();
      if (!caps.canActivate) return ack(env, 'Rejected', caps.blockReason);
      if (!camera) return ack(env, 'Rejected', 'unknownCamera');
      if (!activatable(camera)) return ack(env, 'Rejected', MODE_CAPS[camera.mode] ? MODE_CAPS[camera.mode].reason : 'modeNotSupported');
      ack(env, 'Accepted');
      clearMotion();
      if (!opts.peeking) { m.peek = null; if (m.selection.cameraId !== cameraId) select(cameraId); }
      if (m.ownership.state !== 'Active') {
        setOwnership('Acquiring');
        later(() => { setOwnership('Active'); show(camera, opts.peeking); }, 450);
      } else show(camera, opts.peeking);
    }
    function release(env, reason) {
      if (env) ack(env, 'Accepted');
      clearMotion();
      m.peek = null;
      if (m.ownership.state === 'Observing') return;
      setOwnership('Releasing');
      later(() => {
        setOwnership('Observing', reason || 'userReleased');
        m.active = { profileId: m.working.profile.profileId, cameraId: null, peeking: false, transitioning: false, fromDraft: false };
        sendActive();
        m.look = NEUTRAL_LOOK();
        event('TemporaryLookChanged', m.look);
      }, 260);
    }
    function cycle(env, delta) {
      const current = m.active.cameraId || m.selection.cameraId || m.working.profile.defaultCameraId;
      const category = categoryOf(current);
      if (!category) return ack(env, 'Rejected', 'noActiveCamera');
      const ids = category.cameraIds.filter(id => activatable(workingCamera(id)));
      if (!ids.length) return ack(env, 'Rejected', NOT_AVAILABLE);
      const index = ids.indexOf(current);
      return activate(env, ids[index < 0 ? 0 : (index + delta + ids.length) % ids.length]);
    }
    function nextCategory(env) {
      const cats = m.working.profile.categories;
      const current = categoryOf(m.active.cameraId || m.selection.cameraId) || cats[0];
      for (let step = 1; step <= cats.length; step++) {
        const next = cats[(cats.indexOf(current) + step) % cats.length];
        const id = next.cameraIds.find(cid => activatable(workingCamera(cid)));
        if (id) return activate(env, id);
      }
      return ack(env, 'Rejected', NOT_AVAILABLE);
    }
    function composedPose() {
      const camera = m.live, look = m.look;
      const q = M.quatNormalize(M.quatMultiply(camera.orientationCameraToAnchorXYZW, M.quatFromHpb(look.pbhDeg[2], look.pbhDeg[0], look.pbhDeg[1])));
      const offset = M.rotateVector(camera.orientationCameraToAnchorXYZW, look.positionMetersFRD);
      return { positionMetersFRD: camera.positionMetersFRD.map((v, i) => round(v + offset[i], 5)), orientationCameraToAnchorXYZW: q.map(v => round(v, 7)), fovRad: round(camera.projection.value + look.fovDeltaRad, 5) };
    }
    const draftChanged = (ops, origin) => event('DraftChanged', { draftRevision: m.working.draftRevision, dirty: dirty(), ops, origin: origin || null });
    function testBinding(env, binding) {
      const action = binding && binding.action;
      const caps = capabilities();
      const camera = binding && binding.viewId ? m.working.profile.cameras.find(c => c.viewId === binding.viewId) : null;
      let recalled = false, message = 'Command sent.';
      const blocked = text => 'Command sent, but the view could not be recalled: ' + text;
      switch (action) {
        case 'selectView': case 'peekView':
          if (!camera) message = 'Command sent, but no camera uses view ID ' + binding.viewId + '.';
          else if (!caps.canActivate) message = blocked(caps.blockReason);
          else if (!activatable(camera)) message = blocked('"' + camera.name + '" cannot be activated in this version.');
          else { recalled = true; message = 'Recalled ' + camera.name + '.'; activate(internal(env, 'dispatch'), camera.id); }
          break;
        case 'releaseCamera': release(null); message = 'Camera released.'; break;
        case 'nextCamera': case 'previousCamera': case 'returnToPilot': case 'nextCategory':
          if (!caps.canActivate) { message = blocked(caps.blockReason); break; }
          recalled = true;
          if (action === 'returnToPilot') { activate(internal(env, 'dispatch'), m.working.profile.defaultCameraId); message = 'Returned to the default view.'; }
          else if (action === 'nextCategory') { nextCategory(internal(env, 'dispatch')); message = 'Moved to the next category.'; }
          else { cycle(internal(env, 'dispatch'), action === 'nextCamera' ? 1 : -1); message = 'Switched camera.'; }
          break;
        case 'toggleMouseLook': event('SurfaceRequest', {target:'mini', op:'mouseLook'}); break;
        case 'toggleEditor': case 'toggleMini':
          event('SurfaceRequest', { target: action === 'toggleEditor' ? 'main' : 'mini', op: 'toggle' });
          break;
        case 'recenterLook': m.look = NEUTRAL_LOOK(); event('TemporaryLookChanged', m.look); break;
        default: message = m.ownership.state === 'Active' ? 'Command sent - the camera moved briefly.' : 'Command sent. Take camera control to see it move.';
      }
      event('BindingTriggered', { action, viewId: (binding && binding.viewId) || null, result: recalled ? 'recalled' : 'dispatched' });
      ack(env, 'Completed', null, { dispatched: true, recalled, message });
    }

    function handle(env) {
      const action = env.action, payload = env.payload || {};
      if (env.protocolVersion !== 2) return ack(env, 'Rejected', 'protocolVersion');
      if (['Hello', 'RequestSnapshot', 'ReleaseCamera'].indexOf(action) < 0 && env.moduleBootId !== m.bootId) return ack(env, 'Rejected', 'staleBoot');
      if (MOTION_ACTIONS.has(action) && env.sessionEpoch !== m.epoch) return ack(env, 'Rejected', 'staleEpoch');
      switch (action) {
        case 'Hello':
          m.instances.set(env.uiInstanceId, { surface: payload.surface, lease: Date.now() });
          ack(env, 'Completed');
          return event('Snapshot', snapshot(), env.uiInstanceId);
        case 'RenewLease': { const i = m.instances.get(env.uiInstanceId); if (i) i.lease = Date.now(); return ack(env, 'Completed'); }
        case 'RequestSnapshot': ack(env, 'Completed'); return event('Snapshot', snapshot(), env.uiInstanceId);
        case 'SelectCamera':
          if (!workingCamera(payload.cameraId)) return ack(env, 'Rejected', 'unknownCamera');
          select(payload.cameraId);
          return ack(env, 'Completed');
        case 'ActivateCamera': return activate(env, payload.cameraId);
        case 'EnableCamera': return activate(env, m.selection.cameraId || m.working.profile.defaultCameraId);
        case 'ReleaseCamera': return release(env);
        case 'BeginPeek':
          m.peek = { token: payload.peekToken, before: { cameraId: m.active.cameraId, state: m.ownership.state } };
          return activate(env, payload.cameraId, { peeking: true });
        case 'EndPeek': {
          if (!m.peek || m.peek.token !== payload.peekToken) return ack(env, 'Cancelled', 'notCurrentPeek');
          const before = m.peek.before;
          m.peek = null;
          if (before.state === 'Active' && before.cameraId) return activate(env, before.cameraId);
          return release(env, 'peekEnded');
        }
        case 'CycleCamera': return cycle(env, payload.delta >= 0 ? 1 : -1);
        case 'NextCategory': return nextCategory(env);
        case 'ReturnToPilot': return activate(env, m.working.profile.defaultCameraId);
        case 'StartWalkaround': event('WalkaroundChanged',Object.assign({running:true},payload)); return ack(env,'Accepted');
        case 'StopWalkaround': event('WalkaroundChanged',{running:false,categoryId:null,intervalSeconds:5,loop:true}); return ack(env,'Completed');
        case 'RecenterLook': m.look = NEUTRAL_LOOK(); event('TemporaryLookChanged', m.look); return ack(env, 'Completed');
        case 'MouseLookDelta': {
          if (m.ownership.state !== 'Active') return ack(env, 'Rejected', 'notActive');
          m.look.pbhDeg[2] = M.wrapDeg(m.look.pbhDeg[2] + (payload.headingDeg || 0));
          m.look.pbhDeg[0] = M.clamp(m.look.pbhDeg[0] + (payload.pitchDeg || 0), -89, 89);
          m.look.active = true;
          event('TemporaryLookChanged', m.look); return ack(env, 'Completed');
        }
        case 'BeginGesture': case 'RenewGesture': case 'EndGesture': return ack(env, 'Completed');
        case 'UpdateDraft': {
          if (env.draftRevision !== m.working.draftRevision) return ack(env, 'Rejected', 'staleDraft');
          const ops = payload.ops || [];
          let next;
          try { next = applyOps(m.working, ops); } catch (error) { return ack(env, 'Rejected', error.reason || 'invalidOps'); }
          m.working.profile = next.profile; m.working.settings = next.settings; m.working.bindings = next.bindings;
          m.working.draftRevision++;
          ack(env, 'Completed', null, { draftRevision: m.working.draftRevision });
          draftChanged(ops, env.uiInstanceId);
          if (ops.some(op => op.op === 'setSettings')) {
            // Pinning is a global presentation preference: persisted at once, never a pending change.
            repo.settings.ui.miniPinned = Boolean(m.working.settings.ui.miniPinned);
            event('CapabilitiesChanged', capabilities());
          }
          const edited = ops.find(op => op.op === 'setCamera' && op.camera.id === m.active.cameraId);
          if (edited) {
            m.live = clone(edited.camera);
            if (m.ownership.state === 'Active' && !m.active.fromDraft) { m.active = Object.assign({}, m.active, { fromDraft: true }); sendActive(); }
          }
          return undefined;
        }
        case 'RevertChanges': {
          if (m.pendingCommit) return ack(env, 'Rejected', 'commitPending');
          const committed = committedProfile();
          if (m.active.cameraId && !committed.cameras.some(c => c.id === m.active.cameraId)) release(null, 'activeViewReverted');
          m.working.profile = clone(committed); m.working.settings = clone(repo.settings); m.working.bindings = clone(repo.bindings);
          m.working.draftRevision++;
          if (!workingCamera(m.selection.cameraId)) m.selection = { profileId: committed.profileId, cameraId: committed.defaultCameraId };
          ack(env, 'Completed');
          event('CapabilitiesChanged', capabilities());
          return broadcastSnapshot();
        }
        case 'CommitChanges': {
          if (m.pendingCommit) {
            if (m.pendingCommit.requestId === env.requestId) return ack(env, 'Pending', null, { revisions: m.pendingCommit.revisions });
            return ack(env, 'Rejected', 'commitPending');
          }
          const captured = { profile: clone(m.working.profile), settings: clone(m.working.settings), bindings: clone(m.working.bindings) };
          const wasDirty = dirty();
          const revisions = { profile: m.working.draftRevision, settings: m.working.draftRevision, bindings: m.working.draftRevision };
          m.pendingCommit = { requestId: env.requestId, revisions };
          ack(env, 'Pending', null, { revisions });
          later(() => {
            const problem = wasDirty.profile ? validateProfile(captured.profile) : null;
            const requestId = m.pendingCommit.requestId;
            m.pendingCommit = null;
            if (problem) { event('CommitCompleted', { requestId, status: 'Failed', revisions, message: problem }); return; }
            const entry = workingEntry();
            if (wasDirty.profile) {
              if (payload.backup) { entry.history.push({ revision: entry.revision, savedAt: new Date().toISOString(), profile: clone(entry.committed) }); while (entry.history.length > 5) entry.history.shift(); }
              entry.committed = captured.profile;
              entry.revision++;
            }
            repo.settings = captured.settings;
            repo.bindings = captured.bindings;
            event('CommitCompleted', { requestId, status: 'Committed', revisions, message: null });
            draftChanged([], null);
            broadcastProfiles();
          }, 300, true);
          return undefined;
        }
        case 'OpenProfile': {
          if (!repo.profiles.has(payload.profileId)) return ack(env, 'Rejected', 'unknownProfile');
          if (m.pendingCommit) return ack(env, 'Rejected', 'commitPending');
          if (dirty().profile && !payload.discard) return ack(env, 'Rejected', 'unsavedChanges');
          if (m.ownership.state === 'Active' || m.ownership.state === 'Acquiring') release(null, 'profileChanged');
          openWorking(payload.profileId, true); // global settings/bindings edits stay pending across the switch
          ack(env, 'Completed');
          broadcastSnapshot();
          return event('AircraftChanged', aircraftPayload());
        }
        case 'CreateProfile': {
          let base;
          if (payload.copyOf && payload.copyOf === m.working.profile.profileId) base = clone(m.working.profile);
          else if (payload.copyOf && repo.profiles.has(payload.copyOf)) base = clone(repo.profiles.get(payload.copyOf).committed);
          else base = makeProfile({ name: 'New', defaultName: 'Pilot view', aircraftMatch: { strategy: 'none', modelKey: null, variantKey: null, displayName: null, confirmed: false }, categories: [{ name: 'Cockpit', cameras: [['Pilot view', 1, { x: 0, y: 0, z: 0 }, { favorite: true, temporaryLookPolicy: 'resetOnRecall' }]] }] });
          remapIds(base);
          base.name = String(payload.name || 'New profile').slice(0, 60);
          if (payload.forAircraft && m.aircraft && m.aircraft.modelKey) base.aircraftMatch = { strategy: 'aircraftPath', modelKey: m.aircraft.modelKey, variantKey: null, displayName: m.aircraft.title, confirmed: false };
          const problem = validateProfile(base);
          if (problem) return ack(env, 'Rejected', problem);
          if (m.ownership.state === 'Active' || m.ownership.state === 'Acquiring') release(null, 'profileChanged');
          addRepo(base, 1);
          openWorking(base.profileId, true);
          ack(env, 'Completed', null, { profileId: base.profileId });
          broadcastProfiles();
          broadcastSnapshot();
          return event('AircraftChanged', aircraftPayload());
        }
        case 'DeleteProfile':
          if (payload.profileId === m.working.profile.profileId) return ack(env, 'Rejected', 'profileInUse');
          if (!repo.profiles.delete(payload.profileId)) return ack(env, 'Rejected', 'unknownProfile');
          ack(env, 'Completed');
          broadcastProfiles();
          return event('AircraftChanged', aircraftPayload());
        case 'BindProfileToAircraft': {
          const entry = repo.profiles.get(payload.profileId);
          if (!entry) return ack(env, 'Rejected', 'unknownProfile');
          if (!m.aircraft || !m.aircraft.modelKey) return ack(env, 'Rejected', 'noAircraft');
          const match = { strategy: 'userConfirmedModelVariant', modelKey: m.aircraft.modelKey, variantKey: null, displayName: m.aircraft.title, confirmed: true };
          entry.committed.aircraftMatch = match;
          entry.revision++;
          ack(env, 'Completed');
          if (payload.profileId === m.working.profile.profileId) {
            m.working.profile.aircraftMatch = clone(match);
            m.working.draftRevision++;
            draftChanged([{ op: 'setProfileFields', aircraftMatch: match }], null);
          }
          broadcastProfiles();
          return event('AircraftChanged', aircraftPayload());
        }
        case 'ListBackups':
          return ack(env, 'Completed', null, { backups: workingEntry().history.map(h => ({ revision: h.revision, savedAt: h.savedAt })).reverse() });
        case 'LoadBackup': {
          const entry = workingEntry();
          const backup = entry.history.find(h => h.revision === payload.revision);
          if (!backup) return ack(env, 'Rejected', 'unknownRevision');
          const profile = clone(backup.profile);
          profile.profileId = entry.committed.profileId;
          m.working.profile = profile;
          m.working.draftRevision++;
          if (!workingCamera(m.selection.cameraId)) m.selection = { profileId: profile.profileId, cameraId: profile.defaultCameraId };
          ack(env, 'Completed');
          return broadcastSnapshot();
        }
        case 'ExportProfile': {
          const entry = repo.profiles.get(payload.profileId);
          if (!entry) return ack(env, 'Rejected', 'unknownProfile');
          const useWorking = Boolean(payload.working) && payload.profileId === m.working.profile.profileId;
          return ack(env, 'Completed', null, { code: JSON.stringify(useWorking ? m.working.profile : entry.committed, null, 2), label: useWorking ? 'working copy' : 'saved' });
        }
        case 'ImportProfile': {
          let parsed;
          try { parsed = JSON.parse(String(payload.code || '')); } catch (error) { return ack(env, 'Rejected', 'This profile code is not valid JSON.'); }
          const candidate = migrate(parsed && parsed.profile && parsed.profile.schemaVersion ? parsed.profile : parsed);
          const problem = validateProfile(candidate);
          if (problem) return ack(env, 'Rejected', problem);
          const imported = remapIds(candidate);
          const unsupported = [];
          imported.cameras.forEach(c => { if (c.effects && c.effects.enabled) { c.effects = { enabled: false }; unsupported.push('Camera effects on "' + c.name + '" were turned off - effects are not supported yet.'); } });
          imported.cameras.forEach(c => { if (MODE_CAPS[c.mode] && !MODE_CAPS[c.mode].available) unsupported.push('"' + c.name + '" is kept but cannot be activated in this version.'); });
          if (imported.sequences && imported.sequences.length) unsupported.push(imported.sequences.length + ' camera sequence(s) are kept for the cinematic extension.');
          addRepo(imported, 1);
          ack(env, 'Completed', null, { profileId: imported.profileId, report: {
            name: imported.name,
            aircraftMatch: { matches: Boolean(m.aircraft && imported.aircraftMatch && imported.aircraftMatch.modelKey === m.aircraft.modelKey), displayName: imported.aircraftMatch ? imported.aircraftMatch.displayName : null },
            cameraCount: imported.cameras.length, unsupported,
          } });
          return broadcastProfiles();
        }
        case 'CaptureView': case 'SeedFromDefinition': {
          ack(env, 'Accepted');
          const pose = m.ownership.state === 'Active' && m.live ? composedPose() : { positionMetersFRD: [0.012, -0.004, 0.006], orientationCameraToAnchorXYZW: M.quatFromHpb(1.5, -4, 0).map(v => round(v, 7)), fovRad: round(M.degToRad(64), 5) };
          return event('ViewCaptured', Object.assign({ ok: true }, pose, { frame: FRAME_CONTRACT, qualified: m.captureQualified, source: action === 'SeedFromDefinition' ? 'definition:' + payload.name : (m.ownership.state === 'Active' ? 'addOnCamera' : 'simulatorView'), message: m.captureQualified ? null : 'View capture is not qualified for this simulator build yet. Check the values before you use them.' }), env.uiInstanceId, 150);
        }
        case 'EnumerateDefinitions': ack(env, 'Completed'); return event('DefinitionsListed', { names: ['Pilot', 'Copilot', 'Instrument_01', 'Instrument_02'] }, env.uiInstanceId);
        case 'BakeTemporaryLook': {
          if (m.ownership.state !== 'Active' || !m.live) return ack(env, 'Rejected', 'notActive');
          const camera = workingCamera(m.live.id);
          if (!camera) return ack(env, 'Rejected', 'unknownCamera');
          const pose = composedPose();
          const next = Object.assign(clone(camera), { positionMetersFRD: pose.positionMetersFRD, orientationCameraToAnchorXYZW: pose.orientationCameraToAnchorXYZW, projection: Object.assign({}, camera.projection, { value: pose.fovRad }) });
          const ops = [{ op: 'setCamera', camera: next }];
          m.working.profile = applyOps(m.working, ops).profile;
          m.working.draftRevision++;
          m.live = clone(next);
          m.look = NEUTRAL_LOOK();
          ack(env, 'Completed');
          draftChanged(ops, null);
          return event('TemporaryLookChanged', m.look);
        }
        case 'ListControllers':
          ack(env, 'Completed');
          return event('ControllersListed', { controllers: [{ deviceId: 1, name: 'T.16000M' }, { deviceId: 2, name: 'TWCS Throttle' }] }, env.uiInstanceId);
        case 'TestInput': {
          ack(env, 'Accepted');
          const definition = payload.input && payload.input.definition;
          event('InputTest', { definition, phase: 'down' }, env.uiInstanceId, 800);
          return event('InputTest', { definition, phase: 'up' }, env.uiInstanceId, 1150);
        }
        case 'TestBinding': return testBinding(env, payload.binding);
        case 'RelaySurface': ack(env, 'Completed'); return event('SurfaceRequest', { target: payload.target, op: payload.op });
        case 'TextFocus': return ack(env, 'Completed');
        case 'QualificationStart':
          // Live control is allowed on an unqualified build while the axis check runs.
          m.qualification = { started: Date.now() };
          ack(env, 'Completed', null, { simVersion: m.simVersion, contract: 'camera-api-v1', axes: ['forward', 'right', 'up', 'pitch', 'heading', 'bank'] });
          return event('CapabilitiesChanged', capabilities());
        case 'QualificationProbe': {
          if (!m.qualification) return ack(env, 'Rejected', 'qualificationNotStarted');
          if (m.ownership.state !== 'Active') return ack(env, 'Rejected', 'notActive');
          const expect = {
            forward: 'The view moves about 30 cm forward, toward the instrument panel.',
            right: 'The view moves about 30 cm to the right.',
            up: 'The view moves about 30 cm up, toward the roof.',
            pitch: 'The view tilts about 15 deg up.',
            heading: 'The view turns about 15 deg to the right.',
            bank: 'The view rolls about 15 deg to the right (the right side goes down).',
          }[payload.axis];
          if (!expect) return ack(env, 'Rejected', 'unknownAxis');
          return ack(env, 'Completed', null, { axis: payload.axis, expect });
        }
        case 'QualificationRestore': return ack(env, m.qualification ? 'Completed' : 'Rejected', m.qualification ? null : 'qualificationNotStarted');
        case 'QualificationRecord': {
          if (!m.qualification) return ack(env, 'Rejected', 'qualificationNotStarted');
          const answers = payload.answers || {};
          const g01 = ['forward', 'right', 'up', 'pitch', 'heading', 'bank'].every(axis => answers[axis] === 'correct' || answers[axis] === 'reversed');
          const g03 = payload.recall === 'pass';
          ack(env, 'Pending', null, { revisions: null });
          return later(() => {
            m.qualified = g01 && g03;
            event('CommitCompleted', { requestId: env.requestId, kind: 'qualification', status: 'Committed', revisions: null, message: null, g01, g03 });
            event('CapabilitiesChanged', capabilities());
          }, 300, true);
        }
        case 'QualificationEnd': {
          m.qualification = null;
          ack(env, 'Completed');
          const caps = capabilities();
          if (!caps.canActivate && (m.ownership.state === 'Active' || m.ownership.state === 'Acquiring')) release(null, 'qualificationEnded');
          return event('CapabilitiesChanged', caps);
        }
        case 'GetDiagnostics':
          return ack(env, 'Completed', null, { text: ['FS Camera Studio demo module', 'moduleBootId: ' + m.bootId, 'sessionEpoch: ' + m.epoch, 'session: ' + m.session, 'ownership: ' + m.ownership.state, 'working profile: ' + m.working.profile.name + ' (draft ' + m.working.draftRevision + ')', 'instances: ' + m.instances.size].join('\n') });
        default: return ack(env, 'Rejected', 'unknownAction');
      }
    }

    openWorking(mine.profileId, false);

    const controls = {
      setCanActivate(on, reason) { m.forcedBlock = on ? null : (reason || 'Camera control is turned off for testing.'); event('CapabilitiesChanged', capabilities()); },
      setQualified(on) { m.qualified = Boolean(on); event('CapabilitiesChanged', capabilities()); },
      loadAircraft(title, modelKey) {
        m.aircraft = (title || modelKey) ? { title: title || null, modelKey: modelKey || null } : null;
        if (m.ownership.state !== 'Observing') release(null, 'aircraftChanged');
        const match = matchFor(m.aircraft);
        if (match.matchProfileId && match.matchProfileId !== m.working.profile.profileId && !dirty().profile) {
          openWorking(match.matchProfileId, true);
          broadcastSnapshot();
        }
        event('AircraftChanged', aircraftPayload());
        event('CapabilitiesChanged', capabilities());
      },
      setSession(session) { m.session = session; if (session !== 'Ready') release(null, 'session'); event('SessionChanged', { moduleBootId: m.bootId, sessionEpoch: m.epoch, session, reason: null }); event('CapabilitiesChanged', capabilities()); },
      newEpoch() { m.epoch++; event('SessionChanged', { moduleBootId: m.bootId, sessionEpoch: m.epoch, session: m.session, reason: 'flightRestarted' }); },
      reboot() { m.bootId = 'demo-' + randomHex(8); },
      setOwnership(state, reason) { setOwnership(state, reason); },
      raiseFault(code, message) { clearMotion(); setOwnership('Faulted', code || 'adapterError'); event('FaultRaised', { code: code || 'adapterError', message: message || 'The simulator rejected a camera update.' }); },
      get state() { return clone({ session: m.session, ownership: m.ownership, active: m.active, selection: m.selection, draftRevision: m.working.draftRevision, dirty: dirty(), instances: Array.from(m.instances.keys()) }); },
    };

    return {
      controls,
      receiveFrame(frame) {
        const env = assembler.push(frame);
        if (env) handle(env);
      },
    };
  }

  root.FSCBridge = { create, encodeFrames, Assembler };
})(typeof globalThis !== 'undefined' ? globalThis : window);
