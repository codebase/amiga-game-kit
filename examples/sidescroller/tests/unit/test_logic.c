// Host-side tests for src/logic.c. Run with: agk unit
#include <stdio.h>
#include "logic.h"

static int s_failures;

#define CHECK(cond) do { \
	if(!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); ++s_failures; } \
} while(0)

static void step(tGameState *s, int8_t dx, uint8_t jump, int frames) {
	tInput in = {.dx = dx, .jump = jump};
	for(int i = 0; i < frames; ++i) logicUpdate(s, &in);
}

static void place(tGameState *s, int16_t x, int16_t y) {
	logicInit(s);
	s->x = x; s->y = y; s->yFix = y << FIX_SHIFT; s->vy = 0; s->onGround = 0;
	step(s, 0, 0, 1); // settle
}

static void testLevelShape(void) {
	CHECK(logicTileAt(0, 13) == TILE_GRASS);
	CHECK(logicTileAt(0, 14) == TILE_DIRT);
	CHECK(logicTileAt(22, 13) == TILE_EMPTY); // first pit
	CHECK(logicTileAt(8, 10) == TILE_STONE);
	CHECK(logicTileAt(-1, 13) == TILE_EMPTY && logicTileAt(80, 13) == TILE_EMPTY);
	CHECK(logicTileAt(5, -1) == TILE_EMPTY && logicTileAt(5, 16) == TILE_EMPTY);
}

static void testStartsOnGround(void) {
	tGameState s;
	logicInit(&s);
	CHECK(s.x == START_X && s.y == START_Y && s.onGround);
	step(&s, 0, 0, 10);
	CHECK(s.y == START_Y && s.onGround && s.walkFrame == 0);
}

static void testRunsTwoPixelsPerFrame(void) {
	tGameState s;
	logicInit(&s);
	step(&s, 1, 0, 10);
	CHECK(s.x == START_X + 20);
	CHECK(s.onGround);
	CHECK(s.frame == 10);
}

static void testWalkAnimation(void) {
	tGameState s;
	logicInit(&s);
	uint8_t seen[WALK_FRAMES] = {0};
	for(int i = 0; i < 4 * WALK_FRAME_TICKS * WALK_FRAMES; ++i) {
		step(&s, 1, 0, 1);
		seen[s.walkFrame] = 1;
	}
	CHECK(seen[0] && seen[1] && seen[2] && seen[3]);
	step(&s, 0, 0, 1);
	CHECK(s.walkFrame == 0); // standing
}

static void testJumpArc(void) {
	tGameState s;
	logicInit(&s);
	step(&s, 0, 1, 1);
	CHECK(!s.onGround && s.jumps == 1);
	int16_t minY = s.y;
	int frames = 1;
	while(!s.onGround && frames < 200) {
		step(&s, 0, 0, 1);
		if(s.y < minY) minY = s.y;
		++frames;
	}
	int16_t height = START_Y - minY;
	printf("jump: height %d px, airtime %d frames\n", height, frames);
	CHECK(height >= 50 && height <= 60);   // > 3 tiles: reaches platforms 3 rows up
	CHECK(frames >= 30 && frames <= 40);
	CHECK(s.y == START_Y && s.vy == 0);     // back on the ground, exactly
}

static void testJumpNeedsRelease(void) {
	tGameState s;
	logicInit(&s);
	step(&s, 0, 1, 100); // hold jump through one full arc and beyond
	CHECK(s.jumps == 1);
	CHECK(s.onGround);
	step(&s, 0, 0, 1);
	step(&s, 0, 1, 1);
	CHECK(s.jumps == 2);
}

static void testLandsOnPlatformFromBelow(void) {
	// Stone slab at row 10, columns 8..11 (x 128..191): jump through it from
	// below (one-way) and land on its top, y = 160 - 16.
	tGameState s;
	logicInit(&s);
	s.x = 144;
	step(&s, 0, 1, 1);
	step(&s, 0, 0, 60);
	CHECK(s.onGround);
	CHECK(s.y == 10 * TILE_SIZE - PLAYER_H);
}

static void testFallsOffEdge(void) {
	tGameState s;
	place(&s, 144, 10 * TILE_SIZE - PLAYER_H);
	CHECK(s.onGround && s.y == 144);
	// Walk right off the slab's end (x 191): falls to the ground (row 13).
	int16_t lastGroundY = s.y;
	uint8_t wasAirborne = 0;
	for(int i = 0; i < 60; ++i) {
		step(&s, 1, 0, 1);
		if(!s.onGround) wasAirborne = 1;
		if(s.onGround) lastGroundY = s.y;
	}
	CHECK(wasAirborne);
	CHECK(lastGroundY == START_Y);
	CHECK(s.onGround && s.y == START_Y);
}

static void testSolidGroundFromSide(void) {
	// In the first pit (x 352..399) at the bottom-ish: the far wall blocks.
	tGameState s;
	place(&s, 372, 13 * TILE_SIZE); // inside the pit, below the grass line
	CHECK(!s.onGround);
	int16_t x0 = s.x;
	step(&s, 1, 0, 3);
	CHECK(s.x - x0 <= 400 - (x0 + PLAYER_HB_R + 1)); // stopped at the wall at x=400
}

static void testPitRespawns(void) {
	tGameState s;
	logicInit(&s);
	step(&s, 1, 0, 200); // runs into the first pit
	CHECK(s.deaths >= 1);
	CHECK(s.x <= 22 * TILE_SIZE); // respawned, running again from the start
}

static void testLevelEdges(void) {
	tGameState s;
	logicInit(&s);
	step(&s, -1, 0, 50);
	CHECK(s.x == 0);
	place(&s, LEVEL_W - PLAYER_W - 4, START_Y);
	step(&s, 1, 0, 50);
	CHECK(s.x == LEVEL_W - PLAYER_W);
	CHECK(s.cam == CAM_MAX);
}

static void testCameraClamp(void) {
	CHECK(logicCameraFor(0) == 0);
	CHECK(logicCameraFor(152) == 0);
	CHECK(logicCameraFor(153) == 1);
	CHECK(logicCameraFor(500) == 348);
	CHECK(logicCameraFor(LEVEL_W - PLAYER_W) == CAM_MAX);
	CHECK(CAM_MAX == 960);
	tGameState s;
	logicInit(&s);
	step(&s, 1, 0, 80);  // x = 192
	CHECK(s.cam == s.x + 8 - 160);
}

static void testParallaxOffsets(void) {
	for(int16_t cam = 0; cam <= CAM_MAX; cam += 7) {
		CHECK(logicBandOffset(cam, MOUNTAINS_SHIFT, 384) == (cam / 4) % 384);
		CHECK(logicBandOffset(cam, HILLS_SHIFT, 384) == (cam / 2) % 384);
	}
	CHECK(logicBandOffset(960, HILLS_SHIFT, 384) == 96);   // wrapped
	CHECK(logicBandOffset(767, HILLS_SHIFT, 384) == 383);
	CHECK(logicBandOffset(768, HILLS_SHIFT, 384) == 0);
	CHECK(logicBandOffset(960, MOUNTAINS_SHIFT, 384) == 240);
}

static void testScrollRegisters(void) {
	// pixel column X at screen x 0, one extra fetch word to the left
	CHECK(logicScrollByteOffset(0) == -2 && logicScrollDelay(0) == 0);
	CHECK(logicScrollByteOffset(1) == 0 && logicScrollDelay(1) == 15);
	CHECK(logicScrollByteOffset(15) == 0 && logicScrollDelay(15) == 1);
	CHECK(logicScrollByteOffset(16) == 0 && logicScrollDelay(16) == 0);
	CHECK(logicScrollByteOffset(17) == 2 && logicScrollDelay(17) == 15);
	CHECK(logicScrollByteOffset(960) == 118 && logicScrollDelay(960) == 0);
	// The fetch never runs past a 704-px band bitmap (384 loop + 320 wrap)
	for(uint16_t o = 0; o < 384; ++o) {
		CHECK(logicScrollByteOffset(o) * 8 + 21 * 16 <= 704);
	}
}

static void testGradients(void) {
	CHECK(logicSkyColor(0) == 0x114);
	CHECK(logicSkyColor(SKY_BANDS - 1) == 0xFB6);
	for(uint8_t i = 1; i < SKY_BANDS; ++i) {
		uint16_t a = logicSkyColor(i - 1), b = logicSkyColor(i);
		for(uint8_t s = 0; s <= 8; s += 4) {
			int d = ((b >> s) & 15) - ((a >> s) & 15);
			CHECK(d >= -1 && d <= 1); // smooth: one step per channel at most
		}
	}
	CHECK(logicHazeColor(0) == 0xBDE && logicHazeColor(HAZE_BANDS - 1) == 0x7AC);
}

static void testHeroFrames(void) {
	tGameState s;
	logicInit(&s);
	CHECK(logicHeroFrame(&s) == HERO_IDLE);
	s.frame = 1 << HERO_BREATHE_SHIFT;
	CHECK(logicHeroFrame(&s) == HERO_BREATHE);
	s.moving = 1;
	s.walkFrame = 3;
	CHECK(logicHeroFrame(&s) == HERO_WALK + 3);
	s.onGround = 0;
	s.vy = -10;
	CHECK(logicHeroFrame(&s) == HERO_JUMP);
	s.vy = 10;
	CHECK(logicHeroFrame(&s) == HERO_FALL);
	tInput in = {.dx = -1};
	logicInit(&s);
	logicUpdate(&s, &in);
	CHECK(s.facingLeft == 1 && s.moving == 1);
}

int main(void) {
	testHeroFrames();
	testLevelShape();
	testStartsOnGround();
	testRunsTwoPixelsPerFrame();
	testWalkAnimation();
	testJumpArc();
	testJumpNeedsRelease();
	testLandsOnPlatformFromBelow();
	testFallsOffEdge();
	testSolidGroundFromSide();
	testPitRespawns();
	testLevelEdges();
	testCameraClamp();
	testParallaxOffsets();
	testScrollRegisters();
	testGradients();
	if(s_failures) {
		printf("%d check(s) failed\n", s_failures);
		return 1;
	}
	printf("all logic tests passed\n");
	return 0;
}
