/* QuickDraw rendering: clipping, patterns, transfer modes, shapes,
 * CopyBits and friends. */
#include "qd.h"
#include <math.h>
#include <time.h>

bool is_gworld(u32 port);

/* ---------------------------------------------------------------------- */
/* Clipping                                                                */

void port_clip_hrgn(u32 port, HRgn *out) {
    HRgn vis, clip, tmp = { 0, NULL };
    hrgn_from_guest(&vis, rd32(port + PORT_VIS));
    hrgn_from_guest(&clip, rd32(port + PORT_CLIP));
    hrgn_op(&tmp, &vis, &clip, 1);
    Surf s;
    if (surf_from_port(port, &s)) {
        HRgn b;
        hrgn_rect(&b, s.bounds.top, s.bounds.left, s.bounds.bottom, s.bounds.right);
        hrgn_op(&tmp, &tmp, &b, 1);
        hrgn_free(&b);
    }
    hrgn_free(&vis); hrgn_free(&clip);
    *out = tmp;
}

/* ---------------------------------------------------------------------- */
/* Paint sources                                                           */

static void pixpat_to_paint(Paint *p, u32 port, u32 pp) {
    p->fg = port_fg(port); p->bk = port_bk(port);
    if (!pp || !hderef(pp)) { p->kind = 0; return; }
    u32 pr = hderef(pp);
    u16 type = rd16(pr + PP_TYPE);
    if (type == 0 || type == 2) {
        gmemcpy_from(p->pat, pr + PP_PAT1, 8);
        p->kind = 1;
        if (type == 2) { /* RGB pattern: the color is in patMap's ctab */
            u32 pm = rd32(pr + PP_MAP);
            if (pm && hderef(pm)) {
                u32 ct = rd32(hderef(pm) + PM_TABLE);
                if (ct && hderef(ct)) { p->fg = ctab_color(ct, 1); p->kind = 0; }
            }
        }
    } else {
        p->kind = 2;
        p->pixpat = pp;
    }
}

void paint_from_pattern(Paint *p, u32 port, const u8 pat[8]) {
    p->kind = 1;
    memcpy(p->pat, pat, 8);
    p->fg = port_fg(port); p->bk = port_bk(port);
}
void paint_pen(Paint *p, u32 port) {
    if (is_color_port(port)) pixpat_to_paint(p, port, rd32(port + PORT_PNPAT));
    else { u8 pat[8]; gmemcpy_from(pat, port + PORT_PNPAT, 8); paint_from_pattern(p, port, pat); }
}
void paint_back(Paint *p, u32 port) {
    if (is_color_port(port)) {
        pixpat_to_paint(p, port, rd32(port + PORT_BKPAT));
        /* background: an old-style pattern draws bk where bits are 0, fg where 1 */
    } else { u8 pat[8]; gmemcpy_from(pat, port + PORT_BKPAT, 8); paint_from_pattern(p, port, pat); }
}
void paint_fill(Paint *p, u32 port, u32 patptr) {
    u8 pat[8]; gmemcpy_from(pat, patptr, 8);
    paint_from_pattern(p, port, pat);
}

/* For full-color pixpats: sample pattern pixel at pixel coords. */
typedef struct {
    Surf s;
    u32 data;
    int w, h;
    u8 map[256];
    bool ok;
} PixPatCtx;

static void pixpat_ctx(PixPatCtx *c, u32 pp, const Surf *dst) {
    memset(c, 0, sizeof *c);
    u32 pr = hderef(pp);
    u32 pmh = rd32(pr + PP_MAP), dh = rd32(pr + PP_DATA);
    if (!pmh || !hderef(pmh) || !dh || !hderef(dh)) return;
    u32 pm = hderef(pmh);
    c->s.base = hderef(dh);
    c->s.rowbytes = rd16(pm + PM_ROWBYTES) & 0x3FFF;
    c->s.bounds = rd_rect(pm + PM_BOUNDS);
    c->s.depth = rd16(pm + PM_PIXSIZE);
    c->s.ctab = rd32(pm + PM_TABLE);
    c->w = c->s.bounds.right - c->s.bounds.left;
    c->h = c->s.bounds.bottom - c->s.bounds.top;
    if (c->w <= 0 || c->h <= 0) return;
    if (c->s.depth <= 8 && dst->depth <= 8)
        for (int i = 0; i < (1 << c->s.depth); i++) c->map[i] = (u8)pixel_for_rgb(dst, ctab_color(c->s.ctab, i));
    c->ok = true;
}

static u32 pixpat_pixel(PixPatCtx *c, const Surf *dst, int px, int py) {
    int x = px % c->w, y = py % c->h;
    if (x < 0) x += c->w;
    if (y < 0) y += c->h;
    u32 v = surf_get(&c->s, x, y);
    if (c->s.depth <= 8 && dst->depth <= 8) return c->map[v & 0xFF];
    return pixel_for_rgb(dst, rgb_for_pixel(&c->s, v));
}

static inline u32 invert_px(const Surf *s, u32 v) {
    switch (s->depth) {
    case 1: return v ^ 1;
    case 2: return v ^ 3;
    case 4: return v ^ 15;
    case 8: return v ^ 0xFF;
    case 16: return v ^ 0x7FFF;
    default: return v ^ 0x00FFFFFF;
    }
}

static RGB g_hilite = { 0xCCCC, 0xCCCC, 0xFFFF };

void draw_hrgn(u32 port, const HRgn *shape, const Paint *paint, int mode) {
    Surf s;
    if (!surf_from_port(port, &s)) return;
    HRgn clip, r = { 0, NULL };
    port_clip_hrgn(port, &clip);
    hrgn_op(&r, shape, &clip, 1);
    hrgn_free(&clip);
    if (hrgn_empty(&r)) { hrgn_free(&r); return; }
    /* source modes used with patterns act as the corresponding pattern modes */
    if ((mode & 0x7F) < 8) mode = (mode & 0x7F) + 8;
    u32 fgpx = pixel_for_rgb(&s, paint->fg), bkpx = pixel_for_rgb(&s, paint->bk);
    PixPatCtx pc;
    bool colorpat = paint->kind == 2;
    if (colorpat) { pixpat_ctx(&pc, paint->pixpat, &s); if (!pc.ok) colorpat = false; }
    mode &= 0x7F;
    if (s.depth == 8 && (mode == patCopy || mode == patOr) && (paint->kind == 0 || (paint->kind == 1 && !colorpat))) {
        bool solid = paint->kind == 0;
        if (!solid) { solid = true; for (int i = 0; i < 8; i++) if (paint->pat[i] != 0xFF && paint->pat[i] != 0) solid = false;
                      if (solid && paint->pat[0] == 0 && mode == patOr) { hrgn_free(&r); return; } }
        if (solid) {
            u8 v = (u8)(paint->kind == 0 || paint->pat[0] == 0xFF ? fgpx : bkpx);
            bool allsame = true; for (int i = 1; i < 8 && paint->kind == 1; i++) if (paint->pat[i] != paint->pat[0]) allsame = false;
            if (allsame) {
                for (int i = 0; i < r.nb; i++) {
                    const Band *b = &r.b[i];
                    for (int y = b->y0; y < b->y1; y++) {
                        u8 *row = g_mem + s.base + (u32)((y - s.bounds.top) * s.rowbytes);
                        for (int k = 0; k + 1 < b->n; k += 2) memset(row + (b->x[k] - s.bounds.left), v, (size_t)(b->x[k + 1] - b->x[k]));
                    }
                }
                hrgn_free(&r);
                qd_screen_dirty();
                return;
            }
        }
    }
    u32 hipx = pixel_for_rgb(&s, g_hilite);
    for (int i = 0; i < r.nb; i++) {
        const Band *b = &r.b[i];
        for (int y = b->y0; y < b->y1; y++) {
            int py = y - s.bounds.top;
            u8 patrow = paint->pat[py & 7];
            for (int k = 0; k + 1 < b->n; k += 2) {
                for (int x = b->x[k]; x < b->x[k + 1]; x++) {
                    int px = x - s.bounds.left;
                    u32 v;
                    if (mode == hilite) {
                        u32 d = surf_get(&s, px, py);
                        if (d == bkpx) surf_put(&s, px, py, hipx);
                        else if (d == hipx) surf_put(&s, px, py, bkpx);
                        continue;
                    }
                    bool bit;
                    if (paint->kind == 0) bit = true;
                    else if (colorpat) bit = true;
                    else bit = (patrow >> (7 - (px & 7))) & 1;
                    int m = mode;
                    if (m >= notPatCopy && m <= notPatBic) { bit = !bit; m -= 4; }
                    switch (m) {
                    case patCopy:
                        if (colorpat) v = pixpat_pixel(&pc, &s, px, py);
                        else v = bit ? fgpx : bkpx;
                        surf_put(&s, px, py, v);
                        break;
                    case patOr:
                        if (bit) surf_put(&s, px, py, colorpat ? pixpat_pixel(&pc, &s, px, py) : fgpx);
                        break;
                    case patXor:
                        if (bit) surf_put(&s, px, py, invert_px(&s, surf_get(&s, px, py)));
                        break;
                    case patBic:
                        if (bit) surf_put(&s, px, py, bkpx);
                        break;
                    default:
                        /* arithmetic modes with patterns: treat as copy */
                        v = colorpat ? pixpat_pixel(&pc, &s, px, py) : (bit ? fgpx : bkpx);
                        surf_put(&s, px, py, v);
                        break;
                    }
                }
            }
        }
    }
    hrgn_free(&r);
    qd_screen_dirty();
}

void draw_rect(u32 port, Rect rc, const Paint *paint, int mode) {
    HRgn r;
    hrgn_rect(&r, rc.top, rc.left, rc.bottom, rc.right);
    draw_hrgn(port, &r, paint, mode);
    hrgn_free(&r);
}

void qd_invert_hrgn(u32 port, const HRgn *shape) {
    Paint p = { .kind = 0 };
    draw_hrgn(port, shape, &p, patXor);
}

static bool pen_visible(u32 port) { return rds16(port + PORT_PNVIS) >= 0; }
/* shapes other than rects and lines aren't recorded into pictures */
static bool shape_visible(u32 port, const char *what) {
    pict_rec_unsupported(port, what);
    return pen_visible(port);
}

/* ---------------------------------------------------------------------- */
/* Shapes                                                                  */

static void oval_rgn(HRgn *r, Rect rc, int ow, int oh) {
    int w = rc.right - rc.left, h = rc.bottom - rc.top;
    r->nb = 0; r->b = NULL;
    if (w <= 0 || h <= 0) return;
    if (ow > w) ow = w;
    if (oh > h) oh = h;
    if (ow < 2 || oh < 2) { hrgn_rect(r, rc.top, rc.left, rc.bottom, rc.right); return; }
    u8 *m = calloc((size_t)w * (size_t)h, 1);
    double a = ow / 2.0, b = oh / 2.0;
    for (int y = 0; y < h; y++) {
        double inset = 0;
        double yy;
        if (y < oh / 2) yy = b - (y + 0.5);
        else if (y >= h - oh / 2) yy = (y + 0.5) - (h - b);
        else yy = 0;
        if (yy > 0) {
            double t = 1.0 - (yy * yy) / (b * b);
            inset = a - a * sqrt(t > 0 ? t : 0);
        }
        int in = (int)(inset + 0.5);
        for (int x = in; x < w - in; x++) m[(size_t)y * (size_t)w + (size_t)x] = 1;
    }
    hrgn_from_mask(r, m, w, h, rc.left, rc.top);
    free(m);
}

void qd_oval_rgn(HRgn *r, Rect rc, int ow, int oh) { oval_rgn(r, rc, ow, oh); }

static void frame_hrgn_of(HRgn *out, const HRgn *shape, int pw, int ph) {
    /* outline = shape minus shape eroded by pen size */
    HRgn er, tmp;
    hrgn_copy(&er, shape);
    HRgn sh;
    for (int dx = 1; dx < pw + 0 && dx <= pw; dx++) { (void)dx; }
    /* erosion: intersect with translated copies */
    for (int dx = -(pw); dx <= pw; dx += pw ? pw : 1) {
        for (int dy = -(ph); dy <= ph; dy += ph ? ph : 1) {
            hrgn_copy(&sh, shape);
            hrgn_offset(&sh, dx, dy);
            tmp.nb = 0; tmp.b = NULL;
            hrgn_op(&er, &er, &sh, 1);
            hrgn_free(&sh);
            if (!ph) break;
        }
        if (!pw) break;
    }
    out->nb = 0; out->b = NULL;
    hrgn_op(out, shape, &er, 2);
    hrgn_free(&er);
}

static void frame_rect_rgn(HRgn *out, Rect rc, int pw, int ph) {
    HRgn o, i;
    hrgn_rect(&o, rc.top, rc.left, rc.bottom, rc.right);
    hrgn_rect(&i, rc.top + ph, rc.left + pw, rc.bottom - ph, rc.right - pw);
    out->nb = 0; out->b = NULL;
    hrgn_op(out, &o, &i, 2);
    hrgn_free(&o); hrgn_free(&i);
}

static void pen_size(u32 port, int *pw, int *ph) { *ph = rds16(port + PORT_PNSIZE); *pw = rds16(port + PORT_PNSIZE + 2); }

TRAP(FrameRect) {
    u32 port = qd_port();
    Rect rc = rd_rect(ARG(0));
    if (rgn_recording()) { rgn_record_frame_rect(rc); return; }
    if (pict_recording(port)) pict_rec_rect(port, 0, rc);
    if (!pen_visible(port)) return;
    int pw, ph; pen_size(port, &pw, &ph);
    HRgn r; frame_rect_rgn(&r, rc, pw, ph);
    Paint p; paint_pen(&p, port);
    draw_hrgn(port, &r, &p, rds16(port + PORT_PNMODE));
    hrgn_free(&r);
}
TRAP(PaintRect) {
    u32 port = qd_port();
    if (pict_recording(port)) pict_rec_rect(port, 1, rd_rect(ARG(0)));
    if (!pen_visible(port)) return;
    Paint p; paint_pen(&p, port);
    draw_rect(port, rd_rect(ARG(0)), &p, rds16(port + PORT_PNMODE));
}
TRAP(EraseRect) {
    u32 port = qd_port();
    if (pict_recording(port)) pict_rec_rect(port, 2, rd_rect(ARG(0)));
    if (!pen_visible(port)) return;
    Paint p; paint_back(&p, port);
    draw_rect(port, rd_rect(ARG(0)), &p, patCopy);
}
TRAP(InvertRect) {
    HRgn r; Rect rc = rd_rect(ARG(0));
    if (pict_recording(qd_port())) pict_rec_rect(qd_port(), 3, rc);
    if (!pen_visible(qd_port())) return;
    hrgn_rect(&r, rc.top, rc.left, rc.bottom, rc.right);
    qd_invert_hrgn(qd_port(), &r);
    hrgn_free(&r);
}
TRAP(FillRect) {
    u32 port = qd_port();
    if (!shape_visible(port, "FillRect")) return;
    Paint p; paint_fill(&p, port, ARG(1));
    draw_rect(port, rd_rect(ARG(0)), &p, patCopy);
}
TRAP(FillCRect) {
    u32 port = qd_port();
    if (!shape_visible(port, "FillCRect")) return;
    Paint p; pixpat_to_paint(&p, port, ARG(1));
    draw_rect(port, rd_rect(ARG(0)), &p, patCopy);
}

static void round_rect_op(CPU *cpu, int which) {
    if (!shape_visible(qd_port(), "round rect")) return;
    u32 port = qd_port();
    Rect rc = rd_rect(ARG(0));
    int ow = ARGS16(1), oh = ARGS16(2);
    HRgn shape; oval_rgn(&shape, rc, ow, oh);
    Paint p;
    if (which == 0) { /* frame */
        if (!pen_visible(port)) { hrgn_free(&shape); return; }
        int pw, ph; pen_size(port, &pw, &ph);
        HRgn f; frame_hrgn_of(&f, &shape, pw, ph);
        paint_pen(&p, port);
        draw_hrgn(port, &f, &p, rds16(port + PORT_PNMODE));
        hrgn_free(&f);
    } else if (which == 1) { paint_pen(&p, port); draw_hrgn(port, &shape, &p, rds16(port + PORT_PNMODE)); }
    else if (which == 2) { paint_back(&p, port); draw_hrgn(port, &shape, &p, patCopy); }
    else if (which == 3) qd_invert_hrgn(port, &shape);
    hrgn_free(&shape);
}
TRAP(FrameRoundRect) { round_rect_op(cpu, 0); }
TRAP(PaintRoundRect) { round_rect_op(cpu, 1); }
TRAP(EraseRoundRect) { round_rect_op(cpu, 2); }
TRAP(InvertRoundRect) { round_rect_op(cpu, 3); }
TRAP(FillRoundRect) {
    u32 port = qd_port();
    if (!shape_visible(port, "FillRoundRect")) return;
    Rect rc = rd_rect(ARG(0));
    HRgn shape; oval_rgn(&shape, rc, ARGS16(1), ARGS16(2));
    Paint p; paint_fill(&p, port, ARG(3));
    draw_hrgn(port, &shape, &p, patCopy);
    hrgn_free(&shape);
}

static void oval_op(CPU *cpu, int which) {
    u32 port = qd_port();
    if (!shape_visible(port, "oval")) return;
    Rect rc = rd_rect(ARG(0));
    HRgn shape; oval_rgn(&shape, rc, rc.right - rc.left, rc.bottom - rc.top);
    Paint p;
    if (which == 0) {
        if (!pen_visible(port)) { hrgn_free(&shape); return; }
        int pw, ph; pen_size(port, &pw, &ph);
        HRgn f; frame_hrgn_of(&f, &shape, pw, ph);
        paint_pen(&p, port);
        draw_hrgn(port, &f, &p, rds16(port + PORT_PNMODE));
        hrgn_free(&f);
    } else if (which == 1) { paint_pen(&p, port); draw_hrgn(port, &shape, &p, rds16(port + PORT_PNMODE)); }
    else if (which == 2) { paint_back(&p, port); draw_hrgn(port, &shape, &p, patCopy); }
    else if (which == 3) qd_invert_hrgn(port, &shape);
    hrgn_free(&shape);
}
TRAP(FrameOval) { oval_op(cpu, 0); }
TRAP(PaintOval) { oval_op(cpu, 1); }
TRAP(EraseOval) { oval_op(cpu, 2); }
TRAP(InvertOval) { oval_op(cpu, 3); }

/* ---- regions ---- */
TRAP(FrameRgn) {
    u32 port = qd_port();
    if (!pen_visible(port)) return;
    HRgn s; hrgn_from_guest(&s, ARG(0));
    int pw, ph; pen_size(port, &pw, &ph);
    HRgn f; frame_hrgn_of(&f, &s, pw, ph);
    Paint p; paint_pen(&p, port);
    draw_hrgn(port, &f, &p, rds16(port + PORT_PNMODE));
    hrgn_free(&f); hrgn_free(&s);
}
TRAP(PaintRgn) {
    u32 port = qd_port();
    if (!shape_visible(port, "PaintRgn")) return;
    HRgn s; hrgn_from_guest(&s, ARG(0));
    Paint p; paint_pen(&p, port);
    draw_hrgn(port, &s, &p, rds16(port + PORT_PNMODE));
    hrgn_free(&s);
}
TRAP(EraseRgn) {
    u32 port = qd_port();
    if (!shape_visible(port, "EraseRgn")) return;
    HRgn s; hrgn_from_guest(&s, ARG(0));
    Paint p; paint_back(&p, port);
    draw_hrgn(port, &s, &p, patCopy);
    hrgn_free(&s);
}
TRAP(InvertRgn) { if (!shape_visible(qd_port(), "InvertRgn")) return; HRgn s; hrgn_from_guest(&s, ARG(0)); qd_invert_hrgn(qd_port(), &s); hrgn_free(&s); }
TRAP(FillRgn) {
    u32 port = qd_port();
    if (!shape_visible(port, "FillRgn")) return;
    HRgn s; hrgn_from_guest(&s, ARG(0));
    Paint p; paint_fill(&p, port, ARG(1));
    draw_hrgn(port, &s, &p, patCopy);
    hrgn_free(&s);
}
TRAP(FillCRgn) {
    u32 port = qd_port();
    if (!shape_visible(port, "FillCRgn")) return;
    HRgn s; hrgn_from_guest(&s, ARG(0));
    Paint p; pixpat_to_paint(&p, port, ARG(1));
    draw_hrgn(port, &s, &p, patCopy);
    hrgn_free(&s);
}

/* ---- lines and polygons ---- */
static u32 g_poly;          /* open polygon handle */
static bool g_poly_open;

static void poly_add(Point p) {
    u32 n = mm_handle_size(g_poly);
    mm_set_handle_size(g_poly, n + 4);
    wr_point(hderef(g_poly) + n, p);
}

static void line_to(u32 port, int x1, int y1) {
    int x0 = rds16(port + PORT_PNLOC + 2), y0 = rds16(port + PORT_PNLOC);
    wr16(port + PORT_PNLOC + 2, (u16)x1); wr16(port + PORT_PNLOC, (u16)y1);
    if (g_poly_open) {
        if (mm_handle_size(g_poly) == 10) poly_add((Point){ (s16)y0, (s16)x0 });
        poly_add((Point){ (s16)y1, (s16)x1 });
        return;
    }
    if (rgn_recording()) return;
    if (pict_recording(port)) pict_rec_line(port, (Point){ (s16)y0, (s16)x0 }, (Point){ (s16)y1, (s16)x1 });
    if (!pen_visible(port)) return;
    int pw, ph; pen_size(port, &pw, &ph);
    if (pw <= 0 || ph <= 0) return;
    /* sweep the pen rectangle along a Bresenham line */
    HRgn acc = { 0, NULL };
    int dx = abs(x1 - x0), dy = -abs(y1 - y0), sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1, err = dx + dy;
    int x = x0, y = y0;
    /* build a mask covering the line bounding box */
    int minx = (x0 < x1 ? x0 : x1), miny = (y0 < y1 ? y0 : y1);
    int w = dx + pw, h = -dy + ph;
    u8 *m = calloc((size_t)w * (size_t)h, 1);
    for (;;) {
        for (int yy = 0; yy < ph; yy++)
            for (int xx = 0; xx < pw; xx++)
                m[(size_t)(y - miny + yy) * (size_t)w + (size_t)(x - minx + xx)] = 1;
        if (x == x1 && y == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x += sx; }
        if (e2 <= dx) { err += dx; y += sy; }
    }
    hrgn_from_mask(&acc, m, w, h, minx, miny);
    free(m);
    Paint p; paint_pen(&p, port);
    draw_hrgn(port, &acc, &p, rds16(port + PORT_PNMODE));
    hrgn_free(&acc);
}

TRAP(LineTo) { line_to(qd_port(), ARGS16(0), ARGS16(1)); }
TRAP(Line) {
    u32 port = qd_port();
    line_to(port, rds16(port + PORT_PNLOC + 2) + ARGS16(0), rds16(port + PORT_PNLOC) + ARGS16(1));
}

TRAP(OpenPoly) {
    g_poly = mm_new_handle(10, true, ZONE_APP);
    wr16(hderef(g_poly), 10);
    g_poly_open = true;
    u32 port = qd_port();
    wr32(port + PORT_POLYSAVE, g_poly);
    wr16(port + PORT_PNVIS, (u16)(rds16(port + PORT_PNVIS) - 1));
    RET(g_poly);
}
TRAP(ClosePoly) {
    u32 port = qd_port();
    g_poly_open = false;
    wr32(port + PORT_POLYSAVE, 0);
    wr16(port + PORT_PNVIS, (u16)(rds16(port + PORT_PNVIS) + 1));
    u32 n = mm_handle_size(g_poly);
    u32 p = hderef(g_poly);
    wr16(p, (u16)n);
    int np = (int)(n - 10) / 4;
    Rect bb = { 32767, 32767, -32768, -32768 };
    for (int i = 0; i < np; i++) {
        Point q = rd_point(p + 10 + 4 * (u32)i);
        if (q.v < bb.top) bb.top = q.v;
        if (q.v > bb.bottom) bb.bottom = q.v;
        if (q.h < bb.left) bb.left = q.h;
        if (q.h > bb.right) bb.right = q.h;
    }
    if (!np) bb = (Rect){ 0, 0, 0, 0 };
    wr_rect(p + 2, bb);
}
TRAP(KillPoly) { mm_dispose_handle(ARG(0)); }

static void poly_rgn(u32 poly, HRgn *out) {
    u32 p = hderef(poly);
    int np = (int)(rd16(p) - 10) / 4;
    Rect bb = rd_rect(p + 2);
    int w = bb.right - bb.left, h = bb.bottom - bb.top;
    out->nb = 0; out->b = NULL;
    if (np < 3 || w <= 0 || h <= 0) return;
    u8 *m = calloc((size_t)w * (size_t)h, 1);
    for (int y = 0; y < h; y++) {
        double sy = bb.top + y + 0.5;
        double xs[256]; int nx = 0;
        for (int i = 0; i < np; i++) {
            Point a = rd_point(p + 10 + 4 * (u32)i), b = rd_point(p + 10 + 4 * (u32)((i + 1) % np));
            if ((a.v <= sy && b.v > sy) || (b.v <= sy && a.v > sy)) {
                if (nx < 256) xs[nx++] = a.h + (sy - a.v) * (b.h - a.h) / (double)(b.v - a.v);
            }
        }
        for (int i = 1; i < nx; i++) for (int j = i; j > 0 && xs[j - 1] > xs[j]; j--) { double t = xs[j]; xs[j] = xs[j - 1]; xs[j - 1] = t; }
        for (int i = 0; i + 1 < nx; i += 2) {
            int x0 = (int)(xs[i] + 0.5) - bb.left, x1 = (int)(xs[i + 1] + 0.5) - bb.left;
            if (x0 < 0) x0 = 0;
            if (x1 > w) x1 = w;
            for (int x = x0; x < x1; x++) m[(size_t)y * (size_t)w + (size_t)x] = 1;
        }
    }
    hrgn_from_mask(out, m, w, h, bb.left, bb.top);
    free(m);
}
TRAP(PaintPoly) {
    u32 port = qd_port();
    if (!shape_visible(port, "PaintPoly")) return;
    HRgn r; poly_rgn(ARG(0), &r);
    Paint p; paint_pen(&p, port);
    draw_hrgn(port, &r, &p, rds16(port + PORT_PNMODE));
    hrgn_free(&r);
}
TRAP(FillPoly) {
    u32 port = qd_port();
    if (!shape_visible(port, "FillPoly")) return;
    HRgn r; poly_rgn(ARG(0), &r);
    Paint p; paint_fill(&p, port, ARG(1));
    draw_hrgn(port, &r, &p, patCopy);
    hrgn_free(&r);
}
TRAP(ErasePoly) {
    u32 port = qd_port();
    if (!shape_visible(port, "ErasePoly")) return;
    HRgn r; poly_rgn(ARG(0), &r);
    Paint p; paint_back(&p, port);
    draw_hrgn(port, &r, &p, patCopy);
    hrgn_free(&r);
}
TRAP(FramePoly) {
    u32 port = qd_port();
    u32 p = hderef(ARG(0));
    int np = (int)(rd16(p) - 10) / 4;
    if (np < 1) return;
    Point a = rd_point(p + 10);
    wr16(port + PORT_PNLOC + 2, (u16)a.h); wr16(port + PORT_PNLOC, (u16)a.v);
    for (int i = 1; i < np; i++) { Point b = rd_point(p + 10 + 4 * (u32)i); line_to(port, b.h, b.v); }
}

/* ---------------------------------------------------------------------- */
/* CopyBits                                                                */

typedef struct { u8 map[256]; bool identity; bool valid; } Xlate;

static void build_xlate(Xlate *x, const Surf *src, const Surf *dst) {
    x->valid = src->depth <= 8 && dst->depth <= 8;
    x->identity = false;
    if (!x->valid) return;
    int n = 1 << src->depth;
    if (src->ctab && dst->ctab && src->depth == dst->depth) {
        bool same = ctab_seed(src->ctab) == ctab_seed(dst->ctab) && src->ctab == dst->ctab;
        if (!same) {
            same = true;
            for (int i = 0; i < n && same; i++) {
                RGB a = ctab_color(src->ctab, i), b = ctab_color(dst->ctab, i);
                same = a.r == b.r && a.g == b.g && a.b == b.b;
            }
        }
        if (same) { for (int i = 0; i < 256; i++) x->map[i] = (u8)i; x->identity = true; return; }
    }
    for (int i = 0; i < n; i++) x->map[i] = (u8)pixel_for_rgb(dst, rgb_for_pixel(src, (u32)i));
}

static inline RGB rgb_blend(RGB a, RGB b, u32 wa) { /* wa in 0..65535 weight of a */
    return (RGB){ (u16)((a.r * wa + b.r * (65535 - wa)) / 65535), (u16)((a.g * wa + b.g * (65535 - wa)) / 65535),
                  (u16)((a.b * wa + b.b * (65535 - wa)) / 65535) };
}

static RGB op_color(u32 port) {
    if (is_color_port(port)) {
        u32 gv = port_grafvars(port);
        if (gv && hderef(gv)) { RGB c; rd_rgb(hderef(gv), &c); return c; }
    }
    return (RGB){ 0x8000, 0x8000, 0x8000 };
}

static RGB arith(int mode, RGB s, RGB d, RGB op);
u32 ctab_hash_of(u32 h);

/* Cached result tables for 8-bit arithmetic transfer modes: t[src][dst]. */
typedef struct { u32 sh, dh; RGB op; int mode; u8 *t; } ArithTab;
static ArithTab g_at[8];
static int g_at_next;
static const u8 *arith_table(const Surf *src, const Surf *dst, int mode, RGB op) {
    u32 sh = ctab_hash_of(src->ctab), dh = ctab_hash_of(dst->ctab);
    for (int i = 0; i < 8; i++) {
        ArithTab *a = &g_at[i];
        if (a->t && a->sh == sh && a->dh == dh && a->mode == mode && a->op.r == op.r && a->op.g == op.g && a->op.b == op.b) return a->t;
    }
    ArithTab *a = &g_at[g_at_next];
    g_at_next = (g_at_next + 1) % 8;
    if (!a->t) a->t = malloc(65536);
    a->sh = sh; a->dh = dh; a->mode = mode; a->op = op;
    RGB sc[256], dc[256];
    for (int i = 0; i < 256; i++) { sc[i] = rgb_for_pixel(src, (u32)i); dc[i] = rgb_for_pixel(dst, (u32)i); }
    for (int x = 0; x < 256; x++)
        for (int y = 0; y < 256; y++)
            a->t[x * 256 + y] = (u8)pixel_for_rgb(dst, arith(mode, sc[x], dc[y], op));
    return a->t;
}

static RGB arith(int mode, RGB s, RGB d, RGB op) {
    RGB r;
    switch (mode) {
    case blend:
        r.r = (u16)((s.r * (u32)op.r + d.r * (u32)(65535 - op.r)) / 65535);
        r.g = (u16)((s.g * (u32)op.g + d.g * (u32)(65535 - op.g)) / 65535);
        r.b = (u16)((s.b * (u32)op.b + d.b * (u32)(65535 - op.b)) / 65535);
        return r;
    case addPin: {
        u32 a = (u32)s.r + d.r, b = (u32)s.g + d.g, c = (u32)s.b + d.b;
        return (RGB){ (u16)(a > op.r ? op.r : a), (u16)(b > op.g ? op.g : b), (u16)(c > op.b ? op.b : c) };
    }
    case addOver: return (RGB){ (u16)(s.r + d.r), (u16)(s.g + d.g), (u16)(s.b + d.b) };
    case subPin: {
        s32 a = (s32)d.r - s.r, b = (s32)d.g - s.g, c = (s32)d.b - s.b;
        return (RGB){ (u16)(a < op.r ? op.r : a), (u16)(b < op.g ? op.g : b), (u16)(c < op.b ? op.b : c) };
    }
    case subOver: return (RGB){ (u16)(d.r - s.r), (u16)(d.g - s.g), (u16)(d.b - s.b) };
    case addMax: return (RGB){ s.r > d.r ? s.r : d.r, s.g > d.g ? s.g : d.g, s.b > d.b ? s.b : d.b };
    case adMin: return (RGB){ s.r < d.r ? s.r : d.r, s.g < d.g ? s.g : d.g, s.b < d.b ? s.b : d.b };
    }
    return s;
}

/* Core: copies srcBits->dstBits.  maskbm (optional) selects pixels. */
void copybits(u32 srcbm, u32 dstbm, Rect sr, Rect dr, int mode, u32 maskrgn, u32 maskbm, Rect mr) {
    Surf src, dst, msk;
    if (!surf_from_bitmap(srcbm, &src) || !surf_from_bitmap(dstbm, &dst)) return;
    bool have_mask = maskbm && surf_from_bitmap(maskbm, &msk);
    int dw = dr.right - dr.left, dh = dr.bottom - dr.top;
    int sw = sr.right - sr.left, sh = sr.bottom - sr.top;
    if (dw <= 0 || dh <= 0 || sw <= 0 || sh <= 0) return;
    int mw = mr.right - mr.left, mh = mr.bottom - mr.top;
    mode &= ~ditherCopy;

    /* destination region: dstRect ∩ dst bounds ∩ port clip ∩ maskRgn */
    HRgn reg, t;
    hrgn_rect(&reg, dr.top, dr.left, dr.bottom, dr.right);
    hrgn_rect(&t, dst.bounds.top, dst.bounds.left, dst.bounds.bottom, dst.bounds.right);
    hrgn_op(&reg, &reg, &t, 1); hrgn_free(&t);
    u32 port = qd_port();
    if (port) {
        /* the current port's clip applies in the destination's coordinates */
        HRgn vis, clip; hrgn_from_guest(&vis, rd32(port + PORT_VIS)); hrgn_from_guest(&clip, rd32(port + PORT_CLIP));
        hrgn_op(&reg, &reg, &vis, 1); hrgn_op(&reg, &reg, &clip, 1);
        hrgn_free(&vis); hrgn_free(&clip);
    }
    if (maskrgn) { HRgn m; hrgn_from_guest(&m, maskrgn); hrgn_op(&reg, &reg, &m, 1); hrgn_free(&m); }
    if (hrgn_empty(&reg)) { hrgn_free(&reg); return; }

    /* snapshot the source rows if src and dst overlap in memory */
    Surf s = src;
    u8 *snap = NULL;
    u32 snap_base = 0;
    if (src.base == dst.base) {
        int ssy0 = sr.top - src.bounds.top, ssy1 = sr.bottom - src.bounds.top;
        if (ssy0 < 0) ssy0 = 0;
        int maxy = src.bounds.bottom - src.bounds.top;
        if (ssy1 > maxy) ssy1 = maxy;
        if (ssy1 > ssy0) {
            size_t n = (size_t)(ssy1 - ssy0) * (size_t)src.rowbytes;
            snap = malloc(n);
            memcpy(snap, g_mem + src.base + (u32)(ssy0 * src.rowbytes), n);
            /* use a scratch guest area?  read directly from snap instead */
            snap_base = (u32)ssy0;
        }
    }

    Xlate xl; build_xlate(&xl, &src, &dst);
    RGB fg = port ? port_fg(port) : (RGB){ 0, 0, 0 }, bk = port ? port_bk(port) : (RGB){ 0xFFFF, 0xFFFF, 0xFFFF };
    u32 fgpx = pixel_for_rgb(&dst, fg), bkpx = pixel_for_rgb(&dst, bk);
    u32 src_bkpx = pixel_for_rgb(&src, bk);
    RGB opc = port ? op_color(port) : (RGB){ 0x8000, 0x8000, 0x8000 };
    bool one_bit = src.depth == 1;
    int srw = src.bounds.right - src.bounds.left, srh = src.bounds.bottom - src.bounds.top;
    const u8 *atab = NULL;

    /* fast path: unscaled 8-bit to 8-bit copy without masks/arithmetic */
    bool fast = src.depth == 8 && dst.depth == 8 && !have_mask && sw == dw && sh == dh && xl.valid &&
                (mode == srcCopy || mode == transparent);
    if (fast) {
        int ox = sr.left - dr.left - src.bounds.left, oy = sr.top - dr.top - src.bounds.top;
        for (int i = 0; i < reg.nb; i++) {
            const Band *b = &reg.b[i];
            for (int y = b->y0; y < b->y1; y++) {
                int sy = y + oy;
                if (sy < 0 || sy >= srh) continue;
                const u8 *srow = snap ? snap + (size_t)(sy - (int)snap_base) * (size_t)src.rowbytes
                                      : g_mem + src.base + (u32)(sy * src.rowbytes);
                u8 *drow = g_mem + dst.base + (u32)((y - dst.bounds.top) * dst.rowbytes);
                for (int k = 0; k + 1 < b->n; k += 2) {
                    int x0 = b->x[k], x1 = b->x[k + 1];
                    if (x0 + ox < 0) x0 = -ox;
                    if (x1 + ox > srw) x1 = srw - ox;
                    if (x1 <= x0) continue;
                    const u8 *sp = srow + x0 + ox;
                    u8 *dp = drow + (x0 - dst.bounds.left);
                    int n = x1 - x0;
                    if (mode == srcCopy) {
                        if (xl.identity) memmove(dp, sp, (size_t)n);
                        else for (int q = 0; q < n; q++) dp[q] = xl.map[sp[q]];
                    } else {
                        u8 bkv = (u8)src_bkpx;
                        for (int q = 0; q < n; q++) if (sp[q] != bkv) dp[q] = xl.map[sp[q]];
                    }
                }
            }
        }
        free(snap);
        hrgn_free(&reg);
        qd_screen_dirty();
        return;
    }

    for (int i = 0; i < reg.nb; i++) {
        const Band *b = &reg.b[i];
        for (int y = b->y0; y < b->y1; y++) {
            int sy = sr.top + (int)((s64)(y - dr.top) * sh / dh) - src.bounds.top;
            if (sy < 0 || sy >= srh) continue;
            int dy = y - dst.bounds.top;
            int my = have_mask ? mr.top + (int)((s64)(y - dr.top) * mh / dh) - msk.bounds.top : 0;
            for (int k = 0; k + 1 < b->n; k += 2) {
                for (int x = b->x[k]; x < b->x[k + 1]; x++) {
                    int sx = sr.left + (int)((s64)(x - dr.left) * sw / dw) - src.bounds.left;
                    if (sx < 0 || sx >= srw) continue;
                    int dx = x - dst.bounds.left;
                    u32 weight = 65535;
                    if (have_mask) {
                        int mx = mr.left + (int)((s64)(x - dr.left) * mw / dw) - msk.bounds.left;
                        u32 mv = surf_get(&msk, mx, my);
                        if (msk.depth == 1) { if (!mv) continue; }
                        else {
                            RGB mc = rgb_for_pixel(&msk, mv);
                            u32 lum = (mc.r * 30u + mc.g * 59u + mc.b * 11u) / 100u;
                            weight = 65535 - lum;
                            if (weight == 0) continue;
                        }
                    }
                    u32 sv;
                    if (snap) {
                        const u8 *row = snap + (size_t)(sy - (int)snap_base) * (size_t)src.rowbytes;
                        switch (src.depth) {
                        case 8: sv = row[sx]; break;
                        case 1: sv = (row[sx >> 3] >> (7 - (sx & 7))) & 1; break;
                        case 16: sv = be16(row + 2 * sx); break;
                        case 32: sv = be32(row + 4 * sx); break;
                        case 4: sv = (row[sx >> 1] >> (4 - 4 * (sx & 1))) & 15; break;
                        default: sv = (row[sx >> 2] >> (6 - 2 * (sx & 3))) & 3; break;
                        }
                    } else sv = surf_get(&s, sx, sy);
                    int m = mode;
                    if (one_bit) {
                        bool bit = sv != 0;
                        if (m >= notSrcCopy && m <= notSrcBic) { bit = !bit; m -= 4; }
                        switch (m) {
                        case srcCopy: surf_put(&dst, dx, dy, bit ? fgpx : bkpx); break;
                        case srcOr: if (bit) surf_put(&dst, dx, dy, fgpx); break;
                        case srcXor: if (bit) surf_put(&dst, dx, dy, invert_px(&dst, surf_get(&dst, dx, dy))); break;
                        case srcBic: if (bit) surf_put(&dst, dx, dy, bkpx); break;
                        case transparent: if (bit) surf_put(&dst, dx, dy, fgpx); break;
                        default: surf_put(&dst, dx, dy, bit ? fgpx : bkpx); break;
                        }
                        continue;
                    }
                    u32 dv;
                    if (m == transparent) {
                        if (sv == src_bkpx) continue;
                        m = srcCopy;
                    }
                    if (weight == 65535 && m >= blend && m <= adMin && src.depth == 8 && dst.depth == 8) {
                        if (!atab) atab = arith_table(&src, &dst, m, opc);
                        surf_put(&dst, dx, dy, atab[(sv & 0xFF) * 256 + (surf_get(&dst, dx, dy) & 0xFF)]);
                        continue;
                    }
                    if (weight != 65535 || (m >= blend && m <= adMin)) {
                        RGB sc = rgb_for_pixel(&src, sv), dc = rgb_for_pixel(&dst, surf_get(&dst, dx, dy));
                        RGB rc = (m >= blend && m <= adMin) ? arith(m, sc, dc, opc) : sc;
                        if (weight != 65535) rc = rgb_blend(rc, dc, weight);
                        surf_put(&dst, dx, dy, pixel_for_rgb(&dst, rc));
                        continue;
                    }
                    if (xl.valid) dv = xl.map[sv & 0xFF];
                    else dv = pixel_for_rgb(&dst, rgb_for_pixel(&src, sv));
                    switch (m) {
                    case srcCopy: surf_put(&dst, dx, dy, dv); break;
                    case notSrcCopy: surf_put(&dst, dx, dy, invert_px(&dst, dv)); break;
                    case srcOr: surf_put(&dst, dx, dy, surf_get(&dst, dx, dy) | dv); break;
                    case srcXor: surf_put(&dst, dx, dy, surf_get(&dst, dx, dy) ^ dv); break;
                    case srcBic: surf_put(&dst, dx, dy, surf_get(&dst, dx, dy) & ~dv); break;
                    case notSrcOr: surf_put(&dst, dx, dy, surf_get(&dst, dx, dy) | invert_px(&dst, dv)); break;
                    case notSrcXor: surf_put(&dst, dx, dy, surf_get(&dst, dx, dy) ^ invert_px(&dst, dv)); break;
                    case notSrcBic: surf_put(&dst, dx, dy, surf_get(&dst, dx, dy) & dv); break;
                    case hilite: {
                        u32 d = surf_get(&dst, dx, dy);
                        u32 hp = pixel_for_rgb(&dst, g_hilite);
                        if (d == bkpx) surf_put(&dst, dx, dy, hp); else if (d == hp) surf_put(&dst, dx, dy, bkpx);
                        break;
                    }
                    default: surf_put(&dst, dx, dy, dv); break;
                    }
                }
            }
        }
    }
    free(snap);
    hrgn_free(&reg);
    qd_screen_dirty();
}

/* debug: CYTHERA_DUMP_COPYBITS=N writes the destination of the N-th CopyBits */
extern int stbi_write_png(char const *filename, int w, int h, int comp, const void *data, int stride_in_bytes);
static void dump_surface(u32 bm, const char *path) {
    Surf s; if (!surf_from_bitmap(bm, &s)) return;
    int w = s.bounds.right - s.bounds.left, h = s.bounds.bottom - s.bounds.top;
    u8 *rgb = malloc((size_t)w * (size_t)h * 3);
    for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) {
        RGB c = rgb_for_pixel(&s, surf_get(&s, x, y));
        u8 *p = rgb + ((size_t)y * (size_t)w + (size_t)x) * 3; p[0] = (u8)(c.r >> 8); p[1] = (u8)(c.g >> 8); p[2] = (u8)(c.b >> 8);
    }
    stbi_write_png(path, w, h, 3, rgb, w * 3);
    free(rgb);
    LOG_I("dumped surface to %s", path);
}
static u32 g_cb_count;
TRAP(CopyBits) {
    u32 s = ARG(0), d = ARG(1);
    g_cb_count++;
    static int dump_n = -2;
    if (dump_n == -2) { const char *e = getenv("CYTHERA_DUMP_COPYBITS"); dump_n = e ? atoi(e) : -1; }
    Rect sr = rd_rect(ARG(2)), dr = rd_rect(ARG(3));
    s16 mode = ARGS16(4);
    u32 mrgn = ARG(5);
    if (d == qd_port() + PORT_BITS) pict_rec_unsupported(qd_port(), "CopyBits");
    if (g_log_level >= 3) {
        Surf a = {0}, b = {0};
        surf_from_bitmap(s, &a); surf_from_bitmap(d, &b);
        int nz = 0;
        for (int yy = sr.top; yy < sr.bottom; yy++) for (int xx = sr.left; xx < sr.right; xx++) {
            int px = xx - a.bounds.left, py = yy - a.bounds.top;
            if (px >= 0 && py >= 0 && px < a.bounds.right - a.bounds.left && py < a.bounds.bottom - a.bounds.top && surf_get(&a, px, py)) nz++;
        }
        LOG_D("CopyBits nonzero src pixels %d", nz);
        LOG_D("CopyBits src %08x(base %08x rb %d d%d b(%d,%d,%d,%d) ct %08x) dst %08x(base %08x d%d b(%d,%d,%d,%d)) sr(%d,%d,%d,%d) dr(%d,%d,%d,%d) mode %d mask %08x",
              s, a.base, a.rowbytes, a.depth, a.bounds.top, a.bounds.left, a.bounds.bottom, a.bounds.right, a.ctab,
              d, b.base, b.depth, b.bounds.top, b.bounds.left, b.bounds.bottom, b.bounds.right,
              sr.top, sr.left, sr.bottom, sr.right, dr.top, dr.left, dr.bottom, dr.right, mode, mrgn);
    }
    struct timespec t0, t1; clock_gettime(CLOCK_MONOTONIC, &t0);
    copybits(s, d, sr, dr, mode, mrgn, 0, (Rect){ 0, 0, 0, 0 });
    if ((int)g_cb_count == dump_n) { dump_surface(s, "work/shots/cb_src.png"); dump_surface(d, "work/shots/cb_dst.png"); }
    clock_gettime(CLOCK_MONOTONIC, &t1);
    double ms = (t1.tv_sec - t0.tv_sec) * 1e3 + (t1.tv_nsec - t0.tv_nsec) / 1e6;
    if (ms > 5 && g_log_level >= 3) {
        Surf a = {0}, b = {0}; surf_from_bitmap(s, &a); surf_from_bitmap(d, &b);
        LOG_D("slow CopyBits %.1f ms: d%d->d%d sr(%d,%d,%d,%d) dr(%d,%d,%d,%d) mode %d mask %08x vis %u bytes clip %u bytes",
              ms, a.depth, b.depth, sr.top, sr.left, sr.bottom, sr.right, dr.top, dr.left, dr.bottom, dr.right, mode, mrgn,
              mm_handle_size(rd32(qd_port() + PORT_VIS)), mm_handle_size(rd32(qd_port() + PORT_CLIP)));
    }
}

TRAP(CopyMask) {
    u32 s = ARG(0), m = ARG(1), d = ARG(2);
    Rect sr = rd_rect(ARG(3)), mr = rd_rect(ARG(4)), dr = rd_rect(ARG(5));
    copybits(s, d, sr, dr, srcCopy, 0, m, mr);
}

TRAP(CopyDeepMask) {
    u32 s = ARG(0), m = ARG(1), d = ARG(2);
    Rect sr = rd_rect(ARG(3)), mr = rd_rect(ARG(4)), dr = rd_rect(ARG(5));
    s16 mode = ARGS16(6);
    u32 mrgn = ARG(7);
    copybits(s, d, sr, dr, mode, mrgn, m, mr);
}

/* ---- SeedFill / CalcMask on 1-bit images ---- */
static void flood(const u8 *src, int srow, u8 *filled, int w, int h, int sx, int sy) {
    int *stack = malloc(sizeof(int) * (size_t)w * (size_t)h * 2 + 16);
    int sp = 0;
#define BLACK(x, y) ((src[(size_t)(y) * (size_t)srow + (size_t)((x) >> 3)] >> (7 - ((x) & 7))) & 1)
    if (sx < 0 || sy < 0 || sx >= w || sy >= h) { free(stack); return; }
    stack[sp++] = sx; stack[sp++] = sy;
    while (sp) {
        int y = stack[--sp], x = stack[--sp];
        if (x < 0 || y < 0 || x >= w || y >= h) continue;
        if (filled[(size_t)y * (size_t)w + (size_t)x] || BLACK(x, y)) continue;
        filled[(size_t)y * (size_t)w + (size_t)x] = 1;
        stack[sp++] = x + 1; stack[sp++] = y;
        stack[sp++] = x - 1; stack[sp++] = y;
        stack[sp++] = x; stack[sp++] = y + 1;
        stack[sp++] = x; stack[sp++] = y - 1;
    }
#undef BLACK
    free(stack);
}

TRAP(SeedFill) {
    u32 src = ARG(0), dst = ARG(1); s16 srow = ARGS16(2), drow = ARGS16(3), height = ARGS16(4), words = ARGS16(5);
    s16 sh = ARGS16(6), sv = ARGS16(7);
    int w = words * 16, h = height;
    u8 *s = malloc((size_t)srow * (size_t)h);
    gmemcpy_from(s, src, (u32)(srow * h));
    u8 *f = calloc((size_t)w * (size_t)h, 1);
    flood(s, srow, f, w, h, sh, sv);
    for (int y = 0; y < h; y++)
        for (int bx = 0; bx < words * 2; bx++) {
            u8 v = 0;
            for (int k = 0; k < 8; k++) if (f[(size_t)y * (size_t)w + (size_t)(bx * 8 + k)]) v |= (u8)(0x80 >> k);
            wr8(dst + (u32)(y * drow + bx), v);
        }
    free(s); free(f);
}

TRAP(CalcMask) {
    u32 src = ARG(0), dst = ARG(1); s16 srow = ARGS16(2), drow = ARGS16(3), height = ARGS16(4), words = ARGS16(5);
    int w = words * 16, h = height;
    u8 *s = malloc((size_t)srow * (size_t)h);
    gmemcpy_from(s, src, (u32)(srow * h));
    u8 *f = calloc((size_t)w * (size_t)h, 1);
    for (int x = 0; x < w; x++) { flood(s, srow, f, w, h, x, 0); flood(s, srow, f, w, h, x, h - 1); }
    for (int y = 0; y < h; y++) { flood(s, srow, f, w, h, 0, y); flood(s, srow, f, w, h, w - 1, y); }
    for (int y = 0; y < h; y++)
        for (int bx = 0; bx < words * 2; bx++) {
            u8 v = 0;
            for (int k = 0; k < 8; k++) if (!f[(size_t)y * (size_t)w + (size_t)(bx * 8 + k)]) v |= (u8)(0x80 >> k);
            wr8(dst + (u32)(y * drow + bx), v);
        }
    free(s); free(f);
}

TRAP(ScrollRect) {
    Rect r = rd_rect(ARG(0)); s16 dh = ARGS16(1), dv = ARGS16(2); u32 upd = ARG(3);
    u32 port = qd_port();
    Rect dst = { (s16)(r.top + dv), (s16)(r.left + dh), (s16)(r.bottom + dv), (s16)(r.right + dh) };
    Rect d2 = rect_sect(dst, r);
    Rect s2 = { (s16)(d2.top - dv), (s16)(d2.left - dh), (s16)(d2.bottom - dv), (s16)(d2.right - dh) };
    if (!rect_empty(d2)) copybits(port + PORT_BITS, port + PORT_BITS, s2, d2, srcCopy, 0, 0, (Rect){ 0, 0, 0, 0 });
    HRgn a, b, o = { 0, NULL };
    hrgn_rect(&a, r.top, r.left, r.bottom, r.right);
    hrgn_rect(&b, d2.top, d2.left, d2.bottom, d2.right);
    hrgn_op(&o, &a, &b, 2);
    Paint p; paint_back(&p, port);
    draw_hrgn(port, &o, &p, patCopy);
    if (upd) hrgn_to_guest(&o, upd);
    hrgn_free(&a); hrgn_free(&b); hrgn_free(&o);
}
