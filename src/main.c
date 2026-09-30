/* Delver runtime: runs the original Cythera PowerPC engine on a
 * high-level emulation of the classic Mac OS Toolbox. */
#include "common.h"
#include "cpu/ppc.h"
#include "loader/pef.h"
#include "os/os.h"
#include "config.h"
#include "host/host.h"
#include <sys/stat.h>

Config g_cfg;

static u8 *read_file(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    u8 *buf = malloc((size_t)n + 1);
    if (fread(buf, 1, (size_t)n, f) != (size_t)n) { fclose(f); free(buf); return NULL; }
    fclose(f);
    *len = (size_t)n;
    return buf;
}

static u32 resolver(int index, const PefImport *imp) {
    (void)index;
    return trap_resolve_import(imp->lib, imp->name, imp->weak);
}

static void usage(void) {
    fprintf(stderr,
        "usage: cythera [options]\n"
        "  --data DIR        game directory (default: gamedata, here or next to the program)\n"
        "  --headless        no window; render offscreen\n"
        "  --app NAME        run another application from the game folder (e.g. \"Register Cythera\")\n"
        "  --script FILE     run an input script (see README.md, tests/scripts/)\n"
        "  --sysdir DIR      emulated System Folder (default ~/.cythera-port/System Folder)\n"
#ifdef LICENSE_BYPASS
        "  --registered NAME name the game is registered to (default: Cythera Port)\n"
#endif
        "  --turbo N         run the emulated clock N times faster (tests)\n"
        "  --profile         print Toolbox/guest profile at exit\n"
        "  --deterministic   headless, virtual clock driven by instructions (reproducible tests)\n"
        "  --render-pict F O render PICT file F to PNG O and exit\n"
        "  --soundfont FILE  General MIDI SoundFont for music (default DATA/soundfont.sf2)\n"
        "  --wav FILE        record audio output (headless testing)\n"
        "  --timeout N       exit after N seconds\n"
        "  --trace-traps     log every Toolbox call\n"
        "  --trap-stats FILE append per-import call counts to FILE at exit\n"
        "  --strict          abort on unimplemented Toolbox calls\n"
        "  --screen WxH      emulated screen size (default 640x480)\n"
        "  --scale N         window scale factor\n"
        "  --fullscreen      start in fullscreen (toggle: Alt+Enter, or Ctrl+Cmd+F on macOS)\n"
        "  -v / -q           more / less logging\n");
    exit(2);
}

int main(int argc, char **argv) {
    g_cfg.data_dir = NULL;
    g_cfg.app = "Cythera";
    g_cfg.screen_w = 640;
    g_cfg.screen_h = 480;
    g_cfg.scale = 0;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "--data") && i + 1 < argc) g_cfg.data_dir = argv[++i];
        else if (!strcmp(a, "--headless")) g_cfg.headless = true;
        else if (!strcmp(a, "--fullscreen")) g_cfg.fullscreen = true;
        else if (!strcmp(a, "--app") && i + 1 < argc) g_cfg.app = argv[++i];
        else if (!strcmp(a, "--script") && i + 1 < argc) g_cfg.script = argv[++i];
        else if (!strcmp(a, "--sysdir") && i + 1 < argc) g_cfg.sysdir = argv[++i];
#ifdef LICENSE_BYPASS
        else if (!strcmp(a, "--registered") && i + 1 < argc) g_cfg.registered_to = argv[++i];
#endif
        else if (!strcmp(a, "--soundfont") && i + 1 < argc) g_cfg.soundfont = argv[++i];
        else if (!strcmp(a, "--wav") && i + 1 < argc) { extern void sound_wav_open(const char *); sound_wav_open(argv[++i]); }
        else if (!strcmp(a, "--render-pict") && i + 2 < argc) { g_cfg.render_pict = argv[++i]; g_cfg.render_out = argv[++i]; g_cfg.headless = true; }
        else if (!strcmp(a, "--timeout") && i + 1 < argc) g_cfg.timeout_s = atoi(argv[++i]);
        else if (!strcmp(a, "--turbo") && i + 1 < argc) { extern int g_turbo; g_turbo = atoi(argv[++i]); }
        else if (!strcmp(a, "--shot-at-trap") && i + 2 < argc) {
            extern const char *g_shot_trap, *g_shot_out; extern u32 g_shot_n;
            static char nm[64]; snprintf(nm, sizeof nm, "%s", argv[++i]);
            char *c = strchr(nm, ':'); g_shot_n = 1; if (c) { *c = 0; g_shot_n = (u32)atoi(c + 1); }
            g_shot_trap = nm; g_shot_out = argv[++i];
        }
        else if (!strcmp(a, "--deterministic")) { extern bool g_deterministic; g_deterministic = true; g_cfg.headless = true; }
        else if (!strcmp(a, "--trace-traps")) g_trace_traps = true;
        else if (!strcmp(a, "--trace-only") && i + 1 < argc) { extern const char *g_trace_only; g_trace_only = argv[++i]; g_trace_traps = true; }
        else if (!strcmp(a, "--profile")) { extern bool g_profile; extern void trap_profile_report(void); g_profile = true; atexit(trap_profile_report); }
        else if (!strcmp(a, "--trap-stats") && i + 1 < argc) { extern const char *g_trap_stats; extern void trap_stats_report(void); g_trap_stats = argv[++i]; atexit(trap_stats_report); }
        else if (!strcmp(a, "--strict")) g_strict_traps = true;
        else if (!strcmp(a, "--screen") && i + 1 < argc) {
            if (sscanf(argv[++i], "%dx%d", &g_cfg.screen_w, &g_cfg.screen_h) != 2) usage();
        } else if (!strcmp(a, "--scale") && i + 1 < argc) g_cfg.scale = atoi(argv[++i]);
        else if (!strcmp(a, "-v")) g_log_level++;
        else if (!strcmp(a, "-q")) g_log_level--;
        else usage();
    }

    if (!g_cfg.headless) g_fatal_hook = host_error_box;
    /* default: ./gamedata, else gamedata next to the executable (packaged builds) */
    if (!g_cfg.data_dir) {
        static char base[1024];
        struct stat st;
        snprintf(base, sizeof base, "%sgamedata", host_base_path());
        g_cfg.data_dir = "gamedata";
        if (stat("gamedata", &st) != 0 && stat(base, &st) == 0) { g_cfg.data_dir = base; g_cfg.data_readonly = true; }
    }

    g_mem = calloc(1, GUEST_MEM_SIZE);
    if (!g_mem) fatal("cannot allocate guest memory");

    char path[1024];
    snprintf(path, sizeof path, "%s/%s", g_cfg.data_dir, g_cfg.app);
    size_t len;
    u8 *pefbuf = read_file(path, &len);
    if (!pefbuf) fatal("cannot read %s (run tools/setup_gamedata.sh first)", path);

    static CPU main_cpu;
    g_cpu = &main_cpu;

    PefImage img;
    if (!pef_load(pefbuf, len, CODE_ADDR, DATA_ADDR, resolver, &img))
        fatal("%s is not a PowerPC PEF container", path);
    free(pefbuf);
    sym_scan_tracebacks(img.code_addr, img.code_size);
    extern void license_bypass(u32 code_addr, u32 code_size);
    license_bypass(img.code_addr, img.code_size);
    /* CYTHERA_WATCH=off,off,... (hex code offsets, as in tools/ppcdis.py) */
    for (const char *w = getenv("CYTHERA_WATCH"); w && *w; ) {
        char *end;
        u32 off = (u32)strtoul(w, &end, 16);
        if (end == w) break;
        cpu_watch_add(img.code_addr + off);
        w = *end ? end + 1 : end;
    }
    LOG_I("loaded code %#x bytes @%08x, data %#x bytes @%08x, %d imports",
          img.code_size, img.code_addr, img.data_size, img.data_addr, img.nimports);

    os_init();

    if (g_cfg.render_pict) {
        extern void trap_InitGraf(CPU *), trap_InitWindows(CPU *);
        extern int pict_render_file(const char *in, const char *out);
        u32 qdg = DATA_ADDR + img.data_size - 256; /* scratch QD globals */
        CPU f; memset(&f, 0, sizeof f); f.r[3] = qdg; trap_InitGraf(&f);
        memset(&f, 0, sizeof f); trap_InitWindows(&f);
        /* use the game's palette */
        extern u32 res_get(u32 type, s16 id);
        return pict_render_file(g_cfg.render_pict, g_cfg.render_out);
    }
    CPU *c = &main_cpu;
    u32 sp = STACK_AREA_END - 0x100;
    wr32(sp, 0);
    c->r[1] = sp;
    c->r[2] = rd32(img.main_tvec + 4);
    c->lr = RET_SENTINEL;
    c->pc = rd32(img.main_tvec);
    LOG_I("starting at %08x, TOC %08x", c->pc, c->r[2]);
    cpu_run(c);
    LOG_I("main returned after %llu instructions", (unsigned long long)c->icount);
    return 0;
}
