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

int main(void) {
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
