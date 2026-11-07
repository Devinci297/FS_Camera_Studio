// XP Camera Studio transport: a page opened with ?host=xplane talks to the plugin through xplane.fscs(frame) and
// receives FSCXPlaneReceive(frame). Run: node ui/tests/xplane-bridge.test.mjs
import assert from 'node:assert/strict';
import { createRequire } from 'node:module';

const require = createRequire(import.meta.url);
globalThis.document = { hidden: false };
globalThis.location = { search: '?surface=main&host=xplane' };
const sent = [], panelCalls = [];
require('../math.js');
require('../bridge.js');
const { create, encodeFrames, Assembler } = globalThis.FSCBridge;

let passed = 0;
async function test(name, fn) {
  try { await fn(); passed++; console.log('ok   ' + name); } catch (error) { console.error('FAIL ' + name); console.error(error); process.exitCode = 1; }
}
const wait = ms => new Promise(r => setTimeout(r, ms));

await test('the X-Plane host is picked over the demo module', () => {
  const bridge = create({ surface: 'main' });
  assert.equal(bridge.mode, 'xplane');
  assert.equal(bridge.demo, null);
});

await test('Hello waits for the xplane namespace, then goes out through xplane.fscs', async () => {
  const bridge = create({ surface: 'main' });
  bridge.start();
  await wait(20);
  assert.equal(sent.length, 0);  // no namespace yet: nothing sent, nothing thrown
  globalThis.xplane = {
    fscs: frame => { sent.push(frame); return Promise.resolve(true); },
    fscsPanel: arg => { panelCalls.push(arg); return Promise.resolve(true); },
  };
  bridge.send('RequestSnapshot', {});
  assert.equal(sent.length, 1);
  const envelope = new Assembler().push(sent[0]);
  assert.equal(envelope.action, 'RequestSnapshot');
  assert.equal(envelope.uiInstanceId, bridge.uiInstanceId);
  bridge.stop();
});

await test('module frames delivered by FSCXPlaneReceive reach the bridge, chunked ones too', async () => {
  globalThis.xplane = { fscs: frame => { sent.push(frame); return true; }, fscsPanel: arg => { panelCalls.push(arg); return true; } };
  const bridge = create({ surface: 'main' });
  const events = [];
  bridge.on('event', e => events.push(e));
  bridge.start();
  await wait(10);
  const hello = new Assembler().push(sent[sent.length - 1]);
  assert.equal(hello.action, 'Hello');
  assert.deepEqual(hello.payload, { surface: 'main' });

  const snapshot = { type: 'event', name: 'Snapshot', moduleBootId: 'boot-1', sessionEpoch: 3, payload: { big: 'x'.repeat(9000) } };
  encodeFrames(snapshot, 'module:1').forEach(frame => globalThis.FSCXPlaneReceive(frame));
  await wait(10);
  assert.equal(events.length, 1);
  assert.equal(events[0].name, 'Snapshot');
  assert.equal(bridge.context.moduleBootId, 'boot-1');
  assert.equal(bridge.context.sessionEpoch, 3);
  bridge.stop();
});

await test('opening and closing panels goes to the plugin', () => {
  panelCalls.length = 0;
  assert.equal(globalThis.fscsSetPanelOpen('mini', true), true);
  assert.equal(globalThis.fscsSetPanelOpen('main', false), true);
  assert.deepEqual(panelCalls, [{ surface: 'mini', open: true }, { surface: 'main', open: false }]);
});

console.log(`${passed} passed`);
process.exit(process.exitCode || 0);
