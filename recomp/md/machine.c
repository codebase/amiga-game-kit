/* The Mega Drive as a machine for the recompiled game: the start state is
 * the cartridge's reset vector (start.h), and the window's keys are the
 * three-button pad in port 1: arrows/WASD the d-pad, Z/X/C (or J/K/L) the
 * A/B/C buttons, Return the Start button, Esc quits. */
#ifdef USE_SDL
#include <SDL.h>
#endif
#include "host.h"
#include "start.h"
#include <string.h>

void md_init(void);
void md_dump_vdp(void);
extern u16 pad_state[2];

void machine_init(void) { md_init(); frame_w = 320; frame_h = 224; }
void md_save_sites(void);
#ifdef PORT_RAM
void port_report(void);
void machine_at_exit(void) { md_dump_vdp(); port_report(); }
#else
void md_sound_rip(void);
void machine_at_exit(void) { md_dump_vdp(); md_save_sites(); md_sound_rip(); }
#endif

void md_sound_rip_arg(const char *dir);
int machine_arg(int argc, char **argv, int *i) {
	if (!strcmp(argv[*i], "--sound-rip") && *i + 1 < argc) { md_sound_rip_arg(argv[++*i]); return 1; }
	return 0;
}
const char *machine_usage(void) { return "[--sound-rip DIR] "; }

void machine_start(void) {
	cpu.d[0] = START_D0; cpu.d[1] = START_D1; cpu.d[2] = START_D2; cpu.d[3] = START_D3;
	cpu.d[4] = START_D4; cpu.d[5] = START_D5; cpu.d[6] = START_D6; cpu.d[7] = START_D7;
	cpu.a[0] = START_A0; cpu.a[1] = START_A1; cpu.a[2] = START_A2; cpu.a[3] = START_A3;
	cpu.a[4] = START_A4; cpu.a[5] = START_A5; cpu.a[6] = START_A6; cpu.a[7] = START_A7;
	cpu.usp = START_USP; cpu.ssp = START_SSP;
	cpu.s = (START_SR >> 13) & 1; cpu.ipl = (START_SR >> 8) & 7;
	set_ccr(START_SR & 0xFF);
	cpu.pc = START_PC;
}

/* scripted input: "up down left right a b c start" for pad 1, "up2 down2 ..." for pad 2 */
static u16 words_pad(const char *words, const char *suffix) {
	static const char *names[8] = { "up", "down", "left", "right", "b", "c", "a", "start" };
	u16 v = 0;
	for (int i = 0; i < 8; i++) {
		char w[16]; snprintf(w, sizeof w, "%s%s", names[i], suffix);
		if (has_word(words, w)) v |= (u16)(1 << i);
	}
	return v;
}
void machine_apply_input(const char *words) {
	pad_state[0] = words_pad(words, "");
	pad_state[1] = words_pad(words, "2");
}
void machine_input_words(char *buf, size_t n) {
	static const char *names[8] = { "up", "down", "left", "right", "b", "c", "a", "start" };
	buf[0] = 0;
	for (int p = 0; p < 2; p++)
		for (int i = 0; i < 8; i++)
			if (pad_state[p] & (1 << i)) { size_t l = strlen(buf); snprintf(buf + l, n - l, " %s%s", names[i], p ? "2" : ""); }
}

#ifdef USE_SDL
void machine_sdl_key(int sym, int mod, int down) {
	(void)mod;
	if (sym >= 0 && down == 1 && (SDL_Scancode)sym == SDL_SCANCODE_ESCAPE) host_exit();
}
void machine_sdl_input(void) {
	const Uint8 *k = SDL_GetKeyboardState(NULL);
	u16 v = 0;
	if (k[SDL_SCANCODE_UP] || k[SDL_SCANCODE_W]) v |= 1;
	if (k[SDL_SCANCODE_DOWN] || k[SDL_SCANCODE_S]) v |= 2;
	if (k[SDL_SCANCODE_LEFT] || k[SDL_SCANCODE_A]) v |= 4;
	if (k[SDL_SCANCODE_RIGHT] || k[SDL_SCANCODE_D]) v |= 8;
	if (k[SDL_SCANCODE_X] || k[SDL_SCANCODE_K]) v |= 16;         /* B */
	if (k[SDL_SCANCODE_C] || k[SDL_SCANCODE_L]) v |= 32;         /* C */
	if (k[SDL_SCANCODE_Z] || k[SDL_SCANCODE_J]) v |= 64;         /* A */
	if (k[SDL_SCANCODE_RETURN]) v |= 128;                         /* Start */
	pad_state[0] = v;
}
#else
void machine_sdl_key(int sym, int mod, int down) { (void)sym; (void)mod; (void)down; }
void machine_sdl_input(void) {}
#endif

int main(int argc, char **argv) { return host_main(argc, argv); }
