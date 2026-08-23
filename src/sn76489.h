/* SN76489A - the PSG used twice on the Mikie sound board.
 *
 * Register model and noise LFSR follow MAME's sn76496 device with the
 * SN76489A parameters: 15-bit shift register, taps on bits 2 and 3, output
 * not inverted, and a /8 divider on the clock input (so the tone counters run
 * at clock/16, since the chip is clocked at half the input rate internally).
 *
 * The two chips on the board run at DIFFERENT clocks - 1.789772 MHz for sn1
 * and 3.579545 MHz for sn2 - which is why the step period is per instance.
 */
#ifndef SN76489_H
#define SN76489_H
#include <stdint.h>

typedef struct {
    uint16_t reg[8];
    uint16_t period[4];
    int32_t  count[4];
    uint8_t  output[4];
    int16_t  volume[4];
    uint8_t  last_reg;
    uint32_t rng;
} sn76489_t;

void    sn76489_reset(sn76489_t *s);
void    sn76489_write(sn76489_t *s, uint8_t data);
void    sn76489_step(sn76489_t *s);      /* one clock/16 tick */
int16_t sn76489_out(const sn76489_t *s);

#endif
