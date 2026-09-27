// Host-side tests for src/logic.c. Run with: agk unit
#include <stdio.h>
#include <stdlib.h>
#include "logic.h"

static int s_failures;

#define CHECK(cond) do { \
	if(!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); ++s_failures; } \
} while(0)

static tGame s_g;   // (big: not on the stack)

static void run(tGame *g, tInput in, int frames) {
	for(int i = 0; i < frames; ++i) logicUpdate(g, &in);
}

// A game with nothing in it but the player (the script far away)
static void emptyGame(tGame *g) {
	logicInit(g);
	logicStart(g);
	g->levelFrame = 100000;          // past the script
	g->script = 0xFFFF;
	g->invulnerable = 0;
	g->message = 0;
}

static int count(const tGame *g, uint8_t type) {
	int n = 0;
	for(int i = 0; i < ENEMIES_MAX; ++i) n += g->enemies[i].type == type;
	return n;
}

static int shots(const tGame *g) {
	int n = 0;
	for(int i = 0; i < SHOTS_MAX; ++i) n += g->shots[i].alive;
	return n;
}

// ------------------------------------------------------------------ tests

static void testTitleThenStart(void) {
	tGame *g = &s_g;
	logicInit(g);
	CHECK(g->phase == PHASE_TITLE && g->message == MSG_TITLE && !logicShipVisible(g));
	run(g, (tInput){.fire = 1}, TITLE_WAIT);
	CHECK(g->phase == PHASE_TITLE);              // fire held from before: ignored
	run(g, (tInput){.fire = 1}, 2);
	CHECK(g->phase == PHASE_PLAY && g->lives == LIVES && g->message == MSG_STAGE);
	run(g, (tInput){0}, INTRO_FRAMES);
	CHECK(g->message == MSG_NONE);
}

static void testMovesAndStaysOnScreen(void) {
	tGame *g = &s_g;
	emptyGame(g);
	int16_t x0 = g->px;
	run(g, (tInput){.dx = 1}, 10);
	CHECK(g->px == x0 + 10 * PLAYER_SPEED);
	run(g, (tInput){.dx = 1, .dy = -1}, 500);
	CHECK(g->px == 300 * FIX && g->py == (PLAY_TOP + 9) * FIX);
	CHECK(logicShipFrame(g) == 2);               // climbing
	run(g, (tInput){.dy = 1}, 1);
	CHECK(logicShipFrame(g) == 3);
}

static void testWeapons(void) {
	tGame *g = &s_g;
	for(int w = 0; w <= WEAPON_MAX; ++w) {
		emptyGame(g);
		g->weapon = (uint8_t)w;
		run(g, (tInput){.fire = 1}, 1);
		CHECK(shots(g) == w + 1);                // 1, 2, 3 shots
		int gap = w == 2 ? FIRE_FRAMES + 2 : FIRE_FRAMES;   // (the spread gun is a little slower)
		run(g, (tInput){.fire = 1}, gap - 1);
		CHECK(shots(g) == w + 1);                // not before the gun is ready
		run(g, (tInput){.fire = 1}, 1);
		CHECK(shots(g) == 2 * (w + 1));
	}
}

static void testKillADart(void) {
	tGame *g = &s_g;
	emptyGame(g);
	g->enemies[0] = (tEnemy){.type = EN_DART, .hp = 1, .x = 200 * FIX, .y = g->py, .y0 = g->py};
	for(int f = 0; f < 60 && g->enemies[0].type; ++f) logicUpdate(g, &(tInput){.fire = 1});
	CHECK(g->enemies[0].type == EN_NONE && g->score == 100 && g->kills == 1);
	int pops = 0;
	for(int i = 0; i < FX_MAX; ++i) pops += g->fx[i].type == FX_POP;
	CHECK(pops == 1);
}

static void testFormationDropsACapsule(void) {
	tGame *g = &s_g;
	emptyGame(g);
	g->groupSize[1] = 5;
	for(int i = 0; i < 5; ++i) {
		g->enemies[i] = (tEnemy){.type = EN_DART, .hp = 1, .group = 1, .x = (int16_t)(140 + 20 * i) * FIX,
		                         .y = g->py, .y0 = g->py};
	}
	for(int f = 0; f < 200 && count(g, EN_DART); ++f) logicUpdate(g, &(tInput){.fire = 1});
	CHECK(count(g, EN_DART) == 0 && count(g, EN_CAPSULE) == 1);
	// it drifts into the ship: a wider gun
	for(int f = 0; f < 400 && count(g, EN_CAPSULE); ++f) logicUpdate(g, &(tInput){0});
	CHECK(count(g, EN_CAPSULE) == 0 && g->weapon == 1 && !g->dead);
}

static void testRockBreaks(void) {
	tGame *g = &s_g;
	emptyGame(g);
	g->enemies[0] = (tEnemy){.type = EN_ROCK, .hp = 4, .x = 250 * FIX, .y = g->py, .y0 = g->py};
	for(int f = 0; f < 80 && count(g, EN_ROCK); ++f) logicUpdate(g, &(tInput){.fire = 1});
	CHECK(count(g, EN_ROCK) == 0 && count(g, EN_PEBBLE) == 2 && g->score == 200);
}

static void testBulletKillsThenRespawn(void) {
	tGame *g = &s_g;
	emptyGame(g);
	g->weapon = 2;
	g->bullets[0] = (tShot){.x = g->px + 30 * FIX, .y = g->py, .vx = -2 * FIX, .alive = 1};
	run(g, (tInput){0}, 30);
	CHECK(g->dead && g->lives == LIVES - 1 && g->weapon == 1);
	CHECK(!logicShipVisible(g));
	run(g, (tInput){0}, RESPAWN_FRAMES);
	CHECK(!g->dead && g->invulnerable > 0 && g->px == 48 * FIX);
	// invulnerable: a bullet passes through
	g->bullets[0] = (tShot){.x = g->px + 20 * FIX, .y = g->py, .vx = -2 * FIX, .alive = 1};
	run(g, (tInput){0}, 20);
	CHECK(!g->dead && g->lives == LIVES - 1);
}

static void testGameOver(void) {
	tGame *g = &s_g;
	emptyGame(g);
	for(int life = 0; life < LIVES; ++life) {
		g->invulnerable = 0;
		g->bullets[0] = (tShot){.x = g->px, .y = g->py, .alive = 1};
		run(g, (tInput){0}, RESPAWN_FRAMES + 2);
	}
	CHECK(g->phase == PHASE_OVER && g->message == MSG_OVER && g->lives == 0);
	run(g, (tInput){0}, OVER_FRAMES);
	CHECK(g->phase == PHASE_TITLE);
}

// The whole level, played by a simple bot that can't die (it's testing the
// level, not the bot): every wave comes, the Warden comes, can be beaten at
// its core, and the stage clears.
static void testTheWholeLevel(void) {
	tGame *g = &s_g;
	logicInit(g);
	logicStart(g);
	int bossSeen = 0, clearFrame = -1, maxEnemies = 0, maxBullets = 0, bossDead = 0;
	uint32_t score = 0;
	uint16_t kills = 0;
	for(int f = 0; f < 180 * FPS && g->phase != PHASE_TITLE; ++f) {
		g->invulnerable = 2;
		// follow the boss's core (or stay mid-screen), always firing
		int16_t ty = g->boss.state == BOSS_FIGHT ? g->boss.y : 140 * FIX;
		int16_t tx = g->boss.state == BOSS_FIGHT ? 150 * FIX : 48 * FIX;
		tInput in = {.fire = 1, .dy = (int8_t)(g->py < ty - FIX ? 1 : g->py > ty + FIX ? -1 : 0),
		             .dx = (int8_t)(g->px < tx - 2 * FIX ? 1 : g->px > tx + 2 * FIX ? -1 : 0)};
		logicUpdate(g, &in);
		bossSeen |= g->boss.state == BOSS_FIGHT;
		if(g->phase == PHASE_CLEAR && clearFrame < 0) {
			clearFrame = f;
			score = g->score;
			kills = g->kills;
			bossDead = g->boss.state == BOSS_DEAD;
		}
		int e = 0, b = 0;
		for(int i = 0; i < ENEMIES_MAX; ++i) e += g->enemies[i].type != EN_NONE;
		for(int i = 0; i < BULLETS_MAX; ++i) b += g->bullets[i].alive;
		if(e > maxEnemies) maxEnemies = e;
		if(b > maxBullets) maxBullets = b;
		tDraw pDraw[DRAW_MAX];
		CHECK(logicDraw(g, pDraw) <= DRAW_MAX);
	}
	printf("level: boss beaten at %d s, score %u, kills %u, up to %d enemies and %d bullets at once\n",
	       clearFrame / FPS, (unsigned)score, kills, maxEnemies, maxBullets);
	CHECK(bossSeen && clearFrame > 0 && bossDead);
	CHECK(score > 10000 && kills > 20);
	CHECK(g->phase == PHASE_TITLE);              // after STAGE CLEAR
	CHECK(g->hiScore >= 10000);
}

static void testTheDemoPlaysTheLevel(void) {
	// left on the title, the game plays itself - all of it, the Warden too
	tGame *g = &s_g;
	logicInit(g);
	run(g, (tInput){0}, DEMO_WAIT + 2);
	CHECK(g->phase == PHASE_PLAY && g->isDemo);
	int cleared = 0, kills = 0, lives = 0;
	for(int f = 0; f < 150 * FPS && g->phase != PHASE_TITLE; ++f) {
		logicUpdate(g, &(tInput){0});
		if(g->phase == PHASE_CLEAR && !cleared) {
			cleared = 1;
			kills = g->kills;
			lives = g->lives;
		}
	}
	printf("demo: stage cleared with %d kills\n", kills);
	CHECK(cleared && lives == LIVES && kills > 50);
	// fire ends a demo
	run(g, (tInput){0}, DEMO_WAIT + 2);
	CHECK(g->isDemo);
	run(g, (tInput){.fire = 1}, 1);
	CHECK(g->phase == PHASE_TITLE && !g->isDemo);
}

int main(void) {
	testTitleThenStart();
	testMovesAndStaysOnScreen();
	testWeapons();
	testKillADart();
	testFormationDropsACapsule();
	testRockBreaks();
	testBulletKillsThenRespawn();
	testGameOver();
	testTheWholeLevel();
	testTheDemoPlaysTheLevel();
	if(s_failures) {
		printf("%d check(s) failed\n", s_failures);
		return 1;
	}
	return 0;
}
