"""Extract from a MAME coverage trace:
  - the set of PCs actually executed (a coverage bitmap)
  - the observed destinations of INDIRECT jumps and calls (JSR [..], JMP [..])
  - the observed call graph (JSR/BSR -> target)
Everything lands in trace/*.json and trace/coverage.bin, after which the
multi-hundred-megabyte .log can be deleted.
"""
import os, re, sys, json
from collections import Counter, defaultdict

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
LOG  = os.path.join(ROOT, 'trace', 'cov.log')
OUT  = os.path.join(ROOT, 'trace')

LINE = re.compile(r'^([0-9A-Fa-f]{4}): (\S+)\s*(.*)$')

def main(path=LOG):
    cov = bytearray(0x10000)
    indirect = defaultdict(Counter)     # indirect-jump pc -> Counter(target)
    calls    = defaultdict(Counter)     # JSR/BSR pc       -> Counter(target)
    opcount  = Counter()
    prev = None                          # (pc, mnem, operand text)
    n = 0

    with open(path, 'r', errors='replace') as f:
        for line in f:
            m = LINE.match(line)
            if not m:
                continue
            pc = int(m.group(1), 16)
            mnem, oper = m.group(2).upper(), m.group(3).strip()
            cov[pc] = 1
            opcount[mnem] += 1
            n += 1
            if prev is not None:
                ppc, pmn, pop = prev
                # was the previous instruction an indirect jump? then the current pc is its resolved target
                if pop.startswith('[') and pmn in ('JSR', 'JMP'):
                    indirect[ppc][pc] += 1
                elif pmn in ('JSR', 'BSR', 'LBSR'):
                    calls[ppc][pc] += 1
            prev = (pc, mnem, oper)

    with open(os.path.join(OUT, 'coverage.bin'), 'wb') as f:
        f.write(cov)

    def dump(name, d):
        j = {f'{k:04X}': {f'{t:04X}': c for t, c in v.most_common()} for k, v in sorted(d.items())}
        with open(os.path.join(OUT, name), 'w') as f:
            json.dump(j, f, indent=1)
        return j

    ind = dump('indirect_targets.json', indirect)
    dump('calls.json', calls)
    with open(os.path.join(OUT, 'opcount.json'), 'w') as f:
        json.dump(dict(opcount.most_common()), f, indent=1)

    # ---- report
    exec_bytes = sum(cov[0x6000:0x10000])
    print(f'instructions traced  : {n:,}')
    print(f'distinct PCs executed: {exec_bytes:,} of 40960 ROM bytes '
          f'({100*exec_bytes/40960:.1f}% of bytes are observed instruction starts)')
    print()
    print(f'INDIRECT jumps observed: {len(ind)}')
    for pc, tg in list(ind.items()):
        tl = ' '.join(f'{t}({c})' for t, c in list(tg.items())[:12])
        print(f'  {pc}: {len(tg)} targets -> {tl}{" ..." if len(tg) > 12 else ""}')
    print()
    print('most frequent opcodes:')
    for mn, c in opcount.most_common(20):
        print(f'  {mn:6s} {c:>10,}')

if __name__ == '__main__':
    main(*sys.argv[1:])
