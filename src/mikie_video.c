/* Video for Mikie (Konami GX469) - see mikie_video.h.
 *
 * The three things that are easy to get wrong, all handled here:
 *   - the palette is INDIRECT and two-stage: resistor-DAC PROMs give 256
 *     physical colours, then two lookup PROMs map (colour set, pixel) onto one
 *     of them, and a global 3-bit palette bank shifts the whole scene.
 *   - tiles with bit $10 of colour RAM are drawn AFTER the sprites.
 *   - sprite flipX is INVERTED, and the "gfx bank" is not a ROM bank but a
 *     second decode of the same data offset by one byte.
 */
#include "mikie_video.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* ------------------------------------------------------------ ROM images */
static uint8_t tile_rom[0x4000];
static uint8_t spr_rom [0x10000];
static uint8_t prom_r[0x100], prom_g[0x100], prom_b[0x100];
static uint8_t lut_char[0x100], lut_spr[0x100];

/* decoded graphics: one byte per pixel, holding the 4-bit colour index */
static uint8_t tile_px[512][8 * 8];
static uint8_t spr_px[2][256][16 * 16];

/* the 256 physical colours */
static uint8_t pal_rgb[256][3];

/* ------------------------------------------------------------- ROM loading */
static int load(const char *dir, const char *name, uint8_t *dst, size_t len,
                size_t off)
{
    char path[512];
    FILE *f;
    snprintf(path, sizeof path, "%s/%s", dir, name);
    f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "video: cannot open %s\n", path); return -1; }
    if (fread(dst + off, 1, len, f) != len) {
        fprintf(stderr, "video: %s is short\n", path); fclose(f); return -1;
    }
    fclose(f);
    return 0;
}

/* ------------------------------------------------------- generic gfx decode
 * A direct transcription of MAME's gfx_layout walk: every offset is in BITS,
 * plane 0 is the most significant bit of the resulting pixel value.
 */
static inline int getbit(const uint8_t *data, int bit)
{
    return (data[bit >> 3] >> (7 - (bit & 7))) & 1;
}

static void decode_gfx(const uint8_t *data, int base_byte, int w, int h,
                       int planes, const int *planeoff, const int *xoff,
                       const int *yoff, int charinc, int count, uint8_t *out)
{
    int c, x, y, p;
    int base_bit = base_byte * 8;
    for (c = 0; c < count; c++) {
        int cb = base_bit + c * charinc;
        uint8_t *dst = out + (size_t)c * w * h;
        for (y = 0; y < h; y++)
            for (x = 0; x < w; x++) {
                int pix = 0;
                for (p = 0; p < planes; p++)
                    pix = (pix << 1) | getbit(data, cb + planeoff[p] + yoff[y] + xoff[x]);
                dst[y * w + x] = (uint8_t)pix;
            }
    }
}

/* ------------------------------------------------------------------ palette
 * Resistor DAC: 2.2k / 1k / 470 / 220 ohm on bits 0..3. The weights are linear
 * in conductance, so bit i contributes 255 * (1/Ri) / sum(1/R).
 */
static void build_palette(void)
{
    static const double res[4] = { 2200.0, 1000.0, 470.0, 220.0 };
    double tot = 0.0;
    int i, b;
    uint8_t w[16];

    for (b = 0; b < 4; b++) tot += 1.0 / res[b];
    for (i = 0; i < 16; i++) {
        double s = 0.0;
        for (b = 0; b < 4; b++) if (i & (1 << b)) s += 1.0 / res[b];
        w[i] = (uint8_t)(255.0 * s / tot + 0.5);
    }
    for (i = 0; i < 256; i++) {
        pal_rgb[i][0] = w[prom_r[i] & 0x0F];
        pal_rgb[i][1] = w[prom_g[i] & 0x0F];
        pal_rgb[i][2] = w[prom_b[i] & 0x0F];
    }
}

/* Final physical colour for a tile / sprite pixel. */
static inline const uint8_t *tile_colour(int pbank, int cset, int pix)
{
    return pal_rgb[(pbank << 5) | 0x10 | (lut_char[((cset * 16) + pix) & 0xFF] & 0x0F)];
}
static inline const uint8_t *spr_colour(int pbank, int cset, int pix)
{
    return pal_rgb[(pbank << 5) | (lut_spr[((cset * 16) + pix) & 0xFF] & 0x0F)];
}

/* --------------------------------------------------------------------- init */
int video_init(const char *romdir)
{
    int i;
    int tile_plane[4] = { 0, 1, 2, 3 };
    int tile_x[8], tile_y[8];
    int spr_plane[4], spr_x[16], spr_y[16];
    static const int spr_x_tab[16] = {
        32*8+0, 32*8+1, 32*8+2, 32*8+3, 16*8+0, 16*8+1, 16*8+2, 16*8+3,
        0, 1, 2, 3, 48*8+0, 48*8+1, 48*8+2, 48*8+3
    };

    if (load(romdir, "o11.8i", tile_rom, 0x4000, 0)) return -1;
    if (load(romdir, "001.f1", spr_rom, 0x4000, 0x0000)) return -1;
    if (load(romdir, "003.f3", spr_rom, 0x4000, 0x4000)) return -1;
    if (load(romdir, "005.h1", spr_rom, 0x4000, 0x8000)) return -1;
    if (load(romdir, "007.h3", spr_rom, 0x4000, 0xC000)) return -1;
    if (load(romdir, "d19.1i",  prom_r,   0x100, 0)) return -1;
    if (load(romdir, "d21.3i",  prom_g,   0x100, 0)) return -1;
    if (load(romdir, "d20.2i",  prom_b,   0x100, 0)) return -1;
    if (load(romdir, "d22.12h", lut_char, 0x100, 0)) return -1;
    if (load(romdir, "d18.f9",  lut_spr,  0x100, 0)) return -1;

    build_palette();

    /* tiles: gfx_8x8x4_packed_msb - one nibble per pixel, high nibble first */
    for (i = 0; i < 8; i++) { tile_x[i] = i * 4; tile_y[i] = i * 32; }
    decode_gfx(tile_rom, 0, 8, 8, 4, tile_plane, tile_x, tile_y,
               8 * 8 * 4, 512, &tile_px[0][0]);

    /* sprites: planes 0/1 in the first half of the ROM, 2/3 in the second.
       Two decodes of the same data, one byte apart, are the "gfx banks". */
    spr_plane[0] = 0;
    spr_plane[1] = 4;
    spr_plane[2] = 256 * 128 * 8 + 0;
    spr_plane[3] = 256 * 128 * 8 + 4;
    for (i = 0; i < 16; i++) spr_x[i] = spr_x_tab[i];
    for (i = 0; i < 8; i++)  spr_y[i] = i * 16;
    for (i = 0; i < 8; i++)  spr_y[8 + i] = (32 + i) * 16;
    for (i = 0; i < 2; i++)
        decode_gfx(spr_rom, i, 16, 16, 4, spr_plane, spr_x, spr_y,
                   128 * 8, 256, &spr_px[i][0][0]);
    return 0;
}

/* ------------------------------------------------------------------ render */
/* Working buffer: the whole 256x256 raster, RGB. */
static uint8_t frame[SCR_W * 256 * 3];

static void put_tile(int col, int row, int code, int cset, int pbank,
                     int fx, int fy)
{
    const uint8_t *px = tile_px[code & 511];
    int x, y;
    for (y = 0; y < 8; y++) {
        int sy = row * 8 + y;
        if (sy < 0 || sy >= 256) continue;
        for (x = 0; x < 8; x++) {
            int sx = col * 8 + x;
            int v = px[(fy ? 7 - y : y) * 8 + (fx ? 7 - x : x)];
            const uint8_t *c = tile_colour(pbank, cset, v);
            uint8_t *d = frame + ((size_t)sy * SCR_W + sx) * 3;
            d[0] = c[0]; d[1] = c[1]; d[2] = c[2];
        }
    }
}

static void put_sprite(int bank, int code, int cset, int pbank,
                       int fx, int fy, int sx, int sy)
{
    const uint8_t *px = spr_px[bank][code & 255];
    int x, y;
    for (y = 0; y < 16; y++) {
        int py = sy + y;
        if (py < 0 || py >= 256) continue;
        for (x = 0; x < 16; x++) {
            int pxx = sx + x;
            int v;
            const uint8_t *c;
            uint8_t *d;
            if (pxx < 0 || pxx >= SCR_W) continue;
            v = px[(fy ? 15 - y : y) * 16 + (fx ? 15 - x : x)];
            if (v == 0) continue;                 /* pixel 0 is transparent */
            c = spr_colour(pbank, cset, v);
            d = frame + ((size_t)py * SCR_W + pxx) * 3;
            d[0] = c[0]; d[1] = c[1]; d[2] = c[2];
        }
    }
}

static void draw_tilemap(const uint8_t *colorram, const uint8_t *videoram,
                         int pbank, int category)
{
    int i;
    for (i = 0; i < 32 * 32; i++) {
        int attr = colorram[i];
        if (((attr & 0x10) ? 1 : 0) != category) continue;
        put_tile(i % 32, i / 32,
                 videoram[i] + ((attr & 0x20) << 3),
                 attr & 0x0F, pbank,
                 attr & 0x40, attr & 0x80);
    }
}

void video_render(const uint8_t *spriteram, const uint8_t *colorram,
                  const uint8_t *videoram, int palettebank, int flip,
                  uint8_t *rgb)
{
    int offs, x, y;

    memset(frame, 0, sizeof frame);
    draw_tilemap(colorram, videoram, palettebank, 0);

    /* 36 sprites of 4 bytes each: $2800-$288F */
    for (offs = 0; offs < 0x90; offs += 4) {
        int b0 = spriteram[offs + 0], b1 = spriteram[offs + 1];
        int b2 = spriteram[offs + 2], b3 = spriteram[offs + 3];
        int bank = (b2 & 0x40) ? 1 : 0;
        int code = (b2 & 0x3F) + ((b2 & 0x80) >> 1) + ((b0 & 0x40) << 1);
        int cset = b0 & 0x0F;
        int sx   = b3;
        int sy   = 244 - b1;
        int fx   = (~b0) & 0x10;          /* inverted, deliberately */
        int fy   = b0 & 0x20;
        if (flip) { sy = 242 - sy; fy = !fy; }
        put_sprite(bank, code, cset, palettebank, fx, fy, sx, sy);
    }

    draw_tilemap(colorram, videoram, palettebank, 1);

    /* Crop to the visible area (y 16..239) and apply ROT270.
       ROT270 = swap X/Y then flip Y, so dst(x,y) = src(255 - y, x). */
    for (y = 0; y < OUT_H; y++)
        for (x = 0; x < OUT_W; x++) {
            int src_x = (OUT_H - 1) - y;
            int src_y = 16 + x;
            const uint8_t *s = frame + ((size_t)src_y * SCR_W + src_x) * 3;
            uint8_t *d = rgb + ((size_t)y * OUT_W + x) * 3;
            d[0] = s[0]; d[1] = s[1]; d[2] = s[2];
        }
}
