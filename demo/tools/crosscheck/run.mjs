// The JavaScript half of the browser demo's cross-check: the PORT block of
// demo/plugin.js (between its BEGIN PORT and END PORT markers, sliced out as
// text and run here, so what is checked is exactly what the page runs), driven
// through scenario.mjs's settings and printed the way printer.cpp prints the
// real plugin class. crosscheck.sh compares the two. Read this beside
// printer.cpp: each block mirrors one there.
//
//     node demo/tools/crosscheck/run.mjs < scenario.txt > js.txt
//
// LIBM_REPLAY=calls.bin answers the port's libm calls from the plugin's own
// (see replay.mjs); without it the port runs on V8's Math, as the page does.
// A summary line goes to stderr either way.

import { readFileSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { loadReplay, replayMath } from './replay.mjs';

const here = dirname(fileURLToPath(import.meta.url));
const source = readFileSync(join(here, '..', '..', 'plugin.js'), 'utf8');
const begin = source.indexOf('// ==== BEGIN PORT');
const end = source.indexOf('// ==== END PORT');
if (begin < 0 || end < begin) {
  process.stderr.write('run.mjs: the PORT markers are missing from demo/plugin.js\n');
  process.exit(2);
}
const names = ['Tables', 'Frame', 'Defaults', 'Declarations', 'PT', 'Slope', 'ChromaReach', 'kRows', 'kChains', 'kChannels', 'PowerStage'];
const replay = process.env.LIBM_REPLAY ? loadReplay(process.env.LIBM_REPLAY) : null;
const { math, misses, counter, compare } = replayMath(replay);
// eslint-disable-next-line no-new-func
const P = new Function('Math', `${source.slice(begin, end)}\nreturn { ${names.join(', ')} };`)(math);

const lines = [];
const print = (line) => lines.push(line);

const view = new DataView(new ArrayBuffer(8));
const h32 = (v) => (v >>> 0).toString(16).padStart(8, '0');
const f = (v) => { view.setFloat32(0, v); return h32(view.getUint32(0)); };
const d = (v) => { view.setFloat64(0, v); return h32(view.getUint32(0)) + h32(view.getUint32(4)); };
const bits = (v) => { view.setFloat32(0, v); return view.getUint32(0); };
const fnv = (h, word) => Math.imul((h ^ word) >>> 0, 16777619) >>> 0;

const FF = { standard: 10, option: 11, integer: 13, boolean: 0 };

// What the constructor declared.
const defaults = P.Defaults();
P.Declarations().forEach(([name, group, kind, elements], i) => {
  let field = kind === 'option' ? elements.join(',') : '';
  if (kind === 'integer') field += `range ${f(0)}..${f(elements.length - 1)}`;
  print(`decl ${i} ${name}|${group}|${FF[kind]}|${field}|${f(defaults[i])}`);
});

const lastHash = new Array(P.kRows).fill(0);
const lastBuilt = new Array(P.kRows).fill(false);

function uniform(pass, name, value, kind) {
  let line = `u ${pass} ${name}`;
  const values = typeof value === 'number' ? [value] : [...value];
  for (const v of values) line += ` ${kind === 'int' ? String(v) : f(v)}`;
  print(line);
}

function library(pass, u) {
  uniform(pass, 'WarpOf', u.WarpOf, 'float');
  uniform(pass, 'RowOf', u.RowOf, 'int');
  uniform(pass, 'StagesOf', u.StagesOf, 'int');
  uniform(pass, 'PowerOf', u.PowerOf, 'int');
  uniform(pass, 'DriveOf', u.DriveOf, 'float');
  uniform(pass, 'MasterOf', u.MasterOf, 'float');
  uniform(pass, 'VgqOf', u.VgqOf, 'float');
  uniform(pass, 'Interstage', u.Interstage, 'float');
  uniform(pass, 'Rest', u.Rest, 'float');
}

function printState(t, frame) {
  const s = frame.settings;
  for (let c = 0; c < P.kChains; ++c) {
    const cs = s.chain[c];
    print(`s ${c} stages ${cs.stages} preamp ${cs.preamp} drive ${d(cs.drive)} bias ${d(cs.bias)} power ${cs.power} valve ${cs.powerValve} master ${d(cs.master)} idle ${d(cs.idle)}`);
  }
  print(`s interstage ${d(s.interstage)} mismatch ${d(s.mismatch)} rest ${d(s.rest)} perChannel ${s.perChannel ? 1 : 0}`);

  print(`vgq ${d(t.Vgq(0))} ${d(t.Vgq(1))}`);
  for (let c = 0; c < P.kChains; ++c) {
    const split = c === 0 && s.perChannel;
    for (let ch = split ? 0 : 1; ch <= (split ? 2 : 1); ++ch) {
      const q = t.Quiescent(c, ch);
      print(`q ${c} ${ch} ${d(q.eg)} ${d(q.vp)} ${d(q.ip)} ${d(q.gm)} ${d(q.rp)} ${d(q.mu)} ${d(q.rout)}`);
      if (s.chain[c].power !== P.PowerStage.Off) {
        const pq = t.PowerQuiescent(c, ch);
        print(`pq ${c} ${ch} ${d(pq.bias)} ${d(pq.iq)} ${d(pq.seLoad)} ${d(pq.vhq)}`);
      }
    }
  }

  for (let r = 0; r < P.kRows; ++r) {
    const row = t.Row(r);
    let h = 2166136261;
    for (const v of row.values) h = fnv(h, bits(v));
    const wf = t.RowWarp(r);
    print(`row ${r} built ${row.built ? 1 : 0} warp ${d(row.warp.c)} ${d(row.warp.w)} ${d(row.warp.du)} wf ${f(wf.c)} ${f(wf.w)} ${f(wf.invDu)} n ${row.values.length} hash ${h32(h)}`);
    if (row.built && (!lastBuilt[r] || lastHash[r] !== h)) {
      for (let i = 0; i < row.values.length; i += 16) {
        let line = `data ${r} ${i}`;
        for (let k = i; k < Math.min(i + 16, row.values.length); ++k) line += ` ${f(row.values[k])}`;
        print(line);
      }
    }
    lastBuilt[r] = row.built;
    lastHash[r] = h;
  }

  const n = frame.norm;
  print(`norm ${d(n.offset)} ${d(n.ref[0])} ${d(n.ref[1])} ${d(n.ref[2])} ${d(n.scale[0])} ${d(n.scale[1])} ${d(n.scale[2])} ${d(n.chromaScale)}`);

  const rest = s.rest;
  for (let ch = 0; ch < P.kChannels; ++ch) {
    for (const x of [-rest, -0.1, 0.0, 0.05, 1.0 - rest]) print(`eval 0 ${ch} ${d(x)} ${d(t.Evaluate(0, ch, x))}`);
  }
  for (const x of [-0.2, 0.0, 0.2]) print(`eval 1 1 ${d(x)} ${d(t.Evaluate(1, 1, x))}`);
  for (const a of [0.1, P.ChromaReach()]) {
    const dc = [0];
    const one = t.Fundamental(1, 1, 0.0, a, dc);
    print(`fund ${d(a)} ${d(one)} ${d(dc[0])}`);
  }
  print(`slope ${d(P.Slope(t, 0, 1))} ${d(P.Slope(t, 1, 1))}`);

  if (frame.tables) {
    const u = frame.tables;
    library('tables', u);
    uniform('tables', 'Lut', u.Lut, 'int');
    uniform('tables', 'Cosines', u.Cosines, 'float');
    uniform('tables', 'Reach', u.Reach, 'float');
  }
  const u = frame.display;
  library('display', u);
  for (const name of ['InputTexture', 'Lut', 'Tables', 'InWidth', 'InHeight', 'VpX', 'VpY', 'VpW', 'VpH', 'Mode']) uniform('display', name, u[name], 'int');
  uniform('display', 'Offset', u.Offset, 'float');
  uniform('display', 'Ref', u.Ref, 'float');
  uniform('display', 'Scale', u.Scale, 'float');
  for (const name of ['ChromaScale', 'Reach', 'CarrierGain']) uniform('display', name, u[name], 'float');
  uniform('display', 'Perturb', u.Perturb, 'int');
  uniform('display', 'MixAmount', u.MixAmount, 'float');
  uniform('display', 'ShowCurve', u.ShowCurve, 'int');
}

const scenario = readFileSync(0, 'utf8').split('\n');
let tables = null;
let width = 0;
let height = 0;
let step = 0;
const host = new Float32Array(P.PT.COUNT);
for (const text of scenario) {
  const words = text.trim().split(/\s+/);
  if (words[0] === 'size') {
    width = Number(words[1]);
    height = Number(words[2]);
    // InitGL starts from empty tables: every row is solved again.
    tables = new P.Tables();
    print(`size ${width} ${height}`);
  } else if (words[0] === 'set') {
    for (let i = 0; i < P.PT.COUNT; ++i) {
      view.setUint32(0, parseInt(words[1 + i], 16));
      host[i] = view.getFloat32(0);
    }
    const frame = P.Frame(tables, host, width, height, [0, 0, width, height]);
    print(`step ${step++}`);
    printState(tables, frame);
  }
}

process.stdout.write(`${lines.join('\n')}\n`);
const summary = { replayed: counter.replayed, misses };
if (replay) {
  summary.recorded = replay.calls;
  summary.libm = compare();
}
process.stderr.write(`${JSON.stringify(summary)}\n`);
