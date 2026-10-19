/* synth.h — procedural PCM generation interface (src/audio/synth.c)
 *
 * Everything declared here is pure math: fills caller-owned int16_t PCM
 * buffers from oscillators/noise/envelopes and returns/mutates plain
 * data. No file descriptors, no processes, no threads, no system calls
 * beyond reading the clock to seed a PRNG — playback (spawning
 * aplay/pacat/pw-play, writing to their stdin, volume control) lives in
 * src/platform/linux/linux_audio.c instead, which calls into this
 * header's functions to get the samples it writes out.
 *
 * Self-contained on purpose (no timer.h/App dependency) so this file
 * never needs X11/Xft or any OS-specific header — it's just math.
 */
#ifndef SYNTH_H
#define SYNTH_H

#include <stdint.h>
#include <math.h>

#ifndef SAMPLE_RATE
#define SAMPLE_RATE 44100
#endif
#ifndef TAU
#define TAU (2.0 * M_PI)
#endif

/* Alarm motif shape — must match ALARM_N/ALARM_ONSETS in timer_state.h
 * (shared compile-time constant, not a runtime setting). */
#ifndef ALARM_N
#define ALARM_N 6
#endif

/* ── Cue synthesis (calm, low-volume UI feedback sounds) ──── */
float clampf(float x, float lo, float hi);
float smootherstep(float t);

/* Returns a malloc'd buffer of `*out_n` samples for the named cue
 * ("launch","start","pause","resume","stop","close","half","last",
 * "dismiss","alert_open"); caller frees it. NULL/0 on failure. */
int16_t *generate_cue(const char *kind, int *out_n);

/* ── Alarm arpeggio tone (one struck-bar note) ─────────────
 * freq: pitch (Hz). dt: seconds since this note's onset. dc: decay
 * rate. bright: 0..1 brightness/overtone mix. Returns a sample in
 * roughly [-1,1] before mixing/gain. */
float render_alarm_tone(float freq, float dt, float dc, float bright);

/* ── Sleep-mode ambient/wake phase generators ──────────────
 * Each phase keeps its own small generator state, freshly seeded via
 * sleep_seed() at the start of every phase so no two plays sound
 * identical. `t_base` + `phase0` (in samples) give continuous
 * oscillator time across repeated chunk calls for the same phase. */
uint32_t sleep_seed(void);

typedef struct {
    uint32_t rng;
    float    noise_lp;
    float    drop_env, drop_freq, drop_phase;
    double   next_drop_at;
} RainState;

/* "preparing" phase: filtered noise bed + randomly-timed droplet chirps. */
void generate_rain_pcm(int16_t *chunk, int n, RainState *st,
                        double t_base, long phase0, float env_gain);

/* "sleeping" phase: three slow detuned, LFO-modulated sine oscillators
 * (soft chord) over a very quiet filtered-noise pad. */
void generate_sleep_bed_pcm(int16_t *chunk, int n, float *lp_state,
                             uint32_t *rng, double t_base, long phase0,
                             float env_gain);

/* "waking" phase: gentle rising chime melody over the sleep-bed pad,
 * brightening over `duration` seconds. */
void generate_waking_pcm(int16_t *chunk, int n, float *lp_state,
                          uint32_t *rng, double t_base, long phase0,
                          double duration, float env_gain);

/* Shared proportional phase-length calculation (preparing/waking window
 * lengths from a total timer duration) — used by both the real playback
 * control thread and the "/testsleep" preview, so they can never drift
 * apart. */
void sleep_phase_lens(double total_seconds, double *intro_len, double *wake_len);

#endif
