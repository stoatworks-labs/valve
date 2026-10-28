/**
 * Valve — browser demo.
 *
 * The picture through a valve amplifier, run like a guitar through an
 * overdriven amp. The one idea, from `AGENTS.md`: **nobody draws a valve's
 * transfer curve.** It is where the valve's plate-current law (Koren's) meets
 * the circuit's load line. Put the video level on the grid of a real stage,
 * solve for the plate, chain the stages the way a guitar amplifier does, and
 * the overdrive falls out: the operating point's gain, a soft fade into cutoff
 * against a hard knee where the grid conducts, even harmonics from one valve
 * and their cancellation in a pair, crossover when the pair is biased cold.
 *
 * Two halves, and they are not equally faithful here:
 *
 *   **The GPU half is the plugin's own GLSL.** `shaders.js` is the six
 *   `R"( ... )"` pieces of `source/Shaders.cpp` -- the vertex stage, the shared
 *   library (the warped table lookup and the chain), and the head and body of
 *   the describing-function pass and the display pass -- spliced by
 *   `demo/tools/check_shaders.py --write` and never typed. The same script
 *   compares them character for character, and checks they are joined as
 *   `TablesSource()` and `DisplaySource()` join them; `tools/verify.sh` runs
 *   it. They run as the plugin's two passes, with the same textures: every
 *   stage's curve in one 4097 x 18 R32F texture, and the chroma's describing
 *   functions in a 256 x 65 RGBA32F buffer, both read with texelFetch.
 *
 *   **The CPU half is a port.** The plugin solves every stage's curve on the
 *   CPU, in double, whenever a setting it depends on moves, and normalises the
 *   chain there every frame; here that is JavaScript, whose numbers are
 *   doubles too. Between the PORT markers below: Valves.cpp (Koren's table and
 *   law, the operating points, the preamp, single-ended and push-pull solvers),
 *   Chain.cpp (the warped tables, Tables::Update with its re-solve-only-what-
 *   moved keys, the chain as the shader runs it, the describing function, the
 *   normalisation), Controls.cpp (every control law), and the CPU half of
 *   Valve.cpp: the constructor's names, groups and defaults, currentSettings,
 *   and ProcessOpenGL's uniforms, as a function that RETURNS them so the GL code
 *   further down only binds and draws. demo/tools/crosscheck/ (verify.sh runs
 *   it) drives the REAL plugin class in a GL context through a scenario, reads
 *   its tables, its normalisation and every uniform of its two programs back,
 *   and requires this block to print the same thing, bit for bit, with the
 *   plugin's own libm results replayed into it.
 *
 * ---------------------------------------------------------------------------
 * Decisions this page made, and why
 * ---------------------------------------------------------------------------
 *
 * **Stages is FF_TYPE_INTEGER, 0 to 4,** which the kit has no control for: it
 * is a dropdown of its five values, and since they start at 0 the element
 * index the page stores is the integer the host would. Chroma Stages the same.
 *
 * **Nothing audio.** The plugin has no audio input; "overdrive" here is
 * a voltage on a grid, not a sound, so nothing is left out for want of one.
 *
 * **The test hooks are not ported**: the perturbation bits (no grid current,
 * an undriven half, split load lines, baseband chroma, the carrier round the
 * valve, a 12AX7 with mu = 10) exist so `vatest --negative` has wrong models
 * to catch, and the plugin never sets them in a host. `Perturb` is sent as 0,
 * which is what the plugin sends in operation.
 *
 * **The About block is not a parameter here**, as on every page in the suite.
 *
 * **The Presets menu is this page's**: the plugin has none. Each preset is a
 * whole set of the plugin's own controls, after the six settings in the
 * README's curves figure.
 */

import { Program, PassBuffer, GLError, bindTexture } from './vendor/gl.js';
import { mountDemo } from './vendor/demo.js';
import * as S from './shaders.js';

// ==== BEGIN PORT ===========================================================
// Valves.cpp, Chain.cpp, Controls.cpp and the CPU half of Valve.cpp, function
// for function. Everything is double, as in the plugin; `F` is a float cast
// (static_cast< float >). demo/tools/crosscheck/run.mjs slices this block out
// as text and runs it, so what is checked is exactly what the page runs. Keep
// it free of the DOM and of GL.

const F = Math.fround;

//---------------------------------------------------------------------------
// Valves.cpp
//---------------------------------------------------------------------------
const kPi = 3.14159265358979323846;
const Kind = { Triode: 0, Pentode: 1 };

// Koren's Tube.lib (2001), parameter for parameter.
//  name     kind           mu    X     kG1    kG2  kP   kVB    RGI
const kPreamps = [
  { name: '12AX7', kind: Kind.Triode, mu: 100.0, ex: 1.40, kg1: 1060.0, kg2: 0.0, kp: 600.0, kvb: 300.0, rgi: 2000.0 },
  { name: '12AT7', kind: Kind.Triode, mu: 60.0, ex: 1.35, kg1: 460.0, kg2: 0.0, kp: 300.0, kvb: 300.0, rgi: 2000.0 },
  { name: '12AU7', kind: Kind.Triode, mu: 21.5, ex: 1.30, kg1: 1180.0, kg2: 0.0, kp: 84.0, kvb: 300.0, rgi: 2000.0 },
  { name: '6DJ8', kind: Kind.Triode, mu: 28.0, ex: 1.30, kg1: 330.0, kg2: 0.0, kp: 320.0, kvb: 300.0, rgi: 2000.0 },
];
const kPowers = [
  { name: 'EL34', kind: Kind.Pentode, mu: 11.0, ex: 1.35, kg1: 650.0, kg2: 4200.0, kp: 60.0, kvb: 24.0, rgi: 1000.0 },
  { name: '6L6GC', kind: Kind.Pentode, mu: 8.7, ex: 1.35, kg1: 1460.0, kg2: 4500.0, kp: 48.0, kvb: 12.0, rgi: 1000.0 },
  { name: 'KT88', kind: Kind.Pentode, mu: 8.8, ex: 1.35, kg1: 730.0, kg2: 4200.0, kp: 32.0, kvb: 16.0, rgi: 1000.0 },
  { name: '300B', kind: Kind.Triode, mu: 3.95, ex: 1.40, kg1: 1550.0, kg2: 0.0, kp: 65.0, kvb: 300.0, rgi: 1000.0 },
];
const kPreampCount = 4;
const kPowerCount = 4;
const k12AX7 = 0;
const k12AU7 = 2;
const kEL34 = 0;

// B+, rated plate dissipation, plate-to-plate load.
const kCircuits = [
  { bplus: 450.0, ratedWatts: 25.0, raa: 3400.0 },
  { bplus: 450.0, ratedWatts: 30.0, raa: 4000.0 },
  { bplus: 500.0, ratedWatts: 35.0, raa: 4000.0 },
  { bplus: 400.0, ratedWatts: 36.0, raa: 5000.0 },
];

const kPreampSupply = 300.0;
const kPlateLoad = 100e3;
const kGridLeak = 1e6;
const kInputStopper = 68e3;
const kDriverSource = 47e3;
const kDefaultIdle = 0.70;

const clamp = (x, lo, hi) => (x < lo ? lo : hi < x ? hi : x);

function AcLoad() {
  return (kPlateLoad * kGridLeak) / (kPlateLoad + kGridLeak);
}

function logistic(z) {
  if (z >= 0.0) return 1.0 / (1.0 + Math.exp(-z));
  const e = Math.exp(z);
  return e / (1.0 + e);
}

/**
 * The root of an INCREASING function on [lo, hi]: Newton where it stays
 * inside the bracket, bisection where it does not. `f( x, out )` writes
 * out.value and out.slope.
 */
const solveOut = { value: 0, slope: 0 };
function solveIncreasing(f, lo, hi, guess) {
  let x = clamp(guess, lo, hi);
  for (let iteration = 0; iteration < 200; ++iteration) {
    f(x, solveOut);
    const value = solveOut.value;
    const slope = solveOut.slope;
    if (value === 0.0) return x;
    if (value < 0.0) lo = x;
    else hi = x;
    let next = slope > 0.0 ? x - value / slope : 0.5 * (lo + hi);
    if (!(next > lo && next < hi)) next = 0.5 * (lo + hi);
    if (Math.abs(next - x) <= 1e-13 * Math.max(1.0, Math.abs(x)) || hi - lo <= 1e-13 * Math.max(1.0, Math.abs(x))) return next;
    x = next;
  }
  return x;
}

function PreampValve(index) {
  return kPreamps[clamp(index, 0, kPreampCount - 1)];
}

function PowerValve(index) {
  return kPowers[clamp(index, 0, kPowerCount - 1)];
}

function PowerCircuitOf(index) {
  return kCircuits[clamp(index, 0, kPowerCount - 1)];
}

function PowerSupply(valve) {
  return PowerCircuitOf(valve).bplus;
}

function Softplus(z) {
  return z > 0.0 ? z + Math.log1p(Math.exp(-z)) : Math.log1p(Math.exp(z));
}

/** Koren's plate current and its partial derivatives: { ip, dEp, dEg }. */
function PlateCurrent(v, eg, ep, screen, scale = 1.0) {
  const c = { ip: 0.0, dEp: 0.0, dEg: 0.0 };
  const kg1 = v.kg1 * scale;
  if (v.kind === Kind.Triode) {
    if (ep <= 0.0) return c;
    const s = Math.sqrt(v.kvb + ep * ep);
    const z = v.kp * (1.0 / v.mu + eg / s);
    const e1 = (ep / v.kp) * Softplus(z);
    if (e1 <= 0.0) return c;
    const sig = logistic(z);
    const dIdE1 = (2.0 * v.ex * Math.pow(e1, v.ex - 1.0)) / kg1;
    const dE1dEp = Softplus(z) / v.kp - (ep * ep * eg * sig) / (s * s * s);
    const dE1dEg = (ep * sig) / s;
    c.ip = (2.0 * Math.pow(e1, v.ex)) / kg1;
    c.dEp = dIdE1 * dE1dEp;
    c.dEg = dIdE1 * dE1dEg;
    return c;
  }

  const z = v.kp * (1.0 / v.mu + eg / screen);
  const e1 = (screen / v.kp) * Softplus(z);
  if (e1 <= 0.0) return c;
  const plate = Math.max(ep, 0.0);
  const knee = Math.atan(plate / v.kvb);
  const law = (2.0 * Math.pow(e1, v.ex)) / kg1;
  c.ip = law * knee;
  c.dEp = ep > 0.0 ? law / v.kvb / (1.0 + (plate / v.kvb) * (plate / v.kvb)) : 0.0;
  c.dEg = ((2.0 * v.ex * Math.pow(e1, v.ex - 1.0)) / kg1) * knee * logistic(z);
  return c;
}

/** The grid seen through its source (the plugin's perturbation bit is a test hook, not ported). */
function GridVolts(open, rgi, source) {
  if (open <= 0.0) return open;
  return (open * rgi) / (rgi + source);
}

function CutoffGrid(v, supply) {
  if (v.kind === Kind.Triode) return -Math.sqrt(v.kvb + supply * supply) / v.mu;
  return -supply / v.mu;
}

function PreampBiasGrid(v, fraction) {
  return CutoffGrid(v, kPreampSupply) * (1.0 - fraction);
}

function PreampQuiescent(v, eg, scale) {
  const vp = solveIncreasing(
    (x, out) => {
      const c = PlateCurrent(v, eg, x, 0.0, scale);
      out.value = x - kPreampSupply + kPlateLoad * c.ip;
      out.slope = 1.0 + kPlateLoad * c.dEp;
    },
    0.0, kPreampSupply, 0.6 * kPreampSupply,
  );

  const c = PlateCurrent(v, eg, vp, 0.0, scale);
  const q = { eg, vp, ip: c.ip, gm: c.dEg, rp: 0, mu: 0, rout: 0 };
  q.rp = c.dEp > 0.0 ? 1.0 / c.dEp : 1e300;
  q.mu = q.gm * q.rp;
  q.rout = 1.0 / (1.0 / q.rp + 1.0 / AcLoad());
  return q;
}

function PreampPlate(v, q, open, source, scale, guess = -1.0) {
  const eg = GridVolts(open, v.rgi, source);
  const rl = AcLoad();
  const top = q.vp + rl * q.ip;
  const vp = solveIncreasing(
    (x, out) => {
      const c = PlateCurrent(v, eg, x, 0.0, scale);
      out.value = x - q.vp + rl * (c.ip - q.ip);
      out.slope = 1.0 + rl * c.dEp;
    },
    0.0, top, guess >= 0.0 ? guess : q.vp,
  );
  return vp - q.vp;
}

const PowerStage = { Off: 0, SingleEnded: 1, PushPull: 2, Count: 3 };

/** `guess`, when given, is a one-element array: the raw solve's unknown at a neighbouring drive. */
function powerRaw(valve, stage, bias, seLoad, d, scaleA, scaleB, guess = null) {
  const v = PowerValve(valve);
  const circ = PowerCircuitOf(valve);
  const bplus = circ.bplus;

  if (stage === PowerStage.SingleEnded) {
    const eg = GridVolts(bias + d, v.rgi, kDriverSource);
    const iq = PlateCurrent(v, bias, bplus, bplus, scaleA).ip;
    const vp = solveIncreasing(
      (x, out) => {
        const c = PlateCurrent(v, eg, x, bplus, scaleA);
        out.value = x - bplus + seLoad * (c.ip - iq);
        out.slope = 1.0 + seLoad * c.dEp;
      },
      0.0, bplus + seLoad * iq, guess ? guess[0] : bplus,
    );
    if (guess) guess[0] = vp;
    return vp - bplus;
  }

  const g1 = GridVolts(bias + d, v.rgi, kDriverSource);
  const g2 = GridVolts(bias - d, v.rgi, kDriverSource);
  const q4 = circ.raa / 4.0;

  const vh = solveIncreasing(
    (x, out) => {
      const c1 = PlateCurrent(v, g1, bplus - x, bplus, scaleA);
      const c2 = PlateCurrent(v, g2, bplus + x, bplus, scaleB);
      out.value = x - q4 * (c1.ip - c2.ip);
      out.slope = 1.0 + q4 * (c1.dEp + c2.dEp);
    },
    -bplus, bplus, guess ? guess[0] : 0.0,
  );
  if (guess) guess[0] = vh;
  return vh;
}

function PowerQuiescent(valve, stage, idle, scaleA, scaleB) {
  const v = PowerValve(valve);
  const circ = PowerCircuitOf(valve);
  const bplus = circ.bplus;

  const q = { bias: 0.0, iq: 0.0, seLoad: 0.0, vhq: 0.0 };
  const target = (idle * circ.ratedWatts) / bplus;
  if (PlateCurrent(v, 0.0, bplus, bplus).ip <= target) q.bias = 0.0;
  else {
    q.bias = solveIncreasing(
      (x, out) => {
        const c = PlateCurrent(v, x, bplus, bplus);
        out.value = c.ip - target;
        out.slope = c.dEg;
      },
      -bplus, 0.0, CutoffGrid(v, bplus) * 0.5,
    );
  }
  q.iq = PlateCurrent(v, q.bias, bplus, bplus).ip;
  q.seLoad = (bplus * bplus) / (kDefaultIdle * circ.ratedWatts);
  q.vhq = stage === PowerStage.PushPull ? powerRaw(valve, stage, q.bias, q.seLoad, 0.0, scaleA, scaleB) : 0.0;
  return q;
}

function PowerOut(valve, stage, q, d, scaleA, scaleB, guess = null) {
  if (stage === PowerStage.Off) return d;
  return powerRaw(valve, stage, q.bias, q.seLoad, d, scaleA, scaleB, guess) - (stage === PowerStage.PushPull ? q.vhq : 0.0);
}

//---------------------------------------------------------------------------
// Chain.cpp
//---------------------------------------------------------------------------
const kNodes = 4097;
const kCentre = 2048;
const kChains = 2;
const kChannels = 3;
const kMain = 0;
const kChroma = 1;
const kFirst = 0;
const kLater = 1;
const kPower = 2;
const kKinds = 3;
const kRows = kChains * kChannels * kKinds;
const kReach = 1500.0;
const kTaps = 64;
const kTableY = 256;
const kTableA = 64;

const RowIndex = (chain, channel, kind) => (chain * kChannels + channel) * kKinds + kind;

const Signal = { RGB: 0, YC: 1, Composite: 2, Count: 3 };
const Output = { Fit: 0, Unity: 1, Plate: 2, Count: 3 };

function warpAt(warp, i) {
  return warp.c + warp.w * Math.sinh((i - kCentre) * warp.du);
}

function ChannelScale(channel, mismatch) {
  return 1.0 + mismatch * (1 - channel);
}

function HalfScale(channel, mismatch, half) {
  return ChannelScale(channel, mismatch) * (half === 0 ? 1.0 + mismatch : 1.0 - mismatch);
}

let cosTable = null;
function CosTable() {
  if (!cosTable) {
    cosTable = new Float32Array(kTaps);
    for (let k = 0; k < kTaps; ++k) cosTable[k] = F(Math.cos((2.0 * kPi * k) / kTaps));
  }
  return cosTable;
}

function ChromaReach() {
  const y = 0.299;
  const u = 0.492111 * (0.0 - y);
  const v = 0.877283 * (1.0 - y);
  return Math.sqrt(u * u + v * v);
}

/** std::lround: half away from zero. */
function lround(x) {
  return x >= 0 ? Math.round(x) : -Math.round(-x);
}

function AlignedWarp(c, scale, knee) {
  const warp = { c, w: scale, du: Math.asinh(kReach / scale) / kCentre };
  if (!(knee > 0.0)) return warp;
  const exact = Math.asinh(knee / scale) / warp.du;
  const j = clamp(lround(exact), 1, kCentre);
  warp.w = knee / Math.sinh(j * warp.du);
  return warp;
}

const keyEqual = (a, b) =>
  a.valve === b.valve && a.stage === b.stage && a.bias === b.bias && a.scaleA === b.scaleA && a.scaleB === b.scaleB
  && a.source === b.source;
const newKey = () => ({ valve: -1, stage: -1, bias: -1.0, scaleA: -1.0, scaleB: -1.0, source: -1.0 });

/** The settings struct: ChainSettings x 2 and the rest, with the plugin's member defaults. */
function newChainSettings() {
  return { stages: 2, preamp: k12AX7, bias: 0.5, drive: 1.0, power: PowerStage.PushPull, powerValve: kEL34, idle: kDefaultIdle, master: 1.0 };
}
function newSettings() {
  return { chain: [newChainSettings(), newChainSettings()], interstage: 0.1, mismatch: 0.0, rest: 0.5, perChannel: false };
}

class Tables {
  constructor() {
    this.rows = Array.from({ length: kRows }, () => ({ warp: { c: 0.0, w: 1.0, du: 1.0 }, values: new Float32Array(0), built: false }));
    this.keys = Array.from({ length: kRows }, newKey);
    this.vgq = [0.0, 0.0];
    this.stageQ = Array.from({ length: kChains * kChannels }, () => null);
    this.powerQ = Array.from({ length: kChains * kChannels }, () => null);
    this.settings = newSettings();
  }

  buildPreamp(row, v, q, source, scale) {
    const t = this.rows[row];
    t.warp = AlignedWarp(q.eg, Math.abs(CutoffGrid(v, kPreampSupply)) / 8.0, -q.eg);
    t.values = new Float32Array(kNodes);
    for (const direction of [+1, -1]) {
      let plate = q.vp;
      for (let i = kCentre + direction; i >= 0 && i < kNodes; i += direction) {
        const dp = PreampPlate(v, q, warpAt(t.warp, i), source, scale, plate);
        plate = q.vp + dp;
        t.values[i] = dp;
      }
    }
    t.built = true;
  }

  buildPower(row, valve, stage, q, scaleA, scaleB) {
    const t = this.rows[row];
    const knee = Math.abs(q.bias);
    t.warp = AlignedWarp(0.0, Math.max(knee, 1.0) / 8.0, knee);
    t.values = new Float32Array(kNodes);
    for (const direction of [+1, -1]) {
      const unknown = [stage === PowerStage.SingleEnded ? PowerCircuitOf(valve).bplus : q.vhq];
      for (let i = kCentre + direction; i >= 0 && i < kNodes; i += direction) {
        t.values[i] = PowerOut(valve, stage, q, warpAt(t.warp, i), scaleA, scaleB, unknown);
      }
    }
    t.built = true;
  }

  /** Solve every table the settings need that is missing or stale; a bitmask of the rows that changed. */
  Update(next) {
    this.settings = next;
    let mask = 0;

    for (let chain = 0; chain < kChains; ++chain) {
      const cs = next.chain[chain];
      const v = PreampValve(cs.preamp);
      this.vgq[chain] = PreampBiasGrid(v, cs.bias);

      const split = chain === kMain && next.perChannel;
      for (let ch = split ? 0 : 1; ch <= (split ? 2 : 1); ++ch) {
        const at = chain * kChannels + ch;
        const scale = ChannelScale(ch, next.mismatch);
        this.stageQ[at] = PreampQuiescent(v, this.vgq[chain], scale);

        const preamp = (kind, source) => {
          const row = RowIndex(chain, ch, kind);
          const key = newKey();
          key.valve = cs.preamp;
          key.bias = this.vgq[chain];
          key.scaleA = scale;
          key.source = source;
          if (this.rows[row].built && keyEqual(this.keys[row], key)) return;
          this.buildPreamp(row, v, this.stageQ[at], source, scale);
          this.keys[row] = key;
          mask |= 1 << row;
        };
        if (cs.stages >= 1) preamp(kFirst, kInputStopper);
        if (cs.stages >= 2) preamp(kLater, this.stageQ[at].rout);

        if (cs.power !== PowerStage.Off) {
          const pp = cs.power === PowerStage.PushPull;
          const sA = pp ? HalfScale(ch, next.mismatch, 0) : scale;
          const sB = pp ? HalfScale(ch, next.mismatch, 1) : scale;
          this.powerQ[at] = PowerQuiescent(cs.powerValve, cs.power, cs.idle, sA, sB);

          const row = RowIndex(chain, ch, kPower);
          const key = newKey();
          key.valve = cs.powerValve;
          key.stage = cs.power;
          key.bias = cs.idle;
          key.scaleA = sA;
          key.scaleB = sB;
          if (!(this.rows[row].built && keyEqual(this.keys[row], key))) {
            this.buildPower(row, cs.powerValve, cs.power, this.powerQ[at], sA, sB);
            this.keys[row] = key;
            mask |= 1 << row;
          }
        }
      }
    }
    return mask >>> 0;
  }

  Row(row) {
    return this.rows[row];
  }

  /** The warp as the shader receives it: c, w and 1 / du, as floats. */
  RowWarp(row) {
    const w = this.rows[row].warp;
    return { c: F(w.c), w: F(w.w), invDu: F(1.0 / w.du) };
  }

  RowBase(chain, channel) {
    const split = chain === kMain && this.settings.perChannel;
    return RowIndex(chain, split ? channel : 1, 0);
  }

  Vgq(chain) {
    return this.vgq[chain];
  }

  Quiescent(chain, channel) {
    return this.stageQ[chain * kChannels + channel];
  }

  PowerQuiescent(chain, channel) {
    return this.powerQ[chain * kChannels + channel];
  }

  Current() {
    return this.settings;
  }

  Lookup(row, v) {
    const t = this.rows[row];
    if (!t.built) return 0.0;
    const wf = this.RowWarp(row);
    const z = (v - wf.c) / wf.w;
    const a = Math.abs(z);
    const magnitude = Math.log(a + Math.sqrt(a * a + 1.0));
    const u = z < 0 || Object.is(z, -0) ? -magnitude : magnitude;
    let x = kCentre + u * wf.invDu;
    x = clamp(x, 0.0, kNodes - 1);
    const i = Math.min(Math.trunc(x), kNodes - 2);
    const f = x - i;
    const l = t.values[i];
    const r = t.values[i + 1];
    return l + f * (r - l);
  }

  Evaluate(chain, channel, s) {
    const cs = this.settings.chain[chain];
    const base = this.RowBase(chain, channel);
    const vg = F(this.vgq[chain]);
    const drive = F(cs.drive);
    const gain = F(cs.master);
    const k = F(this.settings.interstage);

    let d = 0.0;
    if (cs.stages > 0) {
      let p = this.Lookup(base + kFirst, vg + drive * s);
      for (let i = 1; i < cs.stages; ++i) p = this.Lookup(base + kLater, vg + k * p);
      if (cs.power === PowerStage.Off) return p;
      d = gain * p;
    } else {
      if (cs.power === PowerStage.Off) return drive * s;
      d = drive * s;
    }
    return this.Lookup(base + kPower, d);
  }

  /** The fundamental over one subcarrier cycle; `dc`, if given, a one-element array for the mean. */
  Fundamental(chain, channel, offset, a, dc = null) {
    const cosines = CosTable();
    let sum = 0.0;
    let first = 0.0;
    for (let k = 0; k < kTaps; ++k) {
      const c = cosines[k];
      const out = this.Evaluate(chain, channel, offset + a * c);
      sum += out;
      first += out * c;
    }
    if (dc) dc[0] = sum / kTaps;
    return (first * 2.0) / kTaps;
  }
}

function Slope(tables, chain, channel) {
  const cs = tables.Current().chain[chain];
  const h = 1e-9 / Math.max(1.0, cs.drive);
  const f0 = tables.Evaluate(chain, channel, 0.0);
  const right = (tables.Evaluate(chain, channel, h) - f0) / h;
  const left = (f0 - tables.Evaluate(chain, channel, -h)) / h;
  return 0.5 * (left + right);
}

function PlateSupply(c) {
  return c.power !== PowerStage.Off ? PowerSupply(c.powerValve) : kPreampSupply;
}

const inverse = (x) => (Math.abs(x) > 1e-300 ? 1.0 / x : 0.0);
const signOf = (x) => (x < 0.0 ? -1.0 : 1.0);

function Normalise(tables, signal, output, asWired) {
  const n = { offset: 0.0, ref: [0.0, 0.0, 0.0], scale: [1.0, 1.0, 1.0], chromaScale: 1.0 };
  const s = tables.Current();
  for (let ch = 0; ch < kChannels; ++ch) {
    const channel = signal === Signal.RGB ? ch : 1;
    const f0 = tables.Evaluate(kMain, channel, -s.rest);
    const f1 = tables.Evaluate(kMain, channel, 1.0 - s.rest);
    const fs = Slope(tables, kMain, channel);
    switch (output) {
      case Output.Unity:
        n.offset = s.rest;
        n.ref[ch] = tables.Evaluate(kMain, channel, 0.0);
        n.scale[ch] = asWired ? inverse(Math.abs(fs)) : inverse(fs);
        break;
      case Output.Plate:
        n.offset = s.rest;
        n.ref[ch] = 0.0;
        n.scale[ch] = (asWired ? 1.0 : signOf(fs)) / PlateSupply(s.chain[kMain]);
        break;
      default: // Fit, Count
        n.offset = 0.0;
        n.ref[ch] = asWired ? Math.min(f0, f1) : f0;
        n.scale[ch] = asWired ? inverse(Math.abs(f1 - f0)) : inverse(f1 - f0);
        break;
    }
  }

  if (signal !== Signal.YC) {
    n.chromaScale = n.scale[1];
    return n;
  }
  switch (output) {
    case Output.Unity: {
      const gs = Slope(tables, kChroma, 1);
      n.chromaScale = asWired ? inverse(Math.abs(gs)) : inverse(gs);
      break;
    }
    case Output.Plate:
      n.chromaScale = (asWired ? 1.0 : signOf(Slope(tables, kChroma, 1))) / PlateSupply(s.chain[kChroma]);
      break;
    default: {
      const reach = ChromaReach();
      const f = tables.Fundamental(kChroma, 1, 0.0, reach);
      n.chromaScale = reach * (asWired ? inverse(Math.abs(f)) : inverse(f));
      break;
    }
  }
  return n;
}

//---------------------------------------------------------------------------
// Controls.cpp. A host hands the plugin floats: `host` values are float.
//---------------------------------------------------------------------------
const unit = (value) => clamp(value, 0.0, 1.0);
const kMaxStages = 4;

function OptionIndex(value, count) {
  return clamp(lround(value), 0, count - 1);
}
function Stages(value) {
  return clamp(lround(value), 0, kMaxStages);
}
function DriveVolts(value) {
  return Math.pow(10.0, -2.0 + 4.5 * unit(value));
}
function MasterGain(value) {
  return Math.pow(10.0, -2.0 + 4.0 * unit(value));
}
function BiasFraction(value) {
  return 0.05 + 0.9 * unit(value);
}
function IdleFraction(value) {
  return 0.05 + 0.95 * unit(value);
}
function InterstageGain(value) {
  return Math.pow(10.0, (-40.0 + 40.0 * unit(value)) / 20.0);
}
function RestLevel(value) {
  return unit(value);
}
function MismatchFraction(value) {
  return 0.25 * unit(value);
}
function DriveSlider(volts) {
  return F((Math.log10(volts) + 2.0) / 4.5);
}
function MasterSlider(gain) {
  return F((Math.log10(gain) + 2.0) / 4.0);
}
function IdleSlider(fraction) {
  return F((fraction - 0.05) / 0.95);
}

//---------------------------------------------------------------------------
// Valve.cpp: the parameters, the constructor, currentSettings, and the
// uniforms ProcessOpenGL hands its two programs.
//---------------------------------------------------------------------------
const PT = {
  SIGNAL: 0, REST: 1, INTERSTAGE: 2, MISMATCH: 3,
  STAGES: 4, PREAMP: 5, DRIVE: 6, BIAS: 7, POWER_STAGE: 8, POWER_VALVE: 9, MASTER: 10, POWER_BIAS: 11,
  C_STAGES: 12, C_PREAMP: 13, C_DRIVE: 14, C_BIAS: 15, C_POWER_STAGE: 16, C_POWER_VALVE: 17, C_MASTER: 18, C_POWER_BIAS: 19,
  OUTPUT: 20, POLARITY: 21, SHOW_CURVE: 22, MIX: 23,
  COUNT: 24, // the About block follows; it is not on this page
};
const At = { Stages: 0, Preamp: 1, Drive: 2, Bias: 3, PowerStage: 4, PowerValve: 5, Master: 6, PowerBias: 7 };

const kSignalNames = ['RGB', 'Y/C', 'Composite'];
const kStageNames = ['Off', 'Single-ended', 'Push-pull'];
const kOutputNames = ['Fit', 'Unity', 'Plate'];
const kPolarityNames = ['Corrected', 'As Wired'];
const kPreampNames = kPreamps.map((v) => v.name);
const kPowerNames = kPowers.map((v) => v.name);

/** The constructor's `params[]`, filled before any declaration. */
function Defaults() {
  const p = new Float32Array(PT.COUNT);
  p[PT.SIGNAL] = Signal.RGB;
  p[PT.REST] = 0.3;
  p[PT.INTERSTAGE] = 0.5;
  p[PT.MISMATCH] = 0.0;

  p[PT.STAGES] = 2.0;
  p[PT.PREAMP] = k12AX7;
  p[PT.DRIVE] = DriveSlider(1.2);
  p[PT.BIAS] = 0.5;
  p[PT.POWER_STAGE] = PowerStage.PushPull;
  p[PT.POWER_VALVE] = kEL34;
  p[PT.MASTER] = MasterSlider(0.3);
  p[PT.POWER_BIAS] = IdleSlider(kDefaultIdle);

  p[PT.C_STAGES] = 1.0;
  p[PT.C_PREAMP] = k12AU7;
  p[PT.C_DRIVE] = DriveSlider(10.0);
  p[PT.C_BIAS] = 0.5;
  p[PT.C_POWER_STAGE] = PowerStage.Off;
  p[PT.C_POWER_VALVE] = kEL34;
  p[PT.C_MASTER] = MasterSlider(1.0);
  p[PT.C_POWER_BIAS] = IdleSlider(kDefaultIdle);

  p[PT.OUTPUT] = Output.Fit;
  p[PT.POLARITY] = 0.0;
  p[PT.SHOW_CURVE] = 0.0;
  p[PT.MIX] = 1.0;
  return p;
}

/** declareChain's names: a prefixed chain says "Pwr" where the main one says "Power". */
function chainName(prefix, base) {
  let b = base;
  if (prefix !== '' && b.startsWith('Power ')) b = `Pwr ${b.slice(6)}`;
  return prefix + b;
}

/** Every parameter the constructor declares, in id order: [name, group, kind, elements]. */
function Declarations() {
  const out = [
    ['Signal', 'Signal', 'option', kSignalNames],
    ['Rest Level', 'Signal', 'standard', null],
    ['Interstage', 'Signal', 'standard', null],
    ['Mismatch', 'Signal', 'standard', null],
  ];
  for (const [prefix, group] of [['', 'Luma / RGB'], ['Chroma ', 'Chroma']]) {
    out.push([chainName(prefix, 'Stages'), group, 'integer', ['0', '1', '2', '3', '4']]);
    out.push([chainName(prefix, 'Preamp'), group, 'option', kPreampNames]);
    out.push([chainName(prefix, 'Drive'), group, 'standard', null]);
    out.push([chainName(prefix, 'Bias'), group, 'standard', null]);
    out.push([chainName(prefix, 'Power Stage'), group, 'option', kStageNames]);
    out.push([chainName(prefix, 'Power Valve'), group, 'option', kPowerNames]);
    out.push([chainName(prefix, 'Master'), group, 'standard', null]);
    out.push([chainName(prefix, 'Power Bias'), group, 'standard', null]);
  }
  out.push(['Output', 'Output', 'option', kOutputNames]);
  out.push(['Polarity', 'Output', 'option', kPolarityNames]);
  out.push(['Show Curve', 'Output', 'boolean', null]);
  out.push(['Mix', 'Output', 'standard', null]);
  return out;
}

/** Valve::currentSettings, from the host's floats. */
function currentSettings(host) {
  const s = newSettings();
  for (let c = 0; c < kChains; ++c) {
    const first = c === kMain ? PT.STAGES : PT.C_STAGES;
    const cs = s.chain[c];
    cs.stages = Stages(host[first + At.Stages]);
    cs.preamp = OptionIndex(host[first + At.Preamp], kPreampCount);
    cs.drive = DriveVolts(host[first + At.Drive]);
    cs.bias = BiasFraction(host[first + At.Bias]);
    cs.power = OptionIndex(host[first + At.PowerStage], PowerStage.Count);
    cs.powerValve = OptionIndex(host[first + At.PowerValve], kPowerCount);
    cs.master = MasterGain(host[first + At.Master]);
    cs.idle = IdleFraction(host[first + At.PowerBias]);
  }
  s.interstage = InterstageGain(host[PT.INTERSTAGE]);
  s.mismatch = MismatchFraction(host[PT.MISMATCH]);
  s.rest = RestLevel(host[PT.REST]);
  const signal = OptionIndex(host[PT.SIGNAL], Signal.Count);
  s.perChannel = signal === Signal.RGB && s.mismatch > 0.0;
  return s;
}

/** Valve::setLibrary: the uniforms both programs share, as GL would store them. */
function libraryUniforms(tables, s) {
  const WarpOf = new Float32Array(kRows * 3);
  for (let r = 0; r < kRows; ++r) {
    const w = tables.RowWarp(r);
    WarpOf[r * 3 + 0] = w.c;
    WarpOf[r * 3 + 1] = w.w;
    WarpOf[r * 3 + 2] = w.invDu;
  }
  const RowOf = new Int32Array(kChains * kChannels);
  for (let c = 0; c < kChains; ++c) for (let ch = 0; ch < kChannels; ++ch) RowOf[c * kChannels + ch] = tables.RowBase(c, ch);
  const StagesOf = new Int32Array(kChains);
  const PowerOf = new Int32Array(kChains);
  const DriveOf = new Float32Array(kChains);
  const MasterOf = new Float32Array(kChains);
  const VgqOf = new Float32Array(kChains);
  for (let c = 0; c < kChains; ++c) {
    const cs = s.chain[c];
    StagesOf[c] = cs.stages;
    PowerOf[c] = cs.power;
    DriveOf[c] = cs.drive;
    MasterOf[c] = cs.master;
    VgqOf[c] = tables.Vgq(c);
  }
  return { WarpOf, RowOf, StagesOf, PowerOf, DriveOf, MasterOf, VgqOf, Interstage: F(s.interstage), Rest: F(s.rest) };
}

/**
 * The CPU half of Valve::ProcessOpenGL. Updates `tables` (re-solving only what
 * a setting touched) and returns what the plugin hands GL: the changed rows,
 * whether the describing-function pass runs, and every uniform of each pass.
 * `host` is the 24 parameter floats; the viewport is the host's.
 */
function Frame(tables, host, inWidth, inHeight, vp) {
  const settings = currentSettings(host);
  const signal = OptionIndex(host[PT.SIGNAL], Signal.Count);
  const output = OptionIndex(host[PT.OUTPUT], Output.Count);
  const asWired = OptionIndex(host[PT.POLARITY], 2) === 1;

  const changed = tables.Update(settings);
  const norm = Normalise(tables, signal, output, asWired);
  const needTables = signal !== Signal.RGB;
  const library = libraryUniforms(tables, settings);

  const tablesPass = needTables
    ? { ...library, Lut: 1, Cosines: CosTable(), Reach: F(ChromaReach()) }
    : null;

  const display = {
    ...library,
    InputTexture: 0,
    Lut: 1,
    Tables: 2,
    InWidth: inWidth,
    InHeight: inHeight,
    VpX: vp[0],
    VpY: vp[1],
    VpW: vp[2],
    VpH: vp[3],
    Mode: signal,
    Offset: F(norm.offset),
    Ref: Float32Array.of(norm.ref[0], norm.ref[1], norm.ref[2]),
    Scale: Float32Array.of(norm.scale[0], norm.scale[1], norm.scale[2]),
    ChromaScale: F(norm.chromaScale),
    Reach: F(ChromaReach()),
    CarrierGain: F(Slope(tables, kMain, 1) * norm.scale[1]),
    Perturb: 0,
    MixAmount: F(host[PT.MIX]),
    ShowCurve: host[PT.SHOW_CURVE] >= 0.5 ? 1 : 0,
  };

  return { settings, signal, output, asWired, changed, norm, needTables, tables: tablesPass, display };
}

// ==== END PORT =============================================================

//---------------------------------------------------------------------------
// The renderer: what ProcessOpenGL does with GL, in WebGL2. Nothing below
// computes a number the picture depends on; it binds what Frame() returns.
//---------------------------------------------------------------------------
const INT_UNIFORMS = new Set(['InputTexture', 'Lut', 'Tables', 'InWidth', 'InHeight', 'VpX', 'VpY', 'VpW', 'VpH', 'Mode', 'Perturb', 'ShowCurve']);

function bindUniforms(gl, program, uniforms) {
  for (const [name, value] of Object.entries(uniforms)) {
    if (name === 'RowOf' || name === 'StagesOf' || name === 'PowerOf') {
      const loc = program.location(`${name}[0]`) ?? program.location(name);
      if (loc === null) program.missing.add(name);
      else gl.uniform1iv(loc, value);
    } else if (name === 'WarpOf' || name === 'Ref' || name === 'Scale') {
      program.setArray(name, value, 3);
    } else if (value instanceof Float32Array) {
      program.setArray(name, value, 1);
    } else if (INT_UNIFORMS.has(name)) {
      program.setInt(name, value);
    } else {
      program.set(name, value);
    }
  }
}

function createRenderer(gl, quad) {
  // The tables are 4097 texels wide, one more than a 4096 limit: say so rather
  // than shrink them, which would move the warp's nodes off the grid-current
  // knee the plugin places on one.
  const maxSize = gl.getParameter(gl.MAX_TEXTURE_SIZE);
  if (maxSize < kNodes) {
    throw new GLError(`This GPU's largest texture is ${maxSize} texels wide. The plugin keeps every valve stage's curve in a table ${kNodes} texels wide, and this page will not shrink it.`);
  }

  const tablesSource = S.TABLES_HEAD + S.LIBRARY + S.TABLES_BODY;
  const displaySource = S.DISPLAY_HEAD + S.LIBRARY + S.DISPLAY_BODY;
  const programs = {
    tables: new Program(gl, S.VERTEX, tablesSource, 'tables'),
    display: new Program(gl, S.VERTEX, displaySource, 'display'),
  };

  // Every stage's curve, one row each, R32F, allocated at its full size and
  // zeroed, as InitGL does.
  const lut = gl.createTexture();
  gl.bindTexture(gl.TEXTURE_2D, lut);
  gl.texStorage2D(gl.TEXTURE_2D, 1, gl.R32F, kNodes, kRows);
  gl.texSubImage2D(gl.TEXTURE_2D, 0, 0, 0, kNodes, kRows, gl.RED, gl.FLOAT, new Float32Array(kNodes * kRows));
  gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.NEAREST);
  gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.NEAREST);
  gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, gl.CLAMP_TO_EDGE);
  gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, gl.CLAMP_TO_EDGE);
  gl.bindTexture(gl.TEXTURE_2D, null);

  // 256 x 65 RGBA32F, nearest: the chroma's describing functions. Allocated in
  // every mode, as in the plugin, so the display's Tables sampler always has a
  // real texture.
  const describing = new PassBuffer(gl, { filter: 'nearest' });

  const tables = new Tables();
  let uploaded = false;
  const host = new Float32Array(PT.COUNT);

  const renderer = {
    tables,
    programs,
    stats: null,
    solveMs: 0,
    solvedRows: 0,
    capture: null,

    render({ input, params, width, height }) {
      for (const p of PARAMS) host[p.slot] = params.get(p.id);

      const started = performance.now();
      const frame = Frame(tables, host, input.width, input.height, [0, 0, width, height]);
      const elapsed = performance.now() - started;

      // 1. The stage curves: only the rows that changed go up.
      let changed = frame.changed;
      if (!uploaded) changed = (1 << kRows) - 1;
      if (frame.changed) {
        this.solveMs = elapsed;
        this.solvedRows = popcount(frame.changed);
      }
      if (changed) {
        gl.bindTexture(gl.TEXTURE_2D, lut);
        gl.pixelStorei(gl.UNPACK_ALIGNMENT, 4);
        for (let r = 0; r < kRows; ++r) {
          const t = tables.Row(r);
          if ((changed & (1 << r)) && t.built) gl.texSubImage2D(gl.TEXTURE_2D, 0, 0, r, kNodes, 1, gl.RED, gl.FLOAT, t.values);
        }
        gl.bindTexture(gl.TEXTURE_2D, null);
        uploaded = true;
      }

      // 2. Buffers.
      describing.ensure(kTableY, kTableA + 1, gl.RGBA32F);

      // 3. The chroma's describing functions, when the signal has a chroma.
      if (frame.needTables) {
        describing.bind();
        programs.tables.use();
        bindTexture(gl, 1, lut);
        bindUniforms(gl, programs.tables, frame.tables);
        quad.draw();
        bindTexture(gl, 1, null);
      }

      // 4. Display, into the host's framebuffer and viewport.
      gl.bindFramebuffer(gl.FRAMEBUFFER, null);
      gl.viewport(0, 0, width, height);
      programs.display.use();
      bindTexture(gl, 0, input.texture);
      bindTexture(gl, 1, lut);
      bindTexture(gl, 2, describing.texture);
      bindUniforms(gl, programs.display, frame.display);
      quad.draw();

      if (this.capture) {
        // For the headless comparison with the plugin's harness (AGENTS.md):
        // the picture just drawn and the clip it was drawn from, as RGBA8.
        const out = new Uint8Array(width * height * 4);
        gl.readPixels(0, 0, width, height, gl.RGBA, gl.UNSIGNED_BYTE, out);
        const fb = gl.createFramebuffer();
        gl.bindFramebuffer(gl.FRAMEBUFFER, fb);
        gl.framebufferTexture2D(gl.FRAMEBUFFER, gl.COLOR_ATTACHMENT0, gl.TEXTURE_2D, input.texture, 0);
        const inPixels = new Uint8Array(input.width * input.height * 4);
        gl.readPixels(0, 0, input.width, input.height, gl.RGBA, gl.UNSIGNED_BYTE, inPixels);
        gl.bindFramebuffer(gl.FRAMEBUFFER, null);
        gl.deleteFramebuffer(fb);
        this.capture({ output: out, input: inPixels, width, height });
        this.capture = null;
      }

      for (const unitIndex of [0, 1, 2]) bindTexture(gl, unitIndex, null);

      this.stats = {
        settings: frame.settings,
        signal: frame.signal,
        q: tables.Quiescent(kMain, 1),
        pq: frame.settings.chain[kMain].power !== PowerStage.Off ? tables.PowerQuiescent(kMain, 1) : null,
        missing: [...programs.tables.missing, ...programs.display.missing],
      };
    },
  };
  return renderer;
}

function popcount(n) {
  let c = 0;
  for (let x = n >>> 0; x; x &= x - 1) c += 1;
  return c;
}

//---------------------------------------------------------------------------
// The panel: every declaration, with the plugin's own conversions as readouts.
//---------------------------------------------------------------------------
const kDefaults = Defaults();

const volts = (v) => {
  const a = Math.abs(v);
  if (a < 1) return `${(v * 1000).toPrecision(3)} mV`;
  return `${v.toPrecision(3)} V`;
};
const percent = (x) => `${(x * 100).toFixed(1)} %`;

// id, slot, readout, hint
const SPEC = [
  ['signal', PT.SIGNAL, null, 'RGB: each channel its own amplifier. Y/C: luma through one chain, chroma as a subcarrier through the Chroma chain, so hue survives. Composite: both through one chain, so the colour’s gain follows the brightness it rides on.'],
  ['rest', PT.REST, (v) => RestLevel(v).toFixed(2), 'The picture level that sits at the valves’ rest, where a black-level clamp would hold it. Lower it for dark footage.'],
  ['interstage', PT.INTERSTAGE, (v) => `${(-40 + 40 * unit(v)).toFixed(1)} dB`, 'The loss between one plate and the next grid: the tone stack’s.'],
  ['mismatch', PT.MISMATCH, (v) => percent(MismatchFraction(v)), 'Spread between valves: R, G and B become three valve sets, and each pair’s halves differ.'],

  ['stages', PT.STAGES, null, 'Preamp triodes, 0 to 4. Every one inverts.'],
  ['preamp', PT.PREAMP, null, 'Koren’s model of each. A 12AX7 has a gain of 59 and clips at a volt; a 12AU7 has 13 and stays clean.'],
  ['drive', PT.DRIVE, (v) => volts(DriveVolts(v)), 'Volts on the first grid for black to white, 10 mV to 316 V.'],
  ['bias', PT.BIAS, (v) => percent(BiasFraction(v)), 'The preamp’s rest, from cutoff (0 %) to 0 V on the grid (100 %).'],
  ['powerStage', PT.POWER_STAGE, null, 'Off, one valve into a transformer, or a pair into a centre-tapped one.'],
  ['powerValve', PT.POWER_VALVE, null, 'EL34, 6L6GC, KT88 or 300B, each in its own typical circuit.'],
  ['master', PT.MASTER, (v) => `× ${MasterGain(v).toPrecision(3)}`, 'Volts on the power grids per volt of preamp swing: the phase inverter and the master pot.'],
  ['powerBias', PT.POWER_BIAS, (v) => percent(IdleFraction(v)), 'Idle dissipation, 5 % to 100 % of the valve’s rating. 70 % is where a tech sets it; cold gives crossover.'],

  ['cStages', PT.C_STAGES, null, 'The chroma’s own amplifier, used in Y/C only.'],
  ['cPreamp', PT.C_PREAMP, null, null],
  ['cDrive', PT.C_DRIVE, (v) => volts(DriveVolts(v)), null],
  ['cBias', PT.C_BIAS, (v) => percent(BiasFraction(v)), null],
  ['cPowerStage', PT.C_POWER_STAGE, null, null],
  ['cPowerValve', PT.C_POWER_VALVE, null, null],
  ['cMaster', PT.C_MASTER, (v) => `× ${MasterGain(v).toPrecision(3)}`, null],
  ['cPowerBias', PT.C_POWER_BIAS, (v) => percent(IdleFraction(v)), null],

  ['output', PT.OUTPUT, null, 'Fit: black stays black and white stays white. Unity: the operating point’s small-signal gain is 1, so drive compresses. Plate: the plate’s own volts over its supply.'],
  ['polarity', PT.POLARITY, null, 'As Wired shows the inversions: one stage is a negative.'],
  ['showCurve', PT.SHOW_CURVE, null, 'The transfer curve in the corner: input across, output up.'],
  ['mix', PT.MIX, (v) => unit(v).toFixed(2), 'The amplified picture against the clip.'],
];

const DECLARATIONS = Declarations();
const PARAMS = SPEC.map(([id, slot, readout, hint]) => {
  const [name, group, kind, elements] = DECLARATIONS[slot];
  const p = { id, slot, name, group, hint: hint ?? undefined, default: kDefaults[slot] };
  if (kind === 'option' || kind === 'integer') Object.assign(p, { type: 'option', elements });
  else if (kind === 'boolean') p.type = 'boolean';
  else Object.assign(p, { type: 'standard', display: readout });
  return p;
});
if (PARAMS.length !== PT.COUNT || PARAMS.some((p, i) => p.slot !== i)) throw new Error('the panel is not the declaration order');

// The README's curves figure, as whole settings. Slider values are host 0..1.
const slider = { drive: (v) => DriveSlider(v), master: (g) => MasterSlider(g), idle: (f) => IdleSlider(f) };
const PRESETS = {
  'One 12AU7, Drive 1.2 V (clean)': { stages: 1, preamp: 2, powerStage: 0, showCurve: 1 },
  'One 12AX7, Drive 1.2 V': { stages: 1, preamp: 0, powerStage: 0, showCurve: 1 },
  'Four 12AX7s into a pair (fuzz)': { stages: 4, showCurve: 1 },
  'Cold EL34 pair: crossover': { stages: 0, drive: slider.drive(60), powerBias: slider.idle(0.05), rest: 0.5, showCurve: 1 },
  'Y/C: hue kept, chroma compressed': { signal: 1, cStages: 2, cPreamp: 0, cDrive: slider.drive(3), showCurve: 1 },
  'Composite: differential gain': { signal: 2, showCurve: 1 },
  'Mismatch: three valve sets': { mismatch: 1, showCurve: 1 },
  'Plate volts, as wired': { output: 2, polarity: 1, showCurve: 1 },
};

let renderer = null;

const mounted = mountDemo({
  name: 'Valve',
  pluginId: 'VA01',
  kind: 'effect',
  tagline:
    'Your picture through a valve amplifier, run like a guitar through an overdriven amp: shadows crushed, highlights hot, colour saturated. Nobody draws the curve. Each stage is a real valve (Koren’s models of the 12AX7, 12AT7, 12AU7 and 6DJ8; EL34, 6L6GC, KT88 and 300B) solved on its load line, chained like a guitar amp, and fed as RGB, as Y/C with the chroma on a subcarrier, or as Composite.',
  repo: 'https://github.com/stoatworks-labs/valve',
  page: 'https://stoatworks-labs.com/software/valve/',
  video: 'https://www.youtube.com/watch?v=DM2kHbvMlJw',

  blurb:
    'It is Valve’s own two GLSL passes, ported from the repository to WebGL2 unedited, reading valve curves solved in this page by a JavaScript port of the plugin’s CPU half — Koren’s valves, the load lines, the operating points and the normalisation — which a check in the repository holds to the plugin’s C++, table for table and uniform for uniform. It runs on generated clips or your own. The plugin has no audio input, so nothing is left out for want of one.',

  // Premultiplied alpha in and out; the shape-on-transparency clip shows it.
  showBackdrop: true,
  needFloat: true,

  // The scene for footage; the ramps make the curve readable in the picture;
  // the bars show RGB turning hue where Y/C keeps it; lights on black is the
  // dark clip Rest Level was added for.
  sources: ['scene', 'ramp', 'bars', 'bars100', 'spot', 'grid', 'alpha'],

  params: PARAMS,
  presets: PRESETS,

  differences: [
    'The CPU half is a PORT, not the plugin’s code. The plugin solves each valve stage’s curve on the CPU in double whenever a setting it depends on moves (Valves.cpp: Koren’s law, the operating points, the preamp, single-ended and push-pull load lines; Chain.cpp: the warped 4,097-node tables, solved outward from rest), and normalises the chain there every frame. Here all of that is JavaScript, whose numbers are doubles too. A check in the repository drives the real plugin class in a GL context through a scenario of settings and requires this page’s port to print the same thing: every table, every operating point, the normalisation and every uniform of both passes, read back out of the plugin’s own programs. With the plugin’s libm results replayed into the port, all 36,611 lines it prints over 102 settings are identical to the bit. On this page’s own Math, which differs from Apple’s libm in the last bit of a few results in a hundred, everything the GPU reads (every table float, every warp, every uniform) is still identical, to the plugin built without fused multiply-adds (its Intel slice’s arithmetic) and as built for Apple Silicon alike; only doubles that never reach the GPU drift, by a few units in the last place, or a few thousand where a Newton solve stops at its tolerance of one part in 10¹³.',
    'The GPU half is not a port: the describing-function pass and the display pass are the plugin’s own GLSL, reading the same 4,097 × 18 R32F table and 256 × 65 RGBA32F buffer by texelFetch. demo/tools/check_shaders.py fails the repository’s verify script if a character of them, or the way they are joined, drifts. WebGL2 needs EXT_color_buffer_float to render the describing functions, and a texture 4,097 texels wide; without either the page stops and says so. Frames drawn here were compared once with the plugin’s own harness on the same input at the same settings (320 × 180): of thirteen cases across RGB, Y/C and Composite, Fit, Unity and Plate, both polarities, Mismatch, a cold pair, Show Curve and Mix, twelve were identical in every pixel and the thirteenth in all but one (a premultiplied alpha edge, one level of 255), in headless Chrome’s software renderer; four of them run again on an Apple M4 Max through ANGLE’s Metal renderer were identical too. Other GPUs and browsers were not tried.',
    'Re-solving the tables when a valve, bias, idle or mismatch setting moves happens in this page, as the plugin does it in ProcessOpenGL. The plugin takes about 10 ms for a whole chain; this page took 11 to 22 ms for the defaults’ four tables on an M4 Max, and the line under the picture says how long the last solve took on yours. Drive, Master, Interstage, Stages and Rest Level cost nothing, as in the plugin.',
    'Stages and Chroma Stages are FF_TYPE_INTEGER, 0 to 4: here they are dropdowns of their five values. The About block is not on the panel. The Presets menu is this page’s; the plugin has none.',
    'The harness’s wrong models (no grid current, an undriven half, split load lines, baseband chroma, the carrier round the valve, a 12AX7 with μ = 10) are not on this page: they exist so the harness can show its checks fail, and the plugin never uses them in a host.',
    'The model is memoryless, in the plugin and here: no Miller smear, no coupling-capacitor blocking, no supply sag, no output transformer, no tone stack beyond Interstage’s flat loss.',
    'The plugin’s harness holds each valve’s gain to small-signal theory to five figures, the grid-current knee to r_g/(r_g + R_s), the harmonics to Taylor, a matched pair’s cancelled second harmonic, the crossover notch and what happens to hue, with eight deliberately wrong models and six mutants all caught. That harness, not this page, is the reason to believe the curves. The plugin has passed the fleet’s Arena gate on Windows; it has never been loaded into Resolume on macOS.',
  ],

  createRenderer: (gl, quad) => {
    renderer = createRenderer(gl, quad);
    return renderer;
  },
});

//---------------------------------------------------------------------------
// A status line: reports what the port computed, measures nothing.
//---------------------------------------------------------------------------
const query = new URLSearchParams(window.location.search);
const embed = query.has('embed') && query.get('embed') !== '0';

if (mounted && !embed) {
  const stage = document.querySelector('.stage');
  if (stage) {
    const line = document.createElement('p');
    line.className = 'stage__status';
    line.id = 'valve-stats';
    line.setAttribute('aria-live', 'off');
    stage.append(line);
    const fmt = (x, digits = 3) => (Math.abs(x) >= 100 ? x.toFixed(1) : x.toPrecision(digits)).replace('-', '−');
    setInterval(() => {
      const s = renderer?.stats;
      if (!s) return;
      const main = s.settings.chain[kMain];
      const parts = [];
      if (main.stages > 0 && s.q) {
        const q = s.q;
        const gain = -q.gm * q.rout;
        parts.push(`${kPreampNames[main.preamp]} at rest: grid ${fmt(q.eg)} V, plate ${fmt(q.vp)} V, ${fmt(q.ip * 1000)} mA, stage gain ${fmt(gain)}`);
      }
      if (s.pq) {
        const what = main.power === PowerStage.PushPull ? 'pair' : 'single-ended';
        parts.push(`${kPowerNames[main.powerValve]} ${what}: grid bias ${fmt(s.pq.bias)} V, idle ${fmt(s.pq.iq * 1000)} mA`);
      }
      parts.push(`last table solve here: ${renderer.solvedRows} row${renderer.solvedRows === 1 ? '' : 's'} in ${Math.round(renderer.solveMs)} ms`);
      line.textContent = parts.join(' · ');
    }, 250);
  }
}

// For the headless checks in AGENTS.md: the page's pieces, read-only by convention.
window.valveDemo = { mounted, get renderer() { return renderer; }, PARAMS, PT, Frame, Tables, kDefaults };
