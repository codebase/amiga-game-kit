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

#define TILE_FRAME_EDGE_R 3 // grass with a pit on the right; +1 pit on the left, +2 dirt
uint8_t logicTileArtFrame(int16_t tx, int16_t ty) {
	uint8_t t = logicTileAt(tx, ty);
	if(t == TILE_GRASS || t == TILE_DIRT) {
		uint8_t ubDirt = t == TILE_DIRT ? 2 : 0;
		// the level ends are not pit edges
		if(tx < LEVEL_TILES_W - 1 && logicTileAt(tx + 1, ty) == TILE_EMPTY) return TILE_FRAME_EDGE_R + ubDirt;
		if(tx > 0 && logicTileAt(tx - 1, ty) == TILE_EMPTY) return TILE_FRAME_EDGE_R + 1 + ubDirt;
	}
	return t - 1;
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

// ----------------------------------------------------------------- enemies

// Where the enemies start, their initial direction and their patrol range.
// All of them walk on the ground (y 192), and they share ONE multiplexed
// sprite pair, which can't show two enemies on the same lines: so the ranges
// are at least SCREEN_W + ENEMY_W (336) px apart and two are never on screen
// together (unit-tested).
static const struct {
	int16_t x;
	int8_t dir;
	int16_t xMin, xMax;
} s_pEnemySpawns[ENEMY_COUNT] = {
	{ 240, -1,  170,  264}, // 0: first stretch of ground (0..351)
	{ 700, -1,  600,  735}, // 1: between pit 1 and pit 2 (400..751)
	{1150, -1, 1071, 1264}, // 2: last stretch of ground (1040..1279), to the level end
};

// Can an enemy at x,y take one pixel step in dir? Not past the level ends,
// not into a solid tile, and not off its floor: the tile under the leading
// foot column must be ground or a slab top (that's how it turns at pits and
// platform edges).
static uint8_t enemyCanStep(int16_t x, int16_t y, int8_t dir) {
	int16_t nx = x + dir;
	if(nx < 0 || nx > LEVEL_W - ENEMY_W) {
		return 0;
	}
	int16_t lead = dir > 0 ? nx + ENEMY_HB_R : nx + ENEMY_HB_L;
	if(isSolidPx(lead, y) || isSolidPx(lead, y + ENEMY_H - 1)) {
		return 0;
	}
	uint8_t t = logicTileAt(lead >> TILE_SHIFT, (y + ENEMY_H) >> TILE_SHIFT);
	return t != TILE_EMPTY;
}

void logicEnemyPlace(tEnemy *pEnemy, int16_t x, int16_t y, int8_t dir) {
	pEnemy->x = x;
	pEnemy->xFix = x << FIX_SHIFT;
	pEnemy->y = y;
	pEnemy->dir = dir;
	pEnemy->state = ENEMY_WALK;
	pEnemy->timer = 0;
	pEnemy->xMin = x;
	while(enemyCanStep(pEnemy->xMin, y, -1)) --pEnemy->xMin;
	pEnemy->xMax = x;
	while(enemyCanStep(pEnemy->xMax, y, 1)) ++pEnemy->xMax;
}

static void enemiesInit(tGameState *pState) {
	for(uint8_t i = 0; i < ENEMY_COUNT; ++i) {
		tEnemy *pE = &pState->pEnemies[i];
		logicEnemyPlace(pE, s_pEnemySpawns[i].x, START_Y, s_pEnemySpawns[i].dir);
		// never beyond the floor, and never closer to the next enemy than a screen
		if(pE->xMin < s_pEnemySpawns[i].xMin) pE->xMin = s_pEnemySpawns[i].xMin;
		if(pE->xMax > s_pEnemySpawns[i].xMax) pE->xMax = s_pEnemySpawns[i].xMax;
	}
	logicEnemySortY(pState);
}

static void enemiesMove(tGameState *pState) {
	for(uint8_t i = 0; i < ENEMY_COUNT; ++i) {
		tEnemy *pE = &pState->pEnemies[i];
		if(pE->state == ENEMY_SQUASHED) {
			if(--pE->timer == 0) {
				pE->state = ENEMY_GONE;
			}
			continue;
		}
		if(pE->state != ENEMY_WALK) {
			continue;
		}
		int16_t nextFix = pE->xFix + (pE->dir > 0 ? ENEMY_SPEED_FIX : -ENEMY_SPEED_FIX);
		if((nextFix >> FIX_SHIFT) == pE->x) {
			pE->xFix = nextFix; // sub-pixel step only
		}
		else if(pE->dir > 0 ? pE->x < pE->xMax : pE->x > pE->xMin) {
			pE->xFix = nextFix;
			pE->x = nextFix >> FIX_SHIFT;
		}
		else {
			// Turn around: stays on this pixel, starts the other way from
			// the pixel's middle so both directions take the same time.
			pE->dir = -pE->dir;
			pE->xFix = (pE->x << FIX_SHIFT) + (1 << (FIX_SHIFT - 1));
		}
	}
}

static void respawn(tGameState *pState);

// Hero vs enemies, after both have moved. Returns 1 if the hero died.
static uint8_t enemiesCollide(tGameState *pState, uint8_t isJumpHeld) {
	int16_t hL = pState->x + PLAYER_HB_L, hR = pState->x + PLAYER_HB_R;
	int16_t hT = pState->y, hB = pState->y + PLAYER_H - 1;
	uint8_t isFalling = pState->vy > 0;
	uint8_t isStomped = 0;
	for(uint8_t i = 0; i < ENEMY_COUNT; ++i) {
		tEnemy *pE = &pState->pEnemies[i];
		if(pE->state != ENEMY_WALK) {
			continue;
		}
		int16_t eT = pE->y + ENEMY_HB_T;
		if(hR < pE->x + ENEMY_HB_L || hL > pE->x + ENEMY_HB_R || hB < eT || hT > pE->y + ENEMY_H - 1) {
			continue; // no overlap
		}
		if(isFalling && hB < eT + STOMP_WINDOW) {
			pE->state = ENEMY_SQUASHED;
			pE->timer = SQUASH_TICKS;
			++pState->stomps;
			pState->eventStomp |= 1 << i;
			isStomped = 1;
		}
		else {
			// Side, below, or rising into it: the hero dies.
			pState->eventHit = i + 1;
			++pState->hits;
			++pState->deaths;
			respawn(pState);
			return 1;
		}
	}
	if(isStomped) {
		pState->vy = isJumpHeld ? -JUMP_VEL : -BOUNCE_VEL;
		pState->yFix = pState->y << FIX_SHIFT;
		pState->onGround = 0;
	}
	return 0;
}

uint8_t logicEnemyFrame(const tEnemy *pEnemy) {
	uint8_t ubFrame = pEnemy->state == ENEMY_WALK ?
		ENEMY_FRAME_WALK + ((pEnemy->x >> ENEMY_ANIM_SHIFT) & 1) : ENEMY_FRAME_SQUASHED;
	return pEnemy->dir > 0 ? ubFrame + ENEMY_FRAME_MIRROR : ubFrame;
}

void logicEnemySortY(tGameState *pState) {
	// Insertion sort by y, stable (equal y keeps id order). Enemies never
	// change height, so this runs once, not per frame.
	for(uint8_t i = 0; i < ENEMY_COUNT; ++i) {
		uint8_t j = i;
		while(j > 0 && pState->pEnemies[pState->pOrderY[j - 1]].y > pState->pEnemies[i].y) {
			pState->pOrderY[j] = pState->pOrderY[j - 1];
			--j;
		}
		pState->pOrderY[j] = i;
	}
}

void logicEnemyPlan(const tGameState *pState, tEnemyPlan *pPlan) {
	// Walk the enemies top to bottom, keeping the visible ones (any column
	// on screen: screen x -15..319, and not gone). Greedy: a candidate that
	// starts less than ENEMY_MUX_GAP below the last shown one conflicts with
	// it. Even frame: keep the upper one; odd frame: replace it (the
	// candidate is lower, so it still fits below the one before).
	uint8_t ubCount = 0, ubSkipped = 0;
	int16_t wLastY = 0;
	for(uint8_t k = 0; k < ENEMY_COUNT; ++k) {
		uint8_t id = pState->pOrderY[k];
		const tEnemy *pE = &pState->pEnemies[id];
		if((uint16_t)(pE->x - pState->cam + (ENEMY_W - 1)) >= SCREEN_W + ENEMY_W - 1 || pE->state == ENEMY_GONE) {
			continue;
		}
		if(ubCount == 0 || pE->y >= wLastY + ENEMY_MUX_GAP) {
			pPlan->pId[ubCount++] = id;
			wLastY = pE->y;
		}
		else {
			++ubSkipped;
			if(pState->frame & 1) {
				pPlan->pId[ubCount - 1] = id;
				wLastY = pE->y;
			}
		}
	}
	pPlan->count = ubCount;
	pPlan->skipped = ubSkipped;
}

// -------------------------------------------------------------------- hero

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
	pState->stomps = 0;
	pState->hits = 0;
	pState->eventStomp = 0;
	pState->eventHit = 0;
	enemiesInit(pState);
}

uint8_t logicUpdate(tGameState *pState, const tInput *pInput) {
	int16_t oldX = pState->x, oldY = pState->y, oldCam = pState->cam;
	uint8_t oldGround = pState->onGround, oldWalk = pState->walkFrame;
	pState->eventStomp = 0;
	pState->eventHit = 0;

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

	// Enemies walk; then hero vs enemies (stomp = bounce, anything else =
	// respawn at the start like a pit). Squashed enemies stay squashed and
	// the walkers keep walking across a respawn: the level isn't reset.
	enemiesMove(pState);
	enemiesCollide(pState, pInput->jump);

	pState->moving = pState->onGround && pInput->dx && pState->x != oldX;
	pState->cam = logicCameraFor(pState->x);
	++pState->frame;
	return pState->x != oldX || pState->y != oldY || pState->cam != oldCam ||
		pState->onGround != oldGround || pState->walkFrame != oldWalk ||
		pState->eventStomp || pState->eventHit;
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
// Per-line gradient: OCS has 16 levels per channel, so a plain lerp shows
// stripes. Interpolate in quarter steps and dither the fraction across lines
// (ordered 0,2,1,3): neighbouring lines never differ by more than one level.
static uint16_t lerpColorDither(uint16_t a, uint16_t b, int16_t t, int16_t n, uint8_t ubLine) {
	static const uint8_t pThreshold[4] = {0, 2, 1, 3};
	uint16_t out = 0;
	for(uint8_t s = 0; s <= 8; s += 4) {
		int16_t ca = (a >> s) & 0xF, cb = (b >> s) & 0xF;
		int16_t q = (ca * 4 * n + (cb - ca) * 4 * t) / n; // quarter levels, >= 0
		int16_t c = (q >> 2) + ((q & 3) > pThreshold[ubLine & 3]);
		out |= (uint16_t)c << s;
	}
	return out;
}

uint16_t logicSkyColor(uint8_t i) {
	// Dawn: deep blue at the top -> violet -> warm orange at the horizon.
	static const uint8_t pKeyLine[] = {0, 56, 104, SKY_BANDS - 1};
	static const uint16_t pKeyColor[] = {0x114, 0x437, 0xB67, 0xFB6};
	for(uint8_t k = 0; k < 3; ++k) {
		if(i <= pKeyLine[k + 1]) {
			return lerpColorDither(pKeyColor[k], pKeyColor[k + 1], i - pKeyLine[k], pKeyLine[k + 1] - pKeyLine[k], i);
		}
	}
	return pKeyColor[3];
}

uint16_t logicHazeColor(uint8_t i) {
	// Starts at the mist the mountains fade into (art-clean --fade-bottom 10:0xBCE)
	return lerpColorDither(0xBCE, 0x7AB, i, HAZE_BANDS - 1, i);
}

uint16_t logicPitColor(uint8_t i) {
	// Earth into black. A plain lerp would dither R, G and B on different
	// lines (tinted stripes), so step through a hand-picked ramp of browns and
	// dither between neighbouring entries instead: PIT_RAMP_LINES lines each.
	static const uint16_t pRamp[] = {0x432, 0x321, 0x210, 0x100, 0x000};
	static const uint8_t pThreshold[4] = {0, 2, 1, 3};
	uint8_t k = i / PIT_RAMP_LINES;
	if(k >= sizeof(pRamp) / sizeof(pRamp[0]) - 1) return 0x000;
	uint8_t q = (i % PIT_RAMP_LINES) * 4 / PIT_RAMP_LINES; // 0..3: how far towards the next
	return q > pThreshold[i & 3] ? pRamp[k + 1] : pRamp[k];
}
