#ifndef _AGK_TEXT_H_
#define _AGK_TEXT_H_

/**
 * Text in a bitmap: a score bar, a message, a menu. Written by the CPU, a
 * byte per plane per row, only for the characters that changed since the
 * last call, so a score that ticks up costs one or two characters a frame.
 *
 * A font is 8x8 cells, one byte per row (bit 7 = the left pixel), the
 * letter in rows 0-6; the shadow (one pixel right and down) is made here.
 * The kit's font, g_pAgkFont8, has digits, capitals and " !/:.-".
 *
 *   static tAgkFont s_sFont;
 *   static tAgkText s_pHud[2];                           // one per frame buffer
 *   agkFontInit(&s_sFont, g_pAgkFont8, AGK_FONT8_CHARS);
 *   agkTextInit(&s_pHud[b], &s_sFont, pBitmap[b], 16, 8, 40, 5, 1);   // x, y, chars, colour, shadow (0: none)
 *   char szLine[41] = "SCORE 0000000";
 *   agkTextNumber(&szLine[6], ulScore, 7);
 *   agkTextSet(&s_pHud[b], szLine);                      // draws what changed
 *
 * Colours are pen numbers of the bitmap: planes whose bit is set in the
 * colour get the letter, planes set in the shadow colour get the shadow,
 * the others are cleared under the character. examples/voidrunner's HUD.
 */

#include <ace/types.h>
#include <ace/utils/bitmap.h>

#ifndef AGK_TEXT_MAX
#define AGK_TEXT_MAX 48            // characters per tAgkText (-DAGK_TEXT_MAX=... for more)
#endif

#ifndef AGK_FONT_MAX_CHARS
#define AGK_FONT_MAX_CHARS 64      // characters per font (-DAGK_FONT_MAX_CHARS=... for more)
#endif

typedef struct {
	const unsigned char (*pRows)[8];   // per character, 8 rows: the letter
	UBYTE pShadow[AGK_FONT_MAX_CHARS][8];   // the shadow alone, and with the letter: made once,
	UBYTE pBoth[AGK_FONT_MAX_CHARS][8];     // so a character is plain copies (the HUD redraws every frame)
	UBYTE pIndex[128];                 // ASCII -> character (0: the first, usually a space)
} tAgkFont;

typedef struct {
	const tAgkFont *pFont;
	UBYTE *pPlane[8];                  // per plane: the line's first byte (row 0, column 0)
	UWORD uwRowBytes;                  // from a row to the next, per plane
	UBYTE ubDepth, ubLen, ubColor, ubShadow;
	char szShown[AGK_TEXT_MAX + 1];
} tAgkText;

#define AGK_FONT8_CHARS " 0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ!/:.-"
#define AGK_FONT8_COUNT 42
extern const unsigned char g_pAgkFont8[AGK_FONT8_COUNT][8];   // 5x7 arcade capitals (tools/font8.py)

/** szChars lists the characters in pRows's order; others draw as the first. */
void agkFontInit(tAgkFont *pFont, const unsigned char (*pRows)[8], const char *szChars);

/** A line of ubLen characters at uwX (a multiple of 8), uwY px in pBm (any
 *  depth, interleaved or not), colour and shadow pens (shadow 0: none).
 *  Draws spaces, so the area is cleared. */
void agkTextInit(tAgkText *pText, const tAgkFont *pFont, tBitMap *pBm, UWORD uwX, UWORD uwY,
	UBYTE ubLen, UBYTE ubColor, UBYTE ubShadow);

/** Show szText (padded with spaces to the line's length): only the characters
 *  that differ from what's shown are drawn. */
void agkTextSet(tAgkText *pText, const char *szText);

/** ubDigits decimal digits of ulValue into pOut, zero-padded, no terminator
 *  (DIVU on the 68000: no library divide). */
void agkTextNumber(char *pOut, ULONG ulValue, UBYTE ubDigits);

#endif // _AGK_TEXT_H_
