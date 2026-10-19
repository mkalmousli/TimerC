/* ui.h — public interface to the ui/ screen files, called from src/core/app.c
 *
 * These prototypes previously lived in the "draw.c" section of the old
 * monolithic timer.h; they're unchanged, just given their own header
 * now that the implementation is split across draw_common.c/
 * draw_timer.c/draw_alert.c.
 */
#ifndef UI_H
#define UI_H

#include <X11/Xlib.h>

typedef struct App App; /* full definition: timer_state.h */

void generate_themes(void);
void cfg_refresh_preview(App *app);
void cfg_update_layout(App *app);
void cfg_draw_cyber_bg(App *app, Drawable buf, const Theme *th,
                       int x, int y, int w, int h, double t);
void draw_release_quote_cache(App *app);
void draw_config(App *app);
void draw_config_anim(App *app);
void draw_config_panel(App *app);
void cfg_redraw_after_input(App *app);



void draw_timer(App *app);
void draw_alert(App *app);
void create_config_window(App *app);
void create_timer_window(App *app);
void destroy_timer_window(App *app);
void create_alert_windows(App *app);
void destroy_alert_windows(App *app);

#ifdef USE_GL
int    gl_init(App *app);
void   gl_shutdown(App *app);
Window gl_create_timer_window(App *app, int x, int y, int w, int h, int fullscreen);
void   gl_make_current(App *app, Window win);
void   gl_draw_timer(App *app);
#endif

#endif
