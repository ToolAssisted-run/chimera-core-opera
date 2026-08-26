#!/usr/bin/env python3
# Deterministic synthetic assets for the gate. Real 3DO BIOSes and discs are
# copyrighted, so the public smoke legs boot a tiny handwritten ARM "BIOS"
# over a pseudo-random disc image: not a runnable game, but the whole
# pipeline runs - BIOS load, disc open, the ARM core executing real code
# that fills System RAM with a counter (a live digest witness), all exactly
# identical in both flavors. Real-game legs use the user's own dumps.
#
# Usage: gen-fakecd.py --iso <outfile> <mib> <seed>
#        gen-fakecd.py --bios <outfile>
import struct
import sys

def rng_bytes(n, seed):
    out = bytearray()
    x = seed
    while len(out) < n:
        x = (x * 6364136223846793005 + 1442695040888963407) & (2**64 - 1)
        out += struct.pack("<Q", x)
    return bytes(out[:n])

if sys.argv[1] == "--bios":
    # ARM code at the reset vector (ROM1 maps at 0x03000000, where the CPU
    # starts). First park every exception vector (DRAM 0x00..0x1C) on a
    # branch-to-self, so a stray interrupt can never walk the PC into the
    # wild (upstream's MADAM peek does not mask its register index, and a
    # runaway PC eventually reads out of bounds). Then fill a small DRAM
    # window at 0x8000 with an incrementing counter - the RAM digest
    # witnesses real execution - and spin.
    code = [
        0xE3A01000,  # mov r1, #0
        0xE59F0020,  # ldr r0, [pc, #32]    ; the 0xEAFFFFFE literal below
        0xE4810004,  # vloop: str r0, [r1], #4
        0xE3510020,  # cmp r1, #32
        0x3AFFFFFC,  # blo vloop
        0xE3A00000,  # mov r0, #0
        0xE3A01C80,  # mov r1, #0x8000
        0xE4810004,  # cloop: str r0, [r1], #4
        0xE2800001,  # add r0, r0, #1
        0xE3510C81,  # cmp r1, #0x8100
        0x3AFFFFFB,  # blo cloop
        0xEAFFFFFE,  # b . (also the vector literal)
    ]
    rom = bytearray(1024 * 1024)
    for i, insn in enumerate(code):
        # 3DO BIOS dumps are big-endian; opera byteswaps them at load
        rom[i * 4:i * 4 + 4] = struct.pack(">I", insn)
    open(sys.argv[2], "wb").write(rom)
    print("dummy bios written")
    sys.exit(0)

if sys.argv[1] == "--iso":
    out, mib, seed = sys.argv[2], int(sys.argv[3]), int(sys.argv[4])
    open(out, "wb").write(rng_bytes(mib * 1024 * 1024, seed))
    print(f"{out}: {mib} MiB")
    sys.exit(0)

sys.exit("usage: gen-fakecd.py --bios <out> | --iso <out> <mib> <seed>")
