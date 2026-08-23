/* Z80 interpreter - see z80.h for the two rules that matter. */
#include "z80.h"

#define SF   0x80u
#define ZF   0x40u
#define YF   0x20u
#define HF   0x10u
#define XF   0x08u
#define PF   0x04u
#define NF   0x02u
#define CF   0x01u
#define YXF  (YF | XF)

static uint8_t parity_tab[256];
static int     tables_ready;

static void make_tables(void)
{
    int i, j, p;
    for (i = 0; i < 256; i++) {
        for (p = 0, j = 0; j < 8; j++) p ^= (i >> j) & 1;
        parity_tab[i] = p ? 0 : PF;
    }
    tables_ready = 1;
}

/* --------------------------------------------------------- register pairs */
#define BC   ((uint16_t)(((uint16_t)z->b << 8) | z->c))
#define DE   ((uint16_t)(((uint16_t)z->d << 8) | z->e))
#define HL   ((uint16_t)(((uint16_t)z->h << 8) | z->l))
#define AF   ((uint16_t)(((uint16_t)z->a << 8) | z->f))
#define IX   ((uint16_t)(((uint16_t)z->ixh << 8) | z->ixl))
#define IY   ((uint16_t)(((uint16_t)z->iyh << 8) | z->iyl))
#define SETP(hi, lo, v) do { uint16_t v_ = (uint16_t)(v); \
                             (hi) = (uint8_t)(v_ >> 8); (lo) = (uint8_t)v_; } while (0)
#define SETBC(v) SETP(z->b, z->c, v)
#define SETDE(v) SETP(z->d, z->e, v)
#define SETHL(v) SETP(z->h, z->l, v)
#define SETAF(v) SETP(z->a, z->f, v)

/* ------------------------------------------------------------ bus helpers
 * Access first, cycles after: that is the order MAME charges them in, and the
 * $8005 timer read can tell the difference. */
static uint8_t rd(z80_t *z, uint16_t a)
{
    uint8_t v = z80_read(a);
    z->cycles += 3;
    return v;
}

static void wr(z80_t *z, uint16_t a, uint8_t v)
{
    z80_write(a, v);
    z->cycles += 3;
}

static uint8_t arg(z80_t *z)
{
    uint8_t v = z80_read(z->pc++);
    z->cycles += 3;
    return v;
}

static uint16_t arg16(z80_t *z)
{
    uint8_t lo = arg(z), hi = arg(z);
    return (uint16_t)(lo | (hi << 8));
}

/* opcode fetch: 4 T, refreshes R, and rolls the SCF/CCF quirk register over */
static uint8_t opfetch(z80_t *z)
{
    uint8_t v = z80_read(z->pc++);
    z->cycles += 4;
    z->r  = (uint8_t)((z->r & 0x80) | ((z->r + 1) & 0x7F));
    z->q  = z->qt;
    z->qt = YXF;
    return v;
}

static void push16(z80_t *z, uint16_t v)
{
    wr(z, --z->sp, (uint8_t)(v >> 8));
    wr(z, --z->sp, (uint8_t)v);
}

static uint16_t pop16(z80_t *z)
{
    uint8_t lo = rd(z, z->sp++), hi = rd(z, z->sp++);
    return (uint16_t)(lo | (hi << 8));
}

/* every instruction that writes F clears qt; see z80.h */
static void setf(z80_t *z, unsigned f) { z->f = (uint8_t)f; z->qt = 0; }

#define SZYX(r)  (((r) & (SF | YF | XF)) | (((r) & 0xFF) ? 0 : ZF))

/* ------------------------------------------------------------------- ALU */
static void alu_add(z80_t *z, uint8_t v)
{
    unsigned r = z->a + v;
    setf(z, SZYX(r) | ((r & 0x100) ? CF : 0)
            | ((z->a ^ v ^ r) & HF)
            | (((z->a ^ r) & (v ^ r) & 0x80) ? PF : 0));
    z->a = (uint8_t)r;
}

static void alu_adc(z80_t *z, uint8_t v)
{
    unsigned c = z->f & CF, r = z->a + v + c;
    setf(z, SZYX(r) | ((r & 0x100) ? CF : 0)
            | ((z->a ^ v ^ r) & HF)
            | (((z->a ^ r) & (v ^ r) & 0x80) ? PF : 0));
    z->a = (uint8_t)r;
}

static void alu_sub(z80_t *z, uint8_t v)
{
    unsigned r = z->a - v;
    setf(z, SZYX(r) | ((r & 0x100) ? CF : 0) | NF
            | ((z->a ^ v ^ r) & HF)
            | (((z->a ^ v) & (z->a ^ r) & 0x80) ? PF : 0));
    z->a = (uint8_t)r;
}

static void alu_sbc(z80_t *z, uint8_t v)
{
    unsigned c = z->f & CF, r = z->a - v - c;
    setf(z, SZYX(r) | ((r & 0x100) ? CF : 0) | NF
            | ((z->a ^ v ^ r) & HF)
            | (((z->a ^ v) & (z->a ^ r) & 0x80) ? PF : 0));
    z->a = (uint8_t)r;
}

static void alu_and(z80_t *z, uint8_t v)
{
    z->a &= v;
    setf(z, SZYX(z->a) | HF | parity_tab[z->a]);
}

static void alu_xor(z80_t *z, uint8_t v)
{
    z->a ^= v;
    setf(z, SZYX(z->a) | parity_tab[z->a]);
}

static void alu_or(z80_t *z, uint8_t v)
{
    z->a |= v;
    setf(z, SZYX(z->a) | parity_tab[z->a]);
}

/* CP takes YX from the operand, not from the result */
static void alu_cp(z80_t *z, uint8_t v)
{
    unsigned r = z->a - v;
    setf(z, (r & SF) | ((r & 0xFF) ? 0 : ZF) | (v & YXF) | NF
            | ((r & 0x100) ? CF : 0)
            | ((z->a ^ v ^ r) & HF)
            | (((z->a ^ v) & (z->a ^ r) & 0x80) ? PF : 0));
}

static uint8_t alu_inc(z80_t *z, uint8_t v)
{
    uint8_t r = (uint8_t)(v + 1);
    setf(z, (z->f & CF) | SZYX(r) | ((r & 0x0F) ? 0 : HF) | ((r == 0x80) ? PF : 0));
    return r;
}

static uint8_t alu_dec(z80_t *z, uint8_t v)
{
    uint8_t r = (uint8_t)(v - 1);
    setf(z, (z->f & CF) | SZYX(r) | NF | (((r & 0x0F) == 0x0F) ? HF : 0)
            | ((r == 0x7F) ? PF : 0));
    return r;
}

static uint16_t add16(z80_t *z, uint16_t a, uint16_t b)
{
    unsigned r = a + b;
    z->wz = (uint16_t)(a + 1);
    z->cycles += 7;
    setf(z, (z->f & (SF | ZF | PF)) | ((r >> 8) & YXF)
            | (((a ^ r ^ b) >> 8) & HF) | ((r & 0x10000) ? CF : 0));
    return (uint16_t)r;
}

static void adc16(z80_t *z, uint16_t v)
{
    uint16_t a = HL;
    unsigned c = z->f & CF, r = a + v + c;
    z->wz = (uint16_t)(a + 1);
    z->cycles += 7;
    setf(z, ((r >> 8) & (SF | YF | XF)) | ((r & 0xFFFF) ? 0 : ZF)
            | (((a ^ r ^ v) >> 8) & HF) | ((r & 0x10000) ? CF : 0)
            | (((a ^ r) & (v ^ r) & 0x8000) ? PF : 0));
    SETHL(r);
}

static void sbc16(z80_t *z, uint16_t v)
{
    uint16_t a = HL;
    unsigned c = z->f & CF, r = a - v - c;
    z->wz = (uint16_t)(a + 1);
    z->cycles += 7;
    setf(z, ((r >> 8) & (SF | YF | XF)) | ((r & 0xFFFF) ? 0 : ZF) | NF
            | (((a ^ r ^ v) >> 8) & HF) | ((r & 0x10000) ? CF : 0)
            | (((a ^ v) & (a ^ r) & 0x8000) ? PF : 0));
    SETHL(r);
}

static void op_daa(z80_t *z)
{
    uint8_t a = z->a, r = a;
    if (z->f & NF) {
        if ((z->f & HF) || (a & 0x0F) > 9) r -= 6;
        if ((z->f & CF) || a > 0x99)       r -= 0x60;
    } else {
        if ((z->f & HF) || (a & 0x0F) > 9) r += 6;
        if ((z->f & CF) || a > 0x99)       r += 0x60;
    }
    setf(z, (z->f & NF) | SZYX(r) | parity_tab[r] | ((a ^ r) & HF)
            | (((z->f & CF) || a > 0x99) ? CF : 0));
    z->a = r;
}

/* --------------------------------------------------------------- rotates */
static uint8_t op_rlc(z80_t *z, uint8_t v)
{
    uint8_t r = (uint8_t)((v << 1) | (v >> 7));
    setf(z, SZYX(r) | parity_tab[r] | ((v & 0x80) ? CF : 0));
    return r;
}

static uint8_t op_rrc(z80_t *z, uint8_t v)
{
    uint8_t r = (uint8_t)((v >> 1) | (v << 7));
    setf(z, SZYX(r) | parity_tab[r] | (v & CF));
    return r;
}

static uint8_t op_rl(z80_t *z, uint8_t v)
{
    uint8_t r = (uint8_t)((v << 1) | (z->f & CF));
    setf(z, SZYX(r) | parity_tab[r] | ((v & 0x80) ? CF : 0));
    return r;
}

static uint8_t op_rr(z80_t *z, uint8_t v)
{
    uint8_t r = (uint8_t)((v >> 1) | ((z->f & CF) << 7));
    setf(z, SZYX(r) | parity_tab[r] | (v & CF));
    return r;
}

static uint8_t op_sla(z80_t *z, uint8_t v)
{
    uint8_t r = (uint8_t)(v << 1);
    setf(z, SZYX(r) | parity_tab[r] | ((v & 0x80) ? CF : 0));
    return r;
}

static uint8_t op_sra(z80_t *z, uint8_t v)
{
    uint8_t r = (uint8_t)((v >> 1) | (v & 0x80));
    setf(z, SZYX(r) | parity_tab[r] | (v & CF));
    return r;
}

/* SLL: undocumented, shifts a 1 in */
static uint8_t op_sll(z80_t *z, uint8_t v)
{
    uint8_t r = (uint8_t)((v << 1) | 1);
    setf(z, SZYX(r) | parity_tab[r] | ((v & 0x80) ? CF : 0));
    return r;
}

static uint8_t op_srl(z80_t *z, uint8_t v)
{
    uint8_t r = (uint8_t)(v >> 1);
    setf(z, SZYX(r) | parity_tab[r] | (v & CF));
    return r;
}

/* BIT takes YX from the value for register operands and from the high byte of
   the address for memory ones - which is the reason WZ is tracked at all. */
static void op_bit(z80_t *z, int b, uint8_t v, uint8_t yx)
{
    uint8_t m = (uint8_t)(v & (1 << b));
    setf(z, (z->f & CF) | (m & SF) | (m ? 0 : (ZF | PF)) | HF | (yx & YXF));
}

/* ------------------------------------------------------------- addressing
 * ix: 0 = HL, 1 = IX, 2 = IY. */
static uint16_t idx16(z80_t *z, int ix)
{
    return ix == 0 ? HL : ix == 1 ? IX : IY;
}

static void set_idx16(z80_t *z, int ix, uint16_t v)
{
    if      (ix == 0) SETHL(v);
    else if (ix == 1) SETP(z->ixh, z->ixl, v);
    else              SETP(z->iyh, z->iyl, v);
}

/* the 8-bit register file; index 6 means "memory", handled by the caller.
   H and L become IXH/IXL only when the instruction has no memory operand. */
static uint8_t *reg8(z80_t *z, int n, int ix)
{
    switch (n) {
    case 0: return &z->b;
    case 1: return &z->c;
    case 2: return &z->d;
    case 3: return &z->e;
    case 4: return ix == 0 ? &z->h : ix == 1 ? &z->ixh : &z->iyh;
    case 5: return ix == 0 ? &z->l : ix == 1 ? &z->ixl : &z->iyl;
    case 7: return &z->a;
    }
    return &z->a;                    /* unreachable: 6 is memory */
}

/* address of the (HL) / (IX+d) operand, charging the displacement fetch and
   the internal cycles that follow it */
static uint16_t idx_addr(z80_t *z, int ix, int idle)
{
    uint16_t a;
    if (ix == 0) return HL;
    a = (uint16_t)(idx16(z, ix) + (int8_t)arg(z));
    z->wz = a;
    z->cycles += idle;
    return a;
}

static void alu_op(z80_t *z, int op, uint8_t v)
{
    switch (op) {
    case 0: alu_add(z, v); break;
    case 1: alu_adc(z, v); break;
    case 2: alu_sub(z, v); break;
    case 3: alu_sbc(z, v); break;
    case 4: alu_and(z, v); break;
    case 5: alu_xor(z, v); break;
    case 6: alu_or (z, v); break;
    default: alu_cp(z, v); break;
    }
}

static uint8_t shift_op(z80_t *z, int op, uint8_t v)
{
    switch (op) {
    case 0: return op_rlc(z, v);
    case 1: return op_rrc(z, v);
    case 2: return op_rl (z, v);
    case 3: return op_rr (z, v);
    case 4: return op_sla(z, v);
    case 5: return op_sra(z, v);
    case 6: return op_sll(z, v);
    default: return op_srl(z, v);
    }
}

static int cond_met(z80_t *z, int cc)
{
    switch (cc) {
    case 0: return !(z->f & ZF);
    case 1: return  (z->f & ZF) != 0;
    case 2: return !(z->f & CF);
    case 3: return  (z->f & CF) != 0;
    case 4: return !(z->f & PF);
    case 5: return  (z->f & PF) != 0;
    case 6: return !(z->f & SF);
    default: return (z->f & SF) != 0;
    }
}

/* ---------------------------------------------------------- CB / DDCB set */
static void exec_cb(z80_t *z)
{
    uint8_t op = opfetch(z);
    int x = op >> 6, y = (op >> 3) & 7, r = op & 7;
    uint8_t v;

    if (r == 6) {
        uint16_t a = HL;
        v = rd(z, a);
        z->cycles += 1;
        switch (x) {
        case 0: wr(z, a, shift_op(z, y, v)); break;
        case 1: op_bit(z, y, v, (uint8_t)(z->wz >> 8)); break;
        case 2: wr(z, a, (uint8_t)(v & ~(1 << y))); break;
        default: wr(z, a, (uint8_t)(v | (1 << y))); break;
        }
    } else {
        uint8_t *p = reg8(z, r, 0);
        v = *p;
        switch (x) {
        case 0: *p = shift_op(z, y, v); break;
        case 1: op_bit(z, y, v, v); break;
        case 2: *p = (uint8_t)(v & ~(1 << y)); break;
        default: *p = (uint8_t)(v | (1 << y)); break;
        }
    }
}

/* DD CB d op: the opcode byte comes after the displacement and is read as an
   argument, so it does NOT refresh R. Undocumented forms also copy the result
   into the register named by the low three bits. */
static void exec_xycb(z80_t *z, int ix)
{
    uint16_t a = (uint16_t)(idx16(z, ix) + (int8_t)arg(z));
    uint8_t  op = arg(z);
    int x = op >> 6, y = (op >> 3) & 7, r = op & 7;
    uint8_t v, res;

    z->wz = a;
    z->cycles += 2;
    v = rd(z, a);
    z->cycles += 1;
    if (x == 1) { op_bit(z, y, v, (uint8_t)(a >> 8)); return; }
    switch (x) {
    case 0:  res = shift_op(z, y, v); break;
    case 2:  res = (uint8_t)(v & ~(1 << y)); break;
    default: res = (uint8_t)(v | (1 << y)); break;
    }
    wr(z, a, res);
    if (r != 6) *reg8(z, r, 0) = res;
}

/* ----------------------------------------------------------------- ED set */
static void exec_ed(z80_t *z)
{
    uint8_t op = opfetch(z);
    int x = op >> 6, y = (op >> 3) & 7, w = op & 7, p = y >> 1, q = y & 1;

    if (x == 1) {
        switch (w) {
        case 0: {                                   /* IN r,(C) */
            uint8_t v = z80_in(BC);
            z->cycles += 4;
            z->wz = (uint16_t)(BC + 1);
            if (y != 6) *reg8(z, y, 0) = v;
            setf(z, (z->f & CF) | SZYX(v) | parity_tab[v]);
            break;
        }
        case 1:                                     /* OUT (C),r */
            z80_out(BC, y == 6 ? 0 : *reg8(z, y, 0));
            z->cycles += 4;
            z->wz = (uint16_t)(BC + 1);
            break;
        case 2: {                                   /* SBC/ADC HL,rr */
            uint16_t v = p == 0 ? BC : p == 1 ? DE : p == 2 ? HL : z->sp;
            if (q) adc16(z, v); else sbc16(z, v);
            break;
        }
        case 3: {                                   /* LD (nn),rr / LD rr,(nn) */
            uint16_t a = arg16(z);
            z->wz = (uint16_t)(a + 1);
            if (q == 0) {
                uint16_t v = p == 0 ? BC : p == 1 ? DE : p == 2 ? HL : z->sp;
                wr(z, a, (uint8_t)v);
                wr(z, (uint16_t)(a + 1), (uint8_t)(v >> 8));
            } else {
                uint8_t lo = rd(z, a), hi = rd(z, (uint16_t)(a + 1));
                uint16_t v = (uint16_t)(lo | (hi << 8));
                if      (p == 0) SETBC(v);
                else if (p == 1) SETDE(v);
                else if (p == 2) SETHL(v);
                else             z->sp = v;
            }
            break;
        }
        case 4: {                                   /* NEG */
            uint8_t v = z->a;
            z->a = 0;
            alu_sub(z, v);
            break;
        }
        case 5:                                     /* RETN / RETI */
            z->iff1 = z->iff2;
            z->pc = pop16(z);
            z->wz = z->pc;
            break;
        case 6:                                     /* IM n */
            z->im = (uint8_t)(y == 0 || y == 4 ? 0 : y == 2 || y == 6 ? 1 : y == 3 || y == 7 ? 2 : 0);
            break;
        default:
            switch (y) {
            case 0: z->cycles += 1; z->i = z->a; break;        /* LD I,A */
            case 1: z->cycles += 1; z->r = z->a; break;        /* LD R,A */
            case 2:                                            /* LD A,I */
                z->cycles += 1;
                z->a = z->i;
                setf(z, (z->f & CF) | SZYX(z->a) | (z->iff2 ? PF : 0));
                break;
            case 3:                                            /* LD A,R */
                z->cycles += 1;
                z->a = z->r;
                setf(z, (z->f & CF) | SZYX(z->a) | (z->iff2 ? PF : 0));
                break;
            case 4: {                                          /* RRD */
                uint8_t v = rd(z, HL);
                z->wz = (uint16_t)(HL + 1);
                z->cycles += 4;
                wr(z, HL, (uint8_t)((v >> 4) | (z->a << 4)));
                z->a = (uint8_t)((z->a & 0xF0) | (v & 0x0F));
                setf(z, (z->f & CF) | SZYX(z->a) | parity_tab[z->a]);
                break;
            }
            case 5: {                                          /* RLD */
                uint8_t v = rd(z, HL);
                z->wz = (uint16_t)(HL + 1);
                z->cycles += 4;
                wr(z, HL, (uint8_t)((v << 4) | (z->a & 0x0F)));
                z->a = (uint8_t)((z->a & 0xF0) | (v >> 4));
                setf(z, (z->f & CF) | SZYX(z->a) | parity_tab[z->a]);
                break;
            }
            default: break;                                    /* NOP */
            }
            break;
        }
        return;
    }

    if (x == 2 && y >= 4 && w <= 3) {               /* block instructions */
        int inc = (y & 1) ? -1 : 1;                 /* y: 4/5 = I/D, 6/7 = repeat */
        int rep = (y & 2) != 0;
        switch (w) {
        case 0: {                                   /* LDI / LDD / LDIR / LDDR */
            uint8_t v = rd(z, HL);
            uint8_t n;
            wr(z, DE, v);
            z->cycles += 2;
            SETHL(HL + inc);
            SETDE(DE + inc);
            SETBC(BC - 1);
            n = (uint8_t)(z->a + v);
            setf(z, (z->f & (SF | ZF | CF)) | ((n & 0x02) ? YF : 0) | (n & XF)
                    | (BC ? PF : 0));
            if (rep && BC != 0) {
                z->cycles += 5;
                z->pc -= 2;
                z->wz = (uint16_t)(z->pc + 1);
                setf(z, (z->f & ~YXF) | ((z->pc >> 8) & YXF));
            }
            break;
        }
        case 1: {                                   /* CPI / CPD / CPIR / CPDR */
            uint8_t v = rd(z, HL);
            uint8_t res;
            unsigned t;
            z->cycles += 5;
            z->wz = (uint16_t)(z->wz + inc);
            SETHL(HL + inc);
            SETBC(BC - 1);
            t = (unsigned)(z->a - v);
            res = (uint8_t)t;
            {
                unsigned h = (z->a ^ v ^ t) & HF;
                uint8_t yx = h ? (uint8_t)(res - 1) : res;
                setf(z, (z->f & CF) | (t & SF) | (res ? 0 : ZF) | NF | h
                        | ((yx & 0x02) ? YF : 0) | (yx & XF) | (BC ? PF : 0));
            }
            if (rep && BC != 0 && res != 0) {
                z->cycles += 5;
                z->pc -= 2;
                z->wz = (uint16_t)(z->pc + 1);
                setf(z, (z->f & ~YXF) | ((z->pc >> 8) & YXF));
            }
            break;
        }
        case 2: {                                   /* INI / IND / INIR / INDR */
            uint8_t v;
            z->cycles += 1;
            v = z80_in(BC);
            z->cycles += 4;
            z->wz = (uint16_t)(BC + inc);
            wr(z, HL, v);
            z->b--;
            SETHL(HL + inc);
            setf(z, SZYX(z->b) | NF);
            if (rep && z->b != 0) { z->cycles += 5; z->pc -= 2; }
            break;
        }
        default: {                                  /* OUTI / OUTD / OTIR / OTDR */
            uint8_t v;
            z->cycles += 1;
            v = rd(z, HL);
            z->b--;
            z80_out(BC, v);
            z->cycles += 4;
            z->wz = (uint16_t)(BC + inc);
            SETHL(HL + inc);
            setf(z, SZYX(z->b) | NF);
            if (rep && z->b != 0) { z->cycles += 5; z->pc -= 2; }
            break;
        }
        }
        return;
    }
    /* everything else on the ED page is a two-byte NOP */
}

/* --------------------------------------------------------------- main set */
static void exec_main(z80_t *z, int ix)
{
    uint8_t op = opfetch(z);
    int x = op >> 6, y = (op >> 3) & 7, w = op & 7, p = y >> 1, q = y & 1;

    switch (x) {
    case 0:
        switch (w) {
        case 0:
            if (y == 0) break;                                  /* NOP */
            if (y == 1) {                                       /* EX AF,AF' */
                uint8_t t;
                t = z->a; z->a = z->a2; z->a2 = t;
                t = z->f; z->f = z->f2; z->f2 = t;
                break;
            }
            if (y == 2) {                                       /* DJNZ d */
                int8_t d;
                z->cycles += 1;
                d = (int8_t)arg(z);
                if (--z->b != 0) {
                    z->cycles += 5;
                    z->pc = (uint16_t)(z->pc + d);
                    z->wz = z->pc;
                }
                break;
            }
            {                                                   /* JR / JR cc */
                int8_t d = (int8_t)arg(z);
                if (y == 3 || cond_met(z, y - 4)) {
                    z->cycles += 5;
                    z->pc = (uint16_t)(z->pc + d);
                    z->wz = z->pc;
                }
            }
            break;

        case 1:
            if (q == 0) {                                       /* LD rr,nn */
                uint16_t v = arg16(z);
                if      (p == 0) SETBC(v);
                else if (p == 1) SETDE(v);
                else if (p == 2) set_idx16(z, ix, v);
                else             z->sp = v;
            } else {                                            /* ADD HL,rr */
                uint16_t v = p == 0 ? BC : p == 1 ? DE : p == 2 ? idx16(z, ix) : z->sp;
                set_idx16(z, ix, add16(z, idx16(z, ix), v));
            }
            break;

        case 2:
            if (q == 0) {
                switch (p) {
                case 0: wr(z, BC, z->a); z->wz = (uint16_t)((z->a << 8) | ((BC + 1) & 0xFF)); break;
                case 1: wr(z, DE, z->a); z->wz = (uint16_t)((z->a << 8) | ((DE + 1) & 0xFF)); break;
                case 2: {                                       /* LD (nn),HL */
                    uint16_t a = arg16(z), v = idx16(z, ix);
                    z->wz = (uint16_t)(a + 1);
                    wr(z, a, (uint8_t)v);
                    wr(z, (uint16_t)(a + 1), (uint8_t)(v >> 8));
                    break;
                }
                default: {                                      /* LD (nn),A */
                    uint16_t a = arg16(z);
                    wr(z, a, z->a);
                    z->wz = (uint16_t)((z->a << 8) | ((a + 1) & 0xFF));
                    break;
                }
                }
            } else {
                switch (p) {
                case 0: z->a = rd(z, BC); z->wz = (uint16_t)(BC + 1); break;
                case 1: z->a = rd(z, DE); z->wz = (uint16_t)(DE + 1); break;
                case 2: {                                       /* LD HL,(nn) */
                    uint16_t a = arg16(z);
                    uint8_t lo, hi;
                    z->wz = (uint16_t)(a + 1);
                    lo = rd(z, a);
                    hi = rd(z, (uint16_t)(a + 1));
                    set_idx16(z, ix, (uint16_t)(lo | (hi << 8)));
                    break;
                }
                default: {                                      /* LD A,(nn) */
                    uint16_t a = arg16(z);
                    z->a = rd(z, a);
                    z->wz = (uint16_t)(a + 1);
                    break;
                }
                }
            }
            break;

        case 3: {                                               /* INC/DEC rr */
            uint16_t v = p == 0 ? BC : p == 1 ? DE : p == 2 ? idx16(z, ix) : z->sp;
            z->cycles += 2;
            v = (uint16_t)(q ? v - 1 : v + 1);
            if      (p == 0) SETBC(v);
            else if (p == 1) SETDE(v);
            else if (p == 2) set_idx16(z, ix, v);
            else             z->sp = v;
            break;
        }

        case 4: case 5: {                                       /* INC/DEC r */
            if (y == 6) {
                uint16_t a = idx_addr(z, ix, 5);
                uint8_t v = rd(z, a);
                z->cycles += 1;
                wr(z, a, w == 4 ? alu_inc(z, v) : alu_dec(z, v));
            } else {
                uint8_t *r = reg8(z, y, ix);
                *r = (w == 4) ? alu_inc(z, *r) : alu_dec(z, *r);
            }
            break;
        }

        case 6:                                                 /* LD r,n */
            if (y == 6) {
                if (ix == 0) {
                    wr(z, HL, arg(z));
                } else {                                        /* LD (IX+d),n */
                    uint16_t a = (uint16_t)(idx16(z, ix) + (int8_t)arg(z));
                    uint8_t v = arg(z);
                    z->wz = a;
                    z->cycles += 2;
                    wr(z, a, v);
                }
            } else {
                *reg8(z, y, ix) = arg(z);
            }
            break;

        default:
            switch (y) {
            case 0:                                             /* RLCA */
                z->a = (uint8_t)((z->a << 1) | (z->a >> 7));
                setf(z, (z->f & (SF | ZF | PF)) | (z->a & YXF) | (z->a & CF));
                break;
            case 1: {                                           /* RRCA */
                uint8_t c = (uint8_t)(z->a & CF);
                z->a = (uint8_t)((z->a >> 1) | (z->a << 7));
                setf(z, (z->f & (SF | ZF | PF)) | (z->a & YXF) | c);
                break;
            }
            case 2: {                                           /* RLA */
                uint8_t r = (uint8_t)((z->a << 1) | (z->f & CF));
                setf(z, (z->f & (SF | ZF | PF)) | (r & YXF) | ((z->a & 0x80) ? CF : 0));
                z->a = r;
                break;
            }
            case 3: {                                           /* RRA */
                uint8_t r = (uint8_t)((z->a >> 1) | ((z->f & CF) << 7));
                setf(z, (z->f & (SF | ZF | PF)) | (r & YXF) | (z->a & CF));
                z->a = r;
                break;
            }
            case 4: op_daa(z); break;
            case 5:                                             /* CPL */
                z->a = (uint8_t)~z->a;
                setf(z, (z->f & (SF | ZF | PF | CF)) | (z->a & YXF) | HF | NF);
                break;
            case 6:                                             /* SCF */
                setf(z, (z->f & (SF | ZF | PF)) | CF
                        | (((z->f & z->q) | z->a) & YXF));
                break;
            default:                                            /* CCF */
                setf(z, (z->f & (SF | ZF | PF)) | ((z->f & CF) ? HF : 0)
                        | ((z->f & CF) ? 0 : CF)
                        | (((z->f & z->q) | z->a) & YXF));
                break;
            }
            break;
        }
        break;

    case 1:                                                     /* LD r,r' */
        if (y == 6 && w == 6) {                                 /* HALT */
            z->halt = 1;
            break;
        }
        if (w == 6) {
            *reg8(z, y, 0) = rd(z, idx_addr(z, ix, 5));
        } else if (y == 6) {
            wr(z, idx_addr(z, ix, 5), *reg8(z, w, 0));
        } else {
            *reg8(z, y, ix) = *reg8(z, w, ix);
        }
        break;

    case 2:                                                     /* ALU A,r */
        alu_op(z, y, w == 6 ? rd(z, idx_addr(z, ix, 5)) : *reg8(z, w, ix));
        break;

    default:
        switch (w) {
        case 0:                                                 /* RET cc */
            z->cycles += 1;
            if (cond_met(z, y)) { z->pc = pop16(z); z->wz = z->pc; }
            break;

        case 1:
            if (q == 0) {                                       /* POP rr */
                uint16_t v = pop16(z);
                if      (p == 0) SETBC(v);
                else if (p == 1) SETDE(v);
                else if (p == 2) set_idx16(z, ix, v);
                else             SETAF(v);
            } else switch (p) {
            case 0: z->pc = pop16(z); z->wz = z->pc; break;      /* RET */
            case 1: {                                           /* EXX */
                uint8_t t;
                t = z->b; z->b = z->b2; z->b2 = t;
                t = z->c; z->c = z->c2; z->c2 = t;
                t = z->d; z->d = z->d2; z->d2 = t;
                t = z->e; z->e = z->e2; z->e2 = t;
                t = z->h; z->h = z->h2; z->h2 = t;
                t = z->l; z->l = z->l2; z->l2 = t;
                break;
            }
            case 2: z->pc = idx16(z, ix); break;                 /* JP (HL) */
            default: z->cycles += 2; z->sp = idx16(z, ix); break;/* LD SP,HL */
            }
            break;

        case 2: {                                               /* JP cc,nn */
            uint16_t a = arg16(z);
            z->wz = a;
            if (cond_met(z, y)) z->pc = a;
            break;
        }

        case 3:
            switch (y) {
            case 0: z->pc = arg16(z); z->wz = z->pc; break;      /* JP nn */
            case 1: if (ix) exec_xycb(z, ix); else exec_cb(z); break;
            case 2: {                                           /* OUT (n),A */
                uint8_t n = arg(z);
                z80_out((uint16_t)((z->a << 8) | n), z->a);
                z->cycles += 4;
                z->wz = (uint16_t)((z->a << 8) | ((n + 1) & 0xFF));
                break;
            }
            case 3: {                                           /* IN A,(n) */
                uint8_t n = arg(z);
                uint16_t port = (uint16_t)((z->a << 8) | n);
                z->a = z80_in(port);
                z->cycles += 4;
                z->wz = (uint16_t)(port + 1);
                break;
            }
            case 4: {                                           /* EX (SP),HL */
                uint8_t lo = rd(z, z->sp), hi = rd(z, (uint16_t)(z->sp + 1));
                uint16_t v = idx16(z, ix);
                z->cycles += 1;
                wr(z, (uint16_t)(z->sp + 1), (uint8_t)(v >> 8));
                wr(z, z->sp, (uint8_t)v);
                z->cycles += 2;
                z->wz = (uint16_t)(lo | (hi << 8));
                set_idx16(z, ix, z->wz);
                break;
            }
            case 5: {                                           /* EX DE,HL */
                uint8_t t;
                t = z->d; z->d = z->h; z->h = t;
                t = z->e; z->e = z->l; z->l = t;
                break;
            }
            case 6: z->iff1 = z->iff2 = 0; break;                /* DI */
            default: z->iff1 = z->iff2 = 1; z->after_ei = 1; break; /* EI */
            }
            break;

        case 4: {                                               /* CALL cc,nn */
            uint16_t a = arg16(z);
            z->wz = a;
            if (cond_met(z, y)) {
                z->cycles += 1;
                push16(z, z->pc);
                z->pc = a;
            }
            break;
        }

        case 5:
            if (q == 0) {                                       /* PUSH rr */
                uint16_t v = p == 0 ? BC : p == 1 ? DE : p == 2 ? idx16(z, ix) : AF;
                z->cycles += 1;
                push16(z, v);
            } else switch (p) {
            case 0: {                                           /* CALL nn */
                uint16_t a = arg16(z);
                z->cycles += 1;
                push16(z, z->pc);
                z->pc = a;
                z->wz = a;
                break;
            }
            case 1: exec_main(z, 1); break;                     /* DD */
            case 2: exec_ed(z); break;                          /* ED */
            default: exec_main(z, 2); break;                    /* FD */
            }
            break;

        case 6: alu_op(z, y, arg(z)); break;                    /* ALU A,n */

        default:                                                /* RST */
            z->cycles += 1;
            push16(z, z->pc);
            z->pc = (uint16_t)(y * 8);
            z->wz = z->pc;
            break;
        }
        break;
    }
}

/* ------------------------------------------------------------------- API */
void z80_reset(z80_t *z)
{
    int i;
    uint8_t *p = (uint8_t *)z;
    for (i = 0; i < (int)sizeof *z; i++) p[i] = 0;
    if (!tables_ready) make_tables();
    /* what MAME reports on the first traced instruction */
    z->f = ZF;
    z->ixh = z->ixl = z->iyh = z->iyl = 0xFF;
    z->irq_vector = 0xFF;                 /* the board leaves $FF on the bus */
}

/* IM 0 with $FF on the bus is RST $38: 13 T-states, one refresh. */
static void take_irq(z80_t *z)
{
    z->halt = 0;
    z->iff1 = z->iff2 = 0;
    z->irq_line = 0;                      /* HOLD_LINE: cleared on acknowledge */
    z->r = (uint8_t)((z->r & 0x80) | ((z->r + 1) & 0x7F));
    z->cycles += 2;
    if (z->im == 2) {
        uint16_t v = (uint16_t)((z->i << 8) | z->irq_vector);
        uint8_t lo, hi;
        z->cycles += 5;
        push16(z, z->pc);
        lo = rd(z, v);
        hi = rd(z, (uint16_t)(v + 1));
        z->pc = (uint16_t)(lo | (hi << 8));
    } else if (z->im == 1) {
        z->cycles += 5;
        push16(z, z->pc);
        z->pc = 0x0038;
    } else if ((z->irq_vector & 0xC7) == 0xC7) {
        z->cycles += 5;
        push16(z, z->pc);
        z->pc = (uint16_t)(z->irq_vector & 0x38);
    }
    z->wz = z->pc;
}

int z80_ready(z80_t *z)
{
    if (z->after_ei) {
        z->after_ei = 0;
    } else if (z->irq_line && z->iff1) {
        take_irq(z);
    }
    if (z->halt) {                        /* refresh cycles, no instruction */
        z->cycles += 4;
        z->r = (uint8_t)((z->r & 0x80) | ((z->r + 1) & 0x7F));
        z->q = z->qt;
        z->qt = YXF;
        return 0;
    }
    return 1;
}

void z80_exec(z80_t *z) { exec_main(z, 0); }
