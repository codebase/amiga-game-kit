/* IRON WRAITH -- AGA eight-plane mech assault. */
#include "art.h"
#include "font.h"
#include "logic.h"
#include "sound.h"
#include <ace/generic/main.h>
#include <ace/managers/blit.h>
#include <ace/managers/bob.h>
#include <ace/managers/joy.h>
#include <ace/managers/key.h>
#include <ace/managers/sprite.h>
#include <ace/managers/timer.h>
#include <ace/managers/viewport/simplebuffer.h>
#include <ace/utils/custom.h>
#include <ace/utils/extview.h>
#include <agk/debug.h>
#include <agk/perf.h>
#include <hardware/dmabits.h>
#define TYPES 7
static tView *view;
static tVPort *port;
static tSimpleBufferManager *buffer;
static tBitMap *background, *pages[4], *art[TYPES], *mask[TYPES], *bufferA;
static UBYTE *frames[TYPES][18], *masks[TYPES][18];
static const UBYTE heights[TYPES] = {48, 28, 80, 32, 8, 8, 16};
static const UBYTE widths[TYPES] = {48, 32, 80, 32, 16, 16, 16};
static const UBYTE counts[TYPES] = {4, 4, 18, 6, 2, 2, 1};
static tSprite *mech[6];
static tBitMap *mechFrames[2][4][6];
static tBob boss, enemies[ENEMIES], shots[SHOTS], bolts[BOLTS], blasts[BLASTS],
    repair;
static Game game;
static UBYTE shown[2] = {255, 255}, musicOn = 1;
static char cached[2][3][40];
static const UBYTE textY[3] = {5, 18, 244};

static void pixel(tBitMap *bm, UWORD x, UWORD y, UBYTE color) {
  UBYTE bit = 0x80 >> (x & 7);
  ULONG row = (ULONG)y * bm->BytesPerRow + (x >> 3);
  for (UBYTE p = 0; p < 8; p++) {
    UBYTE *v = bm->Planes[p] + row;
    if (color & (1 << p))
      *v |= bit;
    else
      *v &= ~bit;
  }
}
static void label(tBitMap *bm, const char *str, UWORD x, UWORD y, UBYTE scale,
                  UBYTE color) {
  blitWait();
  while (*str) {
    const UBYTE *glyph = glyphs[(UBYTE)*str++];
    for (UBYTE yy = 0; yy < 7; yy++)
      for (UBYTE xx = 0; xx < 6; xx++)
        if (glyph[yy] & (1 << (6 - xx)))
          for (UBYTE sy = 0; sy < scale; sy++)
            for (UBYTE sx = 0; sx < scale; sx++)
              pixel(bm, x + xx * scale + sx, y + yy * scale + sy, color);
    x += 6 * scale;
  }
}
static void textRow(UBYTE b, UBYTE row, const char *s) {
  tBitMap *bm = buffer->pBack;
  blitWait();
  for (UBYTE x = 0; x < 40; x++) {
    char c = *s ? *s++ : ' ';
    if (cached[b][row][x] == c)
      continue;
    cached[b][row][x] = c;
    for (UBYTE p = 0; p < 6; p++) {
      UBYTE *dst = bm->Planes[p] + textY[row] * 320 + x;
      const UBYTE *g = glyphs[(UBYTE)c];
      dst[0] = g[0];
      dst[320] = g[1];
      dst[640] = g[2];
      dst[960] = g[3];
      dst[1280] = g[4];
      dst[1600] = g[5];
      dst[1920] = g[6];
    }
  }
}
static void number(char *s, UWORD n, UBYTE digits) {
  while (digits) {
    s[--digits] = '0' + n % 10;
    n /= 10;
  }
}
static void hud(UBYTE b) {
  char top[] = " IRON WRAITH  ARMOR 6  SCORE 00000       ";
  char bottom[] = " SECTOR 1/3   EMP READY    P PAUSE       ";
  number(top + 29, game.score, 5);
  top[20] = '0' + game.health;
  bottom[8] = '0' + game.wave;
  if (game.empCooldown) {
    const char *s = "CHARGE";
    for (UBYTE i = 0; i < 6; i++)
      bottom[18 + i] = s[i];
  }
  if (game.paused)
    textRow(b, 0, " PAUSED - PRESS P TO RESUME              ");
  else
    textRow(b, 0, top);
  textRow(b, 1,
          game.phase == PLAY
              ? (game.bossActive ? " MOLOCH // CORE                         "
                                 : " BLACK SITE 07 // BREAK THE BLOCKADE     ")
              : " A1200 / AGA        FURNACE PROTOCOL     ");
  if (game.phase == PLAY)
    textRow(b, 2, bottom);
  else
    textRow(b, 2, " ARROWS MOVE  SPACE FIRE  X EMP  M MUSIC ");
}
static void initBob(tBob *b, UBYTE type) {
  bobInit(b, widths[type], heights[type], 1, frames[type][0], masks[type][0], 0,
          0);
}
static void push(tBob *b, UBYTE type, UBYTE frame, WORD x, WORD y) {
  bobSetFrame(b, frames[type][frame], masks[type][frame]);
  b->sPos.uwX = x;
  b->sPos.uwY = y;
  bobPush(b);
}
static const char *rowOne(UBYTE phase) {
  return phase == PLAY ? " BLACK SITE 07 // BREAK THE BLOCKADE     "
                       : " A1200 / AGA        FURNACE PROTOCOL     ";
}
static const char *rowTwo(UBYTE phase) {
  return phase == PLAY ? " SECTOR 1/3   EMP READY    P PAUSE       "
                       : " ARROWS MOVE  SPACE FIRE  X EMP  M MUSIC ";
}
static void bakeRow(tBitMap *bm, UBYTE row, const char *s) {
  blitWait();
  for (UBYTE x = 0; x < 40; x++) {
    UBYTE c = *s ? *s++ : ' ';
    for (UBYTE p = 0; p < 6; p++) {
      UBYTE *dst = bm->Planes[p] + textY[row] * 320 + x;
      for (UBYTE y = 0; y < 7; y++)
        dst[y * 320] = glyphs[c][y];
    }
  }
}
static void makePages(void) {
  for (UBYTE phase = 0; phase < 4; phase++) {
    pages[phase] = bitmapCreate(320, 256, 8, BMF_CLEAR | BMF_INTERLEAVED);
    tBitMap *bm = pages[phase];
    blitCopyAligned(background, 0, 0, bm, 0, 0, 320, 256);
    blitWait();
    blitRect(bm, 0, 0, 320, 30, 192);
    blitRect(bm, 0, 239, 320, 17, 192);
    if (phase != PLAY) {
      blitRect(bm, 32, 42, 256, 60, 192);
      blitRect(bm, 32, 42, 256, 1, 218);
      blitRect(bm, 32, 101, 256, 1, 195);
      label(bm, "IRON WRAITH", 62, 52, 3, 193);
      label(bm, "IRON WRAITH", 60, 50, 3, 251);
      label(bm, "STEEL. FIRE. NO MERCY.", 94, 79, 1, 199);
      label(bm,
            phase == TITLE ? "FURNACE PROTOCOL"
            : phase == WON ? "REACTOR DESTROYED"
                           : "ARMOR BREACHED",
            112, 93, 1, 223);
      blitRect(bm, 48, 212, 224, 20, 192);
      label(bm,
            phase == TITLE ? "PRESS FIRE TO DEPLOY" : "PRESS FIRE TO REDEPLOY",
            94, 219, 1, 255);
    }
    blitWait();
    bakeRow(bm, 1, rowOne(phase));
    bakeRow(bm, 2, rowTwo(phase));
  }
}
static void scene(UBYTE b) {
  // Only these two panels differ between screens. BOBs have already restored
  // their own background, and the industrial panorama never needs repainting.
  blitCopyAligned(pages[game.phase], 32, 42, buffer->pBack, 32, 42, 256, 60);
  blitCopyAligned(pages[game.phase], 48, 212, buffer->pBack, 48, 212, 224, 20);
  for (UBYTE row = 1; row < 3; row++) {
    blitCopyAligned(pages[game.phase], 0, textY[row], buffer->pBack, 0,
                    textY[row], 320, 7);
    const char *s = row == 1 ? rowOne(game.phase) : rowTwo(game.phase);
    for (UBYTE x = 0; x < 40; x++)
      cached[b][row][x] = *s ? *s++ : ' ';
  }
  shown[b] = game.phase;
}
static void render(void) {
  UBYTE b = buffer->pBack == bufferA ? 0 : 1;
  systemSetDmaBit(DMAB_BLITHOG, 1);
  bobBegin(buffer->pBack);
  UBYTE transition = shown[b] != game.phase;
  if (transition)
    scene(b);
  if (!transition && game.phase == PLAY) {
    for (UBYTE i = 0; i < ENEMIES; i++)
      if (game.enemies[i].active)
        push(&enemies[i], 1, (game.tick >> 3) & 3, game.enemies[i].x,
             game.enemies[i].y);
    if (game.bossActive)
      push(&boss, 2, logicBossFrame(&game), game.bossX, game.bossY);
    if (game.repair.active)
      push(&repair, 6, 0, game.repair.x, game.repair.y);
    // Keep armor visible even during damage invulnerability; effects carry the
    // feedback.

    for (UBYTE i = 0; i < SHOTS; i++)
      if (game.shots[i].active)
        push(&shots[i], 4, (game.tick >> 1) & 1, game.shots[i].x,
             game.shots[i].y);
    for (UBYTE i = 0; i < BOLTS; i++)
      if (game.bolts[i].active)
        push(&bolts[i], 5, (game.tick >> 2) & 1, game.bolts[i].x,
             game.bolts[i].y);
    for (UBYTE i = 0; i < BLASTS; i++)
      if (game.blasts[i].active)
        push(&blasts[i], 3, (18 - game.blasts[i].timer) / 3, game.blasts[i].x,
             game.blasts[i].y);
  } else if (!transition) {

    if (game.phase != WON)
      push(&boss, 2, (game.frame >> 3) & 3, 206, 128);
  }
  bobEnd();
  hud(b);
  // Bars live above the playfield so no BOB save/restore can erase them.
  blitRect(buffer->pBack, 0, 29, 320, 1,
           game.flash        ? 253
           : game.invincible ? 223
                             : 195);
  if (game.phase == PLAY && game.bossActive) {
    blitRect(buffer->pBack, 184, 20, 128, 5, 193);
    blitRect(buffer->pBack, 184, 20, game.bossHp + 1, 5, 223);
  }
  blitWait();
  systemSetDmaBit(DMAB_BLITHOG, 0);
  UBYTE f = (game.phase == PLAY ? game.tick >> 2 : game.frame >> 3) & 3;
  WORD x = game.phase == PLAY ? game.x : 65,
       y = game.phase == PLAY ? game.y : 142;
  for (UBYTE p = 0; p < 6; p++) {
    spriteSetBitmap(mech[p], mechFrames[b][f][p]);
    mech[p]->wX = x + (p >> 1) * 16;
    mech[p]->wY = transition ? -50 : y;
    spriteRequestMetadataUpdate(mech[p]);
    spriteProcess(mech[p]);
    spriteProcessChannel(p);
  }
  viewProcessManagers(view);
}
static void report(void) {
  agkState("frame", game.frame);
  agkState("phase", game.phase);
  agkState("tick", game.tick);
  agkState("x", game.x);
  agkState("y", game.y);
  agkState("armor", game.health);
  agkState("score", game.score);
  agkState("wave", game.wave);
  agkState("boss", game.bossHp);
  agkState("emp", game.empCooldown);
  agkState("bstate", game.bossState);
  agkState("bpose", logicBossFrame(&game));
  agkEnd();
}
void genericCreate(void) {
  agkDebugInit();
  agkPrint("AGK boot ironwraith AGA 256 colors\n");
  keyCreate();
  joyOpen();
  view = viewCreate(0, TAG_VIEW_USES_AGA, 1, TAG_VIEW_GLOBAL_PALETTE, 1,
                    TAG_VIEW_COPLIST_MODE, VIEW_COPLIST_MODE_RAW,
                    TAG_VIEW_COPLIST_RAW_COUNT, 38, TAG_DONE);
  port = vPortCreate(0, TAG_VPORT_VIEW, view, TAG_VPORT_BPP, 8,
                     TAG_VPORT_USES_AGA, 1, TAG_VPORT_FMODE, 3, TAG_DONE);
  buffer = simpleBufferCreate(
      0, TAG_SIMPLEBUFFER_VPORT, port, TAG_SIMPLEBUFFER_BITMAP_FLAGS,
      BMF_CLEAR | BMF_INTERLEAVED, TAG_SIMPLEBUFFER_IS_DBLBUF, 1,
      TAG_SIMPLEBUFFER_COPLIST_OFFSET, 16, TAG_DONE);
  bufferA = buffer->pBack;
  artPaletteApply((ULONG *)port->pPalette);
  artMechApplyColors((ULONG *)port->pPalette);
  background = artFoundryCreate();
  art[1] = artDroneCreate();
  mask[1] = artDroneCreateMask();
  art[2] = artBossCreate();
  mask[2] = artBossCreateMask();
  art[3] = artBlastCreate();
  mask[3] = artBlastCreateMask();
  art[4] = artShotCreate();
  mask[4] = artShotCreateMask();
  art[5] = artBoltCreate();
  mask[5] = artBoltCreateMask();
  art[6] = artRepairCreate();
  mask[6] = artRepairCreateMask();
  for (UBYTE t = 1; t < TYPES; t++)
    for (UBYTE f = 0; f < counts[t]; f++) {
      frames[t][f] = bobCalcFrameAddress(art[t], f * heights[t]);
      masks[t][f] = bobCalcFrameAddress(mask[t], f * heights[t]);
    }
  bobManagerCreate(buffer->pFront, buffer->pBack, background, 256);
  initBob(&boss, 2);
  initBob(&repair, 6);
  for (UBYTE i = 0; i < ENEMIES; i++)
    initBob(&enemies[i], 1);
  for (UBYTE i = 0; i < SHOTS; i++)
    initBob(&shots[i], 4);
  for (UBYTE i = 0; i < BOLTS; i++)
    initBob(&bolts[i], 5);
  for (UBYTE i = 0; i < BLASTS; i++)
    initBob(&blasts[i], 3);
  bobReallocateBuffers();
  spriteManagerCreate(view, 0, 0);
  systemSetDmaBit(DMAB_SPRITE, 1);
  for (UBYTE b = 0; b < 2; b++)
    for (UBYTE f = 0; f < 4; f++)
      for (UBYTE p = 0; p < 6; p++)
        mechFrames[b][f][p] = artMechCreate(f, p);
  for (UBYTE p = 0; p < 6; p++) {
    mech[p] = spriteAdd(p, mechFrames[0][0][p]);
    if (p & 1)
      spriteSetAttached(mech[p], 1);
  }
  logicInit(&game);
  soundCreate();
  makePages();
  for (UBYTE b = 0; b < 2; b++) {
    blitCopyAligned(pages[TITLE], 0, 0, buffer->pBack, 0, 0, 320, 256);
    blitWait();
    shown[buffer->pBack == bufferA ? 0 : 1] = TITLE;
    hud(buffer->pBack == bufferA ? 0 : 1);
    viewProcessManagers(view);
    copProcessBlocks();
  }
  viewLoad(view);
  systemUnuse();
  agkDebugAsync(1);
  soundMusicStart(SOUND_MUSIC_SIEGE);
}
void genericProcess(void) {
  agkPerfBegin();
  ULONG vblank = timerGet();
  keyProcess();
  joyProcess();
  if (keyCheck(KEY_ESCAPE)) {
    gameExit();
    return;
  }
  if (keyUse(KEY_M)) {
    musicOn = !musicOn;
    if (musicOn)
      soundMusicStart(SOUND_MUSIC_SIEGE);
    else
      soundMusicStop();
  }
  Input in = {0};
  if (joyCheck(JOY1 + JOY_LEFT) || keyCheck(KEY_LEFT))
    in.dx = -1;
  if (joyCheck(JOY1 + JOY_RIGHT) || keyCheck(KEY_RIGHT))
    in.dx = 1;
  if (joyCheck(JOY1 + JOY_UP) || keyCheck(KEY_UP))
    in.dy = -1;
  if (joyCheck(JOY1 + JOY_DOWN) || keyCheck(KEY_DOWN))
    in.dy = 1;
  in.fire = joyCheck(JOY1 + JOY_FIRE) || keyCheck(KEY_SPACE);
  in.emp = joyCheck(JOY1 + JOY_FIRE2) || keyCheck(KEY_X);
  in.pause = keyCheck(KEY_P);
  UBYTE phase = game.phase;
  logicUpdate(&game, &in);
  if (game.events & EV_BOSS)
    soundPlay(SOUND_SFX_WARNING);
  else if (game.events & EV_EMP)
    soundPlay(SOUND_SFX_EMP);
  else if (game.events & EV_HIT)
    soundPlay(SOUND_SFX_HIT);
  else if (game.events & (EV_BLAST | EV_WIN))
    soundPlay(SOUND_SFX_EXPLOSION);
  else if (game.events & EV_SHOT)
    soundPlay(SOUND_SFX_CANNON);
  if (phase != game.phase ||
      game.events & (EV_EMP | EV_HIT | EV_BOSS | EV_WIN) || !(game.frame & 31))
    report();
  render();
  copProcessBlocks();
  agkPerfEnd();
  while (timerGet() == vblank)
    continue;
  if (game.frame == 2)
    agkReady();
  if (game.frame == 10)
    agkPrint("AGK t0\n");
}
void genericDestroy(void) {
  agkDebugAsync(0);
  systemUse();
  soundDestroy();
  systemSetDmaBit(DMAB_SPRITE, 0);
  spriteManagerDestroy();
  bobManagerDestroy();
  for (UBYTE b = 0; b < 2; b++)
    for (UBYTE f = 0; f < 4; f++)
      for (UBYTE p = 0; p < 6; p++)
        bitmapDestroy(mechFrames[b][f][p]);
  for (UBYTE t = 1; t < TYPES; t++) {
    bitmapDestroy(art[t]);
    bitmapDestroy(mask[t]);
  }
  for (UBYTE phase = 0; phase < 4; phase++)
    bitmapDestroy(pages[phase]);
  bitmapDestroy(background);
  viewDestroy(view);
  joyClose();
  keyDestroy();
}
