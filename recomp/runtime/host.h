/* The host side of a recompiled game, shared by every machine: the picture
 * buffer and its PNGs, the wav, the window (SDL2), pacing, scripted input and
 * its recording, the interpreter's bookkeeping files, traces. A machine
 * (recomp/amiga, recomp/md) provides the functions declared under "the
 * machine provides" and its own main() is host_main(). */
#ifndef HOST_H
#define HOST_H
#include "m68k.h"

/* ---- the host provides */
#define FRAME_MAX_W 320
#define FRAME_MAX_H 256
extern u8 frame_rgb[FRAME_MAX_W * FRAME_MAX_H * 3];   /* the picture, frame_w x frame_h RGB, as the machine renders it */
extern int frame_w, frame_h;              /* set by the machine before the first frame */
extern u32 frame_count;                   /* video frames so far (the machine's run_events counts) */
extern int frame_done;
extern int cycle_shift;                   /* --speed N: the CPU N times faster, as the game sees it */
extern const char *image_dir;             /* where memory.bin is: needed.txt, patched.txt go next to it */
extern int host_fast;                     /* no pacing */

void frame_hook(void);                    /* the machine calls this after every video frame */
void host_record_event(const char *words);    /* --record: "key 44", "disk 2": appended to this frame's line */
int has_word(const char *words, const char *w);   /* w as a whole word of an input line */
void note_interpreted(u32 pc);
void note_patched(u32 addr, u16 was, u16 now);
void trace_die(const char *why, u32 addr);
void itrace(u32 addr);
int host_main(int argc, char **argv);     /* parses the common flags, machine_arg() the rest, then runs */
void host_exit(void);                     /* report and leave (Esc, the window closed) */
int host_window_open(void);               /* --play and SDL2: a window exists */
u64 host_perf_counter(void);

/* ---- the machine provides */
void machine_init(void);                  /* allocate memory, reset the chips; before the image loads */
void machine_start(void);                 /* the start state (start.h): registers, chips, where the pc begins */
int machine_arg(int argc, char **argv, int *i);   /* a flag the host doesn't know: consume it (advance *i) and return 1 */
const char *machine_usage(void);          /* its flags, for the usage line */
void machine_apply_input(const char *words);      /* a scripted input line's words -> the ports, the state from now on */
void machine_input_words(char *buf, size_t n);    /* the ports' current state as those words (what --record writes) */
void machine_sdl_input(void);             /* SDL events and keyboard state -> the ports (only in a --play window) */
void machine_sdl_key(int sym, int mod, int down);  /* a key the host didn't take (Esc, F11, F12 are the host's) */
void audio_render(s16 *out, int n, int rate);     /* n stereo frames of this video frame's sound at `rate` */
void machine_audio_tag(void);             /* called once per frame before audio_render (nothing for most) */
void machine_at_exit(void);               /* --frames ran out: dumps the machine may offer */
#endif
