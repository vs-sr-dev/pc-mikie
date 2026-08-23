"""Compare the recompiled code's trace against MAME's.

MAME  (trace ... ,noloop,{tracelog "A=.. B=.. ..."}):
    A=00 B=00 X=5FF0 Y=0000 U=0000 S=0000 DP=00 CC=50 CYC=14
    CD98: LDA    #$55
Ours (build/mikie_trace.exe):
    A=00 B=00 X=5FF0 Y=0000 U=0000 S=0000 DP=00 CC=50 CYC=14
    CD98

Reports the first divergence with context: precisely the instruction where the
recompiled code parts company with the hardware.
"""
import re, sys, os

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
REG  = re.compile(r'^A=([0-9A-F]{2}) B=([0-9A-F]{2}) X=([0-9A-F]{4}) Y=([0-9A-F]{4}) '
                  r'U=([0-9A-F]{4}) S=([0-9A-F]{4}) DP=([0-9A-F]{2}) CC=([0-9A-F]{2})'
                  r'(?: CYC=(\d+))?$')
PCM  = re.compile(r'^([0-9A-Fa-f]{4}):\s*(.*)$')
PCO  = re.compile(r'^([0-9A-Fa-f]{4})$')

def steps(path, pcre):
    """yield (pc, regs_tuple, text) for every instruction"""
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

def main(mame_log, our_log, maxsteps=10**9):
    a = steps(mame_log, PCM)
    b = steps(our_log,  PCO)
    hist = []
    n = 0

    # Alignment: the MAME trace may start mid-run (to capture only an
    # interesting window). The cycle counter is absolute, so MAME's first
    # state identifies a unique point in our own trace.
    try:
        pa0, ra0, dis0 = next(a)
    except StopIteration:
        print('MAME trace is empty'); return 0
    skipped = 0
    while True:
        try: pb, rb, _ = next(b)
        except StopIteration:
            print(f'ALIGNMENT FAILED: MAME initial state '
                  f'(PC={pa0:04X} CYC={ra0[8]}) does not appear in our trace')
            return 2
        if pb == pa0 and rb == ra0:
            break
        skipped += 1
    if skipped:
        print(f'aligned by skipping {skipped:,} of our own instructions '
              f'(MAME starts at PC={pa0:04X} CYC={ra0[8]})')
    hist.append((pa0, ra0, dis0)); n = 1
    while n < maxsteps:
        try:    pa, ra, dis = next(a)
        except StopIteration: print(f'MAME trace exhausted after {n:,} instructions - NO DIVERGENCE'); return 0
        try:    pb, rb, _   = next(b)
        except StopIteration: print(f'our trace exhausted after {n:,} instructions - NO DIVERGENCE'); return 0
        n += 1
        hist.append((pa, ra, dis))
        if len(hist) > 8: hist.pop(0)
        if pa != pb or ra != rb:
            print(f'DIVERGENCE at instruction #{n:,}\n')
            print('  last instructions in agreement (MAME):')
            for p, r, d in hist[:-1]:
                print(f'    {p:04X}: {d:<22s} A={r[0]} B={r[1]} X={r[2]} Y={r[3]} '
                      f'U={r[4]} S={r[5]} DP={r[6]} CC={r[7]} CYC={r[8]}')
            print()
            print(f'  MAME   : PC={pa:04X} {dis:<22s} A={ra[0]} B={ra[1]} X={ra[2]} Y={ra[3]} '
                  f'U={ra[4]} S={ra[5]} DP={ra[6]} CC={ra[7]} CYC={ra[8]}')
            print(f'  ours   : PC={pb:04X} {"":<22s} A={rb[0]} B={rb[1]} X={rb[2]} Y={rb[3]} '
                  f'U={rb[4]} S={rb[5]} DP={rb[6]} CC={rb[7]} CYC={rb[8]}')
            diff = [k for k, (x, y) in
                    zip('A B X Y U S DP CC CYC'.split(), zip(ra, rb)) if x != y]
            print(f'\n  PC differs: {"yes" if pa != pb else "no"};  '
                  f'registers differing: {", ".join(diff) if diff else "none"}')
            return 1
    print(f'{n:,} instructions compared, no divergence')
    return 0

if __name__ == '__main__':
    mame = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, 'trace', 'maincpu.log')
    ours = sys.argv[2] if len(sys.argv) > 2 else os.path.join(ROOT, 'trace', 'ours.log')
    sys.exit(main(mame, ours, int(sys.argv[3]) if len(sys.argv) > 3 else 10**9))
