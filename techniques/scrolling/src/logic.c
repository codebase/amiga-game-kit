#include "logic.h"

// The level, one character per 16x16 tile. Easy to read and edit; main.c
// copies it into ACE's tile buffer once at startup via levelTileAt().
//   . sky   c cloud   # grass   = dirt   B brick platform
//   T column top   | column   R/G/U red/green/blue sign (test markers)
static const char s_pLevel[LEVEL_TILES_H][LEVEL_TILES_W + 1] = {
	"............................................................", //  0
	"...cc............cc..............cc...............cc........", //  1
	"........cc...............cc...............cc............cc..", //  2
	"............................................................", //  3
	"............................................................", //  4
	"............................................................", //  5
	"............................................................", //  6
	"................................................BBBBB.......", //  7
	"....................BBBBB...................................", //  8
	"...........................T..........................T.....", //  9
	"......BBBB.....T...........|..........................|.....", // 10
	"...............|...........|........BBBBB...T.........|.....", // 11
	"...............|...........|................|.........|.....", // 12
	"..........R....|...........|..G.............|.........|...U.", // 13
	"############################################################", // 14
	"============================================================", // 15
};

uint8_t levelTileAt(int16_t tx, int16_t ty) {
	if(tx < 0 || tx >= LEVEL_TILES_W || ty < 0 || ty >= LEVEL_TILES_H) {
		return TILE_SKY;
	}
	switch(s_pLevel[ty][tx]) {
		case 'c': return TILE_CLOUD;
		case '#': return TILE_GRASS;
		case '=': return TILE_DIRT;
		case 'B': return TILE_BRICK;
		case '|': return TILE_COLUMN;
		case 'T': return TILE_COLUMN_TOP;
		case 'R': return TILE_SIGN_RED;
		case 'G': return TILE_SIGN_GREEN;
		case 'U': return TILE_SIGN_BLUE;
		default: return TILE_SKY;
	}
}

static int16_t clamp(int16_t v, int16_t lo, int16_t hi) {
	return v < lo ? lo : (v > hi ? hi : v);
}

int16_t cameraFollow(int16_t playerX) {
	return clamp(playerX + PLAYER_W / 2 - SCREEN_W / 2, 0, CAMERA_MAX_X);
}

void logicInit(tGameState *pState) {
	pState->x = PLAYER_START_X;
	pState->y = PLAYER_Y;
	pState->cameraX = cameraFollow(pState->x);
	pState->frame = 0;
	pState->isMoving = 0;
}

uint8_t logicUpdate(tGameState *pState, const tInput *pInput) {
	int16_t oldX = pState->x, oldCam = pState->cameraX;
	uint8_t wasMoving = pState->isMoving;
	int16_t speed = pInput->fire ? PLAYER_RUN_SPEED : PLAYER_WALK_SPEED;

	pState->x = clamp(pState->x + pInput->dx * speed, 0, LEVEL_W - PLAYER_W);
	pState->cameraX = cameraFollow(pState->x);
	pState->isMoving = pState->x != oldX;
	++pState->frame;
	return pState->x != oldX || pState->cameraX != oldCam || pState->isMoving != wasMoving;
}
