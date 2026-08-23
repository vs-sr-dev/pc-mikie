"""Generic MAME gfx_layout decoder + Mikie palette/tile/sprite extraction.

Reproduces exactly:
  - mikie_state::palette()  (3 RGB PROMs + 2 lookup PROMs)
  - gfx_8x8x4_packed_msb    (tiles)
  - spritelayout            (16x16 sprites, planes split across two ROM halves)
"""
import os, sys
from PIL import Image

ROM = os.path.join(os.path.dirname(__file__), '..', 'rom')
OUT = os.path.join(os.path.dirname(__file__), '..', 'assets')

def rd(name):
    with open(os.path.join(ROM, name), 'rb') as f:
        return f.read()

# ---------------------------------------------------------------- gfx decode
def getbit(data, bit):
    return (data[bit >> 3] >> (7 - (bit & 7))) & 1

def decode_gfx(data, base_byte, width, height, planes, planeoffset,
               xoffset, yoffset, charinc, count):
    """Return a list of bytearrays (width*height) of colour indices 0..2^planes-1."""
    out = []
    base_bit = base_byte * 8
    for c in range(count):
        cb = base_bit + c * charinc
        buf = bytearray(width * height)
        for y in range(height):
            yo = yoffset[y]
            for x in range(width):
                xo = xoffset[x]
                pix = 0
                for p in range(planes):
                    pix = (pix << 1) | getbit(data, cb + planeoffset[p] + yo + xo)
                buf[y * width + x] = pix
        out.append(buf)
    return out

# ---------------------------------------------------------------- palette
def build_palette():
    prom_r, prom_g, prom_b = rd('d19.1i'), rd('d21.3i'), rd('d20.2i')
    lut_char, lut_spr = rd('d22.12h'), rd('d18.f9')

    res = [2200.0, 1000.0, 470.0, 220.0]     # bit0..bit3
    tot = sum(1.0 / r for r in res)
    def comp(v):
        s = sum((1.0 / res[b]) for b in range(4) if (v >> b) & 1)
        return int(round(255.0 * s / tot))

    # the 256 "indirect" colours (actual output of the resistor DAC)
    indirect = [(comp(prom_r[i] & 0xf), comp(prom_g[i] & 0xf), comp(prom_b[i] & 0xf))
                for i in range(256)]
    return indirect, lut_char, lut_spr

def char_ctab(pbank, colorlow, pix):
    """index into the indirect palette for a tile pixel"""
    return (pbank << 5) | 0x10 | (LUT_CHAR[(colorlow * 16 + pix) & 0xff] & 0x0f)

def spr_ctab(pbank, colorlow, pix):
    return (pbank << 5) | (LUT_SPR[(colorlow * 16 + pix) & 0xff] & 0x0f)

# ---------------------------------------------------------------- sheets
def sheet(chars, w, h, cols, path, colorfn, transparent=None, scale=3):
    rows = (len(chars) + cols - 1) // cols
    img = Image.new('RGBA', (cols * w, rows * h), (0, 0, 0, 0))
    px = img.load()
    for i, ch in enumerate(chars):
        ox, oy = (i % cols) * w, (i // cols) * h
        for y in range(h):
            for x in range(w):
                v = ch[y * w + x]
                if transparent is not None and v == transparent:
                    continue
                r, g, b = colorfn(v)
                px[ox + x, oy + y] = (r, g, b, 255)
    img = img.resize((img.width * scale, img.height * scale), Image.NEAREST)
    img.save(path)
    return img.size

if __name__ == '__main__':
    INDIRECT, LUT_CHAR, LUT_SPR = build_palette()
    os.makedirs(OUT, exist_ok=True)

    # ---- tiles: 512 x 8x8x4
    tiles_rom = rd('o11.8i')
    tiles = decode_gfx(tiles_rom, 0, 8, 8, 4,
                       [0, 1, 2, 3],
                       [i * 4 for i in range(8)],
                       [i * 32 for i in range(8)],
                       8 * 8 * 4, len(tiles_rom) * 8 // (8 * 8 * 4))
    print('tiles decodificate:', len(tiles))

    # ---- sprites: 256 x 16x16x4, two banks (byte offset 0 and 1)
    spr_rom = rd('001.f1') + rd('003.f3') + rd('005.h1') + rd('007.h3')
    HALF = 256 * 128 * 8            # 0x8000 expressed in bits
    xoff = [32*8+0, 32*8+1, 32*8+2, 32*8+3, 16*8+0, 16*8+1, 16*8+2, 16*8+3,
            0, 1, 2, 3, 48*8+0, 48*8+1, 48*8+2, 48*8+3]
    yoff = [i * 16 for i in range(8)] + [(32 + i) * 16 for i in range(8)]
    banks = []
    for bank_base in (0, 1):
        banks.append(decode_gfx(spr_rom, bank_base, 16, 16, 4,
                                [0, 4, HALF + 0, HALF + 4], xoff, yoff, 128 * 8, 256))
    print('sprite ROM:', len(spr_rom), 'bytes -> 2 banchi x 256 sprite')

    # palette bank 0 as the preview default
    PB = 0
    tsize = sheet(tiles, 8, 8, 32, os.path.join(OUT, 'tiles_pb0.png'),
                  lambda v: INDIRECT[char_ctab(PB, 0, v)])
    print('assets/tiles_pb0.png', tsize)
    for bi, b in enumerate(banks):
        s = sheet(b, 16, 16, 16, os.path.join(OUT, f'sprites_bank{bi+1}_pb0.png'),
                  lambda v: INDIRECT[spr_ctab(PB, 0, v)], transparent=0, scale=2)
        print(f'assets/sprites_bank{bi+1}_pb0.png', s)

    # dump the full palette: 8 banks x (16 char sets + 16 sprite sets)
    img = Image.new('RGB', (16 * 16, 8 * 2 * 16))
    p = img.load()
    for pb in range(8):
        for cl in range(16):
            for pix in range(16):
                for k in (0, 1):
                    idx = char_ctab(pb, cl, pix) if k == 0 else spr_ctab(pb, cl, pix)
                    p[cl * 16 + pix, (pb * 2 + k) * 16 + 8] = INDIRECT[idx]
    img.resize((img.width * 3, img.height * 3), Image.NEAREST).save(
        os.path.join(OUT, 'palette_map.png'))
    print('assets/palette_map.png')
