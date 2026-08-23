/* Mikie sound board.
 *
 *   $0000-$3FFF  program ROM (only the first 8 K is populated; the rest of the
 *                region reads 0, which is what MAME's ROM region gives)
 *   $4000-$43FF  RAM
 *   $8000    W   latch for the value the PSGs will take (write-only, ignored)
 *   $8002    W   strobe sn1
 *   $8003   R    command latch from the main CPU
 *   $8004    W   strobe sn2
 *   $8005   R    free-running timer: (Z80 cycles / 512) & $FF
 *
 * The PSG data really comes from the $8000 latch on the board, but the sound
 * program always writes the same byte to $8000 and to the strobe address, so
 * taking the strobe's own data byte - as MAME does - is equivalent.
 */
#include "mikie_sound.h"
#include "z80.h"
#include "sn76489.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define M6809_CLK 1536000u           /* 18432000/12 */

/* Converting a 6809 cycle into a Z80 cycle decides the exact instruction
 * boundary at which the sound CPU sees a command, so it has to be done the
 * way MAME does it or the two traces cannot be compared at all.
 *
 * MAME holds a device clock as an integer number of Hz, so the Z80 is
 * 14318181/4 = 3579545.25 rounded down to 3579545, and a device's local time
 * advances by a whole (truncated) number of attoseconds per cycle. The
 * difference against the true ratio is 7 parts in a hundred million - three
 * Z80 cycles after twenty-four seconds - but three cycles is exactly enough to
 * miss one turn of a twelve-cycle polling loop, which then shows up as a
 * different sound-driver state minutes later.
 */
/* Where the PSG tick grid sits inside a sample.
 *
 * On the board the divider's power-up phase is whatever the counter happened
 * to be, so nothing here is a hardware fact - but it does decide how well the
 * output lines up with MAME's, whose resampler effectively reads the middle of
 * each tick rather than its leading edge. Sampling the midpoint (half of 32
 * and of 16) is what that amounts to.
 *
 * Measured, over five seconds of the attract mode, as mean correlation against
 * MAME's own recording: leading edge 0.9647, midpoint 0.9937, and a broad
 * plateau either side of it - 8/4 through 18/9 all sit within 0.0002. Going
 * the other way, to 28/14, drops it to 0.8799. So the value is not fitted to
 * three decimal places; anywhere on the plateau does.
 */
#ifndef SN_PH0
#define SN_PH0 16
#endif
#ifndef SN_PH1
#define SN_PH1 8
#endif

#define ATTO_6809  651041666666ULL   /* 1e18 / 1536000, truncated */
#define ATTO_Z80   279365114840ULL   /* 1e18 / 3579545, truncated */

static uint64_t z80_cycle_of(uint64_t m6809)
{
#ifndef __SIZEOF_INT128__
#error "needs 128-bit integers; the product overflows 64 bits after 14 seconds"
#endif
    unsigned __int128 t = (unsigned __int128)m6809 * ATTO_6809;
    return (uint64_t)((t + (ATTO_Z80 - 1)) / ATTO_Z80);
}

static uint8_t   srom[0x4000];
static uint8_t   sram[0x0400];
static z80_t     cpu;
static uint8_t   latch;
static sn76489_t sn[2];

/* PSG writes are queued with the Z80 cycle they happened at, so the chips can
   be clocked later, when samples are actually pulled. */
#define QSIZE 16384
static struct { uint64_t cyc; uint8_t chip, data; } queue[QSIZE];
static int qhead, qtail;

static uint64_t sn_next[2];          /* next tick of each PSG, in Z80 cycles */
static uint64_t snd_pos;             /* how far the PSGs have been clocked */
static uint64_t sample_no;           /* samples emitted since reset */
static long     acc_sum;
static int      acc_n;
static int16_t  acc_last;
static long     acc_prev;

static FILE *snlog;                  /* MIKIE_SNLOG: PSG write trace */

/* --------------------------------------------------------------- Z80 bus */
uint8_t z80_read(uint16_t a)
{
    if (a < 0x4000)  return srom[a];
    if (a < 0x4400)  return sram[a & 0x3FF];
    if (a == 0x8003) return latch;
    if (a == 0x8005) return (uint8_t)(cpu.cycles / 512);
    return 0;
}

void z80_write(uint16_t a, uint8_t v)
{
    if (a >= 0x4000 && a < 0x4400) { sram[a & 0x3FF] = v; return; }
    if (a == 0x8002 || a == 0x8004) {
        int chip = (a == 0x8004);
        if (snlog) fprintf(snlog, "%llu %d %02X\n",
                           (unsigned long long)cpu.cycles, chip, v);
        if ((qhead + 1) % QSIZE == qtail) {
            /* nobody is pulling audio: retire the oldest write so the queue
               keeps its order instead of jumping the newest one ahead */
            sn76489_write(&sn[queue[qtail].chip], queue[qtail].data);
            qtail = (qtail + 1) % QSIZE;
        }
        queue[qhead].cyc  = cpu.cycles;
        queue[qhead].chip = (uint8_t)chip;
        queue[qhead].data = v;
        qhead = (qhead + 1) % QSIZE;
    }
}

uint8_t z80_in(uint16_t port)            { (void)port; return 0xFF; }
void    z80_out(uint16_t port, uint8_t v) { (void)port; (void)v; }

/* ------------------------------------------------------------------ board */
int sound_init(const char *romdir)
{
    char path[512];
    FILE *f;
    size_t n;

    snprintf(path, sizeof path, "%s/n10.6e", romdir ? romdir : "rom");
    f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "cannot open %s\n", path); return 1; }
    memset(srom, 0, sizeof srom);
    n = fread(srom, 1, 0x2000, f);
    fclose(f);
    if (n != 0x2000) { fprintf(stderr, "%s: expected 8192 bytes\n", path); return 1; }

    { const char *e = getenv("MIKIE_SNLOG");
      if (e) snlog = fopen(e, "w"); }
    return 0;
}

void sound_reset(void)
{
    memset(sram, 0, sizeof sram);
    z80_reset(&cpu);
    sn76489_reset(&sn[0]);
    sn76489_reset(&sn[1]);
    latch = 0;
    qhead = qtail = 0;
    sn_next[0] = SN_PH0;
    sn_next[1] = SN_PH1;
    snd_pos = 0;
    sample_no = 0;
    acc_sum = 0;
    acc_n = 0;
    acc_last = 0;
    acc_prev = 0;
}

void sound_latch_w(uint8_t v) { latch = v; }

/* The LS259 output going 0 then 1 pulses the Z80's interrupt line. */
void sound_irq_w(int state) { if (state) cpu.irq_line = 1; }

#ifdef MIKIE_Z80_TRACE
void z80_trace_hook(const z80_t *z);
#endif

void sound_run_z80_to(uint64_t target)
{
    while (cpu.cycles < target) {
        if (!z80_ready(&cpu)) continue;
#ifdef MIKIE_Z80_TRACE
        z80_trace_hook(&cpu);
#endif
        z80_exec(&cpu);
    }
}

void sound_run_to(uint64_t m6809_cycles)
{
    sound_run_z80_to(z80_cycle_of(m6809_cycles));
}

/* --------------------------------------------------------------- audio */
static void apply_writes(uint64_t until)
{
    while (qtail != qhead && queue[qtail].cyc <= until) {
        sn76489_write(&sn[queue[qtail].chip], queue[qtail].data);
        qtail = (qtail + 1) % QSIZE;
    }
}

/* sn2 is clocked at the Z80 rate and sn1 at half of it, and both divide by 16
   internally: one tick every 16 and every 32 Z80 cycles.
 *
 * Where that grid starts inside a sample is set by SN_PH0/SN_PH1 above.
 *
 * Output is averaged over TIME, not over ticks: the two grids do not line up,
 * so counting events would weight the chips unevenly. */
static void advance_to(uint64_t until)
{
    while (snd_pos < until) {
        uint64_t t    = (sn_next[0] < sn_next[1]) ? sn_next[0] : sn_next[1];
        uint64_t stop = (t < until) ? t : until;
        if (stop > snd_pos) {
            long dt = (long)(stop - snd_pos);
            acc_sum += (long)(sn76489_out(&sn[0]) + sn76489_out(&sn[1])) * dt;
            acc_n   += (int)dt;
            snd_pos  = stop;
        }
        if (snd_pos >= until) break;
        apply_writes(t);
        if (sn_next[1] == t) { sn76489_step(&sn[1]); sn_next[1] += 16; }
        if (sn_next[0] == t) { sn76489_step(&sn[0]); sn_next[0] += 32; }
    }
    apply_writes(until);
}

void sound_render(int16_t *buf, int nsamples)
{
    int k;
    for (k = 0; k < nsamples; k++) {
        /* the window is placed on the main CPU's timeline and converted the
           same way as everything else, so a render can never run past the
           point the Z80 has actually been executed to */
        uint64_t end = z80_cycle_of(++sample_no * M6809_CLK / SOUND_RATE);
        long v;
        acc_sum = 0;
        acc_n = 0;
        advance_to(end);
        if (acc_n == 0) { buf[k] = acc_last; continue; }
        /* Averaging the chip ticks inside one sample window is a box filter,
           which barely attenuates anything near Nyquist; averaging two
           adjacent windows makes it triangular, with its first null right at
           24 kHz. The PSGs run at 224 and 112 kHz, so without that the
           harmonics of every square wave fold straight back into the audible
           band. */
        v = acc_sum / acc_n;
        v = (v + acc_prev) / 2;
        acc_prev = acc_sum / acc_n;
        v = v * 3 / 5;                        /* MAME routes each chip at 0.60 */
        if (v >  32767) v =  32767;
        if (v < -32768) v = -32768;
        acc_last = (int16_t)v;
        buf[k] = acc_last;
    }
}
