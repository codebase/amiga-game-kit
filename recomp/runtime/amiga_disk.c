/* The floppy: the user's ADF, served as the MFM the hardware would read.
 * The game steps the head through CIA-B, starts a DMA read with DSKLEN,
 * and gets a track's worth of raw MFM at DSKPT; its own loader decodes
 * the sectors. The track is built from the ADF in AmigaDOS's format
 * (11 sectors, $4489 syncs, odd/even bits) when it's asked for. */
#include "m68k.h"
#include <stdlib.h>
#include <string.h>

static u8 *adf;
static u32 adf_size;
static int track;                         /* the cylinder the head is on */
static int side, motor;
static int disk_trace;                    /* RECOMP_DISK_TRACE: say what the drive does */
static u8 prb_last = 0xFF;

/* a disk change: the new image, and /CHNG is low until the game steps the head */
static int changed;
void disk_set_adf(const char *path);
void disk_change(const char *path) {
	free(adf); adf = NULL; adf_size = 0;
	disk_set_adf(path);
	changed = 1;
}

void disk_set_adf(const char *path) {
	disk_trace = getenv("RECOMP_DISK_TRACE") != NULL;
	FILE *f = fopen(path, "rb");
	if (!f) { fprintf(stderr, "disk: can't open %s\n", path); exit(1); }
	fseek(f, 0, SEEK_END); adf_size = (u32)ftell(f); rewind(f);
	adf = malloc(adf_size);
	if (fread(adf, 1, adf_size, f) != adf_size) { fprintf(stderr, "disk: short read\n"); exit(1); }
	fclose(f);
}

/* CIA-B PRB: bit 0 /STEP, 1 /DIR (0 = inwards), 2 /SIDE (0 = upper = side 1), 3 /SEL0, 7 /MTR */
void disk_cia_write(u8 prb) {
	int sel = !(prb & 0x08);
	if (sel && (prb_last & 0x08)) {                          /* the drive latches /MTR as /SEL goes low */
		motor = !(prb & 0x80);
		if (disk_trace) fprintf(stderr, "disk: motor %s\n", motor ? "on" : "off");
	}
	if (sel) {
		if (!(prb & 1) && (prb_last & 1)) {                   /* STEP: a falling edge */
			changed = 0;
			if (prb & 2) { if (track > 0) track--; } else { if (track < 79) track++; }
			if (disk_trace) fprintf(stderr, "disk: step to cylinder %d\n", track);
		}
		side = (prb & 4) ? 0 : 1;
	}
	prb_last = prb;
}

int disk_motor_on(void) { return motor; }

/* where the drive was when the start state was taken (start.h) */
void disk_set_position(int cyl, int sd, int on) { track = cyl; side = sd; motor = on; }

/* the drive's status lines, read through CIA-A PRA: bit 2 /CHNG, 3 /WPRO, 4 /TRK0, 5 /RDY (all active low) */
u8 disk_cia_status(void) {
	u8 v = 0xFF;
	if (!adf || changed) v &= ~0x04;      /* /CHNG low: no disk, or one just inserted (until a step) */
	v &= ~0x08;                           /* write-protected: low */
	if (track == 0) v &= ~0x10;
	if (motor) v &= ~0x20;
	return v;
}

/* MFM-encode `n` bytes of data: the odd bits then the even bits, with clock bits */
static void mfm_put(u16 *out, u32 *pos, const u8 *data, int n) {
	u32 start = *pos;
	for (int half = 0; half < 2; half++) {
		for (int i = 0; i < n; i += 2) {
			u16 w = (u16)(data[i] << 8 | (i + 1 < n ? data[i + 1] : 0));
			u16 bits = half == 0 ? (w >> 1) & 0x5555 : w & 0x5555;
			out[(*pos)++] = bits;
		}
	}
	/* clock bits: a 1 between two 0 data bits */
	u16 prev = start ? out[start - 1] : 0;
	for (u32 i = start; i < *pos; i++) {
		u16 w = out[i], clk = 0;
		for (int b = 15; b >= 0; b -= 2) {
			int left = (b == 15) ? (prev & 1) : (w >> (b + 1) & 1);
			int right = w >> (b - 1) & 1;
			if (!left && !right) clk |= (u16)(1 << b);
		}
		out[i] = w | clk;
		prev = out[i];
	}
}

static u32 checksum(const u16 *w, int n) {
	u32 s = 0;
	for (int i = 0; i < n; i += 2) s ^= (u32)w[i] << 16 | w[i + 1];
	return s & 0x55555555;
}

/* the whole track as MFM words (11 sectors, then the gap) */
static u32 build_track(u16 *out, int cyl, int sd) {
	u32 pos = 0;
	int t = cyl * 2 + sd;
	for (int s = 0; s < 11; s++) {
		out[pos++] = 0xAAAA; out[pos++] = 0xAAAA;
		out[pos++] = 0x4489; out[pos++] = 0x4489;
		u8 info[4] = { 0xFF, (u8)t, (u8)s, (u8)(11 - s) };
		u32 p0 = pos;
		mfm_put(out, &pos, info, 4);
		u8 label[16] = { 0 };
		mfm_put(out, &pos, label, 16);
		u32 hsum = checksum(out + p0, (int)(pos - p0));
		u8 hs[4] = { (u8)(hsum >> 24), (u8)(hsum >> 16), (u8)(hsum >> 8), (u8)hsum };
		mfm_put(out, &pos, hs, 4);
		u32 offset = ((u32)t * 11 + (u32)s) * 512;
		u8 data[512];
		if (offset + 512 <= adf_size) memcpy(data, adf + offset, 512); else memset(data, 0, 512);
		u32 pd = pos + 4;                 /* the data checksum comes before the data: compute it first */
		u16 tmp[512];
		u32 tp = 0;
		mfm_put(tmp, &tp, data, 512);
		u32 dsum = checksum(tmp, 512);
		u8 ds[4] = { (u8)(dsum >> 24), (u8)(dsum >> 16), (u8)(dsum >> 8), (u8)dsum };
		mfm_put(out, &pos, ds, 4);
		(void)pd;
		mfm_put(out, &pos, data, 512);
	}
	for (int i = 0; i < 400; i++) out[pos++] = 0xAAAA;   /* the gap */
	return pos;
}

extern void intreq_set_disk_after(u32 words);

void disk_dma(u32 pt, u16 len) {
	u32 words = len & 0x3FFF;
	if (!adf) { trace_die("disk read with no ADF (give one with --adf)", pt); return; }
	static u16 trk[8192];
	u32 n = build_track(trk, track, side);
	/* the disk turns: the read starts where the head is now (300 rpm, a revolution
	 * every 200 ms), at the next sync word (loaders rely on WORDSYNC in ADKCON) */
	const u64 rev = 1418758;                     /* cycles per revolution at 7.09 MHz */
	static u64 last_cycles;
	static u32 head;                             /* the word under the head when the last read ended */
	u32 start = (u32)((head + (cpu.cycles - last_cycles) % rev * n / rev) % n);
	for (u32 i = 0; i < n; i++) if (trk[(start + i) % n] == 0x4489) { start = (start + i) % n; break; }
	for (u32 i = 0; i < words; i++) wr16(pt + 2 * i, trk[(start + i) % n]);
	head = (start + words) % n;
	last_cycles = cpu.cycles;
	if (disk_trace) fprintf(stderr, "disk: read track %d side %d, %u words from word %u of %u -> $%06X\n", track, side, words, start, n, pt);
	intreq_set_disk_after(words);
}
