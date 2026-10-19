/* platform.h — OS-abstraction interfaces
 *
 * Linux/X11 is the only implementation right now (src/platform/linux/),
 * selected at compile time via #ifdef __linux__ in the Makefile's source
 * list. A future Windows/macOS port would add its own src/platform/<os>/
 * implementing the same interfaces declared here, without src/core or
 * src/audio needing to change.
 *
 * These are the *existing* app-facing function signatures (previously
 * declared in the old monolithic timer.h, implemented in audio.c/
 * config.c) formalized as the platform boundary, not a newly-invented
 * parallel API — src/core/app.c and the ui/ screen files call exactly these names,
 * same as before; only their implementation moved to
 * src/platform/linux/{linux_audio.c,linux_fs.c}.
 *
 * Scope of what's actually abstracted behind this header today:
 *   - Audio playback (PCM streaming) and system volume get/set —
 *     src/platform/linux/linux_audio.c. The procedural PCM generators in
 *     src/audio/synth.c are pure math and only ever reach the speakers
 *     through the functions declared below.
 *   - Filesystem/path resolution (config dir) — src/platform/linux/linux_fs.c.
 *   - Hardware input lock (USB HID deauthorize, "-l" switch) — see
 *     include/platform/lock.h / src/platform/linux/lock.c, which was
 *     already OS-specific and is unchanged, just relocated.
 *
 * NOT abstracted yet: windowing/input/drawing. The X11-specific window
 * management, event loop and Xft/XRender drawing primitives remain
 * directly in src/core/app.c and the ui/ screen files (as they were in the
 * original timer.c/draw.c) rather than going through a PlatWindow/
 * PlatEvent/drawing-primitive layer — the App struct's window and
 * drawing-surface handles are real Xlib/Xft types throughout the
 * codebase (hundreds of call sites in the state machine and every
 * screen's renderer), and decoupling that fully is a substantially
 * larger rewrite than the mechanical move+relink done for the rest of
 * this refactor. A future port would need to finish that piece too.
 *
 * Threading: pthreads are used as-is throughout (audio playback thread,
 * sleep-mode control thread) — no abstraction wrapper here. A future
 * Windows port would need its own thin thread wrapper; not built
 * speculatively ahead of that need.
 */
#ifndef PLATFORM_H
#define PLATFORM_H

#include <stdint.h>

/* Forward declarations only — used by pointer here. If timer_state.h
 * has already been included (as it has by the time every .c file in
 * this project reaches this header via the aggregate timer.h) these
 * just match its real `struct App` / `struct SleepPreviewData` tags;
 * a standalone includer (e.g. linux_fs.c, which only needs
 * get_config_dir()) can include this header without ever pulling in
 * the full App definition. */
typedef struct App App;
typedef struct SleepPreviewData SleepPreviewData;

/* ── Filesystem / paths ────────────────────────────────────
 * Resolves (creating if needed) the "config" directory used for
 * runtime state (config.conf, session log, SQLite database) — normally
 * sitting next to the running binary; see linux_fs.c for the exact
 * resolution order and fallback. */
void get_config_dir(char *out, int out_sz);

/* ── Audio playback ─────────────────────────────────────────
 * The one and only volume knob used anywhere in this program: sets the
 * system/speaker output level to exactly `pct` (0-100) on every active
 * sink, right now, unmuting first. */
void set_system_volume(int pct);

/* Short calm UI-feedback cue ("launch","start","pause","resume","stop",
 * "close","half","last","dismiss","alert_open") — procedurally
 * generated (src/audio/synth.c), played once via the persistent cue
 * backend. */
void play_cue(App *app, const char *kind);

/* Looping alarm siren (bell arpeggio, ramping volume/tempo) — replaced
 * by the sleep-mode wake melody instead when sleep_mode is set. */
void play_alert_sound(App *app);

/* Starts the sleep-mode ambient control thread (preparing -> sleeping ->
 * waking phases) for a timer of `total_seconds`. */
void play_sleep_ambient(App *app, double total_seconds);

/* Builds (but does not play) the phase-length plan for a sleep-mode
 * timer of `total_seconds` — shared by the GUI preview overlay and the
 * "/testsleep" CLI printout. Caller frees the result. */
SleepPreviewData *sleep_build_preview(double total_seconds);

/* Prints a sleep-mode phase plan to stdout ("/testsleep <time>"). */
void sleep_print_preview(const SleepPreviewData *d, const char *raw_input);

/* Plays a short back-to-back audition of all three sleep-mode phases
 * (GUI "Play preview" button). */
void play_sleep_audition(App *app, const SleepPreviewData *d);

/* `timer test <phase>` — foreground CLI audition of one procedurally
 * generated phase at its configured system volume; blocks for `seconds`
 * or until Ctrl+C. */
void play_test_phase_cli(const char *phase_name, int volume_pct, double seconds);

/* Stops every currently-playing audio job (alarm siren, sleep ambient,
 * sleep audition) and reaps its player processes. */
void kill_all_audio(App *app);

#endif
