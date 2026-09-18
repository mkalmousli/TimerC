/* draw.c — All X11 UI rendering and window management */
#include "timer.h"

/* Default to a lightweight render path (less CPU/GPU, fewer animations). */
#ifndef LOW_POWER
#define LOW_POWER 1
#endif

/* ── Theme data (procedurally generated at runtime) ───────
 * No fixed palettes: every run, generate_themes() below builds
 * N_THEMES fresh black-mode palettes. Each theme's accent hue is
 * spread evenly around the color wheel using golden-angle steps (so
 * hues never bunch together), and the accent/danger colors are solved
 * against the near-black background using the WCAG contrast formula
 * so they land at a comfortably legible lightness instead of a
 * guessed hex value — background, surfaces, borders and text all
 * share that one hue at low saturation, so each theme reads as one
 * coherent tinted-black room rather than mismatched colors. */
Theme THEMES[N_THEMES];

static const char *QUOTES[] = {
    "Focus now.",
    "No excuses today.",
    "Ship it.",
    "Discipline over mood.",
    "Time won't wait.",
    "Do the work.",
    "Stay on task.",
    "Small steps count.",
    "Finish what matters.",
    "Cut the noise.",
    "One task only.",
    "Start. Just start.",
    "Progress beats perfect.",
    "Show up daily.",
    "Silence the doubt.",
    "Move with purpose.",
    "Less talk, work.",
    "Own this hour.",
    "Build, don't scroll.",
    "Momentum is earned.",
};
#define N_QUOTES ((int)(sizeof(QUOTES)/sizeof(QUOTES[0])))
#define QUOTE_MORPH_MS  1000
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

/* ── Procedural theme generation ──────────────────────────
 * hsl_rgb / rel_luminance / contrast_ratio implement standard HSL->RGB
 * conversion and the WCAG relative-luminance contrast formula, used by
 * generate_themes() (below the font/text helpers, since it also wants
 * get_font-adjacent color logic kept together) to pick colors that are
 * both harmonious (shared hue, golden-angle spacing) and legible
 * (contrast-checked against the background) rather than hand-picked. */
static double hue_wrap(double v, double m) {
    double r = fmod(v, m);
    return (r < 0.0) ? r + m : r;
}

static unsigned long hsl_rgb(double h, double s, double l) {
    h = hue_wrap(h, 360.0);
    if (s < 0.0) s = 0.0;
    if (s > 1.0) s = 1.0;
    if (l < 0.0) l = 0.0;
    if (l > 1.0) l = 1.0;
    double c  = (1.0 - fabs(2.0 * l - 1.0)) * s;
    double hp = h / 60.0;
    double x  = c * (1.0 - fabs(fmod(hp, 2.0) - 1.0));
    double r1 = 0, g1 = 0, b1 = 0;
    if      (hp < 1) { r1 = c; g1 = x; }
    else if (hp < 2) { r1 = x; g1 = c; }
    else if (hp < 3) { g1 = c; b1 = x; }
    else if (hp < 4) { g1 = x; b1 = c; }
    else if (hp < 5) { r1 = x; b1 = c; }
    else             { r1 = c; b1 = x; }
    double m = l - c / 2.0;
    int r = (int)lround((r1 + m) * 255.0);
    int g = (int)lround((g1 + m) * 255.0);
    int b = (int)lround((b1 + m) * 255.0);
    if (r < 0) r = 0;
    if (r > 255) r = 255;
    if (g < 0) g = 0;
    if (g > 255) g = 255;
    if (b < 0) b = 0;
    if (b > 255) b = 255;
    return ((unsigned long)r << 16) | ((unsigned long)g << 8) | (unsigned long)b;
}

static double srgb_channel_lin(double c) {
    return c <= 0.03928 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4);
}
static double rel_luminance(unsigned long rgb) {
    double r = ((rgb >> 16) & 0xff) / 255.0;
    double g = ((rgb >> 8)  & 0xff) / 255.0;
    double b = ( rgb        & 0xff) / 255.0;
    return 0.2126 * srgb_channel_lin(r) + 0.7152 * srgb_channel_lin(g) + 0.0722 * srgb_channel_lin(b);
}
static double contrast_ratio(unsigned long a, unsigned long b) {
    double la = rel_luminance(a) + 0.05;
    double lb = rel_luminance(b) + 0.05;
    return la > lb ? la / lb : lb / la;
}

/* Nearest named hue, purely for a human-readable theme name. */
static const char *hue_name(double h) {
    static const struct { double hue; const char *name; } names[] = {
        {   0, "Crimson" }, {  20, "Ember"   }, {  35, "Amber"  }, {  50, "Gold"    },
        {  70, "Olive"   }, { 100, "Moss"    }, { 140, "Jade"   }, { 165, "Teal"    },
        { 190, "Cyan"    }, { 210, "Azure"   }, { 230, "Cobalt" }, { 255, "Indigo"  },
        { 275, "Violet"  }, { 295, "Plum"    }, { 315, "Magenta"}, { 335, "Rose"    },
    };
    double best_d = 1e9; const char *best = "Slate";
    for (size_t i = 0; i < sizeof(names)/sizeof(names[0]); i++) {
        double d = fabs(h - names[i].hue);
        if (d > 180.0) d = 360.0 - d;
        if (d < best_d) { best_d = d; best = names[i].name; }
    }
    return best;
}

/* Walk lightness upward at a fixed hue/saturation until the color
 * clears target_contrast against bg — "smart" color choice instead of
 * a guessed constant, tuned per-theme since backgrounds differ. */
static double fit_lightness(double hue, double sat, unsigned long bg, double target_contrast) {
    double best_l = 0.86;
    for (double l = 0.45; l <= 0.86; l += 0.02) {
        unsigned long c = hsl_rgb(hue, sat, l);
        if (contrast_ratio(c, bg) >= target_contrast) { best_l = l; break; }
        best_l = l;
    }
    return best_l;
}

static char g_theme_names[N_THEMES][24];

/* Time-of-day mood: a smooth 24h arc of keyframes carrying this run's
 * palettes from restful midnight indigo through cyan dawn, crisp azure
 * midday, teal afternoon, warm amber sunset, and violet dusk, back to
 * indigo — so re-generating at 9am vs 9pm actually feels different, and
 * the feel changes gradually across the day rather than snapping between
 * a few fixed buckets. `sat` is the accent's saturation target and also
 * scales the background/surface/border saturation below it; `bg_l` is
 * the background's target lightness (nights sit a little darker, midday
 * a little brighter); `contrast` is the WCAG target fit_lightness solves
 * the accent against. */
typedef struct { double hour, hue, sat, bg_l, contrast; } DayAnchor;
static const DayAnchor DAY_ANCHORS[] = {
    {  0.0, 255.0, 0.42, 0.015, 4.2 },  /* midnight — dim indigo */
    {  5.0, 230.0, 0.40, 0.018, 4.3 },  /* predawn — cobalt */
    {  7.5, 190.0, 0.46, 0.023, 4.5 },  /* dawn — cyan */
    { 12.0, 205.0, 0.50, 0.028, 4.8 },  /* midday — crisp azure */
    { 17.0, 150.0, 0.48, 0.024, 4.6 },  /* afternoon — teal/jade */
    { 19.0,  32.0, 0.55, 0.020, 4.6 },  /* sunset — warm amber */
    { 21.5, 280.0, 0.46, 0.016, 4.3 },  /* dusk — violet */
    { 24.0, 255.0, 0.42, 0.015, 4.2 },  /* wraps back to midnight indigo */
};
#define N_DAY_ANCHORS ((int)(sizeof(DAY_ANCHORS)/sizeof(DAY_ANCHORS[0])))

/* Shortest circular path between two hues (never the "long way round"). */
static double hue_lerp_shortest(double a, double b, double t) {
    double d = hue_wrap(b - a + 180.0, 360.0) - 180.0;
    return hue_wrap(a + d * t, 360.0);
}

static void day_mood(double hour_frac, double *hue, double *sat,
                      double *bg_l, double *contrast) {
    int i = 0;
    while (i < N_DAY_ANCHORS - 2 && DAY_ANCHORS[i+1].hour <= hour_frac) i++;
    const DayAnchor *a = &DAY_ANCHORS[i], *b = &DAY_ANCHORS[i+1];
    double span = b->hour - a->hour;
    double t = span > 0.0 ? (hour_frac - a->hour) / span : 0.0;
    *hue      = hue_lerp_shortest(a->hue, b->hue, t);
    *sat      = a->sat      + (b->sat      - a->sat)      * t;
    *bg_l     = a->bg_l     + (b->bg_l     - a->bg_l)     * t;
    *contrast = a->contrast + (b->contrast - a->contrast) * t;
}

/* Build N_THEMES fresh black-mode palettes, re-rolled every call (at
 * launch, and again on every Tab press) and shaped by the current wall-
 * clock time via day_mood() above. Each call still produces N_THEMES
 * distinct-but-related hues — a random base plus golden-ish jitter
 * spread across a narrow arc around the moment's mood hue — so cycling
 * through them feels like variety on a theme, not a random grab bag. */
void generate_themes(void) {
    time_t now_t = time(NULL);
    struct tm *lt = localtime(&now_t);
    double hour_frac = lt->tm_hour + lt->tm_min / 60.0 + lt->tm_sec / 3600.0;

    double mood_hue, mood_sat, mood_bg_l, mood_contrast;
    day_mood(hour_frac, &mood_hue, &mood_sat, &mood_bg_l, &mood_contrast);

    const double ARC = 40.0;
    double step      = ARC / N_THEMES;
    double base_hue  = mood_hue - ARC / 2.0 + ((double)(rand() % 10000) / 10000.0) * step;

    for (int i = 0; i < N_THEMES; i++) {
        double jitter = (((double)(rand() % 10000) / 10000.0) - 0.5) * step * 0.6;
        double hue = hue_wrap(base_hue + step * i + jitter, 360.0);

        /* Background/surfaces/border: near-black, faintly tinted by hue,
         * scaled by this moment's mood lightness/saturation. */
        double sat_scale = mood_sat / 0.50;
        unsigned long bg           = hsl_rgb(hue, 0.32 * sat_scale, mood_bg_l);
        unsigned long surface      = hsl_rgb(hue, 0.28 * sat_scale, mood_bg_l * 2.25);
        unsigned long surface_soft = hsl_rgb(hue, 0.24 * sat_scale, mood_bg_l * 4.0);
        unsigned long border       = hsl_rgb(hue, 0.18 * sat_scale, mood_bg_l * 7.5);

        /* Text: near-white and muted grey, barely tinted — reads as
         * plain black-mode text, not colored. */
        unsigned long text       = hsl_rgb(hue, 0.05, 0.93);
        unsigned long text_muted = hsl_rgb(hue, 0.07, 0.50);

        /* Accent: the theme's real color — saturated to match the
         * moment's mood, with its lightness solved against bg for the
         * mood's WCAG contrast target. */
        double accent_l = fit_lightness(hue, mood_sat, bg, mood_contrast);
        unsigned long accent      = hsl_rgb(hue, mood_sat, accent_l);
        unsigned long accent_soft = hsl_rgb(hue, 0.34 * sat_scale, mood_bg_l * 6.0);

        /* Danger stays true red for universal "time's up" urgency, but
         * its lightness is solved the same way so it's always legible
         * against this particular background. */
        double danger_l = fit_lightness(2.0, 0.70, bg, mood_contrast);
        unsigned long danger = hsl_rgb(2.0, 0.70, danger_l);

        Theme *t = &THEMES[i];
        snprintf(g_theme_names[i], sizeof(g_theme_names[i]), "%s", hue_name(hue));
        t->name = g_theme_names[i];
        t->bg = bg; t->surface = surface; t->surface_soft = surface_soft;
        t->text = text; t->text_muted = text_muted;
        t->accent = accent; t->accent_soft = accent_soft;
        t->border = border; t->danger = danger;
        t->until_chip_bg = surface; t->until_chip_fg = text; t->until_chip_border = border;
        t->until_clock_fg = accent; t->until_clock_bg = bg;
        t->lcd_active = accent; t->lcd_paused = text_muted;
    }
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

static double ease_in_out_cubic(double t) {
    if (t <= 0.0) return 0.0;
    if (t >= 1.0) return 1.0;
    return (t < 0.5) ? 4.0 * t * t * t : 1.0 - pow(-2.0 * t + 2.0, 3.0) / 2.0;
}

static double pos_mod(double v, double m) {
    if (m <= 0.0) return 0.0;
    double r = fmod(v, m);
    return (r < 0.0) ? r + m : r;
}

static int next_quote_idx(int cur) {
    if (N_QUOTES <= 1) return 0;
    int idx = rand() % N_QUOTES;
    if (idx == cur) idx = (idx + 1) % N_QUOTES;
    return idx;
}

static long quote_hold_ms(const char *text) {
    int chars = 0;
    for (const char *p = text; p && *p; p++)
        if (!isspace((unsigned char)*p)) chars++;

    long ms = 2600 + chars * 24;
    if (ms < 3800) ms = 3800;
    if (ms > 5200) ms = 5200;
    return ms;
}

static long quote_morph_start_ms(const char *text) {
    long total = quote_hold_ms(text);
    long start = total - QUOTE_MORPH_MS;
    if (start < 2000) start = 2000;
    return start;
}

static void quote_tick_cycle(App *app, long now_ms) {
    if (app->quote_idx < 0 || app->quote_idx >= N_QUOTES) {
        app->quote_idx = rand() % N_QUOTES;
        app->quote_next_idx = next_quote_idx(app->quote_idx);
        app->quote_cycle_ts = now_ms;
        return;
    }
    if (app->quote_cycle_ts <= 0) {
        app->quote_cycle_ts = now_ms;
        if (app->quote_next_idx < 0 || app->quote_next_idx >= N_QUOTES)
            app->quote_next_idx = next_quote_idx(app->quote_idx);
        return;
    }

    long elapsed = now_ms - app->quote_cycle_ts;
    if (elapsed < quote_hold_ms(QUOTES[app->quote_idx])) return;

    app->quote_idx = app->quote_next_idx;
    app->quote_next_idx = next_quote_idx(app->quote_idx);
    app->quote_cycle_ts = now_ms;
}

void draw_release_quote_cache(App *app) {
    for (int i = 0; i < 2; i++) {
        if (app->quote_mask_pic[i]) {
            XRenderFreePicture(app->dpy, app->quote_mask_pic[i]);
            app->quote_mask_pic[i] = None;
        }
        if (app->quote_mask_px[i]) {
            XFreePixmap(app->dpy, app->quote_mask_px[i]);
            app->quote_mask_px[i] = 0;
        }
        app->quote_mask_idx[i] = -1;
    }
}

/* Rasterize QUOTES[q_idx] once into an off-screen A8 alpha-coverage
 * picture (an actual "text as an image" — a pure glyph-shape mask, no
 * color baked in) and cache it in the given slot, keyed by quote index
 * so it's only rebuilt when that slot's quote actually changes. This is
 * what draw_quote_image() below scales/slides/blends every frame instead
 * of re-rasterizing glyphs each time — the morph transforms an image,
 * it doesn't re-run text layout. */
static Picture get_quote_mask(App *app, int slot, int q_idx, XftFont *font,
                              int *out_w, int *out_h) {
    if (q_idx < 0 || q_idx >= N_QUOTES || !font) return None;

    if (app->quote_mask_idx[slot] == q_idx && app->quote_mask_pic[slot] != None) {
        *out_w = app->quote_mask_w[slot];
        *out_h = app->quote_mask_h[slot];
        return app->quote_mask_pic[slot];
    }

    if (app->quote_mask_pic[slot]) {
        XRenderFreePicture(app->dpy, app->quote_mask_pic[slot]);
        app->quote_mask_pic[slot] = None;
    }
    if (app->quote_mask_px[slot]) {
        XFreePixmap(app->dpy, app->quote_mask_px[slot]);
        app->quote_mask_px[slot] = 0;
    }
    app->quote_mask_idx[slot] = -1;

    const char *text = QUOTES[q_idx];
    int w = text_width(app, font, text);
    int h = font->ascent + font->descent;
    if (w <= 0 || h <= 0) return None;

    XRenderPictFormat *a8 = XRenderFindStandardFormat(app->dpy, PictStandardA8);
    if (!a8) return None;

    Pixmap px = XCreatePixmap(app->dpy, DefaultRootWindow(app->dpy), w, h, 8);
    if (!px) return None;
    Picture pic = XRenderCreatePicture(app->dpy, px, a8, 0, NULL);
    if (!pic) { XFreePixmap(app->dpy, px); return None; }

    XRenderColor clear = {0, 0, 0, 0};
    XRenderFillRectangle(app->dpy, PictOpSrc, pic, &clear, 0, 0, w, h);

    XftDraw *xd = XftDrawCreateAlpha(app->dpy, px, 8);
    if (xd) {
        XftColor col;
        memset(&col, 0, sizeof(col));
        col.color.red = col.color.green = col.color.blue = col.color.alpha = 0xffff;
        XftDrawStringUtf8(xd, &col, font, 0, font->ascent,
                          (const FcChar8 *)text, (int)strlen(text));
        XftDrawDestroy(xd);
    }

    app->quote_mask_idx[slot] = q_idx;
    app->quote_mask_px[slot]  = px;
    app->quote_mask_pic[slot] = pic;
    app->quote_mask_w[slot]   = w;
    app->quote_mask_h[slot]   = h;
    *out_w = w; *out_h = h;
    return pic;
}

/* Composite one glyph-shape mask onto `dest_buf`, scaled and repositioned
 * via an XRenderComposite transform (the same bilinear-scale technique
 * draw_text_stretched() uses above, but blended with PictOpOver through
 * a solid color + alpha instead of PictOpSrc-overwriting pixels) — real
 * image compositing, not a glyph re-draw. */
static void composite_quote_mask(App *app, Drawable dest_buf,
                                 Picture mask, int mw, int mh, XftFont *font,
                                 unsigned long color, double alpha,
                                 int cx, double baseline_px, double scale) {
    if (mask == None || alpha <= 0.004 || mw <= 0 || mh <= 0 || scale <= 0.01) return;
    if (alpha > 1.0) alpha = 1.0;

    XRenderPictFormat *dfmt = XRenderFindVisualFormat(app->dpy, app->vis);
    if (!dfmt) return;
    Picture dst = XRenderCreatePicture(app->dpy, dest_buf, dfmt, 0, NULL);
    if (!dst) return;

    XRenderColor rc;
    rc.red   = (unsigned short)(((color >> 16) & 0xff) * 257);
    rc.green = (unsigned short)(((color >> 8)  & 0xff) * 257);
    rc.blue  = (unsigned short)(( color         & 0xff) * 257);
    rc.alpha = (unsigned short)(alpha * 65535.0 + 0.5);
    Picture src = XRenderCreateSolidFill(app->dpy, &rc);
    if (src) {
        int area_w = (int)lround(mw * scale);
        int area_h = (int)lround(mh * scale);
        if (area_w > 0 && area_h > 0) {
            int area_x = cx - area_w / 2;
            int area_y = (int)lround(baseline_px - font->ascent * scale);

            XTransform xf;
            memset(&xf, 0, sizeof(xf));
            xf.matrix[0][0] = XDoubleToFixed((double)mw / area_w);
            xf.matrix[1][1] = XDoubleToFixed((double)mh / area_h);
            xf.matrix[2][2] = XDoubleToFixed(1.0);
            XRenderSetPictureTransform(app->dpy, mask, &xf);
            XRenderSetPictureFilter(app->dpy, mask, FilterBilinear, NULL, 0);

            XRenderComposite(app->dpy, PictOpOver, src, mask, dst,
                             0, 0, 0, 0, area_x, area_y, area_w, area_h);
        }
        XRenderFreePicture(app->dpy, src);
    }
    XRenderFreePicture(app->dpy, dst);
}

/* One quote "frame": drop shadow + accent glyphs, same look as the old
 * plain text draw, plus — only while actually moving (dy != 0) — a
 * couple of extra copies smeared along the motion at falling alpha. That
 * smear is the whole trick behind the "buttery melt" feel: it's not a
 * real blur, just 2 cheap extra composites, but combined with the scale
 * + slide + long cross-fade overlap below it reads as one shape flowing
 * into the next rather than a flat opacity crossfade. */
static void draw_quote_image(App *app, Drawable buf, XftFont *font, const Theme *th,
                             Picture mask, int mw, int mh,
                             int cx, int baseline, double alpha, double scale, double dy) {
    if (mask == None || alpha <= 0.004) return;

    if (fabs(dy) > 0.4) {
        composite_quote_mask(app, buf, mask, mw, mh, font, th->accent,
                             alpha * 0.10, cx, baseline + dy * 2.2, scale);
        composite_quote_mask(app, buf, mask, mw, mh, font, th->accent,
                             alpha * 0.18, cx, baseline + dy * 1.3, scale);
    }
    composite_quote_mask(app, buf, mask, mw, mh, font, th->text_muted,
                         alpha * 0.45, cx + 1, baseline + dy + 1, scale);
    composite_quote_mask(app, buf, mask, mw, mh, font, th->accent,
                         alpha, cx, baseline + dy, scale);
}

/* Full-duration overlapping crossfade with motion: the outgoing quote
 * shrinks, drifts up and fades while the incoming quote drifts down into
 * place, grows to full size and fades in — both visible together the
 * whole time, never sequential — so it reads as one continuous morph
 * rather than two flat fades. Both quotes are cached glyph-mask images
 * (get_quote_mask), so nothing here re-rasterizes text. */
static void draw_quote_morph(App *app, XftDraw *draw, XftFont *font,
                             Drawable buf, const Theme *th,
                             int cx, int baseline, int max_w) {
    (void)draw;
    (void)max_w;
    if (!font) return;

    long now_ms = ms_now();
    quote_tick_cycle(app, now_ms);

    long elapsed = now_ms - app->quote_cycle_ts;
    if (elapsed < 0) elapsed = 0;

    long morph_start = quote_morph_start_ms(QUOTES[app->quote_idx]);
    int morphing = elapsed >= morph_start;
    long morph_elapsed = morphing ? elapsed - morph_start : 0;
    if (morph_elapsed > QUOTE_MORPH_MS) morph_elapsed = QUOTE_MORPH_MS;

    int mw0 = 0, mh0 = 0;
    Picture m0 = get_quote_mask(app, 0, app->quote_idx, font, &mw0, &mh0);

    if (!morphing) {
        draw_quote_image(app, buf, font, th, m0, mw0, mh0, cx, baseline, 1.0, 1.0, 0.0);
        return;
    }

    int mw1 = 0, mh1 = 0;
    Picture m1 = get_quote_mask(app, 1, app->quote_next_idx, font, &mw1, &mh1);

    double t = ease_in_out_cubic((double)morph_elapsed / (double)QUOTE_MORPH_MS);

    double out_a     = 1.0 - t;
    double out_scale = 1.0 - 0.10 * t;
    double out_dy    = -7.0 * t;

    double in_a     = t;
    double in_scale = 1.10 - 0.10 * t;
    double in_dy    = 7.0 * (1.0 - t);

    if (out_a > 0.008)
        draw_quote_image(app, buf, font, th, m0, mw0, mh0, cx, baseline, out_a, out_scale, out_dy);
    if (in_a > 0.008)
        draw_quote_image(app, buf, font, th, m1, mw1, mh1, cx, baseline, in_a, in_scale, in_dy);
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
    if (elapsed > g_config.alarm_max_seconds) return 0x0a0a0eUL;

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
        if ((double)onset[j] / g_config.alarm_period <= fp) idx = j;
    double dt = (fp - (double)onset[idx] / g_config.alarm_period) * persec;

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
    int w = app->insights_open ? INS_W : g_config.cfg_w;
    int h = app->insights_open ? INS_H : g_config.cfg_h;
    if (app->cfg_buf && app->cfg_buf_w == w && app->cfg_buf_h == h) return;
    if (app->cfg_buf_xft) { XftDrawDestroy(app->cfg_buf_xft); app->cfg_buf_xft = NULL; }
    if (app->cfg_buf)     { XFreePixmap(app->dpy, app->cfg_buf); app->cfg_buf = 0; }
    if (!app->cfg_win) return;
    app->cfg_buf_w   = w;
    app->cfg_buf_h   = h;
    app->cfg_buf     = XCreatePixmap(app->dpy, app->cfg_win, w, h, app->depth);
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

static void cfg_draw_borders_wh(App *app, Drawable buf, const Theme *th, int W, int H) {
    fill_rect(app, buf, th->accent, 0, 0, W, 2);
    fill_rect(app, buf, th->accent, 0, H - 2, W, 2);
    fill_rect(app, buf, th->accent, 0, 0, 2, H);
    fill_rect(app, buf, th->accent, W - 2, 0, 2, H);
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

static void cfg_draw_panel(App *app, Drawable buf, XftDraw *draw,
                           const Theme *th, const CfgLayout *L) {
    const int input_h = 54;
    const int chip_h  = 24;
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

    struct { int active; const char *label;
             unsigned long bg, fg, border; } chips[] = {
        { pr->sw_shutdown,   "d  shutdown",   th->danger,      0xf0e8d8UL, th->danger },
        { pr->sw_alert,      "a  alert",      th->accent_soft, th->accent, th->border },
        { pr->sw_fullscreen, "f  fullscreen", th->accent_soft, th->accent, th->border },
        { pr->sw_pausable,   "p  pausable",   th->accent_soft, th->accent, th->border },
        { pr->sw_lock,       "l  lock input", th->danger,      0xf0e8d8UL, th->danger },
        { pr->sw_sleep,      "/  sleep",      th->accent_soft, th->accent, th->border },
    };
    XftFont *cf = get_font(app, 9, 1);
    if (cf) {
        int n_active = 0, chip_widths[6] = {0};
        for (int i = 0; i < 6; i++) {
            if (!chips[i].active) continue;
            chip_widths[i] = text_width(app, cf, chips[i].label) + 22;
            n_active++;
        }
        if (n_active > 0) {
            int total_w = -6;
            for (int i = 0; i < 6; i++)
                if (chips[i].active) total_w += chip_widths[i] + 6;
            int cx2 = (g_config.cfg_w - total_w) / 2, cy = L->ch_top;
            for (int i = 0; i < 6; i++) {
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

void cfg_draw_cyber_bg(App *app, Drawable buf, const Theme *th,
                       int x, int y, int w, int h, double t) {
    fill_rect(app, buf, th->bg, x, y, w, h);

    /* Slow ambient gradient — wide bands, clearly visible drift */
    const int band = 20;
    int bands = (h + band - 1) / band;
    for (int i = 0; i < bands; i++) {
        int by = y + i * band;
        int bh = (by + band > y + h) ? (y + h - by) : band;
        double wave = 0.5 + 0.5 * sin(t * 0.95 + i * 0.42);
        unsigned long c = lerp_rgb(th->bg, th->accent_soft, 0.10 + 0.16 * wave);
        fill_rect(app, buf, c, x, by, w, bh);
    }

    /* Grid — continuous scroll, stronger lines for readability */
    const int grid = 32;
    unsigned long grid_col = lerp_rgb(th->bg, th->accent, 0.14);
    double gy = pos_mod(t * 16.0, (double)grid);
    for (int row = y + (int)gy; row < y + h; row += grid)
        fill_rect(app, buf, grid_col, x, row, w, 1);
    double gx = pos_mod(t * 10.0, (double)grid);
    for (int col = x + (int)gx; col < x + w; col += grid)
        fill_rect(app, buf, lerp_rgb(th->bg, th->accent, 0.10), col, y, 1, h);

    /* Scan beam — smooth sweep with soft trail */
    double scan = pos_mod(t * 48.0, (double)h + 48.0) - 24.0;
    int sy = y + (int)scan;
    for (int trail = 0; trail < 6; trail++) {
        int ly = sy - trail * 4;
        if (ly < y || ly >= y + h) continue;
        double a = 0.38 * (1.0 - trail * 0.16);
        if (a < 0.04) break;
        fill_rect(app, buf, lerp_rgb(th->bg, th->accent, a),
                  x, ly, w, trail == 0 ? 2 : 1);
    }

    /* Edge vignette — frames the content */
    unsigned long edge = lerp_rgb(th->bg, th->border, 0.55);
    fill_rect(app, buf, edge, x, y, w, 1);
    fill_rect(app, buf, edge, x, y + h - 1, w, 1);
    fill_rect(app, buf, edge, x, y, 1, h);
    fill_rect(app, buf, edge, x + w - 1, y, 1, h);
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

/* ── Sleep preview overlay ("/testsleep <time>" + Enter) ──── */

static void sleep_preview_button_rect(int *bx, int *by, int *bw, int *bh) {
    int W = g_config.cfg_w, H = g_config.cfg_h;
    int pad = 24;
    *bx = pad; *by = H - 64; *bw = W - 2*pad; *bh = 40;
}

int sleep_preview_handle_click(App *app, int mx, int my) {
    if (!app->sleep_preview_open || !app->sleep_preview) return 0;
    int bx, by, bw, bh;
    sleep_preview_button_rect(&bx, &by, &bw, &bh);
    if (mx < bx || mx >= bx + bw || my < by || my >= by + bh) return 0;

    if (app->sleep_audio_active) {
        app->sleep_audio_active = 0;
        kill_all_audio(app);
    } else {
        play_sleep_audition(app, app->sleep_preview);
    }
    draw_sleep_preview(app);
    return 1;
}

void draw_sleep_preview(App *app) {
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

    int pad = 24;
    if (fh) xft_draw_string(app, draw, fh, th->accent, pad, pad + 14, "SLEEP PREVIEW");
    if (fs) {
        const char *hint = "Esc to close";
        xft_draw_string(app, draw, fs, th->text_muted,
                        W - pad - text_width(app, fs, hint), pad + 12, hint);
    }

    int y = pad + 42;
    SleepPreviewData *d = app->sleep_preview;
    if (!d) return;

    if (fs) {
        char line[300];
        char dur[32]; format_time((int)d->total_seconds, dur, sizeof(dur));
        snprintf(line, sizeof(line), "\"%s\"  —  %s total", app->sleep_preview_input, dur);
        xft_draw_string(app, draw, fs, th->text_muted, pad, y, line);
        y += 22;
    }

    struct { const char *title; const char *note; double span; double volume_pct;
             const char *fallback; } phases[3] = {
        { "1  PREPARING",  "plays fully if it fits",       d->intro_len,
          g_config.sleep_volume_preparing_percent,
          "generated: rain — filtered noise + droplets, fades in" },
        { "2  SLEEPING",   "lowest priority — loops to fill / fades if cut",
          d->sleep_end - d->intro_len > 0 ? d->sleep_end - d->intro_len : 0.0,
          g_config.sleep_volume_sleeping_percent,
          "generated: slow detuned-oscillator drone + LFO (looped)" },
        { "3  WAKING",     "highest priority — always plays in full",
          d->wake_len,
          g_config.sleep_volume_waking_percent,
          "generated: gentle rising chime over a soft pad (looped)" },
    };

    for (int p = 0; p < 3 && y < H - 90; p++) {
        if (fm) { xft_draw_string(app, draw, fm, th->accent, pad, y, phases[p].title); y += 16; }
        if (fs) {
            xft_draw_string(app, draw, fs, th->text_muted, pad + 4, y, phases[p].note);
            y += 16;
            char span[64]; char sbuf[24]; format_time((int)phases[p].span, sbuf, sizeof(sbuf));
            snprintf(span, sizeof(span), "~%s   system volume: %.0f%%", sbuf, phases[p].volume_pct);
            xft_draw_string(app, draw, fs, th->text, pad + 4, y, span);
            y += 18;
            xft_draw_string(app, draw, fs, th->text_muted, pad + 8, y, phases[p].fallback);
            y += 16;
        }
        y += 10;
    }

    int bx, by, bw, bh;
    sleep_preview_button_rect(&bx, &by, &bw, &bh);
    int playing = app->sleep_audio_active;
    fill_rect(app, buf, playing ? th->danger : th->accent_soft, bx, by, bw, bh);
    draw_rect_outline(app, buf, playing ? th->danger : th->accent, bx, by, bw, bh);
    if (fm) {
        const char *label = playing ? "■  Stop preview" : "▶  Play preview  (4s per phase)";
        int tw = text_width(app, fm, label);
        xft_draw_string(app, draw, fm, playing ? 0xf0e8d8UL : th->accent,
                        bx + (bw - tw)/2, by + (bh + fm->ascent - fm->descent)/2, label);
    }

    if (app->cfg_buf)
        XCopyArea(app->dpy, app->cfg_buf, app->cfg_win, app->gc, 0, 0, W, H, 0, 0);
    XFlush(app->dpy);
}

/* ── Help overlay (Ctrl+H, start screen) ──────────────────── */

typedef struct { const char *key; const char *desc; } HelpLine;

void draw_help(App *app) {
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

    int pad = 24;
    if (fh) xft_draw_string(app, draw, fh, th->accent, pad, pad + 14, "HELP");
    if (fs) {
        const char *hint = "Ctrl+H  or  Esc  to close";
        xft_draw_string(app, draw, fs, th->text_muted,
                        W - pad - text_width(app, fs, hint), pad + 12, hint);
    }

    int y = pad + 44;

    if (fm) { xft_draw_string(app, draw, fm, th->accent, pad, y, "TYPING A TIMER"); y += 20; }
    static const HelpLine syntax[] = {
        {"25m task",     "duration + optional task name (s/m/h)"},
        {"u15:30",       "run until the clock reads 15:30"},
        {"u:30",         "run until minute 30 of this/next hour"},
        {"d 25m",        "shut down the machine when it ends"},
        {"a 25m",        "fullscreen alert when it ends"},
        {"f 25m",        "start fullscreen"},
        {"p 25m",        "allow pausing with Space"},
        {"l 25m",        "lock keyboard/mouse, uncancelable"},
    };
    for (size_t i = 0; i < sizeof(syntax)/sizeof(syntax[0]) && fs; i++) {
        xft_draw_string(app, draw, fs, th->text, pad + 4, y, syntax[i].key);
        xft_draw_string(app, draw, fs, th->text_muted, pad + 120, y, syntax[i].desc);
        y += 18;
    }

    y += 12;
    if (fm) { xft_draw_string(app, draw, fm, th->accent, pad, y, "KEYS"); y += 20; }
    static const HelpLine keys[] = {
        {"Enter",        "start the timer"},
        {"Tab / Ctrl+T", "cycle theme"},
        {"Ctrl+I",       "open the data log"},
        {"Ctrl+A",       "cursor to end of input"},
        {"Ctrl+H",       "toggle this help"},
        {"Ctrl+G",       "settings (also works while a timer runs)"},
        {"Space",        "pause / resume (if pausable)"},
        {"Esc",          "dismiss alert, or quit"},
    };
    for (size_t i = 0; i < sizeof(keys)/sizeof(keys[0]) && fs; i++) {
        xft_draw_string(app, draw, fs, th->text, pad + 4, y, keys[i].key);
        xft_draw_string(app, draw, fs, th->text_muted, pad + 120, y, keys[i].desc);
        y += 18;
    }

    if (app->cfg_buf)
        XCopyArea(app->dpy, app->cfg_buf, app->cfg_win, app->gc, 0, 0, W, H, 0, 0);
    XFlush(app->dpy);
}

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

static void settings_field_rect(int row, int *x, int *y, int *w, int *h) {
    *x = SET_PAD; *w = g_config.cfg_w - 2*SET_PAD;
    *y = SET_PAD + 50 + row * SET_FIELD_ROW_H;
    *h = SET_FIELD_ROW_H - 10;
}

static void settings_slider_track_rect(int fx, int fy, int fw, int fh,
                                        int *tx, int *ty, int *tw, int *th_) {
    *tx = fx; *tw = fw; *ty = fy + fh - 14; *th_ = 8;
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
    if (fh) xft_draw_string(app, draw, fh, th->accent, pad, pad + 14, "SETTINGS");
    if (fs) {
        const char *hint = app->settings_cat < 0 ? "Ctrl+G / Esc to close" : "Esc: back";
        xft_draw_string(app, draw, fs, th->text_muted,
                        W - pad - text_width(app, fs, hint), pad + 12, hint);
    }

    if (app->settings_cat < 0) {
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
        } else {
            char valbuf[32];
            if (cf->type == CFG_F_DOUBLE) snprintf(valbuf, sizeof(valbuf), "%.2f", v);
            else                          snprintf(valbuf, sizeof(valbuf), "%d", (int)v);
            if (fs) xft_draw_string(app, draw, fs, th->text_muted,
                                    fx + fw - text_width(app, fs, valbuf), fy + 12, valbuf);

            int tx, ty, tw, th2; settings_slider_track_rect(fx, fy, fw, fhh, &tx, &ty, &tw, &th2);
            fill_rect(app, buf, th->surface_soft, tx, ty, tw, th2);
            draw_rect_outline(app, buf, th->border, tx, ty, tw, th2);
            double frac = (cf->max > cf->min) ? (v - cf->min) / (cf->max - cf->min) : 0.0;
            if (frac < 0) frac = 0;
            if (frac > 1) frac = 1;
            int fillw = (int)(tw * frac);
            if (fillw > 0) fill_rect(app, buf, th->accent, tx, ty, fillw, th2);
        }
    }

    if (app->cfg_buf)
        XCopyArea(app->dpy, app->cfg_buf, app->cfg_win, app->gc, 0, 0, W, H, 0, 0);
    XFlush(app->dpy);
}

int settings_handle_click(App *app, int mx, int my, int is_press) {
    if (!is_press) return 0;

    if (app->settings_cat < 0) {
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
        } else {
            int tx, ty, tw, th2; settings_slider_track_rect(fx, fy, fw, fhh, &tx, &ty, &tw, &th2);
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
        int tx, ty, tw, th2; settings_slider_track_rect(fx, fy, fw, fhh, &tx, &ty, &tw, &th2);
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
    if (ks == XK_Escape) {
        if (app->settings_cat >= 0) { app->settings_cat = -1; draw_settings(app); }
        else settings_close_overlay(app);
        return;
    }
    if ((ev->state & ControlMask) && (ks == XK_g || ks == XK_G)) {
        settings_close_overlay(app);
        return;
    }
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
    app->settings_open = 0;
    app->settings_drag_field = -1;
    app->settings_cat = -1;
    if (app->settings_from_timer && app->cfg_win) {
        XWithdrawWindow(app->dpy, app->cfg_win, app->screen);
        if (app->tmr_win) XSetInputFocus(app->dpy, app->tmr_win, RevertToPointerRoot, CurrentTime);
    } else {
        draw_config(app);
    }
    app->settings_from_timer = 0;
}

void draw_config(App *app) {
    if (app->settings_open) {
        draw_settings(app);
        return;
    }
    if (!app->cfg_win) return;
    if (app->sleep_preview_open) {
        draw_sleep_preview(app);
        return;
    }
    if (app->help_open) {
        draw_help(app);
        return;
    }
    if (app->insights_open) {
        draw_insights(app);
        return;
    }
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

    XStoreName(app->dpy, app->cfg_win, "Timer");
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

/* Whichever volume is actually "live" right now during the alert: the
 * sleep-mode waking phase's volume if this timer was sleep-mode, else
 * the alarm's ramped-to ceiling. */
static double *alert_active_volume_field(App *app) {
    return app->sleep_mode ? &g_config.sleep_volume_waking_percent
                            : &g_config.alarm_volume_max_percent;
}

static void alert_vol_rect(AlertScreen *s, int *x, int *y, int *w, int *h) {
    *w = 320; if (*w > s->w - 80) *w = s->w - 80;
    *h = 10;
    *x = (s->w - *w) / 2;
    *y = s->h*2/3 + 52 + 40;
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
    /* Volume slider — bound to whichever volume is currently active
     * (alarm ceiling, or sleep-mode waking volume), reusing the same
     * slider look as the Settings overlay. Dragging it calls
     * set_system_volume() live for immediate audible feedback. */
    {
        int sx, sy, sw, sh; alert_vol_rect(s, &sx, &sy, &sw, &sh);
        double pct = *alert_active_volume_field(app);
        char lbl[32]; snprintf(lbl, sizeof(lbl), "Volume  %d%%", (int)pct);
        XftFont *f = get_font(app, 11, 0);
        if (f) xft_draw_string_centered(app, draw, f, text_col, W/2, sy - 12, lbl);
        fill_rect(app, buf, 0x000000UL, sx, sy, sw, sh);
        draw_rect_outline(app, buf, text_col, sx, sy, sw, sh);
        int fillw = (int)(sw * (pct / 100.0));
        if (fillw > 0) fill_rect(app, buf, text_col, sx, sy, fillw, sh);
    }
    XCopyArea(app->dpy, buf, s->win, app->gc, 0, 0, W, H, 0, 0);
    (void)elapsed;
}

int alert_vol_handle_click(App *app, int screen_idx, int mx, int my) {
    if (screen_idx < 0 || screen_idx >= app->n_alert_screens) return 0;
    AlertScreen *s = &app->alert_screens[screen_idx];
    int sx, sy, sw, sh; alert_vol_rect(s, &sx, &sy, &sw, &sh);
    if (mx < sx-8 || mx >= sx+sw+8 || my < sy-16 || my >= sy+sh+16) return 0;

    double frac = sw > 0 ? (double)(mx - sx) / (double)sw : 0.0;
    if (frac < 0) frac = 0;
            if (frac > 1) frac = 1;
    double pct = frac * 100.0;
    *alert_active_volume_field(app) = pct;
    config_save();
    set_system_volume((int)pct);
    app->alert_vol_dragging = 1;
    draw_alert(app);
    return 1;
}

int alert_vol_handle_motion(App *app, int screen_idx, int mx, int my) {
    if (!app->alert_vol_dragging) return 0;
    if (screen_idx < 0 || screen_idx >= app->n_alert_screens) return 0;
    AlertScreen *s = &app->alert_screens[screen_idx];
    int sx, sy, sw, sh; alert_vol_rect(s, &sx, &sy, &sw, &sh);
    (void)sy; (void)sh;
    double frac = sw > 0 ? (double)(mx - sx) / (double)sw : 0.0;
    if (frac < 0) frac = 0;
            if (frac > 1) frac = 1;
    double pct = frac * 100.0;
    *alert_active_volume_field(app) = pct;
    config_save();
    set_system_volume((int)pct);
    draw_alert(app);
    return 1;
}

void alert_vol_handle_release(App *app) {
    app->alert_vol_dragging = 0;
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
