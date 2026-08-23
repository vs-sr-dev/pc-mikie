/* Z80 core - Mikie sound board (Konami GX469, Z80 at 14.318181 MHz / 4).
 *
 * Verified cycle by cycle against MAME's z80 device: the counter here is
 * diffed against a MAME trace instruction by instruction, so the counting
 * rules are MAME's.  Two of them are worth stating up front:
 *
 *  - a memory access happens BEFORE the T-states of its own machine cycle are
 *    charged.  That is observable, not academic: the sound timer at $8005 is
 *    (Z80 cycles / 512) sampled in the middle of an instruction.
 *  - `q` holds the YX flags left by the previous instruction, but only if that
 *    instruction wrote F at all.  SCF and CCF merge it with A.
 */
#ifndef Z80_H
#define Z80_H
#include <stdint.h>

typedef struct {
    uint8_t  a, f, b, c, d, e, h, l;
    uint8_t  a2, f2, b2, c2, d2, e2, h2, l2;
    uint8_t  ixh, ixl, iyh, iyl;
    uint16_t pc, sp, wz;
    uint8_t  i, r, im;
    uint8_t  iff1, iff2, halt, after_ei;
    uint8_t  q, qt;              /* SCF/CCF quirk, see above */
    uint64_t cycles;
    int      irq_line;           /* HOLD_LINE: set by the board, cleared on ack */
    uint8_t  irq_vector;         /* byte the board puts on the bus in IM 0 */
} z80_t;

void z80_reset(z80_t *z);

/* Split in two because that is where MAME's trace hook sits: interrupts are
   taken, and halt cycles burnt, BEFORE the instruction is logged. */
int  z80_ready(z80_t *z);        /* service interrupts/halt; 1 = will execute */
void z80_exec(z80_t *z);         /* execute exactly one instruction */

/* supplied by the board */
uint8_t z80_read(uint16_t addr);
void    z80_write(uint16_t addr, uint8_t data);
uint8_t z80_in(uint16_t port);
void    z80_out(uint16_t port, uint8_t data);

#endif
