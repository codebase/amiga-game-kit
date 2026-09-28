#include <agk/perf.h>
#include <agk/debug.h>
#include <ace/managers/timer.h>
#include <ace/utils/custom.h>

#define PAL_LINES 313
#define REPORT_EVERY 50

static UWORD s_uwBeginLine;
static ULONG s_ulLastVblank;
static UBYTE s_ubFrameVbls = 1;   // vertical blanks per game frame (1 = 50 fps)
static UBYTE s_isStarted;
static UWORD s_uwFrames, s_uwDropped, s_uwMaxLines;
static ULONG s_ulSumLines;

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
	// Percentages via 16-bit math where possible (68000 has no 32-bit divide).
	// Runs once a second, so libgcc divides are fine here.
	UWORD uwAvgLines = (UWORD)(s_ulSumLines / s_uwFrames);
	agkState(g_szAgkPerfKey, s_uwFrames);
	agkState("dropped", s_uwDropped);
	agkState("load", (uwAvgLines * 100) / (PAL_LINES * s_ubFrameVbls));
	agkState("maxload", (s_uwMaxLines * 100) / (PAL_LINES * s_ubFrameVbls));
	agkEnd();
	s_uwFrames = 0;
	s_uwDropped = 0;
	s_uwMaxLines = 0;
	s_ulSumLines = 0;
}

void agkPerfSetFrameVbls(UBYTE ubVbls) {
	s_ubFrameVbls = ubVbls ? ubVbls : 1;
}
