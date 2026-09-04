/* audio.c — PCM synthesis and playback */
#include "timer.h"

/* ── Cue synthesis (calm, low volume) / alarm (loud, aggressive) ── */

static float clampf(float x, float lo, float hi) {
    if (x < lo) return lo;
    if (x > hi) return hi;
    return x;
}

static float smootherstep(float t) {
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
static int16_t *generate_cue(const char *kind, int *out_n) {

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

static float render_alarm_tone(float freq, float dt, float dc, float bright) {
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

        while (*job->active_flag) {
            float elapsed = (float)phase / SAMPLE_RATE;

            /* Volume fade-in (length configurable) — smootherstep keeps the
             * start gentle. A zero-length ramp means "full volume immediately". */
            float fade  = job->alert_ramp_seconds > 0.0f
                          ? smootherstep(elapsed / job->alert_ramp_seconds)
                          : 1.0f;
            float ramp  = 0.04f + 0.96f * fade;

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

static void ensure_volume(int pct) {
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
        return;
    }

    for (int i = 0; i < n; i++) {
        char cmd[640];
        snprintf(cmd, sizeof(cmd), "%s set-sink-mute %s 0 2>/dev/null", pactl, sinks[i]);
        run_cmd(cmd);

        /* Never lower volume that's already above the alarm target — only
         * raise it up to the target if it's below. */
        int current = query_sink_volume_pct(pactl, sinks[i]);
        if (current >= pct) continue;

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

void play_alert_sound(App *app) {
    /* The only place in the program that ever changes system volume — see
     * the "Volume guarantee" note at the top of config.c. */
    ensure_volume((int)g_config.alarm_volume_percent);
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

    pthread_t tid; pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_create(&tid, &attr, audio_writer_thread, job);
    pthread_attr_destroy(&attr);
}
