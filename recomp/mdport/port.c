/* The host side of a ported Mega Drive game under test: the hardware layer's
 * entry points answered by the emulated chips, and the reports of anything
 * that still reaches for the Mega Drive's own addresses. */
#include "host.h"
#include "start.h"
#include <string.h>

#define ENTRIES (PORT_SHIM + 0x100)       /* r8 r16 r32 w8 w16 w32, 8 bytes apart (agk port's SHIM_NAMES) */
#define CELL PORT_SHIM

static struct { u32 pc; int kind; u32 addr; u32 origin[8]; } faults[4096];
static int nfaults, in_shim;
static u64 shim_calls;

/* where each address register got its value: the last instruction that set it to something new
 * (not one that only stepped it). When a pointer into the Mega Drive's RAM is followed, that
 * instruction is where the port has to bring it over (agk port --learn does, from the report). */
static u32 a_seen[8], a_writer[8];
static u32 recent[8192], recent_sp[8192]; static unsigned nrecent;
/* an interrupt can arrive between a rewritten instruction and its call into the layer, with the
 * value in the cell; the handler uses the cell too. The Amiga's interrupt entry saves and restores
 * it around the game's handler (start.S); so does this, at the interrupt and at its rte. */
static u32 cell_saved[16]; static int cell_depth;
void port_irq_taken(void) { if (cell_depth < 16) memcpy(&cell_saved[cell_depth], &mem[CELL], 4); cell_depth++; }
void port_stepped(void) {
	if (cell_depth && (((u32)mem[cpu.pc] << 8 | mem[cpu.pc + 1]) == 0x4E73) && cpu.pc < PORT_TOP) {
		cell_depth--;
		if (cell_depth < 16) memcpy(&mem[CELL], &cell_saved[cell_depth], 4);
	}
	recent_sp[nrecent & 8191] = A[7]; recent[nrecent++ & 8191] = cpu.pc;
	for (int i = 0; i < 8; i++) if (A[i] != a_seen[i]) {
		u32 d = A[i] - a_seen[i];
		if (d + 64 > 128) a_writer[i] = cpu.pc;
		a_seen[i] = A[i];
	}
}
static int old_ram(u32 v) { return (v >> 24 == 0xFF || v >> 24 == 0) && (v & 0xFFFFFF) >= 0xE00000; }

void port_fault(u32 a, int kind) {
	if (in_shim) return;                  /* (the shim asked: that's what it's for) */
	for (int i = 0; i < nfaults; i++) if (faults[i].pc == cpu.pc && faults[i].kind == kind) return;
	if (nfaults < 4096) {
		faults[nfaults].pc = cpu.pc; faults[nfaults].kind = kind; faults[nfaults].addr = a;
		for (int i = 0; i < 8; i++) faults[nfaults].origin[i] = old_ram(A[i]) || old_ram(a_seen[i]) ? a_writer[i] : 0;
		nfaults++;
	}
	if (nfaults <= 40)
		fprintf(stderr, "port: frame %u pc $%06X touches %s $%06X\n", frame_count, cpu.pc,
		        kind == 'm' ? "the Mega Drive's RAM (a pointer the port didn't move) at" :
		        kind == 'h' ? "hardware directly (not through the layer) at" : "nothing at", a);
}

/* the pc is at one of the layer's entry points: 4(sp) is the address, the cell holds the value */
int port_trap(u32 pc) {
	if (pc < PORT_SHIM) {
		if (pc < START_ROMSIZE && !(pc & 1) && ((u32)mem[pc] << 8 | mem[pc + 1]) == 0x4AFC) {
			fprintf(stderr, "port: old code entered at $%06X, which no known path leads to: add `entry %06X` to port.txt "
			        "(a table or a computed jump the analysis run didn't take)\n", pc, pc);
			trace_die("unknown entry into old code", pc);
		}
		return 0;
	}
	if (pc >= PORT_TOP || (pc & 1)) trace_die("the pc left the port's memory", pc);
	u32 k = pc - ENTRIES;
	if (k >= 8 * 13 || (k & 7)) return 0;
	k >>= 3;
	if (k >= 6) {                         /* fixa0..fixa6: a pointer that may still be a Mega Drive RAM address */
		u32 *r = &A[k - 6];
		if (old_ram(*r)) *r = PORT_RAM + (*r & 0xFFFF);
		return 1;
	}
	int bits = 8 << (k % 3);
	u32 a = (((u32)mem[A[7] + 4] << 24 | (u32)mem[A[7] + 5] << 16 | (u32)mem[A[7] + 6] << 8 | mem[A[7] + 7])) & 0xFFFFFF;
	shim_calls++;
	in_shim = 1;
	if (k < 3) {
		u32 v;
		if (a >= 0xA00000 && a < 0xE00000) v = bits == 32 ? md_io_read(a, 16) << 16 | md_io_read(a + 2, 16) : md_io_read(a, bits);
		else v = rd(a, bits);
		if (bits == 8) mem[CELL] = (u8)v;
		else if (bits == 16) { mem[CELL] = (u8)(v >> 8); mem[CELL + 1] = (u8)v; }
		else { mem[CELL] = (u8)(v >> 24); mem[CELL + 1] = (u8)(v >> 16); mem[CELL + 2] = (u8)(v >> 8); mem[CELL + 3] = (u8)v; }
	} else {
		u32 v = bits == 8 ? mem[CELL] : bits == 16 ? (u32)mem[CELL] << 8 | mem[CELL + 1]
		      : (u32)mem[CELL] << 24 | (u32)mem[CELL + 1] << 16 | (u32)mem[CELL + 2] << 8 | mem[CELL + 3];
		if (a >= 0xA00000 && a < 0xE00000) { if (bits == 32) { md_io_write(a, v >> 16, 16); md_io_write(a + 2, v & 0xFFFF, 16); } else md_io_write(a, v, bits); }
		else wr(a, v, bits);
	}
	in_shim = 0;
	return 1;
}

void port_report(void) {
	if (getenv("RECOMP_PORT_RECENT")) {   /* the last instructions, oldest first (after a crash) */
		fprintf(stderr, "port: recent pcs:");
		for (unsigned i = nrecent > 8192 ? nrecent - 8192 : 0; i < nrecent; i++) fprintf(stderr, " %06X:%X", recent[i & 8191], recent_sp[i & 8191] & 0xFFFF);
		fputc('\n', stderr);
	}
	fprintf(stderr, "port: %llu calls into the hardware layer; %d place(s) still reach for the Mega Drive's addresses\n",
	        (unsigned long long)shim_calls, nfaults);
	const char *path = getenv("RECOMP_PORT_FAULTS");
	if (path) {
		FILE *f = fopen(path, "w");
		if (f) {
			for (int i = 0; i < nfaults; i++) {
				fprintf(f, "%06X %c %06X", faults[i].pc, faults[i].kind, faults[i].addr);
				for (int r = 0; r < 8; r++) if (faults[i].origin[r]) fprintf(f, " a%d=%06X", r, faults[i].origin[r]);
				fputc('\n', f);
			}
			fclose(f);
		}
	}
}
