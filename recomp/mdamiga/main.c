/* The loader of a ported Mega Drive game: reads the port's image (game.z:
 * the patched ROM and the rewritten code, deflated) into fast RAM, then takes
 * the machine over and puts everything where agk port laid it out, in chip
 * RAM from address 0. Needs a 68020, AGA, 2 MB of chip RAM and fast RAM for
 * itself (this program must not sit where the image goes). */
#include <proto/exec.h>
#include <proto/dos.h>
#include <exec/memory.h>
#include "port_layout.h"

typedef unsigned char u8;
typedef unsigned long u32;
u32 inflate_raw(const u8 *src, u8 *dst);
void port_go(void);
void port_copy(const u8 *src, u8 *dst, u32 len);
extern u32 stub_table[];

static u8 *image;

/* in supervisor mode with everything off: the image to its addresses */
void port_place(void) {
	port_copy(image, (u8 *)0, PORT_ROMSIZE);
	port_copy(image, (u8 *)PORT_D, 0x8000);                    /* the first 32K again, above the RAM */
	port_copy(image + PORT_ROMSIZE, (u8 *)PORT_CODE, PORT_CODESIZE);
	for (u8 *p = (u8 *)PORT_RAM; p < (u8 *)PORT_D; p++) *p = 0;
	u8 *s = (u8 *)(PORT_SHIM + 0x100);
	for (int k = 0; stub_table[k]; k++, s += 8) {              /* the layer's entry points: jumps to start.S */
		u32 a = stub_table[k];
		s[0] = 0x4E; s[1] = 0xF9; s[2] = (u8)(a >> 24); s[3] = (u8)(a >> 16); s[4] = (u8)(a >> 8); s[5] = (u8)a;
	}
}

int main(void) {
	if ((u32)&main < 0x200000) { PutStr("This needs fast RAM (the game takes the chip RAM).\n"); return 20; }
	BPTR f = Open("game.z", MODE_OLDFILE);
	if (!f) { PutStr("game.z not found\n"); return 20; }
	u8 *z = AllocMem(PORT_ZSIZE, MEMF_FAST);
	image = AllocMem(PORT_ROMSIZE + PORT_CODESIZE, MEMF_FAST);
	if (!z || !image) { PutStr("not enough fast RAM\n"); return 20; }
	PutStr("loading...\n");
	if (Read(f, z, PORT_ZSIZE) != PORT_ZSIZE) { PutStr("game.z: read error\n"); return 20; }
	Close(f);
	PutStr("unpacking...\n");
	if (inflate_raw(z, image) != PORT_ROMSIZE + PORT_CODESIZE) { PutStr("game.z is damaged\n"); return 20; }
	Disable();
	Supervisor((void *)port_go);
	return 0;
}
