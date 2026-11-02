// hotpath - the Amiga side: a field of particles plotted every frame, first
// in C, then the same loop in 68000 assembly. The asm runs every frame; every
// 100 frames the C version runs on a scratch plane and the two results are
// compared with the blitter, so the asm is never trusted, only checked.
// The rules (where the particles go) are in logic.c, tested on the host.
// Double-buffered: each buffer remembers where its particles were, so the
// erase clears the pixel from two frames ago in that buffer.

#include <ace/generic/main.h>
#include <ace/managers/blit.h>
#include <ace/managers/joy.h>
#include <ace/managers/key.h>
#include <ace/managers/memory.h>
#include <ace/managers/viewport/simplebuffer.h>
#include <ace/utils/custom.h>
#include <ace/utils/extview.h>
#include <hardware/dmabits.h>
#include <agk/debug.h>
#include <agk/perf.h>
#include "art.h"
#include "logic.h"

#define PLANE_BYTES (WORLD_W / 8)
#define PLANE_SIZE (PLANE_BYTES * WORLD_H)
#define CHECK_EVERY 100
#define PAL_LINES 313

static tView *s_pView;
static tVPort *s_pVPort;
static tSimpleBufferManager *s_pBuffer;
static tGameState s_sState;
static UWORD s_pRowOffs[WORLD_H];      // y * 40: no multiply in the loop
static UWORD s_pOld[2][PARTICLES * 2]; // per buffer: where each particle was plotted there (x, y px)
static UBYTE s_ubBfr;                  // the back buffer this frame
static UWORD s_pOldCopy[PARTICLES * 2];
static UBYTE *s_pShadow;               // a plane for the C version to draw on
static UWORD s_uwCheckTimer;           // (counters, not frame % N: that's a library call on the 68000)
static UBYTE s_isCheckPending;         // the compare blit is running; its result comes next frame
static UWORD s_uwLinesC, s_uwLinesAsm;

// ------------------------------------------------------------ the hot path

// The reference: for each particle, clear the pixel it had and set its new one.
static void plotC(UBYTE *pPlane, const UWORD *pRowOffs, const tParticle *pP, UWORD *pOld, UWORD uwCount) {
	for(; uwCount--; ++pP, pOld += 2) {
		UWORD x = pOld[0], y = pOld[1];
		pPlane[pRowOffs[y] + (x >> 3)] &= (UBYTE)~(0x80 >> (x & 7));
		x = (UWORD)(pP->x >> 2);
		y = (UWORD)(pP->y >> 2);
		pOld[0] = x;
		pOld[1] = y;
		pPlane[pRowOffs[y] + (x >> 3)] |= (UBYTE)(0x80 >> (x & 7));
	}
}

// The same, by hand. What the compiler doesn't know: BCLR/BSET take the bit
// number from a register modulo 8, so 7 - (x & 7) is NOT.W x; the row table
// indexes with (a1,d1.w); dbra counts the loop.
static void plotAsm(UBYTE *pPlane, const UWORD *pRowOffs, const tParticle *pP, UWORD *pOld, UWORD uwCount) {
	UWORD uwLast = (UWORD)(uwCount - 1);
	__asm__ volatile(
		"1:\n\t"
		"move.w (%1),%%d0\n\t"          // old x
		"move.w 2(%1),%%d1\n\t"         // old y
		"add.w %%d1,%%d1\n\t"
		"move.w (%4,%%d1.w),%%d1\n\t"   // row offset
		"move.w %%d0,%%d2\n\t"
		"lsr.w #3,%%d2\n\t"
		"add.w %%d2,%%d1\n\t"           // byte offset
		"not.w %%d0\n\t"                // bit 7 - (x & 7)
		"bclr %%d0,(%3,%%d1.w)\n\t"     // erase
		"move.w (%0),%%d0\n\t"          // new x, y: 1/4 px
		"move.w 2(%0),%%d1\n\t"
		"lsr.w #2,%%d0\n\t"
		"lsr.w #2,%%d1\n\t"
		"move.w %%d0,(%1)+\n\t"         // remember them
		"move.w %%d1,(%1)+\n\t"
		"add.w %%d1,%%d1\n\t"
		"move.w (%4,%%d1.w),%%d1\n\t"
		"move.w %%d0,%%d2\n\t"
		"lsr.w #3,%%d2\n\t"
		"add.w %%d2,%%d1\n\t"
		"not.w %%d0\n\t"
		"bset %%d0,(%3,%%d1.w)\n\t"     // plot
		"addq.l #8,%0\n\t"              // sizeof(tParticle)
		"dbra %2,1b"
		: "+a"(pP), "+a"(pOld), "+d"(uwLast)
		: "a"(pPlane), "a"(pRowOffs)
		: "d0", "d1", "d2", "cc", "memory");
}

// ------------------------------------------------------------- the check

static UWORD rasterLine(void) {
	return getRayPos().bfPosY;
}

static UWORD linesSince(UWORD uwStart) {
	UWORD uwNow = rasterLine();
	return (UWORD)(uwNow >= uwStart ? uwNow - uwStart : uwNow + PAL_LINES - uwStart);
}

// The C plotter on a cleared plane with the same particles and the same "old"
// pixels gives exactly what the asm plotter leaves on the screen (every pixel
// on the screen is some particle's old one, and both erase then plot in the
// same order). Both are timed in raster lines; the blitter compares the planes
// (XOR with no output: its zero flag says whether they're identical).
//
// The blits (10 KB each) run while the CPU does other work: the clear during
// the frame's logic, the compare during the next frame's logic. Waiting for
// them instead cost 45% of the check frame.
static void checkClearStart(void) {
	blitWait();
	g_pCustom->bltcon0 = USED;              // D only, minterm 0: clear the shadow
	g_pCustom->bltcon1 = 0;
	g_pCustom->bltdpt = s_pShadow;
	g_pCustom->bltdmod = 0;
	g_pCustom->bltsize = (UWORD)((WORLD_H << 6) | (PLANE_BYTES / 2));
}

static void checkPlot(UBYTE *pPlane) {
	for(UWORD i = 0; i < PARTICLES * 2; ++i) s_pOldCopy[i] = s_pOld[s_ubBfr][i];
	blitWait();                             // (the clear: long done)
	UWORD uwT0 = rasterLine();
	plotC(s_pShadow, s_pRowOffs, s_sState.pParticles, s_pOldCopy, PARTICLES);
	s_uwLinesC = linesSince(uwT0);
	uwT0 = rasterLine();
	plotAsm(pPlane, s_pRowOffs, s_sState.pParticles, s_pOld[s_ubBfr], PARTICLES);
	s_uwLinesAsm = linesSince(uwT0);
	g_pCustom->bltcon0 = USEA | USEB | 0x3C;   // A XOR B, no D: just the zero flag
	g_pCustom->bltcon1 = 0;
	g_pCustom->bltafwm = 0xFFFF;
	g_pCustom->bltalwm = 0xFFFF;
	g_pCustom->bltapt = s_pShadow;
	g_pCustom->bltbpt = pPlane;
	g_pCustom->bltamod = 0;
	g_pCustom->bltbmod = 0;
	g_pCustom->bltsize = (UWORD)((WORLD_H << 6) | (PLANE_BYTES / 2));
	s_isCheckPending = 1;
}

static void checkResult(void) {
	blitWait();
	// DMACONR bit 13 (the NDK's misleading BLTNZERO) is BZERO: 1 = the result was all zeros
	(void)g_pCustom->dmaconr;                   // (the first read after a blit can be stale)
	UBYTE isSame = (g_pCustom->dmaconr & DMAF_BLTNZERO) != 0;
	if(isSame) {
		agkState("hot check", 1);          // "AGK hot check=1 ..." - see tests/check.agk
		agkState("c_lines", s_uwLinesC);
		agkState("asm_lines", s_uwLinesAsm);
		agkEnd();
	}
	else {
		agkPrint("AGK ERR hot: the asm plotter's screen differs from the C plotter's\n");
	}
	s_isCheckPending = 0;
}

// ------------------------------------------------------------------ game

static void readInput(tInput *pInput) {
	pInput->dx = 0;
	pInput->dy = 0;
	if(joyCheck(JOY1 + JOY_LEFT)) pInput->dx = -1;
	if(joyCheck(JOY1 + JOY_RIGHT)) pInput->dx = 1;
	if(joyCheck(JOY1 + JOY_UP)) pInput->dy = -1;
	if(joyCheck(JOY1 + JOY_DOWN)) pInput->dy = 1;
	pInput->fire = joyCheck(JOY1 + JOY_FIRE);
}

void genericCreate(void) {
	agkDebugInit();
	agkPrint("AGK boot hotpath\n");
	keyCreate();
	joyOpen();
	s_pView = viewCreate(0, TAG_VIEW_GLOBAL_PALETTE, 1, TAG_DONE);
	s_pVPort = vPortCreate(0, TAG_VPORT_VIEW, s_pView, TAG_VPORT_BPP, ART_DEPTH, TAG_DONE);
	s_pBuffer = simpleBufferCreate(0,
		TAG_SIMPLEBUFFER_VPORT, s_pVPort,
		TAG_SIMPLEBUFFER_BITMAP_FLAGS, BMF_CLEAR,
		TAG_SIMPLEBUFFER_IS_DBLBUF, 1,     // the CPU plots the buffer the display isn't showing
		TAG_DONE
	);
	artPaletteApply(s_pVPort->pPalette);
	s_pShadow = memAllocChipClear(PLANE_SIZE);
	for(UWORD y = 0; y < WORLD_H; ++y) s_pRowOffs[y] = (UWORD)(y * PLANE_BYTES);
	logicInit(&s_sState);
	for(UWORD i = 0; i < PARTICLES; ++i) {   // "old" = where they start, not yet drawn (no harm erasing)
		for(UBYTE b = 0; b < 2; ++b) {
			s_pOld[b][2 * i] = (UWORD)(s_sState.pParticles[i].x >> 2);
			s_pOld[b][2 * i + 1] = (UWORD)(s_sState.pParticles[i].y >> 2);
		}
	}
	viewLoad(s_pView);
	systemUnuse();
	agkDebugAsync(1);
}

void genericProcess(void) {
	agkPerfBegin();
	keyProcess();
	joyProcess();
	if(keyCheck(KEY_ESCAPE)) {
		gameExit();
		return;
	}
	tInput sInput;
	readInput(&sInput);
	int8_t gx = s_sState.gx, gy = s_sState.gy;
	UBYTE isCheck = ++s_uwCheckTimer == CHECK_EVERY;
	if(isCheck) {
		s_uwCheckTimer = 0;
		checkClearStart();                  // the blitter clears the shadow while the logic runs
	}
	logicUpdate(&s_sState, &sInput);

	UBYTE *pPlane = s_pBuffer->pBack->Planes[0];
	if(s_isCheckPending) {
		checkResult();                      // last frame's compare, before the plane changes again
	}
	if(isCheck) {
		checkPlot(pPlane);
	}
	else {
		plotAsm(pPlane, s_pRowOffs, s_sState.pParticles, s_pOld[s_ubBfr], PARTICLES);
	}
	s_ubBfr ^= 1;

	if(gx != s_sState.gx || gy != s_sState.gy || s_sState.frame < 3 || (s_sState.frame & 63) == 0) {
		agkState("frame", s_sState.frame);
		agkState("gx", s_sState.gx);
		agkState("gy", s_sState.gy);
		agkEnd();
	}
	viewProcessManagers(s_pView);           // swap the buffers
	copProcessBlocks();
	agkPerfEnd();
	vPortWaitForEnd(s_pVPort);
	if(s_sState.frame == 2) {
		agkReady();
	}
}

void genericDestroy(void) {
	agkDebugAsync(0);
	systemUse();
	memFree(s_pShadow, PLANE_SIZE);
	viewDestroy(s_pView);
	joyClose();
	keyDestroy();
}
