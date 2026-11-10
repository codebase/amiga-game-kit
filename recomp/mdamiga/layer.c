/* The hardware layer of a Mega Drive game ported with agk port, on an AGA
 * Amiga: what the rewritten code's calls reach instead of the Mega Drive's
 * chips. This one makes the Amiga's hardware do what the VDP did:
 *
 *   plane A, plane B   a dual playfield, four bitplanes each. Each plane is a
 *                      bitmap of the VDP's whole 64x32-cell plane, twice
 *                      side by side so it wraps, and the hardware scrolls it:
 *                      the copper sets the bitplane pointers and the fine
 *                      scroll at the top and on every line where the game's
 *                      scroll table changes (parallax, water ripples). A cell
 *                      is drawn only when the game writes it or the tile in
 *                      it changes: eight rows copied from a cache of the
 *                      tiles in planar form.
 *   sprites            drawn into plane A's bitmap, a masked tile at a time,
 *                      where the screen will read them; the cells they cover
 *                      are put back from the tiles next frame, and cells with
 *                      the priority bit are drawn over them again.
 *   colours            fifteen per playfield (and the backdrop). The game's
 *                      61 are grouped, per playfield, by which palette lines
 *                      it uses there: the nearest colours share a slot. The
 *                      grouping holds while a fade changes the values and is
 *                      redone when it no longer fits.
 *   interrupts, pads   as in the software layer (layer_soft.c): the vertical
 *                      blank hands the game its interrupt, a copper interrupt
 *                      stands in for the horizontal one; joystick and keys.
 *
 * Not here yet: the window plane, the interlaced two-player mode, H32, mid-
 * screen register changes, hardware sprites for the player, sound.
 *
 * Called from start.S: sh_r/sh_w with interrupts off on the layer's stack,
 * layer_vbl at the vertical blank, layer_frame after the game's interrupt. */
#include "port_layout.h"

typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned long u32;
typedef signed short s16;

#define CUSTOM(off) (*(volatile u16 *)(0xDFF000 + (off)))
#define CIAA(off) (*(volatile u8 *)(0xBFE001 + (off)))
#define H 224
#define PF_ROW 128                        /* bytes per bitmap row: 1024 pixels, the 512-pixel plane twice */
#define PF_PLANE (PF_ROW * 256)           /* a bitplane: 256 rows */
#define PF_BYTES (PF_PLANE * 4)
#define CELLS 2048                        /* 64 x 32 */

static u8 vram[0x10000];
static u16 cram[64], vsram[40];
static u8 reg[24];
static u32 vdp_code, vdp_addr;
static int ctrl_pending, dma_fill_pending;
static u16 status = 0x3400;
static u8 pad_th[2], pad_ctrl[2];
static u16 pad_state[2];                  /* bits: 0 up 1 down 2 left 3 right 4 B 5 C 6 A 7 Start */
static u16 keys;
volatile u32 vbl_count;

static u8 *const pf[2] = { (u8 *)PORT_SCREEN0, (u8 *)(PORT_SCREEN0 + PF_BYTES) };   /* plane A, plane B */
static u16 *const cop[2] = { (u16 *)PORT_COPPER, (u16 *)(PORT_COPPER + 0x4000) };
static int cop_shown;

static u32 mem16(u32 a) { const u8 *p = (const u8 *)a; return (u32)p[0] << 8 | p[1]; }
static u32 game_addr(u32 a) {
	a &= 0xFFFFFF;
	return a >= 0xE00000 ? PORT_RAM + (a & 0xFFFF) : a;
}

/* ------------------------------------------- what changed, as it's written */
static u32 nt[2];                         /* the planes' name tables in VRAM */
static u8 tile_dirty[2048];
static int tiles_dirty;
static u8 cell_flag[2][CELLS];
static u16 cell_list[2][CELLS];
static int cell_n[2], cell_all[2];
static int cram_dirty = 1;

static void cell_dirty(int p, u32 cell) {
	if (!cell_flag[p][cell]) { cell_flag[p][cell] = 1; cell_list[p][cell_n[p]++] = (u16)cell; }
}
/* the VDP's planes are pw x ph cells (32, 64 or 128 each way); the bitmaps hold 64 x 32. A narrower
 * plane repeats across the bitmap; what lies beyond 64 x 32 of a bigger one isn't shown. */
static int pw_shift = 6, pw = 64, ph = 32;
static void entry_dirty(int p, u32 n) {   /* the name table entry n of plane p was written */
	u32 cy = n >> pw_shift, cx = n & (u32)(pw - 1);
	if (cy >= 32 || cx >= 64) return;
	cell_dirty(p, cy << 6 | cx);
	if (pw == 32) cell_dirty(p, cy << 6 | (cx + 32));
}
static u32 entry_of(u32 cell) {           /* a bitmap cell -> its name table entry */
	return (((cell >> 6) & (u32)(ph - 1)) << pw_shift) | (cell & 63 & (u32)(pw - 1));
}
static void vram_w(u32 a, u8 hi, u8 lo) {
	a &= 0xFFFE;
	if (vram[a] == hi && vram[a + 1] == lo) return;
	vram[a] = hi; vram[a + 1] = lo;
	tile_dirty[a >> 5] = 1; tiles_dirty = 1;
	u32 o = a - nt[0], size = (u32)(pw * ph * 2);
	if (o < size) entry_dirty(0, o >> 1);
	o = a - nt[1];
	if (o < size) entry_dirty(1, o >> 1);
}
static void vram_w8(u32 a, u8 v) {        /* a single byte (fills, copies) */
	a &= 0xFFFF;
	if (a & 1) vram_w(a, vram[a & 0xFFFE], v); else vram_w(a, v, vram[a + 1]);
}
static void tables_moved(void) {
	nt[0] = (u32)(reg[2] & 0x38) << 10;
	nt[1] = (u32)(reg[4] & 7) << 13;
	static const u8 shifts[4] = { 5, 6, 5, 7 };
	pw_shift = shifts[reg[16] & 3]; pw = 1 << pw_shift;
	ph = 1 << shifts[(reg[16] >> 4) & 3];
	cell_all[0] = cell_all[1] = 1;
}

/* ----------------------------------------------------------------- VDP */
static void vdp_data_write(u16 v) {
	switch (vdp_code & 0xF) {
	case 1:
		if (vdp_addr & 1) vram_w(vdp_addr, (u8)v, (u8)(v >> 8)); else vram_w(vdp_addr, (u8)(v >> 8), (u8)v);
		break;
	case 3: { u16 n = v & 0xEEE, *c = &cram[(vdp_addr >> 1) & 63]; if (*c != n) { *c = n; cram_dirty = 1; } break; }
	case 5: vsram[(vdp_addr >> 1) % 40] = v & 0x7FF; break;
	}
	vdp_addr = (vdp_addr + reg[15]) & 0xFFFF;
	if (dma_fill_pending) {
		dma_fill_pending = 0;
		u32 len = reg[19] | (u32)reg[20] << 8;
		if (!len) len = 0x10000;
		u32 a = (vdp_addr - reg[15]) & 0xFFFF;
		vram_w8(a ^ 1, (u8)v);
		do { vram_w8(a ^ 1, (u8)(v >> 8)); a = (a + reg[15]) & 0xFFFF; } while (--len);
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
		if ((vdp_code & 0xF) == 1 && reg[15] == 2 && !(vdp_addr & 1)) {   /* the usual case: words into VRAM */
			const u8 *p = (const u8 *)game_addr(src);
			u32 a = vdp_addr;
			for (u32 i = 0; i < len; i++) { vram_w(a, p[0], p[1]); p += 2; a += 2; }
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
		for (u32 i = 0; i < len; i++) { vram_w8(a ^ 1, vram[(src & 0xFFFF) ^ 1]); src++; a = (a + reg[15]) & 0xFFFF; }
		vdp_addr = a;
	}
}

static void vdp_ctrl_write(u16 v) {
	if (!ctrl_pending && (v & 0xC000) == 0x8000) {
		int r = (v >> 8) & 0x1F;
		if (r < 24 && reg[r] != (u8)v) {
			reg[r] = (u8)v;
			if (r == 2 || r == 4 || r == 16) tables_moved();
			if (r == 7) cram_dirty = 1;
		}
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
			v = status | 1;
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
		case 0x00: v = 0xE0; break;
		case 0x02: v = pad_read(0); break;
		case 0x04: v = pad_read(1); break;
		case 0x06: v = 0x7F; break;
		case 0x08: v = pad_ctrl[0]; break;
		case 0x0A: v = pad_ctrl[1]; break;
		}
		return bits == 8 ? v : v << 8 | v;
	}
	return 0;
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
}

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

/* ------------------------------------------------------------ colours */
/* Each playfield has slots 1..15. map[p][i] is the slot of the game's colour i
 * (0 for the transparent ones); a slot shows the average of its colours. */
static u8 map[2][64];
static u8 lines_used[2];                  /* the palette lines the grouping covered */
static u32 grouped_at[2];
static const u8 lev[8] = { 0, 52, 87, 116, 144, 172, 206, 255 };

static int dist(u16 a, u16 b) {           /* two CRAM words */
	int dr = (int)(a >> 1 & 7) - (int)(b >> 1 & 7), dg = (int)(a >> 5 & 7) - (int)(b >> 5 & 7), db = (int)(a >> 9 & 7) - (int)(b >> 9 & 7);
	return dr * dr * 3 + dg * dg * 4 + db * db * 2;
}

static u8 used_lines(int p) {
	u8 used = 0;
	const u8 *t = &vram[nt[p] & 0xFFFF];
	int count = pw * ph > 4096 ? 4096 : pw * ph;
	for (int i = 0; i < count && used != 15; i++, t += 2) if ((t[0] & 7) | t[1]) used |= (u8)(1 << ((t[0] >> 5) & 3));
	if (p == 0) {                         /* the sprites are drawn into plane A */
		const u8 *s = &vram[(u32)(reg[5] & 0x7F) << 9];
		for (int link = 0, n = 0; n < 80; n++) {
			const u8 *e = s + link * 8;
			used |= (u8)(1 << ((e[4] >> 5) & 3));
			link = e[3] & 0x7F;
			if (!link) break;
		}
	}
	return used ? used : 1;
}

/* group playfield p's colours into 15 slots: fifteen colours picked to be as far from each other
 * as possible (the first, then whichever is farthest from all picked so far), and every colour
 * with the nearest of them. -> nonzero if the grouping changed */
static int group(int p) {
	static u8 member[64], near[64];
	static int far[64];
	u8 rep[15], old[64];
	int count = 0, reps = 0, i, k;
	u8 used = used_lines(p);
	for (i = 0; i < 64; i++) {
		old[i] = map[p][i];
		map[p][i] = 0;
		if ((i & 15) && (used >> (i >> 4) & 1)) member[count++] = (u8)i;
	}
	lines_used[p] = used;
	grouped_at[p] = vbl_count;
	if (count) {
		for (i = 0; i < count; i++) { far[i] = 1 << 30; near[i] = 0; }
		int next = 0;
		while (reps < 15) {
			rep[reps] = member[next];
			int best = 0, bi = -1;
			for (i = 0; i < count; i++) {
				int d = dist(cram[member[i]], cram[rep[reps]]);
				if (d < far[i]) { far[i] = d; near[i] = (u8)reps; }
				if (far[i] > best) { best = far[i]; bi = i; }
			}
			reps++;
			if (bi < 0) break;            /* every colour is one of the picked ones already */
			next = bi;
		}
		for (i = 0; i < count; i++) map[p][member[i]] = (u8)(near[i] + 1);
	}
	for (k = 0, i = 0; i < 64; i++) k |= old[i] ^ map[p][i];
	return k;
}

static void set_colour(int i, int r, int g, int b) {   /* 8 bits each */
	CUSTOM(0x106) = (u16)(0x1000 | (i >> 5) << 13);     /* BPLCON3: the bank; the second playfield's colours from 16 */
	CUSTOM(0x180 + 2 * (i & 31)) = (u16)((r >> 4) << 8 | (g >> 4) << 4 | (b >> 4));
	CUSTOM(0x106) = (u16)(0x1000 | (i >> 5) << 13 | 0x0200);
	CUSTOM(0x180 + 2 * (i & 31)) = (u16)((r & 15) << 8 | (g & 15) << 4 | (b & 15));
}

/* the slots' colours from CRAM as it stands; -> the worst a colour is off from its slot, per playfield */
static void load_palette(int *err) {
	u16 bg = cram[reg[7] & 63];
	set_colour(0, lev[bg >> 1 & 7], lev[bg >> 5 & 7], lev[bg >> 9 & 7]);
	for (int p = 0; p < 2; p++) {
		int r[16], g[16], b[16], n[16], i;
		for (i = 0; i < 16; i++) r[i] = g[i] = b[i] = n[i] = 0;
		for (i = 0; i < 64; i++) {
			int s = map[p][i];
			if (s) { u16 c = cram[i]; r[s] += lev[c >> 1 & 7]; g[s] += lev[c >> 5 & 7]; b[s] += lev[c >> 9 & 7]; n[s]++; }
		}
		for (i = 1; i < 16; i++) if (n[i]) { r[i] /= n[i]; g[i] /= n[i]; b[i] /= n[i]; set_colour(p * 16 + i, r[i], g[i], b[i]); }
		int worst = 0;
		for (i = 0; i < 64; i++) {
			int s = map[p][i];
			if (s) {
				u16 c = cram[i];
				int dr = lev[c >> 1 & 7] - r[s], dg = lev[c >> 5 & 7] - g[s], db = lev[c >> 9 & 7] - b[s];
				int d = (dr < 0 ? -dr : dr) + (dg < 0 ? -dg : dg) + (db < 0 ? -db : db);
				if (d > worst) worst = d;
			}
		}
		err[p] = worst;
	}
	CUSTOM(0x106) = 0x1000;
}

/* ----------------------------------------------------------- the tiles */
/* cache[p][tile][line]: the tile in playfield p's slots, planar: 8 rows of 4 plane bytes and a mask */
typedef struct { u8 row[8][4]; u8 mask[8]; } ptile_t;
static ptile_t cache[2][2048][4];
static u8 cache_ok[2][2048];              /* a bit per palette line */
static u8 tile_in_nt[2048];               /* the tile was drawn in a cell (so a change to it means cells to redraw) */
static u8 rev[256];

static const ptile_t *get_tile(int p, int tile, int line) {
	ptile_t *t = &cache[p][tile][line];
	if (cache_ok[p][tile] >> line & 1) return t;
	const u8 *src = &vram[tile * 32];
	const u8 *m = &map[p][line * 16];
	for (int y = 0; y < 8; y++, src += 4) {
		u8 p0 = 0, p1 = 0, p2 = 0, p3 = 0;
		for (int x = 0; x < 4; x++) {
			u8 s = m[src[x] >> 4];
			p0 = (u8)(p0 << 1 | (s & 1)); p1 = (u8)(p1 << 1 | (s >> 1 & 1)); p2 = (u8)(p2 << 1 | (s >> 2 & 1)); p3 = (u8)(p3 << 1 | (s >> 3));
			s = m[src[x] & 15];
			p0 = (u8)(p0 << 1 | (s & 1)); p1 = (u8)(p1 << 1 | (s >> 1 & 1)); p2 = (u8)(p2 << 1 | (s >> 2 & 1)); p3 = (u8)(p3 << 1 | (s >> 3));
		}
		t->row[y][0] = p0; t->row[y][1] = p1; t->row[y][2] = p2; t->row[y][3] = p3;
		t->mask[y] = (u8)(p0 | p1 | p2 | p3);
	}
	cache_ok[p][tile] |= (u8)(1 << line);
	return t;
}

/* a cell of playfield p from its name table entry, at both copies; over = keep what the tile leaves clear */
static void draw_cell(int p, u32 cell, int over) {
	const u8 *e = &vram[(nt[p] + entry_of(cell) * 2) & 0xFFFF];
	int tile = (e[0] & 7) << 8 | e[1], line = (e[0] >> 5) & 3, hf = e[0] & 8, vf = e[0] & 16;
	const ptile_t *t = get_tile(p, tile, line);
	u8 *d = pf[p] + (cell >> 6) * 8 * PF_ROW + (cell & 63);
	tile_in_nt[tile] = 1;
	for (int y = 0; y < 8; y++, d += PF_ROW) {
		const u8 *r = t->row[vf ? 7 - y : y];
		u8 b0 = r[0], b1 = r[1], b2 = r[2], b3 = r[3];
		if (hf) { b0 = rev[b0]; b1 = rev[b1]; b2 = rev[b2]; b3 = rev[b3]; }
		if (over) {
			u8 k = (u8)~(b0 | b1 | b2 | b3);
			d[0] = (u8)((d[0] & k) | b0); d[64] = (u8)((d[64] & k) | b0);
			d[PF_PLANE] = (u8)((d[PF_PLANE] & k) | b1); d[PF_PLANE + 64] = (u8)((d[PF_PLANE + 64] & k) | b1);
			d[PF_PLANE * 2] = (u8)((d[PF_PLANE * 2] & k) | b2); d[PF_PLANE * 2 + 64] = (u8)((d[PF_PLANE * 2 + 64] & k) | b2);
			d[PF_PLANE * 3] = (u8)((d[PF_PLANE * 3] & k) | b3); d[PF_PLANE * 3 + 64] = (u8)((d[PF_PLANE * 3 + 64] & k) | b3);
		} else {
			d[0] = b0; d[64] = b0;
			d[PF_PLANE] = b1; d[PF_PLANE + 64] = b1;
			d[PF_PLANE * 2] = b2; d[PF_PLANE * 2 + 64] = b2;
			d[PF_PLANE * 3] = b3; d[PF_PLANE * 3 + 64] = b3;
		}
	}
}

/* ------------------------------------------------- the frame's geometry */
static u16 sx[2][H];                      /* per line: the bitmap x the screen's left edge shows, 16..527 */
static u8 row_of[2][H];                   /* per line: the bitmap row */

static void geometry(void) {
	const u8 *hs = &vram[(u32)(reg[13] & 0x3F) << 10];
	int mode = reg[11] & 3;
	for (int p = 0; p < 2; p++) {
		int vs = vsram[p];
		for (int y = 0; y < H; y++) {
			const u8 *e = hs + (mode == 0 ? 0 : mode == 3 ? y * 4 : (y & ~7) * 4) + p * 2;
			int x = (0 - (e[0] << 8 | e[1])) & 511;
			if (x < 16) x += 512;
			sx[p][y] = (u16)x;
			row_of[p][y] = (u8)(y + vs);
		}
	}
}

/* the copper list for this frame: the pointers and the fine scroll at the top, and again on every
 * line where they don't simply carry on; the game's horizontal interrupt as a copper interrupt */
static void build_copper(void) {
	u16 *c = cop[cop_shown ^ 1];
	u32 ptr[2], want[2];
	int fine[2], con1, past255 = 0, hint = (reg[0] & 0x10) ? reg[10] : -1;
	*c++ = 0x100; *c++ = (reg[1] & 0x40) ? 0x0611 : 0x0201;   /* BPLCON0: eight planes as two playfields, or nothing */
	for (int y = 0; y < H; y++) {
		for (int p = 0; p < 2; p++) {
			int x = sx[p][y];
			want[p] = (u32)pf[p] + (u32)row_of[p][y] * PF_ROW + (u32)((x - 1) >> 4) * 2;
			fine[p] = (16 - (x & 15)) & 15;
		}
		int c1 = fine[1] << 4 | fine[0];
		int moveA = y == 0 || want[0] != ptr[0] + PF_ROW, moveB = y == 0 || want[1] != ptr[1] + PF_ROW;
		int movec = y == 0 || c1 != con1, irq = y && y - 1 == hint;
		if (y && (moveA || moveB || movec || irq)) {   /* after line y-1's fetch, before line y's */
			int l = 0x2C + y - 1;
			if (l >= 256 && !past255) { *c++ = 0xFFDF; *c++ = 0xFFFE; past255 = 1; }
			if (l != 255) { *c++ = (u16)((l & 0xFF) << 8 | 0xDB); *c++ = 0xFFFE; }
			else { *c++ = 0xFFDF; *c++ = 0xFFFE; past255 = 1; }
		}
		if (irq) { *c++ = 0x009C; *c++ = 0x8010; }
		if (movec) { *c++ = 0x102; *c++ = (u16)c1; }
		for (int p = 0; p < 2; p++) if (p ? moveB : moveA) {
			u32 a = want[p];
			for (int k = 0; k < 4; k++, a += PF_PLANE) {
				int r = 0x0E0 + (k * 2 + p) * 4;       /* plane A is bitplanes 1,3,5,7; plane B 2,4,6,8 */
				*c++ = (u16)r; *c++ = (u16)(a >> 16); *c++ = (u16)(r + 2); *c++ = (u16)a;
			}
		}
		ptr[0] = want[0]; ptr[1] = want[1]; con1 = c1;
	}
	*c++ = 0xFFFF; *c++ = 0xFFFE;
	cop_shown ^= 1;
	CUSTOM(0x080) = (u16)((u32)cop[cop_shown] >> 16); CUSTOM(0x082) = (u16)(u32)cop[cop_shown];
	if (beam_line() >= H) CUSTOM(0x088) = 0;   /* still in the blank: this frame already */
}

/* ------------------------------------------------------------- sprites */
static u16 bob_list[CELLS], over_list[CELLS];
static u8 bob_flag[CELLS];
static int bob_n, over_n;

static void sprites(void) {
	static struct { s16 x, y; u8 w, h; u16 attr; } sp[80];
	const u8 *table = &vram[(u32)(reg[5] & 0x7F) << 9];
	int n = 0, i;
	for (int link = 0; n < 80;) {
		const u8 *s = table + link * 8;
		sp[n].y = (s16)(((s[0] << 8 | s[1]) & 0x3FF) - 128);
		sp[n].w = (u8)(((s[2] >> 2) & 3) + 1); sp[n].h = (u8)((s[2] & 3) + 1);
		sp[n].attr = (u16)(s[4] << 8 | s[5]);
		sp[n].x = (s16)(((s[6] << 8 | s[7]) & 0x1FF) - 128);
		n++;
		link = s[3] & 0x7F;
		if (!link) break;
	}
	const u8 *nta = &vram[nt[0] & 0xFFFF];
	u8 *base = pf[0];
	for (i = n - 1; i >= 0; i--) {        /* the first in the list is on top: drawn last */
		int w = sp[i].w, h = sp[i].h, X0 = sp[i].x, Y0 = sp[i].y;
		u16 attr = sp[i].attr;
		if (X0 <= -w * 8 || X0 >= 320 || Y0 <= -h * 8 || Y0 >= H) continue;
		int tile0 = attr & 0x7FF, line = (attr >> 13) & 3, hf = attr & 0x800, vf = attr & 0x1000;
		for (int cx = 0; cx < w; cx++) {
			int X = X0 + cx * 8;
			if (X <= -8 || X >= 320) continue;
			int col = hf ? w - 1 - cx : cx;
			for (int cy = 0; cy < h; cy++) {
				int Y = Y0 + cy * 8;
				if (Y <= -8 || Y >= H) continue;
				const ptile_t *t = get_tile(0, (tile0 + col * h + (vf ? h - 1 - cy : cy)) & 0x7FF, line);
				u32 last = 0xFFFFFFFF;
				for (int r = 0; r < 8; r++) {
					int yy = Y + r;
					if (yy < 0 || yy >= H) continue;
					u8 m = t->mask[vf ? 7 - r : r];
					if (!m) continue;
					const u8 *src = t->row[vf ? 7 - r : r];
					int bx = sx[0][yy] + X, brow = row_of[0][yy];
					if (bx < 0) continue;
					int sh = bx & 7;
					u8 *d = base + brow * PF_ROW + (bx >> 3);
					u8 b0 = src[0], b1 = src[1], b2 = src[2], b3 = src[3];
					if (hf) { m = rev[m]; b0 = rev[b0]; b1 = rev[b1]; b2 = rev[b2]; b3 = rev[b3]; }
					u16 k = (u16)~((m << 8) >> sh);
					u16 v;
					v = (u16)(d[0] << 8 | d[1]); v = (u16)((v & k) | ((b0 << 8) >> sh)); d[0] = (u8)(v >> 8); d[1] = (u8)v; d += PF_PLANE;
					v = (u16)(d[0] << 8 | d[1]); v = (u16)((v & k) | ((b1 << 8) >> sh)); d[0] = (u8)(v >> 8); d[1] = (u8)v; d += PF_PLANE;
					v = (u16)(d[0] << 8 | d[1]); v = (u16)((v & k) | ((b2 << 8) >> sh)); d[0] = (u8)(v >> 8); d[1] = (u8)v; d += PF_PLANE;
					v = (u16)(d[0] << 8 | d[1]); v = (u16)((v & k) | ((b3 << 8) >> sh)); d[0] = (u8)(v >> 8); d[1] = (u8)v;
					/* the cells this row touched: to put back next frame, and to draw over it now if they have priority */
					u32 cell = (u32)(brow >> 3) << 6 | ((bx >> 3) & 63);
					if (cell != last) {
						last = cell;
						for (int e = 0; e < (sh ? 2 : 1); e++) {
							u32 cl = (cell & ~63UL) | ((cell + e) & 63);
							if (!bob_flag[cl]) {
								bob_flag[cl] = 1; bob_list[bob_n++] = (u16)cl;
								if (nta[entry_of(cl) * 2] & 0x80) over_list[over_n++] = (u16)cl;
							}
						}
					}
				}
			}
		}
	}
	for (i = 0; i < over_n; i++) draw_cell(0, over_list[i], 1);
	over_n = 0;
}

/* ------------------------------------------------------------ the frame */
/* after the game's vertical interrupt, interrupts masked: the display for this frame, then the
 * cells and the sprites */
static int rest;                          /* frames to sit out: the last one drawn took longer than a frame */
void layer_frame(void) {
	int p, i;
	if (rest) { rest--; return; }         /* (the game's main loop needs the time; the picture stays as it is) */
	geometry();
	build_copper();
	if (cram_dirty) {
		int err[2];
		cram_dirty = 0;
		load_palette(err);
		for (p = 0; p < 2; p++) {
			u8 used = used_lines(p);
			if (((used & ~lines_used[p]) || err[p] > 60) && vbl_count - grouped_at[p] >= 25 && group(p)) {
				for (i = 0; i < 2048; i++) cache_ok[p][i] = 0;
				cell_all[p] = 1;
				cram_dirty = 1;             /* (the new slots' colours next frame) */
			}
		}
	} else if (!(vbl_count & 31)) {       /* now and then: has a playfield started using another palette line? */
		for (p = 0; p < 2; p++) if (used_lines(p) & ~lines_used[p]) cram_dirty = 1;
	}
	if (tiles_dirty) {                    /* tiles the game rewrote: their planar copies go, and the cells showing them */
		int scan = 0;
		tiles_dirty = 0;
		for (i = 0; i < 2048; i++) if (tile_dirty[i]) { cache_ok[0][i] = cache_ok[1][i] = 0; if (tile_in_nt[i]) scan = 1; else tile_dirty[i] = 0; }
		if (scan) {
			for (p = 0; p < 2; p++) {
				const u8 *t = &vram[nt[p] & 0xFFFF];
				int count = pw * ph > 4096 ? 4096 : pw * ph;
				for (i = 0; i < count; i++, t += 2) if (tile_dirty[(t[0] & 7) << 8 | t[1]]) entry_dirty(p, i);
			}
			for (i = 0; i < 2048; i++) tile_dirty[i] = 0;
		}
	}
	for (i = 0; i < bob_n; i++) { bob_flag[bob_list[i]] = 0; cell_dirty(0, bob_list[i]); }   /* under last frame's sprites */
	bob_n = 0;
	for (p = 0; p < 2; p++) {
		if (cell_all[p]) {
			cell_all[p] = 0;
			for (i = 0; i < CELLS; i++) { cell_flag[p][i] = 0; draw_cell(p, i, 0); }
		} else {
			for (i = 0; i < cell_n[p]; i++) { u32 c = cell_list[p][i]; cell_flag[p][c] = 0; draw_cell(p, c, 0); }
		}
		cell_n[p] = 0;
	}
	if (reg[1] & 0x40) sprites();
	if (CUSTOM(0x01E) & 0x0020) rest = 1; /* the next vertical blank has come already */
}

static void keyboard(void) {
	if (!(CIAA(0xD00) & 0x08)) return;
	u8 raw = (u8)~CIAA(0xC00);
	u8 code = (u8)(raw >> 1 | raw << 7);
	CIAA(0xE00) |= 0x40;
	{ u16 l = CUSTOM(0x006) >> 8; int n = 0; while (n < 3) { u16 m = CUSTOM(0x006) >> 8; if (m != l) { l = m; n++; } } }
	CIAA(0xE00) &= (u8)~0x40;
	u16 bit = 0;
	switch (code & 0x7F) {
	case 0x4C: bit = 1; break; case 0x4D: bit = 2; break; case 0x4F: bit = 4; break; case 0x4E: bit = 8; break;
	case 0x32: case 0x40: bit = 16; break;
	case 0x33: bit = 32; break;
	case 0x31: bit = 64; break;
	case 0x44: case 0x43: bit = 128; break;
	}
	if (code & 0x80) keys &= (u16)~bit; else keys |= bit;
}

/* the vertical blank: the pads. -> nonzero if the game's own vertical interrupt is enabled */
int layer_vbl(void) {
	vbl_count++;
	keyboard();
	u16 j = CUSTOM(0x00C), s = keys;
	if (j & 2) s |= 8;
	if (j & 0x200) s |= 4;
	if ((j ^ j >> 1) & 1) s |= 2;
	if ((j ^ j >> 1) & 0x100) s |= 1;
	if (!(CIAA(0) & 0x80)) s |= 16;
	if (!(CUSTOM(0x016) & 0x4000)) s |= 128;
	pad_state[0] = s;
	return reg[1] & 0x20;
}

void layer_init(void) {
	u32 i;
	for (i = 0; i < 256; i++) { u8 v = 0; for (int k = 0; k < 8; k++) if (i >> k & 1) v |= (u8)(0x80 >> k); rev[i] = v; }
	for (i = 0; i < 2 * PF_BYTES; i++) pf[0][i] = 0;
	tables_moved();
	group(0); group(1);
	for (int k = 0; k < 2; k++) { cop[k][0] = 0xFFFF; cop[k][1] = 0xFFFE; }
	CUSTOM(0x100) = 0x0201;
	CUSTOM(0x102) = 0;
	CUSTOM(0x104) = 0x0024;               /* BPLCON2: plane A in front; sprites over both */
	CUSTOM(0x106) = 0x1000;               /* BPLCON3: the second playfield's colours from 16 */
	CUSTOM(0x10C) = 0;
	CUSTOM(0x108) = PF_ROW - 42; CUSTOM(0x10A) = PF_ROW - 42;   /* 21 words are fetched per line */
	CUSTOM(0x1FC) = 0;
	CUSTOM(0x08E) = 0x2C81;               /* 320 x 224 from line $2C */
	CUSTOM(0x090) = 0x0CC1;
	CUSTOM(0x092) = 0x0030; CUSTOM(0x094) = 0x00D0;   /* one word early, for the fine scroll */
	CUSTOM(0x080) = (u16)((u32)cop[0] >> 16); CUSTOM(0x082) = (u16)(u32)cop[0];
	CUSTOM(0x088) = 0;
	CUSTOM(0x034) = 0xFF00;
}
