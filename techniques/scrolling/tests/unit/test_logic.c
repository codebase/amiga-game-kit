// Host-side tests for src/logic.c. Run with: agk unit
#include <stdio.h>
#include "logic.h"

static int s_failures;

#define CHECK(cond) do { \
	if(!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); ++s_failures; } \
} while(0)

static tGameState run(tGameState s, tInput in, int frames) {
	for(int i = 0; i < frames; ++i) logicUpdate(&s, &in);
	return s;
}

static tGameState start(void) {
	tGameState s;
	logicInit(&s);
	return s;
}

static void testLevelIsThreeScreensWide(void) {
	CHECK(LEVEL_W == 960);
	CHECK(LEVEL_W >= 3 * SCREEN_W);
	CHECK(LEVEL_H == SCREEN_H);
	CHECK(CAMERA_MAX_X == 640);
}

static void testLevelTiles(void) {
	// Ground spans the whole level; the last column proves each row string
	// is really LEVEL_TILES_W long.
	for(int16_t x = 0; x < LEVEL_TILES_W; ++x) {
		CHECK(levelTileAt(x, 14) == TILE_GRASS);
		CHECK(levelTileAt(x, 15) == TILE_DIRT);
		CHECK(levelTileAt(x, 0) == TILE_SKY);
	}
	// Test markers the emulator tests look for.
	CHECK(levelTileAt(10, 13) == TILE_SIGN_RED);
	CHECK(levelTileAt(30, 13) == TILE_SIGN_GREEN);
	CHECK(levelTileAt(58, 13) == TILE_SIGN_BLUE);
	CHECK(levelTileAt(9, 13) == TILE_SKY);
	CHECK(levelTileAt(29, 13) == TILE_SKY);
	CHECK(levelTileAt(57, 13) == TILE_SKY);
	CHECK(levelTileAt(27, 9) == TILE_COLUMN_TOP);
	CHECK(levelTileAt(27, 12) == TILE_COLUMN);
	CHECK(levelTileAt(54, 12) == TILE_COLUMN);
	CHECK(levelTileAt(20, 8) == TILE_BRICK);
	CHECK(levelTileAt(3, 1) == TILE_CLOUD);
	// Outside the level reads as sky, never out of bounds.
	CHECK(levelTileAt(-1, 14) == TILE_SKY);
	CHECK(levelTileAt(LEVEL_TILES_W, 14) == TILE_SKY);
	CHECK(levelTileAt(0, LEVEL_TILES_H) == TILE_SKY);
}

static void testCameraFollowCentresAndClamps(void) {
	CHECK(cameraFollow(0) == 0);
	CHECK(cameraFollow(152) == 0);    // player centre at screen centre
	CHECK(cameraFollow(153) == 1);    // from here the camera moves 1:1
	CHECK(cameraFollow(433) == 281);
	CHECK(cameraFollow(792) == 640);  // last position that still centres
	CHECK(cameraFollow(793) == 640);
	CHECK(cameraFollow(LEVEL_W - PLAYER_W) == 640);
}

static void testStartsAtLeftEdge(void) {
	tGameState s = start();
	CHECK(s.x == 32 && s.y == 208 && s.cameraX == 0);
	CHECK(worldToScreenX(10 * TILE_SIZE, s.cameraX) == 160);  // red sign
}

static void testWalksOnePixelRunsTwo(void) {
	tGameState s = run(start(), (tInput){.dx = 1}, 10);
	CHECK(s.x == 42 && s.isMoving);
	s = run(start(), (tInput){.dx = 1, .fire = 1}, 10);
	CHECK(s.x == 52);
	CHECK(s.frame == 10);
}

static void testCameraScrollsWithPlayer(void) {
	// The same inputs the emulator test 'scroll' uses.
	tGameState s = run(start(), (tInput){.dx = 1, .fire = 1}, 200);
	CHECK(s.x == 432 && s.cameraX == 280);
	s = run(s, (tInput){.dx = 1}, 1);
	CHECK(s.x == 433 && s.cameraX == 281);   // odd scroll offset
	CHECK(worldToScreenX(s.x, s.cameraX) == 152);
	CHECK(worldToScreenX(30 * TILE_SIZE, s.cameraX) == 199);  // green sign
}

static void testClampsAtRightEdge(void) {
	tGameState s = run(start(), (tInput){.dx = 1, .fire = 1}, 1000);
	CHECK(s.x == LEVEL_W - PLAYER_W);
	CHECK(s.cameraX == CAMERA_MAX_X);
	CHECK(!s.isMoving);
	CHECK(worldToScreenX(58 * TILE_SIZE, s.cameraX) == 288);  // blue sign
	CHECK(worldToScreenX(s.x, s.cameraX) == 304);
}

static void testClampsAtLeftEdge(void) {
	tGameState s = run(start(), (tInput){.dx = -1, .fire = 1}, 100);
	CHECK(s.x == 0 && s.cameraX == 0);
}

static void testReturnsToStartView(void) {
	tGameState s = run(start(), (tInput){.dx = 1, .fire = 1}, 1000);
	s = run(s, (tInput){.dx = -1, .fire = 1}, 400);
	CHECK(s.x == 144 && s.cameraX == 0);
}

static void testReportsChanges(void) {
	tGameState s = start();
	tInput none = {0}, right = {.dx = 1};
	CHECK(logicUpdate(&s, &right) == 1);
	CHECK(logicUpdate(&s, &none) == 1);   // stopped: isMoving changed
	CHECK(logicUpdate(&s, &none) == 0);
}

int main(void) {
	testLevelIsThreeScreensWide();
	testLevelTiles();
	testCameraFollowCentresAndClamps();
	testStartsAtLeftEdge();
	testWalksOnePixelRunsTwo();
	testCameraScrollsWithPlayer();
	testClampsAtRightEdge();
	testClampsAtLeftEdge();
	testReturnsToStartView();
	testReportsChanges();
	if(s_failures) {
		printf("%d check(s) failed\n", s_failures);
		return 1;
	}
	printf("all logic tests passed\n");
	return 0;
}
