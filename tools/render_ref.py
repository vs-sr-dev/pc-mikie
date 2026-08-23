"""Render a frame in Python from a dump of $2800-$3FFF, and compare it against
a MAME snapshot.

This exists to split one question into two: if the Python render of MAME's own
RAM matches MAME's PNG, the rendering rules are right and any difference in the
C build comes from the data it was given (i.e. the CPU/IO side). If it does not
match, the rules themselves are wrong.

The palette bank is driver state rather than memory, so all eight are tried and
the best match is reported.
"""
import os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from PIL import Image
import gfxdecode as G

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SCR_W, VIS_TOP, VIS_H = 256, 16, 224
OUT_W, OUT_H = VIS_H, SCR_W


def build():
    indirect, lut_char, lut_spr = G.build_palette()
    tiles_rom = G.rd('o11.8i')
    tiles = G.decode_gfx(tiles_rom, 0, 8, 8, 4, [0, 1, 2, 3],
                         [i * 4 for i in range(8)], [i * 32 for i in range(8)],
                         8 * 8 * 4, 512)
    spr_rom = G.rd('001.f1') + G.rd('003.f3') + G.rd('005.h1') + G.rd('007.h3')
    HALF = 256 * 128 * 8
    xoff = [32*8+0, 32*8+1, 32*8+2, 32*8+3, 16*8+0, 16*8+1, 16*8+2, 16*8+3,
            0, 1, 2, 3, 48*8+0, 48*8+1, 48*8+2, 48*8+3]
    yoff = [i * 16 for i in range(8)] + [(32 + i) * 16 for i in range(8)]
    banks = [G.decode_gfx(spr_rom, b, 16, 16, 4, [0, 4, HALF, HALF + 4],
                          xoff, yoff, 128 * 8, 256) for b in (0, 1)]
    return indirect, lut_char, lut_spr, tiles, banks


def render(ram, pbank, flip, gfx):
    indirect, lut_char, lut_spr, tiles, banks = gfx
    spriteram = ram[0x0000:0x0090]
    colorram  = ram[0x1000:0x1400]
    videoram  = ram[0x1400:0x1800]

    fb = [[(0, 0, 0)] * SCR_W for _ in range(256)]

    def tile_col(cset, pix):
        return indirect[(pbank << 5) | 0x10 | (lut_char[(cset * 16 + pix) & 0xFF] & 0x0F)]

    def spr_col(cset, pix):
        return indirect[(pbank << 5) | (lut_spr[(cset * 16 + pix) & 0xFF] & 0x0F)]

    def draw_tiles(cat):
        for i in range(32 * 32):
            attr = colorram[i]
            if ((attr & 0x10) and 1 or 0) != cat:
                continue
            code = videoram[i] + ((attr & 0x20) << 3)
            px = tiles[code & 511]
            fx, fy = attr & 0x40, attr & 0x80
            ox, oy = (i % 32) * 8, (i // 32) * 8
            for y in range(8):
                for x in range(8):
                    v = px[(7 - y if fy else y) * 8 + (7 - x if fx else x)]
                    fb[oy + y][ox + x] = tile_col(attr & 0x0F, v)

    draw_tiles(0)
    for offs in range(0, 0x90, 4):
        b0, b1, b2, b3 = spriteram[offs:offs + 4]
        bank = 1 if (b2 & 0x40) else 0
        code = (b2 & 0x3F) + ((b2 & 0x80) >> 1) + ((b0 & 0x40) << 1)
        cset, sx = b0 & 0x0F, b3
        sy = 244 - b1
        fx, fy = (~b0) & 0x10, b0 & 0x20
        if flip:
            sy = 242 - sy
            fy = not fy
        px = banks[bank][code & 255]
        for y in range(16):
            py = sy + y
            if not (0 <= py < 256):
                continue
            for x in range(16):
                pxx = sx + x
                if not (0 <= pxx < SCR_W):
                    continue
                v = px[(15 - y if fy else y) * 16 + (15 - x if fx else x)]
                if v == 0:
                    continue
                fb[py][pxx] = spr_col(cset, v)
    draw_tiles(1)

    img = Image.new('RGB', (OUT_W, OUT_H))
    p = img.load()
    for y in range(OUT_H):
        for x in range(OUT_W):
            p[x, y] = fb[VIS_TOP + x][(OUT_H - 1) - y]
    return img


def main(ram_path, png_path):
    G.INDIRECT, G.LUT_CHAR, G.LUT_SPR = G.build_palette()
    gfx = build()
    ram = open(ram_path, 'rb').read()
    ref = Image.open(png_path).convert('RGB')
    best = None
    for pbank in range(8):
        img = render(ram, pbank, 0, gfx)
        bad = sum(1 for y in range(OUT_H) for x in range(OUT_W)
                  if img.getpixel((x, y)) != ref.getpixel((x, y)))
        print(f'  palette bank {pbank}: {bad:6d} differing pixels '
              f'({100.0*bad/(OUT_W*OUT_H):.2f}%)')
        if best is None or bad < best[0]:
            best = (bad, pbank, img)
    bad, pbank, img = best
    img.save('trace/render_ref.png')
    print(f'\nbest: palette bank {pbank}, {bad} differing pixels')
    print('wrote trace/render_ref.png')


if __name__ == '__main__':
    main(*sys.argv[1:])
