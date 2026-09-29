/* QuickDraw pictures: PICT v1/v2 interpreter (DrawPicture) and minimal
 * picture recording. */
#include "qd.h"
#include "resources.h"

typedef struct {
    const u8 *d;
    u32 len, p;
    bool v2;
    Rect frame, dst;
    int ovw, ovh;
    Point pen, txloc;
    u32 port;
} Pict;

static u16 g16(Pict *pc) { u16 v = pc->p + 2 <= pc->len ? be16(pc->d + pc->p) : 0; pc->p += 2; return v; }
static u32 g32(Pict *pc) { u32 v = pc->p + 4 <= pc->len ? be32(pc->d + pc->p) : 0; pc->p += 4; return v; }
static u8 g8(Pict *pc) { u8 v = pc->p < pc->len ? pc->d[pc->p] : 0; pc->p++; return v; }
static Rect grect(Pict *pc) { Rect r; r.top = (s16)g16(pc); r.left = (s16)g16(pc); r.bottom = (s16)g16(pc); r.right = (s16)g16(pc); return r; }

static int mapx(Pict *pc, int x) {
    int fw = pc->frame.right - pc->frame.left, dw = pc->dst.right - pc->dst.left;
    return fw ? pc->dst.left + (int)((s64)(x - pc->frame.left) * dw / fw) : x;
}
static int mapy(Pict *pc, int y) {
    int fh = pc->frame.bottom - pc->frame.top, dh = pc->dst.bottom - pc->dst.top;
    return fh ? pc->dst.top + (int)((s64)(y - pc->frame.top) * dh / fh) : y;
}
static Rect maprect(Pict *pc, Rect r) {
    return mkrect(mapy(pc, r.top), mapx(pc, r.left), mapy(pc, r.bottom), mapx(pc, r.right));
}

static void unpackbits(const u8 *src, u32 srclen, u8 *dst, u32 dstlen, int unit) {
    u32 s = 0, d = 0;
    while (s < srclen && d < dstlen) {
        s8 n = (s8)src[s++];
        if (n >= 0) {
            u32 cnt = (u32)(n + 1) * (u32)unit;
            for (u32 i = 0; i < cnt && s < srclen && d < dstlen; i++) dst[d++] = src[s++];
        } else if (n != -128) {
            u32 cnt = (u32)(1 - n);
            u8 v[4];
            for (int k = 0; k < unit; k++) v[k] = s < srclen ? src[s++] : 0;
            for (u32 i = 0; i < cnt; i++) for (int k = 0; k < unit && d < dstlen; k++) dst[d++] = v[k];
        }
    }
}

/* Decode the pixel data of a Bits/PackBits/DirectBits opcode into a guest
   pixmap and CopyBits it into the port. */
static void bits_op(Pict *pc, u16 op) {
    bool direct = op == 0x9A || op == 0x9B;
    bool packed = op == 0x98 || op == 0x99 || direct;
    bool has_rgn = op & 1;
    if (direct) g32(pc); /* baseAddr placeholder */
    u16 rb = g16(pc);
    bool pixmap = (rb & 0x8000) != 0;
    int rowbytes = rb & 0x3FFF;
    Rect bounds = grect(pc);
    int depth = 1, packtype = 0, cmpcount = 1;
    u32 ctab = 0;
    if (pixmap) {
        g16(pc); /* pmVersion */
        packtype = g16(pc);
        g32(pc); g32(pc); g32(pc); /* packSize hRes vRes */
        g16(pc); /* pixelType */
        depth = g16(pc);
        cmpcount = g16(pc);
        g16(pc); /* cmpSize */
        g32(pc); g32(pc); g32(pc); /* planeBytes pmTable pmReserved */
        if (!direct) {
            u32 seed = g32(pc); u16 flags = g16(pc); int n = (s16)g16(pc) + 1;
            (void)seed;
            if (n < 0) n = 0;
            if (n > 256) n = 256;
            ctab = mm_new_handle(8 + 8 * (u32)n, true, ZONE_SYS);
            u32 cp = hderef(ctab);
            wr32(cp, 0x7000); wr16(cp + 4, flags); wr16(cp + 6, (u16)(n - 1));
            for (int i = 0; i < n; i++) {
                u16 v = g16(pc), r = g16(pc), g = g16(pc), b = g16(pc);
                wr16(cp + 8 + 8 * (u32)i, (flags & 0x8000) ? (u16)i : v);
                wr16(cp + 10 + 8 * (u32)i, r); wr16(cp + 12 + 8 * (u32)i, g); wr16(cp + 14 + 8 * (u32)i, b);
            }
        }
    }
    Rect sr = grect(pc), dr = grect(pc);
    s16 mode = (s16)g16(pc);
    u32 maskrgn = 0;
    if (has_rgn) {
        u32 at = pc->p;
        u16 size = g16(pc);
        maskrgn = mm_new_handle(size, false, ZONE_SYS);
        gmemcpy_to(hderef(maskrgn), pc->d + at, size);
        /* map region coordinates */
        HRgn r; hrgn_from_guest(&r, maskrgn);
        Rect bb; hrgn_bbox(&r, &bb);
        (void)bb;
        hrgn_free(&r);
        pc->p = at + size;
    }
    int h = bounds.bottom - bounds.top, w = bounds.right - bounds.left;
    if (h <= 0 || w <= 0) goto out;
    /* output buffer depth: indexed stays, 16/32 stay */
    int outdepth = depth;
    int outrb = rowbytes;
    if (direct && depth == 32) outrb = w * 4;
    if (direct && depth == 16) outrb = w * 2;
    if (!pixmap) { outdepth = 1; }
    u32 pix = mm_new_ptr((u32)(outrb * h) + 16, true, ZONE_SYS);
    u8 *row = malloc((size_t)(rowbytes > outrb ? rowbytes : outrb) * 4 + 16);
    for (int y = 0; y < h; y++) {
        if (!packed || rowbytes < 8 || (direct && packtype == 1)) {
            u32 n = (u32)rowbytes;
            if (pc->p + n > pc->len) break;
            gmemcpy_to(pix + (u32)(y * outrb), pc->d + pc->p, n < (u32)outrb ? n : (u32)outrb);
            pc->p += n;
            continue;
        }
        u32 bc = (rowbytes > 250) ? g16(pc) : g8(pc);
        if (pc->p + bc > pc->len) break;
        const u8 *src = pc->d + pc->p;
        pc->p += bc;
        if (direct && depth == 16) {
            unpackbits(src, bc, row, (u32)(w * 2), packtype == 3 ? 2 : 1);
            gmemcpy_to(pix + (u32)(y * outrb), row, (u32)(w * 2));
        } else if (direct && depth == 32) {
            int planes = cmpcount;
            if (packtype == 4 || packtype == 0) {
                unpackbits(src, bc, row, (u32)(w * planes), 1);
                for (int x = 0; x < w; x++) {
                    int o = planes == 4 ? w : 0;
                    u8 r = row[o + x], g = row[o + w + x], b = row[o + 2 * w + x];
                    wr32(pix + (u32)(y * outrb + 4 * x), (u32)r << 16 | (u32)g << 8 | b);
                }
            } else {
                /* packtype 2: RGB triples, no padding byte */
                for (int x = 0; x < w && (u32)(3 * x + 2) < bc; x++)
                    wr32(pix + (u32)(y * outrb + 4 * x), (u32)src[3 * x] << 16 | (u32)src[3 * x + 1] << 8 | src[3 * x + 2]);
            }
        } else {
            unpackbits(src, bc, row, (u32)rowbytes, 1);
            gmemcpy_to(pix + (u32)(y * outrb), row, (u32)rowbytes);
        }
    }
    free(row);
    /* temporary PixMap/BitMap */
    u32 bm = mm_new_ptr(PM_SIZE, true, ZONE_SYS);
    wr32(bm + PM_BASE, pix);
    wr_rect(bm + PM_BOUNDS, bounds);
    if (pixmap) {
        wr16(bm + PM_ROWBYTES, (u16)(0x8000 | outrb));
        wr16(bm + PM_PIXSIZE, (u16)outdepth);
        wr16(bm + PM_PIXTYPE, direct ? 16 : 0);
        wr16(bm + PM_CMPCOUNT, direct ? 3 : 1);
        wr16(bm + PM_CMPSIZE, (u16)(depth == 32 ? 8 : depth == 16 ? 5 : depth));
        wr32(bm + PM_TABLE, ctab);
    } else {
        wr16(bm + 4, (u16)outrb);
    }
    Rect mdr = maprect(pc, dr);
    if (maskrgn) {
        HRgn r; hrgn_from_guest(&r, maskrgn);
        /* regions are in picture coordinates: map by offset/scale of bbox */
        Rect bb; hrgn_bbox(&r, &bb);
        int fw = pc->frame.right - pc->frame.left, fh = pc->frame.bottom - pc->frame.top;
        int dw = pc->dst.right - pc->dst.left, dh = pc->dst.bottom - pc->dst.top;
        if (fw == dw && fh == dh) hrgn_offset(&r, pc->dst.left - pc->frame.left, pc->dst.top - pc->frame.top);
        hrgn_to_guest(&r, maskrgn);
        hrgn_free(&r);
    }
    copybits(bm, pc->port + PORT_BITS, sr, mdr, mode, maskrgn, 0, (Rect){ 0, 0, 0, 0 });
    mm_dispose_ptr(bm);
    mm_dispose_ptr(pix);
out:
    if (ctab) mm_dispose_handle(ctab);
    if (maskrgn) mm_dispose_handle(maskrgn);
}

static void pict_text(Pict *pc, int n) {
    u8 buf[256];
    for (int i = 0; i < n; i++) buf[i] = g8(pc);
    u32 port = pc->port;
    wr16(port + PORT_PNLOC + 2, (u16)mapx(pc, pc->txloc.h));
    wr16(port + PORT_PNLOC, (u16)mapy(pc, pc->txloc.v));
    text_draw(port, buf, n);
}

static void shape_op(Pict *pc, int kind, int verb, Rect r) {
    u32 port = pc->port;
    Rect m = maprect(pc, r);
    HRgn shape;
    if (kind == 0) hrgn_rect(&shape, m.top, m.left, m.bottom, m.right);
    else {
        /* ovals and round rects: approximate via rect region for frames */
        hrgn_rect(&shape, m.top, m.left, m.bottom, m.right);
    }
    Paint p;
    switch (verb) {
    case 0: { /* frame */
        HRgn in, f = { 0, NULL };
        hrgn_rect(&in, m.top + 1, m.left + 1, m.bottom - 1, m.right - 1);
        hrgn_op(&f, &shape, &in, 2);
        paint_pen(&p, port);
        draw_hrgn(port, &f, &p, rds16(port + PORT_PNMODE));
        hrgn_free(&in); hrgn_free(&f);
        break;
    }
    case 1: paint_pen(&p, port); draw_hrgn(port, &shape, &p, rds16(port + PORT_PNMODE)); break;
    case 2: paint_back(&p, port); draw_hrgn(port, &shape, &p, patCopy); break;
    case 3: qd_invert_hrgn(port, &shape); break;
    case 4: paint_pen(&p, port); draw_hrgn(port, &shape, &p, patCopy); break;
    }
    hrgn_free(&shape);
}

static void skip_pixpat(Pict *pc) {
    u16 type = g16(pc);
    pc->p += 8; /* pat1Data */
    if (type == 1) {
        u16 rb = g16(pc) & 0x3FFF;
        Rect b = grect(pc);
        pc->p += 36; /* rest of pixmap */
        g32(pc); g16(pc); int n = (s16)g16(pc) + 1; pc->p += 8 * (u32)n;
        int h = b.bottom - b.top;
        for (int y = 0; y < h; y++) {
            if (rb < 8) pc->p += rb;
            else { u32 bc = rb > 250 ? g16(pc) : g8(pc); pc->p += bc; }
        }
    } else if (type == 2) pc->p += 6;
}

static void run_pict(Pict *pc) {
    u32 port = pc->port;
    Rect last_rect = { 0, 0, 0, 0 };
    for (int guard = 0; guard < 1000000 && pc->p < pc->len; guard++) {
        u16 op;
        if (pc->v2) { if (pc->p & 1) pc->p++; op = g16(pc); }
        else op = g8(pc);
        switch (op) {
        case 0x0000: break;
        case 0x0001: { u32 at = pc->p; u16 size = g16(pc); pc->p = at + size; break; } /* clip: ignored (we clip to dst) */
        case 0x0002: pc->p += 8; break;
        case 0x0003: wr16(port + PORT_TXFONT, g16(pc)); break;
        case 0x0004: wr8(port + PORT_TXFACE, g8(pc)); break;
        case 0x0005: wr16(port + PORT_TXMODE, g16(pc)); break;
        case 0x0006: g32(pc); break;
        case 0x0007: { u16 v = g16(pc), h = g16(pc); wr16(port + PORT_PNSIZE, v); wr16(port + PORT_PNSIZE + 2, h); break; }
        case 0x0008: wr16(port + PORT_PNMODE, g16(pc)); break;
        case 0x0009: case 0x000A: pc->p += 8; break;
        case 0x000B: pc->ovh = (s16)g16(pc); pc->ovw = (s16)g16(pc); break;
        case 0x000C: g16(pc); g16(pc); break;
        case 0x000D: wr16(port + PORT_TXSIZE, g16(pc)); break;
        case 0x000E: case 0x000F: g32(pc); break;
        case 0x0010: pc->p += 8; break;
        case 0x0011: if (pc->v2) g16(pc); else { u8 v = g8(pc); if (v == 2) { pc->v2 = true; pc->p++; } } break;
        case 0x0012: case 0x0013: case 0x0014: skip_pixpat(pc); break;
        case 0x0015: case 0x0016: g16(pc); break;
        case 0x0017: case 0x0018: case 0x0019: break;
        case 0x001A: { RGB c = { g16(pc), g16(pc), g16(pc) }; if (is_color_port(port)) wr_rgb(port + PORT_RGBFG, c); break; }
        case 0x001B: { RGB c = { g16(pc), g16(pc), g16(pc) }; if (is_color_port(port)) wr_rgb(port + PORT_RGBBK, c); break; }
        case 0x001C: break;
        case 0x001D: case 0x001F: pc->p += 6; break;
        case 0x001E: break;
        case 0x0020: pc->pen.v = (s16)g16(pc); pc->pen.h = (s16)g16(pc); pc->pen.v = (s16)g16(pc); pc->pen.h = (s16)g16(pc); break;
        case 0x0021: pc->pen.v = (s16)g16(pc); pc->pen.h = (s16)g16(pc); break;
        case 0x0022: pc->p += 6; break;
        case 0x0023: pc->p += 2; break;
        case 0x0028: pc->txloc.v = (s16)g16(pc); pc->txloc.h = (s16)g16(pc); pict_text(pc, g8(pc)); break;
        case 0x0029: pc->txloc.h = (s16)(pc->txloc.h + g8(pc)); pict_text(pc, g8(pc)); break;
        case 0x002A: pc->txloc.v = (s16)(pc->txloc.v + g8(pc)); pict_text(pc, g8(pc)); break;
        case 0x002B: pc->txloc.h = (s16)(pc->txloc.h + g8(pc)); pc->txloc.v = (s16)(pc->txloc.v + g8(pc)); pict_text(pc, g8(pc)); break;
        case 0x002C: case 0x002D: case 0x002E: case 0x002F: { u16 n = g16(pc); pc->p += n; break; }
        case 0x0030: case 0x0031: case 0x0032: case 0x0033: case 0x0034:
            last_rect = grect(pc); shape_op(pc, 0, op - 0x30, last_rect); break;
        case 0x0038: case 0x0039: case 0x003A: case 0x003B: case 0x003C:
            shape_op(pc, 0, op - 0x38, last_rect); break;
        case 0x0040: case 0x0041: case 0x0042: case 0x0043: case 0x0044:
        case 0x0050: case 0x0051: case 0x0052: case 0x0053: case 0x0054:
            last_rect = grect(pc); shape_op(pc, 1, op & 7, last_rect); break;
        case 0x0048: case 0x0049: case 0x004A: case 0x004B: case 0x004C:
        case 0x0058: case 0x0059: case 0x005A: case 0x005B: case 0x005C:
            shape_op(pc, 1, op & 7, last_rect); break;
        case 0x0090: case 0x0091: case 0x0098: case 0x0099: case 0x009A: case 0x009B:
            bits_op(pc, op); break;
        case 0x00A0: g16(pc); break;
        case 0x00A1: { g16(pc); u16 n = g16(pc); pc->p += n; break; }
        case 0x00FF: return;
        case 0x0C00: pc->p += 24; break;
        default:
            if (op >= 0x0024 && op <= 0x0027) { u16 n = g16(pc); pc->p += n; }
            else if ((op >= 0x0035 && op <= 0x0037) || (op >= 0x0045 && op <= 0x0047) || (op >= 0x0055 && op <= 0x0057)) pc->p += 8;
            else if (op >= 0x0060 && op <= 0x0067) pc->p += 12;
            else if (op >= 0x0068 && op <= 0x006F) pc->p += 4;
            else if ((op >= 0x0070 && op <= 0x0077) || (op >= 0x0080 && op <= 0x0087)) { u32 at = pc->p; u16 n = g16(pc); pc->p = at + n; }
            else if ((op >= 0x003D && op <= 0x003F) || (op >= 0x004D && op <= 0x004F) || (op >= 0x005D && op <= 0x005F) ||
                     (op >= 0x0078 && op <= 0x007F) || (op >= 0x0088 && op <= 0x008F)) { }
            else if ((op >= 0x0092 && op <= 0x0097) || (op >= 0x009C && op <= 0x009F) || (op >= 0x00A2 && op <= 0x00AF)) { u16 n = g16(pc); pc->p += n; }
            else if (op >= 0x00B0 && op <= 0x00CF) { }
            else if (op >= 0x00D0 && op <= 0x00FE) { u32 n = g32(pc); pc->p += n; }
            else if (op >= 0x0100 && op <= 0x7FFF) pc->p += (u32)(op >> 8) * 2;
            else if (op >= 0x8000 && op <= 0x80FF) { }
            else if (op >= 0x8100) { u32 n = g32(pc); pc->p += n; if (op == 0x8200) LOG_W("PICT: QuickTime-compressed image skipped"); }
            else { LOG_W("PICT: unknown opcode %04x", op); return; }
        }
    }
}

void pict_draw(u32 pich, Rect dst) {
    if (!pich || !hderef(pich)) return;
    u32 len = mm_handle_size(pich);
    u8 *d = malloc(len);
    gmemcpy_from(d, hderef(pich), len);
    Pict pc = { .d = d, .len = len, .p = 10, .dst = dst, .port = qd_port() };
    pc.frame = (Rect){ (s16)be16(d + 2), (s16)be16(d + 4), (s16)be16(d + 6), (s16)be16(d + 8) };
    if (len >= 14 && be16(d + 10) == 0x0011 && be16(d + 12) == 0x02FF) { pc.v2 = true; }
    /* save port state that the picture changes */
    u32 port = pc.port;
    u8 save[PORT_SIZE];
    gmemcpy_from(save, port, PORT_SIZE);
    run_pict(&pc);
    /* restore text/pen state (not the bits) */
    gmemcpy_to(port + PORT_PNLOC, save + PORT_PNLOC, PORT_GRAFPROCS - PORT_PNLOC);
    if (is_color_port(port)) { gmemcpy_to(port + PORT_RGBFG, save + PORT_RGBFG, 12); }
    free(d);
}

TRAP(DrawPicture) { pict_draw(ARG(0), rd_rect(ARG(1))); }
TRAP(GetPicture) { RET(res_get(FOURCC('P','I','C','T'), ARGS16(0))); }
TRAP(KillPicture) {
    u32 h = ARG(0);
    if (!h) return;
    if (mm_hgetstate(h) & HS_RESOURCE) res_release(h);
    else mm_dispose_handle(h);
}

/* ---- Picture recording ----
   Drawing into a port with an open picture is appended as v2 opcodes to a
   host buffer (text, lines, rects and the state they depend on). The pen is
   hidden while recording, so nothing reaches the port itself. */
static struct {
    u32 port, h;
    u8 *b; u32 n, cap;
    bool sync;                 /* recorded state below is valid */
    s16 font, size, mode, pnmode, pnh, pnw;
    u8 face;
    RGB fg, bk;
    bool warned;
} R;

static void rec_bytes(const void *d, u32 n) {
    if (R.n + n > R.cap) { R.cap = (R.n + n) * 2 + 256; R.b = realloc(R.b, R.cap); }
    memcpy(R.b + R.n, d, n); R.n += n;
}
static void rec16(u16 v) { u8 b[2]; put_be16(b, v); rec_bytes(b, 2); }
static void rec8(u8 v) { rec_bytes(&v, 1); }
static void rec_pad(void) { if (R.n & 1) rec8(0); }
static void rec_rect(Rect r) { rec16((u16)r.top); rec16((u16)r.left); rec16((u16)r.bottom); rec16((u16)r.right); }
static bool rgb_eq(RGB a, RGB b) { return a.r == b.r && a.g == b.g && a.b == b.b; }

bool pict_recording(u32 port) { return R.h && port == R.port; }

static void rec_colors(u32 port) {
    if (!is_color_port(port)) return;
    RGB fg, bk; rd_rgb(port + PORT_RGBFG, &fg); rd_rgb(port + PORT_RGBBK, &bk);
    if (!R.sync || !rgb_eq(fg, R.fg)) { rec16(0x001A); rec16(fg.r); rec16(fg.g); rec16(fg.b); R.fg = fg; }
    if (!R.sync || !rgb_eq(bk, R.bk)) { rec16(0x001B); rec16(bk.r); rec16(bk.g); rec16(bk.b); R.bk = bk; }
}
static void rec_text_state(u32 port) {
    s16 font = rds16(port + PORT_TXFONT), size = rds16(port + PORT_TXSIZE), mode = rds16(port + PORT_TXMODE);
    u8 face = rd8(port + PORT_TXFACE);
    if (!R.sync || font != R.font) { rec16(0x0003); rec16((u16)font); R.font = font; }
    if (!R.sync || face != R.face) { rec16(0x0004); rec8(face); rec_pad(); R.face = face; }
    if (!R.sync || mode != R.mode) { rec16(0x0005); rec16((u16)mode); R.mode = mode; }
    if (!R.sync || size != R.size) { rec16(0x000D); rec16((u16)size); R.size = size; }
}
static void rec_pen_state(u32 port) {
    s16 pnh = rds16(port + PORT_PNSIZE), pnw = rds16(port + PORT_PNSIZE + 2), pnmode = rds16(port + PORT_PNMODE);
    if (!R.sync || pnh != R.pnh || pnw != R.pnw) { rec16(0x0007); rec16((u16)pnh); rec16((u16)pnw); R.pnh = pnh; R.pnw = pnw; }
    if (!R.sync || pnmode != R.pnmode) { rec16(0x0008); rec16((u16)pnmode); R.pnmode = pnmode; }
}

void pict_rec_text(u32 port, int h, int v, const u8 *str, int n) {
    rec_text_state(port); rec_colors(port); R.sync = true;
    /* LongText carries at most 255 characters; callers pass chunks */
    if (n > 255) n = 255;
    rec16(0x0028); rec16((u16)v); rec16((u16)h); rec8((u8)n); rec_bytes(str, (u32)n); rec_pad();
}
void pict_rec_rect(u32 port, int verb, Rect r) {
    rec_pen_state(port); rec_colors(port); R.sync = true;
    rec16((u16)(0x0030 + verb)); rec_rect(r);
}
void pict_rec_line(u32 port, Point a, Point b) {
    rec_pen_state(port); rec_colors(port); R.sync = true;
    rec16(0x0020); rec16((u16)a.v); rec16((u16)a.h); rec16((u16)b.v); rec16((u16)b.h);
}
void pict_rec_unsupported(u32 port, const char *what) {
    if (!pict_recording(port) || R.warned) return;
    R.warned = true;
    LOG_W("picture recording: %s not recorded", what);
}

TRAP(OpenPicture) {
    Rect f = rd_rect(ARG(0));
    u32 port = qd_port();
    free(R.b);
    memset(&R, 0, sizeof R);
    R.port = port;
    R.h = mm_new_handle(0, false, ZONE_APP);
    /* v2 extended header */
    rec16(0); rec_rect(f);
    rec16(0x0011); rec16(0x02FF);
    rec16(0x0C00); rec16(0xFFFE); rec16(0);
    rec16(72); rec16(0); rec16(72); rec16(0);
    rec_rect(f); rec16(0); rec16(0);
    rec16(0x001E); /* DefHilite */
    rec16(0x0001); rec16(10); rec_rect(f); /* clip */
    wr32(port + PORT_PICSAVE, R.h);
    wr16(port + PORT_PNVIS, (u16)(rds16(port + PORT_PNVIS) - 1));
    RET(R.h);
}
TRAP(ClosePicture) {
    if (!R.h) return;
    u32 port = R.port;
    rec16(0x00FF);
    put_be16(R.b, (u16)R.n); /* picSize: low 16 bits */
    if (mm_set_handle_size(R.h, R.n)) gmemcpy_to(hderef(R.h), R.b, R.n);
    wr32(port + PORT_PICSAVE, 0);
    wr16(port + PORT_PNVIS, (u16)(rds16(port + PORT_PNVIS) + 1));
    free(R.b);
    memset(&R, 0, sizeof R);
}
TRAP(PicComment) { }

/* Debug/test helper: render a PICT file (data fork with 512-byte header)
   to PNG through our own QuickDraw. */
#include "../host/host.h"
int pict_render_file(const char *in, const char *out) {
    FILE *f = fopen(in, "rb");
    if (!f) return 1;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    u8 *d = malloc((size_t)n);
    if (fread(d, 1, (size_t)n, f) != (size_t)n) { fclose(f); return 1; }
    fclose(f);
    u32 h = mm_handle_from_data(d + 512, (u32)(n - 512), ZONE_APP);
    free(d);
    Rect fr = rd_rect(hderef(h) + 2);
    int w = fr.right - fr.left, hh = fr.bottom - fr.top;
    /* draw into the screen port */
    extern u32 wm_port(void);
    u32 port = wm_port();
    qd_set_port(port);
    Rect dst = mkrect(0, 0, hh > qd_screen_h() ? qd_screen_h() : hh, w > qd_screen_w() ? qd_screen_w() : w);
    rgn_set_rect(rd32(port + PORT_VIS), mkrect(0, 0, qd_screen_h(), qd_screen_w()));
    pict_draw(h, mkrect(0, 0, hh, w));
    (void)dst;
    qd_present();
    return host_screenshot(out) ? 0 : 1;
}
