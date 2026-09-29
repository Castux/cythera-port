/* Thread Manager (cooperative threads).
 *
 * Every guest thread runs on its own host thread with its own CPU context
 * and guest stack. A mutex+condition "baton" ensures exactly one runs at a
 * time, so scheduling is cooperative, as on the Mac. Yielding therefore
 * works even from inside nested host->guest callbacks.
 */
#include "os.h"
#include "mm.h"
#include "threads.h"
#include <pthread.h>

enum { kReadyThreadState = 0, kStoppedThreadState = 1, kRunningThreadState = 2 };
enum { kNoThreadID = 0, kCurrentThreadID = 1, kApplicationThreadID = 2 };
enum { kNewSuspend = 1 << 0, kUsePremadeThread = 1 << 1, kCreateIfNeeded = 1 << 2,
       kFPUNotNeeded = 1 << 3, kExactMatchThread = 1 << 4 };

typedef struct GThread {
    u32 id;
    bool used;
    int state;
    bool finished;
    CPU cpu;
    pthread_t host;
    pthread_cond_t cond;
    u32 entry, param, result_ptr;
    u32 stack_base, stack_size;
} GThread;

#define MAX_THREADS 64
static GThread g_threads[MAX_THREADS];
static pthread_mutex_t g_baton = PTHREAD_MUTEX_INITIALIZER;
static u32 g_current = kApplicationThreadID;
static u32 g_next_id = 3;
static u32 g_scheduler;       /* guest ThreadSchedulerUPP */
static u32 g_stack_top = STACK_AREA_END - 0x00200000; /* below main stack */

static GThread *thr(u32 id) {
    for (int i = 0; i < MAX_THREADS; i++) if (g_threads[i].used && g_threads[i].id == id) return &g_threads[i];
    return NULL;
}

void threads_init(CPU *main_cpu) {
    GThread *t = &g_threads[0];
    t->used = true;
    t->id = kApplicationThreadID;
    t->state = kRunningThreadState;
    pthread_cond_init(&t->cond, NULL);
    t->host = pthread_self();
    main_cpu->thread = t;
    pthread_mutex_lock(&g_baton);
}

u32 thread_current_id(void) { return g_current; }

/* Hand the baton to thread `to` and wait until it comes back to us. */
static void switch_to(GThread *me, GThread *to) {
    if (to == me) return;
    if (me->state == kRunningThreadState) me->state = kReadyThreadState;
    to->state = kRunningThreadState;
    g_current = to->id;
    pthread_cond_signal(&to->cond);
    while (g_current != me->id) pthread_cond_wait(&me->cond, &g_baton);
    g_cpu = &me->cpu;
    me->state = kRunningThreadState;
}

/* Round-robin choice of the next ready thread after `me`. */
static GThread *next_ready(GThread *me) {
    int start = (int)(me - g_threads);
    for (int k = 1; k <= MAX_THREADS; k++) {
        GThread *t = &g_threads[(start + k) % MAX_THREADS];
        if (t->used && !t->finished && t->state == kReadyThreadState) return t;
    }
    return NULL;
}

static GThread *consult_scheduler(GThread *me, GThread *suggested) {
    if (!g_scheduler) return suggested;
    u32 info = sys_alloc(16);
    wr32(info, 16);
    wr32(info + 4, me->id);
    wr32(info + 8, suggested ? suggested->id : kNoThreadID);
    wr32(info + 12, kNoThreadID);
    u32 a[1] = { info };
    u32 r = call_upp(g_scheduler, 1, a);
    mm_dispose_ptr(info);
    if (r == kNoThreadID) return suggested;
    GThread *t = thr(r);
    if (t && !t->finished && (t->state == kReadyThreadState || t == me)) return t;
    return suggested;
}

static void *thread_main(void *arg) {
    GThread *t = arg;
    pthread_mutex_lock(&g_baton);
    while (g_current != t->id) pthread_cond_wait(&t->cond, &g_baton);
    g_cpu = &t->cpu;
    CPU *c = &t->cpu;
    memset(c, 0, sizeof *c);
    c->thread = t;
    u32 sp = (t->stack_base + t->stack_size - 64) & ~15u;
    wr32(sp, 0);
    c->r[1] = sp;
    c->r[3] = t->param;
    c->r[2] = rd32(t->entry + 4);
    c->lr = RET_SENTINEL;
    /* entry may be a routine descriptor (ThreadEntryUPP) */
    u32 tv = t->entry;
    if (rd16(tv) == 0xAAFE) tv = rd32(tv + 20);
    c->r[2] = rd32(tv + 4);
    c->pc = rd32(tv);
    cpu_run(c);
    if (t->result_ptr) wr32(t->result_ptr, c->r[3]);
    t->finished = true;
    t->state = kStoppedThreadState;
    /* pass the baton on and exit */
    GThread *n = next_ready(t);
    if (!n) n = thr(kApplicationThreadID);
    n->state = kRunningThreadState;
    g_current = n->id;
    pthread_cond_signal(&n->cond);
    t->used = false;
    pthread_mutex_unlock(&g_baton);
    return NULL;
}

void thread_yield(void) {
    GThread *me = g_cpu->thread;
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
    pthread_cond_init(&t->cond, NULL);
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 8 << 20);
    pthread_create(&t->host, &attr, thread_main, t);
    pthread_detach(t->host);
    if (idp) wr32(idp, t->id);
    LOG_D("NewThread id %u entry %08x param %08x stack %u opts %x", t->id, entry, param, stack, opts);
    RETERR(noErr);
}

TRAP(YieldToAnyThread) { thread_yield(); RETERR(noErr); }

TRAP(YieldToThread) {
    u32 id = ARG(0);
    GThread *me = g_cpu->thread;
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
    GThread *me = g_cpu->thread;
    if (id == kCurrentThreadID) id = g_current;
    GThread *t = thr(id);
    if (!t || t->finished) { RETERR(-617); return; }
    if (t == me) {
        if (st == kStoppedThreadState) {
            me->state = kStoppedThreadState;
            GThread *n = sugg ? thr(sugg) : NULL;
            if (!n || n->state != kReadyThreadState) n = next_ready(me);
            n = consult_scheduler(me, n);
            if (!n || n == me) {
                /* nothing else can run: stay running */
                me->state = kRunningThreadState;
                RETERR(noErr);
                return;
            }
            me->state = kStoppedThreadState;
            g_current = n->id;
            n->state = kRunningThreadState;
            pthread_cond_signal(&n->cond);
            while (g_current != me->id) pthread_cond_wait(&me->cond, &g_baton);
            g_cpu = &me->cpu;
            me->state = kRunningThreadState;
        }
        RETERR(noErr);
        return;
    }
    if (st == kReadyThreadState || st == kStoppedThreadState) t->state = st;
    else if (st == kRunningThreadState) { t->state = kReadyThreadState; switch_to(me, t); }
    RETERR(noErr);
}

TRAP(SetThreadScheduler) { g_scheduler = ARG(0); RETERR(noErr); }
