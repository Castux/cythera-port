/* QuickDraw internal definitions.  All structures live in guest memory with
 * their classic Mac layouts; these offsets describe them. */
#ifndef QD_H
#define QD_H
#include "os.h"
#include "mm.h"

/* ---- GrafPort / CGrafPort (108 bytes) ---- */
#define PORT_SIZE        108
#define PORT_DEVICE      0
#define PORT_BITS        2    /* BitMap (old) or portPixMap handle (color) */
#define PORT_VERSION     6    /* color ports: 0xC000 flag */
#define PORT_GRAFVARS    8
#define PORT_CHEXTRA     12
#define PORT_PNLOCHFRAC  14
#define PORT_RECT        16
#define PORT_VIS         24
#define PORT_CLIP        28
#define PORT_BKPAT       32   /* old: Pattern; color: bkPixPat handle */
#define PORT_FILLPAT_OLD 40
#define PORT_RGBFG       36
#define PORT_RGBBK       42
#define PORT_PNLOC       48
#define PORT_PNSIZE      52
#define PORT_PNMODE      56
#define PORT_PNPAT       58   /* old: Pattern; color: pnPixPat handle */
#define PORT_FILLPIXPAT  62
#define PORT_PNVIS       66
#define PORT_TXFONT      68
#define PORT_TXFACE      70
#define PORT_TXMODE      72
#define PORT_TXSIZE      74
#define PORT_SPEXTRA     76
#define PORT_FGCOLOR     80
#define PORT_BKCOLOR     84
#define PORT_COLRBIT     88
#define PORT_PATSTRETCH  90
#define PORT_PICSAVE     92
#define PORT_RGNSAVE     96
#define PORT_POLYSAVE    100
#define PORT_GRAFPROCS   104

/* ---- PixMap (50 bytes) ---- */
#define PM_SIZE      50
#define PM_BASE      0
#define PM_ROWBYTES  4
#define PM_BOUNDS    6
#define PM_VERSION   14
#define PM_PACKTYPE  16
#define PM_PACKSIZE  18
#define PM_HRES      22
#define PM_VRES      26
#define PM_PIXTYPE   30
#define PM_PIXSIZE   32
#define PM_CMPCOUNT  34
#define PM_CMPSIZE   36
#define PM_PLANEBYTES 38
#define PM_TABLE     42
#define PM_RESERVED  46

/* ---- GDevice (62 bytes) ---- */
#define GD_SIZE    62
#define GD_REFNUM  0
#define GD_ID      2
#define GD_TYPE    4
#define GD_ITABLE  6
#define GD_RESPREF 10
#define GD_SEARCH  12
#define GD_COMP    16
#define GD_FLAGS   20
#define GD_PMAP    22
#define GD_REFCON  26
#define GD_NEXT    30
#define GD_RECT    34
#define GD_MODE    42

/* ---- PixPat (28 bytes) ---- */
#define PP_SIZE    28
#define PP_TYPE    0
#define PP_MAP     2
#define PP_DATA    6
#define PP_XDATA   10
#define PP_XVALID  14
#define PP_XMAP    16
#define PP_PAT1    20

/* Transfer modes */
enum {
    srcCopy = 0, srcOr, srcXor, srcBic, notSrcCopy, notSrcOr, notSrcXor, notSrcBic,
    patCopy = 8, patOr, patXor, patBic, notPatCopy, notPatOr, notPatXor, notPatBic,
    blend = 32, addPin, addOver, subPin, transparent, addMax, subOver, adMin,
    hilite = 50, ditherCopy = 64
};

typedef struct { u16 r, g, b; } RGB;

/* ---- Regions (host representation) ---- */
typedef struct {
    int y0, y1;
    int n;          /* number of x endpoints (even) */
    s16 *x;
} Band;
typedef struct {
    int nb;
    Band *b;
} HRgn;

void hrgn_free(HRgn *r);
void hrgn_rect(HRgn *r, int top, int left, int bottom, int right);
void hrgn_from_guest(HRgn *r, u32 rgnh);
void hrgn_to_guest(const HRgn *r, u32 rgnh);
void hrgn_op(HRgn *out, const HRgn *a, const HRgn *b, int op); /* 0 union 1 sect 2 diff 3 xor */
void hrgn_offset(HRgn *r, int dx, int dy);
bool hrgn_empty(const HRgn *r);
void hrgn_bbox(const HRgn *r, Rect *bb);
bool hrgn_contains(const HRgn *r, int x, int y);
void hrgn_copy(HRgn *dst, const HRgn *src);
void hrgn_from_mask(HRgn *r, const u8 *mask, int w, int h, int ox, int oy); /* mask: 1 byte/pixel */

u32 rgn_new(void);                  /* empty region handle */
void rgn_set_rect(u32 rgnh, Rect r);
Rect rgn_bbox(u32 rgnh);
void rgn_dispose(u32 rgnh);
u32 rgn_recording(void);            /* current OpenRgn target or 0 */
void rgn_record_frame_rect(Rect r); /* add outline of rect to open region */

/* ---- ports & drawing surfaces ---- */
typedef struct {
    u32 base;
    int rowbytes;
    Rect bounds;
    int depth;
    u32 ctab;       /* CTabHandle for indexed pixmaps, 0 for bitmaps/direct */
} Surf;

extern u32 g_qd_theport_ptr; /* address of qd.thePort in app globals */
u32 qd_port(void);
void qd_set_port(u32 port);
bool is_color_port(u32 port);
u32 port_pixmap(u32 port);          /* PixMapHandle (color ports) */
bool surf_from_port(u32 port, Surf *s);
bool surf_from_bitmap(u32 bm, Surf *s); /* BitMap*, PixMap* or port bits */

/* Clip region (local coords) of port = visRgn ∩ clipRgn ∩ bounds */
void port_clip_hrgn(u32 port, HRgn *out);

/* Colors */
RGB ctab_color(u32 ctab, int index);
int ctab_count(u32 ctab);
int ctab_nearest(u32 ctab, RGB c);
u32 ctab_seed(u32 ctab);
void ctab_invalidate(u32 ctab);
u32 pixel_for_rgb(const Surf *s, RGB c);
RGB rgb_for_pixel(const Surf *s, u32 px);
RGB port_fg(u32 port);
RGB port_bk(u32 port);
void rd_rgb(u32 a, RGB *c);
void wr_rgb(u32 a, RGB c);

/* Pixel access */
u32 surf_get(const Surf *s, int x, int y);   /* x,y relative to bounds top-left */
void surf_put(const Surf *s, int x, int y, u32 v);

/* Drawing core (local coordinates of current port) */
typedef struct {
    int kind;          /* 0 = solid color, 1 = old 8x8 pattern, 2 = pixpat */
    u8 pat[8];
    RGB fg, bk;
    u32 pixpat;        /* PixPatHandle when kind==2 */
} Paint;
void paint_from_pattern(Paint *p, u32 port, const u8 pat[8]);
void paint_pen(Paint *p, u32 port);
void paint_back(Paint *p, u32 port);
void paint_fill(Paint *p, u32 port, u32 patptr);
void draw_hrgn(u32 port, const HRgn *shape, const Paint *paint, int mode);
void draw_rect(u32 port, Rect r, const Paint *paint, int mode);
void qd_invert_hrgn(u32 port, const HRgn *shape);

/* CopyBits core */
void copybits(u32 srcbm, u32 dstbm, Rect sr, Rect dr, int mode, u32 maskrgn, u32 maskbm, Rect mr);

/* Screen */
void qd_init_screen(int w, int h);
u32 qd_main_device(void);
u32 qd_screen_pixmap(void);  /* PixMapHandle */
u32 qd_screen_base(void);
int qd_screen_w(void);
int qd_screen_h(void);
void qd_present(void);
void qd_screen_dirty(void);
u32 qd_cur_device(void);
void qd_set_device_clut(u32 ctab_src, int start, int count, bool use_value);

/* Text */
void text_init(void);
int text_width(u32 port, const u8 *s, int n);
void text_draw(u32 port, const u8 *s, int n);
void text_font_info(u32 port, int *ascent, int *descent, int *widmax, int *leading);

/* Pictures */
void pict_draw(u32 pich, Rect dst);
/* Picture recording (OpenPicture/ClosePicture) */
bool pict_recording(u32 port);
void pict_rec_text(u32 port, int h, int v, const u8 *s, int n);
void pict_rec_rect(u32 port, int verb, Rect r);   /* 0 frame, 1 paint, 2 erase, 3 invert */
void pict_rec_line(u32 port, Point from, Point to);
void pict_rec_unsupported(u32 port, const char *what);
bool pict_decode_to_rgb(const u8 *pic, u32 len, int *w, int *h, u32 **rgb);

/* Ports */
u32 port_grafvars(u32 port);
void port_mirror_bounds(u32 port);   /* window ports: bounds.topLeft at +8/+10 */
void port_make_window_port(u32 port);
void port_init(u32 port, bool color);
void port_init_color_state(u32 port);
u32 new_pixpat_from_pattern(const u8 pat[8]);
void rect_from_pts(Point a, Point b, Rect *r);

static inline Rect mkrect(int t, int l, int b, int r) { Rect x = { (s16)t, (s16)l, (s16)b, (s16)r }; return x; }
static inline bool rect_empty(Rect r) { return r.bottom <= r.top || r.right <= r.left; }
static inline Rect rect_sect(Rect a, Rect b) {
    Rect r = { a.top > b.top ? a.top : b.top, a.left > b.left ? a.left : b.left,
               a.bottom < b.bottom ? a.bottom : b.bottom, a.right < b.right ? a.right : b.right };
    if (rect_empty(r)) r = (Rect){ 0, 0, 0, 0 };
    return r;
}
#endif
