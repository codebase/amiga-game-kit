/*
 * The 68000 as the translated code sees it: registers, flags, memory
 * access, and the helpers every instruction compiles to. agk recomp writes
 * C that calls these; amiga.c supplies the chips behind the memory map.
 *
 * Sizes are passed as bit counts (8, 16, 32). Results are kept in 32-bit
 * registers with the operation's size masked where the 68000 masks.
 */
#ifndef _M68K_H_
#define _M68K_H_

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef int8_t s8;
typedef int16_t s16;
typedef int32_t s32;
typedef uint64_t u64;
typedef int64_t s64;

#define MEM_SIZE 0x1000000u               /* the 24-bit address space */
#define CUSTOM_BASE 0xDFF000u
#define CIA_A 0xBFE001u
#define CIA_B 0xBFD000u

typedef struct {
	u32 d[8], a[8];
	u32 pc;                               /* the address of the running instruction */
	u32 usp, ssp;                         /* the stack pointer not in a[7] */
	u32 x, n, z, v, c;                    /* 0 or 1 */
	u32 s;                                /* the supervisor bit */
	u32 ipl;                              /* the interrupt mask, 0-7 */
	u32 stopped;                          /* STOP: waiting for an interrupt */
	u32 frac;                             /* cycles not yet counted at --speed (the fraction) */
	u64 cycles;                           /* an approximation, for timing */
} cpu_t;

extern cpu_t cpu;
extern u8 *mem;                           /* MEM_SIZE bytes; the chips are trapped by address */

/* amiga.c: the chips behind the memory map */
void run_events(void);                    /* called when cycles pass the next event */
extern u64 next_event;
extern int irq_pending;                   /* the highest pending interrupt level, or 0 */
void trace_die(const char *why, u32 addr);
extern u8 *coverage_bits;                 /* --coverage: a bit per address the interpreter executed (host.c) */
#ifdef RECOMP_WATCH                       /* -DRECOMP_WATCH=0x1234: say who writes there (the pc is exact under RECOMP_INTERP) */
#define WATCH(a, bits) do { if ((a) <= (RECOMP_WATCH) && (a) + (bits) / 8 > (RECOMP_WATCH)) fprintf(stderr, "watch: write %d bits at %06X from pc %06X\n", bits, (a), cpu.pc); } while (0)
#else
#define WATCH(a, bits) ((void)0)
#endif
/* the machine's bus: rd8/16/32, wr8/16/32 (the chips trapped by address), rd/wr by
   width, CODE_WRITE for writes into translated code, MACHINE_HZ, the dispatch regions */
#include "bus.h"

/* -------------------------------------------------------- registers */

#define D cpu.d
#define A cpu.a
#define MASK(bits) ((bits) == 32 ? 0xFFFFFFFFu : (bits) == 16 ? 0xFFFFu : 0xFFu)
#define SIGN(bits) ((bits) == 32 ? 0x80000000u : (bits) == 16 ? 0x8000u : 0x80u)
static inline u32 sext(u32 v, int bits) { return bits == 8 ? (u32)(s32)(s8)v : bits == 16 ? (u32)(s32)(s16)v : v; }
/* set the low `bits` of a data register */
static inline void setd(int n, u32 v, int bits) { D[n] = bits == 32 ? v : (D[n] & ~MASK(bits)) | (v & MASK(bits)); }
static inline u32 getd(int n, int bits) { return D[n] & MASK(bits); }

static inline void push32(u32 v) { A[7] -= 4; wr32(A[7], v); }
static inline u32 pop32(void) { u32 v = rd32(A[7]); A[7] += 4; return v; }
static inline void push16(u32 v) { A[7] -= 2; wr16(A[7], v); }
static inline u32 pop16(void) { u32 v = rd16(A[7]); A[7] += 2; return v; }

static inline u32 get_sr(void) {
	return (cpu.s << 13) | (cpu.ipl << 8) | (cpu.x << 4) | (cpu.n << 3) | (cpu.z << 2) | (cpu.v << 1) | cpu.c;
}
static inline void set_ccr(u32 v) {
	cpu.x = v >> 4 & 1; cpu.n = v >> 3 & 1; cpu.z = v >> 2 & 1; cpu.v = v >> 1 & 1; cpu.c = v & 1;
}
static inline void set_sr(u32 v) {
	u32 s = v >> 13 & 1;
	if (s != cpu.s) {                     /* swap the stack pointers */
		if (cpu.s) cpu.ssp = A[7]; else cpu.usp = A[7];
		A[7] = s ? cpu.ssp : cpu.usp;
		cpu.s = s;
	}
	cpu.ipl = v >> 8 & 7;
	set_ccr(v);
}

/* ----------------------------------------------------------- flags */

static inline void flags_nz(u32 r, int bits) { r &= MASK(bits); cpu.n = (r & SIGN(bits)) != 0; cpu.z = r == 0; }
static inline void flags_logic(u32 r, int bits) { flags_nz(r, bits); cpu.v = 0; cpu.c = 0; }

static inline u32 op_add(u32 s, u32 d, int bits) {
	u32 r = (s + d) & MASK(bits), sb = SIGN(bits);
	flags_nz(r, bits);
	cpu.v = ((s ^ r) & (d ^ r) & sb) != 0;
	cpu.c = cpu.x = (bits == 32) ? ((u64)(s & 0xFFFFFFFFu) + (u64)(d & 0xFFFFFFFFu)) >> 32 & 1
	                             : (((s & MASK(bits)) + (d & MASK(bits))) >> bits) & 1;
	return r;
}
static inline u32 op_addx(u32 s, u32 d, int bits) {
	u32 r = (s + d + cpu.x) & MASK(bits), sb = SIGN(bits);
	if (r) cpu.z = 0;
	cpu.n = (r & sb) != 0;
	cpu.v = ((s ^ r) & (d ^ r) & sb) != 0;
	cpu.c = cpu.x = (bits == 32) ? ((u64)s + (u64)d + cpu.x) >> 32 & 1 : (((s & MASK(bits)) + (d & MASK(bits)) + cpu.x) >> bits) & 1;
	return r;
}
/* d - s */
static inline u32 op_sub(u32 s, u32 d, int bits) {
	u32 r = (d - s) & MASK(bits), sb = SIGN(bits);
	flags_nz(r, bits);
	cpu.v = ((s ^ d) & (r ^ d) & sb) != 0;
	cpu.c = cpu.x = (s & MASK(bits)) > (d & MASK(bits));
	return r;
}
static inline u32 op_subx(u32 s, u32 d, int bits) {
	u32 r = (d - s - cpu.x) & MASK(bits), sb = SIGN(bits);
	if (r) cpu.z = 0;
	cpu.n = (r & sb) != 0;
	cpu.v = ((s ^ d) & (r ^ d) & sb) != 0;
	cpu.c = cpu.x = (u64)(s & MASK(bits)) + cpu.x > (d & MASK(bits));
	return r;
}
static inline void op_cmp(u32 s, u32 d, int bits) {
	u32 r = (d - s) & MASK(bits), sb = SIGN(bits);
	flags_nz(r, bits);
	cpu.v = ((s ^ d) & (r ^ d) & sb) != 0;
	cpu.c = (s & MASK(bits)) > (d & MASK(bits));
}
static inline u32 op_neg(u32 d, int bits) {
	u32 r = (0u - d) & MASK(bits);
	flags_nz(r, bits);
	cpu.v = ((d & r) & SIGN(bits)) != 0;
	cpu.c = cpu.x = r != 0;
	return r;
}
static inline u32 op_negx(u32 d, int bits) {
	u32 r = (0u - d - cpu.x) & MASK(bits);
	if (r) cpu.z = 0;
	cpu.n = (r & SIGN(bits)) != 0;
	cpu.v = ((d & r) & SIGN(bits)) != 0;
	cpu.c = cpu.x = (d & MASK(bits)) != 0 || cpu.x;
	return r;
}

/* shifts and rotates: count already reduced (0-63 for registers) */
static inline u32 op_lsl(u32 v, u32 n, int bits) {
	v &= MASK(bits);
	if (n == 0) { flags_nz(v, bits); cpu.v = 0; cpu.c = 0; return v; }
	u32 c = n > (u32)bits ? 0 : (v >> (bits - n)) & 1;
	u32 r = n >= (u32)bits ? 0 : (v << n) & MASK(bits);
	flags_nz(r, bits); cpu.v = 0; cpu.c = cpu.x = c;
	return r;
}
static inline u32 op_lsr(u32 v, u32 n, int bits) {
	v &= MASK(bits);
	if (n == 0) { flags_nz(v, bits); cpu.v = 0; cpu.c = 0; return v; }
	u32 c = n > (u32)bits ? 0 : (v >> (n - 1)) & 1;
	u32 r = n >= (u32)bits ? 0 : v >> n;
	flags_nz(r, bits); cpu.v = 0; cpu.c = cpu.x = c;
	return r;
}
static inline u32 op_asl(u32 v, u32 n, int bits) {
	v &= MASK(bits);
	if (n == 0) { flags_nz(v, bits); cpu.v = 0; cpu.c = 0; return v; }
	u32 c = n > (u32)bits ? 0 : (v >> (bits - n)) & 1;
	u32 r = n >= (u32)bits ? 0 : (v << n) & MASK(bits);
	/* V: the sign changed at any point */
	u32 sb = SIGN(bits), ov = 0;
	for (u32 i = 1, t = v; i <= n && i <= (u32)bits; i++) { u32 t2 = (t << 1) & MASK(bits); if ((t ^ t2) & sb) ov = 1; t = t2; }
	if (n > (u32)bits && v) ov = 1;
	flags_nz(r, bits); cpu.v = ov; cpu.c = cpu.x = c;
	return r;
}
static inline u32 op_asr(u32 v, u32 n, int bits) {
	v &= MASK(bits);
	u32 sb = SIGN(bits);
	if (n == 0) { flags_nz(v, bits); cpu.v = 0; cpu.c = 0; return v; }
	u32 sign = (v & sb) ? MASK(bits) : 0;
	u32 c = n > (u32)bits ? (sign & 1) : (v >> (n - 1)) & 1;
	u32 r = n >= (u32)bits ? sign : ((v >> n) | (sign << (bits - n))) & MASK(bits);
	flags_nz(r, bits); cpu.v = 0; cpu.c = cpu.x = c;
	return r;
}
static inline u32 op_rol(u32 v, u32 n, int bits) {
	v &= MASK(bits);
	if (n == 0) { flags_nz(v, bits); cpu.v = 0; cpu.c = 0; return v; }
	n %= bits;
	u32 r = n ? ((v << n) | (v >> (bits - n))) & MASK(bits) : v;
	flags_nz(r, bits); cpu.v = 0; cpu.c = r & 1;
	return r;
}
static inline u32 op_ror(u32 v, u32 n, int bits) {
	v &= MASK(bits);
	if (n == 0) { flags_nz(v, bits); cpu.v = 0; cpu.c = 0; return v; }
	n %= bits;
	u32 r = n ? ((v >> n) | (v << (bits - n))) & MASK(bits) : v;
	flags_nz(r, bits); cpu.v = 0; cpu.c = (r & SIGN(bits)) != 0;
	return r;
}
static inline u32 op_roxl(u32 v, u32 n, int bits) {
	v &= MASK(bits);
	if (n == 0) { flags_nz(v, bits); cpu.v = 0; cpu.c = cpu.x; return v; }
	for (u32 i = 0; i < n; i++) { u32 c = (v & SIGN(bits)) != 0; v = ((v << 1) | cpu.x) & MASK(bits); cpu.x = c; }
	flags_nz(v, bits); cpu.v = 0; cpu.c = cpu.x;
	return v;
}
static inline u32 op_roxr(u32 v, u32 n, int bits) {
	v &= MASK(bits);
	if (n == 0) { flags_nz(v, bits); cpu.v = 0; cpu.c = cpu.x; return v; }
	for (u32 i = 0; i < n; i++) { u32 c = v & 1; v = (v >> 1) | (cpu.x ? SIGN(bits) : 0); cpu.x = c; }
	flags_nz(v, bits); cpu.v = 0; cpu.c = cpu.x;
	return v;
}

static inline u32 op_mulu(u32 s, u32 d) { u32 r = (s & 0xFFFF) * (d & 0xFFFF); flags_logic(r, 32); return r; }
static inline u32 op_muls(u32 s, u32 d) { u32 r = (u32)((s32)(s16)s * (s32)(s16)d); flags_logic(r, 32); return r; }
/* d / s -> quotient low word, remainder high word; returns 0 and sets *ok = 0 on a divide by zero / overflow */
static inline u32 op_divu(u32 s, u32 d, int *ok) {
	s &= 0xFFFF; *ok = 1;
	if (s == 0) { *ok = 0; return d; }
	u32 q = d / s, r = d % s;
	if (q > 0xFFFF) { cpu.v = 1; cpu.n = 1; cpu.c = 0; return d; }
	cpu.v = 0; cpu.c = 0; flags_nz(q, 16);
	return (r << 16) | q;
}
static inline u32 op_divs(u32 s, u32 d, int *ok) {
	s32 ss = (s16)s, dd = (s32)d; *ok = 1;
	if (ss == 0) { *ok = 0; return d; }
	s32 q = dd / ss, r = dd % ss;
	if (q > 32767 || q < -32768) { cpu.v = 1; cpu.n = 1; cpu.c = 0; return d; }
	cpu.v = 0; cpu.c = 0; flags_nz((u32)q, 16);
	return ((u32)r << 16) | ((u32)q & 0xFFFF);
}

/* condition codes */
#define CC_T 1
#define CC_F 0
#define CC_HI (!cpu.c && !cpu.z)
#define CC_LS (cpu.c || cpu.z)
#define CC_CC (!cpu.c)
#define CC_CS (cpu.c)
#define CC_NE (!cpu.z)
#define CC_EQ (cpu.z)
#define CC_VC (!cpu.v)
#define CC_VS (cpu.v)
#define CC_PL (!cpu.n)
#define CC_MI (cpu.n)
#define CC_GE (cpu.n == cpu.v)
#define CC_LT (cpu.n != cpu.v)
#define CC_GT (!cpu.z && cpu.n == cpu.v)
#define CC_LE (cpu.z || cpu.n != cpu.v)

/* bit operations on a value: returns the value after; Z from the bit before */
static inline u32 op_btst(u32 v, u32 bit) { cpu.z = !(v >> bit & 1); return v; }
static inline u32 op_bchg(u32 v, u32 bit) { cpu.z = !(v >> bit & 1); return v ^ (1u << bit); }
static inline u32 op_bclr(u32 v, u32 bit) { cpu.z = !(v >> bit & 1); return v & ~(1u << bit); }
static inline u32 op_bset(u32 v, u32 bit) { cpu.z = !(v >> bit & 1); return v | (1u << bit); }

/* BCD arithmetic (abcd/sbcd/nbcd): the X flag in, X/Z/C out, N/V as the 68000 leaves them */
static inline u32 bcd_add(u32 s, u32 d) {
	u32 lo = (s & 15) + (d & 15) + cpu.x, hi = (s & 0xF0) + (d & 0xF0);
	if (lo > 9) { lo += 6; }
	u32 r = hi + lo;
	if (r > 0x99) { r += 0x60; cpu.c = cpu.x = 1; } else cpu.c = cpu.x = 0;
	r &= 0xFF;
	if (r) cpu.z = 0;
	cpu.n = r >> 7 & 1;
	return r;
}

static inline u32 bcd_sub(u32 s, u32 d) {
	s32 lo = (s32)(d & 15) - (s32)(s & 15) - (s32)cpu.x, hi = (s32)(d & 0xF0) - (s32)(s & 0xF0);
	if (lo < 0) { lo -= 6; }
	s32 r = hi + lo;
	if (r < 0) { r -= 0x60; cpu.c = cpu.x = 1; } else cpu.c = cpu.x = 0;
	r &= 0xFF;
	if (r) cpu.z = 0;
	cpu.n = r >> 7 & 1;
	return (u32)r;
}

/* ---------------------------------------------------------- control */

/* An exception: push PC and SR on the supervisor stack, go to the vector */
static inline u32 take_exception(int vector, u32 pc) {
	u32 sr = get_sr();
	if (!cpu.s) { cpu.usp = A[7]; A[7] = cpu.ssp; cpu.s = 1; }
	push32(pc);
	push16(sr);
	return rd32((u32)vector * 4);
}
/* An interrupt of level `level`: as an exception, with the mask raised */
static inline u32 take_interrupt(int level, u32 pc) {
	u32 target = take_exception(24 + level, pc);
	cpu.ipl = (u32)level;
	cpu.stopped = 0;
	irq_ack(level);                       /* (the machine: a VDP drops its pending flag, Paula keeps INTREQ) */
	return target;
}

void frame_sync(void);                    /* for hooks: the game finished a frame; show it and wait for the vertical blank */
void frame_wait(void);                    /* for hooks: wait for the vertical blank, keep showing the last picture */
extern int cycle_shift;                   /* --speed: the CPU this much faster than an A500 (cycles >> shift) */
/* the translated code charges each instruction as it executes (CYC) and looks
 * at the chips' events at control flow (EVENTS); the interpreter does the same.
 * --speed divides the cost by 2^cycle_shift; the remainder carries over, so a
 * fast CPU still advances time (8 >> 4 would be 0) */
#define CYC(n) do { cpu.frac += (n); cpu.cycles += cpu.frac >> cycle_shift; cpu.frac &= (1u << cycle_shift) - 1; } while (0)
#define TICK(n) do { CYC(n); if (cpu.cycles >= next_event) run_events(); } while (0)
#define EVENTS() do { if (cpu.cycles >= next_event) run_events(); } while (0)

/* the per-instruction trace, for agk recomp --diff (built with -DRECOMP_ITRACE) */
#ifdef RECOMP_ITRACE
void itrace(u32 addr);
#define ITRACE(a) itrace(a)
#else
#define ITRACE(a) ((void)0)
#endif

#endif
