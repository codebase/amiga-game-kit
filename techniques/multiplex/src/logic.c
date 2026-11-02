#include "logic.h"

static uint16_t rnd16(tGameState *pState) {   // xorshift
	uint32_t x = pState->rnd;
	x ^= x << 13;
	x ^= x >> 17;
	x ^= x << 5;
	pState->rnd = x;
	return (uint16_t)x;
}

static void bulletRespawn(tGameState *pState, tBullet *pB) {
	pB->x = (int16_t)(rnd16(pState) % (WORLD_W - BULLET_W));
	pB->vy = (int8_t)(1 + (rnd16(pState) & 3));
}

void logicInit(tGameState *pState) {
	pState->rnd = 0x2545F491;
	pState->frame = 0;
	pState->hits = 0;
	pState->x = (WORLD_W - PLAYER_W) / 2;
	pState->y = 200;
	for(uint8_t i = 0; i < BULLETS; ++i) {
		tBullet *pB = &pState->pBullets[i];
		pState->pOrder[i] = i;
		bulletRespawn(pState, pB);
		pB->y = (int16_t)(rnd16(pState) % (WORLD_H - BULLET_H));
		pB->isOver = 0;
	}
}

uint8_t logicUpdate(tGameState *pState, const tInput *pInput) {
	++pState->frame;
	int16_t x = (int16_t)(pState->x + pInput->dx * PLAYER_SPEED);
	int16_t y = (int16_t)(pState->y + pInput->dy * PLAYER_SPEED);
	if(x < 0) x = 0;
	if(x > WORLD_W - PLAYER_W) x = WORLD_W - PLAYER_W;
	if(y < 0) y = 0;
	if(y > WORLD_H - PLAYER_H) y = WORLD_H - PLAYER_H;
	uint8_t isChanged = x != pState->x || y != pState->y;
	pState->x = x;
	pState->y = y;
	for(uint8_t i = 0; i < BULLETS; ++i) {
		tBullet *pB = &pState->pBullets[i];
		pB->y = (int16_t)(pB->y + pB->vy);
		if(pB->y > WORLD_H - BULLET_H) {
			pB->y = 0;
			bulletRespawn(pState, pB);
		}
		uint8_t isOver = pB->x < x + PLAYER_W && pB->x + BULLET_W > x && pB->y < y + PLAYER_H && pB->y + BULLET_H > y;
		if(isOver && !pB->isOver) {
			++pState->hits;
			isChanged = 1;
		}
		pB->isOver = isOver;
	}
	return isChanged;
}

uint8_t logicMultiplex(const tBullet *pBullets, uint8_t *pOrder, uint8_t ubCount, tChain pChains[CHANNELS]) {
	// Insertion sort of the order by y. Bullets move a few lines a frame, so
	// last frame's order is nearly right and this is a pass of compares
	// (sorting from scratch every frame cost half the game's CPU time)
	for(uint8_t i = 1; i < ubCount; ++i) {
		uint8_t ubIdx = pOrder[i];
		int16_t y = pBullets[ubIdx].y;
		uint8_t j = i;
		while(j > 0 && pBullets[pOrder[j - 1]].y > y) {
			pOrder[j] = pOrder[j - 1];
			--j;
		}
		pOrder[j] = ubIdx;
	}
	int16_t pEnd[CHANNELS];              // where each chain's last sprite ends (its vstop line)
	for(uint8_t c = 0; c < CHANNELS; ++c) {
		pChains[c].count = 0;
		pEnd[c] = -0x7FFF;
	}
	uint8_t ubDropped = 0;
	for(uint8_t k = 0; k < ubCount; ++k) {
		uint8_t i = pOrder[k];
		int16_t y = pBullets[i].y;
		uint8_t c = 0;
		while(c < CHANNELS && pEnd[c] + CHAIN_GAP > y) ++c;   // the first chain with room
		if(c == CHANNELS) {
			++ubDropped;
			continue;
		}
		pChains[c].pIndex[pChains[c].count++] = i;
		pEnd[c] = (int16_t)(y + BULLET_H);
	}
	return ubDropped;
}
