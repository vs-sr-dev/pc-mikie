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

**13 078 instructions, 616 functions.**

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

Nothing in the static analysis contradicts the hardware. But "harmless" was the wrong
conclusion to draw: a phantom label that is never *jumped to* can still be **fallen into**
if the code generator lets an instruction fall through to whatever label comes next in the
file. That bug was real, and it took a trace diff 6.9 million instructions long to surface.
See [03-transpiler.md](03-transpiler.md) — the generator now makes fall-through explicit
and reports every instruction with a phantom label inside it.

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

**62 of 64 resolve statically.** Four things were needed to get there, and the last two
were only found by a human playing the game.

### Bound each table by the next base
Tables are packed contiguously. Reading entries "while the value looks like a valid code
address" walks straight into the neighbouring table and steals its entries.

`$D00C` has **3** entries, not 13 — `$D012` is already the next table.

### Scan backwards as well as forwards
Forward propagation of "which immediate is in X/Y/U" depends on the order blocks are
visited. When a trace seed drops the walker into the middle of a block, the `LDU #imm`
before it was never seen. A backward scan over the linear predecessor chain — stopping at
the first block boundary or redefinition of the register — recovers those.

### Scan the flow graph, not the listing
The base is not always an immediate. It can be read out of a table of its own:

```
91C6: LDU  #$DC0C          ; the table of handlers
91CA: LDY  A,U             ; pick one
  ...
91F8: JSR  ,Y
```

Following that needs two things the linear scan does not have.

*An exact test for "does this instruction redefine the register".* Checking the mnemonic
stops on a `TFR B,A` that cannot touch Y. Decode the postbyte of `TFR`/`EXG` and the
bitmask of `PULS`/`PULU` instead.

*Predecessors, not the previous address.* The dispatch loop puts a `BRA` between the load
and the use, and an unconditional jump really is a boundary walking backwards. Use the
xrefs already collected. Then a second problem appears: the loop's own back edge arrives
through a `PULS B,X,Y`, which redefines Y. Treating that as failure resolves nothing, so a
path that redefines the register is dropped as *no information* rather than fatal. What is
still refused is two paths disagreeing, or a path that loads the register some other way —
either would be a guess, and a guessed base recompiles whichever bytes happen to be there.

### Collect every base, not the first one
This is the one that hurt. Dispatchers are shared:

```
80AD: LDY  #$DA2D          ; one caller's table
80B1: JMP  $8303
  ...
8303: PSHS B / LDA -$E,X / ASLA
8308: JSR  [A,Y]           ; serves both
```

with another caller arriving at `$8303` with `$DA3B`. Taking the first base found
recompiled ten handlers and silently dropped seven — and the site counted as *resolved*, so
nothing in the report suggested a problem. Collect all the bases that can reach the site
and take the union of their tables: `$8308` has **17** entries, not 10.

Proof that the missing seven were real code: `$80B4`, `$8107`, `$81C3` and `$8238` were
four of the largest blocks the analysis had been listing as *non-code runs*.

### The two that cannot resolve statically
| site | form | why |
|---|---|---|
| `$AD4C` | `JMP [$29B6]` | soft vector held in RAM — undecidable by construction |
| `$72B5` | `JMP ,Y` | Y supplied by the caller |

### Coverage is a correctness requirement, not an optimisation

It is tempting to write that missing entries are harmless because the generated code traps
on any address that was never recompiled. The trap is a crash. Twice, the game ran through
its whole attract loop, passed a four-million-instruction diff against MAME, and then died
in the first minute of actual play: once on the first press of the attack button
(collision handlers), once walking into the corridor after level one (movement handlers).

Thirty seconds of attract executes 4 561 instructions out of 13 078 recompiled. **Two
thirds of the port is inference**, and reachability is undecidable, so some of it will be
wrong. `analysis/extra_entries.txt` exists for that: one hex address per line with a note
saying how it was found, fed back in as an entry point. The trap prints the address and
says so.

The metric that mattered was never "how many sites did I resolve". It was "how many paths
reach this site, and did I look at all of them".

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
