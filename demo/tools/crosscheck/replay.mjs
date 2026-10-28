// The libm replay for the cross-check: a stand-in for JavaScript's Math whose
// transcendental functions answer from a record of the plugin's own libm calls
// (libm_record.c), so the port can be compared with the C++ to the bit. See
// libm_record.c for why. Everything that is not a libm call in the plugin
// (sqrt, abs, min, max, round, trunc, fround -- exact, or the same IEEE
// operation either side) is Math's own.
//
// A call the record does not hold (an argument the C++ never passed, which is
// what a port error looks like, or a constant the compiler folded at build time
// so that it never reached libm at run time) falls back to Math's and is
// counted: `misses`.
//
// Adapted from slipstream's: keyed by the argument's NUMBER rather than a hex
// string (Valve's record runs to millions of calls), with -0 kept apart from
// +0, since log1p( -0 ) is -0.

import { readFileSync } from 'node:fs';

const NAMES = { 1: 'exp', 2: 'log1p', 3: 'log', 4: 'log10', 5: 'sinh', 6: 'asinh', 7: 'atan', 8: 'cos', 9: 'exp10', 10: 'sin', 11: 'pow' };
const key = (x) => (Object.is(x, -0) ? 'neg0' : x);

export function loadReplay(path) {
  const buf = readFileSync(path);
  const view = new DataView(buf.buffer, buf.byteOffset, buf.byteLength);
  const maps = Object.fromEntries(Object.values(NAMES).map((n) => [n, new Map()]));
  let calls = 0;
  let at = 0;
  while (at < buf.byteLength) {
    const fn = NAMES[view.getUint8(at)];
    at += 1;
    calls += 1;
    if (fn === 'pow') {
      const x = view.getFloat64(at, true);
      const y = view.getFloat64(at + 8, true);
      let inner = maps.pow.get(key(y));
      if (!inner) maps.pow.set(key(y), (inner = new Map()));
      inner.set(key(x), view.getFloat64(at + 16, true));
      at += 24;
    } else if (fn) {
      maps[fn].set(key(view.getFloat64(at, true)), view.getFloat64(at + 8, true));
      at += 16;
    } else {
      throw new Error(`replay: a record with an unknown function at byte ${at - 1}`);
    }
  }
  return { maps, calls };
}

const ulp64 = (a, b) => {
  if (a === b || (Number.isNaN(a) && Number.isNaN(b))) return 0;
  const v = new DataView(new ArrayBuffer(16));
  v.setFloat64(0, a);
  v.setFloat64(8, b);
  const ia = v.getBigInt64(0);
  const ib = v.getBigInt64(8);
  const d = ia > ib ? ia - ib : ib - ia;
  return d > 1000000n ? Infinity : Number(d);
};

const unkey = (k) => (k === 'neg0' ? -0 : k);

/**
 * A Math whose libm functions answer from `replay` (or Math's own when no
 * replay is given). Returns { math, misses, replayed, compare }.
 */
export function replayMath(replay) {
  const misses = {};
  const math = Object.create(Math);
  const counter = { replayed: 0 };
  if (!replay) return { math, misses, counter, compare: () => null };
  const { maps } = replay;
  const miss = (fn, own) => { misses[fn] = (misses[fn] ?? 0) + 1; return own; };
  const one = (fn) => (x) => {
    const r = maps[fn].get(key(x));
    if (r === undefined) return miss(fn, Math[fn](x));
    counter.replayed += 1;
    return r;
  };
  for (const fn of ['exp', 'log1p', 'log', 'log10', 'sinh', 'asinh', 'atan', 'cos', 'sin']) math[fn] = one(fn);
  // std::pow( 10.0, x ) reaches libm as __exp10( x ) on Darwin.
  math.pow = (x, y) => {
    let r = maps.pow.get(key(y))?.get(key(x));
    if (r === undefined && x === 10) r = maps.exp10.get(key(y));
    if (r === undefined) return miss('pow', Math.pow(x, y));
    counter.replayed += 1;
    return r;
  };

  /** How V8's Math differs from the plugin's libm over every distinct recorded call. */
  const compare = () => {
    const out = {};
    const tally = (fn, own, r) => {
      const o = (out[fn] ??= { distinct: 0, differ: 0, worstUlps: 0 });
      o.distinct += 1;
      const u = ulp64(own, r);
      if (u) { o.differ += 1; o.worstUlps = Math.max(o.worstUlps, u); }
    };
    for (const [fn, map] of Object.entries(maps)) {
      if (fn === 'pow') {
        for (const [y, inner] of map) for (const [x, r] of inner) tally('pow', Math.pow(unkey(x), unkey(y)), r);
      } else if (fn === 'exp10') {
        for (const [x, r] of map) tally('exp10', Math.pow(10, unkey(x)), r);
      } else {
        for (const [x, r] of map) tally(fn, Math[fn](unkey(x)), r);
      }
    }
    return out;
  };

  return { math, misses, counter, compare };
}
