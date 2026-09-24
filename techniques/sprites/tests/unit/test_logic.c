// Host-side tests for src/logic.c. Run with: agk unit
#include <stdio.h>
#include "logic.h"

static int s_failures;

#define CHECK(cond) do { \
	if(!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); ++s_failures; } \
} while(0)

static tGameState run(tInput in, int frames) {
	tGameState s;
	logicInit(&s);
	for(int i = 0; i < frames; ++i) logicUpdate(&s, &in);
	return s;
}

static void testStartsCentered(void) {
	tGameState s;
	logicInit(&s);
	CHECK(s.x == 152 && s.y == 100);
}

static void testMovesTwoPixelsPerFrame(void) {
	tGameState s = run((tInput){.dx = 1}, 10);
	CHECK(s.x == 172);
	CHECK(s.frame == 10);
}

static void testStopsAtRightPillar(void) {
	// Right pillar starts at x=240; the 16px-wide player stops touching it.
	tGameState s = run((tInput){.dx = 1}, 100);
	CHECK(s.x == 224);
	CHECK(s.isBlocked);
	CHECK(s.bumps == 1);
}

static void testClampsToScreen(void) {
	tGameState s = run((tInput){.dy = -1}, 200);
	CHECK(s.y == 0);
	CHECK(!s.isBlocked);
}

static void testSlidesAlongWall(void) {
	// Pressing right+down against the pillar still moves down.
	tGameState s = run((tInput){.dx = 1, .dy = 1}, 40);
	CHECK(s.x == 224);
	CHECK(s.y == 180);
}

static void testEnemyStartsAtLeftEnd(void) {
	tGameState s;
	logicInit(&s);
	CHECK(s.enemyX == ENEMY_MIN_X && s.enemyDir == 1);
	CHECK(s.lives == 3 && !s.isGameOver);
}

static void testEnemyPatrolsOnePixelPerFrame(void) {
	tGameState s;
	logicInit(&s);
	for(int i = 0; i < 10; ++i) logicUpdateEnemy(&s);
	CHECK(s.enemyX == 100);
}

static void testEnemyTurnsAtRightEnd(void) {
	tGameState s;
	logicInit(&s);
	for(int i = 0; i < 120; ++i) logicUpdateEnemy(&s);
	CHECK(s.enemyX == 210);
	CHECK(s.enemyDir == -1);
	logicUpdateEnemy(&s);
	CHECK(s.enemyX == 209);
}

static void testEnemyTurnsAtLeftEndAndStaysInRange(void) {
	tGameState s;
	logicInit(&s);
	int16_t lo = 1000, hi = -1000;
	for(int i = 0; i < 1000; ++i) {
		logicUpdateEnemy(&s);
		if(s.enemyX < lo) lo = s.enemyX;
		if(s.enemyX > hi) hi = s.enemyX;
	}
	CHECK(lo == 90 && hi == 210);
	// Full cycle is 240 frames: back at the left end moving right.
	logicInit(&s);
	for(int i = 0; i < 240; ++i) logicUpdateEnemy(&s);
	CHECK(s.enemyX == 90);
	logicUpdateEnemy(&s);
	CHECK(s.enemyX == 91 && s.enemyDir == 1);
}

static void testEnemyMovesDuringLogicUpdate(void) {
	tGameState s = run((tInput){0}, 5);
	CHECK(s.enemyX == 95);
}

static void testOverlapIsStrict(void) {
	tGameState s;
	logicInit(&s);
	s.enemyX = 100;
	s.x = 100 - PLAYER_W; s.y = ENEMY_Y;      // touching left edge
	CHECK(!logicPlayerHitsEnemy(&s));
	s.x = 100 - PLAYER_W + 1;                 // 1px overlap
	CHECK(logicPlayerHitsEnemy(&s));
	s.x = 100 + ENEMY_W; s.y = ENEMY_Y;       // touching right edge
	CHECK(!logicPlayerHitsEnemy(&s));
	s.x = 110; s.y = ENEMY_Y - PLAYER_H;      // touching top
	CHECK(!logicPlayerHitsEnemy(&s));
	s.y = ENEMY_Y + ENEMY_H - 1;              // 1px overlap at bottom
	CHECK(logicPlayerHitsEnemy(&s));
}

static void testHitLosesLifeAndRespawns(void) {
	// Let the enemy patrol for 30 frames (x=120, spanning 120..151), then walk
	// down from the start (x 152..167): by the time the player reaches y=136
	// the enemy has moved under it.
	tGameState s;
	logicInit(&s);
	tInput none = {0}, down = {.dy = 1};
	for(int i = 0; i < 30; ++i) logicUpdate(&s, &none);
	CHECK(s.enemyX == 120);
	uint8_t hitFrame = 0;
	for(int i = 0; i < 40 && !hitFrame; ++i) {
		logicUpdate(&s, &down);
		if(s.wasHit) hitFrame = 1;
	}
	CHECK(hitFrame);
	CHECK(s.lives == 2);
	CHECK(s.x == START_X && s.y == START_Y);
	CHECK(!s.isGameOver);
	// Next frame: no longer overlapping, no further loss.
	logicUpdate(&s, &none);
	CHECK(!s.wasHit && s.lives == 2);
}

static void testGameOverAfterThreeHitsFreezesPlayer(void) {
	tGameState s;
	logicInit(&s);
	tInput down = {.dy = 1}, right = {.dx = 1};
	int hits = 0;
	for(int i = 0; i < 5000 && !s.isGameOver; ++i) {
		logicUpdate(&s, &down);
		if(s.wasHit) ++hits;
	}
	CHECK(s.isGameOver);
	CHECK(hits == 3);
	CHECK(s.lives == 0);
	int16_t x = s.x, y = s.y, ex = s.enemyX;
	logicUpdate(&s, &right);
	logicUpdate(&s, &down);
	CHECK(s.x == x && s.y == y);   // player frozen
	CHECK(s.enemyX != ex);          // enemy keeps patrolling
	CHECK(s.lives == 0);
}

int main(void) {
	testEnemyStartsAtLeftEnd();
	testEnemyPatrolsOnePixelPerFrame();
	testEnemyTurnsAtRightEnd();
	testEnemyTurnsAtLeftEndAndStaysInRange();
	testEnemyMovesDuringLogicUpdate();
	testOverlapIsStrict();
	testHitLosesLifeAndRespawns();
	testGameOverAfterThreeHitsFreezesPlayer();
	testStartsCentered();
	testMovesTwoPixelsPerFrame();
	testStopsAtRightPillar();
	testClampsToScreen();
	testSlidesAlongWall();
	if(s_failures) {
		printf("%d check(s) failed\n", s_failures);
		return 1;
	}
	printf("all logic tests passed\n");
	return 0;
}
