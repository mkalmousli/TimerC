/* draw_alert.c — fullscreen alert/strobe windows + volume slider.
 * Relocated from the old monolithic draw.c; see src/ui/ui_priv.h for
 * the shared primitives/theme/buffer helpers this pulls in from
 * draw_common.c. */
#include "timer.h"
#include "ui_priv.h"

/* ── Alert windows ─────────────────────────────────────── */

static void ensure_alert_screen_buf(App *app, AlertScreen *s) {
    if (s->buf || !s->win) return;
    s->buf     = XCreatePixmap(app->dpy, s->win, s->w, s->h, app->depth);
    s->buf_xft = make_xft_draw(app, s->buf);
}

static void draw_alert_screen(App *app, AlertScreen *s,
                               double elapsed, unsigned long bg_rgb,
                               double brightness) {
    ensure_alert_screen_buf(app, s);
    if (!s->buf) return;
    int W = s->w, H = s->h;
    Drawable buf  = s->buf;
    XftDraw *draw = s->buf_xft;
    unsigned long text_col = (brightness > 0.55) ? 0x000000UL : 0xffffffUL;
    fill_rect(app, buf, bg_rgb, 0, 0, W, H);
    {
        const char *msg = "Time's Up!";
        int sz = fit_font_size(app, msg, W-120, H/4, 20, 180, 1);
        XftFont *f = get_font(app, sz, 1);
        if (f) xft_draw_string_centered(app, draw, f, text_col, W/2, H/3+f->ascent, msg);
    }
    if (app->task[0]) {
        int sz = fit_font_size(app, app->task, W-120, H/8, 14, 72, 0);
        XftFont *f = get_font(app, sz, 0);
        if (f) xft_draw_string_centered(app, draw, f, text_col,
                                         W/2, H/3+H/5+f->ascent, app->task);
    }
    {
        unsigned long btn_bg = (brightness < 0.45) ? 0xffffffUL : 0x000000UL;
        unsigned long btn_fg = (brightness < 0.45) ? 0x000000UL : 0xffffffUL;
        int bw = 280, bh = 52, bx = (W-bw)/2, by = H*2/3;
        fill_rect(app, buf, btn_bg, bx, by, bw, bh);
        draw_rect_outline(app, buf, btn_fg, bx, by, bw, bh);
        XftFont *f = get_font(app, 20, 1);
        if (f) xft_draw_string_centered(app, draw, f, btn_fg,
                                         W/2, by+(bh-f->ascent-f->descent)/2+f->ascent,
                                         "Dismiss  (ESC / click)");
    }
    XCopyArea(app->dpy, buf, s->win, app->gc, 0, 0, W, H, 0, 0);
    (void)elapsed;
}


void draw_alert(App *app) {
    if (app->n_alert_screens == 0) return;
    struct timespec now_ts;
    clock_gettime(CLOCK_MONOTONIC, &now_ts);
    double elapsed = (now_ts.tv_sec  - app->alert_start_ts.tv_sec) +
                     (now_ts.tv_nsec - app->alert_start_ts.tv_nsec) * 1e-9;

    /* Auto-mute the alarm after 5 minutes if the user never stops it.
     * Clearing the flag stops the streaming synth; kill_all_audio stops
     * the player.  The alert window stays up, but silent. */
    if (elapsed > g_config.alarm_max_seconds && app->alert_audio_active) {
        app->alert_audio_active = 0;
        kill_all_audio(app);
    }

    app->alert_frame++;
    unsigned long bg_rgb = alert_strobe_color(elapsed, app->alert_frame);
    int r2 = (bg_rgb>>16)&0xff, g2 = (bg_rgb>>8)&0xff, b2 = bg_rgb&0xff;
    double brightness = (r2*0.299 + g2*0.587 + b2*0.114) / 255.0;
    for (int i = 0; i < app->n_alert_screens; i++)
        draw_alert_screen(app, &app->alert_screens[i], elapsed, bg_rgb, brightness);
    XFlush(app->dpy);
}


void destroy_alert_windows(App *app) {
    for (int i = 0; i < app->n_alert_screens; i++) {
        AlertScreen *s = &app->alert_screens[i];
        if (s->buf_xft) { XftDrawDestroy(s->buf_xft); s->buf_xft = NULL; }
        if (s->buf)     { XFreePixmap(app->dpy, s->buf); s->buf = 0; }
        if (s->xft)     { XftDrawDestroy(s->xft); s->xft = NULL; }
        if (s->win)     { XDestroyWindow(app->dpy, s->win); s->win = 0; }
    }
    app->n_alert_screens = 0;
}


void create_alert_windows(App *app) {
    destroy_alert_windows(app);
    clock_gettime(CLOCK_MONOTONIC, &app->alert_start_ts);
    app->alert_frame = 0;
    Window root = DefaultRootWindow(app->dpy);

    int n_mons = 0;
    int mx[MAX_MONITORS], my[MAX_MONITORS], mw[MAX_MONITORS], mh[MAX_MONITORS];

    int eb, err;
    if (XRRQueryExtension(app->dpy, &eb, &err)) {
        XRRScreenResources *res = XRRGetScreenResourcesCurrent(app->dpy, root);
        if (res) {
            for (int i = 0; i < res->ncrtc && n_mons < MAX_MONITORS; i++) {
                XRRCrtcInfo *ci = XRRGetCrtcInfo(app->dpy, res, res->crtcs[i]);
                if (!ci) continue;
                if (ci->width > 0 && ci->height > 0) {
                    mx[n_mons]=ci->x; my[n_mons]=ci->y;
                    mw[n_mons]=(int)ci->width; mh[n_mons]=(int)ci->height;
                    n_mons++;
                }
                XRRFreeCrtcInfo(ci);
            }
            XRRFreeScreenResources(res);
        }
    }
    if (n_mons == 0) {
        mx[0]=0; my[0]=0;
        mw[0]=DisplayWidth(app->dpy, app->screen);
        mh[0]=DisplayHeight(app->dpy, app->screen);
        n_mons=1;
    }

    XSetWindowAttributes swa;
    swa.override_redirect = True;
    swa.background_pixel  = 0;
    swa.colormap          = app->cmap;
    /* ButtonReleaseMask/PointerMotionMask added for the fullscreen-alert
     * volume slider (item 5) — dragging it needs motion + release. */
    swa.event_mask        = ExposureMask | KeyPressMask | ButtonPressMask
                          | ButtonReleaseMask | PointerMotionMask;

    for (int i = 0; i < n_mons; i++) {
        AlertScreen *s = &app->alert_screens[i];
        s->x=mx[i]; s->y=my[i]; s->w=mw[i]; s->h=mh[i];
        s->buf=0; s->buf_xft=NULL;
        s->win = XCreateWindow(app->dpy, root, s->x, s->y, s->w, s->h, 0,
            app->depth, InputOutput, app->vis,
            CWOverrideRedirect|CWBackPixel|CWColormap|CWEventMask, &swa);
        XMapRaised(app->dpy, s->win);
        s->xft = make_xft_draw(app, s->win);
    }
    app->n_alert_screens = n_mons;
    if (n_mons > 0)
        XSetInputFocus(app->dpy, app->alert_screens[0].win, RevertToNone, CurrentTime);
    XFlush(app->dpy);
}
