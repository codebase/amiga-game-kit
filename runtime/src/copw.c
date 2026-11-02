#include <agk/copw.h>
#include <agk/debug.h>

void agkCopFill(tAgkCopWriter *pW, UWORD uwCount) {
	if(pW->uwPos > uwCount) {
		agkPrint("AGK ERR copper list overflow: raise the raw count\n");
		return;
	}
	while(pW->uwPos < uwCount) {
		copSetWait(&pW->pList[pW->uwPos++].sWait, 0xDF, 0xFF);   // (line 255, x 0xDF: after the frame)
	}
}
