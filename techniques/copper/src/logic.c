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
	int16_t ny = clamp(pState->y + pInput->dy * PLAYER_SPEED, HUD_H, WORLD_H - PLAYER_H);
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

// ---------------------------------------------------------------- copper ---

// Key colours of the sunset sky, top to bottom. Consecutive keys are
// SKY_STEPS channel steps apart in total, so the walk below gives exactly
// SKY_BANDS different colours.
static const uint16_t s_pSkyKeys[] = {0x000, 0x138, 0x54C, 0xB5A, 0xF84, 0xFFB};
#define SKY_KEY_COUNT (sizeof(s_pSkyKeys) / sizeof(s_pSkyKeys[0]))

static int16_t channel(uint16_t uwColor, uint8_t ubIdx) {
	return (uwColor >> (8 - 4 * ubIdx)) & 0xF;
}

static int16_t iabs(int16_t v) {
	return v < 0 ? -v : v;
}

void logicBuildSkyBands(uint16_t pBands[SKY_BANDS]) {
	uint8_t ubBand = 0;
	int16_t pCurr[3];
	for(uint8_t i = 0; i < 3; ++i) {
		pCurr[i] = channel(s_pSkyKeys[0], i);
	}
	pBands[ubBand++] = s_pSkyKeys[0];
	for(uint8_t k = 1; k < SKY_KEY_COUNT && ubBand < SKY_BANDS; ++k) {
		int16_t pDelta[3], pMoved[3] = {0, 0, 0};
		int16_t wLen = 0;
		for(uint8_t i = 0; i < 3; ++i) {
			pDelta[i] = channel(s_pSkyKeys[k], i) - pCurr[i];
			wLen += iabs(pDelta[i]);
		}
		// Bresenham-like walk: each band moves the channel that lags most
		// behind a straight line in RGB space by one step.
		for(int16_t t = 1; t <= wLen && ubBand < SKY_BANDS; ++t) {
			uint8_t ubBest = 0;
			int16_t wBestLag = -32767;
			for(uint8_t i = 0; i < 3; ++i) {
				int16_t wAbs = iabs(pDelta[i]);
				if(pMoved[i] == wAbs) {
					continue;
				}
				int16_t wLag = wAbs * t - pMoved[i] * wLen;
				if(wLag > wBestLag) {
					wBestLag = wLag;
					ubBest = i;
				}
			}
			++pMoved[ubBest];
			pCurr[ubBest] += pDelta[ubBest] > 0 ? 1 : -1;
			pBands[ubBand++] = (uint16_t)((pCurr[0] << 8) | (pCurr[1] << 4) | pCurr[2]);
		}
	}
	while(ubBand < SKY_BANDS) { // only if the keys are too close together
		pBands[ubBand] = pBands[ubBand - 1];
		++ubBand;
	}
}

// Half of a shaded cyan bar; the other half mirrors it.
static const uint16_t s_pBarHalf[BAR_H / 2] = {
	0x013, 0x035, 0x057, 0x079, 0x09B, 0x2BD, 0x7DE, 0xEFF
};

uint16_t logicBarColor(uint8_t ubLine) {
	return ubLine < BAR_H / 2 ? s_pBarHalf[ubLine] : s_pBarHalf[BAR_H - 1 - ubLine];
}

int16_t logicBarY(uint16_t uwFrame) {
	// Triangle wave 0..127..0 over 256 frames: masks and shifts only, no
	// division (the 68000 has no 32-bit divide).
	uint8_t ubPhase = uwFrame & (BAR_PERIOD - 1);
	int16_t wTri = ubPhase < 128 ? ubPhase : 255 - ubPhase;
	return BAR_MIN_Y + ((wTri * 13) >> 3); // 16-bit multiply is one MULS
}

uint16_t logicLineColor(const uint16_t pBands[SKY_BANDS], int16_t y, int16_t barY) {
	if(y >= barY && y < barY + BAR_H) {
		return logicBarColor((uint8_t)(y - barY));
	}
	return pBands[(y - SKY_TOP) >> 2]; // SKY_BAND_H == 4
}
