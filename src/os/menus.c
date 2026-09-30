/* Menu Manager: menu bar, pull-down/pop-up menu tracking, keyboard
 * equivalents, and item manipulation on MenuInfo handles. */
#include "wm.h"
#include "resources.h"
#include "misc.h"

#define MI_ID      0
#define MI_WIDTH   2
#define MI_HEIGHT  4
#define MI_PROC    6
#define MI_ENABLE  10
#define MI_DATA    14

#define MAX_MENUS 32
static u32 g_bar[MAX_MENUS];   /* menu handles in the bar, left to right */
static int g_nbar;
static u32 g_hier[MAX_MENUS];  /* hierarchical / popup menus */
static int g_nhier;
static s16 g_hilited;
/* Menus are drawn in the system font (low memory SysFontFam/SysFontSize),
   which the game switches to the window's font around some pop-ups
   (PopUpMenuSelectWithCurFont). Item height and baseline follow it. */
static int ITEM_H = 16, ITEM_BASE = 12;
#define LM_SysFontFam  0x0BA6
#define LM_SysFontSize 0x0BA8
#define BAR_H 20
int menu_bar_height(void) { return BAR_H; }

/* ---- item access ---- */
typedef struct { u32 name; u8 icon, key, mark, style; u32 after; } Item;

static int menu_items(u32 m, Item *items, int max) {
    u32 p = hderef(m) + MI_DATA;
    p += 1 + rd8(p); /* title */
    int n = 0;
    u32 end = hderef(m) + mm_handle_size(m);
    while (p < end && rd8(p) && n < max) {
        Item *it = &items[n++];
        it->name = p;
        p += 1 + rd8(p);
        it->icon = rd8(p); it->key = rd8(p + 1); it->mark = rd8(p + 2); it->style = rd8(p + 3);
        p += 4;
        it->after = p;
    }
    return n;
}
static int count_items(u32 m) { Item it[256]; return menu_items(m, it, 256); }
static bool item_enabled(u32 m, int item) {
    u32 f = rd32(hderef(m) + MI_ENABLE);
    if (!(f & 1)) return false;
    if (item > 31) return true;
    return (f >> item) & 1;
}
static bool item_is_sep(u32 m, int idx0, const Item *it) {
    (void)m; (void)idx0;
    return rd8(it->name) >= 1 && rd8(it->name + 1) == '-';
}

/* ---- metacharacter parsing for AppendMenu ---- */
static void append_item(u32 m, const u8 *s, int n) {
    u8 name[256]; int nl = 0;
    u8 icon = 0, key = 0, mark = 0, style = 0;
    bool disabled = false;
    for (int i = 0; i < n; i++) {
        u8 c = s[i];
        if (c == '^' && i + 1 < n) { icon = (u8)(s[++i] - '0'); continue; }
        if (c == '!' && i + 1 < n) { mark = s[++i]; continue; }
        if (c == '<' && i + 1 < n) {
            u8 st = s[++i];
            style |= st == 'B' ? 1 : st == 'I' ? 2 : st == 'U' ? 4 : st == 'O' ? 8 : st == 'S' ? 16 : 0;
            continue;
        }
        if (c == '/' && i + 1 < n) { key = s[++i]; continue; }
        if (c == '(') { disabled = true; continue; }
        name[nl++] = c;
    }
    if (nl == 1 && name[0] == '-') disabled = true;
    u32 size = mm_handle_size(m);
    int idx = count_items(m) + 1;
    mm_set_handle_size(m, size + (u32)nl + 5);
    u32 p = hderef(m) + size - 1; /* overwrite terminator */
    wr8(p, (u8)nl);
    gmemcpy_to(p + 1, name, (u32)nl);
    wr8(p + 1 + (u32)nl, icon); wr8(p + 2 + (u32)nl, key); wr8(p + 3 + (u32)nl, mark); wr8(p + 4 + (u32)nl, style);
    wr8(p + 5 + (u32)nl, 0);
    if (idx <= 31) {
        u32 f = rd32(hderef(m) + MI_ENABLE);
        if (disabled) f &= ~(1u << idx); else f |= 1u << idx;
        wr32(hderef(m) + MI_ENABLE, f);
    }
}

static void calc_size(u32 m);

TRAP(NewMenu) {
    s16 id = ARGS16(0); u32 tp = ARG(1);
    u8 tl = rd8(tp);
    u32 h = mm_new_handle(MI_DATA + 1 + tl + 1, true, ZONE_APP);
    u32 p = hderef(h);
    wr16(p + MI_ID, (u16)id);
    wr32(p + MI_ENABLE, 0xFFFFFFFF);
    gmemmove(p + MI_DATA, tp, 1u + tl);
    wr8(p + MI_DATA + 1 + tl, 0);
    RET(h);
}

TRAP(GetMenu) {
    s16 id = ARGS16(0);
    u32 len;
    u8 *d = res_load_raw(FOURCC('M','E','N','U'), id, &len);
    if (!d) { RET(0); return; }
    u32 h = mm_handle_from_data(d, len, ZONE_APP);
    free(d);
    wr32(hderef(h) + MI_PROC, 0);
    calc_size(h);
    RET(h);
}

TRAP(DisposeMenu) { mm_dispose_handle(ARG(0)); }

TRAP(AppendMenu) {
    u32 m = ARG(0), dp = ARG(1);
    u8 n = rd8(dp);
    u8 buf[256];
    gmemcpy_from(buf, dp + 1, n);
    int start = 0;
    for (int i = 0; i <= n; i++) {
        if (i == n || buf[i] == ';' || buf[i] == '\r') {
            if (i > start) append_item(m, buf + start, i - start);
            start = i + 1;
        }
    }
    calc_size(m);
}
TRAP(AppendResMenu) { }
TRAP(InsertResMenu) { }

TRAP(InsertMenuItem) {
    u32 m = ARG(0), dp = ARG(1); s16 after = ARGS16(2);
    /* append then rotate into place */
    int before = count_items(m);
    u8 n = rd8(dp); u8 buf[256];
    gmemcpy_from(buf, dp + 1, n);
    append_item(m, buf, n);
    int total = count_items(m);
    if (after >= before || total <= 1) { calc_size(m); return; }
    Item it[256]; menu_items(m, it, 256);
    u32 newstart = it[total - 1].name, newend = it[total - 1].after;
    u32 insert_at = it[after].name;
    u32 len = newend - newstart;
    u8 *tmp = malloc(len);
    gmemcpy_from(tmp, newstart, len);
    gmemmove(insert_at + len, insert_at, newstart - insert_at);
    gmemcpy_to(insert_at, tmp, len);
    free(tmp);
    /* shift enable flags */
    u32 f = rd32(hderef(m) + MI_ENABLE);
    u32 lowmask = (1u << (after + 1)) - 1;
    bool en = (f >> total) & 1;
    u32 nf = (f & lowmask) | ((f & ~lowmask & ~(1u << total)) << 1) | ((u32)en << (after + 1));
    wr32(hderef(m) + MI_ENABLE, nf | 1);
    calc_size(m);
}

TRAP(DeleteMenuItem) {
    u32 m = ARG(0); s16 item = ARGS16(1);
    Item it[256];
    int n = menu_items(m, it, 256);
    if (item < 1 || item > n) return;
    u32 a = it[item - 1].name, b = it[item - 1].after;
    u32 end = hderef(m) + mm_handle_size(m);
    gmemmove(a, b, end - b);
    mm_set_handle_size(m, mm_handle_size(m) - (b - a));
    u32 f = rd32(hderef(m) + MI_ENABLE);
    u32 lowmask = (1u << item) - 1;
    f = (f & lowmask) | ((f >> 1) & ~lowmask);
    wr32(hderef(m) + MI_ENABLE, f);
    calc_size(m);
}

TRAP(InsertMenu) {
    u32 m = ARG(0); s16 before = ARGS16(1);
    s16 id = rds16(hderef(m) + MI_ID);
    if (before == -1) {
        for (int i = 0; i < g_nhier; i++) if (g_hier[i] == m) return;
        if (g_nhier < MAX_MENUS) g_hier[g_nhier++] = m;
        return;
    }
    for (int i = 0; i < g_nbar; i++) if (rds16(hderef(g_bar[i]) + MI_ID) == id) return;
    int pos = g_nbar;
    if (before) for (int i = 0; i < g_nbar; i++) if (rds16(hderef(g_bar[i]) + MI_ID) == before) { pos = i; break; }
    if (g_nbar >= MAX_MENUS) return;
    memmove(&g_bar[pos + 1], &g_bar[pos], sizeof(u32) * (size_t)(g_nbar - pos));
    g_bar[pos] = m;
    g_nbar++;
}

TRAP(DeleteMenu) {
    s16 id = ARGS16(0);
    for (int i = 0; i < g_nbar; i++) if (rds16(hderef(g_bar[i]) + MI_ID) == id) {
        memmove(&g_bar[i], &g_bar[i + 1], sizeof(u32) * (size_t)(g_nbar - i - 1)); g_nbar--; return; }
    for (int i = 0; i < g_nhier; i++) if (rds16(hderef(g_hier[i]) + MI_ID) == id) {
        memmove(&g_hier[i], &g_hier[i + 1], sizeof(u32) * (size_t)(g_nhier - i - 1)); g_nhier--; return; }
}

TRAP(ClearMenuBar) { g_nbar = 0; g_nhier = 0; }

static u32 menu_by_id(s16 id) {
    for (int i = 0; i < g_nbar; i++) if (rds16(hderef(g_bar[i]) + MI_ID) == id) return g_bar[i];
    for (int i = 0; i < g_nhier; i++) if (rds16(hderef(g_hier[i]) + MI_ID) == id) return g_hier[i];
    return 0;
}
TRAP(GetMenuHandle) { RET(menu_by_id(ARGS16(0))); }
TRAP(GetMHandle) { RET(menu_by_id(ARGS16(0))); }

/* ---- drawing helpers ---- */
static u32 g_mport;  /* port used for menu drawing (full screen) */

static void mport_prepare(void) {
    u32 port = wm_port();
    g_mport = port;
    rgn_set_rect(rd32(port + PORT_VIS), mkrect(0, 0, qd_screen_h(), qd_screen_w()));
    rgn_set_rect(rd32(port + PORT_CLIP), mkrect(-32767, -32767, 32767, 32767));
    s16 fam = rds16(LM_SysFontFam), size = rds16(LM_SysFontSize);
    if (size <= 0) size = 12;
    wr16(port + PORT_TXFONT, (u16)fam); wr16(port + PORT_TXSIZE, (u16)size); wr8(port + PORT_TXFACE, 0);
    if (fam == 0 && size == 12) { ITEM_H = 16; ITEM_BASE = 12; } /* Chicago 12 */
    else {
        int a, d, w, l;
        text_font_info(port, &a, &d, &w, &l);
        ITEM_H = a + d + l + 1;
        if (ITEM_H < 10) ITEM_H = 10;
        ITEM_BASE = a + (ITEM_H - (a + d + l)) / 2;
    }
    wr16(port + PORT_TXMODE, srcOr);
    wr_rgb(port + PORT_RGBFG, (RGB){ 0, 0, 0 });
    wr_rgb(port + PORT_RGBBK, (RGB){ 0xFFFF, 0xFFFF, 0xFFFF });
}
static void mport_done(void) {
    u32 port = wm_port();
    HRgn g; hrgn_from_guest(&g, rd32(LM_GrayRgn));
    hrgn_to_guest(&g, rd32(port + PORT_VIS));
    hrgn_free(&g);
}

static void fill(Rect r, RGB c) { Paint p = { .kind = 0, .fg = c }; draw_rect(g_mport, r, &p, patCopy); }

static const u8 GLYPH_APPLE[12] = { 0x04, 0x08, 0x08, 0x76, 0xFF, 0xFE, 0xFC, 0xFC, 0xFE, 0xFF, 0x7E, 0x24 };
static const u8 GLYPH_CHECK[9] = { 0x01, 0x02, 0x02, 0x04, 0x84, 0x48, 0x28, 0x10, 0x00 };
static const u8 GLYPH_CMD[9] = { 0x66, 0x99, 0x99, 0x7E, 0x18, 0x7E, 0x99, 0x99, 0x66 };

static void draw_glyph(const u8 *rows, int n, int x, int y, RGB c) {
    for (int r = 0; r < n; r++)
        for (int b = 0; b < 8; b++)
            if ((rows[r] >> (7 - b)) & 1) fill(mkrect(y + r, x + b, y + r + 1, x + b + 1), c);
}

static void draw_str(const u8 *s, int n, int x, int y, RGB c, int face) {
    u32 port = g_mport;
    wr_rgb(port + PORT_RGBFG, c);
    wr8(port + PORT_TXFACE, (u8)face);
    wr16(port + PORT_PNLOC + 2, (u16)x); wr16(port + PORT_PNLOC, (u16)y);
    text_draw(port, s, n);
    wr8(port + PORT_TXFACE, 0);
    wr_rgb(port + PORT_RGBFG, (RGB){ 0, 0, 0 });
}

static int title_width(u32 m) {
    u32 t = hderef(m) + MI_DATA;
    u8 n = rd8(t);
    if (n == 1 && rd8(t + 1) == 0x14) return 16;
    u8 buf[256]; gmemcpy_from(buf, t + 1, n);
    return text_width(g_mport, buf, n);
}

static void title_rect(int idx, Rect *r) {
    int x = 10;
    for (int i = 0; i < idx; i++) x += title_width(g_bar[i]) + 16;
    int mb = BAR_H;
    *r = mkrect(0, x, mb - 1, x + title_width(g_bar[idx]) + 16);
}

static void draw_title(int idx, bool hil) {
    Rect r; title_rect(idx, &r);
    fill(r, hil ? (RGB){ 0, 0, 0 } : (RGB){ 0xFFFF, 0xFFFF, 0xFFFF });
    u32 m = g_bar[idx];
    RGB c = hil ? (RGB){ 0xFFFF, 0xFFFF, 0xFFFF } : (RGB){ 0, 0, 0 };
    if (!(rd32(hderef(m) + MI_ENABLE) & 1) && !hil) c = (RGB){ 0x8888, 0x8888, 0x8888 };
    u32 t = hderef(m) + MI_DATA;
    u8 n = rd8(t);
    if (n == 1 && rd8(t + 1) == 0x14) draw_glyph(GLYPH_APPLE, 12, r.left + 8, 4, c);
    else { u8 buf[256]; gmemcpy_from(buf, t + 1, n); draw_str(buf, n, r.left + 8, 14, c, 0); }
}

void menu_draw_bar(void) {
    if (rds16(LM_MBarHeight) == 0) return; /* hidden by the application */
    mport_prepare();
    int mb = BAR_H;
    fill(mkrect(0, 0, mb - 1, qd_screen_w()), (RGB){ 0xFFFF, 0xFFFF, 0xFFFF });
    fill(mkrect(mb - 1, 0, mb, qd_screen_w()), (RGB){ 0, 0, 0 });
    for (int i = 0; i < g_nbar; i++) draw_title(i, rds16(hderef(g_bar[i]) + MI_ID) == g_hilited && g_hilited);
    mport_done();
    qd_screen_dirty();
}

TRAP(DrawMenuBar) { menu_draw_bar(); }
TRAP(InvalMenuBar) { menu_draw_bar(); }
TRAP(HiliteMenu) { g_hilited = ARGS16(0); menu_draw_bar(); }
TRAP(GetMBarHeight) { RET(rds16(LM_MBarHeight)); }
TRAP(FlashMenuBar) { }

static void calc_size(u32 m) {
    u32 save = qd_port();
    mport_prepare();
    Item it[256];
    int n = menu_items(m, it, 256);
    int w = 0;
    for (int i = 0; i < n; i++) {
        u8 l = rd8(it[i].name); u8 buf[256];
        gmemcpy_from(buf, it[i].name + 1, l);
        int iw = text_width(g_mport, buf, l) + 32 + (it[i].key > 32 ? 30 : 0) + (it[i].key == 0x1B ? 16 : 0);
        if (iw > w) w = iw;
    }
    if (w < 64) w = 64;
    wr16(hderef(m) + MI_WIDTH, (u16)w);
    wr16(hderef(m) + MI_HEIGHT, (u16)(n * ITEM_H));
    mport_done();
    qd_set_port(save);
}
TRAP(CalcMenuSize) { calc_size(ARG(0)); }
TRAP(CountMItems) { RET(count_items(ARG(0))); }
TRAP(CountMenuItems) { RET(count_items(ARG(0))); }

static void draw_item(u32 m, const Item *it, int idx, Rect mr, bool hil) {
    Rect r = mkrect(mr.top + idx * ITEM_H, mr.left, mr.top + (idx + 1) * ITEM_H, mr.right);
    bool sep = item_is_sep(m, idx, it);
    bool en = !sep && item_enabled(m, idx + 1);
    fill(r, hil && en ? (RGB){ 0, 0, 0 } : (RGB){ 0xFFFF, 0xFFFF, 0xFFFF });
    if (sep) {
        static const u8 dots[1] = { 0xAA };
        (void)dots;
        for (int x = r.left; x < r.right; x += 2) fill(mkrect(r.top + ITEM_H / 2, x, r.top + ITEM_H / 2 + 1, x + 1), (RGB){ 0x8888, 0x8888, 0x8888 });
        return;
    }
    RGB c = !en ? (RGB){ 0x8888, 0x8888, 0x8888 } : hil ? (RGB){ 0xFFFF, 0xFFFF, 0xFFFF } : (RGB){ 0, 0, 0 };
    if (it->mark == 0x12) draw_glyph(GLYPH_CHECK, 9, r.left + 3, r.top + (ITEM_H - 8) / 2, c);
    else if (it->mark && it->key != 0x1B) { u8 mc = it->mark; draw_str(&mc, 1, r.left + 4, r.top + ITEM_BASE, c, 0); }
    u8 l = rd8(it->name); u8 buf[256];
    gmemcpy_from(buf, it->name + 1, l);
    draw_str(buf, l, r.left + 16, r.top + ITEM_BASE, c, it->style);
    if (it->key > 32 && it->key != 0x1B) {
        draw_glyph(GLYPH_CMD, 9, r.right - 26, r.top + (ITEM_H - 8) / 2, c);
        u8 k = it->key;
        draw_str(&k, 1, r.right - 15, r.top + ITEM_BASE, c, 0);
    }
    if (it->key == 0x1B) {
        for (int k = 0; k < 5; k++) fill(mkrect(r.top + 4 + k, r.right - 12, r.top + 5 + k, r.right - 12 + k), c);
        for (int k = 0; k < 4; k++) fill(mkrect(r.top + 9 + k, r.right - 12, r.top + 10 + k, r.right - 8 - k), c);
    }
}

typedef struct { Rect r; u8 *save; } Saved;

static void save_under(Saved *s, Rect r) {
    s->r = r;
    int w = r.right - r.left, h = r.bottom - r.top;
    s->save = malloc((size_t)w * (size_t)h);
    u32 base = qd_screen_base();
    int row = qd_screen_w();
    row = (row + 3) & ~3;
    for (int y = 0; y < h; y++) {
        int sy = r.top + y;
        if (sy < 0 || sy >= qd_screen_h()) continue;
        for (int x = 0; x < w; x++) {
            int sx = r.left + x;
            s->save[(size_t)y * (size_t)w + (size_t)x] = (sx >= 0 && sx < qd_screen_w()) ? g_mem[base + (u32)(sy * row + sx)] : 0;
        }
    }
}
static void restore_under(Saved *s) {
    Rect r = s->r;
    int w = r.right - r.left, h = r.bottom - r.top;
    u32 base = qd_screen_base();
    int row = (qd_screen_w() + 3) & ~3;
    for (int y = 0; y < h; y++) {
        int sy = r.top + y;
        if (sy < 0 || sy >= qd_screen_h()) continue;
        for (int x = 0; x < w; x++) {
            int sx = r.left + x;
            if (sx >= 0 && sx < qd_screen_w()) g_mem[base + (u32)(sy * row + sx)] = s->save[(size_t)y * (size_t)w + (size_t)x];
        }
    }
    free(s->save);
    s->save = NULL;
    qd_screen_dirty();
}

static void draw_menu(u32 m, Rect mr, int hil_item) {
    fill(mkrect(mr.top - 1, mr.left - 1, mr.bottom + 1, mr.right + 1), (RGB){ 0, 0, 0 });
    fill(mkrect(mr.top + 2, mr.right + 1, mr.bottom + 2, mr.right + 2), (RGB){ 0, 0, 0 });
    fill(mkrect(mr.bottom + 1, mr.left + 2, mr.bottom + 2, mr.right + 2), (RGB){ 0, 0, 0 });
    Item it[256];
    int n = menu_items(m, it, 256);
    for (int i = 0; i < n; i++) draw_item(m, &it[i], i, mr, i + 1 == hil_item);
}

static int item_at(u32 m, Rect mr, Point p) {
    if (p.h < mr.left || p.h >= mr.right || p.v < mr.top || p.v >= mr.bottom) return 0;
    int i = (p.v - mr.top) / ITEM_H + 1;
    return i <= count_items(m) ? i : 0;
}

static void flash_item(u32 m, Rect mr, int item) {
    Item it[256]; menu_items(m, it, 256);
    for (int k = 0; k < 3; k++) {
        draw_item(m, &it[item - 1], item - 1, mr, false); qd_present();
        u32 t = tick_count(); while (tick_count() - t < 3) ev_idle_frame();
        draw_item(m, &it[item - 1], item - 1, mr, true); qd_present();
        t = tick_count(); while (tick_count() - t < 3) ev_idle_frame();
    }
}

static Rect menu_rect_for(int idx) {
    Rect tr; title_rect(idx, &tr);
    u32 m = g_bar[idx];
    int w = rds16(hderef(m) + MI_WIDTH), h = rds16(hderef(m) + MI_HEIGHT);
    int left = tr.left;
    if (left + w + 3 > qd_screen_w()) left = qd_screen_w() - w - 3;
    int mb = BAR_H;
    return mkrect(mb, left, mb + h, left + w);
}

static void draw_bar_now(void) {
    int mb = BAR_H;
    fill(mkrect(0, 0, mb - 1, qd_screen_w()), (RGB){ 0xFFFF, 0xFFFF, 0xFFFF });
    fill(mkrect(mb - 1, 0, mb, qd_screen_w()), (RGB){ 0, 0, 0 });
    for (int i = 0; i < g_nbar; i++) draw_title(i, false);
}


u32 menu_select(Point start) {
    (void)start;
    u32 save = qd_port();
    mport_prepare();
    bool hidden = rds16(LM_MBarHeight) == 0;
    Saved barsv = { { 0, 0, 0, 0 }, NULL };
    if (hidden) { save_under(&barsv, mkrect(0, 0, BAR_H, qd_screen_w())); draw_bar_now(); }
    int open = -1, hil = 0;
    Saved sv = { { 0, 0, 0, 0 }, NULL };
    Rect mr = { 0, 0, 0, 0 };
    u32 result = 0;
    for (;;) {
        bool down = ev_mouse_button();
        Point p = ev_mouse_global();
        int mb = BAR_H;
        int over = -1;
        if (p.v >= 0 && p.v < mb) {
            for (int i = 0; i < g_nbar; i++) { Rect tr; title_rect(i, &tr); if (p.h >= tr.left && p.h < tr.right) over = i; }
        }
        if (over >= 0 && over != open) {
            if (open >= 0) { restore_under(&sv); draw_title(open, false); }
            open = over; hil = 0;
            draw_title(open, true);
            u32 m = g_bar[open];
            if (rd32(hderef(m) + MI_ENABLE) & 1 || true) {
                mr = menu_rect_for(open);
                save_under(&sv, mkrect(mr.top - 1, mr.left - 1, mr.bottom + 2, mr.right + 2));
                draw_menu(m, mr, 0);
            }
        }
        if (open >= 0) {
            u32 m = g_bar[open];
            int it = item_at(m, mr, p);
            Item items[256]; int n = menu_items(m, items, 256);
            if (it && (it > n || item_is_sep(m, it - 1, &items[it - 1]) || !item_enabled(m, it))) it = 0;
            if (it != hil) {
                if (hil) draw_item(m, &items[hil - 1], hil - 1, mr, false);
                if (it) draw_item(m, &items[it - 1], it - 1, mr, true);
                hil = it;
            }
        }
        u32 hook = rd32(0x0A30);
        if (hook) call_upp(hook, 0, NULL);
        if (!down) break;
        ev_idle_frame();
    }
    if (open >= 0) {
        u32 m = g_bar[open];
        if (hil) {
            flash_item(m, mr, hil);
            result = (u32)(u16)rds16(hderef(m) + MI_ID) << 16 | (u16)hil;
            g_hilited = rds16(hderef(m) + MI_ID);
        }
        restore_under(&sv);
        if (!hil) draw_title(open, false);
    }
    if (hidden) { restore_under(&barsv); g_hilited = 0; }
    mport_done();
    qd_set_port(save);
    return result;
}

TRAP(MenuSelect) { RET(menu_select(pt_from_u32(ARG(0)))); }

static u32 menu_key(u8 ch) {
    if (ch >= 'a' && ch <= 'z') ch = (u8)(ch - 32);
    for (int i = 0; i < g_nbar; i++) {
        u32 m = g_bar[i];
        Item it[256];
        int n = menu_items(m, it, 256);
        for (int k = 0; k < n; k++) {
            u8 key = it[k].key;
            if (key >= 'a' && key <= 'z') key = (u8)(key - 32);
            if (key == ch && key > 32 && item_enabled(m, k + 1)) {
                g_hilited = rds16(hderef(m) + MI_ID);
                menu_draw_bar();
                return (u32)(u16)g_hilited << 16 | (u16)(k + 1);
            }
        }
    }
    return 0;
}
TRAP(MenuKey) { RET(menu_key((u8)ARG(0))); }
TRAP(MenuEvent) {
    u32 ep = ARG(0);
    if (!(rd16(ep + 14) & 0x0100)) { RET(0); return; }
    RET(menu_key((u8)rd32(ep + 2)));
}

/* A pop-up menu taller than the screen scrolls, as on the Mac: the visible
   part is `fr`, the whole menu `mr`; resting the mouse on the arrow at the
   top or bottom edge scrolls it. */
static void popup_paint(u32 m, Rect mr, Rect fr, int hil, const Item *items, int n) {
    fill(mkrect(fr.top - 1, fr.left - 1, fr.bottom + 1, fr.right + 1), (RGB){ 0, 0, 0 });
    fill(mkrect(fr.top + 2, fr.right + 1, fr.bottom + 2, fr.right + 2), (RGB){ 0, 0, 0 });
    fill(mkrect(fr.bottom + 1, fr.left + 2, fr.bottom + 2, fr.right + 2), (RGB){ 0, 0, 0 });
    u32 clip = rd32(g_mport + PORT_CLIP);
    rgn_set_rect(clip, fr);
    for (int i = 0; i < n; i++) {
        int t = mr.top + i * ITEM_H;
        if (t + ITEM_H > fr.top && t < fr.bottom) draw_item(m, &items[i], i, mr, i + 1 == hil);
    }
    int cx = (fr.left + fr.right) / 2;
    if (mr.top < fr.top) {
        fill(mkrect(fr.top, fr.left, fr.top + ITEM_H, fr.right), (RGB){ 0xFFFF, 0xFFFF, 0xFFFF });
        for (int k = 0; k < 5; k++) fill(mkrect(fr.top + 5 + k, cx - k, fr.top + 6 + k, cx + k + 1), (RGB){ 0, 0, 0 });
    }
    if (mr.bottom > fr.bottom) {
        fill(mkrect(fr.bottom - ITEM_H, fr.left, fr.bottom, fr.right), (RGB){ 0xFFFF, 0xFFFF, 0xFFFF });
        for (int k = 0; k < 5; k++) fill(mkrect(fr.bottom - 6 - k, cx - k, fr.bottom - 5 - k, cx + k + 1), (RGB){ 0, 0, 0 });
    }
    rgn_set_rect(clip, mkrect(-32767, -32767, 32767, 32767));
}

TRAP(PopUpMenuSelect) {
    u32 m = ARG(0); s16 top = ARGS16(1), left = ARGS16(2); s16 popitem = ARGS16(3);
    u32 save = qd_port();
    calc_size(m);
    mport_prepare();
    int w = rds16(hderef(m) + MI_WIDTH), h = rds16(hderef(m) + MI_HEIGHT);
    int y = top - (popitem > 0 ? (popitem - 1) * ITEM_H : 0);
    int vt = rds16(LM_MBarHeight), vb = qd_screen_h() - 2;
    Rect fr;
    if (h <= vb - vt) {
        if (y < vt) y = vt;
        if (y + h > vb) y = vb - h;
        fr = mkrect(y, 0, y + h, 0);
    } else { /* too tall: show what fits, keeping popitem under the mouse */
        if (y > vt) y = vt;
        if (y + h < vb) y = vb - h;
        fr = mkrect(vt, 0, vt + (vb - vt) / ITEM_H * ITEM_H, 0);
    }
    int x = left;
    if (x + w > qd_screen_w() - 2) x = qd_screen_w() - 2 - w;
    Rect mr = mkrect(y, x, y + h, x + w);
    fr.left = mr.left; fr.right = mr.right;
    Saved sv;
    save_under(&sv, mkrect(fr.top - 1, fr.left - 1, fr.bottom + 2, fr.right + 2));
    Item items[256]; int n = menu_items(m, items, 256);
    popup_paint(m, mr, fr, 0, items, n);
    int hil = 0;
    u32 last_scroll = tick_count();
    for (;;) {
        bool down = ev_mouse_button();
        Point pt = ev_mouse_global();
        bool in = pt.h >= fr.left && pt.h < fr.right;
        int dir = 0;
        if (in && mr.top < fr.top && pt.v >= fr.top - 8 && pt.v < fr.top + ITEM_H) dir = 1;
        if (in && mr.bottom > fr.bottom && pt.v >= fr.bottom - ITEM_H && pt.v < fr.bottom + 8) dir = -1;
        if (dir) {
            if (tick_count() - last_scroll >= 4) {
                last_scroll = tick_count();
                mr.top = (s16)(mr.top + dir * ITEM_H); mr.bottom = (s16)(mr.bottom + dir * ITEM_H);
                hil = 0;
                popup_paint(m, mr, fr, 0, items, n);
            }
        }
        int it = !dir && pt.v >= fr.top && pt.v < fr.bottom ? item_at(m, mr, pt) : 0;
        if (it && (it > n || item_is_sep(m, it - 1, &items[it - 1]) || !item_enabled(m, it))) it = 0;
        if (it != hil) {
            u32 clip = rd32(g_mport + PORT_CLIP);
            rgn_set_rect(clip, fr);
            if (hil) draw_item(m, &items[hil - 1], hil - 1, mr, false);
            if (it) draw_item(m, &items[it - 1], it - 1, mr, true);
            rgn_set_rect(clip, mkrect(-32767, -32767, 32767, 32767));
            hil = it;
        }
        if (!down) break;
        ev_idle_frame();
    }
    if (hil) {
        u32 clip = rd32(g_mport + PORT_CLIP);
        rgn_set_rect(clip, fr);
        flash_item(m, mr, hil);
        rgn_set_rect(clip, mkrect(-32767, -32767, 32767, 32767));
    }
    restore_under(&sv);
    mport_done();
    qd_set_port(save);
    RET(hil ? ((u32)(u16)rds16(hderef(m) + MI_ID) << 16 | (u16)hil) : 0);
}

/* ---- item attributes ---- */
TRAP(EnableItem) {
    u32 m = ARG(0); s16 item = ARGS16(1);
    if (item > 31) return;
    wr32(hderef(m) + MI_ENABLE, rd32(hderef(m) + MI_ENABLE) | (1u << item));
}
TRAP(DisableItem) {
    u32 m = ARG(0); s16 item = ARGS16(1);
    if (item > 31) return;
    wr32(hderef(m) + MI_ENABLE, rd32(hderef(m) + MI_ENABLE) & ~(1u << item));
}
TRAP(EnableMenuItem) { u32 m = ARG(0); s16 i = ARGS16(1); if (i <= 31) wr32(hderef(m) + MI_ENABLE, rd32(hderef(m) + MI_ENABLE) | (1u << i)); }
TRAP(DisableMenuItem) { u32 m = ARG(0); s16 i = ARGS16(1); if (i <= 31) wr32(hderef(m) + MI_ENABLE, rd32(hderef(m) + MI_ENABLE) & ~(1u << i)); }

static Item *get_item(u32 m, int item, Item *buf) {
    int n = menu_items(m, buf, 256);
    return (item >= 1 && item <= n) ? &buf[item - 1] : NULL;
}
TRAP(CheckItem) {
    Item b[256]; Item *it = get_item(ARG(0), ARGS16(1), b);
    if (it) wr8(it->name + 1 + rd8(it->name) + 2, ARGB(2) ? 0x12 : 0);
}
TRAP(CheckMenuItem) {
    Item b[256]; Item *it = get_item(ARG(0), ARGS16(1), b);
    if (it) wr8(it->name + 1 + rd8(it->name) + 2, ARGB(2) ? 0x12 : 0);
}
TRAP(SetItemMark) {
    Item b[256]; Item *it = get_item(ARG(0), ARGS16(1), b);
    if (it) wr8(it->name + 1 + rd8(it->name) + 2, (u8)ARG(2));
}
TRAP(GetItemMark) {
    Item b[256]; Item *it = get_item(ARG(0), ARGS16(1), b);
    wr16(ARG(2), it ? it->mark : 0);
}
TRAP(SetItemStyle) {
    Item b[256]; Item *it = get_item(ARG(0), ARGS16(1), b);
    if (it) wr8(it->name + 1 + rd8(it->name) + 3, (u8)ARG(2));
}
TRAP(GetItemStyle) {
    Item b[256]; Item *it = get_item(ARG(0), ARGS16(1), b);
    wr8(ARG(2), it ? it->style : 0);
}
TRAP(SetItemCmd) {
    Item b[256]; Item *it = get_item(ARG(0), ARGS16(1), b);
    if (it) wr8(it->name + 1 + rd8(it->name) + 1, (u8)ARG(2));
}
TRAP(GetItemCmd) {
    Item b[256]; Item *it = get_item(ARG(0), ARGS16(1), b);
    wr16(ARG(2), it ? it->key : 0);
}
TRAP(GetMenuItemText) {
    Item b[256]; Item *it = get_item(ARG(0), ARGS16(1), b);
    u32 out = ARG(2);
    if (it) gmemmove(out, it->name, 1u + rd8(it->name)); else wr8(out, 0);
}
TRAP(SetMenuItemText) {
    u32 m = ARG(0); s16 item = ARGS16(1); u32 sp = ARG(2);
    Item b[256]; Item *it = get_item(m, item, b);
    if (!it) return;
    u8 oldn = rd8(it->name), newn = rd8(sp);
    u32 off = it->name - hderef(m);
    u32 size = mm_handle_size(m);
    u8 tmp[256]; gmemcpy_from(tmp, sp, 1u + newn);
    if (newn > oldn) mm_set_handle_size(m, size + (u32)(newn - oldn));
    u32 p = hderef(m) + off;
    u32 tail = size - (off + 1 + oldn);
    gmemmove(p + 1 + newn, p + 1 + oldn, tail);
    gmemcpy_to(p, tmp, 1u + newn);
    if (newn < oldn) mm_set_handle_size(m, size - (u32)(oldn - newn));
    calc_size(m);
}
TRAP(GetMenuItemCommandID) { u32 p = ARG(2); if (p) wr32(p, 0); RETERR(-5623); }

void menu_init(void) { g_nbar = 0; g_nhier = 0; }
TRAP(InitMenus) { menu_init(); menu_draw_bar(); }
TRAP(InitProcMenu) { }
