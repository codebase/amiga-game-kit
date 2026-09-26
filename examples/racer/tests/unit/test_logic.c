// Host-side tests for src/logic.c. Run with: agk unit
#include <stdio.h>
#include <stdlib.h>
#include "logic.h"

static int s_failures;

#define CHECK(cond) do { \
	if(!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); ++s_failures; } \
} while(0)

static tRoadLine s_pLines[REGION_LINES];
static tRoadView s_sView;
#define LINE(y) (s_pLines[(y) - REGION_TOP])

static void drive(tGameState *s, tInput in, int frames) {
	for(int i = 0; i < frames; ++i) logicUpdate(s, &in);
}

// A fresh game with the road to ourselves (the driving rules without traffic)
static void initNoTraffic(tGameState *s) {
	logicInit(s);
	for(uint8_t i = 0; i < TRAFFIC_N; ++i) {
		s->rivals[i].speed = 0;
		s->rivals[i].pos = TRACK_LEN / 2;
	}
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
	logicRoadLines(&s, s_pLines, &s_sView);
	// above the road: the horizon strip (its bottom row on the road's top line), then sky
	int top = REGION_TOP;
	while(LINE(top).row >= ROAD_ROWS) ++top;
	CHECK(top == HORIZON_Y + 1 + (ROAD_ROWS - 1) % ROW_STEP);   // the farthest projected row
	for(int y = REGION_TOP; y < top; ++y) {
		int back = y - (top - BACK_H);
		CHECK(back < 0 ? LINE(y).row == ROW_SKY : LINE(y).row == ROW_BACK + back);
	}
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
	logicRoadLines(&s, s_pLines, &s_sView);
	int y0 = -1;
	for(int y = SCREEN_H - 1; y > HORIZON_Y; --y) {
		if(LINE(y).dark != LINE(SCREEN_H - 1).dark) { y0 = y; break; }   // first stripe edge from the bottom
	}
	CHECK(y0 > HORIZON_Y);
	s.pos += 4;
	logicRoadLines(&s, s_pLines, &s_sView);
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
	logicRoadLines(&s, s_pLines, &s_sView);
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
	logicRoadLines(&s, s_pLines, &s_sView);
	int nearShift = (ROAD_CX - SCREEN_W / 2) - LINE(SCREEN_H - 1).left;
	int farRow = -1;
	for(int y = REGION_TOP; y < SCREEN_H; ++y) if(LINE(y).row < ROAD_ROWS) { farRow = y; break; }
	CHECK(farRow > 0);
	int farShift = (ROAD_CX - SCREEN_W / 2) - LINE(farRow).left;
	CHECK(farShift > nearShift && farShift > 40);
	// and a left curve the other way
	s.pos = findCurve(-1);
	logicRoadLines(&s, s_pLines, &s_sView);
	for(int y = REGION_TOP; y < SCREEN_H; ++y) if(LINE(y).row < ROAD_ROWS) { farRow = y; break; }
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
		logicRoadLines(&s, s_pLines, &s_sView);
		top = SCREEN_H;
		for(int y = REGION_TOP; y < SCREEN_H; ++y) if(LINE(y).row < ROAD_ROWS) { top = y; break; }
		if(top < topMin) topMin = top;
		if(top > topMax) topMax = top;
		// every line below the top shows road, and rows never go back up the
		// screen (nearer rows are lower)
		for(int y = top + 1; y < SCREEN_H; ++y) {
			CHECK(LINE(y).row < ROAD_ROWS && LINE(y).row >= LINE(y - 1).row);
		}
		// the horizon strip sits right on top of the road
		if(top - 1 >= REGION_TOP) CHECK(LINE(top - 1).row == ROW_BACK + BACK_H - 1);
	}
	printf("road top: %d..%d (flat %d)\n", topMin, topMax, HORIZON_Y + 1);
	CHECK(topMin < HORIZON_Y - 10);   // uphill ahead
	CHECK(topMax > HORIZON_Y + 10);   // over a crest
}

static void testBackdropDriftsInCurves(void) {
	// The horizon strip scrolls against a curve (bgX), looping every BACK_PERIOD px
	tGameState s;
	logicInit(&s);
	logicRoadLines(&s, s_pLines, &s_sView);
	int left0 = LINE(HORIZON_Y).left;
	s.pos = findCurve(1);
	s.speed = SPEED_MAX;
	for(int i = 0; i < 50; ++i) logicUpdate(&s, &(tInput){.accel = 1, .steer = 1});
	CHECK((int16_t)s.bgX < 0);   // right curve: the scenery slides left
	logicRoadLines(&s, s_pLines, &s_sView);
	int y = REGION_TOP;
	while(LINE(y).row < ROW_BACK || LINE(y).row == ROW_SKY) ++y;
	CHECK(LINE(y).left != left0);
	CHECK(LINE(y).left == LEFT_MIN + ((s.bgX >> 4) & (BACK_PERIOD - 1)));
}

static tObject s_pObj[OBJ_MAX];

static void testObjectsInView(void) {
	// The start straight has palm trees on both sides: they come out farthest
	// first (their feet lower down the screen, bigger), left of and right of the road
	tGameState s;
	logicInit(&s);
	logicRoadLines(&s, s_pLines, &s_sView);
	uint8_t n = logicObjects(&s, &s_sView, s_pObj);
	CHECK(n >= 6);
	int left = 0, right = 0, rivals = 0;
	for(uint8_t i = 0; i < n; ++i) {
		const tObject *o = &s_pObj[i];
		CHECK(o->scale > 0 && o->scale <= 256);
		CHECK(o->y > HORIZON_Y && o->y < SCREEN_H);
		if(i) {
			CHECK(o->y >= s_pObj[i - 1].y && o->scale >= s_pObj[i - 1].scale);
		}
		int hw = (int)o->scale * ROAD_HALF_MAX / 256;   // the road's half width at that depth
		if(o->type == OBJ_RIVAL) {
			// rivals ahead (the first in the left lane): on the road
			++rivals;
			CHECK(abs(o->x - SCREEN_W / 2) <= hw + 1);   // (+1: rounding, far away)
			continue;
		}
		if(o->x < SCREEN_W / 2) ++left; else ++right;
		CHECK(abs(o->x - SCREEN_W / 2) > hw);   // palms: beside the road
	}
	CHECK(left == right);
	CHECK(rivals >= 1);
}

static void testObjectsHideBehindCrests(void) {
	// Wherever the road is hidden, no objects are placed there
	tGameState s;
	logicInit(&s);
	for(uint32_t p = 0; p < TRACK_LEN; p += SEG_LEN) {
		s.pos = p;
		logicRoadLines(&s, s_pLines, &s_sView);
		uint8_t n = logicObjects(&s, &s_sView, s_pObj);
		for(uint8_t i = 0; i < n; ++i) {
			// the object's foot is on a line that shows road or its horizon strip, never sky
			CHECK(LINE(s_pObj[i].y).row != ROW_SKY);
		}
	}
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
	initNoTraffic(&s);
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
	initNoTraffic(&s);
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

// ---------------------------------------------------------------- traffic

// A rival alone on the track, just ahead of the player's car in lane `lane`
static void oneRival(tGameState *pS, int8_t lane, uint32_t ahead) {
	initNoTraffic(pS);
	pS->pos = 1000;
	pS->rivals[0].pos = pS->pos + PLAYER_Z + ahead;
	pS->rivals[0].lane = lane;
	pS->rivals[0].speed = 9 << SPEED_SHIFT;
}

static void testRivalsDrive(void) {
	tGameState s;
	logicInit(&s);
	uint32_t p0 = s.rivals[0].pos;
	drive(&s, (tInput){0}, 50);
	CHECK(s.rivals[0].pos > p0 && s.rivals[0].speed > 0);
	CHECK((s.rivals[0].pos - p0) == (uint32_t)(s.rivals[0].speed * 50) >> SPEED_SHIFT);
}

static void testRearEndSlowsYouDown(void) {
	// Full speed into a rival in our lane: we bounce off its back, down below its speed
	tGameState s;
	oneRival(&s, 0, 400);
	s.speed = SPEED_MAX;
	uint16_t bumpFrame = 0;
	for(uint16_t f = 0; f < 100 && !bumpFrame; ++f) {
		logicUpdate(&s, &(tInput){.accel = 1});
		if(s.bumped) bumpFrame = s.frame;
		// never through it
		int32_t dz = (int32_t)(s.rivals[0].pos - (s.pos + PLAYER_Z));
		CHECK(dz >= CAR_LEN || dz <= -CAR_LEN || !s.bumped);
	}
	CHECK(bumpFrame && s.bumps == 1);
	CHECK(s.speed <= s.rivals[0].speed - BUMP_SLOW);
	CHECK((int32_t)(s.rivals[0].pos - (s.pos + PLAYER_Z)) == CAR_LEN);
}

static void testOtherLanesPass(void) {
	// The same, a lane to the left: we just overtake
	tGameState s;
	oneRival(&s, -1, 400);
	s.speed = SPEED_MAX;
	drive(&s, (tInput){.accel = 1}, 100);
	CHECK(s.bumps == 0 && s.speed == SPEED_MAX);
	CHECK(s.pos + PLAYER_Z > s.rivals[0].pos + CAR_LEN);
}

// ---------------------------------------------------------------- game flow

static void testClockRunsOut(void) {
	// Standing still, the clock runs down: time up, the car can't drive, then the title
	tGameState s;
	initNoTraffic(&s);
	CHECK(s.phase == PHASE_RACE && logicTimeSeconds(&s) == START_TIME / FPS);
	drive(&s, (tInput){0}, FPS);
	CHECK(logicTimeSeconds(&s) == START_TIME / FPS - 1);
	drive(&s, (tInput){0}, START_TIME - FPS);
	CHECK(s.phase == PHASE_OVER && s.message == MSG_TIMEUP && s.time == 0);
	drive(&s, (tInput){.accel = 1}, OVER_FRAMES - 1);
	CHECK(s.phase == PHASE_OVER && s.speed == 0 && s.pos == 0);
	drive(&s, (tInput){.accel = 1}, 1);
	CHECK(s.phase == PHASE_TITLE);
}

static void testCheckpointExtendsTime(void) {
	tGameState s;
	initNoTraffic(&s);
	s.pos = CHECKPOINT_LEN - 200;
	s.speed = SPEED_MAX;
	uint16_t t0 = s.time;
	int frames = 0;
	while(!s.extended && frames < 100) {
		logicUpdate(&s, &(tInput){.accel = 1});
		++frames;
	}
	CHECK(s.extended && s.message == MSG_EXTEND && s.pos >= CHECKPOINT_LEN);
	CHECK(s.time == t0 - frames + EXTEND_TIME);
	CHECK(s.nextCheckpoint == 2 * CHECKPOINT_LEN);
	CHECK(s.score > 0);
	// the message goes away, the next frame doesn't extend again
	logicUpdate(&s, &(tInput){.accel = 1});
	CHECK(!s.extended);
	drive(&s, (tInput){.accel = 1}, MESSAGE_FRAMES);
	CHECK(s.message == MSG_NONE);
}

static void testTitleStartsARace(void) {
	tGameState s;
	logicInit(&s);
	s.score = 1234;
	logicTitle(&s);
	CHECK(s.phase == PHASE_TITLE && s.score == 1234);
	// fire still held from the race: ignored at first
	drive(&s, (tInput){.accel = 1}, TITLE_WAIT);
	CHECK(s.phase == PHASE_TITLE && s.speed == 0 && s.pos == 0);
	drive(&s, (tInput){.accel = 1}, 1);
	CHECK(s.phase == PHASE_RACE && s.score == 0 && s.time == START_TIME && s.pos == 0);
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
	testBackdropDriftsInCurves();
	testObjectsInView();
	testObjectsHideBehindCrests();
	testAcceleration();
	testSteering();
	testOffroadSlowsDown();
	testCurvesPushYouOut();
	testLap();
	testRivalsDrive();
	testRearEndSlowsYouDown();
	testOtherLanesPass();
	testClockRunsOut();
	testCheckpointExtendsTime();
	testTitleStartsARace();
	if(s_failures) {
		printf("%d check(s) failed\n", s_failures);
		return 1;
	}
	printf("all logic tests passed\n");
	return 0;
}
