/* The Amiga's bus: chip RAM to $200000 (the chips at $DFF000 and the CIAs at
 * $BFE001/$BFD000 trapped), slow RAM at $C00000, the ROM at $F80000 (writes
 * vanish). Included by m68k.h; the translation's dispatch covers the two RAM
 * regions below. */
#ifndef BUS_H
#define BUS_H
#define MACHINE_HZ 50                     /* PAL */
#define REGION0_BASE 0u                   /* chip RAM: the translation's first table */
#define REGION0_SIZE 0x200000u
#define REGION1_BASE 0xC00000u            /* slow RAM: the second */
#define REGION1_SIZE 0x200000u
u32 chip_read(u32 addr, int bits);
static inline void irq_ack(int level) { (void)level; }   /* Paula's INTREQ stays until the handler clears it */
void chip_write(u32 addr, u32 value, int bits);
void cia_write(u32 addr, u8 value);
u8 cia_read(u32 addr);

/* ------------------------------------------------------------ memory */

#define IS_CUSTOM(a) (((a) & 0xFFF000u) == CUSTOM_BASE)
#define IS_CIA(a) (((a) & 0xFFF000u) == 0xBFE000u || ((a) & 0xFFF000u) == 0xBFD000u)

static inline u32 rd8(u32 a) {
	a &= 0xFFFFFF;
	if (IS_CUSTOM(a)) return chip_read(a, 8);
	if (IS_CIA(a)) return cia_read(a);
	return mem[a];
}
static inline u32 rd16(u32 a) {
	a &= 0xFFFFFF;
	if (IS_CUSTOM(a)) return chip_read(a, 16);
	if (IS_CIA(a)) return (u32)cia_read(a) << 8 | cia_read(a + 1);
	return (u32)mem[a] << 8 | mem[a + 1];
}
static inline u32 rd32(u32 a) {
	a &= 0xFFFFFF;
	if (IS_CUSTOM(a)) return chip_read(a, 32);
	if (IS_CIA(a)) return (u32)cia_read(a) << 24 | (u32)cia_read(a + 1) << 16 | (u32)cia_read(a + 2) << 8 | cia_read(a + 3);   /* (games read a long: the same register four times) */
	return (u32)mem[a] << 24 | (u32)mem[a + 1] << 16 | (u32)mem[a + 2] << 8 | mem[a + 3];
}
/* self-modifying code: a write that lands in a 16-byte granule holding translated
 * code goes to the translation's code_written (game.c), which tells rewritten
 * instructions from patched immediates and hands the block to the interpreter */
extern u8 code_bits[REGION0_SIZE >> 4], code_bits_slow[REGION1_SIZE >> 4];
void code_written(u32 a, int bits, u32 v);
#define CODE_WRITE(a, bits, v) do { if ((a) < 0x200000u ? code_bits[(a) >> 4] : ((a) >= 0xC00000u && (a) < 0xE00000u && code_bits_slow[((a) - 0xC00000u) >> 4])) code_written((a), (bits), (v)); } while (0)
static inline void wr8(u32 a, u32 v) {
	a &= 0xFFFFFF;
	WATCH(a, 8);
	if (IS_CUSTOM(a)) { chip_write(a, v, 8); return; }
	if (IS_CIA(a)) { cia_write(a, (u8)v); return; }
	if (a >= 0xF80000) return;             /* the ROM: writes vanish */
	CODE_WRITE(a, 8, v);
	mem[a] = (u8)v;
}
static inline void wr16(u32 a, u32 v) {
	a &= 0xFFFFFF;
	WATCH(a, 16);
	if (IS_CUSTOM(a)) { chip_write(a, v, 16); return; }
	if (IS_CIA(a)) { cia_write(a, (u8)(v >> 8)); cia_write(a + 1, (u8)v); return; }
	if (a >= 0xF80000) return;
	CODE_WRITE(a, 16, v);
	mem[a] = (u8)(v >> 8); mem[a + 1] = (u8)v;
}
static inline void wr32(u32 a, u32 v) {
	a &= 0xFFFFFF;
	WATCH(a, 32);
	if (IS_CUSTOM(a)) { chip_write(a, v, 32); return; }
	if (IS_CIA(a)) { cia_write(a, (u8)(v >> 24)); cia_write(a + 1, (u8)(v >> 16)); cia_write(a + 2, (u8)(v >> 8)); cia_write(a + 3, (u8)v); return; }
	if (a >= 0xF80000) return;
	CODE_WRITE(a, 32, v);
	mem[a] = (u8)(v >> 24); mem[a + 1] = (u8)(v >> 16); mem[a + 2] = (u8)(v >> 8); mem[a + 3] = (u8)v;
}
static inline u32 rd(u32 a, int bits) { return bits == 8 ? rd8(a) : bits == 16 ? rd16(a) : rd32(a); }
static inline void wr(u32 a, u32 v, int bits) { if (bits == 8) wr8(a, v); else if (bits == 16) wr16(a, v); else wr32(a, v); }
#endif
