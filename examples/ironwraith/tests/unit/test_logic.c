#include "logic.h"
#include <stdio.h>
#include <string.h>
static int failures;
#define CHECK(x)                                                               \
  do {                                                                         \
    if (!(x)) {                                                                \
      printf("FAIL line %d: %s\n", __LINE__, #x);                              \
      ++failures;                                                              \
    }                                                                          \
  } while (0)
static Input idle = {0};
static void start(Game *g) {
  logicInit(g);
  logicUpdate(g, &(Input){.fire = 1});
  logicUpdate(g, &idle);
}
int main(void) {
  Game g;
  start(&g);
  CHECK(g.phase == PLAY && g.health == 6);
  for (int i = 0; i < 200; i++)
    logicUpdate(&g, &(Input){.dx = -1, .dy = -1});
  CHECK(g.x == 8 && g.y == 34);
  start(&g);
  g.enemies[0] = (Entity){.x = 130, .y = 130, .active = 1, .hp = 3};
  g.y = 130;
  for (int i = 0; i < 50; i++)
    logicUpdate(&g, &(Input){.fire = 1});
  CHECK(g.score >= 100 && g.kills >= 1);
  start(&g);
  g.enemies[0] = (Entity){.x = 180, .y = 130, .active = 1, .hp = 5};
  g.bolts[0] = (Entity){.x = 180, .y = 130, .active = 1};
  logicUpdate(&g, &(Input){.emp = 1});
  CHECK(!g.enemies[0].active && !g.bolts[0].active && g.empCooldown == 300);
  for (int i = 0; i < 10; i++)
    logicUpdate(&g, &(Input){.emp = 1});
  CHECK(g.empCooldown == 290);
  start(&g);
  g.bolts[0] = (Entity){.x = g.x + 15, .y = g.y + 15, .active = 1};
  logicUpdate(&g, &idle);
  CHECK(g.health == 5 && g.invincible);
  g.bolts[1] = (Entity){.x = g.x + 15, .y = g.y + 15, .active = 1};
  logicUpdate(&g, &idle);
  CHECK(g.health == 5);
  start(&g);
  logicUpdate(&g, &(Input){.pause = 1});
  uint16_t t = g.tick;
  for (int i = 0; i < 20; i++)
    logicUpdate(&g, &idle);
  CHECK(g.tick == t && g.paused);
  logicUpdate(&g, &(Input){.pause = 1});
  CHECK(!g.paused && g.tick == t + 1);
  start(&g);
  g.tick = 899;
  logicUpdate(&g, &idle);
  CHECK(g.bossActive && g.bossHp == 100);
  CHECK(g.bossY == 128 && g.bossState == BOSS_WALK);
  int bx = g.bossX;
  uint8_t firstPose = logicBossFrame(&g);
  for (int i = 0; i < 12; i++)
    logicUpdate(&g, &idle);
  CHECK(g.bossY == 128 && g.bossX < bx);
  CHECK(logicBossFrame(&g) != firstPose);
  for (int i = 0; i < 35; i++)
    logicUpdate(&g, &idle);
  CHECK(g.bossState == BOSS_CHARGE);
  for (int i = 0; i < BOLTS; i++)
    CHECK(!g.bolts[i].active);
  logicUpdate(&g, &(Input){.pause = 1});
  uint8_t pose = logicBossFrame(&g), bt = g.bossTimer;
  for (int i = 0; i < 20; i++)
    logicUpdate(&g, &idle);
  CHECK(g.bossTimer == bt && logicBossFrame(&g) == pose);
  logicUpdate(&g, &(Input){.pause = 1});
  for (int i = 0; i < 16; i++)
    logicUpdate(&g, &idle);
  CHECK(g.bossState == BOSS_FIRE && g.bolts[0].active);
  CHECK(g.bossY == 128);
  g.bossHp = 10;
  logicUpdate(&g, &(Input){.emp = 1});
  CHECK(g.phase == PLAY && g.bossState == BOSS_DYING);
  CHECK(logicBossFrame(&g) == 12);
  for (int i = 0; i < BOLTS; i++)
    CHECK(!g.bolts[i].active);
  for (int i = 0; i < 24; i++)
    logicUpdate(&g, &idle);
  CHECK(g.phase == PLAY && logicBossFrame(&g) == 15);
  for (int i = 0; i < 24; i++)
    logicUpdate(&g, &idle);
  CHECK(g.phase == WON && !g.bossActive && g.best >= 2500);
  logicUpdate(&g, &idle);
  logicUpdate(&g, &(Input){.fire = 1});
  CHECK(g.phase == PLAY && g.health == 6 && g.best >= 2500);
  start(&g);
  g.health = 1;
  g.bolts[0] = (Entity){.x = g.x + 15, .y = g.y + 15, .active = 1};
  logicUpdate(&g, &idle);
  CHECK(g.phase == LOST);
  start(&g);
  Game copy = g;
  for (int i = 0; i < 1500; i++) {
    Input in = {
        .dy = (i % 100 < 50) ? 1 : -1, .fire = 1, .emp = (i % 310 == 0)};
    logicUpdate(&g, &in);
    logicUpdate(&copy, &in);
  }
  CHECK(!memcmp(&g, &copy, sizeof g));
  if (failures)
    return 1;
  puts("Combat, damage, EMP, pause, boss, restart and determinism passed");
  return 0;
}
