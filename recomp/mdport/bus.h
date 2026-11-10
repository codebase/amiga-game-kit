/* The bus of a Mega Drive game ported with agk port, for checking the port on
 * the host: flat memory up to the port's top (the ROM image with its old code
 * turned into jumps, the game's RAM, the rewritten code: see harness/agk/port.py),
 * and nothing else. The Mega Drive's own addresses must never be touched by a
 * finished port; here an access to them is reported with the pc (once per
 * pc) and then served as the Mega Drive would have, so one run lists every
 * place still to be ported. The hardware layer's entry points are trapped
 * (port.c) and answered by the emulated chips. */
#ifndef BUS_H
#define BUS_H
#include "start.h"
#define MACHINE_HZ START_HZ
#define REGION0_BASE 0u
#define REGION0_SIZE 0x400000u
#define REGION1_BASE 0xFF0000u
#define REGION1_SIZE 0x10000u
#define PORT_RAM ((u32)START_RAM)
#define PORT_SHIM ((u32)START_SHIM)
#define PORT_TOP ((u32)START_TOP)

u32 md_io_read(u32 a, int bits);
void md_io_write(u32 a, u32 v, int bits);
void irq_ack(int level);
void port_fault(u32 a, int kind);         /* 'm': the old RAM, 'h': the hardware, 'x': nothing there */
int port_trap(u32 pc);
#define MACHINE_TRAP(pc) port_trap(pc)
void port_stepped(void);
/* time: an instruction of the game costs what it did; the glue the port adds around it (and the
   jumps from old code) costs nothing, so the port runs frame for frame with the original.
   agk port's map of where the game's instructions start is loaded at $E00000, a bit per word. */
#define PORT_STARTS 0xE00000u
#define MACHINE_CYC(pc) do { if ((pc) >= PORT_TOP || (mem[PORT_STARTS + ((pc) >> 4)] >> (((pc) >> 1) & 7) & 1)) CYC(8); } while (0)
#define MACHINE_STEPPED() port_stepped()
#define CODE_WRITE(a, bits, v) ((void)0)
extern u8 code_bits[REGION0_SIZE >> 4], code_bits_slow[REGION1_SIZE >> 4];
void code_written(u32 a, int bits, u32 v);
#define DMA_RD16(a) (((a) & 0xFFFFFF) >= 0xE00000 ? ((u32)mem[PORT_RAM + ((a) & 0xFFFE)] << 8 | mem[PORT_RAM + ((a) & 0xFFFE) + 1]) : rd16(a))

/* where an address really is: below the top as it stands; the old RAM in the new; -1 the hardware */
static inline u32 port_at(u32 a) {
	if (a < PORT_TOP) return a;
	if (a >= 0xE00000) { port_fault(a, 'm'); return PORT_RAM + (a & 0xFFFF); }
	if (a >= 0xA00000) { port_fault(a, 'h'); return 0xFFFFFFFFu; }
	port_fault(a, 'x');
	return PORT_TOP;                      /* (a spare page past the top: reads 0) */
}
static inline u32 rd8(u32 a) {
	u32 o = a & 0xFFFFFF; a = port_at(o);
	return a == 0xFFFFFFFFu ? md_io_read(o, 8) : mem[a];
}
static inline u32 rd16(u32 a) {
	u32 o = a & 0xFFFFFE; a = port_at(o);
	return a == 0xFFFFFFFFu ? md_io_read(o, 16) : (u32)mem[a] << 8 | mem[a + 1];
}
static inline u32 rd32(u32 a) { return rd16(a) << 16 | rd16(a + 2); }
static inline void wr8(u32 a, u32 v) {
	u32 o = a & 0xFFFFFF; a = port_at(o);
	WATCH(a, 8);
	if (a == 0xFFFFFFFFu) md_io_write(o, v, 8); else mem[a] = (u8)v;
}
static inline void wr16(u32 a, u32 v) {
	u32 o = a & 0xFFFFFE; a = port_at(o);
	WATCH(a, 16);
	if (a == 0xFFFFFFFFu) md_io_write(o, v, 16); else { mem[a] = (u8)(v >> 8); mem[a + 1] = (u8)v; }
}
static inline void wr32(u32 a, u32 v) { wr16(a, v >> 16); wr16(a + 2, v & 0xFFFF); }
static inline u32 rd(u32 a, int bits) { return bits == 8 ? rd8(a) : bits == 16 ? rd16(a) : rd32(a); }
static inline void wr(u32 a, u32 v, int bits) { if (bits == 8) wr8(a, v); else if (bits == 16) wr16(a, v); else wr32(a, v); }
#endif
