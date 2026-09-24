/**
 * Game rules - plain portable C.
 *
 * Nothing in logic.c may include ACE or Amiga headers: it is compiled for the
 * Amiga *and* for the host so `agk unit` can test it in milliseconds.
 * Keep it deterministic and use 16-bit types: the 68000 has no 32-bit
 * multiply/divide instruction, so `int` math on the Amiga silently calls slow
 * library routines.
 */
#ifndef _LOGIC_H_
#define _LOGIC_H_

#include <stdint.h>

// Screen (one PAL lores playfield)
#define SCREEN_W 320
#define SCREEN_H 256

// Level: 80x16 tiles of 16x16 px = 1280x256 px (the front playfield, PF1)
#define TILE_SIZE 16
#define TILE_SHIFT 4
#define LEVEL_TILES_W 80
#define LEVEL_TILES_H 16
#define LEVEL_W (LEVEL_TILES_W * TILE_SIZE)
#define LEVEL_H (LEVEL_TILES_H * TILE_SIZE)
#define CAM_MAX (LEVEL_W - SCREEN_W)

// Tile ids in the level map (tileset frame = id - 1)
#define TILE_EMPTY 0
#define TILE_GRASS 1  // solid
#define TILE_DIRT 2   // solid
#define TILE_STONE 3  // one-way platform: you land on its top, jump through from below

// Player: a 16x16 sprite; collision box is columns 3..12, full height
#define PLAYER_W 16
#define PLAYER_H 16
#define PLAYER_HB_L 3
#define PLAYER_HB_R 12
#define PLAYER_SPEED 2       // px per frame
#define START_X 32
#define START_Y (13 * TILE_SIZE - PLAYER_H)   // standing on the ground (row 13)

// Vertical motion in fixed point: 1/16 px (4 fractional bits), int16_t
#define FIX_SHIFT 4
#define GRAVITY 6            // 0.375 px/frame^2
#define JUMP_VEL 104         // 6.5 px/frame upwards
#define MAX_FALL_VEL 112     // 7 px/frame: < TILE_SIZE, and we step pixel by pixel anyway

// Walk animation: art/player.txt frames 0..3, frame 0 = standing
#define WALK_FRAMES 4
#define WALK_FRAME_TICKS 6

// Parallax bands (back playfield, PF2). Game y of each band's first line.
#define MOUNTAINS_Y 39
#define MOUNTAINS_H 96
#define GAP_Y (MOUNTAINS_Y + MOUNTAINS_H)     // 135: one line of blank PF2 to reload the palette
#define HILLS_Y (GAP_Y + 1)                   // 136
#define HILLS_H 80
#define BANDS_END (HILLS_Y + HILLS_H)          // 216: below this PF2 is blank
#define MOUNTAINS_SHIFT 2   // camera / 4
#define HILLS_SHIFT 1       // camera / 2

// Sky gradient (COLOR00): one colour every SKY_STEP lines from 0 to HILLS_Y
#define SKY_STEP 4
#define SKY_BANDS (HILLS_Y / SKY_STEP)         // 34
// Behind the hills: pale snowfield haze, one colour every HAZE_STEP lines
#define HAZE_STEP 8
#define HAZE_BANDS ((BANDS_END - HILLS_Y) / HAZE_STEP)  // 10
#define PIT_COLOR 0x102     // COLOR00 below the bands (seen through pits)

typedef struct {
	int8_t dx;      // -1, 0 or 1
	uint8_t jump;   // fire or up held
} tInput;

typedef struct {
	int16_t x, y;       // player top-left in level pixels
	int16_t yFix;       // y << FIX_SHIFT plus sub-pixel
	int16_t vy;         // vertical velocity, 1/16 px per frame (+ = down)
	int16_t cam;        // camera x (level px at screen x 0), 0..CAM_MAX
	uint16_t frame;     // frames since start
	uint8_t onGround;
	uint8_t jumpLatch;  // jump held last frame: must release to jump again
	uint8_t walkFrame;  // sprite frame 0..3
	uint8_t walkTicks;
	uint8_t deaths;     // fell into a pit
	uint8_t jumps;      // jumps started
	uint8_t facingLeft; // last horizontal direction pressed
	uint8_t moving;     // ran this frame on the ground
} tGameState;

// Hero animation frames (art/hero.txt); left-facing = + ART_HERO_MIRROR
#define HERO_IDLE 0
#define HERO_BREATHE 1
#define HERO_WALK 2        // 2..5
#define HERO_JUMP 6
#define HERO_FALL 7
#define HERO_BREATHE_SHIFT 5  // idle <-> breathe every 32 frames

/** Which hero frame (0..7, right-facing) shows this state. */
uint8_t logicHeroFrame(const tGameState *pState);

extern const char *const g_pLevelRows[LEVEL_TILES_H];

/** Tile id at tile coordinates; outside the map = TILE_EMPTY. */
uint8_t logicTileAt(int16_t tx, int16_t ty);

void logicInit(tGameState *pState);

/** Advance one frame. Returns 1 if anything visible changed. */
uint8_t logicUpdate(tGameState *pState, const tInput *pInput);

/** Camera for a player x: centred on the player, clamped to the level. */
int16_t logicCameraFor(int16_t playerX);

/** Scroll offset of a looping parallax band: (cam >> shift) wrapped to loopW. */
uint16_t logicBandOffset(int16_t cam, uint8_t shift, uint16_t loopW);

/** Bitplane pointer byte offset (may be -2) and BPLCON1 delay (0..15) that
 *  show pixel column `scrollX` at screen x 0, with one extra fetch word
 *  (DDFSTRT 0x30, modulo - 2). */
int16_t logicScrollByteOffset(uint16_t scrollX);
uint8_t logicScrollDelay(uint16_t scrollX);

/** Colour of sky band i (0..SKY_BANDS-1, top to bottom), 0xRGB. */
uint16_t logicSkyColor(uint8_t i);
/** Colour of haze band i (0..HAZE_BANDS-1) behind the hills. */
uint16_t logicHazeColor(uint8_t i);

#endif // _LOGIC_H_
