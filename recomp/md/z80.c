/* The Z80, for the Mega Drive's sound side: the documented instruction set,
 * the index registers' halves, interrupt mode 1, T-states per instruction
 * (the sound drivers time their sample playback by counting them). The
 * machine supplies z80_rd/z80_wr; there are no I/O ports on a Mega Drive. */
#include "z80.h"

enum { FC = 1, FN = 2, FP = 4, FX = 8, FH = 16, FY = 32, FZ = 64, FS = 128 };
static u8 szp[256];
static int tables;
static z80_t *z;
static int idx;                           /* 0 HL, 1 IX, 2 IY: the prefix in force */
static int cyc;

#undef A
#define A z->a
#define F z->f
#define BC ((u16)(z->b << 8 | z->c))
#define DE ((u16)(z->d << 8 | z->e))
#define HL ((u16)(z->h << 8 | z->l))
static void set_bc(u16 v) { z->b = (u8)(v >> 8); z->c = (u8)v; }
static void set_de(u16 v) { z->d = (u8)(v >> 8); z->e = (u8)v; }
static void set_hl(u16 v) { z->h = (u8)(v >> 8); z->l = (u8)v; }

static u8 fetch(void) { return z80_rd(z->pc++); }
static u16 fetch16(void) { u16 lo = fetch(); return (u16)(lo | fetch() << 8); }
static u16 zrd16(u16 a) { return (u16)(z80_rd(a) | z80_rd((u16)(a + 1)) << 8); }
static void zwr16(u16 a, u16 v) { z80_wr(a, (u8)v); z80_wr((u16)(a + 1), (u8)(v >> 8)); }
static void push(u16 v) { z->sp -= 2; zwr16(z->sp, v); }
static u16 pop(void) { u16 v = zrd16(z->sp); z->sp += 2; return v; }

static u16 hlx(void) { return idx == 1 ? z->ix : idx == 2 ? z->iy : HL; }
static void set_hlx(u16 v) { if (idx == 1) z->ix = v; else if (idx == 2) z->iy = v; else set_hl(v); }
/* the address of the (HL) operand: (IX+d) under a prefix */
static u16 maddr(void) {
	if (!idx) return HL;
	cyc += 8;
	return (u16)(hlx() + (s8)fetch());
}
/* register r (B C D E H L - A); H and L are the index register's halves under a prefix
 * unless the instruction also has the memory operand (plain = 1) */
static u8 getr(int r, int plain) {
	switch (r) {
	case 0: return z->b; case 1: return z->c; case 2: return z->d; case 3: return z->e;
	case 4: return idx && !plain ? (u8)(hlx() >> 8) : z->h;
	case 5: return idx && !plain ? (u8)hlx() : z->l;
	default: return A;
	}
}
static void setr(int r, u8 v, int plain) {
	switch (r) {
	case 0: z->b = v; break; case 1: z->c = v; break; case 2: z->d = v; break; case 3: z->e = v; break;
	case 4: if (idx && !plain) set_hlx((u16)((hlx() & 0xFF) | v << 8)); else z->h = v; break;
	case 5: if (idx && !plain) set_hlx((u16)((hlx() & 0xFF00) | v)); else z->l = v; break;
	default: A = v;
	}
}
static u16 getrp(int p) { return p == 0 ? BC : p == 1 ? DE : p == 2 ? hlx() : z->sp; }
static void setrp(int p, u16 v) { if (p == 0) set_bc(v); else if (p == 1) set_de(v); else if (p == 2) set_hlx(v); else z->sp = v; }

static int cond(int c) {
	switch (c) {
	case 0: return !(F & FZ); case 1: return F & FZ; case 2: return !(F & FC); case 3: return F & FC;
	case 4: return !(F & FP); case 5: return F & FP; case 6: return !(F & FS); default: return F & FS;
	}
}

static void add8(u8 b, int c) {
	int r = A + b + c;
	F = (u8)((szp[r & 0xFF] & ~FP) | ((A ^ b ^ r) & FH) | ((~(A ^ b) & (A ^ r) & 0x80) ? FP : 0) | (r >> 8 & 1));
	A = (u8)r;
}
static u8 sub8(u8 b, int c) {
	int r = A - b - c;
	F = (u8)((szp[r & 0xFF] & ~FP) | FN | ((A ^ b ^ r) & FH) | (((A ^ b) & (A ^ r) & 0x80) ? FP : 0) | (r >> 8 & 1));
	return (u8)r;
}
static void alu(int op, u8 b) {
	switch (op) {
	case 0: add8(b, 0); break;
	case 1: add8(b, F & FC); break;
	case 2: A = sub8(b, 0); break;
	case 3: A = sub8(b, F & FC); break;
	case 4: A &= b; F = szp[A] | FH; break;
	case 5: A ^= b; F = szp[A]; break;
	case 6: A |= b; F = szp[A]; break;
	default: sub8(b, 0); break;
	}
}
static u8 inc8(u8 v) { v++; F = (u8)((F & FC) | (szp[v] & ~FP) | (v == 0x80 ? FP : 0) | ((v & 15) == 0 ? FH : 0)); return v; }
static u8 dec8(u8 v) { v--; F = (u8)((F & FC) | FN | (szp[v] & ~FP) | (v == 0x7F ? FP : 0) | ((v & 15) == 15 ? FH : 0)); return v; }
static u8 rot(int op, u8 v) {
	int c = F & FC, co;
	switch (op) {
	case 0: co = v >> 7; v = (u8)(v << 1 | co); break;                 /* RLC */
	case 1: co = v & 1; v = (u8)(v >> 1 | co << 7); break;             /* RRC */
	case 2: co = v >> 7; v = (u8)(v << 1 | c); break;                  /* RL */
	case 3: co = v & 1; v = (u8)(v >> 1 | c << 7); break;              /* RR */
	case 4: co = v >> 7; v = (u8)(v << 1); break;                      /* SLA */
	case 5: co = v & 1; v = (u8)((v & 0x80) | v >> 1); break;          /* SRA */
	case 6: co = v >> 7; v = (u8)(v << 1 | 1); break;                  /* SLL */
	default: co = v & 1; v >>= 1; break;                               /* SRL */
	}
	F = (u8)(szp[v] | co);
	return v;
}

static void op_cb(void) {
	u16 a = 0;
	int op, r;
	if (idx) { a = (u16)(hlx() + (s8)fetch()); op = fetch(); r = 6; cyc += 11; }
	else { op = fetch(); r = op & 7; z->r++; if (r == 6) { a = HL; cyc += 7; } }
	int x = op >> 6, y = op >> 3 & 7;
	u8 v = r == 6 ? z80_rd(a) : getr(r, 1);
	cyc += 8;
	if (x == 1) {                         /* BIT */
		F = (u8)((F & FC) | FH | ((v & (1 << y)) ? (y == 7 ? FS : 0) : FZ | FP));
		if (r == 6) cyc -= 3;
		return;
	}
	if (x == 0) v = rot(y, v);
	else if (x == 2) v &= (u8)~(1 << y);
	else v |= (u8)(1 << y);
	if (r == 6) { z80_wr(a, v); if (idx && (op & 7) != 6) setr(op & 7, v, 1); }
	else setr(r, v, 1);
}

static void op_ed(void) {
	int op = fetch(), x = op >> 6, y = op >> 3 & 7, zz = op & 7, p = y >> 1, q = y & 1;
	z->r++;
	cyc += 8;
	idx = 0;
	if (x == 1) {
		switch (zz) {
		case 0: { u8 v = 0xFF; if (y != 6) setr(y, v, 1); F = (u8)((F & FC) | szp[v]); cyc += 4; break; }   /* IN r,(C): nothing there */
		case 1: cyc += 4; break;          /* OUT (C),r */
		case 2: {
			u32 hl = HL, v = getrp(p), c = F & FC, r;
			if (q) { r = hl + v + c; F = (u8)(((hl ^ v ^ r) >> 8 & FH) | ((~(hl ^ v) & (hl ^ r) & 0x8000) ? FP : 0) | (r >> 16 & 1)); }
			else { r = hl - v - c; F = (u8)(FN | ((hl ^ v ^ r) >> 8 & FH) | (((hl ^ v) & (hl ^ r) & 0x8000) ? FP : 0) | (r >> 16 & 1)); }
			F |= (u8)((r >> 8 & FS) | ((r & 0xFFFF) ? 0 : FZ));
			set_hl((u16)r); cyc += 7; break;
		}
		case 3: { u16 a = fetch16(); if (q) setrp(p, zrd16(a)); else zwr16(a, getrp(p)); cyc += 12; break; }
		case 4: { u8 v = A; A = 0; A = sub8(v, 0); break; }
		case 5: z->pc = pop(); z->iff1 = z->iff2; cyc += 6; break;
		case 6: z->im = (u8)((y & 3) == 0 || (y & 3) == 1 ? 0 : (y & 3) - 1); break;
		default:
			switch (y) {
			case 0: z->i = A; cyc++; break;
			case 1: z->r = A; cyc++; break;
			case 2: A = z->i; F = (u8)((F & FC) | (szp[A] & ~FP) | (z->iff2 ? FP : 0)); cyc++; break;
			case 3: A = z->r; F = (u8)((F & FC) | (szp[A] & ~FP) | (z->iff2 ? FP : 0)); cyc++; break;
			case 4: { u8 m = z80_rd(HL); z80_wr(HL, (u8)(m >> 4 | A << 4)); A = (u8)((A & 0xF0) | (m & 15)); F = (u8)((F & FC) | szp[A]); cyc += 10; break; }   /* RRD */
			case 5: { u8 m = z80_rd(HL); z80_wr(HL, (u8)(m << 4 | (A & 15))); A = (u8)((A & 0xF0) | m >> 4); F = (u8)((F & FC) | szp[A]); cyc += 10; break; }  /* RLD */
			}
		}
		return;
	}
	if (x == 2 && zz < 4 && y >= 4) {     /* the block instructions */
		int dir = (y & 1) ? -1 : 1, rep = y >= 6;
		u16 hl = HL, bc = BC;
		cyc += 8;
		switch (zz) {
		case 0: {                         /* LDI LDD LDIR LDDR */
			z80_wr(DE, z80_rd(hl));
			set_de((u16)(DE + dir)); set_hl((u16)(hl + dir)); set_bc(--bc);
			F = (u8)((F & (FS | FZ | FC)) | (bc ? FP : 0));
			if (rep && bc) { z->pc -= 2; cyc += 5; }
			break;
		}
		case 1: {                         /* CPI CPD CPIR CPDR */
			int c = F & FC;
			u8 r = sub8(z80_rd(hl), 0);
			set_hl((u16)(hl + dir)); set_bc(--bc);
			F = (u8)((F & ~(FC | FP)) | c | (bc ? FP : 0));
			if (rep && bc && r) { z->pc -= 2; cyc += 5; }
			break;
		}
		default: {                        /* INI/OUTI and the rest: no ports; the counting still happens */
			if (zz == 2) z80_wr(hl, 0xFF);
			set_hl((u16)(hl + dir)); z->b--;
			F = (u8)((F & FC) | FN | (z->b ? 0 : FZ));
			if (rep && z->b) { z->pc -= 2; cyc += 5; }
		}
		}
	}
}

int z80_step(z80_t *zz_) {
	z = zz_;
	if (!tables) {
		tables = 1;
		for (int i = 0; i < 256; i++) {
			int p = 0;
			for (int b = 0; b < 8; b++) p ^= i >> b & 1;
			szp[i] = (u8)((i & FS) | (i ? 0 : FZ) | (p ? 0 : FP));
		}
	}
	cyc = 0;
	idx = 0;
	if (z->ei_wait) z->ei_wait--;
	if (z->halt) return 4;
	int op;
	for (;;) {
		op = fetch(); z->r++;
		if (op == 0xDD) { idx = 1; cyc += 4; } else if (op == 0xFD) { idx = 2; cyc += 4; } else break;
	}
	int x = op >> 6, y = op >> 3 & 7, zz = op & 7, p = y >> 1, q = y & 1;
	switch (x) {
	case 1:
		if (op == 0x76) { z->halt = 1; cyc += 4; break; }
		if (zz == 6) { setr(y, z80_rd(maddr()), 1); cyc += 7; }
		else if (y == 6) { z80_wr(maddr(), getr(zz, 1)); cyc += 7; }
		else { setr(y, getr(zz, 0), 0); cyc += 4; }
		break;
	case 2:
		if (zz == 6) { alu(y, z80_rd(maddr())); cyc += 7; } else { alu(y, getr(zz, 0)); cyc += 4; }
		break;
	case 0:
		switch (zz) {
		case 0:
			if (y == 0) cyc += 4;
			else if (y == 1) { u8 t = A; A = z->a_; z->a_ = t; t = F; F = z->f_; z->f_ = t; cyc += 4; }
			else if (y == 2) { s8 d = (s8)fetch(); if (--z->b) { z->pc += d; cyc += 13; } else cyc += 8; }
			else { s8 d = (s8)fetch(); if (y == 3 || cond(y - 4)) { z->pc += d; cyc += 12; } else cyc += 7; }
			break;
		case 1:
			if (!q) { setrp(p, fetch16()); cyc += 10; }
			else {
				u32 h = hlx(), v = getrp(p), r = h + v;
				F = (u8)((F & (FS | FZ | FP)) | ((h ^ v ^ r) >> 8 & FH) | (r >> 16 & 1));
				set_hlx((u16)r); cyc += 11;
			}
			break;
		case 2:
			switch (y) {
			case 0: z80_wr(BC, A); cyc += 7; break;
			case 1: A = z80_rd(BC); cyc += 7; break;
			case 2: z80_wr(DE, A); cyc += 7; break;
			case 3: A = z80_rd(DE); cyc += 7; break;
			case 4: zwr16(fetch16(), hlx()); cyc += 16; break;
			case 5: set_hlx(zrd16(fetch16())); cyc += 16; break;
			case 6: z80_wr(fetch16(), A); cyc += 13; break;
			default: A = z80_rd(fetch16()); cyc += 13; break;
			}
			break;
		case 3: setrp(p, (u16)(getrp(p) + (q ? -1 : 1))); cyc += 6; break;
		case 4: case 5:
			if (y == 6) { u16 a = maddr(); z80_wr(a, zz == 4 ? inc8(z80_rd(a)) : dec8(z80_rd(a))); cyc += 11; }
			else { setr(y, zz == 4 ? inc8(getr(y, 0)) : dec8(getr(y, 0)), 0); cyc += 4; }
			break;
		case 6:
			if (y == 6) { u16 a = maddr(); z80_wr(a, fetch()); cyc += idx ? 7 : 10; }   /* (LD (IX+d),n: 19 in all) */
			else { setr(y, fetch(), 0); cyc += 7; }
			break;
		default:
			cyc += 4;
			switch (y) {
			case 0: F = (u8)((F & (FS | FZ | FP)) | A >> 7); A = (u8)(A << 1 | A >> 7); break;
			case 1: F = (u8)((F & (FS | FZ | FP)) | (A & 1)); A = (u8)(A >> 1 | A << 7); break;
			case 2: { int c = F & FC; F = (u8)((F & (FS | FZ | FP)) | A >> 7); A = (u8)(A << 1 | c); break; }
			case 3: { int c = F & FC; F = (u8)((F & (FS | FZ | FP)) | (A & 1)); A = (u8)(A >> 1 | c << 7); break; }
			case 4: {                     /* DAA */
				int a = A, c = F & FC, corr = 0, h;
				if ((F & FH) || (a & 15) > 9) corr |= 6;
				if (c || a > 0x99) { corr |= 0x60; c = 1; }
				if (F & FN) { h = (F & FH) && (a & 15) < 6; a -= corr; } else { h = (a & 15) > 9; a += corr; }
				A = (u8)a;
				F = (u8)(szp[A] | c | (F & FN) | (h ? FH : 0));
				break;
			}
			case 5: A = (u8)~A; F |= FH | FN; break;
			case 6: F = (u8)((F & (FS | FZ | FP)) | FC); break;
			default: F = (u8)((F & (FS | FZ | FP)) | ((F & FC) ? FH : FC)); break;
			}
		}
		break;
	default:
		switch (zz) {
		case 0: if (cond(y)) { z->pc = pop(); cyc += 11; } else cyc += 5; break;
		case 1:
			if (!q) { u16 v = pop(); if (p == 3) { A = (u8)(v >> 8); F = (u8)v; } else setrp(p, v); cyc += 10; }
			else if (p == 0) { z->pc = pop(); cyc += 10; }
			else if (p == 1) {
				u8 t;
				t = z->b; z->b = z->b_; z->b_ = t; t = z->c; z->c = z->c_; z->c_ = t;
				t = z->d; z->d = z->d_; z->d_ = t; t = z->e; z->e = z->e_; z->e_ = t;
				t = z->h; z->h = z->h_; z->h_ = t; t = z->l; z->l = z->l_; z->l_ = t;
				cyc += 4;
			}
			else if (p == 2) { z->pc = hlx(); cyc += 4; }
			else { z->sp = hlx(); cyc += 6; }
			break;
		case 2: { u16 a = fetch16(); if (cond(y)) z->pc = a; cyc += 10; break; }
		case 3:
			switch (y) {
			case 0: z->pc = fetch16(); cyc += 10; break;
			case 1: op_cb(); break;
			case 2: fetch(); cyc += 11; break;
			case 3: fetch(); A = 0xFF; cyc += 11; break;
			case 4: { u16 v = zrd16(z->sp); zwr16(z->sp, hlx()); set_hlx(v); cyc += 19; break; }
			case 5: { u16 v = DE; set_de(HL); set_hl(v); cyc += 4; break; }
			case 6: z->iff1 = z->iff2 = 0; cyc += 4; break;
			default: z->iff1 = z->iff2 = 1; z->ei_wait = 1; cyc += 4; break;
			}
			break;
		case 4: { u16 a = fetch16(); if (cond(y)) { push(z->pc); z->pc = a; cyc += 17; } else cyc += 10; break; }
		case 5:
			if (!q) { push(p == 3 ? (u16)(A << 8 | F) : getrp(p)); cyc += 11; }
			else if (p == 0) { u16 a = fetch16(); push(z->pc); z->pc = a; cyc += 17; }
			else if (p == 2) op_ed();
			else cyc += 4;                /* (DD/FD were taken above) */
			break;
		case 6: alu(y, fetch()); cyc += 7; break;
		default: push(z->pc); z->pc = (u16)(y * 8); cyc += 11; break;
		}
	}
	return cyc;
}

/* the interrupt line is low: taken (mode 1: RST 38h) if they are enabled -> its T-states, or 0 */
int z80_irq(z80_t *zz_) {
	z = zz_;
	if (!z->iff1 || z->ei_wait) return 0;
	z->halt = 0;
	z->iff1 = z->iff2 = 0;
	push(z->pc);
	z->pc = 0x38;
	return 13;
}

void z80_reset(z80_t *zz_) {
	u8 *b = (u8 *)zz_;
	for (unsigned i = 0; i < sizeof *zz_; i++) b[i] = 0;
	zz_->sp = 0xFFFF; zz_->a = zz_->f = 0xFF;
}
