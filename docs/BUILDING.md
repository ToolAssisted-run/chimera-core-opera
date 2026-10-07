# Building the Opera core

This repository builds Opera, the libretro 3DO emulator, as a core for
Chimera. The result is one file, `opera.chimeraCore`, which Chimera loads.
The steps below are the ones this repository's CI runs on a fresh clone
(`.github/workflows/chimera.yml`); when this document and the workflow
disagree, the workflow is right.

Placeholders used below:

- `<repo>` - the checkout of this repository. Commands run from `<repo>`
  unless a block says otherwise.
- `<chimera>` - a checkout of https://github.com/ToolAssisted-run/chimera.
- `<miniBox>` - `<chimera>/extern/chimera-common-minibox`, a git submodule
  of Chimera. It is the sandbox host and the guest toolchain.

## Requirements

Cores are built on Linux, x86-64. CI uses GitHub's `ubuntu-latest` runner.

For the package and the core gate (workflow job `core-gate`):

```sh
sudo apt-get update
sudo apt-get install -y --no-install-recommends meson ninja-build build-essential python3
```

For the frontend gate, which builds all of Chimera (workflow job
`frontend-gate`):

```sh
sudo apt-get update
sudo apt-get install -y --no-install-recommends \
  meson ninja-build build-essential cmake pkg-config python3 \
  mono-complete xvfb \
  libgl1-mesa-dev libx11-dev libxext-dev libasound2-dev
```

The frontend gate also needs the .NET SDK 8.0. The workflow installs it with
`actions/setup-dotnet@v4` and `dotnet-version: '8.0'`. By hand, Chimera's
README gives the command and says why the distribution's SDK is not enough:

```sh
curl -sSL https://dot.net/v1/dotnet-install.sh | bash -s -- --channel 8.0
```

No compiler version is pinned. The build uses the gcc that `build-essential`
installs, and the package records that gcc version in its `build.json`. Opera
is plain C: it needs miniBox's plain guest toolchain, not the C++ one.

After the clones the build downloads nothing. The upstream emulator is a git
submodule, and the gates make their own test disc and BIOS.

## Get the sources

This repository, with its submodule. The workflow uses `actions/checkout@v6`
with `submodules: true`:

```sh
git clone https://github.com/ToolAssisted-run/chimera-core-opera.git
cd chimera-core-opera
git submodule update --init
```

The submodule is `extern/opera-libretro`
(https://github.com/libretro/opera-libretro.git), pinned at an unmodified
upstream commit.

A Chimera checkout. CI builds against Chimera's `main` branch
(`CHIMERA_REF: main`). The package and the core gate need only the miniBox
submodule:

```sh
git clone https://github.com/ToolAssisted-run/chimera.git <chimera>
cd <chimera>
git submodule update --init extern/chimera-common-minibox
```

The frontend gate builds Chimera itself, and for that the workflow checks
Chimera out with every submodule:

```sh
cd <chimera>
git submodule update --init --recursive
```

Where each script looks for Chimera when it is not told:

| script | option | fallback |
|---|---|---|
| `waterbox/build-package.sh` | `-r <chimera>`, `-m <miniBox>` (or `MINIBOX_DIR`) | `../chimera` beside `<repo>`, then `$HOME/chimera`; miniBox defaults to `<chimera>/extern/chimera-common-minibox` |
| `waterbox/setup-guest.sh` | `-m <miniBox>` (or `MINIBOX_DIR`) | `$HOME/chimera/extern/chimera-common-minibox` |
| `meson.build`, both builds | `-Dminibox_dir=<miniBox>` | `../chimera/extern/chimera-common-minibox` beside `<repo>`, else an error |
| `waterbox/tests/run-frontend.sh` | `--chimera-root <chimera>` | `../chimera` beside `<repo>`, then `$HOME/chimera` |

The fallbacks differ from script to script. Pass the paths.

## Build miniBox

The host library and the guest toolchain (musl). The scripts do not build
miniBox; do it first:

```sh
mb=<chimera>/extern/chimera-common-minibox
[ -f "$mb/build/meson-linux/build.ninja" ] || meson setup "$mb/build/meson-linux" "$mb"
meson compile -C "$mb/build/meson-linux"
```

The workflow caches `<miniBox>/build/meson-linux` with `actions/cache@v4`. By
hand, keep the directory: the test in the first line skips the configure when
it is already there.

`setup-guest.sh` takes the guest toolchain from `build/meson-linux`, or from
`build/meson-cpp` if only that one is built. The sandbox driver `run-wbx`
links the host library from `build/meson-linux`, so the gates need that one.

## Build the core

### Patches

Upstream is not edited in place. The changes this core needs are in
`patches/` (one file today, `0001-chimera-hooks.patch`), and
`waterbox/apply-patches.sh` applies them to the working tree of
`extern/opera-libretro`.

`meson.build` runs `apply-patches.sh` at every configure, of either build, so
there is no separate step. The script decides once for the whole series: if
`libopera/opera_madam.c` in the submodule already contains the marker
`opera_input_ports_read`, it does nothing; otherwise it applies every
`patches/*.patch` in name order with `git apply`. A tree that has the marker
is taken to have the whole series.

After that the submodule's working tree is modified and `git status` shows
`extern/opera-libretro` as changed. That is expected. The pinned commit does
not move, and nothing is committed inside the submodule.

### The native reference

It is needed by the gates, not by the package:

```sh
meson setup build/meson-native -Dminibox_dir="$mb"
ninja -C build/meson-native
```

This builds `run-native` and `run-wbx` in `build/meson-native`. `run-native`
is the same `waterbox/cinterface.c` and the same upstream sources as the
guest, built for the host; it is what the gates compare the sandboxed core
against. `run-wbx` runs the guest through miniBox's host library. The
frontend gate needs `run-native` alone, and the workflow's `frontend-gate`
job builds just that target:

```sh
ninja -C build/meson-native run-native
```

### The guest

```sh
MINIBOX_DIR="$mb" sh waterbox/setup-guest.sh -- -Dminibox_dir="$mb"
ninja -C build/meson-guest
```

`setup-guest.sh` writes the cross file `build/guest-cross.ini` and configures
`build/meson-guest`. It also takes `-m <miniBox>`; arguments after `--` go to
`meson setup`. The cross file holds absolute paths of this machine; it is
under `build/`, which git ignores. The result is
`build/meson-guest/core.wbx`.

Both builds leave two upstream options off, on purpose: `THREADED_DSP`,
because a worker thread trades determinism for speed, and `HAVE_CDROM`,
physical drive access, which is dead code in a sandbox.

## Build the package

```sh
./waterbox/build-package.sh -m "$mb" -r <chimera>
```

Options:

- `-r <chimera>` - the Chimera checkout the package is written into.
- `-m <miniBox>` - the miniBox checkout. Default:
  `<chimera>/extern/chimera-common-minibox`. `MINIBOX_DIR` does the same.

There is no `-o` option; the staging directory is always
`<repo>/build/package-staging`.

What the script does, in order:

1. Runs `waterbox/setup-guest.sh` if `build/meson-guest` is not configured.
   That stops with a message when the miniBox guest toolchain is not built.
2. Builds `core.wbx` and checks it with miniBox's
   `source/guest/check-wbx.sh`.
3. Stages `core.wbx`, `waterbox.config`, `default_keybinds.json`,
   `file_slots.json` and the licence files that
   `waterbox/package-licenses.json` declares.
4. Stamps the version into the staged `waterbox.config` and writes
   `build.json` (source commit, toolchain, miniBox commit, upstream pin).
5. Writes `<chimera>/build/Cores/opera.chimeraCore`, packs it a second time
   and stops if the two files differ.
6. Removes `<chimera>/build/CoreCache/opera-*`, so Chimera does not load an
   older extraction.

It does not build the native reference; the package does not need it.

The version of a package is the commit it was built from. CI passes
`CORE_VERSION` (the full commit) and publishes that package. A package built
by hand, without `CORE_VERSION`, is stamped `<12-character commit>+local`,
or `<commit>-dirty+local` when `git diff --quiet HEAD` finds changes in the
tree. In this repository the patched submodule counts as a change, so a
package built by hand reads `-dirty` once the patches are applied.
`versionDate` is the date of the commit in UTC, never the date of the build.
Hand-built packages are for testing: Chimera's publishing script refuses a
version that carries `+local` or `-dirty`.

The package inherits opera-libretro's terms, the FreeDO-descended
non-commercial restriction included. See `LICENSE` and
`waterbox/package-licenses.json`.

## Install it into Chimera

Chimera ships no cores and downloads nothing: it has no network code. A core
gets into Chimera because somebody puts the file there.

- In a Chimera source checkout the cores folder is `<chimera>/build/Cores/`.
  `build-package.sh -r <chimera>` writes the package there, so there is
  nothing else to do.
- In a release bundle, copy `opera.chimeraCore` into the `Cores` folder
  beside `Chimera.exe` (or into the folder chosen in File > Core Manager >
  Change folder...).
- File > Core Manager lists what is in that folder. Refresh List rescans it.

The same package file works on Linux and on Windows. The guest inside it is
run by Chimera's sandbox (miniBox) on either.

Packages built by CI are on this repository's Releases page,
https://github.com/ToolAssisted-run/chimera-core-opera/releases : a rolling
`dev` release and dated `nightly-YYYY-MM-DD` releases. The asset is named
`opera-<version>.chimeraCore`. Download it and put it in the `Cores` folder.

To use the core: File > New Project... and pick it. To play a disc with no
project, start Chimera with `--core=<package> <rom>`.

## Run the gates

There are two gates that CI runs and needs green before it publishes, and
one that runs only where the user's own discs and BIOS are.

Real 3DO BIOS dumps and discs are copyrighted. The two CI gates therefore
boot a synthesized machine: a pseudo-random disc image and a handwritten
dummy BIOS, both made on the spot by `waterbox/tests/gen-fakecd.py`. The
dummy BIOS fills a window of RAM with a counter, which shows that the ARM
core really ran. They need nothing provisioned.

### The core gate

```sh
./waterbox/run-gate.sh
```

It needs `run-native` and `run-wbx` in `build/meson-native`, and
`build/meson-guest/core.wbx`, and nothing else: no .NET, no Mono, no X.

```
./waterbox/run-gate.sh [-n <native build dir>] [-g <guest build dir>]
```

| leg | what it proves |
|---|---|
| `smoke:equivalence` | over 300 frames with the pad exercised, the frame count, the vsync rate and the video, audio, lag and memory-domain digests are identical between the native build and `core.wbx` |
| `smoke:turbo` | with the renderer off for the first half of the run (`run-wbx --turbo`), the machine, the sound, the lag count and the pictures of the second half are unchanged |
| `smoke:savestate` | saving and loading the whole machine around every frame (`run-wbx --rerecord`) changes no digest |
| `smoke:executed` | the dummy BIOS really executed: System RAM does not digest as untouched |
| `savedata:export` | the NVRAM both flavors export (`NVRAM.ram`) is identical |
| `savedata:seeded` | NVRAM a project supplies reaches the machine and comes back unchanged (run on the native build) |
| `settings:videoStandard` | `videoStandard` set to `pal1` reaches the guest in both flavors: vsync is 50/1 for PAL and 3928227/65536 for NTSC |
| `settings:randomSeed` | `randomSeed` pins the core's random seed in both flavors: the core reports a fixed seed, never a time-based one |

No leg of this gate can skip. The last line reads `N ok, N failed`, and
anything other than PASS counts as failed.

### The frontend gate

It runs the package inside Chimera and compares the machine with the native
reference. It needs a built Chimera, the installed package and `run-native`.
Build Chimera as the workflow does:

```sh
cd <chimera>
meson setup build/meson-linux --prefix "$PWD/build" --libdir dll
meson compile -C build/meson-linux
meson install -C build/meson-linux
dotnet build source/gui/Chimera.sln -c Release /nodeReuse:false -p:UseSharedCompilation=false
```

Then, from `<repo>`:

```sh
./waterbox/build-package.sh -m "$mb" -r <chimera>
./waterbox/tests/run-frontend.sh --chimera-root <chimera>
```

```
./waterbox/tests/run-frontend.sh [--chimera-root <chimera>] [--frames N]
```

It starts `<chimera>/build/Chimera.exe` under Mono with `--headless` and the
package. With `DISPLAY` unset it starts its own Xvfb; with `DISPLAY` set it
uses that display. The default is 300 frames. The dummy BIOS reaches the
core through Chimera's real firmware channel, which accepts a dump it does
not recognise.

| leg | what it proves |
|---|---|
| `disc:frontend` | after the frames, the whole System RAM domain inside Chimera is byte-identical to the native reference |
| `settings:videoStandard` | `videoStandard` set to `pal1` through the frontend's configuration builds a machine that matches its own native reference, with a 288-line frame |
| `keybinds` | from a configuration that has never seen this controller, the bindings in `default_keybinds.json` become the frontend's defaults |

Anything other than PASS counts as failed. Logs and dumps go to
`waterbox/tests/work/`, which git ignores.

### Real games (local files only)

```sh
./waterbox/tests/run-roms.sh
```

CI does not run it. It needs both flavors built, and the user's own files:
disc images in `tests/roms-local/` and BIOS dumps in `tests/firmware-local/`,
under the file names the core expects (`panafz1.bin` and the others). Git
ignores both directories.

- For each entry of `tests/movies/manifest.json` it replays the movie and
  requires native == sandbox == the per-frame savestate round-trip on every
  digest. An entry whose disc image or BIOS is not there reports SKIP and
  says which file to add.
- `disc:swap` proves a disc change on a two-disc game: the machine that
  swapped discs ends on the same picture as a machine that booted the second
  disc, native and sandboxed alike. It needs `tests/roms-local/swap/` with
  both discs' files and a `params` file that sets three frame counts
  (`swapAt`, `after`, `direct`), and `panafz1.bin`. Without them it reports
  SKIP.

With no local files every leg skips and the script exits 0. That is not a
pass: it ran nothing.

### What CI runs

`.github/workflows/chimera.yml` runs `core-gate` and `frontend-gate` on every
pull request and push to `main`, every day at 04:00 UTC, and on manual
dispatch. `publish` runs when both jobs passed and the event is not a pull
request. A push publishes the rolling `dev` release. The scheduled run
publishes `nightly-YYYY-MM-DD`, and only when `main` moved since the last
nightly. A manual run takes `kind`, `dev` or `nightly`; blank means `dev`.

## Files the core needs at run time

Discs, BIOS and font ROMs are never in this repository or in the package.
The user provides them. From `waterbox/file_slots.json` and
`waterbox/waterbox.config`:

- Discs. Required, one or more: `.iso` or `.bin` raw images, a `.cue` sheet
  (the track files it names join the project), or a `.chd`. The first disc
  is in the drive at boot. Their order is the order the Previous Disc and
  Next Disc inputs step through.
- Save data. Optional, at most one: `NVRAM.ram`, the file Emulator > Export
  Save Data... writes. It is the console's NVRAM, where a 3DO keeps saved
  games and its own settings.
- A BIOS. Required, exactly one, chosen by the `systemType` setting. The
  default, `panasonicFZ1U`, needs `panafz1.bin` (1048576 bytes, SHA-1
  `34BF189111295F74D7B7DFC1F304D98B8D36325A`). The other eleven values need,
  one each: `panafz1e.bin`, `panafz1j.bin`, `panafz10.bin`,
  `panafz10e-anvil.bin`, `panafz10j.bin`, `goldstar.bin`,
  `goldstar_fc1_enc.bin`, `sanyotry.bin`, `sanyo_hc21_b3_unenc.bin`,
  `3do_arcade_saot.bin` and `3do_devkit_1.0fc2.bin`.
- A Kanji font ROM, only when the `fontROM` setting asks for one. The
  default is `none`. `panasonicFZ1Kanji` needs `panafz1-kanji.bin`;
  `panasonicFZ10Kanji` needs `panafz10ja-anvil-kanji.bin`.

The `firmware` list in `waterbox/waterbox.config` is the authority: each
entry gives the file name, which setting value requires it, its size and its
SHA-1.

## Troubleshooting

- `chimera checkout not found; pass -r <path>` (`build-package.sh`) or
  `pass --chimera-root <path>` (`run-frontend.sh`): pass the Chimera
  checkout.
- `miniBox guest toolchain missing under <miniBox>/build` (`setup-guest.sh`,
  also reached through `build-package.sh`): build miniBox first. The message
  prints the command.
- `pass -Dminibox_dir=<miniBox checkout>` (meson): the fallback is
  `../chimera` beside `<repo>`; give the path.
- `native build missing` or `guest build missing` (`run-gate.sh`): the gate
  builds nothing. Build both flavors first; the message prints the commands.
- The first configure fails in `apply-patches.sh`: check that
  `extern/opera-libretro` is really there (`git submodule update --init`).
  Chimera's `docs/porting-a-core.md` warns that git, run in an empty
  submodule directory, answers for the repository above it.
- A change to a file in `patches/` has no effect: `apply-patches.sh` skips a
  tree that already has the marker. The submodule's working tree has to be
  back at the pinned commit before the series is applied again.
- A hand-built package reads `-dirty`: the patched submodule is a change in
  the tree. See "Build the package".
- `packaging is not deterministic` (`build-package.sh`): two packings of the
  same staging directory differed. The package's SHA-1 is the core's
  identity, so the script stops.
- `Chimera not built`, `package not installed` or `native reference not
  built` (`run-frontend.sh`): build Chimera, run `build-package.sh`, build
  `run-native`.
- `Xvfb not found (apt install xvfb)` (`run-frontend.sh`): install `xvfb`.
- A frontend leg fails with `no OK meta` or `run did not report OK`: read the
  log it names under `waterbox/tests/work/`. A first run that cannot make its
  configuration says `config bootstrap failed`; read
  `waterbox/tests/work/bootstrap.log`.
- The Chimera checkout moved: run `waterbox/setup-guest.sh` again. The cross
  file holds absolute paths; the script rewrites it and reconfigures.
