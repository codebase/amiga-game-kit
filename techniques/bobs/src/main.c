// bobs technique - Amiga side: display, input, and hooking the rules in logic.c up
// to the hardware. Game rules belong in logic.c (host-testable), not here.

#include <ace/generic/main.h>
#include <ace/managers/blit.h>
#include <ace/managers/bob.h>
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
#define COLOR_ENEMY_BODY 4
#define COLOR_ENEMY_EYE 5
#define COLOR_ENEMY_DARK 6
#define DISPLAY_BPP 3

// Enemy BOB art: '.' is transparent, R/Y/K are body/eye/dark.
static const char *s_pEnemyArt[ENEMY_H] = {
	"............RRRRRRRR............",
	".........RRRRRRRRRRRRRR.........",
	".......RRRRRRRRRRRRRRRRRR.......",
	".....RRRRRRRRRRRRRRRRRRRRRR.....",
	"....RRRRYYYYYRRRRRRYYYYYRRRR....",
	"...RRRRYYYYYYYRRRRYYYYYYYRRRR...",
	"..RRRRRYYYKKYYRRRRYYKKYYYRRRRR..",
	"..RRRRRYYYKKYYRRRRYYKKYYYRRRRR..",
	".RRRRRRRYYYYYRRRRRRYYYYYRRRRRRR.",
	".RRRRRRRRRRRRRRRRRRRRRRRRRRRRRR.",
	"RRRRRRRKKKKKKKKKKKKKKKKKKRRRRRRR",
	"RRRRRRRRKYKYKYKYKYKYKYKYRRRRRRRR",
	"RRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRR",
	".RRRR..RRRR..RRRRRR..RRRR..RRRR.",
	".RRR....RR....RRRR....RR....RRR.",
	"..R.....R......RR......R.....R..",
};

static tView *s_pView;
static tVPort *s_pVPort;
static tSimpleBufferManager *s_pBuffer;
static tBitMap *s_pPlayerBm;
static tSprite *s_pPlayer;
static tGameState s_sState;
static tBitMap *s_pEnemyBm;
static tBitMap *s_pEnemyMask;
static tBob s_sEnemy;

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

static void createEnemyBitmaps(void) {
	// BOB frame and mask: same width as the bob, same depth as the display,
	// interleaved. The mask has the same bits in every plane.
	s_pEnemyBm = bitmapCreate(ENEMY_W, ENEMY_H, DISPLAY_BPP, BMF_CLEAR | BMF_INTERLEAVED);
	s_pEnemyMask = bitmapCreate(ENEMY_W, ENEMY_H, DISPLAY_BPP, BMF_CLEAR | BMF_INTERLEAVED);
	for(UBYTE y = 0; y < ENEMY_H; ++y) {
		UWORD pPlane[DISPLAY_BPP][2] = {{0}};
		UWORD pMask[2] = {0};
		for(UBYTE x = 0; x < ENEMY_W; ++x) {
			UBYTE ubColor;
			switch(s_pEnemyArt[y][x]) {
				case 'R': ubColor = COLOR_ENEMY_BODY; break;
				case 'Y': ubColor = COLOR_ENEMY_EYE; break;
				case 'K': ubColor = COLOR_ENEMY_DARK; break;
				default: continue;
			}
			UWORD uwBit = 0x8000 >> (x & 15);
			pMask[x >> 4] |= uwBit;
			for(UBYTE p = 0; p < DISPLAY_BPP; ++p) {
				if(ubColor & (1 << p)) {
					pPlane[p][x >> 4] |= uwBit;
				}
			}
		}
		for(UBYTE p = 0; p < DISPLAY_BPP; ++p) {
			// Interleaved: Planes[p] points at plane p of row 0; rows are BytesPerRow apart.
			UWORD *pDst = (UWORD *)(s_pEnemyBm->Planes[p] + y * s_pEnemyBm->BytesPerRow);
			UWORD *pDstMask = (UWORD *)(s_pEnemyMask->Planes[p] + y * s_pEnemyMask->BytesPerRow);
			pDst[0] = pPlane[p][0];
			pDst[1] = pPlane[p][1];
			pDstMask[0] = pMask[0];
			pDstMask[1] = pMask[1];
		}
	}
}

static void drawWalls(tBitMap *pBm) {
	for(UBYTE i = 0; i < WALL_COUNT; ++i) {
		const tRect *w = &g_pWalls[i];
		blitRect(pBm, w->x, w->y, w->w, w->h, COLOR_WALL_EDGE);
		blitRect(pBm, w->x + 1, w->y + 1, w->w - 2, w->h - 2, COLOR_WALL);
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
	agkPrint("AGK boot bobs\n");

	keyCreate();
	joyOpen();

	s_pView = viewCreate(0, TAG_VIEW_GLOBAL_PALETTE, 1, TAG_DONE);
	s_pVPort = vPortCreate(0, TAG_VPORT_VIEW, s_pView, TAG_VPORT_BPP, DISPLAY_BPP, TAG_DONE);
	// Double-buffered and interleaved, as ACE's bob manager expects: each frame
	// the enemy is undrawn/drawn in the back buffer while the front is shown.
	s_pBuffer = simpleBufferCreate(0,
		TAG_SIMPLEBUFFER_VPORT, s_pVPort,
		TAG_SIMPLEBUFFER_BITMAP_FLAGS, BMF_CLEAR | BMF_INTERLEAVED,
		TAG_SIMPLEBUFFER_IS_DBLBUF, 1,
		TAG_DONE
	);
	s_pVPort->pPalette[COLOR_BG] = 0x113;
	s_pVPort->pPalette[COLOR_WALL] = 0x468;
	s_pVPort->pPalette[COLOR_WALL_EDGE] = 0x9BD;
	s_pVPort->pPalette[COLOR_ENEMY_BODY] = 0xD22;
	s_pVPort->pPalette[COLOR_ENEMY_EYE] = 0xFF8;
	s_pVPort->pPalette[COLOR_ENEMY_DARK] = 0x200;
	s_pVPort->pPalette[17] = 0xFFF;
	s_pVPort->pPalette[18] = 0xFA0;
	s_pVPort->pPalette[19] = 0x000;

	logicInit(&s_sState);
	// Static background goes into both buffers, or it would flicker.
	drawWalls(s_pBuffer->pFront);
	drawWalls(s_pBuffer->pBack);

	createEnemyBitmaps();
	bobManagerCreate(s_pBuffer->pFront, s_pBuffer->pBack, s_pBuffer->pBack->Rows);
	bobInit(
		&s_sEnemy, ENEMY_W, ENEMY_H, 1,
		bobCalcFrameAddress(s_pEnemyBm, 0), bobCalcFrameAddress(s_pEnemyMask, 0),
		s_sState.enemyX, ENEMY_Y
	);
	bobReallocateBuffers();

	createPlayerBitmap();
	spriteManagerCreate(s_pView, 0, 0);
	systemSetDmaBit(DMAB_SPRITE, 1);
	s_pPlayer = spriteAdd(0, s_pPlayerBm);

	viewLoad(s_pView);
	systemUnuse();
	agkDebugAsync(1); // from here on, debug output doesn't cost frame time
}

void genericProcess(void) {
	agkPerfBegin();
	keyProcess();
	joyProcess();
	if(keyCheck(KEY_ESCAPE)) {
		gameExit();
		return;
	}

	tInput sInput;
	readInput(&sInput);
	UBYTE ubOldLives = s_sState.lives;
	int8_t bOldDir = s_sState.enemyDir;
	UBYTE isChanged = logicUpdate(&s_sState, &sInput);

	// Enemy BOB: restore the background behind last frame's image in this
	// back buffer, then draw at the new position. No other blits in between.
	bobBegin(s_pBuffer->pBack);
	s_sEnemy.sPos.uwX = s_sState.enemyX;
	s_sEnemy.sPos.uwY = ENEMY_Y;
	bobPush(&s_sEnemy);
	bobEnd();

	s_pPlayer->wX = s_sState.x;
	s_pPlayer->wY = s_sState.y;
	spriteRequestMetadataUpdate(s_pPlayer);
	spriteProcess(s_pPlayer);
	spriteProcessChannel(0);

	// Report state for tests: on every change, when the enemy turns, plus a
	// heartbeat every 10 frames so tests can check the enemy position.
	if(isChanged || s_sState.enemyDir != bOldDir || s_sState.frame % 10 == 0) {
		agkState("frame", s_sState.frame);
		agkState("x", s_sState.x);
		agkState("y", s_sState.y);
		agkState("blocked", s_sState.isBlocked);
		agkState("bumps", s_sState.bumps);
		agkState("lives", s_sState.lives);
		agkState("ex", s_sState.enemyX);
		agkEnd();
	}
	if(s_sState.lives != ubOldLives) {
		agkPrint("AGK hit\n");
		if(s_sState.isGameOver) {
			agkPrint("AGK gameover\n");
		}
	}

	viewProcessManagers(s_pView); // shows the buffer we just drew, swaps pBack
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
	bobManagerDestroy();
	bitmapDestroy(s_pEnemyBm);
	bitmapDestroy(s_pEnemyMask);
	bitmapDestroy(s_pPlayerBm);
	viewDestroy(s_pView);
	joyClose();
	keyDestroy();
}
