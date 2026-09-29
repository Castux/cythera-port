/* Cursors and color icons. */
#include "qd.h"
#include "resources.h"
#include "mm.h"
#include "../host/host.h"

static int g_hide_level;
static u8 g_cur_rgba[16 * 16 * 4];
static int g_hotx, g_hoty;

static void apply_cursor(void) { host_set_cursor(g_cur_rgba, g_hotx, g_hoty, g_hide_level >= 0); }

static void set_bw_cursor(u32 crs) {
    for (int y = 0; y < 16; y++) {
        u16 d = rd16(crs + 2 * (u32)y), m = rd16(crs + 32 + 2 * (u32)y);
        for (int x = 0; x < 16; x++) {
            bool db = (d >> (15 - x)) & 1, mb = (m >> (15 - x)) & 1;
            u8 *p = &g_cur_rgba[(y * 16 + x) * 4];
            if (mb) { u8 v = db ? 0 : 255; p[0] = p[1] = p[2] = v; p[3] = 255; }
            else if (db) { p[0] = p[1] = p[2] = 0; p[3] = 255; } /* inverted pixel: show black */
            else { p[0] = p[1] = p[2] = 0; p[3] = 0; }
        }
    }
    g_hoty = rds16(crs + 64); g_hotx = rds16(crs + 66);
    apply_cursor();
}

TRAP(InitCursor) {
    extern u32 g_qd_theport_ptr;
    g_hide_level = 0;
    if (g_qd_theport_ptr) set_bw_cursor(g_qd_theport_ptr - 108);
}
TRAP(SetCursor) { set_bw_cursor(ARG(0)); }
/* The System file's standard cursors (iBeamCursor, crossCursor, plusCursor,
   watchCursor), redrawn: there is no System file. data, mask, hotspot v,h. */
static const u16 k_sys_curs[4][34] = {
    { 0x0C60, 0x0280, 0x0100, 0x0100, 0x0100, 0x0100, 0x0100, 0x0100,
      0x0100, 0x0100, 0x0100, 0x0100, 0x0100, 0x0100, 0x0280, 0x0C60,
      0x1EF0, 0x07C0, 0x0380, 0x0380, 0x0380, 0x0380, 0x0380, 0x0380,
      0x0380, 0x0380, 0x0380, 0x0380, 0x0380, 0x0380, 0x07C0, 0x1EF0, 11, 7 },
    { 0x0400, 0x0400, 0x0400, 0x0400, 0x0400, 0xFFE0, 0x0400, 0x0400,
      0x0400, 0x0400, 0x0400, 0, 0, 0, 0, 0,
      0x0E00, 0x0E00, 0x0E00, 0x0E00, 0xFFE0, 0xFFE0, 0xFFE0, 0x0E00,
      0x0E00, 0x0E00, 0x0E00, 0x0E00, 0, 0, 0, 0, 5, 5 },
    { 0, 0x0E00, 0x0A00, 0x0A00, 0x0A00, 0xFBE0, 0x8020, 0xFBE0,
      0x0A00, 0x0A00, 0x0A00, 0x0E00, 0, 0, 0, 0,
      0, 0x0E00, 0x0E00, 0x0E00, 0x0E00, 0xFFE0, 0xFFE0, 0xFFE0,
      0x0E00, 0x0E00, 0x0E00, 0x0E00, 0, 0, 0, 0, 6, 6 },
    { 0x3F00, 0x3F00, 0x3F00, 0x3F00, 0x4080, 0x8440, 0x8440, 0x8460,
      0x9C40, 0x8040, 0x8040, 0x4080, 0x3F00, 0x3F00, 0x3F00, 0x3F00,
      0x3F00, 0x3F00, 0x3F00, 0x3F00, 0x7F80, 0xFFC0, 0xFFC0, 0xFFE0,
      0xFFC0, 0xFFC0, 0xFFC0, 0x7F80, 0x3F00, 0x3F00, 0x3F00, 0x3F00, 8, 8 },
};
TRAP(GetCursor) {
    s16 id = ARGS16(0);
    u32 h = res_get(FOURCC('C','U','R','S'), id);
    if (!h && id >= 1 && id <= 4) {
        static u32 sys[4];
        if (!sys[id - 1]) {
            u8 b[68];
            for (int i = 0; i < 34; i++) put_be16(b + 2 * i, k_sys_curs[id - 1][i]);
            sys[id - 1] = mm_handle_from_data(b, 68, ZONE_SYS);
        }
        h = sys[id - 1];
    }
    RET(h);
}
TRAP(HideCursor) { g_hide_level--; apply_cursor(); }
TRAP(ShowCursor) { if (g_hide_level < 0) g_hide_level++; apply_cursor(); }
TRAP(ObscureCursor) { }
TRAP(ShieldCursor) { }

/* 'crsr' resources are kept as their raw resource image in a handle; the
   offsets inside remain relative to the start, which is what SetCCursor
   interprets. */
TRAP(GetCCursor) { RET(res_get(FOURCC('c','r','s','r'), ARGS16(0))); }
TRAP(DisposeCCursor) { }

TRAP(SetCCursor) {
    u32 h = ARG(0);
    if (!h || !hderef(h)) return;
    u32 p = hderef(h);
    u32 len = mm_handle_size(h);
    u32 pmoff = rd32(p + 2), dataoff = rd32(p + 6);
    if (pmoff == 0 || pmoff + PM_SIZE > len) { set_bw_cursor(p + 20); return; }
    u32 pm = p + pmoff;
    int rb = rd16(pm + 4) & 0x3FFF;
    int depth = rd16(pm + 32);
    u32 ctoff = rd32(pm + 42);
    for (int y = 0; y < 16; y++) {
        u16 m = rd16(p + 52 + 2 * (u32)y);
        u16 bw = rd16(p + 20 + 2 * (u32)y);
        for (int x = 0; x < 16; x++) {
            u8 *o = &g_cur_rgba[(y * 16 + x) * 4];
            bool mb = (m >> (15 - x)) & 1;
            u32 v = 0;
            u32 row = p + dataoff + (u32)(y * rb);
            if (depth == 8) v = rd8(row + (u32)x);
            else if (depth == 4) v = (rd8(row + (u32)(x >> 1)) >> (4 - 4 * (x & 1))) & 15;
            else if (depth == 2) v = (rd8(row + (u32)(x >> 2)) >> (6 - 2 * (x & 3))) & 3;
            else v = (rd8(row + (u32)(x >> 3)) >> (7 - (x & 7))) & 1;
            u16 r = 0, g = 0, b = 0;
            if (ctoff && ctoff < len) {
                u32 ct = p + ctoff;
                int n = (s16)rd16(ct + 6) + 1;
                for (int i = 0; i < n; i++) if ((rd16(ct + 8 + 8 * (u32)i) & 0xFF) == v || ((rd16(ct + 4) & 0x8000) && (u32)i == v)) {
                    r = rd16(ct + 10 + 8 * (u32)i); g = rd16(ct + 12 + 8 * (u32)i); b = rd16(ct + 14 + 8 * (u32)i); break; }
            }
            if (mb) { o[0] = (u8)(r >> 8); o[1] = (u8)(g >> 8); o[2] = (u8)(b >> 8); o[3] = 255; }
            else if ((bw >> (15 - x)) & 1) { o[0] = o[1] = o[2] = 0; o[3] = 255; }
            else o[3] = 0;
        }
    }
    g_hoty = rds16(p + 84); g_hotx = rds16(p + 86);
    apply_cursor();
}

/* ---- color icons ('cicn') ---- */
/* CIcon: iconPMap(50) iconMask(14) iconBMap(14) iconData(4) iconMaskData[] */
TRAP(GetCIcon) {
    s16 id = ARGS16(0);
    u32 len;
    u8 *d = res_load_raw(FOURCC('c','i','c','n'), id, &len);
    if (!d || len < 82) { free(d); RET(0); return; }
    int prb = be16(d + 4) & 0x3FFF;
    Rect pb = { (s16)be16(d + 6), (s16)be16(d + 8), (s16)be16(d + 10), (s16)be16(d + 12) };
    int mrb = be16(d + 54), brb = be16(d + 68);
    int h = pb.bottom - pb.top;
    u32 masklen = (u32)(mrb * h), bmaplen = (u32)(brb * h);
    u32 ctoff = 82 + masklen + bmaplen;
    int n = (s16)be16(d + ctoff + 6) + 1;
    u32 ctlen = 8 + 8 * (u32)n;
    u32 pixoff = ctoff + ctlen, pixlen = (u32)(prb * h);
    if (pixoff + pixlen > len) { free(d); RET(0); return; }
    u32 ch = mm_new_handle(82 + masklen + bmaplen, true, ZONE_APP);
    u32 cp = hderef(ch);
    gmemcpy_to(cp, d, 82 + masklen + bmaplen);
    u32 ct = mm_handle_from_data(d + ctoff, ctlen, ZONE_APP);
    u32 dh = mm_handle_from_data(d + pixoff, pixlen, ZONE_APP);
    wr32(cp + PM_TABLE, ct);
    wr32(cp + 78, dh);
    free(d);
    RET(ch);
}

TRAP(DisposeCIcon) {
    u32 h = ARG(0);
    if (!h || !hderef(h)) return;
    u32 p = hderef(h);
    if (rd32(p + PM_TABLE)) mm_dispose_handle(rd32(p + PM_TABLE));
    if (rd32(p + 78)) mm_dispose_handle(rd32(p + 78));
    mm_dispose_handle(h);
}

static void plot_cicon(u32 h, Rect dst, bool disabled) {
    if (!h || !hderef(h)) return;
    u32 p = hderef(h);
    Rect pb = rd_rect(p + 6);
    int mrb = rd16(p + 54);
    int ih = pb.bottom - pb.top;
    /* build temporary pixmap and mask bitmaps in guest memory */
    u32 pm = mm_new_ptr(PM_SIZE, false, ZONE_SYS);
    gmemmove(pm, p, PM_SIZE);
    wr32(pm, hderef(rd32(p + 78)));
    u32 mask = mm_new_ptr(14, false, ZONE_SYS);
    wr32(mask, p + 82);
    wr16(mask + 4, (u16)mrb);
    wr_rect(mask + 6, rd_rect(p + 56));
    (void)ih;
    copybits(pm, qd_port() + PORT_BITS, pb, dst, srcCopy, 0, mask, rd_rect(p + 56));
    if (disabled) {
        /* dim: blend with background through a gray pattern */
        static const u8 gray[8] = { 0xAA, 0x55, 0xAA, 0x55, 0xAA, 0x55, 0xAA, 0x55 };
        Paint pt; paint_from_pattern(&pt, qd_port(), gray);
        pt.fg = port_bk(qd_port());
        draw_rect(qd_port(), dst, &pt, patOr);
    }
    mm_dispose_ptr(pm);
    mm_dispose_ptr(mask);
}

TRAP(PlotCIcon) { plot_cicon(ARG(1), rd_rect(ARG(0)), false); }
TRAP(PlotCIconHandle) {
    Rect r = rd_rect(ARG(0));
    s16 transform = ARGS16(2);
    plot_cicon(ARG(3), r, (transform & 3) == 1);
    RETERR(noErr);
}
