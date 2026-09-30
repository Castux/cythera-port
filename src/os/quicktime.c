/* QuickTime (subset): Component Manager instances for the tune player and
 * note allocator, file previews.  Music synthesis lives in music.c (M6);
 * until then tunes are accepted and play silently. */
#include "os.h"
#include "mm.h"
#include "qd.h"

#define COMP_TUNE FOURCC('t','u','n','e')
#define COMP_NOTA FOURCC('n','o','t','a')

typedef struct { u32 inst; u32 type; } Comp;
static Comp g_comps[32];

TRAP(EnterMovies) { RETERR(noErr); }
TRAP(ExitMovies) { }

TRAP(OpenDefaultComponent) {
    u32 type = ARG(0);
    if (type != COMP_TUNE && type != COMP_NOTA) { LOG_D("OpenDefaultComponent('%s') -> none", fourcc_str(type)); RET(0); return; }
    for (int i = 0; i < 32; i++) if (!g_comps[i].inst) {
        g_comps[i].inst = mm_new_ptr(16, true, ZONE_SYS);
        g_comps[i].type = type;
        wr32(g_comps[i].inst, type);
        RET(g_comps[i].inst);
        return;
    }
    RET(0);
}
TRAP(CloseComponent) {
    u32 c = ARG(0);
    extern void music_close_player(u32 inst);
    for (int i = 0; i < 32; i++) if (g_comps[i].inst == c) { music_close_player(c); mm_dispose_ptr(c); g_comps[i].inst = 0; }
    RETERR(noErr);
}

/* ---- File previews ----
   The game saves a thumbnail of the map view in each saved game
   (MakeThumbnailFromPixMap + AddFilePreview), which the Open dialog
   (StandardGetFilePreview) shows. */

/* MakeFilePreview makes a preview from a movie or picture file's own
   contents; a saved game is neither, so there is nothing to do (the game
   adds its own thumbnail next). */
TRAP(MakeFilePreview) { RETERR(noErr); }

typedef struct { u8 *b; size_t n, cap; } Buf;
static void put(Buf *o, const void *d, size_t n) {
    if (o->n + n > o->cap) { o->cap = (o->n + n) * 2 + 256; o->b = realloc(o->b, o->cap); }
    memcpy(o->b + o->n, d, n); o->n += n;
}
static void put8(Buf *o, u8 v) { put(o, &v, 1); }
static void put16(Buf *o, u32 v) { u8 b[2] = { (u8)(v >> 8), (u8)v }; put(o, b, 2); }
static void put32(Buf *o, u32 v) { put16(o, v >> 16); put16(o, v & 0xFFFF); }
static void put_rect(Buf *o, Rect r) { put16(o, (u16)r.top); put16(o, (u16)r.left); put16(o, (u16)r.bottom); put16(o, (u16)r.right); }
/* one PackBits-compressed row */
static void packbits(Buf *o, const u8 *row, int n) {
    int i = 0;
    while (i < n) {
        int run = 1;
        while (i + run < n && run < 128 && row[i + run] == row[i]) run++;
        if (run >= 3) { put8(o, (u8)(257 - run)); put8(o, row[i]); i += run; continue; }
        int lit = 0;
        while (i + lit < n && lit < 128) {
            if (i + lit + 2 < n && row[i + lit] == row[i + lit + 1] && row[i + lit] == row[i + lit + 2]) break;
            lit++;
        }
        put8(o, (u8)(lit - 1)); put(o, row + i, (size_t)lit); i += lit;
    }
}

/* MakeThumbnailFromPixMap(src, srcRect, colorDepth, picture, progress):
   an 8-bit picture of the area, scaled down (box filter) so that its longer
   side is at most 80 pixels, like QuickTime's thumbnails. */
TRAP(MakeThumbnailFromPixMap) {
    u32 pmh = ARG(0), srp = ARG(1), pic = ARG(3);
    Surf s;
    if (!pmh || !hderef(pmh) || !pic || !surf_from_bitmap(hderef(pmh), &s)) { RETERR(paramErr); return; }
    Rect sr = srp ? rd_rect(srp) : s.bounds;
    Rect sect = { sr.top > s.bounds.top ? sr.top : s.bounds.top, sr.left > s.bounds.left ? sr.left : s.bounds.left,
                  sr.bottom < s.bounds.bottom ? sr.bottom : s.bounds.bottom, sr.right < s.bounds.right ? sr.right : s.bounds.right };
    int sw = sect.right - sect.left, sh = sect.bottom - sect.top;
    if (sw <= 0 || sh <= 0) { RETERR(paramErr); return; }
    int big = sw > sh ? sw : sh;
    int tw = sw, th = sh;
    if (big > 80) { tw = (sw * 80 + big / 2) / big; th = (sh * 80 + big / 2) / big; }
    if (tw < 1) tw = 1;
    if (th < 1) th = 1;
    /* colors: the source's own table if it is indexed, else the screen's */
    u32 ct = (s.depth <= 8 && s.ctab) ? s.ctab : rd32(hderef(qd_screen_pixmap()) + PM_TABLE);
    int nc = ctab_count(ct);
    if (nc > 256) nc = 256;
    int rb = (tw + 1) & ~1;
    u8 *px = calloc((size_t)rb * (size_t)th, 1);
    for (int y = 0; y < th; y++)
        for (int x = 0; x < tw; x++) {
            int x0 = sect.left - s.bounds.left + x * sw / tw, x1 = sect.left - s.bounds.left + (x + 1) * sw / tw;
            int y0 = sect.top - s.bounds.top + y * sh / th, y1 = sect.top - s.bounds.top + (y + 1) * sh / th;
            if (x1 <= x0) x1 = x0 + 1;
            if (y1 <= y0) y1 = y0 + 1;
            u64 r = 0, g = 0, b = 0, k = 0;
            for (int yy = y0; yy < y1; yy++)
                for (int xx = x0; xx < x1; xx++) {
                    RGB c = rgb_for_pixel(&s, surf_get(&s, xx, yy));
                    r += c.r; g += c.g; b += c.b; k++;
                }
            RGB avg = { (u16)(r / k), (u16)(g / k), (u16)(b / k) };
            px[(size_t)y * (size_t)rb + (size_t)x] = (u8)ctab_nearest(ct, avg);
        }
    Rect fr = mkrect(0, 0, (s16)th, (s16)tw);
    Buf o = {0};
    put16(&o, 0); put_rect(&o, fr);                       /* picSize, picFrame */
    put16(&o, 0x0011); put16(&o, 0x02FF);                 /* version 2 */
    put16(&o, 0x0C00); put16(&o, 0xFFFE); put16(&o, 0);   /* extended header */
    put32(&o, 0x00480000); put32(&o, 0x00480000); put_rect(&o, fr); put32(&o, 0);
    put16(&o, 0x001E);                                    /* DefHilite */
    put16(&o, 0x0001); put16(&o, 10); put_rect(&o, fr);   /* clip */
    put16(&o, 0x0098);                                    /* PackBitsRect */
    put16(&o, 0x8000u | (u32)rb); put_rect(&o, fr);
    put16(&o, 0); put16(&o, 0); put32(&o, 0);             /* pmVersion, packType, packSize */
    put32(&o, 0x00480000); put32(&o, 0x00480000);
    put16(&o, 0); put16(&o, 8); put16(&o, 1); put16(&o, 8); /* pixelType, pixelSize, cmpCount, cmpSize */
    put32(&o, 0); put32(&o, 0); put32(&o, 0);             /* planeBytes, pmTable, pmReserved */
    put32(&o, 0); put16(&o, 0); put16(&o, (u32)(nc - 1));  /* color table: seed, flags, size */
    for (int i = 0; i < nc; i++) {
        RGB c = ctab_color(ct, i);
        put16(&o, (u32)i); put16(&o, c.r); put16(&o, c.g); put16(&o, c.b);
    }
    put_rect(&o, fr); put_rect(&o, fr); put16(&o, 0);     /* srcRect, dstRect, srcCopy */
    Buf row = {0};
    for (int y = 0; y < th; y++) {
        row.n = 0;
        if (rb < 8) put(&row, px + (size_t)y * (size_t)rb, (size_t)rb);
        else packbits(&row, px + (size_t)y * (size_t)rb, rb);
        if (rb >= 8) { if (rb > 250) put16(&o, (u32)row.n); else put8(&o, (u8)row.n); }
        put(&o, row.b, row.n);
    }
    if (o.n & 1) put8(&o, 0);
    put16(&o, 0x00FF);
    o.b[0] = (u8)(o.n >> 8); o.b[1] = (u8)o.n;            /* picSize: low 16 bits */
    if (mm_set_handle_size(pic, (u32)o.n)) gmemcpy_to(hderef(pic), o.b, (u32)o.n);
    free(row.b); free(o.b); free(px);
    RETERR(mm_handle_size(pic) == o.n ? noErr : memFullErr);
}

/* AddFilePreview(resRefNum, previewType, previewData): store a copy of the
   preview in the file as a resource of that type, and a 'pnot' resource
   {modDate, version, resType, resID} pointing at it, replacing any
   previous preview. */
static u32 call1(void (*fn)(CPU *), u32 a0, u32 a1, u32 a2, u32 a3) {
    CPU f; memset(&f, 0, sizeof f);
    f.r[3] = a0; f.r[4] = a1; f.r[5] = a2; f.r[6] = a3;
    fn(&f);
    return f.r[3];
}
extern void trap_CurResFile(CPU *), trap_UseResFile(CPU *), trap_Get1Resource(CPU *), trap_RemoveResource(CPU *),
            trap_AddResource(CPU *), trap_UniqueID(CPU *), trap_ResError(CPU *);
#define PNOT FOURCC('p','n','o','t')
TRAP(AddFilePreview) {
    s16 ref = ARGS16(0); u32 type = ARG(1), data = ARG(2);
    if (!data || !hderef(data)) { RETERR(paramErr); return; }
    s16 save = (s16)call1(trap_CurResFile, 0, 0, 0, 0);
    call1(trap_UseResFile, (u32)(u16)ref, 0, 0, 0);
    if ((s16)call1(trap_ResError, 0, 0, 0, 0)) { call1(trap_UseResFile, (u32)(u16)save, 0, 0, 0); RETERR(resFNotFound); return; }
    u32 old = call1(trap_Get1Resource, PNOT, 0, 0, 0);
    if (old && mm_handle_size(old) >= 12) {
        u32 otype = rd32(hderef(old) + 6); s16 oid = rds16(hderef(old) + 10);
        u32 op = call1(trap_Get1Resource, otype, (u32)(u16)oid, 0, 0);
        if (op) { call1(trap_RemoveResource, op, 0, 0, 0); mm_dispose_handle(op); }
    }
    if (old) { call1(trap_RemoveResource, old, 0, 0, 0); mm_dispose_handle(old); }
    u32 n = mm_handle_size(data);
    u32 copy = mm_new_handle(n, false, ZONE_APP);
    u32 pn = mm_new_handle(12, true, ZONE_APP);
    if (!copy || !pn) { call1(trap_UseResFile, (u32)(u16)save, 0, 0, 0); RETERR(memFullErr); return; }
    gmemmove(hderef(copy), hderef(data), n);
    s16 id = (s16)call1(trap_UniqueID, type, 0, 0, 0);
    call1(trap_AddResource, copy, type, (u32)(u16)id, 0);
    extern u32 mac_time_now(void);
    wr32(hderef(pn), mac_time_now()); wr16(hderef(pn) + 4, 0);
    wr32(hderef(pn) + 6, type); wr16(hderef(pn) + 10, (u16)id);
    call1(trap_AddResource, pn, PNOT, 0, 0);
    s16 err = (s16)call1(trap_ResError, 0, 0, 0, 0);
    call1(trap_UseResFile, (u32)(u16)save, 0, 0, 0);
    RETERR(err);
}
