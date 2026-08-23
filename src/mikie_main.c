/* Host runtime for the recompiled 6809 code - Mikie (Konami GX469)
 *
 * Provides: the hardware memory map, reset, DAA, the unknown-address trap,
 * vblank/IRQ scheduling, and the trace mode used to diff against MAME.
 */
#include "m6809_rt.h"
#include "mikie_video.h"
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
    if (a >= 0x2000 && a <= 0x2007) { ls259[a - 0x2000] = v & 1; return; }
    if (a == 0x2100)                return;              /* watchdog */
    if (a == 0x2200)                { palettebank = v & 7; return; }
    if (a == 0x2400)                { soundlatch = v; return; }
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
 * Set MIKIE_DUMP_FRAME to a frame index and MIKIE_DUMP_PATH to a file: the
 * renderer writes that frame as a raw OUT_W*OUT_H*3 RGB buffer and exits, so
 * it can be diffed against a MAME snapshot taken at the same frame number
 * (tools/snap_frame.lua + tools/compare_frame.py).
 */
static long      dump_frame = -1;
static const char *dump_path = "trace/frame.raw";
static int       video_ready;

void vbl_frame_hook(void)
{
    static uint8_t rgb[OUT_W * OUT_H * 3];
    FILE *f;
    if (dump_frame < 0 || !video_ready) return;
    if ((long)(vbl_frame - 1) != dump_frame) return;

    video_render(WORK + 0x0000,          /* sprite RAM at $2800 */
                 WORK + 0x1000,          /* colour RAM at $3800 */
                 WORK + 0x1400,          /* video  RAM at $3C00 */
                 palettebank, ls259[6], rgb);
    f = fopen(dump_path, "wb");
    if (!f) { fprintf(stderr, "cannot write %s\n", dump_path); exit(4); }
    fwrite(rgb, 1, sizeof rgb, f);
    fclose(f);
    fprintf(stderr, "frame %ld written to %s (cycle %llu)\n",
            dump_frame, dump_path, (unsigned long long)cpu_cycles);
    exit(0);
}

/* ----------------------------------------------------------------- trap */
void cpu_unknown_pc(uint16_t pc)
{
    fprintf(stderr, "\n*** address was never recompiled: $%04X "
                    "(A=%02X B=%02X X=%04X Y=%04X U=%04X S=%04X DP=%02X CC=%02X)\n",
            pc, r_a, r_b, r_x, r_y, r_u, r_s, r_dp, r_cc);
    exit(3);
}

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
    cpu_reset();
    cpu_run();
    return 0;
}
