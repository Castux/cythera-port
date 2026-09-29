/* Shared definitions for the high-level-emulated Mac OS Toolbox. */
#ifndef OS_H
#define OS_H

#include "../common.h"
#include "../cpu/ppc.h"

/* Define a trap handler for an imported Toolbox routine.  The build scans
   for TRAP(Name) to generate the name->handler table. */
#define TRAP(name) void trap_##name(CPU *cpu)

/* Argument access (PowerOPEN ABI): first 8 words in r3..r10, rest in the
   caller's parameter area at 24(r1). */
static inline u32 trap_arg(CPU *c, int n) {
    return n < 8 ? c->r[3 + n] : rd32(c->r[1] + 24 + 4 * (u32)n);
}
#define ARG(n)    trap_arg(cpu, (n))
#define ARGS16(n) ((s16)ARG(n))
#define ARGU16(n) ((u16)ARG(n))
#define ARGB(n)   ((u8)ARG(n) != 0)
#define RET(v)    (cpu->r[3] = (u32)(v))
#define RETERR(e) (cpu->r[3] = (u32)(s32)(e))

/* Call guest code through a TVector. Returns r3. */
u32 guest_call(u32 tvector, int nargs, const u32 *args);
/* Call a UniversalProcPtr (routine descriptor or raw TVector). */
u32 call_upp(u32 upp, int nargs, const u32 *args);
/* Allocate a trap slot bound to a native function; returns its TVector
   (usable as a ProcPtr by guest code). */
u32 trap_native_tvector(const char *name, void (*fn)(CPU *));

/* Trap table (generated) */
typedef struct { const char *name; void (*fn)(CPU *); } TrapDef;
extern const TrapDef g_trap_defs[];
extern const int g_trap_ndefs;

/* Import binding */
u32 trap_resolve_import(const char *lib, const char *name, bool weak);
const char *trap_name(u32 index);
extern bool g_trace_traps;
extern bool g_strict_traps;

/* Initialisation of all managers */
void os_init(void);

/* Mac Rect / Point helpers (guest memory) */
typedef struct { s16 top, left, bottom, right; } Rect;
typedef struct { s16 v, h; } Point;
static inline Rect rd_rect(u32 a) {
    Rect r = { rds16(a), rds16(a + 2), rds16(a + 4), rds16(a + 6) };
    return r;
}
static inline void wr_rect(u32 a, Rect r) {
    wr16(a, (u16)r.top); wr16(a + 2, (u16)r.left); wr16(a + 4, (u16)r.bottom); wr16(a + 6, (u16)r.right);
}
static inline Point pt_from_u32(u32 v) { Point p = { (s16)(v >> 16), (s16)v }; return p; }
static inline u32 pt_to_u32(Point p) { return (u32)(u16)p.v << 16 | (u16)p.h; }
static inline Point rd_point(u32 a) { Point p = { rds16(a), rds16(a + 2) }; return p; }
static inline void wr_point(u32 a, Point p) { wr16(a, (u16)p.v); wr16(a + 2, (u16)p.h); }

#endif
