import assert from 'node:assert/strict';
import fs from 'node:fs';
import vm from 'node:vm';

const source = fs.readFileSync(process.env.FSCS_PANEL_SOURCE || new URL('../msfs-panel.js', import.meta.url), 'utf8');
class Node {
  constructor(id) {
    this.id = id; this.nodeType = 1; this.listeners = {}; this.dataset = {};
    const classes = new Set();
    this.classList = { add: c => classes.add(c), remove: c => classes.delete(c), contains: c => classes.has(c) };
  }
  addEventListener(type, fn) { (this.listeners[type] ||= new Set()).add(fn); }
  removeEventListener(type, fn) { this.listeners[type]?.delete(fn); }
  emit(type, event = {}) { this.listeners[type]?.forEach(fn => fn(event)); }
  getBoundingClientRect() { return this.rect; }
}
function harness(surface) {
  const panel = new Node(surface === 'main' ? 'FSCSMain' : 'FSCSMini');
  panel.active = panel.visible = true; panel.panelID = 'mini'; panel.hoverReleases = 0;
  panel.m_toolbar_listener = { pushPanelMouseOut: () => panel.hoverReleases++ };
  const editor = new Node('mc-sim'); editor.parentNode = panel;
  const header = new Node('mc-drag-header');
  const strip = new Node('mc-mini-region'); strip.rect = { left: 0, right: 1920, top: 1044, bottom: 1080 };
  const dock = new Node('mc-mini-panel'); dock.rect = { left: 480, right: 1440, top: 970, bottom: 1044 };
  const dialog = new Node('mc-dialog-shade'); dialog.hidden = true; dialog.rect = { left: 0, right: 1920, top: 0, bottom: 1080 };
  const nodes = Object.fromEntries([editor,header,strip,dock,dialog].map(n => [n.id,n]));
  const document = new Node('document'); document.readyState = 'complete'; document.body = editor;
  document.getElementById = id => nodes[id]; document.querySelector = () => panel; document.querySelectorAll = () => [panel];
  const calls = []; const timers = []; const observers = [];
  const context = { document, console, innerWidth: 1920, innerHeight: 1080, Coherent: { call: (...args) => calls.push(args), trigger: (...args) => calls.push(args) },
    MutationObserver: class { constructor(fn) { this.fn = fn; } observe(target) { observers.push({ target, fn: this.fn }); } }, ResizeObserver: class { observe() {} },
    requestAnimationFrame: fn => fn(), setTimeout: fn => timers.push(fn), setInterval: fn => timers.push(fn), clearInterval() {}, Event };
  context.window = context; context.addEventListener = () => {}; context.removeEventListener = () => {};
  vm.createContext(context); vm.runInContext(source, context);
  return { observers, panel, editor, header, strip, dock, dialog, context, calls, run: () => timers.forEach(fn => fn()) };
}

const main = harness('main'); let drags = 0;
main.panel.dragDropHandler = { startDrag: e => { if (!e.target.classList.contains('nodrag')) drags++; }, checkCanDrag() {}, leaveDrag() {} };
main.run(); main.run(); // Repeated host callbacks must not duplicate input handlers.
const background = new Node('blank'); background.closest = () => null;
main.header.emit('mousedown', { target: background, button: 0 });
assert.equal(drags, 1);
const button = new Node('button'); button.closest = () => button;
main.editor.emit('mousedown', { target: button, button: 0 });
assert.equal(drags, 1, 'controls stay interactive');
main.editor.emit('mousedown', { target: background, button: 2 });
assert.equal(drags, 1, 'right clicks do not start window drags');
main.editor.emit('mousedown', { target: background, button: 0 });
assert.equal(drags, 1, 'body cannot drag the main window');
main.context.fscsSetPanelOpen('main', false);
assert.deepEqual(main.calls.at(-1), ['TOOLBAR_BUTTON_TOGGLE','PANEL_FSCS_CAMERA_STUDIO',false]);
main.context.fscsSetPanelOpen('main', true);
assert.equal(main.calls.at(-1)[2], true);

const mini = harness('mini'); mini.run();
assert.deepEqual(mini.calls.at(-1), ['UPDATE_PANEL_RECT','mini',0,1920,1044,1080]);
mini.editor.dataset.miniOpen = 'true'; mini.context.fscsUpdateMiniRect();
assert.equal(mini.calls.at(-1)[4], 970, 'revealed dock remains inside the hit area');
mini.editor.dataset.miniOpen = 'false'; mini.context.fscsUpdateMiniRect();
assert.equal(mini.calls.at(-1)[4], 1044, 'closing returns cockpit clicks above the strip');
mini.dialog.hidden = false; mini.context.fscsUpdateMiniRect();
assert.equal(mini.calls.at(-1)[4], 0, 'an open modal stays usable');
mini.editor.dataset.mouseLook = 'true'; mini.context.fscsUpdateMiniRect();
assert.deepEqual(mini.calls.at(-1), ['UPDATE_PANEL_RECT','mini',0,1920,process.env.FSCS_LEGACY_PANEL ? 129.6 : 0,1080], 'host mouse-look bounds match its supported mode');
mini.dialog.hidden = true; mini.context.fscsUpdateMiniRect();
assert.deepEqual(mini.calls.at(-1), ['UPDATE_PANEL_RECT','mini',0,1920,process.env.FSCS_LEGACY_PANEL ? 129.6 : 1044,1080], 'host mouse-look bounds remain explicit');
mini.editor.dataset.mouseLook = 'false';
mini.editor.dataset.miniMode = 'off'; mini.context.fscsUpdateMiniRect();
assert.equal(mini.panel.hoverReleases,1,'off clears stock toolbar hover even without mouseleave');
mini.context.fscsUpdateMiniRect();assert.equal(mini.panel.hoverReleases,1,'hover release is not flooded every frame');
assert.deepEqual(mini.calls.at(-1), ['UPDATE_PANEL_RECT','mini',0,0,0,0]);
mini.editor.dataset.miniMode = 'hover'; mini.dialog.hidden = true;
mini.panel.visible = false; mini.context.fscsUpdateMiniRect();
assert.deepEqual(mini.calls.at(-1), ['UPDATE_PANEL_RECT','mini',0,0,0,0]);
mini.panel.visible = true;
mini.observers.find(o => o.target === mini.panel).fn();
assert.deepEqual(mini.calls.at(-1), ['UPDATE_PANEL_RECT','mini',0,1920,1044,1080], 'visibility-only startup restores the hover area');
const count = mini.calls.length; mini.run(); mini.run();
assert.equal(mini.calls.length, count, 'unchanged bounds are not repeatedly sent');
mini.panel.active = false; mini.panel.emit('panelInactive');
assert.deepEqual(mini.calls.at(-1), ['UPDATE_PANEL_RECT','mini',0,0,0,0]);
const relays=[];
main.context.FSCApp={state:{snapshot:{}},bridge:{send:(...args)=>relays.push(args)}};
main.run();main.run();
assert.equal(relays.length,1,'visible editor sends capture release once');
assert.equal(relays[0][1].op,'stopMouseLook');
main.panel.visible=false;main.run();main.panel.visible=true;main.run();
assert.equal(relays.length,2,'reopening editor releases mini capture');
mini.context.innerWidth=mini.context.innerHeight=67;mini.panel.active=true;mini.editor.dataset.mouseLook='true';mini.context.fscsUpdateMiniRect();
assert.deepEqual(mini.calls.at(-1), ['UPDATE_PANEL_RECT','mini',0,67,67,67], 'loading viewport never receives inverted bounds');
console.log('5 panel host regressions passed');
