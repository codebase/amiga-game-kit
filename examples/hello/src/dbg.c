#include "dbg.h"
#include <ace/utils/custom.h>

// PAL colour clock 3546895 Hz / 115200 baud - 1
#define DBG_SERPER 30
#define SERDATR_TBE (1 << 13)

void dbgInit(void) {
	g_pCustom->serper = DBG_SERPER;
}

void dbgPutc(char c) {
	while(!(g_pCustom->serdatr & SERDATR_TBE)) continue;
	// 8 data bits + 1 stop bit
	g_pCustom->serdat = 0x100 | (UBYTE)c;
}

void dbgPuts(const char *sz) {
	while(*sz) {
		if(*sz == '\n') {
			dbgPutc('\r');
		}
		dbgPutc(*sz++);
	}
}

void dbgPutNum(WORD wValue) {
	char szBuf[7];
	UBYTE ubPos = sizeof(szBuf) - 1;
	UWORD uwAbs = wValue < 0 ? -wValue : wValue;
	szBuf[ubPos] = '\0';
	do {
		szBuf[--ubPos] = '0' + (uwAbs % 10);
		uwAbs /= 10;
	} while(uwAbs);
	if(wValue < 0) {
		szBuf[--ubPos] = '-';
	}
	dbgPuts(&szBuf[ubPos]);
}
