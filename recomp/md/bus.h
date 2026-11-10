/* The Mega Drive's bus, as the 68000 sees it: the cartridge ROM at $000000
 * (up to 4 MB, writes vanish), 64 KB of work RAM at $FF0000 mirrored through
 * $E00000.., and in between the Z80's side ($A00000), the I/O ports
 * ($A10000), the Z80 bus and reset latches ($A11100/$A11200), TMSS ($A14000)
 * and the VDP ($C00000). Included by m68k.h; md.c has the chips. */
#ifndef BUS_H
#define BUS_H
#define MACHINE_HZ START_HZ               /* 60 (NTSC) or 50 (PAL), from the ROM's region */
#define REGION0_BASE 0u                   /* the ROM: the translation's first table */
#define REGION0_SIZE 0x400000u
#define REGION1_BASE 0xFF0000u            /* work RAM: the second (code the game copies there) */
#define REGION1_SIZE 0x10000u
#define RAM_BASE 0xFF0000u

u32 md_io_read(u32 a, int bits);          /* everything that isn't ROM or RAM */
void md_io_write(u32 a, u32 v, int bits);
void irq_ack(int level);                  /* the CPU took an interrupt of this level */

extern u8 code_bits[REGION0_SIZE >> 4], code_bits_slow[REGION1_SIZE >> 4];
void code_written(u32 a, int bits, u32 v);
#define CODE_WRITE(a, bits, v) do { if (code_bits_slow[((a) - RAM_BASE) >> 4]) code_written((a), (bits), (v)); } while (0)

static inline u32 rd8(u32 a) {
	a &= 0xFFFFFF;
	if (a < REGION0_SIZE) return mem[a];
	if (a >= 0xE00000) return mem[RAM_BASE | (a & 0xFFFF)];
	return md_io_read(a, 8);
}
static inline u32 rd16(u32 a) {
	a &= 0xFFFFFE;
	if (a < REGION0_SIZE) return (u32)mem[a] << 8 | mem[a + 1];
	if (a >= 0xE00000) { a = RAM_BASE | (a & 0xFFFF); return (u32)mem[a] << 8 | mem[a + 1]; }
	return md_io_read(a, 16);
}
static inline u32 rd32(u32 a) {
	a &= 0xFFFFFE;
	if (a < REGION0_SIZE - 2) return (u32)mem[a] << 24 | (u32)mem[a + 1] << 16 | (u32)mem[a + 2] << 8 | mem[a + 3];
	if (a >= 0xE00000) { u32 b = RAM_BASE | (a & 0xFFFF), c = RAM_BASE | ((a + 2) & 0xFFFF); return (u32)mem[b] << 24 | (u32)mem[b + 1] << 16 | (u32)mem[c] << 8 | mem[c + 1]; }
	return md_io_read(a, 16) << 16 | md_io_read(a + 2, 16);
}
static inline void wr8(u32 a, u32 v) {
	a &= 0xFFFFFF;
	WATCH(a, 8);
	if (a < REGION0_SIZE) return;         /* the ROM */
	if (a >= 0xE00000) { a = RAM_BASE | (a & 0xFFFF); CODE_WRITE(a, 8, v); mem[a] = (u8)v; return; }
	md_io_write(a, v, 8);
}
static inline void wr16(u32 a, u32 v) {
	a &= 0xFFFFFE;
	WATCH(a, 16);
	if (a < REGION0_SIZE) return;
	if (a >= 0xE00000) { a = RAM_BASE | (a & 0xFFFF); CODE_WRITE(a, 16, v); mem[a] = (u8)(v >> 8); mem[a + 1] = (u8)v; return; }
	md_io_write(a, v, 16);
}
static inline void wr32(u32 a, u32 v) {
	a &= 0xFFFFFE;
	WATCH(a, 32);
	if (a < REGION0_SIZE) return;
	if (a >= 0xE00000) { wr16(a, v >> 16); wr16(a + 2, v & 0xFFFF); return; }
	md_io_write(a, v >> 16, 16); md_io_write(a + 2, v & 0xFFFF, 16);
}
static inline u32 rd(u32 a, int bits) { return bits == 8 ? rd8(a) : bits == 16 ? rd16(a) : rd32(a); }
static inline void wr(u32 a, u32 v, int bits) { if (bits == 8) wr8(a, v); else if (bits == 16) wr16(a, v); else wr32(a, v); }
#endif
