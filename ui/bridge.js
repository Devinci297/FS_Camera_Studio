/*
 * FS Camera Studio - panel <-> camera module bridge (doc/protocol.md v2 sec. 3-sec. 6).
 *
 * The UI runs as MSFS 2024 in-game toolbar panels (Coherent GT). Its only peer is the Camera Studio WASM module:
 *   panel > module   listener.callWasm("FSCS_UI_TO_HOST", jsonText)
 *   module > panel   listener.on("FSCS_HOST_TO_UI", jsonText)
 * Messages longer than 4000 characters travel as { part, of, msgId, chunk } frames in both directions.
 *
 * X-Plane (XP Camera Studio): the page runs in an X-Plane 12.4.4 browser window opened with ?host=xplane. Frames
 * go out through the plugin's browser function xplane.fscs(text) and arrive as FSCXPlaneReceive(text) calls.
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
    constructor() { this.listener = null; this.handlers = []; this.isOpen = false; this.closing = false; }
    static available() { return typeof root.RegisterCommBusListener === 'function'; }
    start(onOpen) {
      // A view being detached must not call the native CommBus (see app.js blur/pagehide note).
      const close = () => { this.closing = true; this.isOpen = false; };
      if (typeof root.addEventListener === 'function') ['pagehide', 'beforeunload', 'unload'].forEach(name => root.addEventListener(name, close, true));
      try {
        this.listener = root.RegisterCommBusListener(() => {
          if (this.closing) return;
          this.isOpen = true;
          if (onOpen) onOpen();
        });
        this.listener.on('FSCS_HOST_TO_UI', data => { const frame = typeof data === 'string' ? data : clone(data); setTimeout(() => this.handlers.forEach(cb => cb(frame)), 0); });
      } catch (error) {
        console.error('CommBus listener could not be created', error);
      }
    }
    send(frame) {
      if (!this.listener || !this.isOpen || this.closing) return false;
      try { this.listener.callWasm('FSCS_UI_TO_HOST', frame); return true; } catch (error) { console.warn('CommBus send failed', error); return false; }
    }
    onMessage(cb) { this.handlers.push(cb); }
  }

  /** XP Camera Studio: X-Plane 12.4.4 browser windows. The plugin registers xplane.fscs / xplane.fscsPanel on each
   *  window and delivers module frames by injecting FSCXPlaneReceive(frame). The xplane namespace may appear after
   *  this script runs, so it is looked up on every send; Hello simply retries until it does. */
  class XPlaneTransport {
    constructor() { this.handlers = []; this.closing = false; }
    static available() { return /[?&]host=xplane\b/.test((root.location && root.location.search) || ''); }
    start(onOpen) {
      const close = () => { this.closing = true; };
      if (typeof root.addEventListener === 'function') ['pagehide', 'beforeunload', 'unload'].forEach(name => root.addEventListener(name, close, true));
      root.FSCXPlaneReceive = frame => { if (!this.closing && typeof frame === 'string') setTimeout(() => this.handlers.forEach(cb => cb(frame)), 0); };
      setTimeout(onOpen, 0);
    }
    send(frame) {
      const api = root.xplane;
      if (this.closing || !api || typeof api.fscs !== 'function') return false;
      try {
        const reply = api.fscs(frame);
        if (reply && typeof reply.catch === 'function') reply.catch(error => console.warn('X-Plane send failed', error));
        return true;
      } catch (error) { console.warn('X-Plane send failed', error); return false; }
    }
    onMessage(cb) { this.handlers.push(cb); }
  }
  // The editor's open/close buttons (app.js) call fscsSetPanelOpen; in X-Plane the plugin owns the windows.
  if (XPlaneTransport.available()) {
    root.fscsSetPanelOpen = (surface, open) => {
      const api = root.xplane;
      if (!api || typeof api.fscsPanel !== 'function') return false;
      try { api.fscsPanel({ surface, open: Boolean(open) }); return true; } catch (error) { return false; }
    };
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
    const transport = XPlaneTransport.available() ? new XPlaneTransport() : CommBusTransport.available() ? new CommBusTransport() : new DemoTransport();
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
      mode: transport instanceof XPlaneTransport ? 'xplane' : transport instanceof CommBusTransport ? 'commbus' : 'demo',
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
  const MODE_FROM_TYPE = { cockpit: 'aircraftCockpit', external: 'aircraftExterior', orbit: 'orbit', flyby: 'flyby', airport: 'airport', free: 'free', mounted: 'mounted' };
  const NOT_AVAILABLE = 'Not available in this version - saved with the profile but cannot be activated.';
  const MODE_CAPS = {
    aircraftCockpit: { available: true, reason: null },
    aircraftExterior: { available: true, reason: 'Preview - exterior views keep cockpit sound in the current simulator build.' },
    orbit: { available: true, reason: null }, flyby: { available: true, reason: null },
    mounted: { available: true, reason: 'Preview - mounted views keep cockpit sound and need live checking of the gimbal and vibration.' },
    airport: { available: false, reason: NOT_AVAILABLE },
    free: { available: true, reason: 'Preview - free cameras need live checking of world placement and terrain clearance.' },
  };
  // Free cameras (drones) keep a geodetic world origin, as the module validates it.
  const validOrigin = o => Boolean(o && [o.lat, o.lon, o.altitude].every(Number.isFinite) && Math.abs(o.lat) < 89.9 && Math.abs(o.lon) <= 180 && o.altitude > -1500 && o.altitude < 30000);
  const DEMO_AIRCRAFT_ORIGIN = { lat: 47.4582, lon: 8.5481, altitude: 432 };  // the demo aircraft, heading north and level
  const MOTION_ACTIONS = new Set(['ActivateCamera', 'EnableCamera', 'BeginPeek', 'CycleCamera', 'NextCategory', 'ReturnToPilot', 'CaptureView', 'SeedFromDefinition', 'BakeTemporaryLook']);
  const NEUTRAL_LOOK = () => ({ active: false, positionMetersFRD: [0, 0, 0], pbhDeg: [0, 0, 0], fovDeltaRad: 0 });
  const round = (value, digits) => { const r = Number(value.toFixed(digits)); return Object.is(r, -0) ? 0 : r; };

  // Smart views: mirrors defaultGlance() in the module. A chosen icon decides; otherwise the name, as the editor's
  // automatic icon does. Only cockpit views can be glance views.
  function defaultGlance(c) {
    if ((c.mode || 'aircraftCockpit') !== 'aircraftCockpit') return false;
    if (c.icon) return c.icon === 'view-instrument';
    const name = String(c.name || '').toLowerCase();
    if (/runway|threshold|approach|gate|jetway|apron|parking|ramp|taxi|tower|airport/.test(name)) return false;
    return /pfd|mfd|instrument|g1000|radio|avionics|autopilot/.test(name);
  }
  // Cinematic mode: mirrors defaultCinematic() in the module. Outside views take part; of the cockpit views only the
  // profile's default view. Free cameras never do.
  function defaultCinematic(c, defaultCameraId) {
    const mode = c.mode || 'aircraftCockpit';
    if (mode === 'free') return false;
    return mode !== 'aircraftCockpit' || (Boolean(defaultCameraId) && c.id === defaultCameraId);
  }
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
      temporaryLookPolicy: x.temporaryLookPolicy || 'retain', followHorizon: 1, seedDefinitionName: null,
      glance: defaultGlance({ mode, name }),
    };
  }
  function calibration() {
    return { frameContract: FRAME_CONTRACT, anchor: { kind: 'sdkPilotEyepoint', seedDefinitionName: null, correctionMetersFRD: [0, 0, 0], correctionRotationXYZW: [0, 0, 0, 1] }, adapterContract: 'pending-qualification', simulatorBuild: null };
  }
  function makeProfile(spec) {
    const profile = { schemaVersion: 4, profileId: M.uuid(), name: spec.name, aircraftMatch: spec.aircraftMatch, calibration: calibration(), requiredSchemaFeatures: [], defaultCameraId: null, categories: [], cameras: [], sequences: [] };
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
    profile.cameras.forEach(c => { c.cinematic = defaultCinematic(c, profile.defaultCameraId); });
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
    list.push({ action: 'panicRelease', input: ctrlAlt('X', 'X') });
    list.push({ action: 'binoculars', input: ctrlAlt('Z', 'Z') });
    list.push({ action: 'toggleEditor', input: ctrlAlt('E', 'E') });
    list.push({ action: 'toggleMini', input: ctrlAlt('M', 'M') });
    list.push({ action: 'recenterLook', input: ctrlAlt('C', 'C') });
    list.push({ action: 'lookLeft', input: ctrlAlt('VK_LEFT', 'Left arrow') });
    list.push({ action: 'lookRight', input: ctrlAlt('VK_RIGHT', 'Right arrow') });
    list.push({ action: 'lookUp', input: ctrlAlt('VK_UP', 'Up arrow') });
    list.push({ action: 'lookDown', input: ctrlAlt('VK_DOWN', 'Down arrow') });
    list.push({ action: 'glanceHold', input: ctrlAlt('G', 'G') });
    list.push({ action: 'glanceNext', input: ctrlAlt('I', 'I') });
    list.push({ action: 'glanceReturn', input: ctrlAlt('U', 'U') });
    list.push({ action: 'toggleCinematic', input: ctrlAlt('K', 'K') });
    list.push({ action: 'spotNext', input: ctrlAlt('T', 'T') });
    list.push({ action: 'spotPrevious', input: ctrlAlt('Y', 'Y') });
    list.push({ action: 'spotReturn', input: ctrlAlt('O', 'O') });
    list.push({ action: 'toggleAutoSpot', input: ctrlAlt('J', 'J') });
    [['moveForward','W'],['moveBack','S'],['moveLeft','A'],['moveRight','D'],['moveUp','Q'],['moveDown','E'],['moveFast','VK_SHIFT']].forEach(entry => list.push({action:entry[0],input:{kind:'key',definition:entry[1],label:entry[1]==='VK_SHIFT'?'Shift':entry[1]}}));
    [['zoomIn','sim:ZOOM_IN','Mouse wheel up / MSFS zoom in'],['zoomOut','sim:ZOOM_OUT','Mouse wheel down / MSFS zoom out']].forEach(entry => list.push({action:entry[0],input:{kind:'simZoom',definition:entry[1],label:entry[2]}}));
    list.push({action:'saveViews',input:{kind:'key',definition:'VK_LCONTROL+S',label:'Ctrl + S'}});
    return { schemaVersion: 2, movementDefaultsVersion: 2, saveViewsDefaultsVersion:1, bindings: list };
  }

  function validateProfile(p) {
    if (!p || p.schemaVersion !== 4) return 'This profile uses an unknown format.';
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
      if (c.followHorizon !== undefined && !(typeof c.followHorizon === 'number' && c.followHorizon >= 0 && c.followHorizon <= 1)) return '"' + c.name + '" needs a follow horizon from 0 to 1.';
      if (c.gimbal !== undefined && ['locked', 'horizon', 'yaw'].indexOf(c.gimbal) < 0) return '"' + c.name + '" needs a gimbal of locked, horizon or yaw.';
      if (c.mountVibration !== undefined && !(typeof c.mountVibration === 'number' && c.mountVibration >= 0 && c.mountVibration <= 1)) return '"' + c.name + '" needs an airframe vibration from 0 to 1.';
      if (c.glance !== undefined && typeof c.glance !== 'boolean') return '"' + c.name + '" needs Glance view on or off.';
      if (c.mode === 'free' && c.worldOrigin !== undefined && !validOrigin(c.worldOrigin)) return '"' + c.name + '" needs a valid world origin.';
      if (c.followAircraft !== undefined && typeof c.followAircraft !== 'boolean') return '"' + c.name + '" needs Follow aircraft on or off.';
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
  /** Schema 1 > 2 > 3 > 4 migration (sec. 2.1). */
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
      });
      if (p.aircraftMatch && p.aircraftMatch.confirmed === undefined) p.aircraftMatch.confirmed = p.aircraftMatch.strategy === 'userConfirmedModelVariant';
    }
    // Schema 2 > 3: levelHorizon true became followHorizon 0; false or absent banks with the aircraft (1).
    if (p && p.schemaVersion === 2) {
      p.schemaVersion = 3;
      (p.cameras || []).forEach(c => {
        if (c.followHorizon === undefined) c.followHorizon = c.levelHorizon === true ? 0 : 1;
        delete c.levelHorizon;
      });
    }
    // Schema 3 > 4: no field changes; 4 adds the mounted camera mode, which schema 3 readers reject.
    if (p && p.schemaVersion === 3) p.schemaVersion = 4;
    (p && p.cameras || []).forEach(c => {
      if (c && c.mode === 'mounted') { if (c.gimbal === undefined) c.gimbal = 'locked'; if (c.mountVibration === undefined) c.mountVibration = 0.3; }
      if (c && c.mode === 'free' && c.followAircraft === undefined) c.followAircraft = false;
    });
    // Optional glance flag (no schema bump): derived from the icon or name when absent.
    if (p && Array.isArray(p.cameras)) p.cameras.forEach(c => { if (c && c.glance === undefined) c.glance = defaultGlance(c); });
    if (p && Array.isArray(p.cameras)) p.cameras.forEach(c => { if (c && c.cinematic === undefined) c.cinematic = defaultCinematic(c, p.defaultCameraId); });
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

  // Temporary airport set, as the module generates it near an airport without saved cameras: runway ends,
  // spotting views, a few gates and the tower. Local NED meters about the airport origin (authoring axes here:
  // x east, y up, z north). Shown under Airport; never stored until the user saves it.
  function demoAirportSet(code) {
    const origin = { lat: 40, lon: -90, altitude: 100 }, set = { categories: [], cameras: [] };
    const groups = [
      ['Runways', 'view-runway', 'runways', [['09 threshold', 'runway:09:threshold', { x: -1000, y: 3, z: -52, yaw: 90, pitch: -2 }], ['27 threshold', 'runway:27:threshold', { x: 1000, y: 3, z: 52, yaw: -90, pitch: -2 }]]],
      ['Spotting', 'view-runway', 'runways', [['09 fence line', 'spot:09:fence', { x: -1150, y: 3, z: -80, yaw: 86, pitch: -0.2 }], ['09 approach, 1 NM', 'spot:09:final', { x: -2852, y: 7.06, z: 0, yaw: 90, pitch: -0.2 }, 7.06]]],
      ['Gates', 'view-gate', 'gates', [['Gate A1', 'parking:A1', { x: 100, y: 3, z: 212, yaw: 180, pitch: -10 }]]],
      ['Tower', 'view-tower', 'tower', [['Tower overview', 'tower', { x: 0, y: 35, z: 300, yaw: 180, pitch: -6 }]]],
    ];
    groups.forEach(([name, iconName, heightCategory, cameras]) => {
      const category = { id: M.uuid(), name, icon: iconName, cameraIds: [] };
      cameras.forEach(([cameraName, key, pose, minimum]) => {
        const camera = seedCamera(category.id, cameraName, null, Object.assign({ fov: 62, roll: 0 }, pose), { type: 'airport', favorite: true, transition: { kind: 'cut', durationSeconds: 0.6, delaySeconds: 0 } });
        Object.assign(camera, { icon: iconName, generatedKey: key, worldOrigin: clone(origin), aircraftTracking: 'both', interaction: false, requiredCapabilities: [],
          airportHeight: { category: heightCategory, baseAltitudeMeters: origin.altitude, minimumMeters: minimum || 0 } });
        category.cameraIds.push(camera.id);
        set.cameras.push(camera);
      });
      category.defaultCameraId = category.cameraIds[0];
      set.categories.push(category);
    });
    set.code = code;
    return set;
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
    [mine,cinematic,airport].forEach(profile=>profile.cameras.forEach(camera=>camera.favorite=true));
    const now = Date.now();
    const repo = { profiles: new Map(), settings: null, bindings: null };
    const addRepo = (profile, revision, history) => repo.profiles.set(profile.profileId, { committed: profile, revision, history: history || [], lastCameraId: null });
    const r2 = clone(mine); r2.cameras[0].projection.value = round(M.degToRad(70), 4);
    const r1 = clone(r2); r1.cameras[0].name = 'Pilot view (original)';
    addRepo(mine, 3, [{ revision: 1, savedAt: new Date(now - 3 * 86400e3).toISOString(), profile: r1 }, { revision: 2, savedAt: new Date(now - 86400e3 - 5 * 3600e3).toISOString(), profile: r2 }]);
    addRepo(cinematic, 1);
    addRepo(airport, 1);
    repo.settings = { schemaVersion: 2, ui: { shortcuts: true, contrast: false, backups: true, remember: true, precision: 'normal', theme: 'dark', reducedMotion: false, miniEnabled: true, miniHover: true, miniPinned: false, miniOpacity: 0.65, nameDisplay: 'both' }, host: { allowUnqualifiedAdapter: false, lookSpeedDegPerSec: 60, moveSpeedMetersPerSec: 0.5, binocularsFovDeg: 10 } };
    Object.assign(repo.settings.host, { cinematicMinSeconds: 8, cinematicMaxSeconds: 20, cinematicCockpitWeight: 0.15, cinematicCuts: true, cinematicDrift: true });
    Object.assign(repo.settings.host, { spottingRadiusNm: 20, autoSpotHoldSeconds: 30 });
    // Saved spotting spots (a preference, saved at once): one runway spot to start with.
    repo.settings.host.spottingSpots = [{ id: 'demo-fence-27', name: 'KDEM 27 fence line', icao: 'KDEM', kind: 'fence', runwayEnd: '27', behindMeters: 150, sideMeters: 80, heightMeters: 3 }];
    repo.bindings = seedBindings();

    const m = {
      bootId: 'demo-' + randomHex(8), epoch: 1, seq: 0, frameSeq: 0,
      session: 'Ready',
      aircraft: { title: C172.title, modelKey: C172.modelKey },
      ownership: { state: 'Observing', gameControlled: true, reason: null },
      forcedBlock: null, forcedSpotting: false, qualified: true, captureQualified: false, simVersion: '1.8.16.0', qualification: null,
      active: { profileId: null, cameraId: null, peeking: false, transitioning: false, fromDraft: false },
      live: null, look: NEUTRAL_LOOK(), peek: null, pendingCommit: null, instances: new Map(), timers: new Set(),
      working: null, selection: { profileId: null, cameraId: null },
      traffic: { aircraft: [], at: 0 }, trafficEnabled: true, trafficTime: null, trafficShown: 0,
      spot: { state: 'off', targetId: null, view: null, autoSpot: false, targetLabel: '', reason: null, before: null, since: 0 },
    };
    const assembler = new Assembler();
    // The demo aircraft starts away from the airport; FSCDemo.arriveAtAirport() (or ?demoAirport=1) parks it at KDEM.
    const demoAirport = { icao: 'KDEM', name: 'Demo Field', transient: true, saved: null, generated: demoAirportSet('KDEM'),
      nearby: typeof location !== 'undefined' && /[?&]demoAirport=1\b/.test(location.search || '') };
    demoAirport.profile = clone(demoAirport.generated);
    // Like the module: a temporary set loads only while Settings > Airport defaults allows it; a saved one always loads.
    const airportLoaded = () => demoAirport.nearby && (!demoAirport.transient || !m.working || m.working.settings.host.airportAutoGenerate !== false);
    const isDemoAirport = item => item.airportIcao === demoAirport.icao;
    function displayProfile() {
      const p = clone(m.working.profile);
      if (!airportLoaded()) return p;
      demoAirport.profile.categories.forEach(c => p.categories.push(Object.assign(clone(c), { airportIcao: demoAirport.icao })));
      demoAirport.profile.cameras.forEach(c => p.cameras.push(Object.assign(clone(c), { airportIcao: demoAirport.icao })));
      return p;
    }
    function splitDisplay(doc) {
      if (!airportLoaded()) return doc;
      demoAirport.profile = { categories: doc.categories.filter(isDemoAirport), cameras: doc.cameras.filter(isDemoAirport) };
      return Object.assign({}, doc, { categories: doc.categories.filter(c => !isDemoAirport(c)), cameras: doc.cameras.filter(c => !isDemoAirport(c)) });
    }
    const airportDirty = () => Boolean(demoAirport.saved) && !same(demoAirport.profile, demoAirport.saved);
    function airportsPayload() {
      const loaded = airportLoaded() ? demoAirport.icao : '', host = m.working ? m.working.settings.host : {};
      return { scope: 'global', autoSwitch: false, loaded, loadedName: loaded ? demoAirport.name : '', pinned: '', transient: Boolean(loaded) && demoAirport.transient,
        autoGenerate: host.airportAutoGenerate !== false, radiusNm: host.airportRadiusNm || 25, positionKnown: true,
        nearby: loaded ? [{ icao: demoAirport.icao, name: demoAirport.name, distanceNm: 0.4, transient: demoAirport.transient }] : [],
        facilities: demoAirport.nearby ? [{ icao: demoAirport.icao, region: 'K1', lat: 40, lon: -90, altitude: 100 }] : [], worldStatus: 'Preview - world coordinates need in-sim qualification' };
    }

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
      const aircraftProfile = !same(m.working.profile, committedProfile());
      return { profile: aircraftProfile || airportDirty(), aircraftProfile, airport: airportDirty(), settings: !same(m.working.settings, repo.settings), bindings: !same(m.working.bindings, repo.bindings) };
    }
    function hasCommittedVersion() {
      const committed = committedProfile();
      const result = {};
      m.working.profile.cameras.forEach(c => { result[c.id] = Boolean(committed && committed.cameras.some(k => k.id === c.id)); });
      if (airportLoaded()) demoAirport.profile.cameras.forEach(c => { result[c.id] = Boolean(demoAirport.saved && demoAirport.saved.cameras.some(k => k.id === c.id)); });
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
      // Spotting needs no camera profile: a profile-only block (forcedSpotting) leaves spotting available.
      let spottingBlockReason = null;
      if (m.forcedBlock && !m.forcedSpotting) spottingBlockReason = m.forcedBlock;
      else if (m.session !== 'Ready') spottingBlockReason = 'Camera Studio is not ready yet.';
      else if (!m.aircraft || !m.aircraft.modelKey) spottingBlockReason = 'Load a flight to use your cameras.';
      else if (!m.qualified && !allow && !m.qualification) spottingBlockReason = 'This simulator build has not passed the axis check yet. Run it from Settings > Simulator.';
      const blockReason = spottingBlockReason || m.forcedBlock || null;
      return { adapterQualified: m.qualified, adapterContract: 'camera-api-v1', captureQualified: m.captureQualified, allowUnqualifiedAdapter: allow, canActivate: !blockReason, blockReason, spottingAvailable: !spottingBlockReason, spottingBlockReason, simVersion: m.simVersion, fovRangeDeg: [20, 120], levelHorizon: true, followHorizon: true, modes: clone(MODE_CAPS) };
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
        working: { profile: displayProfile(), settings: clone(m.working.settings), bindings: clone(m.working.bindings), draftRevision: m.working.draftRevision, dirty: dirty(), committedRevision: workingEntry().revision, hasCommittedVersion: hasCommittedVersion() },
        pendingCommit: m.pendingCommit ? { requestId: m.pendingCommit.requestId, revisions: m.pendingCommit.revisions } : null,
        look: clone(m.look),
        cinematic: cinematicState(),
        traffic: trafficPayload(),
        spotting: spottingState(),
        airports: airportsPayload(),
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
    const setOwnership = (state, reason) => { m.ownership = { state, gameControlled: state !== 'Active', reason: reason || null }; event('OwnershipChanged', m.ownership); if (state !== 'Active') setBinoculars(false); };
    const sendActive = () => event('ActiveCameraChanged', m.active);
    const workingCamera = id => displayProfile().cameras.find(c => c.id === id);
    const categoryOf = id => displayProfile().categories.find(c => c.cameraIds.indexOf(id) >= 0);
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
      if (camera.mode === 'free' && !validOrigin(camera.worldOrigin)) return ack(env, 'Rejected', 'This drone has no position yet. Use Drop drone here to place it.');
      ack(env, 'Accepted');
      clearMotion();
      m.glance = null;
      if (!opts.cinematic) stopCinematic('A camera was selected manually.');
      if (!opts.spotting) endSpotting('A camera was selected manually.');
      if (!opts.peeking) { m.peek = null; if (m.selection.cameraId !== cameraId) select(cameraId); }
      if (m.ownership.state !== 'Active') {
        setOwnership('Acquiring');
        later(() => { setOwnership('Active'); show(camera, opts.peeking); }, 450);
      } else show(camera, opts.peeking);
    }
    function release(env, reason) {
      if (env) ack(env, 'Accepted');
      clearMotion();
      setBinoculars(false);  // releasing the camera (or Panic) cancels the zoom immediately
      stopCinematic(reason === 'Panic release: the simulator has control' ? 'Panic release.' : 'Camera control was released.');
      endSpotting(reason === 'Panic release: the simulator has control' ? 'Panic release.' : 'Camera control was released.');
      m.peek = null;
      m.glance = null;
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
    // Binoculars hold, as the module reports it. Driven by FSCApp.bridge.demo.binoculars(down) in the demo.
    function setBinoculars(active) {
      if (Boolean(m.binoculars) === Boolean(active)) return;
      m.binoculars = Boolean(active);
      event('BinocularsChanged', { active: m.binoculars, fovDeg: (m.working.settings.host && m.working.settings.host.binocularsFovDeg) || 10 });
    }
    // Cinematic mode, as the module runs it: a random view that takes part (never the same twice in a row), each for a
    // random dwell. The demo has no flight, so the phase stays "parked". Its timer survives clearMotion().
    function cinematicState() {
      const c = m.cinematic || {};
      return { enabled: Boolean(c.enabled), state: c.enabled ? (m.ownership.state === 'Active' ? 'running' : 'starting') : 'off', phase: 'parked',
        cameraId: c.enabled ? c.cameraId || null : null, dwellSeconds: c.enabled ? c.dwell || 0 : 0, reason: c.reason || null };
    }
    function stopCinematic(reason) {
      if (!m.cinematic || !m.cinematic.enabled) return;
      clearTimeout(m.cinematic.timer);
      m.cinematic = { enabled: false, reason };
      event('CinematicChanged', cinematicState());
    }
    function cinematicNext(env) {
      const host = m.working.settings.host || {}, c = m.cinematic;
      const views = m.working.profile.cameras.filter(v => activatable(v) && (typeof v.cinematic === 'boolean' ? v.cinematic : defaultCinematic(v, m.working.profile.defaultCameraId)) && v.id !== m.active.cameraId);
      if (!views.length) return false;
      const cockpit = views.filter(v => v.mode === 'aircraftCockpit'), others = views.filter(v => v.mode !== 'aircraftCockpit');
      const share = host.cinematicCockpitWeight == null ? 0.15 : host.cinematicCockpitWeight;
      const pool = cockpit.length && (!others.length || Math.random() < share) ? cockpit : others;
      const view = pool[Math.floor(Math.random() * pool.length)];
      const min = Math.max(3, host.cinematicMinSeconds || 8), max = Math.max(min, host.cinematicMaxSeconds || 20);
      c.cameraId = view.id;
      c.dwell = min + Math.random() * (max - min);
      activate(env || { uiInstanceId: 'module', requestId: 'cinematic-' + (++m.seq) }, view.id, { cinematic: true });
      clearTimeout(c.timer);
      c.timer = later(() => { if (m.cinematic === c && c.enabled) { cinematicNext(null); event('CinematicChanged', cinematicState()); } }, c.dwell * 1000, true);
      return true;
    }
    function setCinematic(env, enabled) {
      const on = Boolean(m.cinematic && m.cinematic.enabled);
      if (enabled === on) return ack(env, 'Completed', null, { cinematic: cinematicState() });
      if (!enabled) { stopCinematic('Cinematic mode was turned off.'); return ack(env, 'Completed', null, { cinematic: cinematicState() }); }
      const caps = capabilities();
      if (!caps.canActivate) return ack(env, 'Rejected', caps.blockReason, { cinematic: cinematicState() });
      m.cinematic = { enabled: true, reason: null };
      endSpotting('Cinematic mode started.');
      if (!cinematicNext({ uiInstanceId: env.uiInstanceId, requestId: env.requestId + ':cinematic' })) {
        m.cinematic = { enabled: false, reason: null };
        return ack(env, 'Rejected', 'No views are included in Cinematic mode. Turn on Include in Cinematic mode for exterior, airport or cockpit views.', { cinematic: cinematicState() });
      }
      event('CinematicChanged', cinematicState());
      return ack(env, 'Completed', null, { cinematic: cinematicState() });
    }
    /* Spotting: AI and network traffic around the demo field, reported at most once a second as the module does.
       Local meters about the demo aircraft (x east, y north); runway 09/27 runs east-west 600 m north of it. Each
       aircraft flies a repeating path, so the list keeps moving: parked, taxiing, taking off, departing, arriving,
       landing and cruising traffic. FSCDemo.traffic({ enabled, time }) empties the list or freezes the clock. */
    const NM = 1852, KT = 0.514444, FT = 0.3048, GLIDE = Math.tan(3 * Math.PI / 180);
    const FIELD = { x: 0, y: 600, elevationFt: 1417 }, THRESHOLD_09 = -1200;
    const phase = (t, period) => ((t % period) + period) % period;
    const ground = (x, y, gsKt, hdg) => ({ x, y, altFt: FIELD.elevationFt, gsKt, vsFpm: 0, hdg, onGround: true });
    const air = (x, y, altFt, gsKt, vsFpm, hdg) => ({ x, y, altFt, gsKt, vsFpm, hdg, onGround: false });
    const DEMO_TRAFFIC = [
      { objectId: 101, title: 'Airbus A320neo Lufthansa', atcId: 'D-AINB', airline: 'Lufthansa', flightNumber: '441', model: 'A320', at: () => ground(150, 150, 0, 0) },
      { objectId: 102, title: 'Cessna 172 Skyhawk', atcId: 'N172DM', airline: '', flightNumber: '', model: 'C172', at: () => ground(-300, 120, 0, 180) },
      { objectId: 103, title: 'Airbus A220-300 Swiss', atcId: 'HB-JCA', airline: 'Swiss', flightNumber: '1084', model: 'A220', at: t => {
        const s = phase(t, 518) * 15 * KT; return s < 2000 ? ground(-1000 + s, 450, 15, 90) : ground(1000 - (s - 2000), 450, 15, 270);
      } },
      { objectId: 104, title: 'Airbus A319 easyJet', atcId: 'G-EZBA', airline: 'easyJet', flightNumber: '2914', model: 'A319', period: 240, at: t => {
        const p = phase(t, 240);
        if (p < 35) return ground(THRESHOLD_09 + (40 * p + 1.5 * p * p) * KT, FIELD.y, 40 + 3 * p, 90);
        const q = p - 35; return air(THRESHOLD_09 + 3237.5 * KT + q * 160 * KT, FIELD.y, FIELD.elevationFt + q * 2000 / 60, 160, 2000, 90);
      } },
      { objectId: 105, title: 'Boeing 737-800 KLM', atcId: 'PH-BXA', airline: 'KLM', flightNumber: '1956', model: 'B738', at: t => {
        const q = phase(t + 120, 300); return air(THRESHOLD_09 - 2000 - q * 200 * KT, FIELD.y, FIELD.elevationFt + 1500 + q * 1800 / 60, 200, 1800, 270);
      } },
      { objectId: 106, title: 'Airbus A321 British Airways', atcId: 'G-EUXC', airline: 'British Airways', flightNumber: '710', model: 'A321', period: 300, offset: 60, at: t => {
        const p = phase(t + 60, 300);
        if (p >= 270) { const r = p - 270; return ground(THRESHOLD_09 + (130 * r - 1.5 * r * r) * KT, FIELD.y, 130 - 3 * r, 90); }
        const d = (270 - p) * 140 * KT; return air(THRESHOLD_09 - d, FIELD.y, FIELD.elevationFt + 50 + d * GLIDE / FT, 140, -140 * KT * GLIDE / FT * 60, 90);
      } },
      { objectId: 107, title: 'Piper PA-28 Cherokee', atcId: 'N456AB', airline: '', flightNumber: '', model: 'PA28', at: t => {
        const p = phase(t + 30, 200); return air(THRESHOLD_09 - 1500, FIELD.y + (200 - p) * 90 * KT, FIELD.elevationFt + 400 + (200 - p) * 500 / 60, 90, -500, 180);
      } },
      { objectId: 108, title: 'Boeing 787-9 United', atcId: 'N36962', airline: 'United', flightNumber: '15', model: 'B789', at: t => {
        const p = phase(t, 330); return air(-20 * NM + p * 480 * KT, FIELD.y + 9 * NM, 35000, 480, 0, 90);
      } },
    ];
    const trafficStart = Date.now();
    const trafficClock = () => (m.trafficTime !== null ? m.trafficTime : (Date.now() - trafficStart) / 1000);
    // The module's status rules, against the demo field (the loaded or nearest airport).
    function trafficStatus(def, s, t) {
      if (s.onGround) {
        if (s.gsKt < 1) return 'parked';
        if (s.gsKt < 40) return 'taxi';
        // Airborne in the last 60 s of this pass (a path's previous pass does not count).
        const since = def.period ? phase(t + (def.offset || 0), def.period) : 60;
        for (let k = 5; k <= Math.min(60, since); k += 5) if (!def.at(t - k).onGround) return 'landing';
        return 'takeoff';
      }
      const near = Math.hypot(s.x - FIELD.x, s.y - FIELD.y) <= 15 * NM;
      if (near && s.vsFpm > 300) return 'departing';
      if (near && s.vsFpm < -300) return 'arriving';
      return 'cruise';
    }
    function trafficPayload() {
      const t = trafficClock(), host = m.working ? m.working.settings.host : {}, radius = host.spottingRadiusNm == null ? 20 : host.spottingRadiusNm;
      const at = Math.round(t * 1000);
      if (!m.trafficEnabled || !m.aircraft || !m.aircraft.modelKey) { m.traffic = { aircraft: [], at }; return clone(m.traffic); }
      const o = DEMO_AIRCRAFT_ORIGIN, perLon = 111320 * Math.cos(o.lat * Math.PI / 180);
      const list = DEMO_TRAFFIC.map(def => {
        const s = def.at(t), distance = Math.hypot(s.x, s.y);
        return { objectId: def.objectId, title: def.title, atcId: def.atcId, airline: def.airline, flightNumber: def.flightNumber, model: def.model,
          latitude: round(o.lat + s.y / 111320, 6), longitude: round(o.lon + s.x / perLon, 6), altitudeFt: Math.round(s.altFt), groundSpeedKt: Math.round(s.gsKt),
          verticalSpeedFpm: Math.round(s.vsFpm), headingDeg: Math.round(s.hdg), onGround: s.onGround, distanceNm: round(distance / NM, 2),
          bearingDeg: Math.round((Math.atan2(s.x, s.y) * 180 / Math.PI + 360) % 360), status: trafficStatus(def, s, t) };
      }).filter(a => a.distanceNm <= radius).sort((a, b) => a.distanceNm - b.distanceNm);
      m.traffic = { aircraft: list, at };
      return clone(m.traffic);
    }
    const SPOT_VIEWS = ['tower', 'runway', 'chase', 'flyby', 'track', 'spot'];
    const AUTO_STATUSES = ['arriving', 'landing', 'takeoff', 'departing'];
    const AUTO_VIEWS = { arriving: 'runway', landing: 'runway', takeoff: 'runway', taxi: 'tower', departing: 'chase' };
    const NO_VIEW_DATA = { tower: 'No tower position is known for an airport near this aircraft yet.', runway: 'No runway data is known for an airport near this aircraft yet.' };
    const spotLabel = a => ((a.airline && a.flightNumber ? a.airline + ' ' + a.flightNumber : '') || a.atcId || a.title) + (a.model ? ' - ' + a.model : '');
    // Computed tower and runway views come from the scenery data of the loaded airport (the demo field's runway 09/27).
    const DEMO_RUNWAYS = [{ icao: 'KDEM', end: '09', headingDeg: 90, lengthMeters: 2000, widthMeters: 45 }, { icao: 'KDEM', end: '27', headingDeg: 270, lengthMeters: 2000, widthMeters: 45 }];
    const RUNWAY_SPOT_KINDS = ['fence', 'approach', 'threshold', 'custom'];
    const spotRunwayDefaults = (kind, end) => kind === 'approach' ? { behindMeters: 1852, sideMeters: 0, heightMeters: 1852 * GLIDE - 90 }
      : kind === 'threshold' ? { behindMeters: 0, sideMeters: end.widthMeters / 2 + 12, heightMeters: 3 } : { behindMeters: 150, sideMeters: 80, heightMeters: 3 };
    const spotName = value => String(value && value.name || '').replace(/[\u0000-\u001f\u007f]/g, '').trim().slice(0, 60);
    const metersBetween = (a, b) => Math.hypot((b.latitude - a.latitude) * 111320, (b.longitude - a.longitude) * 111320 * Math.cos(a.latitude * Math.PI / 180));
    /** SpotTarget / AdjustSpot `spot` as the module parses it: { spot } (with pinned) or { error }. */
    function parseSpot(value, target) {
      if (!value || typeof value !== 'object') return { error: 'A runway spot needs an airport and a runway end.' };
      const finite = (key, low, high) => typeof value[key] === 'number' && isFinite(value[key]) && value[key] >= low && value[key] <= high;
      const name = spotName(value), named = out => (name ? Object.assign(out, { name }) : out);
      if (value.kind === 'position') {
        if (!finite('latitude', -90, 90) || !finite('longitude', -180, 180)) return { error: "The spot's position is not valid." };
        if (!finite('altitudeMeters', -500, 10000)) return { error: "The spot's altitudeMeters is out of range." };
        if (target && metersBetween(value, target) > 20000) return { error: 'That spot is more than 20 km from this aircraft. Choose a spot near it.' };
        return { spot: named({ kind: 'position', latitude: value.latitude, longitude: value.longitude, altitudeMeters: value.altitudeMeters, pinned: true }) };
      }
      const end = airportLoaded() ? DEMO_RUNWAYS.find(r => r.icao === value.icao && r.end === value.runwayEnd) : null;
      if (!end) return { error: 'That runway end is not known. Wait for the airport\'s scenery data, or choose another end.' };
      const kind = value.kind === undefined ? 'fence' : value.kind;
      if (RUNWAY_SPOT_KINDS.indexOf(kind) < 0) return { error: 'Choose a spot type: fence, approach, threshold or custom.' };
      const spot = Object.assign({ icao: end.icao, runwayEnd: end.end, kind }, spotRunwayDefaults(kind === 'custom' ? 'fence' : kind, end));
      for (const [key, low, high] of [['behindMeters', -5000, 5000], ['sideMeters', -5000, 5000], ['heightMeters', 0, 1000]]) {
        if (value[key] === undefined || value[key] === null) continue;
        if (!finite(key, low, high)) return { error: "The spot's " + key + ' is out of range.' };
        spot[key] = value[key];
      }
      spot.pinned = true;
      return { spot: named(spot) };
    }
    /** host.spottingSpots as the module normalizes it (at most 200, clamped, ids repaired). */
    function normalizeSpots(list) {
      const out = [], ids = {};
      (Array.isArray(list) ? list : []).forEach(v => {
        if (out.length >= 200 || !v || typeof v !== 'object') return;
        const kind = v.kind, num = key => (typeof v[key] === 'number' && isFinite(v[key]) ? v[key] : null), clamp = (x, a, b) => Math.min(b, Math.max(a, x));
        const spot = { kind };
        if (kind === 'position') {
          if (num('latitude') === null || num('longitude') === null || num('altitudeMeters') === null || Math.abs(v.latitude) > 90 || Math.abs(v.longitude) > 180) return;
          Object.assign(spot, { latitude: v.latitude, longitude: v.longitude, altitudeMeters: clamp(v.altitudeMeters, -500, 10000) });
        } else if (RUNWAY_SPOT_KINDS.indexOf(kind) >= 0) {
          const end = String(v.runwayEnd || '').trim().slice(0, 8);
          if (!end) return;
          spot.runwayEnd = end;
          [['behindMeters', -5000, 5000], ['sideMeters', -5000, 5000], ['heightMeters', 0, 1000]].forEach(([key, low, high]) => { if (num(key) !== null) spot[key] = clamp(v[key], low, high); });
        } else return;
        const icao = String(v.icao || '').toUpperCase().replace(/[^A-Z0-9]/g, '');
        spot.icao = icao.length <= 8 ? icao : '';
        spot.name = spotName(v) || 'Spot';
        let id = typeof v.id === 'string' && /^[A-Za-z0-9_.:-]{1,64}$/.test(v.id) && !ids[v.id] ? v.id : '';
        for (let n = out.length + 1; !id; n++) if (!ids['spot-' + n]) id = 'spot-' + n;
        ids[id] = true;
        spot.id = id;
        out.push(spot);
      });
      return out;
    }
    const spottingState = () => ({ targetId: m.spot.targetId, view: m.spot.view, autoSpot: m.spot.autoSpot, state: m.spot.state, targetLabel: m.spot.targetLabel || '', reason: m.spot.reason || null,
      views: { tower: airportLoaded(), runway: airportLoaded() }, runways: airportLoaded() ? clone(DEMO_RUNWAYS) : [], spot: m.spot.spot ? clone(m.spot.spot) : null });
    const spotEnv = () => ({ uiInstanceId: 'module', requestId: 'spotting-' + (++m.seq) });
    function spotAirportCamera(view, target) {
      if (!airportLoaded()) return null;
      const list = demoAirport.profile.cameras.map(c => workingCamera(c.id)).filter(Boolean);
      if (view === 'tower') return list.find(c => c.generatedKey === 'tower' || /tower/i.test(c.name)) || null;
      // The runway or spotting view nearest the target (FRD north/east about the field).
      const rad = target.bearingDeg * Math.PI / 180, east = target.distanceNm * NM * Math.sin(rad), north = target.distanceNm * NM * Math.cos(rad) - FIELD.y;
      const runway = list.filter(c => /^(runway|spot):/.test(c.generatedKey || '') || /runway|threshold|approach|fence/i.test(c.name));
      runway.sort((a, b) => Math.hypot(a.positionMetersFRD[0] - north, a.positionMetersFRD[1] - east) - Math.hypot(b.positionMetersFRD[0] - north, b.positionMetersFRD[1] - east));
      return runway[0] || null;
    }
    /** Validates a spotting request; returns { error } or what spotApply needs. */
    function spotCheck(objectId, view, pinned) {
      if (SPOT_VIEWS.indexOf(view) < 0) return { error: 'Choose a tower, runway, chase, fly-by, track or spot view.' };
      const position = Boolean(pinned) && pinned.kind === 'position';
      if (view === 'spot' && !position) return { error: 'The spot view needs a position spot.' };
      if (pinned && position && view !== 'spot') return { error: 'A position spot can only be used with the spot view.' };
      if (pinned && !position && view !== 'runway') return { error: 'A runway spot can only be used with the runway view.' };
      const caps = capabilities();
      if (!caps.spottingAvailable) return { error: caps.spottingBlockReason };
      trafficPayload();
      const target = m.traffic.aircraft.find(a => a.objectId === objectId);
      if (!target) return { error: 'That aircraft is no longer nearby.' };
      let camera = null, runway = null;
      if (pinned) {
        // A chosen runway spot or position spot: a computed view at that place, aimed at the target.
        const parsed = parseSpot(pinned, target);
        if (parsed.error) return { error: parsed.error };
        return { target, camera: null, view, runway: parsed.spot };
      }
      if (view === 'tower' || view === 'runway') {
        // The loaded airport set's view when the profile is usable, else a computed view from scenery data.
        if (!airportLoaded()) return { error: NO_VIEW_DATA[view] };
        camera = caps.canActivate ? spotAirportCamera(view, target) : null;
        if (!camera && view === 'runway') runway = { icao: 'KDEM', runwayEnd: target.headingDeg > 0 && target.headingDeg <= 180 ? '09' : '27', kind: 'fence', behindMeters: 150, sideMeters: 80, heightMeters: 3, pinned: false };
      }
      if (view === 'track') {
        const shown = m.ownership.state === 'Active' && workingCamera(m.active.cameraId);
        const overlay = m.spot.state !== 'off' && m.spot.view ? ['tower', 'runway', 'track', 'spot'].indexOf(m.spot.view) >= 0 : false;
        if (!caps.canActivate && !overlay) return { error: 'Track aims an airport or free camera from your camera profile. Without one, use Chase, Fly-by, Tower or Runway.' };
        const world = m.spot.state !== 'off' && m.spot.view ? overlay : Boolean(shown && (shown.mode === 'airport' || shown.mode === 'free'));
        if (!world) return { error: 'Track aims the current airport or free camera at the aircraft. Show one of those views first.' };
      }
      return { target, camera, view, runway };
    }
    // Takes control if needed. Chase and fly-by views have no saved camera; tower and runway show the airport camera.
    function spotApply(check, auto) {
      const s = m.spot, target = check.target;
      stopCinematic('Spotting started.');
      if (s.state === 'off' || s.state === 'waiting') s.before = m.ownership.state === 'Active' || m.ownership.state === 'Acquiring' ? m.active.cameraId : null;
      Object.assign(s, { state: 'tracking', targetId: target.objectId, view: check.view, targetLabel: spotLabel(target), reason: null, since: trafficClock(), autoSpot: Boolean(auto), spot: check.runway || null });
      clearMotion();
      m.glance = null; m.peek = null;
      const cameraId = check.view === 'track' ? m.active.cameraId : check.camera ? check.camera.id : null;
      if (check.camera) m.live = clone(check.camera);
      const shown = () => { m.active = { profileId: m.working.profile.profileId, cameraId, peeking: false, transitioning: false, fromDraft: false }; sendActive(); };
      if (m.ownership.state !== 'Active') { setOwnership('Acquiring'); later(() => { setOwnership('Active'); shown(); }, 450); } else shown();
      event('SpottingChanged', spottingState());
    }
    function spotTarget(env, objectId, view, spot) {
      const check = spotCheck(objectId, view, spot || null);
      if (check.error) return ack(env, 'Rejected', check.error, { spotting: spottingState() });
      spotApply(check, false);
      return ack(env, 'Completed', null, { spotting: spottingState() });
    }
    /** AdjustSpot: moves the pinned spot being shown; same target, view and auto-spot state. */
    function adjustSpot(env, value) {
      const s = m.spot, current = s.spot;
      const reject = reason => ack(env, 'Rejected', reason, { spotting: spottingState() });
      if ((s.state !== 'tracking' && s.state !== 'lost') || !current || !current.pinned) return reject('Not watching from a pinned spot. Choose Watch from here first.');
      const position = Boolean(value) && value.kind === 'position';
      if (position !== (current.kind === 'position')) return reject(position ? 'This view shows a runway spot. Adjust it with a runway spot.' : 'This view shows a position spot. Adjust it with a position spot.');
      const parsed = parseSpot(value, m.traffic.aircraft.find(a => a.objectId === s.targetId));
      if (parsed.error) return reject(parsed.error);
      s.spot = parsed.spot;
      m.spotAdjusts = (m.spotAdjusts || 0) + 1;
      event('SpottingChanged', spottingState());
      return ack(env, 'Completed', null, { spotting: spottingState() });
    }
    /** Ends spotting (and Auto-spot). restore: back to the camera shown before spotting began, or release. */
    function endSpotting(reason, restore) {
      const s = m.spot, was = s.state, before = s.before;
      if (was === 'off' && !s.autoSpot) return false;
      m.spot = { state: 'off', targetId: null, view: null, autoSpot: false, targetLabel: '', reason: reason || null, before: null, since: 0 };
      event('SpottingChanged', spottingState());
      if (restore && (was === 'tracking' || was === 'lost')) {
        const camera = before && workingCamera(before);
        if (camera && activatable(camera)) activate(spotEnv(), before, { spotting: true });
        else release(null, 'spottingStopped');
      }
      return true;
    }
    function autoCandidate(exclude) {
      const rank = a => (a.status === 'arriving' || a.status === 'landing' ? 0 : 1) * 1000 + a.distanceNm;
      return m.traffic.aircraft.filter(a => AUTO_STATUSES.indexOf(a.status) >= 0 && a.objectId !== exclude).sort((a, b) => rank(a) - rank(b))[0] || null;
    }
    function autoSpotTo(target) {
      let check = spotCheck(target.objectId, AUTO_VIEWS[target.status] || 'chase');
      if (check.error && check.view !== 'chase') check = spotCheck(target.objectId, 'chase');
      if (check.error) return false;
      spotApply(check, true);
      return true;
    }
    function setAutoSpot(env, enabled) {
      const s = m.spot;
      if (!enabled) {
        if (s.autoSpot) { s.autoSpot = false; if (s.state === 'waiting') s.state = 'off'; event('SpottingChanged', spottingState()); }
        return ack(env, 'Completed', null, { spotting: spottingState() });
      }
      if (s.autoSpot) return ack(env, 'Completed', null, { spotting: spottingState() });
      const caps = capabilities();
      if (!caps.spottingAvailable) return ack(env, 'Rejected', caps.spottingBlockReason, { spotting: spottingState() });
      stopCinematic('Auto-spot started.');
      trafficPayload();
      const target = autoCandidate(null);
      if (!target || !autoSpotTo(target)) {
        Object.assign(s, { autoSpot: true, state: s.state === 'tracking' ? 'tracking' : 'waiting', reason: s.state === 'tracking' ? null : 'No arriving or departing traffic near the airport yet.', since: trafficClock() });
        event('SpottingChanged', spottingState());
      }
      return ack(env, 'Completed', null, { spotting: spottingState() });
    }
    // Once a second: follow the target, report it lost when it leaves the radius, and let Auto-spot move on.
    function spottingTick() {
      const s = m.spot;
      if (s.state === 'off') return;
      const target = s.targetId !== null ? m.traffic.aircraft.find(a => a.objectId === s.targetId) : null;
      let changed = false;
      if (s.state === 'tracking' && s.targetId !== null && !target) {
        const host = m.working.settings.host;
        Object.assign(s, { state: 'lost', reason: 'It left the ' + (host.spottingRadiusNm == null ? 20 : host.spottingRadiusNm) + ' NM traffic radius.' }); changed = true;
      } else if (s.state === 'lost' && target) { Object.assign(s, { state: 'tracking', reason: null }); changed = true; }
      if (s.autoSpot) {
        const hold = m.working.settings.host.autoSpotHoldSeconds == null ? 30 : m.working.settings.host.autoSpotHoldSeconds;
        const good = Boolean(target) && AUTO_STATUSES.indexOf(target.status) >= 0;
        // Holds each aircraft at least autoSpotHoldSeconds, then moves on to the next arrival or departure.
        if (s.state === 'waiting' || s.state === 'lost' || trafficClock() - s.since >= hold) {
          const next = autoCandidate(s.targetId);
          if (next && autoSpotTo(next)) return;
          if (s.state === 'lost' || (s.state === 'tracking' && !good)) {
            Object.assign(s, { state: 'waiting', targetId: null, view: null, targetLabel: '', reason: 'No arriving or departing traffic near the airport.' }); changed = true;
          }
        }
      }
      if (changed) event('SpottingChanged', spottingState());
    }
    function trafficTick(force) {
      if (!m.instances.size && !force) return;
      const payload = trafficPayload();
      spottingTick();
      if (payload.aircraft.length || m.trafficShown || force) event('TrafficChanged', payload);
      m.trafficShown = payload.aircraft.length;
    }
    {
      const timer = setInterval(() => trafficTick(false), 1000);
      if (timer && typeof timer.unref === 'function') timer.unref(); // never keeps a Node test process alive
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
    /* Smart views (glance): the module's hold / next / return, with the same nearest-instrument rule. */
    const GLANCE_FOCUS_METERS = 0.75, GLANCE_MS = 350;
    const forward = q => M.rotateVector(q, [1, 0, 0]);
    const angleOf = (a, b) => Math.acos(Math.max(-1, Math.min(1, (a[0] * b[0] + a[1] * b[1] + a[2] * b[2]) / (Math.hypot.apply(null, a) * Math.hypot.apply(null, b) || 1))));
    function glanceViews(exclude) {
      const order = [];
      m.working.profile.categories.forEach(g => g.cameraIds.forEach(id => order.push(id)));
      return order.map(workingCamera).filter(c => c && c.mode === 'aircraftCockpit' && (typeof c.glance === 'boolean' ? c.glance : defaultGlance(c)) && c.id !== exclude);
    }
    function glanceBlock() {
      const camera = m.active.cameraId && workingCamera(m.active.cameraId);
      if (m.ownership.state !== 'Active' || !camera || !m.live) return 'Show a cockpit view first: a glance returns to the view you are in.';
      if (m.spot.state !== 'off') return 'Glance views are not available while spotting. Stop spotting first.';
      if (camera.mode !== 'aircraftCockpit') return 'Glance views work from cockpit views.';
      return null;
    }
    function nearestGlance(exclude) {
      const shown = composedPose(), look = forward(shown.orientationCameraToAnchorXYZW);
      let best = null, bestAngle = 0;
      glanceViews(exclude).forEach(c => {
        const aim = forward(c.orientationCameraToAnchorXYZW);
        let toward = c.positionMetersFRD.map((v, i) => v + aim[i] * GLANCE_FOCUS_METERS - shown.positionMetersFRD[i]);
        if (Math.hypot.apply(null, toward) < 1e-3) toward = aim;
        const angle = angleOf(look, toward);
        if (!best || angle < bestAngle - 0.5 * Math.PI / 180) { best = c; bestAngle = angle; }
      });
      return best;
    }
    function glanceShow(camera, look) {
      const committed = committedProfile() && committedProfile().cameras.find(c => c.id === camera.id);
      clearMotion();
      m.live = clone(camera);
      m.look = look ? clone(look) : NEUTRAL_LOOK();
      if (m.selection.cameraId !== camera.id) select(camera.id);
      const glance = m.glance ? { cameraId: m.glance.cameraId, returnCameraId: m.glance.returnCameraId, held: m.glance.held } : null;
      m.active = { profileId: m.working.profile.profileId, cameraId: camera.id, peeking: false, transitioning: true, fromDraft: !same(committed, camera), glance };
      sendActive();
      event('TemporaryLookChanged', m.look);
      later(() => { if (m.active.cameraId === camera.id) { m.active = Object.assign({}, m.active, { transitioning: false }); sendActive(); } }, GLANCE_MS);
    }
    function glanceTo(camera, held) {
      if (!m.glance) m.glance = { returnCameraId: m.active.cameraId, returnLook: clone(m.look), cameraId: null, held: false };
      m.glance.cameraId = camera.id;
      m.glance.held = m.glance.held || held;
      glanceShow(camera);
    }
    function glance(action) {
      if (m.glance && m.active.cameraId !== m.glance.cameraId) m.glance = null;
      if (action === 'glanceReturn') {
        if (!m.glance) return 'Not glancing: there is no view to return to.';
        const back = workingCamera(m.glance.returnCameraId), look = m.glance.returnLook;
        m.glance = null;
        if (!back) { sendActive(); return 'The view you glanced from is no longer available.'; }
        glanceShow(back, look);
        return 'Returned to ' + back.name + '.';
      }
      const blocked = glanceBlock();
      if (blocked) return blocked;
      const exclude = m.glance ? m.glance.returnCameraId : m.active.cameraId;
      let target = null;
      if (action === 'glanceHold') target = nearestGlance(exclude);
      else {
        const views = glanceViews(exclude), order = [];
        m.working.profile.categories.forEach(g => g.cameraIds.forEach(id => order.push(id)));
        const start = order.indexOf(m.active.cameraId);
        for (let k = 1; start >= 0 && k <= order.length && !target; k++) target = views.find(v => v.id === order[(start + k) % order.length]) || null;
        target = target || views[0] || null;
      }
      if (!target) return 'No glance views in this profile. Turn on Glance view for an instrument camera in its Behavior settings.';
      // The Test button presses without a release: the glance stays until Return from glance.
      glanceTo(target, false);
      return 'Glancing at ' + target.name + '.';
    }
    const draftChanged = (ops, origin) => event('DraftChanged', { draftRevision: m.working.draftRevision, dirty: dirty(), ops, origin: origin || null });
    function testBinding(env, binding) {
      const action = binding && binding.action;
      if (action === 'binoculars') return ack(env, 'Completed', null, { dispatched: false, message: 'Close the editor and use the assigned input in the simulator to test this held or wheel control.' });
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
        case 'panicRelease':
          // Same hand-back as releaseCamera, and idempotent: a press while Observing still reports.
          m.cinematicStoppedByPanic = Boolean(m.cinematic && m.cinematic.enabled);
          m.spottingStoppedByPanic = m.spot.state !== 'off' || m.spot.autoSpot;
          release(null, 'Panic release: the simulator has control');
          event('PanicReleased', { state: m.ownership.state, reason: m.ownership.reason, airportAutoStopped: false, cinematicStopped: Boolean(m.cinematicStoppedByPanic), spottingStopped: Boolean(m.spottingStoppedByPanic) });
          message = 'Camera released and simulator settings restored.';
          break;
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
        case 'spotNext': case 'spotPrevious': {
          const list = (trafficPayload(), m.traffic.aircraft), n = list.length;
          if (!n) { message = 'No traffic nearby.'; break; }
          const step = action === 'spotNext' ? 1 : -1, i = list.findIndex(a => a.objectId === m.spot.targetId);
          const target = list[i < 0 ? (step > 0 ? 0 : n - 1) : (i + step + n) % n];
          // A pinned runway spot or position spot stays where it is for the next aircraft.
          const view = m.spot.state !== 'off' && m.spot.view ? m.spot.view : 'chase', kept = m.spot.state !== 'off' && m.spot.spot && m.spot.spot.pinned ? m.spot.spot : null;
          let check = spotCheck(target.objectId, view, kept);
          if (check.error && check.view !== 'chase') check = spotCheck(target.objectId, 'chase');
          if (check.error) { message = blocked(check.error); break; }
          spotApply(check, false); recalled = true; message = 'Spotting ' + spotLabel(target) + '.';
          break;
        }
        case 'spotReturn': recalled = endSpotting('Spotting was stopped.', true); message = recalled ? 'Back to your own aircraft.' : 'Not spotting.'; break;
        case 'toggleAutoSpot': setAutoSpot(internal(env, 'dispatch'), !m.spot.autoSpot); recalled = m.spot.autoSpot; message = recalled ? 'Auto-spot is on.' : 'Auto-spot is off.'; break;
        case 'toggleCinematic': setCinematic(internal(env, 'dispatch'), !(m.cinematic && m.cinematic.enabled)); recalled = Boolean(m.cinematic && m.cinematic.enabled); message = recalled ? 'Cinematic mode is on.' : 'Cinematic mode is off.'; break;
        case 'glanceHold': case 'glanceNext': case 'glanceReturn': message = glance(action); recalled = Boolean(m.active.glance) || action === 'glanceReturn'; break;
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
        case 'SetCinematic': return setCinematic(env, Boolean(payload.enabled));
        case 'SpotTarget': return spotTarget(env, payload.objectId, payload.view, payload.spot);
        case 'AdjustSpot': return adjustSpot(env, payload.spot);
        case 'StopSpotting': endSpotting('Spotting was stopped.', true); return ack(env, 'Completed', null, { spotting: spottingState() });
        case 'SetAutoSpot': return setAutoSpot(env, Boolean(payload.enabled));
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
          ops.forEach(op => { if (op.op === 'setCamera' && op.camera && op.camera.glance === undefined) op.camera.glance = defaultGlance(op.camera); });
          ops.forEach(op => { if (op.op === 'setCamera' && op.camera && op.camera.cinematic === undefined) op.camera.cinematic = defaultCinematic(op.camera, m.working.profile.defaultCameraId); });
          let next;
          const airportBefore = airportLoaded();
          try { next = applyOps({ profile: displayProfile(), settings: m.working.settings, bindings: m.working.bindings }, ops); } catch (error) { return ack(env, 'Rejected', error.reason || 'invalidOps'); }
          m.working.profile = splitDisplay(next.profile); m.working.settings = next.settings; m.working.bindings = next.bindings;
          if (ops.some(op => op.op === 'setSettings')) {
            // Preferences are saved at once and never pending: the mini pin and the saved spotting spots.
            m.working.settings.host.spottingSpots = normalizeSpots(m.working.settings.host.spottingSpots);
            repo.settings.ui.miniPinned = Boolean(m.working.settings.ui.miniPinned);
            repo.settings.host.spottingSpots = clone(m.working.settings.host.spottingSpots);
          }
          m.working.draftRevision++;
          ack(env, 'Completed', null, { draftRevision: m.working.draftRevision });
          draftChanged(ops, env.uiInstanceId);
          if (airportBefore !== airportLoaded()) {
            // The temporary airport set was turned on or off in Settings.
            if (!workingCamera(m.selection.cameraId)) m.selection = { profileId: m.working.profile.profileId, cameraId: m.working.profile.defaultCameraId };
            broadcastSnapshot();
          }
          if (ops.some(op => op.op === 'setSettings')) event('CapabilitiesChanged', capabilities());
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
          demoAirport.profile = clone(demoAirport.saved || demoAirport.generated);
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
            if (!demoAirport.transient) demoAirport.saved = clone(demoAirport.profile); // a temporary set is only stored by its own Save
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
          if(payload.forAircraft&&(!m.aircraft||!m.aircraft.modelKey))return ack(env,'Rejected','noAircraft');
          if(payload.expectedAircraftModelKey&&(!m.aircraft||payload.expectedAircraftModelKey!==m.aircraft.modelKey))return ack(env,'Rejected','The loaded aircraft changed. Try again.');
          const from=payload.from||(payload.copyOf?(payload.copyOf===m.working.profile.profileId?'working':'profile'):'starter');
          const sourceId=payload.profileId||payload.copyOf;
          let base;
          if(from!=='working'&&dirty().profile&&!payload.discard)return ack(env,'Rejected','unsavedChanges');
          if (from==='working') base = clone(m.working.profile);
          else if (from==='profile') {if(!repo.profiles.has(sourceId))return ack(env,'Rejected','unknownProfile');base=clone(repo.profiles.get(sourceId).committed);}
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
        case 'DroneHere': {
          // The demo aircraft sits at DEMO_AIRCRAFT_ORIGIN heading north, level: its frame is north, east, down.
          if (!m.aircraft || !m.aircraft.modelKey) return ack(env, 'Rejected', 'Load a flight and wait for the aircraft position before placing a drone.');
          const worldOf = (origin, frd) => ({ latitude: round(origin.lat + frd[0] / 111320, 7), longitude: round(origin.lon + frd[1] / (111320 * Math.cos(origin.lat * Math.PI / 180)), 7), altitudeMeters: round(origin.altitude - frd[2], 2) });
          if (m.ownership.state === 'Active' && m.spot.state !== 'off' && (!m.active.cameraId || m.spot.view === 'spot')) {
            // A spotting view on screen: a position spot where it stands, else 60 m behind and 15 m above the target.
            const target = m.traffic.aircraft.find(a => a.objectId === m.spot.targetId), place = m.spot.spot && m.spot.spot.kind === 'position' ? m.spot.spot : null;
            const world = place ? { latitude: place.latitude, longitude: place.longitude, altitudeMeters: place.altitudeMeters }
              : target ? worldOf({ lat: target.latitude, lon: target.longitude, altitude: target.altitudeFt * FT }, [-60 * Math.cos(target.headingDeg * Math.PI / 180), -60 * Math.sin(target.headingDeg * Math.PI / 180), -15]) : worldOf(DEMO_AIRCRAFT_ORIGIN, [-40, 0, -12]);
            return ack(env, 'Completed', null, { worldOrigin: clone(DEMO_AIRCRAFT_ORIGIN), positionMetersFRD: [0, 0, 0], orientationCameraToAnchorXYZW: [0, 0, 0, 1], fovRad: round(M.degToRad(60), 5),
              world, source: 'spot', message: 'Drone placed at the spotting view.' });
          }
          const live = m.ownership.state === 'Active' && m.live ? composedPose() : null;
          const drone = live && m.live.mode === 'free' && validOrigin(m.live.worldOrigin);
          const origin = drone ? m.live.worldOrigin : DEMO_AIRCRAFT_ORIGIN;
          return ack(env, 'Completed', null, live ? {
            world: worldOf(origin, live.positionMetersFRD),
            worldOrigin: clone(drone ? m.live.worldOrigin : DEMO_AIRCRAFT_ORIGIN), positionMetersFRD: live.positionMetersFRD,
            orientationCameraToAnchorXYZW: live.orientationCameraToAnchorXYZW, fovRad: live.fovRad, source: 'view', message: 'Drone placed at the current view.',
          } : {
            worldOrigin: clone(DEMO_AIRCRAFT_ORIGIN), positionMetersFRD: [-40, 0, -12], orientationCameraToAnchorXYZW: M.quatFromHpb(0, -16.7, 0).map(v => round(v, 7)),
            fovRad: round(M.degToRad(62), 5), source: 'behindAircraft', message: 'No Camera Studio view was on screen: drone placed behind and above the aircraft.', world: worldOf(DEMO_AIRCRAFT_ORIGIN, [-40, 0, -12]),
          });
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
        case 'StartControllerCapture':
          m.controllerCapture = {deviceId:payload.deviceId,captureId:payload.captureId,uiInstanceId:env.uiInstanceId};
          ack(env,'Accepted'); return event('ControllerCaptureStatus',{state:'listening',deviceId:payload.deviceId,captureId:payload.captureId},env.uiInstanceId);
        case 'StopControllerCapture':
          m.controllerCapture = null; return ack(env,'Completed');
        case 'StartAxisCapture': {
          // Demo: a gamepad's left stick, right stick and combined triggers, as SimConnect names them.
          const demoAxes = { forward: ['YAxis', 'Y axis', 0, -32767], right: ['XAxis', 'X axis', 0, 32767], climb: ['ZAxis', 'Z axis', 0, -32767],
            descend: ['ZAxis', 'Z axis', 0, 32767], yaw: ['RxAxis', 'X rotation', 0, 32767], pitch: ['RyAxis', 'Y rotation', 0, -32767] }[payload.function];
          if (!demoAxes) return ack(env, 'Rejected', 'Choose a drone motion to assign.');
          ack(env, 'Accepted', null, { seconds: 15 });
          event('AxisCaptureStatus', { state: 'listening', deviceId: payload.deviceId, captureId: payload.captureId }, env.uiInstanceId);
          return event('ControllerAxisCaptured', { captureId: payload.captureId, axis: { function: payload.function, definition: 'joystick:' + payload.deviceId + ':' + demoAxes[0],
            label: 'Xbox Controller - ' + demoAxes[1], rest: demoAxes[2], full: demoAxes[3], deviceId: payload.deviceId, deviceName: 'Xbox Controller' } }, env.uiInstanceId, 900);
        }
        case 'StopAxisCapture': return ack(env, 'Completed');
        case 'TestInput': {
          ack(env, 'Accepted');
          const definition = payload.input && payload.input.definition;
          event('InputTest', { definition, phase: 'down' }, env.uiInstanceId, 800);
          return event('InputTest', { definition, phase: 'up' }, env.uiInstanceId, 1150);
        }
        case 'TestBinding': return testBinding(env, payload.binding);
        case 'RelaySurface': ack(env, 'Completed'); return event('SurfaceRequest', { target: payload.target, op: payload.op });
        case 'PointerOverPanel':
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
        case 'SaveTemporaryAirport': {
          if (!demoAirport.transient || payload.icao !== demoAirport.icao || !airportLoaded()) return ack(env, 'Rejected', 'These airport cameras are already saved.');
          const revisions = { draft: m.working.draftRevision };
          ack(env, 'Pending', null, { revisions });
          return later(() => {
            // Same IDs: the active view and any view bindings carry over.
            demoAirport.transient = false; demoAirport.saved = clone(demoAirport.profile);
            event('CommitCompleted', { requestId: env.requestId, kind: 'airport', status: 'Committed', revisions, message: null, report: { icao: demoAirport.icao, temporary: true } });
            broadcastSnapshot();
          }, 300, true);
        }
        case 'GetDiagnostics':
          return ack(env, 'Completed', null, { text: ['FS Camera Studio demo module', 'moduleBootId: ' + m.bootId, 'sessionEpoch: ' + m.epoch, 'session: ' + m.session, 'ownership: ' + m.ownership.state, 'working profile: ' + m.working.profile.name + ' (draft ' + m.working.draftRevision + ')', 'instances: ' + m.instances.size].join('\n') });
        default: return ack(env, 'Rejected', 'unknownAction');
      }
    }

    openWorking(mine.profileId, false);

    const controls = {
      captureControllerButton(deviceId, button, delayedCaptureId) {
        const capture=m.controllerCapture; if(!capture || capture.deviceId!==deviceId || capture.hatDirection)return false;
        if(!delayedCaptureId)m.controllerCapture=null; const name=deviceId===1?'T.16000M':'TWCS Throttle';
        event('ControllerInputCaptured',{captureId:delayedCaptureId || capture.captureId,input:{kind:'joystick',deviceId,deviceName:name,definition:'joystick:'+deviceId+':button:'+(button-1),label:name+' - Button '+button}},capture.uiInstanceId); return true;
      },
      captureControllerHat(deviceId, angle, delayedCaptureId) {
        const capture=m.controllerCapture; if(!capture || capture.deviceId!==deviceId)return false;
        if (Number.isFinite(angle) && angle>=0 && angle<36000) {
          if (!capture.hatDirection) capture.hatDirection=['up','right','down','left'][Math.floor(((angle+4500)%36000)/9000)];
          return false;
        }
        if (!capture.hatDirection) return false;
        const direction=capture.hatDirection,name=deviceId===1?'T.16000M':'TWCS Throttle';
        if(!delayedCaptureId)m.controllerCapture=null;
        event('ControllerInputCaptured',{captureId:delayedCaptureId || capture.captureId,input:{kind:'joystick',deviceId,deviceName:name,definition:'joystick:'+deviceId+':POV:0',pov:direction,label:name+' - Hat 1 '+direction}},capture.uiInstanceId); return true;
      },
      controllerCaptureStatus(state) { const capture=m.controllerCapture;if(!capture)return; m.controllerCapture=null;event('ControllerCaptureStatus',{state,deviceId:capture.deviceId,captureId:capture.captureId},capture.uiInstanceId); },
      // spotting: true for a block that leaves spotting available, like a missing camera profile (the default for such a reason).
      setCanActivate(on, reason, spotting) {
        m.forcedBlock = on ? null : (reason || 'Camera control is turned off for testing.');
        m.forcedSpotting = !on && (spotting === undefined ? /camera profile/i.test(m.forcedBlock) : Boolean(spotting));
        event('CapabilitiesChanged', capabilities());
      },
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
      // Taxi to (or away from) the demo airport: within its radius the module shows its temporary airport set.
      arriveAtAirport(nearby) {
        const next = nearby !== false;
        if (demoAirport.nearby === next) return airportsPayload();
        if (!next && m.active.cameraId && isDemoAirport({ airportIcao: (workingCamera(m.active.cameraId) || {}).airportIcao })) release(null, 'airportUnloaded');
        demoAirport.nearby = next; m.working.draftRevision++;
        if (!workingCamera(m.selection.cameraId)) m.selection = { profileId: m.working.profile.profileId, cameraId: m.working.profile.defaultCameraId };
        broadcastSnapshot();
        return airportsPayload();
      },
      binoculars(down) { setBinoculars(Boolean(down) && m.ownership.state === 'Active'); return Boolean(m.binoculars); },
      // Spotting traffic: { enabled: false } empties the list; { time: seconds } freezes the paths, { time: null } resumes.
      traffic(options) {
        const o = options || {};
        if (o.enabled !== undefined) m.trafficEnabled = Boolean(o.enabled);
        if (o.time !== undefined) m.trafficTime = o.time === null ? null : Number(o.time);
        trafficTick(true);
        return clone(m.traffic);
      },
      // The saved spotting spots as stored (host.spottingSpots, saved at once), and how many AdjustSpot edits were applied.
      get savedSpots() { return clone(repo.settings.host.spottingSpots || []); },
      get spotAdjusts() { return m.spotAdjusts || 0; },
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
