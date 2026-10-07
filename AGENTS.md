# AGENTS.md - Opera core for Chimera

This repository ports Opera, the libretro 3DO emulator, to Chimera, a
frontend for tool-assisted speedruns. It produces one file,
`opera.chimeraCore`: the emulator built as a sandboxed guest (`core.wbx`)
plus the declarations Chimera reads. Chimera's sandbox, miniBox, runs that
guest on Linux and on Windows. The emulator itself is an upstream submodule;
this repository holds the integration layer, a patch set, the build and the
gates.

## Layout

- `extern/opera-libretro/` - git submodule: upstream Opera, pinned at an
  unmodified upstream commit.
- `patches/0001-chimera-hooks.patch` - this repository's changes to upstream.
- `waterbox/apply-patches.sh` - applies `patches/` to the submodule's working
  tree. `meson.build` runs it at every configure.
- `waterbox/cinterface.c` - the guest ABI layer. Both flavors compile it;
  `native-shim/emulibc.h` stands in for miniBox's emulibc natively.
- `waterbox/waterbox.config` - what Chimera is told about the machine:
  video, audio, the controller, settings, firmware.
- `waterbox/file_slots.json` - the files a project asks the user for.
- `waterbox/default_keybinds.json` - default key bindings.
- `waterbox/package-licenses.json`, `waterbox/licenses/` - the licence terms
  the package carries.
- `waterbox/setup-guest.sh` - configures the guest build.
- `waterbox/build-package.sh` - builds and installs the package.
- `waterbox/run-gate.sh` - the core gate; `run-native.c`, `run-wbx.c` and
  `gate-harness.h` are its drivers.
- `waterbox/tests/run-frontend.sh` - the frontend gate.
- `waterbox/tests/gen-fakecd.py` - makes the disc and dummy BIOS they boot.
- `waterbox/tests/run-roms.sh` - real-game legs over the user's own files.
- `tests/movies/` - the movie manifest `run-roms.sh` replays.
- `tests/roms-local/`, `tests/firmware-local/` - the user's discs and BIOS
  dumps. Ignored by git.
- `meson.build` - a cross configure is the guest; a native configure is the
  reference and the sandbox driver.
- `docs/PLAN.md` - milestones and the reasoning behind each decision.
- `.github/workflows/chimera.yml` - CI: core gate, frontend gate, publish.
- `build/` - every build tree. Ignored by git.

## Set up the build environment

```sh
sudo apt-get update
sudo apt-get install -y --no-install-recommends meson ninja-build build-essential python3

git submodule update --init

CHIMERA=$HOME/chimera
[ -d "$CHIMERA" ] || git clone https://github.com/ToolAssisted-run/chimera.git "$CHIMERA"
git -C "$CHIMERA" submodule update --init extern/chimera-common-minibox
MB=$CHIMERA/extern/chimera-common-minibox

[ -f "$MB/build/meson-linux/build.ninja" ] || meson setup "$MB/build/meson-linux" "$MB"
meson compile -C "$MB/build/meson-linux"
```

Opera is plain C: the plain miniBox build is enough. The scripts do not build
miniBox, so this comes first. The frontend gate also needs Mono, Xvfb, the
.NET SDK 8.0 and a built Chimera: see `docs/BUILDING.md`.

## Build

The shortest path to a package is one command. It configures the guest when
it is not configured, builds it, and writes
`$CHIMERA/build/Cores/opera.chimeraCore`. The patches need no step of their
own: every `meson setup` applies them.

```sh
./waterbox/build-package.sh -m "$MB" -r "$CHIMERA"

# the native reference and the guest, which the gates need
meson setup build/meson-native -Dminibox_dir="$MB"
ninja -C build/meson-native
MINIBOX_DIR="$MB" sh waterbox/setup-guest.sh -- -Dminibox_dir="$MB"
ninja -C build/meson-guest
```

## Install the core into Chimera

`build-package.sh -r "$CHIMERA"` installs it: `$CHIMERA/build/Cores/` is the
cores folder of a Chimera source checkout. For a release bundle, copy the
`.chimeraCore` file into the `Cores` folder beside `Chimera.exe` (or the
folder chosen in File > Core Manager > Change folder...). Chimera downloads
nothing. File > Core Manager lists the folder; Refresh List rescans it.

A package built by hand is stamped `<commit>+local`, with `-dirty` because
the patched submodule is a change in the tree. It is for testing. Only CI
sets `CORE_VERSION` and publishes.

## Test before you commit

```sh
./waterbox/run-gate.sh                                       # the core gate
./waterbox/tests/run-frontend.sh --chimera-root "$CHIMERA"   # the frontend gate
./waterbox/tests/run-roms.sh                                 # real games, local files only
```

The core gate must end with `0 failed`. It boots a synthesized disc and a
dummy BIOS, compares the sandboxed core with the same sources built for the
host (video, audio, lag, memory), round-trips a savestate around every frame,
and checks turbo, save data and two settings. No leg of it can skip.

The frontend gate needs a built Chimera (`$CHIMERA/build/Chimera.exe`), the
installed package and `build/meson-native/run-native`. Every leg must report
PASS. CI publishes only when the core gate and the frontend gate are green.

`run-roms.sh` skips every leg unless discs and BIOS dumps are in
`tests/roms-local/` and `tests/firmware-local/`; a run that skipped
everything proved nothing. Real games and the disc change (`disc:swap`) are
covered only there.

## Rules of this repository

- Keep upstream as clean as possible. `extern/opera-libretro` stays pinned at
  an unmodified upstream commit; changes to it live in `patches/`, applied
  by `waterbox/apply-patches.sh`. Never commit inside the submodule. `git
  status` shows it modified once patched; that is expected.
- To change upstream code, edit the submodule's working tree, then update the
  patch so that the pinned commit plus `patches/` gives that same tree.
  `apply-patches.sh` skips a tree that has its marker, so an edited patch is
  not applied again to a tree that is already patched.
- Determinism is the product. The guest must not read host time, host
  randomness or anything else that differs between runs, and a savestate
  must round-trip. The gate checks it; a change that breaks it is a bug.
  `THREADED_DSP` stays off, and the `randomSeed` setting pins the seed
  upstream would otherwise take from the clock.
- Both flavors compile the same `cinterface.c` and the same upstream source
  list (`meson.build`). Keep it so.
- Run the gate before committing. A new leg needs a negative control: break
  the thing it checks, watch it fail, and say so in the commit.
- Never commit game files, BIOS or firmware: 3DO discs and BIOS dumps are
  copyrighted. They go in `tests/roms-local/` and `tests/firmware-local/`.
  Never add network access.
- Every component compiled into the package is declared in
  `waterbox/package-licenses.json`. The built package inherits
  opera-libretro's non-commercial restriction (`LICENSE`).
- Shell scripts stay executable (git mode 100755). CI runs them directly.
- Documentation prose is plain ASCII.
- Commit messages: a type and an optional scope (`fix(disc):`,
  `feat(package):`, `docs(issues):`, `ci:`), then a full sentence that states
  the outcome, such as `fix(disc): the tray opens and closes where the 3DO
  can see it`. The body says why and what was measured. A fix for a reported
  problem cites `ToolAssisted-run/chimera#N`: problems with this core are
  reported in the Chimera repository.
- Do not edit `.github/workflows` unless the task is the workflow.

## Where to read more

- `docs/BUILDING.md` - the full build, every option, troubleshooting.
- `docs/PLAN.md` - settings, firmware, the input wire, the disc change.
- `.github/workflows/chimera.yml` - the authoritative build recipe.
- In the Chimera repository: `docs/porting-a-core.md`, `docs/gates.md` and
  `docs/core-manager.md`.
