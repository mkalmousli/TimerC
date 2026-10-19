/* draw_insights.c — Insights / session data browser (Ctrl+I).
 * Relocated from the old monolithic draw.c; see src/ui/ui_priv.h for
 * the shared primitives/theme/buffer helpers this pulls in from
 * draw_common.c. */
#include "timer.h"
#include "ui_priv.h"

/* ── Insights / data browser (Ctrl+I) ───────────────────── */

typedef struct {
    int W, H, pad, gap;

    int hdr_y, hdr_h;
    int nav_y, nav_h;
    int nav_prev_x, nav_today_x, nav_next_x, nav_btn_y, nav_btn_h;

    int chips_y, chips_h, chip_w;
    int footer_y;

    int chart_y, chart_h, chart_lbl_h, bar_y, bar_h, bar_axis_h;
    int chart_l_x, chart_l_w, chart_r_x, chart_r_w;
    int bar_w, bar_gap;

    int mini_y, mini_h, mini_lbl_h, mini_bar_h;

    int panel_y, panel_h, panel_lbl_h;
    int left_x, left_w, right_x, right_w;

    int list_y, list_h, sess_row_h, sess_visible;
    int detail_y, detail_h;
    int evt_hdr_y, evt_y, evt_h, evt_row_h, evt_visible;
} InsLayout;


static InsLayout g_ins_L;


static void ins_compute_layout(InsLayout *L) {
    memset(L, 0, sizeof(*L));
    L->W   = INS_W;
    L->H   = INS_H;
    L->pad = 12;
    L->gap = 8;

    int y = L->pad;

    L->hdr_y = y;
    L->hdr_h = 22;
    y += L->hdr_h + 4;

    L->nav_y = y;
    L->nav_h = 30;
    y += L->nav_h + L->gap;

    L->chips_y = y;
    L->chips_h = 58;
    L->chip_w  = (L->W - L->pad * 2 - 4 * L->gap - 64) / 5;
    if (L->chip_w < 52) L->chip_w = 52;
    y += L->chips_h + L->gap;

    L->footer_y = y;
    y += L->gap;

    L->chart_y      = y;
    L->chart_lbl_h  = 0;
    L->bar_h        = 76;
    L->bar_axis_h   = 14;
    L->chart_h      = L->bar_h + L->bar_axis_h;
    L->bar_y        = L->chart_y;
    y += L->chart_h + L->gap;

    L->mini_y       = y;
    L->mini_lbl_h   = 0;
    L->mini_bar_h   = 44;
    L->mini_h       = L->mini_bar_h + 6;
    y += L->mini_h + L->gap;

    L->panel_y      = y;
    L->panel_h      = L->H - L->pad - y;
    L->panel_lbl_h  = 0;

    L->left_x  = L->pad;
    L->left_w  = (L->W - L->pad * 2 - L->gap) * 42 / 100;
    L->right_x = L->left_x + L->left_w + L->gap;
    L->right_w = L->W - L->pad - L->right_x;

    L->chart_l_x = L->left_x;
    L->chart_l_w = L->left_w;
    L->chart_r_x = L->right_x;
    L->chart_r_w = L->right_w;

    L->bar_gap = 3;
    L->bar_w   = (L->chart_l_w - (INS_CHART_DAYS - 1) * L->bar_gap) / INS_CHART_DAYS;
    if (L->bar_w < 3) L->bar_w = 3;

    int nav_btn_w = 40, today_w = 52;
    L->nav_btn_h  = L->nav_h - 4;
    L->nav_btn_y  = L->nav_y + 2;
    L->nav_next_x = L->W - L->pad - nav_btn_w;
    L->nav_today_x = L->nav_next_x - L->gap - today_w;
    L->nav_prev_x  = L->nav_today_x - L->gap - nav_btn_w;

    L->list_y = L->panel_y + 4;
    L->list_h = L->panel_h - 4;

    L->sess_row_h = 28;
    L->sess_visible = L->list_h / L->sess_row_h;
    if (L->sess_visible > INS_SESS_VISIBLE) L->sess_visible = INS_SESS_VISIBLE;
    if (L->sess_visible < 3) L->sess_visible = 3;
    L->sess_row_h = L->list_h / L->sess_visible;
    if (L->sess_row_h > 38) L->sess_row_h = 38;
    if (L->sess_row_h < 26) L->sess_row_h = 26;
    L->sess_visible = L->list_h / L->sess_row_h;
    if (L->sess_visible < 1) L->sess_visible = 1;

    int right_body = L->panel_h;
    L->detail_h = right_body * 36 / 100;
    if (L->detail_h < 100) L->detail_h = 100;
    if (L->detail_h > 140) L->detail_h = 140;
    if (L->detail_h > right_body - 100) L->detail_h = right_body - 100;

    L->detail_y   = L->panel_y + 4;
    L->evt_hdr_y  = L->detail_y + L->detail_h + 6;
    L->evt_y      = L->evt_hdr_y + 4;
    L->evt_h      = L->panel_y + L->panel_h - L->evt_y;
    if (L->evt_h < 40) L->evt_h = 40;

    L->evt_row_h = 20;
    L->evt_visible = L->evt_h / L->evt_row_h;
    if (L->evt_visible > INS_EVT_VISIBLE) L->evt_visible = INS_EVT_VISIBLE;
    if (L->evt_visible < 4) L->evt_visible = 4;
    if (L->evt_visible * L->evt_row_h > L->evt_h)
        L->evt_row_h = L->evt_h / L->evt_visible;
    if (L->evt_row_h < 15) L->evt_row_h = 15;
    L->evt_visible = L->evt_h / L->evt_row_h;
    if (L->evt_visible < 1) L->evt_visible = 1;
}


static void ins_fit_text(App *app, XftFont *f, char *s, int max_w) {
    if (!f || !s || max_w <= 0) return;
    int len = (int)strlen(s);
    while (len > 0 && text_width(app, f, s) > max_w) {
        len--;
        s[len] = '\0';
    }
    if (len > 3 && text_width(app, f, s) > max_w) {
        s[len - 1] = s[len - 2] = s[len - 3] = '.';
        s[len] = '\0';
    }
}


static void ins_fmt_duration(long secs, char *out, int sz) {
    if (secs < 0) secs = 0;
    int h = (int)(secs / 3600);
    int m = (int)((secs % 3600) / 60);
    if (h > 0) snprintf(out, sz, "%dh %02dm", h, m);
    else       snprintf(out, sz, "%dm", m);
}


static void ins_fmt_clock(const char *iso, char *out, int sz) {
    if (!iso || !iso[0]) { snprintf(out, sz, "--:--"); return; }
    struct tm tm = {0};
    if (strptime(iso, "%Y-%m-%dT%H:%M:%S", &tm))
        strftime(out, sz, "%H:%M:%S", &tm);
    else
        snprintf(out, sz, "--:--");
}


static int ins_parse_iso_ts(const char *iso, time_t *out) {
    if (!iso || !iso[0] || !out) return 0;
    struct tm tm = {0};
    if (!strptime(iso, "%Y-%m-%dT%H:%M:%S", &tm)) return 0;
    *out = mktime(&tm);
    return 1;
}


static void ins_fmt_delta(long secs, char *out, int sz) {
    if (secs < 0) secs = 0;
    if (secs < 60)
        snprintf(out, sz, "%lds", secs);
    else if (secs < 3600)
        snprintf(out, sz, "%dm%02ds", (int)(secs / 60), (int)(secs % 60));
    else
        snprintf(out, sz, "%dh%02dm", (int)(secs / 3600), (int)((secs % 3600) / 60));
}


static int ins_session_cross_day(const DbSession *row) {
    time_t ts, te;
    if (!ins_parse_iso_ts(row->started_at, &ts)) return 0;
    if (!row->ended_at[0] || !ins_parse_iso_ts(row->ended_at, &te)) return 0;
    struct tm tm_s, tm_e;
    localtime_r(&ts, &tm_s);
    localtime_r(&te, &tm_e);
    return tm_s.tm_yday != tm_e.tm_yday || tm_s.tm_year != tm_e.tm_year;
}


static long ins_event_delta_s(App *app, int idx) {
    if (idx < 0 || idx >= app->ins_n_events) return 0;
    time_t cur;
    if (!ins_parse_iso_ts(app->ins_events[idx].ts, &cur)) return 0;

    time_t prev;
    if (idx > 0) {
        if (!ins_parse_iso_ts(app->ins_events[idx - 1].ts, &prev)) return 0;
    } else if (app->ins_n_sessions > 0 && app->ins_sel >= 0 &&
               app->ins_sel < app->ins_n_sessions) {
        if (!ins_parse_iso_ts(app->ins_sessions[app->ins_sel].started_at, &prev))
            return 0;
    } else {
        return 0;
    }
    return (long)difftime(cur, prev);
}


static unsigned long ins_status_color(const Theme *th, const char *status) {
    if (!status) return th->text_muted;
    if (!strcmp(status, "completed")) return th->accent;
    if (!strcmp(status, "stopped"))   return th->danger;
    return th->text_muted;
}


static void ins_draw_hbar(App *app, Drawable buf, const Theme *th,
                          int x, int y, int w, int h,
                          int val, int max_val, unsigned long fill,
                          unsigned long track) {
    fill_rect(app, buf, track, x, y, w, h);
    if (max_val <= 0 || val <= 0) return;
    int fw = (int)((long)val * w / max_val);
    if (fw < 2 && val > 0) fw = 2;
    if (fw > w) fw = w;
    fill_rect(app, buf, fill, x, y, fw, h);
    if (fw > 2)
        fill_rect(app, buf, lerp_rgb(fill, th->text, 0.3), x, y, fw, 1);
}


static void ins_draw_vbar(App *app, Drawable buf, const Theme *th,
                          int x, int y, int w, int h,
                          int val, int max_val, unsigned long col, int selected) {
    fill_rect(app, buf, th->surface_soft, x, y, w, h);
    if (max_val <= 0) max_val = 1;
    int fh = (int)((long)val * h / max_val);
    if (val > 0 && fh < 3) fh = 3;
    if (fh > h) fh = h;
    if (fh > 0) {
        fill_rect(app, buf, col, x, y + h - fh, w, fh);
        fill_rect(app, buf, lerp_rgb(col, th->text, 0.28), x, y + h - fh, w, 2);
    }
    if (selected)
        draw_rect_outline(app, buf, th->accent, x, y, w, h);
}


static void ins_draw_tile(App *app, XftDraw *draw, Drawable buf, const Theme *th,
                          int x, int y, int w, int h,
                          const char *sym, const char *val, unsigned long accent) {
    fill_rect(app, buf, th->surface, x, y, w, h);
    fill_rect(app, buf, accent, x, y, w, 3);
    draw_rect_outline(app, buf, th->border, x, y, w, h);
    XftFont *fv = get_font(app, 20, 1);
    XftFont *fs = get_font(app, 8, 1);
    if (fv) xft_draw_string_centered(app, draw, fv, th->accent, x + w / 2, y + h / 2 + 2, val);
    if (fs) xft_draw_string_centered(app, draw, fs, th->text_muted, x + w / 2, y + h - 7, sym);
}


static void ins_draw_gauge(App *app, XftDraw *draw, Drawable buf, const Theme *th,
                           int cx, int cy, int r, int pct, unsigned long col) {
    int d = r * 2;
    XSetLineAttributes(app->dpy, app->gc, 5, LineSolid, CapRound, JoinRound);
    XSetForeground(app->dpy, app->gc, rgb_pixel(app, th->surface_soft));
    XDrawArc(app->dpy, buf, app->gc, cx - r, cy - r, d, d, 0, 360 * 64);
    if (pct > 0) {
        int span = (int)(360.0 * pct / 100.0 * 64.0);
        if (span > 360 * 64) span = 360 * 64;
        if (span < 64) span = 64;
        XSetForeground(app->dpy, app->gc, rgb_pixel(app, col));
        XDrawArc(app->dpy, buf, app->gc, cx - r, cy - r, d, d, 90 * 64, -span);
    }
    XSetLineAttributes(app->dpy, app->gc, 0, LineSolid, CapButt, JoinMiter);
    char pct_s[8];
    snprintf(pct_s, sizeof(pct_s), "%d", pct);
    XftFont *fv = get_font(app, 14, 1);
    XftFont *fs = get_font(app, 7, 0);
    if (fv) xft_draw_string_centered(app, draw, fv, th->accent, cx, cy + 2, pct_s);
    if (fs) xft_draw_string_centered(app, draw, fs, th->text_muted, cx, cy + r - 4, "OK%");
}


static const char *ins_event_glyph(const char *event) {
    if (!strcmp(event, "timer_pause"))   return "||";
    if (!strcmp(event, "timer_resume"))  return ">";
    if (!strcmp(event, "timer_stop"))    return "[]";
    if (!strcmp(event, "alert_dismiss")) return "v";
    if (!strcmp(event, "alert_open"))    return "!";
    if (!strcmp(event, "shutdown_trigger")) return "X";
    if (!strcmp(event, "half_point"))    return "o";
    if (!strcmp(event, "last_3s"))       return "*";
    if (!strcmp(event, "insights_open")) return "i";
    return ".";
}


static void ins_draw_nav_btn(App *app, XftDraw *draw, Drawable buf, const Theme *th,
                             int x, int y, int w, int h, const char *label) {
    fill_rect(app, buf, th->surface_soft, x, y, w, h);
    draw_rect_outline(app, buf, th->border, x, y, w, h);
    XftFont *f = get_font(app, 10, 1);
    if (f) xft_draw_string_centered(app, draw, f, th->text, x + w / 2, y + h / 2 + 4, label);
}


static void ins_goto_day(App *app, int offset) {
    if (offset < 0) offset = 0;
    app->ins_sel = 0;
    app->ins_sess_scroll = 0;
    app->ins_evt_scroll = 0;
    db_insights_load_day(app, offset);
    draw_insights(app);
}


void ins_enter(App *app) {
    int w0 = app->cfg_buf_w ? app->cfg_buf_w : g_config.cfg_w;
    int h0 = app->cfg_buf_h ? app->cfg_buf_h : g_config.cfg_h;
    int cx = app->cfg_x + w0 / 2;
    int cy = app->cfg_y + h0 / 2;
    app->insights_open = 1;
    app->cfg_x = cx - INS_W / 2;
    app->cfg_y = cy - INS_H / 2;
    if (app->cfg_buf_xft) { XftDrawDestroy(app->cfg_buf_xft); app->cfg_buf_xft = NULL; }
    if (app->cfg_buf)     { XFreePixmap(app->dpy, app->cfg_buf); app->cfg_buf = 0; }
    app->cfg_buf_w = app->cfg_buf_h = 0;
    XMoveResizeWindow(app->dpy, app->cfg_win, app->cfg_x, app->cfg_y, INS_W, INS_H);
    XFlush(app->dpy);
}


void ins_leave(App *app) {
    int cx = app->cfg_x + INS_W / 2;
    int cy = app->cfg_y + INS_H / 2;
    app->insights_open = 0;
    app->cfg_x = cx - g_config.cfg_w / 2;
    app->cfg_y = cy - g_config.cfg_h / 2;
    if (app->cfg_buf_xft) { XftDrawDestroy(app->cfg_buf_xft); app->cfg_buf_xft = NULL; }
    if (app->cfg_buf)     { XFreePixmap(app->dpy, app->cfg_buf); app->cfg_buf = 0; }
    app->cfg_buf_w = app->cfg_buf_h = 0;
    XMoveResizeWindow(app->dpy, app->cfg_win, app->cfg_x, app->cfg_y, g_config.cfg_w, g_config.cfg_h);
    XFlush(app->dpy);
}


static void ins_draw_session_detail(App *app, XftDraw *draw, Drawable buf,
                                    const Theme *th, int x, int y, int w, int h) {
    fill_rect(app, buf, th->surface, x, y, w, h);
    draw_rect_outline(app, buf, th->border, x, y, w, h);

    if (app->ins_n_sessions <= 0) {
        XftFont *fr = get_font(app, 9, 0);
        if (fr) xft_draw_string_centered(app, draw, fr, th->text_muted,
                                         x + w / 2, y + h / 2, "—");
        return;
    }

    const DbSession *row = &app->ins_sessions[app->ins_sel];
    int pct = 0;
    if (row->duration_s > 0 && row->elapsed_s >= 0)
        pct = (int)((long)row->elapsed_s * 100L / row->duration_s);
    if (pct > 100) pct = 100;

    int pauses = 0, resumes = 0;
    for (int i = 0; i < app->ins_n_events; i++) {
        if (!strcmp(app->ins_events[i].event, "timer_pause")) pauses++;
        if (!strcmp(app->ins_events[i].event, "timer_resume")) resumes++;
    }

    int pad = 10;
    int ring_r = (h - pad * 2) / 2 - 4;
    if (ring_r > 42) ring_r = 42;
    if (ring_r < 24) ring_r = 24;
    int ring_cx = x + pad + ring_r + 4;
    int ring_cy = y + h / 2;
    ins_draw_gauge(app, draw, buf, th, ring_cx, ring_cy, ring_r, pct,
                   ins_status_color(th, row->status));

    int bx = x + pad * 2 + ring_r * 2 + 16;
    int bw = w - (bx - x) - pad;
    if (bw < 80) bw = 80;
    int by = y + pad + 8;
    int bar_h = 10;
    int gap = 18;

    char dur[16], elapsed[16];
    format_time(row->duration_s, dur, sizeof(dur));
    if (row->elapsed_s >= 0) format_time(row->elapsed_s, elapsed, sizeof(elapsed));
    else                     snprintf(elapsed, sizeof(elapsed), "—");

    ins_draw_hbar(app, buf, th, bx, by, bw, bar_h, 100, 100, th->surface_soft, th->bg);
    ins_draw_hbar(app, buf, th, bx, by, bw, bar_h, pct, 100,
                  ins_status_color(th, row->status), th->surface_soft);

    XftFont *fs = get_font(app, 8, 0);
    XftFont *ft = get_font(app, 10, 1);
    if (ft) {
        char task[48];
        snprintf(task, sizeof(task), "%s", row->task[0] ? row->task : "—");
        ins_fit_text(app, ft, task, bw);
        xft_draw_string(app, draw, ft, th->text, bx, y + pad + 2, task);
    }

    by += gap + 4;
    if (fs) {
        char start[8], durbuf[16], times[32];
        ins_fmt_clock(row->started_at, start, sizeof(start));
        if (row->elapsed_s >= 0) ins_fmt_duration(row->elapsed_s, durbuf, sizeof(durbuf));
        else                       ins_fmt_duration(row->duration_s, durbuf, sizeof(durbuf));
        snprintf(times, sizeof(times), "%s  %s%s", start, durbuf,
                 ins_session_cross_day(row) ? "  +1d" : "");
        xft_draw_string(app, draw, fs, th->text_muted, bx, by, times);
        by += 14;
    }

    int plan_w = bw / 3 - 4;
    ins_draw_hbar(app, buf, th, bx, by, plan_w, 6, 100, 100, th->accent_soft, th->surface_soft);
    ins_draw_hbar(app, buf, th, bx + plan_w + 6, by, plan_w, 6,
                  row->elapsed_s > 0 ? pct : 0, 100, th->accent, th->surface_soft);
    int delta = (row->elapsed_s >= 0 && row->duration_s > 0)
                ? row->elapsed_s - row->duration_s : 0;
    unsigned long dcol = delta >= 0 ? th->accent : th->danger;
    int dw = delta >= 0 ? delta : -delta;
    int dmax = row->duration_s > 0 ? row->duration_s : 1;
    ins_draw_hbar(app, buf, th, bx + (plan_w + 6) * 2, by, plan_w, 6,
                  dw, dmax, dcol, th->surface_soft);

    by += 12;
    if (fs) {
        char meta[48];
        snprintf(meta, sizeof(meta), "||%d  >%d  ·  %s",
                 pauses, resumes, row->status);
        xft_draw_string(app, draw, fs, th->text_muted, bx, by, meta);
        char durline[32];
        snprintf(durline, sizeof(durline), "%s / %s", elapsed, dur);
        int tw = text_width(app, fs, durline);
        xft_draw_string(app, draw, fs, th->text_muted, bx + bw - tw, by, durline);
    }
}


static unsigned long ins_event_color(const Theme *th, const char *event) {
    if (!strcmp(event, "timer_pause"))  return th->text_muted;
    if (!strcmp(event, "timer_resume")) return th->accent;
    if (!strcmp(event, "timer_stop") || !strcmp(event, "alert_dismiss"))
        return th->danger;
    if (!strcmp(event, "alert_open") || !strcmp(event, "shutdown_trigger"))
        return th->accent;
    if (!strcmp(event, "half_point") || !strcmp(event, "last_3s"))
        return th->until_clock_fg;
    return th->text;
}


static int ins_chart_day_offset(int bar_idx) {
    return (INS_CHART_DAYS - 1) - bar_idx;
}


static int ins_sess_index_at(int my) {
    const InsLayout *L = &g_ins_L;
    if (my < L->list_y || my >= L->list_y + L->sess_visible * L->sess_row_h)
        return -1;
    return (my - L->list_y) / L->sess_row_h;
}


static int ins_chart_bar_at(int mx, int my) {
    const InsLayout *L = &g_ins_L;
    if (my < L->bar_y || my >= L->bar_y + L->bar_h + L->bar_axis_h)
        return -1;
    if (mx < L->chart_l_x || mx >= L->chart_l_x + L->chart_l_w)
        return -1;
    int rel = mx - L->chart_l_x;
    int step = L->bar_w + L->bar_gap;
    if (step <= 0) return -1;
    int idx = rel / step;
    if (idx < 0 || idx >= INS_CHART_DAYS) return -1;
    if (rel % step > L->bar_w) return -1;
    return idx;
}


void draw_insights(App *app) {
    if (!app->cfg_win) return;
    ensure_cfg_buf(app);
    ins_compute_layout(&g_ins_L);
    const InsLayout *L = &g_ins_L;

    const Theme *th = cfg_theme(app);
    const DbSummary *s = &app->ins_summary;
    const DbDayView *day = &app->ins_day;
    Drawable buf  = app->cfg_buf ? (Drawable)app->cfg_buf : (Drawable)app->cfg_win;
    XftDraw *draw = app->cfg_buf ? app->cfg_buf_xft : app->cfg_xft;
    if (!draw) return;

    cfg_draw_cyber_bg(app, buf, th, 0, 0, L->W, L->H, monotonic_now());

    XftFont *fh = get_font(app, 14, 1);
    XftFont *fs = get_font(app, 8, 0);
    XftFont *fm = get_font(app, 9, 1);
    XftFont *fr = get_font(app, 8, 0);

    if (fh) xft_draw_string(app, draw, fh, th->accent, L->pad, L->hdr_y + 14, "DATA LOG");
    if (fs) {
        const char *hint = "← →  ↑↓  Ctrl+I";
        xft_draw_string(app, draw, fs, th->text_muted,
                        L->W - L->pad - text_width(app, fs, hint), L->hdr_y + 12, hint);
    }

    ins_draw_nav_btn(app, draw, buf, th, L->nav_prev_x, L->nav_btn_y, 40, L->nav_btn_h, "<");
    ins_draw_nav_btn(app, draw, buf, th, L->nav_next_x, L->nav_btn_y, 40, L->nav_btn_h, ">");
    ins_draw_nav_btn(app, draw, buf, th, L->nav_today_x, L->nav_btn_y, 52, L->nav_btn_h, "now");
    if (fh) {
        char nav[48];
        snprintf(nav, sizeof(nav), "%s%s",
                 day->day_label, app->ins_day_offset == 0 ? " · today" : "");
        xft_draw_string_centered(app, draw, fh, th->text, L->W / 2, L->nav_y + 18, nav);
    }

    /* Summary stat line: last-7-days session count + current streak
     * (consecutive days with at least one session, counting back from
     * the most recent day in the loaded window) — derived from the
     * already-loaded 14-day summary, no extra DB query. */
    if (fr) {
        int week_sessions = 0;
        for (int i = INS_CHART_DAYS - 7; i < INS_CHART_DAYS; i++)
            if (i >= 0) week_sessions += s->day_counts[i];
        int streak = 0;
        for (int i = INS_CHART_DAYS - 1; i >= 0; i--) {
            if (s->day_counts[i] <= 0) break;
            streak++;
        }
        char stat[64];
        snprintf(stat, sizeof(stat), "7d: %d session%s  ·  streak: %d day%s",
                 week_sessions, week_sessions == 1 ? "" : "s",
                 streak, streak == 1 ? "" : "s");
        xft_draw_string(app, draw, fr, th->text_muted,
                        L->W - L->pad - text_width(app, fr, stat), L->nav_y + 30, stat);
    }

    /* Stat tiles + success gauge */
    char vbuf[32];
    int cx = L->pad;
    snprintf(vbuf, sizeof(vbuf), "%d", day->sessions);
    ins_draw_tile(app, draw, buf, th, cx, L->chips_y, L->chip_w, L->chips_h,
                  "runs", vbuf, th->accent); cx += L->chip_w + L->gap;
    ins_fmt_duration(day->elapsed_s, vbuf, sizeof(vbuf));
    ins_draw_tile(app, draw, buf, th, cx, L->chips_y, L->chip_w, L->chips_h,
                  "time", vbuf, th->until_clock_fg); cx += L->chip_w + L->gap;
    snprintf(vbuf, sizeof(vbuf), "%d", day->completed);
    ins_draw_tile(app, draw, buf, th, cx, L->chips_y, L->chip_w, L->chips_h,
                  "done", vbuf, th->accent); cx += L->chip_w + L->gap;
    snprintf(vbuf, sizeof(vbuf), "%d", day->stopped);
    ins_draw_tile(app, draw, buf, th, cx, L->chips_y, L->chip_w, L->chips_h,
                  "stop", vbuf, th->danger); cx += L->chip_w + L->gap;
    if (day->avg_duration_s > 0) ins_fmt_duration(day->avg_duration_s, vbuf, sizeof(vbuf));
    else snprintf(vbuf, sizeof(vbuf), "—");
    ins_draw_tile(app, draw, buf, th, cx, L->chips_y, L->chip_w, L->chips_h,
                  "avg", vbuf, th->text_muted);
    int succ = day->sessions > 0 ? day->completed * 100 / day->sessions : 0;
    int gx = L->W - L->pad - 56;
    int gy = L->chips_y + L->chips_h / 2;
    ins_draw_gauge(app, draw, buf, th, gx + 28, gy, 24, succ, th->accent);

    /* 14-day activity */
    int chart_max = 1;
    for (int i = 0; i < INS_CHART_DAYS; i++)
        if (s->day_counts[i] > chart_max) chart_max = s->day_counts[i];
    for (int i = 0; i < INS_CHART_DAYS; i++) {
        int bx = L->chart_l_x + i * (L->bar_w + L->bar_gap);
        int selected = !strcmp(s->day_dates[i], day->day_iso);
        ins_draw_vbar(app, buf, th, bx, L->bar_y, L->bar_w, L->bar_h,
                      s->day_counts[i], chart_max,
                      selected ? th->accent : th->text_muted, selected);
        if (fs && (selected || s->day_counts[i] > 0)) {
            int ax = bx + L->bar_w / 2;
            int ay = L->bar_y + L->bar_h + 10;
            if (selected) {
                char cnt[8];
                snprintf(cnt, sizeof(cnt), "%d", s->day_counts[i]);
                xft_draw_string_centered(app, draw, fs, th->accent, ax, ay, cnt);
            }
            xft_draw_string_centered(app, draw, fs,
                                     selected ? th->text : th->text_muted,
                                     ax, ay + (selected ? 11 : 0), s->day_labels[i]);
        }
    }

    /* Hour heatmap */
    int hour_max = 1;
    for (int i = 0; i < 24; i++)
        if (day->hour_counts[i] > hour_max) hour_max = day->hour_counts[i];
    int hw = L->chart_r_w / 24;
    if (hw < 2) hw = 2;
    for (int i = 0; i < 24; i++) {
        int hx = L->chart_r_x + i * hw;
        double intensity = hour_max > 0
                           ? (double)day->hour_counts[i] / (double)hour_max : 0.0;
        unsigned long hc = lerp_rgb(th->surface_soft, th->accent, 0.15 + 0.85 * intensity);
        fill_rect(app, buf, hc, hx, L->bar_y, hw - 1, L->bar_h);
        if (day->hour_counts[i] > 0 && hw >= 8 && fs) {
            char n[4];
            snprintf(n, sizeof(n), "%d", day->hour_counts[i]);
            xft_draw_string_centered(app, draw, fs, th->bg,
                                     hx + hw / 2, L->bar_y + L->bar_h / 2 + 3, n);
        }
    }
    if (fs) {
        int ay = L->bar_y + L->bar_h + 10;
        xft_draw_string(app, draw, fs, th->text_muted, L->chart_r_x, ay, "0");
        xft_draw_string(app, draw, fs, th->text_muted,
                        L->chart_r_x + L->chart_r_w - 12, ay, "23");
    }

    /* Status stack + duration mix */
    int mini_y = L->mini_y;
    int mix_total = day->completed + day->stopped;
    if (mix_total < 1) mix_total = 1;
    fill_rect(app, buf, th->surface_soft, L->left_x, mini_y, L->left_w, L->mini_bar_h);
    int done_w = (int)((long)day->completed * L->left_w / mix_total);
    int stop_w = (int)((long)day->stopped * L->left_w / mix_total);
    if (day->completed > 0 && done_w < 2) done_w = 2;
    if (day->stopped > 0 && stop_w < 2) stop_w = 2;
    if (done_w > 0) fill_rect(app, buf, th->accent, L->left_x, mini_y, done_w, L->mini_bar_h);
    if (stop_w > 0) fill_rect(app, buf, th->danger,
                              L->left_x + done_w, mini_y, stop_w, L->mini_bar_h);

    static const char *bucket_syms[] = { "<5", "15", "30", "30+" };
    int bmax = 1;
    for (int i = 0; i < 4; i++)
        if (day->dur_buckets[i] > bmax) bmax = day->dur_buckets[i];
    int bw = L->right_w / 4;
    for (int i = 0; i < 4; i++) {
        int bx = L->right_x + i * bw;
        int slot_w = bw - 4;
        ins_draw_vbar(app, buf, th, bx, mini_y, slot_w, L->mini_bar_h,
                      day->dur_buckets[i], bmax, th->accent, 0);
        if (fs && day->dur_buckets[i] > 0) {
            char cnt[8];
            snprintf(cnt, sizeof(cnt), "%d", day->dur_buckets[i]);
            xft_draw_string_centered(app, draw, fs, th->text,
                                     bx + slot_w / 2, mini_y + L->mini_bar_h / 2 + 3, cnt);
        }
        if (fs)
            xft_draw_string_centered(app, draw, fs, th->text_muted,
                                     bx + slot_w / 2, mini_y + L->mini_bar_h + 8,
                                     bucket_syms[i]);
    }

    fill_rect(app, buf, th->border, L->pad, L->panel_y - 1, L->W - L->pad * 2, 1);

    /* Sessions — visual rows */
    if (app->ins_n_sessions == 0) {
        if (fr) xft_draw_string_centered(app, draw, fr, th->text_muted,
                                         L->left_x + L->left_w / 2, L->list_y + 30, "—");
    } else {
        for (int vis = 0; vis < L->sess_visible; vis++) {
            int idx = app->ins_sess_scroll + vis;
            if (idx >= app->ins_n_sessions) break;
            const DbSession *row = &app->ins_sessions[idx];
            int ry = L->list_y + vis * L->sess_row_h;
            int rh = L->sess_row_h - 2;
            int selected = (idx == app->ins_sel);

            if (selected) {
                fill_rect(app, buf, th->accent_soft, L->left_x, ry, L->left_w, rh);
                fill_rect(app, buf, th->accent, L->left_x, ry, 3, rh);
            } else if (vis % 2 == 0) {
                fill_rect(app, buf, th->surface, L->left_x, ry, L->left_w, rh);
            }

            unsigned long sc = ins_status_color(th, row->status);
            fill_rect(app, buf, sc, L->left_x + 6, ry + rh / 2 - 3, 6, 6);

            int pct = 0;
            if (row->duration_s > 0 && row->elapsed_s >= 0)
                pct = (int)((long)row->elapsed_s * 100L / row->duration_s);
            if (pct > 100) pct = 100;
            int bar_x = L->left_x + 18;
            int bar_w = L->left_w - 90;
            ins_draw_hbar(app, buf, th, bar_x, ry + rh - 8, bar_w, 5, pct, 100,
                          sc, th->surface_soft);

            char dur[16], task[40];
            if (row->elapsed_s >= 0) ins_fmt_duration(row->elapsed_s, dur, sizeof(dur));
            else                       ins_fmt_duration(row->duration_s, dur, sizeof(dur));
            snprintf(task, sizeof(task), "%s", row->task[0] ? row->task : "—");
            ins_fit_text(app, fm, task, bar_w);

            if (fm)
                xft_draw_string(app, draw, fm, selected ? th->text : th->text_muted,
                                bar_x, ry + rh / 2 + 2, task);
            if (fr) {
                char label[20];
                snprintf(label, sizeof(label), "%s%s", dur,
                         ins_session_cross_day(row) ? " +1d" : "");
                xft_draw_string(app, draw, fr, th->text_muted,
                                L->left_x + L->left_w - 6 - text_width(app, fr, label),
                                ry + 10, label);
            }
        }
    }

    ins_draw_session_detail(app, draw, buf, th, L->right_x, L->detail_y, L->right_w, L->detail_h);

    /* Event spine timeline */
    if (app->ins_n_sessions > 0 && app->ins_n_events > 0) {
        int spine_x = L->right_x + 14;
        fill_rect(app, buf, th->border, spine_x, L->evt_y, 2, L->evt_h);

        for (int vis = 0; vis < L->evt_visible; vis++) {
            int idx = app->ins_evt_scroll + vis;
            if (idx >= app->ins_n_events) break;
            const DbEvent *ev = &app->ins_events[idx];
            int ey = L->evt_y + vis * L->evt_row_h + L->evt_row_h / 2;
            unsigned long ec = ins_event_color(th, ev->event);

            fill_rect(app, buf, ec, spine_x - 3, ey - 3, 8, 8);
            draw_rect_outline(app, buf, th->bg, spine_x - 3, ey - 3, 8, 8);

            if (fm) {
                const char *glyph = ins_event_glyph(ev->event);
                xft_draw_string(app, draw, fm, ec, spine_x + 12, ey + 4, glyph);
            }
            if (fr) {
                char dbuf[16];
                ins_fmt_delta(ins_event_delta_s(app, idx), dbuf, sizeof(dbuf));
                xft_draw_string(app, draw, fr, th->text_muted, spine_x + 36, ey + 3, dbuf);
            }
        }
    } else if (fr) {
        xft_draw_string_centered(app, draw, fr, th->text_muted,
                                 L->right_x + L->right_w / 2, L->evt_y + 24, "—");
    }

    cfg_draw_borders_wh(app, buf, th, L->W, L->H);
    if (app->cfg_buf)
        XCopyArea(app->dpy, app->cfg_buf, app->cfg_win, app->gc, 0, 0, L->W, L->H, 0, 0);
    XFlush(app->dpy);
}


void ins_handle_key(App *app, XKeyEvent *ev) {
    char buf[32] = "";
    KeySym ks;
    XLookupString(ev, buf, sizeof(buf) - 1, &ks, NULL);

    if (ks == XK_Escape ||
        ((ev->state & ControlMask) && (ks == XK_i || ks == XK_I))) {
        ins_leave(app);
        draw_config(app);
        return;
    }

    if (ks == XK_Left || ks == XK_bracketleft) {
        ins_goto_day(app, app->ins_day_offset + 1);
        return;
    }
    if (ks == XK_Right || ks == XK_bracketright) {
        if (app->ins_day_offset > 0)
            ins_goto_day(app, app->ins_day_offset - 1);
        return;
    }
    if (ks == XK_Home && (ev->state & ControlMask)) {
        ins_goto_day(app, 0);
        return;
    }

    ins_compute_layout(&g_ins_L);
    const InsLayout *L = &g_ins_L;
    int redraw = 0;

    if (app->ins_n_sessions > 0) {
        if (ks == XK_Up || ks == XK_k) {
            if (app->ins_sel > 0) {
                app->ins_sel--;
                if (app->ins_sel < app->ins_sess_scroll)
                    app->ins_sess_scroll = app->ins_sel;
                db_insights_load_events(app, app->ins_sessions[app->ins_sel].id);
                app->ins_evt_scroll = 0;
                redraw = 1;
            }
        } else if (ks == XK_Down || ks == XK_j) {
            if (app->ins_sel < app->ins_n_sessions - 1) {
                app->ins_sel++;
                if (app->ins_sel >= app->ins_sess_scroll + L->sess_visible)
                    app->ins_sess_scroll = app->ins_sel - L->sess_visible + 1;
                db_insights_load_events(app, app->ins_sessions[app->ins_sel].id);
                app->ins_evt_scroll = 0;
                redraw = 1;
            }
        } else if (ks == XK_Home) {
            app->ins_sel = 0;
            app->ins_sess_scroll = 0;
            db_insights_load_events(app, app->ins_sessions[0].id);
            app->ins_evt_scroll = 0;
            redraw = 1;
        } else if (ks == XK_End) {
            app->ins_sel = app->ins_n_sessions - 1;
            if (app->ins_sel >= L->sess_visible)
                app->ins_sess_scroll = app->ins_sel - L->sess_visible + 1;
            else
                app->ins_sess_scroll = 0;
            db_insights_load_events(app, app->ins_sessions[app->ins_sel].id);
            app->ins_evt_scroll = 0;
            redraw = 1;
        }
    }

    if (ks == XK_Page_Up) {
        if (app->ins_sess_scroll > 0) {
            app->ins_sess_scroll -= L->sess_visible;
            if (app->ins_sess_scroll < 0) app->ins_sess_scroll = 0;
            redraw = 1;
        } else if (app->ins_evt_scroll > 0) {
            app->ins_evt_scroll -= L->evt_visible;
            if (app->ins_evt_scroll < 0) app->ins_evt_scroll = 0;
            redraw = 1;
        }
    } else if (ks == XK_Page_Down) {
        if (app->ins_sess_scroll + L->sess_visible < app->ins_n_sessions) {
            app->ins_sess_scroll += L->sess_visible;
            int max_scroll = app->ins_n_sessions - L->sess_visible;
            if (max_scroll < 0) max_scroll = 0;
            if (app->ins_sess_scroll > max_scroll) app->ins_sess_scroll = max_scroll;
            redraw = 1;
        } else if (app->ins_evt_scroll + L->evt_visible < app->ins_n_events) {
            app->ins_evt_scroll += L->evt_visible;
            int max_scroll = app->ins_n_events - L->evt_visible;
            if (max_scroll < 0) max_scroll = 0;
            if (app->ins_evt_scroll > max_scroll) app->ins_evt_scroll = max_scroll;
            redraw = 1;
        }
    }

    if (redraw) draw_insights(app);
}


int ins_handle_click(App *app, int mx, int my) {
    if (!app->insights_open) return 0;
    ins_compute_layout(&g_ins_L);
    const InsLayout *L = &g_ins_L;

    if (my >= L->nav_btn_y && my < L->nav_btn_y + L->nav_btn_h) {
        if (mx >= L->nav_prev_x && mx < L->nav_prev_x + 40) {
            ins_goto_day(app, app->ins_day_offset + 1);
            return 1;
        }
        if (mx >= L->nav_next_x && mx < L->nav_next_x + 40 &&
            app->ins_day_offset > 0) {
            ins_goto_day(app, app->ins_day_offset - 1);
            return 1;
        }
        if (mx >= L->nav_today_x && mx < L->nav_today_x + 52) {
            ins_goto_day(app, 0);
            return 1;
        }
    }

    int bar = ins_chart_bar_at(mx, my);
    if (bar >= 0) {
        ins_goto_day(app, ins_chart_day_offset(bar));
        return 1;
    }

    if (app->ins_n_sessions <= 0) return 0;
    int vis = ins_sess_index_at(my);
    if (vis < 0) return 0;
    int idx = app->ins_sess_scroll + vis;
    if (idx >= app->ins_n_sessions) return 0;
    if (idx == app->ins_sel) return 1;
    app->ins_sel = idx;
    db_insights_load_events(app, app->ins_sessions[idx].id);
    app->ins_evt_scroll = 0;
    draw_insights(app);
    return 1;
}
