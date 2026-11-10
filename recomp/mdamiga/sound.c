/* The sound of a ported Mega Drive game: a twelve-voice mixer on Paula,
 * played from the stream agk port made (harness/agk/mdsound.py has the
 * format and how it is made: the game's own driver run on the host, its
 * notes as "start sample N, this period, this volume", its FM patches
 * rendered to samples). Nothing musical is worked out here.
 *
 * Paula plays two buffers a side, over and over, 232 samples at period 255:
 * a sixtieth of a second. Channels 0 and 3 are the left (0 at volume 64 has
 * the high byte, 3 at volume 1 the next six bits: fourteen bits between
 * them), 1 and 2 the right. When channel 0 comes to the end of a buffer its
 * interrupt (level 4) is the tick: the stream's next bytes are read, and the
 * voices are mixed into the buffer Paula will take after the one it has just
 * started. So the music keeps its beat whatever the game is doing, and the
 * samples never leave fast RAM. Voices 0..7 are the music's (or a jingle's,
 * while the music waits), 8..11 two effects'.
 *
 * The game talks to "its Z80": a byte written to one of the driver's request
 * addresses is looked up (music, an effect, a jingle, stop, fade), and what
 * it reads back is the idle driver's RAM, where the driver is always ready. */
typedef unsigned char u8;
typedef signed char s8;
typedef unsigned short u16;
typedef short s16;
typedef unsigned long u32;
typedef long s32;
#define CUSTOM(o) (*(volatile u16 *)(0xDFF000 + (o)))
#define CUSTOML(o) (*(volatile u32 *)(0xDFF000 + (o)))

const u8 *port_sound;                     /* main.c: the data, or 0 */
int port_hz = 50;

struct smp { u32 data; u16 len, loop, looplen, flags; };
struct strm { u32 data, loop; u8 nch, kind; u16 nlocal; u32 local; };
enum { K_MUSIC = 1, K_OVER, K_STOP, K_FADE, K_JINGLE };
#define N 232                             /* samples a tick */
#define PERIOD 255                        /* 3546895 / 255 = 13909 Hz; 232 of them: 59.95 ticks a second */
#define NV 12

static const struct smp *smps;
static const struct strm *strms;
static const u8 *reqs, *idle;
static u16 n_addrs;
static u8 *chipbuf;                       /* 8 buffers of N: [which of the two][channel] */
static u8 fill;                           /* the pair to mix into next */

struct voice { const s8 *data; u32 pos, step, end, llen; u8 vol, pan, on; };
static struct voice voice[NV];
struct player {
	const u8 *p, *loop;                   /* p == 0: not playing */
	u16 wait, id;
	u8 nch, base;                         /* its voices: base.. */
	u16 per[8];
	const u16 *local;
};
static struct player music, jingle, sfx[2];
static u8 mvol = 64, fading, fade_n;
static volatile u8 rq[16], rq_w, rq_r;    /* requests waiting for the tick: address index, value */
static s16 voltab[65][256];               /* a sample byte at a volume */
static s32 acc[2][N];

static void hush(int a, int b) { for (int i = a; i < b; i++) voice[i].on = 0; }

static void step(struct player *pl) {
	if (!pl->p) return;
	if (pl->wait) { pl->wait--; return; }
	const u8 *p = pl->p;
	u8 b = *p++;
	if (b == 0xFF) {
		if (!pl->loop) { pl->p = 0; hush(pl->base, pl->base + pl->nch); return; }
		p = pl->loop;
		b = *p++;
	}
	if (b < 0x80) { pl->wait = b; pl->p = p; return; }
	int mask = b & 15;
	if (b & 0x40) mask |= *p++ << 4;
	for (int c = 0; c < pl->nch; c++) {
		if (!(mask & (1 << c))) continue;
		u8 fl = *p++;
		struct voice *v = &voice[pl->base + c];
		if (fl & 1) {
			const struct smp *s = &smps[pl->local[*p++] & 0x7FFF];
			v->data = (const s8 *)(port_sound + s->data);
			v->pos = 0;
			v->end = (u32)s->len << 17;
			v->llen = s->loop != 0xFFFF ? (u32)s->looplen << 17 : 0;
			v->on = 1;
		}
		if (fl & 16) v->pan = *p++;
		if (fl & 2) { pl->per[c] = (u16)(p[0] << 8 | p[1]); p += 2; }
		if (fl & 8) pl->per[c] = (u16)(pl->per[c] + (s8)*p++);
		if (fl & 10) v->step = ((u32)PERIOD << 16) / (pl->per[c] ? pl->per[c] : 1);
		if (fl & 4) v->vol = *p++;
	}
	pl->p = p;
}

static void begin(struct player *pl, int id, int base) {
	const struct strm *st = &strms[id];
	pl->local = (const u16 *)(port_sound + st->local);
	pl->loop = st->loop ? port_sound + st->loop : 0;
	pl->wait = 0;
	pl->id = (u16)id;
	pl->nch = st->nch;
	pl->base = (u8)base;
	hush(base, base + pl->nch);
	pl->p = port_sound + st->data;
}

static void request(int ai, int v) {
	const u8 *e = reqs + ai * 514 + 2 + v * 2;
	int kind = e[0] >> 4, id = (e[0] & 15) << 8 | e[1];
	switch (kind) {
	case K_MUSIC:
		jingle.p = 0;
		mvol = 64; fading = 0;
		hush(0, 8);
		begin(&music, id, 0);
		break;
	case K_JINGLE:
		hush(0, 8);
		begin(&jingle, id, 0);
		break;
	case K_OVER: {                        /* the same effect again takes its own place; else the free one, else the first */
		int k = sfx[0].p && sfx[0].id == id ? 0 : sfx[1].p && sfx[1].id == id ? 1 : !sfx[0].p ? 0 : !sfx[1].p ? 1 : 0;
		begin(&sfx[k], id, 8 + k * 2);
		break;
	}
	case K_STOP:
		music.p = jingle.p = 0;
		hush(0, 8);
		break;
	case K_FADE:
		if (music.p) { fading = 1; fade_n = 0; }
		break;
	}
}

static void mix(void) {
	for (int i = 0; i < N; i++) acc[0][i] = acc[1][i] = 0;
	for (int k = 0; k < NV; k++) {
		struct voice *v = &voice[k];
		if (!v->on || !v->step) continue;
		int vol = k < 8 ? v->vol * mvol >> 6 : v->vol;
		if (v->pan == 3) vol = vol * 3 >> 2;                  /* in the middle: on both sides, a little lower */
		if (vol > 64) vol = 64;
		const s16 *vt = voltab[vol];
		const s8 *d = v->data;
		u32 pos = v->pos, st = v->step;
		s32 *l = acc[0], *r = acc[1];
		int n = N;
		while (n) {
			if (pos >= v->end) {
				if (!v->llen) { v->on = 0; break; }
				do pos -= v->llen; while (pos >= v->end);
			}
			u32 room = (v->end - pos + st - 1) / st;
			int m = room < (u32)n ? (int)room : n;
			n -= m;
			if (!vol) { pos += st * (u32)m; l += m; r += m; }
			else if (v->pan == 3) { while (m--) { s32 s = vt[(u8)d[pos >> 16]]; *l++ += s; *r++ += s; pos += st; } }
			else if (v->pan == 2) { while (m--) { *l++ += vt[(u8)d[pos >> 16]]; pos += st; } r += 0; }
			else { while (m--) { *r++ += vt[(u8)d[pos >> 16]]; pos += st; } }
		}
		v->pos = pos;
	}
	u8 *b = chipbuf + fill * 4 * N;
	static const u8 hi[2] = { 0, 1 }, lo[2] = { 3, 2 };
	for (int side = 0; side < 2; side++) {
		u8 *h = b + hi[side] * N, *w = b + lo[side] * N;
		const s32 *a = acc[side];
		for (int i = 0; i < N; i++) {
			s32 s = a[i] * 2;
			if (s > 32767) s = 32767; else if (s < -32768) s = -32768;
			h[i] = (u8)(s >> 8);
			w[i] = (u8)((s & 0xFF) >> 2);
		}
	}
}

/* channel 0 has just taken the buffers it was pointed at: point all four at the other pair, and fill that */
void sound_tick(void) {
	u8 *b = chipbuf + fill * 4 * N;
	for (int ch = 0; ch < 4; ch++) CUSTOML(0xA0 + ch * 16) = (u32)(b + ch * N);
	while (rq_r != rq_w) { int a = rq[rq_r], v = rq[rq_r + 1]; rq_r = (u8)((rq_r + 2) & 15); request(a, v); }
	if (fading && ++fade_n >= 3) {
		fade_n = 0;
		if (mvol) mvol--;
		if (!mvol) { fading = 0; music.p = 0; hush(0, 8); mvol = 64; }
	}
	if (jingle.p) step(&jingle); else step(&music);
	step(&sfx[0]);
	step(&sfx[1]);
	mix();
	fill ^= 1;
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
	n_addrs = w[4];
	smps = (const struct smp *)(b + l[3]);
	strms = (const struct strm *)(b + l[4]);
	reqs = b + l[5];
	chipbuf = (u8 *)l[7];
	if (!chipbuf) return;
	for (int i = 0; i < 8 * N; i++) chipbuf[i] = 0;
	for (int v = 0; v <= 64; v++) for (int s = 0; s < 256; s++) voltab[v][s] = (s16)((s8)s * v);
	CUSTOM(0x096) = 0x000F;
	CUSTOM(0x09E) = 0x00FF;               /* ADKCON: no modulation between channels */
	for (int ch = 0; ch < 4; ch++) {
		CUSTOML(0xA0 + ch * 16) = (u32)(chipbuf + ch * N);
		CUSTOM(0xA4 + ch * 16) = N / 2;
		CUSTOM(0xA6 + ch * 16) = PERIOD;
		CUSTOM(0xA8 + ch * 16) = ch == 0 || ch == 1 ? 64 : 1;
	}
	fill = 1;
	idle = b + l[6];
	CUSTOM(0x09C) = 0x0780;
	CUSTOM(0x09A) = 0x8080;               /* channel 0's interrupt (level 4) */
	CUSTOM(0x096) = 0x820F;
}
