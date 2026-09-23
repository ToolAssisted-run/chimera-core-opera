#!/bin/bash
# Replays the movie manifest (tests/movies/manifest.json - headlessOpera's
# test base) over whatever images sit in tests/roms-local/, with the real
# BIOS dumps from tests/firmware-local/ (named by their canonical filenames:
# panafz1.bin and friends). Nothing copyrighted lives in this repo, so every
# leg SKIPs cleanly until you drop the files in.
#
# Every present game must be native == sandbox == per-frame savestate
# round-trip on all digests, over its whole movie.
#
# Usage: ./run-roms.sh
set -u

here="$(cd "$(dirname "$0")" && pwd)"
wb="$(cd "$here/.." && pwd)"
root="$(cd "$wb/.." && pwd)"
nat="$root/build/meson-native"
gst="$root/build/meson-guest"

[ -x "$nat/run-native" ] && [ -x "$nat/run-wbx" ] && [ -f "$gst/core.wbx" ] || {
	echo "build both flavors first (see README)" >&2; exit 1; }

romdir="$root/tests/roms-local"
fwdir="$root/tests/firmware-local"

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
digests() { grep -E '^(frames|vsync|videoHash|audioHash|lagFrames|domain\[)'; }

ok=0
failed=0
skipped=0
report() {
	printf "%-34s %-6s %s\n" "$1" "$2" "$3"
	case "$2" in PASS) ok=$((ok+1)) ;; SKIP) skipped=$((skipped+1)) ;; *) failed=$((failed+1)) ;; esac
}
printf "%-34s %-6s %s\n" "Check" "Result" "Detail"
printf "%-34s %-6s %s\n" "-----" "------" "------"

count="$(python3 -c "import json;print(len(json.load(open('$root/tests/movies/manifest.json'))))")"
for i in $(seq 0 $((count - 1))); do
	eval "$(python3 - "$root/tests/movies/manifest.json" "$i" <<'PYENT'
import json, shlex, sys
m = json.load(open(sys.argv[1]))[int(sys.argv[2])]
for k, v in m.items():
    print(f"{k}={shlex.quote(str(v))}")
PYENT
)"
	src="$(find "$romdir" -name "$rom" -type f 2>/dev/null | head -1)"
	[ -n "$src" ] || { report "$name" SKIP "drop '$rom' into tests/roms-local/"; continue; }
	[ -f "$fwdir/$bios" ] || { report "$name" SKIP "drop '$bios' into tests/firmware-local/"; continue; }

	wd="$work/$name"
	mkdir -p "$wd"
	if [ "${rom##*.}" = "cue" ]; then
		cp "$(dirname "$src")"/* "$wd/" 2>/dev/null
	else
		cp "$src" "$wd/"
	fi
	cp "$fwdir/$bios" "$wd/"
	printf '{"cd":["%s"]}' "$rom" > "$wd/slots"
	python3 - "$wd/settings" "$systemType" "$ctl1" "$ctl2" <<'PYSET'
import json, sys
def port(c):
    return "none" if c == "none" else "gamepad"
json.dump({"systemType": sys.argv[2], "port1": port(sys.argv[3]),
           "port2": port(sys.argv[4])}, open(sys.argv[1], "w"))
PYSET

	shaGot="$(sha1sum "$src" | awk '{print toupper($1)}')"
	shaNote=""
	[ -n "$sha1" ] && [ "$shaGot" != "$sha1" ] && shaNote=" (rom sha1 differs from the movie's!)"

	args=(--ctl1 "$ctl1" --ctl2 "$ctl2" --sol "$root/tests/movies/$sol")
	if ! "$nat/run-native" "$wd" "${args[@]}" 2>"$work/err" | digests > "$work/nat.txt"; then
		report "$name" FAIL "native runner error: $(head -1 "$work/err")"; continue
	fi
	if ! "$nat/run-wbx" "$gst/core.wbx" "$wd" "${args[@]}" 2>"$work/err" | digests > "$work/box.txt"; then
		report "$name" FAIL "waterbox runner error: $(head -1 "$work/err")"; continue
	fi
	frames="$(sed -n 's/^frames=//p' "$work/box.txt")"
	if ! cmp -s "$work/nat.txt" "$work/box.txt"; then
		report "$name" FAIL "$(diff "$work/nat.txt" "$work/box.txt" | tr '\n' ' ' | head -c 110)"
		continue
	fi
	if ! "$nat/run-wbx" "$gst/core.wbx" "$wd" "${args[@]}" --rerecord 2>/dev/null | digests > "$work/rr.txt"; then
		report "$name" FAIL "rerecord runner error"
	elif cmp -s "$work/box.txt" "$work/rr.txt"; then
		report "$name" PASS "$frames frames, native == sandbox == rerecord$shaNote"
	else
		report "$name" FAIL "rerecord differs$shaNote"
	fi
done

# disc:swap - a disc change taken the way the console takes it. The OS sees
# the drive's media-access bit, soft-resets into the ROM's dipir, dipir finds
# a disc the running title will not share and reboots the machine through the
# watchdog pin, and the console comes up on the new disc. The witness is the
# picture: a machine that swapped discs mid-game must end up exactly where a
# machine that booted the second disc does, native and sandboxed alike.
#
# tests/roms-local/swap/ holds the first disc, then the second (in that order
# by name - symlinks do), and a file "params" with three frame counts for that
# game: swapAt (the tray opens there, for one second), after (the swapped run's
# length) and direct (the direct boot's), each landing on a static screen.
swapdir="$romdir/swap"
if [ ! -f "$swapdir/params" ]; then
	report "disc:swap" SKIP "tests/roms-local/swap/: two discs' .cue files and a params file"
elif [ ! -f "$fwdir/panafz1.bin" ]; then
	report "disc:swap" SKIP "drop 'panafz1.bin' into tests/firmware-local/"
else
	. "$swapdir/params"
	mapfile -t cues < <(cd "$swapdir" && ls *.cue | sort)
	wd="$work/swap"
	mkdir -p "$wd"
	for f in "$swapdir"/*.cue "$swapdir"/*.bin; do ln -s "$(readlink -f "$f")" "$wd/$(basename "$f")"; done
	cp "$fwdir/panafz1.bin" "$wd/"
	swapBtn=87 # Disc Swap: after both ports' controls
	printf '{"cd":["%s","%s"]}' "${cues[0]}" "${cues[1]}" > "$wd/slots"
	"$nat/run-native" "$wd" --frames "$after" --press "$swapAt:60:$swapBtn" --screenshot "$work/swap-nat.tga" >/dev/null 2>&1
	"$nat/run-wbx" "$gst/core.wbx" "$wd" --frames "$after" --press "$swapAt:60:$swapBtn" --screenshot "$work/swap-box.tga" >/dev/null 2>&1
	"$nat/run-native" "$wd" --frames "$after" --screenshot "$work/stay.tga" >/dev/null 2>&1
	printf '{"cd":["%s"]}' "${cues[1]}" > "$wd/slots"
	"$nat/run-native" "$wd" --frames "$direct" --screenshot "$work/direct.tga" >/dev/null 2>&1
	if [ ! -s "$work/direct.tga" ] || [ ! -s "$work/swap-nat.tga" ]; then
		report "disc:swap" FAIL "a run produced no picture"
	elif cmp -s "$work/stay.tga" "$work/direct.tga"; then
		report "disc:swap" FAIL "the run that never swapped already shows the second disc's screen: params prove nothing"
	elif ! cmp -s "$work/swap-nat.tga" "$work/direct.tga"; then
		report "disc:swap" FAIL "after the swap the machine is not where booting the second disc puts it"
	elif ! cmp -s "$work/swap-nat.tga" "$work/swap-box.tga"; then
		report "disc:swap" FAIL "native and sandbox differ after the swap"
	else
		report "disc:swap" PASS "swapped at $swapAt, rebooted into the second disc: frame $after == its direct boot at $direct, native == sandbox"
	fi
fi

echo ""
echo "$ok ok, $failed failed, $skipped skipped"
[ "$failed" -gt 0 ] && exit 1
exit 0
