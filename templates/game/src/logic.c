#include "logic.h"

const tRect g_pWalls[WALL_COUNT] = {
	{ 64,  64, 16, 128},  // left pillar
	{240,  64, 16, 128},  // right pillar
	{112, 200, 96,  16},  // floor slab
};

void logicInit(tGameState *pState) {
	pState->x = (WORLD_W - PLAYER_W) / 2;
	pState->y = 100;
	pState->frame = 0;
	pState->bumps = 0;
	pState->isBlocked = 0;
}

static uint8_t overlapsWall(int16_t x, int16_t y) {
	for(uint8_t i = 0; i < WALL_COUNT; ++i) {
		const tRect *w = &g_pWalls[i];
		if(x < w->x + w->w && x + PLAYER_W > w->x && y < w->y + w->h && y + PLAYER_H > w->y) {
			return 1;
		}
	}
	return 0;
}

static int16_t clamp(int16_t v, int16_t lo, int16_t hi) {
	return v < lo ? lo : (v > hi ? hi : v);
}

uint8_t logicUpdate(tGameState *pState, const tInput *pInput) {
	int16_t oldX = pState->x, oldY = pState->y;
	uint8_t wasBlocked = pState->isBlocked;
	pState->isBlocked = 0;

	// Move one axis at a time so the player can slide along walls.
	int16_t nx = clamp(pState->x + pInput->dx * PLAYER_SPEED, 0, WORLD_W - PLAYER_W);
	if(overlapsWall(nx, pState->y)) {
		pState->isBlocked = 1;
	}
	else {
		pState->x = nx;
	}
	int16_t ny = clamp(pState->y + pInput->dy * PLAYER_SPEED, 0, WORLD_H - PLAYER_H);
	if(overlapsWall(pState->x, ny)) {
		pState->isBlocked = 1;
	}
	else {
		pState->y = ny;
	}

	if(pState->isBlocked && !wasBlocked) {
		++pState->bumps;
	}
	++pState->frame;
	return pState->x != oldX || pState->y != oldY || pState->isBlocked != wasBlocked;
}
