// copper - Amiga side: display, input, and hooking the rules in logic.c up
// to the hardware. Game rules belong in logic.c (host-testable), not here.
//
// The technique: the copper (the display co-processor) rewrites colour
// registers at chosen scanlines, so a 4-colour screen shows:
//   - a HUD band (lines 0..23) with its own palette,
//   - a palette split at line 24: COLOR01/02 change from HUD gold to wall blue,
//   - a sky gradient: COLOR00 changes every 4 lines (58 colours),
//   - an animated raster bar: 16 shaded lines of COLOR00 that move every frame.
// The copper list has one WAIT+MOVE(COLOR00) slot per playfield line; each
// frame only the slots the bar left or entered get a new value.
//
// COPPER_RAW selects how the list is built (same picture either way):
//   1 = raw mode: we write copper instructions straight into ACE's two
//       chip-RAM copper buffers. Cheapest per frame; we manage offsets,
//       the WAIT wrap past line 255 and the double buffering ourselves.
//   0 = block mode: one ACE copper block per line; ACE sorts and merges the
//       blocks into the list. Simple, but costs far more per frame (see
//       TECHNIQUE.md for measurements).

#ifndef COPPER_RAW
#define COPPER_RAW 1
#endif

#include <ace/generic/main.h>
#include <ace/managers/blit.h>
#include <ace/managers/copper.h>
#include <ace/managers/joy.h>
#include <ace/managers/key.h>
#include <ace/managers/sprite.h>
#include <ace/managers/viewport/simplebuffer.h>
#include <ace/utils/custom.h>
#include <ace/utils/extview.h>
#include <hardware/dmabits.h>
#include <agk/debug.h>
#include <agk/perf.h>
#include "logic.h"

// Palette indices. Sprite channels 0/1 use colours 17-19.
#define COLOR_BG 0
#define COLOR_WALL 1
#define COLOR_WALL_EDGE 2
#define BPP 2

static tView *s_pView;
static tVPort *s_pVPort;
static tSimpleBufferManager *s_pBuffer;
static tBitMap *s_pPlayerBm;
static tSprite *s_pPlayer;
static tGameState s_sState;

static uint16_t s_pSkyBands[SKY_BANDS]; // gradient, from logic.c
static UWORD s_uwBeamTop;                // beam line of game y=0 (0x2C on PAL)
// Per-frame code reads these tables instead of calling logic.c for each line:
// a call per line costs ~300 cycles on a 68000, a table read ~20.
static UWORD s_pSkyLine[SKY_LINES];      // sky colour of each playfield line
static UWORD s_pBarColor[BAR_H];         // bar shading

static void buildColorTables(void) {
	logicBuildSkyBands(s_pSkyBands);
	for(UWORD i = 0; i < SKY_LINES; ++i) {
		s_pSkyLine[i] = logicLineColor(s_pSkyBands, SKY_TOP + i, -BAR_H); // no bar
	}
	for(UBYTE i = 0; i < BAR_H; ++i) {
		s_pBarColor[i] = logicBarColor(i);
	}
}

// Point the bar at wNew (playfield y) in a table of MOVE value words, one per
// sky line: first put the sky back where the bar was (wOld), then draw the
// bar. This is logicLineColor() done incrementally: 32 word writes a frame.
static void paintBar(UWORD **pVal, WORD wOld, WORD wNew) {
	for(WORD y = wOld; y < wOld + BAR_H; ++y) {
		if(y >= SKY_TOP && y < WORLD_H) {
			*pVal[y - SKY_TOP] = s_pSkyLine[y - SKY_TOP];
		}
	}
	for(UBYTE i = 0; i < BAR_H; ++i) {
		WORD y = wNew + i;
		if(y >= SKY_TOP && y < WORLD_H) {
			*pVal[y - SKY_TOP] = s_pBarColor[i];
		}
	}
}

// ------------------------------------------------------------ copper list ---

#if COPPER_RAW
// Raw list layout (instruction indices):
//   [0..15]   sprite pointers (ACE sprite manager, 2 MOVEs x 8 channels)
//   [16..18]  HUD palette: MOVE COLOR00/01/02 (still in vblank, no WAIT)
//   [19..28]  simple buffer: WAIT + display setup + bitplane pointers
//   [29..]    one slot per playfield line: WAIT line, MOVE COLOR00; the first
//             slot also sets COLOR01/02 (palette split); before the first
//             line past beam line 255 comes an extra WAIT(0xDF, 0xFF).
#define COP_SPRITES_POS 0
#define COP_HUD_POS (COP_SPRITES_POS + 16)
#define COP_SIMPLEBUFFER_POS (COP_HUD_POS + 3)
#define COP_LINES_POS (COP_SIMPLEBUFFER_POS + 10) // = GetRaw...Count(BPP)
#define COP_RAW_COUNT (COP_LINES_POS + SKY_LINES * 2 + 2 + 1)

static UWORD s_pLineCmd[SKY_LINES];  // index of each line's MOVE COLOR00
static UWORD *s_pLineVal[2][SKY_LINES]; // its value word, in buffer A and B
static tCopBfr *s_pCopBfrA;          // to tell the two copper buffers apart
static WORD s_pBarInBfr[2];          // bar top currently written in each buffer

// Write our part of the list (HUD palette + per-line slots) into one buffer.
static void rawWriteList(tCopCmd *pList) {
	copSetMove(&pList[COP_HUD_POS + 0].sMove, &g_pCustom->color[0], HUD_COLOR0);
	copSetMove(&pList[COP_HUD_POS + 1].sMove, &g_pCustom->color[1], HUD_COLOR1);
	copSetMove(&pList[COP_HUD_POS + 2].sMove, &g_pCustom->color[2], HUD_COLOR2);
	UWORD uwPos = COP_LINES_POS;
	UBYTE isPast255 = 0;
	for(UWORD i = 0; i < SKY_LINES; ++i) {
		UWORD uwBeamY = s_uwBeamTop + SKY_TOP + i;
		if(uwBeamY > 0xFF && !isPast255) {
			// WAIT has only 8 bits of Y: wait for the end of line 255 first,
			// then the low byte of the line number counts from 0 again.
			copSetWait(&pList[uwPos++].sWait, 0xDF, 0xFF);
			isPast255 = 1;
		}
		copSetWait(&pList[uwPos++].sWait, 0, uwBeamY & 0xFF);
		s_pLineCmd[i] = uwPos;
		copSetMove(&pList[uwPos++].sMove, &g_pCustom->color[0], s_pSkyLine[i]);
		if(i == 0) { // palette split: same registers, playfield colours
			copSetMove(&pList[uwPos++].sMove, &g_pCustom->color[1], PF_COLOR1);
			copSetMove(&pList[uwPos++].sMove, &g_pCustom->color[2], PF_COLOR2);
		}
	}
	// Any spare slots before ACE's final WAIT(0xFF,0xFF): a harmless WAIT.
	while(uwPos < COP_RAW_COUNT) {
		copSetWait(&pList[uwPos++].sWait, 0xDF, 0xFF);
	}
}

static void copperCreate(void) {
	tCopBfr *pBfrs[2] = {s_pView->pCopList->pBackBfr, s_pView->pCopList->pFrontBfr};
	s_pCopBfrA = pBfrs[0];
	for(UBYTE b = 0; b < 2; ++b) {
		rawWriteList(pBfrs[b]->pList);
		for(UWORD i = 0; i < SKY_LINES; ++i) {
			// A MOVE is two words: register, value. Point at the value word.
			s_pLineVal[b][i] = (UWORD *)&pBfrs[b]->pList[s_pLineCmd[i]] + 1;
		}
		s_pBarInBfr[b] = -BAR_H; // no bar drawn yet
	}
}

static void copperUpdate(WORD wBarY) {
	// The copper list is double-buffered: we edit the back buffer, which
	// still holds what we wrote two frames ago, not last frame. So remember
	// the bar position per buffer.
	UBYTE ubBfr = (s_pView->pCopList->pBackBfr == s_pCopBfrA) ? 0 : 1;
	if(s_pBarInBfr[ubBfr] != wBarY) {
		paintBar(s_pLineVal[ubBfr], s_pBarInBfr[ubBfr], wBarY);
		s_pBarInBfr[ubBfr] = wBarY;
	}
}

#else // block mode

static tCopBlock *s_pHudBlock;
static tCopBlock *s_pLineBlocks[SKY_LINES];
static UWORD *s_pLineVal[SKY_LINES]; // value word of each block's MOVE COLOR00
static WORD s_wBarInBlocks;

static void copperCreate(void) {
	tCopList *pList = s_pView->pCopList;
	// HUD palette at the top of the frame (vblank). Needed every frame: the
	// copper changed these registers further down last frame, and ACE only
	// loads the palette once, in viewLoad().
	s_pHudBlock = copBlockCreate(pList, 3, 0, 0);
	copMove(pList, s_pHudBlock, &g_pCustom->color[0], HUD_COLOR0);
	copMove(pList, s_pHudBlock, &g_pCustom->color[1], HUD_COLOR1);
	copMove(pList, s_pHudBlock, &g_pCustom->color[2], HUD_COLOR2);
	for(UWORD i = 0; i < SKY_LINES; ++i) {
		// Block wait Y is a beam line; ACE adds the WAIT past line 255 itself.
		// (Stock ACE put a block at exactly line 255 one line late; AGK's ACE
		// patch fixes that - see TECHNIQUE.md.)
		UWORD uwBeamY = s_uwBeamTop + SKY_TOP + i;
		tCopBlock *pBlock = copBlockCreate(pList, i == 0 ? 3 : 1, 0, uwBeamY);
		copMove(pList, pBlock, &g_pCustom->color[0], s_pSkyLine[i]); // pCmds[0]
		if(i == 0) {
			copMove(pList, pBlock, &g_pCustom->color[1], PF_COLOR1);
			copMove(pList, pBlock, &g_pCustom->color[2], PF_COLOR2);
		}
		s_pLineBlocks[i] = pBlock;
		s_pLineVal[i] = (UWORD *)&pBlock->pCmds[0] + 1;
	}
	s_wBarInBlocks = -BAR_H;
}

static void copperUpdate(WORD wBarY) {
	// Blocks are single-buffered (in fast RAM); ACE merges them into both
	// chip-RAM copper buffers over the next two copProcessBlocks() calls.
	if(s_wBarInBlocks == wBarY) {
		return;
	}
	// Editing the MOVE values in place is cheaper than copMove(), but ACE
	// isn't told about it...
	paintBar(s_pLineVal, s_wBarInBlocks, wBarY);
	s_wBarInBlocks = wBarY;
	// ...so tell it the blocks changed, or it never re-merges them.
	s_pView->pCopList->ubStatus |= STATUS_UPDATE;
}
#endif

// --------------------------------------------------------------- drawing ---

static void createPlayerBitmap(void) {
	// Sprites are 16px wide, 2 bitplanes, interleaved, with an empty first and
	// last line where the hardware keeps its control words.
	s_pPlayerBm = bitmapCreate(16, PLAYER_H + 2, 2, BMF_CLEAR | BMF_INTERLEAVED);
	UWORD uwWordsPerRow = s_pPlayerBm->BytesPerRow / 2;
	for(UBYTE y = 0; y < PLAYER_H; ++y) {
		// Planes[] is a byte pointer: index rows in words explicitly.
		UWORD *pRow = (UWORD *)s_pPlayerBm->Planes[0] + (y + 1) * uwWordsPerRow;
		UBYTE isEdge = (y == 0 || y == PLAYER_H - 1);
		pRow[0] = isEdge ? 0xFFFF : 0x8001;                   // plane 0 -> colour 17
		pRow[1] = (y >= 4 && y < 12) ? 0x0FF0 : 0;            // plane 1 -> colour 18
	}
}

static void drawBox(UWORD x, UWORD y, UWORD w, UWORD h) {
	blitRect(s_pBuffer->pBack, x, y, w, h, COLOR_WALL_EDGE);
	blitRect(s_pBuffer->pBack, x + 1, y + 1, w - 2, h - 2, COLOR_WALL);
}

static void drawWalls(void) {
	for(UBYTE i = 0; i < WALL_COUNT; ++i) {
		const tRect *w = &g_pWalls[i];
		drawBox(w->x, w->y, w->w, w->h);
	}
}

static void drawHud(void) {
	// Drawn with the same colour indices as the walls (1 and 2). The copper
	// palette split makes them gold/brown here and blue below line SKY_TOP.
	for(UBYTE i = 0; i < 5; ++i) {
		drawBox(8 + i * 20, 5, 16, 12);
	}
	blitRect(s_pBuffer->pBack, 0, HUD_H - 2, WORLD_W, 2, COLOR_WALL_EDGE);
}

static void readInput(tInput *pInput) {
	// Joystick in port 2, the game port. In ACE that is JOY1 (JOY2 is port 1,
	// the mouse port: reading it turns mouse movement into phantom input).
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
	agkPrint("AGK boot copper\n");

	keyCreate();
	joyOpen();

	buildColorTables();

#if COPPER_RAW
	s_pView = viewCreate(0,
		TAG_VIEW_GLOBAL_PALETTE, 1,
		TAG_VIEW_COPLIST_MODE, VIEW_COPLIST_MODE_RAW,
		TAG_VIEW_COPLIST_RAW_COUNT, COP_RAW_COUNT,
		TAG_DONE);
#else
	s_pView = viewCreate(0, TAG_VIEW_GLOBAL_PALETTE, 1, TAG_DONE);
#endif
	s_uwBeamTop = s_pView->ubPosY;
	s_pVPort = vPortCreate(0, TAG_VPORT_VIEW, s_pView, TAG_VPORT_BPP, BPP, TAG_DONE);
	s_pBuffer = simpleBufferCreate(0,
		TAG_SIMPLEBUFFER_VPORT, s_pVPort,
		TAG_SIMPLEBUFFER_BITMAP_FLAGS, BMF_CLEAR,
#if COPPER_RAW
		TAG_SIMPLEBUFFER_COPLIST_OFFSET, COP_SIMPLEBUFFER_POS,
#endif
		TAG_DONE
	);
	// Loaded once by viewLoad(); from then on the copper list sets 0..2.
	s_pVPort->pPalette[COLOR_BG] = HUD_COLOR0;
	s_pVPort->pPalette[COLOR_WALL] = HUD_COLOR1;
	s_pVPort->pPalette[COLOR_WALL_EDGE] = HUD_COLOR2;
	s_pVPort->pPalette[17] = 0xFFF;
	s_pVPort->pPalette[18] = 0xFA0;
	s_pVPort->pPalette[19] = 0x000;

	logicInit(&s_sState);
	drawHud();
	drawWalls();

	createPlayerBitmap();
#if COPPER_RAW
	spriteManagerCreate(s_pView, COP_SPRITES_POS, 0);
#else
	spriteManagerCreate(s_pView, 0, 0);
#endif
	systemSetDmaBit(DMAB_SPRITE, 1);
	s_pPlayer = spriteAdd(0, s_pPlayerBm);

	copperCreate();
	copperUpdate(logicBarY(s_sState.frame));

	viewLoad(s_pView);
	systemUnuse();
	agkDebugAsync(1); // from here on, debug output doesn't cost frame time
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
	WORD wBarY = logicBarY(s_sState.frame);

	s_pPlayer->wX = s_sState.x;
	s_pPlayer->wY = s_sState.y;
	spriteRequestMetadataUpdate(s_pPlayer);
	spriteProcess(s_pPlayer);
	spriteProcessChannel(0);

	copperUpdate(wBarY);

	// Report state for tests: on every change, plus a heartbeat.
	if(isChanged || s_sState.frame % 50 == 0) {
		agkState("frame", s_sState.frame);
		agkState("x", s_sState.x);
		agkState("y", s_sState.y);
		agkState("blocked", s_sState.isBlocked);
		agkState("bumps", s_sState.bumps);
		agkState("bar", wBarY);
		agkEnd();
	}

	copProcessBlocks(); // block mode: merge blocks; both modes: swap buffers
	agkPerfEnd();
	vPortWaitForEnd(s_pVPort);

	// Copper lists are double-buffered: after two frames our first frame is
	// on screen. Only then tell the harness we're ready.
	if(s_sState.frame == 2) {
		agkReady();
	}
}

void genericDestroy(void) {
	agkDebugAsync(0); // must be off before the OS takes interrupts back
	systemUse();
	systemSetDmaBit(DMAB_SPRITE, 0);
	spriteManagerDestroy();
	bitmapDestroy(s_pPlayerBm);
	viewDestroy(s_pView); // also frees the copper list and all its blocks
	joyClose();
	keyDestroy();
}
