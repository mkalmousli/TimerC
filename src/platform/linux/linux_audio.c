/* linux_audio.c — audio playback backend (aplay/pacat/pw-play spawning,
 * volume control, playback threads). Relocated from the old monolithic
 * audio.c; the pure-math PCM generators it calls now live in
 * src/audio/synth.c (see include/audio/synth.h) — this file owns
 * everything that actually touches a process, a pipe, or the system
 * mixer, which is exactly what src/audio/synth.c must never do.
 */
#include "timer.h"
#include "synth.h"

/* ── Cue synthesis (calm, low volume) / alarm (loud, aggressive) ── */



/* ── Playback (all sinks in parallel) ─────────────────────── */

typedef enum { PLAY_PACAT, PLAY_PW, PLAY_APLAY } PlayerKind;

static PlayerKind detect_player(void) {
    if (access("/usr/bin/pactl", X_OK) == 0 && access("/usr/bin/pacat", X_OK) == 0)
        return PLAY_PACAT;
    if (access("/usr/local/bin/pactl", X_OK) == 0 && access("/usr/local/bin/pacat", X_OK) == 0)
        return PLAY_PACAT;
    if (access("/usr/bin/pw-play", X_OK) == 0)
        return PLAY_PW;
    if (access("/usr/local/bin/pw-play", X_OK) == 0)
        return PLAY_PW;
    return PLAY_APLAY;
}

static int enumerate_sinks(char sinks[][128], int max) {
    const char *pactl_paths[] = {"/usr/bin/pactl", "/usr/local/bin/pactl", NULL};
    FILE *f = NULL;
    for (int i = 0; pactl_paths[i]; i++) {
        if (access(pactl_paths[i], X_OK) != 0) continue;
        char cmd[256];
        snprintf(cmd, sizeof(cmd), "%s list short sinks 2>/dev/null", pactl_paths[i]);
        f = popen(cmd, "r");
        if (f) break;
    }
    if (!f) return 0;

    int n = 0;
    char line[512];
    while (n < max && fgets(line, sizeof(line), f)) {
        char *p = strchr(line, '\t');
        if (!p) continue;
        p++;
        char *q = strchr(p, '\t');
        if (!q) continue;
        int len = (int)(q - p);
        if (len <= 0 || len >= 128) continue;
        memcpy(sinks[n], p, (size_t)len);
        sinks[n][len] = '\0';
        n++;
    }
    pclose(f);
    return n;
}

static pid_t spawn_player_to_sink(const char *sink, int *stdin_fd, PlayerKind kind) {
    int pipefd[2];
    if (pipe(pipefd) < 0) return -1;

    pid_t pid = fork();
    if (pid < 0) { close(pipefd[0]); close(pipefd[1]); return -1; }

    if (pid == 0) {
        close(pipefd[1]);
        dup2(pipefd[0], STDIN_FILENO);
        close(pipefd[0]);
        int null_fd = open("/dev/null", O_WRONLY);
        if (null_fd >= 0) {
            dup2(null_fd, STDOUT_FILENO);
            dup2(null_fd, STDERR_FILENO);
            close(null_fd);
        }

        if (kind == PLAY_PACAT) {
            if (sink)
                execlp("pacat", "pacat", "--playback", "--raw",
                       "--format=s16le", "--rate=44100", "--channels=1",
                       "--device", sink, NULL);
            else
                execlp("pacat", "pacat", "--playback", "--raw",
                       "--format=s16le", "--rate=44100", "--channels=1", NULL);
        } else if (kind == PLAY_PW) {
            if (sink)
                execlp("pw-play", "pw-play", "--format=s16", "--rate=44100",
                       "--channels=1", "--target", sink, "-", NULL);
            else
                execlp("pw-play", "pw-play", "--format=s16", "--rate=44100",
                       "--channels=1", "-", NULL);
        } else {
            execlp("aplay", "aplay", "-q", "-f", "S16_LE", "-r", "44100", "-c", "1", NULL);
        }
        _exit(1);
    }

    close(pipefd[0]);
    *stdin_fd = pipefd[1];
    return pid;
}

static int spawn_all_outputs(AudioOutput *outs, int max_outs) {
    PlayerKind kind = detect_player();
    char sinks[MAX_AUDIO_OUTPUTS][128];
    int n_sinks = (kind == PLAY_APLAY) ? 0 : enumerate_sinks(sinks, MAX_AUDIO_OUTPUTS);
    int n = 0;

    if (n_sinks > 0) {
        for (int i = 0; i < n_sinks && n < max_outs; i++) {
            int fd;
            pid_t pid = spawn_player_to_sink(sinks[i], &fd, kind);
            if (pid < 0) continue;
            outs[n].fd  = fd;
            outs[n].pid = pid;
            n++;
        }
    }

    if (n == 0) {
        int fd;
        pid_t pid = spawn_player_to_sink(NULL, &fd, kind);
        if (pid < 0) return 0;
        outs[0].fd  = fd;
        outs[0].pid = pid;
        n = 1;
    }
    return n;
}

static void track_audio_pid(App *app, pid_t pid) {
    pthread_mutex_lock(&app->audio_lock);
    int j = 0;
    for (int i = 0; i < app->n_audio_pids; i++) {
        if (waitpid(app->audio_pids[i], NULL, WNOHANG) == 0)
            app->audio_pids[j++] = app->audio_pids[i];
    }
    app->n_audio_pids = j;
    if (app->n_audio_pids < MAX_AUDIO_PIDS)
        app->audio_pids[app->n_audio_pids++] = pid;
    pthread_mutex_unlock(&app->audio_lock);
}

void kill_all_audio(App *app) {
    pthread_mutex_lock(&app->audio_lock);
    for (int i = 0; i < app->n_audio_pids; i++) {
        kill(app->audio_pids[i], SIGTERM);
        waitpid(app->audio_pids[i], NULL, WNOHANG);
    }
    app->n_audio_pids = 0;
    pthread_mutex_unlock(&app->audio_lock);
}

/* ── Alarm streaming synthesis ──────────────────────────────
   Cheerful note-block-style bell arpeggio (think Minecraft's harp/XP
   pickup chime) — starts quiet, fades in, and gets a little more
   insistent over time, but stays warm and friendly rather than tense
   or ominous. Pure sine + soft overtones only, no square waves or
   sub-bass rumble.                                              */

static const float AL_ONSET[ALARM_N] = ALARM_ONSETS;
/* A major arpeggio, up and back down — A4 C#5 E5 A5 E5 C#5 */
static const float AL_FREQ [ALARM_N] = {440.00f, 554.37f, 659.25f, 880.00f, 659.25f, 554.37f};
static const float AL_DECAY[ALARM_N] = {5.0f,    5.0f,    4.6f,    4.2f,    4.6f,   5.0f};
static const float AL_AMP  [ALARM_N] = {0.85f,   0.85f,   0.90f,   1.00f,   0.85f,  0.80f};


static ssize_t write_all_fd(int fd, const void *buf, size_t n) {
    const uint8_t *ptr = (const uint8_t *)buf;
    while (n > 0) {
        ssize_t w = write(fd, ptr, n);
        if (w <= 0) return -1;
        ptr += w;
        n   -= (size_t)w;
    }
    return 0;
}

static void write_all_outputs(AudioJob *job, const void *buf, size_t n) {
    for (int i = 0; i < job->n_outputs; i++)
        write_all_fd(job->outputs[i].fd, buf, n);
}

static void close_all_outputs(AudioJob *job) {
    for (int i = 0; i < job->n_outputs; i++)
        close(job->outputs[i].fd);
}

/* This thread only ever handles the long-running, looping alert siren —
 * short UI cues go through the persistent cue backend below instead, so
 * job->active_flag is always non-NULL here. */
static void *audio_writer_thread(void *arg) {
    AudioJob *job = (AudioJob *)arg;
    {
        const int CHUNK = SAMPLE_RATE / 20;
        int16_t *chunk  = (int16_t *)malloc(CHUNK * sizeof(int16_t));
        if (!chunk) goto done;

        int phase = 0;
        int last_volume_pct = job->alert_volume_min_pct;
        double last_reassert = 0.0;

        while (*job->active_flag) {
            float elapsed = (float)phase / SAMPLE_RATE;

            /* Volume fade-in (length configurable) — smootherstep keeps the
             * start gentle. A zero-length ramp means "full volume immediately". */
            float fade  = job->alert_ramp_seconds > 0.0f
                          ? smootherstep(elapsed / job->alert_ramp_seconds)
                          : 1.0f;
            float ramp  = 0.04f + 0.96f * fade;

            /* System volume rides the same fade, from the min it was set
             * to at alarm start up to the configured max — kept in step
             * with the PCM loudness ramp above so the room actually gets
             * louder as the tone does, not just the synthesized signal.
             * Only re-applied when the target percent has moved (set_system_volume
             * spawns a pactl process — no need to call it every chunk). */
            int target_pct = job->alert_volume_min_pct +
                (int)((job->alert_volume_max_pct - job->alert_volume_min_pct) * fade + 0.5f);
            /* Re-assert periodically even when the target hasn't moved
             * (e.g. once the fade-in has finished) — this pulls back any
             * externally-raised system mixer instead of letting it
             * compound on top of the configured ceiling. */
            if (target_pct != last_volume_pct || (double)elapsed - last_reassert >= 1.0) {
                set_system_volume(target_pct);
                last_volume_pct = target_pct;
                last_reassert = elapsed;
            }

            /* Gets a touch brighter/more present over time so it stays
             * noticeable, without pitch-bending or growling like a siren. */
            float bright = smootherstep((float)(elapsed / g_config.alarm_aggro_seconds));
            if (bright > 1.0f) bright = 1.0f;
            float tone_bright = bright * (0.30f + 0.70f * fade);

            for (int i = 0; i < CHUNK; i++) {
                double tt     = (double)(phase + i) / SAMPLE_RATE;
                double ph     = alarm_phase(tt);
                double persec = 1.0 / alarm_speed(tt);
                double fp     = ph - floor(ph);

                float s = 0.0f;

                for (int j = 0; j < ALARM_N; j++) {
                    double onf = (double)AL_ONSET[j] / g_config.alarm_period;
                    double dph = fp - onf;
                    if (dph < 0.0) dph += 1.0;
                    float dt = (float)(dph * persec);
                    s += render_alarm_tone(AL_FREQ[j], dt, AL_DECAY[j], tone_bright) * AL_AMP[j];
                }
                /* Headroom: when the tempo is fast, several notes ring at
                 * once and can sum past +/-1 — divide down first so the
                 * final soft-clip below is shaping, not hard-clipping a
                 * huge peak (hard clipping is what made this sound like a
                 * harsh, distorted siren). */
                s *= 0.42f;

                float level = 0.50f + 0.24f * fade + 0.14f * bright;
                s *= ramp * level;
                /* Soft saturation (tanh) instead of a hard clamp — gentle
                 * rounding of any remaining peaks, no digital clipping. */
                s = tanhf(s * 1.15f);
                chunk[i] = (int16_t)(s * 32767.0f);
            }

            phase += CHUNK;

            ssize_t rem = CHUNK * (ssize_t)sizeof(int16_t);
            const uint8_t *ptr2 = (const uint8_t *)chunk;
            while (rem > 0) {
                size_t wn = (size_t)rem;
                if (wn > 8192) wn = 8192;
                write_all_outputs(job, ptr2, wn);
                ptr2 += wn;
                rem  -= (ssize_t)wn;
            }
        }
        free(chunk);
    }
done:
    close_all_outputs(job);
    free(job->samples);
    free(job);
    return NULL;
}

static void run_cmd(const char *cmd) { int r = system(cmd); (void)r; }

/* Returns the sink's current volume as a percentage, or -1 if it could not
 * be determined (in which case the caller should just set it). */
static int query_sink_volume_pct(const char *pactl, const char *sink) {
    char cmd[640];
    snprintf(cmd, sizeof(cmd), "%s get-sink-volume %s 2>/dev/null", pactl, sink);
    FILE *f = popen(cmd, "r");
    if (!f) return -1;

    int pct = -1;
    char line[512];
    if (fgets(line, sizeof(line), f)) {
        char *p = strchr(line, '/');
        if (p) {
            /* Landed on "front-left: N /  PCT% / ..." — walk to the '%' after this '/'. */
            char *pct_str = p + 1;
            while (*pct_str == ' ') pct_str++;
            pct = atoi(pct_str);
        }
    }
    pclose(f);
    return pct;
}

/* The single volume knob for the whole program: sets your actual system
 * output level to exactly `pct` (0-100), on every active sink, right
 * now — raising or lowering as needed, whichever direction gets there.
 * Every sound in the app (alarm, sleep-mode phases, `timer test`) goes
 * through this instead of any separate app-level/PCM gain, so 100% is
 * always "full system volume" and every configured percent means
 * exactly what it says. Unmutes each sink first (a sound that's
 * supposed to be heard shouldn't stay silent because something else
 * muted your output earlier). Skips the pactl call when a sink is
 * already at the target — no point spawning a process to set what's
 * already set. */
void set_system_volume(int pct) {
    const char *pactl_paths[] = {"/usr/bin/pactl", "/usr/local/bin/pactl", NULL};
    const char *pactl = NULL;
    for (int i = 0; pactl_paths[i]; i++) {
        if (access(pactl_paths[i], X_OK) == 0) { pactl = pactl_paths[i]; break; }
    }
    if (!pactl) return;

    char sinks[MAX_AUDIO_OUTPUTS][128];
    int n = enumerate_sinks(sinks, MAX_AUDIO_OUTPUTS);
    if (n == 0) {
        char cmd[256];
        snprintf(cmd, sizeof(cmd), "%s set-sink-mute @DEFAULT_SINK@ 0 2>/dev/null", pactl);
        run_cmd(cmd);
        snprintf(cmd, sizeof(cmd), "%s set-sink-volume @DEFAULT_SINK@ %d%% 2>/dev/null", pactl, pct);
        run_cmd(cmd);
        return;
    }

    for (int i = 0; i < n; i++) {
        char cmd[640];
        snprintf(cmd, sizeof(cmd), "%s set-sink-mute %s 0 2>/dev/null", pactl, sinks[i]);
        run_cmd(cmd);

        int current = query_sink_volume_pct(pactl, sinks[i]);
        if (current == pct) continue;

        snprintf(cmd, sizeof(cmd), "%s set-sink-volume %s %d%% 2>/dev/null",
                 pactl, sinks[i], pct);
        run_cmd(cmd);
    }
}

/* ── Persistent cue backend ───────────────────────────────
   UI cues (start/pause/stop/half/...) fire far more often than the alarm
   and are short, so spawning a fresh pacat/pw-play/aplay process per sink
   for every single cue wastes a fork+exec's worth of latency and process
   churn each time. Instead, keep one set of player processes open and
   reuse it across cues, respawning lazily only if a player has died (e.g.
   the audio server restarted). A mutex serializes writes so two cues
   fired back to back don't interleave their samples on the wire. */

static pthread_mutex_t g_cue_lock = PTHREAD_MUTEX_INITIALIZER;
static AudioOutput     g_cue_outs[MAX_AUDIO_OUTPUTS];
static int             g_cue_n = 0;
static int             g_cue_ready = 0;

typedef struct { int16_t *samples; int n; App *app; } CueJob;

static void *cue_writer_thread(void *arg) {
    CueJob *cj = (CueJob *)arg;

    pthread_mutex_lock(&g_cue_lock);

    if (g_cue_ready) {
        /* waitpid(WNOHANG), not kill(pid,0) — a zombie still answers
         * kill(pid,0) successfully, so that check would miss a player
         * that already exited (e.g. killed alongside the alarm via
         * kill_all_audio) but hasn't been reaped yet. */
        int alive = 1;
        for (int i = 0; i < g_cue_n; i++)
            if (waitpid(g_cue_outs[i].pid, NULL, WNOHANG) != 0) { alive = 0; break; }
        if (!alive) {
            for (int i = 0; i < g_cue_n; i++) close(g_cue_outs[i].fd);
            g_cue_n = 0;
            g_cue_ready = 0;
        }
    }
    if (!g_cue_ready) {
        g_cue_n = spawn_all_outputs(g_cue_outs, MAX_AUDIO_OUTPUTS);
        if (g_cue_n > 0) {
            for (int i = 0; i < g_cue_n; i++)
                track_audio_pid(cj->app, g_cue_outs[i].pid);
            g_cue_ready = 1;
        }
    }

    if (g_cue_ready) {
        ssize_t total = (ssize_t)cj->n * 2;
        const uint8_t *ptr = (const uint8_t *)cj->samples;
        while (total > 0) {
            size_t chunk = (size_t)total;
            if (chunk > 8192) chunk = 8192;
            for (int i = 0; i < g_cue_n; i++)
                write_all_fd(g_cue_outs[i].fd, ptr, chunk);
            ptr   += chunk;
            total -= (ssize_t)chunk;
        }
    }

    pthread_mutex_unlock(&g_cue_lock);

    free(cj->samples);
    free(cj);
    return NULL;
}

void play_cue(App *app, const char *kind) {
    int n = 0;
    int16_t *samples = generate_cue(kind, &n);
    if (!samples || n == 0) { free(samples); return; }

    CueJob *cj = (CueJob *)calloc(1, sizeof(CueJob));
    if (!cj) { free(samples); return; }
    cj->samples = samples;
    cj->n       = n;
    cj->app     = app;

    pthread_t tid; pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_create(&tid, &attr, cue_writer_thread, cj);
    pthread_attr_destroy(&attr);
}

/* ── Sleep mode ambient/wake audio ───────────────────────────
   Three phases, all procedurally generated (no user-supplied audio
   files) — each phase is seeded fresh from the system clock every time
   it starts, so no two nights sound identical:
     - preparing: rain — filtered noise bed + periodic droplet impulses.
                  Played once, through, at the start ("falling asleep").
     - sleeping:  a slow, detuned-oscillator drone + LFO ("sleep bed"),
                  looped for the bulk of the timer.
     - waking:    a gentle rising chime/pad, looped from shortly before
                  the timer ends until the alert is dismissed, in place
                  of the usual siren. */

typedef struct {
    App          *app;
    volatile int *active_flag;
    AudioOutput   outputs[MAX_AUDIO_OUTPUTS];
    int           n_outputs;
    double        total_seconds;
} SleepJob;

typedef enum { SLEEP_SYN_RAIN, SLEEP_SYN_BED, SLEEP_SYN_WAKE } SleepSynMode;


/* Volume enforcement: how often (seconds) a running phase pulls the
 * system mixer back to its configured level. Checked every chunk inside
 * synth_sleep_segment() below (cheap — reassert_phase_volume itself
 * no-ops until this interval has elapsed), so an external volume change
 * (user turning a knob, another app raising/lowering the mixer) is
 * corrected within about a second instead of drifting until the next
 * phase transition. This is what makes configured volumes "strict" —
 * they hold at all times a sound is playing, not just at its start. */
#define VOLUME_REASSERT_INTERVAL_SEC 1.0

static void reassert_phase_volume(int pct, double *last_check_mono) {
    double now = monotonic_now();
    if (now - *last_check_mono < VOLUME_REASSERT_INTERVAL_SEC) return;
    *last_check_mono = now;
    set_system_volume(pct);
}

/* Streams one procedurally-generated phase for up to `duration` seconds
 * (or until *active_flag drops, or `deadline_mono` — fading out cleanly
 * rather than cutting), dispatching each chunk to the right generator
 * above. `t_base` carries continuous oscillator time across repeated
 * calls for the same phase (so a looped phase doesn't click/restart);
 * each *phase* still gets a freshly-seeded generator state, so every
 * phase-start is its own independent random draw. `pct`/`last_check`
 * pin the system volume to the configured level for the whole duration
 * of this call, not just when the caller happens to loop back — pass
 * `last_check` as NULL to skip (short auditions don't need it). */
static void synth_sleep_segment(AudioOutput *outs, int n_outs, volatile int *active_flag,
                                 SleepSynMode mode, double duration, double deadline_mono,
                                 double *t_base, int pct, double *last_check) {
    const int CHUNK = SAMPLE_RATE / 20;
    int16_t *chunk = (int16_t *)malloc(CHUNK * sizeof(int16_t));
    if (!chunk) return;

    RainState rain_st = {0}; rain_st.rng = sleep_seed();
    float    lp_state = 0.0f;
    uint32_t rng2      = sleep_seed();
    long phase = 0;

    while (*active_flag) {
        double seg_t = (double)phase / SAMPLE_RATE;
        if (seg_t >= duration) break;

        if (last_check) reassert_phase_volume(pct, last_check);

        double now = monotonic_now();
        double fade_out = 1.0;
        if (deadline_mono >= 0.0) {
            double remain = deadline_mono - now;
            if (remain <= 0.0) break;
            if (remain < g_config.sleep_fade_seconds) fade_out = remain / g_config.sleep_fade_seconds;
        }

        double elapsed0 = *t_base + (double)phase / SAMPLE_RATE;
        double fade_in = elapsed0 < g_config.sleep_fade_seconds ? elapsed0 / g_config.sleep_fade_seconds : 1.0;
        float env = (float)(fade_in * fade_out);
        if (mode == SLEEP_SYN_RAIN)
            env *= elapsed0 < duration ? smootherstep((float)(elapsed0 / duration)) : 1.0f;

        switch (mode) {
        case SLEEP_SYN_RAIN:
            generate_rain_pcm(chunk, CHUNK, &rain_st, *t_base, phase, env);
            break;
        case SLEEP_SYN_BED:
            generate_sleep_bed_pcm(chunk, CHUNK, &lp_state, &rng2, *t_base, phase, env);
            break;
        case SLEEP_SYN_WAKE:
            generate_waking_pcm(chunk, CHUNK, &lp_state, &rng2, *t_base, phase, duration, env);
            break;
        }

        phase += CHUNK;

        ssize_t rem = CHUNK * (ssize_t)sizeof(int16_t);
        const uint8_t *ptr = (const uint8_t *)chunk;
        while (rem > 0) {
            size_t wn = (size_t)rem;
            if (wn > 8192) wn = 8192;
            for (int i = 0; i < n_outs; i++)
                write_all_fd(outs[i].fd, ptr, wn);
            ptr += wn;
            rem -= (ssize_t)wn;
        }
    }
    *t_base += (double)phase / SAMPLE_RATE;
    free(chunk);
}


/* Sets the system volume for the given phase and remembers it, so the
 * caller can periodically re-assert it (see reassert_phase_volume())
 * without spamming pactl every chunk. */
static void set_phase_volume(double pct, int *last_pct) {
    set_system_volume((int)pct);
    *last_pct = (int)pct;
}

static void *sleep_control_thread(void *arg) {
    SleepJob *job = (SleepJob *)arg;
    double start_mono = monotonic_now();

    double intro_len, wake_len;
    sleep_phase_lens(job->total_seconds, &intro_len, &wake_len);

    /* Absolute deadline by which preparing/sleeping must have handed off
     * to waking, so waking (highest priority) always gets its full
     * window and is never itself truncated. Unbounded (-1) only if the
     * timer has no fixed duration, which shouldn't normally happen. */
    double sleep_phase_end = job->total_seconds > 0.0
        ? start_mono + (job->total_seconds > wake_len ? job->total_seconds - wake_len : 0.0)
        : -1.0;

    int last_pct; double last_check = monotonic_now();

    /* Phase 1: preparing — rain, played once through, cut only (with a
     * fade) if it doesn't fit before the deadline. */
    set_phase_volume(g_config.sleep_volume_preparing_percent, &last_pct);
    if (*job->active_flag) {
        double t_base = 0.0;
        synth_sleep_segment(job->outputs, job->n_outputs, job->active_flag,
                             SLEEP_SYN_RAIN, intro_len, sleep_phase_end, &t_base,
                             last_pct, &last_check);
    }

    /* Phase 2: sleeping — slow drone, looped to fill whatever's left
     * before the deadline, faded out right at it. */
    if (*job->active_flag) set_phase_volume(g_config.sleep_volume_sleeping_percent, &last_pct);
    {
        double t_base = 0.0;
        while (*job->active_flag) {
            if (sleep_phase_end >= 0.0 && monotonic_now() >= sleep_phase_end) break;
            synth_sleep_segment(job->outputs, job->n_outputs, job->active_flag,
                                 SLEEP_SYN_BED, 20.0, sleep_phase_end, &t_base,
                                 last_pct, &last_check);
        }
    }

    /* Phase 3: waking — highest priority: always plays through in full
     * (no deadline), looped until the alert is dismissed. */
    if (*job->active_flag) set_phase_volume(g_config.sleep_volume_waking_percent, &last_pct);
    {
        double t_base = 0.0;
        while (*job->active_flag) {
            synth_sleep_segment(job->outputs, job->n_outputs, job->active_flag,
                                 SLEEP_SYN_WAKE, 12.0, -1.0, &t_base,
                                 last_pct, &last_check);
        }
    }

    for (int i = 0; i < job->n_outputs; i++) close(job->outputs[i].fd);
    free(job);
    return NULL;
}

static void fmt_clock_offset(time_t base, double offset_secs, char *out, size_t out_sz) {
    time_t t = base + (time_t)offset_secs;
    struct tm *lt = localtime(&t);
    strftime(out, out_sz, "%H:%M:%S", lt);
}

/* Computes the phase plan for a sleep-mode timer of `total_seconds` —
 * purely time-based now that both phases are procedurally generated —
 * without starting any audio. Shared by the "/testsleep" CLI command
 * and the GUI preview overlay. Caller frees the result. */
SleepPreviewData *sleep_build_preview(double total_seconds) {
    SleepPreviewData *d = (SleepPreviewData *)calloc(1, sizeof(SleepPreviewData));
    if (!d) return NULL;
    d->total_seconds = total_seconds;

    sleep_phase_lens(total_seconds, &d->intro_len, &d->wake_len);
    d->sleep_end = total_seconds > d->wake_len ? total_seconds - d->wake_len : 0.0;
    return d;
}

/* `/testsleep <time>` — prints the phase plan to stdout, no GUI needed. */
void sleep_print_preview(const SleepPreviewData *d, const char *raw_input) {
    time_t now = time(NULL);
    char dur_buf[32], t_start[16], t_sleep[16], t_wake[16], t_end[16];
    format_time((int)d->total_seconds, dur_buf, sizeof(dur_buf));
    fmt_clock_offset(now, 0.0,             t_start, sizeof(t_start));
    fmt_clock_offset(now, d->intro_len,    t_sleep, sizeof(t_sleep));
    fmt_clock_offset(now, d->sleep_end,    t_wake,  sizeof(t_wake));
    fmt_clock_offset(now, d->total_seconds, t_end,  sizeof(t_end));

    printf("Sleep mode preview\n");
    printf("  input:     \"%s\"\n", raw_input ? raw_input : "");
    printf("  duration:  %s  (%.0fs from now, ends ~%s)\n\n", dur_buf, d->total_seconds, t_end);

    printf("Phase 1 — preparing (falls asleep to this; plays fully if it fits)\n");
    printf("  %s -> %s\n", t_start, d->sleep_end > 0 ? t_wake : t_end);
    printf("  system volume: %.0f%%\n", g_config.sleep_volume_preparing_percent);
    printf("  source: generated — rain (filtered noise + droplets), fades in over %.0fs\n", d->intro_len);

    printf("\nPhase 2 — sleeping (lowest priority; loops/cuts to fit)\n");
    printf("  %s -> %s  (%.0fs)\n", t_sleep, t_wake, d->sleep_end - d->intro_len > 0 ? d->sleep_end - d->intro_len : 0.0);
    printf("  system volume: %.0f%%\n", g_config.sleep_volume_sleeping_percent);
    printf("  source: generated — slow detuned-oscillator drone + LFO, looped\n");

    printf("\nPhase 3 — waking (highest priority; always plays in full)\n");
    printf("  %s -> dismissed  (wake window budgeted at %.0fs, then loops until stopped)\n", t_wake, d->wake_len);
    printf("  system volume: %.0f%%\n", g_config.sleep_volume_waking_percent);
    printf("  source: generated — gentle rising chime over a soft pad, looped\n");

    printf("\nAll transitions fade in/out over %.1fs — no hard cuts.\n", (double)g_config.sleep_fade_seconds);
}

/* ── Audition (GUI "Play preview" button) ────────────────────
   Plays a short snippet of each phase back-to-back — preparing, then
   sleeping, then waking — so the fades can be auditioned without
   waiting out the real timer. Reuses the same deadline-based fade
   machinery as the real playback, just with a short deadline per phase.
   Stops on its own once all three snippets have played, or immediately
   if kill_all_audio() is called (e.g. the user hits "Stop"). */
#define SLEEP_AUDITION_SEC 4.0

static void *sleep_audition_thread(void *arg) {
    SleepJob *job = (SleepJob *)arg;

    double t_base;
    double deadline = monotonic_now() + SLEEP_AUDITION_SEC;
    set_system_volume((int)g_config.sleep_volume_preparing_percent);
    if (*job->active_flag) {
        t_base = 0.0;
        synth_sleep_segment(job->outputs, job->n_outputs, job->active_flag,
                             SLEEP_SYN_RAIN, SLEEP_AUDITION_SEC, deadline, &t_base, 0, NULL);
    }

    deadline = monotonic_now() + SLEEP_AUDITION_SEC;
    if (*job->active_flag) {
        set_system_volume((int)g_config.sleep_volume_sleeping_percent);
        t_base = 0.0;
        synth_sleep_segment(job->outputs, job->n_outputs, job->active_flag,
                             SLEEP_SYN_BED, SLEEP_AUDITION_SEC, deadline, &t_base, 0, NULL);
    }

    deadline = monotonic_now() + SLEEP_AUDITION_SEC;
    if (*job->active_flag) {
        set_system_volume((int)g_config.sleep_volume_waking_percent);
        t_base = 0.0;
        synth_sleep_segment(job->outputs, job->n_outputs, job->active_flag,
                             SLEEP_SYN_WAKE, SLEEP_AUDITION_SEC, deadline, &t_base, 0, NULL);
    }

    *job->active_flag = 0;
    for (int i = 0; i < job->n_outputs; i++) close(job->outputs[i].fd);
    free(job);
    return NULL;
}

void play_sleep_audition(App *app, const SleepPreviewData *d) {
    if (!d) return;
    kill_all_audio(app);
    app->sleep_audio_active = 1;

    SleepJob *job = (SleepJob *)calloc(1, sizeof(SleepJob));
    if (!job) return;

    job->n_outputs = spawn_all_outputs(job->outputs, MAX_AUDIO_OUTPUTS);
    if (job->n_outputs <= 0) { free(job); app->sleep_audio_active = 0; return; }
    for (int i = 0; i < job->n_outputs; i++)
        track_audio_pid(app, job->outputs[i].pid);

    job->app         = app;
    job->active_flag = &app->sleep_audio_active;

    pthread_t tid; pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_create(&tid, &attr, sleep_audition_thread, job);
    pthread_attr_destroy(&attr);
}

void play_sleep_ambient(App *app, double total_seconds) {
    kill_all_audio(app);
    app->sleep_audio_active = 1;

    SleepJob *job = (SleepJob *)calloc(1, sizeof(SleepJob));
    if (!job) return;

    job->n_outputs = spawn_all_outputs(job->outputs, MAX_AUDIO_OUTPUTS);
    if (job->n_outputs <= 0) { free(job); return; }

    for (int i = 0; i < job->n_outputs; i++)
        track_audio_pid(app, job->outputs[i].pid);

    job->app            = app;
    job->active_flag    = &app->sleep_audio_active;
    job->total_seconds  = total_seconds;

    pthread_t tid; pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_create(&tid, &attr, sleep_control_thread, job);
    pthread_attr_destroy(&attr);
}

/* `timer test <phase>` — foreground CLI audition of one procedurally
 * generated sleep-mode phase, at the exact same SYSTEM volume the app
 * itself would set for that phase (see set_system_volume() — the one
 * function in this whole program that ever touches your system volume).
 * Blocks for `seconds`, or until Ctrl+C. No files, no mpv/ffmpeg — just
 * the same generator used by the real sleep-mode phases. */
void play_test_phase_cli(const char *phase_name, int volume_pct, double seconds) {
    AudioOutput outs[MAX_AUDIO_OUTPUTS];
    int n_outs = spawn_all_outputs(outs, MAX_AUDIO_OUTPUTS);
    if (n_outs <= 0) { fprintf(stderr, "[%s] could not start an audio output\n", phase_name); return; }

    SleepSynMode mode = SLEEP_SYN_BED;
    if (!strcmp(phase_name, "preparing")) mode = SLEEP_SYN_RAIN;
    else if (!strcmp(phase_name, "sleeping")) mode = SLEEP_SYN_BED;
    else if (!strcmp(phase_name, "waking") || !strcmp(phase_name, "alarm")) mode = SLEEP_SYN_WAKE;

    printf("[%s] system volume -> %d%%, playing generated audio for %.0fs (Ctrl+C to stop)\n",
           phase_name, volume_pct, seconds);
    set_system_volume(volume_pct);

    volatile int active = 1;
    double t_base = 0.0;
    double last_check = monotonic_now();
    double deadline = monotonic_now() + seconds;
    while (active && monotonic_now() < deadline) {
        synth_sleep_segment(outs, n_outs, &active, mode, 12.0, -1.0, &t_base,
                             volume_pct, &last_check);
    }

    for (int i = 0; i < n_outs; i++) { close(outs[i].fd); kill(outs[i].pid, SIGTERM); waitpid(outs[i].pid, NULL, 0); }
}

void play_alert_sound(App *app) {
    /* Raise system volume to the alarm's starting floor right away — see
     * the "Volume guarantee" note at the top of config.c. It then ramps
     * up to alarm_volume_max_percent as the PCM fade-in progresses (see
     * audio_writer_thread()). */
    set_system_volume((int)g_config.alarm_volume_min_percent);
    kill_all_audio(app);
    app->alert_audio_active = 1;

    AudioJob *job = (AudioJob *)calloc(1, sizeof(AudioJob));
    if (!job) return;

    job->n_outputs = spawn_all_outputs(job->outputs, MAX_AUDIO_OUTPUTS);
    if (job->n_outputs <= 0) { free(job); return; }

    for (int i = 0; i < job->n_outputs; i++)
        track_audio_pid(app, job->outputs[i].pid);

    job->samples = NULL;
    job->n_samples = 0;
    job->active_flag = &app->alert_audio_active;
    job->alert_ramp_seconds = (float)g_config.alarm_ramp_seconds;
    job->alert_volume_min_pct = (int)g_config.alarm_volume_min_percent;
    job->alert_volume_max_pct = (int)g_config.alarm_volume_max_percent;

    pthread_t tid; pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_create(&tid, &attr, audio_writer_thread, job);
    pthread_attr_destroy(&attr);
}
