"""Static analysis of Mikie's 6809 program.

  - recursive descent from the vectors plus every PC seen in the MAME trace
  - jump-table resolution: local propagation of immediates loaded into X/Y/U,
    then enumeration of the table entries
  - code / data / unreached classification
  - call graph

Outputs: analysis/*.json plus a report on stdout.
"""
import os, sys, json
from collections import defaultdict, deque

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import m6809 as M

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUTD = os.path.join(ROOT, 'analysis')
ROM_LO, ROM_HI = 0x6000, 0x10000       # program ROM

mem = bytearray(open(os.path.join(ROOT, 'rom', 'maincpu.bin'), 'rb').read())
covpath = os.path.join(ROOT, 'trace', 'coverage.bin')
cov = bytearray(open(covpath, 'rb').read()) if os.path.exists(covpath) else bytearray(0x10000)

obs_ind = {}
p = os.path.join(ROOT, 'trace', 'indirect_targets.json')
if os.path.exists(p):
    obs_ind = {int(k, 16): [int(t, 16) for t in v] for k, v in json.load(open(p)).items()}

# ------------------------------------------------------------------ state
code   = bytearray(0x10000)   # 1 = byte belongs to an instruction
starts = bytearray(0x10000)   # 1 = instruction start
insns  = {}                   # pc -> Insn
calls  = defaultdict(set)     # caller -> {target}
xrefs  = defaultdict(set)     # target -> {source}
funcs  = set()                # JSR/BSR targets
tables = {}                   # indirect-jump pc -> {'base':.., 'reg':.., 'entries':[..]}
unresolved = []

def in_rom(a):
    return ROM_LO <= a < ROM_HI

# --- padding mask: long runs of identical bytes ($FF or $00) are not code.
#     Without this filter recursive descent happily "executes" ROM filler:
#     $FF FF FF... decodes as a perfectly legal run of extended STU.
padding = bytearray(0x10000)
def _mark_padding(minrun=6):
    a = ROM_LO
    while a < ROM_HI:
        b = mem[a]
        if b in (0x00, 0xFF):
            e = a
            while e < ROM_HI and mem[e] == b:
                e += 1
            if e - a >= minrun:
                for q in range(a, e):
                    padding[q] = 1
            a = e
        else:
            a += 1
_mark_padding()

def plausible(a):
    return in_rom(a) and not padding[a]

# ------------------------------------------------------ table resolution
KNOWN_BASES = set()          # filled in by the first pass

def table_entries(base, limit=64):
    """Read big-endian words while they look like plausible code addresses.
    Stops at the next known table base: tables are packed contiguously, so
    reading past the end would steal the neighbouring table's entries."""
    nxt = min((b for b in KNOWN_BASES if b > base), default=ROM_HI)
    out = []
    a = base
    while len(out) < limit and in_rom(a) and a + 1 < ROM_HI and a < nxt:
        t = (mem[a] << 8) | mem[a + 1]
        if not plausible(t):
            break
        out.append(t)
        a += 2
    return out

_TFR_REG = {0: 'D', 1: 'X', 2: 'Y', 3: 'U', 4: 'S', 5: 'PC',
            8: 'A', 9: 'B', 10: 'CC', 11: 'DP'}
_PULL_BIT = {'CC': 0x01, 'A': 0x02, 'B': 0x04, 'DP': 0x08,
             'X': 0x10, 'Y': 0x20, 'U': 0x40, 'S': 0x40, 'PC': 0x80}

def clobbers(ins, reg):
    """Does this instruction write `reg`? Being exact about TFR and PULS is
    what makes the backward scan usable: a coarse "any TFR ends the search"
    test stops one instruction short of the LDY that matters, because the
    dispatch loop happens to do a TFR B,A on the way past."""
    mn = ins.mnem
    if mn in ('LD' + reg, 'LEA' + reg, 'CLR' + reg):
        return True
    if mn in ('TFR', 'EXG') and ins.operand is not None:
        hi = _TFR_REG.get((ins.operand >> 4) & 0xF)
        lo = _TFR_REG.get(ins.operand & 0xF)
        return lo == reg or (mn == 'EXG' and hi == reg)
    if mn in ('PULS', 'PULU') and ins.operand is not None:
        return bool(ins.operand & _PULL_BIT.get(reg, 0))
    if mn == 'ABX':
        return reg == 'X'
    return False

def preds(a):
    """Instructions that can execute immediately before `a`: the linear one if
    it falls through, plus everything that branches or jumps here."""
    out = []
    for back in range(1, 6):
        q = a - back
        if q >= ROM_LO and starts[q] and insns.get(q) is not None \
           and q + insns[q].length == a \
           and insns[q].flow not in (M.JUMP, M.RET, M.IND_JUMP):
            out.append(q)
            break
    out.extend(q for q in xrefs.get(a, ()) if insns.get(q) is not None)
    return out

def backscan_bases(pc, reg, budget=600):
    """Every table base that can arrive at `pc` in `reg`.

    Returning one base is not enough, because the game shares dispatchers:

        80AD: LDY  #$DA2D          one caller's table
        80B1: JMP  $8303
        ...
        8303: PSHS B / LDA -$E,X / ASLA
        8308: JSR  [A,Y]           <- serves both

    with another caller arriving with $DA3B. Picking whichever base the scan
    reached first recompiled ten handlers and silently dropped seven, which is
    how the game died the moment the player walked into the corridor.

    So: walk the flow graph backwards, gather all of them, and take the union
    of the tables. A path that redefines the register carries no information
    and is dropped; a path that loads the register some other way means we
    would be guessing, and the whole site is abandoned instead.
    """
    seen, found = set(), set()
    q = deque([pc])
    while q:
        for prev in preds(q.popleft()):
            if prev in seen:
                continue
            seen.add(prev)
            budget -= 1
            if budget <= 0:
                return set()
            ins = insns[prev]
            if ins.mnem == 'LD' + reg:
                if ins.mode == M.IMM16:
                    found.add(ins.operand)
                    continue
                idx = ins.idx or {}
                if idx.get('kind') in ('areg', 'breg', 'dreg') and not idx.get('ind'):
                    b = backscan(prev, idx['reg'])
                    if b is not None:
                        found.add(b)
                    continue
                return set()
            if clobbers(ins, reg):
                continue
            q.append(prev)
    return {b for b in found if in_rom(b)}

def backscan_table(pc, reg, budget=400):
    """The register was not loaded with an immediate but read out of a table:

        LDU  #$DC0C          <- the table of handlers
        LDY  A,U             <- pick one
        JSR  ,Y

    Find the LD<reg> <acc>,<base> that filled it, then the immediate that
    filled <base>. This is how the game dispatches per-object behaviour.

    It has to walk the flow graph backwards, not the linear listing: the real
    dispatch loop puts a BRA between the two, and a linear scan stops fourteen
    instructions short of the answer.

    A path that redefines the register is abandoned, not fatal. The loop here
    ends in PULS B,X,Y, restoring what its own PSHS saved a few instructions
    earlier, so the back edge always reaches a write to Y that this code cannot
    see through. Treating that as a failure resolves nothing; treating it as
    "no information" leaves the one path that does reach the LDY. What is NOT
    allowed is two paths disagreeing, or a path reaching the register through
    something other than a table read: either of those would mean guessing, and
    a guessed base recompiles whichever bytes happen to be there.
    """
    seen, found = set(), set()
    q = deque([pc])
    while q:
        for prev in preds(q.popleft()):
            if prev in seen:
                continue
            seen.add(prev)
            budget -= 1
            if budget <= 0:
                return None
            ins = insns[prev]
            if ins.mnem == 'LD' + reg:
                idx = ins.idx or {}
                if idx.get('kind') not in ('areg', 'breg', 'dreg') or idx.get('ind'):
                    return None               # loaded some other way: guessing
                b = backscan(prev, idx['reg'])
                if b is None:
                    return None
                found.add(b)
                continue                      # this path is answered
            if clobbers(ins, reg):
                continue                      # this path tells us nothing
            q.append(prev)
    return found.pop() if len(found) == 1 else None

def backscan(pc, reg, maxi=20):
    """Walk back up the linear chain of instructions preceding pc inside the
    same basic block, looking for LD<reg> #immediate. Needed because forward
    propagation depends on the order in which blocks happen to be visited."""
    chain = []
    a = pc
    for _ in range(maxi):
        prev = None
        for back in range(1, 6):
            q = a - back
            if q >= ROM_LO and starts[q] and insns.get(q) is not None                and q + insns[q].length == a:
                prev = q; break
        if prev is None:
            break
        ins = insns[prev]
        if ins.flow in (M.JUMP, M.RET, M.IND_JUMP):
            break                                   # basic-block boundary
        chain.append(ins)
        a = prev
    for ins in chain:                               # nearest first
        if ins.mnem == 'LD' + reg and ins.mode == M.IMM16:
            return ins.operand
        if ins.mnem in ('LD' + reg, 'LEA' + reg, 'TFR', 'EXG', 'PULS', 'PULU'):
            return None                             # the register is redefined
    return None

def resolve_indirect(pc, ins, regs):
    """Return (base, entries), or (None, []) if not statically resolvable."""
    idx = ins.idx or {}
    if idx.get('kind') == 'extind':                 # JMP [$xxxx] -> vector in RAM
        return None, []
    reg = idx.get('reg')
    base = regs.get(reg)
    if base is None or not in_rom(base):
        return None, []
    return base, table_entries(base)

# ---------------------------------------------------------- recursive descent
def walk(entry, queue):
    """Walk a linear block from `entry`, queueing every branch target."""
    pc = entry
    regs = {}                                   # known immediates in X/Y/U
    while True:
        if not in_rom(pc) or starts[pc] or padding[pc]:
            return
        ins = M.decode(mem, pc)
        if ins.flow == M.ILLEGAL:
            return
        starts[pc] = 1
        for a in range(pc, pc + ins.length):
            code[a & 0xFFFF] = 1
        insns[pc] = ins

        mn, f = ins.mnem, ins.flow
        # --- local propagation of immediates
        if mn in ('LDX', 'LDY', 'LDU') and ins.mode == M.IMM16:
            regs[mn[2]] = ins.operand
        elif mn in ('LDX', 'LDY', 'LDU', 'LEAX', 'LEAY', 'LEAU'):
            regs.pop(mn[-1], None)
        elif mn in ('TFR', 'EXG', 'PULS', 'PULU'):
            regs.clear()

        if f == M.RET:
            return
        if f == M.CALL:
            if ins.target is not None:
                calls[pc].add(ins.target); xrefs[ins.target].add(pc)
                funcs.add(ins.target); queue.append(ins.target)
        elif f == M.BRANCH:
            xrefs[ins.target].add(pc); queue.append(ins.target)
        elif f == M.JUMP:
            xrefs[ins.target].add(pc); queue.append(ins.target)
            return
        elif f in (M.IND_JUMP, M.IND_CALL):
            base, entries = resolve_indirect(pc, ins, regs)
            seen = set(obs_ind.get(pc, []))
            if base is not None:
                tables[pc] = {'base': base, 'bases': [base],
                              'reg': (ins.idx or {}).get('reg'),
                              'entries': entries, 'observed': sorted(seen)}
                for t in entries:
                    xrefs[t].add(pc); funcs.add(t); queue.append(t)
            else:
                unresolved.append(pc)
            for t in seen:                       # observed targets are certain
                if in_rom(t):
                    xrefs[t].add(pc); funcs.add(t); queue.append(t)
            if f == M.IND_JUMP:
                return
        pc += ins.length

def main():
    os.makedirs(OUTD, exist_ok=True)
    queue = deque()

    # --- entry points: the vectors
    entries = {}
    for i, nm in enumerate(['RSVD', 'SWI3', 'SWI2', 'FIRQ', 'IRQ', 'SWI', 'NMI', 'RESET']):
        a = 0xFFF0 + i * 2
        v = (mem[a] << 8) | mem[a + 1]
        entries[nm] = v
        if in_rom(v):
            queue.append(v); funcs.add(v)

    # --- trace seeds: every executed PC is certainly code
    seeds = [a for a in range(ROM_LO, ROM_HI) if cov[a]]
    queue.extend(seeds)

    # --- addresses found the hard way.
    # Reachability is undecidable and this game dispatches through pointers the
    # caller supplies, so some entry points cannot be proved from the ROM alone
    # and are never executed during the attract loop either. When the runtime
    # traps on an address that was never recompiled, put it here and re-run.
    # Each line: hex address, then why it is known to be code.
    extra = os.path.join(OUTD, 'extra_entries.txt')
    n_extra = 0
    if os.path.exists(extra):
        for line in open(extra):
            line = line.split('#')[0].strip()
            if not line:
                continue
            a = int(line.split()[0], 16)
            if in_rom(a):
                queue.append(a); funcs.add(a); n_extra += 1
        print(f'extra entry points  : {n_extra} from {extra}')

    while queue:
        walk(queue.popleft(), queue)

    # ---- pass 2: with the whole flow graph in hand, redo EVERY indirect site
    #      - resolved ones included, because a site can be reached with more
    #      than one table and pass 1 only ever recorded the first base it saw.
    #      Two rounds: adding a base moves where its neighbours are cut off.
    again = deque()
    for _ in range(2):
        KNOWN_BASES.update(b for t in tables.values() for b in t['bases'])
        sites = [(pc, (insns[pc].idx or {})) for pc in list(tables) + list(unresolved)
                 if insns.get(pc) is not None]
        for pc, idx in sites:
            if idx.get('kind') == 'extind':      # vector in RAM: not knowable
                continue
            reg = idx.get('reg')
            bases = backscan_bases(pc, reg)
            if not bases:
                continue
            KNOWN_BASES.update(bases)
            ents = sorted({e for b in bases for e in table_entries(b)})
            tables[pc] = {'base': min(bases), 'bases': sorted(bases), 'reg': reg,
                          'entries': ents, 'observed': sorted(obs_ind.get(pc, []))}
            if pc in unresolved:
                unresolved.remove(pc)
            for e in ents:
                if e not in funcs:
                    xrefs[e].add(pc); funcs.add(e); again.append(e)
        while again:
            walk(again.popleft(), again)

    # ---- quality check: overlapping instructions
    overlaps = []
    for pc, ins in insns.items():
        for a in range(pc + 1, pc + ins.length):
            if starts[a & 0xFFFF]:
                overlaps.append((pc, a))

    # ---------------------------------------------------------------- report
    n_pad = sum(padding[ROM_LO:ROM_HI])
    n_code   = sum(code[ROM_LO:ROM_HI])
    n_exec   = sum(cov[ROM_LO:ROM_HI])
    n_starts = sum(starts[ROM_LO:ROM_HI])
    total    = ROM_HI - ROM_LO

    print(f'=== Program ROM coverage ({total} bytes, ${ROM_LO:04X}-${ROM_HI-1:04X}) ===')
    print(f'  instructions found            : {n_starts:,}')
    print(f'  bytes classified as CODE      : {n_code:,}  ({100*n_code/total:.1f}%)')
    print(f'  padding bytes ($00/$FF)       : {n_pad:,}  ({100*n_pad/total:.1f}%)')
    print(f'  remaining bytes (DATA)        : {total-n_code-n_pad:,}  '
          f'({100*(total-n_code-n_pad)/total:.1f}%)')
    print(f'  (instructions seen in trace   : {n_exec:,})')
    print(f'  OVERLAPPING instructions      : {len(overlaps)}   '
          f'{"(analysis is consistent)" if not overlaps else "<-- investigate"}')
    for a, b in overlaps[:10]:
        print(f'      ${a:04X} contains another instruction start at ${b:04X}')
    print()
    print(f'=== Indirect jumps ===')
    print(f'  total sites         : {len(tables) + len(unresolved)}')
    print(f'  statically resolved : {len(tables)}')
    print(f'  UNRESOLVED          : {len(unresolved)}  ' +
          ' '.join(f'${a:04X}' for a in unresolved))
    print()
    for pc, t in sorted(tables.items()):
        obs = t['observed']
        extra = [e for e in t['entries'] if e not in obs]
        print(f'  ${pc:04X}  base=${t["base"]:04X} ({t["reg"]})  '
              f'{len(obs)} observed, {len(t["entries"])} read statically')
    print()
    print(f'=== Functions ===')
    print(f'  JSR/BSR/table targets : {len(funcs)}')

    # largest contiguous non-code runs
    runs, s = [], None
    for a in range(ROM_LO, ROM_HI):
        if not code[a]:
            if s is None:
                s = a
        else:
            if s is not None:
                runs.append((s, a - 1)); s = None
    if s is not None:
        runs.append((s, ROM_HI - 1))
    runs.sort(key=lambda r: r[1] - r[0], reverse=True)
    print()
    print('=== 15 largest non-code runs (tables / graphics / level data) ===')
    for a, b in runs[:15]:
        print(f'  ${a:04X}-${b:04X}  {b-a+1:5d} byte')

    json.dump({'entries': {k: f'{v:04X}' for k, v in entries.items()},
               'n_insns': n_starts, 'n_code_bytes': n_code,
               'functions': sorted(f'{f:04X}' for f in funcs),
               'tables': {f'{k:04X}': {'base': f'{v["base"]:04X}',
                                       'bases': [f'{b:04X}' for b in v['bases']],
                                       'reg': v['reg'],
                                       'entries': [f'{e:04X}' for e in v['entries']],
                                       'observed': [f'{e:04X}' for e in v['observed']]}
                          for k, v in tables.items()},
               'unresolved': [f'{a:04X}' for a in unresolved],
               'data_runs': [[f'{a:04X}', f'{b:04X}', b - a + 1] for a, b in sorted(runs)]},
              open(os.path.join(OUTD, 'static.json'), 'w'), indent=1)
    with open(os.path.join(OUTD, 'code.bin'), 'wb') as f:
        f.write(code)
    print('\nwrote analysis/static.json and analysis/code.bin')

if __name__ == '__main__':
    main()
