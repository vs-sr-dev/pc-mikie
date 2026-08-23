# pc-mikie

Static recompilation of Konami's **Mikie** (1984, arcade) from MC6809 machine code to
portable C — with every instruction verified against MAME.

> **4 026 322 instructions from reset with PC, all eight registers and the cycle counter
> identical to MAME, across 124 vblank interrupts — and 13 of 13 rendered frames
> pixel-identical across the whole attract loop.**

This repository holds the **tools and the documentation**. It contains no ROM data, no
extracted graphics and no generated code. See [Scope](#scope) below.

---

## What this is

Mikie runs on Konami's GX469 board: an MC6809E at 1.536 MHz for the game, a Z80 at
3.579545 MHz driving two SN76489A chips for sound, and a vertical 224×256 screen at 60.59 Hz.
The program is 40 KB of 6809 code and under 4.5 KB of RAM.

There are three honest ways to move that onto a PC:

| | approach | trade-off |
|---|---|---|
| A | write a single-game emulator | correct on day one, but it is an emulator, not a port; no help on weaker targets |
| B | **static recompilation** | native speed, real source, portable to hardware an emulator cannot reach |
| C | full reimplementation from extracted assets | most portable and moddable, most work |

This project takes **B**, using **A's reference — MAME — as an executable oracle**. That
combination is the whole point: you get native C *and* instruction-exact proof that it
behaves like the arcade board.

## Why the oracle matters

Porting from a CD or a cartridge, you judge correctness by looking at the result. Porting an
arcade game, a cycle-accurate reference already exists and ships with a debugger. Every bug
below surfaced as a single line naming an address and two values, instead of as a mysterious
crash minutes later:

- a DIP switch idle value wrong by one bit
- `RTI` costing 15 cycles rather than 6
- the interrupt stacking the wrong return address (symptom appeared 456 instructions later)
- the `CWAI` path not paying the 19-cycle interrupt entry — **identical PC and registers**,
  only the cycle counter off
- an instruction falling through into a phantom label *inside another opcode*, which showed
  up as a PC off by one **6.9 million instructions** into a trace

The reference lied twice, too, in ways that looked like success. Both are written up in
[docs/04-verification.md](docs/04-verification.md).

---

## Documentation

| | |
|---|---|
| [01-hardware.md](docs/01-hardware.md) | the Konami GX469: memory maps, video model, palette, the `CWAI` vblank sync |
| [02-static-analysis.md](docs/02-static-analysis.md) | telling code from data; jump-table resolution; verifying the decoder |
| [03-transpiler.md](docs/03-transpiler.md) | the 6809 → C code generation model and the details that bite |
| [04-verification.md](docs/04-verification.md) | the MAME harness, deriving the cycle table, measuring interrupt latency |
| [05-porting-gotchas.md](docs/05-porting-gotchas.md) | a checklist of what is easy to get wrong |

Much of this is not Mikie-specific. `tools/m6809.py` is a complete, independently verified
MC6809 decoder and cycle table; the trace-diff method applies to any MAME-supported target.

---

## Scope

**BYOA — bring your own assets.** Nothing here is derived from Konami's ROMs.

Included:
- `tools/` — analysis, decoding, recompilation and verification tooling (Python + MAME Lua)
- `src/` — the host runtime the generated code links against, written from scratch:
  memory map, flag and ALU helpers, vblank/IRQ scheduling, and the video hardware
- `docs/` — the hardware reference and the write-up

Not included, and not distributable from here:
- the ROM set — supply your own `mikie.zip`
- extracted graphics, palettes or text
- `src/gen/mikie_gen.c`, the recompiled output — it is a translation of copyrighted code,
  and it is produced on your machine from your own ROM by `tools/transpile.py`

You will also need a MAME build (0.287 or near) and `unidasm` from the same source tree.

---

## Using the tools

Expected working layout (the tools use relative paths):

```
rom/          extracted ROM files + maincpu.bin (64K image, program mapped at $6000)
disasm/       unidasm output
trace/        MAME traces and derived coverage data
analysis/     static analysis output
src/          the runtime (shipped); src/gen/ receives the generated C
build/
```

`rom/maincpu.bin` is a 64K image with the three program ROMs placed at their board
addresses (`n14.11c` at `$6000`, `o13.12a` at `$8000`, `o17.12d` at `$C000`) and the rest
zero-filled. The graphics ROMs and PROMs are read individually from `rom/` by the video
code, under their MAME filenames.

```sh
# 0. extract the ROMs and build the 64K program image yourself

# 1. coverage trace: which addresses are really executed
mame mikie -rompath . -debug -debugger none -autoboot_delay 0 \
     -autoboot_script tools/trace_cov.lua -seconds_to_run 41 \
     -video none -sound none -nothrottle -skip_gameinfo
python tools/cov_extract.py          # -> trace/coverage.bin, indirect_targets.json

# 2. static analysis: code vs data, jump tables, call graph
python tools/analyze.py              # -> analysis/static.json

# 3. sanity-check the decoder against unidasm
python tools/verify_decoder.py

# 4. recompile
python tools/transpile.py            # -> src/gen/mikie_gen.c

# 5. build (needs GCC or Clang: the dispatch table uses label-as-value)
gcc -O1 -Isrc -DMIKIE_TRACE -o build/mikie_trace.exe \
    src/gen/mikie_gen.c src/mikie_main.c

# 6. verify against MAME, instruction by instruction
MIKIE_TRACE_SECS=13.5 mame mikie -rompath . -debug -debugger none \
     -autoboot_delay 0 -autoboot_script tools/trace.lua -seconds_to_run 14 \
     -video none -sound none -nothrottle -skip_gameinfo
./build/mikie_trace rom/maincpu.bin 4300000 > trace/ours.log 2> trace/ours.err
python tools/difftrace.py

# 7. compare a rendered frame against a MAME snapshot of the SAME frame number
MIKIE_SNAP_FRAMES=1818 mame mikie -rompath . -autoboot_delay 0      -autoboot_script tools/snap_frame.lua -seconds_to_run 31      -video none -sound none -nothrottle -skip_gameinfo      -snapshot_directory ref/frames
MIKIE_DUMP_FRAME=1818 ./build/mikie rom/maincpu.bin
python tools/compare_frame.py trace/frame.raw ref/frames/mikie/0000.png
```

Comparing at an exact frame number rather than "about the same time" matters: the
recompiled code is cycle-accurate, so the two should agree pixel for pixel, and any
mismatch is a real rendering bug rather than a timing artefact.

Graphics extraction (tiles, sprites, the two-stage indirect palette) is in
`tools/gfxdecode.py`; it reproduces MAME's `gfx_layout` decoding generically.

> Traces run about **23 MB per emulated second**. They are regenerable artefacts — delete
> them, do not archive them.

---

## Status

| part | state |
|---|---|
| MC6809E semantics | **verified**, 4.03 M instructions |
| Cycle timing | **verified**, counter included |
| Vblank IRQ and `CWAI` | **verified**, 124 interrupts |
| Memory map and I/O | verified as far as boot and attract exercise it (30.5 s) |
| Video renderer | **verified**, 13/13 frames pixel-identical |
| Audio (Z80 + 2× SN76489A) | not started |

---

## Licence

Tools and documentation: MIT (see [LICENSE](LICENSE)).

Mikie is © 1984 Konami. Nothing in this repository is derived from or reproduces Konami's
code or data, and none of it is usable without a ROM set you already own.
