/* The Amiga as a machine for the recompiled game: its flags (--adf, the
 * disks), its start state (start.h, from the emulator snapshot), and how
 * the window's keys and mouse reach it: arrows/WASD and space/ctrl are the
 * joystick in port 2, every other key reaches the Amiga keyboard, a click
 * grabs the mouse and F11 lets it go, Esc quits when the mouse isn't
 * grabbed. --adf gives the floppy; given several, Cmd/Alt+1..9 swaps them
 * (in the order given), and --adf-in N says which one is in the drive at
 * the start. */
#ifdef USE_SDL
#include <SDL.h>
#endif
#include "host.h"
#include "start.h"
#include <string.h>

void amiga_init(void);
void amiga_set_reg(u32 off, u16 v);
void amiga_set_cia(int which, int r, u16 v);
void disk_set_adf(const char *path);
void disk_change(const char *path);
void disk_set_position(int cyl, int side, int motor);
void kbd_push(u8 code);
extern u16 joy1dat;
extern u8 joy_fire;
extern u8 mouse_x, mouse_y, mouse_left, mouse_right;

static const char *adfs[9];               /* --adf, repeatable: the game's disks; Cmd/Win+1..9 swaps */
static int nadfs;
static int adf_in = 1;                    /* --adf-in N: the Nth --adf is in the drive at the start */

void machine_init(void) { amiga_init(); frame_w = 320; frame_h = 256; }
void machine_audio_tag(void) {}

int machine_arg(int argc, char **argv, int *i) {
	if (!strcmp(argv[*i], "--adf") && *i + 1 < argc) { if (nadfs < 9) adfs[nadfs++] = argv[++*i]; return 1; }
	if (!strcmp(argv[*i], "--adf-in") && *i + 1 < argc) { adf_in = atoi(argv[++*i]); return 1; }
	return 0;
}
const char *machine_usage(void) { return "[--adf disk.adf]... [--adf-in N] "; }

void machine_start(void) {
	const char *adf = nadfs ? adfs[0] : NULL;
	if (nadfs > 1 && adf_in >= 1 && adf_in <= nadfs) adf = adfs[adf_in - 1];
	if (adf) disk_set_adf(adf);
	for (unsigned i = 0; i < sizeof start_regs / sizeof start_regs[0]; i++) amiga_set_reg(start_regs[i].off, start_regs[i].val);
	for (unsigned i = 0; i < sizeof start_cia / sizeof start_cia[0]; i++) amiga_set_cia(start_cia[i].cia, start_cia[i].reg, start_cia[i].val);
	cpu.d[0] = START_D0; cpu.d[1] = START_D1; cpu.d[2] = START_D2; cpu.d[3] = START_D3;
	cpu.d[4] = START_D4; cpu.d[5] = START_D5; cpu.d[6] = START_D6; cpu.d[7] = START_D7;
	cpu.a[0] = START_A0; cpu.a[1] = START_A1; cpu.a[2] = START_A2; cpu.a[3] = START_A3;
	cpu.a[4] = START_A4; cpu.a[5] = START_A5; cpu.a[6] = START_A6; cpu.a[7] = START_A7;
	disk_set_position(START_DF0_CYL, START_DF0_SIDE, START_DF0_MOTOR);
	mouse_x = (u8)START_JOY0DAT; mouse_y = (u8)(START_JOY0DAT >> 8);   /* the counters where the game last saw them */
	cpu.usp = START_USP; cpu.ssp = START_SSP;
	cpu.s = (START_SR >> 13) & 1; cpu.ipl = (START_SR >> 8) & 7;
	set_ccr(START_SR & 0xFF);
	cpu.pc = START_PC;
}

/* scripted input: "right left up down fire" (the joystick), "click" / "rclick"
 * (the mouse buttons), "mouse DX DY" (moved by that much at that frame),
 * "key HH" (an Amiga keycode tapped), "disk N" (the Nth --adf into the drive) */
void machine_apply_input(const char *words) {
	const char *d = strstr(words, "disk ");
	if (d) { int n = atoi(d + 5); if (n >= 1 && n <= nadfs) disk_change(adfs[n - 1]); }
	for (const char *k = words; (k = strstr(k, "key ")) != NULL; k += 4) {
		unsigned code = (unsigned)strtoul(k + 4, NULL, 16); kbd_push((u8)code); kbd_push((u8)(code | 0x80));
	}
	const char *m = strstr(words, "mouse");
	if (m) { int dx = 0, dy = 0; if (sscanf(m + 5, "%d %d", &dx, &dy) == 2) { mouse_x = (u8)(mouse_x + dx); mouse_y = (u8)(mouse_y + dy); } }
	mouse_left = has_word(words, "click");
	mouse_right = has_word(words, "rclick");
	int up = has_word(words, "up"), down = has_word(words, "down");
	int left = has_word(words, "left"), right = has_word(words, "right");
	u16 v = 0;
	if (right) v |= 0x0002;
	if (left) v |= 0x0200;
	if (down ^ right) v |= 0x0001;
	if (up ^ left) v |= 0x0100;
	joy1dat = v;
	joy_fire = has_word(words, "fire");
}

/* the same words for the state as it stands (the mouse as its motion since last asked) */
void machine_input_words(char *buf, size_t n) {
	static u8 last_x, last_y, first = 1;
	if (first) { last_x = mouse_x; last_y = mouse_y; first = 0; }
	int dx = (s8)(mouse_x - last_x), dy = (s8)(mouse_y - last_y);
	last_x = mouse_x; last_y = mouse_y;
	snprintf(buf, n, "%s%s%s%s%s%s%s", mouse_left ? " click" : "", mouse_right ? " rclick" : "", joy_fire ? " fire" : "",
	         (joy1dat & 0x0002) ? " right" : "", (joy1dat & 0x0200) ? " left" : "",
	         (((joy1dat >> 1) ^ joy1dat) & 1) ? " down" : "", (((joy1dat >> 9) ^ (joy1dat >> 8)) & 1) ? " up" : "");
	if (dx || dy) { size_t l = strlen(buf); snprintf(buf + l, n - l, " mouse %d %d", dx, dy); }
}

#ifdef USE_SDL
/* SDL scancodes -> Amiga raw keycodes (0xFF = no such key) */
static u8 amiga_key(SDL_Scancode sc) {
	static const struct { SDL_Scancode sc; u8 code; } map[] = {
		{ SDL_SCANCODE_GRAVE, 0x00 }, { SDL_SCANCODE_1, 0x01 }, { SDL_SCANCODE_2, 0x02 }, { SDL_SCANCODE_3, 0x03 }, { SDL_SCANCODE_4, 0x04 },
		{ SDL_SCANCODE_5, 0x05 }, { SDL_SCANCODE_6, 0x06 }, { SDL_SCANCODE_7, 0x07 }, { SDL_SCANCODE_8, 0x08 }, { SDL_SCANCODE_9, 0x09 },
		{ SDL_SCANCODE_0, 0x0A }, { SDL_SCANCODE_MINUS, 0x0B }, { SDL_SCANCODE_EQUALS, 0x0C }, { SDL_SCANCODE_BACKSLASH, 0x0D },
		{ SDL_SCANCODE_Q, 0x10 }, { SDL_SCANCODE_W, 0x11 }, { SDL_SCANCODE_E, 0x12 }, { SDL_SCANCODE_R, 0x13 }, { SDL_SCANCODE_T, 0x14 },
		{ SDL_SCANCODE_Y, 0x15 }, { SDL_SCANCODE_U, 0x16 }, { SDL_SCANCODE_I, 0x17 }, { SDL_SCANCODE_O, 0x18 }, { SDL_SCANCODE_P, 0x19 },
		{ SDL_SCANCODE_LEFTBRACKET, 0x1A }, { SDL_SCANCODE_RIGHTBRACKET, 0x1B },
		{ SDL_SCANCODE_A, 0x20 }, { SDL_SCANCODE_S, 0x21 }, { SDL_SCANCODE_D, 0x22 }, { SDL_SCANCODE_F, 0x23 }, { SDL_SCANCODE_G, 0x24 },
		{ SDL_SCANCODE_H, 0x25 }, { SDL_SCANCODE_J, 0x26 }, { SDL_SCANCODE_K, 0x27 }, { SDL_SCANCODE_L, 0x28 }, { SDL_SCANCODE_SEMICOLON, 0x29 },
		{ SDL_SCANCODE_APOSTROPHE, 0x2A },
		{ SDL_SCANCODE_Z, 0x31 }, { SDL_SCANCODE_X, 0x32 }, { SDL_SCANCODE_C, 0x33 }, { SDL_SCANCODE_V, 0x34 }, { SDL_SCANCODE_B, 0x35 },
		{ SDL_SCANCODE_N, 0x36 }, { SDL_SCANCODE_M, 0x37 }, { SDL_SCANCODE_COMMA, 0x38 }, { SDL_SCANCODE_PERIOD, 0x39 }, { SDL_SCANCODE_SLASH, 0x3A },
		{ SDL_SCANCODE_SPACE, 0x40 }, { SDL_SCANCODE_BACKSPACE, 0x41 }, { SDL_SCANCODE_TAB, 0x42 }, { SDL_SCANCODE_KP_ENTER, 0x43 },
		{ SDL_SCANCODE_RETURN, 0x44 }, { SDL_SCANCODE_ESCAPE, 0x45 }, { SDL_SCANCODE_DELETE, 0x46 },
		{ SDL_SCANCODE_UP, 0x4C }, { SDL_SCANCODE_DOWN, 0x4D }, { SDL_SCANCODE_RIGHT, 0x4E }, { SDL_SCANCODE_LEFT, 0x4F },
		{ SDL_SCANCODE_F1, 0x50 }, { SDL_SCANCODE_F2, 0x51 }, { SDL_SCANCODE_F3, 0x52 }, { SDL_SCANCODE_F4, 0x53 }, { SDL_SCANCODE_F5, 0x54 },
		{ SDL_SCANCODE_F6, 0x55 }, { SDL_SCANCODE_F7, 0x56 }, { SDL_SCANCODE_F8, 0x57 }, { SDL_SCANCODE_F9, 0x58 }, { SDL_SCANCODE_F10, 0x59 },
		{ SDL_SCANCODE_LSHIFT, 0x60 }, { SDL_SCANCODE_RSHIFT, 0x61 }, { SDL_SCANCODE_LCTRL, 0x63 }, { SDL_SCANCODE_LALT, 0x64 },
		{ SDL_SCANCODE_RALT, 0x65 }, { SDL_SCANCODE_LGUI, 0x66 }, { SDL_SCANCODE_RGUI, 0x67 }, { SDL_SCANCODE_HELP, 0x5F },
	};
	for (unsigned i = 0; i < sizeof map / sizeof map[0]; i++) if (map[i].sc == sc) return map[i].code;
	return 0xFF;
}

static int mouse_grabbed;
static int any_key_in;                    /* frames until a Return is tapped for a "press any key" after a disk swap */
static int centre_x, centre_y;            /* where the warped pointer reads back, in SDL's mouse space */
static int mouse_acc_x, mouse_acc_y;      /* host motion not yet turned into counts */
static int settle;                        /* frames after a grab during which the warp is still landing */
extern SDL_Window *host_sdl_window(void);

/* a key (down = 1 pressed, 0 released, -1 repeat) or a click (sym = -1) the host didn't take */
void machine_sdl_key(int sym, int mod, int down) {
	SDL_Window *win = host_sdl_window();
	if (sym == -1) {                      /* a click: grab the mouse, hide the cursor and keep it at the window's centre;
	                                         the motion is what moved it away from there each frame (SDL's relative mode
	                                         on macOS reports phantom motion while the pointer is still) */
		if (!mouse_grabbed) {
			int w, h; SDL_GetWindowSize(win, &w, &h);
			mouse_grabbed = 1; SDL_ShowCursor(SDL_DISABLE); SDL_SetWindowGrab(win, SDL_TRUE); SDL_WarpMouseInWindow(win, w / 2, h / 2);
			SDL_PumpEvents(); SDL_FlushEvent(SDL_MOUSEMOTION);
			SDL_GetMouseState(&centre_x, &centre_y);   /* (in whatever space SDL reports: the logical size scales it) */
			settle = 5;                       /* (macOS applies the warp a frame or two later: no deltas until it has) */
		}
		return;
	}
	if (down < 0) return;                 /* repeats */
	SDL_Scancode sc = (SDL_Scancode)sym;
	if (down && sc == SDL_SCANCODE_ESCAPE && !mouse_grabbed) host_exit();
	if (down && sc == SDL_SCANCODE_F11) { mouse_grabbed = 0; SDL_ShowCursor(SDL_ENABLE); SDL_SetWindowGrab(win, SDL_FALSE); return; }
	if (down && (mod & (KMOD_GUI | KMOD_ALT)) && sc >= SDL_SCANCODE_1 && sc <= SDL_SCANCODE_9) {
		int n = sc - SDL_SCANCODE_1;
		if (n < nadfs) { disk_change(adfs[n]); any_key_in = 25; fprintf(stderr, "recomp: disk %d (%s) in the drive\n", n + 1, adfs[n]);
		                 char w[16]; snprintf(w, sizeof w, "disk %d", n + 1); host_record_event(w); }
		return;
	}
	int control = sc == SDL_SCANCODE_UP || sc == SDL_SCANCODE_DOWN || sc == SDL_SCANCODE_LEFT || sc == SDL_SCANCODE_RIGHT
	           || sc == SDL_SCANCODE_SPACE || sc == SDL_SCANCODE_LCTRL || sc == SDL_SCANCODE_RCTRL;
	u8 code = amiga_key(sc);
	if (code != 0xFF && !control) {       /* (the controls aren't typed) */
		kbd_push(down ? code : (code | 0x80));
		if (down) { char w[16]; snprintf(w, sizeof w, "key %02X", code); host_record_event(w); }
	}
}

void machine_sdl_input(void) {
	SDL_Window *win = host_sdl_window();
	if (any_key_in && --any_key_in == 0) { kbd_push(0x44); kbd_push(0x44 | 0x80); host_record_event("key 44"); }   /* "press any key to continue": Return */
	if (mouse_grabbed) {                  /* the mouse: its motion since the last frame into Paula's 8-bit counters */
		int w, h, x, y; SDL_GetWindowSize(win, &w, &h);
		u32 b = SDL_GetMouseState(&x, &y);
		int dx = x - centre_x, dy = y - centre_y;
		if (settle) { settle--; centre_x = x; centre_y = y; dx = dy = 0; }
		else if (dx || dy) { SDL_WarpMouseInWindow(win, w / 2, h / 2); SDL_PumpEvents(); SDL_GetMouseState(&centre_x, &centre_y); }
		if (dx > 127) dx = 127; if (dx < -127) dx = -127; if (dy > 127) dy = 127; if (dy < -127) dy = -127;
		{ static int div = -1; if (div < 0) { const char *e = getenv("RECOMP_MOUSE_DIV"); div = e ? atoi(e) : 4; if (div < 1) div = 1; }
		  mouse_acc_x += dx; mouse_acc_y += dy;   /* RECOMP_MOUSE_DIV: host pixels per Amiga mouse count (4: a trackpad at 50 fps) */
		  mouse_x = (u8)(mouse_x + mouse_acc_x / div); mouse_acc_x %= div;
		  mouse_y = (u8)(mouse_y + mouse_acc_y / div); mouse_acc_y %= div; }
		const Uint8 *k = SDL_GetKeyboardState(NULL);
		/* (with the mouse grabbed, space or up also press the left button, down the right: a trackpad is awkward to hold) */
		mouse_left = (b & SDL_BUTTON(SDL_BUTTON_LEFT)) != 0 || k[SDL_SCANCODE_SPACE] || k[SDL_SCANCODE_UP];
		mouse_right = (b & SDL_BUTTON(SDL_BUTTON_RIGHT)) != 0 || k[SDL_SCANCODE_DOWN];
	}
	const Uint8 *k = SDL_GetKeyboardState(NULL);
	int up = k[SDL_SCANCODE_UP] || k[SDL_SCANCODE_W], down = k[SDL_SCANCODE_DOWN] || k[SDL_SCANCODE_S];
	int left = k[SDL_SCANCODE_LEFT] || k[SDL_SCANCODE_A], right = k[SDL_SCANCODE_RIGHT] || k[SDL_SCANCODE_D];
	u16 v = 0;
	if (right) v |= 0x0002;
	if (left) v |= 0x0200;
	if (down ^ right) v |= 0x0001;
	if (up ^ left) v |= 0x0100;
	joy1dat = v;
	joy_fire = k[SDL_SCANCODE_SPACE] || k[SDL_SCANCODE_LCTRL] || k[SDL_SCANCODE_RCTRL] || k[SDL_SCANCODE_X];
}
#else
void machine_sdl_key(int sym, int mod, int down) { (void)sym; (void)mod; (void)down; }
void machine_sdl_input(void) {}
#endif

int main(int argc, char **argv) { return host_main(argc, argv); }
