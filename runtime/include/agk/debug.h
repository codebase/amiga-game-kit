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
 * Channels (CMake AGK_DEBUG_CHANNEL):
 *   host   (default) three writes to the NOOP register hand the string to
 *          AGK's emulator - costs a few instructions, nothing on real hardware.
 *   serial the real serial port at 115200 baud, for real-hardware debugging.
 *          Each character costs CPU time; after systemUnuse() call
 *          agkDebugAsync(1) to make it interrupt-driven, and agkDebugAsync(0)
 *          before systemUse(). (Both are no-ops on the host channel.)
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
/** Mark the start of a game frame for the harness (agkPerfBegin calls it). */
void agkTick(void);
void agkPrint(const char *szText);
void agkPrintNum(LONG lValue);
void agkReady(void);
UBYTE agkIsReady(void);

/** Start (or continue) an "AGK key=value ..." line. Finish it with agkEnd(). */
void agkState(const char *szKey, LONG lValue);
void agkEnd(void);

#else

#define agkDebugInit() do {} while(0)
#define agkDebugAsync(on) do { (void)(on); } while(0)
#define agkDebugFlush() do {} while(0)
#define agkTick() do {} while(0)
#define agkPrint(sz) do { (void)(sz); } while(0)
#define agkPrintNum(l) do { (void)(l); } while(0)
#define agkReady() do {} while(0)
#define agkIsReady() 1
#define agkState(sz, l) do { (void)(sz); (void)(l); } while(0)
#define agkEnd() do {} while(0)

#endif

#endif // _AGK_DEBUG_H_
