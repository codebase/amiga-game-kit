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
	// The player can't walk into the HUD band at the top.
	tGameState s = run((tInput){.dy = -1}, 200);
	CHECK(s.y == HUD_H);
	CHECK(!s.isBlocked);
}

static void testSlidesAlongWall(void) {
	// Pressing right+down against the pillar still moves down.
	tGameState s = run((tInput){.dx = 1, .dy = 1}, 40);
	CHECK(s.x == 224);
	CHECK(s.y == 180);
}

static int channel(uint16_t c, int i) {
	return (c >> (8 - 4 * i)) & 0xF;
}

static void testSkyStartsAndEndsOnKeyColours(void) {
	uint16_t pBands[SKY_BANDS];
	logicBuildSkyBands(pBands);
	CHECK(pBands[0] == 0x000);
	CHECK(pBands[SKY_BANDS - 1] == 0xFFB);
}

static void testSkyHasMoreColoursThanThePalette(void) {
	// The point of the copper gradient: 58 background colours on a 4-colour
	// (2-bitplane) screen, more than even a 32-colour palette could hold.
	uint16_t pBands[SKY_BANDS];
	logicBuildSkyBands(pBands);
	int distinct = 0;
	for(int i = 0; i < SKY_BANDS; ++i) {
		int isNew = 1;
		for(int j = 0; j < i; ++j) {
			if(pBands[j] == pBands[i]) isNew = 0;
		}
		distinct += isNew;
	}
	CHECK(distinct == SKY_BANDS);
	CHECK(distinct > 32);
}

static void testSkyIsSmooth(void) {
	// Neighbouring bands differ by exactly one step in exactly one channel.
	uint16_t pBands[SKY_BANDS];
	logicBuildSkyBands(pBands);
	for(int i = 1; i < SKY_BANDS; ++i) {
		int diff = 0;
		for(int c = 0; c < 3; ++c) {
			int d = channel(pBands[i], c) - channel(pBands[i - 1], c);
			diff += d < 0 ? -d : d;
		}
		CHECK(diff == 1);
	}
}

static void testLineColourUsesBands(void) {
	uint16_t pBands[SKY_BANDS];
	logicBuildSkyBands(pBands);
	int16_t noBar = -100;
	CHECK(logicLineColor(pBands, SKY_TOP, noBar) == pBands[0]);
	CHECK(logicLineColor(pBands, SKY_TOP + 3, noBar) == pBands[0]);
	CHECK(logicLineColor(pBands, SKY_TOP + 4, noBar) == pBands[1]);
	CHECK(logicLineColor(pBands, WORLD_H - 1, noBar) == pBands[SKY_BANDS - 1]);
}

static void testBarCoversSky(void) {
	uint16_t pBands[SKY_BANDS];
	logicBuildSkyBands(pBands);
	CHECK(logicLineColor(pBands, 99, 100) == pBands[(99 - SKY_TOP) / 4]);
	CHECK(logicLineColor(pBands, 100, 100) == logicBarColor(0));
	CHECK(logicLineColor(pBands, 107, 100) == 0xEFF); // brightest, middle
	CHECK(logicLineColor(pBands, 115, 100) == logicBarColor(15));
	CHECK(logicLineColor(pBands, 116, 100) == pBands[(116 - SKY_TOP) / 4]);
}

static void testBarShadingIsSymmetric(void) {
	for(int i = 0; i < BAR_H; ++i) {
		CHECK(logicBarColor(i) == logicBarColor(BAR_H - 1 - i));
	}
	CHECK(logicBarColor(0) == 0x013);
	CHECK(logicBarColor(7) == 0xEFF);
}

static void testBarBouncesAsFunctionOfFrame(void) {
	CHECK(logicBarY(0) == BAR_MIN_Y);
	CHECK(logicBarY(127) == BAR_MAX_Y);
	CHECK(logicBarY(128) == BAR_MAX_Y);
	CHECK(logicBarY(10) == BAR_MIN_Y + (10 * 13 >> 3));
	CHECK(logicBarY(BAR_PERIOD + 10) == logicBarY(10));
	CHECK(logicBarY(65535) == logicBarY(255));
	for(int f = 0; f < BAR_PERIOD; ++f) {
		int16_t y = logicBarY(f);
		// Stays inside the sky, and moves at most 2 lines per frame.
		CHECK(y >= SKY_TOP && y + BAR_H <= WORLD_H);
		int16_t d = y - logicBarY(f + 1);
		CHECK(d >= -2 && d <= 2);
	}
	// The bar reaches below beam line 255 (game y 211): the copper list must
	// handle the WAIT wrap there.
	CHECK(BAR_MAX_Y + BAR_H > 211);
}

int main(void) {
	testSkyStartsAndEndsOnKeyColours();
	testSkyHasMoreColoursThanThePalette();
	testSkyIsSmooth();
	testLineColourUsesBands();
	testBarCoversSky();
	testBarShadingIsSymmetric();
	testBarBouncesAsFunctionOfFrame();
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
