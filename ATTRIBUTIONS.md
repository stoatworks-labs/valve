# Attributions

Valve is built on other people's work. This file lists what that work is, who did
it, and what it is doing here.

It is generated — the master lists live in the `stoatworks-backend` repo and are
pushed out by `scripts/sync-attributions.py`. Edit it there, not here.

## Code we derived from other people's work

Someone else solved this first, and this project would not exist in its current form without their work.

### Single-effect shape, harness, PassBuffer, Diag and tools — Stoatworks clamp

<https://github.com/stoatworks-labs/clamp>  
Licence: MIT  
Copyright: Stoatworks Labs

The plugin's shape (an OBJECT core, the About block last, SetTextParameter accepting its own default), PassBuffer (tinsel's, with the SDK's colour-texture leak fixed), Diag, the harness's GL rig, parameter-by-name and --pipe with cue sheets, tools/verify.sh, tools/check-shaders.sh, tools/sweep.py, the CI and release workflows and scripts/release-lib.sh are adapted from clamp.

### Mutation testing and the GLSL greps — Stoatworks conway

<https://github.com/stoatworks-labs/conway>  
Licence: MIT  
Copyright: Stoatworks Labs

tools/mutate.sh and the reserved-word and trig greps in tools/verify.sh are conway's.

## Third-party code this project uses

Libraries, SDKs and frameworks the project is built on or bundles.

### Resolume FFGL SDK

<https://github.com/resolume/ffgl>  
Licence: BSD-3-Clause  
Copyright: FreeFrame

Vendored as a git submodule at external/ffgl (third_party/ffgl in oxbow).

The plugin ABI itself. An FFGL effect or source is defined by this SDK's headers — there is no other way to be loadable by Resolume Arena and Avenue.

### GLEW — the OpenGL Extension Wrangler Library

<https://github.com/nigels-com/glew>  
Licence: BSD-3-Clause (with Mesa 3-D and Khronos components)  
Copyright: Milan Ikits, Marcelo E. Magallon and Lev Povalahev

Arrives inside the FFGL submodule at external/ffgl/deps/glew-2.1.0. Not fetched separately.

Resolves OpenGL entry points on Windows, where the system headers stop at OpenGL 1.1.

### libpng

<http://www.libpng.org/pub/png/libpng.html>  
Licence: PNG Reference Library License (libpng)  
Copyright: the PNG Reference Library authors

Arrives inside the FFGL submodule, under the SDK's CustomThumbnail sample.

Part of the upstream SDK tree rather than something these plugins call directly — listed because it is present in the checkout.

## Work we checked ourselves against

No code was taken from these — but they were how we knew we had it right, and that is worth saying out loud.

### Improved vacuum tube models for SPICE simulations — Norman L. Koren

<https://www.normankoren.com/Audio/Tubemodspice_article.html>

Koren's triode and pentode plate-current equations (Glass Audio 8(5), 1996; web edition updated 2003) are the plugin's valve law, and his grid resistance RGI its grid current, with the diode made ideal. Read on 2026-10-04.

### Tube.lib, the SPICE model library — Norman L. Koren

<https://www.normankoren.com/Audio/Tubemods.zip>

The parameters of all eight valves (12AX7, 12AT7, 12AU7, 6DJ8, EL34, 6L6GC, KT88, 300B) are typed from this 2001 library, which adds the 12AT7 and EL34 to the article's Table 1, and typed again in the harness, which holds the two together. Koren credits the small-signal triode data to Tom Mitchell's The Audio Designer's Tube Register and the EL34 data to Audiomatica's Sofia page.

## Inspirations

What this set out to be. No code, assets or binaries from any of these were used or examined — the debt is to the idea.

### A guitar through an overdriven valve amplifier

The chain is a guitar amplifier's: common-cathode preamp stages with a tone stack's loss between them, a phase inverter, and a single-ended or push-pull output stage, biased the way a tech biases one (70 % of rated dissipation). The circuit values are typical ones, not any one amplifier's; nothing is copied from anyone's schematic or source.

## Standards and published specifications

What the implementation is measured against.

- **ITU-R BT.601 / BT.470 Y'UV** — the luma weights 0.299, 0.587, 0.114 and PAL's U and V scale factors 0.492111 and 0.877283, which split the picture into luma and a chroma subcarrier for the Y/C and Composite modes.

## Getting this wrong

If your work is here and the description is inaccurate, the licence is wrong, or you would rather not be listed — open an issue and it will be fixed.
