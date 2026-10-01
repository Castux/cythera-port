/* Golden traces (--golden FILE): a line per script command with hashes of
 * everything that happened up to it, so a deterministic run can be compared
 * with a recorded one (tests/run.sh, tests/golden/). A refactoring that
 * changes nothing keeps every line identical; the first line that differs
 * tells when a run diverged, and the hash that changed tells how:
 *   calls  the sequence of Toolbox calls made by the guest (names)
 *   vals   their arguments (r3-r10) and results (r3)
 *   fb     the screen
 */
#include "golden.h"
#include "cpu/ppc.h"
#include "host/host.h"

static FILE *g_f;
static u64 g_calls = 0xcbf29ce484222325ull, g_vals = 0xcbf29ce484222325ull;
static u64 g_ntraps;
static u32 g_args[8];

static u64 fnv(u64 h, const void *p, size_t n) {
    const u8 *b = p;
    for (size_t i = 0; i < n; i++) h = (h ^ b[i]) * 0x100000001b3ull;
    return h;
}

void golden_open(const char *path) {
    g_f = fopen(path, "wb"); /* "\n" line ends on every host */
    if (!g_f) fatal("cannot write %s", path);
}

bool golden_on(void) { return g_f != NULL; }

void golden_trap_enter(const CPU *c) { memcpy(g_args, &c->r[3], sizeof g_args); }

void golden_trap_leave(const CPU *c, u32 index) {
    g_ntraps++;
    g_calls = fnv(g_calls, &index, sizeof index);
    g_vals = fnv(g_vals, g_args, sizeof g_args);
    g_vals = fnv(g_vals, &c->r[3], sizeof c->r[3]);
}

void golden_mark(int line, const char *cmd, const char *arg) {
    if (!g_f) return;
    fprintf(g_f, "%d %s%s%s | traps %llu calls %016llx vals %016llx fb %016llx\n", line, cmd, *arg ? " " : "",
            arg, (unsigned long long)g_ntraps, (unsigned long long)g_calls, (unsigned long long)g_vals,
            (unsigned long long)host_frame_hash());
    fflush(g_f);
}

/* the end of the run (the game may quit before the script does) */
void golden_exit(void) { golden_mark(0, "exit", ""); }
