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
                tables[pc] = {'base': base, 'reg': (ins.idx or {}).get('reg'),
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

    while queue:
        walk(queue.popleft(), queue)

    # ---- pass 2: the bases are known now, so re-read the tables with correct
    #      bounds and retry the still-unresolved indirect jumps using the
    #      backward scan.
    KNOWN_BASES.update(t['base'] for t in tables.values())
    for pc, t in list(tables.items()):
        t['entries'] = table_entries(t['base'])
    again = deque()
    for pc in list(unresolved):
        ins = insns.get(pc)
        if ins is None:
            continue
        idx = ins.idx or {}
        if idx.get('kind') == 'extind':
            continue
        reg = idx.get('reg')
        base = backscan(pc, reg)
        if base is not None and in_rom(base):
            KNOWN_BASES.add(base)
            tables[pc] = {'base': base, 'reg': reg, 'entries': table_entries(base),
                          'observed': sorted(obs_ind.get(pc, []))}
            unresolved.remove(pc)
            for e in tables[pc]['entries']:
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
               'tables': {f'{k:04X}': {'base': f'{v["base"]:04X}', 'reg': v['reg'],
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
