"""6809 -> C transpiler for Mikie.

Code generation model:
  - one single function cpu_run() with a label per instruction
  - direct control flow (branches, absolute JMP/JSR) -> goto: free
  - indirect control flow (RTS, RTI, JMP [A,U], JSR ,Y) -> a label table indexed
    by PC, trapping on any address that was never recompiled
  - the stack is the real one in emulated memory: the game manipulates it by
    hand (LDS #$3000, PULS PC, RTI, CWAI), so the C stack cannot be used

Usage: python tools/transpile.py  ->  src/gen/mikie_gen.c
"""
import os, sys, io, contextlib
from collections import defaultdict

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
import m6809 as M
import analyze as A

def s8(v):  return v - 256 if v & 0x80 else v
def s16(v): return v - 65536 if v & 0x8000 else v

TFRREG = {0: ('R_D', 16), 1: ('r_x', 16), 2: ('r_y', 16), 3: ('r_u', 16),
          4: ('r_s', 16), 5: ('PC', 16), 8: ('r_a', 8), 9: ('r_b', 8),
          10: ('r_cc', 8), 11: ('r_dp', 8)}

class Gen:
    def __init__(self, insns):
        self.insns = insns
        self.known = set(insns)
        self.out = []
        self.traps = 0

    # ---------------------------------------------------------- addressing
    def ea(self, ins):
        """(prelude statements, address expression)"""
        d = ins.idx
        if d is None or d.get('kind') in (None, 'invalid'):
            return [], None
        reg = 'r_' + d['reg'].lower()
        k, pre = d['kind'], []
        if   k == 'off5':   e = f'(uint16_t)({reg} + ({d["off"]}))'
        elif k == 'off8':   e = f'(uint16_t)({reg} + ({s8(d["val"])}))'
        elif k == 'off16':  e = f'(uint16_t)({reg} + ({s16(d["val"])}))'
        elif k == 'zero':   e = reg
        elif k == 'inc1':   pre.append(f'ea = {reg}; {reg}++;'); e = 'ea'
        elif k == 'inc2':   pre.append(f'ea = {reg}; {reg} += 2;'); e = 'ea'
        elif k == 'dec1':   pre.append(f'{reg}--; ea = {reg};'); e = 'ea'
        elif k == 'dec2':   pre.append(f'{reg} -= 2; ea = {reg};'); e = 'ea'
        # A, B and D are SIGNED offsets (two's complement)
        elif k == 'areg':   e = f'(uint16_t)({reg} + (int8_t)r_a)'
        elif k == 'breg':   e = f'(uint16_t)({reg} + (int8_t)r_b)'
        elif k == 'dreg':   e = f'(uint16_t)({reg} + (int16_t)R_D)'
        # PC-relative: resolvable at compile time
        elif k == 'pc8':    e = f'0x{(ins.pc + ins.length + s8(d["val"])) & 0xFFFF:04X}'
        elif k == 'pc16':   e = f'0x{(ins.pc + ins.length + s16(d["val"])) & 0xFFFF:04X}'
        elif k == 'extind': e = f'0x{d["val"]:04X}'
        else:               return [], None
        if d.get('ind'):
            e = f'mem_read16({e})'
        return pre, e

    def src8(self, ins):
        """expression reading the 8-bit operand"""
        m = ins.mode
        if m == M.IMM8: return [], f'0x{ins.operand:02X}'
        if m == M.DIR:  return [], f'mem_read(DIRA(0x{ins.operand:02X}))'
        if m == M.EXT:  return [], f'mem_read(0x{ins.operand:04X})'
        if m == M.IDX:
            pre, e = self.ea(ins)
            return (pre, f'mem_read({e})') if e else (pre, None)
        return [], None

    def src16(self, ins):
        m = ins.mode
        if m == M.IMM16: return [], f'0x{ins.operand:04X}'
        if m == M.DIR:   return [], f'mem_read16(DIRA(0x{ins.operand:02X}))'
        if m == M.EXT:   return [], f'mem_read16(0x{ins.operand:04X})'
        if m == M.IDX:
            pre, e = self.ea(ins)
            return (pre, f'mem_read16({e})') if e else (pre, None)
        return [], None

    def dst_ea(self, ins):
        """destination address for stores and read-modify-write"""
        m = ins.mode
        if m == M.DIR: return [], f'DIRA(0x{ins.operand:02X})'
        if m == M.EXT: return [], f'0x{ins.operand:04X}'
        if m == M.IDX: return self.ea(ins)
        return [], None

    # ------------------------------------------------------------- jumps
    def goto(self, target):
        if target in self.known:
            return f'goto L_{target:04X};'
        self.traps += 1
        return f'DISPATCH(0x{target:04X});   /* not recompiled */'

    # -------------------------------------------------------- generation
    def insn(self, ins):
        mn, mode = ins.mnem, ins.mode
        nxt = (ins.pc + ins.length) & 0xFFFF
        L = []
        def add(s): L.append(s)

        # ---- 8-bit loads and stores
        if mn in ('LDA', 'LDB'):
            r = 'r_a' if mn == 'LDA' else 'r_b'
            pre, s = self.src8(ins)
            if s is None: return self.trap(ins)
            L += pre; add(f'{r} = {s}; SETNZ8({r}); CLRV();')
        elif mn in ('STA', 'STB'):
            r = 'r_a' if mn == 'STA' else 'r_b'
            pre, e = self.dst_ea(ins)
            if e is None: return self.trap(ins)
            L += pre; add(f'mem_write({e}, {r}); SETNZ8({r}); CLRV();')
        # ---- 16-bit loads and stores
        elif mn in ('LDD', 'LDX', 'LDY', 'LDU', 'LDS'):
            pre, s = self.src16(ins)
            if s is None: return self.trap(ins)
            L += pre
            if mn == 'LDD':
                add(f'SET_D({s}); SETNZ16(R_D); CLRV();')
            else:
                r = 'r_' + mn[2].lower()
                add(f'{r} = {s}; SETNZ16({r}); CLRV();')
        elif mn in ('STD', 'STX', 'STY', 'STU', 'STS'):
            pre, e = self.dst_ea(ins)
            if e is None: return self.trap(ins)
            L += pre
            v = 'R_D' if mn == 'STD' else 'r_' + mn[2].lower()
            add(f'mem_write16({e}, {v}); SETNZ16({v}); CLRV();')
        # ---- 8-bit ALU
        elif mn[:-1] in ('ADD', 'ADC', 'SUB', 'SBC', 'CMP', 'AND', 'OR', 'EOR', 'BIT') \
                and mn[-1] in 'AB' and mn not in ('CMPD', 'CMPX', 'CMPY', 'CMPU', 'CMPS'):
            op, acc = mn[:-1], mn[-1]
            r = 'r_a' if acc == 'A' else 'r_b'
            pre, s = self.src8(ins)
            if s is None: return self.trap(ins)
            L += pre
            if   op == 'ADD': add(f'{r} = alu_add8({r}, {s}, 0);')
            elif op == 'ADC': add(f'{r} = alu_add8({r}, {s}, GETC() ? 1 : 0);')
            elif op == 'SUB': add(f'{r} = alu_sub8({r}, {s}, 0);')
            elif op == 'SBC': add(f'{r} = alu_sub8({r}, {s}, GETC() ? 1 : 0);')
            elif op == 'CMP': add(f'(void)alu_sub8({r}, {s}, 0);')
            elif op == 'AND': add(f'{r} &= {s}; SETNZ8({r}); CLRV();')
            elif op == 'OR':  add(f'{r} |= {s}; SETNZ8({r}); CLRV();')
            elif op == 'EOR': add(f'{r} ^= {s}; SETNZ8({r}); CLRV();')
            elif op == 'BIT': add(f't8 = (uint8_t)({r} & {s}); SETNZ8(t8); CLRV();')
        # ---- 16-bit ALU
        elif mn in ('ADDD', 'SUBD'):
            pre, s = self.src16(ins)
            if s is None: return self.trap(ins)
            L += pre
            f = 'alu_add16' if mn == 'ADDD' else 'alu_sub16'
            add(f'SET_D({f}(R_D, {s}));')
        elif mn in ('CMPD', 'CMPX', 'CMPY', 'CMPU', 'CMPS'):
            pre, s = self.src16(ins)
            if s is None: return self.trap(ins)
            L += pre
            v = 'R_D' if mn == 'CMPD' else 'r_' + mn[3].lower()
            add(f'(void)alu_sub16({v}, {s});')
        # ---- read-modify-write (memory and accumulators)
        elif mn.rstrip('AB') in ('NEG', 'COM', 'LSR', 'ROR', 'ASR', 'ASL', 'LSL',
                                 'ROL', 'DEC', 'INC', 'TST', 'CLR') \
                and mn not in ('LDA', 'LDB'):
            base = mn.rstrip('AB') if mn[-1] in 'AB' and len(mn) > 3 else mn
            inher = mode == M.INH
            if inher:
                r = 'r_a' if mn[-1] == 'A' else 'r_b'
                get, setv = r, lambda x: f'{r} = {x};'
                pre = []
            else:
                pre, e = self.dst_ea(ins)
                if e is None: return self.trap(ins)
                pre = pre + [f'ea2 = {e};']
                get, setv = 'mem_read(ea2)', lambda x: f'mem_write(ea2, {x});'
            L += pre
            add(f't8 = {get};')
            if   base == 'NEG': add('t8b = (uint8_t)(0 - t8); SETC(t8 != 0); '
                                    'SETV(t8 == 0x80); SETNZ8(t8b);')
            elif base == 'COM': add('t8b = (uint8_t)~t8; SETNZ8(t8b); CLRV(); SETC(1);')
            elif base == 'LSR': add('SETC(t8 & 1); t8b = (uint8_t)(t8 >> 1); SETNZ8(t8b);')
            elif base == 'ROR': add('t8b = (uint8_t)((t8 >> 1) | (GETC() ? 0x80 : 0)); '
                                    'SETC(t8 & 1); SETNZ8(t8b);')
            elif base == 'ASR': add('SETC(t8 & 1); t8b = (uint8_t)((t8 >> 1) | (t8 & 0x80)); '
                                    'SETNZ8(t8b);')
            elif base in ('ASL', 'LSL'):
                add('t8b = (uint8_t)(t8 << 1); SETC(t8 & 0x80); '
                    'SETV(((t8 ^ t8b) & 0x80) != 0); SETNZ8(t8b);')
            elif base == 'ROL': add('t8b = (uint8_t)((t8 << 1) | (GETC() ? 1 : 0)); '
                                    'SETC(t8 & 0x80); SETV((((uint8_t)(t8 << 1) ^ t8) & 0x80) != 0); '
                                    'SETNZ8(t8b);')
            elif base == 'DEC': add('t8b = (uint8_t)(t8 - 1); SETV(t8 == 0x80); SETNZ8(t8b);')
            elif base == 'INC': add('t8b = (uint8_t)(t8 + 1); SETV(t8 == 0x7F); SETNZ8(t8b);')
            elif base == 'TST': add('SETNZ8(t8); CLRV();')
            elif base == 'CLR': add('t8b = 0; r_cc = (uint8_t)((r_cc & ~(CC_N|CC_V|CC_C)) | CC_Z);')
            if base not in ('TST',):
                add(setv('t8b'))
        # ---- assorted inherent operations
        elif mn == 'NOP':  add(';')
        elif mn == 'MUL':  add('t16 = (uint16_t)((uint16_t)r_a * (uint16_t)r_b); '
                               'SETC(t16 & 0x80); r_cc = (uint8_t)((r_cc & ~CC_Z) | '
                               '(t16 == 0 ? CC_Z : 0)); SET_D(t16);')
        elif mn == 'SEX':  add('r_a = (uint8_t)((r_b & 0x80) ? 0xFF : 0x00); '
                               'SETNZ16(R_D); CLRV();')
        elif mn == 'ABX':  add('r_x = (uint16_t)(r_x + r_b);')
        elif mn == 'DAA':  add('daa();')
        elif mn == 'ANDCC': add(f'r_cc &= 0x{ins.operand:02X};')
        elif mn == 'ORCC':  add(f'r_cc |= 0x{ins.operand:02X};')
        elif mn in ('LEAX', 'LEAY', 'LEAU', 'LEAS'):
            pre, e = self.ea(ins)
            if e is None: return self.trap(ins)
            L += pre
            r = 'r_' + mn[3].lower()
            add(f'{r} = {e};')
            if mn in ('LEAX', 'LEAY'):      # only LEAX/LEAY touch Z
                add(f'r_cc = (uint8_t)((r_cc & ~CC_Z) | ({r} == 0 ? CC_Z : 0));')
        elif mn in ('TFR', 'EXG'):
            hi, lo = ins.operand >> 4, ins.operand & 0xF
            a_, aw = TFRREG.get(hi, (None, 0)); b_, bw = TFRREG.get(lo, (None, 0))
            if a_ is None or b_ is None: return self.trap(ins)
            def rd(n):  return 'R_D' if n == 'R_D' else n
            def wr(n, v):
                return f'SET_D({v});' if n == 'R_D' else f'{n} = ({"uint16_t" if n in ("r_x","r_y","r_u","r_s") else "uint8_t"})({v});'
            if mn == 'TFR':
                if b_ == 'PC': add(f'DISPATCH({rd(a_)});')
                else:          add(wr(b_, rd(a_)))
            else:
                add(f't16 = {rd(a_)}; t16b = {rd(b_)};')
                add(wr(a_, 't16b')); add(wr(b_, 't16'))
        # ---- push / pull (constant postbyte, so emit the explicit sequence)
        elif mn in ('PSHS', 'PSHU', 'PULS', 'PULU'):
            st = 'S' if mn[-1] == 'S' else 'U'
            pb = ins.operand
            order_push = [(0x80, 'PC'), (0x40, 'r_u' if st == 'S' else 'r_s'),
                          (0x20, 'r_y'), (0x10, 'r_x'), (0x08, 'r_dp'),
                          (0x04, 'r_b'), (0x02, 'r_a'), (0x01, 'r_cc')]
            if mn.startswith('PSH'):
                for bit, reg in order_push:
                    if not (pb & bit): continue
                    if reg == 'PC': add(f'PUSH{st}16(0x{nxt:04X});')
                    elif reg in ('r_dp', 'r_b', 'r_a', 'r_cc'): add(f'PUSH{st}8({reg});')
                    else: add(f'PUSH{st}16({reg});')
            else:
                for bit, reg in reversed(order_push):
                    if not (pb & bit): continue
                    if reg == 'PC': add(f'PULL{st}16(t16); DISPATCH(t16);')
                    elif reg in ('r_dp', 'r_b', 'r_a', 'r_cc'): add(f'PULL{st}8({reg});')
                    else: add(f'PULL{st}16({reg});')
        # ---- control flow
        elif ins.flow == M.BRANCH:
            cond = {'BEQ': 'C_Z', 'BNE': '!C_Z', 'BCS': 'C_C', 'BCC': '!C_C',
                    'BMI': 'C_N', 'BPL': '!C_N', 'BVS': 'C_V', 'BVC': '!C_V',
                    'BHI': 'C_HI', 'BLS': 'C_LS', 'BGE': 'C_GE', 'BLT': 'C_LT',
                    'BGT': 'C_GT', 'BLE': 'C_LE'}[mn.lstrip('L')]
            # LONG branches cost one extra cycle when the branch is taken
            extra = 'cpu_cycles++; ' if mode == M.REL16 else ''
            add(f'if ({cond}) {{ {extra}{self.goto(ins.target)} }}')
        elif mn in ('BRA', 'LBRA', 'JMP') and ins.flow == M.JUMP:
            add(self.goto(ins.target))
        elif mn == 'BRN':  add(';')
        elif mn in ('BSR', 'LBSR') or (mn == 'JSR' and ins.flow == M.CALL):
            add(f'PUSHS16(0x{nxt:04X});')
            add(self.goto(ins.target))
        elif mn == 'JSR' and ins.flow == M.IND_CALL:
            pre, e = self.ea(ins)
            if e is None: return self.trap(ins)
            L += pre
            add(f'ea = {e}; PUSHS16(0x{nxt:04X}); DISPATCH(ea);')
        elif mn == 'JMP' and ins.flow == M.IND_JUMP:
            pre, e = self.ea(ins)
            if e is None: return self.trap(ins)
            L += pre
            add(f'DISPATCH({e});')
        elif mn == 'RTS':
            add('PULLS16(t16); DISPATCH(t16);')
        elif mn == 'RTI':
            add('PULLS8(r_cc);')
            add('if (r_cc & CC_E) { PULLS8(r_a); PULLS8(r_b); PULLS8(r_dp); '
                'PULLS16(r_x); PULLS16(r_y); PULLS16(r_u); }')
            add('PULLS16(t16); DISPATCH(t16);')
        elif mn == 'CWAI':
            # CC &= imm (normally unmasking I), then push the ENTIRE machine
            # state and wait for the interrupt. The handler does NOT push again:
            # it finds everything stacked, and its RTI returns to the next insn.
            add(f'r_cc &= 0x{ins.operand:02X}; r_cc |= CC_E;')
            add(f'PUSHS16(0x{nxt:04X}); PUSHS16(r_u); PUSHS16(r_y); PUSHS16(r_x);')
            add('PUSHS8(r_dp); PUSHS8(r_b); PUSHS8(r_a); PUSHS8(r_cc);')
            add('cpu_wait_irq();')
            add('r_cc |= CC_I; irq_pending = 0;')
            # even leaving CWAI the interrupt entry costs 19 cycles, although
            # the state is already on the stack
            add('cpu_cycles += 19;')
            add('goto L_IRQVEC;')
        elif mn in ('SWI', 'SWI2', 'SWI3', 'SYNC'):
            add(f'cpu_unknown_pc(0x{ins.pc:04X}); return;   /* {mn} non implementata */')
        else:
            return self.trap(ins)

        return L

    def trap(self, ins):
        self.traps += 1
        return [f'cpu_unknown_pc(0x{ins.pc:04X}); return;   '
                f'/* NOT TRANSLATED: {ins.mnem} {ins.mode} */']


def main():
    with contextlib.redirect_stdout(io.StringIO()):
        A.main()
    insns = A.insns
    g = Gen(insns)
    addrs = sorted(insns)

    o = []
    w = o.append
    w('/* GENERATED by tools/transpile.py - do not edit by hand */')
    w('#include "m6809_rt.h"')
    w('')
    w('#if defined(__GNUC__) || defined(__clang__)')
    w('# define USE_COMPUTED_GOTO 1')
    w('#else')
    w('# error "needs a compiler with label-as-value (GCC/Clang)"')
    w('#endif')
    w('')
    w('static const void *LBL[0x10000];')
    w('')
    w('#define DISPATCH(a) do {                          \\')
    w('        uint16_t _a = (uint16_t)(a);              \\')
    w('        const void *_t = LBL[_a];                 \\')
    w('        if (!_t) { cpu_unknown_pc(_a); return; }  \\')
    w('        goto *_t;                                 \\')
    w('    } while (0)')
    w('')
    w('/* Instruction boundary: advance vblank and, if the line is asserted and')
    w('   the mask allows it, take the interrupt (19 cycles). */')
    w('#define IRQCK(next) do {                                                  \\')
    w('        vblank_poll();                                                    \\')
    w('        if (irq_pending && !(r_cc & CC_I)) {                              \\')
    w('            r_cc |= CC_E;                                                 \\')
    w('            PUSHS16((uint16_t)(next)); PUSHS16(r_u); PUSHS16(r_y);        \\')
    w('            PUSHS16(r_x); PUSHS8(r_dp); PUSHS8(r_b); PUSHS8(r_a);         \\')
    w('            PUSHS8(r_cc); r_cc |= CC_I; irq_pending = 0;                  \\')
    w('            cpu_cycles += 19;                                             \\')
    w('            goto L_IRQVEC;                                                \\')
    w('        } } while (0)')
    w('')
    w('#ifdef MIKIE_TRACE')
    w('# define TRACE(pc) trace_insn((uint16_t)(pc))')
    w('#else')
    w('# define TRACE(pc) ((void)0)')
    w('#endif')
    w('')
    w('void cpu_run(void)')
    w('{')
    w('    uint16_t ea, ea2, t16, t16b;')
    w('    uint8_t  t8, t8b;')
    w('    (void)ea; (void)ea2; (void)t16; (void)t16b; (void)t8; (void)t8b;')
    w('')
    w('    if (!LBL[0]) { LBL[0] = (const void *)1; goto init_labels; }')
    w('resume:')
    w('    DISPATCH(cpu_entry_pc());')
    w('')

    irqvec = A.mem[0xFFF8] << 8 | A.mem[0xFFF9]
    w(f'L_IRQVEC: goto L_{irqvec:04X};')
    w('')

    for pc in addrs:
        ins = insns[pc]
        body = g.insn(ins)
        dis = f'{ins.mnem} {ins.mode}'
        w(f'L_{pc:04X}: /* {ins.bytes.hex()}  {dis} */')
        # The 6809 stacks the PC of the instruction ABOUT TO run, not the next
        # one: the interrupt arrives BEFORE the instruction starts.
        w(f'    IRQCK(0x{pc:04X});')
        w(f'    TRACE(0x{pc:04X});')
        if ins.mnem == 'RTI':
            # 6 cycles when only CC+PC come back, 15 for the full state
            w('    cpu_cycles += (r_cc & CC_E) ? 15 : 6;')
        else:
            w(f'    cpu_cycles += {M.cycles(ins)};')
        for line in body:
            w('    ' + line)
        # natural fall-through to the next instruction
        nxt = (pc + ins.length) & 0xFFFF
        if ins.flow in (M.SEQ, M.BRANCH, M.CALL) and nxt not in insns:
            w(f'    DISPATCH(0x{nxt:04X});')
    w('')
    w('init_labels:')
    for pc in addrs:
        w(f'    LBL[0x{pc:04X}] = &&L_{pc:04X};')
    w('    goto resume;')
    w('}')

    outd = os.path.join(ROOT, 'src', 'gen')
    os.makedirs(outd, exist_ok=True)
    path = os.path.join(outd, 'mikie_gen.c')
    with open(path, 'w') as f:
        f.write('\n'.join(o) + '\n')

    print(f'instructions translated : {len(addrs):,}')
    print(f'traps emitted           : {g.traps}')
    print(f'C lines generated       : {len(o):,}')
    print(f'wrote {path} ({os.path.getsize(path)//1024} KB)')

if __name__ == '__main__':
    main()
