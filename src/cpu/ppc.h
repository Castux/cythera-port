/* PowerPC 32-bit user-mode interpreter. */
#ifndef PPC_H
#define PPC_H

#include "../common.h"

typedef union { double d; u64 u; } FPR;

typedef struct CPU {
    u32 r[32];
    FPR f[32];
    u32 cr;
    u32 lr, ctr, xer;
    u32 pc;
    u32 fpscr;
    u64 icount;
    u64 last_trap_icount; /* for stall detection */
    int depth;          /* nesting depth of cpu_run() */
    void *thread;       /* owning guest thread (threads.c) */
} CPU;

/* Current CPU of the calling host thread. */
extern _Thread_local CPU *g_cpu;

/* Run until PC reaches RET_SENTINEL (at the same nesting level). */
void cpu_run(CPU *c);

/* Called by the interpreter when PC enters the trap range. */
void trap_dispatch(CPU *c, u32 index);

/* Hook for periodic host work (events, timers); called every N instructions. */
extern void (*g_cpu_poll)(CPU *c);
#define CPU_POLL_INTERVAL 20000

#endif
