"""Verify m6809.py against MAME's own unidasm disassembly.
Checks address, length (raw bytes) and mnemonic for every instruction.
"""
import os, re, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import m6809

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
mem = bytearray(open(os.path.join(ROOT, 'rom', 'maincpu.bin'), 'rb').read())

LINE = re.compile(r'^([0-9a-f]{4}): ((?:[0-9a-f]{2} )+)\s*(\S+)')

# accepted aliases (unidasm spells some mnemonics differently)
ALIAS = {
    'ASL': 'LSL', 'ASLA': 'LSLA', 'ASLB': 'LSLB',
    'BCC': 'BHS', 'BCS': 'BLO', 'LBCC': 'LBHS', 'LBCS': 'LBLO',
}

bad_len = bad_mn = total = 0
errors = []
for line in open(os.path.join(ROOT, 'disasm', 'maincpu_raw.asm')):
    m = LINE.match(line)
    if not m:
        continue
    pc = int(m.group(1), 16)
    raw = bytes(int(b, 16) for b in m.group(2).split())
    mn = m.group(3).upper()
    total += 1
    ins = m6809.decode(mem, pc)
    if ins.length != len(raw):
        bad_len += 1
        if len(errors) < 25:
            errors.append(f'LEN  {pc:04X}: unidasm={len(raw)} ours={ins.length} '
                          f'({mn} vs {ins.mnem}) raw={raw.hex()}')
        continue
    a, b = ins.mnem.upper(), mn
    if a != b and ALIAS.get(a) != b and ALIAS.get(b) != a:
        bad_mn += 1
        if len(errors) < 25:
            errors.append(f'MNEM {pc:04X}: unidasm={b} ours={a} raw={raw.hex()}')

print(f'instructions compared : {total}')
print(f'wrong lengths         : {bad_len}')
print(f'wrong mnemonics       : {bad_mn}')
if errors:
    print('\nfirst errors:')
    for e in errors:
        print('  ' + e)
else:
    print('\nDECODER OK - exact agreement with unidasm.')
