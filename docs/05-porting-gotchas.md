# Porting gotchas

Things that are easy to get wrong when moving Mikie off the arcade board, roughly ordered by
how long they take to notice.

---

## Found the hard way (or nearly)

### `CWAI` is real code
`CWAI #$EF` at `$6FFF` executes. It unmasks the IRQ, pushes the full machine state, and
halts until the interrupt. The handler does **not** push again, and its `RTI` returns to the
instruction after the `CWAI`.

This is the game's vblank sync. A recompiler or interpreter that skips it hangs at boot,
and no amount of static analysis will tell you — only running against a reference will.

### `RTI` costs 15 cycles, not 6
Six cycles when only CC and PC are pulled; **fifteen** when the E flag is set and the full
state comes back. Every `RTI` in Mikie is the 15-cycle form, because they all return from
IRQ or `CWAI`. Using the "documented" 6 makes every frame 300+ cycles short.

### The frame period is not an integer
1'536'000 / 60.59 = **25350.718** cycles. Rounded to 25351 you gain a cycle every ~4 frames.
Track it as a rational (`153600000 / 6059`), not a rounded constant.

### `$4000-$5FFF` reads as `$00`
The empty conversion-kit socket. Return `$FF` and you have not matched the hardware, and
your trace diverges the first time the boot code probes `$5FF0`.

### DIP idle values
`DSW1 = $FF`, `DSW2 = $7B`, `DSW3 = $FE`. Getting DSW3 wrong by one bit changes behaviour in
service paths you will not exercise until much later. It is a one-line bug that costs a day
if you find it by playing rather than by diffing.

---

## Video

### Vertical screen
ROT270: 224×256. On PC that means integer scaling plus rotation. On a console with a
horizontal framebuffer the rotation is manual and costs memory bandwidth — plan for it.

### Two-stage indirect palette
256 physical colours from resistor-DAC PROMs, then two lookup PROMs, then a **global 3-bit
palette bank**. Collapse it into a flat 256-colour table and the environment changes and
flash effects break.

### Tiles can draw on top of sprites
Bit `0x10` of colour RAM puts a tile in a second pass drawn *after* the sprites. One
tilemap pass is not enough.

### Sprite flipX is inverted
`flipx = ~byte0 & 0x10`. Not what you would guess, and the resulting bug is a mirrored
character that still animates correctly, so it reads as an art problem rather than a code one.

### 36 sprites, not 32
Sprite RAM ends at `$288F`.

### The sprite "gfx bank" is not a ROM bank
It selects between two decodes of the same ROM data offset by one byte. Decode 512 sprites,
256 per alignment.

---

## Audio

### The two PSGs run at different clocks
sn1 at 1.789772 MHz, sn2 at 3.579545 MHz — a factor of two. The *same* register value
produces a different pitch on each chip. Easy to miss, very audible when wrong.

### The sound timer is cycle-derived
`$8005` returns `(Z80 total cycles / 512) & 0xFF`, a 6991 Hz counter. The Z80 driver polls it
for musical timing. Derive it from emulated Z80 cycles, not from host time.

---

## Input and options

### Four-way stick
Not eight-way. On a modern pad the four-way restriction has to be imposed or the controls
feel loose in a way the original never did.

### Difficulty is not cosmetic
The DSW2 difficulty setting (1-4) changes actual gameplay. If a port exposes it as a menu
option, it is exposing a real difficulty curve, which is a feature — just do not assume the
default is the only tuned one.

---

## Timing

### 60.59 Hz, not 60
Game speed and music both ride on it. On a 60.00 or 59.94 Hz target the game runs about 1 %
slow. Decide deliberately whether to accept that, resample audio, or run the logic on its own
clock.
