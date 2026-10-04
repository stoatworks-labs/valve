# AGENTS.md — valve

Built 2026-10-04 from Allan's request: "a new resolume plugin which models passing
a video signal through a vacuum tube, it should contain a couple of models of tube,
and the ability to treat the signal as chroma + Luma or RGB, and apply multiple
tubes to each, like running the video signal through a guitar amplifier adding
overdrive/saturation/distortion". Spec: `~/Projects/resolume/specs/SPEC-valve.md`.
Logged in `~/Projects/resolume/IDEAS.md` under "In progress, started 2026-10-04".

## The idea

A valve's transfer curve is the valve's plate-current law meeting the circuit's
load line. Everything else is consequence: gain μR_L/(R_L + r_p), soft cutoff
against a hard grid-current knee, inversion per stage, even harmonics from one
valve and their cancellation in a pair, crossover at cold bias, hue kept by a
subcarrier and moved by RGB, differential gain in Composite. The plugin solves
the curves; the harness measures the consequences out of the picture.

## Decisions taken (no one was asked; each can be revisited)

- **Name `SW Valve`, id `VA01`** — "valve" is the British word for the tube; both
  free in the fleet on 2026-10-04.
- **One source for every valve: Koren's `Tube.lib`.** It has eight that matter
  for a guitar amp: four preamp triodes and three power pentodes plus the 300B.
  No EL84 or 6V6: Koren never published them, and a second source would mean a
  second fitting method. The article's Table 1 and the 2001 library disagree on
  the 6550's kG2 (4800 / 4200); the 6550 is not used.
- **The grid's diode is ideal**: Koren's RGI, conducting from exactly 0 V. Real
  grid current starts a fraction of a volt early (contact potential). Ideal makes
  the knee a sharp, checkable ratio.
- **Drive is absolute volts, not normalised to each valve's headroom.** So
  swapping a 12AX7 for a 12AU7 at the same Drive cleans the picture up, as it
  does in an amplifier. The cost: the Drive range is four and a half decades.
- **Memoryless.** The picture is a colour transform with physics in it, not a
  scan-order signal: no Miller smear, no coupling-capacitor blocking, no sag, no
  transformer. Each is a mechanism of its own and a check of its own; see "Not
  done". Real values would also be brutal: a 12AX7 into another 12AX7 has a
  Miller pole near 40 kHz, which would smear 8 % of a PAL line per time constant
  and take a 4.43 MHz chroma carrier out entirely.
- **Chroma as a subcarrier, by its describing function.** For a memoryless chain
  the fundamental of g(A cos θ) has the input's phase, so Y/C scales (U, V) and
  never rotates them. Composite looks up the mean and the fundamental of
  f(Y + A cos θ) in a 256 × 64 table the GPU fills each frame (64 phases, cosines
  from the CPU).
- **Rest Level** (added after rendering Resolume's dark demo clips): with mid-grey
  pinned to the valves' rest, a dark clip lives in cutoff and the defaults
  rendered it nearly black. A real video amplifier's black-level clamp chooses
  where the signal sits; the control is that choice. Default 0.3.
- **Defaults**: two 12AX7 stages, −20 dB between them, into an EL34 pair at 70 %
  idle, Master 0.3, Drive 1.2 V, Rest 0.3, Fit. Picked by eye on the card and on
  `IntoTheGlow_02`; nothing measures "a pleasing default".
- **Mismatch pattern is fixed**: R, G, B valve sets at kG1 × (1 + m, 1, 1 − m),
  pair halves × (1 ± m). There is no "retube" seed.
- **Power stage circuit values** (B+, R_aa, rated watts) are typical, not one
  amplifier's: EL34 450 V / 3.4 k / 25 W, 6L6GC 450 V / 4 k / 30 W, KT88 500 V /
  4 k / 35 W, 300B 400 V / 5 k / 36 W. The single-ended load is B+²/(0.7 P_max),
  the class-A load for the default idle, and does not change when rebiased.
- **The chroma chain has a power stage too.** Symmetry with the luma chain,
  as asked ("multiple tubes to each"); a power pentode amplifying a subcarrier is
  not a thing anyone built.

## Shape of the code

(Interpolation is linear in the warped INDEX, not in volts: any bound on a
lookup's error takes each segment's midpoint in u. `interpBound` does.)


    source/Valves.{h,cpp}   Koren's table and law, the operating points, the three
                            stage solvers (preamp, single-ended, push-pull). Double.
    source/Chain.{h,cpp}    the warped tables, Tables::Update (re-solve only what a
                            setting touches), Evaluate (the shader's chain on the
                            CPU), Normalise.
    source/Shaders.cpp      the library (lookup, chainOut), the describing-function
                            pass, the display pass with Show Curve.
    source/Valve.{h,cpp}    the plugin: parameters, uploads, two passes.
    source/Controls.*       0..1 host parameters to physical units.
    tools/vatest/           the harness. It restates Koren, the circuit and the
                            control laws itself and never reads them from the plugin;
                            it reads only the tables' geometry, for its bounds.

## The traps actually hit

- **An index above 2048 rounds twice as coarsely as one below** (float ULP 2^-12
  against 2^-13), so an odd stage looked up at ±d is not exactly odd on the GPU.
  That is the floor `--harmonics` holds a matched pair's H2 under (6e-6; measured
  4e-8).
- **Koren's LOG(1 + EXP(z)), written as he writes it, loses the 1's rounding**
  against a small softplus. The plugin uses log1p; the harness's restatement
  keeps Koren's form, so `--model` holds them together to a bound derived from that
  rounding (X·1.1e-16/L), not to a flat 1e-12 that failed at 1.8e-11 near cutoff.
- **An EL34 at 450 V idling at its full rating is a few volts above Koren's
  cutoff knee**, where gm is still convex: one valve driven hard out-steepens the
  idling pair. The first `--crossover` claimed a hot pair "peaks at the centre"
  and failed; it was the claim that was wrong. The check now measures the notch's
  depth (centre slope over its neighbours') at five biases against the model, and
  that the notch fills monotonically.
- **A tolerance as big as the thing measured passes anything.** At 0.1 V of grid
  swing, H3/H1 was 5.7e-5 with a tolerance of 8.5e-5. Drive went to 0.3 V and
  `--harmonics` now refuses any tolerance over a quarter of its value.
- **A units slip made `--dg` 100× looser than intended** (an extra 1/Drive in the
  table term). After the fix its tolerance is dominated by a conservative float
  term — 64 ULPs of 1.0 per channel over a 0.01 carrier — and the measured error
  is ~1 % of it; the check is still 300× tighter than the 48 % effect, and the
  bypass control fails it 241×.
- **A wall clock over 60 identical frames measured 36 µs at 4K**, which no GPU can
  shade; the driver was overlapping or eliding them. `--bench` now times each
  frame with a GL_TIME_ELAPSED query and reads a pixel back between frames.
- **An unbound sampler is a warning on every frame in Apple's GL** ("unit 2 ...
  unloadable"). The describing-function buffer is allocated in every mode (66 KB)
  so the display's `Tables` sampler always has a real texture.
- **Show Curve draws over its own border**, so a curve crushed to black lights row
  0. `--curve` first ignored the border rows and reported 56 px of disagreement.
- **Re-solving the tables cost 28 ms**, a visible stall while dragging Bias. Each
  node's Newton now starts from its neighbour's answer, solving outward from rest:
  9.6 ms for a whole chain, ~3 ms a row, and only the rows a setting touches.
- **This shell's `grep` and `head` are ugrep** in the interactive profile; scripts
  under `/bin/sh` are unaffected. Use `/usr/bin/grep` in one-off commands.
- mutate.sh needs each target to occur exactly once: the lerp `l + t * ( r - l )`
  is in two functions, so the GLSL lookup mutant is the `2048.0` instead.

## Verified (2026-10-04, M4 Max, macOS 26.4.1)

`tools/verify.sh` passes: see README "Status" for the numbers. In short: gains to
five figures against small-signal theory for 4 preamp and 8 power configurations;
the knee ratio and position; H2/H1 and H3/H1 against Taylor; matched-pair H2
cancellation and mismatch linearity; crossover's centre slope and notch depth;
Y/C hue and describing function; Composite DG and luma shift; polarity; identity;
the GPU against the CPU's run of the tables, and the tables against the harness's
independent valves; the overlay; the model offline; 8 negative controls; 6
mutants; 23 live controls; universal bundle; oxbow probe and selftest.

### Mutation record (tools/mutate.sh)

| mutant | caught by |
| --- | --- |
| GLSL `2048.0` → `2047.0` in the lookup | `--reference` |
| GLSL `y + c.x / kU` → `y - c.x / kU` (U back into blue) | `--hue` |
| GLSL the Composite table's upper-row lerp `a11 - a01` → `a11 + a01` | `--dg` |
| C++ push-pull `c1.ip - c2.ip` → `c1.ip + c2.ip` | `--crossover` |
| C++ grid divider `rgi + source` → `rgi - source` | `--knee` |
| C++ the knee alignment `knee / sinh` → `knee * sinh` | `--model` |

## Would this hold on another rasteriser, at another raster?

Every lookup is `texelFetch` at integer coordinates with the interpolation in
float in the shader; no check depends on where a rasteriser's interpolated uv
lands or on a texture unit's filtering. Each tolerance is derived, and every
rendered check runs at 320×180 and 1280×720 and on Apple's software renderer.

- `--gain`: central difference over ±(m + ½) px, m chosen so h ≈ 1 % of the swing
  at any width; tolerance = the model's own curvature over that h + the tables'
  interpolation (from the harness's valves) + the float walked per lookup.
  Raster-independent by construction.
- `--knee`: the knee is put between two pixel centres at ¾ of any width divisible
  by 4; one-pixel slopes, so the curvature term scales with 1/W and is computed,
  not fitted. Holds at both rasters (0.0285 at both).
- `--harmonics`: 40 px a cycle at any width divisible by 40, so the DFT sees the
  same samples per cycle at every raster. The matched-pair floor is the index's
  float rounding (2^-12) times the largest node step, a property of float, not of
  a GPU.
- `--crossover`: one-pixel centre slope and twentieth-of-the-ramp neighbours,
  both against the model at the same pixel positions.
- `--hue`, `--dg`, `--polarity`: patch centres, far from any edge; the hue bound
  is 64 float ULPs per channel through the colour conversions; the amplitude
  bounds are the model's quadrature (64 vs 4096 taps), its 256-node table and the
  stage tables.
- `--identity`: float ULP bounds; the 64-tap sums in Y/C and Composite carry the
  float cosines' own rounding (Σcos ≠ 0 exactly), counted as 96 ULPs.
- `--reference`: a per-pixel bound walked lookup by lookup from the GLSL spec's
  allowance for log (counted generously as 2^-20 (1 + |u|)) and the index's float
  rounding; the tables against the model with the interpolation bound carried
  through each later stage's slope.
- `--curve`: integer box geometry from the viewport; the lit span compared to
  ±1 px (measured 0).
- Not yet run on Mesa llvmpipe (the fleet's Arena gate) or any non-Apple GPU.
  GLSL 4.10 reserved words are grepped for; glslc compiles all three shaders.

## Assumed, not verified

- That Resolume hands FFGL premultiplied colour (the fleet's finding on DXV clips;
  the plugin un-premultiplies and re-premultiplies).
- That Koren's fits are close enough to real valves for the look; they are fits to
  published curves, a few per cent off in places (his 12AX7 gives 0.95 mA at 250 V,
  −2 V where RCA's sheet says 1.2 mA).
- That a 10 ms table solve while dragging Bias is acceptable in Arena.
- That 64 phase taps are enough: for a chain driven into hard clipping, harmonics
  above the 63rd alias into the describing function. `--hue` bounds the 64-tap
  error against 4096 taps at its drive; a fuzz-level chroma chain is not bounded.

## Not done

- Never loaded into Resolume on macOS.
- No OpenFX port and no browser demo. (Released as v0.1.0 on 2026-10-04 with a user
  guide; `StoatworksAbout.h` and `ATTRIBUTIONS.md` are generated by the backend now.)
- The time-domain mechanisms, each the obvious next version: the **Miller pole**
  (C_in = C_gk + (1 + A) C_gp: a 12AX7 smears to the right, a 12AU7 barely, and
  colour dies first — a blocked linear scan per stage, as clamp does its RC);
  **blocking** (grid current charging the coupling capacitor: a bright object
  darkens what follows it, recovering over the grid leak's time constant);
  **supply sag** with APL; the **output transformer**; a **tone stack** between
  stages (a horizontal EQ).

## Open design questions

- Should the chroma chain lose its power stage (eight controls instead of six)?
- Should Drive be relative to the first valve's headroom, at the cost of the
  12AU7 swap?
- Is a per-composition "retube" (seeded mismatch pattern) wanted?
- Default Rest Level 0.3 was chosen on one dark clip and the card.
