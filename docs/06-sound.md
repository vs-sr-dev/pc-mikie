# The sound board

The GX469's audio is a separate machine bolted to the side of the game: a **Z80 at
3.579545 MHz** with 8 KB of ROM and 1 KB of RAM, driving **two SN76489A PSGs at different
clocks**. The main CPU never reads anything back from it.

```
Z80 map
  0000-3FFF  R   program ROM (only 0000-1FFF populated; the rest of the region reads 0)
  4000-43FF  RW  RAM
  8000        W  data latch for the PSGs (ignored: see below)
  8002        W  strobe sn1        8003  R  command latch from the 6809
  8004        W  strobe sn2        8005  R  timer = (Z80 cycles / 512) & $FF

sn1  1.789772 MHz   (14.318181 / 8)
sn2  3.579545 MHz   (14.318181 / 4)
```

Two things drive the sound program:

- **the command latch.** The 6809 writes a byte to `$2400`, then pulses LS259 output 2
  (SOUNDON), which interrupts the Z80. The handler reads `$8003`. Note the edge: the LS259
  only pulses an output when the bit actually *changes*, so writing the same value twice
  interrupts nothing.
- **the timer at `$8005`.** A free-running counter, `(Z80 cycles / 512)`, with no register
  behind it. The music driver spins on bit 2 — one full period is 4096 Z80 cycles, about
  874 Hz — and that is the entire time base of the music. Get it wrong and the tempo is
  wrong; get it wrong *by a little* and the driver silently misses ticks when a routine
  runs long.

The data the PSGs latch really comes from `$8000`, but the sound program always writes the
same byte to `$8000` and to the strobe address, so taking the strobe's own data byte — as
MAME does — is equivalent.

---

## Cycle counting in the Z80 core

Two rules matter and neither is obvious from a T-state table.

**A memory access happens before the T-states of its own machine cycle are charged.** In
MAME's microcode `3 !! value = read()` emits `read(); icount -= 3;`. That is observable
here, not academic: `$8005` is sampled in the middle of an instruction, and `ld a,(nn)`
reads at +10 cycles from the start of the instruction, not at +13. Get it backwards and
one read in every 512 lands on the wrong side of a counter boundary.

**Q — the SCF/CCF quirk.** `SCF` and `CCF` merge A into the undocumented flags 5 and 3,
but they keep the previous flags 5 and 3 only if the *previous instruction did not write
F at all*: `yx = (F & q) | A`, where `q` is `0x28` when the last instruction left F alone
and `0` when it touched it. Any core that zeroes F from scratch in every ALU helper needs
this tracked explicitly.

Interrupt entry in IM 1 costs 13 T-states (2 of latency, 5, then two stack writes) and
refreshes R once. MAME logs no separate trace line for it: the interrupt sequence shows up
as part of the state of the instruction that follows, so a differ needs no special case.

---

## Tracing the sound CPU with MAME

`trace <file>,audiocpu,noloop,{tracelog "..."}` produces a file containing **only
disassembly** — no register lines, no error. `tracelog` resolves its symbols against, and
writes to, the debugger's *visible* CPU, which is the 6809. Asking a 6809 for `af` or `ix`
fails and the action is dropped silently.

`focus audiocpu` fixes the symbols but stops every other CPU, so the game no longer runs.
From Lua:

```lua
manager.machine.debugger.visible_cpu = manager.machine.devices[":audiocpu"]
```

`tools/trace_z80.lua` does that before starting the trace. Everything else is as for the
main CPU: `noloop` is mandatory, and never call `manager.machine:exit()`.

`tools/sound_events.lua` is the complementary view — every write the 6809 makes to the
LS259 and to the command latch, stamped with the exact 6809 cycle. MAME exposes no Lua
binding for `total_cycles()`, but `machine.time` is the local time of whichever device is
executing, and a CPU's local time advances in whole cycles, so the cycle number comes back
exactly.

---

## Converting between the two clocks

The 6809 cycle at which SOUNDON is pulsed decides which Z80 instruction boundary takes the
interrupt. Doing that conversion with the true hardware ratio — 3579545.25 / 1536000 —
does **not** reproduce MAME, because MAME holds a device clock as an integer number of Hz:
14318181/4 becomes 3579545, and a device's local time advances by a whole (truncated)
number of attoseconds per cycle.

```
        6809 cycle 23907847  (the first SOUNDON of the attract)
  true ratio      -> Z80 cycle 55715638
  MAME arithmetic -> Z80 cycle 55715635   <- the boundary MAME actually used
```

Seven parts in a hundred million; three cycles after twenty-four seconds. Three cycles is
enough to miss one turn of a twelve-cycle polling loop, and from there the sound driver's
state diverges permanently. If you want to diff against MAME past the first sound command,
use MAME's arithmetic:

```c
#define ATTO_6809  651041666666ULL   /* 1e18 / 1536000, truncated */
#define ATTO_Z80   279365114840ULL   /* 1e18 / 3579545, truncated */
z80_cycle = (m6809_cycles * ATTO_6809 + ATTO_Z80 - 1) / ATTO_Z80;   /* 128-bit */
```

The product overflows 64 bits after fourteen seconds of emulated time.

Interleaving needs no further thought: MAME synchronises the scheduler when one device
sets another's interrupt line, so running the Z80 up to the exact converted cycle before
delivering the event is what MAME effectively does anyway.

---

## The PSGs

Follow MAME's `sn76496` device with the SN76489A parameters: 15-bit shift register, taps
on bits 2 and 3, output not inverted, `/8` on the clock input. Worth knowing:

- **attenuation registers power up at 0, which is maximum volume.** Both MAME and a
  faithful port therefore emit half a second of loud noise from reset until the sound
  program silences the channels. That is correct, not a bug.
- a tone period register written as 0 behaves as 0x400.
- the two chips are at different clocks but both divide by 16, so on the Z80's timeline
  they tick every **32** (sn1) and every **16** (sn2) cycles. Scheduling all audio on the
  Z80 cycle count keeps one timeline for the whole board.

### Resampling

From 223 kHz down to 48 kHz. Averaging the chip ticks inside one output sample is a box
filter, which attenuates almost nothing near Nyquist: the harmonics of every square wave
fold straight back into the audible band. Averaging **two adjacent windows** makes the
filter triangular, with its first null at 24 kHz, for the cost of one addition per sample.

Average over **time**, not over ticks — the two chips' tick grids do not line up, so
counting events weights them unequally.

Where the tick grid starts inside a sample is not a hardware fact (the divider's power-up
phase is arbitrary), but it decides how well the output lines up with MAME's. Measured as
mean sample-by-sample correlation against MAME's own recording:

| tick phase (sn1/sn2) | correlation |
|---|---|
| 0/0 — leading edge | 0.9647 |
| 8/4 | 0.9939 |
| **16/8 — midpoint** | **0.9937** |
| 28/14 | 0.8799 |

A broad plateau from 8/4 to 18/9, all within 0.0002. The midpoint of the tick is on it and
has a reason behind it — MAME's resampler reads the middle of a tick, not its leading
edge — rather than being a constant tuned to three decimal places.

---

## What the verification is worth

Four windows of the attract mode, diffed instruction by instruction with
`tools/diffz80.py` (PC, all register pairs, I, R, IM, IFF1, HALT and the cycle counter):

| window | instructions |
|---|---:|
| boot, 0–1 s | 393 917 |
| first sound command, 15.4–17 s | 589 550 |
| music, 23–25.5 s | 942 773 |
| attract effects, 35–38 s | 1 136 889 |
| **total** | **3 063 129, no divergence** |

Covering 50 490 indexed (DD/FD) instructions, 90 `LDIR`, 625 `SCF`, 64 `EXX`, 27 581 `RST`
and 539 `BIT`. `HALT` is never executed by this program, so that path stays unverified.

The differ aligns by searching our own trace for MAME's first state — registers and cycle
counter included — so a window starting 23 seconds after reset only lines up at all if the
entire preceding history matches.

Against MAME's `-wavwrite` recording of the same forty seconds: per-second RMS within
**0.2 %** everywhere, silences in the same places, sample-by-sample correlation **0.9937**
on the busiest seconds at a constant 3-sample lag, and no drift between 15 s and 37 s.

Two bugs this found, neither of which listening would have caught:

- `BIT b,r` clearing the carry flag. `BIT` leaves C alone; rewriting F from scratch does
  not. Visible as `F=18` against MAME's `F=19`, eighteen instructions after the first
  interrupt.
- the clock-conversion difference above, which looked at first like an interrupt latency
  problem and was actually an arithmetic one.
