#ifndef _AGK_BLITQ_H_
#define _AGK_BLITQ_H_

/**
 * A blit queue: draw BOBs without the CPU waiting for the blitter.
 *
 * On an A500 the CPU and the blitter share one bus, and a game that starts a
 * blit and waits for it wastes both. Instead, work out every blit up front
 * (templates, at startup), place them per frame (a few stores each), queue
 * them, and let the CPU keep working: each agkBlitqAdd/agkBlitqPoll starts
 * the next blit if the blitter is free. At the end of the frame's work,
 * agkBlitqFinish waits for the rest with the blitter first on the bus.
 *
 *   // startup, per BOB frame: all planes of an interleaved bitmap in one blit
 *   tAgkBlit pTpl[2];
 *   agkBlitBob(pTpl, pBm, pMask, W, H, frame, pFb->BytesPerRow / pFb->Depth);
 *   // every frame
 *   agkBlitqReset();
 *   for each area drawn last time in this buffer:     // (starts at once)
 *     agkBlitClear(agkBlitqSlot(), &area); agkBlitqPush();
 *   ... game logic (the blitter erases meanwhile) ...
 *   for each object:
 *     tAgkBlit *pB = agkBlitqSlot();
 *     agkBlitPlace(pB, pTpl, W, pFb->Planes[0] + y * pFb->BytesPerRow, x);
 *     agkBlitArea(&area[n++], pB);       // what to clear next time
 *     agkBlitqPush();
 *   ... more work, calling agkBlitqPoll() now and then ...
 *   agkBlitqFinish();
 *
 * examples/voidrunner draws ~60 objects a frame this way at 50 fps.
 */

#include <ace/types.h>
#include <ace/utils/bitmap.h>
#include <ace/utils/custom.h>
#include <hardware/dmabits.h>

#ifndef AGK_BLITQ_MAX
#define AGK_BLITQ_MAX 160            // blits per frame (-DAGK_BLITQ_MAX=... for more)
#endif

typedef struct {
	UWORD uwCon0, uwCon1, uwAlwm, uwSize;
	UBYTE *pA, *pB, *pCD;              // A: mask (or the image, for a copy), B: image, C/D: destination
	WORD wSrcMod, wDstMod;
} tAgkBlit;

/**
 * Templates for drawing frame uwFrame of a BOB (uwW x uwH, frames stacked
 * vertically in pBm, interleaved, the depth of the destination) into an
 * interleaved bitmap whose rows are uwDstPlaneBytes wide per plane.
 * pTpl[0] is for positions where the BOB fits its own words, pTpl[1] for one
 * word more (shifted across a word boundary). pMask 0: a plain copy (A -> D),
 * half the bus of a cookie-cut: for small, nearly solid things like shots,
 * whose corners blank the background they cross.
 */
void agkBlitBob(tAgkBlit pTpl[2], const tBitMap *pBm, const tBitMap *pMask,
	UWORD uwW, UWORD uwH, UWORD uwFrame, UWORD uwDstPlaneBytes);

/** A template placed at x on the destination row pDstRow (its first plane). */
static inline void agkBlitPlace(tAgkBlit *pOut, const tAgkBlit pTpl[2], UWORD uwW, UBYTE *pDstRow, UWORD uwX) {
	UBYTE ubShift = uwX & 15;
	*pOut = pTpl[((ubShift + uwW + 15) >> 4) > ((uwW + 15) >> 4)];
	pOut->uwCon0 |= (UWORD)ubShift << 12;   // ASHIFT
	pOut->uwCon1 = (UWORD)ubShift << 12;    // BSHIFT (unused by copies)
	pOut->pCD = pDstRow + ((uwX >> 4) << 1);
}

/** Where a blit drew: all a later clear needs (8 bytes, not a whole blit's 32). */
typedef struct {
	UBYTE *pD;
	UWORD uwSize;
	WORD wMod;
} tAgkBlitArea;

static inline void agkBlitArea(tAgkBlitArea *pOut, const tAgkBlit *pDrawn) {
	pOut->pD = pDrawn->pCD;
	pOut->uwSize = pDrawn->uwSize;
	pOut->wMod = pDrawn->wDstMod;
}

/** A blit that clears an area to colour 0: D only. */
static inline void agkBlitClear(tAgkBlit *pOut, const tAgkBlitArea *pArea) {
	pOut->uwCon0 = 0x0100;                  // USED, minterm 0
	pOut->uwCon1 = 0;
	pOut->uwAlwm = 0xFFFF;
	pOut->pA = pOut->pB = 0;
	pOut->pCD = pArea->pD;
	pOut->wSrcMod = 0;
	pOut->wDstMod = pArea->wMod;
	pOut->uwSize = pArea->uwSize;
}

/** Start one blit now (the blitter must be idle). Inline: it's most of what
 *  starting a queued blit costs. */
static inline void agkBlitGo(const tAgkBlit *pB) {
	g_pCustom->bltcon0 = pB->uwCon0;
	g_pCustom->bltcon1 = pB->uwCon1;
	g_pCustom->bltafwm = 0xFFFF;
	g_pCustom->bltalwm = pB->uwAlwm;
	g_pCustom->bltapt = pB->pA;
	g_pCustom->bltbpt = pB->pB;
	g_pCustom->bltcpt = pB->pCD;
	g_pCustom->bltdpt = pB->pCD;
	g_pCustom->bltamod = pB->wSrcMod;
	g_pCustom->bltbmod = pB->wSrcMod;
	g_pCustom->bltcmod = pB->wDstMod;
	g_pCustom->bltdmod = pB->wDstMod;
	g_pCustom->bltsize = pB->uwSize;     // starts it
}

// The queue. The per-object calls are inline: at 60+ objects a frame, a
// function call and a copy each cost ~4% of an A500 frame.
extern tAgkBlit g_pAgkBlitq[AGK_BLITQ_MAX + 1];   // (+1: a slot to write into when full)
extern UWORD g_uwAgkBlitqNext, g_uwAgkBlitqCount;
void agkBlitqFull(void);

/** Start the next queued blit if the blitter is free: call between CPU work. */
static inline void agkBlitqPoll(void) {
	// (the first read of DMACONR after a start can lie on OCS Agnus: read twice)
	if(g_uwAgkBlitqNext < g_uwAgkBlitqCount &&
		((void)g_pCustom->dmaconr, !(g_pCustom->dmaconr & DMAF_BLTDONE))) {
		agkBlitGo(&g_pAgkBlitq[g_uwAgkBlitqNext++]);
	}
}

/** The next free slot: fill it (agkBlitPlace, agkBlitClear...), then agkBlitqPush(). */
static inline tAgkBlit *agkBlitqSlot(void) {
	return &g_pAgkBlitq[g_uwAgkBlitqCount];
}

/** Queue the slot just filled, and start the next blit if the blitter is free. */
static inline void agkBlitqPush(void) {
	if(g_uwAgkBlitqCount < AGK_BLITQ_MAX) {
		++g_uwAgkBlitqCount;
	}
	else {
		agkBlitqFull();
	}
	agkBlitqPoll();
}

/** Queue a copy of a blit. */
static inline void agkBlitqAdd(const tAgkBlit *pB) {
	*agkBlitqSlot() = *pB;
	agkBlitqPush();
}

/** Empty the queue (the start of a frame, after the last Finish). */
static inline void agkBlitqReset(void) {
	g_uwAgkBlitqNext = g_uwAgkBlitqCount = 0;
}

/** Wait until every queued blit is done, the blitter first on the bus (BLTPRI). */
void agkBlitqFinish(void);

#endif // _AGK_BLITQ_H_
