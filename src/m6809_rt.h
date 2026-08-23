/* Runtime for the statically recompiled 6809 code - Mikie (Konami GX469)
 *
 * Model:
 *   - registers live in globals (A, B, X, Y, U, S, DP, CC)
 *   - a 64K emulated address space behind a mapped accessor (RAM / IO / ROM)
 *   - the stack is the REAL one, in emulated memory: the game manipulates it
 *     explicitly (LDS #$3000, PULS PC, RTI, CWAI), so the C stack cannot be used
 *   - direct control flow   -> goto (free)
 *   - indirect control flow -> a label table indexed by PC
 */
#ifndef M6809_RT_H
#define M6809_RT_H

#include <stdint.h>
#include <string.h>

/* ------------------------------------------------------------- CC flags */
#define CC_C 0x01
#define CC_V 0x02
#define CC_Z 0x04
#define CC_N 0x08
#define CC_I 0x10
#define CC_H 0x20
#define CC_F 0x40
#define CC_E 0x80

/* ------------------------------------------------------------ registers */
extern uint8_t  r_a, r_b, r_dp, r_cc;
extern uint16_t r_x, r_y, r_u, r_s;
extern uint64_t cpu_cycles;
extern int      irq_pending;

#define R_D        ((uint16_t)(((uint16_t)r_a << 8) | r_b))
#define SET_D(v)   do { uint16_t _d = (uint16_t)(v); r_a = (uint8_t)(_d >> 8); \
                        r_b = (uint8_t)_d; } while (0)

/* --------------------------------------------------------------- memory */
uint8_t  mem_read (uint16_t a);
void     mem_write(uint16_t a, uint8_t v);

static inline uint16_t mem_read16(uint16_t a) {
    return (uint16_t)((mem_read(a) << 8) | mem_read((uint16_t)(a + 1)));
}
static inline void mem_write16(uint16_t a, uint16_t v) {
    mem_write(a, (uint8_t)(v >> 8));
    mem_write((uint16_t)(a + 1), (uint8_t)v);
}

/* direct addressing: the page is selected by DP */
#define DIRA(off)  ((uint16_t)(((uint16_t)r_dp << 8) | (uint8_t)(off)))

/* ------------------------------------------------------------ stack (S) */
#define PUSHS8(v)   do { r_s--; mem_write(r_s, (uint8_t)(v)); } while (0)
#define PULLS8(dst) do { (dst) = mem_read(r_s); r_s++; } while (0)
#define PUSHS16(v)  do { uint16_t _v = (uint16_t)(v); r_s -= 2; mem_write16(r_s, _v); } while (0)
#define PULLS16(dst) do { (dst) = mem_read16(r_s); r_s += 2; } while (0)
#define PUSHU8(v)   do { r_u--; mem_write(r_u, (uint8_t)(v)); } while (0)
#define PULLU8(dst) do { (dst) = mem_read(r_u); r_u++; } while (0)
#define PUSHU16(v)  do { uint16_t _v = (uint16_t)(v); r_u -= 2; mem_write16(r_u, _v); } while (0)
#define PULLU16(dst) do { (dst) = mem_read16(r_u); r_u += 2; } while (0)

/* --------------------------------------------------------- flag helpers */
#define SETNZ8(v)  do { r_cc &= (uint8_t)~(CC_N | CC_Z);                       \
                        r_cc |= (uint8_t)(((v) & 0x80) ? CC_N : 0);            \
                        r_cc |= (uint8_t)(((uint8_t)(v) == 0) ? CC_Z : 0); } while (0)
#define SETNZ16(v) do { r_cc &= (uint8_t)~(CC_N | CC_Z);                       \
                        r_cc |= (uint8_t)(((v) & 0x8000) ? CC_N : 0);          \
                        r_cc |= (uint8_t)(((uint16_t)(v) == 0) ? CC_Z : 0); } while (0)
#define CLRV()     (r_cc &= (uint8_t)~CC_V)
#define SETC(x)    do { r_cc = (uint8_t)((r_cc & ~CC_C) | ((x) ? CC_C : 0)); } while (0)
#define SETV(x)    do { r_cc = (uint8_t)((r_cc & ~CC_V) | ((x) ? CC_V : 0)); } while (0)
#define SETH(x)    do { r_cc = (uint8_t)((r_cc & ~CC_H) | ((x) ? CC_H : 0)); } while (0)
#define GETC()     (r_cc & CC_C)

/* 8-bit add with optional carry in: updates H N Z V C */
static inline uint8_t alu_add8(uint8_t a, uint8_t m, uint8_t c) {
    uint16_t r = (uint16_t)a + m + c;
    SETH(((a ^ m ^ (uint8_t)r) & 0x10));
    SETC(r > 0xFF);
    SETV(((a ^ m ^ 0x80) & (a ^ (uint8_t)r)) & 0x80);
    SETNZ8((uint8_t)r);
    return (uint8_t)r;
}
/* 8-bit subtract with optional borrow: updates N Z V C (H undefined) */
static inline uint8_t alu_sub8(uint8_t a, uint8_t m, uint8_t c) {
    uint16_t r = (uint16_t)a - m - c;
    SETC(r > 0xFF);
    SETV(((a ^ m) & (a ^ (uint8_t)r)) & 0x80);
    SETNZ8((uint8_t)r);
    return (uint8_t)r;
}
static inline uint16_t alu_add16(uint16_t a, uint16_t m) {
    uint32_t r = (uint32_t)a + m;
    SETC(r > 0xFFFF);
    SETV(((a ^ m ^ 0x8000) & (a ^ (uint16_t)r)) & 0x8000);
    SETNZ16((uint16_t)r);
    return (uint16_t)r;
}
static inline uint16_t alu_sub16(uint16_t a, uint16_t m) {
    uint32_t r = (uint32_t)a - m;
    SETC(r > 0xFFFF);
    SETV(((a ^ m) & (a ^ (uint16_t)r)) & 0x8000);
    SETNZ16((uint16_t)r);
    return (uint16_t)r;
}

/* ---------------------------------------------------- branch conditions */
#define C_N   ((r_cc & CC_N) != 0)
#define C_Z   ((r_cc & CC_Z) != 0)
#define C_V   ((r_cc & CC_V) != 0)
#define C_C   ((r_cc & CC_C) != 0)
#define C_HI  (!C_C && !C_Z)
#define C_LS  ( C_C ||  C_Z)
#define C_GE  (C_N == C_V)
#define C_LT  (C_N != C_V)
#define C_GT  (!C_Z && (C_N == C_V))
#define C_LE  ( C_Z || (C_N != C_V))

/* --------------------------------------------------------- vblank timing
 *
 * Exact period: 1'536'000 Hz / 60.59 Hz = 153600000/6059 cycles. That is not an
 * integer, so it is kept as a rational rather than a rounded constant.
 *
 * Phase and latency were MEASURED against MAME's `totalcycles` counter, not
 * deduced:
 *   - interrupts taken during CWAI land exactly on the edge (28 samples, delay
 *     0 in all 28). With the CPU halted the edge is clean, which pins the phase.
 *   - during normal execution the CPU takes the interrupt at the first
 *     instruction boundary at or after edge + 15 cycles (89 samples out of 89,
 *     and 15 is the only threshold that explains them all).
 *   Those 15 cycles are the latency with which MAME's scheduler delivers the
 *   interrupt-line state change.
 */
#define VBL_NUM   153600000ULL
#define VBL_DEN        6059ULL
#define VBL_PHASE     (-90900LL)
#define VBL_LATENCY        15ULL

extern uint64_t vbl_frame;   /* index of the NEXT edge */
extern uint64_t vbl_next;    /* cycle of the next edge */
int  vbl_irq_enabled(void);  /* LS259 bit 7, the IRQ mask */
void vbl_frame_hook(void);   /* called once per frame, at the edge */

static inline uint64_t vbl_edge(uint64_t k) {
    return (uint64_t)(((int64_t)(k * VBL_NUM) + VBL_PHASE) / (int64_t)VBL_DEN);
}
/* consume one edge: raise the IRQ if the mask allows it */
static inline void vbl_advance(void) {
    if (vbl_irq_enabled()) irq_pending = 1;
    vbl_frame++;
    vbl_next = vbl_edge(vbl_frame);
    vbl_frame_hook();
}
/* called at every instruction boundary */
static inline void vblank_poll(void) {
    while (cpu_cycles >= vbl_next + VBL_LATENCY) vbl_advance();
}

/* ----------------------------------------------------- external interface */
void cpu_reset(void);
void cpu_run(void);                 /* generated by tools/transpile.py */
void cpu_unknown_pc(uint16_t pc);   /* trap: address that was never recompiled */
uint16_t cpu_entry_pc(void);
void cpu_wait_irq(void);            /* CWAI: advance time to the vblank edge */
void daa(void);
#ifdef MIKIE_TRACE
void trace_insn(uint16_t pc);
#endif

#endif /* M6809_RT_H */
