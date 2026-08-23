"""Verify m6809.py's cycle table against MAME's `totalcycles` counter, and
derive the exact vblank phase.

trace/cyc.log looks like:
    CYC=4
    CD91: CLRA
The delta between two consecutive CYC values is the cost of the preceding
instruction, except where the 6809 took an interrupt in between.
"""
import os, re, sys
from collections import Counter, defaultdict

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
import m6809 as M

mem = bytearray(open(os.path.join(ROOT, 'rom', 'maincpu.bin'), 'rb').read())
LOG = os.path.join(ROOT, 'trace', 'cyc.log')

CYC = re.compile(r'^CYC=(\d+)$')
PCL = re.compile(r'^([0-9A-Fa-f]{4}):')

IRQVEC = (mem[0xFFF8] << 8) | mem[0xFFF9]

def main():
    prev_pc = prev_cyc = None
    prev_ins = None
    ok = Counter()
    bad = defaultdict(Counter)     # (mnem, mode, idxkind, ind) -> Counter(delta)
    irq_cycles = []
    n = 0
    cyc = None
    with open(LOG, 'r', errors='replace') as f:
        for line in f:
            m = CYC.match(line)
            if m:
                cyc = int(m.group(1)); continue
            m = PCL.match(line)
            if not m or cyc is None:
                continue
            pc = int(m.group(1), 16)
            if prev_pc is not None:
                delta = cyc - prev_cyc
                n += 1
                # for long branches the cost depends on whether it was taken
                if prev_ins.mode == M.REL16:
                    prev_ins.exp_cyc = M.cycles(prev_ins, taken=(pc == prev_ins.target))
                # interrupt entry: the 6809 stacks 12 bytes and vectors away
                if pc == IRQVEC and prev_ins.flow not in (M.JUMP, M.IND_JUMP) \
                        and delta > (prev_ins.exp_cyc + 5):
                    irq_cycles.append((cyc, delta - prev_ins.exp_cyc))
                else:
                    key = (prev_ins.mnem, prev_ins.mode,
                           (prev_ins.idx or {}).get('kind'),
                           bool((prev_ins.idx or {}).get('ind')))
                    if delta == prev_ins.exp_cyc:
                        ok[key] += 1
                    else:
                        bad[key][delta - prev_ins.exp_cyc] += 1
            ins = M.decode(mem, pc)
            ins.exp_cyc = M.cycles(ins)
            prev_pc, prev_cyc, prev_ins = pc, cyc, ins

    tot_ok = sum(ok.values()); tot_bad = sum(sum(c.values()) for c in bad.values())
    print(f'instructions compared  : {tot_ok + tot_bad:,}')
    print(f'  cycle counts correct : {tot_ok:,}  ({100*tot_ok/max(1,tot_ok+tot_bad):.3f}%)')
    print(f'  cycle counts wrong   : {tot_bad:,}')
    print(f'  distinct forms OK    : {len(ok)}')
    print(f'  distinct forms WRONG : {len(bad)}')
    if bad:
        print('\nforms with a wrong cost (delta = observed - predicted):')
        rows = sorted(bad.items(), key=lambda kv: -sum(kv[1].values()))
        for (mn, mode, k, ind), c in rows[:30]:
            tot = sum(c.values())
            det = ' '.join(f'{d:+d}x{v}' for d, v in c.most_common(4))
            kk = f' [{k}{"/ind" if ind else ""}]' if k else ''
            print(f'   {mn:6s} {mode:6s}{kk:16s} {tot:7d} times   {det}')
    print(f'\ninterrupt entries observed: {len(irq_cycles)}')
    if irq_cycles:
        print('  entry cost (cycles beyond the instruction):',
              Counter(d for _, d in irq_cycles).most_common(5))
        cs = [c for c, _ in irq_cycles]
        deltas = [b - a for a, b in zip(cs, cs[1:])]
        print(f'  first IRQ at cycle {cs[0]:,}')
        print('  spacing between consecutive IRQs:', Counter(deltas).most_common(6))

if __name__ == '__main__':
    main()
