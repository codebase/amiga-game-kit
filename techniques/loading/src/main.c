// loading - the Amiga side: levels as packed files on the floppy, loaded
// when needed. Each level here is a 40 KB picture (disk/levelN.pln, 4
// bitplanes interleaved, the display's own format), packed by the build to
// about a quarter of that, read through AmigaDOS and unpacked straight into
// the display bitmap. The executable holds no level at all.

#include <ace/generic/main.h>
#include <ace/managers/joy.h>
#include <ace/managers/key.h>
#include <ace/managers/timer.h>
#include <ace/managers/viewport/simplebuffer.h>
#include <ace/utils/custom.h>
#include <ace/utils/extview.h>
#include <agk/debug.h>
#include <agk/pack.h>
#include <agk/perf.h>
#include <agk/text.h>
#include "art.h"
#include "logic.h"

#define SCREEN_W 320
#define SCREEN_H 256
#define PICTURE_BYTES (SCREEN_W / 8 * SCREEN_H * ART_DEPTH)
#define COLOR_TEXT 15
#define COLOR_SHADOW 1

static tView *s_pView;
static tVPort *s_pVPort;
static tSimpleBufferManager *s_pBuffer;
static tGameState s_sState;
static tAgkFont s_sFont;
static tAgkText s_sText;
static const char *s_pFiles[LEVELS] = {"level1.pln.pk", "level2.pln.pk"};

// "LOADING LEVEL N" over a cleared screen, then the file unpacked into the
// display bitmap itself. Printed for the tests: what was loaded, its size
// and how long it took, in frames (50 a second).
static void levelLoad(UBYTE ubLevel) {
	tBitMap *pBm = s_pBuffer->pBack;
	blitRect(pBm, 0, 0, SCREEN_W, SCREEN_H, 0);
	char szMsg[] = "LOADING LEVEL 1";
	szMsg[14] = (char)('0' + ubLevel);
	agkTextInit(&s_sText, &s_sFont, pBm, 112, 120, 15, COLOR_TEXT, COLOR_SHADOW);
	agkTextSet(&s_sText, szMsg);
	ULONG ulStart = timerGet();
	ULONG ulSize = agkLoadFile(s_pFiles[ubLevel - 1], pBm->Planes[0], PICTURE_BYTES);
	ULONG ulFrames = timerGet() - ulStart;
	agkPerfSkip();                           // the frame meter: this wasn't dropped frames
	if(ulSize) {
		agkPrint("AGK loaded ");
		agkPrint(s_pFiles[ubLevel - 1]);
		agkPrint("\n");
		agkState("size", (LONG)ulSize);
		agkState("frames", (LONG)ulFrames);
		agkState("level", ubLevel);
		agkEnd();
	}
	s_sState.wantLoad = 0;
}

static void readInput(tInput *pInput) {
	pInput->dx = 0;
	pInput->dy = 0;
	pInput->fire = joyCheck(JOY1 + JOY_FIRE);
}

void genericCreate(void) {
	agkDebugInit();
	agkPrint("AGK boot loading\n");
	keyCreate();
	joyOpen();
	s_pView = viewCreate(0, TAG_VIEW_GLOBAL_PALETTE, 1, TAG_DONE);
	s_pVPort = vPortCreate(0, TAG_VPORT_VIEW, s_pView, TAG_VPORT_BPP, ART_DEPTH, TAG_DONE);
	s_pBuffer = simpleBufferCreate(0,
		TAG_SIMPLEBUFFER_VPORT, s_pVPort,
		TAG_SIMPLEBUFFER_BITMAP_FLAGS, BMF_CLEAR | BMF_INTERLEAVED,   // the pictures' format
		TAG_DONE);
	artPaletteApply(s_pVPort->pPalette);
	agkFontInit(&s_sFont, g_pAgkFont8, AGK_FONT8_CHARS);
	logicInit(&s_sState);
	viewLoad(s_pView);
	systemUnuse();
	agkDebugAsync(1);
	levelLoad(s_sState.level);               // (the OS comes back for the read; ACE handles that)
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
	if(logicUpdate(&s_sState, &sInput)) {
		levelLoad(s_sState.level);
	}
	if(s_sState.frame < 3 || (s_sState.frame & 63) == 0) {
		agkState("frame", s_sState.frame);
		agkState("level", s_sState.level);
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
	viewDestroy(s_pView);
	joyClose();
	keyDestroy();
}
