/* Toolbox initialisation. */
#include "os.h"
#include "mm.h"
#include "files.h"
#include "resources.h"
#include "threads.h"
#include "misc.h"
#include "../host/host.h"
#include "../config.h"
#include <sys/stat.h>

static void cpu_poll(CPU *c) {
    (void)c;
    misc_poll();
    irq_service();
    static u32 n;
    if ((++n & 15) == 0) host_pump(false);
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
    if (g_cfg.script) script_load(g_cfg.script);
    g_cpu_poll = cpu_poll;
}
