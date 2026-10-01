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
static bool g_quit_sent;

/* Auto-key: like the Mac Event Manager, generate autoKey events for the last
   key pressed while it stays down, after KeyThresh ticks and then every
   KeyRepThresh ticks (host key repeats are ignored, so the rate doesn't
   depend on the host's keyboard settings). */
#define LM_KeyThresh    0x018E
#define LM_KeyRepThresh 0x0190
static bool g_rep_on;
static u8 g_rep_key;
static u32 g_rep_msg, g_rep_next;

u16 ev_modifiers(void) { return g_mods; }
Point ev_mouse_global(void) { return g_mouse; }

static u32 g_posted;
void ev_post(u16 what, u32 message, u16 mods) {
    g_posted++;
    if (g_qn == EVQ) { memmove(g_q, g_q + 1, sizeof(Ev) * (EVQ - 1)); g_qn--; }
    g_q[g_qn++] = (Ev){ what, message, tick_count(), g_mouse, mods };
}

static void post_host_events(void) {
    HostEvent he;
    while (host_next_event(&he)) {
        g_mods = he.mods;
        u32 posted = g_posted;
        switch (he.type) {
        case HEV_MOUSE_DOWN:
            g_mouse = (Point){ (s16)he.y, (s16)he.x };
            g_button = true;
            autosave_note_input();
            ev_post(mouseDown, 0, he.mods);
            break;
        case HEV_MOUSE_UP:
            g_mouse = (Point){ (s16)he.y, (s16)he.x };
            g_button = false;
            ev_post(mouseUp, 0, he.mods);
            break;
        case HEV_KEY_DOWN:
            if (he.repeat) break;
            autosave_note_input();
            ev_post(keyDown, (u32)he.mac_key << 8 | he.ch, he.mods);
            g_rep_on = true; g_rep_key = he.mac_key; g_rep_msg = (u32)he.mac_key << 8 | he.ch;
            g_rep_next = he.when + rd16(LM_KeyThresh);
            break;
        case HEV_KEY_UP:
            if (g_rep_on && he.mac_key == g_rep_key) g_rep_on = false;
            ev_post(keyUp, (u32)he.mac_key << 8 | he.ch, he.mods);
            break;
        case HEV_QUIT:
            if (!g_quit_sent) { g_quit_sent = true; ev_post(kHighLevelEvent, FOURCC('a','e','v','t'), 0);
                g_q[g_qn - 1].where = pt_from_u32(FOURCC('q','u','i','t')); }
            else { host_shutdown(); exit(0); }
            break;
        case HEV_FOCUS_IN: ev_post(osEvt, 0x01000001u, 0); break;
        case HEV_FOCUS_OUT: g_rep_on = false; ev_post(osEvt, 0x01000000u, 0); break;
        default: break;
        }
        if (g_posted != posted) g_q[g_qn - 1].when = he.when; /* when it happened */
    }
    int x, y; bool d;
    host_mouse(&x, &y, &d);
    extern void cursor_mouse_moved(void);
    if (g_mouse.h != x || g_mouse.v != y) cursor_mouse_moved();
    g_mouse = (Point){ (s16)y, (s16)x };
    if (!host_is_headless()) g_button = d;
}

void ev_pump_host(void) {
    host_pump(false);
    post_host_events();
}

bool ev_mouse_button(void) { ev_pump_host(); return g_button; }

/* Present the screen once per tick (the emulated display refreshes at
   60.15 Hz, whatever the host's refresh rate). */
void ev_present_tick(void) {
    static u32 last = ~0u;
    u32 now = tick_count();
    if (now != last) { last = now; qd_present(); }
}

/* Present the screen if due, and yield the host. */
void ev_idle_frame(void) {
    ev_present_tick();
    host_pump(true);
    post_host_events();
    irq_service();
    misc_poll();
}

static bool mask_ok(u16 mask, u16 what) {
    if (what == kHighLevelEvent) return (mask & 0x0400) != 0;
    return what < 16 && ((mask >> what) & 1);
}

/* Find and optionally remove the next event matching mask. */
static bool get_event(u16 mask, Ev *out, bool remove, bool os_only) {
    for (int i = 0; i < g_qn; i++) {
        if (!mask_ok(mask, g_q[i].what)) continue;
        if (!mask_ok(g_sysmask | (1 << activateEvt) | (1 << updateEvt) | 0x0400 | (1 << osEvt), g_q[i].what)) continue;
        /* activate events have priority */
        *out = g_q[i];
        if (remove) { memmove(&g_q[i], &g_q[i + 1], sizeof(Ev) * (size_t)(g_qn - i - 1)); g_qn--; }
        return true;
    }
    if (g_rep_on && mask_ok(mask, autoKey) && mask_ok(g_sysmask, autoKey)) {
        u8 km[16];
        host_keymap(km);
        u32 now = tick_count();
        if (!(km[g_rep_key >> 3] & (1 << (g_rep_key & 7)))) g_rep_on = false;
        else if ((s32)(now - g_rep_next) >= 0) {
            *out = (Ev){ autoKey, g_rep_msg, now, g_mouse, g_mods };
            if (remove) g_rep_next = now + (rd16(LM_KeyRepThresh) ? rd16(LM_KeyRepThresh) : 1);
            return true;
        }
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

/* A high-level event: message = event class, where = event ID (ae.c keeps
   the parameters). */
void ev_post_high_level(u32 cls, u32 id) {
    ev_post(kHighLevelEvent, cls, 0);
    g_q[g_qn - 1].where = pt_from_u32(id);
}

TRAP(WaitNextEvent) {
    u16 mask = ARGU16(0); u32 ep = ARG(1); u32 sleep = ARG(2); u32 mrgn = ARG(3);
    /* a display mode change is applied here, between the game's own event
       handling, as the Mac's Monitors control panel would */
    int rw, rh;
    if (host_take_screen_request(&rw, &rh)) { extern void wm_change_screen(int w, int h); wm_change_screen(rw, rh); }
    if (!g_oapp_sent) {
        g_oapp_sent = true;
        ev_post_high_level(FOURCC('a','e','v','t'), FOURCC('o','a','p','p'));
    }
    u32 start = tick_count();
    bool autosave = autosave_poll(cpu);
    for (;;) {
        ev_idle_frame();
        Ev e;
        if (autosave && !g_qn && !g_button && mask_ok(mask, keyDown)) { /* File > Backup: autosave.c */
            Ev b = { keyDown, 0x0B00u | 'b', tick_count(), g_mouse, 0x0100 };
            write_event(ep, &b);
            autosave_sent();
            RET(1);
            return;
        }
        autosave = false;
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
        /* no event: sleep up to `sleep` ticks, as the Mac did (the game's
           main loop runs at the rate it asked for, 20 Hz in play) */
        if (tick_count() - start >= sleep) break;
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
    if (g_trace_traps) LOG_I("  mouse local (%d,%d)", p.h, p.v);
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
TRAP(GetCaretTime) { RET(rd32(0x02F4)); }
TRAP(GetDblTime) { RET(rd32(0x02F0)); }

void ev_init(void) {
    /* factory settings of parameter RAM (SPKbd = $63): first repeat after
       24 ticks, then every 6 ticks */
    wr16(LM_KeyThresh, 24);
    wr16(LM_KeyRepThresh, 6);
}

