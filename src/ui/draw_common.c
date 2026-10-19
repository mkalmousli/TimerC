/* draw_common.c — shared UI primitives: color/pixel helpers, font cache,
 * procedural theme generation, quote-cycling glyph-mask cache, config/
 * timer double-buffer management, and small drawing primitives
 * (fill_rect/draw_rect_outline/text) used by every src/ui screen file.
 *
 * Relocated verbatim from the top portion of the old monolithic draw.c —
 * see src/ui/ui_priv.h for the internal prototypes these expose to the
 * other ui-screen files (draw_timer.c, draw_settings.c, draw_insights.c,
 * draw_alert.c, draw_help.c). */
#include "timer.h"
#include "ui_priv.h"

/* Default to a lightweight render path (less CPU/GPU, fewer animations). */
#ifndef LOW_POWER
#define LOW_POWER 1
#endif

/* ── Theme data (filled by generate_themes(), see below) ── */
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


unsigned long rgb_pixel(App *app, unsigned long rgb) {
    XColor c;
    c.red   = ((rgb >> 16) & 0xff) * 257;
    c.green = ((rgb >> 8)  & 0xff) * 257;
    c.blue  = ( rgb        & 0xff) * 257;
    c.flags = DoRed | DoGreen | DoBlue;
    XAllocColor(app->dpy, app->cmap, &c);
    return c.pixel;
}


void set_window_on_all_workspaces(App *app, Window win) {
    unsigned long desktop = NET_WM_DESKTOP_ALL;
    XChangeProperty(app->dpy, win, app->net_wm_desktop,
                    XA_CARDINAL, 32, PropModeReplace,
                    (unsigned char *)&desktop, 1);
}


void disable_compositor_bypass(App *app, Window win) {
    unsigned long value = 2; /* EWMH: ask compositor not to unredirect this window. */
    XChangeProperty(app->dpy, win, app->net_wm_bypass_compositor,
                    XA_CARDINAL, 32, PropModeReplace,
                    (unsigned char *)&value, 1);
}


void rgb_xft(App *app, unsigned long rgb, XftColor *out) {
    XRenderColor rc;
    rc.red   = ((rgb >> 16) & 0xff) * 257;
    rc.green = ((rgb >> 8)  & 0xff) * 257;
    rc.blue  = ( rgb        & 0xff) * 257;
    rc.alpha = 0xffff;
    XftColorAllocValue(app->dpy, app->vis, app->cmap, &rc, out);
}


unsigned long lerp_rgb(unsigned long a, unsigned long b, double t) {
    if (t <= 0.0) return a;
    if (t >= 1.0) return b;
    int ar = (a>>16)&0xff, ag = (a>>8)&0xff, ab2 = a&0xff;
    int br = (b>>16)&0xff, bg = (b>>8)&0xff, bb  = b&0xff;
    int r  = (int)(ar + (br-ar)*t);
    int g  = (int)(ag + (bg-ag)*t);
    int b2 = (int)(ab2 + (bb-ab2)*t);
    return ((unsigned long)r<<16)|((unsigned long)g<<8)|(unsigned long)b2;
}

/* ── Themes ───────────────────────────────────────────────
 * A fixed set of hand-picked dark palettes. Each is just a background, an
 * accent, a text color and a danger color; surfaces, borders and muted text
 * are mixed from those so every theme stays coherent. Tab / Shift+Tab (or a
 * click on the theme dots) switches between them on the input page. */

typedef struct { const char *name; unsigned long bg, accent, text, danger; } ThemeDef;

static const ThemeDef THEME_DEFS[N_THEMES] = {
    { "Midnight",  0x0B0E17, 0x6EA8FE, 0xE8EEFA, 0xFF5C5C },
    { "Dracula",   0x15161E, 0xBD93F9, 0xF8F8F2, 0xFF5555 },
    { "Nord",      0x161A22, 0x88C0D0, 0xECEFF4, 0xBF616A },
    { "Gruvbox",   0x14120F, 0xFABD2F, 0xEBDBB2, 0xFB4934 },
    { "Rose",      0x14111A, 0xEBBCBA, 0xE0DEF4, 0xEB6F92 },
    { "Forest",    0x0A120E, 0x7BD88F, 0xE3F2E6, 0xFF6B6B },
    { "Ember",     0x140C0A, 0xFF8A4C, 0xFBEDE4, 0xFF4D4D },
    { "Crimson",   0x130A0D, 0xFF5C7A, 0xFBE9ED, 0xFFA94D },
    { "Cyber",     0x0A0A12, 0x00F0FF, 0xE6FBFF, 0xFF2E88 },
    { "Amethyst",  0x120B1A, 0xC77DFF, 0xF1E7FB, 0xFF6B8B },
    { "Solar",     0x00141A, 0x2AA198, 0xEEE8D5, 0xDC322F },
    { "Mono",      0x000000, 0xE6E6E6, 0xFFFFFF, 0xFF5C5C },
};

static char g_theme_names[N_THEMES][24];

static unsigned long mix_rgb(unsigned long a, unsigned long b, double t) {
    unsigned long out = 0;
    for (int sh = 16; sh >= 0; sh -= 8) {
        double ca = (double)((a >> sh) & 0xFF), cb = (double)((b >> sh) & 0xFF);
        out |= (unsigned long)(ca + (cb - ca) * t + 0.5) << sh;
    }
    return out;
}

void generate_themes(void) {
    for (int i = 0; i < N_THEMES; i++) {
        const ThemeDef *d = &THEME_DEFS[i];
        Theme *t = &THEMES[i];
        snprintf(g_theme_names[i], sizeof(g_theme_names[i]), "%s", d->name);
        t->name         = g_theme_names[i];
        t->bg           = d->bg;
        t->surface      = mix_rgb(d->bg, d->text, 0.05);
        t->surface_soft = mix_rgb(mix_rgb(d->bg, d->accent, 0.05), d->text, 0.06);
        t->border       = mix_rgb(d->bg, d->text, 0.22);
        t->text         = d->text;
        t->text_muted   = mix_rgb(d->bg, d->text, 0.55);
        t->accent       = d->accent;
        t->accent_soft  = mix_rgb(d->bg, d->accent, 0.24);
        t->danger       = d->danger;
        t->until_chip_bg = t->surface; t->until_chip_fg = t->text; t->until_chip_border = t->border;
        t->until_clock_fg = t->accent; t->until_clock_bg = t->bg;
        t->lcd_active = t->accent; t->lcd_paused = t->text_muted;
    }
}

/* Cycle the active theme (dir = +1 / -1). */
void theme_step(App *app, int dir) {
    app->theme_idx = (app->theme_idx + dir + N_THEMES) % N_THEMES;
}


void fill_rect(App *app, Drawable d, unsigned long rgb,
                      int x, int y, int w, int h) {
    XSetForeground(app->dpy, app->gc, rgb_pixel(app, rgb));
    XFillRectangle(app->dpy, d, app->gc, x, y, w, h);
}


void draw_rect_outline(App *app, Drawable d, unsigned long rgb,
                               int x, int y, int w, int h) {
    XSetForeground(app->dpy, app->gc, rgb_pixel(app, rgb));
    XDrawRectangle(app->dpy, d, app->gc, x, y, w-1, h-1);
}

/* ── Font management ───────────────────────────────────── */


XftFont *get_font(App *app, int size, int style) {
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


int text_width(App *app, XftFont *font, const char *s) {
    if (!font || !s || !s[0]) return 0;
    XGlyphInfo ext;
    XftTextExtentsUtf8(app->dpy, font, (const FcChar8 *)s, (int)strlen(s), &ext);
    return ext.xOff;
}


int fit_font_size(App *app, const char *text,
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

int fit_font_size_h(App *app, int max_h,
                            int min_sz, int max_sz, int style) {
    int best = min_sz;
    for (int sz = max_sz; sz >= min_sz; sz -= (sz > 60 ? 4 : 2)) {
        XftFont *f = get_font(app, sz, style);
        if (!f) continue;
        if (f->ascent + f->descent <= max_h) { best = sz; break; }
    }
    return best;
}


void xft_draw_string(App *app, XftDraw *draw, XftFont *font,
                            unsigned long rgb, int x, int y, const char *s) {
    if (!draw || !font || !s || !s[0]) return;
    XftColor col;
    rgb_xft(app, rgb, &col);
    XftDrawStringUtf8(draw, &col, font, x, y, (const FcChar8 *)s, (int)strlen(s));
    XftColorFree(app->dpy, app->vis, app->cmap, &col);
}


void xft_draw_string_centered(App *app, XftDraw *draw, XftFont *font,
                                     unsigned long rgb, int cx, int y, const char *s) {
    xft_draw_string(app, draw, font, rgb, cx - text_width(app, font, s)/2, y, s);
}


double ease_in_out_cubic(double t) {
    if (t <= 0.0) return 0.0;
    if (t >= 1.0) return 1.0;
    return (t < 0.5) ? 4.0 * t * t * t : 1.0 - pow(-2.0 * t + 2.0, 3.0) / 2.0;
}


double pos_mod(double v, double m) {
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

void draw_quote_morph(App *app, XftDraw *draw, XftFont *font,
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

void draw_text_stretched(App *app, Pixmap buf,
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


const Theme *cur_theme(App *app) { return &THEMES[app->theme_idx]; }

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

const Theme *cfg_theme(App *app) {
    return app->cfg_focused ? cur_theme(app) : &UNFOCUSED_THEME;
}

/* Smooth sinusoidal pulse for the shutdown icon: always red, breathing
 * between 25 % and 100 % brightness at ~3.5 Hz to signal urgency. */

unsigned long shutdown_pulse_color(const Theme *th) {
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


unsigned long timer_display_color(App *app) {
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

unsigned long alert_strobe_color(double elapsed, int frame) {
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


XftDraw *make_xft_draw(App *app, Drawable d) {
    return XftDrawCreate(app->dpy, d, app->vis, app->cmap);
}


void ensure_cfg_buf(App *app) {
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

/* ── Clamped scale helper ──────────────────────────────── */

int scaled(int base, int lo, int hi) {
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

int draw_power_icon(App *app, Drawable d,
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


void cfg_draw_borders_wh(App *app, Drawable buf, const Theme *th, int W, int H) {
    fill_rect(app, buf, th->accent, 0, 0, W, 2);
    fill_rect(app, buf, th->accent, 0, H - 2, W, 2);
    fill_rect(app, buf, th->accent, 0, 0, 2, H);
    fill_rect(app, buf, th->accent, W - 2, 0, 2, H);
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

