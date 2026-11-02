#include <agk/perf.h>
#include <agk/debug.h>
#include <ace/managers/timer.h>
#include <clib/exec_protos.h>
#include <exec/memory.h>
#include <ace/utils/custom.h>

#define PAL_LINES 313
#define REPORT_EVERY 50

static UWORD s_uwBeginLine;
static ULONG s_ulLastVblank;
static UBYTE s_ubFrameVbls = 1;   // vertical blanks per game frame (1 = 50 fps)
static UBYTE s_isStarted;
static UWORD s_uwFrames, s_uwDropped, s_uwMaxLines;
static ULONG s_ulSumLines;

static inline ULONG mulu(UWORD a, UWORD b) {
#if defined(__mc68000__)
	ULONG r = a;
	__asm__("mulu.w %1,%0" : "+d"(r) : "dmi"(b));
	return r;
#else
	return (ULONG)a * b;
#endif
}

// a / b for a < 65536 * b (the quotient fits 16 bits)
static inline UWORD divu32(ULONG a, UWORD b) {
#if defined(__mc68000__)
	__asm__("divu.w %1,%0" : "+d"(a) : "dmi"(b));
	return (UWORD)a;
#else
	return (UWORD)(a / b);
#endif
}

static UWORD currentLine(void) {
	tRayPos sPos = getRayPos();
	return sPos.bfPosY;
}

// The vertical blank count and the beam line as one consistent pair: if the
// blank interrupt fires between reading one and the other, read again.
static UWORD lineAndVblank(ULONG *pVblank) {
	ULONG ulBefore, ulAfter;
	UWORD uwLine;
	do {
		ulBefore = timerGet();
		uwLine = currentLine();
		ulAfter = timerGet();
	} while(ulBefore != ulAfter);
	*pVblank = ulAfter;
	return uwLine;
}

void agkPerfBegin(void) {
	// Tell the harness a game frame starts now: in tick-sync mode it applies
	// scenario input here, just before the game reads it.
	agkTick();
	if(!agkIsReady()) {
		// Startup (OS takeover, first frames) isn't gameplay: don't count it.
		s_isStarted = 0;
		return;
	}
	ULONG ulVblank;
	UWORD uwLine = lineAndVblank(&ulVblank);
	if(s_isStarted) {
		// s_ubFrameVbls vertical blanks per frame are on time; more mean frames were lost.
		UWORD uwPassed = (UWORD)(ulVblank - s_ulLastVblank);
		if(uwPassed > s_ubFrameVbls) {
			s_uwDropped += uwPassed - s_ubFrameVbls;
		}
	}
	s_ulLastVblank = ulVblank;
	s_isStarted = 1;
	s_uwBeginLine = uwLine;
}

void agkPerfEnd(void) {
	if(!s_isStarted) {
		return;
	}
	ULONG ulVblank;
	UWORD uwEnd = lineAndVblank(&ulVblank);
	// Lines between Begin and End, across any vertical blanks in between.
	// The blank counter can lag the beam: its interrupt (level 3) waits while
	// a higher one runs, e.g. a music player's CIA timer (level 6). So if the
	// beam has wrapped but the counter hasn't, count that blank ourselves.
	UWORD uwVbls = (UWORD)(ulVblank - s_ulLastVblank);
	if(uwVbls * PAL_LINES + uwEnd < s_uwBeginLine) {
		++uwVbls;
	}
	UWORD uwLines = uwVbls * PAL_LINES + uwEnd - s_uwBeginLine;
	s_ulSumLines += uwLines;
	if(uwLines > s_uwMaxLines) {
		s_uwMaxLines = uwLines;
	}
	if(++s_uwFrames >= REPORT_EVERY) {
		agkPerfReport();
	}
	agkTickEnd();
}

// Global (not a literal) so the linker map has it: the emulator notes its
// address from the report, and agk profile places the game's code with it.
const char g_szAgkPerfKey[] = "perf frames";

void agkPerfReport(void) {
	if(!s_uwFrames) {
		return;
	}
	// DIVU/MULU (16-bit): the report lands in one frame a second, and a
	// library divide there showed as that frame's peak in agk profile
	UWORD uwFrameLines = (UWORD)(PAL_LINES * s_ubFrameVbls);
	UWORD uwAvgLines = divu32(s_ulSumLines, s_uwFrames);
	agkState(g_szAgkPerfKey, s_uwFrames);
	agkState("dropped", s_uwDropped);
	agkState("load", divu32(mulu(uwAvgLines, 100), uwFrameLines));
	agkState("maxload", divu32(mulu(s_uwMaxLines, 100), uwFrameLines));
	agkEnd();
	agkMemReport();
	s_uwFrames = 0;
	s_uwDropped = 0;
	s_uwMaxLines = 0;
	s_ulSumLines = 0;
}

void agkMemReport(void) {
	// AvailMem only walks exec's free lists under Forbid: safe with the OS
	// switched off (ACE does the same when an allocation fails), and far
	// cheaper than switching it on and off, which costs a few % of a frame
	ULONG ulChip = AvailMem(MEMF_CHIP), ulChipMax = AvailMem(MEMF_CHIP | MEMF_LARGEST);
	ULONG ulFast = AvailMem(MEMF_FAST);
	agkState("mem chip", (LONG)(ulChip >> 10));
	agkState("chipmax", (LONG)(ulChipMax >> 10));
	agkState("fast", (LONG)(ulFast >> 10));
	agkEnd();
}

void agkPerfSetFrameVbls(UBYTE ubVbls) {
	s_ubFrameVbls = ubVbls ? ubVbls : 1;
}
