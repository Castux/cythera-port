/* List Manager.
 *
 * ListRec, cell data and the cellArray (offsets + selection bit) use the
 * documented in-memory layout. Cells are drawn by the LDEF: the app's
 * LDEF 128 is a trampoline to a proc stored in the list's refCon (on
 * lInitMsg it clears refCon), which we reproduce natively; otherwise a
 * native text LDEF draws. */
#include "wm.h"
#include "resources.h"
#include "misc.h"

#define LR_RVIEW   0
#define LR_PORT    8
#define LR_INDENT  12
#define LR_CELLSZ  16
#define LR_VISIBLE 20
#define LR_VSCROLL 28
#define LR_HSCROLL 32
#define LR_SELFLAGS 36
#define LR_ACTIVE  37
#define LR_FLAGS   39
#define LR_CLIKTIME 40
#define LR_CLIKLOC 44
#define LR_MOUSELOC 48
#define LR_CLIKLOOP 52
#define LR_LASTCLICK 56
#define LR_REFCON  60
#define LR_DEFPROC 64
#define LR_USERH   68
#define LR_DATABNDS 72
#define LR_CELLS   80
#define LR_MAXINDEX 84
#define LR_CELLARRAY 86

enum { lInitMsg = 0, lDrawMsg, lHiliteMsg, lCloseMsg };
u32 ctl_new(u32 win, Rect r, const u8 *title, bool vis, s16 val, s16 mn, s16 mx, s16 procid, u32 refcon);

typedef struct { u32 list; bool trampoline; bool draw; } LInfo;
#define MAX_LIST 128
static LInfo g_li[MAX_LIST];
static LInfo *linfo(u32 l) { for (int i = 0; i < MAX_LIST; i++) if (g_li[i].list == l) return &g_li[i]; return NULL; }

static u32 LP(u32 l) { return hderef(l); }
static Rect bounds_of(u32 l) { return rd_rect(LP(l) + LR_DATABNDS); }
static int ncols(u32 l) { Rect b = bounds_of(l); return b.right - b.left; }
static int nrows(u32 l) { Rect b = bounds_of(l); return b.bottom - b.top; }
static int ncells(u32 l) { return ncols(l) * nrows(l); }
static int cell_index(u32 l, Point c) { Rect b = bounds_of(l); return (c.v - b.top) * ncols(l) + (c.h - b.left); }
static bool cell_valid(u32 l, Point c) { Rect b = bounds_of(l); return c.h >= b.left && c.h < b.right && c.v >= b.top && c.v < b.bottom; }
static u16 carr(u32 l, int i) { return rd16(LP(l) + LR_CELLARRAY + 2 * (u32)i); }
static void set_carr(u32 l, int i, u16 v) { wr16(LP(l) + LR_CELLARRAY + 2 * (u32)i, v); }
static int cell_off(u32 l, int i) { return carr(l, i) & 0x7FFF; }
static int cell_len(u32 l, int i) { return cell_off(l, i + 1) - cell_off(l, i); }
static bool cell_sel(u32 l, int i) { return (carr(l, i) & 0x8000) != 0; }

static void ensure_size(u32 l, int cells) {
    u32 need = LR_CELLARRAY + 2 * (u32)(cells + 1);
    if (mm_handle_size(l) < need) mm_set_handle_size(l, need);
    wr16(LP(l) + LR_MAXINDEX, (u16)(2 * cells));
}

static Rect cell_rect(u32 l, Point c) {
    u32 p = LP(l);
    Rect rv = rd_rect(p + LR_RVIEW);
    Rect vis = rd_rect(p + LR_VISIBLE);
    Point cs = rd_point(p + LR_CELLSZ);
    int x = rv.left + (c.h - vis.left) * cs.h, y = rv.top + (c.v - vis.top) * cs.v;
    return mkrect(y, x, y + cs.v, x + cs.h);
}

static void call_ldef(u32 l, int msg, bool sel, Rect r, Point c, int off, int len) {
    LInfo *li = linfo(l);
    u32 p = LP(l);
    if (li && li->trampoline) {
        if (msg == lInitMsg) { wr32(p + LR_REFCON, 0); return; }
        u32 proc = rd32(p + LR_REFCON);
        if (!proc) return;
        u32 rp = sys_alloc(8);
        wr_rect(rp, r);
        u32 a[7] = { (u32)msg, sel, rp, pt_to_u32(c), (u32)off, (u32)len, l };
        call_upp(proc, 7, a);
        mm_dispose_ptr(rp);
        return;
    }
    /* native text LDEF */
    u32 port = rd32(p + LR_PORT);
    if (msg == lDrawMsg) {
        Paint bk; paint_back(&bk, port);
        draw_rect(port, r, &bk, patCopy);
        if (len > 0) {
            u8 buf[256]; int n = len > 255 ? 255 : len;
            gmemcpy_from(buf, hderef(rd32(p + LR_CELLS)) + (u32)off, (u32)n);
            Point ind = rd_point(p + LR_INDENT);
            int a, d, w, lead; text_font_info(port, &a, &d, &w, &lead);
            wr16(port + PORT_PNLOC + 2, (u16)(r.left + (ind.h ? ind.h : 5)));
            wr16(port + PORT_PNLOC, (u16)(r.top + (ind.v ? ind.v : a)));
            text_draw(port, buf, n);
        }
        if (sel) { HRgn h; hrgn_rect(&h, r.top, r.left, r.bottom, r.right); qd_invert_hrgn(port, &h); hrgn_free(&h); }
    } else if (msg == lHiliteMsg) {
        HRgn h; hrgn_rect(&h, r.top, r.left, r.bottom, r.right); qd_invert_hrgn(port, &h); hrgn_free(&h);
    }
}

static void clip_to_view(u32 l, u32 *saved) {
    u32 p = LP(l);
    u32 port = rd32(p + LR_PORT);
    u32 pc = rd32(port + PORT_CLIP);
    *saved = rgn_new();
    mm_set_handle_size(*saved, mm_handle_size(pc));
    gmemmove(hderef(*saved), hderef(pc), mm_handle_size(pc));
    Rect rv = rd_rect(p + LR_RVIEW);
    HRgn a, b, o = { 0, NULL };
    hrgn_from_guest(&a, pc);
    hrgn_rect(&b, rv.top, rv.left, rv.bottom, rv.right);
    hrgn_op(&o, &a, &b, 1);
    hrgn_to_guest(&o, pc);
    hrgn_free(&a); hrgn_free(&b); hrgn_free(&o);
}
static void restore_clip(u32 l, u32 saved) {
    u32 port = rd32(LP(l) + LR_PORT);
    u32 pc = rd32(port + PORT_CLIP);
    mm_set_handle_size(pc, mm_handle_size(saved));
    gmemmove(hderef(pc), hderef(saved), mm_handle_size(saved));
    rgn_dispose(saved);
}

static void draw_cell(u32 l, Point c) {
    LInfo *li = linfo(l);
    if (!li || !li->draw || !cell_valid(l, c)) return;
    u32 p = LP(l);
    Rect vis = rd_rect(p + LR_VISIBLE);
    if (c.h < vis.left || c.h >= vis.right || c.v < vis.top || c.v >= vis.bottom) return;
    u32 port = rd32(p + LR_PORT);
    u32 save = qd_port();
    qd_set_port(port);
    u32 sc; clip_to_view(l, &sc);
    int i = cell_index(l, c);
    call_ldef(l, lDrawMsg, cell_sel(l, i), cell_rect(l, c), c, cell_off(l, i), cell_len(l, i));
    restore_clip(l, sc);
    qd_set_port(save);
}

static void draw_all(u32 l) {
    LInfo *li = linfo(l);
    if (!li || !li->draw) return;
    u32 p = LP(l);
    Rect vis = rd_rect(p + LR_VISIBLE);
    u32 port = rd32(p + LR_PORT);
    u32 save = qd_port();
    qd_set_port(port);
    u32 sc; clip_to_view(l, &sc);
    Paint bk; paint_back(&bk, port);
    draw_rect(port, rd_rect(p + LR_RVIEW), &bk, patCopy);
    for (int v = vis.top; v < vis.bottom; v++)
        for (int h = vis.left; h < vis.right; h++) {
            Point c = { (s16)v, (s16)h };
            if (!cell_valid(l, c)) continue;
            int i = cell_index(l, c);
            call_ldef(l, lDrawMsg, cell_sel(l, i), cell_rect(l, c), c, cell_off(l, i), cell_len(l, i));
        }
    restore_clip(l, sc);
    qd_set_port(save);
}

static void update_scroll(u32 l) {
    u32 p = LP(l);
    Rect vis = rd_rect(p + LR_VISIBLE), b = bounds_of(l);
    u32 vs = rd32(p + LR_VSCROLL), hs = rd32(p + LR_HSCROLL);
    if (vs) {
        int mx = (b.bottom - b.top) - (vis.bottom - vis.top);
        if (mx < 0) mx = 0;
        wr16(hderef(vs) + 22, (u16)mx);
        wr16(hderef(vs) + 18, (u16)(vis.top - b.top));
        wr8(hderef(vs) + 17, mx > 0 ? 0 : 255);
    }
    if (hs) {
        int mx = (b.right - b.left) - (vis.right - vis.left);
        if (mx < 0) mx = 0;
        wr16(hderef(hs) + 22, (u16)mx);
        wr16(hderef(hs) + 18, (u16)(vis.left - b.left));
        wr8(hderef(hs) + 17, mx > 0 ? 0 : 255);
    }
}

static void calc_visible(u32 l) {
    u32 p = LP(l);
    Rect rv = rd_rect(p + LR_RVIEW);
    Point cs = rd_point(p + LR_CELLSZ);
    Rect vis = rd_rect(p + LR_VISIBLE);
    int rows = cs.v > 0 ? (rv.bottom - rv.top + cs.v - 1) / cs.v : 0;
    int cols = cs.h > 0 ? (rv.right - rv.left + cs.h - 1) / cs.h : 0;
    vis.bottom = (s16)(vis.top + rows);
    vis.right = (s16)(vis.left + cols);
    wr_rect(p + LR_VISIBLE, vis);
    update_scroll(l);
}

TRAP(LNew) {
    Rect rv = rd_rect(ARG(0)); Rect db = rd_rect(ARG(1)); Point cs = pt_from_u32(ARG(2));
    s16 proc = ARGS16(3); u32 win = ARG(4); bool draw = ARGB(5), grow = ARGB(6), sh = ARGB(7), sv = ARGB(8);
    (void)grow;
    int cells = (db.right - db.left) * (db.bottom - db.top);
    if (cells < 0) cells = 0;
    u32 l = mm_new_handle(LR_CELLARRAY + 2 * (u32)(cells + 1), true, ZONE_APP);
    u32 p = LP(l);
    u32 save = qd_port();
    qd_set_port(win);
    if (cs.v <= 0) { int a, d, w, lead; text_font_info(win, &a, &d, &w, &lead); cs.v = (s16)(a + d + lead); }
    if (cs.h <= 0) cs.h = (s16)((rv.right - rv.left) / ((db.right - db.left) > 0 ? (db.right - db.left) : 1));
    wr_rect(p + LR_RVIEW, rv);
    wr32(p + LR_PORT, win);
    wr_point(p + LR_CELLSZ, cs);
    wr_rect(p + LR_VISIBLE, mkrect(db.top, db.left, db.top, db.left));
    wr_rect(p + LR_DATABNDS, db);
    wr32(p + LR_CELLS, mm_new_handle(0, false, ZONE_APP));
    wr16(p + LR_MAXINDEX, (u16)(2 * cells));
    wr8(p + LR_ACTIVE, 1);
    wr32(p + LR_LASTCLICK, 0xFFFFFFFF);
    u32 defh = res_get(FOURCC('L','D','E','F'), proc);
    wr32(p + LR_DEFPROC, defh);
    LInfo *li = NULL;
    for (int i = 0; i < MAX_LIST; i++) if (!g_li[i].list) { li = &g_li[i]; break; }
    if (!li) fatal("too many lists");
    li->list = l; li->draw = draw;
    li->trampoline = defh && hderef(defh) && mm_handle_size(defh) >= 8 && rd32(hderef(defh) + 4) == FOURCC('L','D','E','F');
    u8 notitle[1] = { 0 };
    if (sv) {
        u32 c = ctl_new(win, mkrect(rv.top - 1, rv.right, rv.bottom + 1, rv.right + 16), notitle, draw, 0, 0, 0, 16, 0);
        wr32(LP(l) + LR_VSCROLL, c);
    }
    if (sh) {
        u32 c = ctl_new(win, mkrect(rv.bottom, rv.left - 1, rv.bottom + 16, rv.right + 1), notitle, draw, 0, 0, 0, 16, 0);
        wr32(LP(l) + LR_HSCROLL, c);
    }
    calc_visible(l);
    call_ldef(l, lInitMsg, false, rv, (Point){ 0, 0 }, 0, 0);
    qd_set_port(save);
    RET(l);
}

TRAP(LDispose) {
    u32 l = ARG(0);
    if (!l) return;
    LInfo *li = linfo(l);
    call_ldef(l, lCloseMsg, false, (Rect){ 0, 0, 0, 0 }, (Point){ 0, 0 }, 0, 0);
    extern void trap_DisposeControl(CPU *);
    CPU f;
    u32 vs = rd32(LP(l) + LR_VSCROLL), hs = rd32(LP(l) + LR_HSCROLL);
    if (vs) { memset(&f, 0, sizeof f); f.r[3] = vs; trap_DisposeControl(&f); }
    if (hs) { memset(&f, 0, sizeof f); f.r[3] = hs; trap_DisposeControl(&f); }
    mm_dispose_handle(rd32(LP(l) + LR_CELLS));
    mm_dispose_handle(l);
    if (li) li->list = 0;
}

TRAP(LSetDrawingMode) {
    bool d = ARGB(0); u32 l = ARG(1);
    LInfo *li = linfo(l);
    if (li) li->draw = d;
    if (d) {
        u32 vs = rd32(LP(l) + LR_VSCROLL), hs = rd32(LP(l) + LR_HSCROLL);
        if (vs) wr8(hderef(vs) + 16, 1);
        if (hs) wr8(hderef(hs) + 16, 1);
    }
}

/* Insert `count` rows (or columns) at `at`; returns first new index. */
static int add_lines(u32 l, int count, int at, bool rows) {
    Rect b = bounds_of(l);
    int cols = b.right - b.left, nrow = b.bottom - b.top;
    if (count <= 0) return rows ? b.bottom : b.right;
    if (rows) {
        if (at < b.top) at = b.top;
        if (at > b.bottom) at = b.bottom;
        int old = cols * nrow;
        ensure_size(l, old + count * cols);
        int ins = (at - b.top) * cols;
        int off = cell_off(l, ins < old ? ins : old);
        if (ins > old) off = cell_off(l, old);
        /* shift cellArray entries (including terminator) */
        for (int i = old; i >= ins; i--) set_carr(l, i + count * cols, carr(l, i));
        for (int i = 0; i < count * cols; i++) set_carr(l, ins + i, (u16)off);
        b.bottom = (s16)(b.bottom + count);
        wr_rect(LP(l) + LR_DATABNDS, b);
    } else {
        if (at < b.left) at = b.left;
        if (at > b.right) at = b.right;
        int ncol = cols + count;
        int old = cols * nrow;
        ensure_size(l, ncol * nrow);
        /* rebuild cellArray: new columns have empty data */
        u16 *tmp = malloc(sizeof(u16) * (size_t)(old + 1));
        for (int i = 0; i <= old; i++) tmp[i] = carr(l, i);
        int k = 0;
        for (int r = 0; r < nrow; r++) {
            for (int c = 0; c < ncol; c++) {
                int oc = c < at - b.left ? c : c - count;
                bool isnew = c >= at - b.left && c < at - b.left + count;
                int src = r * cols + (isnew ? (at - b.left < cols ? at - b.left : cols) : oc);
                if (isnew) { int sidx = r * cols + (at - b.left); if (sidx > old) sidx = old; set_carr(l, k++, tmp[sidx] & 0x7FFF); }
                else set_carr(l, k++, tmp[src]);
            }
        }
        set_carr(l, k, tmp[old]);
        free(tmp);
        b.right = (s16)(b.right + count);
        wr_rect(LP(l) + LR_DATABNDS, b);
    }
    calc_visible(l);
    return at;
}

TRAP(LAddRow) { s16 count = ARGS16(0), row = ARGS16(1); u32 l = ARG(2); RET(add_lines(l, count, row, true)); draw_all(l); }
TRAP(LAddColumn) { s16 count = ARGS16(0), col = ARGS16(1); u32 l = ARG(2); RET(add_lines(l, count, col, false)); draw_all(l); }

static void del_data(u32 l, int from_cell, int to_cell) {
    /* remove data of cells [from,to) and entries */
    int n = ncells(l);
    int o0 = cell_off(l, from_cell), o1 = cell_off(l, to_cell);
    u32 cells = rd32(LP(l) + LR_CELLS);
    u32 total = mm_handle_size(cells);
    gmemmove(hderef(cells) + (u32)o0, hderef(cells) + (u32)o1, total - (u32)o1);
    mm_set_handle_size(cells, total - (u32)(o1 - o0));
    int d = to_cell - from_cell;
    for (int i = from_cell; i + d <= n; i++) {
        u16 v = carr(l, i + d);
        set_carr(l, i, (u16)((v & 0x8000) | ((v & 0x7FFF) - (o1 - o0))));
    }
}

TRAP(LDelRow) {
    s16 count = ARGS16(0), row = ARGS16(1); u32 l = ARG(2);
    Rect b = bounds_of(l);
    int cols = b.right - b.left;
    if (count == 0) { row = b.top; count = (s16)(b.bottom - b.top); }
    if (row < b.top || row >= b.bottom) return;
    if (row + count > b.bottom) count = (s16)(b.bottom - row);
    int from = (row - b.top) * cols, to = from + count * cols;
    del_data(l, from, to);
    b.bottom = (s16)(b.bottom - count);
    wr_rect(LP(l) + LR_DATABNDS, b);
    wr16(LP(l) + LR_MAXINDEX, (u16)(2 * ncells(l)));
    Rect vis = rd_rect(LP(l) + LR_VISIBLE);
    int rows = vis.bottom - vis.top;
    if (vis.top > b.top && vis.top + rows > b.bottom) { vis.top = (s16)(b.bottom - rows < b.top ? b.top : b.bottom - rows); wr_rect(LP(l) + LR_VISIBLE, vis); }
    calc_visible(l);
    draw_all(l);
}

TRAP(LDelColumn) {
    s16 count = ARGS16(0), col = ARGS16(1); u32 l = ARG(2);
    Rect b = bounds_of(l);
    if (count == 0) { col = b.left; count = (s16)(b.right - b.left); }
    int rows = b.bottom - b.top;
    for (int r = rows - 1; r >= 0; r--) {
        int cols = b.right - b.left;
        int from = r * cols + (col - b.left);
        del_data(l, from, from + count);
    }
    b.right = (s16)(b.right - count);
    wr_rect(LP(l) + LR_DATABNDS, b);
    wr16(LP(l) + LR_MAXINDEX, (u16)(2 * ncells(l)));
    calc_visible(l);
    draw_all(l);
}

static void set_cell_data(u32 l, int i, const u8 *data, int len, bool append) {
    int n = ncells(l);
    u32 cells = rd32(LP(l) + LR_CELLS);
    int o0 = cell_off(l, i), o1 = cell_off(l, i + 1);
    int old = o1 - o0;
    int nl = append ? old + len : len;
    int d = nl - old;
    u32 total = mm_handle_size(cells);
    if (d > 0) mm_set_handle_size(cells, total + (u32)d);
    u32 base = hderef(cells);
    gmemmove(base + (u32)(o1 + d), base + (u32)o1, total - (u32)o1);
    if (append) gmemcpy_to(base + (u32)o1, data, (u32)len);
    else if (len) gmemcpy_to(base + (u32)o0, data, (u32)len);
    if (d < 0) mm_set_handle_size(cells, total + (u32)d);
    for (int k = i + 1; k <= n; k++) {
        u16 v = carr(l, k);
        set_carr(l, k, (u16)((v & 0x8000) | ((v & 0x7FFF) + d)));
    }
}

TRAP(LSetCell) {
    u32 data = ARG(0); s16 len = ARGS16(1); Point c = pt_from_u32(ARG(2)); u32 l = ARG(3);
    if (!cell_valid(l, c)) return;
    u8 *buf = malloc((size_t)(len > 0 ? len : 1));
    if (len > 0) gmemcpy_from(buf, data, (u32)len);
    set_cell_data(l, cell_index(l, c), buf, len > 0 ? len : 0, false);
    free(buf);
    draw_cell(l, c);
}
TRAP(LAddToCell) {
    u32 data = ARG(0); s16 len = ARGS16(1); Point c = pt_from_u32(ARG(2)); u32 l = ARG(3);
    if (!cell_valid(l, c) || len <= 0) return;
    u8 *buf = malloc((size_t)len);
    gmemcpy_from(buf, data, (u32)len);
    set_cell_data(l, cell_index(l, c), buf, len, true);
    free(buf);
    draw_cell(l, c);
}
TRAP(LClrCell) {
    Point c = pt_from_u32(ARG(0)); u32 l = ARG(1);
    if (!cell_valid(l, c)) return;
    set_cell_data(l, cell_index(l, c), NULL, 0, false);
    draw_cell(l, c);
}
TRAP(LGetCell) {
    u32 data = ARG(0), lenp = ARG(1); Point c = pt_from_u32(ARG(2)); u32 l = ARG(3);
    if (!cell_valid(l, c)) { wr16(lenp, 0); return; }
    int i = cell_index(l, c);
    int n = cell_len(l, i), want = rds16(lenp);
    if (n > want) n = want;
    if (n > 0) gmemmove(data, hderef(rd32(LP(l) + LR_CELLS)) + (u32)cell_off(l, i), (u32)n);
    wr16(lenp, (u16)n);
}
TRAP(LGetCellDataLocation) {
    u32 offp = ARG(0), lenp = ARG(1); Point c = pt_from_u32(ARG(2)); u32 l = ARG(3);
    if (!cell_valid(l, c)) { wr16(offp, (u16)-1); wr16(lenp, (u16)-1); return; }
    int i = cell_index(l, c);
    wr16(offp, (u16)cell_off(l, i));
    wr16(lenp, (u16)cell_len(l, i));
}

static void hilite_cell(u32 l, Point c, bool sel) {
    int i = cell_index(l, c);
    bool old = cell_sel(l, i);
    if (old == sel) return;
    u16 v = carr(l, i);
    set_carr(l, i, sel ? (v | 0x8000) : (v & 0x7FFF));
    LInfo *li = linfo(l);
    if (!li || !li->draw) return;
    Rect vis = rd_rect(LP(l) + LR_VISIBLE);
    if (c.h < vis.left || c.h >= vis.right || c.v < vis.top || c.v >= vis.bottom) return;
    u32 port = rd32(LP(l) + LR_PORT);
    u32 save = qd_port();
    qd_set_port(port);
    u32 sc; clip_to_view(l, &sc);
    call_ldef(l, lHiliteMsg, sel, cell_rect(l, c), c, cell_off(l, i), cell_len(l, i));
    restore_clip(l, sc);
    qd_set_port(save);
}

TRAP(LSetSelect) {
    bool set = ARGB(0); Point c = pt_from_u32(ARG(1)); u32 l = ARG(2);
    if (!cell_valid(l, c)) return;
    hilite_cell(l, c, set);
}

TRAP(LGetSelect) {
    bool next = ARGB(0); u32 cp = ARG(1); u32 l = ARG(2);
    Point c = rd_point(cp);
    Rect b = bounds_of(l);
    if (!next) { RET(cell_valid(l, c) && cell_sel(l, cell_index(l, c))); return; }
    if (c.v < b.top) { c.v = b.top; c.h = b.left; }
    if (c.h < b.left) c.h = b.left;
    for (int v = c.v; v < b.bottom; v++) {
        for (int h = (v == c.v ? c.h : b.left); h < b.right; h++) {
            Point q = { (s16)v, (s16)h };
            if (cell_sel(l, cell_index(l, q))) { wr_point(cp, q); RET(1); return; }
        }
    }
    RET(0);
}

static void scroll_to(u32 l, int top, int left) {
    u32 p = LP(l);
    Rect vis = rd_rect(p + LR_VISIBLE), b = bounds_of(l);
    int rows = vis.bottom - vis.top, cols = vis.right - vis.left;
    if (top > b.bottom - rows) top = b.bottom - rows;
    if (top < b.top) top = b.top;
    if (left > b.right - cols) left = b.right - cols;
    if (left < b.left) left = b.left;
    if (top == vis.top && left == vis.left) return;
    vis = mkrect(top, left, top + rows, left + cols);
    wr_rect(p + LR_VISIBLE, vis);
    update_scroll(l);
    extern void ctl_draw_one(u32 c);
    if (rd32(p + LR_VSCROLL)) ctl_draw_one(rd32(p + LR_VSCROLL));
    if (rd32(p + LR_HSCROLL)) ctl_draw_one(rd32(p + LR_HSCROLL));
    draw_all(l);
}

TRAP(LScroll) {
    s16 dc = ARGS16(0), dr = ARGS16(1); u32 l = ARG(2);
    Rect vis = rd_rect(LP(l) + LR_VISIBLE);
    scroll_to(l, vis.top + dr, vis.left + dc);
}

TRAP(LAutoScroll) {
    u32 l = ARG(0);
    Rect b = bounds_of(l), vis = rd_rect(LP(l) + LR_VISIBLE);
    for (int v = b.top; v < b.bottom; v++)
        for (int h = b.left; h < b.right; h++)
            if (cell_sel(l, cell_index(l, (Point){ (s16)v, (s16)h }))) {
                if (v < vis.top || v >= vis.bottom || h < vis.left || h >= vis.right) scroll_to(l, v, h);
                return;
            }
}

TRAP(LUpdate) {
    u32 l = ARG(1);
    draw_all(l);
    extern void ctl_draw_one(u32 c);
    if (rd32(LP(l) + LR_VSCROLL)) ctl_draw_one(rd32(LP(l) + LR_VSCROLL));
    if (rd32(LP(l) + LR_HSCROLL)) ctl_draw_one(rd32(LP(l) + LR_HSCROLL));
}
TRAP(LDraw) { draw_cell(ARG(1), pt_from_u32(ARG(0))); }
TRAP(LActivate) { wr8(LP(ARG(1)) + LR_ACTIVE, ARGB(0)); }

TRAP(LSize) {
    s16 w = ARGS16(0), h = ARGS16(1); u32 l = ARG(2);
    u32 p = LP(l);
    Rect rv = rd_rect(p + LR_RVIEW);
    rv.right = (s16)(rv.left + w); rv.bottom = (s16)(rv.top + h);
    wr_rect(p + LR_RVIEW, rv);
    extern void ctl_place(u32 c, Rect r);
    if (rd32(p + LR_VSCROLL)) ctl_place(rd32(p + LR_VSCROLL), mkrect(rv.top - 1, rv.right, rv.bottom + 1, rv.right + 16));
    if (rd32(p + LR_HSCROLL)) ctl_place(rd32(p + LR_HSCROLL), mkrect(rv.bottom, rv.left - 1, rv.bottom + 16, rv.right + 1));
    calc_visible(l);
}

TRAP(LRect) {
    u32 rp = ARG(0); Point c = pt_from_u32(ARG(1)); u32 l = ARG(2);
    Rect vis = rd_rect(LP(l) + LR_VISIBLE);
    if (c.h < vis.left || c.h >= vis.right || c.v < vis.top || c.v >= vis.bottom) wr_rect(rp, (Rect){ 0, 0, 0, 0 });
    else wr_rect(rp, cell_rect(l, c));
}

TRAP(LSearch) {
    u32 data = ARG(0); s16 len = ARGS16(1); u32 proc = ARG(2), cp = ARG(3); u32 l = ARG(4);
    Point c = rd_point(cp);
    Rect b = bounds_of(l);
    if (c.v < b.top) c.v = b.top;
    if (c.h < b.left) c.h = b.left;
    u8 key[256]; int kl = len > 255 ? 255 : len;
    if (kl > 0) gmemcpy_from(key, data, (u32)kl);
    for (int v = c.v; v < b.bottom; v++)
        for (int h = (v == c.v ? c.h : b.left); h < b.right; h++) {
            Point q = { (s16)v, (s16)h };
            int i = cell_index(l, q);
            u32 cd = hderef(rd32(LP(l) + LR_CELLS)) + (u32)cell_off(l, i);
            int cl = cell_len(l, i);
            bool match;
            if (proc) {
                u32 a[4] = { data, cd, (u32)len, (u32)cl };
                match = (s16)call_upp(proc, 4, a) == 0;
            } else {
                match = cl == kl;
                for (int k = 0; k < cl && match; k++) {
                    u8 x = rd8(cd + (u32)k), y = key[k];
                    if (x >= 'a' && x <= 'z') x = (u8)(x - 32);
                    if (y >= 'a' && y <= 'z') y = (u8)(y - 32);
                    match = x == y;
                }
            }
            if (match) { wr_point(cp, q); RET(1); return; }
        }
    RET(0);
}

/* LClick(pt (local), modifiers, list) -> double click */
TRAP(LClick) {
    Point pt = pt_from_u32(ARG(0)); u16 mods = ARGU16(1); u32 l = ARG(2);
    u32 p = LP(l);
    u32 port = rd32(p + LR_PORT);
    Rect rv = rd_rect(p + LR_RVIEW);
    /* scroll bars */
    u32 sbs[2] = { rd32(p + LR_VSCROLL), rd32(p + LR_HSCROLL) };
    for (int k = 0; k < 2; k++) {
        u32 sb = sbs[k];
        if (!sb) continue;
        Rect r = rd_rect(hderef(sb) + 8);
        if (pt.h >= r.left && pt.h < r.right && pt.v >= r.top && pt.v < r.bottom) {
            extern u32 ctl_track_scroll(u32 c, Point pt);
            u32 val = ctl_track_scroll(sb, pt);
            Rect vis = rd_rect(LP(l) + LR_VISIBLE), b = bounds_of(l);
            if (k == 0) scroll_to(l, b.top + (s16)val, vis.left);
            else scroll_to(l, vis.top, b.left + (s16)val);
            RET(0);
            return;
        }
    }
    if (pt.h < rv.left || pt.h >= rv.right || pt.v < rv.top || pt.v >= rv.bottom) { RET(0); return; }
    Point cs = rd_point(p + LR_CELLSZ);
    Rect vis = rd_rect(p + LR_VISIBLE);
    u8 flags = rd8(p + LR_SELFLAGS);
    bool only_one = flags & 0x80;
    Point cell = { (s16)(vis.top + (pt.v - rv.top) / cs.v), (s16)(vis.left + (pt.h - rv.left) / cs.h) };
    bool valid = cell_valid(l, cell);
    u32 now = tick_count();
    Point last = rd_point(p + LR_LASTCLICK);
    bool dbl = valid && last.h == cell.h && last.v == cell.v && now - rd32(p + LR_CLIKTIME) < rd32(0x02F0);
    bool shift = mods & 0x0200, cmd = mods & 0x0100;
    if (valid) {
        Rect b = bounds_of(l);
        if (only_one || (!shift && !cmd)) {
            for (int v = b.top; v < b.bottom; v++)
                for (int h = b.left; h < b.right; h++) {
                    Point q = { (s16)v, (s16)h };
                    if ((q.h != cell.h || q.v != cell.v) && cell_sel(l, cell_index(l, q))) hilite_cell(l, q, false);
                }
            hilite_cell(l, cell, true);
        } else if (cmd) {
            hilite_cell(l, cell, !cell_sel(l, cell_index(l, cell)));
        } else {
            hilite_cell(l, cell, true);
        }
        /* drag selection within the list (single selection follows the mouse) */
        Point cur = cell;
        u32 save = qd_port();
        qd_set_port(port);
        while (ev_mouse_button()) {
            Point m = ev_mouse_global();
            Surf s; surf_from_port(port, &s);
            Point lm = { (s16)(m.v + s.bounds.top), (s16)(m.h + s.bounds.left) };
            if (lm.v >= rv.top && lm.v < rv.bottom && lm.h >= rv.left && lm.h < rv.right) {
                Point nc = { (s16)(vis.top + (lm.v - rv.top) / cs.v), (s16)(vis.left + (lm.h - rv.left) / cs.h) };
                if (cell_valid(l, nc) && (nc.h != cur.h || nc.v != cur.v) && (only_one || (!shift && !cmd))) {
                    hilite_cell(l, cur, false);
                    hilite_cell(l, nc, true);
                    cur = nc;
                }
            }
            ev_idle_frame();
        }
        qd_set_port(save);
        cell = cur;
    }
    p = LP(l);
    wr32(p + LR_CLIKTIME, now);
    wr_point(p + LR_LASTCLICK, valid ? cell : (Point){ -1, -1 });
    wr_point(p + LR_CLIKLOC, pt);
    RET(dbl);
}

TRAP(LLastClick) { RET(rd32(LP(ARG(0)) + LR_LASTCLICK)); }
TRAP(LCellSize) {
    Point cs = pt_from_u32(ARG(0)); u32 l = ARG(1);
    wr_point(LP(l) + LR_CELLSZ, cs);
    calc_visible(l);
}
