"""Complete MC6809 decoder: length, mnemonic, addressing mode, operand and
control-flow class, plus the cycle table.

It is the shared foundation of the static analysis (analyze.py) and the
transpiler (transpile.py).

decode(mem, pc) -> Insn
"""

# control-flow classes
SEQ, BRANCH, JUMP, CALL, RET, IND_JUMP, IND_CALL, ILLEGAL = \
    'seq', 'branch', 'jump', 'call', 'ret', 'ijump', 'icall', 'illegal'

# addressing modes
INH, IMM8, IMM16, DIR, EXT, IDX, REL8, REL16, PSH = \
    'inh', 'imm8', 'imm16', 'dir', 'ext', 'idx', 'rel8', 'rel16', 'psh'

# ----------------------------------------------------------------- tables
_ALU8  = ['SUB', 'CMP', 'SBC', None, 'AND', 'BIT', 'LD', None,
          'EOR', 'ADC', 'OR', 'ADD', None, None, None, None]

def _build():
    t = {}
    def put(op, mn, mode, flow=SEQ):
        t[op] = (mn, mode, flow)

    # --- 0x00-0x0F direct read-modify-write
    rmw = {0x0: 'NEG', 0x3: 'COM', 0x4: 'LSR', 0x6: 'ROR', 0x7: 'ASR',
           0x8: 'ASL', 0x9: 'ROL', 0xA: 'DEC', 0xC: 'INC', 0xD: 'TST', 0xF: 'CLR'}
    for lo, mn in rmw.items():
        put(0x00 | lo, mn, DIR)
        put(0x60 | lo, mn, IDX)
        put(0x70 | lo, mn, EXT)
    put(0x0E, 'JMP', DIR, JUMP)
    put(0x6E, 'JMP', IDX, IND_JUMP)
    put(0x7E, 'JMP', EXT, JUMP)

    # --- inherent / misc
    put(0x12, 'NOP',  INH)
    put(0x13, 'SYNC', INH)
    put(0x16, 'LBRA', REL16, JUMP)
    put(0x17, 'LBSR', REL16, CALL)
    put(0x19, 'DAA',  INH)
    put(0x1A, 'ORCC', IMM8)
    put(0x1C, 'ANDCC',IMM8)
    put(0x1D, 'SEX',  INH)
    put(0x1E, 'EXG',  IMM8)
    put(0x1F, 'TFR',  IMM8)
    put(0x39, 'RTS',  INH, RET)
    put(0x3A, 'ABX',  INH)
    put(0x3B, 'RTI',  INH, RET)
    put(0x3C, 'CWAI', IMM8)
    put(0x3D, 'MUL',  INH)
    put(0x3F, 'SWI',  INH, RET)

    # --- short branches
    cond = ['BRA', 'BRN', 'BHI', 'BLS', 'BCC', 'BCS', 'BNE', 'BEQ',
            'BVC', 'BVS', 'BPL', 'BMI', 'BGE', 'BLT', 'BGT', 'BLE']
    for i, mn in enumerate(cond):
        flow = JUMP if mn == 'BRA' else (SEQ if mn == 'BRN' else BRANCH)
        put(0x20 + i, mn, REL8, flow)
    put(0x8D, 'BSR', REL8, CALL)

    # --- LEA
    for i, r in enumerate('XYSU'):
        put(0x30 + i, 'LEA' + r, IDX)
    # --- push/pull
    put(0x34, 'PSHS', PSH); put(0x35, 'PULS', PSH)
    put(0x36, 'PSHU', PSH); put(0x37, 'PULU', PSH)

    # --- inherent A/B accumulator forms (0x40-0x5F)
    for lo, mn in rmw.items():
        if mn == 'TST':
            put(0x40 | lo, 'TSTA', INH); put(0x50 | lo, 'TSTB', INH); continue
        put(0x40 | lo, mn + 'A', INH)
        put(0x50 | lo, mn + 'B', INH)

    # --- ALU on A (0x80..0xBF) and on B (0xC0..0xFF)
    for acc, b in (('A', 0x80), ('B', 0xC0)):
        for lo in range(16):
            mn = _ALU8[lo]
            for k, mode in ((0x00, IMM8), (0x10, DIR), (0x20, IDX), (0x30, EXT)):
                op = b + k + lo
                if lo == 0x3:                      # SUBD / ADDD
                    put(op, 'SUBD' if acc == 'A' else 'ADDD',
                        IMM16 if mode == IMM8 else mode); continue
                if lo == 0xC:                      # CMPX / LDD
                    put(op, 'CMPX' if acc == 'A' else 'LDD',
                        IMM16 if mode == IMM8 else mode); continue
                if lo == 0xE:                      # LDX / LDU
                    put(op, 'LDX' if acc == 'A' else 'LDU',
                        IMM16 if mode == IMM8 else mode); continue
                if lo in (0xD, 0xF):
                    continue                       # handled below
                if mn is None:
                    continue
                put(op, mn + acc, mode)
        # 0xD / 0xF: JSR/STA/STX ...
    for acc, b in (('A', 0x80), ('B', 0xC0)):
        st = 'ST' + acc
        sr = 'STX' if acc == 'A' else 'STU'
        for k, mode in ((0x10, DIR), (0x20, IDX), (0x30, EXT)):
            put(b + k + 0x7, st, mode)
            put(b + k + 0xF, sr, mode)
        put(b + 0x30 + 0xD, 'JSR', EXT, CALL)
        put(b + 0x10 + 0xD, 'JSR', DIR, CALL)
        put(b + 0x20 + 0xD, 'JSR', IDX, IND_CALL)
    # fixups: 0x8D is BSR; 0xCD, 0xC7, 0x8F and 0xCF do not exist
    put(0x8D, 'BSR', REL8, CALL)
    for op in (0x87, 0x8F, 0xC7, 0xCF, 0xCD):
        t.pop(op, None)
    # STD/LDD on the B page: 0xDD STD dir, 0xED STD idx, 0xFD STD ext
    for k, mode in ((0x10, DIR), (0x20, IDX), (0x30, EXT)):
        put(0xC0 + k + 0xD, 'STD', mode)
    put(0xCC, 'LDD', IMM16)
    put(0x83, 'SUBD', IMM16); put(0xC3, 'ADDD', IMM16)
    put(0x8C, 'CMPX', IMM16); put(0x8E, 'LDX', IMM16); put(0xCE, 'LDU', IMM16)
    return t

OPS = _build()

# --- page 0x10
OPS10 = {}
for i, mn in enumerate(['BRN', 'BHI', 'BLS', 'BCC', 'BCS', 'BNE', 'BEQ',
                        'BVC', 'BVS', 'BPL', 'BMI', 'BGE', 'BLT', 'BGT', 'BLE']):
    OPS10[0x21 + i] = ('L' + mn, REL16, SEQ if mn == 'BRN' else BRANCH)
OPS10[0x3F] = ('SWI2', INH, SEQ)
for k, mode in ((0x80, IMM16), (0x90, DIR), (0xA0, IDX), (0xB0, EXT)):
    OPS10[k + 0x3] = ('CMPD', mode)
    OPS10[k + 0xC] = ('CMPY', mode)
    OPS10[k + 0xE] = ('LDY',  mode)
    if k != 0x80:
        OPS10[k + 0xF] = ('STY', mode)
for k, mode in ((0xC0, IMM16), (0xD0, DIR), (0xE0, IDX), (0xF0, EXT)):
    OPS10[k + 0xE] = ('LDS', mode)
    if k != 0xC0:
        OPS10[k + 0xF] = ('STS', mode)

# --- page 0x11
OPS11 = {0x3F: ('SWI3', INH, SEQ)}
for k, mode in ((0x80, IMM16), (0x90, DIR), (0xA0, IDX), (0xB0, EXT)):
    OPS11[k + 0x3] = ('CMPU', mode)
    OPS11[k + 0xC] = ('CMPS', mode)

IDXREG = ['X', 'Y', 'U', 'S']

class Insn:
    __slots__ = ('pc', 'length', 'mnem', 'mode', 'flow', 'operand', 'target',
                 'postbyte', 'idx', 'bytes', 'exp_cyc')
    def __init__(self, **kw):
        for k in self.__slots__:
            setattr(self, k, kw.get(k))
    def __repr__(self):
        t = f' -> ${self.target:04X}' if self.target is not None else ''
        return f'{self.pc:04X}: {self.mnem} {self.mode}{t} (len {self.length})'

def _idx_extra(pb):
    """extra bytes consumed by the indexed postbyte, plus a description"""
    if not (pb & 0x80):
        off = pb & 0x1F
        if off & 0x10:
            off -= 0x20
        return 0, {'kind': 'off5', 'off': off, 'reg': IDXREG[(pb >> 5) & 3], 'ind': False}
    ind = bool(pb & 0x10)
    reg = IDXREG[(pb >> 5) & 3]
    m = pb & 0x0F
    kinds = {0x0: ('inc1', 0), 0x1: ('inc2', 0), 0x2: ('dec1', 0), 0x3: ('dec2', 0),
             0x4: ('zero', 0), 0x5: ('breg', 0), 0x6: ('areg', 0), 0x8: ('off8', 1),
             0x9: ('off16', 2), 0xB: ('dreg', 0), 0xC: ('pc8', 1), 0xD: ('pc16', 2),
             0xF: ('extind', 2)}
    if m not in kinds:
        return 0, {'kind': 'invalid', 'reg': reg, 'ind': ind}
    kind, extra = kinds[m]
    return extra, {'kind': kind, 'reg': reg, 'ind': ind}

def decode(mem, pc):
    R = lambda a: mem[a & 0xFFFF]          # the 6809 address space wraps at 64K
    start = pc
    op = R(pc); pc += 1
    if op == 0x10:
        ent = OPS10.get(R(pc)); pc += 1
    elif op == 0x11:
        ent = OPS11.get(R(pc)); pc += 1
    else:
        ent = OPS.get(op)
    if ent is None:
        return Insn(pc=start, length=1, mnem='???', mode=INH, flow=ILLEGAL,
                    bytes=bytes([R(start)]))
    mnem, mode = ent[0], ent[1]
    flow = ent[2] if len(ent) > 2 else SEQ
    operand = target = None
    idx = None

    if mode in (INH,):
        pass
    elif mode in (IMM8, DIR, PSH):
        operand = R(pc); pc += 1
    elif mode in (IMM16, EXT):
        operand = (R(pc) << 8) | R(pc + 1); pc += 2
        if mode == EXT and flow in (JUMP, CALL):
            target = operand
    elif mode == REL8:
        d = R(pc); pc += 1
        target = (pc + (d - 256 if d & 0x80 else d)) & 0xFFFF
        operand = target
    elif mode == REL16:
        d = (R(pc) << 8) | R(pc + 1); pc += 2
        target = (pc + (d - 65536 if d & 0x8000 else d)) & 0xFFFF
        operand = target
    elif mode == IDX:
        pb = R(pc); pc += 1
        extra, idx = _idx_extra(pb)
        if extra == 1:
            idx['val'] = R(pc)
        elif extra == 2:
            idx['val'] = (R(pc) << 8) | R(pc + 1)
        pc += extra
        idx['postbyte'] = pb

    # PULS/PULU including PC is a return
    if mnem in ('PULS', 'PULU') and operand is not None and (operand & 0x80):
        flow = RET
    # JMP/JSR direct: the destination depends on DP, so treat them as indirect
    if mnem in ('JMP', 'JSR') and mode == DIR:
        flow = IND_JUMP if mnem == 'JMP' else IND_CALL

    return Insn(pc=start, length=pc - start, mnem=mnem, mode=mode, flow=flow,
                operand=operand, target=target, idx=idx,
                bytes=bytes(R(a) for a in range(start, pc)))


# ============================================================== CYCLES
# MC6809 cycle table (Motorola datasheet), verified empirically against
# MAME's `totalcycles` counter: see tools/verify_cycles.py.

# extra cycles for the indexed postbyte, NON-indirect form.
# The indirect form costs 3 more (extended indirect is a special case).
_IDX_EXTRA = {
    'zero': 0, 'off5': 1, 'off8': 1, 'off16': 4,
    'areg': 1, 'breg': 1, 'dreg': 4,
    'inc1': 2, 'inc2': 3, 'dec1': 2, 'dec2': 3,
    'pc8': 1, 'pc16': 5, 'extind': 2,
}

def idx_cycles(idx):
    if not idx or idx.get('kind') not in _IDX_EXTRA:
        return 0
    k = idx['kind']
    if k == 'extind':
        return 5                      # [$nnnn]: flat 5
    return _IDX_EXTRA[k] + (3 if idx.get('ind') else 0)

# (imm, dir, idx_base, ext) per family
_GRP = {
    'alu8':   (2, 4, 4, 5),   # LDA LDB ADDA CMPA ANDA ... STA STB
    'ld16':   (3, 5, 5, 6),   # LDD LDX LDU STD STX STU
    'ld16p':  (4, 6, 6, 7),   # LDY LDS STY STS  (page-1 prefix)
    'alu16':  (4, 6, 6, 7),   # ADDD SUBD CMPX
    'cmp16p': (5, 7, 7, 8),   # CMPD CMPY CMPU CMPS
    'rmw':    (0, 6, 6, 7),   # NEG COM LSR ROR ASR ASL ROL DEC INC TST CLR
    'jmp':    (0, 3, 3, 4),
    'jsr':    (0, 7, 7, 8),
}

_FAM = {}
for _m in ('LDA', 'LDB', 'STA', 'STB', 'ADDA', 'ADDB', 'ADCA', 'ADCB', 'SUBA',
           'SUBB', 'SBCA', 'SBCB', 'CMPA', 'CMPB', 'ANDA', 'ANDB', 'ORA', 'ORB',
           'EORA', 'EORB', 'BITA', 'BITB'):
    _FAM[_m] = 'alu8'
for _m in ('LDD', 'LDX', 'LDU', 'STD', 'STX', 'STU'):   _FAM[_m] = 'ld16'
for _m in ('LDY', 'LDS', 'STY', 'STS'):                 _FAM[_m] = 'ld16p'
for _m in ('ADDD', 'SUBD', 'CMPX'):                     _FAM[_m] = 'alu16'
for _m in ('CMPD', 'CMPY', 'CMPU', 'CMPS'):             _FAM[_m] = 'cmp16p'
for _m in ('NEG', 'COM', 'LSR', 'ROR', 'ASR', 'ASL', 'LSL', 'ROL', 'DEC',
           'INC', 'TST', 'CLR'):                        _FAM[_m] = 'rmw'
_FAM['JMP'] = 'jmp'
_FAM['JSR'] = 'jsr'

_INHERENT = {
    'NOP': 2, 'DAA': 2, 'SEX': 2, 'ABX': 3, 'MUL': 11, 'RTS': 5, 'RTI': 6,
    'SWI': 19, 'SWI2': 20, 'SWI3': 20, 'SYNC': 4,
    'NEGA': 2, 'NEGB': 2, 'COMA': 2, 'COMB': 2, 'LSRA': 2, 'LSRB': 2,
    'RORA': 2, 'RORB': 2, 'ASRA': 2, 'ASRB': 2, 'ASLA': 2, 'ASLB': 2,
    'LSLA': 2, 'LSLB': 2, 'ROLA': 2, 'ROLB': 2, 'DECA': 2, 'DECB': 2,
    'INCA': 2, 'INCB': 2, 'TSTA': 2, 'TSTB': 2, 'CLRA': 2, 'CLRB': 2,
}
_MODE_SLOT = {IMM8: 0, IMM16: 0, DIR: 1, IDX: 2, EXT: 3}

def cycles(ins, taken=None):
    """Cycle cost. `taken` matters only for long branches (LBcc), which cost
    5 when not taken and 6 when taken; short branches always cost 3."""
    mn, mode = ins.mnem, ins.mode
    if mn in _INHERENT:
        return _INHERENT[mn]
    if mn in ('ANDCC', 'ORCC'):     return 3
    if mn == 'TFR':                 return 6
    if mn == 'EXG':                 return 8
    if mn == 'CWAI':                return 20
    if mn.startswith('LEA'):        return 4 + idx_cycles(ins.idx)
    if mn in ('PSHS', 'PSHU', 'PULS', 'PULU'):
        pb = ins.operand or 0
        n = sum((2 if b & 0xF0 else 1) for b in
                (0x80, 0x40, 0x20, 0x10, 0x08, 0x04, 0x02, 0x01) if pb & b)
        return 5 + n
    if mn == 'BSR':                 return 7
    if mn == 'LBSR':                return 9
    if mn == 'LBRA':                return 5
    if mode == REL8:                return 3                  # Bcc and BRA
    if mode == REL16:               return 6 if taken else 5   # LBcc
    fam = _FAM.get(mn)
    if fam is None:
        return 2
    base = _GRP[fam][_MODE_SLOT.get(mode, 1)]
    return base + (idx_cycles(ins.idx) if mode == IDX else 0)
