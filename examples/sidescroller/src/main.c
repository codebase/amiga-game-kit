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
#include "logic.h"

// ------------------------------------------------------ display constants ---

#define BPLCON0_DPF 0x6600   // 6 planes (BPU=6) | DBLPF (bit 10) | COLOR (bit 9)
#define BPLCON2_DPF 0x0024   // PF2PRI=0 (PF1 in front), PF1P=PF2P=4: sprites in front of both
#define DDFSTRT_SCROLL 0x30  // one fetch word earlier than 0x38 for fine scroll (costs sprite 7)
#define DDFSTOP_LORES 0xD0
#define FETCH_BYTES 42       // 21 words per line: 320 px + the extra scroll word
#define BAND_WAIT_X 0xD8     // after the last fetch of the line (DDFSTOP 0xD0 + 8)

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
#define HERO_Y_OFS 15   // feet (row 30 of the art) on the hitbox bottom
static tBitMap *s_pHeroFrames[ART_HERO_FRAMES][ART_HERO_PARTS];
static tSprite *s_pHero[ART_HERO_PARTS];
static UBYTE s_ubShownHeroFrame;
static tGameState s_sState;

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
#define COP_RAW_COUNT 200

typedef struct {
	UWORD uwCon1;          // index of the MOVE BPLCON1
	UWORD uwPtr;           // index of the first of 6 pointer MOVEs (hi, lo per plane)
} tCopSlots;

static tCopSlots s_sTopSlots, s_sMtnSlots, s_sHillSlots;
static UWORD s_uwCopUsed;

static UWORD s_pSkyColors[SKY_BANDS];
static UWORD s_pHazeColors[HAZE_BANDS];

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

static void cwSky(tCopWriter *pW, UWORD uwGameY) {
	cwWait(pW, uwGameY, 0);
	cwMove(pW, &g_pCustom->color[0], s_pSkyColors[uwGameY / SKY_STEP]);
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
		cwSky(&sW, y);
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
		cwWait(&sW, HILLS_Y + i * HAZE_STEP, 0);
		cwMove(&sW, &g_pCustom->color[0], s_pHazeColors[i]);
	}
	// Below the hills: PF2 back to the blank row.
	cwWait(&sW, BANDS_END - 1, BAND_WAIT_X);
	cwMove(&sW, &g_pCustom->bpl2mod, (UWORD)-FETCH_BYTES);
	cwPlanePtrs(&sW, 1, pBlank);
	cwWait(&sW, BANDS_END, 0);
	cwMove(&sW, &g_pCustom->color[0], PIT_COLOR);

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

// ----------------------------------------------------------------- level ---

static void drawLevel(void) {
	// Once, at startup: one blit per tile, all 3 planes (both bitmaps are
	// interleaved with the same depth).
	for(UBYTE ty = 0; ty < LEVEL_TILES_H; ++ty) {
		for(UBYTE tx = 0; tx < LEVEL_TILES_W; ++tx) {
			UBYTE ubTile = logicTileAt(tx, ty);
			if(ubTile != TILE_EMPTY) {
				blitCopyAligned(
					s_pTiles, 0, (ubTile - 1) * TILE_SIZE,
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

	copperCreate();
	copperUpdate(s_sState.cam);
	copProcessBlocks();       // both buffers get the initial scroll values
	copperUpdate(s_sState.cam);

	viewLoad(s_pView);
	systemUnuse();
	agkDebugAsync(1); // serial channel only: interrupt-driven from here on
}

void genericProcess(void) {
	agkPerfBegin(); // frame budget meter: prints "AGK perf ... dropped= load= maxload="
	keyProcess();
	joyProcess();
	if(keyCheck(KEY_ESCAPE)) {
		gameExit();
		return;
	}

	tInput sInput;
	readInput(&sInput);
	UBYTE isChanged = logicUpdate(&s_sState, &sInput);

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

	copperUpdate(s_sState.cam);

	// Report state for tests: on every change, plus a heartbeat.
	if(isChanged || s_sState.frame % 50 == 0 || s_sState.frame <= 3) {
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
		agkEnd();
	}

	copProcessBlocks(); // raw mode: swap copper buffers
	agkPerfEnd();
	vPortWaitForEnd(s_pVPort);

	// Copper lists are double-buffered: after two frames our first frame is
	// on screen. Only then tell the harness we're ready.
	if(s_sState.frame == 2) {
		agkState("copper_used", s_uwCopUsed);
		agkState("copper_max", COP_RAW_COUNT);
		agkEnd();
		agkReady();
	}
}

void genericDestroy(void) {
	agkDebugAsync(0); // must be off before the OS takes interrupts back
	systemUse();
	systemSetDmaBit(DMAB_SPRITE, 0);
	spriteManagerDestroy();
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
