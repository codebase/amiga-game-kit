/**
 * The rules, such as they are: which level is shown, and fire (pressed, not
 * held) asks for the next one. Plain portable C, tested by agk unit.
 */
#ifndef _LOGIC_H_
#define _LOGIC_H_

#include <stdint.h>

#define LEVELS 2

typedef struct {
	int8_t dx, dy;
	uint8_t fire;
} tInput;

typedef struct {
	uint8_t level;                   // 1..LEVELS, the one on screen (or being loaded)
	uint8_t wantLoad;                // set when fire asks for the next level; main.c clears it after loading
	uint8_t fireWas;
	uint16_t frame;
} tGameState;

void logicInit(tGameState *pState);
/** One frame. Returns 1 when a load was asked for. */
uint8_t logicUpdate(tGameState *pState, const tInput *pInput);

#endif // _LOGIC_H_
