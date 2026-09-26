// Game rules for racer: pure C, no Amiga headers, so `agk unit` can test them
// on the host. main.c turns the results into copper lists and sprites.
//
// The road is a classic "raster road": a bitmap with one row per distance
// (row r = depth z_r, a flat road shows it r+1 lines below the horizon).
// Every frame logicRoadLines() decides, for each screen line, which row it
// shows (hills), where (curves, steering) and in which stripe colours
// (movement). The copper does the rest; the CPU draws nothing.
#ifndef _LOGIC_H_
#define _LOGIC_H_

#include <stdint.h>

#define SCREEN_W 320
#define SCREEN_H 256

// ------------------------------------------------------------- the road ---
#define ROAD_ROWS 160         // bitmap rows (depths); row 0 is the farthest
#define ROW_STEP 2            // logicRoadLines() projects every 2nd row (68000 budget);
                              // a projected row also fills the line(s) above it
#define ROAD_BMP_W 1024       // wide enough to shift the road for curves and the grass
#define ROAD_CX 512           // road centre column in every row
#define ROAD_HALF_MAX 200     // half the road's width in the nearest row, px
#define HORIZON_Y 95          // flat road: row r shows on line HORIZON_Y + 1 + r
#define REGION_TOP 40         // lines from here to the bottom get a per-line copper block
#define REGION_LINES (SCREEN_H - REGION_TOP)
#define ROW_SKY 0xFF          // tRoadLine.row for a line that shows no road
// The horizon strip (art/backdrop.txt) sits on the road's top line, BACK_H
// lines tall: tRoadLine.row = ROW_BACK + its row. It loops every BACK_PERIOD px
// and drifts sideways in curves (tGameState.bgX).
#define BACK_H 32
#define BACK_PERIOD 512
#define ROW_BACK 200
#define LEFT_MIN 16           // scroll range in the road bitmap (one fetch word
#define LEFT_MAX (ROAD_BMP_W - SCREEN_W - 16)   // of pre-roll on each side)

// World units: the track is made of segments of 256 units; row r looks
// z_r = ROAD_ZSCALE / (r + 1) units ahead (64 at the bottom, 10240 at the
// horizon: 40 segments). The camera is CAM_HEIGHT units above the road, so a point h units
// above the camera's road height at row r's depth shows h * (r + 1) / CAM_HEIGHT
// lines higher: a slope of 1 lifts the horizon ROAD_ZSCALE / CAM_HEIGHT = 20 lines.
#define SEG_SHIFT 8
#define SEG_LEN (1 << SEG_SHIFT)
#define ROAD_ZSCALE 10240
#define CAM_HEIGHT 512
#define CAM_SHIFT 9            // log2(CAM_HEIGHT)
#define STRIPE_SHIFT 5        // a light or dark stripe every 32 units

// Road colours (12-bit), light and dark stripe: grass (COLOR00), rumble
// strip, asphalt, lane marks (the marks vanish in the dark stripes: dashes).
#define GRASS_LIGHT 0x4A2
#define GRASS_DARK 0x391
#define RUMBLE_LIGHT 0xEEE
#define RUMBLE_DARK 0xD22
#define ROAD_LIGHT 0x889
#define ROAD_DARK 0x778
#define LANE_LIGHT 0xEEF

// Road bitmap colours (playfield 2: 0 = transparent = COLOR00 = grass)
#define ROADPIX_GRASS 0
#define ROADPIX_RUMBLE 1
#define ROADPIX_ASPHALT 2
#define ROADPIX_LANE 3

// ------------------------------------------------------------- driving ---
#define SPEED_SHIFT 8          // speed is in 1/256 units per frame
#define SPEED_MAX (20 << SPEED_SHIFT)      // 20 units/frame
#define SPEED_OFFROAD (7 << SPEED_SHIFT)   // top speed on the grass
#define ACCEL 36               // per frame at low speed (less near the top)
#define BRAKE 80
#define DRAG 10
#define OFFROAD_DRAG 60
#define X_ROAD_EDGE 256        // player x: 0 = road centre, +-256 = road edges
#define X_MAX 400              // how far onto the grass you can go (the near rows
                               // shift by X_MAX * 200 / 256 = 312 px: inside LEFT_MIN..MAX)
#define STEER 5                // x per frame at full speed
#define CENTRIFUGAL 3          // how hard curves push you out (see logicUpdate)
#define KMH_MAX 290            // speed shown for SPEED_MAX

typedef struct {
	int8_t steer;      // -1 left, 0, +1 right
	uint8_t accel;     // gas (up / fire)
	uint8_t brake;     // down
} tInput;

typedef struct {
	uint32_t pos;      // distance along the track, units (wraps at the track length)
	uint8_t posFrac;   // 1/256 units
	int16_t speed;     // 1/256 units per frame
	int16_t x;         // lateral position, 0 = centre, +-X_ROAD_EDGE = edges
	uint16_t bgX;      // horizon scenery scroll, 1/16 px (drifts in curves; wraps)
	uint16_t frame;
	uint8_t offroad;   // wheels on the grass
	uint16_t laps;
} tGameState;

typedef struct {
	uint8_t dark;      // stripe phase: 0 light, 1 dark
	uint8_t row;       // road bitmap row, or ROW_SKY
	int16_t left;      // bitmap column shown at screen x 0 (LEFT_MIN..LEFT_MAX)
} tRoadLine;

void logicInit(tGameState *pState);
/** One frame of driving. Returns 1 if anything test-visible changed. */
uint8_t logicUpdate(tGameState *pState, const tInput *pInput);
/** Road layout for lines REGION_TOP..SCREEN_H-1 (pOut[0] is line REGION_TOP). */
void logicRoadLines(const tGameState *pState, tRoadLine *pOut);

/** Half the road's width in row r, px. */
uint16_t logicRoadHalf(uint8_t r);
/** What the road bitmap holds at column x of row r (ROADPIX_*). */
uint8_t logicRoadPixel(uint8_t r, int16_t x);
/** The same as thresholds on the distance d from the road centre (d = x -
 *  ROAD_CX, or ROAD_CX - 1 - x on the left): grass if d >= half, rumble if
 *  d >= half - rumble, lane mark if |d - lane| <= laneHalf, else asphalt. */
typedef struct { int16_t half, rumble, lane, laneHalf; } tRoadSpans;
void logicRoadSpans(uint8_t r, tRoadSpans *pSpans);
/** Speed in km/h for the dashboard. */
uint16_t logicKmh(const tGameState *pState);

// Track (a loop): curve per segment (1/256 px of extra shift per row,
// accumulated twice: the road bends more the farther away it is) and height
// (units; the camera follows it).
#define TRACK_SEGS 400
#define TRACK_LEN ((uint32_t)TRACK_SEGS << SEG_SHIFT)
int8_t logicCurveAt(uint32_t pos);
int16_t logicHeightAt(uint32_t pos);

/** Sky colour of screen line y (0..SCREEN_H-1). */
uint16_t logicSkyColor(uint16_t y);

#endif
