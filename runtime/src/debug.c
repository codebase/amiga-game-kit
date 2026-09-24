#include <agk/debug.h>
#include <ace/managers/system.h>
#include <ace/utils/custom.h>
#include <hardware/intbits.h>

static UBYTE s_isLineOpen;
static UBYTE s_isReady;

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

void agkPrintNum(LONG lValue) {
	// No division: the 68000 has no 32-bit divide, and libgcc's is slow.
	static const ULONG pPowers[] = {
		1000000000, 100000000, 10000000, 1000000, 100000, 10000, 1000, 100, 10, 1
	};
	char szBuf[12];
	UBYTE ubPos = 0;
	ULONG ulAbs = lValue < 0 ? -(ULONG)lValue : (ULONG)lValue;
	if(lValue < 0) {
		szBuf[ubPos++] = '-';
	}
	UBYTE isStarted = 0;
	for(UBYTE i = 0; i < sizeof(pPowers) / sizeof(pPowers[0]); ++i) {
		char cDigit = '0';
		while(ulAbs >= pPowers[i]) {
			ulAbs -= pPowers[i];
			++cDigit;
		}
		if(cDigit != '0' || isStarted || i == 9) {
			szBuf[ubPos++] = cDigit;
			isStarted = 1;
		}
	}
	szBuf[ubPos] = '\0';
	agkPrint(szBuf);
}

void agkReady(void) {
	agkPrint("AGK ready\n");
	s_isReady = 1;
}

UBYTE agkIsReady(void) {
	return s_isReady;
}

void agkState(const char *szKey, LONG lValue) {
	agkPrint(s_isLineOpen ? " " : "AGK ");
	agkPrint(szKey);
	agkPrint("=");
	agkPrintNum(lValue);
	s_isLineOpen = 1;
}

void agkEnd(void) {
	if(s_isLineOpen) {
		agkPrint("\n");
		s_isLineOpen = 0;
	}
}
