# The transpiler: 6809 → C

`tools/transpile.py` turns the 13 078 instructions found by the static analysis into a
single C file. This is the design and the details that took measurement to get right.

---

## Code generation model

**One function, one label per instruction.**

```c
void cpu_run(void)
{
    ...
L_620A: /* 4f  CLRA inh */
    IRQCK(0x620A);
    cpu_cycles += 2;
    r_a = 0; ...
L_620B: /* b7 20 07  STA ext */
    ...
}
```

- **Direct control flow is free.** A branch, a `BRA`, an absolute `JMP` or `JSR` becomes a
  plain `goto L_xxxx`. No dispatch, no lookup.
- **Indirect control flow goes through a label table.** `RTS`, `RTI`, `JMP [A,U]`,
  `JSR ,Y`, `PULS PC` compute an address at runtime and dispatch:

  ```c
  #define DISPATCH(a) do {                          \
          uint16_t _a = (uint16_t)(a);              \
          const void *_t = LBL[_a];                 \
          if (!_t) { cpu_unknown_pc(_a); return; }  \
          goto *_t;                                 \
      } while (0)
  ```

  `LBL` is a 64K table of label addresses (GCC/Clang label-as-value), populated once on
  first entry. Any address that was never recompiled **traps loudly** instead of failing
  silently — which is what makes the incomplete-static-analysis problem survivable.

- **The stack is the real one, in emulated memory.** This is not optional. The game does
  `LDS #$3000`, `PULS PC`, `RTI` and `CWAI`; it reads and writes its own stack frames.
  The C call stack cannot model that.

Because indirect dispatch is a runtime table, **statically resolving jump tables is an
optimisation and a coverage aid, not a correctness requirement**. The five unresolvable
sites cost nothing.

---

## Numbers

| | |
|---|---|
| instructions translated | 13 078 |
| traps emitted | 13 (all in data regions never executed) |
| generated C | 83 684 lines, 2.2 MB |
| compile time | ~6.5 min with `gcc -O1` |
| binary | 4.6 MB |

Compile time is the one real ergonomic cost, and it is worth being specific about it:
13 078 computed-goto labels in a single function is genuinely hard on the optimiser.

**Do not use `-O2`.** GCC 15 was still working after **26 minutes of CPU time and 17 GB
resident** on this file, with no end in sight; `-O1` finishes in about six minutes. The
generated code is mostly straight-line loads, stores and flag updates, so the higher
optimisation level has little to win in the first place — nearly all of it goes into
whole-function analyses that scale badly with label count.

If `-O2` matters to you, split the generated code into several functions sharing the
dispatch table first. Nothing in the design prevents that: only the `goto` edges are
function-local, and those can fall back to `DISPATCH` across a split.

---

## Details that are easy to get wrong

Every item here was either caught by the trace comparison or would have been.

### `A`, `B` and `D` are *signed* index offsets
`LDA A,X` adds A as a **two's complement** offset. Treating it as unsigned works for small
values and silently breaks for large ones.

### PC-relative addressing folds at compile time
`n,PCR` resolves to a constant during translation; there is no runtime PC to read.

### `CLR` touches more than the operand
It clears N, V and C and *sets* Z. It is not just "store zero".

### `LEAX`/`LEAY` affect Z; `LEAS`/`LEAU` do not
A classic 6809 asymmetry.

### `MUL` sets C from bit 7 of the low result byte
Not from a carry out. Z comes from the full 16-bit product.

### `RTI` costs 6 or 15 cycles
Six when only CC and PC are pulled, fifteen when E is set and the whole state returns.
Emitted as a runtime condition:
```c
cpu_cycles += (r_cc & CC_E) ? 15 : 6;
```

### Long branches cost one extra cycle when taken
`LBcc` is 5 not taken, 6 taken. Short branches are always 3.

### `PSHS`/`PULS` postbytes are constants
So the push/pull sequence is emitted explicitly — no loop, no mask testing at runtime.

### The interrupt stacks the PC of the instruction *about to run*
Not the following one. The interrupt arrives **before** the instruction starts.

This one is worth dwelling on, because of how it fails. Emitting `IRQCK(pc + len)` instead
of `IRQCK(pc)` makes the handler's `RTI` return one instruction too far. In Mikie the
visible symptom was an `RTS` landing at `$61C7` instead of `$61C5` — **456 instructions
after the actual mistake** — followed later by a trap at `$FE00` with the stack driven up
into colour RAM. Debugging that by watching the game would have been a bad afternoon.

### Fall-through must be explicit, not positional
This one is subtle and it cost a long trace to find.

Instructions are emitted in address order, and a `SEQ` instruction was allowed to fall
through to whatever label came next in the file. That is correct only while the next label
is the next instruction — and it is not always, because the static analysis can leave a
**phantom label inside an instruction**: a speculative decode that overlaps a real one.

At `$9104` the real instruction is `A7 88 21` (`STA $21,X`, three bytes). A speculative walk
had also produced a label at `$9106` for `21 CC` (`BRN`). Falling through in file order sent
execution to `L_9106`, i.e. **into the middle of an opcode**.

64 instructions were affected, five of them in code the trace confirms is executed
(`$9104 $9107 $9285 $9594 $9597`). The symptom appeared **6.9 million instructions into a
trace diff**, as a PC off by one with every register and the cycle counter still matching.

The fix is to emit an explicit `goto` whenever `pc + length` is not the next emitted label.
`tools/transpile.py` now reports how many it emitted and how many of those are in confirmed
code, so the hazard is visible at generation time instead of being left for a diff to find.

### `CWAI` must be implemented, and its interrupt entry still costs 19 cycles
```c
r_cc &= 0xEF; r_cc |= CC_E;
PUSHS16(next); PUSHS16(r_u); PUSHS16(r_y); PUSHS16(r_x);
PUSHS8(r_dp); PUSHS8(r_b); PUSHS8(r_a); PUSHS8(r_cc);
cpu_wait_irq();          /* time jumps exactly to the vblank edge */
r_cc |= CC_I; irq_pending = 0;
cpu_cycles += 19;        /* entry costs 19 even though the state is already stacked */
goto L_IRQVEC;
```

---

## The runtime contract

The generated file is not self-contained. It expects a host runtime providing:

```c
/* registers */
uint8_t  r_a, r_b, r_dp, r_cc;
uint16_t r_x, r_y, r_u, r_s;
uint64_t cpu_cycles;
int      irq_pending;

/* memory, with the hardware map wired in */
uint8_t  mem_read (uint16_t a);
void     mem_write(uint16_t a, uint8_t v);

/* vblank scheduling, called at every instruction boundary */
void     vblank_poll(void);
int      vbl_irq_enabled(void);   /* LS259 bit 7 */

/* misc */
uint16_t cpu_entry_pc(void);
void     cpu_wait_irq(void);      /* CWAI: advance time to the vblank edge */
void     cpu_unknown_pc(uint16_t pc);   /* trap */
void     daa(void);
```

plus the flag helpers and ALU primitives (`alu_add8`, `alu_sub16`, `SETNZ8`, …). Keeping
these in a header lets the compiler inline them, which matters: they run on nearly every
instruction.

See [`04-verification.md`](04-verification.md) for how all of this is proven correct.
