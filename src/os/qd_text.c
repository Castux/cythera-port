/* Font Manager and QuickDraw text.
 *
 * Fonts are resolved like the Mac Font Manager: family ID -> FOND in the
 * resource chain -> font association table -> NFNT/FONT bitmap strike or
 * 'sfnt' TrueType outlines (rasterised with stb_truetype). Missing system
 * fonts fall back to a built-in proportional bitmap font. QuickDraw style
 * variations (bold, italic, underline, outline, shadow, condense, extend)
 * are synthesised.
 */
#include "qd.h"
#include "resources.h"
#include "../../third_party/font8x8_basic.h"
#define STB_TRUETYPE_IMPLEMENTATION

#include "../../third_party/stb_truetype.h"

#define LM_ApFontID 0x0984
#define LM_SysFontFam 0x0BA6

enum { bold = 1, italic = 2, underline = 4, outline = 8, shadow = 16, condense = 32, extend = 64 };

typedef struct {
    s16 adv;        /* advance width */
    s16 xoff;       /* offset of image from pen (kern + offset) */
    s16 w;          /* image width */
    u8 *img;        /* w * height bytes, 1 = ink; rows from top of font rect */
} Glyph;

typedef struct {
    bool used;
    int family, size, face; /* face = style bits that the font itself provides */
    int ascent, descent, leading, widmax;
    int height;             /* rows in glyph images (ascent + descent) */
    Glyph g[256];
    unsigned gen;
} Font;

#define FONT_CACHE 48
static Font g_fonts[FONT_CACHE];
static int g_font_next;
static unsigned g_font_gen = 1;

void text_invalidate_fonts(void) { g_font_gen++; }

/* ---- Mac Roman -> Unicode for TrueType cmap lookups ---- */
static const u16 mr_hi[128] = {
    0x00C4,0x00C5,0x00C7,0x00C9,0x00D1,0x00D6,0x00DC,0x00E1,0x00E0,0x00E2,0x00E4,0x00E3,0x00E5,0x00E7,0x00E9,0x00E8,
    0x00EA,0x00EB,0x00ED,0x00EC,0x00EE,0x00EF,0x00F1,0x00F3,0x00F2,0x00F4,0x00F6,0x00F5,0x00FA,0x00F9,0x00FB,0x00FC,
    0x2020,0x00B0,0x00A2,0x00A3,0x00A7,0x2022,0x00B6,0x00DF,0x00AE,0x00A9,0x2122,0x00B4,0x00A8,0x2260,0x00C6,0x00D8,
    0x221E,0x00B1,0x2264,0x2265,0x00A5,0x00B5,0x2202,0x2211,0x220F,0x03C0,0x222B,0x00AA,0x00BA,0x03A9,0x00E6,0x00F8,
    0x00BF,0x00A1,0x00AC,0x221A,0x0192,0x2248,0x2206,0x00AB,0x00BB,0x2026,0x00A0,0x00C0,0x00C3,0x00D5,0x0152,0x0153,
    0x2013,0x2014,0x201C,0x201D,0x2018,0x2019,0x00F7,0x25CA,0x00FF,0x0178,0x2044,0x20AC,0x2039,0x203A,0xFB01,0xFB02,
    0x2021,0x00B7,0x201A,0x201E,0x2030,0x00C2,0x00CA,0x00C1,0x00CB,0x00C8,0x00CD,0x00CE,0x00CF,0x00CC,0x00D3,0x00D4,
    0xF8FF,0x00D2,0x00DA,0x00DB,0x00D9,0x0131,0x02C6,0x02DC,0x00AF,0x02D8,0x02D9,0x02DA,0x00B8,0x02DD,0x02DB,0x02C7
};

/* ---- built-in fallback font (public-domain font8x8, made proportional) ---- */
static void builtin_font(Font *f, int size) {
    int scale = size >= 14 ? 2 : 1;
    f->ascent = 7 * scale; f->descent = 2 * scale; f->leading = scale; f->height = f->ascent + f->descent;
    f->widmax = 0;
    for (int c = 0; c < 256; c++) {
        Glyph *g = &f->g[c];
        int src = c < 128 ? c : '?';
        if (c >= 128) {
            /* approximate accented letters by their base letter */
            u16 u = mr_hi[c - 128];
            static const char *acc = "AACENOUaaaaaaceeeeiiiinooooouuuu";
            if (c - 128 < 32) src = acc[c - 128];
            else if (u == 0x2026) src = '.';
            else if (u == 0x2018 || u == 0x2019) src = '\'';
            else if (u == 0x201C || u == 0x201D) src = '"';
            else if (u == 0x2013 || u == 0x2014) src = '-';
            else if (u == 0x2022) src = '*';
            else src = '?';
        }
        if (c < 32 && c != 9) { g->adv = 0; g->w = 0; g->img = NULL; continue; }
        const char *bm = font8x8_basic[src];
        int minx = 8, maxx = -1;
        for (int y = 0; y < 8; y++) for (int x = 0; x < 8; x++) if ((bm[y] >> x) & 1) { if (x < minx) minx = x; if (x > maxx) maxx = x; }
        if (maxx < 0) { g->adv = (s16)(4 * scale); g->w = 0; g->img = NULL; continue; }
        int w = (maxx - minx + 1) * scale;
        g->w = (s16)w; g->xoff = 0; g->adv = (s16)(w + scale);
        g->img = calloc((size_t)w * (size_t)f->height, 1);
        for (int y = 0; y < 8; y++)
            for (int x = minx; x <= maxx; x++)
                if ((bm[y] >> x) & 1)
                    for (int sy = 0; sy < scale; sy++)
                        for (int sx = 0; sx < scale; sx++) {
                            int yy = y * scale + sy;
                            if (yy < f->height) g->img[(size_t)yy * (size_t)w + (size_t)((x - minx) * scale + sx)] = 1;
                        }
        if (g->adv > f->widmax) f->widmax = g->adv;
    }
    if (f->g[' '].adv == 0) f->g[' '].adv = (s16)(4 * scale);
}

/* ---- NFNT/FONT ---- */
static bool load_nfnt(Font *f, const u8 *d, u32 len, int target_size, int native_size) {
    if (len < 26) return false;
    int first = (s16)be16(d + 2), last = (s16)be16(d + 4);
    int kernmax = (s16)be16(d + 8);
    int frw = be16(d + 12), frh = be16(d + 14);
    int ascent = (s16)be16(d + 18), descent = (s16)be16(d + 20), leading = (s16)be16(d + 22);
    int rowwords = be16(d + 24);
    (void)frw;
    int n = last - first + 3;
    u32 bits_off = 26, loc_off = bits_off + (u32)(rowwords * 2 * frh), ow_off = loc_off + (u32)(n * 2);
    /* some fonts omit the final owTable entry; tolerate a short table */
    if (ow_off + (u32)(n - 1) * 2 > len || first < 0 || last > 255 || first > last) return false;
    double sc = native_size > 0 ? (double)target_size / native_size : 1.0;
    if (sc < 0.25) sc = 1;
    int H = (int)(frh * sc + 0.5);
    if (H < 1) H = 1;
    f->ascent = (int)(ascent * sc + 0.5); f->descent = (int)(descent * sc + 0.5);
    f->leading = (int)(leading * sc + 0.5);
    f->height = H;
    f->widmax = (int)((s16)be16(d + 6) * sc + 0.5);
    const u8 *bits = d + bits_off;
    int rowbytes = rowwords * 2;
    for (int c = 0; c < 256; c++) {
        int ci = (c >= first && c <= last) ? c - first : last - first + 1; /* missing glyph */
#define OWT(k) (ow_off + 2 * (u32)(k) + 2 <= len ? be16(d + ow_off + 2 * (u32)(k)) : 0xFFFF)
        u16 ow = OWT(ci);
        if (ow == 0xFFFF) {
            ci = last - first + 1;
            ow = OWT(ci);
            if (ow == 0xFFFF) { f->g[c].adv = 0; continue; }
            if (c < 32) { f->g[c].adv = 0; f->g[c].w = 0; continue; }
        }
        u16 loc = be16(d + loc_off + 2 * (u32)ci), nloc = be16(d + loc_off + 2 * (u32)(ci + 1));
        int bw = nloc - loc;
        Glyph *g = &f->g[c];
        g->adv = (s16)((ow & 0xFF) * sc + 0.5);
        g->xoff = (s16)((kernmax + (ow >> 8)) * sc);
        int W = bw > 0 ? (int)(bw * sc + 0.5) : 0;
        if (bw > 0 && W < 1) W = 1;
        g->w = (s16)W;
        if (W <= 0) { g->img = NULL; continue; }
        g->img = calloc((size_t)W * (size_t)H, 1);
        for (int y = 0; y < H; y++) {
            int sy = (int)(y / sc);
            if (sy >= frh) sy = frh - 1;
            for (int x = 0; x < W; x++) {
                int sx = loc + (int)(x / sc);
                if (sx >= nloc) sx = nloc - 1;
                if ((bits[sy * rowbytes + (sx >> 3)] >> (7 - (sx & 7))) & 1) g->img[(size_t)y * (size_t)W + (size_t)x] = 1;
            }
        }
    }
    return true;
}

/* ---- TrueType ---- */
static bool load_sfnt(Font *f, const u8 *d, u32 len, int size) {
    stbtt_fontinfo fi;
    if (!stbtt_InitFont(&fi, d, stbtt_GetFontOffsetForIndex(d, 0))) return false;
    (void)len;
    float sc = stbtt_ScaleForMappingEmToPixels(&fi, (float)size);
    int asc, desc, gap;
    stbtt_GetFontVMetrics(&fi, &asc, &desc, &gap);
    f->ascent = (int)(asc * sc + 0.99f);
    f->descent = (int)(-desc * sc + 0.99f);
    f->leading = (int)(gap * sc + 0.5f);
    f->height = f->ascent + f->descent;
    f->widmax = 0;
    for (int c = 0; c < 256; c++) {
        Glyph *g = &f->g[c];
        if (c < 32) { g->adv = 0; g->w = 0; g->img = NULL; continue; }
        int cp = c < 128 ? c : mr_hi[c - 128];
        int gi = stbtt_FindGlyphIndex(&fi, cp);
        if (!gi && c >= 128) gi = stbtt_FindGlyphIndex(&fi, c);
        int adv, lsb;
        stbtt_GetGlyphHMetrics(&fi, gi, &adv, &lsb);
        g->adv = (s16)(adv * sc + 0.5f);
        int x0, y0, x1, y1;
        stbtt_GetGlyphBitmapBox(&fi, gi, sc, sc, &x0, &y0, &x1, &y1);
        int w = x1 - x0, h = y1 - y0;
        if (w <= 0 || h <= 0) { g->w = 0; g->img = NULL; continue; }
        u8 *tmp = calloc((size_t)w * (size_t)h, 1);
        stbtt_MakeGlyphBitmap(&fi, tmp, w, h, w, sc, sc, gi);
        g->w = (s16)w; g->xoff = (s16)x0;
        g->img = calloc((size_t)w * (size_t)f->height, 1);
        for (int y = 0; y < h; y++) {
            int yy = f->ascent + y0 + y;
            if (yy < 0 || yy >= f->height) continue;
            const u8 *src = tmp + (size_t)y * (size_t)w;
            u8 *dst = g->img + (size_t)yy * (size_t)w;
            int on = 0, best = 0;
            for (int x = 0; x < w; x++) { dst[x] = src[x] >= 128; on |= dst[x]; if (src[x] > src[best]) best = x; }
            /* a thin stem straddling two pixel columns has <50% coverage in
               each: keep its strongest pixel so i, l, | don't vanish */
            if (!on && src[best] >= 48) dst[best] = 1;
        }
        free(tmp);
        if (g->adv > f->widmax) f->widmax = g->adv;
    }
    return true;
}

/* ---- host font substitution for classic system families ---- */
static const char *const *host_font_candidates(int family) {
    static const char *const chicago[] = { "Charcoal.ttf", "ChicagoFLF.ttf", "Chicago.ttf", "/System/Library/Fonts/Geneva.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf", "C:/Windows/Fonts/tahomabd.ttf", NULL };
    static const char *const geneva[] = { "Geneva.ttf", "/System/Library/Fonts/Geneva.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", "C:/Windows/Fonts/tahoma.ttf", NULL };
    static const char *const monaco[] = { "Monaco.ttf", "/System/Library/Fonts/Monaco.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf", "C:/Windows/Fonts/consola.ttf", NULL };
    static const char *const times[] = { "Times.ttf", "/System/Library/Fonts/Times.ttc",
        "/usr/share/fonts/truetype/dejavu/DejaVuSerif.ttf", "C:/Windows/Fonts/times.ttf", NULL };
    static const char *const helv[] = { "Helvetica.ttf", "/System/Library/Fonts/Helvetica.ttc",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", "C:/Windows/Fonts/arial.ttf", NULL };
    static const char *const courier[] = { "Courier.ttf", "/System/Library/Fonts/Courier.ttc",
        "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf", "C:/Windows/Fonts/cour.ttf", NULL };
    switch (family) {
    case 0: return chicago;
    case 4: return monaco;
    case 2: case 20: return times;
    case 21: return helv;
    case 22: return courier;
    default: return geneva;
    }
}

static u8 *load_host_font(int family, u32 *len) {
    static struct { int family; u8 *data; u32 len; bool tried; } cache[8];
    int slot = family == 0 ? 0 : family == 4 ? 1 : (family == 2 || family == 20) ? 2 : family == 21 ? 3 : family == 22 ? 4 : 5;
    if (cache[slot].tried) { *len = cache[slot].len; return cache[slot].data; }
    cache[slot].tried = true;
    const char *dir = getenv("CYTHERA_FONT_DIR");
    for (const char *const *c = host_font_candidates(family); *c; c++) {
        char path[1024];
        if ((*c)[0] == '/' || (*c)[1] == ':') snprintf(path, sizeof path, "%s", *c);
        else if (dir) snprintf(path, sizeof path, "%s/%s", dir, *c);
        else continue;
        FILE *f = fopen(path, "rb");
        if (!f) continue;
        fseek(f, 0, SEEK_END);
        long n = ftell(f);
        fseek(f, 0, SEEK_SET);
        u8 *d = malloc((size_t)n);
        if (fread(d, 1, (size_t)n, f) == (size_t)n) {
            fclose(f);
            cache[slot].data = d; cache[slot].len = (u32)n;
            LOG_I("font family %d substituted by %s", family, path);
            *len = (u32)n;
            return d;
        }
        fclose(f);
        free(d);
    }
    *len = 0;
    return NULL;
}

/* Resolve family/size into a Font (without synthesised styles). */
static Font *get_font(int family, int size) {
    if (family == 1) family = rds16(LM_ApFontID);
    if (size <= 0) size = 12;
    for (int i = 0; i < FONT_CACHE; i++)
        if (g_fonts[i].used && g_fonts[i].family == family && g_fonts[i].size == size && g_fonts[i].gen == g_font_gen)
            return &g_fonts[i];
    Font *f = &g_fonts[g_font_next];
    g_font_next = (g_font_next + 1) % FONT_CACHE;
    for (int c = 0; c < 256; c++) free(f->g[c].img);
    memset(f, 0, sizeof *f);
    f->used = true; f->family = family; f->size = size; f->gen = g_font_gen;

    bool ok = false;
    u32 len;
    u8 *fond = res_load_raw(FOURCC('F','O','N','D'), (s16)family, &len);
    if (fond && len >= 54) {
        int nassoc = (s16)be16(fond + 52) + 1;
        int best = -1, bestdiff = 1 << 30, bestsize = 0, sfnt_id = -1;
        for (int i = 0; i < nassoc && 54 + 6 * (u32)(i + 1) <= len; i++) {
            const u8 *e = fond + 54 + 6 * i;
            int fs = be16(e), st = be16(e + 2), id = (s16)be16(e + 4);
            if (st != 0) continue;
            if (fs == 0) { sfnt_id = id; continue; }
            int diff = abs(fs - size) * 2 + (fs < size ? 1 : 0);
            if (diff < bestdiff) { bestdiff = diff; best = id; bestsize = fs; }
        }
        if (best >= 0 && bestsize == size) {
            u8 *nf = res_load_raw(FOURCC('N','F','N','T'), (s16)best, &len);
            if (!nf) nf = res_load_raw(FOURCC('F','O','N','T'), (s16)best, &len);
            if (nf) { ok = load_nfnt(f, nf, len, size, size); free(nf); }
        }
        if (!ok && sfnt_id >= 0) {
            u8 *tt = res_load_raw(FOURCC('s','f','n','t'), (s16)sfnt_id, &len);
            if (tt) { ok = load_sfnt(f, tt, len, size); free(tt); }
        }
        if (!ok && best >= 0) {
            u8 *nf = res_load_raw(FOURCC('N','F','N','T'), (s16)best, &len);
            if (!nf) nf = res_load_raw(FOURCC('F','O','N','T'), (s16)best, &len);
            if (nf) { ok = load_nfnt(f, nf, len, size, bestsize); free(nf); }
        }
    }
    if (!ok && !fond) {
        u32 hl;
        u8 *hd = load_host_font(family, &hl);
        if (hd) ok = load_sfnt(f, hd, hl, size);
    }
    LOG_D("get_font(%d, %d): FOND %s, %s", family, size, fond ? "found" : "missing", ok ? "loaded" : "builtin fallback");
    free(fond);
    if (!ok) builtin_font(f, size);
    return f;
}

/* ---- styled layout ---- */
typedef struct { Font *f; int face; int extra; int ascent, descent, leading, widmax; } Styled;

static void styled(u32 port, Styled *s) {
    int family = rds16(port + PORT_TXFONT);
    int size = rds16(port + PORT_TXSIZE);
    s->face = rd8(port + PORT_TXFACE);
    s->f = get_font(family, size);
    s->extra = 0;
    if (s->face & bold) s->extra += 1;
    if (s->face & outline) s->extra += 1;
    if (s->face & shadow) s->extra += 2;
    if (s->face & condense) s->extra -= 1;
    if (s->face & extend) s->extra += 1;
    s->ascent = s->f->ascent;
    s->descent = s->f->descent + ((s->face & (shadow)) ? 1 : 0);
    s->leading = s->f->leading;
    s->widmax = s->f->widmax + s->extra;
}

static int char_adv(u32 port, const Styled *s, u8 c) {
    int a = s->f->g[c].adv;
    if (a == 0 && c != ' ') return 0;
    a += s->extra;
    if (c == ' ') a += (s32)rd32(port + PORT_SPEXTRA) >> 16;
    if (is_color_port(port)) a += rds16(port + PORT_CHEXTRA) >> 12;
    return a;
}

int text_width(u32 port, const u8 *str, int n) {
    Styled s; styled(port, &s);
    int w = 0;
    for (int i = 0; i < n; i++) w += char_adv(port, &s, str[i]);
    /* fractional spExtra accumulates */
    return w;
}

void text_font_info(u32 port, int *ascent, int *descent, int *widmax, int *leading) {
    Styled s; styled(port, &s);
    *ascent = s.ascent; *descent = s.descent; *widmax = s.widmax; *leading = s.leading;
}

void text_draw(u32 port, const u8 *str, int n) {
    Styled s; styled(port, &s);
    int x0 = rds16(port + PORT_PNLOC + 2), y0 = rds16(port + PORT_PNLOC);
    int total = 0;
    for (int i = 0; i < n; i++) total += char_adv(port, &s, str[i]);
    if (pict_recording(port)) {
        int x = x0;
        for (int i = 0; i < n; i += 255) {
            int k = n - i < 255 ? n - i : 255;
            pict_rec_text(port, x, y0, str + i, k);
            for (int j = 0; j < k; j++) x += char_adv(port, &s, str[i + j]);
        }
    }
    if (rds16(port + PORT_PNVIS) < 0) { wr16(port + PORT_PNLOC + 2, (u16)(x0 + total)); return; }
    int pad = 4 + (s.face & italic ? s.ascent / 2 : 0);
    int W = total + 2 * pad + s.f->widmax, H = s.f->height + 4;
    int top = y0 - s.ascent - 1;
    int left = x0 - pad;
    u8 *m = calloc((size_t)W * (size_t)H, 1);
    int pen = pad;
    for (int i = 0; i < n; i++) {
        Glyph *g = &s.f->g[str[i]];
        if (g->img) {
            for (int y = 0; y < s.f->height; y++) {
                int sh = (s.face & italic) ? (s.ascent - y) / 2 : 0;
                for (int x = 0; x < g->w; x++) {
                    if (!g->img[(size_t)y * (size_t)g->w + (size_t)x]) continue;
                    int X = pen + g->xoff + x + sh, Y = y + 1;
                    if (X < 0 || X >= W || Y < 0 || Y >= H) continue;
                    m[(size_t)Y * (size_t)W + (size_t)X] = 1;
                    if ((s.face & bold) && X + 1 < W) m[(size_t)Y * (size_t)W + (size_t)X + 1] = 1;
                }
            }
        }
        pen += char_adv(port, &s, str[i]);
    }
    if (s.face & underline) {
        int Y = s.ascent + 2;
        if (Y < H) for (int x = pad; x < pad + total; x++) m[(size_t)Y * (size_t)W + (size_t)x] = 1;
    }
    u8 *ink = m;
    u8 *fill = NULL;
    if (s.face & (outline | shadow)) {
        /* outline: dilate, keep interior hollow */
        fill = m;
        ink = calloc((size_t)W * (size_t)H, 1);
        int so = (s.face & shadow) ? 1 : 0;
        for (int y = 0; y < H; y++)
            for (int x = 0; x < W; x++) {
                if (!m[(size_t)y * (size_t)W + (size_t)x]) continue;
                for (int dy = -1; dy <= 1 + so; dy++)
                    for (int dx = -1; dx <= 1 + so; dx++) {
                        int X = x + dx, Y = y + dy;
                        if (X >= 0 && X < W && Y >= 0 && Y < H) ink[(size_t)Y * (size_t)W + (size_t)X] = 1;
                    }
            }
        for (size_t i = 0; i < (size_t)W * (size_t)H; i++) if (fill[i]) ink[i] = 0;
    }
    HRgn r;
    hrgn_from_mask(&r, ink, W, H, left, top);
    int mode = rds16(port + PORT_TXMODE) & 0x7F;
    Paint p = { .kind = 0, .fg = port_fg(port), .bk = port_bk(port) };
    switch (mode) {
    case srcCopy: {
        HRgn box, bg = { 0, NULL };
        hrgn_rect(&box, y0 - s.ascent, x0, y0 + s.descent + s.leading, x0 + total);
        hrgn_op(&bg, &box, &r, 2);
        Paint b = { .kind = 0, .fg = p.bk };
        draw_hrgn(port, &bg, &b, patCopy);
        hrgn_free(&box); hrgn_free(&bg);
        draw_hrgn(port, &r, &p, patCopy);
        break;
    }
    case srcXor: case notSrcXor: draw_hrgn(port, &r, &p, patXor); break;
    case srcBic: { Paint b = { .kind = 0, .fg = p.bk }; draw_hrgn(port, &r, &b, patCopy); break; }
    case 49: { /* grayishTextOr */
        Paint g = { .kind = 0, .fg = { (u16)((p.fg.r + p.bk.r) / 2), (u16)((p.fg.g + p.bk.g) / 2), (u16)((p.fg.b + p.bk.b) / 2) } };
        draw_hrgn(port, &r, &g, patCopy);
        break;
    }
    default: draw_hrgn(port, &r, &p, patCopy); break;
    }
    hrgn_free(&r);
    if (ink != m) free(ink);
    free(m);
    wr16(port + PORT_PNLOC + 2, (u16)(x0 + total));
}

/* ---------------------------------------------------------------------- */
/* Traps                                                                   */

TRAP(InitFonts) { }
TRAP(TextFont) { wr16(qd_port() + PORT_TXFONT, (u16)ARGS16(0)); }
TRAP(TextFace) { wr8(qd_port() + PORT_TXFACE, (u8)ARG(0)); }
TRAP(TextMode) { wr16(qd_port() + PORT_TXMODE, (u16)ARGS16(0)); }
TRAP(TextSize) { wr16(qd_port() + PORT_TXSIZE, (u16)ARGS16(0)); }
TRAP(SpaceExtra) { wr32(qd_port() + PORT_SPEXTRA, ARG(0)); }
TRAP(CharExtra) {
    u32 port = qd_port();
    if (is_color_port(port)) wr16(port + PORT_CHEXTRA, (u16)((s32)ARG(0) >> 4));
}

TRAP(DrawChar) { u8 c = (u8)ARG(0); text_draw(qd_port(), &c, 1); }
TRAP(DrawString) {
    u32 s = ARG(0);
    u8 n = rd8(s);
    text_draw(qd_port(), gptr(s + 1, n), n);
}
TRAP(DrawText) {
    u32 buf = ARG(0); s16 first = ARGS16(1), count = ARGS16(2);
    if (count <= 0) return;
    u8 *tmp = malloc((size_t)count);
    gmemcpy_from(tmp, buf + (u32)first, (u32)count);
    text_draw(qd_port(), tmp, count);
    free(tmp);
}
TRAP(CharWidth) { u8 c = (u8)ARG(0); RET(text_width(qd_port(), &c, 1)); }
TRAP(StringWidth) { u32 s = ARG(0); u8 n = rd8(s); RET(text_width(qd_port(), gptr(s + 1, n), n)); }
TRAP(TextWidth) {
    u32 buf = ARG(0); s16 first = ARGS16(1), count = ARGS16(2);
    if (count <= 0) { RET(0); return; }
    RET(text_width(qd_port(), gptr(buf + (u32)first, (u32)count), count));
}
TRAP(GetFontInfo) {
    u32 fi = ARG(0);
    int a, d, w, l;
    text_font_info(qd_port(), &a, &d, &w, &l);
    wr16(fi, (u16)a); wr16(fi + 2, (u16)d); wr16(fi + 4, (u16)w); wr16(fi + 6, (u16)l);
}

TRAP(GetFNum) {
    char name[256];
    pstr_to_c(ARG(0), name, sizeof name);
    u32 out = ARG(1);
    s16 id = 0;
    static const struct { const char *n; s16 id; } sys[] = {
        { "Chicago", 0 }, { "New York", 2 }, { "Geneva", 3 }, { "Monaco", 4 }, { "Venice", 5 }, { "London", 6 },
        { "Athens", 7 }, { "San Francisco", 8 }, { "Toronto", 9 }, { "Cairo", 11 }, { "Los Angeles", 12 },
        { "Times", 20 }, { "Helvetica", 21 }, { "Courier", 22 }, { "Symbol", 23 }, { "Mobile", 24 },
        { "Charcoal", 0 }, { NULL, 0 }
    };
    u32 h = res_get_named(FOURCC('F','O','N','D'), name);
    if (h) {
        s16 rid; u32 t;
        if (res_info(h, &t, &rid, NULL)) id = rid;
    } else {
        for (int i = 0; sys[i].n; i++) if (!strcmp(sys[i].n, name)) { id = sys[i].id; break; }
    }
    LOG_D("GetFNum(%s) -> %d", name, id);
    wr16(out, (u16)id);
}
TRAP(GetFontName) {
    s16 id = ARGS16(0); u32 out = ARG(1);
    wr8(out, 0);
    u32 len;
    (void)len;
    u32 h = res_get(FOURCC('F','O','N','D'), id);
    char name[256];
    if (h && res_info(h, NULL, NULL, name)) c_to_pstr(name, out, 255);
    else if (id == 0) c_to_pstr("Chicago", out, 255);
    else if (id == 3) c_to_pstr("Geneva", out, 255);
}
TRAP(RealFont) { RET(1); }
TRAP(SetFractEnable) { }
TRAP(SetFScaleDisable) { }

/* ---- Script Manager text utilities ---- */
TRAP(VisibleLength) {
    u32 p = ARG(0); s32 len = (s32)ARG(1);
    while (len > 0 && (rd8(p + (u32)len - 1) == ' ' || rd8(p + (u32)len - 1) == '\t' || rd8(p + (u32)len - 1) == '\r')) len--;
    RET(len);
}

/* StyledLineBreak(textPtr, textLen, textStart, textEnd, flags, &textWidth, &textOffset) */
TRAP(StyledLineBreak) {
    u32 tp = ARG(0); s32 start = (s32)ARG(2), end = (s32)ARG(3);
    u32 widthp = ARG(5), offp = ARG(6);
    s32 width = (s32)rd32(widthp); /* Fixed */
    bool first_run = rd32(offp) != 0;
    u32 port = qd_port();
    Styled s; styled(port, &s);
    s32 used = 0;
    s32 last_break = -1;
    for (s32 i = start; i < end; i++) {
        u8 c = rd8(tp + (u32)i);
        if (c == '\r') {
            wr32(offp, (u32)(i + 1));
            wr32(widthp, (u32)(width - used));
            RET(0);
            return;
        }
        s32 w = (s32)char_adv(port, &s, c) << 16;
        if (c == ' ' || c == '\t') {
            used += w;
            last_break = i + 1;
            continue;
        }
        if (used + w > width) {
            if (last_break > start || (last_break == start && !first_run)) {
                wr32(offp, (u32)last_break);
                RET(0); /* smBreakWord */
                return;
            }
            if (!first_run && last_break < 0) { wr32(offp, (u32)start); RET(0); return; }
            /* word longer than the line: break inside it (at least one char) */
            s32 at = i > start ? i : start + 1;
            wr32(offp, (u32)at);
            RET(1); /* smBreakChar */
            return;
        }
        used += w;
    }
    wr32(widthp, (u32)(width - used));
    wr32(offp, (u32)end);
    RET(2); /* smBreakOverflow */
}

TRAP(TruncString) {
    s16 width = ARGS16(0); u32 str = ARG(1); s16 where = ARGS16(2);
    u32 port = qd_port();
    u8 n = rd8(str);
    u8 buf[256];
    gmemcpy_from(buf, str + 1, n);
    if (text_width(port, buf, n) <= width) { RET(0); return; }
    const u8 ell = 0xC9;
    int ew = text_width(port, &ell, 1);
    if (where == 0) { /* truncEnd */
        int k = n;
        while (k > 0 && text_width(port, buf, k) + ew > width) k--;
        buf[k] = ell;
        wr8(str, (u8)(k + 1));
        gmemcpy_to(str + 1, buf, (u32)k + 1);
    } else { /* truncMiddle */
        int keep = n;
        u8 out[256];
        int len = 0;
        while (keep > 0) {
            int a = keep / 2, b = keep - a;
            len = 0;
            memcpy(out, buf, (size_t)a); len = a;
            out[len++] = ell;
            memcpy(out + len, buf + n - b, (size_t)b); len += b;
            if (text_width(port, out, len) <= width) break;
            keep--;
        }
        wr8(str, (u8)len);
        gmemcpy_to(str + 1, out, (u32)len);
    }
    RET(1);
}

void text_init(void) {
    wr16(LM_ApFontID, 3);
}
