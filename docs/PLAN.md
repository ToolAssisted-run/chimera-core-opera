# chimera-core-opera: Opera (3DO) as a Chimera waterbox core

Panasonic 3DO emulation for Chimera, built from the libretro Opera core
compiled into miniBox's deterministic sandbox and packaged as `core.wbx` +
`waterbox.config`, the same shape as chimera-core-gpgx and the other core
repos. The same playbook as chimera-core-gpgx, executed the same day.

## Sources and their roles

- **BizHawk Consoles/3DO + waterbox/opera** (the author's own integration):
  the reference imitated almost verbatim - bizhawk.cpp's libretro-callback
  shims, the sync-settings catalogue (systemType/fontROM/videoStandard/
  port types), the controller catalogue, the firmware naming and hashes.
- **Upstream libretro/opera-libretro @ a501a27** (75 commits past the
  TASEmulators fork's 67a29e6 base; the VFS rework and RCHD landed in
  between): the emulation core, vendored as submodule
  `extern/opera-libretro`. THE RULE: keep upstream as clean as possible.
- **headlessOpera** (the author's testing harness): the .sol movie format
  (|r|UDLRSsBYAXlr....| - 1 console char + 16 joypad chars per port) and
  the JinglesDefense homebrew test (movie vendored here; the .iso itself
  is not in that repo either - a local-files test like the commercial roms).

## The fork diff (67a29e6..7d50087) - what survives, what dissolves

The TASEmulators fork carried ~228 diff lines. In the chimera design most
of them are unnecessary:

- **CD callbacks / open_cdimage bypass**: GONE. Discs mount as raw files
  (.iso/.cue/.chd) in the guest FS; upstream's retro_cdimage + VFS
  filestream (plain stdio underneath) reads them. `opera_cdrom_set_callbacks`
  is never called.
- **BIOS table bypass**: mostly gone. Firmware mounts under opera's own
  canonical filenames (panafz1.bin, goldstar.bin, ...), so the table lookup
  succeeds for the 10 dumps upstream knows. A ~8 line patch adds a fallback
  entry for dumps the table has never seen (BizHawk's extra systemTypes:
  FZ1_E, FZ10_J, Goldstar FC-1, Sanyo HC-21, 3DO-NTSC-1.0fc2).
- **XRGB8888 defaults**: gone - the `vdlp_pixel_format` core variable is
  answered as "XRGB8888" through the environment callback instead.
- **NVRAM-changed flags (opera_arm.c)**: gone - chimera's savedata channel
  reads the buffer when the user exports; no dirty flag needed.
- **KEPT: opera_madam.c `_inputPortsRead`** on PBus DMA (lag detection,
  feeds the InputWasRead export; ~3 lines).
- **KEPT (reshaped): retro_reset's NVRAM stash** - upstream round-trips
  NVRAM through a save-directory file across reset, which in a read-only
  guest FS silently WIPES it. The patch stashes it in memory instead
  (the fork's "Preventing NVRAM from being cleared on reset", rebased
  onto the new retro_reset_core shape).
- coretypes.h `#undef _LARGEFILE_SOURCE` if the musl build needs it.

## Settings (all of BizHawk's sync settings, chimera style)

`systemType` (12 options, exactly BizHawk's SystemType list - picks the
BIOS via per-option requiredWhen firmware conditions), `fontROM`
(none/panasonicFZ1Kanji/panasonicFZ10Kanji), `videoStandard`
(ntsc/pal1/pal2 - decides resolution and rate: 320x240@60, 320x288@50,
384x288@50), `port1`/`port2` (none/gamepad/mouse/flightStick/lightGun/
arcadeLightGun/orbatakTrackball). BizHawk's non-sync settings: none exist.

## Firmware

One decl per systemType option (requiredWhen {"setting":"systemType",
"is": <option>}), id = opera's canonical filename, sha1/size from the
author's BizHawk firmware DB (ideal dumps). Same for the two font ROMs
(requiredWhen fontROM is the matching option). All are copyrighted: real
runs need the user's own dumps; the public gate boots a deterministic
dummy BIOS (garbage but identical in both flavors).

## Input (the wire)

0 Reset, 1 Previous Disc, 2 Next Disc, then per port (P1 at 3, P2 at 45):
gamepad {Up,Down,Left,Right,X,P,A,B,C,L,R} (11), mouse buttons
{Left,Middle,Right,Fourth} (4), flight stick {Up,Down,Left,Right,Fire,
A,B,C,X,P,LT,RT} (12 - including C, which BizHawk's definition forgot to
declare), light gun {Trigger,Select,Reload,Offscreen} (4), arcade light gun
{Trigger,Select,Start,Reload,AuxA,Offscreen} (6), trackball {StartP1,
StartP2,CoinP1,CoinP2,Service} (5) = 42 buttons per port, 87 total (wide
SetButton channel). Axes (SetAxis): per port Mouse X/Y (-64..64), Flight
Stick H/V/Alt (-64..64), Gun Screen X/Y (-32768..32767), Trackball X/Y
(-64..64) = 18 axes. The guest maps wire -> controllerData exactly like
bizhawk.cpp's processController, keyed by the port-type settings.

## Milestones

- **M1+M2 - native reference + core.wbx + gate**: repo, submodule, patches,
  cinterface.c (plain C port of bizhawk.cpp behind the chimera guest ABI,
  shared by both flavors), run-native/run-wbx over gate-harness.h (the gpgx
  harness with opera's sol format), dummy-BIOS smoke legs: equivalence +
  per-frame savestate round-trips over a synthesized iso. Memory layout:
  BizHawk used sbrk 256MB + mmap 256MB (opera allocs its RAM/VRAM arena).
- **M3 - package**: waterbox.config (settings, 87-button wire + 18 axes,
  lag group, extensions .iso/.cue/.chd/.bin), file_slots.json (cd slot,
  min 1 max unbounded = swap order), firmware decls, default_keybinds.json
  (BizHawk's 3DO defctrl block), build-package.sh, CI.
- **M4 - frontend gate**: package inside Chimera headless; RAM digest vs
  native needs a real BIOS, so the frontend legs assert the load-error path
  cleanly reports missing firmware, and keybind adoption; the RAM-equality
  leg joins run-roms once real files exist.
- **M5 - real games (local)**: run-roms.sh over tests/roms-local/ +
  tests/firmware-local/ (canonical bios names): JinglesDefense homebrew
  movie from headlessOpera first, commercial discs after. Disc swapping
  proof needs a multi-disc game.
- **M6 - tooling**: registers/trace via the ARM core, later.

## Build

Same commands as chimera-core-gpgx (meson native + setup-guest.sh cross
build over miniBox, C only; package via build-package.sh).

## Milestone log

- 2026-08-26: repo created, upstream pinned at a501a27.
- 2026-08-26: M1-M4 DONE the same day, all gates green. The patch set held
  at 35 lines (madam lag hook, reset NVRAM stash, unknown-BIOS fallback);
  everything else the fork carried dissolved into environment-callback
  answers (XRGB8888, region, bios/font by mounted canonical filename, and
  the NEW randomSeed setting pinning upstream's time(NULL) fallback - a
  wallclock in the machine, caught by the gate design before it could bite).
  run-gate.sh 6/6 over a synthesized machine: a handwritten ARM dummy BIOS
  (vectors parked on branch-to-self; a RAM counter as execution witness -
  and it must be BIG-endian, opera byteswaps ROM dumps at load) + a random
  iso; equivalence, per-frame savestate round-trips, NVRAM savedata trees,
  videoStandard and randomSeed legs. tests/run-frontend.sh 3/3 (System RAM
  byte-identical inside Chimera via the real firmware channel - an
  unrecognized dump is usable by design; PAL frame height; keybinds).
  Package installs as build/Cores/opera.chimeraCore; CI written.
  FOUND AND FIXED IN CHIMERA (be85106): ce_session_open's abort path
  double-freed the package on any post-copy failure - Opera's missing-BIOS
  refusal was the first Init failure ever to travel it.
  romFile is "rom.iso": upstream dispatches disc opens by extension, so the
  bare (non-project) mount carries one.
  DEVIATIONS from the BizHawk build, both deliberate: THREADED_DSP off (a
  worker thread trades determinism for speed the gate cannot accept) and
  HAVE_CDROM off (physical drive access, dead code in a sandbox).
  REMAINING: real-game verification (drop jdef.iso + panafz1.bin into
  tests/*-local/ and run-roms.sh lights up; commercial discs after), disc
  swapping (wire reserved; upstream has no public swap entry point yet -
  cdimage_ode_launch is static), the exotic input devices are wired but
  untested against real games, CI first run.

## A disc change is a dipir, and the 3DO reboots into the new disc (2026-09-23)

Supreme Warrior (two discs) never took its second disc: the tray opened and
closed, the drive reported the new disc, and nothing happened. No 3DO
emulator handles this (BizHawk #4708 is the same symptom in Opera). The
Portfolio OS source (github.com/trapexit/portfolio_os, 1995) says what the
console actually does, and the BIOS disassembly confirms it:

1. The drive raises XBUS poll bit 0x80 - `XBUS_POLL_MEDIA_ACCESS` in the OS's
   clio.h; this tree calls it POLRE. Nothing raised it.
2. The OS sees it the next time it takes the bus (`GrabXBus`,
   app/oper/xbusdevice.c), stops video and DMA (`Duck`) and calls
   `SoftReset()`: push registers, write CSTAT `SOFTRES|CLRDIP` (0x30), a slide
   of no-ops, pop them, return with the ROM's answer in r0.
3. The soft reset is the ARM's reset EXCEPTION - registers and memory kept,
   r14_svc pointing back into that slide, the primary ROM mapped. The ROM's
   reset handler reads CSTAT and takes its dipir path only when it reads
   DIPIR_RESET (0x40; 1 or 2 is a cold boot, anything else hangs). Dipir keeps
   the incoming lr and returns to it (`ldmia sp!,{sp,pc}^`, dipir/dipir.s).
4. Dipir reads the new disc and decides. A disc the running title will share
   (its volume label marked as a data disc, or the title's binary marked
   `_3DO_DATADISCOK`) is handed back to the OS, which completes its "wait for
   dipir end" requests. Anything else is rebooted into: `hardboot()` drives
   ADBIO pin 3, the watchdog reset output, and the console comes up on the
   new disc. Supreme Warrior's discs carry no data-disc flag, so with this OS
   a swap reboots into the other disc - as a real 3DO does.

Opera had none of the four. Now (libopera, in the hooks patch):

- closing the tray on a disc latches media access (`opera_cdrom_door_close`);
- a CSTAT soft reset sets DIPIR_RESET and is taken at the end of the storing
  instruction as the reset exception (`arm_soft_reset_exception`) - it used to
  re-initialise the whole machine, a power cycle the OS cannot come back from;
- ADBIO pin 3 driven high asks for a hardware reset, taken between frames by
  the frontend's own reset (`retro_reset_core`, NVRAM kept). An in-place
  re-initialisation was tried first and is not a power-on: the ROM rebooted
  into the new disc and stalled in its logo;
- a reset drive forgets a pending "medium changed" (it is a file-level static,
  and it survived the reset: the first command after power-on failed).

**Measured** (tests/run-roms.sh, leg `disc:swap`, Supreme Warrior with the
real panafz1 BIOS): the tray opened at frame 5000 on disc 1 and closed on
disc 2 a second later; at frame 16000 the picture is byte-identical to a
direct boot of disc 2 at frame 12000 (its gong menu, at the Wind location),
native == sandbox. With media access disabled the leg fails ("not where
booting the second disc puts it"); the leg also refuses params under which a
run that never swapped already shows the second disc.

**Not yet seen:** the game's own "insert the other disc" prompt (choosing a
disc-2 opponent from disc 1). What the console does there is the same dipir,
so the same reboot is expected; whether the game then resumes where it asked
(the manual says either disc can start it) is what the user's test will show.
