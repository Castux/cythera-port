/* Regions: a band/span representation on the host side, converted to and
 * from the Mac region format (rgnSize, rgnBBox, inversion-point scanlines).
 */
#include "qd.h"

void hrgn_free(HRgn *r) {
    for (int i = 0; i < r->nb; i++) free(r->b[i].x);
    free(r->b);
    r->b = NULL; r->nb = 0;
}

static void add_band(HRgn *r, int *cap, int y0, int y1, const s16 *x, int n) {
    if (n == 0 || y1 <= y0) return;
    /* coalesce with previous identical band */
    if (r->nb) {
        Band *p = &r->b[r->nb - 1];
        if (p->y1 == y0 && p->n == n && !memcmp(p->x, x, sizeof(s16) * (size_t)n)) { p->y1 = y1; return; }
    }
    if (r->nb == *cap) { *cap = *cap ? *cap * 2 : 8; r->b = realloc(r->b, sizeof(Band) * (size_t)*cap); }
    Band *b = &r->b[r->nb++];
    b->y0 = y0; b->y1 = y1; b->n = n;
    b->x = malloc(sizeof(s16) * (size_t)n);
    memcpy(b->x, x, sizeof(s16) * (size_t)n);
}

void hrgn_rect(HRgn *r, int top, int left, int bottom, int right) {
    r->nb = 0; r->b = NULL;
    if (bottom <= top || right <= left) return;
    int cap = 0;
    s16 x[2] = { (s16)left, (s16)right };
    add_band(r, &cap, top, bottom, x, 2);
}

void hrgn_copy(HRgn *dst, const HRgn *src) {
    dst->nb = 0; dst->b = NULL;
    int cap = 0;
    for (int i = 0; i < src->nb; i++) add_band(dst, &cap, src->b[i].y0, src->b[i].y1, src->b[i].x, src->b[i].n);
}

bool hrgn_empty(const HRgn *r) { return r->nb == 0; }

void hrgn_bbox(const HRgn *r, Rect *bb) {
    if (!r->nb) { *bb = (Rect){ 0, 0, 0, 0 }; return; }
    int l = 32767, rr = -32768;
    for (int i = 0; i < r->nb; i++) {
        if (r->b[i].x[0] < l) l = r->b[i].x[0];
        if (r->b[i].x[r->b[i].n - 1] > rr) rr = r->b[i].x[r->b[i].n - 1];
    }
    *bb = (Rect){ (s16)r->b[0].y0, (s16)l, (s16)r->b[r->nb - 1].y1, (s16)rr };
}

bool hrgn_contains(const HRgn *r, int x, int y) {
    for (int i = 0; i < r->nb; i++) {
        const Band *b = &r->b[i];
        if (y < b->y0 || y >= b->y1) continue;
        for (int k = 0; k + 1 < b->n; k += 2) if (x >= b->x[k] && x < b->x[k + 1]) return true;
        return false;
    }
    return false;
}

void hrgn_offset(HRgn *r, int dx, int dy) {
    for (int i = 0; i < r->nb; i++) {
        r->b[i].y0 += dy; r->b[i].y1 += dy;
        for (int k = 0; k < r->b[i].n; k++) r->b[i].x[k] = (s16)(r->b[i].x[k] + dx);
    }
}

/* Combine two span lists. op: 0 union, 1 sect, 2 diff (a-b), 3 xor */
static int span_op(const s16 *a, int na, const s16 *b, int nb, int op, s16 *out) {
    int i = 0, j = 0, n = 0;
    bool ina = false, inb = false, prev = false;
    while (i < na || j < nb) {
        int x;
        if (j >= nb || (i < na && a[i] <= b[j])) x = a[i]; else x = b[j];
        while (i < na && a[i] == x) { ina = !ina; i++; }
        while (j < nb && b[j] == x) { inb = !inb; j++; }
        bool cur;
        switch (op) {
        case 0: cur = ina || inb; break;
        case 1: cur = ina && inb; break;
        case 2: cur = ina && !inb; break;
        default: cur = ina != inb; break;
        }
        if (cur != prev) { out[n++] = (s16)x; prev = cur; }
    }
    return n;
}

static int cmp_int(const void *a, const void *b) { return *(const int *)a - *(const int *)b; }

void hrgn_op(HRgn *out, const HRgn *a, const HRgn *b, int op) {
    HRgn res = { 0, NULL };
    int cap = 0;
    int ny = 0;
    int *ys = malloc(sizeof(int) * (size_t)(2 * (a->nb + b->nb) + 2));
    for (int i = 0; i < a->nb; i++) { ys[ny++] = a->b[i].y0; ys[ny++] = a->b[i].y1; }
    for (int i = 0; i < b->nb; i++) { ys[ny++] = b->b[i].y0; ys[ny++] = b->b[i].y1; }
    qsort(ys, (size_t)ny, sizeof(int), cmp_int);
    int maxn = 0;
    for (int i = 0; i < a->nb; i++) if (a->b[i].n > maxn) maxn = a->b[i].n;
    int maxb = 0;
    for (int i = 0; i < b->nb; i++) if (b->b[i].n > maxb) maxb = b->b[i].n;
    s16 *tmp = malloc(sizeof(s16) * (size_t)(maxn + maxb + 2));
    int ia = 0, ib = 0;
    for (int k = 0; k + 1 < ny; k++) {
        int y0 = ys[k], y1 = ys[k + 1];
        if (y0 == y1) continue;
        while (ia < a->nb && a->b[ia].y1 <= y0) ia++;
        while (ib < b->nb && b->b[ib].y1 <= y0) ib++;
        const Band *ba = (ia < a->nb && a->b[ia].y0 <= y0) ? &a->b[ia] : NULL;
        const Band *bb = (ib < b->nb && b->b[ib].y0 <= y0) ? &b->b[ib] : NULL;
        int n = span_op(ba ? ba->x : NULL, ba ? ba->n : 0, bb ? bb->x : NULL, bb ? bb->n : 0, op, tmp);
        add_band(&res, &cap, y0, y1, tmp, n);
    }
    free(tmp);
    free(ys);
    hrgn_free(out);
    *out = res;
}

void hrgn_from_mask(HRgn *r, const u8 *mask, int w, int h, int ox, int oy) {
    r->nb = 0; r->b = NULL;
    int cap = 0;
    s16 *x = malloc(sizeof(s16) * (size_t)(w + 2));
    for (int y = 0; y < h; y++) {
        int n = 0;
        bool in = false;
        for (int i = 0; i < w; i++) {
            bool v = mask[(size_t)y * (size_t)w + (size_t)i] != 0;
            if (v != in) { x[n++] = (s16)(i + ox); in = v; }
        }
        if (in) x[n++] = (s16)(w + ox);
        add_band(r, &cap, y + oy, y + oy + 1, x, n);
    }
    free(x);
}

/* ---- guest format ---- */
void hrgn_from_guest(HRgn *r, u32 rgnh) {
    r->nb = 0; r->b = NULL;
    if (!rgnh) return;
    u32 p = hderef(rgnh);
    if (!p) return;
    u16 size = rd16(p);
    Rect bb = rd_rect(p + 2);
    if (size <= 10) {
        hrgn_rect(r, bb.top, bb.left, bb.bottom, bb.right);
        return;
    }
    int cap = 0;
    s16 cur[1024]; int ncur = 0;
    u32 q = p + 10, end = p + size;
    int prev_y = 0;
    bool have = false;
    while (q + 2 <= end) {
        s16 y = rds16(q); q += 2;
        if (y == 0x7FFF) break;
        if (have) add_band(r, &cap, prev_y, y, cur, ncur);
        /* read inversion points and xor into cur */
        s16 inv[1024]; int ni = 0;
        while (q + 2 <= end) {
            s16 x = rds16(q); q += 2;
            if (x == 0x7FFF) break;
            if (ni < 1024) inv[ni++] = x;
        }
        s16 nxt[2048];
        int nn = span_op(cur, ncur, inv, ni, 3, nxt);
        ncur = nn < 1024 ? nn : 1024;
        memcpy(cur, nxt, sizeof(s16) * (size_t)ncur);
        prev_y = y; have = true;
    }
}

void hrgn_to_guest(const HRgn *r, u32 rgnh) {
    Rect bb;
    hrgn_bbox(r, &bb);
    bool rect = r->nb == 0 || (r->nb == 1 && r->b[0].n == 2);
    if (rect) {
        mm_set_handle_size(rgnh, 10);
        u32 p = hderef(rgnh);
        wr16(p, 10);
        wr_rect(p + 2, bb);
        return;
    }
    /* encode inversion points */
    size_t cap = 256, n = 0;
    s16 *buf = malloc(sizeof(s16) * cap);
#define PUSH(v) do { if (n == cap) { cap *= 2; buf = realloc(buf, sizeof(s16) * cap); } buf[n++] = (s16)(v); } while (0)
    /* Each band start sets the span list; a band end not immediately
       followed by another band clears it. Emit the XOR of consecutive
       span lists as inversion points. */
    const s16 *prev = NULL; int nprev = 0;
    int maxn = 2;
    for (int i = 0; i < r->nb; i++) if (r->b[i].n > maxn) maxn = r->b[i].n;
    s16 *tmp = malloc(sizeof(s16) * (size_t)(2 * maxn + 2));
    for (int i = 0; i < r->nb; i++) {
        for (int pass = 0; pass < 2; pass++) {
            int y; const s16 *cur; int ncur;
            if (pass == 0) { y = r->b[i].y0; cur = r->b[i].x; ncur = r->b[i].n; }
            else {
                if (i + 1 < r->nb && r->b[i + 1].y0 == r->b[i].y1) continue;
                y = r->b[i].y1; cur = NULL; ncur = 0;
            }
            int k = span_op(prev, nprev, cur, ncur, 3, tmp);
            if (k) {
                PUSH(y);
                for (int j = 0; j < k; j++) PUSH(tmp[j]);
                PUSH(0x7FFF);
            }
            prev = cur; nprev = ncur;
        }
    }
    PUSH(0x7FFF);
#undef PUSH
    free(tmp);
    u32 size = 10 + 2 * (u32)n;
    mm_set_handle_size(rgnh, size);
    u32 p = hderef(rgnh);
    wr16(p, (u16)(size > 0x7FFF ? 0x7FFF : size));
    wr_rect(p + 2, bb);
    for (size_t i = 0; i < n; i++) wr16(p + 10 + 2 * (u32)i, (u16)buf[i]);
    free(buf);
}

u32 rgn_new(void) {
    u32 h = mm_new_handle(10, true, ZONE_APP);
    wr16(hderef(h), 10);
    return h;
}
void rgn_dispose(u32 h) { if (h) mm_dispose_handle(h); }
void rgn_set_rect(u32 h, Rect r) {
    if (rect_empty(r)) r = (Rect){ 0, 0, 0, 0 };
    mm_set_handle_size(h, 10);
    u32 p = hderef(h);
    wr16(p, 10);
    wr_rect(p + 2, r);
}
Rect rgn_bbox(u32 h) { return rd_rect(hderef(h) + 2); }

/* ---- region recording (OpenRgn/CloseRgn) ---- */
static bool g_rec_open;
static HRgn g_rec;
u32 rgn_recording(void) { return g_rec_open ? 1 : 0; }
void rgn_record_frame_rect(Rect r) {
    HRgn a;
    hrgn_rect(&a, r.top, r.left, r.bottom, r.right);
    hrgn_op(&g_rec, &g_rec, &a, 3);
    hrgn_free(&a);
}

/* ---------------------------------------------------------------------- */
/* Traps                                                                   */

static void binop(u32 a, u32 b, u32 dst, int op) {
    HRgn ra, rb, out = { 0, NULL };
    hrgn_from_guest(&ra, a);
    hrgn_from_guest(&rb, b);
    hrgn_op(&out, &ra, &rb, op);
    hrgn_to_guest(&out, dst);
    hrgn_free(&ra); hrgn_free(&rb); hrgn_free(&out);
}

TRAP(NewRgn) { RET(rgn_new()); }
TRAP(DisposeRgn) { rgn_dispose(ARG(0)); }
TRAP(CopyRgn) {
    u32 src = ARG(0), dst = ARG(1);
    if (src == dst) return;
    u32 n = mm_handle_size(src);
    mm_set_handle_size(dst, n);
    gmemmove(hderef(dst), hderef(src), n);
}
TRAP(SetEmptyRgn) { rgn_set_rect(ARG(0), (Rect){ 0, 0, 0, 0 }); }
TRAP(RectRgn) { rgn_set_rect(ARG(0), rd_rect(ARG(1))); }
TRAP(SetRectRgn) { rgn_set_rect(ARG(0), mkrect(ARGS16(2), ARGS16(1), ARGS16(4), ARGS16(3))); }
TRAP(UnionRgn) { binop(ARG(0), ARG(1), ARG(2), 0); }
TRAP(SectRgn) { binop(ARG(0), ARG(1), ARG(2), 1); }
TRAP(DiffRgn) { binop(ARG(0), ARG(1), ARG(2), 2); }
TRAP(XorRgn) { binop(ARG(0), ARG(1), ARG(2), 3); }
TRAP(OffsetRgn) {
    u32 h = ARG(0); s16 dh = ARGS16(1), dv = ARGS16(2);
    HRgn r; hrgn_from_guest(&r, h);
    hrgn_offset(&r, dh, dv);
    hrgn_to_guest(&r, h);
    hrgn_free(&r);
}
TRAP(InsetRgn) {
    u32 h = ARG(0); s16 dh = ARGS16(1), dv = ARGS16(2);
    Rect bb = rgn_bbox(h);
    /* exact only for rectangles; good enough for the uses we have */
    HRgn r; hrgn_from_guest(&r, h);
    if (r.nb <= 1) {
        rgn_set_rect(h, mkrect(bb.top + dv, bb.left + dh, bb.bottom - dv, bb.right - dh));
    } else {
        LOG_D("InsetRgn on complex region approximated");
        HRgn out = { 0, NULL };
        HRgn tmp;
        /* intersect shifted copies (erosion) for positive insets */
        hrgn_copy(&out, &r);
        for (int s = -dh; s <= dh; s += (dh ? 2 * dh : 1)) {
            hrgn_copy(&tmp, &r); hrgn_offset(&tmp, s, 0);
            hrgn_op(&out, &out, &tmp, dh >= 0 ? 1 : 0); hrgn_free(&tmp);
            if (!dh) break;
        }
        for (int s = -dv; s <= dv; s += (dv ? 2 * dv : 1)) {
            hrgn_copy(&tmp, &r); hrgn_offset(&tmp, 0, s);
            hrgn_op(&out, &out, &tmp, dv >= 0 ? 1 : 0); hrgn_free(&tmp);
            if (!dv) break;
        }
        hrgn_to_guest(&out, h);
        hrgn_free(&out);
    }
    hrgn_free(&r);
}
TRAP(PtInRgn) {
    Point pt = pt_from_u32(ARG(0));
    HRgn r; hrgn_from_guest(&r, ARG(1));
    RET(hrgn_contains(&r, pt.h, pt.v));
    hrgn_free(&r);
}
TRAP(RectInRgn) {
    Rect rc = rd_rect(ARG(0));
    HRgn r, a, o = { 0, NULL };
    hrgn_from_guest(&r, ARG(1));
    hrgn_rect(&a, rc.top, rc.left, rc.bottom, rc.right);
    hrgn_op(&o, &r, &a, 1);
    RET(!hrgn_empty(&o));
    hrgn_free(&r); hrgn_free(&a); hrgn_free(&o);
}
TRAP(EmptyRgn) {
    HRgn r; hrgn_from_guest(&r, ARG(0));
    RET(hrgn_empty(&r));
    hrgn_free(&r);
}
TRAP(EqualRgn) {
    HRgn a, b, o = { 0, NULL };
    hrgn_from_guest(&a, ARG(0)); hrgn_from_guest(&b, ARG(1));
    hrgn_op(&o, &a, &b, 3);
    RET(hrgn_empty(&o));
    hrgn_free(&a); hrgn_free(&b); hrgn_free(&o);
}
TRAP(OpenRgn) {
    hrgn_free(&g_rec);
    g_rec_open = true;
    u32 port = qd_port();
    if (port) wr32(port + PORT_RGNSAVE, 1);
}
TRAP(CloseRgn) {
    u32 dst = ARG(0);
    hrgn_to_guest(&g_rec, dst);
    hrgn_free(&g_rec);
    g_rec_open = false;
    u32 port = qd_port();
    if (port) wr32(port + PORT_RGNSAVE, 0);
}

/* BitMapToRegion(rgn, bitmap): 1-bits become the region */
TRAP(BitMapToRegion) {
    u32 rgn = ARG(0), bm = ARG(1);
    Surf s;
    if (!surf_from_bitmap(bm, &s)) { RETERR(paramErr); return; }
    int w = s.bounds.right - s.bounds.left, h = s.bounds.bottom - s.bounds.top;
    u8 *m = calloc((size_t)(w > 0 ? w : 1) * (size_t)(h > 0 ? h : 1), 1);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            u32 v = surf_get(&s, x, y);
            m[(size_t)y * (size_t)w + (size_t)x] = s.depth == 1 ? (v != 0) : (v != 0);
        }
    HRgn r;
    hrgn_from_mask(&r, m, w, h, s.bounds.left, s.bounds.top);
    hrgn_to_guest(&r, rgn);
    hrgn_free(&r);
    free(m);
    RETERR(noErr);
}
