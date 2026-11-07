/* The display at the end of a frame: the copper list from COP1LC
 * interpreted line by line, the bitplanes fetched as the registers say
 * (DDFSTRT/STOP, the modulos, BPLCON1's fine scroll, dual playfields), the
 * sprites from their DMA lists, into frame_rgb (320x256, lores, PAL). */
#include "m68k.h"

extern u8 frame_rgb[320 * 256 * 3];
u32 *bplpt_ptr(void);
u32 *sprpt_ptr(void);
u16 *custom_regs(void);
u16 *color_regs(void);
u32 cop1lc_get(void);
int copjmp_take(void);
void chip_write(u32 addr, u32 value, int bits);

#define TOP 0x2C
#define LEFT 0x81

static void put(int x, int y, u16 rgb12) {
	if (x < 0 || x >= 320 || y < 0 || y >= 256) return;
	u8 *p = frame_rgb + (y * 320 + x) * 3;
	p[0] = (u8)((rgb12 >> 8 & 15) * 17); p[1] = (u8)((rgb12 >> 4 & 15) * 17); p[2] = (u8)((rgb12 & 15) * 17);
}

/* one raster line of bitplanes */
static void render_line(int y, u16 *reg, u16 *col, u32 *bpl) {
	u16 bplcon0 = reg[0x100 >> 1], bplcon1 = reg[0x102 >> 1], bplcon2 = reg[0x104 >> 1];
	u16 diwstrt = reg[0x08E >> 1], diwstop = reg[0x090 >> 1];
	u16 ddfstrt = reg[0x092 >> 1] & 0xFC, ddfstop = reg[0x094 >> 1] & 0xFC;
	int top = diwstrt >> 8, bottom = (diwstop >> 8) | ((diwstop >> 8) < 0x80 ? 0x100 : 0);
	int line = y + TOP;
	int depth = (bplcon0 >> 12) & 7, dpf = bplcon0 & 0x400, hires = bplcon0 & 0x8000;
	if (line < top || line >= bottom || depth == 0 || !(reg[0x096 >> 1] & 0x0100) || !(reg[0x096 >> 1] & 0x0200)) {
		for (int x = 0; x < 320; x++) put(x, y, col[0]);
		return;
	}
	int words = ((ddfstop - ddfstrt) >> 3) + 1;
	if (hires) words = ((ddfstop - ddfstrt) >> 2) + 2;
	if (words < 1) words = 1;
	if (words > 64) words = 64;
	int skip = (0x38 - ddfstrt) >> 3;
	if (skip < 0) skip = 0;
	int shift[2] = { bplcon1 & 15, (bplcon1 >> 4) & 15 };
	int left = (diwstrt & 0xFF) - LEFT;   /* the window's left edge relative to the usual */
	u16 rows[8][64];
	for (int p = 0; p < depth; p++) {
		for (int w = 0; w < words; w++) rows[p][w] = (u16)rd16(bpl[p] + 2 * w);
		bpl[p] += (u32)words * 2 + (u32)(s16)reg[(p & 1 ? 0x10A : 0x108) >> 1];
	}
	int pf2pri = bplcon2 & 0x40;
	for (int x = 0; x < 320; x++) {
		int c = 0;
		if (dpf) {
			int c1 = 0, c2 = 0;
			for (int p = 0; p < depth; p++) {
				int bx = 16 * skip + x - left - shift[p & 1];
				if (bx < 0 || bx >= words * 16) continue;
				if (rows[p][bx >> 4] >> (15 - (bx & 15)) & 1) { if (p & 1) c2 |= 1 << (p >> 1); else c1 |= 1 << (p >> 1); }
			}
			c = pf2pri ? (c2 ? 8 + c2 : c1) : (c1 ? c1 : (c2 ? 8 + c2 : 0));
		} else {
			int bx = 16 * skip + x - left - shift[0];
			if (bx >= 0 && bx < words * 16) {
				for (int p = 0; p < depth; p++) if (rows[p][bx >> 4] >> (15 - (bx & 15)) & 1) c |= 1 << p;
			}
		}
		if (bplcon0 & 0x800) {            /* HAM: not supported, show the base colour */
			c &= 15;
		}
		put(x, y, col[c & 31]);
	}
}

/* the sprites: each channel's DMA list, VSTART..VSTOP with the next sprite after */
static void render_sprites(u32 *spr, u16 *col) {
	for (int ch = 0; ch < 8; ch++) {
		u32 p = spr[ch];
		if (!p) continue;
		for (int guard = 0; guard < 64; guard++) {
			u16 pos = (u16)rd16(p), ctl = (u16)rd16(p + 2);
			if (!pos && !ctl) break;
			int vstart = (pos >> 8) | ((ctl >> 2 & 1) << 8), vstop = (ctl >> 8) | ((ctl >> 1 & 1) << 8);
			int hstart = ((pos & 0xFF) << 1) | (ctl & 1);
			int attached = ctl & 0x80;
			u32 data = p + 4;
			int x0 = hstart - LEFT * 2 / 2 - 1;  /* HSTART is in half-pixels from the left of the line: 0x81 = pixel 0 */
			x0 = (hstart - 0x81) ;
			for (int line = vstart; line < vstop; line++) {
				u16 a = (u16)rd16(data), b = (u16)rd16(data + 2);
				data += 4;
				int y = line - TOP;
				if (y < 0 || y >= 256) continue;
				for (int i = 0; i < 16; i++) {
					int c = (a >> (15 - i) & 1) | ((b >> (15 - i) & 1) << 1);
					if (!c) continue;
					int x = x0 + i;
					if (attached && (ch & 1)) continue;      /* (attached pairs: drawn with the even one, below) */
					put(x, y, col[16 + (ch >> 1) * 4 + c]);
				}
			}
			p = data;
		}
	}
}

void display_render(void) {
	u16 *reg = custom_regs();
	u16 *col = color_regs();
	u32 bpl[8], *live = bplpt_ptr();
	for (int i = 0; i < 8; i++) bpl[i] = live[i];
	/* the copper: run the list from COP1LC through the frame, applying MOVEs at their WAIT lines */
	u32 cp = cop1lc_get();
	int copper_on = reg[0x096 >> 1] & 0x0080;
	int wait_line = -1;
	int done = !copper_on || !cp;
	int past255 = 0;                      /* a WAIT compares 8 bits of the line: after one for line 255, add 256 */
	for (int y = 0; y < 256; y++) {
		int line = y + TOP;
		while (!done) {
			u16 a = (u16)rd16(cp), b = (u16)rd16(cp + 2);
			if (a & 1) {
				int vp = a >> 8;
				if (a == 0xFFFF && b == 0xFFFE) { done = 1; break; }
				if ((b & 1) == 0) {       /* WAIT */
					int target = vp + (past255 ? 256 : 0);
					if (!past255 && vp == 0xFF && (a & 0xFE) >= 0xDE) {
						if (line < 256) { wait_line = 256; break; }   /* the end of line 255 */
						past255 = 1; cp += 4; continue;
					}
					if (target > line) { wait_line = target; break; }
					cp += 4;
				} else cp += 4;           /* SKIP: never skips here */
				continue;
			}
			cp += 4;
			if (a == 0x088) { cp = cop1lc_get(); continue; }
			if (a == 0x08A) { extern u32 cop2lc_get(void); cp = cop2lc_get(); continue; }
			chip_write(CUSTOM_BASE + (a & 0x1FE), b, 16);
			if ((a & 0x1FE) >= 0x0E0 && (a & 0x1FE) < 0x100) bpl[(a - 0x0E0) >> 2 & 7] = live[(a - 0x0E0) >> 2 & 7];   /* a pointer reload */
		}
		(void)wait_line;
		render_line(y, reg, col, bpl);
	}
	render_sprites(sprpt_ptr(), col);
	copjmp_take();
}
