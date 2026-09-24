/**
 * Game rules - plain portable C.
 *
 * Nothing in logic.c may include ACE or Amiga headers: it is compiled for the
 * Amiga *and* for the host so `agk unit` can test it in milliseconds.
 * Keep it deterministic (no timers, no randomness without an explicit seed)
 * and use 16-bit types: the 68000 has no 32-bit multiply/divide instruction,
 * so `int` math on the Amiga silently calls slow library routines.
 */
#ifndef _LOGIC_H_
#define _LOGIC_H_

#include <stdint.h>

#define WORLD_W 320
#define WORLD_H 256
#define PLAYER_W 16
#define PLAYER_H 16
#define PLAYER_SPEED 2
#define WALL_COUNT 3

// --- Copper effects: what colour each scanline gets (the Amiga side, main.c,
// turns these tables into copper WAIT/MOVE instructions).
#define HUD_H 24                        // top band: HUD palette (lines 0..23)
#define SKY_TOP HUD_H                   // first playfield line: palette split
#define SKY_LINES (WORLD_H - SKY_TOP)   // 232 lines of sky gradient
#define SKY_BAND_H 4                    // background colour changes every 4 lines
#define SKY_BANDS (SKY_LINES / SKY_BAND_H) // 58 bands -> 58 background colours
#define BAR_H 16                        // animated raster bar height, in lines
#define BAR_MIN_Y (SKY_TOP + 4)         // bar top at frame 0
#define BAR_MAX_Y (BAR_MIN_Y + ((127 * 13) >> 3)) // lowest bar top (= 234)
#define BAR_PERIOD 256                  // frames for one down-and-up trip

// Palette-split colours: the same registers show different colours above
// and below line SKY_TOP.
#define HUD_COLOR0 0x000   // HUD background
#define HUD_COLOR1 0xFC0   // HUD fill (colour index 1)
#define HUD_COLOR2 0xA50   // HUD edges (colour index 2)
#define PF_COLOR1 0x468    // playfield wall fill (same index 1)
#define PF_COLOR2 0x9BD    // playfield wall edge (same index 2)

typedef struct {
	int16_t x, y, w, h;
} tRect;

typedef struct {
	int8_t dx, dy;  // -1, 0 or 1 per axis
	uint8_t fire;
} tInput;

typedef struct {
	int16_t x, y;       // player top-left, in playfield pixels
	uint16_t frame;     // frames since start
	uint8_t bumps;      // how many times the player walked into a wall
	uint8_t isBlocked;  // blocked on the latest frame
} tGameState;

extern const tRect g_pWalls[WALL_COUNT];

void logicInit(tGameState *pState);

/** Advance one frame. Returns 1 if anything visible changed. */
uint8_t logicUpdate(tGameState *pState, const tInput *pInput);

/**
 * Sky gradient table: the background colour (0xRGB) of each of the SKY_BANDS
 * bands, top to bottom. Every band is a different colour: it walks through
 * key colours one 4-bit channel step at a time. Build it once at startup.
 */
void logicBuildSkyBands(uint16_t pBands[SKY_BANDS]);

/** Raster bar shading: colour of bar line 0..BAR_H-1. */
uint16_t logicBarColor(uint8_t ubLine);

/**
 * Top line of the raster bar at a given frame: a pure function of the frame,
 * bouncing between BAR_MIN_Y and BAR_MAX_Y with period BAR_PERIOD.
 */
int16_t logicBarY(uint16_t uwFrame);

/**
 * Final background colour of playfield line y (SKY_TOP..WORLD_H-1): the bar's
 * shade if the bar (top at barY) covers it, otherwise the sky band's colour.
 */
uint16_t logicLineColor(const uint16_t pBands[SKY_BANDS], int16_t y, int16_t barY);

#endif // _LOGIC_H_
