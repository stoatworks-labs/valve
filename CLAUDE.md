# valve

The picture through a valve amplifier for Resolume Arena/Avenue: `SW Valve`
(`VA01`, effect). Koren's valve models solved on real load lines, chained like a
guitar amplifier (0–4 preamp triodes, an optional single-ended or push-pull power
stage), fed as RGB, Y/C or Composite. C++/GLSL, CMake MODULE → a universal `.bundle`
(macOS) + Windows `.dll`. MIT. Bundle id `com.stoatworks.ffgl.valve`.

Read `AGENTS.md` before touching the valve table (`Valves.cpp`), the table warp or
the chain (`Chain.cpp`), the library GLSL (`Shaders.cpp`) or the parameter enum.

## Commands (CMake)
- Configure: `cmake -B build -DCMAKE_BUILD_TYPE=Release`
- Fast dev build: add `-DCMAKE_OSX_ARCHITECTURES=arm64`
- Build: `cmake --build build`
- Install to Resolume: `cmake --install build` (never from `~/Projects`)
- Render the card: `./build/vatest --out /tmp/valve.png --size 1280x720`
- A frame of a real clip: `ffmpeg -ss 2 -i clip.mov -frames:v 1 -vf "scale=1280:720,format=rgb24,format=rgba" -f rawvideo -pix_fmt rgba /tmp/c.rgba && ./build/vatest --clip /tmp/c.rgba --size 1280x720 --out /tmp/v.png`
- List parameters: `./build/vatest --list`
- Set anything by name: `./build/vatest --set "Stages=4" --set "Drive=0.4"` (options and integers by value, sliders 0..1)
- A clip through the effect: `ffmpeg -i in.mov -f rawvideo -pix_fmt rgba -s WxH - | ./build/vatest --pipe --size WxH | ffmpeg -f rawvideo -pix_fmt rgba -s WxH -r 30 -i - out.mp4`
  A `--script` cue line is `frame  Parameter Name  value` (`#` starts a comment), in
  the same units as `--set`; a standard control is linear between keys, an option,
  boolean or integer steps. An unknown name exits 2 before any frame; a partial
  frame at EOF ends the stream with exit 0; a failed render or a reader that hangs
  up exits 1 (SIGPIPE is ignored), never a silent 141.

## Verify
- Everything: `tools/verify.sh` (~3 min: reserved words, no trig in the GLSL, the
  SDK pin, a fresh universal build with no warnings, glslc, the offline set, every
  rendered check at 320x180 and 1280x720, the same on Apple's software renderer,
  the mutants, the `--pipe` contract, the sweep, the bench, the bundle, oxbow).
- The physics: `--gain`, `--knee`, `--harmonics`, `--crossover`, `--hue`, `--dg`.
- The machinery: `--polarity`, `--identity`, `--reference`, `--curve`; no GL:
  `--model`, `--names`.
- One raster: add `--size WxH` (`--knee` needs a width divisible by 4,
  `--harmonics` by 40). The software renderer: `VATEST_RENDERER=software ./build/vatest --gain --size 320x180`.
- The checks can fail: `--negative` (seven), `--negative-offline` (one),
  `tools/mutate.sh` (six mutants). `--perturb BITS` runs any check verbosely
  against a perturbed model (bits in `Valves.h`).
- What CI runs: `--offline`, `tools/check-shaders.sh`, and the rendered checks with
  `--allow-no-gl`.
- No dead controls: `python3 tools/sweep.py` (24 parameters).
- Cost: `--bench` (GPU timer queries at 720p/1080p/4K, and a table solve on the CPU).

## Notes
- **A stage is a table.** Every valve stage is memoryless, so its plate deviation
  against the open-circuit grid voltage is solved once on the CPU in double
  (`Chain.cpp`), into one R32F row of 4097 nodes per stage kind, chain and channel.
  The GPU looks up, multiplies and adds. Drive, Master, Interstage, Stages and Rest
  Level are uniforms; valve, bias, idle and mismatch re-solve only the rows they touch.
- **The grid is warped**: node i sits at `c + w sinh( ( i - 2048 ) du )`. The centre
  node is the quiescent point and holds exactly 0; `w` is nudged so the
  grid-current knee lands ON a node. `Tables::Evaluate` runs the shader's chain over
  the same float tables in double: the normalisation comes from it.
- **Every read is `texelFetch` and every lerp is written out** in float: a texture
  unit's bilinear weights can be 8-bit.
- **No trig in the GLSL** (verify.sh greps): the subcarrier's cosines are a CPU
  table, and asinh is written out (`asinhSigned`) because the built-in may cancel
  for large negative arguments.
- **Premultiplied in, premultiplied out**: colour is divided by alpha before the
  valves and multiplied back after.
- Every host parameter is 0..1 except Stages and Chroma Stages (real integers 0–4).
  An option reads back 0..1 whatever its count: map by element index.
- The About block is LAST. Its `SetTextParameter` returns FF_SUCCESS or no host can
  instantiate the plugin.
- `valve_core` is an OBJECT library: the registration is a file-scope constructor
  nothing references.
- Public repo (`stoatworks-labs/valve`), registered in the fleet: `StoatworksAbout.h`
  and `ATTRIBUTIONS.md` are generated (sync-about.py, sync-attributions.py) -- edit the
  masters in stoatworks-backend, never these. "Commit" = commit **and** push.

## Not done yet
- Never loaded into Resolume on macOS (oxbow selftest only); on Windows the fleet
  Arena gate passed 9/9 on win-lab (plugin-bench `arena/expect/valve.json`). No OpenFX port, no
  browser demo. The user guide is `docs/USER-GUIDE.md`; the PDF and the site page are
  generated from it by the website's `build_guides.py valve`.
- Time-domain effects (Miller smear, coupling-capacitor blocking, supply sag, the
  output transformer, a tone stack) are not modelled; see AGENTS.md.

## Diagnostics

`source/Diag.{h,cpp}` is a log file only, with no crash handler (this runs
inside Resolume). It records which shader failed to compile and the GL
vendor/renderer.

    ~/Library/Logs/valve/valve.YYYY-MM-DD.log
