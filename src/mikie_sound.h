/* The Mikie sound board: Z80 + 2x SN76489A, seen from the main CPU. */
#ifndef MIKIE_SOUND_H
#define MIKIE_SOUND_H
#include <stdint.h>

#define SOUND_RATE 48000

int  sound_init(const char *romdir);      /* loads n10.6e; 0 on success */
void sound_reset(void);

/* Catch the sound board up with the main CPU. The 6809 runs at 1.536 MHz and
   the Z80 at 14.318181/4 MHz, so one 6809 cycle is 14318181/6144000 Z80
   cycles - a ratio, kept exact. */
void sound_run_to(uint64_t m6809_cycles);
void sound_run_z80_to(uint64_t z80_cycles);

void sound_latch_w(uint8_t v);            /* main CPU writes $2400 */
void sound_irq_w(int state);              /* LS259 bit 2 (SOUNDON) */

/* Pull mono samples at SOUND_RATE. The Z80 must already have been run past
   the end of the window, so call sound_run_to() first. */
void sound_render(int16_t *buf, int nsamples);

#endif
