#include "logic.h"

const tRect g_pWalls[WALL_COUNT] = {
	{ 64,  64, 16, 128},  // left pillar
	{240,  64, 16, 128},  // right pillar
	{112, 200, 96,  16},  // floor slab
};

void logicInit(tGameState *pState) {
	pState->x = START_X;
	pState->y = START_Y;
	pState->frame = 0;
	pState->bumps = 0;
	pState->isBlocked = 0;
	pState->enemyX = ENEMY_MIN_X;
	pState->enemyDir = 1;
	pState->lives = START_LIVES;
	pState->isGameOver = 0;
	pState->wasHit = 0;
}

void logicUpdateEnemy(tGameState *pState) {
	pState->enemyX += pState->enemyDir * ENEMY_SPEED;
	if(pState->enemyX >= ENEMY_MAX_X) {
		pState->enemyX = ENEMY_MAX_X;
		pState->enemyDir = -1;
	}
	else if(pState->enemyX <= ENEMY_MIN_X) {
		pState->enemyX = ENEMY_MIN_X;
		pState->enemyDir = 1;
	}
}

uint8_t logicPlayerHitsEnemy(const tGameState *pState) {
	return (
		pState->x < pState->enemyX + ENEMY_W && pState->x + PLAYER_W > pState->enemyX &&
		pState->y < ENEMY_Y + ENEMY_H && pState->y + PLAYER_H > ENEMY_Y
	);
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
	pState->wasHit = 0;

	logicUpdateEnemy(pState);
	if(pState->isGameOver) {
		++pState->frame;
		return 0;
	}

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

	if(logicPlayerHitsEnemy(pState)) {
		pState->wasHit = 1;
		--pState->lives;
		pState->x = START_X;
		pState->y = START_Y;
		if(pState->lives == 0) {
			pState->isGameOver = 1;
		}
	}
	++pState->frame;
	return pState->x != oldX || pState->y != oldY || pState->isBlocked != wasBlocked || pState->wasHit;
}
