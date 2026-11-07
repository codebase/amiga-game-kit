/*
 * The chips a recompiled game talks to, standing in for an A500: memory
 * (chip and slow RAM), the custom registers (DMA control, interrupts, the
 * blitter, the copper and the display, sprites, the disk, Paula's audio
 * registers), the two CIAs (timers, the joystick's fire button, the disk's
 * motor and stepping), and time: cycles become raster lines, lines become
 * frames, a frame renders the picture and raises the vertical-blank
 * interrupt.
 *
 * Headless: the picture goes to a buffer (frame_rgb) that main.c writes as
 * PNG or hands to SDL. Nothing here is cycle-exact; it's exact enough for
 * games that wait for the blank and the blitter, which is nearly all of
 * them.
 */
#include "m68k.h"
#include <string.h>

cpu_t cpu;
u8 *mem;
u64 next_event;
int irq_pending;

/* --------------------------------------------------------------- time */

#define CYCLES_PER_LINE 227               /* 7.09 MHz / 15.6 kHz, near enough */
#define LINES_PER_FRAME 313
#define CYCLES_PER_FRAME (CYCLES_PER_LINE * LINES_PER_FRAME)

static u64 frame_start;                   /* cpu.cycles at the start of this frame */
static u64 disk_done_at;                  /* when the running disk DMA finishes (0: none) */
u32 frame_count;
int quiet;                                /* no warnings about unknown registers */

static u32 beam_line(void) { return (u32)((cpu.cycles - frame_start) / CYCLES_PER_LINE); }

/* ------------------------------------------------------ custom chips */

static u16 reg[0x100];                    /* the last value written to each register (index = offset / 2) */
u32 stat_blits, stat_cop1lc;              /* per frame, for --trace: how the game paces itself */
extern u8 mouse_x, mouse_y, mouse_left, mouse_right;
#define R(off) reg[(off) >> 1]
#define DMACON R(0x096)
#define INTENA R(0x09A)
#define INTREQ R(0x09C)
#define ADKCON R(0x09E)

static u32 cop1lc, cop2lc, dskpt, bltpt[4], audlc[4], sprpt[8], bplpt[8];
static u16 dsklen_last;
static int copjmp;                        /* 1 or 2: a COPJMP happened this frame */

u8 frame_rgb[320 * 256 * 3];
static u16 colors[32];

static void irq_update(void) {
	u16 pend = INTREQ & INTENA;
	int level = 0;
	if (INTENA & 0x4000) {
		if (pend & 0x2000) level = 6;             /* EXTER: CIA-B */
		else if (pend & 0x1F80) level = (pend & 0x0780) ? 4 : 5;   /* audio: 4; RBF/DSKSYNC: 5 */
		else if (pend & 0x0070) level = 3;        /* VERTB, COPER, BLIT */
		else if (pend & 0x0008) level = 2;        /* PORTS: CIA-A */
		else if (pend & 0x0007) level = 1;        /* TBE, DSKBLK, SOFT */
	}
	irq_pending = level;
}

static void intreq_set(u16 bits) { INTREQ |= bits; irq_update(); }

/* the blitter: amiga_blit.c */
void blitter_run(u16 *regs, u32 *pts);
/* the display: amiga_display.c */
void display_render(void);
/* the disk: amiga_disk.c */
void disk_dma(u32 pt, u16 len);
void disk_cia_write(u8 prb);
u8 disk_cia_status(void);

void chip_write(u32 addr, u32 value, int bits) {
	u32 off = addr & 0x1FE;
	if (bits == 32) { chip_write(addr, value >> 16, 16); chip_write(addr + 2, value & 0xFFFF, 16); return; }
	if (bits == 8) { u16 cur = R(off); value = (addr & 1) ? (cur & 0xFF00) | (value & 0xFF) : (cur & 0x00FF) | (value << 8); }
	u16 v = (u16)value;
	switch (off) {
	case 0x020: dskpt = (dskpt & 0xFFFF) | ((u32)(v & 0x1F) << 16); break;
	case 0x022: dskpt = (dskpt & 0xFFFF0000u) | (v & 0xFFFE); break;
	case 0x024:                                   /* DSKLEN: written twice with DMAEN to start */
		if ((v & 0x8000) && (dsklen_last & 0x8000) && (DMACON & 0x0210) == 0x0210) disk_dma(dskpt, v);
		dsklen_last = v; R(off) = v; break;
	case 0x040: case 0x042: case 0x044: case 0x046: case 0x060: case 0x062: case 0x064: case 0x066:
	case 0x070: case 0x072: case 0x074:
		R(off) = v; break;
	case 0x048: bltpt[2] = (bltpt[2] & 0xFFFF) | ((u32)(v & 0x1F) << 16); break;
	case 0x04A: bltpt[2] = (bltpt[2] & 0xFFFF0000u) | (v & 0xFFFE); break;
	case 0x04C: bltpt[1] = (bltpt[1] & 0xFFFF) | ((u32)(v & 0x1F) << 16); break;
	case 0x04E: bltpt[1] = (bltpt[1] & 0xFFFF0000u) | (v & 0xFFFE); break;
	case 0x050: bltpt[0] = (bltpt[0] & 0xFFFF) | ((u32)(v & 0x1F) << 16); break;
	case 0x052: bltpt[0] = (bltpt[0] & 0xFFFF0000u) | (v & 0xFFFE); break;
	case 0x054: bltpt[3] = (bltpt[3] & 0xFFFF) | ((u32)(v & 0x1F) << 16); break;
	case 0x056: bltpt[3] = (bltpt[3] & 0xFFFF0000u) | (v & 0xFFFE); break;
	case 0x058: stat_blits++; R(off) = v; blitter_run(reg, bltpt); intreq_set(0x0040); break;   /* BLTSIZE starts it; done at once */
	case 0x080: stat_cop1lc++; cop1lc = (cop1lc & 0xFFFF) | ((u32)(v & 0x1F) << 16); break;
	case 0x082: cop1lc = (cop1lc & 0xFFFF0000u) | (v & 0xFFFE); break;
	case 0x084: cop2lc = (cop2lc & 0xFFFF) | ((u32)(v & 0x1F) << 16); break;
	case 0x086: cop2lc = (cop2lc & 0xFFFF0000u) | (v & 0xFFFE); break;
	case 0x088: copjmp = 1; break;
	case 0x08A: copjmp = 2; break;
	case 0x096: { u16 was = DMACON; DMACON = (v & 0x8000) ? (DMACON | (v & 0x7FFF)) : (DMACON & ~(v & 0x7FFF));
		extern void audio_dmacon(u16, u16); audio_dmacon(was, DMACON); break; }
	case 0x09A: INTENA = (v & 0x8000) ? (INTENA | (v & 0x7FFF)) : (INTENA & ~(v & 0x7FFF)); irq_update(); break;
	case 0x09C: INTREQ = (v & 0x8000) ? (INTREQ | (v & 0x7FFF)) : (INTREQ & ~(v & 0x7FFF)); irq_update(); break;
	case 0x09E: ADKCON = (v & 0x8000) ? (ADKCON | (v & 0x7FFF)) : (ADKCON & ~(v & 0x7FFF)); break;
	default:
		if (off >= 0x0A0 && off < 0x0E0) {        /* audio */
			int ch = (off - 0xA0) / 16, r = (off - 0xA0) % 16;
			if (r == 0) audlc[ch] = (audlc[ch] & 0xFFFF) | ((u32)(v & 0x1F) << 16);
			else if (r == 2) audlc[ch] = (audlc[ch] & 0xFFFF0000u) | (v & 0xFFFE);
			else R(off) = v;
		} else if (off >= 0x0E0 && off < 0x100) {  /* BPLxPT */
			int p = (off - 0xE0) / 4;
			if (off & 2) bplpt[p] = (bplpt[p] & 0xFFFF0000u) | (v & 0xFFFE); else bplpt[p] = (bplpt[p] & 0xFFFF) | ((u32)(v & 0x1F) << 16);
		} else if (off >= 0x120 && off < 0x140) {  /* SPRxPT */
			int p = (off - 0x120) / 4;
			if (off & 2) sprpt[p] = (sprpt[p] & 0xFFFF0000u) | (v & 0xFFFE); else sprpt[p] = (sprpt[p] & 0xFFFF) | ((u32)(v & 0x1F) << 16);
		} else if (off >= 0x180 && off < 0x1C0) {
			colors[(off - 0x180) / 2] = v & 0xFFF;
		} else {
			R(off) = v;                           /* BPLCON, DIW, DDF, modulos, sprite data...: kept for the display */
		}
	}
}

u32 chip_read(u32 addr, int bits) {
	u32 off = addr & 0x1FE;
	if (bits == 32) return chip_read(addr, 16) << 16 | chip_read(addr + 2, 16);
	u32 v;
	switch (off) {
	case 0x002: v = (DMACON & 0x7FFF) | 0x2000;   /* DMACONR: the blitter is never busy, BZERO from the blit */
		v = (v & ~0x4000) | (R(0x002) & 0x6000); break;
	case 0x004: { u32 line = beam_line(); v = (line >> 8) & 1; break; }          /* VPOSR */
	case 0x006: { u32 line = beam_line(); v = ((line & 0xFF) << 8) | (((cpu.cycles - frame_start) % CYCLES_PER_LINE) & 0xFF); break; }  /* VHPOSR */
	case 0x00A: v = (u16)(mouse_y << 8 | mouse_x); break;   /* JOY0DAT: the mouse */
	case 0x00C: { extern u16 joy1dat; v = joy1dat; break; }
	case 0x010: v = ADKCON; break;
	case 0x016: v = 0xFF00 & ~(mouse_right ? 0x0400 : 0); break;   /* POTGOR: the right mouse button is bit 10 */
	case 0x01A: v = 0x8000; break;                /* DSKBYTR: nothing */
	case 0x01C: v = INTENA; break;
	case 0x01E: v = INTREQ; break;
	default: v = R(off); break;
	}
	return bits == 8 ? ((addr & 1) ? v & 0xFF : v >> 8) : v;
}

/* ------------------------------------------------------------ the CIAs */

typedef struct {
	u8 pra, prb, ddra, ddrb, icr_mask, icr_data;
	u16 ta, ta_latch, tb, tb_latch;
	u8 cra, crb, sdr;
	u32 tod;
} cia_t;
static cia_t ciaa, ciab;
u16 joy1dat;
u8 joy_fire;                              /* 1: pressed */
u8 mouse_x, mouse_y;                      /* the mouse's counters (JOY0DAT) */
u8 mouse_left, mouse_right;

/* the keyboard: a byte at a time over CIA-A's serial port, each raising the SP
 * interrupt; the computer answers by pulsing the line (CRA's SPMODE) before
 * the next one is sent */
static u8 kbd_queue[64];
static int kbd_head, kbd_tail, kbd_busy;
static u32 kbd_sent_frame;
void kbd_push(u8 code) { if (((kbd_tail + 1) & 63) != kbd_head) { kbd_queue[kbd_tail] = code; kbd_tail = (kbd_tail + 1) & 63; } }

static void cia_irq(cia_t *c, u8 bit) {
	c->icr_data |= bit;
	if (c->icr_mask & bit) {
		c->icr_data |= 0x80;
		intreq_set(c == &ciaa ? 0x0008 : 0x2000);
	}
}

void cia_write(u32 addr, u8 value) {
	cia_t *c = (addr & 0x1000) ? &ciab : &ciaa;   /* $BFD000 = B, $BFE001 = A */
	if (((addr & 0xFFF000u) == 0xBFE000u) != (c == &ciaa)) c = (addr & 0xFFF000u) == 0xBFE000u ? &ciaa : &ciab;
	int r = (addr >> 8) & 15;
	switch (r) {
	case 0: c->pra = value; break;
	case 1: c->prb = value; if (c == &ciab) disk_cia_write(value); break;
	case 2: c->ddra = value; break;
	case 3: c->ddrb = value; break;
	case 4: c->ta_latch = (c->ta_latch & 0xFF00) | value; break;
	case 5: c->ta_latch = (c->ta_latch & 0x00FF) | (value << 8); if (!(c->cra & 1)) c->ta = c->ta_latch; break;
	case 6: c->tb_latch = (c->tb_latch & 0xFF00) | value; break;
	case 7: c->tb_latch = (c->tb_latch & 0x00FF) | (value << 8); if (!(c->crb & 1)) c->tb = c->tb_latch; break;
	case 13: if (value & 0x80) c->icr_mask |= value & 0x7F; else c->icr_mask &= ~(value & 0x7F); break;
	case 14:
		if (c == &ciaa && (c->cra & 0x40) && !(value & 0x40)) kbd_busy = 0;   /* the keyboard handshake is over */
		c->cra = value; if (value & 0x10) c->ta = c->ta_latch; break;
	case 15: c->crb = value; if (value & 0x10) c->tb = c->tb_latch; break;
	default: break;
	}
}

u8 cia_read(u32 addr) {
	cia_t *c = (addr & 0xFFF000u) == 0xBFE000u ? &ciaa : &ciab;
	int r = (addr >> 8) & 15;
	switch (r) {
	case 0:
		/* CIA-A: /OVL /LED, the drive's /CHNG /WPRO /TK0 /RDY, /FIR0 (the mouse, up), /FIR1 (the joystick) */
		if (c == &ciaa) return (u8)((c->pra & c->ddra & 3) | (disk_cia_status() & 0x3C) | (mouse_left ? 0 : 0x40) | (joy_fire ? 0 : 0x80));
		return (u8)((c->pra & c->ddra) | (~c->ddra & 0xFF));   /* CIA-B: the serial port's handshake lines, floating high */
	case 1: return c->prb;
	case 2: return c->ddra;
	case 3: return c->ddrb;
	case 4: return (u8)c->ta;
	case 5: return (u8)(c->ta >> 8);
	case 6: return (u8)c->tb;
	case 7: return (u8)(c->tb >> 8);
	case 8: case 9: case 10: {                /* TOD: CIA-A counts frames, CIA-B raster lines (HSYNC) */
		u32 t = c == &ciab ? c->tod + (u32)((cpu.cycles - frame_start) / CYCLES_PER_LINE) : c->tod;
		return (u8)(t >> (8 * (r - 8)));
	}
	case 13: { u8 v = c->icr_data; c->icr_data = 0; return v; }
	case 12: return c->sdr;
	case 14: return c->cra;
	case 15: return c->crb;
	default: return 0;
	}
}

/* the CIA timers count at E / 10 = cycles / 10 */
static void cia_tick(cia_t *c, u32 ticks) {
	if (c->cra & 1) {
		if (c->ta > ticks) c->ta -= ticks;
		else { c->ta = c->ta_latch - (ticks - c->ta - 1) % (c->ta_latch + 1); cia_irq(c, 1); if (c->cra & 8) c->cra &= ~1; }
	}
	if ((c->crb & 1) && !(c->crb & 0x40)) {
		if (c->tb > ticks) c->tb -= ticks;
		else { c->tb = c->tb_latch - (ticks - c->tb - 1) % (c->tb_latch + 1); cia_irq(c, 2); if (c->crb & 8) c->crb &= ~1; }
	}
}

/* --------------------------------------------------------------- events */

static u64 last_cia_cycles;
int frame_done;                           /* for main.c: a frame was rendered since it last looked */

void run_events(void) {
	u32 ticks = (u32)((cpu.cycles - last_cia_cycles) / 10);
	if (ticks) { cia_tick(&ciaa, ticks); cia_tick(&ciab, ticks); last_cia_cycles += (u64)ticks * 10; }
	while (cpu.cycles - frame_start >= CYCLES_PER_FRAME) {
		frame_start += CYCLES_PER_FRAME;
		display_render();
		frame_count++;
		frame_done = 1;
		{ extern void frame_hook(void); frame_hook(); }
		ciaa.tod++; ciab.tod += LINES_PER_FRAME;
		if (kbd_busy && frame_count - kbd_sent_frame > 3) kbd_busy = 0;   /* (no handshake came: the game doesn't do it) */
		if (!kbd_busy && kbd_head != kbd_tail) {
			u8 code = kbd_queue[kbd_head]; kbd_head = (kbd_head + 1) & 63;
			ciaa.sdr = (u8)~((code << 1) | (code >> 7));   /* rotated and inverted, as the keyboard sends it */
			cia_irq(&ciaa, 0x08);
			kbd_busy = 1; kbd_sent_frame = frame_count;
		}
		if (DMACON & 0x0200) intreq_set(0x0020);  /* VERTB */
		{ extern int disk_motor_on(void); if (disk_motor_on() && frame_count % 10 == 0) cia_irq(&ciab, 0x10); }   /* the index pulse on CIA-B FLAG, 5 a second */
	}
	if (disk_done_at && cpu.cycles >= disk_done_at) { disk_done_at = 0; intreq_set(0x0002); }
	next_event = frame_start + CYCLES_PER_FRAME;
	if (next_event > cpu.cycles + 2000) next_event = cpu.cycles + 2000;   /* the CIA timers want checking */
	if (disk_done_at && disk_done_at < next_event) next_event = disk_done_at;
}

/* -------------------------------------------------------------- setup */

u32 *bplpt_ptr(void) { return bplpt; }
u32 *sprpt_ptr(void) { return sprpt; }
u16 *custom_regs(void) { return reg; }
u32 *audlc_ptr(void) { return audlc; }
u8 cia_pra_a(void) { return ciaa.pra; }       /* (bit 1: /LED, the audio filter) */
void intreq_set_audio(int c) { intreq_set((u16)(0x0080 << c)); }   /* AUD0..AUD3 */
u16 *color_regs(void) { return colors; }
u32 cop1lc_get(void) { return cop1lc; }
u32 cop2lc_get(void) { return cop2lc; }
/* DSKBLK arrives when the DMA would have finished (a word every 32 us): loaders
 * clear INTREQ right after starting the transfer and then wait for it */
void intreq_set_disk_after(u32 words) { disk_done_at = cpu.cycles + (u64)words * 227 + 500; if (disk_done_at < next_event) next_event = disk_done_at; }
int copjmp_take(void) { int j = copjmp; copjmp = 0; return j; }

void amiga_init(void) {
	mem = calloc(MEM_SIZE + 16, 1);          /* (+16: a long read at the very top of the space) */
	frame_start = 0;
	next_event = CYCLES_PER_FRAME;
	last_cia_cycles = 0;
	ciaa.pra = 0xFC; ciab.pra = 0xFF;
}

/* a register as it was at the snapshot: restore it (the set/clear ones as values) */
void amiga_set_reg(u32 off, u16 v) {
	if (off == 0x096 || off == 0x09A || off == 0x09C || off == 0x09E) R(off) = v;
	else chip_write(CUSTOM_BASE + off, v, 16);
	irq_update();
}

/* a CIA register as it was: reg 16/17 are the running timer counters */
void amiga_set_cia(int which, int r, u16 v) {
	cia_t *c = which ? &ciab : &ciaa;
	switch (r) {
	case 0: c->pra = (u8)v; break;
	case 1: c->prb = (u8)v; break;
	case 2: c->ddra = (u8)v; break;
	case 3: c->ddrb = (u8)v; break;
	case 4: c->ta_latch = v; break;
	case 6: c->tb_latch = v; break;
	case 13: c->icr_mask = (u8)v; break;
	case 14: c->cra = (u8)v; break;
	case 15: c->crb = (u8)v; break;
	case 16: c->ta = v; break;
	case 17: c->tb = v; break;
	}
}
