/*
 * FS Camera Studio - pose math shared by the editor UI and its tests.
 *
 * Conventions (doc/protocol.md sec. 1):
 *  - Canonical frame FRD: x forward, y right, z down. A camera looks along local +x, its up is -z.
 *  - Orientation is a unit quaternion [x, y, z, w] rotating camera coordinates into the anchor frame,
 *    q = Rz(heading) - Ry(pitch) - Rx(bank). heading > 0 turns right, pitch > 0 looks up, bank > 0 rolls right.
 *  - UI axes: X = right (FRD y), Y = up (-FRD z), Z = forward (FRD x). Meters.
 *  - Projection is stored in radians; the UI shows degrees.
 *
 * Classic script (ES2020, no build step): exposes FSCMath on the global object and module.exports under Node.
 */
(function (root) {
  'use strict';

  const DEG = Math.PI / 180;
  const PITCH_LIMIT_DEG = 89;

  const degToRad = deg => deg * DEG;
  const radToDeg = rad => rad / DEG;
  const clamp = (value, min, max) => Math.min(max, Math.max(min, value));

  /** Wrap an angle in degrees into (-180, 180]. */
  function wrapDeg(deg) {
    let a = ((deg + 180) % 360 + 360) % 360 - 180;
    if (a === -180) a = 180;
    return a;
  }

  function quatNormalize(q) {
    const n = Math.hypot(q[0], q[1], q[2], q[3]);
    if (!(n > 0) || !Number.isFinite(n)) return [0, 0, 0, 1];
    return [q[0] / n, q[1] / n, q[2] / n, q[3] / n];
  }

  /**
   * Build the camera-to-anchor quaternion from aerospace angles in degrees.
   * Pitch is clamped to +/-89 deg (the editor never produces a vertical view).
   */
  function quatFromHpb(headingDeg, pitchDeg, bankDeg) {
    const h = degToRad(headingDeg) / 2;
    const p = degToRad(clamp(pitchDeg, -PITCH_LIMIT_DEG, PITCH_LIMIT_DEG)) / 2;
    const b = degToRad(bankDeg) / 2;
    const ch = Math.cos(h), sh = Math.sin(h);
    const cp = Math.cos(p), sp = Math.sin(p);
    const cb = Math.cos(b), sb = Math.sin(b);
    // q = qz(h) * qy(p) * qx(b)
    const x = sb * cp * ch - cb * sp * sh;
    const y = cb * sp * ch + sb * cp * sh;
    const z = cb * cp * sh - sb * sp * ch;
    const w = cb * cp * ch + sb * sp * sh;
    return quatNormalize([x, y, z, w]);
  }

  /**
   * Decompose a camera-to-anchor quaternion into { heading, pitch, bank } degrees.
   * Pitch is reported within +/-89 deg to match the editor's limits.
   */
  function hpbFromQuat(q) {
    const [x, y, z, w] = quatNormalize(q);
    const sinp = clamp(2 * (w * y - z * x), -1, 1);
    const pitch = radToDeg(Math.asin(sinp));
    let heading, bank;
    if (Math.abs(sinp) > 0.999999) {
      // Gimbal lock: fold everything into heading.
      heading = radToDeg(2 * Math.atan2(z, w));
      bank = 0;
    } else {
      heading = radToDeg(Math.atan2(2 * (w * z + x * y), 1 - 2 * (y * y + z * z)));
      bank = radToDeg(Math.atan2(2 * (w * x + y * z), 1 - 2 * (x * x + y * y)));
    }
    return {
      heading: wrapDeg(heading),
      pitch: clamp(pitch, -PITCH_LIMIT_DEG, PITCH_LIMIT_DEG),
      bank: wrapDeg(bank),
    };
  }

  function quatMultiply(a, b) {
    const [ax, ay, az, aw] = a, [bx, by, bz, bw] = b;
    return [
      aw * bx + ax * bw + ay * bz - az * by,
      aw * by - ax * bz + ay * bw + az * bx,
      aw * bz + ax * by - ay * bx + az * bw,
      aw * bw - ax * bx - ay * by - az * bz,
    ];
  }

  /** Rotate vector v (camera coordinates) into the anchor frame with q. */
  function rotateVector(q, v) {
    const [x, y, z, w] = q;
    const [vx, vy, vz] = v;
    // t = 2 * cross(q.xyz, v)
    const tx = 2 * (y * vz - z * vy);
    const ty = 2 * (z * vx - x * vz);
    const tz = 2 * (x * vy - y * vx);
    return [
      vx + w * tx + (y * tz - z * ty),
      vy + w * ty + (z * tx - x * tz),
      vz + w * tz + (x * ty - y * tx),
    ];
  }

  /** Angle in degrees between two orientations (0 when equal, sign-insensitive). */
  function quatAngleDeg(a, b) {
    const qa = quatNormalize(a), qb = quatNormalize(b);
    const dot = Math.abs(qa[0] * qb[0] + qa[1] * qb[1] + qa[2] * qb[2] + qa[3] * qb[3]);
    return radToDeg(2 * Math.acos(clamp(dot, -1, 1)));
  }

  /** FRD [forward, right, down] > UI { x: right, y: up, z: forward }. */
  function uiFromFrd(frd) {
    return { x: frd[1], y: -frd[2], z: frd[0] };
  }

  /** UI { x: right, y: up, z: forward } > FRD [forward, right, down]. */
  function frdFromUi(ui) {
    return [ui.z, ui.x, -ui.y];
  }

  /** Index into positionMetersFRD for a UI axis plus the sign that maps UI > FRD. */
  const UI_AXIS_TO_FRD = { x: { index: 1, sign: 1 }, y: { index: 2, sign: -1 }, z: { index: 0, sign: 1 } };

  function uuid() {
    const c = root.crypto;
    if (c && typeof c.randomUUID === 'function') return c.randomUUID();
    // Fallback (Coherent GT has no crypto.randomUUID): RFC 4122 v4 from getRandomValues or Math.random.
    const bytes = new Uint8Array(16);
    if (c && typeof c.getRandomValues === 'function') c.getRandomValues(bytes);
    else for (let i = 0; i < 16; i++) bytes[i] = Math.floor(Math.random() * 256);
    bytes[6] = (bytes[6] & 0x0f) | 0x40;
    bytes[8] = (bytes[8] & 0x3f) | 0x80;
    const hex = Array.from(bytes, b => ('0' + b.toString(16)).slice(-2)).join('');
    return `${hex.slice(0, 8)}-${hex.slice(8, 12)}-${hex.slice(12, 16)}-${hex.slice(16, 20)}-${hex.slice(20)}`;
  }

  /**
   * Transition easing (protocol v2 sec. 2.1): cut = jump at the end, smoothstep3 (Smooth) = 3u^2 - 2u^3,
   * smoothstep5 (Cinematic) = 6u^5 - 15u^4 + 10u^3.
   */
  function ease(kind, u) {
    const t = clamp(u, 0, 1);
    if (kind === 'cut') return t >= 1 ? 1 : 0;
    if (kind === 'smoothstep5') return t * t * t * (10 + t * (-15 + 6 * t));
    return t * t * (3 - 2 * t);
  }

  const api = {
    DEG, PITCH_LIMIT_DEG, UI_AXIS_TO_FRD,
   degToRad, radToDeg, clamp, wrapDeg,
    quatNormalize, quatFromHpb, hpbFromQuat, quatMultiply, rotateVector, quatAngleDeg,
    uiFromFrd, frdFromUi, uuid, ease,
  };

  root.FSCMath = api;
  if (typeof module !== 'undefined' && module.exports) module.exports = api;
})(typeof globalThis !== 'undefined' ? globalThis : (typeof window !== 'undefined' ? window : this));
