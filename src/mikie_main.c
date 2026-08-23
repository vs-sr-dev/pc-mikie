/* Host runtime for the recompiled 6809 code - Mikie (Konami GX469)
 *
 * Provides: the hardware memory map, reset, DAA, the unknown-address trap,
 * vblank/IRQ scheduling, and the trace mode used to diff against MAME.
 */
#include "m6809_rt.h"
#include "mikie_video.h"
#include "mikie_sound.h"
#ifdef MIKIE_SDL
#include "mikie_host.h"
#endif
#include <stdio.h>
#include <stdlib.h>

/* ------------------------------------------------------------ registers */
uint8_t  r_a, r_b, r_dp, r_cc;
uint16_t r_x, r_y, r_u, r_s;
uint64_t cpu_cycles;
int      irq_pending;

static uint8_t ROM[0x10000];      /* ROM image ($6000-$FFFF populated) */
static uint8_t RAM0[0x0100];      /* $0000-$00FF */
static uint8_t WORK[0x1800];      /* $2800-$3FFF: sprites + work + video RAM */

/* --- inputs (active low) and DIP switches, idle values.
   These are exactly what MAME reports when nothing is pressed; getting DSW3
   wrong by one bit was worth ~500k instructions of debugging. */
static uint8_t in_system = 0xFF, in_p1 = 0xFF, in_p2 = 0xFF;
static uint8_t dsw1 = 0xFF;   /* 1 coin / 1 credit, both slots */
static uint8_t dsw2 = 0x7B;   /* 3 lives, upright, 20k/70k/50k+, easy, demo on */
static uint8_t dsw3 = 0xFE;   /* single upright controls + unused bits + $F0 */

/* --- hardware latches */
static uint8_t ls259[8];
static uint8_t palettebank;
static uint8_t soundlatch;

/* --------------------------------------------------------------- memory */
uint8_t mem_read(uint16_t a)
{
    if (a < 0x0100)                 return RAM0[a];
    if (a >= 0x6000)                return ROM[a];
    if (a >= 0x2800 && a < 0x4000)  return WORK[a - 0x2800];
    switch (a) {
        case 0x2400: return in_system;
        case 0x2401: return in_p1;
        case 0x2402: return in_p2;
        case 0x2403: return dsw3;
        case 0x2500: return dsw1;
        case 0x2501: return dsw2;
    }
    /* $4000-$5FFF: the socket for the "conversion kit" ROM, absent in every
       dumped set. MAME returns 0, and so must we: the boot code looks for a
       $55/$AA signature at $5FF0 and takes the normal path when it is missing.
       Returning $FF here would not match the signature either, but it would
       stop the traces from lining up. */
    return 0x00;
}

void mem_write(uint16_t a, uint8_t v)
{
    if (a < 0x0100)                 { RAM0[a] = v; return; }
    if (a >= 0x2800 && a < 0x4000)  { WORK[a - 0x2800] = v; return; }
    if (a >= 0x6000)                return;              /* ROM: ignore */
    if (a >= 0x2000 && a <= 0x2007) {
        /* The LS259 only pulses an output when the bit actually changes, so
           SOUNDON has to be edge-detected: a 0 -> 1 there is what interrupts
           the sound Z80. */
        int bit = a - 0x2000, s = v & 1;
        if (ls259[bit] != s) {
            ls259[bit] = (uint8_t)s;
            if (bit == 2 && s) { sound_run_to(cpu_cycles); sound_irq_w(1); }
        }
        return;
    }
    if (a == 0x2100)                return;              /* watchdog */
    if (a == 0x2200)                { palettebank = v & 7; return; }
    if (a == 0x2400)                {
        /* the sound board must not see the new command before the cycle the
           main CPU wrote it at */
        sound_run_to(cpu_cycles);
        sound_latch_w(v);
        soundlatch = v;
        return;
    }
}

/* the IRQ mask is LS259 output 7 */
int vbl_irq_enabled(void) { return ls259[7]; }

/* ------------------------------------------------------------------ DAA */
void daa(void)
{
    uint8_t  msn = (uint8_t)(r_a & 0xF0), lsn = (uint8_t)(r_a & 0x0F);
    uint16_t cf = 0;
    if (lsn > 0x09 || (r_cc & CC_H)) cf |= 0x06;
    if (msn > 0x80 && lsn > 0x09)    cf |= 0x60;
    if (msn > 0x90 || (r_cc & CC_C)) cf |= 0x60;
    {
        uint16_t t = (uint16_t)r_a + cf;
        r_cc &= (uint8_t)~CC_V;
        if (t & 0x100) r_cc |= CC_C;
        r_a = (uint8_t)t;
    }
    SETNZ8(r_a);
}

/* ----------------------------------------------------------- video timing */
uint64_t vbl_frame;      /* index of the next edge */
uint64_t vbl_next;       /* cycle of the next vblank edge */

/* CWAI halts the CPU until the interrupt, so time jumps EXACTLY to the edge,
   without the 15-cycle delivery latency that normal execution sees. That is
   precisely why interrupts taken during CWAI gave the exact vblank phase. */
void cpu_wait_irq(void)
{
    long guard = 0;
    while (!irq_pending) {
        if (cpu_cycles < vbl_next) cpu_cycles = vbl_next;
        vbl_advance();
        if (++guard > 1000000) {
            fprintf(stderr, "cpu_wait_irq: the IRQ was never enabled, giving up\n");
            exit(2);
        }
    }
}

/* ------------------------------------------------------- frame dumping
 *
 * The screen is drawn at the END of the visible period, that is on the vblank
 * edge that *terminates* the frame - and before the vblank handler for the
 * next frame runs. So MAME's frame N is our edge N+1, taken at the edge with
 * no offset of any kind.
 *
 * Worth stating because the obvious alternative is wrong: capturing on the
 * edge that *starts* frame N shows the frame before it, and no amount of
 * fudging the capture point inside the frame fixes that in general. It looks
 * like it does during normal play, when the game only touches video memory in
 * the vblank handler, and then falls apart during the boot self-test, which
 * writes video RAM continuously.
 *
 * MIKIE_DUMP_FRAME  MAME frame number to capture
 * MIKIE_DUMP_PATH   output file (raw OUT_W * OUT_H * 3 RGB)
 * MIKIE_ROM_DIR     directory holding the graphics ROMs and PROMs
 */
static long      dump_frame = -1;
static const char *dump_path = "trace/frame.raw";
static int       video_ready;
static long      run_frames;        /* MIKIE_RUN_FRAMES: stop after this many */

static void sound_pump(void);

#ifdef MIKIE_SDL
static int host_ready;
static void host_serve(const uint8_t *rgb);
#endif

void vbl_frame_hook(void)
{
    static uint8_t rgb[OUT_W * OUT_H * 3];
    FILE *f;

    sound_pump();
    if (run_frames && (long)vbl_frame > run_frames) exit(0);
    if (!video_ready) return;

#ifdef MIKIE_SDL
    if (host_ready) {
        video_render(WORK + 0x0000, WORK + 0x1000, WORK + 0x1400,
                     palettebank, ls259[6], rgb);
        host_serve(rgb);
        return;
    }
#endif
    if (dump_frame < 0) return;
    if ((long)(vbl_frame - 2) != dump_frame) return;    /* MAME frame N = edge N+1 */

    video_render(WORK + 0x0000,          /* sprite RAM at $2800 */
                 WORK + 0x1000,          /* colour RAM at $3800 */
                 WORK + 0x1400,          /* video  RAM at $3C00 */
                 palettebank, ls259[6], rgb);
    f = fopen(dump_path, "wb");
    if (!f) { fprintf(stderr, "cannot write %s\n", dump_path); exit(4); }
    fwrite(rgb, 1, sizeof rgb, f);
    fclose(f);
    fprintf(stderr, "MAME frame %ld -> %s (cycle %llu)\n",
            dump_frame, dump_path, (unsigned long long)cpu_cycles);
    exit(0);
}

/* ------------------------------------------------------------- front end
 *
 * The board is paced by the sound device, not by a timer: its clock is the
 * only one in the machine that cannot be argued with, and letting the queue
 * length steer the emulation keeps audio and video locked to each other for
 * free. If there is no sound device we fall back to a wall-clock deadline.
 *
 * The display is deliberately NOT vsynced. The screen runs at 60.59 Hz, which
 * is nobody's refresh rate; syncing to the monitor would mean dropping or
 * repeating one frame every few seconds.
 */
#ifdef MIKIE_SDL
#define AUDIO_HIGH_WATER  (SOUND_RATE * 2 / 60)     /* about two frames */

static void host_serve(const uint8_t *rgb)
{
    static unsigned epoch, deadline;
    static uint64_t frames_shown;
    int guard = 0;

    host_frame(rgb);
    host_inputs(&in_system, &in_p1, &in_p2);
    if (host_should_quit()) exit(0);

    if (host_audio_running()) {
        while (host_audio_queued() > AUDIO_HIGH_WATER && guard++ < 500)
            host_sleep(1);
    } else {
        /* No sound device: fall back to the clock. The deadline is computed
           from the frame index rather than added up frame by frame, because a
           frame lasts 16.5 ms and rounding that to whole milliseconds every
           time would run 3 % fast. */
        unsigned now = host_ticks();
        if (!epoch || now < epoch || now - epoch > frames_shown * 20 + 1000) {
            epoch = now;
            frames_shown = 0;
        }
        frames_shown++;
        deadline = epoch + (unsigned)(frames_shown * VBL_NUM * 1000ULL
                                      / (VBL_DEN * 1536000ULL));
        while (host_ticks() < deadline && guard++ < 500) host_sleep(1);
    }
}
#endif

/* -------------------------------------------------------------- audio out
 *
 * The sound board is caught up once per frame and, if MIKIE_WAV is set, the
 * samples it produces are written out. The sample count is derived from the
 * 6809 cycle counter rather than accumulated per frame, so it cannot drift.
 */
static FILE   *wav;
static uint64_t samples_out;      /* samples handed to the outputs so far */

static void wav_close(void)
{
    uint32_t n;
    if (!wav) return;
    n = (uint32_t)(samples_out * 2);
    fseek(wav, 40, SEEK_SET); fwrite(&n, 4, 1, wav);
    n += 36;
    fseek(wav, 4, SEEK_SET);  fwrite(&n, 4, 1, wav);
    fclose(wav);
    wav = NULL;
}

static void wav_open(const char *path)
{
    static const uint8_t hdr[44] = {
        'R','I','F','F', 0,0,0,0, 'W','A','V','E', 'f','m','t',' ',
        16,0,0,0, 1,0, 1,0,
        (uint8_t)(SOUND_RATE & 0xFF), (uint8_t)((SOUND_RATE >> 8) & 0xFF),
        (uint8_t)((SOUND_RATE >> 16) & 0xFF), (uint8_t)(SOUND_RATE >> 24),
        (uint8_t)((SOUND_RATE * 2) & 0xFF), (uint8_t)(((SOUND_RATE * 2) >> 8) & 0xFF),
        (uint8_t)(((SOUND_RATE * 2) >> 16) & 0xFF), (uint8_t)((SOUND_RATE * 2) >> 24),
        2,0, 16,0, 'd','a','t','a', 0,0,0,0
    };
    wav = fopen(path, "wb");
    if (!wav) { fprintf(stderr, "cannot write %s\n", path); exit(5); }
    fwrite(hdr, 1, sizeof hdr, wav);
    atexit(wav_close);
}

static void sound_pump(void)
{
    static int16_t buf[4096];
    long want;
    int  live = 0;

    sound_run_to(cpu_cycles);
#ifdef MIKIE_SDL
    live = host_ready;
#endif
    if (!wav && !live) return;
    want = (long)(cpu_cycles * SOUND_RATE / 1536000 - samples_out);
    while (want > 0) {
        int n = (want > 4096) ? 4096 : (int)want;
        sound_render(buf, n);
        if (wav) fwrite(buf, 2, (size_t)n, wav);
#ifdef MIKIE_SDL
        if (live) host_audio(buf, n);
#endif
        samples_out += (unsigned)n;
        want -= n;
    }
}

/* ----------------------------------------------------------------- trap */
void cpu_unknown_pc(uint16_t pc)
{
    fprintf(stderr, "\n*** address was never recompiled: $%04X "
                    "(A=%02X B=%02X X=%04X Y=%04X U=%04X S=%04X DP=%02X CC=%02X)\n",
            pc, r_a, r_b, r_x, r_y, r_u, r_s, r_dp, r_cc);
    exit(3);
}

/* ------------------------------------------------- tracing the sound Z80
 *
 * MIKIE_Z80_SKIP   start logging at this Z80 cycle (a full run from reset
 *                  would be gigabytes; tools/diffz80.py aligns on the state,
 *                  so the exact starting point does not matter)
 * MIKIE_Z80_LIMIT  stop after this many instructions
 */
#ifdef MIKIE_Z80_TRACE
#include "z80.h"
static uint64_t z80_skip;
static long     z80_limit, z80_count;

void z80_trace_hook(const z80_t *z)
{
    if (z->cycles < z80_skip) return;
    printf("AF=%02X%02X BC=%02X%02X DE=%02X%02X HL=%02X%02X "
           "IX=%02X%02X IY=%02X%02X SP=%04X "
           "AF2=%02X%02X BC2=%02X%02X DE2=%02X%02X HL2=%02X%02X "
           "I=%02X R=%02X IM=%X IFF1=%X HALT=%X CYC=%llu\n%04X\n",
           z->a, z->f, z->b, z->c, z->d, z->e, z->h, z->l,
           z->ixh, z->ixl, z->iyh, z->iyl, z->sp,
           z->a2, z->f2, z->b2, z->c2, z->d2, z->e2, z->h2, z->l2,
           z->i, z->r, z->im, z->iff1, z->halt,
           (unsigned long long)z->cycles, z->pc);
    if (z80_limit && ++z80_count >= z80_limit) { fflush(stdout); exit(0); }
}
#endif

/* ------------------------------------------------------------- tracing */
#ifdef MIKIE_TRACE
static long trace_limit = 0, trace_count = 0;
void trace_insn(uint16_t pc)
{
    printf("A=%02X B=%02X X=%04X Y=%04X U=%04X S=%04X DP=%02X CC=%02X CYC=%llu\n%04X\n",
           r_a, r_b, r_x, r_y, r_u, r_s, r_dp, r_cc,
           (unsigned long long)cpu_cycles, pc);
    if (trace_limit && ++trace_count >= trace_limit) { fflush(stdout); exit(0); }
}
#endif

/* ------------------------------------------------------------- start-up */
static uint16_t entry_pc;
uint16_t cpu_entry_pc(void) { return entry_pc; }

void cpu_reset(void)
{
    r_a = r_b = r_dp = 0;
    r_x = r_y = r_u = r_s = 0;
    r_cc = CC_I | CC_F;
    cpu_cycles = 4;          /* cost of the reset sequence, as MAME counts it */
    vbl_frame  = 1;
    vbl_next   = vbl_edge(1);
    irq_pending = 0;
    entry_pc = (uint16_t)((ROM[0xFFFE] << 8) | ROM[0xFFFF]);
}

int main(int argc, char **argv)
{
    const char *rompath = (argc > 1) ? argv[1] : "rom/maincpu.bin";
    FILE *f = fopen(rompath, "rb");
    if (!f) { fprintf(stderr, "cannot open %s\n", rompath); return 1; }
    if (fread(ROM, 1, sizeof ROM, f) != sizeof ROM) {
        fprintf(stderr, "%s: expected 65536 bytes\n", rompath); return 1;
    }
    fclose(f);
#ifdef MIKIE_TRACE
    if (argc > 2) trace_limit = atol(argv[2]);
#endif
    {
        const char *e = getenv("MIKIE_DUMP_FRAME");
        if (e) {
            const char *romdir = getenv("MIKIE_ROM_DIR");
            dump_frame = atol(e);
            if (getenv("MIKIE_DUMP_PATH")) dump_path = getenv("MIKIE_DUMP_PATH");
            if (video_init(romdir ? romdir : "rom") == 0) video_ready = 1;
            else { fprintf(stderr, "video_init failed\n"); return 1; }
        }
    }
#ifdef MIKIE_Z80_TRACE
    if (getenv("MIKIE_Z80_SKIP"))  z80_skip  = strtoull(getenv("MIKIE_Z80_SKIP"), 0, 10);
    if (getenv("MIKIE_Z80_LIMIT")) z80_limit = atol(getenv("MIKIE_Z80_LIMIT"));
#endif
    if (getenv("MIKIE_RUN_FRAMES")) run_frames = atol(getenv("MIKIE_RUN_FRAMES"));
    if (sound_init(getenv("MIKIE_ROM_DIR") ? getenv("MIKIE_ROM_DIR") : "rom"))
        return 1;
    if (getenv("MIKIE_WAV")) wav_open(getenv("MIKIE_WAV"));
#ifdef MIKIE_SDL
    /* dumping a frame is a batch job: it must not open a window */
    if (dump_frame < 0) {
        const char *romdir = getenv("MIKIE_ROM_DIR");
        if (video_init(romdir ? romdir : "rom")) {
            fprintf(stderr, "video_init failed\n");
            return 1;
        }
        video_ready = 1;
        if (host_init(OUT_W, OUT_H, SOUND_RATE)) return 1;
        atexit(host_shutdown);
        host_ready = 1;
    }
#endif
    cpu_reset();
    sound_reset();
    cpu_run();
    return 0;
}
