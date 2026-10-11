// Bridge tests (chunking + demo module message flow). Run: node ui/tests/bridge.test.mjs
import assert from 'node:assert/strict';
import { createRequire } from 'node:module';

const require = createRequire(import.meta.url);
globalThis.document = { hidden: false };
require('../math.js');
require('../bridge.js');
const { create, encodeFrames, Assembler } = globalThis.FSCBridge;

let passed = 0;
async function test(name, fn) {
  try { await fn(); passed++; console.log('ok   ' + name); } catch (error) { console.error('FAIL ' + name); console.error(error); process.exitCode = 1; }
}
const wait = ms => new Promise(r => setTimeout(r, ms));
function nextEvent(bridge, name, timeout = 2000) {
  return new Promise((resolve, reject) => {
    const timer = setTimeout(() => reject(new Error('timeout waiting for ' + name)), timeout);
    bridge.on('event', e => { if (e.name === name) { clearTimeout(timer); resolve(e); } });
  });
}

await test('small messages are sent as one frame', () => {
  const frames = encodeFrames({ a: 1 }, 'x');
  assert.equal(frames.length, 1);
  assert.deepEqual(new Assembler().push(frames[0]), { a: 1 });
});

await test('large messages are chunked under 4000 chars and reassembled in any order', () => {
  const big = { text: '"quoted" '.repeat(3000), list: Array.from({ length: 500 }, (_, i) => ({ i })) };
  const frames = encodeFrames(big, 'm1');
  assert.ok(frames.length > 1);
  frames.forEach(f => assert.ok(f.length <= 4000, 'frame length ' + f.length));
  const assembler = new Assembler();
  const shuffled = frames.slice().reverse();
  let result = null;
  shuffled.forEach(f => { const r = assembler.push(f); if (r) result = r; });
  assert.deepEqual(result, big);
});

await test('demo: Hello → Snapshot, UpdateDraft → DraftChanged, Commit → Pending then CommitCompleted', async () => {
  const bridge = create({ surface: 'main' });
  const snap = nextEvent(bridge, 'Snapshot');
  bridge.start();
  const { payload } = await snap;
  assert.equal(payload.session, 'Ready');
  assert.ok(payload.working.profile.cameras.length > 5);
  assert.equal(payload.working.dirty.profile, false);
  Object.assign(bridge.context, { moduleBootId: payload.moduleBootId, sessionEpoch: payload.sessionEpoch, draftRevision: payload.working.draftRevision });

  const cam = JSON.parse(JSON.stringify(payload.working.profile.cameras[0]));
  cam.name = 'Edited';
  const changed = nextEvent(bridge, 'DraftChanged');
  const ack = await bridge.request('UpdateDraft', { ops: [{ op: 'setCamera', camera: cam }] });
  assert.equal(ack.status, 'Completed');
  const dc = await changed;
  assert.equal(dc.payload.dirty.profile, true);
  assert.equal(dc.payload.origin, bridge.uiInstanceId);

  const stale = await bridge.request('UpdateDraft', { ops: [] });
  assert.equal(stale.reason, 'staleDraft', 'old draftRevision must be rejected');
  bridge.context.draftRevision = dc.payload.draftRevision;

  const done = nextEvent(bridge, 'CommitCompleted');
  const pending = await bridge.request('CommitChanges', { backup: true });
  assert.equal(pending.status, 'Pending');
  const completed = await done;
  assert.equal(completed.payload.status, 'Committed');
  assert.equal(completed.payload.requestId, pending.requestId);
  bridge.stop();
});

await test('demo: stale boot id is rejected, Release always processed', async () => {
  const bridge = create({ surface: 'mini' });
  const snap = nextEvent(bridge, 'Snapshot');
  bridge.start();
  const { payload } = await snap;
  Object.assign(bridge.context, { moduleBootId: 'old-boot', sessionEpoch: payload.sessionEpoch, draftRevision: payload.working.draftRevision });
  assert.equal((await bridge.request('SelectCamera', { cameraId: payload.working.profile.cameras[1].id })).reason, 'staleBoot');
  assert.notEqual((await bridge.request('ReleaseCamera', {})).reason, 'staleBoot');
  bridge.stop();
});

await test('demo: activation goes Observing → Acquiring → Active', async () => {
  const bridge = create({ surface: 'main' });
  const snap = nextEvent(bridge, 'Snapshot');
  bridge.start();
  const { payload } = await snap;
  Object.assign(bridge.context, { moduleBootId: payload.moduleBootId, sessionEpoch: payload.sessionEpoch, draftRevision: payload.working.draftRevision });
  const states = [];
  bridge.on('event', e => { if (e.name === 'OwnershipChanged') states.push(e.payload.state); });
  const ack = await bridge.request('ActivateCamera', { cameraId: payload.working.profile.defaultCameraId });
  assert.equal(ack.status, 'Accepted');
  await wait(700);
  assert.deepEqual(states.slice(0, 2), ['Acquiring', 'Active']);
  bridge.stop();
});

await test('demo: axis check start → probe → record marks the build qualified', async () => {
  const bridge = create({ surface: 'main' });
  const snap = nextEvent(bridge, 'Snapshot');
  bridge.start();
  const { payload } = await snap;
  Object.assign(bridge.context, { moduleBootId: payload.moduleBootId, sessionEpoch: payload.sessionEpoch, draftRevision: payload.working.draftRevision });
  bridge.demo.setQualified(false);
  await wait(50);
  const start = await bridge.request('QualificationStart', {});
  assert.equal(start.status, 'Completed');
  assert.deepEqual(start.result.axes, ['forward', 'right', 'up', 'pitch', 'heading', 'bank']);
  assert.equal((await bridge.request('ActivateCamera', { cameraId: payload.working.profile.defaultCameraId })).status, 'Accepted', 'live control allowed while the check runs');
  await wait(700);
  const probe = await bridge.request('QualificationProbe', { axis: 'heading' });
  assert.equal(probe.status, 'Completed');
  assert.match(probe.result.expect, /right/);
  const answers = Object.fromEntries(start.result.axes.map(a => [a, a === 'bank' ? 'reversed' : 'correct']));
  const caps = new Promise(resolve => bridge.on('event', e => { if (e.name === 'CapabilitiesChanged' && e.payload.adapterQualified) resolve(e.payload); }));
  const done = nextEvent(bridge, 'CommitCompleted');
  const record = await bridge.request('QualificationRecord', { answers, recall: 'pass', notes: '' });
  assert.equal(record.status, 'Pending');
  const completed = await done;
  assert.equal(completed.payload.kind, 'qualification');
  assert.equal((await caps).canActivate, true);
  assert.equal((await bridge.request('QualificationEnd', {})).status, 'Completed');
  bridge.stop();
});

console.log(`\n${passed} passed${process.exitCode ? ', some FAILED' : ''}`);
process.exit(process.exitCode || 0);
