// Phase 0 spike: copper bars + joystick-driven hardware sprite + serial debug.
// Deterministic by design: no RNG, no timers — state depends only on frame
// count and joystick input, so emulator runs can be compared byte-for-byte.

#include <ace/generic/main.h>
#include <ace/managers/joy.h>
#include <ace/managers/key.h>
#include <ace/managers/sprite.h>
#include <ace/managers/viewport/simplebuffer.h>
#include <ace/utils/custom.h>
#include <ace/utils/extview.h>
#include <hardware/dmabits.h>
#include <exec/execbase.h>
#include <graphics/gfxbase.h>
#include <agk/debug.h>

extern struct ExecBase *SysBase;
extern struct GfxBase *GfxBase;

#define BAR_COUNT 6
#define BAR_HEIGHT 16
#define BAR_TOP 60
#define SPR_H 16
#define SPEED 2

static tView *s_pView;
static tVPort *s_pVPort;
static tSimpleBufferManager *s_pBuffer;
static tBitMap *s_pSprBm;
static tSprite *s_pSpr;
static WORD s_wX = 152, s_wY = 120;
static UWORD s_uwFrame;

static const UWORD s_pBarColors[BAR_COUNT] = {
	0xF00, 0xF80, 0xFF0, 0x0F0, 0x08F, 0x80F
};

static void createSpriteBitmap(void) {
	// 16px wide, 2bpp interleaved, plus empty first/last line for control words.
	s_pSprBm = bitmapCreate(16, SPR_H + 2, 2, BMF_CLEAR | BMF_INTERLEAVED);
	UWORD uwWordsPerRow = s_pSprBm->BytesPerRow / 2;
	for(UBYTE y = 0; y < SPR_H; ++y) {
		UWORD *pRow = (UWORD *)s_pSprBm->Planes[0] + (y + 1) * uwWordsPerRow;
		UWORD uwOuter = (y == 0 || y == SPR_H - 1) ? 0xFFFF : 0x8001;
		pRow[0] = uwOuter;           // plane 0: frame -> color 1
		pRow[1] = (y >= 4 && y < 12) ? 0x0FF0 : 0; // plane 1: core -> color 2
	}
}

void genericCreate(void) {
	agkDebugInit();
	agkPrint("AGK boot hello\n");
	agkState("vblankfreq", SysBase->VBlankFrequency);
	agkState("gfxpal", (GfxBase->DisplayFlags & PAL) ? 1 : 0);
	agkState("acepal", systemIsPal());
	agkEnd();

	keyCreate();
	joyOpen();

	s_pView = viewCreate(0, TAG_VIEW_GLOBAL_PALETTE, 1, TAG_DONE);
	s_pVPort = vPortCreate(0, TAG_VPORT_VIEW, s_pView, TAG_VPORT_BPP, 1, TAG_DONE);
	s_pBuffer = simpleBufferCreate(0,
		TAG_SIMPLEBUFFER_VPORT, s_pVPort,
		TAG_SIMPLEBUFFER_BITMAP_FLAGS, BMF_CLEAR,
		TAG_DONE
	);
	s_pVPort->pPalette[0] = 0x012;
	s_pVPort->pPalette[1] = 0xFFF;
	s_pVPort->pPalette[17] = 0xFFF;
	s_pVPort->pPalette[18] = 0xF0F;
	s_pVPort->pPalette[19] = 0x000;

	// Copper bars: change COLOR00 at the start of each bar, reset after last.
	for(UBYTE i = 0; i <= BAR_COUNT; ++i) {
		tCopBlock *pBlock = copBlockCreate(
			s_pView->pCopList, 1, 0, BAR_TOP + i * BAR_HEIGHT
		);
		copMove(
			s_pView->pCopList, pBlock, &g_pCustom->color[0],
			i < BAR_COUNT ? s_pBarColors[i] : 0x012
		);
	}

	createSpriteBitmap();
	spriteManagerCreate(s_pView, 0, 0);
	systemSetDmaBit(DMAB_SPRITE, 1);
	s_pSpr = spriteAdd(0, s_pSprBm);
	s_pSpr->wX = s_wX;
	s_pSpr->wY = s_wY;
	spriteRequestMetadataUpdate(s_pSpr);

	viewLoad(s_pView);
	systemUnuse();
}

void genericProcess(void) {
	keyProcess();
	joyProcess();

	if(keyCheck(KEY_ESCAPE)) {
		gameExit();
	}

	// Joystick in port 2 = ACE JOY1 (JOY2 is the mouse port: don't read it).
	WORD wDx = 0, wDy = 0;
	if(joyCheck(JOY1 + JOY_LEFT)) wDx -= SPEED;
	if(joyCheck(JOY1 + JOY_RIGHT)) wDx += SPEED;
	if(joyCheck(JOY1 + JOY_UP)) wDy -= SPEED;
	if(joyCheck(JOY1 + JOY_DOWN)) wDy += SPEED;
	s_wX += wDx;
	s_wY += wDy;
	if(s_wX < 0) s_wX = 0;
	if(s_wX > 320 - 16) s_wX = 320 - 16;
	if(s_wY < 0) s_wY = 0;
	if(s_wY > 256 - SPR_H) s_wY = 256 - SPR_H;

	s_pSpr->wX = s_wX;
	s_pSpr->wY = s_wY;
	spriteRequestMetadataUpdate(s_pSpr);
	spriteProcess(s_pSpr);
	spriteProcessChannel(0);

	// Machine-readable state line whenever the sprite moves (and every
	// 50 frames as a heartbeat) so tests can assert on game state.
	if(wDx || wDy || s_uwFrame % 50 == 0) {
		agkState("frame", s_uwFrame);
		agkState("x", s_wX);
		agkState("y", s_wY);
		agkEnd();
	}
	++s_uwFrame;

	copProcessBlocks();
	vPortWaitForEnd(s_pVPort);

	// Copper lists are double-buffered: after two frames the display shows
	// our first real frame. Only then tell the harness we're ready.
	if(s_uwFrame == 2) {
		agkReady();
	}
}

void genericDestroy(void) {
	systemUse();
	systemSetDmaBit(DMAB_SPRITE, 0);
	spriteManagerDestroy();
	bitmapDestroy(s_pSprBm);
	viewDestroy(s_pView);
	joyClose();
	keyDestroy();
}
