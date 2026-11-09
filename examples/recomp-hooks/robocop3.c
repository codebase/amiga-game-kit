/* RoboCop 3 at a higher frame rate. The game syncs to the vertical blank, so
 * with a faster CPU (--speed 16) it renders every blank instead of every
 * 12th; some of its physics is stepped once per *rendered* frame, so at that
 * rate the car and its quarry would accelerate, slow and turn twelve times
 * too eagerly. These hooks scale those steps by the frame's share of the
 * A500's 12 blanks per frame, so everything behaves as at 1x whatever the
 * rate. Steps the game already paces by the blank (its mouse routine runs
 * from the VBL interrupt: the throttle countdown, braking, the full-lock
 * drag) are left alone.
 *   agk recomp DIR --hook 2071A=robocop3.c:drag --hook 20208=robocop3.c:steering --hook 259BE=robocop3.c:traffic
 *                  --hook 25830=robocop3.c:car_accel --hook 257FA=robocop3.c:car_converge
 *                  --hook 25876=robocop3.c:car_turn_r --hook 25898=robocop3.c:car_turn_l
 *                  --hook 22250=robocop3.c:crash_units_a --hook 222A2=robocop3.c:crash_units_b */
#include "m68k.h"
#include <stdlib.h>
#include <string.h>

/* Bisecting: RECOMP_NO_HOOKS stands every hook down; main.c sets RECOMP_NO_HOOKS_ACTIVE at
   RECOMP_NO_HOOKS_FROM=frame, and then RECOMP_NO_HOOK=name[,name] stands those down (the
   run up to that frame is unchanged, so what differs after it is that hook's doing) */
#define NO_HOOKS(name) do { if (getenv("RECOMP_NO_HOOKS")) return 0; \
	if (getenv("RECOMP_NO_HOOKS_ACTIVE")) { const char *l_ = getenv("RECOMP_NO_HOOK"); if (!l_ || strstr(l_, name)) return 0; } } while (0)

#define VBL_DELTA 0x18200u               /* VBLs since the last frame, as the game measured it (a word) */
#define SPEED 0x20BFEu                   /* the car's speed (a word) */
#define A500_FRAME 12                    /* VBLs per rendered frame on the A500 as measured in the emulator (12.4) */

/* a step of `amount` per rendered frame, spread over the frame's VBLs; the
 * remainder carries, so nothing is lost at a high frame rate */
static int scaled(int *frac, int amount, int delta) {
	*frac += amount * delta;
	int d = *frac / A500_FRAME;
	*frac %= A500_FRAME;
	return d;
}

/* $2071A: subq.w #2,SPEED above 64 - drag. Called three times per rendered
 * frame when the car is off the road ($2446C), which is scaled here, and once
 * per VBL from the mouse routine at full steering lock ($2090E), which is
 * already rate-correct and left alone. */
static int frac_drag;
int drag(void) {
	NO_HOOKS("drag");
	int delta = (s16)rd16(VBL_DELTA);
	if (delta <= 0 || rd32(A[7]) == 0x20914u) return 0;
	wr16(SPEED, op_sub((u32)scaled(&frac_drag, 2, delta), rd16(SPEED), 16));
	cpu.pc = 0x20720u;
	return 1;
}

/* $20208: add.w d0,HEADING (the steering, once per rendered frame). The game
 * then rounds HEADING to a multiple of 64, which would swallow a scaled-down
 * turn, so the full-precision heading lives here and the rounded one is what
 * the game sees; if the game changed HEADING itself (a crash), start from that. */
#define HEADING 0x20C22u
static int fine_heading, last_written = -1;
int steering(void) {
	NO_HOOKS("steering");
	int delta = (s16)rd16(VBL_DELTA);
	if (delta <= 0) return 0;
	int cur = (s16)rd16(HEADING);
	if (cur != last_written) fine_heading = cur * A500_FRAME;   /* (in 1/12ths) */
	fine_heading += (s16)D[0] * delta;
	int h = fine_heading / A500_FRAME;
	wr16(HEADING, (u32)h & 0xFFFF);
	last_written = (s16)((h & 0xFFFF) & ~63);                /* what the game's andi will leave */
	cpu.pc = 0x2020Eu;
	return 1;
}

/* $259BE..$259DE: the pursued car moves by direction x speed once per rendered
 * frame (d0-d2 the direction, $2579C its speed, $2594E its position): the
 * same step spread over the frame's share of 12 VBLs, with the remainder
 * carried so nothing is lost at a high frame rate */
#define CAR_POS 0x2594Eu
#define CAR_SPEED 0x2579Cu
static long long car_acc0, car_acc2;
static long long floordiv(long long a, long long b) { long long q = a / b; if ((a % b) && ((a < 0) != (b < 0))) q--; return q; }
/* RECOMP_CAR_SPEED=0.7 slows (or speeds) the pursued car by taste: at 50 fps the
 * same speed reads as faster than it did in a 4 fps slideshow; 1.0 is the A500's */
int traffic(void) {
	NO_HOOKS("traffic");
	int delta = (s16)rd16(VBL_DELTA);
	if (delta <= 0) return 0;
	static double factor = -1; if (factor < 0) { const char *e = getenv("RECOMP_CAR_SPEED"); factor = e ? atof(e) : 1.0; if (factor <= 0) factor = 1.0; }
	long long spd = (long long)((s16)rd16(CAR_SPEED) * factor);
	long long d0 = (s16)D[0], d2 = (s16)D[2];
	car_acc0 += d0 * spd * delta;
	car_acc2 += d2 * spd * delta;
	long long den = 65536LL * A500_FRAME;
	long long s0 = floordiv(car_acc0, den), s2 = floordiv(car_acc2, den);
	car_acc0 -= s0 * den; car_acc2 -= s2 * den;
	wr32(CAR_POS, (u32)((s32)rd32(CAR_POS) - (s32)s0));
	wr32(CAR_POS + 8, (u32)((s32)rd32(CAR_POS + 8) + (s32)s2));
	cpu.pc = 0x259E0u;                    /* the rts */
	return 1;
}

/* The pursued car's driver, once per rendered frame:
 * $25830: addi.w #30,CAR_SPEED (flooring it, capped at 256 by the next instruction) */
static int frac_ca;
int car_accel(void) {
	NO_HOOKS("car_accel");
	int delta = (s16)rd16(VBL_DELTA);
	if (delta <= 0) return 0;
	wr16(CAR_SPEED, (rd16(CAR_SPEED) + (u32)scaled(&frac_ca, 30, delta)) & 0xFFFF);
	cpu.pc = 0x25838u;
	return 1;
}

/* $257FA: sub.w d1,CAR_SPEED, d1 a quarter of the way to the speed its corner
 * allows (the next instruction wraps it into 0..511 as the original does) */
static int frac_cc;
int car_converge(void) {
	NO_HOOKS("car_converge");
	int delta = (s16)rd16(VBL_DELTA);
	if (delta <= 0) return 0;
	wr16(CAR_SPEED, (rd16(CAR_SPEED) - (u32)scaled(&frac_cc, (s16)D[1], delta)) & 0xFFFF);
	cpu.pc = 0x25800u;
	return 1;
}

/* $25876 / $25898: sub.w d0,CAR_HEADING - it turns towards its target by a
 * quarter of the angle left, then rounds to a multiple of 64, which would
 * swallow a scaled-down turn: the fine heading lives here, as for the player */
#define CAR_HEADING 0x2595Cu
static int car_fine, car_last = -1;
static int car_turn(u32 next) {
	int delta = (s16)rd16(VBL_DELTA);
	if (delta <= 0) return 0;
	int cur = (s16)rd16(CAR_HEADING);
	if (cur != car_last) car_fine = cur * A500_FRAME;
	car_fine -= (s16)D[0] * delta;
	int h = car_fine / A500_FRAME;
	wr16(CAR_HEADING, (u32)h & 0xFFFF);
	car_last = (s16)((h & 0xFFFF) & ~63);
	cpu.pc = next;
	return 1;
}
int car_turn_r(void) { NO_HOOKS("car_turn"); return car_turn(0x2587Cu); }
int car_turn_l(void) { NO_HOOKS("car_turn"); return car_turn(0x2589Eu); }

/* $22250 / $222A2: colliding with the pursued car swaps speeds between the
 * player's (per VBL) and the car's (per rendered frame) using the measured
 * VBLs per frame as the ratio, which the A500 kept near 12 and which is 1
 * here (and the formula divides by it minus 2). The ratio is 12. */
int crash_units_a(void) {
	NO_HOOKS("crash_units");
	setd(2, A500_FRAME, 16); setd(3, A500_FRAME, 16);
	cpu.pc = 0x2225Cu;
	return 1;
}
int crash_units_b(void) {
	NO_HOOKS("crash_units");
	setd(2, A500_FRAME, 16);
	cpu.pc = 0x222A8u;
	return 1;
}
