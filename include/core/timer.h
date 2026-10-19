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
#include "lock.h"

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
#include <dirent.h>

/* ── Constants ─────────────────────────────────────────── */

#define APP_NAME        "Timer In Pure C"
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

/* Insights / data browser (Ctrl+I in config window) */
#define INS_W             1180
#define INS_H              880
#define INS_MAX_SESSIONS   128
#define INS_MAX_EVENTS     128
#define INS_CHART_DAYS      14
#define INS_SESS_VISIBLE    16
#define INS_EVT_VISIBLE     20

/* Alarm rhythm — shared between audio.c (note synthesis) and draw.c (the
 * alert window flashes a color on each strike, in time with the sound).
 * Onsets are seconds-into-period for the six notes of the motif. The
 * motif's note count/shape is fixed at compile time; its tempo/volume/
 * timing are runtime-configurable — see config.c and the Config struct
 * below. */
#define ALARM_N       6
#define ALARM_ONSETS  { 0.00f, 0.40f, 0.80f, 1.20f, 1.65f, 2.05f }

/* All other tunables below (tick pacing, alarm tempo/volume/duration,
 * window sizing) live in the Config struct, filled in by config_load()
 * in config.c. There is no config file — every default constant is
 * defined right there in config.c, next to the field it initializes;
 * to change a value, edit it there and rebuild. Nothing should
 * reference a default constant directly — use g_config.* instead. */

/* ── Config (user-tunable settings) ────────────────────────
 * No config file — config_load() in config.c assigns every field below
 * directly. Access the live values through the global g_config.
 *
 * ONE kind of "volume", one reference point: every *_volume_percent
 * field below is your actual SYSTEM/speaker output level, 0-100%, with
 * 100% meaning "full system volume" — never a separate PCM/app-level
 * gain layered on top of it. Sounds themselves always play at their
 * natural full level; set_system_volume() (audio.c) is what actually
 * gets you the requested loudness, by moving your system volume up or
 * down — instantly, automatically, in whichever direction is needed —
 * the moment a phase/alarm starts. See set_system_volume() for the
 * mechanism (pactl, per-sink, unmuting as it goes). */
typedef struct {
    /* ── Alarm volume ────────────────────────────────────────
     * System volume the alarm ramps from/to as it fades in — see
     * alarm_ramp_seconds below for how long that takes. */
    double alarm_volume_min_percent; /* system volume set the instant the alarm starts */
    double alarm_volume_max_percent; /* system volume ramped to by the end of the fade-in */

    /* ── Alarm: sound/timing ────────────────────────────────
     * Everything else about the alarm — its own tempo and how long it
     * runs — independent of the volume settings above. */
    double alarm_ramp_seconds;    /* fade-in length of the volume ramp above       */
    double alarm_period;          /* base seconds per motif repeat                 */
    double alarm_period_min;      /* fastest (shortest) period the tempo ramps to  */
    double alarm_accel;           /* tempo increase per second of alerting         */
    double alarm_aggro_seconds;   /* seconds for brightness/level to ramp 0→1      */
    double alarm_max_seconds;     /* auto-mute the alarm after this long           */

    /* ── Sleep mode: volume ──────────────────────────────────
     * System volume set the instant each procedurally-generated phase
     * (preparing/sleeping/waking) starts playing, and periodically
     * re-asserted while it runs — see audio.c's sleep_control_thread(). */
    double sleep_volume_preparing_percent;
    double sleep_volume_sleeping_percent;
    double sleep_volume_waking_percent;

    /* ── Sleep mode: timing ──────────────────────────────────
     * Fade in/out window used at every cut/transition between phases. */
    double sleep_fade_seconds;

    /* ── Window sizing (pixels) ─────────────────────────────── */
    int cfg_w, cfg_h;
    int mini_w, mini_h_base, mini_h_until;
    int mini_w_min, mini_h_min;
    int resize_zone;

    /* ── Tick pacing (milliseconds) ───────────────────────────
     * Redraw intervals, not timer accuracy. */
    int tick_config_ms;
    int tick_config_anim_ms;
    int tick_run_ms;
    int tick_last10_ms;
    int tick_paused_ms;
    int tick_alert_ms;

    /* ── Startup defaults ─────────────────────────────────────
     * CLI flags (-p, -f, --theme) override these for that one run. */
    int default_theme;     /* -1 = random, else 0..N_THEMES-1 */
    int default_pausable;  /* 0/1 */
    int default_fullscreen;/* 0/1 */
} Config;

extern Config g_config;

/* ── Generic config-field accessor table (config.c) ────────────────
 * Backs the Settings overlay (Ctrl+G, draw.c): one entry per tunable
 * Config field, so the GUI/persistence stay in sync with the struct
 * automatically instead of needing per-field UI code. */
typedef enum { CFG_F_DOUBLE, CFG_F_INT, CFG_F_BOOL } CfgFieldKind;

typedef struct {
    const char   *key;       /* stable name, used as the config.conf key */
    const char   *label;     /* human-readable label for the GUI */
    const char   *category;  /* grouping shown as a top-level category   */
    CfgFieldKind  type;
    double        min, max;
    size_t        offset;    /* offsetof(Config, field) */
} CfgField;

extern const CfgField CFG_FIELDS[];
extern const int      N_CFG_FIELDS;
double cfg_field_get(int idx);
void   cfg_field_set(int idx, double v);

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

extern Theme THEMES[N_THEMES];   /* generated at runtime — see generate_themes() */

/* ── Font cache entry ──────────────────────────────────── */

/* style: 0=regular  1=bold  2=italic  3=monospace-bold */
typedef struct {
    int      size;
    int      style;
    XftFont *font;
} FontEntry;

/* ── Audio job ─────────────────────────────────────────── */

#define MAX_AUDIO_OUTPUTS 16
#define MAX_AUDIO_PIDS    32

typedef struct {
    int   fd;
    pid_t pid;
} AudioOutput;

typedef struct {
    int16_t        *samples;
    int             n_samples;
    AudioOutput     outputs[MAX_AUDIO_OUTPUTS];
    int             n_outputs;
    volatile int   *active_flag;
    float           alert_ramp_seconds;
    int             alert_volume_min_pct;  /* system volume at fade start */
    int             alert_volume_max_pct;  /* system volume once fade completes */
} AudioJob;

/* ── Sleep-mode phase plan (built by sleep_build_preview() in audio.c,
 * consumed by both the "/testsleep" CLI printout and the GUI preview
 * overlay in draw.c) ──────────────────────────────────── */

typedef struct {
    double total_seconds;
    double intro_len, wake_len, sleep_end;
} SleepPreviewData;

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
    int  sw_shutdown, sw_alert, sw_fullscreen, sw_pausable, sw_lock, sw_sleep;
} ParseResult;

typedef struct {
    int64_t id;
    char    started_at[24];
    char    ended_at[24];
    int     duration_s;
    int     elapsed_s;
    char    task[MAX_TASK];
    char    mode[12];
    char    status[12];
    int     pausable;
    int     fullscreen;
} DbSession;

typedef struct {
    int64_t id;
    int64_t session_id;
    char    ts[24];
    char    event[48];
    char    detail[256];
} DbEvent;

typedef struct {
    char day_iso[16];
    char day_label[40];
    int  sessions;
    int  completed, stopped;
    long elapsed_s, planned_s;
    int  avg_duration_s;
    int  avg_elapsed_s;
    int  events;
    int  hour_counts[24];
    int  dur_buckets[4];   /* <5m, 5-15m, 15-30m, 30m+ */
} DbDayView;

typedef struct {
    int  sessions_total;
    int  completed, stopped, running;
    int  mode_alert, mode_shutdown;
    long elapsed_total_s;
    long planned_total_s;
    int  events_total;
    int  day_counts[INS_CHART_DAYS];
    char day_labels[INS_CHART_DAYS][4];
    char day_dates[INS_CHART_DAYS][16];
} DbSummary;

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
    int        cfg_buf_w, cfg_buf_h;
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
    int        lock;          /* -l switch: disconnect keyboard+mouse while timer runs */
    int        input_grabbed; /* 1 while the X11 keyboard/pointer grab is active */
    HwLock     hw_lock;       /* kernel-level USB HID deauthorize (see lock.h) */
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
    int        quote_next_idx;
    long       quote_cycle_ts;

    /* Quote morph: each held quote is rasterized once to an off-screen
     * alpha-coverage mask (XftDrawCreateAlpha) and cached here — the morph
     * transform (scale/slide/blur) then samples these as images via
     * XRenderComposite instead of re-rasterizing text every frame. Slot
     * layout is whatever was built most recently; matched by quote index. */
    int        quote_mask_idx[2];
    Pixmap     quote_mask_px[2];
    Picture    quote_mask_pic[2];
    int        quote_mask_w[2], quote_mask_h[2];

    /* Config preview cache (parse once per keystroke, not per frame) */
    ParseResult cfg_preview;
    int         cfg_layout_sig;
    int         cfg_anim_top, cfg_anim_h;
    int         cfg_panel_top, cfg_panel_h;
    long        cfg_anim_last_ms;

    /* Task / until */
    char       task[MAX_TASK];
    char       until_str[MAX_UNTIL];

    /* Theme */
    int        theme_idx;
    int        cfg_focused;   /* 1 = config window has keyboard focus */

    /* Help overlay (Ctrl+H, on the start screen) */
    int        help_open;

    /* Settings overlay (Ctrl+G) — reachable from the setup screen AND
     * (by mapping cfg_win on top of tmr_win) while a timer is running.
     * settings_cat < 0 shows the top-level category list; otherwise it's
     * the index of the category being drilled into. settings_from_timer
     * remembers whether it was opened over a running timer, so closing
     * it knows to withdraw cfg_win again instead of leaving it mapped. */
    int        settings_open;
    int        settings_cat;
    int        settings_from_timer;
    int        settings_drag_field;   /* index into CFG_FIELDS, or -1 */
    int        settings_edit_field;   /* duration field being typed into, or -1 */
    char       settings_edit_buf[32];
    int        settings_edit_len, settings_edit_cursor;
    int        settings_sleep_test_active;
    long       settings_test_started_ms;
    int        cfg_footer_hover;      /* 1-based footer button under pointer, 0 none */

    /* Fullscreen-alert volume slider (item 5) — dragging it live-adjusts
     * whichever volume is currently active (alarm or sleep-waking). */
    int        alert_vol_dragging;

    /* Sleep preview overlay — typed "/testsleep <time>" then Enter */
    int               sleep_preview_open;
    SleepPreviewData *sleep_preview;
    char              sleep_preview_input[MAX_INPUT];

    /* Insights browser (Ctrl+I) */
    int        insights_open;
    int        ins_day_offset;
    DbSummary  ins_summary;
    DbDayView  ins_day;
    DbSession  ins_sessions[INS_MAX_SESSIONS];
    int        ins_n_sessions;
    DbEvent    ins_events[INS_MAX_EVENTS];
    int        ins_n_events;
    int        ins_sel;
    int        ins_sess_scroll;
    int        ins_evt_scroll;
    int64_t    ins_loaded_session_id;

    /* Monitor geometry */
    int        mon_x, mon_y, mon_w, mon_h;

    /* Timer window geometry */
    int        tmr_x, tmr_y, tmr_w, tmr_h;

    /* Mini-window drag */
    int        drag_px, drag_py;
    int        drag_wx, drag_wy;
    int        is_dragging;
    int        drag_grabbed;

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
    pid_t           audio_pids[MAX_AUDIO_PIDS];
    int             n_audio_pids;
    pthread_mutex_t audio_lock;

    /* Sleep mode ("/" switch) — calming ambient while running, gentle
     * wake melody near/after the end, in place of the usual alarm. */
    int             sleep_mode;
    volatile int    sleep_audio_active;

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

/* ── Alarm tempo (shared by audio synthesis and the alert strobe) ──
 * The motif plays faster as the alert goes on.  alarm_speed() is the
 * instantaneous tempo in periods/second; alarm_phase() is its integral,
 * giving a continuous musical phase in "periods" so the audio beat and
 * the visual flashes stay perfectly locked as the tempo changes. */
static inline double alarm_speed(double t) {
    double s0   = 1.0 / g_config.alarm_period;
    double smax = 1.0 / g_config.alarm_period_min;
    double s    = s0 * (1.0 + g_config.alarm_accel * t);
    return s > smax ? smax : s;
}

static inline double alarm_phase(double t) {
    if (t < 0.0) t = 0.0;
    double s0   = 1.0 / g_config.alarm_period;
    double smax = 1.0 / g_config.alarm_period_min;
    double tc   = (smax / s0 - 1.0) / g_config.alarm_accel;   /* time speed hits the cap */
    if (t <= tc)
        return s0 * (t + 0.5 * g_config.alarm_accel * t * t);
    double phase_tc = s0 * (tc + 0.5 * g_config.alarm_accel * tc * tc);
    return phase_tc + smax * (t - tc);
}

/* ── Function prototypes ───────────────────────────────── */

/* timer.c */
void app_log(App *app, const char *level, const char *fmt, ...);
void detect_monitor(App *app, int ptr_x, int ptr_y);

/* config.c */
void config_load(App *app);
void config_save(void);
void get_config_dir(char *out, int out_sz);

/* parse.c */
void format_time(int secs, char *out, size_t sz);
void parse_time_input(const char *raw, ParseResult *out);
int  parse_duration_ms(const char *s, double *ms_out);
void format_duration_ms(double ms, char *out, size_t sz);
void get_preview_switches(const char *input,
                          int *sw_d, int *sw_a, int *sw_f, int *sw_p, int *sw_l,
                          char *until_buf, int until_sz);

/* audio.c */
void play_cue(App *app, const char *kind);
void play_alert_sound(App *app);
void play_sleep_ambient(App *app, double total_seconds);
SleepPreviewData *sleep_build_preview(double total_seconds);
void sleep_print_preview(const SleepPreviewData *d, const char *raw_input);
void play_sleep_audition(App *app, const SleepPreviewData *d);
void play_test_phase_cli(const char *phase_name, int volume_pct, double seconds);
void kill_all_audio(App *app);
void set_system_volume(int pct);  /* sets system volume exactly, up or down — see audio.c */

/* db.c */
void db_init(App *app);
void db_start_session(App *app);
void db_finish_session(App *app, const char *status);
void db_log_event(App *app, const char *event, const char *detail);
void db_insights_reload(App *app);
void db_insights_load_day(App *app, int day_offset);
void db_insights_load_events(App *app, int64_t session_id);

/* draw.c */
void generate_themes(void);
void cfg_refresh_preview(App *app);
void cfg_update_layout(App *app);
void cfg_draw_cyber_bg(App *app, Drawable buf, const Theme *th,
                       int x, int y, int w, int h, double t);
void draw_release_quote_cache(App *app);
void draw_config(App *app);
void draw_config_anim(App *app);
void draw_config_panel(App *app);
void cfg_redraw_after_input(App *app);
void draw_help(App *app);
void draw_sleep_preview(App *app);
int  sleep_preview_handle_click(App *app, int mx, int my);

/* Settings overlay (Ctrl+G) — item 3 */
void draw_settings(App *app);
int  cfg_footer_hit_test(App *app, int mx, int my);
int  timer_settings_btn_hit_test(App *app, int mx, int my);
int  settings_handle_click(App *app, int mx, int my, int is_press);
int  settings_handle_motion(App *app, int mx, int my);
void settings_handle_release(App *app);
void settings_handle_key(App *app, XKeyEvent *ev);
void settings_open_overlay(App *app, int from_timer);
void settings_close_overlay(App *app);

/* Fullscreen alert volume slider — item 5 */
int  alert_vol_handle_click(App *app, int screen_idx, int mx, int my);
int  alert_vol_handle_motion(App *app, int screen_idx, int mx, int my);
void alert_vol_handle_release(App *app);

void ins_enter(App *app);
void ins_leave(App *app);
void draw_insights(App *app);
void ins_handle_key(App *app, XKeyEvent *ev);
int  ins_handle_click(App *app, int mx, int my);
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
