/**
 * Game rules - plain portable C.
 *
 * Nothing in logic.c may include ACE or Amiga headers: it is compiled for the
 * Amiga *and* for the host so `agk unit` can test it in milliseconds.
 * Keep it deterministic and use 16-bit types: the 68000 has no 32-bit
 * multiply/divide instruction.
 *
 * This game: a 60x16-tile level (960x256 px, three screens wide). The player
 * walks along the ground; the camera keeps the player centred and stops at
 * the level edges. Everything here is in world pixels; main.c turns camera X
 * into a hardware scroll and player X - camera X into a sprite position.
 */
#ifndef _LOGIC_H_
#define _LOGIC_H_

#include <stdint.h>

#define TILE_SHIFT 4
#define TILE_SIZE (1 << TILE_SHIFT)
#define LEVEL_TILES_W 60
#define LEVEL_TILES_H 16
#define LEVEL_W (LEVEL_TILES_W * TILE_SIZE)   // 960
#define LEVEL_H (LEVEL_TILES_H * TILE_SIZE)   // 256

#define SCREEN_W 320
#define SCREEN_H 256
#define CAMERA_MAX_X (LEVEL_W - SCREEN_W)     // 640

#define PLAYER_W 16
#define PLAYER_H 16
#define PLAYER_WALK_SPEED 1   // px/frame, joystick left/right
#define PLAYER_RUN_SPEED 2    // px/frame, with fire held
#define PLAYER_START_X 32
#define GROUND_ROW 14
#define PLAYER_Y (GROUND_ROW * TILE_SIZE - PLAYER_H)  // 208: standing on the grass

typedef enum {
	TILE_SKY,
	TILE_CLOUD,
	TILE_GRASS,
	TILE_DIRT,
	TILE_BRICK,
	TILE_COLUMN,
	TILE_COLUMN_TOP,
	TILE_SIGN_RED,
	TILE_SIGN_GREEN,
	TILE_SIGN_BLUE,
	TILE_COUNT
} tTile;

typedef struct {
	int8_t dx;     // -1, 0 or 1
	uint8_t fire;  // run
} tInput;

typedef struct {
	int16_t x;        // player left edge, world pixels
	int16_t y;        // player top edge, world pixels
	int16_t cameraX;  // left edge of the screen, world pixels, 0..CAMERA_MAX_X
	uint16_t frame;
	uint8_t isMoving; // player moved on the latest frame
} tGameState;

/** Tile index at tile coords (tx, ty). Outside the level: TILE_SKY. */
uint8_t levelTileAt(int16_t tx, int16_t ty);

/** Camera X that centres the player, clamped to the level. */
int16_t cameraFollow(int16_t playerX);

/** Screen X of a world X for the given camera. */
static inline int16_t worldToScreenX(int16_t worldX, int16_t cameraX) {
	return worldX - cameraX;
}

void logicInit(tGameState *pState);

/** Advance one frame. Returns 1 if anything visible changed. */
uint8_t logicUpdate(tGameState *pState, const tInput *pInput);

#endif // _LOGIC_H_
