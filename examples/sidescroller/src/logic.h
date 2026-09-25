/**
 * Game rules - plain portable C.
 *
 * Nothing in logic.c may include ACE or Amiga headers: it is compiled for the
 * Amiga *and* for the host so `agk unit` can test it in milliseconds.
 * Keep it deterministic and use 16-bit types: the 68000 has no 32-bit
 * multiply/divide instruction, so `int` math on the Amiga silently calls slow
 * library routines.
 */
#ifndef _LOGIC_H_
#define _LOGIC_H_

#include <stdint.h>

// Screen (one PAL lores playfield)
#define SCREEN_W 320
#define SCREEN_H 256

// Level: 80x16 tiles of 16x16 px = 1280x256 px (the front playfield, PF1)
#define TILE_SIZE 16
#define TILE_SHIFT 4
#define LEVEL_TILES_W 80
#define LEVEL_TILES_H 16
#define LEVEL_W (LEVEL_TILES_W * TILE_SIZE)
#define LEVEL_H (LEVEL_TILES_H * TILE_SIZE)
#define CAM_MAX (LEVEL_W - SCREEN_W)

// Tile ids in the level map (tileset frame = id - 1)
#define TILE_EMPTY 0
#define TILE_GRASS 1  // solid
#define TILE_DIRT 2   // solid
#define TILE_STONE 3  // one-way platform: you land on its top, jump through from below

// Player: a 16x16 sprite; collision box is columns 3..12, full height
#define PLAYER_W 16
#define PLAYER_H 16
#define PLAYER_HB_L 3
#define PLAYER_HB_R 12
#define PLAYER_SPEED 2       // px per frame
#define START_X 32
#define START_Y (13 * TILE_SIZE - PLAYER_H)   // standing on the ground (row 13)

// Vertical motion in fixed point: 1/16 px (4 fractional bits), int16_t
#define FIX_SHIFT 4
#define GRAVITY 6            // 0.375 px/frame^2
#define JUMP_VEL 104         // 6.5 px/frame upwards
#define MAX_FALL_VEL 112     // 7 px/frame: < TILE_SIZE, and we step pixel by pixel anyway

// Walk animation: art/player.txt frames 0..3, frame 0 = standing
#define WALK_FRAMES 8
#define WALK_FRAME_TICKS 4

// Parallax bands (back playfield, PF2). Game y of each band's first line.
#define MOUNTAINS_Y 79   // peaks sit low against a tall sky
#define MOUNTAINS_H 56   // art cropped to the peaks (agk art-clean --crop 6:62)
#define GAP_Y (MOUNTAINS_Y + MOUNTAINS_H)     // 135: one line of blank PF2 to reload the palette
#define HILLS_Y (GAP_Y + 1)                   // 136
#define HILLS_H 73
#define BANDS_END (HILLS_Y + HILLS_H)          // 209: below this PF2 is blank
// (the ground's grass blades on line 208 still have hills behind them; from
// line 209 on, what shows through the level is the inside of a pit)
#define MOUNTAINS_SHIFT 2   // camera / 4
#define HILLS_SHIFT 1       // camera / 2

// Sky gradient (COLOR00): one colour every SKY_STEP lines from 0 to HILLS_Y
#define SKY_STEP 1
#define SKY_BANDS (HILLS_Y / SKY_STEP)         // 136
// Behind the hills: mist (starts at the colour the mountains fade into), one colour every HAZE_STEP lines
#define HAZE_STEP 1
#define HAZE_BANDS ((BANDS_END - HILLS_Y) / HAZE_STEP)  // 73
// Inside the pits (COLOR00 below the bands): dark earth fading to black
#define PIT_BANDS (SCREEN_H - BANDS_END)       // 47
#define PIT_RAMP_LINES 8                        // lines per step of the pit's colour ramp

typedef struct {
	int8_t dx;      // -1, 0 or 1
	uint8_t jump;   // fire or up held
} tInput;

// Enemies: 16x16 mushroom critters (art/enemy.txt) that patrol a floor.
// They walk ENEMY_SPEED_FIX/16 px per frame and turn at walls, at the end of
// their floor (pit or platform edge) and at the level ends. They never fall.
#define ENEMY_COUNT 3
#define ENEMY_W 16
#define ENEMY_H 16
#define ENEMY_HB_L 2        // collision box: columns 2..13, rows 3..15
#define ENEMY_HB_R 13
#define ENEMY_HB_T 3
#define ENEMY_SPEED_FIX 8   // 0.5 px/frame (1/16 px units)
#define ENEMY_ANIM_SHIFT 2  // walk pose A/B alternates every 4 px walked
#define SQUASH_TICKS 25     // squashed frame shows 0.5 s, then the enemy is gone
// Stomp: the hero is falling (vy > 0) and his feet (bottom row) are within
// STOMP_WINDOW rows of the enemy's top: rows ENEMY_HB_T..ENEMY_HB_T+7 = the
// cap. Max fall speed is 7 px/frame, so a falling hero can't skip past it.
#define STOMP_WINDOW 8
#define BOUNCE_VEL 72       // after a stomp: 4.5 px/frame up (~27 px hop)
// Holding jump during a stomp bounces a full jump (JUMP_VEL) instead.

// Enemy art frames (art/enemy.txt): base frames face LEFT, + ART_ENEMY_MIRROR = right
#define ENEMY_FRAME_WALK 0  // 0, 1
#define ENEMY_FRAME_SQUASHED 2
#define ENEMY_FRAME_MIRROR 3

typedef enum {
	ENEMY_WALK = 0,
	ENEMY_SQUASHED,         // timer counts down, no collision
	ENEMY_GONE,
} tEnemyState;

typedef struct {
	int16_t x, y;       // sprite top-left in level pixels (y fixed: they never fall)
	int16_t xFix;       // x << FIX_SHIFT plus sub-pixel
	int16_t xMin, xMax; // patrol range, from the level at placement (logicEnemyPlace)
	int8_t dir;         // -1 left, +1 right
	uint8_t state;      // tEnemyState
	uint8_t timer;      // squash countdown
} tEnemy;

typedef struct {
	int16_t x, y;       // player top-left in level pixels
	int16_t yFix;       // y << FIX_SHIFT plus sub-pixel
	int16_t vy;         // vertical velocity, 1/16 px per frame (+ = down)
	int16_t cam;        // camera x (level px at screen x 0), 0..CAM_MAX
	uint16_t frame;     // frames since start
	uint8_t onGround;
	uint8_t jumpLatch;  // jump held last frame: must release to jump again
	uint8_t walkFrame;  // sprite frame 0..3
	uint8_t walkTicks;
	uint8_t deaths;     // fell into a pit
	uint8_t jumps;      // jumps started
	uint8_t facingLeft; // last horizontal direction pressed
	uint8_t moving;     // ran this frame on the ground
	tEnemy pEnemies[ENEMY_COUNT];
	uint8_t pOrderY[ENEMY_COUNT]; // enemy ids sorted by y (logicEnemySortY)
	uint8_t stomps;     // enemies stomped
	uint8_t hits;       // times an enemy killed the hero (also counted in deaths)
	uint8_t eventStomp; // this frame: bit i = enemy i was stomped
	uint8_t eventHit;   // this frame: enemy id + 1 that killed the hero, 0 = none
} tGameState;

// Sprite multiplexing plan: which enemies the ONE attached sprite pair
// (channels 4+5) shows this frame, top to bottom. On a channel a sprite must
// end (VSTOP) before the next one starts: next y >= previous y + ENEMY_H + 1
// (the DMA needs the blank line to fetch the next control words).
#define ENEMY_MUX_GAP (ENEMY_H + 1)
typedef struct {
	uint8_t count;              // enemies shown
	uint8_t skipped;            // visible but not shown (vertical overlap)
	uint8_t pId[ENEMY_COUNT];   // shown enemies, sorted by y
} tEnemyPlan;

// Hero animation frames (art/hero.txt); left-facing = + ART_HERO_MIRROR
#define HERO_IDLE 0
#define HERO_BREATHE 1
#define HERO_WALK 2        // 2..9 (8-frame walk cycle)
#define HERO_JUMP 10
#define HERO_FALL 11
#define HERO_BREATHE_SHIFT 5  // idle <-> breathe every 32 frames

/** Which hero frame (0..7, right-facing) shows this state. */
uint8_t logicHeroFrame(const tGameState *pState);

extern const char *const g_pLevelRows[LEVEL_TILES_H];

/** Tile id at tile coordinates; outside the map = TILE_EMPTY. */
uint8_t logicTileAt(int16_t tx, int16_t ty);
/** Art frame (art/tiles.txt, 0-based) for a solid tile: grass and dirt next
 *  to a pit get a shaded cliff face. Drawing only; collision uses logicTileAt. */
uint8_t logicTileArtFrame(int16_t tx, int16_t ty);

void logicInit(tGameState *pState);

/** Advance one frame. Returns 1 if anything visible changed. */
uint8_t logicUpdate(tGameState *pState, const tInput *pInput);

/** Put an enemy at x,y walking dir, and work out its patrol range once:
 *  it walks until the next step would leave the level, enter a solid tile
 *  or step off its floor (pit / platform edge). The level never changes, so
 *  the per-frame move is a compare instead of three tile lookups. */
void logicEnemyPlace(tEnemy *pEnemy, int16_t x, int16_t y, int8_t dir);

/** Art frame (0..5) for an enemy: walk A/B by distance walked, squashed;
 *  + ENEMY_FRAME_MIRROR when walking right. */
uint8_t logicEnemyFrame(const tEnemy *pEnemy);

/** Sort pOrderY by enemy y (stable). logicInit() does it; call it again
 *  after changing an enemy's y (enemies never change height in the game). */
void logicEnemySortY(tGameState *pState);

/** Plan the enemy sprite chain for the current camera: visible enemies
 *  (any column on screen, not gone), sorted by y. When two overlap
 *  vertically only one can be shown: on even frames the upper (earlier) one
 *  wins, on odd frames the lower one - they flicker at 25 Hz instead of one
 *  vanishing for good. */
void logicEnemyPlan(const tGameState *pState, tEnemyPlan *pPlan);

/** Camera for a player x: centred on the player, clamped to the level. */
int16_t logicCameraFor(int16_t playerX);

/** Scroll offset of a looping parallax band: (cam >> shift) wrapped to loopW. */
uint16_t logicBandOffset(int16_t cam, uint8_t shift, uint16_t loopW);

/** Bitplane pointer byte offset (may be -2) and BPLCON1 delay (0..15) that
 *  show pixel column `scrollX` at screen x 0, with one extra fetch word
 *  (DDFSTRT 0x30, modulo - 2). */
int16_t logicScrollByteOffset(uint16_t scrollX);
uint8_t logicScrollDelay(uint16_t scrollX);

/** Colour of sky band i (0..SKY_BANDS-1, top to bottom), 0xRGB. */
uint16_t logicSkyColor(uint8_t i);
/** Colour of haze band i (0..HAZE_BANDS-1) behind the hills. */
uint16_t logicHazeColor(uint8_t i);
/** Colour of pit line i (0..PIT_BANDS-1), from BANDS_END down. */
uint16_t logicPitColor(uint8_t i);

#endif // _LOGIC_H_
