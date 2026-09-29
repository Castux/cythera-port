/* Control Manager.
 *
 * ControlRecords live in guest memory. Controls are drawn/hit-tested by
 * their CDEF: the application's own (reached through patched JMP stubs) or
 * a native implementation of the standard System 7 buttons, check boxes,
 * radio buttons and scroll bars. */
#include "wm.h"
#include "resources.h"

#define CR_NEXT    0
#define CR_OWNER   4
#define CR_RECT    8
#define CR_VIS     16
#define CR_HILITE  17
#define CR_VALUE   18
#define CR_MIN     20
#define CR_MAX     22
#define CR_DEFPROC 24
#define CR_DATA    28
#define CR_ACTION  32
#define CR_REFCON  36
#define CR_TITLE   40
#define CR_SIZE    296

enum { drawCntl = 0, testCntl, calcCRgns, initCntl, dispCntl, posCntl, thumbCntl, dragCntl, autoTrack,
       calcCntlRgn = 10, calcThumbRgn = 11 };
enum { inButton = 10, inCheckBox = 11, inUpButton = 20, inDownButton = 21, inPageUp = 22, inPageDown = 23, inThumb = 129 };

typedef struct { u32 ctl; s16 procid; int var; bool native; int kind; } CInfo;
#define MAX_CTL 1024
static CInfo g_ci[MAX_CTL];

static CInfo *cinfo(u32 c) { for (int i = 0; i < MAX_CTL; i++) if (g_ci[i].ctl == c) return &g_ci[i]; return NULL; }

static u32 cdef_upp(u32 c) {
    u32 h = rd32(hderef(c) + CR_DEFPROC);
    if (!h || !hderef(h)) return 0;
    u32 p = hderef(h);
    if (rd16(p) == 0x4EF9 && rd32(p + 2)) return rd32(p + 2);
    return 0;
}

/* ---- native standard CDEF ---- */
static void ctl_fill(u32 port, Rect r, RGB c) { Paint p = { .kind = 0, .fg = c }; draw_rect(port, r, &p, patCopy); }
static void ctl_frame(u32 port, Rect r, RGB c) {
    ctl_fill(port, mkrect(r.top, r.left, r.top + 1, r.right), c);
    ctl_fill(port, mkrect(r.bottom - 1, r.left, r.bottom, r.right), c);
    ctl_fill(port, mkrect(r.top, r.left, r.bottom, r.left + 1), c);
    ctl_fill(port, mkrect(r.top, r.right - 1, r.bottom, r.right), c);
}

static void draw_title(u32 port, u32 c, int x, int y, bool dim, bool inverse) {
    u32 p = hderef(c);
    u8 n = rd8(p + CR_TITLE);
    u8 buf[256]; gmemcpy_from(buf, p + CR_TITLE + 1, n);
    RGB save; rd_rgb(port + PORT_RGBFG, &save);
    RGB col = inverse ? (RGB){ 0xFFFF, 0xFFFF, 0xFFFF } : dim ? (RGB){ 0x8888, 0x8888, 0x8888 } : (RGB){ 0, 0, 0 };
    if (is_color_port(port)) wr_rgb(port + PORT_RGBFG, col);
    wr16(port + PORT_PNLOC + 2, (u16)x); wr16(port + PORT_PNLOC, (u16)y);
    s16 saveMode = rds16(port + PORT_TXMODE);
    wr16(port + PORT_TXMODE, srcOr);
    text_draw(port, buf, n);
    wr16(port + PORT_TXMODE, (u16)saveMode);
    if (is_color_port(port)) wr_rgb(port + PORT_RGBFG, save);
}

static int title_width(u32 port, u32 c) {
    u32 p = hderef(c);
    u8 n = rd8(p + CR_TITLE);
    return text_width(port, gptr(p + CR_TITLE + 1, n), n);
}

static void std_draw(u32 c, CInfo *ci, int part) {
    (void)part;
    u32 p = hderef(c);
    if (!rd8(p + CR_VIS)) return;
    u32 port = rd32(p + CR_OWNER);
    u32 save = qd_port();
    qd_set_port(port);
    Rect r = rd_rect(p + CR_RECT);
    u8 hil = rd8(p + CR_HILITE);
    bool dim = hil == 255;
    RGB black = { 0, 0, 0 }, white = { 0xFFFF, 0xFFFF, 0xFFFF };
    int asc, desc, wm, lead;
    text_font_info(port, &asc, &desc, &wm, &lead);
    int ty = (r.top + r.bottom + asc - desc) / 2;
    switch (ci->kind) {
    case 0: { /* push button: rounded rect */
        HRgn shape; Paint pb = { .kind = 0, .fg = white }, pk = { .kind = 0, .fg = dim ? (RGB){ 0x8888, 0x8888, 0x8888 } : black };
        extern void qd_oval_rgn(HRgn *r, Rect rc, int ow, int oh);
        qd_oval_rgn(&shape, r, 16, 16);
        bool pressed = hil && hil != 255;
        pb.fg = pressed ? black : white;
        draw_hrgn(port, &shape, &pb, patCopy);
        HRgn in; qd_oval_rgn(&in, mkrect(r.top + 1, r.left + 1, r.bottom - 1, r.right - 1), 14, 14);
        HRgn fr = { 0, NULL }; hrgn_op(&fr, &shape, &in, 2);
        draw_hrgn(port, &fr, &pk, patCopy);
        hrgn_free(&shape); hrgn_free(&in); hrgn_free(&fr);
        int tw = title_width(port, c);
        draw_title(port, c, (r.left + r.right - tw) / 2, ty, dim, pressed);
        break;
    }
    case 1: case 2: { /* check box / radio button */
        int box = 12;
        Rect b = mkrect((r.top + r.bottom - box) / 2, r.left + 2, (r.top + r.bottom - box) / 2 + box, r.left + 2 + box);
        s16 v = rds16(p + CR_VALUE);
        if (ci->kind == 1) {
            ctl_fill(port, b, white);
            ctl_frame(port, b, dim ? (RGB){ 0x8888, 0x8888, 0x8888 } : black);
            if (hil && !dim) ctl_frame(port, mkrect(b.top + 1, b.left + 1, b.bottom - 1, b.right - 1), black);
            if (v) {
                for (int i = 0; i < box - 2; i++) {
                    ctl_fill(port, mkrect(b.top + 1 + i, b.left + 1 + i, b.top + 2 + i, b.left + 2 + i), black);
                    ctl_fill(port, mkrect(b.top + 1 + i, b.right - 2 - i, b.top + 2 + i, b.right - 1 - i), black);
                }
            }
        } else {
            HRgn o; extern void qd_oval_rgn(HRgn *r, Rect rc, int ow, int oh);
            qd_oval_rgn(&o, b, box, box);
            Paint pw = { .kind = 0, .fg = white }, pk = { .kind = 0, .fg = dim ? (RGB){ 0x8888, 0x8888, 0x8888 } : black };
            draw_hrgn(port, &o, &pw, patCopy);
            HRgn in; qd_oval_rgn(&in, mkrect(b.top + 1, b.left + 1, b.bottom - 1, b.right - 1), box - 2, box - 2);
            HRgn fr = { 0, NULL }; hrgn_op(&fr, &o, &in, 2);
            draw_hrgn(port, &fr, &pk, patCopy);
            if (v) { HRgn d; qd_oval_rgn(&d, mkrect(b.top + 3, b.left + 3, b.bottom - 3, b.right - 3), box - 6, box - 6);
                draw_hrgn(port, &d, &pk, patCopy); hrgn_free(&d); }
            hrgn_free(&o); hrgn_free(&in); hrgn_free(&fr);
        }
        draw_title(port, c, b.right + 5, ty, dim, false);
        break;
    }
    case 16: { /* scroll bar */
        bool vert = (r.bottom - r.top) > (r.right - r.left);
        ctl_fill(port, r, (RGB){ 0xDDDD, 0xDDDD, 0xDDDD });
        ctl_frame(port, r, black);
        s16 mn = rds16(p + CR_MIN), mx = rds16(p + CR_MAX), v = rds16(p + CR_VALUE);
        int len = vert ? r.bottom - r.top : r.right - r.left, aw = 16;
        if (mx > mn && !dim && len > 3 * aw) {
            int track = len - 3 * aw;
            int pos = aw + (int)((s64)(v - mn) * track / (mx - mn));
            Rect th = vert ? mkrect(r.top + pos, r.left, r.top + pos + aw, r.right) : mkrect(r.top, r.left + pos, r.bottom, r.left + pos + aw);
            ctl_fill(port, th, white);
            ctl_frame(port, th, black);
        }
        Rect a1 = vert ? mkrect(r.top, r.left, r.top + aw, r.right) : mkrect(r.top, r.left, r.bottom, r.left + aw);
        Rect a2 = vert ? mkrect(r.bottom - aw, r.left, r.bottom, r.right) : mkrect(r.top, r.right - aw, r.bottom, r.right);
        ctl_fill(port, a1, hil == inUpButton ? black : white); ctl_frame(port, a1, black);
        ctl_fill(port, a2, hil == inDownButton ? black : white); ctl_frame(port, a2, black);
        break;
    }
    }
    qd_set_port(save);
}

static int std_test(u32 c, CInfo *ci, Point pt) {
    u32 p = hderef(c);
    Rect r = rd_rect(p + CR_RECT);
    if (rd8(p + CR_HILITE) == 255) return 0;
    if (pt.h < r.left || pt.h >= r.right || pt.v < r.top || pt.v >= r.bottom) return 0;
    switch (ci->kind) {
    case 0: return inButton;
    case 1: case 2: return inCheckBox;
    case 16: {
        bool vert = (r.bottom - r.top) > (r.right - r.left);
        int pos = vert ? pt.v - r.top : pt.h - r.left;
        int len = vert ? r.bottom - r.top : r.right - r.left, aw = 16;
        if (pos < aw) return inUpButton;
        if (pos >= len - aw) return inDownButton;
        s16 mn = rds16(p + CR_MIN), mx = rds16(p + CR_MAX), v = rds16(p + CR_VALUE);
        if (mx <= mn) return 0;
        int track = len - 3 * aw;
        int tp = aw + (int)((s64)(v - mn) * track / (mx - mn));
        if (pos < tp) return inPageUp;
        if (pos >= tp + aw) return inPageDown;
        return inThumb;
    }
    }
    return 0;
}

static u32 call_cdef(u32 c, int msg, u32 param) {
    CInfo *ci = cinfo(c);
    if (!ci) return 0;
    u32 upp = ci->native ? 0 : cdef_upp(c);
    if (!upp) {
        switch (msg) {
        case drawCntl: std_draw(c, ci, (int)param); return 0;
        case testCntl: return (u32)std_test(c, ci, pt_from_u32(param));
        case calcCRgns: case calcCntlRgn: {
            Rect r = rd_rect(hderef(c) + CR_RECT);
            rgn_set_rect(param & 0x7FFFFFFF, r);
            return 0;
        }
        default: return 0;
        }
    }
    u32 a[4] = { (u32)ci->var, c, (u32)msg, param };
    u32 save = qd_port();
    u32 r = call_upp(upp, 4, a);
    qd_set_port(save);
    return r;
}

static void ctl_draw(u32 c) {
    u32 p = hderef(c);
    if (!rd8(p + CR_VIS)) return;
    u32 save = qd_port();
    qd_set_port(rd32(p + CR_OWNER));
    call_cdef(c, drawCntl, 0);
    qd_set_port(save);
}

void ctl_draw_all(u32 win) {
    for (u32 c = rd32(win + WIN_CONTROLS); c; c = rd32(hderef(c) + CR_NEXT)) ctl_draw(c);
}

void ctl_dispose_all(u32 win) {
    u32 c = rd32(win + WIN_CONTROLS);
    while (c) {
        u32 next = rd32(hderef(c) + CR_NEXT);
        call_cdef(c, dispCntl, 0);
        CInfo *ci = cinfo(c);
        if (ci) ci->ctl = 0;
        mm_dispose_handle(c);
        c = next;
    }
    wr32(win + WIN_CONTROLS, 0);
}

u32 ctl_new(u32 win, Rect r, const u8 *title, bool vis, s16 val, s16 mn, s16 mx, s16 procid, u32 refcon) {
    u32 c = mm_new_handle(CR_SIZE, true, ZONE_APP);
    u32 p = hderef(c);
    wr32(p + CR_OWNER, win);
    wr_rect(p + CR_RECT, r);
    wr8(p + CR_VIS, vis);
    wr16(p + CR_VALUE, (u16)val); wr16(p + CR_MIN, (u16)mn); wr16(p + CR_MAX, (u16)mx);
    wr32(p + CR_REFCON, refcon);
    wr8(p + CR_TITLE, title[0]);
    gmemcpy_to(p + CR_TITLE + 1, title + 1, title[0]);
    CInfo *ci = NULL;
    for (int i = 0; i < MAX_CTL; i++) if (!g_ci[i].ctl) { ci = &g_ci[i]; break; }
    if (!ci) fatal("too many controls");
    memset(ci, 0, sizeof *ci);
    ci->ctl = c; ci->procid = procid; ci->var = procid & 15;
    s16 cdefid = (s16)(procid >> 4);
    u32 defh = res_get(FOURCC('C','D','E','F'), cdefid);
    if (defh && hderef(defh) && rd16(hderef(defh)) == 0x4EF9) wr32(p + CR_DEFPROC, defh);
    else {
        ci->native = true;
        ci->kind = cdefid == 1 ? 16 : (procid & 7) == 1 ? 1 : (procid & 7) == 2 ? 2 : 0;
        wr32(p + CR_DEFPROC, defh);
    }
    /* link at head of the window's control list */
    wr32(p + CR_NEXT, rd32(win + WIN_CONTROLS));
    wr32(win + WIN_CONTROLS, c);
    call_cdef(c, initCntl, 0);
    if (vis) ctl_draw(c);
    return c;
}

TRAP(NewControl) {
    u32 win = ARG(0); Rect r = rd_rect(ARG(1)); u32 tp = ARG(2); bool vis = ARGB(3);
    s16 val = ARGS16(4), mn = ARGS16(5), mx = ARGS16(6), proc = ARGS16(7); u32 refcon = ARG(8);
    u8 title[256]; title[0] = tp ? rd8(tp) : 0;
    if (title[0]) gmemcpy_from(title + 1, tp + 1, title[0]);
    RET(ctl_new(win, r, title, vis, val, mn, mx, proc, refcon));
}

TRAP(GetNewControl) {
    s16 id = ARGS16(0); u32 win = ARG(1);
    u32 len;
    u8 *d = res_load_raw(FOURCC('C','N','T','L'), id, &len);
    if (!d) { RET(0); return; }
    Rect r = { (s16)be16(d), (s16)be16(d + 2), (s16)be16(d + 4), (s16)be16(d + 6) };
    s16 val = (s16)be16(d + 8); bool vis = be16(d + 10) != 0;
    s16 mx = (s16)be16(d + 12), mn = (s16)be16(d + 14), proc = (s16)be16(d + 16);
    u32 refcon = be32(d + 18);
    u8 title[256]; title[0] = d[22]; memcpy(title + 1, d + 23, title[0]);
    free(d);
    RET(ctl_new(win, r, title, vis, val, mn, mx, proc, refcon));
}

static void unlink_ctl(u32 c) {
    u32 win = rd32(hderef(c) + CR_OWNER);
    u32 prev = 0, x = rd32(win + WIN_CONTROLS);
    while (x && x != c) { prev = x; x = rd32(hderef(x) + CR_NEXT); }
    if (!x) return;
    u32 next = rd32(hderef(c) + CR_NEXT);
    if (prev) wr32(hderef(prev) + CR_NEXT, next); else wr32(win + WIN_CONTROLS, next);
}

static void erase_ctl(u32 c) {
    u32 p = hderef(c);
    u32 port = rd32(p + CR_OWNER);
    u32 save = qd_port();
    qd_set_port(port);
    Paint pb; paint_back(&pb, port);
    draw_rect(port, rd_rect(p + CR_RECT), &pb, patCopy);
    qd_set_port(save);
}

TRAP(DisposeControl) {
    u32 c = ARG(0);
    if (!c || !cinfo(c)) return;
    if (rd8(hderef(c) + CR_VIS)) erase_ctl(c);
    call_cdef(c, dispCntl, 0);
    unlink_ctl(c);
    cinfo(c)->ctl = 0;
    mm_dispose_handle(c);
}
TRAP(KillControls) { ctl_dispose_all(ARG(0)); }
TRAP(ShowControl) { u32 c = ARG(0); if (!rd8(hderef(c) + CR_VIS)) { wr8(hderef(c) + CR_VIS, 1); ctl_draw(c); } }
TRAP(HideControl) { u32 c = ARG(0); if (rd8(hderef(c) + CR_VIS)) { erase_ctl(c); wr8(hderef(c) + CR_VIS, 0); } }
TRAP(DrawControls) { ctl_draw_all(ARG(0)); }
TRAP(Draw1Control) { ctl_draw(ARG(0)); }
TRAP(UpdateControls) { ctl_draw_all(ARG(0)); }
TRAP(HiliteControl) {
    u32 c = ARG(0); u8 h = (u8)ARG(1);
    if (rd8(hderef(c) + CR_HILITE) == h) return;
    wr8(hderef(c) + CR_HILITE, h);
    ctl_draw(c);
}
TRAP(MoveControl) {
    u32 c = ARG(0); s16 h = ARGS16(1), v = ARGS16(2);
    u32 p = hderef(c);
    bool vis = rd8(p + CR_VIS);
    if (vis) erase_ctl(c);
    Rect r = rd_rect(p + CR_RECT);
    int w = r.right - r.left, ht = r.bottom - r.top;
    wr_rect(p + CR_RECT, mkrect(v, h, v + ht, h + w));
    if (vis) ctl_draw(c);
}
TRAP(SizeControl) {
    u32 c = ARG(0); s16 w = ARGS16(1), h = ARGS16(2);
    u32 p = hderef(c);
    bool vis = rd8(p + CR_VIS);
    if (vis) erase_ctl(c);
    Rect r = rd_rect(p + CR_RECT);
    wr_rect(p + CR_RECT, mkrect(r.top, r.left, r.top + h, r.left + w));
    if (vis) ctl_draw(c);
}

static void set_val(u32 c, s16 v) {
    u32 p = hderef(c);
    s16 mn = rds16(p + CR_MIN), mx = rds16(p + CR_MAX);
    if (v < mn) v = mn;
    if (v > mx) v = mx;
    if (rds16(p + CR_VALUE) == v) return;
    wr16(p + CR_VALUE, (u16)v);
    ctl_draw(c);
}
TRAP(SetControlValue) { set_val(ARG(0), ARGS16(1)); }
TRAP(GetControlValue) { RET((s32)rds16(hderef(ARG(0)) + CR_VALUE)); }
TRAP(SetControlMinimum) { u32 c = ARG(0); wr16(hderef(c) + CR_MIN, (u16)ARGS16(1)); set_val(c, rds16(hderef(c) + CR_VALUE)); ctl_draw(c); }
TRAP(GetControlMinimum) { RET((s32)rds16(hderef(ARG(0)) + CR_MIN)); }
TRAP(SetControlMaximum) { u32 c = ARG(0); wr16(hderef(c) + CR_MAX, (u16)ARGS16(1)); set_val(c, rds16(hderef(c) + CR_VALUE)); ctl_draw(c); }
TRAP(GetControlMaximum) { RET((s32)rds16(hderef(ARG(0)) + CR_MAX)); }
TRAP(SetControlReference) { wr32(hderef(ARG(0)) + CR_REFCON, ARG(1)); }
TRAP(GetControlReference) { RET(rd32(hderef(ARG(0)) + CR_REFCON)); }
TRAP(SetControlAction) { wr32(hderef(ARG(0)) + CR_ACTION, ARG(1)); }
TRAP(GetControlAction) { RET(rd32(hderef(ARG(0)) + CR_ACTION)); }
TRAP(SetControlTitle) {
    u32 c = ARG(0), tp = ARG(1);
    gmemmove(hderef(c) + CR_TITLE, tp, 1u + rd8(tp));
    ctl_draw(c);
}
TRAP(GetControlTitle) { u32 c = ARG(0), out = ARG(1); gmemmove(out, hderef(c) + CR_TITLE, 1u + rd8(hderef(c) + CR_TITLE)); }

static int test_ctl(u32 c, Point pt) {
    u32 p = hderef(c);
    if (!rd8(p + CR_VIS) || rd8(p + CR_HILITE) == 255) return 0;
    return (int)(s16)call_cdef(c, testCntl, pt_to_u32(pt));
}

TRAP(TestControl) { RET(test_ctl(ARG(0), pt_from_u32(ARG(1)))); }

/* FindControl(pt (local), window, &control) */
TRAP(FindControl) {
    Point pt = pt_from_u32(ARG(0)); u32 win = ARG(1), cp = ARG(2);
    wr32(cp, 0);
    if (!wm_is_window(win) || !rd8(win + WIN_VISIBLE)) { RET(0); return; }
    for (u32 c = rd32(win + WIN_CONTROLS); c; c = rd32(hderef(c) + CR_NEXT)) {
        int part = test_ctl(c, pt);
        if (part) { wr32(cp, c); RET(part); return; }
    }
    RET(0);
}
TRAP(FindControlUnderMouse) {
    Point pt = pt_from_u32(ARG(0)); u32 win = ARG(1), partp = ARG(2);
    for (u32 c = rd32(win + WIN_CONTROLS); c; c = rd32(hderef(c) + CR_NEXT)) {
        int part = test_ctl(c, pt);
        if (part) { if (partp) wr16(partp, (u16)part); RET(c); return; }
    }
    if (partp) wr16(partp, 0);
    RET(0);
}

/* TrackControl(control, startPt, actionProc) */
TRAP(TrackControl) {
    u32 c = ARG(0); Point start = pt_from_u32(ARG(1)); u32 action = ARG(2);
    int part = test_ctl(c, start);
    if (!part) { RET(0); return; }
    u32 p = hderef(c);
    u32 win = rd32(p + CR_OWNER);
    u32 save = qd_port();
    qd_set_port(win);
    if (action == 0xFFFFFFFFu) action = rd32(p + CR_ACTION);
    /* app CDEFs may do their own tracking via autoTrack/dragCntl; we use the
       generic loop, invoking the action proc for non-thumb parts */
    bool in = true;
    int cur = part;
    wr8(hderef(c) + CR_HILITE, (u8)part);
    ctl_draw(c);
    u32 last_action = 0;
    while (ev_mouse_button()) {
        Point m = ev_mouse_global();
        int x = m.h, y = m.v;
        Surf s; surf_from_port(win, &s);
        Point lm = { (s16)(y + s.bounds.top), (s16)(x + s.bounds.left) };
        int now = test_ctl(c, lm);
        bool nin = now == part || (part == inThumb && true);
        if (nin != in || now != cur) {
            in = nin; cur = now;
            wr8(hderef(c) + CR_HILITE, in ? (u8)part : 0);
            ctl_draw(c);
        }
        if (in && action && part != inThumb) {
            u32 t = rd32(0x016A);
            if (t != last_action) {
                last_action = t;
                u32 a[2] = { c, (u32)part };
                call_upp(action, 2, a);
            }
        }
        if (part == inThumb) {
            /* drag thumb: map mouse to value */
            Rect r = rd_rect(hderef(c) + CR_RECT);
            bool vert = (r.bottom - r.top) > (r.right - r.left);
            int len = vert ? r.bottom - r.top : r.right - r.left, aw = 16, track = len - 3 * aw;
            int pos = (vert ? lm.v - r.top : lm.h - r.left) - aw - aw / 2;
            s16 mn = rds16(hderef(c) + CR_MIN), mx = rds16(hderef(c) + CR_MAX);
            if (track > 0 && cinfo(c) && cinfo(c)->native) {
                int v = mn + (int)((s64)pos * (mx - mn) / track);
                set_val(c, (s16)v);
            }
        }
        ev_idle_frame();
    }
    wr8(hderef(c) + CR_HILITE, 0);
    ctl_draw(c);
    qd_set_port(save);
    if (!in) { RET(0); return; }
    /* toggle-style controls are left to the application (standard Mac behavior) */
    RET(part);
}

TRAP(DragControl) { }
TRAP(GetAuxiliaryControlRecord) { u32 hp = ARG(1); if (hp) wr32(hp, 0); RET(0); }

/* Appearance Manager control calls (Appearance is reported absent). */
TRAP(CreateRootControl) { u32 p = ARG(1); if (p) wr32(p, 0); RETERR(-30581); }
TRAP(EmbedControl) { RETERR(-30581); }
TRAP(GetControlData) { RETERR(-30581); }
TRAP(SetControlData) { RETERR(-30581); }
TRAP(HandleControlClick) { RET(0); }
TRAP(HandleControlKey) { RET(0); }
TRAP(GetKeyboardFocus) { u32 p = ARG(1); if (p) wr32(p, 0); RETERR(-30581); }
TRAP(IdleControls) { }
TRAP(ActivateControl) { u32 c = ARG(0); if (c) { wr8(hderef(c) + CR_HILITE, 0); ctl_draw(c); } RETERR(noErr); }
TRAP(DeactivateControl) { u32 c = ARG(0); if (c) { wr8(hderef(c) + CR_HILITE, 255); ctl_draw(c); } RETERR(noErr); }
TRAP(RegisterAppearanceClient) { RETERR(-30581); }

/* ---- helpers for the List/Dialog Managers ---- */
void ctl_draw_one(u32 c) { ctl_draw(c); }
void ctl_place(u32 c, Rect r) {
    u32 p = hderef(c);
    bool vis = rd8(p + CR_VIS);
    if (vis) erase_ctl(c);
    wr_rect(p + CR_RECT, r);
    if (vis) ctl_draw(c);
}
/* Track a click in a native scroll bar; returns the new value. */
u32 ctl_track_scroll(u32 c, Point pt) {
    int part = test_ctl(c, pt);
    u32 p;
    u32 win = rd32(hderef(c) + CR_OWNER);
    u32 last = 0;
    bool first = true;
    while (first || ev_mouse_button()) {
        p = hderef(c);
        s16 v = rds16(p + CR_VALUE);
        u32 t = rd32(0x016A);
        if (part == inThumb) {
            Point m = ev_mouse_global();
            Surf s; surf_from_port(win, &s);
            Point lm = { (s16)(m.v + s.bounds.top), (s16)(m.h + s.bounds.left) };
            Rect r = rd_rect(p + CR_RECT);
            bool vert = (r.bottom - r.top) > (r.right - r.left);
            int len = vert ? r.bottom - r.top : r.right - r.left, aw = 16, track = len - 3 * aw;
            int pos = (vert ? lm.v - r.top : lm.h - r.left) - aw - aw / 2;
            s16 mn = rds16(p + CR_MIN), mx = rds16(p + CR_MAX);
            if (track > 0) set_val(c, (s16)(mn + (int)((s64)pos * (mx - mn) / track)));
        } else if (first || t - last >= 4) {
            last = t;
            int d = part == inUpButton ? -1 : part == inDownButton ? 1 : part == inPageUp ? -8 : part == inPageDown ? 8 : 0;
            if (part == inUpButton || part == inDownButton) { wr8(p + CR_HILITE, (u8)part); }
            set_val(c, (s16)(v + d));
            ctl_draw(c);
        }
        first = false;
        ev_idle_frame();
    }
    wr8(hderef(c) + CR_HILITE, 0);
    ctl_draw(c);
    return (u32)(s32)rds16(hderef(c) + CR_VALUE);
}
