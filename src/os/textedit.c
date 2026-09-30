/* TextEdit.
 *
 * The TERec lives in guest memory and its public fields (rects,
 * selection, length, hText, line starts, metrics) are kept up to date.
 * Style runs of styled TE records are kept host-side. */
#include "wm.h"
#include "misc.h"
#include "mm.h"
#include "files.h"
#include "../host/host.h"

#define TE_DEST     0
#define TE_VIEW     8
#define TE_SELRECT  16
#define TE_LINEH    24
#define TE_ASCENT   26
#define TE_SELPOINT 28
#define TE_SELSTART 32
#define TE_SELEND   34
#define TE_ACTIVE   36
#define TE_WORDBRK  38
#define TE_CLIKLOOP 42
#define TE_CLICKTIME 46
#define TE_CLICKLOC 50
#define TE_CARETTIME 52
#define TE_CARETSTATE 56
#define TE_JUST     58
#define TE_LENGTH   60
#define TE_HTEXT    62
#define TE_HDISP    66
#define TE_CLIKSTUFF 70
#define TE_CRONLY   72
#define TE_FONT     74
#define TE_FACE     76
#define TE_MODE     78
#define TE_SIZE     80
#define TE_INPORT   82
#define TE_HIGHHOOK 86
#define TE_CARETHOOK 90
#define TE_NLINES   94
#define TE_LINESTARTS 96

typedef struct { s32 start; s16 font; u8 face; s16 size; RGB color; } Run;

typedef struct {
    u32 te;
    bool styled;
    Run *runs; int nruns, capruns;
    bool caret_on;
    u32 caret_tick;
} TEInfo;

#define MAX_TE 128
static TEInfo g_te[MAX_TE];
static u8 *g_scrap; static u32 g_scraplen;

static TEInfo *teinfo(u32 te) { for (int i = 0; i < MAX_TE; i++) if (g_te[i].te == te) return &g_te[i]; return NULL; }

static u32 te_rec(u32 te) { return hderef(te); }
static u32 te_text(u32 te) { return rd32(te_rec(te) + TE_HTEXT); }
static int te_len(u32 te) { return rds16(te_rec(te) + TE_LENGTH); }

/* ---- style helpers ---- */
static void run_apply(u32 port, const Run *r) {
    wr16(port + PORT_TXFONT, (u16)r->font);
    wr8(port + PORT_TXFACE, r->face);
    wr16(port + PORT_TXSIZE, (u16)r->size);
    if (is_color_port(port)) wr_rgb(port + PORT_RGBFG, r->color);
}

static Run base_run(u32 te) {
    u32 p = te_rec(te);
    TEInfo *ti = teinfo(te);
    if (ti && ti->styled && ti->nruns) return ti->runs[0];
    Run r = { 0, rds16(p + TE_FONT), rd8(p + TE_FACE), rds16(p + TE_SIZE), port_fg(rd32(p + TE_INPORT)) };
    return r;
}

static const Run *run_at(TEInfo *ti, u32 te, int pos, Run *tmp) {
    if (!ti || !ti->styled || !ti->nruns) { *tmp = base_run(te); return tmp; }
    int k = 0;
    for (int i = 0; i < ti->nruns; i++) if (ti->runs[i].start <= pos) k = i;
    return &ti->runs[k];
}

static int char_w(u32 port, TEInfo *ti, u32 te, int pos, u8 ch) {
    Run tmp; const Run *r = run_at(ti, te, pos, &tmp);
    run_apply(port, r);
    return text_width(port, &ch, 1);
}

static void metrics(u32 port, TEInfo *ti, u32 te, int from, int to, int *asc, int *height) {
    int a = 0, d = 0, l = 0;
    Run tmp;
    if (!ti || !ti->styled || !ti->nruns || from >= to) {
        const Run *r = run_at(ti, te, from, &tmp);
        run_apply(port, r);
        int wm; text_font_info(port, &a, &d, &wm, &l);
    } else {
        for (int i = 0; i < ti->nruns; i++) {
            int s = ti->runs[i].start, e = i + 1 < ti->nruns ? ti->runs[i + 1].start : 0x7FFFFFFF;
            if (e <= from || s >= to) continue;
            run_apply(port, &ti->runs[i]);
            int ra, rd, wm, rl; text_font_info(port, &ra, &rd, &wm, &rl);
            if (ra > a) a = ra;
            if (rd > d) d = rd;
            if (rl > l) l = rl;
        }
    }
    *asc = a; *height = a + d + l;
}

/* ---- line breaking ---- */
static void te_calc(u32 te) {
    u32 p = te_rec(te);
    TEInfo *ti = teinfo(te);
    u32 port = rd32(p + TE_INPORT);
    u32 save = qd_port();
    qd_set_port(port);
    u8 sf[PORT_SIZE]; gmemcpy_from(sf, port, PORT_SIZE);
    Rect dest = rd_rect(p + TE_DEST);
    int width = dest.right - dest.left;
    int n = te_len(te);
    u32 th = te_text(te);
    u8 *t = malloc((size_t)n + 1);
    if (n) gmemcpy_from(t, hderef(th), (u32)n);
    int cap = 64, nl = 0;
    s16 *starts = malloc(sizeof(s16) * (size_t)cap);
    int i = 0;
    starts[nl++] = 0;
    while (i < n) {
        int w = 0, last_space = -1, j = i;
        bool crOnly = rds16(p + TE_CRONLY) < 0;
        while (j < n) {
            if (t[j] == '\r') { j++; break; }
            int cw = char_w(port, ti, te, j, t[j]);
            if (!crOnly && w + cw > width && j > i) {
                if (t[j] == ' ') { while (j < n && t[j] == ' ') j++; break; }
                if (last_space > i) j = last_space;
                break;
            }
            w += cw;
            if (t[j] == ' ') last_space = j + 1;
            j++;
        }
        if (j <= i) j = i + 1;
        i = j;
        if (i <= n) {
            if (nl == cap) { cap *= 2; starts = realloc(starts, sizeof(s16) * (size_t)cap); }
            starts[nl++] = (s16)i;
        }
        if (i == n) break;
    }
    if (starts[nl - 1] != n) {
        if (nl == cap) { cap *= 2; starts = realloc(starts, sizeof(s16) * (size_t)cap); }
        starts[nl++] = (s16)n;
    }
    /* a trailing CR starts an empty last line */
    int nlines = nl - 1;
    if (n > 0 && t[n - 1] == '\r') nlines = nl - 1;
    if (nlines < 1) nlines = n > 0 ? 1 : 0;
    u32 need = TE_LINESTARTS + 2 * (u32)(nl + 1);
    if (mm_handle_size(te) < need) mm_set_handle_size(te, need);
    p = te_rec(te);
    wr16(p + TE_NLINES, (u16)(n == 0 ? 0 : nl - 1));
    for (int k = 0; k < nl; k++) wr16(p + TE_LINESTARTS + 2 * (u32)k, (u16)starts[k]);
    if (!ti || !ti->styled) {
        int a, h; metrics(port, ti, te, 0, n, &a, &h);
        wr16(p + TE_LINEH, (u16)h);
        wr16(p + TE_ASCENT, (u16)a);
    }
    free(starts);
    free(t);
    gmemcpy_to(port, sf, PORT_SIZE);
    qd_set_port(save);
}

static int line_of(u32 te, int pos) {
    u32 p = te_rec(te);
    int nl = rds16(p + TE_NLINES);
    for (int k = 0; k < nl; k++) {
        int s = rds16(p + TE_LINESTARTS + 2 * (u32)k), e = rds16(p + TE_LINESTARTS + 2 * (u32)(k + 1));
        if (pos >= s && pos < e) return k;
    }
    return nl > 0 ? nl - 1 : 0;
}

static int line_top(u32 te, TEInfo *ti, u32 port, int line) {
    u32 p = te_rec(te);
    int y = rd_rect(p + TE_DEST).top;
    if (!ti || !ti->styled) return y + line * rds16(p + TE_LINEH);
    for (int k = 0; k < line; k++) {
        int s = rds16(p + TE_LINESTARTS + 2 * (u32)k), e = rds16(p + TE_LINESTARTS + 2 * (u32)(k + 1));
        int a, h; metrics(port, ti, te, s, e, &a, &h);
        y += h;
    }
    return y;
}

/* x position of char offset `pos` on its line */
static int pos_x(u32 te, TEInfo *ti, u32 port, int pos) {
    u32 p = te_rec(te);
    int line = line_of(te, pos);
    int s = rds16(p + TE_LINESTARTS + 2 * (u32)line);
    int x = rd_rect(p + TE_DEST).left;
    u32 th = te_text(te);
    for (int i = s; i < pos; i++) {
        u8 c = rd8(hderef(th) + (u32)i);
        if (c == '\r') break;
        x += char_w(port, ti, te, i, c);
    }
    return x;
}

static int pos_from_point(u32 te, TEInfo *ti, u32 port, Point pt) {
    u32 p = te_rec(te);
    int nl = rds16(p + TE_NLINES);
    int n = te_len(te);
    if (nl == 0) return 0;
    int line = nl - 1;
    for (int k = 0; k < nl; k++) if (pt.v < line_top(te, ti, port, k + 1)) { line = k; break; }
    if (pt.v < rd_rect(p + TE_DEST).top) line = 0;
    int s = rds16(p + TE_LINESTARTS + 2 * (u32)line), e = rds16(p + TE_LINESTARTS + 2 * (u32)(line + 1));
    int x = rd_rect(p + TE_DEST).left;
    u32 th = te_text(te);
    for (int i = s; i < e; i++) {
        u8 c = rd8(hderef(th) + (u32)i);
        if (c == '\r') return i;
        int w = char_w(port, ti, te, i, c);
        if (pt.h < x + w / 2) return i;
        x += w;
    }
    return e < n || (n && rd8(hderef(th) + (u32)n - 1) != '\r' && e == n) ? (e == n ? n : e) : e;
}

/* ---- drawing ---- */
static void te_draw(u32 te, Rect area) {
    u32 p = te_rec(te);
    TEInfo *ti = teinfo(te);
    u32 port = rd32(p + TE_INPORT);
    u32 save = qd_port();
    qd_set_port(port);
    u8 sf[PORT_SIZE]; gmemcpy_from(sf, port, PORT_SIZE);
    Rect view = rd_rect(p + TE_VIEW);
    Rect clip = rect_sect(view, area);
    /* clip */
    u32 oldclip = rgn_new();
    u32 pc = rd32(port + PORT_CLIP);
    mm_set_handle_size(oldclip, mm_handle_size(pc));
    gmemmove(hderef(oldclip), hderef(pc), mm_handle_size(pc));
    HRgn c0, cr, o = { 0, NULL };
    hrgn_from_guest(&c0, pc);
    hrgn_rect(&cr, clip.top, clip.left, clip.bottom, clip.right);
    hrgn_op(&o, &c0, &cr, 1);
    hrgn_to_guest(&o, pc);
    hrgn_free(&c0); hrgn_free(&cr); hrgn_free(&o);
    Paint bk; paint_back(&bk, port);
    draw_rect(port, clip, &bk, patCopy);
    int nl = rds16(p + TE_NLINES);
    u32 th = te_text(te);
    int n = te_len(te);
    u8 *t = malloc((size_t)n + 1);
    if (n) gmemcpy_from(t, hderef(th), (u32)n);
    int just = rds16(p + TE_JUST);
    Rect dest = rd_rect(p + TE_DEST);
    for (int k = 0; k < nl; k++) {
        int s = rds16(p + TE_LINESTARTS + 2 * (u32)k), e = rds16(p + TE_LINESTARTS + 2 * (u32)(k + 1));
        int top = line_top(te, ti, port, k);
        int a, h;
        if (ti && ti->styled) metrics(port, ti, te, s, e, &a, &h);
        else { a = rds16(p + TE_ASCENT); h = rds16(p + TE_LINEH); }
        if (top > clip.bottom) break;
        if (top + h < clip.top) continue;
        int ee = e;
        while (ee > s && t[ee - 1] == '\r') ee--;
        int lw = 0;
        for (int i = s; i < ee; i++) lw += char_w(port, ti, te, i, t[i]);
        int x = dest.left;
        if (just == 1) x = (dest.left + dest.right - lw) / 2;       /* teCenter */
        else if (just == -1) x = dest.right - lw;                  /* teFlushRight */
        wr16(port + PORT_TXMODE, srcOr);
        for (int i = s; i < ee;) {
            Run tmp; const Run *r = run_at(ti, te, i, &tmp);
            int j = i + 1;
            while (j < ee) { Run t2; const Run *r2 = run_at(ti, te, j, &t2); if (r2 != r && (ti && ti->styled)) break; j++; }
            run_apply(port, r);
            wr16(port + PORT_PNLOC + 2, (u16)x); wr16(port + PORT_PNLOC, (u16)(top + a));
            text_draw(port, t + i, j - i);
            x = rds16(port + PORT_PNLOC + 2);
            i = j;
        }
        if (!ti || !ti->styled) gmemcpy_to(port + PORT_RGBFG, sf + PORT_RGBFG, 6);
    }
    /* selection */
    if (rds16(p + TE_ACTIVE)) {
        int s0 = rds16(p + TE_SELSTART), s1 = rds16(p + TE_SELEND);
        if (s0 != s1) {
            for (int k = line_of(te, s0); k <= line_of(te, s1 > 0 ? s1 - 1 : 0) && k < nl; k++) {
                int ls = rds16(p + TE_LINESTARTS + 2 * (u32)k), le = rds16(p + TE_LINESTARTS + 2 * (u32)(k + 1));
                int a0 = s0 > ls ? s0 : ls, a1 = s1 < le ? s1 : le;
                int top = line_top(te, ti, port, k), bot = line_top(te, ti, port, k + 1);
                HRgn r; hrgn_rect(&r, top, pos_x(te, ti, port, a0), bot, a1 >= le ? dest.right : pos_x(te, ti, port, a1));
                qd_invert_hrgn(port, &r);
                hrgn_free(&r);
            }
        }
    }
    free(t);
    /* restore clip & port state */
    mm_set_handle_size(pc, mm_handle_size(oldclip));
    gmemmove(hderef(pc), hderef(oldclip), mm_handle_size(oldclip));
    rgn_dispose(oldclip);
    gmemcpy_to(port + PORT_PNLOC, sf + PORT_PNLOC, PORT_GRAFPROCS - PORT_PNLOC);
    if (is_color_port(port)) gmemcpy_to(port + PORT_RGBFG, sf + PORT_RGBFG, 6);
    qd_set_port(save);
}

static void te_caret(u32 te, bool on) {
    u32 p = te_rec(te);
    TEInfo *ti = teinfo(te);
    if (!ti || !rds16(p + TE_ACTIVE)) return;
    int s0 = rds16(p + TE_SELSTART), s1 = rds16(p + TE_SELEND);
    if (s0 != s1 || ti->caret_on == on) return;
    ti->caret_on = on;
    u32 port = rd32(p + TE_INPORT);
    u32 save = qd_port();
    qd_set_port(port);
    int line = line_of(te, s0);
    int x = pos_x(te, ti, port, s0);
    int top = line_top(te, ti, port, line), bot = line_top(te, ti, port, line + 1);
    if (bot <= top) bot = top + rds16(p + TE_LINEH);
    if (te_len(te) == 0) bot = top + (rds16(p + TE_LINEH) > 0 ? rds16(p + TE_LINEH) : 12);
    HRgn r; hrgn_rect(&r, top, x - 1, bot, x);
    Rect view = rd_rect(p + TE_VIEW);
    HRgn v; hrgn_rect(&v, view.top, view.left, view.bottom, view.right);
    HRgn o = { 0, NULL }; hrgn_op(&o, &r, &v, 1);
    qd_invert_hrgn(port, &o);
    hrgn_free(&r); hrgn_free(&v); hrgn_free(&o);
    qd_set_port(save);
}

static void te_redraw(u32 te) {
    TEInfo *ti = teinfo(te);
    if (ti) ti->caret_on = false;
    te_draw(te, rd_rect(te_rec(te) + TE_VIEW));
}

/* ---- creation ---- */
static u32 te_new(Rect dest, Rect view, bool styled) {
    u32 te = mm_new_handle(TE_LINESTARTS + 4, true, ZONE_APP);
    u32 p = hderef(te);
    u32 port = qd_port();
    wr_rect(p + TE_DEST, dest);
    wr_rect(p + TE_VIEW, view);
    wr16(p + TE_SELSTART, 0); wr16(p + TE_SELEND, 0);
    wr16(p + TE_ACTIVE, 0);
    wr32(p + TE_CARETTIME, 32);
    wr16(p + TE_JUST, 0);
    wr32(p + TE_HTEXT, mm_new_handle(0, false, ZONE_APP));
    wr16(p + TE_FONT, rd16(port + PORT_TXFONT));
    wr8(p + TE_FACE, rd8(port + PORT_TXFACE));
    wr16(p + TE_MODE, rd16(port + PORT_TXMODE));
    wr16(p + TE_SIZE, rd16(port + PORT_TXSIZE));
    wr32(p + TE_INPORT, port);
    TEInfo *ti = NULL;
    for (int i = 0; i < MAX_TE; i++) if (!g_te[i].te) { ti = &g_te[i]; break; }
    if (!ti) fatal("too many TE records");
    memset(ti, 0, sizeof *ti);
    ti->te = te;
    ti->styled = styled;
    if (styled) {
        ti->capruns = 8;
        ti->runs = malloc(sizeof(Run) * 8);
        RGB fg = port_fg(port);
        ti->runs[0] = (Run){ 0, rds16(port + PORT_TXFONT), rd8(port + PORT_TXFACE), rds16(port + PORT_TXSIZE), fg };
        ti->nruns = 1;
    }
    te_calc(te);
    return te;
}

TRAP(TEInit) { }
TRAP(TENew) { RET(te_new(rd_rect(ARG(0)), rd_rect(ARG(1)), false)); }
TRAP(TEStyleNew) { RET(te_new(rd_rect(ARG(0)), rd_rect(ARG(1)), true)); }
TRAP(TEDispose) {
    u32 te = ARG(0);
    TEInfo *ti = teinfo(te);
    if (ti) { free(ti->runs); ti->te = 0; }
    mm_dispose_handle(te_text(te));
    mm_dispose_handle(te);
}

static void runs_insert(TEInfo *ti, int at, int len) {
    if (!ti || !ti->styled) return;
    for (int i = 0; i < ti->nruns; i++) if (ti->runs[i].start > at) ti->runs[i].start += len;
}
static void runs_delete(TEInfo *ti, int from, int to) {
    if (!ti || !ti->styled) return;
    int d = to - from;
    for (int i = 0; i < ti->nruns; i++) {
        if (ti->runs[i].start >= to) ti->runs[i].start -= d;
        else if (ti->runs[i].start > from) ti->runs[i].start = from;
    }
    /* drop empty runs (keep the first) */
    int j = 0;
    for (int i = 0; i < ti->nruns; i++) {
        if (j > 0 && ti->runs[i].start == ti->runs[j - 1].start) { ti->runs[j - 1] = ti->runs[i]; continue; }
        ti->runs[j++] = ti->runs[i];
    }
    ti->nruns = j;
    if (ti->nruns) ti->runs[0].start = 0;
}

static void text_replace(u32 te, int from, int to, const u8 *ins, int n) {
    u32 p = te_rec(te);
    u32 th = te_text(te);
    int len = te_len(te);
    if (from < 0) from = 0;
    if (to > len) to = len;
    if (from > to) from = to;
    int nl = len - (to - from) + n;
    if (nl > 32767) { n -= nl - 32767; nl = 32767; }
    u8 *buf = malloc((size_t)nl + 1);
    if (from) gmemcpy_from(buf, hderef(th), (u32)from);
    memcpy(buf + from, ins, (size_t)n);
    if (len - to) gmemcpy_from(buf + from + n, hderef(th) + (u32)to, (u32)(len - to));
    mm_set_handle_size(th, (u32)nl);
    if (nl) gmemcpy_to(hderef(th), buf, (u32)nl);
    free(buf);
    p = te_rec(te);
    wr16(p + TE_LENGTH, (u16)nl);
    TEInfo *ti = teinfo(te);
    if (to > from) runs_delete(ti, from, to);
    if (n) runs_insert(ti, from, n);
}

TRAP(TESetText) {
    u32 text = ARG(0); s32 len = (s32)ARG(1); u32 te = ARG(2);
    u8 *buf = malloc((size_t)(len > 0 ? len : 1));
    if (len > 0) gmemcpy_from(buf, text, (u32)len);
    TEInfo *ti = teinfo(te);
    if (ti && ti->styled) ti->nruns = 1;
    text_replace(te, 0, te_len(te), buf, len > 0 ? len : 0);
    free(buf);
    u32 p = te_rec(te);
    wr16(p + TE_SELSTART, (u16)te_len(te)); wr16(p + TE_SELEND, (u16)te_len(te));
    te_calc(te);
}
TRAP(TEGetText) { RET(te_text(ARG(0))); }
TRAP(TECalText) { te_calc(ARG(0)); }

TRAP(TEInsert) {
    u32 text = ARG(0); s32 len = (s32)ARG(1); u32 te = ARG(2);
    if (len <= 0) return;
    u8 *buf = malloc((size_t)len);
    gmemcpy_from(buf, text, (u32)len);
    u32 p = te_rec(te);
    int s0 = rds16(p + TE_SELSTART);
    text_replace(te, s0, s0, buf, len);
    free(buf);
    p = te_rec(te);
    wr16(p + TE_SELSTART, (u16)(rds16(p + TE_SELSTART) + len));
    wr16(p + TE_SELEND, (u16)(rds16(p + TE_SELEND) + len));
    te_calc(te);
    te_redraw(te);
}

/* TEStyleInsert(text, len, StScrpHandle, te) */
TRAP(TEStyleInsert) {
    u32 text = ARG(0); s32 len = (s32)ARG(1); u32 st = ARG(2); u32 te = ARG(3);
    if (len <= 0) return;
    TEInfo *ti = teinfo(te);
    u8 *buf = malloc((size_t)len);
    gmemcpy_from(buf, text, (u32)len);
    u32 p = te_rec(te);
    int at = rds16(p + TE_SELSTART);
    text_replace(te, at, at, buf, len);
    free(buf);
    if (ti && ti->styled && st && hderef(st)) {
        u32 sp = hderef(st);
        int n = rds16(sp);
        /* remember the style that follows the insertion */
        Run tmp; Run after = *run_at(ti, te, at + len, &tmp);
        for (int k = 0; k < n; k++) {
            u32 e = sp + 2 + 20 * (u32)k;
            Run r;
            r.start = at + (s32)rd32(e);
            r.font = rds16(e + 8); r.face = rd8(e + 10); r.size = rds16(e + 12);
            rd_rgb(e + 14, &r.color);
            /* insert keeping runs sorted; replace a run starting at the same offset */
            int pos = ti->nruns;
            for (int i = 0; i < ti->nruns; i++) if (ti->runs[i].start >= r.start) { pos = i; break; }
            if (pos < ti->nruns && ti->runs[pos].start == r.start) ti->runs[pos] = r;
            else {
                if (ti->nruns == ti->capruns) { ti->capruns *= 2; ti->runs = realloc(ti->runs, sizeof(Run) * (size_t)ti->capruns); }
                memmove(&ti->runs[pos + 1], &ti->runs[pos], sizeof(Run) * (size_t)(ti->nruns - pos));
                ti->runs[pos] = r;
                ti->nruns++;
            }
        }
        /* restore following style */
        int endpos = at + len;
        bool have = false;
        for (int i = 0; i < ti->nruns; i++) if (ti->runs[i].start == endpos) have = true;
        if (!have && endpos < te_len(te)) {
            after.start = endpos;
            int pos = ti->nruns;
            for (int i = 0; i < ti->nruns; i++) if (ti->runs[i].start > endpos) { pos = i; break; }
            if (ti->nruns == ti->capruns) { ti->capruns *= 2; ti->runs = realloc(ti->runs, sizeof(Run) * (size_t)ti->capruns); }
            memmove(&ti->runs[pos + 1], &ti->runs[pos], sizeof(Run) * (size_t)(ti->nruns - pos));
            ti->runs[pos] = after;
            ti->nruns++;
        }
        if (ti->nruns) ti->runs[0].start = 0;
    }
    p = te_rec(te);
    wr16(p + TE_SELSTART, (u16)(at + len));
    wr16(p + TE_SELEND, (u16)(at + len));
    te_calc(te);
    te_redraw(te);
}

TRAP(TEDelete) {
    u32 te = ARG(0);
    u32 p = te_rec(te);
    int s0 = rds16(p + TE_SELSTART), s1 = rds16(p + TE_SELEND);
    if (s0 == s1) return;
    text_replace(te, s0, s1, NULL, 0);
    p = te_rec(te);
    wr16(p + TE_SELEND, (u16)s0);
    te_calc(te);
    te_redraw(te);
}

TRAP(TESetSelect) {
    s32 s0 = (s32)ARG(0), s1 = (s32)ARG(1); u32 te = ARG(2);
    int n = te_len(te);
    if (s0 < 0) s0 = 0;
    if (s1 > n) s1 = n;
    if (s0 > n) s0 = n;
    if (s1 < s0) s1 = s0;
    u32 p = te_rec(te);
    te_caret(te, false);
    wr16(p + TE_SELSTART, (u16)s0); wr16(p + TE_SELEND, (u16)s1);
    if (rds16(p + TE_ACTIVE)) te_redraw(te);
}

TRAP(TEActivate) {
    u32 te = ARG(0);
    u32 p = te_rec(te);
    if (rds16(p + TE_ACTIVE)) return;
    wr16(p + TE_ACTIVE, 1);
    te_redraw(te);
}
TRAP(TEDeactivate) {
    u32 te = ARG(0);
    u32 p = te_rec(te);
    if (!rds16(p + TE_ACTIVE)) return;
    te_caret(te, false);
    wr16(p + TE_ACTIVE, 0);
    te_redraw(te);
}

TRAP(TEIdle) {
    u32 te = ARG(0);
    TEInfo *ti = teinfo(te);
    if (!ti) return;
    u32 t = tick_count();
    if (t - ti->caret_tick >= rd32(0x02F4) /* CaretTime */) {
        ti->caret_tick = t;
        te_caret(te, !ti->caret_on);
    }
}

TRAP(TEUpdate) {
    Rect r = rd_rect(ARG(0)); u32 te = ARG(1);
    TEInfo *ti = teinfo(te);
    if (ti) ti->caret_on = false;
    te_draw(te, r);
}

TRAP(TEKey) {
    u8 ch = (u8)ARG(0); u32 te = ARG(1);
    u32 p = te_rec(te);
    int s0 = rds16(p + TE_SELSTART), s1 = rds16(p + TE_SELEND), n = te_len(te);
    te_caret(te, false);
    switch (ch) {
    case 0x08: /* backspace */
        if (s0 != s1) text_replace(te, s0, s1, NULL, 0);
        else if (s0 > 0) { text_replace(te, s0 - 1, s0, NULL, 0); s0--; }
        p = te_rec(te);
        wr16(p + TE_SELSTART, (u16)s0); wr16(p + TE_SELEND, (u16)s0);
        break;
    case 0x7F: /* forward delete */
        if (s0 != s1) text_replace(te, s0, s1, NULL, 0);
        else if (s0 < n) text_replace(te, s0, s0 + 1, NULL, 0);
        p = te_rec(te);
        wr16(p + TE_SELSTART, (u16)s0); wr16(p + TE_SELEND, (u16)s0);
        break;
    case 0x1C: { int q = s0 != s1 ? s0 : (s0 > 0 ? s0 - 1 : 0); wr16(p + TE_SELSTART, (u16)q); wr16(p + TE_SELEND, (u16)q); break; }
    case 0x1D: { int q = s0 != s1 ? s1 : (s1 < n ? s1 + 1 : n); wr16(p + TE_SELSTART, (u16)q); wr16(p + TE_SELEND, (u16)q); break; }
    case 0x1E: case 0x1F: break;
    default: {
        u8 c = ch == 0x03 ? '\r' : ch;
        text_replace(te, s0, s1, &c, 1);
        p = te_rec(te);
        wr16(p + TE_SELSTART, (u16)(s0 + 1)); wr16(p + TE_SELEND, (u16)(s0 + 1));
    }
    }
    te_calc(te);
    te_redraw(te);
}

TRAP(TEClick) {
    Point pt = pt_from_u32(ARG(0)); bool extend = ARGB(1); u32 te = ARG(2);
    TEInfo *ti = teinfo(te);
    u32 p = te_rec(te);
    u32 port = rd32(p + TE_INPORT);
    u32 save = qd_port();
    qd_set_port(port);
    int anchor = extend ? rds16(p + TE_SELSTART) : pos_from_point(te, ti, port, pt);
    u32 now = tick_count();
    bool dbl = (now - rd32(p + TE_CLICKTIME)) < rd32(0x02F0) /* DoubleTime */ && abs(rds16(p + TE_CLICKLOC) - anchor) <= 1;
    wr32(p + TE_CLICKTIME, now);
    wr16(p + TE_CLICKLOC, (u16)anchor);
    te_caret(te, false);
    if (dbl) {
        u32 th = te_text(te);
        int n = te_len(te), a = anchor, b = anchor;
        while (a > 0 && rd8(hderef(th) + (u32)a - 1) > ' ') a--;
        while (b < n && rd8(hderef(th) + (u32)b) > ' ') b++;
        wr16(p + TE_SELSTART, (u16)a); wr16(p + TE_SELEND, (u16)b);
        te_redraw(te);
    } else {
        int last = -1;
        do {
            Point m = ev_mouse_global();
            Surf s; surf_from_port(port, &s);
            Point lm = { (s16)(m.v + s.bounds.top), (s16)(m.h + s.bounds.left) };
            int q = pos_from_point(te, ti, port, lm);
            if (q != last) {
                last = q;
                int a = anchor < q ? anchor : q, b = anchor < q ? q : anchor;
                wr16(p + TE_SELSTART, (u16)a); wr16(p + TE_SELEND, (u16)b);
                te_redraw(te);
            }
            ev_idle_frame();
        } while (ev_mouse_button());
    }
    qd_set_port(save);
}

TRAP(TEScroll) {
    s16 dh = ARGS16(0), dv = ARGS16(1); u32 te = ARG(2);
    u32 p = te_rec(te);
    Rect d = rd_rect(p + TE_DEST);
    d.left += dh; d.right += dh; d.top += dv; d.bottom += dv;
    wr_rect(p + TE_DEST, d);
    te_redraw(te);
}

TRAP(TESetAlignment) { wr16(te_rec(ARG(1)) + TE_JUST, (u16)ARGS16(0)); }
TRAP(TETextBox) {
    u32 text = ARG(0); s32 len = (s32)ARG(1); Rect box = rd_rect(ARG(2)); s16 just = ARGS16(3);
    u32 te = te_new(box, box, false);
    u32 p = te_rec(te);
    wr16(p + TE_JUST, (u16)just);
    u8 *buf = malloc((size_t)(len > 0 ? len : 1));
    if (len > 0) gmemcpy_from(buf, text, (u32)len);
    text_replace(te, 0, 0, buf, len > 0 ? len : 0);
    free(buf);
    te_calc(te);
    te_draw(te, box);
    TEInfo *ti = teinfo(te);
    if (ti) { free(ti->runs); ti->te = 0; }
    mm_dispose_handle(te_text(te));
    mm_dispose_handle(te);
}

/* ---- scrap ----
   The TextEdit scrap doubles as the desk scrap (TEXT only) and is kept in
   sync with the host clipboard, CR line breaks <-> LF. */
static void scrap_export(void) {
    char *mr = malloc(g_scraplen + 1), *utf = malloc(g_scraplen * 3 + 1);
    for (u32 i = 0; i < g_scraplen; i++) mr[i] = g_scrap[i] == '\r' ? '\n' : (char)g_scrap[i];
    mr[g_scraplen] = 0;
    macroman_to_utf8(mr, utf, g_scraplen * 3 + 1);
    host_clipboard_set(utf);
    free(mr); free(utf);
}
static void scrap_import(void) {
    char *utf = host_clipboard_get();
    if (!utf) return;
    size_t n = strlen(utf);
    char *mr = malloc(n + 1);
    utf8_to_macroman(utf, mr, n + 1);
    free(g_scrap);
    g_scraplen = 0;
    g_scrap = malloc(strlen(mr) + 1);
    for (char *c = mr; *c; c++) {
        if (*c == '\r' && c[1] == '\n') continue;
        g_scrap[g_scraplen++] = *c == '\n' ? '\r' : (u8)*c;
    }
    free(mr); free(utf);
}

/* GetScrap(Handle dest, ResType type, long *offset) -> length or noTypeErr */
TRAP(GetScrap) {
    u32 h = ARG(0), type = ARG(1), offp = ARG(2);
    if (type != FOURCC('T','E','X','T')) { RET((u32)-102); return; }
    scrap_import();
    if (!g_scrap) { RET((u32)-102); return; }
    if (h) {
        if (!mm_set_handle_size(h, g_scraplen)) { RETERR(memFullErr); return; }
        if (g_scraplen) gmemcpy_to(hderef(h), g_scrap, g_scraplen);
    }
    if (offp) wr32(offp, 0);
    RET(g_scraplen);
}
TRAP(ZeroScrap) { free(g_scrap); g_scrap = NULL; g_scraplen = 0; RETERR(noErr); }
TRAP(PutScrap) {
    u32 len = ARG(0), type = ARG(1), src = ARG(2);
    if (type == FOURCC('T','E','X','T')) {
        free(g_scrap);
        g_scraplen = len;
        g_scrap = malloc(len + 1);
        if (len) gmemcpy_from(g_scrap, src, len);
        scrap_export();
    }
    RETERR(noErr);
}
TRAP(LoadScrap) { RETERR(noErr); }
TRAP(UnloadScrap) { RETERR(noErr); }
TRAP(TEFromScrap) { scrap_import(); RETERR(noErr); }
TRAP(TEToScrap) { RETERR(noErr); }

static void te_copy(u32 te) {
    u32 p = te_rec(te);
    int s0 = rds16(p + TE_SELSTART), s1 = rds16(p + TE_SELEND);
    free(g_scrap);
    g_scraplen = (u32)(s1 - s0);
    g_scrap = malloc(g_scraplen + 1);
    if (g_scraplen) gmemcpy_from(g_scrap, hderef(te_text(te)) + (u32)s0, g_scraplen);
    scrap_export();
}
TRAP(TECopy) { te_copy(ARG(0)); }
TRAP(TECut) {
    u32 te = ARG(0);
    te_copy(te);
    u32 p = te_rec(te);
    int s0 = rds16(p + TE_SELSTART), s1 = rds16(p + TE_SELEND);
    text_replace(te, s0, s1, NULL, 0);
    p = te_rec(te);
    wr16(p + TE_SELEND, (u16)s0);
    te_calc(te); te_redraw(te);
}
TRAP(TEPaste) {
    u32 te = ARG(0);
    u32 p = te_rec(te);
    int s0 = rds16(p + TE_SELSTART), s1 = rds16(p + TE_SELEND);
    scrap_import();
    text_replace(te, s0, s1, g_scrap, (int)g_scraplen);
    p = te_rec(te);
    wr16(p + TE_SELSTART, (u16)(s0 + (int)g_scraplen)); wr16(p + TE_SELEND, (u16)(s0 + (int)g_scraplen));
    te_calc(te); te_redraw(te);
}

/* internal API for the Dialog Manager */
void te_api_key(u32 te, u8 ch) { u32 a[2] = { ch, te }; (void)a; CPU fake; memset(&fake, 0, sizeof fake);
    fake.r[3] = ch; fake.r[4] = te; trap_TEKey(&fake); }
void te_api_click(u32 te, Point pt, bool ext) { CPU fake; memset(&fake, 0, sizeof fake);
    fake.r[3] = pt_to_u32(pt); fake.r[4] = ext; fake.r[5] = te; trap_TEClick(&fake); }
void te_api_idle(u32 te) { CPU fake; memset(&fake, 0, sizeof fake); fake.r[3] = te; trap_TEIdle(&fake); }
void te_api_activate(u32 te, bool on) { CPU fake; memset(&fake, 0, sizeof fake); fake.r[3] = te;
    if (on) trap_TEActivate(&fake); else trap_TEDeactivate(&fake); }
void te_api_update(u32 te) { te_redraw(te); }
u32 te_api_new(Rect r) { return te_new(r, r, false); }
void te_api_settext(u32 te, const u8 *s, int n) {
    text_replace(te, 0, te_len(te), s, n);
    wr16(te_rec(te) + TE_SELSTART, 0); wr16(te_rec(te) + TE_SELEND, (u16)te_len(te));
    te_calc(te);
}
void te_api_rects(u32 te, Rect r) { wr_rect(te_rec(te) + TE_DEST, r); wr_rect(te_rec(te) + TE_VIEW, r); te_calc(te); }
void te_api_cut(u32 te) { CPU f; memset(&f, 0, sizeof f); f.r[3] = te; trap_TECut(&f); }
void te_api_copy(u32 te) { te_copy(te); }
void te_api_paste(u32 te) { CPU f; memset(&f, 0, sizeof f); f.r[3] = te; trap_TEPaste(&f); }
void te_api_delete(u32 te) { CPU f; memset(&f, 0, sizeof f); f.r[3] = te; trap_TEDelete(&f); }
void te_api_setselect(u32 te, int a, int b) { CPU f; memset(&f, 0, sizeof f); f.r[3] = (u32)a; f.r[4] = (u32)b; f.r[5] = te; trap_TESetSelect(&f); }
