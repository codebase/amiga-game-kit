#include <agk/perf.h>
#include <agk/debug.h>
#include <ace/managers/timer.h>
#include <ace/utils/custom.h>

#define PAL_LINES 313
#define REPORT_EVERY 50

static UWORD s_uwBeginLine;
static ULONG s_ulLastVblank;
static UBYTE s_isStarted;
static UWORD s_uwFrames, s_uwDropped, s_uwMaxLines;
static ULONG s_ulSumLines;

static UWORD currentLine(void) {
	tRayPos sPos = getRayPos();
	return sPos.bfPosY;
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
	ULONG ulVblank = timerGet();
	if(s_isStarted) {
		// One vertical blank per frame is on time; more mean frames were lost.
		UWORD uwPassed = (UWORD)(ulVblank - s_ulLastVblank);
		if(uwPassed > 1) {
			s_uwDropped += uwPassed - 1;
		}
	}
	s_ulLastVblank = ulVblank;
	s_isStarted = 1;
	s_uwBeginLine = currentLine();
}

void agkPerfEnd(void) {
	if(!s_isStarted) {
		return;
	}
	UWORD uwEnd = currentLine();
	UWORD uwLines = uwEnd >= s_uwBeginLine
		? uwEnd - s_uwBeginLine
		: uwEnd + PAL_LINES - s_uwBeginLine;
	s_ulSumLines += uwLines;
	if(uwLines > s_uwMaxLines) {
		s_uwMaxLines = uwLines;
	}
	if(++s_uwFrames >= REPORT_EVERY) {
		agkPerfReport();
	}
}

void agkPerfReport(void) {
	if(!s_uwFrames) {
		return;
	}
	// Percentages via 16-bit math where possible (68000 has no 32-bit divide).
	// Runs once a second, so libgcc divides are fine here.
	UWORD uwAvgLines = (UWORD)(s_ulSumLines / s_uwFrames);
	agkState("perf frames", s_uwFrames);
	agkState("dropped", s_uwDropped);
	agkState("load", (uwAvgLines * 100) / PAL_LINES);
	agkState("maxload", (s_uwMaxLines * 100) / PAL_LINES);
	agkEnd();
	s_uwFrames = 0;
	s_uwDropped = 0;
	s_uwMaxLines = 0;
	s_ulSumLines = 0;
}
