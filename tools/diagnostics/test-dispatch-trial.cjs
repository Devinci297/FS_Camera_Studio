const fs = require('node:fs');
const assert = require('node:assert/strict');
(async () => {
  let calls = 0, closes = 0, failNext = false;
  const { instance } = await WebAssembly.instantiate(fs.readFileSync('build/wasm/mouse-dispatch-trial/dispatch-test.wasm'), {
    env: {
      __real_SimConnect_CallDispatch() { ++calls; if (failNext) { failNext = false; return -1; } return 0; },
      __real_SimConnect_Close() { ++closes; return 0; },
    },
  });
  const call = instance.exports.__wrap_SimConnect_CallDispatch;
  const close = instance.exports.__wrap_SimConnect_Close;
  for (let i = 0; i < 216000; ++i) assert.equal(call(1n, 0, 12), 0); // one hour at 60 FPS
  assert.equal(calls, 1);
  close(1n);
  assert.equal(closes, 1);
  assert.equal(call(1n, 0, 12), 0); // reused handle after reconnect must register again
  assert.equal(calls, 2);
  assert.equal(call(1n, 0, 13), 0); // changed callback context is an explicit new registration
  assert.equal(calls, 3);
  close(1n);
  failNext = true;
  assert.equal(call(2n, 0, 12), -1);
  assert.equal(call(2n, 0, 12), 0); // failure cannot suppress a retry
  assert.equal(calls, 5);
  console.log('PASS: 216,000 frame calls register once; reconnect, context change and failure retry work.');
})().catch(error => { console.error(error); process.exit(1); });
