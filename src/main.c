/* Delver runtime: runs the original Cythera PowerPC engine on a
 * high-level emulation of the classic Mac OS Toolbox. */
#include "common.h"
#include "cpu/ppc.h"
#include "loader/pef.h"
#include "os/os.h"
#include "config.h"

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
        "  --data DIR        game directory (default: gamedata)\n"
        "  --headless        no window; render offscreen\n"
        "  --script FILE     run an input script (see docs/SCRIPTING.md)\n"
        "  --soundfont FILE  General MIDI SoundFont for music (default DATA/soundfont.sf2)\n"
        "  --wav FILE        record audio output (headless testing)\n"
        "  --timeout N       exit after N seconds\n"
        "  --trace-traps     log every Toolbox call\n"
        "  --strict          abort on unimplemented Toolbox calls\n"
        "  --screen WxH      emulated screen size (default 640x480)\n"
        "  --scale N         window scale factor\n"
        "  -v / -q           more / less logging\n");
    exit(2);
}

int main(int argc, char **argv) {
    g_cfg.data_dir = "gamedata";
    g_cfg.screen_w = 640;
    g_cfg.screen_h = 480;
    g_cfg.scale = 0;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "--data") && i + 1 < argc) g_cfg.data_dir = argv[++i];
        else if (!strcmp(a, "--headless")) g_cfg.headless = true;
        else if (!strcmp(a, "--script") && i + 1 < argc) g_cfg.script = argv[++i];
        else if (!strcmp(a, "--sysdir") && i + 1 < argc) g_cfg.sysdir = argv[++i];
        else if (!strcmp(a, "--soundfont") && i + 1 < argc) g_cfg.soundfont = argv[++i];
        else if (!strcmp(a, "--wav") && i + 1 < argc) { extern void sound_wav_open(const char *); sound_wav_open(argv[++i]); }
        else if (!strcmp(a, "--timeout") && i + 1 < argc) g_cfg.timeout_s = atoi(argv[++i]);
        else if (!strcmp(a, "--turbo") && i + 1 < argc) { extern int g_turbo; g_turbo = atoi(argv[++i]); }
        else if (!strcmp(a, "--trace-traps")) g_trace_traps = true;
        else if (!strcmp(a, "--profile")) { extern bool g_profile; extern void trap_profile_report(void); g_profile = true; atexit(trap_profile_report); }
        else if (!strcmp(a, "--strict")) g_strict_traps = true;
        else if (!strcmp(a, "--screen") && i + 1 < argc) {
            if (sscanf(argv[++i], "%dx%d", &g_cfg.screen_w, &g_cfg.screen_h) != 2) usage();
        } else if (!strcmp(a, "--scale") && i + 1 < argc) g_cfg.scale = atoi(argv[++i]);
        else if (!strcmp(a, "-v")) g_log_level++;
        else if (!strcmp(a, "-q")) g_log_level--;
        else usage();
    }

    g_mem = calloc(1, GUEST_MEM_SIZE);
    if (!g_mem) fatal("cannot allocate guest memory");

    char path[1024];
    snprintf(path, sizeof path, "%s/Cythera", g_cfg.data_dir);
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
    LOG_I("loaded code %#x bytes @%08x, data %#x bytes @%08x, %d imports",
          img.code_size, img.code_addr, img.data_size, img.data_addr, img.nimports);

    os_init();

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
