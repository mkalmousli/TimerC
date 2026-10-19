/* draw_timer.c — config/setup screen + mini/fullscreen timer display + sleep preview overlay.
 * Relocated from the old monolithic draw.c; see src/ui/ui_priv.h for
 * the shared primitives/theme/buffer helpers this pulls in from
 * draw_common.c. */
#include "timer.h"
#include "ui_priv.h"

static void ensure_tmr_buf(App *app) {
    /* Reuse the existing pixmap if it's already large enough — avoids
     * destroying/recreating on every resize step, making resize smooth. */
    if (app->tmr_buf &&
        app->tmr_w <= app->tmr_buf_w && app->tmr_h <= app->tmr_buf_h) return;

    if (app->tmr_buf_xft) { XftDrawDestroy(app->tmr_buf_xft); app->tmr_buf_xft = NULL; }
    if (app->tmr_buf)     { XFreePixmap(app->dpy, app->tmr_buf); app->tmr_buf = 0; }
    if (!app->tmr_win || app->tmr_w <= 0 || app->tmr_h <= 0) return;

    /* Allocate with 25% headroom so subsequent small growths don't reallocate */
    int alloc_w = (app->tmr_w * 5) / 4;
    int alloc_h = (app->tmr_h * 5) / 4;
    app->tmr_buf     = XCreatePixmap(app->dpy, app->tmr_win, alloc_w, alloc_h, app->depth);
    app->tmr_buf_xft = make_xft_draw(app, app->tmr_buf);
    app->tmr_buf_w   = alloc_w;
    app->tmr_buf_h   = alloc_h;
    app->cd_font_sz  = 0;
}


typedef struct {
    int top_y, ly, q_base, in_top, ch_top, ul_top;
    int quote_h, has_chips, has_until, content_h;
} CfgLayout;


static void cfg_compute_layout(App *app, CfgLayout *L) {
    ParseResult *pr = &app->cfg_preview;
    L->has_chips = pr->sw_shutdown || pr->sw_alert ||
                   pr->sw_fullscreen || pr->sw_pausable || pr->sw_lock || pr->sw_sleep;
    L->has_until = pr->until[0] != '\0';

    XftFont *qf = get_font(app, 16, 2);
    if (!qf) qf = get_font(app, 16, 0);
    L->quote_h = qf ? qf->ascent + qf->descent : 22;

    const int logo_r = 155, logo_gap = 30;
    const int input_h = 54, input_gap = 34;
    const int chip_h = 24, chip_gap = 24;
    const int until_badge_h = 62, until_badge_gap = 20;

    int logo_h = 310;
    L->content_h = logo_h + logo_gap + L->quote_h + input_gap + input_h;
    if (L->has_chips) L->content_h += chip_gap + chip_h;
    if (L->has_until) L->content_h += until_badge_gap + until_badge_h;

    L->top_y = (g_config.cfg_h - L->content_h) / 2;
    if (L->top_y < 8) L->top_y = 8;
    L->ly     = L->top_y + logo_r;
    L->q_base = L->ly + logo_r + logo_gap + (qf ? qf->ascent : 18);
    L->in_top = L->q_base + (qf ? qf->descent : 4) + input_gap;
    L->ch_top = L->in_top + input_h + chip_gap;
    L->ul_top = L->has_chips ? L->ch_top + chip_h + until_badge_gap
                            : L->in_top + input_h + until_badge_gap;
}


static int cfg_layout_signature(const CfgLayout *L) {
    return L->content_h | (L->has_chips << 12) | (L->has_until << 13) | (L->top_y << 14);
}


void cfg_refresh_preview(App *app) {
    parse_time_input(app->input, &app->cfg_preview);
}


void cfg_update_layout(App *app) {
    CfgLayout L;
    cfg_compute_layout(app, &L);
    app->cfg_layout_sig  = cfg_layout_signature(&L);
    app->cfg_anim_top    = L.top_y;
    app->cfg_anim_h      = L.in_top - L.top_y;
    app->cfg_panel_top   = L.in_top - 6;
    app->cfg_panel_h     = g_config.cfg_h - app->cfg_panel_top - 2;
}


static void cfg_present_region(App *app, int y, int h) {
    if (h <= 0) return;
    if (app->cfg_buf)
        XCopyArea(app->dpy, app->cfg_buf, app->cfg_win, app->gc,
                  0, y, g_config.cfg_w, h, 0, y);
}


static void cfg_draw_borders(App *app, Drawable buf, const Theme *th) {
    cfg_draw_borders_wh(app, buf, th, g_config.cfg_w, g_config.cfg_h);
}


static void cfg_present_borders(App *app) {
    if (!app->cfg_buf) return;
    XCopyArea(app->dpy, app->cfg_buf, app->cfg_win, app->gc,
              0, 0, g_config.cfg_w, 2, 0, 0);
    XCopyArea(app->dpy, app->cfg_buf, app->cfg_win, app->gc,
              0, g_config.cfg_h - 2, g_config.cfg_w, 2, 0, g_config.cfg_h - 2);
    XCopyArea(app->dpy, app->cfg_buf, app->cfg_win, app->gc,
              0, 0, 2, g_config.cfg_h, 0, 0);
    XCopyArea(app->dpy, app->cfg_buf, app->cfg_win, app->gc,
              g_config.cfg_w - 2, 0, 2, g_config.cfg_h, g_config.cfg_w - 2, 0);
}


static void cfg_draw_clock(App *app, Drawable buf, const Theme *th,
                           const CfgLayout *L) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    time_t raw = ts.tv_sec;
    struct tm *tm_info = localtime(&raw);
    double secs_f  = tm_info->tm_sec  + ts.tv_nsec / 1e9;
    double mins_f  = tm_info->tm_min  + secs_f  / 60.0;
    double hours_f = (tm_info->tm_hour % 12) + mins_f / 60.0;

    double whole = floor(secs_f);
    double frac  = secs_f - whole;
    double eased = frac * frac * frac * (frac * (frac * 6.0 - 15.0) + 10.0);
    double sweep = 0.30 * eased + 0.70 * frac;
    double secs_smooth = whole + sweep;

    double ang_h = hours_f * TAU / 12.0;
    double ang_m = mins_f  * TAU / 60.0;
    double ang_s = secs_smooth * TAU / 60.0;

    int r = 155, lx = g_config.cfg_w / 2, lcy = L->ly;
    int lw_bezel = (r + 9) / 18;
    int lw_hour  = (r + 6) / 12;
    int lw_min   = (r + 9) / 18;
    int dot_rad  = (r + 4) / 9;
    int r_rim    = r - lw_bezel - 1;
    int r_in_qtr = r_rim - r / 6;
    int r_in_hr  = r_rim - r / 9;
    int r_in_min = r_rim - r / 18;

    XSetForeground(app->dpy, app->gc, rgb_pixel(app, th->surface));
    XFillArc(app->dpy, buf, app->gc, lx-r, lcy-r, 2*r, 2*r, 0, 360*64);

    XSetLineAttributes(app->dpy, app->gc, lw_bezel, LineSolid, CapRound, JoinRound);
    XSetForeground(app->dpy, app->gc, rgb_pixel(app, th->accent));
    XDrawArc(app->dpy, buf, app->gc, lx-r, lcy-r, 2*r, 2*r, 0, 360*64);

    int ri = r - lw_bezel - 2;
    XSetLineAttributes(app->dpy, app->gc, 1, LineSolid, CapRound, JoinRound);
    XSetForeground(app->dpy, app->gc, rgb_pixel(app, th->border));
    XDrawArc(app->dpy, buf, app->gc, lx-ri, lcy-ri, 2*ri, 2*ri, 0, 360*64);

    for (int i = 0; i < 60; i++) {
        double a  = i * TAU / 60.0;
        double sa = sin(a), ca = cos(a);
        int is_qtr  = (i % 15 == 0);
        int is_hour = (i %  5 == 0);
        int r_in    = is_qtr  ? r_in_qtr :
                      is_hour ? r_in_hr  : r_in_min;
        int lw      = is_qtr  ? lw_bezel : (is_hour ? 2 : 1);
        unsigned long col = is_hour ? th->text : th->text_muted;
        XSetForeground(app->dpy, app->gc, rgb_pixel(app, col));
        XSetLineAttributes(app->dpy, app->gc, lw, LineSolid, CapRound, JoinRound);
        XDrawLine(app->dpy, buf, app->gc,
                  lx + (int)(r_rim * sa), lcy - (int)(r_rim * ca),
                  lx + (int)(r_in  * sa), lcy - (int)(r_in  * ca));
    }

    XSetForeground(app->dpy, app->gc, rgb_pixel(app, th->text));
    XSetLineAttributes(app->dpy, app->gc, lw_hour, LineSolid, CapRound, JoinRound);
    XDrawLine(app->dpy, buf, app->gc, lx, lcy,
              lx + (int)(r * 0.48 * sin(ang_h)),
              lcy - (int)(r * 0.48 * cos(ang_h)));

    XSetLineAttributes(app->dpy, app->gc, lw_min, LineSolid, CapRound, JoinRound);
    XDrawLine(app->dpy, buf, app->gc, lx, lcy,
              lx + (int)(r * 0.73 * sin(ang_m)),
              lcy - (int)(r * 0.73 * cos(ang_m)));

    XSetForeground(app->dpy, app->gc, rgb_pixel(app, th->accent));
    XSetLineAttributes(app->dpy, app->gc, 2, LineSolid, CapRound, JoinRound);
    {
        double sl = r * 0.84, tail = r * 0.18;
        int sx = lx + (int)(sl * sin(ang_s));
        int sy = lcy - (int)(sl * cos(ang_s));
        XDrawLine(app->dpy, buf, app->gc,
                  lx - (int)(tail * sin(ang_s)), lcy + (int)(tail * cos(ang_s)),
                  sx, sy);
        XSetForeground(app->dpy, app->gc, rgb_pixel(app, th->accent_soft));
        XFillArc(app->dpy, buf, app->gc, sx - 3, sy - 3, 6, 6, 0, 360 * 64);
        XSetForeground(app->dpy, app->gc, rgb_pixel(app, th->accent));
    }

    XSetForeground(app->dpy, app->gc, rgb_pixel(app, th->accent));
    XFillArc(app->dpy, buf, app->gc,
             lx - dot_rad, lcy - dot_rad, 2*dot_rad, 2*dot_rad, 0, 360*64);
    int cap = dot_rad / 2;
    XSetForeground(app->dpy, app->gc, rgb_pixel(app, th->surface));
    XFillArc(app->dpy, buf, app->gc, lx-cap, lcy-cap, 2*cap, 2*cap, 0, 360*64);
    XSetLineAttributes(app->dpy, app->gc, 0, LineSolid, CapButt, JoinMiter);
}


/* Theme strip along the bottom of the input page: a name label above a row
 * of colored dots (one per theme) with ‹ › arrows either side. */
#define TS_PITCH 24
#define TS_R      7

static void theme_strip_geom(int *x0, int *cy) {
    int total = N_THEMES * TS_PITCH - (TS_PITCH - 2 * TS_R);
    *x0 = (g_config.cfg_w - total) / 2 + TS_R;
    *cy = g_config.cfg_h - 30;
}

/* Returns 0..N_THEMES-1 for a dot, -2 for the ‹ arrow, -3 for ›, -1 otherwise. */
int theme_strip_hit(int x, int y) {
    int x0, cy;
    theme_strip_geom(&x0, &cy);
    if (y < cy - 14 || y > cy + 14) return -1;
    for (int i = 0; i < N_THEMES; i++)
        if (abs(x - (x0 + i * TS_PITCH)) <= TS_PITCH / 2) return i;
    if (x < x0 && x > x0 - 44) return -2;
    if (x > x0 + (N_THEMES - 1) * TS_PITCH && x < x0 + (N_THEMES - 1) * TS_PITCH + 44) return -3;
    return -1;
}

static void cfg_draw_theme_strip(App *app, Drawable buf, XftDraw *draw, const Theme *th) {
    int x0, cy;
    theme_strip_geom(&x0, &cy);
    XftFont *fs = get_font(app, 11, 0);
    if (fs) {
        char lbl[48];
        snprintf(lbl, sizeof(lbl), "%s  \xc2\xb7  Tab", THEMES[app->theme_idx].name);
        xft_draw_string_centered(app, draw, fs, th->text_muted, g_config.cfg_w / 2, cy - 18, lbl);
        xft_draw_string_centered(app, draw, fs, th->text_muted, x0 - 24, cy + 4, "\xe2\x80\xb9");
        xft_draw_string_centered(app, draw, fs, th->text_muted,
                                 x0 + (N_THEMES - 1) * TS_PITCH + 24, cy + 4, "\xe2\x80\xba");
    }
    for (int i = 0; i < N_THEMES; i++) {
        int cx = x0 + i * TS_PITCH;
        XSetForeground(app->dpy, app->gc, rgb_pixel(app, THEMES[i].bg));
        XFillArc(app->dpy, buf, app->gc, cx - TS_R, cy - TS_R, 2 * TS_R, 2 * TS_R, 0, 360 * 64);
        XSetForeground(app->dpy, app->gc, rgb_pixel(app, THEMES[i].accent));
        XFillArc(app->dpy, buf, app->gc, cx - TS_R + 3, cy - TS_R + 3, 2 * TS_R - 6, 2 * TS_R - 6, 0, 360 * 64);
        XSetForeground(app->dpy, app->gc, rgb_pixel(app, i == app->theme_idx ? th->text : th->border));
        XSetLineAttributes(app->dpy, app->gc, i == app->theme_idx ? 2 : 1, LineSolid, CapRound, JoinRound);
        XDrawArc(app->dpy, buf, app->gc, cx - TS_R, cy - TS_R, 2 * TS_R, 2 * TS_R, 0, 360 * 64);
        XSetLineAttributes(app->dpy, app->gc, 1, LineSolid, CapButt, JoinMiter);
    }
}


static void cfg_draw_panel(App *app, Drawable buf, XftDraw *draw,
                           const Theme *th, const CfgLayout *L) {
    const int input_h = 54;
    ParseResult *pr = &app->cfg_preview;

    int fx = 24, fy = L->in_top, fw = g_config.cfg_w - 48, fh = input_h;
    fill_rect(app, buf, th->surface_soft, fx, fy, fw, fh);
    draw_rect_outline(app, buf, th->accent, fx, fy, fw, fh);
    XftFont *f = get_font(app, 17, 0);
    if (f) {
        int ty = fy + (fh + f->ascent - f->descent) / 2;
        int text_x = fx + 12, avail_w = fw - 24;
        const char *text = app->input;
        int start = 0;
        if (app->cursor_pos > 0) {
            char tmp[MAX_INPUT];
            for (start = 0; start <= app->cursor_pos; start++) {
                strncpy(tmp, text + start, (size_t)(app->cursor_pos - start));
                tmp[app->cursor_pos - start] = '\0';
                if (text_width(app, f, tmp) <= avail_w - 6) break;
            }
        }
        char disp[MAX_INPUT];
        strncpy(disp, text + start, sizeof(disp) - 1);
        disp[sizeof(disp) - 1] = '\0';
        xft_draw_string(app, draw, f, th->text, text_x, ty, disp);
        int blink = app->cursor_blink ||
                    (app->state == STATE_CONFIG &&
                     ms_now() - app->cursor_blink_ts < 100);
        if (blink) {
            char before[MAX_INPUT];
            int cp = app->cursor_pos - start;
            if (cp < 0) cp = 0;
            strncpy(before, text + start, (size_t)cp);
            before[cp] = '\0';
            int cx2 = text_x + text_width(app, f, before);
            XSetForeground(app->dpy, app->gc, rgb_pixel(app, th->text));
            XFillRectangle(app->dpy, buf, app->gc, cx2, fy + 9, 2, fh - 18);
        }
    }

    cfg_draw_theme_strip(app, buf, draw, th);

    if (pr->until[0]) {
        char big[16] = "";
        if (pr->seconds > 0) format_time(pr->seconds, big, sizeof(big));
        char sml[24];
        snprintf(sml, sizeof(sml), "until  %s", pr->until);

        XftFont *fb = get_font(app, 22, 1);
        XftFont *fs = get_font(app, 9,  0);
        int fbh = fb ? fb->ascent + fb->descent : 30;
        int fsh = fs ? fs->ascent + fs->descent : 13;
        int big_w = fb ? text_width(app, fb, big) : 80;
        int sml_w = fs ? text_width(app, fs, sml) : 60;
        int inner = big_w > sml_w ? big_w : sml_w;
        int bw = inner + 28;
        int bh = fsh + 5 + fbh + 12;
        int bx = (g_config.cfg_w - bw) / 2;
        int uy = L->ul_top;

        fill_rect(app, buf, th->until_chip_bg, bx, uy, bw, bh);
        draw_rect_outline(app, buf, th->until_chip_border, bx, uy, bw, bh);
        int y_sml = uy + 6 + (fs ? fs->ascent : 11);
        if (fs) xft_draw_string_centered(app, draw, fs, th->text_muted,
                                          g_config.cfg_w / 2, y_sml, sml);
        int y_big = y_sml + (fs ? fs->descent : 2) + 5 + (fb ? fb->ascent : 25);
        if (fb && big[0])
            xft_draw_string_centered(app, draw, fb, th->until_clock_fg,
                                     g_config.cfg_w / 2, y_big, big);
    }
}


void draw_config_anim(App *app) {
    if (!app->cfg_win) return;
    ensure_cfg_buf(app);
    const Theme *th = cfg_theme(app);
    Drawable buf  = app->cfg_buf ? (Drawable)app->cfg_buf : (Drawable)app->cfg_win;
    XftDraw *draw = app->cfg_buf ? app->cfg_buf_xft : app->cfg_xft;
    if (!draw) return;

    cfg_refresh_preview(app);
    CfgLayout L;
    cfg_compute_layout(app, &L);

    double t = monotonic_now();
    cfg_draw_cyber_bg(app, buf, th, 0, 0, g_config.cfg_w, g_config.cfg_h, t);
    cfg_draw_clock(app, buf, th, &L);
    XftFont *qf = get_font(app, 16, 2);
    if (!qf) qf = get_font(app, 16, 0);
    if (qf) draw_quote_morph(app, draw, qf, buf, th, g_config.cfg_w / 2, L.q_base, g_config.cfg_w - 48);
    cfg_draw_panel(app, buf, draw, th, &L);
    cfg_draw_borders(app, buf, th);

    if (app->cfg_buf)
        XCopyArea(app->dpy, app->cfg_buf, app->cfg_win, app->gc,
                  0, 0, g_config.cfg_w, g_config.cfg_h, 0, 0);
    XFlush(app->dpy);
}


void draw_config_panel(App *app) {
    if (!app->cfg_win) return;
    ensure_cfg_buf(app);
    const Theme *th = cfg_theme(app);
    Drawable buf  = app->cfg_buf ? (Drawable)app->cfg_buf : (Drawable)app->cfg_win;
    XftDraw *draw = app->cfg_buf ? app->cfg_buf_xft : app->cfg_xft;
    if (!draw) return;

    CfgLayout L;
    cfg_compute_layout(app, &L);
    int y = app->cfg_panel_top;
    int h = app->cfg_panel_h;
    if (h <= 0) return;

    cfg_draw_cyber_bg(app, buf, th, 0, y, g_config.cfg_w, h, monotonic_now());
    cfg_draw_panel(app, buf, draw, th, &L);
    cfg_draw_borders(app, buf, th);
    cfg_present_region(app, y, h);
    cfg_present_borders(app);
}

void draw_config(App *app) {
    if (!app->cfg_win) return;
    ensure_cfg_buf(app);
    const Theme *th = cfg_theme(app);
    int W = g_config.cfg_w, H = g_config.cfg_h;
    Drawable buf  = app->cfg_buf ? (Drawable)app->cfg_buf : (Drawable)app->cfg_win;
    XftDraw *draw = app->cfg_buf ? app->cfg_buf_xft : app->cfg_xft;
    if (!draw) return;

    cfg_refresh_preview(app);
    cfg_update_layout(app);

    cfg_draw_cyber_bg(app, buf, th, 0, 0, W, H, monotonic_now());

    CfgLayout L;
    cfg_compute_layout(app, &L);
    cfg_draw_clock(app, buf, th, &L);
    XftFont *qf = get_font(app, 16, 2);
    if (!qf) qf = get_font(app, 16, 0);
    if (qf) draw_quote_morph(app, draw, qf, buf, th, W / 2, L.q_base, W - 48);
    cfg_draw_panel(app, buf, draw, th, &L);
    cfg_draw_borders(app, buf, th);

    if (app->cfg_buf)
        XCopyArea(app->dpy, app->cfg_buf, app->cfg_win, app->gc, 0, 0, W, H, 0, 0);
    XFlush(app->dpy);
}


void cfg_redraw_after_input(App *app) {
    CfgLayout L;
    cfg_refresh_preview(app);
    cfg_compute_layout(app, &L);
    int sig = cfg_layout_signature(&L);
    if (sig != app->cfg_layout_sig) {
        draw_config(app);
    } else {
        cfg_update_layout(app);
        /* Full-frame redraw keeps the animated background in sync while typing. */
        draw_config_anim(app);
    }
}


void create_config_window(App *app) {
    const Theme *th = cur_theme(app);
    Window root_ret, child_ret; int rx, ry, wx, wy; unsigned int mask;
    Window root = DefaultRootWindow(app->dpy);
    XQueryPointer(app->dpy, root, &root_ret, &child_ret, &rx, &ry, &wx, &wy, &mask);
    detect_monitor(app, rx, ry);

    int x = app->mon_x + (app->mon_w - g_config.cfg_w)/2;
    int y = app->mon_y + (app->mon_h - g_config.cfg_h)/2;
    app->cfg_x = x; app->cfg_y = y;

    XSetWindowAttributes swa;
    swa.override_redirect = True;
    swa.background_pixel  = rgb_pixel(app, th->bg);
    swa.border_pixel      = 0;
    swa.colormap          = app->cmap;
    swa.event_mask        = ExposureMask | KeyPressMask
                          | ButtonPressMask | ButtonReleaseMask
                          | PointerMotionMask | StructureNotifyMask
                          | FocusChangeMask;
    app->cfg_win = XCreateWindow(app->dpy, root, x, y, g_config.cfg_w, g_config.cfg_h, 0,
        app->depth, InputOutput, app->vis,
        CWOverrideRedirect|CWBackPixel|CWBorderPixel|CWColormap|CWEventMask, &swa);

    XStoreName(app->dpy, app->cfg_win, "TimerC");
    XMapRaised(app->dpy, app->cfg_win);
    XFlush(app->dpy);
    /* Focus is requested in the MapNotify handler once the window is viewable. */
    app->cfg_xft = make_xft_draw(app, app->cfg_win);
    app->cursor_blink = 1;
    app->cursor_blink_ts = ms_now();
}

/* ── Sleep-mode indicator badge ────────────────────────────
 * Small pill shown in the corner of the timer view(s) whenever the
 * sleep-mode phase state machine (audio.c's sleep_control_thread) is
 * actively playing — i.e. this is a sleep-mode timer with its ambient
 * audio thread still running. */

static void draw_sleep_badge(App *app, Drawable buf, XftDraw *draw,
                              const Theme *th, int W, int H, int mini) {
    if (!app->sleep_mode || !app->sleep_audio_active) return;
    if (mini && W < 150) return; /* not enough room in a tiny mini window */

    const char *label = "● Sleeping…";
    XftFont *f = get_font(app, mini ? 9 : 13, 1);
    if (!f) return;
    int tw = text_width(app, f, label);
    int bw = tw + (mini ? 14 : 24);
    int bh = f->ascent + f->descent + (mini ? 6 : 12);
    int bx = mini ? 6 : 14;
    int by = mini ? 6 : 14;
    fill_rect(app, buf, th->accent_soft, bx, by, bw, bh);
    draw_rect_outline(app, buf, th->accent, bx, by, bw, bh);
    xft_draw_string(app, draw, f, th->accent, bx + (bw - tw)/2,
                    by + (bh + f->ascent - f->descent)/2, label);
    (void)H;
}

/* ── Mini timer window ─────────────────────────────────── */


void draw_timer_mini(App *app) {
    if (!app->tmr_win) return;
    ensure_tmr_buf(app);
    if (!app->tmr_buf) return;

    const Theme *th = cur_theme(app);
    int W = app->tmr_w, H = app->tmr_h;
    Drawable buf  = app->tmr_buf;
    XftDraw *draw = app->tmr_buf_xft;
    if (!draw) return;

    fill_rect(app, buf, th->bg, 0, 0, W, H);

    /* Layout constants: 2px accent border + 2px inner padding on every side */
    const int border = 2;
    const int pad    = 2;
    /* Minimum countdown height before we start hiding decorative elements */
    const int min_cd = 20;

    int prog_h = (H >= 76) ? scaled(H / 20, 4, 12) : 0;

    /* Content region (inside border+pad) */
    int cx  = border + pad;              /* left edge  = 4 */
    int cw  = W - (border + pad) * 2;   /* width      = W-8 */
    int cy2 = H - prog_h - border;      /* bottom edge of content */

    /* y walks downward, accumulating used height from the top */
    int y = border + pad;                /* start at 4 */

    /* ── Shutdown icon — only if countdown still has room ── */
    if (app->mode == MODE_SHUTDOWN) {
        int r    = scaled(H / 10, 5, 13);
        int need = r + r / 2 + 1 + r + pad + 3;  /* top gap + radius + gap */
        if (cy2 - (y + need) >= min_cd) {
            int cy_i = y + r + r / 2 + 1;
            y = draw_power_icon(app, buf, W / 2, cy_i, r,
                                shutdown_pulse_color(th)) + 3;
        }
    }

    /* ── Task name — only if wide enough and countdown still has room ── */
    if (app->task[0] && W > 80) {
        int sz = scaled(H / 14, 6, 11);
        XftFont *f = get_font(app, sz, 0);
        if (f) {
            int need = f->ascent + f->descent + pad + 2;
            if (cy2 - (y + need) >= min_cd) {
                char task[MAX_TASK];
                strncpy(task, app->task, sizeof(task) - 1);
                task[sizeof(task) - 1] = '\0';
                while ((int)strlen(task) > 1 &&
                       text_width(app, f, task) > cw - pad * 2) {
                    int len = (int)strlen(task);
                    task[len - 1] = '\0';
                    if (len > 4) {
                        task[len-2] = '.'; task[len-3] = '.'; task[len-4] = '.';
                    }
                }
                xft_draw_string_centered(app, draw, f, th->text_muted,
                                         W / 2, y + f->ascent, task);
                y += f->ascent + f->descent + 2;
            }
        }
    }

    /* ── Until badge — only if countdown still has room ── */
    if (app->until_str[0]) {
        int sz = scaled(H / 9, 8, 18);
        XftFont *f = get_font(app, sz, 1);
        if (f) {
            int bh   = f->ascent + f->descent + 6;
            int need = bh + pad + 3;
            if (cy2 - (y + need) >= min_cd) {
                int bw = text_width(app, f, app->until_str) + 20;
                int bx = (W - bw) / 2;
                fill_rect(app, buf, th->until_clock_bg, bx, y, bw, bh);
                draw_rect_outline(app, buf, th->until_chip_border, bx, y, bw, bh);
                xft_draw_string_centered(app, draw, f, th->until_clock_fg,
                                         W / 2, y + f->ascent + 3, app->until_str);
                y += bh + 3;
            }
        }
    }

    /* ── Countdown — stretches to fill ALL remaining space ── */
    {
        /* 2px padding below last element and above the progress/bottom border */
        int cd_x = cx;
        int cd_y = y + pad;
        int cd_w = cw;
        int cd_h = cy2 - pad - cd_y;

        char time_str[16];
        format_time(app->remaining_seconds, time_str, sizeof(time_str));
        int show = (app->state != STATE_PAUSED) ||
                   ((int)(monotonic_now() * 2) % 2 == 0);

        if (cd_w > 8 && cd_h > 8) {
            int fmt_len = (int)strlen(time_str);
            /* Invalidate font cache when the available box changes */
            if (app->cd_font_sz == 0 ||
                app->cd_font_avail_h != cd_h ||
                app->cd_font_avail_w != cd_w ||
                app->cd_font_fmt_len != fmt_len) {
                app->cd_font_sz      = fit_font_size_h(app, cd_h, 10, 200, 3);
                app->cd_font_avail_w = cd_w;
                app->cd_font_avail_h = cd_h;
                app->cd_font_fmt_len = fmt_len;
            }
            XftFont *f = get_font(app, app->cd_font_sz, 3);
            if (f && show) {
                draw_text_stretched(app, buf, cd_x, cd_y, cd_w, cd_h,
                                    f, timer_display_color(app), th->bg, time_str);
            }
            if (app->state == STATE_PAUSED) {
                int psz = scaled(H / 12, 6, 10);
                XftFont *pf = get_font(app, psz, 0);
                if (pf) xft_draw_string_centered(app, draw, pf, th->danger,
                                                  W / 2, cy2 - pad, "PAUSED");
            }
        }
    }

    /* ── Progress bar ──────────────────────────────────── */
    if (prog_h > 0 && app->total_seconds > 0) {
        int py = H - prog_h;
        double frac = 1.0 - app->remaining_exact / app->total_seconds;
        if (frac < 0.0) frac = 0.0;
        if (frac > 1.0) frac = 1.0;
        int bar_w = (int)(W * frac);
        fill_rect(app, buf, th->surface_soft, 0, py, W, prog_h);
        if (bar_w > 0) fill_rect(app, buf, timer_display_color(app), 0, py, bar_w, prog_h);
    }

    /* ── Border — color matches the countdown text ──────── */
    {
        unsigned long bc = timer_display_color(app);
        fill_rect(app, buf, bc, 0, 0, W, border);
        fill_rect(app, buf, bc, 0, H - border, W, border);
        fill_rect(app, buf, bc, 0, 0, border, H);
        fill_rect(app, buf, bc, W - border, 0, border, H);
    }

    draw_sleep_badge(app, buf, draw, th, W, H, 1);

    XCopyArea(app->dpy, buf, app->tmr_win, app->gc, 0, 0, W, H, 0, 0);
    XFlush(app->dpy);
}

/* ── Fullscreen timer window ───────────────────────────── */


static void draw_timer_fullscreen(App *app) {
    if (!app->tmr_win) return;
    ensure_tmr_buf(app);
    if (!app->tmr_buf) return;

    const Theme *th = cur_theme(app);
    int W = app->tmr_w, H = app->tmr_h;
    Drawable buf  = app->tmr_buf;
    XftDraw *draw = app->tmr_buf_xft;
    if (!draw) return;

    fill_rect(app, buf, th->bg, 0, 0, W, H);

    int prog_h = H/10;
    int top    = H/12;
    int bottom = H - prog_h;

    /* ── Shutdown icon ─────────────────────────────────── */
    if (app->mode == MODE_SHUTDOWN) {
        int r    = scaled(W / 28, 14, 52);
        int cy_i = top + r + r / 2 + 2;
        top = draw_power_icon(app, buf, W / 2, cy_i, r,
                              shutdown_pulse_color(th)) + 10;
    }

    /* ── Task name ─────────────────────────────────────── */
    if (app->task[0]) {
        int tsz = fit_font_size(app, app->task, W - W/8, H/10, 10, 48, 0);
        XftFont *f = get_font(app, tsz, 0);
        if (f) {
            xft_draw_string_centered(app, draw, f, th->text, W/2, top+f->ascent, app->task);
            top += f->ascent + f->descent + 8;
        }
    }

    /* ── Until badge (fullscreen size — kept as-is) ────── */
    if (app->until_str[0]) {
        int usz = W/16; if (usz < 18) usz = 18;
        XftFont *f = get_font(app, usz, 1);
        if (f) {
            int bw = text_width(app, f, app->until_str) + 40;
            int bh = f->ascent + f->descent + 16;
            int bx = (W-bw)/2;
            fill_rect(app, buf, th->until_clock_bg, bx, top, bw, bh);
            draw_rect_outline(app, buf, th->until_chip_border, bx, top, bw, bh);
            xft_draw_string_centered(app, draw, f, th->until_clock_fg,
                                     W/2, top+f->ascent+8, app->until_str);
            top += bh + 12;
        }
    }

    /* ── Countdown — monospace, stretched ──────────────── */
    {
        char time_str[16];
        format_time(app->remaining_seconds, time_str, sizeof(time_str));

        /* All geometry proportional to W and H */
        int h_margin  = W / 16;          /* horizontal margin ~6 % of width */
        int pause_rsv = (app->state == STATE_PAUSED) ? H / 18 : 0;
        int pad       = 2;
        int cd_x  = h_margin + pad;
        int cd_y  = top + pad;
        int cd_w  = W - h_margin * 2 - pad * 2;
        int cd_h  = bottom - top - pause_rsv - pad * 2;

        int show = (app->state != STATE_PAUSED) ||
                   ((int)(monotonic_now() * 2) % 2 == 0);

        if (cd_w > 0 && cd_h > 0) {
            int fmt_len = (int)strlen(time_str);
            if (app->cd_font_sz == 0 ||
                app->cd_font_avail_h != cd_h ||
                app->cd_font_avail_w != cd_w  ||
                app->cd_font_fmt_len != fmt_len) {
                app->cd_font_sz      = fit_font_size_h(app, cd_h, 20, 600, 3);
                app->cd_font_avail_w = cd_w;
                app->cd_font_avail_h = cd_h;
                app->cd_font_fmt_len = fmt_len;
            }
            XftFont *f = get_font(app, app->cd_font_sz, 3);
            if (f && show) {
                draw_text_stretched(app, buf, cd_x, cd_y, cd_w, cd_h,
                                    f, timer_display_color(app), th->bg, time_str);
            }
        }
    }

    /* ── Paused label — proportional to H ──────────────── */
    if (app->state == STATE_PAUSED) {
        int psz = scaled(H / 30, 14, 28);
        XftFont *f = get_font(app, psz, 0);
        if (f) xft_draw_string_centered(app, draw, f, th->danger,
                                         W / 2, bottom - H / 40, "PAUSED");
    }

    /* ── Progress bar ──────────────────────────────────── */
    if (app->total_seconds > 0) {
        double frac = 1.0 - app->remaining_exact / app->total_seconds;
        if (frac < 0.0) frac = 0.0;
        if (frac > 1.0) frac = 1.0;
        int bar_w = (int)(W * frac);
        fill_rect(app, buf, th->surface, 0, bottom, W, prog_h);
        if (bar_w > 0)
            fill_rect(app, buf, timer_display_color(app), 0, bottom, bar_w, prog_h);
    }

    draw_sleep_badge(app, buf, draw, th, W, H, 0);

    XCopyArea(app->dpy, buf, app->tmr_win, app->gc, 0, 0, W, H, 0, 0);
    XFlush(app->dpy);
}


void draw_timer(App *app) {
#ifdef USE_GL
    if (app->use_gl) {
        gl_make_current(app, app->tmr_win);
        gl_draw_timer(app);
        return;
    }
#endif
    if (app->mini) draw_timer_mini(app);
    else            draw_timer_fullscreen(app);
}

/* ── Timer window creation/destruction ─────────────────── */


void destroy_timer_window(App *app) {
    app->cd_font_sz = 0;
    app->cursor_in_resize = 0;
#ifdef USE_GL
    if (app->use_gl) {
        if (app->tmr_win) { XDestroyWindow(app->dpy, app->tmr_win); app->tmr_win = 0; }
        XFlush(app->dpy);
        return;
    }
#endif
    if (app->tmr_buf_xft) { XftDrawDestroy(app->tmr_buf_xft); app->tmr_buf_xft = NULL; }
    if (app->tmr_buf)     { XFreePixmap(app->dpy, app->tmr_buf); app->tmr_buf = 0; }
    app->tmr_buf_w = 0; app->tmr_buf_h = 0;
    if (app->tmr_xft) { XftDrawDestroy(app->tmr_xft);  app->tmr_xft = NULL; }
    if (app->tmr_win) { XDestroyWindow(app->dpy, app->tmr_win); app->tmr_win = 0; }
    XFlush(app->dpy);
}


void create_timer_window(App *app) {
    destroy_timer_window(app);
    const Theme *th = cur_theme(app);
    Window root = DefaultRootWindow(app->dpy);
    int has_until = app->until_str[0] != '\0';

#ifdef USE_GL
    if (app->use_gl) {
        int X, Y, W, H;
        if (app->mini) {
            if (app->has_saved_mini) {
                W = app->saved_mini_w; H = app->saved_mini_h;
                X = app->saved_mini_x; Y = app->saved_mini_y;
            } else {
                W = g_config.mini_w;
                H = has_until ? g_config.mini_h_until : g_config.mini_h_base;
                X = app->mon_x + app->mon_w - W - 20;
                Y = app->mon_y + 20;
            }
        } else {
            W = app->mon_w; H = app->mon_h; X = app->mon_x; Y = app->mon_y;
        }
        app->tmr_w = W; app->tmr_h = H;
        app->tmr_x = X; app->tmr_y = Y;
        app->tmr_win = gl_create_timer_window(app, X, Y, W, H, !app->mini);
        if (app->tmr_win) {
            gl_make_current(app, app->tmr_win);
            draw_timer(app);
        }
        return;
    }
#endif

    if (app->mini) {
        int W, H, X, Y;
        if (app->has_saved_mini) {
            W = app->saved_mini_w; H = app->saved_mini_h;
            X = app->saved_mini_x; Y = app->saved_mini_y;
            if (X+W > app->mon_x+app->mon_w) X = app->mon_x+app->mon_w-W-4;
            if (Y+H > app->mon_y+app->mon_h) Y = app->mon_y+app->mon_h-H-4;
            if (X < app->mon_x) X = app->mon_x;
            if (Y < app->mon_y) Y = app->mon_y;
        } else {
            W = g_config.mini_w;
            H = has_until ? g_config.mini_h_until : g_config.mini_h_base;
            X = app->mon_x + app->mon_w - W - 20;
            Y = app->mon_y + 20;
        }
        app->tmr_w = W; app->tmr_h = H;
        app->tmr_x = X; app->tmr_y = Y;

        XSetWindowAttributes swa;
        swa.override_redirect = True;
        swa.background_pixel  = rgb_pixel(app, th->bg);
        swa.border_pixel      = rgb_pixel(app, th->accent);
        swa.colormap          = app->cmap;
        swa.event_mask        = ExposureMask | ButtonPressMask
                              | ButtonReleaseMask | PointerMotionMask | KeyPressMask;
        app->tmr_win = XCreateWindow(app->dpy, root, X, Y, W, H, 0,
            app->depth, InputOutput, app->vis,
            CWOverrideRedirect|CWBackPixel|CWBorderPixel|CWColormap|CWEventMask, &swa);

        Atom states[2] = { app->net_wm_state_above, app->net_wm_state_skip_taskbar };
        XChangeProperty(app->dpy, app->tmr_win, app->net_wm_state,
                        XA_ATOM, 32, PropModeReplace, (unsigned char *)states, 2);
    } else {
        int W = app->mon_w, H = app->mon_h, X = app->mon_x, Y = app->mon_y;
        app->tmr_w = W; app->tmr_h = H;
        app->tmr_x = X; app->tmr_y = Y;

        XSetWindowAttributes swa;
        swa.override_redirect = True;
        swa.background_pixel  = rgb_pixel(app, th->bg);
        swa.border_pixel      = 0;
        swa.colormap          = app->cmap;
        swa.event_mask        = ExposureMask | ButtonPressMask
                              | ButtonReleaseMask | StructureNotifyMask | KeyPressMask;
        app->tmr_win = XCreateWindow(app->dpy, root, X, Y, W, H, 0,
            app->depth, InputOutput, app->vis,
            CWOverrideRedirect|CWBackPixel|CWBorderPixel|CWColormap|CWEventMask, &swa);

        XWMHints hints;
        memset(&hints, 0, sizeof(hints));
        hints.flags = InputHint;
        hints.input = True;
        XSetWMHints(app->dpy, app->tmr_win, &hints);

        set_window_on_all_workspaces(app, app->tmr_win);
        disable_compositor_bypass(app, app->tmr_win);

        Atom states[3] = {
            app->net_wm_state_above,
            app->net_wm_state_sticky,
            app->net_wm_state_skip_taskbar
        };
        XChangeProperty(app->dpy, app->tmr_win, app->net_wm_state,
                        XA_ATOM, 32, PropModeReplace, (unsigned char *)states, 3);
    }

    XStoreName(app->dpy, app->tmr_win, "TimerC");
    XMapRaised(app->dpy, app->tmr_win);
    /* override_redirect windows never receive focus from the WM on their
     * own (see the cfg_win MapNotify/ButtonPress handling in app.c) —
     * grab it explicitly so key events like Ctrl+G actually arrive. */
    XSetInputFocus(app->dpy, app->tmr_win, RevertToPointerRoot, CurrentTime);
    XFlush(app->dpy);
    app->tmr_xft = make_xft_draw(app, app->tmr_win);
}
