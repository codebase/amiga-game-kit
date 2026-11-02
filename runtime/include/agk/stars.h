#ifndef _AGK_STARS_H_
#define _AGK_STARS_H_

/**
 * A parallax starfield from one hardware sprite, re-used on every line by
 * the copper: a star per line, three layers that move at 2, 1 and 1/2 px a
 * frame, and the CPU moves a whole layer by one byte per star. It takes no
 * bitplanes and no blitter time: VOIDRUNNER's sky.
 *
 * The stars are a copper list of their own (two: one per copper buffer),
 * one block per line: a WAIT, the sprite's position, its two data words
 * (the star's colour: 1, 2 or 3 of the channel's registers, far to near),
 * and COLOR00 when a per-line background colour changes. The list ends the
 * frame, so it's the last thing of the frame: the main list jumps to it
 * with agkStarsJump().
 *
 *   static tAgkStars s_sStars;
 *   agkStarsCreate(&s_sStars, 6, 24, 256, s_pView->ubPosY, 0x2545F491, pSpaceColors, 320);
 *   // in the main list, after everything else: agkStarsJump(&sW, &s_sStars, ubBfr);
 *   // every frame, for the back buffer: agkStarsUpdate(&s_sStars, ubBfr, uwFrame);
 *
 * The channel's three colours (registers 17 + 4 * (channel / 2), + 1, + 2)
 * are the game's to set: dim, mid, bright. Sprites on the same pair share
 * them. With BPLCON2 putting the channel behind the playfields, the stars
 * show through transparent pixels only.
 */

#include <ace/types.h>
#include <ace/managers/copper.h>
#include <agk/copw.h>

#define AGK_STARS_MAX_LINES 256
#define AGK_STARS_LAYERS 3

typedef struct {
	tCopCmd *pList[2];
	UWORD uwListLen;                                  // commands allocated per list
	UBYTE *pPos[2][AGK_STARS_LAYERS][AGK_STARS_MAX_LINES];   // per list and layer: each star's HSTART byte
	UWORD pCount[AGK_STARS_LAYERS];
	UBYTE ubHMin;                                     // HSTART / 2 at the left edge (off screen)
	UWORD uwHSpan;                                    // in 2 px steps: screen width + 16
} tAgkStars;

/** Lines uwY0..uwY1 - 1 (game lines), a star on half of them. pColor0 (or 0):
 *  COLOR00 per game line, written where it changes from the line above. */
void agkStarsCreate(tAgkStars *pStars, UBYTE ubChannel, UWORD uwY0, UWORD uwY1, UBYTE ubViewPosY,
	ULONG ulSeed, const UWORD *pColor0, UWORD uwScreenW);
void agkStarsDestroy(tAgkStars *pStars);

/** Jump to buffer ubBfr's star list (COP2LC + COPJMP2): the end of the main list. */
static inline void agkStarsJump(tAgkCopWriter *pW, const tAgkStars *pStars, UBYTE ubBfr) {
	agkCopJump2(pW, pStars->pList[ubBfr]);
}

/** Move the stars of list ubBfr to frame uwFrame (it last showed uwFrame - 2). */
void agkStarsUpdate(tAgkStars *pStars, UBYTE ubBfr, UWORD uwFrame);

#endif // _AGK_STARS_H_
