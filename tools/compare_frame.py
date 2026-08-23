"""Compare a frame rendered by the recompiled game against a MAME snapshot.

Ours arrives as a raw RGB dump (224*256*3); MAME's as a PNG. Reports the
fraction of differing pixels and writes a side-by-side plus a difference mask.
"""
import sys, os
from PIL import Image, ImageChops

W, H = 224, 256

def main(raw_path, png_path, out_prefix='trace/frame'):
    data = open(raw_path, 'rb').read()
    if len(data) != W * H * 3:
        print(f'{raw_path}: expected {W*H*3} bytes, got {len(data)}')
        return 2
    ours = Image.frombytes('RGB', (W, H), data)
    mame = Image.open(png_path).convert('RGB')
    if mame.size != (W, H):
        print(f'{png_path}: size {mame.size}, expected {(W, H)}')
        return 2

    diff = ImageChops.difference(ours, mame)
    bbox = diff.getbbox()
    px = diff.load()
    bad = sum(1 for y in range(H) for x in range(W) if px[x, y] != (0, 0, 0))
    total = W * H
    print(f'differing pixels : {bad:,} / {total:,}  ({100.0*bad/total:.2f}%)')
    print(f'difference bbox  : {bbox}')

    side = Image.new('RGB', (W * 3 + 16, H), (24, 24, 24))
    side.paste(ours, (0, 0))
    side.paste(mame, (W + 8, 0))
    side.paste(diff.point(lambda v: 255 if v else 0), (W * 2 + 16, 0))
    side.save(out_prefix + '_compare.png')
    print(f'wrote {out_prefix}_compare.png  (ours | MAME | difference)')
    return 0 if bad == 0 else 1

if __name__ == '__main__':
    sys.exit(main(*sys.argv[1:]))
