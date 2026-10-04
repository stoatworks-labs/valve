# Attributions

Valve is built on other people's work. This file lists what that work is, who did
it, and what it is doing here.

It is a provisional hand copy written before this repo was registered in the fleet;
registration replaces it with the generated file (`scripts/sync-attributions.py` in
the `stoatworks-backend` repo, `--adopt` once, since this is a hand copy).

## The idea and the valves

### Vacuum-tube models — Norman L. Koren, 1996

"Improved vacuum tube models for SPICE simulations", *Glass Audio* vol. 8 no. 5
(1996), p. 18, and its web edition (normankoren.com/Audio/Tubemodspice_article.html,
updated 2003), with his model library `Tube.lib` from
<https://www.normankoren.com/Audio/Tubemods.zip> (2001). Every valve in the plugin --
the 12AX7, 12AT7, 12AU7 and 6DJ8 triodes, the EL34, 6L6GC and KT88 pentodes and the
300B -- is Koren's plate-current equation with Koren's parameters, typed from
`Tube.lib` and checked against a second typing in `vatest --model`. His grid-current
resistance RGI is used with the diode made ideal. Koren credits the small-signal
triode data to Tom Mitchell's *The Audio Designer's Tube Register* and the EL34 data
to Audiomatica's Sofia page. Read on 2026-10-04.

### The circuits

The common-cathode preamp stage (300 V, 100 k plate load, bypassed cathode, 1 M grid
leak, 68 k input stopper), the push-pull output stage on a centre-tapped transformer,
the "bias to 70 % of rated dissipation" rule and the typical supplies and loads are
the guitar-amplifier and valve-audio literature's common practice, not any one
design. The power valves' rated dissipations (EL34 25 W, 6L6GC 30 W, KT88 35 W,
300B 36 W) are the manufacturers' published maxima.

### Colour

BT.601's luma weights and the PAL U and V scale factors (0.492111, 0.877283), as ITU-R
BT.470 and BT.601 give them. The describing function of a nonlinearity -- the
fundamental of its response to a sinusoid -- is standard nonlinear-systems theory;
differential gain is the broadcast measurement of the same thing on a modulated
staircase.

## Code we derived from other people's work

### Single-effect shape, harness, PassBuffer, Diag, tools — Stoatworks clamp

<https://github.com/stoatworks-labs/clamp>  
Licence: MIT  
Copyright: Stoatworks Labs

The plugin's shape (an OBJECT core, the About block last, `SetTextParameter`
accepting its own default), `PassBuffer` (tinsel's, with the SDK's colour-texture
leak fixed), `Diag`, the harness's GL rig, parameter-by-name, `--pipe` and its cue
sheets, `tools/verify.sh`, `tools/check-shaders.sh`, `tools/sweep.py`, the CI and
release workflows and `scripts/release-lib.sh`, all adapted from clamp.

### Mutation testing — Stoatworks conway

<https://github.com/stoatworks-labs/conway>  
Licence: MIT  
Copyright: Stoatworks Labs

`tools/mutate.sh` and the GLSL reserved-word and trig greps in `tools/verify.sh`.

## Third-party code this project uses

### Resolume FFGL SDK

<https://github.com/resolume/ffgl>  
Licence: BSD-3-Clause  
Copyright: FreeFrame

Vendored as a git submodule at external/ffgl, pinned to b1afaf9 like the fleet.
