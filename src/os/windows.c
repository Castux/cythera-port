/* Window Manager.
 *
 * WindowRecords live in guest memory. Window frames are drawn by the
 * window definition procedure: the application's own WDEFs (reached through
 * the patched "JMP routine-descriptor" stubs in its WDEF resources) or a
 * native implementation of the standard System 7 WDEF. visRgns, update
 * regions and exposure painting are computed from the window list.
 */
#include "wm.h"
#include "resources.h"
#include "../host/host.h"

enum { wDraw = 0, wHit, wCalcRgns, wNew, wDispose, wGrow, wDrawGIcon };
enum { wNoHit = 0, wInContent, wInDrag, wInGrow, wInGoAway, wInZoomIn, wInZoomOut };
enum { inDesk = 0, inMenuBar, inSysWindow, inContent, inDrag, inGrow, inGoAway, inZoomIn, inZoomOut };

typedef struct {
    u32 win;
    int var;
    s16 procid;
    bool native;      /* native standard WDEF */
    bool zoomed;
} WInfo;

#define MAX_WIN 128
static WInfo g_wi[MAX_WIN];
static u32 g_wmport;
static u32 g_last_front;

static WInfo *winfo(u32 w) {
    for (int i = 0; i < MAX_WIN; i++) if (g_wi[i].win == w) return &g_wi[i];
    return NULL;
}
static WInfo *winfo_new(u32 w) {
    for (int i = 0; i < MAX_WIN; i++) if (!g_wi[i].win) { memset(&g_wi[i], 0, sizeof g_wi[i]); g_wi[i].win = w; return &g_wi[i]; }
    fatal("too many windows");
}
bool wm_is_window(u32 w) { return w && winfo(w) != NULL; }

u32 wm_port(void) { return g_wmport; }
u32 wm_first(void) { return rd32(LM_WindowList); }
u32 wm_front(void) {
    for (u32 w = wm_first(); w; w = rd32(w + WIN_NEXT)) if (rd8(w + WIN_VISIBLE)) return w;
    return 0;
}

static void gray_hrgn(HRgn *r) { hrgn_from_guest(r, rd32(LM_GrayRgn)); }

void wm_local_to_global(u32 win, int *x, int *y) {
    Surf s;
    if (!surf_from_port(win, &s)) return;
    *x -= s.bounds.left; *y -= s.bounds.top;
}
void wm_global_rgn_to_local(u32 win, HRgn *r) {
    Surf s;
    if (!surf_from_port(win, &s)) return;
    hrgn_offset(r, s.bounds.left, s.bounds.top);
}
static Rect content_global(u32 win) {
    Rect pr = rd_rect(win + PORT_RECT);
    int x = pr.left, y = pr.top;
    wm_local_to_global(win, &x, &y);
    return mkrect(y, x, y + (pr.bottom - pr.top), x + (pr.right - pr.left));
}

/* ---------------------------------------------------------------------- */
/* Native standard WDEF                                                    */

static bool std_has_title(int procid) {
    int v = procid & 15;
    return (procid >> 4) == 0 ? (v == 0 || v == 4 || v == 5 || v == 8 || v == 12) : true;
}

static void std_calc(u32 win, WInfo *wi) {
    Rect c = content_global(win);
    Rect s = c;
    int v = wi->procid & 15;
    if ((wi->procid >> 4) != 0) v = 0;
    switch (v) {
    case 1: s = mkrect(c.top - 8, c.left - 8, c.bottom + 8, c.right + 8); break;          /* dBoxProc */
    case 2: s = mkrect(c.top - 1, c.left - 1, c.bottom + 1, c.right + 1); break;          /* plainDBox */
    case 3: s = mkrect(c.top - 1, c.left - 1, c.bottom + 3, c.right + 3); break;          /* altDBox */
    default: s = mkrect(c.top - 19, c.left - 1, c.bottom + 2, c.right + 2); break;         /* documents, movable */
    }
    rgn_set_rect(rd32(win + WIN_CONT), c);
    rgn_set_rect(rd32(win + WIN_STRUC), s);
}

static void wm_port_set_vis(const HRgn *vis) {
    hrgn_to_guest(vis, rd32(g_wmport + PORT_VIS));
    rgn_set_rect(rd32(g_wmport + PORT_CLIP), mkrect(-32767, -32767, 32767, 32767));
}

static void std_draw(u32 win, WInfo *wi) {
    u32 port = g_wmport;
    Rect c = content_global(win);
    Rect s = rgn_bbox(rd32(win + WIN_STRUC));
    int v = wi->procid & 15;
    if ((wi->procid >> 4) != 0) v = 0;
    bool hil = rd8(win + WIN_HILITED);
    Paint black = { .kind = 0, .fg = { 0, 0, 0 } }, white = { .kind = 0, .fg = { 0xFFFF, 0xFFFF, 0xFFFF } };
    HRgn a, b, f;
#define FRAME(r_, t_) do { hrgn_rect(&a, (r_).top, (r_).left, (r_).bottom, (r_).right); \
        hrgn_rect(&b, (r_).top + (t_), (r_).left + (t_), (r_).bottom - (t_), (r_).right - (t_)); \
        f.nb = 0; f.b = NULL; hrgn_op(&f, &a, &b, 2); draw_hrgn(port, &f, &black, patCopy); \
        hrgn_free(&a); hrgn_free(&b); hrgn_free(&f); } while (0)
    if (v == 1) {
        draw_rect(port, s, &white, patCopy);
        FRAME(s, 1);
        Rect in = mkrect(s.top + 3, s.left + 3, s.bottom - 3, s.right - 3);
        FRAME(in, 2);
        return;
    }
    if (v == 2) { FRAME(s, 1); return; }
    if (v == 3) {
        Rect fr = mkrect(c.top - 1, c.left - 1, c.bottom + 1, c.right + 1);
        FRAME(fr, 1);
        draw_rect(port, mkrect(c.top + 2, c.right + 1, c.bottom + 3, c.right + 3), &black, patCopy);
        draw_rect(port, mkrect(c.bottom + 1, c.left + 2, c.bottom + 3, c.right + 3), &black, patCopy);
        return;
    }
    /* document window: frame, title bar, shadow */
    Rect fr = mkrect(s.top, s.left, s.bottom - 1, s.right - 1);
    FRAME(fr, 1);
    draw_rect(port, mkrect(s.top + 1, s.right - 1, s.bottom, s.right), &black, patCopy);
    draw_rect(port, mkrect(s.bottom - 1, s.left + 1, s.bottom, s.right), &black, patCopy);
    Rect tb = mkrect(s.top + 1, s.left + 1, c.top - 1, s.right - 2);
    draw_rect(port, tb, &white, patCopy);
    draw_rect(port, mkrect(c.top - 1, s.left + 1, c.top, s.right - 2), &black, patCopy);
    if (hil) for (int y = tb.top + 3; y < tb.bottom - 3; y += 2) draw_rect(port, mkrect(y, tb.left + 1, y + 1, tb.right - 1), &black, patCopy);
    /* title */
    u32 th = rd32(win + WIN_TITLE);
    if (th && hderef(th)) {
        u8 n = rd8(hderef(th));
        u8 buf[256];
        gmemcpy_from(buf, hderef(th) + 1, n);
        wr16(port + PORT_TXFONT, 0); wr16(port + PORT_TXSIZE, 12); wr8(port + PORT_TXFACE, 0);
        wr16(port + PORT_TXMODE, srcOr);
        int tw = text_width(port, buf, n);
        int x = (tb.left + tb.right - tw) / 2;
        draw_rect(port, mkrect(tb.top + 1, x - 6, tb.bottom - 1, x + tw + 6), &white, patCopy);
        wr16(port + PORT_PNLOC + 2, (u16)x); wr16(port + PORT_PNLOC, (u16)(tb.bottom - 5));
        if (is_color_port(port)) wr_rgb(port + PORT_RGBFG, (RGB){ hil ? 0 : 0x8888, hil ? 0 : 0x8888, hil ? 0 : 0x8888 });
        text_draw(port, buf, n);
        if (is_color_port(port)) wr_rgb(port + PORT_RGBFG, (RGB){ 0, 0, 0 });
    }
    if (hil && rd8(win + WIN_GOAWAY)) {
        Rect cb = mkrect(tb.top + 3, tb.left + 7, tb.top + 14, tb.left + 18);
        draw_rect(port, mkrect(cb.top - 1, cb.left - 1, cb.bottom + 1, cb.right + 1), &white, patCopy);
        FRAME(cb, 1);
    }
#undef FRAME
}

static int std_hit(u32 win, WInfo *wi, Point p) {
    Rect c = content_global(win);
    Rect s = rgn_bbox(rd32(win + WIN_STRUC));
    if (p.h < s.left || p.h >= s.right || p.v < s.top || p.v >= s.bottom) return wNoHit;
    if (p.h >= c.left && p.h < c.right && p.v >= c.top && p.v < c.bottom) {
        int v = wi->procid & 15;
        bool grow = (wi->procid >> 4) == 0 && (v == 0 || v == 8);
        if (grow && p.h >= c.right - 15 && p.v >= c.bottom - 15) return wInGrow;
        return wInContent;
    }
    if (std_has_title(wi->procid) && p.v < c.top) {
        if (rd8(win + WIN_GOAWAY) && rd8(win + WIN_HILITED) && p.h >= s.left + 8 && p.h < s.left + 19 && p.v >= s.top + 4 && p.v < s.top + 15)
            return wInGoAway;
        return wInDrag;
    }
    return wNoHit;
}

/* ---------------------------------------------------------------------- */
/* WDEF dispatch                                                           */

static u32 wdef_upp(u32 win) {
    u32 h = rd32(win + WIN_DEFPROC);
    if (!h || !hderef(h)) return 0;
    u32 p = hderef(h);
    if (rd16(p) == 0x4EF9) {
        u32 t = rd32(p + 2);
        if (t) return t;
    }
    return 0;
}

static u32 call_wdef(u32 win, int msg, u32 param) {
    WInfo *wi = winfo(win);
    if (!wi) return 0;
    u32 upp = wi->native ? 0 : wdef_upp(win);
    if (!upp) {
        switch (msg) {
        case wCalcRgns: std_calc(win, wi); return 0;
        case wDraw: std_draw(win, wi); return 0;
        case wHit: return (u32)std_hit(win, wi, pt_from_u32(param));
        default: return 0;
        }
    }
    u32 save = qd_port();
    u32 a[4] = { (u32)wi->var, win, (u32)msg, param };
    u32 r = call_upp(upp, 4, a);
    qd_set_port(save);
    return r;
}

/* ---------------------------------------------------------------------- */
/* Visibility and exposure                                                 */

static void erase_content_global(u32 win, const HRgn *g) {
    (void)win;
    Paint white = { .kind = 0, .fg = { 0xFFFF, 0xFFFF, 0xFFFF } };
    HRgn v; hrgn_copy(&v, g);
    wm_port_set_vis(&v);
    draw_hrgn(g_wmport, &v, &white, patCopy);
    hrgn_free(&v);
}

static void paint_desktop(const HRgn *g) {
    static const u8 gray[8] = { 0xAA, 0x55, 0xAA, 0x55, 0xAA, 0x55, 0xAA, 0x55 };
    Paint p; paint_from_pattern(&p, g_wmport, gray);
    p.fg = (RGB){ 0x5555, 0x5555, 0x7777 }; p.bk = (RGB){ 0x9999, 0x9999, 0xBBBB };
    HRgn v; hrgn_copy(&v, g);
    wm_port_set_vis(&v);
    draw_hrgn(g_wmport, &v, &p, patCopy);
    hrgn_free(&v);
}

void wm_recalc(const HRgn *damage) {
    HRgn gray; gray_hrgn(&gray);
    HRgn covered = { 0, NULL };
    for (u32 w = wm_first(); w; w = rd32(w + WIN_NEXT)) {
        if (!rd8(w + WIN_VISIBLE)) {
            rgn_set_rect(rd32(w + PORT_VIS), (Rect){ 0, 0, 0, 0 });
            continue;
        }
        HRgn st, ct, vs = { 0, NULL }, vc = { 0, NULL };
        hrgn_from_guest(&st, rd32(w + WIN_STRUC));
        hrgn_from_guest(&ct, rd32(w + WIN_CONT));
        hrgn_op(&vs, &st, &gray, 1);
        hrgn_op(&vs, &vs, &covered, 2);
        hrgn_op(&vc, &ct, &vs, 1);
        /* port visRgn in local coordinates */
        HRgn loc; hrgn_copy(&loc, &vc);
        wm_global_rgn_to_local(w, &loc);
        hrgn_to_guest(&loc, rd32(w + PORT_VIS));
        hrgn_free(&loc);
        if (damage) {
            HRgn d = { 0, NULL };
            hrgn_op(&d, damage, &vs, 1);
            if (!hrgn_empty(&d)) {
                HRgn fr = { 0, NULL }, cn = { 0, NULL };
                hrgn_op(&fr, &d, &ct, 2);
                hrgn_op(&cn, &d, &ct, 1);
                if (!hrgn_empty(&cn)) {
                    erase_content_global(w, &cn);
                    HRgn up, nu = { 0, NULL };
                    hrgn_from_guest(&up, rd32(w + WIN_UPDATE));
                    hrgn_op(&nu, &up, &cn, 0);
                    hrgn_to_guest(&nu, rd32(w + WIN_UPDATE));
                    hrgn_free(&up); hrgn_free(&nu);
                }
                if (!hrgn_empty(&fr)) {
                    u32 save = qd_port();
                    qd_set_port(g_wmport);
                    wm_port_set_vis(&fr);
                    call_wdef(w, wDraw, 0);
                    qd_set_port(save);
                }
                hrgn_free(&fr); hrgn_free(&cn);
            }
            hrgn_free(&d);
        }
        hrgn_op(&covered, &covered, &st, 0);
        hrgn_free(&st); hrgn_free(&ct); hrgn_free(&vs); hrgn_free(&vc);
    }
    if (damage) {
        HRgn desk = { 0, NULL };
        hrgn_op(&desk, damage, &gray, 1);
        hrgn_op(&desk, &desk, &covered, 2);
        if (!hrgn_empty(&desk)) paint_desktop(&desk);
        hrgn_free(&desk);
    }
    /* restore WMgrPort vis to the whole gray region */
    hrgn_to_guest(&gray, rd32(g_wmport + PORT_VIS));
    hrgn_free(&gray); hrgn_free(&covered);
    qd_screen_dirty();
}

void wm_invalidate_global(const HRgn *g) { wm_recalc(g); }

static void damage_struct(u32 w) {
    HRgn st; hrgn_from_guest(&st, rd32(w + WIN_STRUC));
    wm_recalc(&st);
    hrgn_free(&st);
}

void wm_draw_frame(u32 w) {
    if (!rd8(w + WIN_VISIBLE)) return;
    HRgn st, ct, fr = { 0, NULL }, gray, covered = { 0, NULL };
    hrgn_from_guest(&st, rd32(w + WIN_STRUC));
    hrgn_from_guest(&ct, rd32(w + WIN_CONT));
    gray_hrgn(&gray);
    for (u32 x = wm_first(); x && x != w; x = rd32(x + WIN_NEXT)) {
        if (!rd8(x + WIN_VISIBLE)) continue;
        HRgn o; hrgn_from_guest(&o, rd32(x + WIN_STRUC));
        hrgn_op(&covered, &covered, &o, 0);
        hrgn_free(&o);
    }
    hrgn_op(&fr, &st, &ct, 2);
    hrgn_op(&fr, &fr, &gray, 1);
    hrgn_op(&fr, &fr, &covered, 2);
    u32 save = qd_port();
    qd_set_port(g_wmport);
    wm_port_set_vis(&fr);
    call_wdef(w, wDraw, 0);
    hrgn_to_guest(&gray, rd32(g_wmport + PORT_VIS));
    qd_set_port(save);
    hrgn_free(&st); hrgn_free(&ct); hrgn_free(&fr); hrgn_free(&gray); hrgn_free(&covered);
}

void wm_activate_changed(void) {
    u32 f = wm_front();
    if (f == g_last_front) return;
    if (g_last_front && wm_is_window(g_last_front)) ev_post(8, g_last_front, 0);
    if (f) ev_post(8, f, 1);
    g_last_front = f;
}

/* ---------------------------------------------------------------------- */
/* Window creation                                                         */

static void link_window(u32 win, u32 behind) {
    u32 first = wm_first();
    if (behind == 0xFFFFFFFFu || !first) {
        wr32(win + WIN_NEXT, first);
        wr32(LM_WindowList, win);
        return;
    }
    if (behind == 0) {
        u32 w = first;
        while (rd32(w + WIN_NEXT)) w = rd32(w + WIN_NEXT);
        wr32(w + WIN_NEXT, win);
        wr32(win + WIN_NEXT, 0);
        return;
    }
    wr32(win + WIN_NEXT, rd32(behind + WIN_NEXT));
    wr32(behind + WIN_NEXT, win);
}

static void unlink_window(u32 win) {
    u32 prev = 0, w = wm_first();
    while (w && w != win) { prev = w; w = rd32(w + WIN_NEXT); }
    if (!w) return;
    if (prev) wr32(prev + WIN_NEXT, rd32(win + WIN_NEXT));
    else wr32(LM_WindowList, rd32(win + WIN_NEXT));
    wr32(win + WIN_NEXT, 0);
}

static u32 new_string_handle(const char *s) {
    size_t n = strlen(s);
    u32 h = mm_new_handle((u32)n + 1, false, ZONE_APP);
    c_to_pstr(s, hderef(h), 255);
    return h;
}

u32 wm_create(u32 storage, Rect bounds, const char *title, bool visible, s16 procid,
              u32 behind, bool goaway, u32 refcon, bool color) {
    u32 win = storage ? storage : mm_new_ptr(WIN_SIZE, true, ZONE_APP);
    gmemset(win, 0, WIN_SIZE);
    u32 save = qd_port();
    /* port on the screen, local (0,0) at bounds.topLeft */
    u32 dpm = qd_screen_pixmap();
    if (color) {
        u32 pmh = mm_new_handle(PM_SIZE, false, ZONE_APP);
        gmemmove(hderef(pmh), hderef(dpm), PM_SIZE);
        wr32(win + PORT_BITS, pmh);
        wr16(win + PORT_VERSION, 0xC000);
    } else {
        wr32(win + PORT_BITS, qd_screen_base());
        wr16(win + PORT_BITS + 4, (u16)(rd16(hderef(dpm) + PM_ROWBYTES) & 0x3FFF));
    }
    Rect sb = mkrect(-bounds.top, -bounds.left, qd_screen_h() - bounds.top, qd_screen_w() - bounds.left);
    wr_rect((color ? hderef(rd32(win + PORT_BITS)) : win + PORT_BITS) + 6, sb);
    port_init(win, color);
    if (color) port_make_window_port(win);
    wr_rect(win + PORT_RECT, mkrect(0, 0, bounds.bottom - bounds.top, bounds.right - bounds.left));
    rgn_set_rect(rd32(win + PORT_VIS), (Rect){ 0, 0, 0, 0 });
    wr16(win + PORT_TXFONT, 0);
    wr16(win + WIN_KIND, userKind);
    wr8(win + WIN_VISIBLE, visible);
    wr8(win + WIN_HILITED, 0);
    wr8(win + WIN_GOAWAY, goaway);
    wr32(win + WIN_STRUC, rgn_new());
    wr32(win + WIN_CONT, rgn_new());
    wr32(win + WIN_UPDATE, rgn_new());
    wr32(win + WIN_TITLE, new_string_handle(title));
    wr32(win + WIN_REFCON, refcon);
    WInfo *wi = winfo_new(win);
    wi->procid = procid;
    wi->var = procid & 15;
    s16 wdefid = (s16)(procid >> 4);
    u32 defh = res_get(FOURCC('W','D','E','F'), wdefid);
    if (defh && hderef(defh) && rd16(hderef(defh)) == 0x4EF9) {
        wr32(win + WIN_DEFPROC, defh);
    } else {
        wi->native = true;
        wr32(win + WIN_DEFPROC, defh ? defh : mm_new_handle(4, true, ZONE_APP));
    }
    link_window(win, behind);
    call_wdef(win, wNew, 0);
    call_wdef(win, wCalcRgns, 0);
    qd_set_port(save);
    if (visible) {
        if (wm_front() == win) {
            for (u32 w = wm_first(); w; w = rd32(w + WIN_NEXT)) {
                bool h = w == win;
                if (rd8(w + WIN_HILITED) != h) { wr8(w + WIN_HILITED, h); if (w != win) wm_draw_frame(w); }
            }
        }
        damage_struct(win);
        wm_activate_changed();
    } else wm_recalc(NULL);
    qd_set_port(win);
    LOG_D("window %08x created proc %d (%d,%d,%d,%d) '%s'%s", win, procid, bounds.top, bounds.left,
          bounds.bottom, bounds.right, title, wi->native ? " [native wdef]" : "");
    return win;
}

TRAP(InitWindows) {
    /* WMgr port covering the screen */
    g_wmport = mm_new_ptr(PORT_SIZE, true, ZONE_SYS);
    u32 save = qd_port();
    u32 pmh = mm_new_handle(PM_SIZE, false, ZONE_SYS);
    gmemmove(hderef(pmh), hderef(qd_screen_pixmap()), PM_SIZE);
    wr32(g_wmport + PORT_BITS, pmh);
    wr16(g_wmport + PORT_VERSION, 0xC000);
    port_init(g_wmport, true);
    qd_set_port(save ? save : g_wmport);
    /* gray region: screen minus menu bar */
    u32 gray = rgn_new();
    int mb = rds16(LM_MBarHeight);
    rgn_set_rect(gray, mkrect(mb, 0, qd_screen_h(), qd_screen_w()));
    wr32(LM_GrayRgn, gray);
    rgn_set_rect(rd32(g_wmport + PORT_VIS), mkrect(0, 0, qd_screen_h(), qd_screen_w()));
    wr32(LM_WindowList, 0);
    HRgn all; hrgn_from_guest(&all, gray);
    paint_desktop(&all);
    hrgn_free(&all);
    menu_draw_bar();
}

TRAP(GetGrayRgn) { RET(rd32(LM_GrayRgn)); }
TRAP(LMGetGrayRgn) { RET(rd32(LM_GrayRgn)); }

TRAP(NewCWindow) {
    u32 storage = ARG(0); Rect b = rd_rect(ARG(1)); u32 tp = ARG(2); bool vis = ARGB(3);
    s16 proc = ARGS16(4); u32 behind = ARG(5); bool goaway = ARGB(6); u32 refcon = ARG(7);
    char title[256]; pstr_to_c(tp, title, sizeof title);
    RET(wm_create(storage, b, title, vis, proc, behind, goaway, refcon, true));
}
TRAP(NewWindow) {
    u32 storage = ARG(0); Rect b = rd_rect(ARG(1)); u32 tp = ARG(2); bool vis = ARGB(3);
    s16 proc = ARGS16(4); u32 behind = ARG(5); bool goaway = ARGB(6); u32 refcon = ARG(7);
    char title[256]; pstr_to_c(tp, title, sizeof title);
    RET(wm_create(storage, b, title, vis, proc, behind, goaway, refcon, true));
}

static u32 get_new_window(CPU *cpu, bool color) {
    s16 id = ARGS16(0); u32 storage = ARG(1), behind = ARG(2);
    u32 len;
    u8 *d = res_load_raw(FOURCC('W','I','N','D'), id, &len);
    if (!d) { LOG_W("WIND %d not found", id); return 0; }
    Rect b = { (s16)be16(d), (s16)be16(d + 2), (s16)be16(d + 4), (s16)be16(d + 6) };
    s16 proc = (s16)be16(d + 8);
    bool vis = be16(d + 10) != 0, goaway = be16(d + 12) != 0;
    u32 refcon = be32(d + 14);
    char title[256];
    int tl = d[18];
    memcpy(title, d + 19, (size_t)tl); title[tl] = 0;
    /* optional positioning word after the (word-aligned) title */
    u32 posoff = (u32)(19 + tl + 1) & ~1u;
    if (posoff + 2 <= len) {
        u16 pos = be16(d + posoff);
        int w = b.right - b.left, h = b.bottom - b.top;
        int mb = rds16(LM_MBarHeight);
        if ((pos & 0x3FFF) == 0x280A || (pos & 0x3FFF) == 0x300A) { /* center / alert position on main screen */
            int x = (qd_screen_w() - w) / 2, y = (pos & 0x3FFF) == 0x280A ? mb + (qd_screen_h() - mb - h) / 2 : mb + (qd_screen_h() - mb - h) / 3;
            b = mkrect(y, x, y + h, x + w);
        }
    }
    free(d);
    return wm_create(storage, b, title, vis, proc, behind, goaway, refcon, color);
}
TRAP(GetNewCWindow) { RET(get_new_window(cpu, true)); }
TRAP(GetNewWindow) { RET(get_new_window(cpu, true)); }

static void dispose_window(u32 win, bool free_storage) {
    WInfo *wi = winfo(win);
    if (!wi) return;
    bool vis = rd8(win + WIN_VISIBLE);
    HRgn st; hrgn_from_guest(&st, rd32(win + WIN_STRUC));
    call_wdef(win, wDispose, 0);
    extern void ctl_dispose_all(u32 win);
    ctl_dispose_all(win);
    unlink_window(win);
    wi->win = 0;
    if (vis) wm_recalc(&st);
    hrgn_free(&st);
    rgn_dispose(rd32(win + WIN_STRUC));
    rgn_dispose(rd32(win + WIN_CONT));
    rgn_dispose(rd32(win + WIN_UPDATE));
    rgn_dispose(rd32(win + PORT_VIS));
    rgn_dispose(rd32(win + PORT_CLIP));
    if (rd32(win + WIN_TITLE)) mm_dispose_handle(rd32(win + WIN_TITLE));
    if (qd_port() == win) qd_set_port(g_wmport);
    if (g_last_front == win) g_last_front = 0;
    /* hilite new front */
    u32 f = wm_front();
    if (f && !rd8(f + WIN_HILITED)) { wr8(f + WIN_HILITED, 1); wm_draw_frame(f); }
    wm_activate_changed();
    if (free_storage) mm_dispose_ptr(win);
}
TRAP(DisposeWindow) { dispose_window(ARG(0), true); }
TRAP(CloseWindow) { dispose_window(ARG(0), false); }

void wm_show(u32 win, bool show) {
    if (!winfo(win)) return;
    if ((rd8(win + WIN_VISIBLE) != 0) == show) return;
    u32 oldfront = wm_front();
    wr8(win + WIN_VISIBLE, show);
    if (show) {
        if (wm_front() == win) {
            wr8(win + WIN_HILITED, 1);
            if (oldfront && oldfront != win) { wr8(oldfront + WIN_HILITED, 0); wm_draw_frame(oldfront); }
        }
        damage_struct(win);
    } else {
        HRgn st; hrgn_from_guest(&st, rd32(win + WIN_STRUC));
        wm_recalc(&st);
        hrgn_free(&st);
        if (oldfront == win) {
            wr8(win + WIN_HILITED, 0);
            u32 f = wm_front();
            if (f) { wr8(f + WIN_HILITED, 1); wm_draw_frame(f); }
        }
    }
    wm_activate_changed();
}
TRAP(ShowWindow) { wm_show(ARG(0), true); }
TRAP(HideWindow) { wm_show(ARG(0), false); }
TRAP(ShowHide) { wm_show(ARG(0), ARGB(1)); }

void wm_select(u32 win) {
    if (!winfo(win)) return;
    u32 oldfront = wm_front();
    if (wm_first() != win) {
        unlink_window(win);
        link_window(win, 0xFFFFFFFFu);
    }
    if (oldfront && oldfront != win) { wr8(oldfront + WIN_HILITED, 0); wm_draw_frame(oldfront); }
    if (rd8(win + WIN_VISIBLE)) {
        bool was = rd8(win + WIN_HILITED);
        wr8(win + WIN_HILITED, 1);
        if (oldfront != win) damage_struct(win);
        else if (!was) wm_draw_frame(win);
    }
    wm_activate_changed();
}
TRAP(SelectWindow) { wm_select(ARG(0)); }
TRAP(BringToFront) {
    u32 win = ARG(0);
    if (!winfo(win) || wm_first() == win) return;
    unlink_window(win);
    link_window(win, 0xFFFFFFFFu);
    if (rd8(win + WIN_VISIBLE)) damage_struct(win);
    wm_activate_changed();
}
TRAP(SendBehind) {
    u32 win = ARG(0), behind = ARG(1);
    if (!winfo(win)) return;
    HRgn st; hrgn_from_guest(&st, rd32(win + WIN_STRUC));
    unlink_window(win);
    link_window(win, behind ? behind : 0);
    wm_recalc(&st);
    hrgn_free(&st);
    wm_activate_changed();
}
TRAP(HiliteWindow) {
    u32 win = ARG(0); bool h = ARGB(1);
    if (!winfo(win) || (rd8(win + WIN_HILITED) != 0) == h) return;
    wr8(win + WIN_HILITED, h);
    wm_draw_frame(win);
}

static void move_window(u32 win, int h, int v, bool front) {
    HRgn old; hrgn_from_guest(&old, rd32(win + WIN_STRUC));
    Rect pr = rd_rect(win + PORT_RECT);
    u32 bm = is_color_port(win) ? hderef(rd32(win + PORT_BITS)) : win + PORT_BITS;
    Rect b = rd_rect(bm + 6);
    /* new global origin of portRect.topLeft is (h, v) */
    int dx = h - (pr.left - b.left), dy = v - (pr.top - b.top);
    if (dx || dy) {
        b.left = (s16)(b.left - dx); b.right = (s16)(b.right - dx);
        b.top = (s16)(b.top - dy); b.bottom = (s16)(b.bottom - dy);
        wr_rect(bm + 6, b);
        port_mirror_bounds(win);
        call_wdef(win, wCalcRgns, 0);
    }
    if (front) {
        u32 oldfront = wm_front();
        if (wm_first() != win) { unlink_window(win); link_window(win, 0xFFFFFFFFu); }
        if (oldfront && oldfront != win) { wr8(oldfront + WIN_HILITED, 0); wm_draw_frame(oldfront); }
        if (rd8(win + WIN_VISIBLE)) wr8(win + WIN_HILITED, 1);
    }
    if (rd8(win + WIN_VISIBLE)) {
        HRgn nw, dmg = { 0, NULL };
        hrgn_from_guest(&nw, rd32(win + WIN_STRUC));
        hrgn_op(&dmg, &old, &nw, 0);
        /* also forget pending updates of the window: redraw all of it */
        wm_recalc(&dmg);
        hrgn_free(&nw); hrgn_free(&dmg);
    } else wm_recalc(NULL);
    hrgn_free(&old);
    if (front) wm_activate_changed();
}

TRAP(MoveWindow) { move_window(ARG(0), ARGS16(1), ARGS16(2), ARGB(3)); }

TRAP(SizeWindow) {
    u32 win = ARG(0); s16 w = ARGS16(1), h = ARGS16(2); bool upd = ARGB(3);
    if (!winfo(win)) return;
    HRgn old; hrgn_from_guest(&old, rd32(win + WIN_STRUC));
    Rect pr = rd_rect(win + PORT_RECT);
    pr.right = (s16)(pr.left + w); pr.bottom = (s16)(pr.top + h);
    wr_rect(win + PORT_RECT, pr);
    call_wdef(win, wCalcRgns, 0);
    if (rd8(win + WIN_VISIBLE)) {
        HRgn nw, dmg = { 0, NULL };
        hrgn_from_guest(&nw, rd32(win + WIN_STRUC));
        hrgn_op(&dmg, &old, &nw, 0);
        if (!upd) {
            /* only uncovered areas outside the old content get updates */
        }
        wm_recalc(&dmg);
        hrgn_free(&nw); hrgn_free(&dmg);
    }
    hrgn_free(&old);
}

TRAP(FindWindow) {
    Point p = pt_from_u32(ARG(0)); u32 wp = ARG(1);
    u32 win = 0;
    int part = wm_find(p, &win);
    if (wp) wr32(wp, win);
    RET(part);
}

int wm_find(Point p, u32 *winout) {
    *winout = 0;
    if (p.v >= 0 && p.v < rds16(LM_MBarHeight)) return inMenuBar;
    for (u32 w = wm_first(); w; w = rd32(w + WIN_NEXT)) {
        if (!rd8(w + WIN_VISIBLE)) continue;
        HRgn st; hrgn_from_guest(&st, rd32(w + WIN_STRUC));
        bool in = hrgn_contains(&st, p.h, p.v);
        hrgn_free(&st);
        if (!in) continue;
        *winout = w;
        u32 hit = call_wdef(w, wHit, pt_to_u32(p));
        switch (hit) {
        case wInContent: return inContent;
        case wInDrag: return inDrag;
        case wInGrow: return inGrow;
        case wInGoAway: return inGoAway;
        case wInZoomIn: return inZoomIn;
        case wInZoomOut: return inZoomOut;
        default: return inDesk;
        }
    }
    return inDesk;
}

TRAP(FrontWindow) { RET(wm_front()); }

TRAP(SetWTitle) {
    u32 win = ARG(0), tp = ARG(1);
    u32 th = rd32(win + WIN_TITLE);
    u32 n = rd8(tp) + 1u;
    mm_set_handle_size(th, n);
    gmemmove(hderef(th), tp, n);
    wm_draw_frame(win);
}
TRAP(GetWTitle) {
    u32 win = ARG(0), out = ARG(1);
    u32 th = rd32(win + WIN_TITLE);
    if (th && hderef(th)) gmemmove(out, hderef(th), rd8(hderef(th)) + 1u);
    else wr8(out, 0);
}
TRAP(SetWRefCon) { wr32(ARG(0) + WIN_REFCON, ARG(1)); }
TRAP(GetWRefCon) { RET(rd32(ARG(0) + WIN_REFCON)); }
TRAP(GetWVariant) { WInfo *wi = winfo(ARG(0)); RET(wi ? wi->var : 0); }
TRAP(SetWindowPic) { wr32(ARG(0) + WIN_PIC, ARG(1)); }
TRAP(GetWindowPic) { RET(rd32(ARG(0) + WIN_PIC)); }

/* ---- update regions ---- */
static void inval_local(u32 port, HRgn *r, bool add) {
    if (!wm_is_window(port)) return;
    HRgn g; hrgn_copy(&g, r);
    Surf s; surf_from_port(port, &s);
    hrgn_offset(&g, -s.bounds.left, -s.bounds.top);
    HRgn up, out = { 0, NULL };
    hrgn_from_guest(&up, rd32(port + WIN_UPDATE));
    if (add) {
        HRgn ct; hrgn_from_guest(&ct, rd32(port + WIN_CONT));
        hrgn_op(&g, &g, &ct, 1);
        hrgn_free(&ct);
        hrgn_op(&out, &up, &g, 0);
    } else hrgn_op(&out, &up, &g, 2);
    hrgn_to_guest(&out, rd32(port + WIN_UPDATE));
    hrgn_free(&g); hrgn_free(&up); hrgn_free(&out);
}
TRAP(InvalRect) { Rect rc = rd_rect(ARG(0)); HRgn r; hrgn_rect(&r, rc.top, rc.left, rc.bottom, rc.right); inval_local(qd_port(), &r, true); hrgn_free(&r); }
TRAP(ValidRect) { Rect rc = rd_rect(ARG(0)); HRgn r; hrgn_rect(&r, rc.top, rc.left, rc.bottom, rc.right); inval_local(qd_port(), &r, false); hrgn_free(&r); }
TRAP(InvalRgn) { HRgn r; hrgn_from_guest(&r, ARG(0)); inval_local(qd_port(), &r, true); hrgn_free(&r); }
TRAP(ValidRgn) { HRgn r; hrgn_from_guest(&r, ARG(0)); inval_local(qd_port(), &r, false); hrgn_free(&r); }

/* BeginUpdate: visRgn := visRgn ∩ updateRgn (local); EndUpdate restores */
static u32 g_saved_vis[16][2];
TRAP(BeginUpdate) {
    u32 win = ARG(0);
    if (!wm_is_window(win)) return;
    u32 vis = rd32(win + PORT_VIS);
    u32 copy = rgn_new();
    u32 n = mm_handle_size(vis);
    mm_set_handle_size(copy, n);
    gmemmove(hderef(copy), hderef(vis), n);
    for (int i = 0; i < 16; i++) if (!g_saved_vis[i][0]) { g_saved_vis[i][0] = win; g_saved_vis[i][1] = copy; break; }
    HRgn v, u, o = { 0, NULL };
    hrgn_from_guest(&v, vis);
    hrgn_from_guest(&u, rd32(win + WIN_UPDATE));
    wm_global_rgn_to_local(win, &u);
    hrgn_op(&o, &v, &u, 1);
    hrgn_to_guest(&o, vis);
    hrgn_free(&v); hrgn_free(&u); hrgn_free(&o);
    rgn_set_rect(rd32(win + WIN_UPDATE), (Rect){ 0, 0, 0, 0 });
}
TRAP(EndUpdate) {
    u32 win = ARG(0);
    for (int i = 0; i < 16; i++) if (g_saved_vis[i][0] == win) {
        u32 copy = g_saved_vis[i][1];
        u32 vis = rd32(win + PORT_VIS);
        u32 n = mm_handle_size(copy);
        mm_set_handle_size(vis, n);
        gmemmove(hderef(vis), hderef(copy), n);
        rgn_dispose(copy);
        g_saved_vis[i][0] = 0;
        return;
    }
}

TRAP(CalcVisBehind) { wm_recalc(NULL); }
TRAP(CalcVis) { wm_recalc(NULL); }
TRAP(PaintBehind) {
    HRgn r; hrgn_from_guest(&r, ARG(1));
    wm_recalc(&r);
    hrgn_free(&r);
}
TRAP(PaintOne) {
    HRgn r; hrgn_from_guest(&r, ARG(1));
    wm_recalc(&r);
    hrgn_free(&r);
}

TRAP(DrawGrowIcon) {
    u32 win = ARG(0);
    WInfo *wi = winfo(win);
    if (!wi) return;
    if (!wi->native) { call_wdef(win, wDrawGIcon, 0); return; }
    Rect pr = rd_rect(win + PORT_RECT);
    Paint black = { .kind = 0 };
    u32 save = qd_port();
    qd_set_port(win);
    draw_rect(win, mkrect(pr.top, pr.right - 16, pr.bottom - 15, pr.right - 15), &black, patCopy);
    draw_rect(win, mkrect(pr.bottom - 16, pr.left, pr.bottom - 15, pr.right - 15), &black, patCopy);
    qd_set_port(save);
}

/* AuxWinRec (awNext, awOwner, awCTable, dialogCItem, awFlags, awReserved, awRefCon) */
TRAP(GetAuxWin) {
    u32 win = ARG(0), hp = ARG(1);
    static u32 aux;
    if (!aux) {
        aux = mm_new_handle(28, true, ZONE_SYS);
        u32 ct = mm_new_handle(8 + 5 * 8, true, ZONE_SYS);
        u32 p = hderef(ct);
        wr16(p + 6, 4);
        RGB cols[5] = { { 0xFFFF, 0xFFFF, 0xFFFF }, { 0, 0, 0 }, { 0, 0, 0 }, { 0, 0, 0 }, { 0xFFFF, 0xFFFF, 0xFFFF } };
        for (int i = 0; i < 5; i++) { wr16(p + 8 + 8 * (u32)i, (u16)i); wr_rgb(p + 10 + 8 * (u32)i, cols[i]); }
        wr32(hderef(aux) + 8, ct);
    }
    wr32(hderef(aux) + 4, win);
    if (hp) wr32(hp, aux);
    RET(1);
}

/* ---------------------------------------------------------------------- */
/* Tracking loops                                                          */

static void xor_outline(const HRgn *r) {
    /* 1-pixel gray outline of a region, drawn with XOR */
    HRgn in, f = { 0, NULL }, tmp;
    hrgn_copy(&in, r);
    hrgn_copy(&tmp, r); hrgn_offset(&tmp, 1, 0); hrgn_op(&in, &in, &tmp, 1); hrgn_free(&tmp);
    hrgn_copy(&tmp, r); hrgn_offset(&tmp, -1, 0); hrgn_op(&in, &in, &tmp, 1); hrgn_free(&tmp);
    hrgn_copy(&tmp, r); hrgn_offset(&tmp, 0, 1); hrgn_op(&in, &in, &tmp, 1); hrgn_free(&tmp);
    hrgn_copy(&tmp, r); hrgn_offset(&tmp, 0, -1); hrgn_op(&in, &in, &tmp, 1); hrgn_free(&tmp);
    hrgn_op(&f, r, &in, 2);
    u32 save = qd_port();
    qd_set_port(g_wmport);
    HRgn all; hrgn_rect(&all, 0, 0, qd_screen_h(), qd_screen_w());
    wm_port_set_vis(&all);
    static const u8 gray[8] = { 0xAA, 0x55, 0xAA, 0x55, 0xAA, 0x55, 0xAA, 0x55 };
    Paint p; paint_from_pattern(&p, g_wmport, gray);
    draw_hrgn(g_wmport, &f, &p, patXor);
    HRgn gr; hrgn_from_guest(&gr, rd32(LM_GrayRgn));
    hrgn_to_guest(&gr, rd32(g_wmport + PORT_VIS));
    hrgn_free(&gr); hrgn_free(&all);
    qd_set_port(save);
    hrgn_free(&in); hrgn_free(&f);
}

/* Drag an outline; returns (dv<<16)|dh or 0x80008000 if outside slop. */
static u32 drag_gray(const HRgn *rgn, Point start, Rect limit, Rect slop, int axis) {
    Point cur = start;
    HRgn r; hrgn_copy(&r, rgn);
    bool shown = false;
    int dh = 0, dv = 0;
    while (ev_mouse_button()) {
        Point m = ev_mouse_global();
        if (m.h < limit.left) m.h = limit.left;
        if (m.h >= limit.right) m.h = (s16)(limit.right - 1);
        if (m.v < limit.top) m.v = limit.top;
        if (m.v >= limit.bottom) m.v = (s16)(limit.bottom - 1);
        if (axis == 1) m.v = start.v;
        if (axis == 2) m.h = start.h;
        if (m.h != cur.h || m.v != cur.v || !shown) {
            if (shown) xor_outline(&r);
            hrgn_free(&r); hrgn_copy(&r, rgn);
            dh = m.h - start.h; dv = m.v - start.v;
            hrgn_offset(&r, dh, dv);
            xor_outline(&r);
            shown = true;
            cur = m;
        }
        ev_idle_frame();
    }
    if (shown) xor_outline(&r);
    hrgn_free(&r);
    Point m = ev_mouse_global();
    if (m.h < slop.left || m.h >= slop.right || m.v < slop.top || m.v >= slop.bottom) return 0x80008000u;
    return (u32)(u16)dv << 16 | (u16)dh;
}

TRAP(DragGrayRgn) {
    u32 rgn = ARG(0); Point start = pt_from_u32(ARG(1)); Rect limit = rd_rect(ARG(2)), slop = rd_rect(ARG(3));
    s16 axis = ARGS16(4);
    HRgn r; hrgn_from_guest(&r, rgn);
    u32 res = drag_gray(&r, start, limit, slop, axis);
    hrgn_free(&r);
    RET(res);
}

TRAP(DragWindow) {
    u32 win = ARG(0); Point start = pt_from_u32(ARG(1)); u32 bp = ARG(2);
    Rect limit = bp ? rd_rect(bp) : mkrect(-32767, -32767, 32767, 32767);
    bool cmd = (ev_modifiers() & 0x0100) != 0;
    HRgn st; hrgn_from_guest(&st, rd32(win + WIN_STRUC));
    Rect slop = mkrect(limit.top - 4, limit.left - 4, limit.bottom + 4, limit.right + 4);
    u32 res = drag_gray(&st, start, limit, slop, 0);
    hrgn_free(&st);
    if (res != 0x80008000u && res) {
        s16 dh = (s16)res, dv = (s16)(res >> 16);
        Rect c = content_global(win);
        move_window(win, c.left + dh, c.top + dv, false);
    }
    if (!cmd) wm_select(win);
}

TRAP(GrowWindow) {
    u32 win = ARG(0); Point start = pt_from_u32(ARG(1)); Rect sz = rd_rect(ARG(2));
    Rect c = content_global(win);
    HRgn r;
    Point cur = start;
    int w = c.right - c.left, h = c.bottom - c.top;
    bool shown = false;
    hrgn_rect(&r, c.top, c.left, c.bottom, c.right);
    while (ev_mouse_button()) {
        Point m = ev_mouse_global();
        int nw = w + (m.h - start.h), nh = h + (m.v - start.v);
        if (nw < sz.left) nw = sz.left;
        if (nw > sz.right) nw = sz.right;
        if (nh < sz.top) nh = sz.top;
        if (nh > sz.bottom) nh = sz.bottom;
        if (m.h != cur.h || m.v != cur.v || !shown) {
            if (shown) xor_outline(&r);
            hrgn_free(&r);
            hrgn_rect(&r, c.top, c.left, c.top + nh, c.left + nw);
            xor_outline(&r);
            shown = true; cur = m;
        }
        ev_idle_frame();
    }
    if (shown) xor_outline(&r);
    Rect bb; hrgn_bbox(&r, &bb);
    hrgn_free(&r);
    int nw = bb.right - bb.left, nh = bb.bottom - bb.top;
    if (nw == w && nh == h) { RET(0); return; }
    RET((u32)(u16)nh << 16 | (u16)nw);
}

static bool track_part(u32 win, Point pt, int part) {
    (void)pt;
    bool in = true;
    while (ev_mouse_button()) {
        u32 w2;
        in = wm_find(ev_mouse_global(), &w2) == part && w2 == win;
        ev_idle_frame();
    }
    u32 w2;
    return wm_find(ev_mouse_global(), &w2) == part && w2 == win;
}
TRAP(TrackGoAway) { RET(track_part(ARG(0), pt_from_u32(ARG(1)), inGoAway)); }
TRAP(TrackBox) { RET(track_part(ARG(0), pt_from_u32(ARG(1)), ARGS16(2))); }

TRAP(ZoomWindow) {
    u32 win = ARG(0); s16 part = ARGS16(1); bool front = ARGB(2);
    WInfo *wi = winfo(win);
    if (!wi) return;
    u32 dh = rd32(win + WIN_DATA);
    Rect c = content_global(win);
    if (!dh) {
        dh = mm_new_handle(16, true, ZONE_APP);
        wr32(win + WIN_DATA, dh);
        wr_rect(hderef(dh), c);
        int mb = rds16(LM_MBarHeight);
        wr_rect(hderef(dh) + 8, mkrect(mb + 22, 4, qd_screen_h() - 4, qd_screen_w() - 4));
    }
    Rect target = part == inZoomOut ? rd_rect(hderef(dh) + 8) : rd_rect(hderef(dh));
    if (part == inZoomOut) wr_rect(hderef(dh), c);
    wi->zoomed = part == inZoomOut;
    u32 save = qd_port();
    qd_set_port(win);
    move_window(win, target.left, target.top, front);
    Rect pr = rd_rect(win + PORT_RECT);
    pr.right = (s16)(pr.left + target.right - target.left);
    pr.bottom = (s16)(pr.top + target.bottom - target.top);
    wr_rect(win + PORT_RECT, pr);
    call_wdef(win, wCalcRgns, 0);
    HRgn st; hrgn_from_guest(&st, rd32(win + WIN_STRUC));
    wm_recalc(&st);
    hrgn_free(&st);
    qd_set_port(save);
}

TRAP(CheckUpdate) {
    u32 evp = ARG(0);
    for (u32 w = wm_first(); w; w = rd32(w + WIN_NEXT)) {
        if (!rd8(w + WIN_VISIBLE)) continue;
        Rect bb = rgn_bbox(rd32(w + WIN_UPDATE));
        if (!rect_empty(bb)) {
            wr16(evp, 6); wr32(evp + 2, w);
            RET(1);
            return;
        }
    }
    RET(0);
}

TRAP(SetThemeWindowBackground) { RETERR(noErr); }

void wm_debug_dump(void) {
    for (u32 w = wm_first(); w; w = rd32(w + WIN_NEXT)) {
        Rect s = rgn_bbox(rd32(w + WIN_STRUC)), c = rgn_bbox(rd32(w + WIN_CONT)), v = rgn_bbox(rd32(w + PORT_VIS));
        Rect u = rgn_bbox(rd32(w + WIN_UPDATE)), pr = rd_rect(w + PORT_RECT);
        Surf sf; surf_from_port(w, &sf);
        WInfo *wi = winfo(w);
        LOG_I("win %08x vis=%d hil=%d kind=%d proc=%d%s port(%d,%d,%d,%d) bounds(%d,%d) struc(%d,%d,%d,%d) cont(%d,%d,%d,%d) visRgn(%d,%d,%d,%d) upd(%d,%d,%d,%d)",
              w, rd8(w + WIN_VISIBLE), rd8(w + WIN_HILITED), rds16(w + WIN_KIND), wi ? wi->procid : -1, wi && wi->native ? " native" : "",
              pr.top, pr.left, pr.bottom, pr.right, sf.bounds.top, sf.bounds.left,
              s.top, s.left, s.bottom, s.right, c.top, c.left, c.bottom, c.right, v.top, v.left, v.bottom, v.right,
              u.top, u.left, u.bottom, u.right);
    }
}
