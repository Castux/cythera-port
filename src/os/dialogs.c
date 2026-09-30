/* Dialog Manager.
 *
 * DialogRecords (WindowRecord + items/textH/editField/editOpen/aDefItem)
 * live in guest memory. The item list is a copy of the DITL whose
 * placeholders hold control handles, text handles, resource handles or
 * user-item procs, exactly as the real Dialog Manager does. */
#include "wm.h"
#include "resources.h"
#include "misc.h"
#include "../host/host.h"

#define LM_DAStrings 0x0AA0
#define DLG_SIZE     170
#define DLG_ITEMS    156
#define DLG_TEXTH    160
#define DLG_EDITFIELD 164
#define DLG_EDITOPEN 166
#define DLG_DEFITEM  168

enum { userItem = 0, btnCtrl = 4, chkCtrl = 5, radCtrl = 6, resCtrl = 7, statText = 8, editText = 16,
       iconItem = 32, picItem = 64, itemDisable = 128 };

u32 ctl_new(u32 win, Rect r, const u8 *title, bool vis, s16 val, s16 mn, s16 mx, s16 procid, u32 refcon);
void ctl_draw_one(u32 c);
void te_api_key(u32 te, u8 ch);
void te_api_click(u32 te, Point pt, bool ext);
void te_api_idle(u32 te);
void te_api_activate(u32 te, bool on);
void te_api_update(u32 te);
u32 te_api_new(Rect r);
void te_api_settext(u32 te, const u8 *s, int n);
void te_api_rects(u32 te, Rect r);
void te_api_cut(u32 te);
void te_api_copy(u32 te);
void te_api_paste(u32 te);
void te_api_delete(u32 te);
void te_api_setselect(u32 te, int a, int b);

static u32 g_param[4];

TRAP(ParamText) {
    for (int i = 0; i < 4; i++) {
        u32 s = ARG(i);
        if (!g_param[i]) g_param[i] = mm_new_handle(256, true, ZONE_SYS);
        if (s) gmemmove(hderef(g_param[i]), s, 1u + rd8(s)); else wr8(hderef(g_param[i]), 0);
        wr32(LM_DAStrings + 4 * (u32)i, g_param[i]);
    }
}

typedef struct { u32 dlg; bool is_alert; } DInfo;
#define MAX_DLG 64
static DInfo g_di[MAX_DLG];
static DInfo *dinfo(u32 d) { for (int i = 0; i < MAX_DLG; i++) if (g_di[i].dlg == d) return &g_di[i]; return NULL; }
static bool is_dialog(u32 w) { return w && dinfo(w) != NULL; }

/* ---- item list access ---- */
typedef struct { u32 at; u32 handle; Rect r; u8 type; u8 len; u32 data; } Item;

static int item_count(u32 dlg) {
    u32 h = rd32(dlg + DLG_ITEMS);
    return h && hderef(h) ? (s16)rd16(hderef(h)) + 1 : 0;
}
static bool get_item(u32 dlg, int n, Item *it) {
    u32 h = rd32(dlg + DLG_ITEMS);
    if (!h || n < 1 || n > item_count(dlg)) return false;
    u32 p = hderef(h) + 2;
    for (int i = 1; i <= n; i++) {
        it->at = p;
        it->handle = rd32(p);
        it->r = rd_rect(p + 4);
        it->type = rd8(p + 12);
        it->len = rd8(p + 13);
        it->data = p + 14;
        p += 14 + it->len + (it->len & 1);
    }
    return true;
}

static void subst_param(const u8 *in, int n, u8 *out, int *outn) {
    int o = 0;
    for (int i = 0; i < n && o < 1000; i++) {
        if (in[i] == '^' && i + 1 < n && in[i + 1] >= '0' && in[i + 1] <= '3') {
            u32 h = g_param[in[i + 1] - '0'];
            if (h && hderef(h)) {
                u8 l = rd8(hderef(h));
                gmemcpy_from(out + o, hderef(h) + 1, l);
                o += l;
            }
            i++;
        } else out[o++] = in[i];
    }
    *outn = o;
}

static void draw_frame_rect(u32 port, Rect r) {
    Paint k = { .kind = 0, .fg = { 0, 0, 0 } };
    draw_rect(port, mkrect(r.top, r.left, r.top + 1, r.right), &k, patCopy);
    draw_rect(port, mkrect(r.bottom - 1, r.left, r.bottom, r.right), &k, patCopy);
    draw_rect(port, mkrect(r.top, r.left, r.bottom, r.left + 1), &k, patCopy);
    draw_rect(port, mkrect(r.top, r.right - 1, r.bottom, r.right), &k, patCopy);
}

static void text_box(u32 port, const u8 *s, int n, Rect box) {
    u32 buf = mm_new_ptr((u32)n + 1, false, ZONE_SYS);
    if (n) gmemcpy_to(buf, s, (u32)n);
    u32 rp = mm_new_ptr(8, false, ZONE_SYS);
    wr_rect(rp, box);
    extern void trap_TETextBox(CPU *);
    CPU f; memset(&f, 0, sizeof f);
    f.r[3] = buf; f.r[4] = (u32)n; f.r[5] = rp; f.r[6] = 0;
    trap_TETextBox(&f);
    mm_dispose_ptr(rp);
    mm_dispose_ptr(buf);
}

static void draw_item(u32 dlg, int n) {
    Item it;
    if (!get_item(dlg, n, &it)) return;
    u8 t = it.type & 0x7F;
    u32 port = dlg;
    switch (t) {
    case btnCtrl: case chkCtrl: case radCtrl: case resCtrl:
        if (it.handle) ctl_draw_one(it.handle);
        break;
    case statText: {
        u32 h = it.handle;
        if (!h || !hderef(h)) break;
        u32 len = mm_handle_size(h);
        u8 *raw = malloc(len + 1); gmemcpy_from(raw, hderef(h), len);
        u8 out[1100]; int on;
        subst_param(raw, (int)len, out, &on);
        free(raw);
        text_box(port, out, on, it.r);
        break;
    }
    case editText: {
        Rect fr = mkrect(it.r.top - 3, it.r.left - 3, it.r.bottom + 3, it.r.right + 3);
        draw_frame_rect(port, fr);
        u32 te = rd32(dlg + DLG_TEXTH);
        if (te && rds16(dlg + DLG_EDITFIELD) == n - 1) te_api_update(te);
        else if (it.handle && hderef(it.handle)) {
            u32 len = mm_handle_size(it.handle);
            u8 *raw = malloc(len + 1); gmemcpy_from(raw, hderef(it.handle), len);
            Paint bk; paint_back(&bk, port);
            draw_rect(port, it.r, &bk, patCopy);
            text_box(port, raw, (int)len, it.r);
            free(raw);
        }
        break;
    }
    case iconItem:
        if (it.handle && hderef(it.handle)) {
            u32 rp = mm_new_ptr(8, false, ZONE_SYS);
            wr_rect(rp, it.r);
            extern void trap_PlotCIcon(CPU *);
            CPU f; memset(&f, 0, sizeof f); f.r[3] = rp; f.r[4] = it.handle;
            if (mm_handle_size(it.handle) > 128) trap_PlotCIcon(&f);
            mm_dispose_ptr(rp);
        }
        break;
    case picItem:
        if (it.handle) pict_draw(it.handle, it.r);
        break;
    case userItem:
        if (it.handle) {
            u32 a[2] = { dlg, (u32)n };
            call_upp(it.handle, 2, a);
        }
        break;
    }
}

static void draw_dialog(u32 dlg) {
    u32 save = qd_port();
    qd_set_port(dlg);
    int n = item_count(dlg);
    for (int i = 1; i <= n; i++) draw_item(dlg, i);
    qd_set_port(save);
}

static void apply_dctb(u32 dlg, s16 id) {
    u32 len;
    u8 *d = res_load_raw(FOURCC('d','c','t','b'), id, &len);
    if (!d) return;
    int n = (s16)be16(d + 6) + 1;
    for (int i = 0; i < n && 8 + 8 * (u32)(i + 1) <= len; i++) {
        const u8 *e = d + 8 + 8 * i;
        u16 part = be16(e);
        RGB c = { be16(e + 2), be16(e + 4), be16(e + 6) };
        if (part == 0) wr_rgb(dlg + PORT_RGBBK, c);      /* wContentColor */
        else if (part == 2) wr_rgb(dlg + PORT_RGBFG, c); /* wTextColor */
    }
    free(d);
}

/* Build items for a dialog from a DITL resource handle's data. */
static void build_items(u32 dlg, const u8 *ditl, u32 len) {
    u32 h = mm_handle_from_data(ditl, len, ZONE_APP);
    wr32(dlg + DLG_ITEMS, h);
    int n = item_count(dlg);
    u32 save = qd_port();
    qd_set_port(dlg);
    for (int i = 1; i <= n; i++) {
        Item it; get_item(dlg, i, &it);
        u8 t = it.type & 0x7F;
        u8 title[256]; title[0] = it.len;
        gmemcpy_from(title + 1, it.data, it.len);
        u32 hnd = 0;
        switch (t) {
        case btnCtrl: hnd = ctl_new(dlg, it.r, title, true, 0, 0, 1, 0, 0); break;
        case chkCtrl: hnd = ctl_new(dlg, it.r, title, true, 0, 0, 1, 1, 0); break;
        case radCtrl: hnd = ctl_new(dlg, it.r, title, true, 0, 0, 1, 2, 0); break;
        case resCtrl: {
            s16 cid = (s16)rd16(it.data);
            u32 cl;
            u8 *c = res_load_raw(FOURCC('C','N','T','L'), cid, &cl);
            if (c) {
                u8 ct[256]; ct[0] = c[22]; memcpy(ct + 1, c + 23, ct[0]);
                hnd = ctl_new(dlg, it.r, ct, be16(c + 10) != 0, (s16)be16(c + 8), (s16)be16(c + 14), (s16)be16(c + 12),
                              (s16)be16(c + 16), be32(c + 18));
                free(c);
            }
            break;
        }
        case statText: case editText:
            hnd = mm_new_handle(it.len, false, ZONE_APP);
            if (it.len) gmemcpy_to(hderef(hnd), title + 1, it.len);
            break;
        case iconItem: {
            s16 iid = (s16)rd16(it.data);
            extern void trap_GetCIcon(CPU *);
            CPU f; memset(&f, 0, sizeof f); f.r[3] = (u32)iid;
            trap_GetCIcon(&f);
            hnd = f.r[3];
            if (!hnd) hnd = res_get(FOURCC('I','C','O','N'), iid);
            break;
        }
        case picItem: hnd = res_get(FOURCC('P','I','C','T'), (s16)rd16(it.data)); break;
        default: hnd = 0; break;
        }
        /* items handle may have moved */
        get_item(dlg, i, &it);
        wr32(it.at, hnd);
    }
    /* first edit text item gets the TE */
    wr16(dlg + DLG_EDITFIELD, (u16)-1);
    for (int i = 1; i <= n; i++) {
        Item it; get_item(dlg, i, &it);
        if ((it.type & 0x7F) != editText) continue;
        u32 te = rd32(dlg + DLG_TEXTH);
        if (!te) { te = te_api_new(it.r); wr32(dlg + DLG_TEXTH, te); }
        te_api_rects(te, it.r);
        u32 len2 = mm_handle_size(it.handle);
        u8 *s = malloc(len2 + 1); gmemcpy_from(s, hderef(it.handle), len2);
        te_api_settext(te, s, (int)len2);
        free(s);
        wr16(dlg + DLG_EDITFIELD, (u16)(i - 1));
        break;
    }
    qd_set_port(save);
}

static u32 new_dialog(u32 storage, Rect r, const char *title, bool vis, s16 proc, u32 behind, bool goaway,
                      u32 refcon, const u8 *ditl, u32 ditllen, s16 dctb_id) {
    u32 dlg = storage ? storage : mm_new_ptr(DLG_SIZE, true, ZONE_APP);
    gmemset(dlg, 0, DLG_SIZE);
    wm_create(dlg, r, title, false, proc, behind, goaway, refcon, true);
    wr16(dlg + WIN_KIND, dialogKind);
    wr16(dlg + DLG_DEFITEM, 0);
    DInfo *di = NULL;
    for (int i = 0; i < MAX_DLG; i++) if (!g_di[i].dlg) { di = &g_di[i]; break; }
    if (!di) fatal("too many dialogs");
    di->dlg = dlg; di->is_alert = false;
    u32 save = qd_port();
    qd_set_port(dlg);
    wr16(dlg + PORT_TXFONT, 0); wr16(dlg + PORT_TXSIZE, 12);
    if (dctb_id) apply_dctb(dlg, dctb_id);
    build_items(dlg, ditl, ditllen);
    qd_set_port(save);
    if (vis) wm_show(dlg, true);
    return dlg;
}

TRAP(InitDialogs) { }

TRAP(GetNewDialog) {
    s16 id = ARGS16(0); u32 storage = ARG(1), behind = ARG(2);
    u32 len;
    u8 *d = res_load_raw(FOURCC('D','L','O','G'), id, &len);
    if (!d) { LOG_W("DLOG %d not found", id); RET(0); return; }
    Rect r = { (s16)be16(d), (s16)be16(d + 2), (s16)be16(d + 4), (s16)be16(d + 6) };
    s16 proc = (s16)be16(d + 8);
    bool vis = d[10] != 0, goaway = d[12] != 0;
    u32 refcon = be32(d + 14);
    s16 items = (s16)be16(d + 18);
    char title[256]; int tl = d[20]; memcpy(title, d + 21, (size_t)tl); title[tl] = 0;
    u32 posoff = (u32)(21 + tl + 1) & ~1u;
    if (posoff + 2 <= len) {
        u16 pos = be16(d + posoff) & 0x3FFF;
        int w = r.right - r.left, h = r.bottom - r.top;
        int mb = rds16(LM_MBarHeight);
        if (pos == 0x280A || pos == 0x300A || pos == 0x700A || pos == 0xB00A) {
            int x = (qd_screen_w() - w) / 2;
            int y = pos == 0x280A ? mb + (qd_screen_h() - mb - h) / 2 : mb + (qd_screen_h() - mb - h) / 3;
            r = mkrect(y, x, y + h, x + w);
        }
    }
    free(d);
    u32 dl;
    u8 *ditl = res_load_raw(FOURCC('D','I','T','L'), items, &dl);
    if (!ditl) { LOG_W("DITL %d not found", items); RET(0); return; }
    u32 dlg = new_dialog(storage, r, title, vis, proc, behind, goaway, refcon, ditl, dl, id);
    free(ditl);
    LOG_D("GetNewDialog(%d) -> %08x", id, dlg);
    RET(dlg);
}

TRAP(NewDialog) {
    u32 storage = ARG(0); Rect r = rd_rect(ARG(1)); u32 tp = ARG(2); bool vis = ARGB(3); s16 proc = ARGS16(4);
    u32 behind = ARG(5); bool goaway = ARGB(6); u32 refcon = ARG(7); u32 items = ARG(8);
    char title[256]; pstr_to_c(tp, title, sizeof title);
    u32 len = mm_handle_size(items);
    u8 *d = malloc(len); gmemcpy_from(d, hderef(items), len);
    RET(new_dialog(storage, r, title, vis, proc, behind, goaway, refcon, d, len, 0));
    free(d);
}

static void dispose_dialog(u32 dlg, bool free_storage) {
    DInfo *di = dinfo(dlg);
    if (!di) return;
    int n = item_count(dlg);
    for (int i = 1; i <= n; i++) {
        Item it; get_item(dlg, i, &it);
        u8 t = it.type & 0x7F;
        if ((t == statText || t == editText) && it.handle) mm_dispose_handle(it.handle);
    }
    u32 te = rd32(dlg + DLG_TEXTH);
    if (te) { extern void trap_TEDispose(CPU *); CPU f; memset(&f, 0, sizeof f); f.r[3] = te; trap_TEDispose(&f); }
    mm_dispose_handle(rd32(dlg + DLG_ITEMS));
    di->dlg = 0;
    extern void trap_DisposeWindow(CPU *), trap_CloseWindow(CPU *);
    CPU f; memset(&f, 0, sizeof f); f.r[3] = dlg;
    if (free_storage) trap_DisposeWindow(&f); else trap_CloseWindow(&f);
}
TRAP(DisposeDialog) { dispose_dialog(ARG(0), true); }
TRAP(CloseDialog) { dispose_dialog(ARG(0), false); }
TRAP(DrawDialog) { draw_dialog(ARG(0)); }
TRAP(UpdateDialog) { draw_dialog(ARG(0)); }

TRAP(GetDialogItem) {
    u32 dlg = ARG(0); s16 n = ARGS16(1); u32 tp = ARG(2), hp = ARG(3), rp = ARG(4);
    Item it;
    if (!get_item(dlg, n, &it)) { if (tp) wr16(tp, 0); if (hp) wr32(hp, 0); return; }
    if (tp) wr16(tp, it.type);
    if (hp) wr32(hp, it.handle);
    if (rp) wr_rect(rp, it.r);
}
TRAP(SetDialogItem) {
    u32 dlg = ARG(0); s16 n = ARGS16(1); s16 type = ARGS16(2); u32 h = ARG(3), rp = ARG(4);
    Item it;
    if (!get_item(dlg, n, &it)) return;
    wr32(it.at, h);
    if (rp) wr_rect(it.at + 4, rd_rect(rp));
    wr8(it.at + 12, (u8)type);
}
TRAP(CountDITL) { RET(item_count(ARG(0))); }

TRAP(GetDialogItemText) {
    u32 h = ARG(0), out = ARG(1);
    /* if this is the active edit field, the TE holds the current text */
    for (int d = 0; d < MAX_DLG; d++) {
        u32 dlg = g_di[d].dlg;
        if (!dlg) continue;
        s16 ef = rds16(dlg + DLG_EDITFIELD);
        Item it;
        if (ef >= 0 && get_item(dlg, ef + 1, &it) && it.handle == h) {
            u32 te = rd32(dlg + DLG_TEXTH);
            u32 th = rd32(hderef(te) + 62);
            u32 n = (u32)rds16(hderef(te) + 60);
            mm_set_handle_size(h, n);
            if (n) gmemmove(hderef(h), hderef(th), n);
        }
    }
    u32 n = h && hderef(h) ? mm_handle_size(h) : 0;
    if (n > 255) n = 255;
    wr8(out, (u8)n);
    if (n) gmemmove(out + 1, hderef(h), n);
}

TRAP(SetDialogItemText) {
    u32 h = ARG(0), s = ARG(1);
    u8 n = rd8(s);
    mm_set_handle_size(h, n);
    if (n) gmemmove(hderef(h), s + 1, n);
    /* redraw the item wherever it lives */
    for (int d = 0; d < MAX_DLG; d++) {
        u32 dlg = g_di[d].dlg;
        if (!dlg) continue;
        int cnt = item_count(dlg);
        for (int i = 1; i <= cnt; i++) {
            Item it; get_item(dlg, i, &it);
            if (it.handle != h) continue;
            u32 save = qd_port();
            qd_set_port(dlg);
            if ((it.type & 0x7F) == editText && rds16(dlg + DLG_EDITFIELD) == i - 1) {
                u8 buf[256]; gmemcpy_from(buf, s + 1, n);
                te_api_settext(rd32(dlg + DLG_TEXTH), buf, n);
            }
            if ((it.type & 0x7F) == statText) {
                Paint bk; paint_back(&bk, dlg);
                draw_rect(dlg, it.r, &bk, patCopy);
            }
            if (rd8(dlg + WIN_VISIBLE)) draw_item(dlg, i);
            qd_set_port(save);
        }
    }
}

static void activate_field(u32 dlg, int n) {
    Item it;
    if (!get_item(dlg, n, &it) || (it.type & 0x7F) != editText) return;
    u32 te = rd32(dlg + DLG_TEXTH);
    s16 ef = rds16(dlg + DLG_EDITFIELD);
    if (ef == n - 1) return;
    /* save current field's text */
    if (ef >= 0 && te) {
        Item cur; get_item(dlg, ef + 1, &cur);
        u32 th = rd32(hderef(te) + 62);
        u32 len = (u32)rds16(hderef(te) + 60);
        mm_set_handle_size(cur.handle, len);
        if (len) gmemmove(hderef(cur.handle), hderef(th), len);
        te_api_activate(te, false);
        u32 save = qd_port(); qd_set_port(dlg);
        te_api_setselect(te, 0, 0);
        draw_item(dlg, ef + 1);
        qd_set_port(save);
    }
    if (!te) { te = te_api_new(it.r); wr32(dlg + DLG_TEXTH, te); }
    wr16(dlg + DLG_EDITFIELD, (u16)(n - 1));
    te_api_rects(te, it.r);
    u32 len = mm_handle_size(it.handle);
    u8 *s = malloc(len + 1); gmemcpy_from(s, hderef(it.handle), len);
    te_api_settext(te, s, (int)len);
    free(s);
    te_api_activate(te, true);
}

TRAP(SelectDialogItemText) {
    u32 dlg = ARG(0); s16 n = ARGS16(1); s16 a = ARGS16(2), b = ARGS16(3);
    u32 save = qd_port();
    qd_set_port(dlg);
    activate_field(dlg, n);
    u32 te = rd32(dlg + DLG_TEXTH);
    if (te) { te_api_activate(te, true); te_api_setselect(te, a, b); }
    qd_set_port(save);
}

TRAP(HideDialogItem) {
    u32 dlg = ARG(0); s16 n = ARGS16(1);
    Item it; if (!get_item(dlg, n, &it)) return;
    Rect r = it.r;
    if (r.left < 8192) { r.left += 16384; r.right += 16384; wr_rect(it.at + 4, r); }
    u8 t = it.type & 0x7F;
    if (t >= btnCtrl && t <= resCtrl && it.handle) { extern void trap_HideControl(CPU *); CPU f; memset(&f, 0, sizeof f); f.r[3] = it.handle; trap_HideControl(&f); }
    u32 save = qd_port(); qd_set_port(dlg);
    Paint bk; paint_back(&bk, dlg); draw_rect(dlg, it.r, &bk, patCopy);
    qd_set_port(save);
}
TRAP(ShowDialogItem) {
    u32 dlg = ARG(0); s16 n = ARGS16(1);
    Item it; if (!get_item(dlg, n, &it)) return;
    Rect r = it.r;
    if (r.left > 8192) { r.left -= 16384; r.right -= 16384; wr_rect(it.at + 4, r); }
    u8 t = it.type & 0x7F;
    if (t >= btnCtrl && t <= resCtrl && it.handle) { extern void trap_ShowControl(CPU *); CPU f; memset(&f, 0, sizeof f); f.r[3] = it.handle; trap_ShowControl(&f); }
    u32 save = qd_port(); qd_set_port(dlg);
    draw_item(dlg, n);
    qd_set_port(save);
}

static u32 cur_te(u32 dlg) { return is_dialog(dlg) && rds16(dlg + DLG_EDITFIELD) >= 0 ? rd32(dlg + DLG_TEXTH) : 0; }
TRAP(DialogCut) { u32 te = cur_te(ARG(0)); if (te) te_api_cut(te); }
TRAP(DialogCopy) { u32 te = cur_te(ARG(0)); if (te) te_api_copy(te); }
TRAP(DialogPaste) { u32 te = cur_te(ARG(0)); if (te) te_api_paste(te); }
TRAP(DialogDelete) { u32 te = cur_te(ARG(0)); if (te) te_api_delete(te); }

/* ---------------------------------------------------------------------- */
/* Event handling                                                          */

static int item_at(u32 dlg, Point lp) {
    int n = item_count(dlg);
    for (int i = 1; i <= n; i++) {
        Item it; get_item(dlg, i, &it);
        if (lp.h >= it.r.left && lp.h < it.r.right && lp.v >= it.r.top && lp.v < it.r.bottom) return i;
    }
    return 0;
}

static void flash_button(u32 dlg, int n) {
    Item it;
    if (!get_item(dlg, n, &it) || !it.handle) return;
    u8 t = it.type & 0x7F;
    if (t != btnCtrl && t != resCtrl) return;
    wr8(hderef(it.handle) + 17, 1); ctl_draw_one(it.handle); qd_present();
    u32 t0 = tick_count(); while (tick_count() - t0 < 8) ev_idle_frame();
    wr8(hderef(it.handle) + 17, 0); ctl_draw_one(it.handle); qd_present();
}

/* Handle one event for a dialog. Returns item hit or 0. */
static int handle_event(u32 dlg, u32 ep) {
    u16 what = rd16(ep);
    u32 msg = rd32(ep + 2);
    u32 save = qd_port();
    int hit = 0;
    qd_set_port(dlg);
    switch (what) {
    case 1: { /* mouseDown */
        Point gp = rd_point(ep + 10);
        u32 w;
        int part = wm_find(gp, &w);
        if (w != dlg || part != 3) break;
        Surf s; surf_from_port(dlg, &s);
        Point lp = { (s16)(gp.v + s.bounds.top), (s16)(gp.h + s.bounds.left) };
        int n = item_at(dlg, lp);
        if (!n) break;
        Item it; get_item(dlg, n, &it);
        u8 t = it.type & 0x7F;
        bool enabled = !(it.type & itemDisable);
        if (t >= btnCtrl && t <= resCtrl && it.handle) {
            extern void trap_TrackControl(CPU *);
            CPU f; memset(&f, 0, sizeof f); f.r[3] = it.handle; f.r[4] = pt_to_u32(lp); f.r[5] = 0xFFFFFFFFu;
            trap_TrackControl(&f);
            if (f.r[3] && enabled) hit = n;
        } else if (t == editText) {
            activate_field(dlg, n);
            u32 te = rd32(dlg + DLG_TEXTH);
            te_api_click(te, lp, (rd16(ep + 14) & 0x0200) != 0);
            if (enabled) hit = n;
        } else if (enabled) hit = n;
        break;
    }
    case 3: case 5: { /* keyDown, autoKey */
        u8 ch = (u8)msg;
        if (ch == 0x0D || ch == 0x03) {
            s16 def = rds16(dlg + DLG_DEFITEM);
            if (def <= 0) def = 1;
            Item it;
            if (get_item(dlg, def, &it) && ((it.type & 0x7F) == btnCtrl) && !(it.type & itemDisable) &&
                it.handle && rd8(hderef(it.handle) + 17) != 255) {
                flash_button(dlg, def);
                hit = def;
                break;
            }
        }
        s16 ef = rds16(dlg + DLG_EDITFIELD);
        if (ef >= 0) {
            if (ch == 0x09) { /* tab to next edit field */
                int n = item_count(dlg);
                for (int k = 1; k <= n; k++) {
                    int idx = (ef + k) % n + 1;
                    Item it; get_item(dlg, idx, &it);
                    if ((it.type & 0x7F) == editText) { activate_field(dlg, idx); te_api_setselect(rd32(dlg + DLG_TEXTH), 0, 32767); break; }
                }
            } else te_api_key(rd32(dlg + DLG_TEXTH), ch);
            hit = rds16(dlg + DLG_EDITFIELD) + 1;
        }
        break;
    }
    case 6: /* update */
        if (msg == dlg) {
            extern void trap_BeginUpdate(CPU *), trap_EndUpdate(CPU *);
            CPU f; memset(&f, 0, sizeof f); f.r[3] = dlg;
            trap_BeginUpdate(&f);
            draw_dialog(dlg);
            memset(&f, 0, sizeof f); f.r[3] = dlg;
            trap_EndUpdate(&f);
        }
        break;
    case 8: /* activate */
        if (msg == dlg) {
            u32 te = cur_te(dlg);
            if (te) te_api_activate(te, rd16(ep + 14) & 1);
        }
        break;
    case 0: {
        u32 te = cur_te(dlg);
        if (te) te_api_idle(te);
        break;
    }
    }
    qd_set_port(save);
    return hit;
}

static void next_event(u32 ep) {
    extern void trap_WaitNextEvent(CPU *);
    CPU f; memset(&f, 0, sizeof f);
    f.r[3] = 0xFFFF; f.r[4] = ep; f.r[5] = 1; f.r[6] = 0;
    trap_WaitNextEvent(&f);
}

TRAP(ModalDialog) {
    u32 filter = ARG(0), hitp = ARG(1);
    u32 dlg = wm_front();
    if (!is_dialog(dlg)) { LOG_W("ModalDialog: front window is not a dialog"); if (hitp) wr16(hitp, 1); return; }
    u32 ep = mm_new_ptr(16, true, ZONE_SYS);
    for (;;) {
        next_event(ep);
        if (filter) {
            wr16(hitp, 0);
            u32 a[3] = { dlg, ep, hitp };
            u32 r = call_upp(filter, 3, a);
            if (r & 0xFF) break;
        }
        u16 what = rd16(ep);
        if (what == 6 && rd32(ep + 2) != dlg) continue;
        int hit = handle_event(dlg, ep);
        if (hit) { wr16(hitp, (u16)hit); break; }
    }
    mm_dispose_ptr(ep);
}

TRAP(IsDialogEvent) {
    u32 ep = ARG(0);
    u16 what = rd16(ep);
    u32 w = 0;
    if (what == 6 || what == 8) w = rd32(ep + 2);
    else if (what == 1) wm_find(rd_point(ep + 10), &w);
    else w = wm_front();
    RET(is_dialog(w));
}

TRAP(DialogSelect) {
    u32 ep = ARG(0), dlgp = ARG(1), itemp = ARG(2);
    u16 what = rd16(ep);
    u32 w = 0;
    if (what == 6 || what == 8) w = rd32(ep + 2);
    else if (what == 1) wm_find(rd_point(ep + 10), &w);
    else w = wm_front();
    if (!is_dialog(w)) { RET(0); return; }
    wr32(dlgp, w);
    int hit = handle_event(w, ep);
    wr16(itemp, (u16)hit);
    RET(hit != 0);
}

TRAP(SetDialogDefaultItem) { wr16(ARG(0) + DLG_DEFITEM, (u16)ARGS16(1)); RETERR(noErr); }
TRAP(SetDialogCancelItem) { RETERR(noErr); }
TRAP(SetDialogTracksCursor) { RETERR(noErr); }
TRAP(GetDialogDefaultItem) { RET(rds16(ARG(0) + DLG_DEFITEM)); }

/* ---------------------------------------------------------------------- */
/* Alerts                                                                  */

static void draw_alert_icon(u32 dlg, int kind) {
    /* simple native icons: 0 stop, 1 note, 2 caution */
    Rect r = mkrect(10, 20, 42, 52);
    u32 save = qd_port();
    qd_set_port(dlg);
    RGB c = kind == 0 ? (RGB){ 0xDDDD, 0, 0 } : kind == 2 ? (RGB){ 0xFFFF, 0xCCCC, 0 } : (RGB){ 0, 0, 0xCCCC };
    extern void qd_oval_rgn(HRgn *r, Rect rc, int ow, int oh);
    HRgn o; qd_oval_rgn(&o, r, 32, 32);
    Paint p = { .kind = 0, .fg = c };
    draw_hrgn(dlg, &o, &p, patCopy);
    hrgn_free(&o);
    u8 bang = '!';
    wr_rgb(dlg + PORT_RGBFG, (RGB){ 0xFFFF, 0xFFFF, 0xFFFF });
    wr16(dlg + PORT_TXFONT, 0); wr16(dlg + PORT_TXSIZE, 18); wr8(dlg + PORT_TXFACE, 1);
    wr16(dlg + PORT_PNLOC + 2, 33); wr16(dlg + PORT_PNLOC, 33);
    text_draw(dlg, &bang, 1);
    wr_rgb(dlg + PORT_RGBFG, (RGB){ 0, 0, 0 });
    wr16(dlg + PORT_TXSIZE, 12); wr8(dlg + PORT_TXFACE, 0);
    qd_set_port(save);
}

static void outline_default(u32 dlg, int item) {
    Item it;
    if (!get_item(dlg, item, &it) || (it.type & 0x7F) != btnCtrl) return;
    Rect r = mkrect(it.r.top - 4, it.r.left - 4, it.r.bottom + 4, it.r.right + 4);
    extern void qd_oval_rgn(HRgn *r, Rect rc, int ow, int oh);
    HRgn o, i, f = { 0, NULL };
    qd_oval_rgn(&o, r, 16, 16);
    qd_oval_rgn(&i, mkrect(r.top + 3, r.left + 3, r.bottom - 3, r.right - 3), 10, 10);
    hrgn_op(&f, &o, &i, 2);
    Paint k = { .kind = 0 };
    u32 save = qd_port(); qd_set_port(dlg);
    draw_hrgn(dlg, &f, &k, patCopy);
    qd_set_port(save);
    hrgn_free(&o); hrgn_free(&i); hrgn_free(&f);
}

static int run_alert(s16 id, u32 filter, int kind) {
    u32 len;
    u8 *a = res_load_raw(FOURCC('A','L','R','T'), id, &len);
    if (!a) { LOG_W("ALRT %d not found", id); return 1; }
    Rect r = { (s16)be16(a), (s16)be16(a + 2), (s16)be16(a + 4), (s16)be16(a + 6) };
    s16 ditl_id = (s16)be16(a + 8);
    free(a);
    int w = r.right - r.left, h = r.bottom - r.top;
    int mb = rds16(LM_MBarHeight);
    int x = (qd_screen_w() - w) / 2, y = mb + (qd_screen_h() - mb - h) / 3;
    r = mkrect(y, x, y + h, x + w);
    u32 dl;
    u8 *ditl = res_load_raw(FOURCC('D','I','T','L'), ditl_id, &dl);
    if (!ditl) return 1;
    u32 save = qd_port(); /* Alert returns with the caller's port current */
    u32 dlg = new_dialog(0, r, "", false, 1, 0xFFFFFFFFu, false, 0, ditl, dl, 0);
    free(ditl);
    dinfo(dlg)->is_alert = true;
    wm_show(dlg, true);
    extern void trap_InitCursor(CPU *);
    CPU f; memset(&f, 0, sizeof f); trap_InitCursor(&f);
    draw_dialog(dlg);
    if (kind >= 0) draw_alert_icon(dlg, kind);
    outline_default(dlg, 1);
    wr16(dlg + DLG_DEFITEM, 1);
    u32 hitp = mm_new_ptr(2, true, ZONE_SYS);
    u32 ep = mm_new_ptr(16, true, ZONE_SYS);
    int hit = 0;
    /* log the alert text for headless runs */
    for (int i = 1; i <= item_count(dlg); i++) {
        Item it; get_item(dlg, i, &it);
        if ((it.type & 0x7F) == statText && it.handle) {
            u32 n = mm_handle_size(it.handle);
            u8 raw[1100], out[1100]; int on;
            if (n > 1000) n = 1000;
            gmemcpy_from(raw, hderef(it.handle), n);
            subst_param(raw, (int)n, out, &on);
            out[on] = 0;
            LOG_I("Alert %d: %s", id, (char *)out);
        }
    }
    for (;;) {
        next_event(ep);
        if (rd16(ep) == 6 && rd32(ep + 2) == dlg) {
            handle_event(dlg, ep);
            if (kind >= 0) draw_alert_icon(dlg, kind);
            outline_default(dlg, 1);
            continue;
        }
        if (filter) {
            wr16(hitp, 0);
            u32 fa[3] = { dlg, ep, hitp };
            if (call_upp(filter, 3, fa) & 0xFF) { hit = rds16(hitp); break; }
        }
        hit = handle_event(dlg, ep);
        if (hit) break;
    }
    mm_dispose_ptr(ep); mm_dispose_ptr(hitp);
    dispose_dialog(dlg, true);
    if (save != dlg) qd_set_port(save);
    return hit;
}

TRAP(Alert) { RET(run_alert(ARGS16(0), ARG(1), -1)); }
TRAP(StopAlert) { RET(run_alert(ARGS16(0), ARG(1), 0)); }
TRAP(NoteAlert) { RET(run_alert(ARGS16(0), ARG(1), 1)); }
TRAP(CautionAlert) { RET(run_alert(ARGS16(0), ARG(1), 2)); }

/* ---------------------------------------------------------------------- */
/* Standard File (Navigation Services is reported absent)                  */

#include "files.h"

typedef struct { u8 b[4096]; u32 n; int count; } Ditl;
static void ditl_add(Ditl *d, Rect r, u8 type, const char *text) {
    if (!d->n) d->n = 2;
    u8 *p = d->b + d->n;
    memset(p, 0, 4);
    put_be16(p + 4, (u16)r.top); put_be16(p + 6, (u16)r.left); put_be16(p + 8, (u16)r.bottom); put_be16(p + 10, (u16)r.right);
    p[12] = type;
    size_t l = strlen(text);
    p[13] = (u8)l;
    memcpy(p + 14, text, l);
    d->n += 14 + (u32)l + (l & 1);
    d->count++;
    put_be16(d->b, (u16)(d->count - 1));
}

static u32 sf_dialog(int w, int h, Ditl *d) {
    int mb = rds16(LM_MBarHeight);
    int x = (qd_screen_w() - w) / 2, y = mb + (qd_screen_h() - mb - h) / 3;
    u32 dlg = new_dialog(0, mkrect(y, x, y + h, x + w), "", false, 1, 0xFFFFFFFFu, false, 0, d->b, d->n, 0);
    wr16(dlg + DLG_DEFITEM, 1);
    wm_show(dlg, true);
    draw_dialog(dlg);
    outline_default(dlg, 1);
    return dlg;
}

static void fill_reply(u32 reply, bool good, bool replacing, u32 type, s32 dir, const char *name) {
    gmemset(reply, 0, 88);
    wr8(reply, good);
    wr8(reply + 1, replacing);
    wr32(reply + 2, type);
    if (good) fsspec_write(reply + 6, VOL_REFNUM, dir, name);
    wr16(reply + 76, 0);
}

TRAP(StandardPutFile) {
    u32 prompt = ARG(0), defname = ARG(1), reply = ARG(2);
    char pr[256], dn[256];
    pstr_to_c(prompt, pr, sizeof pr);
    pstr_to_c(defname, dn, sizeof dn);
    LOG_I("StandardPutFile('%s', '%s')", pr, dn);
    Ditl d = { .n = 0 };
    ditl_add(&d, mkrect(100, 280, 120, 350), btnCtrl, "Save");
    ditl_add(&d, mkrect(100, 200, 120, 270), btnCtrl, "Cancel");
    ditl_add(&d, mkrect(12, 15, 30, 355), statText | itemDisable, pr);
    ditl_add(&d, mkrect(42, 18, 58, 352), editText, dn);
    ditl_add(&d, mkrect(70, 15, 88, 355), statText | itemDisable, "Location: Saved Games");
    u32 save = qd_port(); /* the caller's port, restored at the end */
    u32 dlg = sf_dialog(370, 132, &d);
    qd_set_port(dlg);
    u32 te = rd32(dlg + DLG_TEXTH);
    te_api_activate(te, true);
    te_api_setselect(te, 0, 32767);
    u32 ep = mm_new_ptr(16, true, ZONE_SYS);
    int hit = 0;
    for (;;) {
        next_event(ep);
        if (rd16(ep) == 6 && rd32(ep + 2) == dlg) { handle_event(dlg, ep); outline_default(dlg, 1); continue; }
        if (rd16(ep) == 6) continue;
        if ((rd16(ep) == 3) && ((u8)rd32(ep + 2) == 0x1B)) { hit = 2; break; }
        hit = handle_event(dlg, ep);
        if (hit == 1 || hit == 2) break;
    }
    char name[256] = "";
    if (hit == 1) {
        u32 th = rd32(hderef(te) + 62);
        int n = rds16(hderef(te) + 60);
        if (n > 31) n = 31; /* HFS file names are at most 31 characters (Standard File enforces it) */
        gmemcpy_from(name, hderef(th), (u32)n);
        name[n] = 0;
        for (int i = 0; name[i]; i++) if (name[i] == ':') name[i] = '-';
    }
    mm_dispose_ptr(ep);
    dispose_dialog(dlg, true);
    qd_set_port(save);
    if (hit == 1 && name[0]) {
        char host[1100];
        int err = vfs_resolve(VOL_REFNUM, files_saves_dir(), name, host, sizeof host, NULL, NULL);
        fill_reply(reply, true, err == noErr, 0, files_saves_dir(), name);
        LOG_I("StandardPutFile -> '%s'%s", name, err == noErr ? " (replacing)" : "");
    } else fill_reply(reply, false, false, 0, 0, "");
}

/* StandardGetFilePreview's preview box: the selected file's QuickTime
   preview (the thumbnail AddFilePreview stored), centred. */
static void sf_draw_preview(u32 dlg, Rect box, const char *name) {
    u32 save = qd_port();
    qd_set_port(dlg);
    Paint w = { .kind = 0, .fg = { 0xFFFF, 0xFFFF, 0xFFFF } };
    draw_rect(dlg, mkrect(box.top + 1, box.left + 1, box.bottom - 1, box.right - 1), &w, patCopy);
    draw_frame_rect(dlg, box);
    char host[1100];
    u32 type = 0, len = 0;
    u8 *d = NULL;
    if (name && vfs_resolve(VOL_REFNUM, files_saves_dir(), name, host, sizeof host, NULL, NULL) == noErr)
        d = res_file_preview(host, &type, &len);
    if (d && type == FOURCC('P','I','C','T') && len >= 10) {
        Rect fr = { (s16)be16(d + 2), (s16)be16(d + 4), (s16)be16(d + 6), (s16)be16(d + 8) };
        int fw = fr.right - fr.left, fh = fr.bottom - fr.top;
        int bw = box.right - box.left - 4, bh = box.bottom - box.top - 4;
        if (fw > 0 && fh > 0) {
            if (fw > bw || fh > bh) { /* shrink to fit, keeping the aspect ratio */
                if (fw * bh > fh * bw) { fh = fh * bw / fw; fw = bw; } else { fw = fw * bh / fh; fh = bh; }
            }
            int x = (box.left + box.right - fw) / 2, y = (box.top + box.bottom - fh) / 2;
            u32 h = mm_handle_from_data(d, len, ZONE_SYS);
            if (h) { pict_draw(h, mkrect(y, x, y + fh, x + fw)); mm_dispose_handle(h); }
        }
    }
    free(d);
    qd_set_port(save);
}

static void std_get(CPU *cpu, u32 filter, s16 ntypes, u32 types, u32 reply, bool preview) {
    (void)filter;
    static char names[512][64];
    static char shown[512][64];
    int n = vfs_list_files(files_saves_dir(), names, 512);
    int k = 0;
    for (int i = 0; i < n; i++) {
        bool ok = ntypes < 0 || ntypes == 0 ? true : false;
        if (!ok) {
            char host[1100];
            vfs_resolve(VOL_REFNUM, files_saves_dir(), names[i], host, sizeof host, NULL, NULL);
            u32 t, c; u16 fl; finfo_get(host, &t, &c, &fl);
            for (int j = 0; j < ntypes; j++) if (rd32(types + 4 * (u32)j) == t) ok = true;
        }
        if (ok) snprintf(shown[k++], 64, "%s", names[i]);
    }
    LOG_I("StandardGetFile: %d candidate files", k);
    s16 px = preview ? 116 : 0; /* the preview box goes on the left */
    Rect pbox = mkrect(36, 15, 136, 115);
    Ditl d = { .n = 0 };
    ditl_add(&d, mkrect(222, 280 + px, 242, 350 + px), btnCtrl, "Open");
    ditl_add(&d, mkrect(222, 200 + px, 242, 270 + px), btnCtrl, "Cancel");
    ditl_add(&d, mkrect(12, 15 + px, 30, 355 + px), statText | itemDisable, "Saved Games:");
    ditl_add(&d, mkrect(36, 16 + px, 210, 338 + px), userItem | itemDisable, "");
    if (preview) ditl_add(&d, mkrect(12, 15, 30, 115), statText | itemDisable, "Preview");
    u32 save = qd_port(); /* the caller's port, restored at the end */
    u32 dlg = sf_dialog(370 + px, 254, &d);
    qd_set_port(dlg);
    /* list */
    u32 rp = mm_new_ptr(8, false, ZONE_SYS), bp = mm_new_ptr(8, false, ZONE_SYS);
    Rect lr = mkrect(37, 17 + px, 209, 321 + px);
    wr_rect(rp, lr);
    wr_rect(bp, mkrect(0, 0, (s16)k, 1));
    extern void trap_LNew(CPU *), trap_LSetCell(CPU *), trap_LSetSelect(CPU *), trap_LClick(CPU *),
                trap_LGetSelect(CPU *), trap_LUpdate(CPU *), trap_LDispose(CPU *), trap_LAutoScroll(CPU *);
    CPU f; memset(&f, 0, sizeof f);
    f.r[3] = rp; f.r[4] = bp; f.r[5] = 0; f.r[6] = 0; f.r[7] = dlg; f.r[8] = 1; f.r[9] = 0; f.r[10] = 0;
    u32 sp = cpu->r[1]; (void)sp;
    /* 9th argument (scrollVert) goes on the stack: emulate by direct flag */
    u32 stack = mm_new_ptr(64, true, ZONE_SYS);
    f.r[1] = stack;
    wr32(stack + 24 + 4 * 8, 1);
    trap_LNew(&f);
    u32 list = f.r[3];
    for (int i = 0; i < k; i++) {
        u32 buf = mm_new_ptr(64, false, ZONE_SYS);
        size_t l = strlen(shown[i]);
        gmemcpy_to(buf, shown[i], (u32)l);
        memset(&f, 0, sizeof f); f.r[3] = buf; f.r[4] = (u32)l; f.r[5] = (u32)i << 16; f.r[6] = list;
        trap_LSetCell(&f);
        mm_dispose_ptr(buf);
    }
    if (k) { memset(&f, 0, sizeof f); f.r[3] = 1; f.r[4] = 0; f.r[5] = list; trap_LSetSelect(&f); }
    draw_frame_rect(dlg, mkrect(lr.top - 1, lr.left - 1, lr.bottom + 1, lr.right + 17));
    u32 ep = mm_new_ptr(16, true, ZONE_SYS);
    int hit = 0;
    u32 cellp = mm_new_ptr(4, true, ZONE_SYS);
    int shown_sel = -2; /* file whose preview is on screen */
    for (;;) {
        if (preview) {
            wr32(cellp, 0);
            memset(&f, 0, sizeof f); f.r[3] = 1; f.r[4] = cellp; f.r[5] = list;
            trap_LGetSelect(&f);
            int cur = (f.r[3] & 0xFF) ? rds16(cellp) : -1;
            if (cur != shown_sel) { shown_sel = cur; sf_draw_preview(dlg, pbox, cur >= 0 && cur < k ? shown[cur] : NULL); }
        }
        next_event(ep);
        u16 what = rd16(ep);
        if (what == 6 && rd32(ep + 2) == dlg) {
            handle_event(dlg, ep);
            outline_default(dlg, 1);
            draw_frame_rect(dlg, mkrect(lr.top - 1, lr.left - 1, lr.bottom + 1, lr.right + 17));
            memset(&f, 0, sizeof f); f.r[3] = 0; f.r[4] = list; trap_LUpdate(&f);
            shown_sel = -2;
            continue;
        }
        if (what == 6) continue;
        if (what == 1) {
            Point gp = rd_point(ep + 10);
            Surf s; surf_from_port(dlg, &s);
            Point lp = { (s16)(gp.v + s.bounds.top), (s16)(gp.h + s.bounds.left) };
            if (lp.h >= lr.left && lp.h < lr.right + 16 && lp.v >= lr.top && lp.v < lr.bottom) {
                memset(&f, 0, sizeof f); f.r[3] = pt_to_u32(lp); f.r[4] = rd16(ep + 14); f.r[5] = list;
                trap_LClick(&f);
                if (f.r[3] & 0xFF) { hit = 1; break; }
                continue;
            }
        }
        if (what == 3 || what == 5) {
            u8 ch = (u8)rd32(ep + 2);
            if (ch == 0x1B) { hit = 2; break; }
            if ((ch == 0x1E || ch == 0x1F) && k) {
                wr32(cellp, 0);
                memset(&f, 0, sizeof f); f.r[3] = 1; f.r[4] = cellp; f.r[5] = list;
                trap_LGetSelect(&f);
                int row = (f.r[3] & 0xFF) ? rds16(cellp) : 0;
                int nr = ch == 0x1E ? row - 1 : row + 1;
                if (nr >= 0 && nr < k) {
                    memset(&f, 0, sizeof f); f.r[3] = 0; f.r[4] = (u32)row << 16; f.r[5] = list; trap_LSetSelect(&f);
                    memset(&f, 0, sizeof f); f.r[3] = 1; f.r[4] = (u32)nr << 16; f.r[5] = list; trap_LSetSelect(&f);
                    memset(&f, 0, sizeof f); f.r[3] = list; trap_LAutoScroll(&f);
                }
                continue;
            }
        }
        hit = handle_event(dlg, ep);
        if (hit == 1 || hit == 2) break;
    }
    int sel = -1;
    if (hit == 1) {
        wr32(cellp, 0);
        memset(&f, 0, sizeof f); f.r[3] = 1; f.r[4] = cellp; f.r[5] = list;
        trap_LGetSelect(&f);
        if (f.r[3] & 0xFF) sel = rds16(cellp);
    }
    memset(&f, 0, sizeof f); f.r[3] = list; trap_LDispose(&f);
    mm_dispose_ptr(ep); mm_dispose_ptr(cellp); mm_dispose_ptr(rp); mm_dispose_ptr(bp); mm_dispose_ptr(stack);
    dispose_dialog(dlg, true);
    qd_set_port(save);
    if (sel >= 0 && sel < k) {
        char host[1100];
        vfs_resolve(VOL_REFNUM, files_saves_dir(), shown[sel], host, sizeof host, NULL, NULL);
        u32 t, c; u16 fl; finfo_get(host, &t, &c, &fl);
        fill_reply(reply, true, false, t, files_saves_dir(), shown[sel]);
        LOG_I("StandardGetFile -> '%s'", shown[sel]);
    } else fill_reply(reply, false, false, 0, 0, "");
}

TRAP(StandardGetFile) { std_get(cpu, ARG(0), ARGS16(1), ARG(2), ARG(3), false); }
TRAP(StandardGetFilePreview) { std_get(cpu, ARG(0), ARGS16(1), ARG(2), ARG(3), true); }
