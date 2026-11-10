/* The sound of a ported Mega Drive game: Paula played from the stream agk
 * port made (harness/agk/mdsound.py has the format and how it is made: the
 * game's own driver run on the host, its notes given to four channels, its
 * FM patches rendered to samples). Nothing is worked out here. Sixty times a
 * second, on CIA-B's timer (level 6: music keeps its beat when the game
 * doesn't), each player reads its next bytes: start sample N on channel C,
 * this period, this volume.
 *
 * The game talks to "its Z80": a byte written to one of the driver's request
 * addresses is looked up (music: replace; effect: over the music on channel 3,
 * a second one on 2; jingle: the music waits; stop; fade), and what it reads
 * back is the idle driver's RAM, where the driver always says it is ready.
 *
 * The samples stay in fast RAM. The effects' are copied to chip RAM once, a
 * piece of music's when it starts (into the one buffer music has). */
typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned long u32;
typedef signed char s8;
#define CUSTOM(o) (*(volatile u16 *)(0xDFF000 + (o)))
#define CUSTOML(o) (*(volatile u32 *)(0xDFF000 + (o)))
#define CIAB(o) (*(volatile u8 *)(0xBFD000 + (o)))

const u8 *port_sound;                     /* main.c: the data, or 0 */
int port_hz = 50;                         /* the machine's (for CIA-B's clock) */

struct smp { u32 data; u16 len, loop, looplen, flags; };
struct strm { u32 data, loop; u8 nch, kind; u16 nlocal; u32 local; };
enum { K_MUSIC = 1, K_OVER, K_STOP, K_FADE, K_JINGLE };

static const struct smp *smps;
static const struct strm *strms;
static const u8 *reqs, *idle;
static u16 n_smps, n_strms, n_addrs;
static u32 chip_silence, chip_music, chip_music_size, chip_jingle, chip_jingle_size;
#define MAX_SMPS 1024
static u32 kept[MAX_SMPS];                /* an effect's sample in chip RAM (0: not there) */

struct player {
	const u8 *p, *loop;                   /* p == 0: not playing */
	u16 wait, id;
	u8 nch, map[4];
	u16 per[4];
	const u16 *local;
	u32 addr[256];                        /* its samples in chip RAM */
};
static struct player music, jingle, sfx[2];
static struct { u32 addr; const struct smp *s; u16 per; u8 vol; } mus[4];   /* what the music has on each channel */
static u8 owner[4];                       /* 0: the music's; 1, 2: that effect's */
static u8 trig;                           /* channels starting a sample this tick */
static struct { u32 lc, llc; u16 len, llen; } go[4];
static u8 mvol = 64, fading, fade_n;
static volatile u8 rq[16], rq_w, rq_r;    /* requests waiting for the tick: address index, value */

static void pa_start(int ch, u32 addr, const struct smp *s, int from_loop) {
	if (!addr) { CUSTOM(0xA8 + ch * 16) = 0; return; }
	trig |= (u8)(1 << ch);
	if (s->loop != 0xFFFF) { go[ch].llc = addr + s->loop * 2; go[ch].llen = s->looplen; }
	else { go[ch].llc = chip_silence; go[ch].llen = 2; }
	if (from_loop) { go[ch].lc = go[ch].llc; go[ch].len = go[ch].llen; }
	else { go[ch].lc = addr; go[ch].len = s->len; }
}
static void lines(int n) {
	while (n--) { u8 v = *(volatile u8 *)0xDFF006; while (*(volatile u8 *)0xDFF006 == v) {} }
}
static void quiet_music(void) {
	for (int ch = 0; ch < 4; ch++) { mus[ch].vol = 0; mus[ch].s = 0; if (!owner[ch]) CUSTOM(0xA8 + ch * 16) = 0; }
}
/* a channel an effect had goes back to the music: a note that is still held picks up at its loop */
static void give_back(int ch) {
	owner[ch] = 0;
	if (mus[ch].s && mus[ch].vol && mus[ch].addr && mus[ch].s->loop != 0xFFFF && (music.p || jingle.p)) {
		pa_start(ch, mus[ch].addr, mus[ch].s, 1);
		CUSTOM(0xA6 + ch * 16) = mus[ch].per;
		CUSTOM(0xA8 + ch * 16) = (u16)(mus[ch].vol * mvol >> 6);
	} else CUSTOM(0xA8 + ch * 16) = 0;
}
static void finish(struct player *pl, int who) {
	pl->p = 0;
	if (who) { for (int c = 0; c < pl->nch; c++) if (owner[pl->map[c]] == who) give_back(pl->map[c]); }
	else quiet_music();
}

static void step(struct player *pl, int who) {
	if (!pl->p) return;
	if (pl->wait) { pl->wait--; return; }
	const u8 *p = pl->p;
	u8 b = *p++;
	if (b == 0xFF) {
		if (!pl->loop) { finish(pl, who); return; }
		p = pl->loop;
		b = *p++;
	}
	if (b < 0x80) { pl->wait = b; pl->p = p; return; }
	for (int c = 0; c < pl->nch; c++) {
		if (!(b & (1 << c))) continue;
		u8 fl = *p++;
		int ch = pl->map[c], mine = owner[ch] == who;
		if (fl & 1) {
			u8 i = *p++;
			const struct smp *s = &smps[pl->local[i] & 0x7FFF];
			if (!who) { mus[ch].s = s; mus[ch].addr = pl->addr[i]; }
			if (mine) pa_start(ch, pl->addr[i], s, 0);
		}
		if (fl & 2) { pl->per[c] = (u16)(p[0] << 8 | p[1]); p += 2; }
		if (fl & 8) pl->per[c] = (u16)(pl->per[c] + (s8)*p++);
		if (fl & 10) { if (!who) mus[ch].per = pl->per[c]; if (mine) CUSTOM(0xA6 + ch * 16) = pl->per[c]; }
		if (fl & 4) {
			u8 v = *p++;
			if (!who) { mus[ch].vol = v; v = (u8)(v * mvol >> 6); }
			if (mine) CUSTOM(0xA8 + ch * 16) = v;
		}
	}
	pl->p = p;
}

static void copy(const u8 *s, u8 *d, u32 n) { while (n--) *d++ = *s++; }

static void begin(struct player *pl, int id, u32 base, u32 size) {
	const struct strm *st = &strms[id];
	u32 at = base;
	pl->local = (const u16 *)(port_sound + st->local);
	for (int i = 0; i < st->nlocal && i < 256; i++) {
		u16 w = pl->local[i];
		const struct smp *s = &smps[w & 0x7FFF];
		u32 n = (u32)s->len * 2;
		if ((w & 0x7FFF) < MAX_SMPS && kept[w & 0x7FFF]) pl->addr[i] = kept[w & 0x7FFF];
		else if ((w & 0x8000) || !size || at + n > base + size) pl->addr[i] = 0;
		else { copy(port_sound + s->data, (u8 *)at, n); pl->addr[i] = at; at += n; }
	}
	pl->p = port_sound + st->data;
	pl->loop = st->loop ? port_sound + st->loop : 0;
	pl->wait = 0;
	pl->id = (u16)id;
	pl->nch = st->nch;
}

static void request(int ai, int v) {
	const u8 *e = reqs + ai * 514 + 2 + v * 2;
	int kind = e[0] >> 4, id = (e[0] & 15) << 8 | e[1];
	switch (kind) {
	case K_MUSIC:
		jingle.p = 0;
		quiet_music();
		mvol = 64; fading = 0;
		for (int c = 0; c < 4; c++) music.map[c] = (u8)c;
		begin(&music, id, chip_music, chip_music_size);
		break;
	case K_JINGLE:
		quiet_music();
		for (int c = 0; c < 4; c++) jingle.map[c] = (u8)c;
		begin(&jingle, id, chip_jingle, chip_jingle_size);
		break;
	case K_OVER: {
		int n = strms[id].nch, k;
		if (n >= 2) {                     /* both channels: whatever effects were playing stop */
			sfx[1].p = 0;
			k = 0;
			owner[3] = owner[2] = 1;
			sfx[0].map[0] = 3; sfx[0].map[1] = 2;
		} else {
			if (sfx[0].p && sfx[0].id == id && sfx[0].nch == 1) k = 0;
			else if (sfx[1].p && sfx[1].id == id) k = 1;
			else if (!sfx[0].p) k = 0;
			else if (!sfx[1].p && sfx[0].nch == 1) k = 1;
			else k = 0;
			if (k == 0 && sfx[0].p && sfx[0].nch == 2) { if (owner[2] == 1) give_back(2); }
			sfx[k].map[0] = (u8)(k ? 2 : 3);
			owner[k ? 2 : 3] = (u8)(k + 1);
		}
		begin(&sfx[k], id, 0, 0);
		break;
	}
	case K_STOP:
		music.p = jingle.p = 0;
		quiet_music();
		break;
	case K_FADE:
		if (music.p) { fading = 1; fade_n = 0; }
		break;
	}
}

void sound_tick(void) {
	while (rq_r != rq_w) { int a = rq[rq_r], v = rq[rq_r + 1]; rq_r = (u8)((rq_r + 2) & 15); request(a, v); }
	if (fading && ++fade_n >= 3) {
		fade_n = 0;
		if (mvol) mvol--;
		if (!mvol) { fading = 0; music.p = 0; quiet_music(); mvol = 64; }
		else for (int ch = 0; ch < 4; ch++) if (!owner[ch]) CUSTOM(0xA8 + ch * 16) = (u16)(mus[ch].vol * mvol >> 6);
	}
	if (jingle.p) step(&jingle, 0); else step(&music, 0);
	step(&sfx[0], 1);
	step(&sfx[1], 2);
	if (trig) {                           /* the samples that start now: off, pointed, on; then their loops */
		u8 t = trig;
		trig = 0;
		CUSTOM(0x096) = t;
		for (int ch = 0; ch < 4; ch++) if (t & (1 << ch)) { CUSTOML(0xA0 + ch * 16) = go[ch].lc; CUSTOM(0xA4 + ch * 16) = go[ch].len; }
		lines(5);
		CUSTOM(0x096) = (u16)(0x8200 | t);
		lines(3);
		for (int ch = 0; ch < 4; ch++) if (t & (1 << ch)) { CUSTOML(0xA0 + ch * 16) = go[ch].llc; CUSTOM(0xA4 + ch * 16) = go[ch].llen; }
	}
}

/* the game's side: its Z80's RAM */
u32 sound_z80_read(u32 a) { return idle ? idle[a & 0x1FFF] : 0; }
void sound_z80_write(u32 a, u32 v) {
	if (!idle || !(v & 0xFF)) return;
	for (int i = 0; i < n_addrs; i++) {
		const u8 *e = reqs + i * 514;
		if ((u32)(e[0] << 8 | e[1]) != (a & 0x1FFF)) continue;
		if (((rq_w + 2) & 15) != rq_r) { rq[rq_w] = (u8)i; rq[rq_w + 1] = (u8)v; rq_w = (u8)((rq_w + 2) & 15); }
		return;
	}
}

void sound_init(void) {
	const u8 *b = port_sound;
	if (!b || b[0] != 'M' || b[1] != 'D' || b[2] != 'S' || b[3] != 'N') return;
	const u16 *w = (const u16 *)b;
	const u32 *l = (const u32 *)b;
	n_smps = w[2]; n_strms = w[3]; n_addrs = w[4];
	smps = (const struct smp *)(b + l[3]);
	strms = (const struct strm *)(b + l[4]);
	reqs = b + l[5];
	chip_silence = l[7];
	u32 at = l[8];
	chip_music = l[9]; chip_music_size = l[10]; chip_jingle = l[11]; chip_jingle_size = l[12];
	*(u32 *)chip_silence = 0;
	for (int i = 0; i < n_smps && i < MAX_SMPS; i++) {
		if (!(smps[i].flags & 1) || !at) continue;
		u32 n = (u32)smps[i].len * 2;
		copy(b + smps[i].data, (u8 *)at, n);
		kept[i] = at;
		at += n;
	}
	CUSTOM(0x096) = 0x000F;
	for (int ch = 0; ch < 4; ch++) CUSTOM(0xA8 + ch * 16) = 0;
	CUSTOM(0x09E) = 0x00FF;               /* ADKCON: no modulation between channels */
	idle = b + l[6];
	u32 count = (port_hz == 60 ? 715909 : 709379) / 60;
	CIAB(0xD00) = 0x7F;                   /* its interrupts: timer A only */
	CIAB(0xE00) = 0;
	CIAB(0x400) = (u8)count; CIAB(0x500) = (u8)(count >> 8);
	(void)CIAB(0xD00);
	CIAB(0xD00) = 0x81;
	CIAB(0xE00) = 0x11;                   /* load and run, over and over */
	CUSTOM(0x09C) = 0x2000;
	CUSTOM(0x09A) = 0xA000;               /* level 6 on */
}
