/* Miscellaneous OS services: Gestalt, Process Manager, ticks, Time
 * Manager, queues, low-memory accessors, string utilities, and stubs for
 * subsystems that are absent in the emulated machine. */
#include "os.h"
#include "mm.h"
#include "files.h"
#include "misc.h"
#include "../loader/pef.h"
#include "../host/host.h"
#include <time.h>
#include <math.h>

/* ---- low-memory globals ---- */
#define LM_Ticks       0x016A
#define LM_Time        0x020C
#define LM_DoubleTime  0x02F0
#define LM_CaretTime   0x02F4
#define LM_HiliteMode  0x0938
#define LM_MenuHook    0x0A30
#define LM_WindowList  0x09D6
#define LM_PaintWhite  0x09DC
#define LM_LastSPExtra 0x0B4C
#define LM_SysFontFam  0x0BA6
#define LM_SysFontSize 0x0BA8
#define LM_MBarHeight  0x0BAA
#define LM_CurApName   0x0910

static u64 g_start_ns;

int g_turbo = 1; /* test option: emulated clock runs this many times faster */
static u64 now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ((u64)ts.tv_sec * 1000000000ull + (u64)ts.tv_nsec) * (u64)g_turbo;
}
u64 host_now_us(void) { return (now_ns() - g_start_ns) / 1000; }

u32 tick_count(void) {
    /* 60.15 Hz */
    return (u32)((now_ns() - g_start_ns) * 6015ull / 100000000000ull);
}

/* ---- deferred "interrupt" callbacks ---- */
typedef struct { u32 upp; u32 arg; bool used; } Interrupt;
#define MAX_IRQ 64
static Interrupt g_irq[MAX_IRQ];
static volatile int g_irq_pending;

void irq_post(u32 upp, u32 arg) {
    for (int i = 0; i < MAX_IRQ; i++) {
        if (!g_irq[i].used) { g_irq[i] = (Interrupt){ upp, arg, true }; g_irq_pending = 1; return; }
    }
    LOG_W("interrupt queue full");
}

/* Run a guest callback preserving the complete CPU state. */
void guest_call_async(u32 upp, int n, const u32 *args) {
    CPU save = *g_cpu;
    call_upp(upp, n, args);
    int depth = g_cpu->depth;
    *g_cpu = save;
    g_cpu->depth = depth;
}

/* ---- Time Manager ---- */
typedef struct { u32 task; u64 due_us; bool active; } TMEntry;
#define MAX_TM 32
static TMEntry g_tm[MAX_TM];
static bool g_in_irq;

static TMEntry *tm_find(u32 task, bool create) {
    for (int i = 0; i < MAX_TM; i++) if (g_tm[i].task == task) return &g_tm[i];
    if (!create) return NULL;
    for (int i = 0; i < MAX_TM; i++) if (!g_tm[i].task) { g_tm[i].task = task; return &g_tm[i]; }
    return NULL;
}

void irq_service(void) {
    if (g_in_irq) return;
    g_in_irq = true;
    u64 now = host_now_us();
    for (int i = 0; i < MAX_TM; i++) {
        TMEntry *e = &g_tm[i];
        if (e->task && e->active && now >= e->due_us) {
            e->active = false;
            u32 task = e->task;
            wr16(task + 4, rd16(task + 4) & 0x7FFF); /* clear active bit in qType */
            u32 upp = rd32(task + 6);
            if (upp) { u32 a[1] = { task }; guest_call_async(upp, 1, a); }
        }
    }
    if (g_irq_pending) {
        g_irq_pending = 0;
        for (int i = 0; i < MAX_IRQ; i++) {
            if (g_irq[i].used) {
                Interrupt q = g_irq[i];
                g_irq[i].used = false;
                u32 a[1] = { q.arg };
                guest_call_async(q.upp, 1, a);
            }
        }
    }
    g_in_irq = false;
}

TRAP(InsTime) { tm_find(ARG(0), true); }
TRAP(InsXTime) { tm_find(ARG(0), true); }
TRAP(RmvTime) {
    TMEntry *e = tm_find(ARG(0), false);
    if (e) { e->task = 0; e->active = false; }
}
TRAP(PrimeTime) {
    u32 task = ARG(0); s32 count = (s32)ARG(1);
    TMEntry *e = tm_find(task, true);
    if (!e) return;
    u64 delay = count >= 0 ? (u64)count * 1000 : (u64)(-(s64)count);
    e->due_us = host_now_us() + delay;
    e->active = true;
    wr16(task + 4, rd16(task + 4) | 0x8000);
}

/* ---- ticks, delays ---- */
TRAP(TickCount) { RET(tick_count()); }

TRAP(Delay) {
    u32 n = ARG(0), finalp = ARG(1);
    u32 until = tick_count() + n;
    while ((s32)(until - tick_count()) > 0) {
        host_pump(true);
        irq_service();
    }
    if (finalp) wr32(finalp, tick_count());
}

/* ---- Gestalt ---- */
TRAP(Gestalt) {
    u32 sel = ARG(0), resp = ARG(1);
    u32 v;
    bool ok = true;
    switch (sel) {
    case FOURCC('s','y','s','v'): v = 0x0761; break;
    case FOURCC('q','d',' ',' '): v = 0x0230; break;  /* 32-bit Color QuickDraw */
    case FOURCC('s','y','s','a'): v = 2; break;       /* PowerPC */
    case FOURCC('c','f','r','g'): v = 1; break;       /* CFM present */
    case FOURCC('t','h','d','s'): v = 0x7; break;     /* threads present, in ROM-ish */
    case FOURCC('f','o','l','d'): v = 1; break;
    case FOURCC('a','l','i','s'): v = 1; break;
    case FOURCC('f','s',' ',' '): v = 0x2; break;     /* FSSpec calls */
    case FOURCC('t','m','g','r'): v = 3; break;       /* extended Time Manager */
    case FOURCC('p','r','o','c'): v = 5; break;       /* 68040 (for 68k-minded checks) */
    case FOURCC('c','p','u',' '): v = 0x0108; break;  /* PPC 750 */
    case FOURCC('f','p','u',' '): v = 0; break;
    case FOURCC('m','m','u',' '): v = 0; break;
    case FOURCC('r','a','m',' '): v = 128u << 20; break;
    case FOURCC('l','r','a','m'): v = 128u << 20; break;
    case FOURCC('s','n','d',' '): v = 0x1FFF; break;  /* stereo, 16-bit, etc */
    case FOURCC('a','t','t','r'): v = 0x0003; break;
    case FOURCC('k','b','d',' '): v = 2; break;       /* extended keyboard */
    case FOURCC('m','a','c','h'): v = 406; break;     /* Power Mac G3 */
    case FOURCC('o','s','a','t'): v = 0x1F; break;
    case FOURCC('t','e',' ',' '): v = 5; break;       /* TextEdit version */
    case FOURCC('d','i','t','l'): v = 1; break;       /* Dialog Manager extensions */
    case FOURCC('m','e','n','u'): v = 0; break;
    case FOURCC('p','o','w','r'): v = 0; break;
    case FOURCC('v','m',' ',' '): v = 0; break;
    case FOURCC('h','e','l','p'): v = 1; break;
    case FOURCC('p','m','g','r'): v = 0; break;
    case FOURCC('s','c','r','i'): v = 0; break;
    case FOURCC('d','p','l','y'): v = 0; ok = false; break;
    case FOURCC('q','t','i','m'): v = 0x03008000; break; /* QuickTime 3.0 */
    case FOURCC('e','v','n','t'): v = 1; break;       /* Apple Events present */
    case FOURCC('c','p','n','t'): v = 0x00030000; break; /* Component Manager */
    case FOURCC('q','t','r','s'): v = 1; break;
    default: ok = false; break;
    }
    if (!ok) {
        LOG_D("Gestalt('%s') undefined", fourcc_str(sel));
        RETERR(gestaltUndefSelectorErr);
        return;
    }
    if (resp) wr32(resp, v);
    RETERR(noErr);
}

/* ---- Process Manager ---- */
TRAP(GetCurrentProcess) { u32 p = ARG(0); wr32(p, 0); wr32(p + 4, 2); RETERR(noErr); }

TRAP(GetProcessInformation) {
    u32 info = ARG(1);
    u32 namep = rd32(info + 4), specp = rd32(info + 56);
    if (namep) c_to_pstr("Cythera", namep, 31);
    wr32(info + 8, 0); wr32(info + 12, 2);
    wr32(info + 16, FOURCC('A','P','P','L'));
    wr32(info + 20, FOURCC('D','e','l','v'));
    wr32(info + 24, 0x5AC0);
    wr32(info + 28, APPZONE_START);
    wr32(info + 32, APPZONE_END - APPZONE_START);
    wr32(info + 36, mm_free_bytes(ZONE_APP));
    wr32(info + 40, 0); wr32(info + 44, 0);
    wr32(info + 48, 0); wr32(info + 52, tick_count());
    if (specp) fsspec_write(specp, VOL_REFNUM, ROOT_DIRID, "Cythera");
    RETERR(noErr);
}

TRAP(LaunchApplication) { RETERR(-600); /* procNotFound */ }

TRAP(ExitToShell) {
    LOG_I("ExitToShell");
    host_shutdown();
    exit(0);
}

TRAP(DebugStr) {
    char s[256];
    pstr_to_c(ARG(0), s, sizeof s);
    LOG_W("DebugStr: %s", s);
}
TRAP(Debugger) { LOG_W("Debugger()"); }

TRAP(SystemTask) { host_pump(false); irq_service(); }

/* ---- CFM ---- */
TRAP(GetSharedLibrary) {
    char name[64];
    pstr_to_c(ARG(0), name, sizeof name);
    LOG_I("GetSharedLibrary(%s) -> not found", name);
    u32 errp = ARG(6);
    if (errp) c_to_pstr("not available", errp, 255);
    RETERR(-2804); /* cfragNoLibraryErr */
}
TRAP(FindSymbol) {
    char name[256];
    pstr_to_c(ARG(1), name, sizeof name);
    LOG_D("FindSymbol(%s) -> not found", name);
    RETERR(-2802); /* cfragNoSymbolErr */
}

/* ---- trap address queries: every trap is "implemented" ---- */
TRAP(NGetTrapAddress) { u16 t = ARGU16(0); RET(t == 0xA89F ? 0x00050000u : 0x00060000u + t); }
TRAP(GetToolboxTrapAddress) { u16 t = ARGU16(0); RET(t == 0xA89F ? 0x00050000u : 0x00060000u + t); }

/* ---- A5 world (no-ops on PPC) ---- */
TRAP(SetCurrentA5) { RET(0); }
TRAP(SetA5) { RET(0); }

/* ---- queues ---- */
TRAP(Enqueue) {
    u32 e = ARG(0), q = ARG(1);
    wr32(e, 0);
    u32 tail = rd32(q + 6);
    if (tail) wr32(tail, e); else wr32(q + 2, e);
    wr32(q + 6, e);
}
TRAP(Dequeue) {
    u32 e = ARG(0), q = ARG(1);
    u32 prev = 0, cur = rd32(q + 2);
    while (cur && cur != e) { prev = cur; cur = rd32(cur); }
    if (!cur) { RETERR(-1); return; } /* qErr */
    u32 next = rd32(cur);
    if (prev) wr32(prev, next); else wr32(q + 2, next);
    if (rd32(q + 6) == e) wr32(q + 6, prev);
    RETERR(noErr);
}

/* ---- low-memory accessors ---- */
TRAP(LMGetDoubleTime) { RET(rd32(LM_DoubleTime)); }
TRAP(LMGetHiliteMode) { RET(rd8(LM_HiliteMode)); }
TRAP(LMSetHiliteMode) { wr8(LM_HiliteMode, (u8)ARG(0)); }
TRAP(LMGetMenuHook) { RET(rd32(LM_MenuHook)); }
TRAP(LMSetMenuHook) { wr32(LM_MenuHook, ARG(0)); }
TRAP(LMGetSysFontFam) { RET(rd16(LM_SysFontFam)); }
TRAP(LMSetSysFontFam) { wr16(LM_SysFontFam, (u16)ARG(0)); }
TRAP(LMGetSysFontSize) { RET(rd16(LM_SysFontSize)); }
TRAP(LMSetSysFontSize) { wr16(LM_SysFontSize, (u16)ARG(0)); }
TRAP(LMGetWindowList) { RET(rd32(LM_WindowList)); }
TRAP(LMSetLastSPExtra) { wr32(LM_LastSPExtra, ARG(0)); }
TRAP(LMSetMBarHeight) { wr16(LM_MBarHeight, (u16)ARG(0)); }
TRAP(LMSetPaintWhite) { wr16(LM_PaintWhite, (u16)ARG(0)); }

/* ---- string utilities ---- */
static u8 upper_mac(u8 c, bool diac) {
    if (c >= 'a' && c <= 'z') return (u8)(c - 32);
    (void)diac;
    return c;
}

TRAP(EqualString) {
    u32 a = ARG(0), b = ARG(1); bool cs = ARGB(2), ds = ARGB(3);
    (void)ds;
    u8 la = rd8(a), lb = rd8(b);
    if (la != lb) { RET(0); return; }
    for (u32 i = 1; i <= la; i++) {
        u8 x = rd8(a + i), y = rd8(b + i);
        if (!cs) { x = upper_mac(x, ds); y = upper_mac(y, ds); }
        if (x != y) { RET(0); return; }
    }
    RET(1);
}

TRAP(UpperString) {
    u32 s = ARG(0); bool ds = ARGB(1);
    u8 n = rd8(s);
    for (u32 i = 1; i <= n; i++) wr8(s + i, upper_mac(rd8(s + i), ds));
}

TRAP(c2pstr) {
    u32 s = ARG(0);
    size_t n = gstrlen(s);
    if (n > 255) n = 255;
    gmemmove(s + 1, s, (u32)n);
    wr8(s, (u8)n);
    RET(s);
}

TRAP(NumToString) {
    s32 n = (s32)ARG(0); u32 out = ARG(1);
    char buf[16];
    snprintf(buf, sizeof buf, "%d", n);
    c_to_pstr(buf, out, 255);
}

TRAP(StringToNum) {
    char buf[256];
    pstr_to_c(ARG(0), buf, sizeof buf);
    wr32(ARG(1), (u32)(s32)strtol(buf, NULL, 10));
}

/* num2dec(const decform *f, double x, decimal *d): x occupies f1 and the
   r4/r5 slots, so d arrives in r6. */
TRAP(num2dec) {
    u32 form = ARG(0), dec = ARG(3);
    double x = cpu->f[1].d;
    u8 style = rd8(form);
    s16 digits = rds16(form + 2);
    wr8(dec, x < 0 || (x == 0 && signbit(x)));
    wr8(dec + 1, 0);
    double ax = fabs(x);
    char buf[64];
    if (isnan(ax)) { wr16(dec + 2, 0); c_to_pstr("N", dec + 4, 36); return; }
    if (isinf(ax)) { wr16(dec + 2, 0); c_to_pstr("I", dec + 4, 36); return; }
    if (ax == 0) { wr16(dec + 2, 0); c_to_pstr("0", dec + 4, 36); return; }
    if (style == 1) { /* FIXEDDECIMAL: digits after the point */
        if (digits < 0) digits = 0;
        if (digits > 18) digits = 18;
        snprintf(buf, sizeof buf, "%.*f", digits, ax);
        char sig[64]; int k = 0; bool lead = true;
        for (char *p = buf; *p; p++) {
            if (*p == '.') continue;
            if (lead && *p == '0' && p[1] && p[1] != '.') continue;
            lead = false;
            sig[k++] = *p;
        }
        sig[k] = 0;
        if (!k) { strcpy(sig, "0"); }
        wr16(dec + 2, (u16)(s16)-digits);
        c_to_pstr(sig, dec + 4, 36);
    } else { /* FLOATDECIMAL: `digits` significant digits */
        if (digits < 1) digits = 1;
        if (digits > 19) digits = 19;
        snprintf(buf, sizeof buf, "%.*e", digits - 1, ax);
        char *e = strchr(buf, 'e');
        int ex = atoi(e + 1);
        *e = 0;
        char sig[64]; int k = 0;
        for (char *p = buf; *p; p++) if (*p != '.') sig[k++] = *p;
        sig[k] = 0;
        wr16(dec + 2, (u16)(s16)(ex - (digits - 1)));
        c_to_pstr(sig, dec + 4, 36);
    }
}

TRAP(FixMul) {
    s64 r = (s64)(s32)ARG(0) * (s32)ARG(1);
    r = (r + 0x8000) >> 16;
    if (r > 0x7FFFFFFF) r = 0x7FFFFFFF;
    if (r < -0x7FFFFFFFLL - 1) r = -0x7FFFFFFFLL - 1;
    RET((u32)(s32)r);
}
TRAP(FixDiv) {
    s32 a = (s32)ARG(0), b = (s32)ARG(1);
    if (b == 0) { RET(a >= 0 ? 0x7FFFFFFF : 0x80000000); return; }
    s64 r = ((s64)a << 16) / b;
    if (r > 0x7FFFFFFF) r = 0x7FFFFFFF;
    if (r < -0x7FFFFFFFLL - 1) r = -0x7FFFFFFFLL - 1;
    RET((u32)(s32)r);
}
TRAP(BitAnd) { RET(ARG(0) & ARG(1)); }

/* ---- absent subsystems ---- */
TRAP(NMRemove) { RETERR(noErr); }
TRAP(NMInstall) { RETERR(noErr); }
TRAP(HMShowBalloon) { RETERR(-850); /* hmBalloonAborted */ }
TRAP(HMRemoveBalloon) { RETERR(noErr); }
TRAP(CrsrDevNextDevice) { u32 p = ARG(0); if (p) wr32(p, 0); RETERR(-1); }
TRAP(CrsrDevNewDevice) { RETERR(-1); }
TRAP(CrsrDevDisposeDevice) { RETERR(-1); }
TRAP(CrsrDevButtonDown) { RETERR(-1); }
TRAP(CrsrDevButtonUp) { RETERR(-1); }
TRAP(CrsrDevUnitsPerInch) { RETERR(-1); }
TRAP(OpenDeskAcc) { RETERR(0); }
TRAP(SystemEdit) { RET(0); }
TRAP(SystemClick) { }
TRAP(GetDCtlEntry) { RET(0); }
TRAP(SndSoundManagerVersion) { RET(0x03200000); }

void misc_init(void) {
    g_start_ns = now_ns();
    wr32(LM_DoubleTime, 30);
    wr32(LM_CaretTime, 32);
    wr16(LM_SysFontFam, 0);
    wr16(LM_SysFontSize, 12);
    wr16(LM_MBarHeight, 20);
    c_to_pstr("Cythera", LM_CurApName, 31);
}

void misc_poll(void) {
    wr32(LM_Ticks, tick_count());
    wr32(LM_Time, mac_time_now());
}
