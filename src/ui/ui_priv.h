/* ui_priv.h — internal helpers shared between src/ui
 *
 * These were `static` functions in the old monolithic draw.c; splitting
 * that file by feature screen (draw_timer.c, draw_settings.c,
 * draw_insights.c, draw_alert.c, draw_help.c) means the small set of
 * primitives/theme/buffer helpers actually used across more than one of
 * those files need an internal (non-installed, not in include/) shared
 * prototype header instead. Nothing here is part of the public API used
 * by src/core — only draw_common.c defines these, the other ui-screen files
 * just call them.
 */
#ifndef UI_PRIV_H
#define UI_PRIV_H

#include "timer.h"

/* ── Color / pixel helpers ─────────────────────────────── */
unsigned long rgb_pixel(App *app, unsigned long rgb);
void          rgb_xft(App *app, unsigned long rgb, XftColor *out);
unsigned long lerp_rgb(unsigned long a, unsigned long b, double t);

/* ── Drawing primitives (formalized further in include/platform for a
 * future backend — see platform.h) ───────────────────────*/
void fill_rect(App *app, Drawable d, unsigned long rgb, int x, int y, int w, int h);
void draw_rect_outline(App *app, Drawable d, unsigned long rgb, int x, int y, int w, int h);

/* ── Font management ───────────────────────────────────── */
XftFont *get_font(App *app, int size, int style);
int      text_width(App *app, XftFont *font, const char *s);
int      fit_font_size(App *app, const char *text, int max_w, int max_h,
                        int min_sz, int max_sz, int bold);
int      fit_font_size_h(App *app, int max_h, int min_sz, int max_sz, int style);
void     xft_draw_string(App *app, XftDraw *draw, XftFont *font,
                          unsigned long rgb, int x, int y, const char *s);
void     xft_draw_string_centered(App *app, XftDraw *draw, XftFont *font,
                                   unsigned long rgb, int cx, int y, const char *s);
void     draw_text_stretched(App *app, Pixmap buf,
                              int area_x, int area_y, int area_w, int area_h,
                              XftFont *f, unsigned long col, unsigned long bg,
                              const char *text);

/* ── Easing / misc math ────────────────────────────────── */
double ease_in_out_cubic(double t);
double pos_mod(double v, double m);
int    scaled(int base, int lo, int hi);

/* ── Theme helpers ─────────────────────────────────────── */
const Theme *cur_theme(App *app);
const Theme *cfg_theme(App *app);
unsigned long shutdown_pulse_color(const Theme *th);
unsigned long timer_display_color(App *app);
unsigned long alert_strobe_color(double elapsed, int frame);

/* ── Quote-cycling morph (config/setup screen) ─────────── */
void draw_quote_morph(App *app, XftDraw *draw, XftFont *font,
                       Drawable buf, const Theme *th,
                       int cx, int baseline, int max_w);

/* ── Double-buffer / window-property helpers ───────────── */
XftDraw *make_xft_draw(App *app, Drawable d);
void     ensure_cfg_buf(App *app);
void     set_window_on_all_workspaces(App *app, Window win);
void     disable_compositor_bypass(App *app, Window win);

/* ── Power / shutdown icon ─────────────────────────────── */
int draw_power_icon(App *app, Drawable d, int cx, int cy, int r, unsigned long rgb);

/* ── Shared borders helper (config + insights) ─────────── */
void cfg_draw_borders_wh(App *app, Drawable buf, const Theme *th, int W, int H);

#endif
