/* the Z80 (z80.c) */
#ifndef Z80_H
#define Z80_H
#include "host.h"

typedef struct {
	u8 a, f, b, c, d, e, h, l;
	u8 a_, f_, b_, c_, d_, e_, h_, l_;
	u16 ix, iy, sp, pc;
	u8 i, r, iff1, iff2, im, halt, ei_wait;
} z80_t;

u8 z80_rd(u16 a);                         /* the machine's */
void z80_wr(u16 a, u8 v);

int z80_step(z80_t *z);                   /* one instruction -> its T-states */
int z80_irq(z80_t *z);                    /* the interrupt line is low -> T-states if it was taken, else 0 */
void z80_reset(z80_t *z);
#endif
