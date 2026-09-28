// VOIDRUNNER's rules: pure C, no Amiga headers, so `agk unit` tests them on
// the host. main.c draws what logicDraw() lists and plays what happened.
//
// Level 1, "Outer Belt": a scripted run of enemy waves through an asteroid
// belt, then the Warden. Coordinates are screen pixels (0..319 across,
// PLAY_TOP..255 down) in 1/16 px (FIX), positions are centres.
#ifndef _LOGIC_H_
#define _LOGIC_H_

#include <stdint.h>

#define SCREEN_W 320
#define SCREEN_H 256
#define HUD_H 24                  // the top lines: score, lives
#define PLAY_TOP HUD_H
#define FIX 16                    // 1/16 px
#define FPS 50

// ------------------------------------------------------------ the player
#define PLAYER_W 32
#define PLAYER_H 16
#define PLAYER_SPEED (2 * FIX + 8)     // 2.5 px per frame
#define PLAYER_HIT_W 10                // the hit box: the cockpit, not the wings
#define PLAYER_HIT_H 4
#define LIVES 3
#define RESPAWN_FRAMES (2 * FPS)
#define INVULNERABLE_FRAMES (2 * FPS)
#define WEAPON_MAX 2                   // 0 single, 1 double, 2 spread
#define FIRE_FRAMES 6                  // between shots, holding fire

typedef struct {
	int8_t dx, dy;                     // -1, 0, +1
	uint8_t fire;
} tInput;

// ----------------------------------------------------------- the objects
#define SHOTS_MAX 14
#define BULLETS_MAX 24
#define ENEMIES_MAX 14
#define FX_MAX 12

// Enemy types
#define EN_NONE 0
#define EN_DART 1                 // formations of 5 on a wave; all 5 -> a capsule
#define EN_MINE 2                 // stops, fires at you, leaves
#define EN_ROCK 3                 // big asteroid: breaks into two pebbles
#define EN_PEBBLE 4
#define EN_GUNSHIP 5              // hovers, fires three ways
#define EN_CAPSULE 6              // not an enemy: the weapon power-up

typedef struct {
	uint8_t type, hp, flash;       // flash: frames of the hit blink
	uint8_t group;                 // a dart formation's id (for the capsule)
	int16_t x, y, vx, vy;          // FIX
	int16_t y0;                    // the path's centre line
	uint16_t t;                    // frames alive
} tEnemy;

typedef struct {
	int16_t x, y, vx, vy;
	uint8_t alive;
} tShot;

#define FX_BOOM 1                 // 32x32 explosion
#define FX_POP 2                  // 16x16
typedef struct {
	uint8_t type, t;
	int16_t x, y;                  // FIX
} tFx;

// -------------------------------------------------------------- the boss
#define BOSS_W 96
#define BOSS_H 46
#define BOSS_HP 90
#define BOSS_CORE_DX (-13)        // the core's centre left of the boss's centre, px (it's right of it)
#define BOSS_CORE_R 8
#define BOSS_CORE_REACH 20        // shots hit the core in the channel in front of it, this far out: where the old core was (fewer shots in flight)
typedef struct {
	uint8_t state;                 // BOSS_*
	uint8_t flash;
	uint16_t hp, t;
	int16_t x, y;                  // FIX, the centre
	uint8_t pattern;
} tBoss;
#define BOSS_OFF 0
#define BOSS_ENTER 1
#define BOSS_FIGHT 2
#define BOSS_DYING 3
#define BOSS_DEAD 4

// ------------------------------------------------------------ the game
#define PHASE_TITLE 0
#define PHASE_PLAY 1
#define PHASE_CLEAR 2             // the Warden is dead: bonus, then the title
#define PHASE_OVER 3              // no lives left
#define TITLE_WAIT FPS            // fire held from the game is ignored this long
#define DEMO_WAIT (10 * FPS)      // on the title this long: the game plays itself
#define CLEAR_FRAMES (7 * FPS)
#define OVER_FRAMES (5 * FPS)
#define INTRO_FRAMES (3 * FPS)    // "STAGE 1 - OUTER BELT"

// Messages in the middle of the screen (MSG_*): what main.c shows
#define MSG_NONE 0
#define MSG_STAGE 1
#define MSG_WARNING 2
#define MSG_CLEAR 3
#define MSG_OVER 4
#define MSG_TITLE 5
#define MSG_DEMO 6                // (with the level's own messages: "DEMO" stays in the corner)

typedef struct {
	uint8_t phase;
	uint16_t phaseFrames;
	uint16_t frame;
	uint32_t levelFrame;           // frames into the level (the script's clock)
	uint16_t script;               // the next script entry
	uint32_t rnd;
	// the player
	int16_t px, py;                // FIX, the centre
	int8_t bank;                   // -1 climbing .. +1 diving (the ship's frame)
	uint8_t lives, weapon, fireWait;
	uint8_t dead;                  // frames until respawn (0: alive)
	uint8_t invulnerable;          // frames left
	uint32_t score, hiScore;
	uint8_t groupKills[8], groupSize[8];
	// things
	tShot shots[SHOTS_MAX];
	tShot bullets[BULLETS_MAX];
	tEnemy enemies[ENEMIES_MAX];
	tFx fx[FX_MAX];
	tBoss boss;
	// what happened this frame (for the sound)
	uint8_t evShot, evHit, evKill, evBigKill, evPlayerDied, evPowerUp, evBossHit;
	uint16_t kills;
	uint8_t message;
	uint8_t isDemo;                // played by logicAutopilot(); the ship can't be hit
	uint16_t bgX;                  // the backdrop's scroll, 1/4 px
} tGame;

void logicInit(tGame *g);                      // the title
void logicStart(tGame *g);                     // a new game, level 1
void logicUpdate(tGame *g, const tInput *in);  // one frame (1/50 s)
/** The demo's pilot: hunts the nearest enemy's height, fires all the time,
 *  and in the boss fight faces the Warden's core. */
void logicAutopilot(const tGame *g, tInput *out);

// What to draw, back to front: rocks, enemies, the boss, shots, bullets,
// explosions. x, y: the top left corner in screen px.
#define DRAW_MAX 64
#define BOB_DART 0
#define BOB_GUNSHIP 1
#define BOB_LASER 2
#define BOB_ORB 3
#define BOB_CAPSULE 4
#define BOB_ROCK 5
#define BOB_PEBBLE 6
#define BOB_MINE 7
#define BOB_BOOM 8
#define BOB_POP 9
#define BOB_BOSS 10
#define BOB_TYPES 11
typedef struct {
	int16_t x, y;
	uint8_t bob, frame;
} tDraw;
uint8_t logicDraw(const tGame *g, tDraw *pOut);
// The size of each BOB type (px), shared with main.c's art
extern const uint8_t g_pBobW[BOB_TYPES], g_pBobH[BOB_TYPES], g_pBobFrames[BOB_TYPES];

/** The ship's sprite frame (0-1 cruising, 2 climbing, 3 diving) and
 *  whether it's shown (it blinks while invulnerable, not while dead). */
uint8_t logicShipFrame(const tGame *g);
uint8_t logicShipVisible(const tGame *g);

#endif
