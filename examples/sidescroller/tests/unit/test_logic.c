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
	CHECK(logicHazeColor(0) == 0xBCE && logicHazeColor(HAZE_BANDS - 1) == 0x7AB);
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

// ----------------------------------------------------------------- enemies

// Freeze every enemy except `keep` (GONE), so tests see one at a time.
static void onlyEnemy(tGameState *s, uint8_t keep) {
	for(uint8_t i = 0; i < ENEMY_COUNT; ++i) {
		if(i != keep) s->pEnemies[i].state = ENEMY_GONE;
	}
}

static void setEnemy(tEnemy *e, int16_t x, int16_t y, int8_t dir) {
	logicEnemyPlace(e, x, y, dir);
}

// Park the hero far away (on the far right) so he touches nothing.
static void parkHero(tGameState *s) {
	s->x = LEVEL_W - PLAYER_W; s->y = START_Y; s->yFix = START_Y << FIX_SHIFT;
	s->vy = 0; s->onGround = 1;
}

static void testEnemySpawns(void) {
	tGameState s;
	logicInit(&s);
	for(uint8_t i = 0; i < ENEMY_COUNT; ++i) {
		tEnemy *e = &s.pEnemies[i];
		CHECK(e->state == ENEMY_WALK);
		// standing on a floor tile, not inside anything
		CHECK(logicTileAt((e->x + ENEMY_HB_L) >> TILE_SHIFT, (e->y + ENEMY_H) >> TILE_SHIFT) != TILE_EMPTY);
		CHECK(logicTileAt((e->x + ENEMY_HB_R) >> TILE_SHIFT, (e->y + ENEMY_H) >> TILE_SHIFT) != TILE_EMPTY);
		CHECK(logicTileAt((e->x + 8) >> TILE_SHIFT, e->y >> TILE_SHIFT) == TILE_EMPTY);
		// far enough from the hero's start not to kill him on spawn
		CHECK(e->x > START_X + 150 || e->y + ENEMY_H <= START_Y - 64);
	}
}

// Walk one enemy for `frames` frames; return its x range.
static void patrol(tGameState *s, uint8_t id, int frames, int16_t *pMin, int16_t *pMax, int *pTurns) {
	int8_t dir = s->pEnemies[id].dir;
	*pMin = *pMax = s->pEnemies[id].x;
	*pTurns = 0;
	for(int f = 0; f < frames; ++f) {
		int16_t x0 = s->pEnemies[id].x;
		step(s, 0, 0, 1);
		int16_t x = s->pEnemies[id].x;
		CHECK(x - x0 >= -1 && x - x0 <= 1);
		if(x < *pMin) *pMin = x;
		if(x > *pMax) *pMax = x;
		if(s->pEnemies[id].dir != dir) { ++*pTurns; dir = s->pEnemies[id].dir; }
	}
}

static void testEnemySpeed(void) {
	tGameState s;
	logicInit(&s);
	parkHero(&s);
	int16_t x0 = s.pEnemies[1].x;
	step(&s, 0, 0, 40);
	CHECK(s.pEnemies[1].x == x0 - 20); // 0.5 px/frame, walking left
}

static void testEnemyTurnsAtPlatformEdges(void) {
	// Enemy 0 on the slab x 240..303: its feet (cols 2..13) never leave it.
	tGameState s;
	logicInit(&s);
	parkHero(&s);
	int16_t mn, mx; int turns;
	patrol(&s, 0, 600, &mn, &mx, &turns);
	CHECK(mn == 240 - ENEMY_HB_L);          // 238
	CHECK(mx == 303 - ENEMY_HB_R);          // 290
	CHECK(turns >= 4);
	CHECK(s.pEnemies[0].y == 7 * TILE_SIZE - ENEMY_H);
	// Enemy 3 on the narrow high slab 720..767
	patrol(&s, 3, 400, &mn, &mx, &turns);
	CHECK(mn == 720 - ENEMY_HB_L && mx == 767 - ENEMY_HB_R);
}

static void testEnemyTurnsAtPits(void) {
	// Enemy 1 on the ground between pit 1 (352..399) and pit 2 (752..799).
	tGameState s;
	logicInit(&s);
	parkHero(&s);
	int16_t mn, mx; int turns;
	patrol(&s, 1, 1600, &mn, &mx, &turns);
	CHECK(mn == 400 - ENEMY_HB_L);
	CHECK(mx == 751 - ENEMY_HB_R);
	CHECK(turns >= 2);
}

static void testEnemyTurnsAtLevelEnds(void) {
	tGameState s;
	logicInit(&s);
	onlyEnemy(&s, 5);
	s.x = 200; // hero out of the way (enemy 5 is on the right)
	setEnemy(&s.pEnemies[5], LEVEL_W - ENEMY_W - 3, START_Y, 1);
	int16_t mn, mx; int turns;
	patrol(&s, 5, 20, &mn, &mx, &turns);
	CHECK(mx == LEVEL_W - ENEMY_W && turns == 1);
	setEnemy(&s.pEnemies[5], 3, START_Y, -1);
	s.x = 600;
	patrol(&s, 5, 20, &mn, &mx, &turns);
	CHECK(mn == 0 && turns == 1);
}

static void testEnemyFrames(void) {
	tEnemy e;
	setEnemy(&e, 100, 0, -1);
	CHECK(logicEnemyFrame(&e) == ENEMY_FRAME_WALK + 1);   // (100 >> 2) & 1
	e.x = 104;
	CHECK(logicEnemyFrame(&e) == ENEMY_FRAME_WALK);
	e.dir = 1;
	CHECK(logicEnemyFrame(&e) == ENEMY_FRAME_WALK + ENEMY_FRAME_MIRROR);
	e.state = ENEMY_SQUASHED;
	CHECK(logicEnemyFrame(&e) == ENEMY_FRAME_SQUASHED + ENEMY_FRAME_MIRROR);
	e.dir = -1;
	CHECK(logicEnemyFrame(&e) == ENEMY_FRAME_SQUASHED);
	// walking: both poses show within 8 px (16 frames)
	tGameState s;
	logicInit(&s);
	parkHero(&s);
	uint8_t seen[6] = {0};
	for(int f = 0; f < 20; ++f) { step(&s, 0, 0, 1); seen[logicEnemyFrame(&s.pEnemies[1])] = 1; }
	CHECK(seen[0] && seen[1] && !seen[2]);
}

// Hero directly above enemy 1, falling onto it.
static void dropOnEnemy(tGameState *s, int16_t heightAbove) {
	logicInit(s);
	onlyEnemy(s, 1);
	setEnemy(&s->pEnemies[1], 500, START_Y, -1);
	s->x = 500; s->y = START_Y - heightAbove; s->yFix = s->y << FIX_SHIFT;
	s->vy = 0; s->onGround = 0;
}

static void testStompBouncesAndSquashes(void) {
	tGameState s;
	dropOnEnemy(&s, 40);
	int f = 0;
	while(!s.stomps && f < 60) { step(&s, 0, 0, 1); ++f; }
	CHECK(s.stomps == 1 && s.deaths == 0 && s.hits == 0);
	CHECK(s.eventStomp == (1 << 1));
	CHECK(s.pEnemies[1].state == ENEMY_SQUASHED);
	CHECK(logicEnemyFrame(&s.pEnemies[1]) == ENEMY_FRAME_SQUASHED);
	CHECK(s.vy == -BOUNCE_VEL && !s.onGround);
	// feet landed in the cap window
	CHECK(s.y + PLAYER_H - 1 >= START_Y + ENEMY_HB_T && s.y + PLAYER_H - 1 < START_Y + ENEMY_HB_T + STOMP_WINDOW);
	int16_t yStomp = s.y, minY = s.y;
	int16_t ex = s.pEnemies[1].x;
	// squashed: stays put, SQUASH_TICKS frames, then gone
	for(int t = 1; t < SQUASH_TICKS; ++t) {
		step(&s, 0, 0, 1);
		if(s.y < minY) minY = s.y;
		CHECK(s.pEnemies[1].state == ENEMY_SQUASHED && s.pEnemies[1].x == ex);
		CHECK(s.eventStomp == 0);
	}
	step(&s, 0, 0, 1);
	CHECK(s.pEnemies[1].state == ENEMY_GONE);
	int16_t bounce = yStomp - minY;
	printf("stomp: bounce %d px\n", bounce);
	CHECK(bounce >= 20 && bounce <= 32);   // a small hop, well under a jump (54)
	// landing back where the enemy was: no collision with a squashed/gone enemy
	step(&s, 0, 0, 40);
	CHECK(s.onGround && s.y == START_Y && s.deaths == 0 && s.stomps == 1);
}

static void testStompWithJumpHeldBouncesHigh(void) {
	tGameState s;
	dropOnEnemy(&s, 40);
	int f = 0;
	while(!s.stomps && f < 60) { step(&s, 0, 1, 1); ++f; }
	CHECK(s.stomps == 1 && s.vy == -JUMP_VEL);
}

static void testStompWindow(void) {
	// One frame of a slow fall (vy 16 -> 22: 1 px). Feet end up at
	// eT + 7 (last cap row in the window) -> stomp; eT + 8 -> hit.
	int16_t eT = START_Y + ENEMY_HB_T;
	for(int16_t d = 7; d <= 8; ++d) {
		tGameState s;
		dropOnEnemy(&s, 0);
		s.y = eT + d - PLAYER_H; // feet 1 px above, moves 1 px down
		s.yFix = s.y << FIX_SHIFT;
		s.vy = 16;
		step(&s, 0, 0, 1);
		if(d == 7) CHECK(s.stomps == 1 && s.deaths == 0);
		else CHECK(s.stomps == 0 && s.deaths == 1 && s.eventHit == 2);
	}
}

static void testSideHitRespawns(void) {
	tGameState s;
	logicInit(&s);
	onlyEnemy(&s, 1);
	setEnemy(&s.pEnemies[1], 500, START_Y, -1);
	s.x = 440; // on the ground, walking right into it
	int f = 0;
	while(!s.deaths && f < 60) { step(&s, 1, 0, 1); ++f; }
	CHECK(s.deaths == 1 && s.hits == 1 && s.eventHit == 2 && s.stomps == 0);
	CHECK(s.x == START_X && s.y == START_Y && s.onGround); // respawned
	CHECK(s.pEnemies[1].state == ENEMY_WALK);              // enemies keep walking
	int16_t ex = s.pEnemies[1].x;
	step(&s, 0, 0, 2);
	CHECK(s.eventHit == 0 && s.deaths == 1 && s.pEnemies[1].x == ex - 1);
	// an enemy walking into a standing hero kills him too
	logicInit(&s);
	onlyEnemy(&s, 1);
	setEnemy(&s.pEnemies[1], 480, START_Y, -1);
	s.x = 460;
	step(&s, 0, 0, 20);
	CHECK(s.deaths == 1 && s.hits == 1);
}

static void testJumpingUpIntoEnemyKills(void) {
	// Enemy 2 walks on the low one-way slab (y 144). Jumping up through the
	// slab into it from below is a hit, not a stomp.
	tGameState s;
	logicInit(&s);
	onlyEnemy(&s, 2);
	setEnemy(&s.pEnemies[2], 510, 10 * TILE_SIZE - ENEMY_H, 1);
	s.x = 510 + 8;
	step(&s, 0, 1, 1);
	step(&s, 0, 0, 30);
	CHECK(s.deaths == 1 && s.stomps == 0 && s.eventHit == 0 && s.x == START_X);
}

static void testSquashedEnemyIsHarmless(void) {
	tGameState s;
	logicInit(&s);
	onlyEnemy(&s, 1);
	setEnemy(&s.pEnemies[1], 500, START_Y, -1);
	s.pEnemies[1].state = ENEMY_SQUASHED;
	s.pEnemies[1].timer = SQUASH_TICKS;
	s.x = 470;
	step(&s, 1, 0, 30); // runs right through it
	CHECK(s.deaths == 0 && s.x == 530);
}

static void testEnemyPlan(void) {
	tGameState s;
	tEnemyPlan p;
	logicInit(&s);
	// camera 0: only enemy 0 (x 270) is on screen
	logicEnemyPlan(&s, &p);
	CHECK(p.count == 1 && p.pId[0] == 0 && p.skipped == 0);
	// camera 400: enemies 1 (y 192) and 2 (y 144) -> sorted by y: 2 then 1
	s.cam = 400;
	logicEnemyPlan(&s, &p);
	CHECK(p.count == 2 && p.pId[0] == 2 && p.pId[1] == 1 && p.skipped == 0);
	// culling: x - cam in -15..319 is visible
	s.cam = 270 + ENEMY_W - 1;
	logicEnemyPlan(&s, &p);
	CHECK(p.count >= 1 && p.pId[0] == 0);
	s.cam = 270 + ENEMY_W;
	logicEnemyPlan(&s, &p);
	CHECK(p.count == 0 || p.pId[0] != 0);
	// gone enemies are not shown; squashed ones are
	s.cam = 400;
	s.pEnemies[2].state = ENEMY_GONE;
	s.pEnemies[1].state = ENEMY_SQUASHED;
	logicEnemyPlan(&s, &p);
	CHECK(p.count == 1 && p.pId[0] == 1);
	// all spawn heights are multiplex-compatible (different floors)
	logicInit(&s);
	for(uint8_t i = 0; i < ENEMY_COUNT; ++i) {
		for(uint8_t j = 0; j < ENEMY_COUNT; ++j) {
			int16_t d = s.pEnemies[i].y - s.pEnemies[j].y;
			CHECK(d == 0 || d >= ENEMY_MUX_GAP || d <= -ENEMY_MUX_GAP);
		}
	}
}

static void testEnemyPlanOverlap(void) {
	// Three enemies on screen: A at y 100, B at y 110 (overlaps A), C at 150.
	tGameState s;
	tEnemyPlan p;
	logicInit(&s);
	for(uint8_t i = 0; i < ENEMY_COUNT; ++i) s.pEnemies[i].state = ENEMY_GONE;
	s.cam = 0;
	setEnemy(&s.pEnemies[4], 50, 110, 1);  // B (listed first: sort must reorder)
	setEnemy(&s.pEnemies[2], 100, 100, 1); // A
	setEnemy(&s.pEnemies[6], 150, 150, 1); // C
	logicEnemySortY(&s);
	s.frame = 0; // even: the upper one (A) wins
	logicEnemyPlan(&s, &p);
	CHECK(p.count == 2 && p.skipped == 1 && p.pId[0] == 2 && p.pId[1] == 6);
	s.frame = 1; // odd: the lower one (B) wins
	logicEnemyPlan(&s, &p);
	CHECK(p.count == 2 && p.skipped == 1 && p.pId[0] == 4 && p.pId[1] == 6);
	// Exactly ENEMY_MUX_GAP apart: both fit (VSTOP of A < VSTART of B)
	s.pEnemies[4].y = 100 + ENEMY_MUX_GAP;
	logicEnemySortY(&s);
	logicEnemyPlan(&s, &p);
	CHECK(p.count == 3 && p.skipped == 0);
	s.pEnemies[4].y = 100 + ENEMY_MUX_GAP - 1; // one line closer: conflict
	logicEnemySortY(&s);
	logicEnemyPlan(&s, &p);
	CHECK(p.count == 2 && p.skipped == 1);
	// Invariant for any layout: shown list is sorted with gaps >= ENEMY_MUX_GAP
	for(int16_t yb = 60; yb < 200; yb += 3) {
		s.pEnemies[4].y = yb;
		logicEnemySortY(&s);
		for(uint16_t fr = 0; fr < 2; ++fr) {
			s.frame = fr;
			logicEnemyPlan(&s, &p);
			CHECK(p.count + p.skipped == 3);
			for(uint8_t k = 1; k < p.count; ++k) {
				CHECK(s.pEnemies[p.pId[k]].y >= s.pEnemies[p.pId[k - 1]].y + ENEMY_MUX_GAP);
			}
		}
	}
}

int main(void) {
	testEnemySpawns();
	testEnemySpeed();
	testEnemyTurnsAtPlatformEdges();
	testEnemyTurnsAtPits();
	testEnemyTurnsAtLevelEnds();
	testEnemyFrames();
	testStompBouncesAndSquashes();
	testStompWithJumpHeldBouncesHigh();
	testStompWindow();
	testSideHitRespawns();
	testJumpingUpIntoEnemyKills();
	testSquashedEnemyIsHarmless();
	testEnemyPlan();
	testEnemyPlanOverlap();
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
