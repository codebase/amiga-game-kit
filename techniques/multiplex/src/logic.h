/**
 * The rules: a player dodging a rain of bullets, and the part of sprite
 * multiplexing that isn't hardware: which bullet goes on which sprite
 * channel. Plain portable C, tested on the host by agk unit.
 */
#ifndef _LOGIC_H_
#define _LOGIC_H_

#include <stdint.h>

#define WORLD_W 320
#define WORLD_H 256
#define PLAYER_W 16
#define PLAYER_H 16
#define PLAYER_SPEED 2
#define BULLETS 24
#define BULLET_W 16
#define BULLET_H 8
#define CHANNELS 6                   // sprite channels 2-7 carry the bullets
#define CHAIN_GAP 1                  // lines between one sprite's end and the next one's start, same channel

typedef struct {
	int16_t x, y;                    // top left, px; 0 <= y <= WORLD_H - BULLET_H
	int8_t vy;                       // px per frame, down
	uint8_t isOver;                  // overlapping the player (a hit counts when it starts)
} tBullet;

typedef struct {
	uint8_t count;
	uint8_t pIndex[BULLETS];         // bullets, top to bottom
} tChain;

typedef struct {
	int8_t dx, dy;
	uint8_t fire;
} tInput;

typedef struct {
	int16_t x, y;                    // the player's top left
	tBullet pBullets[BULLETS];
	uint8_t pOrder[BULLETS];         // the bullets top to bottom, kept between frames (nearly sorted already)
	uint16_t frame, hits;
	uint32_t rnd;
} tGameState;

void logicInit(tGameState *pState);
/** One frame. Returns 1 when the player moved or was hit. */
uint8_t logicUpdate(tGameState *pState, const tInput *pInput);

/**
 * Sort the bullets by y (pOrder, kept from last frame, so the sort is nearly
 * free) and deal them onto CHANNELS chains so that on each chain every sprite
 * starts at least CHAIN_GAP lines below the previous one's end: what the
 * hardware needs to show them one after another from a single channel.
 * Returns how many bullets found no chain (too many at the same height);
 * they're not shown this frame.
 */
uint8_t logicMultiplex(const tBullet *pBullets, uint8_t *pOrder, uint8_t ubCount, tChain pChains[CHANNELS]);

#endif // _LOGIC_H_
