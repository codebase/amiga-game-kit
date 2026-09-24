// scrolling - Amiga side: a tile-map level scrolled with ACE's tile buffer
// manager, a hardware sprite player, and the rules from logic.c.
//
// The tile buffer keeps a bitmap only slightly bigger than the screen. The
// copper points the display at the right place in it (coarse scroll, 16 px
// steps) and BPLCON1 delays it by 0..15 px (fine scroll). When the camera
// moves, ACE blits just the column of tiles that is about to scroll into
// view, one tile per frame, off-screen. See TECHNIQUE.md.

#include <ace/generic/main.h>
#include <ace/managers/blit.h>
#include <ace/managers/joy.h>
#include <ace/managers/key.h>
#include <ace/managers/sprite.h>
#include <ace/managers/viewport/tilebuffer.h>
#include <ace/utils/custom.h>
#include <ace/utils/extview.h>
#include <hardware/dmabits.h>
#include <agk/debug.h>
#include <agk/perf.h>
#include "logic.h"

// Build-time knobs, used for the cost measurements in TECHNIQUE.md.
#ifndef SCROLL_BPP
#define SCROLL_BPP 4        // 16 colours
#endif
#ifndef SCROLL_DBLBUF
#define SCROLL_DBLBUF 1     // double buffering (what you need once you add BOBs)
#endif

// Palette. Colour 0 is never used by tiles, so a black stripe would reveal
// any part of the buffer that ACE didn't draw.
#define C_SKY 1
#define C_WHITE 2
#define C_GRASS 3
#define C_GRASS_DARK 4
#define C_DIRT 5
#define C_DIRT_DARK 6
#define C_BRICK 7
#define C_STONE_LIGHT 8
#define C_STONE_DARK 9
#define C_STONE 10
#define C_RED 11
#define C_GREEN 12
#define C_BLUE 13
#define C_POST 14

static const UWORD s_pPalette[16] = {
	0x000, 0x6AE, 0xFFF, 0x3B3, 0x272, 0x853, 0x631, 0xC62,
	0x999, 0x666, 0xAAB, 0xE22, 0x2D2, 0x22E, 0x642, 0x000,
};

static tView *s_pView;
static tVPort *s_pVPort;
static tTileBufferManager *s_pTileBuffer;
static tBitMap *s_pTileset;
static tBitMap *s_pPlayerBm;
static tSprite *s_pPlayer;
static tGameState s_sState;

// --- Tileset: all tiles in one 16-px-wide column, tile i at y = i * 16 ------

static void tileRect(UBYTE ubTile, UBYTE x, UBYTE y, UBYTE w, UBYTE h, UBYTE ubColor) {
	blitRect(s_pTileset, x, ubTile * TILE_SIZE + y, w, h, ubColor);
}

static void drawSign(UBYTE ubTile, UBYTE ubColor) {
	tileRect(ubTile, 0, 0, 16, 16, C_SKY);
	tileRect(ubTile, 0, 0, 16, 8, ubColor);   // board
	tileRect(ubTile, 0, 0, 1, 8, C_WHITE);    // 1-px white left edge: tests
	                                          // find it to check the scroll offset
	tileRect(ubTile, 7, 8, 2, 8, C_POST);     // post
}

static void createTileset(void) {
	// Same depth as the display, and interleaved like the buffer: then ACE
	// blits a whole tile (all planes) with one blit.
	s_pTileset = bitmapCreate(
		TILE_SIZE, TILE_SIZE * TILE_COUNT, SCROLL_BPP, BMF_CLEAR | BMF_INTERLEAVED
	);

	tileRect(TILE_SKY, 0, 0, 16, 16, C_SKY);

	tileRect(TILE_CLOUD, 0, 0, 16, 16, C_SKY);
	tileRect(TILE_CLOUD, 1, 6, 14, 6, C_WHITE);
	tileRect(TILE_CLOUD, 4, 3, 7, 3, C_WHITE);

	tileRect(TILE_GRASS, 0, 0, 16, 16, C_DIRT);
	tileRect(TILE_GRASS, 0, 0, 16, 4, C_GRASS);
	tileRect(TILE_GRASS, 0, 4, 16, 1, C_GRASS_DARK);
	tileRect(TILE_GRASS, 3, 9, 2, 2, C_DIRT_DARK);
	tileRect(TILE_GRASS, 11, 12, 2, 2, C_DIRT_DARK);

	tileRect(TILE_DIRT, 0, 0, 16, 16, C_DIRT);
	tileRect(TILE_DIRT, 3, 4, 2, 2, C_DIRT_DARK);
	tileRect(TILE_DIRT, 10, 9, 2, 2, C_DIRT_DARK);
	tileRect(TILE_DIRT, 6, 13, 2, 1, C_DIRT_DARK);

	tileRect(TILE_BRICK, 0, 0, 16, 16, C_BRICK);
	tileRect(TILE_BRICK, 0, 0, 16, 1, C_STONE_LIGHT);
	tileRect(TILE_BRICK, 0, 8, 16, 1, C_STONE_LIGHT);
	tileRect(TILE_BRICK, 0, 1, 1, 7, C_STONE_LIGHT);
	tileRect(TILE_BRICK, 8, 9, 1, 7, C_STONE_LIGHT);

	tileRect(TILE_COLUMN, 0, 0, 16, 16, C_STONE);
	tileRect(TILE_COLUMN, 0, 0, 2, 16, C_STONE_LIGHT);
	tileRect(TILE_COLUMN, 14, 0, 2, 16, C_STONE_DARK);

	tileRect(TILE_COLUMN_TOP, 0, 0, 16, 16, C_STONE);
	tileRect(TILE_COLUMN_TOP, 0, 4, 2, 12, C_STONE_LIGHT);
	tileRect(TILE_COLUMN_TOP, 14, 4, 2, 12, C_STONE_DARK);
	tileRect(TILE_COLUMN_TOP, 0, 0, 16, 3, C_STONE_LIGHT);
	tileRect(TILE_COLUMN_TOP, 0, 3, 16, 1, C_STONE_DARK);

	drawSign(TILE_SIGN_RED, C_RED);
	drawSign(TILE_SIGN_GREEN, C_GREEN);
	drawSign(TILE_SIGN_BLUE, C_BLUE);
}

static void createPlayerBitmap(void) {
	// Sprites are 16px wide, 2 bitplanes, interleaved, with an empty first and
	// last line where the hardware keeps its control words.
	s_pPlayerBm = bitmapCreate(16, PLAYER_H + 2, 2, BMF_CLEAR | BMF_INTERLEAVED);
	UWORD uwWordsPerRow = s_pPlayerBm->BytesPerRow / 2;
	for(UBYTE y = 0; y < PLAYER_H; ++y) {
		UWORD *pRow = (UWORD *)s_pPlayerBm->Planes[0] + (y + 1) * uwWordsPerRow;
		UBYTE isEdge = (y == 0 || y == PLAYER_H - 1);
		pRow[0] = isEdge ? 0xFFFF : 0x8001;              // plane 0 -> colour 17
		pRow[1] = (y >= 4 && y < 12) ? 0x0FF0 : 0;       // plane 1 -> colour 18
	}
}

static void readInput(tInput *pInput) {
	pInput->dx = 0;
	if(joyCheck(JOY1 + JOY_LEFT) || joyCheck(JOY2 + JOY_LEFT)) pInput->dx = -1;
	if(joyCheck(JOY1 + JOY_RIGHT) || joyCheck(JOY2 + JOY_RIGHT)) pInput->dx = 1;
	pInput->fire = joyCheck(JOY1 + JOY_FIRE) || joyCheck(JOY2 + JOY_FIRE);
}

static void redrawAll(void);

void genericCreate(void) {
	agkDebugInit();
	agkPrint("AGK boot scrolling\n");

	keyCreate();
	joyOpen();

	s_pView = viewCreate(0, TAG_VIEW_GLOBAL_PALETTE, 1, TAG_DONE);
	s_pVPort = vPortCreate(0, TAG_VPORT_VIEW, s_pView, TAG_VPORT_BPP, SCROLL_BPP, TAG_DONE);

	createTileset();
	// The tile buffer creates its scroll buffer and camera managers itself.
	s_pTileBuffer = tileBufferCreate(0,
		TAG_TILEBUFFER_VPORT, s_pVPort,
		TAG_TILEBUFFER_BITMAP_FLAGS, BMF_CLEAR | BMF_INTERLEAVED,
		TAG_TILEBUFFER_BOUND_TILE_X, LEVEL_TILES_W,
		TAG_TILEBUFFER_BOUND_TILE_Y, LEVEL_TILES_H,
		TAG_TILEBUFFER_TILE_SHIFT, TILE_SHIFT,
		TAG_TILEBUFFER_TILESET, s_pTileset,
		TAG_TILEBUFFER_IS_DBLBUF, SCROLL_DBLBUF,
		TAG_TILEBUFFER_REDRAW_QUEUE_LENGTH, 8,   // only for tileBufferSetTile(); mandatory
		TAG_TILEBUFFER_MAX_TILESET_SIZE, TILE_COUNT,
		TAG_DONE
	);

	for(UBYTE i = 0; i < 16; ++i) {
		s_pVPort->pPalette[i] = s_pPalette[i];
	}
	s_pVPort->pPalette[17] = 0xFFF;  // player sprite
	s_pVPort->pPalette[18] = 0xFA0;
	s_pVPort->pPalette[19] = 0x000;

	// pTileData is [x][y] (column-major), allocated by tileBufferCreate.
	for(UWORD x = 0; x < LEVEL_TILES_W; ++x) {
		for(UWORD y = 0; y < LEVEL_TILES_H; ++y) {
			s_pTileBuffer->pTileData[x][y] = levelTileAt(x, y);
		}
	}

	logicInit(&s_sState);
	cameraSetCoord(s_pTileBuffer->pCamera, s_sState.cameraX, 0);

	createPlayerBitmap();
	spriteManagerCreate(s_pView, 0, 0);
	systemSetDmaBit(DMAB_SPRITE, 1);
	s_pPlayer = spriteAdd(0, s_pPlayerBm);

	// Chip RAM used by the scroll buffer(s), for TECHNIQUE.md's cost table.
	tBitMap *pBuf = s_pTileBuffer->pScroll->pBack;
	agkState("buffer w", bitmapGetByteWidth(pBuf) * 8);
	agkState("h", pBuf->Rows);
	agkState("bpp", pBuf->Depth);
	agkState("dblbuf", SCROLL_DBLBUF);
	agkEnd();

	systemUnuse();
	redrawAll();
	viewLoad(s_pView);
	agkDebugAsync(1); // output is sent asynchronously, but formatting a 3-field
	                  // agkState line still costs ~4% of that frame (measured)
}

static void redrawAll(void) {
	// Draws the visible area + margins into both buffers, sets ACE's margin
	// redraw state and both copper lists. It drives the blitter directly, so
	// call it after systemUnuse() and after the camera is at its start position.
	// (AGK's ACE patch makes this safe with fast RAM - see TECHNIQUE.md.)
	tileBufferRedrawAll(s_pTileBuffer);
}

static void scrollProcess(void) {
	cameraSetCoord(s_pTileBuffer->pCamera, s_sState.cameraX, 0);
	// Same work as viewProcessManagers(s_pView), in the order ACE's own tile
	// buffer showcase uses: draw margin tiles into the back buffer, then point
	// the copper at it (and swap), then let the camera remember this position
	// for the buffer. viewProcessManagers() runs scroll before tile, which
	// works too but blits into the buffer currently on screen.
	tileBufferProcess(s_pTileBuffer);
	scrollBufferProcess(s_pTileBuffer->pScroll);
	cameraProcess(s_pTileBuffer->pCamera);
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

	scrollProcess();

	s_pPlayer->wX = worldToScreenX(s_sState.x, s_sState.cameraX);
	s_pPlayer->wY = s_sState.y;
	spriteRequestMetadataUpdate(s_pPlayer);
	spriteProcess(s_pPlayer);
	spriteProcessChannel(0);

	// Report state for tests: when the player stops, plus a heartbeat. (Not
	// every frame while moving, to keep the perf numbers about scrolling.)
	if((isChanged && !s_sState.isMoving) || s_sState.frame % 50 == 0) {
		agkState("frame", s_sState.frame);
		agkState("x", s_sState.x);
		agkState("cam", s_sState.cameraX);
		agkEnd();
	}

	copProcessBlocks();
	agkPerfEnd();
	vPortWaitForEnd(s_pVPort);

	// Copper lists are double-buffered: after two frames our first frame is
	// on screen. Only then tell the harness we're ready.
	if(s_sState.frame == 2) {
		agkReady();
		agkState("x", s_sState.x);
		agkState("cam", s_sState.cameraX);
		agkEnd();
	}
}

void genericDestroy(void) {
	agkDebugAsync(0); // must be off before the OS takes interrupts back
	systemUse();
	systemSetDmaBit(DMAB_SPRITE, 0);
	spriteManagerDestroy();
	bitmapDestroy(s_pPlayerBm);
	viewDestroy(s_pView);          // also destroys the tile/scroll/camera managers
	bitmapDestroy(s_pTileset);     // the tile buffer doesn't own the tileset
	joyClose();
	keyDestroy();
}
