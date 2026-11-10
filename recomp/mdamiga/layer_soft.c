/* The hardware layer of a Mega Drive game ported with agk port, on an AGA
 * Amiga: what the rewritten code's calls reach instead of the Mega Drive's
 * chips. First version, for correctness: the VDP is kept as the game left it
 * (registers, VRAM, CRAM, VSRAM, DMA) and a frame is drawn from it by the
 * CPU, a line at a time, into six bitplanes: 64 colours, the Mega Drive's
 * own palette exactly. That is the same picture the kit's host runner draws
 * (recomp/md/md.c, which this follows), and far too slow for a stock A1200;
 * the frame rate is whatever the CPU manages while the game's logic keeps
 * its own time from the vertical blank. The pads are the joystick in port 2
 * and the keyboard. No sound yet.
 *
 * Called from start.S: sh_r/sh_w with interrupts off on the layer's stack,
 * layer_vbl from the vertical blank, layer_render with interrupts on. */
#include "port_layout.h"

typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned long u32;
typedef signed short s16;

#define CUSTOM(off) (*(volatile u16 *)(0xDFF000 + (off)))
#define CIAA(off) (*(volatile u8 *)(0xBFE001 + (off)))
#define W 320
#define H 224
#define ROWBYTES 40
#define PLANES 6
#define PLANE_SIZE (ROWBYTES * H)

static u8 vram[0x10000];
static u16 cram[64], vsram[40];
static u8 reg[24];
static u32 vdp_code, vdp_addr;
static int ctrl_pending, dma_fill_pending;
static u16 status = 0x3400;
static u8 pad_th[2], pad_ctrl[2];
static u16 pad_state[2];                  /* bits: 0 up 1 down 2 left 3 right 4 B 5 C 6 A 7 Start */
static u16 keys;                          /* the same bits, from the keyboard */
static int palette_dirty = 1;
volatile u32 vbl_count;
static volatile int render_busy;
static u32 render_began, render_cost, render_ended;
static int shown;                         /* the buffer the copper shows */
static u8 *const screen[2] = { (u8 *)PORT_SCREEN0, (u8 *)PORT_SCREEN1 };
static u16 *const copper = (u16 *)PORT_COPPER;

static u32 mem16(u32 a) { const u8 *p = (const u8 *)a; return (u32)p[0] << 8 | p[1]; }
static u32 game_addr(u32 a) {             /* a Mega Drive address the game passed as a value -> where it is here */
	a &= 0xFFFFFF;
	return a >= 0xE00000 ? PORT_RAM + (a & 0xFFFF) : a;
}

/* ----------------------------------------------------------------- VDP */
static void vdp_data_write(u16 v) {
	switch (vdp_code & 0xF) {
	case 1:
		if (vdp_addr & 1) { vram[vdp_addr & 0xFFFE] = (u8)v; vram[(vdp_addr & 0xFFFE) + 1] = (u8)(v >> 8); }
		else { vram[vdp_addr & 0xFFFE] = (u8)(v >> 8); vram[(vdp_addr & 0xFFFE) + 1] = (u8)v; }
		break;
	case 3: cram[(vdp_addr >> 1) & 63] = v & 0xEEE; palette_dirty = 1; break;
	case 5: vsram[(vdp_addr >> 1) % 40] = v & 0x7FF; break;
	}
	vdp_addr = (vdp_addr + reg[15]) & 0xFFFF;
	if (dma_fill_pending) {
		dma_fill_pending = 0;
		u32 len = reg[19] | (u32)reg[20] << 8;
		if (!len) len = 0x10000;
		u32 a = (vdp_addr - reg[15]) & 0xFFFF;
		vram[a ^ 1] = (u8)v;
		do { vram[a ^ 1] = (u8)(v >> 8); a = (a + reg[15]) & 0xFFFF; } while (--len);
		vdp_addr = a;
	}
}

static u16 vdp_data_read(void) {
	u16 v = 0;
	switch (vdp_code & 0xF) {
	case 0: v = (u16)(vram[vdp_addr & 0xFFFE] << 8 | vram[(vdp_addr & 0xFFFE) + 1]); break;
	case 8: v = cram[(vdp_addr >> 1) & 63]; break;
	case 4: v = vsram[(vdp_addr >> 1) % 40]; break;
	}
	vdp_addr = (vdp_addr + reg[15]) & 0xFFFF;
	return v;
}

static void dma_run(void) {
	u32 len = reg[19] | (u32)reg[20] << 8;
	if (!len) len = 0x10000;
	int mode = reg[23] >> 6;
	if (mode < 2) {
		u32 src = ((u32)(reg[23] & 0x7F) << 17 | (u32)reg[22] << 9 | (u32)reg[21] << 1);
		int inc = reg[15];
		if ((vdp_code & 0xF) == 1 && inc == 2 && !(vdp_addr & 1)) {   /* the usual case: words into VRAM, straight across */
			const u8 *p = (const u8 *)game_addr(src);
			u32 a = vdp_addr;
			for (u32 i = 0; i < len; i++) { vram[a & 0xFFFF] = p[0]; vram[(a + 1) & 0xFFFF] = p[1]; p += 2; a += 2; }
			vdp_addr = a & 0xFFFF;
			src += len * 2;
		} else {
			for (u32 i = 0; i < len; i++) {
				u16 v = (u16)mem16(game_addr(src));
				src += 2;
				int save = dma_fill_pending; dma_fill_pending = 0;
				vdp_data_write(v);
				dma_fill_pending = save;
			}
		}
		reg[21] = (u8)(src >> 1); reg[22] = (u8)(src >> 9); reg[23] = (u8)((reg[23] & 0x80) | ((src >> 17) & 0x7F));
		reg[19] = reg[20] = 0;
	} else if (mode == 2) {
		dma_fill_pending = 1;
	} else {
		u32 src = (u32)reg[22] << 8 | reg[21];
		u32 a = vdp_addr & 0xFFFF;
		for (u32 i = 0; i < len; i++) { vram[a ^ 1] = vram[(src & 0xFFFF) ^ 1]; src++; a = (a + reg[15]) & 0xFFFF; }
		vdp_addr = a;
	}
}

static void vdp_ctrl_write(u16 v) {
	if (!ctrl_pending && (v & 0xC000) == 0x8000) {
		int r = (v >> 8) & 0x1F;
		if (r < 24) reg[r] = (u8)v;
		vdp_code &= ~3UL;
		return;
	}
	if (!ctrl_pending) {
		vdp_code = (vdp_code & 0x3C) | (v >> 14);
		vdp_addr = (vdp_addr & 0xC000) | (v & 0x3FFF);
		ctrl_pending = 1;
	} else {
		vdp_code = (vdp_code & 3) | ((v >> 2) & 0x3C);
		vdp_addr = (vdp_addr & 0x3FFF) | ((v & 3) << 14);
		ctrl_pending = 0;
		if ((vdp_code & 0x20) && (reg[1] & 0x10)) dma_run();
	}
}

/* the beam, as the Mega Drive's line: the Amiga's display starts at line $2C */
static int beam_line(void) {
	u32 v = ((u32)(CUSTOM(0x004) & 1) << 8) | (CUSTOM(0x006) >> 8);
	int l = (int)v - 0x2C;
	return l < 0 ? l + 312 : l;
}

static u8 pad_read(int p) {
	u16 s = pad_state[p];
	u8 v;
	if (pad_th[p]) v = (u8)(0x40 | ((s >> 5 & 1) << 5) | ((s >> 4 & 1) << 4) | (s & 0xF));
	else v = (u8)(((s >> 7 & 1) << 5) | ((s >> 6 & 1) << 4) | (s & 3));
	return (u8)((~v & 0x7F) | (pad_th[p] ? 0x40 : 0));
}

static u32 io_read(u32 a, int bits) {
	if (a >= 0xC00000 && a < 0xC00010) {
		u32 v;
		switch (a & 0xE) {
		case 0x0: case 0x2: v = vdp_data_read(); ctrl_pending = 0; break;
		case 0x4: case 0x6: {
			int l = beam_line();
			v = status | 1;                 /* (PAL) */
			if (l >= H) v |= 0x0008;
			if ((CUSTOM(0x006) & 0xFF) > 0xB0) v |= 0x0004;
			ctrl_pending = 0;
			break;
		}
		default: { int l = beam_line(); if (l > 0x102) l -= 56; v = (u32)(l & 0xFF) << 8 | (CUSTOM(0x006) & 0xFF); break; }
		}
		return bits == 8 ? ((a & 1) ? v & 0xFF : v >> 8) : v;
	}
	if (a >= 0xA10000 && a < 0xA10020) {
		u32 v = 0;
		switch (a & 0x1E) {
		case 0x00: v = 0xE0; break;         /* overseas, PAL */
		case 0x02: v = pad_read(0); break;
		case 0x04: v = pad_read(1); break;
		case 0x06: v = 0x7F; break;
		case 0x08: v = pad_ctrl[0]; break;
		case 0x0A: v = pad_ctrl[1]; break;
		}
		return bits == 8 ? v : v << 8 | v;
	}
	return 0;                             /* the Z80's bus is always granted; its side reads 0 */
}

static void io_write(u32 a, u32 v, int bits) {
	if (a >= 0xC00000 && a < 0xC00010) {
		if (bits == 8) v = (v & 0xFF) * 0x101;
		switch (a & 0xE) {
		case 0x0: case 0x2: ctrl_pending = 0; vdp_data_write((u16)v); break;
		case 0x4: case 0x6: vdp_ctrl_write((u16)v); break;
		}
		return;
	}
	if (a >= 0xA10000 && a < 0xA10020) {
		switch (a & 0x1E) {
		case 0x02: pad_th[0] = (v >> 6) & 1; break;
		case 0x04: pad_th[1] = (v >> 6) & 1; break;
		case 0x08: pad_ctrl[0] = (u8)v; break;
		case 0x0A: pad_ctrl[1] = (u8)v; break;
		}
	}
	/* the Z80, its RAM, the sound chips: nothing yet */
}

/* the rewritten code's calls: the value goes through the cell at the layer's page */
void sh_r(u32 a, int bits) {
	u8 *cell = (u8 *)PORT_SHIM;
	u32 v;
	a &= 0xFFFFFF;
	if (a >= 0xA00000 && a < 0xE00000) v = bits == 32 ? io_read(a, 16) << 16 | io_read(a + 2, 16) : io_read(a, bits);
	else {
		const u8 *p = (const u8 *)game_addr(a);
		v = bits == 8 ? p[0] : bits == 16 ? (u32)p[0] << 8 | p[1] : (u32)p[0] << 24 | (u32)p[1] << 16 | (u32)p[2] << 8 | p[3];
	}
	if (bits == 8) cell[0] = (u8)v;
	else if (bits == 16) { cell[0] = (u8)(v >> 8); cell[1] = (u8)v; }
	else { cell[0] = (u8)(v >> 24); cell[1] = (u8)(v >> 16); cell[2] = (u8)(v >> 8); cell[3] = (u8)v; }
}

void sh_w(u32 a, int bits) {
	const u8 *cell = (const u8 *)PORT_SHIM;
	u32 v = bits == 8 ? cell[0] : bits == 16 ? (u32)cell[0] << 8 | cell[1]
	      : (u32)cell[0] << 24 | (u32)cell[1] << 16 | (u32)cell[2] << 8 | cell[3];
	a &= 0xFFFFFF;
	if (a >= 0xA00000 && a < 0xE00000) {
		if (bits == 32) { io_write(a, v >> 16, 16); io_write(a + 2, v & 0xFFFF, 16); } else io_write(a, v, bits);
	} else {
		u8 *p = (u8 *)game_addr(a);
		if (bits == 8) p[0] = (u8)v;
		else if (bits == 16) { p[0] = (u8)(v >> 8); p[1] = (u8)v; }
		else { p[0] = (u8)(v >> 24); p[1] = (u8)(v >> 16); p[2] = (u8)(v >> 8); p[3] = (u8)v; }
	}
}

/* ------------------------------------------------------------- render */
/* What a frame is drawn from: the registers, the scroll and the sprite table as they stood when the
 * game's vertical interrupt finished, and again after its horizontal interrupt if it has one (a
 * split screen, a water line): the lines from there down use the second. The tiles and the name
 * tables are read from VRAM as it is. */
typedef struct { u8 reg[24]; u16 vsram[40]; u8 sat[640]; u8 hs[1024]; int line; } snap_t;
static snap_t snap_now[2], snap_draw[2];
static int snap_split, snap_split_now;    /* the horizontal interrupt ran last frame (its state is still there); this frame */
static const u8 *rreg, *rsat, *rhs;
static const u16 *rvsram;

void layer_snap(int which) {
	snap_t *n = &snap_now[which];
	int i;
	for (i = 0; i < 24; i++) n->reg[i] = reg[i];
	for (i = 0; i < 40; i++) n->vsram[i] = vsram[i];
	const u8 *p = &vram[(u32)(reg[5] & 0x7F) << 9];
	for (i = 0; i < 640; i++) n->sat[i] = p[i];
	p = &vram[(u32)(reg[13] & 0x3F) << 10];
	for (i = 0; i < 1024; i++) n->hs[i] = p[i];
	n->line = reg[10];
	if (which) snap_split_now = 1;
}
static void use_snap(const snap_t *n) { rreg = n->reg; rsat = n->sat; rhs = n->hs; rvsram = n->vsram; }
static int h40(void) { return rreg[12] & 1; }
static int ilace2(void) { return (rreg[12] & 6) == 6; }

static void plane_line(int y, int plane, u8 *out, int width) {
	static const int sizes[4] = { 32, 64, 32, 128 };
	int pw = sizes[rreg[16] & 3], ph = sizes[(rreg[16] >> 4) & 3];
	u32 nt = plane == 0 ? (u32)(rreg[2] & 0x38) << 10 : (u32)(rreg[4] & 7) << 13;
	int hs_mode = rreg[11] & 3;
	int il = ilace2(), th = il ? 16 : 8, yy = il ? y * 2 : y, tsh = il ? 4 : 3;
	u32 hs_off = hs_mode == 0 ? 0 : hs_mode == 3 ? (u32)y * 4 : (u32)(y & ~7) * 4;
	if (il) hs_off = hs_mode == 0 ? 0 : hs_mode == 3 ? (u32)yy * 4 : (u32)(yy & ~15) * 4;
	const u8 *hp = &rhs[(hs_off + plane * 2) & 0x3FE];
	int hscroll = (hp[0] << 8 | hp[1]) & 0x3FF;
	int vs_2cell = rreg[11] & 4;
	int x = 0;
	while (x < width) {
		int vscroll = rvsram[vs_2cell ? (((x >> 4) * 2 + plane) % 40) : plane] & (il ? 0x7FF : 0x3FF);
		int px = (x - hscroll) & (pw * 8 - 1);
		int py = (yy + vscroll) & (ph * th - 1);
		u32 e = nt + (u32)((py >> tsh) * pw + (px >> 3)) * 2;
		u16 entry = (u16)(vram[e & 0xFFFF] << 8 | vram[(e + 1) & 0xFFFF]);
		int tile = entry & 0x7FF, pal = (entry >> 9) & 0x30, pri = entry & 0x8000 ? 0x40 : 0;
		int ty = entry & 0x1000 ? th - 1 - (py & (th - 1)) : py & (th - 1);
		const u8 *row = &vram[(tile * 4 * th + ty * 4) & 0xFFFF];
		int n = 8 - (px & 7);             /* the rest of this cell */
		if (n > width - x) n = width - x;
		if (!(row[0] | row[1] | row[2] | row[3])) { while (n--) out[x++] = 0; continue; }
		int tx = px & 7;
		if (entry & 0x800) {
			while (n--) { int fx = 7 - tx; u8 b = row[fx >> 1]; int c = (fx & 1) ? b & 0xF : b >> 4; out[x++] = c ? (u8)(pal | c | pri) : 0; tx++; }
		} else {
			while (n--) { u8 b = row[tx >> 1]; int c = (tx & 1) ? b & 0xF : b >> 4; out[x++] = c ? (u8)(pal | c | pri) : 0; tx++; }
		}
	}
}

static void window_line(int y, u8 *a, int width) {
	int wx = (rreg[17] & 0x1F) * 16, wright = rreg[17] & 0x80;
	int wy = (rreg[18] & 0x1F) * 8, wdown = rreg[18] & 0x80;
	int in_y = wdown ? y >= wy : y < wy;
	int x0, x1;
	if (in_y && (wy || wdown)) { x0 = 0; x1 = width; }
	else if (wright) { x0 = wx; x1 = width; }
	else { x0 = 0; x1 = wx; }
	if (x0 >= x1) return;
	int ww = h40() ? 64 : 32;
	int il = ilace2(), th = il ? 16 : 8, yy = il ? y * 2 : y;
	u32 nt = h40() ? (u32)(rreg[3] & 0x3C) << 10 : (u32)(rreg[3] & 0x3E) << 10;
	for (int x = x0; x < x1 && x < width; x++) {
		u32 e = nt + (u32)((yy / th) * ww + (x >> 3)) * 2;
		u16 entry = (u16)(vram[e & 0xFFFF] << 8 | vram[(e + 1) & 0xFFFF]);
		int tile = entry & 0x7FF, hf = entry & 0x800, vf = entry & 0x1000, pal = (entry >> 9) & 0x30, pri = entry & 0x8000 ? 0x40 : 0;
		int ty = vf ? th - 1 - (yy & (th - 1)) : yy & (th - 1), tx = hf ? 7 - (x & 7) : x & 7;
		u8 b = vram[(tile * 4 * th + ty * 4 + (tx >> 1)) & 0xFFFF];
		int c = (tx & 1) ? b & 0xF : b >> 4;
		a[x] = c ? (u8)(pal | c | pri) : 0;
	}
}

/* the sprite list, once per frame: what the link chain says, in order */
typedef struct { s16 x, y; u8 w, h; u16 attr; } spr_t;
static spr_t spr_lists[2][80], *spr;
static int nspr_lists[2], nspr;
static void sprite_list(void) {
	int max = h40() ? 80 : 64, il = ilace2();
	nspr = 0;
	for (int link = 0, count = 0; count < max; count++) {
		const u8 *s = &rsat[link * 8];
		spr[nspr].y = (s16)(((s[0] << 8 | s[1]) & 0x3FF) - (il ? 256 : 128));
		spr[nspr].w = (u8)(((s[2] >> 2) & 3) + 1); spr[nspr].h = (u8)((s[2] & 3) + 1);
		spr[nspr].attr = (u16)(s[4] << 8 | s[5]);
		spr[nspr].x = (s16)(((s[6] << 8 | s[7]) & 0x1FF) - 128);
		nspr++;
		link = s[3] & 0x7F;
		if (!link) break;
	}
}

static int sprite_line(int y, u8 *lo, u8 *hi, int width) {
	int per_line = h40() ? 20 : 16;
	int n = 0, any = 0, masked = 0;
	int il = ilace2(), th = il ? 16 : 8, yy = il ? y * 2 : y;
	for (int i = 0; i < nspr; i++) {
		int sy = spr[i].y, h = spr[i].h, w = spr[i].w, sx = spr[i].x;
		if (yy < sy || yy >= sy + h * th) continue;
		if (sx == -128 && n) masked = 1;
		else if (!masked) {
			u16 attr = spr[i].attr;
			int row = yy - sy;
			int tile = attr & 0x7FF, hf = attr & 0x800, pal = (attr >> 9) & 0x30;
			u8 *dst = attr & 0x8000 ? hi : lo, *other = attr & 0x8000 ? lo : hi;
			if (attr & 0x1000) row = h * th - 1 - row;
			for (int cx = 0; cx < w * 8; cx++) {
				int x = sx + cx;
				if (x < 0 || x >= width) continue;
				int col = hf ? w * 8 - 1 - cx : cx;
				int t = tile + (col >> 3) * h + row / th;
				u8 b = vram[(t * 4 * th + (row & (th - 1)) * 4 + ((col & 7) >> 1)) & 0xFFFF];
				int c = (col & 1) ? b & 0xF : b >> 4;
				if (c && !dst[x] && !other[x]) dst[x] = (u8)(pal | c);
			}
			any = 1;
		}
		if (++n >= per_line) break;
	}
	return any;
}

static u32 c2p[64];                       /* a colour's six bits, one per nibble (bit 3 of each) */

void layer_render(void) {
	static u8 pa[W], pb[W], slo[W], shi[W], out[W + 8];
	u8 *dst = screen[shown ^ 1];
	int split = H, i;
	snap_draw[0] = snap_now[0];
	if (snap_split) { snap_draw[1] = snap_now[1]; split = snap_now[1].line; }
	for (i = 0; i < (snap_split ? 2 : 1); i++) {
		use_snap(&snap_draw[i]);
		spr = spr_lists[i]; sprite_list(); nspr_lists[i] = nspr;
	}
	for (int y = 0; y < H; y++) {
		int x, part = y >= split;
		use_snap(&snap_draw[part]);
		spr = spr_lists[part]; nspr = nspr_lists[part];
		int width = h40() ? 320 : 256, x0 = h40() ? 0 : 32;
		u8 bg = rreg[7] & 0x3F;
		if (!(rreg[1] & 0x40)) { for (x = 0; x < W; x++) out[x] = bg; }
		else {
			for (x = 0; x < W; x++) slo[x] = shi[x] = 0;
			plane_line(y, 1, pb, width);
			plane_line(y, 0, pa, width);
			if (rreg[17] | rreg[18]) window_line(y, pa, width);
			sprite_line(y, slo, shi, width);
			for (x = 0; x < x0; x++) out[x] = out[W - 1 - x] = bg;
			for (int i = 0; i < width; i++) {
				u8 a = pa[i], b = pb[i], c = bg;
				if (b & 0x3F) c = b & 0x3F;
				if ((a & 0x3F) && !(a & 0x40)) c = a & 0x3F;
				if (slo[i]) c = slo[i];
				if (b & 0x40) c = b & 0x3F;
				if (a & 0x40) c = a & 0x3F;
				if (shi[i]) c = shi[i];
				out[x0 + i] = c;
			}
		}
		/* chunky to planar: four pixels make a nibble of each plane */
		u8 *p = dst + y * ROWBYTES;
		for (x = 0; x < W; x += 8) {
			u32 hi4 = c2p[out[x]] | c2p[out[x + 1]] >> 1 | c2p[out[x + 2]] >> 2 | c2p[out[x + 3]] >> 3;
			u32 lo4 = c2p[out[x + 4]] | c2p[out[x + 5]] >> 1 | c2p[out[x + 6]] >> 2 | c2p[out[x + 7]] >> 3;
			p[0] = (u8)((hi4 & 0xF) << 4 | (lo4 & 0xF));
			p[PLANE_SIZE] = (u8)((hi4 & 0xF0) | (lo4 >> 4 & 0xF));
			p[PLANE_SIZE * 2] = (u8)((hi4 >> 4 & 0xF0) | (lo4 >> 8 & 0xF));
			p[PLANE_SIZE * 3] = (u8)((hi4 >> 8 & 0xF0) | (lo4 >> 12 & 0xF));
			p[PLANE_SIZE * 4] = (u8)((hi4 >> 12 & 0xF0) | (lo4 >> 16 & 0xF));
			p[PLANE_SIZE * 5] = (u8)((hi4 >> 16 & 0xF0) | (lo4 >> 20 & 0xF));
			p++;
		}
	}
}

/* ----------------------------------------------------------- display */
static void show(int which) {
	u32 a = (u32)screen[which];
	for (int i = 0; i < PLANES; i++) {
		copper[i * 4 + 1] = (u16)(a >> 16);
		copper[i * 4 + 3] = (u16)a;
		a += PLANE_SIZE;
	}
	shown = which;
}

/* the rest of the copper list: the game's horizontal interrupt, as a copper interrupt on its line
 * (the first one of the frame: the counter's reloads aren't followed) */
static void copper_tail(void) {
	u16 *c = copper + PLANES * 4;
	if (reg[0] & 0x10) {
		int l = 0x2C + reg[10];
		if (l > 255) { *c++ = 0xFFDF; *c++ = 0xFFFE; l -= 256; }
		*c++ = (u16)(l << 8 | 0x01); *c++ = 0xFFFE;
		*c++ = 0x009C; *c++ = 0x8010;
	}
	*c++ = 0xFFFF; *c++ = 0xFFFE;
}

static void load_palette(void) {
	static const u8 lev[8] = { 0, 52, 87, 116, 144, 172, 206, 255 };
	for (int pass = 0; pass < 2; pass++)
		for (int i = 0; i < 64; i++) {
			u16 v = cram[i];
			u8 r = lev[(v >> 1) & 7], g = lev[(v >> 5) & 7], b = lev[(v >> 9) & 7];
			CUSTOM(0x106) = (u16)(0x0C00 | (i >> 5) << 13 | (pass ? 0x0200 : 0));   /* BPLCON3: the bank; LOCT for the low nibbles */
			CUSTOM(0x180 + 2 * (i & 31)) = pass ? (u16)((r & 15) << 8 | (g & 15) << 4 | (b & 15))
			                                    : (u16)((r >> 4) << 8 | (g >> 4) << 4 | (b >> 4));
		}
	CUSTOM(0x106) = 0x0C00;
}

/* the keyboard: one key per vertical blank from CIA-A's serial port, acknowledged */
static void keyboard(void) {
	if (!(CIAA(0xD00) & 0x08)) return;
	u8 raw = (u8)~CIAA(0xC00);
	u8 code = (u8)(raw >> 1 | raw << 7);
	CIAA(0xE00) |= 0x40;                  /* the handshake: hold the line low for a few scanlines */
	{ u16 l = CUSTOM(0x006) >> 8; int n = 0; while (n < 3) { u16 m = CUSTOM(0x006) >> 8; if (m != l) { l = m; n++; } } }
	CIAA(0xE00) &= (u8)~0x40;
	u16 bit = 0;
	switch (code & 0x7F) {
	case 0x4C: bit = 1; break; case 0x4D: bit = 2; break; case 0x4F: bit = 4; break; case 0x4E: bit = 8; break;
	case 0x32: case 0x40: bit = 16; break;   /* X, space: B */
	case 0x33: bit = 32; break;              /* C */
	case 0x31: bit = 64; break;              /* Z: A */
	case 0x44: case 0x43: bit = 128; break;  /* Return, Enter: Start */
	}
	if (code & 0x80) keys &= (u16)~bit; else keys |= bit;
}

/* the vertical blank: flip, the palette, the pads. -> nonzero if the game's own
 * vertical interrupt is enabled (start.S then hands it the interrupt) */
int layer_vbl(void) {
	vbl_count++;
	snap_split = snap_split_now; snap_split_now = 0;
	copper_tail();
	if (palette_dirty) { palette_dirty = 0; load_palette(); }
	keyboard();
	u16 j = CUSTOM(0x00C), s = keys;
	if (j & 2) s |= 8;
	if (j & 0x200) s |= 4;
	if ((j ^ j >> 1) & 1) s |= 2;
	if ((j ^ j >> 1) & 0x100) s |= 1;
	if (!(CIAA(0) & 0x80)) s |= 16;         /* fire: B */
	if (!(CUSTOM(0x016) & 0x4000)) s |= 128;   /* the second button: Start */
	pad_state[0] = s;
	return reg[1] & 0x20;
}

/* after the game's interrupt: is there time to draw a frame? The game's main loop only runs
 * between frames being drawn, so it gets as long as the last one took */
int layer_want_render(void) {
	if (render_busy) return 0;
	if (vbl_count - render_ended < render_cost) return 0;
	render_busy = 1;
	render_began = vbl_count;
	return 1;
}
void layer_render_done(void) {
	show(shown ^ 1);
	render_cost = vbl_count - render_began;
	render_ended = vbl_count;
	render_busy = 0;
}

void layer_init(void) {
	int i;
	for (i = 0; i < 64; i++) {
		u32 v = 0;
		for (int p = 0; p < 6; p++) if (i >> p & 1) v |= 8UL << (4 * p);
		c2p[i] = v;
	}
	for (i = 0; i < 2 * PLANES * PLANE_SIZE; i++) screen[0][i] = 0;   /* (the two buffers are one after the other) */
	u16 *c = copper;
	for (i = 0; i < PLANES; i++) { *c++ = (u16)(0x0E0 + i * 4); *c++ = 0; *c++ = (u16)(0x0E2 + i * 4); *c++ = 0; }
	*c++ = 0xFFFF; *c++ = 0xFFFE;
	show(0);
	CUSTOM(0x100) = 0x6201;               /* BPLCON0: six planes, colour (ECSENA for BPLCON3) */
	CUSTOM(0x102) = 0;
	CUSTOM(0x104) = 0x0224;               /* BPLCON2: KILLEHB: six planes are 64 colours, not half-brite */
	CUSTOM(0x106) = 0x0C00;
	CUSTOM(0x10C) = 0;
	CUSTOM(0x108) = 0; CUSTOM(0x10A) = 0;
	CUSTOM(0x1FC) = 0;                    /* FMODE: plain fetches */
	CUSTOM(0x08E) = 0x2C81;               /* 320 x 224 from line $2C */
	CUSTOM(0x090) = 0x0CC1;
	CUSTOM(0x092) = 0x0038; CUSTOM(0x094) = 0x00D0;
	CUSTOM(0x080) = (u16)(PORT_COPPER >> 16); CUSTOM(0x082) = (u16)PORT_COPPER;
	CUSTOM(0x088) = 0;
	CUSTOM(0x034) = 0xFF00;               /* POTGO: the buttons' lines as inputs, pulled up */
	load_palette();
	layer_snap(0);
}
