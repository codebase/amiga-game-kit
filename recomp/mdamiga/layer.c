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
static u8 in_vint;                        /* the game's vertical interrupt handler is running */
static u8 pad_th[2], pad_ctrl[2];
static u16 pad_state[2];                  /* bits: 0 up 1 down 2 left 3 right 4 B 5 C 6 A 7 Start */
static u16 keys;
volatile u32 vbl_count;

/* the bitmaps: two of each plane, one on screen while the other is brought up to date (sprites
 * rubbed out and drawn again, a screenful of cells after the colours were regrouped: never seen
 * half done). Surface back * 2 + p is plane p's that is being made. */
static u8 *const surf[4] = { (u8 *)PORT_SCREEN0, (u8 *)(PORT_SCREEN0 + PF_BYTES),
                             (u8 *)(PORT_SCREEN0 + 2 * PF_BYTES), (u8 *)(PORT_SCREEN0 + 3 * PF_BYTES) };
static u16 *const cop[2] = { (u16 *)PORT_COPPER, (u16 *)(PORT_COPPER + 0x4000) };   /* (in one 64K: COP1LC changes in one write) */
#ifdef LAYER_DEBUG                        /* (counts a debug build reports) */
#define DBG(x) x
static int dbg_cells, dbg_tiles;         /* drawn this frame */
static int dbg_moved, dbg_group, dbg_flip, dbg_geo;
static u32 dbg_rd, dbg_wr, dbg_data, dbg_dma, dbg_vint, dbg_w2, dbg_en;
#else
#define DBG(x) (void)0
#endif
static int dbg_snap;
static int back;                          /* which of plane A's bitmaps (and which copper list) is being made */

static void ser(int c) { while (!(CUSTOM(0x018) & 0x2000)) ; CUSTOM(0x030) = (u16)(0x100 | c); }
static void ser_hex(u32 v, int digits) { for (int k = digits * 4 - 4; k >= 0; k -= 4) ser("0123456789ABCDEF"[(v >> k) & 15]); ser(' '); }
extern u32 port_ram;
static u32 mem16(u32 a);
/* an exception nothing handles (start.S, before it stops with the border striped): what and where,
 * on the serial port. Vector 010 at an address in the ROM is the port's mark on old code no known
 * path leads to: a computed jump the analysis run never took (`entry HEX` in port.txt, or play
 * that part in the analysis run). */
void layer_fault(u32 pc, u32 vector) {
	const char *m = "port: exception, vector offset ";
	while (*m) ser(*m++);
	ser_hex(vector & 0xFFF, 3);
	m = "at ";
	while (*m) ser(*m++);
	ser_hex(pc, 8);
	if (pc - port_ram < 0x200000) { m = "(the port's layout: "; while (*m) ser(*m++); ser_hex(pc - (port_ram - PORT_RAM), 6); ser(')'); }
	ser('\n');
	extern u32 fault_regs[16];
	for (int i = 0; i < 16; i++) { if (!(i & 7)) { ser("da"[i >> 3]); ser(' '); } ser_hex(fault_regs[i], 8); if ((i & 7) == 7) ser('\n'); }
	const u8 *sp = (const u8 *)fault_regs[15];   /* (the exception's frame, then what the game had on its stack) */
	ser('s'); ser(' ');
	for (int i = 0; i < 24; i++) ser_hex(mem16((u32)sp + 2 * i), 4);
	ser('\n');
}
static u32 mem16(u32 a) { const u8 *p = (const u8 *)a; return (u32)p[0] << 8 | p[1]; }
static u32 game_addr(u32 a) {
	a &= 0xFFFFFF;
	return a >= 0xE00000 ? port_ram + (a & 0xFFFF) : a;
}

/* ------------------------------------------- what changed, as it's written */
/* Chip RAM is the scarce thing: the CPU gets a few thousand accesses to it a frame, whatever its
 * speed. So each plane is kept in fast RAM first (`shadow`: the 64x32 cells once, a byte a cell
 * row, plane A's with the sprites in it), and a cell goes to a bitmap in chip RAM only when it is
 * on screen and differs: `valid` says, per bitmap and per cell of its 128 columns, that the cell
 * there is what the shadow holds. Everything is lazy: a cell the game writes just loses its marks. */
#define SH_ROW 64
#define SH_PLANE (SH_ROW * 256)
static u8 shadow[2][4 * SH_PLANE];
static u8 sh_ok[2][CELLS];                /* the shadow's cell holds its tile */
static u8 valid[4][32 * 128];             /* per surface */
static u32 nt[2];                         /* the planes' name tables in VRAM */
static u8 tile_dirty[2048];
static int tiles_dirty, td_lo = 2048, td_hi = -1;   /* ... and the range they are in */
static u32 hs_at;                         /* the horizontal scroll table in VRAM */
static u8 hs_dirty = 1;                   /* the scroll (table, vertical, mode) changed: the geometry is to be worked out again */
static int cell_all[2];
static int cram_dirty = 1;
static u16 seen[2][4];                    /* the colours the tiles drawn in a playfield have used, per palette line */
static u8 nslot[2], slot_rep[2][16];      /* its slots in use, and a colour of each (newcomers are measured against it) */
static u8 want_group[2], scene_new = 1, pal_stale;
static u8 cram_chg[64];                   /* written with a new value since the last frame */
static u8 anim[64], anim_n[64];           /* a colour the game keeps changing on its own (a waterfall's cycle) */

/* The window: a third plane that takes plane A's place on some lines, unscrolled (a status bar).
 * Its cells go straight into a bitmap of its own in chip RAM (after the copper lists: 8 cell rows
 * of 40, a word of lead-in for the fetch that starts early), and the copper points plane A's
 * bitplanes there on its lines. A bar of up to 64 lines at the top or the bottom is what it does. */
#define WIN_ROW 48
#define WIN_PLANE (WIN_ROW * 64)
static u8 *const wsurf = (u8 *)(PORT_COPPER + 0x8000);
static u8 wok[8 * 40], wall = 1;
static u32 wnt;                           /* its name table */
static int wtop, wlines, wrow0;           /* the lines it covers (none: wlines 0), and its first cell row */
static void window_moved(void) {
	int r = reg[18] & 31, h40 = reg[12] & 1;
	wnt = (u32)(reg[3] & (h40 ? 0x3C : 0x3E)) << 10;
	wlines = 0;
	if (!(reg[17] & 0x9F)) {              /* (full width only) */
		if (!(reg[18] & 0x80)) { if (r && r <= 8) { wtop = 0; wrow0 = 0; wlines = r * 8; } }
		else if (r < 28 && 28 - r <= 8) { wtop = r * 8; wrow0 = r; wlines = (28 - r) * 8; }
	}
	wall = 1;
}
static void window_entry(u32 n) {         /* entry n of its name table was written */
	u32 cy = n >> ((reg[12] & 1) ? 6 : 5), cx = n & ((reg[12] & 1) ? 63 : 31);
	if (cx < 40 && cy - (u32)wrow0 < 8) wok[(cy - (u32)wrow0) * 40 + cx] = 0;
}

static void cell_dirty(int p, u32 cell) {
	u32 at = (cell >> 6) * 128 + (cell & 63);
	sh_ok[p][cell] = 0;
	valid[p][at] = valid[p][at + 64] = 0;
	valid[p + 2][at] = valid[p + 2][at + 64] = 0;
}
static void all_dirty(int p) {
	u32 *a = (u32 *)sh_ok[p], *b, i;        /* (by longs: a stock A1200 feels this) */
	for (i = 0; i < CELLS / 4; i++) a[i] = 0;
	a = (u32 *)valid[p]; b = (u32 *)valid[p + 2];
	for (i = 0; i < 32 * 128 / 4; i++) a[i] = b[i] = 0;
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
	int t = (int)(a >> 5);
	tile_dirty[t] = 1; tiles_dirty = 1;
	if (t < td_lo) td_lo = t;
	if (t > td_hi) td_hi = t;
	if (a - hs_at < 0x380) hs_dirty = 1;
	u32 o = a - nt[0], size = (u32)(pw * ph * 2);
	if (o < size) entry_dirty(0, o >> 1);
	o = a - nt[1];
	if (o < size) entry_dirty(1, o >> 1);
	o = a - wnt;
	if (wlines && o < 0x1000) window_entry(o >> 1);
}
static void vram_w8(u32 a, u8 v) {        /* a single byte (fills, copies) */
	a &= 0xFFFF;
	if (a & 1) vram_w(a, vram[a & 0xFFFE], v); else vram_w(a, v, vram[a + 1]);
}
/* n words from p into VRAM at a (even): what a DMA is. A tile's worth (32 bytes) at a time where
 * that fits: compared by longs and, most of the time, found the same (the game sends its scroll
 * and sprite tables every frame); copied by longs where no name table is, a word at a time there. */
static void vram_block(u32 a, const u8 *p, u32 n) {
	u32 size = (u32)(pw * ph * 2);
	if (a + n * 2 > 0x10000) { for (; n; n--, p += 2, a += 2) vram_w(a, p[0], p[1]); return; }
	for (; n && (a & 31); n--, p += 2, a += 2) vram_w(a, p[0], p[1]);
	for (; n >= 16; n -= 16, p += 32, a += 32) {
		const u32 *s = (const u32 *)p;
		u32 *d = (u32 *)&vram[a];
		if (s[0] == d[0] && s[1] == d[1] && s[2] == d[2] && s[3] == d[3] && s[4] == d[4] && s[5] == d[5] && s[6] == d[6] && s[7] == d[7]) continue;
		if (a + 32 > nt[0] && a < nt[0] + size) { for (int k = 0; k < 32; k += 2) vram_w(a + (u32)k, p[k], p[k + 1]); continue; }
		if (a + 32 > nt[1] && a < nt[1] + size) { for (int k = 0; k < 32; k += 2) vram_w(a + (u32)k, p[k], p[k + 1]); continue; }
		if (wlines && a + 32 > wnt && a < wnt + 0x1000) { for (int k = 0; k < 32; k += 2) vram_w(a + (u32)k, p[k], p[k + 1]); continue; }
		d[0] = s[0]; d[1] = s[1]; d[2] = s[2]; d[3] = s[3]; d[4] = s[4]; d[5] = s[5]; d[6] = s[6]; d[7] = s[7];
		int t = (int)(a >> 5);
		tile_dirty[t] = 1; tiles_dirty = 1;
		if (t < td_lo) td_lo = t;
		if (t > td_hi) td_hi = t;
		if (a + 32 > hs_at && a < hs_at + 0x380) hs_dirty = 1;
	}
	for (; n; n--, p += 2, a += 2) vram_w(a, p[0], p[1]);
}
static void tables_moved(void) {
	DBG(dbg_moved++);
	nt[0] = (u32)(reg[2] & 0x38) << 10;
	nt[1] = (u32)(reg[4] & 7) << 13;
	static const u8 shifts[4] = { 5, 6, 5, 7 };
	pw_shift = shifts[reg[16] & 3]; pw = 1 << pw_shift;
	ph = 1 << shifts[(reg[16] >> 4) & 3];
	cell_all[0] = cell_all[1] = 1;
	scene_new = 1;
}

/* ----------------------------------------------------------------- VDP */
static void vdp_data_write(u16 v) {
	DBG(dbg_data++);
	switch (vdp_code & 0xF) {
	case 1:
		if (vdp_addr & 1) vram_w(vdp_addr, (u8)v, (u8)(v >> 8)); else vram_w(vdp_addr, (u8)(v >> 8), (u8)v);
		break;
	case 3: { u16 n = v & 0xEEE, *c = &cram[(vdp_addr >> 1) & 63]; if (*c != n) { *c = n; cram_dirty = 1; cram_chg[(vdp_addr >> 1) & 63] = 1; } break; }
	case 5: { u16 *c = &vsram[(vdp_addr >> 1) % 40]; if (*c != (v & 0x7FF)) { *c = v & 0x7FF; hs_dirty = 1; } break; }
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
	DBG(dbg_dma += len);
	if (mode < 2) {
		u32 src = ((u32)(reg[23] & 0x7F) << 17 | (u32)reg[22] << 9 | (u32)reg[21] << 1);
		if ((vdp_code & 0xF) == 1 && reg[15] == 2 && !(vdp_addr & 1)) {   /* the usual case: words into VRAM */
			vram_block(vdp_addr, (const u8 *)game_addr(src), len);
			vdp_addr = (vdp_addr + len * 2) & 0xFFFF;
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
			if (r == 3 || r == 12 || r == 17 || r == 18) window_moved();
			if (r == 7) cram_dirty = 1;
			if (r == 11 || r == 13) { hs_at = (u32)(reg[13] & 0x3F) << 10; hs_dirty = 1; }
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

/* The game's interrupt handlers use the VDP too (a DMA of the sprite table, even on a frame the
 * main loop hasn't finished). On the Mega Drive they never caught the main loop between setting
 * a VRAM address and writing there: the loop was always done in time. Here the layer takes time
 * of its own, so it can happen; the address the interrupted code had set is put back after. */
static u32 held_code, held_addr;
static int held_pending;
void layer_irq_enter(void) { held_code = vdp_code; held_addr = vdp_addr; held_pending = ctrl_pending; }
void layer_irq_leave(void) { vdp_code = held_code; vdp_addr = held_addr; ctrl_pending = held_pending; }

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

u32 sound_z80_read(u32 a);
static u32 io_read(u32 a, int bits) {
	if (a >= 0xC00000 && a < 0xC00010) {
		u32 v;
		switch (a & 0xE) {
		case 0x0: case 0x2: v = vdp_data_read(); ctrl_pending = 0; break;
		case 0x4: case 0x6: {
			int l = beam_line();
			v = status | 1;
			/* (the game's vertical interrupt is in the blank whenever it comes: the layer may have held it
			 * up past the top of the picture, and a handler that waits for the blank's flag, as Sonic 2's
			 * does, would wait a frame for the next) */
			if (l >= H || in_vint) v |= 0x0008;
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
		case 0x00: v = PORT_VERSION; break;   /* (the region the cartridge is for: agk port, from its header) */
		case 0x02: v = pad_read(0); break;
		case 0x04: v = pad_read(1); break;
		case 0x06: v = 0x7F; break;
		case 0x08: v = pad_ctrl[0]; break;
		case 0x0A: v = pad_ctrl[1]; break;
		}
		return bits == 8 ? v : v << 8 | v;
	}
	if (a >= 0xA00000 && a < 0xA04000) { u32 v = sound_z80_read(a); return bits == 8 ? v : v << 8 | v; }   /* the sound driver's RAM */
	return 0;
}

u32 sound_z80_read(u32 a);
void sound_z80_write(u32 a, u32 v);
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
		return;
	}
	if (a >= 0xA00000 && a < 0xA04000) sound_z80_write(a, bits == 16 ? v >> 8 : v);   /* a request of the sound driver (sound.c) */
}

void sh_r(u32 a, int bits) {
	DBG(dbg_rd++);
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
	DBG(dbg_wr++);
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
static u32 grouped_at[2];
static const u8 lev[8] = { 0, 52, 87, 116, 144, 172, 206, 255 };

/* lut[p][line][v]: a byte of a tile (two pixels) in playfield p's slots, as two bits in each of four
 * plane bytes (plane 0's in the top byte): what a tile is turned planar with, a row in four look-ups */
static u32 lut[2][4][256];
static u8 lut_ok[2];                      /* a bit per palette line */
static u16 used[256];                     /* the two colours of such a byte, as bits */
static u32 lut_entry(const u8 *m, int v) {
	u32 a = m[v >> 4], b = m[v & 15];
	return (a & 1) << 25 | (b & 1) << 24 | (a & 2) << 16 | (b & 2) << 15 | (a & 4) << 7 | (b & 4) << 6 | (a & 8) >> 2 | (b & 8) >> 3;
}

static int dist(u16 a, u16 b) {           /* two CRAM words */
	int dr = (int)(a >> 1 & 7) - (int)(b >> 1 & 7), dg = (int)(a >> 5 & 7) - (int)(b >> 5 & 7), db = (int)(a >> 9 & 7) - (int)(b >> 9 & 7);
	return dr * dr * 3 + dg * dg * 4 + db * db * 2;
}

/* a colour a tile of playfield p uses for the first time: a slot of its own while there are free
 * ones, else the nearest (and the grouping is done again if that is far off) */
static void newcomer(int p, int c) {
	int best = 1 << 30, bs = 0, s;
	for (s = 1; s <= nslot[p]; s++) {
		int d = dist(cram[c], cram[slot_rep[p][s]]);
		if (d < best) { best = d; bs = s; }
	}
	if (nslot[p] < 15 && best > 0) { bs = ++nslot[p]; slot_rep[p][bs] = (u8)c; }
	else if (best > 12) want_group[p] = 1;
	map[p][c] = (u8)bs;
	pal_stale = 1;
	if (lut_ok[p] >> (c >> 4) & 1) {      /* the table's entries with this colour in them */
		u32 *l = lut[p][c >> 4];
		const u8 *m = &map[p][c & 0x30];
		int i = c & 15;
		for (int j = 0; j < 16; j++) { l[i << 4 | j] = lut_entry(m, i << 4 | j); l[j << 4 | i] = lut_entry(m, j << 4 | i); }
	}
}

/* group the colours playfield p's tiles use into 15 slots: fifteen colours picked to be as far from each other
 * as possible (the first, then whichever is farthest from all picked so far), and every colour
 * with the nearest of them. -> nonzero if the grouping changed */
static int group(int p) {
	static u8 member[64], near[64];
	static int far[64];
	u8 rep[15], old[64];
	int count = 0, reps = 0, i, k;
	for (i = 0; i < 64; i++) {
		old[i] = map[p][i];
		map[p][i] = 0;
		if ((i & 15) && (seen[p][i >> 4] >> (i & 15) & 1)) member[count++] = (u8)i;
	}
	grouped_at[p] = vbl_count;
	if (count) {
		for (i = 0; i < count; i++) { far[i] = 1 << 30; near[i] = 0; }
		/* the colours the game keeps changing get slots of their own, up to eight: sharing one,
		 * they would drag the others' colour along (and the grouping would never settle) */
		for (i = 0; i < count && reps < 8; i++) if (anim[member[i]]) { near[i] = (u8)reps; far[i] = -1; rep[reps++] = member[i]; }
		int next = -1;
		for (i = 0; i < count; i++) if (far[i] >= 0) { next = i; break; }
		while (next >= 0 && reps < 15) {
			rep[reps] = member[next];
			int best = 0, bi = -1;
			for (i = 0; i < count; i++) {
				if (far[i] < 0) continue;
				int d = dist(cram[member[i]], cram[rep[reps]]);
				if (d < far[i]) { far[i] = d; near[i] = (u8)reps; }
				if (far[i] > best) { best = far[i]; bi = i; }
			}
			reps++;
			next = bi;                    /* (none: every colour is one of the picked ones already) */
		}
		for (i = 0; i < count; i++) map[p][member[i]] = (u8)(near[i] + 1);
	}
	nslot[p] = (u8)reps;
	lut_ok[p] = 0;
	for (i = 0; i < reps; i++) slot_rep[p][i + 1] = rep[i];
	for (k = 0, i = 0; i < 64; i++) k |= old[i] ^ map[p][i];
	return k;
}

static u16 pal_hi[32], pal_lo[32];        /* the 32 colour registers: set by the copper, with the bitmaps they belong to */
static void set_colour(int i, int r, int g, int b) {   /* 8 bits each */
	pal_hi[i] = (u16)((r >> 4) << 8 | (g >> 4) << 4 | (b >> 4));
	pal_lo[i] = (u16)((r & 15) << 8 | (g & 15) << 4 | (b & 15));
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
			if (s && !anim[i]) {
				u16 c = cram[i];
				int dr = lev[c >> 1 & 7] - r[s], dg = lev[c >> 5 & 7] - g[s], db = lev[c >> 9 & 7] - b[s];
				int d = (dr < 0 ? -dr : dr) + (dg < 0 ? -dg : dg) + (db < 0 ? -db : db);
				if (d > worst) worst = d;
			}
		}
		err[p] = worst;
	}
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
	u16 bits = 0;
	for (int i = 0; i < 32; i++) bits |= used[src[i]];
	bits &= (u16)~(seen[p][line] | 1);
	if (bits) {
		seen[p][line] |= bits;
		for (int i = 1; i < 16; i++) if (bits >> i & 1) newcomer(p, line * 16 + i);
	}
	u32 *l = lut[p][line];
	if (!(lut_ok[p] >> line & 1)) {
		for (int v = 0; v < 256; v++) l[v] = lut_entry(m, v);
		lut_ok[p] |= (u8)(1 << line);
	}
	for (int y = 0; y < 8; y++, src += 4) {
		u32 v = l[src[0]] << 6 | l[src[1]] << 4 | l[src[2]] << 2 | l[src[3]];
		*(u32 *)t->row[y] = v;
		t->mask[y] = (u8)(v >> 24 | v >> 16 | v >> 8 | v);
	}
	cache_ok[p][tile] |= (u8)(1 << line);
	return t;
}

/* a cell of plane p's shadow from its name table entry; over = keep what the tile leaves clear */
static void draw_cell(int p, u32 cell, int over) {
	const u8 *e = &vram[(nt[p] + entry_of(cell) * 2) & 0xFFFF];
	int tile = (e[0] & 7) << 8 | e[1], line = (e[0] >> 5) & 3, hf = e[0] & 8, vf = e[0] & 16;
	const ptile_t *t = get_tile(p, tile, line);
	u8 *d = shadow[p] + (cell >> 6) * 8 * SH_ROW + (cell & 63);
	DBG(dbg_cells++);
	tile_in_nt[tile] = 1;
	if (!over) sh_ok[p][cell] = 1;
	for (int y = 0; y < 8; y++, d += SH_ROW) {
		const u8 *r = t->row[vf ? 7 - y : y];
		u8 b0 = r[0], b1 = r[1], b2 = r[2], b3 = r[3];
		if (hf) { b0 = rev[b0]; b1 = rev[b1]; b2 = rev[b2]; b3 = rev[b3]; }
		if (over) {
			u8 k = (u8)~(b0 | b1 | b2 | b3);
			d[0] = (u8)((d[0] & k) | b0); d[SH_PLANE] = (u8)((d[SH_PLANE] & k) | b1);
			d[SH_PLANE * 2] = (u8)((d[SH_PLANE * 2] & k) | b2); d[SH_PLANE * 3] = (u8)((d[SH_PLANE * 3] & k) | b3);
		} else {
			d[0] = b0; d[SH_PLANE] = b1; d[SH_PLANE * 2] = b2; d[SH_PLANE * 3] = b3;
		}
	}
}

/* the window's cells that changed, into its bitmap */
static void window(void) {
	if (!wlines) return;
	int rows = wlines >> 3, sh = (reg[12] & 1) ? 6 : 5;
	for (int r = 0; r < rows; r++) for (int c = 0; c < 40; c++) {
		u8 *okp = &wok[r * 40 + c];
		if (*okp && !wall) continue;
		*okp = 1;
		const u8 *e = &vram[(wnt + ((u32)((r + wrow0) << sh | c) << 1)) & 0xFFFF];
		int tile = (e[0] & 7) << 8 | e[1], line = (e[0] >> 5) & 3, hf = e[0] & 8, vf = e[0] & 16;
		const ptile_t *t = get_tile(0, tile, line);
		u8 *d = wsurf + r * 8 * WIN_ROW + 2 + c;
		tile_in_nt[tile] = 1;
		for (int y = 0; y < 8; y++, d += WIN_ROW) {
			const u8 *q = t->row[vf ? 7 - y : y];
			if (hf) { d[0] = rev[q[0]]; d[WIN_PLANE] = rev[q[1]]; d[WIN_PLANE * 2] = rev[q[2]]; d[WIN_PLANE * 3] = rev[q[3]]; }
			else { d[0] = q[0]; d[WIN_PLANE] = q[1]; d[WIN_PLANE * 2] = q[2]; d[WIN_PLANE * 3] = q[3]; }
		}
	}
	wall = 0;
}

/* n cells (1, 2 or 4: a byte, a word or a long a row, at an address that suits it) of cell row r
 * from plane p's shadow into surface s at column c of its 128 */
static void put(int s, int p, int r, int c, int n) {
	const u8 *src = shadow[p] + r * 8 * SH_ROW + (c & 63);
	u8 *dst = surf[s] + r * 8 * PF_ROW + c;
	int k, y;
	DBG(dbg_tiles += n);
	if (n == 4) {
		for (k = 0; k < 4; k++, src += SH_PLANE - 8 * SH_ROW, dst += PF_PLANE - 8 * PF_ROW)
			for (y = 0; y < 8; y++, src += SH_ROW, dst += PF_ROW) *(u32 *)dst = *(const u32 *)src;
	} else if (n == 2) {
		for (k = 0; k < 4; k++, src += SH_PLANE - 8 * SH_ROW, dst += PF_PLANE - 8 * PF_ROW)
			for (y = 0; y < 8; y++, src += SH_ROW, dst += PF_ROW) *(u16 *)dst = *(const u16 *)src;
	} else {
		for (k = 0; k < 4; k++, src += SH_PLANE - 8 * SH_ROW, dst += PF_PLANE - 8 * PF_ROW)
			for (y = 0; y < 8; y++, src += SH_ROW, dst += PF_ROW) *dst = *src;
	}
}

/* ------------------------------------------------- the frame's geometry */
static u16 sx[2][H];                      /* per line: the bitmap x the screen's left edge shows, 16..527 */
static u8 row_of[2][H];                   /* per line: the bitmap row */

static u8 wmin[2][32], wmax[2][32];       /* per cell row of the bitmap: the columns on screen (none: min > max) */

static void geometry(void) {
	const u8 *hs = &vram[(u32)(reg[13] & 0x3F) << 10];
	int mode = reg[11] & 3;
	hs_dirty = 0;
	DBG(dbg_geo++);
	for (int p = 0; p < 2; p++) {
		int vs = vsram[p];
		for (int r = 0; r < 32; r++) { wmin[p][r] = 255; wmax[p][r] = 0; }
		for (int y = 0; y < H; y++) {
			const u8 *e = hs + (mode == 0 ? 0 : mode == 3 ? y * 4 : (y & ~7) * 4) + p * 2;
			int x = (0 - (e[0] << 8 | e[1])) & 511;
			if (x < 16) x += 512;
			sx[p][y] = (u16)x;
			int row = (y + vs) & 255, r = row >> 3, c0 = x >> 3, c1 = (x + 319) >> 3;
			row_of[p][y] = (u8)row;
			if (c0 < wmin[p][r]) wmin[p][r] = (u8)c0;
			if (c1 > wmax[p][r]) wmax[p][r] = (u8)c1;
		}
	}
}

/* surface s brought up to plane p's shadow where it is on screen */
/* the frame's time for the layer is up: the game's main loop must have the rest of it, and the
 * next vertical blank its interrupt on time (the game counts frames by it) */
#define DEADLINE 170                      /* raster line, of 312 */
#define SLICE 70                          /* ... but this many lines at least, however late the game's interrupt left it */
static u32 deadline = DEADLINE;
static u32 raster(void) { return ((u32)(CUSTOM(0x004) & 1) << 8) | (CUSTOM(0x006) >> 8); }
static int late(void) { return raster() >= deadline || (CUSTOM(0x01E) & 0x0020); }
/* -> 0 if time ran out before every visible cell was there (what is done stays done) */
static int present(int s, int p) {
	for (int r = 0; r < 32; r++) {
		int c = wmin[p][r], c1 = wmax[p][r];
		if (c <= c1 && late()) return 0;
		u8 *v = &valid[s][r * 128];
		u8 *ok = &sh_ok[p][r << 6];
		while (c <= c1) {
			if (!(c & 3) && c + 3 <= c1) {
				u32 four = *(const u32 *)(v + c);
				if (four == 0x01010101) { c += 4; continue; }
				if (!four) {
					for (int k = 0; k < 4; k++) if (!ok[(c + k) & 63]) draw_cell(p, (u32)(r << 6 | ((c + k) & 63)), 0);
					put(s, p, r, c, 4);
					*(u32 *)(v + c) = 0x01010101;
					c += 4;
					continue;
				}
			}
			if (v[c]) { c++; continue; }
			if (!ok[c & 63]) draw_cell(p, (u32)(r << 6 | (c & 63)), 0);
			if (!(c & 1) && c < c1 && !v[c + 1]) {
				if (!ok[(c + 1) & 63]) draw_cell(p, (u32)(r << 6 | ((c + 1) & 63)), 0);
				put(s, p, r, c, 2);
				v[c] = v[c + 1] = 1;
				c += 2;
			} else {
				put(s, p, r, c, 1);
				v[c++] = 1;
			}
		}
	}
	return 1;
}

/* the copper list for the frame just drawn: the colours, the pointers and the fine scroll at the
 * top, and the pointers and scroll again on every line where they don't simply carry on; the
 * game's horizontal interrupt as a copper interrupt. It takes over at the next vertical blank:
 * plane A's new bitmap, its scroll and its colours all at once. */
static void build_copper(void) {
	u16 *c = cop[back];
	u32 ptr[2], want[2];
	const u8 *base[2] = { surf[back * 2], surf[back * 2 + 1] };
	u32 pstep = PF_PLANE;
	int fine[2], con1, past255 = 0, hint = (reg[0] & 0x10) ? reg[10] : -1, i;
	*c++ = 0x100; *c++ = (reg[1] & 0x40) ? 0x0611 : 0x0201;   /* BPLCON0: eight planes as two playfields, or nothing */
	*c++ = 0x106; *c++ = 0x1000;          /* BPLCON3: the second playfield's colours from 16 */
	for (i = 0; i < 32; i++) { *c++ = (u16)(0x180 + 2 * i); *c++ = pal_hi[i]; }
	*c++ = 0x106; *c++ = 0x1200;          /* and their low bits */
	for (i = 0; i < 32; i++) { *c++ = (u16)(0x180 + 2 * i); *c++ = pal_lo[i]; }
	*c++ = 0x106; *c++ = 0x1000;
	/* every sixteenth line the copper raises an interrupt (the blitter's bit): start.S looks where the game is */
	*c++ = 0x1001; *c++ = 0xFFFE; *c++ = 0x009C; *c++ = 0x8040;
	*c++ = 0x2001; *c++ = 0xFFFE; *c++ = 0x009C; *c++ = 0x8040;
	for (int y = 0; y < H; y++) {
		for (int p = 0; p < 2; p++) {
			int x = sx[p][y];
			want[p] = (u32)base[p] + (u32)row_of[p][y] * PF_ROW + (u32)((x - 1) >> 4) * 2;
			fine[p] = (16 - (x & 15)) & 15;
		}
		if ((u32)(y - wtop) < (u32)wlines) { want[0] = (u32)wsurf + (u32)(y - wtop) * WIN_ROW; fine[0] = 0; }   /* the window's lines */
		int c1 = fine[1] << 4 | fine[0];
		int moveA = y == 0 || want[0] != ptr[0] + PF_ROW, moveB = y == 0 || want[1] != ptr[1] + PF_ROW;
		int movec = y == 0 || c1 != con1, irq = y && y - 1 == hint, poll = y && ((0x2C + y - 1) & 15) == 15;
		if (y && (moveA || moveB || movec || irq || poll)) {   /* after line y-1's fetch, before line y's */
			int l = 0x2C + y - 1;
			if (l >= 256 && !past255) { *c++ = 0xFFDF; *c++ = 0xFFFE; past255 = 1; }
			if (l != 255) { *c++ = (u16)((l & 0xFF) << 8 | 0xDB); *c++ = 0xFFFE; }
			else { *c++ = 0xFFDF; *c++ = 0xFFFE; past255 = 1; }
		}
		if (irq) { *c++ = 0x009C; *c++ = 0x8010; }
		if (movec) { *c++ = 0x102; *c++ = (u16)c1; }
		u32 step = (u32)(y - wtop) < (u32)wlines ? WIN_PLANE : PF_PLANE;   /* plane A's bitplanes: how far apart on this line */
		for (int p = 0; p < 2; p++) if (p ? moveB : moveA) {
			u32 a = want[p], st = p ? PF_PLANE : step, was = p ? PF_PLANE : pstep;
			for (int k = 0; k < 4; k++, a += st) {
				int r = 0x0E0 + (k * 2 + p) * 4;       /* plane A is bitplanes 1,3,5,7; plane B 2,4,6,8 */
				/* (few moves fit between two lines' fetches: the high word only when the pointer, which
				 * has run on a row from the last line, isn't in the same 64K) */
				if (y == 0 || ((ptr[p] + (u32)k * was + PF_ROW) >> 16) != (a >> 16)) { *c++ = (u16)r; *c++ = (u16)(a >> 16); }
				*c++ = (u16)(r + 2); *c++ = (u16)a;
			}
		}
		pstep = step;
		if (poll) { *c++ = 0x009C; *c++ = 0x8040; }
		ptr[0] = want[0]; ptr[1] = want[1]; con1 = c1;
	}
	if (!past255) { *c++ = 0xFFDF; *c++ = 0xFFFE; }
	*c++ = 0x0F01; *c++ = 0xFFFE; *c++ = 0x009C; *c++ = 0x8040;   /* (lines 271 and 287) */
	*c++ = 0x1F01; *c++ = 0xFFFE; *c++ = 0x009C; *c++ = 0x8040;
	*c++ = 0xFFFF; *c++ = 0xFFFE;
	CUSTOM(0x082) = (u16)(u32)cop[back];
}

/* ------------------------------------------------------------- sprites */
/* into plane A's shadow. Two records: the shadow's cells with sprite in them (their tiles are
 * drawn again before the next frame's sprites), and, per bitmap of plane A, the cells where that
 * bitmap got sprite (they are stale there the next time it is the one being made). */
static u16 bob_list[CELLS], over_list[CELLS];
static u8 bob_flag[CELLS];
static int bob_n, over_n;
static u16 got_list[2][32 * 128];
static u8 got_flag[2][32 * 128];
static int got_n[2];

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
	u8 *base = shadow[0];
	u8 *gflag = got_flag[back], *val = valid[back * 2];
	u16 *glist = got_list[back];
	int gn = got_n[back];
	/* the sprites without priority, then plane A's cells with priority over them, then the sprites with */
	for (int pass = 0; pass < 2; pass++) {
		for (i = n - 1; i >= 0; i--) {    /* the first in the list is on top: drawn last */
			int w = sp[i].w, h = sp[i].h, X0 = sp[i].x, Y0 = sp[i].y;
			u16 attr = sp[i].attr;
			if ((attr >> 15) != pass) continue;
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
					/* the usual case, the quick way: the tile whole on screen, its eight lines scrolled alike, and
					 * no edge of the bitmap in it. The (at most four) cells it touches once, then a word a plane a row. */
					if (Y >= 0 && Y + 7 < H && sx[0][Y] == sx[0][Y + 7] && sx[0][Y] == sx[0][Y + 3] && row_of[0][Y + 7] == row_of[0][Y] + 7) {
						int bx = sx[0][Y] + X, brow = row_of[0][Y], sh = bx & 7, c0 = (bx >> 3) & 63;
						if (bx >= 0 && c0 < 63) {
							int two_c = sh != 0, two_r = (brow & 7) != 0;
							for (int e = 0; e < 4; e++) {
								if (((e & 1) && !two_c) || ((e & 2) && !two_r)) continue;
								u32 cr = (u32)((brow >> 3) + (e >> 1));
								u32 cl = cr << 6 | (u32)(c0 + (e & 1)), at2 = cr * 128 + (u32)(bx >> 3) + (u32)(e & 1);
								if (!sh_ok[0][cl]) draw_cell(0, cl, 0);
								if (!bob_flag[cl]) {
									bob_flag[cl] = 1; bob_list[bob_n++] = (u16)cl;
									if (!pass && (nta[entry_of(cl) * 2] & 0x80)) over_list[over_n++] = (u16)cl;
								}
								if (!gflag[at2]) { gflag[at2] = 1; glist[gn++] = (u16)at2; val[at2] = 0; }
							}
							u8 *d = base + brow * SH_ROW + c0;
							int up = 8 - sh;
							for (int r = 0; r < 8; r++, d += SH_ROW) {
								int rr = vf ? 7 - r : r;
								u8 m = t->mask[rr];
								if (!m) continue;
								const u8 *src = t->row[rr];
								u8 b0 = src[0], b1 = src[1], b2 = src[2], b3 = src[3];
								if (hf) { m = rev[m]; b0 = rev[b0]; b1 = rev[b1]; b2 = rev[b2]; b3 = rev[b3]; }
								u16 k = (u16)~(m << up);
								*(u16 *)d = (u16)((*(u16 *)d & k) | (b0 << up));
								*(u16 *)(d + SH_PLANE) = (u16)((*(u16 *)(d + SH_PLANE) & k) | (b1 << up));
								*(u16 *)(d + SH_PLANE * 2) = (u16)((*(u16 *)(d + SH_PLANE * 2) & k) | (b2 << up));
								*(u16 *)(d + SH_PLANE * 3) = (u16)((*(u16 *)(d + SH_PLANE * 3) & k) | (b3 << up));
							}
							continue;
						}
					}
					for (int r = 0; r < 8; r++) {
						int yy = Y + r;
						if (yy < 0 || yy >= H) continue;
						u8 m = t->mask[vf ? 7 - r : r];
						if (!m) continue;
						const u8 *src = t->row[vf ? 7 - r : r];
						int bx = sx[0][yy] + X, brow = row_of[0][yy];
						if (bx < 0) continue;
						int sh = bx & 7, c0 = (bx >> 3) & 63, c1 = (c0 + 1) & 63;
						/* the cells this row touches: their tiles first, and the records */
						u32 at = (u32)(brow >> 3) * 128 + (u32)(bx >> 3);
						if (at != last) {
							last = at;
							for (int e = 0; e < (sh ? 2 : 1); e++) {
								u32 cl = (u32)(brow >> 3) << 6 | (e ? c1 : c0);
								if (!sh_ok[0][cl]) draw_cell(0, cl, 0);
								if (!bob_flag[cl]) {
									bob_flag[cl] = 1; bob_list[bob_n++] = (u16)cl;
									if (!pass && (nta[entry_of(cl) * 2] & 0x80)) over_list[over_n++] = (u16)cl;
								}
								if (!gflag[at + e]) { gflag[at + e] = 1; glist[gn++] = (u16)(at + e); val[at + e] = 0; }
							}
						}
						u8 *d = base + brow * SH_ROW;
						u8 b0 = src[0], b1 = src[1], b2 = src[2], b3 = src[3];
						if (hf) { m = rev[m]; b0 = rev[b0]; b1 = rev[b1]; b2 = rev[b2]; b3 = rev[b3]; }
						u16 k = (u16)~((m << 8) >> sh);
						u16 v;
						v = (u16)(d[c0] << 8 | d[c1]); v = (u16)((v & k) | ((b0 << 8) >> sh)); d[c0] = (u8)(v >> 8); d[c1] = (u8)v; d += SH_PLANE;
						v = (u16)(d[c0] << 8 | d[c1]); v = (u16)((v & k) | ((b1 << 8) >> sh)); d[c0] = (u8)(v >> 8); d[c1] = (u8)v; d += SH_PLANE;
						v = (u16)(d[c0] << 8 | d[c1]); v = (u16)((v & k) | ((b2 << 8) >> sh)); d[c0] = (u8)(v >> 8); d[c1] = (u8)v; d += SH_PLANE;
						v = (u16)(d[c0] << 8 | d[c1]); v = (u16)((v & k) | ((b3 << 8) >> sh)); d[c0] = (u8)(v >> 8); d[c1] = (u8)v;
					}
				}
			}
		}
		if (!pass) { for (i = 0; i < over_n; i++) draw_cell(0, over_list[i], 1); over_n = 0; }
	}
	got_n[back] = gn;
}

/* ------------------------------------------------------------ the frame */
static u32 began_at;                      /* the blank the picture being made was begun in */
static int rest;                          /* (debug: the times it was gone on with) */
static u8 busy;                           /* a picture is being made in the bitmaps off screen, and isn't whole yet */
static u32 vint_seq, drawn_seq;           /* the game's interrupts, and the one the picture being made (or last made) began after */
static u32 work_at;                       /* the blank in which the layer last drew while the game waited */
static const u32 idles[] = { PORT_IDLES 0, 0 };   /* agk port's: the loops the game waits for an interrupt in */
static u32 fade_at, fade_gap;             /* when most of the palette last moved, and how long after the time before */
static void picture(void);
/* The game comes first: a frame of its logic, then the picture in what is left of the frame. The
 * game says when it is done by waiting for its interrupt in a loop agk port found, and the copper
 * asks every sixteen lines where it is (start.S). So a slow machine shows fewer pictures and plays
 * at the game's speed, and a fast one loses nothing. */
static u8 stops;                          /* the game waits with `stop` */
static int waiting(u32 pc) {              /* the game, interrupted at pc, was in one of its waiting loops (or stopped) */
	if (*(u16 *)(pc - 4) == 0x4E72) { stops = 1; return 1; }
	pc -= port_ram - PORT_RAM;
	for (int i = 0; idles[i]; i += 2) if (pc >= idles[i] && pc <= idles[i + 1]) return 1;
	return 0;
}
void layer_idle(u32 pc) {
	if (!busy && drawn_seq == vint_seq) return;   /* nothing new to show */
	if (!waiting(pc)) return;
	deadline = 300;                       /* (the vertical blank's interrupt is to come on time) */
	if (late()) return;
	work_at = vbl_count;
	picture();
}

/* after the game's vertical interrupt. A game that never waits (it takes more than a frame, every
 * frame, or agk port found no such loop) is given its picture here, a slice of the frame at a time. */
void layer_frame(void) {
	layer_irq_leave();
	in_vint = 0;
	vint_seq++;
	if ((idles[0] || stops) && vbl_count - work_at < 6) return;
	deadline = raster() + SLICE;          /* (without it a busy game would never be given a picture) */
	if (deadline < DEADLINE) deadline = DEADLINE;
	if (deadline > 300) deadline = 300;
	if (late()) return;
	picture();
}

/* the cells and the sprites, then the display that shows them from the next vertical blank; as far
 * as the time goes (deadline), and gone on with the next time */
static void picture(void) {
	int p, i;
	if (hs_dirty) geometry();
	if (scene_new) {                      /* the planes were set up anew: the colours are found again as tiles are drawn */
		scene_new = 0;
		for (p = 0; p < 2; p++) {
			for (i = 0; i < 4; i++) seen[p][i] = 0;
			for (i = 0; i < 64; i++) map[p][i] = 0;
			for (i = 0; i < 2048; i++) cache_ok[p][i] = 0;
			nslot[p] = want_group[p] = lut_ok[p] = 0;
		}
		for (i = 0; i < 64; i++) anim[i] = anim_n[i] = 0;
		cram_dirty = 1;
	}
	if (cram_dirty) {
		int err[2], n = 0;
		cram_dirty = 0;
		/* a few colours changing by themselves, again and again: a cycle (a fade changes most of them) */
		for (i = 0; i < 64; i++) n += cram_chg[i];
		if (n > 16) {                     /* (a slow machine's fade has long steps: it isn't over between two of them) */
			fade_gap = vbl_count - fade_at;
			if (fade_gap > 100) fade_gap = 100;
			fade_at = vbl_count;
		}
		for (i = 0; i < 64; i++) {
			if (n > 16) anim[i] = anim_n[i] = 0;
			else if (cram_chg[i] && !anim[i] && ++anim_n[i] >= 3) {
				anim[i] = 1;
				for (p = 0; p < 2; p++) if (seen[p][i >> 4] >> (i & 15) & 1) want_group[p] = 1;
			}
			cram_chg[i] = 0;
		}
		load_palette(err);
		for (p = 0; p < 2; p++) if (err[p] > 60) want_group[p] = 1;   /* (a fade came up: what was grouped in the dark) */
	}
	/* (not while a fade is moving: every step of it would be a new grouping and a screenful of cells) */
	for (p = 0; p < 2; p++) if (want_group[p] && vbl_count - grouped_at[p] >= 25 && vbl_count - fade_at >= 12 + 3 * fade_gap) {
		want_group[p] = 0;
		if (group(p)) {
			DBG(dbg_group++);
			for (i = 0; i < 2048; i++) cache_ok[p][i] = 0;
			cell_all[p] = 1;
		}
		pal_stale = 1;
	}
	if (tiles_dirty) {                    /* tiles the game rewrote: their planar copies go, and the cells showing them */
		int scan = 0, lo = td_lo, hi = td_hi;
		tiles_dirty = 0; td_lo = 2048; td_hi = -1;
		for (i = lo; i <= hi; i++) if (tile_dirty[i]) { cache_ok[0][i] = cache_ok[1][i] = 0; if (tile_in_nt[i]) scan = 1; else tile_dirty[i] = 0; }
		if (scan) {
			for (p = 0; p < 2; p++) {
				const u8 *t = &vram[nt[p] & 0xFFFF];
				int count = pw * ph > 4096 ? 4096 : pw * ph;
				for (i = 0; i < count; i++, t += 2) if (tile_dirty[(t[0] & 7) << 8 | t[1]]) entry_dirty(p, i);
			}
			if (wlines) {
				const u8 *t = &vram[(wnt + ((u32)wrow0 << ((reg[12] & 1) ? 7 : 6))) & 0xFFFF];
				int count = (wlines >> 3) << ((reg[12] & 1) ? 6 : 5);
				for (i = 0; i < count; i++, t += 2) if (tile_dirty[(t[0] & 7) << 8 | t[1]]) window_entry((u32)i + ((u32)wrow0 << ((reg[12] & 1) ? 6 : 5)));
			}
			for (i = lo; i <= hi; i++) if (tile_dirty[i]) tile_dirty[i] = tile_in_nt[i] = 0;   /* (drawn again, they say so again) */
		}
	}
	if (cell_all[0]) wall = 1;            /* (plane A's colours moved: the window's tiles are in them) */
	for (p = 0; p < 2; p++) if (cell_all[p]) { cell_all[p] = 0; all_dirty(p); }
	int s = back * 2;                     /* plane A's bitmap that isn't on screen */
	if (!busy) {                          /* a new picture: the sprites as they are now */
		for (i = 0; i < got_n[back]; i++) { u32 c = got_list[back][i]; got_flag[back][c] = 0; valid[s][c] = 0; }   /* the sprites it had */
		got_n[back] = 0;
		for (i = 0; i < bob_n; i++) { bob_flag[bob_list[i]] = 0; sh_ok[0][bob_list[i]] = 0; }   /* and the shadow's */
		bob_n = 0;
		if (reg[1] & 0x40) sprites();
		busy = 1;
		drawn_seq = vint_seq;
		began_at = vbl_count;
		rest = 0;
	}
	rest++;
	/* the cells in view, as far as the time goes; a picture not whole by then is gone on with after the
	 * next blank (for the scroll as it is then), and what is on screen stays meanwhile */
	window();
	if ((!present(s, 0) || !present(s + 1, 1)) && vbl_count - began_at < 8) return;
	/* (eight blanks and still not whole: the game changes more a frame than there is time for. It is shown
	 * as far as it got: cells out of date here and there are better than no picture) */
	if (pal_stale) { int err[2]; pal_stale = 0; load_palette(err); }   /* (colours that came with this frame's tiles) */
	build_copper();
	back ^= 1;
	busy = 0;
	DBG(dbg_flip++);
#ifdef LAYER_DEBUG                        /* the layer's state where a memory dump finds it */
	{ u8 *d = (u8 *)0x900000; for (i = 0; i < 24; i++) d[i] = reg[i];
	  for (i = 0; i < 64; i++) { d[32 + i * 2] = (u8)(cram[i] >> 8); d[33 + i * 2] = (u8)cram[i]; }
	  for (i = 0; i < 40; i++) { d[160 + i * 2] = (u8)(vsram[i] >> 8); d[161 + i * 2] = (u8)vsram[i]; }
	  d[240] = (u8)rest; d[241] = (u8)bob_n; d[242] = nslot[0]; d[243] = nslot[1];
	  { const u8 *m = &map[0][0]; for (i = 0; i < 128; i++) d[256 + i] = m[i]; }
	  d[452] = (u8)(dbg_cells >> 8); d[453] = (u8)dbg_cells; d[454] = (u8)(dbg_tiles >> 8); d[455] = (u8)dbg_tiles; dbg_cells = dbg_tiles = 0;
	  for (i = 0; i < 64; i++) d[464 + i] = anim[i];
	  d[448] = (u8)back; d[451]++;
	  if (dbg_snap) { dbg_snap = 0; d = (u8 *)0x910000; for (u32 k = 0; k < 0x10000; k++) d[k] = vram[k]; } }   /* (F1) */
#endif
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
	if (code == 0x50) dbg_snap = 1;
	if (code & 0x80) keys &= (u16)~bit; else keys |= bit;
}

/* the vertical blank: the pads. -> nonzero if the game's own vertical interrupt is enabled */
static int waiting(u32 pc);
#ifdef LAYER_DEBUG_TRACE
static u16 tr[48][3];
static int tn;
#endif
int layer_vbl(u32 pc) {
	vbl_count++;
	DBG(dbg_vint += (u32)waiting(pc));
	DBG(dbg_w2 += (pc - (port_ram - PORT_RAM) >= idles[2] && idles[2]));
	DBG(dbg_en += (reg[1] >> 5) & 1);
#ifdef LAYER_DEBUG_TRACE                  /* 48 blanks from the 2400th: the line the blank's interrupt came at, whether the game waited, its frame count */
	{ u32 n = vbl_count - LAYER_DEBUG_TRACE;
	  if (n < 48) { tr[n][0] = (u16)raster(); tr[n][1] = (u16)(waiting(pc) | busy << 4 | (vint_seq - drawn_seq) << 8); tr[n][2] = (u16)mem16(port_ram + 0xFE04); }
	  else if (n < 96 && tn < 48) { ser_hex(tr[tn][0], 3); ser_hex(tr[tn][1], 4); ser_hex(tr[tn][2], 4); ser('\n'); tn++; } }
#endif
#ifdef LAYER_DEBUG                        /* every 256 blanks: pictures shown, geometries, groupings, plane set-ups; the game's reads and writes of
	                                        * hardware, its words to the VDP's data port, words by DMA, and the frames it finished in time */
	if (!(vbl_count & 255)) {
		ser_hex((u32)dbg_flip, 2); ser_hex((u32)dbg_geo, 2); ser_hex((u32)dbg_group, 2); ser_hex((u32)dbg_moved, 2); ser_hex(dbg_rd, 5); ser_hex(dbg_wr, 5); ser_hex(dbg_data, 5); ser_hex(dbg_dma, 5); ser_hex(dbg_vint, 3); ser_hex(dbg_w2, 3); ser_hex(dbg_en, 3); ser_hex(mem16(port_ram + 0xFE04), 4); ser('\n'); dbg_w2 = dbg_en = 0;
		dbg_rd = dbg_wr = dbg_data = dbg_dma = dbg_vint = 0;
		dbg_flip = dbg_geo = dbg_group = dbg_moved = 0;
	}
#endif
	keyboard();
	u16 j = CUSTOM(0x00C), s = keys;
	if (j & 2) s |= 8;
	if (j & 0x200) s |= 4;
	if ((j ^ j >> 1) & 1) s |= 2;
	if ((j ^ j >> 1) & 0x100) s |= 1;
	if (!(CIAA(0) & 0x80)) s |= 16;
	if (!(CUSTOM(0x016) & 0x4000)) s |= 128;
	pad_state[0] = s;
	layer_irq_enter();
	in_vint = 1;
	return reg[1] & 0x20;
}

void layer_init(void) {
	u32 i;
	for (i = 0; i < 256; i++) { u8 v = 0; for (int k = 0; k < 8; k++) if (i >> k & 1) v |= (u8)(0x80 >> k); rev[i] = v; }
	for (i = 0; i < 256; i++) used[i] = (u16)(1 << (i >> 4) | 1 << (i & 15));
	for (i = 0; i < 4 * PF_BYTES; i++) surf[0][i] = 0;
#ifdef LAYER_DEBUG
	CUSTOM(0x032) = 372;                  /* where the program was loaded, on the serial line (a profile needs it) */
	ser_hex((u32)layer_init, 8); ser('\n');
#endif
	tables_moved();
	window_moved();
	for (i = 0; i < 4 * WIN_PLANE; i++) wsurf[i] = 0;
	for (int k = 0; k < 2; k++) { cop[k][0] = 0xFFFF; cop[k][1] = 0xFFFE; }
	CUSTOM(0x100) = 0x0201;
	CUSTOM(0x102) = 0;
	CUSTOM(0x104) = 0x0024;               /* BPLCON2: plane A in front; sprites over both */
	CUSTOM(0x106) = 0x1000;               /* BPLCON3: the second playfield's colours from 16 */
	CUSTOM(0x180) = 0;
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
