#include <agk/pack.h>
#include <agk/debug.h>
#include <ace/managers/memory.h>
#include <ace/managers/system.h>
#include <ace/utils/disk_file.h>
#include <ace/utils/file.h>

ULONG agkLoadFile(const char *szName, UBYTE *pDest, ULONG ulDestMax) {
	// The OS is briefly back for AmigaDOS (ACE's disk files do that themselves)
	tFile *pFile = diskFileOpen(szName, DISK_FILE_MODE_READ, 1);
	if(!pFile) {
		agkPrint("AGK ERR load: can't open ");
		agkPrint(szName);
		agkPrint(" (is it in disk/? the build packs disk/NAME into NAME.pk)\n");
		return 0;
	}
	LONG lPacked = fileGetSize(pFile);
	if(lPacked < 8) {
		fileClose(pFile);
		agkPrint("AGK ERR load: too small to be a packed file\n");
		return 0;
	}
	UBYTE *pPacked = memAlloc((ULONG)lPacked, MEMF_ANY);
	if(!pPacked) {
		fileClose(pFile);
		agkPrint("AGK ERR load: no memory for the packed file\n");
		return 0;
	}
	ULONG ulRead = fileReadBytes(pFile, pPacked, (ULONG)lPacked);
	fileClose(pFile);
	ULONG ulSize = agkUnpackedSize(pPacked);
	if(ulRead != (ULONG)lPacked || !ulSize) {
		agkPrint("AGK ERR load: not an AGKZ file or a short read\n");
		memFree(pPacked, (ULONG)lPacked);
		return 0;
	}
	if(ulSize > ulDestMax) {
		agkPrint("AGK ERR load: the unpacked file is bigger than the buffer\n");
		memFree(pPacked, (ULONG)lPacked);
		return 0;
	}
	agkUnpack(pPacked, pDest);
	memFree(pPacked, (ULONG)lPacked);
	return ulSize;
}
