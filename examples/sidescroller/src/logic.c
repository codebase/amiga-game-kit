#include "logic.h"

// 80 columns x 16 rows. '.' empty, '#' grass top, 'd' dirt, '=' stone slab.
const char *const g_pLevelRows[LEVEL_TILES_H] = {
//   0         1         2         3         4         5         6         7
//   01234567890123456789012345678901234567890123456789012345678901234567890123456789
	"................................................................................", // 0
	"................................................................................", // 1
	"................................................................................", // 2
	"................................................................................", // 3
	"........................====.................===..................===...........", // 4
	"................................................................................", // 5
	"................................................................................", // 6
	"...............====...................====...............====...................", // 7
	"................................................................................", // 8
	"................................................................................", // 9
	"........====..................=====...................====..............====....", // 10
	"................................................................................", // 11
	"................................................................................", // 12
	"######################...######################...#############..###############", // 13
	"dddddddddddddddddddddd...dddddddddddddddddddddd...ddddddddddddd..ddddddddddddddd", // 14
	"dddddddddddddddddddddd...dddddddddddddddddddddd...ddddddddddddd..ddddddddddddddd", // 15
};

uint8_t logicTileAt(int16_t tx, int16_t ty) {
	if(tx < 0 || tx >= LEVEL_TILES_W || ty < 0 || ty >= LEVEL_TILES_H) {
		return TILE_EMPTY;
	}
	switch(g_pLevelRows[ty][tx]) {
		case '#': return TILE_GRASS;
		case 'd': return TILE_DIRT;
		case '=': return TILE_STONE;
		default: return TILE_EMPTY;
	}
}

static uint8_t isSolidPx(int16_t px, int16_t py) {
	if(py < 0) return 0;
	uint8_t t = logicTileAt(px >> TILE_SHIFT, py >> TILE_SHIFT);
	return t == TILE_GRASS || t == TILE_DIRT;
}

// Can the player's box (top-left x,y) move down one pixel? Checks the row
// just under the feet: solid tiles block; a stone slab blocks only at its
// top edge (one-way platform).
static uint8_t isBlockedBelow(int16_t x, int16_t y) {
	int16_t py = y + PLAYER_H;
	if(py < 0) return 0;
	int16_t ty = py >> TILE_SHIFT;
	uint8_t isTopEdge = (py & (TILE_SIZE - 1)) == 0;
	int16_t txL = (x + PLAYER_HB_L) >> TILE_SHIFT, txR = (x + PLAYER_HB_R) >> TILE_SHIFT;
	for(int16_t tx = txL; tx <= txR; ++tx) {
		uint8_t t = logicTileAt(tx, ty);
		if(t == TILE_GRASS || t == TILE_DIRT || (t == TILE_STONE && isTopEdge)) {
			return 1;
		}
	}
	return 0;
}

static uint8_t isBlockedAbove(int16_t x, int16_t y) {
	return isSolidPx(x + PLAYER_HB_L, y - 1) || isSolidPx(x + PLAYER_HB_R, y - 1);
}

static uint8_t isBlockedSide(int16_t x, int16_t y, int8_t dx) {
	int16_t px = dx > 0 ? x + PLAYER_HB_R + 1 : x + PLAYER_HB_L - 1;
	return isSolidPx(px, y) || isSolidPx(px, y + PLAYER_H - 1);
}

int16_t logicCameraFor(int16_t playerX) {
	int16_t cam = playerX + PLAYER_W / 2 - SCREEN_W / 2;
	if(cam < 0) cam = 0;
	if(cam > CAM_MAX) cam = CAM_MAX;
	return cam;
}

uint16_t logicBandOffset(int16_t cam, uint8_t shift, uint16_t loopW) {
	uint16_t o = (uint16_t)cam >> shift;
	while(o >= loopW) o -= loopW; // at most a couple of iterations: no divide
	return o;
}

int16_t logicScrollByteOffset(uint16_t scrollX) {
	return (int16_t)((((int16_t)scrollX - 1) >> 4) * 2); // * 2, not << 1: may be negative
}

uint8_t logicScrollDelay(uint16_t scrollX) {
	return (uint8_t)((16 - (scrollX & 15)) & 15);
}

static void respawn(tGameState *pState) {
	pState->x = START_X;
	pState->y = START_Y;
	pState->yFix = START_Y << FIX_SHIFT;
	pState->vy = 0;
	pState->onGround = 1;
	pState->cam = logicCameraFor(pState->x);
}

void logicInit(tGameState *pState) {
	respawn(pState);
	pState->frame = 0;
	pState->jumpLatch = 0;
	pState->walkFrame = 0;
	pState->walkTicks = 0;
	pState->deaths = 0;
	pState->jumps = 0;
	pState->facingLeft = 0;
	pState->moving = 0;
}

uint8_t logicUpdate(tGameState *pState, const tInput *pInput) {
	int16_t oldX = pState->x, oldY = pState->y, oldCam = pState->cam;
	uint8_t oldGround = pState->onGround, oldWalk = pState->walkFrame;

	if(pInput->dx) {
		pState->facingLeft = pInput->dx < 0;
	}

	// Horizontal: pixel by pixel, stop at solid tiles and level edges.
	if(pInput->dx) {
		for(uint8_t i = 0; i < PLAYER_SPEED; ++i) {
			int16_t nx = pState->x + pInput->dx;
			if(nx < 0 || nx > LEVEL_W - PLAYER_W || isBlockedSide(pState->x, pState->y, pInput->dx)) {
				break;
			}
			pState->x = nx;
		}
	}

	// Jump: only from the ground, and only on a fresh press.
	if(pInput->jump && !pState->jumpLatch && pState->onGround) {
		pState->vy = -JUMP_VEL;
		pState->onGround = 0;
		++pState->jumps;
	}
	pState->jumpLatch = pInput->jump;

	// Walked off an edge?
	if(pState->onGround && !isBlockedBelow(pState->x, pState->y)) {
		pState->onGround = 0;
		pState->vy = 0;
	}

	if(!pState->onGround) {
		pState->vy += GRAVITY;
		if(pState->vy > MAX_FALL_VEL) pState->vy = MAX_FALL_VEL;
		int16_t targetFix = pState->yFix + pState->vy;
		int16_t targetY = targetFix >> FIX_SHIFT;
		// Step one pixel at a time so nothing is skipped.
		while(pState->y < targetY) {
			if(isBlockedBelow(pState->x, pState->y)) {
				pState->onGround = 1;
				pState->vy = 0;
				break;
			}
			++pState->y;
		}
		while(pState->y > targetY) {
			if(isBlockedAbove(pState->x, pState->y)) {
				pState->vy = 0;
				break;
			}
			--pState->y;
		}
		if(pState->y == targetY) {
			pState->yFix = targetFix;
		}
		else {
			pState->yFix = pState->y << FIX_SHIFT; // stopped by a tile
		}
		if(!pState->onGround && pState->vy >= 0 && isBlockedBelow(pState->x, pState->y)) {
			pState->onGround = 1; // landed exactly on a surface this frame
			pState->vy = 0;
			pState->yFix = pState->y << FIX_SHIFT;
		}
		if(pState->y > LEVEL_H) { // fell into a pit
			++pState->deaths;
			respawn(pState);
		}
	}

	// Walk animation: cycle while running on the ground, frame 0 standing,
	// frame 1 (stride) in the air.
	if(!pState->onGround) {
		pState->walkFrame = 1;
		pState->walkTicks = 0;
	}
	else if(pInput->dx && pState->x != oldX) {
		if(++pState->walkTicks >= WALK_FRAME_TICKS) {
			pState->walkTicks = 0;
			pState->walkFrame = (pState->walkFrame + 1) & (WALK_FRAMES - 1);
		}
	}
	else {
		pState->walkFrame = 0;
		pState->walkTicks = 0;
	}

	pState->moving = pState->onGround && pInput->dx && pState->x != oldX;
	pState->cam = logicCameraFor(pState->x);
	++pState->frame;
	return pState->x != oldX || pState->y != oldY || pState->cam != oldCam ||
		pState->onGround != oldGround || pState->walkFrame != oldWalk;
}

uint8_t logicHeroFrame(const tGameState *pState) {
	if(!pState->onGround) {
		return pState->vy < 0 ? HERO_JUMP : HERO_FALL;
	}
	if(pState->moving) {
		return HERO_WALK + pState->walkFrame;
	}
	return (pState->frame >> HERO_BREATHE_SHIFT) & 1 ? HERO_BREATHE : HERO_IDLE;
}

// Colour gradients: startup only (tables are built once in main.c).
static uint16_t lerpColor(uint16_t a, uint16_t b, int16_t t, int16_t n) {
	uint16_t out = 0;
	for(uint8_t s = 0; s <= 8; s += 4) {
		int16_t ca = (a >> s) & 0xF, cb = (b >> s) & 0xF;
		int16_t d = (cb - ca) * t; // rounded to nearest, symmetric for +/-
		int16_t c = ca + (d >= 0 ? (d + n / 2) / n : -((-d + n / 2) / n));
		out |= (uint16_t)c << s;
	}
	return out;
}

uint16_t logicSkyColor(uint8_t i) {
	// Dawn: deep blue at the top -> violet -> warm orange at the horizon.
	static const uint8_t pKeyBand[] = {0, 14, 26, SKY_BANDS - 1};
	static const uint16_t pKeyColor[] = {0x114, 0x437, 0xB67, 0xFB6};
	for(uint8_t k = 0; k < 3; ++k) {
		if(i <= pKeyBand[k + 1]) {
			return lerpColor(pKeyColor[k], pKeyColor[k + 1], i - pKeyBand[k], pKeyBand[k + 1] - pKeyBand[k]);
		}
	}
	return pKeyColor[3];
}

uint16_t logicHazeColor(uint8_t i) {
	return lerpColor(0xBDE, 0x7AC, i, HAZE_BANDS - 1);
}
