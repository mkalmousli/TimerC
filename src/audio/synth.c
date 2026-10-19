/* synth.c — procedural PCM generation: pure math, no OS calls.
 *
 * Relocated verbatim from the top/middle of the old monolithic audio.c —
 * see include/audio/synth.h for what's exposed to the playback backend
 * (src/platform/linux/linux_audio.c), which is the only thing that ever
 * writes these generated samples anywhere.
 */
#include "synth.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>

float clampf(float x, float lo, float hi) {
    if (x < lo) return lo;
    if (x > hi) return hi;
    return x;
}

float smootherstep(float t) {
    t = clampf(t, 0.0f, 1.0f);
    return t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f);
}

/* Soft chime — pure tone, gentle attack, for UI cues only. */
static void render_chime(int16_t *buf, int n, float freq, float decay, float vol) {
    for (int i = 0; i < n; i++) {
        float t  = (float)i / SAMPLE_RATE;
        float at = 1.0f - expf(-t * 48.0f);
        float en = at * expf(-t * decay);
        float s  = sinf((float)TAU * freq * t);
        s += 0.12f * sinf((float)TAU * freq * 2.0f * t) * expf(-t * decay * 1.2f);
        s *= en * vol * 0.58f;
        buf[i] = (int16_t)(clampf(s, -1.0f, 1.0f) * 32767.0f);
    }
}

/* Gentle pitch glide for cues. */
static void render_soft_glide(int16_t *buf, int n, float f0, float f1, float vol) {
    for (int i = 0; i < n; i++) {
        float t  = (float)i / SAMPLE_RATE;
        float p  = (float)i / (float)(n > 1 ? n - 1 : 1);
        float at = 1.0f - expf(-t * 40.0f);
        float en = at * (1.0f - 0.06f * p);
        float u  = p * p * (3.0f - 2.0f * p);
        float f  = f0 + (f1 - f0) * u;
        float s  = sinf((float)TAU * f * t) * en * vol * 0.55f;
        buf[i] = (int16_t)(clampf(s, -1.0f, 1.0f) * 32767.0f);
    }
}
static void apply_gentle_tail(int16_t *buf, int n) {
    static const int   d[] = {882, 1541};
    static const float g[] = {0.08f, 0.04f};
    for (int i = 0; i < n; i++) {
        float echo = 0.0f;
        for (int t = 0; t < 2; t++)
            if (i >= d[t]) echo += buf[i - d[t]] * g[t];
        int v = buf[i] + (int)echo;
        buf[i] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
    }
}

typedef enum { SYN_CHIME, SYN_GLIDE } SynKind;

typedef struct { SynKind kind; float p1, p2, decay, dur, gap, vol; } CueNote;

/* ── Cue note table — calm, quiet UI feedback ───────────── */
int16_t *generate_cue(const char *kind, int *out_n) {

    static const CueNote launch[] = {
        {SYN_GLIDE, 330.0f, 440.0f, 0.0f, 0.22f, 0.015f, 0.11f},
        {SYN_CHIME, 523.25f,   0.0f, 2.8f, 0.28f, 0.000f, 0.10f},
    };
    static const CueNote start_n[] = {
        {SYN_CHIME, 392.00f, 0.0f, 2.8f, 0.14f, 0.025f, 0.09f},
        {SYN_CHIME, 493.88f, 0.0f, 2.6f, 0.14f, 0.025f, 0.10f},
        {SYN_CHIME, 587.33f, 0.0f, 2.4f, 0.30f, 0.000f, 0.09f},
    };
    static const CueNote pause_n[] = {
        {SYN_CHIME, 440.00f, 0.0f, 2.6f, 0.16f, 0.030f, 0.09f},
        {SYN_CHIME, 349.23f, 0.0f, 2.4f, 0.28f, 0.000f, 0.08f},
    };
    static const CueNote resume_n[] = {
        {SYN_GLIDE, 349.23f, 440.00f, 0.0f, 0.16f, 0.020f, 0.09f},
        {SYN_CHIME, 493.88f, 0.0f, 2.6f, 0.14f, 0.025f, 0.09f},
        {SYN_CHIME, 587.33f, 0.0f, 2.4f, 0.26f, 0.000f, 0.09f},
    };
    static const CueNote stop_n[] = {
        {SYN_CHIME, 440.00f, 0.0f, 2.6f, 0.14f, 0.025f, 0.08f},
        {SYN_CHIME, 369.99f, 0.0f, 2.4f, 0.14f, 0.025f, 0.08f},
        {SYN_CHIME, 293.66f, 0.0f, 2.2f, 0.30f, 0.000f, 0.08f},
    };
    static const CueNote close_n[] = {
        {SYN_CHIME, 392.00f, 0.0f, 2.6f, 0.16f, 0.000f, 0.08f},
    };
    static const CueNote half_n[] = {
        {SYN_CHIME, 523.25f, 0.0f, 2.6f, 0.14f, 0.030f, 0.08f},
        {SYN_CHIME, 659.25f, 0.0f, 2.4f, 0.26f, 0.000f, 0.08f},
    };
    static const CueNote last_n[] = {
        {SYN_CHIME, 784.00f, 0.0f, 3.2f, 0.09f, 0.040f, 0.09f},
        {SYN_CHIME, 880.00f, 0.0f, 3.0f, 0.09f, 0.040f, 0.09f},
        {SYN_CHIME, 987.77f, 0.0f, 2.8f, 0.12f, 0.000f, 0.10f},
    };
    static const CueNote dismiss_n[] = {
        {SYN_CHIME, 440.00f, 0.0f, 2.6f, 0.16f, 0.025f, 0.08f},
        {SYN_CHIME, 349.23f, 0.0f, 2.4f, 0.28f, 0.000f, 0.08f},
    };
    static const CueNote alert_open[] = {
        {SYN_CHIME, 392.00f, 0.0f, 2.8f, 0.12f, 0.022f, 0.09f},
        {SYN_CHIME, 493.88f, 0.0f, 2.6f, 0.12f, 0.022f, 0.09f},
        {SYN_CHIME, 587.33f, 0.0f, 2.4f, 0.28f, 0.000f, 0.09f},
    };

    const CueNote *notes = start_n;
    int n_notes = 3;
#define PICK(k, arr, cnt) if (!strcmp(kind, k)) { notes = arr; n_notes = cnt; }
    PICK("launch",     launch,     2)
    PICK("start",      start_n,    3)
    PICK("pause",      pause_n,    2)
    PICK("resume",     resume_n,   3)
    PICK("stop",       stop_n,     3)
    PICK("close",      close_n,    1)
    PICK("half",       half_n,     2)
    PICK("last",       last_n,     3)
    PICK("dismiss",    dismiss_n,  3)
    PICK("alert_open", alert_open, 3)
#undef PICK

    const int TAIL = SAMPLE_RATE / 5;

    int body = 0;
    for (int i = 0; i < n_notes; i++)
        body += (int)(SAMPLE_RATE * notes[i].dur)
              + (int)(SAMPLE_RATE * notes[i].gap);
    int total = body + TAIL;
    if (total <= 0) { *out_n = 0; return NULL; }

    int16_t *buf = (int16_t *)calloc(total, sizeof(int16_t));
    if (!buf)      { *out_n = 0; return NULL; }

    const float CUE_GAIN = 0.48f;
    int off = 0;
    for (int i = 0; i < n_notes; i++) {
        int n_s = (int)(SAMPLE_RATE * notes[i].dur);
        float v = notes[i].vol * CUE_GAIN;
        switch (notes[i].kind) {
        case SYN_CHIME:
            render_chime(buf + off, n_s, notes[i].p1, notes[i].decay, v);
            break;
        case SYN_GLIDE:
            render_soft_glide(buf + off, n_s, notes[i].p1, notes[i].p2, v);
            break;
        }
        off += n_s + (int)(SAMPLE_RATE * notes[i].gap);
    }

    apply_gentle_tail(buf, total);

    *out_n = total;
    return buf;
}

/* ── Alarm arpeggio tone ────────────────────────────────── */
float render_alarm_tone(float freq, float dt, float dc, float bright) {
    /* Soft mallet attack (rounded, not a hard click) + natural decay,
     * like a struck wooden bar. */
    float pluck = 1.0f - expf(-dt * 140.0f);
    float ring  = expf(-dt * dc);
    float f     = freq;
    float s     = sinf((float)TAU * f * dt);
    /* Gentle warm overtones only — no harsh square wave, kept quiet so
     * they can never dominate the fundamental. */
    s += 0.16f * sinf((float)TAU * f * 2.0f * dt) * expf(-dt * dc * 1.7f);
    s += 0.05f * sinf((float)TAU * f * 4.0f * dt) * expf(-dt * dc * 2.4f) * bright;
    return s * ring * pluck;
}

/* ── Sleep-mode ambient/wake phase generators ─────────────
   Three phases, all procedurally generated (no user-supplied audio
   files) — each phase is seeded fresh from the system clock every time
   it starts, so no two nights sound identical. */

uint32_t sleep_seed(void) {
    struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
    uint32_t s = (uint32_t)(ts.tv_nsec ^ (ts.tv_sec * 2654435761u));
    return s ? s : 0x9e3779b9u;
}

/* generate_rain_pcm — one chunk of the "preparing" phase: a softly
 * filtered noise bed with irregular, randomly-timed droplet impulses
 * (short decaying sine chirps at random pitch), giving a light,
 * calming rain-on-a-window texture rather than a wall of hiss.
 *
 * Tuned to be deliberately gentle: the noise bed is low-passed harder
 * (0.08 -> 0.045 coefficient) so it reads as soft rumble instead of
 * hiss, droplets are pitched lower and softer (they were bright/sharp
 * enough to sound like clicks rather than rain), spaced further apart
 * on average, and decay a little faster so they never linger or stack
 * into a busy texture — the whole point of "preparing" is to help
 * someone relax, not add a rhythmic distraction. */
void generate_rain_pcm(int16_t *chunk, int n, RainState *st,
                        double t_base, long phase0, float env_gain) {
    for (int i = 0; i < n; i++) {
        double elapsed = t_base + (double)(phase0 + i) / SAMPLE_RATE;

        st->rng = st->rng * 1664525u + 1013904223u;
        float white = ((int32_t)(st->rng >> 8) / 8388608.0f) - 1.0f;
        st->noise_lp += 0.045f * (white - st->noise_lp);
        float bed = st->noise_lp * 1.3f;

        if (elapsed >= st->next_drop_at) {
            uint32_t r = st->rng;
            st->drop_env   = 0.35f + 0.30f * (((r >> 8) & 0xff) / 255.0f);
            st->drop_freq  = 500.0f + 700.0f * (((r >> 16) & 0xff) / 255.0f);
            st->drop_phase = 0.0f;
            double gap = 0.09 + 0.42 * (((r >> 4) & 0xff) / 255.0);
            st->next_drop_at = elapsed + gap;
        }
        float drop = 0.0f;
        if (st->drop_env > 0.0008f) {
            st->drop_phase += (float)(TAU * st->drop_freq / SAMPLE_RATE);
            drop = sinf(st->drop_phase) * st->drop_env;
            st->drop_env *= 0.9955f;
        }

        float s = tanhf((bed * 0.30f + drop * 0.34f) * 0.95f) * env_gain;
        chunk[i] = (int16_t)(clampf(s, -1.0f, 1.0f) * 32767.0f);
    }
}

/* generate_sleep_bed_pcm — one chunk of the "sleeping" phase: three
 * slow, mutually-detuned sine oscillators (a soft chord) each slowly
 * amplitude-modulated by its own LFO, plus a very quiet filtered-noise
 * pad underneath for warmth. */
void generate_sleep_bed_pcm(int16_t *chunk, int n, float *lp_state,
                             uint32_t *rng, double t_base, long phase0,
                             float env_gain) {
    static const float BASE[3]   = {146.83f, 220.00f, 293.66f}; /* D3 A3 D4 */
    static const float DETUNE[3] = {1.0f, 1.006f, 0.994f};      /* slow beating */
    for (int i = 0; i < n; i++) {
        double elapsed = t_base + (double)(phase0 + i) / SAMPLE_RATE;

        *rng = *rng * 1664525u + 1013904223u;
        float white = ((int32_t)(*rng >> 8) / 8388608.0f) - 1.0f;
        *lp_state += 0.015f * (white - *lp_state);
        float pad = *lp_state * 0.5f;

        float lfo0 = 0.5f + 0.5f * sinf((float)TAU * 0.025f * (float)elapsed);
        float lfo1 = 0.5f + 0.5f * sinf((float)TAU * 0.019f * (float)elapsed + 1.3f);
        float lfo2 = 0.5f + 0.5f * sinf((float)TAU * 0.014f * (float)elapsed + 2.1f);
        float chord = 0.50f * sinf((float)TAU * BASE[0] * DETUNE[0] * (float)elapsed) * lfo0
                    + 0.35f * sinf((float)TAU * BASE[1] * DETUNE[1] * (float)elapsed) * lfo1
                    + 0.25f * sinf((float)TAU * BASE[2] * DETUNE[2] * (float)elapsed) * lfo2;

        float s = tanhf((pad * 0.30f + chord * 0.55f) * 1.05f) * env_gain;
        chunk[i] = (int16_t)(clampf(s, -1.0f, 1.0f) * 32767.0f);
    }
}

/* generate_waking_pcm — one chunk of the "waking" phase: a gentle,
 * slowly rising chime melody over the same soft pad as the sleep bed,
 * brightening/rising slightly over the given duration so it never feels
 * like the usual jarring siren. */
void generate_waking_pcm(int16_t *chunk, int n, float *lp_state,
                          uint32_t *rng, double t_base, long phase0,
                          double duration, float env_gain) {
    static const float MEL_FREQ[4] = {392.00f, 440.00f, 523.25f, 659.25f};
    const double MEL_PERIOD = 3.0;
    for (int i = 0; i < n; i++) {
        double elapsed = t_base + (double)(phase0 + i) / SAMPLE_RATE;
        double seg_t   = (double)(phase0 + i) / SAMPLE_RATE;
        float  rise    = duration > 0 ? smootherstep((float)(seg_t / duration)) : 1.0f;

        *rng = *rng * 1664525u + 1013904223u;
        float white = ((int32_t)(*rng >> 8) / 8388608.0f) - 1.0f;
        *lp_state += 0.02f * (white - *lp_state);
        float pad = *lp_state * 0.4f;

        float lfo = 0.5f + 0.5f * sinf((float)TAU * 0.03f * (float)elapsed);
        float chord = 0.4f * sinf((float)TAU * 220.0f * (float)elapsed) * lfo
                    + 0.3f * sinf((float)TAU * 293.66f * (float)elapsed);

        double phi = fmod(elapsed, MEL_PERIOD * 4.0) / MEL_PERIOD;
        int    idx = (int)phi;
        double local_t = phi - idx;
        float  pluck = 1.0f - expf(-(float)local_t * 30.0f);
        float  ring  = expf(-(float)local_t * 3.0f);
        float  freq  = MEL_FREQ[idx & 3] * (1.0f + 0.5f * rise);
        float  mel   = sinf((float)TAU * freq * (float)elapsed) * pluck * ring;
        mel += 0.15f * sinf((float)TAU * freq * 2.0f * (float)elapsed) * pluck * ring;

        float amb = (pad * 0.35f + chord * 0.45f) * (0.25f + 0.15f * rise);
        float s = tanhf((amb + mel * (0.25f + 0.35f * rise)) * 1.1f) * env_gain;
        chunk[i] = (int16_t)(clampf(s, -1.0f, 1.0f) * 32767.0f);
    }
}

/* Shared by the control thread and the /testsleep preview so the two can
 * never drift apart. Purely proportional now — no custom files to size
 * against. */
void sleep_phase_lens(double total_seconds, double *intro_len, double *wake_len) {
    *intro_len = total_seconds > 0 ? total_seconds * 0.12 : 60.0;
    if (*intro_len < 8.0)  *intro_len = 8.0;
    if (*intro_len > 60.0) *intro_len = 60.0;

    *wake_len = total_seconds > 0 ? total_seconds * 0.12 : 150.0;
    if (*wake_len < 20.0)  *wake_len = 20.0;
    if (*wake_len > 150.0) *wake_len = 150.0;
}
