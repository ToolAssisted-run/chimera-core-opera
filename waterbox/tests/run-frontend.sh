#!/bin/bash
# The frontend half of the gate: load the Opera package in Chimera's
# window (run headless: it needs no display), boot the synthesized dummy-BIOS
# machine (real 3DO dumps are copyrighted; the firmware channel accepts an
# unrecognized dump by design) for a fixed number of frames, and require the
# whole System RAM domain to be byte-identical to the native reference. Then
# prove a machine-shaping setting arrives (videoStandard=pal1 builds a
# different machine that still matches ITS native reference), and that the
# package's keybinds become the frontend's defaults.
#
# Usage: ./run-frontend.sh [--chimera-root <path>] [--frames N]
set -u

here="$(cd "$(dirname "$0")" && pwd)"
wb="$(cd "$here/.." && pwd)"
root="$(cd "$wb/.." && pwd)"
frames=300
chimera_root=""
while [ $# -gt 0 ]; do
	case "$1" in
		--chimera-root) chimera_root="$2"; shift ;;
		--frames) frames="$2"; shift ;;
		-*) echo "unknown option: $1" >&2; exit 2 ;;
		*) break ;;
	esac
	shift
done

if [ -z "$chimera_root" ]; then
	for candidate in "$root/../chimera" "$HOME/chimera"; do
		[ -d "$candidate" ] && { chimera_root="$candidate"; break; }
	done
fi
[ -n "$chimera_root" ] && [ -d "$chimera_root" ] || {
	echo "chimera checkout not found; pass --chimera-root <path>" >&2; exit 1; }
chimera_root="$(cd "$chimera_root" && pwd)"

chimera_exe="$chimera_root/build/meson-linux/source/ui/chimera"
package="$chimera_root/build/Cores/opera.chimeraCore"
rn="$root/build/meson-native/run-native"
rom="$here/work/fake.iso"
[ -x "$chimera_exe" ] || { echo "Chimera not built: $chimera_exe" >&2; exit 1; }
[ -f "$package" ] || { echo "package not installed: $package (run ../build-package.sh)" >&2; exit 1; }
[ -x "$rn" ] || { echo "native reference not built" >&2; exit 1; }

work="$here/work"
mkdir -p "$work"

# the synthesized machine: a pseudo-random disc + the deterministic dummy
# BIOS, pointed at through the real firmware channel (an unrecognized dump
# is usable by design; the sha1 check only reports)
python3 "$here/gen-fakecd.py" --iso "$work/fake.iso" 4 777 >/dev/null
python3 "$here/gen-fakecd.py" --bios "$work/panafz1.bin" >/dev/null

# Chimera's window, run headless: the cores installed in the checkout, its
# settings, cache and recovery files the run's own, and nothing carried over
# from a Chimera installed on this machine
export ALSOFT_DRIVERS=null CHIMERA_CORES_DIR="$chimera_root/build/Cores" CHIMERA_QT_CACHE="$work/qt-cache" CHIMERA_QT_RECOVERY="$work/qt-recovery" \
	CHIMERA_QT_HASHES="$work/qt-file-hashes.txt" CHIMERA_QT_CHIMERA_SETTINGS="$work/no-chimera-config.ini"

ok=0
failed=0
report() { printf "%-28s %-9s %s\n" "$1" "$2" "$3"; case "$2" in PASS) ok=$((ok+1)) ;; *) failed=$((failed+1)) ;; esac; }
printf "%-28s %-9s %s\n" "Check" "Result" "Detail"
printf "%-28s %-9s %s\n" "-----" "------" "------"

run_frontend() {
	local tag="$1" cfg="$2" nframes="$3" shot="${4:-}"
	local job="$work/job.$tag.txt"
	local out="$work/$tag.ram.bin"
	[ "${lua_script:-}" = dump-controls.lua ] && out="$work/$tag.controls.json"
	{
		echo "frames=$nframes"
		echo "out=$out"
		echo "meta=$work/$tag.meta.txt"
		echo "shot=$shot"
	} > "$job"
	rm -f "$work/$tag.ram.bin" "$work/$tag.meta.txt"
	[ -n "$shot" ] && rm -f "$shot"
	local project
	project="$(game_project "$tag" "$cfg" "$package" "$rom")" || { cp "$work/projects/$tag.make.log" "$work/$tag.log"; return 1; }
	( cd "$chimera_root" && MINIHAWK_JOB="$job" timeout 900 "$chimera_exe" --headless \
		"--config=$cfg" "--core=$package" \
		"--lua=$here/${lua_script:-frontend-ram.lua}" "--project=$project" ) > "$work/$tag.log" 2>&1
	[ -f "$work/$tag.meta.txt" ] && grep -q "^status=OK" "$work/$tag.meta.txt"
}

# a native reference run: same rom, same idle schedule, settings JSON in $2
native_ram() {
	local tag="$1" settings="$2"
	local wd="$work/native.$tag"
	rm -rf "$wd"
	mkdir -p "$wd"
	cp "$rom" "$wd/"
	cp "$work/panafz1.bin" "$wd/"
	printf '{"cd":["%s"]}' "$(basename "$rom")" > "$wd/slots"
	[ -n "$settings" ] && printf '%s' "$settings" > "$wd/settings"
	"$rn" "$wd" --frames "$frames" --dump-domain "System RAM" "$work/native.$tag.ram.bin" \
		> "$work/native.$tag.txt" 2>&1
}

firmware_json="{\"panafz1.bin\": \"$work/panafz1.bin\"}"
# A game runs only in a project (Chimera, 2026-10-26): each run opens one made
# of its game with chimera-run --new-project, holding the settings (and the firmware)
# settings_config was given for the config it runs with.
declare -A settings_of firmware_of
settings_config() { python3 "$here/settings-config.py" "$1" "$2" "$firmware_json"; settings_of["$1"]="$2"; firmware_of["$1"]="$firmware_json"; }
crun="$chimera_root/build/dll/chimera-run"
game_project() { # tag, config, package, game: prints the project's path
	local dir="$work/projects" settings="${settings_of[$2]:-}" firmware="${firmware_of[$2]:-}"
	local pkg="$3" game="$4"
	local fw=()
	[ -n "$firmware" ] && mapfile -t fw < <(python3 -c 'import json,sys
for k, v in json.loads(sys.argv[1]).items(): print("--firmware"); print(k + "=" + v)' "$firmware")
	# one copy of each game, every run's project beside it
	local copy="$dir/$(basename "$game")"
	mkdir -p "$dir"
	[ -f "$copy" ] && [ "$(stat -c %s "$copy")" = "$(stat -c %s "$game")" ] || cp "$game" "$copy"
	"$crun" --new-project "$dir/$1.chimeraProject" "$pkg" "$copy" \
		--settings "${settings:-"{}"}" "${fw[@]}" > "$dir/$1.make.log" 2>&1 && echo "$dir/$1.chimeraProject"
}

# --- the machine the frontend builds must be the one the gate signed off on ---
settings_config "$work/settings.base.json" '{}'
if ! native_ram "base" ""; then
	report "disc:frontend" FAIL "native runner error (see tests/work/native.base.txt)"
elif ! run_frontend "base" "$work/settings.base.json" "$frames" "$work/base.png"; then
	report "disc:frontend" FAIL "no OK meta (see tests/work/base.log)"
elif cmp -s "$work/native.base.ram.bin" "$work/base.ram.bin"; then
	report "disc:frontend" PASS "$frames frames, System RAM identical to the native reference"
else
	report "disc:frontend" FAIL "System RAM differs from the native reference"
fi

# --- a machine-shaping setting must reach the guest through the frontend ---
settings_config "$work/settings.pal.json" '{"videoStandard": "pal1"}'
if ! native_ram "pal" '{"videoStandard":"pal1"}'; then
	report "settings:videoStandard" FAIL "native runner error (see tests/work/native.pal.txt)"
elif ! run_frontend "pal" "$work/settings.pal.json" "$frames"; then
	report "settings:videoStandard" FAIL "run did not report OK (see tests/work/pal.log)"
elif ! cmp -s "$work/native.pal.ram.bin" "$work/pal.ram.bin"; then
	report "settings:videoStandard" FAIL "PAL System RAM differs from its native reference"
elif ! grep -q '^height=288$' "$work/pal.meta.txt"; then
	report "settings:videoStandard" FAIL "the frame is not PAL-sized: $(grep '^height=' "$work/pal.meta.txt")"
else
	report "settings:videoStandard" PASS "videoStandard=pal1 arrived (288-line frame), matches its native reference"
fi

# --- the bindings the package ships must become the frontend's defaults ---
# the keys the package ships are the machine's when nobody changed them (the
# window keeps only the keys a person changed; fresh settings have none)
lua_script=dump-controls.lua
# made as the other runs are, so its project carries the test BIOS too
settings_config "$work/settings.keys.json" '{}'
if run_frontend "keys" "$work/settings.keys.json" 1; then
	lua_script=
	if python3 "$here/check-keybinds.py" "$work/keys.controls.json" \
		"$wb/default_keybinds.json" "3DO Controller" > "$work/keys.txt" 2>&1; then
		report "keybinds" PASS "$(cat "$work/keys.txt")"
	else
		report "keybinds" FAIL "$(head -1 "$work/keys.txt")"
	fi
else
	report "keybinds" FAIL "run did not report OK (see tests/work/keys.log)"
fi

echo
echo "$ok ok, $failed failed"
[ "$failed" -eq 0 ]
