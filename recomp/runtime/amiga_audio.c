/* Paula: four DMA channels playing 8-bit samples from chip RAM at the rate
 * AUDxPER sets, at AUDxVOL, channels 0 and 3 on the left, 1 and 2 on the
 * right. A channel that reaches the end of its AUDxLEN words reloads the
 * pointer and the length from the registers (which the music driver has
 * meanwhile pointed at the next sample or a silent loop) and raises its
 * interrupt. Rendered a frame at a time, from the registers as they stand:
 * drivers set them once per vertical blank. */
#include "m68k.h"
#include <math.h>

u16 *custom_regs(void);
u8 cia_pra_a(void);
u32 *audlc_ptr(void);
void intreq_set_audio(int ch);

#define PAULA_CLOCK 3546895.0            /* PAL: the period counts these */

typedef struct {
	int on, pending;                      /* DMA enabled; a restart requested by DMACON */
	u32 ptr;                              /* the word being played */
	int left;                             /* words left after it, until the reload */
	int half;                             /* 0: the high byte, 1: the low byte */
	double phase;                         /* output samples to the next Paula sample, fractional */
} chan_t;

static chan_t ch[4];

/* DMACON changed: a channel switched on restarts from AUDxLC/AUDxLEN */
void audio_dmacon(u16 before, u16 after) {
	for (int c = 0; c < 4; c++) {
		int was = (before & 0x200) && (before & (1 << c)), now = (after & 0x200) && (after & (1 << c));
		if (now && !was) ch[c].pending = 1;
		if (!now) ch[c].on = 0;
	}
}

static void start(int c, const u16 *reg, const u32 *lc) {
	ch[c].ptr = lc[c];
	ch[c].left = reg[(0xA4 + 16 * c) >> 1];
	if (ch[c].left == 0) ch[c].left = 0x10000;
	ch[c].half = 0;
	ch[c].phase = 0;
	ch[c].on = 1;
	ch[c].pending = 0;
}

/* n stereo frames of 16-bit output at `rate` */
void audio_render(s16 *out, int n, int rate) {
	u16 *reg = custom_regs();
	u32 *lc = audlc_ptr();
	for (int c = 0; c < 4; c++)
		if (ch[c].pending) start(c, reg, lc);
	static double lp[6];
	double a_fixed = 1.0 - exp(-2.0 * M_PI * 4900.0 / rate), a_led = 1.0 - exp(-2.0 * M_PI * 3300.0 / rate);
	int led = !(cia_pra_a() & 2);         /* /LED low: the LED is on and so is the filter */
	for (int i = 0; i < n; i++) {
		int l = 0, r = 0;
		for (int c = 0; c < 4; c++) {
			if (!ch[c].on) continue;
			int per = reg[(0xA6 + 16 * c) >> 1];
			if (per < 124) per = 124;         /* Paula can't go faster */
			int vol = reg[(0xA8 + 16 * c) >> 1] & 0x7F;
			if (vol > 64) vol = 64;
			ch[c].phase += PAULA_CLOCK / per / rate;
			while (ch[c].phase >= 1.0) {
				ch[c].phase -= 1.0;
				if (++ch[c].half == 2) {
					ch[c].half = 0;
					ch[c].ptr = (ch[c].ptr + 2) & 0x1FFFFE;
					if (--ch[c].left == 0) {  /* the sample is over: loop on what the registers say now */
						ch[c].ptr = lc[c];
						ch[c].left = reg[(0xA4 + 16 * c) >> 1];
						if (ch[c].left == 0) ch[c].left = 0x10000;
						intreq_set_audio(c);
					}
				}
			}
			int s = (s8)mem[(ch[c].ptr + (u32)ch[c].half) & 0x1FFFFF] * vol;   /* -8128..8128 */
			if (c == 0 || c == 3) l += s; else r += s;
		}
		/* the A500's output filter (one pole near 5 kHz) and, with the power LED
		 * lit, the "LED filter" (two poles near 3.3 kHz) that games switch through CIA-A */
		double fl = (double)l * 2, fr = (double)r * 2;
		fl = lp[0] += (fl - lp[0]) * a_fixed; fr = lp[1] += (fr - lp[1]) * a_fixed;
		if (led) {
			fl = lp[2] += (fl - lp[2]) * a_led; fl = lp[3] += (fl - lp[3]) * a_led;
			fr = lp[4] += (fr - lp[4]) * a_led; fr = lp[5] += (fr - lp[5]) * a_led;
		}
		l = (int)fl; r = (int)fr;
		out[2 * i] = (s16)(l > 32767 ? 32767 : l < -32768 ? -32768 : l);
		out[2 * i + 1] = (s16)(r > 32767 ? 32767 : r < -32768 ? -32768 : r);
	}
}
