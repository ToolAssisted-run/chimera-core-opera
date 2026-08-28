#!/bin/bash
# The core-level equivalence gate: the sandboxed core must produce
# byte-identical video, audio, lag and memory-domain digests to the native
# reference build (the same cinterface.c compiled natively), and must survive
# a whole-machine savestate round-trip around every frame.
#
# Real 3DO BIOSes and discs are copyrighted, so these public legs boot a
# handwritten deterministic ARM "BIOS" (it parks the exception vectors and
# fills a RAM window with a counter - a live execution witness) over a
# synthesized disc image. Real games run through tests/run-roms.sh once the
# user's own dumps sit in tests/*-local/.
#
# Usage: ./run-gate.sh [-n <native build dir>] [-g <guest build dir>]
set -u

here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/.." && pwd)"
nat="$root/build/meson-native"
gst="$root/build/meson-guest"
while getopts "n:g:" opt; do
	case "$opt" in
		n) nat="$OPTARG" ;;
		g) gst="$OPTARG" ;;
		*) exit 2 ;;
	esac
done

[ -x "$nat/run-native" ] && [ -x "$nat/run-wbx" ] || {
	echo "native build missing: meson setup build/meson-native && ninja -C build/meson-native" >&2; exit 1; }
[ -f "$gst/core.wbx" ] || {
	echo "guest build missing: sh waterbox/setup-guest.sh && ninja -C build/meson-guest" >&2; exit 1; }

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
digests() { grep -E '^(frames|vsync|videoHash|audioHash|lagFrames|domain\[)'; }
# What a turbo run can be held to: everything except the whole-run video hash,
# which a run that skipped the first half cannot possibly match - the second
# half it did draw is compared instead.
turboDigests() { grep -E '^(frames|vsync|tailVideoHash|audioHash|lagFrames|domain\[)'; }

ok=0
failed=0
report() { printf "%-30s %-6s %s\n" "$1" "$2" "$3"; case "$2" in PASS) ok=$((ok+1)) ;; *) failed=$((failed+1)) ;; esac; }
printf "%-30s %-6s %s\n" "Check" "Result" "Detail"
printf "%-30s %-6s %s\n" "-----" "------" "------"

wd="$work/smoke"
mkdir -p "$wd"
python3 "$here/tests/gen-fakecd.py" --iso "$wd/fake.iso" 4 777 >/dev/null
python3 "$here/tests/gen-fakecd.py" --bios "$wd/panafz1.bin" >/dev/null
printf '{"cd":["fake.iso"]}' > "$wd/slots"

run_pair() {
	local tag="$1"
	shift
	if ! "$nat/run-native" "$wd" "$@" 2>"$work/err" | digests > "$work/$tag.nat.txt"; then
		report "$tag:equivalence" FAIL "native runner error: $(head -1 "$work/err")"
		return 1
	fi
	if ! "$nat/run-wbx" "$gst/core.wbx" "$wd" "$@" 2>"$work/err" | digests > "$work/$tag.box.txt"; then
		report "$tag:equivalence" FAIL "waterbox runner error: $(head -1 "$work/err")"
		return 1
	fi
	if ! cmp -s "$work/$tag.nat.txt" "$work/$tag.box.txt"; then
		report "$tag:equivalence" FAIL "$(diff "$work/$tag.nat.txt" "$work/$tag.box.txt" | tr '\n' ' ' | head -c 120)"
		return 1
	fi
	report "$tag:equivalence" PASS "$(sed -n 's/^frames=//p' "$work/$tag.box.txt") frames, native == waterboxed"

	# Turbo: the VDLP's scanline renderer switched off for the first half of the
	# run and back on for the second. The machine, the sound, the lag count and
	# every picture of that second half must be what they would have been.
	"$nat/run-wbx" "$gst/core.wbx" "$wd" "$@" 2>/dev/null | turboDigests > "$work/$tag.tnorm.txt"
	if "$nat/run-wbx" "$gst/core.wbx" "$wd" "$@" --turbo 2>/dev/null | turboDigests > "$work/$tag.turbo.txt"; then
		if cmp -s "$work/$tag.tnorm.txt" "$work/$tag.turbo.txt"; then
			report "$tag:turbo" PASS "$(sed -n 's/^frames=//p' "$work/$tag.box.txt") frames, half of them undrawn, same machine and same pictures"
		else
			report "$tag:turbo" FAIL "$(diff "$work/$tag.tnorm.txt" "$work/$tag.turbo.txt" | tr '\n' ' ' | head -c 120)"
		fi
	else
		report "$tag:turbo" FAIL "turbo runner error"
	fi

	if ! "$nat/run-wbx" "$gst/core.wbx" "$wd" "$@" --rerecord 2>/dev/null | digests > "$work/$tag.rr.txt"; then
		report "$tag:savestate" FAIL "rerecord runner error"
		return 1
	fi
	if cmp -s "$work/$tag.box.txt" "$work/$tag.rr.txt"; then
		report "$tag:savestate" PASS "per-frame round-trip is lossless"
	else
		report "$tag:savestate" FAIL "$(diff "$work/$tag.box.txt" "$work/$tag.rr.txt" | tr '\n' ' ' | head -c 120)"
	fi
}

# the dummy machine, 300 frames with the pad exercised (the garbage BIOS
# never reads the pads, but the schedule must still be identical)
run_pair "smoke" --frames 300 --exercise

# the ARM witness must actually run: System RAM cannot digest as all-zeroes
if grep -q '^domain\[System RAM\]=16369f44951d0383$' "$work/smoke.box.txt"; then
	report "smoke:executed" FAIL "System RAM digests as untouched - the BIOS never ran"
else
	report "smoke:executed" PASS "the dummy BIOS visibly executed (RAM counter present)"
fi

# ---- savedata export: NVRAM must come out identical from both flavors ----
mkdir -p "$work/sd.nat" "$work/sd.box"
"$nat/run-native" "$wd" --frames 60 --savedata-out "$work/sd.nat" >/dev/null 2>&1
"$nat/run-wbx" "$gst/core.wbx" "$wd" --frames 60 --savedata-out "$work/sd.box" >/dev/null 2>&1
if [ ! -f "$work/sd.nat/NVRAM.ram" ]; then
	report "savedata:export" FAIL "the native run exported no NVRAM"
elif diff -r "$work/sd.nat" "$work/sd.box" >/dev/null 2>&1; then
	report "savedata:export" PASS "NVRAM.ram identical across flavors"
else
	report "savedata:export" FAIL "export trees differ"
fi

# ...and back in. A project supplies save data by mounting it under the name the
# export wrote, so NVRAM marked with bytes the machine could not have written
# must reach the machine and come back carrying them.
seed="$work/nv-seed"
back="$work/nv-back"
mkdir -p "$seed" "$back"
cp "$wd"/* "$seed/" 2>/dev/null
if [ -f "$work/sd.nat/NVRAM.ram" ] && python3 - "$work/sd.nat/NVRAM.ram" "$seed/NVRAM.ram" <<'PYSEED'
import sys
d = bytearray(open(sys.argv[1], 'rb').read())
d[0x40:0x50] = b'CHIMERA-SEED-TST'
open(sys.argv[2], 'wb').write(bytes(d))
PYSEED
then
	"$nat/run-native" "$seed" --frames 60 --savedata-out "$back" >/dev/null 2>&1
	if [ ! -f "$back/NVRAM.ram" ]; then
		report "savedata:seeded" FAIL "nothing came back with NVRAM mounted"
	elif ! cmp -s "$seed/NVRAM.ram" "$back/NVRAM.ram"; then
		report "savedata:seeded" FAIL "the mounted NVRAM is not what came back"
	else
		report "savedata:seeded" PASS "NVRAM the project supplied reached the machine and returned unchanged"
	fi
else
	report "savedata:seeded" FAIL "could not make a marked NVRAM to mount"
fi

# ---- settings leg: videoStandard=pal1 must reach the guest (vsync flips) ----
printf '{"videoStandard":"pal1"}' > "$wd/settings"
"$nat/run-native" "$wd" --frames 120 2>/dev/null | digests > "$work/pal.nat.txt"
"$nat/run-wbx" "$gst/core.wbx" "$wd" --frames 120 2>/dev/null | digests > "$work/pal.box.txt"
if ! cmp -s "$work/pal.nat.txt" "$work/pal.box.txt"; then
	report "settings:videoStandard" FAIL "$(diff "$work/pal.nat.txt" "$work/pal.box.txt" | tr '\n' ' ' | head -c 120)"
elif ! grep -q '^vsync=50/1$' "$work/pal.box.txt"; then
	report "settings:videoStandard" FAIL "vsync did not flip to PAL: $(grep '^vsync=' "$work/pal.box.txt")"
elif ! grep -q '^vsync=3928227/65536$' "$work/smoke.box.txt"; then
	# The NTSC rate is the machine's, not the standard's round number: opera
	# keeps the field rate as 16.16 fixed point and times itself by it. This
	# said a flat 60 once, which is nobody's rate, and it is the number a
	# movie header carries - so both regions are pinned rather than one.
	report "settings:videoStandard" FAIL "ntsc vsync is $(grep '^vsync=' "$work/smoke.box.txt"), want 3928227/65536"
else
	report "settings:videoStandard" PASS "pal1 reached the guest: 50/1 for PAL, 3928227/65536 for NTSC"
fi
rm -f "$wd/settings"

# ---- settings leg: randomSeed must be pinned in both flavors (the PRNGs
# feed the DSP, which a dummy-BIOS machine never drives, so the proof is the
# core's own seed report: "(fixed)" with the exact value, never "time-based")
printf '{"randomSeed":12345}' > "$wd/settings"
"$nat/run-native" "$wd" --frames 10 2>"$work/seed.nat.err" | digests > "$work/seed.nat.txt"
"$nat/run-wbx" "$gst/core.wbx" "$wd" --frames 10 2>"$work/seed.box.err" | digests > "$work/seed.box.txt"
rm -f "$wd/settings"
if ! cmp -s "$work/seed.nat.txt" "$work/seed.box.txt"; then
	report "settings:randomSeed" FAIL "flavors diverge under a custom seed"
elif ! grep -q 'random seed 0x00003039 (fixed)' "$work/seed.nat.err" \
	|| ! grep -q 'random seed 0x00003039 (fixed)' "$work/seed.box.err"; then
	report "settings:randomSeed" FAIL "the core did not report the fixed seed in both flavors"
else
	report "settings:randomSeed" PASS "randomSeed=12345 pinned in both flavors (no wallclock)"
fi

echo ""
echo "$ok ok, $failed failed"
[ "$failed" -gt 0 ] && exit 1
exit 0
