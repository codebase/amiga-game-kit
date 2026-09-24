/**
 * AGK frame budget meter: how much of each 1/50s frame the game's work uses,
 * and whether frames were dropped. Agents can't feel a game getting slow -
 * this makes it a number tests can assert on.
 *
 *   void genericProcess(void) {
 *       agkPerfBegin();          // first thing in the frame
 *       ...input, logic, drawing...
 *       agkPerfEnd();            // last thing before waiting for the next frame
 *       vPortWaitForEnd(...);
 *   }
 *
 * Every 50 frames (and at agkPerfReport()) it prints e.g.
 *   AGK perf frames=50 dropped=0 load=31 maxload=44
 * load/maxload = average/worst % of a PAL frame (313 lines) spent between
 * Begin and End; dropped = vertical blanks that passed without a new frame.
 * In tests:  expect-no-serial "dropped=[1-9]"   /   expect-no-serial "maxload=(9[0-9]|1[0-9][0-9])"
 *
 * agkPerfBegin() also marks the frame start for the harness ("tick"): with
 * sync = "ticks" in agk.toml, scenario time is counted in these game frames.
 *
 * Counting starts at agkReady(), so startup isn't reported as dropped frames.
 * Needs ACE's timer manager (the default generic main creates it).
 */
#ifndef _AGK_PERF_H_
#define _AGK_PERF_H_

#include <ace/types.h>

#ifdef AGK_SERIAL_ENABLED
void agkPerfBegin(void);
void agkPerfEnd(void);
void agkPerfReport(void);
#else
#define agkPerfBegin() do {} while(0)
#define agkPerfEnd() do {} while(0)
#define agkPerfReport() do {} while(0)
#endif

#endif // _AGK_PERF_H_
