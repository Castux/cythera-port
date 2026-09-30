/* Logging, fatal errors, guest memory helpers. */
#include "common.h"
#include "cpu/ppc.h"
#include <stdarg.h>

u8 *g_mem;
int g_log_level = 2;
static FILE *g_logf;

void log_msg(int level, const char *fmt, ...) {
    static const char *tags[] = { "E", "W", "I", "D", "T" };
    if (level > g_log_level) return;
    FILE *f = g_logf ? g_logf : stderr;
    va_list ap;
    va_start(ap, fmt);
    fprintf(f, "[%s] ", tags[level < 5 ? level : 4]);
    vfprintf(f, fmt, ap);
    fputc('\n', f);
    va_end(ap);
    if (level == 0) fflush(f);
}

void cpu_backtrace(CPU *c, FILE *f);

void (*g_fatal_hook)(const char *msg);

_Noreturn void fatal(const char *fmt, ...) {
    char msg[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    fprintf(stderr, "FATAL: %s\n", msg);
    if (g_cpu) cpu_backtrace(g_cpu, stderr);
    fflush(stderr);
    if (g_fatal_hook) g_fatal_hook(msg);
    exit(1);
}

_Noreturn void mem_fault(u32 addr, u32 size, bool write) {
    fatal("guest memory fault: %s of %u bytes at %08x (pc %08x)",
          write ? "write" : "read", size, addr, g_cpu ? g_cpu->pc : 0);
}

void gmemcpy_to(u32 dst, const void *src, u32 n) { mchk(dst, n, true); memcpy(g_mem + dst, src, n); }
void gmemcpy_from(void *dst, u32 src, u32 n) { mchk(src, n, false); memcpy(dst, g_mem + src, n); }
void gmemset(u32 dst, u8 v, u32 n) { mchk(dst, n, true); memset(g_mem + dst, v, n); }
void gmemmove(u32 dst, u32 src, u32 n) { mchk(dst, n, true); mchk(src, n, false); memmove(g_mem + dst, g_mem + src, n); }

void pstr_to_c(u32 pstr, char *out, size_t outsz) {
    if (!pstr) { out[0] = 0; return; }
    u32 n = rd8(pstr);
    if (n >= outsz) n = (u32)outsz - 1;
    gmemcpy_from(out, pstr + 1, n);
    out[n] = 0;
}

void c_to_pstr(const char *s, u32 pstr, int maxlen) {
    size_t n = strlen(s);
    if ((int)n > maxlen) n = (size_t)maxlen;
    wr8(pstr, (u8)n);
    gmemcpy_to(pstr + 1, s, (u32)n);
}

size_t gstrlen(u32 a) {
    size_t n = 0;
    while (rd8(a + (u32)n)) n++;
    return n;
}

const char *fourcc_str(u32 t) {
    static char buf[4][8];
    static int k;
    char *b = buf[k++ & 3];
    for (int i = 0; i < 4; i++) {
        u8 ch = (u8)(t >> (24 - 8 * i));
        b[i] = (ch >= 32 && ch < 127) ? (char)ch : '?';
    }
    b[4] = 0;
    return b;
}
