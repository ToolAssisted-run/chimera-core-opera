# chimera-core-opera

**Opera (3DO) as a Chimera waterbox core** - the
[libretro Opera core](https://github.com/libretro/opera-libretro)
(Panasonic 3DO and friends), compiled into
[miniBox](https://github.com/ToolAssisted-run/chimera-common-minibox)'s
deterministic sandbox and packaged as a Chimera core (`core.wbx` +
`waterbox.config`), the same shape as
[chimera-core-gpgx](https://github.com/ToolAssisted-run/chimera-core-gpgx)
and the other core repos.

The integration imitates the author's own BizHawk Opera port
(`src/BizHawk.Emulation.Cores/Consoles/3DO` + `waterbox/opera`) almost
verbatim, on a current upstream pin, with upstream kept as clean as possible:
the whole patch set is ~35 lines (see `patches/` and `docs/PLAN.md`). There
is no host-side CD plumbing at all - disc images (.iso/.cue/.chd) are
mounted raw into the guest filesystem and upstream's own VFS reads them. The
BIOS and font roms arrive through Chimera's firmware channel under opera's
canonical dump filenames, driven by the systemType/fontROM settings.

Status and plan: `docs/PLAN.md`.

## Build and test

```
# native reference + sandbox drivers
meson setup build/meson-native && ninja -C build/meson-native

# the guest core (needs a built miniBox checkout, e.g. chimera/extern/tools/chimera-common-minibox)
sh waterbox/setup-guest.sh && ninja -C build/meson-guest

# the equivalence gate: native == sandbox == savestate-rerecord on video,
# audio, lag and every memory domain, over a synthesized deterministic
# machine (real 3DO dumps are copyrighted and stay out of this repo)
./waterbox/run-gate.sh

# real games: drop images into tests/roms-local/ and BIOS dumps (canonical
# names: panafz1.bin, ...) into tests/firmware-local/, then
./waterbox/tests/run-roms.sh
```
