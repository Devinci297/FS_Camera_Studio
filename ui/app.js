/*
 * FS Camera Studio — in-sim panel application (doc/protocol.md v2).
 *
 * The camera module owns the working draft; this page is a mirror. It renders from `Snapshot`, applies
 * `DraftChanged.ops`, and sends edits as coalesced `UpdateDraft` ops. Save = `CommitChanges`, Revert =
 * `RevertChanges`. Selection is shared between surfaces through `SelectCamera` / `SelectionChanged`.
 *
 * Surfaces (?surface=): main (editor toolbar panel), mini (dock-only toolbar panel), none (browser preview
 * with the mockup's simulated cockpit around the editor). Layout, class names and interactions follow the
 * approved mockup.
 */
(function () {
  'use strict';

  const M = window.FSCMath;
  const icon = window.FSCIcons.icon;
  const root = document.getElementById('mc-sim');

  /* ───────────── Surface ───────────── */
  // Packaged panel pages set window.FSCS_SURFACE (toolbar URLs may drop query strings); browsers use ?surface=.
  const forcedSurface = typeof window.FSCS_SURFACE === 'string' && /^(main|mini)$/.test(window.FSCS_SURFACE) ? window.FSCS_SURFACE : null;
  const surfaceMatch = /[?&]surface=(main|mini)\b/.exec(window.location.search || '');
  const SURFACE = forcedSurface || (surfaceMatch ? surfaceMatch[1] : 'demo');
  const IS_MINI = SURFACE === 'mini';
  const bridge = window.FSCBridge.create({ surface: IS_MINI ? 'mini' : 'main' });
  root.setAttribute('data-surface', SURFACE);
  if (SURFACE !== 'demo') {
    root.classList.remove('demo');
    document.body.className = 'surface-' + SURFACE;
    document.documentElement.className = 'surface-' + SURFACE;
  }
  root.dataset.mode = SURFACE === 'main' ? 'edit' : SURFACE === 'mini' ? 'mini' : 'fly';

  // Tell the module while a text field has focus so bindings (except Release Camera) stay quiet while typing.
  let textFocus = false;
  const isTextEntry = el => !!el && (el.tagName === 'TEXTAREA' ||
    (el.tagName === 'INPUT' && ['text', 'search', 'number'].indexOf(el.type) >= 0) || el.isContentEditable);
  const reportTextFocus = () => {
    const now = isTextEntry(document.activeElement);
    if (now !== textFocus) { textFocus = now; bridge.send('TextFocus', { focused: now }); }
  };
  document.addEventListener('focusin', reportTextFocus, true);
  document.addEventListener('focusout', () => setTimeout(reportTextFocus, 0), true);

  /* ───────────── DOM helpers (Coherent GT safe) ───────────── */
  const $ = id => document.getElementById(id);
  const $$ = (selector, scope) => Array.prototype.slice.call((scope || root).querySelectorAll(selector));
  const clone = value => (value === undefined ? undefined : JSON.parse(JSON.stringify(value)));
  const same = (a, b) => JSON.stringify(a) === JSON.stringify(b);
  const pad2 = n => (n < 10 ? '0' : '') + n;
  const fmt = (value, digits) => { const t = Number(value).toFixed(digits); return /^-0(\.0+)?$/.test(t) ? t.slice(1) : t; };
  const fmtFov = deg => String(Math.round(deg * 10) / 10);
  function empty(el) { while (el.firstChild) el.removeChild(el.firstChild); return el; }
  function add(parent) { for (let i = 1; i < arguments.length; i++) { const c = arguments[i]; if (c === null || c === undefined) continue; parent.appendChild(typeof c === 'string' ? document.createTextNode(c) : c); } return parent; }
  function make(tag, text, className) {
    const el = document.createElement(tag);
    if (text !== undefined && text !== null) el.textContent = text;
    if (className) el.className = className;
    return el;
  }
  function button(label, action, className, iconName) {
    const el = make('button', undefined, className || '');
    el.type = 'button';
    el.classList.add('cursor-interaction');
    if (iconName) el.appendChild(icon(iconName));
    if (label) el.appendChild(make('span', label));
    if (action) el.addEventListener('click', action);
    return el;
  }
  function setIcon(control, name) {
    const svg = control.querySelector('svg');
    if (svg && svg.getAttribute('data-icon') === name) return;
    const next = icon(name);
    if (svg) svg.parentNode.replaceChild(next, svg); else control.insertBefore(next, control.firstChild);
  }
  function setLabel(control, text) { const span = control.querySelector('span'); if (span) span.textContent = text; }
  function keepFocus(container, render) {
    const active = document.activeElement;
    const key = active && container.contains(active) ? active.getAttribute('data-focus-key') : null;
    render();
    if (key) { const target = $$('[data-focus-key]', container).filter(el => el.getAttribute('data-focus-key') === key)[0]; if (target) target.focus(); }
  }
  const HAS_POINTER = typeof window.PointerEvent === 'function';
  const POINTER_FALLBACK = { pointerenter: 'mouseenter', pointerleave: 'mouseleave', pointermove: 'mousemove', pointerdown: 'mousedown', pointerup: 'mouseup', pointercancel: null, lostpointercapture: null };
  function onPointer(target, type, handler, capture) {
    const name = HAS_POINTER ? type : POINTER_FALLBACK[type];
    if (name) target.addEventListener(name, handler, Boolean(capture));
  }
  const pointerType = event => event.pointerType || 'mouse';
  function matchesFocusVisible(el) { try { return el.matches(':focus-visible'); } catch (error) { return true; } }
  const reducedMotion = () => Boolean(S.working && S.working.settings.ui.reducedMotion) || Boolean(window.matchMedia && window.matchMedia('(prefers-reduced-motion: reduce)').matches);

  /* ───────────── Constants ───────────── */
  const NOT_AVAILABLE = 'Not available in this version — saved with the profile but cannot be activated.';
  const EXTERIOR_NOTE = 'Exterior views keep cockpit sound in the current simulator build.';
  const START_TEXT = 'Starting Camera Studio…';
  const START_FAILED_TEXT = "Camera Studio's camera module did not start — reload the panel or restart the flight.";
  const STATUS_TEXT = {
    Observing: 'Simulator in control', Acquiring: 'Taking camera control…', Active: 'Camera active',
    SimulatorSuspended: 'Paused by the simulator', OtherClient: 'Another add-on has control',
    UserDisabled: 'Camera control by add-ons is disabled in the simulator', Releasing: 'Returning camera to the simulator…',
    Faulted: "Camera stopped after an error — use the simulator's camera controls, then Enable",
  };
  const MODE_TEXT = {
    aircraftCockpit: { reference: 'Aircraft relative · meters', blurb: 'An interior view, ready to fine-tune.' },
    aircraftExterior: { reference: 'Aircraft relative · meters', blurb: 'Your aircraft from another perspective.' },
    orbit: { reference: 'Aircraft relative · meters', blurb: 'A moving view around your aircraft.' },
    flyby: { reference: 'Aircraft relative · meters', blurb: 'A fixed perspective as the aircraft passes.' },
    airport: { reference: 'Airport local · meters', blurb: 'A view anchored around the airport.' },
    free: { reference: 'Scene origin · meters', blurb: 'Position this view freely.' },
  };
  const REASONS = {
    staleEpoch: 'The flight changed. Try again.', staleBoot: 'Camera Studio restarted. Try again.', staleDraft: 'The editor was refreshed. Try again.',
    unknownCamera: 'That camera is no longer in this profile.', modeNotSupported: NOT_AVAILABLE, notActive: 'Show this camera in the sim first.',
    noActiveCamera: 'No camera is active yet.', unsavedChanges: 'This profile has unsaved camera changes.', commitPending: 'Wait for the current save to finish.',
    profileInUse: 'You cannot delete the profile that is open.', unknownProfile: 'That profile no longer exists.', noAircraft: 'Load a flight first.',
    unknownRevision: 'That backup is no longer available.', notConnected: START_TEXT, timeout: 'Camera Studio did not answer in time. Try again.',
    notCurrentPeek: null, qualificationNotStarted: 'The axis check is not running. Start it again from Settings.', unknownAxis: 'Camera Studio does not know that axis.',
  };
  const reasonText = reason => (reason && Object.prototype.hasOwnProperty.call(REASONS, reason) ? REASONS[reason] : (reason || 'Camera Studio could not do that.'));
  const PRECISION = { fine: [0.001, 0.1], normal: [0.01, 1], coarse: [0.1, 5] };
  const UI_FOV = [30, 110];
  const DEFAULT_FOV_RAD = 1.0821;
  const COMMANDS = [
    ['nextCamera', 'Next camera', 'Follow the current category order'],
    ['previousCamera', 'Previous camera', 'Step back in the current category'],
    ['nextCategory', 'Next category', 'Jump to the next category'],
    ['returnToPilot', 'Return to default view', 'The starting view for this profile'],
    ['releaseCamera', 'Release camera control', 'Give control back to the simulator'],
    ['recenterLook', 'Recenter look', 'Clear the temporary mouse look'],
    ['toggleEditor', 'Toggle editor', 'Open or hide the Camera Studio editor'],
    ['toggleMini', 'Toggle mini control panel', 'Show or hide the quick-access dock'],
  ];
  const MANUAL = [['lookLeft', 'Look left'], ['lookRight', 'Look right'], ['lookUp', 'Look up'], ['lookDown', 'Look down'], ['moveForward', 'Move forward'], ['moveBack', 'Move back'], ['moveLeft', 'Move left'], ['moveRight', 'Move right'], ['moveUp', 'Move up'], ['moveDown', 'Move down'], ['zoomIn', 'Zoom in'], ['zoomOut', 'Zoom out']];
  const ACTION_NAMES = {};
  COMMANDS.concat(MANUAL).forEach(entry => { ACTION_NAMES[entry[0]] = entry[1]; });
  const actionName = (action, viewId) => (action === 'selectView' ? 'Select view ID ' + viewId : action === 'peekView' ? 'Peek at view ID ' + viewId : (ACTION_NAMES[action] || action));

  /* ───────────── State (mirror of the module) ───────────── */
  const S = {
    snapshot: false, failedStart: false,
    session: 'Initializing', aircraft: null,
    ownership: { state: 'Observing', gameControlled: true, reason: null },
    caps: { canActivate: false, blockReason: null, modes: {}, fovRangeDeg: null, features: {} },
    active: { profileId: null, cameraId: null, peeking: false, transitioning: false, fromDraft: false },
    selection: { profileId: null, cameraId: null },
    profiles: [], working: null, pendingCommit: null, look: null, userReleased: false, fault: null,
    page: 'cameras', tab: 'position', precision: 'normal', expanded: {}, angleCache: null,
    queue: [], inflight: null, flushTimer: 0, lastFlush: 0, idleWaiters: [], commitWaiters: {},
    pendingCapture: null, dismissedAircraftKey: null, miniPicking: false, editTimer: 0,
    assign: null, manualOpen: false, peekToken: null,
  };
  const W = () => S.working;
  const P = () => (S.working ? S.working.profile : null);
  const camIn = id => (P() && id ? P().cameras.filter(c => c.id === id)[0] || null : null);
  const catIn = id => (P() ? P().categories.filter(c => c.id === id)[0] || null : null);
  const categoryOf = id => (P() ? P().categories.filter(c => c.cameraIds.indexOf(id) >= 0)[0] || null : null);
  const cameras = () => (P() ? P().categories.reduce((all, g) => all.concat(g.cameraIds.map(camIn).filter(Boolean)), []) : []);
  const current = () => camIn(S.selection.cameraId) || camIn(P() && P().defaultCameraId) || cameras()[0] || null;
  const category = () => { const c = current(); return c ? categoryOf(c.id) : null; };
  const readOnly = () => Boolean(P() && P().requiredSchemaFeatures && P().requiredSchemaFeatures.length) || Boolean(currentSummary() && currentSummary().readOnly);
  const currentSummary = () => (P() ? S.profiles.filter(p => p.profileId === P().profileId)[0] || null : null);
  const localPending = () => S.queue.length > 0 || Boolean(S.inflight);
  const dirtyFlags = () => (S.working ? S.working.dirty || {} : {});
  const isDirty = () => Boolean(dirtyFlags().profile || dirtyFlags().settings || dirtyFlags().bindings) || localPending();
  const hasCommitted = id => Boolean(S.working && S.working.hasCommittedVersion && S.working.hasCommittedVersion[id]);
  const engaged = () => S.ownership.state === 'Active' || S.ownership.state === 'Acquiring';
  const isLive = id => Boolean(id) && S.ownership.state === 'Active' && S.active.cameraId === id && (!S.active.profileId || !P() || S.active.profileId === P().profileId);

  /* ───────────── Draft ops (mirror + coalesced UpdateDraft) ───────────── */
  function applyOpsLocal(ops) {
    const w = S.working;
    ops.forEach(op => {
      if (op.op === 'setCamera') {
        const i = w.profile.cameras.map(c => c.id).indexOf(op.camera.id);
        if (i >= 0) w.profile.cameras[i] = clone(op.camera); else w.profile.cameras.push(clone(op.camera));
      } else if (op.op === 'removeCamera') {
        w.profile.cameras = w.profile.cameras.filter(c => c.id !== op.cameraId);
        w.profile.categories.forEach(g => { g.cameraIds = g.cameraIds.filter(id => id !== op.cameraId); });
      } else if (op.op === 'setCategories') w.profile.categories = clone(op.categories);
      else if (op.op === 'setProfileFields') ['name', 'defaultCameraId', 'aircraftMatch'].forEach(k => { if (op[k] !== undefined) w.profile[k] = clone(op[k]); });
      else if (op.op === 'setSettings') w.settings = clone(op.settings);
      else if (op.op === 'setBindings') w.bindings = clone(op.bindings);
    });
  }
  /** Queue ops already applied to the mirror; only the latest state of each target is kept. */
  function queueOps(ops) {
    if (!S.working || readOnly()) return;
    ops.forEach(op => {
      const key = op.op === 'setCamera' ? 'cam:' + op.camera.id : op.op === 'removeCamera' ? 'cam:' + op.cameraId : op.op;
      if (op.op === 'setProfileFields') {
        const existing = S.queue.filter(q => q.op === 'setProfileFields')[0];
        if (existing) { Object.assign(existing, clone(op)); return; }
      }
      S.queue = S.queue.filter(q => q._key !== key);
      const copy = clone(op);
      Object.defineProperty(copy, '_key', { value: key, enumerable: false });
      S.queue.push(copy);
    });
    scheduleFlush();
    updateSavedState();
  }
  function scheduleFlush(immediate) {
    if (S.flushTimer) { if (!immediate) return; clearTimeout(S.flushTimer); }
    const wait = immediate ? 0 : Math.max(0, 100 - (Date.now() - S.lastFlush));
    S.flushTimer = setTimeout(flush, wait);
  }
  function flush() {
    S.flushTimer = 0;
    if (S.inflight || !S.queue.length || !S.snapshot) { if (!S.inflight && !S.queue.length) settleIdle(); return; }
    const ops = S.queue;
    S.queue = [];
    S.inflight = ops;
    S.lastFlush = Date.now();
    bridge.context.draftRevision = S.working.draftRevision;
    bridge.request('UpdateDraft', { ops: ops.map(op => clone(op)) }).then(ack => {
      S.inflight = null;
      if (ack.status === 'Completed') {
        if (ack.result && typeof ack.result.draftRevision === 'number') setDraftRevision(ack.result.draftRevision);
      } else if (ack.reason === 'staleDraft' || ack.reason === 'staleBoot') {
        S.queue = ops.concat(S.queue);
        bridge.send('RequestSnapshot', {});
        return;
      } else {
        toast(reasonText(ack.reason), 'warn');
        bridge.send('RequestSnapshot', {});
      }
      if (S.queue.length) scheduleFlush(); else settleIdle();
      updateSavedState();
    });
  }
  function settleIdle() { if (localPending()) return; const waiters = S.idleWaiters; S.idleWaiters = []; waiters.forEach(fn => fn()); }
  function whenIdle() {
    return new Promise(resolve => {
      if (!localPending()) { resolve(); return; }
      S.idleWaiters.push(resolve);
      scheduleFlush(true);
    });
  }
  function setDraftRevision(rev) {
    if (!S.working) return;
    if (rev > S.working.draftRevision) S.working.draftRevision = rev;
    bridge.context.draftRevision = S.working.draftRevision;
  }
  const saveCamera = cam => queueOps([{ op: 'setCamera', camera: cam }]);
  const saveCategories = extra => queueOps((extra || []).concat([{ op: 'setCategories', categories: P().categories }]));
  const saveSettings = () => queueOps([{ op: 'setSettings', settings: S.working.settings }]);
  const saveBindings = () => queueOps([{ op: 'setBindings', bindings: S.working.bindings }]);

  /** Send a camera command after pending edits reached the module (recall uses the working draft). */
  function command(action, payload, quietReasons) {
    return whenIdle().then(() => bridge.request(action, payload)).then(ack => {
      if ((ack.status === 'Rejected' || ack.status === 'TimedOut') && !(quietReasons && quietReasons.indexOf(ack.reason) >= 0)) {
        const text = reasonText(ack.reason);
        if (text) toast(text, 'warn');
        if (ack.reason === 'staleEpoch' || ack.reason === 'staleBoot') bridge.send('RequestSnapshot', {});
      }
      return ack;
    });
  }

  /* ───────────── Snapshot & events ───────────── */
  function applySnapshot(p) {
    const firstSnapshot = !S.snapshot;
    S.snapshot = true;
    S.failedStart = false;
    bridge.context.moduleBootId = p.moduleBootId;
    bridge.context.sessionEpoch = p.sessionEpoch;
    S.session = p.session || 'Ready';
    S.aircraft = p.aircraft || null;
    S.ownership = p.ownership || S.ownership;
    S.caps = p.capabilities || S.caps;
    S.active = p.activeCamera || S.active;
    S.selection = p.selection || S.selection;
    S.profiles = p.profiles || [];
    S.working = p.working;
    S.pendingCommit = p.pendingCommit || null;
    S.look = p.look || null;
    bridge.context.draftRevision = S.working.draftRevision;
    // Edits that were not yet accepted are re-applied on top of the fresh snapshot and sent again.
    if (S.inflight) { S.queue = S.inflight.concat(S.queue); S.inflight = null; }
    if (S.queue.length) { applyOpsLocal(S.queue); scheduleFlush(true); }
    S.angleCache = null;
    const group = category();
    if (group) S.expanded[group.id] = true;
    if (firstSnapshot) {
      setPrecision(S.working.settings.ui.precision || 'normal');
      if (S.working.settings.ui.miniPinned && !IS_MINI) setMiniPinned(true, false);
    }
    applySettings();
    render();
    renderVisiblePage();
  }
  const EVENTS = {
    Snapshot: applySnapshot,
    SessionChanged(p) {
      const reboot = p.moduleBootId && p.moduleBootId !== bridge.context.moduleBootId;
      if (p.sessionEpoch !== undefined) bridge.context.sessionEpoch = p.sessionEpoch;
      S.session = p.session || S.session;
      if (reboot) { bridge.context.moduleBootId = p.moduleBootId; bridge.send('RequestSnapshot', {}); }
      render();
    },
    AircraftChanged(p) {
      const key = S.aircraft ? S.aircraft.modelKey : null;
      S.aircraft = p;
      if ((p && p.modelKey) !== key) S.dismissedAircraftKey = null;
      render();
      if (S.page === 'profiles') renderProfiles();
    },
    OwnershipChanged(p) {
      S.ownership = p;
      if (p.state === 'Active' || p.state === 'Acquiring') S.userReleased = false;
      if (p.state === 'Observing' && p.reason === 'userReleased') S.userReleased = true;
      if (p.state !== 'Active') S.look = null;
      render();
    },
    CapabilitiesChanged(p) { S.caps = p; render(); if (S.page === 'settings') renderSettings(); },
    ActiveCameraChanged(p) { S.active = p; render(); },
    SelectionChanged(p) {
      if (P() && p.profileId && p.profileId !== P().profileId) return;
      if (S.selection.cameraId !== p.cameraId) { S.selection = p; S.angleCache = null; const g = category(); if (g) S.expanded[g.id] = true; render(); }
    },
    DraftChanged(p) {
      if (!S.working) return;
      if (p.origin !== bridge.uiInstanceId) {
        applyOpsLocal(p.ops || []);
        if (S.inflight) applyOpsLocal(S.inflight);
        if (S.queue.length) applyOpsLocal(S.queue);
        S.angleCache = null;
      }
      setDraftRevision(p.draftRevision);
      S.working.dirty = p.dirty || S.working.dirty;
      if (p.origin !== bridge.uiInstanceId) { applySettings(); render(); renderVisiblePage(); } else updateSavedState();
    },
    CommitCompleted(p) {
      const resolve = S.commitWaiters[p.requestId];
      delete S.commitWaiters[p.requestId];
      if (S.pendingCommit && S.pendingCommit.requestId === p.requestId) S.pendingCommit = null;
      if (resolve) resolve(p);
      else if (p.status === 'Committed') bridge.send('RequestSnapshot', {});
      updateSavedState();
    },
    ProfilesChanged(p) { S.profiles = p.profiles || []; if (S.page === 'profiles') renderProfiles(); renderBanners(); },
    TemporaryLookChanged(p) { S.look = p && p.active ? p : null; renderTempLook(); renderPoseActions(); },
    ViewCaptured: onViewCaptured,
    DefinitionsListed() { /* not used by the editor yet */ },
    ControllersListed(p) { if (S.assign) { S.assign.controllers = p.controllers || []; renderAssignControllers(); } },
    InputTest: onInputTest,
    BindingTriggered(p) { if (S.page === 'bindings') setBindingResult('Triggered “' + actionName(p.action, p.viewId) + '”' + (p.result === 'recalled' ? ' — the view was recalled.' : '.')); },
    SurfaceRequest: onSurfaceRequest,
    FaultRaised(p) { S.fault = p; announce(p.message || 'The camera stopped after an error.', 'error'); render(); },
  };
  bridge.on('event', message => { const handler = EVENTS[message.name]; if (handler) handler(message.payload || {}); });
  bridge.on('ack', ack => {
    if (ack.status === 'Rejected' && ack.reason === 'staleBoot') bridge.send('RequestSnapshot', {});
  });

  /* ───────────── Status & gating ───────────── */
  function statusText() {
    if (!S.snapshot) return S.failedStart ? START_FAILED_TEXT : START_TEXT;
    if (S.ownership.state === 'Faulted' || S.session === 'Faulted') return STATUS_TEXT.Faulted;
    if (S.session === 'Suspended') return STATUS_TEXT.SimulatorSuspended;
    if (S.session !== 'Ready') return START_TEXT;
    if (!S.aircraft || !S.aircraft.title) return 'Load a flight to use your cameras';
    if (S.ownership.state === 'Observing' && S.userReleased) return 'Camera released';
    return STATUS_TEXT[S.ownership.state] || S.ownership.state;
  }
  function controlBlockReason() {
    if (!S.snapshot) return statusText();
    if (readOnly()) return 'This profile needs a newer version of Camera Studio and cannot be activated.';
    if (!S.caps.canActivate) return S.caps.blockReason || 'Camera control is not available right now.';
    return null;
  }
  function modeAvailability(mode) {
    const info = S.caps.modes && S.caps.modes[mode];
    if (info) return { available: Boolean(info.available), reason: info.reason || (info.available ? null : NOT_AVAILABLE) };
    return { available: mode === 'aircraftCockpit' || mode === 'aircraftExterior', reason: mode === 'aircraftCockpit' || mode === 'aircraftExterior' ? null : NOT_AVAILABLE };
  }
  function cameraBlockReason(cam) {
    if (!cam) return 'Choose a camera first.';
    const mode = modeAvailability(cam.mode);
    if (!mode.available) return mode.reason || NOT_AVAILABLE;
    return controlBlockReason();
  }
  function fovRange() {
    const r = S.caps.fovRangeDeg;
    if (r && r.length === 2 && r[0] < r[1]) return [Math.max(UI_FOV[0], Math.ceil(r[0])), Math.min(UI_FOV[1], Math.floor(r[1]))];
    return UI_FOV.slice();
  }
  const levelHorizonAvailable = () => Boolean((S.caps.features && S.caps.features.levelHorizon) || S.caps.levelHorizon === true);

  /* ───────────── Toast ───────────── */
  let toastTimer = 0;
  function announce(message, tone) {
    if (!message) return;
    clearTimeout(toastTimer);
    const el = $('mc-toast');
    $('mc-toast-text').textContent = message;
    el.setAttribute('data-tone', tone || 'ok');
    setIcon(el, !tone || tone === 'ok' ? 'check-circle-2' : 'alert-triangle');
    el.hidden = false;
    toastTimer = setTimeout(() => { el.hidden = true; }, !tone || tone === 'ok' ? 2600 : 4200);
  }
  const toast = announce;

  /* ───────────── Mini dock presentation (mockup §4.5 behaviour) ───────────── */
  // Mini surface presentation: hover (handle + reveal, default) | always (fixed dock) | off (note only).
  let miniMode = 'hover';
  if (IS_MINI) root.dataset.miniMode = miniMode;
  const miniStatic = () => IS_MINI && miniMode !== 'hover';
  let miniPinned = false, miniHovered = false, miniTapped = false, miniKeyboard = false, miniDialogOwned = false, miniHideTimer = 0, miniSuppressFocus = false;
  const miniEnabled = () => !S.working || S.working.settings.ui.miniEnabled !== false;
  const miniHover = () => !S.working || S.working.settings.ui.miniHover !== false;
  function revealMini() {
    if (miniStatic()) return;
    clearTimeout(miniHideTimer);
    root.dataset.miniOpen = 'true';
    $('mc-mini-panel').setAttribute('aria-hidden', 'false');
    $('mc-mini-trigger').setAttribute('aria-expanded', 'true');
    $('mc-mini-trigger').setAttribute('aria-label', miniPinned ? 'Mini control panel pinned open' : 'Hide mini control panel');
  }
  function miniHasKeyboardFocus() { return miniKeyboard && $('mc-mini-region').contains(document.activeElement); }
  function regionHovered() { try { return $('mc-mini-region').matches(':hover'); } catch (error) { return miniHovered; } }
  function hideMini(force) {
    if (miniStatic()) return;
    if (miniHovered && !regionHovered()) miniHovered = false;
    if (!force && (miniPinned || miniHovered || miniTapped || miniDialogOwned || miniHasKeyboardFocus())) return;
    clearTimeout(miniHideTimer);
    if ($('mc-mini-panel').contains(document.activeElement)) document.activeElement.blur();
    root.dataset.miniOpen = 'false';
    $('mc-mini-panel').setAttribute('aria-hidden', 'true');
    $('mc-mini-trigger').setAttribute('aria-expanded', 'false');
    $('mc-mini-trigger').setAttribute('aria-label', 'Show mini control panel');
  }
  function scheduleMiniHide(delay) {
    if (miniStatic()) return;
    clearTimeout(miniHideTimer);
    if (miniHovered && !regionHovered()) miniHovered = false;
    if (miniPinned || miniHovered || miniTapped || miniDialogOwned || miniHasKeyboardFocus()) return;
    miniHideTimer = setTimeout(() => hideMini(), delay === undefined ? 650 : delay);
  }
  function dismissMini(restoreFocus) {
    miniTapped = false;
    miniKeyboard = false;
    if (!miniPinned) hideMini(true);
    if (restoreFocus && !miniPinned) { miniSuppressFocus = true; $('mc-mini-trigger').focus(); miniSuppressFocus = false; }
  }
  function enterFlightMode() {
    if (IS_MINI) return;
    if (S.assign) closeDialog();
    root.dataset.mode = 'fly';
    miniHovered = false; miniTapped = false; miniKeyboard = false;
    if (miniEnabled()) { revealMini(); scheduleMiniHide(1400); }
    sizeMiniClearance();
  }
  function openEditor(page) {
    if (IS_MINI) return;
    root.dataset.mode = 'edit';
    if (page) setPage(page);
    miniHovered = false;
    dismissMini();
    const tab = $('mc-nav-' + S.page);
    if (tab) tab.focus();
    sizeMiniClearance();
  }
  function setMiniPinned(on, persist) {
    miniPinned = Boolean(on);
    root.dataset.miniPinned = String(miniPinned);
    const pin = $('mc-mini-pin');
    pin.setAttribute('aria-pressed', String(miniPinned));
    pin.setAttribute('aria-label', (miniPinned ? 'Unpin' : 'Pin') + ' mini control panel');
    setLabel(pin, miniPinned ? 'Pinned' : 'Pin');
    if (!miniStatic()) { if (miniPinned) revealMini(); else { $('mc-mini-trigger').setAttribute('aria-label', 'Hide mini control panel'); scheduleMiniHide(); } }
    if (persist && S.working && S.working.settings.ui.miniPinned !== miniPinned) {
      S.working.settings.ui.miniPinned = miniPinned;
      saveSettings();
    }
  }
  function sizeMiniClearance() {
    if (SURFACE !== 'demo') return;
    const panel = $('mc-mini-panel');
    const clearance = panel.getBoundingClientRect().height + parseFloat(getComputedStyle(panel).bottom || '36') + 16;
    if (clearance > 16) root.style.setProperty('--mc-mini-clearance', Math.ceil(clearance) + 'px');
  }

  /* ───────────── Dialogs ───────────── */
  let dialogOpener = null, dialogOnClose = null;
  const dialogOpen = () => !$('mc-dialog-shade').hidden;
  function fitDialog() {
    if (SURFACE !== 'demo' || !dialogOpen()) return;
    const height = root.querySelector('.mc-dialog').getBoundingClientRect().height + (root.clientWidth < 621 ? 85 : 125);
    root.style.minHeight = Math.ceil(height) + 'px';
  }
  function openDialog(title, onClose) {
    if (dialogOpen()) closeDialog(true);
    dialogOpener = document.activeElement;
    dialogOnClose = onClose || null;
    miniDialogOwned = !IS_MINI && $('mc-mini-region').contains(dialogOpener);
    if (miniDialogOwned) revealMini();
    $('mc-dialog-title').textContent = title;
    empty($('mc-dialog-body'));
    empty($('mc-dialog-actions'));
    $('mc-dialog-error').textContent = '';
    $('mc-dialog-shade').hidden = false;
    requestAnimationFrame(() => { fitDialog(); if (!$('mc-dialog-shade').contains(document.activeElement)) $('mc-dialog-close').focus(); });
    return $('mc-dialog-body');
  }
  function closeDialog(silent) {
    if (!dialogOpen()) return;
    if (S.peekToken) endPeek();
    const callback = dialogOnClose;
    dialogOnClose = null;
    $('mc-dialog-shade').hidden = true;
    const fromMini = miniDialogOwned;
    miniDialogOwned = false;
    root.style.minHeight = '';
    if (!silent) {
      if (dialogOpener && document.body.contains(dialogOpener)) dialogOpener.focus();
      else if (fromMini) $('mc-quick-picker').focus();
    }
    if (callback) callback();
    if (!silent) scheduleMiniHide();
  }
  function dialogActions(actions) {
    const host = empty($('mc-dialog-actions'));
    actions.forEach(entry => add(host, button(entry[0], entry[1], entry[2] || 'mc-secondary', entry[3])));
    fitDialog();
    requestAnimationFrame(fitDialog);
  }
  const dialogError = text => { $('mc-dialog-error').textContent = text || ''; };
  function field(labelText, initial, type) {
    const label = make('label', labelText, 'mc-field');
    const input = make('input');
    input.type = type || 'text';
    input.value = initial;
    label.appendChild(input);
    return { label, input };
  }
  function namedDialog(title, initial, apply, actionLabel) {
    const body = openDialog(title);
    const name = field('Name', initial);
    name.input.maxLength = 60;
    body.appendChild(name.label);
    const submit = () => {
      const value = name.input.value.trim();
      if (!value) { dialogError('Enter a name to continue.'); name.input.focus(); return; }
      closeDialog();
      apply(value);
    };
    name.input.addEventListener('keydown', e => { if (e.key === 'Enter') { e.preventDefault(); submit(); } });
    dialogActions([['Cancel', () => closeDialog()], [actionLabel || 'Save', submit, 'mc-primary']]);
    requestAnimationFrame(() => { name.input.focus(); name.input.select(); });
  }
  function confirmDialog(title, message, action, actionLabel) {
    const body = openDialog(title);
    body.appendChild(make('p', message));
    const label = actionLabel || 'Delete';
    dialogActions([['Cancel', () => closeDialog()], [label, () => { closeDialog(); action(); }, label === 'Delete' ? 'mc-danger' : 'mc-primary']]);
  }
  function actionDialog(title, items) {
    const body = openDialog(title);
    const list = make('div', undefined, 'mc-action-list');
    items.forEach(item => {
      const entry = button(item[0], () => { closeDialog(); item[2](); }, '', item[1]);
      entry.disabled = Boolean(item[3]);
      list.appendChild(entry);
    });
    body.appendChild(list);
    dialogActions([]);
  }
  function choiceDialog(title, message, choices) {
    return new Promise(resolve => {
      let settled = false;
      const done = value => { if (!settled) { settled = true; resolve(value); } };
      const body = openDialog(title, () => done(null));
      body.appendChild(make('p', message));
      dialogActions(choices.map(c => [c[0], () => { done(c[1]); closeDialog(); }, c[2]]));
    });
  }
  function codeDialog(title, intro, code, extra) {
    const body = openDialog(title);
    const input = make('textarea');
    input.readOnly = true;
    input.setAttribute('aria-label', title);
    input.value = code;
    add(body, make('p', intro), extra || null, input);
    dialogActions([['Close', () => closeDialog()], ['Select profile code', () => { input.focus(); input.select(); announce('Profile code selected. Use your copy command.'); }, 'mc-primary']]);
  }

  /* ───────────── Navigation ───────────── */
  function setPage(page) {
    S.page = page;
    root.dataset.page = page;
    $$('[data-page][role="tab"]').forEach(tab => {
      const on = tab.getAttribute('data-page') === page;
      tab.setAttribute('aria-selected', String(on));
      $('mc-page-' + tab.getAttribute('data-page')).hidden = !on;
    });
    renderVisiblePage();
  }
  function renderVisiblePage() {
    if (S.page === 'profiles') renderProfiles();
    if (S.page === 'bindings') renderBindings();
    if (S.page === 'settings') renderSettings();
  }
  function setTab(tab) {
    S.tab = tab;
    $$('[data-tab]').forEach(control => {
      const on = control.getAttribute('data-tab') === tab;
      control.setAttribute('aria-selected', String(on));
      $('mc-detail-' + control.getAttribute('data-tab')).hidden = !on;
    });
  }

  /* ───────────── Selection & activation ───────────── */
  function selectCamera(id, options) {
    const target = camIn(id);
    if (!target) return;
    if (S.peekToken) endPeek();
    S.selection = { profileId: P().profileId, cameraId: id };
    S.angleCache = null;
    const g = categoryOf(id);
    if (g) S.expanded[g.id] = true;
    root.dataset.browserOpen = 'false';
    $('mc-browser-toggle').setAttribute('aria-expanded', 'false');
    motionToken++;
    $('mc-motion-marker').style.left = '0%';
    render();
    if (!(options && options.local)) whenIdle().then(() => bridge.send('SelectCamera', { cameraId: id }));
  }
  function activateCamera(id) {
    const cam = camIn(id);
    const reason = cameraBlockReason(cam);
    if (reason) { announce(reason, 'warn'); return; }
    if (S.selection.cameraId !== id) selectCamera(id, { local: true });
    command('ActivateCamera', { cameraId: id });
  }
  function toggleControl() {
    if (engaged()) { S.userReleased = true; command('ReleaseCamera', {}); return; }
    const reason = controlBlockReason();
    if (reason) { announce(reason, 'warn'); return; }
    command('EnableCamera', {});
  }
  function cycleCamera(delta) {
    const reason = controlBlockReason();
    if (reason) { announce(reason, 'warn'); return; }
    command('CycleCamera', { delta: delta > 0 ? 1 : -1 });
  }

  /* ───────────── Camera library ───────────── */
  function setFavorite(view) {
    if (readOnly()) return;
    view.favorite = !view.favorite;
    saveCamera(view);
    renderTree(); renderDetails(); renderDock();
  }
  function renderTree() {
    const tree = $('mc-camera-tree');
    if (!P()) { empty(tree); return; }
    keepFocus(tree, () => {
      empty(tree);
      const query = $('mc-search').value.trim().toLowerCase();
      let visible = 0;
      const ro = readOnly();
      P().categories.forEach(group => {
        const views = group.cameraIds.map(camIn).filter(Boolean);
        const matches = views.filter(view => !query || view.name.toLowerCase().indexOf(query) >= 0 || group.name.toLowerCase().indexOf(query) >= 0);
        if (!matches.length) return;
        visible += matches.length;
        const section = make('section', undefined, 'mc-category');
        const open = Boolean(query) || Boolean(S.expanded[group.id]);
        const head = button('', () => { S.expanded[group.id] = !S.expanded[group.id]; renderTree(); }, 'mc-category-head');
        add(head, icon(open ? 'chevron-down' : 'chevron-right'), make('span', group.name), make('span', String(views.length), 'mc-count'));
        head.setAttribute('aria-expanded', String(open));
        head.setAttribute('data-focus-key', 'cat:' + group.id);
        head.addEventListener('contextmenu', e => { e.preventDefault(); if (!ro) categoryActions(group); });
        const heading = make('div', undefined, 'mc-category-line');
        const menu = button('', () => categoryActions(group), 'mc-category-menu', 'ellipsis');
        menu.setAttribute('aria-label', group.name + ' category actions');
        menu.setAttribute('data-focus-key', 'catmenu:' + group.id);
        menu.disabled = ro;
        add(heading, head, menu);
        section.appendChild(heading);
        if (open) {
          const rows = make('div', undefined, 'mc-camera-rows');
          matches.forEach(view => rows.appendChild(cameraRow(view, group, ro)));
          section.appendChild(rows);
        }
        tree.appendChild(section);
      });
      if (!visible) tree.appendChild(make('div', 'No cameras match your search.', 'mc-empty'));
    });
  }
  function cameraRow(view, group, ro) {
    const row = make('div', undefined, 'mc-camera-row');
    const sel = current();
    row.setAttribute('data-current', String(Boolean(sel) && view.id === sel.id));
    const choose = button(view.name, () => selectCamera(view.id), 'mc-camera-select');
    choose.setAttribute('data-focus-key', 'cam:' + view.id);
    choose.setAttribute('aria-pressed', String(Boolean(sel) && view.id === sel.id));
    let label = view.name;
    if (isLive(view.id)) { choose.appendChild(make('span', 'Live', 'mc-live')); label += ', showing in the simulator'; }
    if (view.viewId) { choose.appendChild(make('span', String(view.viewId), 'mc-key')); label += ', view ID ' + view.viewId; }
    if (!hasCommitted(view.id)) label += ', unsaved';
    choose.setAttribute('aria-label', label);
    choose.addEventListener('dblclick', () => { if (!ro) renameCamera(view); });
    choose.addEventListener('contextmenu', e => { e.preventDefault(); selectCamera(view.id); if (!ro) cameraActions(); });
    if (!ro) {
      choose.draggable = true;
      choose.addEventListener('dragstart', e => { e.dataTransfer.setData('text/plain', view.id); e.dataTransfer.effectAllowed = 'move'; });
      choose.addEventListener('dragover', e => e.preventDefault());
      choose.addEventListener('drop', e => {
        e.preventDefault();
        const sourceId = e.dataTransfer.getData('text/plain');
        const from = group.cameraIds.indexOf(sourceId), to = group.cameraIds.indexOf(view.id);
        if (from < 0 || from === to) return;
        const moved = group.cameraIds.splice(from, 1)[0];
        group.cameraIds.splice(to, 0, moved);
        saveCategories();
        render();
      });
    }
    const favorite = button('', () => setFavorite(view), 'mc-row-star', 'star');
    favorite.setAttribute('aria-label', (view.favorite ? 'Remove ' : 'Add ') + view.name + (view.favorite ? ' from' : ' to') + ' quick access');
    favorite.setAttribute('aria-pressed', String(Boolean(view.favorite)));
    favorite.setAttribute('data-focus-key', 'star:' + view.id);
    favorite.disabled = ro;
    add(row, choose, favorite);
    return row;
  }

  /* ───────────── Inspector ───────────── */
  function renderDetails() {
    const view = current();
    const hasProfile = Boolean(view);
    $('mc-cameras-empty').hidden = hasProfile;
    $('mc-camera-layout').hidden = !hasProfile;
    $('mc-browser-toggle').hidden = !hasProfile;
    if (!hasProfile) {
      root.querySelector('#mc-cameras-empty h3').textContent = S.snapshot ? 'No cameras in this profile' : statusText();
      $('mc-cameras-empty-text').textContent = S.snapshot ? 'Add a camera to start building views for this aircraft.' : 'Your cameras appear as soon as the camera module answers.';
      return;
    }
    const group = category();
    const ro = readOnly();
    const modeText = MODE_TEXT[view.mode] || { reference: 'Meters', blurb: '' };
    const availability = modeAvailability(view.mode);
    $('mc-reference-label').textContent = modeText.reference;
    $('mc-camera-name').textContent = view.name;
    $('mc-camera-path').textContent = (group ? group.name : '') + (view.viewId ? ' / VIEW ' + pad2(view.viewId) : ' / CAMERA');
    let description = view.id === P().defaultCameraId ? 'Your default view for this profile.' : modeText.blurb;
    if (!availability.available) description = availability.reason || NOT_AVAILABLE;
    else if (!hasCommitted(view.id)) description += ' New camera — unsaved.';
    $('mc-camera-description').textContent = description;
    $('mc-favorite').setAttribute('aria-pressed', String(Boolean(view.favorite)));
    $('mc-favorite').setAttribute('aria-label', (view.favorite ? 'Remove camera from' : 'Add camera to') + ' quick access');
    $('mc-favorite').disabled = ro;
    $('mc-camera-menu').disabled = ro;
    renderViewButton();
    renderTempLook();
    $$('[data-pose]').forEach(input => { if (document.activeElement !== input) input.value = formatPose(input.getAttribute('data-pose'), poseValue(view, input.getAttribute('data-pose'))); });
    const range = fovRange();
    const deg = M.radToDeg(view.projection.value);
    $('mc-fov-number').min = String(range[0]); $('mc-fov-number').max = String(range[1]);
    $('mc-fov-range').min = String(range[0]); $('mc-fov-range').max = String(range[1]);
    $('mc-fov-min-label').textContent = range[0] + '° · close';
    $('mc-fov-max-label').textContent = range[1] + '° · wide';
    if (document.activeElement !== $('mc-fov-number')) $('mc-fov-number').value = fmtFov(deg);
    $('mc-fov-range').value = String(M.clamp(deg, range[0], range[1]));
    $$('[data-fov]').forEach(control => {
      const preset = Number(control.getAttribute('data-fov'));
      control.setAttribute('aria-pressed', String(Math.abs(preset - deg) < 0.25));
      control.disabled = ro || preset < range[0] || preset > range[1];
    });
    renderPoseActions();
    // Motion
    $$('[data-transition]').forEach(control => control.setAttribute('aria-pressed', String(control.getAttribute('data-transition') === view.transition.kind)));
    if (document.activeElement !== $('mc-duration')) $('mc-duration').value = String(view.transition.durationSeconds);
    $('mc-duration').disabled = ro || view.transition.kind === 'cut';
    if (document.activeElement !== $('mc-delay')) $('mc-delay').value = String(view.transition.delaySeconds);
    $('mc-motion-destination').textContent = view.name;
    // Behavior
    $('mc-view-type').value = view.mode;
    $('mc-mode-pill').hidden = view.mode !== 'aircraftExterior';
    const note = $('mc-mode-note');
    note.hidden = availability.available && view.mode !== 'aircraftExterior';
    note.textContent = view.mode === 'aircraftExterior' ? EXTERIOR_NOTE : (availability.reason || NOT_AVAILABLE);
    note.className = availability.available ? '' : 'mc-warn';
    $('mc-view-id').value = String(view.viewId || 0);
    $('mc-default-view').checked = P().defaultCameraId === view.id;
    $('mc-quick-access').checked = Boolean(view.favorite);
    $('mc-retain-look').checked = view.temporaryLookPolicy === 'retain';
    const level = levelHorizonAvailable();
    $('mc-level').checked = Boolean(view.levelHorizon);
    $('mc-level').disabled = ro || !level;
    $('mc-level-note').textContent = level ? 'Stabilize roll for this camera.' : 'Not available until qualified.';
    $('mc-interaction').checked = Boolean(view.interaction);
    $('mc-tracking').value = view.trackingPolicy || 'nativeIfQualified';
    $$('#mc-inspector input, #mc-inspector select, #mc-inspector .mc-stepper button, #mc-inspector .mc-select-card button, #mc-inspector .mc-precision button', document).forEach(el => {
      if (el.id === 'mc-tracking' || el.id === 'mc-level' || el.id === 'mc-duration' || el.hasAttribute('data-fov')) return;
      el.disabled = ro;
    });
  }
  function renderViewButton() {
    const control = $('mc-view-in-sim');
    const view = current();
    if (!view) return;
    const live = isLive(view.id);
    const reason = cameraBlockReason(view);
    control.setAttribute('data-live', String(live));
    setIcon(control, live ? 'check-circle-2' : 'play');
    setLabel(control, live ? 'Showing in sim' : 'View in sim');
    control.setAttribute('aria-label', live ? view.name + ' is showing in the simulator. Recall it again' : 'View ' + view.name + ' in the simulator');
    control.disabled = Boolean(reason);
    control.title = reason || '';
  }
  function renderTempLook() {
    const view = current();
    $('mc-templook').hidden = !(view && isLive(view.id) && S.look && S.look.active);
  }
  function renderPoseActions() {
    const view = current();
    if (!view) return;
    const ro = readOnly();
    const live = isLive(view.id);
    $('mc-restore-pose').disabled = ro || !hasCommitted(view.id);
    $('mc-restore-pose').title = hasCommitted(view.id) ? '' : 'Available after this camera is saved.';
    $('mc-center-look').disabled = ro;
    $('mc-bake-look').disabled = ro || !live;
    $('mc-bake-look').title = live ? 'Write the current in-sim view, including mouse look, into this camera' : 'Show this camera in the sim first.';
    $('mc-capture-view').disabled = ro || !S.snapshot || !S.aircraft || !S.aircraft.title;
    $('mc-capture-view').title = 'Read the simulator’s current view and review it before using it';
  }

  /* Pose fields */
  const AXES = [['x', 'X', 'Left / right', 'mc-position-fields'], ['y', 'Y', 'Up / down', 'mc-position-fields'], ['z', 'Z', 'Back / forward', 'mc-position-fields'], ['pitch', '', 'Pitch', 'mc-angle-fields'], ['heading', '', 'Heading', 'mc-angle-fields'], ['bank', '', 'Roll', 'mc-angle-fields']];
  const linear = key => key === 'x' || key === 'y' || key === 'z';
  const formatPose = (key, v) => fmt(v, linear(key) ? 3 : 1);
  function anglesFor(view) {
    const q = view.orientationCameraToAnchorXYZW;
    const c = S.angleCache;
    if (c && c.id === view.id && same(c.q, q)) return c.hpb;
    const hpb = M.hpbFromQuat(q);
    S.angleCache = { id: view.id, q: q.slice(), hpb };
    return hpb;
  }
  const poseValue = (view, key) => (linear(key) ? M.uiFromFrd(view.positionMetersFRD)[key] : anglesFor(view)[key]);
  const limits = key => (linear(key) ? [-10000, 10000] : key === 'pitch' ? [-89, 89] : [-180, 180]);
  function setAxis(key, value) {
    const view = current();
    if (!view || readOnly() || !isFinite(value)) return;
    if (linear(key)) {
      const map = M.UI_AXIS_TO_FRD[key];
      const v = map.sign * M.clamp(value, -10000, 10000);
      view.positionMetersFRD[map.index] = Object.is(v, -0) ? 0 : v;
    } else {
      const hpb = Object.assign({}, anglesFor(view));
      hpb[key] = key === 'pitch' ? M.clamp(value, -89, 89) : M.wrapDeg(value);
      const q = M.quatFromHpb(hpb.heading, hpb.pitch, hpb.bank);
      view.orientationCameraToAnchorXYZW = q;
      S.angleCache = { id: view.id, q: q.slice(), hpb };
    }
    saveCamera(view);
    renderDetails();
  }
  function nudge(key, sign) {
    const view = current();
    if (!view) return;
    const step = PRECISION[S.precision][linear(key) ? 0 : 1];
    let next = Number((poseValue(view, key) + sign * step).toFixed(6));
    if (key === 'heading' || key === 'bank') next = M.wrapDeg(next);
    const range = limits(key);
    setAxis(key, M.clamp(next, range[0], range[1]));
  }
  function buildAxes() {
    AXES.forEach(entry => {
      const key = entry[0], letter = entry[1], labelText = entry[2];
      const fieldEl = make('div', undefined, 'mc-axis');
      const label = make('label');
      label.htmlFor = 'mc-pose-' + key;
      if (letter) label.appendChild(make('span', letter, 'mc-axis-letter'));
      label.appendChild(make('span', labelText));
      const stepper = make('div', undefined, 'mc-stepper');
      const input = make('input');
      input.type = 'number';
      input.id = 'mc-pose-' + key;
      input.setAttribute('data-pose', key);
      const range = limits(key);
      input.min = String(range[0]);
      input.max = String(range[1]);
      input.step = linear(key) ? '.001' : '.1';
      const minus = button('', () => nudge(key, -1), '', 'minus');
      const plus = button('', () => nudge(key, 1), '', 'plus');
      minus.setAttribute('aria-label', 'Decrease ' + labelText);
      plus.setAttribute('aria-label', 'Increase ' + labelText);
      input.addEventListener('input', () => { if (input.value !== '' && isFinite(Number(input.value))) setAxis(key, Number(input.value)); });
      input.addEventListener('change', () => { const view = current(); if (view) input.value = formatPose(key, poseValue(view, key)); });
      input.addEventListener('keydown', e => {
        if (e.key !== 'ArrowUp' && e.key !== 'ArrowDown') return;
        e.preventDefault();
        nudge(key, e.key === 'ArrowUp' ? 1 : -1);
        const view = current();
        if (view) input.value = formatPose(key, poseValue(view, key));
      });
      add(stepper, minus, input, plus);
      add(fieldEl, label, stepper);
      $(entry[3]).appendChild(fieldEl);
    });
  }
  function setPrecision(value) {
    if (!PRECISION[value]) value = 'normal';
    S.precision = value;
    $$('[data-precision]').forEach(control => control.setAttribute('aria-pressed', String(control.getAttribute('data-precision') === value)));
    const factors = PRECISION[value];
    $('mc-position-step').textContent = 'Step ' + factors[0].toFixed(3) + ' m';
    $('mc-angle-step').textContent = 'Step ' + factors[1].toFixed(1) + '°';
  }
  function setFov(value) {
    const view = current();
    if (!view || readOnly() || !isFinite(value)) return;
    const range = fovRange();
    view.projection = Object.assign({}, view.projection, { value: M.degToRad(M.clamp(value, range[0], range[1])) });
    saveCamera(view);
    renderDetails();
  }

  /* Motion preview (same easing as the module) */
  let motionToken = 0;
  function previewMotion() {
    const marker = $('mc-motion-marker'), view = current();
    if (!view) return;
    const token = ++motionToken;
    const t = view.transition;
    const duration = t.kind === 'cut' || reducedMotion() ? 0 : t.durationSeconds * 1000;
    const start = performance.now() + (reducedMotion() ? 0 : t.delaySeconds * 1000);
    function tick(time) {
      if (token !== motionToken) return;
      if (time < start) { requestAnimationFrame(tick); return; }
      const u = duration ? Math.min(1, (time - start) / duration) : 1;
      marker.style.left = (M.ease(t.kind, u) * 100) + '%';
      if (u < 1) requestAnimationFrame(tick);
    }
    marker.style.left = '0%';
    requestAnimationFrame(tick);
  }

  /* Camera & category operations */
  function newCamera(categoryId, name, template) {
    const base = template ? clone(template) : {
      mode: 'aircraftCockpit', requiredCapabilities: ['cockpitPose', 'interaction', 'fov'],
      positionMetersFRD: [0, 0, 0], orientationCameraToAnchorXYZW: [0, 0, 0, 1],
      projection: { kind: 'simulatorFovRadians', value: DEFAULT_FOV_RAD, axis: 'adapterDefined' },
      transition: { kind: 'smoothstep3', durationSeconds: 0.65, delaySeconds: 0 },
      interaction: true, trackingPolicy: 'nativeIfQualified', effects: { enabled: false },
      temporaryLookPolicy: 'resetOnRecall', levelHorizon: false, seedDefinitionName: null,
    };
    return Object.assign(base, { id: M.uuid(), categoryId, name, viewId: 0, favorite: false });
  }
  function addCamera() {
    if (!P() || readOnly()) return;
    namedDialog('Add camera', 'New camera', name => {
      const group = category() || P().categories[0];
      const view = newCamera(group.id, name, current());
      P().cameras.push(view);
      group.cameraIds.push(view.id);
      queueOps([{ op: 'setCamera', camera: view }, { op: 'setCategories', categories: P().categories }]);
      selectCamera(view.id);
      announce('Camera added to ' + group.name + '.');
    }, 'Add camera');
  }
  function addCategory() {
    if (!P() || readOnly()) return;
    namedDialog('Add category', 'New category', name => {
      const group = { id: M.uuid(), name, cameraIds: [] };
      const view = newCamera(group.id, 'New view', current());
      group.cameraIds.push(view.id);
      P().categories.push(group);
      P().cameras.push(view);
      S.expanded[group.id] = true;
      queueOps([{ op: 'setCamera', camera: view }, { op: 'setCategories', categories: P().categories }]);
      selectCamera(view.id);
    }, 'Add category');
  }
  function renameCamera(view) {
    const target = view || current();
    namedDialog('Rename camera', target.name, name => { target.name = name; saveCamera(target); render(); });
  }
  function moveCamera(delta) {
    const group = category(), view = current();
    const from = group.cameraIds.indexOf(view.id), to = from + delta;
    if (to < 0 || to >= group.cameraIds.length) return;
    const tmp = group.cameraIds[from]; group.cameraIds[from] = group.cameraIds[to]; group.cameraIds[to] = tmp;
    saveCategories();
    render();
  }
  function moveCameraCategory() {
    const original = category(), view = current();
    const body = openDialog('Move to category');
    const label = make('label', 'Category', 'mc-field'), select = make('select');
    P().categories.filter(g => g.id !== original.id).forEach(g => { const o = make('option', g.name); o.value = g.id; select.appendChild(o); });
    label.appendChild(select);
    body.appendChild(label);
    dialogActions([['Cancel', () => closeDialog()], ['Move camera', () => {
      const target = catIn(select.value);
      if (!target) return;
      if (original.cameraIds.length === 1) { dialogError('Keep at least one camera in each category.'); return; }
      original.cameraIds.splice(original.cameraIds.indexOf(view.id), 1);
      target.cameraIds.push(view.id);
      view.categoryId = target.id;
      S.expanded[target.id] = true;
      closeDialog();
      queueOps([{ op: 'setCamera', camera: view }, { op: 'setCategories', categories: P().categories }]);
      render();
    }, 'mc-primary']]);
  }
  function cameraActions() {
    const view = current(), group = category();
    if (!view || readOnly()) return;
    const index = group.cameraIds.indexOf(view.id);
    actionDialog(view.name, [
      ['Rename', 'pencil', () => renameCamera()],
      ['Duplicate camera', 'copy', () => {
        const duplicate = clone(view);
        duplicate.id = M.uuid(); duplicate.name = (view.name + ' copy').slice(0, 60); duplicate.viewId = 0; duplicate.favorite = false;
        P().cameras.push(duplicate);
        group.cameraIds.splice(index + 1, 0, duplicate.id);
        queueOps([{ op: 'setCamera', camera: duplicate }, { op: 'setCategories', categories: P().categories }]);
        selectCamera(duplicate.id);
      }],
      ['Move up', 'arrow-up', () => moveCamera(-1), index === 0],
      ['Move down', 'arrow-down', () => moveCamera(1), index === group.cameraIds.length - 1],
      ['Move to category', 'folder-input', moveCameraCategory, P().categories.length === 1],
      ['Delete camera', 'trash-2', () => confirmDialog('Delete camera?', 'Remove “' + view.name + '” from this profile?', () => {
        group.cameraIds = group.cameraIds.filter(id => id !== view.id);
        P().cameras = P().cameras.filter(c => c.id !== view.id);
        const ops = [{ op: 'removeCamera', cameraId: view.id }, { op: 'setCategories', categories: P().categories }];
        if (P().defaultCameraId === view.id) { P().defaultCameraId = group.cameraIds[0]; ops.push({ op: 'setProfileFields', defaultCameraId: P().defaultCameraId }); }
        queueOps(ops);
        selectCamera(group.cameraIds[0]);
      }), group.cameraIds.length === 1],
    ]);
  }
  function moveCategory(group, delta) {
    const list = P().categories, from = list.indexOf(group), to = from + delta;
    if (to < 0 || to >= list.length) return;
    list[from] = list[to]; list[to] = group;
    saveCategories();
    render();
  }
  function categoryActions(group) {
    if (readOnly()) return;
    const list = P().categories;
    actionDialog(group.name, [
      ['Move category up', 'arrow-up', () => moveCategory(group, -1), list.indexOf(group) === 0],
      ['Move category down', 'arrow-down', () => moveCategory(group, 1), list.indexOf(group) === list.length - 1],
      ['Rename category', 'pencil', () => namedDialog('Rename category', group.name, name => { group.name = name; saveCategories(); render(); })],
      ['Add category', 'folder-plus', addCategory],
      ['Delete category', 'trash-2', () => confirmDialog('Delete category?', 'Remove “' + group.name + '” and its ' + group.cameraIds.length + ' cameras?', () => {
        const removed = group.cameraIds.slice();
        P().categories = P().categories.filter(g => g.id !== group.id);
        P().cameras = P().cameras.filter(c => removed.indexOf(c.id) < 0);
        const ops = removed.map(id => ({ op: 'removeCamera', cameraId: id }));
        ops.push({ op: 'setCategories', categories: P().categories });
        if (removed.indexOf(P().defaultCameraId) >= 0) { P().defaultCameraId = cameras()[0].id; ops.push({ op: 'setProfileFields', defaultCameraId: P().defaultCameraId }); }
        queueOps(ops);
        selectCamera(cameras()[0].id);
      }), list.length === 1],
    ]);
  }

  /* Pose tools */
  function restoreSavedPose() {
    const view = current();
    if (!view || !hasCommitted(view.id)) { announce('Save this new camera before restoring its pose.', 'warn'); return; }
    const id = view.id;
    bridge.request('ExportProfile', { profileId: P().profileId, working: false }).then(ack => {
      let saved = null;
      try { saved = JSON.parse(ack.result.code).cameras.filter(c => c.id === id)[0]; } catch (error) { saved = null; }
      const target = camIn(id);
      if (ack.status !== 'Completed' || !saved || !target) { announce('The saved pose could not be read.', 'warn'); return; }
      target.positionMetersFRD = clone(saved.positionMetersFRD);
      target.orientationCameraToAnchorXYZW = clone(saved.orientationCameraToAnchorXYZW);
      target.projection = clone(saved.projection);
      S.angleCache = null;
      saveCamera(target);
      renderDetails();
      announce('Saved camera pose restored.');
    });
  }
  function resetRotation() {
    const view = current();
    if (!view) return;
    view.orientationCameraToAnchorXYZW = [0, 0, 0, 1];
    S.angleCache = null;
    saveCamera(view);
    renderDetails();
    announce('Camera rotation reset.');
  }
  function bakeLook() {
    command('BakeTemporaryLook', {}).then(ack => { if (ack.status === 'Completed') announce('Pose updated from the current view. Save changes to keep it.'); });
  }
  function captureView() {
    const view = current();
    if (!view) return;
    S.pendingCapture = { cameraId: view.id };
    command('CaptureView', {}).then(ack => { if (ack.status === 'Rejected') S.pendingCapture = null; });
  }
  function onViewCaptured(p) {
    const pending = S.pendingCapture;
    if (!pending) return;
    S.pendingCapture = null;
    if (!p.ok) { announce(p.message || 'The current view could not be read.', 'error'); return; }
    const view = camIn(pending.cameraId);
    if (!view) return;
    const body = openDialog('Use the captured view?');
    body.appendChild(make('p', 'Values read from the simulator for “' + view.name + '”. Nothing changes until you choose Use these values, and nothing is saved until you choose Save changes.'));
    if (!p.qualified) {
      const warn = make('p', undefined, 'mc-warn');
      add(warn, make('strong', 'Not qualified: '), p.message || 'View capture is not qualified for this simulator build. The values may be inaccurate.');
      body.appendChild(warn);
    } else if (p.message) body.appendChild(make('p', p.message));
    const ui = Array.isArray(p.positionMetersFRD) ? M.uiFromFrd(p.positionMetersFRD) : null;
    const hpb = Array.isArray(p.orientationCameraToAnchorXYZW) ? M.hpbFromQuat(p.orientationCameraToAnchorXYZW) : null;
    const kv = make('dl', undefined, 'mc-kv');
    const row = (k, v) => add(kv, make('dt', k), make('dd', v));
    if (ui) row('X · Y · Z', fmt(ui.x, 3) + ' · ' + fmt(ui.y, 3) + ' · ' + fmt(ui.z, 3) + ' m');
    if (hpb) row('Pitch · Heading · Roll', fmt(hpb.pitch, 1) + '° · ' + fmt(hpb.heading, 1) + '° · ' + fmt(hpb.bank, 1) + '°');
    if (isFinite(p.fovRad)) row('Field of view', fmtFov(M.radToDeg(p.fovRad)) + '° sim FOV');
    row('Qualified', p.qualified ? 'Yes' : 'No');
    body.appendChild(kv);
    dialogActions([['Cancel', () => closeDialog()], ['Use these values', () => {
      closeDialog();
      const target = camIn(pending.cameraId);
      if (!target) return;
      if (ui) target.positionMetersFRD = p.positionMetersFRD.slice(0, 3);
      if (hpb) target.orientationCameraToAnchorXYZW = M.quatNormalize(p.orientationCameraToAnchorXYZW);
      if (isFinite(p.fovRad)) target.projection = Object.assign({}, target.projection, { value: p.fovRad });
      S.angleCache = null;
      saveCamera(target);
      renderDetails();
      announce('Captured view applied to “' + target.name + '”. Save changes to keep it.');
    }, 'mc-primary']]);
  }

  /* ───────────── Dock ───────────── */
  function dockCamera() { return (S.ownership.state === 'Active' && camIn(S.active.cameraId)) || current(); }
  function renderDock() {
    const dock = $('mc-mini-panel');
    keepFocus(dock, () => {
      const shown = dockCamera();
      const group = shown ? categoryOf(shown.id) : null;
      $('mc-dock-current').textContent = shown ? shown.name : '—';
      $('mc-dock-category').textContent = group ? group.name : (S.snapshot ? 'No camera' : 'Camera Studio');
      $('mc-flight-name').textContent = shown ? shown.name : '—';
      const blocked = controlBlockReason();
      ['mc-prev', 'mc-next'].forEach(id => { $(id).disabled = Boolean(blocked); $(id).title = blocked || ''; });
      $('mc-quick-picker').disabled = !P();
      const favorites = empty($('mc-dock-favorites'));
      dock.setAttribute('data-picking', String(IS_MINI && S.miniPicking));
      if (IS_MINI && S.miniPicking) {
        P().categories.forEach(g => {
          favorites.appendChild(make('span', g.name, 'mc-dock-group'));
          g.cameraIds.map(camIn).filter(Boolean).forEach(view => favorites.appendChild(dockViewButton(view, 'pick:')));
        });
        const done = button('Done', () => { S.miniPicking = false; renderDock(); $('mc-quick-picker').focus(); });
        done.setAttribute('data-focus-key', 'pick-done');
        favorites.appendChild(done);
      } else {
        const all = cameras().filter(view => view.favorite);
        all.slice(0, 5).forEach(view => favorites.appendChild(dockViewButton(view, 'fav:')));
        if (all.length > 5) { const more = button('+' + (all.length - 5) + ' more', quickPicker); more.setAttribute('data-focus-key', 'more'); favorites.appendChild(more); }
        if (!all.length && P()) favorites.appendChild(button('Add favorite views', () => { if (IS_MINI) requestMainEditor(); else { openEditor('cameras'); setTab('behavior'); } }));
      }
    });
    updateActiveState();
  }
  function dockViewButton(view, prefix) {
    const entry = button(view.name, () => { S.miniPicking = false; activateCamera(view.id); });
    entry.setAttribute('data-focus-key', prefix + view.id);
    entry.setAttribute('data-favorite', view.id);
    entry.setAttribute('aria-pressed', String(isLive(view.id)));
    if (view.viewId) entry.appendChild(make('span', String(view.viewId), 'mc-key'));
    const reason = cameraBlockReason(view);
    entry.disabled = Boolean(reason);
    entry.title = reason || '';
    return entry;
  }
  function quickPicker() {
    if (!P()) return;
    if (IS_MINI) { S.miniPicking = !S.miniPicking; renderDock(); const first = $('mc-dock-favorites').querySelector('button:not([disabled])'); if (first) first.focus(); return; }
    const body = openDialog('Choose a camera');
    P().categories.forEach(group => {
      body.appendChild(make('div', group.name, 'mc-section-label'));
      const choices = make('div', undefined, 'mc-picker-views');
      group.cameraIds.map(camIn).filter(Boolean).forEach(view => {
        const entry = button(view.name, () => { closeDialog(); activateCamera(view.id); });
        entry.setAttribute('aria-pressed', String(view.id === (dockCamera() || {}).id));
        const reason = cameraBlockReason(view);
        entry.disabled = Boolean(reason);
        entry.title = reason || '';
        choices.appendChild(entry);
      });
      body.appendChild(choices);
    });
    dialogActions([['Close', () => closeDialog()]]);
  }
  function requestMainEditor() {
    clearTimeout(S.editTimer);
    bridge.send('RelaySurface', { target: 'main', op: 'expand' });
    S.editTimer = setTimeout(() => announce('Open Camera Studio from the toolbar to edit'), 1000);
  }
  function onSurfaceRequest(p) {
    const me = IS_MINI ? 'mini' : 'main';
    if (p.target !== me) {
      // The editor surface also hosts an embedded dock in fly mode; let the mini toggle binding reach it.
      if (!IS_MINI && p.target === 'mini' && p.op === 'toggle' && root.dataset.mode === 'fly') { if (root.dataset.miniOpen === 'true') dismissMini(); else revealMini(); }
      return;
    }
    if (IS_MINI) { if (p.op === 'show') clearTimeout(S.editTimer); return; }
    if (p.op === 'expand') { openEditor('cameras'); bridge.send('RelaySurface', { target: 'mini', op: 'show' }); }
    else if (p.op === 'compact') enterFlightMode();
    else if (p.op === 'show') openEditor();
    else if (p.op === 'toggle') { if (root.dataset.mode === 'edit') enterFlightMode(); else openEditor(); }
  }

  /* ───────────── Header / status / save bar ───────────── */
  function updateActiveState() {
    const on = engaged();
    root.dataset.enabled = String(S.ownership.state === 'Active');
    const text = statusText();
    $('mc-control-state').textContent = text;
    $('mc-flight-state').textContent = text;
    $('mc-fault-details').hidden = !(S.ownership.state === 'Faulted' && S.fault);
    const blocked = on ? null : controlBlockReason();
    const release = $('mc-release'), dockRelease = $('mc-dock-release');
    setLabel(release, on ? 'Release camera' : 'Enable camera');
    setIcon(release, on ? 'circle-stop' : 'play');
    setLabel(dockRelease, on ? 'Release' : 'Enable');
    setIcon(dockRelease, on ? 'circle-stop' : 'play');
    dockRelease.setAttribute('aria-label', on ? 'Release camera control' : 'Enable camera control');
    [release, dockRelease].forEach(control => { control.disabled = Boolean(blocked); control.title = blocked || ''; });
  }
  function updateSavedState() {
    const dirty = isDirty();
    const saving = Boolean(S.pendingCommit);
    $('mc-save-state').setAttribute('data-dirty', String(dirty && !saving));
    setIcon($('mc-save-state'), dirty && !saving ? 'alert-triangle' : 'check');
    $('mc-save-label').textContent = saving ? 'Saving…' : dirty ? 'Unsaved changes' : 'All changes saved';
    $('mc-revert').disabled = !dirty || saving;
    $('mc-save').disabled = !dirty || saving || !S.snapshot;
    const exportButton = $('mc-export-profile');
    setLabel(exportButton, dirtyFlags().profile ? 'Export working copy' : 'Export current');
  }
  function renderBanners() {
    const host = $('mc-banners');
    keepFocus(host, () => {
      empty(host);
      if (!S.snapshot) return;
      if (S.session === 'Ready' && S.aircraft && S.aircraft.title && !S.caps.canActivate && S.caps.blockReason) {
        host.appendChild(banner('alert-triangle', [make('strong', 'Camera control unavailable: '), S.caps.blockReason]));
      } else if (S.caps.canActivate && S.caps.adapterQualified === false && S.caps.allowUnqualifiedAdapter) {
        host.appendChild(banner('alert-triangle', ['Live control is running before axis qualification — for testing only.']));
      }
      if (readOnly()) host.appendChild(banner('info', ['This profile needs a newer version of Camera Studio. It opens read-only and cannot be activated.'], [], true));
      const a = S.aircraft;
      if (a && a.modelKey && P() && a.matchProfileId !== P().profileId && S.dismissedAircraftKey !== a.modelKey) {
        const dismiss = button('', () => { S.dismissedAircraftKey = a.modelKey; renderBanners(); }, 'mc-icon-button', 'x');
        dismiss.setAttribute('aria-label', 'Dismiss');
        dismiss.setAttribute('data-focus-key', 'banner-x');
        const match = a.matchProfileId ? S.profiles.filter(p => p.profileId === a.matchProfileId)[0] : null;
        if (match) {
          const open = button('Open “' + match.name + '”', () => openProfile(match.profileId), 'mc-secondary');
          open.setAttribute('data-focus-key', 'banner-open');
          host.appendChild(banner('plane', ['“' + match.name + '” is the camera profile for ', make('strong', a.title), '.'], [open, dismiss], true));
        } else if (!a.matchProfileId) {
          const create = button('Create profile for this aircraft', createProfileForAircraft, 'mc-secondary');
          const use = button('Use current profile for this aircraft', useCurrentForAircraft, 'mc-secondary');
          create.setAttribute('data-focus-key', 'banner-create');
          use.setAttribute('data-focus-key', 'banner-use');
          host.appendChild(banner('plane', [make('strong', 'No camera profile for ' + a.title)], [create, use, dismiss]));
        }
      }
    });
  }
  function banner(iconName, parts, actions, info) {
    const el = make('div', undefined, 'mc-banner' + (info ? ' mc-info' : ''));
    el.setAttribute('role', 'note');
    const text = make('div', undefined, 'mc-banner-text');
    parts.forEach(part => add(text, part));
    add(el, icon(iconName), text);
    (actions || []).forEach(a => el.appendChild(a));
    return el;
  }
  function render() {
    const d = P();
    $('mc-profile-label').textContent = d ? (d.aircraftMatch && d.aircraftMatch.displayName ? d.aircraftMatch.displayName : 'No aircraft') + ' · ' + d.name : statusText();
    setIcon($('mc-profile-picker'), d && d.aircraftMatch && d.aircraftMatch.strategy === 'none' ? 'map-pin' : 'plane');
    renderTree();
    renderDetails();
    renderDock();
    updateActiveState();
    updateSavedState();
    renderBanners();
  }

  /* ───────────── Save / revert ───────────── */
  function saveChanges() {
    if (S.pendingCommit) return Promise.resolve(false);
    return whenIdle().then(() => {
      if (!isDirty()) return true;
      S.pendingCommit = { requestId: null };
      updateSavedState();
      return bridge.request('CommitChanges', { backup: S.working.settings.ui.backups !== false }).then(ack => {
        if (ack.status !== 'Pending' && ack.status !== 'Completed') {
          S.pendingCommit = null;
          updateSavedState();
          announce(reasonText(ack.reason), 'error');
          return false;
        }
        S.pendingCommit = { requestId: ack.requestId, revisions: ack.result && ack.result.revisions };
        updateSavedState();
        return new Promise(resolve => { S.commitWaiters[ack.requestId] = resolve; }).then(done => {
          S.pendingCommit = null;
          if (done.status === 'Committed') {
            announce('Changes saved for ' + P().name + '.');
            bridge.send('RequestSnapshot', {});
            updateSavedState();
            return true;
          }
          announce(done.message || 'Your changes could not be saved. They are still here — try again.', 'error');
          updateSavedState();
          return false;
        });
      });
    });
  }
  function revertChanges() {
    S.queue = [];
    clearTimeout(S.flushTimer);
    S.flushTimer = 0;
    whenIdle().then(() => bridge.request('RevertChanges', {})).then(ack => {
      if (ack.status === 'Completed') announce('Unsaved changes reverted.');
      else announce(reasonText(ack.reason), 'warn');
    });
  }

  /* ───────────── Bindings ───────────── */
  const bindingsList = () => (S.working ? S.working.bindings.bindings : []);
  const findBinding = (action, viewId) => bindingsList().filter(b => b.action === action && (viewId ? b.viewId === viewId : !b.viewId))[0] || null;
  const sameInput = (a, b) => Boolean(a && b) && a.definition === b.definition && (a.pov || null) === (b.pov || null);
  function setBindingResult(text, tone) {
    const el = $('mc-binding-result');
    el.textContent = text;
    if (tone) el.setAttribute('data-tone', tone); else el.removeAttribute('data-tone');
  }
  function bindingSections() {
    const used = cameras().map(c => c.viewId).filter(n => n > 0);
    const bound = action => bindingsList().filter(b => b.action === action && b.viewId > 0).map(b => b.viewId);
    const unique = list => list.filter((n, i) => list.indexOf(n) === i).sort((a, b) => a - b);
    const camFor = n => cameras().filter(c => c.viewId === n)[0];
    return [
      ['Camera views', unique(used.concat(bound('selectView'))).map(n => ({ action: 'selectView', viewId: n, name: 'Select view ID ' + n, detail: camFor(n) ? camFor(n).name : 'No camera uses this view ID' }))],
      ['Peek · hold to look, release to return', unique(used.concat(bound('peekView'))).map(n => ({ action: 'peekView', viewId: n, peek: true, name: 'Peek at ' + (camFor(n) ? camFor(n).name : 'view ID ' + n), detail: 'View ID ' + n + ' · hold to look · release to return' }))],
      ['Camera commands', COMMANDS.map(c => ({ action: c[0], viewId: 0, name: c[1], detail: c[2] }))],
    ];
  }
  function renderBindings() {
    const list = $('mc-bindings-list');
    keepFocus(list, () => {
      empty(list);
      bindingSections().forEach(section => {
        list.appendChild(make('div', section[0], 'mc-section-label'));
        if (!section[1].length) list.appendChild(make('p', 'Give a camera a view ID on its Behavior tab to bind it here.', 'mc-note'));
        section[1].forEach(row => list.appendChild(bindingRow(row)));
      });
      const details = make('details');
      details.open = S.manualOpen;
      details.addEventListener('toggle', () => { S.manualOpen = details.open; });
      details.appendChild(make('summary', 'Manual look & move'));
      const inner = make('div');
      const host = S.working ? S.working.settings.host : { lookSpeedDegPerSec: 60, moveSpeedMetersPerSec: 0.5 };
      inner.appendChild(make('p', 'Hold to look or move the active camera. Speeds come from Settings (' + host.lookSpeedDegPerSec + '°/s, ' + host.moveSpeedMetersPerSec + ' m/s).', 'mc-note'));
      MANUAL.forEach(entry => inner.appendChild(bindingRow({ action: entry[0], viewId: 0, name: entry[1], detail: entry[0].indexOf('zoom') === 0 ? 'Changes the field of view' : entry[0].indexOf('look') === 0 ? 'Temporary look' : 'Temporary position offset' })));
      details.appendChild(inner);
      list.appendChild(details);
    });
  }
  function bindingRow(spec) {
    const key = spec.action + (spec.viewId ? ':' + spec.viewId : '');
    const binding = findBinding(spec.action, spec.viewId);
    const ro = !S.snapshot;
    const row = make('div', undefined, 'mc-binding-row');
    row.setAttribute('data-binding', key);
    const name = make('div', spec.name, 'mc-binding-name');
    name.appendChild(make('small', spec.detail));
    const chip = make('span', undefined, 'mc-binding-input');
    chip.setAttribute('data-empty', String(!binding));
    if (binding) add(chip, icon(binding.input.kind === 'key' ? 'keyboard' : 'radio'), make('span', binding.input.label || binding.input.definition));
    else chip.textContent = 'Unassigned';
    const actions = make('div', undefined, 'mc-binding-actions');
    const assign = button('Assign', () => assignDialog(spec));
    assign.setAttribute('data-focus-key', 'assign:' + key);
    assign.setAttribute('aria-label', 'Assign an input to ' + spec.name);
    assign.disabled = ro;
    const clear = button('Clear', () => {
      S.working.bindings.bindings = bindingsList().filter(b => b !== binding);
      saveBindings();
      renderBindings();
      setBindingResult('Cleared “' + spec.name + '”. Save changes to keep it.');
    });
    clear.setAttribute('data-focus-key', 'clear:' + key);
    clear.setAttribute('aria-label', 'Clear ' + spec.name);
    clear.disabled = ro || !binding;
    const test = button(spec.peek ? 'Hold' : 'Test', spec.peek ? null : () => testBinding(binding, spec.name));
    test.setAttribute('data-focus-key', 'test:' + key);
    test.setAttribute('aria-label', spec.peek ? 'Hold to peek at view ID ' + spec.viewId : 'Test ' + spec.name);
    test.disabled = ro || !binding;
    if (spec.peek) wirePeek(test, spec.viewId);
    add(actions, assign, clear, test);
    add(row, name, chip, actions);
    return row;
  }
  function testBinding(binding, name) {
    if (!binding) return;
    whenIdle().then(() => bridge.request('TestBinding', { binding: clone(binding) })).then(ack => {
      if (ack.status === 'Rejected' || ack.status === 'TimedOut') { setBindingResult(reasonText(ack.reason), 'warn'); return; }
      const r = ack.result || {};
      setBindingResult(r.message || (r.recalled ? '“' + name + '” sent — the view was recalled.' : '“' + name + '” sent to the simulator.'), r.dispatched === false ? 'warn' : '');
    });
  }
  function endPeek() {
    if (!S.peekToken) return;
    bridge.send('EndPeek', { peekToken: S.peekToken });
    S.peekToken = null;
    setBindingResult('Returned to the previous view.');
  }
  function wirePeek(control, viewId) {
    let holding = false;
    const start = () => {
      if (holding || S.peekToken) return;
      const target = cameras().filter(c => c.viewId === viewId)[0];
      if (!target) { setBindingResult('Assign view ID ' + viewId + ' to a camera to use peek.', 'warn'); return; }
      const reason = cameraBlockReason(target);
      if (reason) { setBindingResult(reason, 'warn'); return; }
      holding = true;
      S.peekToken = M.uuid();
      control.setAttribute('data-holding', 'true');
      command('BeginPeek', { cameraId: target.id, peekToken: S.peekToken });
      setBindingResult('Peeking at ' + target.name + '. Release to return.');
    };
    const stop = () => {
      if (!holding) return;
      holding = false;
      control.setAttribute('data-holding', 'false');
      endPeek();
    };
    onPointer(control, 'pointerdown', event => {
      if (event.button !== 0 || control.disabled) return;
      if (event.pointerId !== undefined && control.setPointerCapture) { try { control.setPointerCapture(event.pointerId); } catch (error) { /* optional */ } }
      start();
    });
    ['pointerup', 'pointercancel', 'lostpointercapture'].forEach(type => onPointer(control, type, stop));
    if (!HAS_POINTER) document.addEventListener('mouseup', stop);
    control.addEventListener('blur', stop);
    control.addEventListener('keydown', event => { if ((event.key === ' ' || event.key === 'Enter') && !event.repeat) { event.preventDefault(); start(); } });
    control.addEventListener('keyup', event => { if (event.key === ' ' || event.key === 'Enter') { event.preventDefault(); stop(); } });
  }

  /* Key capture → MSFS input definition (SimConnect_MapInputEventToClientEvent_EX1 format) */
  const MODIFIERS = {
    ControlLeft: ['VK_LCONTROL', 'Ctrl', 0], ControlRight: ['VK_RCONTROL', 'Right Ctrl', 0],
    AltLeft: ['VK_LMENU', 'Alt', 1], AltRight: ['VK_RMENU', 'Right Alt', 1],
    ShiftLeft: ['VK_LSHIFT', 'Shift', 2], ShiftRight: ['VK_RSHIFT', 'Right Shift', 2],
  };
  const KEY_TABLE = (() => {
    const t = {};
    for (let i = 0; i < 26; i++) { const ch = String.fromCharCode(65 + i); t['Key' + ch] = [ch, ch]; }
    for (let i = 0; i <= 9; i++) { t['Digit' + i] = [String(i), String(i)]; t['Numpad' + i] = ['VK_NUMPAD' + i, 'Num ' + i]; }
    for (let i = 1; i <= 24; i++) t['F' + i] = ['VK_F' + i, 'F' + i, i > 12];
    Object.assign(t, {
      NumpadMultiply: ['VK_MULTIPLY', 'Num *'], NumpadAdd: ['VK_ADD', 'Num +'], NumpadSubtract: ['VK_SUBTRACT', 'Num -'], NumpadDecimal: ['VK_DECIMAL', 'Num .'], NumpadDivide: ['VK_DIVIDE', 'Num /'],
      ArrowLeft: ['VK_LEFT', 'Left arrow'], ArrowUp: ['VK_UP', 'Up arrow'], ArrowRight: ['VK_RIGHT', 'Right arrow'], ArrowDown: ['VK_DOWN', 'Down arrow'],
      Space: ['VK_SPACE', 'Space'], Enter: ['VK_RETURN', 'Enter'], Tab: ['VK_TAB', 'Tab'], Backspace: ['VK_BACK', 'Backspace'],
      Insert: ['VK_INSERT', 'Insert'], Delete: ['VK_DELETE', 'Delete'], Home: ['VK_HOME', 'Home'], End: ['VK_END', 'End'], PageUp: ['VK_PRIOR', 'Page Up'], PageDown: ['VK_NEXT', 'Page Down'],
      Semicolon: ['VK_SEMICOLON', ';', true], Equal: ['VK_PLUS', '=', true], Comma: ['VK_COMMA', ',', true], Minus: ['VK_MINUS', '-', true], Period: ['VK_PERIOD', '.', true],
      Slash: ['VK_SLASH', '/', true], Backquote: ['VK_TILDE', '`', true], BracketLeft: ['VK_LBRACKET', '[', true], Backslash: ['VK_BACKSLASH', '\\', true], BracketRight: ['VK_RBRACKET', ']', true], Quote: ['VK_QUOTE', "'", true],
    });
    return t;
  })();
  function keyCapture(event, held) {
    if (MODIFIERS[event.code]) { held[event.code] = true; return { pending: true }; }
    const key = KEY_TABLE[event.code];
    if (!key) return { error: 'That key cannot be used for a binding. Try another key.' };
    // Fall back to the flags when a modifier was pressed before the dialog had focus.
    if (event.ctrlKey && !held.ControlLeft && !held.ControlRight) held.ControlLeft = true;
    if (event.altKey && !held.AltLeft && !held.AltRight) held.AltLeft = true;
    if (event.shiftKey && !held.ShiftLeft && !held.ShiftRight) held.ShiftLeft = true;
    const mods = Object.keys(held).filter(code => held[code] && MODIFIERS[code]).map(code => MODIFIERS[code]).sort((a, b) => a[2] - b[2]);
    if (mods.length > 2) return { error: 'Use at most two modifier keys.' };
    return {
      input: { kind: 'key', definition: mods.map(m => m[0]).concat([key[0]]).join('+'), label: mods.map(m => m[1]).concat([key[1]]).join(' + ') },
      note: key[2] ? 'This key may not work on every simulator build.' : null,
    };
  }
  function assignDialog(spec) {
    const existing = findBinding(spec.action, spec.viewId);
    const body = openDialog('Assign ' + spec.name, () => { S.assign = null; });
    S.assign = { spec, staged: null, held: {}, controllers: null, tested: { down: false, up: false } };
    body.appendChild(make('p', 'Press a key combination now. Esc cancels. Up to two modifier keys (Ctrl, Alt, Shift) plus one key.'));
    const capture = make('div', existing ? existing.input.label : 'Waiting for a key…', 'mc-binding-input');
    capture.tabIndex = 0;
    capture.id = 'mc-assign-capture';
    capture.setAttribute('role', 'textbox');
    capture.setAttribute('aria-label', 'Key capture. Press a key combination');
    capture.setAttribute('data-capturing', 'true');
    body.appendChild(capture);
    const note = make('p', '', 'mc-warn');
    note.id = 'mc-assign-note';
    note.hidden = true;
    body.appendChild(note);
    body.appendChild(make('div', 'Or use a controller', 'mc-section-label'));
    const controllerHost = make('div');
    controllerHost.id = 'mc-assign-controller';
    controllerHost.appendChild(make('p', 'Looking for controllers…'));
    body.appendChild(controllerHost);
    capture.addEventListener('keydown', event => {
      if (event.key === 'Escape' || event.key === 'Tab') return;
      event.preventDefault();
      event.stopPropagation();
      const result = keyCapture(event, S.assign.held);
      if (result.pending) { capture.textContent = Object.keys(S.assign.held).filter(c => S.assign.held[c]).map(c => MODIFIERS[c][1]).join(' + ') + ' + …'; return; }
      if (result.error) { dialogError(result.error); S.assign.held = {}; return; }
      stageInput(result.input, result.note);
      S.assign.held = {};
    });
    capture.addEventListener('keyup', event => { if (S.assign && MODIFIERS[event.code]) S.assign.held[event.code] = false; });
    dialogActions([['Cancel', () => closeDialog()], ['Assign', applyAssign, 'mc-primary']]);
    updateAssignButton();
    bridge.send('ListControllers', {});
    requestAnimationFrame(() => capture.focus());
  }
  function stageInput(input, note) {
    const a = S.assign;
    if (!a) return;
    a.staged = input;
    $('mc-assign-capture').textContent = input.label;
    const noteEl = $('mc-assign-note');
    noteEl.hidden = !note;
    noteEl.textContent = note || '';
    const conflict = bindingsList().filter(b => sameInput(b.input, input) && !(b.action === a.spec.action && (b.viewId || 0) === (a.spec.viewId || 0)))[0];
    dialogError(conflict ? input.label + ' is already assigned to “' + actionName(conflict.action, conflict.viewId) + '”. Clear it there first.' : '');
    a.conflict = Boolean(conflict);
    updateAssignButton();
  }
  function updateAssignButton() {
    const a = S.assign;
    const buttons = $('mc-dialog-actions').querySelectorAll('button');
    const assignButton = buttons[buttons.length - 1];
    if (!assignButton || !a) return;
    const controllerReady = !a.staged || a.staged.kind !== 'joystick' || (a.tested.down && a.tested.up);
    assignButton.disabled = !a.staged || a.conflict || !controllerReady;
  }
  function renderAssignControllers() {
    const a = S.assign;
    const host = $('mc-assign-controller');
    if (!a || !host) return;
    empty(host);
    if (!a.controllers || !a.controllers.length) { host.appendChild(make('p', 'No controllers found. Connect one and reopen this dialog.')); return; }
    const grid = make('div', undefined, 'mc-form-grid mc-flush');
    const deviceLabel = make('label', 'Controller', 'mc-field'), device = make('select');
    a.controllers.forEach(c => { const o = make('option', c.name); o.value = String(c.deviceId); device.appendChild(o); });
    deviceLabel.appendChild(device);
    const kindLabel = make('label', 'Input', 'mc-field'), kind = make('select');
    [['button', 'Button'], ['pov', 'Hat switch']].forEach(k => { const o = make('option', k[1]); o.value = k[0]; kind.appendChild(o); });
    kindLabel.appendChild(kind);
    const numberField = field('Button number', '1', 'number');
    numberField.input.min = '1'; numberField.input.max = '128';
    const dirLabel = make('label', 'Direction', 'mc-field'), dir = make('select');
    ['up', 'right', 'down', 'left'].forEach(d => { const o = make('option', d.charAt(0).toUpperCase() + d.slice(1)); o.value = d; dir.appendChild(o); });
    dirLabel.appendChild(dir);
    add(grid, deviceLabel, kindLabel, numberField.label, dirLabel);
    const status = make('p', 'Choose the input, then press Test input and use the control once.');
    status.id = 'mc-assign-test';
    status.setAttribute('aria-live', 'polite');
    const testButton = button('Test input', () => {
      const c = a.controllers.filter(x => String(x.deviceId) === device.value)[0];
      if (!c) return;
      let input;
      if (kind.value === 'button') {
        const n = Math.max(1, Math.min(128, Math.round(Number(numberField.input.value) || 1)));
        input = { kind: 'joystick', deviceId: c.deviceId, deviceName: c.name, definition: 'joystick:' + c.deviceId + ':button:' + (n - 1), label: c.name + ' · Button ' + n };
      } else {
        input = { kind: 'joystick', deviceId: c.deviceId, deviceName: c.name, definition: 'joystick:' + c.deviceId + ':POV:0', pov: dir.value, label: c.name + ' · Hat ' + dir.value };
      }
      a.tested = { down: false, up: false };
      stageInput(input, null);
      status.textContent = 'Waiting for ' + input.label + '… use the control now.';
      bridge.send('TestInput', { input: clone(input) });
    }, 'mc-secondary');
    const sync = () => { numberField.label.hidden = kind.value !== 'button'; dirLabel.hidden = kind.value === 'button'; };
    kind.addEventListener('change', sync);
    sync();
    add(host, grid, status, testButton);
  }
  function onInputTest(p) {
    const a = S.assign;
    if (!a || !a.staged || a.staged.definition !== p.definition) return;
    a.tested[p.phase === 'down' ? 'down' : 'up'] = true;
    const status = $('mc-assign-test');
    if (status) status.textContent = p.phase === 'down' ? 'Pressed…' : (a.tested.down ? 'Pressed and released — this input works.' : 'Released.');
    updateAssignButton();
  }
  function applyAssign() {
    const a = S.assign;
    if (!a || !a.staged || a.conflict) return;
    const list = bindingsList().filter(b => !(b.action === a.spec.action && (b.viewId || 0) === (a.spec.viewId || 0)));
    const next = { action: a.spec.action };
    if (a.spec.viewId) next.viewId = a.spec.viewId;
    next.input = clone(a.staged);
    list.push(next);
    S.working.bindings.bindings = list;
    const label = a.staged.label;
    closeDialog();
    saveBindings();
    renderBindings();
    setBindingResult('Assigned ' + label + ' to “' + a.spec.name + '”. Save changes to keep it.');
  }

  /* ───────────── Profiles ───────────── */
  function renderProfiles() {
    const card = empty($('mc-aircraft-card'));
    const a = S.aircraft;
    const entry = make('div', undefined, 'mc-profile-entry mc-aircraft-card');
    const mark = make('span', undefined, 'mc-profile-icon');
    mark.appendChild(icon('plane'));
    const info = make('div', undefined, 'mc-profile-text');
    if (!a || !a.title) {
      add(info, make('strong', 'No aircraft loaded'), make('small', 'Load a flight to use your cameras.'));
      add(entry, mark, info);
    } else {
      const isMatch = P() && a.matchProfileId === P().profileId;
      const other = a.matchProfileId && !isMatch ? S.profiles.filter(p => p.profileId === a.matchProfileId)[0] : null;
      add(info, make('strong', 'Current aircraft · ' + a.title), make('small', isMatch ? (a.matchReason || 'Matched') + ' · ' + P().name : other ? '“' + other.name + '” is the profile for this aircraft' : 'No camera profile for this aircraft yet'));
      add(entry, mark, info);
      const actions = make('div', undefined, 'mc-row-actions');
      if (other) actions.appendChild(button('Open', () => openProfile(other.profileId), 'mc-secondary'));
      if (!isMatch && !other) actions.appendChild(button('Create profile for this aircraft', createProfileForAircraft, 'mc-secondary'));
      if (!isMatch && P() && !readOnly()) actions.appendChild(button('Use current profile for this aircraft', useCurrentForAircraft, 'mc-secondary'));
      entry.appendChild(actions);
    }
    card.appendChild(entry);
    const list = $('mc-profiles-list');
    keepFocus(list, () => {
      empty(list);
      S.profiles.forEach(profile => {
        const isCurrent = P() && profile.profileId === P().profileId;
        const row = make('div', undefined, 'mc-profile-entry');
        row.setAttribute('data-current', String(Boolean(isCurrent)));
        const pm = make('span', undefined, 'mc-profile-icon');
        pm.appendChild(icon(profile.matchStrategy === 'none' ? 'map-pin' : 'plane'));
        const text = make('div', undefined, 'mc-profile-text');
        const title = make('strong', isCurrent ? P().name : profile.name);
        if (a && a.matchProfileId === profile.profileId) title.appendChild(make('span', 'This aircraft', 'mc-pill mc-accent'));
        if (profile.readOnly) title.appendChild(make('span', 'Read-only', 'mc-pill'));
        const count = isCurrent ? P().cameras.length : profile.cameraCount;
        add(text, title, make('small', (profile.displayName || 'No aircraft') + ' · ' + count + ' cameras' + (isCurrent && dirtyFlags().profile ? ' · unsaved changes' : '')));
        add(row, pm, text);
        const actions = make('div', undefined, 'mc-row-actions');
        if (isCurrent) {
          const active = make('span', 'Current', 'mc-profile-active');
          active.insertBefore(icon('check'), active.firstChild);
          const rename = button('Rename', () => namedDialog('Rename profile', P().name, value => { P().name = value; queueOps([{ op: 'setProfileFields', name: value }]); render(); renderProfiles(); }), 'mc-secondary');
          rename.setAttribute('data-focus-key', 'rename');
          rename.disabled = readOnly();
          add(actions, active, rename);
        } else {
          const open = button('Open', () => openProfile(profile.profileId), 'mc-secondary');
          open.setAttribute('data-focus-key', 'open:' + profile.profileId);
          open.setAttribute('aria-label', 'Open ' + profile.name);
          const del = button('', () => confirmDialog('Delete profile?', 'Delete “' + profile.name + '” and its backups? This cannot be undone.', () => {
            bridge.request('DeleteProfile', { profileId: profile.profileId }).then(ack => announce(ack.status === 'Completed' ? 'Deleted ' + profile.name + '.' : reasonText(ack.reason), ack.status === 'Completed' ? 'ok' : 'warn'));
          }), 'mc-icon-button mc-quiet', 'trash-2');
          del.setAttribute('aria-label', 'Delete ' + profile.name);
          del.setAttribute('data-focus-key', 'del:' + profile.profileId);
          add(actions, open, del);
        }
        row.appendChild(actions);
        list.appendChild(row);
      });
    });
    updateSavedState();
  }
  /** Save / Discard / Cancel before replacing the working profile. Resolves 'clean' | 'discard' | null. */
  function guardUnsaved(verb) {
    if (!dirtyFlags().profile && !localPending()) return Promise.resolve('clean');
    const body = 'This profile has unsaved camera changes.';
    const next = verb === 'continuing' ? 'continue' : 'switch';
    return choiceDialog('Save before ' + (verb || 'switching') + '?', body, [['Cancel', null], ['Discard & ' + next, 'discard'], ['Save & ' + next, 'save', 'mc-primary']]).then(choice => {
      if (choice === 'save') return saveChanges().then(ok => (ok ? 'clean' : null));
      return choice;
    });
  }
  function openProfile(profileId) {
    if (P() && profileId === P().profileId) { setPage('cameras'); return; }
    guardUnsaved('switching').then(choice => {
      if (!choice) return;
      if (choice === 'discard') { S.queue = []; clearTimeout(S.flushTimer); S.flushTimer = 0; }
      whenIdle().then(() => bridge.request('OpenProfile', { profileId, discard: choice === 'discard' })).then(ack => {
        if (ack.status === 'Completed') { $('mc-search').value = ''; setPage('cameras'); const s = S.profiles.filter(p => p.profileId === profileId)[0]; announce('Opened ' + (s ? s.name : 'profile') + '.'); }
        else announce(reasonText(ack.reason), 'warn');
      });
    });
  }
  function createProfile(payload, message) {
    return whenIdle().then(() => bridge.request('CreateProfile', payload)).then(ack => {
      if (ack.status === 'Completed') { setPage('cameras'); announce(message); return true; }
      announce(reasonText(ack.reason), 'warn');
      return false;
    });
  }
  function saveAsProfile() {
    if (!P()) return;
    namedDialog('Save as new profile', (P().name + ' copy').slice(0, 60), name => createProfile({ name, forAircraft: false, copyOf: P().profileId }, 'Created ' + name + '.'), 'Create profile');
  }
  function newProfile() {
    namedDialog('New profile', S.aircraft && S.aircraft.title ? S.aircraft.title : 'My cameras', name => {
      guardUnsaved('continuing').then(choice => { if (choice) createProfile({ name, forAircraft: Boolean(S.aircraft && S.aircraft.modelKey), copyOf: null }, 'Created ' + name + '.'); });
    }, 'Create profile');
  }
  function createProfileForAircraft() {
    const a = S.aircraft;
    if (!a || !a.modelKey) return;
    guardUnsaved('continuing').then(choice => { if (choice) createProfile({ name: a.title || 'My cameras', forAircraft: true, copyOf: null }, 'Created a camera profile for ' + a.title + '.'); });
  }
  function useCurrentForAircraft() {
    if (!P() || !S.aircraft) return;
    const name = P().name, title = S.aircraft.title;
    bridge.request('BindProfileToAircraft', { profileId: P().profileId }).then(ack => {
      announce(ack.status === 'Completed' ? '“' + name + '” now opens for ' + title + '.' : reasonText(ack.reason), ack.status === 'Completed' ? 'ok' : 'warn');
    });
  }
  function importProfile() {
    const body = openDialog('Import a camera profile');
    const label = make('label', 'Profile code', 'mc-field'), input = make('textarea');
    input.setAttribute('aria-label', 'Profile code');
    input.placeholder = 'Paste a Camera Studio profile code';
    label.appendChild(input);
    add(body, label, make('p', 'Import a profile directly in this panel. It is added as a copy; your current profiles are not changed.'));
    if (bridge.mode === 'demo') {
      body.appendChild(button('Use example profile', () => {
        bridge.request('ExportProfile', { profileId: (S.profiles[1] || S.profiles[0]).profileId, working: false }).then(ack => { if (ack.result) input.value = ack.result.code; });
      }, 'mc-secondary'));
    }
    dialogActions([['Cancel', () => closeDialog()], ['Import', () => {
      const code = input.value.trim();
      if (!code) { dialogError('Paste a profile code first.'); return; }
      bridge.request('ImportProfile', { code }).then(ack => {
        if (ack.status !== 'Completed') { dialogError(reasonText(ack.reason)); return; }
        showImportReport(ack.result || {});
      });
    }, 'mc-primary']]);
    requestAnimationFrame(() => input.focus());
  }
  function showImportReport(result) {
    const report = result.report || {};
    const body = openDialog('Profile imported');
    body.appendChild(make('p', '“' + (report.name || 'The profile') + '” was added as a copy with new camera IDs.'));
    const kv = make('dl', undefined, 'mc-kv');
    const am = report.aircraftMatch || {};
    add(kv, make('dt', 'Aircraft'), make('dd', (am.matches ? 'Matches the loaded aircraft' : 'Not the loaded aircraft') + (am.displayName ? ' · ' + am.displayName : '')));
    add(kv, make('dt', 'Cameras'), make('dd', String(report.cameraCount || 0)));
    body.appendChild(kv);
    const unsupported = report.unsupported || [];
    if (unsupported.length) {
      body.appendChild(make('p', 'Kept, but not available in this version:', 'mc-warn'));
      const ul = make('ul', undefined, 'mc-report-list');
      unsupported.forEach(item => ul.appendChild(make('li', typeof item === 'string' ? item : (item.message || item.name || ''))));
      body.appendChild(ul);
    } else body.appendChild(make('p', 'Everything in the code was imported.'));
    dialogActions([['Close', () => closeDialog()], ['Open profile', () => { closeDialog(); if (result.profileId) openProfile(result.profileId); }, 'mc-primary']]);
  }
  function exportProfile() {
    if (!P()) return;
    const working = Boolean(dirtyFlags().profile || localPending());
    whenIdle().then(() => bridge.request('ExportProfile', { profileId: P().profileId, working })).then(ack => {
      if (ack.status !== 'Completed' || !ack.result) { announce(reasonText(ack.reason), 'warn'); return; }
      const label = ack.result.label === 'working copy' ? make('p', 'This is the working copy and includes unsaved changes.', 'mc-warn') : null;
      codeDialog(ack.result.label === 'working copy' ? 'Export working copy' : 'Export current profile', 'A shareable copy of your current camera set.', ack.result.code, label);
    });
  }
  function backupDialog() {
    bridge.request('ListBackups', {}).then(ack => {
      const list = ack.result && (ack.result.backups || ack.result) || [];
      const body = openDialog('Restore a camera backup');
      if (!list.length) body.appendChild(make('p', 'No backups for this profile yet. A backup is created when you save camera changes.'));
      list.slice(0, 5).forEach(backup => {
        const row = make('div', undefined, 'mc-profile-entry');
        const when = backup.savedAt ? new Date(backup.savedAt) : null;
        const text = make('span', 'Revision ' + backup.revision);
        text.appendChild(make('small', when && !isNaN(when.getTime()) ? 'Saved ' + when.toLocaleString() : 'Earlier save'));
        const restore = button('Restore', () => {
          bridge.request('LoadBackup', { revision: backup.revision }).then(r => {
            if (r.status !== 'Completed') { dialogError(reasonText(r.reason)); return; }
            closeDialog();
            setPage('cameras');
            announce('Backup restored. Save changes to keep it.');
          });
        }, 'mc-secondary');
        add(row, text, restore);
        body.appendChild(row);
      });
      dialogActions([['Close', () => closeDialog()]]);
    });
  }

  /* ───────────── Settings ───────────── */
  function applySettings() {
    if (!S.working) return;
    const ui = S.working.settings.ui;
    root.dataset.shortcuts = String(ui.shortcuts !== false);
    root.dataset.contrast = String(Boolean(ui.contrast));
    root.dataset.theme = ui.theme === 'light' ? 'light' : 'dark';
    root.dataset.reducedMotion = String(Boolean(ui.reducedMotion));
    root.dataset.miniEnabled = String(ui.miniEnabled !== false);
    if (IS_MINI) applyMiniMode(ui.miniEnabled === false ? 'off' : ui.miniHover !== false ? 'hover' : 'always');
    if (Boolean(ui.miniPinned) !== miniPinned) setMiniPinned(Boolean(ui.miniPinned), false);
  }
  /** Mini surface (design §4.4/§4.5): handle + hover reveal, fixed dock, or a short note when turned off. */
  function applyMiniMode(mode) {
    if (mode === miniMode) return;
    miniMode = mode;
    root.dataset.miniMode = mode;
    const panel = $('mc-mini-panel');
    if (mode === 'always') {
      clearTimeout(miniHideTimer);
      root.dataset.miniOpen = 'true';
      panel.setAttribute('aria-hidden', 'false');
    } else if (mode === 'hover') {
      miniHovered = false; miniTapped = false; miniKeyboard = false;
      if (miniPinned) revealMini(); else hideMini(true);
    } else {
      clearTimeout(miniHideTimer);
      root.dataset.miniOpen = 'false';
      panel.setAttribute('aria-hidden', 'true');
    }
  }
  function renderSettings() {
    if (!S.working) return;
    const ui = S.working.settings.ui, host = S.working.settings.host;
    ['shortcuts', 'contrast', 'backups', 'remember'].forEach(key => { $('mc-setting-' + key).checked = ui[key] !== false && Boolean(ui[key]); });
    $('mc-setting-precision').value = ui.precision || 'normal';
    $('mc-setting-theme').value = ui.theme === 'light' ? 'light' : 'dark';
    $('mc-setting-reduced-motion').checked = Boolean(ui.reducedMotion);
    $('mc-setting-mini-enabled').checked = ui.miniEnabled !== false;
    $('mc-setting-mini-hover').checked = ui.miniHover !== false;
    $('mc-setting-mini-hover').disabled = ui.miniEnabled === false;
    $('mc-setting-unqualified').checked = Boolean(host.allowUnqualifiedAdapter);
    if (document.activeElement !== $('mc-setting-look-speed')) $('mc-setting-look-speed').value = String(host.lookSpeedDegPerSec);
    if (document.activeElement !== $('mc-setting-move-speed')) $('mc-setting-move-speed').value = String(host.moveSpeedMetersPerSec);
    $('mc-qual-state').textContent = qualificationText();
    $('mc-qual-run').disabled = !S.snapshot || !S.aircraft || !S.aircraft.title || Boolean(Q);
    $('mc-qual-run').title = S.aircraft && S.aircraft.title ? '' : 'Load a flight first.';
  }
  function settingChanged() { saveSettings(); applySettings(); renderSettings(); }

  /* ───────────── Simulator axis check (QualificationStart / Probe / Restore / Record / End) ───────────── */
  const AXIS_NAMES = { forward: 'Forward', right: 'Right', up: 'Up', pitch: 'Pitch', heading: 'Heading', bank: 'Roll' };
  const DEFAULT_AXES = ['forward', 'right', 'up', 'pitch', 'heading', 'bank'];
  let Q = null;
  const waitFor = (test, ms) => new Promise(resolve => {
    const start = Date.now();
    (function poll() { if (test()) resolve(true); else if (Date.now() - start > ms) resolve(false); else setTimeout(poll, 100); })();
  });
  function qualificationText() {
    const build = S.caps.simVersion ? 'Axis mapping for ' + S.caps.simVersion : 'Axis mapping for this simulator build';
    return build + ': ' + (S.caps.adapterQualified ? 'checked' : 'not checked yet');
  }
  /** Replace the open wizard dialog's content without closing it (closing ends the session). */
  function wizardStep(title, nodes, actions) {
    if (!Q || !dialogOpen()) return;
    $('mc-dialog-title').textContent = title;
    const body = empty($('mc-dialog-body'));
    nodes.forEach(node => add(body, node));
    dialogError('');
    dialogActions(actions);
    requestAnimationFrame(() => { const target = $('mc-dialog-actions').querySelector('.mc-primary') || $('mc-dialog-close'); if (target) target.focus(); });
  }
  function endQualification() {
    if (Q && Q.started) bridge.send('QualificationEnd', {});
    Q = null;
    renderSettings();
  }
  function runAxisCheck() {
    if (!S.snapshot || !P()) return;
    Q = { started: false, axes: DEFAULT_AXES.slice(), index: 0, answers: {}, cameraId: null, recall: null };
    openDialog('Simulator axis check', endQualification);
    wizardStep('Simulator axis check', [
      make('p', 'Camera Studio will move your view in small steps. Sit in a cockpit view with the aircraft parked.'),
      make('p', 'For each step, tell Camera Studio how the view moved. It takes about a minute and does not change your camera profiles.'),
    ], [['Cancel', () => closeDialog()], ['Start', startAxisCheck, 'mc-primary']]);
  }
  function startAxisCheck() {
    const q = Q;
    wizardStep('Simulator axis check', [make('p', 'Taking camera control…')], [['Cancel', () => closeDialog()]]);
    bridge.request('QualificationStart', {}).then(ack => {
      if (Q !== q) return null;
      if (ack.status !== 'Completed') { dialogError(reasonText(ack.reason)); return null; }
      q.started = true;
      if (ack.result && Array.isArray(ack.result.axes) && ack.result.axes.length) q.axes = ack.result.axes;
      const target = [S.selection.cameraId, P().defaultCameraId].map(camIn).filter(c => c && c.mode === 'aircraftCockpit' && modeAvailability(c.mode).available)[0]
        || [S.selection.cameraId, P().defaultCameraId].map(camIn).filter(c => c && modeAvailability(c.mode).available)[0];
      if (!target) { dialogError('Choose a cockpit camera in this profile first.'); return null; }
      q.cameraId = target.id;
      return command('ActivateCamera', { cameraId: target.id })
        .then(() => waitFor(() => Q !== q || (S.ownership.state === 'Active' && S.active.cameraId === q.cameraId && !S.active.transitioning), 8000))
        .then(ok => {
          if (Q !== q) return;
          if (!ok) {
            wizardStep('Simulator axis check', [make('p', 'Camera Studio could not take control of the view: ' + statusText() + '.')], [['Close', () => closeDialog()], ['Try again', startAxisCheck, 'mc-primary']]);
            return;
          }
          probeStep();
        });
    });
  }
  function stepTitle(q, label) { return 'Step ' + (q.index + 1) + ' of ' + (q.axes.length + 1) + ' · ' + label; }
  function probeStep() {
    const q = Q;
    if (!q) return;
    if (q.index >= q.axes.length) { recallStep(); return; }
    const axis = q.axes[q.index];
    const title = stepTitle(q, AXIS_NAMES[axis] || axis);
    wizardStep(title, [make('p', 'Moving the view…')], [['Cancel', () => closeDialog()]]);
    bridge.request('QualificationProbe', { axis }).then(ack => {
      if (Q !== q) return;
      if (ack.status !== 'Completed') {
        wizardStep(title, [make('p', reasonText(ack.reason), 'mc-warn')], [['Cancel', () => closeDialog()], ['Try again', probeStep, 'mc-primary']]);
        return;
      }
      const expect = make('dl', undefined, 'mc-kv');
      add(expect, make('dt', 'Expected'), make('dd', (ack.result && ack.result.expect) || 'Watch the view.'));
      wizardStep(title, [make('p', 'Watch the simulator view. How did it move?'), expect], [
        ['Moved some other way', () => answerProbe(axis, 'wrong')],
        ['Moved the opposite way', () => answerProbe(axis, 'reversed')],
        ['Moved as described', () => answerProbe(axis, 'correct'), 'mc-primary'],
      ]);
    });
  }
  function answerProbe(axis, value) {
    const q = Q;
    q.answers[axis] = value;
    wizardStep(stepTitle(q, AXIS_NAMES[axis] || axis), [make('p', 'Returning the view…')], [['Cancel', () => closeDialog()]]);
    bridge.request('QualificationRestore', {}).then(() => { if (Q !== q) return; q.index++; probeStep(); });
  }
  function recallStep() {
    const q = Q;
    const title = stepTitle(q, 'Recall');
    wizardStep(title, [make('p', 'Look at a landmark outside or on the panel, then press Recall.')], [['Cancel', () => closeDialog()], ['Recall', () => {
      wizardStep(title, [make('p', 'Recalling the camera…')], [['Cancel', () => closeDialog()]]);
      command('ActivateCamera', { cameraId: q.cameraId })
        .then(() => waitFor(() => Q !== q || (S.ownership.state === 'Active' && !S.active.transitioning), 6000))
        .then(() => {
          if (Q !== q) return;
          wizardStep(title, [make('p', 'Did the view return to exactly the same place?')], [['No', () => recordQualification('fail')], ['Yes', () => recordQualification('pass'), 'mc-primary']]);
        });
    }, 'mc-primary']]);
  }
  function recordQualification(recall) {
    const q = Q;
    q.recall = recall;
    wizardStep('Recording the result', [make('p', 'Saving the axis check for this simulator build…')], []);
    bridge.request('QualificationRecord', { answers: q.answers, recall, notes: '' }).then(ack => {
      if (Q !== q) return null;
      if (ack.status === 'Completed') { showQualificationResult(q, ack.result || {}, null); return null; }
      if (ack.status !== 'Pending') { showQualificationResult(q, null, reasonText(ack.reason)); return null; }
      return new Promise(resolve => { S.commitWaiters[ack.requestId] = resolve; }).then(done => {
        if (Q !== q) return;
        showQualificationResult(q, done, done.status === 'Committed' ? null : (done.message || 'The result could not be saved.'));
      });
    });
  }
  function showQualificationResult(q, done, error) {
    const failedAxes = q.axes.filter(a => q.answers[a] !== 'correct' && q.answers[a] !== 'reversed');
    const reversed = q.axes.filter(a => q.answers[a] === 'reversed');
    const g01 = done && typeof done.g01 === 'boolean' ? done.g01 : failedAxes.length === 0;
    const g03 = done && typeof done.g03 === 'boolean' ? done.g03 : q.recall === 'pass';
    const names = list => list.map(a => AXIS_NAMES[a] || a).join(', ');
    const nodes = [];
    if (error) nodes.push(make('p', error, 'mc-warn'));
    else if (g01 && g03) {
      nodes.push(make('p', 'Axis mapping recorded — live control is enabled for this simulator build.'));
      if (reversed.length) nodes.push(make('p', 'Corrected automatically: ' + names(reversed) + ' moved the opposite way and was flipped.'));
    } else {
      nodes.push(make('p', 'The axis check did not pass:', 'mc-warn'));
      const ul = make('ul', undefined, 'mc-report-list');
      failedAxes.forEach(a => ul.appendChild(make('li', (AXIS_NAMES[a] || a) + ' moved some other way.')));
      if (!g03) ul.appendChild(make('li', 'Recall did not return the view to the same place.'));
      nodes.push(ul);
      nodes.push(make('p', 'Live control stays off for this simulator build unless you allow it in Settings → Simulator.'));
    }
    wizardStep('Axis check result', nodes, [['Close', () => closeDialog(), 'mc-primary']]);
    renderSettings();
  }

  /* ───────────── Wiring ───────────── */
  function wire() {
    buildAxes();
    for (let value = 0; value <= 20; value++) { const option = make('option', value ? String(value) : 'Unassigned'); option.value = String(value); $('mc-view-id').appendChild(option); }
    $$('[data-page][role="tab"]').forEach(control => control.addEventListener('click', () => setPage(control.getAttribute('data-page'))));
    $$('[data-tab]').forEach(control => control.addEventListener('click', () => setTab(control.getAttribute('data-tab'))));
    $$('[data-precision]').forEach(control => control.addEventListener('click', () => setPrecision(control.getAttribute('data-precision'))));
    $('mc-launch').addEventListener('click', () => openEditor());
    $('mc-expand').addEventListener('click', () => { if (IS_MINI) requestMainEditor(); else openEditor('cameras'); });
    ['mc-compact', 'mc-hide'].forEach(id => $(id).addEventListener('click', enterFlightMode));
    $('mc-profile-picker').addEventListener('click', () => setPage('profiles'));
    $('mc-browser-toggle').addEventListener('click', () => { const open = root.dataset.browserOpen !== 'true'; root.dataset.browserOpen = String(open); $('mc-browser-toggle').setAttribute('aria-expanded', String(open)); });
    $('mc-search').addEventListener('input', renderTree);
    $('mc-favorite').addEventListener('click', () => { const v = current(); if (v) setFavorite(v); });
    $('mc-camera-menu').addEventListener('click', cameraActions);
    $('mc-view-in-sim').addEventListener('click', () => { const v = current(); if (v) activateCamera(v.id); });
    $('mc-recenter').addEventListener('click', () => command('RecenterLook', {}));
    $('mc-add').addEventListener('click', () => actionDialog('Add to your library', [['New camera', 'camera', addCamera], ['New category', 'folder-plus', addCategory]]));
    $('mc-add-camera').addEventListener('click', addCamera);
    $('mc-save').addEventListener('click', () => saveChanges());
    $('mc-revert').addEventListener('click', () => confirmDialog('Revert unsaved changes?', 'Restore the last saved camera profile, settings and bindings?', revertChanges, 'Revert changes'));
    ['mc-release', 'mc-dock-release'].forEach(id => $(id).addEventListener('click', toggleControl));
    $('mc-prev').addEventListener('click', () => cycleCamera(-1));
    $('mc-next').addEventListener('click', () => cycleCamera(1));
    $('mc-quick-picker').addEventListener('click', quickPicker);
    $('mc-fault-details').addEventListener('click', () => {
      const body = openDialog('Camera stopped');
      body.appendChild(make('p', (S.fault && S.fault.message) || 'Camera control stopped after an error.'));
      body.appendChild(make('p', "Use the simulator's camera controls to check your view, then choose Enable camera."));
      dialogActions([['Close', () => closeDialog()]]);
    });
    $('mc-fov-range').addEventListener('input', e => setFov(Number(e.target.value)));
    $('mc-fov-number').addEventListener('change', e => { if (e.target.value !== '' && isFinite(Number(e.target.value))) setFov(Number(e.target.value)); else renderDetails(); });
    $$('[data-fov]').forEach(control => control.addEventListener('click', () => setFov(Number(control.getAttribute('data-fov')))));
    $('mc-center-look').addEventListener('click', resetRotation);
    $('mc-restore-pose').addEventListener('click', restoreSavedPose);
    $('mc-bake-look').addEventListener('click', bakeLook);
    $('mc-capture-view').addEventListener('click', captureView);
    $$('[data-transition]').forEach(control => control.addEventListener('click', () => {
      const view = current();
      if (!view || readOnly()) return;
      view.transition = Object.assign({}, view.transition, { kind: control.getAttribute('data-transition') });
      saveCamera(view);
      renderDetails();
    }));
    $('mc-preview-motion').addEventListener('click', previewMotion);
    [['mc-duration', 'durationSeconds', 0.1, 15], ['mc-delay', 'delaySeconds', 0, 30]].forEach(entry => $(entry[0]).addEventListener('change', e => {
      const view = current();
      if (!view) return;
      const v = Number(e.target.value);
      if (e.target.value !== '' && isFinite(v)) { view.transition = Object.assign({}, view.transition, { [entry[1]]: Number(M.clamp(v, entry[2], entry[3]).toFixed(3)) }); saveCamera(view); }
      renderDetails();
    }));
    $('mc-view-type').addEventListener('change', e => { const view = current(); if (!view) return; view.mode = e.target.value; saveCamera(view); render(); });
    $('mc-view-id').addEventListener('change', e => {
      const view = current();
      if (!view) return;
      const value = Number(e.target.value);
      const conflict = value > 0 && cameras().filter(c => c.id !== view.id && c.viewId === value)[0];
      if (conflict) { e.target.value = String(view.viewId || 0); announce('View ID ' + value + ' belongs to ' + conflict.name + '.', 'warn'); return; }
      view.viewId = value;
      saveCamera(view);
      render();
    });
    $('mc-default-view').addEventListener('change', e => {
      const view = current();
      if (!view) return;
      if (e.target.checked) { P().defaultCameraId = view.id; queueOps([{ op: 'setProfileFields', defaultCameraId: view.id }]); }
      else announce('Choose another camera to set a new default view.');
      renderDetails();
    });
    $('mc-quick-access').addEventListener('change', () => { const v = current(); if (v) setFavorite(v); });
    [['mc-retain-look', v => (v ? 'retain' : 'resetOnRecall'), 'temporaryLookPolicy'], ['mc-level', v => v, 'levelHorizon'], ['mc-interaction', v => v, 'interaction']].forEach(entry => $(entry[0]).addEventListener('change', e => {
      const view = current();
      if (!view) return;
      view[entry[2]] = entry[1](e.target.checked);
      saveCamera(view);
    }));
    ['shortcuts', 'contrast', 'backups', 'remember'].forEach(key => $('mc-setting-' + key).addEventListener('change', e => { S.working.settings.ui[key] = e.target.checked; settingChanged(); }));
    $('mc-setting-precision').addEventListener('change', e => { S.working.settings.ui.precision = e.target.value; setPrecision(e.target.value); settingChanged(); });
    $('mc-setting-theme').addEventListener('change', e => { S.working.settings.ui.theme = e.target.value; settingChanged(); });
    $('mc-setting-reduced-motion').addEventListener('change', e => { S.working.settings.ui.reducedMotion = e.target.checked; settingChanged(); });
    $('mc-setting-mini-enabled').addEventListener('change', e => { S.working.settings.ui.miniEnabled = e.target.checked; settingChanged(); });
    $('mc-setting-mini-hover').addEventListener('change', e => { S.working.settings.ui.miniHover = e.target.checked; settingChanged(); });
    $('mc-setting-unqualified').addEventListener('change', e => { S.working.settings.host.allowUnqualifiedAdapter = e.target.checked; settingChanged(); });
    [['mc-setting-look-speed', 'lookSpeedDegPerSec', 5, 360], ['mc-setting-move-speed', 'moveSpeedMetersPerSec', 0.05, 10]].forEach(entry => $(entry[0]).addEventListener('change', e => {
      const v = Number(e.target.value);
      if (e.target.value !== '' && isFinite(v)) S.working.settings.host[entry[1]] = Number(M.clamp(v, entry[2], entry[3]).toFixed(2));
      e.target.value = String(S.working.settings.host[entry[1]]);
      settingChanged();
    }));
    $('mc-qual-run').addEventListener('click', runAxisCheck);
    $('mc-diagnostics').addEventListener('click', () => bridge.request('GetDiagnostics', {}).then(ack => codeDialog('Diagnostics', 'Technical details for support. Nothing is sent anywhere.', (ack.result && ack.result.text) || reasonText(ack.reason))));
    $('mc-new-profile').addEventListener('click', saveAsProfile);
    $('mc-create-profile').addEventListener('click', newProfile);
    $('mc-import-profile').addEventListener('click', importProfile);
    $('mc-export-profile').addEventListener('click', exportProfile);
    $('mc-backups').addEventListener('click', backupDialog);
    $('mc-dialog-close').addEventListener('click', () => closeDialog());

    root.addEventListener('keydown', event => {
      if (event.key === 'Escape') {
        if (dialogOpen()) closeDialog();
        else if (IS_MINI && S.miniPicking) { S.miniPicking = false; renderDock(); $('mc-quick-picker').focus(); }
        else if (IS_MINI) { if (!miniStatic() && root.dataset.miniOpen === 'true' && !miniPinned) dismissMini(true); }
        else if (root.dataset.miniOpen === 'true' && !miniPinned) dismissMini(true);
        else if (root.dataset.mode === 'edit') enterFlightMode();
      }
      if (event.key === 'Tab' && dialogOpen()) {
        const controls = $$('button,input,select,textarea,[tabindex="0"]', $('mc-dialog-shade')).filter(c => !c.disabled && c.getClientRects().length);
        const first = controls[0], last = controls[controls.length - 1];
        if (event.shiftKey && document.activeElement === first) { event.preventDefault(); last.focus(); }
        else if (!event.shiftKey && document.activeElement === last) { event.preventDefault(); first.focus(); }
      }
      if ((event.ctrlKey || event.metaKey) && !event.altKey && (event.key === 's' || event.key === 'S') && !dialogOpen() && !IS_MINI) { event.preventDefault(); saveChanges(); }
    });

    // Mini dock reveal: pointer, focus, tap and pin are independent reasons to stay open (design §4.5).
    {
      const region = $('mc-mini-region');
      onPointer(region, 'pointerenter', event => { if (pointerType(event) === 'touch' || !miniHover() || !miniEnabled()) return; miniHovered = true; revealMini(); });
      onPointer(region, 'pointerleave', event => { if (pointerType(event) === 'touch') return; miniHovered = false; scheduleMiniHide(); });
      onPointer(document, 'pointermove', event => {
        if (pointerType(event) === 'touch' || !miniHover() || !miniEnabled()) return;
        const inside = region.contains(event.target);
        if (inside === miniHovered) return;
        miniHovered = inside;
        if (inside) revealMini(); else scheduleMiniHide();
      }, true);
      region.addEventListener('focusin', event => { if (miniSuppressFocus) return; if (miniKeyboard || matchesFocusVisible(event.target)) { miniKeyboard = true; revealMini(); } });
      region.addEventListener('focusout', () => { setTimeout(() => { if (!region.contains(document.activeElement)) { miniKeyboard = false; scheduleMiniHide(); } }, 0); });
      root.addEventListener('keydown', event => { if (event.key === 'Tab' || event.key === 'Enter' || event.key === ' ') miniKeyboard = true; }, true);
      onPointer(root, 'pointerdown', event => {
        miniKeyboard = false;
        if (pointerType(event) === 'touch') miniHovered = false;
        if (!region.contains(event.target) && !dialogOpen()) { miniTapped = false; miniHovered = false; scheduleMiniHide(0); }
      }, true);
      $('mc-mini-trigger').addEventListener('click', event => {
        if (miniPinned) { revealMini(); return; }
        if (miniTapped || (event.detail > 0 && root.dataset.miniOpen === 'true')) { dismissMini(); return; }
        miniTapped = true;
        revealMini();
        if (event.detail === 0) { miniKeyboard = true; $('mc-prev').focus(); }
      });
      if (typeof window.ResizeObserver === 'function') {
        new window.ResizeObserver(sizeMiniClearance).observe($('mc-mini-panel'));
        new window.ResizeObserver(fitDialog).observe(root.querySelector('.mc-dialog'));
      } else window.addEventListener('resize', () => { sizeMiniClearance(); fitDialog(); });
    }
    $('mc-mini-pin').addEventListener('click', () => setMiniPinned(!miniPinned, true));
  }

  window.FSCApp = { state: S, bridge };
  if (bridge.demo) window.FSCDemo = bridge.demo;
  window.FSCIcons.hydrateIcons(root);
  $$('button,summary,label').forEach(control => control.classList.add('cursor-interaction'));
  wire();
  setTab('position');
  setPage('cameras');
  render();
  if (SURFACE === 'demo') sizeMiniClearance();
  if (SURFACE === 'main') root.dataset.miniOpen = 'false';
  setTimeout(() => { if (!S.snapshot) { S.failedStart = true; render(); } }, 10000);
  bridge.start();
})();
