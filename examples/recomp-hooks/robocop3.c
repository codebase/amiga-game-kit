/* RoboCop 3 at a higher frame rate: the game steps some of its physics once
 * per rendered frame, so with a faster CPU (more frames a second) the car
 * felt more drag. These hooks scale those steps by the frame's share of the
 * A500's 13 VBLs per frame, so the car behaves as at 1x whatever the rate.
 *   agk recomp DIR --hook 205D4=robocop3.c:friction --hook 2071A=robocop3.c:drag --hook 20208=robocop3.c:steering --hook 1742A=robocop3.c:frame_begin */
#include "m68k.h"

#define VBL_DELTA 0x18200u               /* VBLs since the last frame, as the game measured it (a word) */
#define SPEED 0x20BFEu                   /* the car's speed (a word) */
#define A500_FRAME 13                    /* VBLs per rendered frame on the A500, what the game was tuned for */

static int frac;                          /* the unapplied remainder, in 1/13ths of a frame's drag */

/* $205D4: subq.w #3,SPEED (coasting drag, once per rendered frame) */
int friction(void) {
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
int frame_begin(void) {
	frame_sync();
	return 0;                             /* then the original instruction */
}
