// Host-side tests for src/logic.c. Run with: agk unit
#include <stdio.h>
#include "logic.h"

static int s_failures;
#define CHECK(cond) do { \
	if(!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); ++s_failures; } \
} while(0)

static tGameState s_g;

int main(void) {
	logicInit(&s_g);
	tInput in = {0};
	for(int f = 0; f < 2000; ++f) {
		if(f == 500) in = (tInput){.dx = 1, .dy = -1};
		if(f == 501) in = (tInput){0};
		logicUpdate(&s_g, &in);
		for(int i = 0; i < PARTICLES; ++i) {
			const tParticle *p = &s_g.pParticles[i];
			CHECK(p->x >= 0 && p->x < WORLD_W * FIX && p->y >= 0 && p->y < WORLD_H * FIX);
		}
		if(f == 499) CHECK(s_g.gx == 0 && s_g.gy == 1);
		if(f == 501) CHECK(s_g.gx == 1 && s_g.gy == -1);   // the tilt stays after the stick is let go
	}
	// deterministic: the same seed gives the same field
	tGameState g2;
	logicInit(&g2);
	for(int f = 0; f < 100; ++f) logicUpdate(&g2, &(tInput){0});
	tGameState g3;
	logicInit(&g3);
	for(int f = 0; f < 100; ++f) logicUpdate(&g3, &(tInput){0});
	CHECK(g2.pParticles[77].x == g3.pParticles[77].x && g2.pParticles[100].vy == g3.pParticles[100].vy);
	if(s_failures) { printf("%d check(s) failed\n", s_failures); return 1; }
	return 0;
}
