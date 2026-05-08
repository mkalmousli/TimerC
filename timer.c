/* =========================================================
 * timer_c — Native Linux X11 Countdown Timer
 *
 * Features:
 *   - Config window with styled text input
 *   - Mini floating window (borderless, top-right, draggable)
 *   - Fullscreen timer window
 *   - Right-click: toggle mini/fullscreen
 *   - Middle-click / ESC: stop/close
 *   - Click-to-pause (if pausable)
 *   - Animated progress bar with color transition
 *   - Alert mode: fullscreen color strobe + alarm sound
 *   - Shutdown mode: calls systemctl poweroff
 *   - 5 night themes (ember/moss/tide/ash/plum)
 *   - "Until" timer: u15:30, u1520
 *   - Input switches: d(shutdown) a(alert) r/f(fullscreen) p(pausable)
 *   - Glass-chime UI sounds via generated PCM
 *   - Session logging to SQLite + log file
 *   - Multi-monitor via XRandR
 *
 * Build: make  (requires libX11, libXft, libXrandr, libsqlite3, libm, libpthread)
 * Usage: ./timer [time] [task] [-m alert|shutdown] [-p] [-f] [--theme NAME]
 * ========================================================= */

#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>
#include <X11/keysym.h>
#include <X11/XKBlib.h>
#include <X11/Xft/Xft.h>
#include <X11/extensions/Xrandr.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>
#include <ctype.h>
#include <math.h>
#include <time.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/select.h>
#include <pthread.h>
#include <signal.h>
#include <sqlite3.h>

/* ─────────────────────────────────────────────────────── */
/* Constants                                               */
/* ─────────────────────────────────────────────────────── */

#define APP_NAME      "timer_c"
#define APP_MAX_INPUT  512
#define MAX_TASK       256
#define MAX_UNTIL      16
#define MAX_PATH_LEN   4096
/* Avoid conflict with linux/limits.h MAX_INPUT=255 */
#undef  MAX_INPUT
#define MAX_INPUT      APP_MAX_INPUT
#define TICK_MS       50
#define N_THEMES      5
#define N_QUOTES      16
#define SAMPLE_RATE   44100
#define TAU           (2.0 * M_PI)

/* Config window dimensions */
#define CFG_W  440
#define CFG_H  270

/* Mini timer default dimensions */
#define MINI_W       250
#define MINI_H_BASE  130
#define MINI_H_UNTIL 172

/* Progress bar thickness */
#define PROG_H_MINI  8
#define PROG_H_FULL  22

/* Resize corner hit area */
#define RESIZE_ZONE  22

/* ─────────────────────────────────────────────────────── */
/* Types                                                   */
/* ─────────────────────────────────────────────────────── */

typedef enum { STATE_CONFIG, STATE_RUNNING, STATE_PAUSED, STATE_ALERT, STATE_DONE } AppState;
typedef enum { MODE_ALERT, MODE_SHUTDOWN } TimerMode;

typedef struct {
    const char *name;
    unsigned long bg, surface, surface_soft;
    unsigned long text, text_muted;
    unsigned long accent, accent_soft;
    unsigned long border, danger;
    unsigned long until_chip_bg, until_chip_fg, until_chip_border;
    unsigned long until_clock_fg, until_clock_bg;
    unsigned long lcd_active, lcd_paused;
} Theme;

typedef struct {
    int size;
    int bold;
    XftFont *font;
} FontEntry;

/* Audio job sent to writer thread */
typedef struct {
    int16_t *samples;
    int      n_samples;
    int      fd_write;   /* write end of pipe */
    volatile int *active_flag; /* NULL for cues; ptr for alert loop */
    float    alert_ramp_seconds;
} AudioJob;

/* ─────────────────────────────────────────────────────── */
/* Themes                                                  */
/* ─────────────────────────────────────────────────────── */

static const Theme THEMES[N_THEMES] = {
    { "Night Ember",
      0x030403, 0x080907, 0x0c0d0a,
      0xc7bda8, 0x756f62,
      0xb48348, 0x2a2116,
      0x241d13, 0x8a3430,
      0x100c08, 0xb9915d, 0x342416,
      0xc49a63, 0x080604,
      0xb48348, 0x8a3430 },
    { "Night Moss",
      0x020504, 0x070b09, 0x0a100d,
      0xb7c4b8, 0x667267,
      0x6f9f73, 0x172218,
      0x1b2b1e, 0x854039,
      0x071008, 0x8db78f, 0x223725,
      0x9ec49d, 0x030805,
      0x6f9f73, 0x854039 },
    { "Night Tide",
      0x020507, 0x060b0e, 0x091014,
      0xb2c0c0, 0x637072,
      0x5d97a0, 0x111f23,
      0x182c32, 0x813a38,
      0x061015, 0x7fb0b8, 0x213d44,
      0x8bbac1, 0x03080b,
      0x5d97a0, 0x813a38 },
    { "Night Ash",
      0x030303, 0x080808, 0x0d0d0d,
      0xbdb8ad, 0x6e6a63,
      0x8f8a7f, 0x1b1a18,
      0x24231f, 0x783734,
      0x0b0a09, 0xaaa395, 0x302d28,
      0xbbb4a5, 0x060606,
      0x8f8a7f, 0x783734 },
    { "Night Plum",
      0x040306, 0x0a080d, 0x0f0c14,
      0xbdb4c5, 0x6d6574,
      0x8a78a1, 0x1c1724,
      0x292032, 0x803743,
      0x0d0913, 0xa390bc, 0x342840,
      0xad9ac4, 0x060409,
      0x8a78a1, 0x803743 },
};

static const char *QUOTES[N_QUOTES] = {
    "Stay here.", "One thing.", "Breathe slow.", "Begin now.",
    "Keep going.", "Small steps.", "Easy focus.", "Just this.",
    "Quiet mind.", "Soft effort.", "Time flows.", "Hold steady.",
    "No rush.", "Be present.", "Gentle focus.", "Finish calmly.",
};

/* ─────────────────────────────────────────────────────── */
/* App State Struct                                        */
/* ─────────────────────────────────────────────────────── */

#define MAX_FONT_CACHE 64

typedef struct {
    /* X11 */
    Display   *dpy;
    int        screen;
    Visual    *vis;
    Colormap   cmap;
    int        depth;
    GC         gc;

    /* Atoms */
    Atom wm_delete;
    Atom net_wm_state;
    Atom net_wm_state_above;
    Atom net_wm_state_fullscreen;
    Atom net_wm_state_skip_taskbar;
    Atom net_wm_window_type;
    Atom net_wm_window_type_dock;

    /* Windows */
    Window     cfg_win;
    XftDraw   *cfg_xft;
    Window     tmr_win;
    XftDraw   *tmr_xft;
    Window     alert_win;
    XftDraw   *alert_xft;

    /* Font cache */
    FontEntry  fonts[MAX_FONT_CACHE];
    int        n_fonts;

    /* App state */
    AppState   state;
    TimerMode  mode;
    int        mini;       /* 1 = mini window, 0 = fullscreen */
    int        pausable;
    int        fullscreen_requested;

    /* Timer tracking */
    int        total_seconds;
    int        remaining_seconds;
    double     remaining_exact;
    struct timespec start_ts;
    double     pause_total;
    struct timespec pause_start_ts;

    /* Input */
    char       input[MAX_INPUT];
    int        input_len;
    int        cursor_pos;
    int        cursor_blink;   /* 0/1 display state */
    long       cursor_blink_ts; /* ms since epoch */

    /* Task / until */
    char       task[MAX_TASK];
    char       until_str[MAX_UNTIL]; /* "HH:MM" or "" */

    /* Theme */
    int        theme_idx;

    /* Monitor geometry (of the monitor containing config/pointer) */
    int        mon_x, mon_y, mon_w, mon_h;

    /* Mini window geometry */
    int        tmr_x, tmr_y, tmr_w, tmr_h;

    /* Dragging */
    int        drag_px, drag_py;   /* pointer position at drag start */
    int        drag_wx, drag_wy;   /* window position at drag start */
    int        is_dragging;

    /* Corner resize */
    int        is_resizing;
    int        resize_px, resize_py;
    int        resize_wx, resize_wy;
    int        resize_ww, resize_wh;

    /* Alert animation */
    struct timespec alert_start_ts;
    int        alert_frame;

    /* Shutdown icon blink */
    int        blink_on;
    long       blink_ts;

    /* Audio */
    volatile int alert_audio_active;
    pid_t      audio_pids[8];
    int        n_audio_pids;
    pthread_mutex_t audio_lock;

    /* Cue state */
    int        cue_half_played;
    int        cue_last3[3]; /* flags for seconds 3,2,1 */
    int        prev_remaining;

    /* SQLite */
    sqlite3   *db;
    int64_t    session_id;
    int        session_finalized;

    /* Log */
    FILE      *logfp;
    char       log_path[MAX_PATH_LEN];
    char       db_path[MAX_PATH_LEN];

    /* Quit flag (set by signal) */
    volatile int quit;
} App;

/* Global pointer for signal handler */
static App *g_app = NULL;

/* ─────────────────────────────────────────────────────── */
/* Logging                                                 */
/* ─────────────────────────────────────────────────────── */

static void app_log(App *app, const char *level, const char *fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    time_t t = time(NULL);
    struct tm *tm = localtime(&t);
    char ts[32];
    strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", tm);

    if (app && app->logfp) {
        fprintf(app->logfp, "%s [%s] %s\n", ts, level, buf);
        fflush(app->logfp);
    }
    fprintf(stderr, "%s [%s] %s\n", ts, level, buf);
}

/* ─────────────────────────────────────────────────────── */
/* Time utilities                                          */
/* ─────────────────────────────────────────────────────── */

static double monotonic_now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

static long ms_now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long)(ts.tv_sec * 1000L + ts.tv_nsec / 1000000L);
}

static void format_time(int secs, char *out, size_t sz) {
    if (secs < 0) secs = 0;
    int h = secs / 3600;
    int m = (secs % 3600) / 60;
    int s = secs % 60;
    if (h > 0)
        snprintf(out, sz, "%02d:%02d:%02d", h, m, s);
    else
        snprintf(out, sz, "%02d:%02d", m, s);
}

/* ─────────────────────────────────────────────────────── */
/* Color utilities                                         */
/* ─────────────────────────────────────────────────────── */

static unsigned long rgb_pixel(App *app, unsigned long rgb) {
    XColor c;
    c.red   = ((rgb >> 16) & 0xff) * 257;
    c.green = ((rgb >> 8)  & 0xff) * 257;
    c.blue  = ( rgb        & 0xff) * 257;
    c.flags = DoRed | DoGreen | DoBlue;
    XAllocColor(app->dpy, app->cmap, &c);
    return c.pixel;
}

static void rgb_xft(App *app, unsigned long rgb, XftColor *out) {
    XRenderColor rc;
    rc.red   = ((rgb >> 16) & 0xff) * 257;
    rc.green = ((rgb >> 8)  & 0xff) * 257;
    rc.blue  = ( rgb        & 0xff) * 257;
    rc.alpha = 0xffff;
    XftColorAllocValue(app->dpy, app->vis, app->cmap, &rc, out);
}

/* Lerp between two 0xRRGGBB colors */
static unsigned long lerp_rgb(unsigned long a, unsigned long b, double t) {
    if (t <= 0.0) return a;
    if (t >= 1.0) return b;
    int ar = (a >> 16) & 0xff, ag = (a >> 8) & 0xff, ab2 = a & 0xff;
    int br = (b >> 16) & 0xff, bg = (b >> 8) & 0xff, bb = b & 0xff;
    int r = (int)(ar + (br - ar) * t);
    int g = (int)(ag + (bg - ag) * t);
    int b2 = (int)(ab2 + (bb - ab2) * t);
    return ((unsigned long)r << 16) | ((unsigned long)g << 8) | (unsigned long)b2;
}

static void fill_rect(App *app, Window win, unsigned long rgb,
                      int x, int y, int w, int h) {
    XSetForeground(app->dpy, app->gc, rgb_pixel(app, rgb));
    XFillRectangle(app->dpy, win, app->gc, x, y, w, h);
}

static void draw_rect_outline(App *app, Window win, unsigned long rgb,
                               int x, int y, int w, int h) {
    XSetForeground(app->dpy, app->gc, rgb_pixel(app, rgb));
    XDrawRectangle(app->dpy, win, app->gc, x, y, w - 1, h - 1);
}

/* ─────────────────────────────────────────────────────── */
/* Font management                                         */
/* ─────────────────────────────────────────────────────── */

static XftFont *get_font(App *app, int size, int bold) {
    for (int i = 0; i < app->n_fonts; i++) {
        if (app->fonts[i].size == size && app->fonts[i].bold == bold)
            return app->fonts[i].font;
    }
    char pattern[128];
    if (bold)
        snprintf(pattern, sizeof(pattern), "Sans:size=%d:weight=bold", size);
    else
        snprintf(pattern, sizeof(pattern), "Sans:size=%d", size);

    XftFont *f = XftFontOpenName(app->dpy, app->screen, pattern);
    if (!f) {
        /* Fallback to fixed */
        snprintf(pattern, sizeof(pattern), "fixed:size=%d", size);
        f = XftFontOpenName(app->dpy, app->screen, pattern);
    }
    if (!f) return NULL;

    if (app->n_fonts < MAX_FONT_CACHE) {
        app->fonts[app->n_fonts].size = size;
        app->fonts[app->n_fonts].bold = bold;
        app->fonts[app->n_fonts].font = f;
        app->n_fonts++;
    }
    return f;
}

static int text_width(App *app, XftFont *font, const char *s) {
    if (!font || !s || !s[0]) return 0;
    XGlyphInfo ext;
    XftTextExtentsUtf8(app->dpy, font, (const FcChar8 *)s, (int)strlen(s), &ext);
    return ext.xOff;
}

static int fit_font_size(App *app, const char *text,
                         int max_w, int max_h,
                         int min_sz, int max_sz, int bold) {
    int best = min_sz;
    for (int sz = max_sz; sz >= min_sz; sz -= (sz > 60 ? 4 : 2)) {
        XftFont *f = get_font(app, sz, bold);
        if (!f) continue;
        int tw = text_width(app, f, text);
        int th = f->ascent + f->descent;
        if (tw <= max_w && th <= max_h) { best = sz; break; }
    }
    return best;
}

static void xft_draw_string(App *app, XftDraw *draw, XftFont *font,
                            unsigned long rgb,
                            int x, int y, const char *s) {
    if (!draw || !font || !s || !s[0]) return;
    XftColor col;
    rgb_xft(app, rgb, &col);
    XftDrawStringUtf8(draw, &col, font, x, y, (const FcChar8 *)s, (int)strlen(s));
    XftColorFree(app->dpy, app->vis, app->cmap, &col);
}

static void xft_draw_string_centered(App *app, XftDraw *draw, XftFont *font,
                                     unsigned long rgb,
                                     int cx, int y, const char *s) {
    int w = text_width(app, font, s);
    xft_draw_string(app, draw, font, rgb, cx - w / 2, y, s);
}

/* ─────────────────────────────────────────────────────── */
/* Time string parser                                      */
/* ─────────────────────────────────────────────────────── */

typedef struct {
    int     seconds;      /* total duration, -1 if parse failed */
    char    task[MAX_TASK];
    char    until[MAX_UNTIL]; /* "HH:MM" or "" */
    int     sw_shutdown;
    int     sw_alert;
    int     sw_fullscreen;
    int     sw_pausable;
} ParseResult;

/* Extract leading switches: d a r f p */
static int extract_switches(const char *s, char *remainder, int rem_sz,
                             ParseResult *out) {
    out->sw_shutdown = out->sw_alert = out->sw_fullscreen = out->sw_pausable = 0;
    const char *p = s;
    while (*p && isspace((unsigned char)*p)) p++;

    /* Collect leading letters that are valid switches */
    const char *q = p;
    while (*q && strchr("daprfDAPRF", *q)) q++;
    /* After switches there must be a digit or 'u' */
    const char *r = q;
    while (*r && isspace((unsigned char)*r)) r++;
    if (!*r || (!isdigit((unsigned char)*r) && tolower((unsigned char)*r) != 'u')) {
        /* No valid switches found */
        snprintf(remainder, rem_sz, "%s", s);
        return 0;
    }

    for (const char *c = p; c < q; c++) {
        char lc = (char)tolower((unsigned char)*c);
        if (lc == 'd') out->sw_shutdown  = 1;
        if (lc == 'a') out->sw_alert     = 1;
        if (lc == 'r') out->sw_fullscreen = 1;
        if (lc == 'f') out->sw_fullscreen = 1;
        if (lc == 'p') out->sw_pausable  = 1;
    }
    snprintf(remainder, rem_sz, "%s", r);
    return 1;
}

/* Parse "u15:30", "u1520", "u15" → seconds until HH:MM */
static int parse_until(const char *s, ParseResult *out) {
    if (tolower((unsigned char)s[0]) != 'u') return 0;
    const char *p = s + 1;
    while (*p && isspace((unsigned char)*p)) p++;

    int hh = -1, mm = 0;
    /* Collect digits */
    char digits[8] = "";
    int di = 0;
    int colon = 0;
    char ac[4] = "";
    int aci = 0;

    while (*p && isdigit((unsigned char)*p) && di < 4)
        digits[di++] = *p++;
    digits[di] = '\0';

    if (*p == ':') {
        colon = 1;
        p++;
        while (*p && isdigit((unsigned char)*p) && aci < 2)
            ac[aci++] = *p++;
        ac[aci] = '\0';
    }

    if (di == 0) return 0;

    if (colon) {
        hh = atoi(digits);
        mm = aci > 0 ? atoi(ac) : 0;
    } else {
        int val = atoi(digits);
        if (di <= 2) { hh = val; mm = 0; }
        else if (di == 3) { hh = digits[0] - '0'; mm = atoi(digits + 1); }
        else { hh = atoi(digits) / 100; mm = atoi(digits) % 100; }
    }

    if (hh < 0 || hh > 23 || mm < 0 || mm > 59) return 0;

    /* Compute seconds until target */
    time_t now_t = time(NULL);
    struct tm *lt = localtime(&now_t);
    struct tm target = *lt;
    target.tm_hour = hh;
    target.tm_min  = mm;
    target.tm_sec  = 0;
    time_t target_t = mktime(&target);
    if (target_t <= now_t) target_t += 86400; /* next day */

    int secs = (int)(target_t - now_t);
    if (secs <= 0) return 0;

    out->seconds = secs;
    snprintf(out->until, sizeof(out->until), "%02d:%02d", hh, mm);

    /* Remainder is the task name */
    while (*p && isspace((unsigned char)*p)) p++;
    strncpy(out->task, p, sizeof(out->task) - 1);
    out->task[sizeof(out->task) - 1] = '\0';

    return 1;
}

/* Main parse entry: "25m focus", "1h30m work", "30", "u15:30 nap", etc. */
static void parse_time_input(const char *raw, ParseResult *out) {
    memset(out, 0, sizeof(*out));
    out->seconds = -1;

    if (!raw || !raw[0]) return;

    char remainder[MAX_INPUT];
    extract_switches(raw, remainder, sizeof(remainder), out);

    /* Skip leading whitespace */
    const char *s = remainder;
    while (*s && isspace((unsigned char)*s)) s++;

    /* Until format? */
    if (tolower((unsigned char)*s) == 'u') {
        if (parse_until(s, out)) return;
    }

    /* Colon-separated: MM:SS or HH:MM:SS */
    {
        const char *p = s;
        int parts[3] = {0,0,0};
        int nparts = 0;
        while (*p && nparts < 3) {
            if (!isdigit((unsigned char)*p)) break;
            int v = 0;
            while (*p && isdigit((unsigned char)*p)) v = v*10 + (*p++ - '0');
            parts[nparts++] = v;
            if (*p == ':') p++;
            else break;
        }
        if (nparts >= 2) {
            int total = 0;
            if (nparts == 2) {
                if (parts[1] > 59) goto skip_colon;
                total = parts[0] * 60 + parts[1];
            } else {
                if (parts[1] > 59 || parts[2] > 59) goto skip_colon;
                total = parts[0] * 3600 + parts[1] * 60 + parts[2];
            }
            if (total > 0) {
                out->seconds = total;
                while (*p && isspace((unsigned char)*p)) p++;
                strncpy(out->task, p, sizeof(out->task) - 1);
                out->task[sizeof(out->task) - 1] = '\0';
                return;
            }
        }
    }
skip_colon:;

    /* Standard: [value][unit] [task] */
    const char *p = s;
    if (!isdigit((unsigned char)*p)) return;

    int value = 0;
    while (*p && isdigit((unsigned char)*p)) value = value*10 + (*p++ - '0');

    char unit = 'm'; /* default to minutes */
    if (*p && strchr("hHmMsS", *p)) {
        unit = (char)tolower((unsigned char)*p);
        p++;
    }

    int secs = 0;
    if      (unit == 'h') secs = value * 3600;
    else if (unit == 'm') secs = value * 60;
    else                   secs = value;

    if (secs <= 0) return;

    out->seconds = secs;
    while (*p && isspace((unsigned char)*p)) p++;
    strncpy(out->task, p, sizeof(out->task) - 1);
    out->task[sizeof(out->task) - 1] = '\0';
}

/* Quick preview of switches while user types */
static void get_preview_switches(const char *input,
                                 int *sd, int *sa, int *sf, int *sp,
                                 char *until_out) {
    *sd = *sa = *sf = *sp = 0;
    if (until_out) until_out[0] = '\0';
    if (!input || !input[0]) return;
    ParseResult pr;
    parse_time_input(input, &pr);
    *sd = pr.sw_shutdown;
    *sa = pr.sw_alert;
    *sf = pr.sw_fullscreen;
    *sp = pr.sw_pausable;
    if (until_out) snprintf(until_out, MAX_UNTIL, "%s", pr.until);
}

/* ─────────────────────────────────────────────────────── */
/* Monitor detection (XRandR)                              */
/* ─────────────────────────────────────────────────────── */

static void detect_monitor(App *app, int ptr_x, int ptr_y) {
    app->mon_x = 0;
    app->mon_y = 0;
    app->mon_w = DisplayWidth(app->dpy, app->screen);
    app->mon_h = DisplayHeight(app->dpy, app->screen);

    int event_base, error_base;
    if (!XRRQueryExtension(app->dpy, &event_base, &error_base)) return;

    Window root = DefaultRootWindow(app->dpy);
    XRRScreenResources *res = XRRGetScreenResourcesCurrent(app->dpy, root);
    if (!res) return;

    for (int i = 0; i < res->ncrtc; i++) {
        XRRCrtcInfo *ci = XRRGetCrtcInfo(app->dpy, res, res->crtcs[i]);
        if (!ci) continue;
        if (ci->width == 0 || ci->height == 0) { XRRFreeCrtcInfo(ci); continue; }
        int x = ci->x, y = ci->y, w = (int)ci->width, h = (int)ci->height;
        if (ptr_x >= x && ptr_x < x + w && ptr_y >= y && ptr_y < y + h) {
            app->mon_x = x; app->mon_y = y;
            app->mon_w = w; app->mon_h = h;
            XRRFreeCrtcInfo(ci);
            break;
        }
        XRRFreeCrtcInfo(ci);
    }
    XRRFreeScreenResources(res);
}

/* ─────────────────────────────────────────────────────── */
/* Audio: PCM generation                                   */
/* ─────────────────────────────────────────────────────── */

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

typedef struct { float freq; float dur; float gap; float decay; float vol; } Note;

static int16_t *generate_cue(const char *kind, int *out_n) {
    static const Note launch[]   = {{523.25f,0.09f,0.018f,10.0f,0.125f},
                                    {783.99f,0.10f,0.025f,9.2f, 0.135f},
                                    {1046.5f,0.11f,0.035f,8.8f, 0.125f}};
    static const Note start_n[]  = {{440.0f, 0.08f,0.018f,10.0f,0.120f},
                                    {659.25f,0.09f,0.022f,9.4f, 0.130f},
                                    {987.77f,0.10f,0.03f, 8.8f, 0.125f}};
    static const Note pause_n[]  = {{783.99f,0.08f,0.018f,10.0f,0.110f},
                                    {493.88f,0.12f,0.03f, 9.0f, 0.115f}};
    static const Note resume_n[] = {{493.88f,0.08f,0.018f,10.0f,0.115f},
                                    {659.25f,0.09f,0.022f,9.4f, 0.125f},
                                    {880.0f, 0.10f,0.03f, 8.8f, 0.120f}};
    static const Note stop_n[]   = {{880.0f, 0.07f,0.016f,10.8f,0.110f},
                                    {659.25f,0.08f,0.018f,10.0f,0.112f},
                                    {440.0f, 0.12f,0.03f, 9.0f, 0.115f}};
    static const Note close_n[]  = {{659.25f,0.08f,0.018f,10.0f,0.105f},
                                    {392.0f, 0.12f,0.03f, 9.2f, 0.110f}};
    static const Note half_n[]   = {{587.33f,0.08f,0.018f,10.0f,0.105f},
                                    {880.0f, 0.09f,0.025f,9.2f, 0.110f}};
    static const Note last_n[]   = {{1174.66f,0.065f,0.014f,11.2f,0.120f},
                                    {1567.98f,0.075f,0.018f,10.4f,0.120f}};
    static const Note dismiss_n[]= {{987.77f,0.07f,0.016f,10.5f,0.105f},
                                    {659.25f,0.09f,0.022f,9.8f, 0.110f},
                                    {493.88f,0.12f,0.03f, 9.0f, 0.108f}};
    static const Note alert_open[]={{880.0f,0.07f,0.014f,11.0f,0.140f},
                                    {1174.66f,0.08f,0.018f,10.4f,0.145f},
                                    {1567.98f,0.09f,0.025f,9.8f,0.135f}};

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

    /* Compute total samples needed */
    int total = 0;
    for (int i = 0; i < n_notes; i++) {
        total += (int)(SAMPLE_RATE * notes[i].dur) + (int)(SAMPLE_RATE * notes[i].gap);
    }
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

/* ─────────────────────────────────────────────────────── */
/* Audio: playback                                         */
/* ─────────────────────────────────────────────────────── */

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
        /* Check binary exists */
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
            /* Child */
            close(pipefd[1]);
            dup2(pipefd[0], STDIN_FILENO);
            close(pipefd[0]);
            int null_fd = open("/dev/null", O_WRONLY);
            if (null_fd >= 0) { dup2(null_fd, STDOUT_FILENO); dup2(null_fd, STDERR_FILENO); close(null_fd); }
            execvp(PLAYER_CMDS[ci][0], (char *const *)PLAYER_CMDS[ci]);
            _exit(1);
        }
        /* Parent */
        close(pipefd[0]);
        *stdin_fd = pipefd[1];
        return pid;
    }
    *stdin_fd = -1;
    return -1;
}

static void track_audio_pid(App *app, pid_t pid) {
    pthread_mutex_lock(&app->audio_lock);
    /* Clean up finished */
    int j = 0;
    for (int i = 0; i < app->n_audio_pids; i++) {
        if (waitpid(app->audio_pids[i], NULL, WNOHANG) == 0) {
            app->audio_pids[j++] = app->audio_pids[i];
        }
    }
    app->n_audio_pids = j;
    if (app->n_audio_pids < 8) {
        app->audio_pids[app->n_audio_pids++] = pid;
    }
    pthread_mutex_unlock(&app->audio_lock);
}

static void kill_all_audio(App *app) {
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
        /* One-shot cue */
        ssize_t total = job->n_samples * 2;
        const uint8_t *ptr = (const uint8_t *)job->samples;
        while (total > 0) {
            ssize_t w = write(job->fd_write, ptr, total);
            if (w <= 0) break;
            ptr += w; total -= w;
        }
    } else {
        /* Alert loop: stream until active_flag becomes 0 */
        int phase = 0;
        const int CHUNK = SAMPLE_RATE / 20; /* 50ms chunks */
        int16_t *chunk = (int16_t *)malloc(CHUNK * 2);
        if (!chunk) goto done;

        while (*job->active_flag) {
            float elapsed = (float)phase / SAMPLE_RATE;
            float ramp = fminf(1.0f, elapsed / job->alert_ramp_seconds);

            for (int i = 0; i < CHUNK; i++) {
                float t = (float)(phase + i) / SAMPLE_RATE;
                float chirp = fmodf(t, 0.20f);
                float gate = chirp < 0.145f ? 1.0f : 0.0f;
                float attack = fminf(1.0f, chirp / 0.004f);
                float release = fminf(1.0f, fmaxf(0.0f, (0.145f - chirp) / 0.018f));
                float env = gate * attack * release;
                float sweep = chirp < 0.145f ? chirp / 0.145f : 1.0f;
                float sfreq = 820.0f + 760.0f * sweep;
                float warble = ((int)(t * 12) % 2) ? 1.0f : 0.72f;
                float f = sfreq * warble;
                float sq = sinf((float)TAU * f * t) >= 0 ? 1.0f : -1.0f;
                float pulse = ((int)(t * 16) % 2) ? 1.0f : -1.0f;
                float glass = sinf((float)TAU * 2100.0f * t) * expf(-chirp * 22.0f);
                float beep = 0.72f * sq + 0.22f * pulse + 0.20f * glass;
                float s = tanhf(beep * env * 1.55f) * ramp;
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

static void run_cmd(const char *cmd) {
    int r = system(cmd); (void)r;
}

static void ensure_volume(void) {
    /* Unmute and set minimum volume via pactl if available */
    if (access("/usr/bin/pactl", X_OK) == 0) {
        run_cmd("pactl set-sink-mute @DEFAULT_SINK@ 0 2>/dev/null");
        /* Only raise if below 30% — avoid clobbering user volume */
        FILE *f = popen("pactl get-sink-volume @DEFAULT_SINK@ 2>/dev/null", "r");
        if (f) {
            char line[256] = "";
            if (fgets(line, sizeof(line), f)) {
                char *pct = strstr(line, "%");
                if (pct) {
                    const char *p = pct - 1;
                    while (p > line && isdigit((unsigned char)*p)) p--;
                    int vol = atoi(p + 1);
                    if (vol < 30)
                        run_cmd("pactl set-sink-volume @DEFAULT_SINK@ 30% 2>/dev/null");
                }
            }
            pclose(f);
        }
    }
}

static void play_cue(App *app, const char *kind) {
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
    job->samples   = samples;
    job->n_samples = n;
    job->fd_write  = fd;
    job->active_flag = NULL;

    pthread_t tid;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_create(&tid, &attr, audio_writer_thread, job);
    pthread_attr_destroy(&attr);
}

static void play_alert_sound(App *app) {
    ensure_volume();
    kill_all_audio(app);
    app->alert_audio_active = 1;

    int fd;
    pid_t pid = spawn_player(&fd);
    if (pid < 0) return;

    track_audio_pid(app, pid);

    AudioJob *job = (AudioJob *)calloc(1, sizeof(AudioJob));
    if (!job) { close(fd); return; }
    job->samples       = NULL;
    job->n_samples     = 0;
    job->fd_write      = fd;
    job->active_flag   = &app->alert_audio_active;
    job->alert_ramp_seconds = 15.0f;

    pthread_t tid;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_create(&tid, &attr, audio_writer_thread, job);
    pthread_attr_destroy(&attr);
}

/* ─────────────────────────────────────────────────────── */
/* Database (SQLite)                                       */
/* ─────────────────────────────────────────────────────── */

static void db_init(App *app) {
    if (sqlite3_open(app->db_path, &app->db) != SQLITE_OK) {
        app_log(app, "WARN", "SQLite open failed: %s", sqlite3_errmsg(app->db));
        app->db = NULL;
        return;
    }
    const char *sql =
        "CREATE TABLE IF NOT EXISTS sessions ("
        "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  started_at TEXT NOT NULL,"
        "  ended_at TEXT,"
        "  duration_seconds INTEGER NOT NULL,"
        "  elapsed_seconds INTEGER,"
        "  task_name TEXT,"
        "  mode TEXT NOT NULL,"
        "  pausable INTEGER NOT NULL,"
        "  fullscreen INTEGER NOT NULL,"
        "  status TEXT NOT NULL"
        ");";
    char *err = NULL;
    if (sqlite3_exec(app->db, sql, NULL, NULL, &err) != SQLITE_OK) {
        app_log(app, "WARN", "DB create table failed: %s", err);
        sqlite3_free(err);
    }
}

static void db_start_session(App *app) {
    if (!app->db || app->session_id > 0) return;
    time_t t = time(NULL);
    struct tm *tm = localtime(&t);
    char ts[32];
    strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%S", tm);

    const char *sql =
        "INSERT INTO sessions "
        "(started_at, duration_seconds, task_name, mode, pausable, fullscreen, status)"
        " VALUES (?,?,?,?,?,?,?)";
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(app->db, sql, -1, &stmt, NULL) != SQLITE_OK) return;
    sqlite3_bind_text(stmt, 1, ts, -1, SQLITE_STATIC);
    sqlite3_bind_int(stmt, 2, app->total_seconds);
    sqlite3_bind_text(stmt, 3, app->task, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 4, app->mode == MODE_SHUTDOWN ? "shutdown" : "alert",
                      -1, SQLITE_STATIC);
    sqlite3_bind_int(stmt, 5, app->pausable);
    sqlite3_bind_int(stmt, 6, !app->mini);
    sqlite3_bind_text(stmt, 7, "running", -1, SQLITE_STATIC);
    sqlite3_step(stmt);
    app->session_id = sqlite3_last_insert_rowid(app->db);
    sqlite3_finalize(stmt);
}

static void db_finish_session(App *app, const char *status) {
    if (!app->db || app->session_id <= 0 || app->session_finalized) return;
    time_t t = time(NULL);
    struct tm *tm = localtime(&t);
    char ts[32];
    strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%S", tm);
    int elapsed = app->total_seconds - app->remaining_seconds;
    if (elapsed < 0) elapsed = 0;

    const char *sql =
        "UPDATE sessions SET ended_at=?, status=?, elapsed_seconds=? WHERE id=?";
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(app->db, sql, -1, &stmt, NULL) != SQLITE_OK) return;
    sqlite3_bind_text(stmt, 1, ts, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, status, -1, SQLITE_STATIC);
    sqlite3_bind_int(stmt, 3, elapsed);
    sqlite3_bind_int64(stmt, 4, app->session_id);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    app->session_finalized = 1;
}

/* ─────────────────────────────────────────────────────── */
/* EWMH / WM hints                                         */
/* ─────────────────────────────────────────────────────── */

static void set_wm_delete(App *app, Window win) {
    XSetWMProtocols(app->dpy, win, &app->wm_delete, 1);
}

static void set_always_on_top(App *app, Window win) {
    XChangeProperty(app->dpy, win,
                    app->net_wm_state, XA_ATOM, 32, PropModeReplace,
                    (unsigned char *)&app->net_wm_state_above, 1);
}

/* ─────────────────────────────────────────────────────── */
/* Timer color calculation                                 */
/* ─────────────────────────────────────────────────────── */

static const Theme *cur_theme(App *app) { return &THEMES[app->theme_idx]; }

static unsigned long timer_display_color(App *app) {
    float frac = app->total_seconds > 0
                 ? (float)app->remaining_exact / app->total_seconds
                 : 1.0f;
    if (frac < 0.0f) frac = 0.0f;
    if (frac > 1.0f) frac = 1.0f;

    const Theme *th = cur_theme(app);
    unsigned long color;
    if (frac >= 0.8f) {
        color = th->lcd_active;
    } else {
        double t = (0.8 - frac) / 0.8;
        color = lerp_rgb(th->lcd_active, th->lcd_paused, t);
    }

    /* Gentle pulse */
    double mono = monotonic_now();
    double pulse = 0.94 + 0.06 * (0.5 + 0.5 * sin(mono * TAU / 5.5));

    int r = (int)(((color >> 16) & 0xff) * pulse);
    int g = (int)(((color >> 8)  & 0xff) * pulse);
    int b2 = (int)((color        & 0xff) * pulse);
    if (r > 255) r = 255;
    if (r < 0) r = 0;
    if (g > 255) g = 255;
    if (g < 0) g = 0;
    if (b2 > 255) b2 = 255;
    if (b2 < 0) b2 = 0;
    return ((unsigned long)r << 16) | ((unsigned long)g << 8) | (unsigned long)b2;
}

/* ─────────────────────────────────────────────────────── */
/* Alert strobe color                                      */
/* ─────────────────────────────────────────────────────── */

static unsigned long alert_strobe_color(double elapsed, int frame) {
    static const unsigned long pal[] =
        { 0x000000, 0xffffff, 0xff0000, 0xffff00, 0xff00ff, 0x00ffff, 0x0000ff };
    double chirp = fmod(elapsed, 0.20);
    int warble = (int)(elapsed * 12) % 2;
    int pulse  = (int)(elapsed * 16) % 2;
    int burst  = chirp < 0.145;
    if (!burst) return (frame % 2 == 0) ? 0x000000UL : 0x220000UL;
    if (chirp < 0.020) return 0xffffffUL;
    if (chirp > 0.115) return pulse ? 0xff0000UL : 0xffff00UL;
    int aframe = (int)(chirp * 1000.0 / 16.0);
    int idx = (frame*5 + (int)(elapsed*37) + warble*3 + pulse*2 + aframe) % 7;
    return pal[idx];
}

/* ─────────────────────────────────────────────────────── */
/* XftDraw creation helper                                 */
/* ─────────────────────────────────────────────────────── */

static XftDraw *make_xft_draw(App *app, Window win) {
    return XftDrawCreate(app->dpy, win, app->vis, app->cmap);
}

/* ─────────────────────────────────────────────────────── */
/* Config window drawing                                   */
/* ─────────────────────────────────────────────────────── */

static void draw_config(App *app) {
    if (!app->cfg_win) return;
    const Theme *th = cur_theme(app);
    int W = CFG_W, H = CFG_H;

    /* Background */
    fill_rect(app, app->cfg_win, th->bg, 0, 0, W, H);

    XftDraw *draw = app->cfg_xft;
    if (!draw) return;

    /* ── Theme badge (top-right) ─────────────────────── */
    {
        XftFont *f = get_font(app, 8, 0);
        if (f) {
            char badge[64];
            snprintf(badge, sizeof(badge), "theme: %s", th->name);
            int bw = text_width(app, f, badge);
            xft_draw_string(app, draw, f, th->text_muted, W - bw - 12, 14, badge);
        }
    }

    /* ── Quote ───────────────────────────────────────── */
    {
        XftFont *f = get_font(app, 20, 1);
        if (f) {
            static const char *quote = NULL;
            if (!quote) quote = QUOTES[rand() % N_QUOTES];
            xft_draw_string_centered(app, draw, f, th->accent, W / 2, 58, quote);
        }
    }

    /* ── Input field ─────────────────────────────────── */
    {
        int fx = 22, fy = 72, fw = W - 44, fh = 46;
        /* Background */
        fill_rect(app, app->cfg_win, th->surface_soft, fx, fy, fw, fh);
        /* Border */
        draw_rect_outline(app, app->cfg_win, th->accent, fx, fy, fw, fh);

        XftFont *f = get_font(app, 17, 0);
        if (f) {
            int ty = fy + (fh + f->ascent) / 2;
            int text_x = fx + 12;
            int avail_w = fw - 24;

            /* Render input text — scroll to keep cursor visible */
            const char *text = app->input;
            int tlen = app->input_len;

            /* Find display start so cursor is always visible */
            int start = 0;
            if (app->cursor_pos > 0) {
                char tmp[MAX_INPUT];
                /* Walk back from cursor until text fits */
                for (start = 0; start <= app->cursor_pos; start++) {
                    strncpy(tmp, text + start, app->cursor_pos - start);
                    tmp[app->cursor_pos - start] = '\0';
                    if (text_width(app, f, tmp) <= avail_w - 6) break;
                }
            }

            char disp[MAX_INPUT];
            strncpy(disp, text + start, tlen - start);
            disp[tlen - start] = '\0';
            xft_draw_string(app, draw, f, th->text, text_x, ty, disp);

            /* Cursor */
            int blink_on = app->cursor_blink ||
                           (app->state == STATE_CONFIG && /* always show on fresh draw */
                            ms_now() - app->cursor_blink_ts < 100);
            if (blink_on) {
                char before[MAX_INPUT];
                int cp_disp = app->cursor_pos - start;
                if (cp_disp < 0) cp_disp = 0;
                strncpy(before, text + start, cp_disp);
                before[cp_disp] = '\0';
                int cx = text_x + text_width(app, f, before);
                XSetForeground(app->dpy, app->gc, rgb_pixel(app, th->text));
                XFillRectangle(app->dpy, app->cfg_win, app->gc,
                               cx, fy + 6, 2, fh - 12);
            }

            /* Placeholder */
            if (tlen == 0) {
                XftFont *pf = get_font(app, 13, 0);
                if (pf)
                    xft_draw_string(app, draw, pf, th->text_muted,
                                    text_x, ty, "25m focus  ·  1h coding  ·  u16:30");
            }
        }
    }

    /* ── Switch chips ────────────────────────────────── */
    {
        int sw_d=0, sw_a=0, sw_f=0, sw_p=0;
        char until_preview[MAX_UNTIL] = "";
        get_preview_switches(app->input, &sw_d, &sw_a, &sw_f, &sw_p, until_preview);

        struct { int active; const char *label; unsigned long bg; unsigned long fg; unsigned long border; }
        chips[] = {
            { sw_d, "d shutdown", th->danger,       0xf0e8d8UL, th->danger },
            { sw_a, "a alert",    th->accent_soft,  th->accent,  th->border },
            { sw_f, "f fullscreen",th->accent_soft, th->accent,  th->border },
            { sw_p, "p pausable", th->accent_soft,  th->accent,  th->border },
            { until_preview[0] != '\0', NULL, th->until_chip_bg, th->until_chip_fg, th->until_chip_border },
        };
        int n_active = 0;
        int chip_widths[5] = {0};
        XftFont *cf = get_font(app, 9, 1);
        if (cf) {
            for (int i = 0; i < 5; i++) {
                if (!chips[i].active) continue;
                const char *lbl = (i == 4) ? until_preview : chips[i].label;
                char tmp[32];
                if (i == 4) snprintf(tmp, sizeof(tmp), "until %s", until_preview);
                else snprintf(tmp, sizeof(tmp), "%s", lbl);
                chip_widths[i] = text_width(app, cf, tmp) + 20;
                n_active++;
            }
            if (n_active > 0) {
                int total_w = 0;
                for (int i = 0; i < 5; i++)
                    if (chips[i].active) total_w += chip_widths[i] + 6;
                total_w -= 6;
                int cx = (W - total_w) / 2;
                int cy = 132;
                for (int i = 0; i < 5; i++) {
                    if (!chips[i].active) continue;
                    const char *lbl;
                    char tmp[32];
                    if (i == 4) { snprintf(tmp, sizeof(tmp), "until %s", until_preview); lbl = tmp; }
                    else lbl = chips[i].label;
                    int cw = chip_widths[i];
                    int ch2 = 20;
                    fill_rect(app, app->cfg_win, chips[i].bg, cx, cy, cw, ch2);
                    draw_rect_outline(app, app->cfg_win, chips[i].border, cx, cy, cw, ch2);
                    int tw2 = text_width(app, cf, lbl);
                    xft_draw_string(app, draw, cf, chips[i].fg,
                                    cx + (cw - tw2)/2, cy + cf->ascent + 2, lbl);
                    cx += cw + 6;
                }
            }
        }
    }

    /* ── Mode / hint line ────────────────────────────── */
    {
        XftFont *f = get_font(app, 9, 0);
        if (f) {
            const char *hint = "Enter: start   ESC: quit   Ctrl+T: cycle theme   Right-click: fullscreen";
            xft_draw_string_centered(app, draw, f, th->text_muted, W/2, H - 14, hint);
        }
    }

    /* ── Top accent bar ──────────────────────────────── */
    fill_rect(app, app->cfg_win, th->accent, 0, 0, W, 3);

    XFlush(app->dpy);
}

/* ─────────────────────────────────────────────────────── */
/* Config window creation                                  */
/* ─────────────────────────────────────────────────────── */

static void create_config_window(App *app) {
    const Theme *th = cur_theme(app);

    /* Get pointer position to find the right monitor */
    Window root_ret, child_ret;
    int root_x, root_y, win_x, win_y;
    unsigned int mask;
    Window root = DefaultRootWindow(app->dpy);
    XQueryPointer(app->dpy, root, &root_ret, &child_ret,
                  &root_x, &root_y, &win_x, &win_y, &mask);
    detect_monitor(app, root_x, root_y);

    int x = app->mon_x + (app->mon_w - CFG_W) / 2;
    int y = app->mon_y + (app->mon_h - CFG_H) / 2;

    XSetWindowAttributes swa;
    swa.background_pixel  = rgb_pixel(app, th->bg);
    swa.border_pixel      = rgb_pixel(app, th->accent);
    swa.colormap          = app->cmap;
    swa.event_mask        = ExposureMask | KeyPressMask | ButtonPressMask
                          | StructureNotifyMask;

    app->cfg_win = XCreateWindow(
        app->dpy, root,
        x, y, CFG_W, CFG_H, 0,
        app->depth, InputOutput, app->vis,
        CWBackPixel | CWBorderPixel | CWColormap | CWEventMask, &swa);

    /* Window title */
    XStoreName(app->dpy, app->cfg_win, "Timer");

    /* WM delete */
    set_wm_delete(app, app->cfg_win);

    /* Stay on top */
    set_always_on_top(app, app->cfg_win);

    /* Size hints (fixed) */
    XSizeHints hints;
    hints.flags = PMinSize | PMaxSize;
    hints.min_width = hints.max_width = CFG_W;
    hints.min_height= hints.max_height= CFG_H;
    XSetWMNormalHints(app->dpy, app->cfg_win, &hints);

    XMapWindow(app->dpy, app->cfg_win);
    XFlush(app->dpy);

    app->cfg_xft = make_xft_draw(app, app->cfg_win);

    app->cursor_blink = 1;
    app->cursor_blink_ts = ms_now();
}

/* ─────────────────────────────────────────────────────── */
/* Timer window drawing                                    */
/* ─────────────────────────────────────────────────────── */

static void draw_timer_mini(App *app) {
    if (!app->tmr_win) return;
    const Theme *th = cur_theme(app);
    int W = app->tmr_w, H = app->tmr_h;
    XftDraw *draw = app->tmr_xft;
    if (!draw) return;

    /* Background */
    fill_rect(app, app->tmr_win, th->bg, 0, 0, W, H);

    /* 1px accent border */
    draw_rect_outline(app, app->tmr_win, th->accent, 0, 0, W, H);

    int y = 4;

    /* Shutdown icon */
    if (app->mode == MODE_SHUTDOWN) {
        XftFont *f = get_font(app, 11, 0);
        if (f) {
            unsigned long ic = app->blink_on ? th->danger : th->border;
            xft_draw_string_centered(app, draw, f, ic, W/2, y + f->ascent, "⏻");
            y += f->ascent + f->descent + 2;
        }
    }

    /* Task name */
    {
        XftFont *f = get_font(app, 8, 0);
        if (f && W > 80) {
            char task[MAX_TASK];
            snprintf(task, sizeof(task), "%s", app->task);
            if ((int)strlen(task) > 22) { task[20] = '.'; task[21] = '.'; task[22] = '\0'; }
            xft_draw_string_centered(app, draw, f, th->text, W/2, y + f->ascent, task);
            y += f->ascent + f->descent + 2;
        }
    }

    /* Until time */
    int has_until = app->until_str[0] != '\0';
    if (has_until) {
        XftFont *f = get_font(app, 17, 1);
        if (f) {
            int bw = text_width(app, f, app->until_str) + 24;
            int bh = f->ascent + f->descent + 8;
            int bx = (W - bw) / 2;
            fill_rect(app, app->tmr_win, th->until_clock_bg, bx, y, bw, bh);
            draw_rect_outline(app, app->tmr_win, th->until_chip_border, bx, y, bw, bh);
            xft_draw_string_centered(app, draw, f, th->until_clock_fg,
                                     W/2, y + f->ascent + 4, app->until_str);
            y += bh + 4;
        }
    }

    /* Countdown */
    {
        int prog_h = (H >= 90) ? PROG_H_MINI : 0;
        int avail_h = H - y - prog_h - 8;
        int avail_w = W - 8;

        char time_str[16];
        format_time(app->remaining_seconds, time_str, sizeof(time_str));

        /* Blink when paused */
        int show = 1;
        if (app->state == STATE_PAUSED) {
            show = (int)(monotonic_now() * 2) % 2 == 0;
        }

        if (avail_w > 20 && avail_h > 10) {
            int sz = fit_font_size(app, time_str, avail_w, avail_h, 10, 96, 1);
            XftFont *f = get_font(app, sz, 1);
            if (f && show) {
                unsigned long col = timer_display_color(app);
                int tw3 = text_width(app, f, time_str);
                int tx = (W - tw3) / 2;
                int center_y = y + avail_h / 2;
                int ty = center_y - (f->ascent + f->descent) / 2 + f->ascent;
                xft_draw_string(app, draw, f, col, tx, ty, time_str);
            }
        }

        /* Pause label */
        if (app->state == STATE_PAUSED && H > 80) {
            XftFont *f = get_font(app, 7, 0);
            if (f) {
                xft_draw_string_centered(app, draw, f, th->danger,
                                         W/2, H - prog_h - 14, "PAUSED");
            }
        }

        /* Progress bar */
        if (prog_h > 0 && app->total_seconds > 0) {
            int px = 8, py = H - prog_h - 6, pw = W - 16;
            double frac = 1.0 - (double)app->remaining_exact / app->total_seconds;
            if (frac < 0) frac = 0;
            if (frac > 1) frac = 1;
            int bar_w = (int)(pw * frac);
            fill_rect(app, app->tmr_win, th->surface, px, py, pw, prog_h);
            if (bar_w > 0) {
                unsigned long bcol = timer_display_color(app);
                fill_rect(app, app->tmr_win, bcol, px, py, bar_w, prog_h);
            }
        }
    }

    /* Resize corner ◢ */
    {
        XftFont *f = get_font(app, 11, 0);
        if (f) {
            int tw4 = text_width(app, f, "◢");
            xft_draw_string(app, draw, f, th->accent,
                            W - tw4 - 3, H - f->descent - 2, "◢");
        }
    }

    XFlush(app->dpy);
}

static void draw_timer_fullscreen(App *app) {
    if (!app->tmr_win) return;
    const Theme *th = cur_theme(app);
    int W = app->tmr_w, H = app->tmr_h;
    XftDraw *draw = app->tmr_xft;
    if (!draw) return;

    fill_rect(app, app->tmr_win, th->bg, 0, 0, W, H);

    int prog_h = PROG_H_FULL;
    int content_top = H / 8;
    int content_bot = H - prog_h - 30;

    /* Shutdown icon */
    if (app->mode == MODE_SHUTDOWN) {
        XftFont *f = get_font(app, W / 25, 0);
        if (f) {
            unsigned long ic = app->blink_on ? th->danger : th->border;
            int tw5 = text_width(app, f, "⏻");
            xft_draw_string(app, draw, f, ic,
                            (W - tw5) / 2, content_top + f->ascent, "⏻");
            content_top += f->ascent + f->descent + 8;
        }
    }

    /* Task name */
    {
        int tsz = fit_font_size(app, app->task, W - 80, H / 12, 10, 42, 0);
        XftFont *f = get_font(app, tsz, 0);
        if (f && app->task[0]) {
            xft_draw_string_centered(app, draw, f, th->text,
                                     W/2, content_top + f->ascent, app->task);
            content_top += f->ascent + f->descent + 12;
        }
    }

    /* Until time */
    if (app->until_str[0]) {
        int usz = W / 18;
        if (usz < 16) usz = 16;
        XftFont *f = get_font(app, usz, 1);
        if (f) {
            int bw = text_width(app, f, app->until_str) + 40;
            int bh = f->ascent + f->descent + 16;
            int bx = (W - bw) / 2;
            fill_rect(app, app->tmr_win, th->until_clock_bg, bx, content_top, bw, bh);
            draw_rect_outline(app, app->tmr_win, th->until_chip_border, bx, content_top, bw, bh);
            xft_draw_string_centered(app, draw, f, th->until_clock_fg,
                                     W/2, content_top + f->ascent + 8, app->until_str);
            content_top += bh + 16;
        }
    }

    /* Countdown — fits remaining space */
    {
        char time_str[16];
        format_time(app->remaining_seconds, time_str, sizeof(time_str));

        int avail_h = content_bot - content_top - 20;
        int avail_w = W - 120;

        int show = 1;
        if (app->state == STATE_PAUSED) {
            show = (int)(monotonic_now() * 2) % 2 == 0;
        }

        if (avail_w > 0 && avail_h > 0) {
            int sz = fit_font_size(app, time_str, avail_w, avail_h, 20, 280, 1);
            XftFont *f = get_font(app, sz, 1);
            if (f && show) {
                unsigned long col = timer_display_color(app);
                int tw6 = text_width(app, f, time_str);
                int cx2 = (W - tw6) / 2;
                int cy2 = content_top + (avail_h - f->ascent - f->descent) / 2 + f->ascent;
                xft_draw_string(app, draw, f, col, cx2, cy2, time_str);
            }
        }
    }

    /* Pause label */
    if (app->state == STATE_PAUSED) {
        XftFont *f = get_font(app, 22, 0);
        if (f) {
            xft_draw_string_centered(app, draw, f, th->danger,
                                     W/2, content_bot + 8, "PAUSED — click to resume");
        }
    }

    /* Progress bar */
    if (app->total_seconds > 0) {
        int px = 60, py = H - prog_h - 20, pw = W - 120;
        double frac = 1.0 - (double)app->remaining_exact / app->total_seconds;
        if (frac < 0) frac = 0;
        if (frac > 1) frac = 1;
        int bar_w = (int)(pw * frac);
        fill_rect(app, app->tmr_win, th->surface, px, py, pw, prog_h);
        if (bar_w > 0) {
            unsigned long bcol = timer_display_color(app);
            fill_rect(app, app->tmr_win, bcol, px, py, bar_w, prog_h);
        }
        /* Track border */
        draw_rect_outline(app, app->tmr_win, th->border, px, py, pw, prog_h);
    }

    /* ESC hint */
    {
        XftFont *f = get_font(app, 9, 0);
        if (f) {
            const char *hint = "Right-click: mini   Middle-click/ESC: stop   Click: pause";
            xft_draw_string_centered(app, draw, f, th->text_muted, W/2, H - 8, hint);
        }
    }

    XFlush(app->dpy);
}

static void draw_timer(App *app) {
    if (app->mini) draw_timer_mini(app);
    else            draw_timer_fullscreen(app);
}

/* ─────────────────────────────────────────────────────── */
/* Timer window creation                                   */
/* ─────────────────────────────────────────────────────── */

static void destroy_timer_window(App *app) {
    if (app->tmr_xft) { XftDrawDestroy(app->tmr_xft); app->tmr_xft = NULL; }
    if (app->tmr_win) { XDestroyWindow(app->dpy, app->tmr_win); app->tmr_win = 0; }
    XFlush(app->dpy);
}

static void create_timer_window(App *app) {
    destroy_timer_window(app);

    const Theme *th = cur_theme(app);
    Window root = DefaultRootWindow(app->dpy);

    int has_until = app->until_str[0] != '\0';

    if (app->mini) {
        /* ── Mini borderless window ─────────────────── */
        int W = MINI_W;
        int H = has_until ? MINI_H_UNTIL : MINI_H_BASE;
        /* Top-right of current monitor */
        int X = app->mon_x + app->mon_w - W - 20;
        int Y = app->mon_y + 20;
        app->tmr_w = W; app->tmr_h = H;
        app->tmr_x = X; app->tmr_y = Y;

        XSetWindowAttributes swa;
        swa.override_redirect   = True;
        swa.background_pixel    = rgb_pixel(app, th->bg);
        swa.border_pixel        = rgb_pixel(app, th->accent);
        swa.colormap            = app->cmap;
        swa.event_mask          = ExposureMask | ButtonPressMask
                                | ButtonReleaseMask | PointerMotionMask
                                | KeyPressMask;
        app->tmr_win = XCreateWindow(
            app->dpy, root, X, Y, W, H, 0,
            app->depth, InputOutput, app->vis,
            CWOverrideRedirect | CWBackPixel | CWBorderPixel
            | CWColormap | CWEventMask, &swa);

        /* EWMH: always on top + skip taskbar */
        Atom states[2] = { app->net_wm_state_above, app->net_wm_state_skip_taskbar };
        XChangeProperty(app->dpy, app->tmr_win,
                        app->net_wm_state, XA_ATOM, 32, PropModeReplace,
                        (unsigned char *)states, 2);

    } else {
        /* ── Fullscreen window ──────────────────────── */
        int W = app->mon_w;
        int H = app->mon_h;
        int X = app->mon_x;
        int Y = app->mon_y;
        app->tmr_w = W; app->tmr_h = H;
        app->tmr_x = X; app->tmr_y = Y;

        XSetWindowAttributes swa;
        swa.override_redirect   = True; /* avoid WM interference */
        swa.background_pixel    = rgb_pixel(app, th->bg);
        swa.border_pixel        = 0;
        swa.colormap            = app->cmap;
        swa.event_mask          = ExposureMask | ButtonPressMask
                                | ButtonReleaseMask | KeyPressMask
                                | StructureNotifyMask;
        app->tmr_win = XCreateWindow(
            app->dpy, root, X, Y, W, H, 0,
            app->depth, InputOutput, app->vis,
            CWOverrideRedirect | CWBackPixel | CWBorderPixel
            | CWColormap | CWEventMask, &swa);
    }

    XStoreName(app->dpy, app->tmr_win, "Timer");
    XMapRaised(app->dpy, app->tmr_win);
    XSetInputFocus(app->dpy, app->tmr_win, RevertToNone, CurrentTime);
    XFlush(app->dpy);

    app->tmr_xft = make_xft_draw(app, app->tmr_win);
}

/* ─────────────────────────────────────────────────────── */
/* Alert window                                            */
/* ─────────────────────────────────────────────────────── */

static void draw_alert(App *app) {
    if (!app->alert_win) return;
    int W = app->mon_w, H = app->mon_h;
    XftDraw *draw = app->alert_xft;

    struct timespec now_ts;
    clock_gettime(CLOCK_MONOTONIC, &now_ts);
    double elapsed = (now_ts.tv_sec  - app->alert_start_ts.tv_sec) +
                     (now_ts.tv_nsec - app->alert_start_ts.tv_nsec) * 1e-9;
    app->alert_frame++;

    unsigned long bg_rgb = alert_strobe_color(elapsed, app->alert_frame);
    fill_rect(app, app->alert_win, bg_rgb, 0, 0, W, H);

    /* Compute text brightness for contrast */
    int r2 = (bg_rgb >> 16) & 0xff;
    int g2 = (bg_rgb >> 8)  & 0xff;
    int b2 =  bg_rgb        & 0xff;
    double brightness = (r2 * 0.299 + g2 * 0.587 + b2 * 0.114) / 255.0;
    unsigned long text_col = (brightness > 0.55) ? 0x000000UL : 0xffffffUL;

    /* "Time's Up!" */
    {
        char msg[MAX_TASK + 32];
        snprintf(msg, sizeof(msg), "Time's Up!");
        int sz = fit_font_size(app, msg, W - 120, H / 4, 20, 180, 1);
        XftFont *f = get_font(app, sz, 1);
        if (f) xft_draw_string_centered(app, draw, f, text_col, W/2,
                                         H/3 + f->ascent, msg);
    }

    /* Task name */
    if (app->task[0]) {
        int sz = fit_font_size(app, app->task, W - 120, H / 8, 14, 72, 0);
        XftFont *f = get_font(app, sz, 0);
        if (f) xft_draw_string_centered(app, draw, f, text_col, W/2,
                                         H/3 + H/5 + f->ascent, app->task);
    }

    /* Dismiss button area */
    {
        unsigned long btn_bg = (brightness < 0.45) ? 0xffffffUL : 0x000000UL;
        unsigned long btn_fg = (brightness < 0.45) ? 0x000000UL : 0xffffffUL;
        int bw = 280, bh = 52;
        int bx = (W - bw) / 2, by = H * 2 / 3;
        fill_rect(app, app->alert_win, btn_bg, bx, by, bw, bh);
        draw_rect_outline(app, app->alert_win, btn_fg, bx, by, bw, bh);
        XftFont *f = get_font(app, 20, 1);
        if (f) xft_draw_string_centered(app, draw, f, btn_fg,
                                         W/2, by + f->ascent + (bh - f->ascent - f->descent)/2,
                                         "Dismiss  (ESC / click)");
    }

    XFlush(app->dpy);
}

static void create_alert_window(App *app) {
    if (app->alert_xft) { XftDrawDestroy(app->alert_xft); app->alert_xft = NULL; }
    if (app->alert_win) { XDestroyWindow(app->dpy, app->alert_win); app->alert_win = 0; }

    clock_gettime(CLOCK_MONOTONIC, &app->alert_start_ts);
    app->alert_frame = 0;

    Window root = DefaultRootWindow(app->dpy);
    int W = app->mon_w, H = app->mon_h;
    int X = app->mon_x, Y = app->mon_y;

    XSetWindowAttributes swa;
    swa.override_redirect = True;
    swa.background_pixel  = 0;
    swa.colormap          = app->cmap;
    swa.event_mask        = ExposureMask | KeyPressMask | ButtonPressMask;

    app->alert_win = XCreateWindow(
        app->dpy, root, X, Y, W, H, 0,
        app->depth, InputOutput, app->vis,
        CWOverrideRedirect | CWBackPixel | CWColormap | CWEventMask, &swa);

    XMapRaised(app->dpy, app->alert_win);
    XSetInputFocus(app->dpy, app->alert_win, RevertToNone, CurrentTime);
    XFlush(app->dpy);

    app->alert_xft = make_xft_draw(app, app->alert_win);
}

/* ─────────────────────────────────────────────────────── */
/* State transitions                                       */
/* ─────────────────────────────────────────────────────── */

static void do_quit(App *app) {
    app_log(app, "INFO", "Quitting");
    app->alert_audio_active = 0;
    kill_all_audio(app);
    db_finish_session(app, "stopped");
    if (app->alert_xft) { XftDrawDestroy(app->alert_xft); app->alert_xft = NULL; }
    if (app->alert_win) { XDestroyWindow(app->dpy, app->alert_win); app->alert_win = 0; }
    destroy_timer_window(app);
    if (app->cfg_xft)  { XftDrawDestroy(app->cfg_xft);  app->cfg_xft  = NULL; }
    if (app->cfg_win)  { XDestroyWindow(app->dpy, app->cfg_win); app->cfg_win = 0; }
    XFlush(app->dpy);
    app->state = STATE_DONE;
}

static void dismiss_alert(App *app) {
    app_log(app, "INFO", "Alert dismissed");
    app->alert_audio_active = 0;
    kill_all_audio(app);
    play_cue(app, "dismiss");
    db_finish_session(app, "completed");
    do_quit(app);
}

static void stop_timer(App *app) {
    app_log(app, "INFO", "Timer stopped by user");
    play_cue(app, "stop");
    db_finish_session(app, "stopped");
    do_quit(app);
}

static void start_running(App *app) {
    app->state = STATE_RUNNING;
    app->cue_half_played = 0;
    memset(app->cue_last3, 0, sizeof(app->cue_last3));
    app->prev_remaining = app->total_seconds;

    clock_gettime(CLOCK_MONOTONIC, &app->start_ts);
    app->remaining_exact = (double)app->total_seconds;
    app->pause_total = 0.0;

    /* Detect monitor based on config window position */
    if (app->cfg_win) {
        Window root_ret, child_ret;
        int rx, ry, wx, wy;
        unsigned int mask;
        XQueryPointer(app->dpy, DefaultRootWindow(app->dpy),
                      &root_ret, &child_ret, &rx, &ry, &wx, &wy, &mask);
        detect_monitor(app, rx, ry);
    }

    create_timer_window(app);
    db_start_session(app);

    app_log(app, "INFO", "Timer started: %ds, task='%s', mode=%s, mini=%d, pausable=%d",
            app->total_seconds, app->task,
            app->mode == MODE_SHUTDOWN ? "shutdown" : "alert",
            app->mini, app->pausable);

    /* Hide config window */
    if (app->cfg_win) XWithdrawWindow(app->dpy, app->cfg_win, app->screen);
    XFlush(app->dpy);

    play_cue(app, "start");
}

static void toggle_mini_fullscreen(App *app) {
    app->mini = !app->mini;
    app_log(app, "INFO", "Toggled to %s mode", app->mini ? "mini" : "fullscreen");
    create_timer_window(app);
    play_cue(app, app->mini ? "pause" : "resume");
}

static void toggle_pause(App *app) {
    if (!app->pausable) return;
    if (app->state == STATE_RUNNING) {
        app->state = STATE_PAUSED;
        clock_gettime(CLOCK_MONOTONIC, &app->pause_start_ts);
        play_cue(app, "pause");
        app_log(app, "INFO", "Timer paused");
    } else if (app->state == STATE_PAUSED) {
        struct timespec now_ts;
        clock_gettime(CLOCK_MONOTONIC, &now_ts);
        double paused_for = (now_ts.tv_sec  - app->pause_start_ts.tv_sec) +
                            (now_ts.tv_nsec - app->pause_start_ts.tv_nsec) * 1e-9;
        app->pause_total += paused_for;
        app->state = STATE_RUNNING;
        play_cue(app, "resume");
        app_log(app, "INFO", "Timer resumed");
    }
}

static void timer_finished(App *app) {
    app_log(app, "INFO", "Timer finished: task='%s', mode=%s",
            app->task, app->mode == MODE_SHUTDOWN ? "shutdown" : "alert");

    app->remaining_seconds = 0;
    app->remaining_exact   = 0.0;

    destroy_timer_window(app);

    if (app->mode == MODE_SHUTDOWN) {
        play_cue(app, "stop");
        db_finish_session(app, "completed");
        app_log(app, "WARN", "Initiating system shutdown");
        usleep(800000);
        execl("/usr/bin/systemctl", "systemctl", "poweroff", (char *)NULL);
        /* Fallback */
        run_cmd("systemctl poweroff");
        do_quit(app);
    } else {
        /* Alert mode */
        play_cue(app, "alert_open");
        create_alert_window(app);
        app->state = STATE_ALERT;
        play_alert_sound(app);
    }
}

/* ─────────────────────────────────────────────────────── */
/* Timer tick                                              */
/* ─────────────────────────────────────────────────────── */

static void maybe_play_cues(App *app, int prev_rem) {
    if (app->total_seconds <= 0) return;
    /* Half-time cue */
    if (!app->cue_half_played) {
        int half = app->total_seconds / 2;
        if (prev_rem > half && app->remaining_seconds <= half) {
            app->cue_half_played = 1;
            play_cue(app, "half");
        }
    }
    /* Last 3 seconds */
    for (int i = 0; i < 3; i++) {
        int s = 3 - i; /* 3, 2, 1 */
        if (!app->cue_last3[i] && prev_rem > s && app->remaining_seconds <= s) {
            app->cue_last3[i] = 1;
            play_cue(app, "last");
        }
    }
}

static void do_timer_tick(App *app) {
    if (app->state != STATE_RUNNING && app->state != STATE_PAUSED) return;

    int prev_rem = app->remaining_seconds;

    if (app->state == STATE_RUNNING) {
        struct timespec now_ts;
        clock_gettime(CLOCK_MONOTONIC, &now_ts);
        double elapsed = (now_ts.tv_sec  - app->start_ts.tv_sec) +
                         (now_ts.tv_nsec - app->start_ts.tv_nsec) * 1e-9
                         - app->pause_total;
        app->remaining_exact   = (double)app->total_seconds - elapsed;
        if (app->remaining_exact < 0.0) app->remaining_exact = 0.0;
        app->remaining_seconds = (int)ceil(app->remaining_exact);
        if (app->remaining_seconds < 0) app->remaining_seconds = 0;
    }

    /* Cursor blink in timer mode (for paused blink) */
    long now_ms = ms_now();
    if (now_ms - app->cursor_blink_ts > 500) {
        app->cursor_blink = !app->cursor_blink;
        app->cursor_blink_ts = now_ms;
    }

    /* Shutdown icon blink */
    if (app->mode == MODE_SHUTDOWN && now_ms - app->blink_ts > 700) {
        app->blink_on = !app->blink_on;
        app->blink_ts = now_ms;
    }

    if (app->state == STATE_RUNNING) {
        maybe_play_cues(app, prev_rem);
        if (app->remaining_seconds <= 0 && app->remaining_exact <= 0.0) {
            timer_finished(app);
            return;
        }
    }

    draw_timer(app);
}

static void do_alert_tick(App *app) {
    draw_alert(app);
}

/* ─────────────────────────────────────────────────────── */
/* Event handling                                          */
/* ─────────────────────────────────────────────────────── */

static void handle_config_key(App *app, XKeyEvent *ev) {
    char buf[32] = "";
    KeySym ks;
    XLookupString(ev, buf, sizeof(buf) - 1, &ks, NULL);

    /* Ctrl-T: cycle theme */
    if ((ev->state & ControlMask) && (ks == XK_t || ks == XK_T)) {
        app->theme_idx = (app->theme_idx + 1) % N_THEMES;
        app_log(app, "INFO", "Theme: %s", cur_theme(app)->name);
        play_cue(app, "resume");
        /* Rebuild window background color */
        XSetWindowBackground(app->dpy, app->cfg_win,
                             rgb_pixel(app, cur_theme(app)->bg));
        draw_config(app);
        return;
    }

    /* Ctrl-A: move cursor to end */
    if ((ev->state & ControlMask) && (ks == XK_a || ks == XK_A)) {
        app->cursor_pos = app->input_len;
        app->cursor_blink = 1;
        app->cursor_blink_ts = ms_now();
        draw_config(app);
        return;
    }

    if (ks == XK_Escape) { do_quit(app); return; }

    if (ks == XK_Return || ks == XK_KP_Enter) {
        /* Parse and start */
        ParseResult pr;
        parse_time_input(app->input, &pr);
        if (pr.seconds <= 0) {
            /* Flash border red briefly by redrawing — TODO: visual feedback */
            app_log(app, "WARN", "Invalid time input: '%s'", app->input);
            return;
        }
        app->total_seconds     = pr.seconds;
        app->remaining_seconds = pr.seconds;
        app->remaining_exact   = (double)pr.seconds;
        snprintf(app->task,      sizeof(app->task),  "%s", pr.task);
        snprintf(app->until_str, sizeof(app->until_str), "%s", pr.until);
        if (pr.sw_shutdown) app->mode = MODE_SHUTDOWN;
        else                app->mode = MODE_ALERT;
        if (pr.sw_fullscreen) app->mini = 0;
        if (pr.sw_pausable)   app->pausable = 1;
        start_running(app);
        return;
    }

    if (ks == XK_BackSpace) {
        if (app->cursor_pos > 0) {
            memmove(app->input + app->cursor_pos - 1,
                    app->input + app->cursor_pos,
                    app->input_len - app->cursor_pos + 1);
            app->cursor_pos--;
            app->input_len--;
        }
    } else if (ks == XK_Delete) {
        if (app->cursor_pos < app->input_len) {
            memmove(app->input + app->cursor_pos,
                    app->input + app->cursor_pos + 1,
                    app->input_len - app->cursor_pos);
            app->input_len--;
        }
    } else if (ks == XK_Left) {
        if (app->cursor_pos > 0) app->cursor_pos--;
    } else if (ks == XK_Right) {
        if (app->cursor_pos < app->input_len) app->cursor_pos++;
    } else if (ks == XK_Home) {
        app->cursor_pos = 0;
    } else if (ks == XK_End) {
        app->cursor_pos = app->input_len;
    } else if (buf[0] && isprint((unsigned char)buf[0])
               && !(ev->state & ControlMask)) {
        int ch_len = (int)strlen(buf);
        if (app->input_len + ch_len < MAX_INPUT - 1) {
            memmove(app->input + app->cursor_pos + ch_len,
                    app->input + app->cursor_pos,
                    app->input_len - app->cursor_pos + 1);
            memcpy(app->input + app->cursor_pos, buf, ch_len);
            app->cursor_pos += ch_len;
            app->input_len  += ch_len;
        }
    }

    app->cursor_blink = 1;
    app->cursor_blink_ts = ms_now();
    draw_config(app);
}

static void handle_event(App *app, XEvent *ev) {
    /* Config window events */
    if (app->cfg_win && ev->xany.window == app->cfg_win) {
        switch (ev->type) {
        case Expose:
            if (ev->xexpose.count == 0) draw_config(app);
            break;
        case KeyPress:
            handle_config_key(app, &ev->xkey);
            break;
        case ButtonPress:
            /* Right-click = fullscreen start, left-click = focus */
            if (ev->xbutton.button == Button3) {
                app->fullscreen_requested = 1;
                app->mini = 0;
            }
            break;
        case ClientMessage:
            if ((Atom)ev->xclient.data.l[0] == app->wm_delete)
                do_quit(app);
            break;
        }
        return;
    }

    /* Timer window events */
    if (app->tmr_win && ev->xany.window == app->tmr_win) {
        switch (ev->type) {
        case Expose:
            if (ev->xexpose.count == 0) draw_timer(app);
            break;
        case KeyPress: {
            KeySym ks = XLookupKeysym(&ev->xkey, 0);
            if (ks == XK_Escape) { stop_timer(app); return; }
            if (ks == XK_space && app->pausable) toggle_pause(app);
            break;
        }
        case ButtonPress: {
            int bx = ev->xbutton.x, by = ev->xbutton.y;
            if (ev->xbutton.button == Button2) {
                /* Middle: stop */
                stop_timer(app); return;
            }
            if (ev->xbutton.button == Button3) {
                /* Right: toggle mini/fullscreen */
                toggle_mini_fullscreen(app); return;
            }
            if (ev->xbutton.button == Button1) {
                /* Check resize corner (mini only) */
                if (app->mini) {
                    int W = app->tmr_w, H = app->tmr_h;
                    if (bx >= W - RESIZE_ZONE && by >= H - RESIZE_ZONE) {
                        app->is_resizing = 1;
                        app->resize_px = ev->xbutton.x_root;
                        app->resize_py = ev->xbutton.y_root;
                        app->resize_wx = app->tmr_x;
                        app->resize_wy = app->tmr_y;
                        app->resize_ww = app->tmr_w;
                        app->resize_wh = app->tmr_h;
                        return;
                    }
                }
                /* Pause toggle */
                if (app->pausable) {
                    toggle_pause(app);
                    return;
                }
                /* Start drag */
                app->is_dragging = 1;
                app->drag_px = ev->xbutton.x_root;
                app->drag_py = ev->xbutton.y_root;
                app->drag_wx = app->tmr_x;
                app->drag_wy = app->tmr_y;
            }
            break;
        }
        case ButtonRelease:
            app->is_dragging = 0;
            app->is_resizing = 0;
            break;
        case MotionNotify:
            if (app->is_dragging && app->mini) {
                int dx = ev->xmotion.x_root - app->drag_px;
                int dy = ev->xmotion.y_root - app->drag_py;
                app->tmr_x = app->drag_wx + dx;
                app->tmr_y = app->drag_wy + dy;
                XMoveWindow(app->dpy, app->tmr_win, app->tmr_x, app->tmr_y);
            } else if (app->is_resizing && app->mini) {
                int dx = ev->xmotion.x_root - app->resize_px;
                int dy = ev->xmotion.y_root - app->resize_py;
                int nw = app->resize_ww + dx;
                int nh = app->resize_wh + dy;
                if (nw < 130) nw = 130;
                if (nh < 72)  nh = 72;
                app->tmr_w = nw; app->tmr_h = nh;
                XResizeWindow(app->dpy, app->tmr_win, nw, nh);
                draw_timer(app);
            }
            break;
        case ClientMessage:
            if ((Atom)ev->xclient.data.l[0] == app->wm_delete)
                stop_timer(app);
            break;
        }
        return;
    }

    /* Alert window events */
    if (app->alert_win && ev->xany.window == app->alert_win) {
        switch (ev->type) {
        case Expose:
            if (ev->xexpose.count == 0) draw_alert(app);
            break;
        case KeyPress: {
            KeySym ks = XLookupKeysym(&ev->xkey, 0);
            if (ks == XK_Escape) { dismiss_alert(app); return; }
            break;
        }
        case ButtonPress:
            dismiss_alert(app); return;
        }
        return;
    }
}

/* ─────────────────────────────────────────────────────── */
/* Cursor blink in config window                           */
/* ─────────────────────────────────────────────────────── */

static void do_config_tick(App *app) {
    long now_ms = ms_now();
    if (now_ms - app->cursor_blink_ts > 530) {
        app->cursor_blink = !app->cursor_blink;
        app->cursor_blink_ts = now_ms;
        if (app->cfg_win && app->state == STATE_CONFIG) {
            /* Partial redraw: just the input area */
            draw_config(app);
        }
    }
}

/* ─────────────────────────────────────────────────────── */
/* Main event loop                                         */
/* ─────────────────────────────────────────────────────── */

static void main_loop(App *app) {
    int xfd = XConnectionNumber(app->dpy);

    while (app->state != STATE_DONE && !app->quit) {
        XFlush(app->dpy);

        int timeout_ms = TICK_MS;

        struct timeval tv;
        tv.tv_sec  = timeout_ms / 1000;
        tv.tv_usec = (timeout_ms % 1000) * 1000;

        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(xfd, &rfds);

        int nfds = select(xfd + 1, &rfds, NULL, NULL, &tv);
        if (nfds < 0) {
            if (errno == EINTR) continue;
            break;
        }

        /* Drain all pending events */
        while (XPending(app->dpy)) {
            XEvent ev;
            XNextEvent(app->dpy, &ev);
            if (app->state == STATE_DONE) break;
            handle_event(app, &ev);
        }

        if (app->state == STATE_DONE) break;

        /* Ticks */
        switch (app->state) {
        case STATE_CONFIG:
            do_config_tick(app);
            break;
        case STATE_RUNNING:
        case STATE_PAUSED:
            do_timer_tick(app);
            break;
        case STATE_ALERT:
            do_alert_tick(app);
            break;
        default:
            break;
        }
    }
}

/* ─────────────────────────────────────────────────────── */
/* App initialization                                      */
/* ─────────────────────────────────────────────────────── */

static void signal_handler(int sig) {
    (void)sig;
    if (g_app) g_app->quit = 1;
}

static int app_init(App *app) {
    memset(app, 0, sizeof(*app));

    /* Paths */
    const char *home = getenv("HOME");
    if (!home) home = "/tmp";
    /* Use a smaller dir buffer so log/db paths fit in MAX_PATH_LEN */
    char data_dir[MAX_PATH_LEN - 16];
    snprintf(data_dir, sizeof(data_dir), "%s/.local/share/timer_c", home);
    mkdir(data_dir, 0755);

    snprintf(app->log_path, sizeof(app->log_path), "%s/timer.log", data_dir);
    snprintf(app->db_path,  sizeof(app->db_path),  "%s/sessions.db", data_dir);

    app->logfp = fopen(app->log_path, "a");

    /* Defaults */
    app->theme_idx  = rand() % N_THEMES;
    app->state      = STATE_CONFIG;
    app->mode       = MODE_ALERT;
    app->mini       = 1;
    app->pausable   = 0;
    app->session_id = 0;
    app->blink_on   = 1;
    app->blink_ts   = ms_now();

    pthread_mutex_init(&app->audio_lock, NULL);

    /* X11 */
    app->dpy = XOpenDisplay(NULL);
    if (!app->dpy) {
        fprintf(stderr, "Cannot open X display\n");
        return 0;
    }
    app->screen = DefaultScreen(app->dpy);
    app->vis    = DefaultVisual(app->dpy, app->screen);
    app->cmap   = DefaultColormap(app->dpy, app->screen);
    app->depth  = DefaultDepth(app->dpy, app->screen);

    /* Atoms */
    app->wm_delete                   = XInternAtom(app->dpy, "WM_DELETE_WINDOW", False);
    app->net_wm_state                = XInternAtom(app->dpy, "_NET_WM_STATE", False);
    app->net_wm_state_above          = XInternAtom(app->dpy, "_NET_WM_STATE_ABOVE", False);
    app->net_wm_state_fullscreen     = XInternAtom(app->dpy, "_NET_WM_STATE_FULLSCREEN", False);
    app->net_wm_state_skip_taskbar   = XInternAtom(app->dpy, "_NET_WM_STATE_SKIP_TASKBAR", False);
    app->net_wm_window_type          = XInternAtom(app->dpy, "_NET_WM_WINDOW_TYPE", False);
    app->net_wm_window_type_dock     = XInternAtom(app->dpy, "_NET_WM_WINDOW_TYPE_DOCK", False);

    /* GC */
    app->gc = XCreateGC(app->dpy, DefaultRootWindow(app->dpy), 0, NULL);

    /* Monitor (initial) */
    app->mon_x = 0; app->mon_y = 0;
    app->mon_w = DisplayWidth(app->dpy, app->screen);
    app->mon_h = DisplayHeight(app->dpy, app->screen);

    /* SQLite */
    db_init(app);

    /* Signals */
    g_app = app;
    signal(SIGINT,  signal_handler);
    signal(SIGTERM, signal_handler);
    signal(SIGPIPE, SIG_IGN);

    app_log(app, "INFO", "timer_c started, theme: %s", cur_theme(app)->name);
    return 1;
}

/* ─────────────────────────────────────────────────────── */
/* CLI argument parsing                                    */
/* ─────────────────────────────────────────────────────── */

static void print_usage(const char *prog) {
    fprintf(stderr,
        "Usage: %s [time] [task] [options]\n\n"
        "Time formats:\n"
        "  25m          25 minutes\n"
        "  1h30m        1 hour 30 minutes\n"
        "  45           45 minutes (default unit)\n"
        "  30:00        30 minutes (mm:ss)\n"
        "  1:30:00      1 hour 30 min\n"
        "  u15:30       Until 15:30\n"
        "  u1520        Until 15:20\n\n"
        "Input switches (prefix before time):\n"
        "  d            Shutdown mode\n"
        "  a            Alert mode\n"
        "  f / r        Fullscreen timer\n"
        "  p            Pausable\n\n"
        "Options:\n"
        "  -m alert|shutdown   Mode\n"
        "  -p / --pausable     Enable pause\n"
        "  -f / --fullscreen   Fullscreen timer\n"
        "  --theme NAME        Theme: 'Night Ember|Moss|Tide|Ash|Plum'\n"
        "  -h / --help         Show this help\n\n"
        "Examples:\n"
        "  %s 25m focus\n"
        "  %s 1h deep work -p\n"
        "  %s d30m shutdown in 30\n"
        "  %s u16:30 meeting\n"
        "  %s          (interactive config window)\n",
        prog, prog, prog, prog, prog, prog);
}

/* ─────────────────────────────────────────────────────── */
/* main                                                    */
/* ─────────────────────────────────────────────────────── */

int main(int argc, char **argv) {
    srand((unsigned)time(NULL));

    App app;
    if (!app_init(&app)) return 1;

    /* ── Parse CLI ──────────────────────────────────── */
    char cli_time[MAX_INPUT]  = "";
    char cli_task[MAX_TASK]   = "";
    int  cli_mode    = -1;  /* -1 = not set */
    int  cli_pausable = 0;
    int  cli_fullscreen = 0;
    int  cli_theme = -1;
    int  skip_next = 0;

    for (int i = 1; i < argc; i++) {
        if (skip_next) { skip_next = 0; continue; }
        const char *a = argv[i];

        if (!strcmp(a, "-h") || !strcmp(a, "--help")) {
            print_usage(argv[0]);
            return 0;
        }
        if (!strcmp(a, "-p") || !strcmp(a, "--pausable")) {
            cli_pausable = 1; continue;
        }
        if (!strcmp(a, "-f") || !strcmp(a, "--fullscreen")) {
            cli_fullscreen = 1; continue;
        }
        if (!strcmp(a, "-m") || !strcmp(a, "--mode")) {
            if (i + 1 < argc) {
                const char *m = argv[++i];
                cli_mode = (!strcmp(m, "shutdown")) ? 1 : 0;
            }
            continue;
        }
        if (!strcmp(a, "--theme")) {
            if (i + 1 < argc) {
                const char *tn = argv[++i];
                for (int t = 0; t < N_THEMES; t++) {
                    if (!strcasecmp(tn, THEMES[t].name) ||
                        /* short match */
                        (strstr(THEMES[t].name, " ") &&
                         !strncasecmp(tn, strstr(THEMES[t].name," ")+1, strlen(tn)))) {
                        cli_theme = t; break;
                    }
                }
            }
            continue;
        }
        /* First non-flag arg is time (may include task) */
        if (a[0] != '-' && cli_time[0] == '\0') {
            snprintf(cli_time, sizeof(cli_time), "%s", a);
            /* Collect remaining non-flag args as task */
            char task_buf[MAX_TASK] = "";
            for (int j = i + 1; j < argc; j++) {
                if (argv[j][0] == '-') break;
                if (task_buf[0]) strncat(task_buf, " ", sizeof(task_buf) - strlen(task_buf) - 1);
                strncat(task_buf, argv[j], sizeof(task_buf) - strlen(task_buf) - 1);
                i = j;
            }
            snprintf(cli_task, sizeof(cli_task), "%s", task_buf);
            continue;
        }
    }

    /* Apply CLI overrides */
    if (cli_theme >= 0) app.theme_idx = cli_theme;
    if (cli_pausable)   app.pausable  = 1;
    if (cli_fullscreen) app.mini      = 0;
    if (cli_mode == 1)  app.mode      = MODE_SHUTDOWN;
    if (cli_mode == 0)  app.mode      = MODE_ALERT;

    /* ── Direct CLI start ───────────────────────────── */
    if (cli_time[0]) {
        /* Build combined input string (time + maybe separate task) */
        char combined[MAX_INPUT];
        if (cli_task[0]) {
            snprintf(combined, sizeof(combined), "%s %s", cli_time, cli_task);
        } else {
            snprintf(combined, sizeof(combined), "%s", cli_time);
        }

        ParseResult pr;
        parse_time_input(combined, &pr);
        if (pr.seconds <= 0) {
            fprintf(stderr, "Error: invalid time '%s'\n", combined);
            print_usage(argv[0]);
            return 1;
        }
        app.total_seconds     = pr.seconds;
        app.remaining_seconds = pr.seconds;
        app.remaining_exact   = (double)pr.seconds;

        /* If cli_task was given separately, prefer it over parsed task */
        if (cli_task[0])
            snprintf(app.task, sizeof(app.task), "%s", cli_task);
        else
            snprintf(app.task, sizeof(app.task), "%s", pr.task);

        snprintf(app.until_str, sizeof(app.until_str), "%s", pr.until);

        /* CLI switches take lowest priority */
        if (pr.sw_shutdown && cli_mode < 0) app.mode = MODE_SHUTDOWN;
        if (pr.sw_fullscreen && !cli_fullscreen) app.mini = 0;
        if (pr.sw_pausable   && !cli_pausable)   app.pausable = 1;

        /* We still need the config window created so Tk mainloop works… 
         * but we're native, so just detect monitor from pointer: */
        Window root_ret, child_ret;
        int rx, ry, wx, wy;
        unsigned int mask;
        XQueryPointer(app.dpy, DefaultRootWindow(app.dpy),
                      &root_ret, &child_ret, &rx, &ry, &wx, &wy, &mask);
        detect_monitor(&app, rx, ry);

        start_running(&app);
    } else {
        /* ── Interactive config window ──────────────── */
        create_config_window(&app);
        play_cue(&app, "launch");
    }

    main_loop(&app);

    /* Cleanup */
    if (app.tmr_xft)   XftDrawDestroy(app.tmr_xft);
    if (app.cfg_xft)   XftDrawDestroy(app.cfg_xft);
    if (app.alert_xft) XftDrawDestroy(app.alert_xft);
    if (app.gc)        XFreeGC(app.dpy, app.gc);
    for (int i = 0; i < app.n_fonts; i++)
        if (app.fonts[i].font) XftFontClose(app.dpy, app.fonts[i].font);
    if (app.db) sqlite3_close(app.db);
    if (app.logfp) fclose(app.logfp);
    if (app.dpy) XCloseDisplay(app.dpy);

    pthread_mutex_destroy(&app.audio_lock);
    return 0;
}
