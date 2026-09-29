/* Toolbox initialisation. */
#include "os.h"
#include "mm.h"
#include "files.h"
#include "resources.h"
#include "threads.h"
#include "misc.h"
#include "../host/host.h"
#include "../config.h"
#include "qd.h"
#include "wm.h"
void text_init(void);
#include <sys/stat.h>

/* guest PC sampling (--profile) */
#include "../loader/pef.h"
typedef struct { const char *name; u32 hits; } GHit;
static GHit g_ghits[4096];
static u32 g_gtotal;
static void guest_sample(u32 pc) {
    u32 off;
    const char *n = sym_lookup(pc, &off);
    if (!n) n = "?";
    g_gtotal++;
    for (int i = 0; i < 4096; i++) {
        if (g_ghits[i].name == n) { g_ghits[i].hits++; return; }
        if (!g_ghits[i].name) { g_ghits[i].name = n; g_ghits[i].hits = 1; return; }
    }
}
static int ghit_cmp(const void *a, const void *b) { const GHit *x = a, *y = b; return (int)y->hits - (int)x->hits; }
void guest_profile_report(void) {
    int n = 0; while (n < 4096 && g_ghits[n].name) n++;
    qsort(g_ghits, (size_t)n, sizeof(GHit), ghit_cmp);
    extern u64 g_polls;
    fprintf(stderr, "guest instructions: ~%.0f million\n", g_polls * (double)CPU_POLL_INTERVAL / 1e6);
    fprintf(stderr, "guest functions (by interpreter samples):\n");
    for (int i = 0; i < n && i < 25; i++) fprintf(stderr, "  %-40s %5.1f%%\n", g_ghits[i].name, 100.0 * g_ghits[i].hits / (g_gtotal ? g_gtotal : 1));
}

u64 g_polls;
static void cpu_poll(CPU *c) {
    g_polls++;
    extern bool g_profile;
    if (g_profile) guest_sample(c->pc);
    static u64 warned_at;
    if (c->icount - c->last_trap_icount > 300000000ull && warned_at != c->last_trap_icount) {
        warned_at = c->last_trap_icount;
        extern void cpu_backtrace(CPU *c, FILE *f);
        LOG_W("stall: no Toolbox call for %llu instructions", (unsigned long long)(c->icount - c->last_trap_icount));
        cpu_backtrace(c, stderr);
    }
    misc_poll();
    irq_service();
    static u32 n;
    if ((++n & 15) == 0) host_pump(false);
    static u32 last;
    extern u32 tick_count(void);
    u32 t = tick_count();
    if (t != last) { last = t; qd_present(); }
    if (g_cfg.timeout_s && host_now_us() > (u64)g_cfg.timeout_s * 1000000u) {
        LOG_I("timeout reached");
        host_shutdown();
        exit(3);
    }
}

void os_init(void) {
    mm_init();
    misc_init();
    char sysdir[1024];
    if (g_cfg.sysdir) snprintf(sysdir, sizeof sysdir, "%s", g_cfg.sysdir);
    else {
        const char *home = getenv("HOME");
        snprintf(sysdir, sizeof sysdir, "%s/.cythera-port/System Folder", home ? home : ".");
    }
    files_init(g_cfg.data_dir, sysdir);
    res_init();
    char app[1100];
    snprintf(app, sizeof app, "%s/Cythera", g_cfg.data_dir);
    res_open_app(app);
    threads_init(g_cpu);
    host_init(g_cfg.screen_w, g_cfg.screen_h, g_cfg.headless, g_cfg.scale);
    qd_init_screen(g_cfg.screen_w, g_cfg.screen_h);
    text_init();
    extern void sound_init(void);
    sound_init();
    ev_init();
    if (g_cfg.script) script_load(g_cfg.script);
    g_cpu_poll = cpu_poll;
}
