/* A 68000 interpreter for the code the translation doesn't have: what the
 * game loads from disk later, jump-table targets the emulator never took,
 * anything. It executes straight from memory with the same registers, flags
 * and helpers as the translated code (m68k.h), one instruction at a time,
 * until the pc lands on an address the translation knows; then the fast
 * path takes over again. RECOMP_INTERP=1 interprets everything, which is how
 * the interpreter and the translator are checked against each other. */
#include "m68k.h"
#include <stdlib.h>

int translated(u32 pc);                   /* generated: does the translation have this address? */
void note_interpreted(u32 pc);            /* main.c: record it for the next agk recomp */
extern int irq_pending;

u64 interp_count;                         /* instructions interpreted (for the report at the end) */
static int interp_all = -1;

static u32 pc;                            /* of the next word to fetch */

static inline u32 fetch16(void) { u32 v = rd16(pc); pc += 2; return v; }
static inline u32 fetch32(void) { u32 v = rd32(pc); pc += 4; return v; }

/* ------------------------------------------------------- addressing */

typedef struct { int mode, reg, bits; u32 addr; u32 imm; } ea_t;

/* resolve the effective address (fetching extension words); for modes with
 * an address it's in ea->addr, (An)+ and -(An) have moved the register */
static void ea_calc(ea_t *ea, int mode, int reg, int bits) {
	ea->mode = mode; ea->reg = reg; ea->bits = bits;
	int step = bits / 8;
	if (reg == 7 && bits == 8 && (mode == 3 || mode == 4)) step = 2;
	switch (mode) {
	case 2: ea->addr = A[reg]; break;
	case 3: ea->addr = A[reg]; A[reg] += (u32)step; break;
	case 4: A[reg] -= (u32)step; ea->addr = A[reg]; break;
	case 5: ea->addr = A[reg] + sext(fetch16(), 16); break;
	case 6: {
		u32 ext = fetch16();
		u32 idx = (ext & 0x8000) ? A[(ext >> 12) & 7] : D[(ext >> 12) & 7];
		if (!(ext & 0x800)) idx = sext(idx, 16);
		ea->addr = A[reg] + sext(ext & 0xFF, 8) + idx;
		break;
	}
	case 7:
		switch (reg) {
		case 0: ea->addr = sext(fetch16(), 16); break;
		case 1: ea->addr = fetch32(); break;
		case 2: { u32 base = pc; ea->addr = base + sext(fetch16(), 16); break; }
		case 3: {
			u32 base = pc, ext = fetch16();
			u32 idx = (ext & 0x8000) ? A[(ext >> 12) & 7] : D[(ext >> 12) & 7];
			if (!(ext & 0x800)) idx = sext(idx, 16);
			ea->addr = base + sext(ext & 0xFF, 8) + idx;
			break;
		}
		case 4: ea->imm = bits == 32 ? fetch32() : (fetch16() & MASK(bits)); break;
		}
		break;
	}
}

static u32 ea_read(const ea_t *ea) {
	switch (ea->mode) {
	case 0: return D[ea->reg] & MASK(ea->bits);
	case 1: return A[ea->reg] & MASK(ea->bits);
	case 7: if (ea->reg == 4) return ea->imm; /* fall through */
	default: return rd(ea->addr, ea->bits);
	}
}

static void ea_write(const ea_t *ea, u32 v) {
	switch (ea->mode) {
	case 0: setd(ea->reg, v, ea->bits); break;
	case 1: A[ea->reg] = sext(v, ea->bits); break;
	default: wr(ea->addr, v, ea->bits); break;
	}
}

static int cond(int cc) {
	switch (cc) {
	case 0: return 1; case 1: return 0; case 2: return CC_HI; case 3: return CC_LS;
	case 4: return CC_CC; case 5: return CC_CS; case 6: return CC_NE; case 7: return CC_EQ;
	case 8: return CC_VC; case 9: return CC_VS; case 10: return CC_PL; case 11: return CC_MI;
	case 12: return CC_GE; case 13: return CC_LT; case 14: return CC_GT; default: return CC_LE;
	}
}

static const int size_of[4] = { 8, 16, 32, 0 };

static void exception(int vector) { pc = take_exception(vector, pc); }
static void illegal(u32 at) { pc = at; exception(4); }

/* a shift or rotate: type 0 as, 1 ls, 2 rox, 3 ro; left if dir */
static u32 shift(int type, int dir, u32 v, u32 n, int bits) {
	switch (type) {
	case 0: return dir ? op_asl(v, n, bits) : op_asr(v, n, bits);
	case 1: return dir ? op_lsl(v, n, bits) : op_lsr(v, n, bits);
	case 2: return dir ? op_roxl(v, n, bits) : op_roxr(v, n, bits);
	default: return dir ? op_rol(v, n, bits) : op_ror(v, n, bits);
	}
}

static u32 bcd_add(u32 s, u32 d) {
	u32 lo = (s & 15) + (d & 15) + cpu.x, hi = (s & 0xF0) + (d & 0xF0);
	if (lo > 9) { lo += 6; }
	u32 r = hi + lo;
	if (r > 0x99) { r += 0x60; cpu.c = cpu.x = 1; } else cpu.c = cpu.x = 0;
	r &= 0xFF;
	if (r) cpu.z = 0;
	cpu.n = r >> 7 & 1;
	return r;
}

static u32 bcd_sub(u32 s, u32 d) {
	s32 lo = (s32)(d & 15) - (s32)(s & 15) - (s32)cpu.x, hi = (s32)(d & 0xF0) - (s32)(s & 0xF0);
	if (lo < 0) { lo -= 6; }
	s32 r = hi + lo;
	if (r < 0) { r -= 0x60; cpu.c = cpu.x = 1; } else cpu.c = cpu.x = 0;
	r &= 0xFF;
	if (r) cpu.z = 0;
	cpu.n = r >> 7 & 1;
	return (u32)r;
}

/* ------------------------------------------------------------ step */

static void step(void) {
	u32 at = pc;
	u32 op = fetch16();
	int mode = (op >> 3) & 7, reg = op & 7, size = size_of[(op >> 6) & 3];
	int dn = (op >> 9) & 7;
	ea_t ea;
	switch (op >> 12) {

	case 0x0: {
		if (op & 0x100) {                 /* dynamic bit ops: the bit number in Dn */
			if (mode == 1) {              /* movep */
				u32 addr = A[reg] + sext(fetch16(), 16);
				int lng = op & 0x40;
				if (op & 0x80) {          /* register to memory */
					u32 v = D[dn];
					if (lng) { wr8(addr, v >> 24); wr8(addr + 2, v >> 16); wr8(addr + 4, v >> 8); wr8(addr + 6, v); }
					else { wr8(addr, v >> 8); wr8(addr + 2, v); }
				} else {
					if (lng) D[dn] = rd8(addr) << 24 | rd8(addr + 2) << 16 | rd8(addr + 4) << 8 | rd8(addr + 6);
					else setd(dn, rd8(addr) << 8 | rd8(addr + 2), 16);
				}
				return;
			}
			int bits = mode == 0 ? 32 : 8;
			u32 bit = D[dn] & (u32)(bits - 1);
			ea_calc(&ea, mode, reg, bits);
			u32 v = ea_read(&ea), r;
			switch ((op >> 6) & 3) {
			case 0: op_btst(v, bit); return;
			case 1: r = op_bchg(v, bit); break;
			case 2: r = op_bclr(v, bit); break;
			default: r = op_bset(v, bit); break;
			}
			ea_write(&ea, r);
			return;
		}
		int kind = (op >> 9) & 7;
		if (kind == 4) {                  /* static bit ops: the bit number follows */
			u32 bit = fetch16();
			int bits = mode == 0 ? 32 : 8;
			bit &= (u32)(bits - 1);
			ea_calc(&ea, mode, reg, bits);
			u32 v = ea_read(&ea), r;
			switch ((op >> 6) & 3) {
			case 0: op_btst(v, bit); return;
			case 1: r = op_bchg(v, bit); break;
			case 2: r = op_bclr(v, bit); break;
			default: r = op_bset(v, bit); break;
			}
			ea_write(&ea, r);
			return;
		}
		if (!size) { illegal(at); return; }
		u32 imm = size == 32 ? fetch32() : (fetch16() & MASK(size));
		if (mode == 7 && reg == 4) {      /* to CCR (byte) or SR (word) */
			u32 cur = size == 8 ? (get_sr() & 0xFF) : get_sr(), r;
			switch (kind) { case 0: r = cur | imm; break; case 1: r = cur & imm; break; case 5: r = cur ^ imm; break; default: illegal(at); return; }
			if (size == 8) set_ccr(r); else set_sr(r);
			return;
		}
		ea_calc(&ea, mode, reg, size);
		u32 v = ea_read(&ea), r;
		switch (kind) {
		case 0: r = (v | imm) & MASK(size); flags_logic(r, size); ea_write(&ea, r); return;
		case 1: r = v & imm; flags_logic(r, size); ea_write(&ea, r); return;
		case 2: r = op_sub(imm, v, size); ea_write(&ea, r); return;
		case 3: r = op_add(imm, v, size); ea_write(&ea, r); return;
		case 5: r = (v ^ imm) & MASK(size); flags_logic(r, size); ea_write(&ea, r); return;
		case 6: op_cmp(imm, v, size); return;
		default: illegal(at); return;
		}
	}

	case 0x1: case 0x2: case 0x3: {       /* move */
		int bits = (op >> 12) == 1 ? 8 : (op >> 12) == 2 ? 32 : 16;
		int dmode = (op >> 6) & 7, dreg = dn;
		ea_calc(&ea, mode, reg, bits);
		u32 v = ea_read(&ea);
		if (dmode == 1) { A[dreg] = sext(v, bits); return; }   /* movea */
		ea_t dst;
		ea_calc(&dst, dmode, dreg, bits);
		ea_write(&dst, v);
		flags_logic(v, bits);
		return;
	}

	case 0x4: {
		if ((op & 0xFFC0) == 0x4E80) {    /* jsr */
			ea_calc(&ea, mode, reg, 32);
			push32(pc);
			pc = ea.addr;
			return;
		}
		if ((op & 0xFFC0) == 0x4EC0) { ea_calc(&ea, mode, reg, 32); pc = ea.addr; return; }   /* jmp */
		if ((op & 0xFFF0) == 0x4E40) { exception(32 + (op & 15)); return; }                    /* trap */
		if ((op & 0xFFF8) == 0x4E50) {    /* link */
			u32 disp = sext(fetch16(), 16);
			push32(A[reg]); A[reg] = A[7]; A[7] += disp;
			return;
		}
		if ((op & 0xFFF8) == 0x4E58) { A[7] = A[reg]; A[reg] = pop32(); return; }             /* unlk */
		if ((op & 0xFFF8) == 0x4E60) { if (cpu.s) cpu.usp = A[reg]; else A[7] = A[reg]; return; }   /* move An,usp */
		if ((op & 0xFFF8) == 0x4E68) { A[reg] = cpu.s ? cpu.usp : A[7]; return; }             /* move usp,An */
		switch (op) {
		case 0x4E70: return;              /* reset */
		case 0x4E71: return;              /* nop */
		case 0x4E72: set_sr(fetch16()); cpu.stopped = 1; return;
		case 0x4E73: { u32 sr = pop16(); u32 ret = pop32(); set_sr(sr); pc = ret; return; }   /* rte */
		case 0x4E75: pc = pop32(); return;
		case 0x4E76: if (cpu.v) exception(7); return;
		case 0x4E77: { u32 ccr = pop16(); u32 ret = pop32(); set_ccr(ccr); pc = ret; return; }   /* rtr */
		case 0x4AFC: illegal(at); return;
		}
		if ((op & 0xFFB8) == 0x4880 && mode == 0) {   /* ext */
			if (op & 0x40) D[reg] = sext(D[reg], 16); else setd(reg, sext(D[reg], 8), 16);
			flags_logic(D[reg], (op & 0x40) ? 32 : 16);
			return;
		}
		if ((op & 0xFFF8) == 0x4840) { D[reg] = (D[reg] << 16) | (D[reg] >> 16); flags_logic(D[reg], 32); return; }   /* swap */
		if ((op & 0xFFC0) == 0x4840) { ea_calc(&ea, mode, reg, 32); push32(ea.addr); return; }   /* pea */
		if ((op & 0xFB80) == 0x4880) {    /* movem */
			int lng = op & 0x40, bits = lng ? 32 : 16, step = lng ? 4 : 2;
			u32 mask = fetch16();
			if (!(op & 0x400)) {          /* registers to memory */
				if (mode == 4) {          /* predecrement: the mask is reversed, A7..D0 */
					u32 addr = A[reg];
					for (int i = 0; i < 16; i++) if (mask >> i & 1) {
						addr -= (u32)step;
						u32 v = i < 8 ? A[7 - i] : D[15 - i];
						wr(addr, v, bits);
					}
					A[reg] = addr;
				} else {
					ea_calc(&ea, mode, reg, bits);
					u32 addr = ea.addr;
					for (int i = 0; i < 16; i++) if (mask >> i & 1) {
						wr(addr, i < 8 ? D[i] : A[i - 8], bits);
						addr += (u32)step;
					}
				}
			} else {                      /* memory to registers */
				u32 addr;
				if (mode == 3) addr = A[reg]; else { ea_calc(&ea, mode, reg, bits); addr = ea.addr; }
				for (int i = 0; i < 16; i++) if (mask >> i & 1) {
					u32 v = sext(rd(addr, bits), bits);
					if (i < 8) D[i] = v; else A[i - 8] = v;
					addr += (u32)step;
				}
				if (mode == 3) A[reg] = addr;
			}
			return;
		}
		if ((op & 0xF1C0) == 0x41C0) { ea_calc(&ea, mode, reg, 32); A[dn] = ea.addr; return; }   /* lea */
		if ((op & 0xF1C0) == 0x4180) {    /* chk */
			ea_calc(&ea, mode, reg, 16);
			s32 bound = (s16)ea_read(&ea), v = (s16)D[dn];
			if (v < 0 || v > bound) { cpu.n = v < 0; exception(6); }
			return;
		}
		if ((op & 0xFFC0) == 0x40C0) { ea_calc(&ea, mode, reg, 16); ea_write(&ea, get_sr()); return; }   /* move sr,ea */
		if ((op & 0xFFC0) == 0x44C0) { ea_calc(&ea, mode, reg, 16); set_ccr(ea_read(&ea)); return; }   /* move ea,ccr */
		if ((op & 0xFFC0) == 0x46C0) { ea_calc(&ea, mode, reg, 16); set_sr(ea_read(&ea)); return; }    /* move ea,sr */
		if ((op & 0xFFC0) == 0x4AC0) {    /* tas */
			ea_calc(&ea, mode, reg, 8);
			u32 v = ea_read(&ea); flags_logic(v, 8); ea_write(&ea, v | 0x80);
			return;
		}
		if ((op & 0xFFC0) == 0x4800) {    /* nbcd */
			ea_calc(&ea, mode, reg, 8);
			ea_write(&ea, bcd_sub(ea_read(&ea), 0));
			return;
		}
		if (!size) { illegal(at); return; }
		ea_calc(&ea, mode, reg, size);
		switch ((op >> 8) & 15) {
		case 0x0: ea_write(&ea, op_negx(ea_read(&ea), size)); return;
		case 0x2: ea_write(&ea, 0); flags_logic(0, size); return;              /* clr */
		case 0x4: ea_write(&ea, op_neg(ea_read(&ea), size)); return;
		case 0x6: { u32 r = ~ea_read(&ea) & MASK(size); flags_logic(r, size); ea_write(&ea, r); return; }
		case 0xA: flags_logic(ea_read(&ea), size); return;                      /* tst */
		}
		illegal(at);
		return;
	}

	case 0x5: {
		if (size == 0) {
			if (mode == 1) {              /* dbcc */
				u32 disp = sext(fetch16(), 16);
				if (!cond((op >> 8) & 15)) {
					u32 c = (D[reg] - 1) & 0xFFFF;
					setd(reg, c, 16);
					if (c != 0xFFFF) pc = at + 2 + disp;
				}
				return;
			}
			ea_calc(&ea, mode, reg, 8);   /* scc */
			ea_write(&ea, cond((op >> 8) & 15) ? 0xFF : 0);
			return;
		}
		u32 n = dn ? (u32)dn : 8;
		if (mode == 1) { A[reg] = (op & 0x100) ? A[reg] - n : A[reg] + n; return; }   /* no flags on An */
		ea_calc(&ea, mode, reg, size);
		u32 v = ea_read(&ea);
		ea_write(&ea, (op & 0x100) ? op_sub(n, v, size) : op_add(n, v, size));
		return;
	}

	case 0x6: {
		int cc = (op >> 8) & 15;
		u32 disp = sext(op & 0xFF, 8);
		if ((op & 0xFF) == 0) disp = sext(fetch16(), 16);
		u32 target = at + 2 + disp;
		if (cc == 1) { push32(pc); pc = target; return; }   /* bsr */
		if (cc == 0 || cond(cc)) pc = target;
		return;
	}

	case 0x7: setd(dn, sext(op & 0xFF, 8), 32); flags_logic(D[dn], 32); return;   /* moveq */

	case 0x8: {
		if ((op & 0x1F0) == 0x100) {      /* sbcd */
			u32 s, d;
			if (op & 8) { A[reg] -= (reg == 7 ? 2 : 1); s = rd8(A[reg]); A[dn] -= (dn == 7 ? 2 : 1); d = rd8(A[dn]); wr8(A[dn], bcd_sub(s, d)); }
			else setd(dn, bcd_sub(D[reg] & 0xFF, D[dn] & 0xFF), 8);
			return;
		}
		if (size == 0) {                  /* divu / divs */
			ea_calc(&ea, mode, reg, 16);
			u32 s = ea_read(&ea);
			if (s == 0) { exception(5); return; }
			int ok;
			u32 r = (op & 0x100) ? op_divs(s, D[dn], &ok) : op_divu(s, D[dn], &ok);
			if (ok) D[dn] = r;
			return;
		}
		ea_calc(&ea, mode, reg, size);
		if (op & 0x100) { u32 r = (ea_read(&ea) | D[dn]) & MASK(size); flags_logic(r, size); ea_write(&ea, r); }
		else { u32 r = (ea_read(&ea) | D[dn]) & MASK(size); flags_logic(r, size); setd(dn, r, size); }
		return;
	}

	case 0x9: case 0xD: {
		int add = (op >> 12) == 0xD;
		if (size == 0) {                  /* adda / suba */
			int bits = (op & 0x100) ? 32 : 16;
			ea_calc(&ea, mode, reg, bits);
			u32 v = sext(ea_read(&ea), bits);
			A[dn] = add ? A[dn] + v : A[dn] - v;
			return;
		}
		if ((op & 0x100) && mode < 2) {   /* addx / subx */
			u32 s, d, r;
			if (mode == 0) { s = D[reg]; d = D[dn]; r = add ? op_addx(s, d, size) : op_subx(s, d, size); setd(dn, r, size); }
			else {
				int step = size / 8;
				A[reg] -= (u32)(reg == 7 && size == 8 ? 2 : step); s = rd(A[reg], size);
				A[dn] -= (u32)(dn == 7 && size == 8 ? 2 : step); d = rd(A[dn], size);
				r = add ? op_addx(s, d, size) : op_subx(s, d, size);
				wr(A[dn], r, size);
			}
			return;
		}
		ea_calc(&ea, mode, reg, size);
		if (op & 0x100) { u32 r = add ? op_add(D[dn], ea_read(&ea), size) : op_sub(D[dn], ea_read(&ea), size); ea_write(&ea, r); }
		else { u32 r = add ? op_add(ea_read(&ea), D[dn], size) : op_sub(ea_read(&ea), D[dn], size); setd(dn, r, size); }
		return;
	}

	case 0xB: {
		if (size == 0) {                  /* cmpa */
			int bits = (op & 0x100) ? 32 : 16;
			ea_calc(&ea, mode, reg, bits);
			op_cmp(sext(ea_read(&ea), bits), A[dn], 32);
			return;
		}
		if (op & 0x100) {
			if (mode == 1) {              /* cmpm */
				int step = size / 8;
				u32 s = rd(A[reg], size); A[reg] += (u32)(reg == 7 && size == 8 ? 2 : step);
				u32 d = rd(A[dn], size); A[dn] += (u32)(dn == 7 && size == 8 ? 2 : step);
				op_cmp(s, d, size);
				return;
			}
			ea_calc(&ea, mode, reg, size); /* eor */
			u32 r = (ea_read(&ea) ^ D[dn]) & MASK(size); flags_logic(r, size); ea_write(&ea, r);
			return;
		}
		ea_calc(&ea, mode, reg, size);    /* cmp */
		op_cmp(ea_read(&ea), D[dn], size);
		return;
	}

	case 0xC: {
		if ((op & 0x1F0) == 0x100) {      /* abcd */
			u32 s, d;
			if (op & 8) { A[reg] -= (reg == 7 ? 2 : 1); s = rd8(A[reg]); A[dn] -= (dn == 7 ? 2 : 1); d = rd8(A[dn]); wr8(A[dn], bcd_add(s, d)); }
			else setd(dn, bcd_add(D[reg] & 0xFF, D[dn] & 0xFF), 8);
			return;
		}
		if ((op & 0x1F8) == 0x140) { u32 t = D[dn]; D[dn] = D[reg]; D[reg] = t; return; }   /* exg dn,dn */
		if ((op & 0x1F8) == 0x148) { u32 t = A[dn]; A[dn] = A[reg]; A[reg] = t; return; }   /* exg an,an */
		if ((op & 0x1F8) == 0x188) { u32 t = D[dn]; D[dn] = A[reg]; A[reg] = t; return; }   /* exg dn,an */
		if (size == 0) {                  /* mulu / muls */
			ea_calc(&ea, mode, reg, 16);
			u32 s = ea_read(&ea);
			D[dn] = (op & 0x100) ? op_muls(s, D[dn]) : op_mulu(s, D[dn]);
			return;
		}
		ea_calc(&ea, mode, reg, size);
		if (op & 0x100) { u32 r = ea_read(&ea) & D[dn] & MASK(size); flags_logic(r, size); ea_write(&ea, r); }
		else { u32 r = ea_read(&ea) & D[dn] & MASK(size); flags_logic(r, size); setd(dn, r, size); }
		return;
	}

	case 0xE: {
		int dir = op & 0x100;
		if (size == 0) {                  /* memory shifts: a word by one */
			ea_calc(&ea, mode, reg, 16);
			ea_write(&ea, shift((op >> 9) & 3, dir, ea_read(&ea), 1, 16));
			return;
		}
		u32 n = (op & 0x20) ? (D[dn] & 63) : (dn ? (u32)dn : 8);
		setd(reg, shift((op >> 3) & 3, dir, D[reg], n, size), size);
		return;
	}

	case 0xA: pc = at; exception(10); return;
	case 0xF: pc = at; exception(11); return;
	}
	illegal(at);
}

/* is this instruction one the translated code checks for interrupts before? */
static int irq_point(u32 at) {
	u32 op = rd16(at);
	if ((op >> 12) == 6) {                /* bcc / bra / bsr */
		if (((op >> 8) & 15) == 1) return 1;
		s32 disp = (op & 0xFF) ? (s8)(op & 0xFF) : (s16)rd16(at + 2);
		return disp < 0;
	}
	if ((op & 0xFFC0) == 0x4E80) return 1;                                      /* jsr */
	if ((op & 0xF0F8) == 0x50C8) return 1;                                      /* dbcc, either way */
	return 0;
}

/* interpret from cpu.pc until the translation has the address (never, with RECOMP_INTERP) */
void interp_run(void) {
	if (interp_all < 0) interp_all = getenv("RECOMP_INTERP") != NULL;
	pc = cpu.pc;
	if (!interp_all) note_interpreted(pc);
	for (;;) {
		if (!interp_all && translated(pc)) { cpu.pc = pc; return; }
		if (cpu.stopped) { TICK(8); if (irq_pending > (int)cpu.ipl) pc = take_interrupt(irq_pending, pc); continue; }
		/* as the translated code: each instruction charged as it executes, the
		   chips' events looked at and interrupts taken at control flow (before a
		   backward branch, a dbcc, a bsr or a jsr), so the two agree exactly */
		u32 op = rd16(pc);
		int flow = (op >> 12) == 6 || (op & 0xFF80) == 0x4E80 || (op & 0xF0F8) == 0x50C8 || (op & 0xFFF0) == 0x4E40
		        || op == 0x4E71 || op == 0x4E72 || op == 0x4E73 || op == 0x4E75 || op == 0x4E77;
		CYC(8);
		if (flow) EVENTS();
		if (irq_pending > (int)cpu.ipl && irq_point(pc)) { pc = take_interrupt(irq_pending, pc); continue; }
		ITRACE(pc);
		cpu.pc = pc;
		step();
		interp_count++;
	}
}
