#include "logic.h"

// 16 x 16 -> 32 bit signed multiply. On the 68000 that's one MULS.W, but GCC
// doesn't always see that two int16 operands fit it and calls the (slow)
// 32-bit ___mulsi3 instead: force it in per-frame code.
// 32 / 16 -> 16 bit unsigned divide (one DIVU.W; the quotient must fit 16
// bits). As plain C, GCC calls the 32-bit ___udivsi3.
static inline uint16_t div16u(uint32_t a, uint16_t b) {
#if defined(__mc68000__)
	uint32_t r = a;
	__asm__("divu.w %1,%0" : "+d"(r) : "dmi"(b));
	return (uint16_t)r;
#else
	return (uint16_t)(a / b);
#endif
}

static inline int32_t mul16(int16_t a, int16_t b) {
#if defined(__mc68000__)
	int32_t r = a;
	__asm__("muls.w %1,%0" : "+d"(r) : "dmi"(b));
	return r;
#else
	return (int32_t)a * b;
#endif
}

// ------------------------------------------------------------------ track ---
// Parts of the loop: length in segments, curve (see logic.h), height change
// over the part (eased in and out). Heights add up to 0 so the loop closes.
static const struct {
	uint16_t len;
	int8_t curve;
	int16_t hill;
	uint8_t palms;    // SCENERY_PALM_L | SCENERY_PALM_R: palm trees every PALM_EVERY segments
} s_pParts[] = {
	{ 45,  0,     0, 3},  // start straight (the whole view ahead is flat)
	{ 30,  0,  5000, 3},  // climb
	{ 35,  2,     0, 1},  // right sweeper over the top
	{ 20,  0, -5000, 2},  // down
	{ 40, -3,     0, 3},  // long left
	{ 10,  0,     0, 0},
	{ 30,  4,  3000, 1},  // right, climbing
	{ 30, -4, -3000, 2},  // left, falling: an S
	{ 15,  0,  1500, 3},  // rolling hills
	{ 15,  0, -1500, 3},
	{ 15,  0,  1500, 3},
	{ 15,  0, -1500, 3},
	{ 35,  1,     0, 1},  // gentle right
	{ 25, -2, -2000, 2},  // left, down to the coast
	{ 20,  0,  2000, 3},
	{ 20,  3,     0, 3},  // right, back to the start line
};
#define PART_COUNT (sizeof(s_pParts) / sizeof(s_pParts[0]))

static int8_t s_pCurve[TRACK_SEGS];
static int16_t s_pHeight[TRACK_SEGS];
static uint8_t s_pScenery[TRACK_SEGS];
#define PALM_EVERY 1

// Per-row tables
static uint16_t s_pZ[ROAD_ROWS];      // depth of row r, units
static uint16_t s_pHalf[ROAD_ROWS];   // road half width, px
static uint16_t s_pRowScale[PROJ_ROWS];   // projected row i: object scale (1/256 of the nearest)
static int16_t s_pRowSide[PROJ_ROWS];     // projected row i: where roadside objects stand, px from the centre
static uint8_t s_isInit;

static void tablesInit(void) {
	if(s_isInit) return;
	s_isInit = 1;
	for(uint16_t r = 0; r < ROAD_ROWS; ++r) {
		s_pZ[r] = ROAD_ZSCALE / (r + 1);
		s_pHalf[r] = (uint16_t)((uint32_t)ROAD_HALF_MAX * (r + 1) / ROAD_ROWS);
	}
	for(uint8_t i = 0; i < PROJ_ROWS; ++i) {
		uint16_t hw = s_pHalf[ROW_STEP * i + 1];
		s_pRowScale[i] = (uint16_t)(((uint32_t)hw << 8) / ROAD_HALF_MAX);
		s_pRowSide[i] = (int16_t)(((int32_t)hw * OBJ_SIDE_X) >> 8);
	}
	uint16_t seg = 0;
	int16_t h = 0;
	for(uint16_t p = 0; p < PART_COUNT; ++p) {
		uint16_t n = s_pParts[p].len;
		for(uint16_t i = 0; i < n && seg < TRACK_SEGS; ++i, ++seg) {
			// smoothstep: t^2 (3 - 2t), t = i/n in 1/256
			int32_t t = ((int32_t)i << 8) / n;
			int32_t ease = (t * t * (768 - 2 * t)) >> 16;   // 0..256
			s_pCurve[seg] = s_pParts[p].curve;
			// palm rows (a bush every 5th), bushes along the bare parts
			uint8_t palms = s_pParts[p].palms, l = 0, r = 0;
			if(palms & SCENERY_PALM_L) l = (seg % 5 == 2 ? OBJ_BUSH : OBJ_PALM) + 1;
			else if(seg % 3 == 0) l = OBJ_BUSH + 1;
			if(palms & SCENERY_PALM_R) r = (seg % 5 == 4 ? OBJ_BUSH : OBJ_PALM) + 1;
			else if(seg % 3 == 1) r = OBJ_BUSH + 1;
			s_pScenery[seg] = (seg % PALM_EVERY) ? 0 : (uint8_t)(l | r << 4);
			s_pHeight[seg] = (int16_t)(h + ((s_pParts[p].hill * ease) >> 8));
		}
		h += s_pParts[p].hill;
	}
	while(seg < TRACK_SEGS) { // (if the parts are shorter than the track)
		s_pCurve[seg] = 0;
		s_pScenery[seg] = 0;
		s_pHeight[seg++] = h;
	}
	// Curve warnings on the outside of each bend, from 8 segments before it,
	// arrows pointing into it; the checkpoints' pillars on both sides
	seg = 0;
	for(uint16_t p = 0; p < PART_COUNT; seg += s_pParts[p++].len) {
		int8_t c = s_pParts[p].curve;
		if(!c || (p && s_pParts[p - 1].curve)) continue;   // (not where a bend goes on)
		for(int16_t k = -8; k <= 6; k += 2) {
			uint16_t i = (uint16_t)((seg + k + TRACK_SEGS) % TRACK_SEGS);
			if(c > 0) s_pScenery[i] = (s_pScenery[i] & 0xF0) | (OBJ_SIGN_R + 1);      // left side
			else s_pScenery[i] = (s_pScenery[i] & 0x0F) | (OBJ_SIGN_L + 1) << 4;     // right side
		}
	}
	for(uint16_t i = 0; i < TRACK_SEGS; i += CHECKPOINT_SEGS) {
		s_pScenery[i] = (OBJ_GATE + 1) | (OBJ_GATE + 1) << 4;
	}
}

static inline uint32_t wrapPos(uint32_t pos) {
	return pos >= TRACK_LEN ? pos - TRACK_LEN : pos;
}

// Lane -1, 0, +1 (index lane + 1): where, and how fast its cars go
static const int16_t s_pLaneX[3] = {-LANE_X, 0, LANE_X};
#define RIVAL_SPEED_MIN (7 << SPEED_SHIFT)
static const int16_t s_pLaneSpeed[3] = {11 << SPEED_SHIFT, 9 << SPEED_SHIFT, RIVAL_SPEED_MIN};

// a - b along the loop, in -TRACK_LEN/2 .. TRACK_LEN/2
static inline int32_t trackDelta(uint32_t a, uint32_t b) {
	int32_t d = (int32_t)(a - b);
	if(d > (int32_t)(TRACK_LEN / 2)) d -= TRACK_LEN;
	else if(d < -(int32_t)(TRACK_LEN / 2)) d += TRACK_LEN;
	return d;
}

// Every other frame, two frames' worth (32 rivals are a noticeable part of a
// 68000 frame). Closing speeds stay under 2 * CAR_LEN per step: no one
// passes through anyone.
_Static_assert(2 * ((SPEED_MAX - RIVAL_SPEED_MIN) >> SPEED_SHIFT) < 2 * CAR_LEN, "a step within the collision window");
static void trafficUpdate(tGameState *pState) {
	uint32_t carPos = pState->pos + PLAYER_Z;
	for(uint8_t i = 0; i < TRAFFIC_N; ++i) {
		tRival *pR = &pState->rivals[i];
		uint16_t step = (uint16_t)pR->posFrac + 2 * (uint16_t)pR->speed;
		pR->pos = wrapPos(pR->pos + (step >> SPEED_SHIFT));
		pR->posFrac = step & 0xFF;

		int32_t dz = trackDelta(pR->pos, carPos);
		if(dz <= -CAR_LEN || dz >= CAR_LEN) continue;
		int16_t dx = pState->x - s_pLaneX[pR->lane + 1];
		if(dx <= -CAR_HIT_X || dx >= CAR_HIT_X) continue;
		if(dz >= 0) {
			// we ran into its back: bounce off, down to its speed
			uint32_t pos = pR->pos + TRACK_LEN - CAR_LEN - PLAYER_Z;
			pState->pos = wrapPos(pos);
			pState->posFrac = 0;
			int16_t s = pR->speed - BUMP_SLOW;
			if(s < 0) s = 0;
			if(pState->speed > s) pState->speed = s;
			pState->bumped = 1;
			++pState->bumps;
			carPos = pState->pos + PLAYER_Z;
		}
		else {
			// it caught up with us (we're slow): it waits behind
			pR->pos = wrapPos(carPos + TRACK_LEN - CAR_LEN);
			pR->posFrac = 0;
		}
	}
}

int8_t logicCurveAt(uint32_t pos) {
	tablesInit();
	return s_pCurve[wrapPos(pos) >> SEG_SHIFT];
}

uint8_t logicSceneryAt(uint32_t pos) {
	tablesInit();
	return s_pScenery[wrapPos(pos) >> SEG_SHIFT];
}

int16_t logicHeightAt(uint32_t pos) {
	tablesInit();
	pos = wrapPos(pos);
	uint16_t seg = pos >> SEG_SHIFT;
	uint16_t next = seg + 1 < TRACK_SEGS ? seg + 1 : 0;
	int16_t h0 = s_pHeight[seg];
	int16_t d = s_pHeight[next] - h0;
	// 16 x 16 -> 32 bit multiply (one MULS)
	return h0 + (int16_t)(mul16(d, (int16_t)(pos & (SEG_LEN - 1))) >> SEG_SHIFT);
}

uint16_t logicRoadHalf(uint8_t r) {
	tablesInit();
	return s_pHalf[r];
}

void logicRoadSpans(uint8_t r, tRoadSpans *pSpans) {
	int16_t hw = logicRoadHalf(r);
	pSpans->half = hw;
	pSpans->rumble = hw / 8 + 1;
	pSpans->lane = hw / 3;         // three lanes: marks a third of the way out
	pSpans->laneHalf = hw / 48;
}

uint8_t logicRoadPixel(uint8_t r, int16_t x) {
	tRoadSpans s;
	logicRoadSpans(r, &s);
	int16_t d = x >= ROAD_CX ? x - ROAD_CX : ROAD_CX - 1 - x;   // symmetric
	if(d >= s.half) return ROADPIX_GRASS;
	if(d >= s.half - s.rumble) return ROADPIX_RUMBLE;
	if(d >= s.lane - s.laneHalf && d <= s.lane + s.laneHalf) return ROADPIX_LANE;
	return ROADPIX_ASPHALT;
}

// ----------------------------------------------------------------- driving

void logicInit(tGameState *pState) {
	tablesInit();
	pState->pos = 0;
	pState->posFrac = 0;
	pState->speed = 0;
	pState->x = 0;
	pState->bgX = 0;
	pState->frame = 0;
	pState->offroad = 0;
	pState->laps = 0;
	pState->bumps = 0;
	pState->bumped = 0;
	pState->phase = PHASE_RACE;
	pState->phaseFrames = 0;
	pState->time = START_TIME;
	pState->score = 0;
	pState->nextCheckpoint = CHECKPOINT_LEN;
	pState->message = MSG_NONE;
	pState->messageFrames = 0;
	pState->extended = 0;
	pState->crash = 0;
	pState->crashed = 0;
	pState->crashes = 0;
	pState->isDemo = 0;
	pState->autoLane = 0;
	pState->autoInput = (tInput){0};
	// spread around the loop at irregular gaps (a fixed pseudo-random
	// sequence: every game is the same), the first one ahead in our lane
	uint16_t rnd = 0xACE1;
	for(uint8_t i = 0; i < TRAFFIC_N; ++i) {
		tRival *pR = &pState->rivals[i];
		rnd = (uint16_t)(rnd * 25173u + 13849u);
		pR->pos = 1200 + i * (TRACK_LEN / TRAFFIC_N) + (i ? (rnd >> 6) : 0);   // + 0..1023
		pR->posFrac = 0;
		pR->lane = i ? (int8_t)((rnd >> 3) % 3) - 1 : 0;
		pR->speed = s_pLaneSpeed[pR->lane + 1];
	}
}

void logicTitle(tGameState *pState) {
	uint32_t score = pState->score;
	uint16_t frame = pState->frame;
	logicInit(pState);
	pState->phase = PHASE_TITLE;
	pState->score = score;
	pState->frame = frame;
}

void logicAutopilot(tGameState *pState, tInput *pOut) {
	// the room ahead in each lane: the nearest rival's distance from our nose
	int32_t pRoom[3] = {TRACK_LEN, TRACK_LEN, TRACK_LEN};
	uint32_t nose = pState->pos + PLAYER_Z;
	for(uint8_t i = 0; i < TRAFFIC_N; ++i) {
		const tRival *pR = &pState->rivals[i];
		int32_t d = trackDelta(pR->pos, nose);
		if(d > -CAR_LEN && d < pRoom[pR->lane + 1]) pRoom[pR->lane + 1] = d;
	}
	int16_t x = pState->x;
	int8_t lane = x < -LANE_X / 2 ? -1 : x > LANE_X / 2 ? 1 : 0;
	int8_t target = pState->autoLane;
	int8_t c = logicCurveAt(pState->pos);
	// in a bend, the inside lane (the push is outwards) if it's clear
	if(c) {
		int8_t inward = lane + (c > 0 ? 1 : -1);   // a lane towards the inside
		if(inward >= -1 && inward <= 1 && pRoom[inward + 1] > 900) target = inward;
	}
	if(pRoom[target + 1] < 450) {
		// a rival close ahead: the neighbouring lane with (much) more room
		for(int8_t l = -1; l <= 1; ++l) {
			if(l - lane > 1 || lane - l > 1) continue;
			if(pRoom[l + 1] > pRoom[target + 1] + 300) target = l;
		}
	}
	pState->autoLane = target;
	int16_t err = s_pLaneX[target + 1] - x + (int16_t)mul16(c, 12);   // lean against the bend's push
	tInput in = {.steer = err > 40 ? 1 : err < -40 ? -1 : 0, .accel = 1, .brake = 0};
	if(x > 220) in.steer = -1;
	else if(x < -220) in.steer = 1;            // never near the objects
	if(pRoom[lane + 1] < 220 && pState->speed > 8 << SPEED_SHIFT) {
		in.accel = 0;                           // boxed in: brake behind it
		in.brake = 1;
	}
	// in the sharpest bends steering can't beat the push at full speed: lift
	// off when pushed out, brake when far out
	int16_t out = c > 0 ? -x : c < 0 ? x : 0;
	if(out > 180 && pState->speed > 13 << SPEED_SHIFT) in.accel = 0;
	if(out > 230 && pState->speed > 10 << SPEED_SHIFT) {
		in.accel = 0;
		in.brake = 1;
	}
	*pOut = in;
}

uint8_t logicTimeSeconds(const tGameState *pState) {
	return (uint8_t)div16u(pState->time + FPS - 1, FPS);
}

uint8_t logicUpdate(tGameState *pState, const tInput *pInputIn) {
	++pState->frame;
	++pState->phaseFrames;
	pState->extended = 0;
	if(pState->messageFrames && !--pState->messageFrames) pState->message = MSG_NONE;
	// What the phase lets the player do
	static const tInput sNone = {0};
	const tInput *pInput = pInputIn;
	if(pState->phase == PHASE_TITLE) {
		uint8_t isStart = pInputIn->accel && pState->phaseFrames > TITLE_WAIT;
		if(isStart || pState->phaseFrames > DEMO_WAIT) {
			uint16_t frame = pState->frame;
			uint32_t score = pState->score;
			logicInit(pState);   // (the score starts again)
			pState->frame = frame;
			if(!isStart) {
				pState->isDemo = 1;
				pState->score = score;   // (the title keeps showing the last race's)
			}
			return 1;
		}
		pInput = &sNone;
	}
	else if(pState->isDemo) {
		// fire, a crash, time up or enough: back to the title
		if(pInputIn->accel || pState->crash || pState->phase == PHASE_OVER || pState->phaseFrames >= DEMO_FRAMES) {
			uint16_t frame = pState->frame;
			logicTitle(pState);
			pState->frame = frame;
			return 1;
		}
		if(pState->frame & 1) logicAutopilot(pState, &pState->autoInput);
		pInput = &pState->autoInput;
	}
	else if(pState->phase == PHASE_OVER) {
		if(pState->phaseFrames >= OVER_FRAMES) {
			logicTitle(pState);
			return 1;
		}
		pInput = &sNone;    // coast to a stop
	}
	pState->crashed = 0;
	if(pState->crash) {
		// tumbling: no control, sliding to a stop, then back on the road
		pInput = &sNone;
		pState->speed -= pState->speed >> 3;
		if(!--pState->crash) {
			pState->speed = 0;
			pState->x = 0;
			pState->offroad = 0;
		}
	}
	int16_t oldSpeed = pState->speed, oldX = pState->x;
	uint32_t oldPos = pState->pos;

	int16_t s = pState->speed;
	if(pInput->brake) {
		s -= BRAKE;
	}
	else if(pInput->accel) {
		s += s < SPEED_MAX / 2 ? ACCEL : ACCEL / 2;   // pulls harder at low speed
	}
	else {
		s -= DRAG;
	}
	if(pState->offroad && s > SPEED_OFFROAD) {
		s -= OFFROAD_DRAG;
	}
	if(s < 0) s = 0;
	if(s > SPEED_MAX) s = SPEED_MAX;
	pState->speed = s;

	// Steering works in proportion to speed; curves push you outwards.
	int8_t curve = logicCurveAt(pState->pos);
	int16_t x = pState->x;
	if(!pState->crash) {
		x += (int16_t)(mul16(pInput->steer * STEER, s) >> 12);
		x -= (int16_t)(mul16(curve * CENTRIFUGAL, s) >> 14);
	}
	if(x > X_MAX) x = X_MAX;
	if(x < -X_MAX) x = -X_MAX;
	pState->x = x;
	pState->offroad = x > X_ROAD_EDGE || x < -X_ROAD_EDGE;

	// Move along the track (s is 1/256 units per frame)
	uint16_t step = (uint16_t)pState->posFrac + (uint16_t)s;
	pState->pos += step >> SPEED_SHIFT;
	pState->posFrac = step & 0xFF;
	if(pState->pos >= TRACK_LEN) {
		pState->pos -= TRACK_LEN;
		++pState->laps;
	}
	// The scenery on the horizon drifts against the curve
	pState->bgX -= (uint16_t)(mul16(curve, s) >> 10);   // 1/16 px

	// Into a roadside object? They stand at the start of their segment: see
	// if the car's wheels crossed one this frame, on the side it's on
	if(!pState->crash && (x > OBJ_SIDE_X - OBJ_HIT_X || x < -(OBJ_SIDE_X - OBJ_HIT_X))) {
		uint32_t carNew = wrapPos(pState->pos + PLAYER_Z), carOld = wrapPos(oldPos + PLAYER_Z);
		if((carNew >> SEG_SHIFT) != (carOld >> SEG_SHIFT)) {
			uint8_t b = s_pScenery[carNew >> SEG_SHIFT];
			if((x > 0 && SCENERY_RIGHT(b) >= 0) || (x < 0 && SCENERY_LEFT(b) >= 0)) {
				if(s > CRASH_SPEED) {
					pState->crash = CRASH_FRAMES;
					pState->crashed = 1;
					++pState->crashes;
				}
				else {
					// too slow to crash: it stops against it, nudged clear to
					// the road side (steering needs speed: it would be stuck)
					uint32_t stop = (carNew & ~(uint32_t)(SEG_LEN - 1)) + TRACK_LEN - PLAYER_Z - 1;
					pState->pos = wrapPos(stop);
					pState->posFrac = 0;
					pState->speed = 0;
					pState->x = x > 0 ? OBJ_SIDE_X - OBJ_HIT_X - 1 : -(OBJ_SIDE_X - OBJ_HIT_X - 1);
				}
			}
		}
	}

	uint32_t prePos = pState->pos;
	pState->bumped = 0;
	if(pState->frame & 1) trafficUpdate(pState);
	if(pState->pos > prePos + TRACK_LEN / 2) --pState->laps;   // bounced back over the start line

	if(pState->phase == PHASE_RACE) {
		if(!pState->isDemo) pState->score += (uint16_t)pState->speed >> SPEED_SHIFT;   // (the title keeps the last race's)
		if(trackDelta(pState->pos, pState->nextCheckpoint) >= 0) {
			pState->nextCheckpoint = wrapPos(pState->nextCheckpoint + CHECKPOINT_LEN);
			pState->time = pState->time + EXTEND_TIME > TIME_MAX ? TIME_MAX : pState->time + EXTEND_TIME;
			pState->message = MSG_EXTEND;
			pState->messageFrames = MESSAGE_FRAMES;
			pState->extended = 1;
		}
		if(!--pState->time) {
			pState->phase = PHASE_OVER;
			pState->phaseFrames = 0;
			pState->message = MSG_TIMEUP;
			pState->messageFrames = OVER_FRAMES;
		}
	}

	return pState->speed != oldSpeed || pState->x != oldX || (pState->pos >> STRIPE_SHIFT) != (oldPos >> STRIPE_SHIFT) ||
		pState->bumped || pState->crash;
}

uint16_t logicKmh(const tGameState *pState) {
	// (every picture for the HUD: one MULU and one DIVU, no library calls)
	uint32_t v = (uint32_t)(uint16_t)pState->speed * (uint16_t)KMH_MAX;
	return div16u(v, SPEED_MAX);
}

// -------------------------------------------------------------- the road ---

// Segments in view: the farthest row is ROAD_ZSCALE units ahead
#define SEGS_AHEAD ((ROAD_ZSCALE >> SEG_SHIFT) + 2)

#define FAR_ROWS 8
#define FAR_LINES_MAX 4
uint8_t logicRoadRuns(const tGameState *pState, tRoadRun *pRuns, tRoadView *pView) {
	// This runs every picture for 80 rows, so a row costs only additions:
	//  - within a segment the road is a straight line in 3D (height linear in
	//    z), and z = ROAD_ZSCALE / (r + 1), so the screen line of row r is
	//    linear in r: y = HORIZON_Y + A (r + 1) + B. A and B are worked out once
	//    per segment (two MULS), then each row just steps y by A.
	//  - curves and the player's side offset are accumulators too.
	// Fixed point 16.16 throughout: the integer part is a SWAP away.
	tablesInit();
	static int16_t pRelH[SEGS_AHEAD + 2];
	static int8_t pCurve[SEGS_AHEAD + 1];
	uint16_t camSeg = pState->pos >> SEG_SHIFT;
	uint16_t camFrac = pState->pos & (SEG_LEN - 1);
	int16_t camH = logicHeightAt(pState->pos);
	uint16_t seg = camSeg;
	for(uint8_t k = 0; k <= SEGS_AHEAD + 1; ++k) {
		pRelH[k] = s_pHeight[seg] - camH;
		if(k <= SEGS_AHEAD) pCurve[k] = s_pCurve[seg];
		if(++seg >= TRACK_SEGS) seg = 0;
	}
	// Where the road's centre is on screen, per row, in 16.16: the curve shift
	// minus the player's offset (x * half(r) / 256 px, half(r) = 1.25 (r + 1)).
	// Both are accumulators, so one: cxFx += dxFx each row, and dxFx grows by
	// the segment's curve (dxAdd) each row.
	int32_t pxStep = mul16(pState->x, (int16_t)(ROW_STEP * 5)) * 64;
	// (one step back: the loop adds a step before using it)
	int32_t cxFx = mul16(-pState->x, (int16_t)(ROAD_ROWS * 5)) * 64 - pxStep;   // row ROAD_ROWS-1: -x * 1.25 * 160 / 256
	int32_t dxFx = pxStep, dxAdd = 0;
	int16_t minY = SCREEN_H;          // lines minY.. are filled
	int32_t yFx = 0, yStep = 0;       // screen line of the current row, 16.16
	int16_t kCur = -1;
	tRoadRun *pRun = pRuns;
	int16_t *pViewY = &pView->y[PROJ_ROWS];   // (and cx: PROJ_ROWS further on)
	for(int16_t r = ROAD_ROWS - 1; r >= 0; r -= ROW_STEP) {
		uint16_t off = camFrac + s_pZ[r];               // units ahead of the camera's segment
		int16_t k = off >> SEG_SHIFT;
		if(k != kCur) {
			// New segment: height h0 at its start (z0 units ahead), slope s per unit.
			// dh(z) (r + 1) = (h0 - s z0)(r + 1) + s ROAD_ZSCALE, so
			// y = HORIZON_Y + (r + 1) (1 - (h0 - s z0) / CAM_HEIGHT) - s ROAD_ZSCALE / CAM_HEIGHT
			kCur = k;
			int16_t h0 = pRelH[k], d = pRelH[k + 1] - h0;          // d = s * SEG_LEN
			int16_t z0 = (int16_t)(k << SEG_SHIFT) - (int16_t)camFrac;
			// (h0 - s z0) in 1/16 units, then A = 1 - that / CAM_HEIGHT in 1/1024
			// (a 16-bit value: one MULS per row count below, no 32-bit multiply)
			int32_t c = (int32_t)h0 * 16 - (mul16(d, z0) >> (SEG_SHIFT - 4));
			int16_t aFx = (int16_t)(1024 - (c >> (4 + CAM_SHIFT - 10)));
			// s * ZSCALE / CAM_H: one MULS by a folded 16-bit constant (as
			// "* 40 * 2, negated" GCC called the 32-bit ___mulsi3 by -2)
			int32_t bFx = mul16(d, -(ROAD_ZSCALE >> SEG_SHIFT) * (1 << (10 - CAM_SHIFT)));
			yStep = (int32_t)aFx * (ROW_STEP << 6);                // 1/1024 -> 16.16
			yFx = ((int32_t)HORIZON_Y * 1024 + mul16(aFx, r + 1) + bFx) << 6;
			dxAdd = (int32_t)pCurve[k] * (256L * ROW_STEP * ROW_STEP);
		}
		dxFx += dxAdd;
		cxFx += dxFx;
		int16_t y = (int16_t)(yFx >> 16);
		yFx -= yStep;
		int16_t cx = (int16_t)(cxFx >> 16);
		--pViewY;
		pViewY[PROJ_ROWS] = SCREEN_W / 2 + cx;
		if(y >= minY || y < REGION_TOP) {
			*pViewY = -1;
			if(y >= minY) continue;   // hidden behind a nearer crest (or below the screen)
			y = REGION_TOP;
		}
		else {
			*pViewY = y;
		}
		// The farthest rows are 10-20 segments away and a few px wide: seen
		// from high above (going down a long hill) one would stretch over many
		// lines into a thin column. They keep a few; the horizon strip covers
		// the rest.
		if(r < FAR_ROWS && minY - y > FAR_LINES_MAX) y = minY - FAR_LINES_MAX;
		int16_t left = ROAD_CX - SCREEN_W / 2 - cx;
		if(left < LEFT_MIN) left = LEFT_MIN;
		else if(left > LEFT_MAX) left = LEFT_MAX;
		// the segment starts on a multiple of 256 units: its stripes are off's
		*pRun++ = (tRoadRun){.y = y, .dark = (off >> STRIPE_SHIFT) & 1, .row = (uint8_t)r, .step = 0, .left = left};
		minY = y;
		if(y == REGION_TOP) {
			while(pViewY != pView->y) *--pViewY = -1;   // the rest is hidden
			break;
		}
	}
	// The horizon strip on top of the road (as much as fits below REGION_TOP), then sky
	int16_t wBackTop = minY - BACK_H;
	if(wBackTop < REGION_TOP) wBackTop = REGION_TOP;
	if(wBackTop < minY) {
		int16_t wBackLeft = LEFT_MIN + (int16_t)((pState->bgX >> 4) & (BACK_PERIOD - 1));
		*pRun++ = (tRoadRun){
			.y = wBackTop, .dark = 0, .row = (uint8_t)(ROW_BACK + BACK_H - (minY - wBackTop)),
			.step = 1, .left = wBackLeft
		};
		minY = wBackTop;
	}
	if(minY > REGION_TOP) {
		*pRun++ = (tRoadRun){.y = REGION_TOP, .dark = 0, .row = ROW_SKY, .step = 0, .left = LEFT_MIN};
	}
	return (uint8_t)(pRun - pRuns);
}

void logicRoadLines(const tGameState *pState, tRoadLine *pOut, tRoadView *pView) {
	static tRoadRun pRuns[RUN_MAX];
	uint8_t n = logicRoadRuns(pState, pRuns, pView);
	int16_t bottom = SCREEN_H - 1;
	for(uint8_t i = 0; i < n; ++i) {
		for(int16_t y = bottom; y >= pRuns[i].y; --y) {
			uint8_t row = pRuns[i].row + (pRuns[i].step ? y - pRuns[i].y : 0);
			pOut[y - REGION_TOP] = (tRoadLine){.dark = pRuns[i].dark, .row = row, .left = pRuns[i].left};
		}
		bottom = pRuns[i].y - 1;
	}
}

// ---------------------------------------------------------------- objects

// Where an object at depth z (units from the camera) shows: projected row
// index, or -1 if it's behind the nearest row, beyond the horizon or hidden
// behind a crest
static inline int8_t objectRow(uint16_t z, const tRoadView *pView) {
	if(z < (ROAD_ZSCALE / ROAD_ROWS) || z >= ROAD_ZSCALE) return -1;
	uint16_t r1 = div16u(ROAD_ZSCALE, z);                     // r + 1: z = ZSCALE / (r + 1)
	_Static_assert(ROW_STEP == 2, "the shift below divides by ROW_STEP");
	uint8_t i = (uint8_t)((uint16_t)(r1 - 1) >> 1);          // / ROW_STEP
	if(i >= PROJ_ROWS) i = PROJ_ROWS - 1;
	return pView->y[i] < 0 ? -1 : (int8_t)i;
}

uint8_t logicObjects(const tGameState *pState, const tRoadView *pView, tObject *pOut) {
	// Everything in view, nearest first: palms segment by segment from the
	// camera outwards, the rivals slotted in by depth. Then the nearest
	// OBJ_MAX come out farthest first.
	static tObject pAll[OBJ_PALMS_MAX + TRAFFIC_N];
	static uint16_t pZ[OBJ_PALMS_MAX + TRAFFIC_N];
	uint8_t n = 0;
	uint16_t camSeg = pState->pos >> SEG_SHIFT;
	uint16_t camFrac = pState->pos & (SEG_LEN - 1);
	for(uint8_t k = 1; k <= (ROAD_ZSCALE >> SEG_SHIFT) && n < OBJ_PALMS_MAX - 1; ++k) {
		uint16_t seg = camSeg + k;
		if(seg >= TRACK_SEGS) seg -= TRACK_SEGS;
		uint8_t flags = s_pScenery[seg];
		if(!flags) continue;
		uint16_t z = (uint16_t)(k << SEG_SHIFT) - camFrac;
		int8_t i = objectRow(z, pView);
		if(i < 0) continue;
		int16_t y = pView->y[i], side = s_pRowSide[i];
		uint16_t scale = s_pRowScale[i];
		int8_t t = SCENERY_LEFT(flags);
		if(t >= 0) {
			pZ[n] = z;
			pAll[n++] = (tObject){.x = pView->cx[i] - side, .y = y, .scale = scale, .type = (uint8_t)t};
		}
		t = SCENERY_RIGHT(flags);
		if(t >= 0) {
			pZ[n] = z;
			pAll[n++] = (tObject){.x = pView->cx[i] + side, .y = y, .scale = scale, .type = (uint8_t)t};
		}
	}
	for(uint8_t c = 0; c < TRAFFIC_N; ++c) {
		const tRival *pR = &pState->rivals[c];
		uint32_t d = pR->pos - pState->pos;
		if(d >= TRACK_LEN) d += TRACK_LEN;                       // (it's behind the start line)
		if(d >= ROAD_ZSCALE) continue;
		uint16_t z = (uint16_t)d;
		int8_t i = objectRow(z, pView);
		if(i < 0) continue;
		int16_t half = (int16_t)s_pHalf[ROW_STEP * i + 1];
		tObject o = {
			.x = pView->cx[i] + (int16_t)(mul16(s_pLaneX[pR->lane + 1], half) >> 8),
			.y = pView->y[i], .scale = s_pRowScale[i], .type = OBJ_RIVAL
		};
		uint8_t j = n++;
		for(; j && pZ[j - 1] > z; --j) {
			pZ[j] = pZ[j - 1];
			pAll[j] = pAll[j - 1];
		}
		pZ[j] = z;
		pAll[j] = o;
	}
	if(n > OBJ_MAX) n = OBJ_MAX;
	for(uint8_t k = 0; k < n; ++k) {
		pOut[k] = pAll[n - 1 - k];
	}
	return n;
}

// ------------------------------------------------------------------- sky

// Per-line gradient: interpolate in quarter levels and dither the fraction
// across lines (ordered 0,2,1,3), so 16 levels per channel don't show stripes.
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

uint16_t logicSkyColor(uint16_t y) {
	// Deep blue at the top to a pale haze at the horizon (and below it,
	// where a crest hides the road).
	if(y >= 120) return 0xBDF;
	return lerpColorDither(0x15B, 0xBDF, (int16_t)y, 120, (uint8_t)y);
}
