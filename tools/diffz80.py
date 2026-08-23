"""Compare our Z80 core against MAME's, instruction by instruction.

Same shape as tools/difftrace.py, for the sound CPU:

MAME  (tools/trace_z80.lua):
    AF=0040 BC=0000 ... R=00 IM=0 IFF1=0 HALT=0 CYC=0
    0000: ld   b,$00
Ours (build/z80_trace.exe):
    AF=0040 BC=0000 ... R=00 IM=0 IFF1=0 HALT=0 CYC=0
    0000

Interrupts need no special handling: MAME logs the interrupt sequence as part
of the state of the instruction that follows it, and so do we.
"""
import re, sys, os

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FIELDS = 'AF BC DE HL IX IY SP AF2 BC2 DE2 HL2 I R IM IFF1 HALT CYC'.split()
REG = re.compile(r'^AF=([0-9A-F]{4}) BC=([0-9A-F]{4}) DE=([0-9A-F]{4}) HL=([0-9A-F]{4}) '
                 r'IX=([0-9A-F]{4}) IY=([0-9A-F]{4}) SP=([0-9A-F]{4}) '
                 r'AF2=([0-9A-F]{4}) BC2=([0-9A-F]{4}) DE2=([0-9A-F]{4}) HL2=([0-9A-F]{4}) '
                 r'I=([0-9A-F]{2}) R=([0-9A-F]{2}) IM=([0-9A-F]) IFF1=([0-9A-F]) '
                 r'HALT=([0-9A-F]) CYC=(\d+)$')
PCM = re.compile(r'^([0-9A-Fa-f]{4}):\s*(.*)$')
PCO = re.compile(r'^([0-9A-Fa-f]{4})$')


def steps(path, pcre):
    regs = None
    with open(path, 'r', errors='replace') as f:
        for line in f:
            line = line.rstrip('\n')
            m = REG.match(line)
            if m:
                regs = m.groups()
                continue
            m = pcre.match(line)
            if m and regs is not None:
                yield int(m.group(1), 16), regs, (m.group(2).strip() if pcre is PCM else '')
                regs = None


def show(pc, r, dis=''):
    return (f'PC={pc:04X} {dis:<20s} AF={r[0]} BC={r[1]} DE={r[2]} HL={r[3]} '
            f'IX={r[4]} IY={r[5]} SP={r[6]} R={r[12]} IFF1={r[14]} CYC={r[16]}')


def main(mame_log, our_log, maxsteps=10 ** 9):
    a, b = steps(mame_log, PCM), steps(our_log, PCO)
    hist, n = [], 0

    # The MAME trace can start mid-run; its first state is unique thanks to the
    # absolute cycle counter, so align on it.
    try:
        pa0, ra0, dis0 = next(a)
    except StopIteration:
        print('MAME trace is empty')
        return 0
    skipped = 0
    while True:
        try:
            pb, rb, _ = next(b)
        except StopIteration:
            print(f'ALIGNMENT FAILED: MAME initial state (PC={pa0:04X} CYC={ra0[16]}) '
                  f'never appears in our trace')
            return 2
        if pb == pa0 and rb == ra0:
            break
        skipped += 1
    if skipped:
        print(f'aligned by skipping {skipped:,} of our own instructions')
    hist.append((pa0, ra0, dis0))
    n = 1

    while n < maxsteps:
        try:
            pa, ra, dis = next(a)
        except StopIteration:
            print(f'MAME trace exhausted after {n:,} instructions - NO DIVERGENCE')
            return 0
        try:
            pb, rb, _ = next(b)
        except StopIteration:
            print(f'our trace exhausted after {n:,} instructions - NO DIVERGENCE')
            return 0
        n += 1
        hist.append((pa, ra, dis))
        if len(hist) > 8:
            hist.pop(0)
        if pa != pb or ra != rb:
            print(f'DIVERGENCE at instruction #{n:,}\n')
            print('  last instructions in agreement (MAME):')
            for p, r, d in hist[:-1]:
                print('    ' + show(p, r, d))
            print()
            print('  MAME   : ' + show(pa, ra, dis))
            print('  ours   : ' + show(pb, rb))
            diff = [k for k, (x, y) in zip(FIELDS, zip(ra, rb)) if x != y]
            print(f'\n  PC differs: {"yes" if pa != pb else "no"};  '
                  f'fields differing: {", ".join(diff) if diff else "none"}')
            return 1
    print(f'{n:,} instructions compared, no divergence')
    return 0


if __name__ == '__main__':
    mame = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, 'trace', 'audiocpu.log')
    ours = sys.argv[2] if len(sys.argv) > 2 else os.path.join(ROOT, 'trace', 'z80_ours.log')
    sys.exit(main(mame, ours, int(sys.argv[3]) if len(sys.argv) > 3 else 10 ** 9))
