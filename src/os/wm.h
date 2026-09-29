/* Window Manager / Event Manager / Menu Manager shared internals. */
#ifndef WM_H
#define WM_H
#include "qd.h"

/* WindowRecord (156 bytes) */
#define WIN_SIZE        156
#define WIN_KIND        108
#define WIN_VISIBLE     110
#define WIN_HILITED     111
#define WIN_GOAWAY      112
#define WIN_SPARE       113
#define WIN_STRUC       114
#define WIN_CONT        118
#define WIN_UPDATE      122
#define WIN_DEFPROC     126
#define WIN_DATA        130
#define WIN_TITLE       134
#define WIN_TITLEWIDTH  138
#define WIN_CONTROLS    140
#define WIN_NEXT        144
#define WIN_PIC         148
#define WIN_REFCON      152

#define LM_WindowList   0x09D6
#define LM_GrayRgn      0x09EE
#define LM_MBarHeight   0x0BAA

enum { dialogKind = 2, userKind = 8 };

u32 wm_port(void);
u32 wm_front(void);                 /* front visible window or 0 */
u32 wm_first(void);
void wm_invalidate_global(const HRgn *g);  /* add to update regions */
void wm_recalc(const HRgn *damage);        /* recompute vis, repaint damage */
void wm_local_to_global(u32 win, int *x, int *y);
void wm_global_rgn_to_local(u32 win, HRgn *r);
void wm_draw_frame(u32 win);
int wm_find(Point pt, u32 *winout);
bool wm_is_window(u32 w);
void wm_activate_changed(void);
u32 wm_create(u32 storage, Rect bounds, const char *title, bool visible, s16 procid,
              u32 behind, bool goaway, u32 refcon, bool color);
void wm_show(u32 win, bool show);
void wm_select(u32 win);

/* events */
void ev_init(void);
void ev_post(u16 what, u32 message, u16 mods);
bool ev_mouse_button(void);
Point ev_mouse_global(void);
void ev_pump_host(void);
void ev_idle_frame(void);           /* present screen, pump host */
u16 ev_modifiers(void);
void ev_post_update_check(void);

/* menus */
void menu_init(void);
void menu_draw_bar(void);
u32 menu_select(Point start);

/* dialogs & controls (used by events/windows) */
void ctl_draw_all(u32 win);
#endif
