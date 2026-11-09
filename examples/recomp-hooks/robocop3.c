/* RoboCop 3 at a higher frame rate: the game steps some of its physics once
 * per rendered frame, so with a faster CPU (more frames a second) the car
 * felt more drag. These hooks scale those steps by the frame's share of the
 * A500's 13 VBLs per frame, so the car behaves as at 1x whatever the rate.
 *   agk recomp DIR --hook 205D4=robocop3.c:friction --hook 2071A=robocop3.c:drag --hook 20208=robocop3.c:steering --hook 1742A=robocop3.c:frame_begin --hook 259BE=robocop3.c:traffic --hook 7A868=robocop3.c:near_plane --hook 7A8CE=robocop3.c:project */
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

static int frac;                          /* the unapplied remainder, in 1/13ths of a frame's drag */

/* $205D4: subq.w #3,SPEED (coasting drag, once per rendered frame) */
int friction(void) {
	NO_HOOKS("friction");
	int delta = (s16)rd16(VBL_DELTA);
	if (delta <= 0) return 0;
	frac += 3 * delta;
	int drag = frac / A500_FRAME;
	frac %= A500_FRAME;
	u32 v = rd16(SPEED);
	u32 r = op_sub((u32)drag, v, 16);     /* the flags as subq would leave them: the bmi after it looks */
	wr16(SPEED, r);
	cpu.pc = 0x205DAu;                    /* the instruction after */
	return 1;
}

/* $2071A: subq.w #2,SPEED above 64 (air drag, once per rendered frame): the
 * terminal speed at a high frame rate was exactly 64 */
static int frac2;
int drag(void) {
	NO_HOOKS("drag");
	int delta = (s16)rd16(VBL_DELTA);
	if (delta <= 0) return 0;
	frac2 += 2 * delta;
	int d = frac2 / A500_FRAME;
	frac2 %= A500_FRAME;
	u32 v = rd16(SPEED);
	wr16(SPEED, op_sub((u32)d, v, 16));
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
	if (cur != last_written) fine_heading = cur * A500_FRAME;   /* (in 1/13ths) */
	fine_heading += (s16)D[0] * delta;
	int h = fine_heading / A500_FRAME;
	wr16(HEADING, (u32)h & 0xFFFF);
	last_written = (s16)((h & 0xFFFF) & ~63);                /* what the game's andi will leave */
	cpu.pc = 0x2020Eu;
	return 1;
}

/* $1742A: the game begins a new frame here (it reads its VBL counter). The
 * frame it just drew is complete: show it, and wait for the vertical blank so
 * each video frame shows one whole picture (the game never synced: at 4 fps
 * it didn't have to, at 50 it tears). */
/* The view is double-buffered: the copper list's BPL1PT (high word at $19F84)
 * alternates between the buffers at $3AA88 and $42AA8 each frame. If the game
 * ever shows a buffer it hasn't redrawn since it was last shown, that frame is
 * stale: RECOMP_STALE_TRACE reports it (and the hook can show the fresh one). */
#define VIEW_BPL1PTH 0x19F84u
#define VIEW_A 0x3AA88u
#define VIEW_B 0x42AA8u
static unsigned long long view_sum(u32 base) {
	unsigned long long h = 1469598103934665603ULL;
	for (u32 a = base; a < base + 8000 * 4; a += 16) h = (h ^ rd32(a)) * 1099511628211ULL;   /* a sample of the 4 planes */
	return h;
}
static unsigned long long shown_sum[2];
static int stale_frames, frames_seen;
int frame_begin(void) {
	NO_HOOKS("frame_begin");
	if (getenv("RECOMP_BLIT_TRACE")) fprintf(stderr, "blit: ---- frame_begin ----\n");
	int which = rd16(VIEW_BPL1PTH) == 4;  /* which buffer the copper shows */
	unsigned long long h = view_sum(which ? VIEW_B : VIEW_A);
	frames_seen++;
	if (h == shown_sum[which] && frames_seen > 2) {
		stale_frames++;
		if (getenv("RECOMP_STALE_TRACE")) fprintf(stderr, "stale: frame %d shows buffer %c unchanged since it was last shown\n", frames_seen, which ? 'B' : 'A');
	}
	shown_sum[which] = h;
	frame_sync();
	return 0;                             /* then the original instruction */
}

/* $259BE..$259DE: the pursued car moves by direction x speed once per rendered
 * frame (d0-d2 the direction, $2579C its speed, $2594E its position): the
 * same step spread over the frame's share of 13 VBLs, with the remainder
 * carried so nothing is lost at a high frame rate */
#define CAR_POS 0x2594Eu
#define CAR_SPEED 0x2579Cu
static long long car_acc0, car_acc2;
static long long floordiv(long long a, long long b) { long long q = a / b; if ((a % b) && ((a < 0) != (b < 0))) q--; return q; }
int traffic(void) {
	NO_HOOKS("traffic");
	int delta = (s16)rd16(VBL_DELTA);
	if (delta <= 0) return 0;
	long long spd = (s16)rd16(CAR_SPEED);
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

/* $7A868: movea.w #16,a1 - the near plane of the vertex transform. A polygon
 * with a vertex just in front of it projects to enormous coordinates and
 * sweeps across the view for a frame (a mountain "flipping up"). Further out
 * it takes the engine's partially-behind path instead. Only for the renderer
 * (called from $7B06C): the collision test uses the same routine and must
 * keep the original distance, or everything becomes an obstacle. */
int near_plane(void) {
	NO_HOOKS("near_plane");
	static int near = -1;
	if (near < 0) { const char *e = getenv("RECOMP_NEAR"); near = e ? atoi(e) : 96; }
	A[1] = (rd32(A[7]) == 0x7B070u) ? (u32)near : 16u;
	cpu.pc = 0x7A86Cu;
	return 1;
}

/* $7A8CE..$7A8D0: divs d3,d1 / divs d3,d2 - the perspective divide. When a
 * vertex is very close, x/z no longer fits 16 bits; the 68000 leaves the
 * register alone and sets V, which the engine never looks at, so the vertex
 * lands anywhere (a mountain sweeping across the view for a frame). Divide in
 * 64 bits and clamp well off-screen instead. */
int project(void) {
	NO_HOOKS("project");
	s32 z = (s16)D[3];
	if (z == 0) return 0;                 /* (the original would trap; leave it to the game) */
	long long x = (s32)D[1] / z, y = (s32)D[2] / z;
	if (x > 4000) x = 4000; if (x < -4000) x = -4000;
	if (y > 4000) y = 4000; if (y < -4000) y = -4000;
	setd(1, (u32)x, 16); setd(2, (u32)y, 16);
	cpu.pc = 0x7A8D2u;
	return 1;
}
