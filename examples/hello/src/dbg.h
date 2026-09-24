// Minimal serial debug channel: polls SERDAT directly, no OS needed.
// The harness captures this stream (vAmiga: serial -> RetroShell / stdout).
#ifndef _DBG_H_
#define _DBG_H_

#include <ace/types.h>

void dbgInit(void);
void dbgPutc(char c);
void dbgPuts(const char *sz);
void dbgPutNum(WORD wValue);

#endif // _DBG_H_
