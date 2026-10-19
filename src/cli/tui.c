/* tui.c — terminal UI: settings, insights, about.
 *
 * The GUI window only does three things (input, timer, alarm); everything
 * else lives here, in the terminal:
 *
 *   timerc config            interactive settings   (also: config list|get|set|reset|path)
 *   timerc insights          interactive statistics (plain text when piped)
 *   timerc about             keys, syntax, credits  (plain text when piped)
 *
 * Plain ANSI + termios, no curses dependency. Number keys 1/2/3 switch
 * between the three screens. The terminal is always restored on exit,
 * including on SIGINT/SIGTERM/SIGHUP. */
#include "timer.h"
#include <termios.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <signal.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <poll.h>
#include <unistd.h>
#include <errno.h>
#include <stdarg.h>

/* ── Output buffer ─────────────────────────────────────── */

typedef struct { char *p; size_t n, cap; } Buf;

static void bput(Buf *b, const char *s, size_t n) {
    if (b->n + n + 1 > b->cap) {
        b->cap = (b->n + n + 1) * 2;
        b->p = realloc(b->p, b->cap);
        if (!b->p) exit(1);
    }
    memcpy(b->p + b->n, s, n);
    b->n += n;
    b->p[b->n] = '\0';
}

static void bprintf(Buf *b, const char *fmt, ...) {
    char tmp[1024];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if ((size_t)n >= sizeof(tmp)) n = sizeof(tmp) - 1;
    bput(b, tmp, (size_t)n);
}

/* ── Terminal state ────────────────────────────────────── */

static struct termios g_saved_tio;
static int  g_raw;
static int  g_color = 1;
static int  g_rows = 24, g_cols = 80;
static volatile sig_atomic_t g_quit, g_resized;

#define C_RESET  (g_color ? "\033[0m"        : "")
#define C_BOLD   (g_color ? "\033[1m"        : "")
#define C_DIM    (g_color ? "\033[2m"        : "")
#define C_REV    (g_color ? "\033[7m"        : "")
#define C_ACC    (g_color ? "\033[38;5;141m" : "")
#define C_GREEN  (g_color ? "\033[32m"       : "")
#define C_YELLOW (g_color ? "\033[33m"       : "")
#define C_CYAN   (g_color ? "\033[36m"       : "")
#define C_RED    (g_color ? "\033[31m"       : "")

static void term_restore(void) {
    if (!g_raw) return;
    g_raw = 0;
    const char *s = "\033[?7h\033[?25h\033[?1049l";
    if (write(STDOUT_FILENO, s, strlen(s)) < 0) { /* nothing useful to do */ }
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &g_saved_tio);
}

static void on_signal(int sig) { if (sig == SIGWINCH) g_resized = 1; else g_quit = 1; }

static int term_raw(void) {
    if (tcgetattr(STDIN_FILENO, &g_saved_tio) < 0) return 0;
    struct termios t = g_saved_tio;
    t.c_lflag &= ~(tcflag_t)(ICANON | ECHO | ISIG | IEXTEN);
    t.c_iflag &= ~(tcflag_t)(IXON | ICRNL | BRKINT | INPCK | ISTRIP);
    t.c_cc[VMIN] = 1; t.c_cc[VTIME] = 0;
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &t) < 0) return 0;
    g_raw = 1;
    atexit(term_restore);

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_signal;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGHUP, &sa, NULL);
    sigaction(SIGWINCH, &sa, NULL);

    const char *s = "\033[?1049h\033[?25l\033[?7l";
    if (write(STDOUT_FILENO, s, strlen(s)) < 0) return 0;
    return 1;
}

static void term_size(void) {
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_row > 0 && ws.ws_col > 0) {
        g_rows = ws.ws_row; g_cols = ws.ws_col;
    }
}

typedef enum {
    K_NONE, K_CHAR, K_UP, K_DOWN, K_LEFT, K_RIGHT, K_PGUP, K_PGDN, K_HOME, K_END,
    K_ENTER, K_ESC, K_BACKSPACE, K_TAB, K_BTAB, K_QUIT
} KeyKind;

typedef struct { KeyKind k; int ch; } Key;


static int read_byte(int timeout_ms) {
    struct pollfd p = { STDIN_FILENO, POLLIN, 0 };
    int r = poll(&p, 1, timeout_ms);
    if (r <= 0) return -1;
    unsigned char c;
    if (read(STDIN_FILENO, &c, 1) != 1) return -1;
    return c;
}

/* Waits up to 250ms so the loop can notice SIGWINCH/SIGINT. */
static Key read_key(void) {
    Key k = { K_NONE, 0 };
    int c = read_byte(250);
    if (c < 0) return k;
    if (c == 27) {
        int c2 = read_byte(30);
        if (c2 < 0) { k.k = K_ESC; return k; }
        if (c2 == '[' || c2 == 'O') {
            int c3 = read_byte(30);
            switch (c3) {
            case 'A': k.k = K_UP; return k;
            case 'B': k.k = K_DOWN; return k;
            case 'C': k.k = K_RIGHT; return k;
            case 'D': k.k = K_LEFT; return k;
            case 'H': k.k = K_HOME; return k;
            case 'F': k.k = K_END; return k;
            case 'Z': k.k = K_BTAB; return k;
            case '1': case '7': read_byte(30); k.k = K_HOME; return k;
            case '4': case '8': read_byte(30); k.k = K_END; return k;
            case '5': read_byte(30); k.k = K_PGUP; return k;
            case '6': read_byte(30); k.k = K_PGDN; return k;
            default:  while (read_byte(5) >= 0) {}  /* swallow unknown sequence */
                      return k;
            }
        }
        return k;
    }
    if (c == '\r' || c == '\n') { k.k = K_ENTER; return k; }
    if (c == '\t') { k.k = K_TAB; return k; }
    if (c == 127 || c == 8) { k.k = K_BACKSPACE; return k; }
    if (c == 3 || c == 4) { k.k = K_QUIT; return k; }   /* Ctrl+C / Ctrl+D */
    if (c >= 32) { k.k = K_CHAR; k.ch = c; }
    return k;
}

/* ── Line layout helpers ───────────────────────────────── */

/* Number keys 1..4 map to screens in tab order. */
static int g_row;   /* lines emitted so far in the current frame */

/* Emits one full-width line (clipped to the screen height; the last row
 * is reserved for the status line). */
static void ln(Buf *b, const char *fmt, ...) {
    char tmp[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    if (g_row >= g_rows - 1) return;
    bput(b, tmp, strlen(tmp));
    bput(b, "\033[0m\033[K\r\n", 9);
    g_row++;
}

/* Copies at most `cols` UTF-8 code points of src into dst. */
static void fit(char *dst, size_t dsz, const char *src, int cols) {
    size_t o = 0; int c = 0;
    for (const unsigned char *s = (const unsigned char *)src; *s && c < cols; ) {
        size_t len = 1;
        if (*s >= 0xF0) len = 4; else if (*s >= 0xE0) len = 3; else if (*s >= 0xC0) len = 2;
        if (o + len + 1 > dsz) break;
        for (size_t i = 0; i < len && s[i]; i++) dst[o++] = (char)s[i];
        s += len; c++;
    }
    dst[o] = '\0';
}

static void rule(Buf *b) {
    Buf r = {0};
    for (int i = 0; i < g_cols; i++) bput(&r, "─", 3);
    ln(b, "%s%s%s", C_DIM, r.p ? r.p : "", C_RESET);
    free(r.p);
}

static void fmt_dur(long s, char *out, size_t sz) {
    if (s < 0) { snprintf(out, sz, "—"); return; }
    if (s >= 3600) snprintf(out, sz, "%ldh %02ldm", s / 3600, (s % 3600) / 60);
    else if (s >= 60) snprintf(out, sz, "%ldm %02lds", s / 60, s % 60);
    else snprintf(out, sz, "%lds", s);
}

typedef enum { SCR_CONFIG, SCR_INSIGHTS, SCR_TEST, SCR_ABOUT } TuiScreen;
#define N_SCREENS 4

static TuiScreen key_screen(int ch) { return (TuiScreen)(ch - '1'); }

/* Below this the layouts can't say anything useful; show a hint instead. */
static int tiny(Buf *b) {
    if (g_cols >= 32 && g_rows >= 8) return 0;
    bput(b, "\033[H\033[J", 6);
    bprintf(b, "Terminal too small\r\n(need 32x8, have %dx%d)\r\nq quits", g_cols, g_rows);
    return 1;
}

static void header(Buf *b, TuiScreen cur) {
    static const char *full[]  = { "1 Settings", "2 Insights", "3 Test", "4 About" };
    static const char *small[] = { "1", "2", "3", "4" };
    int compact = g_cols < 52;
    Buf t = {0};
    if (!compact) bprintf(&t, " %sTimerC%s   ", C_ACC, C_RESET);
    else bprintf(&t, " ");
    for (int i = 0; i < N_SCREENS; i++) {
        const char *nm = compact ? small[i] : full[i];
        if (i == (int)cur) bprintf(&t, "%s %s %s ", C_REV, nm, C_RESET);
        else               bprintf(&t, "%s %s %s ", C_DIM, nm, C_RESET);
    }
    ln(b, "%s", t.p);
    free(t.p);
    rule(b);
}

static void status_line(Buf *b, const char *text) {
    char clipped[512];
    fit(clipped, sizeof(clipped), text, g_cols - 2);
    bprintf(b, "\033[%d;1H%s %s%s\033[K", g_rows, C_DIM, clipped, C_RESET);
}

static void flush_frame(Buf *b) {
    size_t off = 0;
    while (off < b->n) {
        ssize_t w = write(STDOUT_FILENO, b->p + off, b->n - off);
        if (w < 0) { if (errno == EINTR) continue; break; }
        off += (size_t)w;
    }
}

/* ══ Settings ══════════════════════════════════════════════ */

static Config g_defaults;

static int cfg_ncats(char cats[][24], int max) {
    int n = 0;
    for (int i = 0; i < N_CFG_FIELDS; i++) {
        int seen = 0;
        for (int j = 0; j < n; j++) if (!strcmp(cats[j], CFG_FIELDS[i].category)) seen = 1;
        if (!seen && n < max) snprintf(cats[n++], 24, "%s", CFG_FIELDS[i].category);
    }
    return n;
}

static void cfg_value_str(const CfgField *f, double v, char *out, size_t sz) {
    if (f->type == CFG_F_BOOL) snprintf(out, sz, "%s", v != 0 ? "on" : "off");
    else if (f->type == CFG_F_INT) snprintf(out, sz, "%d", (int)v);
    else snprintf(out, sz, "%.6g", v);
}

static double cfg_step(const CfgField *f) {
    double range = f->max - f->min;
    if (f->type == CFG_F_BOOL) return 1;
    if (f->type == CFG_F_DOUBLE) return range <= 10 ? 0.1 : range <= 100 ? 1 : range <= 600 ? 5 : 10;
    return range <= 120 ? 1 : range <= 1000 ? 5 : range <= 2000 ? 10 : 50;
}

/* Index of the n-th field belonging to `cat`, or -1. */
static int cfg_nth_in_cat(const char *cat, int n) {
    for (int i = 0, k = 0; i < N_CFG_FIELDS; i++)
        if (!strcmp(CFG_FIELDS[i].category, cat) && k++ == n) return i;
    return -1;
}

static int cfg_count_in_cat(const char *cat) {
    int k = 0;
    for (int i = 0; i < N_CFG_FIELDS; i++) if (!strcmp(CFG_FIELDS[i].category, cat)) k++;
    return k;
}

/* Parses typed input: plain number, or a duration like "1m30s"/"500ms"
 * for fields whose label says (s) or (ms). Returns 1 on success. */
static int cfg_parse_input(const CfgField *f, const char *in, double *out) {
    char *end;
    double v = strtod(in, &end);
    if (end != in && *end == '\0') { *out = v; return 1; }
    double ms;
    if (parse_duration_ms(in, &ms)) {
        *out = strstr(f->label, "(ms)") ? ms : ms / 1000.0;
        return 1;
    }
    if (f->type == CFG_F_BOOL) {
        if (!strcasecmp(in, "on") || !strcasecmp(in, "yes") || !strcasecmp(in, "true")) { *out = 1; return 1; }
        if (!strcasecmp(in, "off") || !strcasecmp(in, "no") || !strcasecmp(in, "false")) { *out = 0; return 1; }
    }
    return 0;
}

static void cfg_apply(int idx, double v) {
    cfg_field_set(idx, v);
    config_normalize();
    config_save();
}

static size_t u8len(const char *s) {
    size_t c = 0;
    for (; *s; s++) if (((unsigned char)*s & 0xC0) != 0x80) c++;
    return c;
}

/* Settings rows that fit: header(2) + tabs + gap, then rule + hint + status. */
static int cfg_avail(void) {
    int a = g_rows - 8;
    return a < 1 ? 1 : a;
}

static void cfg_draw(Buf *b, int cat, int sel, int scroll, const char *msg,
                     const char *edit_buf, int editing, char cats[][24], int ncats) {
    header(b, SCR_CONFIG);

    Buf tabs = {0};
    bprintf(&tabs, " ");
    for (int i = 0; i < ncats; i++) {
        if (i == cat) bprintf(&tabs, "%s%s %s %s ", C_ACC, C_REV, cats[i], C_RESET);
        else          bprintf(&tabs, "%s %s %s ", C_DIM, cats[i], C_RESET);
    }
    ln(b, "%s", tabs.p);
    free(tabs.p);
    ln(b, "%s", "");

    int n = cfg_count_in_cat(cats[cat]);
    int avail = cfg_avail();
    int lw = g_cols >= 60 ? 28 : g_cols >= 44 ? 22 : g_cols - 14;
    if (lw < 8) lw = 8;
    int bar_w = g_cols >= 100 ? 24 : g_cols >= 84 ? 14 : 0;
    int show_def = g_cols >= 64;
    for (int r = scroll; r < n && r < scroll + avail; r++) {
        int idx = cfg_nth_in_cat(cats[cat], r);
        const CfgField *f = &CFG_FIELDS[idx];
        double v = cfg_field_get(idx), d = cfg_field_get_from(&g_defaults, idx);
        char val[32], def[32], label[40];
        cfg_value_str(f, v, val, sizeof(val));
        cfg_value_str(f, d, def, sizeof(def));
        fit(label, sizeof(label), f->label, lw);

        Buf row = {0};
        int selected = (r == sel);
        bprintf(&row, " %s%s %s%*s %10s%s ", selected ? C_ACC : "", selected ? "▸" : " ",
                label, lw - (int)u8len(label), "", val, C_RESET);
        if (bar_w && f->type != CFG_F_BOOL) {
            double t = (v - f->min) / (f->max - f->min);
            if (t < 0) t = 0;
            if (t > 1) t = 1;
            int fill = (int)(t * bar_w + 0.5);
            bprintf(&row, "%s", selected ? C_ACC : C_DIM);
            for (int i = 0; i < bar_w; i++) bput(&row, i < fill ? "█" : "░", 3);
            bprintf(&row, "%s ", C_RESET);
        }
        if (v != d) bprintf(&row, show_def ? "%s● default %s%s" : "%s●%.0s%s", C_YELLOW, def, C_RESET);
        else if (show_def) bprintf(&row, "%sdefault%s", C_DIM, C_RESET);
        ln(b, "%s", row.p);
        free(row.p);
    }
    while (g_row < g_rows - 4) ln(b, "%s", "");

    rule(b);
    if (editing) {
        int idx = cfg_nth_in_cat(cats[cat], sel);
        const CfgField *f = &CFG_FIELDS[idx];
        char lo[32], hi[32];
        cfg_value_str(f, f->min, lo, sizeof(lo));
        cfg_value_str(f, f->max, hi, sizeof(hi));
        ln(b, " %sNew value%s (%s … %s): %s%s█", C_ACC, C_RESET, lo, hi, C_BOLD, edit_buf);
    } else {
        ln(b, g_cols >= 84 ? " %s←/→ adjust   ↑/↓ select   Tab category   Enter type value   r reset   R reset all   q quit%s"
            : g_cols >= 52 ? " %s←/→ adjust  ↑/↓ select  Tab cat  Enter edit  r reset  q quit%s"
            : " %s←→ ↑↓ Tab Enter r q%s", C_DIM, C_RESET);
    }
    status_line(b, msg && msg[0] ? msg : "Changes are saved immediately and apply to the next timer you start.");
}

static void screen_config(TuiScreen *next) {
    char cats[16][24];
    int ncats = cfg_ncats(cats, 16);
    int cat = 0, sel = 0, scroll = 0, editing = 0;
    char edit[32] = "", msg[160] = "";

    for (;;) {
        if (g_quit) { *next = (TuiScreen)-1; return; }
        if (g_resized) { g_resized = 0; term_size(); }
        int n = cfg_count_in_cat(cats[cat]);
        if (sel >= n) sel = n - 1;
        if (sel < 0) sel = 0;
        int avail = cfg_avail();
        if (sel < scroll) scroll = sel;
        if (sel >= scroll + avail) scroll = sel - avail + 1;

        Buf b = {0};
        bput(&b, "\033[H", 3);
        g_row = 0;
        if (!tiny(&b)) cfg_draw(&b, cat, sel, scroll, msg, edit, editing, cats, ncats);
        flush_frame(&b);
        free(b.p);

        Key k = read_key();
        if (k.k == K_NONE) continue;
        if (k.k == K_QUIT) { *next = (TuiScreen)-1; return; }
        int idx = cfg_nth_in_cat(cats[cat], sel);
        const CfgField *f = idx >= 0 ? &CFG_FIELDS[idx] : NULL;
        msg[0] = '\0';

        if (editing) {
            size_t len = strlen(edit);
            if (k.k == K_ESC) editing = 0;
            else if (k.k == K_BACKSPACE) { if (len) edit[len - 1] = '\0'; }
            else if (k.k == K_CHAR && len < sizeof(edit) - 1) { edit[len] = (char)k.ch; edit[len + 1] = '\0'; }
            else if (k.k == K_ENTER) {
                double v;
                if (f && cfg_parse_input(f, edit, &v)) {
                    cfg_apply(idx, v);
                    char s[32]; cfg_value_str(f, cfg_field_get(idx), s, sizeof(s));
                    snprintf(msg, sizeof(msg), "%s = %s (saved)", f->key, s);
                } else snprintf(msg, sizeof(msg), "Not a valid value: '%s'", edit);
                editing = 0;
            }
            continue;
        }

        switch (k.k) {
        case K_UP:   if (sel > 0) sel--; break;
        case K_DOWN: if (sel < n - 1) sel++; break;
        case K_HOME: sel = 0; break;
        case K_END:  sel = n - 1; break;
        case K_TAB:  cat = (cat + 1) % ncats; sel = scroll = 0; break;
        case K_BTAB: cat = (cat + ncats - 1) % ncats; sel = scroll = 0; break;
        case K_LEFT: case K_RIGHT: case K_PGUP: case K_PGDN: {
            if (!f) break;
            double step = cfg_step(f) * ((k.k == K_PGUP || k.k == K_PGDN) ? 10 : 1);
            double dir = (k.k == K_LEFT || k.k == K_PGDN) ? -1 : 1;
            if (f->type == CFG_F_BOOL) cfg_apply(idx, cfg_field_get(idx) != 0 ? 0 : 1);
            else cfg_apply(idx, cfg_field_get(idx) + dir * step);
            break;
        }
        case K_ENTER:
            if (f && f->type == CFG_F_BOOL) cfg_apply(idx, cfg_field_get(idx) != 0 ? 0 : 1);
            else if (f) { editing = 1; edit[0] = '\0'; }
            break;
        case K_ESC: *next = (TuiScreen)-1; return;
        case K_CHAR:
            if (k.ch == 'q') { *next = (TuiScreen)-1; return; }
            if (k.ch == 'k') { if (sel > 0) sel--; }
            else if (k.ch == 'j') { if (sel < n - 1) sel++; }
            else if (k.ch == 'h' && f) cfg_apply(idx, cfg_field_get(idx) - cfg_step(f));
            else if (k.ch == 'l' && f) cfg_apply(idx, cfg_field_get(idx) + cfg_step(f));
            else if (k.ch == '-' && f) cfg_apply(idx, cfg_field_get(idx) - cfg_step(f));
            else if ((k.ch == '+' || k.ch == '=') && f) cfg_apply(idx, cfg_field_get(idx) + cfg_step(f));
            else if (k.ch == ' ' && f && f->type == CFG_F_BOOL) cfg_apply(idx, cfg_field_get(idx) != 0 ? 0 : 1);
            else if (k.ch == 'r' && f) { cfg_apply(idx, cfg_field_get_from(&g_defaults, idx)); snprintf(msg, sizeof(msg), "%s reset to default", f->key); }
            else if (k.ch == 'R') {
                for (int i = 0; i < N_CFG_FIELDS; i++) cfg_field_set(i, cfg_field_get_from(&g_defaults, i));
                config_normalize(); config_save();
                snprintf(msg, sizeof(msg), "All settings reset to defaults");
            }
            else if (k.ch >= '1' && k.ch <= '4') { *next = key_screen(k.ch); if (*next != SCR_CONFIG) return; }
            break;
        default: break;
        }
    }
}

/* ══ Insights ══════════════════════════════════════════════ */

static App g_ins_app;   /* only db + ins_* fields are used */

static void day_dates_range(void) {}

static void ins_hour_cells(Buf *b, const int *h) {
    static const char *shade[] = { "·", "░", "▒", "▓", "█" };
    int mx = 0;
    for (int i = 0; i < 24; i++) if (h[i] > mx) mx = h[i];
    for (int i = 0; i < 24; i++) {
        int lvl = h[i] == 0 ? 0 : mx <= 1 ? 4 : 1 + (h[i] * 3) / mx;
        if (lvl > 4) lvl = 4;
        bprintf(b, "%s%s%s", h[i] ? C_ACC : C_DIM, shade[lvl], C_RESET);
    }
}

static void ins_draw(Buf *b, int ev_scroll) {
    App *a = &g_ins_app;
    DbSummary *s = &a->ins_summary;
    DbDayView *d = &a->ins_day;
    char t1[32], t2[32];

    header(b, SCR_INSIGHTS);

    fmt_dur(s->elapsed_total_s, t1, sizeof(t1));
    fmt_dur(s->planned_total_s, t2, sizeof(t2));
    ln(b, " %sAll time%s  %d sessions · %s%d completed%s · %s%d stopped%s · focused %s%s%s of %s planned",
       C_BOLD, C_RESET, s->sessions_total, C_GREEN, s->completed, C_RESET,
       C_YELLOW, s->stopped, C_RESET, C_ACC, t1, C_RESET, t2);

    /* 14-day sparkline */
    static const char *bars[] = { "▁", "▂", "▃", "▄", "▅", "▆", "▇", "█" };
    int nd = (g_cols - 16) / 3; if (nd > INS_CHART_DAYS) nd = INS_CHART_DAYS; if (nd < 3) nd = 3;
    int d0 = INS_CHART_DAYS - nd;
    int mx = 1;
    for (int i = d0; i < INS_CHART_DAYS; i++) if (s->day_counts[i] > mx) mx = s->day_counts[i];
    Buf spark = {0}, lab = {0};
    bprintf(&spark, " %sLast %d days%s  ", C_BOLD, nd, C_RESET);
    bprintf(&lab, "              ");
    for (int i = d0; i < INS_CHART_DAYS; i++) {
        int c = s->day_counts[i];
        int lvl = c == 0 ? -1 : (c * 7) / mx;
        if (lvl < 0) bprintf(&spark, "%s·%s  ", C_DIM, C_RESET);
        else bprintf(&spark, "%s%s%s  ", C_ACC, bars[lvl], C_RESET);
        bprintf(&lab, "%s%.2s%s ", C_DIM, s->day_labels[i], C_RESET);
    }
    ln(b, "%s", spark.p);
    ln(b, "%s", lab.p);
    free(spark.p); free(lab.p);
    ln(b, "%s", "");

    char dl[48]; fit(dl, sizeof(dl), d->day_label, 40);
    fmt_dur(d->elapsed_s, t1, sizeof(t1));
    fmt_dur(d->avg_elapsed_s, t2, sizeof(t2));
    ln(b, " %s%s%s  %d sessions · %d completed · %d stopped · focused %s · avg %s",
       C_ACC, dl, C_RESET, d->sessions, d->completed, d->stopped, t1, t2);
    Buf hours = {0};
    bprintf(&hours, " %sHours%s      ", C_DIM, C_RESET);
    ins_hour_cells(&hours, d->hour_counts);
    bprintf(&hours, "  %s0h → 23h%s", C_DIM, C_RESET);
    ln(b, "%s", hours.p);
    free(hours.p);
    rule(b);

    int rem = g_rows - 1 - g_row - 3;      /* titles + footer */
    if (rem < 4) rem = 4;
    int sess_rows = a->ins_n_sessions == 0 ? 1 : rem * 55 / 100;
    if (sess_rows < 2) sess_rows = 2;
    int ev_rows = rem - sess_rows; if (ev_rows < 2) ev_rows = 2;

    ln(b, " %sSessions%s %s(↑/↓ select)%s", C_BOLD, C_RESET, C_DIM, C_RESET);
    if (a->ins_n_sessions == 0) ln(b, " %sNo sessions on this day.%s", C_DIM, C_RESET);
    int first = a->ins_sel - sess_rows + 1; if (first < 0) first = 0;
    for (int i = first; i < a->ins_n_sessions && i < first + sess_rows; i++) {
        DbSession *r = &a->ins_sessions[i];
        char pl[32], el[32], task[64], tm[8] = "";
        fmt_dur(r->duration_s, pl, sizeof(pl));
        fmt_dur(r->elapsed_s, el, sizeof(el));
        fit(task, sizeof(task), r->task, g_cols > 90 ? 40 : 20);
        if (strlen(r->started_at) >= 16) { memcpy(tm, r->started_at + 11, 5); tm[5] = 0; }
        const char *sc = !strcmp(r->status, "completed") ? C_GREEN : !strcmp(r->status, "running") ? C_CYAN : C_YELLOW;
        ln(b, " %s%s %s  %8s %8s  %s%-10s%s %s%s",
           i == a->ins_sel ? C_REV : "", i == a->ins_sel ? "▸" : " ", tm, pl, el, sc, r->status, C_RESET,
           task, C_RESET);
    }
    while (g_row < g_rows - 1 - ev_rows - 3) ln(b, "%s", "");

    ln(b, " %sEvents%s %s(PgUp/PgDn scroll)%s", C_BOLD, C_RESET, C_DIM, C_RESET);
    int maxscroll = a->ins_n_events - ev_rows; if (maxscroll < 0) maxscroll = 0;
    if (ev_scroll > maxscroll) ev_scroll = maxscroll;
    for (int i = ev_scroll; i < a->ins_n_events && i < ev_scroll + ev_rows; i++) {
        DbEvent *e = &a->ins_events[i];
        char det[96], tm[10] = "";
        fit(det, sizeof(det), e->detail, g_cols - 36);
        if (strlen(e->ts) >= 19) { memcpy(tm, e->ts + 11, 8); tm[8] = 0; }
        ln(b, "  %s%s%s  %-20s %s%s%s", C_DIM, tm, C_RESET, e->event, C_DIM, det, C_RESET);
    }
    day_dates_range();
}

static void screen_insights(TuiScreen *next) {
    App *a = &g_ins_app;
    int ev_scroll = 0;
    char msg[128] = "";

    char dir[MAX_PATH_LEN];
    get_config_dir(dir, sizeof(dir));
    mkdir(dir, 0755);
    db_init(a);
    if (!a->db) { snprintf(msg, sizeof(msg), "Could not open the session database."); }
    else db_insights_load_day(a, 0);

    for (;;) {
        if (g_quit) { *next = (TuiScreen)-1; return; }
        if (g_resized) { g_resized = 0; term_size(); }

        Buf b = {0};
        bput(&b, "\033[H", 3);
        g_row = 0;
        if (!tiny(&b)) ins_draw(&b, ev_scroll);
        while (g_row < g_rows - 1) ln(&b, "%s", "");
        status_line(&b, msg[0] ? msg : "←/→ day   t today   ↑/↓ session   PgUp/PgDn events   r reload   q quit");
        flush_frame(&b);
        free(b.p);

        Key k = read_key();
        if (k.k == K_NONE) continue;
        msg[0] = '\0';
        if (k.k == K_QUIT || k.k == K_ESC) { *next = (TuiScreen)-1; break; }
        int changed_day = 0;
        switch (k.k) {
        case K_LEFT:  a->ins_day_offset++; changed_day = 1; break;
        case K_RIGHT: if (a->ins_day_offset > 0) { a->ins_day_offset--; changed_day = 1; } break;
        case K_UP:    if (a->ins_sel > 0) { a->ins_sel--; ev_scroll = 0;
                          db_insights_load_events(a, a->ins_sessions[a->ins_sel].id); } break;
        case K_DOWN:  if (a->ins_sel < a->ins_n_sessions - 1) { a->ins_sel++; ev_scroll = 0;
                          db_insights_load_events(a, a->ins_sessions[a->ins_sel].id); } break;
        case K_PGUP:  ev_scroll -= 5; if (ev_scroll < 0) ev_scroll = 0; break;
        case K_PGDN:  ev_scroll += 5; break;
        case K_CHAR:
            if (k.ch == 'q') { *next = (TuiScreen)-1; goto done; }
            if (k.ch == 'h') { a->ins_day_offset++; changed_day = 1; }
            else if (k.ch == 'l' && a->ins_day_offset > 0) { a->ins_day_offset--; changed_day = 1; }
            else if (k.ch == 't') { a->ins_day_offset = 0; changed_day = 1; }
            else if (k.ch == 'r') changed_day = 1;
            else if (k.ch == 'j' && a->ins_sel < a->ins_n_sessions - 1) { a->ins_sel++; ev_scroll = 0;
                db_insights_load_events(a, a->ins_sessions[a->ins_sel].id); }
            else if (k.ch == 'k' && a->ins_sel > 0) { a->ins_sel--; ev_scroll = 0;
                db_insights_load_events(a, a->ins_sessions[a->ins_sel].id); }
            else if (k.ch >= '1' && k.ch <= '4') { *next = key_screen(k.ch); if (*next != SCR_INSIGHTS) goto done; }
            break;
        default: break;
        }
        if (changed_day) { a->ins_sel = 0; ev_scroll = 0; db_insights_load_day(a, a->ins_day_offset); }
    }
done:
    if (a->db) { sqlite3_close(a->db); a->db = NULL; }
}

/* ══ About ═════════════════════════════════════════════════ */

/* Lines starting with '#' are section titles. */
static const char *ABOUT[] = {
    "# TimerC",
    "A fast, native countdown timer for Linux / X11, written in C.",
    "Author   Mohamad Almousli — https://al-mo.de",
    "Source   https://github.com/mkalmousli/timerc",
    "License  MIT",
    "",
    "# Commands",
    "timerc 25m focus      run in the foreground; returns when the timer ends",
    "timerc-bg 25m focus   same, detached into the background",
    "timerc                open the setup window",
    "timerc config         settings (this UI) — also: config list | get KEY | set KEY VALUE | reset",
    "timerc insights       statistics and session log",
    "timerc about          this page",
    "timerc /testsleep 8h  print the sleep-mode plan",
    "",
    "# Typing a timer",
    "25m task    duration + optional task name (ms/s/m/h, 1h30m, 45, 30:00)",
    "u15:30      run until the clock reads 15:30",
    "u:30        run until minute 30 of this (or the next) hour",
    "d 25m       shut the machine down when it ends",
    "a 25m       fullscreen alert when it ends",
    "f 25m       start fullscreen",
    "p 25m       allow pausing with Space",
    "l 25m       lock keyboard and mouse (needs the lock helper)",
    "/8h         sleep mode: ambient audio that fades in, then wakes you",
    "Letters combine: fp25m, dp30m, /fa30m",
    "",
    "# Keys in the setup window",
    "Enter       start the timer",
    "Tab         new theme",
    "Esc         quit",
    "Ctrl+A      cursor to end of input",
    "",
    "# While a timer runs",
    "Space or left click   pause / resume (with p)",
    "Right click           toggle mini / fullscreen",
    "Middle click          stop",
    "Drag                  move the mini window; drag the corner to resize",
    NULL
};

static void about_print_plain(void) {
    for (int i = 0; ABOUT[i]; i++) {
        const char *l = ABOUT[i];
        if (l[0] == '#') printf("%s%s%s\n", C_BOLD, l + 2, C_RESET);
        else printf("%s\n", l);
    }
    char dir[MAX_PATH_LEN];
    get_config_dir(dir, sizeof(dir));
    printf("\nVersion %s · data in %s\n", TIMERC_VERSION, dir);
}

/* Word-wraps ABOUT to `width` columns into rows[]; continuation lines are
 * indented. Section titles are flagged so they can be styled. */
typedef struct { char t[512]; int title; } AboutRow;
static AboutRow g_arow[256];

static int about_wrap(int width) {
    int n = 0;
    char dir[MAX_PATH_LEN];
    get_config_dir(dir, sizeof(dir));
    char tail[3][512];
    snprintf(tail[0], sizeof(tail[0]), "%s", "");
    snprintf(tail[1], sizeof(tail[1]), "Version %s", TIMERC_VERSION);
    snprintf(tail[2], sizeof(tail[2]), "Data: %.400s", dir);
    for (int i = 0; ABOUT[i]; i++) {
        const char *l = ABOUT[i];
        int is_title = 0;
        if (l[0] == '#') { is_title = 1; l += 2; }
        if (width < 8) width = 8;
        const char *p = l;
        int first = 1;
        do {
            if (n >= 255) return n;
            int w = width - (first ? 0 : 4);
            /* take up to w columns, breaking at the last space */
            const char *q = p; int cols = 0; const char *brk = NULL;
            while (*q && cols < w) {
                if (*q == ' ') brk = q;
                size_t len = 1; unsigned char c = (unsigned char)*q;
                if (c >= 0xF0) len = 4; else if (c >= 0xE0) len = 3; else if (c >= 0xC0) len = 2;
                q += len; cols++;
            }
            const char *end = q;
            if (*q && brk && brk > p) end = brk;
            size_t len = (size_t)(end - p);
            if (len > sizeof(g_arow[n].t) - 1) len = sizeof(g_arow[n].t) - 1;
            snprintf(g_arow[n].t, sizeof(g_arow[n].t), "%s%.*s", first ? "" : "    ", (int)len, p);
            g_arow[n].title = is_title;
            n++;
            p = end;
            while (*p == ' ') p++;
            first = 0;
        } while (*p);
    }
    for (int i = 0; i < 3 && n < 256; i++, n++) {
        snprintf(g_arow[n].t, sizeof(g_arow[n].t), "%.500s", tail[i]);
        g_arow[n].title = -1;   /* dim footer */
    }
    return n;
}

static void screen_about(TuiScreen *next) {
    int scroll = 0;
    for (;;) {
        if (g_quit) { *next = (TuiScreen)-1; return; }
        if (g_resized) { g_resized = 0; term_size(); }
        int total = about_wrap(g_cols - 4);
        int view = g_rows - 1 - 2; if (view < 1) view = 1;
        int maxs = total - view; if (maxs < 0) maxs = 0;
        if (scroll > maxs) scroll = maxs;
        if (scroll < 0) scroll = 0;

        Buf b = {0};
        bput(&b, "\033[H", 3);
        g_row = 0;
        if (!tiny(&b)) {
            header(&b, SCR_ABOUT);
            for (int i = scroll; i < total && i < scroll + view; i++) {
                AboutRow *r = &g_arow[i];
                if (r->title == 1) ln(&b, " %s%s%s%s", C_BOLD, C_ACC, r->t, C_RESET);
                else if (r->title == -1) ln(&b, "  %s%s%s", C_DIM, r->t, C_RESET);
                else ln(&b, "  %s", r->t);
            }
            while (g_row < g_rows - 1) ln(&b, "%s", "");
            status_line(&b, "↑/↓ scroll   1-4 switch screen   q quit");
        }
        flush_frame(&b);
        free(b.p);

        Key k = read_key();
        if (k.k == K_NONE) continue;
        switch (k.k) {
        case K_QUIT: case K_ESC: *next = (TuiScreen)-1; return;
        case K_UP: scroll--; break;
        case K_DOWN: scroll++; break;
        case K_PGUP: scroll -= view; break;
        case K_PGDN: scroll += view; break;
        case K_HOME: scroll = 0; break;
        case K_END: scroll = maxs; break;
        case K_CHAR:
            if (k.ch == 'q') { *next = (TuiScreen)-1; return; }
            if (k.ch == 'k') scroll--;
            else if (k.ch == 'j') scroll++;
            else if (k.ch >= '1' && k.ch <= '4') { *next = key_screen(k.ch); if (*next != SCR_ABOUT) return; }
            break;
        default: break;
        }
    }
}

/* ══ Test ══════════════════════════════════════════════════
 * Auditions things without starting a real timer. Each test re-runs this
 * binary as a child (its own process group) with output captured, so a
 * keypress can always stop it cleanly. */

typedef struct { const char *label, *desc; const char *argv[4]; int needs_display; int ask_duration; } TestItem;

static const TestItem TESTS[] = {
    { "Alarm sound",           "Plays the alarm at its start volume (30 s)",          { "test", "alarm", NULL },     0, 0 },
    { "Sleep: preparing",      "Rain ambience that fades in (30 s)",                  { "test", "preparing", NULL }, 0, 0 },
    { "Sleep: sleeping",       "Low drone loop (30 s)",                               { "test", "sleeping", NULL },  0, 0 },
    { "Sleep: waking",         "Gentle chime and pad (30 s)",                         { "test", "waking", NULL },    0, 0 },
    { "Sleep: all phases",     "Preparing, sleeping, then waking in a row",           { "test", "all", NULL },       0, 0 },
    { "Sleep-mode plan",       "Show the phase plan for a duration you type",         { "/testsleep", NULL, NULL },  0, 1 },
    { "Fullscreen alert",      "Runs a 2 s timer, then shows the alert (needs X11)",  { "2s", "Test alert", NULL },  1, 0 },
};
#define N_TESTS ((int)(sizeof(TESTS) / sizeof(TESTS[0])))

typedef struct { pid_t pid; int fd; Buf out; int running; int status; } TestRun;

static void test_stop(TestRun *r) {
    if (r->running) {
        kill(-r->pid, SIGTERM);
        usleep(100000);
        kill(-r->pid, SIGKILL);
        waitpid(r->pid, &r->status, 0);
        r->running = 0;
    }
    if (r->fd >= 0) { close(r->fd); r->fd = -1; }
}

static int test_start(TestRun *r, const TestItem *t, const char *arg) {
    int fds[2];
    if (pipe(fds) < 0) return 0;
    char exe[MAX_PATH_LEN];
    ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    if (n <= 0) { close(fds[0]); close(fds[1]); return 0; }
    exe[n] = '\0';
    pid_t pid = fork();
    if (pid < 0) { close(fds[0]); close(fds[1]); return 0; }
    if (pid == 0) {
        setsid();
        int dn = open("/dev/null", O_RDONLY);
        if (dn >= 0) dup2(dn, STDIN_FILENO);
        dup2(fds[1], STDOUT_FILENO);
        dup2(fds[1], STDERR_FILENO);
        close(fds[0]); close(fds[1]);
        const char *av[8]; int ac = 0;
        av[ac++] = "timerc";
        for (int i = 0; i < 4 && t->argv[i]; i++) av[ac++] = t->argv[i];
        if (t->ask_duration) av[ac++] = arg;
        av[ac] = NULL;
        execv(exe, (char *const *)av);
        _exit(127);
    }
    close(fds[1]);
    fcntl(fds[0], F_SETFL, O_NONBLOCK);
    free(r->out.p); memset(&r->out, 0, sizeof(r->out));
    r->pid = pid; r->fd = fds[0]; r->running = 1; r->status = 0;
    return 1;
}

static void test_pump(TestRun *r) {
    if (r->fd >= 0) {
        char tmp[2048]; ssize_t n;
        while ((n = read(r->fd, tmp, sizeof(tmp))) > 0) {
            if (r->out.n < 64 * 1024) bput(&r->out, tmp, (size_t)n);
        }
    }
    if (r->running) {
        int st;
        if (waitpid(r->pid, &st, WNOHANG) == r->pid) { r->running = 0; r->status = st; }
    }
}

static void test_draw(Buf *b, int sel, int editing, const char *edit, TestRun *run, int scroll, const char *msg) {
    header(b, SCR_TEST);
    int list_rows = N_TESTS;
    int out_rows = g_rows - 1 - g_row - list_rows - 2;
    int compact = out_rows < 4;           /* short terminal: hide the output pane while idle */
    if (compact && !run->out.n && !run->running) { out_rows = 0; }
    if (g_rows - g_row - 2 < list_rows) list_rows = g_rows - g_row - 2;
    if (list_rows < 1) list_rows = 1;
    int first = sel - list_rows + 1; if (first < 0) first = 0;
    for (int i = first; i < N_TESTS && i < first + list_rows; i++) {
        char lab[48], d[160];
        fit(lab, sizeof(lab), TESTS[i].label, 24);
        int room = g_cols - 32;
        fit(d, sizeof(d), TESTS[i].desc, room > 0 ? room : 0);
        int on = (i == sel);
        ln(b, " %s%s %-24s%s %s%s%s", on ? C_ACC : "", on ? "▸" : " ", lab, C_RESET, C_DIM, d, C_RESET);
    }
    rule(b);
    if (editing) {
        ln(b, " %sDuration%s (e.g. 8h, u6:30): %s%s█", C_ACC, C_RESET, C_BOLD, edit);
    } else if (run->running) {
        ln(b, " %s● running%s — press any key to stop", C_GREEN, C_RESET);
    } else if (run->out.n || run->pid) {
        int ok = WIFEXITED(run->status) && WEXITSTATUS(run->status) == 0;
        ln(b, " %s%s%s", ok ? C_GREEN : C_YELLOW, ok ? "finished" : "stopped / failed", C_RESET);
    } else ln(b, " %sPick a test and press Enter%s", C_DIM, C_RESET);

    /* Output pane: split captured text into lines, show the tail (or scrolled view). */
    int rows = g_rows - 1 - g_row; if (rows < 0) rows = 0;
    char *lines[512]; int nl = 0;
    if (run->out.p) {
        char *p = run->out.p;
        while (*p && nl < 511) {
            lines[nl++] = p;
            char *e = strchr(p, '\n');
            if (!e) break;
            *e = '\0';
            p = e + 1;
        }
    }
    int maxs = nl > rows ? nl - rows : 0;
    int start = run->running ? maxs : (scroll > maxs ? maxs : scroll);
    for (int i = start; i < nl && i < start + rows; i++) {
        char clip[512], *c = lines[i];
        for (char *q = c; *q; q++) if (*q == '\r' || *q == '\033') *q = ' ';
        fit(clip, sizeof(clip), c, g_cols - 3);
        ln(b, "  %s", clip);
    }
    (void)out_rows;
    while (g_row < g_rows - 1) ln(b, "%s", "");
    status_line(b, msg && msg[0] ? msg : "↑/↓ select   Enter run   PgUp/PgDn output   1-4 switch screen   q quit");
}

static void screen_test(TuiScreen *next) {
    int sel = 0, editing = 0, scroll = 0;
    char edit[32] = "", msg[128] = "";
    TestRun run; memset(&run, 0, sizeof(run)); run.fd = -1;

    for (;;) {
        if (g_quit) { test_stop(&run); *next = (TuiScreen)-1; goto out; }
        if (g_resized) { g_resized = 0; term_size(); }
        test_pump(&run);

        Buf b = {0};
        bput(&b, "\033[H", 3);
        g_row = 0;
        if (!tiny(&b)) test_draw(&b, sel, editing, edit, &run, scroll, msg);
        flush_frame(&b);
        free(b.p);
        /* test_draw splits the output in place; undo so appends stay valid. */
        if (run.out.p) for (size_t i = 0; i < run.out.n; i++) if (run.out.p[i] == '\0') run.out.p[i] = '\n';

        struct pollfd pf[2] = { { STDIN_FILENO, POLLIN, 0 }, { run.fd, POLLIN, 0 } };
        int ready = poll(pf, run.fd >= 0 ? 2 : 1, 200);
        if (ready <= 0 || !(pf[0].revents & POLLIN)) continue;
        Key k = read_key();
        if (k.k == K_NONE) continue;
        if (k.k == K_QUIT) { test_stop(&run); *next = (TuiScreen)-1; goto out; }
        msg[0] = '\0';

        if (run.running) { test_stop(&run); snprintf(msg, sizeof(msg), "Stopped"); continue; }

        if (editing) {
            size_t len = strlen(edit);
            if (k.k == K_ESC) editing = 0;
            else if (k.k == K_BACKSPACE) { if (len) edit[len - 1] = '\0'; }
            else if (k.k == K_CHAR && len < sizeof(edit) - 1) { edit[len] = (char)k.ch; edit[len + 1] = '\0'; }
            else if (k.k == K_ENTER) {
                editing = 0;
                if (!edit[0]) snprintf(edit, sizeof(edit), "8h");
                if (!test_start(&run, &TESTS[sel], edit)) snprintf(msg, sizeof(msg), "Could not start the test");
                scroll = 0;
            }
            continue;
        }

        switch (k.k) {
        case K_UP: if (sel > 0) sel--; break;
        case K_DOWN: if (sel < N_TESTS - 1) sel++; break;
        case K_PGUP: scroll -= 5; if (scroll < 0) scroll = 0; break;
        case K_PGDN: scroll += 5; break;
        case K_ESC: *next = (TuiScreen)-1; goto out;
        case K_ENTER:
            if (TESTS[sel].needs_display && !getenv("DISPLAY") && !getenv("WAYLAND_DISPLAY")) {
                snprintf(msg, sizeof(msg), "No graphical display found (DISPLAY is not set)");
                break;
            }
            if (TESTS[sel].ask_duration) { editing = 1; snprintf(edit, sizeof(edit), "8h"); }
            else { test_start(&run, &TESTS[sel], NULL); scroll = 0; }
            break;
        case K_CHAR:
            if (k.ch == 'q') { *next = (TuiScreen)-1; goto out; }
            if (k.ch == 'k' && sel > 0) sel--;
            else if (k.ch == 'j' && sel < N_TESTS - 1) sel++;
            else if (k.ch >= '1' && k.ch <= '4') { *next = key_screen(k.ch); if (*next != SCR_TEST) goto out; }
            break;
        default: break;
        }
    }
out:
    test_stop(&run);
    free(run.out.p);
}

/* ══ Plain-text (non-interactive) output ═══════════════════ */

static void insights_print_plain(void) {
    char dir[MAX_PATH_LEN];
    get_config_dir(dir, sizeof(dir));
    mkdir(dir, 0755);
    App *a = &g_ins_app;
    db_init(a);
    if (!a->db) { fprintf(stderr, "timerc: cannot open the session database\n"); return; }
    db_insights_load_day(a, 0);
    DbSummary *s = &a->ins_summary;
    char t1[32], t2[32];
    fmt_dur(s->elapsed_total_s, t1, sizeof(t1));
    fmt_dur(s->planned_total_s, t2, sizeof(t2));
    printf("All time: %d sessions, %d completed, %d stopped, focused %s of %s planned\n",
           s->sessions_total, s->completed, s->stopped, t1, t2);
    printf("Last %d days:", INS_CHART_DAYS);
    for (int i = 0; i < INS_CHART_DAYS; i++) printf(" %d", s->day_counts[i]);
    printf("\n\nToday (%d sessions):\n", a->ins_n_sessions);
    for (int i = 0; i < a->ins_n_sessions; i++) {
        DbSession *r = &a->ins_sessions[i];
        char pl[32], el[32];
        fmt_dur(r->duration_s, pl, sizeof(pl));
        fmt_dur(r->elapsed_s, el, sizeof(el));
        printf("  %.5s  planned %-8s elapsed %-8s %-10s %s\n",
               strlen(r->started_at) >= 16 ? r->started_at + 11 : "", pl, el, r->status, r->task);
    }
    sqlite3_close(a->db);
}

static int config_command(int argc, char **argv) {
    const char *sub = argc > 1 ? argv[1] : "";
    if (!strcmp(sub, "path")) {
        char dir[MAX_PATH_LEN];
        get_config_dir(dir, sizeof(dir));
        printf("%s/config.conf\n", dir);
        return 0;
    }
    if (!strcmp(sub, "list")) {
        for (int i = 0; i < N_CFG_FIELDS; i++) {
            char v[32], d[32];
            cfg_value_str(&CFG_FIELDS[i], cfg_field_get(i), v, sizeof(v));
            cfg_value_str(&CFG_FIELDS[i], cfg_field_get_from(&g_defaults, i), d, sizeof(d));
            printf("%-34s = %-8s (default %s)\n", CFG_FIELDS[i].key, v, d);
        }
        return 0;
    }
    if (!strcmp(sub, "get") || !strcmp(sub, "set") || !strcmp(sub, "reset")) {
        if (!strcmp(sub, "reset") && argc == 2) {
            for (int i = 0; i < N_CFG_FIELDS; i++) cfg_field_set(i, cfg_field_get_from(&g_defaults, i));
            config_normalize(); config_save();
            printf("All settings reset to defaults\n");
            return 0;
        }
        if (argc < 3 || (!strcmp(sub, "set") && argc < 4)) {
            fprintf(stderr, "usage: timerc config get KEY | set KEY VALUE | reset [KEY]\n");
            return 2;
        }
        int idx = -1;
        for (int i = 0; i < N_CFG_FIELDS; i++) if (!strcmp(CFG_FIELDS[i].key, argv[2])) idx = i;
        if (idx < 0) { fprintf(stderr, "timerc: unknown setting '%s' (see: timerc config list)\n", argv[2]); return 1; }
        char v[32];
        if (!strcmp(sub, "get")) {
            cfg_value_str(&CFG_FIELDS[idx], cfg_field_get(idx), v, sizeof(v));
            printf("%s\n", v);
            return 0;
        }
        double nv;
        if (!strcmp(sub, "reset")) nv = cfg_field_get_from(&g_defaults, idx);
        else if (!cfg_parse_input(&CFG_FIELDS[idx], argv[3], &nv)) {
            fprintf(stderr, "timerc: invalid value '%s'\n", argv[3]);
            return 1;
        }
        cfg_apply(idx, nv);
        cfg_value_str(&CFG_FIELDS[idx], cfg_field_get(idx), v, sizeof(v));
        printf("%s = %s\n", CFG_FIELDS[idx].key, v);
        return 0;
    }
    return -1;   /* no subcommand → interactive */
}

/* ══ Entry point ═══════════════════════════════════════════ */

int cli_main(const char *cmd, int argc, char **argv) {
    config_load(NULL);
    config_get_defaults(&g_defaults);
    g_color = isatty(STDOUT_FILENO) && !getenv("NO_COLOR");

    TuiScreen scr = SCR_CONFIG;
    if (!strcmp(cmd, "config") || !strcmp(cmd, "settings")) {
        int rc = config_command(argc, argv);
        if (rc >= 0) return rc;
        scr = SCR_CONFIG;
    } else if (!strcmp(cmd, "insights") || !strcmp(cmd, "stats")) scr = SCR_INSIGHTS;
    else scr = SCR_ABOUT;

    int interactive = isatty(STDIN_FILENO) && isatty(STDOUT_FILENO);
    if (!interactive) {
        if (scr == SCR_INSIGHTS) insights_print_plain();
        else if (scr == SCR_ABOUT) about_print_plain();
        else { char *a[] = { "config", "list", NULL }; config_command(2, a); }
        return 0;
    }

    term_size();
    if (!term_raw()) { fprintf(stderr, "timerc: cannot enter terminal raw mode\n"); return 1; }
    while ((int)scr >= 0) {
        TuiScreen next = (TuiScreen)-1;
        if (scr == SCR_CONFIG) screen_config(&next);
        else if (scr == SCR_INSIGHTS) screen_insights(&next);
        else if (scr == SCR_TEST) screen_test(&next);
        else screen_about(&next);
        scr = next;
    }
    term_restore();
    return 0;
}
