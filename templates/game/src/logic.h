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

#endif // _LOGIC_H_
