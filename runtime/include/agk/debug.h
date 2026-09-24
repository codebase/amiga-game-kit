/**
 * AGK debug channel: tell the test harness what the game is doing.
 *
 * Writes text straight to the serial port (SERDAT), no OS needed, so it works
 * before and after ACE takes over the machine. The harness captures it; tests
 * assert on it with `expect-serial`, and `wait-serial` can sync on it.
 *
 * Conventions:
 *   agkReady()                      -> "AGK ready"   print once, after your first
 *                                      frame is actually on screen
 *   agkState("x", x); agkEnd();     -> "AGK x=12 y=5" machine-readable state
 *
 * Speed: by default each character busy-waits ~87us (115200 baud). After
 * systemUnuse(), call agkDebugAsync(1): output then goes through a 2KB ring
 * buffer drained by the serial interrupt and costs the game almost nothing.
 * Call agkDebugAsync(0) before systemUse() - the OS must not see our handler.
 *
 * Build with -DAGK_SERIAL=OFF for release: every call compiles to nothing.
 */
#ifndef _AGK_DEBUG_H_
#define _AGK_DEBUG_H_

#include <ace/types.h>

#ifdef AGK_SERIAL_ENABLED

void agkDebugInit(void);
void agkDebugAsync(UBYTE isOn);
void agkDebugFlush(void);
void agkPrint(const char *szText);
void agkPrintNum(LONG lValue);
void agkReady(void);

/** Start (or continue) an "AGK key=value ..." line. Finish it with agkEnd(). */
void agkState(const char *szKey, LONG lValue);
void agkEnd(void);

#else

#define agkDebugInit() do {} while(0)
#define agkDebugAsync(on) do { (void)(on); } while(0)
#define agkDebugFlush() do {} while(0)
#define agkPrint(sz) do { (void)(sz); } while(0)
#define agkPrintNum(l) do { (void)(l); } while(0)
#define agkReady() do {} while(0)
#define agkState(sz, l) do { (void)(sz); (void)(l); } while(0)
#define agkEnd() do {} while(0)

#endif

#endif // _AGK_DEBUG_H_
