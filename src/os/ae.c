/* Apple Event Manager (the calls the game imports).
 *
 * Events come from the host side: 'oapp' at start-up, 'quit' when the window
 * is closed, and 'cnfg' (kAESystemConfigNotice) when the emulated screen
 * changes size, the Display Manager's notice that the game handles
 * (HandleDisplayNotice -> TApp::DoMonitorChanged).
 *
 * Descriptors are {descriptorType, dataHandle}. Lists, records and events
 * ('list', 'reco', 'aevt') hold their items in the data handle: a u32
 * count, then per item u32 keyword, u32 type, u32 size and the data, padded
 * to 4 bytes. Other descriptors hold their raw data.
 */
#include "os.h"
#include "mm.h"
#include "misc.h"

#define errAEDescNotFound (-1701)
#define errAEBadListItem  (-1705)
#define kHighLevelEvent 23

static bool is_aggregate(u32 type) {
    return type == FOURCC('l','i','s','t') || type == FOURCC('r','e','c','o') || type == FOURCC('a','e','v','t');
}

/* ---- building records (host side) ---- */
u32 ae_rec_new(void) { return mm_new_handle(4, true, ZONE_SYS); }

static void rec_add(u32 h, u32 key, u32 type, const u8 *data, u32 guest_data, u32 size) {
    u32 old = mm_handle_size(h), pad = (size + 3) & ~3u;
    mm_set_handle_size(h, old + 12 + pad);
    u32 p = hderef(h), at = p + old;
    wr32(at, key); wr32(at + 4, type); wr32(at + 8, size);
    gmemset(at + 12, 0, pad);
    if (data) gmemcpy_to(at + 12, data, size);
    else if (size) gmemmove(at + 12, guest_data, size);
    wr32(p, rd32(p) + 1);
}
/* Add host bytes as an item. */
void ae_rec_put(u32 h, u32 key, u32 type, const void *data, u32 size) { rec_add(h, key, type, data, 0, size); }
/* Add a record or list (its data handle) as an item, then dispose of it. */
void ae_rec_put_rec(u32 h, u32 key, u32 type, u32 sub) {
    rec_add(h, key, type, NULL, hderef(sub), mm_handle_size(sub));
    mm_dispose_handle(sub);
}

/* ---- reading ---- */
/* Item `index` (1-based), or the item with keyword `key` if index is 0. */
static bool rec_item(u32 desc, u32 index, u32 key, u32 *okey, u32 *otype, u32 *odata, u32 *osize) {
    if (!desc || !is_aggregate(rd32(desc))) return false;
    u32 h = rd32(desc + 4);
    if (!h || !hderef(h) || mm_handle_size(h) < 4) return false;
    u32 p = hderef(h), end = p + mm_handle_size(h), n = rd32(p), at = p + 4;
    for (u32 i = 1; i <= n && at + 12 <= end; i++) {
        u32 k = rd32(at), t = rd32(at + 4), sz = rd32(at + 8);
        if (at + 12 + sz > end) return false;
        if (index ? i == index : k == key) {
            if (okey) *okey = k;
            *otype = t; *odata = at + 12; *osize = sz;
            return true;
        }
        at += 12 + ((sz + 3) & ~3u);
    }
    return false;
}
static void make_desc(u32 d, u32 type, u32 data, u32 size) {
    u32 h = mm_new_handle(size, false, ZONE_APP);
    if (size) gmemmove(hderef(h), data, size);
    wr32(d, type); wr32(d + 4, h);
}
static void null_desc(u32 d) { if (d) { wr32(d, FOURCC('n','u','l','l')); wr32(d + 4, 0); } }

/* AEGetParamDesc(theAppleEvent, keyword, desiredType, result) */
TRAP(AEGetParamDesc) {
    u32 t, data, size, d = ARG(3);
    if (!rec_item(ARG(0), 0, ARG(1), NULL, &t, &data, &size)) { null_desc(d); RETERR(errAEDescNotFound); return; }
    make_desc(d, t, data, size);
    RETERR(noErr);
}
/* AEGetParamPtr(theAppleEvent, keyword, desiredType, &typeCode, dataPtr, maximumSize, &actualSize) */
TRAP(AEGetParamPtr) {
    u32 t, data, size;
    if (!rec_item(ARG(0), 0, ARG(1), NULL, &t, &data, &size)) { RETERR(errAEDescNotFound); return; }
    u32 max = ARG(5);
    if (ARG(3)) wr32(ARG(3), t);
    if (ARG(4)) gmemmove(ARG(4), data, size < max ? size : max);
    if (ARG(6)) wr32(ARG(6), size);
    RETERR(noErr);
}
TRAP(AEGetAttributePtr) { RETERR(errAEDescNotFound); }
TRAP(AECountItems) {
    u32 d = ARG(0), p = ARG(1), n = 0;
    if (d && is_aggregate(rd32(d)) && rd32(d + 4) && hderef(rd32(d + 4))) n = rd32(hderef(rd32(d + 4)));
    if (p) wr32(p, n);
    RETERR(noErr);
}
/* AEGetNthDesc(theAEDescList, index, desiredType, &keyword, result) */
TRAP(AEGetNthDesc) {
    u32 k, t, data, size, d = ARG(4);
    if (!rec_item(ARG(0), ARG(1), 0, &k, &t, &data, &size)) { null_desc(d); RETERR(errAEBadListItem); return; }
    if (ARG(3)) wr32(ARG(3), k);
    make_desc(d, t, data, size);
    RETERR(noErr);
}
/* AEGetNthPtr(theAEDescList, index, desiredType, &keyword, &typeCode, dataPtr, maximumSize, &actualSize) */
TRAP(AEGetNthPtr) {
    u32 k, t, data, size;
    if (!rec_item(ARG(0), ARG(1), 0, &k, &t, &data, &size)) { RETERR(errAEBadListItem); return; }
    u32 max = ARG(6);
    if (ARG(3)) wr32(ARG(3), k);
    if (ARG(4)) wr32(ARG(4), t);
    if (ARG(5)) gmemmove(ARG(5), data, size < max ? size : max);
    if (ARG(7)) wr32(ARG(7), size);
    RETERR(noErr);
}
/* AESizeOfNthItem(theAEDescList, index, &typeCode, &dataSize) */
TRAP(AESizeOfNthItem) {
    u32 t, data, size;
    if (!rec_item(ARG(0), ARG(1), 0, NULL, &t, &data, &size)) { RETERR(errAEBadListItem); return; }
    if (ARG(2)) wr32(ARG(2), t);
    if (ARG(3)) wr32(ARG(3), size);
    RETERR(noErr);
}
TRAP(AECreateDesc) {
    u32 type = ARG(0), data = ARG(1), size = ARG(2), d = ARG(3);
    u32 h = mm_new_handle(size, false, ZONE_APP);
    if (size && data) gmemmove(hderef(h), data, size);
    wr32(d, type); wr32(d + 4, h);
    RETERR(noErr);
}
TRAP(AEDisposeDesc) {
    u32 d = ARG(0);
    if (d && rd32(d + 4)) mm_dispose_handle(rd32(d + 4));
    null_desc(d);
    RETERR(noErr);
}

/* ---- handlers and dispatch ---- */
typedef struct { u32 cls, id, handler, refcon; } AEHandler;
static AEHandler g_ae[32];
static int g_nae;

TRAP(AEInstallEventHandler) {
    u32 cls = ARG(0), id = ARG(1), h = ARG(2), refcon = ARG(3);
    for (int i = 0; i < g_nae; i++) if (g_ae[i].cls == cls && g_ae[i].id == id) { g_ae[i].handler = h; g_ae[i].refcon = refcon; RETERR(noErr); return; }
    if (g_nae < 32) g_ae[g_nae++] = (AEHandler){ cls, id, h, refcon };
    RETERR(noErr);
}

/* Events posted by the host, with their parameters, in order. The high-level
   event in the queue carries only class and ID (message and where). */
typedef struct { u32 cls, id, params; } Pending;
static Pending g_pending[8];
static int g_npending;

void ae_post(u32 cls, u32 id, u32 params) {
    extern void ev_post_high_level(u32 cls, u32 id);
    if (g_npending < 8) g_pending[g_npending++] = (Pending){ cls, id, params };
    ev_post_high_level(cls, id);
}

/* The Display Manager's notice after a display mode change: 'aevt'/'cnfg'
   with keyDisplayNotice ('dspl'), a list of one item per display, each a
   record of the old and new configuration ('dold', 'dnew'), which hold the
   device ('dmdd') and its rectangle ('dddr'). */
static u32 display_config(u32 gd, Rect r) {
    u32 c = ae_rec_new();
    u8 rect[8] = { (u8)(r.top >> 8), (u8)r.top, (u8)(r.left >> 8), (u8)r.left,
                   (u8)(r.bottom >> 8), (u8)r.bottom, (u8)(r.right >> 8), (u8)r.right };
    u8 dev[4] = { (u8)(gd >> 24), (u8)(gd >> 16), (u8)(gd >> 8), (u8)gd };
    ae_rec_put(c, FOURCC('d','m','d','d'), FOURCC('l','o','n','g'), dev, 4);
    ae_rec_put(c, FOURCC('d','d','d','r'), FOURCC('q','d','r','t'), rect, 8);
    return c;
}
void ae_post_display_notice(u32 gd, Rect old, Rect now) {
    u32 item = ae_rec_new();
    ae_rec_put_rec(item, FOURCC('d','o','l','d'), FOURCC('r','e','c','o'), display_config(gd, old));
    ae_rec_put_rec(item, FOURCC('d','n','e','w'), FOURCC('r','e','c','o'), display_config(gd, now));
    u32 list = ae_rec_new();
    ae_rec_put_rec(list, FOURCC('*','*','*','*'), FOURCC('r','e','c','o'), item);
    u32 params = ae_rec_new();
    ae_rec_put_rec(params, FOURCC('d','s','p','l'), FOURCC('l','i','s','t'), list);
    ae_post(FOURCC('a','e','v','t'), FOURCC('c','n','f','g'), params);
}

TRAP(AEProcessAppleEvent) {
    u32 ep = ARG(0);
    u32 cls = rd32(ep + 2), id = rd32(ep + 10);
    u32 params = 0;
    for (int i = 0; i < g_npending; i++)
        if (g_pending[i].cls == cls && g_pending[i].id == id) {
            params = g_pending[i].params;
            memmove(&g_pending[i], &g_pending[i + 1], sizeof(Pending) * (size_t)(g_npending - i - 1));
            g_npending--;
            break;
        }
    if (!params) params = ae_rec_new();
    for (int i = 0; i < g_nae; i++) {
        if ((g_ae[i].cls == cls || g_ae[i].cls == FOURCC('*','*','*','*')) && (g_ae[i].id == id || g_ae[i].id == FOURCC('*','*','*','*'))) {
            u32 evt = sys_alloc(8), reply = sys_alloc(8);
            wr32(evt, FOURCC('a','e','v','t')); wr32(evt + 4, params);
            null_desc(reply);
            LOG_I("AppleEvent %s/%s -> handler", fourcc_str(cls), fourcc_str(id));
            u32 a[3] = { evt, reply, g_ae[i].refcon };
            call_upp(g_ae[i].handler, 3, a);
            mm_dispose_handle(params); /* the handler copies what it keeps */
            if (id == FOURCC('c','n','f','g')) { extern void wm_constrain_windows(void); wm_constrain_windows(); }
            RETERR(noErr);
            return;
        }
    }
    mm_dispose_handle(params);
    RETERR(errAEEventNotHandled);
}
