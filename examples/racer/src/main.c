// racer - Amiga side: display, input, and hooking the rules in logic.c up to
// the hardware. Game rules belong in logic.c (host-testable), not here.
//
// The road is a raster road driven entirely by the copper. The display is an
// OCS dual playfield, 6 bitplanes:
//   PF2 (even planes 2/4/6, colours 8-15) = the road bitmap: one row per
//        distance (logic.h), each drawn twice - light stripe rows 0..159,
//        dark stripe rows 160..319 - plus an empty row 320 for the sky. The
//        grass, rumble strips, asphalt and lane marks are all opaque colours
//        9-15, so no colour register changes per line.
//   PF1 (odd planes 1/3/5, colours 1-7) = in front: a double-buffered
//        frame buffer for the roadside objects (blitter), one per copper
//        buffer: each copper list points PF1 at its own.
//   COLOR00 = the sky gradient (shows through PF2's empty row).
// Per line the copper sets just two registers, in the horizontal blank before
// the line: BPLCON1 (the line's fine scroll) and BPL2MOD, the modulo added
// after the line's fetch - which is how the NEXT line jumps to its road row
// and horizontal position. Both are rewritten every frame from
// logicRoadLines(): two words per line.

#include <ace/generic/main.h>
#include <ace/managers/blit.h>
#include <ace/managers/copper.h>
#include <ace/managers/joy.h>
#include <ace/managers/key.h>
#include <ace/managers/memory.h>
#include <ace/managers/sprite.h>
#include <ace/managers/system.h>
#include <ace/utils/custom.h>
#include <ace/utils/extview.h>
#include <hardware/dmabits.h>
#include <hardware/intbits.h>
#include <agk/debug.h>
#include <agk/perf.h>
#include "art.h"      // generated from art/ by agk (agk help-art)
#include "gen_palm.h"  // the palm tree's and the rival's sizes (art/tools/scale_sheet.py)
#include "gen_rival.h"
#include "logic.h"
#include <ace/managers/timer.h>

// ------------------------------------------------------ display constants ---

#define BPLCON0_DPF6 0x6600   // 6 planes (BPU=6) | DBLPF | COLOR
#define BPLCON2_DPF 0x0024    // PF2PRI=0 (PF1 in front), sprites in front of both
#define DDFSTRT_SCROLL 0x30   // one fetch word early for fine scroll (costs sprite 7)
#define DDFSTOP_LORES 0xD0
#define FETCH_BYTES 42        // 21 words per line: 320 px + the scroll word
#define LINE_WAIT_X 0x06      // start of the line, before its first bitplane fetch
#define ROAD_BYTES_PER_ROW (ROAD_BMP_W / 8)
#define ROAD_BPP 3
// Only the projected rows (every ROW_STEP-th depth) are stored: depth r is
// bitmap row r / ROW_STEP. Light rows, dark rows, then the empty row.
#define ROAD_STORED (ROAD_ROWS / ROW_STEP)
#define ROAD_BMP_ROWS (2 * ROAD_STORED + 1 + BACK_H)   // + the horizon strip
#define ROAD_EMPTY_OFFS (2 * ROAD_STORED * ROAD_BYTES_PER_ROW)   // 20480: < 32K, so
                                    // every jump between rows fits the 16-bit modulo
#define ROAD_BACK_OFFS (ROAD_EMPTY_OFFS + ROAD_BYTES_PER_ROW)   // the horizon strip

#define COP_SPRITES_POS 0
#define COP_TOP_POS 16
#define COP_RAW_COUNT 1000
#define BLOCK_FIRST_LINE (REGION_TOP - 1)   // the modulo into line REGION_TOP lives here
#define BLOCK_COUNT (SCREEN_H - BLOCK_FIRST_LINE)

#define SYNC_FRAME 10         // "AGK t0": scenarios start here on every profile
// 25 fps: a new picture every 2nd vertical blank (the road's per-line work
// doesn't fit one 68000 frame). The driving rules still step at 50 Hz: two
// logicUpdate() per picture.
#define FRAME_VBLS 2

static tView *s_pView;
static tVPort *s_pVPort;
static tBitMap *s_pRoad;          // PF2 source rows, 3 planes, not interleaved
static UBYTE *s_pBlankRow;        // FETCH_BYTES of zeros

// PF1: two frame buffers (one per copper buffer) with a 48 px margin on each
// side, so objects never need clipping: screen x is buffer x - FB_X0. The
// display fetches from buffer x 32 (one word early for DDFSTRT 0x30).
#define FB_X0 48
#define FB_W (SCREEN_W + 2 * FB_X0)
#define FB_BPP 3
#define FB_PLANE_BYTES (FB_W / 8)                    // one plane's row
#define FB_ROW_BYTES (FB_PLANE_BYTES * FB_BPP)        // interleaved: all planes of a row
#define FB_FETCH_OFFS ((FB_X0 - 16) / 8)
static tBitMap *s_pFb[2];
// Objects (tObject.type: OBJ_PALM, OBJ_RIVAL) come in OBJ_LEVELS pre-scaled
// sizes, stacked in one bitmap (and its mask) per type
#define OBJ_TYPES 2
#define OBJ_LEVELS 10
_Static_assert(PALM_LEVELS == OBJ_LEVELS && RIVAL_LEVELS == OBJ_LEVELS, "one scale table for all");
static tBitMap *s_pObjBm[OBJ_TYPES], *s_pObjMask[OBJ_TYPES];
static struct { UWORD y, w, h; } s_pObjSize[OBJ_TYPES][OBJ_LEVELS];
static tRoadView s_sView;
static tObject s_pObj[OBJ_MAX];
static UBYTE s_pScaleLevel[257];         // object scale (1/256) -> size (the same for every type)
static UBYTE s_pDrawnCount[2];          // objects drawn in each buffer
static tGameState s_sState;
static tRoadRun s_pRuns[RUN_MAX];
static UWORD s_pBlockMove[BLOCK_COUNT];   // copper index of each line block's BPLCON1 MOVE
static UWORD *s_pBlockVal[2][BLOCK_COUNT];  // [buffer][block]: its BPLCON1 value word
// The blocks are 4 copper commands (8 words) apart, except that the line-255
// wrap WAIT sits before the block of line s_wWrapLine (2 words more)
static WORD s_wWrapLine;
static tCopBfr *s_pCopBfrA;                 // copper buffer 0 of s_pBlockVal
static UWORD s_pRowOffs[2][ROAD_ROWS];    // [dark][row]: byte offset of the row in a plane
// Per line lookups for copperUpdate(), indexed by the tRoadLine's (dark, row)
// word (dark << 8 | row; ROW_SKY -> the empty row) and by its left column
static WORD s_pKeyOffs[512];
static WORD s_pLeftOffs[ROAD_BMP_W];
static UWORD s_pLeftCon1[ROAD_BMP_W];
static UWORD s_pSky[SCREEN_H];
static UWORD s_uwCopUsed;

// The player's car: 48x32, 15 colours = 3 columns x an attached sprite pair
// (channels 0-5; 6 is free, 7 is lost to the early fetch). Frames: straight,
// leaning left, leaning right.
#define CAR_X ((SCREEN_W - ART_CAR_W) / 2)
#define CAR_Y 212
#define CAR_FRAME_LEFT 1
#define CAR_FRAME_RIGHT 2
static tBitMap *s_pCarFrames[ART_CAR_FRAMES][ART_CAR_PARTS];
static tSprite *s_pCar[ART_CAR_PARTS];
static UBYTE s_ubCarFrame = 0xFF;
static WORD s_wCarY;

// Road bitmap colour (PF2 index 1-7) for [dark][ROADPIX_*]
static const UBYTE s_pPixColor[2][4] = {
	{1, 3, 5, 7},   // light: grass, rumble white, asphalt, lane mark
	{2, 4, 6, 6},   // dark: grass, rumble red, asphalt, no lane mark (dashes)
};
static const UWORD s_pRoadPalette[8] = {
	0, GRASS_LIGHT, GRASS_DARK, RUMBLE_LIGHT, RUMBLE_DARK, ROAD_LIGHT, ROAD_DARK, LANE_LIGHT
};

// --------------------------------------------------------- copper list ---

typedef struct {
	tCopCmd *pList;
	UWORD uwPos;
	UWORD uwBeamTop;
	UBYTE isPast255;
} tCopWriter;

static void cwWait(tCopWriter *pW, UWORD uwGameY, UBYTE ubX) {
	UWORD uwBeamY = pW->uwBeamTop + uwGameY;
	if(uwBeamY > 0xFF && !pW->isPast255) {
		// WAIT compares 8 bits of Y: before any WAIT on line 256+ the copper
		// must wait for the end of line 255 (0xDF,0xFF). This only works if it
		// gets there while still on line 255, and if nothing after it runs
		// before line 256 - so the per-line blocks sit at the START of their
		// line (LINE_WAIT_X) and never spill into the next one. (Blocks at the
		// end of the line, x=0xD8, ran into line 256 or burst through all the
		// following WAITs on line 255: every road line below game line 211
		// showed one frozen row.)
		copSetWait(&pW->pList[pW->uwPos++].sWait, 0xDF, 0xFF);
		pW->isPast255 = 1;
	}
	copSetWait(&pW->pList[pW->uwPos++].sWait, ubX, uwBeamY & 0xFF);
}

static void cwMove(tCopWriter *pW, volatile void *pReg, UWORD uwValue) {
	copSetMove(&pW->pList[pW->uwPos++].sMove, pReg, uwValue);
}

static void cwPtrs(tCopWriter *pW, UBYTE ubPlane, ULONG ulAddr) {
	cwMove(pW, &g_pBplFetch[ubPlane].uwHi, ulAddr >> 16);
	cwMove(pW, &g_pBplFetch[ubPlane].uwLo, ulAddr & 0xFFFF);
}

static void copperWriteList(tCopCmd *pList, UWORD uwBeamTop, const tBitMap *pFb) {
	tCopWriter sW = {.pList = pList, .uwPos = COP_TOP_POS, .uwBeamTop = uwBeamTop};

	// Top of frame (vertical blank, no WAIT)
	cwMove(&sW, &g_pCustom->bplcon0, BPLCON0_DPF6);
	cwMove(&sW, &g_pCustom->bplcon2, BPLCON2_DPF);
	cwMove(&sW, &g_pCustom->ddfstrt, DDFSTRT_SCROLL);
	cwMove(&sW, &g_pCustom->ddfstop, DDFSTOP_LORES);
	cwMove(&sW, &g_pCustom->bpl1mod, FB_ROW_BYTES - FETCH_BYTES);   // PF1: interleaved frame buffer
	cwMove(&sW, &g_pCustom->bpl2mod, (UWORD)-FETCH_BYTES);   // PF2: re-read the empty row
	cwMove(&sW, &g_pCustom->bplcon1, 0);
	for(UBYTE p = 0; p < 3; ++p) {
		cwPtrs(&sW, 2 * p, (ULONG)pFb->Planes[p] + FB_FETCH_OFFS);         // PF1
		cwPtrs(&sW, 2 * p + 1, (ULONG)s_pRoad->Planes[p] + ROAD_EMPTY_OFFS); // PF2
	}
	cwMove(&sW, &g_pCustom->color[0], s_pSky[0]);
	for(UBYTE i = 1; i < 8; ++i) cwMove(&sW, &g_pCustom->color[8 + i], s_pRoadPalette[i]);

	// Sky above the road region: one colour per line (when it changes)
	for(UWORD y = 1; y < BLOCK_FIRST_LINE; ++y) {
		if(s_pSky[y] != s_pSky[y - 1]) {
			cwWait(&sW, y, 0);
			cwMove(&sW, &g_pCustom->color[0], s_pSky[y]);
		}
	}
	// A block per line b, at the start of that line before its first fetch:
	// BPLCON1 for line b, BPL2MOD for the jump after line b's fetch (to line
	// b+1's row), and the sky colour (static; the road hides it).
	for(UWORD b = BLOCK_FIRST_LINE; b < SCREEN_H; ++b) {
		cwWait(&sW, b, LINE_WAIT_X);
		s_pBlockMove[b - BLOCK_FIRST_LINE] = sW.uwPos;
		cwMove(&sW, &g_pCustom->bplcon1, 0);
		cwMove(&sW, &g_pCustom->bpl2mod, (UWORD)-FETCH_BYTES);
		cwMove(&sW, &g_pCustom->color[0], s_pSky[b]);
	}

	s_uwCopUsed = sW.uwPos;
	if(s_uwCopUsed > COP_RAW_COUNT) {
		agkPrint("AGK ERR copper list overflow: raise COP_RAW_COUNT\n");
	}
	// Spare slots before ACE's final WAIT: a WAIT that never fires
	// (never zeros: that's a MOVE to BLTDDAT and stops the copper).
	while(sW.uwPos < COP_RAW_COUNT) {
		copSetWait(&pList[sW.uwPos++].sWait, 0xDF, 0xFF);
	}
}

// Scroll within a road row: which word to fetch first, and the fine delay
// (DDFSTRT fetches one word early, so the first word may be scrolled out).
static inline WORD scrollByteOffset(WORD wLeft) {
	return (WORD)(((wLeft - 1) >> 4) * 2);
}

static inline UWORD scrollDelay(WORD wLeft) {
	return (UWORD)((16 - (wLeft & 15)) & 15);
}

static void rowOffsCreate(void) {
	// Where each depth's row is stored: only every ROW_STEP-th depth is
	// projected, so depth r is bitmap row r / ROW_STEP (light rows, then dark)
	for(UBYTE ubDark = 0; ubDark < 2; ++ubDark) {
		for(UBYTE r = 0; r < ROAD_ROWS; ++r) {
			s_pRowOffs[ubDark][r] = (ubDark * ROAD_STORED + r / ROW_STEP) * ROAD_BYTES_PER_ROW;
		}
	}
}

static void copperTablesCreate(void) {
	for(UWORD k = 0; k < 512; ++k) {
		UBYTE ubRow = k & 0xFF, ubDark = k >> 8;
		s_pKeyOffs[k] = ubRow < ROAD_ROWS ? s_pRowOffs[ubDark][ubRow]
			: (ubRow >= ROW_BACK && ubRow < ROW_BACK + BACK_H) ? ROAD_BACK_OFFS + (ubRow - ROW_BACK) * ROAD_BYTES_PER_ROW
			: ROAD_EMPTY_OFFS;
	}
	for(WORD x = 0; x < ROAD_BMP_W; ++x) {
		s_pLeftOffs[x] = scrollByteOffset(x);
		s_pLeftCon1[x] = scrollDelay(x) << 4;   // PF2 delay: BPLCON1 bits 4-7
	}
	// (sky lines have left = LEFT_MIN: offset 0 and delay 0, the empty row as is)
	tCopList *pCopList = s_pView->pCopList;
	s_pCopBfrA = pCopList->pBackBfr;
	for(UBYTE b = 0; b < 2; ++b) {
		UWORD *pVal = (UWORD *)(b ? pCopList->pFrontBfr : pCopList->pBackBfr)->pList;
		for(UWORD i = 0; i < BLOCK_COUNT; ++i) {
			// the value word of copper instruction n is word 2n+1 of the list
			s_pBlockVal[b][i] = &pVal[2 * s_pBlockMove[i] + 1];
		}
	}
	s_wWrapLine = 0x7FFF;
	for(UWORD i = 1; i < BLOCK_COUNT; ++i) {
		UWORD uwGap = s_pBlockMove[i] - s_pBlockMove[i - 1];
		if(uwGap == 5 && s_wWrapLine == 0x7FFF) {
			s_wWrapLine = BLOCK_FIRST_LINE + i;
		}
		else if(uwGap != 4) {
			agkPrint("AGK ERR copper blocks aren't evenly spaced: see copperUpdate\n");
		}
	}
}

// -------------------------------------------------------------- objects ---

static void objectsInit(void) {
	for(UBYTE l = 0; l < OBJ_LEVELS; ++l) {
		s_pObjSize[OBJ_PALM][l].y = g_pPalmLevel[l].y;
		s_pObjSize[OBJ_PALM][l].w = g_pPalmLevel[l].w;
		s_pObjSize[OBJ_PALM][l].h = g_pPalmLevel[l].h;
		s_pObjSize[OBJ_RIVAL][l].y = g_pRivalLevel[l].y;
		s_pObjSize[OBJ_RIVAL][l].w = g_pRivalLevel[l].w;
		s_pObjSize[OBJ_RIVAL][l].h = g_pRivalLevel[l].h;
	}
	// For each scale (1/256 of the nearest row's size), the size whose height
	// is closest to that fraction of the full size (the sheets all use the
	// same steps: the palm's stand for all)
	UWORD uwFull = g_pPalmLevel[PALM_LEVELS - 1].h;
	for(UWORD s = 0; s <= 256; ++s) {
		UWORD uwWant = (UWORD)((ULONG)uwFull * s / 256);
		UBYTE ubBest = 0;
		for(UBYTE l = 1; l < PALM_LEVELS; ++l) {
			WORD dNew = g_pPalmLevel[l].h - uwWant, dOld = g_pPalmLevel[ubBest].h - uwWant;
			if((dNew < 0 ? -dNew : dNew) <= (dOld < 0 ? -dOld : dOld)) ubBest = l;
		}
		s_pScaleLevel[s] = ubBest;
	}
}

// ------------------------------------------------------------ blit queue ---
// The objects' blits (erase old rectangles, draw new ones) are queued, and the
// CPU feeds them to the blitter while it works out the road (blitQueuePoll()
// in the copper loop): the blitter draws in parallel. Each is one blit over
// all 3 interleaved planes. (Fed from the blitter's interrupt instead, the
// interrupt and the final wait could both start a blit at once: garbage.)

#define BLIT_MAX (2 * OBJ_MAX + 2)
typedef struct {
	UWORD uwCon0, uwCon1, uwAlwm, uwSize;
	UBYTE *pA, *pB, *pCD;
	WORD wSrcMod, wDstMod;
} tBlit;
static tBlit s_pBlits[BLIT_MAX];
static UBYTE s_ubBlitNext, s_ubBlitCount;

static void blitGo(const tBlit *pB) {
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

// Start the next queued blit if the blitter is free
static inline void blitQueuePoll(void) {
	// (blitIsIdle() inline: the first read after a start can lie on OCS Agnus)
	if(s_ubBlitNext < s_ubBlitCount &&
		((void)g_pCustom->dmaconr, !(g_pCustom->dmaconr & DMAF_BLTDONE))) {
		blitGo(&s_pBlits[s_ubBlitNext++]);
	}
}

static void blitQueueStart(void) {
	s_ubBlitNext = 0;
	blitQueuePoll();
}

static void blitQueueWait(void) {
	while(s_ubBlitNext < s_ubBlitCount) {
		blitQueuePoll();
	}
	blitWait();
	s_ubBlitCount = 0;
	s_ubBlitNext = 0;
}

// The objects' blits, precomputed per type, size and "needs an extra word
// for the shift" (x & 15 + w > the source's width): only the shift and the
// destination change per object.
static tBlit s_pObjBlit[OBJ_TYPES][OBJ_LEVELS][2];
static UWORD s_pFbRowOffs[SCREEN_H];      // y * FB_ROW_BYTES
// What each buffer showed: its blits, reused (destination only) to erase it
static tBlit s_pErase[2][OBJ_MAX];

static void blitTemplatesCreate(void) {
	for(UBYTE t = 0; t < OBJ_TYPES; ++t) {
		const UWORD uwSrcWords = s_pObjBm[t]->BytesPerRow / (2 * FB_BPP);
		for(UBYTE l = 0; l < OBJ_LEVELS; ++l) {
			UWORD w = s_pObjSize[t][l].w, h = s_pObjSize[t][l].h;
			ULONG ulSrc = (ULONG)s_pObjSize[t][l].y * s_pObjBm[t]->BytesPerRow;
			for(UBYTE e = 0; e < 2; ++e) {
				UWORD uwWords = ((w + 15) >> 4) + e;
				tBlit *pB = &s_pObjBlit[t][l][e];
				pB->uwCon0 = USEA | USEB | USEC | USED | 0xCA;    // cookie cut (+ shift)
				pB->uwCon1 = 0;
				// A blit one word wider than the source reads a word past each
				// row: the last-word mask hides it
				pB->uwAlwm = uwWords > uwSrcWords ? 0x0000 : 0xFFFF;
				pB->pA = s_pObjMask[t]->Planes[0] + ulSrc;
				pB->pB = s_pObjBm[t]->Planes[0] + ulSrc;
				pB->pCD = 0;
				pB->wSrcMod = (WORD)(uwSrcWords * 2) - 2 * uwWords;
				pB->wDstMod = FB_PLANE_BYTES - 2 * uwWords;
				pB->uwSize = ((h * FB_BPP) << 6) | uwWords;
			}
		}
	}
	for(UWORD y = 0; y < SCREEN_H; ++y) s_pFbRowOffs[y] = y * FB_ROW_BYTES;
}

// Queue this picture's object blits for the back buffer: erase what it showed
// two pictures ago, then the objects (farthest first, so nearer ones cover them).
static void objectsQueue(UBYTE ubBfr) {
	tBitMap *pFb = s_pFb[ubBfr];
	tBlit *pErase = s_pErase[ubBfr];
	tBlit *pQ = &s_pBlits[s_ubBlitCount];
	for(UBYTE i = s_pDrawnCount[ubBfr]; i--; ++pErase, ++pQ) {
		// the same destination and size, but only D, all zeros
		*pQ = *pErase;
		pQ->uwCon0 = USED;
		pQ->uwCon1 = 0;
	}
	pErase = s_pErase[ubBfr];
	UBYTE n = logicObjects(&s_sState, &s_sView, s_pObj), ubDrawn = 0;
	const tObject *pO = s_pObj;
	for(UBYTE i = n; i--; ++pO) {
		UBYTE ubLevel = s_pScaleLevel[pO->scale];
		WORD w = s_pObjSize[pO->type][ubLevel].w, h = s_pObjSize[pO->type][ubLevel].h;
		WORD x = pO->x - (w >> 1) + FB_X0, y = pO->y - h + 1;
		if(x < 0 || x + w > FB_W || y < 0 || pO->y >= SCREEN_H) continue;   // off screen
		UBYTE ubShift = x & 15;
		const tBlit *pT = &s_pObjBlit[pO->type][ubLevel][((ubShift + w + 15) >> 4) > ((w + 15) >> 4)];
		*pQ = *pT;
		pQ->uwCon0 |= ubShift << ASHIFTSHIFT;
		pQ->uwCon1 = ubShift << BSHIFTSHIFT;
		pQ->pCD = pFb->Planes[0] + s_pFbRowOffs[y] + ((x >> 4) << 1);
		*pErase++ = *pQ++;
		++ubDrawn;
	}
	s_ubBlitCount = (UBYTE)(pQ - s_pBlits);
	s_pDrawnCount[ubBfr] = ubDrawn;
}

// Rewrite the back buffer's per-line values from this picture's road, run by
// run (a run = lines showing the same road row) from the bottom up. Inside a
// run every line gets the same BPLCON1 and the modulo -FETCH_BYTES (re-read the
// row); the last line of a run gets the jump to the row of the run below.
// Fill the uwLines line blocks above pVal (BPLCON1 = uwCon1, BPL2MOD =
// uwMod: the same row again or the next one); returns the topmost. (A pointer
// compare with a hidden stop: counting lines down made GCC work out the end
// pointer with a library multiply.)
static inline UWORD *linesFill(UWORD *pVal, UWORD uwLines, UWORD uwCon1, UWORD uwMod) {
	ULONG ulBytes = (ULONG)uwLines << 4;   // 8 words per block
	__asm__("" : "+d"(ulBytes));
	UWORD *pStop = (UWORD *)((UBYTE *)pVal - ulBytes);
	while(pVal != pStop) {
		pVal -= 8;
		pVal[0] = uwCon1;
		pVal[2] = uwMod;
	}
	return pStop;
}

static void copperUpdate(void) {
	UBYTE n = logicRoadRuns(&s_sState, s_pRuns, &s_sView);
	// The objects need this picture's road; queue their blits now so the
	// blitter draws them while the CPU does the copper loop below.
	objectsQueue(s_pView->pCopList->pBackBfr == s_pCopBfrA ? 0 : 1);
	blitQueueStart();

	// Bottom-up, a line's block is 8 words above the one below it (10 across
	// the wrap WAIT): stepping a pointer, not a table of them, halves the
	// memory accesses (the program runs from slow RAM, which shares the bus
	// with the display and the blitter)
	UWORD **pBlock = s_pBlockVal[s_pView->pCopList->pBackBfr == s_pCopBfrA ? 0 : 1];
	UWORD *pVal = pBlock[SCREEN_H - 1 - BLOCK_FIRST_LINE];    // line 255's block
	WORD wOffsBelow = 0, wBottom = SCREEN_H - 1;
	const tRoadRun *pRun = s_pRuns;
	for(UBYTE i = 0; i < n; ++i, ++pRun) {
		blitQueuePoll();   // keep the blitter busy with the objects
		WORD wLeft = pRun->left;
		WORD wOffs = s_pKeyOffs[((UWORD)pRun->dark << 8) | pRun->row] + s_pLeftOffs[wLeft];
		UWORD uwCon1 = s_pLeftCon1[wLeft];
		// its bottom line: the jump into the run below (line 255: nothing
		// follows). A stepping run (the horizon strip) shows the next row on
		// each line: its bottom row is further down the bitmap.
		WORD wTop = pRun->y;
		UWORD uwMod = (UWORD)-FETCH_BYTES;
		WORD wOffsBottom = wOffs;
		if(pRun->step) {
			uwMod = ROAD_BYTES_PER_ROW - FETCH_BYTES;
			wOffsBottom += (wBottom - wTop) << 7;
			_Static_assert(ROAD_BYTES_PER_ROW == 1 << 7, "the shift above");
		}
		pVal[0] = uwCon1;
		pVal[2] = i ? (UWORD)(wOffsBelow - wOffsBottom - FETCH_BYTES) : (UWORD)-FETCH_BYTES;
		// the lines above it in the run
		if(wBottom >= s_wWrapLine && wTop < s_wWrapLine) {
			pVal = linesFill(pVal, wBottom - s_wWrapLine, uwCon1, uwMod) - 2;   // (- 2: over the wrap WAIT)
			wBottom = s_wWrapLine;
		}
		pVal = linesFill(pVal, wBottom - wTop, uwCon1, uwMod);
		if(wTop == s_wWrapLine) pVal -= 2;
		pVal -= 8;                            // the block of the next run's bottom line
		wOffsBelow = wOffs;
		wBottom = wTop - 1;
	}
	// Line REGION_TOP-1 (always sky): the jump into the top run
	pVal[2] = (UWORD)(wOffsBelow - ROAD_EMPTY_OFFS - FETCH_BYTES);
}

// ---------------------------------------------------------------- road ---

// The horizon strip (after the empty row) is copied from art/backdrop.txt
static void backdropCopy(void) {
	tBitMap *pBack = artBackdropCreate();
	for(UBYTE y = 0; y < BACK_H; ++y) {
		for(UBYTE p = 0; p < ROAD_BPP; ++p) {
			const UWORD *pSrc = (const UWORD *)(pBack->Planes[p] + y * pBack->BytesPerRow);
			UWORD *pDst = (UWORD *)(s_pRoad->Planes[p] + ROAD_BACK_OFFS + y * ROAD_BYTES_PER_ROW);
			for(UBYTE w = 0; w < ROAD_BMP_W / 16; ++w) {
				pDst[w] = pSrc[w];
			}
		}
	}
	bitmapDestroy(pBack);
}

static void roadDraw(void) {
	// Once, at startup: each row twice (light, dark), 16 px at a time.
	for(UBYTE ubDark = 0; ubDark < 2; ++ubDark) {
		for(UBYTE r = 0; r < ROAD_ROWS; ++r) {
			if((ROAD_ROWS - 1 - r) % ROW_STEP) {
				continue;   // never projected: shares its stored row, not drawn
			}
			tRoadSpans sSp;
			logicRoadSpans(r, &sSp);
			UWORD *pPl[ROAD_BPP];
			for(UBYTE p = 0; p < ROAD_BPP; ++p) {
				pPl[p] = (UWORD *)(s_pRoad->Planes[p] + s_pRowOffs[ubDark][r]);
			}
			for(UWORD w = 0; w < ROAD_BMP_W / 16; ++w) {
				UWORD pWord[ROAD_BPP] = {0};
				for(UBYTE b = 0; b < 16; ++b) {
					WORD x = w * 16 + b;
					WORD d = x >= ROAD_CX ? x - ROAD_CX : ROAD_CX - 1 - x;
					UBYTE ubPix = d >= sSp.half ? ROADPIX_GRASS
						: d >= sSp.half - sSp.rumble ? ROADPIX_RUMBLE
						: (d >= sSp.lane - sSp.laneHalf && d <= sSp.lane + sSp.laneHalf) ? ROADPIX_LANE
						: ROADPIX_ASPHALT;
					UBYTE c = s_pPixColor[ubDark][ubPix];
					for(UBYTE p = 0; p < ROAD_BPP; ++p) {
						pWord[p] = (pWord[p] << 1) | ((c >> p) & 1);
					}
				}
				for(UBYTE p = 0; p < ROAD_BPP; ++p) pPl[p][w] = pWord[p];
			}
		}
	}
}

// ------------------------------------------------------------------ main ---

static void readInput(tInput *pInput) {
	// Joystick in port 2 (ACE JOY1). Up or fire = gas, down = brake.
	pInput->steer = 0;
	if(joyCheck(JOY1 + JOY_LEFT)) pInput->steer = -1;
	if(joyCheck(JOY1 + JOY_RIGHT)) pInput->steer = 1;
	pInput->accel = joyCheck(JOY1 + JOY_UP) || joyCheck(JOY1 + JOY_FIRE);
	pInput->brake = joyCheck(JOY1 + JOY_DOWN);
}

void genericCreate(void) {
	agkDebugInit();
	agkPrint("AGK boot racer\n");

	keyCreate();
	joyOpen();

	s_pView = viewCreate(0,
		TAG_VIEW_GLOBAL_PALETTE, 1,
		TAG_VIEW_COPLIST_MODE, VIEW_COPLIST_MODE_RAW,
		TAG_VIEW_COPLIST_RAW_COUNT, COP_RAW_COUNT,
		TAG_DONE);
	s_pVPort = vPortCreate(0, TAG_VPORT_VIEW, s_pView, TAG_VPORT_BPP, 6, TAG_DONE);

	s_pRoad = bitmapCreate(ROAD_BMP_W, ROAD_BMP_ROWS, ROAD_BPP, BMF_CLEAR);
	s_pBlankRow = memAllocChipClear(FETCH_BYTES);
	for(UBYTE b = 0; b < 2; ++b) {
		s_pFb[b] = bitmapCreate(FB_W, SCREEN_H, FB_BPP, BMF_CLEAR | BMF_INTERLEAVED);
	}
	s_pObjBm[OBJ_PALM] = artPalmCreate();
	s_pObjMask[OBJ_PALM] = artPalmCreateMask();
	s_pObjBm[OBJ_RIVAL] = artRivalCreate();
	s_pObjMask[OBJ_RIVAL] = artRivalCreateMask();
	objectsInit();
	blitTemplatesCreate();
	rowOffsCreate();
	roadDraw();
	backdropCopy();

	for(UWORD y = 0; y < SCREEN_H; ++y) s_pSky[y] = logicSkyColor(y);
	logicInit(&s_sState);

	// Copper slots 0-15 are the sprite pointers (the sprite manager fills them;
	// left at zero they'd stop the copper: 0 is a MOVE to BLTDDAT).
	spriteManagerCreate(s_pView, COP_SPRITES_POS, 0);
	systemSetDmaBit(DMAB_SPRITE, 1);
	artCarApplyColors(s_pVPort->pPalette);   // colours 17-31 (loaded by viewLoad)
	artPaletteApply(s_pVPort->pPalette);     // PF1 colours 1-7 (art/palette.txt)
	for(UBYTE f = 0; f < ART_CAR_FRAMES; ++f) {
		for(UBYTE p = 0; p < ART_CAR_PARTS; ++p) {
			s_pCarFrames[f][p] = artCarCreate(f, p);
		}
	}
	for(UBYTE p = 0; p < ART_CAR_PARTS; ++p) {
		s_pCar[p] = spriteAdd(ART_CAR_CHANNEL + p, s_pCarFrames[0][p]);
		if(p & 1) {
			spriteSetAttached(s_pCar[p], 1);   // odd channel adds colour bits 2-3
		}
	}

	tCopList *pCopList = s_pView->pCopList;
	copperWriteList(pCopList->pBackBfr->pList, s_pView->ubPosY, s_pFb[0]);
	copperWriteList(pCopList->pFrontBfr->pList, s_pView->ubPosY, s_pFb[1]);
	copperTablesCreate();
	copperUpdate();
	blitQueueWait();    // (each copperUpdate() queues a picture's blits)
	copProcessBlocks();
	copperUpdate();
	blitQueueWait();

	viewLoad(s_pView);
	systemUnuse();
	agkPerfSetFrameVbls(FRAME_VBLS);
	agkDebugAsync(1); // serial channel only: interrupt-driven from here on
}

static ULONG s_ulFrameVbl;   // vertical blank count when this picture started
static WORD s_wLastX;
static UBYTE s_ubLastOff;

static void carUpdate(BYTE bSteer) {
	UBYTE ubFrame = bSteer < 0 ? CAR_FRAME_LEFT : bSteer > 0 ? CAR_FRAME_RIGHT : 0;
	// On the grass the car shakes (1 px, every other picture). Written without
	// branches: as "offroad && speed && (frame & 2) ? 1 : 0" GCC 6.5 (-O3) left
	// the register for the 0 case uninitialised - the car jumped around the top
	// of the screen whenever it was on the road.
	WORD wBounce = (WORD)(s_sState.offroad & (s_sState.speed != 0) & ((s_sState.frame >> 1) & 1));
	WORD wY = CAR_Y + wBounce;
	// The sprite headers only when something changed (the sprite manager's work
	// costs ~3% a picture); the channel update always - it fills in the sprite
	// pointers of both copper buffers over two pictures after a change.
	UBYTE isChanged = ubFrame != s_ubCarFrame || wY != s_wCarY;
	s_wCarY = wY;
	for(UBYTE p = 0; p < ART_CAR_PARTS; ++p) {
		tSprite *pSpr = s_pCar[p];
		if(isChanged) {
			if(ubFrame != s_ubCarFrame) {
				spriteSetBitmap(pSpr, s_pCarFrames[ubFrame][p]);
			}
			pSpr->wX = CAR_X + (p >> 1) * 16;
			pSpr->wY = wY;
			spriteRequestMetadataUpdate(pSpr);
			spriteProcess(pSpr);
		}
		spriteProcessChannel(ART_CAR_CHANNEL + p);
	}
	s_ubCarFrame = ubFrame;
}

void genericProcess(void) {
	agkPerfBegin(); // frame budget meter: prints "AGK perf ... dropped= load= maxload="
	s_ulFrameVbl = timerGet();
	keyProcess();
	joyProcess();
	if(keyCheck(KEY_ESCAPE)) {
		gameExit();
		return;
	}

	tInput sInput;
	readInput(&sInput);
	UBYTE isChanged = logicUpdate(&s_sState, &sInput);
	isChanged |= logicUpdate(&s_sState, &sInput);   // 50 Hz rules, 25 fps pictures
	copperUpdate();
	carUpdate(sInput.steer);

	// State for tests: when the car goes onto or off the grass, and every
	// 5th picture (a line costs ~1% of the picture per value)
	(void)isChanged;
	if(s_sState.offroad != s_ubLastOff || s_sState.frame % 10 == 0 || s_sState.frame <= 3) {
		s_wLastX = s_sState.x;
		s_ubLastOff = s_sState.offroad;
		agkState("frame", s_sState.frame);
		agkState("pos", (LONG)s_sState.pos);
		agkState("kmh", logicKmh(&s_sState));
		agkState("x", s_sState.x);
		agkState("off", s_sState.offroad);
		agkState("laps", s_sState.laps);
		agkState("bumps", s_sState.bumps);
		agkEnd();
	}

	blitQueueWait();    // the objects are in the back buffer before it's shown
	copProcessBlocks(); // raw mode: swap copper buffers
	agkPerfEnd();
	// Next picture FRAME_VBLS vertical blanks after this one started
	do {
		vPortWaitForEnd(s_pVPort);
	} while(timerGet() - s_ulFrameVbl < FRAME_VBLS);

	if(s_sState.frame == 2) {
		agkState("copper_used", s_uwCopUsed);
		agkState("copper_max", COP_RAW_COUNT);
		agkEnd();
		agkReady();
	}
	if(s_sState.frame == SYNC_FRAME) {
		agkPrint("AGK t0\n");
	}
}

void genericDestroy(void) {
	agkDebugAsync(0); // must be off before the OS takes interrupts back
	blitQueueWait();
	systemUse();
	systemSetDmaBit(DMAB_SPRITE, 0);
	spriteManagerDestroy();
	for(UBYTE f = 0; f < ART_CAR_FRAMES; ++f) {
		for(UBYTE p = 0; p < ART_CAR_PARTS; ++p) {
			bitmapDestroy(s_pCarFrames[f][p]);
		}
	}
	memFree(s_pBlankRow, FETCH_BYTES);
	for(UBYTE t = 0; t < OBJ_TYPES; ++t) {
		bitmapDestroy(s_pObjBm[t]);
		bitmapDestroy(s_pObjMask[t]);
	}
	for(UBYTE b = 0; b < 2; ++b) bitmapDestroy(s_pFb[b]);
	bitmapDestroy(s_pRoad);
	viewDestroy(s_pView);
	joyClose();
	keyDestroy();
}
