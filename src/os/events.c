/* Event Manager and a minimal Apple Event Manager. */
#include "wm.h"
#include "misc.h"
#include "threads.h"
#include "../host/host.h"

enum { nullEvent = 0, mouseDown = 1, mouseUp = 2, keyDown = 3, keyUp = 4, autoKey = 5, updateEvt = 6,
       diskEvt = 7, activateEvt = 8, osEvt = 15, kHighLevelEvent = 23 };

typedef struct { u16 what; u32 message; u32 when; Point where; u16 mods; } Ev;

#define EVQ 64
static Ev g_q[EVQ];
static int g_qn;
static u16 g_sysmask = 0xFFEF;
static Point g_mouse;
static bool g_button;
static u16 g_mods;
static u32 g_last_present;
static bool g_quit_sent;

u16 ev_modifiers(void) { return g_mods; }
Point ev_mouse_global(void) { return g_mouse; }

void ev_post(u16 what, u32 message, u16 mods) {
    if (g_qn == EVQ) { memmove(g_q, g_q + 1, sizeof(Ev) * (EVQ - 1)); g_qn--; }
    g_q[g_qn++] = (Ev){ what, message, tick_count(), g_mouse, mods };
}

static void post_host_events(void) {
    HostEvent he;
    while (host_next_event(&he)) {
        g_mods = he.mods;
        switch (he.type) {
        case HEV_MOUSE_DOWN:
            g_mouse = (Point){ (s16)he.y, (s16)he.x };
            g_button = true;
            ev_post(mouseDown, 0, he.mods);
            break;
        case HEV_MOUSE_UP:
            g_mouse = (Point){ (s16)he.y, (s16)he.x };
            g_button = false;
            ev_post(mouseUp, 0, he.mods);
            break;
        case HEV_KEY_DOWN:
            ev_post(he.repeat ? autoKey : keyDown, (u32)he.mac_key << 8 | he.ch, he.mods);
            break;
        case HEV_KEY_UP:
            ev_post(keyUp, (u32)he.mac_key << 8 | he.ch, he.mods);
            break;
        case HEV_QUIT:
            if (!g_quit_sent) { g_quit_sent = true; ev_post(kHighLevelEvent, FOURCC('a','e','v','t'), 0);
                g_q[g_qn - 1].where = pt_from_u32(FOURCC('q','u','i','t')); }
            else { host_shutdown(); exit(0); }
            break;
        case HEV_FOCUS_IN: ev_post(osEvt, 0x01000001u, 0); break;
        case HEV_FOCUS_OUT: ev_post(osEvt, 0x01000000u, 0); break;
        default: break;
        }
    }
    int x, y; bool d;
    host_mouse(&x, &y, &d);
    g_mouse = (Point){ (s16)y, (s16)x };
    if (!host_is_headless()) g_button = d;
}

void ev_pump_host(void) {
    host_pump(false);
    post_host_events();
}

bool ev_mouse_button(void) { ev_pump_host(); return g_button; }

/* Present the screen at most ~60 times per second, and yield the host. */
void ev_idle_frame(void) {
    u32 now = tick_count();
    if (now != g_last_present) { qd_present(); g_last_present = now; }
    host_pump(true);
    post_host_events();
    irq_service();
    misc_poll();
}

static bool mask_ok(u16 mask, u16 what) { return (mask >> what) & 1; }

/* Find and optionally remove the next event matching mask. */
static bool get_event(u16 mask, Ev *out, bool remove, bool os_only) {
    for (int i = 0; i < g_qn; i++) {
        if (!mask_ok(mask, g_q[i].what)) continue;
        if (!mask_ok(g_sysmask | (1 << activateEvt) | (1 << updateEvt) | (1 << kHighLevelEvent) | (1 << osEvt), g_q[i].what)) continue;
        /* activate events have priority */
        *out = g_q[i];
        if (remove) { memmove(&g_q[i], &g_q[i + 1], sizeof(Ev) * (size_t)(g_qn - i - 1)); g_qn--; }
        return true;
    }
    if (os_only) return false;
    if (mask_ok(mask, updateEvt)) {
        for (u32 w = wm_first(); w; w = rd32(w + WIN_NEXT)) {
            if (!rd8(w + WIN_VISIBLE)) continue;
            Rect bb = rgn_bbox(rd32(w + WIN_UPDATE));
            if (!rect_empty(bb)) {
                *out = (Ev){ updateEvt, w, tick_count(), g_mouse, 0 };
                return true;
            }
        }
    }
    return false;
}

static void write_event(u32 ep, const Ev *e) {
    u16 mods = e->mods | (g_button ? 0 : 0x0080);
    if (e->what != activateEvt) mods = (u16)((mods & ~1) | (g_mods & 0x1F00) | (g_button ? 0 : 0x80));
    wr16(ep, e->what);
    wr32(ep + 2, e->message);
    wr32(ep + 6, e->when);
    wr_point(ep + 10, e->where);
    wr16(ep + 14, mods);
}

static bool mouse_in_rgn(u32 rgn) {
    if (!rgn || !hderef(rgn)) return true;
    HRgn r; hrgn_from_guest(&r, rgn);
    bool in = hrgn_contains(&r, g_mouse.h, g_mouse.v);
    hrgn_free(&r);
    return in;
}

static bool g_oapp_sent;

TRAP(WaitNextEvent) {
    u16 mask = ARGU16(0); u32 ep = ARG(1); u32 sleep = ARG(2); u32 mrgn = ARG(3);
    if (!g_oapp_sent) {
        g_oapp_sent = true;
        ev_post(kHighLevelEvent, FOURCC('a','e','v','t'), 0);
        g_q[g_qn - 1].where = pt_from_u32(FOURCC('o','a','p','p'));
    }
    u32 start = tick_count();
    for (;;) {
        ev_idle_frame();
        Ev e;
        if (get_event(mask, &e, true, false)) {
            write_event(ep, &e);
            RET(1);
            return;
        }
        if (mrgn && !mouse_in_rgn(mrgn) && mask_ok(mask, osEvt)) {
            Ev mm = { osEvt, 0xFA000000u, tick_count(), g_mouse, 0 };
            write_event(ep, &mm);
            RET(1);
            return;
        }
        if ((s32)(tick_count() - start) >= (s32)(sleep ? 1 : 0)) break;
    }
    Ev n = { nullEvent, 0, tick_count(), g_mouse, 0 };
    write_event(ep, &n);
    RET(0);
}

TRAP(GetNextEvent) {
    u16 mask = ARGU16(0); u32 ep = ARG(1);
    ev_idle_frame();
    Ev e;
    if (get_event(mask, &e, true, false)) { write_event(ep, &e); RET(1); return; }
    Ev n = { nullEvent, 0, tick_count(), g_mouse, 0 };
    write_event(ep, &n);
    RET(0);
}

TRAP(EventAvail) {
    u16 mask = ARGU16(0); u32 ep = ARG(1);
    ev_pump_host();
    Ev e;
    if (get_event(mask, &e, false, false)) { write_event(ep, &e); RET(1); return; }
    Ev n = { nullEvent, 0, tick_count(), g_mouse, 0 };
    write_event(ep, &n);
    RET(0);
}

TRAP(GetOSEvent) {
    u16 mask = ARGU16(0); u32 ep = ARG(1);
    ev_pump_host();
    Ev e;
    if (get_event(mask, &e, true, true)) { write_event(ep, &e); RET(1); return; }
    Ev n = { nullEvent, 0, tick_count(), g_mouse, 0 };
    write_event(ep, &n);
    RET(0);
}

TRAP(OSEventAvail) {
    u16 mask = ARGU16(0); u32 ep = ARG(1);
    ev_pump_host();
    Ev e;
    if (get_event(mask, &e, false, true)) { write_event(ep, &e); RET(1); return; }
    Ev n = { nullEvent, 0, tick_count(), g_mouse, 0 };
    write_event(ep, &n);
    RET(0);
}

TRAP(FlushEvents) {
    u16 mask = ARGU16(0), stop = ARGU16(1);
    ev_pump_host();
    int j = 0;
    bool stopped = false;
    for (int i = 0; i < g_qn; i++) {
        if (!stopped && mask_ok(stop, g_q[i].what)) stopped = true;
        if (!stopped && mask_ok(mask, g_q[i].what)) continue;
        g_q[j++] = g_q[i];
    }
    g_qn = j;
}

TRAP(SetEventMask) { g_sysmask = ARGU16(0); }

TRAP(GetMouse) {
    u32 pp = ARG(0);
    ev_pump_host();
    Point p = g_mouse;
    Surf s;
    u32 port = qd_port();
    if (port && surf_from_port(port, &s)) { p.h = (s16)(p.h + s.bounds.left); p.v = (s16)(p.v + s.bounds.top); }
    wr_point(pp, p);
}
TRAP(Button) { RET(ev_mouse_button()); }
TRAP(StillDown) {
    ev_pump_host();
    Ev e;
    /* false if a mouseUp is pending */
    for (int i = 0; i < g_qn; i++) if (g_q[i].what == mouseUp || g_q[i].what == mouseDown) { RET(0); return; }
    (void)e;
    RET(g_button);
}
TRAP(WaitMouseUp) {
    ev_pump_host();
    for (int i = 0; i < g_qn; i++) if (g_q[i].what == mouseUp) {
        memmove(&g_q[i], &g_q[i + 1], sizeof(Ev) * (size_t)(g_qn - i - 1)); g_qn--;
        RET(0);
        return;
    }
    RET(g_button);
}
TRAP(GetKeys) {
    u8 km[16];
    ev_pump_host();
    host_keymap(km);
    /* The Mac KeyMap is four big-endian longs; key k is bit (k & 31) of
       long (k >> 5), i.e. byte (k>>3) bit (k&7) in memory order. */
    gmemcpy_to(ARG(0), km, 16);
}
TRAP(KeyTranslate) { RET((u32)(ARGU16(1) & 0xFF)); }
TRAP(GetCaretTime) { RET(32); }
TRAP(GetDblTime) { RET(30); }

void ev_init(void) {
    wr16(0x018E, 16); /* KeyThresh */
    wr16(0x0190, 4);  /* KeyRepThresh */
}

/* ---------------------------------------------------------------------- */
/* Apple Events (minimal)                                                  */

typedef struct { u32 cls, id, handler, refcon; } AEHandler;
static AEHandler g_ae[32];
static int g_nae;

TRAP(AEInstallEventHandler) {
    u32 cls = ARG(0), id = ARG(1), h = ARG(2), refcon = ARG(3);
    for (int i = 0; i < g_nae; i++) if (g_ae[i].cls == cls && g_ae[i].id == id) { g_ae[i].handler = h; g_ae[i].refcon = refcon; RETERR(noErr); return; }
    if (g_nae < 32) g_ae[g_nae++] = (AEHandler){ cls, id, h, refcon };
    RETERR(noErr);
}

TRAP(AEProcessAppleEvent) {
    u32 ep = ARG(0);
    u32 cls = rd32(ep + 2), id = rd32(ep + 10);
    for (int i = 0; i < g_nae; i++) {
        if ((g_ae[i].cls == cls || g_ae[i].cls == FOURCC('*','*','*','*')) && (g_ae[i].id == id || g_ae[i].id == FOURCC('*','*','*','*'))) {
            /* AppleEvent and reply descriptors: {descriptorType, dataHandle} */
            u32 evt = sys_alloc(8), reply = sys_alloc(8);
            wr32(evt, FOURCC('a','e','v','t'));
            u32 dh = mm_new_handle(8, true, ZONE_SYS);
            wr32(hderef(dh), cls); wr32(hderef(dh) + 4, id);
            wr32(evt + 4, dh);
            wr32(reply, FOURCC('n','u','l','l'));
            wr32(reply + 4, 0);
            LOG_I("AppleEvent %s/%s -> handler", fourcc_str(cls), fourcc_str(id));
            u32 a[3] = { evt, reply, g_ae[i].refcon };
            u32 r = call_upp(g_ae[i].handler, 3, a);
            (void)r;
            RETERR(noErr);
            return;
        }
    }
    RETERR(errAEEventNotHandled);
}

TRAP(AEGetParamDesc) { u32 d = ARG(3); if (d) { wr32(d, FOURCC('n','u','l','l')); wr32(d + 4, 0); } RETERR(-1701); }
TRAP(AEGetParamPtr) { RETERR(-1701); }
TRAP(AEGetAttributePtr) { RETERR(-1701); }
TRAP(AECountItems) { u32 p = ARG(1); if (p) wr32(p, 0); RETERR(noErr); }
TRAP(AEGetNthPtr) { RETERR(-1701); }
TRAP(AEGetNthDesc) { RETERR(-1701); }
TRAP(AESizeOfNthItem) { RETERR(-1701); }
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
    if (d) { wr32(d, FOURCC('n','u','l','l')); wr32(d + 4, 0); }
    RETERR(noErr);
}
