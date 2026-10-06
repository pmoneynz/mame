/* license:BSD-3-Clause
 * copyright-holders:MPC3000 for Mac contributors
 *
 * libmpc3k.h - C API of the MPC3000 core library (MAME mpc3000 driver,
 * headless "mpcapp" OSD).
 *
 * One machine per process. The machine runs on its own emulation thread,
 * started by mpc3k_start(). Unless stated otherwise a function may be
 * called from any thread.
 *
 * Time: every timestamp is emulated time in nanoseconds since power-on.
 * Audio: 10 channels at 44 100 Hz, float, straight from the L7A1045 DSP
 * (main L, main R, individual outs 1-8), before any mixing or resampling.
 * Channel n of the individual outs is the DSP output the OS calls n.
 *
 * Pacing: the emulation thread produces audio in blocks of 44 or 45 frames
 * (one per 1 ms sound flush) into a ring. Unless free-running, it waits
 * while the ring holds more than ring_target_frames, so the consumer of
 * mpc3k_audio_read() (normally the Core Audio callback) clocks the machine.
 *
 * Frame indices are absolute: frame n is emulated time n / 44100 s (the DSP
 * runs at exactly 44 100 Hz from power-on), also after a state load.
 */

#ifndef LIBMPC3K_H
#define LIBMPC3K_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MPC3K_API_VERSION 5

#define MPC3K_SAMPLE_RATE    44100
#define MPC3K_AUDIO_CHANNELS 10
#define MPC3K_LCD_WIDTH      240
#define MPC3K_LCD_HEIGHT     64

typedef struct mpc3k mpc3k;

/* Front-panel events (driver: src/mame/akai/mpc3000_app.h). */
enum mpc3k_event_kind
{
	MPC3K_EVENT_KEY = 1,        /* code: panel key code 0x40..0x79; value: 1 press, 0 release */
	MPC3K_EVENT_PAD = 2,        /* code: pad 1..16; value: velocity 1..127, 0 release */
	MPC3K_EVENT_PRESSURE = 3,   /* code: pad 1..16; value: 0..127 */
	MPC3K_EVENT_SLIDER = 4,     /* value: 0..127 */
	MPC3K_EVENT_DIAL = 5,       /* value: signed steps -128..127, + is clockwise */
	MPC3K_EVENT_FOOTSWITCH = 6  /* code: 1 or 2; value: line level 0/1 (polarity unknown) */
};

typedef struct mpc3k_event
{
	uint64_t time_ns;           /* emulated time at which the panel sends it */
	uint8_t kind;               /* enum mpc3k_event_kind */
	uint8_t code;
	int16_t value;
} mpc3k_event;

/* Called on the emulation thread for every audio block, before the ring:
 * ch[c][i] is channel c, frame first_frame + i. Must not block for long. */
typedef void (*mpc3k_audio_tap)(void *user, const float *const ch[MPC3K_AUDIO_CHANNELS],
		unsigned frames, uint64_t first_frame);

typedef struct mpc3k_config
{
	const char *rom_path;       /* MAME -rompath: directory holding mpc3000/ (required) */
	const char *bios;           /* "vailixi" (3.50, default), "default" (3.12), "v311", "v308" */
	const char *work_dir;       /* cfg/, nvram/, sta/, snap/ live here (required, created) */
	const char *floppy;         /* floppy image to insert at start, or NULL */
	int board_mb;               /* sound memory board: 2 (default), 8 or 32 */
	int simm_mb;                /* MB per SIMM in the pair: 0 (default), 1 or 4 */
	unsigned sound_update_hz;   /* sound flush rate; 0 = 1000 */
	unsigned ring_target_frames;/* back-pressure threshold; 0 = 132 (3 ms) */
	int free_run;               /* nonzero: start free-running (mpc3k_set_free_run) */
	uint64_t stop_at_ns;        /* nonzero: exit when emulated time reaches it */
	mpc3k_audio_tap audio_tap;  /* optional */
	void *user;                 /* passed to audio_tap */
	int extra_argc;             /* further MAME options, e.g. "-autoboot_script", "x.lua" */
	const char *const *extra_argv;
} mpc3k_config;

/* Create the machine (not yet running). Copies the config. NULL on error
 * (missing rom_path/work_dir, or a machine already exists). */
mpc3k *mpc3k_create(const mpc3k_config *config);

/* Start the emulation thread. 0 on success. */
int mpc3k_start(mpc3k *m);

/* Ask the machine to exit (asynchronous). */
void mpc3k_stop(mpc3k *m);

/* Wait for the emulation thread to end; returns MAME's exit code. */
int mpc3k_wait(mpc3k *m);

/* Stop, wait and free. */
void mpc3k_destroy(mpc3k *m);

/* Queue a panel event. Single producer: call from one thread only (it may
 * differ from the reader threads). Events may be pushed before
 * mpc3k_start(). Returns 0, or -1 if the queue is full. The driver fires
 * the event at time_ns; an event whose time has passed fires at once and
 * counts in mpc3k_late_events(). */
int mpc3k_push_event(mpc3k *m, const mpc3k_event *event);

/* Free-running (nonzero): never wait on the ring; blocks that do not fit are
 * dropped (cold boot, tests, offline bounce). Paced (0): the ring reader
 * clocks the machine. May change at any time; on the switch to paced the
 * reader's next read skips what free-running left in the ring. */
void mpc3k_set_free_run(mpc3k *m, int free_run);

/* Emulated time at the last sound flush. */
uint64_t mpc3k_time_ns(mpc3k *m);

/* Absolute index of the next frame to be produced (emulated time x 44100). */
uint64_t mpc3k_frames_produced(mpc3k *m);

/* Read up to `frames` frames, interleaved, `channels` = 2 (main L/R) or 10.
 * Real-time safe: never blocks, never allocates. Returns frames read; the
 * caller fills the rest (silence) and the shortfall counts as an underrun.
 * _at also stores the absolute index of the first frame read (or that would
 * have been read) in *first_frame. Frames come out contiguous: after a time
 * jump the ring is relabelled only once it is empty. */
size_t mpc3k_audio_read(mpc3k *m, float *out, size_t frames, unsigned channels);
size_t mpc3k_audio_read_at(mpc3k *m, float *out, size_t frames, unsigned channels, uint64_t *first_frame);

/* Frames waiting in the ring. */
size_t mpc3k_audio_available(mpc3k *m);

/* Copy the latest LCD frame, one byte per pixel (1 lit, 0 unlit), row
 * major 240x64. Returns its sequence number (0 before the first frame). */
uint64_t mpc3k_lcd(mpc3k *m, uint8_t pixels[MPC3K_LCD_WIDTH * MPC3K_LCD_HEIGHT]);

/* LED states at the last video frame: bit n = LED n (led0..led15 in the
 * driver: Edit Loop, Simul Seq, Transpose, Wait For, Count In, Auto Punch,
 * Rec, Over Dub, Play, Bank A-D, Full Level, 16 Levels, After). */
uint16_t mpc3k_leds(mpc3k *m);

/* Counters: events that fired late, underruns (reads that came up short
 * after the first block), blocks dropped because the ring was full. */
uint32_t mpc3k_late_events(mpc3k *m);
/* The back-pressure threshold now, in frames: ring_target_frames, plus 44
 * (1 ms) per underrun up to 441 (10 ms), minus 44 per 5 s without one. */
uint32_t mpc3k_ring_target(mpc3k *m);
uint32_t mpc3k_underruns(mpc3k *m);
uint32_t mpc3k_dropped_blocks(mpc3k *m);

/* Run code on the emulation thread (SPEC.md 5.2: it joins the output
 * device's audio workgroup, which the app only has once the device is open).
 * The emulation thread calls fn(user, 1) at its next 1 ms slice, and
 * fn(user, 0) exactly once later: when the hook is replaced or cleared, or
 * as the thread ends. A hook replaced before the thread took it gets only
 * fn(user, 0), on the caller's thread. user must stay valid until then. */
typedef void (*mpc3k_thread_hook)(void *user, int enter);
void mpc3k_set_thread_hook(mpc3k *m, mpc3k_thread_hook fn, void *user);

/* Pause or resume emulation. */
void mpc3k_pause(mpc3k *m, int paused);

/* Save or load a state slot in work_dir/sta (taken at the next safe point,
 * asynchronously). Return 0 if queued. */
int mpc3k_save_state(mpc3k *m, const char *slot);
int mpc3k_load_state(mpc3k *m, const char *slot);

/* Insert (path) or eject (NULL) the floppy, asynchronously. Return 0 if queued.
 * A new insert is a disk change for the OS. */
int mpc3k_floppy(mpc3k *m, const char *path);

/* Writes by the OS reach the image file once the drive is idle (unsaved
 * writes and the motor off for 0.5 s emulated): the count of those
 * write-backs, and whether the drive is busy now (motor on or unsaved
 * writes). A host syncing the image with a folder reads the file after the
 * count changes, and replaces the disk only while the drive is not busy. */
uint32_t mpc3k_floppy_writebacks(mpc3k *m);
int mpc3k_floppy_busy(mpc3k *m);

/* MAME command-line entry point with the headless OSD (used by the CLI
 * host for the test harness). */
int mpc3k_main(int argc, char **argv);

#ifdef __cplusplus
}
#endif

#endif /* LIBMPC3K_H */
