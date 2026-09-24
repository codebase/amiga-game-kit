// sprites technique - Amiga side. All graphics come from art/ through the AGK
// art pipeline: art/player.txt (text art, 4-frame walk) becomes a hardware
// sprite, art/enemy.png (PNG sheet, 2 frames) becomes a BOB. `agk build`
// converts them into build/art/art.c + art.h; this file only uses art.h.

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
#include "art.h"
#include "logic.h"

#define COLOR_WALL 1
#define COLOR_WALL_EDGE 2
#define WALK_FRAME_TICKS 6   // change walk frame every 6 game frames
#define ENEMY_FRAME_TICKS 16

static tView *s_pView;
static tVPort *s_pVPort;
static tSimpleBufferManager *s_pBuffer;
static tBitMap *s_pPlayerFrames[ART_PLAYER_FRAMES];
static tSprite *s_pPlayer;
static tBitMap *s_pEnemyBm, *s_pEnemyMask;
static tBob s_sEnemy;
static tGameState s_sState;
static UBYTE s_ubWalkFrame, s_ubWalkTicks, s_ubEnemyFrame;

static void drawWalls(tBitMap *pBm) {
	for(UBYTE i = 0; i < WALL_COUNT; ++i) {
		const tRect *w = &g_pWalls[i];
		blitRect(pBm, w->x, w->y, w->w, w->h, COLOR_WALL_EDGE);
		blitRect(pBm, w->x + 1, w->y + 1, w->w - 2, w->h - 2, COLOR_WALL);
	}
}

static void readInput(tInput *pInput) {
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
	agkPrint("AGK boot sprites\n");

	keyCreate();
	joyOpen();

	s_pView = viewCreate(0, TAG_VIEW_GLOBAL_PALETTE, 1, TAG_DONE);
	s_pVPort = vPortCreate(0, TAG_VPORT_VIEW, s_pView, TAG_VPORT_BPP, ART_DEPTH, TAG_DONE);
	s_pBuffer = simpleBufferCreate(0,
		TAG_SIMPLEBUFFER_VPORT, s_pVPort,
		TAG_SIMPLEBUFFER_BITMAP_FLAGS, BMF_CLEAR | BMF_INTERLEAVED,
		TAG_SIMPLEBUFFER_IS_DBLBUF, 1,
		TAG_DONE
	);
	artPaletteApply(s_pVPort->pPalette);       // art/palette.txt -> colours 0-7
	artPlayerApplyColors(s_pVPort->pPalette);  // player sprite -> colours 17-19

	logicInit(&s_sState);
	drawWalls(s_pBuffer->pFront);
	drawWalls(s_pBuffer->pBack);

	// Enemy BOB: all frames stacked vertically in one bitmap (+ matching mask)
	s_pEnemyBm = artEnemyCreate();
	s_pEnemyMask = artEnemyCreateMask();
	bobManagerCreate(s_pBuffer->pFront, s_pBuffer->pBack, s_pBuffer->pBack->Rows);
	bobInit(
		&s_sEnemy, ART_ENEMY_BITMAP_W, ART_ENEMY_H, 1,
		bobCalcFrameAddress(s_pEnemyBm, 0), bobCalcFrameAddress(s_pEnemyMask, 0),
		s_sState.enemyX, ENEMY_Y
	);
	bobReallocateBuffers();

	// Player sprite: one bitmap per frame (the sprite manager writes control
	// words into the bitmap it shows, so frames can't share one)
	for(UBYTE i = 0; i < ART_PLAYER_FRAMES; ++i) {
		s_pPlayerFrames[i] = artPlayerCreate(i);
	}
	spriteManagerCreate(s_pView, 0, 0);
	systemSetDmaBit(DMAB_SPRITE, 1);
	s_pPlayer = spriteAdd(ART_PLAYER_CHANNEL, s_pPlayerFrames[0]);

	viewLoad(s_pView);
	systemUnuse();
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
	UBYTE isChanged = logicUpdate(&s_sState, &sInput);

	// Walk animation: cycle frames while moving, stand (frame 0) otherwise
	UBYTE ubOldWalk = s_ubWalkFrame;
	if(sInput.dx || sInput.dy) {
		if(++s_ubWalkTicks >= WALK_FRAME_TICKS) {
			s_ubWalkTicks = 0;
			s_ubWalkFrame = (s_ubWalkFrame + 1) % ART_PLAYER_FRAMES;
		}
	}
	else {
		s_ubWalkTicks = 0;
		s_ubWalkFrame = 0;
	}
	if(s_ubWalkFrame != ubOldWalk) {
		spriteSetBitmap(s_pPlayer, s_pPlayerFrames[s_ubWalkFrame]);
	}

	// Enemy: swap BOB frame (same bitmap, different offset)
	UBYTE ubEnemyFrame = (s_sState.frame / ENEMY_FRAME_TICKS) % ART_ENEMY_FRAMES;
	if(ubEnemyFrame != s_ubEnemyFrame) {
		s_ubEnemyFrame = ubEnemyFrame;
		bobSetFrame(&s_sEnemy,
			bobCalcFrameAddress(s_pEnemyBm, ubEnemyFrame * ART_ENEMY_H),
			bobCalcFrameAddress(s_pEnemyMask, ubEnemyFrame * ART_ENEMY_H)
		);
	}

	bobBegin(s_pBuffer->pBack);
	s_sEnemy.sPos.uwX = s_sState.enemyX;
	s_sEnemy.sPos.uwY = ENEMY_Y;
	bobPush(&s_sEnemy);
	bobEnd();

	s_pPlayer->wX = s_sState.x;
	s_pPlayer->wY = s_sState.y;
	spriteRequestMetadataUpdate(s_pPlayer);
	spriteProcess(s_pPlayer);
	spriteProcessChannel(ART_PLAYER_CHANNEL);

	if(isChanged || s_ubWalkFrame != ubOldWalk || s_sState.frame % 10 == 0) {
		agkState("frame", s_sState.frame);
		agkState("x", s_sState.x);
		agkState("y", s_sState.y);
		agkState("walk", s_ubWalkFrame);
		agkState("ex", s_sState.enemyX);
		agkState("eframe", s_ubEnemyFrame);
		agkState("lives", s_sState.lives);
		agkEnd();
	}
	if(s_sState.lives != ubOldLives) {
		agkPrint("AGK hit\n");
	}

	viewProcessManagers(s_pView);
	copProcessBlocks();
	agkPerfEnd();
	vPortWaitForEnd(s_pVPort);

	if(s_sState.frame == 2) {
		agkReady();
	}
}

void genericDestroy(void) {
	systemUse();
	systemSetDmaBit(DMAB_SPRITE, 0);
	spriteManagerDestroy();
	bobManagerDestroy();
	bitmapDestroy(s_pEnemyBm);
	bitmapDestroy(s_pEnemyMask);
	for(UBYTE i = 0; i < ART_PLAYER_FRAMES; ++i) {
		bitmapDestroy(s_pPlayerFrames[i]);
	}
	viewDestroy(s_pView);
	joyClose();
	keyDestroy();
}
