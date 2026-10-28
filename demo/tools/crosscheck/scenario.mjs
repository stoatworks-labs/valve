// The cross-check's scenario: a sequence of host settings, each the 24 floats a
// host hands the plugin, printed as their bits. printer.cpp sets them on the
// real plugin class and renders a frame; run.mjs runs the port's Frame over the
// same. The ORDER matters as much as the settings: Tables::Update re-solves only
// the rows a setting touched, so the sequence walks every control from the
// constructor's defaults and back, and then through random settings, and a port
// that re-solved too little would carry a stale row into the next step's print.
//
//     node demo/tools/crosscheck/scenario.mjs > scenario.txt
//
// Kept small on purpose: every row solved is ~100,000 libm calls to record and
// replay. Slider values are rounded to float, as a host stores them.

import { readFileSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';

const here = dirname(fileURLToPath(import.meta.url));
const source = readFileSync(join(here, '..', '..', 'plugin.js'), 'utf8');
const begin = source.indexOf('// ==== BEGIN PORT');
const end = source.indexOf('// ==== END PORT');
// eslint-disable-next-line no-new-func
const P = new Function(`${source.slice(begin, end)}\nreturn { Defaults, PT };`)();
const { PT } = P;

const out = [];
const view = new DataView(new ArrayBuffer(4));
const hex = (v) => { view.setFloat32(0, v); return view.getUint32(0).toString(16).padStart(8, '0'); };

let host = P.Defaults();
const size = (w, h) => out.push(`size ${w} ${h}`);
const emit = () => out.push(`set ${[...host].map(hex).join(' ')}`);
const set = (changes) => { for (const [k, v] of Object.entries(changes)) host[PT[k]] = v; emit(); };
const reset = () => { host = P.Defaults(); };

size(64, 36);
emit();

// Every option of every menu, one at a time.
for (const v of [1, 2, 0]) set({ SIGNAL: v });
for (const v of [1, 2, 3, 0]) set({ PREAMP: v });
for (const v of [0, 1, 3, 4, 2]) set({ STAGES: v });
for (const v of [0, 1, 2]) set({ POWER_STAGE: v });
for (const v of [1, 2, 3, 0]) set({ POWER_VALVE: v });
set({ POWER_STAGE: 1, POWER_VALVE: 3 });
set({ POWER_STAGE: 2, POWER_VALVE: 0 });
for (const output of [1, 2, 0]) for (const polarity of [1, 0]) for (const signal of [0, 1, 2]) set({ OUTPUT: output, POLARITY: polarity, SIGNAL: signal });

// Sliders across their range.
for (const v of [0, 0.25, 0.77, 1]) set({ DRIVE: v });
reset(); emit();
for (const v of [0, 0.3, 1]) set({ BIAS: v });
reset(); emit();
for (const v of [0, 1, 0.6]) set({ MASTER: v });
for (const v of [0, 0.4, 1]) set({ POWER_BIAS: v });
reset(); emit();
for (const v of [0, 1]) set({ INTERSTAGE: v });
for (const v of [0, 1, 0.5]) set({ REST: v });
for (const v of [0.3, 1]) set({ MISMATCH: v });
set({ POWER_STAGE: 1 });
set({ SIGNAL: 1 });
reset(); emit();
for (const v of [0, 0.5]) set({ MIX: v });
set({ SHOW_CURVE: 1 });
reset(); emit();

// The chroma chain, in Y/C.
set({ SIGNAL: 1 });
for (const v of [0, 2, 4]) set({ C_STAGES: v });
for (const v of [0, 3]) set({ C_PREAMP: v });
for (const v of [0, 1]) set({ C_DRIVE: v });
set({ C_BIAS: 0.1 });
for (const v of [1, 2]) set({ C_POWER_STAGE: v });
set({ C_POWER_VALVE: 2 });
set({ C_MASTER: 0.8, C_POWER_BIAS: 0.2 });
set({ MISMATCH: 0.6 });
for (const output of [1, 2]) for (const polarity of [0, 1]) set({ OUTPUT: output, POLARITY: polarity });

// No stages at all, into each power stage and none.
reset();
set({ STAGES: 0, POWER_STAGE: 0 });
set({ STAGES: 0, POWER_STAGE: 2, DRIVE: 0.85, POWER_BIAS: 0 });
set({ STAGES: 0, POWER_STAGE: 1, DRIVE: 0.85 });

// Another raster, and random settings (a fixed seed).
size(320, 180);
reset(); emit();
let seed = 0x5eed1e55;
const random = () => {
  seed = (seed + 0x6d2b79f5) >>> 0;
  let t = seed;
  t = Math.imul(t ^ (t >>> 15), t | 1);
  t ^= t + Math.imul(t ^ (t >>> 7), t | 61);
  return ((t ^ (t >>> 14)) >>> 0) / 4294967296;
};
const pick = (n) => Math.floor(random() * n);
for (let i = 0; i < 10; ++i) {
  host = P.Defaults();
  host[PT.SIGNAL] = pick(3);
  for (const k of ['REST', 'INTERSTAGE', 'DRIVE', 'BIAS', 'MASTER', 'POWER_BIAS', 'C_DRIVE', 'C_BIAS', 'C_MASTER', 'C_POWER_BIAS', 'MIX']) host[PT[k]] = random();
  host[PT.MISMATCH] = i % 3 === 0 ? random() : 0;
  for (const [k, n] of [['STAGES', 5], ['PREAMP', 4], ['POWER_STAGE', 3], ['POWER_VALVE', 4], ['C_STAGES', 5], ['C_PREAMP', 4], ['C_POWER_STAGE', 3], ['C_POWER_VALVE', 4], ['OUTPUT', 3], ['POLARITY', 2], ['SHOW_CURVE', 2]]) host[PT[k]] = pick(n);
  emit();
}

process.stdout.write(`${out.join('\n')}\n`);
