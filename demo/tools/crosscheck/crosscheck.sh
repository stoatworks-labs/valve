#!/usr/bin/env bash
#
# The browser demo's JavaScript port of the CPU half, against the plugin's C++.
#
#     demo/tools/crosscheck/crosscheck.sh        exit 1 on any difference
#
# printer.cpp drives the REAL plugin class -- Valve.cpp and everything it calls,
# with the FFGL SDK -- in a headless GL 4.1 context through scenario.mjs's
# settings, and after every frame prints what the plugin made of them: the
# declarations and defaults its constructor set, the settings currentSettings()
# built, every operating point and every stage table Tables::Update solved, the
# normalisation, the chain at fixed points, and every uniform of its two shader
# programs read back out of GL. run.mjs runs the PORT block of demo/plugin.js
# through the same settings and prints the same. Floats and doubles print as
# their bits.
#
#   - THE LIBM IS TAKEN OUT. JavaScript's Math.exp and friends are V8's, the
#     plugin's are Apple's libm, and they disagree in the last bit of a few
#     results in a hundred (this script prints the counts). So the C++ runs with
#     libm_record.c interposed, which writes down every libm call it makes, and
#     the JavaScript answers its Math calls from that record (replay.mjs). With
#     that, and with the C++ built -ffp-contract=off (below), the port must
#     print EXACTLY what the plugin prints, every line.
#   - THE PAGE'S OWN NUMBERS are then measured on V8's Math, as the page runs:
#     everything the GPU reads -- every table float, every row's warp, every
#     uniform -- must still be identical to the plugin's; the CPU's doubles are
#     reported in ulps. (Newton stops at a relative 1e-13, so a last-bit libm
#     difference can move a solved bias by a few thousand ulps of a double.)
#   - THE FUSED MULTIPLY-ADD. Apple clang's default -ffp-contract=on fuses
#     a*b+c on arm64, which JavaScript cannot do, so the plugin's Apple Silicon
#     slice rounds differently from its x86_64 slice (no FMA in its baseline)
#     and from this port. -ffp-contract=off is the x86_64 slice's arithmetic and
#     is what must match. The arm64 build is compared too, and reported.
#
# Run by tools/verify.sh. Needs node, a C and C++ compiler, the FFGL submodule
# and an OpenGL context. About half a minute; the libm record is ~450 MB in a
# temporary directory and the replay holds ~1.3 GB while it runs.
#
# Adapted from slipstream's crosscheck (demo/tools/crosscheck/ there).
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$HERE/../../.." && pwd)"
CXX="${CXX:-/usr/bin/c++}"
CC="${CC:-/usr/bin/cc}"

command -v node >/dev/null 2>&1 || { echo "crosscheck: node is not installed"; exit 2; }
[[ -f "$REPO/external/ffgl/CMakeLists.txt" ]] || { echo "crosscheck: FFGL SDK missing -- git submodule update --init --recursive"; exit 2; }

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

plugin_sources=( "$REPO/source/Valve.cpp" "$REPO/source/Valves.cpp" "$REPO/source/Chain.cpp" "$REPO/source/Controls.cpp"
                 "$REPO/source/Shaders.cpp" "$REPO/source/PassBuffer.cpp" "$REPO/source/Diag.cpp"
                 "$REPO/external/ffgl/source/lib/FFGLSDK.cpp" )
flags=( -std=c++17 -O3 -DGL_SILENCE_DEPRECATION -I "$REPO/source" -I "$REPO/external/ffgl/source/lib"
        -framework OpenGL -framework AppKit -w )

"$CC" -O2 -fno-builtin -dynamiclib "$HERE/libm_record.c" -o "$tmp/librecord.dylib"
"$CXX" -ffp-contract=off "${flags[@]}" "$HERE/printer.cpp" "${plugin_sources[@]}" -o "$tmp/printer-strict"
"$CXX" "${flags[@]}" "$HERE/printer.cpp" "${plugin_sources[@]}" -o "$tmp/printer-native"

node "$HERE/scenario.mjs" > "$tmp/scenario.txt"
steps=$( grep -c '^set ' "$tmp/scenario.txt" )

DYLD_INSERT_LIBRARIES="$tmp/librecord.dylib" LIBM_RECORD="$tmp/calls.bin" "$tmp/printer-strict" < "$tmp/scenario.txt" > "$tmp/cpp.txt"
# A dyld that ignored DYLD_INSERT_LIBRARIES would leave no record, and the
# replay would silently be V8's Math again.
[[ -s "$tmp/calls.bin" ]] || { echo "FAIL the libm recorder wrote nothing -- was DYLD_INSERT_LIBRARIES ignored?"; exit 1; }
"$tmp/printer-native" < "$tmp/scenario.txt" > "$tmp/cpp-native.txt"

LIBM_REPLAY="$tmp/calls.bin" node --max-old-space-size=8192 "$HERE/run.mjs" < "$tmp/scenario.txt" > "$tmp/js-replay.txt" 2> "$tmp/js-replay.err"
node "$HERE/run.mjs" < "$tmp/scenario.txt" > "$tmp/js.txt" 2>/dev/null

lines=$( wc -l < "$tmp/js-replay.txt" | tr -d ' ' )
[[ "$lines" -gt 30000 ]] || { echo "FAIL the JavaScript side printed only $lines lines"; exit 1; }
if ! out=$( python3 "$HERE/compare.py" exact "$tmp/cpp.txt" "$tmp/js-replay.txt" ); then
	echo "FAIL the JavaScript port differs from the real plugin class (libm replayed, contraction off):"
	printf '%s\n' "$out" | sed 's/^/      /'
	exit 1
fi
echo "ok   $steps settings through the real plugin class: all $lines lines identical to the port's, libm replayed, contraction off"
python3 - "$tmp/js-replay.err" <<'PY'
import json, sys
s = json.loads(open(sys.argv[1]).read().strip().splitlines()[-1])
miss = s.get("misses", {})
print(f"     {s['replayed']:,} libm calls replayed;", end=" ")
print("none fell back" if not miss else
      "fell back to V8 (arguments clang folded at build time): " + ", ".join(f"{k} {v:,}" for k, v in sorted(miss.items())))
parts = [f"{k} {v['differ']:,} of {v['distinct']:,}" for k, v in s["libm"].items()]
worst = max(v["worstUlps"] for v in s["libm"].values())
print(f"     V8's Math against Apple's libm, distinct calls differing (worst {worst} ulp): " + ", ".join(parts))
PY

if ! out=$( python3 "$HERE/compare.py" gpu "$tmp/cpp.txt" "$tmp/js.txt" ); then
	printf '%s\n' "$out" | sed 's/^/   /'
	echo "FAIL on V8's own Math the port hands the GPU something different"
	exit 1
fi
echo "ok   the port on V8's own Math, as the page runs it: every table float, warp and uniform identical; the doubles:"
printf '%s\n' "$out" | grep -v '^tables:' | sed 's/^/       /' || true
echo "     against the plugin as built for arm64 (fused multiply-adds), reported, not required:"
python3 "$HERE/compare.py" ulps "$tmp/cpp-native.txt" "$tmp/js.txt" | sed 's/^/       /'
