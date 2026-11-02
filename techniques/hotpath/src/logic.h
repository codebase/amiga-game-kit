/**
 * The rules: a field of particles that fall, bounce and drift with the
 * joystick's "tilt". Plain portable C, tested on the host by agk unit.
 * The plotting of the particles is the Amiga side's hot path (main.c).
 */
#ifndef _LOGIC_H_
#define _LOGIC_H_

#include <stdint.h>

#define WORLD_W 320
#define WORLD_H 256
#define PARTICLES 128
#define FIX 4                    // positions in 1/4 px

typedef struct {
	int16_t x, y;                // 1/4 px, 0 <= x < WORLD_W * FIX
	int16_t vx, vy;              // 1/4 px per frame
} tParticle;

typedef struct {
	int8_t dx, dy;               // -1, 0 or 1 per axis
	uint8_t fire;
} tInput;

typedef struct {
	tParticle pParticles[PARTICLES];
	int8_t gx, gy;               // the tilt: where the particles are pulled
	uint16_t frame;
	uint32_t rnd;
} tGameState;

void logicInit(tGameState *pState);
/** One frame: the tilt follows the joystick, every particle moves and bounces. */
void logicUpdate(tGameState *pState, const tInput *pInput);

#endif // _LOGIC_H_
