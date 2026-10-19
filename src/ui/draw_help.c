/* draw_help.c — keybinding help overlay (Ctrl+H).
 * Relocated from the old monolithic draw.c; see src/ui/ui_priv.h for
 * the shared primitives/theme/buffer helpers this pulls in from
 * draw_common.c. */
#include "timer.h"
#include "ui_priv.h"

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

