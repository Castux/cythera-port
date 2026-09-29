/* Thread Manager (cooperative threads).
 *
 * Guest threads other than the application thread run as stackful
 * coroutines (minicoro) on the single host thread, each with its own CPU
 * context and guest stack. The application thread's context acts as the
 * dispatcher: a coroutine that switches to another thread records the
 * target and yields back to it, and the dispatcher resumes the target.
 * Switching therefore works even from inside nested host->guest calls.
 */
#include "os.h"
#include "mm.h"
#include "threads.h"
#define MINICORO_IMPL
#define MCO_DEFAULT_STACK_SIZE (2 * 1024 * 1024)
#include "../../third_party/minicoro.h"

enum { kReadyThreadState = 0, kStoppedThreadState = 1, kRunningThreadState = 2 };
enum { kNoThreadID = 0, kCurrentThreadID = 1, kApplicationThreadID = 2 };
enum { kNewSuspend = 1 << 0 };

typedef struct GThread {
    u32 id;
    bool used;
    int state;
    bool finished;
    CPU cpu;
    CPU *cpup;          /* CPU context in use (main thread: the host's) */
    mco_coro *co;
    u32 entry, param, result_ptr;
    u32 stack_base, stack_size;
} GThread;

#define MAX_THREADS 64
static GThread g_threads[MAX_THREADS];
static GThread *g_main;
static GThread *g_req;          /* switch target requested by a coroutine */
static u32 g_current = kApplicationThreadID;
static u32 g_next_id = 3;
static u32 g_scheduler;         /* guest ThreadSchedulerUPP */
static u32 g_sched_info;
static u32 g_stack_top = STACK_AREA_END - 0x00200000; /* below the main stack */

static GThread *thr(u32 id) {
    for (int i = 0; i < MAX_THREADS; i++) if (g_threads[i].used && g_threads[i].id == id) return &g_threads[i];
    return NULL;
}

void threads_init(CPU *main_cpu) {
    g_main = &g_threads[0];
    g_main->used = true;
    g_main->id = kApplicationThreadID;
    g_main->state = kRunningThreadState;
    g_main->cpup = main_cpu;
    main_cpu->thread = g_main;
}

u32 thread_current_id(void) { return g_current; }

static void become(GThread *t) {
    g_current = t->id;
    t->state = kRunningThreadState;
    g_cpu = t->cpup;
}

static GThread *next_ready(GThread *me) {
    int start = (int)(me - g_threads);
    for (int k = 1; k <= MAX_THREADS; k++) {
        GThread *t = &g_threads[(start + k) % MAX_THREADS];
        if (t->used && !t->finished && t->state == kReadyThreadState) return t;
    }
    return NULL;
}

static void reap(GThread *t) {
    if (t->co) { mco_destroy(t->co); t->co = NULL; }
    t->used = false;
}

/* Transfer control from `me` to `to` and return when `me` runs again. */
static void switch_to(GThread *me, GThread *to) {
    if (to == me || !to) return;
    if (me->state == kRunningThreadState) me->state = kReadyThreadState;
    if (me != g_main) {
        g_req = to;
        mco_yield(me->co);
        become(me);
        return;
    }
    /* dispatcher: run coroutines until one hands control back to main */
    GThread *t = to;
    while (t && t != g_main) {
        become(t);
        g_req = NULL;
        mco_resume(t->co);
        GThread *n = g_req;
        if (t->finished) reap(t);
        if (!n) n = g_main;
        t = n;
    }
    become(g_main);
}

static GThread *consult_scheduler(GThread *me, GThread *suggested) {
    if (!g_scheduler) return suggested;
    if (!g_sched_info) g_sched_info = sys_alloc(16);
    u32 info = g_sched_info;
    wr32(info, 16);
    wr32(info + 4, me->id);
    wr32(info + 8, suggested ? suggested->id : kNoThreadID);
    wr32(info + 12, kNoThreadID);
    u32 a[1] = { info };
    u32 r = call_upp(g_scheduler, 1, a);
    if (r == kNoThreadID) return suggested;
    GThread *t = thr(r);
    if (t && !t->finished && (t->state == kReadyThreadState || t == me)) return t;
    return suggested;
}

static void co_entry(mco_coro *co) {
    GThread *t = mco_get_user_data(co);
    CPU *c = &t->cpu;
    memset(c, 0, sizeof *c);
    c->thread = t;
    g_cpu = c;
    u32 sp = (t->stack_base + t->stack_size - 64) & ~15u;
    wr32(sp, 0);
    c->r[1] = sp;
    c->r[3] = t->param;
    u32 tv = t->entry;
    if (rd16(tv) == 0xAAFE) tv = rd32(tv + 20);   /* ThreadEntryUPP */
    c->r[2] = rd32(tv + 4);
    c->pc = rd32(tv);
    c->lr = RET_SENTINEL;
    cpu_run(c);
    if (t->result_ptr) wr32(t->result_ptr, c->r[3]);
    t->finished = true;
    t->state = kStoppedThreadState;
    GThread *n = next_ready(t);
    g_req = n ? n : g_main;
}

void thread_yield(void) {
    GThread *me = ((CPU *)g_cpu)->thread;
    GThread *n = next_ready(me);
    GThread *choice = consult_scheduler(me, n);
    if (choice && choice != me) switch_to(me, choice);
}

TRAP(NewThread) {
    /* NewThread(style, entry, param, stackSize, options, result*, threadID*) */
    u32 entry = ARG(1), param = ARG(2), stack = ARG(3), opts = ARG(4), resp = ARG(5), idp = ARG(6);
    GThread *t = NULL;
    for (int i = 1; i < MAX_THREADS; i++) if (!g_threads[i].used) { t = &g_threads[i]; break; }
    if (!t) { RETERR(memFullErr); return; }
    memset(t, 0, sizeof *t);
    t->used = true;
    t->id = g_next_id++;
    t->entry = entry; t->param = param; t->result_ptr = resp;
    if (stack < 0x10000) stack = 0x10000;
    stack = (stack + 0xFFF) & ~0xFFFu;
    if (g_stack_top - stack < STACK_AREA_START) { t->used = false; RETERR(memFullErr); return; }
    g_stack_top -= stack;
    t->stack_base = g_stack_top;
    t->stack_size = stack;
    t->state = (opts & kNewSuspend) ? kStoppedThreadState : kReadyThreadState;
    t->cpup = &t->cpu;
    mco_desc desc = mco_desc_init(co_entry, 0);
    desc.user_data = t;
    if (mco_create(&t->co, &desc) != MCO_SUCCESS) fatal("cannot create coroutine");
    if (idp) wr32(idp, t->id);
    LOG_D("NewThread id %u entry %08x param %08x stack %u opts %x", t->id, entry, param, stack, opts);
    RETERR(noErr);
}

TRAP(DisposeThread) {
    u32 id = ARG(0);
    GThread *t = thr(id);
    if (!t || t == g_main) { RETERR(-617); return; }
    GThread *me = ((CPU *)g_cpu)->thread;
    if (t == me) {
        /* disposing ourselves: finish and hand control away */
        t->finished = true;
        t->state = kStoppedThreadState;
        GThread *n = next_ready(t);
        g_req = n ? n : g_main;
        mco_yield(t->co);
        return; /* not reached */
    }
    t->finished = true;
    reap(t);
    RETERR(noErr);
}

TRAP(YieldToAnyThread) { thread_yield(); RETERR(noErr); }

TRAP(YieldToThread) {
    u32 id = ARG(0);
    GThread *me = ((CPU *)g_cpu)->thread;
    GThread *t = thr(id);
    if (!t || t->finished) { RETERR(-617 /* threadNotFoundErr */); return; }
    if (t->state == kReadyThreadState) switch_to(me, t);
    RETERR(noErr);
}

TRAP(GetCurrentThread) { u32 p = ARG(0); if (p) wr32(p, g_current); RETERR(noErr); }

TRAP(GetThreadState) {
    u32 id = ARG(0), sp = ARG(1);
    if (id == kCurrentThreadID) id = g_current;
    GThread *t = thr(id);
    if (!t) { RETERR(-617); return; }
    if (sp) wr16(sp, (u16)t->state);
    RETERR(noErr);
}

TRAP(SetThreadState) {
    u32 id = ARG(0); u16 st = ARGU16(1); u32 sugg = ARG(2);
    GThread *me = ((CPU *)g_cpu)->thread;
    if (id == kCurrentThreadID) id = g_current;
    GThread *t = thr(id);
    if (!t || t->finished) { RETERR(-617); return; }
    if (t == me) {
        if (st == kStoppedThreadState) {
            GThread *n = sugg ? thr(sugg) : NULL;
            if (!n || n->state != kReadyThreadState) n = next_ready(me);
            me->state = kStoppedThreadState;
            n = consult_scheduler(me, n);
            if (!n || n == me) { me->state = kRunningThreadState; RETERR(noErr); return; }
            me->state = kStoppedThreadState;
            switch_to(me, n);
        }
        RETERR(noErr);
        return;
    }
    if (st == kReadyThreadState || st == kStoppedThreadState) t->state = st;
    else if (st == kRunningThreadState) { t->state = kReadyThreadState; switch_to(me, t); }
    RETERR(noErr);
}

TRAP(SetThreadScheduler) { g_scheduler = ARG(0); RETERR(noErr); }

void threads_debug_dump(void) {
    extern void cpu_backtrace(CPU *c, FILE *f);
    fprintf(stderr, "current thread %u\n", g_current);
    for (int i = 0; i < MAX_THREADS; i++) {
        GThread *t = &g_threads[i];
        if (!t->used) continue;
        fprintf(stderr, "thread %u state %d%s entry %08x\n", t->id, t->state, t->finished ? " finished" : "", t->entry);
        if (t->id != g_current) cpu_backtrace(t->cpup, stderr);
    }
}
