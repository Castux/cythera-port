/* Trap dispatch: binds PEF imports to native handlers, and lets native
 * code call back into guest code. */
#include "os.h"
#include "../loader/pef.h"

bool g_trace_traps;
bool g_strict_traps;

typedef struct {
    const char *name;
    void (*fn)(CPU *);
    u32 calls;
    u64 ns;       /* inclusive host time (--profile) */
} TrapSlot;
bool g_profile;
#include <time.h>
static u64 prof_now(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return (u64)ts.tv_sec * 1000000000ull + (u64)ts.tv_nsec; }
static u64 g_prof_start;
static TrapSlot *g_prof_stack[64];
static u64 g_prof_t0[64];
static int g_prof_sp;
u64 g_prof_guest_ns;

static TrapSlot g_slots[TRAP_COUNT];
static u32 g_nslots;
static int prof_cmp(const void *a, const void *b) {
    const TrapSlot *x = *(TrapSlot *const *)a, *y = *(TrapSlot *const *)b;
    return x->ns < y->ns ? 1 : x->ns > y->ns ? -1 : 0;
}
void trap_profile_report(void) {
    if (!g_profile) return;
    static TrapSlot *v[TRAP_COUNT];
    int n = 0;
    for (u32 i = 0; i < TRAP_COUNT; i++) if (g_slots[i].calls) v[n++] = &g_slots[i];
    qsort(v, (size_t)n, sizeof v[0], prof_cmp);
    u64 total = prof_now() - g_prof_start;
    u64 tt = 0;
    for (int i = 0; i < n; i++) tt += v[i]->ns;
    fprintf(stderr, "profile: %.2f s wall, %.2f s in Toolbox (exclusive), rest in the interpreter\n", total / 1e9, tt / 1e9);
    extern void guest_profile_report(void);
    guest_profile_report();
    for (int i = 0; i < n && i < 30; i++)
        fprintf(stderr, "  %-28s %8u calls %8.3f s %5.1f%%\n", v[i]->name, v[i]->calls, v[i]->ns / 1e9, 100.0 * (double)v[i]->ns / (double)total);
}

/* Libraries we emulate.  Weak imports from any other library resolve to
   NULL, so the application takes its "library not installed" path. */
static const char *g_provided_libs[] = {
    "InterfaceLib", "ThreadsLib", "SoundLib", "MathLib", "QuickTimeLib", NULL
};

static const TrapDef *find_def(const char *name) {
    for (int i = 0; i < g_trap_ndefs; i++)
        if (!strcmp(g_trap_defs[i].name, name)) return &g_trap_defs[i];
    return NULL;
}

static u32 new_slot(const char *name, void (*fn)(CPU *)) {
    if (g_nslots >= TRAP_COUNT - 1) fatal("out of trap slots");
    u32 k = g_nslots++;
    g_slots[k].name = strdup(name);
    g_slots[k].fn = fn;
    u32 tv = TVEC_BASE + 8 * k;
    wr32(tv, TRAP_BASE + 4 * k);
    wr32(tv + 4, 0);
    wr32(TRAP_BASE + 4 * k, 0x4E800020); /* blr, for disassembly readability */
    return tv;
}

u32 trap_native_tvector(const char *name, void (*fn)(CPU *)) {
    return new_slot(name, fn);
}

u32 trap_resolve_import(const char *lib, const char *name, bool weak) {
    bool provided = false;
    for (int i = 0; g_provided_libs[i]; i++)
        if (!strcmp(lib, g_provided_libs[i])) provided = true;
    const TrapDef *d = find_def(name);
    if (!provided && weak && !d) return 0;
    if (!provided && weak) return 0;
    return new_slot(name, d ? d->fn : NULL);
}

const char *trap_name(u32 index) {
    return index < g_nslots ? g_slots[index].name : "?";
}

void cpu_backtrace(CPU *c, FILE *f) {
    u32 off;
    const char *n = sym_lookup(c->pc, &off);
    fprintf(f, "  pc %08x %s+%#x\n", c->pc, n ? n : "?", n ? off : 0);
    n = sym_lookup(c->lr, &off);
    fprintf(f, "  lr %08x %s+%#x\n", c->lr, n ? n : "?", n ? off : 0);
    u32 sp = c->r[1];
    for (int i = 0; i < 40 && sp && sp < GUEST_MEM_SIZE - 12; i++) {
        u32 back = rd32(sp);
        if (!back || back <= sp || back >= GUEST_MEM_SIZE - 12) break;
        u32 ret = rd32(back + 8);
        n = sym_lookup(ret, &off);
        fprintf(f, "  #%-2d %08x %s+%#x\n", i, ret, n ? n : "?", n ? off : 0);
        sp = back;
    }
}

u64 g_last_trap_icount;
u32 g_trap_epoch = 1;
void trap_dispatch(CPU *c, u32 index) {
    g_trap_epoch++;
    if (index >= g_nslots) fatal("jump to invalid trap slot %u", index);
    c->last_trap_icount = c->icount;
    TrapSlot *s = &g_slots[index];
    s->calls++;
    if (g_trace_traps) {
        u32 off;
        const char *n = sym_lookup(c->lr, &off);
        LOG_I("trap %s(%08x %08x %08x %08x) from %s+%#x", s->name, c->r[3], c->r[4], c->r[5], c->r[6],
              n ? n : "?", n ? off : 0);
    }
    if (s->fn) {
        if (g_profile && g_prof_sp < 64) {
            u64 now = prof_now();
            if (!g_prof_start) g_prof_start = now;
            if (g_prof_sp > 0) g_prof_stack[g_prof_sp - 1]->ns += now - g_prof_t0[g_prof_sp - 1];
            g_prof_stack[g_prof_sp] = s;
            g_prof_t0[g_prof_sp] = now;
            g_prof_sp++;
            s->fn(c);
            g_prof_sp--;
            now = prof_now();
            s->ns += now - g_prof_t0[g_prof_sp];
            if (g_prof_sp > 0) g_prof_t0[g_prof_sp - 1] = now;
        } else s->fn(c);
        return;
    }
    if (s->calls <= 3) {
        LOG_W("unimplemented trap %s (call #%u)", s->name, s->calls);
        if (g_log_level >= 2) cpu_backtrace(c, stderr);
    }
    if (g_strict_traps) fatal("unimplemented trap %s", s->name);
    c->r[3] = 0;
}

u32 guest_call(u32 tvector, int nargs, const u32 *args) {
    CPU *c = g_cpu;
    if (!tvector) fatal("guest_call: NULL tvector");
    u32 save_lr = c->lr, save_pc = c->pc, save_r1 = c->r[1], save_r2 = c->r[2];
    u32 save_ctr = c->ctr;
    u32 sp = (c->r[1] - 512 - 4 * (u32)(nargs > 8 ? nargs : 8)) & ~15u;
    wr32(sp, c->r[1]);
    for (int i = 0; i < nargs; i++) {
        if (i < 8) c->r[3 + i] = args[i];
        wr32(sp + 24 + 4 * (u32)i, args[i]);
    }
    c->r[1] = sp;
    c->r[2] = rd32(tvector + 4);
    c->lr = RET_SENTINEL;
    c->pc = rd32(tvector);
    u64 pt = 0;
    if (g_profile && g_prof_sp > 0) { pt = prof_now(); g_prof_stack[g_prof_sp - 1]->ns += pt - g_prof_t0[g_prof_sp - 1]; }
    cpu_run(c);
    if (g_profile && g_prof_sp > 0) { u64 now = prof_now(); g_prof_t0[g_prof_sp - 1] = now; g_prof_guest_ns += now - pt; }
    u32 ret = c->r[3];
    c->r[1] = save_r1; c->r[2] = save_r2; c->lr = save_lr; c->pc = save_pc; c->ctr = save_ctr;
    return ret;
}

/* Known 68k code snippets reached through Mixed Mode (the emulated
   machine has no 68k CPU). Returns true and sets *ret if recognised. */
static u32 g_fake_sr = 0x2000;
static bool hle_68k(u32 code, int nargs, const u32 *args, u32 *ret) {
    u32 w0 = rd32(code), w1 = rd32(code + 4);
    if (w0 == 0x40C0007Cu && w1 == 0x07004E75u) { /* move sr,d0; ori #$700,sr; rts */
        *ret = g_fake_sr; g_fake_sr |= 0x0700; return true;
    }
    if (w0 == 0x46C04E75u) { /* move d0,sr; rts */
        g_fake_sr = nargs > 0 ? (args[0] & 0xFFFF) : 0x2000; *ret = 0; return true;
    }
    if ((w0 >> 16) == 0x4E75) { *ret = 0; return true; } /* rts */
    return false;
}

static bool is_tvector(u32 p) {
    u32 code = rd32(p);
    return code >= CODE_ADDR && code < CODE_ADDR + 0x00100000u && !(code & 3);
}

u32 call_upp(u32 upp, int nargs, const u32 *args) {
    if (!upp) fatal("call_upp: NULL");
    if (rd16(upp) != 0xAAFE && !is_tvector(upp) && !(rd32(upp) >= TRAP_BASE && rd32(upp) < TRAP_END)) {
        u32 r;
        if (hle_68k(upp, nargs, args, &r)) return r;
        LOG_W("call_upp: unknown 68k code at %08x: %08x %08x", upp, rd32(upp), rd32(upp + 4));
        return 0;
    }
    if (rd16(upp) == 0xAAFE) {
        /* RoutineDescriptor: first routine record's procDescriptor */
        u32 proc = rd32(upp + 20);
        u8 isa = rd8(upp + 17);
        if ((isa & 0x0F) != 1) fatal("call_upp: non-PPC routine descriptor (ISA %d)", isa);
        return guest_call(proc, nargs, args);
    }
    return guest_call(upp, nargs, args);
}

/* ---- Mixed Mode: routine descriptors ---- */
#include "mm.h"
TRAP(NewRoutineDescriptor) {
    u32 proc = ARG(0), info = ARG(1); u8 isa = (u8)ARG(2);
    u32 rd = mm_new_ptr(32, true, ZONE_SYS);
    wr16(rd, 0xAAFE);
    wr8(rd + 2, 7);
    wr16(rd + 10, 0);          /* routineCount - 1 */
    wr32(rd + 12, info);
    wr8(rd + 17, isa);
    wr16(rd + 18, 0);
    wr32(rd + 20, proc);
    RET(rd);
}
TRAP(NewRoutineDescriptorTrap) { trap_NewRoutineDescriptor(cpu); }
TRAP(DisposeRoutineDescriptor) { u32 rd = ARG(0); if (rd && rd16(rd) == 0xAAFE) mm_dispose_ptr(rd); }

/* CallUniversalProc(upp, procInfo, ...): count parameters from procInfo. */
TRAP(CallUniversalProc) {
    u32 upp = ARG(0), info = ARG(1);
    int n = 0;
    if ((info & 0xF) == 2 || (info & 0xF) == 7) n = 4; /* register based: pass what we have */
    else for (int i = 0; i < 13; i++) { if ((info >> (6 + 2 * i)) & 3) n = i + 1; }
    u32 a[16];
    for (int i = 0; i < n; i++) a[i] = ARG(2 + i);
    RET(call_upp(upp, n, a));
}
