/* Automatic backups of the game in progress (--autosave MIN, default 10).
 *
 * The game's own File > Backup (Cmd-B) saves a copy of the game under
 * another name without switching to it. When an autosave is due and the
 * game is idle in its main event loop (main > MEL > MyGetEvent: no dialog,
 * conversation or start screen, which run nested event loops), WaitNextEvent
 * hands the game a Cmd-B, and the "Backup Game As:" StandardPutFile that
 * follows is answered here, without showing it, with one of AUTOSAVE_SLOTS
 * rotating files "NAME autosave N" in Saved Games: the missing or oldest one,
 * never the game currently open. A crash during a save can only damage that
 * slot; the player's own saves and the other slots stay intact.
 *
 * If the game doesn't ask (Backup disabled, e.g. no game in progress), it is
 * retried a minute later. Nothing is saved if the player did nothing since
 * the last autosave, so an idle game doesn't push the older slots out, and
 * the count restarts when the player opens or saves a game with a dialog.
 */
#include "os.h"
#include "misc.h"
#include "files.h"
#include "../loader/pef.h"
#include <sys/stat.h>

#define AUTOSAVE_SLOTS 3

static u32 g_interval;  /* ticks, 0 = off */
static u32 g_next;      /* tick count of the next attempt */
static bool g_armed;    /* the Cmd-B was handed to the game: answer its StandardPutFile */
static bool g_input;    /* the player did something since the last autosave */
static int g_last;      /* slot written last in this session, 0 = none yet */

void autosave_set_minutes(double min) {
    g_interval = min > 0 ? (u32)(min * 60 * 60) : 0;
    g_next = g_interval;
}

void autosave_note_input(void) { g_input = true; }

/* The game's event loop asked for an event: is it the moment for a Cmd-B?
   `cpu` is in WaitNextEvent, called from the game. */
bool autosave_poll(CPU *cpu) {
    g_armed = false; /* the last command, if any, is over */
    if (!g_interval || !g_input || (s32)(tick_count() - g_next) < 0) return false;
    const char *chain[3] = { 0 };
    u32 sp = cpu->r[1], off;
    for (int i = 0; i < 3 && sp; i++) {
        u32 back = rd32(sp);
        if (!back || back <= sp || back >= GUEST_MEM_SIZE - 12) break;
        chain[i] = sym_lookup(rd32(back + 8), &off);
        sp = back;
    }
    return chain[2] && !strcmp(chain[2], "main");
}

/* The player opens or saves a game with a file dialog: start counting again. */
void autosave_restart(void) {
    if (!g_interval) return;
    g_input = false;
    g_next = tick_count() + g_interval;
}

/* WaitNextEvent handed the game the Cmd-B. */
void autosave_sent(void) {
    g_next = tick_count() + 60 * 60; /* retry in a minute unless the game asks for a file */
    g_armed = true;
}

static bool slot_path(const char *name, char *host, size_t hsz) {
    return vfs_resolve(VOL_REFNUM, files_saves_dir(), name, host, hsz, NULL, NULL) == noErr;
}

/* StandardPutFile: if it answers our Cmd-B, choose the slot (`name`, at
   most 31 characters) and whether it replaces a file. */
bool autosave_put_file(const char *prompt, const char *defname, char *name, bool *replacing) {
    if (!g_armed || strcmp(prompt, "Backup Game As:")) return false;
    g_armed = false;
    /* the default name is "CURRENT backup"; CURRENT may itself be a slot */
    char base[256], cur[256];
    snprintf(cur, sizeof cur, "%s", defname);
    size_t n = strlen(cur);
    if (n > 7 && !strcmp(cur + n - 7, " backup")) cur[n - 7] = 0;
    snprintf(base, sizeof base, "%s", cur);
    char *p = strstr(base, " autosave ");
    if (p && p[10] >= '1' && p[10] <= '0' + AUTOSAVE_SLOTS && !p[11]) *p = 0;
    base[31 - 11] = 0; /* room for " autosave N" */
    /* the slot after the last one this session; else the missing or oldest */
    int best = -1;
    time_t oldest = 0;
    for (int k = 0; k < AUTOSAVE_SLOTS; k++) {
        int i = (g_last + k) % AUTOSAVE_SLOTS + 1;
        char slot[64], host[1100];
        snprintf(slot, sizeof slot, "%s autosave %d", base, i);
        if (mac_names_equal(slot, cur)) continue;
        if (g_last) { best = i; break; }
        struct stat st;
        if (!slot_path(slot, host, sizeof host) || stat(host, &st)) { best = i; break; }
        if (best < 0 || st.st_mtime < oldest) { best = i; oldest = st.st_mtime; }
    }
    g_last = best;
    snprintf(name, 32, "%s autosave %d", base, best);
    char host[1100];
    *replacing = slot_path(name, host, sizeof host);
    autosave_restart();
    LOG_I("autosave: '%s'%s", name, *replacing ? " (replacing)" : "");
    return true;
}
