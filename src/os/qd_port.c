/* QuickDraw: ports, devices, color tables, GWorlds, palettes, pen state,
 * patterns and rectangle utilities. */
#include "qd.h"
#include "resources.h"
#include "../host/host.h"
#include "../config.h"

#define LM_MainDevice  0x08A4
#define LM_DeviceList  0x08A8
#define LM_TheGDevice  0x0CC8
#define LM_ScrnBase    0x0824
#define LM_ScreenRow   0x0106
#define LM_HiliteRGB   0x0DA0

u32 g_qd_theport_ptr;
static u32 g_screen_gd, g_screen_pm, g_screen_base, g_screen_ctab;
static int g_sw, g_sh, g_srow;
static bool g_screen_dirty = true;
static u32 g_std_ctab8;       /* standard 8-bit system clut (for GWorlds) */
static u32 g_ctab_seed = 1024;

/* ---------------------------------------------------------------------- */
/* Color tables                                                            */

static void std_clut8(RGB out[256]) {
    static const u16 lv[6] = { 0xFFFF, 0xCCCC, 0x9999, 0x6666, 0x3333, 0x0000 };
    static const u16 ramp[10] = { 0xEEEE, 0xDDDD, 0xBBBB, 0xAAAA, 0x8888, 0x7777, 0x5555, 0x4444, 0x2222, 0x1111 };
    int i = 0;
    for (int r = 0; r < 6; r++)
        for (int g = 0; g < 6; g++)
            for (int b = 0; b < 6; b++) {
                if (i == 215) break;
                out[i++] = (RGB){ lv[r], lv[g], lv[b] };
            }
    for (int k = 0; k < 10; k++) out[215 + k] = (RGB){ ramp[k], 0, 0 };
    for (int k = 0; k < 10; k++) out[225 + k] = (RGB){ 0, ramp[k], 0 };
    for (int k = 0; k < 10; k++) out[235 + k] = (RGB){ 0, 0, ramp[k] };
    for (int k = 0; k < 10; k++) out[245 + k] = (RGB){ ramp[k], ramp[k], ramp[k] };
    out[255] = (RGB){ 0, 0, 0 };
}

static u32 new_ctab(const RGB *c, int n, bool device) {
    u32 h = mm_new_handle(8 + 8 * (u32)n, true, ZONE_SYS);
    u32 p = hderef(h);
    wr32(p, g_ctab_seed++);
    wr16(p + 4, device ? 0x8000 : 0);
    wr16(p + 6, (u16)(n - 1));
    for (int i = 0; i < n; i++) { wr16(p + 8 + 8 * (u32)i, (u16)i); wr_rgb(p + 10 + 8 * (u32)i, c[i]); }
    return h;
}

static u32 default_ctab(int depth) {
    RGB c[256];
    switch (depth) {
    case 1: c[0] = (RGB){ 0xFFFF, 0xFFFF, 0xFFFF }; c[1] = (RGB){ 0, 0, 0 }; return new_ctab(c, 2, false);
    case 2: {
        u16 v[4] = { 0xFFFF, 0xAAAA, 0x5555, 0 };
        for (int i = 0; i < 4; i++) c[i] = (RGB){ v[i], v[i], v[i] };
        return new_ctab(c, 4, false);
    }
    case 4: {
        static const u16 t[16][3] = {
            {0xFFFF,0xFFFF,0xFFFF},{0xFC00,0xF37D,0x052F},{0xFFFF,0x648A,0x028C},{0xDD6B,0x08C2,0x06A2},
            {0xF2D7,0x0856,0x84EC},{0x46E3,0x0000,0xA53E},{0x0000,0x0000,0xD400},{0x0241,0xAB54,0xEAFF},
            {0x1F21,0xB793,0x1431},{0x0000,0x64AF,0x11B0},{0x5600,0x2C9D,0x0524},{0x90D7,0x7160,0x3A34},
            {0xC000,0xC000,0xC000},{0x8000,0x8000,0x8000},{0x4000,0x4000,0x4000},{0x0000,0x0000,0x0000} };
        for (int i = 0; i < 16; i++) c[i] = (RGB){ t[i][0], t[i][1], t[i][2] };
        return new_ctab(c, 16, false);
    }
    default:
        std_clut8(c);
        return new_ctab(c, 256, false);
    }
}

void rd_rgb(u32 a, RGB *c) { c->r = rd16(a); c->g = rd16(a + 2); c->b = rd16(a + 4); }
void wr_rgb(u32 a, RGB c) { wr16(a, c.r); wr16(a + 2, c.g); wr16(a + 4, c.b); }

/* Host cache of color tables keyed by (handle, content hash). */
typedef struct {
    u32 handle, hash;
    int n;
    RGB rgb[256];
    u8 *inv;   /* 32768-entry RGB555 -> index */
} CtCache;
#define CT_CACHE 16
static CtCache g_ct[CT_CACHE];
static int g_ct_next;

static u32 ctab_hash(u32 h) {
    u32 p = hderef(h);
    int n = (s16)rd16(p + 6) + 1;
    if (n < 0) n = 0;
    if (n > 256) n = 256;
    u32 x = 2166136261u ^ rd16(p + 4);
    const u8 *d = gptr(p + 8, 8 * (u32)n);
    for (int i = 0; i < 8 * n; i++) { x ^= d[i]; x *= 16777619u; }
    return x ^ (u32)n;
}

static CtCache *ct_get(u32 h) {
    if (!h || !hderef(h)) return NULL;
    u32 hash = ctab_hash(h);
    for (int i = 0; i < CT_CACHE; i++) if (g_ct[i].handle == h && g_ct[i].hash == hash) return &g_ct[i];
    CtCache *c = &g_ct[g_ct_next];
    g_ct_next = (g_ct_next + 1) % CT_CACHE;
    free(c->inv);
    memset(c, 0, sizeof *c);
    c->handle = h; c->hash = hash;
    u32 p = hderef(h);
    bool dev = rd16(p + 4) & 0x8000;
    int n = (s16)rd16(p + 6) + 1;
    if (n > 256) n = 256;
    if (n < 0) n = 0;
    c->n = n;
    for (int i = 0; i < 256; i++) c->rgb[i] = (RGB){ 0, 0, 0 };
    for (int i = 0; i < n; i++) {
        u16 v = rd16(p + 8 + 8 * (u32)i);
        int idx = dev ? i : (v & 0xFF);
        rd_rgb(p + 10 + 8 * (u32)i, &c->rgb[idx]);
    }
    if (!dev) { /* entries not covered: count = max index + 1 */
        int mx = 0;
        for (int i = 0; i < n; i++) { int v = rd16(p + 8 + 8 * (u32)i) & 0xFF; if (v + 1 > mx) mx = v + 1; }
        c->n = mx > n ? mx : n;
    }
    return c;
}

void ctab_invalidate(u32 h) { for (int i = 0; i < CT_CACHE; i++) if (g_ct[i].handle == h) g_ct[i].handle = 0; }
RGB ctab_color(u32 h, int i) { CtCache *c = ct_get(h); return c && i >= 0 && i < 256 ? c->rgb[i] : (RGB){ 0, 0, 0 }; }
int ctab_count(u32 h) { CtCache *c = ct_get(h); return c ? c->n : 0; }
u32 ctab_seed(u32 h) { return h && hderef(h) ? rd32(hderef(h)) : 0; }

static int nearest(const CtCache *c, int r8, int g8, int b8) {
    int best = 0; long bd = 0x7FFFFFFF;
    for (int i = 0; i < c->n; i++) {
        int dr = (c->rgb[i].r >> 8) - r8, dg = (c->rgb[i].g >> 8) - g8, db = (c->rgb[i].b >> 8) - b8;
        long d = (long)dr * dr * 3 + (long)dg * dg * 4 + (long)db * db * 2;
        if (d < bd) { bd = d; best = i; if (!d) break; }
    }
    return best;
}

int ctab_nearest(u32 h, RGB col) {
    CtCache *c = ct_get(h);
    if (!c || !c->n) return 0;
    /* exact match first */
    for (int i = 0; i < c->n; i++)
        if (c->rgb[i].r == col.r && c->rgb[i].g == col.g && c->rgb[i].b == col.b) return i;
    if (!c->inv) {
        c->inv = malloc(32768);
        memset(c->inv, 0xFF, 32768);
        /* lazily filled: mark unknown as 0xFF and compute on demand below */
    }
    int key = ((col.r >> 11) << 10) | ((col.g >> 11) << 5) | (col.b >> 11);
    u8 v = c->inv[key];
    if (v == 0xFF) {
        int r8 = col.r >> 8, g8 = col.g >> 8, b8 = col.b >> 8;
        int n = nearest(c, r8, g8, b8);
        c->inv[key] = (u8)n;
        return n;
    }
    return v;
}

/* ---------------------------------------------------------------------- */
/* Surfaces                                                                */

bool surf_from_bitmap(u32 bm, Surf *s) {
    if (!bm) return false;
    u16 rb = rd16(bm + 4);
    if ((rb & 0xC000) == 0xC000) {
        /* portBits of a CGrafPort: baseAddr is the PixMapHandle */
        u32 pmh = rd32(bm);
        if (!pmh || !hderef(pmh)) return false;
        bm = hderef(pmh);
        rb = rd16(bm + 4);
    }
    s->base = rd32(bm);
    s->rowbytes = rb & 0x3FFF;
    s->bounds = rd_rect(bm + 6);
    if (rb & 0x8000) {
        s->depth = rd16(bm + PM_PIXSIZE);
        s->ctab = s->depth <= 8 ? rd32(bm + PM_TABLE) : 0;
    } else {
        s->depth = 1;
        s->ctab = 0;
    }
    return s->base != 0;
}

bool is_color_port(u32 port) { return port && (rd16(port + PORT_VERSION) & 0xC000) == 0xC000; }
u32 port_pixmap(u32 port) { return is_color_port(port) ? rd32(port + PORT_BITS) : 0; }
bool surf_from_port(u32 port, Surf *s) { return port && surf_from_bitmap(port + PORT_BITS, s); }

u32 surf_get(const Surf *s, int x, int y) {
    u32 row = s->base + (u32)(y * s->rowbytes);
    switch (s->depth) {
    case 8: return g_mem[row + (u32)x];
    case 1: return (g_mem[row + (u32)(x >> 3)] >> (7 - (x & 7))) & 1;
    case 2: return (g_mem[row + (u32)(x >> 2)] >> (6 - 2 * (x & 3))) & 3;
    case 4: return (g_mem[row + (u32)(x >> 1)] >> (4 - 4 * (x & 1))) & 15;
    case 16: return rd16(row + (u32)(2 * x));
    case 32: return rd32(row + (u32)(4 * x));
    }
    return 0;
}

void surf_put(const Surf *s, int x, int y, u32 v) {
    u32 row = s->base + (u32)(y * s->rowbytes);
    u8 *p;
    switch (s->depth) {
    case 8: g_mem[row + (u32)x] = (u8)v; return;
    case 1: p = &g_mem[row + (u32)(x >> 3)]; *p = (u8)((*p & ~(0x80 >> (x & 7))) | ((v & 1) << (7 - (x & 7)))); return;
    case 2: p = &g_mem[row + (u32)(x >> 2)]; { int sh = 6 - 2 * (x & 3); *p = (u8)((*p & ~(3 << sh)) | ((v & 3) << sh)); } return;
    case 4: p = &g_mem[row + (u32)(x >> 1)]; { int sh = 4 - 4 * (x & 1); *p = (u8)((*p & ~(15 << sh)) | ((v & 15) << sh)); } return;
    case 16: wr16(row + (u32)(2 * x), (u16)v); return;
    case 32: wr32(row + (u32)(4 * x), v); return;
    }
}

u32 pixel_for_rgb(const Surf *s, RGB c) {
    switch (s->depth) {
    case 1: {
        if (s->ctab) return (u32)ctab_nearest(s->ctab, c);
        u32 lum = (u32)c.r * 30 + (u32)c.g * 59 + (u32)c.b * 11;
        return lum >= 0xFFFFu * 100u - 0x100u * 100u ? 0 : 1; /* only white is white */
    }
    case 16: return (u32)((c.r >> 11) << 10 | (c.g >> 11) << 5 | (c.b >> 11));
    case 32: return (u32)((c.r >> 8) << 16 | (c.g >> 8) << 8 | (c.b >> 8));
    default: return s->ctab ? (u32)ctab_nearest(s->ctab, c) : 0;
    }
}

RGB rgb_for_pixel(const Surf *s, u32 px) {
    switch (s->depth) {
    case 1:
        if (s->ctab) return ctab_color(s->ctab, (int)px);
        return px ? (RGB){ 0, 0, 0 } : (RGB){ 0xFFFF, 0xFFFF, 0xFFFF };
    case 16: {
        u16 r = (u16)((px >> 10) & 31), g = (u16)((px >> 5) & 31), b = (u16)(px & 31);
        return (RGB){ (u16)(r << 11 | r << 6 | r << 1), (u16)(g << 11 | g << 6 | g << 1), (u16)(b << 11 | b << 6 | b << 1) };
    }
    case 32: return (RGB){ (u16)(((px >> 16) & 0xFF) * 0x101), (u16)(((px >> 8) & 0xFF) * 0x101), (u16)((px & 0xFF) * 0x101) };
    default: return s->ctab ? ctab_color(s->ctab, (int)px) : (RGB){ 0, 0, 0 };
    }
}

/* ---------------------------------------------------------------------- */
/* Screen device                                                           */

static u32 make_pixmap(u32 base, int rowbytes, Rect bounds, int depth, u32 ctab) {
    u32 h = mm_new_handle(PM_SIZE, true, ZONE_SYS);
    u32 p = hderef(h);
    wr32(p + PM_BASE, base);
    wr16(p + PM_ROWBYTES, (u16)(0x8000 | rowbytes));
    wr_rect(p + PM_BOUNDS, bounds);
    wr16(p + PM_VERSION, 0);
    wr32(p + PM_HRES, 72u << 16); wr32(p + PM_VRES, 72u << 16);
    wr16(p + PM_PIXTYPE, depth > 8 ? 16 : 0);
    wr16(p + PM_PIXSIZE, (u16)depth);
    wr16(p + PM_CMPCOUNT, depth > 8 ? 3 : 1);
    wr16(p + PM_CMPSIZE, (u16)(depth == 16 ? 5 : depth == 32 ? 8 : depth));
    wr32(p + PM_TABLE, ctab);
    return h;
}

static u32 make_gdevice(u32 pmh, Rect r, bool screen) {
    u32 h = mm_new_handle(GD_SIZE, true, ZONE_SYS);
    u32 p = hderef(h);
    wr16(p + GD_REFNUM, screen ? (u16)-50 : 0);
    wr16(p + GD_TYPE, 0); /* clutType */
    wr32(p + GD_ITABLE, mm_new_handle(16, true, ZONE_SYS));
    wr16(p + GD_RESPREF, 4);
    wr16(p + GD_FLAGS, screen ? (0x8000 | 0x2000 | 0x0800 | 0x0400 | 0x0001) : 0x0001);
    wr32(p + GD_PMAP, pmh);
    wr32(p + GD_NEXT, 0);
    wr_rect(p + GD_RECT, r);
    wr32(p + GD_MODE, screen ? 0x83 : 0xFFFFFFFF);
    return h;
}

void qd_init_screen(int w, int h) {
    g_sw = w; g_sh = h;
    g_srow = (w + 3) & ~3;
    g_screen_base = mm_new_ptr((u32)(g_srow * h), true, ZONE_SYS);
    RGB c[256];
    std_clut8(c);
    g_screen_ctab = new_ctab(c, 256, true);
    g_std_ctab8 = default_ctab(8);
    g_screen_pm = make_pixmap(g_screen_base, g_srow, mkrect(0, 0, h, w), 8, g_screen_ctab);
    g_screen_gd = make_gdevice(g_screen_pm, mkrect(0, 0, h, w), true);
    wr32(LM_MainDevice, g_screen_gd);
    wr32(LM_DeviceList, g_screen_gd);
    wr32(LM_TheGDevice, g_screen_gd);
    wr32(LM_ScrnBase, g_screen_base);
    wr16(LM_ScreenRow, (u16)g_srow);
    wr_rgb(LM_HiliteRGB, (RGB){ 0xCCCC, 0xCCCC, 0xFFFF });
    gmemset(g_screen_base, 0, (u32)(g_srow * h)); /* index 0 = white */
}

u32 qd_main_device(void) { return g_screen_gd; }
u32 qd_screen_pixmap(void) { return g_screen_pm; }
u32 qd_screen_base(void) { return g_screen_base; }
int qd_screen_w(void) { return g_sw; }
int qd_screen_h(void) { return g_sh; }
void qd_screen_dirty(void) { g_screen_dirty = true; }
u32 qd_cur_device(void) { u32 d = rd32(LM_TheGDevice); return d ? d : g_screen_gd; }

void menu_draw_overlay(u32 *pal); /* menus.c: nothing, kept for symmetry */

void qd_present(void) {
    static u32 pal[256];
    CtCache *c = ct_get(g_screen_ctab);
    for (int i = 0; i < 256; i++) pal[i] = (u32)(c->rgb[i].r >> 8) << 16 | (u32)(c->rgb[i].g >> 8) << 8 | (c->rgb[i].b >> 8);
    host_present(g_mem + g_screen_base, g_srow, g_sw, g_sh, pal);
    g_screen_dirty = false;
}

/* Set entries of the screen clut. */
void qd_set_device_clut(u32 src_ctab, int start, int count, bool use_value) {
    (void)use_value;
    u32 dp = hderef(g_screen_ctab);
    CtCache *c = ct_get(src_ctab);
    if (!c) return;
    for (int i = 0; i < count && start + i < 256; i++) wr_rgb(dp + 10 + 8 * (u32)(start + i), c->rgb[i]);
    wr32(dp, g_ctab_seed++);
    qd_screen_dirty();
}

/* ---------------------------------------------------------------------- */
/* Ports                                                                   */

u32 qd_port(void) { return g_qd_theport_ptr ? rd32(g_qd_theport_ptr) : 0; }
void qd_set_port(u32 port) { if (g_qd_theport_ptr) wr32(g_qd_theport_ptr, port); }

u32 new_pixpat_from_pattern(const u8 pat[8]) {
    u32 h = mm_new_handle(PP_SIZE, true, ZONE_APP);
    u32 p = hderef(h);
    wr16(p + PP_TYPE, 0);
    wr32(p + PP_MAP, make_pixmap(0, 0, mkrect(0, 0, 8, 8), 1, 0));
    wr32(p + PP_DATA, mm_new_handle(0, false, ZONE_APP));
    wr16(p + PP_XVALID, (u16)-1);
    gmemcpy_to(p + PP_PAT1, pat, 8);
    return h;
}

static void set_pixpat_pattern(u32 port, int field, const u8 pat[8]) {
    u32 h = rd32(port + (u32)field);
    if (h && hderef(h) && rd16(hderef(h) + PP_TYPE) == 0) {
        gmemcpy_to(hderef(h) + PP_PAT1, pat, 8);
        return;
    }
    wr32(port + (u32)field, new_pixpat_from_pattern(pat));
}

static const u8 PAT_BLACK[8] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
static const u8 PAT_WHITE[8] = { 0 };

void port_init_color_state(u32 port) {
    wr_rgb(port + PORT_RGBFG, (RGB){ 0, 0, 0 });
    wr_rgb(port + PORT_RGBBK, (RGB){ 0xFFFF, 0xFFFF, 0xFFFF });
    wr32(port + PORT_BKPAT, new_pixpat_from_pattern(PAT_WHITE));
    wr32(port + PORT_PNPAT, new_pixpat_from_pattern(PAT_BLACK));
    wr32(port + PORT_FILLPIXPAT, new_pixpat_from_pattern(PAT_BLACK));
}

/* Window ports keep portBits.bounds.topLeft mirrored at offsets 8/10 (where
   an old GrafPort has it), because application WDEFs read it there; their
   grafVars handle is kept here instead. */
typedef struct { u32 port, gv; } WinGV;
static WinGV g_wgv[256];

u32 port_grafvars(u32 port) {
    for (int i = 0; i < 256; i++) if (g_wgv[i].port == port) return g_wgv[i].gv;
    return rd32(port + PORT_GRAFVARS);
}
void port_make_window_port(u32 port) {
    u32 gv = rd32(port + PORT_GRAFVARS);
    for (int i = 0; i < 256; i++) if (g_wgv[i].port == port || !g_wgv[i].port) { g_wgv[i].port = port; g_wgv[i].gv = gv; break; }
    port_mirror_bounds(port);
}
void port_mirror_bounds(u32 port) {
    if (!is_color_port(port)) return;
    for (int i = 0; i < 256; i++) if (g_wgv[i].port == port) {
        Rect b = rd_rect(hderef(rd32(port + PORT_BITS)) + PM_BOUNDS);
        wr16(port + 8, (u16)b.top);
        wr16(port + 10, (u16)b.left);
        return;
    }
}

/* Initialise the common fields of a port (portBits must already be set). */
void port_init(u32 port, bool color) {
    Surf s;
    Rect b = { 0, 0, 0, 0 };
    if (surf_from_port(port, &s)) b = s.bounds;
    wr16(port + PORT_DEVICE, 0);
    wr_rect(port + PORT_RECT, b);
    u32 vis = rgn_new(), clip = rgn_new();
    rgn_set_rect(vis, b);
    rgn_set_rect(clip, mkrect(-32767, -32767, 32767, 32767));
    wr32(port + PORT_VIS, vis);
    wr32(port + PORT_CLIP, clip);
    wr32(port + PORT_PNLOC, 0);
    wr32(port + PORT_PNSIZE, 0x00010001);
    wr16(port + PORT_PNMODE, patCopy);
    wr16(port + PORT_PNVIS, 0);
    wr16(port + PORT_TXFONT, 0);
    wr8(port + PORT_TXFACE, 0);
    wr16(port + PORT_TXMODE, srcOr);
    wr16(port + PORT_TXSIZE, 0);
    wr32(port + PORT_SPEXTRA, 0);
    wr32(port + PORT_FGCOLOR, 33);
    wr32(port + PORT_BKCOLOR, 30);
    wr16(port + PORT_COLRBIT, 0);
    wr16(port + PORT_PATSTRETCH, 0);
    wr32(port + PORT_PICSAVE, 0);
    wr32(port + PORT_RGNSAVE, 0);
    wr32(port + PORT_POLYSAVE, 0);
    wr32(port + PORT_GRAFPROCS, 0);
    if (color) {
        wr32(port + PORT_GRAFVARS, mm_new_handle(32, true, ZONE_APP));
        wr16(port + PORT_CHEXTRA, 0);
        wr16(port + PORT_PNLOCHFRAC, 0x8000);
        port_init_color_state(port);
    } else {
        gmemcpy_to(port + PORT_BKPAT, PAT_WHITE, 8);
        gmemcpy_to(port + PORT_FILLPAT_OLD, PAT_BLACK, 8);
        gmemcpy_to(port + PORT_PNPAT, PAT_BLACK, 8);
    }
}

/* Open a color port on the current device (OpenCPort semantics). */
static void open_cport(u32 port) {
    u32 dev = qd_cur_device();
    u32 dpm = rd32(hderef(dev) + GD_PMAP);
    u32 pmh = mm_new_handle(PM_SIZE, false, ZONE_APP);
    gmemmove(hderef(pmh), hderef(dpm), PM_SIZE);
    wr32(port + PORT_BITS, pmh);
    wr16(port + PORT_VERSION, 0xC000);
    port_init(port, true);
    qd_set_port(port);
}

static void open_port(u32 port) {
    /* old-style port on the screen bits */
    wr32(port + PORT_BITS, g_screen_base);
    wr16(port + PORT_BITS + 4, (u16)g_srow);
    wr_rect(port + PORT_BITS + 6, mkrect(0, 0, g_sh, g_sw));
    port_init(port, false);
    qd_set_port(port);
}

RGB port_fg(u32 port) {
    if (is_color_port(port)) { RGB c; rd_rgb(port + PORT_RGBFG, &c); return c; }
    return (RGB){ 0, 0, 0 };
}
RGB port_bk(u32 port) {
    if (is_color_port(port)) { RGB c; rd_rgb(port + PORT_RGBBK, &c); return c; }
    return (RGB){ 0xFFFF, 0xFFFF, 0xFFFF };
}

/* Classic 8-color constants used by ForeColor/BackColor */
static RGB classic_color(s32 c) {
    switch (c) {
    case 33: return (RGB){ 0, 0, 0 };             /* black */
    case 30: return (RGB){ 0xFFFF, 0xFFFF, 0xFFFF }; /* white */
    case 205: return (RGB){ 0xDD6B, 0x08C2, 0x06A2 }; /* red */
    case 341: return (RGB){ 0x0000, 0x8000, 0x11B0 }; /* green */
    case 409: return (RGB){ 0x0000, 0x0000, 0xD400 }; /* blue */
    case 273: return (RGB){ 0x0241, 0xAB54, 0xEAFF }; /* cyan */
    case 137: return (RGB){ 0xF2D7, 0x0856, 0x84EC }; /* magenta */
    case 69: return (RGB){ 0xFC00, 0xF37D, 0x052F };  /* yellow */
    }
    return (RGB){ 0, 0, 0 };
}

static u32 index_for_port(u32 port, RGB c) {
    (void)port;
    u32 dev = qd_cur_device();
    u32 pm = hderef(rd32(hderef(dev) + GD_PMAP));
    u32 ct = rd32(pm + PM_TABLE);
    int depth = rd16(pm + PM_PIXSIZE);
    Surf s = { .depth = depth, .ctab = depth <= 8 ? ct : 0 };
    return pixel_for_rgb(&s, c);
}

/* ---------------------------------------------------------------------- */
/* Traps: initialisation and ports                                         */

static const u8 ARROW_CURSOR[68] = {
    0x00,0x00,0x40,0x00,0x60,0x00,0x70,0x00,0x78,0x00,0x7C,0x00,0x7E,0x00,0x7F,0x00,
    0x7F,0x80,0x7C,0x00,0x6C,0x00,0x46,0x00,0x06,0x00,0x03,0x00,0x03,0x00,0x00,0x00,
    0xC0,0x00,0xE0,0x00,0xF0,0x00,0xF8,0x00,0xFC,0x00,0xFE,0x00,0xFF,0x00,0xFF,0x80,
    0xFF,0xC0,0xFF,0xE0,0xFE,0x00,0xEF,0x00,0xCF,0x00,0x87,0x80,0x07,0x80,0x03,0x80,
    0x00,0x01,0x00,0x01
};

TRAP(InitGraf) {
    u32 p = ARG(0);
    g_qd_theport_ptr = p;
    wr32(p - 126, 1);                    /* randSeed */
    wr32(p - 122, g_screen_base);        /* screenBits */
    wr16(p - 118, (u16)g_srow);
    wr_rect(p - 116, mkrect(0, 0, g_sh, g_sw));
    gmemcpy_to(p - 108, ARROW_CURSOR, 68);
    static const u8 dk[8] = { 0x77, 0xDD, 0x77, 0xDD, 0x77, 0xDD, 0x77, 0xDD };
    static const u8 lt[8] = { 0x88, 0x22, 0x88, 0x22, 0x88, 0x22, 0x88, 0x22 };
    static const u8 gr[8] = { 0xAA, 0x55, 0xAA, 0x55, 0xAA, 0x55, 0xAA, 0x55 };
    gmemcpy_to(p - 40, dk, 8);
    gmemcpy_to(p - 32, lt, 8);
    gmemcpy_to(p - 24, gr, 8);
    gmemcpy_to(p - 16, PAT_BLACK, 8);
    gmemcpy_to(p - 8, PAT_WHITE, 8);
    wr32(p, 0);
    wr32(0x0904, p); /* CurrentA5 points at thePort pointer location conventionally */
    LOG_D("InitGraf qd.thePort at %08x", p);
}

TRAP(OpenPort) { open_port(ARG(0)); }
TRAP(InitPort) { open_port(ARG(0)); }
TRAP(OpenCPort) { open_cport(ARG(0)); }
TRAP(InitCPort) { open_cport(ARG(0)); }
TRAP(ClosePort) {
    u32 port = ARG(0);
    rgn_dispose(rd32(port + PORT_VIS));
    rgn_dispose(rd32(port + PORT_CLIP));
}
TRAP(CloseCPort) {
    u32 port = ARG(0);
    rgn_dispose(rd32(port + PORT_VIS));
    rgn_dispose(rd32(port + PORT_CLIP));
}
TRAP(SetPort) { qd_set_port(ARG(0)); }
TRAP(GetPort) { wr32(ARG(0), qd_port()); }
TRAP(PortChanged) { }
TRAP(GetCWMgrPort) { extern u32 wm_port(void); wr32(ARG(0), wm_port()); }
TRAP(GetWMgrPort) { extern u32 wm_port(void); wr32(ARG(0), wm_port()); }

TRAP(SetOrigin) {
    s16 h = ARGS16(0), v = ARGS16(1);
    u32 port = qd_port();
    Rect pr = rd_rect(port + PORT_RECT);
    int dh = h - pr.left, dv = v - pr.top;
    if (!dh && !dv) return;
    pr.left += dh; pr.right += dh; pr.top += dv; pr.bottom += dv;
    wr_rect(port + PORT_RECT, pr);
    u32 bm = is_color_port(port) ? hderef(rd32(port + PORT_BITS)) : port + PORT_BITS;
    Rect b = rd_rect(bm + 6);
    b.left += dh; b.right += dh; b.top += dv; b.bottom += dv;
    wr_rect(bm + 6, b);
    port_mirror_bounds(port);
    HRgn r; hrgn_from_guest(&r, rd32(port + PORT_VIS));
    hrgn_offset(&r, dh, dv);
    hrgn_to_guest(&r, rd32(port + PORT_VIS));
    hrgn_free(&r);
}

TRAP(ClipRect) { rgn_set_rect(rd32(qd_port() + PORT_CLIP), rd_rect(ARG(0))); }
TRAP(SetClip) {
    u32 src = ARG(0), dst = rd32(qd_port() + PORT_CLIP);
    u32 n = mm_handle_size(src);
    mm_set_handle_size(dst, n);
    gmemmove(hderef(dst), hderef(src), n);
}
TRAP(GetClip) {
    u32 dst = ARG(0), src = rd32(qd_port() + PORT_CLIP);
    u32 n = mm_handle_size(src);
    mm_set_handle_size(dst, n);
    gmemmove(hderef(dst), hderef(src), n);
}

TRAP(LocalToGlobal) {
    u32 pt = ARG(0);
    Surf s;
    if (!surf_from_port(qd_port(), &s)) return;
    Point p = rd_point(pt);
    p.h = (s16)(p.h - s.bounds.left); p.v = (s16)(p.v - s.bounds.top);
    wr_point(pt, p);
}
TRAP(GlobalToLocal) {
    u32 pt = ARG(0);
    Surf s;
    if (!surf_from_port(qd_port(), &s)) return;
    Point p = rd_point(pt);
    p.h = (s16)(p.h + s.bounds.left); p.v = (s16)(p.v + s.bounds.top);
    wr_point(pt, p);
}

/* ---- pen ---- */
TRAP(PenNormal) {
    u32 port = qd_port();
    wr32(port + PORT_PNSIZE, 0x00010001);
    wr16(port + PORT_PNMODE, patCopy);
    if (is_color_port(port)) set_pixpat_pattern(port, PORT_PNPAT, PAT_BLACK);
    else gmemcpy_to(port + PORT_PNPAT, PAT_BLACK, 8);
}
TRAP(PenSize) { u32 port = qd_port(); wr16(port + PORT_PNSIZE + 2, (u16)ARGS16(0)); wr16(port + PORT_PNSIZE, (u16)ARGS16(1)); }
TRAP(PenMode) { wr16(qd_port() + PORT_PNMODE, (u16)ARGS16(0)); }
TRAP(PenPat) {
    u8 pat[8]; gmemcpy_from(pat, ARG(0), 8);
    u32 port = qd_port();
    if (is_color_port(port)) set_pixpat_pattern(port, PORT_PNPAT, pat);
    else gmemcpy_to(port + PORT_PNPAT, pat, 8);
}
TRAP(PenPixPat) { u32 port = qd_port(); if (is_color_port(port)) wr32(port + PORT_PNPAT, ARG(0)); }
TRAP(BackPat) {
    u8 pat[8]; gmemcpy_from(pat, ARG(0), 8);
    u32 port = qd_port();
    if (is_color_port(port)) set_pixpat_pattern(port, PORT_BKPAT, pat);
    else gmemcpy_to(port + PORT_BKPAT, pat, 8);
}
TRAP(BackPixPat) { u32 port = qd_port(); if (is_color_port(port)) wr32(port + PORT_BKPAT, ARG(0)); }
TRAP(HidePen) { u32 port = qd_port(); wr16(port + PORT_PNVIS, (u16)(rds16(port + PORT_PNVIS) - 1)); }
TRAP(ShowPen) { u32 port = qd_port(); wr16(port + PORT_PNVIS, (u16)(rds16(port + PORT_PNVIS) + 1)); }
/* PenState: pnLoc(4) pnSize(4) pnMode(2) pnPat(8) = 18 bytes */
TRAP(GetPenState) {
    u32 ps = ARG(0), port = qd_port();
    wr32(ps, rd32(port + PORT_PNLOC));
    wr32(ps + 4, rd32(port + PORT_PNSIZE));
    wr16(ps + 8, rd16(port + PORT_PNMODE));
    if (is_color_port(port)) {
        u32 h = rd32(port + PORT_PNPAT);
        /* stash the handle in the pattern bytes so SetPenState can restore it */
        wr32(ps + 10, 0x50505050); wr32(ps + 14, h);
    } else gmemmove(ps + 10, port + PORT_PNPAT, 8);
}
TRAP(SetPenState) {
    u32 ps = ARG(0), port = qd_port();
    wr32(port + PORT_PNLOC, rd32(ps));
    wr32(port + PORT_PNSIZE, rd32(ps + 4));
    wr16(port + PORT_PNMODE, rd16(ps + 8));
    if (is_color_port(port)) {
        if (rd32(ps + 10) == 0x50505050) wr32(port + PORT_PNPAT, rd32(ps + 14));
        else { u8 pat[8]; gmemcpy_from(pat, ps + 10, 8); set_pixpat_pattern(port, PORT_PNPAT, pat); }
    } else gmemmove(port + PORT_PNPAT, ps + 10, 8);
}
TRAP(MoveTo) { u32 port = qd_port(); wr16(port + PORT_PNLOC + 2, (u16)ARGS16(0)); wr16(port + PORT_PNLOC, (u16)ARGS16(1)); }
TRAP(Move) {
    u32 port = qd_port();
    wr16(port + PORT_PNLOC + 2, (u16)(rds16(port + PORT_PNLOC + 2) + ARGS16(0)));
    wr16(port + PORT_PNLOC, (u16)(rds16(port + PORT_PNLOC) + ARGS16(1)));
}

/* ---- colors ---- */
TRAP(ForeColor) {
    s32 c = (s32)ARG(0);
    u32 port = qd_port();
    wr32(port + PORT_FGCOLOR, (u32)c);
    if (is_color_port(port)) {
        RGB rgb = classic_color(c);
        wr_rgb(port + PORT_RGBFG, rgb);
        wr32(port + PORT_FGCOLOR, index_for_port(port, rgb));
    }
}
TRAP(BackColor) {
    s32 c = (s32)ARG(0);
    u32 port = qd_port();
    wr32(port + PORT_BKCOLOR, (u32)c);
    if (is_color_port(port)) {
        RGB rgb = classic_color(c);
        wr_rgb(port + PORT_RGBBK, rgb);
        wr32(port + PORT_BKCOLOR, index_for_port(port, rgb));
    }
}
TRAP(RGBForeColor) {
    RGB c; rd_rgb(ARG(0), &c);
    u32 port = qd_port();
    if (is_color_port(port)) { wr_rgb(port + PORT_RGBFG, c); wr32(port + PORT_FGCOLOR, index_for_port(port, c)); }
    else wr32(port + PORT_FGCOLOR, (c.r | c.g | c.b) == 0xFFFF && c.r == 0xFFFF ? 30 : 33);
}
TRAP(RGBBackColor) {
    RGB c; rd_rgb(ARG(0), &c);
    u32 port = qd_port();
    if (is_color_port(port)) { wr_rgb(port + PORT_RGBBK, c); wr32(port + PORT_BKCOLOR, index_for_port(port, c)); }
    else wr32(port + PORT_BKCOLOR, c.r == 0xFFFF && c.g == 0xFFFF && c.b == 0xFFFF ? 30 : 33);
}
TRAP(GetForeColor) { wr_rgb(ARG(0), port_fg(qd_port())); }
TRAP(GetBackColor) { wr_rgb(ARG(0), port_bk(qd_port())); }
TRAP(OpColor) {
    u32 port = qd_port();
    if (!is_color_port(port)) return;
    u32 gv = port_grafvars(port);
    if (gv && hderef(gv)) { RGB c; rd_rgb(ARG(0), &c); wr_rgb(hderef(gv), c); }
}
TRAP(HiliteColor) { }
TRAP(Color2Index) { RGB c; rd_rgb(ARG(0), &c); RET(index_for_port(qd_port(), c)); }
TRAP(Index2Color) {
    u32 dev = qd_cur_device();
    u32 pm = hderef(rd32(hderef(dev) + GD_PMAP));
    wr_rgb(ARG(1), ctab_color(rd32(pm + PM_TABLE), (int)ARG(0)));
}
TRAP(GetGray) {
    RGB bk, fg; rd_rgb(ARG(1), &bk); rd_rgb(ARG(2), &fg);
    RGB m = { (u16)((bk.r + fg.r) / 2), (u16)((bk.g + fg.g) / 2), (u16)((bk.b + fg.b) / 2) };
    wr_rgb(ARG(2), m);
    RET(1);
}

/* HSL <-> RGB (SmallFract components) */
TRAP(HSL2RGB) {
    u32 hp = ARG(0), rp = ARG(1);
    double h = rd16(hp) / 65536.0, s = rd16(hp + 2) / 65535.0, l = rd16(hp + 4) / 65535.0;
    double r, g, b;
    if (s == 0) r = g = b = l;
    else {
        double q = l < 0.5 ? l * (1 + s) : l + s - l * s, p = 2 * l - q;
        double t[3] = { h + 1.0 / 3, h, h - 1.0 / 3 }, o[3];
        for (int i = 0; i < 3; i++) {
            double x = t[i];
            if (x < 0) x += 1;
            if (x > 1) x -= 1;
            if (x < 1.0 / 6) o[i] = p + (q - p) * 6 * x;
            else if (x < 0.5) o[i] = q;
            else if (x < 2.0 / 3) o[i] = p + (q - p) * (2.0 / 3 - x) * 6;
            else o[i] = p;
        }
        r = o[0]; g = o[1]; b = o[2];
    }
    wr_rgb(rp, (RGB){ (u16)(r * 65535), (u16)(g * 65535), (u16)(b * 65535) });
}
TRAP(RGB2HSL) {
    RGB c; rd_rgb(ARG(0), &c);
    u32 hp = ARG(1);
    double r = c.r / 65535.0, g = c.g / 65535.0, b = c.b / 65535.0;
    double mx = r > g ? (r > b ? r : b) : (g > b ? g : b), mn = r < g ? (r < b ? r : b) : (g < b ? g : b);
    double l = (mx + mn) / 2, h = 0, s = 0;
    if (mx != mn) {
        double d = mx - mn;
        s = l > 0.5 ? d / (2 - mx - mn) : d / (mx + mn);
        if (mx == r) h = (g - b) / d + (g < b ? 6 : 0);
        else if (mx == g) h = (b - r) / d + 2;
        else h = (r - g) / d + 4;
        h /= 6;
    }
    wr16(hp, (u16)(h * 65535)); wr16(hp + 2, (u16)(s * 65535)); wr16(hp + 4, (u16)(l * 65535));
}

/* ---- Random: QuickDraw's Park-Miller generator ---- */
TRAP(Random) {
    u32 sp = g_qd_theport_ptr - 126;
    u32 seed = rd32(sp);
    if (seed == 0) seed = 1;
    u64 v = (u64)seed * 16807u % 0x7FFFFFFFu;
    wr32(sp, (u32)v);
    u16 r = (u16)v;
    if (r == 0x8000) r = 0;
    RET((u32)(s32)(s16)r);
}

/* ---- rectangles and points ---- */
TRAP(SetRect) { wr_rect(ARG(0), mkrect(ARGS16(2), ARGS16(1), ARGS16(4), ARGS16(3))); }
TRAP(OffsetRect) {
    u32 a = ARG(0); s16 dh = ARGS16(1), dv = ARGS16(2);
    Rect r = rd_rect(a);
    r.left += dh; r.right += dh; r.top += dv; r.bottom += dv;
    wr_rect(a, r);
}
TRAP(InsetRect) {
    u32 a = ARG(0); s16 dh = ARGS16(1), dv = ARGS16(2);
    Rect r = rd_rect(a);
    r.left += dh; r.right -= dh; r.top += dv; r.bottom -= dv;
    if (rect_empty(r)) r = (Rect){ 0, 0, 0, 0 };
    wr_rect(a, r);
}
TRAP(SectRect) {
    Rect a = rd_rect(ARG(0)), b = rd_rect(ARG(1));
    Rect r = rect_sect(a, b);
    wr_rect(ARG(2), r);
    RET(!rect_empty(r));
}
TRAP(UnionRect) {
    Rect a = rd_rect(ARG(0)), b = rd_rect(ARG(1));
    Rect r = { a.top < b.top ? a.top : b.top, a.left < b.left ? a.left : b.left,
               a.bottom > b.bottom ? a.bottom : b.bottom, a.right > b.right ? a.right : b.right };
    wr_rect(ARG(2), r);
}
TRAP(PtInRect) {
    Point p = pt_from_u32(ARG(0));
    Rect r = rd_rect(ARG(1));
    RET(p.h >= r.left && p.h < r.right && p.v >= r.top && p.v < r.bottom);
}
TRAP(EqualRect) {
    Rect a = rd_rect(ARG(0)), b = rd_rect(ARG(1));
    RET(a.top == b.top && a.left == b.left && a.bottom == b.bottom && a.right == b.right);
}
TRAP(EmptyRect) { RET(rect_empty(rd_rect(ARG(0)))); }
TRAP(Pt2Rect) {
    Point a = pt_from_u32(ARG(0)), b = pt_from_u32(ARG(1));
    Rect r;
    rect_from_pts(a, b, &r);
    wr_rect(ARG(2), r);
}
void rect_from_pts(Point a, Point b, Rect *r) {
    r->top = a.v < b.v ? a.v : b.v; r->bottom = a.v < b.v ? b.v : a.v;
    r->left = a.h < b.h ? a.h : b.h; r->right = a.h < b.h ? b.h : a.h;
}
TRAP(MapPt) {
    u32 pp = ARG(0); Rect s = rd_rect(ARG(1)), d = rd_rect(ARG(2));
    Point p = rd_point(pp);
    int sw = s.right - s.left, sh = s.bottom - s.top;
    if (sw && sh) {
        p.h = (s16)(d.left + (p.h - s.left) * (d.right - d.left) / sw);
        p.v = (s16)(d.top + (p.v - s.top) * (d.bottom - d.top) / sh);
    }
    wr_point(pp, p);
}
TRAP(ScalePt) {
    u32 pp = ARG(0); Rect s = rd_rect(ARG(1)), d = rd_rect(ARG(2));
    Point p = rd_point(pp);
    int sw = s.right - s.left, sh = s.bottom - s.top;
    if (sw && sh) {
        p.h = (s16)(p.h * (d.right - d.left) / sw);
        p.v = (s16)(p.v * (d.bottom - d.top) / sh);
        if (p.h < 1) p.h = 1;
        if (p.v < 1) p.v = 1;
    }
    wr_point(pp, p);
}

/* ---- devices ---- */
TRAP(GetDeviceList) { RET(g_screen_gd); }
TRAP(GetMainDevice) { RET(g_screen_gd); }
TRAP(GetNextDevice) { u32 d = ARG(0); RET(d ? rd32(hderef(d) + GD_NEXT) : 0); }
TRAP(GetGDevice) { RET(qd_cur_device()); }
TRAP(SetGDevice) { wr32(LM_TheGDevice, ARG(0)); }
TRAP(TestDeviceAttribute) { u32 d = ARG(0); s16 a = ARGS16(1); RET(d && (rd16(hderef(d) + GD_FLAGS) >> a) & 1); }
TRAP(HasDepth) {
    s16 depth = ARGS16(1);
    RET(depth == 8 ? 0x0100 : 0);
}
TRAP(SetDepth) {
    s16 depth = ARGS16(1);
    RETERR(depth == 8 ? noErr : paramErr);
}
TRAP(DeviceLoop) {
    /* DeviceLoop(drawingRgn, drawingProc, userData, flags): one 8-bit device */
    u32 proc = ARG(1), data = ARG(2);
    u32 a[3] = { 8, 0x0001, data };
    a[1] = rd16(hderef(g_screen_gd) + GD_FLAGS);
    u32 args[3] = { 8, a[1], data };
    (void)a;
    /* drawingProc(depth, deviceFlags, targetDevice, userData) */
    u32 args4[4] = { 8, args[1], g_screen_gd, data };
    call_upp(proc, 4, args4);
}

/* ---- color tables ---- */
TRAP(GetCTable) {
    s16 id = ARGS16(0);
    u32 len;
    u8 *d = res_load_raw(FOURCC('c','l','u','t'), id, &len);
    if (d) {
        u32 h = mm_handle_from_data(d, len, ZONE_APP);
        free(d);
        wr32(hderef(h), (u32)id); /* seed = resource id, like QD */
        RET(h);
        return;
    }
    if (id == 8 || id == 72) { RET(default_ctab(8)); return; }
    if (id == 1 || id == 33) { RET(default_ctab(1)); return; }
    if (id == 2 || id == 34) { RET(default_ctab(2)); return; }
    if (id == 4 || id == 36) { RET(default_ctab(4)); return; }
    RET(0);
}
TRAP(DisposeCTable) { u32 h = ARG(0); ctab_invalidate(h); if (h && h != g_screen_ctab) mm_dispose_handle(h); }
TRAP(CTabChanged) { u32 h = ARG(0); if (h && hderef(h)) wr32(hderef(h), g_ctab_seed++); ctab_invalidate(h); }
TRAP(GetSubTable) { /* map colors in myColors to indexes of refTable */
    u32 mine = ARG(0), ref = ARG(2);
    if (!ref) ref = rd32(hderef(rd32(hderef(qd_cur_device()) + GD_PMAP)) + PM_TABLE);
    u32 p = hderef(mine);
    int n = (s16)rd16(p + 6) + 1;
    for (int i = 0; i < n; i++) {
        RGB c; rd_rgb(p + 10 + 8 * (u32)i, &c);
        wr16(p + 8 + 8 * (u32)i, (u16)ctab_nearest(ref, c));
    }
}

/* ---------------------------------------------------------------------- */
/* GWorlds                                                                 */

#define GW_EXTRA_DEV   108
#define GW_EXTRA_MAGIC 112
#define GW_EXTRA_PIX   116
#define GW_SIZE        124
#define GW_MAGIC       0x47574C44u

static u32 g_gworlds[512];
static void gw_register(u32 gw, bool add) {
    for (int i = 0; i < 512; i++) {
        if (add && !g_gworlds[i]) { g_gworlds[i] = gw; return; }
        if (!add && g_gworlds[i] == gw) { g_gworlds[i] = 0; return; }
    }
}
bool is_gworld(u32 port) {
    if (!port) return false;
    for (int i = 0; i < 512; i++) if (g_gworlds[i] == port) return true;
    return false;
}

static int alloc_gworld(u32 gw, int depth, Rect bounds, u32 ctab, bool create) {
    int w = bounds.right - bounds.left, h = bounds.bottom - bounds.top;
    if (w <= 0) w = 1;
    if (h <= 0) h = 1;
    int rb = ((w * depth + 31) / 32) * 4;
    u32 pix = mm_new_ptr((u32)(rb * h) + 64, true, ZONE_APP);
    if (!pix) return memFullErr;
    /* fresh GWorld pixels are white (index 0 for indexed depths) */
    if (depth >= 16) gmemset(pix, 0xFF, (u32)(rb * h));
    u32 ct = 0;
    if (depth <= 8) {
        if (ctab) {
            u32 n = mm_handle_size(ctab);
            ct = mm_new_handle(n, false, ZONE_APP);
            gmemmove(hderef(ct), hderef(ctab), n);
        } else ct = depth == 8 ? (u32)({ u32 t = mm_new_handle(mm_handle_size(g_std_ctab8), false, ZONE_APP);
                                    gmemmove(hderef(t), hderef(g_std_ctab8), mm_handle_size(g_std_ctab8)); t; })
                               : default_ctab(depth);
    }
    u32 pmh;
    if (create) {
        pmh = make_pixmap(pix, rb, bounds, depth, ct);
        wr32(gw + PORT_BITS, pmh);
        wr16(gw + PORT_VERSION, 0xC001);
        port_init(gw, true);
        u32 dev = make_gdevice(pmh, bounds, false);
        wr16(hderef(dev) + GD_TYPE, depth > 8 ? 2 : 0);
        wr32(gw + GW_EXTRA_DEV, dev);
        wr32(gw + GW_EXTRA_MAGIC, GW_MAGIC);
        wr32(gw + GW_EXTRA_PIX, pix);
        gw_register(gw, true);
    } else {
        pmh = rd32(gw + PORT_BITS);
        u32 pm = hderef(pmh);
        u32 old = rd32(gw + GW_EXTRA_PIX);
        u32 oldct = rd32(pm + PM_TABLE);
        wr32(pm + PM_BASE, pix);
        wr16(pm + PM_ROWBYTES, (u16)(0x8000 | rb));
        wr_rect(pm + PM_BOUNDS, bounds);
        wr16(pm + PM_PIXSIZE, (u16)depth);
        wr16(pm + PM_CMPSIZE, (u16)(depth == 16 ? 5 : depth == 32 ? 8 : depth));
        wr16(pm + PM_CMPCOUNT, depth > 8 ? 3 : 1);
        wr16(pm + PM_PIXTYPE, depth > 8 ? 16 : 0);
        wr32(pm + PM_TABLE, ct);
        if (oldct && oldct != ct) mm_dispose_handle(oldct);
        if (old) mm_dispose_ptr(old);
        wr32(gw + GW_EXTRA_PIX, pix);
        wr_rect(gw + PORT_RECT, bounds);
        rgn_set_rect(rd32(gw + PORT_VIS), bounds);
        wr_rect(hderef(rd32(gw + GW_EXTRA_DEV)) + GD_RECT, bounds);
    }
    return noErr;
}

TRAP(NewGWorld) {
    u32 gwp = ARG(0); s16 depth = ARGS16(1); Rect bounds = rd_rect(ARG(2)); u32 ctab = ARG(3);
    if (depth == 0) depth = 8;
    if (depth != 1 && depth != 2 && depth != 4 && depth != 8 && depth != 16 && depth != 32) { RETERR(paramErr); return; }
    u32 gw = mm_new_ptr(GW_SIZE, true, ZONE_APP);
    u32 save = qd_port();
    int err = alloc_gworld(gw, depth, bounds, ctab, true);
    qd_set_port(save);
    if (err) { mm_dispose_ptr(gw); wr32(gwp, 0); RETERR(err); return; }
    wr32(gwp, gw);
    LOG_D("NewGWorld %08x depth %d (%d,%d,%d,%d)", gw, depth, bounds.top, bounds.left, bounds.bottom, bounds.right);
    RETERR(noErr);
}

TRAP(UpdateGWorld) {
    u32 gwp = ARG(0); s16 depth = ARGS16(1); Rect bounds = rd_rect(ARG(2)); u32 ctab = ARG(3);
    u32 flags = ARG(5);
    u32 gw = rd32(gwp);
    if (!is_gworld(gw)) { RET(0x80000000); return; } /* gwFlagErr */
    u32 pm = hderef(rd32(gw + PORT_BITS));
    int od = rd16(pm + PM_PIXSIZE);
    Rect ob = rd_rect(pm + PM_BOUNDS);
    if (depth == 0) depth = (s16)od;
    int ow = ob.right - ob.left, oh = ob.bottom - ob.top;
    int nw = bounds.right - bounds.left, nh = bounds.bottom - bounds.top;
    u32 result = 0;
    bool ctab_changed = ctab && ctab_seed(ctab) != ctab_seed(rd32(pm + PM_TABLE));
    if (od != depth || ow != nw || oh != nh || ctab_changed) {
        u32 oldpix = rd32(gw + GW_EXTRA_PIX);
        int orb = rd16(pm + PM_ROWBYTES) & 0x3FFF;
        u8 *copy = NULL;
        if ((flags & 0x2) && od == depth) { /* clipPix: preserve */
            copy = malloc((size_t)(orb * oh));
            gmemcpy_from(copy, oldpix, (u32)(orb * oh));
        }
        wr32(gw + GW_EXTRA_PIX, oldpix);
        alloc_gworld(gw, depth, bounds, ctab ? ctab : (od == depth ? 0 : 0), false);
        if (copy) {
            u32 npm = hderef(rd32(gw + PORT_BITS));
            u32 np = rd32(npm + PM_BASE);
            int nrb = rd16(npm + PM_ROWBYTES) & 0x3FFF;
            int cr = orb < nrb ? orb : nrb;
            int ch = oh < nh ? oh : nh;
            for (int y = 0; y < ch; y++) gmemcpy_to(np + (u32)(y * nrb), copy + y * orb, (u32)cr);
            free(copy);
        }
        result = (ow != nw || oh != nh) ? 0x1 : 0;
        if (od != depth) result |= 0x4;
        if (ctab_changed) result |= 0x8;
    } else {
        /* bounds moved (same size): re-origin */
        wr_rect(pm + PM_BOUNDS, bounds);
        wr_rect(gw + PORT_RECT, bounds);
        rgn_set_rect(rd32(gw + PORT_VIS), bounds);
    }
    RET(result);
}

TRAP(DisposeGWorld) {
    u32 gw = ARG(0);
    if (!is_gworld(gw)) return;
    u32 pmh = rd32(gw + PORT_BITS);
    u32 ct = rd32(hderef(pmh) + PM_TABLE);
    mm_dispose_ptr(rd32(gw + GW_EXTRA_PIX));
    if (ct) { ctab_invalidate(ct); mm_dispose_handle(ct); }
    mm_dispose_handle(pmh);
    rgn_dispose(rd32(gw + PORT_VIS));
    rgn_dispose(rd32(gw + PORT_CLIP));
    wr32(gw + GW_EXTRA_MAGIC, 0);
    gw_register(gw, false);
    if (qd_port() == gw) qd_set_port(0);
    mm_dispose_ptr(gw);
}

TRAP(GetGWorld) { u32 pp = ARG(0), dp = ARG(1); if (pp) wr32(pp, qd_port()); if (dp) wr32(dp, qd_cur_device()); }
TRAP(SetGWorld) {
    u32 port = ARG(0), dev = ARG(1);
    qd_set_port(port);
    if (is_gworld(port)) wr32(LM_TheGDevice, rd32(port + GW_EXTRA_DEV));
    else if (dev) wr32(LM_TheGDevice, dev);
    else wr32(LM_TheGDevice, g_screen_gd);
}
TRAP(GetGWorldPixMap) { RET(rd32(ARG(0) + PORT_BITS)); }
TRAP(GetGWorldDevice) { u32 gw = ARG(0); RET(is_gworld(gw) ? rd32(gw + GW_EXTRA_DEV) : g_screen_gd); }
TRAP(LockPixels) { RET(1); }
TRAP(UnlockPixels) { }
TRAP(GetPixelsState) { RET(0); }
TRAP(SetPixelsState) { }
TRAP(GetPixBaseAddr) { u32 pm = ARG(0); RET(pm && hderef(pm) ? rd32(hderef(pm)) : 0); }
TRAP(PixMap32Bit) { RET(0); }

/* ---------------------------------------------------------------------- */
/* Pixel patterns                                                          */

TRAP(NewPixPat) {
    u32 h = mm_new_handle(PP_SIZE, true, ZONE_APP);
    u32 p = hderef(h);
    wr16(p + PP_TYPE, 1);
    wr32(p + PP_MAP, make_pixmap(0, 0, mkrect(0, 0, 8, 8), 8, default_ctab(8)));
    wr32(p + PP_DATA, mm_new_handle(0, false, ZONE_APP));
    wr16(p + PP_XVALID, (u16)-1);
    RET(h);
}
TRAP(DisposePixPat) {
    u32 h = ARG(0);
    if (!h || !hderef(h)) return;
    u32 p = hderef(h);
    u32 pm = rd32(p + PP_MAP);
    if (pm) mm_dispose_handle(pm);
    if (rd32(p + PP_DATA)) mm_dispose_handle(rd32(p + PP_DATA));
    mm_dispose_handle(h);
}
TRAP(PixPatChanged) { }
TRAP(GetPixPat) {
    s16 id = ARGS16(0);
    u32 len;
    u8 *d = res_load_raw(FOURCC('p','p','a','t'), id, &len);
    if (!d || len < PP_SIZE) { free(d); RET(0); return; }
    u32 h = mm_new_handle(PP_SIZE, true, ZONE_APP);
    u32 p = hderef(h);
    u16 type = be16(d);
    u32 pmoff = be32(d + 2), dataoff = be32(d + 6);
    wr16(p + PP_TYPE, type);
    memcpy(g_mem + p + PP_PAT1, d + 20, 8);
    wr16(p + PP_XVALID, (u16)-1);
    if (type == 1 && pmoff + PM_SIZE <= len) {
        const u8 *pm = d + pmoff;
        int rb = be16(pm + 4) & 0x3FFF;
        Rect b = { (s16)be16(pm + 6), (s16)be16(pm + 8), (s16)be16(pm + 10), (s16)be16(pm + 12) };
        int depth = be16(pm + 32);
        u32 ctoff = be32(pm + 42);
        u32 ct = 0;
        if (ctoff && ctoff < len) {
            int n = (s16)be16(d + ctoff + 6) + 1;
            u32 ctlen = 8 + 8 * (u32)n;
            if (ctoff + ctlen <= len) ct = mm_handle_from_data(d + ctoff, ctlen, ZONE_APP);
        }
        u32 datalen = (u32)(rb * (b.bottom - b.top));
        u32 dh = mm_new_handle(datalen, true, ZONE_APP);
        if (dataoff + datalen <= len) gmemcpy_to(hderef(dh), d + dataoff, datalen);
        u32 pmh = make_pixmap(0, rb, b, depth, ct);
        wr32(p + PP_MAP, pmh);
        wr32(p + PP_DATA, dh);
    } else {
        wr32(p + PP_MAP, make_pixmap(0, 0, mkrect(0, 0, 8, 8), 1, 0));
        wr32(p + PP_DATA, mm_new_handle(0, false, ZONE_APP));
    }
    free(d);
    RET(h);
}

/* ---------------------------------------------------------------------- */
/* Palette Manager                                                         */

#define PAL_HDR 16
#define PAL_ENT 16

static u32 g_win_pal[256][2]; /* window -> palette */

static u32 new_palette(int n) {
    u32 h = mm_new_handle(PAL_HDR + PAL_ENT * (u32)n, true, ZONE_APP);
    wr16(hderef(h), (u16)n);
    return h;
}

TRAP(NewPalette) {
    s16 n = ARGS16(0); u32 src = ARG(1); s16 usage = ARGS16(2), tol = ARGS16(3);
    u32 h = new_palette(n);
    u32 p = hderef(h);
    for (int i = 0; i < n; i++) {
        u32 e = p + PAL_HDR + PAL_ENT * (u32)i;
        if (src) wr_rgb(e, ctab_color(src, i));
        wr16(e + 6, (u16)usage); wr16(e + 8, (u16)tol);
    }
    RET(h);
}
TRAP(GetNewPalette) {
    u32 len;
    u8 *d = res_load_raw(FOURCC('p','l','t','t'), ARGS16(0), &len);
    if (!d) { RET(0); return; }
    u32 h = mm_handle_from_data(d, len, ZONE_APP);
    free(d);
    RET(h);
}
TRAP(DisposePalette) { mm_dispose_handle(ARG(0)); }
TRAP(CTab2Palette) {
    u32 ct = ARG(0), pal = ARG(1); s16 usage = ARGS16(2), tol = ARGS16(3);
    int n = ctab_count(ct);
    mm_set_handle_size(pal, PAL_HDR + PAL_ENT * (u32)n);
    u32 p = hderef(pal);
    wr16(p, (u16)n);
    for (int i = 0; i < n; i++) {
        u32 e = p + PAL_HDR + PAL_ENT * (u32)i;
        wr_rgb(e, ctab_color(ct, i));
        wr16(e + 6, (u16)usage); wr16(e + 8, (u16)tol);
    }
}
TRAP(SetEntryUsage) {
    u32 pal = ARG(0); s16 idx = ARGS16(1), usage = ARGS16(2), tol = ARGS16(3);
    u32 p = hderef(pal);
    if (idx < 0 || idx >= (s16)rd16(p)) return;
    u32 e = p + PAL_HDR + PAL_ENT * (u32)idx;
    if (usage != -1) wr16(e + 6, (u16)usage);
    if (tol != -1) wr16(e + 8, (u16)tol);
}
TRAP(SetPalette) {
    u32 win = ARG(0), pal = ARG(1);
    for (int i = 0; i < 256; i++) if (g_win_pal[i][0] == win || !g_win_pal[i][0]) { g_win_pal[i][0] = win; g_win_pal[i][1] = pal; break; }
}
TRAP(GetPalette) {
    u32 win = ARG(0);
    for (int i = 0; i < 256; i++) if (g_win_pal[i][0] == win) { RET(g_win_pal[i][1]); return; }
    RET(0);
}

static void apply_palette(u32 pal) {
    u32 p = hderef(pal);
    int n = (s16)rd16(p);
    u32 dp = hderef(g_screen_ctab);
    for (int i = 0; i < n && i < 256; i++) {
        u32 e = p + PAL_HDR + PAL_ENT * (u32)i;
        u16 usage = rd16(e + 6);
        if (!(usage & 0x000E)) continue; /* courteous entries don't change the clut */
        if (i == 0 || i == 255) continue;
        RGB c; rd_rgb(e, &c);
        wr_rgb(dp + 10 + 8 * (u32)i, c);
    }
    wr32(dp, g_ctab_seed++);
    qd_screen_dirty();
}

TRAP(ActivatePalette) {
    u32 win = ARG(0);
    for (int i = 0; i < 256; i++) if (g_win_pal[i][0] == win) { if (g_win_pal[i][1]) apply_palette(g_win_pal[i][1]); return; }
}
TRAP(SetEntries) {
    s16 start = ARGS16(0), count = ARGS16(1); u32 table = ARG(2);
    u32 dev = qd_cur_device();
    u32 ct = rd32(hderef(rd32(hderef(dev) + GD_PMAP)) + PM_TABLE);
    u32 dp = hderef(ct);
    for (int i = 0; i <= count; i++) {
        u32 e = table + 8 * (u32)i;
        int idx = start < 0 ? rd16(e) : start + i;
        if (idx < 0 || idx > 255) continue;
        RGB c; rd_rgb(e + 2, &c);
        wr_rgb(dp + 10 + 8 * (u32)idx, c);
    }
    wr32(dp, g_ctab_seed++);
    qd_screen_dirty();
}
TRAP(RestoreDeviceClut) {
    RGB c[256];
    std_clut8(c);
    u32 dp = hderef(g_screen_ctab);
    for (int i = 0; i < 256; i++) wr_rgb(dp + 10 + 8 * (u32)i, c[i]);
    wr32(dp, g_ctab_seed++);
    qd_screen_dirty();
}
