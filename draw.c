/* draw.c — All X11 UI rendering and window management */
#include "timer.h"

/* Default to a lightweight render path (less CPU/GPU, fewer animations). */
#ifndef LOW_POWER
#define LOW_POWER 1
#endif

/* ── Theme data ────────────────────────────────────────── */

const Theme THEMES[N_THEMES] = {
    /* Forge — charcoal near-black, glowing hot-metal orange accent */
    { "Forge",
      0x070504, 0x110d08, 0x1a1510,
      0xcdc0a8, 0x5a4e3c,
      0xd06018, 0x251408,
      0x2c1e0e, 0xc02818,
      0x160e06, 0xc07840, 0x382010,
      0xd07830, 0x060402,
      0xd06018, 0x682808 },
    /* Abyss — deep ocean near-black, electric cyan accent */
    { "Abyss",
      0x010508, 0x050c12, 0x081419,
      0xa4bdc6, 0x344750,
      0x1888a8, 0x061420,
      0x0e2430, 0x7a2c38,
      0x050c14, 0x50a0bc, 0x0e2840,
      0x30b0d0, 0x010508,
      0x28a8c8, 0x144060 },
    /* Grove — forest floor near-black, warm moss-green accent */
    { "Grove",
      0x030502, 0x070d06, 0x0b1409,
      0xa6bcaa, 0x445844,
      0x689040, 0x101c08,
      0x182a14, 0x8c3828,
      0x070e06, 0x78b060, 0x183018,
      0x88c070, 0x030502,
      0x70b050, 0x384c20 },
    /* Dusk — dark purple sky, soft violet accent */
    { "Dusk",
      0x040308, 0x0b0914, 0x110d1c,
      0xb4aecb, 0x4c4460,
      0x7055b8, 0x150c28,
      0x221538, 0x903248,
      0x090814, 0x9075c8, 0x281848,
      0xa085d8, 0x030208,
      0x8865c0, 0x401c60 },
};

static const char *QUOTES[] = {
    "Nobody cares. Deliver.",
    "Your excuses are getting stronger. Are you?",
    "Potential means nothing unpaid.",
    "The mirror remembers every skipped day.",
    "Weak habits create weak lives.",
    "You don't need motivation. You need standards.",
    "Comfort has already taken enough from you.",
    "Every delay is a vote for failure.",
    "Talk less. Produce more.",
    "The goal doesn't care how you feel.",
    "Your future is watching today's choices.",
    "If it's important, prove it.",
    "Quit waiting for perfect conditions.",
    "You either train discipline or regret.",
    "Nobody remembers almost.",
    "The work is hard. So what?",
    "You can suffer progress or suffer stagnation.",
    "Results are the only argument.",
    "Stop seeking permission.",
    "Be the person your excuses fear.",
};
#define N_QUOTES ((int)(sizeof(QUOTES)/sizeof(QUOTES[0])))
#define QUOTE_MORPH_MS   950
#define NET_WM_DESKTOP_ALL 0xffffffffUL

/* ── Color utilities ───────────────────────────────────── */

static unsigned long rgb_pixel(App *app, unsigned long rgb) {
    XColor c;
    c.red   = ((rgb >> 16) & 0xff) * 257;
    c.green = ((rgb >> 8)  & 0xff) * 257;
    c.blue  = ( rgb        & 0xff) * 257;
    c.flags = DoRed | DoGreen | DoBlue;
    XAllocColor(app->dpy, app->cmap, &c);
    return c.pixel;
}

static void set_window_on_all_workspaces(App *app, Window win) {
    unsigned long desktop = NET_WM_DESKTOP_ALL;
    XChangeProperty(app->dpy, win, app->net_wm_desktop,
                    XA_CARDINAL, 32, PropModeReplace,
                    (unsigned char *)&desktop, 1);
}

static void disable_compositor_bypass(App *app, Window win) {
    unsigned long value = 2; /* EWMH: ask compositor not to unredirect this window. */
    XChangeProperty(app->dpy, win, app->net_wm_bypass_compositor,
                    XA_CARDINAL, 32, PropModeReplace,
                    (unsigned char *)&value, 1);
}

static void rgb_xft(App *app, unsigned long rgb, XftColor *out) {
    XRenderColor rc;
    rc.red   = ((rgb >> 16) & 0xff) * 257;
    rc.green = ((rgb >> 8)  & 0xff) * 257;
    rc.blue  = ( rgb        & 0xff) * 257;
    rc.alpha = 0xffff;
    XftColorAllocValue(app->dpy, app->vis, app->cmap, &rc, out);
}

static unsigned long lerp_rgb(unsigned long a, unsigned long b, double t) {
    if (t <= 0.0) return a;
    if (t >= 1.0) return b;
    int ar = (a>>16)&0xff, ag = (a>>8)&0xff, ab2 = a&0xff;
    int br = (b>>16)&0xff, bg = (b>>8)&0xff, bb  = b&0xff;
    int r  = (int)(ar + (br-ar)*t);
    int g  = (int)(ag + (bg-ag)*t);
    int b2 = (int)(ab2 + (bb-ab2)*t);
    return ((unsigned long)r<<16)|((unsigned long)g<<8)|(unsigned long)b2;
}

static void fill_rect(App *app, Drawable d, unsigned long rgb,
                      int x, int y, int w, int h) {
    XSetForeground(app->dpy, app->gc, rgb_pixel(app, rgb));
    XFillRectangle(app->dpy, d, app->gc, x, y, w, h);
}

static void draw_rect_outline(App *app, Drawable d, unsigned long rgb,
                               int x, int y, int w, int h) {
    XSetForeground(app->dpy, app->gc, rgb_pixel(app, rgb));
    XDrawRectangle(app->dpy, d, app->gc, x, y, w-1, h-1);
}

/* ── Font management ───────────────────────────────────── */

static XftFont *get_font(App *app, int size, int style) {
    for (int i = 0; i < app->n_fonts; i++)
        if (app->fonts[i].size == size && app->fonts[i].style == style)
            return app->fonts[i].font;

    char pattern[128];
    if      (style == 1) snprintf(pattern, sizeof(pattern), "Sans:size=%d:weight=bold",   size);
    else if (style == 2) snprintf(pattern, sizeof(pattern), "Sans:size=%d:slant=italic",  size);
    else if (style == 3) snprintf(pattern, sizeof(pattern), "monospace:size=%d:weight=bold", size);
    else                 snprintf(pattern, sizeof(pattern), "Sans:size=%d",                size);

    XftFont *f = XftFontOpenName(app->dpy, app->screen, pattern);
    if (!f) {
        snprintf(pattern, sizeof(pattern), "fixed:size=%d", size);
        f = XftFontOpenName(app->dpy, app->screen, pattern);
    }
    if (!f) return NULL;
    if (app->n_fonts < MAX_FONT_CACHE) {
        app->fonts[app->n_fonts].size  = size;
        app->fonts[app->n_fonts].style = style;
        app->fonts[app->n_fonts].font  = f;
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
        if (text_width(app, f, text) <= max_w && f->ascent + f->descent <= max_h)
            { best = sz; break; }
    }
    return best;
}

/* Fit by HEIGHT only — width is stretched separately via XRender */
static int fit_font_size_h(App *app, int max_h,
                            int min_sz, int max_sz, int style) {
    int best = min_sz;
    for (int sz = max_sz; sz >= min_sz; sz -= (sz > 60 ? 4 : 2)) {
        XftFont *f = get_font(app, sz, style);
        if (!f) continue;
        if (f->ascent + f->descent <= max_h) { best = sz; break; }
    }
    return best;
}

static void xft_draw_string(App *app, XftDraw *draw, XftFont *font,
                            unsigned long rgb, int x, int y, const char *s) {
    if (!draw || !font || !s || !s[0]) return;
    XftColor col;
    rgb_xft(app, rgb, &col);
    XftDrawStringUtf8(draw, &col, font, x, y, (const FcChar8 *)s, (int)strlen(s));
    XftColorFree(app->dpy, app->vis, app->cmap, &col);
}

static void xft_draw_string_centered(App *app, XftDraw *draw, XftFont *font,
                                     unsigned long rgb, int cx, int y, const char *s) {
    xft_draw_string(app, draw, font, rgb, cx - text_width(app, font, s)/2, y, s);
}

static double ease_smoothstep(double t) {
    if (t <= 0.0) return 0.0;
    if (t >= 1.0) return 1.0;
    return t * t * (3.0 - 2.0 * t);
}

static int next_quote_idx(int cur) {
    if (N_QUOTES <= 1) return 0;
    int idx = rand() % N_QUOTES;
    if (idx == cur) idx = (idx + 1) % N_QUOTES;
    return idx;
}

static long quote_read_ms(const char *text) {
    int words = 0, chars = 0, in_word = 0;
    for (const char *p = text; p && *p; p++) {
        unsigned char ch = (unsigned char)*p;
        if (!isspace(ch)) chars++;
        if (isspace(ch)) {
            in_word = 0;
        } else if (!in_word) {
            words++;
            in_word = 1;
        }
    }

    long ms = 1500 + words * 360 + chars * 18;
    if (ms < 2800) ms = 2800;
    if (ms > 6200) ms = 6200;
    return ms;
}

static void advance_quote(App *app, long now_ms) {
    if (app->quote_idx < 0 || app->quote_idx >= N_QUOTES) {
        app->quote_idx = rand() % N_QUOTES;
        app->quote_prev_idx = app->quote_idx;
        app->quote_cycle_ts = now_ms;
        return;
    }
    if (app->quote_cycle_ts <= 0)
        app->quote_cycle_ts = now_ms;

    long elapsed = now_ms - app->quote_cycle_ts;
    if (elapsed < quote_read_ms(QUOTES[app->quote_idx])) return;

    app->quote_prev_idx = app->quote_idx;
    app->quote_idx = next_quote_idx(app->quote_idx);
    app->quote_cycle_ts = now_ms;
}

static Pixmap render_quote_image(App *app, Drawable target, XftFont *font,
                                 const Theme *th, int w, int h, const char *text) {
    Pixmap pm = XCreatePixmap(app->dpy, target, w, h, app->depth);
    fill_rect(app, pm, th->bg, 0, 0, w, h);

    XftDraw *xd = XftDrawCreate(app->dpy, pm, app->vis, app->cmap);
    if (xd) {
        int x = (w - text_width(app, font, text)) / 2;
        int y = (h + font->ascent - font->descent) / 2;
        xft_draw_string(app, xd, font, th->accent, x, y, text);
        XftDrawDestroy(xd);
    }

    return pm;
}

static void composite_quote_image(App *app, Drawable src, Drawable dst,
                                  int src_w, int src_h,
                                  int dst_x, int dst_y, int dst_w, int dst_h,
                                  double alpha) {
    if (alpha <= 0.01 || dst_w <= 0 || dst_h <= 0) return;
    if (alpha > 1.0) alpha = 1.0;

    XRenderPictFormat *fmt = XRenderFindVisualFormat(app->dpy, app->vis);
    if (!fmt) {
        XCopyArea(app->dpy, src, dst, app->gc, 0, 0, src_w, src_h, dst_x, dst_y);
        return;
    }

    Picture sp = XRenderCreatePicture(app->dpy, src, fmt, 0, NULL);
    Picture dp = XRenderCreatePicture(app->dpy, dst, fmt, 0, NULL);
    XRenderColor mask_col = { 0xffff, 0xffff, 0xffff,
                              (unsigned short)(alpha * 65535.0 + 0.5) };
    Picture mask = XRenderCreateSolidFill(app->dpy, &mask_col);

    XTransform xf;
    memset(&xf, 0, sizeof(xf));
    xf.matrix[0][0] = XDoubleToFixed((double)src_w / (double)dst_w);
    xf.matrix[1][1] = XDoubleToFixed((double)src_h / (double)dst_h);
    xf.matrix[2][2] = XDoubleToFixed(1.0);
    XRenderSetPictureTransform(app->dpy, sp, &xf);
    XRenderSetPictureFilter(app->dpy, sp, FilterBilinear, NULL, 0);

    XRenderComposite(app->dpy, PictOpOver,
                     sp, mask, dp, 0, 0, 0, 0,
                     dst_x, dst_y, dst_w, dst_h);
    XRenderFreePicture(app->dpy, mask);
    XRenderFreePicture(app->dpy, sp);
    XRenderFreePicture(app->dpy, dp);
}

static void draw_quote_morph(App *app, XftDraw *draw, XftFont *font,
                             Drawable buf, const Theme *th,
                             int cx, int baseline, int max_w) {
    (void)draw;
    if (!font) return;

    long now_ms = ms_now();
    advance_quote(app, now_ms);

    const char *cur = QUOTES[app->quote_idx];
    long elapsed = now_ms - app->quote_cycle_ts;
    if (elapsed < 0) elapsed = QUOTE_MORPH_MS;

    int img_w = max_w;
    int img_h = font->ascent + font->descent + 18;
    int img_x = cx - img_w / 2;
    int img_y = baseline - font->ascent - 9;

    if (app->quote_prev_idx == app->quote_idx || elapsed >= QUOTE_MORPH_MS) {
        Pixmap cur_img = render_quote_image(app, buf, font, th, img_w, img_h, cur);
        XCopyArea(app->dpy, cur_img, buf, app->gc, 0, 0, img_w, img_h, img_x, img_y);
        XFreePixmap(app->dpy, cur_img);
        return;
    }

    const char *prev = QUOTES[app->quote_prev_idx];
    double t = ease_smoothstep((double)elapsed / (double)QUOTE_MORPH_MS);
    double pulse = sin(t * M_PI);
    double prev_a = 1.0 - t;
    double cur_a  = t;
    double prev_scale = 1.0 + 0.018 * pulse;
    double cur_scale = 1.0 - 0.018 * pulse;

    Pixmap prev_img = render_quote_image(app, buf, font, th, img_w, img_h, prev);
    Pixmap cur_img  = render_quote_image(app, buf, font, th, img_w, img_h, cur);

    int prev_w = (int)(img_w * prev_scale + 0.5);
    int prev_h = (int)(img_h * prev_scale + 0.5);
    int cur_w  = (int)(img_w * cur_scale + 0.5);
    int cur_h  = (int)(img_h * cur_scale + 0.5);

    composite_quote_image(app, prev_img, buf, img_w, img_h,
                          cx - prev_w / 2, img_y + (img_h - prev_h) / 2,
                          prev_w, prev_h, prev_a);
    composite_quote_image(app, cur_img, buf, img_w, img_h,
                          cx - cur_w / 2, img_y + (img_h - cur_h) / 2,
                          cur_w, cur_h, cur_a);

    XFreePixmap(app->dpy, prev_img);
    XFreePixmap(app->dpy, cur_img);
}

/* Render text stretched to exactly (area_w × area_h) via XRender bilinear scale */
static void draw_text_stretched(App *app, Pixmap buf,
                                 int area_x, int area_y, int area_w, int area_h,
                                 XftFont *f, unsigned long col, unsigned long bg,
                                 const char *text) {
    if (!f || !text || !text[0] || area_w <= 0 || area_h <= 0) return;

    int src_w = text_width(app, f, text);
    int src_h = f->ascent + f->descent;
    if (src_w <= 0 || src_h <= 0) return;

    Pixmap src = XCreatePixmap(app->dpy, buf, src_w, src_h, app->depth);
    fill_rect(app, src, bg, 0, 0, src_w, src_h);
    XftDraw *xd = XftDrawCreate(app->dpy, src, app->vis, app->cmap);
    if (xd) {
        xft_draw_string(app, xd, f, col, 0, f->ascent, text);
        XftDrawDestroy(xd);
    }

    XRenderPictFormat *fmt = XRenderFindVisualFormat(app->dpy, app->vis);
    if (fmt) {
        Picture sp = XRenderCreatePicture(app->dpy, src, fmt, 0, NULL);
        Picture dp = XRenderCreatePicture(app->dpy, buf, fmt, 0, NULL);
        XTransform xf;
        memset(&xf, 0, sizeof(xf));
        xf.matrix[0][0] = XDoubleToFixed((double)src_w / area_w);
        xf.matrix[1][1] = XDoubleToFixed((double)src_h / area_h);
        xf.matrix[2][2] = XDoubleToFixed(1.0);
        XRenderSetPictureTransform(app->dpy, sp, &xf);
        XRenderSetPictureFilter(app->dpy, sp, FilterBilinear, NULL, 0);
        XRenderComposite(app->dpy, PictOpSrc,
                         sp, None, dp, 0, 0, 0, 0,
                         area_x, area_y, area_w, area_h);
        XRenderFreePicture(app->dpy, sp);
        XRenderFreePicture(app->dpy, dp);
    } else {
        int dx = area_x + (area_w - src_w) / 2;
        int dy = area_y + (area_h - src_h) / 2;
        XCopyArea(app->dpy, src, buf, app->gc, 0, 0, src_w, src_h, dx, dy);
    }
    XFreePixmap(app->dpy, src);
}

/* ── Theme / display-color helpers ────────────────────── */

static const Theme *cur_theme(App *app) { return &THEMES[app->theme_idx]; }

/* Desaturated grey theme applied when the config window loses focus.
 * Not in THEMES[] — not selectable by the user. */
static const Theme UNFOCUSED_THEME = {
    "unfocused",
    0x080808, 0x0d0d0d, 0x111111,   /* bg, surface, surface_soft */
    0x464646, 0x282828,              /* text, text_muted */
    0x272727, 0x161616,              /* accent, accent_soft */
    0x1c1c1c, 0x383838,             /* border, danger */
    0x0c0c0c, 0x4a4a4a, 0x1e1e1e,  /* until_chip_bg/fg/border */
    0x606060, 0x0a0a0a,             /* until_clock_fg/bg */
    0x505050, 0x303030,             /* lcd_active, lcd_paused */
};

/* Returns the grey unfocused theme when cfg has lost focus. */
static const Theme *cfg_theme(App *app) {
    return app->cfg_focused ? cur_theme(app) : &UNFOCUSED_THEME;
}

/* Smooth sinusoidal pulse for the shutdown icon: always red, breathing
 * between 25 % and 100 % brightness at ~3.5 Hz to signal urgency. */
static unsigned long shutdown_pulse_color(const Theme *th) {
#if LOW_POWER
    return th->danger;
#else
    double frac   = 0.5 + 0.5 * sin(monotonic_now() * TAU * 3.5);
    double bright = 0.25 + 0.75 * frac;
    int r = (int)(((th->danger >> 16) & 0xff) * bright);
    int g = (int)(((th->danger >>  8) & 0xff) * bright);
    int b = (int)(( th->danger        & 0xff) * bright);
    return ((unsigned long)r << 16) | ((unsigned long)g << 8) | (unsigned long)b;
#endif
}

static unsigned long timer_display_color(App *app) {
    float frac = app->total_seconds > 0
                 ? (float)app->remaining_exact / app->total_seconds : 1.0f;
    if (frac < 0.0f) frac = 0.0f;
    if (frac > 1.0f) frac = 1.0f;

    const Theme *th = cur_theme(app);
    unsigned long color = (frac >= 0.8f) ? th->lcd_active
                        : lerp_rgb(th->lcd_active, th->lcd_paused, (0.8-frac)/0.8);

#if LOW_POWER
    return color;
#else
    double pulse = 0.94 + 0.06 * (0.5 + 0.5 * sin(monotonic_now() * TAU / 5.5));
    int r  = (int)(((color>>16)&0xff) * pulse); if (r>255) r=255; if (r<0) r=0;
    int g  = (int)(((color>>8) &0xff) * pulse); if (g>255) g=255; if (g<0) g=0;
    int b2 = (int)(( color     &0xff) * pulse); if (b2>255) b2=255; if (b2<0) b2=0;
    return ((unsigned long)r<<16)|((unsigned long)g<<8)|(unsigned long)b2;
#endif
}

/* Beat-synced strobe: the screen slams a vivid color on every alarm
 * strike and decays toward black before the next one, so the flashing
 * dances in time with the sound.  A white-hot transient marks the exact
 * strike, and the palette rotates each period for ever-changing colors. */
static unsigned long alert_strobe_color(double elapsed, int frame) {
    (void)frame;
    static const float onset[ALARM_N] = ALARM_ONSETS;
    static const unsigned long pal[ALARM_N] = {
        0xff2020, 0xff5500, 0xffaa00, 0xffff30, 0xff4080, 0xff0040
    };
    /* Once the alarm auto-mutes, the flashing stops too: hold a calm dim. */
    if (elapsed > ALARM_MAX_SECONDS) return 0x0a0a0eUL;

    /* Sound is heard a touch after it is generated; nudge the visuals
     * back slightly so the flash lands on the audible beat. */
    double e = elapsed - 0.08;
    if (e < 0.0) e = 0.0;

    /* Follow the same accelerating phase as the audio so the flashes stay
     * locked to the (speeding-up) beat. */
    double ph     = alarm_phase(e);
    double persec = 1.0 / alarm_speed(e);
    double fp     = ph - floor(ph);
    int    period = (int)floor(ph);

    /* Most recent strike: largest onset fraction not after the position. */
    int idx = 0;
    for (int j = 0; j < ALARM_N; j++)
        if ((double)onset[j] / ALARM_PERIOD <= fp) idx = j;
    double dt = (fp - (double)onset[idx] / ALARM_PERIOD) * persec;

    /* White-hot transient right on the strike. */
    if (dt < 0.022) return 0xffffffUL;

    /* Vivid color, decaying toward black until the next strike. */
    unsigned long c = pal[(idx + period) % ALARM_N];
    double env = exp(-dt * 6.5);
    int r = (int)(((c >> 16) & 0xff) * env);
    int g = (int)(((c >>  8) & 0xff) * env);
    int b = (int)(( c        & 0xff) * env);
    return ((unsigned long)r << 16) | ((unsigned long)g << 8) | (unsigned long)b;
}

/* ── Double-buffer management ──────────────────────────── */

static XftDraw *make_xft_draw(App *app, Drawable d) {
    return XftDrawCreate(app->dpy, d, app->vis, app->cmap);
}

static void ensure_cfg_buf(App *app) {
    if (app->cfg_buf) return;
    if (!app->cfg_win) return;
    app->cfg_buf     = XCreatePixmap(app->dpy, app->cfg_win, CFG_W, CFG_H, app->depth);
    app->cfg_buf_xft = make_xft_draw(app, app->cfg_buf);
}

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

static void ensure_alert_screen_buf(App *app, AlertScreen *s) {
    if (s->buf || !s->win) return;
    s->buf     = XCreatePixmap(app->dpy, s->win, s->w, s->h, app->depth);
    s->buf_xft = make_xft_draw(app, s->buf);
}

/* ── Clamped scale helper ──────────────────────────────── */

static int scaled(int base, int lo, int hi) {
    if (base < lo) return lo;
    if (base > hi) return hi;
    return base;
}

/* ── Power / shutdown icon ─────────────────────────────── */
/*
 *  Drawn with primitives so it looks crisp at any size:
 *
 *        |          ← vertical bar extends above circle
 *      /   \
 *     |     |       ← 280° arc, 80° gap centred at top
 *      \   /
 *
 *  cx/cy  = arc centre
 *  r      = arc radius
 *  Returns the y of the lowest pixel (cy + r) so callers can advance layout.
 */
static int draw_power_icon(App *app, Drawable d,
                            int cx, int cy, int r, unsigned long rgb) {
    int lw = (r + 3) / 4;   /* line width proportional to size */
    if (lw < 1) lw = 1;

    XSetForeground(app->dpy, app->gc, rgb_pixel(app, rgb));
    XSetLineAttributes(app->dpy, app->gc, lw, LineSolid, CapRound, JoinRound);

    /* Arc: 280° extent, 80° gap centred at 12-o'clock.
     * Gap spans 50°–130° (CCW from 3-o'clock).
     * Arc starts at 130° and sweeps 280° CCW → ends at 50°. */
    int ar = r - lw / 2;          /* arc radius inset by half line-width */
    XDrawArc(app->dpy, d, app->gc,
             cx - ar, cy - ar, 2 * ar, 2 * ar,
             130 * 64, 280 * 64);

    /* Vertical bar: from just below centre upward, clearing the gap opening
     * and extending r/2 above the circle for a strong, readable shape. */
    int bar_top    = cy - r - r / 2;
    int bar_bottom = cy + r / 4;
    XDrawLine(app->dpy, d, app->gc, cx, bar_bottom, cx, bar_top);

    /* Reset GC line attributes */
    XSetLineAttributes(app->dpy, app->gc, 0, LineSolid, CapButt, JoinMiter);

    return cy + r;   /* bottom-most pixel */
}

/* ── Config window ─────────────────────────────────────── */

typedef struct {
    int top_y, ly, q_base, in_top, ch_top, ul_top;
    int quote_h, has_chips, has_until, content_h;
} CfgLayout;

static void cfg_compute_layout(App *app, CfgLayout *L) {
    ParseResult *pr = &app->cfg_preview;
    L->has_chips = pr->sw_shutdown || pr->sw_alert ||
                   pr->sw_fullscreen || pr->sw_pausable;
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

    L->top_y = (CFG_H - L->content_h) / 2;
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
    app->cfg_panel_h     = CFG_H - app->cfg_panel_top - 2;
}

static void cfg_present_region(App *app, int y, int h) {
    if (h <= 0) return;
    if (app->cfg_buf)
        XCopyArea(app->dpy, app->cfg_buf, app->cfg_win, app->gc,
                  0, y, CFG_W, h, 0, y);
}

static void cfg_draw_borders(App *app, Drawable buf, const Theme *th) {
    fill_rect(app, buf, th->accent, 0, 0, CFG_W, 2);
    fill_rect(app, buf, th->accent, 0, CFG_H - 2, CFG_W, 2);
    fill_rect(app, buf, th->accent, 0, 0, 2, CFG_H);
    fill_rect(app, buf, th->accent, CFG_W - 2, 0, 2, CFG_H);
}

static void cfg_present_borders(App *app) {
    if (!app->cfg_buf) return;
    XCopyArea(app->dpy, app->cfg_buf, app->cfg_win, app->gc,
              0, 0, CFG_W, 2, 0, 0);
    XCopyArea(app->dpy, app->cfg_buf, app->cfg_win, app->gc,
              0, CFG_H - 2, CFG_W, 2, 0, CFG_H - 2);
    XCopyArea(app->dpy, app->cfg_buf, app->cfg_win, app->gc,
              0, 0, 2, CFG_H, 0, 0);
    XCopyArea(app->dpy, app->cfg_buf, app->cfg_win, app->gc,
              CFG_W - 2, 0, 2, CFG_H, CFG_W - 2, 0);
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

    int r = 155, lx = CFG_W / 2, lcy = L->ly;
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
    XSetLineAttributes(app->dpy, app->gc, 1, LineSolid, CapRound, JoinRound);
  {
        double sl = r * 0.84, tail = r * 0.18;
        XDrawLine(app->dpy, buf, app->gc,
                  lx - (int)(tail * sin(ang_s)), lcy + (int)(tail * cos(ang_s)),
                  lx + (int)(sl   * sin(ang_s)), lcy - (int)(sl   * cos(ang_s)));
    }

    XSetForeground(app->dpy, app->gc, rgb_pixel(app, th->accent));
    XFillArc(app->dpy, buf, app->gc,
             lx - dot_rad, lcy - dot_rad, 2*dot_rad, 2*dot_rad, 0, 360*64);
    int cap = dot_rad / 2;
    XSetForeground(app->dpy, app->gc, rgb_pixel(app, th->surface));
    XFillArc(app->dpy, buf, app->gc, lx-cap, lcy-cap, 2*cap, 2*cap, 0, 360*64);
    XSetLineAttributes(app->dpy, app->gc, 0, LineSolid, CapButt, JoinMiter);
}

static void cfg_draw_panel(App *app, Drawable buf, XftDraw *draw,
                           const Theme *th, const CfgLayout *L) {
    const int input_h = 54;
    const int chip_h  = 24;
    ParseResult *pr = &app->cfg_preview;

    int fx = 24, fy = L->in_top, fw = CFG_W - 48, fh = input_h;
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

    struct { int active; const char *label;
             unsigned long bg, fg, border; } chips[] = {
        { pr->sw_shutdown,   "d  shutdown",   th->danger,      0xf0e8d8UL, th->danger },
        { pr->sw_alert,      "a  alert",      th->accent_soft, th->accent, th->border },
        { pr->sw_fullscreen, "f  fullscreen", th->accent_soft, th->accent, th->border },
        { pr->sw_pausable,   "p  pausable",   th->accent_soft, th->accent, th->border },
    };
    XftFont *cf = get_font(app, 9, 1);
    if (cf) {
        int n_active = 0, chip_widths[4] = {0};
        for (int i = 0; i < 4; i++) {
            if (!chips[i].active) continue;
            chip_widths[i] = text_width(app, cf, chips[i].label) + 22;
            n_active++;
        }
        if (n_active > 0) {
            int total_w = -6;
            for (int i = 0; i < 4; i++)
                if (chips[i].active) total_w += chip_widths[i] + 6;
            int cx2 = (CFG_W - total_w) / 2, cy = L->ch_top;
            for (int i = 0; i < 4; i++) {
                if (!chips[i].active) continue;
                int cw = chip_widths[i];
                fill_rect(app, buf, chips[i].bg, cx2, cy, cw, chip_h);
                draw_rect_outline(app, buf, chips[i].border, cx2, cy, cw, chip_h);
                int tw2 = text_width(app, cf, chips[i].label);
                xft_draw_string(app, draw, cf, chips[i].fg,
                                cx2 + (cw - tw2) / 2, cy + cf->ascent + 2,
                                chips[i].label);
                cx2 += cw + 6;
            }
        }
    }

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
        int bx = (CFG_W - bw) / 2;
        int uy = L->ul_top;

        fill_rect(app, buf, th->until_chip_bg, bx, uy, bw, bh);
        draw_rect_outline(app, buf, th->until_chip_border, bx, uy, bw, bh);
        int y_sml = uy + 6 + (fs ? fs->ascent : 11);
        if (fs) xft_draw_string_centered(app, draw, fs, th->text_muted,
                                          CFG_W / 2, y_sml, sml);
        int y_big = y_sml + (fs ? fs->descent : 2) + 5 + (fb ? fb->ascent : 25);
        if (fb && big[0])
            xft_draw_string_centered(app, draw, fb, th->until_clock_fg,
                                     CFG_W / 2, y_big, big);
    }
}

void draw_config_anim(App *app) {
    if (!app->cfg_win) return;
    ensure_cfg_buf(app);
    const Theme *th = cfg_theme(app);
    Drawable buf  = app->cfg_buf ? (Drawable)app->cfg_buf : (Drawable)app->cfg_win;
    XftDraw *draw = app->cfg_buf ? app->cfg_buf_xft : app->cfg_xft;
    if (!draw) return;

    CfgLayout L;
    cfg_compute_layout(app, &L);
    int y = app->cfg_anim_top;
    int h = app->cfg_anim_h;
    if (h <= 0) return;

    fill_rect(app, buf, th->bg, 0, y, CFG_W, h);
    cfg_draw_clock(app, buf, th, &L);
    XftFont *qf = get_font(app, 16, 2);
    if (!qf) qf = get_font(app, 16, 0);
    if (qf) draw_quote_morph(app, draw, qf, buf, th, CFG_W / 2, L.q_base, CFG_W - 48);
    cfg_draw_borders(app, buf, th);
    cfg_present_region(app, y, h);
    cfg_present_borders(app);
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

    fill_rect(app, buf, th->bg, 0, y, CFG_W, h);
    cfg_draw_panel(app, buf, draw, th, &L);
    cfg_draw_borders(app, buf, th);
    cfg_present_region(app, y, h);
    cfg_present_borders(app);
}

void draw_config(App *app) {
    if (!app->cfg_win) return;
    ensure_cfg_buf(app);
    const Theme *th = cfg_theme(app);
    int W = CFG_W, H = CFG_H;
    Drawable buf  = app->cfg_buf ? (Drawable)app->cfg_buf : (Drawable)app->cfg_win;
    XftDraw *draw = app->cfg_buf ? app->cfg_buf_xft : app->cfg_xft;
    if (!draw) return;

    cfg_refresh_preview(app);
    cfg_update_layout(app);

    fill_rect(app, buf, th->bg, 0, 0, W, H);

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
        draw_config_panel(app);
    }
}

void create_config_window(App *app) {
    const Theme *th = cur_theme(app);
    Window root_ret, child_ret; int rx, ry, wx, wy; unsigned int mask;
    Window root = DefaultRootWindow(app->dpy);
    XQueryPointer(app->dpy, root, &root_ret, &child_ret, &rx, &ry, &wx, &wy, &mask);
    detect_monitor(app, rx, ry);

    int x = app->mon_x + (app->mon_w - CFG_W)/2;
    int y = app->mon_y + (app->mon_h - CFG_H)/2;
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
    app->cfg_win = XCreateWindow(app->dpy, root, x, y, CFG_W, CFG_H, 0,
        app->depth, InputOutput, app->vis,
        CWOverrideRedirect|CWBackPixel|CWBorderPixel|CWColormap|CWEventMask, &swa);

    XStoreName(app->dpy, app->cfg_win, "Timer");
    XMapRaised(app->dpy, app->cfg_win);
    XFlush(app->dpy);
    /* Focus is requested in the MapNotify handler once the window is viewable. */
    app->cfg_xft = make_xft_draw(app, app->cfg_win);
    app->cursor_blink = 1;
    app->cursor_blink_ts = ms_now();
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
                W = MINI_W;
                H = has_until ? MINI_H_UNTIL : MINI_H_BASE;
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
            W = MINI_W;
            H = has_until ? MINI_H_UNTIL : MINI_H_BASE;
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
                              | ButtonReleaseMask | StructureNotifyMask;
        app->tmr_win = XCreateWindow(app->dpy, root, X, Y, W, H, 0,
            app->depth, InputOutput, app->vis,
            CWOverrideRedirect|CWBackPixel|CWBorderPixel|CWColormap|CWEventMask, &swa);

        XWMHints hints;
        memset(&hints, 0, sizeof(hints));
        hints.flags = InputHint;
        hints.input = False;
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

    XStoreName(app->dpy, app->tmr_win, "Timer");
    XMapRaised(app->dpy, app->tmr_win);
    XFlush(app->dpy);
    app->tmr_xft = make_xft_draw(app, app->tmr_win);
}

/* ── Alert windows ─────────────────────────────────────── */

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
    if (elapsed > ALARM_MAX_SECONDS && app->alert_audio_active) {
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
    swa.event_mask        = ExposureMask | KeyPressMask | ButtonPressMask;

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
