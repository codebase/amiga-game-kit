#include "logic.h"

void logicInit(tGameState *pState) {
	pState->level = 1;
	pState->wantLoad = 1;            // the first level is loaded too
	pState->fireWas = 0;
	pState->frame = 0;
}

uint8_t logicUpdate(tGameState *pState, const tInput *pInput) {
	++pState->frame;
	uint8_t isPressed = pInput->fire && !pState->fireWas;
	pState->fireWas = pInput->fire;
	if(isPressed && !pState->wantLoad) {
		pState->level = (uint8_t)(pState->level == LEVELS ? 1 : pState->level + 1);
		pState->wantLoad = 1;
		return 1;
	}
	return 0;
}
