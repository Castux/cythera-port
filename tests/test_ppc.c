/* PowerPC interpreter unit tests (build: make test).
 *
 * Each test assembles a few instruction words at CODE_ADDR, terminated by
 * "ba RET_SENTINEL", sets up registers directly in the CPU struct, runs
 * cpu_run() and compares the resulting state against the architected
 * PowerPC 32-bit UISA semantics (reference models below are written
 * independently of the interpreter, using 64-bit arithmetic).
 */
#include "../src/cpu/ppc.h"
#include <math.h>
#include <setjmp.h>

/* ---- stubs for symbols normally provided by the rest of the runtime ---- */
bool g_deterministic;
u64 g_vclock_ns;
static jmp_buf g_crash;
static bool g_in_run;
void trap_dispatch(CPU *c, u32 index) {
    fprintf(stderr, "unexpected trap %u at pc %08x\n", index, c->pc);
    if (g_in_run) longjmp(g_crash, 1);
    exit(2);
}
/* Called by illegal() and fatal() (via g_cpu): turn a CPU halt into a test failure. */
void cpu_backtrace(CPU *c, FILE *f) {
    if (g_in_run) { g_in_run = false; longjmp(g_crash, 1); }
}

/* ---- harness ---- */
#define XER_SO 0x80000000u
#define XER_OV 0x40000000u
#define XER_CA 0x20000000u
#define DATA   DATA_ADDR

static CPU C;
static u32 prog[512];
static int np;
static int n_tests, n_fail;
static const char *g_name;

static void emit(u32 w) { prog[np++] = w; }
static u32 here(void) { return CODE_ADDR + 4u * (u32)np; }

/* Reset CPU state and start a new program. */
static void begin(const char *name) {
    memset(&C, 0, sizeof C);
    C.r[1] = STACK_AREA_END - 0x1000;
    np = 0;
    g_name = name;
}

static u32 enc_b(u32 target_abs, int aa, int lk, u32 from);
static bool run(void) {
    for (int i = 0; i < np; i++) wr32(CODE_ADDR + 4u * i, prog[i]);
    wr32(CODE_ADDR + 4u * np, enc_b(RET_SENTINEL, 1, 0, 0)); /* ba RET_SENTINEL */
    C.pc = CODE_ADDR;
    if (!C.lr) C.lr = RET_SENTINEL;
    C.depth = 0;
    g_cpu = &C;
    g_in_run = true;
    if (setjmp(g_crash)) {
        g_in_run = false;
        n_tests++; n_fail++;
        printf("FAIL %s: CPU halted (see stderr)\n", g_name);
        return false;
    }
    cpu_run(&C);
    g_in_run = false;
    return true;
}

static bool check(const char *what, u64 got, u64 exp) {
    n_tests++;
    if (got == exp) return true;
    n_fail++;
    printf("FAIL %s: %s = %08llx, expected %08llx\n", g_name, what,
           (unsigned long long)got, (unsigned long long)exp);
    return false;
}
static bool checkd(const char *what, double got, double exp) {
    n_tests++;
    FPR g = { .d = got }, e = { .d = exp };
    if (g.u == e.u || (isnan(got) && isnan(exp))) return true;
    n_fail++;
    printf("FAIL %s: %s = %.17g (%016llx), expected %.17g (%016llx)\n", g_name, what,
           got, (unsigned long long)g.u, exp, (unsigned long long)e.u);
    return false;
}
static char nbuf[256];
#define NAME(...) (snprintf(nbuf, sizeof nbuf, __VA_ARGS__), nbuf)

/* ---- instruction encoders ---- */
static u32 D(int op, int d, int a, int imm) { return (u32)op << 26 | (u32)d << 21 | (u32)a << 16 | ((u32)imm & 0xFFFF); }
static u32 X(int d, int a, int b, int xo, int rc) { return 31u << 26 | (u32)d << 21 | (u32)a << 16 | (u32)b << 11 | (u32)xo << 1 | (u32)rc; }
static u32 XO(int d, int a, int b, int oe, int xo, int rc) { return 31u << 26 | (u32)d << 21 | (u32)a << 16 | (u32)b << 11 | (u32)oe << 10 | (u32)xo << 1 | (u32)rc; }
static u32 M(int op, int s, int a, int sh, int mb, int me, int rc) { return (u32)op << 26 | (u32)s << 21 | (u32)a << 16 | (u32)sh << 11 | (u32)mb << 6 | (u32)me << 1 | (u32)rc; }
static u32 FA(int op, int d, int a, int b, int c, int xo, int rc) { return (u32)op << 26 | (u32)d << 21 | (u32)a << 16 | (u32)b << 11 | (u32)c << 6 | (u32)xo << 1 | (u32)rc; }
static u32 FX(int d, int a, int b, int xo, int rc) { return 63u << 26 | (u32)d << 21 | (u32)a << 16 | (u32)b << 11 | (u32)xo << 1 | (u32)rc; }
static u32 XL(int bt, int ba, int bb, int xo, int lk) { return 19u << 26 | (u32)bt << 21 | (u32)ba << 16 | (u32)bb << 11 | (u32)xo << 1 | (u32)lk; }

static u32 enc_b(u32 target, int aa, int lk, u32 from) {
    u32 li = aa ? target : target - from;
    return 18u << 26 | (li & 0x03FFFFFCu) | (u32)aa << 1 | (u32)lk;
}
static u32 enc_bc(int bo, int bi, u32 target, u32 from, int lk) {
    return 16u << 26 | (u32)bo << 21 | (u32)bi << 16 | ((target - from) & 0xFFFC) | (u32)lk;
}
static u32 li(int d, int v) { return D(14, d, 0, v); }
static u32 nop(void) { return D(24, 0, 0, 0); }
static u32 spr_field(int spr) { return (u32)(((spr & 31) << 5) | (spr >> 5)) << 11; }
static u32 mfspr(int d, int spr) { return 31u << 26 | (u32)d << 21 | spr_field(spr) | 339u << 1; }
static u32 mtspr(int spr, int s) { return 31u << 26 | (u32)s << 21 | spr_field(spr) | 467u << 1; }
static u32 cmp(int crf, int a, int b) { return X(crf << 2, a, b, 0, 0); }
static u32 cmpl(int crf, int a, int b) { return X(crf << 2, a, b, 32, 0); }
static u32 cmpi(int crf, int a, int v) { return D(11, crf << 2, a, v); }
static u32 cmpli(int crf, int a, int v) { return D(10, crf << 2, a, v); }

/* XO-form opcodes */
enum { ADD = 266, ADDC = 10, ADDE = 138, ADDZE = 202, ADDME = 234, SUBF = 40, SUBFC = 8,
       SUBFE = 136, SUBFZE = 200, SUBFME = 232, NEG = 104, MULLW = 235, MULHW = 75,
       MULHWU = 11, DIVW = 491, DIVWU = 459 };

static u32 cr0_of(u32 r, bool so) { return ((s32)r < 0 ? 8u : (s32)r > 0 ? 4u : 2u) | (so ? 1u : 0u); }

/* ------------------------------------------------------------------------ */
/* Integer arithmetic (XO form): reference model over edge-case operands.   */

static const u32 vals[] = { 0, 1, 2, 0x7FFFFFFF, 0x80000000, 0x80000001, 0xFFFFFFFF,
                            0xFFFFFFFE, 0x12345678, 0xEDCBA988, 0x00010000, 0xFFFF0000 };
#define NV (int)(sizeof vals / sizeof vals[0])

static bool s32_ovf(s64 v) { return v < INT32_MIN || v > INT32_MAX; }

/* returns false if the case is architecturally undefined (skipped) */
static bool ref_xo(int xo, u32 a, u32 b, u32 ca, u32 *r, int *ca_out, bool *ov) {
    s64 sa = (s32)a, sb = (s32)b;
    *ca_out = -1;
    switch (xo) {
    case ADD:    *r = a + b; *ov = s32_ovf(sa + sb); break;
    case ADDC:   *r = a + b; *ov = s32_ovf(sa + sb); *ca_out = (u64)a + b > 0xFFFFFFFFu; break;
    case ADDE:   *r = a + b + ca; *ov = s32_ovf(sa + sb + ca); *ca_out = (u64)a + b + ca > 0xFFFFFFFFu; break;
    case ADDZE:  *r = a + ca; *ov = s32_ovf(sa + ca); *ca_out = (u64)a + ca > 0xFFFFFFFFu; break;
    case ADDME:  *r = a + ca - 1; *ov = s32_ovf(sa + ca - 1); *ca_out = a != 0 || ca; break;
    case SUBF:   *r = b - a; *ov = s32_ovf(sb - sa); break;
    case SUBFC:  *r = b - a; *ov = s32_ovf(sb - sa); *ca_out = b >= a; break;
    /* ~a + b + ca == b - a - 1 + ca; carry out iff b + ca > a (unsigned) */
    case SUBFE:  *r = b - a - 1 + ca; *ov = s32_ovf(sb - sa - 1 + ca); *ca_out = (u64)b + ca > a; break;
    case SUBFZE: *r = ~a + ca; *ov = s32_ovf(-sa - 1 + ca); *ca_out = a == 0 && ca; break;
    case SUBFME: *r = ~a + ca - 1; *ov = s32_ovf(-sa - 2 + ca); *ca_out = a != 0xFFFFFFFFu || ca; break;
    case NEG:    *r = 0u - a; *ov = a == 0x80000000u; break;
    case MULLW:  *r = (u32)(sa * sb); *ov = s32_ovf(sa * sb); break;
    case MULHW:  *r = (u32)((u64)(sa * sb) >> 32); *ov = false; break;
    case MULHWU: *r = (u32)(((u64)a * b) >> 32); *ov = false; break;
    case DIVW:   if (b == 0 || (a == 0x80000000u && b == 0xFFFFFFFFu)) return false;
                 *r = (u32)(s32)(sa / sb); *ov = false; break;
    case DIVWU:  if (b == 0) return false;
                 *r = a / b; *ov = false; break;
    }
    return true;
}

static void test_xo_arith(void) {
    static const struct { const char *name; int xo; bool unary, has_oe; } ops[] = {
        { "add", ADD, 0, 1 }, { "addc", ADDC, 0, 1 }, { "adde", ADDE, 0, 1 },
        { "addze", ADDZE, 1, 1 }, { "addme", ADDME, 1, 1 }, { "subf", SUBF, 0, 1 },
        { "subfc", SUBFC, 0, 1 }, { "subfe", SUBFE, 0, 1 }, { "subfze", SUBFZE, 1, 1 },
        { "subfme", SUBFME, 1, 1 }, { "neg", NEG, 1, 1 }, { "mullw", MULLW, 0, 1 },
        { "mulhw", MULHW, 0, 0 }, { "mulhwu", MULHWU, 0, 0 }, { "divw", DIVW, 0, 1 },
        { "divwu", DIVWU, 0, 1 },
    };
    static const u32 xers[] = { 0, XER_CA, XER_SO | XER_OV, XER_SO | XER_CA, XER_SO };
    for (size_t k = 0; k < sizeof ops / sizeof ops[0]; k++)
    for (int oe = 0; oe <= (int)ops[k].has_oe; oe++)
    for (int rc = 0; rc <= 1; rc++)
    for (int i = 0; i < NV; i++)
    for (int j = 0; j < (ops[k].unary ? 1 : NV); j++)
    for (size_t x = 0; x < sizeof xers / sizeof xers[0]; x++) {
        u32 a = vals[i], b = ops[k].unary ? 0 : vals[j], xin = xers[x];
        u32 r; int ca; bool ov;
        if (!ref_xo(ops[k].xo, a, b, (xin >> 29) & 1, &r, &ca, &ov)) continue;
        u32 xexp = xin;
        if (ca >= 0) xexp = (xexp & ~XER_CA) | (ca ? XER_CA : 0);
        if (oe) xexp = (xexp & ~XER_OV) | (ov ? XER_OV | XER_SO : 0);
        u32 crin = 0x0ABCDEF5;
        u32 crexp = rc ? (crin & 0x0FFFFFFF) | cr0_of(r, xexp & XER_SO) << 28 : crin;

        begin(NAME("%s%s%s a=%08x b=%08x xer=%08x", ops[k].name, oe ? "o" : "", rc ? "." : "", a, b, xin));
        C.r[3] = a; C.r[4] = b; C.xer = xin; C.cr = crin;
        emit(XO(5, 3, ops[k].unary ? 0 : 4, oe, ops[k].xo, rc));
        if (!run()) continue;
        n_tests++;
        if (C.r[5] != r || C.xer != xexp || C.cr != crexp) {
            n_fail++;
            printf("FAIL %s: r=%08x xer=%08x cr=%08x, expected r=%08x xer=%08x cr=%08x\n",
                   g_name, C.r[5], C.xer, C.cr, r, xexp, crexp);
        }
    }

    /* A few hand-computed vectors, independent of the reference model. */
    begin("addc 0xFFFFFFFF+1"); C.r[3] = 0xFFFFFFFF; C.r[4] = 1;
    emit(XO(5, 3, 4, 0, ADDC, 1)); run();
    check("r5", C.r[5], 0); check("CA", C.xer & XER_CA, XER_CA); check("cr0", C.cr >> 28, 2);

    begin("subfc 5-3 (CA=1, no borrow)"); C.r[3] = 3; C.r[4] = 5;
    emit(XO(5, 3, 4, 0, SUBFC, 0)); run();
    check("r5", C.r[5], 2); check("CA", C.xer & XER_CA, XER_CA);

    begin("subfc 3-5 (CA=0, borrow)"); C.r[3] = 5; C.r[4] = 3;
    emit(XO(5, 3, 4, 0, SUBFC, 0)); run();
    check("r5", C.r[5], 0xFFFFFFFE); check("CA", C.xer & XER_CA, 0);

    /* 64-bit add: (r3:r4) + (r5:r6) = (r7:r8) */
    begin("64-bit add addc/adde"); C.r[3] = 0x00000001; C.r[4] = 0xFFFFFFFF; C.r[5] = 0x00000002; C.r[6] = 0x00000001;
    emit(XO(8, 4, 6, 0, ADDC, 0)); emit(XO(7, 3, 5, 0, ADDE, 0)); run();
    check("hi", C.r[7], 4); check("lo", C.r[8], 0);

    /* 64-bit sub: (r3:r4) - (r5:r6) */
    begin("64-bit sub subfc/subfe"); C.r[3] = 1; C.r[4] = 0; C.r[5] = 0; C.r[6] = 1;
    emit(XO(8, 6, 4, 0, SUBFC, 0)); emit(XO(7, 5, 3, 0, SUBFE, 0)); run();
    check("hi", C.r[7], 0); check("lo", C.r[8], 0xFFFFFFFF);

    begin("addo overflow sets OV+SO, cr0 SO"); C.r[3] = 0x7FFFFFFF; C.r[4] = 1;
    emit(XO(5, 3, 4, 1, ADD, 1)); run();
    check("r5", C.r[5], 0x80000000); check("xer", C.xer, XER_SO | XER_OV); check("cr0", C.cr >> 28, 9);

    begin("addo no overflow clears OV, keeps SO"); C.r[3] = 1; C.r[4] = 1; C.xer = XER_SO | XER_OV;
    emit(XO(5, 3, 4, 1, ADD, 1)); run();
    check("xer", C.xer, XER_SO); check("cr0", C.cr >> 28, 5);

    begin("add. copies SO into cr0"); C.r[3] = 0; C.r[4] = 0; C.xer = XER_SO;
    emit(XO(5, 3, 4, 0, ADD, 1)); run();
    check("cr0", C.cr >> 28, 3);

    begin("nego 0x80000000"); C.r[3] = 0x80000000;
    emit(XO(5, 3, 0, 1, NEG, 0)); run();
    check("r5", C.r[5], 0x80000000); check("xer", C.xer, XER_SO | XER_OV);

    begin("mullwo overflow"); C.r[3] = 0x10000; C.r[4] = 0x10000;
    emit(XO(5, 3, 4, 1, MULLW, 0)); run();
    check("r5", C.r[5], 0); check("xer", C.xer, XER_SO | XER_OV);

    begin("mulhw -1*-1 / mulhwu"); C.r[3] = 0xFFFFFFFF; C.r[4] = 0xFFFFFFFF;
    emit(XO(5, 3, 4, 0, MULHW, 0)); emit(XO(6, 3, 4, 0, MULHWU, 0)); run();
    check("mulhw", C.r[5], 0); check("mulhwu", C.r[6], 0xFFFFFFFE);

    begin("divw -7/2 truncates toward zero"); C.r[3] = (u32)-7; C.r[4] = 2;
    emit(XO(5, 3, 4, 0, DIVW, 0)); emit(XO(6, 3, 4, 0, DIVWU, 0)); run();
    check("divw", C.r[5], (u32)-3); check("divwu", C.r[6], 0x7FFFFFFC);
}

/* ---- D-form arithmetic ---- */
static void test_imm_arith(void) {
    begin("addi/li rA=0 means literal 0"); C.r[0] = 1000;
    emit(D(14, 3, 0, -5)); emit(D(14, 4, 3, 0x7FFF)); emit(D(14, 5, 3, -0x8000)); run();
    check("li", C.r[3], (u32)-5); check("addi", C.r[4], 0x7FFA); check("addi neg", C.r[5], (u32)-5 - 0x8000);

    begin("addis/lis"); C.r[0] = 1000; C.r[6] = 0x1234;
    emit(D(15, 3, 0, 0x8000)); emit(D(15, 4, 6, 1)); emit(D(15, 5, 6, -1)); run();
    check("lis", C.r[3], 0x80000000); check("addis", C.r[4], 0x11234); check("addis -1", C.r[5], 0xFFFF1234);

    begin("addic CA"); C.r[3] = 0xFFFFFFFF;
    emit(D(12, 4, 3, 1)); run();
    check("r4", C.r[4], 0); check("CA", C.xer & XER_CA, XER_CA);

    begin("addic -1 on 0: no carry"); C.r[3] = 0; C.xer = XER_CA;
    emit(D(12, 4, 3, -1)); run();
    check("r4", C.r[4], 0xFFFFFFFF); check("CA", C.xer & XER_CA, 0);

    begin("addic. sets cr0 with SO"); C.r[3] = 5; C.xer = XER_SO;
    emit(D(13, 4, 3, -10)); run();
    check("r4", C.r[4], (u32)-5); check("cr0", C.cr >> 28, 9); check("CA", C.xer & XER_CA, 0);

    begin("subfic"); C.r[3] = 3;
    emit(D(8, 4, 3, 10)); emit(D(8, 5, 3, 2)); run();
    check("r4", C.r[4], 7); check("r5", C.r[5], 0xFFFFFFFF); check("CA (last: 2-3 borrows)", C.xer & XER_CA, 0);

    begin("subfic CA=1"); C.r[3] = 3;
    emit(D(8, 4, 3, 3)); run();
    check("r4", C.r[4], 0); check("CA", C.xer & XER_CA, XER_CA);

    begin("subfic imm -1 (0xFFFFFFFF - a)"); C.r[3] = 0x10;
    emit(D(8, 4, 3, -1)); run();
    check("r4", C.r[4], 0xFFFFFFEF); check("CA", C.xer & XER_CA, XER_CA);

    begin("mulli"); C.r[3] = (u32)-3;
    emit(D(7, 4, 3, 1000)); emit(D(7, 5, 3, -2)); run();
    check("r4", C.r[4], (u32)-3000); check("r5", C.r[5], 6);
}

/* ---- logical ---- */
static void test_logical(void) {
    static const struct { const char *name; int xo; } ops[] = {
        { "and", 28 }, { "or", 444 }, { "xor", 316 }, { "nand", 476 }, { "nor", 124 },
        { "andc", 60 }, { "orc", 412 }, { "eqv", 284 },
    };
    for (size_t k = 0; k < sizeof ops / sizeof ops[0]; k++)
    for (int rc = 0; rc <= 1; rc++)
    for (int i = 0; i < NV; i++)
    for (int j = 0; j < NV; j++) {
        u32 s = vals[i], b = vals[j], r = 0;
        switch (ops[k].xo) {
        case 28: r = s & b; break;
        case 444: r = s | b; break;
        case 316: r = s ^ b; break;
        case 476: r = ~(s & b); break;
        case 124: r = ~(s | b); break;
        case 60: r = s & ~b; break;
        case 412: r = s | ~b; break;
        case 284: r = ~(s ^ b); break;
        }
        begin(NAME("%s%s %08x,%08x", ops[k].name, rc ? "." : "", s, b));
        C.r[3] = s; C.r[4] = b; C.cr = 0x02345678; C.xer = (i & 1) ? XER_SO : 0;
        emit(X(3, 5, 4, ops[k].xo, rc));
        if (!run()) continue;
        check("rA", C.r[5], r);
        check("cr", C.cr, rc ? (0x02345678 & 0x0FFFFFFF) | cr0_of(r, i & 1) << 28 : 0x02345678);
    }

    begin("andi./andis./ori/oris/xori/xoris"); C.r[3] = 0x8421F00F; C.cr = 0xFFFFFFFF;
    emit(D(28, 3, 4, 0xFF0F));   /* andi. r4,r3,0xFF0F */
    emit(D(29, 3, 5, 0x8000));   /* andis. r5,r3,0x8000 */
    emit(D(24, 3, 6, 0x0FF0));   /* ori */
    emit(D(25, 3, 7, 0x1234));   /* oris */
    emit(D(26, 3, 8, 0xFFFF));   /* xori */
    emit(D(27, 3, 9, 0xFFFF));   /* xoris */
    run();
    check("andi.", C.r[4], 0xF00F); check("andis.", C.r[5], 0x80000000);
    check("cr0 after andis. (neg)", C.cr >> 28, 8);
    check("cr1-7 untouched", C.cr & 0x0FFFFFFF, 0x0FFFFFFF);
    check("ori", C.r[6], 0x8421FFFF); check("oris", C.r[7], 0x9635F00F);
    check("xori", C.r[8], 0x84210FF0); check("xoris", C.r[9], 0x7BDEF00F);

    begin("andi. zero result -> EQ"); C.r[3] = 0xFFFF0000; C.xer = XER_SO;
    emit(D(28, 3, 4, 0xFFFF)); run();
    check("r4", C.r[4], 0); check("cr0", C.cr >> 28, 3);

    begin("ori/oris/xori don't touch cr0"); C.r[3] = 0; C.cr = 0x50000000;
    emit(D(24, 3, 4, 0)); emit(D(25, 3, 4, 0)); emit(D(26, 3, 4, 0)); run();
    check("cr", C.cr, 0x50000000);

    begin("mr (or rA,rS,rS)"); C.r[7] = 0xCAFEBABE;
    emit(X(7, 8, 7, 444, 0)); run();
    check("r8", C.r[8], 0xCAFEBABE);

    /* cntlzw / extsb / extsh */
    static const struct { u32 v, clz; } cz[] = { { 0, 32 }, { 1, 31 }, { 0x80000000, 0 }, { 0x00010000, 15 }, { 0x7FFFFFFF, 1 } };
    for (size_t i = 0; i < sizeof cz / sizeof cz[0]; i++) {
        begin(NAME("cntlzw. %08x", cz[i].v)); C.r[3] = cz[i].v;
        emit(X(3, 4, 0, 26, 1)); run();
        check("r4", C.r[4], cz[i].clz); check("cr0", C.cr >> 28, cz[i].clz ? 4 : 2);
    }
    static const struct { u32 v, sb, sh; } ex[] = {
        { 0x12345680, 0xFFFFFF80, 0x00005680 }, { 0x1234807F, 0x7F, 0xFFFF807F },
        { 0xFFFFFF00, 0, 0xFFFFFF00 }, { 0x0000FFFF, 0xFFFFFFFF, 0xFFFFFFFF },
    };
    for (size_t i = 0; i < sizeof ex / sizeof ex[0]; i++) {
        begin(NAME("extsb./extsh %08x", ex[i].v)); C.r[3] = ex[i].v;
        emit(X(3, 4, 0, 954, 1)); run();
        check("extsb", C.r[4], ex[i].sb); check("cr0", C.cr >> 28, cr0_of(ex[i].sb, 0));
        begin(NAME("extsh. %08x", ex[i].v)); C.r[3] = ex[i].v;
        emit(X(3, 4, 0, 922, 1)); run();
        check("extsh", C.r[4], ex[i].sh); check("cr0", C.cr >> 28, cr0_of(ex[i].sh, 0));
    }
}

/* ---- compare ---- */
static void test_compare(void) {
    for (int i = 0; i < NV; i++)
    for (int j = 0; j < NV; j++)
    for (int crf = 0; crf < 8; crf += 3) {
        u32 a = vals[i], b = vals[j];
        bool so = (i + j) & 1;
        u32 s = ((s32)a < (s32)b ? 8 : (s32)a > (s32)b ? 4 : 2) | so;
        u32 u = (a < b ? 8 : a > b ? 4 : 2) | so;
        int sh = 28 - 4 * crf;
        u32 crin = 0x13572468;
        begin(NAME("cmp/cmpl cr%d %08x,%08x", crf, a, b));
        C.r[3] = a; C.r[4] = b; C.xer = so ? XER_SO : 0; C.cr = crin;
        emit(cmp(crf, 3, 4)); emit(X(10, 0, 0, 19, 0)); /* mfcr r10 */
        emit(cmpl(crf, 3, 4)); emit(X(11, 0, 0, 19, 0));
        if (!run()) continue;
        check("cmp", C.r[10], (crin & ~(0xFu << sh)) | s << sh);
        check("cmpl", C.r[11], (crin & ~(0xFu << sh)) | u << sh);
    }
    static const struct { u32 a; int imm; } ci[] = {
        { 5, 5 }, { 5, -1 }, { 0xFFFFFFFF, -1 }, { 0x80000000, 0x7FFF }, { 0x8000, -0x8000 }, { 0, 1 },
    };
    for (size_t i = 0; i < sizeof ci / sizeof ci[0]; i++)
    for (int crf = 0; crf < 8; crf += 7) {
        u32 a = ci[i].a; s32 si = ci[i].imm; u32 ui = (u32)ci[i].imm & 0xFFFF;
        u32 s = (s32)a < si ? 8 : (s32)a > si ? 4 : 2;
        u32 u = a < ui ? 8 : a > ui ? 4 : 2;
        int sh = 28 - 4 * crf;
        begin(NAME("cmpi/cmpli cr%d %08x,%d", crf, a, si));
        C.r[3] = a;
        emit(cmpi(crf, 3, si)); emit(X(10, 0, 0, 19, 0));
        emit(cmpli(crf, 3, (int)ui)); emit(X(11, 0, 0, 19, 0));
        if (!run()) continue;
        check("cmpi", C.r[10], s << sh);
        check("cmpli (uimm zero-extended)", C.r[11], u << sh);
    }
}

/* ---- rotate / shift ---- */
static u32 ref_mask(int mb, int me) {
    u32 m = 0;
    for (int i = mb;; i = (i + 1) & 31) { m |= 0x80000000u >> i; if (i == me) break; }
    return m;
}
static u32 ref_rotl(u32 v, int n) {
    for (n &= 31; n; n--) v = v << 1 | v >> 31;
    return v;
}

static void test_rotate(void) {
    static const int shs[] = { 0, 1, 7, 16, 31 };
    static const int mbs[] = { 0, 1, 8, 16, 24, 31 };
    const u32 v = 0x9ABCDEF1, ins = 0x13572468;
    for (size_t s = 0; s < sizeof shs / sizeof shs[0]; s++)
    for (size_t m1 = 0; m1 < sizeof mbs / sizeof mbs[0]; m1++)
    for (size_t m2 = 0; m2 < sizeof mbs / sizeof mbs[0]; m2++) {
        int sh = shs[s], mb = mbs[m1], me = mbs[m2];
        u32 m = ref_mask(mb, me), rot = ref_rotl(v, sh);
        begin(NAME("rlwinm./rlwimi./rlwnm. sh=%d mb=%d me=%d", sh, mb, me));
        C.r[3] = v; C.r[5] = ins; C.r[6] = 0xFFFFFFE0u | (u32)sh; /* rlwnm uses only low 5 bits */
        emit(M(21, 3, 4, sh, mb, me, 1)); emit(X(8, 0, 0, 19, 0));
        emit(M(20, 3, 5, sh, mb, me, 1)); emit(X(9, 0, 0, 19, 0));
        emit(M(23, 3, 7, 6, mb, me, 1));  emit(X(10, 0, 0, 19, 0));
        if (!run()) continue;
        check("rlwinm", C.r[4], rot & m);
        check("rlwinm cr0", C.r[8] >> 28, cr0_of(rot & m, 0));
        check("rlwimi", C.r[5], (rot & m) | (ins & ~m));
        check("rlwimi cr0", C.r[9] >> 28, cr0_of((rot & m) | (ins & ~m), 0));
        check("rlwnm", C.r[7], rot & m);
        check("rlwnm cr0", C.r[10] >> 28, cr0_of(rot & m, 0));
    }
    begin("rlwinm no Rc keeps cr"); C.r[3] = 0; C.cr = 0x40000000;
    emit(M(21, 3, 4, 0, 0, 31, 0)); run();
    check("cr", C.cr, 0x40000000);

    /* familiar idioms */
    begin("slwi/srwi/clrlwi/rotlwi idioms"); C.r[3] = 0x12345678;
    emit(M(21, 3, 4, 4, 0, 27, 0));   /* slwi r4,r3,4 */
    emit(M(21, 3, 5, 28, 4, 31, 0));  /* srwi r5,r3,4 */
    emit(M(21, 3, 6, 0, 16, 31, 0));  /* clrlwi r6,r3,16 */
    emit(M(21, 3, 7, 8, 0, 31, 0));   /* rotlwi r7,r3,8 */
    emit(M(21, 3, 8, 0, 28, 3, 0));   /* mb>me wrap: keep top 4 and bottom 4 bits */
    run();
    check("slwi", C.r[4], 0x23456780); check("srwi", C.r[5], 0x01234567);
    check("clrlwi", C.r[6], 0x5678); check("rotlwi", C.r[7], 0x34567812);
    check("wrap mask", C.r[8], 0x10000008);

    begin("rlwimi insert (inslwi)"); C.r[3] = 0xAB; C.r[4] = 0x11223344;
    emit(M(20, 3, 4, 8, 16, 23, 0)); run();
    check("r4", C.r[4], 0x1122AB44);
}

static void test_shift(void) {
    static const u32 srcs[] = { 0x80000000, 0x80000001, 0xFFFFFFFF, 0x7FFFFFFF, 0xFFFFFFFE, 0x12345678, 0xF0000000, 0 };
    static const u32 amts[] = { 0, 1, 4, 31, 32, 33, 63, 64, 0xFFFFFFC1 };
    for (size_t i = 0; i < sizeof srcs / sizeof srcs[0]; i++)
    for (size_t j = 0; j < sizeof amts / sizeof amts[0]; j++)
    for (int same = 0; same <= 1; same++) {
        u32 s = srcs[i], b = amts[j], n = b & 63;
        u32 slw = n >= 32 ? 0 : s << n, srw = n >= 32 ? 0 : s >> n;
        s64 ss = (s32)s;
        u32 sraw = n >= 32 ? (u32)(ss >> 63) : (u32)(ss >> n);
        u32 lost = n >= 32 ? s : n ? (s & ((1u << n) - 1)) : 0;
        u32 ca = (ss < 0 && lost) ? XER_CA : 0;
        int d = same ? 3 : 5;   /* same: rA == rS */
        begin(NAME("slw/srw/sraw %08x by %u%s", s, b, same ? " (rA==rS)" : ""));
        C.r[3] = s; C.r[4] = b;
        emit(X(3, d, 4, 24, 1)); emit(X(10, 0, 0, 19, 0));
        run(); check("slw", C.r[d], slw); check("slw. cr0", C.r[10] >> 28, cr0_of(slw, 0));

        begin(g_name); C.r[3] = s; C.r[4] = b;
        emit(X(3, d, 4, 536, 1)); emit(X(10, 0, 0, 19, 0));
        run(); check("srw", C.r[d], srw); check("srw. cr0", C.r[10] >> 28, cr0_of(srw, 0));

        begin(g_name); C.r[3] = s; C.r[4] = b; C.xer = ca ? 0 : XER_CA;
        emit(X(3, d, 4, 792, 1)); emit(X(10, 0, 0, 19, 0));
        run(); check("sraw", C.r[d], sraw); check("sraw CA", C.xer & XER_CA, ca);
        check("sraw. cr0", C.r[10] >> 28, cr0_of(sraw, 0));

        if (b < 32) {
            begin(NAME("srawi %08x by %u%s", s, b, same ? " (rA==rS)" : ""));
            C.r[3] = s; C.xer = ca ? 0 : XER_CA;
            emit(X(3, d, (int)b, 824, 1)); emit(X(10, 0, 0, 19, 0));
            run(); check("srawi", C.r[d], sraw); check("srawi CA", C.xer & XER_CA, ca);
            check("srawi. cr0", C.r[10] >> 28, cr0_of(sraw, 0));
        }
    }
    /* signed division by 4 idiom: srawi + addze (rA == rS, as CodeWarrior emits it) */
    static const s32 dv[] = { -9, -8, -7, -2, -1, 0, 1, 7, 8, INT32_MIN };
    for (size_t i = 0; i < sizeof dv / sizeof dv[0]; i++) {
        begin(NAME("srawi/addze div4 %d", dv[i])); C.r[3] = (u32)dv[i];
        emit(X(3, 3, 2, 824, 0)); emit(XO(3, 3, 0, 0, ADDZE, 0)); run();
        check("r3", C.r[3], (u32)(dv[i] / 4));
    }
}

/* ---- loads / stores ---- */
static void test_loadstore(void) {
    static const u8 pat[16] = { 0x80, 0x01, 0xFE, 0x7F, 0x12, 0x34, 0x56, 0x78, 0x9A, 0xBC, 0xDE, 0xF0, 0x00, 0x11, 0x22, 0x33 };
    gmemcpy_to(DATA, pat, 16);

    begin("lbz/lhz/lha/lwz"); C.r[10] = DATA;
    emit(D(34, 3, 10, 0));  /* lbz r3,0(r10) */
    emit(D(40, 4, 10, 0));  /* lhz r4,0(r10) */
    emit(D(42, 5, 10, 0));  /* lha r5,0(r10) -> sign-extended */
    emit(D(42, 6, 10, 2));  /* lha r6,2(r10) */
    emit(D(32, 7, 10, 4));  /* lwz r7,4(r10) */
    emit(D(42, 8, 10, 4));  /* lha r8,4(r10) positive */
    emit(D(34, 9, 10, 3));  /* lbz 0x7F */
    run();
    check("lbz", C.r[3], 0x80); check("lhz", C.r[4], 0x8001);
    check("lha neg", C.r[5], 0xFFFF8001); check("lha neg2", C.r[6], 0xFFFFFE7F);
    check("lwz", C.r[7], 0x12345678); check("lha pos", C.r[8], 0x1234); check("lbz", C.r[9], 0x7F);

    begin("negative displacement"); C.r[10] = DATA + 8;
    emit(D(32, 3, 10, -4));
    run();
    check("lwz -4", C.r[3], 0x12345678);

    begin("update forms lbzu/lhzu/lhau/lwzu"); C.r[10] = DATA; C.r[11] = DATA; C.r[12] = DATA; C.r[13] = DATA;
    emit(D(35, 3, 10, 3)); emit(D(41, 4, 11, 2)); emit(D(43, 5, 12, 0)); emit(D(33, 6, 13, 8));
    run();
    check("lbzu", C.r[3], 0x7F); check("lbzu ea", C.r[10], DATA + 3);
    check("lhzu", C.r[4], 0xFE7F); check("lhzu ea", C.r[11], DATA + 2);
    check("lhau", C.r[5], 0xFFFF8001); check("lhau ea", C.r[12], DATA);
    check("lwzu", C.r[6], 0x9ABCDEF0); check("lwzu ea", C.r[13], DATA + 8);

    begin("indexed loads"); C.r[10] = DATA; C.r[11] = 4; C.r[0] = 0x77777777;
    emit(X(3, 10, 11, 87, 0));   /* lbzx */
    emit(X(4, 10, 11, 279, 0));  /* lhzx */
    emit(X(5, 11, 10, 343, 0));  /* lhax (rA/rB swapped: same EA) */
    emit(X(6, 10, 11, 23, 0));   /* lwzx */
    emit(X(7, 0, 10, 23, 0));    /* lwzx rA=0 -> EA = rB */
    emit(X(8, 10, 11, 534, 0));  /* lwbrx */
    emit(X(9, 10, 11, 790, 0));  /* lhbrx */
    run();
    check("lbzx", C.r[3], 0x12); check("lhzx", C.r[4], 0x1234); check("lhax", C.r[5], 0x1234);
    check("lwzx", C.r[6], 0x12345678); check("lwzx rA=0", C.r[7], 0x8001FE7F);
    check("lwbrx", C.r[8], 0x78563412); check("lhbrx", C.r[9], 0x3412);

    begin("indexed update loads"); C.r[10] = DATA; C.r[11] = 8; C.r[12] = DATA; C.r[13] = DATA; C.r[14] = DATA;
    emit(X(3, 10, 11, 55, 0));   /* lwzux */
    emit(X(4, 12, 11, 119, 0));  /* lbzux */
    emit(X(5, 13, 11, 311, 0));  /* lhzux */
    emit(X(6, 14, 11, 375, 0));  /* lhaux */
    run();
    check("lwzux", C.r[3], 0x9ABCDEF0); check("lwzux ea", C.r[10], DATA + 8);
    check("lbzux", C.r[4], 0x9A); check("lhzux", C.r[5], 0x9ABC); check("lhaux", C.r[6], 0xFFFF9ABC);
    check("lhaux ea", C.r[14], DATA + 8);

    const u32 B = DATA + 0x100;
    gmemset(B, 0xEE, 64);
    begin("stb/sth/stw and update forms"); C.r[10] = B; C.r[11] = B + 16; C.r[12] = B + 32; C.r[13] = B + 48;
    C.r[3] = 0x11223344;
    emit(D(38, 3, 10, 0));   /* stb */
    emit(D(44, 3, 10, 2));   /* sth */
    emit(D(36, 3, 10, 4));   /* stw */
    emit(D(39, 3, 11, 1));   /* stbu */
    emit(D(45, 3, 12, 2));   /* sthu */
    emit(D(37, 3, 13, -4));  /* stwu */
    run();
    check("stb", rd8(B), 0x44); check("stb neighbour", rd8(B + 1), 0xEE);
    check("sth", rd16(B + 2), 0x3344); check("stw", rd32(B + 4), 0x11223344);
    check("stbu", rd8(B + 17), 0x44); check("stbu ea", C.r[11], B + 17);
    check("sthu", rd16(B + 34), 0x3344); check("sthu ea", C.r[12], B + 34);
    check("stwu", rd32(B + 44), 0x11223344); check("stwu ea", C.r[13], B + 44);

    gmemset(B, 0xEE, 64);
    begin("indexed stores"); C.r[10] = B; C.r[11] = 8; C.r[3] = 0xA1B2C3D4;
    C.r[12] = B + 16; C.r[13] = B + 32; C.r[14] = B + 48;
    emit(X(3, 10, 11, 151, 0));  /* stwx  B+8 */
    emit(X(3, 0, 12, 215, 0));   /* stbx rA=0  B+16 */
    emit(X(3, 10, 0, 407, 0));   /* sthx  B + r0 (r0 = 0) */
    emit(X(3, 13, 11, 662, 0));  /* stwbrx B+40 */
    emit(X(3, 14, 11, 918, 0));  /* sthbrx B+56 */
    run();
    check("stwx", rd32(B + 8), 0xA1B2C3D4); check("stbx", rd8(B + 16), 0xD4);
    check("sthx", rd16(B), 0xC3D4); check("stwbrx", rd32(B + 40), 0xD4C3B2A1);
    check("sthbrx", rd16(B + 56), 0xD4C3);

    begin("indexed update stores"); C.r[10] = B; C.r[11] = B; C.r[12] = B; C.r[4] = 12; C.r[3] = 0x01020304;
    emit(X(3, 10, 4, 183, 0));   /* stwux */
    emit(X(3, 11, 4, 247, 0));   /* stbux */
    emit(X(3, 12, 4, 439, 0));   /* sthux */
    run();
    check("stwux ea", C.r[10], B + 12); check("stbux ea", C.r[11], B + 12); check("sthux ea", C.r[12], B + 12);
    check("mem", rd32(B + 12), 0x03040304);

    begin("lmw/stmw"); C.r[10] = B;
    for (int r = 26; r < 32; r++) C.r[r] = 0x1000u * (u32)r + (u32)r;
    emit(D(47, 26, 10, 0));    /* stmw r26,0(r10) */
    emit(D(46, 28, 10, 4));    /* lmw r28,4(r10): r28..r31 <- r27..r30 */
    run();
    for (int r = 26; r < 32; r++) check(NAME("stmw mem[r%d]", r), rd32(B + 4 * (r - 26)), 0x1000u * (u32)r + (u32)r);
    for (int r = 28; r < 32; r++) check(NAME("lmw r%d", r), C.r[r], 0x1000u * (u32)(r - 1) + (u32)(r - 1));
    check("lmw leaves r27", C.r[27], 0x1000u * 27 + 27);
}

/* ---- branches ---- */
static void test_branch(void) {
    begin("b forward");
    emit(li(3, 1)); emit(enc_b(here() + 8, 0, 0, here())); emit(li(3, 2)); emit(li(4, 3));
    run(); check("r3", C.r[3], 1); check("r4", C.r[4], 3);

    begin("b backward + bl/lr");
    /* 0: b 3 ; 1: li r5,7 ; 2: blr ; 3: bl 1 ; 4: mflr r6 */
    emit(enc_b(CODE_ADDR + 12, 0, 0, here()));
    emit(li(5, 7));
    emit(XL(20, 0, 0, 16, 0));          /* blr */
    emit(enc_b(CODE_ADDR + 4, 0, 1, here())); /* bl */
    emit(mfspr(6, 8));
    run(); check("r5", C.r[5], 7); check("lr after bl", C.r[6], CODE_ADDR + 16);

    begin("bdnz loop, ctr=5");
    C.r[4] = 5;
    emit(li(3, 0)); emit(mtspr(9, 4));
    u32 loop = here();
    emit(D(14, 3, 3, 1));
    emit(enc_bc(16, 0, loop, here(), 0));  /* bdnz */
    run(); check("iterations", C.r[3], 5); check("ctr", C.ctr, 0);

    begin("bdnz with ctr=0 wraps (2^32 iterations not run: bdz instead)");
    C.ctr = 1;
    emit(enc_bc(18, 0, here() + 8, here(), 0));   /* bdz: ctr 1->0, taken */
    emit(li(3, 1));
    emit(li(4, 1));
    run(); check("r3 skipped", C.r[3], 0); check("r4", C.r[4], 1); check("ctr", C.ctr, 0);

    begin("bdz not taken when ctr != 0 after decrement");
    C.ctr = 2;
    emit(enc_bc(18, 0, here() + 8, here(), 0));
    emit(li(3, 1));
    run(); check("r3", C.r[3], 1); check("ctr", C.ctr, 1);

    begin("bdnz decrements 0 -> 0xFFFFFFFF and branches");
    C.ctr = 0;
    emit(enc_bc(16, 0, here() + 8, here(), 0));
    emit(li(3, 1));
    run(); check("r3 skipped", C.r[3], 0); check("ctr", C.ctr, 0xFFFFFFFF);

    /* conditional branches on CR bits */
    static const struct { const char *name; int bo, bitoff; u32 crf_val; bool taken; } cb[] = {
        { "beq taken", 12, 2, 2, 1 }, { "beq not", 12, 2, 4, 0 }, { "bne taken", 4, 2, 8, 1 },
        { "bne not", 4, 2, 2, 0 }, { "blt taken", 12, 0, 8, 1 }, { "bge taken", 4, 0, 4, 1 },
        { "bge not", 4, 0, 8, 0 }, { "bgt taken", 12, 1, 4, 1 }, { "ble taken", 4, 1, 2, 1 },
        { "bso taken", 12, 3, 1, 1 }, { "bns not", 4, 3, 1, 0 }, { "b always (bo=20)", 20, 0, 0, 1 },
    };
    for (size_t i = 0; i < sizeof cb / sizeof cb[0]; i++)
    for (int crf = 0; crf < 8; crf += 7) {
        begin(NAME("%s cr%d", cb[i].name, crf));
        C.cr = (cb[i].crf_val << (28 - 4 * crf)) | (crf ? 0x0F000000 : 0x00000F00) /* noise elsewhere */;
        emit(enc_bc(cb[i].bo, crf * 4 + cb[i].bitoff, here() + 8, here(), 0));
        emit(li(3, 1));
        run(); check("fallthrough executed", C.r[3], cb[i].taken ? 0 : 1); check("ctr untouched", C.ctr, 0);
    }

    begin("bdnzt / bdnzf / bdzt");
    C.cr = 0x20000000; C.ctr = 3;  /* cr0.eq */
    emit(enc_bc(8, 2, here() + 8, here(), 0));   /* bdnzt eq: ctr 2, taken */
    emit(li(3, 1));
    emit(enc_bc(0, 2, here() + 8, here(), 0));   /* bdnzf eq: ctr 1, cond false -> not taken */
    emit(li(4, 1));
    emit(enc_bc(10, 2, here() + 8, here(), 0));  /* bdzt eq: ctr 0, taken */
    emit(li(5, 1));
    run();
    check("bdnzt", C.r[3], 0); check("bdnzf", C.r[4], 1); check("bdzt", C.r[5], 0); check("ctr", C.ctr, 0);

    begin("bcl sets LR even if not taken");
    C.cr = 0;
    u32 at = here();
    emit(enc_bc(12, 2, here() + 8, here(), 1));   /* beql, not taken */
    emit(mfspr(3, 8));
    run(); check("lr", C.r[3], at + 4);

    /* bclr / bcctr */
    begin("beqlr taken / bnelr not");
    C.cr = 0x20000000;
    emit(D(15, 5, 0, (int)(CODE_ADDR >> 16)));    /* lis r5 */
    emit(D(24, 5, 5, 6 * 4));                     /* ori r5,r5,24 -> index 6 */
    emit(mtspr(8, 5));
    emit(XL(4, 2, 0, 16, 0));                     /* bnelr: not taken */
    emit(XL(12, 2, 0, 16, 0));                    /* beqlr: taken to index 6 */
    emit(li(3, 1));
    emit(li(4, 1));                               /* index 6 */
    run(); check("skipped", C.r[3], 0); check("target", C.r[4], 1);

    begin("blrl: branch to old LR, LR = next");
    emit(D(15, 5, 0, (int)(CODE_ADDR >> 16)));
    emit(D(24, 5, 5, 5 * 4));
    emit(mtspr(8, 5));
    emit(XL(20, 0, 0, 16, 1));                    /* blrl (index 3) */
    emit(li(3, 1));
    emit(mfspr(4, 8));                            /* index 5 */
    run(); check("skipped", C.r[3], 0); check("lr", C.r[4], CODE_ADDR + 16);

    begin("bdnzlr decrements ctr");
    C.ctr = 2;
    emit(D(15, 5, 0, (int)(CODE_ADDR >> 16)));
    emit(D(24, 5, 5, 5 * 4));
    emit(mtspr(8, 5));
    emit(XL(16, 0, 0, 16, 0));                    /* bdnzlr: ctr 1, taken */
    emit(li(3, 1));
    run(); check("skipped", C.r[3], 0); check("ctr", C.ctr, 1);

    begin("bctr / bctrl / beqctr do not touch CTR");
    emit(D(15, 5, 0, (int)(CODE_ADDR >> 16)));
    emit(D(24, 5, 5, 5 * 4));
    emit(mtspr(9, 5));
    emit(XL(20, 0, 0, 528, 1));                   /* bctrl (index 3) */
    emit(li(3, 1));
    emit(mfspr(4, 8));                            /* 5 */
    emit(mfspr(6, 9));
    emit(D(24, 5, 5, 0));                         /* nop-ish */
    emit(D(15, 7, 0, (int)(CODE_ADDR >> 16)));
    emit(D(24, 7, 7, 13 * 4));
    emit(mtspr(9, 7));                            /* 10 */
    emit(XL(12, 2, 0, 528, 0));                   /* beqctr, cr0.eq=0 -> not taken */
    emit(li(8, 1));
    emit(nop());                                  /* 13 */
    run();
    check("skipped", C.r[3], 0); check("lr", C.r[4], CODE_ADDR + 16);
    check("ctr unchanged", C.r[6], CODE_ADDR + 20); check("beqctr not taken", C.r[8], 1);
    check("ctr final", C.ctr, CODE_ADDR + 52);
}

/* ---- CR / SPR moves ---- */
static void test_cr_spr(void) {
    begin("mtcrf/mfcr");
    C.cr = 0x12345678; C.r[3] = 0xABCDEF01;
    emit(31u << 26 | 3u << 21 | 0x81u << 12 | 144u << 1);  /* mtcrf 0x81 (cr0, cr7) */
    emit(X(4, 0, 0, 19, 0));
    emit(31u << 26 | 3u << 21 | 0xFFu << 12 | 144u << 1);  /* mtcr */
    emit(X(5, 0, 0, 19, 0));
    run(); check("mtcrf 0x81", C.r[4], 0xA2345671); check("mtcr", C.r[5], 0xABCDEF01);

    static const struct { const char *name; int xo; int tt[4]; } crops[] = {
        { "crand", 257, { 0, 0, 0, 1 } }, { "cror", 449, { 0, 1, 1, 1 } }, { "crxor", 193, { 0, 1, 1, 0 } },
        { "crnand", 225, { 1, 1, 1, 0 } }, { "crnor", 33, { 1, 0, 0, 0 } }, { "creqv", 289, { 1, 0, 0, 1 } },
        { "crandc", 129, { 0, 0, 1, 0 } }, { "crorc", 417, { 1, 0, 1, 1 } },
    };
    for (size_t k = 0; k < sizeof crops / sizeof crops[0]; k++)
    for (int ab = 0; ab < 4; ab++) {
        int a = ab >> 1, b = ab & 1;
        begin(NAME("%s a=%d b=%d", crops[k].name, a, b));
        /* bA = bit 5 (cr1.gt), bB = bit 30 (cr7.eq), bT = bit 17 (cr4.gt); fill T with the complement */
        u32 cr = 0;
        if (a) cr |= 1u << (31 - 5);
        if (b) cr |= 1u << (31 - 30);
        if (!crops[k].tt[ab]) cr |= 1u << (31 - 17);
        C.cr = cr | 0x80000000;
        emit(XL(17, 5, 30, crops[k].xo, 0));
        run();
        u32 exp = (cr & ~(1u << (31 - 17))) | (crops[k].tt[ab] ? 1u << (31 - 17) : 0) | 0x80000000;
        check("cr", C.cr, exp);
    }
    begin("crxor/creqv same bit (crclr/crset)");
    C.cr = 0xFFFFFFFF;
    emit(XL(6, 6, 6, 193, 0)); emit(XL(9, 9, 9, 289, 0)); run();
    check("cr", C.cr, 0xFDFFFFFF);

    begin("mcrf");
    C.cr = 0x12345678;
    emit(19u << 26 | 6u << 23 | 1u << 18);  /* mcrf cr6,cr1 */
    emit(19u << 26 | 0u << 23 | 7u << 18);  /* mcrf cr0,cr7 */
    run(); check("cr", C.cr, 0x82345628);

    begin("mcrxr");
    C.cr = 0; C.xer = 0xE000007F;
    emit(X(3 << 2, 0, 0, 512, 0)); run();
    check("cr3", C.cr, 0x000E0000); check("xer", C.xer, 0x7F);

    begin("mtlr/mflr/mtctr/mfctr/mtxer/mfxer");
    C.r[3] = 0x11111110; C.r[4] = 0x22222222; C.r[5] = 0xE0000012;
    emit(mtspr(8, 3)); emit(mtspr(9, 4)); emit(mtspr(1, 5));
    emit(mfspr(6, 8)); emit(mfspr(7, 9)); emit(mfspr(8, 1));
    run();
    check("lr", C.r[6], 0x11111110); check("ctr", C.r[7], 0x22222222); check("xer", C.r[8], 0xE0000012);
    check("C.xer", C.xer, 0xE0000012);

    begin("mtxer CA consumed by adde");
    C.r[5] = XER_CA; C.r[3] = 1; C.r[4] = 1;
    emit(mtspr(1, 5)); emit(XO(6, 3, 4, 0, ADDE, 0)); run();
    check("r6", C.r[6], 3);
}

/* ---- floating point ---- */
static void wrd(u32 a, double d) { FPR f = { .d = d }; wr64(a, f.u); }
static double rdd(u32 a) { FPR f; f.u = rd64(a); return f.d; }
static void wrf(u32 a, float f) { u32 v; memcpy(&v, &f, 4); wr32(a, v); }

static void test_fp_loadstore(void) {
    const u32 B = DATA + 0x400;
    gmemset(B, 0, 0x100);
    wrf(B, 1.5f); wrf(B + 4, -0.1f); wrd(B + 8, 3.141592653589793); wr64(B + 16, 0x7FF4000000000001ull); /* sNaN */
    wr32(B + 24, 0x00000001); /* smallest float denormal */

    begin("lfs/lfd/lfsu/lfdu"); C.r[10] = B; C.r[11] = B; C.r[12] = B;
    emit(D(48, 1, 10, 0)); emit(D(48, 2, 10, 4)); emit(D(50, 3, 10, 8)); emit(D(50, 4, 10, 16));
    emit(D(49, 5, 11, 4)); emit(D(51, 6, 12, 8)); emit(D(48, 7, 10, 24));
    run();
    checkd("lfs 1.5", C.f[1].d, 1.5); checkd("lfs -0.1f", C.f[2].d, (double)-0.1f);
    checkd("lfd", C.f[3].d, 3.141592653589793); check("lfd sNaN bit-exact", C.f[4].u, 0x7FF4000000000001ull);
    checkd("lfsu", C.f[5].d, (double)-0.1f); check("lfsu ea", C.r[11], B + 4);
    checkd("lfdu", C.f[6].d, 3.141592653589793); check("lfdu ea", C.r[12], B + 8);
    checkd("lfs denormal", C.f[7].d, ldexp(1.0, -149));

    begin("stfs/stfd/stfsu/stfdu"); C.r[10] = B + 0x40; C.r[11] = B + 0x40; C.r[12] = B + 0x40;
    C.f[1].d = 0.1; C.f[2].d = -2.5; C.f[3].d = 1e300; C.f[4].u = 0xFFF8000012345678ull;
    emit(D(52, 1, 10, 0));    /* stfs 0.1 -> 0x3DCCCCCD */
    emit(D(54, 2, 10, 8));    /* stfd */
    emit(D(52, 3, 10, 16));   /* stfs overflow -> +inf */
    emit(D(54, 4, 10, 24));   /* stfd NaN bit-exact */
    emit(D(53, 2, 11, 32));   /* stfsu */
    emit(D(55, 1, 12, 40));   /* stfdu */
    run();
    check("stfs 0.1", rd32(B + 0x40), 0x3DCCCCCD); checkd("stfd", rdd(B + 0x48), -2.5);
    check("stfs inf", rd32(B + 0x50), 0x7F800000); check("stfd nan", rd64(B + 0x58), 0xFFF8000012345678ull);
    check("stfsu", rd32(B + 0x60), 0xC0200000); check("stfsu ea", C.r[11], B + 0x60);
    checkd("stfdu", rdd(B + 0x68), 0.1); check("stfdu ea", C.r[12], B + 0x68);

    begin("indexed fp load/store"); C.r[10] = B; C.r[11] = 8; C.r[12] = B; C.r[13] = B + 0x80; C.r[14] = B + 0x80; C.r[15] = B + 0x80;
    emit(X(1, 10, 11, 599, 0));    /* lfdx f1 = pi */
    emit(X(2, 0, 10, 535, 0));     /* lfsx f2 = 1.5 (rA=0) */
    emit(X(3, 12, 11, 631, 0));    /* lfdux */
    emit(X(1, 13, 0, 727, 0));     /* stfdx f1 -> B+0x80 */
    emit(X(2, 13, 11, 663, 0));    /* stfsx f2 -> B+0x88 */
    emit(X(1, 14, 11, 695, 0));    /* stfsux -> B+0x88 (overwrites) */
    emit(X(2, 15, 11, 759, 0));    /* stfdux f2 -> B+0x88 */
    run();
    checkd("lfdx", C.f[1].d, 3.141592653589793); checkd("lfsx", C.f[2].d, 1.5);
    checkd("lfdux", C.f[3].d, 3.141592653589793); check("lfdux ea", C.r[12], B + 8);
    checkd("stfdx", rdd(B + 0x80), 3.141592653589793);
    check("stfsux ea", C.r[14], B + 0x88); check("stfdux ea", C.r[15], B + 0x88);
    checkd("stfdux", rdd(B + 0x88), 1.5);

    begin("lfsux/stfsx"); C.r[10] = B; C.r[11] = 4; C.r[12] = B + 0xA0;
    emit(X(1, 10, 11, 567, 0)); emit(X(1, 12, 0, 663, 0)); run();
    checkd("lfsux", C.f[1].d, (double)-0.1f); check("lfsux ea", C.r[10], B + 4);
    check("stfsx", rd32(B + 0xA0), 0xBDCCCCCD);

    begin("stfiwx"); C.r[10] = B + 0xB0; C.f[1].u = 0xFFF80000DEADBEEFull;
    emit(X(1, 0, 10, 983, 0)); run();
    check("stfiwx", rd32(B + 0xB0), 0xDEADBEEF);
}

static void test_fp_arith(void) {
    const double p30 = ldexp(1.0, -30);
    /* double and single arithmetic */
    static const struct { const char *name; int op, xo; bool usec; } ar[] = {
        { "fadd", 63, 21, 0 }, { "fsub", 63, 20, 0 }, { "fmul", 63, 25, 1 }, { "fdiv", 63, 18, 0 },
        { "fadds", 59, 21, 0 }, { "fsubs", 59, 20, 0 }, { "fmuls", 59, 25, 1 }, { "fdivs", 59, 18, 0 },
    };
    static const double av[][2] = { { 1.0, 3.0 }, { 1.0 + 1e-10, 1e-12 }, { -2.5, 0.1 }, { 1e30, 1e-30 },
                                    { 3.4e38, 10.0 }, { 0.0, -0.0 }, { 1.0, 0.0 } };
    for (size_t k = 0; k < sizeof ar / sizeof ar[0]; k++)
    for (size_t i = 0; i < sizeof av / sizeof av[0]; i++) {
        double a = av[i][0], b = av[i][1], r = 0;
        switch (ar[k].xo) {
        case 21: r = a + b; break;
        case 20: r = a - b; break;
        case 25: r = a * b; break;
        case 18: r = a / b; break;
        }
        if (ar[k].op == 59) r = (double)(float)r;
        begin(NAME("%s %g,%g", ar[k].name, a, b));
        C.f[1].d = a; C.f[2].d = b;
        emit(FA(ar[k].op, 3, 1, ar[k].usec ? 0 : 2, ar[k].usec ? 2 : 0, ar[k].xo, 0));
        if (run()) checkd("f3", C.f[3].d, r);
    }

    begin("fadds rounds to single, fadd doesn't");
    C.f[1].d = 1.0; C.f[2].d = p30;
    emit(FA(59, 3, 1, 2, 0, 21, 0)); emit(FA(63, 4, 1, 2, 0, 21, 0)); run();
    checkd("fadds", C.f[3].d, 1.0); checkd("fadd", C.f[4].d, 1.0 + p30);

    /* fused multiply-add: a*c exactly = 1 - 2^-60, so the fused result differs from a*c+b rounded */
    static const struct { const char *name; int xo; double exp; } fm[] = {
        { "fmadd", 29, -0x1p-60 }, { "fmsub", 28, -0x1p-60 }, { "fnmadd", 31, 0x1p-60 }, { "fnmsub", 30, 0x1p-60 },
    };
    for (size_t k = 0; k < sizeof fm / sizeof fm[0]; k++) {
        begin(NAME("%s fused", fm[k].name));
        C.f[1].d = 1.0 + p30; C.f[2].d = 1.0 - p30;
        C.f[3].d = (fm[k].xo == 29 || fm[k].xo == 31) ? -1.0 : 1.0;
        emit(FA(63, 4, 1, 3, 2, fm[k].xo, 0));   /* frD=4, frA=1, frB=3, frC=2 */
        run(); checkd("f4", C.f[4].d, fm[k].exp);
    }
    static const struct { const char *name; int xo; double exp; } fm2[] = {
        { "fmadd", 29, 2 * 3 + 0.5 }, { "fmsub", 28, 2 * 3 - 0.5 }, { "fnmadd", 31, -(2 * 3 + 0.5) }, { "fnmsub", 30, -(2 * 3 - 0.5) },
    };
    for (size_t k = 0; k < sizeof fm2 / sizeof fm2[0]; k++) {
        begin(NAME("%s 2*3,0.5 (operand order frA*frC+frB)", fm2[k].name));
        C.f[1].d = 2; C.f[2].d = 3; C.f[3].d = 0.5;
        emit(FA(63, 4, 1, 3, 2, fm2[k].xo, 0));
        emit(FA(59, 5, 1, 3, 2, fm2[k].xo, 0));
        run(); checkd("double", C.f[4].d, fm2[k].exp); checkd("single", C.f[5].d, fm2[k].exp);
    }
    begin("fmadds rounds to single");
    C.f[1].d = 1.0; C.f[2].d = 1.0; C.f[3].d = p30;
    emit(FA(59, 4, 1, 3, 2, 29, 0)); run();
    checkd("f4", C.f[4].d, 1.0);

    begin("fabs/fneg/fnabs/fmr");
    C.f[1].d = -0.0; C.f[2].u = 0xFFF8000000000123ull; C.f[3].d = 2.5;
    emit(FX(4, 0, 1, 264, 0)); emit(FX(5, 0, 1, 40, 0)); emit(FX(6, 0, 2, 264, 0));
    emit(FX(7, 0, 3, 136, 0)); emit(FX(8, 0, 2, 72, 0)); emit(FX(9, 0, 3, 40, 0));
    run();
    check("fabs -0", C.f[4].u, 0); check("fneg -0", C.f[5].u, 0);
    check("fabs NaN", C.f[6].u, 0x7FF8000000000123ull); checkd("fnabs", C.f[7].d, -2.5);
    check("fmr", C.f[8].u, 0xFFF8000000000123ull); checkd("fneg", C.f[9].d, -2.5);

    /* frsp: round-to-nearest-even at single precision */
    static const struct { double in, out; } rs[] = {
        { 1.0 + 0x1p-24, 1.0 },                        /* tie -> even (down) */
        { 1.0 + 0x1p-24 + 0x1p-40, 1.0 + 0x1p-23 },    /* above tie -> up */
        { 1.0 + 3 * 0x1p-24, 1.0 + 0x1p-22 },          /* tie -> even (up) */
        { -(1.0 + 3 * 0x1p-24), -(1.0 + 0x1p-22) },
        { 0.1, (double)0.1f }, { 1e300, INFINITY }, { -1e300, -INFINITY }, { 1e-50, 0.0 },
        { 3.4028235e38, 3.4028234663852886e38 },       /* below the FLT_MAX/2^128 tie -> FLT_MAX */
        { 3.4028235677973366e38, INFINITY },           /* exactly the tie -> even (2^128) -> inf */
    };
    for (size_t i = 0; i < sizeof rs / sizeof rs[0]; i++) {
        begin(NAME("frsp %.17g", rs[i].in)); C.f[1].d = rs[i].in;
        emit(FX(2, 0, 1, 12, 0)); run();
        checkd("f2", C.f[2].d, rs[i].out);
    }

    /* fctiw (round to nearest even with RN=0) / fctiwz (truncate), saturating */
    static const struct { double in; u32 w, wz; } ct[] = {
        { 2.5, 2, 2 }, { 3.5, 4, 3 }, { -2.5, (u32)-2, (u32)-2 }, { -2.7, (u32)-3, (u32)-2 },
        { 0.49999999999999994, 0, 0 }, { -0.9, (u32)-1, 0 }, { 1e10, 0x7FFFFFFF, 0x7FFFFFFF },
        { -1e10, 0x80000000, 0x80000000 }, { 2147483647.4, 0x7FFFFFFF, 0x7FFFFFFF },
        { 2147483647.5, 0x7FFFFFFF, 0x7FFFFFFF }, { -2147483648.9, 0x80000000, 0x80000000 },
        { -2147483647.9, 0x80000000, 0x80000001 }, { NAN, 0x80000000, 0x80000000 },
        { INFINITY, 0x7FFFFFFF, 0x7FFFFFFF }, { -INFINITY, 0x80000000, 0x80000000 }, { -0.0, 0, 0 },
    };
    for (size_t i = 0; i < sizeof ct / sizeof ct[0]; i++) {
        begin(NAME("fctiw/fctiwz %.17g", ct[i].in)); C.f[1].d = ct[i].in;
        emit(FX(2, 0, 1, 14, 0)); emit(FX(3, 0, 1, 15, 0)); run();
        check("fctiw", (u32)C.f[2].u, ct[i].w); check("fctiwz", (u32)C.f[3].u, ct[i].wz);
    }

    /* fctiw under other rounding modes (set with mtfsfi 7,RN), then restored */
    static const struct { int rn; double in; u32 w; } cm[] = {
        { 1, 2.7, 2 }, { 1, -2.7, (u32)-2 }, { 2, 2.1, 3 }, { 2, -2.9, (u32)-2 }, { 3, 2.9, 2 }, { 3, -2.1, (u32)-3 },
    };
    for (size_t i = 0; i < sizeof cm / sizeof cm[0]; i++) {
        begin(NAME("fctiw RN=%d %g", cm[i].rn, cm[i].in)); C.f[1].d = cm[i].in;
        emit(63u << 26 | 7u << 23 | (u32)cm[i].rn << 12 | 134u << 1);   /* mtfsfi 7,rn */
        emit(FX(2, 0, 1, 14, 0));
        emit(63u << 26 | 7u << 23 | 0u << 12 | 134u << 1);              /* mtfsfi 7,0 */
        run(); check("fctiw", (u32)C.f[2].u, cm[i].w); check("fpscr restored", C.fpscr & 3, 0);
    }

    /* rounding mode affects arithmetic */
    begin("fadd under RN=toward +inf (mtfsf) / mffs");
    C.f[1].d = 1.0; C.f[2].d = 0x1p-60; C.f[3].u = 0xFFF8000000000002ull;
    emit(63u << 26 | 0x01u << 17 | 3u << 11 | 711u << 1);   /* mtfsf 0x01,f3 -> RN=2 */
    emit(FA(63, 4, 1, 2, 0, 21, 0));                       /* fadd f4 = nextafter(1, 2) */
    emit(FX(5, 0, 0, 583, 0));                             /* mffs f5 */
    emit(FA(63, 6, 1, 2, 0, 20, 0));                       /* fsub f6: RU -> 1.0 */
    emit(63u << 26 | 7u << 23 | 0u << 12 | 134u << 1);     /* mtfsfi 7,0 */
    emit(FA(63, 7, 1, 2, 0, 21, 0));                       /* fadd f7: RN -> 1.0 */
    run();
    checkd("fadd RU", C.f[4].d, nextafter(1.0, 2.0)); check("mffs", (u32)C.f[5].u, 2);
    checkd("fsub RU", C.f[6].d, 1.0); checkd("fadd RN", C.f[7].d, 1.0);

    begin("mtfsb1/mtfsb0 on RN bits");
    emit(FX(31, 0, 0, 38, 0)); emit(FX(5, 0, 0, 583, 0)); emit(FX(31, 0, 0, 70, 0)); emit(FX(6, 0, 0, 583, 0));
    run(); check("mtfsb1 31", (u32)C.f[5].u, 1); check("mtfsb0 31", (u32)C.f[6].u, 0);

    /* fcmpu/fcmpo into CR fields (and FPCC) */
    static const struct { double a, b; u32 v; } fc[] = {
        { 1, 2, 8 }, { 2, 1, 4 }, { 1, 1, 2 }, { 0.0, -0.0, 2 }, { NAN, 1, 1 }, { 1, NAN, 1 },
        { -INFINITY, -1e308, 8 }, { INFINITY, INFINITY, 2 },
    };
    for (size_t i = 0; i < sizeof fc / sizeof fc[0]; i++)
    for (int crf = 0; crf < 8; crf += 5) {
        begin(NAME("fcmpu/fcmpo cr%d %g,%g", crf, fc[i].a, fc[i].b));
        C.f[1].d = fc[i].a; C.f[2].d = fc[i].b; C.cr = 0x11111111;
        int sh = 28 - 4 * crf;
        emit(FX(crf << 2, 1, 2, 0, 0)); emit(X(3, 0, 0, 19, 0));
        emit(FX(((crf + 1) & 7) << 2, 1, 2, 32, 0)); emit(X(4, 0, 0, 19, 0));
        run();
        check("fcmpu cr", C.r[3], (0x11111111 & ~(0xFu << sh)) | fc[i].v << sh);
        int sh2 = 28 - 4 * ((crf + 1) & 7);
        check("fcmpo cr", C.r[4] & (0xFu << sh2), fc[i].v << sh2);
        check("FPCC", (C.fpscr >> 12) & 0xF, fc[i].v);
    }

    /* fsel: frD = frA >= 0 ? frC : frB  (NaN -> frB, -0 counts as >= 0) */
    static const struct { double a; bool c; } fs[] = { { 1, 1 }, { 0.0, 1 }, { -0.0, 1 }, { -1e-300, 0 }, { NAN, 0 }, { -INFINITY, 0 } };
    for (size_t i = 0; i < sizeof fs / sizeof fs[0]; i++) {
        begin(NAME("fsel %g", fs[i].a));
        C.f[1].d = fs[i].a; C.f[2].d = 10; C.f[3].d = 20;
        emit(FA(63, 4, 1, 3, 2, 23, 0)); run();  /* fsel f4,f1,f2(C),f3(B) */
        checkd("f4", C.f[4].d, fs[i].c ? 10 : 20);
    }

    begin("fsqrt / fsqrts");
    C.f[1].d = 2.0;
    emit(FA(63, 2, 0, 1, 0, 22, 0)); emit(FA(59, 3, 0, 1, 0, 22, 0)); run();
    checkd("fsqrt", C.f[2].d, sqrt(2.0)); checkd("fsqrts", C.f[3].d, (double)sqrtf(2.0f));
}

int main(void) {
    g_mem = calloc(1, GUEST_MEM_SIZE);
    if (!g_mem) { fprintf(stderr, "out of memory\n"); return 2; }
    g_log_level = 0;

    test_xo_arith();
    test_imm_arith();
    test_logical();
    test_compare();
    test_rotate();
    test_shift();
    test_loadstore();
    test_branch();
    test_cr_spr();
    test_fp_loadstore();
    test_fp_arith();

    printf("%d tests, %d failed\n", n_tests, n_fail);
    return n_fail ? 1 : 0;
}
