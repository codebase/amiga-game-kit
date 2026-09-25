// sidescroller - Amiga side: display, input, and hooking the rules in logic.c up
// to the hardware. Game rules belong in logic.c (host-testable), not here.
//
// The display is an OCS DUAL PLAYFIELD built from a raw copper list (ACE has
// no dual-playfield support, so no simple/scroll buffer manager is used):
//   PF1 (front) = odd bitplanes 1/3/5 = the level, 1280x256, colours 1-7
//   PF2 (back)  = even bitplanes 2/4/6 = parallax bands, colours 9-15
//   COLOR00     = sky gradient, shows where both playfields are transparent
// The copper splits PF2 into bands: blank above line 39, the mountains
// (camera/4) from line 39, a one-line gap at 135 (palette reload), the
// hills (camera/2) from line 136, blank again from line 216. Each band gets
// its own bitplane pointers, fine scroll and palette. See README.md for register values and the list layout.

#include <ace/generic/main.h>
#include <ace/managers/blit.h>
#include <ace/managers/copper.h>
#include <ace/managers/joy.h>
#include <ace/managers/key.h>
#include <ace/managers/sprite.h>
#include <ace/managers/system.h>
#include <ace/utils/custom.h>
#include <ace/utils/extview.h>
#include <hardware/dmabits.h>
#include <agk/debug.h>
#include <agk/perf.h>
#include "art.h"      // generated from art/ by agk (agk help-art)
#include "sound.h"    // generated from sound/ by agk (agk help-sound)
#include "logic.h"

// ------------------------------------------------------ display constants ---

#define BPLCON0_DPF 0x6600   // 6 planes (BPU=6) | DBLPF (bit 10) | COLOR (bit 9)
#define BPLCON2_DPF 0x0024   // PF2PRI=0 (PF1 in front), PF1P=PF2P=4: sprites in front of both
#define DDFSTRT_SCROLL 0x30  // one fetch word earlier than 0x38 for fine scroll (costs sprite 7)
#define DDFSTOP_LORES 0xD0
#define FETCH_BYTES 42       // 21 words per line: 320 px + the extra scroll word
#define BAND_WAIT_X 0xD8     // after the last fetch of the line (DDFSTOP 0xD0 + 8)

#define SYNC_FRAME 10     // "AGK t0" (see the end of genericProcess)

#define LEVEL_BPP 3
#define BAND_BPP 3

static tView *s_pView;
static tVPort *s_pVPort;
static tBitMap *s_pLevel;        // PF1: whole level, 1280x256x3, interleaved
static tBitMap *s_pTiles;
static tBitMap *s_pMountains;    // PF2 band 1, 704x96x3, interleaved
static tBitMap *s_pHills;        // PF2 band 2, 704x80x3, interleaved
static UWORD *s_pBlankRow;       // PF2 above/below the bands: 42 zero bytes, modulo -42
// Hero: 32x32, 15 colours = 2 columns x attached pair = 4 hardware sprites
// (channels 0-3). One bitmap per frame and part; frames 8.. face left.
#define HERO_X_OFS 8    // art is 32 wide around the 16 px hitbox
#define HERO_Y_OFS 11   // feet (row 30 of the art) 4 px into the tile top face (depth)
static tBitMap *s_pHeroFrames[ART_HERO_FRAMES][ART_HERO_PARTS];
static tSprite *s_pHero[ART_HERO_PARTS];
static UBYTE s_ubShownHeroFrame;
static tGameState s_sState;

// Enemies: ALL of them share ONE attached sprite pair, channels 4 (colour
// bits 0-1) + 5 (bits 2-3, ATTACH), by vertical multiplexing. Each frame we
// write a sprite DMA chain per channel into chip RAM, top to bottom:
//   POS, CTL, 16 x (DATA, DATB), POS, CTL, 16 x (DATA, DATB), ..., 0, 0
// After a sprite's VSTOP line the DMA fetches the next POS/CTL pair, so the
// next sprite must start at least one line below: logicEnemyPlan() decides
// which enemies fit. The chains are double-buffered with the copper list:
// copper buffer A's SPR4PT/SPR5PT MOVEs point at chain A, B's at chain B,
// written once at startup - ACE's sprite manager never touches channels we
// don't spriteAdd() (it only blanks all 8 once in spriteManagerCreate()).
#define ENEMY_CHANNEL ART_ENEMY_CHANNEL      // 4, and 5 attached
#define ENEMY_SPRITE_WORDS (2 + 2 * ART_ENEMY_H)
#define ENEMY_SINK 4   // drawn 4 px down into the tile top face, like the hero (depth)
#define ENEMY_CHAIN_WORDS (ENEMY_COUNT * ENEMY_SPRITE_WORDS + 2)
#define SPRxCTL_ATTACH 0x0080
static tBitMap *s_pEnemyFrames[ART_ENEMY_FRAMES][ART_ENEMY_PARTS];
static const UWORD *s_pEnemyData[ART_ENEMY_FRAMES][ART_ENEMY_PARTS]; // 16 x (DATA, DATB)
static UWORD *s_pEnemyChain[2][ART_ENEMY_PARTS];   // [copper buffer][part]
static tCopBfr *s_pCopBfrA;                        // copper buffer that uses chain 0
// Art frame whose data rows are in chain slot k, per buffer: the 16 data rows
// are only rewritten when a slot's frame changes (walk pose every 8 frames,
// a different enemy in the slot); otherwise just POS/CTL (enemies move).
#define ENEMY_SLOT_EMPTY 0xFF
static UBYTE s_pChainFrame[2][ENEMY_COUNT];
static tEnemyPlan s_sEnemyPlan;

// --------------------------------------------------------- copper list ---
// Raw list, same layout in both buffers:
//   [0..15]  sprite pointers (ACE sprite manager)
//   [16..]   top of frame (vblank, no WAIT): BPLCON0/2, DDF, modulos, BPLCON1,
//            PF1 pointers, PF2 -> blank row, COLOR00, COLOR01-07 (level)
//   then in beam order: sky WAIT+COLOR00 every 4 lines, mountains palette,
//   band switches (WAIT line-1 at x=0xD8: PF2 pointers, BPLCON1, BPL2MOD),
//   the gap line (PF2 blank, hills palette), haze COLOR00 behind the hills,
//   the line-255 wrap WAIT, PF2 off, pit colour.
// Per frame we rewrite only values: BPLCON1 x3, PF1 pointers, band pointers.

#define COP_SPRITES_POS 0
#define COP_TOP_POS 16
#define COP_RAW_COUNT 560
#define FRAME_START_LINES 32   // see the end of genericProcess()

typedef struct {
	UWORD uwCon1;          // index of the MOVE BPLCON1
	UWORD uwPtr;           // index of the first of 6 pointer MOVEs (hi, lo per plane)
} tCopSlots;

static tCopSlots s_sTopSlots, s_sMtnSlots, s_sHillSlots;
static UWORD s_uwCopUsed;
static UBYTE s_isMusicOn, s_isFallSounded;

static UWORD s_pSkyColors[SKY_BANDS];
static UWORD s_pHazeColors[HAZE_BANDS];
static UWORD s_pPitColors[PIT_BANDS];

typedef struct {
	tCopCmd *pList;
	UWORD uwPos;
	UWORD uwBeamTop;
	UBYTE isPast255;
} tCopWriter;

static void cwWait(tCopWriter *pW, UWORD uwGameY, UBYTE ubX) {
	UWORD uwBeamY = pW->uwBeamTop + uwGameY;
	if(uwBeamY > 0xFF && !pW->isPast255) {
		// WAIT has 8 bits of Y: wait for the end of line 255 first.
		copSetWait(&pW->pList[pW->uwPos++].sWait, 0xDF, 0xFF);
		pW->isPast255 = 1;
	}
	copSetWait(&pW->pList[pW->uwPos++].sWait, ubX, uwBeamY & 0xFF);
}

static void cwMove(tCopWriter *pW, volatile void *pReg, UWORD uwValue) {
	copSetMove(&pW->pList[pW->uwPos++].sMove, pReg, uwValue);
}

// 6 MOVEs: bitplane pointers for one playfield. PF1 = BPL1/3/5 (ubFirst 0),
// PF2 = BPL2/4/6 (ubFirst 1).
static UWORD cwPlanePtrs(tCopWriter *pW, UBYTE ubFirst, const ULONG *pAddr) {
	UWORD uwStart = pW->uwPos;
	for(UBYTE p = 0; p < 3; ++p) {
		cwMove(pW, &g_pBplFetch[ubFirst + 2 * p].uwHi, pAddr[p] >> 16);
		cwMove(pW, &g_pBplFetch[ubFirst + 2 * p].uwLo, pAddr[p] & 0xFFFF);
	}
	return uwStart;
}

static void cwPalette(tCopWriter *pW, UBYTE ubFirstColor, const UWORD *pPal) {
	for(UBYTE i = 1; i < 8; ++i) { // [0] is the transparent slot
		cwMove(pW, &g_pCustom->color[ubFirstColor + i], pPal[i]);
	}
}

// COLOR00 for one line of a gradient; nothing when it doesn't change.
static void cwBackground(tCopWriter *pW, UWORD uwGameY, UWORD uwColor, UWORD uwPrev) {
	if(uwColor != uwPrev) {
		cwWait(pW, uwGameY, 0);
		cwMove(pW, &g_pCustom->color[0], uwColor);
	}
}

// Band switch at the end of line uwGameY - 1, after its last bitplane fetch
// (8 MOVEs: fits in the horizontal blank):
// the new pointers are used from line uwGameY on. Returns the slots.
static tCopSlots cwBand(tCopWriter *pW, UWORD uwGameY, const tBitMap *pBm) {
	tCopSlots sSlots;
	ULONG pAddr[3];
	for(UBYTE p = 0; p < 3; ++p) pAddr[p] = (ULONG)pBm->Planes[p];
	cwWait(pW, uwGameY - 1, BAND_WAIT_X);
	sSlots.uwPtr = cwPlanePtrs(pW, 1, pAddr);
	sSlots.uwCon1 = pW->uwPos;
	cwMove(pW, &g_pCustom->bplcon1, 0);
	// Modulo for the band bitmaps (704 px wide, interleaved: 264 - 42 = 222)
	cwMove(pW, &g_pCustom->bpl2mod, pBm->BytesPerRow - FETCH_BYTES);
	return sSlots;
}

static void copperWriteList(tCopCmd *pList, UWORD uwBeamTop) {
	tCopWriter sW = {.pList = pList, .uwPos = COP_TOP_POS, .uwBeamTop = uwBeamTop, .isPast255 = 0};
	ULONG pBlank[3] = {(ULONG)s_pBlankRow, (ULONG)s_pBlankRow, (ULONG)s_pBlankRow};
	ULONG pLevel[3];
	for(UBYTE p = 0; p < 3; ++p) pLevel[p] = (ULONG)s_pLevel->Planes[p];

	// Top of frame: runs in the vertical blank, before any WAIT.
	cwMove(&sW, &g_pCustom->bplcon0, BPLCON0_DPF);
	cwMove(&sW, &g_pCustom->bplcon2, BPLCON2_DPF);
	cwMove(&sW, &g_pCustom->ddfstrt, DDFSTRT_SCROLL);
	cwMove(&sW, &g_pCustom->ddfstop, DDFSTOP_LORES);
	cwMove(&sW, &g_pCustom->bpl1mod, s_pLevel->BytesPerRow - FETCH_BYTES);
	cwMove(&sW, &g_pCustom->bpl2mod, (UWORD)-FETCH_BYTES); // re-read the blank row
	s_sTopSlots.uwCon1 = sW.uwPos;
	cwMove(&sW, &g_pCustom->bplcon1, 0);
	s_sTopSlots.uwPtr = cwPlanePtrs(&sW, 0, pLevel);
	cwPlanePtrs(&sW, 1, pBlank);
	cwMove(&sW, &g_pCustom->color[0], s_pSkyColors[0]);
	cwPalette(&sW, 0, g_pArtTilesPalette); // colours 1-7: level

	// Sky gradient and bands, in beam order.
	for(UWORD y = SKY_STEP; y < GAP_Y; y += SKY_STEP) {
		cwBackground(&sW, y, s_pSkyColors[y / SKY_STEP], s_pSkyColors[(y - 1) / SKY_STEP]);
		if(y < MOUNTAINS_Y && y + SKY_STEP >= MOUNTAINS_Y) {
			// PF2 is still blank here, so the mountains palette can load any
			// time before the band: keeps the band switch short.
			cwPalette(&sW, 8, g_pArtMountainsPalette);
			s_sMtnSlots = cwBand(&sW, MOUNTAINS_Y, s_pMountains);
		}
	}
	// Mountains -> hills. Pointers + scroll + 7 colours don't all fit in one
	// horizontal blank (the last colours landed ~7 px into the line), so:
	// end of line 134: PF2 -> blank row, COLOR00 -> haze (line 135 is a gap);
	// during line 135: load the hills palette (PF2 is blank, invisible);
	// end of line 135: hills pointers + scroll + modulo.
	cwWait(&sW, GAP_Y - 1, BAND_WAIT_X);
	cwPlanePtrs(&sW, 1, pBlank);
	cwMove(&sW, &g_pCustom->bpl2mod, (UWORD)-FETCH_BYTES);
	cwMove(&sW, &g_pCustom->color[0], s_pHazeColors[0]);
	cwWait(&sW, GAP_Y, 0);
	cwPalette(&sW, 8, g_pArtHillsPalette);
	s_sHillSlots = cwBand(&sW, HILLS_Y, s_pHills);
	for(UBYTE i = 1; i < HAZE_BANDS; ++i) {
		cwBackground(&sW, HILLS_Y + i * HAZE_STEP, s_pHazeColors[i], s_pHazeColors[i - 1]);
	}
	// Below the hills: PF2 back to the blank row.
	cwWait(&sW, BANDS_END - 1, BAND_WAIT_X);
	cwMove(&sW, &g_pCustom->bpl2mod, (UWORD)-FETCH_BYTES);
	cwPlanePtrs(&sW, 1, pBlank);
	cwWait(&sW, BANDS_END, 0);
	cwMove(&sW, &g_pCustom->color[0], s_pPitColors[0]);
	for(UBYTE i = 1; i < PIT_BANDS; ++i) {
		cwBackground(&sW, BANDS_END + i, s_pPitColors[i], s_pPitColors[i - 1]);
	}

	s_uwCopUsed = sW.uwPos;
	if(s_uwCopUsed > COP_RAW_COUNT) {
		agkPrint("AGK ERR copper list overflow: raise COP_RAW_COUNT\n");
	}
	// Spare slots before ACE's final WAIT: a WAIT that never fires this frame
	// (never zeros: that's a MOVE to BLTDDAT and stops the copper).
	while(sW.uwPos < COP_RAW_COUNT) {
		copSetWait(&pList[sW.uwPos++].sWait, 0xDF, 0xFF);
	}
}

static void copperCreate(void) {
	for(UBYTE i = 0; i < SKY_BANDS; ++i) s_pSkyColors[i] = logicSkyColor(i);
	for(UBYTE i = 0; i < HAZE_BANDS; ++i) s_pHazeColors[i] = logicHazeColor(i);
	for(UBYTE i = 0; i < PIT_BANDS; ++i) s_pPitColors[i] = logicPitColor(i);
	tCopList *pCopList = s_pView->pCopList;
	copperWriteList(pCopList->pBackBfr->pList, s_pView->ubPosY);
	copperWriteList(pCopList->pFrontBfr->pList, s_pView->ubPosY);
}

static inline void setPtrs(tCopCmd *pList, UWORD uwIdx, const tBitMap *pBm, WORD wOffs) {
	for(UBYTE p = 0; p < 3; ++p) {
		ULONG ulAddr = (ULONG)pBm->Planes[p] + wOffs;
		copSetMoveVal(&pList[uwIdx++].sMove, ulAddr >> 16);
		copSetMoveVal(&pList[uwIdx++].sMove, ulAddr & 0xFFFF);
	}
}

static UWORD s_uwMtnOffs, s_uwHillOffs;

// Rewrite the scroll values in the back buffer (swapped in by copProcessBlocks).
// All values are recomputed from the state, so no per-buffer bookkeeping.
static void copperUpdate(WORD wCam) {
	tCopCmd *pList = s_pView->pCopList->pBackBfr->pList;
	s_uwMtnOffs = logicBandOffset(wCam, MOUNTAINS_SHIFT, ART_MOUNTAINS_LOOP_W);
	s_uwHillOffs = logicBandOffset(wCam, HILLS_SHIFT, ART_HILLS_LOOP_W);
	UWORD uwPf1Delay = logicScrollDelay(wCam);

	copSetMoveVal(&pList[s_sTopSlots.uwCon1].sMove, uwPf1Delay);
	setPtrs(pList, s_sTopSlots.uwPtr, s_pLevel, logicScrollByteOffset(wCam));

	copSetMoveVal(&pList[s_sMtnSlots.uwCon1].sMove,
		uwPf1Delay | (logicScrollDelay(s_uwMtnOffs) << 4));
	setPtrs(pList, s_sMtnSlots.uwPtr, s_pMountains, logicScrollByteOffset(s_uwMtnOffs));

	copSetMoveVal(&pList[s_sHillSlots.uwCon1].sMove,
		uwPf1Delay | (logicScrollDelay(s_uwHillOffs) << 4));
	setPtrs(pList, s_sHillSlots.uwPtr, s_pHills, logicScrollByteOffset(s_uwHillOffs));
}

// --------------------------------------------------------------- enemies ---

static void enemyChainsCreate(void) {
	for(UBYTE f = 0; f < ART_ENEMY_FRAMES; ++f) {
		for(UBYTE p = 0; p < ART_ENEMY_PARTS; ++p) {
			// artEnemyCreate() gives a ready single-sprite bitmap: row 0 is the
			// POS/CTL header, rows 1..16 are (DATA, DATB) words. We only copy
			// the data rows into the chains.
			s_pEnemyFrames[f][p] = artEnemyCreate(f, p);
			s_pEnemyData[f][p] = (const UWORD *)s_pEnemyFrames[f][p]->Planes[0] + 2;
		}
	}
	tCopList *pCopList = s_pView->pCopList;
	s_pCopBfrA = pCopList->pBackBfr;
	for(UBYTE b = 0; b < 2; ++b) {
		for(UBYTE k = 0; k < ENEMY_COUNT; ++k) s_pChainFrame[b][k] = ENEMY_SLOT_EMPTY;
	}
	for(UBYTE b = 0; b < 2; ++b) {
		tCopCmd *pList = (b == 0 ? pCopList->pBackBfr : pCopList->pFrontBfr)->pList;
		for(UBYTE p = 0; p < ART_ENEMY_PARTS; ++p) {
			// Cleared: starts as an empty chain (POS=CTL=0 ends the channel).
			s_pEnemyChain[b][p] = memAllocChipClear(ENEMY_CHAIN_WORDS * sizeof(UWORD));
			ULONG ulAddr = (ULONG)s_pEnemyChain[b][p];
			// The sprite manager's raw slots: 2 MOVEs (SPRxPTH, SPRxPTL) per channel.
			UWORD uwSlot = COP_SPRITES_POS + 2 * (ENEMY_CHANNEL + p);
			copSetMoveVal(&pList[uwSlot].sMove, ulAddr >> 16);
			copSetMoveVal(&pList[uwSlot + 1].sMove, ulAddr & 0xFFFF);
		}
	}
}

static void enemyChainsDestroy(void) {
	for(UBYTE b = 0; b < 2; ++b) {
		for(UBYTE p = 0; p < ART_ENEMY_PARTS; ++p) {
			memFree(s_pEnemyChain[b][p], ENEMY_CHAIN_WORDS * sizeof(UWORD));
		}
	}
	for(UBYTE f = 0; f < ART_ENEMY_FRAMES; ++f) {
		for(UBYTE p = 0; p < ART_ENEMY_PARTS; ++p) {
			bitmapDestroy(s_pEnemyFrames[f][p]);
		}
	}
}

// Fill the back buffer's chains from the plan (sorted by y, non-overlapping).
// Slot k of a chain is always at word k * ENEMY_SPRITE_WORDS.
static void enemyChainsBuild(void) {
	UBYTE ubBfr = s_pView->pCopList->pBackBfr == s_pCopBfrA ? 0 : 1;
	UWORD *pEven = s_pEnemyChain[ubBfr][0];
	UWORD *pOdd = s_pEnemyChain[ubBfr][1];
	UBYTE *pSlotFrame = s_pChainFrame[ubBfr];
	logicEnemyPlan(&s_sState, &s_sEnemyPlan);
	for(UBYTE i = 0; i < s_sEnemyPlan.count; ++i) {
		const tEnemy *pE = &s_sState.pEnemies[s_sEnemyPlan.pId[i]];
		UBYTE ubFrame = logicEnemyFrame(pE);
		// Same coordinate mapping as ACE's spriteProcess(): beam = view origin + game px.
		UWORD uwVStart = s_pView->ubPosY + pE->y + ENEMY_SINK; // stand in the top face
		UWORD uwVStop = uwVStart + ART_ENEMY_H;
		UWORD uwHStart = s_pView->ubPosX - 1 + (pE->x - s_sState.cam);
		UWORD uwPos = (uwVStart << 8) | ((uwHStart >> 1) & 0xFF);
		UWORD uwCtl = (uwVStop << 8) |
			((uwVStart >> 8) & 1) << 2 |  // VSTART bit 8
			((uwVStop >> 8) & 1) << 1 |   // VSTOP bit 8
			(uwHStart & 1);               // HSTART bit 0
		pEven[0] = uwPos;
		pEven[1] = uwCtl;
		pOdd[0] = uwPos;
		pOdd[1] = uwCtl | SPRxCTL_ATTACH;  // odd channel: attach to channel 4
		if(pSlotFrame[i] != ubFrame) {
			pSlotFrame[i] = ubFrame;
			const ULONG *pSrcEven = (const ULONG *)s_pEnemyData[ubFrame][0];
			const ULONG *pSrcOdd = (const ULONG *)s_pEnemyData[ubFrame][1];
			ULONG *pDstEven = (ULONG *)&pEven[2], *pDstOdd = (ULONG *)&pOdd[2];
			for(UBYTE r = ART_ENEMY_H; r--;) { // one (DATA, DATB) longword per row
				*pDstEven++ = *pSrcEven++;
				*pDstOdd++ = *pSrcOdd++;
			}
		}
		pEven += ENEMY_SPRITE_WORDS;
		pOdd += ENEMY_SPRITE_WORDS;
	}
	// End of chain: POS = CTL = 0 (the channel stays off for the rest of the
	// frame). This overwrites the next slot's control words, not its data rows,
	// so that slot's cached frame stays valid.
	pEven[0] = 0; pEven[1] = 0;
	pOdd[0] = 0; pOdd[1] = 0;
}

// ----------------------------------------------------------------- level ---

static void drawLevel(void) {
	// Once, at startup: one blit per tile, all 3 planes (both bitmaps are
	// interleaved with the same depth).
	for(UBYTE ty = 0; ty < LEVEL_TILES_H; ++ty) {
		for(UBYTE tx = 0; tx < LEVEL_TILES_W; ++tx) {
			UBYTE ubTile = logicTileAt(tx, ty);
			if(ubTile != TILE_EMPTY) {
				blitCopyAligned(
					s_pTiles, 0, logicTileArtFrame(tx, ty) * TILE_SIZE,
					s_pLevel, tx * TILE_SIZE, ty * TILE_SIZE, TILE_SIZE, TILE_SIZE
				);
			}
		}
	}
	blitWait();
}

static void readInput(tInput *pInput) {
	// Joystick in port 2, the game port. In ACE that is JOY1 (JOY2 is port 1,
	// the mouse port: reading it turns mouse movement into phantom input).
	pInput->dx = 0;
	if(joyCheck(JOY1 + JOY_LEFT)) pInput->dx = -1;
	if(joyCheck(JOY1 + JOY_RIGHT)) pInput->dx = 1;
	pInput->jump = joyCheck(JOY1 + JOY_FIRE) || joyCheck(JOY1 + JOY_UP);
}

void genericCreate(void) {
	agkDebugInit();
	agkPrint("AGK boot sidescroller\n");

	keyCreate();
	joyOpen();

	s_pView = viewCreate(0,
		TAG_VIEW_GLOBAL_PALETTE, 1,
		TAG_VIEW_COPLIST_MODE, VIEW_COPLIST_MODE_RAW,
		TAG_VIEW_COPLIST_RAW_COUNT, COP_RAW_COUNT,
		TAG_DONE);
	// 6 bpp so viewLoad() sets up a 6-plane display; the copper list then
	// overrides BPLCON0 with DBLPF every frame. No buffer manager: the copper
	// list below owns all bitplane registers.
	s_pVPort = vPortCreate(0, TAG_VPORT_VIEW, s_pView, TAG_VPORT_BPP, 6, TAG_DONE);

	s_pLevel = bitmapCreate(LEVEL_W, LEVEL_H, LEVEL_BPP, BMF_CLEAR | BMF_INTERLEAVED);
	s_pTiles = artTilesCreate();
	s_pMountains = artMountainsCreate();
	s_pHills = artHillsCreate();
	s_pBlankRow = memAllocChipClear(FETCH_BYTES);

	// Loaded once by viewLoad(); the copper list re-sets 0-15 every frame.
	UWORD *pPal = s_pVPort->pPalette;
	for(UBYTE i = 1; i < 8; ++i) {
		pPal[i] = g_pArtTilesPalette[i];
		pPal[8 + i] = g_pArtMountainsPalette[i];
	}
	pPal[0] = logicSkyColor(0);
	artHeroApplyColors(pPal);    // art/hero.txt -> attached-sprite colours 17-31

	logicInit(&s_sState);
	drawLevel();

	for(UBYTE f = 0; f < ART_HERO_FRAMES; ++f) {
		for(UBYTE p = 0; p < ART_HERO_PARTS; ++p) {
			s_pHeroFrames[f][p] = artHeroCreate(f, p);
		}
	}
	spriteManagerCreate(s_pView, COP_SPRITES_POS, 0);
	systemSetDmaBit(DMAB_SPRITE, 1);
	for(UBYTE p = 0; p < ART_HERO_PARTS; ++p) {
		s_pHero[p] = spriteAdd(ART_HERO_CHANNEL + p, s_pHeroFrames[0][p]);
		if(p & 1) {
			spriteSetAttached(s_pHero[p], 1);  // odd channel adds colour bits 2-3
		}
	}
	s_ubShownHeroFrame = 0;
	enemyChainsCreate(); // after spriteManagerCreate(): it blanks all 8 channels

	copperCreate();
	copperUpdate(s_sState.cam);
	enemyChainsBuild();
	copProcessBlocks();       // both buffers get the initial scroll values and chains
	copperUpdate(s_sState.cam);
	enemyChainsBuild();

	soundCreate();   // ptplayer + samples in chip RAM (sound/sound.toml)
	soundMusicStart(SOUND_MUSIC_THEME);
	s_isMusicOn = 1;

	viewLoad(s_pView);
	systemUnuse();
	agkDebugAsync(1); // serial channel only: interrupt-driven from here on
}

// Sound effects for what just happened in the game logic.
static void playSounds(UBYTE ubJumpsBefore, UBYTE ubDeathsBefore) {
	if(s_sState.eventStomp) {
		soundPlay(SOUND_SFX_STOMP);
	}
	else if(s_sState.jumps != ubJumpsBefore) {
		soundPlay(SOUND_SFX_JUMP);
	}
	if(s_sState.eventHit) {
		soundPlay(SOUND_SFX_HIT);
	}
	// Falling into a pit: whistle once as the feet drop below the ground line
	// (the death itself only happens off the bottom of the screen).
	if(!s_sState.onGround && s_sState.y > START_Y + 4 && !s_isFallSounded) {
		soundPlay(SOUND_SFX_FALL);
		s_isFallSounded = 1;
	}
	if(s_sState.deaths != ubDeathsBefore || s_sState.onGround) {
		s_isFallSounded = 0;
	}
}

void genericProcess(void) {
	agkPerfBegin(); // frame budget meter: prints "AGK perf ... dropped= load= maxload="
	keyProcess();
	joyProcess();
	if(keyCheck(KEY_ESCAPE)) {
		gameExit();
		return;
	}

	if(keyUse(KEY_M)) { // music on/off
		s_isMusicOn = !s_isMusicOn;
		if(s_isMusicOn) soundMusicStart(SOUND_MUSIC_THEME);
		else soundMusicStop();
	}

	tInput sInput;
	readInput(&sInput);
	UBYTE ubJumpsBefore = s_sState.jumps, ubDeathsBefore = s_sState.deaths;
	UBYTE isChanged = logicUpdate(&s_sState, &sInput);
	playSounds(ubJumpsBefore, ubDeathsBefore);

	UBYTE ubHeroFrame = logicHeroFrame(&s_sState) + (s_sState.facingLeft ? ART_HERO_MIRROR : 0);
	for(UBYTE p = 0; p < ART_HERO_PARTS; ++p) {
		tSprite *pSpr = s_pHero[p];
		if(ubHeroFrame != s_ubShownHeroFrame) {
			spriteSetBitmap(pSpr, s_pHeroFrames[ubHeroFrame][p]);
		}
		pSpr->wX = s_sState.x - s_sState.cam - HERO_X_OFS + (p >> 1) * 16;
		pSpr->wY = s_sState.y - HERO_Y_OFS;
		spriteRequestMetadataUpdate(pSpr);
		spriteProcess(pSpr);
		spriteProcessChannel(ART_HERO_CHANNEL + p);
	}
	s_ubShownHeroFrame = ubHeroFrame;

	UBYTE ubShownBefore = s_sEnemyPlan.count, ubSkippedBefore = s_sEnemyPlan.skipped;
	enemyChainsBuild();

	copperUpdate(s_sState.cam);

	// Enemy events for tests: one line each, before the state line
	// ("AGK stomp id=1 x=446 y=192", "AGK hit id=1"): agkState() with a key
	// that has a space in it builds the line in one host transfer.
	if(s_sState.eventStomp) {
		for(UBYTE i = 0; i < ENEMY_COUNT; ++i) {
			if(s_sState.eventStomp & (1 << i)) {
				agkState("stomp id", i);
				agkState("x", s_sState.pEnemies[i].x);
				agkState("y", s_sState.pEnemies[i].y);
				agkEnd();
			}
		}
	}
	if(s_sState.eventHit) {
		agkState("hit id", s_sState.eventHit - 1);
		agkEnd();
	}
	UBYTE isMuxChanged = s_sEnemyPlan.count != ubShownBefore || s_sEnemyPlan.skipped != ubSkippedBefore;

	// Report state for tests: on every change, plus a heartbeat.
	if(isChanged || isMuxChanged || s_sState.frame % 50 == 0 || s_sState.frame <= 3) {
		agkState("frame", s_sState.frame);
		agkState("x", s_sState.x);
		agkState("y", s_sState.y);
		agkState("cam", s_sState.cam);
		agkState("mtn", s_uwMtnOffs);
		agkState("hills", s_uwHillOffs);
		agkState("ground", s_sState.onGround);
		agkState("vy", s_sState.vy);
		agkState("walk", s_sState.walkFrame);
		agkState("anim", logicHeroFrame(&s_sState));
		agkState("left", s_sState.facingLeft);
		agkState("jumps", s_sState.jumps);
		agkState("deaths", s_sState.deaths);
		agkState("stomps", s_sState.stomps);
		if(isMuxChanged || s_sState.frame % 50 == 0 || s_sState.frame <= 3) {
			// Multiplexer, only when it changes (each field costs ~1.4% of a frame)
			agkState("spr", s_sEnemyPlan.count);    // enemies in this frame's sprite chain
			agkState("skip", s_sEnemyPlan.skipped); // visible but left out (overlap)
		}
		agkEnd();
	}

	// The new copper list (and its sprite chains) takes over at the next
	// vertical blank. We start the frame before the blank (see below), so a
	// fast machine could get here before it and show this frame one frame
	// earlier than a slow one: always swap after this frame's blank.
	agkPerfEnd(); // (the wait below is idle time, not frame work)
	while(getRayPos().bfPosY >= s_pView->ubPosY + SCREEN_H - FRAME_START_LINES) continue;
	copProcessBlocks(); // raw mode: swap copper buffers
	// Start the next frame FRAME_START_LINES lines before the display ends,
	// not at its very end: the music player's timer interrupt can take ~30
	// lines, and if it lands just before the vertical blank our frame would
	// start after it - one frame late. Those last lines show only dirt: no
	// sprite (hero, enemies) is ever drawn there while we update them.
	vPortWaitForPos(s_pVPort, SCREEN_H - FRAME_START_LINES, 1);

	// Copper lists are double-buffered: after two frames our first frame is
	// on screen. Only then tell the harness we're ready.
	if(s_sState.frame == 2) {
		agkState("copper_used", s_uwCopUsed);
		agkState("copper_max", COP_RAW_COUNT);
		agkEnd();
		agkReady();
	}
	// Test sync point. The harness starts a scenario 4-5 game frames after
	// "AGK ready" depending on the machine profile (frame 6 on AROS, 7 on
	// Kickstart), but the enemies walk from frame 0: a 1-frame difference
	// moves them 0.5 px and the goldens differ per profile. Scenarios start
	// with `wait-serial "AGK t0"`, which resumes at the start of frame
	// SYNC_FRAME + 1 on every profile.
	if(s_sState.frame == SYNC_FRAME) {
		agkPrint("AGK t0\n");
	}
}

void genericDestroy(void) {
	agkDebugAsync(0); // must be off before the OS takes interrupts back
	systemUse();
	soundDestroy();
	systemSetDmaBit(DMAB_SPRITE, 0);
	spriteManagerDestroy();
	enemyChainsDestroy();
	for(UBYTE f = 0; f < ART_HERO_FRAMES; ++f) {
		for(UBYTE p = 0; p < ART_HERO_PARTS; ++p) {
			bitmapDestroy(s_pHeroFrames[f][p]);
		}
	}
	memFree(s_pBlankRow, FETCH_BYTES);
	bitmapDestroy(s_pHills);
	bitmapDestroy(s_pMountains);
	bitmapDestroy(s_pTiles);
	bitmapDestroy(s_pLevel);
	viewDestroy(s_pView);
	joyClose();
	keyDestroy();
}
