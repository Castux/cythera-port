/* PowerPC 32-bit user-mode interpreter (UISA integer + FPU).
 *
 * Covers the instruction set emitted by Metrowerks CodeWarrior for the
 * 603/604/750 targets. Supervisor instructions are not needed.
 */
#include "ppc.h"
#include <math.h>
#include <fenv.h>

_Thread_local CPU *g_cpu;
void (*g_cpu_poll)(CPU *c);

void cpu_backtrace(CPU *c, FILE *f);

#define XER_SO 0x80000000u
#define XER_OV 0x40000000u
#define XER_CA 0x20000000u

static _Noreturn void illegal(CPU *c, u32 w) {
    fprintf(stderr, "illegal/unimplemented instruction %08x at %08x\n", w, c->pc);
    cpu_backtrace(c, stderr);
    fatal("CPU halted");
}

static inline void set_cr_field(CPU *c, int fld, u32 v) {
    int sh = 28 - 4 * fld;
    c->cr = (c->cr & ~(0xFu << sh)) | ((v & 0xF) << sh);
}
static inline void update_cr0(CPU *c, u32 res) {
    u32 v = (s32)res < 0 ? 8 : (s32)res > 0 ? 4 : 2;
    if (c->xer & XER_SO) v |= 1;
    c->cr = (c->cr & 0x0FFFFFFF) | (v << 28);
}
static inline void cmp_signed(CPU *c, int fld, s32 a, s32 b) {
    u32 v = a < b ? 8 : a > b ? 4 : 2;
    if (c->xer & XER_SO) v |= 1;
    set_cr_field(c, fld, v);
}
static inline void cmp_unsigned(CPU *c, int fld, u32 a, u32 b) {
    u32 v = a < b ? 8 : a > b ? 4 : 2;
    if (c->xer & XER_SO) v |= 1;
    set_cr_field(c, fld, v);
}
static inline u32 rotl(u32 v, int n) { n &= 31; return n ? (v << n) | (v >> (32 - n)) : v; }
static inline u32 mask(int mb, int me) {
    u32 a = 0xFFFFFFFFu >> mb;
    u32 b = me == 31 ? 0xFFFFFFFFu : ~(0xFFFFFFFFu >> (me + 1));
    return mb <= me ? (a & b) : (a | b);
}
static inline bool crbit(CPU *c, int bit) { return (c->cr >> (31 - bit)) & 1; }
static inline void set_crbit(CPU *c, int bit, bool v) {
    if (v) c->cr |= 1u << (31 - bit); else c->cr &= ~(1u << (31 - bit));
}
static inline void set_ca(CPU *c, bool ca) { if (ca) c->xer |= XER_CA; else c->xer &= ~XER_CA; }
static inline u32 get_ca(CPU *c) { return (c->xer >> 29) & 1; }
static inline void set_ov(CPU *c, bool ov) {
    if (ov) c->xer |= XER_OV | XER_SO; else c->xer &= ~XER_OV;
}

/* add with carry: returns sum, sets CA */
static inline u32 add_carry(CPU *c, u32 a, u32 b, u32 cin) {
    u64 s = (u64)a + b + cin;
    set_ca(c, s >> 32);
    return (u32)s;
}
static inline bool add_ovf(u32 a, u32 b, u32 r) { return ((a ^ r) & (b ^ r)) >> 31; }

/* ---- FPU helpers ---- */
static void fpscr_apply_rounding(CPU *c) {
    static const int modes[4] = { FE_TONEAREST, FE_TOWARDZERO, FE_UPWARD, FE_DOWNWARD };
    fesetround(modes[c->fpscr & 3]);
}
static inline double tosingle(double d) { return (double)(float)d; }
static void fp_compare(CPU *c, int fld, double a, double b) {
    u32 v;
    if (isnan(a) || isnan(b)) v = 1;
    else if (a < b) v = 8;
    else if (a > b) v = 4;
    else v = 2;
    set_cr_field(c, fld, v);
    c->fpscr = (c->fpscr & ~0xF000u) | (v << 12);
}
static void fp_cr1(CPU *c) {
    /* CR1 <- FPSCR[FX FEX VX OX] */
    set_cr_field(c, 1, c->fpscr >> 28);
}
static u32 fp_to_int(CPU *c, double d, bool toward_zero) {
    if (isnan(d)) return 0x80000000u;
    double r;
    if (toward_zero) r = trunc(d);
    else {
        switch (c->fpscr & 3) {
        case 0: r = nearbyint(d); break;  /* host must be in nearest mode */
        case 1: r = trunc(d); break;
        case 2: r = ceil(d); break;
        default: r = floor(d); break;
        }
    }
    if (r >= 2147483647.0) return 0x7FFFFFFFu;
    if (r <= -2147483648.0) return 0x80000000u;
    return (u32)(s32)r;
}

/* ---- load/store string helpers ---- */
static void do_lsw(CPU *c, int rd, u32 ea, u32 n) {
    int r = rd - 1;
    int i = 0;
    while (n > 0) {
        if (i == 0) { r = (r + 1) & 31; c->r[r] = 0; }
        c->r[r] |= (u32)rd8(ea) << (24 - i * 8);
        i = (i + 1) & 3; ea++; n--;
    }
}
static void do_stsw(CPU *c, int rs, u32 ea, u32 n) {
    int r = rs - 1;
    int i = 0;
    while (n > 0) {
        if (i == 0) r = (r + 1) & 31;
        wr8(ea, (u8)(c->r[r] >> (24 - i * 8)));
        i = (i + 1) & 3; ea++; n--;
    }
}

static u64 host_timebase(void);

#define MAX_WATCH 32
static struct { u32 addr, word; } g_watch[MAX_WATCH];
static int g_nwatch;
void cpu_watch_add(u32 addr) {
    if (g_nwatch == MAX_WATCH || (addr & 3)) return;
    g_watch[g_nwatch].addr = addr;
    g_watch[g_nwatch].word = rd32(addr);
    wr32(addr, 0); /* primary opcode 0: illegal */
    g_nwatch++;
}
bool cpu_watch_hit(CPU *c, u32 pc, u32 *w) {
    for (int i = 0; i < g_nwatch; i++) {
        if (g_watch[i].addr != pc) continue;
        u32 *r = c->r;
        LOG_I("watch %06x: r0=%08x r1=%08x r3=%08x r4=%08x r5=%08x r6=%08x r7=%08x r8=%08x "
              "r29=%08x r30=%08x r31=%08x lr=%08x",
              pc - CODE_ADDR, r[0], r[1], r[3], r[4], r[5], r[6], r[7], r[8], r[29], r[30], r[31], c->lr);
        *w = g_watch[i].word;
        return true;
    }
    return false;
}

void cpu_run(CPU *c) {
    c->depth++;
    int my_depth = c->depth;
    u32 poll = CPU_POLL_INTERVAL;
    for (;;) {
        u32 pc = c->pc;
        if (__builtin_expect(pc - TRAP_BASE < TRAP_COUNT * 4, 0)) {
            if (pc == RET_SENTINEL) {
                c->depth--;
                (void)my_depth;
                return;
            }
            trap_dispatch(c, (pc - TRAP_BASE) >> 2);
            c->pc = c->lr & ~3u;
            continue;
        }
        if (__builtin_expect(--poll == 0, 0)) {
            poll = CPU_POLL_INTERVAL;
            if (g_cpu_poll) g_cpu_poll(c);
        }
        if (__builtin_expect(pc & 3, 0)) fatal("misaligned PC %08x", pc);
        u32 w = rd32(pc);
        if (__builtin_expect((w >> 26) == 0, 0)) cpu_watch_hit(c, pc, &w);
        u32 npc = pc + 4;
        c->icount++;
        int op = w >> 26;
        int rD = (w >> 21) & 31, rA = (w >> 16) & 31, rB = (w >> 11) & 31;
        s32 simm = (s16)(w & 0xFFFF);
        u32 uimm = w & 0xFFFF;
        u32 *R = c->r;
        switch (op) {
        case 3: { /* twi */
            s32 a = (s32)R[rA]; int to = rD;
            if (((to & 16) && a < simm) || ((to & 8) && a > simm) || ((to & 4) && a == simm) ||
                ((to & 2) && (u32)a < (u32)simm) || ((to & 1) && (u32)a > (u32)simm)) {
                fprintf(stderr, "twi trap at %08x\n", pc); cpu_backtrace(c, stderr); fatal("trap");
            }
            break;
        }
        case 7: R[rD] = (u32)((s32)R[rA] * simm); break; /* mulli */
        case 8: R[rD] = add_carry(c, ~R[rA], (u32)simm, 1); break; /* subfic */
        case 10: cmp_unsigned(c, rD >> 2, R[rA], uimm); break; /* cmpli */
        case 11: cmp_signed(c, rD >> 2, (s32)R[rA], simm); break; /* cmpi */
        case 12: R[rD] = add_carry(c, R[rA], (u32)simm, 0); break; /* addic */
        case 13: R[rD] = add_carry(c, R[rA], (u32)simm, 0); update_cr0(c, R[rD]); break;
        case 14: R[rD] = (rA ? R[rA] : 0) + (u32)simm; break; /* addi */
        case 15: R[rD] = (rA ? R[rA] : 0) + (uimm << 16); break; /* addis */
        case 16: { /* bc */
            int bo = rD, bi = rA;
            if (!(bo & 4)) c->ctr--;
            bool ctr_ok = (bo & 4) || ((c->ctr != 0) ^ ((bo >> 1) & 1));
            bool cond_ok = (bo & 16) || (crbit(c, bi) == ((bo >> 3) & 1));
            if (w & 1) c->lr = npc;
            if (ctr_ok && cond_ok) {
                s32 bd = (s16)(w & 0xFFFC);
                npc = (w & 2) ? (u32)bd : pc + bd;
            }
            break;
        }
        case 17: illegal(c, w); /* sc */
        case 18: { /* b */
            s32 li = (s32)(w << 6) >> 6; li &= ~3;
            if (w & 1) c->lr = npc;
            npc = (w & 2) ? (u32)li : pc + li;
            break;
        }
        case 19: {
            int xo = (w >> 1) & 0x3FF;
            switch (xo) {
            case 0: set_cr_field(c, rD >> 2, c->cr >> (28 - 4 * (rA >> 2))); break; /* mcrf */
            case 16: case 528: { /* bclr, bcctr */
                int bo = rD, bi = rA;
                if (xo == 16 && !(bo & 4)) c->ctr--;
                bool ctr_ok = xo == 528 || (bo & 4) || ((c->ctr != 0) ^ ((bo >> 1) & 1));
                bool cond_ok = (bo & 16) || (crbit(c, bi) == ((bo >> 3) & 1));
                u32 target = (xo == 16 ? c->lr : c->ctr) & ~3u;
                if (w & 1) c->lr = npc;
                if (ctr_ok && cond_ok) npc = target;
                break;
            }
            case 33: set_crbit(c, rD, !(crbit(c, rA) || crbit(c, rB))); break;  /* crnor */
            case 129: set_crbit(c, rD, crbit(c, rA) && !crbit(c, rB)); break;   /* crandc */
            case 150: break; /* isync */
            case 193: set_crbit(c, rD, crbit(c, rA) ^ crbit(c, rB)); break;    /* crxor */
            case 225: set_crbit(c, rD, !(crbit(c, rA) && crbit(c, rB))); break; /* crnand */
            case 257: set_crbit(c, rD, crbit(c, rA) && crbit(c, rB)); break;    /* crand */
            case 289: set_crbit(c, rD, !(crbit(c, rA) ^ crbit(c, rB))); break; /* creqv */
            case 417: set_crbit(c, rD, crbit(c, rA) || !crbit(c, rB)); break;   /* crorc */
            case 449: set_crbit(c, rD, crbit(c, rA) || crbit(c, rB)); break;    /* cror */
            default: illegal(c, w);
            }
            break;
        }
        case 20: { /* rlwimi */
            int sh = rB, mb = (w >> 6) & 31, me = (w >> 1) & 31;
            u32 m = mask(mb, me);
            R[rA] = (rotl(R[rD], sh) & m) | (R[rA] & ~m);
            if (w & 1) update_cr0(c, R[rA]);
            break;
        }
        case 21: { /* rlwinm */
            int sh = rB, mb = (w >> 6) & 31, me = (w >> 1) & 31;
            R[rA] = rotl(R[rD], sh) & mask(mb, me);
            if (w & 1) update_cr0(c, R[rA]);
            break;
        }
        case 23: { /* rlwnm */
            int mb = (w >> 6) & 31, me = (w >> 1) & 31;
            R[rA] = rotl(R[rD], R[rB] & 31) & mask(mb, me);
            if (w & 1) update_cr0(c, R[rA]);
            break;
        }
        case 24: R[rA] = R[rD] | uimm; break;          /* ori */
        case 25: R[rA] = R[rD] | (uimm << 16); break;  /* oris */
        case 26: R[rA] = R[rD] ^ uimm; break;          /* xori */
        case 27: R[rA] = R[rD] ^ (uimm << 16); break;  /* xoris */
        case 28: R[rA] = R[rD] & uimm; update_cr0(c, R[rA]); break;          /* andi. */
        case 29: R[rA] = R[rD] & (uimm << 16); update_cr0(c, R[rA]); break;  /* andis. */
        case 31: {
            int xo = (w >> 1) & 0x3FF;
            bool rc = w & 1;
            u32 a = R[rA], b = R[rB];
            u32 ea0 = (rA ? a : 0) + b;  /* EA for indexed forms */
            switch (xo) {
            case 0: cmp_signed(c, rD >> 2, (s32)a, (s32)b); break;   /* cmp */
            case 32: cmp_unsigned(c, rD >> 2, a, b); break;          /* cmpl */
            case 4: { /* tw */
                s32 sa = (s32)a, sb = (s32)b; int to = rD;
                if (((to & 16) && sa < sb) || ((to & 8) && sa > sb) || ((to & 4) && sa == sb) ||
                    ((to & 2) && a < b) || ((to & 1) && a > b)) {
                    fprintf(stderr, "tw trap at %08x\n", pc); cpu_backtrace(c, stderr); fatal("trap");
                }
                break;
            }
            /* XO-form arithmetic (with and without OE) */
            case 8: case 520: { u32 r = add_carry(c, ~a, b, 1); if (xo & 0x200) set_ov(c, add_ovf(~a, b, r)); R[rD] = r; if (rc) update_cr0(c, r); break; } /* subfc */
            case 10: case 522: { u32 r = add_carry(c, a, b, 0); if (xo & 0x200) set_ov(c, add_ovf(a, b, r)); R[rD] = r; if (rc) update_cr0(c, r); break; } /* addc */
            case 11: R[rD] = (u32)(((u64)a * b) >> 32); if (rc) update_cr0(c, R[rD]); break; /* mulhwu */
            case 40: case 552: { u32 r = b - a; if (xo & 0x200) set_ov(c, add_ovf(~a, b, r)); R[rD] = r; if (rc) update_cr0(c, r); break; } /* subf */
            case 75: R[rD] = (u32)(((s64)(s32)a * (s32)b) >> 32); if (rc) update_cr0(c, R[rD]); break; /* mulhw */
            case 104: case 616: { u32 r = -a; if (xo & 0x200) set_ov(c, a == 0x80000000u); R[rD] = r; if (rc) update_cr0(c, r); break; } /* neg */
            case 136: case 648: { u32 r = add_carry(c, ~a, b, get_ca(c)); if (xo & 0x200) set_ov(c, add_ovf(~a, b, r)); R[rD] = r; if (rc) update_cr0(c, r); break; } /* subfe */
            case 138: case 650: { u32 r = add_carry(c, a, b, get_ca(c)); if (xo & 0x200) set_ov(c, add_ovf(a, b, r)); R[rD] = r; if (rc) update_cr0(c, r); break; } /* adde */
            case 200: case 712: { u32 r = add_carry(c, ~a, 0, get_ca(c)); if (xo & 0x200) set_ov(c, add_ovf(~a, 0, r)); R[rD] = r; if (rc) update_cr0(c, r); break; } /* subfze */
            case 202: case 714: { u32 r = add_carry(c, a, 0, get_ca(c)); if (xo & 0x200) set_ov(c, add_ovf(a, 0, r)); R[rD] = r; if (rc) update_cr0(c, r); break; } /* addze */
            case 232: case 744: { u32 r = add_carry(c, ~a, 0xFFFFFFFFu, get_ca(c)); if (xo & 0x200) set_ov(c, add_ovf(~a, 0xFFFFFFFFu, r)); R[rD] = r; if (rc) update_cr0(c, r); break; } /* subfme */
            case 234: case 746: { u32 r = add_carry(c, a, 0xFFFFFFFFu, get_ca(c)); if (xo & 0x200) set_ov(c, add_ovf(a, 0xFFFFFFFFu, r)); R[rD] = r; if (rc) update_cr0(c, r); break; } /* addme */
            case 235: case 747: { s64 p = (s64)(s32)a * (s32)b; u32 r = (u32)p; if (xo & 0x200) set_ov(c, p != (s64)(s32)r); R[rD] = r; if (rc) update_cr0(c, r); break; } /* mullw */
            case 266: case 778: { u32 r = a + b; if (xo & 0x200) set_ov(c, add_ovf(a, b, r)); R[rD] = r; if (rc) update_cr0(c, r); break; } /* add */
            case 459: case 971: { u32 r; bool ov = b == 0; r = ov ? 0 : a / b; if (xo & 0x200) set_ov(c, ov); R[rD] = r; if (rc) update_cr0(c, r); break; } /* divwu */
            case 491: case 1003: { u32 r; bool ov = b == 0 || (a == 0x80000000u && b == 0xFFFFFFFFu); r = ov ? ((s32)a < 0 ? 0xFFFFFFFFu : 0) : (u32)((s32)a / (s32)b); if (xo & 0x200) set_ov(c, ov); R[rD] = r; if (rc) update_cr0(c, r); break; } /* divw */

            case 19: R[rD] = c->cr; break; /* mfcr */
            case 144: { /* mtcrf */
                int crm = (w >> 12) & 0xFF; u32 m = 0;
                for (int i = 0; i < 8; i++) if (crm & (0x80 >> i)) m |= 0xF0000000u >> (4 * i);
                c->cr = (R[rD] & m) | (c->cr & ~m);
                break;
            }
            case 512: set_cr_field(c, rD >> 2, c->xer >> 28); c->xer &= 0x0FFFFFFFu; break; /* mcrxr */
            case 339: case 467: case 371: { /* mfspr / mtspr / mftb */
                int spr = ((w >> 16) & 0x1F) | ((w >> 6) & 0x3E0);
                if (xo == 467) {
                    if (spr == 1) c->xer = R[rD];
                    else if (spr == 8) c->lr = R[rD];
                    else if (spr == 9) c->ctr = R[rD];
                    else LOG_W("mtspr %d ignored at %08x", spr, pc);
                } else {
                    u32 v = 0;
                    if (spr == 1) v = c->xer;
                    else if (spr == 8) v = c->lr;
                    else if (spr == 9) v = c->ctr;
                    else if (spr == 268 || spr == 269) { u64 tb = host_timebase(); v = spr == 268 ? (u32)tb : (u32)(tb >> 32); }
                    else if (spr == 287) v = 0x00080200; /* PVR: 750 */
                    else LOG_W("mfspr %d at %08x", spr, pc);
                    R[rD] = v;
                }
                break;
            }
            case 83: R[rD] = 0x0000D032; break; /* mfmsr (user mode, FP enabled) */
            case 146: break;                     /* mtmsr: ignore */

            case 20: R[rD] = rd32(ea0); break;                 /* lwarx */
            case 150: wr32(ea0, R[rD]); set_cr_field(c, 0, 2 | ((c->xer >> 31) & 1)); break; /* stwcx. */

            case 23: R[rD] = rd32(ea0); break;                 /* lwzx */
            case 55: R[rD] = rd32(a + b); R[rA] = a + b; break; /* lwzux */
            case 87: R[rD] = rd8(ea0); break;                  /* lbzx */
            case 119: R[rD] = rd8(a + b); R[rA] = a + b; break; /* lbzux */
            case 279: R[rD] = rd16(ea0); break;                /* lhzx */
            case 311: R[rD] = rd16(a + b); R[rA] = a + b; break;
            case 343: R[rD] = (u32)(s32)rds16(ea0); break;     /* lhax */
            case 375: R[rD] = (u32)(s32)rds16(a + b); R[rA] = a + b; break;
            case 534: R[rD] = __builtin_bswap32(rd32(ea0)); break;           /* lwbrx */
            case 790: R[rD] = __builtin_bswap16(rd16(ea0)); break;           /* lhbrx */
            case 151: wr32(ea0, R[rD]); break;                 /* stwx */
            case 183: wr32(a + b, R[rD]); R[rA] = a + b; break; /* stwux */
            case 215: wr8(ea0, (u8)R[rD]); break;              /* stbx */
            case 247: wr8(a + b, (u8)R[rD]); R[rA] = a + b; break;
            case 407: wr16(ea0, (u16)R[rD]); break;            /* sthx */
            case 439: wr16(a + b, (u16)R[rD]); R[rA] = a + b; break;
            case 662: wr32(ea0, __builtin_bswap32(R[rD])); break;           /* stwbrx */
            case 918: wr16(ea0, __builtin_bswap16((u16)R[rD])); break;      /* sthbrx */
            case 533: do_lsw(c, rD, ea0, c->xer & 0x7F); break;             /* lswx */
            case 597: do_lsw(c, rD, rA ? a : 0, rB ? rB : 32); break;       /* lswi */
            case 661: do_stsw(c, rD, ea0, c->xer & 0x7F); break;            /* stswx */
            case 725: do_stsw(c, rD, rA ? a : 0, rB ? rB : 32); break;      /* stswi */
            case 535: c->f[rD].d = (double)({ u32 v = rd32(ea0); float fl; memcpy(&fl, &v, 4); fl; }); break; /* lfsx */
            case 567: { u32 ea = a + b; u32 v = rd32(ea); float fl; memcpy(&fl, &v, 4); c->f[rD].d = fl; R[rA] = ea; break; } /* lfsux */
            case 599: c->f[rD].u = rd64(ea0); break;           /* lfdx */
            case 631: c->f[rD].u = rd64(a + b); R[rA] = a + b; break;
            case 663: { float fl = (float)c->f[rD].d; u32 v; memcpy(&v, &fl, 4); wr32(ea0, v); break; } /* stfsx */
            case 695: { float fl = (float)c->f[rD].d; u32 v; memcpy(&v, &fl, 4); wr32(a + b, v); R[rA] = a + b; break; }
            case 727: wr64(ea0, c->f[rD].u); break;            /* stfdx */
            case 759: wr64(a + b, c->f[rD].u); R[rA] = a + b; break;
            case 983: wr32(ea0, (u32)c->f[rD].u); break;       /* stfiwx */

            case 24: { u32 n = b & 0x3F; R[rA] = n & 0x20 ? 0 : R[rD] << n; if (rc) update_cr0(c, R[rA]); break; } /* slw */
            case 536: { u32 n = b & 0x3F; R[rA] = n & 0x20 ? 0 : R[rD] >> n; if (rc) update_cr0(c, R[rA]); break; } /* srw */
            case 792: { /* sraw */
                u32 n = b & 0x3F; s32 s = (s32)R[rD];
                if (n & 0x20) { R[rA] = s < 0 ? 0xFFFFFFFFu : 0; set_ca(c, s < 0); }
                else { R[rA] = (u32)(s >> n); set_ca(c, s < 0 && n && (R[rD] << (32 - n))); }
                if (rc) update_cr0(c, R[rA]);
                break;
            }
            case 824: { /* srawi */
                int n = rB; s32 s = (s32)R[rD];
                R[rA] = (u32)(s >> n);
                set_ca(c, s < 0 && n && (R[rD] << (32 - n)));
                if (rc) update_cr0(c, R[rA]);
                break;
            }
            case 26: R[rA] = R[rD] ? (u32)__builtin_clz(R[rD]) : 32; if (rc) update_cr0(c, R[rA]); break; /* cntlzw */
            case 28: R[rA] = R[rD] & b; if (rc) update_cr0(c, R[rA]); break;    /* and */
            case 60: R[rA] = R[rD] & ~b; if (rc) update_cr0(c, R[rA]); break;   /* andc */
            case 124: R[rA] = ~(R[rD] | b); if (rc) update_cr0(c, R[rA]); break; /* nor */
            case 284: R[rA] = ~(R[rD] ^ b); if (rc) update_cr0(c, R[rA]); break; /* eqv */
            case 316: R[rA] = R[rD] ^ b; if (rc) update_cr0(c, R[rA]); break;   /* xor */
            case 412: R[rA] = R[rD] | ~b; if (rc) update_cr0(c, R[rA]); break;  /* orc */
            case 444: R[rA] = R[rD] | b; if (rc) update_cr0(c, R[rA]); break;   /* or */
            case 476: R[rA] = ~(R[rD] & b); if (rc) update_cr0(c, R[rA]); break; /* nand */
            case 922: R[rA] = (u32)(s32)(s16)R[rD]; if (rc) update_cr0(c, R[rA]); break; /* extsh */
            case 954: R[rA] = (u32)(s32)(s8)R[rD]; if (rc) update_cr0(c, R[rA]); break;  /* extsb */

            case 1014: { u32 ea = ea0 & ~31u; gmemset(ea, 0, 32); break; } /* dcbz */
            case 54: case 86: case 246: case 278: case 470: case 982: case 598: case 854: case 306:
                break; /* dcbst dcbf dcbtst dcbt dcbi icbi sync eieio tlbie */
            default: illegal(c, w);
            }
            break;
        }
        case 32: R[rD] = rd32((rA ? R[rA] : 0) + simm); break; /* lwz */
        case 33: { u32 ea = R[rA] + simm; R[rD] = rd32(ea); R[rA] = ea; break; }
        case 34: R[rD] = rd8((rA ? R[rA] : 0) + simm); break;  /* lbz */
        case 35: { u32 ea = R[rA] + simm; R[rD] = rd8(ea); R[rA] = ea; break; }
        case 36: wr32((rA ? R[rA] : 0) + simm, R[rD]); break;  /* stw */
        case 37: { u32 ea = R[rA] + simm; wr32(ea, R[rD]); R[rA] = ea; break; }
        case 38: wr8((rA ? R[rA] : 0) + simm, (u8)R[rD]); break; /* stb */
        case 39: { u32 ea = R[rA] + simm; wr8(ea, (u8)R[rD]); R[rA] = ea; break; }
        case 40: R[rD] = rd16((rA ? R[rA] : 0) + simm); break; /* lhz */
        case 41: { u32 ea = R[rA] + simm; R[rD] = rd16(ea); R[rA] = ea; break; }
        case 42: R[rD] = (u32)(s32)rds16((rA ? R[rA] : 0) + simm); break; /* lha */
        case 43: { u32 ea = R[rA] + simm; R[rD] = (u32)(s32)rds16(ea); R[rA] = ea; break; }
        case 44: wr16((rA ? R[rA] : 0) + simm, (u16)R[rD]); break; /* sth */
        case 45: { u32 ea = R[rA] + simm; wr16(ea, (u16)R[rD]); R[rA] = ea; break; }
        case 46: { u32 ea = (rA ? R[rA] : 0) + simm; for (int r = rD; r < 32; r++, ea += 4) R[r] = rd32(ea); break; } /* lmw */
        case 47: { u32 ea = (rA ? R[rA] : 0) + simm; for (int r = rD; r < 32; r++, ea += 4) wr32(ea, R[r]); break; } /* stmw */
        case 48: case 49: { /* lfs, lfsu */
            u32 ea = (op == 49 || rA) ? R[rA] + simm : (u32)simm;
            u32 v = rd32(ea); float fl; memcpy(&fl, &v, 4); c->f[rD].d = fl;
            if (op == 49) R[rA] = ea;
            break;
        }
        case 50: case 51: { /* lfd, lfdu */
            u32 ea = (op == 51 || rA) ? R[rA] + simm : (u32)simm;
            c->f[rD].u = rd64(ea);
            if (op == 51) R[rA] = ea;
            break;
        }
        case 52: case 53: { /* stfs, stfsu */
            u32 ea = (op == 53 || rA) ? R[rA] + simm : (u32)simm;
            float fl = (float)c->f[rD].d; u32 v; memcpy(&v, &fl, 4); wr32(ea, v);
            if (op == 53) R[rA] = ea;
            break;
        }
        case 54: case 55: { /* stfd, stfdu */
            u32 ea = (op == 55 || rA) ? R[rA] + simm : (u32)simm;
            wr64(ea, c->f[rD].u);
            if (op == 55) R[rA] = ea;
            break;
        }
        case 59: { /* single-precision arithmetic */
            int xo = (w >> 1) & 31, rC = (w >> 6) & 31;
            double fa = c->f[rA].d, fb = c->f[rB].d, fc = c->f[rC].d, r;
            switch (xo) {
            case 18: r = tosingle(fa / fb); break;
            case 20: r = tosingle(fa - fb); break;
            case 21: r = tosingle(fa + fb); break;
            case 22: r = tosingle(sqrt(fb)); break;
            case 24: r = tosingle(1.0 / fb); break; /* fres */
            case 25: r = tosingle(fa * fc); break;
            case 28: r = tosingle(fma(fa, fc, -fb)); break;
            case 29: r = tosingle(fma(fa, fc, fb)); break;
            case 30: r = tosingle(-fma(fa, fc, -fb)); break;
            case 31: r = tosingle(-fma(fa, fc, fb)); break;
            default: illegal(c, w);
            }
            c->f[rD].d = r;
            if (w & 1) fp_cr1(c);
            break;
        }
        case 63: {
            int xo5 = (w >> 1) & 31, rC = (w >> 6) & 31;
            double fa = c->f[rA].d, fb = c->f[rB].d, fc = c->f[rC].d;
            if (xo5 >= 18 && xo5 != 24 && xo5 != 27) {
                double r;
                switch (xo5) {
                case 18: r = fa / fb; break;
                case 20: r = fa - fb; break;
                case 21: r = fa + fb; break;
                case 22: r = sqrt(fb); break;
                case 23: r = (fa >= 0.0) ? fc : fb; break; /* fsel (NaN -> fb) */
                case 25: r = fa * fc; break;
                case 26: r = 1.0 / sqrt(fb); break; /* frsqrte */
                case 28: r = fma(fa, fc, -fb); break;
                case 29: r = fma(fa, fc, fb); break;
                case 30: r = -fma(fa, fc, -fb); break;
                case 31: r = -fma(fa, fc, fb); break;
                default: illegal(c, w);
                }
                c->f[rD].d = r;
                if (w & 1) fp_cr1(c);
                break;
            }
            int xo = (w >> 1) & 0x3FF;
            switch (xo) {
            case 0: fp_compare(c, rD >> 2, fa, fb); break;  /* fcmpu */
            case 32: fp_compare(c, rD >> 2, fa, fb); break; /* fcmpo */
            case 12: c->f[rD].d = tosingle(fb); break;      /* frsp */
            case 14: c->f[rD].u = 0xFFF8000000000000ull | fp_to_int(c, fb, false); break; /* fctiw */
            case 15: c->f[rD].u = 0xFFF8000000000000ull | fp_to_int(c, fb, true); break;  /* fctiwz */
            case 40: c->f[rD].u = c->f[rB].u ^ 0x8000000000000000ull; break; /* fneg */
            case 72: c->f[rD].u = c->f[rB].u; break;                         /* fmr */
            case 136: c->f[rD].u = c->f[rB].u | 0x8000000000000000ull; break; /* fnabs */
            case 264: c->f[rD].u = c->f[rB].u & 0x7FFFFFFFFFFFFFFFull; break; /* fabs */
            case 38: c->fpscr |= 0x80000000u >> rD; fpscr_apply_rounding(c); break;  /* mtfsb1 */
            case 70: c->fpscr &= ~(0x80000000u >> rD); fpscr_apply_rounding(c); break; /* mtfsb0 */
            case 64: { /* mcrfs */
                int s = rA >> 2;
                set_cr_field(c, rD >> 2, c->fpscr >> (28 - 4 * s));
                c->fpscr &= ~((0xFu << (28 - 4 * s)) & 0x9FF80700u);
                break;
            }
            case 134: { /* mtfsfi */
                int fld = rD >> 2, imm = (w >> 12) & 0xF;
                c->fpscr = (c->fpscr & ~(0xFu << (28 - 4 * fld))) | ((u32)imm << (28 - 4 * fld));
                fpscr_apply_rounding(c);
                break;
            }
            case 583: c->f[rD].u = 0xFFF8000000000000ull | c->fpscr; break; /* mffs */
            case 711: { /* mtfsf */
                int fm = (w >> 17) & 0xFF; u32 m = 0;
                for (int i = 0; i < 8; i++) if (fm & (0x80 >> i)) m |= 0xF0000000u >> (4 * i);
                c->fpscr = ((u32)c->f[rB].u & m) | (c->fpscr & ~m);
                fpscr_apply_rounding(c);
                break;
            }
            default: illegal(c, w);
            }
            if ((w & 1) && xo != 0 && xo != 32) fp_cr1(c);
            break;
        }
        default:
            illegal(c, w);
        }
        c->pc = npc;
    }
}

#include <time.h>
static u64 host_timebase(void) {
    /* 603/750 time base ticks at bus/4; report a 25 MHz time base. */
    extern bool g_deterministic;
    extern u64 g_vclock_ns;
    if (g_deterministic) return g_vclock_ns / 40;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (u64)ts.tv_sec * 25000000ull + (u64)ts.tv_nsec / 40;
}
