/* config.c — user-tunable settings
 *
 * No config file, on purpose: every setting is a plain value assigned
 * directly to its g_config field below in config_load() — there's no
 * separate DEFAULT_* constant to keep in sync, just the value, in place,
 * next to the comment explaining it. To change something, edit the
 * assigned value and rebuild — that's the whole "config system".
 *
 * DUR("..") parses a duration string like "1h30m", "30m20s", "500ms",
 * "2.4s", or a bare number (seconds) — see dur_sec() below. DUR_MS("..")
 * is the same, rounded to whole milliseconds, for the tick (redraw
 * interval) settings, which are stored as ints.
 *
 * ONE volume model, ONE reference point — see the big note at the top of
 * the Config struct in timer.h for the full explanation. Short version:
 * every *_volume_percent field here (alarm_volume_min/max_percent,
 * sleep_volume_preparing/sleeping/waking_percent) is your actual SYSTEM
 * output level, 0-100%, with 100% = full system volume. There's no
 * separate app-level/PCM gain anywhere — set_system_volume() in audio.c
 * (the only thing in this whole program that ever touches your system
 * volume) moves your actual speaker volume to the requested percent,
 * instantly, up or down as needed, the moment each sound starts.
 */
#include "timer.h"
#include <stddef.h>

Config g_config;

/* ── Generic field accessor table ──────────────────────────────
 * One entry per user-tunable Config field, so the Settings overlay
 * (Ctrl+G, draw.c) can enumerate/display/edit every field without any
 * per-field UI code, and so config_save()/config_load() stay in sync
 * with the struct automatically — add a field here once and it shows
 * up in the GUI, gets persisted, and gets restored, with no changes
 * needed anywhere else. */

const CfgField CFG_FIELDS[] = {
    /* key                              label                     category    type       min      max              offset */
    {"alarm_volume_min_percent",  "Min volume (start)",      "Alarm",   CFG_F_DOUBLE, 0,    100,   offsetof(Config, alarm_volume_min_percent)},
    {"alarm_volume_max_percent",  "Max volume (ramped)",     "Alarm",   CFG_F_DOUBLE, 0,    100,   offsetof(Config, alarm_volume_max_percent)},
    {"alarm_ramp_seconds",        "Fade-in length (s)",      "Alarm",   CFG_F_DOUBLE, 0,    60,    offsetof(Config, alarm_ramp_seconds)},
    {"alarm_period",              "Motif period (s)",        "Alarm",   CFG_F_DOUBLE, 0.2,  10,    offsetof(Config, alarm_period)},
    {"alarm_period_min",          "Fastest period (s)",      "Alarm",   CFG_F_DOUBLE, 0.05, 10,    offsetof(Config, alarm_period_min)},
    {"alarm_accel",               "Tempo acceleration",      "Alarm",   CFG_F_DOUBLE, 0,    5,     offsetof(Config, alarm_accel)},
    {"alarm_aggro_seconds",       "Brightness ramp (s)",     "Alarm",   CFG_F_DOUBLE, 1,    600,   offsetof(Config, alarm_aggro_seconds)},
    {"alarm_max_seconds",         "Auto-mute after (s)",     "Alarm",   CFG_F_DOUBLE, 10,   3600,  offsetof(Config, alarm_max_seconds)},

    {"sleep_volume_preparing_percent", "Preparing volume",   "Sleep",   CFG_F_DOUBLE, 0,    100,   offsetof(Config, sleep_volume_preparing_percent)},
    {"sleep_volume_sleeping_percent",  "Sleeping volume",    "Sleep",   CFG_F_DOUBLE, 0,    100,   offsetof(Config, sleep_volume_sleeping_percent)},
    {"sleep_volume_waking_percent",    "Waking volume",      "Sleep",   CFG_F_DOUBLE, 0,    100,   offsetof(Config, sleep_volume_waking_percent)},
    {"sleep_fade_seconds",             "Fade seconds",       "Sleep",   CFG_F_DOUBLE, 0,    10,    offsetof(Config, sleep_fade_seconds)},

    {"cfg_w",        "Setup window width",   "Window", CFG_F_INT, 300, 2000, offsetof(Config, cfg_w)},
    {"cfg_h",        "Setup window height",  "Window", CFG_F_INT, 300, 2000, offsetof(Config, cfg_h)},
    {"mini_w",       "Mini width",           "Window", CFG_F_INT, 100, 1000, offsetof(Config, mini_w)},
    {"mini_h_base",  "Mini height",          "Window", CFG_F_INT, 60,  600,  offsetof(Config, mini_h_base)},
    {"mini_h_until", "Mini height (until)",  "Window", CFG_F_INT, 60,  600,  offsetof(Config, mini_h_until)},
    {"mini_w_min",   "Mini min width",       "Window", CFG_F_INT, 80,  1000, offsetof(Config, mini_w_min)},
    {"mini_h_min",   "Mini min height",      "Window", CFG_F_INT, 40,  600,  offsetof(Config, mini_h_min)},
    {"resize_zone",  "Resize grab zone (px)","Window", CFG_F_INT, 4,   64,   offsetof(Config, resize_zone)},

    {"tick_config_ms",      "Config idle (ms)",   "Timing", CFG_F_INT, 16, 5000, offsetof(Config, tick_config_ms)},
    {"tick_config_anim_ms", "Config anim (ms)",   "Timing", CFG_F_INT, 8,  1000, offsetof(Config, tick_config_anim_ms)},
    {"tick_run_ms",         "Running (ms)",       "Timing", CFG_F_INT, 16, 5000, offsetof(Config, tick_run_ms)},
    {"tick_last10_ms",      "Last 10s (ms)",      "Timing", CFG_F_INT, 16, 1000, offsetof(Config, tick_last10_ms)},
    {"tick_paused_ms",      "Paused (ms)",        "Timing", CFG_F_INT, 16, 5000, offsetof(Config, tick_paused_ms)},
    {"tick_alert_ms",       "Alert (ms)",         "Timing", CFG_F_INT, 8,  1000, offsetof(Config, tick_alert_ms)},

    {"default_theme",      "Default theme (-1=random)", "Startup", CFG_F_INT,  -1, N_THEMES - 1, offsetof(Config, default_theme)},
    {"default_pausable",   "Pausable by default",       "Startup", CFG_F_BOOL, 0,  1,            offsetof(Config, default_pausable)},
    {"default_fullscreen", "Fullscreen by default",     "Startup", CFG_F_BOOL, 0,  1,            offsetof(Config, default_fullscreen)},
};
const int N_CFG_FIELDS = (int)(sizeof(CFG_FIELDS) / sizeof(CFG_FIELDS[0]));

double cfg_field_get(int idx) {
    if (idx < 0 || idx >= N_CFG_FIELDS) return 0.0;
    const CfgField *f = &CFG_FIELDS[idx];
    void *p = (char *)&g_config + f->offset;
    if (f->type == CFG_F_DOUBLE) return *(double *)p;
    return (double)(*(int *)p);
}

void cfg_field_set(int idx, double v) {
    if (idx < 0 || idx >= N_CFG_FIELDS) return;
    const CfgField *f = &CFG_FIELDS[idx];
    if (v < f->min) v = f->min;
    if (v > f->max) v = f->max;
    void *p = (char *)&g_config + f->offset;
    if (f->type == CFG_F_DOUBLE) *(double *)p = v;
    else                         *(int *)p = (int)(v + (v >= 0 ? 0.5 : -0.5));
}

static void config_path(char *out, int out_sz) {
    char dir[MAX_PATH_LEN - 16];
    get_config_dir(dir, sizeof(dir));
    snprintf(out, out_sz, "%s/config.conf", dir);
}

/* Writes every live g_config value (via CFG_FIELDS) back to the same
 * config.conf file config_load() reads below — no separate/parallel
 * config file. Called immediately by every Settings-overlay edit
 * (draw.c) so changes survive a restart right away. */
void config_save(void) {
    char path[MAX_PATH_LEN];
    config_path(path, sizeof(path));

    FILE *f = fopen(path, "w");
    if (!f) return;

    fprintf(f, "# TimerC configuration — edit with `timerc config`, or by hand.\n");
    fprintf(f, "# Format: key = value   Applies to the next timer you start.\n\n");
    const char *cur_cat = NULL;
    for (int i = 0; i < N_CFG_FIELDS; i++) {
        const CfgField *cf = &CFG_FIELDS[i];
        if (!cur_cat || strcmp(cur_cat, cf->category) != 0) {
            cur_cat = cf->category;
            fprintf(f, "\n# ── %s ──\n", cur_cat);
        }
        double v = cfg_field_get(i);
        if (cf->type == CFG_F_DOUBLE)
            fprintf(f, "%s = %.6g\n", cf->key, v);
        else
            fprintf(f, "%s = %d\n", cf->key, (int)v);
    }
    fclose(f);
}

/* Loads config.conf (if present) over the hardcoded defaults already
 * assigned earlier in config_load() — unknown keys are ignored,
 * malformed lines are skipped, values are clamped to each field's
 * range via cfg_field_set(). Missing file just means "use the
 * defaults", so a fresh checkout/install needs nothing extra. */
static void config_load_file(void) {
    char path[MAX_PATH_LEN];
    config_path(path, sizeof(path));

    FILE *f = fopen(path, "r");
    if (!f) return;

    char line[512];
    while (fgets(line, sizeof(line), f)) {
        char *p = line;
        while (isspace((unsigned char)*p)) p++;
        if (*p == '#' || *p == '\0' || *p == '\n') continue;

        char *eq = strchr(p, '=');
        if (!eq) continue;
        *eq = '\0';
        char *key = p;
        char *val = eq + 1;

        /* Trim trailing whitespace off the key. */
        char *ke = key + strlen(key);
        while (ke > key && isspace((unsigned char)ke[-1])) *--ke = '\0';

        while (isspace((unsigned char)*val)) val++;
        char *ve = val + strlen(val);
        while (ve > val && isspace((unsigned char)ve[-1])) *--ve = '\0';

        for (int i = 0; i < N_CFG_FIELDS; i++) {
            if (!strcmp(CFG_FIELDS[i].key, key)) {
                cfg_field_set(i, atof(val));
                break;
            }
        }
    }
    fclose(f);
}

/* Parses a duration string into seconds: chained "<number><unit>" pairs
 * (h=hours, m=minutes, s=seconds, ms=milliseconds), e.g. "1h30m",
 * "30m20s", "500ms", "2.4s". A bare number with no unit is seconds
 * ("10" == "10s"). Unrecognized/empty input yields 0. */
static double dur_sec(const char *s) {
    double total = 0.0;
    const char *p = s;
    while (*p) {
        while (isspace((unsigned char)*p)) p++;
        if (!*p) break;

        char *end;
        double val = strtod(p, &end);
        if (end == p) break;   /* no number here — stop parsing */
        p = end;

        char unit[4] = {0};
        int ui = 0;
        while (isalpha((unsigned char)*p) && ui < 3) unit[ui++] = *p++;

        if (!strcmp(unit, "ms"))      total += val / 1000.0;
        else if (!strcmp(unit, "h"))  total += val * 3600.0;
        else if (!strcmp(unit, "m"))  total += val * 60.0;
        else                          total += val;   /* "s" or no unit */
    }
    return total;
}
#define DUR(s)    dur_sec(s)
#define DUR_MS(s) ((int)(dur_sec(s) * 1000.0 + 0.5))

static int s_skip_file;

void config_load(App *app) {
    (void)app;
    memset(&g_config, 0, sizeof(g_config));

    /* ── Alarm volume ────────────────────────────────────────
     * System volume the alarm ramps from/to — see set_system_volume()
     * in audio.c. */

    /* Range: 0-100%  System volume set the instant the alarm starts. Why:
     * the alarm needs to be heard even if the system was left
     * muted/quiet, so it's the one sound in this program allowed to move
     * your volume UP on its own; every other sound only ever sets volume
     * to its own configured level while it's the one playing. */
    g_config.alarm_volume_min_percent = 40;

    /* Range: 0-100%  System volume the alarm ramps up to by the time its
     * fade-in (alarm_ramp_seconds) completes — synced with the PCM
     * loudness ramp, so the room actually gets louder as the tone does.
     * Clamped up to the min below if set lower than it. */
    g_config.alarm_volume_max_percent = 80;

    /* ── Alarm: sound/timing ────────────────────────────────
     * Everything else about the alarm — its own tempo and how long it
     * runs — independent of the volume settings above. */

    /* Range: 0-60s  How long the alarm takes to fade in from near-silent
     * to full volume once it starts. "0s" = full volume immediately.
     * Why: waking up (or being startled) to a sudden loud tone is
     * unpleasant; a ramp gives you a moment to react first. */
    g_config.alarm_ramp_seconds = DUR("10s");

    /* Range: 0.2-10s  Base length of one repeat of the alarm's musical
     * motif, before the tempo starts accelerating. Why: sets the
     * starting "urgency" — shorter means it opens faster/busier. */
    g_config.alarm_period = DUR("2.4s");

    /* Range: 0.05-10s  The fastest (shortest) the motif period is
     * allowed to ramp down to as the alarm gets more urgent. Clamped
     * down to alarm_period below if set higher than it (a faster floor
     * than the starting speed makes no sense). */
    g_config.alarm_period_min = DUR("1.2s");

    /* Range: 0-5  How fast the alarm's tempo ramps up, in period-speedup
     * per second of alerting. 0 = tempo never accelerates, stays at
     * alarm_period forever. Why: an alarm that gets more insistent the
     * longer it's ignored is more likely to actually be noticed. */
    g_config.alarm_accel = 0.015;

    /* Range: 1-600s  Seconds for the alarm's loudness/brightness to
     * build from its starting level up to full intensity (separate from
     * the tempo ramp above — this affects timbre/volume, not speed). */
    g_config.alarm_aggro_seconds = DUR("1m20s");

    /* Range: 10-3600s  Auto-mute the alarm after this long if nobody
     * dismisses it. Why: prevents it ringing forever (and annoying
     * everyone nearby) if you're away when the timer ends. */
    g_config.alarm_max_seconds = DUR("2m");

    /* ── Window sizing (pixels) ───────────────────────────────
     * All ints; ranges are sanity bounds against silly/broken values,
     * not meaningfully enforced beyond "don't be absurd". */

    g_config.cfg_w        = 500;  /* 300-2000  width of the config (setup) window */
    g_config.cfg_h        = 760;  /* 300-2000  height of the config (setup) window */
    g_config.mini_w       = 250;  /* 100-1000  default width of the floating countdown ("mini") window */
    g_config.mini_h_base  = 130;  /* 60-600    default mini-window height with no "until HH:MM" clock shown */
    g_config.mini_h_until = 172;  /* 60-600    default mini-window height with an "until HH:MM" clock shown */
    g_config.mini_w_min   = 130;  /* 80-1000   smallest width the mini window can be resized down to */
    g_config.mini_h_min   = 72;   /* 40-600    smallest height the mini window can be resized down to */

    /* Range: 4-64px  Size of the draggable corner hit-zone used to
     * resize the mini window. Why: too small and it's hard to grab with
     * a mouse; too large and it eats into normal click area. */
    g_config.resize_zone = 22;

    /* ── Tick pacing ───────────────────────────────────────────
     * Lower = smoother animation, higher = less CPU/redraw work while
     * idle. These are redraw intervals, not timer accuracy — the
     * countdown itself is computed from a monotonic clock regardless of
     * tick rate. */

    /* Range: 16ms-5s  Minimum time between redraws while the config
     * window is idle (not animating). */
    g_config.tick_config_ms = DUR_MS("500ms");

    /* Range: 8-1000ms  Redraw interval while the config window's
     * clock/quote animation or cyber background is running (16ms ~
     * 60fps). */
    g_config.tick_config_anim_ms = DUR_MS("16ms");

    /* Range: 16ms-5s  Redraw interval while a timer is running normally
     * (not in its last 10s). */
    g_config.tick_run_ms = DUR_MS("1s");

    /* Range: 16-1000ms  Redraw interval during the final 10 seconds of a
     * running timer, for a smoother-looking countdown right before it
     * ends. */
    g_config.tick_last10_ms = DUR_MS("100ms");

    /* Range: 16ms-5s  Redraw interval while a timer is paused. */
    g_config.tick_paused_ms = DUR_MS("1s");

    /* Range: 8-1000ms  Redraw interval while the alarm is alerting —
     * controls how smooth the beat-synced flash/strobe looks (16ms ~
     * 60fps). */
    g_config.tick_alert_ms = DUR_MS("16ms");

    /* ── Startup defaults ─────────────────────────────────────
     * What a freshly launched timer defaults to before you type anything
     * or pass CLI flags; CLI flags (-p, -f, etc.) override these for
     * that one run. */

    /* Range: -1..N_THEMES-1  Theme index used on launch; -1 = random.
     * See THEME_DEFS in draw_common.c for the list. */
    g_config.default_theme = -1;

    /* Whether new timers are pausable by default. Same effect as always
     * passing -p on the command line. */
    g_config.default_pausable = 0;

    /* Whether new timers start in fullscreen by default. Same effect as
     * always passing -f on the command line. */
    g_config.default_fullscreen = 0;

    /* ── Sleep mode: volume ────────────────────────────────────
     * System volume set the instant each procedurally-generated phase
     * starts, and periodically re-asserted while it plays — see
     * sleep_control_thread() in audio.c. */

    /* Range: 0-100%  System volume for the "preparing" phase (rain) —
     * plays once as you're falling asleep. Kept the lowest and steadiest
     * of the three: it should never spike. */
    g_config.sleep_volume_preparing_percent = 50;

    /* Range: 0-100%  System volume for the "sleeping" phase (slow
     * drone), looped for the bulk of the timer. */
    g_config.sleep_volume_sleeping_percent = 12;

    /* Range: 0-100%  System volume for the "waking" phase (rising
     * chime) — replaces the usual alarm siren in sleep mode, looping
     * until dismissed. */
    g_config.sleep_volume_waking_percent = 50;

    /* Range: roughly 0.3-5s is sane  Fade in/out window used at the
     * start of every sleep-mode audio segment (custom file or generated
     * fallback) and at every phase cut (e.g. preparing/sleeping trimmed
     * short to make room for waking) — a click-free envelope on the PCM
     * itself, separate from (and much shorter than) the system-volume
     * change between phases above. Why: near 0 would make cuts
     * click/pop; too large and phase transitions feel sluggish or
     * overlap awkwardly. */
    g_config.sleep_fade_seconds = DUR("1.5s");

    /* Override any of the above with values persisted from a previous
     * run's Settings overlay (Ctrl+G) — see config_save()/CFG_FIELDS
     * above. No file present just means "use the defaults set above". */
    if (!s_skip_file) config_load_file();

    /* ── Cross-field sanity fixups ────────────────────────────
     * Guards against a hand-edit above making two related values
     * contradict each other; a plain clamp, not user-facing validation
     * (there's no file input to validate here anymore). */

    config_normalize();
}

/* Keeps related values consistent after any edit (hand-edited file or TUI). */
void config_normalize(void) {
    /* alarm_volume_max_percent can never be below the min — the system
     * volume ramps UP to it, so a ceiling below the starting point is
     * nonsensical. */
    if (g_config.alarm_volume_max_percent < g_config.alarm_volume_min_percent)
        g_config.alarm_volume_max_percent = g_config.alarm_volume_min_percent;

    /* alarm_period_min can never exceed alarm_period — the tempo ramps
     * DOWN to it, so a floor above the starting point is nonsensical. */
    if (g_config.alarm_period_min > g_config.alarm_period)
        g_config.alarm_period_min = g_config.alarm_period;

    /* A resize-down minimum can never exceed the size it's a minimum of. */
    if (g_config.mini_w_min > g_config.mini_w) g_config.mini_w_min = g_config.mini_w;
    if (g_config.mini_h_min > g_config.mini_h_base) g_config.mini_h_min = g_config.mini_h_base;
}

/* Fills *out with the built-in defaults, leaving the live g_config untouched. */
void config_get_defaults(Config *out) {
    Config keep = g_config;
    s_skip_file = 1;
    config_load(NULL);
    s_skip_file = 0;
    *out = g_config;
    g_config = keep;
}

double cfg_field_get_from(const Config *c, int idx) {
    if (idx < 0 || idx >= N_CFG_FIELDS) return 0.0;
    const CfgField *f = &CFG_FIELDS[idx];
    const void *p = (const char *)c + f->offset;
    return f->type == CFG_F_DOUBLE ? *(const double *)p : (double)(*(const int *)p);
}
