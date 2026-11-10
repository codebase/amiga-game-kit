/* The loader of a ported Mega Drive game: reads the port's image (game.z:
 * the patched ROM and the rewritten code, deflated) into fast RAM, then takes
 * the machine over and puts everything where agk port laid it out, in chip
 * RAM from address 0. Needs a 68020, AGA, 2 MB of chip RAM and fast RAM for
 * itself (this program must not sit where the image goes). */
#include <proto/exec.h>
#include <proto/dos.h>
#include <exec/memory.h>
#include <exec/execbase.h>
#include "port_layout.h"

typedef unsigned char u8;
typedef unsigned long u32;
u32 inflate_raw(const u8 *src, u8 *dst);
void port_go(void);
void port_copy(const u8 *src, u8 *dst, u32 len);
extern u32 stub_table[];

static u8 *image;
extern const u8 *port_sound;              /* sound.c: its data, in fast RAM */
extern int port_hz;

/* the music and the effects (sound.z, from agk port): on this disk, or on the second one */
static void load_sound(void) {
#if PORT_SNDDISK
	BPTR f = Open(PORT_SNDDISK == 2 ? "PORT2:sound.z" : "sound.z", MODE_OLDFILE);
	if (!f) { PutStr("no sound.z: silent\n"); return; }
	u8 *z = AllocMem(PORT_SNDZSIZE, MEMF_FAST);
	u8 *s = AllocMem(PORT_SNDSIZE, MEMF_FAST);
	if (z && s && Read(f, z, PORT_SNDZSIZE) == PORT_SNDZSIZE && inflate_raw(z, s) == PORT_SNDSIZE) port_sound = s;
	else PutStr("sound.z: not loaded (memory, or the file is damaged): silent\n");
	if (z) FreeMem(z, PORT_SNDZSIZE);
	Close(f);
#endif
}

static u32 be32(const u8 *p) { return (u32)p[0] << 24 | (u32)p[1] << 16 | (u32)p[2] << 8 | p[3]; }
static void add32(u8 *p, u32 d) { u32 v = be32(p) + d; p[0] = (u8)(v >> 24); p[1] = (u8)(v >> 16); p[2] = (u8)(v >> 8); p[3] = (u8)v; }

u32 port_ram, port_sp;                    /* where the game's RAM is, and its first stack pointer */
static u8 *block;                         /* fast RAM for the game's RAM, the ROM's copy above it and the code */

/* in supervisor mode with everything off: the image to its addresses. The ROM (the game's data, and
 * the jumps its old code has become) goes to address 0: chip RAM, there is nothing else down there.
 * The game's RAM, the ROM's first 32K again above it and the rewritten code were laid out to follow
 * the ROM, but go to fast RAM, at a multiple of 64K (chip RAM is half the speed with eight bitplanes
 * on): agk port listed every address of them in the image, and they are moved by the difference. */
void port_place(void) {
	u32 d = (u32)block - PORT_RAM;
	u8 *code = (u8 *)(PORT_CODE + d);
	const u8 *t = image + PORT_ROMSIZE + PORT_CODESIZE;
	u32 nc = be32(t), nr = be32(t + 4);
	port_ram = PORT_RAM + d;
	port_sp = PORT_SP + d;
	port_copy(image + PORT_ROMSIZE, code, PORT_CODESIZE);
	t += 8;
	for (u32 i = 0; i < nc; i++, t += 4) add32(code + be32(t), d);
	for (u32 i = 0; i < nr; i++, t += 4) add32(image + be32(t), d);
	{                                     /* the header's checksum (the game verifies it) for the ROM with its addresses moved */
		u32 end = be32(image + 0x1A4) + 1, sum = 0;
		if (end > PORT_ROMSIZE) end = PORT_ROMSIZE;
		end &= ~1UL;
		for (u32 a = 0x200; a < end; a += 2) sum += (u32)image[a] << 8 | image[a + 1];
		if (nr && end > 0x200) { image[0x18E] = (u8)(sum >> 8); image[0x18F] = (u8)sum; }
	}
	port_copy(image, (u8 *)0, PORT_ROMSIZE);
	port_copy(image, (u8 *)(PORT_D + d), 0x8000);              /* the first 32K again, above the RAM */
	for (u8 *p = (u8 *)port_ram; p < (u8 *)(PORT_D + d); p++) *p = 0;
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
	image = AllocMem(PORT_IMGSIZE, MEMF_FAST);
	/* (below 16 MB if there is such: the game was written for 24 address lines) */
	u8 *raw = AllocMem(PORT_TOP - PORT_RAM + 0x10000, MEMF_FAST | MEMF_24BITDMA);
	if (!raw) raw = AllocMem(PORT_TOP - PORT_RAM + 0x10000, MEMF_FAST);
	block = (u8 *)(((u32)raw + 0xFFFF) & ~0xFFFFUL);
	if (!z || !image || !raw) { PutStr("not enough fast RAM\n"); return 20; }
	PutStr("loading...\n");
	if (Read(f, z, PORT_ZSIZE) != PORT_ZSIZE) { PutStr("game.z: read error\n"); return 20; }
	Close(f);
	PutStr("unpacking...\n");
	if (inflate_raw(z, image) != PORT_IMGSIZE) { PutStr("game.z is damaged\n"); return 20; }
	FreeMem(z, PORT_ZSIZE);
	PutStr("sound...\n");
	load_sound();
	port_hz = SysBase->VBlankFrequency == 60 ? 60 : 50;
	Disable();
	Supervisor((void *)port_go);
	return 0;
}
