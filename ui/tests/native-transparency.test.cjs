const assert = require('node:assert/strict');
const fs = require('node:fs');
const vm = require('node:vm');
const source = fs.readFileSync(process.argv[2] || 'ui/msfs-panel.js', 'utf8');
function setup(id, available = true) {
  const events = {}, ticks = [], calls = [];
  const panel = id ? { id, active: true, visible: true, addEventListener: (k, f) => { (events[k] ||= []).push(f); } } : null;
  const context = { console, document: {
    readyState: 'complete', querySelector: () => panel, getElementById: () => null,
    addEventListener() {},
  }, setTimeout() {}, setInterval(f) { ticks.push(f); }, clearInterval() {}, addEventListener() {} };
  context.window = context;
  if (available) context.SetBlurredBackground = value => calls.push(value);
  vm.runInNewContext(source, context);
  return { events, ticks, calls, context };
}
for (const id of ['FSCSMain', 'FSCSMini']) {
  const s = setup(id);
  s.ticks[0](); s.ticks[0]();
  assert.deepEqual(s.calls, [false], 'Disable compositor blur once per panel view');
  assert.equal(s.events.panelActive.length, 1);
  s.events.panelActive[0]();
  assert.deepEqual(s.calls, [false, false], 'Reopening restores clear compositing');
  const late = setup(id, false);
  late.ticks[0]();
  late.context.SetBlurredBackground = value => late.calls.push(value);
  late.ticks[0]();
  assert.deepEqual(late.calls, [false], 'Late simulator API remains retryable');
}
assert.equal(setup(null).ticks.length, 0, 'Normal browser is unaffected');
console.log('Native clear-background routing passed for main/mini, reopen, late API and browser.');
