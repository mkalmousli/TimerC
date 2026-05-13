/* audio.c — PCM synthesis and playback */
#include "timer.h"

/* ── PCM generation ────────────────────────────────────── */

static float env_gain(int i, int len, int sr) {
    float fi = 0.004f, fo = 0.02f;
    int fi_s = (int)(sr * fi), fo_s = (int)(sr * fo);
    float g = 1.0f;
    if (i < fi_s) g *= (float)i / fi_s;
    int tail = len - i;
    if (tail < fo_s) g *= (float)tail / fo_s;
    return g;
}

static void render_chime(int16_t *buf, int n,
                         float freq, float decay_rate, float vol) {
    for (int i = 0; i < n; i++) {
        float t = (float)i / SAMPLE_RATE;
        float attack = 1.0f - expf(-t * 520.0f);
        float ring   = expf(-t * decay_rate);
        float env    = attack * ring;
        float h1 = sinf((float)TAU * freq * t);
        float h2 = 0.36f * sinf((float)TAU * freq * 2.02f * t);
        float h3 = 0.13f * sinf((float)TAU * freq * 3.05f * t);
        float sp = 0.055f * sinf((float)TAU * freq * 5.12f * t);
        float body = tanhf((h1 + h2 + h3 + sp) / 1.55f * 1.08f);
        float g = vol * env * env_gain(i, n, SAMPLE_RATE);
        float s = body * g;
        if (s >  1.0f) s =  1.0f;
        if (s < -1.0f) s = -1.0f;
        buf[i] = (int16_t)(s * 32767.0f);
    }
}

typedef struct { float freq, dur, gap, decay, vol; } Note;

static int16_t *generate_cue(const char *kind, int *out_n) {
    static const Note launch[]    = {{523.25f,0.09f,0.018f,10.0f,0.125f},
                                     {783.99f,0.10f,0.025f,9.2f, 0.135f},
                                     {1046.5f,0.11f,0.035f,8.8f, 0.125f}};
    static const Note start_n[]   = {{440.0f, 0.08f,0.018f,10.0f,0.120f},
                                     {659.25f,0.09f,0.022f,9.4f, 0.130f},
                                     {987.77f,0.10f,0.03f, 8.8f, 0.125f}};
    static const Note pause_n[]   = {{783.99f,0.08f,0.018f,10.0f,0.110f},
                                     {493.88f,0.12f,0.03f, 9.0f, 0.115f}};
    static const Note resume_n[]  = {{493.88f,0.08f,0.018f,10.0f,0.115f},
                                     {659.25f,0.09f,0.022f,9.4f, 0.125f},
                                     {880.0f, 0.10f,0.03f, 8.8f, 0.120f}};
    static const Note stop_n[]    = {{880.0f, 0.07f,0.016f,10.8f,0.110f},
                                     {659.25f,0.08f,0.018f,10.0f,0.112f},
                                     {440.0f, 0.12f,0.03f, 9.0f, 0.115f}};
    static const Note close_n[]   = {{659.25f,0.08f,0.018f,10.0f,0.105f},
                                     {392.0f, 0.12f,0.03f, 9.2f, 0.110f}};
    static const Note half_n[]    = {{587.33f,0.08f,0.018f,10.0f,0.105f},
                                     {880.0f, 0.09f,0.025f,9.2f, 0.110f}};
    static const Note last_n[]    = {{1174.66f,0.065f,0.014f,11.2f,0.120f},
                                     {1567.98f,0.075f,0.018f,10.4f,0.120f}};
    static const Note dismiss_n[] = {{987.77f,0.07f,0.016f,10.5f,0.105f},
                                     {659.25f,0.09f,0.022f,9.8f, 0.110f},
                                     {493.88f,0.12f,0.03f, 9.0f, 0.108f}};
    static const Note alert_open[]= {{880.0f,   0.07f,0.014f,11.0f,0.140f},
                                     {1174.66f, 0.08f,0.018f,10.4f,0.145f},
                                     {1567.98f, 0.09f,0.025f,9.8f, 0.135f}};

    const Note *notes = start_n;
    int n_notes = 3;
#define PICK(k, arr, cnt) if(!strcmp(kind,k)){notes=arr; n_notes=cnt;}
    PICK("launch",      launch,    3)
    PICK("start",       start_n,   3)
    PICK("pause",       pause_n,   2)
    PICK("resume",      resume_n,  3)
    PICK("stop",        stop_n,    3)
    PICK("close",       close_n,   2)
    PICK("half",        half_n,    2)
    PICK("last",        last_n,    2)
    PICK("dismiss",     dismiss_n, 3)
    PICK("alert_open",  alert_open,3)
#undef PICK

    int total = 0;
    for (int i = 0; i < n_notes; i++)
        total += (int)(SAMPLE_RATE * notes[i].dur) + (int)(SAMPLE_RATE * notes[i].gap);
    if (total <= 0) { *out_n = 0; return NULL; }

    int16_t *buf = (int16_t *)calloc(total, sizeof(int16_t));
    if (!buf) { *out_n = 0; return NULL; }

    int off = 0;
    for (int i = 0; i < n_notes; i++) {
        int n_s = (int)(SAMPLE_RATE * notes[i].dur);
        render_chime(buf + off, n_s, notes[i].freq, notes[i].decay, notes[i].vol);
        off += n_s + (int)(SAMPLE_RATE * notes[i].gap);
    }
    *out_n = total;
    return buf;
}

/* ── Playback ──────────────────────────────────────────── */

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

static void *audio_writer_thread(void *arg) {
    AudioJob *job = (AudioJob *)arg;
    if (job->active_flag == NULL) {
        ssize_t total = job->n_samples * 2;
        const uint8_t *ptr = (const uint8_t *)job->samples;
        while (total > 0) {
            ssize_t w = write(job->fd_write, ptr, total);
            if (w <= 0) break;
            ptr += w; total -= w;
        }
    } else {
        int phase = 0;
        const int CHUNK = SAMPLE_RATE / 20;
        int16_t *chunk = (int16_t *)malloc(CHUNK * 2);
        if (!chunk) goto done;
        while (*job->active_flag) {
            float elapsed = (float)phase / SAMPLE_RATE;
            float t = (job->alert_ramp_seconds > 0.1f)
                      ? (elapsed / job->alert_ramp_seconds) : 1.0f;
            if (t < 0.0f) t = 0.0f;
            if (t > 1.0f) t = 1.0f;
            /* Start very gentle, then accelerate (avoids a "shock" at t=0). */
            float ramp = 0.02f + 0.98f * (t * t);
            for (int i = 0; i < CHUNK; i++) {
                float tt = (float)(phase + i) / SAMPLE_RATE;
                float chirp   = fmodf(tt, 0.20f);
                float gate    = chirp < 0.145f ? 1.0f : 0.0f;
                float attack  = fminf(1.0f, chirp / 0.004f);
                float release = fminf(1.0f, fmaxf(0.0f, (0.145f - chirp) / 0.018f));
                float env     = gate * attack * release;
                float sweep   = chirp < 0.145f ? chirp / 0.145f : 1.0f;
                float sfreq   = 820.0f + 760.0f * sweep;
                float warble  = ((int)(tt * 12) % 2) ? 1.0f : 0.72f;
                float f2      = sfreq * warble;
                float sq      = sinf((float)TAU * f2 * tt) >= 0 ? 1.0f : -1.0f;
                float pulse   = ((int)(tt * 16) % 2) ? 1.0f : -1.0f;
                float glass   = sinf((float)TAU * 2100.0f * tt) * expf(-chirp * 22.0f);
                float beep    = 0.72f * sq + 0.22f * pulse + 0.20f * glass;
                float s       = tanhf(beep * env * 1.55f) * ramp;
                if (s >  0.96f) s =  0.96f;
                if (s < -0.96f) s = -0.96f;
                chunk[i] = (int16_t)(s * 32767.0f);
            }
            phase += CHUNK;
            ssize_t total2 = CHUNK * 2;
            const uint8_t *ptr2 = (const uint8_t *)chunk;
            while (total2 > 0) {
                ssize_t w = write(job->fd_write, ptr2, total2);
                if (w <= 0) goto done_loop;
                ptr2 += w; total2 -= w;
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
                if (atoi(p+1) < 30)
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
    job->alert_ramp_seconds = 35.0f;
    pthread_t tid; pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_create(&tid, &attr, audio_writer_thread, job);
    pthread_attr_destroy(&attr);
}
