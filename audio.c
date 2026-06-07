/* audio.c — PCM synthesis and playback */
#include "timer.h"

/* ── Glass / water-drop synthesis (Aero-style) ─────────────
   Pure, glassy sine partials with a quick water-drop pitch
   glide and a slow chorus detune — a liquid, nostalgic
   shimmer that blooms out into the reverb tail.              */
static void render_glass(int16_t *buf, int n,
                         float freq, float decay, float vol) {
    for (int i = 0; i < n; i++) {
        float t  = (float)i / SAMPLE_RATE;
        float at = 1.0f - expf(-t * 200.0f);          /* ~18 ms soft swell  */
        float en = at * expf(-t * decay);

        /* Water-drop glide: starts ~4 % sharp, eases down onto pitch. */
        float gl  = 1.0f + 0.04f * expf(-t * 26.0f);
        /* Slow vibrato gives the tone a gentle, liquid movement. */
        float vib = 1.0f + 0.003f * sinf((float)TAU * 5.0f * t);
        float f   = freq * gl * vib;

        /* Mostly pure sines with a touch of glassy upper shimmer. */
        float h1 = sinf((float)TAU * f          * t);
        float h2 = 0.38f * sinf((float)TAU * f * 2.0f  * t) * expf(-t * decay * 0.7f);
        float h3 = 0.14f * sinf((float)TAU * f * 4.02f * t) * expf(-t * decay * 1.5f);
        float h4 = 0.06f * sinf((float)TAU * f * 6.04f * t) * expf(-t * decay * 2.4f);
        /* Detuned twin — watery chorus thickness. */
        float ch = 0.45f * sinf((float)TAU * f * 1.0035f * t);

        float s  = (h1 + ch + h2 + h3 + h4) * en * vol * 0.70f;
        s = s >  1.0f ?  1.0f : s < -1.0f ? -1.0f : s;
        buf[i] = (int16_t)(s * 32767.0f);
    }
}

/* ── Lush hall reverb ──────────────────────────────────────
   Five diffuse taps from 23–97 ms; because each tap reads the
   already-written (echoed) buffer, the taps feed back into one
   another and bloom into a long, smooth, ethereal tail — the
   nostalgic "Aero" space.                                    */
static void apply_reverb(int16_t *buf, int n) {
    static const int   d[] = {1014, 1631, 2337, 3131, 4277};
    static const float g[] = {0.30f, 0.24f, 0.18f, 0.12f, 0.08f};
    for (int i = 0; i < n; i++) {
        float echo = 0.0f;
        for (int t = 0; t < 5; t++)
            if (i >= d[t]) echo += buf[i - d[t]] * g[t];
        int v = buf[i] + (int)echo;
        buf[i] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
    }
}

/* ── Cue note table ─────────────────────────────────────────
   Each cue is a short melodic gesture rendered with the glassy
   water-drop voice; long-ish durations + low decay let the
   notes ring and bloom into the lush reverb tail.            */
typedef struct { float freq, dur, gap, decay, vol; } Note;

static int16_t *generate_cue(const char *kind, int *out_n) {

    /* launch  — C5 E5 G5, welcoming ascending triad */
    static const Note launch[] = {
        {523.25f, 0.16f, 0.020f, 3.0f, 0.14f},
        {659.25f, 0.16f, 0.020f, 2.8f, 0.15f},
        {783.99f, 0.34f, 0.000f, 2.4f, 0.14f},
    };
    /* start   — G4 B4 D5, confident rise */
    static const Note start_n[] = {
        {392.00f, 0.15f, 0.020f, 3.0f, 0.13f},
        {493.88f, 0.15f, 0.020f, 2.8f, 0.14f},
        {587.33f, 0.32f, 0.000f, 2.4f, 0.13f},
    };
    /* pause   — D5 A4, dropping away */
    static const Note pause_n[] = {
        {587.33f, 0.16f, 0.020f, 3.0f, 0.12f},
        {440.00f, 0.34f, 0.000f, 2.4f, 0.12f},
    };
    /* resume  — A4 D5 F#5, rebuilding */
    static const Note resume_n[] = {
        {440.00f, 0.14f, 0.020f, 3.0f, 0.12f},
        {587.33f, 0.14f, 0.020f, 2.8f, 0.13f},
        {739.99f, 0.30f, 0.000f, 2.4f, 0.12f},
    };
    /* stop    — G5 E5 C5, descending resolution */
    static const Note stop_n[] = {
        {783.99f, 0.14f, 0.020f, 3.0f, 0.12f},
        {659.25f, 0.14f, 0.020f, 2.8f, 0.12f},
        {523.25f, 0.34f, 0.000f, 2.2f, 0.12f},
    };
    /* close   — C5 G4, quick exit */
    static const Note close_n[] = {
        {523.25f, 0.15f, 0.020f, 3.0f, 0.11f},
        {392.00f, 0.32f, 0.000f, 2.2f, 0.11f},
    };
    /* half    — F5 A5, neutral checkpoint */
    static const Note half_n[] = {
        {698.46f, 0.15f, 0.020f, 2.8f, 0.11f},
        {880.00f, 0.32f, 0.000f, 2.4f, 0.11f},
    };
    /* last    — D6 G6, crisp bright pair */
    static const Note last_n[] = {
        {1174.66f, 0.12f, 0.020f, 3.0f, 0.12f},
        {1567.98f, 0.28f, 0.000f, 2.6f, 0.12f},
    };
    /* dismiss — B4 G4 E4, resolved descent */
    static const Note dismiss_n[] = {
        {493.88f, 0.15f, 0.020f, 2.8f, 0.12f},
        {392.00f, 0.15f, 0.020f, 2.6f, 0.12f},
        {329.63f, 0.34f, 0.000f, 2.2f, 0.11f},
    };
    /* alert_open — C5 E5 G5 C6, four-note ascending call */
    static const Note alert_open[] = {
        { 523.25f, 0.13f, 0.020f, 3.0f, 0.14f},
        { 659.25f, 0.13f, 0.020f, 2.8f, 0.15f},
        { 783.99f, 0.13f, 0.020f, 2.6f, 0.15f},
        {1046.50f, 0.30f, 0.000f, 2.4f, 0.14f},
    };

    const Note *notes = start_n;
    int n_notes = 3;
#define PICK(k, arr, cnt) if (!strcmp(kind, k)) { notes = arr; n_notes = cnt; }
    PICK("launch",     launch,     3)
    PICK("start",      start_n,    3)
    PICK("pause",      pause_n,    2)
    PICK("resume",     resume_n,   3)
    PICK("stop",       stop_n,     3)
    PICK("close",      close_n,    2)
    PICK("half",       half_n,     2)
    PICK("last",       last_n,     2)
    PICK("dismiss",    dismiss_n,  3)
    PICK("alert_open", alert_open, 4)
#undef PICK

    /* Extra tail so the lush reverb can bloom and ring out past the
     * last note — ~0.9 s of space for that ethereal, nostalgic decay. */
    const int REVERB_TAIL = (SAMPLE_RATE * 9) / 10;

    int body = 0;
    for (int i = 0; i < n_notes; i++)
        body += (int)(SAMPLE_RATE * notes[i].dur)
              + (int)(SAMPLE_RATE * notes[i].gap);
    int total = body + REVERB_TAIL;
    if (total <= 0) { *out_n = 0; return NULL; }

    int16_t *buf = (int16_t *)calloc(total, sizeof(int16_t));
    if (!buf)      { *out_n = 0; return NULL; }

    /* Master gain to keep cues soft and easy on the ears. */
    const float CUE_GAIN = 0.70f;

    int off = 0;
    for (int i = 0; i < n_notes; i++) {
        int n_s = (int)(SAMPLE_RATE * notes[i].dur);
        render_glass(buf + off, n_s, notes[i].freq, notes[i].decay, notes[i].vol * CUE_GAIN);
        off += n_s + (int)(SAMPLE_RATE * notes[i].gap);
    }

    apply_reverb(buf, total);

    *out_n = total;
    return buf;
}

/* ── Playback ──────────────────────────────────────────────── */

static const char *PLAYER_CMDS[][14] = {
    {"aplay", "-q", "-f", "S16_LE", "-r", "44100", "-c", "1", NULL},
    {"pacat", "--raw", "--format=s16le", "--rate=44100", "--channels=1", NULL},
    {"pw-play", "--format=s16", "--rate=44100", "--channels=1", "-", NULL},
    {"ffplay", "-v", "error", "-nodisp", "-autoexit",
     "-f", "s16le", "-ar", "44100", "-ac", "1", "pipe:0", NULL},
    {"mpv", "--no-video", "--no-terminal", "--really-quiet",
     "--demuxer=rawaudio", "--demuxer-rawaudio-format=s16le",
     "--demuxer-rawaudio-rate=44100", "--demuxer-rawaudio-channels=1", "-", NULL},
    {NULL},
};

static pid_t spawn_player(int *stdin_fd) {
    for (int ci = 0; PLAYER_CMDS[ci][0]; ci++) {
        char path[256];
        snprintf(path, sizeof(path), "/usr/bin/%s", PLAYER_CMDS[ci][0]);
        if (access(path, X_OK) != 0) {
            snprintf(path, sizeof(path), "/usr/local/bin/%s", PLAYER_CMDS[ci][0]);
            if (access(path, X_OK) != 0) continue;
        }
        int pipefd[2];
        if (pipe(pipefd) < 0) continue;
        pid_t pid = fork();
        if (pid < 0) { close(pipefd[0]); close(pipefd[1]); continue; }
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
            execvp(PLAYER_CMDS[ci][0], (char *const *)PLAYER_CMDS[ci]);
            _exit(1);
        }
        close(pipefd[0]);
        *stdin_fd = pipefd[1];
        return pid;
    }
    *stdin_fd = -1;
    return -1;
}

static void track_audio_pid(App *app, pid_t pid) {
    pthread_mutex_lock(&app->audio_lock);
    int j = 0;
    for (int i = 0; i < app->n_audio_pids; i++) {
        if (waitpid(app->audio_pids[i], NULL, WNOHANG) == 0)
            app->audio_pids[j++] = app->audio_pids[i];
    }
    app->n_audio_pids = j;
    if (app->n_audio_pids < 8)
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
   An insistent but still musical chime — brighter and more
   urgent than the cues.  A six-note C-major-pentatonic motif
   (C5 E5 G5 C6 G5 E5) drives over a brisk 2.4 s period in a
   bright register, each note a clear bell with a crisp strike
   and ringing upper partials so it cuts through.  Consonant,
   no saturation, but assertive: it swells in fairly quickly
   and reaches a strong, attention-grabbing volume.

   Period 2.4 s — strikes at 0.0, 0.4, 0.8, 1.2, 1.65, 2.05 s:
     C5 523 · E5 659 · G5 784 · C6 1047 · G5 784 · E5 659      */

/* ALARM_PERIOD, ALARM_N and the onsets live in timer.h so the alert
 * window can flash its colors in time with these strikes. */
static const float AL_ONSET[ALARM_N] = ALARM_ONSETS;
static const float AL_FREQ [ALARM_N] = {523.25f, 659.25f, 783.99f, 1046.50f, 783.99f, 659.25f};
static const float AL_DECAY[ALARM_N] = {3.2f,    3.2f,    3.0f,    2.8f,    3.2f,   3.4f};
static const float AL_AMP  [ALARM_N] = {0.95f,   0.85f,   0.85f,   0.80f,   0.70f,  0.65f};

static void *audio_writer_thread(void *arg) {
    AudioJob *job = (AudioJob *)arg;
    if (job->active_flag == NULL) {
        /* One-shot cue */
        ssize_t total = job->n_samples * 2;
        const uint8_t *ptr = (const uint8_t *)job->samples;
        while (total > 0) {
            ssize_t w = write(job->fd_write, ptr, total);
            if (w <= 0) break;
            ptr += w; total -= w;
        }
    } else {
        /* Streaming alarm */
        const int CHUNK = SAMPLE_RATE / 20;
        int16_t *chunk  = (int16_t *)malloc(CHUNK * sizeof(int16_t));
        if (!chunk) goto done;

        int phase = 0;

        while (*job->active_flag) {
            float elapsed = (float)phase / SAMPLE_RATE;
            float t = (job->alert_ramp_seconds > 0.1f)
                      ? (elapsed / job->alert_ramp_seconds) : 1.0f;
            if (t > 1.0f) t = 1.0f;
            /* Smootherstep fade-in: starts quiet and rises smoothly to full
             * over alert_ramp_seconds (~5 s). */
            float rs   = t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f);
            float ramp = 0.06f + 0.94f * rs;

            /* Aggression grows with time: more shimmer, grit and level. */
            float ag = (float)(elapsed / ALARM_AGGRO_SECS);
            if (ag > 1.0f) ag = 1.0f;

            for (int i = 0; i < CHUNK; i++) {
                double tt     = (double)(phase + i) / SAMPLE_RATE;
                /* Accelerating tempo: musical phase + current period length. */
                double ph     = alarm_phase(tt);
                double persec = 1.0 / alarm_speed(tt);
                double fp     = ph - floor(ph);          /* 0..1 within period */

                float breath = 0.96f + 0.04f * sinf((float)TAU * 0.12f * (float)tt);

                float s = 0.0f;
                for (int j = 0; j < ALARM_N; j++) {
                    /* Onset as a fraction of the period; real seconds since it. */
                    double onf = (double)AL_ONSET[j] / ALARM_PERIOD;
                    double dph = fp - onf;
                    if (dph < 0.0) dph += 1.0;            /* ring from prev period */
                    float dt = (float)(dph * persec);

                    float dc   = AL_DECAY[j];
                    float freq = AL_FREQ[j];

                    /* Crisp ~6 ms strike — gives each note a clear, urgent edge. */
                    float at2  = 1.0f - expf(-dt * 170.0f);
                    float ring = expf(-dt * dc);

                    /* Bright, consonant bell — fundamental plus ringing upper
                     * partials; the shimmer partial grows with aggression. */
                    float h1   = sinf((float)TAU * freq         * dt);
                    float oct  = 0.40f * sinf((float)TAU * freq * 2.0f * dt)
                                       * expf(-dt * dc * 1.0f);
                    float twf  = 0.24f * sinf((float)TAU * freq * 3.0f * dt)
                                       * expf(-dt * dc * 1.5f);
                    float shm  = (0.12f + 0.30f * ag) * sinf((float)TAU * freq * 5.0f * dt)
                                       * expf(-dt * dc * 2.4f);
                    /* Sub-octave keeps some body under the brightness. */
                    float warm = 0.14f * sinf((float)TAU * freq * 0.5f * dt)
                                       * expf(-dt * dc * 0.9f);

                    float tone = (h1 + oct + twf + shm + warm) * ring * at2;
                    s += tone * AL_AMP[j];
                }

                /* Saturation drive ramps up with aggression — adds bite/grit. */
                float drive = 1.0f + 4.0f * ag;
                s = tanhf(s * drive) / (1.0f + 0.6f * ag);

                float level = 0.52f + 0.30f * ag;
                s *= ramp * breath * level;
                if (s >  0.96f) s =  0.96f;
                if (s < -0.96f) s = -0.96f;
                chunk[i] = (int16_t)(s * 32767.0f);
            }

            phase += CHUNK;

            ssize_t rem = CHUNK * (ssize_t)sizeof(int16_t);
            const uint8_t *ptr2 = (const uint8_t *)chunk;
            while (rem > 0) {
                ssize_t w = write(job->fd_write, ptr2, rem);
                if (w <= 0) goto done_loop;
                ptr2 += w; rem -= w;
            }
        }
done_loop:
        free(chunk);
    }
done:
    close(job->fd_write);
    free(job->samples);
    free(job);
    return NULL;
}

static void run_cmd(const char *cmd) { int r = system(cmd); (void)r; }

static void ensure_volume(void) {
    if (access("/usr/bin/pactl", X_OK) != 0) return;
    run_cmd("pactl set-sink-mute @DEFAULT_SINK@ 0 2>/dev/null");
    FILE *f = popen("pactl get-sink-volume @DEFAULT_SINK@ 2>/dev/null", "r");
    if (f) {
        char line[256] = "";
        if (fgets(line, sizeof(line), f)) {
            char *pct = strstr(line, "%");
            if (pct) {
                const char *p = pct - 1;
                while (p > line && isdigit((unsigned char)*p)) p--;
                if (atoi(p + 1) < 30)
                    run_cmd("pactl set-sink-volume @DEFAULT_SINK@ 30% 2>/dev/null");
            }
        }
        pclose(f);
    }
}

void play_cue(App *app, const char *kind) {
    ensure_volume();
    int n = 0;
    int16_t *samples = generate_cue(kind, &n);
    if (!samples || n == 0) { free(samples); return; }
    int fd;
    pid_t pid = spawn_player(&fd);
    if (pid < 0) { free(samples); return; }
    track_audio_pid(app, pid);
    AudioJob *job = (AudioJob *)calloc(1, sizeof(AudioJob));
    if (!job) { free(samples); close(fd); return; }
    job->samples = samples; job->n_samples = n;
    job->fd_write = fd; job->active_flag = NULL;
    pthread_t tid; pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_create(&tid, &attr, audio_writer_thread, job);
    pthread_attr_destroy(&attr);
}

void play_alert_sound(App *app) {
    ensure_volume();
    kill_all_audio(app);
    app->alert_audio_active = 1;
    int fd;
    pid_t pid = spawn_player(&fd);
    if (pid < 0) return;
    track_audio_pid(app, pid);
    AudioJob *job = (AudioJob *)calloc(1, sizeof(AudioJob));
    if (!job) { close(fd); return; }
    job->samples = NULL; job->n_samples = 0;
    job->fd_write = fd; job->active_flag = &app->alert_audio_active;
    job->alert_ramp_seconds = 5.0f;    /* start quiet, fade in smoothly over ~5 s */
    pthread_t tid; pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_create(&tid, &attr, audio_writer_thread, job);
    pthread_attr_destroy(&attr);
}
