/* timer.c — App init, event handling, state machine, main loop */
#include "timer.h"

/* ── Global for signal handler ─────────────────────────── */
static App *g_app = NULL;

/* ── Logging ───────────────────────────────────────────── */

void app_log(App *app, const char *level, const char *fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    time_t t = time(NULL);
    struct tm *tm_p = localtime(&t);
    char ts[32];
    strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", tm_p);
    if (app && app->logfp) {
        fprintf(app->logfp, "%s [%s] %s\n", ts, level, buf);
        fflush(app->logfp);
    }
    fprintf(stderr, "%s [%s] %s\n", ts, level, buf);
}

/* ── Monitor detection (XRandR) ────────────────────────── */

void detect_monitor(App *app, int ptr_x, int ptr_y) {
    int fallback_w = DisplayWidth(app->dpy, app->screen);
    int fallback_h = DisplayHeight(app->dpy, app->screen);
    app->mon_x = 0; app->mon_y = 0;
    app->mon_w = fallback_w;
    app->mon_h = fallback_h;

    int xin_event_base, xin_error_base;
    if (XineramaQueryExtension(app->dpy, &xin_event_base, &xin_error_base) &&
        XineramaIsActive(app->dpy)) {
        int nscreen = 0;
        XineramaScreenInfo *screens = XineramaQueryScreens(app->dpy, &nscreen);
        if (screens && nscreen > 0) {
            int best_x = 0, best_y = 0, best_w = 0, best_h = 0;
            long long best_dist = 0;
            int have_best = 0;

            for (int i = 0; i < nscreen; i++) {
                int x = screens[i].x_org, y = screens[i].y_org;
                int w = screens[i].width, h = screens[i].height;
                if (w <= 0 || h <= 0) continue;
                if (ptr_x >= x && ptr_x < x+w && ptr_y >= y && ptr_y < y+h) {
                    app->mon_x = x; app->mon_y = y;
                    app->mon_w = w; app->mon_h = h;
                    XFree(screens);
                    return;
                }

                long long cx = x + w / 2;
                long long cy = y + h / 2;
                long long dx = (long long)ptr_x - cx;
                long long dy = (long long)ptr_y - cy;
                long long dist = dx * dx + dy * dy;
                if (!have_best || dist < best_dist) {
                    best_x = x; best_y = y; best_w = w; best_h = h;
                    best_dist = dist;
                    have_best = 1;
                }
            }

            if (have_best) {
                app->mon_x = best_x; app->mon_y = best_y;
                app->mon_w = best_w; app->mon_h = best_h;
                XFree(screens);
                return;
            }
        }
        if (screens) XFree(screens);
    }

    int event_base, error_base;
    if (!XRRQueryExtension(app->dpy, &event_base, &error_base)) return;

    Window root = DefaultRootWindow(app->dpy);

    XRRScreenResources *res = XRRGetScreenResourcesCurrent(app->dpy, root);
    if (!res) return;

    int best_x = 0, best_y = 0, best_w = 0, best_h = 0;
    long long best_dist = 0;
    int have_best = 0;

    for (int i = 0; i < res->ncrtc; i++) {
        XRRCrtcInfo *ci = XRRGetCrtcInfo(app->dpy, res, res->crtcs[i]);
        if (!ci) continue;
        if (ci->width > 0 && ci->height > 0) {
            int x = ci->x, y = ci->y, w = (int)ci->width, h = (int)ci->height;
            if (ptr_x >= x && ptr_x < x+w && ptr_y >= y && ptr_y < y+h) {
                app->mon_x = x; app->mon_y = y;
                app->mon_w = w; app->mon_h = h;
                XRRFreeCrtcInfo(ci);
                XRRFreeScreenResources(res);
                return;
            }

            long long cx = x + w / 2;
            long long cy = y + h / 2;
            long long dx = (long long)ptr_x - cx;
            long long dy = (long long)ptr_y - cy;
            long long dist = dx * dx + dy * dy;
            if (!have_best || dist < best_dist) {
                best_x = x; best_y = y; best_w = w; best_h = h;
                best_dist = dist;
                have_best = 1;
            }
        }
        XRRFreeCrtcInfo(ci);
    }

    if (have_best) {
        app->mon_x = best_x; app->mon_y = best_y;
        app->mon_w = best_w; app->mon_h = best_h;
    }
    XRRFreeScreenResources(res);
}

/* ── State transitions ─────────────────────────────────── */

static void do_quit(App *app) {
    app_log(app, "INFO", "Quitting");
    app->alert_audio_active = 0;
    /* Only forcibly kill audio when leaving an alert — cue sounds are short
     * and should play to completion after windows are gone. */
    if (app->state == STATE_ALERT) kill_all_audio(app);
    db_finish_session(app, "stopped");
    destroy_alert_windows(app);
    destroy_timer_window(app);
    if (app->cfg_buf_xft) { XftDrawDestroy(app->cfg_buf_xft); app->cfg_buf_xft = NULL; }
    if (app->cfg_buf)     { XFreePixmap(app->dpy, app->cfg_buf); app->cfg_buf = 0; }
    if (app->cfg_xft)     { XftDrawDestroy(app->cfg_xft); app->cfg_xft = NULL; }
    if (app->cfg_win)     { XDestroyWindow(app->dpy, app->cfg_win); app->cfg_win = 0; }
    XFlush(app->dpy);
    app->state = STATE_DONE;
}

static void dismiss_alert(App *app) {
    app_log(app, "INFO", "Alert dismissed");
    app->alert_audio_active = 0;
    kill_all_audio(app);   /* stop the looping alert siren immediately */
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
    app->tmr_last_draw_ms = 0;
    app->tmr_force_redraw = 1;

    /* Use config window center so timer appears on the right monitor */
    if (app->cfg_win)
        detect_monitor(app, app->cfg_x + CFG_W/2, app->cfg_y + CFG_H/2);

    create_timer_window(app);
    db_start_session(app);
    app_log(app, "INFO", "Timer started: %ds, task='%s', mode=%s, mini=%d",
            app->total_seconds, app->task,
            app->mode == MODE_SHUTDOWN ? "shutdown" : "alert", app->mini);

    if (app->cfg_win) XWithdrawWindow(app->dpy, app->cfg_win, app->screen);
    XFlush(app->dpy);
    play_cue(app, "start");
}

static void toggle_mini_fullscreen(App *app) {
    if (app->mini) {
        app->saved_mini_x = app->tmr_x; app->saved_mini_y = app->tmr_y;
        app->saved_mini_w = app->tmr_w; app->saved_mini_h = app->tmr_h;
        app->has_saved_mini = 1;
        detect_monitor(app, app->tmr_x + app->tmr_w/2, app->tmr_y + app->tmr_h/2);
        app->mini = 0;
    } else {
        Window r2, c2; int rx = app->mon_x+app->mon_w/2, ry = app->mon_y+app->mon_h/2;
        int wx, wy; unsigned int msk;
        XQueryPointer(app->dpy, DefaultRootWindow(app->dpy), &r2, &c2, &rx, &ry, &wx, &wy, &msk);
        detect_monitor(app, rx, ry);
        app->mini = 1;
    }
    app_log(app, "INFO", "Toggled to %s mode", app->mini ? "mini" : "fullscreen");
    create_timer_window(app);
    app->tmr_force_redraw = 1;
    play_cue(app, app->mini ? "pause" : "resume");
}

static void toggle_pause(App *app) {
    if (!app->pausable) return;
    if (app->state == STATE_RUNNING) {
        app->state = STATE_PAUSED;
        clock_gettime(CLOCK_MONOTONIC, &app->pause_start_ts);
        play_cue(app, "pause");
        app_log(app, "INFO", "Timer paused");
        app->tmr_force_redraw = 1;
    } else if (app->state == STATE_PAUSED) {
        struct timespec now_ts;
        clock_gettime(CLOCK_MONOTONIC, &now_ts);
        double p = (now_ts.tv_sec  - app->pause_start_ts.tv_sec) +
                   (now_ts.tv_nsec - app->pause_start_ts.tv_nsec) * 1e-9;
        app->pause_total += p;
        app->state = STATE_RUNNING;
        play_cue(app, "resume");
        app_log(app, "INFO", "Timer resumed");
        app->tmr_force_redraw = 1;
    }
}

static void run_cmd_shutdown(const char *cmd) { int r = system(cmd); (void)r; }

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
        run_cmd_shutdown("systemctl poweroff");
        do_quit(app);
    } else {
        play_cue(app, "alert_open");
        create_alert_windows(app);
        app->state = STATE_ALERT;
        play_alert_sound(app);
    }
}

/* ── Timer tick ────────────────────────────────────────── */

static void maybe_play_cues(App *app, int prev_rem) {
    if (app->total_seconds <= 0) return;
    if (!app->cue_half_played) {
        int half = app->total_seconds / 2;
        if (prev_rem > half && app->remaining_seconds <= half) {
            app->cue_half_played = 1;
            play_cue(app, "half");
        }
    }
    for (int i = 0; i < 3; i++) {
        int s = 3 - i;
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

    long now_ms = ms_now();
    if (now_ms - app->cursor_blink_ts > 500) {
        app->cursor_blink = !app->cursor_blink;
        app->cursor_blink_ts = now_ms;
    }
    /* blink_on/blink_ts retained in struct but icon now uses smooth pulse */
    (void)app->blink_on;

    if (app->state == STATE_RUNNING) {
        maybe_play_cues(app, prev_rem);
        if (app->remaining_seconds <= 0 && app->remaining_exact <= 0.0) {
            timer_finished(app);
            return;
        }
    }

    /* Drawing is the expensive part. Throttle redraws unless the value
     * changed or an explicit redraw was requested. */
    long draw_now_ms = now_ms;
    int needs_draw = 0;
    if (app->tmr_force_redraw) needs_draw = 1;
    if (app->remaining_seconds != prev_rem) needs_draw = 1;

    long min_interval = (app->state == STATE_RUNNING) ? TICK_TIMER_RUN_MS : TICK_TIMER_PAUSED_MS;
    if (!needs_draw && (draw_now_ms - app->tmr_last_draw_ms) >= min_interval) needs_draw = 1;

    if (needs_draw) {
        draw_timer(app);
        app->tmr_last_draw_ms = draw_now_ms;
        app->tmr_force_redraw = 0;
    }
}

/* ── Event handling ────────────────────────────────────── */

static void handle_config_key(App *app, XKeyEvent *ev) {
    char buf[32] = "";
    KeySym ks;
    XLookupString(ev, buf, sizeof(buf)-1, &ks, NULL);

    /* Tab (or Ctrl+T) cycles through themes */
    if (ks == XK_Tab ||
        ((ev->state & ControlMask) && (ks == XK_t || ks == XK_T))) {
        app->theme_idx = (app->theme_idx + 1) % N_THEMES;
        app_log(app, "INFO", "Theme: %s", THEMES[app->theme_idx].name);
        play_cue(app, "resume");
        draw_config(app);
        return;
    }
    if ((ev->state & ControlMask) && (ks == XK_a || ks == XK_A)) {
        app->cursor_pos = app->input_len;
        app->cursor_blink = 1; app->cursor_blink_ts = ms_now();
        draw_config(app); return;
    }

    if (ks == XK_Escape) {
        play_cue(app, "close");
        do_quit(app);
        return;
    }

    if (ks == XK_Return || ks == XK_KP_Enter) {
        ParseResult pr;
        parse_time_input(app->input, &pr);
        if (pr.seconds <= 0) {
            app_log(app, "WARN", "Invalid time input: '%s'", app->input);
            return;
        }
        app->total_seconds     = pr.seconds;
        app->remaining_seconds = pr.seconds;
        app->remaining_exact   = (double)pr.seconds;
        snprintf(app->task,      sizeof(app->task),      "%s", pr.task);
        snprintf(app->until_str, sizeof(app->until_str), "%s", pr.until);
        if (pr.sw_shutdown) app->mode = MODE_SHUTDOWN; else app->mode = MODE_ALERT;
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
            app->cursor_pos--; app->input_len--;
        }
    } else if (ks == XK_Delete) {
        if (app->cursor_pos < app->input_len) {
            memmove(app->input + app->cursor_pos,
                    app->input + app->cursor_pos + 1,
                    app->input_len - app->cursor_pos);
            app->input_len--;
        }
    } else if (ks == XK_Left)  { if (app->cursor_pos > 0) app->cursor_pos--; }
    else if (ks == XK_Right) { if (app->cursor_pos < app->input_len) app->cursor_pos++; }
    else if (ks == XK_Home)  { app->cursor_pos = 0; }
    else if (ks == XK_End)   { app->cursor_pos = app->input_len; }
    else if (buf[0] && isprint((unsigned char)buf[0]) && !(ev->state & ControlMask)) {
        int ch = (int)strlen(buf);
        if (app->input_len + ch < MAX_INPUT - 1) {
            memmove(app->input + app->cursor_pos + ch,
                    app->input + app->cursor_pos,
                    app->input_len - app->cursor_pos + 1);
            memcpy(app->input + app->cursor_pos, buf, ch);
            app->cursor_pos += ch; app->input_len += ch;
        }
    }
    app->cursor_blink = 1; app->cursor_blink_ts = ms_now();
    draw_config(app);
}

static void handle_event(App *app, XEvent *ev) {
    /* Config window */
    if (app->cfg_win && ev->xany.window == app->cfg_win) {
        switch (ev->type) {
        case Expose:
            if (ev->xexpose.count == 0) draw_config(app);
            break;
        case MapNotify:
            /* Window is now viewable — safe to grab keyboard focus. */
            app->cfg_focused = 1;
            XSetInputFocus(app->dpy, app->cfg_win, RevertToPointerRoot, CurrentTime);
            draw_config(app);
            break;
        case FocusIn:
            /* Accept all real FocusIn events.
             * Exclude detail=NotifyPointer/Root which are virtual pointer events,
             * not a real keyboard-focus grant. */
            if (ev->xfocus.detail != NotifyPointer &&
                ev->xfocus.detail != NotifyPointerRoot) {
                if (!app->cfg_focused) { app->cfg_focused = 1; draw_config(app); }
            }
            break;
        case FocusOut:
            /* Go grey on any real focus departure.
             * Exclude: child-window virtuals (Inferior), pointer virtuals,
             * and the transient release after a grab (NotifyUngrab). */
            if (ev->xfocus.detail != NotifyInferior      &&
                ev->xfocus.detail != NotifyPointer        &&
                ev->xfocus.detail != NotifyPointerRoot    &&
                ev->xfocus.mode   != NotifyUngrab) {
                if (app->cfg_focused) { app->cfg_focused = 0; draw_config(app); }
            }
            break;
        case KeyPress:
            handle_config_key(app, &ev->xkey);
            break;
        case ButtonPress:
            /* Explicitly restore keyboard focus — override_redirect windows do
             * not receive it automatically when another window takes it. */
            XSetInputFocus(app->dpy, app->cfg_win, RevertToPointerRoot, CurrentTime);
            if (ev->xbutton.button == Button2) {
                play_cue(app, "close");
                do_quit(app);
                return;
            } else if (ev->xbutton.button == Button1) {
                app->cfg_is_dragging = 1;
                app->cfg_drag_px = ev->xbutton.x_root; app->cfg_drag_py = ev->xbutton.y_root;
                app->cfg_drag_wx = app->cfg_x;          app->cfg_drag_wy = app->cfg_y;
            } else if (ev->xbutton.button == Button3) {
                app->fullscreen_requested = 1; app->mini = 0;
            }
            break;
        case ButtonRelease: app->cfg_is_dragging = 0; break;
        case MotionNotify:
            if (app->cfg_is_dragging) {
                int dx = ev->xmotion.x_root - app->cfg_drag_px;
                int dy = ev->xmotion.y_root - app->cfg_drag_py;
                app->cfg_x = app->cfg_drag_wx + dx;
                app->cfg_y = app->cfg_drag_wy + dy;
                XMoveWindow(app->dpy, app->cfg_win, app->cfg_x, app->cfg_y);
            }
            break;
        case ClientMessage:
            if ((Atom)ev->xclient.data.l[0] == app->wm_delete) {
                play_cue(app, "close");
                do_quit(app);
            }
            break;
        }
        return;
    }

    /* Timer window */
    if (app->tmr_win && ev->xany.window == app->tmr_win) {
        switch (ev->type) {
        case Expose:
            if (ev->xexpose.count == 0) draw_timer(app);
            break;
        case KeyPress: {
            KeySym ks = XLookupKeysym(&ev->xkey, 0);
            if (ks == XK_space && app->pausable) toggle_pause(app);
            break;
        }
        case ButtonPress: {
            int bx = ev->xbutton.x, by = ev->xbutton.y;
            if (ev->xbutton.button == Button2) { stop_timer(app); return; }
            if (ev->xbutton.button == Button3) { toggle_mini_fullscreen(app); return; }
            if (ev->xbutton.button == Button1) {
                if (app->mini) {
                    if (bx >= app->tmr_w - RESIZE_ZONE && by >= app->tmr_h - RESIZE_ZONE) {
                        app->is_resizing = 1;
                        app->resize_px = ev->xbutton.x_root; app->resize_py = ev->xbutton.y_root;
                        app->resize_wx = app->tmr_x;         app->resize_wy = app->tmr_y;
                        app->resize_ww = app->tmr_w;         app->resize_wh = app->tmr_h;
                        return;
                    }
                }
                if (app->pausable) { toggle_pause(app); return; }
                app->is_dragging = 1;
                app->drag_px = ev->xbutton.x_root; app->drag_py = ev->xbutton.y_root;
                app->drag_wx = app->tmr_x;          app->drag_wy = app->tmr_y;
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
                if (nw < MINI_W_MIN) nw = MINI_W_MIN;
                if (nh < MINI_H_MIN) nh = MINI_H_MIN;
                if (nw != app->tmr_w || nh != app->tmr_h) {
                    app->tmr_w = nw; app->tmr_h = nh;
                    XResizeWindow(app->dpy, app->tmr_win, nw, nh);
                    /* Throttle: redraw at most 60 fps during resize.
                     * The pixmap buffer is reused if large enough (see ensure_tmr_buf),
                     * so most frames just re-render into the existing pixmap. */
                    static long last_resize_ms = 0;
                    long now_ms = ms_now();
                    if (now_ms - last_resize_ms >= 16) {
                        app->cd_font_sz = 0;   /* recompute font for new size */
                        draw_timer(app);
                        XSync(app->dpy, False); /* flush resize before next event */
                        last_resize_ms = now_ms;
                    }
                }
            } else if (app->mini) {
                int in_zone = (ev->xmotion.x >= app->tmr_w - RESIZE_ZONE &&
                               ev->xmotion.y >= app->tmr_h - RESIZE_ZONE);
                if (in_zone && !app->cursor_in_resize) {
                    XDefineCursor(app->dpy, app->tmr_win, app->cursor_resize);
                    app->cursor_in_resize = 1;
                } else if (!in_zone && app->cursor_in_resize) {
                    XDefineCursor(app->dpy, app->tmr_win, None);
                    app->cursor_in_resize = 0;
                }
            }
            break;
        case ClientMessage:
            if ((Atom)ev->xclient.data.l[0] == app->wm_delete) stop_timer(app);
            break;
        }
        return;
    }

    /* Alert windows */
    for (int i = 0; i < app->n_alert_screens; i++) {
        if (!app->alert_screens[i].win) continue;
        if (ev->xany.window != app->alert_screens[i].win) continue;
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

/* ── Config cursor blink tick ──────────────────────────── */

static void do_config_tick(App *app) {
    long now_ms = ms_now();
    int blink_toggled = 0;
    /* Cursor blink every 530 ms */
    if (now_ms - app->cursor_blink_ts > 530) {
        app->cursor_blink = !app->cursor_blink;
        app->cursor_blink_ts = now_ms;
        blink_toggled = 1;
    }

    /* Redraw only when needed: cursor blink flips or wall-clock second changes. */
    if (app->cfg_win && app->state == STATE_CONFIG) {
        time_t wall = time(NULL);
        int needs = 0;
        if (wall != app->cfg_last_wall_sec) { app->cfg_last_wall_sec = wall; needs = 1; }
        if (blink_toggled) needs = 1;
        if (needs) draw_config(app);
    }
}

/* ── Main event loop ───────────────────────────────────── */

static void main_loop(App *app) {
    int xfd = XConnectionNumber(app->dpy);
    while (app->state != STATE_DONE && !app->quit) {
        /* Avoid a tight render loop: block in select() and only wake for events
         * or a scheduled tick. */
        struct timeval tv;
        int tick_ms = 250;
        switch (app->state) {
        case STATE_CONFIG:  tick_ms = TICK_CONFIG_MS;        break;
        case STATE_RUNNING:
            tick_ms = (app->remaining_seconds <= 10) ? TICK_TIMER_LAST10_MS : TICK_TIMER_RUN_MS;
            break;
        case STATE_PAUSED:  tick_ms = TICK_TIMER_PAUSED_MS;  break;
        case STATE_ALERT:   tick_ms = TICK_ALERT_MS;         break;
        default:            tick_ms = 250;                   break;
        }
        tv.tv_sec  = tick_ms / 1000;
        tv.tv_usec = (tick_ms % 1000) * 1000;
        fd_set rfds; FD_ZERO(&rfds); FD_SET(xfd, &rfds);
        int nfds = select(xfd+1, &rfds, NULL, NULL, &tv);
        if (nfds < 0) { if (errno == EINTR) continue; break; }
        while (XPending(app->dpy)) {
            XEvent ev; XNextEvent(app->dpy, &ev);
            if (app->state == STATE_DONE) break;
            handle_event(app, &ev);
        }
        if (app->state == STATE_DONE) break;
        switch (app->state) {
        case STATE_CONFIG:  do_config_tick(app); break;
        case STATE_RUNNING:
        case STATE_PAUSED:  do_timer_tick(app);  break;
        case STATE_ALERT:   draw_alert(app);     break;
        default: break;
        }
    }
}

/* ── App initialization ────────────────────────────────── */

static void signal_handler(int sig) { (void)sig; if (g_app) g_app->quit = 1; }

static int app_init(App *app) {
    memset(app, 0, sizeof(*app));

    const char *home = getenv("HOME");
    if (!home) home = "/tmp";
    char data_dir[MAX_PATH_LEN-16];
    snprintf(data_dir, sizeof(data_dir), "%s/.local/share/timer_c", home);
    mkdir(data_dir, 0755);
    snprintf(app->log_path, sizeof(app->log_path), "%s/timer.log",    data_dir);
    snprintf(app->db_path,  sizeof(app->db_path),  "%s/sessions.db",  data_dir);
    app->logfp = fopen(app->log_path, "a");

    app->theme_idx  = rand() % N_THEMES;
    app->cfg_focused = 1;
    app->state      = STATE_CONFIG;
    app->mode       = MODE_ALERT;
    app->mini       = 1;
    app->pausable   = 0;
    app->session_id = 0;
    app->blink_on   = 1;
    app->blink_ts   = ms_now();
    app->quote_idx = -1;
    app->quote_prev_idx = -1;
    app->quote_cycle_ts = 0;
    app->cfg_last_wall_sec = 0;

    pthread_mutex_init(&app->audio_lock, NULL);

    app->dpy = XOpenDisplay(NULL);
    if (!app->dpy) { fprintf(stderr, "Cannot open X display\n"); return 0; }
    app->screen = DefaultScreen(app->dpy);
    app->vis    = DefaultVisual(app->dpy, app->screen);
    app->cmap   = DefaultColormap(app->dpy, app->screen);
    app->depth  = DefaultDepth(app->dpy, app->screen);

    app->wm_delete                 = XInternAtom(app->dpy, "WM_DELETE_WINDOW",          False);
    app->net_wm_state              = XInternAtom(app->dpy, "_NET_WM_STATE",              False);
    app->net_wm_state_above        = XInternAtom(app->dpy, "_NET_WM_STATE_ABOVE",        False);
    app->net_wm_state_fullscreen   = XInternAtom(app->dpy, "_NET_WM_STATE_FULLSCREEN",   False);
    app->net_wm_state_skip_taskbar = XInternAtom(app->dpy, "_NET_WM_STATE_SKIP_TASKBAR", False);
    app->net_wm_state_sticky       = XInternAtom(app->dpy, "_NET_WM_STATE_STICKY",       False);
    app->net_wm_desktop            = XInternAtom(app->dpy, "_NET_WM_DESKTOP",            False);
    app->net_wm_bypass_compositor  = XInternAtom(app->dpy, "_NET_WM_BYPASS_COMPOSITOR",  False);
    app->net_wm_window_type        = XInternAtom(app->dpy, "_NET_WM_WINDOW_TYPE",        False);
    app->net_wm_window_type_dock   = XInternAtom(app->dpy, "_NET_WM_WINDOW_TYPE_DOCK",   False);

    app->gc = XCreateGC(app->dpy, DefaultRootWindow(app->dpy), 0, NULL);
    app->cursor_resize = XCreateFontCursor(app->dpy, XC_bottom_right_corner);

    app->mon_x = 0; app->mon_y = 0;
    app->mon_w = DisplayWidth(app->dpy, app->screen);
    app->mon_h = DisplayHeight(app->dpy, app->screen);

    db_init(app);

    g_app = app;
    signal(SIGINT,  signal_handler);
    signal(SIGTERM, signal_handler);
    signal(SIGPIPE, SIG_IGN);

    app_log(app, "INFO", "timer_c started, theme: %s", THEMES[app->theme_idx].name);
    return 1;
}

static void print_usage(const char *prog) {
    fprintf(stderr,
        "Usage: %s [time] [task] [options]\n\n"
        "Time formats: 25m  1h30m  45  30:00  1:30:00  u15:30  u1520\n"
        "Switches (prefix):  d=shutdown  a=alert  f/r=fullscreen  p=pausable\n"
        "Options: -m alert|shutdown  -p  -f  --theme NAME  --gl  -h\n"
        "Example: %s 25m focus  |  %s u16:30 meeting  |  %s\n",
        prog, prog, prog, prog);
}

/* ── main ──────────────────────────────────────────────── */

int main(int argc, char **argv) {
    srand((unsigned)time(NULL));
    App app;
    if (!app_init(&app)) return 1;

    char cli_time[MAX_INPUT] = "", cli_task[MAX_TASK] = "";
    int  cli_mode = -1, cli_pausable = 0, cli_fullscreen = 0, cli_theme = -1;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "-h") || !strcmp(a, "--help")) { print_usage(argv[0]); return 0; }
        if (!strcmp(a, "--gl")) { app.use_gl = 1; continue; }
        if (!strcmp(a, "-p") || !strcmp(a, "--pausable"))   { cli_pausable   = 1; continue; }
        if (!strcmp(a, "-f") || !strcmp(a, "--fullscreen"))  { cli_fullscreen = 1; continue; }
        if ((!strcmp(a, "-m") || !strcmp(a, "--mode")) && i+1 < argc) {
            cli_mode = !strcmp(argv[++i], "shutdown") ? 1 : 0; continue;
        }
        if (!strcmp(a, "--theme") && i+1 < argc) {
            const char *tn = argv[++i];
            for (int t = 0; t < N_THEMES; t++) {
                if (!strcasecmp(tn, THEMES[t].name) ||
                    (strstr(THEMES[t].name, " ") &&
                     !strncasecmp(tn, strstr(THEMES[t].name," ")+1, strlen(tn))))
                    { cli_theme = t; break; }
            }
            continue;
        }
        if (a[0] != '-' && cli_time[0] == '\0') {
            snprintf(cli_time, sizeof(cli_time), "%s", a);
            char task_buf[MAX_TASK] = "";
            for (int j = i+1; j < argc; j++) {
                if (argv[j][0] == '-') break;
                if (task_buf[0]) strncat(task_buf, " ", sizeof(task_buf)-strlen(task_buf)-1);
                strncat(task_buf, argv[j], sizeof(task_buf)-strlen(task_buf)-1);
                i = j;
            }
            snprintf(cli_task, sizeof(cli_task), "%s", task_buf);
            continue;
        }
    }

    if (cli_theme >= 0)  app.theme_idx = cli_theme;
    if (cli_pausable)    app.pausable  = 1;
    if (cli_fullscreen)  app.mini      = 0;
    if (cli_mode == 1)   app.mode      = MODE_SHUTDOWN;
    if (cli_mode == 0)   app.mode      = MODE_ALERT;

    if (cli_time[0]) {
        char combined[MAX_INPUT];
        if (cli_task[0]) snprintf(combined, sizeof(combined), "%s %s", cli_time, cli_task);
        else             snprintf(combined, sizeof(combined), "%s", cli_time);
        ParseResult pr; parse_time_input(combined, &pr);
        if (pr.seconds <= 0) {
            fprintf(stderr, "Error: invalid time '%s'\n", combined);
            print_usage(argv[0]); return 1;
        }
        app.total_seconds = app.remaining_seconds = pr.seconds;
        app.remaining_exact = (double)pr.seconds;
        if (cli_task[0]) snprintf(app.task, sizeof(app.task), "%s", cli_task);
        else             snprintf(app.task, sizeof(app.task), "%s", pr.task);
        snprintf(app.until_str, sizeof(app.until_str), "%s", pr.until);
        if (pr.sw_shutdown  && cli_mode < 0)    app.mode     = MODE_SHUTDOWN;
        if (pr.sw_fullscreen && !cli_fullscreen) app.mini     = 0;
        if (pr.sw_pausable   && !cli_pausable)   app.pausable = 1;

        Window r2, c2; int rx, ry, wx, wy; unsigned int msk;
        XQueryPointer(app.dpy, DefaultRootWindow(app.dpy), &r2, &c2, &rx, &ry, &wx, &wy, &msk);
        detect_monitor(&app, rx, ry);
#ifdef USE_GL
        if (app.use_gl) {
            if (!gl_init(&app)) app.use_gl = 0;
        }
#else
        app.use_gl = 0;
#endif
        start_running(&app);
    } else {
        create_config_window(&app);
        play_cue(&app, "launch");
    }

    main_loop(&app);

    /* Brief grace period so any closing/stop sound finishes playing before
     * the process exits and its audio writer threads are torn down. */
    usleep(680000);

    /* Cleanup */
    if (app.tmr_buf_xft)   XftDrawDestroy(app.tmr_buf_xft);
    if (app.tmr_buf)       XFreePixmap(app.dpy, app.tmr_buf);
    if (app.cfg_buf_xft)   XftDrawDestroy(app.cfg_buf_xft);
    if (app.cfg_buf)       XFreePixmap(app.dpy, app.cfg_buf);
    if (app.tmr_xft)       XftDrawDestroy(app.tmr_xft);
    if (app.cfg_xft)       XftDrawDestroy(app.cfg_xft);
    if (app.gc)            XFreeGC(app.dpy, app.gc);
    for (int i = 0; i < app.n_fonts; i++)
        if (app.fonts[i].font) XftFontClose(app.dpy, app.fonts[i].font);
    if (app.db)    sqlite3_close(app.db);
    if (app.logfp) fclose(app.logfp);
#ifdef USE_GL
    if (app.use_gl) gl_shutdown(&app);
#endif
    if (app.dpy)   XCloseDisplay(app.dpy);
    pthread_mutex_destroy(&app.audio_lock);
    return 0;
}
