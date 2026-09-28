#include "logic.h"

// 16-bit multiply and divide: the 68000 has MULS/DIVS for these, but GCC
// calls slow library routines for int (32-bit) arithmetic.
static inline int32_t mul16(int16_t a, int16_t b) {
#if defined(__mc68000__)
	int32_t r = a;
	__asm__("muls.w %1,%0" : "+d"(r) : "dmi"(b));
	return r;
#else
	return (int32_t)a * b;
#endif
}
static inline int16_t div16(int32_t a, int16_t b) {
#if defined(__mc68000__)
	int32_t r = a;
	__asm__("divs.w %1,%0" : "+d"(r) : "dmi"(b));
	return (int16_t)r;
#else
	return (int16_t)(a / b);
#endif
}

// DIVU: quotient and remainder of a 16-bit division (GCC would call a
// library routine for % and / on int)
static inline uint16_t divu(uint16_t a, uint16_t b) {
#if defined(__mc68000__)
	uint32_t r = a;
	__asm__("divu.w %1,%0" : "+d"(r) : "dmi"(b));
	return (uint16_t)r;
#else
	return a / b;
#endif
}
static inline uint16_t modu(uint16_t a, uint16_t b) {
#if defined(__mc68000__)
	uint32_t r = a;
	__asm__("divu.w %1,%0" : "+d"(r) : "dmi"(b));
	return (uint16_t)(r >> 16);
#else
	return a % b;
#endif
}

const uint8_t g_pBobW[BOB_TYPES] = {16, 32, 16, 8, 16, 32, 16, 16, 32, 16, 96};
const uint8_t g_pBobH[BOB_TYPES] = {12, 16, 4, 8, 16, 32, 16, 16, 32, 16, 46};
const uint8_t g_pBobFrames[BOB_TYPES] = {2, 2, 1, 2, 2, 4, 4, 4, 6, 5, 2};

// sin(i * 2pi / 64) * 127
static const int8_t s_pSin[64] = {
	0, 12, 25, 37, 49, 60, 71, 81, 90, 98, 106, 112, 117, 122, 125, 126,
	127, 126, 125, 122, 117, 112, 106, 98, 90, 81, 71, 60, 49, 37, 25, 12,
	0, -12, -25, -37, -49, -60, -71, -81, -90, -98, -106, -112, -117, -122, -125, -126,
	-127, -126, -125, -122, -117, -112, -106, -98, -90, -81, -71, -60, -49, -37, -25, -12
};
static inline int16_t sinAmp(uint16_t i, int16_t amp) {   // amp * sin(i/64 turn)
	return (int16_t)(mul16(s_pSin[i & 63], amp) >> 7);
}

// The same, a quarter step at a time: i is in 1/4 table steps (a smooth
// glide for slow movements - whole steps are ~6 px jumps on a big sweep)
static inline int16_t sinAmpFine(uint16_t i, int16_t amp) {
	int16_t a = s_pSin[(i >> 2) & 63], b = s_pSin[((i >> 2) + 1) & 63];
	int16_t v = a + (int16_t)(mul16((int16_t)(b - a), (int16_t)(i & 3)) >> 2);
	return (int16_t)(mul16(v, amp) >> 7);
}

static uint16_t rnd16(tGame *g) {   // xorshift: shifts and xors only
	uint32_t x = g->rnd;
	x ^= x << 13;
	x ^= x >> 17;
	x ^= x << 5;
	g->rnd = x;
	return (uint16_t)(x >> 8);
}

// ------------------------------------------------------------ the level

#define W_DARTS 1       // arg: wave height, px (5 darts)
#define W_MINE 2
#define W_ROCK 3        // arg: vertical drift, 1/16 px per frame
#define W_PEBBLES 4     // arg: how many
#define W_GUNSHIP 5
#define W_WARNING 6
#define W_BOSS 7
typedef struct {
	uint16_t frame;
	uint8_t kind;
	uint8_t y;          // px
	int8_t arg;
} tWave;
static const tWave s_pScript[] = {
	{ 170, W_DARTS,   80, 24},
	{ 330, W_DARTS,  180, 24},
	{ 480, W_PEBBLES,  0,  6},
	{ 600, W_MINE,    90,  0},
	{ 660, W_MINE,   190,  0},
	{ 800, W_DARTS,  135, 40},
	{ 950, W_ROCK,    70,  4},
	{1040, W_ROCK,   190, -4},
	{1120, W_PEBBLES,  0,  5},
	{1200, W_ROCK,   130,  0},
	{1330, W_GUNSHIP,135,  0},
	{1560, W_DARTS,   70, 20},
	{1620, W_DARTS,  200, 20},
	{1800, W_MINE,    70,  0},
	{1830, W_MINE,   135,  0},
	{1860, W_MINE,   200,  0},
	{2050, W_ROCK,    60,  6},
	{2130, W_ROCK,   150, -2},
	{2200, W_PEBBLES,  0,  4},
	{2270, W_ROCK,   210, -6},
	{2340, W_DARTS,  110, 48},
	{2480, W_GUNSHIP, 80,  0},
	{2560, W_GUNSHIP,190,  0},
	{2800, W_DARTS,   90, 30},
	{2860, W_DARTS,  180, 30},
	{2940, W_MINE,   135,  0},
	{3100, W_PEBBLES,  0,  8},
	{3200, W_DARTS,  140, 56},
	{3380, W_WARNING,  0,  0},
	{3560, W_BOSS,   140,  0},
};
#define SCRIPT_LEN (sizeof(s_pScript) / sizeof(s_pScript[0]))

// Dart formations spawn one dart every DART_GAP frames
#define DART_GAP 14
#define DARTS 5
typedef struct { uint8_t left, wait, group; uint8_t y; int8_t amp; } tSpawner;
static tSpawner s_pSpawners[4];

// ---------------------------------------------------------------- helpers

static tEnemy *enemyAdd(tGame *g, uint8_t type, int16_t x, int16_t y) {
	for(uint8_t i = 0; i < ENEMIES_MAX; ++i) {
		tEnemy *e = &g->enemies[i];
		if(e->type == EN_NONE) {
			static const uint8_t pHp[] = {0, 1, 3, 4, 1, 8, 1};
			*e = (tEnemy){.type = type, .hp = pHp[type], .x = x, .y = y, .y0 = y};
			return e;
		}
	}
	return 0;
}

static void fxAdd(tGame *g, uint8_t type, int16_t x, int16_t y) {
	for(uint8_t i = 0; i < FX_MAX; ++i) {
		if(!g->fx[i].type) {
			g->fx[i] = (tFx){.type = type, .t = 0, .x = x, .y = y};
			return;
		}
	}
}

static void bulletAdd(tGame *g, int16_t x, int16_t y, int16_t vx, int16_t vy) {
	for(uint8_t i = 0; i < BULLETS_MAX; ++i) {
		tShot *b = &g->bullets[i];
		if(!b->alive) {
			*b = (tShot){.x = x, .y = y, .vx = vx, .vy = vy, .alive = 1};
			return;
		}
	}
}

// A bullet from (x, y) at the player, `speed` FIX per frame
static void bulletAimed(tGame *g, int16_t x, int16_t y, int16_t speed) {
	int16_t dx = (g->px - x) >> 4, dy = (g->py - y) >> 4;   // px
	int16_t ax = dx < 0 ? -dx : dx, ay = dy < 0 ? -dy : dy;
	int16_t m = ax > ay ? ax : ay;
	if(!m) m = 1;
	// the longer axis at full speed (a square, not a circle: close enough)
	bulletAdd(g, x, y, div16(mul16(dx, speed), m), div16(mul16(dy, speed), m));
}

static inline uint8_t boxHit(int16_t ax, int16_t ay, int16_t aw, int16_t ah,
                             int16_t bx, int16_t by, int16_t bw, int16_t bh) {
	// centres and half sizes, FIX
	int16_t dx = ax - bx, dy = ay - by;
	if(dx < 0) dx = -dx;
	if(dy < 0) dy = -dy;
	return dx < aw + bw && dy < ah + bh;
}

static void playerDies(tGame *g) {
	g->evPlayerDied = 1;
	fxAdd(g, FX_BOOM, g->px, g->py);
	g->dead = RESPAWN_FRAMES;
	if(g->weapon) --g->weapon;
	if(g->lives) --g->lives;
}

static void addScore(tGame *g, uint16_t n) {
	g->score += n;
	if(g->score > g->hiScore) g->hiScore = g->score;
}

// ------------------------------------------------------------- the phases

void logicInit(tGame *g) {
	uint32_t hi = g->hiScore;
	*g = (tGame){0};
	g->hiScore = hi;
	g->rnd = 0x1D872B41;
	g->phase = PHASE_TITLE;
	g->message = MSG_TITLE;
	g->px = 64 * FIX;
	g->py = 140 * FIX;
	g->dead = 1;                   // (no ship on the title)
	for(uint8_t i = 0; i < 4; ++i) s_pSpawners[i].left = 0;
}

void logicStart(tGame *g) {
	uint32_t hi = g->hiScore;
	uint16_t frame = g->frame, bgX = g->bgX;
	*g = (tGame){0};
	g->hiScore = hi;
	g->frame = frame;
	g->bgX = bgX;
	g->rnd = 0x1D872B41;
	g->phase = PHASE_PLAY;
	g->lives = LIVES;
	g->px = 48 * FIX;
	g->py = 140 * FIX;
	g->invulnerable = INVULNERABLE_FRAMES;
	g->message = MSG_STAGE;
	for(uint8_t i = 0; i < 4; ++i) s_pSpawners[i].left = 0;
}

// ------------------------------------------------------------ the script

static void scriptRun(tGame *g) {
	while(g->script < SCRIPT_LEN && s_pScript[g->script].frame <= g->levelFrame) {
		const tWave *w = &s_pScript[g->script++];
		int16_t y = (int16_t)w->y * FIX;
		switch(w->kind) {
			case W_DARTS:
				for(uint8_t i = 0; i < 4; ++i) {
					if(!s_pSpawners[i].left) {
						uint8_t grp = (uint8_t)(g->script & 7);
						s_pSpawners[i] = (tSpawner){.left = DARTS, .wait = 0, .group = grp, .y = w->y, .amp = w->arg};
						g->groupKills[grp] = 0;
						g->groupSize[grp] = DARTS;
						break;
					}
				}
				break;
			case W_MINE: {
				tEnemy *e = enemyAdd(g, EN_MINE, 340 * FIX, y);
				if(e) e->vx = -2 * FIX;
				break;
			}
			case W_ROCK: {
				tEnemy *e = enemyAdd(g, EN_ROCK, 350 * FIX, y);
				if(e) { e->vx = -12; e->vy = w->arg; }
				break;
			}
			case W_PEBBLES:
				for(int8_t i = 0; i < w->arg; ++i) {
					int16_t py = (int16_t)(PLAY_TOP + 16 + modu(rnd16(g), SCREEN_H - PLAY_TOP - 32)) * FIX;
					tEnemy *e = enemyAdd(g, EN_PEBBLE, (int16_t)(340 + i * 24) * FIX, py);
					if(e) { e->vx = -(16 + (int16_t)(rnd16(g) & 15)); e->vy = (int16_t)(rnd16(g) & 7) - 4; }
				}
				break;
			case W_GUNSHIP: {
				tEnemy *e = enemyAdd(g, EN_GUNSHIP, 356 * FIX, y);
				if(e) e->vx = -24;
				break;
			}
			case W_WARNING:
				g->message = MSG_WARNING;
				break;
			case W_BOSS:
				g->boss = (tBoss){.state = BOSS_ENTER, .hp = BOSS_HP, .x = 380 * FIX, .y = y};
				g->message = MSG_NONE;
				break;
		}
	}
	// dart formations: one dart every DART_GAP frames
	for(uint8_t i = 0; i < 4; ++i) {
		tSpawner *s = &s_pSpawners[i];
		if(!s->left) continue;
		if(s->wait) { --s->wait; continue; }
		tEnemy *e = enemyAdd(g, EN_DART, 336 * FIX, (int16_t)s->y * FIX);
		if(e) {
			e->vx = -(2 * FIX + 8);
			e->group = s->group;
			e->vy = s->amp;       // (the wave's height, px)
		}
		--s->left;
		s->wait = DART_GAP;
	}
}

// ------------------------------------------------------------- the enemies

static const uint16_t s_pScore[] = {0, 100, 300, 200, 50, 500, 0};

static void enemyDies(tGame *g, tEnemy *e) {
	g->evKill = 1;
	++g->kills;
	addScore(g, s_pScore[e->type]);
	if(e->type == EN_ROCK || e->type == EN_GUNSHIP) {
		g->evBigKill = 1;
		fxAdd(g, FX_BOOM, e->x, e->y);
	}
	else {
		fxAdd(g, FX_POP, e->x, e->y);
	}
	int16_t x = e->x, y = e->y;
	uint8_t type = e->type, grp = e->group;
	e->type = EN_NONE;
	if(type == EN_ROCK) {
		// it breaks in two
		for(int8_t s = -1; s <= 1; s += 2) {
			tEnemy *p = enemyAdd(g, EN_PEBBLE, x, y + s * 8 * FIX);
			if(p) { p->vx = -20; p->vy = s * 12; }
		}
	}
	if(type == EN_DART && ++g->groupKills[grp] == g->groupSize[grp]) {
		// the whole formation: a capsule where the last one fell
		tEnemy *c = enemyAdd(g, EN_CAPSULE, x, y);
		if(c) c->vx = -12;
	}
}

static void enemiesUpdate(tGame *g) {
	for(uint8_t i = 0; i < ENEMIES_MAX; ++i) {
		tEnemy *e = &g->enemies[i];
		if(e->type == EN_NONE) continue;
		++e->t;
		if(e->flash) --e->flash;
		switch(e->type) {
			case EN_DART:
				// a sine wave: vy holds its height, px
				e->x += e->vx;
				e->y = e->y0 + sinAmp((uint16_t)(e->t * 2), (int16_t)(e->vy * FIX));
				break;
			case EN_MINE:
				if(e->t < 200) {
					// glide in and stop; stare and fire
					if(e->x < 250 * FIX && e->vx < 0) e->vx += 1;
					if(e->vx == 0 && modu(e->t, 40) == 20) bulletAimed(g, e->x, e->y, 28);
				}
				else if(e->t == 200) {
					// leave, away from the middle
					e->vx = -20;
					e->vy = e->y < 140 * FIX ? -20 : 20;
				}
				e->x += e->vx;
				e->y += e->vy;
				break;
			case EN_GUNSHIP:
				if(e->t < 420) {
					if(e->x < 236 * FIX) e->vx = 0;
					e->y = e->y0 + sinAmp(e->t, 20 * FIX);
					if(e->vx == 0 && modu(e->t, 70) == 35) {
						for(int16_t vy = -10; vy <= 10; vy += 10) bulletAdd(g, e->x - 16 * FIX, e->y, -28, vy);
					}
				}
				else {
					e->vx = -16;
				}
				e->x += e->vx;
				break;
			case EN_CAPSULE:
				e->x += e->vx;
				e->y = e->y0 + sinAmp((uint16_t)(e->t * 3), 6 * FIX);
				break;
			default:   // rocks and pebbles drift
				e->x += e->vx;
				e->y += e->vy;
				break;
		}
		// gone off an edge
		if(e->x < -40 * FIX || e->y < (PLAY_TOP - 40) * FIX || e->y > (SCREEN_H + 40) * FIX) {
			e->type = EN_NONE;
		}
	}
}

// -------------------------------------------------------------- the boss

static void bossUpdate(tGame *g) {
	tBoss *b = &g->boss;
	if(b->state == BOSS_OFF || b->state == BOSS_DEAD) return;
	++b->t;
	if(b->flash) --b->flash;
	int16_t cx = b->x - BOSS_CORE_DX * FIX;   // the core
	switch(b->state) {
		case BOSS_ENTER:
			b->x -= FIX;
			if(b->x <= 250 * FIX) {
				b->state = BOSS_FIGHT;
				b->t = 0;
			}
			break;
		case BOSS_FIGHT: {
			b->y = 140 * FIX + sinAmpFine(b->t, 58 * FIX);   // a slow sweep: ~5 s up and down
			uint16_t p = modu(b->t, 240);
			b->pattern = (uint8_t)modu(divu(b->t, 240), 3);
			if(b->pattern == 0 && modu(p, 40) == 0) {
				// a fan from the top or the bottom turret
				int16_t ty = b->y + (divu(b->t, 40) & 1 ? 19 : -20) * FIX;   // the barrels' muzzles
				for(int16_t vy = -16; vy <= 16; vy += 8) bulletAdd(g, b->x - 7 * FIX, ty, -28, vy);
			}
			else if(b->pattern == 1 && modu(p, 60) < 13 && modu(p, 6) == 0) {
				bulletAimed(g, cx - 10 * FIX, b->y, 36);        // aimed bursts from the core
			}
			else if(b->pattern == 2 && modu(p, 50) == 0) {
				for(uint8_t a = 0; a < 64; a += 8) {                // a ring
					bulletAdd(g, cx, b->y, sinAmp(a + 16, 24), sinAmp(a, 24));
				}
			}
			break;
		}
		case BOSS_DYING:
			if(modu(b->t, 8) == 0) {
				// a chain of blasts, mostly small ones (big ones are heavy for the blitter)
				int16_t ox = (int16_t)modu(rnd16(g), 80) - 40, oy = (int16_t)modu(rnd16(g), 50) - 25;
				fxAdd(g, modu(b->t, 24) == 0 ? FX_BOOM : FX_POP, b->x + ox * FIX, b->y + oy * FIX);
				g->evBigKill = 1;
			}
			if(b->t >= 150) {
				b->state = BOSS_DEAD;
				fxAdd(g, FX_BOOM, b->x, b->y);            // the last blast: one big, two small
				fxAdd(g, FX_POP, b->x - 28 * FIX, b->y + 6 * FIX);
				fxAdd(g, FX_POP, b->x + 28 * FIX, b->y - 6 * FIX);
				g->evBigKill = 1;
				addScore(g, (uint16_t)(10000 + mul16(2000, g->lives)));
				g->phase = PHASE_CLEAR;
				g->phaseFrames = 0;
				g->message = MSG_CLEAR;
			}
			break;
	}
}

// ---------------------------------------------------------------- the player

static void playerUpdate(tGame *g, const tInput *in) {
	if(g->dead) {
		if(g->phase == PHASE_PLAY && !--g->dead) {
			if(!g->lives) {
				g->phase = PHASE_OVER;
				g->phaseFrames = 0;
				g->message = MSG_OVER;
				g->dead = 1;         // stays gone
				return;
			}
			g->px = 48 * FIX;
			g->py = 140 * FIX;
			g->invulnerable = INVULNERABLE_FRAMES;
		}
		return;
	}
	if(g->invulnerable) --g->invulnerable;
	g->px += in->dx * PLAYER_SPEED;
	g->py += in->dy * PLAYER_SPEED;
	if(g->px < 18 * FIX) g->px = 18 * FIX;
	if(g->px > 300 * FIX) g->px = 300 * FIX;
	if(g->py < (PLAY_TOP + 9) * FIX) g->py = (PLAY_TOP + 9) * FIX;
	if(g->py > (SCREEN_H - 9) * FIX) g->py = (SCREEN_H - 9) * FIX;
	g->bank = in->dy;
	if(g->fireWait) --g->fireWait;
	if(in->fire && !g->fireWait && g->phase == PHASE_PLAY) {
		g->fireWait = g->weapon == 2 ? FIRE_FRAMES + 2 : FIRE_FRAMES;   // (three at a time: a little slower)
		g->evShot = 1;
		int16_t x = g->px + 12 * FIX;
		static const int16_t pOffs[3][3] = {{0, 0x7FFF, 0}, {-4, 4, 0x7FFF}, {0, -1, 1}};
		for(uint8_t k = 0; k < 3; ++k) {
			int16_t o = pOffs[g->weapon][k];
			if(o == 0x7FFF) break;
			for(uint8_t i = 0; i < SHOTS_MAX; ++i) {
				tShot *s = &g->shots[i];
				if(s->alive) continue;
				if(g->weapon == 2) *s = (tShot){.x = x, .y = g->py, .vx = 10 * FIX, .vy = o * 28, .alive = 1};
				else *s = (tShot){.x = x, .y = g->py + o * FIX, .vx = 10 * FIX, .vy = 0, .alive = 1};
				break;
			}
			if(g->weapon == 0) break;
		}
	}
}

// ------------------------------------------------------------- collisions

static void collide(tGame *g) {
	// The player's shots. First the targets, once a frame: each live enemy's
	// box grown by a shot's half size (7 x 2 px), as edges - then a shot is
	// four compares per target.
	static const uint8_t pHalfW[] = {0, 6, 6, 14, 6, 14, 0}, pHalfH[] = {0, 4, 6, 14, 6, 6, 0};
	static struct { int16_t x0, x1, y0, y1; tEnemy *e; } pTarget[ENEMIES_MAX];
	uint8_t nTargets = 0;
	for(uint8_t k = 0; k < ENEMIES_MAX; ++k) {
		tEnemy *e = &g->enemies[k];
		if(e->type == EN_NONE || e->type == EN_CAPSULE) continue;
		int16_t hw = (int16_t)((pHalfW[e->type] + 7) * FIX), hh = (int16_t)((pHalfH[e->type] + 2) * FIX);
		pTarget[nTargets].x0 = e->x - hw;
		pTarget[nTargets].x1 = e->x + hw;
		pTarget[nTargets].y0 = e->y - hh;
		pTarget[nTargets].y1 = e->y + hh;
		pTarget[nTargets++].e = e;
	}
	uint8_t isBoss = g->boss.state == BOSS_FIGHT || g->boss.state == BOSS_ENTER;
	for(uint8_t i = 0; i < SHOTS_MAX; ++i) {
		tShot *s = &g->shots[i];
		if(!s->alive) continue;
		int16_t sx = s->x, sy = s->y;
		for(uint8_t k = 0; k < nTargets; ++k) {
			if(sx <= pTarget[k].x0 || sx >= pTarget[k].x1 || sy <= pTarget[k].y0 || sy >= pTarget[k].y1) continue;
			tEnemy *e = pTarget[k].e;
			if(e->type == EN_NONE) continue;          // (killed by another shot this frame)
			s->alive = 0;
			g->evHit = 1;
			if(!--e->hp) enemyDies(g, e);
			else e->flash = 4;
			break;
		}
		if(!isBoss) continue;
		tBoss *b = &g->boss;
		if(s->alive) {
			if(boxHit(s->x, s->y, 7 * FIX, 2 * FIX, b->x - (BOSS_CORE_DX + BOSS_CORE_REACH / 2) * FIX, b->y,
			          (BOSS_CORE_R + BOSS_CORE_REACH / 2) * FIX, BOSS_CORE_R * FIX)) {
				s->alive = 0;
				if(b->state == BOSS_FIGHT) {
					g->evBossHit = 1;
					b->flash = 3;
					addScore(g, 10);
					if(!--b->hp) {
						b->state = BOSS_DYING;
						b->t = 0;
						g->evBigKill = 1;
						for(uint8_t k = 0; k < BULLETS_MAX; ++k) g->bullets[k].alive = 0;   // its fire dies with it
					}
				}
			}
			else if(boxHit(s->x, s->y, 7 * FIX, 2 * FIX, b->x, b->y, 44 * FIX, 26 * FIX) &&
			        (s->y - b->y > BOSS_CORE_R * FIX || b->y - s->y > BOSS_CORE_R * FIX)) {
				s->alive = 0;            // the armour stops it (a shot level with the core gets through)
				g->evHit = 1;
			}
		}
	}
	if(g->dead) return;
	// capsules (even while invulnerable, and in the demo)
	for(uint8_t k = 0; k < ENEMIES_MAX; ++k) {
		tEnemy *e = &g->enemies[k];
		if(e->type == EN_CAPSULE && boxHit(e->x, e->y, 8 * FIX, 8 * FIX, g->px, g->py, 8 * FIX, 6 * FIX)) {
			g->evPowerUp = 1;
			if(g->weapon < WEAPON_MAX) ++g->weapon;
			else addScore(g, 1000);
			e->type = EN_NONE;
		}
	}
	if(g->invulnerable || g->isDemo) return;
	// what hits the player
	for(uint8_t i = 0; i < BULLETS_MAX; ++i) {
		tShot *b = &g->bullets[i];
		if(b->alive && boxHit(b->x, b->y, 3 * FIX, 3 * FIX, g->px, g->py, PLAYER_HIT_W / 2 * FIX, PLAYER_HIT_H / 2 * FIX)) {
			b->alive = 0;
			playerDies(g);
			return;
		}
	}
	for(uint8_t k = 0; k < ENEMIES_MAX; ++k) {
		tEnemy *e = &g->enemies[k];
		if(e->type == EN_NONE) continue;
		static const uint8_t pHalf[][2] = {{0, 0}, {6, 4}, {6, 6}, {13, 12}, {6, 6}, {14, 6}, {8, 8}};
		if(e->type == EN_CAPSULE) continue;
		if(boxHit(e->x, e->y, pHalf[e->type][0] * FIX, pHalf[e->type][1] * FIX,
		          g->px, g->py, PLAYER_HIT_W / 2 * FIX, PLAYER_HIT_H / 2 * FIX)) {
			playerDies(g);
			return;
		}
	}
	tBoss *b = &g->boss;
	if((b->state == BOSS_FIGHT || b->state == BOSS_ENTER) &&
	   boxHit(b->x, b->y, 42 * FIX, 22 * FIX, g->px, g->py, PLAYER_HIT_W / 2 * FIX, PLAYER_HIT_H / 2 * FIX)) {
		playerDies(g);
	}
}

// ----------------------------------------------------------------- a frame

void logicAutopilot(const tGame *g, tInput *out) {
	int16_t tx = 56 * FIX, ty = 140 * FIX;
	if(g->boss.state == BOSS_FIGHT || g->boss.state == BOSS_ENTER) {
		tx = 150 * FIX;
		ty = g->boss.y;
	}
	else {
		// a capsule first (a wider gun), else the nearest enemy ahead
		for(uint8_t i = 0; i < ENEMIES_MAX; ++i) {
			const tEnemy *e = &g->enemies[i];
			if(e->type == EN_CAPSULE && e->x > 24 * FIX) {
				out->fire = 1;
				out->dx = (int8_t)(g->px < e->x - 4 * FIX ? 1 : g->px > e->x + 4 * FIX ? -1 : 0);
				out->dy = (int8_t)(g->py < e->y - FIX ? 1 : g->py > e->y + FIX ? -1 : 0);
				return;
			}
		}
		int16_t best = 0x7FFF;
		for(uint8_t i = 0; i < ENEMIES_MAX; ++i) {
			const tEnemy *e = &g->enemies[i];
			if(e->type == EN_NONE || e->x < g->px || e->x > 330 * FIX) continue;
			int16_t d = (int16_t)((e->x - g->px) >> 4);
			if(d < best) {
				best = d;
				ty = e->y;
			}
		}
	}
	out->fire = 1;
	out->dx = (int8_t)(g->px < tx - 2 * FIX ? 1 : g->px > tx + 2 * FIX ? -1 : 0);
	out->dy = (int8_t)(g->py < ty - FIX ? 1 : g->py > ty + FIX ? -1 : 0);
}

void logicUpdate(tGame *g, const tInput *in) {
	++g->frame;
	++g->phaseFrames;
	g->evShot = g->evHit = g->evKill = g->evBigKill = g->evPlayerDied = g->evPowerUp = g->evBossHit = 0;
	g->bgX += 1;                     // the backdrop drifts a quarter pixel a frame
	if(g->phase == PHASE_TITLE) {
		if(in->fire && g->phaseFrames > TITLE_WAIT) logicStart(g);
		else if(g->phaseFrames > DEMO_WAIT) {
			logicStart(g);
			g->isDemo = 1;
		}
		return;
	}
	tInput sAuto;
	if(g->isDemo) {
		if(in->fire) {             // fire ends the demo
			logicInit(g);
			return;
		}
		logicAutopilot(g, &sAuto);
		in = &sAuto;
	}
	if(g->phase == PHASE_OVER || g->phase == PHASE_CLEAR) {
		if(g->phaseFrames >= (g->phase == PHASE_OVER ? OVER_FRAMES : CLEAR_FRAMES)) {
			logicInit(g);
			return;
		}
	}
	if(g->phase == PHASE_PLAY) {
		++g->levelFrame;
		if(g->levelFrame == INTRO_FRAMES && g->message == MSG_STAGE) g->message = MSG_NONE;
		if(g->message == MSG_WARNING && g->boss.state == BOSS_ENTER) g->message = MSG_NONE;
		scriptRun(g);
	}
	playerUpdate(g, in);
	// shots and bullets move
	for(uint8_t i = 0; i < SHOTS_MAX; ++i) {
		tShot *s = &g->shots[i];
		if(!s->alive) continue;
		s->x += s->vx;
		s->y += s->vy;
		if(s->x > (SCREEN_W + 16) * FIX || s->y < PLAY_TOP * FIX || s->y > SCREEN_H * FIX) s->alive = 0;
	}
	for(uint8_t i = 0; i < BULLETS_MAX; ++i) {
		tShot *b = &g->bullets[i];
		if(!b->alive) continue;
		b->x += b->vx;
		b->y += b->vy;
		if(b->x < -8 * FIX || b->x > (SCREEN_W + 8) * FIX || b->y < PLAY_TOP * FIX || b->y > SCREEN_H * FIX) b->alive = 0;
	}
	enemiesUpdate(g);
	bossUpdate(g);
	for(uint8_t i = 0; i < FX_MAX; ++i) {
		tFx *f = &g->fx[i];
		if(f->type && ++f->t >= (f->type == FX_BOOM ? 30 : 20)) f->type = 0;
	}
	if(g->phase == PHASE_PLAY) collide(g);
}

// --------------------------------------------------------------- drawing

uint8_t logicShipFrame(const tGame *g) {
	if(g->bank < 0) return 2;
	if(g->bank > 0) return 3;
	return (g->frame >> 1) & 1;
}

uint8_t logicShipVisible(const tGame *g) {
	if(g->dead || g->phase == PHASE_TITLE) return 0;
	return !g->invulnerable || ((g->frame >> 2) & 1);
}

static inline void put(tDraw **pp, uint8_t *n, int16_t cx, int16_t cy, uint8_t bob, uint8_t frame) {
	if(*n >= DRAW_MAX) return;
	tDraw *d = (*pp)++;
	d->x = (int16_t)((cx >> 4) - g_pBobW[bob] / 2);
	d->y = (int16_t)((cy >> 4) - g_pBobH[bob] / 2);
	d->bob = bob;
	d->frame = frame;
	++*n;
}

uint8_t logicDraw(const tGame *g, tDraw *pOut) {
	uint8_t n = 0;
	tDraw *p = pOut;
	// rocks behind everything
	for(uint8_t i = 0; i < ENEMIES_MAX; ++i) {
		const tEnemy *e = &g->enemies[i];
		if((e->type != EN_ROCK && e->type != EN_PEBBLE) || (e->flash & 1)) continue;
		put(&p, &n, e->x, e->y, e->type == EN_ROCK ? BOB_ROCK : BOB_PEBBLE, (uint8_t)((e->t >> 3) & 3));
	}
	if(g->boss.state != BOSS_OFF && g->boss.state != BOSS_DEAD) {
		const tBoss *b = &g->boss;
		int16_t shake = b->state == BOSS_DYING ? (int16_t)((b->t & 2) ? FIX : -FIX) : 0;
		put(&p, &n, b->x + shake, b->y, BOB_BOSS, b->flash ? 1 : 0);
	}
	for(uint8_t i = 0; i < ENEMIES_MAX; ++i) {
		const tEnemy *e = &g->enemies[i];
		if(e->flash & 1) continue;
		switch(e->type) {
			case EN_DART: put(&p, &n, e->x, e->y, BOB_DART, (uint8_t)((e->t >> 2) & 1)); break;
			case EN_MINE: put(&p, &n, e->x, e->y, BOB_MINE, (uint8_t)((e->t >> 2) & 3)); break;
			case EN_GUNSHIP: put(&p, &n, e->x, e->y, BOB_GUNSHIP, (uint8_t)((e->t >> 4) & 1)); break;
			case EN_CAPSULE: put(&p, &n, e->x, e->y, BOB_CAPSULE, (uint8_t)((e->t >> 3) & 1)); break;
		}
	}
	for(uint8_t i = 0; i < SHOTS_MAX; ++i) {
		if(g->shots[i].alive) put(&p, &n, g->shots[i].x, g->shots[i].y, BOB_LASER, 0);
	}
	for(uint8_t i = 0; i < BULLETS_MAX; ++i) {
		if(g->bullets[i].alive) put(&p, &n, g->bullets[i].x, g->bullets[i].y, BOB_ORB, (uint8_t)((g->frame >> 2) & 1));
	}
	for(uint8_t i = 0; i < FX_MAX; ++i) {
		const tFx *f = &g->fx[i];
		if(f->type == FX_BOOM) put(&p, &n, f->x, f->y, BOB_BOOM, (uint8_t)divu(f->t, 5));
		else if(f->type == FX_POP) put(&p, &n, f->x, f->y, BOB_POP, (uint8_t)(f->t >> 2));
	}
	return n;
}
