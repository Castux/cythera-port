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
} TrapSlot;

static TrapSlot g_slots[TRAP_COUNT];
static u32 g_nslots;

/* Libraries we emulate.  Weak imports from any other library resolve to
   NULL, so the application takes its "library not installed" path. */
static const char *g_provided_libs[] = {
    "InterfaceLib", "ThreadsLib", "SoundLib", "MathLib", NULL
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

void trap_dispatch(CPU *c, u32 index) {
    if (index >= g_nslots) fatal("jump to invalid trap slot %u", index);
    TrapSlot *s = &g_slots[index];
    s->calls++;
    if (g_trace_traps) {
        u32 off;
        const char *n = sym_lookup(c->lr, &off);
        LOG_I("trap %s(%08x %08x %08x %08x) from %s+%#x", s->name, c->r[3], c->r[4], c->r[5], c->r[6],
              n ? n : "?", n ? off : 0);
    }
    if (s->fn) {
        s->fn(c);
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
    cpu_run(c);
    u32 ret = c->r[3];
    c->r[1] = save_r1; c->r[2] = save_r2; c->lr = save_lr; c->pc = save_pc; c->ctr = save_ctr;
    return ret;
}

u32 call_upp(u32 upp, int nargs, const u32 *args) {
    if (!upp) fatal("call_upp: NULL");
    if (rd16(upp) == 0xAAFE) {
        /* RoutineDescriptor: first routine record's procDescriptor */
        u32 proc = rd32(upp + 20);
        u8 isa = rd8(upp + 17);
        if ((isa & 0x0F) != 1) fatal("call_upp: non-PPC routine descriptor (ISA %d)", isa);
        return guest_call(proc, nargs, args);
    }
    return guest_call(upp, nargs, args);
}
