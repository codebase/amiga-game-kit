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

#define START_X ((WORLD_W - PLAYER_W) / 2)
#define START_Y 100
#define START_LIVES 3

// Patrolling enemy (a blitter BOB on the Amiga side).
#define ENEMY_W 32
#define ENEMY_H 16
#define ENEMY_Y 150
#define ENEMY_MIN_X 90
#define ENEMY_MAX_X 210
#define ENEMY_SPEED 1

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
	int16_t enemyX;     // enemy top-left x (y is fixed at ENEMY_Y)
	int8_t enemyDir;    // +1 moving right, -1 moving left
	uint8_t lives;      // 0 = game over
	uint8_t isGameOver; // set once lives reach 0; player no longer moves
	uint8_t wasHit;     // player lost a life on the latest frame
} tGameState;

extern const tRect g_pWalls[WALL_COUNT];

void logicInit(tGameState *pState);

/** Advance one frame (player and enemy). Returns 1 if the player state
 * changed (moved, blocked/unblocked, or hit). The enemy moves every frame. */
uint8_t logicUpdate(tGameState *pState, const tInput *pInput);

/** Move the enemy one step along its patrol, turning around at the ends. */
void logicUpdateEnemy(tGameState *pState);

/** 1 if the player's box overlaps the enemy's box. */
uint8_t logicPlayerHitsEnemy(const tGameState *pState);

#endif // _LOGIC_H_
