#ifndef _AGK_PACK_H_
#define _AGK_PACK_H_

/**
 * Packed data files from the disk (agk help-disk). The build packs every
 * file in disk/ into NAME.pk on the floppy; the game loads one with:
 *
 *   ULONG ulSize = agkLoadFile("level1.pk", pDest, ulDestMax);   // 0 on failure ('AGK ERR' says why)
 *   agkPerfSkip();                                                // a load takes longer than a frame
 *
 * agkLoadFile reads the file through AmigaDOS (a floppy, a hard disk or
 * WHDLoad, Kickstart 1.3 up) into a temporary buffer and unpacks it into
 * pDest, which must hold the unpacked size (the file's header says it: the
 * build prints it, and agkLoadFile checks it). techniques/loading shows it.
 *
 * agkUnpack is the unpacker alone, plain C (no Amiga headers): the harness
 * runs it on the host against the packer.
 */

/** Unpack an AGKZ buffer into pDest; returns the unpacked size. */
unsigned long agkUnpack(const unsigned char *pSrc, unsigned char *pDest);
/** The unpacked size an AGKZ buffer's header announces (0 if not one). */
unsigned long agkUnpackedSize(const unsigned char *pSrc);

#ifdef AMIGA
#include <ace/types.h>
/** Load and unpack disk file szName into pDest (at most ulDestMax bytes). */
ULONG agkLoadFile(const char *szName, UBYTE *pDest, ULONG ulDestMax);
#endif

#endif // _AGK_PACK_H_
