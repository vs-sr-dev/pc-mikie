/* Trace build of the sound board, for diffing the Z80 against MAME.
 *
 * Prints the same fields as tools/trace_z80.lua, one instruction per pair of
 * lines (registers, then the PC), exactly like the 6809 trace build does.
 *
 *   build/z80_trace.exe [romdir] [instruction limit]
 */
#include "z80.h"
#include "mikie_sound.h"
#include <stdio.h>
#include <stdlib.h>

static long limit, count;

void z80_trace_hook(const z80_t *z)
{
    printf("AF=%02X%02X BC=%02X%02X DE=%02X%02X HL=%02X%02X "
           "IX=%02X%02X IY=%02X%02X SP=%04X "
           "AF2=%02X%02X BC2=%02X%02X DE2=%02X%02X HL2=%02X%02X "
           "I=%02X R=%02X IM=%X IFF1=%X HALT=%X CYC=%llu\n%04X\n",
           z->a, z->f, z->b, z->c, z->d, z->e, z->h, z->l,
           z->ixh, z->ixl, z->iyh, z->iyl, z->sp,
           z->a2, z->f2, z->b2, z->c2, z->d2, z->e2, z->h2, z->l2,
           z->i, z->r, z->im, z->iff1, z->halt,
           (unsigned long long)z->cycles, z->pc);
    if (limit && ++count >= limit) { fflush(stdout); exit(0); }
}

int main(int argc, char **argv)
{
    const char *romdir = (argc > 1) ? argv[1] : "rom";
    if (argc > 2) limit = atol(argv[2]);
    if (sound_init(romdir)) return 1;
    sound_reset();
    sound_run_z80_to((uint64_t)-1);       /* the limit stops us, not this */
    return 0;
}
