// multiplex - the Amiga side: 24 bullets from 4 hardware sprite channels.
//
// A sprite channel shows one 16 px wide sprite per line, but its data stream
// can hold many sprites one below another: after a sprite's last line come
// the next sprite's control words, and the hardware reads them and carries
// on, until a pair of zero words. The only rule: the next sprite must start
// at least one line below the previous one's end. logic.c sorts the bullets
// by height and deals them onto 6 such chains; this file writes the chains
// into chip RAM (one set per copper buffer) and points the copper at them.
// No blitter, no bitplanes: the bullets cost the CPU the chain writing only.

#include <ace/generic/main.h>
#include <ace/managers/blit.h>
#include <ace/managers/joy.h>
#include <ace/managers/key.h>
#include <ace/managers/memory.h>
#include <ace/managers/sprite.h>
#include <ace/managers/viewport/simplebuffer.h>
#include <ace/utils/custom.h>
#include <ace/utils/extview.h>
#include <hardware/dmabits.h>
#include <agk/debug.h>
#include <agk/perf.h>
#include <agk/copw.h>
#include "art.h"
#include "logic.h"

#define COLOR_FLOOR 1
#define FIRST_CHANNEL 2                       // 2-7: the bullets (0: the player)
#define COP_SPRITES_POS 0                     // the sprite manager's 16 pointer MOVEs
#define COP_SIMPLEBUFFER_POS 16
#define CHAIN_WORDS (BULLETS * (2 + 2 * BULLET_H) + 2)   // all bullets on one chain, plus the end

static tView *s_pView;
static tVPort *s_pVPort;
static tSimpleBufferManager *s_pBuffer;
static tBitMap *s_pPlayerBm, *s_pBulletBm;
static tSprite *s_pPlayer;
static tGameState s_sState;
static tCopBfr *s_pCopBfrA;
static UWORD *s_pChain[2][CHANNELS];          // per copper buffer, per channel: the sprite data stream
static UWORD s_pBulletRows[2 * BULLET_H];     // the bullet's DATA, DATB per line
static tChain s_pChains[CHANNELS];
static UBYTE s_ubDropped, s_ubShown;

// Write this frame's chains for copper buffer ubBfr and point its sprite
// pointers at them. A chain: per sprite POS, CTL, then DATA, DATB per line;
// two zero words end it.
static void chainsWrite(UBYTE ubBfr) {
	s_ubDropped = logicMultiplex(s_sState.pBullets, s_sState.pOrder, BULLETS, s_pChains);
	s_ubShown = 0;
	tCopCmd *pList = s_pView->pCopList->pBackBfr->pList;
	UWORD uwTop = s_pView->ubPosY, uwLeft = (UWORD)(s_pView->ubPosX - 1);
	for(UBYTE c = 0; c < CHANNELS; ++c) {
		UWORD *p = s_pChain[ubBfr][c];
		const tChain *pChain = &s_pChains[c];
		for(UBYTE k = 0; k < pChain->count; ++k) {
			const tBullet *pB = &s_sState.pBullets[pChain->pIndex[k]];
			UWORD uwVStart = (UWORD)(uwTop + pB->y), uwVStop = (UWORD)(uwVStart + BULLET_H);
			UWORD uwHStart = (UWORD)(uwLeft + pB->x);
			*p++ = (UWORD)((uwVStart << 8) | ((uwHStart >> 1) & 0xFF));
			*p++ = (UWORD)((uwVStop << 8) | (((uwVStart >> 8) & 1) << 2) | (((uwVStop >> 8) & 1) << 1) | (uwHStart & 1));
			const ULONG *pRows = (const ULONG *)s_pBulletRows;   // DATA, DATB pairs: 8 longs
			ULONG *pDst = (ULONG *)p;
			for(UBYTE r = 0; r < BULLET_H; ++r) *pDst++ = *pRows++;
			p = (UWORD *)pDst;
		}
		*p++ = 0;                             // the end of the chain
		*p = 0;
		s_ubShown = (UBYTE)(s_ubShown + pChain->count);
		ULONG ulPtr = (ULONG)s_pChain[ubBfr][c];
		copSetMoveVal(&pList[COP_SPRITES_POS + 2 * (FIRST_CHANNEL + c)].sMove, (UWORD)(ulPtr >> 16));
		copSetMoveVal(&pList[COP_SPRITES_POS + 2 * (FIRST_CHANNEL + c) + 1].sMove, (UWORD)(ulPtr & 0xFFFF));
	}
}

static void readInput(tInput *pInput) {
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
	agkPrint("AGK boot multiplex\n");
	keyCreate();
	joyOpen();

	UWORD uwRawCount = COP_SIMPLEBUFFER_POS + simpleBufferGetRawCopperlistInstructionCount(ART_DEPTH);
	s_pView = viewCreate(0,
		TAG_VIEW_GLOBAL_PALETTE, 1,
		TAG_VIEW_COPLIST_MODE, VIEW_COPLIST_MODE_RAW,   // so the sprite pointers can be set per frame
		TAG_VIEW_COPLIST_RAW_COUNT, uwRawCount,
		TAG_DONE);
	s_pVPort = vPortCreate(0, TAG_VPORT_VIEW, s_pView, TAG_VPORT_BPP, ART_DEPTH, TAG_DONE);
	s_pBuffer = simpleBufferCreate(0,
		TAG_SIMPLEBUFFER_VPORT, s_pVPort,
		TAG_SIMPLEBUFFER_BITMAP_FLAGS, BMF_CLEAR,
		TAG_SIMPLEBUFFER_COPLIST_OFFSET, COP_SIMPLEBUFFER_POS,
		TAG_DONE);
	artPaletteApply(s_pVPort->pPalette);
	artPlayerApplyColors(s_pVPort->pPalette);        // channel 0: colours 17-19
	artBulletApplyColors(s_pVPort->pPalette);        // channels 2-3: colours 21-23...
	for(UBYTE i = 0; i < 3; ++i) {                   // ...and 4-5, 6-7 the same
		s_pVPort->pPalette[25 + i] = s_pVPort->pPalette[21 + i];
		s_pVPort->pPalette[29 + i] = s_pVPort->pPalette[21 + i];
	}
	blitRect(s_pBuffer->pBack, 0, WORLD_H - 4, WORLD_W, 4, COLOR_FLOOR);

	logicInit(&s_sState);
	s_pPlayerBm = artPlayerCreate(0);
	s_pBulletBm = artBulletCreate(0);
	// the bullet's lines, from the art's sprite bitmap: row 0 is the control
	// words, then DATA, DATB per line (2 planes, interleaved)
	const UWORD *pSrc = (const UWORD *)(s_pBulletBm->Planes[0] + s_pBulletBm->BytesPerRow);
	for(UBYTE i = 0; i < 2 * BULLET_H; ++i) s_pBulletRows[i] = pSrc[i];
	for(UBYTE b = 0; b < 2; ++b) {
		for(UBYTE c = 0; c < CHANNELS; ++c) s_pChain[b][c] = memAllocChipClear(CHAIN_WORDS * sizeof(UWORD));
	}

	spriteManagerCreate(s_pView, COP_SPRITES_POS, 0);
	systemSetDmaBit(DMAB_SPRITE, 1);
	s_pPlayer = spriteAdd(ART_PLAYER_CHANNEL, s_pPlayerBm);
	s_pCopBfrA = s_pView->pCopList->pBackBfr;
	chainsWrite(0);
	copProcessBlocks();                              // (raw mode: swaps the buffers)
	chainsWrite(1);

	viewLoad(s_pView);
	systemUnuse();
	agkDebugAsync(1);
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
	UBYTE isChanged = logicUpdate(&s_sState, &sInput);

	UBYTE ubBfr = agkCopBfr(s_pView->pCopList, s_pCopBfrA);
	chainsWrite(ubBfr);
	s_pPlayer->wX = s_sState.x;
	s_pPlayer->wY = s_sState.y;
	spriteRequestMetadataUpdate(s_pPlayer);
	spriteProcess(s_pPlayer);
	spriteProcessChannel(ART_PLAYER_CHANNEL);

	if(isChanged || s_sState.frame < 3 || (s_sState.frame & 63) == 0) {
		agkState("frame", s_sState.frame);
		agkState("x", s_sState.x);
		agkState("y", s_sState.y);
		agkState("hits", s_sState.hits);
		agkState("shown", s_ubShown);
		agkState("dropped", s_ubDropped);
		agkEnd();
	}
	copProcessBlocks();
	agkPerfEnd();
	vPortWaitForEnd(s_pVPort);
	if(s_sState.frame == 2) {
		agkReady();
	}
}

void genericDestroy(void) {
	agkDebugAsync(0);
	systemUse();
	systemSetDmaBit(DMAB_SPRITE, 0);
	spriteManagerDestroy();
	for(UBYTE b = 0; b < 2; ++b) {
		for(UBYTE c = 0; c < CHANNELS; ++c) memFree(s_pChain[b][c], CHAIN_WORDS * sizeof(UWORD));
	}
	bitmapDestroy(s_pPlayerBm);
	bitmapDestroy(s_pBulletBm);
	viewDestroy(s_pView);
	joyClose();
	keyDestroy();
}
