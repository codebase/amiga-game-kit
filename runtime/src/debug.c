#include <agk/debug.h>
#include <ace/managers/system.h>
#include <ace/utils/custom.h>
#include <hardware/intbits.h>

// PAL colour clock 3546895 Hz / 115200 baud - 1
#define AGK_SERPER 30
#define SERDATR_TBE (1 << 13)
#define AGK_QUEUE_SIZE 2048 // power of two

static UBYTE s_isLineOpen;

// Async mode: characters go into a ring buffer that the serial "transmit
// buffer empty" interrupt drains, so printing costs the game almost nothing.
static volatile UBYTE s_isAsync;
static volatile UBYTE s_isSending;   // a character is in flight; TBE will follow
static volatile UWORD s_uwHead, s_uwTail;
static char s_pQueue[AGK_QUEUE_SIZE];

static void sendNow(char c) {
	g_pCustom->serdat = 0x100 | (UBYTE)c; // 8 data bits + stop bit
}

static void putcBlocking(char c) {
	while(!(g_pCustom->serdatr & SERDATR_TBE)) continue;
	sendNow(c);
}

static void onTbe(
	REGARG(volatile tCustom *pCustom, "a0"), REGARG(volatile void *pData, "a1")
) {
	(void)pCustom;
	(void)pData;
	if(s_uwHead != s_uwTail) {
		sendNow(s_pQueue[s_uwTail]);
		s_uwTail = (s_uwTail + 1) & (AGK_QUEUE_SIZE - 1);
	}
	else {
		s_isSending = 0;
	}
}

void agkDebugInit(void) {
	g_pCustom->serper = AGK_SERPER;
}

void agkDebugAsync(UBYTE isOn) {
	if(isOn && !s_isAsync) {
		s_uwHead = s_uwTail = 0;
		s_isSending = 0;
		s_isAsync = 1;
		systemSetInt(INTB_TBE, onTbe, 0);
	}
	else if(!isOn && s_isAsync) {
		agkDebugFlush();
		systemSetInt(INTB_TBE, 0, 0);
		s_isAsync = 0;
	}
}

void agkDebugFlush(void) {
	while(s_isAsync && (s_isSending || s_uwHead != s_uwTail)) continue;
}

static void agkPutc(char c) {
	if(!s_isAsync) {
		putcBlocking(c);
		return;
	}
	UWORD uwNext = (s_uwHead + 1) & (AGK_QUEUE_SIZE - 1);
	while(uwNext == s_uwTail) continue; // queue full: wait for the interrupt

	g_pCustom->intena = INTF_TBE; // keep the interrupt out while we decide
	if(!s_isSending) {
		s_isSending = 1;
		sendNow(c);
	}
	else {
		s_pQueue[s_uwHead] = c;
		s_uwHead = uwNext;
	}
	g_pCustom->intena = INTF_SETCLR | INTF_TBE;
}

void agkPrint(const char *szText) {
	while(*szText) {
		if(*szText == '\n') {
			agkPutc('\r');
		}
		agkPutc(*szText++);
	}
}

void agkPrintNum(LONG lValue) {
	char szBuf[12];
	UBYTE ubPos = sizeof(szBuf) - 1;
	ULONG ulAbs = lValue < 0 ? -(ULONG)lValue : (ULONG)lValue;
	szBuf[ubPos] = '\0';
	do {
		// 32-bit divide is a libgcc call on 68000; fine for debug output.
		szBuf[--ubPos] = '0' + (ulAbs % 10);
		ulAbs /= 10;
	} while(ulAbs);
	if(lValue < 0) {
		szBuf[--ubPos] = '-';
	}
	agkPrint(&szBuf[ubPos]);
}

void agkReady(void) {
	agkPrint("AGK ready\n");
}

void agkState(const char *szKey, LONG lValue) {
	agkPrint(s_isLineOpen ? " " : "AGK ");
	agkPrint(szKey);
	agkPutc('=');
	agkPrintNum(lValue);
	s_isLineOpen = 1;
}

void agkEnd(void) {
	if(s_isLineOpen) {
		agkPrint("\n");
		s_isLineOpen = 0;
	}
}
