#include <agk/debug.h>
#include <ace/managers/system.h>
#include <ace/utils/custom.h>
#include <hardware/intbits.h>

static UBYTE s_isLineOpen;
static UBYTE s_isReady;
static UBYTE s_isReadyPending;
static UBYTE s_isTicking;   // the game calls agkTick() (via agkPerfBegin) every frame

#ifdef AGK_CHANNEL_HOST
//------------------------------------------------------------------ host channel
// The emulator (AGK's patched vAmiga) picks the string up directly when the
// CPU writes 0xA6E0, address high, address low to NOOP ($DFF1FE). Costs a few
// instructions; on real hardware these writes do nothing.

#define NOOP (*(volatile UWORD *)0xDFF1FE)

void agkDebugInit(void) {
}

void agkDebugAsync(UBYTE isOn) {
	(void)isOn;
}

void agkDebugFlush(void) {
}

void agkTick(void) {
	s_isTicking = 1;
	if(s_isReadyPending) {
		// Announce "ready" exactly at a frame start (see agkReady)
		s_isReadyPending = 0;
		agkPrint("AGK ready\n");
	}
	NOOP = 0xA6E1;
}

void agkPrint(const char *szText) {
	ULONG ulAddr = (ULONG)szText;
	// Keep interrupts from splitting the 3-word sequence.
	UWORD uwIntEna = g_pCustom->intenar & INTF_INTEN;
	g_pCustom->intena = INTF_INTEN;
	NOOP = 0xA6E0;
	NOOP = (UWORD)(ulAddr >> 16);
	NOOP = (UWORD)ulAddr;
	if(uwIntEna) {
		g_pCustom->intena = INTF_SETCLR | INTF_INTEN;
	}
}

#else
//---------------------------------------------------------------- serial channel
// Real serial port at 115200 baud, for debugging on real hardware.

// PAL colour clock 3546895 Hz / 115200 baud - 1
#define AGK_SERPER 30
#define SERDATR_TBE (1 << 13)
#define AGK_QUEUE_SIZE 2048 // power of two

// Async mode: characters go into a ring buffer that the serial "transmit
// buffer empty" interrupt drains.
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

void agkTick(void) {
	s_isTicking = 1;
	if(s_isReadyPending) {
		s_isReadyPending = 0;
		agkPrint("AGK ready\n");
	}
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

// Queue one character. Caller has the TBE interrupt disabled.
static void enqueue(char c) {
	UWORD uwNext = (s_uwHead + 1) & (AGK_QUEUE_SIZE - 1);
	while(uwNext == s_uwTail) {
		// Queue full: let the interrupt drain some, then block it again.
		g_pCustom->intena = INTF_SETCLR | INTF_TBE;
		g_pCustom->intena = INTF_TBE;
	}
	if(!s_isSending) {
		s_isSending = 1;
		sendNow(c);
	}
	else {
		s_pQueue[s_uwHead] = c;
		s_uwHead = uwNext;
	}
}

void agkPrint(const char *szText) {
	if(!s_isAsync) {
		while(*szText) {
			if(*szText == '\n') {
				putcBlocking('\r');
			}
			putcBlocking(*szText++);
		}
		return;
	}
	// One interrupt toggle per string, not per character.
	g_pCustom->intena = INTF_TBE;
	while(*szText) {
		if(*szText == '\n') {
			enqueue('\r');
		}
		enqueue(*szText++);
	}
	g_pCustom->intena = INTF_SETCLR | INTF_TBE;
}

#endif

//--------------------------------------------------------------------- formatting

// agkState() collects a whole "AGK k=v k=v" line here and agkEnd() sends it in
// one go: one host transfer per line instead of four per value.
#define AGK_LINE_MAX 200
static char s_szLine[AGK_LINE_MAX + 2];
static UBYTE s_ubLineLen;

static const ULONG s_pPowers[] = {
	1000000000, 100000000, 10000000, 1000000, 100000, 10000, 1000, 100, 10, 1
};

// Writes lValue in decimal to pDst, returns the number of chars written.
// No division: the 68000 has no 32-bit divide, and libgcc's is slow.
static UBYTE formatNum(char *pDst, LONG lValue) {
	UBYTE ubLen = 0;
	ULONG ulAbs = lValue < 0 ? -(ULONG)lValue : (ULONG)lValue;
	if(lValue < 0) {
		pDst[ubLen++] = '-';
	}
	// Skip powers of ten larger than the value (most values are small)
	UBYTE i = 0;
	while(i < 9 && ulAbs < s_pPowers[i]) {
		++i;
	}
	for(; i < 10; ++i) {
		char cDigit = '0';
		while(ulAbs >= s_pPowers[i]) {
			ulAbs -= s_pPowers[i];
			++cDigit;
		}
		pDst[ubLen++] = cDigit;
	}
	return ubLen;
}

void agkPrintNum(LONG lValue) {
	char szBuf[12];
	szBuf[formatNum(szBuf, lValue)] = '\0';
	agkPrint(szBuf);
}

void agkReady(void) {
	// Games that tick (agkPerfBegin) announce ready at the start of the next
	// frame instead of now. The harness snapshots "ready" at the next video
	// frame boundary, and a frame start sits at a fixed beam line just before
	// it - so scenario time 0 is always the same game frame, however long this
	// frame's work took. Without ticks, print immediately.
	s_isReady = 1;
	if(s_isTicking) {
		s_isReadyPending = 1;
	}
	else {
		agkPrint("AGK ready\n");
	}
}

UBYTE agkIsReady(void) {
	return s_isReady;
}

static void lineAppend(const char *sz) {
	while(*sz && s_ubLineLen < AGK_LINE_MAX) {
		s_szLine[s_ubLineLen++] = *sz++;
	}
}

void agkState(const char *szKey, LONG lValue) {
	if(!s_isLineOpen) {
		s_ubLineLen = 0;
		lineAppend("AGK ");
		s_isLineOpen = 1;
	}
	else {
		lineAppend(" ");
	}
	lineAppend(szKey);
	lineAppend("=");
	if(s_ubLineLen <= AGK_LINE_MAX - 11) {
		s_ubLineLen += formatNum(&s_szLine[s_ubLineLen], lValue);
	}
}

void agkEnd(void) {
	if(s_isLineOpen) {
		s_szLine[s_ubLineLen++] = '\n';
		s_szLine[s_ubLineLen] = '\0';
		agkPrint(s_szLine);
		s_isLineOpen = 0;
	}
}
