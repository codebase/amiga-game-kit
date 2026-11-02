#include "logic.h"

#define MAX_X (WORLD_W * FIX - 1)
#define MAX_Y (WORLD_H * FIX - 1)
#define MAX_V (6 * FIX)

static uint16_t rnd16(tGameState *pState) {   // xorshift: shifts and xors only
	uint32_t x = pState->rnd;
	x ^= x << 13;
	x ^= x >> 17;
	x ^= x << 5;
	pState->rnd = x;
	return (uint16_t)x;
}

void logicInit(tGameState *pState) {
	pState->rnd = 0x9E3779B9;
	pState->frame = 0;
	pState->gx = 0;
	pState->gy = 1;
	for(uint16_t i = 0; i < PARTICLES; ++i) {
		tParticle *p = &pState->pParticles[i];
		p->x = (int16_t)(rnd16(pState) % (WORLD_W * FIX));
		p->y = (int16_t)(rnd16(pState) % (WORLD_H * FIX));
		p->vx = (int16_t)((rnd16(pState) & 15) - 8);
		p->vy = (int16_t)((rnd16(pState) & 15) - 8);
	}
}

void logicUpdate(tGameState *pState, const tInput *pInput) {
	++pState->frame;
	if(pInput->dx || pInput->dy) {        // the joystick tilts the field; let go and it keeps the tilt
		pState->gx = pInput->dx;
		pState->gy = pInput->dy;
	}
	int16_t gx = pState->gx, gy = pState->gy;
	for(uint16_t i = 0; i < PARTICLES; ++i) {
		tParticle *p = &pState->pParticles[i];
		p->vx = (int16_t)(p->vx + gx);
		p->vy = (int16_t)(p->vy + gy);
		if(p->vx > MAX_V) p->vx = MAX_V;
		if(p->vx < -MAX_V) p->vx = -MAX_V;
		if(p->vy > MAX_V) p->vy = MAX_V;
		if(p->vy < -MAX_V) p->vy = -MAX_V;
		p->x = (int16_t)(p->x + p->vx);
		p->y = (int16_t)(p->y + p->vy);
		// bounce, losing a quarter of the speed
		if(p->x < 0) { p->x = (int16_t)-p->x; p->vx = (int16_t)(-p->vx - (p->vx >> 2)); }
		if(p->x > MAX_X) { p->x = (int16_t)(2 * MAX_X - p->x); p->vx = (int16_t)(-p->vx + (p->vx >> 2)); }
		if(p->y < 0) { p->y = (int16_t)-p->y; p->vy = (int16_t)(-p->vy - (p->vy >> 2)); }
		if(p->y > MAX_Y) { p->y = (int16_t)(2 * MAX_Y - p->y); p->vy = (int16_t)(-p->vy + (p->vy >> 2)); }
	}
}
