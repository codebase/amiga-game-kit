/* The blitter: area mode (copies, cookie cuts, fills) and line mode,
 * run to completion the moment BLTSIZE is written. */
#include "m68k.h"

static u16 ror16(u16 v, int n) { return n ? (u16)((v >> n) | (v << (16 - n))) : v; }

void blitter_run(u16 *regs, u32 *pts) {
	u16 con0 = regs[0x040 >> 1], con1 = regs[0x042 >> 1];
	u16 afwm = regs[0x044 >> 1], alwm = regs[0x046 >> 1];
	s16 amod = (s16)regs[0x064 >> 1], bmod = (s16)regs[0x062 >> 1], cmod = (s16)regs[0x060 >> 1], dmod = (s16)regs[0x066 >> 1];
	u16 size = regs[0x058 >> 1];
	u32 h = size >> 6 ? size >> 6 : 1024, w = size & 63 ? size & 63 : 64;
	u32 apt = pts[0], bpt = pts[1], cpt = pts[2], dpt = pts[3];
	u16 adat = regs[0x074 >> 1], bdat = regs[0x072 >> 1], cdat = regs[0x070 >> 1];
	int use_a = con0 & 0x800, use_b = con0 & 0x400, use_c = con0 & 0x200, use_d = con0 & 0x100;
	int ash = con0 >> 12, bsh = con1 >> 12;
	int desc = con1 & 2;
	u8 minterm = (u8)con0;
	int zero = 1;

	if (con1 & 1) {                       /* line mode: a Bresenham line, one pixel per row */
		/* (the registers hold the octant and the error terms the way the hardware wants:
		   enough games draw lines with it that this stays a TODO until one does) */
		trace_die("blitter line mode isn't implemented yet", 0);
	}
	for (u32 y = 0; y < h; y++) {
		u32 aold = 0, bold = 0;
		for (u32 x = 0; x < w; x++) {
			u16 a_raw = adat, b_raw = bdat, c = cdat;
			if (use_a) { a_raw = (u16)rd16(apt); apt = desc ? apt - 2 : apt + 2; }
			if (use_b) { b_raw = (u16)rd16(bpt); bpt = desc ? bpt - 2 : bpt + 2; }
			if (use_c) { c = (u16)rd16(cpt); cpt = desc ? cpt - 2 : cpt + 2; }
			u16 am = a_raw;
			if (x == 0) am &= afwm;
			if (x == w - 1) am &= alwm;
			u32 a, b;
			if (!desc) {
				a = (u16)(((u32)aold << 16 | am) >> ash);
				b = (u16)(((u32)bold << 16 | b_raw) >> bsh);
			} else {
				a = (u16)(((u32)am << 16 | aold) >> (16 - ash));
				b = (u16)(((u32)b_raw << 16 | bold) >> (16 - bsh));
			}
			aold = am; bold = b_raw;
			u16 d = 0;
			for (int bit = 0; bit < 16; bit++) {
				int ia = a >> bit & 1, ib = b >> bit & 1, ic = c >> bit & 1;
				int idx = ia << 2 | ib << 1 | ic;
				if (minterm >> idx & 1) d |= (u16)(1 << bit);
			}
			if (con1 & 0x18) {            /* fill modes: inclusive (0x08) / exclusive (0x10), from the right */
				int fc = (con1 >> 2) & 1;
				u16 out = 0;
				for (int bit = 0; bit < 16; bit++) {
					int in = d >> bit & 1;
					int o = (con1 & 0x08) ? (in | fc) : (in ^ fc);
					if (in) fc ^= 1;
					if (o) out |= (u16)(1 << bit);
				}
				d = out;
			}
			if (d) zero = 0;
			if (use_d) { wr16(dpt, d); dpt = desc ? dpt - 2 : dpt + 2; }
		}
		if (use_a) apt = desc ? apt - amod : apt + amod;
		if (use_b) bpt = desc ? bpt - bmod : bpt + bmod;
		if (use_c) cpt = desc ? cpt - cmod : cpt + cmod;
		if (use_d) dpt = desc ? dpt - dmod : dpt + dmod;
	}
	pts[0] = apt; pts[1] = bpt; pts[2] = cpt; pts[3] = dpt;
	regs[0x002 >> 1] = zero ? 0x2000 : 0;  /* BZERO for DMACONR */
	(void)ror16;
}
