/* Host front end: window, sound device and keyboard.
 *
 * Deliberately narrow. The emulated machine never learns that SDL exists: it
 * hands over a finished frame and a block of samples, and gets back the state
 * of eight input lines. Everything else - scaling, the audio queue, the event
 * loop - stays on this side.
 */
#ifndef MIKIE_HOST_H
#define MIKIE_HOST_H

#include <stdint.h>

int  host_init(int width, int height, int rate);   /* 0 on success */
void host_shutdown(void);

void host_frame(const uint8_t *rgb);               /* width * height * 3 */
void host_audio(const int16_t *buf, int nsamples);

/* Samples still to be played. The pacing loop uses this: the sound device's
   own clock is the only honest one on the machine, so the emulation follows
   it rather than a wall-clock timer. */
int  host_audio_queued(void);
int  host_audio_running(void);

/* Active-low, exactly as the board reads them at $2400/$2401/$2402. */
void host_inputs(uint8_t *system, uint8_t *p1, uint8_t *p2);

int  host_should_quit(void);
void host_sleep(int ms);
unsigned host_ticks(void);        /* milliseconds since start-up */

#endif
