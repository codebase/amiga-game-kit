#ifndef _AGK_COPW_H_
#define _AGK_COPW_H_

/**
 * A copper list writer, for ACE's raw copper mode: WAITs and MOVEs written
 * in order into a list, with the one thing every game gets wrong once: a
 * WAIT compares 8 bits of the line, so a wait for line 256 or later needs
 * a wait for the end of line 255 first. The writer does that once, the
 * first time it's needed.
 *
 *   tAgkCopWriter sW;
 *   agkCopInit(&sW, pCopList->pBackBfr->pList, 0, s_pView->ubPosY);
 *   agkCopMove(&sW, &g_pCustom->bplcon0, 0x3200);
 *   agkCopPtrs(&sW, 0, (ULONG)pBm->Planes[0]);        // BPL1PT hi + lo
 *   UWORD uwColorAt = sW.uwPos;                         // to patch later: pList[uwColorAt].sMove
 *   agkCopMove(&sW, &g_pCustom->color[0], 0x000);
 *   agkCopWait(&sW, 100, 0);                            // game line 100 (the view's top is added)
 *   ...
 *   agkCopFill(&sW, COP_RAW_COUNT);                     // the rest: WAITs that never fire
 *
 * Double-buffered lists (TAG_VIEW_COPLIST_MODE raw, two buffers): write both
 * at startup, then patch the back one each frame. Which one is the back is
 * agkCopBfr(pCopList, pFirst), with pFirst = pCopList->pBackBfr noted at
 * startup. examples/voidrunner and examples/sidescroller use all of this.
 */

#include <ace/managers/copper.h>
#include <ace/utils/custom.h>

typedef struct {
	tCopCmd *pList;
	UWORD uwPos;
	UWORD uwBeamTop;   // the view's first line (ubPosY): game line 0
	UBYTE isPast255;
} tAgkCopWriter;

static inline void agkCopInit(tAgkCopWriter *pW, tCopCmd *pList, UWORD uwPos, UBYTE ubViewPosY) {
	pW->pList = pList;
	pW->uwPos = uwPos;
	pW->uwBeamTop = ubViewPosY;
	pW->isPast255 = 0;
}

/** WAIT for game line uwGameY (the view's top added), horizontal position ubX
 *  (0 = the start of the line; 0xD8 = after the last bitplane fetch). */
static inline void agkCopWait(tAgkCopWriter *pW, UWORD uwGameY, UBYTE ubX) {
	UWORD uwBeamY = pW->uwBeamTop + uwGameY;
	if(uwBeamY > 0xFF && !pW->isPast255) {
		copSetWait(&pW->pList[pW->uwPos++].sWait, 0xDF, 0xFF);   // the end of line 255 first
		pW->isPast255 = 1;
	}
	copSetWait(&pW->pList[pW->uwPos++].sWait, ubX, uwBeamY & 0xFF);
}

static inline void agkCopMove(tAgkCopWriter *pW, volatile void *pReg, UWORD uwValue) {
	copSetMove(&pW->pList[pW->uwPos++].sMove, pReg, uwValue);
}

/** Two MOVEs: a bitplane pointer (ubPlane 0 = BPL1PT). */
static inline void agkCopPtrs(tAgkCopWriter *pW, UBYTE ubPlane, ULONG ulAddr) {
	agkCopMove(pW, &g_pBplFetch[ubPlane].uwHi, (UWORD)(ulAddr >> 16));
	agkCopMove(pW, &g_pBplFetch[ubPlane].uwLo, (UWORD)(ulAddr & 0xFFFF));
}

/** Two MOVEs: COP2LC, then COPJMP2 jumps to that list (its WAITs continue the frame). */
static inline void agkCopJump2(tAgkCopWriter *pW, const tCopCmd *pList) {
	agkCopMove(pW, &g_pCustom->cop2lc, (UWORD)((ULONG)pList >> 16));
	agkCopMove(pW, (UWORD *)&g_pCustom->cop2lc + 1, (UWORD)((ULONG)pList & 0xFFFF));
	agkCopMove(pW, &g_pCustom->copjmp2, 0);
}

/** Pad the list to uwCount commands with WAITs that never fire; prints an
 *  'AGK ERR' when more than uwCount were written (raise the raw count). */
void agkCopFill(tAgkCopWriter *pW, UWORD uwCount);

/** 0 when pFirst (the back buffer noted at startup) is the back buffer now, else 1. */
static inline UBYTE agkCopBfr(const tCopList *pCopList, const tCopBfr *pFirst) {
	return pCopList->pBackBfr == pFirst ? 0 : 1;
}

#endif // _AGK_COPW_H_
