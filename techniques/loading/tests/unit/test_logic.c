// Host-side tests for src/logic.c. Run with: agk unit
#include <stdio.h>
#include "logic.h"

static int s_failures;
#define CHECK(cond) do { \
	if(!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); ++s_failures; } \
} while(0)

int main(void) {
	tGameState g;
	logicInit(&g);
	CHECK(g.level == 1 && g.wantLoad);          // the first level is a load too
	g.wantLoad = 0;                              // main.c did it
	CHECK(!logicUpdate(&g, &(tInput){0}));
	CHECK(logicUpdate(&g, &(tInput){.fire = 1}) && g.level == 2 && g.wantLoad);
	CHECK(!logicUpdate(&g, &(tInput){.fire = 1}));   // held: nothing more; and a load is pending anyway
	g.wantLoad = 0;
	CHECK(!logicUpdate(&g, &(tInput){.fire = 1}));   // still held
	CHECK(!logicUpdate(&g, &(tInput){0}));
	CHECK(logicUpdate(&g, &(tInput){.fire = 1}) && g.level == 1);   // wraps
	if(s_failures) { printf("%d check(s) failed\n", s_failures); return 1; }
	return 0;
}
