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

/* ── Input lock (-l switch) ────────────────────────────── */

/* Grabs the keyboard and pointer for the whole X server, so nothing but
 * this process sees key/button events until release_input() runs. Retries
 * briefly since a window manager can hold a transient grab (e.g. during a
 * window-map animation) right after the timer window appears. */
static void grab_input_on(App *app, Window w) {
    if (app->input_grabbed || !w) return;
    int kb_ok = 0, ptr_ok = 0;
    for (int i = 0; i < 20 && (!kb_ok || !ptr_ok); i++) {
        if (!kb_ok) {
            int r = XGrabKeyboard(app->dpy, w, True, GrabModeAsync, GrabModeAsync, CurrentTime);
            kb_ok = (r == GrabSuccess);
        }
        if (!ptr_ok) {
            int r = XGrabPointer(app->dpy, w, True,
                                  ButtonPressMask | ButtonReleaseMask | PointerMotionMask,
                                  GrabModeAsync, GrabModeAsync, None, None, CurrentTime);
            ptr_ok = (r == GrabSuccess);
        }
        if (!kb_ok || !ptr_ok) usleep(25000);
    }
    app->input_grabbed = kb_ok || ptr_ok;
    if (!kb_ok) app_log(app, "WARN", "Failed to grab keyboard for lock mode");
    if (!ptr_ok) app_log(app, "WARN", "Failed to grab pointer for lock mode");
    else app_log(app, "INFO", "Input locked: keyboard=%d pointer=%d", kb_ok, ptr_ok);
}

static void grab_input(App *app) { grab_input_on(app, app->tmr_win); }

static void release_input(App *app) {
    if (!app->input_grabbed) return;
    XUngrabKeyboard(app->dpy, CurrentTime);
    XUngrabPointer(app->dpy, CurrentTime);
    XFlush(app->dpy);
    app->input_grabbed = 0;
    app_log(app, "INFO", "Input unlocked");
}

/* ── State transitions ─────────────────────────────────── */

static void do_quit(App *app) {
    if (app->input_grabbed) release_input(app);
    if (app->hw_lock.active) hw_lock_release(&app->hw_lock);
    app_log(app, "INFO", "Quitting");
    app->alert_audio_active = 0;
    app->sleep_audio_active = 0;
    /* Only forcibly kill audio when leaving an alert (or stopping a sleep
     * timer's ambient loop, or a sleep-preview audition) — cue sounds are
     * short and should play to completion after windows are gone. */
    if (app->state == STATE_ALERT || app->sleep_mode)
        kill_all_audio(app);
    db_finish_session(app, "stopped");
    destroy_alert_windows(app);
    destroy_timer_window(app);
    if (app->cfg_buf_xft) { XftDrawDestroy(app->cfg_buf_xft); app->cfg_buf_xft = NULL; }
    if (app->cfg_buf)     { XFreePixmap(app->dpy, app->cfg_buf); app->cfg_buf = 0; }
    draw_release_quote_cache(app);
    if (app->cfg_xft)     { XftDrawDestroy(app->cfg_xft); app->cfg_xft = NULL; }
    if (app->cfg_win)     { XDestroyWindow(app->dpy, app->cfg_win); app->cfg_win = 0; }
    XFlush(app->dpy);
    app->state = STATE_DONE;
}

static void dismiss_alert(App *app) {
    app_log(app, "INFO", "Alert dismissed");
    db_log_event(app, "alert_dismiss", NULL);
    app->alert_audio_active = 0;
    kill_all_audio(app);   /* stop the looping alert siren immediately */
    play_cue(app, "dismiss");
    db_finish_session(app, "completed");
    do_quit(app);
}

static void stop_timer(App *app) {
    app_log(app, "INFO", "Timer stopped by user");
    db_log_event(app, "timer_stop", NULL);
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
        detect_monitor(app, app->cfg_x + g_config.cfg_w/2, app->cfg_y + g_config.cfg_h/2);

    create_timer_window(app);
    db_start_session(app);
    {
        char det[512];
        snprintf(det, sizeof(det), "duration=%ds task=%.200s mode=%s",
                 app->total_seconds, app->task,
                 app->mode == MODE_SHUTDOWN ? "shutdown" : "alert");
        db_log_event(app, "timer_start", det);
    }
    app_log(app, "INFO", "Timer started: %ds, task='%s', mode=%s, mini=%d",
            app->total_seconds, app->task,
            app->mode == MODE_SHUTDOWN ? "shutdown" : "alert", app->mini);

    if (app->cfg_win) XWithdrawWindow(app->dpy, app->cfg_win, app->screen);
    XFlush(app->dpy);
    play_cue(app, "start");
    if (app->sleep_mode) play_sleep_ambient(app, (double)app->total_seconds);
    if (app->lock) {
        grab_input(app); /* X-level fallback / belt-and-suspenders */
        if (hw_lock_engage(&app->hw_lock, app->total_seconds)) {
            app_log(app, "INFO", "Hardware input lock engaged (%d device(s) deauthorized)",
                    app->hw_lock.n_devices);
        } else {
            app_log(app, "WARN",
                    "Hardware input lock unavailable (setuid helper not installed — run "
                    "'sudo make install-lock-helper' once — or no USB HID keyboard/mouse "
                    "found) — falling back to X11 grab only, which is not a full disconnect");
        }
    }
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
    db_log_event(app, "window_toggle", app->mini ? "mini" : "fullscreen");
    create_timer_window(app);
    app->tmr_force_redraw = 1;
    play_cue(app, app->mini ? "pause" : "resume");
    if (app->lock) { app->input_grabbed = 0; grab_input(app); }
}

static void toggle_pause(App *app) {
    if (!app->pausable) return;
    if (app->state == STATE_RUNNING) {
        app->state = STATE_PAUSED;
        clock_gettime(CLOCK_MONOTONIC, &app->pause_start_ts);
        play_cue(app, "pause");
        db_log_event(app, "timer_pause", NULL);
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
        db_log_event(app, "timer_resume", NULL);
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
    /* The lock's whole point was making the countdown uncancelable — once
     * it's actually finished there's nothing left to protect, so hand
     * keyboard/mouse straight back for the alert/shutdown that follows. */
    if (app->input_grabbed) release_input(app);
    if (app->hw_lock.active) hw_lock_release(&app->hw_lock);

    if (app->mode == MODE_SHUTDOWN) {
        db_log_event(app, "shutdown_trigger", NULL);
        play_cue(app, "stop");
        db_finish_session(app, "completed");
        app_log(app, "WARN", "Initiating system shutdown");
        usleep(800000);
        execl("/usr/bin/systemctl", "systemctl", "poweroff", (char *)NULL);
        run_cmd_shutdown("systemctl poweroff");
        do_quit(app);
    } else {
        db_log_event(app, "alert_open", NULL);
        play_cue(app, "alert_open");
        create_alert_windows(app);
        app->state = STATE_ALERT;
        /* Sleep mode: the ambient thread already eased into its wake
         * melody near the end and keeps playing it — no harsh siren. */
        if (!app->sleep_mode) play_alert_sound(app);
    }
}

/* ── Timer tick ────────────────────────────────────────── */

static void maybe_play_cues(App *app, int prev_rem) {
    if (app->total_seconds <= 0) return;
    if (!app->cue_half_played) {
        int half = app->total_seconds / 2;
        if (prev_rem > half && app->remaining_seconds <= half) {
            app->cue_half_played = 1;
            db_log_event(app, "half_point", NULL);
            play_cue(app, "half");
        }
    }
    for (int i = 0; i < 3; i++) {
        int s = 3 - i;
        if (!app->cue_last3[i] && prev_rem > s && app->remaining_seconds <= s) {
            app->cue_last3[i] = 1;
            char det[8]; snprintf(det, sizeof(det), "%ds", s);
            db_log_event(app, "last_3s", det);
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

    long min_interval = (app->state == STATE_RUNNING) ? g_config.tick_run_ms : g_config.tick_paused_ms;
    if (!needs_draw && (draw_now_ms - app->tmr_last_draw_ms) >= min_interval) needs_draw = 1;

    if (needs_draw) {
        draw_timer(app);
        app->tmr_last_draw_ms = draw_now_ms;
        app->tmr_force_redraw = 0;
    }
}

/* ── Event handling ────────────────────────────────────── */

static void drain_motion(App *app, XEvent *ev, int *rx, int *ry) {
    *rx = ev->xmotion.x_root;
    *ry = ev->xmotion.y_root;
    XEvent n;
    while (XCheckTypedEvent(app->dpy, MotionNotify, &n)) {
        *rx = n.xmotion.x_root;
        *ry = n.xmotion.y_root;
    }
}

static void handle_config_key(App *app, XKeyEvent *ev) {
    char buf[32] = "";
    KeySym ks;
    XLookupString(ev, buf, sizeof(buf)-1, &ks, NULL);

    /* Tab / Ctrl+T: next theme; Shift+Tab: previous. */
    if (ks == XK_Tab || ks == XK_ISO_Left_Tab ||
        ((ev->state & ControlMask) && (ks == XK_t || ks == XK_T))) {
        theme_step(app, (ks == XK_ISO_Left_Tab || (ev->state & ShiftMask)) ? -1 : 1);
        draw_config(app);
        return;
    }
    if ((ev->state & ControlMask) && (ks == XK_a || ks == XK_A)) {
        app->cursor_pos = app->input_len;
        app->cursor_blink = 1; app->cursor_blink_ts = ms_now();
        cfg_redraw_after_input(app); return;
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
        if (pr.sw_lock)       app->lock = 1;
        app->sleep_mode = pr.sw_sleep;
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
    cfg_redraw_after_input(app);
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
            } else if (ev->xbutton.button == Button1 &&
                       theme_strip_hit(ev->xbutton.x, ev->xbutton.y) != -1) {
                int h = theme_strip_hit(ev->xbutton.x, ev->xbutton.y);
                if (h >= 0) app->theme_idx = h; else theme_step(app, h == -2 ? -1 : 1);
                draw_config(app);
            } else if (ev->xbutton.button == Button1) {
                app->cfg_is_dragging = 1;
                app->cfg_drag_px = ev->xbutton.x_root; app->cfg_drag_py = ev->xbutton.y_root;
                app->cfg_drag_wx = app->cfg_x;          app->cfg_drag_wy = app->cfg_y;
                XGrabPointer(app->dpy, app->cfg_win, True,
                             PointerMotionMask | ButtonReleaseMask,
                             GrabModeAsync, GrabModeAsync,
                             app->cfg_win, None, CurrentTime);
            } else if (ev->xbutton.button == Button3) {
                app->fullscreen_requested = 1; app->mini = 0;
            }
            break;
        case ButtonRelease:
            if (app->cfg_is_dragging)
                XUngrabPointer(app->dpy, CurrentTime);
            app->cfg_is_dragging = 0;
            break;
        case MotionNotify:
            if (app->cfg_is_dragging) {
                int rx, ry;
                drain_motion(app, ev, &rx, &ry);
                app->cfg_x = app->cfg_drag_wx + (rx - app->cfg_drag_px);
                app->cfg_y = app->cfg_drag_wy + (ry - app->cfg_drag_py);
                XMoveWindow(app->dpy, app->cfg_win, app->cfg_x, app->cfg_y);
                XFlush(app->dpy);
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
            if (ev->xbutton.button == Button2) {
                if (!(app->lock && (app->state == STATE_RUNNING || app->state == STATE_PAUSED)))
                    stop_timer(app);
                return;
            }
            if (ev->xbutton.button == Button3) { toggle_mini_fullscreen(app); return; }
            if (ev->xbutton.button == Button1) {
                if (app->mini) {
                    if (bx >= app->tmr_w - g_config.resize_zone && by >= app->tmr_h - g_config.resize_zone) {
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
                app->drag_grabbed = 1;
                XGrabPointer(app->dpy, app->tmr_win, True,
                             PointerMotionMask | ButtonReleaseMask,
                             GrabModeAsync, GrabModeAsync,
                             app->tmr_win, None, CurrentTime);
            }
            break;
        }
        case ButtonRelease:
            if (app->drag_grabbed) {
                XUngrabPointer(app->dpy, CurrentTime);
                app->drag_grabbed = 0;
            }
            app->is_dragging = 0;
            app->is_resizing = 0;
            break;
        case MotionNotify:
            if (app->is_dragging && app->mini) {
                int rx, ry;
                drain_motion(app, ev, &rx, &ry);
                app->tmr_x = app->drag_wx + (rx - app->drag_px);
                app->tmr_y = app->drag_wy + (ry - app->drag_py);
                XMoveWindow(app->dpy, app->tmr_win, app->tmr_x, app->tmr_y);
                XFlush(app->dpy);
            } else if (app->is_resizing && app->mini) {
                int dx = ev->xmotion.x_root - app->resize_px;
                int dy = ev->xmotion.y_root - app->resize_py;
                int nw = app->resize_ww + dx;
                int nh = app->resize_wh + dy;
                if (nw < g_config.mini_w_min) nw = g_config.mini_w_min;
                if (nh < g_config.mini_h_min) nh = g_config.mini_h_min;
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
                int in_zone = (ev->xmotion.x >= app->tmr_w - g_config.resize_zone &&
                               ev->xmotion.y >= app->tmr_h - g_config.resize_zone);
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
            if ((Atom)ev->xclient.data.l[0] == app->wm_delete &&
                !(app->lock && (app->state == STATE_RUNNING || app->state == STATE_PAUSED)))
                stop_timer(app);
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
        case MotionNotify:
            return;
        }
        return;
    }
}

/* ── Config cursor blink tick ──────────────────────────── */

static void do_config_tick(App *app) {
    long now_ms = ms_now();

    if (now_ms - app->cursor_blink_ts > 530) {
        app->cursor_blink = !app->cursor_blink;
        app->cursor_blink_ts = now_ms;
    }

    if (!app->cfg_anim_last_ms) app->cfg_anim_last_ms = now_ms;
    if (now_ms - app->cfg_anim_last_ms >= g_config.tick_config_anim_ms) {
        if (app->cfg_win && app->state == STATE_CONFIG)
            draw_config_anim(app);
        app->cfg_anim_last_ms = now_ms;
    }

    /* Live until-countdown preview — reparse once per wall-clock second. */
    if (app->cfg_win && app->state == STATE_CONFIG && app->cfg_preview.until[0]) {
        time_t ws = time(NULL);
        if (ws != app->cfg_last_wall_sec) {
            app->cfg_last_wall_sec = ws;
            cfg_refresh_preview(app);
            if (app->cfg_win && app->state == STATE_CONFIG)
                draw_config_anim(app);
        }
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
        case STATE_CONFIG:  tick_ms = g_config.tick_config_anim_ms;   break;
        case STATE_RUNNING:
            tick_ms = (app->remaining_seconds <= 10) ? g_config.tick_last10_ms : g_config.tick_run_ms;
            break;
        case STATE_PAUSED:  tick_ms = g_config.tick_paused_ms;  break;
        case STATE_ALERT:   tick_ms = g_config.tick_alert_ms;         break;
        default:            tick_ms = 250;                   break;
        }
        if (app->is_dragging || app->cfg_is_dragging || app->is_resizing)
            tick_ms = 1;
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

/* While a locked timer is counting down, SIGINT/SIGTERM (e.g. from another
 * terminal) must not be able to cancel it either — only SIGKILL, which no
 * process can block, gets through. */
static void signal_handler(int sig) {
    (void)sig;
    if (!g_app) return;
    if (g_app->lock && (g_app->state == STATE_RUNNING || g_app->state == STATE_PAUSED))
        return;
    g_app->quit = 1;
}

static int app_init(App *app) {
    memset(app, 0, sizeof(*app));

    const char *home = getenv("HOME");
    if (!home) home = "/tmp";
    char data_dir[MAX_PATH_LEN-16];
    snprintf(data_dir, sizeof(data_dir), "%s/.local/share/timer_c", home);
    mkdir(data_dir, 0755);
    snprintf(app->log_path, sizeof(app->log_path), "%s/timer.log", data_dir);
    app->logfp = fopen(app->log_path, "a");
    /* app->db_path is resolved by db_init() to sit next to the binary */

    config_load(app);
    generate_themes();

    app->theme_idx  = (g_config.default_theme >= 0) ? g_config.default_theme
                                                      : rand() % N_THEMES;
    app->cfg_focused = 1;
    app->state      = STATE_CONFIG;
    app->mode       = MODE_ALERT;
    app->mini       = g_config.default_fullscreen ? 0 : 1;
    app->pausable   = g_config.default_pausable;
    app->session_id = 0;
    app->blink_on   = 1;
    app->blink_ts   = ms_now();
    app->quote_idx = -1;
    app->quote_next_idx = -1;
    app->quote_cycle_ts = 0;
    app->quote_mask_idx[0] = -1;
    app->quote_mask_idx[1] = -1;
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
    db_log_event(app, "app_launch", NULL);

    g_app = app;
    signal(SIGINT,  signal_handler);
    signal(SIGTERM, signal_handler);
    signal(SIGPIPE, SIG_IGN);

    app_log(app, "INFO", "timer_c started, theme: %s", THEMES[app->theme_idx].name);
    return 1;
}

static void print_usage(const char *prog, FILE *out) {
    (void)prog;
    fprintf(out,
"TimerC " TIMERC_VERSION " - a native countdown timer for X11\n"
"\n"
"USAGE\n"
"  timerc    [SWITCHES][TIME] [TASK] [OPTIONS]   run in the foreground, wait until done\n"
"  timerc-bg [SWITCHES][TIME] [TASK] [OPTIONS]   same, detached from the terminal\n"
"  timerc                                        open the setup window\n"
"\n"
"TIME\n"
"  25m  1h30m  90s  45 (minutes)  30:00  1:30:00     duration\n"
"  u15:30   u:30                                     until a clock time / minute\n"
"  /8h                                               sleep mode (ambient audio, gentle wake)\n"
"\n"
"SWITCHES (letters before the time, combinable, e.g. fp25m)\n"
"  d shutdown at the end   a fullscreen alert   f fullscreen   p pausable\n"
"  l lock keyboard/mouse (needs timerc-lock-helper)\n"
"\n"
"OPTIONS\n"
"  -m alert|shutdown   mode at the end        -p, --pausable     allow pause (Space)\n"
"  -f, --fullscreen    start fullscreen       -l, --lock         lock input\n"
"  --theme NAME        theme (Tab in the GUI) --gl               OpenGL renderer\n"
"  -V, --version       print version          -h, --help         this help\n"
"\n"
"TERMINAL UIS (settings, statistics and help live here, not in the window)\n"
"  timerc config                    interactive settings, tests and about (TUI)\n"
"  timerc config list               show all options with defaults\n"
"  timerc config get KEY            print one value\n"
"  timerc config set KEY VALUE      change one value\n"
"  timerc config reset [KEY]        restore defaults\n"
"  timerc config path               print the config file path\n"
"  timerc insights                  statistics and session log (TUI; text when piped)\n"
"  timerc about                     syntax, keys, credits\n"
"\n"
"UNINSTALL\n"
"  timerc-uninstall [-y] [-n] [--purge]   remove TimerC (--purge also deletes settings and history)\n"
"\n"
"TESTING\n"
"  timerc test alarm|preparing|sleeping|waking|all   play the generated audio (30 s each)\n"
"  timerc /testsleep 8h                              print the sleep-mode plan\n"
"  (all of these are also available in 'timerc config', screen 3)\n"
"\n"
"EXAMPLES\n"
"  timerc 25m write report        timerc-bg u16:30 meeting\n"
"  timerc pf45m deep work         timerc 5m && notify-send done\n"
"\n"
"More: man timerc, man timerc-config, man timerc-insights    https://al-mo.de/tools/timerc/\n");
}

/* ── `timer test` — foreground CLI audition of the procedurally
 * generated sleep-mode phases (preparing/sleeping/waking), at the exact
 * same SYSTEM volume the app itself sets for that phase (see
 * set_system_volume() in audio.c — the one function in this whole
 * program that ever touches your system volume). Reads the target
 * percent straight out of g_config via config_load() (see config.c) —
 * nothing here is a separate copy of those numbers.
 *
 * Usage:
 *   timer test preparing|sleeping|waking|alarm
 *   timer test all
 */
#define TEST_PHASE_SECONDS 30.0

static int run_test_command(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "Usage: %s test preparing|sleeping|waking|alarm|all\n", argv[0]);
        return 1;
    }
    config_load(NULL);

    const char *phase = argv[2];
    if (!strcmp(phase, "preparing")) {
        play_test_phase_cli("preparing", (int)g_config.sleep_volume_preparing_percent, TEST_PHASE_SECONDS);
    } else if (!strcmp(phase, "sleeping")) {
        play_test_phase_cli("sleeping", (int)g_config.sleep_volume_sleeping_percent, TEST_PHASE_SECONDS);
    } else if (!strcmp(phase, "waking")) {
        play_test_phase_cli("waking", (int)g_config.sleep_volume_waking_percent, TEST_PHASE_SECONDS);
    } else if (!strcmp(phase, "alarm")) {
        play_test_phase_cli("alarm", (int)g_config.alarm_volume_min_percent, TEST_PHASE_SECONDS);
    } else if (!strcmp(phase, "all")) {
        play_test_phase_cli("preparing", (int)g_config.sleep_volume_preparing_percent, TEST_PHASE_SECONDS);
        play_test_phase_cli("sleeping", (int)g_config.sleep_volume_sleeping_percent, TEST_PHASE_SECONDS);
        play_test_phase_cli("waking", (int)g_config.sleep_volume_waking_percent, TEST_PHASE_SECONDS);
    } else {
        fprintf(stderr, "Usage: %s test preparing|sleeping|waking|alarm|all\n", argv[0]);
        return 1;
    }
    return 0;
}

/* ── main ──────────────────────────────────────────────── */

/* ── Physical-display auto-detection (for launching from SSH) ────────
 * When DISPLAY isn't set (e.g. an SSH session), scan /proc for a
 * process owned by us that has DISPLAY (and ideally XAUTHORITY) set —
 * that's the running graphical session — and adopt those values. */
static int find_display_from_procs(void) {
    if (getenv("DISPLAY")) return 1;

    uid_t uid = getuid();
    DIR *d = opendir("/proc");
    if (!d) return 0;

    char found_display[64] = "", found_xauth[MAX_PATH_LEN] = "";
    struct dirent *ent;
    while ((ent = readdir(d))) {
        if (!isdigit((unsigned char)ent->d_name[0])) continue;

        char path[300];
        snprintf(path, sizeof(path), "/proc/%s", ent->d_name);
        struct stat st;
        if (stat(path, &st) != 0 || st.st_uid != uid) continue;

        snprintf(path, sizeof(path), "/proc/%s/environ", ent->d_name);
        FILE *f = fopen(path, "r");
        if (!f) continue;
        char buf[8192];
        size_t n = fread(buf, 1, sizeof(buf) - 1, f);
        fclose(f);
        buf[n] = '\0';

        char disp[64] = "", xauth[MAX_PATH_LEN] = "";
        for (char *p = buf; p < buf + n; ) {
            size_t len = strlen(p);
            if (!strncmp(p, "DISPLAY=", 8))
                snprintf(disp, sizeof(disp), "%s", p + 8);
            else if (!strncmp(p, "XAUTHORITY=", 11))
                snprintf(xauth, sizeof(xauth), "%s", p + 11);
            p += len + 1;
        }
        if (disp[0]) {
            snprintf(found_display, sizeof(found_display), "%s", disp);
            snprintf(found_xauth, sizeof(found_xauth), "%s", xauth);
            break;
        }
    }
    closedir(d);

    if (!found_display[0]) return 0;
    setenv("DISPLAY", found_display, 1);
    if (found_xauth[0]) {
        setenv("XAUTHORITY", found_xauth, 1);
    } else {
        const char *home = getenv("HOME");
        if (home) {
            char xa[MAX_PATH_LEN];
            snprintf(xa, sizeof(xa), "%s/.Xauthority", home);
            setenv("XAUTHORITY", xa, 1);
        }
    }
    return 1;
}

/* ── Daemonize: fully detach from the invoking terminal/SSH session ── */
static void daemonize(void) {
    pid_t pid = fork();
    if (pid < 0) { perror("fork"); exit(1); }
    if (pid > 0) exit(0);            /* parent returns control to the shell */

    if (setsid() < 0) exit(1);

    pid = fork();                    /* second fork: drop controlling tty */
    if (pid < 0) exit(1);
    if (pid > 0) exit(0);

    int devnull = open("/dev/null", O_RDWR);
    if (devnull >= 0) {
        dup2(devnull, STDIN_FILENO);
        dup2(devnull, STDOUT_FILENO);
        dup2(devnull, STDERR_FILENO);
        if (devnull > STDERR_FILENO) close(devnull);
    }
    chdir("/");
}

int main(int argc, char **argv) {
    srand((unsigned)time(NULL));

    /* `timerc` blocks until the timer ends; `timerc-bg` (same binary, run
     * under that name) detaches from the terminal like a daemon. */
    const char *base = strrchr(argv[0], '/');
    base = base ? base + 1 : argv[0];
    int background = !strcmp(base, "timerc-bg");

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
            print_usage(base, stdout);
            return 0;
        }
        if (!strcmp(argv[i], "-V") || !strcmp(argv[i], "--version")) {
            printf("timerc %s\n", TIMERC_VERSION);
            return 0;
        }
    }

    if (!strcmp(base, "timerc-uninstall"))
        return uninstall_main(argc, argv);
    if (argc > 1 && !strcmp(argv[1], "uninstall"))
        return uninstall_main(argc - 1, argv + 1);
    if (argc > 1 && (!strcmp(argv[1], "config") || !strcmp(argv[1], "settings") ||
                     !strcmp(argv[1], "insights") || !strcmp(argv[1], "stats") ||
                     !strcmp(argv[1], "about")))
        return cli_main(argv[1], argc - 1, argv + 1);
    if (argc > 1 && !strcmp(argv[1], "test")) {
        return run_test_command(argc, argv);
    }

    if (argc > 1 && !strcmp(argv[1], "/testsleep")) {
        config_load(NULL);
        char combined[MAX_INPUT] = "";
        for (int i = 2; i < argc; i++) {
            if (combined[0]) strncat(combined, " ", sizeof(combined)-strlen(combined)-1);
            strncat(combined, argv[i], sizeof(combined)-strlen(combined)-1);
        }
        ParseResult pr; parse_time_input(combined, &pr);
        if (pr.seconds <= 0) {
            fprintf(stderr, "Error: invalid time '%s'\n", combined);
            fprintf(stderr, "Usage: %s /testsleep <time>   e.g. /testsleep u6:30, /testsleep 8h\n", argv[0]);
            return 1;
        }
        SleepPreviewData *d = sleep_build_preview(pr.seconds);
        if (d) { sleep_print_preview(d, combined); free(d); }
        return 0;
    }

    find_display_from_procs();
    if (background) daemonize();

    App app;
    if (!app_init(&app)) return 1;

    char cli_time[MAX_INPUT] = "", cli_task[MAX_TASK] = "";
    int  cli_mode = -1, cli_pausable = 0, cli_fullscreen = 0, cli_lock = 0, cli_theme = -1;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "-h") || !strcmp(a, "--help")) { print_usage(base, stdout); return 0; }
        if (!strcmp(a, "--gl")) { app.use_gl = 1; continue; }
        if (!strcmp(a, "-p") || !strcmp(a, "--pausable"))   { cli_pausable   = 1; continue; }
        if (!strcmp(a, "-f") || !strcmp(a, "--fullscreen"))  { cli_fullscreen = 1; continue; }
        if (!strcmp(a, "-l") || !strcmp(a, "--lock"))         { cli_lock       = 1; continue; }
        if ((!strcmp(a, "-m") || !strcmp(a, "--mode")) && i+1 < argc) {
            cli_mode = !strcmp(argv[++i], "shutdown") ? 1 : 0; continue;
        }
        if (!strcmp(a, "--theme") && i+1 < argc) {
            const char *tn = argv[++i];
            generate_themes();
            for (int t = 0; t < N_THEMES; t++)
                if (!strncasecmp(tn, THEMES[t].name, strlen(tn))) { cli_theme = t; break; }
            if (cli_theme < 0) {
                fprintf(stderr, "timerc: unknown theme '%s'. Themes:", tn);
                for (int t = 0; t < N_THEMES; t++) fprintf(stderr, " %s", THEMES[t].name);
                fprintf(stderr, "\n");
                return 2;
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
    if (cli_lock)        app.lock      = 1;
    if (cli_mode == 1)   app.mode      = MODE_SHUTDOWN;
    if (cli_mode == 0)   app.mode      = MODE_ALERT;

    if (cli_time[0]) {
        char combined[MAX_INPUT];
        if (cli_task[0]) snprintf(combined, sizeof(combined), "%s %s", cli_time, cli_task);
        else             snprintf(combined, sizeof(combined), "%s", cli_time);
        ParseResult pr; parse_time_input(combined, &pr);
        if (pr.seconds <= 0) {
            fprintf(stderr, "Error: invalid time '%s'\n", combined);
            print_usage(argv[0], stderr); return 1;
        }
        app.total_seconds = app.remaining_seconds = pr.seconds;
        app.remaining_exact = (double)pr.seconds;
        if (cli_task[0]) snprintf(app.task, sizeof(app.task), "%s", cli_task);
        else             snprintf(app.task, sizeof(app.task), "%s", pr.task);
        snprintf(app.until_str, sizeof(app.until_str), "%s", pr.until);
        if (pr.sw_shutdown  && cli_mode < 0)    app.mode     = MODE_SHUTDOWN;
        if (pr.sw_fullscreen && !cli_fullscreen) app.mini     = 0;
        if (pr.sw_pausable   && !cli_pausable)   app.pausable = 1;
        if (pr.sw_lock       && !cli_lock)       app.lock     = 1;
        app.sleep_mode = pr.sw_sleep;

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
