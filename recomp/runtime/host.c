/* The recompiled game's host: the common main. Load the memory image and the
 * start state, then let the translated code go. Headless by default: --frames N
 * stops after N frames and --png FILE writes the last picture; --play opens an
 * SDL window when built with -DUSE_SDL. --input replays a script of per-frame
 * words, --record writes one. The machine (recomp/amiga, recomp/md) supplies
 * its chips, its flags and how the window's keys reach the game. */
#ifdef USE_SDL
#include <SDL.h>                          /* (before m68k.h: its A and D macros would mangle SDL's prototypes) */
#endif
#include "host.h"
#include "start.h"
#include <string.h>
#include <time.h>
#include <signal.h>
#include <unistd.h>

void run_translated(void);

u8 frame_rgb[FRAME_MAX_W * FRAME_MAX_H * 3];
int frame_w = 320, frame_h = 256;
int cycle_shift;
int host_fast;
const char *image_dir = ".";

static u32 max_frames;
static FILE *recf;                        /* --record FILE: the live input as --input lines (a session replayed headless is the same run) */
static char rec_events[64];               /* this frame's key taps and disk swaps, for the record */
static const char *png_path;
static const char *dump_path;             /* --dump: the first 512K of memory at the end, to compare with the emulator's */
static const char *pngs_dir;              /* --pngs DIR: every frame as DIR/fNNNNN.png */
static FILE *wavf;                        /* --wav: the sound of the run, headless */
static s16 audio_buf[48000 / 50 * 2];     /* this frame's audio (rendered every frame, see frame_hook) */
static u32 wav_bytes;
static int audio_frames = 48000 / 60;     /* per video frame: set from the machine's refresh */
static int refresh_hz = 50;
static const char *coverage_path;         /* --coverage FILE: every pc the interpreter executed, vAmiga's coverage.bin format */
u8 *coverage_bits;
u8 *entry_bits;
static const char *analysis_dir;          /* --analysis DIR: coverage.bin, entries.bin and the machine's own findings, accumulated over runs */
const char *host_analysis_dir(void) { return analysis_dir; }
static u8 *bits_load(const char *dir, const char *name) {   /* a 2 MB bitmap, starting from the file if it's there (runs add up) */
	u8 *b = calloc(1u << 21, 1);
	char path[1024]; snprintf(path, sizeof path, "%s/%s", dir, name);
	FILE *f = fopen(path, "rb");
	if (f) { size_t n = fread(b, 1, 1u << 21, f); (void)n; fclose(f); }
	return b;
}
static void bits_save(const char *dir, const char *name, const u8 *b) {
	char path[1024]; snprintf(path, sizeof path, "%s/%s", dir, name);
	FILE *f = fopen(path, "wb");
	if (f) { fwrite(b, 1, 1u << 21, f); fclose(f); }
}

static void wav_frame(void) {
	fwrite(audio_buf, 1, (size_t)audio_frames * 4, wavf);
	wav_bytes += (u32)audio_frames * 4;
}

static void wav_close(void) {
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
static FILE *inputf;                      /* --input: the script being replayed */

/* the interpreter took over at this address: remember it for the next agk
 * recomp (needed.txt next to memory.bin), so the code gets translated */
extern u64 interp_count;
static u32 interp_entries;
void note_interpreted(u32 pc) {
	static u32 seen[4096];
	static int nseen;
	for (int i = 0; i < nseen; i++) if (seen[i] == pc) return;
	if (nseen < 4096) seen[nseen++] = pc;
	interp_entries++;
	char path[1024];
	snprintf(path, sizeof path, "%s/needed.txt", image_dir);
	FILE *f = fopen(path, "a");
	if (f) { fprintf(f, "%06X\n", pc); fclose(f); }
}

/* the game rewrote a translated instruction (not just its immediate): its block
 * runs interpreted from now on; remember it (patched.txt next to memory.bin) so
 * the next agk recomp checks that instruction's opcode as it runs and keeps the
 * block translated */
static u32 patched_entries;
void note_patched(u32 addr, u16 was, u16 now) {
	static u32 seen[1024];
	static int nseen;
	for (int i = 0; i < nseen; i++) if (seen[i] == addr) return;
	if (nseen < 1024) seen[nseen++] = addr;
	patched_entries++;
	if (patched_entries <= 8) fprintf(stderr, "recomp: the game rewrote the instruction at $%06X (translated as %04X, now %04X): its block runs interpreted; "
	                                       "it's in patched.txt: agk recomp again to translate it with a check\n", addr, was, now);
	char path[1024];
	snprintf(path, sizeof path, "%s/patched.txt", image_dir);
	FILE *f = fopen(path, "a");
	if (f) { fprintf(f, "%06X\n", addr); fclose(f); }
}

static void interp_report(void) {
	if (interp_count)
		fprintf(stderr, "recomp: %llu instructions interpreted (code the translation didn't have, entered at %u addresses; "
		        "they're in needed.txt: agk recomp again to translate them)\n", (unsigned long long)interp_count, interp_entries);
}

static void coverage_save(void) {
	if (analysis_dir) {
		bits_save(analysis_dir, "coverage.bin", coverage_bits); bits_save(analysis_dir, "entries.bin", entry_bits);
		machine_at_exit();
		fprintf(stderr, "recomp: analysis written to %s (coverage.bin, entries.bin, the machine's notes)\n", analysis_dir);
		return;
	}
	if (!coverage_path || !coverage_bits) return;
	FILE *f = fopen(coverage_path, "wb");
	if (!f) { fprintf(stderr, "recomp: can't write %s\n", coverage_path); return; }
	fwrite(coverage_bits, 1, 1u << 21, f);
	fclose(f);
	fprintf(stderr, "recomp: coverage written to %s (agk recomp reads it)\n", coverage_path);
}

void host_exit(void) {
	coverage_save();
	interp_report();
	exit(0);
}

void trace_die(const char *why, u32 addr) {
	fprintf(stderr, "recomp: %s at $%06X (frame %u, pc $%06X)\n", why, addr, frame_count, cpu.pc);
	fprintf(stderr, "  D0-7:"); for (int i = 0; i < 8; i++) fprintf(stderr, " %08X", cpu.d[i]);
	fprintf(stderr, "\n  A0-7:"); for (int i = 0; i < 8; i++) fprintf(stderr, " %08X", cpu.a[i]);
	fprintf(stderr, "\n");
	coverage_save();
	if (!analysis_dir) machine_at_exit();
	exit(3);
}

static void write_png(const char *path) {
	/* a plain RGB PNG: stored (uncompressed) deflate blocks, no library needed */
	FILE *f = fopen(path, "wb");
	if (!f) return;
	u32 crc_table[256];
	for (u32 n = 0; n < 256; n++) { u32 c = n; for (int k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1; crc_table[n] = c; }
	#define CRC(buf, len, c0) ({ u32 c_ = (c0); for (size_t i_ = 0; i_ < (len); i_++) c_ = crc_table[(c_ ^ (buf)[i_]) & 0xFF] ^ (c_ >> 8); c_; })
	#define BE32(v) (u8)((v) >> 24), (u8)((v) >> 16), (u8)((v) >> 8), (u8)(v)
	static const u8 sig[8] = { 0x89, 'P', 'N', 'G', 13, 10, 26, 10 };
	fwrite(sig, 1, 8, f);
	u32 W = (u32)frame_w, H = (u32)frame_h;
	u8 ihdr[17] = { 'I', 'H', 'D', 'R', BE32(W), BE32(H), 8, 2, 0, 0, 0 };
	u8 len[4] = { BE32(13) }; fwrite(len, 1, 4, f); fwrite(ihdr, 1, 17, f);
	u32 c = CRC(ihdr, 17, 0xFFFFFFFFu) ^ 0xFFFFFFFFu; u8 cb[4] = { BE32(c) }; fwrite(cb, 1, 4, f);
	/* IDAT: zlib header, then stored blocks of up to 65535 bytes of the filtered rows */
	u32 row = 1 + W * 3, raw_len = H * row;
	u8 *raw = malloc(raw_len);
	for (u32 y = 0; y < H; y++) { raw[y * row] = 0; memcpy(raw + y * row + 1, frame_rgb + y * W * 3, W * 3); }
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
	{ static long from = -1; if (from < 0) { const char *e = getenv("RECOMP_ITRACE_FROM"); from = e ? atol(e) : 0; }   /* RECOMP_ITRACE_FROM=N: nothing before frame N */
	  if ((long)frame_count < from) return; }
	cpu.pc = addr;
	{ static long peek_pc = -1; static u32 peek_addr, peek_n;   /* RECOMP_ITRACE_PEEK=PC:ADDR:N: N longs at ADDR whenever PC runs */
	  if (peek_pc < 0) { const char *e = getenv("RECOMP_ITRACE_PEEK"); peek_pc = 0; if (e) { char *q; peek_pc = strtol(e, &q, 16); if (*q == ':') { peek_addr = (u32)strtoul(q + 1, &q, 16); if (*q == ':') peek_n = (u32)strtoul(q + 1, NULL, 10); } } }
	  if (peek_pc && addr == (u32)peek_pc) { fprintf(itrace_f, "peek frame %u pc %06X a3 %08X:", frame_count, addr, cpu.a[3]); for (u32 i = 0; i < peek_n; i++) fprintf(itrace_f, " %08X", rd32(peek_addr + 4 * i)); fputc('\n', itrace_f); } }
	fprintf(itrace_f, "%06X", addr);
	for (int i = 0; i < 8; i++) fprintf(itrace_f, " %08X", cpu.d[i]);
	for (int i = 0; i < 8; i++) fprintf(itrace_f, " %08X", cpu.a[i]);
	fprintf(itrace_f, " %04X", get_sr());
	{ static int cyc = -1; if (cyc < 0) cyc = getenv("RECOMP_ITRACE_CYCLES") != NULL; if (cyc) fprintf(itrace_f, " %llu", (unsigned long long)cpu.cycles); }
	fputc('\n', itrace_f);
	if (--itrace_left <= 0) { fclose(itrace_f); itrace_f = NULL; exit(0); }
}
#endif

#ifdef USE_SDL
/* the window: the frame scaled up at a whole-number factor */
static SDL_Window *win;
static SDL_Renderer *ren;
static SDL_Texture *tex;
static u64 next_present;                  /* when the next frame is due (performance counter) */
static int scale = 3;
static SDL_AudioDeviceID adev;
#define AUDIO_RATE 48000

static void sdl_open(void) {
	if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO) != 0) { fprintf(stderr, "recomp: SDL: %s\n", SDL_GetError()); exit(1); }
	SDL_AudioSpec want = { 0 }, have;
	want.freq = AUDIO_RATE; want.format = AUDIO_S16SYS; want.channels = 2; want.samples = 1024;
	adev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
	if (adev) SDL_PauseAudioDevice(adev, 0); else fprintf(stderr, "recomp: no audio: %s\n", SDL_GetError());
	win = SDL_CreateWindow("agk recomp", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, frame_w * scale, frame_h * scale, SDL_WINDOW_RESIZABLE);
	ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
	if (!ren) ren = SDL_CreateRenderer(win, -1, 0);
	/* the picture at a whole-number scale: with a fractional one, rows alternate
	   between n and n+1 pixels tall and anything that moves vertically shimmers */
	SDL_RenderSetLogicalSize(ren, frame_w, frame_h);
	SDL_RenderSetIntegerScale(ren, SDL_TRUE);
	tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_RGB24, SDL_TEXTUREACCESS_STREAMING, frame_w, frame_h);
	next_present = SDL_GetPerformanceCounter();
}

static void sdl_present(void) {
	if (adev) {                           /* the frame's audio, queued unless we're far ahead (full speed) */
		if (SDL_GetQueuedAudioSize(adev) < (u32)audio_frames * 4 * 4) SDL_QueueAudio(adev, audio_buf, (u32)audio_frames * 4);
	}
	SDL_UpdateTexture(tex, NULL, frame_rgb, frame_w * 3);
	SDL_RenderClear(ren);
	SDL_RenderCopy(ren, tex, NULL, NULL);
	SDL_RenderPresent(ren);
	if (!host_fast) {                     /* the machine's refresh rate */
		u64 hz = SDL_GetPerformanceFrequency();
		next_present += hz / (u64)refresh_hz;
		u64 now = SDL_GetPerformanceCounter();
		if (next_present > now + hz / 2) next_present = now;             /* (we fell far behind: skip, don't catch up) */
		while ((now = SDL_GetPerformanceCounter()) < next_present) {
			u64 left = next_present - now;
			if (left > hz / 500) SDL_Delay((Uint32)(left * 1000 / hz) - 1);
		}
	}
}

/* the host's own keys: Esc quits (the machine may veto, e.g. while the mouse is grabbed), F12 full speed */
static void sdl_events(void) {
	SDL_Event e;
	while (SDL_PollEvent(&e)) {
		if (e.type == SDL_QUIT) host_exit();
		if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_F12 && !e.key.repeat) { host_fast = !host_fast; next_present = SDL_GetPerformanceCounter(); continue; }
		if (inputf) { if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_ESCAPE) host_exit(); continue; }   /* (a replay: the keys don't reach the game) */
		if (e.type == SDL_KEYDOWN || e.type == SDL_KEYUP) machine_sdl_key(e.key.keysym.scancode, e.key.keysym.mod, e.type == SDL_KEYDOWN && !e.key.repeat ? 1 : e.type == SDL_KEYUP ? 0 : -1);
		else if (e.type == SDL_MOUSEBUTTONDOWN) machine_sdl_key(-1, 0, 1);   /* (a click: the Amiga grabs the mouse on it) */
	}
}
SDL_Window *host_sdl_window(void) { return win; }
int host_window_open(void) { return win != NULL; }
u64 host_perf_counter(void) { return SDL_GetPerformanceCounter(); }
#else
int host_window_open(void) { return 0; }
u64 host_perf_counter(void) { return 0; }
#endif

/* --record: a line whenever the input differs from the last line's */
void host_record_event(const char *words) {
	if (recf) snprintf(rec_events + strlen(rec_events), sizeof rec_events - strlen(rec_events), " %s", words);
}
static void record_frame(void) {
	static char last[256];
	char now[256];
	machine_input_words(now, sizeof now);
	if (!rec_events[0] && !strcmp(now, last)) return;
	fprintf(recf, "%u%s%s\n", frame_count, now, rec_events);
	fflush(recf);
	snprintf(last, sizeof last, "%s", now);
	rec_events[0] = 0;
}

/* scripted input: lines of "FRAME words...", the machine's words (the state
 * from that frame on, plus events like "key HH"); what --record writes */
static long input_frame = -1;
static char input_words[128];

int has_word(const char *words, const char *w) {
	size_t n = strlen(w);
	for (const char *p = words; (p = strstr(p, w)) != NULL; p += n)
		if ((p == words || p[-1] == ' ') && (p[n] == 0 || p[n] == ' ' || p[n] == '\n')) return 1;
	return 0;
}

static void scripted_input(void) {
	while (inputf && input_frame <= (long)frame_count) {
		if (input_frame >= 0) machine_apply_input(input_words);
		char line[160];
		if (!fgets(line, sizeof line, inputf)) { fclose(inputf); inputf = NULL; break; }
		input_words[0] = 0;
		if (sscanf(line, "%ld %127[^\n]", &input_frame, input_words) < 1) input_frame = -1;
	}
}

/* The sound is rendered every frame whatever the build is doing with it, so
 * the chip state it leaves doesn't depend on whether a window, a wav file or
 * nothing is listening: the same run is the same */
void frame_hook(void) {
	scripted_input();
	{ static long from = -1; if (from < 0) { const char *e = getenv("RECOMP_INTERP_FROM"); from = e ? atol(e) : 0; }   /* RECOMP_INTERP_FROM=N: the interpreter takes over at frame N (bisecting a translation bug) */
	  if (from > 0 && (long)frame_count >= from) { setenv("RECOMP_INTERP", "1", 1); setenv("RECOMP_NO_HOOKS", "1", 1); } }
	{ static long from = -1; if (from < 0) { const char *e = getenv("RECOMP_NO_HOOKS_FROM"); from = e ? atol(e) : 0; }   /* RECOMP_NO_HOOKS_FROM=N: hooks that honour RECOMP_NO_HOOKS stop at frame N */
	  if (from > 0 && (long)frame_count >= from) setenv("RECOMP_NO_HOOKS_ACTIVE", "1", 1); }
	machine_audio_tag();
	audio_render(audio_buf, audio_frames, 48000);
	{ static int nft = -1; static u32 ft[16]; const char *w = getenv("RECOMP_WORD_TRACE");   /* RECOMP_WORD_TRACE=hex[,hex...]: those words of memory, every frame */
	  if (nft < 0) { nft = 0; while (w && *w && nft < 16) { ft[nft++] = (u32)strtoul(w, (char **)&w, 16); if (*w == ',') w++; else break; } }
	  if (nft) { fprintf(stderr, "word: frame %u", frame_count); for (int i = 0; i < nft; i++) fprintf(stderr, " $%06X=%d", ft[i], (s16)rd16(ft[i])); fputc('\n', stderr); } }
	if (wavf) wav_frame();
	if (pngs_dir) { char path[1024]; snprintf(path, sizeof path, "%s/f%05u.png", pngs_dir, frame_count); write_png(path); }
#ifdef USE_SDL
	if (win) {
		sdl_events();
		if (!inputf) { machine_sdl_input(); if (recf) record_frame(); }
		sdl_present();
	}
#endif
	if (tracef) {
		fprintf(tracef, "frame %u pc %06X", frame_count, cpu.pc);
		for (int i = 0; i < 8; i++) fprintf(tracef, " D%d=%08X", i, cpu.d[i]);
		for (int i = 0; i < 8; i++) fprintf(tracef, " A%d=%08X", i, cpu.a[i]);
		fprintf(tracef, "\n");
	}
	if (max_frames && frame_count >= max_frames) {
		if (png_path) write_png(png_path);
		if (wavf) wav_close();
		if (dump_path) { FILE *f = fopen(dump_path, "wb"); const char *e = getenv("RECOMP_DUMP_AT");   /* RECOMP_DUMP_AT=HEX: 64K from there instead (a console's RAM) */
			if (f) { if (e) fwrite(mem + (strtoul(e, NULL, 16) & 0xFFFFFF), 1, 0x10000, f); else fwrite(mem, 1, 0x80000, f); fclose(f); } }
		coverage_save();
		interp_report();
		if (!analysis_dir) machine_at_exit();
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

int host_main(int argc, char **argv) {
	const char *image = "memory.bin";
	int play = 0;
	refresh_hz = MACHINE_HZ;
	audio_frames = 48000 / MACHINE_HZ;
	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--frames") && i + 1 < argc) max_frames = (u32)atoi(argv[++i]);
		else if (!strcmp(argv[i], "--png") && i + 1 < argc) png_path = argv[++i];
		else if (!strcmp(argv[i], "--dump") && i + 1 < argc) dump_path = argv[++i];
		else if (!strcmp(argv[i], "--pngs") && i + 1 < argc) pngs_dir = argv[++i];
		else if (!strcmp(argv[i], "--speed") && i + 1 < argc) { int sp = atoi(argv[++i]); cycle_shift = sp >= 32 ? 5 : sp >= 16 ? 4 : sp >= 8 ? 3 : sp >= 4 ? 2 : sp >= 2 ? 1 : 0; }
		else if (!strcmp(argv[i], "--wav") && i + 1 < argc) { wavf = fopen(argv[++i], "wb"); if (wavf) { u8 z[44] = { 0 }; fwrite(z, 1, 44, wavf); } }
		else if (!strcmp(argv[i], "--memory") && i + 1 < argc) image = argv[++i];
		else if (!strcmp(argv[i], "--trace") && i + 1 < argc) trace_path = argv[++i];
		else if (!strcmp(argv[i], "--analysis") && i + 1 < argc) {   /* everything a port needs to know, from the interpreter (exact pcs) */
			analysis_dir = argv[++i]; setenv("RECOMP_INTERP", "1", 1);
			coverage_bits = bits_load(analysis_dir, "coverage.bin"); entry_bits = bits_load(analysis_dir, "entries.bin");
		}
		else if (!strcmp(argv[i], "--coverage") && i + 1 < argc) { coverage_path = argv[++i]; coverage_bits = calloc(1u << 21, 1); }
		else if (!strcmp(argv[i], "--play")) play = 1;
		else if (!strcmp(argv[i], "--fast")) host_fast = 1;
		else if (!strcmp(argv[i], "--timeout") && i + 1 < argc) { signal(SIGALRM, on_timeout); alarm((unsigned)atoi(argv[++i])); }
		else if (!strcmp(argv[i], "--record") && i + 1 < argc) { recf = fopen(argv[++i], "w"); if (!recf) { fprintf(stderr, "recomp: can't open %s\n", argv[i]); return 2; } }
		else if (!strcmp(argv[i], "--input") && i + 1 < argc) { inputf = fopen(argv[++i], "r"); if (!inputf) { fprintf(stderr, "recomp: can't open %s\n", argv[i]); return 2; } }
#ifdef RECOMP_ITRACE
		else if (!strcmp(argv[i], "--itrace") && i + 2 < argc) { itrace_f = fopen(argv[++i], "w"); itrace_left = atol(argv[++i]); }
#endif
		else if (machine_arg(argc, argv, &i)) {}
		else { fprintf(stderr, "usage: game [--memory memory.bin] %s[--frames N] [--png out.png] [--dump mem.bin] [--wav out.wav] [--pngs DIR] [--speed 1|2|4|8|16|32] "
		                       "[--trace file] [--input keys.txt] [--record keys.txt] [--coverage FILE] [--analysis DIR] [--timeout SEC] [--play [--fast]]\n", machine_usage()); return 2; }
	}
	machine_init();
	load_memory(image);
	{ static char d[1024]; const char *slash = strrchr(image, '/'); if (slash) { snprintf(d, sizeof d, "%.*s", (int)(slash - image), image); image_dir = d; } }
	if (trace_path) tracef = fopen(trace_path, "w");
	machine_start();
#ifdef USE_SDL
	if (play) sdl_open();
#else
	if (play) { fprintf(stderr, "recomp: this build has no window (built without SDL2: brew install sdl2 / apt install libsdl2-dev, then agk recomp again)\n"); return 2; }
#endif
	scripted_input();                     /* (a line for frame 0 applies before the first frame, as in the emulator) */
	run_translated();
	return 0;
}
