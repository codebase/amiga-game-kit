/* The recompiled game, run: load the memory image and the start state,
 * then let the translated code go. Headless by default: --frames N stops
 * after N frames and --png FILE writes the last picture; --play opens an
 * SDL window when built with -DUSE_SDL (arrows/WASD and space/ctrl are the
 * joystick, every key reaches the Amiga keyboard, a click grabs the mouse
 * and F11 lets it go, F12 toggles full speed, Esc quits when the mouse
 * isn't grabbed). --adf gives the floppy; given several, Cmd/Alt+1..9
 * swaps them (in the order given), and --adf-in N says which one is in
 * the drive at the start. */
#ifdef USE_SDL
#include <SDL.h>                          /* (before m68k.h: its A and D macros would mangle SDL's prototypes) */
#endif
#include "m68k.h"
#include "start.h"
#include <string.h>
#include <signal.h>
#include <unistd.h>

void run_translated(void);
void amiga_init(void);
void amiga_set_reg(u32 off, u16 v);
void amiga_set_cia(int which, int r, u16 v);
void disk_set_adf(const char *path);
void disk_change(const char *path);
static const char *adfs[9];               /* --adf, repeatable: the game's disks; Cmd/Win+1..9 swaps */
static int nadfs;
static int adf_in = 1;                    /* --adf-in N: the Nth --adf is in the drive at the start */
void disk_set_position(int cyl, int side, int motor);
extern u8 frame_rgb[320 * 256 * 3];
extern u32 frame_count;
extern int frame_done;
extern u16 joy1dat;
extern u8 joy_fire;
extern u8 mouse_x, mouse_y, mouse_left, mouse_right;
void kbd_push(u8 code);

static u32 max_frames;
int cycle_shift;                          /* --speed N: a 68000 N times faster than the A500's, as the game sees it */
static const char *png_path;
static const char *dump_path;             /* --dump: chip RAM at the end, to compare with the emulator's */
static const char *pngs_dir;              /* --pngs DIR: every frame as DIR/fNNNNN.png */
static FILE *wavf;                        /* --wav: the sound of the run, headless */
static u32 wav_bytes;
void audio_render(s16 *out, int n, int rate);

static void wav_frame(void) {
	static s16 buf[48000 / 50 * 2];
	audio_render(buf, 48000 / 50, 48000);
	fwrite(buf, 1, sizeof buf, wavf);
	wav_bytes += sizeof buf;
}

static void wav_close(void) {
	/* the header, now that the length is known */
	u32 rate = 48000, byterate = rate * 4, datalen = wav_bytes;
	u8 h[44] = { 'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'A', 'V', 'E', 'f', 'm', 't', ' ', 16, 0, 0, 0, 1, 0, 2, 0 };
	u32 riff = 36 + datalen;
	h[4] = (u8)riff; h[5] = (u8)(riff >> 8); h[6] = (u8)(riff >> 16); h[7] = (u8)(riff >> 24);
	h[24] = (u8)rate; h[25] = (u8)(rate >> 8); h[26] = (u8)(rate >> 16); h[27] = (u8)(rate >> 24);
	h[28] = (u8)byterate; h[29] = (u8)(byterate >> 8); h[30] = (u8)(byterate >> 16); h[31] = (u8)(byterate >> 24);
	h[32] = 4; h[34] = 16; memcpy(h + 36, "data", 4);
	h[40] = (u8)datalen; h[41] = (u8)(datalen >> 8); h[42] = (u8)(datalen >> 16); h[43] = (u8)(datalen >> 24);
	fseek(wavf, 0, SEEK_SET);
	fwrite(h, 1, 44, wavf);
	fclose(wavf);
}
static const char *trace_path;
static FILE *tracef;

static const char *image_dir = ".";       /* where memory.bin is: needed.txt goes next to it */

void trace_die(const char *why, u32 addr) {
	fprintf(stderr, "recomp: %s at $%06X (frame %u, pc $%06X)\n", why, addr, frame_count, cpu.pc);
	if (strstr(why, "never executed")) {   /* tell the next agk recomp where the game went */
		char path[1024];
		snprintf(path, sizeof path, "%s/needed.txt", image_dir);
		FILE *f = fopen(path, "a");
		if (f) { fprintf(f, "%06X\n", addr); fclose(f); }
		fprintf(stderr, "  (recorded in %s: agk recomp again and it will be translated)\n", path);
	}
	fprintf(stderr, "  D0-7:"); for (int i = 0; i < 8; i++) fprintf(stderr, " %08X", cpu.d[i]);
	fprintf(stderr, "\n  A0-7:"); for (int i = 0; i < 8; i++) fprintf(stderr, " %08X", cpu.a[i]);
	fprintf(stderr, "\n");
	exit(3);
}

static void write_png(const char *path) {
	/* a plain RGB PNG: stored (uncompressed) deflate blocks, no library needed */
	FILE *f = fopen(path, "wb");
	if (!f) return;
	static const u32 crc_table_init = 0;
	(void)crc_table_init;
	u32 crc_table[256];
	for (u32 n = 0; n < 256; n++) { u32 c = n; for (int k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1; crc_table[n] = c; }
	#define CRC(buf, len, c0) ({ u32 c_ = (c0); for (size_t i_ = 0; i_ < (len); i_++) c_ = crc_table[(c_ ^ (buf)[i_]) & 0xFF] ^ (c_ >> 8); c_; })
	#define BE32(v) (u8)((v) >> 24), (u8)((v) >> 16), (u8)((v) >> 8), (u8)(v)
	static const u8 sig[8] = { 0x89, 'P', 'N', 'G', 13, 10, 26, 10 };
	fwrite(sig, 1, 8, f);
	u8 ihdr[17] = { 'I', 'H', 'D', 'R', BE32(320), BE32(256), 8, 2, 0, 0, 0 };
	u8 len[4] = { BE32(13) }; fwrite(len, 1, 4, f); fwrite(ihdr, 1, 17, f);
	u32 c = CRC(ihdr, 17, 0xFFFFFFFFu) ^ 0xFFFFFFFFu; u8 cb[4] = { BE32(c) }; fwrite(cb, 1, 4, f);
	/* IDAT: zlib header, then stored blocks of up to 65535 bytes of the filtered rows */
	u32 raw_len = 256 * (1 + 320 * 3);
	u8 *raw = malloc(raw_len);
	for (int y = 0; y < 256; y++) { raw[y * 961] = 0; memcpy(raw + y * 961 + 1, frame_rgb + y * 960, 960); }
	u32 nblocks = (raw_len + 65534) / 65535;
	u32 idat_len = 2 + raw_len + nblocks * 5 + 4;
	u8 *idat = malloc(idat_len + 4);
	u8 *p = idat;
	memcpy(p, "IDAT", 4); p += 4;
	*p++ = 0x78; *p++ = 0x01;
	u32 a = 1, b = 0;
	for (u32 i = 0; i < raw_len; i++) { a = (a + raw[i]) % 65521; b = (b + a) % 65521; }
	for (u32 off = 0; off < raw_len; off += 65535) {
		u32 n = raw_len - off > 65535 ? 65535 : raw_len - off;
		*p++ = off + n >= raw_len; *p++ = (u8)n; *p++ = (u8)(n >> 8); *p++ = (u8)~n; *p++ = (u8)(~n >> 8);
		memcpy(p, raw + off, n); p += n;
	}
	u32 adler = (b << 16) | a;
	*p++ = (u8)(adler >> 24); *p++ = (u8)(adler >> 16); *p++ = (u8)(adler >> 8); *p++ = (u8)adler;
	u32 dl = (u32)(p - idat) - 4;
	u8 l2[4] = { BE32(dl) }; fwrite(l2, 1, 4, f); fwrite(idat, 1, dl + 4, f);
	c = CRC(idat, dl + 4, 0xFFFFFFFFu) ^ 0xFFFFFFFFu; u8 c2[4] = { BE32(c) }; fwrite(c2, 1, 4, f);
	u8 iend[4] = { 'I', 'E', 'N', 'D' }; u8 l3[4] = { 0, 0, 0, 0 }; fwrite(l3, 1, 4, f); fwrite(iend, 1, 4, f);
	c = CRC(iend, 4, 0xFFFFFFFFu) ^ 0xFFFFFFFFu; u8 c3[4] = { BE32(c) }; fwrite(c3, 1, 4, f);
	fclose(f);
	free(raw); free(idat);
}

#ifdef RECOMP_ITRACE
static FILE *itrace_f;
static long itrace_left;
void itrace(u32 addr) {
	if (!itrace_f) return;
	cpu.pc = addr;
	fprintf(itrace_f, "%06X", addr);
	for (int i = 0; i < 8; i++) fprintf(itrace_f, " %08X", cpu.d[i]);
	for (int i = 0; i < 8; i++) fprintf(itrace_f, " %08X", cpu.a[i]);
	fprintf(itrace_f, " %04X\n", get_sr());
	if (--itrace_left <= 0) { fclose(itrace_f); itrace_f = NULL; exit(0); }
}
#endif

#ifdef USE_SDL
/* the window: the frame scaled up, the keyboard as the joystick in port 2 */
static SDL_Window *win;
static SDL_Renderer *ren;
static SDL_Texture *tex;
static int fast;                          /* no pacing: as fast as the host goes */
static u64 next_present;                  /* when the next frame is due (performance counter) */
static int scale = 3;
static SDL_AudioDeviceID adev;
#define AUDIO_RATE 48000
void audio_render(s16 *out, int n, int rate);

static void sdl_open(void) {
	if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO) != 0) { fprintf(stderr, "recomp: SDL: %s\n", SDL_GetError()); exit(1); }
	SDL_AudioSpec want = { 0 }, have;
	want.freq = AUDIO_RATE; want.format = AUDIO_S16SYS; want.channels = 2; want.samples = 1024;
	adev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
	if (adev) SDL_PauseAudioDevice(adev, 0); else fprintf(stderr, "recomp: no audio: %s\n", SDL_GetError());
	win = SDL_CreateWindow("agk recomp", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 320 * scale, 256 * scale, SDL_WINDOW_RESIZABLE);
	ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
	if (!ren) ren = SDL_CreateRenderer(win, -1, 0);
	SDL_RenderSetLogicalSize(ren, 320 * 4, 256 * 4);   /* keeps the 5:4 picture whatever the window */
	tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_RGB24, SDL_TEXTUREACCESS_STREAMING, 320, 256);
	next_present = SDL_GetPerformanceCounter();
}

/* the keyboard -> JOY1DAT and the fire button: JOY1DAT's bit 1 is right, bit 9
 * left, bit 0 ^ bit 1 down, bit 8 ^ bit 9 up */
/* SDL scancodes -> Amiga raw keycodes (0 = no such key) */
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

static void sdl_input(void) {
	SDL_Event e;
	while (SDL_PollEvent(&e)) {
		if (e.type == SDL_QUIT) exit(0);
		if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_ESCAPE && !mouse_grabbed) exit(0);
		if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_F12) { fast = !fast; next_present = SDL_GetPerformanceCounter(); }
		if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_F11) { mouse_grabbed = 0; SDL_SetRelativeMouseMode(SDL_FALSE); }
		if (e.type == SDL_KEYDOWN && (e.key.keysym.mod & (KMOD_GUI | KMOD_ALT)) && e.key.keysym.sym >= SDLK_1 && e.key.keysym.sym <= SDLK_9) {
			int n = e.key.keysym.sym - SDLK_1;
			if (n < nadfs) { disk_change(adfs[n]); fprintf(stderr, "recomp: disk %d (%s) in the drive\n", n + 1, adfs[n]); }
			continue;
		}
		if ((e.type == SDL_KEYDOWN || e.type == SDL_KEYUP) && !e.key.repeat) {
			u8 code = amiga_key(e.key.keysym.scancode);
			if (code != 0xFF) kbd_push(e.type == SDL_KEYUP ? (code | 0x80) : code);
		}
		if (e.type == SDL_MOUSEBUTTONDOWN && !mouse_grabbed) { mouse_grabbed = 1; SDL_SetRelativeMouseMode(SDL_TRUE); }
	}
	if (mouse_grabbed) {                  /* the mouse: relative motion into Paula's 8-bit counters */
		int dx, dy;
		u32 b = SDL_GetRelativeMouseState(&dx, &dy);
		mouse_x = (u8)(mouse_x + dx / 2); mouse_y = (u8)(mouse_y + dy / 2);
		mouse_left = (b & SDL_BUTTON(SDL_BUTTON_LEFT)) != 0;
		mouse_right = (b & SDL_BUTTON(SDL_BUTTON_RIGHT)) != 0;
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

static void sdl_present(void) {
	if (adev) {                           /* a frame's worth of Paula, unless we're ahead (full speed) */
		static s16 buf[AUDIO_RATE / 50 * 2];
		if (SDL_GetQueuedAudioSize(adev) < sizeof buf * 4) {
			audio_render(buf, AUDIO_RATE / 50, AUDIO_RATE);
			SDL_QueueAudio(adev, buf, sizeof buf);
		}
	}
	SDL_UpdateTexture(tex, NULL, frame_rgb, 320 * 3);
	SDL_RenderClear(ren);
	SDL_RenderCopy(ren, tex, NULL, NULL);
	SDL_RenderPresent(ren);
	if (!fast) {                          /* 50 frames a second, the PAL Amiga's pace */
		u64 hz = SDL_GetPerformanceFrequency();
		next_present += hz / 50;
		u64 now = SDL_GetPerformanceCounter();
		if (next_present > now + hz / 2) next_present = now;             /* (we fell far behind: skip, don't catch up) */
		while ((now = SDL_GetPerformanceCounter()) < next_present) {
			u64 left = next_present - now;
			if (left > hz / 500) SDL_Delay((Uint32)(left * 1000 / hz) - 1);
		}
	}
}
#endif

/* scripted input: lines of "FRAME right left up down fire" (any subset of
 * the words), the state from that frame on, plus "mouse DX DY" (moved by
 * that much at that frame) and "click" (the left button held); the same
 * words as a scenario's, so an emulator run can be replayed natively */
static FILE *inputf;
static long input_frame = -1;
static char input_words[64];

static void apply_input(const char *words) {
	const char *d = strstr(words, "disk ");     /* "disk N": the Nth --adf goes in the drive */
	if (d) { int n = atoi(d + 5); if (n >= 1 && n <= nadfs) disk_change(adfs[n - 1]); }
	const char *k = strstr(words, "key ");      /* "key HH": an Amiga keycode tapped */
	if (k) { unsigned code = (unsigned)strtoul(k + 4, NULL, 16); kbd_push((u8)code); kbd_push((u8)(code | 0x80)); }
	const char *m = strstr(words, "mouse");
	if (m) { int dx = 0, dy = 0; if (sscanf(m + 5, "%d %d", &dx, &dy) == 2) { mouse_x = (u8)(mouse_x + dx); mouse_y = (u8)(mouse_y + dy); } }
	mouse_left = strstr(words, "click") != NULL;
	int up = strstr(words, "up") != NULL, down = strstr(words, "down") != NULL;
	int left = strstr(words, "left") != NULL, right = strstr(words, "right") != NULL;
	u16 v = 0;
	if (right) v |= 0x0002;
	if (left) v |= 0x0200;
	if (down ^ right) v |= 0x0001;
	if (up ^ left) v |= 0x0100;
	joy1dat = v;
	joy_fire = strstr(words, "fire") != NULL;
}

static void scripted_input(void) {
	while (inputf && input_frame <= (long)frame_count) {
		if (input_frame >= 0) apply_input(input_words);
		char line[80];
		if (!fgets(line, sizeof line, inputf)) { fclose(inputf); inputf = NULL; break; }
		input_words[0] = 0;
		if (sscanf(line, "%ld %63[^\n]", &input_frame, input_words) < 1) input_frame = -1;
	}
}

/* called by the runtime after every rendered frame */
void frame_hook(void) {
	scripted_input();
	if (wavf) wav_frame();
	if (pngs_dir) { char path[1024]; snprintf(path, sizeof path, "%s/f%05u.png", pngs_dir, frame_count); write_png(path); }
#ifdef USE_SDL
	if (win) { if (!inputf) sdl_input(); sdl_present(); }
#endif
	if (tracef) {
		extern u32 stat_blits, stat_cop1lc;
		fprintf(tracef, "frame %u pc %06X blits %u cop1lc %u", frame_count, cpu.pc, stat_blits, stat_cop1lc);
		stat_blits = stat_cop1lc = 0;
		for (int i = 0; i < 8; i++) fprintf(tracef, " D%d=%08X", i, cpu.d[i]);
		for (int i = 0; i < 8; i++) fprintf(tracef, " A%d=%08X", i, cpu.a[i]);
		fprintf(tracef, "\n");
	}
	if (max_frames && frame_count >= max_frames) {
		if (png_path) write_png(png_path);
		if (wavf) wav_close();
		if (dump_path) { FILE *f = fopen(dump_path, "wb"); if (f) { fwrite(mem, 1, 0x80000, f); fclose(f); } }
		printf("recomp: %u frames run, pc $%06X\n", frame_count, cpu.pc);
		exit(0);
	}
}

/* --timeout: the game is stuck somewhere; say where (the pc is exact in the
 * trace build, otherwise the last dispatch or interrupt) */
static void on_timeout(int sig) {
	(void)sig;
	trace_die("timeout: the game is looping (the pc is exact only in the --itrace build)", cpu.pc);
}

static void load_memory(const char *path) {
	FILE *f = fopen(path, "rb");
	if (!f) { fprintf(stderr, "recomp: can't open %s\n", path); exit(1); }
	u8 hdr[8];
	while (fread(hdr, 1, 8, f) == 8) {
		u32 base = hdr[0] | hdr[1] << 8 | hdr[2] << 16 | (u32)hdr[3] << 24;
		u32 len = hdr[4] | hdr[5] << 8 | hdr[6] << 16 | (u32)hdr[7] << 24;
		if (base + len > MEM_SIZE || fread(mem + base, 1, len, f) != len) { fprintf(stderr, "recomp: bad memory image\n"); exit(1); }
	}
	fclose(f);
}

int main(int argc, char **argv) {
	const char *image = "memory.bin", *adf = NULL;
	int play = 0, fast_start = 0;
	(void)fast_start;
	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--frames") && i + 1 < argc) max_frames = (u32)atoi(argv[++i]);
		else if (!strcmp(argv[i], "--png") && i + 1 < argc) png_path = argv[++i];
		else if (!strcmp(argv[i], "--dump") && i + 1 < argc) dump_path = argv[++i];
		else if (!strcmp(argv[i], "--pngs") && i + 1 < argc) pngs_dir = argv[++i];
		else if (!strcmp(argv[i], "--speed") && i + 1 < argc) { int sp = atoi(argv[++i]); cycle_shift = sp >= 32 ? 5 : sp >= 16 ? 4 : sp >= 8 ? 3 : sp >= 4 ? 2 : sp >= 2 ? 1 : 0; }
		else if (!strcmp(argv[i], "--wav") && i + 1 < argc) { wavf = fopen(argv[++i], "wb"); if (wavf) { u8 z[44] = { 0 }; fwrite(z, 1, 44, wavf); } }
		else if (!strcmp(argv[i], "--adf") && i + 1 < argc) { adf = argv[++i]; if (nadfs < 9) adfs[nadfs++] = adf; if (nadfs > 1) adf = adfs[0]; }
		else if (!strcmp(argv[i], "--adf-in") && i + 1 < argc) adf_in = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--memory") && i + 1 < argc) image = argv[++i];
		else if (!strcmp(argv[i], "--trace") && i + 1 < argc) trace_path = argv[++i];
		else if (!strcmp(argv[i], "--play")) play = 1;
		else if (!strcmp(argv[i], "--fast")) fast_start = 1;
		else if (!strcmp(argv[i], "--timeout") && i + 1 < argc) { signal(SIGALRM, on_timeout); alarm((unsigned)atoi(argv[++i])); }
		else if (!strcmp(argv[i], "--input") && i + 1 < argc) { inputf = fopen(argv[++i], "r"); if (!inputf) { fprintf(stderr, "recomp: can't open %s\n", argv[i]); return 2; } }
#ifdef RECOMP_ITRACE
		else if (!strcmp(argv[i], "--itrace") && i + 2 < argc) { itrace_f = fopen(argv[++i], "w"); itrace_left = atol(argv[++i]); }
#endif
		else { fprintf(stderr, "usage: game [--memory memory.bin] [--adf disk.adf]... [--adf-in N] [--frames N] [--png out.png] [--dump chip.bin] [--wav out.wav] [--pngs DIR] [--speed 1|2|4|8|16|32] [--trace file] [--input keys.txt] [--timeout SEC] [--play [--fast]]\n"); return 2; }
	}
	amiga_init();
	load_memory(image);
	{ static char d[1024]; const char *slash = strrchr(image, '/'); if (slash) { snprintf(d, sizeof d, "%.*s", (int)(slash - image), image); image_dir = d; } }
	if (nadfs > 1 && adf_in >= 1 && adf_in <= nadfs) adf = adfs[adf_in - 1];
	if (adf) disk_set_adf(adf);
	if (trace_path) tracef = fopen(trace_path, "w");
	for (unsigned i = 0; i < sizeof start_regs / sizeof start_regs[0]; i++) amiga_set_reg(start_regs[i].off, start_regs[i].val);
	for (unsigned i = 0; i < sizeof start_cia / sizeof start_cia[0]; i++) amiga_set_cia(start_cia[i].cia, start_cia[i].reg, start_cia[i].val);
	cpu.d[0] = START_D0; cpu.d[1] = START_D1; cpu.d[2] = START_D2; cpu.d[3] = START_D3;
	cpu.d[4] = START_D4; cpu.d[5] = START_D5; cpu.d[6] = START_D6; cpu.d[7] = START_D7;
	cpu.a[0] = START_A0; cpu.a[1] = START_A1; cpu.a[2] = START_A2; cpu.a[3] = START_A3;
	cpu.a[4] = START_A4; cpu.a[5] = START_A5; cpu.a[6] = START_A6; cpu.a[7] = START_A7;
	disk_set_position(START_DF0_CYL, START_DF0_SIDE, START_DF0_MOTOR);
	cpu.usp = START_USP; cpu.ssp = START_SSP;
	cpu.s = (START_SR >> 13) & 1; cpu.ipl = (START_SR >> 8) & 7;
	set_ccr(START_SR & 0xFF);
	cpu.pc = START_PC;
#ifdef USE_SDL
	if (play) { sdl_open(); fast = fast_start; }
#else
	if (play) { fprintf(stderr, "recomp: this build has no window (built without SDL2: brew install sdl2 / apt install libsdl2-dev, then agk recomp again)\n"); return 2; }
#endif
	run_translated();
	return 0;
}
