/* A small inflate (RFC 1951, raw deflate: no zlib header), for the loader:
 * the port's image is stored deflated on the disk. */
typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned long u32;

static const u8 *in;
static u32 bitbuf;
static int bitcnt;

static u32 bits(int n) {
	while (bitcnt < n) { bitbuf |= (u32)*in++ << bitcnt; bitcnt += 8; }
	u32 v = bitbuf & ((1UL << n) - 1);
	bitbuf >>= n; bitcnt -= n;
	return v;
}

typedef struct { u16 count[16], symbol[288]; } huff;

static void build(huff *h, const u8 *len, int n) {
	u16 offs[16];
	int i;
	for (i = 0; i < 16; i++) h->count[i] = 0;
	for (i = 0; i < n; i++) h->count[len[i]]++;
	h->count[0] = 0;
	offs[1] = 0;
	for (i = 1; i < 15; i++) offs[i + 1] = offs[i] + h->count[i];
	for (i = 0; i < n; i++) if (len[i]) h->symbol[offs[len[i]]++] = (u16)i;
}

static int decode(const huff *h) {
	int code = 0, first = 0, index = 0, len;
	for (len = 1; len <= 15; len++) {
		code |= (int)bits(1);
		int count = h->count[len];
		if (code - count < first) return h->symbol[index + (code - first)];
		index += count; first += count;
		first <<= 1; code <<= 1;
	}
	return -1;
}

static const u16 lbase[29] = { 3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258 };
static const u8 lext[29] = { 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0 };
static const u16 dbase[30] = { 1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577 };
static const u8 dext[30] = { 0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13 };

/* -> the number of bytes written, or 0 on bad data */
u32 inflate_raw(const u8 *src, u8 *dst) {
	static huff ll, dd;
	static u8 lens[320];
	u8 *out = dst;
	int last, i;
	in = src; bitbuf = 0; bitcnt = 0;
	do {
		last = (int)bits(1);
		int type = (int)bits(2);
		if (type == 0) {
			bitbuf = 0; bitcnt = 0;
			u32 n = in[0] | (u32)in[1] << 8;
			in += 4;
			while (n--) *out++ = *in++;
			continue;
		}
		if (type == 1) {
			for (i = 0; i < 144; i++) lens[i] = 8;
			for (; i < 256; i++) lens[i] = 9;
			for (; i < 280; i++) lens[i] = 7;
			for (; i < 288; i++) lens[i] = 8;
			build(&ll, lens, 288);
			for (i = 0; i < 30; i++) lens[i] = 5;
			build(&dd, lens, 30);
		} else if (type == 2) {
			static const u8 order[19] = { 16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15 };
			int nl = (int)bits(5) + 257, nd = (int)bits(5) + 1, nc = (int)bits(4) + 4;
			for (i = 0; i < 19; i++) lens[i] = 0;
			for (i = 0; i < nc; i++) lens[order[i]] = (u8)bits(3);
			build(&ll, lens, 19);
			i = 0;
			while (i < nl + nd) {
				int sym = decode(&ll), rep = 0, val = 0;
				if (sym < 0) return 0;
				if (sym < 16) { lens[i++] = (u8)sym; continue; }
				if (sym == 16) { if (!i) return 0; val = lens[i - 1]; rep = 3 + (int)bits(2); }
				else if (sym == 17) rep = 3 + (int)bits(3);
				else rep = 11 + (int)bits(7);
				while (rep--) lens[i++] = (u8)val;
			}
			{
				static u8 l2[320];
				for (i = 0; i < nl + nd; i++) l2[i] = lens[i];
				build(&ll, l2, nl);
				build(&dd, l2 + nl, nd);
			}
		} else return 0;
		for (;;) {
			int sym = decode(&ll);
			if (sym < 0) return 0;
			if (sym < 256) { *out++ = (u8)sym; continue; }
			if (sym == 256) break;
			sym -= 257;
			if (sym >= 29) return 0;
			u32 len = lbase[sym] + bits(lext[sym]);
			int ds = decode(&dd);
			if (ds < 0 || ds >= 30) return 0;
			u32 dist = dbase[ds] + bits(dext[ds]);
			const u8 *from = out - dist;
			while (len--) *out++ = *from++;
		}
	} while (!last);
	return (u32)(out - dst);
}
