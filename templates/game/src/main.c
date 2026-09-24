// {{name}} - Amiga side: display, input, and hooking the rules in logic.c up
// to the hardware. Game rules belong in logic.c (host-testable), not here.

#include <ace/generic/main.h>
#include <ace/managers/blit.h>
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

static tView *s_pView;
static tVPort *s_pVPort;
static tSimpleBufferManager *s_pBuffer;
static tBitMap *s_pPlayerBm;
static tSprite *s_pPlayer;
static tGameState s_sState;

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

static void drawWalls(void) {
	for(UBYTE i = 0; i < WALL_COUNT; ++i) {
		const tRect *w = &g_pWalls[i];
		blitRect(s_pBuffer->pBack, w->x, w->y, w->w, w->h, COLOR_WALL_EDGE);
		blitRect(s_pBuffer->pBack, w->x + 1, w->y + 1, w->w - 2, w->h - 2, COLOR_WALL);
	}
}

static void readInput(tInput *pInput) {
	// Joystick in port 2 (the game port). Also accept port 1 so either works.
	pInput->dx = 0;
	pInput->dy = 0;
	if(joyCheck(JOY1 + JOY_LEFT) || joyCheck(JOY2 + JOY_LEFT)) pInput->dx = -1;
	if(joyCheck(JOY1 + JOY_RIGHT) || joyCheck(JOY2 + JOY_RIGHT)) pInput->dx = 1;
	if(joyCheck(JOY1 + JOY_UP) || joyCheck(JOY2 + JOY_UP)) pInput->dy = -1;
	if(joyCheck(JOY1 + JOY_DOWN) || joyCheck(JOY2 + JOY_DOWN)) pInput->dy = 1;
	pInput->fire = joyCheck(JOY1 + JOY_FIRE) || joyCheck(JOY2 + JOY_FIRE);
}

void genericCreate(void) {
	agkDebugInit();
	agkPrint("AGK boot {{name}}\n");

	keyCreate();
	joyOpen();

	s_pView = viewCreate(0, TAG_VIEW_GLOBAL_PALETTE, 1, TAG_DONE);
	s_pVPort = vPortCreate(0, TAG_VPORT_VIEW, s_pView, TAG_VPORT_BPP, 2, TAG_DONE);
	s_pBuffer = simpleBufferCreate(0,
		TAG_SIMPLEBUFFER_VPORT, s_pVPort,
		TAG_SIMPLEBUFFER_BITMAP_FLAGS, BMF_CLEAR,
		TAG_DONE
	);
	s_pVPort->pPalette[COLOR_BG] = 0x113;
	s_pVPort->pPalette[COLOR_WALL] = 0x468;
	s_pVPort->pPalette[COLOR_WALL_EDGE] = 0x9BD;
	s_pVPort->pPalette[17] = 0xFFF;
	s_pVPort->pPalette[18] = 0xFA0;
	s_pVPort->pPalette[19] = 0x000;

	logicInit(&s_sState);
	drawWalls();

	createPlayerBitmap();
	spriteManagerCreate(s_pView, 0, 0);
	systemSetDmaBit(DMAB_SPRITE, 1);
	s_pPlayer = spriteAdd(0, s_pPlayerBm);

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

	s_pPlayer->wX = s_sState.x;
	s_pPlayer->wY = s_sState.y;
	spriteRequestMetadataUpdate(s_pPlayer);
	spriteProcess(s_pPlayer);
	spriteProcessChannel(0);

	// Report state for tests: on every change, plus a heartbeat.
	if(isChanged || s_sState.frame % 50 == 0) {
		agkState("frame", s_sState.frame);
		agkState("x", s_sState.x);
		agkState("y", s_sState.y);
		agkState("blocked", s_sState.isBlocked);
		agkState("bumps", s_sState.bumps);
		agkEnd();
	}

	copProcessBlocks();
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
	viewDestroy(s_pView);
	joyClose();
	keyDestroy();
}
