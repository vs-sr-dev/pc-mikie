# Verification: MAME as an executable oracle

The interesting thing about recompiling an arcade game — as opposed to porting from a CD or
a cartridge — is that a cycle-accurate reference implementation already exists and ships
with a debugger. You do not have to judge a port by looking at it.

**Result: 4 026 322 instructions from reset with PC, all eight registers and the cycle
counter identical to MAME, across 124 vblank interrupts.** 13.5 seconds of emulated time.

---

## The harness

MAME's debugger logs registers and `totalcycles` per instruction:

```
A=00 B=00 X=5FF0 Y=0000 U=0000 S=0000 DP=00 CC=50 CYC=14
CD98: LDA    #$55
```

The recompiled binary, built with `-DMIKIE_TRACE`, emits the same fields. `tools/difftrace.py`
walks both and reports the first divergence with context.

### Three things that are not optional

**`noloop`.** Without it MAME collapses repetition into `(loops for N instructions)` and the
log is useless as a reference.

**Headless.** `-video none -sound none`. Otherwise keystrokes that land in the MAME window
end up in the trace — see the war story below.

**Do not call `manager.machine:exit()` from the Lua script.** Exiting straight after
`trace off` can hang MAME with the trace file still buffered. Use `-seconds_to_run` and let
MAME shut down normally.

### Tracing a window instead of everything
A full trace runs about **23 MB per emulated second**. `difftrace.py` aligns automatically:
because the cycle counter is absolute, MAME's first traced state identifies a unique point
in our own trace, so you can trace only the interesting window and still compare exactly.

```
MIKIE_TRACE_SKIP=11 MIKIE_TRACE_SECS=2.5   # just the window where interrupts start
```

---

## Deriving the cycle table instead of transcribing it

Hand-copying the 6809 cycle table is a classic source of silent errors. `totalcycles` is
available in MAME debugger expressions, so the table can be **checked** rather than trusted:

```
trace file,maincpu,noloop,{tracelog "CYC=%d\n",totalcycles}
```

The delta between consecutive `CYC` values is the cost of the preceding instruction.
`tools/verify_cycles.py` compares that against `tools/m6809.py` for every traced instruction:

| | |
|---|---|
| instructions compared | 3 506 196 |
| cycle counts correct | 3 506 163 — **99.999 %** |
| distinct (mnemonic, mode, postbyte) forms correct | 145 |
| forms wrong | **1** |

The single wrong form was `RTI`: 15 cycles with E set, not the 6 in the "obvious" reading of
the table. Every `RTI` in Mikie is the 15-cycle form. Using 6 would have made every frame
300+ cycles short — a slow drift, nearly impossible to attribute by watching the game.

---

## Working out when the interrupt actually arrives

This part cannot be deduced from a datasheet, because it is a property of the whole machine.
It can be measured.

### The phase, from `CWAI`
During `CWAI` the CPU is halted and burns cycles until the interrupt, so it takes it
**exactly on the edge** with no end-of-instruction delay. Across 28 samples the measured
delay was 0 in all 28. That pins the phase:

```
edge(k) = (k * 153600000 - 90900) / 6059
```

### The latency, from everything else
Outside `CWAI` the delay is 15 to 21 cycles and always lands on an instruction boundary. It
is **not** "the first boundary after the edge": 2 to 4 boundaries get skipped, and the first
available one is only 0-5 cycles past the edge.

Testing every possible threshold against 89 samples:

```
  edge+13 :  42/89
  edge+14 :  63/89
  edge+15 :  89/89   <---
  edge+16 :  70/89
  edge+17 :  51/89
```

**`edge + 15` explains all 89, and is the only threshold that does.** Those 15 cycles are the
latency with which MAME's scheduler delivers the interrupt-line state change.

### The rule, implemented
- the IRQ line is asserted at the vblank edge;
- during normal execution the CPU takes it at the **first instruction boundary at or after
  edge + 15**;
- inside `CWAI` it takes it **on the edge**;
- interrupt entry costs **19 cycles**, including when leaving `CWAI` where the state is
  already stacked.

---

## What the harness actually caught

Each of these surfaced as one precise line naming an address and two values.

| # | first divergence at | cause |
|---|---|---|
| 1 | instruction 1 | the MAME log started a second after reset — `-autoboot_delay 0` |
| 2 | 512 097 | `LDB $2403`: DSW3 idle value, MAME `$FE`, ours `$FA` |
| 3 | 3 445 263 | first genuinely enabled IRQ — no cycle counter yet |
| 4 | 160 390 (windowed) | `IRQCK` stacked `pc + len` instead of `pc` |
| 5 | 175 140 (windowed) | PC and every register matched; `CYC` was off by exactly 19 — the `CWAI` path was not paying the interrupt entry cost |
| 6 | 179 709 (windowed) | `LDB $2402` read `$EF` in MAME, `$FF` for us |

Number 5 is the case for tracking cycles at all. Number 2 is a one-bit constant that, in a
port checked by playing it, would have shown up weeks later in a service screen.

---

## Two ways the oracle lied

Both produced results that looked fine.

### MAME truncated the trace in silence
Calling `manager.machine:exit()` right after `trace off` left the file buffered: 60 MB
written of 316 MB expected, no error, no warning. The comparison then passed "with no
divergence" simply because the reference ran out early.

A verification harness that can pass by running out of reference data is a harness that
will eventually tell you what you want to hear. `difftrace.py` now prints which side was
exhausted and after how many instructions, so a short reference is visible rather than
silently reassuring.

### Real keystrokes leaked into the reference
One comparison broke on `LDB $2402` with MAME reading `$EF` and the recompiled code `$FF`.
Bit 4 of that port is player 2's button 1. Querying MAME's idle port values gave `$FF`,
matching ours exactly — it was a key held down while the MAME window had focus.

Hence: generate the reference headless, always.

The general lesson is that an oracle deserves the same suspicion as the code it checks.
Both failures produced *plausible* output; neither threw an error.

---

## Verifying the video

The same idea applies to the screen, with one extra step: comparing at an exact **frame
number** rather than at an elapsed time. The CPU is cycle-accurate, so the two frames must
agree pixel for pixel; anything else is a real bug rather than a timing artefact.

`tools/snap_frame.lua` snapshots MAME at given frame indices (it works under `-video none`,
so video references are headless too), and `tools/compare_frame.py` diffs our raw RGB dump
against the PNG and writes an ours | MAME | difference strip.

**Result: 13 of 13 frames pixel-identical**, spread across the whole attract loop from the
boot self-test to the demo game.

### Splitting "wrong renderer" from "wrong data"
The useful question is not "is the renderer right?" but "given identical rules, did the two
start from identical data?"

`tools/dump_ram.lua` pulls `$2800-$3FFF` out of MAME at an exact frame, and
`tools/render_ref.py` renders that in Python and compares it with MAME's own snapshot,
trying all eight palette banks (the bank is driver state rather than memory, so it is
recovered by comparison).

Rendering **MAME's own RAM** reproduced MAME's snapshot with **zero differing pixels**. That
settles the rendering rules in one step, and every remaining difference has to come from the
data — which is a trace-diff problem, not a graphics problem. Without that split, a rendering
bug that did not exist would have been an easy thing to hunt for hours.

### The capture point, and a warning about tunable constants
The screen is drawn at the **end** of the visible period — on the vblank edge that
*terminates* the frame, before the next frame's handler runs. So MAME's frame N is our edge
**N+1**, captured at the edge with no offset.

Getting there was instructive. Capturing on the edge that *starts* frame N was wrong, and the
first fix attempt was a tunable "capture delay" in cycles after the edge. It worked: four
sample frames went to zero with a delay of 8000 cycles.

It was still the wrong model. Widening from 4 sample frames to 13 exposed one frame, inside
the boot self-test, that was 27% wrong at any delay under ~16000 — against ~250 for one frame
and ~600 for another. No constant satisfied all three, and the "correct" value drifted toward
the end of the frame. That drift *was* the answer.

The delay appeared to work because during normal play the game only touches video memory in
the vblank handler, so "shortly after edge N" and "at the end of frame N" show the same
image. The self-test, which writes video RAM continuously, broke the illusion.

> A knob you can tune until the numbers agree is an excellent way to not notice you have
> misunderstood something. The warning sign was never a failure — it was that the "right"
> value kept changing.

The knob is gone. The code captures at edge N+1 and needs no calibration.

---

## Reproducing

```sh
# 1. reference trace (headless)
MIKIE_TRACE_SKIP=0 MIKIE_TRACE_SECS=13.5 \
mame mikie -rompath . -debug -debugger none \
     -autoboot_delay 0 -autoboot_script tools/trace.lua \
     -seconds_to_run 14 -video none -sound none -nothrottle -skip_gameinfo

# 2. recompile and run
python tools/transpile.py
gcc -O1 -Isrc -DMIKIE_TRACE -o build/mikie_trace.exe \
    src/gen/mikie_gen.c src/mikie_main.c
./build/mikie_trace.exe rom/maincpu.bin 4300000 > trace/ours.log 2> trace/ours.err

# 3. compare
python tools/difftrace.py

# 4. video: snapshot MAME at exact frame numbers, then diff frame by frame
MIKIE_SNAP_FRAMES=120,400,700,980,1260,1540,1800 \
mame mikie -rompath . -autoboot_delay 0 -autoboot_script tools/snap_frame.lua \
     -seconds_to_run 31 -video none -sound none -nothrottle -skip_gameinfo \
     -snapshot_directory ref/frames
MIKIE_DUMP_FRAME=1800 ./build/mikie rom/maincpu.bin
python tools/compare_frame.py trace/frame.raw ref/frames/mikie/0006.png
```
