#include <agk/stars.h>
#include <agk/debug.h>
#include <ace/managers/memory.h>
#include <ace/utils/custom.h>

#define LINE_WAIT_X 0x06   // the start of a line, before its first fetch
#define WORDS_PER_LINE 5   // WAIT, POS, DATB, DATA, maybe COLOR00
static const UBYTE s_pStarData[4][2] = {{0, 0}, {0x80, 0}, {0, 0x80}, {0xC0, 0xC0}};   // DATA, DATB high bytes

void agkStarsCreate(tAgkStars *pStars, UBYTE ubChannel, UWORD uwY0, UWORD uwY1, UBYTE ubViewPosY,
	ULONG ulSeed, const UWORD *pColor0, UWORD uwScreenW
) {
	if(uwY1 - uwY0 > AGK_STARS_MAX_LINES) {
		agkPrint("AGK ERR agkStarsCreate: more lines than AGK_STARS_MAX_LINES\n");
		uwY1 = uwY0 + AGK_STARS_MAX_LINES;
	}
	pStars->ubHMin = (UBYTE)((128 - 16) / 2);          // HSTART / 2 at screen x -16
	pStars->uwHSpan = (uwScreenW + 16) / 2;
	pStars->uwListLen = (uwY1 - uwY0) * WORDS_PER_LINE + 8;
	for(UBYTE l = 0; l < 2; ++l) {
		pStars->pList[l] = memAllocChip(pStars->uwListLen * sizeof(tCopCmd));
		tAgkCopWriter sW;
		agkCopInit(&sW, pStars->pList[l], 0, ubViewPosY);
		for(UBYTE k = 0; k < AGK_STARS_LAYERS; ++k) pStars->pCount[k] = 0;
		ULONG ulRnd = ulSeed;                            // (the same stars in both lists)
		for(UWORD y = uwY0; y < uwY1; ++y) {
			ulRnd ^= ulRnd << 13; ulRnd ^= ulRnd >> 17; ulRnd ^= ulRnd << 5;
			// a star on half the lines: half of them far and dim, a quarter mid, a quarter near
			static const UBYTE pLayer[8] = {0, 0, 0, 0, 1, 1, 2, 3};
			UBYTE ubLayer = pLayer[(ulRnd >> 8) & 7];   // 1 far .. 3 near, 0 none
			UWORD uwH = pStars->ubHMin + (UWORD)((ulRnd >> 16) % pStars->uwHSpan);
			agkCopWait(&sW, y, LINE_WAIT_X);
			UWORD uwPos = sW.uwPos;
			// VSTART 0: a line the display never reaches. (This line's number
			// there wakes the sprite's DMA, which then fetches "data" from
			// wherever its pointer is over what the copper writes: dashes)
			agkCopMove(&sW, &g_pCustom->spr[ubChannel].pos, uwH);
			agkCopMove(&sW, &g_pCustom->spr[ubChannel].datab, (UWORD)(s_pStarData[ubLayer][1] << 8));
			agkCopMove(&sW, &g_pCustom->spr[ubChannel].dataa, (UWORD)(s_pStarData[ubLayer][0] << 8));   // arms it
			if(pColor0 && (y == 0 || pColor0[y] != pColor0[y - 1])) {
				agkCopMove(&sW, &g_pCustom->color[0], pColor0[y]);
			}
			if(ubLayer) {
				// the low byte of the MOVE's value word (big-endian: +3 in the command)
				pStars->pPos[l][ubLayer - 1][pStars->pCount[ubLayer - 1]++] = (UBYTE *)&pStars->pList[l][uwPos] + 3;
			}
		}
		copSetWait(&pStars->pList[l][sW.uwPos++].sWait, 0xFF, 0xFF);   // the end of the frame
		if(sW.uwPos > pStars->uwListLen) agkPrint("AGK ERR agkStarsCreate: list overflow\n");
	}
}

void agkStarsDestroy(tAgkStars *pStars) {
	for(UBYTE l = 0; l < 2; ++l) memFree(pStars->pList[l], pStars->uwListLen * sizeof(tCopCmd));
}

// Steps a layer has moved by frame t: near 2 px a frame, mid 1, far 1/2
// (a step is 2 px, the position byte's unit)
static inline UWORD starSteps(UBYTE ubLayer, WORD t) {
	if(t < 0) return 0;
	return (UWORD)(ubLayer == 2 ? t : ubLayer == 1 ? t >> 1 : t >> 2);
}

void agkStarsUpdate(tAgkStars *pStars, UBYTE ubBfr, UWORD uwFrame) {
	// This list last showed frame t - 2: move each layer by what it moves in two frames
	WORD t = (WORD)uwFrame;
	for(UBYTE ubLayer = 0; ubLayer < AGK_STARS_LAYERS; ++ubLayer) {
		UBYTE ubSteps = (UBYTE)(starSteps(ubLayer, t) - starSteps(ubLayer, (WORD)(t - 2)));
		UWORD n = pStars->pCount[ubLayer];
		if(!ubSteps || !n) continue;
		UBYTE **pp = pStars->pPos[ubBfr][ubLayer];
#if defined(__mc68000__)
		// The hot loop by hand: ~56 cycles a star (GCC's is about twice that).
		// A star's position byte: minus the steps; below the left edge, plus the span.
		UBYTE ubMin = pStars->ubHMin, ubSpan = (UBYTE)pStars->uwHSpan;
		UWORD uwCount = (UWORD)(n - 1);              // dbra: count + 1 times
		__asm__ volatile(
			"1:\n\t"
			"move.l (%0)+,%%a0\n\t"
			"move.b (%%a0),%%d0\n\t"
			"sub.b %2,%%d0\n\t"
			"cmp.b %3,%%d0\n\t"
			"bcc.s 2f\n\t"
			"add.b %4,%%d0\n"
			"2:\n\t"
			"move.b %%d0,(%%a0)\n\t"
			"dbra %1,1b"
			: "+a"(pp), "+d"(uwCount)
			: "d"(ubSteps), "d"(ubMin), "d"(ubSpan)
			: "a0", "d0", "cc", "memory");
#else
		for(; n--; ++pp) {
			UBYTE *p = *pp;
			UBYTE v = (UBYTE)(*p - ubSteps);
			if(v < pStars->ubHMin) v = (UBYTE)(v + pStars->uwHSpan);
			*p = v;
		}
#endif
	}
}
