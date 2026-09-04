/* config.c — user-tunable settings
 *
 * Loads ~/.config/timer_c/config.conf (or $XDG_CONFIG_HOME/timer_c/config.conf)
 * into the global `g_config`. The file uses simple `key = value` lines; `#`
 * starts a comment, blank lines are ignored. Every key has a documented
 * min/max range — out-of-range values are clamped and a warning is logged,
 * unknown keys are logged and skipped, the file itself is never rejected.
 *
 * If no config file exists yet, one is written out with every key set to
 * its default, each preceded by a comment block explaining what it does,
 * its min/max, and an example alternate value. That generated file is the
 * living documentation — CFG_FIELDS below is its single source of truth,
 * so the docs can never drift out of sync with what the program actually
 * reads.
 *
 * Volume guarantee: alarm.volume_percent is the ONLY setting anywhere in
 * this program that changes your system/master output volume, and it is
 * only ever applied immediately before the alarm sound starts playing
 * (see ensure_volume() in audio.c, called solely from play_alert_sound()).
 * Nothing else — cues, ticks, UI — ever touches system volume.
 */
#include "timer.h"
#include <stddef.h>

Config g_config;

typedef enum { CFG_DOUBLE, CFG_INT, CFG_BOOL } CfgType;

typedef struct {
    const char *key;
    CfgType     type;
    size_t      offset;
    double      min, max;
    double      def;
    const char *example;
    const char *doc;   /* may contain embedded '\n'; each line is prefixed with "# " */
} CfgField;

#define OFS(field) offsetof(Config, field)

static const CfgField CFG_FIELDS[] = {
    { "alarm.volume_percent", CFG_DOUBLE, OFS(alarm_volume_percent),
      0, 100, DEFAULT_ALARM_VOLUME_PERCENT, "70",
      "System output volume (%) applied right before the alarm sound\n"
      "starts. This is the ONLY setting in the whole program that ever\n"
      "changes your system/master volume; it is never touched at any\n"
      "other time (not on launch, not for cues, not while idle)." },

    { "alarm.ramp_seconds", CFG_DOUBLE, OFS(alarm_ramp_seconds),
      0, 60, DEFAULT_ALARM_RAMP_SECONDS, "5",
      "How many seconds the alarm takes to fade in from near-silent to\n"
      "full volume once it starts. 0 = full volume immediately." },

    { "alarm.period_seconds", CFG_DOUBLE, OFS(alarm_period),
      0.2, 10, DEFAULT_ALARM_PERIOD, "3.0",
      "Base length, in seconds, of one repeat of the alarm's musical\n"
      "motif before the tempo starts accelerating." },

    { "alarm.period_min_seconds", CFG_DOUBLE, OFS(alarm_period_min),
      0.05, 10, DEFAULT_ALARM_PERIOD_MIN, "1.0",
      "The fastest (shortest) the motif period is allowed to ramp down\n"
      "to as the alarm gets more urgent. Must be <= alarm.period_seconds\n"
      "(it is clamped down to it automatically if not)." },

    { "alarm.accel", CFG_DOUBLE, OFS(alarm_accel),
      0, 5, DEFAULT_ALARM_ACCEL, "0.1",
      "How fast the alarm's tempo ramps up, in period-speedup per\n"
      "second of alerting. 0 = tempo never accelerates." },

    { "alarm.aggro_seconds", CFG_DOUBLE, OFS(alarm_aggro_seconds),
      1, 600, DEFAULT_ALARM_AGGRO_SECONDS, "45",
      "Seconds for the alarm's loudness/brightness to build from its\n"
      "starting level up to full intensity." },

    { "alarm.max_seconds", CFG_DOUBLE, OFS(alarm_max_seconds),
      10, 3600, DEFAULT_ALARM_MAX_SECONDS, "300",
      "Auto-mute the alarm after this many seconds if nobody dismisses\n"
      "it. Prevents it from ringing forever when you're away." },

    { "window.config_width", CFG_INT, OFS(cfg_w),
      300, 2000, DEFAULT_CFG_W, "560",
      "Width in pixels of the config (setup) window." },

    { "window.config_height", CFG_INT, OFS(cfg_h),
      300, 2000, DEFAULT_CFG_H, "820",
      "Height in pixels of the config (setup) window." },

    { "window.mini_width", CFG_INT, OFS(mini_w),
      100, 1000, DEFAULT_MINI_W, "300",
      "Default width in pixels of the small floating countdown window." },

    { "window.mini_height", CFG_INT, OFS(mini_h_base),
      60, 600, DEFAULT_MINI_H_BASE, "150",
      "Default height in pixels of the small floating countdown window\n"
      "when it has no 'until' clock shown." },

    { "window.mini_height_with_until", CFG_INT, OFS(mini_h_until),
      60, 600, DEFAULT_MINI_H_UNTIL, "190",
      "Default height in pixels of the mini window when it also shows\n"
      "an 'until HH:MM' clock." },

    { "window.mini_min_width", CFG_INT, OFS(mini_w_min),
      80, 1000, DEFAULT_MINI_W_MIN, "150",
      "Smallest width the mini window can be resized down to. Clamped\n"
      "to window.mini_width if larger." },

    { "window.mini_min_height", CFG_INT, OFS(mini_h_min),
      40, 600, DEFAULT_MINI_H_MIN, "90",
      "Smallest height the mini window can be resized down to. Clamped\n"
      "to window.mini_height if larger." },

    { "window.resize_zone", CFG_INT, OFS(resize_zone),
      4, 64, DEFAULT_RESIZE_ZONE, "16",
      "Size in pixels of the draggable corner hit-zone used to resize\n"
      "the mini window." },

    { "tick.config_ms", CFG_INT, OFS(tick_config_ms),
      16, 5000, DEFAULT_TICK_CONFIG_MS, "250",
      "Minimum milliseconds between redraws while the config window is\n"
      "idle (not animating). Lower = smoother, higher = less CPU." },

    { "tick.config_anim_ms", CFG_INT, OFS(tick_config_anim_ms),
      8, 1000, DEFAULT_TICK_CONFIG_ANIM_MS, "33",
      "Milliseconds between redraws while the config window's clock/\n"
      "quote animation is running (~1000/fps). 16 ~= 60fps, 33 ~= 30fps." },

    { "tick.run_ms", CFG_INT, OFS(tick_run_ms),
      16, 5000, DEFAULT_TICK_TIMER_RUN_MS, "500",
      "Milliseconds between redraws while a timer is running normally." },

    { "tick.last10_ms", CFG_INT, OFS(tick_last10_ms),
      16, 1000, DEFAULT_TICK_TIMER_LAST10_MS, "50",
      "Milliseconds between redraws during the final 10 seconds of a\n"
      "running timer, for a smoother countdown." },

    { "tick.paused_ms", CFG_INT, OFS(tick_paused_ms),
      16, 5000, DEFAULT_TICK_TIMER_PAUSED_MS, "500",
      "Milliseconds between redraws while a timer is paused." },

    { "tick.alert_ms", CFG_INT, OFS(tick_alert_ms),
      8, 1000, DEFAULT_TICK_ALERT_MS, "33",
      "Milliseconds between redraws while the alarm is alerting\n"
      "(controls the beat-synced strobe smoothness, ~1000/fps)." },

    { "startup.theme", CFG_INT, OFS(default_theme),
      -1, N_THEMES - 1, DEFAULT_THEME_IDX, "0",
      "Theme used on launch: -1 picks one at random, otherwise an index\n"
      "0..N. Themes are generated fresh each run (see generate_themes()\n"
      "in draw.c), so a fixed index still means a different color each\n"
      "launch — it only pins which of the N freshly-generated palettes." },

    { "startup.pausable", CFG_BOOL, OFS(default_pausable),
      0, 1, DEFAULT_PAUSABLE, "1",
      "Whether new timers are pausable by default (0 or 1). Same effect\n"
      "as always passing -p on the command line." },

    { "startup.fullscreen", CFG_BOOL, OFS(default_fullscreen),
      0, 1, DEFAULT_FULLSCREEN, "1",
      "Whether new timers start in fullscreen by default (0 or 1). Same\n"
      "effect as always passing -f on the command line." },
};

#define N_CFG_FIELDS ((int)(sizeof(CFG_FIELDS) / sizeof(CFG_FIELDS[0])))

static double *dptr(const CfgField *f) { return (double *)((char *)&g_config + f->offset); }
static int    *iptr(const CfgField *f) { return (int    *)((char *)&g_config + f->offset); }

static void field_set(const CfgField *f, double v) {
    if (v < f->min) v = f->min;
    if (v > f->max) v = f->max;
    if (f->type == CFG_DOUBLE) *dptr(f) = v;
    else                       *iptr(f) = (int)(v + (v >= 0 ? 0.5 : -0.5));
}

static void set_defaults(void) {
    memset(&g_config, 0, sizeof(g_config));
    for (int i = 0; i < N_CFG_FIELDS; i++)
        field_set(&CFG_FIELDS[i], CFG_FIELDS[i].def);
}

/* Resolve ~/.config/timer_c/config.conf, honoring $XDG_CONFIG_HOME.
 * Creates the containing directory if it doesn't exist yet. */
static void resolve_config_path(char *out, int out_sz) {
    const char *xdg = getenv("XDG_CONFIG_HOME");
    char base[MAX_PATH_LEN];
    if (xdg && xdg[0]) {
        snprintf(base, sizeof(base), "%s", xdg);
    } else {
        const char *home = getenv("HOME");
        if (!home) home = "/tmp";
        snprintf(base, sizeof(base), "%s/.config", home);
    }
    char dir[MAX_PATH_LEN];
    snprintf(dir, sizeof(dir), "%s/timer_c", base);
    mkdir(base, 0755);
    mkdir(dir, 0755);
    snprintf(out, out_sz, "%s/config.conf", dir);
}

/* Write the fully-documented default config file — this doubles as the
 * example/reference doc for every option. */
static void write_example_file(const char *path) {
    FILE *fp = fopen(path, "w");
    if (!fp) return;

    fprintf(fp,
        "# Timer_C configuration file\n"
        "#\n"
        "# Format: key = value   (one per line)\n"
        "#   - Lines starting with '#' are comments.\n"
        "#   - Blank lines are ignored.\n"
        "#   - Unknown keys are logged and ignored.\n"
        "#   - Out-of-range values are clamped to the nearest valid value\n"
        "#     and a warning is logged.\n"
        "#   - This file is only read at startup; restart the app to pick\n"
        "#     up changes.\n"
        "#\n"
        "# Every key below is shown set to its default. Uncomment the\n"
        "# \"Example\" line (or just edit the value) to change it.\n"
        "\n");

    for (int i = 0; i < N_CFG_FIELDS; i++) {
        const CfgField *f = &CFG_FIELDS[i];
        fprintf(fp, "# %s\n", f->key);
        /* doc, line by line, each prefixed with "#   " */
        const char *p = f->doc;
        while (p && *p) {
            const char *nl = strchr(p, '\n');
            int len = nl ? (int)(nl - p) : (int)strlen(p);
            fprintf(fp, "#   %.*s\n", len, p);
            p = nl ? nl + 1 : NULL;
        }
        if (f->type == CFG_BOOL)
            fprintf(fp, "#   Range: 0 or 1   Default: %d\n", (int)f->def);
        else if (f->type == CFG_INT)
            fprintf(fp, "#   Range: %d to %d   Default: %d\n",
                    (int)f->min, (int)f->max, (int)f->def);
        else
            fprintf(fp, "#   Range: %g to %g   Default: %g\n", f->min, f->max, f->def);
        fprintf(fp, "#   Example: %s = %s\n", f->key, f->example);

        if (f->type == CFG_DOUBLE)
            fprintf(fp, "%s = %g\n\n", f->key, f->def);
        else
            fprintf(fp, "%s = %d\n\n", f->key, (int)f->def);
    }

    fclose(fp);
}

static char *trim(char *s) {
    while (*s == ' ' || *s == '\t') s++;
    char *end = s + strlen(s);
    while (end > s && (end[-1] == ' ' || end[-1] == '\t' ||
                        end[-1] == '\r' || end[-1] == '\n')) end--;
    *end = '\0';
    return s;
}

static void parse_file(App *app, const char *path) {
    FILE *fp = fopen(path, "r");
    if (!fp) return;

    char line[512];
    int lineno = 0;
    while (fgets(line, sizeof(line), fp)) {
        lineno++;
        char *s = line;
        char *hash = strchr(s, '#');
        if (hash) *hash = '\0';
        s = trim(s);
        if (!*s) continue;

        char *eq = strchr(s, '=');
        if (!eq) {
            app_log(app, "WARN", "config.conf:%d: missing '=', ignoring line", lineno);
            continue;
        }
        *eq = '\0';
        char *key = trim(s);
        char *val = trim(eq + 1);
        if (!*key || !*val) continue;

        const CfgField *f = NULL;
        for (int i = 0; i < N_CFG_FIELDS; i++)
            if (!strcasecmp(CFG_FIELDS[i].key, key)) { f = &CFG_FIELDS[i]; break; }
        if (!f) {
            app_log(app, "WARN", "config.conf:%d: unknown key '%s', ignoring", lineno, key);
            continue;
        }

        char *endp = NULL;
        double v = strtod(val, &endp);
        if (endp == val) {
            app_log(app, "WARN", "config.conf:%d: '%s' is not a number, ignoring", lineno, val);
            continue;
        }
        if (v < f->min || v > f->max) {
            double clamped = v < f->min ? f->min : f->max;
            app_log(app, "WARN", "config.conf:%d: %s = %g out of range [%g, %g], clamped to %g",
                    lineno, key, v, f->min, f->max, clamped);
        }
        field_set(f, v);
    }
    fclose(fp);
}

void config_load(App *app) {
    set_defaults();

    char path[MAX_PATH_LEN];
    resolve_config_path(path, sizeof(path));

    struct stat st;
    if (stat(path, &st) != 0) {
        write_example_file(path);
        app_log(app, "INFO", "wrote default config to %s", path);
    } else {
        parse_file(app, path);
    }

    /* alarm.period_min can never exceed alarm.period */
    if (g_config.alarm_period_min > g_config.alarm_period)
        g_config.alarm_period_min = g_config.alarm_period;

    /* min sizes can never exceed their corresponding default sizes */
    if (g_config.mini_w_min > g_config.mini_w) g_config.mini_w_min = g_config.mini_w;
    if (g_config.mini_h_min > g_config.mini_h_base) g_config.mini_h_min = g_config.mini_h_base;
}
