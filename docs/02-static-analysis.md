# Static analysis: telling code from data

A static recompiler is only as good as its answer to one question: **which bytes are
instructions?** Get it wrong and you either miss code (the program traps at runtime) or
"recompile" data (garbage functions, wrong control flow).

Mikie has 40 960 bytes of program ROM. This is how they were classified.

---

## Two sources, combined

### 1. Execution trace from MAME — ground truth, incomplete
`tools/trace_cov.lua` drives the MAME debugger to log every executed PC.

40 seconds of emulated time → **13 205 523 instructions**, a 242 MB log, and
**4 561 addresses that are certainly instruction starts**.

Certain, but far from complete: attract mode never enters most of the game. On its own this
covers about 11 % of the ROM.

> The `noloop` flag is mandatory. Without it MAME collapses repetitions into
> `(loops for N instructions)` and the log is useless as a reference.

### 2. Recursive descent — complete, needs care
`tools/analyze.py` walks from the reset and IRQ vectors, following branches, calls and
resolved jump tables, seeded with every PC the trace confirmed.

---

## Result

| class | bytes | share |
|---|---:|---:|
| code | 28 285 | 69.1 % |
| data (tables, text, level layouts) | 11 298 | 27.6 % |
| padding (`$00` / `$FF` runs) | 1 377 | 3.4 % |

**12 801 instructions, 616 functions.**

### Largest non-code blocks
`$F157-$FDA5` (3151 B) · `$E800-$EF67` (1896 B) · `$D42E-$DAFE` (1745 B) ·
`$E14C-$E61C` (1233 B) · `$D165-$D42C` (712 B) · `$CF59-$D155` (509 B)

---

## The padding filter is not cosmetic

A run of `$FF` bytes decodes as a perfectly legal sequence of extended `STU` instructions.
Recursive descent will happily "execute" ROM filler for hundreds of bytes and drag whatever
it lands on into the code set.

Masking runs of six or more identical `$00`/`$FF` bytes cut **overlapping decodes from 400
to 45** — from 3 % of all instructions to 0.35 %.

### How to know the analysis is sound
Overlapping decodes — one instruction starting inside another — are the signature of a
wrong answer. The useful measurement is not how many there are, but how many are
*confirmed on both sides*:

> Of the 45 remaining overlaps, **zero** have both readings confirmed by the trace.

Nothing in the static analysis contradicts the hardware. The residue is all speculative
walks into data, and the runtime trap catches any of it that ever executes.

---

## Verifying the instruction decoder

`tools/m6809.py` is checked against MAME's own `unidasm` by `tools/verify_decoder.py`.

Comparing every instruction unidasm emits is misleading, because unidasm sweeps linearly and
therefore decodes data as instructions too. The meaningful comparison is over **code that
actually ran**:

| | |
|---|---|
| instructions compared inside executed code | 4 560 |
| length mismatches | **0** |
| mnemonic mismatches | **0** |
| mismatches outside executed code | 1 035 (all in data blocks) |

That 1 035 is itself a useful signal: it is concentrated exactly where the coverage analysis
says the data lives.

---

## Jump tables

**64 indirect jump sites**: `JSR/JMP [A,X]`, `[A,Y]`, `[B,U]`, `[A,U]`, `,Y`, and one
`[$nnnn]`. The trace had only seen 24 of them — attract mode does not reach the rest.

The pattern is uniform and fortunate. The table base is almost always an immediate loaded a
few instructions earlier:

```
6240: LDA    <$02          ; game state
6242: ASLA                 ; *2
6243: LDX    #$D00C        ; <-- table base
6246: JSR    [A,X]
```

**59 of 64 resolve statically.** Two things were needed to get there.

### Bound each table by the next base
Tables are packed contiguously. Reading entries "while the value looks like a valid code
address" walks straight into the neighbouring table and steals its entries.

`$D00C` has **3** entries, not 13 — `$D012` is already the next table.

### Scan backwards as well as forwards
Forward propagation of "which immediate is in X/Y/U" depends on the order blocks are
visited. When a trace seed drops the walker into the middle of a block, the `LDU #imm`
before it was never seen. A backward scan over the linear predecessor chain — stopping at
the first block boundary or redefinition of the register — recovers those.

### The five that cannot resolve statically
| site | form | why |
|---|---|---|
| `$AD4C` | `JMP [$29B6]` | soft vector held in RAM |
| `$91F8` | `JSR ,Y` | computed jump |
| `$72B5` | `JMP ,Y` | computed jump |
| `$DE91`, `$DE99` | — | false positives inside data |

None of them are a problem: the generated code routes *all* indirect control flow through a
runtime dispatch table anyway, and traps on any address that was never recompiled. Static
resolution is an optimisation and a coverage aid, not a correctness requirement.

---

## Instruction inventory

What the recompiler actually has to implement, measured rather than assumed:

- **114 distinct mnemonics**, **218 distinct (mnemonic, addressing mode) pairs**
- all standard MC6809 — nothing exotic, no undocumented opcodes in executed code

Indexed addressing forms actually used:

| form | count | | form | count |
|---|---:|---|---|---:|
| `n5,R` | 1619 | | `A,R` | 64 (+56 indirect) |
| `n8,R` | 382 | | `B,R` | 37 (+5 indirect) |
| `,R` | 299 | | `,-R` | 14 |
| `,R+` | 101 | | `D,R` | 4 |
| `,R++` | 91 | | `n16,PCR` | 2 |
| `n16,R` | 68 | | `[$nnnn]` | 2 |
