// Pose math tests for FS Camera Studio. Run: node ui/tests/math.test.mjs
import assert from 'node:assert/strict';
import { createRequire } from 'node:module';

const require = createRequire(import.meta.url);
const M = require('../math.js');

let passed = 0;
function test(name, fn) {
  try {
    fn();
    passed++;
    console.log('ok   ' + name);
  } catch (error) {
    console.error('FAIL ' + name);
    console.error(error);
    process.exitCode = 1;
  }
}

const near = (actual, expected, eps = 1e-9, label = '') =>
  assert.ok(Math.abs(actual - expected) <= eps, `${label} expected ${expected}, got ${actual}`);
const nearVec = (actual, expected, eps = 1e-9, label = '') =>
  expected.forEach((value, i) => near(actual[i], value, eps, `${label}[${i}]`));
const sameRotation = (a, b, eps = 1e-9) => {
  const dot = Math.abs(a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3]);
  near(dot, 1, eps, 'quaternion dot');
};

test('identity angles give the identity quaternion', () => {
  nearVec(M.quatFromHpb(0, 0, 0), [0, 0, 0, 1]);
  const hpb = M.hpbFromQuat([0, 0, 0, 1]);
  near(hpb.heading, 0); near(hpb.pitch, 0); near(hpb.bank, 0);
});

test('heading 90 looks right: camera +x maps to anchor +y', () => {
  const q = M.quatFromHpb(90, 0, 0);
  nearVec(M.rotateVector(q, [1, 0, 0]), [0, 1, 0], 1e-12, 'forward');
  nearVec(q, [0, 0, Math.SQRT1_2, Math.SQRT1_2], 1e-12, 'q');
});

test('heading -90 looks left: camera +x maps to anchor -y', () => {
  nearVec(M.rotateVector(M.quatFromHpb(-90, 0, 0), [1, 0, 0]), [0, -1, 0], 1e-12);
});

test('pitch 30 looks up: forward gains a negative z (up) component', () => {
  const f = M.rotateVector(M.quatFromHpb(0, 30, 0), [1, 0, 0]);
  near(f[0], Math.cos(M.degToRad(30)), 1e-12, 'x');
  near(f[1], 0, 1e-12, 'y');
  near(f[2], -0.5, 1e-12, 'z');
  assert.ok(f[2] < 0);
});

test('bank 30 rolls right: camera right axis (+y) dips toward +z (down)', () => {
  const r = M.rotateVector(M.quatFromHpb(0, 0, 30), [0, 1, 0]);
  near(r[1], Math.cos(M.degToRad(30)), 1e-12);
  near(r[2], 0.5, 1e-12);
});

test('composition order is Rz(heading) · Ry(pitch) · Rx(bank)', () => {
  const qh = M.quatFromHpb(40, 0, 0), qp = M.quatFromHpb(0, 25, 0), qb = M.quatFromHpb(0, 0, -15);
  sameRotation(M.quatFromHpb(40, 25, -15), M.quatMultiply(M.quatMultiply(qh, qp), qb), 1e-12);
});

test('heading then pitch: heading 90 + pitch 30 looks right and up', () => {
  const f = M.rotateVector(M.quatFromHpb(90, 30, 0), [1, 0, 0]);
  nearVec(f, [0, Math.cos(M.degToRad(30)), -0.5], 1e-12);
});

test('round trip angles → quaternion → angles over a grid', () => {
  for (let h = -175; h <= 180; h += 35) {
    for (let p = -85; p <= 85; p += 17) {
      for (let b = -170; b <= 180; b += 50) {
        const hpb = M.hpbFromQuat(M.quatFromHpb(h, p, b));
        near(hpb.heading, M.wrapDeg(h), 1e-8, `h(${h},${p},${b})`);
        near(hpb.pitch, p, 1e-8, `p(${h},${p},${b})`);
        near(hpb.bank, M.wrapDeg(b), 1e-8, `b(${h},${p},${b})`);
      }
    }
  }
});

test('round trip quaternion → angles → quaternion keeps the rotation', () => {
  const samples = [[0.1, -0.2, 0.3, 0.9], [0, 0, 0, 1], [0.5, 0.5, 0.5, 0.5], [-0.3, 0.1, 0.7, 0.2]];
  for (const raw of samples) {
    const q = M.quatNormalize(raw);
    const { heading, pitch, bank } = M.hpbFromQuat(q);
    if (Math.abs(pitch) >= M.PITCH_LIMIT_DEG) continue; // clamped views are not invertible by design
    sameRotation(M.quatFromHpb(heading, pitch, bank), q, 1e-9);
  }
});

test('pitch is clamped to ±89 degrees', () => {
  near(M.hpbFromQuat(M.quatFromHpb(0, 120, 0)).pitch, 89, 1e-9);
  near(M.hpbFromQuat(M.quatFromHpb(0, -95, 0)).pitch, -89, 1e-9);
  near(M.hpbFromQuat(M.quatFromHpb(0, 90, 0)).pitch, 89, 1e-9);
});

test('produced quaternions are unit length', () => {
  const q = M.quatFromHpb(12.3, -45.6, 78.9);
  near(Math.hypot(...q), 1, 1e-12);
});

test('UI axis mapping: X = right (FRD y), Y = up (-FRD z), Z = forward (FRD x)', () => {
  assert.deepEqual(M.uiFromFrd([3, 2, -1]), { x: 2, y: 1, z: 3 });
  assert.deepEqual(M.frdFromUi({ x: 2, y: 1, z: 3 }), [3, 2, -1]);
  const frd = [0.183, -0.245, -0.369];
  assert.deepEqual(M.frdFromUi(M.uiFromFrd(frd)), frd, 'round trip is exact');
  assert.deepEqual(M.UI_AXIS_TO_FRD.x, { index: 1, sign: 1 });
  assert.deepEqual(M.UI_AXIS_TO_FRD.y, { index: 2, sign: -1 });
  assert.deepEqual(M.UI_AXIS_TO_FRD.z, { index: 0, sign: 1 });
});

test('FOV conversions', () => {
  near(M.degToRad(180), Math.PI, 1e-15);
  near(M.radToDeg(1.0821), 62.0, 0.01);
  near(M.radToDeg(M.degToRad(73.25)), 73.25, 1e-12);
});

test('wrapDeg keeps angles in (-180, 180]', () => {
  near(M.wrapDeg(190), -170); near(M.wrapDeg(-190), 170); near(M.wrapDeg(180), 180); near(M.wrapDeg(-180), 180); near(M.wrapDeg(540), 180);
});

test('easing curves (v2: Smooth = smoothstep3, Cinematic = smoothstep5)', () => {
  near(M.ease('smoothstep3', 0), 0); near(M.ease('smoothstep3', 1), 1); near(M.ease('smoothstep3', 0.5), 0.5);
  near(M.ease('smoothstep3', 0.25), 3 * 0.25 ** 2 - 2 * 0.25 ** 3, 1e-15);
  near(M.ease('smoothstep5', 0.5), 0.5); near(M.ease('smoothstep5', 1), 1);
  near(M.ease('smoothstep5', 0.3), 6 * 0.3 ** 5 - 15 * 0.3 ** 4 + 10 * 0.3 ** 3, 1e-15);
  near(M.ease('smoothstep3', -1), 0); near(M.ease('smoothstep5', 2), 1);
  near(M.ease('cut', 0.99), 0); near(M.ease('cut', 1), 1);
});

test('authoring axes: FRD = [Z, X, -Y] (design §4.2)', () => {
  assert.deepEqual(M.frdFromUi({ x: 1, y: 2, z: 3 }), [3, 1, -2]);
});

test('uuid returns lower-case RFC 4122 strings', () => {
  const id = M.uuid();
  assert.match(id, /^[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$/);
  assert.notEqual(id, M.uuid());
});

console.log(`\n${passed} passed${process.exitCode ? ', some FAILED' : ''}`);
