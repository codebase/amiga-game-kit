// The AGKZ unpacker (harness/agk/pack.py makes the files). Plain C: the
// harness compiles this file on the host and checks it against the packer.
#include <agk/pack.h>

unsigned long agkUnpackedSize(const unsigned char *pSrc) {
	if(pSrc[0] != 'A' || pSrc[1] != 'G' || pSrc[2] != 'K' || pSrc[3] != 'Z') {
		return 0;
	}
	return ((unsigned long)pSrc[4] << 24) | ((unsigned long)pSrc[5] << 16) | ((unsigned long)pSrc[6] << 8) | pSrc[7];
}

unsigned long agkUnpack(const unsigned char *pSrc, unsigned char *pDest) {
	unsigned long ulSize = agkUnpackedSize(pSrc);
	unsigned char *pOut = pDest, *pEnd = pDest + ulSize;
	pSrc += 8;
	// Tokens: a flag byte, then 8 items, bit 7 first: 1 = a literal byte,
	// 0 = a match (two bytes: 12 bits of distance back - 1, 4 bits of length - 3)
	unsigned char ubFlags = 0, ubLeft = 0;
	while(pOut < pEnd) {
		if(!ubLeft) {
			ubFlags = *pSrc++;
			ubLeft = 8;
		}
		--ubLeft;
		if(ubFlags & 0x80) {
			*pOut++ = *pSrc++;
		}
		else {
			unsigned int uwA = *pSrc++, uwB = *pSrc++;
			const unsigned char *pFrom = pOut - (((uwA << 4) | (uwB >> 4)) + 1);
			unsigned int uwLen = (uwB & 15) + 3;
			do {
				*pOut++ = *pFrom++;
			} while(--uwLen);
		}
		ubFlags = (unsigned char)(ubFlags << 1);
	}
	return ulSize;
}
