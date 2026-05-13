/* gl.c — Optional OpenGL (GLX) renderer for the timer window */
#include "timer.h"

#ifdef USE_GL

#include <GL/gl.h>
#include <GL/glx.h>

static void gl_set_ortho(int w, int h) {
    glViewport(0, 0, w, h);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrtho(0, w, h, 0, -1, 1);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
}

static void gl_rgb(unsigned long rgb, float *r, float *g, float *b) {
    *r = ((rgb >> 16) & 0xff) / 255.0f;
    *g = ((rgb >>  8) & 0xff) / 255.0f;
    *b = ( rgb        & 0xff) / 255.0f;
}

static void gl_fill_rect(int x, int y, int w, int h, unsigned long rgb) {
    float r, g, b;
    gl_rgb(rgb, &r, &g, &b);
    glColor3f(r, g, b);
    glBegin(GL_QUADS);
    glVertex2i(x,     y);
    glVertex2i(x + w, y);
    glVertex2i(x + w, y + h);
    glVertex2i(x,     y + h);
    glEnd();
}

static GLuint build_font_lists(Display *dpy, Font font) {
    GLuint base = glGenLists(256);
    /* Convert X11 server-side font into GL display lists. */
    glXUseXFont(font, 0, 256, base);
    return base;
}

static void gl_draw_text(GLuint font_base, int x, int y, unsigned long rgb, const char *s) {
    if (!s || !s[0]) return;
    float r, g, b;
    gl_rgb(rgb, &r, &g, &b);
    glColor3f(r, g, b);
    glRasterPos2i(x, y);
    glListBase(font_base);
    glCallLists((GLsizei)strlen(s), GL_UNSIGNED_BYTE, (const GLubyte *)s);
}

static int gl_text_width_8x13(const char *s) {
    if (!s) return 0;
    return (int)strlen(s) * 8;
}

static const Theme *gl_cur_theme(App *app) { return &THEMES[app->theme_idx]; }

int gl_init(App *app) {
    if (!app || !app->dpy) return 0;
    int attribs[] = { GLX_RGBA, GLX_DOUBLEBUFFER, GLX_DEPTH_SIZE, 16, None };
    app->glx_vi = glXChooseVisual(app->dpy, app->screen, attribs);
    if (!app->glx_vi) return 0;
    app->glx_ctx = glXCreateContext(app->dpy, app->glx_vi, NULL, True);
    if (!app->glx_ctx) return 0;
    app->glx_cmap = XCreateColormap(app->dpy, DefaultRootWindow(app->dpy),
                                    app->glx_vi->visual, AllocNone);
    return 1;
}

void gl_shutdown(App *app) {
    if (!app || !app->dpy) return;
    if (app->glx_ctx) { glXDestroyContext(app->dpy, app->glx_ctx); app->glx_ctx = NULL; }
    if (app->glx_vi)  { XFree(app->glx_vi); app->glx_vi = NULL; }
    if (app->glx_cmap) { XFreeColormap(app->dpy, app->glx_cmap); app->glx_cmap = 0; }
}

Window gl_create_timer_window(App *app, int x, int y, int w, int h, int fullscreen) {
    if (!app || !app->glx_vi) return 0;
    Window root = DefaultRootWindow(app->dpy);

    XSetWindowAttributes swa;
    swa.colormap = app->glx_cmap;
    swa.border_pixel = 0;
    swa.background_pixel = 0;
    swa.override_redirect = True;
    swa.event_mask = ExposureMask | ButtonPressMask | ButtonReleaseMask |
                     PointerMotionMask | KeyPressMask | StructureNotifyMask;

    Window win = XCreateWindow(app->dpy, root, x, y, (unsigned)w, (unsigned)h, 0,
                               app->glx_vi->depth, InputOutput, app->glx_vi->visual,
                               CWColormap | CWBorderPixel | CWBackPixel | CWOverrideRedirect | CWEventMask,
                               &swa);

    Atom states[3] = {
        app->net_wm_state_above,
        app->net_wm_state_sticky,
        app->net_wm_state_skip_taskbar
    };
    int nstates = fullscreen ? 3 : 2;
    XChangeProperty(app->dpy, win, app->net_wm_state,
                    XA_ATOM, 32, PropModeReplace, (unsigned char *)states, nstates);
    XStoreName(app->dpy, win, "Timer");
    XMapRaised(app->dpy, win);
    XFlush(app->dpy);
    return win;
}

void gl_make_current(App *app, Window win) {
    glXMakeCurrent(app->dpy, win, app->glx_ctx);
}

void gl_draw_timer(App *app) {
    if (!app || !app->tmr_win) return;
    const Theme *th = gl_cur_theme(app);
    int W = app->tmr_w, H = app->tmr_h;

    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
    gl_set_ortho(W, H);

    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);

    gl_fill_rect(0, 0, W, H, th->bg);

    /* Simple progress bar */
    if (app->total_seconds > 0) {
        double frac = 1.0 - app->remaining_exact / app->total_seconds;
        if (frac < 0.0) frac = 0.0;
        if (frac > 1.0) frac = 1.0;
        int prog_h = app->mini ? (H / 12) : (H / 10);
        if (prog_h < 4) prog_h = 4;
        gl_fill_rect(0, H - prog_h, W, prog_h, th->surface_soft);
        gl_fill_rect(0, H - prog_h, (int)(W * frac), prog_h, th->accent);
    }

    /* Text via X font -> GL display lists (cheap and GPU-rasterized). */
    static GLuint font_base = 0;
    static Font xfont = 0;
    if (!font_base) {
        xfont = XLoadFont(app->dpy, "fixed");
        if (xfont) font_base = build_font_lists(app->dpy, xfont);
    }

    char time_str[16];
    format_time(app->remaining_seconds, time_str, sizeof(time_str));

    int tw = gl_text_width_8x13(time_str);
    int tx = (W - tw) / 2;
    int ty = H / 2;
    gl_draw_text(font_base, tx, ty, th->text, time_str);

    glXSwapBuffers(app->dpy, app->tmr_win);
}

#endif /* USE_GL */

