// The panel must never call the native CommBus while MSFS is tearing its view down (MSFS 2024 1.9.24 crashed in
// CoherentUIGT when a pagehide listener sent a frame). Run: node ui/tests/teardown-guard.test.mjs
import assert from 'node:assert/strict';
import { createRequire } from 'node:module';

const require = createRequire(import.meta.url);
const target = new EventTarget();
globalThis.addEventListener = target.addEventListener.bind(target);
globalThis.removeEventListener = target.removeEventListener.bind(target);
globalThis.dispatchEvent = target.dispatchEvent.bind(target);
globalThis.document = { hidden: false };
const sent = [];
globalThis.RegisterCommBusListener = onOpen => {
  setTimeout(onOpen, 0);
  return { on() {}, callWasm(name, frame) { sent.push(frame); } };
};
require('../math.js');
require('../bridge.js');
const wait = ms => new Promise(r => setTimeout(r, ms));

const bridge = globalThis.FSCBridge.create({ surface: 'mini' });
if (bridge.start) bridge.start();
await wait(50);
assert.ok(sent.length > 0, 'the bridge talks to the module while the view is alive');

globalThis.dispatchEvent(new Event('pagehide'));
const before = sent.length;
bridge.send('PointerOverPanel', { over: false, cancelDrag: true });
await wait(1200);
assert.equal(sent.length, before, 'no CommBus frame may be sent after pagehide');
console.log('ok   no CommBus sends after pagehide');
process.exit(0);
