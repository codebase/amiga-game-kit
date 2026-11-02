// Host-side tests for src/logic.c: the chains obey the hardware's rule. agk unit
#include <stdio.h>
#include "logic.h"

static int s_failures;
#define CHECK(cond) do { \
	if(!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); ++s_failures; } \
} while(0)

static tGameState s_g;

// every chain top to bottom, each sprite at least CHAIN_GAP lines under the previous one's end
static int chainsValid(const tBullet *pB, const tChain *pChains, int *pShown) {
	int ok = 1;
	*pShown = 0;
	for(int c = 0; c < CHANNELS; ++c) {
		int end = -10000;
		for(int k = 0; k < pChains[c].count; ++k) {
			int y = pB[pChains[c].pIndex[k]].y;
			if(y < end + CHAIN_GAP) ok = 0;
			end = y + BULLET_H;
			++*pShown;
		}
	}
	return ok;
}

int main(void) {
	logicInit(&s_g);
	tChain pChains[CHANNELS];
	int shown, maxDropped = 0;
	for(int f = 0; f < 2000; ++f) {
		logicUpdate(&s_g, &(tInput){0});
		int dropped = logicMultiplex(s_g.pBullets, s_g.pOrder, BULLETS, pChains);
		CHECK(chainsValid(s_g.pBullets, pChains, &shown));
		CHECK(shown + dropped == BULLETS);
		if(dropped > maxDropped) maxDropped = dropped;
	}
	printf("over 2000 frames the most bullets dropped in one frame: %d\n", maxDropped);
	// a stack of 7 at one height: 6 channels take 6, one is dropped
	tBullet pStack[7];
	uint8_t pOrder[7] = {6, 5, 4, 3, 2, 1, 0};     // any starting order works (a permutation of the bullets)
	for(int i = 0; i < 7; ++i) pStack[i] = (tBullet){.x = 16 * i, .y = 100, .vy = 1};
	CHECK(logicMultiplex(pStack, pOrder, 7, pChains) == 1);
	CHECK(chainsValid(pStack, pChains, &shown) && shown == 6);
	// spaced by the sprite height plus the gap: all on one chain
	uint8_t pOrder5[5] = {4, 3, 2, 1, 0};
	for(int i = 0; i < 5; ++i) pStack[i].y = (int16_t)(100 + i * (BULLET_H + CHAIN_GAP));
	CHECK(logicMultiplex(pStack, pOrder5, 5, pChains) == 0 && pChains[0].count == 5 && pChains[1].count == 0);
	// one line closer: alternate chains
	for(int i = 0; i < 5; ++i) pStack[i].y = (int16_t)(100 + i * BULLET_H);
	CHECK(logicMultiplex(pStack, pOrder5, 5, pChains) == 0 && pChains[0].count == 3 && pChains[1].count == 2);
	// the player moves and takes hits
	logicInit(&s_g);
	for(int f = 0; f < 20; ++f) logicUpdate(&s_g, &(tInput){.dx = 1});
	CHECK(s_g.x == (WORLD_W - PLAYER_W) / 2 + 40);
	for(int f = 0; f < 1000; ++f) logicUpdate(&s_g, &(tInput){0});
	CHECK(s_g.hits > 0);
	if(s_failures) { printf("%d check(s) failed\n", s_failures); return 1; }
	return 0;
}
