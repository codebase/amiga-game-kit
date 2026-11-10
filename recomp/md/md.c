/* The Mega Drive's chips for the recompiled 68000 code: the VDP (registers,
 * VRAM/CRAM/VSRAM, DMA, the scanline renderer for the two planes, the window
 * and the sprites, the interrupts and the HV counter), the controller ports,
 * the Z80 bus latches, and the memory ports in between. Time is the 68000's
 * cycle count: a line every 488 cycles, 262 lines a frame (NTSC); each line
 * is rendered as it ends, so palette and scroll changes from the horizontal
 * interrupt land on the right lines. The Z80 and the sound chips are not
 * run yet (the bus handshake is honoured so the game's driver loads). */
#include "host.h"
#include "start.h"
#include <string.h>

#ifndef DMA_RD16
#define DMA_RD16(a) rd16(a)                /* (a port's bus maps the game's old RAM addresses) */
#endif
#define CYCLES_PER_LINE 488               /* 7.67 MHz / 15.7 kHz */
#define LINES (START_HZ == 60 ? 262 : 313)
#define ACTIVE_LINES 224
#define CYCLES_PER_FRAME (CYCLES_PER_LINE * LINES)

u8 *mem;
cpu_t cpu;
u8 *romread_bits;                         /* (--analysis: see bus.h) */
int irq_pending;
u64 next_event;
u32 frame_count;
int frame_done;

/* ----------------------------------------------------------------- VDP */
static u8 vram[0x10000];
static u16 cram[64], vsram[40];
static u8 reg[24];
static u32 vdp_code, vdp_addr;            /* the pending access: CD0-5 and the address */
static int ctrl_pending;                  /* the first control word was written */
static u16 status = 0x3400;               /* FIFO empty, no DMA */
static int line;                          /* the line being generated */
static u64 line_start;
static int hint_counter;
static int vint_pending, hint_pending;
static int dma_fill_pending;
static u32 rgb[64];                       /* CRAM as RGB */
static u8 z80ram[0x2000];
static int z80_busreq, z80_reset = 1;
static u8 pad_th[2];                      /* the TH select line of each port */
static u8 pad_ctrl[2];
u16 pad_state[2];                         /* bits: 0 up 1 down 2 left 3 right 4 B 5 C 6 A 7 Start (1 = pressed) */
static u16 psg_last;
static u8 ym_reg[2][256], ym_addr[2];

static void cram_set(int i, u16 v) {
	cram[i] = v & 0xEEE;
	u32 r = (v >> 1) & 7, g = (v >> 5) & 7, b = (v >> 9) & 7;
	static const u8 lev[8] = { 0, 52, 87, 116, 144, 172, 206, 255 };
	rgb[i] = (u32)lev[r] << 16 | (u32)lev[g] << 8 | lev[b];
}

#ifdef PORT_RAM
/* a port under test: as on the Amiga, both interrupts arrive at level 3 (its vertical blank), the
 * vertical one first; the level 3 vector is pointed at the game's handler for whichever it is */
static int irq_which;
static void irq_update(void) {
	int lvl = 0;
	u32 from = 0;
	if (vint_pending && (reg[1] & 0x20)) { lvl = 3; from = 0x78; irq_which = 6; }
	else if (hint_pending && (reg[0] & 0x10)) { lvl = 3; from = 0x70; irq_which = 4; }
	if (lvl) for (int i = 0; i < 4; i++) mem[0x6C + i] = mem[from + i];
	irq_pending = lvl;
}
void port_irq_taken(void);
void irq_ack(int level) {
	(void)level;
	port_irq_taken();
	if (irq_which == 6) vint_pending = 0; else hint_pending = 0;
	irq_update();
}
#else
static void irq_update(void) {
	int lvl = 0;
	if (vint_pending && (reg[1] & 0x20)) lvl = 6;
	else if (hint_pending && (reg[0] & 0x10)) lvl = 4;
	irq_pending = lvl;
}
void irq_ack(int level) {
	if (level == 6) vint_pending = 0;
	else if (level == 4) hint_pending = 0;
	irq_update();
}
#endif

static void vdp_data_write(u16 v) {
	switch (vdp_code & 0xF) {
	case 1: vram[vdp_addr & 0xFFFE] = (u8)(v >> 8); vram[(vdp_addr & 0xFFFE) + 1] = (u8)v;
	        if (vdp_addr & 1) { vram[vdp_addr & 0xFFFE] = (u8)v; vram[(vdp_addr & 0xFFFE) + 1] = (u8)(v >> 8); } break;   /* (an odd address swaps the bytes) */
	case 3: cram_set((vdp_addr >> 1) & 63, v); break;
	case 5: vsram[((vdp_addr >> 1) % 40)] = v & 0x7FF; break;
	}
	vdp_addr = (vdp_addr + reg[15]) & 0xFFFF;
	if (dma_fill_pending) {               /* VRAM fill: the data write starts it; the high byte fills */
		dma_fill_pending = 0;
		u32 len = reg[19] | (u32)reg[20] << 8;
		if (!len) len = 0x10000;
		u32 a = vdp_addr & 0xFFFF;        /* (after the write above moved it: the fill runs from the next slot) */
		a = (a - reg[15]) & 0xFFFF;
		vram[a ^ 1] = (u8)v;              /* the first word's low byte went to the address, as the write did */
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

static int vdp_trace = -1;
#define VTRACE(...) do { if (vdp_trace < 0) vdp_trace = getenv("RECOMP_VDP_TRACE") != NULL; if (vdp_trace) { fprintf(stderr, "vdp: frame %u line %d ", frame_count, line); fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } } while (0)

static void dma_run(void) {
	/* a 68000-to-VDP transfer (reg 23 bit 7 clear), a fill (bits 7,6 = 1,0: waits for the data write) or a copy (1,1) */
	u32 len = reg[19] | (u32)reg[20] << 8;
	if (!len) len = 0x10000;
	int mode = reg[23] >> 6;
	VTRACE("DMA %s code %X dest %04X len %X src %06X inc %d", mode < 2 ? "68k" : mode == 2 ? "fill" : "copy", vdp_code & 0xF, vdp_addr, len,
	       (u32)(reg[23] & 0x7F) << 17 | (u32)reg[22] << 9 | (u32)reg[21] << 1, reg[15]);
	if (mode < 2) {
		u32 src = ((u32)(reg[23] & 0x7F) << 17 | (u32)reg[22] << 9 | (u32)reg[21] << 1);
		for (u32 i = 0; i < len; i++) {
			u16 v = (u16)DMA_RD16(src);
			src += 2;
			if ((src & 0x1FFFF) == 0) src -= 0x20000;   /* the source stays in its 128K bank */
			int save = dma_fill_pending; dma_fill_pending = 0;
			vdp_data_write(v);
			dma_fill_pending = save;
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
	if (!ctrl_pending && (v & 0xC000) == 0x8000) {   /* a register */
		int r = (v >> 8) & 0x1F;
		if (r < 24) {
			if (reg[r] != (u8)v && r != 10) VTRACE("reg %d = %02X", r, (u8)v);
			reg[r] = (u8)v;
			if (r == 0 || r == 1) irq_update();
		}
		vdp_code = vdp_code & ~3u;        /* (writing a register also resets the pending access) */
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
		if (!(vdp_code & 0x20)) VTRACE("set code %X addr %04X", vdp_code & 0xF, vdp_addr);
		if ((vdp_code & 0x20) && (reg[1] & 0x10)) dma_run();   /* CD5 with DMA enabled */
	}
}

static u16 hv_counter(void) {
	int cyc = (int)(cpu.cycles - line_start);
	int h = (cyc * 342 / CYCLES_PER_LINE) & 0xFF;   /* (the H counter runs 0..$FF then $E4..$FF in H40: near enough) */
	int v = line;
	if (START_HZ == 60) { if (v > 0xEA) v -= 6; }  /* 262 lines: $00..$EA then $E5..$FF */
	else { if (v > 0x102) v -= 56; }
	return (u16)((v & 0xFF) << 8 | h);
}

/* ------------------------------------------------------------ the ports */
static u8 pad_read(int p) {
	u16 s = pad_state[p];                 /* 1 = pressed; the port reads 0 for pressed */
	u8 v;
	if (pad_th[p]) v = (u8)(0x40 | ((s >> 5 & 1) << 5) | ((s >> 4 & 1) << 4) | (s & 0xF));          /* TH=1: ?1CBRLDU */
	else v = (u8)(((s >> 7 & 1) << 5) | ((s >> 6 & 1) << 4) | (s & 3));                                /* TH=0: ?0SA00DU */
	return (u8)(~v & 0x7F) | (pad_th[p] ? 0x40 : 0);
}

/* --analysis: which instructions touch the hardware, and what (pc, address, r/w, width): a port
 * replaces exactly these. Kept as a small open-addressed set; written as hwsites.txt, adding to what's there. */
#define NSITES 16384
static struct { u32 pc, addr; u8 rw, bits, used; } sites[NSITES];
static void site(u32 a, int rw, int bits) {
	if (!host_analysis_dir()) return;
	u32 h = (cpu.pc * 2654435761u ^ a * 40503u ^ (u32)rw << 7 ^ (u32)bits) % NSITES;
	for (int n = 0; n < NSITES; n++, h = (h + 1) % NSITES) {
		if (!sites[h].used) { sites[h].used = 1; sites[h].pc = cpu.pc; sites[h].addr = a; sites[h].rw = (u8)rw; sites[h].bits = (u8)bits; return; }
		if (sites[h].pc == cpu.pc && sites[h].addr == a && sites[h].rw == rw && sites[h].bits == bits) return;
	}
}
void md_save_sites(void) {
	const char *dir = host_analysis_dir();
	if (!dir) return;
	if (romread_bits) {                   /* what was read of the ROM, added to earlier runs' */
		char rp[1024]; snprintf(rp, sizeof rp, "%s/romread.bin", dir);
		FILE *rf = fopen(rp, "rb");
		if (rf) { for (u32 i = 0; i < REGION0_SIZE; i++) { int c = fgetc(rf); if (c == EOF) break; if (c && c != romread_bits[i]) romread_bits[i] = romread_bits[i] ? 255 : (u8)c; } fclose(rf); }
		rf = fopen(rp, "wb");
		if (rf) { fwrite(romread_bits, 1, REGION0_SIZE, rf); fclose(rf); }
	}
	char path[1024]; snprintf(path, sizeof path, "%s/hwsites.txt", dir);
	FILE *f = fopen(path, "a");
	if (!f) return;
	for (int i = 0; i < NSITES; i++) if (sites[i].used) fprintf(f, "%06X %c %06X %d\n", sites[i].pc, sites[i].rw, sites[i].addr, sites[i].bits);
	fclose(f);
}

static u32 md_io_read_(u32 a, int bits);
static void md_io_write_(u32 a, u32 v, int bits);
u32 md_io_read(u32 a, int bits) { site(a, 'r', bits); return md_io_read_(a, bits); }
void md_io_write(u32 a, u32 v, int bits) { site(a, 'w', bits); md_io_write_(a, v, bits); }

static u32 md_io_read_(u32 a, int bits) {
	if (a >= 0xC00000 && a < 0xC00010) {
		u32 v;
		switch (a & 0xE) {
		case 0x0: case 0x2: v = vdp_data_read(); ctrl_pending = 0; break;
		case 0x4: case 0x6:
			v = status;
			if (line >= ACTIVE_LINES) v |= 0x0008;              /* VBlank */
			if (vint_pending) v |= 0x0080;
			if ((int)(cpu.cycles - line_start) > CYCLES_PER_LINE * 320 / 488) v |= 0x0004;   /* HBlank */
			if (START_HZ == 50) v |= 1;
			ctrl_pending = 0;
			status &= ~0x0040u;                                 /* (sprite overflow is reported once) */
			break;
		default: v = hv_counter(); break;
		}
		return bits == 8 ? ((a & 1) ? v & 0xFF : v >> 8) : v;
	}
	if (a >= 0xA10000 && a < 0xA10020) {
		u32 v = 0;
		switch (a & 0x1E) {
		case 0x00: v = 0xA0; break;         /* version: overseas, NTSC, no FDD, no TMSS */
		case 0x02: v = pad_read(0); break;
		case 0x04: v = pad_read(1); break;
		case 0x06: v = 0x7F; break;
		case 0x08: v = pad_ctrl[0]; break;
		case 0x0A: v = pad_ctrl[1]; break;
		}
		return bits == 8 ? v : v << 8 | v;  /* (the byte is at the odd address; a word read sees it in both) */
	}
	if (a >= 0xA11100 && a < 0xA11102) { u32 v = (z80_busreq && !z80_reset) || !z80_busreq ? (z80_busreq ? 0 : 0x100) : 0; return bits == 8 ? ((a & 1) ? v & 0xFF : v >> 8) : v; }
	if (a >= 0xA00000 && a < 0xA10000) {   /* the Z80's side, while the 68000 holds its bus */
		if (a < 0xA04000) { u32 v = z80ram[a & 0x1FFF]; return bits == 8 ? v : v << 8 | v; }
		if (a < 0xA04004) return 0;         /* YM2612 status: never busy */
		return 0xFF;
	}
	return 0;
}

static void md_io_write_(u32 a, u32 v, int bits) {
	if (a >= 0xC00000 && a < 0xC00010) {
		if (bits == 8) v = (v & 0xFF) * 0x101;
		switch (a & 0xE) {
		case 0x0: case 0x2: ctrl_pending = 0; vdp_data_write((u16)v); break;
		case 0x4: case 0x6: vdp_ctrl_write((u16)v); break;
		}
		return;
	}
	if (a >= 0xC00010 && a < 0xC00018) { psg_last = (u16)v; return; }   /* the PSG */
	if (a >= 0xA10000 && a < 0xA10020) {
		int p = -1;
		switch (a & 0x1E) {
		case 0x02: p = 0; break; case 0x04: p = 1; break;
		case 0x08: pad_ctrl[0] = (u8)v; return; case 0x0A: pad_ctrl[1] = (u8)v; return;
		}
		if (p >= 0) pad_th[p] = (v >> 6) & 1;
		return;
	}
	if (a >= 0xA11100 && a < 0xA11102) { z80_busreq = (v >> 8) & 1 || (bits == 8 && !(a & 1) && (v & 1)); if (bits == 16) z80_busreq = (v >> 8) & 1; return; }
	if (a >= 0xA11200 && a < 0xA11202) { int run = bits == 16 ? (v >> 8) & 1 : (!(a & 1) ? v & 1 : z80_reset ? 0 : 1); z80_reset = !run; return; }
	if (a >= 0xA00000 && a < 0xA10000) {
		if (a < 0xA04000) { if (bits == 16) { z80ram[a & 0x1FFF] = (u8)(v >> 8); } else z80ram[a & 0x1FFF] = (u8)v; return; }
		if (a < 0xA04004) { int port = (a >> 1) & 1; if (a & 1) ym_reg[port][ym_addr[port]] = (u8)v; else ym_addr[port] = (u8)v; return; }
		return;                           /* the bank register, the Z80's VDP window: nothing yet */
	}
	/* TMSS ($A14000, $A14101) and the rest: nothing */
}

/* ------------------------------------------------------------- render */
static int h40(void) { return reg[12] & 1; }
/* interlace mode 2 (the two-player split screen): 8x16 cells of 64 bytes, 448 lines
 * of picture; the even field is shown, so output line y is picture line 2y */
static int ilace2(void) { return (reg[12] & 6) == 6; }

/* a line of a plane into `out` (palette index 0..63 | 0x40 for priority; 0 = transparent) */
static void plane_line(int y, int plane, u8 *out, int width) {
	static const int sizes[4] = { 32, 64, 32, 128 };
	int pw = sizes[reg[16] & 3], ph = sizes[(reg[16] >> 4) & 3];
	u32 nt = plane == 0 ? (u32)(reg[2] & 0x38) << 10 : (u32)(reg[4] & 7) << 13;
	u32 hs_table = (u32)(reg[13] & 0x3F) << 10;
	int hs_mode = reg[11] & 3;
	/* horizontal scroll for this line: full screen, per 8-line cell, or per line */
	u32 hs_off = hs_mode == 0 ? 0 : hs_mode == 2 ? (u32)(y & ~7) * 4 : hs_mode == 3 ? (u32)y * 4 : (u32)(y & 7) * 4;
	if (hs_mode == 1) hs_off = (u32)(y & ~7) * 4;   /* (invalid mode 1 behaves like per-cell on a real VDP, near enough) */
	int il = ilace2(), th = il ? 16 : 8, yy = il ? y * 2 : y;   /* cell height; the picture line */
	if (il) hs_off = hs_mode == 0 ? 0 : hs_mode == 3 ? (u32)yy * 4 : (u32)(yy & ~15) * 4;
	int hscroll = (int)((vram[(hs_table + hs_off + plane * 2) & 0xFFFE] << 8 | vram[(hs_table + hs_off + plane * 2 + 1) & 0xFFFF]) & 0x3FF);
	int vs_2cell = reg[11] & 4;
	for (int x = 0; x < width; x++) {
		int vscroll = vsram[vs_2cell ? (((x >> 4) * 2 + plane) % 40) : plane] & (il ? 0x7FF : 0x3FF);
		int px = (x - hscroll) & (pw * 8 - 1);
		int py = (yy + vscroll) & (ph * th - 1);
		u32 e = nt + (u32)((py / th) * pw + (px >> 3)) * 2;
		u16 entry = (u16)(vram[e & 0xFFFF] << 8 | vram[(e + 1) & 0xFFFF]);
		int tile = entry & 0x7FF, hf = entry & 0x800, vf = entry & 0x1000, pal = (entry >> 13) & 3, pri = entry & 0x8000;
		int ty = vf ? th - 1 - (py & (th - 1)) : py & (th - 1), tx = hf ? 7 - (px & 7) : px & 7;
		u8 b = vram[(tile * 4 * th + ty * 4 + (tx >> 1)) & 0xFFFF];
		int c = (tx & 1) ? b & 0xF : b >> 4;
		out[x] = c ? (u8)((pal << 4) | c | (pri ? 0x40 : 0)) : 0;
	}
}

/* the window plane where it covers this line, over plane A's buffer */
static void window_line(int y, u8 *a, int width) {
	int wx = (reg[17] & 0x1F) * 16, wright = reg[17] & 0x80;
	int wy = (reg[18] & 0x1F) * 8, wdown = reg[18] & 0x80;
	int in_y = wdown ? y >= wy : y < wy;
	int x0 = 0, x1 = 0;
	if (in_y && (wy || wdown)) { x0 = 0; x1 = width; }
	else if (wright) { x0 = wx; x1 = width; }
	else { x0 = 0; x1 = wx; }
	if (x0 >= x1) return;
	int ww = h40() ? 64 : 32;
	int il = ilace2(), th = il ? 16 : 8, yy = il ? y * 2 : y;
	u32 nt = h40() ? (u32)(reg[3] & 0x3C) << 10 : (u32)(reg[3] & 0x3E) << 10;
	for (int x = x0; x < x1 && x < width; x++) {
		u32 e = nt + (u32)((yy / th) * ww + (x >> 3)) * 2;
		u16 entry = (u16)(vram[e & 0xFFFF] << 8 | vram[(e + 1) & 0xFFFF]);
		int tile = entry & 0x7FF, hf = entry & 0x800, vf = entry & 0x1000, pal = (entry >> 13) & 3, pri = entry & 0x8000;
		int ty = vf ? th - 1 - (yy & (th - 1)) : yy & (th - 1), tx = hf ? 7 - (x & 7) : x & 7;
		u8 b = vram[(tile * 4 * th + ty * 4 + (tx >> 1)) & 0xFFFF];
		int c = (tx & 1) ? b & 0xF : b >> 4;
		a[x] = c ? (u8)((pal << 4) | c | (pri ? 0x40 : 0)) : 0;
	}
}

/* the sprites on this line (the link list from the table), low and high priority buffers */
static void sprite_line(int y, u8 *lo, u8 *hi, int width) {
	u32 table = h40() ? (u32)(reg[5] & 0x7E) << 9 : (u32)(reg[5] & 0x7F) << 9;
	int max = h40() ? 80 : 64, per_line = h40() ? 20 : 16, pixels = h40() ? 320 : 256;
	int n = 0, drawn = 0, px_budget = pixels;
	int masked = 0;
	int il = ilace2(), th = il ? 16 : 8, yy = il ? y * 2 : y;
	for (int link = 0, count = 0; count < max; count++) {
		u32 s = table + (u32)link * 8;
		int sy = ((vram[s & 0xFFFF] << 8 | vram[(s + 1) & 0xFFFF]) & 0x3FF) - (il ? 256 : 128);
		int size = vram[(s + 2) & 0xFFFF];
		int w = ((size >> 2) & 3) + 1, h = (size & 3) + 1;   /* in cells */
		u16 attr = (u16)(vram[(s + 4) & 0xFFFF] << 8 | vram[(s + 5) & 0xFFFF]);
		int sx = ((vram[(s + 6) & 0xFFFF] << 8 | vram[(s + 7) & 0xFFFF]) & 0x1FF) - 128;
		link = vram[(s + 3) & 0xFFFF] & 0x7F;
		if (yy >= sy && yy < sy + h * th) {
			if (sx == -128 && drawn) { masked = 1; }   /* x = 0: masks the rest of the line's sprites */
			else if (!masked) {
				int row = yy - sy;
				int tile = attr & 0x7FF, hf = attr & 0x800, vf = attr & 0x1000, pal = (attr >> 13) & 3, pri = attr & 0x8000;
				if (vf) row = h * th - 1 - row;
				for (int cx = 0; cx < w * 8; cx++) {
					int x = sx + cx;
					if (x < 0 || x >= width) continue;
					int col = hf ? w * 8 - 1 - cx : cx;
					int t = tile + (col >> 3) * h + row / th;
					u8 b = vram[(t * 4 * th + (row & (th - 1)) * 4 + ((col & 7) >> 1)) & 0xFFFF];
					int c = (col & 1) ? b & 0xF : b >> 4;
					if (!c) continue;
					u8 *dst = pri ? hi : lo;
					if (!dst[x] && !(pri ? lo[x] : 0)) dst[x] = (u8)((pal << 4) | c);
					else if (!dst[x]) dst[x] = (u8)((pal << 4) | c);
				}
				px_budget -= w * 8;
			}
			drawn++;
			n++;
			if (n >= per_line || px_budget <= 0) { status |= 0x0040; break; }
		}
		if (!link) break;
	}
}

static void render_line(int y) {
	if (y >= ACTIVE_LINES) return;
	int width = h40() ? 320 : 256, x0 = h40() ? 0 : 32;
	u8 *out = frame_rgb + y * 320 * 3;
	u32 bg = rgb[reg[7] & 0x3F];
	if (!(reg[1] & 0x40)) {               /* display off: the background colour */
		for (int x = 0; x < 320; x++) { out[x * 3] = (u8)(bg >> 16); out[x * 3 + 1] = (u8)(bg >> 8); out[x * 3 + 2] = (u8)bg; }
		return;
	}
	static u8 pa[320], pb[320], slo[320], shi[320];
	memset(slo, 0, sizeof slo); memset(shi, 0, sizeof shi);
	plane_line(y, 1, pb, width);
	plane_line(y, 0, pa, width);
	window_line(y, pa, width);
	sprite_line(y, slo, shi, width);
	for (int x = 0; x < 320; x++) {
		u32 c = bg;
		if (x >= x0 && x < x0 + width) {
			int i = x - x0;
			int a = pa[i], b = pb[i];
			/* back to front: B, A (low priority), sprites (low), B high, A high, sprites high */
			if (b & 0x3F) c = rgb[b & 0x3F];
			if ((a & 0x3F) && !(a & 0x40)) c = rgb[a & 0x3F];
			if (slo[i]) c = rgb[slo[i] & 0x3F];
			if ((b & 0x40)) c = rgb[b & 0x3F];
			if ((a & 0x40)) c = rgb[a & 0x3F];
			if (shi[i]) c = rgb[shi[i] & 0x3F];
		}
		out[x * 3] = (u8)(c >> 16); out[x * 3 + 1] = (u8)(c >> 8); out[x * 3 + 2] = (u8)c;
	}
}

/* --------------------------------------------------------------- time */
void run_events(void) {
	while (cpu.cycles - line_start >= CYCLES_PER_LINE) {
		/* the line that just ended */
		render_line(line);
		if (line < ACTIVE_LINES) {
			if (hint_counter-- == 0) { hint_counter = reg[10]; hint_pending = 1; irq_update(); }   /* the horizontal interrupt */
		} else hint_counter = reg[10];
		line_start += CYCLES_PER_LINE;
		line++;
		if (line == ACTIVE_LINES) { vint_pending = 1; irq_update(); }   /* the vertical blank begins */
		if (line >= LINES) {
			line = 0;
			frame_count++;
			frame_done = 1;
			frame_hook();
		}
	}
	next_event = line_start + CYCLES_PER_LINE;
}

/* RECOMP_VDP_DUMP=FILE: the VDP's state when the run ends (regs, VRAM, CRAM, VSRAM), for looking at offline */
void md_dump_vdp(void) {
	const char *path = getenv("RECOMP_VDP_DUMP");
	if (!path) return;
	FILE *f = fopen(path, "wb");
	if (!f) return;
	fwrite(reg, 1, 24, f); fwrite(vram, 1, sizeof vram, f); fwrite(cram, 2, 64, f); fwrite(vsram, 2, 40, f);
	fclose(f);
}

void frame_sync(void) { run_events(); }   /* (the VDP already paces the game by the blank) */
void frame_wait(void) { run_events(); }

void md_init(void) {
	mem = calloc(MEM_SIZE + 0x10000, 1);
	if (host_analysis_dir()) romread_bits = calloc(REGION0_SIZE, 1);
	line_start = 0;
	next_event = CYCLES_PER_LINE;
	reg[10] = 0xFF;
	hint_counter = 0xFF;
	for (int i = 0; i < 64; i++) cram_set(i, 0);
}

/* silence until the Z80, the YM2612 and the PSG are run */
void audio_render(s16 *out, int n, int rate) { (void)rate; memset(out, 0, (size_t)n * 4); }
void machine_audio_tag(void) {}
