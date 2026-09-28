#ifndef IRONWRAITH_LOGIC_H
#define IRONWRAITH_LOGIC_H
#include <stdint.h>
#define ENEMIES 4
#define SHOTS 6
#define BOLTS 8
#define BLASTS 4
enum { TITLE, PLAY, WON, LOST };
enum { BOSS_WALK, BOSS_CHARGE, BOSS_FIRE, BOSS_RECOVER, BOSS_DYING };
enum {
  EV_SHOT = 1,
  EV_BLAST = 2,
  EV_HIT = 4,
  EV_EMP = 8,
  EV_BOSS = 16,
  EV_WIN = 32
};
typedef struct {
  int8_t dx, dy;
  uint8_t fire, emp, pause;
} Input;
typedef struct {
  int16_t x, y;
  int8_t dx, dy;
  uint8_t active, hp, timer;
} Entity;
typedef struct {
  uint16_t frame, tick, score, best, empCooldown, bossHp;
  int16_t x, y, bossX, bossY;
  uint8_t phase, wave, health, invincible, fireDelay, events, flash, kills,
      paused;
  uint8_t prevFire, prevEmp, prevPause, bossActive;
  uint8_t bossState, bossTimer, bossHurt;
  int8_t bossDirection;
  Entity enemies[ENEMIES], shots[SHOTS], bolts[BOLTS], blasts[BLASTS], repair;
} Game;
uint8_t logicBossFrame(const Game *g);
void logicInit(Game *g);
void logicUpdate(Game *g, const Input *in);
#endif
