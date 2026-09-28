#include <agk/blitq.h>
#include <agk/debug.h>
#include <ace/managers/blit.h>
#include <ace/utils/custom.h>

tAgkBlit g_pAgkBlitq[AGK_BLITQ_MAX + 1];
UWORD g_uwAgkBlitqNext, g_uwAgkBlitqCount;
static UBYTE s_isFull;

void agkBlitBob(tAgkBlit pTpl[2], const tBitMap *pBm, const tBitMap *pMask,
	UWORD uwW, UWORD uwH, UWORD uwFrame, UWORD uwDstPlaneBytes
) {
	UBYTE ubDepth = pBm->Depth;
	UWORD uwSrcWords = pBm->BytesPerRow / (2 * ubDepth);
	ULONG ulSrc = (ULONG)uwFrame * uwH * pBm->BytesPerRow;
	for(UBYTE e = 0; e < 2; ++e) {
		UWORD uwWords = ((uwW + 15) >> 4) + e;
		tAgkBlit *pT = &pTpl[e];
		pT->uwCon0 = pMask ? USEA | USEB | USEC | USED | 0xCA : USEA | USED | 0xF0;
		pT->uwCon1 = 0;
		// The extra word reads past the source row: mask it off
		pT->uwAlwm = uwWords > uwSrcWords ? 0x0000 : 0xFFFF;
		pT->pA = (pMask ? pMask : pBm)->Planes[0] + ulSrc;
		pT->pB = pBm->Planes[0] + ulSrc;
		pT->pCD = 0;
		pT->wSrcMod = (WORD)(uwSrcWords * 2 - uwWords * 2);
		pT->wDstMod = (WORD)(uwDstPlaneBytes - uwWords * 2);
		pT->uwSize = (UWORD)(((uwH * ubDepth) << 6) | uwWords);   // all planes: interleaved
	}
}

void agkBlitGo(const tAgkBlit *pB) {
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

void agkBlitqFull(void) {
	if(!s_isFull) {
		s_isFull = 1;
		agkPrint("AGK ERR blit queue full: build with a bigger -DAGK_BLITQ_MAX\n");
	}
}

void agkBlitqFinish(void) {
	// Only waiting now: let the blitter have the bus first
	g_pCustom->dmacon = DMAF_SETCLR | DMAF_BLITHOG;
	while(g_uwAgkBlitqNext < g_uwAgkBlitqCount) {
		agkBlitqPoll();
	}
	blitWait();
	g_pCustom->dmacon = DMAF_BLITHOG;
	agkBlitqReset();
}
