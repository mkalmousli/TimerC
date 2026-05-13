/* =========================================================
 * timer.h — Shared types, structs, constants and prototypes
 * ========================================================= */
#pragma once

#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>
#include <X11/keysym.h>
#include <X11/XKBlib.h>
#include <X11/cursorfont.h>
#include <X11/Xft/Xft.h>
#include <X11/extensions/Xinerama.h>
#include <X11/extensions/Xrandr.h>
#include <X11/extensions/Xrender.h>

#ifdef USE_GL
#include <GL/gl.h>
#include <GL/glx.h>
#endif

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

/* ── Constants ─────────────────────────────────────────── */

#define APP_NAME        "timer_c"
#define APP_MAX_INPUT   512
#define MAX_TASK        256
#define MAX_UNTIL       16
#define MAX_PATH_LEN    4096
#undef  MAX_INPUT
#define MAX_INPUT       APP_MAX_INPUT
#define TICK_MS         50
#define N_THEMES        4
#define SAMPLE_RATE     44100
#define TAU             (2.0 * M_PI)
#define MAX_MONITORS    8
#define MAX_FONT_CACHE  64

/* Tick pacing (lower = smoother, higher = lower CPU/GPU) */
#define TICK_CONFIG_MS        500
#define TICK_TIMER_RUN_MS    1000
#define TICK_TIMER_LAST10_MS  100
#define TICK_TIMER_PAUSED_MS 1000
#define TICK_ALERT_MS          50

/* Config window dimensions */
#define CFG_W   500
#define CFG_H   760

/* Mini timer default dimensions */
#define MINI_W        250
#define MINI_H_BASE   130
#define MINI_H_UNTIL  172

/* Mini resize constraints */
#define MINI_W_MIN    130
#define MINI_H_MIN     72

/* Resize corner hit zone */
#define RESIZE_ZONE    22

/* ── Enums ─────────────────────────────────────────────── */

typedef enum {
    STATE_CONFIG, STATE_RUNNING, STATE_PAUSED, STATE_ALERT, STATE_DONE
} AppState;

typedef enum { MODE_ALERT, MODE_SHUTDOWN } TimerMode;

/* ── Theme ─────────────────────────────────────────────── */

typedef struct {
    const char   *name;
    unsigned long bg, surface, surface_soft;
    unsigned long text, text_muted;
    unsigned long accent, accent_soft;
    unsigned long border, danger;
    unsigned long until_chip_bg, until_chip_fg, until_chip_border;
    unsigned long until_clock_fg, until_clock_bg;
    unsigned long lcd_active, lcd_paused;
} Theme;

extern const Theme THEMES[N_THEMES];

/* ── Font cache entry ──────────────────────────────────── */

/* style: 0=regular  1=bold  2=italic  3=monospace-bold */
typedef struct {
    int      size;
    int      style;
    XftFont *font;
} FontEntry;

/* ── Audio job ─────────────────────────────────────────── */

typedef struct {
    int16_t        *samples;
    int             n_samples;
    int             fd_write;
    volatile int   *active_flag;
    float           alert_ramp_seconds;
} AudioJob;

/* ── Per-monitor alert overlay ─────────────────────────── */

typedef struct {
    Window    win;
    XftDraw  *xft;
    Pixmap    buf;
    XftDraw  *buf_xft;
    int       x, y, w, h;
} AlertScreen;

/* ── Parse result ──────────────────────────────────────── */

typedef struct {
    int  seconds;
    char task[MAX_TASK];
    char until[MAX_UNTIL];
    int  sw_shutdown, sw_alert, sw_fullscreen, sw_pausable;
} ParseResult;

/* ── App state struct ──────────────────────────────────── */

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
    Atom net_wm_state_sticky;
    Atom net_wm_desktop;
    Atom net_wm_bypass_compositor;
    Atom net_wm_window_type;
    Atom net_wm_window_type_dock;

    /* Windows */
    Window     cfg_win;
    XftDraw   *cfg_xft;
    int        cfg_x, cfg_y;
    Window     tmr_win;
    XftDraw   *tmr_xft;

    /* Alert overlays — one per monitor */
    AlertScreen alert_screens[MAX_MONITORS];
    int         n_alert_screens;

    /* Double-buffering pixmaps */
    Pixmap     cfg_buf;
    XftDraw   *cfg_buf_xft;
    Pixmap     tmr_buf;
    XftDraw   *tmr_buf_xft;
    int        tmr_buf_w, tmr_buf_h;   /* allocated size (may be >= window size) */

    /* Saved mini-mode geometry (restored when leaving fullscreen) */
    int        saved_mini_x, saved_mini_y;
    int        saved_mini_w, saved_mini_h;
    int        has_saved_mini;

    /* Countdown font-size cache */
    int        cd_font_sz;
    int        cd_font_avail_w, cd_font_avail_h;
    int        cd_font_fmt_len;

    /* Font cache */
    FontEntry  fonts[MAX_FONT_CACHE];
    int        n_fonts;

    /* App state */
    AppState   state;
    TimerMode  mode;
    int        mini;
    int        pausable;
    int        fullscreen_requested;

    /* Timer */
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
    int        cursor_blink;
    long       cursor_blink_ts;
    int        quote_idx;
    int        quote_prev_idx;
    long       quote_cycle_ts;

    /* Task / until */
    char       task[MAX_TASK];
    char       until_str[MAX_UNTIL];

    /* Theme */
    int        theme_idx;
    int        cfg_focused;   /* 1 = config window has keyboard focus */

    /* Monitor geometry */
    int        mon_x, mon_y, mon_w, mon_h;

    /* Timer window geometry */
    int        tmr_x, tmr_y, tmr_w, tmr_h;

    /* Mini-window drag */
    int        drag_px, drag_py;
    int        drag_wx, drag_wy;
    int        is_dragging;

    /* Config-window drag */
    int        cfg_is_dragging;
    int        cfg_drag_px, cfg_drag_py;
    int        cfg_drag_wx, cfg_drag_wy;

    /* Corner resize */
    int        is_resizing;
    int        resize_px, resize_py;
    int        resize_wx, resize_wy;
    int        resize_ww, resize_wh;
    Cursor     cursor_resize;
    int        cursor_in_resize;

    /* OpenGL (optional) */
    int        use_gl;
#ifdef USE_GL
    GLXContext glx_ctx;
    XVisualInfo *glx_vi;
    Colormap   glx_cmap;
#endif

    /* Alert animation */
    struct timespec alert_start_ts;
    int        alert_frame;

    /* Shutdown icon blink */
    int        blink_on;
    long       blink_ts;

    /* Audio */
    volatile int    alert_audio_active;
    pid_t           audio_pids[8];
    int             n_audio_pids;
    pthread_mutex_t audio_lock;

    /* Cue state */
    int        cue_half_played;
    int        cue_last3[3];
    int        prev_remaining;

    /* Redraw pacing */
    long       tmr_last_draw_ms;
    int        tmr_force_redraw;
    time_t     cfg_last_wall_sec;

    /* SQLite */
    sqlite3   *db;
    int64_t    session_id;
    int        session_finalized;

    /* Log */
    FILE      *logfp;
    char       log_path[MAX_PATH_LEN];
    char       db_path[MAX_PATH_LEN];

    /* Quit flag */
    volatile int quit;
} App;

/* ── Inline time utilities (used in multiple TUs) ──────── */

static inline double monotonic_now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

static inline long ms_now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long)(ts.tv_sec * 1000L + ts.tv_nsec / 1000000L);
}

/* ── Function prototypes ───────────────────────────────── */

/* timer.c */
void app_log(App *app, const char *level, const char *fmt, ...);
void detect_monitor(App *app, int ptr_x, int ptr_y);

/* parse.c */
void format_time(int secs, char *out, size_t sz);
void parse_time_input(const char *raw, ParseResult *out);
void get_preview_switches(const char *input,
                          int *sw_d, int *sw_a, int *sw_f, int *sw_p,
                          char *until_buf, int until_sz);

/* audio.c */
void play_cue(App *app, const char *kind);
void play_alert_sound(App *app);
void kill_all_audio(App *app);

/* db.c */
void db_init(App *app);
void db_start_session(App *app);
void db_finish_session(App *app, const char *status);

/* draw.c */
void draw_config(App *app);
void draw_timer(App *app);
void draw_alert(App *app);
void create_config_window(App *app);
void create_timer_window(App *app);
void destroy_timer_window(App *app);
void create_alert_windows(App *app);
void destroy_alert_windows(App *app);

#ifdef USE_GL
int    gl_init(App *app);
void   gl_shutdown(App *app);
Window gl_create_timer_window(App *app, int x, int y, int w, int h, int fullscreen);
void   gl_make_current(App *app, Window win);
void   gl_draw_timer(App *app);
#endif
