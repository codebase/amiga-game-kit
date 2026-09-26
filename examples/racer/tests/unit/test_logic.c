// Host-side tests for src/logic.c. Run with: agk unit
#include <stdio.h>
#include <stdlib.h>
#include "logic.h"

static int s_failures;

#define CHECK(cond) do { \
	if(!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); ++s_failures; } \
} while(0)

static tRoadLine s_pLines[REGION_LINES];
#define LINE(y) (s_pLines[(y) - REGION_TOP])

static void drive(tGameState *s, tInput in, int frames) {
	for(int i = 0; i < frames; ++i) logicUpdate(s, &in);
}

// ------------------------------------------------------------ road bitmap

static void testRoadRows(void) {
	// Wider towards the bottom, symmetric, grass | rumble | asphalt with lane marks
	CHECK(logicRoadHalf(0) == 1 && logicRoadHalf(ROAD_ROWS - 1) == ROAD_HALF_MAX);
	for(int r = 1; r < ROAD_ROWS; ++r) CHECK(logicRoadHalf(r) >= logicRoadHalf(r - 1));
	int r = ROAD_ROWS - 1, hw = logicRoadHalf(r);
	CHECK(logicRoadPixel(r, ROAD_CX) == ROADPIX_ASPHALT);
	CHECK(logicRoadPixel(r, ROAD_CX + hw) == ROADPIX_GRASS);
	CHECK(logicRoadPixel(r, ROAD_CX + hw - 1) == ROADPIX_RUMBLE);
	CHECK(logicRoadPixel(r, ROAD_CX - hw) == ROADPIX_RUMBLE);   // left edge: column CX-1-(hw-1)
	CHECK(logicRoadPixel(r, ROAD_CX - hw - 1) == ROADPIX_GRASS);
	CHECK(logicRoadPixel(r, ROAD_CX + hw / 3) == ROADPIX_LANE);
	CHECK(logicRoadPixel(r, ROAD_CX - 1 - hw / 3) == ROADPIX_LANE);
	for(int x = 0; x < ROAD_BMP_W / 2; ++x) {   // mirror image
		CHECK(logicRoadPixel(r, ROAD_CX + x) == logicRoadPixel(r, ROAD_CX - 1 - x));
	}
}

// ------------------------------------------------------------ projection

static void testFlatStraightRoad(void) {
	// The start is flat and straight: row r on line HORIZON_Y + 1 + r, centred
	tGameState s;
	logicInit(&s);
	logicRoadLines(&s, s_pLines);
	for(int y = REGION_TOP; y <= HORIZON_Y; ++y) CHECK(LINE(y).row == ROW_SKY);
	// the farthest projected row is (ROAD_ROWS - 1) % ROW_STEP
	for(int r = (ROAD_ROWS - 1) % ROW_STEP; r < ROAD_ROWS; ++r) {
		// every ROW_STEP-th row is projected; the lines between show the farther one
		int shown = LINE(HORIZON_Y + 1 + r).row;
		CHECK(shown <= r && shown > r - ROW_STEP && (ROAD_ROWS - 1 - shown) % ROW_STEP == 0);
		CHECK(LINE(HORIZON_Y + 1 + r).left == ROAD_CX - SCREEN_W / 2);
	}
}

static void testStripesMove(void) {
	// Driving forward moves the stripe pattern down the screen (towards you)
	tGameState s;
	logicInit(&s);
	logicRoadLines(&s, s_pLines);
	int y0 = -1;
	for(int y = SCREEN_H - 1; y > HORIZON_Y; --y) {
		if(LINE(y).dark != LINE(SCREEN_H - 1).dark) { y0 = y; break; }   // first stripe edge from the bottom
	}
	CHECK(y0 > HORIZON_Y);
	s.pos += 4;
	logicRoadLines(&s, s_pLines);
	int y1 = -1;
	for(int y = SCREEN_H - 1; y > HORIZON_Y; --y) {
		if(LINE(y).dark != LINE(SCREEN_H - 1).dark) { y1 = y; break; }
	}
	CHECK(y1 > y0);
}

static void testSteeringShiftsTheRoad(void) {
	// Player to the right: the road slides left, more at the bottom (perspective)
	tGameState s;
	logicInit(&s);
	s.x = X_ROAD_EDGE / 2;
	logicRoadLines(&s, s_pLines);
	int near = LINE(SCREEN_H - 1).left - (ROAD_CX - SCREEN_W / 2);
	int far = LINE(HORIZON_Y + 10).left - (ROAD_CX - SCREEN_W / 2);
	CHECK(near > 0 && far >= 0 && near > far);
	CHECK(near == ROAD_HALF_MAX / 2);
}

static uint32_t findCurve(int sign) {
	for(uint32_t p = 0; p < TRACK_LEN; p += SEG_LEN) {
		if(logicCurveAt(p) * sign > 0 && logicCurveAt(p + 10 * SEG_LEN) * sign > 0) return p;
	}
	return 0;
}

static void testCurvesBendTheFarRoad(void) {
	// On a right curve the far rows shift right (left column decreases) more than near rows
	tGameState s;
	logicInit(&s);
	s.pos = findCurve(1);
	CHECK(s.pos != 0);
	logicRoadLines(&s, s_pLines);
	int nearShift = (ROAD_CX - SCREEN_W / 2) - LINE(SCREEN_H - 1).left;
	int farRow = -1;
	for(int y = REGION_TOP; y < SCREEN_H; ++y) if(LINE(y).row != ROW_SKY) { farRow = y; break; }
	CHECK(farRow > 0);
	int farShift = (ROAD_CX - SCREEN_W / 2) - LINE(farRow).left;
	CHECK(farShift > nearShift && farShift > 40);
	// and a left curve the other way
	s.pos = findCurve(-1);
	logicRoadLines(&s, s_pLines);
	for(int y = REGION_TOP; y < SCREEN_H; ++y) if(LINE(y).row != ROW_SKY) { farRow = y; break; }
	CHECK(LINE(farRow).left - (ROAD_CX - SCREEN_W / 2) > 40);
}

static void testHills(void) {
	// Climbing: the road ahead rises above the flat horizon. Cresting: the road
	// beyond drops out of sight and more sky shows.
	tGameState s;
	logicInit(&s);
	int top = SCREEN_H, topMin = SCREEN_H, topMax = 0;
	for(uint32_t p = 0; p < TRACK_LEN; p += SEG_LEN / 2) {
		s.pos = p;
		logicRoadLines(&s, s_pLines);
		top = SCREEN_H;
		for(int y = REGION_TOP; y < SCREEN_H; ++y) if(LINE(y).row != ROW_SKY) { top = y; break; }
		if(top < topMin) topMin = top;
		if(top > topMax) topMax = top;
		// every line below the top shows road, and rows never go back up the
		// screen (nearer rows are lower)
		for(int y = top + 1; y < SCREEN_H; ++y) {
			CHECK(LINE(y).row != ROW_SKY && LINE(y).row >= LINE(y - 1).row);
		}
	}
	printf("road top: %d..%d (flat %d)\n", topMin, topMax, HORIZON_Y + 1);
	CHECK(topMin < HORIZON_Y - 10);   // uphill ahead
	CHECK(topMax > HORIZON_Y + 10);   // over a crest
}

static void testTrackLoops(void) {
	// Height is continuous across the start line
	int d = logicHeightAt(TRACK_LEN - 1) - logicHeightAt(0);
	CHECK(abs(d) <= 2);
	for(uint32_t p = 1; p < TRACK_LEN; ++p) {
		CHECK(abs(logicHeightAt(p) - logicHeightAt(p - 1)) <= 3);   // no steps (steepest slope ~1.6)
	}
}

// ---------------------------------------------------------------- driving

static void testAcceleration(void) {
	tGameState s;
	logicInit(&s);
	drive(&s, (tInput){.accel = 1}, 50);
	CHECK(s.speed > 0 && s.pos > 0);
	int after1s = logicKmh(&s);
	drive(&s, (tInput){.accel = 1}, 300);    // still on the start straight
	CHECK(s.speed == SPEED_MAX && logicKmh(&s) == KMH_MAX);
	CHECK(!s.offroad);
	printf("0-%d km/h in 1 s\n", after1s);
	CHECK(after1s > 60 && after1s < 200);
	// brake to a stop, coast slows down
	drive(&s, (tInput){.brake = 1}, 200);
	CHECK(s.speed == 0);
}

static void testSteering(void) {
	tGameState s;
	logicInit(&s);
	drive(&s, (tInput){.steer = 1}, 50);
	CHECK(s.x == 0);                 // standing still: no steering
	drive(&s, (tInput){.accel = 1}, 100);
	int16_t x0 = s.x;
	drive(&s, (tInput){.accel = 1, .steer = 1}, 20);
	CHECK(s.x > x0);
	drive(&s, (tInput){.accel = 1, .steer = -1}, 40);
	CHECK(s.x < x0);
}

static void testOffroadSlowsDown(void) {
	tGameState s;
	logicInit(&s);
	drive(&s, (tInput){.accel = 1}, 400);
	CHECK(s.speed == SPEED_MAX);
	s.x = X_ROAD_EDGE + 60;
	drive(&s, (tInput){.accel = 1}, 200);
	CHECK(s.offroad && s.speed <= SPEED_OFFROAD);
}

static void testCurvesPushYouOut(void) {
	tGameState s;
	logicInit(&s);
	s.pos = findCurve(1);
	s.speed = SPEED_MAX;
	drive(&s, (tInput){.accel = 1}, 30);
	CHECK(s.x < 0);   // right curve: pushed left
}

static void testLap(void) {
	tGameState s;
	logicInit(&s);
	s.pos = TRACK_LEN - 2;
	s.speed = SPEED_MAX;
	drive(&s, (tInput){.accel = 1}, 1);
	CHECK(s.laps == 1 && s.pos < 32);
}

int main(void) {
	testRoadRows();
	testFlatStraightRoad();
	testStripesMove();
	testSteeringShiftsTheRoad();
	testCurvesBendTheFarRoad();
	testHills();
	testTrackLoops();
	testAcceleration();
	testSteering();
	testOffroadSlowsDown();
	testCurvesPushYouOut();
	testLap();
	if(s_failures) {
		printf("%d check(s) failed\n", s_failures);
		return 1;
	}
	printf("all logic tests passed\n");
	return 0;
}
