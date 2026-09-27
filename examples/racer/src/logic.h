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
#define CENTRIFUGAL 5          // how hard curves push you out: curve * 5 / 16384 of the speed;
                               // full steering just holds the sharpest (4) at full speed
#define KMH_MAX 290            // speed shown for SPEED_MAX

typedef struct {
	int8_t steer;      // -1 left, 0, +1 right
	uint8_t accel;     // gas (up / fire)
	uint8_t brake;     // down
} tInput;

// ------------------------------------------------------------- traffic ---
// Rival cars drive in three lanes, one speed per lane (so a lane never piles
// up); you overtake them. The player's car stands PLAYER_Z units ahead of the
// camera (where the car sprite's wheels meet the road); cars closer than
// CAR_LEN along the track and CAR_HIT_X across touch, and the one behind is
// pushed back and slowed to the front one's speed.
#define TRAFFIC_N 32            // ~3 in view: you pass one every few seconds
#define LANE_X 170             // lane centres: -LANE_X, 0, +LANE_X
#define PLAYER_Z 68             // z of the car sprite's rear wheels (line ~244)
#define CAR_LEN 16              // its nose is at z ~84 (line ~222): perspective squeezes the near rows
#define CAR_HIT_X 72
#define BUMP_SLOW (3 << SPEED_SHIFT)   // the one behind drops to the front's speed minus this

// Roadside objects stand OBJ_SIDE_X from the centre (x units, like the car's
// x): driving into one faster than CRASH_SPEED is a crash - the car tumbles
// for CRASH_FRAMES and is put back on the road, standing. Slower, it stops.
#define OBJ_HIT_X 40           // car's half width + a trunk's, x units
#define CRASH_SPEED (6 << SPEED_SHIFT)
#define CRASH_FRAMES (2 * FPS)

typedef struct {
	uint32_t pos;      // along the track, units
	uint8_t posFrac;
	int8_t lane;       // -1, 0, +1
	int16_t speed;     // 1/256 units per frame
} tRival;

// ----------------------------------------------------------- game flow ---
// Title (attract: the road, rivals driving by, "press fire") -> race against
// the clock: a checkpoint every CHECKPOINT_SEGS segments adds time -> time up:
// coast to a stop -> title. Frames are logic frames (50 per second).
#define PHASE_TITLE 0
#define PHASE_RACE 1
#define PHASE_OVER 2
#define FPS 50
#define START_TIME (40 * FPS)
#define CHECKPOINT_SEGS 100    // 4 per lap (the start line is one)
#define CHECKPOINT_LEN ((uint32_t)CHECKPOINT_SEGS << SEG_SHIFT)
#define EXTEND_TIME (25 * FPS)
#define TIME_MAX (99 * FPS)
#define OVER_FRAMES (4 * FPS)  // time up -> title
#define TITLE_WAIT FPS         // the title ignores fire this long (still held from the race)
#define DEMO_WAIT (10 * FPS)   // on the title this long: a demo drive (the autopilot)
#define DEMO_FRAMES (60 * FPS) // (then back to the title; fire, a crash or time up end it sooner)
#define MESSAGE_FRAMES (2 * FPS)
#define MSG_NONE 0
#define MSG_EXTEND 1           // "EXTEND TIME!"
#define MSG_TIMEUP 2           // "TIME UP"

typedef struct {
	uint32_t pos;      // distance along the track, units (wraps at the track length)
	uint8_t posFrac;   // 1/256 units
	int16_t speed;     // 1/256 units per frame
	int16_t x;         // lateral position, 0 = centre, +-X_ROAD_EDGE = edges
	uint16_t bgX;      // horizon scenery scroll, 1/16 px (drifts in curves; wraps)
	uint16_t frame;
	uint8_t offroad;   // wheels on the grass
	uint16_t laps;
	uint16_t bumps;    // collisions with rivals so far
	uint8_t bumped;    // a collision this frame (for the sound)
	tRival rivals[TRAFFIC_N];
	uint8_t phase;     // PHASE_*
	uint16_t phaseFrames;   // frames since the phase began
	uint16_t time;     // frames left in the race
	uint32_t score;    // units driven (kept on the title after a race)
	uint32_t nextCheckpoint;   // track position of the next one
	uint8_t message;   // MSG_*, shown for messageFrames more frames
	uint16_t messageFrames;
	uint8_t extended;  // a checkpoint this frame (for the sound)
	uint8_t isDemo;    // a race driven by logicAutopilot() (attract mode)
	int8_t autoLane;   // the autopilot's lane
	tInput autoInput;  // its last decision (it decides every other frame)
	uint8_t crash;     // frames of crash left (0: driving)
	uint8_t crashed;   // a crash this frame (for the sound)
	uint16_t crashes;
} tGameState;

typedef struct {
	uint8_t dark;      // stripe phase: 0 light, 1 dark
	uint8_t row;       // road bitmap row, or ROW_SKY
	int16_t left;      // bitmap column shown at screen x 0 (LEFT_MIN..LEFT_MAX)
} tRoadLine;

// The same as runs of lines (what main.c uses): each run shows one row on
// lines y .. (the previous run's y) - 1, runs ordered from the bottom up.
typedef struct {
	int16_t y;         // the run's top line
	uint8_t dark;
	uint8_t row;       // the row on its top line
	uint8_t step;      // 0: that row on every line; 1: the next row on each line
	                   // down (the horizon strip: one run instead of a run per line)
	int16_t left;
} tRoadRun;
#define RUN_MAX (ROAD_ROWS / 2 + 2)   // the projected rows, the horizon strip, the sky

// Where the projected road rows ended up this picture (for placing objects)
#define PROJ_ROWS (ROAD_ROWS / ROW_STEP)   // projected row i is road row r = ROW_STEP * i + 1
typedef struct {
	int16_t y[PROJ_ROWS];    // screen line, or -1 if hidden behind a crest
	int16_t cx[PROJ_ROWS];   // screen x of the road's centre
} tRoadView;

// What's drawn on the road: roadside palms (placed per track segment) and
// the rivals, farthest first
#define OBJ_MAX 14            // the nearest ones: every blit has a fixed cost, far ones are tiny
#define OBJ_PALMS_MAX 10      // of which palms
#define OBJ_SIDE_X 360        // lateral position, 1/256 road half widths from the centre
#define SCENERY_PALM_L 1      // (the track parts table: palm rows on the left/right)
#define SCENERY_PALM_R 2
// Object types (tObject.type); the roadside ones are also the scenery
#define OBJ_PALM 0
#define OBJ_RIVAL 1
#define OBJ_BUSH 2
#define OBJ_SIGN_L 3            // curve warning: chevrons pointing left
#define OBJ_SIGN_R 4
#define OBJ_GATE 5              // checkpoint pillar
#define OBJ_TYPES 6
// A segment's scenery byte: left object type + 1 | (right type + 1) << 4, 0 = none
#define SCENERY_LEFT(b) (((b) & 15) - 1)
#define SCENERY_RIGHT(b) (((b) >> 4) - 1)
typedef struct {
	int16_t x;         // screen x of the object's centre
	int16_t y;         // screen line of its foot
	uint16_t scale;    // size, 1/256 of the nearest row's (256 = full size)
	uint8_t type;      // OBJ_*
} tObject;

/** A new race, from the start line with the clock running. */
void logicInit(tGameState *pState);
/** The title (attract mode): a race's start, standing, rivals driving by;
 *  keeps the last race's score. Fire (tInput.accel) starts a race. */
void logicTitle(tGameState *pState);
/** The demo's driver: keeps a lane until a rival is close, then changes to
 *  the one with the most room; steers against the bends' push, lifts off when
 *  pushed wide. Also used by the host tool that writes scenarios. */
void logicAutopilot(tGameState *pState, tInput *pOut);
/** Seconds left on the clock, rounded up (what the HUD shows). */
uint8_t logicTimeSeconds(const tGameState *pState);
/** One frame of driving. Returns 1 if anything test-visible changed. */
uint8_t logicUpdate(tGameState *pState, const tInput *pInput);
/** Road layout as runs of lines (see tRoadRun), and where each projected row
 *  landed. Returns the number of runs; together they cover lines REGION_TOP..255. */
uint8_t logicRoadRuns(const tGameState *pState, tRoadRun *pRuns, tRoadView *pView);
/** The same, one entry per line REGION_TOP..SCREEN_H-1 (pOut[0] is line REGION_TOP). */
void logicRoadLines(const tGameState *pState, tRoadLine *pOut, tRoadView *pView);
/** The roadside objects and rivals in view, farthest first (draw them in this order).
 *  Needs this picture's tRoadView. Returns how many. */
uint8_t logicObjects(const tGameState *pState, const tRoadView *pView, tObject *pOut);
/** Scenery of the segment at pos: see SCENERY_LEFT/RIGHT (-1: nothing). */
uint8_t logicSceneryAt(uint32_t pos);

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
