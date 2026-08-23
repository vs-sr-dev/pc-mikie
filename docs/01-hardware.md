# The hardware: Konami GX469 (Mikie, 1984)

Everything here was read off the MAME driver (`konami/mikie.cpp`) and confirmed against
the running machine. It is the reference a port has to reproduce.

---

## Chips and clocks

| Block | Detail |
|---|---|
| Main CPU | **MC6809E** @ 18.432 MHz / 12 = **1.536 MHz** |
| Sound CPU | **Z80** @ 14.318181 MHz / 4 = **3.579545 MHz** |
| Audio | **2 × SN76489A** — sn1 @ 1.789772 MHz, sn2 @ 3.579545 MHz (*different clocks*) |
| Video | 256×256, visible 256×224, **60.59 Hz**, orientation **ROT270** → vertical **224×256** |
| Latch | LS259 (6I) at `$2000-$2007` |
| Watchdog | `$2100` |

The exact frame period matters: 1'536'000 / 60.59 = **25350.718 CPU cycles per frame**.
It is not an integer, and rounding it to 25351 drifts by about one cycle every four frames.

### LS259 outputs
| bit | signal |
|---|---|
| 0 | COIN1 counter |
| 1 | COIN2 counter |
| 2 | SOUNDON — rising edge triggers the Z80 IRQ |
| 3 | END (unused) |
| 6 | FLIP screen |
| 7 | **INT** — main CPU IRQ mask |

---

## Main CPU memory map

```
0000-00FF  RAM (256 B)
2000-2007  LS259, one bit per address (write d0)
2100       watchdog reset
2200       palette bank (3 bits)
2400 R     SYSTEM      W  sound latch
2401 R     P1
2402 R     P2
2403 R     DSW3
2500 R     DSW1
2501 R     DSW2
2800-288F  sprite RAM  <- 144 bytes = 36 sprites x 4 bytes (yes, 288F, not 287F)
2890-37FF  work RAM (3952 B)   <- stack below $3000, direct page at $3000-$30FF
3800-3BFF  colour RAM (32x32 attributes)
3C00-3FFF  video RAM  (32x32 tile codes)
4000-5FFF  socket for the optional "conversion kit" ROM — absent in every dumped set
6000-FFFF  program ROM (40 KB)
```

Total game RAM is **under 4.5 KB**. The whole game state lives there.

### Two things that bite

**`$4000-$5FFF` must read as `$00`, not `$FF`.** Both the reset path and the IRQ handler
look for a `55 AA 55 AA …` signature at `$5FF0-$5FFF` and, if present, jump to `$4000` /
`$5000` instead of running the normal code. An unmapped region returning `$FF` will not
match the signature either, but returning `$00` is what MAME does and what the real board
does with an empty socket — match it or your traces will not line up.

**Sprite RAM ends at `$288F`.** 36 sprites, not 32. This is genuinely unusual and the MAME
driver comments on it.

---

## Sound CPU memory map

```
0000-3FFF  ROM (only 8 K populated)
4000-43FF  RAM (1 KB)
8000 W     (ignored)
8002 W     sn1 write        8003 R  sound latch
8004 W     sn2 write        8005 R  timer = (Z80 total cycles / 512) & 0xFF
```

`$8005` is a free-running counter at **3579545 / 512 = 6991 Hz**. The Z80 driver polls it
to pace musical ticks, so it has to be derived from real Z80 cycles — a wall-clock timer
will drift against the music.

The sound ROM uses the usual Konami RST helpers: `RST 08` writes sn1, `RST 10` writes sn2.

---

## Video model

### Tilemap
32×32 tiles of 8×8, 4 bpp, 512 tiles available.

```
code  = videoram[i] + ((colorram[i] & 0x20) << 3)      /* 9 bits */
colour= (colorram[i] & 0x0F) + 16 * palettebank
flipX =  colorram[i] & 0x40
flipY =  colorram[i] & 0x80
prio  =  colorram[i] & 0x10        /* draw this tile ON TOP of sprites */
```

Draw order: `tilemap(category 0)` → `sprites` → `tilemap(category 1)`.

### Sprites — 36 slots of 4 bytes
```
byte 0: bits 0-3 colour | bit 4 flipX (INVERTED) | bit 5 flipY | bit 6 -> code bit 8
byte 1: Y   (screen y = 244 - byte1)
byte 2: bits 0-5 code | bit 6 gfx bank select | bit 7 -> code bit 6
byte 3: X
```
```
code = (b2 & 0x3F) + ((b2 & 0x80) >> 1) + ((b0 & 0x40) << 1)
```

The "gfx bank" is **not** a ROM bank. It selects between two decodes of the *same* ROM
offset by one byte — a Konami trick for half-tile alignment. A port simply decodes
**512 sprites**: 256 per alignment.

### Palette — indirect, two-stage
1. Three 256×4 PROMs feed a resistor DAC (2.2k / 1k / 470 / 220 Ω) giving **256 physical
   colours**. Per-bit weights are linear in conductance: 14 / 31 / 67 / 143 (sum 255).
2. Two more PROMs map `(colour set, pixel)` onto one of those 256:
   - tiles:   `index = (palettebank << 5) | 0x10 | (charLUT[set*16 + pixel] & 0x0F)`
   - sprites: `index = (palettebank << 5) |        (sprLUT [set*16 + pixel] & 0x0F)`
3. `palettebank` is **global** (3 bits, written to `$2200`) and recolours the whole scene
   at once. That is how the game does its environment and flash effects.

Sprite pixel value 0 is transparent.

Flattening this to "256 direct colours" is the single most common way to get Mikie's
colours subtly wrong.

---

## Inputs

Four-way joystick (not eight-way) plus two buttons — door and headbutt — per player.
On a modern pad the four-way restriction has to be emulated, or diagonal input makes the
controls feel loose in a way the original never did.

### DIP switch idle values
| port | value | meaning |
|---|---|---|
| DSW1 `$2500` | `$FF` | 1 coin / 1 credit, both slots |
| DSW2 `$2501` | `$7B` | 3 lives, upright, bonus 20k/70k/50k+, easy, demo sounds on |
| DSW3 `$2403` | `$FE` | flip off, single upright controls |

DSW3 is not fitted on the real PCB and is not in the manual, but the code reads it.

The difficulty setting (1-4) genuinely changes gameplay — it is not cosmetic, so a port
that exposes it as a menu option is exposing a real difficulty curve.

---

## Program structure

Vectors: RESET → `$CD91`, IRQ → `$620A`, everything else → `$CD91`.

The game is entirely **vblank-IRQ driven**. The handler at `$620A`:
1. clears the IRQ mask, kicks the watchdog
2. increments a 16-bit frame counter in direct page
3. reads the game state and, unless it is 3, calls `$6253`, `$6280`, `$BC99`, `$62A5`
4. **dispatches through a table**: `A = state; ASLA; JSR [A,X]` with `X = $D00C`
5. re-enables the IRQ mask, `RTI`

Direct page is `$30`, so globals live at `$3000-$30FF` and the stack grows down from `$3000`.

### `CWAI` — the one that will catch you out
At `$6FFF` there is a `CWAI #$EF`, and it really executes. `CWAI` clears the named CC bits
(here the I flag, unmasking the IRQ), **pushes the entire machine state**, then halts until
an interrupt arrives. The handler finds the state already stacked and does *not* push again;
its `RTI` returns to the instruction after the `CWAI`.

This is how the game syncs to vblank. A recompiler that does not implement it hangs at boot.

### Text format
A tiny string VM: `'@'` is space, `$3C`/`$3D`/`$3E` are positioning commands followed by a
column byte, `$3F` ends the string, `$2F` is a line break. The eight rooms are stored as
plain text: `HALL 1 · WAY 1 · DANCE STUDIO · HALL 2 · WAY 2 · LOCKER ROOM · RESTAURANT · GARDEN`.

---

## ROM set

The parent `mikie` set, 14 files. `mikie`, `mikiej` (Shinnyuu Shain Tooru-kun) and `mikiek`
share **identical program ROMs** and differ only in tile and sprite data — one engine, three
graphic sets, localisation for free. `mikiehs` (High School Graffiti) has different program
ROMs.

| ROM | role | size |
|---|---|---|
| `n14.11c` `o13.12a` `o17.12d` | M6809 program at `$6000` / `$8000` / `$C000` | 8+16+16 K |
| `n10.6e` | Z80 sound program | 8 K |
| `o11.8i` | tiles 8×8×4 | 16 K |
| `001.f1` `003.f3` `005.h1` `007.h3` | sprites 16×16×4 | 4×16 K |
| `d19.1i` `d21.3i` `d20.2i` | colour PROMs R / G / B | 3×256 B |
| `d22.12h` | tile colour LUT | 256 B |
| `d18.f9` | sprite colour LUT | 256 B |
