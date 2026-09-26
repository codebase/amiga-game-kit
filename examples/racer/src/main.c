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
//   PF1 (odd planes 1/3/5, colours 1-7) = in front: blank for now (later
//        roadside objects and the dashboard).
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
#include <agk/debug.h>
#include <agk/perf.h>
#include "art.h"      // generated from art/ by agk (agk help-art)
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
#define ROAD_BMP_ROWS (2 * ROAD_STORED + 1)
#define ROAD_EMPTY_OFFS (2 * ROAD_STORED * ROAD_BYTES_PER_ROW)   // 20480: < 32K, so
                                    // every jump between rows fits the 16-bit modulo

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
static UBYTE *s_pBlankRow;        // PF1: FETCH_BYTES of zeros (modulo -FETCH_BYTES)
static tGameState s_sState;
static tRoadLine s_pLines[REGION_LINES];
static UWORD s_pBlockMove[BLOCK_COUNT];   // copper index of each line block's BPLCON1 MOVE
static UWORD s_pRowOffs[2][ROAD_ROWS];    // [dark][row]: byte offset of the row in a plane
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

static void copperWriteList(tCopCmd *pList, UWORD uwBeamTop) {
	tCopWriter sW = {.pList = pList, .uwPos = COP_TOP_POS, .uwBeamTop = uwBeamTop};

	// Top of frame (vertical blank, no WAIT)
	cwMove(&sW, &g_pCustom->bplcon0, BPLCON0_DPF6);
	cwMove(&sW, &g_pCustom->bplcon2, BPLCON2_DPF);
	cwMove(&sW, &g_pCustom->ddfstrt, DDFSTRT_SCROLL);
	cwMove(&sW, &g_pCustom->ddfstop, DDFSTOP_LORES);
	cwMove(&sW, &g_pCustom->bpl1mod, (UWORD)-FETCH_BYTES);   // PF1: re-read the blank row
	cwMove(&sW, &g_pCustom->bpl2mod, (UWORD)-FETCH_BYTES);   // PF2: re-read the empty row
	cwMove(&sW, &g_pCustom->bplcon1, 0);
	for(UBYTE p = 0; p < 3; ++p) {
		cwPtrs(&sW, 2 * p, (ULONG)s_pBlankRow);                            // PF1
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

// Rewrite the back buffer's per-line values from this frame's road layout.
static void copperUpdate(void) {
	logicRoadLines(&s_sState, s_pLines);
	// The value word of copper instruction i is word 2*i+1 of the list
	UWORD *pVal = (UWORD *)s_pView->pCopList->pBackBfr->pList;
	WORD wPrev = ROAD_EMPTY_OFFS;   // line REGION_TOP-1 always shows the sky
	UWORD *pModPrev = &pVal[2 * (s_pBlockMove[0] + 1) + 1];   // block REGION_TOP-1's BPL2MOD
	for(UWORD l = 0; l < REGION_LINES; ++l) {
		const tRoadLine *pL = &s_pLines[l];
		UWORD *pBlock = &pVal[2 * s_pBlockMove[l + 1] + 1];     // block of line REGION_TOP + l
		WORD wOffs;
		if(pL->row == ROW_SKY) {
			wOffs = ROAD_EMPTY_OFFS;
			pBlock[0] = 0;
		}
		else {
			wOffs = s_pRowOffs[pL->dark][pL->row] + scrollByteOffset(pL->left);
			pBlock[0] = scrollDelay(pL->left) << 4;   // PF2 delay: bits 4-7
		}
		*pModPrev = (UWORD)(wOffs - wPrev - FETCH_BYTES);
		pModPrev = &pBlock[2];
		wPrev = wOffs;
	}
}

// ---------------------------------------------------------------- road ---

static void roadDraw(void) {
	// Once, at startup: each row twice (light, dark), 16 px at a time.
	for(UBYTE ubDark = 0; ubDark < 2; ++ubDark) {
		for(UBYTE r = 0; r < ROAD_ROWS; ++r) {
			UWORD uwRow = ubDark * ROAD_STORED + r / ROW_STEP;
			s_pRowOffs[ubDark][r] = uwRow * ROAD_BYTES_PER_ROW;
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
	copperWriteList(pCopList->pBackBfr->pList, s_pView->ubPosY);
	copperWriteList(pCopList->pFrontBfr->pList, s_pView->ubPosY);
	copperUpdate();
	copProcessBlocks();
	copperUpdate();

	viewLoad(s_pView);
	systemUnuse();
	agkPerfSetFrameVbls(FRAME_VBLS);
	roadDraw();   // with the OS off (see README: drawn before, it came out blank)
	agkDebugAsync(1); // serial channel only: interrupt-driven from here on
}

static ULONG s_ulFrameVbl;   // vertical blank count when this picture started

static void carUpdate(BYTE bSteer) {
	UBYTE ubFrame = bSteer < 0 ? CAR_FRAME_LEFT : bSteer > 0 ? CAR_FRAME_RIGHT : 0;
	// On the grass the car shakes (1 px, every other picture)
	WORD wY = CAR_Y + ((s_sState.offroad && s_sState.speed && (s_sState.frame & 2)) ? 1 : 0);
	for(UBYTE p = 0; p < ART_CAR_PARTS; ++p) {
		tSprite *pSpr = s_pCar[p];
		if(ubFrame != s_ubCarFrame) {
			spriteSetBitmap(pSpr, s_pCarFrames[ubFrame][p]);
		}
		pSpr->wX = CAR_X + (p >> 1) * 16;
		pSpr->wY = wY;
		spriteRequestMetadataUpdate(pSpr);
		spriteProcess(pSpr);
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

	// State for tests: every change, plus a heartbeat
	if(isChanged || s_sState.frame % 50 == 0 || s_sState.frame <= 3) {
		agkState("frame", s_sState.frame);
		agkState("pos", (LONG)s_sState.pos);
		agkState("kmh", logicKmh(&s_sState));
		agkState("x", s_sState.x);
		agkState("off", s_sState.offroad);
		agkState("laps", s_sState.laps);
		agkEnd();
	}

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
	systemUse();
	systemSetDmaBit(DMAB_SPRITE, 0);
	spriteManagerDestroy();
	for(UBYTE f = 0; f < ART_CAR_FRAMES; ++f) {
		for(UBYTE p = 0; p < ART_CAR_PARTS; ++p) {
			bitmapDestroy(s_pCarFrames[f][p]);
		}
	}
	memFree(s_pBlankRow, FETCH_BYTES);
	bitmapDestroy(s_pRoad);
	viewDestroy(s_pView);
	joyClose();
	keyDestroy();
}
