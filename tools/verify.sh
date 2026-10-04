#!/usr/bin/env bash
#
# Everything that can be checked without a host, in one go, in the order that
# fails fastest.
#
#   tools/verify.sh
#
# Each check answers a question none of the others can:
#
#   glsl          no GLSL 4.10 reserved word used as an identifier (Mesa
#                 refuses `packed`; Apple's compiler does not), and no sin, cos
#                 or atan in the shaders: the subcarrier's cosines are a table.
#   submodule     the FFGL SDK at the fleet's b1afaf9.
#   build         a FRESH universal Release build, and no warning in this
#                 repo's code. Not the dev build: CMake latches the
#                 architecture list at the first target, so the only build
#                 worth measuring is one configured from nothing.
#   shaders       does every shader compile, through a real GLSL compiler
#                 (tools/check-shaders.sh, which CI runs too), on the exact
#                 strings `vatest --dump-shaders` writes.
#   offline       the checks that need no GL, and their negative control:
#                   --model     the valves against Koren's Tube.lib, his law
#                               restated, every operating point and load
#                               line against the harness's own solves, the
#                               tables' geometry, the control laws
#                   --names     the host reads SW Valve / VA01 / effect
#   physics       every rendering check, at TWO rasters: 320x180, which is
#                 what CI renders at, and 1280x720. Each is measured out of
#                 the picture:
#                   --gain      the slope at mid-grey is mu R_L / ( R_L + r_p ),
#                               and the power stages' own formulas
#                   --knee      the slope drops by r_g / ( r_g + R_s ) where the
#                               grid crosses 0 V
#                   --harmonics H2/H1 = ( f2/f1 ) a / 4 for one valve; none from
#                               a matched pair; twice from twice the mismatch
#                   --crossover the composite load line at the centre; the
#                               notch the model makes, filling as bias rises
#                   --hue       Y/C keeps hue, follows the describing function
#                   --dg        Composite: chroma gain follows f1( Y )
#                   --polarity, --identity, --reference, --curve
#                   --negative  every physics check FAILS on a perturbed model
#   software      the same checks on Apple's software renderer at 320x180: the
#                 other rasteriser this Mac has.
#   mutants       one character of the shipped GLSL and C++ at a time; each
#                 must be caught (tools/mutate.sh).
#   pipe          the fleet's --pipe contract: whole frames only, a cue naming
#                 no control refused, and exit 1 -- not a silent 0, not a
#                 SIGPIPE 141 -- on a failed render or a closed stdout.
#   sweep         does every control change the picture.
#   bench         the render cost, for the record. Not pass/fail.
#   bundle        plugMain exported (a file-scope CFFGLPluginInfo nothing
#                 names can be dropped by a linker), both architectures, the
#                 plist, the release job's ad-hoc signature.
#   oxbow         a real FFGL host loads the bundle and reports the name, id
#                 and type it sees, and renders through plugMain.
#
set -uo pipefail

cd "$(dirname "$0")/.."

BUILD="${BUILD:-build-universal}"
failures=0

step() { printf '\n\033[1m== %s\033[0m\n' "$1"; }
pass() { printf '   \033[32mok\033[0m   %s\n' "$1"; }
fail() { printf '   \033[31mFAIL\033[0m %s\n' "$1"; failures=$(( failures + 1 )); }

#---------------------------------------------------------------------------
step "GLSL: reserved words and trig"
words="common partition active asm class union enum typedef template this resource goto inline noinline
       public static extern external interface long short half fixed unsigned superp input output filter
       sizeof cast namespace using packed sample patch layout flat smooth noperspective precise"
bad=0
for word in $words; do
	if grep -nE "(float|int|uint|bool|vec[234]|ivec[234]|uvec[234]|mat[234])[[:space:]]+$word[[:space:]]*[;=,)]" source/Shaders.cpp >/dev/null 2>&1; then
		printf '   "%s" is declared as an identifier and is a GLSL reserved word\n' "$word"
		bad=$(( bad + 1 ))
	fi
done
[ "$bad" -eq 0 ] && pass "no GLSL reserved word is used as an identifier" || fail "a GLSL reserved word is used as an identifier"
if grep -nE '(^|[^A-Za-z_])(sin|cos|tan|atan|asin|acos|asinh)[[:space:]]*\(' source/Shaders.cpp | grep -vE '^[0-9]+:[[:space:]]*//' >/dev/null; then
	fail "a trig built-in is in the GLSL"
else
	pass "no sin, cos, tan, atan or asinh in the GLSL"
fi

step "submodule"
if [ ! -f external/ffgl/CMakeLists.txt ]; then
	fail "FFGL SDK missing -- run: git submodule update --init --recursive"
	exit 1
fi
pin="$(git -C external/ffgl rev-parse --short=7 HEAD)"
[ "$pin" = "b1afaf9" ] && pass "FFGL SDK pinned at $pin" || fail "FFGL SDK at $pin, not the fleet's b1afaf9"

#---------------------------------------------------------------------------
step "build (fresh universal Release, $BUILD)"
rm -rf "$BUILD"
log="$( mktemp )"
if cmake -B "$BUILD" -DCMAKE_BUILD_TYPE=Release >"$log" 2>&1 && cmake --build "$BUILD" --parallel 4 >"$log" 2>&1; then
	pass "builds"
else
	tail -30 "$log"
	fail "build failed -- run: cmake -B $BUILD -DCMAKE_BUILD_TYPE=Release && cmake --build $BUILD"
	exit 1
fi
if grep -E 'warning:' "$log" | grep -v 'external/ffgl' | grep -q .; then
	grep -E 'warning:' "$log" | grep -v 'external/ffgl' | sed 's/^/   /'
	fail "a warning in this repo's code"
else
	pass "no warnings outside the SDK"
fi
rm -f "$log"

VATEST="$BUILD/vatest"

step "shaders"
if out=$(tools/check-shaders.sh "$VATEST" 2>&1); then
	pass "$( printf '%s\n' "$out" | tail -1 | sed 's/^ *//' )"
else
	fail "a shader does not compile"
	printf '%s\n' "$out"
fi

#---------------------------------------------------------------------------
step "offline (no GL)"
for check in model names negative-offline; do
	if out=$("$VATEST" --$check 2>&1); then
		summary=$( printf '%s\n' "$out" | grep -E '^negative:' | tail -1 )
		[ -n "$summary" ] || summary="$( printf '%s\n' "$out" | grep -cE ' ok$' ) checks ok"
		pass "vatest --$check: $summary"
	else
		fail "vatest --$check"
		printf '%s\n' "$out" | grep -E 'FAIL' | sed 's/^/      /'
	fi
done

physics() {
	local size="$1" label="$2"
	for check in gain knee harmonics crossover hue dg polarity identity reference curve negative; do
		if out=$("$VATEST" --$check --size "$size" 2>&1); then
			pass "$label --$check: $( printf '%s\n' "$out" | grep -E 'checks,' | tail -1 )"
		else
			fail "$label --$check at $size"
			printf '%s\n' "$out" | grep -E 'FAIL|checks,' | sed 's/^/      /'
		fi
	done
}

for size in 320x180 1280x720; do
	step "physics at $size"
	physics "$size" "vatest"
done

step "software renderer at 320x180"
VATEST_RENDERER=software physics 320x180 "software"

step "mutants"
if out=$(tools/mutate.sh 2>&1); then
	pass "$( printf '%s\n' "$out" | tail -1 )"
else
	fail "a mutant survived"
	printf '%s\n' "$out" | grep -E 'mutant:|FAIL' | sed 's/^/      /'
fi

#---------------------------------------------------------------------------
# --pipe, in the fleet's frame format.
#---------------------------------------------------------------------------
step "pipe"
frame=$(( 64 * 36 * 4 ))
raw=$( mktemp ); many=$( mktemp ); cues=$( mktemp )
head -c $(( frame * 5 / 2 )) /dev/zero > "$raw"
head -c $(( frame * 40 )) /dev/zero > "$many"

got=$( "$VATEST" --pipe --size 64x36 < "$raw" 2>/dev/null | wc -c | tr -d ' ' )
status=${PIPESTATUS[0]}
if [ "$status" -eq 0 ] && [ "$got" = "$(( frame * 2 ))" ]; then
	pass "2.5 frames in, exactly 2 frames out, clean exit"
else
	fail "2.5 frames in gave $got bytes out (want $(( frame * 2 ))), exit $status"
fi

# Read from a file, not a pipe: a writer killed by SIGPIPE would fail the
# pipeline whatever vatest did, and the refusal would pass for the wrong reason.
printf '0 No Such Control 0.5\n' > "$cues"
"$VATEST" --pipe --size 64x36 --script "$cues" < "$raw" >/dev/null 2>&1
status=$?
if [ "$status" -eq 2 ]; then
	pass "a cue naming no parameter is refused (exit 2)"
else
	fail "a cue naming no parameter gave exit $status, not 2"
fi

got=$( "$VATEST" --pipe --size 64x36 --fail-render-at 1 < "$raw" 2>/dev/null | wc -c | tr -d ' ' )
status=${PIPESTATUS[0]}
if [ "$status" -eq 1 ] && [ "$got" = "$frame" ]; then
	pass "a failed render at frame 1: exit 1, one frame out"
else
	fail "a failed render at frame 1 gave exit $status and $got bytes (want 1 and $frame)"
fi

"$VATEST" --pipe --size 64x36 < "$many" 2>/dev/null | head -c 100 >/dev/null
status=${PIPESTATUS[0]}
if [ "$status" -eq 1 ]; then
	pass "a closed stdout: exit 1"
else
	fail "a closed stdout gave exit $status, not 1"
fi
rm -f "$raw" "$many" "$cues"

step "sweep"
if out=$(python3 tools/sweep.py --binary "$VATEST" 2>/dev/null); then
	pass "$( printf '%s\n' "$out" | tail -1 )"
else
	fail "tools/sweep.py reports a dead control"
	printf '%s\n' "$out" | grep -E '^DEAD|DEAD CONTROLS' | sed 's/^/      /'
fi

step "bench (for the record)"
"$VATEST" --bench 2>&1 | sed -n '1p;4,8p' | sed 's/^/   /'

#---------------------------------------------------------------------------
BUNDLE="$BUILD/Valve.bundle"
BIN="$BUNDLE/Contents/MacOS/Valve"

if [ "$(uname)" = "Darwin" ] && [ -d "$BUNDLE" ]; then
	step "bundle"
	# `nm ... | grep -q X` FAILS when grep FINDS its match under `set -o pipefail`:
	# grep exits at once, nm takes SIGPIPE, and the pipeline reports failure.
	# Capture and match instead of piping.
	syms=$(nm -gU "$BIN" 2>/dev/null)
	case "$syms" in
		*_plugMain*) pass "exports plugMain" ;;
		*) fail "no plugMain -- the bundle contains no plugin" ;;
	esac
	archs=$(lipo -archs "$BIN" 2>/dev/null)
	case "$archs" in *arm64*) pass "arm64 present" ;; *) fail "no arm64 (got: $archs)" ;; esac
	case "$archs" in *x86_64*) pass "x86_64 present" ;; *) fail "no x86_64 (got: $archs) -- a universal build was asked for" ;; esac

	exe=$(/usr/libexec/PlistBuddy -c "Print :CFBundleExecutable" "$BUNDLE/Contents/Info.plist" 2>/dev/null)
	ident=$(/usr/libexec/PlistBuddy -c "Print :CFBundleIdentifier" "$BUNDLE/Contents/Info.plist" 2>/dev/null)
	version=$(/usr/libexec/PlistBuddy -c "Print :CFBundleVersion" "$BUNDLE/Contents/Info.plist" 2>/dev/null)
	declared="$( sed -n 's/^[[:space:]]*VERSION \([0-9.]*\)$/\1/p' CMakeLists.txt | head -1 )"
	about="$( sed -n 's/.*versionFallback = "v\([0-9.]*\)".*/\1/p' source/StoatworksAbout.h )"
	if [ -n "$exe" ] && [ -f "$BUNDLE/Contents/MacOS/$exe" ]; then
		pass "CFBundleExecutable ($exe) is on disk"
	else
		fail "CFBundleExecutable is '$exe' but no such binary exists -- codesign will fail after the tag"
	fi
	[ "$ident" = "com.stoatworks.ffgl.valve" ] && pass "CFBundleIdentifier is $ident" || fail "CFBundleIdentifier is '$ident'"
	if [ "$version" = "$declared" ] && [ "$about" = "$declared" ]; then
		pass "version $declared in CMakeLists.txt, the plist and StoatworksAbout.h"
	else
		fail "versions disagree: CMakeLists $declared, plist $version, StoatworksAbout.h $about"
	fi

	tmp=$(mktemp -d)
	cp -R "$BUNDLE" "$tmp/" 2>/dev/null
	if codesign --force --sign - --timestamp=none "$tmp/Valve.bundle" >/dev/null 2>&1; then
		pass "ad-hoc signs (the command the release job runs)"
	else
		fail "ad-hoc signing failed"
	fi
	rm -rf "$tmp"

	step "oxbow"
	OXBOW="${OXBOW:-../oxbow/build/oxbow}"
	[ -x "$OXBOW" ] || OXBOW="$HOME/Projects/resolume/oxbow/build/oxbow"
	if [ -x "$OXBOW" ]; then
		probe=$("$OXBOW" probe "$BUNDLE" 2>&1)
		for want in "name:        SW Valve" "id:          VA01" "type:        effect"; do
			case "$probe" in
				*"$want"*) pass "host sees '$want'" ;;
				*) fail "host does not see '$want' -- see: $OXBOW probe $BUNDLE" ;;
			esac
		done
		self=$("$OXBOW" selftest "$BUNDLE" 2>&1)
		case "$self" in
			*"selftest:    PASS"*) pass "instantiates through plugMain and renders 120 frames" ;;
			*) fail "oxbow selftest did not pass -- see: $OXBOW selftest $BUNDLE" ;;
		esac
	else
		printf '   skipped: oxbow not built at %s\n' "$OXBOW"
	fi
fi

printf '\n'
if [ "$failures" -eq 0 ]; then
	printf '\033[32mall checks passed\033[0m\n'
else
	printf '\033[31m%d check(s) failed\033[0m\n' "$failures"
fi
exit $(( failures > 0 ? 1 : 0 ))
