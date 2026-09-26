#include "logic.h"

// 16 x 16 -> 32 bit signed multiply. On the 68000 that's one MULS.W, but GCC
// doesn't always see that two int16 operands fit it and calls the (slow)
// 32-bit ___mulsi3 instead: force it in per-frame code.
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
} s_pParts[] = {
	{ 45,  0,    0},  // start straight (the whole view ahead is flat)
	{ 30,  0, 8000},  // climb
	{ 35,  2,    0},  // right sweeper over the top
	{ 20,  0, -8000}, // down
	{ 40, -3,    0},  // long left
	{ 10,  0,    0},
	{ 30,  4, 5000},  // right, climbing
	{ 30, -4, -5000}, // left, falling: an S
	{ 15,  0, 4000},  // rolling hills
	{ 15,  0, -4000},
	{ 15,  0, 4000},
	{ 15,  0, -4000},
	{ 35,  1,    0},  // gentle right
	{ 25, -2, -3000}, // left, down to the coast
	{ 20,  0, 3000},
	{ 20,  3,    0},  // right, back to the start line
};
#define PART_COUNT (sizeof(s_pParts) / sizeof(s_pParts[0]))

static int8_t s_pCurve[TRACK_SEGS];
static int16_t s_pHeight[TRACK_SEGS];

// Per-row tables
static uint16_t s_pZ[ROAD_ROWS];      // depth of row r, units
static uint16_t s_pHalf[ROAD_ROWS];   // road half width, px
static uint8_t s_isInit;

static void tablesInit(void) {
	if(s_isInit) return;
	s_isInit = 1;
	for(uint16_t r = 0; r < ROAD_ROWS; ++r) {
		s_pZ[r] = ROAD_ZSCALE / (r + 1);
		s_pHalf[r] = (uint16_t)((uint32_t)ROAD_HALF_MAX * (r + 1) / ROAD_ROWS);
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
			s_pHeight[seg] = (int16_t)(h + ((s_pParts[p].hill * ease) >> 8));
		}
		h += s_pParts[p].hill;
	}
	while(seg < TRACK_SEGS) { // (if the parts are shorter than the track)
		s_pCurve[seg] = 0;
		s_pHeight[seg++] = h;
	}
}

static inline uint32_t wrapPos(uint32_t pos) {
	return pos >= TRACK_LEN ? pos - TRACK_LEN : pos;
}

int8_t logicCurveAt(uint32_t pos) {
	tablesInit();
	return s_pCurve[wrapPos(pos) >> SEG_SHIFT];
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
}

uint8_t logicUpdate(tGameState *pState, const tInput *pInput) {
	++pState->frame;
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
	x += (int16_t)(((int32_t)pInput->steer * STEER * s) >> 12);
	x -= (int16_t)(((int32_t)curve * CENTRIFUGAL * s) >> 13);
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
	pState->bgX -= (uint16_t)(((int32_t)curve * s) >> 10);   // 1/16 px

	return pState->speed != oldSpeed || pState->x != oldX || (pState->pos >> STRIPE_SHIFT) != (oldPos >> STRIPE_SHIFT);
}

uint16_t logicKmh(const tGameState *pState) {
	return (uint16_t)(((uint32_t)pState->speed * KMH_MAX) / SPEED_MAX);
}

// -------------------------------------------------------------- the road ---

// Segments in view: the farthest row is ROAD_ZSCALE units ahead
#define SEGS_AHEAD ((ROAD_ZSCALE >> SEG_SHIFT) + 2)

void logicRoadLines(const tGameState *pState, tRoadLine *pOut) {
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
	// Player offset: x * half(r) / 256 px with half(r) = 1.25 (r + 1), in 16.16
	int32_t pxAcc = mul16(pState->x, (int16_t)(ROAD_ROWS * 5)) * 64;      // x * 1.25 * 160 * 65536 / 256
	int32_t pxStep = mul16(pState->x, (int16_t)(ROW_STEP * 5)) * 64;
	uint16_t stripeBase = (uint16_t)(pState->pos & 0xFFFF);

	int16_t minY = SCREEN_H;          // lines minY.. are filled
	int32_t curveX = 0, curveDx = 0;  // road centre shift, 16.16 px
	int32_t yFx = 0;                  // screen line of the current row, 1/1024 lines
	int16_t yStep = 0;
	int16_t kCur = -1;
	tRoadLine *pFill = &pOut[SCREEN_H - REGION_TOP];   // one past the last line
	for(int16_t r = ROAD_ROWS - 1; r >= 0; r -= ROW_STEP, pxAcc -= pxStep) {
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
			yStep = aFx * ROW_STEP;
			yFx = (int32_t)HORIZON_Y * 1024 + mul16(aFx, r + 1) + bFx;
		}
		curveDx += (int32_t)pCurve[k] * (256 * ROW_STEP * ROW_STEP);
		curveX += curveDx;
		int16_t y = (int16_t)(yFx >> 10);
		yFx -= yStep;
		if(y >= minY) {
			continue;   // hidden behind a nearer crest (or below the screen)
		}
		if(y < REGION_TOP) y = REGION_TOP;
		// Road centre on screen: 160 + curve shift - where the player is on the road
		int16_t cx = (int16_t)((curveX - pxAcc) >> 16);
		int16_t left = ROAD_CX - SCREEN_W / 2 - cx;
		if(left < LEFT_MIN) left = LEFT_MIN;
		if(left > LEFT_MAX) left = LEFT_MAX;
		tRoadLine sLine = {
			.dark = ((uint16_t)(stripeBase + s_pZ[r]) >> STRIPE_SHIFT) & 1,
			.row = (uint8_t)r,
			.left = left
		};
		// Copy until the pointer reaches its target. (Written with a count or
		// as pFill > pTop, GCC computed the pointer's end value as count * -4
		// with a 32-bit ___mulsi3 call per row.)
		tRoadLine *pTop = &pOut[y - REGION_TOP];
		while(pFill != pTop) {
			*--pFill = sLine;
		}
		pFill = pTop;   // (already true: stops GCC computing it with a multiply)
		minY = y;
		if(y == REGION_TOP) break;
	}
	// The horizon strip on top of the road, then sky
	tRoadLine sBack = {.dark = 0, .row = ROW_BACK + BACK_H - 1,
		.left = LEFT_MIN + (int16_t)((pState->bgX >> 4) & (BACK_PERIOD - 1))};
	for(int16_t n = minY - REGION_TOP; n > 0 && sBack.row >= ROW_BACK; --n, --sBack.row) {
		*--pFill = sBack;
	}
	tRoadLine sSky = {.dark = 0, .row = ROW_SKY, .left = LEFT_MIN};
	while(pFill != pOut) {
		*--pFill = sSky;
	}
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
