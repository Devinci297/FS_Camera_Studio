/*
 * FS Camera Studio - in-sim panel application (doc/protocol.md v2).
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

  /* ------------- Surface ------------- */
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
  // MSFS panels must also claim the keyboard from the simulator while a text field has focus, otherwise
  // typing triggers sim shortcuts: Coherent FOCUS_INPUT_FIELD / UNFOCUS_INPUT_FIELD with a stable id per input.
  let coherentFieldId = null, inputIdSeq = 0;
  const hasCoherent = () => typeof window.Coherent !== 'undefined' && window.Coherent && typeof window.Coherent.trigger === 'function';
  function inputFieldId(el) {
    if (!el.getAttribute('data-fscs-input')) el.setAttribute('data-fscs-input', 'fscs-' + (el.id || 'input-' + (++inputIdSeq)));
    return el.getAttribute('data-fscs-input');
  }
  function claimKeyboard(el) {
    const id = el ? inputFieldId(el) : null;
    if (id === coherentFieldId) return;
    if (coherentFieldId && hasCoherent()) { try { window.Coherent.trigger('UNFOCUS_INPUT_FIELD', coherentFieldId); } catch (error) { /* host without input focus API */ } }
    coherentFieldId = id;
    if (id && hasCoherent()) { try { window.Coherent.trigger('FOCUS_INPUT_FIELD', id, '', '', '', el.type === 'number'); } catch (error) { /* host without input focus API */ } }
  }
  const reportTextFocus = () => {
    const active = document.activeElement;
    const now = isTextEntry(active);
    claimKeyboard(now ? active : null);
    if (now !== textFocus) { textFocus = now; bridge.send('TextFocus', { focused: now }); }
  };
  document.addEventListener('focusin', reportTextFocus, true);
  document.addEventListener('focusout', () => setTimeout(reportTextFocus, 0), true);

  /* ------------- DOM helpers (Coherent GT safe) ------------- */
  const $ = id => document.getElementById(id);
  const $$ = (selector, scope) => Array.prototype.slice.call((scope || root).querySelectorAll(selector));
  const clone = value => (value === undefined ? undefined : JSON.parse(JSON.stringify(value)));
  const same = (a, b) => JSON.stringify(a) === JSON.stringify(b);
  const pad2 = n => (n < 10 ? '0' : '') + n;
  const fmt = (value, digits) => { const t = Number(value).toFixed(digits); return /^-0(\.0+)?$/.test(t) ? t.slice(1) : t; };
  const fmtFov = deg => String(Math.round(deg * 10) / 10);
  /** Next animation frame, or 50 ms if frames are not delivered (hidden/occluded panels): runs once. */
  function nextFrame(fn) {
    let done = false;
    const run = () => { if (done) return; done = true; fn(typeof performance !== 'undefined' ? performance.now() : Date.now()); };
    if (typeof window.requestAnimationFrame === 'function') window.requestAnimationFrame(run);
    setTimeout(run, 50);
  }
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
    if (svg && svg.getAttribute('data-icon') === name) { window.FSCIcons.tint(svg, control.style.color); return; }
    const next = icon(name); window.FSCIcons.tint(next, control.style.color);
    if (svg) svg.parentNode.replaceChild(next, svg); else control.insertBefore(next, control.firstChild);
  }
  function viewIcon(name, isCategory) {
    const text = (name || '').toLowerCase();
    if (/runway|threshold|approach/.test(text)) return 'view-runway';
    if (/gate|jetway/.test(text)) return 'view-gate';
    if (/apron|parking|ramp/.test(text)) return 'view-apron';
    if (/taxi/.test(text)) return 'view-taxiway';
    if (/tower/.test(text)) return 'view-tower';
    if (/airport/.test(text)) return 'map-pin';
    if (/pfd|mfd|instrument|g1000|radio|avionics|autopilot/.test(text)) return 'view-instrument';
    if (/throttle|mixture|trim|control|pedal/.test(text)) return 'view-controls';
    if (/cabin|seat|copilot|passenger/.test(text)) return 'view-seat';
    if (/wing/.test(text)) return 'view-wing';
    if (/tail|rear/.test(text)) return 'view-tail';
    if (/front|nose/.test(text)) return 'view-front';
    if (/orbit|walkaround|fly.by/.test(text)) return 'view-orbit';
    if (/window/.test(text)) return 'view-window';
    if (/gear|wheel/.test(text)) return 'view-wheel';
    if (/pilot|cockpit|flight deck|landing/.test(text)) return 'view-cockpit';
    if (/exterior|external/.test(text)) return 'plane';
    return isCategory ? 'folder-open' : 'camera';
  }
  function decorateName(control, name, isCategory, chosenIcon, color) {
    const label = control.querySelector('span');
    if (!label) return;
    empty(label); label.classList.add('mc-entity-name'); label.classList.toggle('mc-category-name', Boolean(isCategory)); if (/^#[0-9a-f]{6}$/i.test(color || '')) label.style.color = color;
    add(label, icon(window.FSCIcons.names.indexOf(chosenIcon) >= 0 ? chosenIcon : viewIcon(name, isCategory)), make('span', name, 'mc-entity-label'));
    window.FSCIcons.tint(label.querySelector('svg'), color);
    if(isCategory)control.title=name;
    else { control.dataset.cameraName=name;control.removeAttribute('title'); }
    control.setAttribute('aria-label', name);
  }
  function entityIcon(entity, isCategory) {
    const legacy = entity.airportIcao && !entity.iconCustomized && (!entity.icon || entity.icon === 'map-pin' || entity.icon === 'camera');
    return !legacy && window.FSCIcons.names.indexOf(entity.icon) >= 0 ? entity.icon : viewIcon(entity.name, isCategory);
  }
  function setLabel(control, text) { const span = control.querySelector('span'); if (span) span.textContent = text; }
  function keepFocus(container, render) {
    const active = document.activeElement;
    const key = active && container.contains(active) ? active.getAttribute('data-focus-key') : null;
    const scrollTop = container.scrollTop, scrollLeft = container.scrollLeft;
    render();
    if (key) { const target = $$('[data-focus-key]', container).filter(el => el.getAttribute('data-focus-key') === key)[0]; if (target) target.focus(); }
    container.scrollTop = scrollTop; container.scrollLeft = scrollLeft;
  }
  /* Delegated help covers rebuilt lists and dialogs as well as fixed controls in both surfaces. */
  function controlHelp(control) {
    if(control.closest('[data-no-tooltip="true"]'))return '';
    if(control.tagName==='SELECT' && !control.title)return control.getAttribute('aria-label') || (control.options[control.selectedIndex] && control.options[control.selectedIndex].textContent) || 'Choose an option';
    // Camera names are already readable on labeled buttons. Keep help only for icon-only cameras.
    if(control.dataset.cameraName) {
      const label=control.querySelector('.mc-entity-label')||control.querySelector('span');
      const bounds=label&&label.getBoundingClientRect();
      if(label&&getComputedStyle(label).display!=='none'&&getComputedStyle(label).visibility!=='hidden'&&bounds.width>0&&bounds.height>0)return '';
      return control.dataset.cameraHelp||control.dataset.cameraName;
    }
    const help = {
      'mc-add':'Add a camera, category or generated airport cameras.',
      'mc-add-camera':'Add a camera to the current category.',
      'mc-camera-icon':'Choose an icon for this camera.',
      'mc-camera-name':'Rename this camera. Save changes to keep the new name.',
      'mc-camera-color':'Choose the camera name and icon color, or inherit its category color.',
      'mc-category-default':'Start with this camera when selecting its category in the mini panel.',
      'mc-camera-menu':'Camera actions: rename, color, icon, duplicate, move or delete.',
      'mc-camera-binding':'Assign a keyboard or controller shortcut to this camera.',
      'mc-favorite':'Show or hide this camera in quick access.',
      'mc-view-in-sim':'Activate this camera in the simulator.',
      'mc-editor-mouse-look':'Hold the right mouse button over the simulator scene and drag to look around.',
      'mc-mouse-look':'Hold the right mouse button over the simulator scene and drag to look around.',
      'mc-mini-pin':'Keep the mini panel open, or let it hide when the pointer leaves.',
      'mc-mini-trigger':'Reveal the mini camera panel.',
      'mc-expand':'Open the full camera editor.',
      'mc-compact':'Close the editor and use the mini camera panel.',
      'mc-hide':'Hide the editor. Camera control remains active.',
      'mc-prev':'Show the previous camera in this category.',
      'mc-next':'Show the next camera in this category.',
      'mc-release':'Release camera control to the simulator.',
      'mc-dock-release':'Release camera control to the simulator.',
      'mc-save':'Save the current adjusted view and all pending changes (Ctrl+S by default).',
      'mc-revert':'Discard unsaved changes and restore the saved profile.',
      'mc-save-current-view':'Capture the simulator view, including mouse look and movement, into this camera. Review it, then Save all changes to keep it.',
      'mc-restore-pose':'Restore this camera to its last saved position and orientation.',
      'mc-center-look':'Reset pitch, yaw and roll for this camera.',
      'mc-fov-number':'Field of view in degrees. Smaller values zoom in; larger values show a wider view.',
      'mc-fov-range':'Field of view: drag left to zoom in, or right for a wider view.',
      'mc-view-id':'Set a unique view ID for recalling this camera with a binding.',
      'mc-view-type':'Choose whether this view is attached to the aircraft or fixed at an airport.',
      'mc-duration':'Time taken to transition to this camera, in seconds.',
      'mc-delay':'Pause before the transition begins, in seconds.',
      'mc-retain-look':'Remember the temporary look direction when revisiting this camera.',
      'mc-search':'Filter the camera library by name.',
      'mc-binding-search':'Find bindings by action, camera name or assigned input.',
      'mc-setting-mini-opacity':'Set only the mini panel background opacity. Camera names and controls stay readable.',
      'mc-setting-move-speed':'Base keyboard movement speed, in meters per second. Precision and the fast-movement binding scale it.',
      'mc-setting-look-speed':'Rotation speed for manual look bindings, in degrees per second.',
      'mc-setting-airport-radius':'Load saved airport cameras within this distance of the aircraft, in nautical miles.',
      'mc-setting-mouse-sensitivity':'Right-drag rotation sensitivity. Lower values turn more slowly.',
      'mc-setting-mouse-smoothing':'Right-drag smoothing in milliseconds. Higher values soften movement; zero is immediate.',
      'mc-mouse-defaults':'Restore the tested default mouse-look sensitivity and smoothing.',
      'mc-new-profile':'Duplicate the current camera set as a new profile.',
      'mc-create-profile':'Create a new profile for an aircraft or airport.',
      'mc-import-profile':'Import a Camera Studio profile file.',
      'mc-export-profile':'Choose any saved profile to export, or include edits from your current profile.',
      'mc-backups':'Browse and restore previous saved profiles.',
      'mc-airport-generator':'Generate airport views from installed scenery and preview them before saving.',
      'mc-diagnostics':'Inspect module, connection and camera-control status.',
      'mc-dialog-close':'Close this dialog.',
      'mc-nav-cameras':'Edit camera views, positions, motion and behavior.',
      'mc-nav-bindings':'Assign and manage keyboard and controller inputs.',
      'mc-nav-profiles':'Create, import, export and manage camera profiles.',
      'mc-nav-settings':'Customize appearance, mouse look and camera controls.',
      'mc-tab-position':'Move, rotate and zoom the selected camera.',
      'mc-tab-motion':'Configure transitions and camera motion effects.',
      'mc-tab-behavior':'Configure view bindings, defaults and camera behavior.'
    };
    // A disabled control's existing reason is more useful than a generic action description.
    if(control.disabled && control.title)return control.title;
    if(help[control.id])return help[control.id];
    if(control.closest('#mc-setting-name-display'))return 'Show '+control.textContent.trim().toLowerCase()+' for categories only. Camera names and icons stay visible.';
    if(control.closest('#mc-setting-theme'))return 'Use the '+control.textContent.trim().toLowerCase()+' appearance.';
    if(control.dataset.precision)return 'Use '+control.dataset.precision+' movement and rotation steps; this also scales keyboard movement.';
    if(control.classList.contains('mc-category-head'))return (control.getAttribute('aria-expanded')==='true'?'Collapse ':'Expand ')+(control.getAttribute('aria-label')||'category')+'. Right-click for category actions.';
    if(control.closest('#mc-dock-categories'))return 'Show '+(control.getAttribute('aria-label')||control.textContent.trim())+' cameras. Right-click for category actions.';
    if(control.classList.contains('mc-camera-select')||control.closest('#mc-dock-favorites'))return (control.title||control.getAttribute('aria-label')||control.textContent.trim())+'. Right-click for camera actions.';
    if(control.title)return control.title;
    const label=control.closest('label'),labelled=control.getAttribute('aria-labelledby');
    return control.getAttribute('aria-label') || (labelled && $(labelled) && $(labelled).textContent) ||
      (label && label.textContent.replace(/\s+/g,' ').trim()) || control.getAttribute('placeholder') || control.textContent.replace(/\s+/g,' ').trim();
  }
  function installTooltips() {
    const tip=make('div',null,'mc-tooltip');tip.id='mc-control-tooltip';tip.setAttribute('role','tooltip');tip.hidden=true;root.appendChild(tip);
    let target=null,timer=null,savedTitle=null,description=null,shown=false;
    const hide=()=>{
      clearTimeout(timer);timer=null;tip.hidden=true;
      if(target&&shown){
        if(savedTitle!==null&&!target.title)target.title=savedTitle;
        if(description===null)target.removeAttribute('aria-describedby');else target.setAttribute('aria-describedby',description);
      }
      target=null;savedTitle=null;description=null;shown=false;
    };
    const find=el=>el&&el.closest?el.closest('button,input,select,textarea,summary,[role="slider"],[tabindex="0"]'):null;
    const show=(control,delay)=>{
      if(!control||!root.contains(control)||control===target)return;
      hide();target=control;
      timer=setTimeout(()=>{
        const message=controlHelp(control);
        if(!message||!document.documentElement.contains(control)||!control.getClientRects().length){hide();return;}
        savedTitle=control.hasAttribute('title')?control.title:null;
        if(savedTitle!==null)control.removeAttribute('title');
        shown=true;description=control.getAttribute('aria-describedby');control.setAttribute('aria-describedby',(description?description+' ':'')+tip.id);
        tip.textContent=message;tip.hidden=false;
        const r=control.getBoundingClientRect(),box=tip.getBoundingClientRect();
        const width=document.documentElement.clientWidth||window.innerWidth,height=window.innerHeight;
        tip.style.left=Math.max(8,Math.min(r.left+(r.width-box.width)/2,width-box.width-8))+'px';
        const above=r.top-box.height-8;
        tip.style.top=Math.max(8,Math.min(above>=8?above:r.bottom+8,height-box.height-8))+'px';
      },delay);
    };
    root.addEventListener('mouseover',event=>show(find(event.target),450),true);
    root.addEventListener('mouseout',event=>{if(target&&!target.contains(event.relatedTarget))hide();},true);
    root.addEventListener('focusin',event=>show(find(event.target),150),true);
    root.addEventListener('focusout',hide,true);
    root.addEventListener('mousedown',hide,true);
    root.addEventListener('scroll',hide,true);
    root.addEventListener('keydown',event=>{if(event.key==='Escape')hide();},true);
    window.addEventListener('blur',hide);window.addEventListener('resize',hide);
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

  /* ------------- Constants ------------- */
  const NOT_AVAILABLE = 'Not available in this version - saved with the profile but cannot be activated.';
  const EXTERIOR_NOTE = 'Exterior views keep cockpit sound in the current simulator build.';
  const START_TEXT = 'Starting Camera Studio...';
  const START_FAILED_TEXT = "Camera Studio's camera module did not start - reload the panel or restart the flight.";
  const STATUS_TEXT = {
    Observing: 'Simulator in control', Acquiring: 'Taking camera control...', Active: 'Camera active',
    SimulatorSuspended: 'Paused by the simulator', OtherClient: 'Another add-on has control',
    UserDisabled: 'Camera control by add-ons is disabled in the simulator', Releasing: 'Returning camera to the simulator...',
    Faulted: "Camera stopped after an error - use the simulator's camera controls, then Enable",
  };
  const MODE_TEXT = {
    aircraftCockpit: { reference: 'Aircraft relative - meters', blurb: 'An interior view, ready to fine-tune.' },
    aircraftExterior: { reference: 'Aircraft relative - meters', blurb: 'Your aircraft from another perspective.' },
    orbit: { reference: 'Aircraft relative - meters', blurb: 'A moving view around your aircraft.' },
    flyby: { reference: 'World fixed fly-by', blurb: 'A stationary viewpoint ahead of the aircraft, tracking its pass.' },
    airport: { reference: 'Airport local - meters', blurb: 'A view anchored around the airport.' },
    free: { reference: 'Scene origin - meters', blurb: 'Position this view freely.' },
  };
  const REASONS = {
    staleEpoch: 'The flight changed. Try again.', staleBoot: 'Camera Studio restarted. Try again.', staleDraft: 'The editor was refreshed. Try again.',
    unknownCamera: 'That camera is no longer in this profile.', modeNotSupported: NOT_AVAILABLE, notActive: 'Show this camera in the sim first.',
    noActiveCamera: 'No camera is active yet.', unsavedChanges: 'This profile has unsaved camera changes.', commitPending: 'Wait for the current save to finish.',
    profileInUse: 'You cannot delete the profile that is open.', unknownProfile: 'That profile no longer exists.', noAircraft: 'Load a flight first.',
    unknownRevision: 'That backup is no longer available.', notConnected: START_TEXT, timeout: 'Camera Studio did not answer in time. Try again.',
    notCurrentPeek: null, notAllowed: 'Camera control is not allowed right now.', qualificationNotStarted: 'The axis check is not running. Start it again from Settings.', unknownAxis: 'Camera Studio does not know that axis.',
  };
  const reasonText = reason => {
    if (!reason) return 'Camera Studio could not do that.';
    if (Object.prototype.hasOwnProperty.call(REASONS, reason)) return REASONS[reason];
    if (/s/.test(reason)) return reason; // the module's own plain-language sentence
    return 'Camera Studio could not do that (' + reason + ').';
  };
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
    ['saveViews', 'Save adjusted view and all changes', 'Ctrl+S stores the current position, look and zoom, then saves pending changes'],
    ['toggleHeadTracking', 'Toggle head tracking', 'Toggle MSFS head tracking for the active camera'],
    ['toggleAirportAuto', 'Toggle auto airport camera switching', 'Switch to the nearest loaded airport camera from any view'],
    ['toggleMouseLook', 'Toggle mouse look', 'Move within the custom camera without releasing control'],
    ['toggleMini', 'Toggle mini control panel', 'Show or hide the quick-access dock'],
  ];
  const MANUAL = [['lookLeft', 'Look left'], ['lookRight', 'Look right'], ['lookUp', 'Look up'], ['lookDown', 'Look down'], ['moveForward', 'Move forward'], ['moveBack', 'Move back'], ['moveLeft', 'Move left'], ['moveRight', 'Move right'], ['moveUp', 'Move up'], ['moveDown', 'Move down'], ['zoomIn', 'Zoom in'], ['zoomOut', 'Zoom out'], ['moveFast', 'Faster movement (hold)']];
  const ACTION_NAMES = {};
  COMMANDS.concat(MANUAL).forEach(entry => { ACTION_NAMES[entry[0]] = entry[1]; });
  const actionName = (action, viewId) => (action === 'selectView' ? 'Select view ID ' + viewId : action === 'peekView' ? 'Peek at view ID ' + viewId : (ACTION_NAMES[action] || action));

  /* ------------- State (mirror of the module) ------------- */
  const S = {
    snapshot: false, failedStart: false,
    session: 'Initializing', aircraft: null,
    ownership: { state: 'Observing', gameControlled: true, reason: null },
    caps: { canActivate: false, blockReason: null, modes: {}, fovRangeDeg: null, features: {} },
    active: { profileId: null, cameraId: null, peeking: false, transitioning: false, fromDraft: false },
    selection: { profileId: null, cameraId: null },
    airports: {nearby:[],facilities:[],loaded:"",radiusNm:25}, airportPreviews: {}, airportExpanded: true,
    profiles: [], working: null, pendingCommit: null, look: null, userReleased: false, fault: null,
    page: 'cameras', tab: 'position', precision: 'normal', expanded: {}, angleCache: null,
    miniCategory: null,
    walkaround: {running:false,categoryId:null,intervalSeconds:5,loop:true},
    queue: [], inflight: null, flushTimer: 0, lastFlush: 0, idleWaiters: [], commitWaiters: {}, completedCommits: {},
    snapshotWaiters: [], pendingCapture: null, dismissedAircraftKey: null, miniPicking: false, editTimer: 0,
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
  /** AircraftChanged may carry title: null with a valid modelKey; the aircraft is loaded whenever modelKey is set. */
  const aircraftLoaded = () => Boolean(S.aircraft && S.aircraft.modelKey);
  const aircraftName = () => (S.aircraft ? (S.aircraft.title || S.aircraft.modelKey || '') : '');
  const engaged = () => S.ownership.state === 'Active' || S.ownership.state === 'Acquiring';
  const isLive = id => Boolean(id) && S.ownership.state === 'Active' && S.active.cameraId === id && (!S.active.profileId || !P() || S.active.profileId === P().profileId);

  /* ------------- Draft ops (mirror + coalesced UpdateDraft) ------------- */
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
        announce(reasonText(ack.reason), 'warn');
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
  function repairCategoryDefaults() {
    P().categories.forEach(g => { if (g.defaultCameraId && g.cameraIds.indexOf(g.defaultCameraId) < 0) delete g.defaultCameraId; });
  }
  const saveCategories = extra => { repairCategoryDefaults(); queueOps((extra || []).concat([{ op: 'setCategories', categories: P().categories }])); };
  const saveSettings = () => queueOps([{ op: 'setSettings', settings: S.working.settings }]);
  const saveBindings = () => queueOps([{ op: 'setBindings', bindings: S.working.bindings }]);

  /** Send a camera command after pending edits reached the module (recall uses the working draft). */
  // Camera-motion actions carry the session epoch; on staleEpoch/staleBoot they are retried once after a fresh snapshot.
  const MOTION_ACTIONS = ['ActivateCamera', 'EnableCamera', 'BeginPeek', 'CycleCamera', 'NextCategory', 'ReturnToPilot', 'CaptureView', 'SeedFromDefinition', 'BakeTemporaryLook'];
  function waitForSnapshot(ms) {
    return new Promise(resolve => {
      const timer = setTimeout(() => { S.snapshotWaiters = S.snapshotWaiters.filter(fn => fn !== done); resolve(false); }, ms);
      function done() { clearTimeout(timer); resolve(true); }
      S.snapshotWaiters.push(done);
    });
  }
  /** Send a camera command after pending edits reached the module. options.quiet: no toast on rejection. */
  function command(action, payload, options) {
    const opts = options || {};
    const send = () => whenIdle().then(() => bridge.request(action, payload));
    return send().then(ack => {
      const stale = ack.status === 'Rejected' && (ack.reason === 'staleEpoch' || ack.reason === 'staleBoot');
      if (stale && MOTION_ACTIONS.indexOf(action) >= 0) {
        const fresh = waitForSnapshot(2000);
        bridge.send('RequestSnapshot', {});
        return fresh.then(send);
      }
      if (stale) bridge.send('RequestSnapshot', {});
      return ack;
    }).then(ack => {
      if ((ack.status === 'Rejected' || ack.status === 'TimedOut') && !opts.quiet) {
        const text = reasonText(ack.reason);
        if (text) announce(text, 'warn');
      }
      return ack;
    });
  }

  /* ------------- Snapshot & events ------------- */
  function applySnapshot(p) {
    const firstSnapshot = !S.snapshot;
    S.snapshot = true;
    reportPanelPointer(pointerOverPanel, true);
    S.failedStart = false;
    bridge.context.moduleBootId = p.moduleBootId;
    bridge.context.sessionEpoch = p.sessionEpoch;
    S.session = p.session || 'Ready';
    S.aircraft = p.aircraft || null;
    S.ownership = p.ownership || S.ownership;
    if (S.ownership.state !== 'Active') stopMouseLook();
    S.caps = p.capabilities || S.caps;
    S.active = p.activeCamera || S.active;
    S.selection = p.selection || S.selection;
    S.profiles = p.profiles || [];
    S.airports = p.airports || S.airports;
    S.walkaround = p.walkaround || S.walkaround;
    S.working = p.working;
    S.pendingCommit = p.pendingCommit || null;
    S.look = p.look || null;
    bridge.context.draftRevision = S.working.draftRevision;
    // Edits that were not yet accepted are re-applied on top of the fresh snapshot and sent again.
    if (S.inflight) { S.queue = S.inflight.concat(S.queue); S.inflight = null; }
    if (S.queue.length) { applyOpsLocal(S.queue); scheduleFlush(true); }
    S.angleCache = null;
    const group = category();
    if (group && firstSnapshot) S.expanded[group.id] = true;
    if (firstSnapshot) {
      setPrecision(S.working.settings.ui.precision || 'normal');
      if (S.working.settings.ui.miniPinned && !IS_MINI) setMiniPinned(true, false);
    }
    applySettings();
    render();
    renderVisiblePage();
    const waiters = S.snapshotWaiters;
    S.snapshotWaiters = [];
    waiters.forEach(fn => fn());
  }
  const EVENTS = {
    Snapshot: applySnapshot,
    WalkaroundChanged(p) { S.walkaround=p; renderCameraTools(); renderMotionSettings(current()); },
    AirportsChanged(p) { S.airports=p; renderDock(); renderAirportSummary(); if($('mc-airport-nearby'))renderAirportNearby(); },
    AirportPreview(p) { S.airportPreviews[p.icao]=p; renderAirportPreview(); },
    SessionChanged(p) {
      stopMouseLook();
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
      if (S.page === 'profiles') { renderProfiles(); renderAirportSummary(); }
    },
    OwnershipChanged(p) {
      S.ownership = p;
      if (p.state !== 'Active') stopMouseLook();
      if (p.state === 'Active' || p.state === 'Acquiring') S.userReleased = false;
      if (p.state === 'Observing' && p.reason === 'userReleased') S.userReleased = true;
      if (p.state !== 'Active') S.look = null;
      render();
    },
    CapabilitiesChanged(p) { S.caps = p; render(); if (S.page === 'settings') renderSettings(); },
    ActiveCameraChanged(p) { S.active = p; render(); },
    SelectionChanged(p) {
      if (P() && p.profileId && p.profileId !== P().profileId) return;
      if (S.selection.cameraId !== p.cameraId) { S.selection = p; S.angleCache = null; render(); }
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
    CamerasSaved(p) { announce('Cameras saved.'); if(window.FSCSSaveFeedback)window.FSCSSaveFeedback.play(p.requestId).then(()=>{if(S.page==='settings')renderSaveFeedbackSettings();}); },
    CommitCompleted(p) {
      const resolve = S.commitWaiters[p.requestId];
      delete S.commitWaiters[p.requestId];
      if (S.pendingCommit && S.pendingCommit.requestId === p.requestId) S.pendingCommit = null;
      if (resolve) resolve(p);
      else {
        S.completedCommits[p.requestId] = p;
        const ids = Object.keys(S.completedCommits);
        if (ids.length > 32) delete S.completedCommits[ids[0]];
        if (p.status === 'Committed') bridge.send('RequestSnapshot', {});
      }
      updateSavedState();
    },
    ProfilesChanged(p) { S.profiles = p.profiles || []; if (S.page === 'profiles') renderProfiles(); renderBanners(); },
    TemporaryLookChanged(p) { S.look = p && p.active ? p : null; renderTempLook(); renderPoseActions(); },
    ViewCaptured: onViewCaptured,
    DefinitionsListed() { /* not used by the editor yet */ },
    ControllersListed(p) { if (S.assign) { const next = p.controllers || []; if (JSON.stringify(S.assign.controllers) !== JSON.stringify(next)) { S.assign.controllers = next; renderAssignControllers(); } } },
    InputTest: onInputTest,
    ControllerInputCaptured: onControllerInputCaptured,
    ControllerCaptureStatus: onControllerCaptureStatus,
    BindingTriggered(p) { if (S.page === 'bindings') setBindingResult('Triggered "' + actionName(p.action, p.viewId) + '"' + (p.result === 'recalled' ? ' - the view was recalled.' : '.')); },
    SurfaceRequest: onSurfaceRequest,
    FaultRaised(p) { S.fault = p; announce(p.message || 'The camera stopped after an error.', 'error'); render(); },
  };
  bridge.on('event', message => { const handler = EVENTS[message.name]; if (handler) handler(message.payload || {}); });
  bridge.on('ack', ack => {
    if (ack.status === 'Rejected' && ack.reason === 'staleBoot') bridge.send('RequestSnapshot', {});
  });

  /* ------------- Status & gating ------------- */
  function statusText() {
    if (!S.snapshot) return S.failedStart ? START_FAILED_TEXT : START_TEXT;
    if (S.ownership.state === 'Faulted' || S.session === 'Faulted') return STATUS_TEXT.Faulted;
    if (S.session === 'Suspended') return STATUS_TEXT.SimulatorSuspended;
    if (S.session !== 'Ready') return START_TEXT;
    if (!S.caps.canActivate && S.caps.blockReason && !engaged()) return S.caps.blockReason;
    if (!aircraftLoaded()) return 'Load a flight to use your cameras';
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

  /* ------------- Toast ------------- */
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

  /* ------------- Mini dock presentation (mockup sec. 4.5 behaviour) ------------- */
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
    if (window.fscsUpdateMiniRect) window.fscsUpdateMiniRect();
  }
  function miniHasKeyboardFocus() { return miniKeyboard && $('mc-mini-region').contains(document.activeElement); }
  function regionHovered() { return miniHovered; }
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
    if (window.fscsUpdateMiniRect) window.fscsUpdateMiniRect();
  }
  function scheduleMiniHide(delay) {
    if (miniStatic()) return;
    clearTimeout(miniHideTimer);
    if (miniHovered && !regionHovered()) miniHovered = false;
    if (miniPinned || miniHovered || miniTapped || miniDialogOwned || miniHasKeyboardFocus()) return;
    miniHideTimer = setTimeout(() => hideMini(), delay === undefined ? (IS_MINI ? 180 : 650) : delay);
  }
  function dismissMini(restoreFocus) {
    miniTapped = false;
    miniKeyboard = false;
    if (!miniPinned) hideMini(true);
    if (restoreFocus && !miniPinned) { miniSuppressFocus = true; $('mc-mini-trigger').focus(); miniSuppressFocus = false; }
  }
  function enterFlightMode() {
    if (IS_MINI) return;
    if (SURFACE === 'main') {
      if (window.fscsSetPanelOpen) window.fscsSetPanelOpen('mini', true);
      closeEditor();
      return;
    }
    if (S.assign) closeDialog();
    root.dataset.mode = 'fly';
    miniHovered = false; miniTapped = false; miniKeyboard = false;
    if (miniEnabled()) { revealMini(); scheduleMiniHide(1400); }
    sizeMiniClearance();
  }
  function closeEditor() {
    if (S.assign) closeDialog();
    claimKeyboard(null);
    textFocus = false;
    bridge.send('TextFocus', { focused: false });
    if (SURFACE === 'main' && window.fscsSetPanelOpen && window.fscsSetPanelOpen('main', false)) return;
    if (SURFACE !== 'main') enterFlightMode();
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

  /* ------------- Dialogs ------------- */
  let dialogOpener = null, dialogOnClose = null;
  const dialogOpen = () => !$('mc-dialog-shade').hidden;
  function fitDialog() {
    if (SURFACE !== 'demo' || !dialogOpen()) return;
    const height = root.querySelector('.mc-dialog').getBoundingClientRect().height + (root.clientWidth < 621 ? 85 : 125);
    root.style.minHeight = Math.ceil(height) + 'px';
  }
  function openDialog(title, onClose) {
    clearTimeout(toastTimer); $('mc-toast').hidden = true;
    $('mc-dialog-shade').classList.remove('mc-tracking-shade');
    $('mc-dialog-shade').classList.remove('mc-binding-shade', 'mc-airport-override-shade', 'mc-context-shade', 'mc-airport-shade', 'mc-mouselook-shade', 'mc-walkaround-shade', 'mc-confirm-shade');
    const panel = $('mc-dialog-shade').querySelector('.mc-dialog'); panel.style.left = ''; panel.style.top = ''; panel.style.maxHeight = ''; $('mc-dialog-body').style.maxHeight = '';
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
    nextFrame(() => { fitDialog(); if (!$('mc-dialog-shade').contains(document.activeElement)) $('mc-dialog-close').focus(); });
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
    nextFrame(() => { fitDialog(); updateScrollbars(); });
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
    nextFrame(() => { name.input.focus(); name.input.select(); });
  }
  function confirmDialog(title, message, action, actionLabel) {
    const body = openDialog(title);
    body.appendChild(make('p', message));
    $('mc-dialog-shade').classList.add('mc-confirm-shade');
    const label = actionLabel || 'Delete';
    dialogActions([['Cancel', () => closeDialog()], [label, () => { closeDialog(); action(); }, label === 'Delete' ? 'mc-danger' : 'mc-primary']]);
    compactEntityMenu();
  }
  function actionDialog(title, items) {
    const body = openDialog(title);
    const list = make('div', undefined, 'mc-action-list');
    items.forEach(item => {
      const entry = button(item[0], () => { closeDialog(); item[2](); }, '', item[1]);
      entry.disabled = Boolean(item[3]);
      if(item[4])entry.title=item[4];
      list.appendChild(entry);
    });
    body.appendChild(list);
    dialogActions([]);
    return body;
  }
  function choiceDialog(title, message, choices) {
    return new Promise(resolve => {
      let settled = false;
      const done = value => { if (!settled) { settled = true; resolve(value); } };
      const body = openDialog(title, () => done(null));
      body.appendChild(make('p', message));
      dialogActions(choices.map(c => [c[0], () => { done(c[1]); closeDialog(); }, c[2]]));
      $('mc-dialog-shade').classList.add('mc-confirm-shade');compactEntityMenu();
    });
  }
  function codeDialog(title, intro, code, extra, selectLabel) {
    const body = openDialog(title);
    const input = make('textarea');
    input.readOnly = true;
    input.setAttribute('aria-label', title);
    input.value = code;
    add(body, make('p', intro), extra || null, input);
    dialogActions([['Close', () => closeDialog()], [selectLabel || 'Select profile code', () => { input.focus(); input.select(); announce((selectLabel ? 'Text' : 'Profile code') + ' selected. Use your copy command.'); }, 'mc-primary']]);
  }

  /* ------------- Navigation ------------- */
  function setPage(page) {
    S.page = page;
    root.dataset.page = page;
    $$('[data-page][role="tab"]').forEach(tab => {
      const on = tab.getAttribute('data-page') === page;
      tab.setAttribute('aria-selected', String(on));
      $('mc-page-' + tab.getAttribute('data-page')).hidden = !on;
    });
    renderVisiblePage();
    nextFrame(updateScrollbars);
  }
  function renderVisiblePage() {
    if (S.page === 'profiles') { renderProfiles(); renderAirportSummary(); }
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

  /* ------------- Selection & activation ------------- */
  function selectCamera(id, options) {
    const target = camIn(id);
    if (!target) return;
    if (S.peekToken) endPeek();
    S.selection = { profileId: P().profileId, cameraId: id };
    S.angleCache = null;
    const g = categoryOf(id);
    if (g && !(options && options.preserveExpansion)) S.expanded[g.id] = true;
    root.dataset.browserOpen = 'false';
    $('mc-browser-toggle').setAttribute('aria-expanded', 'false');
    motionToken++;
    $('mc-motion-marker').style.left = '0%';
    render();
    if (!(options && options.local)) whenIdle().then(() => bridge.send('SelectCamera', { cameraId: id }));
  }
  /** Clicking a camera in the library recalls it (design sec. 4.4/sec. 6.2); unavailable views are still selected for editing. */
  function recallFromLibrary(id) {
    const cam = camIn(id);
    if (!cam) return;
    if (cameraBlockReason(cam)) { selectCamera(id); return; }
    selectCamera(id, { local: true });
    command('ActivateCamera', { cameraId: id });
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
    if(IS_MINI){
      const group=catIn(S.miniCategory)||(categoryOf((dockCamera()||{}).id)),views=group?group.cameraIds.map(camIn).filter(v=>v&&v.favorite):[];
      if(!views.length)return;
      const index=views.findIndex(v=>v.id===(dockCamera()||{}).id),next=index<0?(delta>0?0:views.length-1):(index+(delta>0?1:-1)+views.length)%views.length;
      activateCamera(views[next].id);return;
    }
    command('CycleCamera', { delta: delta > 0 ? 1 : -1 });
  }

  /* ------------- Camera library ------------- */
  let libraryDrag = null, dropMark = null;
  function sameCameraLibrary(first, second) {
    return Boolean(first && second) && (first.airportIcao || '') === (second.airportIcao || '');
  }
  function clearLibraryDrag() {
    libraryDrag = null;
    if (dropMark) dropMark.removeAttribute('data-drop-position');
    dropMark = null;
    root.classList.remove('mc-reordering');
  }
  function markDrop(target, clientY) {
    const node = target && target.closest ? target.closest('[data-drop-kind]') : null;
    if (dropMark && dropMark !== node) dropMark.removeAttribute('data-drop-position');
    dropMark = node;
    if (node) node.setAttribute('data-drop-position', libraryDrag && libraryDrag.kind === 'camera' && node.dataset.dropKind === 'category' ? 'inside' : clientY > node.getBoundingClientRect().top + node.getBoundingClientRect().height / 2 ? 'after' : 'before');
    return node;
  }
  function applyLibraryDrop(target, clientY) {
    const drag = libraryDrag;
    const node = markDrop(target, clientY);
    if (!drag || !node || !P() || drag.profileId !== P().profileId || readOnly()) { clearLibraryDrag(); return; }
    const targetKind = node.dataset.dropKind, targetId = node.dataset.dropId;
    const after = node.getAttribute('data-drop-position') === 'after';
    clearLibraryDrag();
    if (drag.kind === targetKind && drag.id === targetId) return;
    const groups = P().categories;
    const targetGroup = targetKind === 'category' ? groups.find(g => g.id === targetId) : categoryOf(targetId);
    if (!targetGroup) return;
    if (drag.kind === 'category') {
      const from = groups.findIndex(g => g.id === drag.id);
      if (from < 0 || groups[from] === targetGroup) return;
      if(!sameCameraLibrary(groups[from],targetGroup)){announce('Airport categories stay in their shared airport library.','warn');return;}
      const group = groups.splice(from, 1)[0];
      groups.splice(groups.indexOf(targetGroup) + (after ? 1 : 0), 0, group);
    } else {
      const sourceGroup = categoryOf(drag.id), camera = camIn(drag.id);
      if (!sourceGroup || !camera) return;
      if(!sameCameraLibrary(sourceGroup,targetGroup)){announce('Move this camera within its aircraft or airport library.','warn');return;}
      sourceGroup.cameraIds = sourceGroup.cameraIds.filter(id => id !== drag.id);
      const at = targetKind === 'camera' ? targetGroup.cameraIds.indexOf(targetId) + (after ? 1 : 0) : targetGroup.cameraIds.length;
      targetGroup.cameraIds.splice(Math.max(0, at), 0, drag.id);
      camera.categoryId = targetGroup.id;
      S.expanded[targetGroup.id] = true;
    }
    saveCategories(); render();
  }
  function entityColor(view) { return view.color || (categoryOf(view.id) || {}).color || ''; }
  function setCategoryDefault(view) {
    const group = categoryOf(view.id); if (!group || readOnly()) return;
    group.defaultCameraId = view.id; saveCategories(); render();
  }
  function chooseEntityColor(entity, isCategory) {
    if (!entity || readOnly()) return;
    const body = openDialog('Color for ' + entity.name);
    body.appendChild(make('p', isCategory ? 'Camera names and icons inherit this color unless they have their own color.' : 'Use the category color, or choose a color for this camera.'));
    body.appendChild(entityColorPalette(entity, isCategory));dialogActions([['Cancel',closeDialog]]);
  }
  function entityColorPalette(entity, isCategory) {
    const palette = make('div', undefined, 'mc-color-palette');
    palette.setAttribute('role', 'group'); palette.setAttribute('aria-label', 'Name and icon color');
    const choices = [['', isCategory ? 'Automatic' : 'Use category color'],['#75baff','Blue'],['#63dbc3','Teal'],['#8edd9c','Green'],['#ffd27a','Gold'],['#ffa275','Orange'],['#ff8c9c','Rose'],['#c5a0ff','Purple'],['#e8f0f7','White']];
    choices.forEach(([color,name]) => {
      const choice = button(name, () => { if(color) entity.color=color; else delete entity.color; if(isCategory)saveCategories();else saveCamera(entity);closeDialog();render(); }, 'mc-color-choice');
      choice.dataset.colorChoice = color || 'auto'; if(color)choice.style.color=color;
      choice.setAttribute('aria-pressed',String((entity.color || '') === color));palette.appendChild(choice);
    });
    return palette;
  }
  let suppressLibraryClickUntil = 0;
  function compactEntityMenu(anchor) {
    const shade=$('mc-dialog-shade'),menu=shade.querySelector('.mc-dialog');
    if(shade.hidden)return;
    shade.classList.add('mc-context-shade');
    const opener=anchor||(dialogOpener&&dialogOpener.getBoundingClientRect?dialogOpener.getBoundingClientRect():null);
    const fallback=IS_MINI?$('mc-mini-panel').getBoundingClientRect():$('mc-camera-menu').getBoundingClientRect();
    const point=opener&&(opener.bottom||opener.left)?opener:fallback;
    nextFrame(()=>{
      if(shade.hidden||!shade.classList.contains('mc-context-shade'))return;
      menu.style.left=Math.max(8,Math.min(window.innerWidth-menu.offsetWidth-8,point.left))+'px';
      menu.style.top=Math.max(8,Math.min(window.innerHeight-menu.offsetHeight-8,point.bottom))+'px';
      updateScrollbars();if(window.fscsUpdateMiniRect)window.fscsUpdateMiniRect();
    });
  }
  function wireContext(control, action) {
    let rightDownHandled = false, lastOpenAt = 0;
    const open = e => {
      e.preventDefault(); e.stopPropagation(); lastOpenAt=Date.now();
      const rect=control.getBoundingClientRect();
      reportPanelPointer(true, true, true); action();
      compactEntityMenu({left:e.clientX||rect.left,bottom:e.clientY||rect.bottom});
    };
    control.addEventListener('contextmenu',e=>{if(rightDownHandled || Date.now()-lastOpenAt<500){e.preventDefault();e.stopPropagation();return;}open(e);});
    control.addEventListener('mousedown',e=>{if(e.button===2 || e.which===3){rightDownHandled=true;open(e);}});
    // Coherent can deliver right-button mouse events without a contextmenu event.
    control.addEventListener('mouseup',e=>{if(e.button===2 || e.which===3){if(!rightDownHandled)open(e);else{e.preventDefault();e.stopPropagation();}rightDownHandled=false;}});
    control.addEventListener('keydown',e=>{if(e.key==='ContextMenu'||(e.shiftKey&&e.key==='F10'))open(e);});
  }
  function libraryTargetAt(x,y) {
    const targets = $$('[data-drop-kind]', $('mc-camera-tree'));
    return targets.find(node=>{const r=node.getBoundingClientRect();return x>=r.left&&x<=r.right&&y>=r.top&&y<=r.bottom;}) || null;
  }
  function wireMouseReorder(control, kind, id) {
    control.addEventListener('dragstart',e=>e.preventDefault());
    control.addEventListener('mousedown', e => {
      if(e.button!==0 || readOnly())return;
      const x=e.clientX,y=e.clientY,profileId=P().profileId;let moved=false;
      const move=event=>{
        if(!P() || P().profileId!==profileId || readOnly()){cancel();return;}
        if(!moved&&Math.abs(event.clientX-x)+Math.abs(event.clientY-y)<6)return;
        event.preventDefault();event.stopPropagation();
        if(!moved){libraryDrag={kind,id,profileId};moved=true;root.classList.add('mc-reordering');}
        markDrop(libraryTargetAt(event.clientX,event.clientY),event.clientY);
        const browser=$('mc-camera-tree'),r=browser.getBoundingClientRect();
        if(event.clientY<r.top+28)browser.scrollTop-=12;if(event.clientY>r.bottom-28)browser.scrollTop+=12;
      };
      const cleanup=()=>{document.removeEventListener('mousemove',move,true);document.removeEventListener('mouseup',up,true);document.removeEventListener('keydown',escape,true);window.removeEventListener('blur',cancel);};
      const up=event=>{if(event.button!==0)return;cleanup();if(moved){event.preventDefault();event.stopPropagation();suppressLibraryClickUntil=Date.now()+350;applyLibraryDrop(libraryTargetAt(event.clientX,event.clientY),event.clientY);}};
      const cancel=()=>{cleanup();clearLibraryDrag();};
      const escape=event=>{if(event.key==='Escape'||event.keyCode===27){event.preventDefault();cancel();}};
      document.addEventListener('mousemove',move,true);document.addEventListener('mouseup',up,true);document.addEventListener('keydown',escape,true);window.addEventListener('blur',cancel);
    });
  }
  document.addEventListener('click',e=>{if(Date.now()<suppressLibraryClickUntil && $('mc-camera-tree').contains(e.target)){e.preventDefault();e.stopPropagation();}},true);
  function wireReorder(control, kind, id) {
    control.draggable = false;
    wireMouseReorder(control, kind, id);
  }
  function reorderGrip(kind, id) {
    const grip = button('', () => { if(kind==='camera'){selectCamera(id);cameraActions();}else categoryActions(catIn(id)); }, 'mc-reorder-grip', 'grip-vertical');
    grip.setAttribute('aria-label','Reorder '+(kind==='camera'?camIn(id).name:catIn(id).name));
    grip.title='Drag to reorder; click for move options';wireMouseReorder(grip,kind,id);return grip;
  }
  function chooseEntityIcon(entity, isCategory) {
    if (!entity || readOnly()) return;
    const body = openDialog('Icon for ' + entity.name);
    body.appendChild(make('p', 'Choose an icon, or use Automatic to choose it from the name.'));
    const grid = make('div', undefined, 'mc-icon-picker');
    [''].concat(window.FSCIcons.pickerNames.concat(['folder-open','map'])).forEach(name => {
      const entry = button(name ? name.replace(/^view-/, '').replace(/-/g, ' ') : 'Automatic', () => {
        if (name) { entity.icon = name; entity.iconCustomized = true; } else { delete entity.icon; delete entity.iconCustomized; }
        if (isCategory) saveCategories(); else saveCamera(entity);
        closeDialog(); render();
      }, 'mc-icon-choice', name || viewIcon(entity.name, isCategory));
      entry.setAttribute('aria-pressed', String((entity.icon || '') === name)); entry.dataset.iconChoice = name || 'auto';
      grid.appendChild(entry);
    });
    body.appendChild(grid); dialogActions([['Cancel', () => closeDialog()]]);
  }
  function setFavorite(view) {
    if (readOnly()) return;
    view.favorite = !view.favorite;
    saveCamera(view);
    renderTree(); renderDetails(); renderDock();
  }
  function renderTree() {
    const tree = $('mc-camera-tree');
    const toolbar=root.querySelector('.mc-browser-title');
    if(toolbar && $('mc-toggle-categories').parentNode!==toolbar)toolbar.insertBefore($('mc-toggle-categories'),$('mc-add'));
    const groups = P() ? P().categories.filter(g => g.cameraIds.length) : [];
    const allOpen = groups.length > 0 && groups.every(g => S.expanded[g.id]);
    $('mc-toggle-categories').textContent = allOpen ? 'Collapse all' : 'Expand all';
    $('mc-toggle-categories').setAttribute('aria-expanded', String(allOpen));
    $('mc-toggle-categories').disabled = !groups.length;
    if (!P()) { empty(tree); return; }
    keepFocus(tree, () => {
      empty(tree);
      const query = $('mc-search').value.trim().toLowerCase();
      let visible = 0;
      const ro = readOnly();
      let airportRoot=null, aircraftHeading=false;
      P().categories.forEach(group => {
        if(!group.airportIcao && !aircraftHeading){tree.appendChild(make('div','Aircraft','mc-tree-scope'));aircraftHeading=true;}
        if(group.airportIcao && !airportRoot){
          airportRoot=make('section',undefined,'mc-airport-tree');
          const toggle=button('Airport '+group.airportIcao,()=>{S.airportExpanded=!S.airportExpanded;renderTree();},'mc-category-head mc-tree-scope-toggle',S.airportExpanded?'chevron-down':'chevron-right');toggle.setAttribute('aria-expanded',String(S.airportExpanded));airportRoot.appendChild(toggle);tree.appendChild(airportRoot);
        }
        if(group.airportIcao&&!S.airportExpanded&&!query)return;
        const views = group.cameraIds.map(camIn).filter(Boolean);
        const matches = views.filter(view => !query || view.name.toLowerCase().indexOf(query) >= 0 || group.name.toLowerCase().indexOf(query) >= 0);
        if (query && !matches.length) return;
        visible += matches.length;
        const section = make('section', undefined, 'mc-category');
        const open = Boolean(query) || Boolean(S.expanded[group.id]);
        const head = button('', () => { S.expanded[group.id] = !S.expanded[group.id]; renderTree(); }, 'mc-category-head');
        add(head, icon(open ? 'chevron-down' : 'chevron-right'), make('span', group.name), make('span', String(views.length), 'mc-count'));
        decorateName(head, group.name, true, entityIcon(group, true), group.color);
        head.dataset.dropKind = 'category'; head.dataset.dropId = group.id;
        if (!ro) wireReorder(head, 'category', group.id);
        head.setAttribute('aria-expanded', String(open));
        head.setAttribute('data-focus-key', 'cat:' + group.id);
        wireContext(head, () => { if (!readOnly()) categoryActions(catIn(group.id)); });
        const heading = make('div', undefined, 'mc-category-line');
        const menu = button('', () => categoryActions(group), 'mc-category-menu', 'ellipsis');
        menu.setAttribute('aria-label', group.name + ' category actions');
        menu.setAttribute('data-focus-key', 'catmenu:' + group.id);
        menu.disabled = ro;
        add(heading, ro ? null : reorderGrip('category', group.id), head, menu);
        section.appendChild(heading);
        if (open) {
          const rows = make('div', undefined, 'mc-camera-rows');
          matches.forEach(view => rows.appendChild(cameraRow(view, group, ro)));
          section.appendChild(rows);
        }
        (group.airportIcao ? airportRoot : tree).appendChild(section);
      });
      if (!visible && query) tree.appendChild(make('div', 'No cameras match your search.', 'mc-empty'));
    });
  }
  function cameraShortcutLabel(view) {
    return view.viewId ? bindingsFor('selectView',view.viewId).map(b => (b.input.label || b.input.definition || '').replace(/\s*\+\s*/g,'+')).join(' / ') : '';
  }
  function cameraRow(view, group, ro) {
    const row = make('div', undefined, 'mc-camera-row');
    const sel = current();
    row.setAttribute('data-current', String(Boolean(sel) && view.id === sel.id));
    row.setAttribute('data-live', String(isLive(view.id)));
    const choose = button(view.name, () => recallFromLibrary(view.id), 'mc-camera-select');
    decorateName(choose, view.name, false, entityIcon(view, false), entityColor(view));
    choose.classList.toggle('mc-airport-camera-name',view.mode==='airport');
    choose.setAttribute('data-focus-key', 'cam:' + view.id);
    choose.setAttribute('aria-pressed', String(Boolean(sel) && view.id === sel.id));
    let label = view.name;
    if (isLive(view.id)) { choose.appendChild(make('span', 'Live', 'mc-live')); label += ', showing in the simulator'; }
    const shortcut=cameraShortcutLabel(view);
    if (shortcut) {
      const compact=bindingsFor('selectView',view.viewId).map(b=>{
        const input=b.input, buttonMatch=(input.definition||'').match(/:button:(\d+)$/i);
        return input.kind==='joystick' ? (input.pov ? 'Hat '+input.pov : buttonMatch ? 'B'+(Number(buttonMatch[1])+1) : 'Pad') : (input.label||input.definition||'').replace(/\s*\+\s*/g,'+');
      }).join(' / ');
      const badge=make('span',compact,'mc-key'), badgeWidth=Math.min(64,Math.max(20,compact.length*6+8));
      badge.style.maxWidth=badgeWidth+'px';choose.style.paddingRight=(badgeWidth+3)+'px';
      choose.appendChild(badge); choose.classList.add('mc-has-shortcut'); label += ', shortcut ' + shortcut;
    }
    if (!hasCommitted(view.id)) label += ', unsaved';
    choose.setAttribute('aria-label', label);
    choose.addEventListener('dblclick', () => { if (!ro) renameCamera(view); });
    wireContext(choose, () => { selectCamera(view.id); if (!readOnly()) cameraActions(); });
    choose.dataset.dropKind = 'camera'; choose.dataset.dropId = view.id;
    if (!ro) wireReorder(choose, 'camera', view.id);
    const favorite = button('', () => setFavorite(view), 'mc-row-star', 'star');
    favorite.setAttribute('aria-label', (view.favorite ? 'Remove ' : 'Add ') + view.name + (view.favorite ? ' from' : ' to') + ' quick access');
    favorite.setAttribute('aria-pressed', String(Boolean(view.favorite)));
    favorite.setAttribute('data-focus-key', 'star:' + view.id);
    favorite.disabled = ro;
    add(row, ro ? null : reorderGrip('camera', view.id), choose, favorite);
    return row;
  }

  /* ------------- Inspector ------------- */
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
    const automaticMotion=view.mode==='orbit'||view.mode==='flyby';
    $$(' .mc-pose-section, #mc-position-fields, #mc-angle-fields, .mc-pose-toolbar, .mc-pose-actions',$('mc-detail-position')).forEach(el=>{el.hidden=automaticMotion;});
    let motionHint=$('mc-automatic-motion-hint');if(!motionHint){motionHint=make('p','Position and aim are controlled by the Motion tab. Zoom remains adjustable below.','mc-note');motionHint.id='mc-automatic-motion-hint';$('mc-detail-position').insertBefore(motionHint,$('mc-position-fields'));}motionHint.hidden=!automaticMotion;
    const availability = modeAvailability(view.mode);
    $('mc-reference-label').textContent = modeText.reference;
    if (document.activeElement !== $('mc-camera-name')) $('mc-camera-name').value = view.name;
    $('mc-camera-name').disabled = ro;
    $('mc-camera-name').style.color = entityColor(view);
    $('mc-camera-icon').style.color = entityColor(view);
    $('mc-camera-color').disabled = ro;
    const categoryDefault = categoryOf(view.id).defaultCameraId === view.id;
    $('mc-category-default').disabled = ro;
    $('mc-category-default').setAttribute('aria-pressed',String(categoryDefault));
    $('mc-category-default').textContent = categoryDefault ? 'Category default' : 'Make category default';
    $('mc-camera-path').textContent = (group ? group.name : '') + (view.viewId ? ' / VIEW ' + pad2(view.viewId) : ' / CAMERA');
    let description = view.id === P().defaultCameraId ? 'Your default view for this profile.' : modeText.blurb;
    if (!availability.available) description = availability.reason || NOT_AVAILABLE;
    else if (!hasCommitted(view.id)) description += ' New camera - unsaved.';
    $('mc-camera-description').textContent = description;
    $('mc-favorite').setAttribute('aria-pressed', String(Boolean(view.favorite)));
    $('mc-favorite').setAttribute('aria-label', (view.favorite ? 'Remove camera from' : 'Add camera to') + ' quick access');
    $('mc-favorite').disabled = ro;
    $('mc-camera-menu').disabled = ro;
    $('mc-camera-icon').disabled = ro;
    setIcon($('mc-camera-icon'), entityIcon(view, false));
    const assigned = view.viewId ? findBinding('selectView', view.viewId) : null;
    setLabel($('mc-camera-binding'), assigned ? cameraShortcutLabel(view) : 'Assign binding');
    $('mc-camera-binding').disabled = ro;
    $('mc-camera-binding').setAttribute('aria-label', 'Assign binding for ' + view.name);
    renderViewButton();
    renderCameraTools();
    renderTempLook();
    $$('[data-pose]').forEach(input => { if (document.activeElement !== input) input.value = formatPose(input.getAttribute('data-pose'), poseValue(view, input.getAttribute('data-pose'))); });
    $$('[data-angle-control]').forEach(control => renderAngleControl(control, view, ro));
    const range = fovRange();
    const deg = M.radToDeg(view.projection.value);
    $('mc-fov-number').min = String(range[0]); $('mc-fov-number').max = String(range[1]);
    $('mc-fov-range').min = String(range[0]); $('mc-fov-range').max = String(range[1]);
    $('mc-fov-min-label').textContent = range[0] + ' deg - close';
    $('mc-fov-max-label').textContent = range[1] + ' deg - wide';
    if (document.activeElement !== $('mc-fov-number')) $('mc-fov-number').value = fmtFov(deg);
    $('mc-fov-range').value = String(M.clamp(deg, range[0], range[1]));
    $$('[data-fov]').forEach(control => {
      const preset = Number(control.getAttribute('data-fov'));
      control.setAttribute('aria-pressed', String(Math.abs(preset - deg) < 0.25));
      control.disabled = ro || preset < range[0] || preset > range[1];
    });
    renderPoseActions();
    $('mc-pose-context').textContent = view.name;
    $('mc-pose-live').textContent = isLive(view.id) ? 'Live preview' : 'Preview inactive';
    $('mc-pose-live').setAttribute('data-live', String(isLive(view.id)));
    // Motion
    $$('[data-transition]').forEach(control => control.setAttribute('aria-pressed', String(control.getAttribute('data-transition') === view.transition.kind)));
    if (document.activeElement !== $('mc-duration')) $('mc-duration').value = String(view.transition.durationSeconds);
    $('mc-duration').disabled = ro || view.transition.kind === 'cut';
    if (document.activeElement !== $('mc-delay')) $('mc-delay').value = String(view.transition.delaySeconds);
    $('mc-motion-destination').textContent = view.name;
    renderEffects(view, ro);
    renderMotionSettings(view);
    // Behavior
    $('mc-view-type').textContent = ({aircraftCockpit:'Cockpit',aircraftExterior:'Aircraft exterior',airport:'Airport (world fixed)',orbit:'Orbit',flyby:'Fly-by',free:'Free camera'})[view.mode] || view.mode;
    $('mc-view-type').disabled = ro || Boolean(view.airportIcao);
    $('mc-mode-pill').hidden = view.mode !== 'aircraftExterior';
    const note = $('mc-mode-note');
    note.hidden = availability.available && view.mode !== 'aircraftExterior';
    note.textContent = view.mode === 'aircraftExterior' ? EXTERIOR_NOTE : (availability.reason || NOT_AVAILABLE);
    note.className = availability.available ? '' : 'mc-warn';
    if(document.activeElement!==$('mc-view-id'))$('mc-view-id').value = String(view.viewId || 0);
    $('mc-default-view').checked = P().defaultCameraId === view.id;
    $('mc-quick-access').checked = Boolean(view.favorite);
    $('mc-retain-look').checked = view.temporaryLookPolicy === 'retain';
    $$('[data-value]', $('mc-mouse-release')).forEach(b => { b.disabled = ro; b.setAttribute('aria-pressed', String(b.dataset.value === (view.mouseLookOnRelease || 'retain'))); });
    const level = levelHorizonAvailable();
    $('mc-level').checked = Boolean(view.levelHorizon);
    $('mc-level').disabled = ro || !level;
    $('mc-level-note').textContent = level ? 'Stabilize roll for this camera.' : 'Not available until qualified.';
    $('mc-interaction').checked = Boolean(view.interaction);
    $('mc-tracking').value = S.working.settings.host.globalHeadTrackingEnabled === true ? 'on' : 'off';
    let airportSettings=$('mc-camera-airport-settings');
    if(!airportSettings){airportSettings=button('Airport camera settings',()=>airportCameraSettings(current()),'mc-secondary','view-tower');airportSettings.id='mc-camera-airport-settings';$('mc-detail-behavior').appendChild(airportSettings);}
    airportSettings.hidden=view.mode!=='airport';airportSettings.disabled=ro;
    $$('#mc-inspector input, #mc-inspector select, #mc-inspector .mc-stepper button, #mc-inspector .mc-select-card button, #mc-inspector .mc-precision button', document).forEach(el => {
      if (el.id === 'mc-tracking' || el.id === 'mc-level' || el.id === 'mc-duration' || el.hasAttribute('data-fov')) return;
      el.disabled = ro;
    });
    $('mc-default-view').disabled=ro||Boolean(view.airportIcao);
    $('mc-default-view').title=view.airportIcao?'Shared airport cameras cannot be the default view for an aircraft profile.':'';
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
    $('mc-capture-view').disabled = ro || !S.snapshot || !aircraftLoaded();
    $('mc-save-current-view').disabled = $('mc-capture-view').disabled;
    let hints = $('mc-movement-hints');
    if (!hints) {
      hints = make('div', undefined, 'mc-movement-hints'); hints.id = 'mc-movement-hints';
      $('mc-position-fields').before(hints);
    }
    empty(hints);
    const bindings = (S.working.bindings || {}).bindings || [];
    [['moveForward','Forward'],['moveBack','Back'],['moveLeft','Left'],['moveRight','Right'],['moveUp','Up'],['moveDown','Down'],['moveFast','Faster']].forEach(entry => {
      const binding = bindings.find(b => b.action === entry[0] && b.input && b.input.kind === 'key');
      if (!binding) return;
      const hint = make('span');
      add(hint, make('kbd', binding.input.label || binding.input.definition.replace(/VK_/g,'')), make('span',entry[1]));
      hints.appendChild(hint);
    });
    hints.appendChild(make('span','Hold adjustment buttons to repeat','mc-hold-hint'));

    $('mc-capture-view').title = "Read the simulator's current view and review it before using it";
  }

  /* Pose fields */
  const AXES = [['x', 'X', 'Left / right', 'mc-position-fields'], ['y', 'Y', 'Up / down', 'mc-position-fields'], ['z', 'Z', 'Back / forward', 'mc-position-fields'], ['pitch', '', 'Pitch', 'mc-angle-fields'], ['heading', '', 'Yaw', 'mc-angle-fields'], ['bank', '', 'Roll', 'mc-angle-fields']];
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
  function effectiveAirportPosition(view) {
    const position=view.positionMetersFRD.slice(),height=view.airportHeight;
    if(view.mode==='airport'&&height){
      const defaults=Object.assign({runways:3,gates:3,apron:3,taxi:3,tower:35},S.working.settings.host.airportCategoryHeights||{}),override=(view.airportOverrides||{}).heightMeters;
      const meters=override==null?Math.max(defaults[height.category]||0,height.minimumMeters||0):override;
      position[2]=Number((view.worldOrigin||{}).altitude)-height.baseAltitudeMeters-meters;
    }
    return position;
  }
  const poseValue = (view, key) => (linear(key) ? M.uiFromFrd(effectiveAirportPosition(view))[key] : anglesFor(view)[key]);
  const limits = key => (linear(key) ? [-10000, 10000] : key === 'pitch' ? [-89, 89] : [-180, 180]);
  function setAxis(key, value) {
    const view = current();
    if (!view || readOnly() || !isFinite(value)) return;
    if (linear(key)) {
      const map = M.UI_AXIS_TO_FRD[key];
      const v = map.sign * M.clamp(value, -10000, 10000);
      view.positionMetersFRD[map.index] = Object.is(v, -0) ? 0 : v;
      if(map.index===2&&view.mode==='airport'&&view.airportHeight)view.airportOverrides=Object.assign({},view.airportOverrides||{},{heightMeters:M.clamp(Number((view.worldOrigin||{}).altitude)-view.airportHeight.baseAltitudeMeters-v,0,1000)});
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
  function wireNudgeHold(control, key, sign) {
    let timer=0,held=false,repeated=false,cameraId=null;
    const stop=()=>{held=false;clearTimeout(timer);timer=0;};
    const repeat=()=>{
      if(!held||control.disabled||!current()||current().id!==cameraId||readOnly()||document.hidden||!control.getBoundingClientRect().height){stop();return;}
      repeated=true;nudge(key,sign);timer=setTimeout(repeat,80);
    };
    onPointer(control,'pointerdown',e=>{
      if(e.button!==0||control.disabled||readOnly())return;
      stop();held=true;repeated=false;cameraId=current()&&current().id;
      timer=setTimeout(repeat,350);
      if(e.pointerId!==undefined&&control.setPointerCapture)try{control.setPointerCapture(e.pointerId);}catch(_){}
    });
    ['pointerup','pointercancel','lostpointercapture'].forEach(type=>onPointer(control,type,stop));
    onPointer(document,'pointerup',stop,true);
    control.addEventListener('click',e=>{if(repeated){e.preventDefault();e.stopImmediatePropagation();repeated=false;}},true);
    control.addEventListener('blur',stop);window.addEventListener('blur',stop);
    document.addEventListener('visibilitychange',stop);
  }
  // Vector diagrams remain crisp at simulator UI scales without external artwork.
  function axisDiagram(key) {
    const id = 'mc-camera-metal-' + key;
    const camera = (x, y, scale, angle, opacity) => '<g transform="translate(' + x + ' ' + y + ') rotate(' + angle + ') scale(' + scale + ')" opacity="' + opacity + '"><path class="mc-camera-body" style="fill:url(#' + id + ')" d="M-27-15H-14L-10-23H9L14-15H27Q31-15 31-10V19Q31 23 27 23H-27Q-31 23-31 19V-10Q-31-15-27-15Z"/><rect class="mc-camera-detail" x="-24" y="-11" width="9" height="5" rx="1"/><circle class="mc-camera-lens" cx="3" cy="3" r="16"/><circle class="mc-camera-glass" cx="3" cy="3" r="11"/><circle class="mc-camera-shine" cx="0" cy="0" r="4"/></g>';
    const grid = '<g class="mc-diagram-grid"><path d="M10 128L100 35M55 135L110 35M120 135V35M185 135L130 35M230 128L140 35M12 116H228M29 92H211M49 73H191M72 57H168"/></g>';
    const defs = '<defs><linearGradient id="' + id + '" x1="0" y1="0" x2="1" y2="1"><stop offset="0" stop-color="#c2d4e1"/><stop offset=".45" stop-color="#526d82"/><stop offset="1" stop-color="#1a2b3c"/></linearGradient></defs>';
    let scene = '';
    if (key === 'x') scene = camera(35,82,.64,0,.25) + camera(205,82,.64,0,.25) + '<path class="mc-diagram-arrow" d="M63 84H94M70 77L63 84L70 91M148 84H179M172 77L179 84L172 91"/>' + camera(120,81,.84,0,1);
    if (key === 'y') scene = camera(108,28,.58,0,.24) + camera(108,115,.58,0,.24) + '<path class="mc-diagram-arrow" d="M160 23V121M152 32L160 23L168 32M152 112L160 121L168 112"/>' + camera(108,70,.82,0,1);
    if (key === 'z') scene = camera(183,38,.63,0,.3) + '<path class="mc-diagram-arrow" d="M94 91L161 49M94 79V91H107M147 49H161V62"/>' + camera(63,101,.87,0,1);
    if (key === 'pitch') scene = '<path class="mc-diagram-frustum" d="M87 78L168 24V125Z"/><path class="mc-diagram-guide" d="M16 78H220M162 13Q202 74 162 129"/><path class="mc-diagram-arrow" d="M179 34Q196 78 177 119M177 46L179 34L190 39M176 107L177 119L188 114"/>' + camera(70,74,.74,0,1);
    if (key === 'heading') scene = '<path class="mc-diagram-frustum" d="M121 102L60 36H182Z"/><path class="mc-diagram-guide" d="M121 15V130"/><path class="mc-diagram-arrow" d="M49 67Q119-13 190 67M49 55V67H61M178 67H190V55"/>' + camera(118,105,.8,0,1);
    if (key === 'bank') scene = '<path class="mc-diagram-guide" d="M10 78H228"/>' + camera(116,75,.94,-25,.22) + camera(116,75,.94,25,.22) + '<path class="mc-diagram-arrow" d="M173 23Q215 76 173 126M175 35L173 23L185 26M175 114L173 126L185 122"/>' + camera(116,75,.94,0,1);
    return defs + grid + scene;
  }
  function buildAxes() {
    const directions = { x: ['Left','Right'], y: ['Down','Up'], z: ['Back','Forward'], pitch: ['Look down','Look up'], heading: ['Turn left','Turn right'], bank: ['Tilt left','Tilt right'] };
    AXES.forEach(entry => {
      const key = entry[0], letter = entry[1], labelText = entry[2];
      const fieldEl = make('div', undefined, 'mc-axis');
      const label = make('label');
      label.htmlFor = 'mc-pose-' + key;
      fieldEl.setAttribute('data-axis', key);
      const illustration = document.createElementNS('http://www.w3.org/2000/svg', 'svg');
      illustration.setAttribute('viewBox', '0 0 240 135'); illustration.setAttribute('class', 'mc-axis-illustration'); illustration.setAttribute('aria-hidden', 'true');
      illustration.innerHTML = axisDiagram(key);
      label.appendChild(make('span', labelText));
      if (letter) label.appendChild(make('span', letter, 'mc-axis-letter'));
      const description = { x: 'Move left or right', y: 'Move up or down', z: 'Move back or forward', pitch: 'Look up or down', heading: 'Turn left or right', bank: 'Tilt left or right' };
      const diagram = make('div', undefined, 'mc-direction-control');
      diagram.appendChild(illustration);
      add(fieldEl, label, make('small', description[key], 'mc-axis-description'), diagram);
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
      wireNudgeHold(minus,key,-1);wireNudgeHold(plus,key,1);
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
      stepper.appendChild(input);
      input.setAttribute('aria-label', labelText + (linear(key) ? ' in meters' : ' in degrees'));
      stepper.appendChild(make('span', linear(key) ? 'm' : '°', 'mc-axis-unit'));
      minus.className = 'mc-direction-minus'; plus.className = 'mc-direction-plus';
      minus.appendChild(make('span', directions[key][0])); plus.appendChild(make('span', directions[key][1]));
      minus.title = 'Hold to ' + directions[key][0].toLowerCase();
      plus.title = 'Hold to ' + directions[key][1].toLowerCase();
      add(diagram, minus, plus);
      if (!linear(key)) buildAngleControl(illustration, key, labelText);
      fieldEl.appendChild(stepper);
      const reset = button('Reset', () => setAxis(key, 0), 'mc-axis-reset', 'rotate-ccw');
      reset.setAttribute('aria-label', 'Reset ' + labelText + ' to zero');
      reset.setAttribute('data-reset-axis', key);
      label.appendChild(reset);
      $(entry[3]).appendChild(fieldEl);
    });
  }
  function anglePoint(key, value) {
    const a = value * Math.PI / 180;
    return key === 'pitch' ? [86 + 58*Math.cos(a),75 - 58*Math.sin(a)] : [120 + 52*Math.sin(a),75 - 52*Math.cos(a)];
  }
  function renderAngleControl(svg, view, ro) {
    const key = svg.getAttribute('data-angle-control'), value = poseValue(view, key), p = anglePoint(key, value), zero = anglePoint(key, 0);
    const center = key === 'pitch' ? [86,75] : [120,75], radius = key === 'pitch' ? 58 : 52;
    const sweep = key === 'pitch' ? (value < 0 ? 1 : 0) : (value < 0 ? 0 : 1);
    svg.querySelector('.mc-angle-zone').setAttribute('d', 'M'+center.join(' ')+'L'+zero.join(' ')+'A'+radius+' '+radius+' 0 0 '+sweep+' '+p.join(' ')+'Z');
    svg.querySelector('.mc-angle-ray').setAttribute('d','M'+center.join(' ')+'L'+p.join(' '));
    const handle = svg.querySelector('.mc-angle-handle'); handle.setAttribute('cx',p[0]); handle.setAttribute('cy',p[1]);
    svg.querySelector('.mc-angle-readout').textContent = (value > 0 ? '+' : '') + value.toFixed(1) + '°';
    svg.setAttribute('aria-valuenow',String(value)); svg.setAttribute('aria-valuetext',value.toFixed(1)+' degrees'); svg.setAttribute('aria-disabled',String(ro));
  }
  function buildAngleControl(svg, key, label) {
    const pitch = key === 'pitch';
    svg.setAttribute('viewBox','0 0 240 150'); svg.setAttribute('data-angle-control',key); svg.removeAttribute('aria-hidden');
    svg.setAttribute('role','slider'); svg.setAttribute('tabindex','0'); svg.setAttribute('aria-label',label+' angle');
    svg.setAttribute('aria-valuemin',String(limits(key)[0])); svg.setAttribute('aria-valuemax',String(limits(key)[1]));
    svg.innerHTML = '<path class="mc-angle-zone"/><path class="mc-angle-track" d="'+(pitch?'M86 17A58 58 0 0 1 86 133':'M120 23A52 52 0 1 1 120 127A52 52 0 1 1 120 23')+'"/>' +
      '<path class="mc-diagram-guide" d="'+(pitch?'M42 75H157':'M120 19V131M64 75H176')+'"/><path class="mc-angle-ray"/>' +
      '<g class="mc-angle-camera" transform="translate('+(pitch?'58 66':'104 64')+')"><rect width="32" height="23" rx="4"/><path d="M8 0L11-6H22L25 0"/><circle cx="17" cy="11" r="8"/><circle cx="17" cy="11" r="4"/></g>'+
      '<text class="mc-angle-limit" x="'+(pitch?'112':'27')+'" y="'+(pitch?'18':'78')+'">'+(pitch?'+89° Up':'−90°')+'</text>'+
      '<text class="mc-angle-limit" text-anchor="end" x="'+(pitch?'184':'218')+'" y="'+(pitch?'135':'78')+'">'+(pitch?'−89° Down':'+90°')+'</text>'+
      '<text class="mc-angle-zero" x="'+(pitch?'158':'120')+'" y="'+(pitch?'78':'14')+'" text-anchor="middle">0°</text>'+
      (pitch?'':'<text class="mc-angle-limit" x="120" y="145" text-anchor="middle">±180°</text>')+'<circle class="mc-angle-handle" r="8"/><text class="mc-angle-readout" x="'+(pitch?'37':'120')+'" y="'+(pitch?'106':'107')+'" text-anchor="middle"></text>';
    const update = e => {
      const rect = svg.getBoundingClientRect(), scale = Math.min(rect.width/240,rect.height/150);
      if (!scale || readOnly()) return;
      const x = (e.clientX-rect.left-(rect.width-240*scale)/2)/scale, y = (e.clientY-rect.top-(rect.height-150*scale)/2)/scale;
      const angle = pitch ? Math.atan2(75-y,Math.max(0,x-86)) : Math.atan2(x-120,75-y);
      const limit = pitch ? 89 : 180, step = PRECISION[S.precision][1];
      setAxis(key,M.clamp(Math.round(angle*180/Math.PI/step)*step,-limit,limit));
    };
    onPointer(svg,'pointerdown',e=>{
      if (e.button!==0 || readOnly()) return;
      e.preventDefault(); svg.focus(); update(e);
      const camera = current();
      const move=event=>{if(current()!==camera || readOnly()){end();return;}event.preventDefault();update(event);};
      const escape=event=>{if(event.key==='Escape')end();};
      const end=()=>{document.removeEventListener(HAS_POINTER?'pointermove':'mousemove',move);document.removeEventListener(HAS_POINTER?'pointerup':'mouseup',end);document.removeEventListener('pointercancel',end);window.removeEventListener('blur',end);document.removeEventListener('keydown',escape);};
      document.addEventListener(HAS_POINTER?'pointermove':'mousemove',move);document.addEventListener(HAS_POINTER?'pointerup':'mouseup',end);document.addEventListener('pointercancel',end);window.addEventListener('blur',end);document.addEventListener('keydown',escape);
    });
    svg.addEventListener('keydown',e=>{
      if (['ArrowRight','ArrowUp','ArrowLeft','ArrowDown','Home','End'].indexOf(e.key)<0) return;
      e.preventDefault();
      if(e.key==='Home')setAxis(key,0);else if(e.key==='End')setAxis(key,limits(key)[1]);else nudge(key,e.key==='ArrowRight'||e.key==='ArrowUp'?1:-1);
    });
  }
  function setPrecision(value, persist) {
    if (!PRECISION[value]) value = 'normal';
    S.precision = value;
    if (persist && S.working) {
      S.working.settings.ui.precision = value;
      saveSettings();
    }
    $$('[data-precision]').forEach(control => control.setAttribute('aria-pressed', String(control.getAttribute('data-precision') === value)));
    const factors = PRECISION[value];
    $('mc-position-step').textContent = String(Math.round(factors[0] * 1000)) + ' mm';
    $('mc-angle-step').textContent = factors[1].toFixed(1) + '°';
    const view = current();
    if (view) $$('[data-angle-control]').forEach(control => renderAngleControl(control, view, readOnly()));
  }
  function setFov(value) {
    const view = current();
    if (!view || readOnly() || !isFinite(value)) return;
    const range = fovRange();
    view.projection = Object.assign({}, view.projection, { value: M.degToRad(M.clamp(value, range[0], range[1])) });
    saveCamera(view);
    renderDetails();
  }

  const EFFECT_DEFAULTS = { enabled: false, intensity: .35, turbulence: .5, groundRoll: .35, touchdown: .5, acceleration: .35, engine: .2 };
  const EFFECT_FIELDS = [
    ['intensity', 'Overall intensity', 'Scale all effects together'],
    ['turbulence', 'Turbulence response', 'React to changes in vertical G load'],
    ['groundRoll', 'Runway & taxi bumps', 'Speed-based vibration while rolling'],
    ['touchdown', 'Touchdown', 'A brief head dip based on landing vertical speed'],
    ['acceleration', 'Acceleration & braking', 'Head movement under forward and lateral acceleration'],
    ['engine', 'Engine vibration', 'Fine vibration driven by engine 1 RPM']
  ];
  function editEffects(patch) {
    const view = current(); if (!view || readOnly()) return;
    view.effects = Object.assign({}, EFFECT_DEFAULTS, view.effects || {}, patch);
    saveCamera(view); renderEffects(view, false);
  }
  function buildEffects() {
    EFFECT_FIELDS.forEach(([key, label, description]) => {
      const row = make('label', undefined, 'mc-effect-row');
      const text = make('span'); add(text, make('strong', label), make('small', description));
      const range = make('input'); range.type = 'range'; range.min = '0'; range.max = '100'; range.step = '1';
      range.setAttribute('data-effect', key); range.setAttribute('aria-label', label);
      const value = make('output'); value.id = 'mc-effect-value-' + key;
      range.addEventListener('input', () => { const patch = {}; patch[key] = Number(range.value) / 100; editEffects(patch); });
      add(row, text, range, value); $('mc-effects-controls').appendChild(row);
    });
    $('mc-effects-enabled').addEventListener('change', e => editEffects({ enabled: e.target.checked }));
    $('mc-effects-gentle').addEventListener('click', () => editEffects(Object.assign({}, EFFECT_DEFAULTS, {enabled:true, intensity:.2})));
    $('mc-effects-reset').addEventListener('click', () => editEffects(EFFECT_DEFAULTS));
  }
  function renderEffects(view, ro) {
    const fx = Object.assign({}, EFFECT_DEFAULTS, view.effects || {}), cockpit = view.mode === 'aircraftCockpit';
    $('mc-effects-enabled').checked = fx.enabled; $('mc-effects-enabled').disabled = ro || !cockpit;
    $('mc-effects-status').textContent = !cockpit ? 'Effects are available for cockpit cameras.' : !fx.enabled ? 'Effects are off for this camera.' : isLive(view.id) ? 'Enabled for this live view. Motion responds when flight telemetry is available.' : 'Ready. Select View in sim to use these effects.';
    $$('[data-effect]').forEach(control => { const key = control.getAttribute('data-effect'); control.value = String(Math.round(fx[key] * 100)); control.disabled = ro || !cockpit; $('mc-effect-value-' + key).textContent = control.value + '%'; });
    $('mc-effects-gentle').disabled = ro || !cockpit; $('mc-effects-reset').disabled = ro;
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
      if (time < start) { nextFrame(tick); return; }
      const u = duration ? Math.min(1, (time - start) / duration) : 1;
      marker.style.left = (M.ease(t.kind, u) * 100) + '%';
      if (u < 1) nextFrame(tick);
    }
    marker.style.left = '0%';
    nextFrame(tick);
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
    return Object.assign(base, { id: M.uuid(), categoryId, name, viewId: 0, favorite: true });
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
  function addCategory(parentGroup) {
    if (!P() || readOnly()) return;
    namedDialog('Add category', 'New category', name => {
      const group = { id: M.uuid(), name, cameraIds: [] };
      const library=parentGroup&&parentGroup.id?parentGroup:category();
      if(library&&library.airportIcao)group.airportIcao=library.airportIcao;
      const template=library&&!sameCameraLibrary(library,category())?camIn(library.cameraIds[0]):current();
      const view = newCamera(group.id, 'New view', template);
      group.cameraIds.push(view.id);
      P().categories.push(group);
      P().cameras.push(view);
      S.expanded[group.id] = true;
      queueOps([{ op: 'setCamera', camera: view }, { op: 'setCategories', categories: P().categories }]);
      selectCamera(view.id);
    }, 'Add category');
  }
  function addExternalMotionViews() {
    if(!P()||readOnly())return;
    if(P().cameras.length>492||P().categories.length>98){announce('This profile has no room for the eight external views.', 'warn');return;}
    const group={id:M.uuid(),name:'External motion',icon:'view-orbit',cameraIds:[]};
    const walk={id:M.uuid(),name:'Walkaround',icon:'view-wheel',cameraIds:[]},ops=[];
    const insert=(cat,name,mode,pos,heading,pitch)=>{
      const view=newCamera(cat.id,name);view.mode=mode;view.interaction=false;view.trackingPolicy='off';
      view.requiredCapabilities=[];view.positionMetersFRD=pos;view.orientationCameraToAnchorXYZW=M.quatFromHpb(heading,pitch,0);
      view.icon=viewIcon(name,false);cat.cameraIds.push(view.id);P().cameras.push(view);ops.push({op:'setCamera',camera:view});return view;
    };
    const orbit=insert(group,'Orbit','orbit',[35,0,-8],180,-13);orbit.motion={radiusMeters:35,heightMeters:8,durationSeconds:30,clockwise:true};
    const flyby=insert(group,'Fly-by','flyby',[150,50,-15],180,0);flyby.motion={radiusMeters:150,heightMeters:15,durationSeconds:30,sideMeters:35,repeat:false};
    [['Nose',22,0],['Right front',14,20],['Right rear',-14,20],['Tail',-25,0],['Left rear',-14,-20],['Left front',14,-20]].forEach(([label,f,r])=>insert(walk,'Walkaround - '+label,'aircraftExterior',[f,r,-2],Math.atan2(-r,-f)*180/Math.PI,-2));
    group.defaultCameraId=group.cameraIds[0];walk.defaultCameraId=walk.cameraIds[0];P().categories.push(group,walk);
    ops.push({op:'setCategories',categories:P().categories});queueOps(ops);selectCamera(orbit.id);render();
    announce('Orbit, fly-by and six walkaround views added. Adjust them for your aircraft, then Save.');
  }
  function walkaroundCategories() {
    return P()?P().categories.filter(g=>g.cameraIds.filter(id=>camIn(id)&&camIn(id).mode==='aircraftExterior').length>=2):[];
  }
  function walkaroundAvailable(view) {
    return Boolean(view&&view.mode==='aircraftExterior'&&walkaroundCategories().some(g=>g.id===view.categoryId));
  }
  function renderMotionSettings(view) {
    const parent=$('mc-detail-motion');if(!parent)return;
    let host=$('mc-external-motion');if(!host){host=make('section',undefined,'mc-external-motion');host.id='mc-external-motion';parent.insertBefore(host,parent.firstChild);}
    const motionKey=view?view.id+':'+view.mode:'';
    if(host.dataset.cameraMode===motionKey&&host.contains(document.activeElement))return;
    host.dataset.cameraMode=motionKey;empty(host);if(!view)return;
    const orbit=view.mode==='orbit',flyby=view.mode==='flyby';
    if(orbit||flyby){
      add(host,make('h4',orbit?'Orbit settings':'Fly-by settings'),make('p',orbit?'Circle the aircraft while keeping it in view.':'Move the camera past the aircraft, even while parked. Recall this view to restart the pass. Global aircraft tracking controls whether the camera follows the aircraft.','mc-note'));
      const settings=Object.assign({radiusMeters:orbit?35:150,heightMeters:orbit?8:15,durationSeconds:30,clockwise:true,sideMeters:35,repeat:false},view.motion||{});
      const speed=make('p',undefined,'mc-note');
      const updateSpeed=()=>{const m=Object.assign({},settings,view.motion||{});speed.textContent='Average pass speed: '+(2*m.radiusMeters/m.durationSeconds).toFixed(1)+' m/s. A shorter duration makes the camera move faster.';};
      const fields=[['radiusMeters',orbit?'Radius (m)':'Distance each side (m)',2,5000],['heightMeters','Height above aircraft (m)',1,2000],['durationSeconds',orbit?'Seconds per orbit':'Seconds per pass',5,600]];
      if(flyby)fields.push(['sideMeters','Side offset (m)',-5000,5000]);
      fields.forEach(([key,label,min,max])=>{const row=make('label'),input=make('input');Object.assign(input,{type:'number',min,max,step:1,value:settings[key],disabled:readOnly()});input.setAttribute('aria-label',label);input.addEventListener('change',()=>{if(input.value===''||!isFinite(Number(input.value)))return;view.motion=Object.assign({},settings,view.motion||{},{[key]:M.clamp(Number(input.value),min,max)});saveCamera(view);updateSpeed();});add(row,make('span',label),input);host.appendChild(row);});
      if(orbit){const b=button(settings.clockwise?'Clockwise':'Counterclockwise',()=>{view.motion=Object.assign({},settings,view.motion||{},{clockwise:!(view.motion&&typeof view.motion.clockwise==='boolean'?view.motion.clockwise:settings.clockwise)});saveCamera(view);b.blur();renderMotionSettings(view);},'mc-secondary','view-orbit');b.disabled=readOnly();host.appendChild(b);}
      if(flyby){
        updateSpeed();host.appendChild(speed);host.appendChild(make('p','Positive side offset passes to the right; negative passes to the left.','mc-note'));
        const repeat=button('',()=>{view.motion=Object.assign({},settings,view.motion||{},{repeat:!(view.motion&&typeof view.motion.repeat==='boolean'?view.motion.repeat:settings.repeat)});saveCamera(view);updateRepeat();},'mc-secondary');
        const updateRepeat=()=>{const on=view.motion&&typeof view.motion.repeat==='boolean'?view.motion.repeat:settings.repeat;repeat.textContent='Repeat back and forth: '+(on?'On':'Off');repeat.setAttribute('aria-pressed',String(on));};updateRepeat();repeat.disabled=readOnly();host.appendChild(repeat);
      }
    }
    if(walkaroundAvailable(view))host.appendChild(button(S.walkaround.running?'Walkaround: running':'Walkaround settings',walkaroundDialog,'mc-secondary','view-wheel'));
  }  function walkaroundDialog() {
    const groups=walkaroundCategories();
    const anchor=document.activeElement&&document.activeElement.getBoundingClientRect?document.activeElement.getBoundingClientRect():null;
    const body=openDialog('Walkaround'),shade=$('mc-dialog-shade');
    shade.classList.add('mc-walkaround-shade');
    body.appendChild(make('strong',S.walkaround.running?'Sequence running':'Sequence stopped','mc-walkaround-status'));
    body.appendChild(make('p','Advance through exterior cameras in category order. Moving or looking pauses the timer; selecting a camera stops the sequence.','mc-note'));
    const place=()=>{
      compactEntityMenu(anchor);
      if(IS_MINI||miniDialogOwned)nextFrame(()=>{
        if(shade.hidden||!shade.classList.contains('mc-walkaround-shade'))return;
        const menu=shade.querySelector('.mc-dialog'),dock=$('mc-mini-panel').getBoundingClientRect();
        menu.style.top=Math.max(8,dock.top-menu.offsetHeight-8)+'px';
        if(window.fscsUpdateMiniRect)window.fscsUpdateMiniRect();
      });
    };
    if(!groups.length){body.appendChild(make('p','Add at least two exterior cameras to a category, or add External motion views from the library + menu.'));dialogActions([['Done',()=>closeDialog()]]);place();return;}
    let selected=groups.find(g=>g.id===S.walkaround.categoryId)||groups.find(g=>/walkaround/i.test(g.name))||groups[0];
    body.appendChild(make('h4','Camera category'));
    const list=make('div',undefined,'mc-look-choices');list.setAttribute('role','group');list.setAttribute('aria-label','Walkaround category');
    groups.forEach(g=>{const b=button(g.name,()=>{selected=g;Array.from(list.children).forEach(el=>el.setAttribute('aria-pressed',String(el===b)));},'mc-secondary');b.setAttribute('aria-pressed',String(g===selected));list.appendChild(b);});body.appendChild(list);
    const row=make('label',undefined,'mc-walkaround-timing'),seconds=make('input'),value=make('span',undefined,'mc-walkaround-duration');
    Object.assign(seconds,{type:'number',min:1,max:120,step:1,value:S.walkaround.intervalSeconds||5});seconds.setAttribute('aria-label','Seconds at each camera');
    add(value,seconds,make('span','seconds'));add(row,make('span','Time at each camera'),value);body.appendChild(row);
    let repeating=S.walkaround.loop!==false;
    const loop=button('',()=>{repeating=!repeating;updateLoop();},'mc-secondary mc-walkaround-repeat');
    const updateLoop=()=>{loop.textContent=repeating?'Repeat: On':'Repeat: Off';loop.setAttribute('aria-pressed',String(repeating));};updateLoop();body.appendChild(loop);
    const start=async()=>{
      const interval=Number(seconds.value);if(!isFinite(interval)||interval<1||interval>120){dialogError('Choose 1 to 120 seconds.');seconds.focus();return;}
      await whenIdle();const ack=await command('StartWalkaround',{categoryId:selected.id,intervalSeconds:interval,loop:repeating});
      if(ack.status==='Rejected')dialogError(reasonText(ack.reason));else closeDialog();
    };
    const stop=async()=>{const ack=await command('StopWalkaround',{});if(ack.status==='Rejected')dialogError(reasonText(ack.reason));else closeDialog();};
    const actions=[['Done',()=>closeDialog()]];
    if(S.walkaround.running)actions.push(['Restart sequence',start,'mc-secondary'],['Stop walkaround',stop,'mc-primary']);
    else actions.push(['Start walkaround',start,'mc-primary']);
    dialogActions(actions);place();
  }  function renameCamera(view) {
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
    P().categories.filter(g => g.id !== original.id && sameCameraLibrary(original,g)).forEach(g => { const o = make('option', g.name); o.value = g.id; select.appendChild(o); });
    label.appendChild(select);
    body.appendChild(label);
    body.appendChild(make('p',original.airportIcao?'This camera belongs to the shared '+original.airportIcao+' airport library.':'Aircraft cameras stay in this aircraft profile.','mc-note'));
    dialogActions([['Cancel', () => closeDialog()], ['Move camera', () => {
      const target = catIn(select.value);
      if (!target || !sameCameraLibrary(original,target)) { dialogError('Choose a category in the same camera library.'); return; }
      if (original.cameraIds.length === 1) { dialogError('Keep at least one camera in each category.'); return; }
      original.cameraIds.splice(original.cameraIds.indexOf(view.id), 1);
      if(original.defaultCameraId===view.id)delete original.defaultCameraId;
      target.cameraIds.push(view.id);
      view.categoryId = target.id;
      S.expanded[target.id] = true;
      closeDialog();
      queueOps([{ op: 'setCamera', camera: view }, { op: 'setCategories', categories: P().categories }]);
      render();
    }, 'mc-primary']]);
  }
  // Store only a copied snapshot of settings, never identity, category order, bindings or defaults.
  const CAMERA_SETTINGS_KEY='FSCS.cameraSettingsClipboard.v1';
  let cameraSettingsClipboard=null;
  function readCameraSettingsClipboard() {
    try {
      const value=JSON.parse(window.localStorage.getItem(CAMERA_SETTINGS_KEY)||'null');
      if(value&&value.version===1&&value.camera&&value.source)cameraSettingsClipboard=value;
    } catch(error) { /* Some simulator hosts disable browser storage; the current panel still works. */ }
    return cameraSettingsClipboard;
  }
  function copyCameraSettings(view) {
    if(!view||!P())return;
    cameraSettingsClipboard={version:1,source:{name:view.name,modelKey:(P().aircraftMatch||{}).modelKey||null,calibration:clone(P().calibration||null)},camera:{}};
    const fields=['mode','airportIcao','worldOrigin','seedDefinitionName','positionMetersFRD','orientationCameraToAnchorXYZW','projection','transition','effects','interaction','trackingPolicy','temporaryLookPolicy','mouseLookOnRelease','levelHorizon','aircraftTracking'];
    fields.forEach(key=>{if(view[key]!==undefined)cameraSettingsClipboard.camera[key]=clone(view[key]);});
    try {window.localStorage.setItem(CAMERA_SETTINGS_KEY,JSON.stringify(cameraSettingsClipboard));}catch(error){/* In-memory fallback. */}
    announce('Copied settings from '+view.name+'. Choose another camera and use Paste settings.');
  }
  function cameraSettingsPoseReason(copy,view) {
    const source=copy.camera;
    if(source.mode!==view.mode)return 'Position and orientation need the same camera type.';
    if(view.mode==='airport') {
      if(!source.airportIcao||source.airportIcao!==view.airportIcao)return 'Position and orientation can only be pasted within the same airport.';
      if(!source.worldOrigin||!view.worldOrigin||!same(source.worldOrigin,view.worldOrigin))return 'These airport views use different world origins.';
    } else {
      const model=(P().aircraftMatch||{}).modelKey||null;
      if(!copy.source.modelKey||copy.source.modelKey!==model)return 'Position and orientation need the same aircraft model.';
      if(!same(copy.source.calibration||null,P().calibration||null)||(source.seedDefinitionName||null)!==(view.seedDefinitionName||null))return 'These views use different camera anchors.';
    }
    return '';
  }
  function pasteCameraSettings(view) {
    const copy=clone(readCameraSettingsClipboard());
    if(!copy||!view||readOnly())return;
    const body=openDialog('Paste camera settings');
    body.appendChild(make('p','From '+copy.source.name+' to '+view.name+'. Choose what to replace.'));
    body.appendChild(make('p','The destination name, icon, color, category, shortcuts and default status stay unchanged. Global tracking settings are not copied.','mc-note'));
    const profileId=P().profileId,poseReason=cameraSettingsPoseReason(copy,view),choices={};
    [['pose','Position and orientation',poseReason],['lens','Lens / field of view',''],['motion','Transition and motion effects',''],['behavior','Mouse look and camera behavior','']].forEach(([key,title,reason])=>{
      const label=make('label',undefined,'mc-paste-setting'),input=make('input');input.type='checkbox';input.checked=!reason;input.disabled=!!reason;input.dataset.pasteSetting=key;
      add(label,input,make('span',title));if(reason)label.title=reason;
      body.appendChild(label);choices[key]=input;
    });
    if(poseReason)body.appendChild(make('p',poseReason+' Lens, transition and compatible behavior can still be pasted.','mc-note'));
    dialogActions([['Cancel',()=>closeDialog()],['Paste settings',()=>{
      const destination=camIn(view.id);
      if(!destination||!P()||P().profileId!==profileId||readOnly()){dialogError('The destination camera changed. Open Paste settings again.');return;}
      if(choices.pose.checked&&cameraSettingsPoseReason(copy,destination)){dialogError(cameraSettingsPoseReason(copy,destination));return;}
      if(!Object.keys(choices).some(key=>choices[key].checked)){dialogError('Choose at least one group of settings.');return;}
      const next=clone(destination),source=copy.camera;
      const assign=keys=>keys.forEach(key=>{if(source[key]!==undefined)next[key]=clone(source[key]);else delete next[key];});
      if(choices.pose.checked)assign(['positionMetersFRD','orientationCameraToAnchorXYZW']);
      if(choices.lens.checked)assign(['projection']);
      if(choices.motion.checked){assign(['transition']);if(source.mode===next.mode)assign(['effects']);}
      if(choices.behavior.checked){
        assign(['temporaryLookPolicy','mouseLookOnRelease','levelHorizon']);
        if(source.mode===next.mode)assign(['interaction']);
      }
      if(next.mode!=='airport')next.aircraftTracking='off';
      applyOpsLocal([{op:'setCamera',camera:next}]);saveCamera(next);S.angleCache=null;closeDialog();render();
      announce('Pasted settings to '+next.name+'. Save changes to keep them.');
    },'mc-primary']]);
    compactEntityMenu();
  }
  function addEntityAppearance(body, entity, isCategory) {
    const list=body.querySelector('.mc-action-list'),appearance=make('details',undefined,'mc-context-appearance');
    const summary=make('summary');add(summary,icon('aperture'),make('span','Appearance'));appearance.appendChild(summary);
    const content=make('div',undefined,'mc-context-appearance-body');
    add(content,button('Choose icon',()=>chooseEntityIcon(entity,isCategory),'','aperture'),entityColorPalette(entity,isCategory));appearance.appendChild(content);
    const destructive=list.lastElementChild;destructive.classList.add('mc-context-delete');
    list.insertBefore(appearance,destructive);
    appearance.addEventListener('toggle',()=>{nextFrame(()=>{
      const menu=body.closest('.mc-dialog'),rect=menu.getBoundingClientRect();
      if(rect.bottom>window.innerHeight-8)menu.style.top=Math.max(8,window.innerHeight-menu.offsetHeight-8)+'px';
      updateScrollbars();if(window.fscsUpdateMiniRect)window.fscsUpdateMiniRect();
    });});
  }
  function cameraActions() {
    const view = current(), group = category();
    if (!view || readOnly()) return;
    const index = group.cameraIds.indexOf(view.id);
    const body = actionDialog(view.name, [
      ['Assign key binding', 'keyboard', () => assignCameraBinding(view)],
      [view.favorite?'Remove star (hide in mini panel)':'Star camera (show in mini panel)', 'star', () => {setFavorite(view);closeDialog();}],
      ['Copy settings', 'copy', () => copyCameraSettings(view),false,'Copy this camera’s edited pose, lens, motion and behavior settings.'],
      ['Paste settings', 'paste', () => pasteCameraSettings(view),!readCameraSettingsClipboard(),'Choose copied settings to apply to this camera. Identity and shortcuts are preserved.'],
      ['Rename', 'pencil', () => renameCamera()],
      [group.defaultCameraId===view.id?'Category default':'Make category default', 'check-circle-2', () => setCategoryDefault(view),group.defaultCameraId===view.id],
      ['Duplicate camera', 'copy', () => {
        const duplicate = clone(view);
        duplicate.id = M.uuid(); duplicate.name = (view.name + ' copy').slice(0, 60); duplicate.viewId = 0; duplicate.favorite = true;
        P().cameras.push(duplicate);
        group.cameraIds.splice(index + 1, 0, duplicate.id);
        queueOps([{ op: 'setCamera', camera: duplicate }, { op: 'setCategories', categories: P().categories }]);
        selectCamera(duplicate.id);
      }],
      ['Move up', 'arrow-up', () => moveCamera(-1), index === 0],
      ['Move down', 'arrow-down', () => moveCamera(1), index === group.cameraIds.length - 1],
      ['Move to category', 'folder-input', moveCameraCategory, !P().categories.some(g=>g.id!==group.id&&sameCameraLibrary(group,g))],
      ['Delete camera', 'trash-2', () => confirmDialog('Delete camera?', 'Remove "' + view.name + '" from this profile?', () => {
        group.cameraIds = group.cameraIds.filter(id => id !== view.id);
        if(group.defaultCameraId===view.id)delete group.defaultCameraId;
        P().cameras = P().cameras.filter(c => c.id !== view.id);
        const ops = [{ op: 'removeCamera', cameraId: view.id }, { op: 'setCategories', categories: P().categories }];
        if (P().defaultCameraId === view.id) { P().defaultCameraId = group.cameraIds[0]; ops.push({ op: 'setProfileFields', defaultCameraId: P().defaultCameraId }); }
        queueOps(ops);
        selectCamera(group.cameraIds[0], { preserveExpansion: true });
      }), group.cameraIds.length === 1],
    ]);
    addEntityAppearance(body,view,false);
    if(view.mode==='airport'){const list=body.querySelector('.mc-action-list');list.insertBefore(button('Airport camera settings',()=>airportCameraSettings(view),'','view-tower'),list.querySelector('.mc-context-appearance'));}
    compactEntityMenu();
  }
  function moveCategory(group, delta) {
    const list = P().categories, siblings=list.filter(g=>sameCameraLibrary(group,g)), sibling=siblings[siblings.indexOf(group)+delta];
    if(!sibling)return;
    const from = list.indexOf(group), to = list.indexOf(sibling);
    if (to < 0 || to >= list.length) return;
    list[from] = list[to]; list[to] = group;
    saveCategories();
    render();
  }
  function categoryActions(group) {
    if (readOnly()) return;
    const list = P().categories.filter(g=>sameCameraLibrary(group,g));
    const body = actionDialog(group.name, [
      ['Move category up', 'arrow-up', () => moveCategory(group, -1), list.indexOf(group) === 0],
      ['Move category down', 'arrow-down', () => moveCategory(group, 1), list.indexOf(group) === list.length - 1],
      ['Rename category', 'pencil', () => namedDialog('Rename category', group.name, name => { group.name = name; saveCategories(); render(); })],
      ['Add category', 'folder-plus', ()=>addCategory(group)],
      ['Delete category', 'trash-2', () => confirmDialog('Delete category?', 'Remove "' + group.name + '" and its ' + group.cameraIds.length + ' cameras?', () => {
        const removed = group.cameraIds.slice();
        P().categories = P().categories.filter(g => g.id !== group.id);
        P().cameras = P().cameras.filter(c => removed.indexOf(c.id) < 0);
        const ops = removed.map(id => ({ op: 'removeCamera', cameraId: id }));
        ops.push({ op: 'setCategories', categories: P().categories });
        if (removed.indexOf(P().defaultCameraId) >= 0) { P().defaultCameraId = cameras()[0].id; ops.push({ op: 'setProfileFields', defaultCameraId: P().defaultCameraId }); }
        queueOps(ops);
        // Retain the selection when deleting another category and never reopen collapsed groups.
        if (!camIn(S.selection.cameraId)) selectCamera(cameras()[0].id, { preserveExpansion: true });
        else render();
        delete S.expanded[group.id];
      }), list.length === 1],
    ]);
    addEntityAppearance(body,group,true);
    compactEntityMenu();
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
  function captureView(saveImmediately) {
    const view = current();
    if (!view) return;
    S.pendingCapture = { cameraId: view.id, profileId: P().profileId, saveImmediately: saveImmediately === true };
    command('CaptureView', { cameraId: view.id }).then(ack => { if (ack.status === 'Rejected') S.pendingCapture = null; });
  }
  function onViewCaptured(p) {
    const pending = S.pendingCapture;
    if (!pending) return;
    S.pendingCapture = null;
    if (!P() || P().profileId !== pending.profileId) return;
    if (!p.ok) { announce(p.message || 'The current view could not be read.', 'error'); return; }
    const view = camIn(pending.cameraId);
    if (!view) return;
    if (pending.saveImmediately && p.qualified) {
      applyCapturedPose(view, p);
      saveChanges();
      return;
    }
    const body = openDialog('Use the captured view?');
    body.appendChild(make('p', 'Values read from the simulator for "' + view.name + '". ' + (pending.saveImmediately ? 'Use and save writes these values to this camera.' : 'Use these values applies them to the draft. Save all changes keeps them.')));
    if (!p.qualified) {
      const warn = make('p', undefined, 'mc-warn');
      add(warn, make('strong', 'Not qualified: '), p.message || 'View capture is not qualified for this simulator build. The values may be inaccurate.');
      body.appendChild(warn);
    } else if (p.message) body.appendChild(make('p', p.message));
    const ui = Array.isArray(p.positionMetersFRD) ? M.uiFromFrd(p.positionMetersFRD) : null;
    const hpb = Array.isArray(p.orientationCameraToAnchorXYZW) ? M.hpbFromQuat(p.orientationCameraToAnchorXYZW) : null;
    const kv = make('dl', undefined, 'mc-kv');
    const row = (k, v) => add(kv, make('dt', k), make('dd', v));
    if (ui) row('X | Y | Z', fmt(ui.x, 3) + ' | ' + fmt(ui.y, 3) + ' | ' + fmt(ui.z, 3) + ' m');
    if (hpb) row('Pitch | Heading | Roll', fmt(hpb.pitch, 1) + ' | ' + fmt(hpb.heading, 1) + ' | ' + fmt(hpb.bank, 1) + ' deg');
    if (isFinite(p.fovRad)) row('Field of view', fmtFov(M.radToDeg(p.fovRad)) + ' deg sim FOV');
    row('Qualified', p.qualified ? 'Yes' : 'No');
    body.appendChild(kv);
    dialogActions([['Cancel', () => closeDialog()], [pending.saveImmediately ? 'Use and save' : 'Use these values', () => {
      closeDialog();
      const target = camIn(pending.cameraId);
      if (!target) return;
      applyCapturedPose(target, p);
      if (pending.saveImmediately) saveChanges();
      else announce('Captured view applied to "' + target.name + '". Save changes to keep it.');
    }, 'mc-primary']]);
  }
  function applyCapturedPose(target, p) {
    if (Array.isArray(p.positionMetersFRD)) target.positionMetersFRD = p.positionMetersFRD.slice(0, 3);
    if (Array.isArray(p.orientationCameraToAnchorXYZW)) target.orientationCameraToAnchorXYZW = M.quatNormalize(p.orientationCameraToAnchorXYZW);
    if (isFinite(p.fovRad)) target.projection = Object.assign({}, target.projection, { value: p.fovRad });
    S.angleCache = null;
    saveCamera(target);
    renderDetails();
  }

  /* ------------- Dock ------------- */

  function trackingPreference(view, value) {
    if(!S.working)return;
    S.working.settings.host.globalHeadTrackingEnabled=value==='on';
    settingChanged();renderCameraTools();
  }
  function trackingOptions() {
    if(!S.working)return;
    const body=openDialog('Global head tracking');
    body.appendChild(make('p','One head-tracking setting for every camera and profile. TrackIR and compatible MSFS inputs are off by default. Your original simulator setting is restored when camera control is released.','mc-note'));
    [['on','On for all cameras'],['off','Off for all cameras']].forEach(([value,label])=>{
      const b=button(label,()=>{trackingPreference(null,value);closeDialog();render();},'mc-secondary');b.setAttribute('aria-pressed',String((S.working.settings.host.globalHeadTrackingEnabled===true)===(value==='on')));body.appendChild(b);
    });
    const status=S.caps.headTracking||{};if(status.error)body.appendChild(make('p',status.error,'mc-warn'));
    body.appendChild(make('p','This controls the MSFS tracking switch; it does not detect whether a physical tracker is connected. VR tracking and tools that move the camera independently are separate.','mc-note'));
    dialogActions([['Close',()=>closeDialog()]]);
  }
  function aircraftTrackingOptions(view) {
    if(!S.working)return;const body=openDialog('Global aircraft tracking');
    const shade=$('mc-dialog-shade');shade.classList.add('mc-tracking-shade');
    body.appendChild(make('p','All exterior and airport cameras','mc-tracking-camera'));
    body.appendChild(make('p','Shared across every camera and profile. Heading and pitch tracking is on by default. Cockpit cameras never track the aircraft. Right-drag temporarily overrides tracking.','mc-note'));
    [['off','Fixed direction','Keep the saved view direction.'],['heading','Track heading only','Follow left and right; keep the saved pitch.'],['both','Track heading and pitch','Keep your aircraft in the center of the view.']].forEach(([value,label,detail])=>{
      const b=button(label,()=>{S.working.settings.host.globalAircraftTracking=value;settingChanged();closeDialog();render();},'mc-secondary mc-tracking-choice');
      b.setAttribute('aria-label',label);b.setAttribute('aria-pressed',String((S.working.settings.host.globalAircraftTracking||'both')===value));
      b.appendChild(make('small',detail));body.appendChild(b);
    });
    if(view&&view.mode==='airport')body.appendChild(button('This airport camera’s settings',()=>airportCameraSettings(view),'mc-secondary'));
    dialogActions([['Close',()=>closeDialog()]]);
    if(IS_MINI) {
      revealMini();
      const place=()=>{if(shade.hidden||!shade.classList.contains('mc-tracking-shade'))return;
        const menu=shade.querySelector('.mc-dialog'),dock=$('mc-mini-panel').getBoundingClientRect();
        menu.style.left=Math.max(8,Math.min(innerWidth-menu.offsetWidth-8,dock.right-menu.offsetWidth))+'px';
        menu.style.top=Math.max(8,dock.top-menu.offsetHeight-8)+'px';
        if(window.fscsUpdateMiniRect)window.fscsUpdateMiniRect();
      };nextFrame(place);
    }
  }
  function airportCameraSettings(view) {
    if(!view||view.mode!=='airport'||readOnly())return;
    const body=openDialog('Airport camera settings');
    $('mc-dialog-shade').classList.add('mc-tracking-shade','mc-airport-override-shade');
    body.appendChild(make('p',view.name,'mc-tracking-camera'));
    body.appendChild(make('p','Global values follow your airport defaults. Turn off Global to customize this camera.','mc-note'));
    const host=S.working.settings.host,overrides=()=>view.airportOverrides||{};
    const save=(key,value)=>{
      view.airportOverrides=Object.assign({},overrides());
      if(value===undefined)delete view.airportOverrides[key];else view.airportOverrides[key]=value;
      if(value!==undefined&&key==='autoZoomMinFovDeg'&&Object.prototype.hasOwnProperty.call(view.airportOverrides,'autoZoomMaxFovDeg'))view.airportOverrides.autoZoomMaxFovDeg=Math.max(value,view.airportOverrides.autoZoomMaxFovDeg);
      if(value!==undefined&&key==='autoZoomMaxFovDeg'&&Object.prototype.hasOwnProperty.call(view.airportOverrides,'autoZoomMinFovDeg'))view.airportOverrides.autoZoomMinFovDeg=Math.min(value,view.airportOverrides.autoZoomMinFovDeg);
      if(!Object.keys(view.airportOverrides).length)delete view.airportOverrides;
      saveCamera(view);renderCameraTools();
    };
    const section=(title)=>{const group=make('section',undefined,'mc-override-section');group.appendChild(make('h4',title));body.appendChild(group);return group;};
    const trackingGroup=section('Tracking'),zoomGroup=section('Auto zoom'),heightGroup=section('Height');
    const select=(group,key,label,options)=>{
      const row=make('label',undefined,'mc-override-select'),input=make('select');input.setAttribute('aria-label',label);
      options.forEach(([value,text])=>{const option=make('option',text);option.value=value;input.appendChild(option);});
      input.value=Object.prototype.hasOwnProperty.call(overrides(),key)?String(overrides()[key]):'global';
      input.addEventListener('change',()=>save(key,input.value==='global'?undefined:(key==='autoZoomEnabled'||key==='trackingHorizonGuard')?input.value==='true':input.value));
      add(row,make('span',label),input);group.appendChild(row);
    };
    const modeNames={off:'Off',heading:'Heading only',both:'Heading + pitch'};
    select(trackingGroup,'aircraftTracking','Aircraft tracking',[['global','Global · '+modeNames[host.globalAircraftTracking||'both']],['off','Override · Off'],['heading','Override · Heading only'],['both','Override · Heading + pitch']]);
    select(trackingGroup,'trackingHorizonGuard','Keep distant views near the horizon',[['global','Global · '+(host.airportTrackingHorizonGuard!==false?'On':'Off')],['true','Override · On'],['false','Override · Off']]);
    trackingGroup.appendChild(make('p','Softens steep pitch at long distances caused by elevation differences. Heading still follows your aircraft. Turn off for unrestricted vertical tracking.','mc-note'));
    select(zoomGroup,'autoZoomEnabled','Auto zoom',[['global','Global · '+(host.airportAutoZoomEnabled?'On':'Off')],['true','Override · On'],['false','Override · Off']]);
    const categoryKey=view.airportHeight&&view.airportHeight.category||airportHeightCategory(view);
    const heights=Object.assign({runways:3,gates:3,apron:3,taxi:3,tower:35},host.airportCategoryHeights||{});
    const zoomFields=make('div',undefined,'mc-override-grid');zoomGroup.appendChild(zoomFields);
    const fields=[['heightMeters','Height above feature',Math.max(heights[categoryKey],Number((view.airportHeight||{}).minimumMeters)||0),0,1000,1,'m'],
      ['autoZoomFrameMeters','Framing size',host.airportAutoZoomFrameMeters==null?40:host.airportAutoZoomFrameMeters,5,200,1,'m'],
      ['autoZoomMinFovDeg','Closest zoom (minimum FOV)',host.airportAutoZoomMinFovDeg==null?10:host.airportAutoZoomMinFovDeg,2,120,1,'deg'],
      ['autoZoomMaxFovDeg','Widest zoom (maximum FOV)',host.airportAutoZoomMaxFovDeg==null?90:host.airportAutoZoomMaxFovDeg,2,120,1,'deg'],
      ['autoZoomResponseSeconds','Smoothing time',host.airportAutoZoomResponseSeconds==null?1:host.airportAutoZoomResponseSeconds,0.1,5,0.1,'s']];
    fields.forEach(([key,label,fallback,min,max,step,unit])=>{
      const row=make('div',undefined,'mc-override-field'),caption=make('label',undefined,'mc-override-inherit'),inherit=make('input');inherit.type='checkbox';inherit.setAttribute('aria-label','Use global '+label.toLowerCase());
      const legacyHeight=key==='heightMeters'&&!view.airportHeight;
      inherit.checked=!legacyHeight&&!Object.prototype.hasOwnProperty.call(overrides(),key);
      const input=make('input');Object.assign(input,{type:'number',min,max,step,value:legacyHeight?M.clamp(-view.positionMetersFRD[2],min,max):inherit.checked?fallback:overrides()[key],disabled:inherit.checked});input.setAttribute('aria-label',label);
      const source=make('span',inherit.checked?'Global':'Override','mc-override-source');
      const prepareHeight=()=>{if(key==='heightMeters'&&!view.airportHeight)view.airportHeight={category:categoryKey,baseAltitudeMeters:Number((view.worldOrigin||{}).altitude)||0,minimumMeters:0};};
      inherit.addEventListener('change',()=>{prepareHeight();save(key,inherit.checked?undefined:Number(input.value));input.disabled=inherit.checked;if(inherit.checked)input.value=String(fallback);source.textContent=inherit.checked?'Global':'Override';});
      input.addEventListener('change',()=>{prepareHeight();const n=Number(input.value),value=input.value.trim()!==''&&isFinite(n)?M.clamp(n,min,max):fallback;input.value=String(value);save(key,value);if(key==='autoZoomMinFovDeg'||key==='autoZoomMaxFovDeg')airportCameraSettings(view);});
      add(caption,inherit,source);const line=make('div',undefined,'mc-override-value');add(line,input,make('span',unit,'mc-note'),caption);
      add(row,make('span',label,'mc-override-label'),line);(key==='heightMeters'?heightGroup:zoomFields).appendChild(row);
    });
    if(!view.airportHeight)heightGroup.appendChild(make('p','Saved altitude is retained until you choose a height option.','mc-note'));
    body.appendChild(button('Use global settings for everything',()=>{if(!view.airportHeight)view.airportHeight={category:categoryKey,baseAltitudeMeters:Number((view.worldOrigin||{}).altitude)||0,minimumMeters:0};delete view.airportOverrides;saveCamera(view);airportCameraSettings(view);},'mc-secondary mc-override-reset'));
    dialogActions([['Close',()=>closeDialog()]]);
    if(IS_MINI)nextFrame(()=>{const menu=$('mc-dialog-shade').querySelector('.mc-dialog'),dock=$('mc-mini-panel').getBoundingClientRect();menu.style.left=Math.max(8,Math.min(innerWidth-menu.offsetWidth-8,dock.right-menu.offsetWidth))+'px';menu.style.top=Math.max(8,dock.top-menu.offsetHeight-8)+'px';if(window.fscsUpdateMiniRect)window.fscsUpdateMiniRect();});
  }
  function airportHeightCategory(view) {
    const key=view.generatedKey||'',name=((categoryOf(view.id)||{}).name||'').toLowerCase();
    return key.indexOf('runway:')===0||name.indexOf('runway')>=0?'runways':key==='tower'||name.indexOf('tower')>=0?'tower':key.indexOf('sign:')===0||key.indexOf('junction:')===0||name.indexOf('taxi')>=0?'taxi':name.indexOf('gate')>=0?'gates':'apron';
  }
  async function saveAdjustedViews() {
    if(!S.snapshot||S.pendingCommit)return;
    await whenIdle();
    if(S.ownership.state==='Active') {
      const ack=await command('BakeTemporaryLook',{});
      if(ack.status!=='Completed'){announce(reasonText(ack.reason),'error');return;}
    }
    await saveChanges();
  }
  function renderCameraTools() {
    [['mc-editor-mouse-look',current],['mc-mouse-look',dockCamera]].forEach(([anchorId,getView])=>{
      const anchor=$(anchorId);if(!anchor)return;setIcon(anchor,'mouse');const view=getView(),prefix=anchorId+'-',editor=anchorId==='mc-editor-mouse-look';
      const ensure=(key,label,action)=>{let b=$(prefix+key);if(!b){b=button(label,action,'mc-secondary');b.id=prefix+key;anchor.parentNode.insertBefore(b,anchor);}return b;};
      let globalTools=$(prefix+'global');
      if(!globalTools){
        globalTools=make('div',undefined,'mc-global-controls mc-tool-group');globalTools.id=prefix+'global';globalTools.setAttribute('role','group');globalTools.setAttribute('aria-label','Global camera controls');
        globalTools.appendChild(make('span','Global','mc-tool-group-label'));
        if(editor){const header=$('mc-drag-header');header.parentNode.insertBefore(globalTools,header.nextSibling);}else anchor.parentNode.insertBefore(globalTools,anchor);
      }
      const status=S.caps.headTracking||{},host=S.working&&S.working.settings.host||{},enabled=host.globalHeadTrackingEnabled===true;
      const head=ensure('head','Head tracking',()=>trackingPreference(null,S.working.settings.host.globalHeadTrackingEnabled===true?'off':'on'));
      head.textContent='Head tracking: '+(enabled?'On':'Off');setIcon(head,'head-tracking');head.setAttribute('aria-pressed',String(enabled));head.disabled=!S.working;head.title=status.error||'Global head tracking for all cameras and profiles. Save to keep this preference.';
      const tracking=ensure('aircraft','Aircraft tracking',()=>aircraftTrackingOptions(null));const aircraftTracking=host.globalAircraftTracking||'both';
      tracking.hidden=false;tracking.textContent='Aircraft: '+(aircraftTracking==='both'?'Heading + pitch':aircraftTracking==='heading'?'Heading':'Off');setIcon(tracking,'aircraft-tracking');tracking.disabled=!S.working;tracking.setAttribute('aria-pressed',String(aircraftTracking!=='off'));tracking.title='Global aircraft tracking for exterior and airport cameras. Cockpit cameras are excluded; individual airport cameras can override this default.';
      let automatic=$(prefix+'auto');
      if(!automatic){automatic=button('Auto switch',()=>command('SetAirportAuto',{enabled:!S.airports.autoSwitch}),'mc-secondary mc-airport-auto');automatic.id=prefix+'auto';}
      [head,tracking,automatic].forEach(b=>{if(b.parentNode!==globalTools)globalTools.appendChild(b);});
      setIcon(automatic,'auto-switch');automatic.hidden=false;automatic.setAttribute('aria-pressed',String(!!S.airports.autoSwitch));
      automatic.disabled=!S.snapshot||(!S.airports.autoSwitch&&(!cameras().some(c=>c.mode==='airport')||!S.airports.positionKnown));
      automatic.title=automatic.disabled?'Load or generate cameras for a nearby airport first.':'Global: select the nearest airport camera automatically. Camera changes and manual movement pause switching for 3 seconds; releasing camera control pauses it until a camera is active again.';
      const tour=ensure('walkaround','Walkaround',walkaroundDialog);setIcon(tour,'view-wheel');tour.hidden=!walkaroundAvailable(view);tour.setAttribute('aria-pressed',String(S.walkaround.running));tour.title='Start, stop or configure an automatic tour of exterior cameras';
      let effective=$(prefix+'effective');
      if(!effective){effective=make('span',undefined,'mc-effective-tracking');effective.id=prefix+'effective';anchor.parentNode.insertBefore(effective,anchor);}
      const override=view&&view.mode==='airport'&&view.airportOverrides&&view.airportOverrides.aircraftTracking;
      const effectiveTracking=override||aircraftTracking;
      effective.hidden=!view||(!editor&&view.mode!=='airport');
      effective.textContent=!view?'':view.mode==='aircraftCockpit'?'Cockpit · aircraft tracking excluded':(view.mode==='airport'?'Airport':'Exterior')+' · '+(effectiveTracking==='off'?'fixed direction':effectiveTracking==='heading'?'tracks heading':'tracks heading + pitch')+(override?' · override':' · global');
      effective.title=override?'This camera overrides global aircraft tracking. Change it in Airport camera settings.':'Effective aircraft tracking for this camera. Cockpit cameras never track the aircraft.';
      if(!editor){
        const save=ensure('save','Save all',saveAdjustedViews);setIcon(save,'save');save.title='Save adjusted view and all pending changes (Ctrl+S)';save.disabled=!S.snapshot||!!S.pendingCommit;
        let viewTools=$(prefix+'view');
        if(!viewTools){viewTools=make('div',undefined,'mc-tool-group mc-view-controls');viewTools.id=prefix+'view';viewTools.setAttribute('role','group');viewTools.setAttribute('aria-label','Current camera actions');viewTools.appendChild(make('span','View','mc-tool-group-label'));globalTools.parentNode.appendChild(viewTools);}
        [tour,anchor,save].forEach(b=>{if(b.parentNode!==viewTools)viewTools.appendChild(b);});
        let panelTools=$(prefix+'panel');
        if(!panelTools){panelTools=make('div',undefined,'mc-tool-group mc-panel-controls');panelTools.id=prefix+'panel';panelTools.setAttribute('role','group');panelTools.setAttribute('aria-label','Panel and camera control');globalTools.parentNode.appendChild(panelTools);}
        ['mc-mini-pin','mc-expand','mc-dock-release'].forEach(id=>{if($(id).parentNode!==panelTools)panelTools.appendChild($(id));});
        if(IS_MINI){
          const dock=$('mc-mini-panel'),tools=globalTools.parentNode,body=dock.querySelector('.mc-dock-body');
          [head,tracking,tour,anchor].forEach(b=>b.classList.add('mc-mini-secondary'));
          let more=$(prefix+'more');
          if(!more){
            more=button('More',()=>{
              const menu=openDialog('Mini panel controls');
              const sections=[['Global settings',[prefix+'head',prefix+'aircraft']],['Camera',[prefix+'walkaround',anchorId]],['Panel',['mc-mini-pin','mc-expand','mc-dock-release']]];
              sections.forEach(([label,ids])=>{
                const available=ids.map($).filter(b=>b&&!b.hidden);if(!available.length)return;
                menu.appendChild(make('div',label,'mc-section-label'));
                const list=make('div',undefined,'mc-action-list');menu.appendChild(list);
                available.forEach(original=>{const action=button(original.textContent.trim(),()=>{closeDialog();original.click();},'mc-secondary');action.disabled=original.disabled;action.title=original.title;const pressed=original.getAttribute('aria-pressed');if(pressed!==null)action.setAttribute('aria-pressed',pressed);list.appendChild(action);});
              });
              const active=dockCamera();if(active&&active.mode==='airport')menu.appendChild(button('Airport camera settings',()=>{closeDialog();airportCameraSettings(active);},'mc-secondary'));
              compactEntityMenu();$('mc-dialog-close').focus();
            },'mc-secondary');more.id=prefix+'more';more.setAttribute('aria-haspopup','dialog');more.title='Head tracking, aircraft tracking, mouse look and panel controls';tools.appendChild(more);
          }
          if(tools.parentNode!==dock)dock.insertBefore(tools,dock.firstChild);
          const nav=dock.querySelector('.mc-dock-current');if(nav.parentNode!==viewTools)viewTools.insertBefore(nav,viewTools.children[1]);
          if(effective.parentNode!==body)body.insertBefore(effective,body.firstChild);
        }
      }
    });
    const select=$('mc-tracking');if(select){select.hidden=true;let b=$('mc-head-tracking-options');if(!b){b=button('Global head-tracking options',trackingOptions,'mc-secondary');b.id='mc-head-tracking-options';select.parentNode.insertBefore(b,select);}const note=select.parentNode.querySelector('small');if(note)note.textContent='Shared by every camera and profile. Head tracking is off by default.';b.disabled=!S.working;}
    const id=$('mc-view-id');if(id){id.max='2147483647';const note=id.parentNode.querySelector('small');if(note)note.textContent='0 = unassigned. Each camera can have its own binding slot.';}
  }

  function dockCamera() { return (S.ownership.state === 'Active' && camIn(S.active.cameraId)) || current(); }
  function stopMouseLook() {
    // Kept for the host's close/visibility cleanup; the native path never captures the page.
    root.dataset.mouseLook = 'false';
    $('mc-mouse-look-area').hidden = true;
    $('mc-mouse-look').setAttribute('aria-pressed', 'false');
    if (window.fscsUpdateMiniRect) window.fscsUpdateMiniRect();
  }
  function startMouseLook() {
    const view=IS_MINI?dockCamera():current();if(!view)return;
    const body=openDialog('Mouse look');
    $('mc-dialog-shade').classList.add('mc-mouselook-shade');
    add(body,make('strong',view.name,'mc-look-camera'),make('p','Hold right-click and drag over the simulator scene.','mc-note'));
    const choices=(title,key,fallback,items)=>{
      const group=make('div',undefined,'mc-look-choices');group.setAttribute('role','group');group.setAttribute('aria-label',title);
      body.appendChild(make('h4',title));body.appendChild(group);
      items.forEach(([value,label])=>{
        const b=button(label,()=>{view[key]=value;saveCamera(view);Array.from(group.children).forEach(e=>e.setAttribute('aria-pressed',String(e===b)));render();},'mc-secondary','check');
        b.disabled=readOnly();b.setAttribute('aria-pressed',String((view[key]||fallback)===value));group.appendChild(b);
      });
    };
    choices('When you release right-click','mouseLookOnRelease','retain',[['retain','Keep adjusted view'],['reset','Return to saved view']]);
    choices('When you return to this camera','temporaryLookPolicy','resetOnRecall',[['resetOnRecall','Start at saved view'],['retain','Remember temporary look']]);
    body.appendChild(make('p','Save or Ctrl+S stores the adjusted view permanently.','mc-note'));
    dialogActions([['Done',()=>closeDialog()]]);compactEntityMenu();
  }
  window.fscsStopMouseLook = stopMouseLook;
  let pointerOverPanel = false;
  function reportPanelPointer(over, force, cancelDrag) {
    const changed = pointerOverPanel !== over;
    pointerOverPanel = over;
    if (S.snapshot && (changed || force || cancelDrag)) bridge.send('PointerOverPanel', { over, cancelDrag: !!cancelDrag });
  }
  window.fscsClearPanelPointer = () => reportPanelPointer(false, true, true);
  function followAutomaticDockCamera() {
    if(!IS_MINI)return null;
    if(!S.airports.autoSwitch||S.ownership.state!=='Active') { S.miniAutoFollowCamera=null;return null; }
    const camera=camIn(S.active.cameraId),group=camera&&categoryOf(camera.id);
    if(!camera||camera.mode!=='airport'||!group||!isLive(camera.id))return null;
    // Follow only a newly active automatic view; leave manual category browsing alone between switches.
    if(S.miniAutoFollowCamera===camera.id)return null;
    S.miniAutoFollowCamera=camera.id;S.miniCategory=group.id;
    return camera.id;
  }
  function revealAutomaticDockCamera(cameraId) {
    nextFrame(()=>{
      if(!S.airports.autoSwitch||S.miniAutoFollowCamera!==cameraId||!isLive(cameraId))return;
      const list=$('mc-dock-favorites'),entry=$$('button',list).find(b=>b.dataset.favorite===cameraId);
      if(!entry)return;
      // Scroll the camera list only. scrollIntoView could also move the simulator panel or steal focus.
      const pane=list.getBoundingClientRect(),item=entry.getBoundingClientRect();
      if(item.top<pane.top)list.scrollTop-=pane.top-item.top;
      else if(item.bottom>pane.bottom)list.scrollTop+=item.bottom-pane.bottom;
      updateScrollbars();
    });
  }
  function renderDock() {
    const automaticCamera=followAutomaticDockCamera();
    renderCameraTools();
    const dock = $('mc-mini-panel');
    keepFocus(dock, () => {
      const shown = dockCamera();
      const group = shown ? categoryOf(shown.id) : null;
      $('mc-dock-current').textContent = shown ? shown.name : '-';
      $('mc-dock-category').textContent = group ? group.name : (S.snapshot ? 'No camera' : 'Camera Studio');
      $('mc-flight-name').textContent = shown ? shown.name : '-';
      const blocked = controlBlockReason();
      ['mc-prev', 'mc-next'].forEach(id => { $(id).disabled = Boolean(blocked); $(id).title = blocked || ''; });
      $('mc-quick-picker').disabled = !P();
      const favorites = empty($('mc-dock-favorites'));
      const categories = empty($('mc-dock-categories'));
      dock.setAttribute('data-picking', String(IS_MINI && S.miniPicking));
      if (IS_MINI && P()) {
        const groups = P().categories;
        const selectedGroup = groups.find(g => g.id === S.miniCategory) || group || groups[0];
        const aircraftGroup=make('div',undefined,'mc-category-grid mc-dock-aircraft');
        aircraftGroup.appendChild(make('span','Aircraft','mc-category-group-label'));categories.appendChild(aircraftGroup);
        let airportGroup=null;
        groups.forEach(g => {
          if(g.airportIcao&&!airportGroup){airportGroup=make('div',undefined,'mc-dock-airport mc-category-grid');airportGroup.appendChild(make('span',g.airportIcao,'mc-category-group-label'));airportGroup.setAttribute('aria-label','Airport '+g.airportIcao);categories.appendChild(airportGroup);}
          const entry = button(g.name, () => { S.miniCategory = g.id; const preferred=camIn(g.defaultCameraId),first=preferred&&preferred.favorite?preferred:g.cameraIds.map(camIn).find(v=>v&&v.favorite); if(first)activateCamera(first.id); renderDock(); });
          decorateName(entry, g.name, true, entityIcon(g, true), g.color);
          wireContext(entry, () => categoryActions(catIn(g.id)));
          entry.setAttribute('aria-pressed', String(selectedGroup && g.id === selectedGroup.id));
          entry.setAttribute('data-focus-key', 'category:' + g.id);
          (g.airportIcao ? airportGroup : aircraftGroup).appendChild(entry);
        });
        const starred=selectedGroup?selectedGroup.cameraIds.map(camIn).filter(view=>view&&view.favorite):[];
        starred.forEach(view => favorites.appendChild(dockViewButton(view, 'category-view:')));
        if(!starred.length){favorites.appendChild(make('span','No starred cameras in this category. Star cameras in the main panel.','mc-note'));['mc-prev','mc-next'].forEach(id=>$(id).disabled=true);}
      } else if (IS_MINI && S.miniPicking && P()) {
        P().categories.forEach(g => {
          favorites.appendChild(make('span', g.name, 'mc-dock-group'));
          g.cameraIds.map(camIn).filter(view=>view&&view.favorite).forEach(view => favorites.appendChild(dockViewButton(view, 'pick:')));
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
    layoutMiniCategories();
    if(automaticCamera)revealAutomaticDockCamera(automaticCamera);
  }
  function layoutMiniCategories() {
    if(!IS_MINI)return;
    const dock=$('mc-mini-panel'),pane=$('mc-dock-categories');
    const groups=$$('.mc-category-grid',pane).filter(g=>!g.classList.contains('mc-category-flat'));
    let flat=pane.querySelector('.mc-category-flat');
    if(!flat){flat=make('div',undefined,'mc-category-grid mc-category-flat');flat.setAttribute('role','group');flat.setAttribute('aria-label','Aircraft and airport categories');pane.appendChild(flat);}
    groups.forEach(g=>{const buttons=$$('button',g);if(buttons.length)g.dataset.hasCategories='true';buttons.forEach(b=>{b.dataset.categoryScope=g.querySelector('.mc-category-group-label').textContent;flat.appendChild(b);});g.hidden=true;});
    const entries=$$('button',flat);
    const width=dock.clientWidth,icons=root.dataset.nameDisplay==='icons',cell=icons?34:width<1100?88:94,narrow=width<1100;
    dock.classList.remove('mc-dock-stacked');dock.classList.toggle('mc-mini-condensed',narrow);
    const available=Math.max(cell,Math.floor((width-20)*(width<880?.5:.40)));
    const columns=Math.max(1,Math.floor(available/cell));
    const usedColumns=Math.min(columns,Math.max(1,entries.length));
    const rows=Math.max(1,Math.ceil(entries.length/usedColumns));
    let legend=pane.querySelector('.mc-dock-category-legend');
    if(!legend){legend=make('span',undefined,'mc-dock-category-legend');pane.insertBefore(legend,pane.firstChild);}
    legend.textContent=groups.filter(g=>g.dataset.hasCategories==='true').map(g=>g.querySelector('.mc-category-group-label').textContent).join(' / ');legend.hidden=narrow;
    const height=Math.max(narrow?58:72,rows*28+Math.max(0,rows-1)*2+(narrow?0:14));
    const categoryWidth=usedColumns*cell+4;
    pane.style.width=categoryWidth+'px';pane.style.height=height+'px';flat.style.gridTemplateColumns='repeat('+usedColumns+',minmax(0,1fr))';
    pane.style.paddingTop=(narrow?0:14)+'px';
    dock.style.gridTemplateColumns=categoryWidth+'px minmax(0,1fr)';
    const tools=dock.querySelector('.mc-dock-tools');tools.style.minHeight='28px';
    const total=height+42;
    dock.style.height=total+'px';dock.style.maxHeight=total+'px';
    sizeMiniClearance();
  }

  function dockViewButton(view, prefix) {
    const entry = button(view.name, () => { S.miniPicking = false; activateCamera(view.id); });
    decorateName(entry, view.name, false, entityIcon(view, false), entityColor(view));
    entry.classList.toggle('mc-airport-camera-name',view.mode==='airport');
    wireContext(entry, () => { selectCamera(view.id); cameraActions(); });
    entry.setAttribute('data-focus-key', prefix + view.id);
    entry.setAttribute('data-favorite', view.id);
    entry.setAttribute('aria-pressed', String(isLive(view.id)));
    const shortcut=cameraShortcutLabel(view);
    if (shortcut) entry.appendChild(make('span', shortcut, 'mc-key'));
    const reason = cameraBlockReason(view);
    entry.disabled = Boolean(reason);
    entry.dataset.cameraHelp = reason ? view.name + ': ' + reason : view.name;
    entry.removeAttribute('title');
    return entry;
  }
  function quickPicker() {
    if (!P()) return;
    if (IS_MINI) { S.miniCategory = (categoryOf((dockCamera() || {}).id) || {}).id; renderDock(); const first = $('mc-dock-favorites').querySelector('button:not([disabled])'); if (first) first.focus(); return; }
    const body = openDialog('Choose a camera');
    P().categories.forEach(group => {
      body.appendChild(make('div', group.name, 'mc-section-label'));
      const choices = make('div', undefined, 'mc-picker-views');
      group.cameraIds.map(camIn).filter(Boolean).forEach(view => {
        const entry = button(view.name, () => { closeDialog(); activateCamera(view.id); });
        entry.setAttribute('aria-pressed', String(view.id === (dockCamera() || {}).id));
        const reason = cameraBlockReason(view);
        entry.disabled = Boolean(reason);
        entry.dataset.cameraName=view.name;entry.dataset.cameraHelp=reason?view.name+': '+reason:view.name;
        choices.appendChild(entry);
      });
      body.appendChild(choices);
    });
    dialogActions([['Close', () => closeDialog()]]);
  }
  function requestMainEditor() {
    stopMouseLook();
    clearTimeout(S.editTimer);
    const shown = dockCamera();
    if (window.fscsSetPanelOpen) window.fscsSetPanelOpen('main', true);
    let attempts = 0;
    const relay = () => {
      bridge.send('RelaySurface', { target: 'main', op: 'expand', cameraId: shown && shown.id, page: 'cameras', tab: 'position' });
      if (++attempts < 12) S.editTimer = setTimeout(relay, 400);
    };
    relay();
  }
  function onSurfaceRequest(p) {
    const me = IS_MINI ? 'mini' : 'main';
    if (p.op === 'mouseLook') { startMouseLook(); return; }
    if (p.target !== me) {
      // The editor surface also hosts an embedded dock in fly mode; let the mini toggle binding reach it.
      if (!IS_MINI && p.target === 'mini' && p.op === 'toggle' && root.dataset.mode === 'fly') { if (root.dataset.miniOpen === 'true') dismissMini(); else revealMini(); }
      return;
    }
    if (IS_MINI && p.op === 'stopMouseLook') { stopMouseLook(); return; }
    if (IS_MINI) { if (p.op === 'show') clearTimeout(S.editTimer); return; }
    if (p.op === 'expand') {
      if (p.cameraId && camIn(p.cameraId)) selectCamera(p.cameraId);
      openEditor('cameras'); setTab('position');
      bridge.send('RelaySurface', { target: 'mini', op: 'show' });
    }
    else if (p.op === 'compact') enterFlightMode();
    else if (p.op === 'show') openEditor();
    else if (p.op === 'toggle') { if (root.dataset.mode === 'edit') enterFlightMode(); else openEditor(); }
  }

  /* ------------- Header / status / save bar ------------- */
  function updateActiveState() {
    const on = engaged();
    root.dataset.enabled = String(S.ownership.state === 'Active');
    const text = statusText();
    $('mc-control-state').textContent = text;
    $('mc-control-state').title = text;
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
    $('mc-save-label').textContent = saving ? 'Saving cameras...' : dirty ? 'Unsaved changes' : 'All changes saved';
    $('mc-revert').disabled = !dirty || saving;
    $('mc-save').disabled = !dirty || saving || !S.snapshot;
    const shortcut = $('mc-save').querySelector('.mc-save-shortcut');
    if (shortcut) {
      const binding = S.working && (S.working.bindings.bindings || []).find(b => b.action === 'saveViews' && b.input && b.input.kind === 'key');
      shortcut.hidden = !binding;
      shortcut.textContent = binding ? binding.input.label || binding.input.definition.replace(/VK_/g,'') : '';
    }

    const exportButton = $('mc-export-profile');
    setLabel(exportButton, 'Export profile');
  }
  function renderBanners() {
    const host = $('mc-banners');
    keepFocus(host, () => {
      empty(host);
      if (!S.snapshot) return;
      if (S.session === 'Ready' && aircraftLoaded() && !S.caps.canActivate && S.caps.blockReason) {
        const actions = [];
        host.appendChild(banner('alert-triangle', [make('strong', 'Camera control unavailable: '), S.caps.blockReason], actions));
      }
      if (readOnly()) host.appendChild(banner('info', ['This profile needs a newer version of Camera Studio. It opens read-only and cannot be activated.'], [], true));
      const a = S.aircraft;
      if (a && a.modelKey && P() && a.matchProfileId !== P().profileId && S.dismissedAircraftKey !== a.modelKey) {
        const dismiss = button('', () => { S.dismissedAircraftKey = a.modelKey; renderBanners(); }, 'mc-icon-button', 'x');
        dismiss.setAttribute('aria-label', 'Dismiss');
        dismiss.setAttribute('data-focus-key', 'banner-x');
        const match = a.matchProfileId ? S.profiles.filter(p => p.profileId === a.matchProfileId)[0] : null;
        if (match) {
          const open = button('Open "' + match.name + '"', () => openProfile(match.profileId), 'mc-secondary');
          open.setAttribute('data-focus-key', 'banner-open');
          host.appendChild(banner('plane', ['"' + match.name + '" is the camera profile for ', make('strong', aircraftName()), '.'], [open, dismiss], true));
        } else if (!a.matchProfileId) {
          const create = button('Create profile for this aircraft', createProfileForAircraft, 'mc-secondary');
          const use = button('Copy for current aircraft', useCurrentForAircraft, 'mc-secondary');
          create.setAttribute('data-focus-key', 'banner-create');
          use.setAttribute('data-focus-key', 'banner-use');
          host.appendChild(banner('plane', [make('strong', 'No camera profile for ' + aircraftName())], [create, use, dismiss]));
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
    $('mc-profile-label').textContent = d ? (d.aircraftMatch && d.aircraftMatch.displayName ? d.aircraftMatch.displayName : 'No aircraft') + ' - ' + d.name : statusText();
    $('mc-profile-picker').title=$('mc-profile-label').textContent;
    setIcon($('mc-profile-picker'), d && d.aircraftMatch && d.aircraftMatch.strategy === 'none' ? 'map-pin' : 'plane');
    renderTree();
    renderDetails();
    renderDock();
    updateActiveState();
    updateSavedState();
    renderBanners();
  }

  /* ------------- Save / revert ------------- */
  // Local work IO can complete before the Pending ack's promise handler runs.
  function waitForCommit(requestId) {
    if (S.completedCommits[requestId]) {
      const done = S.completedCommits[requestId];
      delete S.completedCommits[requestId];
      return Promise.resolve(done);
    }
    return new Promise(resolve => {
      const timer = setTimeout(() => { delete S.commitWaiters[requestId]; resolve({ status: 'Failed', message: 'The save did not answer in time. Your draft is still here.' }); }, 15000);
      S.commitWaiters[requestId] = done => { clearTimeout(timer); resolve(done); };
    });
  }
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
        return waitForCommit(ack.requestId).then(done => {
          S.pendingCommit = null;
          if (done.status === 'Committed') {
            announce('Changes saved for ' + P().name + '.');
            if(window.FSCSSaveFeedback)window.FSCSSaveFeedback.play(ack.requestId);
            bridge.send('RequestSnapshot', {});
            updateSavedState();
            return true;
          }
          announce(done.message || 'Your changes could not be saved. They are still here - try again.', 'error');
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

  /* ------------- Bindings ------------- */
  const bindingsList = () => (S.working ? S.working.bindings.bindings : []);
  const bindingsFor = (action, viewId) => bindingsList().filter(b => b.action === action && (b.viewId || 0) === (viewId || 0));
  const findBinding = (action, viewId, slot) => {
    const list = bindingsFor(action, viewId);
    if (!slot) return list.filter(b => b.input.kind === 'key')[0] || list[0] || null;
    const explicit = list.filter(b => b.slot === slot)[0];
    if (explicit) return explicit;
    const free = ['primary', 'secondary'].filter(key => !list.some(b => b.slot === key));
    return list.filter(b => !b.slot)[free.indexOf(slot)] || null;
  };
  function preserveBindingSlots() {
    bindingsList().forEach(b => {
      if (b.slot) return;
      const primary = findBinding(b.action, b.viewId, 'primary'), secondary = findBinding(b.action, b.viewId, 'secondary');
      if (primary) primary.slot = 'primary';
      if (secondary) secondary.slot = 'secondary';
    });
  }
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
      ['Peek - hold to look, release to return', unique(used.concat(bound('peekView'))).map(n => ({ action: 'peekView', viewId: n, peek: true, name: 'Peek at ' + (camFor(n) ? camFor(n).name : 'view ID ' + n), detail: 'View ID ' + n + ' - hold to look - release to return' }))],
      ['Camera commands', COMMANDS.map(c => ({ action: c[0], viewId: 0, name: c[1], detail: c[2] }))],
    ];
  }
  function updateBindingToggle() {
    const groups=$$('.mc-binding-section',$('mc-bindings-list'));
    const allOpen=groups.length>0&&groups.every(g=>g.open);
    $('mc-toggle-bindings').textContent=allOpen?'Collapse all':'Expand all';
    $('mc-toggle-bindings').setAttribute('aria-expanded',String(allOpen));
    $('mc-toggle-bindings').disabled=!groups.length;
  }
  function renderBindings() {
    const list = $('mc-bindings-list');
    if (!$('mc-binding-kind').querySelector('[value="simZoom"]')) {
      const option = make('option', 'Mouse wheel / MSFS zoom'); option.value = 'simZoom'; $('mc-binding-kind').appendChild(option);
    }
    const query = $('mc-binding-search').value.trim().toLowerCase();
    const status = $('mc-binding-status').value, kind = $('mc-binding-kind').value;
    const filtering = !!query || status !== 'all' || kind !== 'all';
    if (!S.bindingExpanded) S.bindingExpanded = [];
    const sections = bindingSections().concat([['Manual look & move', MANUAL.map(entry => ({ action: entry[0], viewId: 0, name: entry[1], detail: entry[0] === 'moveFast' ? 'Hold for 4x movement speed' : entry[0].indexOf('zoom') === 0 ? 'Field of view' : entry[0].indexOf('look') === 0 ? 'Temporary look' : 'Fine 0.1x / Normal 1x / Coarse 10x' }))]]);

    let shown = 0, total = 0;
    keepFocus(list, () => {
      empty(list);
      sections.forEach((section, index) => {
        total += section[1].length;
        const rows = section[1].filter(spec => {
          const matches = bindingsFor(spec.action, spec.viewId), binding = matches[0];
          if (status === 'assigned' && !binding || status === 'unassigned' && binding) return false;
          if (kind !== 'all' && !matches.some(b => b.input.kind === kind)) return false;
          return !query || [section[0],spec.name,spec.detail,matches.map(b => (b.input.label || '') + ' ' + b.input.definition).join(' ')].join(' ').toLowerCase().indexOf(query) >= 0;
        });
        shown += rows.length;
        if (!rows.length) return;
        const details = make('details', undefined, 'mc-binding-section');
        details.open = filtering || S.bindingExpanded[index];
        details.addEventListener('toggle', () => {
          if (!details.isConnected) return;
          if (!filtering) S.bindingExpanded[index] = details.open;
          updateBindingToggle();
        });
        details.appendChild(make('summary', section[0] + ' (' + rows.length + ')'));
        rows.forEach(row => details.appendChild(bindingRow(row)));
        list.appendChild(details);
      });
      if (!shown) list.appendChild(make('p', 'No bindings match these filters.', 'mc-empty'));
    });
    updateBindingToggle();
    $('mc-binding-count').textContent = shown + ' of ' + total + ' bindings';
  }
  function bindingRow(spec) {
    const key = spec.action + (spec.viewId ? ':' + spec.viewId : '');
    const row = make('div', undefined, 'mc-binding-row mc-binding-dual');
    row.setAttribute('data-binding', key);
    const name = make('div', spec.name, 'mc-binding-name');
    name.appendChild(make('small', spec.detail)); row.appendChild(name);
    ['primary', 'secondary'].forEach(slot => {
      const binding = findBinding(spec.action, spec.viewId, slot), title = slot === 'primary' ? 'Primary' : 'Secondary';
      const group = make('div', undefined, 'mc-binding-slot'); group.setAttribute('data-slot', slot);
      group.appendChild(make('small', title));
      const assign = button(binding ? binding.input.label || binding.input.definition : 'Assign input', () => assignDialog(Object.assign({}, spec, {slot})), 'mc-binding-input', binding && binding.input.kind === 'key' ? 'keyboard' : 'radio');
      assign.setAttribute('data-focus-key', 'assign:' + key + ':' + slot);
      assign.setAttribute('aria-label', 'Assign ' + slot + ' input to ' + spec.name); assign.disabled = !S.snapshot;
      assign.setAttribute('data-empty', String(!binding)); group.appendChild(assign);
      const actions = make('div', undefined, 'mc-binding-actions');
      const clear = button('Clear', () => {
        preserveBindingSlots(); S.working.bindings.bindings = bindingsList().filter(b => b !== binding);
        saveBindings(); renderBindings(); renderDetails(); renderTree(); renderDock();
        setBindingResult(title + ' input cleared for "' + spec.name + '". Save changes to keep it.');
      });
      clear.setAttribute('aria-label', 'Clear ' + slot + ' input for ' + spec.name); clear.disabled = !S.snapshot || !binding;
      const test = button(spec.peek ? 'Hold' : 'Test', spec.peek ? null : () => testBinding(binding, spec.name));
      test.disabled = !S.snapshot || !binding; test.setAttribute('aria-label', 'Test ' + slot + ' input for ' + spec.name);
      if(spec.peek) wirePeek(test, spec.viewId);
      add(actions, clear, test); group.appendChild(actions); row.appendChild(group);
    });
    return row;
  }
  function testBinding(binding, name) {
    if (!binding) return;
    whenIdle().then(() => bridge.request('TestBinding', { binding: clone(binding) })).then(ack => {
      if (ack.status === 'Rejected' || ack.status === 'TimedOut') { setBindingResult(reasonText(ack.reason), 'warn'); return; }
      const r = ack.result || {};
      setBindingResult(r.message || (r.recalled ? '"' + name + '" sent - the view was recalled.' : '"' + name + '" sent to the simulator.'), r.dispatched === false ? 'warn' : '');
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

  /* Key capture > MSFS input definition (SimConnect_MapInputEventToClientEvent_EX1 format) */
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
  function keyCapture(event, held, allowShift) {
    event = Object.assign({}, { ctrlKey: event.ctrlKey, altKey: event.altKey, shiftKey: event.shiftKey, code: captureCode(event) });
    if (allowShift && (event.code === 'ShiftLeft' || event.code === 'ShiftRight') && !event.ctrlKey && !event.altKey) {
      return { input: { kind: 'key', definition: MODIFIERS[event.code][0], label: MODIFIERS[event.code][1] } };
    }
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
  // Coherent versions can omit KeyboardEvent.code. Prefer physical codes, then legacy VK/location.
  function captureCode(event) {
    if (MODIFIERS[event.code] || KEY_TABLE[event.code]) return event.code;
    const n = event.keyCode || event.which || 0;
    if (n >= 65 && n <= 90) return 'Key' + String.fromCharCode(n);
    if (n >= 48 && n <= 57) return 'Digit' + (n - 48);
    if (n >= 96 && n <= 105) return 'Numpad' + (n - 96);
    if (n >= 112 && n <= 135) return 'F' + (n - 111);
    const codes = { 8:'Backspace',9:'Tab',13:'Enter',16:'Shift',17:'Control',18:'Alt',32:'Space',33:'PageUp',34:'PageDown',35:'End',36:'Home',37:'ArrowLeft',38:'ArrowUp',39:'ArrowRight',40:'ArrowDown',45:'Insert',46:'Delete',106:'NumpadMultiply',107:'NumpadAdd',109:'NumpadSubtract',110:'NumpadDecimal',111:'NumpadDivide',186:'Semicolon',187:'Equal',188:'Comma',189:'Minus',190:'Period',191:'Slash',192:'Backquote',219:'BracketLeft',220:'Backslash',221:'BracketRight',222:'Quote' };
    let key = codes[n] || ({ Ctrl:'Control', Left:'ArrowLeft', Right:'ArrowRight', Up:'ArrowUp', Down:'ArrowDown', Spacebar:'Space', ' ':'Space' })[event.key] || event.key;
    if (key === 'Control' || key === 'Alt' || key === 'Shift') return key + (event.location === 2 ? 'Right' : 'Left');
    if (/^[a-z]$/i.test(key || '')) return 'Key' + key.toUpperCase();
    if (/^[0-9]$/.test(key || '')) return (event.location === 3 ? 'Numpad' : 'Digit') + key;
    return key;
  }
  function assignCameraBinding(view) {
    if(!view||readOnly())return;
    if(!view.viewId){
      const used=cameras().map(c=>c.viewId).concat(bindingsList().map(b=>b.viewId));
      let id=1;while(used.indexOf(id)>=0)id++;
      view.viewId=id;saveCamera(view);renderDetails();renderTree();
    }
    assignDialog({action:'selectView',viewId:view.viewId,name:view.name,detail:'View ID '+view.viewId});
  }
  function fitAssignmentDialog() {
    const shade=$('mc-dialog-shade');
    if(shade.hidden||!shade.classList.contains('mc-binding-shade'))return;
    const menu=shade.querySelector('.mc-dialog'),body=$('mc-dialog-body');
    const available=Math.max(160,window.innerHeight-16);
    menu.style.maxHeight=available+'px';
    const fixed=menu.querySelector('header').getBoundingClientRect().height+$('mc-dialog-error').getBoundingClientRect().height+$('mc-dialog-actions').getBoundingClientRect().height+4;
    body.style.maxHeight=Math.max(40,available-fixed)+'px';
    const dock=$('mc-mini-panel').getBoundingClientRect();
    menu.style.left=Math.max(8,Math.min(innerWidth-menu.offsetWidth-8,IS_MINI?dock.right-menu.offsetWidth:(innerWidth-menu.offsetWidth)/2))+'px';
    menu.style.top=Math.max(8,Math.min(innerHeight-menu.offsetHeight-8,IS_MINI?dock.top-menu.offsetHeight-8:(innerHeight-menu.offsetHeight)/2))+'px';
    updateScrollbars();if(window.fscsUpdateMiniRect)window.fscsUpdateMiniRect();
  }
  window.addEventListener('resize',()=>nextFrame(fitAssignmentDialog));
  function assignDialog(spec) {
    spec = Object.assign({slot:'primary'}, spec);
    const existing = findBinding(spec.action, spec.viewId, spec.slot);
    // Closing the dialog always hands the keyboard back to the simulator.
    const body = openDialog('Assign ' + spec.name + ' - ' + (spec.slot === 'secondary' ? 'Secondary' : 'Primary'), () => { bridge.send('StopControllerCapture', {}); S.assign = null; claimKeyboard(null); setTimeout(reportTextFocus, 0); });
    S.assign = { spec, staged: null, held: {}, controllers: null, tested: { down: false, up: false } };
    $('mc-dialog-shade').classList.add('mc-binding-shade');
    const slots = make('div', undefined, 'mc-assign-slots');
    ['primary','secondary'].forEach(slot => {
      const binding = findBinding(spec.action, spec.viewId, slot), title = slot === 'primary' ? 'Primary' : 'Secondary';
      const choose = button(title, () => assignDialog(Object.assign({}, spec, {slot})), 'mc-secondary');
      choose.setAttribute('data-slot',slot); choose.setAttribute('aria-pressed',String(slot === spec.slot));
      choose.appendChild(make('small',binding ? binding.input.label || binding.input.definition : 'Unassigned')); slots.appendChild(choose);
    }); body.appendChild(slots);
    body.appendChild(make('p', 'Each input works independently. Press a key combination now. Esc cancels. Up to two modifier keys (Ctrl, Alt, Shift) plus one key.'));
    // A (read-only) text input, so the panel claims the keyboard from the sim while capturing.
    const capture = make('input', undefined, 'mc-binding-input');
    capture.type = 'text';
    capture.readOnly = true;
    capture.value = existing ? existing.input.label : 'Waiting for a key...';
    capture.id = 'mc-assign-capture';
    capture.setAttribute('aria-label', 'Key capture. Press a key combination');
    capture.setAttribute('data-capturing', 'true');
    body.appendChild(capture);
    const note = make('p', '', 'mc-warn');
    note.id = 'mc-assign-note';
    note.hidden = true;
    body.appendChild(note);
    if (spec.action === 'moveFast') body.appendChild(make('p', 'Press Shift on its own, or choose another key to hold for 4x faster movement.', 'mc-note'));
    if (spec.action === 'zoomIn' || spec.action === 'zoomOut') {
      const wheelNote = 'Uses MSFS zoom commands. Keep the wheel assigned to zoom in MSFS Controls. Scroll over this capture field to choose a direction, or use the buttons below.';
      body.appendChild(make('p', wheelNote, 'mc-note'));
      const chooseWheel = up => stageInput({kind:'simZoom', definition:up?'sim:ZOOM_IN':'sim:ZOOM_OUT', label:up?'Mouse wheel up / MSFS zoom in':'Mouse wheel down / MSFS zoom out'}, null);
      const wheelActions = make('div', undefined, 'mc-dialog-actions');
      add(wheelActions, button('Wheel up', () => chooseWheel(true), 'mc-secondary'), button('Wheel down', () => chooseWheel(false), 'mc-secondary'));
      body.appendChild(wheelActions);
      capture.addEventListener('wheel', event => { if (!event.deltaY) return; event.preventDefault(); event.stopPropagation(); chooseWheel(event.deltaY < 0); }, {passive:false});
    }
    body.appendChild(make('div', 'Or use a controller', 'mc-section-label'));
    const controllerHost = make('div');
    controllerHost.id = 'mc-assign-controller';
    controllerHost.appendChild(make('p', 'Looking for controllers...'));
    body.appendChild(controllerHost);
    capture.addEventListener('keydown', event => {
      if (event.key === 'Escape' || event.key === 'Tab') return;
      event.preventDefault();
      event.stopPropagation();
      const result = keyCapture(event, S.assign.held, spec.action === 'moveFast');
      if (result.pending) { capture.value = Object.keys(S.assign.held).filter(c => S.assign.held[c]).map(c => MODIFIERS[c][1]).join(' + ') + ' + ...'; return; }
      if (result.error) { dialogError(result.error); S.assign.held = {}; return; }
      stageInput(result.input, result.note);
      S.assign.held = {};
    });
    capture.addEventListener('keyup', event => { const code = captureCode(event); if (S.assign && MODIFIERS[code]) S.assign.held[code] = false; });
    const actions = [['Cancel', () => closeDialog()]];
    if (existing) actions.push(['Clear assignment', () => {
      preserveBindingSlots();
      S.working.bindings.bindings = bindingsList().filter(b => b !== existing);
      closeDialog(); saveBindings(); renderBindings(); renderDetails(); renderTree(); renderDock();
      announce('Assignment cleared. Save changes to keep it.');
    }]);
    actions.push(['Assign', applyAssign, 'mc-primary']);
    dialogActions(actions);
    updateAssignButton();
    bridge.send('ListControllers', {});
    if(IS_MINI){
      $('mc-dialog-shade').classList.add('mc-tracking-shade');
      nextFrame(()=>{const menu=$('mc-dialog-shade').querySelector('.mc-dialog'),dock=$('mc-mini-panel').getBoundingClientRect();menu.style.left=Math.max(8,Math.min(innerWidth-menu.offsetWidth-8,dock.right-menu.offsetWidth))+'px';menu.style.top=Math.max(8,dock.top-menu.offsetHeight-8)+'px';if(window.fscsUpdateMiniRect)window.fscsUpdateMiniRect();});
    }
    nextFrame(() => { capture.focus(); fitAssignmentDialog(); });
  }
  function stageInput(input, note) {
    const a = S.assign;
    if (!a) return;
    if (a.listening) { a.listening = false; bridge.send('StopControllerCapture', {}); }
    a.staged = input;
    $('mc-assign-capture').value = input.label;
    const noteEl = $('mc-assign-note');
    noteEl.hidden = !note;
    noteEl.textContent = note || '';
    const conflict = bindingsList().filter(b => sameInput(b.input, input) && b !== findBinding(a.spec.action, a.spec.viewId, a.spec.slot))[0];
    const conflictCamera = conflict && cameras().filter(c => c.viewId === conflict.viewId)[0];
    dialogError(conflict ? input.label + ' is assigned to "' + (conflictCamera ? conflictCamera.name : actionName(conflict.action, conflict.viewId)) + '". Replace it to move this input to "' + a.spec.name + '".' : '');
    a.conflict = Boolean(conflict);
    updateAssignButton();
  }
  function updateAssignButton() {
    const a = S.assign;
    const buttons = $('mc-dialog-actions').querySelectorAll('button');
    const assignButton = buttons[buttons.length - 1];
    if (!assignButton || !a) return;
    const controllerReady = !a.staged || a.staged.kind !== 'joystick' || (a.tested.down && a.tested.up);
    assignButton.textContent = a.conflict ? 'Replace existing assignment' : 'Assign';
    assignButton.disabled = !a.staged || !controllerReady;
    nextFrame(fitAssignmentDialog);
  }
  function renderAssignControllers() {
    const a = S.assign, host = $('mc-assign-controller');
    if (!a || !host) return;
    empty(host);
    const controllers = (a.controllers || []).filter(c => !/^(mouse|keyboard|touch pointer)$/i.test(c.name.trim()));
    if (!controllers.length) { host.appendChild(make('p', 'No controllers found. Connect one, then refresh.')); host.appendChild(button('Refresh controllers', () => bridge.send('ListControllers', {}), 'mc-secondary')); nextFrame(fitAssignmentDialog); return; }
    // Coherent cannot reliably open HTML select popups. Use ordinary in-panel
    // buttons so controller options participate in the native panel hit region.
    if(!controllers.some(c => c.deviceId === a.selectedDeviceId))a.selectedDeviceId=controllers[0].deviceId;
    const currentDevice = () => controllers.filter(c => c.deviceId === a.selectedDeviceId)[0];
    const deviceLabel=make('div',undefined,'mc-field mc-controller-picker');
    deviceLabel.setAttribute('data-no-tooltip','true');
    const caption=make('span','Controller');caption.id='mc-controller-caption';deviceLabel.appendChild(caption);
    const choices=make('div',undefined,'mc-controller-choices');choices.id='mc-controller-choices';choices.hidden=true;
    choices.setAttribute('role','group');choices.setAttribute('aria-label','Available controllers');
    const setOpen=open=>{choices.hidden=!open;device.setAttribute('aria-expanded',String(open));nextFrame(fitAssignmentDialog);};
    const device=button(currentDevice().name,()=>setOpen(choices.hidden),'mc-secondary');device.id='mc-assign-device';
    device.setAttribute('aria-label','Choose controller: '+currentDevice().name);device.setAttribute('aria-controls',choices.id);device.setAttribute('aria-expanded','false');
    controllers.forEach(c=>{
      const choice=button(c.name,()=>{
        a.selectedDeviceId=c.deviceId;a.listening=false;a.captureId=null;bridge.send('StopControllerCapture',{});
        if(a.staged && a.staged.kind==='joystick'){a.staged=null;a.tested={down:false,up:false};$('mc-assign-capture').value='Waiting for controller input...';dialogError('');updateAssignButton();}
        device.textContent=c.name;device.setAttribute('aria-label','Choose controller: '+c.name);
        Array.from(choices.children).forEach(b=>b.setAttribute('aria-pressed',String(b.dataset.deviceId===String(c.deviceId))));
        listen.textContent='Listen for button or hat';status.textContent='Ready to listen on '+c.name+'.';setOpen(false);device.focus();
      },'mc-secondary');
      choice.dataset.deviceId=String(c.deviceId);choice.setAttribute('aria-pressed',String(c.deviceId===a.selectedDeviceId));choices.appendChild(choice);
    });
    deviceLabel.addEventListener('keydown',event=>{if(event.key==='Escape'&&!choices.hidden){event.preventDefault();event.stopPropagation();setOpen(false);device.focus();}});
    add(deviceLabel,device,choices);
    const status = make('p', 'Select a controller, then listen for a button or hat direction.', 'mc-note');
    status.id = 'mc-assign-test'; status.setAttribute('aria-live', 'polite');
    const listen = button('Listen for button or hat', () => {
      if (a.listening) { a.listening = false; bridge.send('StopControllerCapture', {}); listen.textContent = 'Listen for button or hat'; status.textContent = 'Listening stopped.'; return; }
      const c = currentDevice(); if(!c)return;
      a.listening = true; a.captureDeviceId = c.deviceId; a.captureId = M.uuid(); const captureId = a.captureId; a.staged = null; a.tested = {down:false,up:false};
      updateAssignButton(); listen.textContent = 'Stop listening'; status.textContent = 'Press and release a button, or move a hat and let it return to center on ' + c.name + '.';
      bridge.request('StartControllerCapture', {deviceId:c.deviceId,captureId}).then(ack => {
        if(S.assign !== a || a.captureId !== captureId)return;
        if(ack.status === 'Rejected' || ack.status === 'TimedOut') { a.listening = false; listen.textContent = 'Listen for button or hat'; status.textContent = reasonText(ack.reason) || 'Could not listen. Try the manual input option below.'; }
      });
    }, 'mc-primary', 'radio'); listen.id = 'mc-assign-listen';
    const manual = make('details', undefined, 'mc-controller-manual'); manual.appendChild(make('summary', 'Manual button or hat switch'));
    manual.appendChild(make('p', 'Choose an input manually if listening cannot detect it. Test the control before assigning it.', 'mc-note'));
    const grid = make('div', undefined, 'mc-form-grid mc-flush');
    const kindLabel = make('label', 'Input', 'mc-field'), kind = make('select');
    [['button', 'Button'], ['pov', 'Hat switch']].forEach(k => { const o = make('option', k[1]); o.value = k[0]; kind.appendChild(o); }); kindLabel.appendChild(kind);
    const numberField = field('Button number', '1', 'number'); numberField.input.min = '1'; numberField.input.max = '128';
    const dirLabel = make('label', 'Direction', 'mc-field'), dir = make('select');
    ['up', 'right', 'down', 'left'].forEach(d => { const o = make('option', d.charAt(0).toUpperCase() + d.slice(1)); o.value = d; dir.appendChild(o); }); dirLabel.appendChild(dir);
    add(grid, kindLabel, numberField.label, dirLabel);
    const testButton = button('Test input', () => {
      const c = currentDevice(); if(!c)return;
      let input;
      if (kind.value === 'button') {
        const n = Math.max(1, Math.min(128, Math.round(Number(numberField.input.value) || 1)));
        input = {kind:'joystick',deviceId:c.deviceId,deviceName:c.name,definition:'joystick:' + c.deviceId + ':button:' + (n - 1),label:c.name + ' - Button ' + n};
      } else input = {kind:'joystick',deviceId:c.deviceId,deviceName:c.name,definition:'joystick:' + c.deviceId + ':POV:0',pov:dir.value,label:c.name + ' - Hat ' + dir.value};
      a.tested = {down:false,up:false}; stageInput(input,null);
      status.textContent = 'Press and release ' + input.label + '.'; bridge.send('TestInput',{input:clone(input)});
    }, 'mc-secondary');
    const sync = () => { numberField.label.hidden = kind.value !== 'button'; dirLabel.hidden = kind.value === 'button'; nextFrame(fitAssignmentDialog); };
    kind.addEventListener('change',sync); manual.addEventListener('toggle',()=>nextFrame(fitAssignmentDialog)); sync();
    add(manual,grid,testButton); add(host,deviceLabel,listen,status,manual); nextFrame(fitAssignmentDialog);
  }
  function onControllerInputCaptured(p) {
    const a = S.assign;
    if(!a || !a.listening || !p.input || p.input.deviceId !== a.captureDeviceId || p.captureId !== a.captureId)return;
    a.listening = false; a.tested = {down:true,up:true}; stageInput(p.input,null);
    const status=$('mc-assign-test'),listen=$('mc-assign-listen');
    if(status)status.textContent='Detected ' + p.input.label + '. Choose Assign to use it.';
    if(listen)listen.textContent='Listen for another input';
  }
  function onControllerCaptureStatus(p) {
    const a=S.assign; if(!a || !a.listening || p.captureId !== a.captureId || p.state === 'listening')return;
    a.listening=false;
    const status=$('mc-assign-test'),listen=$('mc-assign-listen');
    if(listen)listen.textContent='Listen for button or hat';
    if(status)status.textContent=p.state === 'error' ? 'MSFS could not register controller listening. Try again or test an input manually. Details are available in Diagnostics.' : p.state === 'disconnected' ? 'Controller disconnected. Reconnect it and refresh controllers.' : p.state === 'timeout' ? 'No button or hat detected. Listen again or use the manual input option.' : 'Listening stopped.';
  }
  function onInputTest(p) {
    const a = S.assign;
    if (!a || !a.staged || a.staged.definition !== p.definition) return;
    a.tested[p.phase === 'down' ? 'down' : 'up'] = true;
    const status = $('mc-assign-test');
    if (status) status.textContent = p.phase === 'down' ? 'Pressed...' : (a.tested.down ? 'Pressed and released - this input works.' : 'Released.');
    updateAssignButton();
  }
  function applyAssign() {
    const a = S.assign;
    if (!a || !a.staged || readOnly() || (a.staged.kind === 'joystick' && !(a.tested.down && a.tested.up))) return;
    // Replace only this slot and the exact conflicting input; retain all other inputs.
    preserveBindingSlots();
    const existing = findBinding(a.spec.action, a.spec.viewId, a.spec.slot);
    const list = bindingsList().filter(b => !sameInput(b.input, a.staged) && b !== existing);
    const next = { action: a.spec.action, slot: a.spec.slot };
    if (a.spec.viewId) next.viewId = a.spec.viewId;
    next.input = clone(a.staged);
    list.push(next);
    S.working.bindings.bindings = list;
    const label = a.staged.label;
    closeDialog();
    saveBindings();
    renderBindings();
    renderDetails();
    renderTree(); renderDock();
    setBindingResult('Assigned ' + label + ' to "' + a.spec.name + '". Save changes to keep it.');
  }

  /* ------------- Profiles ------------- */

  function renderAirportSummary() {
    const box=$('mc-airports-summary');if(!box)return;empty(box);
    const state=S.airports||{},heading=make('div',undefined,'mc-library-heading'),title=make('h4','Shared airport library');
    add(heading,icon('view-tower'),title,make('span','All aircraft','mc-pill'));box.appendChild(heading);
    box.appendChild(make('p','Airport cameras are saved separately and available with every aircraft.','mc-note'));
    const loaded=make('p',state.loaded?'Loaded airport: '+state.loaded:'No saved airport loaded nearby.','mc-library-status');
    add(box,loaded,button('Open airport library',airportDialog,'mc-secondary','map-pin'));
    if(state.loaded)box.appendChild(make('small','Switching aircraft profiles keeps these airport cameras.','mc-note'));
  }
  function airportDialog() {
    const body=openDialog('Airport cameras');
    $('mc-dialog-shade').classList.add('mc-airport-shade');
    add(body,make('p','Choose an airport, preview its cameras, then save the views you want. Saved airport cameras are shared by every aircraft and are kept separately from aircraft profiles.','mc-note'));
    const nearby=make('div');nearby.id='mc-airport-nearby';body.appendChild(nearby);
    const codes=make('input');codes.type='text';codes.id='mc-airport-codes';codes.placeholder='ICAO codes, e.g. KSEA KBFI';codes.setAttribute('aria-label','Airport codes');
    const options=make('div',undefined,'mc-airport-options');
    [['runways','Runways','Thresholds and runway views'],['parking','Gates & apron','Parking stands and apron views'],['taxi','Taxi signs / intersections','Offset intersections when signs are unavailable'],['tower','Tower','An overview from the control tower']].forEach(([key,label,hint])=>{
      const check=button('',()=>{check.checked=!check.checked;check.setAttribute('aria-checked',String(check.checked));},'mc-airport-choice');
      check.id='mc-airport-'+key;check.checked=true;check.setAttribute('role','checkbox');check.setAttribute('aria-checked','true');check.setAttribute('aria-label',label);
      const mark=make('span',undefined,'mc-airport-check');mark.setAttribute('aria-hidden','true');mark.appendChild(icon('check'));const copy=make('span',undefined,'mc-airport-choice-copy'),heading=make('strong');add(heading,icon({runways:'view-runway',parking:'view-gate',taxi:'view-taxiway',tower:'view-tower'}[key]),make('span',label));add(copy,heading,make('small',hint));add(check,mark,copy);options.appendChild(check);
    });
    const placement=make('div',undefined,'mc-airport-placement');
    const globalHeightRow=make('label',undefined,'mc-switch-row'),globalHeight=make('input');globalHeight.type='checkbox';globalHeight.checked=true;globalHeight.id='mc-airport-global-heights';globalHeight.setAttribute('aria-label','Use global category heights');add(globalHeightRow,make('span','Use global category heights'),globalHeight);body.appendChild(globalHeightRow);
    [['height','Height above feature',3,1,100],['offset','Horizontal offset',12,3,150]].forEach(([key,label,value,min,max])=>{const row=make('label',undefined,'mc-airport-measure'),n=make('input'),valueBox=make('span',undefined,'mc-airport-value');Object.assign(n,{type:'number',value,min,max,step:1,id:'mc-airport-'+key});n.setAttribute('aria-label',label);add(valueBox,n,make('span','m'));add(row,make('span',label),valueBox);placement.appendChild(row);});
    const generate=button('Preview cameras',()=>{
      const codes=$('mc-airport-codes').value.toUpperCase().split(/[\s,;]+/).filter(Boolean);
      if(!codes.length){dialogError('Enter an airport code or select one from the nearby list.');return;}
      if(codes.length>10){dialogError('Generate up to ten airports at a time.');return;}
      if(!validAirportOptions())return;
      dialogError('');
      S.airportPreviews={};codes.forEach(code=>S.airportPreviews[code]={icao:code,pending:true});renderAirportPreview();
      command('PreviewAirports',{codes:Array.from(new Set(codes)),options:airportOptions()}).then(ack=>{if(ack.status==='Rejected')dialogError(reasonText(ack.reason));});
    },'mc-primary');generate.id='mc-airport-preview';
    const codeLabel=make('label','Airport ICAO codes','mc-airport-code-label');codeLabel.htmlFor=codes.id;
    add(body,codeLabel,codes,make('small','Enter one or more codes, separated by spaces.','mc-note'),make('h4','2. Camera types','mc-airport-heading'),options,make('h4','3. Camera placement','mc-airport-heading'),placement,generate);
    placement.before(globalHeightRow);$('mc-airport-height').disabled=true;globalHeight.addEventListener('change',()=>{$('mc-airport-height').disabled=globalHeight.checked;});
    const source=make('details'),sourceTitle=make('summary','Use airport XML source (includes taxi signs)');const xml=make('textarea');xml.id='mc-airport-xml';xml.rows=5;xml.placeholder='Paste an Airport or FSData XML document';xml.setAttribute('aria-label','Airport XML source');
    add(source,sourceTitle,make('p','For source XML you can access. Compiled/encrypted BGL files cannot be pasted here.','mc-note'),xml,button('Preview from XML',()=>{
      try{if(!validAirportOptions())return;const facility=parseAirportXml(xml.value);command('PreviewAirportSource',{facility,options:airportOptions()});}catch(err){dialogError(err.message);}
    },'mc-secondary'));body.appendChild(source);
    const results=make('div',undefined,'mc-airport-results');results.id='mc-airport-results';body.appendChild(results);
    dialogActions([['Close',()=>closeDialog()]]);renderAirportNearby();renderAirportPreview();
    command('DiscoverAirports',{}, {quiet:true});
  }
  function airportOptions() {const o={};['runways','parking','taxi','tower'].forEach(k=>o[k]=$('mc-airport-'+k).checked);['height','offset'].forEach(k=>o[k]=Number($('mc-airport-'+k).value));if($('mc-airport-global-heights').checked){delete o.height;o.categoryHeights=Object.assign({runways:3,gates:3,apron:3,taxi:3,tower:35},S.working.settings.host.airportCategoryHeights||{});}return o;}
  function validAirportOptions(){
    const o=airportOptions();
    if(!['runways','parking','taxi','tower'].some(k=>o[k])){dialogError('Choose at least one camera type.');return false;}
    if((o.height!==undefined&&(!isFinite(o.height)||o.height<1||o.height>100))||!isFinite(o.offset)||o.offset<3||o.offset>150){dialogError('Use a height of 1–100 m and an offset of 3–150 m.');return false;}
    return true;
  }
  function renderAirportNearby(){
    const box=$('mc-airport-nearby');if(!box)return;empty(box);
    const saved=make('details',undefined,'mc-airport-saved');add(saved,make('summary','Saved airports · '+(S.airports.loaded||'Automatic nearest')),make('p','Only one airport loads alongside your aircraft cameras. Automatic selection uses a '+(S.airports.radiusNm||25)+' NM radius and switches when another saved airport is at least 2 NM closer.','mc-note'),button('Automatic nearest',()=>command('SelectAirport',{icao:''}),'mc-secondary'));box.appendChild(saved);
    (S.airports.nearby||[]).forEach(a=>{const b=button(a.icao+' · '+a.name+' · '+Number(a.distanceNm).toFixed(1)+' NM',()=>command('SelectAirport',{icao:a.icao}),'mc-secondary');b.setAttribute('aria-pressed',String(a.icao===S.airports.loaded));saved.appendChild(b);});
    const current=(S.airports.facilities||[]).slice(0,8);
    box.appendChild(make('h4','1. Select an airport','mc-airport-heading'));
    if(current.length){const chips=make('div',undefined,'mc-airport-chips');current.forEach((a,i)=>chips.appendChild(button((i===0?'Nearest: ':'')+a.icao,()=>{$('mc-airport-codes').value=a.icao;Array.from(chips.children).forEach(b=>b.setAttribute('aria-pressed',String(b.textContent.indexOf(a.icao)>=0)));},'mc-secondary')));box.appendChild(chips);}
    else box.appendChild(make('p','No nearby scenery airports found yet. Enter an ICAO code below.','mc-note'));
  }
  function renderAirportPreview(){
    const box=$('mc-airport-results');if(!box)return;empty(box);
    Object.keys(S.airportPreviews).forEach(code=>{const result=S.airportPreviews[code],section=make('section');add(section,make('h4',code));box.appendChild(section);
      if(result.pending){section.appendChild(make('p','Reading airport scenery…'));return;}
      if(result.error)section.appendChild(make('p',result.error,'mc-warn'));
      (result.notes||[]).forEach(n=>section.appendChild(make('p',n,'mc-note')));
      const profile=result.profile;if(!profile)return;
      section.appendChild(make('p','World-fixed camera control is a preview feature pending in-sim qualification. Feature elevations use scenery data; check terrain clearance.','mc-note'));
      profile.categories.forEach(cat=>{const group=make('details');group.open=false;group.appendChild(make('summary',cat.name+' ('+cat.cameraIds.length+')'));
        cat.cameraIds.forEach(id=>{const cam=profile.cameras.find(c=>c.id===id),row=make('label'),check=make('input');check.type='checkbox';check.checked=true;check.dataset.airportCamera=id;add(row,check,make('span',cam.name));group.appendChild(row);});section.appendChild(group);});
      const replace=make('input');replace.type='checkbox';const label=make('label');add(label,replace,make('span','Replace matching generated views (discard their refinements)'));section.appendChild(label);
      let confirmation=null;
      const performSave=async(ids,discardAirportEdits)=>{
        save.disabled=true;save.textContent='Saving...';
        const ack=await command('SaveAirportPreview',{icao:code,cameraIds:ids,replace:discardAirportEdits || replace.checked,discardAirportEdits});
        const done=ack.status==='Pending'?await waitForCommit(ack.requestId):{status:ack.status==='Completed'?'Committed':'Failed',message:ack.reason};
        save.disabled=false;save.textContent=done.status==='Committed'?'Saved - update selected views':'Save selected views';
        if(done.status==='Committed'){announce(code+' shared airport cameras saved.');bridge.send('RequestSnapshot',{});}
        else dialogError(done.message||'Airport cameras could not be saved. Try again.');
      };
      const save=button('Save selected views',()=>{
        const ids=Array.from(section.querySelectorAll('[data-airport-camera]:checked')).map(c=>c.dataset.airportCamera);
        if(!ids.length){dialogError('Select at least one camera to save.');return;}
        if(!replace.checked){performSave(ids,false);return;}
        if(confirmation)confirmation.remove();
        confirmation=make('div',undefined,'mc-airport-replace-confirmation');confirmation.setAttribute('role','alert');
        add(confirmation,make('p','Replace the selected generated views at '+code+'? This discards their saved refinements and all unsaved camera edits for this airport. Other airports and aircraft profiles are kept.','mc-warn'),
          button('Cancel',()=>{confirmation.remove();confirmation=null;},'mc-secondary'),
          button('Replace views and discard airport edits',()=>{confirmation.remove();confirmation=null;performSave(ids,true);},'mc-danger'));
        section.appendChild(confirmation);
      },'mc-primary');section.appendChild(save);
    });
  }
  function parseAirportXml(text){
    if(text.length>2*1024*1024)throw new Error('XML is larger than 2 MB.');
    if(/<!DOCTYPE|<!ENTITY/i.test(text))throw new Error('External entities are not supported.');
    const xml=new DOMParser().parseFromString(text,'application/xml');if(xml.querySelector('parsererror'))throw new Error('Invalid XML.');
    const a=xml.querySelector('Airport');if(!a)throw new Error('No Airport element found.');
    const num=(e,k,fallback)=>{const raw=e.getAttribute(k);if(raw==null)return fallback;const n=parseFloat(raw);if(!isFinite(n))return fallback;return /F(T)?$/i.test(raw)?n*.3048:n;};
    const f={icao:(a.getAttribute('ident')||'').toUpperCase(),name:a.getAttribute('name'),lat:num(a,'lat',NaN),lon:num(a,'lon',NaN),altitude:num(a,'alt',NaN),runways:[],parking:[],points:[],paths:[],signs:[]};
    if(!/^[A-Z0-9]{3,8}$/.test(f.icao)||!isFinite(f.lat)||!isFinite(f.lon)||!isFinite(f.altitude))throw new Error('Airport needs ident, lat, lon and alt.');
    const local=e=>({east:num(e,'biasX',(num(e,'lon',NaN)-f.lon)*Math.PI/180*6378137*Math.cos(f.lat*Math.PI/180)),north:num(e,'biasZ',(num(e,'lat',NaN)-f.lat)*Math.PI/180*6378137)});
    a.querySelectorAll('Runway').forEach((r,i)=>{const n=num(r,'number',i+1),d=r.getAttribute('designator')||'',suffix=({LEFT:'L',RIGHT:'R',CENTER:'C',WATER:'W'})[d]||'';f.runways.push({lat:num(r,'lat',f.lat),lon:num(r,'lon',f.lon),altitude:num(r,'alt',f.altitude),heading:num(r,'heading',0),length:num(r,'length',0),width:num(r,'width',30),primary:String(n).padStart(2,'0')+suffix,secondary:String((n+17)%36+1).padStart(2,'0')+({L:'R',R:'L',C:'C',W:'W'}[suffix]||'')});});
    a.querySelectorAll('TaxiwayParking').forEach((e,i)=>{
      const type=(e.getAttribute('type')||'').toUpperCase();if(type==='VEHICLE'||type==='NONE')return;
      const names=['NONE','PARKING','N_PARKING','NE_PARKING','E_PARKING','SE_PARKING','S_PARKING','SW_PARKING','W_PARKING','NW_PARKING','GATE','DOCK'];
      const labels=['','Parking','North parking','Northeast parking','East parking','Southeast parking','South parking','Southwest parking','West parking','Northwest parking','Gate','Dock'];
      const raw=(e.getAttribute('name')||'PARKING').toUpperCase(),suffix=(e.getAttribute('suffix')||'NONE').toUpperCase(),number=e.getAttribute('number')||String(i);
      const code=v=>/^GATE_[A-Z]$/.test(v)?12+v.charCodeAt(5)-65:names.indexOf(v),n=code(raw),s=code(suffix);
      const gate=type.indexOf('GATE')===0||n===10||n>=12;
      let name=(n>=12?'Gate '+raw.slice(-1):(labels[n]||(gate?'Gate':'Parking'))+' ')+number;
      if(s>=12)name+=suffix.slice(-1);else if(s>0)name+=' '+labels[s];
      f.parking.push(Object.assign(local(e),{key:n+':'+number+(s>0?':'+s:''),name,gate,heading:num(e,'heading',0)}));
    });
    a.querySelectorAll('TaxiwayPoint').forEach(e=>{const i=num(e,'index',-1);if(i>=0&&i<10000)f.points[i]=local(e);});
    a.querySelectorAll('TaxiwayPath').forEach(e=>f.paths.push({type:e.getAttribute('type')==='TAXI'?1:0,start:num(e,'start',-1),end:num(e,'end',-1),width:num(e,'width',20)}));
    a.querySelectorAll('TaxiwaySign').forEach((e,i)=>f.signs.push({key:[e.getAttribute('lat'),e.getAttribute('lon'),e.getAttribute('heading'),e.getAttribute('label')].join(':'),lat:num(e,'lat',NaN),lon:num(e,'lon',NaN),altitude:f.altitude,heading:num(e,'heading',0),label:e.getAttribute('label')||'Taxi sign '+(i+1)}));
    const tower=a.querySelector('Tower');if(tower)f.tower={lat:num(tower,'lat',NaN),lon:num(tower,'lon',NaN),altitude:num(tower,'alt',NaN)};
    return f;
  }
  function renderProfiles() {
    renderAirportSummary();
    const card = empty($('mc-aircraft-card'));
    const a = S.aircraft;
    const entry = make('div', undefined, 'mc-profile-entry mc-aircraft-card');
    const mark = make('span', undefined, 'mc-profile-icon');
    mark.appendChild(icon('plane'));
    const info = make('div', undefined, 'mc-profile-text');
    if (!aircraftLoaded()) {
      add(info, make('strong', 'No aircraft loaded'), make('small', 'Load a flight to use your cameras.'));
      add(entry, mark, info);
    } else {
      const isMatch = P() && a.matchProfileId === P().profileId;
      const other = a.matchProfileId && !isMatch ? S.profiles.filter(p => p.profileId === a.matchProfileId)[0] : null;
      add(info, make('strong', 'Flying: ' + aircraftName()), make('small', isMatch ? (a.matchReason || 'Matched') + ' - ' + P().name : other ? '"' + other.name + '" is the profile for this aircraft' : 'No camera profile for this aircraft yet'));
      add(entry, mark, info);
      const actions = make('div', undefined, 'mc-row-actions');
      if (other) actions.appendChild(button('Open', () => openProfile(other.profileId), 'mc-secondary'));
      if (!isMatch && !other) actions.appendChild(button('Create profile for this aircraft', createProfileForAircraft, 'mc-secondary'));
      if (!isMatch && P() && !readOnly()) actions.appendChild(button('Copy to this aircraft', useCurrentForAircraft, 'mc-secondary'));
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
        pm.appendChild(icon(profile.matchStrategy === 'none' ? 'folder' : 'plane'));
        const text = make('div', undefined, 'mc-profile-text');
        const title = make('strong', isCurrent ? P().name : profile.name);
        if (a && a.matchProfileId === profile.profileId) title.appendChild(make('span', 'This aircraft', 'mc-pill mc-accent'));
        if (profile.readOnly) title.appendChild(make('span', 'Read-only', 'mc-pill'));
        const count = isCurrent ? P().cameras.filter(c=>!c.airportIcao).length : profile.cameraCount;
        add(text, title, make('small', (profile.matchStrategy==='none'?'No aircraft assigned':'Aircraft: '+(profile.displayName || 'Unassigned')) + ' · ' + count + ' cameras' + (isCurrent && aircraftProfileDirty() ? ' - unsaved changes' : '')));
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
          const del = button('', () => confirmDialog('Delete profile?', 'Delete "' + profile.name + '" and its backups? This cannot be undone.', () => {
            bridge.request('DeleteProfile', { profileId: profile.profileId }).then(ack => announce(ack.status === 'Completed' ? 'Deleted ' + profile.name + '.' : reasonText(ack.reason), ack.status === 'Completed' ? 'ok' : 'warn'));
          }), 'mc-icon-button mc-quiet', 'trash-2');
          del.setAttribute('aria-label', 'Delete ' + profile.name);
          del.setAttribute('data-focus-key', 'del:' + profile.profileId);
          add(actions, open, del);
        }
        if(aircraftLoaded()){
          const copy=button('Copy to aircraft',()=>copyProfileForAircraft(profile.profileId,profile.name),'mc-secondary');
          copy.setAttribute('aria-label','Copy '+profile.name+' for current aircraft');copy.setAttribute('data-focus-key','copy-aircraft:'+profile.profileId);
          copy.title='Create a separate copy for '+aircraftName()+'. Camera positions may need adjustment.';copy.disabled=Boolean(profile.readOnly);actions.appendChild(copy);
        }
        row.appendChild(actions);
        list.appendChild(row);
      });
    });
    updateSavedState();
  }
  function aircraftProfileDirty() {
    const flags=dirtyFlags();
    return Boolean(typeof flags.aircraftProfile==='boolean'?flags.aircraftProfile:flags.profile);
  }
  /** Save / Discard / Cancel before replacing the working profile. Resolves 'clean' | 'discard' | null. */
  async function guardUnsaved(verb) {
    await whenIdle();
    if (!aircraftProfileDirty()) return 'clean';
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
      if (ack.status === 'Pending') return waitForCommit(ack.requestId).then(done => ({ status: done.status === 'Committed' ? 'Completed' : 'Rejected', reason: done.message }));
      return ack;
    }).then(ack => {
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
    namedDialog('New profile', aircraftLoaded() ? aircraftName() : 'My cameras', name => {
      guardUnsaved('continuing').then(choice => { if (choice) createProfile({ name, forAircraft: Boolean(S.aircraft && S.aircraft.modelKey), copyOf: null }, 'Created ' + name + '.'); });
    }, 'Create profile');
  }
  function createProfileForAircraft() {
    const a = S.aircraft;
    if (!a || !a.modelKey) return;
    guardUnsaved('continuing').then(choice => { if (choice) createProfile({ name: aircraftName() || 'My cameras', forAircraft: true, copyOf: null }, 'Created a camera profile for ' + aircraftName() + '.'); });
  }
  function useCurrentForAircraft() {
    if(P())copyProfileForAircraft(P().profileId,P().name);
  }
  async function copyProfileForAircraft(profileId,sourceName) {
    if(!aircraftLoaded()){announce('Load a flight first.','warn');return;}
    const modelKey=S.aircraft.modelKey,title=aircraftName();
    closeDialog();
    const choice=await guardUnsaved('continuing');if(!choice)return;
    const body=openDialog('Copy profile for current aircraft');
    body.appendChild(make('p','Copy "'+sourceName+'" for '+title+'. The original profile and any existing profiles for this aircraft are kept.'));
    body.appendChild(make('p','Camera positions, zoom and behavior carry across as a starting point. Different cockpit dimensions and aircraft origins may require position adjustments.','mc-note'));
    const field=make('label',undefined,'mc-field'),input=make('input');input.type='text';input.maxLength=60;input.value=(sourceName+' - '+title).slice(0,60);input.setAttribute('aria-label','New profile name');add(field,make('span','New profile name'),input);body.appendChild(field);
    let copying=false;
    dialogActions([['Cancel',()=>closeDialog()],['Create copy',async()=>{
      if(copying)return;const name=input.value.trim();if(!name){dialogError('Enter a profile name.');return;}
      if(!aircraftLoaded()||S.aircraft.modelKey!==modelKey){dialogError('The loaded aircraft changed. Close this dialog and try again.');return;}
      copying=true;closeDialog();
      await createProfile({name,from:'profile',profileId,forAircraft:true,expectedAircraftModelKey:modelKey,discard:choice==='discard'},'Created "'+name+'" for '+title+'. Adjust the camera positions as needed.');
    },'mc-primary']]);
    nextFrame(()=>{input.focus();input.select();});
  }
  function importProfile() {
    const body = openDialog('Import a camera profile');
    const file = make('input'), status = make('p', 'Choose a Camera Studio profile JSON file.', 'mc-note');
    file.type = 'file';
    file.accept = '.json,.txt,application/json,text/plain';
    file.setAttribute('aria-label', 'Profile file');
    file.hidden = true;
    status.setAttribute('role', 'status');
    const choose = button('Choose profile file', () => {
      try { file.click(); } catch (_) { dialogError('The file picker is unavailable here. Use Paste profile code below.'); }
    }, 'mc-secondary');
    const paste = make('details'), summary = make('summary', 'Paste profile code instead');
    const label = make('label', 'Profile code', 'mc-field'), input = make('textarea');
    input.setAttribute('aria-label', 'Profile code');
    input.placeholder = 'Paste a Camera Studio profile code';
    label.appendChild(input);
    add(paste, summary, label);
    add(body, make('p', 'Import a saved profile as a copy. Your existing profiles are kept.'), choose, file, status, paste);
    let fileCode = '', reading = false, importing = false, readSequence = 0;
    const native = bridge.mode !== 'demo';
    const available = !native && typeof window.FileReader === 'function' && file.type === 'file';
    let selectedFilename = '', refreshing = false;
    let refreshButton = null;
    const currentDialog = () => document.documentElement.contains(file);
    if (native) {
      choose.hidden = true;
      const folderLabel = make('label', 'Profile imports folder', 'mc-field'), folder = make('input');
      folder.type = 'text'; folder.readOnly = true; folder.setAttribute('aria-label', 'Profile imports folder');
      folder.value = '%APPDATA%\\Microsoft Flight Simulator 2024\\WASM\\MSFS2024\\fs-camera-studio\\work\\imports';
      folder.addEventListener('click', () => folder.select()); folderLabel.appendChild(folder);
      const copy = button('Copy folder path', () => {
        folder.focus(); folder.select();
        try {
          if (document.execCommand && document.execCommand('copy')) status.textContent = 'Folder path copied. Paste it into File Explorer.';
          else status.textContent = 'Press Ctrl+C to copy the selected folder path, then paste it into File Explorer.';
        } catch (_) { status.textContent = 'Press Ctrl+C to copy the selected folder path, then paste it into File Explorer.'; }
      }, 'mc-secondary');
      const list = make('div', undefined, 'mc-profile-list');
      list.setAttribute('aria-label', 'Profile files'); list.setAttribute('role', 'group');
      list.style.maxHeight = '220px'; list.style.overflowY = 'auto';
      const refresh = async () => {
        if (refreshing || importing) return;
        refreshing = true; refreshButton.disabled = true; selectedFilename = ''; empty(list);
        status.textContent = 'Reading the imports folder...'; dialogError('');
        try {
          const ack = await bridge.request('ListImportFiles', {});
          if (!currentDialog()) return;
          if (ack.status !== 'Completed') { dialogError(reasonText(ack.reason)); status.textContent = 'Could not list profile files.'; return; }
          const result = ack.result || {};
          if (result.folder) folder.value = result.folder;
          const names = Array.isArray(result.files) ? result.files : [];
          names.forEach(name => {
            const pick = button(name, () => {
              if (importing) return;
              selectedFilename = name; input.value = ''; fileCode = '';
              Array.prototype.forEach.call(list.querySelectorAll('button'), b => { b.setAttribute('aria-pressed', b === pick ? 'true' : 'false'); b.className = b === pick ? 'mc-primary' : 'mc-secondary'; });
              status.textContent = 'Ready to import ' + name; dialogError('');
            }, 'mc-secondary');
            pick.setAttribute('aria-pressed', 'false'); pick.style.display = 'block'; pick.style.width = '100%';
            list.appendChild(pick);
          });
          status.textContent = names.length ? 'Select a profile file, then Import.' : 'No JSON files found. Add profiles to this folder, then Refresh.';
          if (result.error) dialogError(result.error);
        } catch (_) { if (currentDialog()) { status.textContent = 'Could not list profile files.'; dialogError('Refresh to try again.'); } }
        finally { refreshing = false; refreshButton.disabled = false; }
      };
      refreshButton = button('Refresh', refresh, 'mc-secondary');
      body.insertBefore(folderLabel, status);
      body.insertBefore(make('p', 'Open this location in File Explorer, copy your profile JSON files into it, then Refresh. The simulator does not provide a Windows file picker or Open folder action.', 'mc-note'), status);
      body.insertBefore(copy, status); body.insertBefore(refreshButton, status); body.insertBefore(list, status);
      nextFrame(refresh);
    } else if (!available) {
      choose.disabled = true;
      status.textContent = 'File selection is unavailable in this panel. Paste the profile code below.';
      paste.open = true;
    } else {
      body.appendChild(make('p', 'If the simulator does not open a file picker, use Paste profile code instead.', 'mc-note'));
    }
    input.addEventListener('input', () => {
      ++readSequence; fileCode = ''; reading = false; file.value = ''; selectedFilename = '';
      status.textContent = 'Using pasted profile code.';
      dialogError('');
    });
    file.addEventListener('change', () => {
      const selected = file.files && file.files[0];
      if (!selected) return;
      const sequence = ++readSequence;
      fileCode = ''; input.value = ''; reading = false;
      dialogError('');
      if (selected.size > 10 * 1024 * 1024) {
        status.textContent = selected.name;
        dialogError('The profile file is larger than 10 MiB.');
        return;
      }
      reading = true; status.textContent = 'Reading ' + selected.name + '...';
      const reader = new window.FileReader();
      const current = () => sequence === readSequence && document.documentElement.contains(file);
      reader.onload = () => {
        if (!current()) return;
        reading = false;
        fileCode = String(reader.result || '').replace(/^\uFEFF/, '').trim();
        status.textContent = selected.name;
        if (!fileCode) dialogError('This profile file is empty. Choose another file.');
      };
      reader.onerror = reader.onabort = () => {
        if (!current()) return;
        reading = false; status.textContent = selected.name;
        dialogError('Could not read this file. Choose it again or paste the profile code below.');
      };
      try { reader.readAsText(selected, 'UTF-8'); } catch (_) { reader.onerror(); }
    });
    if (bridge.mode === 'demo') {
      paste.appendChild(button('Use example profile', () => {
        bridge.request('ExportProfile', { profileId: (S.profiles[1] || S.profiles[0]).profileId, working: false }).then(ack => {
          if (ack.result && document.documentElement.contains(file)) { ++readSequence; reading = false; fileCode = ''; input.value = ack.result.code; status.textContent = 'Using example profile code.'; }
        });
      }, 'mc-secondary'));
    }
    dialogActions([['Cancel', () => closeDialog()], ['Import', async () => {
      if (importing) return;
      if (reading) { dialogError('Wait for the profile file to finish loading.'); return; }
      const code = fileCode || input.value.trim();
      if (!code && !selectedFilename) { dialogError('Choose a profile file or paste its code first.'); return; }
      importing = true; choose.disabled = true; input.disabled = true; if (refreshButton) refreshButton.disabled = true;
      dialogError('');
      try {
        const ack = selectedFilename ? await bridge.request('ImportProfileFile', { filename: selectedFilename }) : await bridge.request('ImportProfile', { code });
        if (ack.status === 'Pending') {
          const done = await waitForCommit(ack.requestId);
          if (!document.documentElement.contains(file)) return;
          if (done.status !== 'Committed') { dialogError(reasonText(done.message)); return; }
        } else if (ack.status !== 'Completed') { if (document.documentElement.contains(file)) dialogError(reasonText(ack.reason)); return; }
        if (document.documentElement.contains(file)) showImportReport(ack.result || {});
      } catch (_) {
        if (document.documentElement.contains(file)) dialogError('The profile could not be imported. Try again.');
      } finally {
        importing = false; choose.disabled = !available; input.disabled = false; if (refreshButton) refreshButton.disabled = false;
      }
    }, 'mc-primary']]);
    nextFrame(() => (native ? refreshButton : available ? choose : input).focus());
  }
  function showImportReport(result) {
    const report = result.report || {};
    const body = openDialog('Profile imported');
    body.appendChild(make('p', '"' + (report.name || 'The profile') + '" was added as a copy with new camera IDs.'));
    const kv = make('dl', undefined, 'mc-kv');
    const am = report.aircraftMatch || { matches: report.matchesLoadedAircraft, displayName: report.aircraft };
    add(kv, make('dt', 'Aircraft'), make('dd', (am.matches ? 'Matches the loaded aircraft' : 'Not the loaded aircraft') + (am.displayName ? ' - ' + am.displayName : '')));
    add(kv, make('dt', 'Cameras'), make('dd', String(report.cameraCount || 0)));
    body.appendChild(kv);
    const unsupported = report.unsupported || report.notes || [];
    if (unsupported.length) {
      body.appendChild(make('p', 'Kept, but not available in this version:', 'mc-warn'));
      const ul = make('ul', undefined, 'mc-report-list');
      unsupported.forEach(item => ul.appendChild(make('li', typeof item === 'string' ? item : (item.message || item.name || ''))));
      body.appendChild(ul);
    } else body.appendChild(make('p', 'Everything in the code was imported.'));
    const actions=[['Close', () => closeDialog()], ['Open profile', () => { closeDialog(); if (result.profileId) openProfile(result.profileId); }, 'mc-secondary']];
    if(aircraftLoaded()&&result.profileId){
      body.appendChild(make('p','You can also copy these views for '+aircraftName()+', even if this profile was made for a different aircraft.','mc-note'));
      actions.push(['Copy for current aircraft',()=>copyProfileForAircraft(result.profileId,report.name||'Imported profile'),'mc-primary']);
    }
    dialogActions(actions);
  }
  function exportProfile() {
    const body = openDialog('Export a camera profile');
    body.appendChild(make('p', 'Choose an aircraft profile to export. Shared airport cameras are stored separately and are not included. Your current camera stays open.'));
    const list = make('div', undefined, 'mc-profile-list');
    const profiles = S.profiles.slice();
    if (P() && !profiles.some(p => p.profileId === P().profileId)) profiles.unshift(P());
    profiles.forEach(profile => {
      const row = make('div', undefined, 'mc-profile-entry');
      const info = make('div', undefined, 'mc-profile-text');
      info.appendChild(make('strong', profile.name || 'Camera profile'));
      const active = P() && profile.profileId === P().profileId;
      info.appendChild(make('small', (active ? 'Current profile' : 'Saved profile') + (profile.aircraftMatch && profile.aircraftMatch.displayName ? ' - ' + profile.aircraftMatch.displayName : '')));
      const exportSaved = button('Export saved', () => exportSelectedProfile(profile.profileId, false), 'mc-secondary');
      exportSaved.title = 'Export the saved version of ' + profile.name;
      add(row, info, exportSaved);
      if (active && (aircraftProfileDirty() || localPending())) {
        const draft = button('Export with edits', () => exportSelectedProfile(profile.profileId, true), 'mc-secondary');
        draft.title = 'Include unsaved camera changes in the exported copy';
        row.appendChild(draft);
      }
      list.appendChild(row);
    });
    body.appendChild(list);
    if (!profiles.length) body.appendChild(make('p', 'No saved profiles are available yet.'));
    dialogActions([['Close', () => closeDialog()]]);
  }
  function exportSelectedProfile(profileId, working) {
    whenIdle().then(() => bridge.request('ExportProfile', { profileId, working })).then(ack => {
      if (ack.status !== 'Completed' || !ack.result) { dialogError(reasonText(ack.reason)); return; }
      const label = ack.result.label === 'working copy' ? make('p', 'This copy includes unsaved camera changes.', 'mc-warn') : null;
      codeDialog(ack.result.label === 'working copy' ? 'Export working copy' : 'Export saved profile', 'Copy this profile code to share or import on another installation.', ack.result.code, label);
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

  /* ------------- Settings ------------- */
  function applyMainOpacity() {
    if(!S.working)return;
    const ui=S.working.settings.ui,value=isFinite(ui.mainOpacity)?M.clamp(ui.mainOpacity,0,1):1;
    const editor=$('mc-editor');
    editor.style.background='rgba('+(ui.theme==='light'?'246,248,251,':'16,25,35,')+value+')';
    // A single translucent backdrop, with opaque text/inputs and no stacked sidebar/footer fill.
    $$('.mc-global-nav,.mc-browser,.mc-savebar',editor).forEach(part=>{part.style.background=value<1?'transparent':'';});
  }
  function renderMainOpacitySettings() {
    if(!S.working)return;
    let slider=$('mc-setting-main-opacity');
    if(!slider){
      const mini=$('mc-setting-mini-opacity').closest('label'),group=make('div',undefined,'mc-opacity-settings');
      mini.before(group);group.appendChild(mini);
      const row=make('label',undefined,'mc-inline-field'),label=make('span','Main background opacity '),value=make('output');
      value.id='mc-main-opacity-value';label.appendChild(value);
      slider=make('input');slider.type='range';slider.id='mc-setting-main-opacity';slider.min='0';slider.max='100';slider.step='5';
      slider.title='Set the main window background opacity. Text and controls stay readable.';
      slider.addEventListener('input',event=>{S.working.settings.ui.mainOpacity=Number(event.target.value)/100;settingChanged();});
      add(row,label,slider);group.appendChild(row);
    }
    const value=Math.round((isFinite(S.working.settings.ui.mainOpacity)?M.clamp(S.working.settings.ui.mainOpacity,0,1):1)*100);
    slider.value=String(value);$('mc-main-opacity-value').textContent=value+'%';
  }
  function applySettings() {
    if (!S.working) return;
    const ui = S.working.settings.ui;
    if (S.precision !== ui.precision) setPrecision(ui.precision);
    root.dataset.shortcuts = String(ui.shortcuts !== false);
    root.dataset.contrast = String(Boolean(ui.contrast));
    root.dataset.theme = ui.theme === 'light' ? 'light' : 'dark';
    root.dataset.reducedMotion = String(Boolean(ui.reducedMotion));
    root.dataset.miniEnabled = String(ui.miniEnabled !== false);
    const miniOpacity=isFinite(ui.miniOpacity)?M.clamp(ui.miniOpacity,0,1):0.65;
    root.style.setProperty('--mc-mini-opacity',String(miniOpacity));
    // Coherent can reject a CSS variable inside rgba(), leaving the old opaque dock background.
    $('mc-mini-panel').style.background='rgba('+(ui.theme==='light'?'238,245,248,':'14,25,35,')+miniOpacity+')';
    applyMainOpacity();
    $('mc-name-display-label').textContent='Mini panel category display';
    root.dataset.nameDisplay = ui.nameDisplay === 'icons' || ui.nameDisplay === 'labels' ? ui.nameDisplay : 'both';
    layoutMiniCategories();
    if (IS_MINI) applyMiniMode(ui.miniEnabled === false ? 'off' : ui.miniHover !== false ? 'hover' : 'always');
    if (Boolean(ui.miniPinned) !== miniPinned) setMiniPinned(Boolean(ui.miniPinned), false);
  }
  /** Mini surface (design sec. 4.4/sec. 4.5): handle + hover reveal, fixed dock, or a short note when turned off. */
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
  function renderSaveFeedbackSettings() {
    const feedback=window.FSCSSaveFeedback;
    if(!feedback)return;
    let row=$('mc-save-feedback-setting');
    if(!row){
      row=make('div',undefined,'mc-inline-field');row.id='mc-save-feedback-setting';
      const text=make('span');add(text,make('strong','Save confirmation'),make('small',undefined,'mc-muted'));
      const test=button('Test sound',()=>{feedback.test().then(result=>{row.querySelector('small').textContent=result.message;});},'mc-secondary');
      test.id='mc-test-save-feedback';test.title='Play the save confirmation without changing your cameras';
      add(row,text,test);$('mc-setting-move-speed').closest('.mc-inline-field').after(row);
    }
    const native=feedback.isNative(),last=feedback.getStatus();
    row.querySelector('small').textContent=last.state==='idle'?(native?'Requests simulator confirmation chime after saving. Spoken confirmation is unavailable in this panel.':'Says Cameras saved after saving your views.'):last.message;
    row.querySelector('small').style.display='block';
  }
  function renderGlobalTrackingSettings() {
    if(!S.working)return;
    let group=$('mc-global-tracking-settings');
    if(!group){
      group=make('div');group.id='mc-global-tracking-settings';
      group.appendChild(make('div','Global tracking','mc-section-label mc-spaced'));
      const head=make('label',undefined,'mc-switch-row'),copy=make('span',undefined,'mc-switch-copy');
      add(copy,make('span','Head tracking'),make('small','TrackIR and compatible MSFS inputs. Shared by all cameras and profiles; off by default.'));
      const toggle=make('input',undefined,'mc-switch');toggle.type='checkbox';toggle.id='mc-setting-global-head-tracking';
      toggle.addEventListener('change',()=>trackingPreference(null,toggle.checked?'on':'off'));add(head,copy,toggle);group.appendChild(head);
      const row=make('section',undefined,'mc-airport-tracking-settings');
      row.appendChild(make('h4','Airport camera tracking'));
      row.appendChild(make('p','Defaults for airport cameras across all aircraft profiles. Individual airport cameras can override these in Behavior > Airport camera settings. Exterior views share the tracking mode; cockpit cameras are excluded.','mc-note'));
      const choices=make('div',undefined,'mc-tracking-settings-choices');choices.id='mc-setting-global-aircraft-tracking';
      choices.setAttribute('role','group');choices.setAttribute('aria-label','Airport camera tracking');
      [['off','Off','Keep the saved view direction.'],['heading','Heading only','Follow left and right; keep the saved pitch.'],['both','Heading + pitch','Follow your aircraft horizontally and vertically. Default.']].forEach(([value,label,detail])=>{
        const choose=button(label,()=>{S.working.settings.host.globalAircraftTracking=value;settingChanged();renderCameraTools();},'mc-secondary mc-tracking-choice');
        choose.dataset.value=value;choose.setAttribute('aria-label',label);choose.title=detail;
        choose.appendChild(make('small',detail));choices.appendChild(choose);
      });
      row.appendChild(choices);
      const zoom=make('label',undefined,'mc-switch-row'),zoomCopy=make('span',undefined,'mc-switch-copy');
      add(zoomCopy,make('span','Auto zoom'),make('small','Keep your aircraft framed as its distance changes. Airport cameras only.'));
      const zoomToggle=make('input',undefined,'mc-switch');zoomToggle.type='checkbox';zoomToggle.id='mc-setting-airport-autozoom';
      zoomToggle.addEventListener('change',()=>{S.working.settings.host.airportAutoZoomEnabled=zoomToggle.checked;settingChanged();});
      add(zoom,zoomCopy,zoomToggle);row.appendChild(zoom);
      const horizon=make('label',undefined,'mc-switch-row'),horizonCopy=make('span',undefined,'mc-switch-copy');
      add(horizonCopy,make('span','Keep distant views near the horizon'),make('small','Soften steep tracking pitch at long distances across elevation differences. Heading still follows your aircraft. Turn off for unrestricted vertical tracking.'));
      const horizonToggle=make('input',undefined,'mc-switch');horizonToggle.type='checkbox';horizonToggle.id='mc-setting-airport-horizon-guard';
      horizonToggle.addEventListener('change',()=>{S.working.settings.host.airportTrackingHorizonGuard=horizonToggle.checked;settingChanged();});
      add(horizon,horizonCopy,horizonToggle);row.appendChild(horizon);
      const fields=make('div',undefined,'mc-autozoom-fields');fields.id='mc-autozoom-options';
      const specs=[
        ['airportAutoZoomFrameMeters','Framing size',40,5,200,1,'m','Smaller values give a closer view; larger values leave more space around your aircraft.'],
        ['airportAutoZoomMinFovDeg','Closest zoom (minimum FOV)',10,2,120,1,'deg','Smallest field of view allowed. Lower values allow more magnification.'],
        ['airportAutoZoomMaxFovDeg','Widest zoom (maximum FOV)',90,2,120,1,'deg','Largest field of view allowed when the aircraft approaches.'],
        ['airportAutoZoomResponseSeconds','Smoothing time',1,0.1,5,0.1,'s','Higher values make zoom changes more gradual.']
      ];
      specs.forEach(([key,label,fallback,min,max,step,unit,help])=>{
        const field=make('label',undefined,'mc-autozoom-field');field.title=help;
        field.appendChild(make('span',label));const line=make('span',undefined,'mc-autozoom-input');
        const input=make('input',undefined,'mc-input');input.type='number';input.id='mc-setting-'+key;input.min=min;input.max=max;input.step=step;input.dataset.fallback=fallback;input.setAttribute('aria-label',label);
        input.addEventListener('change',()=>{
          const host=S.working.settings.host,n=Number(input.value);host[key]=Math.max(min,Math.min(max,input.value.trim()!==''&&isFinite(n)?n:fallback));
          if(key==='airportAutoZoomMinFovDeg')host.airportAutoZoomMaxFovDeg=Math.max(host[key],host.airportAutoZoomMaxFovDeg==null?90:host.airportAutoZoomMaxFovDeg);
          if(key==='airportAutoZoomMaxFovDeg')host.airportAutoZoomMinFovDeg=Math.min(host[key],host.airportAutoZoomMinFovDeg==null?10:host.airportAutoZoomMinFovDeg);
          input.value=String(host[key]);settingChanged();
        });
        add(line,input,make('span',unit,'mc-note'));add(field,line,make('small',help,'mc-note'));fields.appendChild(field);
      });
      row.appendChild(fields);
      const status=make('p',undefined,'mc-note');status.id='mc-airport-autozoom-status';row.appendChild(status);
      row.appendChild(make('h4','Default heights by category'));
      row.appendChild(make('p','Meters above the scenery feature; tower height uses airport elevation. Runway approaches use at least 15 m and overviews at least 25 m. An individual height override can use an exact value.','mc-note'));
      const heights=make('div',undefined,'mc-autozoom-fields');
      [['runways','Runways',3],['gates','Gates',3],['apron','Apron / parking',3],['taxi','Taxi signs / intersections',3],['tower','Tower',35]].forEach(([key,label,fallback])=>{
        const field=make('label',undefined,'mc-autozoom-field'),input=make('input');Object.assign(input,{type:'number',min:0,max:1000,step:1,id:'mc-setting-airport-height-'+key});input.dataset.category=key;input.dataset.fallback=fallback;input.setAttribute('aria-label',label+' height');
        input.addEventListener('change',()=>{const host=S.working.settings.host,n=Number(input.value);host.airportCategoryHeights=Object.assign({runways:3,gates:3,apron:3,taxi:3,tower:35},host.airportCategoryHeights||{},{[key]:input.value.trim()!==''&&isFinite(n)?M.clamp(n,0,1000):fallback});input.value=host.airportCategoryHeights[key];settingChanged();});
        const line=make('span',undefined,'mc-autozoom-input');add(line,input,make('span','m','mc-note'));add(field,make('span',label),line);heights.appendChild(field);
      });row.appendChild(heights);
      row.appendChild(make('p','New cameras inherit these heights. Older cameras keep their saved altitude until you choose a height option in that camera’s settings. Moving a camera vertically creates its own height override.','mc-note'));
      row.appendChild(make('p','Applies immediately to all airports. Save changes or press Ctrl+S to keep your choices. Head tracking is controlled separately in Tracking.','mc-note'));
      group.appendChild(row);
      const anchor=$('mc-mouse-settings')||$('mc-setting-look-speed').closest('.mc-inline-field');
      anchor.before(group);
    }
    const host=S.working.settings.host;
    $('mc-setting-global-head-tracking').checked=host.globalHeadTrackingEnabled===true;
    const tracking=host.globalAircraftTracking||'both';
    $$('button[data-value]',$('mc-setting-global-aircraft-tracking')).forEach(choice=>choice.setAttribute('aria-pressed',String(choice.dataset.value===tracking)));
    const zoomEnabled=host.airportAutoZoomEnabled===true;
    $('mc-setting-airport-autozoom').checked=zoomEnabled;
    $('mc-setting-airport-horizon-guard').checked=host.airportTrackingHorizonGuard!==false;
    $$('#mc-autozoom-options input').forEach(input=>{
      const key=input.id.replace('mc-setting-','');
      if(document.activeElement!==input)input.value=String(host[key]==null?input.dataset.fallback:host[key]);
      input.disabled=!zoomEnabled;
    });
    $('mc-airport-autozoom-status').textContent=!zoomEnabled?'Auto zoom is off. Your manual zoom is used.':tracking==='off'?'Auto zoom is waiting: enable aircraft tracking in the Tracking section.':'Auto zoom follows aircraft distance within these limits. Right-drag temporarily pauses it; turn it off to use manual zoom.';
    $$('#mc-airport-defaults-content input[data-category],#mc-global-tracking-settings input[data-category]').forEach(input=>{if(document.activeElement!==input)input.value=String((host.airportCategoryHeights||{})[input.dataset.category]==null?input.dataset.fallback:host.airportCategoryHeights[input.dataset.category]);});
  }
  function renderSettingsLayout() {
    const page=$('mc-page-settings');
    if(!$('mc-settings-navigation')) {
      const source=page.querySelector('.mc-settings-section'),layout=make('div',undefined,'mc-settings-layout'),nav=make('nav',undefined,'mc-settings-navigation'),content=make('div',undefined,'mc-settings-content');
      nav.id='mc-settings-navigation';nav.setAttribute('aria-label','Settings sections');source.before(layout);add(layout,nav,content);
      const descriptions={appearance:'Panel appearance and quick access.',movement:'Mouse look, keyboard movement and camera precision.',tracking:'Shared controls for every aircraft and profile.',airport:'Shared airport defaults. Individual cameras can override these values.',updates:'Version, updates and troubleshooting.'};
      [['appearance','Appearance'],['movement','Movement'],['tracking','Tracking'],['airport','Airport defaults'],['updates','Saving & updates']].forEach(([key,label])=>{
        const pane=make('section',undefined,'mc-settings-pane');pane.id='mc-settings-section-'+key;pane.setAttribute('aria-labelledby','mc-settings-link-'+key);
        add(pane,make('h4',label,'mc-settings-heading'),make('p',descriptions[key],'mc-note'));
        const link=button(label,()=>showSettingsSection(key),'mc-secondary');link.id='mc-settings-link-'+key;link.setAttribute('aria-controls',pane.id);nav.appendChild(link);content.appendChild(pane);
      });
      const move=(id,key)=>{const node=$(id);if(node){const row=node.closest('.mc-inline-field,.mc-switch-row,.mc-profile-actions')||node;$('mc-settings-section-'+key).appendChild(row);}};
      ['shortcuts','contrast','theme','reduced-motion','remember','mini-enabled','mini-hover','name-display'].forEach(id=>move('mc-setting-'+id,'appearance'));
      const opacity=source.querySelector('.mc-opacity-settings');if(opacity)$('mc-settings-section-appearance').appendChild(opacity);
      ['precision','mouse-enabled','mouse-sensitivity','mouse-smoothing','mouse-invert','look-speed','move-speed'].forEach(id=>move('mc-setting-'+id,'movement'));
      move('mc-mouse-defaults','movement');
      const movement=$('mc-settings-section-movement');movement.appendChild(make('p','Movement follows your current key assignments in Bindings. Hold your faster-movement key to increase speed. Inputs pause while typing.','mc-note mc-settings-keyboard'));
      move('mc-setting-backups','updates');move('mc-save-feedback-setting','updates');move('mc-diagnostics','updates');move('mc-qual-state','updates');move('mc-setting-unqualified','updates');
      move('mc-setting-airport-radius','airport');
      const global=$('mc-global-tracking-settings'),airport=global.querySelector('.mc-airport-tracking-settings'),zoom=$('mc-setting-airport-autozoom').closest('label');
      const defaults=make('div',undefined,'mc-airport-defaults-content');defaults.id='mc-airport-defaults-content';
      while(zoom.nextSibling)defaults.appendChild(zoom.nextSibling);defaults.insertBefore(zoom,defaults.firstChild);
      $('mc-settings-section-airport').appendChild(defaults);$('mc-settings-section-tracking').appendChild(global);
      const title=global.querySelector('.mc-section-label');if(title)title.remove();
      airport.querySelector('h4').textContent='Aircraft tracking';
      source.remove();
      root.addEventListener('click',event=>{if(event.target.closest&&event.target.closest('#mc-app-version'))setTimeout(()=>showSettingsSection('updates'),0);});
      showSettingsSection('appearance');
    }
    const updates=$('mc-update-settings'),target=$('mc-settings-section-updates');if(updates&&updates.parentNode!==target)target.insertBefore(updates,target.children[2]||null);
  }
  function showSettingsSection(key) {
    const page=$('mc-page-settings');
    $$('.mc-settings-pane',page).forEach(pane=>{pane.hidden=pane.id!=='mc-settings-section-'+key;});
    $$('#mc-settings-navigation button',page).forEach(link=>{link.setAttribute('aria-current',link.id==='mc-settings-link-'+key?'page':'false');});
    page.scrollTop=0;
  }
  function renderSettings() {
    if (!S.working) return;
    const ui = S.working.settings.ui, host = S.working.settings.host;
    ['shortcuts', 'contrast', 'backups', 'remember'].forEach(key => { $('mc-setting-' + key).checked = ui[key] !== false && Boolean(ui[key]); });
    $('mc-setting-precision').value = ui.precision || 'normal';
    $$('[data-value]', $('mc-setting-theme')).forEach(b => b.setAttribute('aria-pressed', String(b.dataset.value === (ui.theme === 'light' ? 'light' : 'dark'))));
    $('mc-setting-reduced-motion').checked = Boolean(ui.reducedMotion);
    $('mc-setting-mini-enabled').checked = ui.miniEnabled !== false;
    $('mc-setting-mini-hover').checked = ui.miniHover !== false;
    $('mc-setting-mini-hover').disabled = ui.miniEnabled === false;
    const opacity = Math.round((isFinite(ui.miniOpacity) ? ui.miniOpacity : 0.65) * 100);
    $$('[data-value]', $('mc-setting-name-display')).forEach(b => b.setAttribute('aria-pressed', String(b.dataset.value === (ui.nameDisplay || 'both'))));
    $('mc-setting-mini-opacity').value = String(opacity);
    $('mc-mini-opacity-value').textContent = opacity + '%';
    $('mc-setting-airport-radius').value=String(host.airportRadiusNm || 25);
    $('mc-setting-mouse-enabled').checked = host.mouseLookEnabled !== false;
    $('mc-setting-mouse-invert').checked = Boolean(host.mouseInvertY);
    [['mc-setting-mouse-sensitivity', host.mouseSensitivity == null ? 1 : host.mouseSensitivity], ['mc-setting-mouse-smoothing', host.mouseSmoothingMs == null ? 55 : host.mouseSmoothingMs]].forEach(([id, value]) => { if (document.activeElement !== $(id)) $(id).value = String(value); });
    $('mc-setting-unqualified').checked = Boolean(host.allowUnqualifiedAdapter);
    if (document.activeElement !== $('mc-setting-look-speed')) $('mc-setting-look-speed').value = String(host.lookSpeedDegPerSec);
    if (document.activeElement !== $('mc-setting-move-speed')) $('mc-setting-move-speed').value = String(host.moveSpeedMetersPerSec);
    $('mc-setting-unqualified').closest('label').hidden=true;
    $('mc-qual-state').textContent = qualificationText();
    $('mc-qual-state').parentNode.querySelector('span').textContent='Camera directions';
    $('mc-qual-run').hidden=true;
    $('mc-qual-run').disabled = !S.snapshot || !aircraftLoaded() || Boolean(Q);
    $('mc-qual-run').title = aircraftLoaded() ? '' : 'Load a flight first.';
    renderSaveFeedbackSettings();
    renderMainOpacitySettings();
    renderGlobalTrackingSettings();
    renderSettingsLayout();
  }
  function settingChanged() { saveSettings(); applySettings(); renderSettings(); }

  /* ------------- Simulator axis check (QualificationStart / Probe / Restore / Record / End) ------------- */
  const AXIS_NAMES = { forward: 'Forward', right: 'Right', up: 'Up', pitch: 'Pitch', heading: 'Heading', bank: 'Roll' };
  const DEFAULT_AXES = ['forward', 'right', 'up', 'pitch', 'heading', 'bank'];
  let Q = null;
  const waitFor = (test, ms) => new Promise(resolve => {
    const start = Date.now();
    (function poll() { if (test()) resolve(true); else if (Date.now() - start > ms) resolve(false); else setTimeout(poll, 100); })();
  });
  function qualificationText() {
    return 'Camera directions are configured automatically. No calibration needed.';
  }
  /** Replace the open wizard dialog's content without closing it (closing ends the session). */
  function wizardStep(title, nodes, actions) {
    if (!Q || !dialogOpen()) return;
    $('mc-dialog-title').textContent = title;
    const body = empty($('mc-dialog-body'));
    nodes.forEach(node => add(body, node));
    dialogError('');
    dialogActions(actions);
    nextFrame(() => { const target = $('mc-dialog-actions').querySelector('.mc-primary') || $('mc-dialog-close'); if (target) target.focus(); });
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
    wizardStep('Simulator axis check', [make('p', 'Taking camera control...')], [['Cancel', () => closeDialog()]]);
    bridge.request('QualificationStart', {}).then(ack => {
      if (Q !== q) return null;
      if (ack.status !== 'Completed') { dialogError(reasonText(ack.reason)); return null; }
      q.started = true;
      if (ack.result && Array.isArray(ack.result.axes) && ack.result.axes.length) q.axes = ack.result.axes;
      const target = [S.selection.cameraId, P().defaultCameraId].map(camIn).filter(c => c && c.mode === 'aircraftCockpit' && modeAvailability(c.mode).available)[0]
        || [S.selection.cameraId, P().defaultCameraId].map(camIn).filter(c => c && modeAvailability(c.mode).available)[0];
      if (!target) { dialogError('Choose a cockpit camera in this profile first.'); return null; }
      q.cameraId = target.id;
      let activation = null;
      // State events are coalesced: wait (max 2 s) for the CapabilitiesChanged that follows QualificationStart.
      return waitFor(() => Q !== q || S.caps.canActivate, 2000)
        .then(() => (Q === q ? command('ActivateCamera', { cameraId: target.id }, { quiet: true }) : null))
        .then(ack => {
          activation = ack;
          if (!ack || ack.status === 'Rejected' || ack.status === 'TimedOut') return false;
          return waitFor(() => Q !== q || (S.ownership.state === 'Active' && S.active.cameraId === q.cameraId && !S.active.transitioning), 8000);
        })
        .then(ok => {
          if (Q !== q) return;
          if (!ok) {
            wizardStep('Simulator axis check', [make('p', 'Camera Studio could not take control of the view: ' + activationFailure(activation))], [['Close', () => closeDialog()], ['Try again', startAxisCheck, 'mc-primary']]);
            return;
          }
          probeStep();
        });
    });
  }
  /** The real reason activation did not reach Active: the ack's reason, else ownership, else capabilities. */
  function activationFailure(ack) {
    let text;
    if (ack && (ack.status === 'Rejected' || ack.status === 'TimedOut')) text = reasonText(ack.reason);
    else if (S.ownership.reason && S.ownership.state !== 'Active') text = reasonText(S.ownership.reason) + ' (' + (STATUS_TEXT[S.ownership.state] || S.ownership.state) + ')';
    else if (S.ownership.state !== 'Active') text = STATUS_TEXT[S.ownership.state] || S.ownership.state;
    else text = S.caps.blockReason || 'The view did not settle in time.';
    return /[.!?]$/.test(text) ? text : text + '.';
  }
  function stepTitle(q, label) { return 'Step ' + (q.index + 1) + ' of ' + (q.axes.length + 1) + ' - ' + label; }
  function probeStep() {
    const q = Q;
    if (!q) return;
    if (q.index >= q.axes.length) { recallStep(); return; }
    const axis = q.axes[q.index];
    const title = stepTitle(q, AXIS_NAMES[axis] || axis);
    wizardStep(title, [make('p', 'Moving the view...')], [['Cancel', () => closeDialog()]]);
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
    wizardStep(stepTitle(q, AXIS_NAMES[axis] || axis), [make('p', 'Returning the view...')], [['Cancel', () => closeDialog()]]);
    bridge.request('QualificationRestore', {}).then(() => { if (Q !== q) return; q.index++; probeStep(); });
  }
  function recallStep() {
    const q = Q;
    const title = stepTitle(q, 'Recall');
    wizardStep(title, [make('p', 'Look at a landmark outside or on the panel, then press Recall.')], [['Cancel', () => closeDialog()], ['Recall', () => {
      wizardStep(title, [make('p', 'Recalling the camera...')], [['Cancel', () => closeDialog()]]);
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
    wizardStep('Recording the result', [make('p', 'Saving the axis check for this simulator build...')], []);
    bridge.request('QualificationRecord', { answers: q.answers, recall, notes: '' }).then(ack => {
      if (Q !== q) return null;
      if (ack.status === 'Completed') { showQualificationResult(q, ack.result || {}, null); return null; }
      if (ack.status !== 'Pending') { showQualificationResult(q, null, reasonText(ack.reason)); return null; }
      return waitForCommit(ack.requestId).then(done => {
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
      nodes.push(make('p', 'Axis mapping recorded - live control is enabled for this simulator build.'));
      if (reversed.length) nodes.push(make('p', 'Corrected automatically: ' + names(reversed) + ' moved the opposite way and was flipped.'));
    } else {
      nodes.push(make('p', 'The axis check did not pass:', 'mc-warn'));
      const ul = make('ul', undefined, 'mc-report-list');
      failedAxes.forEach(a => ul.appendChild(make('li', (AXIS_NAMES[a] || a) + ' moved some other way.')));
      if (!g03) ul.appendChild(make('li', 'Recall did not return the view to the same place.'));
      nodes.push(ul);
      nodes.push(make('p', 'Live control stays off for this simulator build unless you allow it in Settings > Simulator.'));
    }
    wizardStep('Axis check result', nodes, [['Close', () => closeDialog(), 'mc-primary']]);
    renderSettings();
  }

  /* ------------- Custom scrollbars (Coherent GT shows no native scrollbars) ------------- */
  const scrollers = [];
  function attachScrollbar(el, axis) {
    if (!el || el.getAttribute('data-fscs-scroll')) return;
    const horizontal = axis === 'x';
    const host = el.parentNode;
    if (getComputedStyle(host).position === 'static') host.style.position = 'relative';
    const track = make('div', undefined, 'mc-sb' + (horizontal ? ' mc-sb-x' : ''));
    const thumb = make('div', undefined, 'mc-sb-thumb');
    track.setAttribute('aria-hidden', 'true');
    track.appendChild(thumb);
    host.appendChild(track);
    el.setAttribute('data-fscs-scroll', axis || 'y');
    el.classList.add('mc-scroll');
    const entry = { el, host, track, thumb, horizontal, thumbLen: 0, trackLen: 0 };
    const metric = () => (horizontal
      ? { size: el.clientWidth, total: el.scrollWidth, pos: el.scrollLeft }
      : { size: el.clientHeight, total: el.scrollHeight, pos: el.scrollTop });
    const setPos = v => { if (horizontal) el.scrollLeft = v; else el.scrollTop = v; };
    entry.metric = metric;
    el.addEventListener('scroll', () => updateScrollbar(entry));
    // Wheel: let native scrolling work; if the host did not scroll the element, scroll it ourselves.
    const onWheel = event => {
      if (event.fscsScrolled) return;
      const m = metric();
      if (m.total <= m.size + 1) return;
      let delta;
      if (typeof event.deltaY === 'number') {
        delta = horizontal && Math.abs(event.deltaX || 0) > Math.abs(event.deltaY) ? event.deltaX : event.deltaY;
        if (event.deltaMode === 1) delta *= 16; else if (event.deltaMode === 2) delta *= m.size;
      } else delta = -(event.wheelDelta || 0) * 0.4;
      if (!delta || (delta < 0 && m.pos <= 0) || (delta > 0 && m.pos >= m.total - m.size - 1)) return; // let an outer region scroll
      event.fscsScrolled = true;
      const before = m.pos;
      setTimeout(() => {
        if (metric().pos === before) setPos(M.clamp(before + delta, 0, m.total - m.size));
        updateScrollbar(entry);
      }, 0);
    };
    el.addEventListener('wheel', onWheel);
    el.addEventListener('mousewheel', onWheel);
    // Thumb dragging and paging on the track.
    thumb.addEventListener('mousedown', event => {
      if (event.button !== 0) return;
      event.preventDefault();
      event.stopPropagation();
      const start = horizontal ? event.clientX : event.clientY;
      const startPos = metric().pos;
      track.setAttribute('data-drag', 'true');
      const move = e => {
        const m = metric();
        const room = Math.max(1, entry.trackLen - entry.thumbLen);
        setPos(M.clamp(startPos + ((horizontal ? e.clientX : e.clientY) - start) * (m.total - m.size) / room, 0, m.total - m.size));
        updateScrollbar(entry);
      };
      const up = () => { track.removeAttribute('data-drag'); document.removeEventListener('mousemove', move, true); document.removeEventListener('mouseup', up, true); };
      document.addEventListener('mousemove', move, true);
      document.addEventListener('mouseup', up, true);
    });
    track.addEventListener('mousedown', event => {
      if (event.target === thumb || event.button !== 0) return;
      event.preventDefault();
      const rect = thumb.getBoundingClientRect();
      const m = metric();
      const forward = horizontal ? event.clientX > rect.right : event.clientY > rect.bottom;
      setPos(M.clamp(m.pos + (forward ? 1 : -1) * m.size * 0.9, 0, m.total - m.size));
      updateScrollbar(entry);
    });
    scrollers.push(entry);
    updateScrollbar(entry);
  }
  function updateScrollbar(entry) {
    const el = entry.el, track = entry.track, host = entry.host;
    const m = entry.metric();
    const visible = el.getClientRects().length > 0 && el.offsetParent !== null;
    if (!visible || m.total <= m.size + 1) { if (track.style.display !== 'none') track.style.display = 'none'; return; }
    const hostRect = host.getBoundingClientRect(), r = el.getBoundingClientRect();
    const left = r.left - hostRect.left - host.clientLeft + host.scrollLeft;
    const top = r.top - hostRect.top - host.clientTop + host.scrollTop;
    track.style.display = 'block';
    if (entry.horizontal) {
      entry.trackLen = Math.max(0, r.width - 8);
      track.style.left = (left + 4) + 'px';
      track.style.top = (top + r.height - 8) + 'px';
      track.style.width = entry.trackLen + 'px';
    } else {
      entry.trackLen = Math.max(0, r.height - 8);
      track.style.left = (left + r.width - 8) + 'px';
      track.style.top = (top + 4) + 'px';
      track.style.height = entry.trackLen + 'px';
    }
    entry.thumbLen = Math.min(entry.trackLen, Math.max(24, entry.trackLen * m.size / m.total));
    const offset = (entry.trackLen - entry.thumbLen) * (m.pos / Math.max(1, m.total - m.size));
    if (entry.horizontal) { entry.thumb.style.width = entry.thumbLen + 'px'; entry.thumb.style.left = offset + 'px'; }
    else { entry.thumb.style.height = entry.thumbLen + 'px'; entry.thumb.style.top = offset + 'px'; }
  }
  function updateScrollbars() { scrollers.forEach(updateScrollbar); }
  function attachScrollbars() {
    $$('.mc-panel > [role="tabpanel"]').forEach(el => attachScrollbar(el));
    attachScrollbar($('mc-camera-tree'));
    attachScrollbar($('mc-inspector'));
    attachScrollbar($('mc-dialog-body'));
    attachScrollbar($('mc-dock-favorites'), IS_MINI ? 'y' : 'x');
    if(IS_MINI){attachScrollbar($('mc-mini-panel').querySelector('.mc-dock-tools'),'x');}
    // Resize events are unreliable in-sim: poll the window size and content every 500 ms as well.
    let lastSize = '';
    const tick = () => {
      const size = document.body.clientWidth + 'x' + window.innerHeight;
      if (size !== lastSize) { lastSize = size; layoutMiniCategories(); if (SURFACE === 'demo') sizeMiniClearance(); }
      updateScrollbars();
    };
    window.addEventListener('resize', layoutMiniCategories);
    window.addEventListener('resize', tick);
    setInterval(tick, 500);
  }

  /* ------------- Wiring ------------- */
  function wire() {
    document.addEventListener('mousedown', e => { const shade=$('mc-dialog-shade'); if(shade.classList.contains('mc-context-shade')&&!shade.hidden&&!e.target.closest('.mc-dialog')&&e.button===0)closeDialog(); },true);
    buildAxes();
    buildEffects();

    $$('[data-page][role="tab"]').forEach(control => control.addEventListener('click', () => setPage(control.getAttribute('data-page'))));
    $$('[data-tab]').forEach(control => control.addEventListener('click', () => setTab(control.getAttribute('data-tab'))));
    $$('[data-precision]').forEach(control => control.addEventListener('click', () => setPrecision(control.getAttribute('data-precision'), true)));
    $('mc-launch').addEventListener('click', () => openEditor());
    $('mc-expand').addEventListener('click', () => { if (IS_MINI) requestMainEditor(); else openEditor('cameras'); });
    $('mc-mouse-look').addEventListener('click', startMouseLook);
    $('mc-editor-mouse-look').addEventListener('click', startMouseLook);
    // Suppress native right-drag inside our controls, independently for each panel instance.
    document.addEventListener('mouseover', e => {
      const target = e.target && e.target.closest ? e.target : null;
      const over = Boolean(target && target.closest('.mc-panel, #mc-mini-panel, .mc-dialog'));
      reportPanelPointer(over);
    }, true);
    document.addEventListener('mouseleave', e => { if (e.target === document || e.target === document.documentElement) reportPanelPointer(false); }, true);
    document.addEventListener('mouseout', e => { if (!e.relatedTarget) reportPanelPointer(false); }, true);
    window.addEventListener('blur', window.fscsClearPanelPointer);
    window.addEventListener('pagehide', window.fscsClearPanelPointer);
    $('mc-compact').addEventListener('click', enterFlightMode);
    $('mc-hide').addEventListener('click', closeEditor);
    $('mc-profile-picker').addEventListener('click', () => setPage('profiles'));
    $('mc-browser-toggle').addEventListener('click', () => { const open = root.dataset.browserOpen !== 'true'; root.dataset.browserOpen = String(open); $('mc-browser-toggle').setAttribute('aria-expanded', String(open)); });
    $('mc-search').addEventListener('input', renderTree);
    $('mc-toggle-bindings').addEventListener('click', () => {
      const groups=$$('.mc-binding-section',$('mc-bindings-list'));
      const expand = !groups.length || !groups.every(g=>g.open);
      S.bindingExpanded = Array(bindingSections().length+1).fill(expand);
      $('mc-binding-search').value = ''; $('mc-binding-status').value = 'all'; $('mc-binding-kind').value = 'all';
      renderBindings();
    });
    $('mc-toggle-categories').addEventListener('click', () => {
      const groups = P() ? P().categories.filter(g => g.cameraIds.length) : [];
      const expand = !groups.every(g => S.expanded[g.id]);
      $('mc-search').value = '';
      groups.forEach(g => { S.expanded[g.id] = expand; });
      renderTree();
    });
    ['mc-binding-search','mc-binding-status','mc-binding-kind'].forEach(id => $(id).addEventListener(id === 'mc-binding-search' ? 'input' : 'change', renderBindings));
    $('mc-favorite').addEventListener('click', () => { const v = current(); if (v) setFavorite(v); });
    $('mc-camera-menu').addEventListener('click', cameraActions);
    $('mc-camera-name').addEventListener('change', () => {
      const view = current(), input = $('mc-camera-name');
      if (!view || readOnly()) return;
      const name = input.value.trim();
      if (!name) { input.value = view.name; announce('Enter a camera name.', 'warn'); return; }
      view.name = name; saveCamera(view); renderTree(); renderDetails(); renderDock();
    });
    $('mc-camera-name').addEventListener('keydown', e => {
      if (e.key === 'Enter') { e.preventDefault(); e.target.blur(); }
      if (e.key === 'Escape') { e.preventDefault(); e.stopPropagation(); e.target.value = current().name; e.target.blur(); }
    });
    $('mc-camera-icon').addEventListener('click', () => chooseEntityIcon(current(), false));
    $('mc-camera-color').addEventListener('click', () => chooseEntityColor(current(), false));
    $('mc-category-default').addEventListener('click', () => setCategoryDefault(current()));
    $('mc-camera-binding').addEventListener('click', () => assignCameraBinding(current()));
    $('mc-view-in-sim').addEventListener('click', () => { const v = current(); if (v) activateCamera(v.id); });
    $('mc-recenter').addEventListener('click', () => command('RecenterLook', {}));
    $('mc-add').addEventListener('click', () => actionDialog('Add to your library', [['New camera', 'camera', addCamera], ['New category', 'folder-plus', addCategory], ['External motion views', 'view-orbit', addExternalMotionViews], ['Create airport cameras', 'map-pin', airportDialog]]));
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
    $('mc-capture-view').addEventListener('click', () => captureView(false));
    $('mc-save-current-view').addEventListener('click', () => captureView(false));
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
    $('mc-view-type').addEventListener('click', () => {
      const view=current();if(!view||readOnly()||view.airportIcao)return;
      const body=openDialog('Camera view type');
      [['aircraftCockpit','Cockpit'],['aircraftExterior','Aircraft exterior'],['orbit','Orbit'],['flyby','Fly-by']].forEach(([mode,label])=>{const b=button(label,()=>{view.mode=mode;if(mode==='orbit'||mode==='flyby'){view.requiredCapabilities=[];view.interaction=false;}saveCamera(view);closeDialog();render();},'mc-secondary');b.setAttribute('aria-pressed',String(view.mode===mode));body.appendChild(b);});
      body.appendChild(make('p','Set orbit and fly-by parameters on the Motion tab. Create airport views from the library + menu.','mc-note'));
    });
    $('mc-view-id').addEventListener('change', e => {
      const view = current();
      if (!view) return;
      const value = Number(e.target.value);
      if(!Number.isInteger(value)||value<0||value>2147483647){e.target.value=String(view.viewId||0);announce("Use 0 for unassigned, or a positive whole View ID.","warn");return;}
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
    $$('[data-value]', $('mc-mouse-release')).forEach(b => b.addEventListener('click', () => { const view = current(); if (!view || readOnly()) return; view.mouseLookOnRelease = b.dataset.value; saveCamera(view); renderDetails(); }));
    [['mc-retain-look', v => (v ? 'retain' : 'resetOnRecall'), 'temporaryLookPolicy'], ['mc-level', v => v, 'levelHorizon'], ['mc-interaction', v => v, 'interaction']].forEach(entry => $(entry[0]).addEventListener('change', e => {
      const view = current();
      if (!view) return;
      view[entry[2]] = entry[1](e.target.checked);
      saveCamera(view);
    }));
    ['shortcuts', 'contrast', 'backups', 'remember'].forEach(key => $('mc-setting-' + key).addEventListener('change', e => { S.working.settings.ui[key] = e.target.checked; settingChanged(); }));
    $('mc-setting-precision').addEventListener('change', e => { S.working.settings.ui.precision = e.target.value; setPrecision(e.target.value); settingChanged(); });
    $$('[data-value]', $('mc-setting-theme')).forEach(b => b.addEventListener('click', () => { S.working.settings.ui.theme = b.dataset.value; settingChanged(); }));
    $('mc-setting-reduced-motion').addEventListener('change', e => { S.working.settings.ui.reducedMotion = e.target.checked; settingChanged(); });
    $('mc-setting-mini-enabled').addEventListener('change', e => { S.working.settings.ui.miniEnabled = e.target.checked; settingChanged(); });
    $('mc-setting-mini-hover').addEventListener('change', e => { S.working.settings.ui.miniHover = e.target.checked; settingChanged(); });
    $('mc-setting-mini-opacity').addEventListener('input', e => { S.working.settings.ui.miniOpacity = Number(e.target.value) / 100; settingChanged(); });
    $$('[data-value]', $('mc-setting-name-display')).forEach(b => b.addEventListener('click', () => { S.working.settings.ui.nameDisplay = b.dataset.value; settingChanged(); }));
    $('mc-setting-unqualified').addEventListener('change', e => { S.working.settings.host.allowUnqualifiedAdapter = e.target.checked; settingChanged(); });
    $('mc-setting-mouse-enabled').addEventListener('change', e => { S.working.settings.host.mouseLookEnabled = e.target.checked; settingChanged(); });
    $('mc-setting-mouse-invert').addEventListener('change', e => { S.working.settings.host.mouseInvertY = e.target.checked; settingChanged(); });
    $('mc-mouse-defaults').addEventListener('click', () => { Object.assign(S.working.settings.host, {mouseSensitivity: 1, mouseSmoothingMs: 55, mouseLookEnabled: true, mouseInvertY: false}); settingChanged(); });
    [['mc-setting-airport-radius', 'airportRadiusNm', 5, 100], ['mc-setting-mouse-sensitivity', 'mouseSensitivity', 0.1, 4], ['mc-setting-mouse-smoothing', 'mouseSmoothingMs', 0, 250], ['mc-setting-look-speed', 'lookSpeedDegPerSec', 5, 360], ['mc-setting-move-speed', 'moveSpeedMetersPerSec', 0.05, 10]].forEach(entry => $(entry[0]).addEventListener('change', e => {
      const v = Number(e.target.value);
      if (e.target.value !== '' && isFinite(v)) S.working.settings.host[entry[1]] = Number(M.clamp(v, entry[2], entry[3]).toFixed(2));
      e.target.value = String(S.working.settings.host[entry[1]]);
      settingChanged();
    }));
    $('mc-qual-run').addEventListener('click', runAxisCheck);
    $('mc-diagnostics').addEventListener('click', () => bridge.request('GetDiagnostics', {}).then(ack => codeDialog('Diagnostics', 'Technical details for support. Nothing is sent anywhere.', (ack.result && ack.result.text) || reasonText(ack.reason), null, 'Select text')));
    if($('mc-airport-generator'))$('mc-airport-generator').addEventListener('click', airportDialog);
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
      if ((event.ctrlKey || event.metaKey) && !event.altKey && (event.key === 's' || event.key === 'S') && !dialogOpen() && bindingsFor('saveViews').some(b => b.input.kind==='key' && /^(VK_LCONTROL|VK_CONTROL)\+S$/.test(b.input.definition)) && !event.shiftKey) { event.preventDefault(); if(!event.repeat && (bridge.demo || isTextEntry(event.target)))saveAdjustedViews(); }
    });

    // Mini dock reveal: pointer, focus, tap and pin are independent reasons to stay open (design sec. 4.5).
    {
      const region = $('mc-mini-region');
      onPointer(region, 'pointerenter', event => { if (pointerType(event) === 'touch' || !miniHover() || !miniEnabled()) return; miniHovered = true; revealMini(); });
      onPointer(region, 'pointerleave', event => { if (pointerType(event) === 'touch') return; miniHovered = false; scheduleMiniHide(); });
      onPointer(document, 'pointermove', event => {
        if (pointerType(event) === 'touch' || !miniHover() || !miniEnabled()) return;
        // Coherent can retarget mouse events to the host instead of the transparent child.
        const bounds = region.getBoundingClientRect(), dock = $('mc-mini-panel').getBoundingClientRect();
        const inRect = r => event.clientX >= r.left && event.clientX <= r.right && event.clientY >= r.top && event.clientY <= r.bottom;
        const inside = region.contains(event.target) || inRect(bounds) || (root.dataset.miniOpen === 'true' && inRect(dock));
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

  window.FSCApp = { state: S, bridge, parseAirportXml };
  if (bridge.demo) window.FSCDemo = bridge.demo;
  window.FSCIcons.hydrateIcons(root);
  $$('button,summary,label').forEach(control => control.classList.add('cursor-interaction'));
  wire();
  installTooltips();
  attachScrollbars();
  setTab('position');
  setPage('cameras');
  render();
  if (SURFACE === 'demo') sizeMiniClearance();
  if (SURFACE === 'main') root.dataset.miniOpen = 'false';
  setTimeout(() => { if (!S.snapshot) { S.failedStart = true; render(); } }, 10000);
  bridge.start();
})();
