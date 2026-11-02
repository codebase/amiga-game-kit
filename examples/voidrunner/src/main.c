// VOIDRUNNER - the Amiga side: display, input, sound. The game's rules are
// in logic.c (host-testable); this draws what logicDraw() lists.
//
// OCS dual playfield, 5 bitplanes, 50 frames a second:
//   PF1 (planes 1/3/5, colours 1-7): lines 0-23 show the HUD bitmap; from
//        line 24 a double-buffered frame buffer with the enemies, shots and
//        explosions (the blitter, fed from a queue while the CPU works).
//   PF2 (planes 2/4, colours 9-11): the backdrop - a gas giant, a moon,
//        nebula - drifting a quarter pixel a frame (fine scroll).
//   Sprites 0-3: the player's ship (32 px, 15 colours, attached pairs).
//   Sprite 6: the starfield. The copper re-positions it on every line (a
//        star on half the lines, three speeds and brightnesses). Those lines
//        are a separate copper list per buffer (COPJMP2 jumps there), and a
//        star moves by decrementing its position byte - a layer at a time.
//        Sprites 6-7 sit behind both playfields: the planet hides stars.
// Everything the display reads is double-buffered - the frame buffer, the
// HUD, the star lists, the ship's sprites - so what a screenshot shows never
// depends on how fast the CPU got there (Kickstart 1.3, 3.1 and AROS differ).
//   COLOR00: a deep blue gradient, set per line by the copper.

#include <ace/generic/main.h>
#include <ace/managers/blit.h>
#include <ace/managers/copper.h>
#include <ace/managers/joy.h>
#include <ace/managers/key.h>
#include <ace/managers/memory.h>
#include <ace/managers/sprite.h>
#include <ace/managers/system.h>
#include <ace/managers/timer.h>
#include <ace/utils/custom.h>
#include <ace/utils/extview.h>
#include <hardware/dmabits.h>
#include <agk/blitq.h>
#include <agk/copw.h>
#include <agk/debug.h>
#include <agk/perf.h>
#include <agk/stars.h>
#include <agk/text.h>
#include "art.h"        // generated from art/ by agk (agk help-art)
#include "sound.h"      // generated from sound/ by agk (agk help-sound)
#include "logic.h"

// ------------------------------------------------------ display constants ---

#define FRAME_VBLS 1
#define SYNC_FRAME 10         // "AGK t0": scenarios start here on every profile
#define BPLCON0_DPF5 0x5600   // 5 planes | DBLPF | COLOR
// PF1P = PF2P = 3: sprite pairs 0-1 and 2-3 (the ship) in front of both
// playfields, 6-7 (the stars) behind them; PF2PRI = 0: PF1 in front of PF2
#define BPLCON2_VR 0x001B
#define DDFSTRT_SCROLL 0x30   // one fetch word early for PF2's fine scroll (costs sprite 7)
#define DDFSTOP_LORES 0xD0
#define FETCH_BYTES 42        // 21 words a line: 320 px + the scroll word
#define SWITCH_WAIT_X 0xD8    // after the last fetch of a line
#define COP_SPRITES_POS 0
#define COP_TOP_POS 16
#define COP_RAW_COUNT 120      // the top of the frame; the lines are in the star list
#define STAR_CHANNEL 6

// PF1 frame buffers: margins all round (48 px left, 112 right for the boss
// flying in, 32 above and below), so objects never need clipping. Screen
// (x, y) is buffer (x + FB_X0, y + FB_Y0).
#define FB_X0 48
#define FB_W (FB_X0 + SCREEN_W + 112)
#define FB_Y0 32
#define FB_H (SCREEN_H + 2 * FB_Y0)
#define FB_BPP 3
#define FB_PLANE_BYTES (FB_W / 8)
#define FB_ROW_BYTES (FB_PLANE_BYTES * FB_BPP)   // interleaved
#define FB_FETCH_OFFS ((FB_X0 - 16) / 8)        // the early fetch word
// The HUD: 336 x HUD_H (the first 16 px are the early fetch word)
#define HUD_W (SCREEN_W + 16)
#define HUD_ROW_BYTES (HUD_W / 8 * FB_BPP)
// The backdrop: 640 px + a 320 px wrap copy, 2 planes interleaved
#define BD_LOOP 640
#define BD_ROW_BYTES (ART_BACKDROP_BITMAP_W / 8 * 2)

static tView *s_pView;
static tVPort *s_pVPort;
static tBitMap *s_pFb[2], *s_pHud[2], *s_pBackdrop;
static UBYTE *s_pBlankRow;          // FETCH_BYTES of zeros (PF2 in the HUD lines)
static tCopBfr *s_pCopBfrA;         // copper buffer 0
static UWORD s_uwCopUsed;
static UWORD s_uwCopCon1, s_uwCopPf2;   // copper indices of BPLCON1 and PF2's pointers
// The starfield (agk/stars.h): a sprite star per line, three layers, in its
// own copper lists; the main list ends by jumping to it
static tAgkStars s_sStars;
static UWORD s_pSpace[SCREEN_H];        // COLOR00 per line

static tGame s_sGame;
static ULONG s_ulFrameVbl;

static inline UBYTE backBuffer(void) {
	return agkCopBfr(s_pView->pCopList, s_pCopBfrA);
}

// --------------------------------------------------------- copper list ---

static void copperWriteList(tCopCmd *pList, UBYTE ubBfr) {
	tAgkCopWriter sW;
	agkCopInit(&sW, pList, COP_TOP_POS, s_pView->ubPosY);
	const tBitMap *pFb = s_pFb[ubBfr];

	// Top of the frame: the HUD in PF1, nothing in PF2
	agkCopMove(&sW, &g_pCustom->bplcon0, BPLCON0_DPF5);
	agkCopMove(&sW, &g_pCustom->bplcon2, BPLCON2_VR);
	agkCopMove(&sW, &g_pCustom->ddfstrt, DDFSTRT_SCROLL);
	agkCopMove(&sW, &g_pCustom->ddfstop, DDFSTOP_LORES);
	s_uwCopCon1 = sW.uwPos;
	agkCopMove(&sW, &g_pCustom->bplcon1, 0);
	agkCopMove(&sW, &g_pCustom->bpl1mod, HUD_ROW_BYTES - FETCH_BYTES);
	agkCopMove(&sW, &g_pCustom->bpl2mod, (UWORD)-FETCH_BYTES);   // re-read the blank row
	for(UBYTE p = 0; p < 3; ++p) {
		agkCopPtrs(&sW, 2 * p, (ULONG)s_pHud[ubBfr]->Planes[p]);
	}
	for(UBYTE p = 0; p < 2; ++p) {
		agkCopPtrs(&sW, 2 * p + 1, (ULONG)s_pBlankRow);
	}
	agkCopMove(&sW, &g_pCustom->color[0], s_pSpace[0]);

	// After the HUD's last line is fetched: the game frame buffer and the backdrop
	agkCopWait(&sW, PLAY_TOP - 1, SWITCH_WAIT_X);
	agkCopMove(&sW, &g_pCustom->bpl1mod, FB_ROW_BYTES - FETCH_BYTES);
	agkCopMove(&sW, &g_pCustom->bpl2mod, BD_ROW_BYTES - FETCH_BYTES);
	for(UBYTE p = 0; p < 3; ++p) {
		agkCopPtrs(&sW, 2 * p, (ULONG)pFb->Planes[p] + (FB_Y0 + PLAY_TOP) * FB_ROW_BYTES + FB_FETCH_OFFS);
	}
	s_uwCopPf2 = sW.uwPos;
	for(UBYTE p = 0; p < 2; ++p) {
		agkCopPtrs(&sW, 2 * p + 1, (ULONG)s_pBackdrop->Planes[p]);
	}
	// ...and on to the lines: this buffer's star list ends the frame
	agkStarsJump(&sW, &s_sStars, ubBfr);
	s_uwCopUsed = sW.uwPos;
	agkCopFill(&sW, COP_RAW_COUNT);
}

// ------------------------------------------------------- stars, backdrop ---

static void starsCreate(void) {
	for(UWORD y = 0; y < SCREEN_H; ++y) {
		// deep space: black at the top, navy lower down; between two shades
		// the lines alternate, so there are no hard bands
		UWORD uwLevel = y < 60 ? 0 : (y - 60) * 6 / (SCREEN_H - 60);    // 0..5
		UWORD uwB = (uwLevel >> 1) + ((uwLevel & 1) & (y & 1));
		s_pSpace[y] = uwB;                                               // 0x000 .. 0x003
	}
	agkStarsCreate(&s_sStars, STAR_CHANNEL, PLAY_TOP, SCREEN_H, s_pView->ubPosY, 0x2545F491, s_pSpace, SCREEN_W);
}

static UWORD s_uwBdX;   // the backdrop's scroll, px (0..BD_LOOP-1)

static void backdropUpdate(UBYTE ubBfr) {
	// a quarter pixel a frame: a pixel every 4th
	(void)ubBfr;
	if(!(s_sGame.frame & 3) && ++s_uwBdX == BD_LOOP) s_uwBdX = 0;
	// The first fetched word is one early (DDFSTRT 0x30) and shifted right by
	// the delay, so pixel s is at screen x 0 when the fetch starts at word
	// (s - 1) / 16 with a delay of 16 - s % 16 (0 on a word boundary). s runs
	// 1..640: the bitmap's wrap copy makes 640 the same as 0.
	UWORD s = s_uwBdX ? s_uwBdX : BD_LOOP;
	UWORD uwWord = (UWORD)(((s - 1) >> 4) * 2), uwDelay = (UWORD)((16 - (s & 15)) & 15);
	UWORD *pList = (UWORD *)s_pView->pCopList->pBackBfr->pList;
	pList[2 * s_uwCopCon1 + 1] = (UWORD)(uwDelay << 4);
	for(UBYTE p = 0; p < 2; ++p) {
		ULONG ulAddr = (ULONG)s_pBackdrop->Planes[p] + uwWord;
		pList[2 * (s_uwCopPf2 + 2 * p) + 1] = (UWORD)(ulAddr >> 16);
		pList[2 * (s_uwCopPf2 + 2 * p + 1) + 1] = (UWORD)ulAddr;
	}
}

// ----------------------------------------------------------- blit queue ---
// Each picture: erase what this buffer showed last time, then draw the
// objects, back to front. The CPU feeds the blits to the blitter while it
// does the stars (polled, not interrupt-driven).

// The game's BOBs (logic.h BOB_*), then the logo and the messages
#define BOB_LOGO (BOB_TYPES + 0)
#define BOB_MSG_STAGE (BOB_TYPES + 1)
#define BOB_MSG_WARNING (BOB_TYPES + 2)
#define BOB_MSG_CLEAR (BOB_TYPES + 3)
#define BOB_MSG_OVER (BOB_TYPES + 4)
#define BOB_MSG_FIRE (BOB_TYPES + 5)
#define BOBS (BOB_TYPES + 6)
#define DRAWS_MAX (DRAW_MAX + 4)

static tAgkBlitArea s_pErase[2][DRAWS_MAX];
static UBYTE s_pErased[2];

static tBitMap *s_pBobBm[BOBS], *s_pBobMask[BOBS];
static UBYTE s_pBobW[BOBS], s_pBobH[BOBS], s_pBobFrames[BOBS], s_pTplFirst[BOBS];
#define TPL_MAX 64
static tAgkBlit s_pTpl[TPL_MAX][2];         // per BOB frame, per "one more word for the shift"
static UWORD s_pFbRowOffs[FB_H];

static void bobAdd(UBYTE ubBob, tBitMap *pBm, tBitMap *pMask, UBYTE ubW, UBYTE ubH, UBYTE ubFrames) {
	s_pBobBm[ubBob] = pBm;
	s_pBobMask[ubBob] = pMask;
	s_pBobW[ubBob] = ubW;
	s_pBobH[ubBob] = ubH;
	s_pBobFrames[ubBob] = ubFrames;
}

static void bobsCreate(void) {
	bobAdd(BOB_DART, artDartCreate(), artDartCreateMask(), ART_DART_W, ART_DART_H, ART_DART_FRAMES);
	bobAdd(BOB_GUNSHIP, artGunshipCreate(), artGunshipCreateMask(), ART_GUNSHIP_W, ART_GUNSHIP_H, ART_GUNSHIP_FRAMES);
	bobAdd(BOB_LASER, artLaserCreate(), artLaserCreateMask(), ART_LASER_W, ART_LASER_H, ART_LASER_FRAMES);
	bobAdd(BOB_ORB, artOrbCreate(), artOrbCreateMask(), ART_ORB_W, ART_ORB_H, ART_ORB_FRAMES);
	bobAdd(BOB_CAPSULE, artCapsuleCreate(), artCapsuleCreateMask(), ART_CAPSULE_W, ART_CAPSULE_H, ART_CAPSULE_FRAMES);
	bobAdd(BOB_ROCK, artRockCreate(), artRockCreateMask(), ART_ROCK_W, ART_ROCK_H, ART_ROCK_FRAMES);
	bobAdd(BOB_PEBBLE, artPebbleCreate(), artPebbleCreateMask(), ART_PEBBLE_W, ART_PEBBLE_H, ART_PEBBLE_FRAMES);
	bobAdd(BOB_MINE, artMineCreate(), artMineCreateMask(), ART_MINE_W, ART_MINE_H, ART_MINE_FRAMES);
	bobAdd(BOB_BOOM, artBoomCreate(), artBoomCreateMask(), ART_BOOM_W, ART_BOOM_H, ART_BOOM_FRAMES);
	bobAdd(BOB_POP, artPopCreate(), artPopCreateMask(), ART_POP_W, ART_POP_H, ART_POP_FRAMES);
	bobAdd(BOB_BOSS, artBossCreate(), artBossCreateMask(), ART_BOSS_W, ART_BOSS_H, ART_BOSS_FRAMES);
	bobAdd(BOB_LOGO, artLogoCreate(), artLogoCreateMask(), ART_LOGO_W, ART_LOGO_H, ART_LOGO_FRAMES);
	bobAdd(BOB_MSG_STAGE, artMsgStageCreate(), artMsgStageCreateMask(), ART_MSG_STAGE_W, ART_MSG_STAGE_H, ART_MSG_STAGE_FRAMES);
	bobAdd(BOB_MSG_WARNING, artMsgWarningCreate(), artMsgWarningCreateMask(), ART_MSG_WARNING_W, ART_MSG_WARNING_H, ART_MSG_WARNING_FRAMES);
	bobAdd(BOB_MSG_CLEAR, artMsgClearCreate(), artMsgClearCreateMask(), ART_MSG_CLEAR_W, ART_MSG_CLEAR_H, ART_MSG_CLEAR_FRAMES);
	bobAdd(BOB_MSG_OVER, artMsgOverCreate(), artMsgOverCreateMask(), ART_MSG_OVER_W, ART_MSG_OVER_H, ART_MSG_OVER_FRAMES);
	bobAdd(BOB_MSG_FIRE, artMsgFireCreate(), artMsgFireCreateMask(), ART_MSG_FIRE_W, ART_MSG_FIRE_H, ART_MSG_FIRE_FRAMES);
	// (logic.c's sizes must be the art's)
	for(UBYTE b = 0; b < BOB_TYPES; ++b) {
		if(s_pBobW[b] != g_pBobW[b] || s_pBobH[b] != g_pBobH[b] || s_pBobFrames[b] != g_pBobFrames[b]) {
			agkPrint("AGK ERR logic.c's BOB sizes don't match the art\n");
		}
	}
	// A blit template per frame (cookie cut, all 3 interleaved planes at once)
	UBYTE t = 0;
	for(UBYTE b = 0; b < BOBS; ++b) {
		s_pTplFirst[b] = t;
		for(UBYTE f = 0; f < s_pBobFrames[b]; ++f, ++t) {
			// Shots and bullets - most of what's on screen - are small and
			// nearly solid: a plain copy (A -> D) costs half a cookie cut
			// (A, B, C -> D); their corners blank a pixel or two of what they
			// cross, which nobody sees
			UBYTE isCopy = b == BOB_LASER || b == BOB_ORB;
			agkBlitBob(s_pTpl[t], s_pBobBm[b], isCopy ? 0 : s_pBobMask[b], s_pBobW[b], s_pBobH[b], f, FB_PLANE_BYTES);
		}
	}
	if(t > TPL_MAX) agkPrint("AGK ERR too many BOB frames: raise TPL_MAX\n");
	for(UWORD y = 0; y < FB_H; ++y) s_pFbRowOffs[y] = (UWORD)(y * FB_ROW_BYTES);
}

// The Warden is big (96x64): cookie-cut and erased every frame it took a
// fifth of the bus. It's drawn as a plain copy (A -> D, half the accesses)
// of a padded image instead: 16 px of blank each side and 4 rows above and
// below wipe whatever it moved away from, so it needs no erase. (Nothing is
// drawn under it: it covers its whole rectangle.)
#define BOSS_PAD_X 16
#define BOSS_PAD_Y 8
#define BOSS_PAD_W (ART_BOSS_W + 2 * BOSS_PAD_X)
#define BOSS_PAD_H (ART_BOSS_H + 2 * BOSS_PAD_Y)
static tBitMap *s_pBossPad[ART_BOSS_FRAMES];
static tAgkBlit s_pBossTpl[ART_BOSS_FRAMES][2];
static tAgkBlitArea s_pBossErase[2];                // per buffer: where it was, to clear once it's gone
static UBYTE s_pBossShown[2];

static void bossPadCreate(void) {
	const tBitMap *pBm = s_pBobBm[BOB_BOSS];
	UWORD uwSrcPlane = ART_BOSS_BITMAP_W / 8, uwDstPlane = BOSS_PAD_W / 8;
	for(UBYTE f = 0; f < ART_BOSS_FRAMES; ++f) {
		s_pBossPad[f] = bitmapCreate(BOSS_PAD_W, BOSS_PAD_H, FB_BPP, BMF_CLEAR | BMF_INTERLEAVED);
		for(UWORD r = 0; r < ART_BOSS_H; ++r) {
			for(UBYTE p = 0; p < FB_BPP; ++p) {
				const UBYTE *pSrc = pBm->Planes[0] + (f * ART_BOSS_H + r) * pBm->BytesPerRow + p * uwSrcPlane;
				UBYTE *pDst = s_pBossPad[f]->Planes[0] + (r + BOSS_PAD_Y) * s_pBossPad[f]->BytesPerRow +
				              p * uwDstPlane + BOSS_PAD_X / 8;
				for(UWORD b = 0; b < uwSrcPlane; ++b) pDst[b] = pSrc[b];
			}
		}
		for(UBYTE e = 0; e < 2; ++e) {
			UWORD uwWords = BOSS_PAD_W / 16 + e;
			tAgkBlit *pT = &s_pBossTpl[f][e];
			pT->uwCon0 = USEA | USED | 0xF0;      // D = A (+ shift)
			pT->uwCon1 = 0;
			pT->uwAlwm = e ? 0x0000 : 0xFFFF;
			pT->pA = s_pBossPad[f]->Planes[0];
			pT->pB = 0;
			pT->pCD = 0;
			pT->wSrcMod = (WORD)(uwDstPlane - uwWords * 2);
			pT->wDstMod = (WORD)(FB_PLANE_BYTES - uwWords * 2);
			pT->uwSize = (UWORD)(((BOSS_PAD_H * FB_BPP) << 6) | uwWords);
		}
	}
}

static tDraw s_pDraw[DRAWS_MAX];

static UBYTE messagesDraw(tDraw *pOut) {
	// the middle of the screen: what logic.c says to show
	UBYTE n = 0;
	const tGame *g = &s_sGame;
	switch(g->message) {
		case MSG_TITLE:
			pOut[n++] = (tDraw){.x = (SCREEN_W - ART_LOGO_W) / 2, .y = 72, .bob = BOB_LOGO};
			if(g->frame & 32) pOut[n++] = (tDraw){.x = (SCREEN_W - ART_MSG_FIRE_W) / 2, .y = 150, .bob = BOB_MSG_FIRE};
			break;
		case MSG_STAGE:
			pOut[n++] = (tDraw){.x = (SCREEN_W - ART_MSG_STAGE_W) / 2, .y = 104, .bob = BOB_MSG_STAGE};
			break;
		case MSG_WARNING:
			if(g->frame & 16) pOut[n++] = (tDraw){.x = (SCREEN_W - ART_MSG_WARNING_W) / 2, .y = 112,
			                                     .bob = BOB_MSG_WARNING, .frame = (UBYTE)((g->frame >> 3) & 1)};
			break;
		case MSG_CLEAR:
			pOut[n++] = (tDraw){.x = (SCREEN_W - ART_MSG_CLEAR_W) / 2, .y = 116, .bob = BOB_MSG_CLEAR};
			break;
		case MSG_OVER:
			pOut[n++] = (tDraw){.x = (SCREEN_W - ART_MSG_OVER_W) / 2, .y = 116, .bob = BOB_MSG_OVER};
			break;
	}
	return n;
}

// Erase what this buffer showed last time: queued and started first thing
// in the frame (they don't depend on the game), so the blitter works while
// the game logic runs
static void erasesQueue(UBYTE ubBfr) {
	agkBlitqReset();
	const tAgkBlitArea *pDrawn = s_pErase[ubBfr];
	for(UBYTE i = s_pErased[ubBfr]; i--; ++pDrawn) {
		agkBlitClear(agkBlitqSlot(), pDrawn);   // the same place, D only: zeros
		agkBlitqPush();
	}
}

static void objectsQueue(UBYTE ubBfr) {
	tBitMap *pFb = s_pFb[ubBfr];
	UBYTE n = logicDraw(&s_sGame, s_pDraw);
	n += messagesDraw(&s_pDraw[n]);
	tAgkBlitArea *pErase = s_pErase[ubBfr];
	UBYTE ubDrawn = 0, isBoss = 0;
	const tDraw *pD = s_pDraw;
	for(UBYTE i = n; i--; ++pD) {
		UBYTE b = pD->bob;
		if(b == BOB_BOSS) {
			WORD x = pD->x - BOSS_PAD_X + FB_X0, y = pD->y - BOSS_PAD_Y + FB_Y0;
			if(x < 0 || y < 0 || x + BOSS_PAD_W + 16 > FB_W || y + BOSS_PAD_H > FB_H) continue;
			tAgkBlit *pB = agkBlitqSlot();
			agkBlitPlace(pB, s_pBossTpl[pD->frame], BOSS_PAD_W, pFb->Planes[0] + s_pFbRowOffs[y], (UWORD)x);
			agkBlitArea(&s_pBossErase[ubBfr], pB);
			agkBlitqPush();
			isBoss = 1;
			continue;
		}
		WORD x = pD->x + FB_X0, y = pD->y + FB_Y0;
		if(x < 0 || y < 0 || x + s_pBobW[b] > FB_W || y + s_pBobH[b] > FB_H) continue;
		tAgkBlit *pB = agkBlitqSlot();
		agkBlitPlace(pB, s_pTpl[s_pTplFirst[b] + pD->frame], s_pBobW[b], pFb->Planes[0] + s_pFbRowOffs[y], (UWORD)x);
		agkBlitArea(pErase++, pB);
		agkBlitqPush();                    // (starts it, or the next one, if the blitter is free)
		++ubDrawn;
	}
	if(!isBoss && s_pBossShown[ubBfr]) {
		// the Warden is gone: clear where this buffer last showed it
		agkBlitClear(agkBlitqSlot(), &s_pBossErase[ubBfr]);
		agkBlitqPush();
	}
	s_pBossShown[ubBfr] = isBoss;
	s_pErased[ubBfr] = ubDrawn;
}

// ------------------------------------------------------------------ HUD ---
// One line of text in the HUD bitmap (agk/text.h): letters in colour 5
// (cyan), their shadow in colour 1. Only the letters that changed are drawn.

#define HUD_LEN 40
static tAgkFont s_sFont;
static tAgkText s_pHudText[2];

static void hudInit(void) {
	agkFontInit(&s_sFont, g_pAgkFont8, AGK_FONT8_CHARS);
	for(UBYTE b = 0; b < 2; ++b) agkTextInit(&s_pHudText[b], &s_sFont, s_pHud[b], 16, 8, HUD_LEN, 5, 1);
}

static void hudUpdate(UBYTE ubBfr) {
	// per buffer: what it shows (a change is drawn into each buffer in turn)
	static ULONG pScore[2] = {~0UL, ~0UL}, pHi[2] = {~0UL, ~0UL};
	static UBYTE pLives[2] = {0xFF, 0xFF}, pWeapon[2] = {0xFF, 0xFF}, pPhase[2] = {0xFF, 0xFF}, pDemo[2] = {0xFF, 0xFF};
	const tGame *g = &s_sGame;
	if(g->score == pScore[ubBfr] && g->hiScore == pHi[ubBfr] && g->lives == pLives[ubBfr] &&
	   g->weapon == pWeapon[ubBfr] && g->phase == pPhase[ubBfr] && g->isDemo == pDemo[ubBfr]) {
		return;
	}
	pScore[ubBfr] = g->score; pHi[ubBfr] = g->hiScore; pLives[ubBfr] = g->lives;
	pWeapon[ubBfr] = g->weapon; pPhase[ubBfr] = g->phase; pDemo[ubBfr] = g->isDemo;
	//               0123456789012345678901234567890123456789
	char szLine[] = "SCORE 0000000  HI 0000000  SHIPS 0 PWR 0";
	agkTextNumber(&szLine[6], g->score, 7);
	agkTextNumber(&szLine[18], g->hiScore, 7);
	szLine[33] = '0' + (char)g->lives;
	szLine[39] = '1' + (char)g->weapon;
	if(g->phase == PHASE_TITLE) {
		for(UBYTE i = 0; i < 15; ++i) szLine[i] = ' ';
		for(UBYTE i = 26; i < HUD_LEN; ++i) szLine[i] = ' ';
	}
	else if(g->isDemo) {
		const char *szDemo = "DEMO          ";
		for(UBYTE i = 0; i < 14; ++i) szLine[i] = szDemo[i];
		const char *szFire = "   PRESS FIRE";
		for(UBYTE i = 0; i < 13; ++i) szLine[27 + i] = szFire[i];
	}
	agkTextSet(&s_pHudText[ubBfr], szLine);
}

// ---------------------------------------------------------------- ship ---

// Two sets of the ship's sprite bitmaps, one per copper buffer: the back
// set gets the new position (the control words) and the back copper list
// points the 4 channels at it (slots 2c, 2c + 1: the sprite manager's).
static tBitMap *s_pShipFrames[2][ART_SHIP_FRAMES][ART_SHIP_PARTS];

static void shipCreate(void) {
	for(UBYTE b = 0; b < 2; ++b)
		for(UBYTE f = 0; f < ART_SHIP_FRAMES; ++f)
			for(UBYTE p = 0; p < ART_SHIP_PARTS; ++p) s_pShipFrames[b][f][p] = artShipCreate(f, p);
}

static void shipUpdate(UBYTE ubBfr) {
	const tGame *g = &s_sGame;
	UBYTE ubFrame = logicShipFrame(g);
	WORD wX = (WORD)((g->px >> 4) - PLAYER_W / 2), wY = (WORD)((g->py >> 4) - PLAYER_H / 2);
	if(!logicShipVisible(g)) wY = -64;     // (above the display: nothing to show)
	UWORD uwVStart = (UWORD)(s_pView->ubPosY + wY), uwVStop = uwVStart + PLAYER_H;
	tCopCmd *pList = s_pView->pCopList->pBackBfr->pList;
	for(UBYTE p = 0; p < ART_SHIP_PARTS; ++p) {
		tBitMap *pBm = s_pShipFrames[ubBfr][ubFrame][p];
		UWORD uwHStart = (UWORD)(s_pView->ubPosX - 1 + wX + (p >> 1) * 16);
		UWORD *pHead = (UWORD *)pBm->Planes[0];
		pHead[0] = (UWORD)((uwVStart << 8) | ((uwHStart >> 1) & 0xFF));
		pHead[1] = (UWORD)((uwVStop << 8) | ((p & 1) << 7) | (((uwVStart >> 8) & 1) << 2) |
		                   (((uwVStop >> 8) & 1) << 1) | (uwHStart & 1));   // (bit 7: attached, odd parts)
		ULONG ulPtr = (ULONG)pBm->Planes[0];
		copSetMoveVal(&pList[COP_SPRITES_POS + 2 * (ART_SHIP_CHANNEL + p)].sMove, ulPtr >> 16);
		copSetMoveVal(&pList[COP_SPRITES_POS + 2 * (ART_SHIP_CHANNEL + p) + 1].sMove, ulPtr & 0xFFFF);
	}
}

// ---------------------------------------------------------------- sound ---

static void playSounds(UBYTE ubMsgBefore) {
	// one effect a frame on the effects channel: the most important
	const tGame *g = &s_sGame;
	if(g->evPlayerDied) soundPlay(SOUND_SFX_DIE);
	else if(g->message == MSG_WARNING && ubMsgBefore != MSG_WARNING) soundPlay(SOUND_SFX_WARNING);
	else if(g->evBigKill) soundPlay(SOUND_SFX_BOOM);
	else if(g->evPowerUp) soundPlay(SOUND_SFX_POWERUP);
	else if(g->evKill) soundPlay(SOUND_SFX_POP);
	else if(g->evBossHit) soundPlay(SOUND_SFX_BOSSHIT);
	else if(g->evHit) soundPlay(SOUND_SFX_HIT);
	else if(g->evShot) soundPlay(SOUND_SFX_SHOT);
}

// ---------------------------------------------------------------- setup ---

static void readInput(tInput *pIn) {
	// Joystick in port 2 (ACE JOY1): move, fire
	pIn->dx = joyCheck(JOY1 + JOY_LEFT) ? -1 : joyCheck(JOY1 + JOY_RIGHT) ? 1 : 0;
	pIn->dy = joyCheck(JOY1 + JOY_UP) ? -1 : joyCheck(JOY1 + JOY_DOWN) ? 1 : 0;
	pIn->fire = joyCheck(JOY1 + JOY_FIRE);
}

void genericCreate(void) {
	agkDebugInit();
	agkPrint("AGK boot voidrunner\n");
	keyCreate();
	joyOpen();

	s_pView = viewCreate(0,
		TAG_VIEW_GLOBAL_PALETTE, 1,
		TAG_VIEW_COPLIST_MODE, VIEW_COPLIST_MODE_RAW,
		TAG_VIEW_COPLIST_RAW_COUNT, COP_RAW_COUNT,
		TAG_DONE);
	s_pVPort = vPortCreate(0, TAG_VPORT_VIEW, s_pView, TAG_VPORT_BPP, 5, TAG_DONE);

	for(UBYTE b = 0; b < 2; ++b) s_pFb[b] = bitmapCreate(FB_W, FB_H, FB_BPP, BMF_CLEAR | BMF_INTERLEAVED);
	for(UBYTE b = 0; b < 2; ++b) s_pHud[b] = bitmapCreate(HUD_W, HUD_H, FB_BPP, BMF_CLEAR | BMF_INTERLEAVED);
	s_pBlankRow = memAllocChipClear(FETCH_BYTES);
	s_pBackdrop = artBackdropCreate();
	bobsCreate();
	bossPadCreate();
	hudInit();

	logicInit(&s_sGame);
	s_sGame.phaseFrames = TITLE_WAIT;   // (no fire held over at boot)
	hudUpdate(0);
	hudUpdate(1);

	spriteManagerCreate(s_pView, COP_SPRITES_POS, 0);
	systemSetDmaBit(DMAB_SPRITE, 1);
	artShipApplyColors(s_pVPort->pPalette);   // colours 17-31 (29-31 are the stars')
	artPaletteApply(s_pVPort->pPalette);      // PF1 and PF2 colours
	shipCreate();

	starsCreate();
	tCopList *pCopList = s_pView->pCopList;
	s_pCopBfrA = pCopList->pBackBfr;
	copperWriteList(pCopList->pBackBfr->pList, 0);
	copperWriteList(pCopList->pFrontBfr->pList, 1);

	soundCreate();
	soundMusicStart(SOUND_MUSIC_THEME);

	viewLoad(s_pView);
	systemUnuse();
	agkPerfSetFrameVbls(FRAME_VBLS);
	agkDebugAsync(1);
}

// ------------------------------------------------------------ the frame ---

void genericProcess(void) {
	agkPerfBegin();
	s_ulFrameVbl = timerGet();
	keyProcess();
	joyProcess();
	if(keyCheck(KEY_ESCAPE)) {
		gameExit();
		return;
	}
	tInput sIn;
	readInput(&sIn);
	UBYTE ubBfr = backBuffer();
	erasesQueue(ubBfr);
	UBYTE ubPhaseBefore = s_sGame.phase, ubLivesBefore = s_sGame.lives, ubMsgBefore = s_sGame.message;
	logicUpdate(&s_sGame, &sIn);
	agkBlitqPoll();
	playSounds(ubMsgBefore);
	objectsQueue(ubBfr);
	agkBlitqPoll();
	agkStarsUpdate(&s_sStars, ubBfr, (UWORD)s_sGame.frame);
	agkBlitqPoll();
	backdropUpdate(ubBfr);
	hudUpdate(ubBfr);
	agkBlitqPoll();
	shipUpdate(ubBfr);

	// State for tests: on events, and every 32 frames
	const tGame *g = &s_sGame;
	if(g->phase != ubPhaseBefore || g->lives != ubLivesBefore || g->evKill || g->evPowerUp ||
	   !(g->frame & 31) || g->frame <= 3) {
		agkState("frame", g->frame);
		agkState("phase", g->phase);
		agkState("level", (LONG)g->levelFrame);
		agkState("score", (LONG)g->score);
		agkState("lives", g->lives);
		agkState("weapon", g->weapon);
		agkState("kills", g->kills);
		agkState("x", g->px >> 4);
		agkState("y", g->py >> 4);
		agkState("boss", g->boss.state);
		agkState("bosshp", g->boss.hp);
		agkEnd();
	}

	agkBlitqFinish();
	copProcessBlocks();
	agkPerfEnd();
	while((UWORD)(timerGet() - s_ulFrameVbl) < FRAME_VBLS) continue;

	if(g->frame == 2) {
		agkState("copper_used", s_uwCopUsed);
		agkState("copper_max", COP_RAW_COUNT);
		agkEnd();
		agkReady();
	}
	if(g->frame == SYNC_FRAME) agkPrint("AGK t0\n");
}

void genericDestroy(void) {
	agkDebugAsync(0);
	agkBlitqFinish();
	systemUse();
	soundDestroy();
	systemSetDmaBit(DMAB_SPRITE, 0);
	spriteManagerDestroy();
	for(UBYTE b = 0; b < 2; ++b)
		for(UBYTE f = 0; f < ART_SHIP_FRAMES; ++f)
			for(UBYTE p = 0; p < ART_SHIP_PARTS; ++p) bitmapDestroy(s_pShipFrames[b][f][p]);
	for(UBYTE f = 0; f < ART_BOSS_FRAMES; ++f) bitmapDestroy(s_pBossPad[f]);
	for(UBYTE b = 0; b < BOBS; ++b) {
		bitmapDestroy(s_pBobBm[b]);
		bitmapDestroy(s_pBobMask[b]);
	}
	agkStarsDestroy(&s_sStars);
	bitmapDestroy(s_pBackdrop);
	for(UBYTE b = 0; b < 2; ++b) bitmapDestroy(s_pHud[b]);
	memFree(s_pBlankRow, FETCH_BYTES);
	for(UBYTE b = 0; b < 2; ++b) bitmapDestroy(s_pFb[b]);
	viewDestroy(s_pView);
	joyClose();
	keyDestroy();
}
