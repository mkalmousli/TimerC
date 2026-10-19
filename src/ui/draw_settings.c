/* draw_settings.c — Settings overlay (Ctrl+G).
 * Relocated from the old monolithic draw.c; see src/ui/ui_priv.h for
 * the shared primitives/theme/buffer helpers this pulls in from
 * draw_common.c. */
#include "timer.h"
#include "ui_priv.h"

/* ── Settings overlay (Ctrl+G) ─────────────────────────────
 * Generic accessor table (CFG_FIELDS, config.c) drives everything here:
 * a top-level category list, drilling into per-category sliders
 * (numeric fields) and toggle chips (bools), matching the visual
 * language of the help/insights overlays above. Every edit updates
 * g_config immediately and calls config_save(). */

#define SET_PAD        24
#define SET_CAT_ROW_H  46
#define SET_FIELD_ROW_H 58

static int settings_list_categories(const char *out[8]) {
    int c = 0;
    for (int i = 0; i < N_CFG_FIELDS; i++) {
        int found = 0;
        for (int j = 0; j < c; j++)
            if (!strcmp(out[j], CFG_FIELDS[i].category)) { found = 1; break; }
        if (!found && c < 8) out[c++] = CFG_FIELDS[i].category;
    }
    return c;
}


static int settings_fields_in_category(const char *cat, int out_idx[32]) {
    int n = 0;
    for (int i = 0; i < N_CFG_FIELDS && n < 32; i++)
        if (!strcmp(CFG_FIELDS[i].category, cat)) out_idx[n++] = i;
    return n;
}


static void settings_cat_rect(int row, int *x, int *y, int *w, int *h) {
    *x = SET_PAD; *w = g_config.cfg_w - 2*SET_PAD;
    *y = SET_PAD + 50 + row * SET_CAT_ROW_H;
    *h = SET_CAT_ROW_H - 8;
}


static void settings_back_rect(int *x, int *y, int *w, int *h) {
    *x = SET_PAD; *y = SET_PAD + 6; *w = 90; *h = 26;
}


static void settings_close_rect(int *x, int *y, int *w, int *h) {
    *w = 90; *h = 26;
    *x = g_config.cfg_w - SET_PAD - *w; *y = SET_PAD + 6;
}


/* Categories whose values feed an actual sound — worth a per-field live
 * audition button rather than making the user squint at slider numbers.
 * There is no category-wide "Test" anymore (removed — every field tests
 * itself in isolation instead, see settings_field_test_btn_rect). */
static int settings_cat_is_testable(const char *cat) {
    return !strcmp(cat, "Alarm") || !strcmp(cat, "Sleep");
}


/* Is a preview currently playing for the category on screen? */
static int settings_test_is_playing(App *app, const char *cat) {
    if (!strcmp(cat, "Alarm")) return app->alert_audio_active;
    if (!strcmp(cat, "Sleep")) return app->sleep_audio_active;
    return 0;
}


/* Every option's preview is isolated: starting one always stops
 * whatever else was playing first, so two previews (or a stale one left
 * over from a field/category the user has since left) never overlap or
 * bleed into each other. Called before every test starts and whenever
 * navigation would otherwise leave a preview running unattended.
 *
 * Only ever touches sleep_audio_active when settings_sleep_test_active
 * says *this* preview set it — a real running timer's ambient sleep
 * audio can be genuinely active underneath an overlay opened via
 * Ctrl+G-from-timer, and must never be silenced by leaving a settings
 * page (see the same guarantee already made for alert_audio_active in
 * settings_close_overlay() below). */
static void settings_stop_any_test(App *app) {
    int stop_sleep = app->settings_sleep_test_active && app->sleep_audio_active;
    if (app->alert_audio_active || stop_sleep) {
        app->alert_audio_active = 0;
        if (stop_sleep) app->sleep_audio_active = 0;
        kill_all_audio(app);
        app->settings_test_started_ms = 0;
        app->settings_sleep_test_active = 0;
    }
}


static void settings_field_rect(int row, int *x, int *y, int *w, int *h) {
    *x = SET_PAD; *w = g_config.cfg_w - 2*SET_PAD;
    *y = SET_PAD + 50 + row * SET_FIELD_ROW_H;
    *h = SET_FIELD_ROW_H - 10;
}


/* `reserve` shrinks the track from the right to leave room for a
 * per-field preview button (see settings_field_test_btn_rect) —
 * passed consistently by every caller (draw/click/drag) so the track
 * and the button never disagree about where the slider actually is. */
static void settings_slider_track_rect(int fx, int fy, int fw, int fh, int reserve,
                                        int *tx, int *ty, int *tw, int *th_) {
    *tx = fx; *tw = fw - reserve; *ty = fy + fh - 14; *th_ = 8;
}


#define SET_TEST_BTN_RESERVE 34
#define SET_TEST_BTN_W       26
#define SET_TEST_BTN_H       18

static void settings_field_test_btn_rect(int fx, int fy, int fw, int fhh,
                                          int *x, int *y, int *w, int *h) {
    int tx, ty, tw, th2;
    settings_slider_track_rect(fx, fy, fw, fhh, 0, &tx, &ty, &tw, &th2);
    (void)tx; (void)tw;
    *w = SET_TEST_BTN_W; *h = SET_TEST_BTN_H;
    *x = fx + fw - *w;
    *y = ty + th2/2 - *h/2;
}


/* Every field in Alarm/Sleep is either a *_percent system-volume level
 * or a timing/tempo shape parameter — the two get different preview
 * behavior (see settings_handle_click), so tell them apart by key. */
static int settings_field_is_volume(const CfgField *cf) {
    return strstr(cf->key, "volume") != NULL;
}


static void settings_draw_play_icon(App *app, Drawable buf, unsigned long rgb,
                                     int cx, int cy, int size) {
    XPoint pts[3] = {
        { (short)(cx - size/3), (short)(cy - size/2) },
        { (short)(cx - size/3), (short)(cy + size/2) },
        { (short)(cx + size/2), (short)cy },
    };
    XSetForeground(app->dpy, app->gc, rgb_pixel(app, rgb));
    XFillPolygon(app->dpy, buf, app->gc, pts, 3, Convex, CoordModeOrigin);
}


static void settings_draw_stop_icon(App *app, Drawable buf, unsigned long rgb,
                                     int cx, int cy, int size) {
    XSetForeground(app->dpy, app->gc, rgb_pixel(app, rgb));
    XFillRectangle(app->dpy, buf, app->gc, cx - size/2, cy - size/2, size, size);
}


/* Timing fields (Ctrl+G > Timing) are millisecond tick intervals —
 * shown/edited as free text ("1m200ms", "500ms", "2s", or a bare
 * number for milliseconds) instead of a slider, since the useful
 * values don't sit on a nice linear scale a drag can hit precisely. */
static int settings_field_is_duration(const CfgField *cf) {
    return !strcmp(cf->category, "Timing");
}


static void settings_duration_rect(int fx, int fy, int fw, int fh,
                                    int *x, int *y, int *w, int *h) {
    *x = fx; *w = fw; *y = fy + 20; *h = fh - 22;
}


static void settings_commit_edit(App *app) {
    if (app->settings_edit_field < 0) return;
    double ms;
    if (parse_duration_ms(app->settings_edit_buf, &ms)) {
        cfg_field_set(app->settings_edit_field, ms);
        config_save();
    }
    app->settings_edit_field = -1;
}


static void settings_toggle_rect(int fx, int fy, int fw, int fh,
                                  int *tx, int *ty, int *tw, int *th_) {
    *tw = 60; *th_ = 24; *tx = fx + fw - *tw; *ty = fy + (fh - *th_) / 2;
}

/* Returns the category currently drilled into, or NULL if the
 * category index is out of range (shouldn't happen in practice). */

static const char *settings_current_category(App *app) {
    const char *cats[8];
    int n = settings_list_categories(cats);
    if (app->settings_cat < 0 || app->settings_cat >= n) return NULL;
    return cats[app->settings_cat];
}


void draw_settings(App *app) {
    if (!app->cfg_win) return;
    ensure_cfg_buf(app);
    const Theme *th = cfg_theme(app);
    int W = g_config.cfg_w, H = g_config.cfg_h;
    Drawable buf  = app->cfg_buf ? (Drawable)app->cfg_buf : (Drawable)app->cfg_win;
    XftDraw *draw = app->cfg_buf ? app->cfg_buf_xft : app->cfg_xft;
    if (!draw) return;

    cfg_draw_cyber_bg(app, buf, th, 0, 0, W, H, monotonic_now());

    XftFont *fh = get_font(app, 14, 1);
    XftFont *fm = get_font(app, 10, 1);
    XftFont *fs = get_font(app, 9, 0);

    int pad = SET_PAD;
    if (fh && app->settings_cat < 0) xft_draw_string(app, draw, fh, th->accent, pad, pad + 14, "SETTINGS");

    if (app->settings_cat < 0) {
        int cx, cy, cw, ch; settings_close_rect(&cx, &cy, &cw, &ch);
        fill_rect(app, buf, th->surface, cx, cy, cw, ch);
        draw_rect_outline(app, buf, th->border, cx, cy, cw, ch);
        if (fs) {
            const char *lbl = "Close";
            int lw = text_width(app, fs, lbl);
            xft_draw_string(app, draw, fs, th->text,
                            cx + (cw - lw)/2, cy + (ch + fs->ascent - fs->descent)/2, lbl);
        }

        const char *cats[8];
        int n = settings_list_categories(cats);
        for (int i = 0; i < n; i++) {
            int x, y, w, h; settings_cat_rect(i, &x, &y, &w, &h);
            fill_rect(app, buf, th->surface, x, y, w, h);
            draw_rect_outline(app, buf, th->border, x, y, w, h);
            if (fm) xft_draw_string(app, draw, fm, th->text, x + 14,
                                    y + (h + fm->ascent - fm->descent)/2, cats[i]);
            if (fs) xft_draw_string(app, draw, fs, th->text_muted, x + w - 20,
                                    y + (h + fs->ascent - fs->descent)/2, ">");
        }
        if (app->cfg_buf)
            XCopyArea(app->dpy, app->cfg_buf, app->cfg_win, app->gc, 0, 0, W, H, 0, 0);
        XFlush(app->dpy);
        return;
    }

    const char *cat = settings_current_category(app);
    if (!cat) { app->settings_cat = -1; return; }

    int bx, by, bw, bh; settings_back_rect(&bx, &by, &bw, &bh);
    fill_rect(app, buf, th->surface, bx, by, bw, bh);
    draw_rect_outline(app, buf, th->border, bx, by, bw, bh);
    if (fs) xft_draw_string(app, draw, fs, th->text, bx + 10,
                            by + (bh + fs->ascent - fs->descent)/2, "< Back");
    if (fm) xft_draw_string(app, draw, fm, th->accent, bx + bw + 16,
                            by + (bh + fm->ascent - fm->descent)/2, cat);

    if (fs) {
        const char *hint = "each option previews itself";
        xft_draw_string(app, draw, fs, th->text_muted,
                        W - pad - text_width(app, fs, hint), pad + 12, hint);
    }

    int idxs[32];
    int nf = settings_fields_in_category(cat, idxs);
    for (int r = 0; r < nf; r++) {
        int fx, fy, fw, fhh; settings_field_rect(r, &fx, &fy, &fw, &fhh);
        if (fy > H - 20) break;
        const CfgField *cf = &CFG_FIELDS[idxs[r]];
        double v = cfg_field_get(idxs[r]);

        if (fs) xft_draw_string(app, draw, fs, th->text, fx, fy + 12, cf->label);

        if (cf->type == CFG_F_BOOL) {
            int tx, ty, tw, th2; settings_toggle_rect(fx, fy, fw, fhh, &tx, &ty, &tw, &th2);
            int on = v > 0.5;
            fill_rect(app, buf, on ? th->accent : th->surface_soft, tx, ty, tw, th2);
            draw_rect_outline(app, buf, th->border, tx, ty, tw, th2);
            if (fs) {
                const char *lbl = on ? "ON" : "OFF";
                int tw2 = text_width(app, fs, lbl);
                xft_draw_string(app, draw, fs, on ? 0x0a0a0aUL : th->text_muted,
                                tx + (tw - tw2)/2, ty + (th2 + fs->ascent - fs->descent)/2, lbl);
            }
        } else if (settings_field_is_duration(cf)) {
            int dx, dy, dw, dh; settings_duration_rect(fx, fy, fw, fhh, &dx, &dy, &dw, &dh);
            int editing = (app->settings_edit_field == idxs[r]);
            fill_rect(app, buf, th->surface_soft, dx, dy, dw, dh);
            draw_rect_outline(app, buf, editing ? th->accent : th->border, dx, dy, dw, dh);
            if (fs) {
                int ty2 = dy + (dh + fs->ascent - fs->descent)/2;
                if (editing) {
                    xft_draw_string(app, draw, fs, th->text, dx + 10, ty2, app->settings_edit_buf);
                    if (app->cursor_blink) {
                        int cp = app->settings_edit_cursor;
                        if (cp > app->settings_edit_len) cp = app->settings_edit_len;
                        char before[32];
                        int n = cp < (int)sizeof(before)-1 ? cp : (int)sizeof(before)-1;
                        memcpy(before, app->settings_edit_buf, (size_t)n); before[n] = '\0';
                        int cx = dx + 10 + text_width(app, fs, before);
                        XSetForeground(app->dpy, app->gc, rgb_pixel(app, th->text));
                        XFillRectangle(app->dpy, buf, app->gc, cx, dy + 4, 2, dh - 8);
                    }
                } else {
                    char shown[48];
                    format_duration_ms(v, shown, sizeof(shown));
                    xft_draw_string(app, draw, fs, th->text_muted, dx + 10, ty2, shown);
                    const char *hint = "click to edit";
                    xft_draw_string(app, draw, fs, th->border,
                                    dx + dw - 10 - text_width(app, fs, hint), ty2, hint);
                }
            }
        } else {
            char valbuf[32];
            if (cf->type == CFG_F_DOUBLE) snprintf(valbuf, sizeof(valbuf), "%.2f", v);
            else                          snprintf(valbuf, sizeof(valbuf), "%d", (int)v);
            if (fs) xft_draw_string(app, draw, fs, th->text_muted,
                                    fx + fw - text_width(app, fs, valbuf), fy + 12, valbuf);

            int has_test = settings_cat_is_testable(cat);
            int tx, ty, tw, th2;
            settings_slider_track_rect(fx, fy, fw, fhh, has_test ? SET_TEST_BTN_RESERVE : 0,
                                       &tx, &ty, &tw, &th2);
            fill_rect(app, buf, th->surface_soft, tx, ty, tw, th2);
            draw_rect_outline(app, buf, th->border, tx, ty, tw, th2);
            double frac = (cf->max > cf->min) ? (v - cf->min) / (cf->max - cf->min) : 0.0;
            if (frac < 0) frac = 0;
            if (frac > 1) frac = 1;
            int fillw = (int)(tw * frac);
            if (fillw > 0) fill_rect(app, buf, th->accent, tx, ty, fillw, th2);

            if (has_test) {
                int bx2, by2, bw2, bh2; settings_field_test_btn_rect(fx, fy, fw, fhh, &bx2, &by2, &bw2, &bh2);
                int is_vol = settings_field_is_volume(cf);
                int playing = !is_vol && settings_test_is_playing(app, cat);
                fill_rect(app, buf, playing ? th->danger : th->accent_soft, bx2, by2, bw2, bh2);
                draw_rect_outline(app, buf, playing ? th->danger : th->accent, bx2, by2, bw2, bh2);
                int icx = bx2 + bw2/2, icy = by2 + bh2/2;
                if (playing) settings_draw_stop_icon(app, buf, 0xf0e8d8UL, icx, icy, 8);
                else         settings_draw_play_icon(app, buf, th->accent, icx, icy, 10);
            }
        }
    }

    if (app->cfg_buf)
        XCopyArea(app->dpy, app->cfg_buf, app->cfg_win, app->gc, 0, 0, W, H, 0, 0);
    XFlush(app->dpy);
}


int settings_handle_click(App *app, int mx, int my, int is_press) {
    if (!is_press) return 0;

    /* Any click commits whatever duration text box was being edited —
     * including a click that re-opens the same box, which is harmless
     * (it just round-trips the same value before starting a fresh edit). */
    settings_commit_edit(app);

    if (app->settings_cat < 0) {
        int cx, cy, cw, ch; settings_close_rect(&cx, &cy, &cw, &ch);
        if (mx >= cx && mx < cx+cw && my >= cy && my < cy+ch) {
            settings_close_overlay(app);
            return 1;
        }
        const char *cats[8];
        int n = settings_list_categories(cats);
        for (int i = 0; i < n; i++) {
            int x, y, w, h; settings_cat_rect(i, &x, &y, &w, &h);
            if (mx >= x && mx < x+w && my >= y && my < y+h) {
                app->settings_cat = i;
                draw_settings(app);
                return 1;
            }
        }
        return 1;
    }

    int bx, by, bw, bh; settings_back_rect(&bx, &by, &bw, &bh);
    if (mx >= bx && mx < bx+bw && my >= by && my < by+bh) {
        /* Any preview still running belongs to the category we're
         * leaving — stop it so audio never keeps playing behind the
         * scenes once its controls are out of view. */
        settings_stop_any_test(app);
        app->settings_cat = -1;
        draw_settings(app);
        return 1;
    }

    const char *cat = settings_current_category(app);
    if (!cat) return 1;

    int idxs[32];
    int nf = settings_fields_in_category(cat, idxs);
    for (int r = 0; r < nf; r++) {
        int fx, fy, fw, fhh; settings_field_rect(r, &fx, &fy, &fw, &fhh);
        if (my < fy || my >= fy+fhh) continue;
        const CfgField *cf = &CFG_FIELDS[idxs[r]];
        if (cf->type == CFG_F_BOOL) {
            int tx, ty, tw, th2; settings_toggle_rect(fx, fy, fw, fhh, &tx, &ty, &tw, &th2);
            if (mx >= tx && mx < tx+tw && my >= ty && my < ty+th2) {
                double v = cfg_field_get(idxs[r]);
                cfg_field_set(idxs[r], v > 0.5 ? 0 : 1);
                config_save();
                draw_settings(app);
                return 1;
            }
        } else if (settings_field_is_duration(cf)) {
            int dx, dy, dw, dh; settings_duration_rect(fx, fy, fw, fhh, &dx, &dy, &dw, &dh);
            if (mx >= dx && mx < dx+dw && my >= dy && my < dy+dh) {
                app->settings_edit_field = idxs[r];
                format_duration_ms(cfg_field_get(idxs[r]), app->settings_edit_buf,
                                   sizeof(app->settings_edit_buf));
                app->settings_edit_len    = (int)strlen(app->settings_edit_buf);
                app->settings_edit_cursor = app->settings_edit_len;
                app->cursor_blink = 1; app->cursor_blink_ts = ms_now();
                draw_settings(app);
                return 1;
            }
        } else {
            int has_test = settings_cat_is_testable(cat);
            if (has_test) {
                int bx2, by2, bw2, bh2; settings_field_test_btn_rect(fx, fy, fw, fhh, &bx2, &by2, &bw2, &bh2);
                if (mx >= bx2 && mx < bx2+bw2 && my >= by2 && my < by2+bh2) {
                    /* Isolated preview: this button's click always stops
                     * whatever else was playing first (a different
                     * field's test, or the same one — clicking again
                     * while it's running just stops it), so exactly one
                     * option is ever auditioned at a time. */
                    int playing = settings_test_is_playing(app, cat);
                    settings_stop_any_test(app);
                    if (!playing) {
                        if (settings_field_is_volume(cf)) {
                            set_system_volume((int)cfg_field_get(idxs[r]));
                            play_cue(app, "start");
                        } else if (!strcmp(cat, "Alarm")) {
                            play_alert_sound(app);
                            app->settings_test_started_ms = ms_now();
                        } else {
                            SleepPreviewData *d = sleep_build_preview(90.0);
                            play_sleep_audition(app, d);
                            free(d);
                            app->settings_sleep_test_active = 1;
                        }
                    }
                    draw_settings(app);
                    return 1;
                }
            }

            int tx, ty, tw, th2;
            settings_slider_track_rect(fx, fy, fw, fhh, has_test ? SET_TEST_BTN_RESERVE : 0,
                                       &tx, &ty, &tw, &th2);
            if (mx >= tx-6 && mx < tx+tw+6 && my >= ty-10 && my < ty+th2+10) {
                double frac = tw > 0 ? (double)(mx - tx) / (double)tw : 0.0;
                if (frac < 0) frac = 0;
                if (frac > 1) frac = 1;
                cfg_field_set(idxs[r], cf->min + frac * (cf->max - cf->min));
                config_save();
                app->settings_drag_field = idxs[r];
                draw_settings(app);
                return 1;
            }
        }
        break;
    }
    return 1;
}


int settings_handle_motion(App *app, int mx, int my) {
    if (app->settings_drag_field < 0) return 0;
    const char *cat = settings_current_category(app);
    if (!cat) return 0;
    int idxs[32];
    int nf = settings_fields_in_category(cat, idxs);
    for (int r = 0; r < nf; r++) {
        if (idxs[r] != app->settings_drag_field) continue;
        const CfgField *cf = &CFG_FIELDS[idxs[r]];
        int fx, fy, fw, fhh; settings_field_rect(r, &fx, &fy, &fw, &fhh);
        int has_test = settings_cat_is_testable(cat);
        int tx, ty, tw, th2;
        settings_slider_track_rect(fx, fy, fw, fhh, has_test ? SET_TEST_BTN_RESERVE : 0,
                                   &tx, &ty, &tw, &th2);
        (void)ty; (void)th2;
        double frac = tw > 0 ? (double)(mx - tx) / (double)tw : 0.0;
        if (frac < 0) frac = 0;
        if (frac > 1) frac = 1;
        cfg_field_set(app->settings_drag_field, cf->min + frac * (cf->max - cf->min));
        config_save();
        draw_settings(app);
        return 1;
    }
    return 0;
}


void settings_handle_release(App *app) {
    app->settings_drag_field = -1;
}


void settings_handle_key(App *app, XKeyEvent *ev) {
    char buf[32] = ""; KeySym ks;
    XLookupString(ev, buf, sizeof(buf)-1, &ks, NULL);

    if (app->settings_edit_field >= 0) {
        int *len = &app->settings_edit_len, *cp = &app->settings_edit_cursor;
        char *eb = app->settings_edit_buf;
        if (ks == XK_Return || ks == XK_KP_Enter) {
            settings_commit_edit(app);
            draw_settings(app);
            return;
        }
        if (ks == XK_Escape) {
            app->settings_edit_field = -1; /* cancel — no back-navigation here */
            draw_settings(app);
            return;
        }
        if (ks == XK_BackSpace) {
            if (*cp > 0) {
                memmove(eb + *cp - 1, eb + *cp, (size_t)(*len - *cp + 1));
                (*cp)--; (*len)--;
            }
        } else if (ks == XK_Delete) {
            if (*cp < *len) {
                memmove(eb + *cp, eb + *cp + 1, (size_t)(*len - *cp));
                (*len)--;
            }
        } else if (ks == XK_Left)  { if (*cp > 0) (*cp)--; }
        else if (ks == XK_Right) { if (*cp < *len) (*cp)++; }
        else if (ks == XK_Home)  { *cp = 0; }
        else if (ks == XK_End)   { *cp = *len; }
        else if (buf[0] && isprint((unsigned char)buf[0]) && !(ev->state & ControlMask)) {
            int ch = (int)strlen(buf);
            if (*len + ch < (int)sizeof(app->settings_edit_buf) - 1) {
                memmove(eb + *cp + ch, eb + *cp, (size_t)(*len - *cp + 1));
                memcpy(eb + *cp, buf, (size_t)ch);
                *cp += ch; *len += ch;
            }
        }
        app->cursor_blink = 1; app->cursor_blink_ts = ms_now();
        draw_settings(app);
        return;
    }

    /* Navigation is button-only here (Close / < Back, see draw_settings())
     * — no Esc-to-back or Ctrl+G-to-close shortcuts, so the on-screen
     * buttons are always the one true way in or out. */
    (void)ks;
}

/* Opens the Settings overlay. When `from_timer` is set (Ctrl+G pressed
 * while a timer is running/paused/alerting), cfg_win — withdrawn but
 * never destroyed while a timer runs, see start_running()/do_quit() in
 * timer.c — is repositioned over the running timer window and mapped as
 * a true overlay; closing it withdraws it again rather than leaving a
 * second window sitting on screen. */

void settings_open_overlay(App *app, int from_timer) {
    app->settings_open = 1;
    app->settings_cat = -1;
    app->settings_drag_field = -1;
    app->settings_edit_field = -1;
    app->settings_sleep_test_active = 0;
    app->settings_from_timer = from_timer;
    if (from_timer && app->cfg_win) {
        int x = app->tmr_x + app->tmr_w/2 - g_config.cfg_w/2;
        int y = app->tmr_y + app->tmr_h/2 - g_config.cfg_h/2;
        XMoveResizeWindow(app->dpy, app->cfg_win, x, y, g_config.cfg_w, g_config.cfg_h);
        XMapRaised(app->dpy, app->cfg_win);
        XSetInputFocus(app->dpy, app->cfg_win, RevertToPointerRoot, CurrentTime);
    }
    draw_settings(app);
}


void settings_close_overlay(App *app) {
    /* alert_audio_active can only be set here by a field "test" button (a
     * real alarm never plays while Settings is reachable — see
     * settings_open_overlay's callers) so it's always safe to cut it off
     * on close; sleep_audio_active is only cut off when it was this
     * overlay's own test preview (settings_sleep_test_active) — otherwise
     * it may be real ambient audio for a running timer underneath. */
    if (app->alert_audio_active || (app->settings_sleep_test_active && app->sleep_audio_active)) {
        app->alert_audio_active = 0;
        if (app->settings_sleep_test_active) app->sleep_audio_active = 0;
        kill_all_audio(app);
        app->settings_test_started_ms = 0;
        app->settings_sleep_test_active = 0;
    }
    app->settings_open = 0;
    app->settings_drag_field = -1;
    app->settings_edit_field = -1;
    app->settings_cat = -1;
    if (app->settings_from_timer && app->cfg_win) {
        XWithdrawWindow(app->dpy, app->cfg_win, app->screen);
        if (app->tmr_win) XSetInputFocus(app->dpy, app->tmr_win, RevertToPointerRoot, CurrentTime);
    } else {
        draw_config(app);
    }
    app->settings_from_timer = 0;
}
