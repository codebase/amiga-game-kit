#include "logic.h"
#include <string.h>
static int16_t clamp(int16_t v, int16_t lo, int16_t hi) {
  return v < lo ? lo : v > hi ? hi : v;
}
static uint8_t overlaps(int16_t x, int16_t y, int16_t w, int16_t h, int16_t xx,
                        int16_t yy, int16_t ww, int16_t hh) {
  return x < xx + ww && x + w > xx && y < yy + hh && y + h > yy;
}
static void blast(Game *g, int16_t x, int16_t y) {
  uint8_t n = 0;
  for (uint8_t i = 0; i < BLASTS; i++)
    if (!g->blasts[i].active) {
      n = i;
      break;
    }
  g->blasts[n] = (Entity){
      .x = clamp(x, 0, 288), .y = clamp(y, 32, 204), .active = 1, .timer = 18};
  g->events |= EV_BLAST;
}
static void kill(Game *g, Entity *e) {
  e->active = 0;
  g->score += 100;
  g->kills++;
  blast(g, e->x, e->y);
  if (!(g->kills & 3))
    g->repair = (Entity){.x = e->x, .y = e->y, .active = 1};
}
static void bolt(Game *g, int16_t x, int16_t y, int8_t dy) {
  for (uint8_t i = 0; i < BOLTS; i++)
    if (!g->bolts[i].active) {
      g->bolts[i] = (Entity){.x = x, .y = y, .dx = -3, .dy = dy, .active = 1};
      break;
    }
}
uint8_t logicBossFrame(const Game *g) {
  if (g->bossState == BOSS_DYING)
    return 12 + (g->bossTimer < 48 ? (g->bossTimer >> 3) : 5);
  if (g->bossHurt)
    return 11;
  switch (g->bossState) {
  case BOSS_CHARGE:
    return 4 + (g->bossTimer >> 3);
  case BOSS_FIRE:
    return 6 + g->bossTimer / 6;
  case BOSS_RECOVER:
    return 10 + (g->bossTimer >> 3);
  default:
    return (g->bossTimer >> 3) & 3;
  }
}
static void bossUpdate(Game *g) {
  if (g->bossHurt)
    --g->bossHurt;
  switch (g->bossState) {
  case BOSS_WALK:
    if (!(g->bossTimer & 3)) {
      g->bossX += g->bossDirection;
      if (g->bossX <= 208 || g->bossX >= 236)
        g->bossDirection = -g->bossDirection;
    }
    if (++g->bossTimer == 48) {
      g->bossState = BOSS_CHARGE;
      g->bossTimer = 0;
    }
    break;
  case BOSS_CHARGE:
    if (++g->bossTimer == 16) {
      g->bossState = BOSS_FIRE;
      g->bossTimer = 0;
    }
    break;
  case BOSS_FIRE:
    if (g->bossTimer == 0 || g->bossTimer == 12) {
      int16_t muzzle = g->bossY + 38;
      int8_t aim = g->y + 24 < muzzle - 12   ? -1
                   : g->y + 24 > muzzle + 12 ? 1
                                             : 0;
      bolt(g, g->bossX - 6, muzzle, aim);
      bolt(g, g->bossX - 4, muzzle - 7, aim - 1);
      bolt(g, g->bossX - 4, muzzle + 7, aim + 1);
    }
    if (++g->bossTimer == 24) {
      g->bossState = BOSS_RECOVER;
      g->bossTimer = 0;
    }
    break;
  case BOSS_RECOVER:
    if (++g->bossTimer == 16) {
      g->bossState = BOSS_WALK;
      g->bossTimer = 0;
    }
    break;
  }
}
static void bossDeath(Game *g) {
  g->bossState = BOSS_DYING;
  g->bossTimer = g->bossHurt = 0;
  g->invincible = 60;
  memset(g->bolts, 0, sizeof(g->bolts));
  memset(g->shots, 0, sizeof(g->shots));
  memset(g->blasts, 0, sizeof(g->blasts));
  g->events |= EV_BLAST;
}
static void hurt(Game *g) {
  if (g->invincible || (g->bossActive && !g->bossHp))
    return;
  g->health--;
  g->invincible = 65;
  g->events |= EV_HIT;
  blast(g, g->x + 8, g->y + 8);
  if (!g->health)
    g->phase = LOST;
}
void logicInit(Game *g) {
  memset(g, 0, sizeof(*g));
  g->x = 40;
  g->y = 130;
  g->health = 6;
}
void logicUpdate(Game *g, const Input *in) {
  uint8_t fireEdge = in->fire && !g->prevFire, empEdge = in->emp && !g->prevEmp,
          pauseEdge = in->pause && !g->prevPause;
  g->prevFire = in->fire;
  g->prevEmp = in->emp;
  g->prevPause = in->pause;
  g->frame++;
  g->events = 0;
  if (g->phase != PLAY) {
    if (fireEdge) {
      uint16_t best = g->best, frame = g->frame;
      logicInit(g);
      g->best = best;
      g->frame = frame;
      g->phase = PLAY;
      g->wave = 1;
      g->prevFire = 1;
    }
    return;
  }
  if (pauseEdge)
    g->paused = !g->paused;
  if (g->paused)
    return;
  g->tick++;
  g->wave = g->tick < 450 ? 1 : g->tick < 900 ? 2 : 3;
  if (g->invincible)
    g->invincible--;
  if (g->empCooldown)
    g->empCooldown--;
  if (g->flash)
    g->flash--;
  if (g->fireDelay)
    g->fireDelay--;
  g->x = clamp(g->x + in->dx * 2, 8, 194);
  g->y = clamp(g->y + in->dy * 2, 34, 164);
  for (uint8_t i = 0; i < BLASTS; i++)
    if (g->blasts[i].active && !--g->blasts[i].timer)
      g->blasts[i].active = 0;
  if (g->bossState == BOSS_DYING) {
    if (++g->bossTimer == 48) {
      g->bossActive = 0;
      g->phase = WON;
      g->score += 2500;
      if (g->score > g->best)
        g->best = g->score;
      g->events |= EV_WIN;
    } else if (g->bossTimer == 16 || g->bossTimer == 32) {
      blast(g, g->bossX + 24, g->bossY + 24);
    }
    return;
  }
  if (in->fire && !g->fireDelay) {
    for (uint8_t i = 0; i < SHOTS; i++)
      if (!g->shots[i].active) {
        g->shots[i] = (Entity){.x = g->x + 44, .y = g->y + 23, .active = 1};
        g->fireDelay = 7;
        g->events |= EV_SHOT;
        break;
      }
  }
  if (g->tick == 900) {
    g->bossActive = 1;
    g->bossHp = 100;
    g->bossX = 236;
    g->bossY = 128;
    g->bossDirection = -1;
    memset(g->enemies, 0, sizeof(g->enemies));
    memset(g->bolts, 0, sizeof(g->bolts));
    g->events |= EV_BOSS;
    g->health = clamp(g->health + 2, 0, 6);
  }
  if (g->bossActive)
    bossUpdate(g);
  if (empEdge && !g->empCooldown) {
    g->empCooldown = 300;
    g->flash = 12;
    g->invincible = 35;
    g->events |= EV_EMP;
    for (uint8_t i = 0; i < ENEMIES; i++)
      if (g->enemies[i].active)
        kill(g, &g->enemies[i]);
    for (uint8_t i = 0; i < BOLTS; i++)
      g->bolts[i].active = 0;
    if (g->bossActive) {
      g->bossHp = g->bossHp > 20 ? g->bossHp - 20 : 0;
      g->bossHurt = 6;
    }
  }
  if (g->wave < 3 && g->tick % (g->wave == 1 ? 85 : 65) == 20) {
    static const uint8_t lanes[] = {130, 64, 168, 100, 146, 80};
    for (uint8_t i = 0; i < ENEMIES; i++)
      if (!g->enemies[i].active) {
        g->enemies[i] = (Entity){.x = 284,
                                 .y = lanes[(g->tick / 65) % 6],
                                 .active = 1,
                                 .hp = g->wave == 1 ? 3 : 4,
                                 .timer = 65};
        break;
      }
  }
  for (uint8_t i = 0; i < ENEMIES; i++)
    if (g->enemies[i].active) {
      Entity *e = &g->enemies[i];
      if (!(g->tick & 1))
        e->x--;
      if (e->x < 4) {
        e->active = 0;
        continue;
      }
      if (e->timer)
        e->timer--;
      else {
        bolt(g, e->x - 4, e->y + 14,
             g->y + 24 < e->y   ? -1
             : g->y > e->y + 28 ? 1
                                : 0);
        e->timer = 90;
      }
      if (overlaps(g->x + 8, g->y + 6, 28, 35, e->x, e->y, 30, 26))
        hurt(g);
    }
  for (uint8_t i = 0; i < SHOTS; i++)
    if (g->shots[i].active) {
      Entity *s = &g->shots[i];
      s->x += 6;
      if (s->x > 300) {
        s->active = 0;
        continue;
      }
      for (uint8_t j = 0; j < ENEMIES; j++) {
        Entity *e = &g->enemies[j];
        if (s->active && e->active &&
            overlaps(s->x, s->y, 16, 8, e->x, e->y, 32, 28)) {
          s->active = 0;
          if (!--e->hp)
            kill(g, e);
        }
      }
      if (s->active && g->bossActive &&
          overlaps(s->x, s->y, 16, 8, g->bossX, g->bossY + 10, 68, 65)) {
        s->active = 0;
        if (g->bossHp)
          g->bossHp--;
        if (!(g->bossHp & 7)) {
          g->bossHurt = 6;
          blast(g, g->bossX + 20, g->bossY + 20);
        }
      }
    }
  if (g->bossActive && !g->bossHp) {
    bossDeath(g);
    return;
  }
  for (uint8_t i = 0; i < BOLTS; i++)
    if (g->bolts[i].active) {
      Entity *e = &g->bolts[i];
      e->x += e->dx;
      e->y += e->dy;
      if (e->x < 2 || e->y < 30 || e->y > 213) {
        e->active = 0;
        continue;
      }
      if (overlaps(g->x + 8, g->y + 6, 28, 35, e->x, e->y, 12, 6)) {
        e->active = 0;
        hurt(g);
      }
    }
  if (g->bossActive &&
      overlaps(g->x + 8, g->y + 6, 28, 35, g->bossX + 11, g->bossY + 8, 60, 70))
    hurt(g);
  if (g->repair.active) {
    Entity *r = &g->repair;
    if (!(g->tick & 1))
      r->x--;
    if (r->x < 4)
      r->active = 0;
    if (overlaps(g->x, g->y, 44, 45, r->x, r->y, 16, 16)) {
      r->active = 0;
      g->health = clamp(g->health + 2, 0, 6);
      g->score += 50;
    }
  }
  if (g->score > g->best)
    g->best = g->score;
}
