#include "sn76489.h"

#define FEEDBACK  0x10000u
#define TAP1      0x04u
#define TAP2      0x08u

/* 2 dB per step from a quarter of full scale, 15 = silence. Built once. */
static int16_t vol_table[16];
static int     vol_ready;

static void build_vol(void)
{
    int i;
    double out = 32767.0 / 4.0;
    for (i = 0; i < 15; i++) {
        vol_table[i] = (int16_t)out;
        out /= 1.258925412;                 /* 10 ^ (2/20) */
    }
    vol_table[15] = 0;
    vol_ready = 1;
}

void sn76489_reset(sn76489_t *s)
{
    int i;
    if (!vol_ready) build_vol();
    for (i = 0; i < 8; i++) s->reg[i] = 0;  /* volume 0 = LOUDEST at power-up */
    s->last_reg = 0;
    for (i = 0; i < 4; i++) {
        s->output[i] = 0;
        s->count[i]  = 0;
        s->period[i] = (i == 3) ? 0 : 0x400;
        s->volume[i] = vol_table[s->reg[i * 2 + 1] & 0x0F];
    }
    s->rng = FEEDBACK;
    s->output[3] = (uint8_t)(s->rng & 1);
}

void sn76489_write(sn76489_t *s, uint8_t data)
{
    int r, c;

    if (data & 0x80) {
        r = (data & 0x70) >> 4;
        s->last_reg = (uint8_t)r;
        s->reg[r] = (uint16_t)((s->reg[r] & 0x3F0) | (data & 0x0F));
    } else {
        r = s->last_reg;
    }
    c = r >> 1;

    switch (r) {
    case 0: case 2: case 4:                 /* tone period */
        if (!(data & 0x80))
            s->reg[r] = (uint16_t)((s->reg[r] & 0x0F) | ((data & 0x3F) << 4));
        s->period[c] = s->reg[r] ? s->reg[r] : 0x400;
        if (r == 4 && (s->reg[6] & 3) == 3)
            s->period[3] = (uint16_t)(s->period[2] << 1);
        break;
    case 1: case 3: case 5: case 7:         /* attenuation */
        s->volume[c] = vol_table[data & 0x0F];
        if (!(data & 0x80))
            s->reg[r] = (uint16_t)((s->reg[r] & 0x3F0) | (data & 0x0F));
        break;
    default: {                              /* noise control */
        int n;
        if (!(data & 0x80))
            s->reg[r] = (uint16_t)((s->reg[r] & 0x3F0) | (data & 0x0F));
        n = s->reg[6];
        s->period[3] = (uint16_t)(((n & 3) == 3) ? (s->period[2] << 1) : (1 << (5 + (n & 3))));
        s->rng = FEEDBACK;
        break;
    }
    }
}

void sn76489_step(sn76489_t *s)
{
    int i;
    for (i = 0; i < 3; i++) {
        if (--s->count[i] <= 0) {
            s->output[i] ^= 1;
            s->count[i] = s->period[i];
        }
    }
    if (--s->count[3] <= 0) {
        int t1 = (s->rng & TAP1) != 0;
        int t2 = ((s->rng & TAP2) != 0) && ((s->reg[6] & 4) != 0);
        s->rng >>= 1;
        if (t1 != t2) s->rng |= FEEDBACK;
        s->output[3] = (uint8_t)(s->rng & 1);
        s->count[3] = s->period[3];
    }
}

int16_t sn76489_out(const sn76489_t *s)
{
    int i, out = 0;
    for (i = 0; i < 4; i++) if (s->output[i]) out += s->volume[i];
    return (int16_t)out;
}
