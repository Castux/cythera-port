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

static void cpu_poll(CPU *c) {
    extern u64 g_last_trap_icount;
    static u64 warned_at;
    if (c->icount - g_last_trap_icount > 300000000ull && warned_at != g_last_trap_icount) {
        warned_at = g_last_trap_icount;
        extern void cpu_backtrace(CPU *c, FILE *f);
        LOG_W("stall: no Toolbox call for %llu instructions", (unsigned long long)(c->icount - g_last_trap_icount));
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
