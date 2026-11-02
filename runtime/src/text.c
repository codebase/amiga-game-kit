#include <agk/text.h>
#include <agk/debug.h>

void agkFontInit(tAgkFont *pFont, const unsigned char (*pRows)[8], const char *szChars) {
	pFont->pRows = pRows;
	for(UBYTE i = 0; i < 128; ++i) pFont->pIndex[i] = 0;
	UBYTE ubCount = 0;
	for(UBYTE i = 0; szChars[i]; ++i) {
		if(i >= AGK_FONT_MAX_CHARS) {
			agkPrint("AGK ERR agkFontInit: more characters than AGK_FONT_MAX_CHARS\n");
			break;
		}
		pFont->pIndex[(UBYTE)szChars[i] & 0x7F] = i;
		ubCount = (UBYTE)(i + 1);
	}
	for(UBYTE c = 0; c < ubCount; ++c) {
		UBYTE ubAbove = 0;
		for(UBYTE r = 0; r < 8; ++r) {
			UBYTE ubLetter = pRows[c][r], ubShadow = (UBYTE)((ubAbove >> 1) & ~ubLetter);   // one right, one down
			pFont->pShadow[c][r] = ubShadow;
			pFont->pBoth[c][r] = (UBYTE)(ubLetter | ubShadow);
			ubAbove = ubLetter;
		}
	}
}

// A character is 8 rows x depth bytes, copied from the font's tables: no
// multiplies, no branches per byte, the eight stores unrolled (a score bar
// redraws a few digits every frame of a fight)
#define COPY8(pDst, pSrc, uwStep) do { \
	UBYTE *pD_ = (pDst); const UBYTE *pS_ = (pSrc); UWORD uwS_ = (uwStep); \
	pD_[0] = pS_[0]; pD_ += uwS_; pD_[0] = pS_[1]; pD_ += uwS_; pD_[0] = pS_[2]; pD_ += uwS_; \
	pD_[0] = pS_[3]; pD_ += uwS_; pD_[0] = pS_[4]; pD_ += uwS_; pD_[0] = pS_[5]; pD_ += uwS_; \
	pD_[0] = pS_[6]; pD_ += uwS_; pD_[0] = pS_[7]; \
} while(0)

static const UBYTE s_pZero[8];

// (not inlined into agkTextSet: its registers would spill the compare loop to the stack)
static void __attribute__((noinline)) drawChar(const tAgkText *pText, UBYTE ubCol, char c) {
	const tAgkFont *pFont = pText->pFont;
	UBYTE ubChar = pFont->pIndex[(UBYTE)c & 0x7F];
	UWORD uwStep = pText->uwRowBytes;
	for(UBYTE p = 0; p < pText->ubDepth; ++p) {
		UBYTE isL = (pText->ubColor >> p) & 1, isS = (pText->ubShadow >> p) & 1;
		const UBYTE *pSrc = isL ? (isS ? pFont->pBoth[ubChar] : pFont->pRows[ubChar])
		                        : (isS ? pFont->pShadow[ubChar] : s_pZero);
		COPY8(pText->pPlane[p] + ubCol, pSrc, uwStep);
	}
}

void agkTextInit(tAgkText *pText, const tAgkFont *pFont, tBitMap *pBm, UWORD uwX, UWORD uwY,
	UBYTE ubLen, UBYTE ubColor, UBYTE ubShadow
) {
	if(ubLen > AGK_TEXT_MAX) {
		agkPrint("AGK ERR agkTextInit: more characters than AGK_TEXT_MAX\n");
		ubLen = AGK_TEXT_MAX;
	}
	pText->pFont = pFont;
	pText->ubDepth = pBm->Depth;
	pText->uwRowBytes = pBm->BytesPerRow;
	// (a multiply once, here: every drawChar steps pointers)
	ULONG ulRow = (ULONG)uwY * pBm->BytesPerRow + (uwX >> 3);
	UWORD uwPlaneBytes = bitmapIsInterleaved(pBm) ? pBm->BytesPerRow / pBm->Depth : 0;
	for(UBYTE p = 0; p < pBm->Depth && p < 8; ++p) {
		pText->pPlane[p] = uwPlaneBytes ? pBm->Planes[0] + ulRow + p * uwPlaneBytes : pBm->Planes[p] + ulRow;
	}
	pText->ubLen = ubLen;
	pText->ubColor = ubColor;
	pText->ubShadow = ubShadow;
	for(UBYTE i = 0; i < ubLen; ++i) {
		pText->szShown[i] = ' ';
		drawChar(pText, i, ' ');
	}
	pText->szShown[ubLen] = 0;
}

void agkTextSet(tAgkText *pText, const char *szText) {
	char *pShown = pText->szShown;
	const char *pNew = szText;
	for(UBYTE i = 0; i < pText->ubLen; ++i, ++pShown) {
		char c = *pNew;
		if(c) {
			++pNew;
		}
		else {
			c = ' ';                       // past the end: spaces
		}
		if(c != *pShown) {
			*pShown = c;
			drawChar(pText, i, c);
		}
	}
}

void agkTextNumber(char *pOut, ULONG ulValue, UBYTE ubDigits) {
	for(BYTE d = ubDigits - 1; d >= 0; --d) {
#if defined(__mc68000__)
		// right to left, two 16-bit DIVUs per digit
		ULONG ulHi = ulValue >> 16, ulLo = ulValue & 0xFFFF;
		__asm__("divu.w #10,%0" : "+d"(ulHi));
		ulLo |= ulHi & 0xFFFF0000;
		__asm__("divu.w #10,%0" : "+d"(ulLo));
		pOut[d] = '0' + (char)(ulLo >> 16);
		ulValue = (ulHi << 16) | (ulLo & 0xFFFF);
#else
		pOut[d] = '0' + (char)(ulValue % 10);
		ulValue /= 10;
#endif
	}
}
