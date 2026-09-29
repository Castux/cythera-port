/* File Manager over a host directory tree.
 *
 * One volume (vRefNum -1) whose root directory (dirID 2) is the game data
 * directory. Directory IDs are assigned on demand. Resource forks are stored
 * as "<name>.rsrc" sidecar files. Finder info (type/creator/flags) is kept in
 * a per-directory ".finderinfo" text file, seeded from the installer
 * manifest.
 */
#include "files.h"
#include "mm.h"
#include <dirent.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <errno.h>

/* ---------------------------------------------------------------------- */
/* Mac Roman                                                               */

static const u16 macroman_hi[128] = {
    0x00C4,0x00C5,0x00C7,0x00C9,0x00D1,0x00D6,0x00DC,0x00E1,0x00E0,0x00E2,0x00E4,0x00E3,0x00E5,0x00E7,0x00E9,0x00E8,
    0x00EA,0x00EB,0x00ED,0x00EC,0x00EE,0x00EF,0x00F1,0x00F3,0x00F2,0x00F4,0x00F6,0x00F5,0x00FA,0x00F9,0x00FB,0x00FC,
    0x2020,0x00B0,0x00A2,0x00A3,0x00A7,0x2022,0x00B6,0x00DF,0x00AE,0x00A9,0x2122,0x00B4,0x00A8,0x2260,0x00C6,0x00D8,
    0x221E,0x00B1,0x2264,0x2265,0x00A5,0x00B5,0x2202,0x2211,0x220F,0x03C0,0x222B,0x00AA,0x00BA,0x03A9,0x00E6,0x00F8,
    0x00BF,0x00A1,0x00AC,0x221A,0x0192,0x2248,0x2206,0x00AB,0x00BB,0x2026,0x00A0,0x00C0,0x00C3,0x00D5,0x0152,0x0153,
    0x2013,0x2014,0x201C,0x201D,0x2018,0x2019,0x00F7,0x25CA,0x00FF,0x0178,0x2044,0x20AC,0x2039,0x203A,0xFB01,0xFB02,
    0x2021,0x00B7,0x201A,0x201E,0x2030,0x00C2,0x00CA,0x00C1,0x00CB,0x00C8,0x00CD,0x00CE,0x00CF,0x00CC,0x00D3,0x00D4,
    0xF8FF,0x00D2,0x00DA,0x00DB,0x00D9,0x0131,0x02C6,0x02DC,0x00AF,0x02D8,0x02D9,0x02DA,0x00B8,0x02DD,0x02DB,0x02C7
};

void macroman_to_utf8(const char *in, char *out, size_t outsz) {
    size_t o = 0;
    for (const u8 *p = (const u8 *)in; *p && o + 4 < outsz; p++) {
        u32 cp = *p < 0x80 ? *p : macroman_hi[*p - 0x80];
        if (cp == '/') cp = ':';
        if (cp < 0x80) out[o++] = (char)cp;
        else if (cp < 0x800) { out[o++] = (char)(0xC0 | cp >> 6); out[o++] = (char)(0x80 | (cp & 0x3F)); }
        else { out[o++] = (char)(0xE0 | cp >> 12); out[o++] = (char)(0x80 | ((cp >> 6) & 0x3F)); out[o++] = (char)(0x80 | (cp & 0x3F)); }
    }
    out[o] = 0;
}

void utf8_to_macroman(const char *in, char *out, size_t outsz) {
    size_t o = 0;
    const u8 *p = (const u8 *)in;
    while (*p && o + 1 < outsz) {
        u32 cp;
        if (*p < 0x80) cp = *p++;
        else if ((*p & 0xE0) == 0xC0 && p[1]) { cp = (u32)(p[0] & 0x1F) << 6 | (p[1] & 0x3F); p += 2; }
        else if ((*p & 0xF0) == 0xE0 && p[1] && p[2]) { cp = (u32)(p[0] & 0x0F) << 12 | (u32)(p[1] & 0x3F) << 6 | (p[2] & 0x3F); p += 3; }
        else { p++; cp = '?'; }
        if (cp == ':') cp = '/';
        if (cp < 0x80) { out[o++] = (char)cp; continue; }
        int k;
        for (k = 0; k < 128; k++) if (macroman_hi[k] == cp) break;
        out[o++] = k < 128 ? (char)(0x80 + k) : '?';
    }
    out[o] = 0;
}

/* Case- and diacritic-insensitive comparison, like HFS name matching. */
static u8 fold(u8 c) {
    if (c >= 'a' && c <= 'z') return (u8)(c - 32);
    static const char *acc = "\x80\x81\x82\x83\x84\x85\x86\x87\x88\x89\x8a\x8b\x8c\x8d\x8e\x8f\x90\x91\x92\x93\x94\x95\x96\x97\x98\x99\x9a\x9b\x9c\x9d\x9e\x9f";
    static const char *base = "AACENOUAAAAAACEEEEIIIINOOOOOUUUU";
    const char *q = strchr(acc, (char)c);
    if (c >= 0x80 && q) return (u8)base[q - acc];
    return c;
}
bool mac_names_equal(const char *a, const char *b) {
    while (*a && *b) { if (fold((u8)*a++) != fold((u8)*b++)) return false; }
    return *a == *b;
}

/* ---------------------------------------------------------------------- */
/* Directory table                                                         */

typedef struct { char *path; s32 parent; char name[64]; } Dir;
static Dir g_dirs[4096];
static s32 g_ndirs = 3; /* 0,1 unused; 2 = root */
static char g_sysdir_path[1024];
static s32 g_sysfolder_id, g_prefs_id;

bool vfs_is_dir(const char *p) { struct stat st; return !stat(p, &st) && S_ISDIR(st.st_mode); }
bool vfs_exists(const char *p) { struct stat st; return !stat(p, &st); }

s32 vfs_dir_id_for(const char *hostpath, s32 parent, const char *macname) {
    for (s32 i = 2; i < g_ndirs; i++)
        if (g_dirs[i].path && !strcmp(g_dirs[i].path, hostpath)) return i;
    if (g_ndirs >= 4096) fatal("too many directories");
    s32 id = g_ndirs++;
    g_dirs[id].path = strdup(hostpath);
    g_dirs[id].parent = parent;
    snprintf(g_dirs[id].name, sizeof g_dirs[id].name, "%s", macname);
    return id;
}
const char *vfs_dir_path(s32 id) { return (id >= 2 && id < g_ndirs) ? g_dirs[id].path : NULL; }
s32 vfs_dir_parent(s32 id) { return (id >= 2 && id < g_ndirs) ? g_dirs[id].parent : 1; }
const char *vfs_dir_name(s32 id) { return (id >= 2 && id < g_ndirs) ? g_dirs[id].name : ""; }

static void mkdir_p(const char *path) {
    char tmp[1024];
    snprintf(tmp, sizeof tmp, "%s", path);
    for (char *p = tmp + 1; *p; p++) if (*p == '/') { *p = 0; mkdir(tmp, 0755); *p = '/'; }
    mkdir(tmp, 0755);
}

static bool is_hidden_host_name(const char *n) {
    if (n[0] == '.') return true;
    size_t l = strlen(n);
    return l > 5 && !strcmp(n + l - 5, ".rsrc");
}

/* Find a child of host directory `dir` whose Mac name matches. Writes the
   host leaf name into `leaf`. */
static bool find_child(const char *dir, const char *macname, char *leaf, size_t lsz) {
    char u[512];
    macroman_to_utf8(macname, u, sizeof u);
    char p[1100];
    snprintf(p, sizeof p, "%s/%s", dir, u);
    if (vfs_exists(p)) { snprintf(leaf, lsz, "%s", u); return true; }
    /* resource-fork-only file */
    snprintf(p, sizeof p, "%s/%s.rsrc", dir, u);
    DIR *d = opendir(dir);
    if (!d) return false;
    struct dirent *e;
    bool found = false;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        char mn[512];
        utf8_to_macroman(e->d_name, mn, sizeof mn);
        size_t l = strlen(mn);
        if (l > 5 && !strcmp(mn + l - 5, ".rsrc")) mn[l - 5] = 0;
        if (mac_names_equal(mn, macname)) {
            snprintf(leaf, lsz, "%s", e->d_name);
            size_t ll = strlen(leaf);
            if (ll > 5 && !strcmp(leaf + ll - 5, ".rsrc")) {
                char base[1100];
                snprintf(base, sizeof base, "%s/%.*s", dir, (int)(ll - 5), leaf);
                if (!vfs_exists(base)) { leaf[ll - 5] = 0; found = true; break; }
                continue;
            }
            found = true;
            break;
        }
    }
    closedir(d);
    return found;
}

int vfs_resolve(s16 vref, s32 dirid, const char *macname, char *hostpath, size_t hsz,
                s32 *out_dir, char *out_name) {
    (void)vref;
    if (dirid == 0 || dirid == 1) dirid = ROOT_DIRID;
    const char *name = macname ? macname : "";
    /* Absolute path "Volume:..." or relative ":a:b" */
    char buf[256];
    snprintf(buf, sizeof buf, "%s", name);
    char *s = buf;
    if (strchr(s, ':')) {
        if (s[0] != ':') {
            /* absolute: skip volume name */
            char *c = strchr(s, ':');
            s = c + 1;
            dirid = ROOT_DIRID;
        } else {
            s++;
        }
        /* walk components */
        for (;;) {
            char *c = strchr(s, ':');
            if (!c) break;
            *c = 0;
            if (*s == 0) { dirid = vfs_dir_parent(dirid); if (dirid < 2) dirid = 2; }
            else {
                const char *dp = vfs_dir_path(dirid);
                char leaf[512];
                if (!dp || !find_child(dp, s, leaf, sizeof leaf)) return dirNFErr;
                char np[1100];
                snprintf(np, sizeof np, "%s/%s", dp, leaf);
                if (!vfs_is_dir(np)) return dirNFErr;
                dirid = vfs_dir_id_for(np, dirid, s);
            }
            s = c + 1;
        }
    }
    const char *dp = vfs_dir_path(dirid);
    if (!dp) return dirNFErr;
    if (out_dir) *out_dir = dirid;
    if (!*s) {
        /* the directory itself */
        if (out_name) strcpy(out_name, vfs_dir_name(dirid));
        if (out_dir) *out_dir = vfs_dir_parent(dirid);
        snprintf(hostpath, hsz, "%s", dp);
        return noErr;
    }
    if (out_name) snprintf(out_name, 64, "%s", s);
    char leaf[512];
    if (find_child(dp, s, leaf, sizeof leaf)) {
        snprintf(hostpath, hsz, "%s/%s", dp, leaf);
        if (out_name) utf8_to_macroman(leaf, out_name, 64);
        return noErr;
    }
    char u[512];
    macroman_to_utf8(s, u, sizeof u);
    snprintf(hostpath, hsz, "%s/%s", dp, u);
    return fnfErr;
}

void rsrc_path(const char *datapath, char *out, size_t outsz) {
    snprintf(out, outsz, "%s.rsrc", datapath);
}

/* ---------------------------------------------------------------------- */
/* Finder info database                                                    */

static void finfo_dbpath(const char *hostpath, char *db, size_t dbsz, char *leaf, size_t lsz) {
    const char *slash = strrchr(hostpath, '/');
    if (!slash) { snprintf(db, dbsz, ".finderinfo"); snprintf(leaf, lsz, "%s", hostpath); return; }
    snprintf(db, dbsz, "%.*s/.finderinfo", (int)(slash - hostpath), hostpath);
    snprintf(leaf, lsz, "%s", slash + 1);
}

static bool finfo_lookup_file(const char *db, const char *leaf, u32 *type, u32 *creator, u16 *flags) {
    FILE *f = fopen(db, "r");
    if (!f) return false;
    char line[1024];
    bool found = false;
    while (fgets(line, sizeof line, f)) {
        char *t1 = strchr(line, '\t');
        if (!t1) continue;
        *t1 = 0;
        if (strcmp(line, leaf)) continue;
        unsigned a, b, c;
        if (sscanf(t1 + 1, "%x %x %x", &a, &b, &c) == 3) { *type = a; *creator = b; *flags = (u16)c; found = true; }
    }
    fclose(f);
    return found;
}

/* Seed type/creator from the installer manifest (".manifest.csv"). */
static bool finfo_from_manifest(const char *hostpath, u32 *type, u32 *creator) {
    char dir[1024];
    const char *slash = strrchr(hostpath, '/');
    if (!slash) return false;
    snprintf(dir, sizeof dir, "%.*s/.manifest.csv", (int)(slash - hostpath), hostpath);
    FILE *f = fopen(dir, "r");
    if (!f) return false;
    char line[1024];
    bool found = false;
    while (fgets(line, sizeof line, f)) {
        /* cat_off,written_as,catalog_name,type,creator,... */
        char *fields[6] = {0};
        int n = 0; char *p = line; bool q = false; fields[n++] = p;
        for (; *p && n < 6; p++) {
            if (*p == '"') q = !q;
            else if (*p == ',' && !q) { *p = 0; fields[n++] = p + 1; }
        }
        if (n < 6) continue;
        if (strcmp(fields[1], slash + 1)) continue;
        if (strlen(fields[3]) >= 4 && strlen(fields[4]) >= 4) {
            char tm[8], cm[8];
            utf8_to_macroman(fields[3], tm, sizeof tm);
            utf8_to_macroman(fields[4], cm, sizeof cm);
            *type = FOURCC((u8)tm[0], (u8)tm[1], (u8)tm[2], (u8)tm[3]);
            *creator = FOURCC((u8)cm[0], (u8)cm[1], (u8)cm[2], (u8)cm[3]);
            found = true;
        }
        break;
    }
    fclose(f);
    return found;
}

void finfo_get(const char *hostpath, u32 *type, u32 *creator, u16 *flags) {
    char db[1100], leaf[512];
    finfo_dbpath(hostpath, db, sizeof db, leaf, sizeof leaf);
    *type = FOURCC('?', '?', '?', '?'); *creator = FOURCC('?', '?', '?', '?'); *flags = 0;
    if (finfo_lookup_file(db, leaf, type, creator, flags)) return;
    if (finfo_from_manifest(hostpath, type, creator)) return;
    size_t l = strlen(leaf);
    if (l > 3 && !strcmp(leaf + l - 3, ".ai")) { *type = FOURCC('T','E','X','T'); *creator = FOURCC('R','*','c','h'); }
}

void finfo_set(const char *hostpath, u32 type, u32 creator, u16 flags) {
    char db[1100], leaf[512];
    finfo_dbpath(hostpath, db, sizeof db, leaf, sizeof leaf);
    /* rewrite db without the leaf, then append */
    char tmp[1200];
    snprintf(tmp, sizeof tmp, "%s.tmp", db);
    FILE *in = fopen(db, "r"), *out = fopen(tmp, "w");
    if (!out) { if (in) fclose(in); return; }
    if (in) {
        char line[1024];
        while (fgets(line, sizeof line, in)) {
            char *t = strchr(line, '\t');
            if (t && (size_t)(t - line) == strlen(leaf) && !strncmp(line, leaf, strlen(leaf))) continue;
            fputs(line, out);
        }
        fclose(in);
    }
    fprintf(out, "%s\t%08x %08x %04x\n", leaf, type, creator, flags);
    fclose(out);
    rename(tmp, db);
}

/* ---------------------------------------------------------------------- */
/* Time                                                                    */

u32 mac_time_from_unix(s64 t) {
    struct tm lt;
    time_t tt = (time_t)t;
    localtime_r(&tt, &lt);
    return (u32)(t + lt.tm_gmtoff + 2082844800LL);
}
u32 mac_time_now(void) { return mac_time_from_unix((s64)time(NULL)); }

TRAP(GetDateTime) { u32 p = ARG(0); if (p) wr32(p, mac_time_now()); }

/* ---------------------------------------------------------------------- */
/* Open file control blocks                                                */

typedef struct {
    bool used;
    FILE *f;
    char path[1024];     /* host path of the fork */
    char datapath[1024]; /* host path of the file (data fork) */
    bool rsrc;
    int perm;
    u32 mark;
    s32 parid;
    char name[64];
} FCB;

#define FCB_MAX 128
static FCB g_fcbs[FCB_MAX];
static bool g_refnum_used[FCB_MAX];

/* refnum = 2 + 2*index, as on real Macs refnums are small positive numbers */
static FCB *fcb_get(s16 ref) {
    int i = (ref - 2) / 2;
    if (ref < 2 || (ref & 1) || i >= FCB_MAX || !g_fcbs[i].used) return NULL;
    return &g_fcbs[i];
}

s16 fcb_alloc_refnum(void) {
    for (int i = 1; i < FCB_MAX; i++)
        if (!g_refnum_used[i]) { g_refnum_used[i] = true; return (s16)(2 + 2 * i); }
    return 0;
}
void fcb_free_refnum(s16 ref) { int i = (ref - 2) / 2; if (i >= 0 && i < FCB_MAX) g_refnum_used[i] = false; }

int fcb_open(const char *hostpath, bool rsrc_fork, int perm, s16 *refnum) {
    char path[1100];
    if (rsrc_fork) rsrc_path(hostpath, path, sizeof path); else snprintf(path, sizeof path, "%s", hostpath);
    bool want_write = perm == 2 || perm == 3 || perm == 4 || perm == 0;
    if (!vfs_exists(hostpath) && !(rsrc_fork && vfs_exists(path))) return fnfErr;
    if (vfs_is_dir(hostpath)) return fnfErr;
    FILE *f = NULL;
    if (want_write) {
        f = fopen(path, "r+b");
        if (!f && rsrc_fork) f = fopen(path, "w+b"); /* create empty resource fork */
        if (!f && perm == 0) f = fopen(path, "rb");  /* fsCurPerm: fall back to read-only */
        if (!f) return perm == 0 ? fnfErr : permErr;
    } else {
        f = fopen(path, "rb");
        if (!f && rsrc_fork) { *refnum = 0; f = fopen("/dev/null", "rb"); } /* empty rsrc fork */
        if (!f) return fnfErr;
    }
    s16 ref = fcb_alloc_refnum();
    if (!ref) { fclose(f); return tmfoErr; }
    FCB *b = &g_fcbs[(ref - 2) / 2];
    memset(b, 0, sizeof *b);
    b->used = true; b->f = f; b->rsrc = rsrc_fork; b->perm = perm;
    snprintf(b->path, sizeof b->path, "%s", path);
    snprintf(b->datapath, sizeof b->datapath, "%s", hostpath);
    *refnum = ref;
    return noErr;
}

int fcb_close(s16 ref) {
    FCB *b = fcb_get(ref);
    if (!b) return rfNumErr;
    fclose(b->f);
    b->used = false;
    fcb_free_refnum(ref);
    return noErr;
}

static u32 fcb_eof(FCB *b) {
    long cur = ftell(b->f);
    fseek(b->f, 0, SEEK_END);
    long e = ftell(b->f);
    fseek(b->f, cur, SEEK_SET);
    return (u32)e;
}

static int fcb_read(FCB *b, u32 buf, u32 req, u32 *act) {
    u32 eof = fcb_eof(b);
    u32 n = req;
    int err = noErr;
    if (b->mark >= eof) { *act = 0; return eofErr; }
    if (b->mark + n > eof) { n = eof - b->mark; err = eofErr; }
    fseek(b->f, (long)b->mark, SEEK_SET);
    mchk(buf, n, true);
    size_t got = fread(g_mem + buf, 1, n, b->f);
    b->mark += (u32)got;
    *act = (u32)got;
    return err;
}

static int fcb_write(FCB *b, u32 buf, u32 req, u32 *act) {
    if (b->perm == 1) { *act = 0; return wrPermErr; }
    fseek(b->f, (long)b->mark, SEEK_SET);
    mchk(buf, req, false);
    size_t put = fwrite(g_mem + buf, 1, req, b->f);
    b->mark += (u32)put;
    *act = (u32)put;
    return put == req ? noErr : dskFulErr;
}

static int fcb_setpos(FCB *b, int mode, s32 off) {
    u32 eof = fcb_eof(b);
    s64 np;
    switch (mode & 3) {
    case 0: return noErr;
    case 1: np = off; break;
    case 2: np = (s64)eof + off; break;
    default: np = (s64)b->mark + off; break;
    }
    if (np < 0) { return posErr; }
    if (np > eof) { b->mark = eof; return eofErr; }
    b->mark = (u32)np;
    return noErr;
}

static int fcb_seteof(FCB *b, u32 len) {
    if (b->perm == 1) return wrPermErr;
    fflush(b->f);
    if (ftruncate(fileno(b->f), (off_t)len)) return ioErr;
    if (b->mark > len) b->mark = len;
    return noErr;
}

/* ---------------------------------------------------------------------- */
/* FSSpec                                                                  */

void fsspec_read(u32 spec, s16 *vref, s32 *parid, char *name) {
    *vref = rds16(spec);
    *parid = (s32)rd32(spec + 2);
    pstr_to_c(spec + 6, name, 64);
}
void fsspec_write(u32 spec, s16 vref, s32 parid, const char *name) {
    gmemset(spec, 0, 70);
    wr16(spec, (u16)vref);
    wr32(spec + 2, (u32)parid);
    c_to_pstr(name, spec + 6, 63);
}
int fsspec_hostpath(u32 spec, char *hostpath, size_t hsz) {
    s16 v; s32 p; char n[64];
    fsspec_read(spec, &v, &p, n);
    return vfs_resolve(v, p, n, hostpath, hsz, NULL, NULL);
}

void files_init(const char *root, const char *sysdir) {
    char abs[1024];
    if (!realpath(root, abs)) fatal("data directory %s not found", root);
    g_dirs[2].path = strdup(abs);
    g_dirs[2].parent = 1;
    snprintf(g_dirs[2].name, sizeof g_dirs[2].name, "Cythera");
    snprintf(g_sysdir_path, sizeof g_sysdir_path, "%s", sysdir);
    char prefs[1100];
    snprintf(prefs, sizeof prefs, "%s/Preferences", sysdir);
    mkdir_p(prefs);
    g_sysfolder_id = vfs_dir_id_for(sysdir, 2, "System Folder");
    g_prefs_id = vfs_dir_id_for(prefs, g_sysfolder_id, "Preferences");
    wr16(0x0210, (u16)VOL_REFNUM); /* BootDrive */
    LOG_I("volume root %s, system folder %s", abs, sysdir);
}

/* ---------------------------------------------------------------------- */
/* FSSpec traps                                                            */

TRAP(FSMakeFSSpec) {
    s16 vref = ARGS16(0); s32 dirid = (s32)ARG(1); u32 namep = ARG(2), spec = ARG(3);
    char name[256], host[1100], leaf[64];
    pstr_to_c(namep, name, sizeof name);
    s32 dir;
    int err = vfs_resolve(vref, dirid, name, host, sizeof host, &dir, leaf);
    if (err == noErr || err == fnfErr) fsspec_write(spec, VOL_REFNUM, dir, leaf);
    LOG_D("FSMakeFSSpec(%d,%d,'%s') -> %d dir %d '%s'", vref, dirid, name, err, dir, leaf);
    RETERR(err);
}

TRAP(FSpOpenDF) {
    u32 spec = ARG(0); s8 perm = (s8)ARG(1); u32 refp = ARG(2);
    char host[1100];
    int err = fsspec_hostpath(spec, host, sizeof host);
    s16 ref = 0;
    if (!err) err = fcb_open(host, false, perm, &ref);
    if (refp) wr16(refp, (u16)ref);
    LOG_D("FSpOpenDF(%s) -> %d ref %d", host, err, ref);
    RETERR(err);
}

TRAP(FSpOpenRF) {
    u32 spec = ARG(0); s8 perm = (s8)ARG(1); u32 refp = ARG(2);
    char host[1100];
    int err = fsspec_hostpath(spec, host, sizeof host);
    s16 ref = 0;
    if (!err) err = fcb_open(host, true, perm, &ref);
    if (refp) wr16(refp, (u16)ref);
    RETERR(err);
}

static int create_file(const char *host, u32 creator, u32 type) {
    if (vfs_exists(host)) return dupFNErr;
    FILE *f = fopen(host, "wb");
    if (!f) return ioErr;
    fclose(f);
    finfo_set(host, type, creator, 0);
    return noErr;
}

TRAP(FSpCreate) {
    u32 spec = ARG(0), creator = ARG(1), type = ARG(2);
    char host[1100];
    int err = fsspec_hostpath(spec, host, sizeof host);
    if (err == fnfErr) err = create_file(host, creator, type);
    else if (err == noErr) err = dupFNErr;
    RETERR(err);
}

static int delete_file(const char *host) {
    if (!vfs_exists(host)) return fnfErr;
    if (vfs_is_dir(host)) return rmdir(host) ? -47 /* fBsyErr */ : noErr;
    if (unlink(host)) return ioErr;
    char rp[1100];
    rsrc_path(host, rp, sizeof rp);
    unlink(rp);
    return noErr;
}

TRAP(FSpDelete) {
    char host[1100];
    int err = fsspec_hostpath(ARG(0), host, sizeof host);
    if (!err) err = delete_file(host);
    RETERR(err);
}

TRAP(FSpExchangeFiles) {
    char a[1100], b[1100], tmp[1200];
    int err = fsspec_hostpath(ARG(0), a, sizeof a);
    if (!err) err = fsspec_hostpath(ARG(1), b, sizeof b);
    if (err) { RETERR(err); return; }
    snprintf(tmp, sizeof tmp, "%s.xchg", a);
    rename(a, tmp); rename(b, a); rename(tmp, b);
    char ra[1100], rb[1100];
    rsrc_path(a, ra, sizeof ra); rsrc_path(b, rb, sizeof rb);
    snprintf(tmp, sizeof tmp, "%s.xchg", ra);
    bool ea = vfs_exists(ra), eb = vfs_exists(rb);
    if (ea) rename(ra, tmp);
    if (eb) rename(rb, ra);
    if (ea) rename(tmp, rb);
    RETERR(noErr);
}

static void write_finfo(u32 fi, const char *host) {
    u32 t, c; u16 fl;
    finfo_get(host, &t, &c, &fl);
    wr32(fi, t); wr32(fi + 4, c); wr16(fi + 8, fl);
    wr32(fi + 10, 0); wr16(fi + 14, 0);
}

TRAP(FSpGetFInfo) {
    char host[1100];
    int err = fsspec_hostpath(ARG(0), host, sizeof host);
    if (!err) write_finfo(ARG(1), host);
    RETERR(err);
}

TRAP(FSpSetFInfo) {
    char host[1100];
    int err = fsspec_hostpath(ARG(0), host, sizeof host);
    u32 fi = ARG(1);
    if (!err) finfo_set(host, rd32(fi), rd32(fi + 4), rd16(fi + 8));
    RETERR(err);
}

TRAP(HCreate) {
    s16 v = ARGS16(0); s32 d = (s32)ARG(1); u32 np = ARG(2), creator = ARG(3), type = ARG(4);
    char name[256], host[1100];
    pstr_to_c(np, name, sizeof name);
    int err = vfs_resolve(v, d, name, host, sizeof host, NULL, NULL);
    if (err == fnfErr) err = create_file(host, creator, type);
    else if (!err) err = dupFNErr;
    RETERR(err);
}

TRAP(HDelete) {
    s16 v = ARGS16(0); s32 d = (s32)ARG(1); u32 np = ARG(2);
    char name[256], host[1100];
    pstr_to_c(np, name, sizeof name);
    int err = vfs_resolve(v, d, name, host, sizeof host, NULL, NULL);
    if (!err) err = delete_file(host);
    RETERR(err);
}

TRAP(HGetFInfo) {
    s16 v = ARGS16(0); s32 d = (s32)ARG(1); u32 np = ARG(2), fi = ARG(3);
    char name[256], host[1100];
    pstr_to_c(np, name, sizeof name);
    int err = vfs_resolve(v, d, name, host, sizeof host, NULL, NULL);
    if (!err) write_finfo(fi, host);
    RETERR(err);
}

TRAP(HSetFInfo) {
    s16 v = ARGS16(0); s32 d = (s32)ARG(1); u32 np = ARG(2), fi = ARG(3);
    char name[256], host[1100];
    pstr_to_c(np, name, sizeof name);
    int err = vfs_resolve(v, d, name, host, sizeof host, NULL, NULL);
    if (!err) finfo_set(host, rd32(fi), rd32(fi + 4), rd16(fi + 8));
    RETERR(err);
}

TRAP(HOpen) {
    s16 v = ARGS16(0); s32 d = (s32)ARG(1); u32 np = ARG(2); s8 perm = (s8)ARG(3); u32 refp = ARG(4);
    char name[256], host[1100];
    pstr_to_c(np, name, sizeof name);
    int err = vfs_resolve(v, d, name, host, sizeof host, NULL, NULL);
    s16 ref = 0;
    if (!err) err = fcb_open(host, false, perm, &ref);
    if (refp) wr16(refp, (u16)ref);
    RETERR(err);
}

TRAP(FSClose) { RETERR(fcb_close(ARGS16(0))); }

TRAP(FSRead) {
    s16 ref = ARGS16(0); u32 cntp = ARG(1), buf = ARG(2);
    FCB *b = fcb_get(ref);
    if (!b) { RETERR(rfNumErr); return; }
    u32 act;
    int err = fcb_read(b, buf, rd32(cntp), &act);
    wr32(cntp, act);
    RETERR(err);
}

TRAP(FSWrite) {
    s16 ref = ARGS16(0); u32 cntp = ARG(1), buf = ARG(2);
    FCB *b = fcb_get(ref);
    if (!b) { RETERR(rfNumErr); return; }
    u32 act;
    int err = fcb_write(b, buf, rd32(cntp), &act);
    wr32(cntp, act);
    RETERR(err);
}

TRAP(GetEOF) {
    FCB *b = fcb_get(ARGS16(0));
    if (!b) { RETERR(rfNumErr); return; }
    wr32(ARG(1), fcb_eof(b));
    RETERR(noErr);
}

TRAP(SetEOF) {
    FCB *b = fcb_get(ARGS16(0));
    if (!b) { RETERR(rfNumErr); return; }
    RETERR(fcb_seteof(b, ARG(1)));
}

TRAP(SetFPos) {
    FCB *b = fcb_get(ARGS16(0));
    if (!b) { RETERR(rfNumErr); return; }
    RETERR(fcb_setpos(b, ARGS16(1), (s32)ARG(2)));
}

TRAP(GetFPos) {
    FCB *b = fcb_get(ARGS16(0));
    if (!b) { RETERR(rfNumErr); return; }
    wr32(ARG(1), b->mark);
    RETERR(noErr);
}

TRAP(HGetVol) {
    u32 volname = ARG(0), vrefp = ARG(1), dirp = ARG(2);
    if (volname) c_to_pstr("Cythera", volname, 27);
    if (vrefp) wr16(vrefp, (u16)VOL_REFNUM);
    if (dirp) wr32(dirp, ROOT_DIRID);
    RETERR(noErr);
}

TRAP(FlushVol) { for (int i = 0; i < FCB_MAX; i++) if (g_fcbs[i].used) fflush(g_fcbs[i].f); RETERR(noErr); }
TRAP(UnmountVol) { RETERR(-47); }
TRAP(Eject) { RETERR(-47); }

/* FindFolder(vRefNum, folderType, createFolder, &foundVRefNum, &foundDirID) */
TRAP(FindFolder) {
    u32 type = ARG(1); u32 vp = ARG(3), dp = ARG(4);
    s32 id;
    switch (type) {
    case FOURCC('m','a','c','s'): id = g_sysfolder_id; break; /* System Folder */
    case FOURCC('p','r','e','f'): id = g_prefs_id; break;
    default: {
        /* create a subfolder of the System Folder named after the type */
        char p[1100];
        snprintf(p, sizeof p, "%s/%s", g_sysdir_path, fourcc_str(type));
        mkdir_p(p);
        id = vfs_dir_id_for(p, g_sysfolder_id, fourcc_str(type));
    }
    }
    if (vp) wr16(vp, (u16)VOL_REFNUM);
    if (dp) wr32(dp, (u32)id);
    RETERR(noErr);
}

/* ---- Alias Manager (private record format) ---- */
TRAP(NewAlias) {
    u32 target = ARG(1), aliasp = ARG(2);
    u32 h = mm_new_handle(80, true, ZONE_APP);
    u32 p = hderef(h);
    wr32(p, FOURCC('C','P','A','L'));
    wr16(p + 4, 80);
    gmemmove(p + 8, target, 70);
    wr32(aliasp, h);
    RETERR(noErr);
}

TRAP(ResolveAlias) {
    u32 alias = ARG(1), target = ARG(2), changedp = ARG(3);
    if (!alias || !hderef(alias) || rd32(hderef(alias)) != FOURCC('C','P','A','L')) { RETERR(paramErr); return; }
    gmemmove(target, hderef(alias) + 8, 70);
    if (changedp) wr8(changedp, 0);
    char host[1100];
    int err = fsspec_hostpath(target, host, sizeof host);
    RETERR(err);
}

/* ---------------------------------------------------------------------- */
/* Parameter block calls                                                   */

#define PB_COMPLETION(pb) rd32((pb) + 12)
#define PB_RESULT(pb, e) wr16((pb) + 16, (u16)(s16)(e))
#define PB_NAME(pb) rd32((pb) + 18)
#define PB_VREF(pb) rds16((pb) + 22)

static void pb_done(CPU *cpu, u32 pb, int err, bool async) {
    PB_RESULT(pb, err);
    RETERR(err);
    if (async && PB_COMPLETION(pb)) {
        u32 a[1] = { pb };
        call_upp(PB_COMPLETION(pb), 1, a);
        RETERR(err);
    }
}

static void do_read(CPU *cpu, u32 pb, bool async) {
    FCB *b = fcb_get(rds16(pb + 24));
    if (!b) { pb_done(cpu, pb, rfNumErr, async); return; }
    int err = fcb_setpos(b, rds16(pb + 44), (s32)rd32(pb + 46));
    u32 act = 0;
    if (!err || err == eofErr) err = fcb_read(b, rd32(pb + 32), rd32(pb + 36), &act);
    wr32(pb + 40, act);
    wr32(pb + 46, b->mark);
    pb_done(cpu, pb, err, async);
}
static void do_write(CPU *cpu, u32 pb, bool async) {
    FCB *b = fcb_get(rds16(pb + 24));
    if (!b) { pb_done(cpu, pb, rfNumErr, async); return; }
    int mode = rds16(pb + 44) & 3;
    s32 off = (s32)rd32(pb + 46);
    int err = noErr;
    if (mode == 1) b->mark = (u32)off;
    else if (mode == 2) b->mark = fcb_eof(b) + (u32)off;
    else if (mode == 3) b->mark += (u32)off;
    u32 act = 0;
    err = fcb_write(b, rd32(pb + 32), rd32(pb + 36), &act);
    wr32(pb + 40, act);
    wr32(pb + 46, b->mark);
    pb_done(cpu, pb, err, async);
}
TRAP(PBReadSync) { do_read(cpu, ARG(0), false); }
TRAP(PBReadAsync) { do_read(cpu, ARG(0), true); }
TRAP(PBWriteSync) { do_write(cpu, ARG(0), false); }
TRAP(PBWriteAsync) { do_write(cpu, ARG(0), true); }

static void do_setfpos(CPU *cpu, u32 pb, bool async) {
    FCB *b = fcb_get(rds16(pb + 24));
    if (!b) { pb_done(cpu, pb, rfNumErr, async); return; }
    int err = fcb_setpos(b, rds16(pb + 44), (s32)rd32(pb + 46));
    wr32(pb + 46, b->mark);
    pb_done(cpu, pb, err, async);
}
TRAP(PBSetFPosSync) { do_setfpos(cpu, ARG(0), false); }
TRAP(PBSetFPosAsync) { do_setfpos(cpu, ARG(0), true); }

static void do_geteof(CPU *cpu, u32 pb, bool async) {
    FCB *b = fcb_get(rds16(pb + 24));
    if (!b) { pb_done(cpu, pb, rfNumErr, async); return; }
    wr32(pb + 28, fcb_eof(b)); /* ioMisc */
    pb_done(cpu, pb, noErr, async);
}
TRAP(PBGetEOFSync) { do_geteof(cpu, ARG(0), false); }
TRAP(PBGetEOFAsync) { do_geteof(cpu, ARG(0), true); }

static void do_seteof(CPU *cpu, u32 pb, bool async) {
    FCB *b = fcb_get(rds16(pb + 24));
    if (!b) { pb_done(cpu, pb, rfNumErr, async); return; }
    pb_done(cpu, pb, fcb_seteof(b, rd32(pb + 28)), async);
}
TRAP(PBSetEOFSync) { do_seteof(cpu, ARG(0), false); }
TRAP(PBSetEOFAsync) { do_seteof(cpu, ARG(0), true); }

TRAP(PBCloseSync) { u32 pb = ARG(0); pb_done(cpu, pb, fcb_close(rds16(pb + 24)), false); }
TRAP(PBFlushFileSync) {
    u32 pb = ARG(0);
    FCB *b = fcb_get(rds16(pb + 24));
    if (b) fflush(b->f);
    pb_done(cpu, pb, b ? noErr : rfNumErr, false);
}

static void pb_name(u32 pb, char *name) { pstr_to_c(PB_NAME(pb), name, 256); }

static void do_hopen(CPU *cpu, u32 pb, bool rsrc) {
    char name[256], host[1100];
    pb_name(pb, name);
    int err = vfs_resolve(PB_VREF(pb), (s32)rd32(pb + 48), name, host, sizeof host, NULL, NULL);
    s16 ref = 0;
    if (!err) err = fcb_open(host, rsrc, (s8)rd8(pb + 27), &ref);
    wr16(pb + 24, (u16)ref);
    pb_done(cpu, pb, err, false);
}
TRAP(PBHOpenSync) { do_hopen(cpu, ARG(0), false); }
TRAP(PBHOpenDFSync) { do_hopen(cpu, ARG(0), false); }
TRAP(PBHOpenDenySync) { do_hopen(cpu, ARG(0), false); }
TRAP(PBHOpenRFSync) { do_hopen(cpu, ARG(0), true); }
TRAP(PBHOpenRFDenySync) { do_hopen(cpu, ARG(0), true); }

TRAP(PBHCreateSync) {
    u32 pb = ARG(0);
    char name[256], host[1100];
    pb_name(pb, name);
    int err = vfs_resolve(PB_VREF(pb), (s32)rd32(pb + 48), name, host, sizeof host, NULL, NULL);
    if (err == fnfErr) err = create_file(host, FOURCC('?','?','?','?'), FOURCC('?','?','?','?'));
    else if (!err) err = dupFNErr;
    pb_done(cpu, pb, err, false);
}

TRAP(PBHDeleteSync) {
    u32 pb = ARG(0);
    char name[256], host[1100];
    pb_name(pb, name);
    int err = vfs_resolve(PB_VREF(pb), (s32)rd32(pb + 48), name, host, sizeof host, NULL, NULL);
    if (!err) err = delete_file(host);
    pb_done(cpu, pb, err, false);
}

static u32 host_fork_len(const char *p) { struct stat st; return stat(p, &st) ? 0 : (u32)st.st_size; }
static u32 host_mtime(const char *p) { struct stat st; return stat(p, &st) ? 0 : mac_time_from_unix(st.st_mtime); }

/* Fill an HFileInfo/DirInfo record for host path `host` in dir `parid`. */
static void fill_catinfo(u32 pb, const char *host, s32 parid, const char *macname) {
    u32 np = PB_NAME(pb);
    wr8(pb + 30, 0); /* ioFlAttrib */
    if (vfs_is_dir(host)) {
        s32 id = vfs_dir_id_for(host, parid, macname);
        wr8(pb + 30, 0x10);
        gmemset(pb + 32, 0, 16);
        wr32(pb + 48, (u32)id);
        int n = 0;
        DIR *d = opendir(host);
        if (d) { struct dirent *e; while ((e = readdir(d))) if (!is_hidden_host_name(e->d_name)) n++; closedir(d); }
        wr16(pb + 52, (u16)n);
        wr32(pb + 72, host_mtime(host)); wr32(pb + 76, host_mtime(host)); wr32(pb + 80, 0);
        gmemset(pb + 84, 0, 16);
        wr32(pb + 100, (u32)vfs_dir_parent(id));
    } else {
        write_finfo(pb + 32, host);
        wr32(pb + 48, 0); /* file number */
        wr16(pb + 52, 0);
        u32 dl = host_fork_len(host);
        char rp[1100]; rsrc_path(host, rp, sizeof rp);
        u32 rl = host_fork_len(rp);
        wr32(pb + 54, dl); wr32(pb + 58, (dl + 511) & ~511u);
        wr16(pb + 62, 0);
        wr32(pb + 64, rl); wr32(pb + 68, (rl + 511) & ~511u);
        wr32(pb + 72, host_mtime(host)); wr32(pb + 76, host_mtime(host)); wr32(pb + 80, 0);
        gmemset(pb + 84, 0, 16);
        wr32(pb + 100, (u32)parid);
        wr32(pb + 104, 0);
    }
    if (np) c_to_pstr(macname, np, 63);
}

/* Sorted listing of a host directory's visible entries (Mac names). */
typedef struct { char mac[64]; char host[512]; } Ent;
static int ent_cmp(const void *a, const void *b) {
    const Ent *x = a, *y = b;
    return strcasecmp(x->mac, y->mac);
}
static int list_dir(const char *dir, Ent **out) {
    DIR *d = opendir(dir);
    if (!d) { *out = NULL; return 0; }
    int n = 0, cap = 64;
    Ent *v = malloc(sizeof(Ent) * (size_t)cap);
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        size_t l = strlen(e->d_name);
        bool is_rsrc = l > 5 && !strcmp(e->d_name + l - 5, ".rsrc");
        char hostname[512];
        snprintf(hostname, sizeof hostname, "%s", e->d_name);
        if (is_rsrc) {
            char base[1100];
            snprintf(base, sizeof base, "%s/%.*s", dir, (int)(l - 5), e->d_name);
            if (vfs_exists(base)) continue; /* sidecar */
            hostname[l - 5] = 0;
        }
        if (n == cap) { cap *= 2; v = realloc(v, sizeof(Ent) * (size_t)cap); }
        utf8_to_macroman(hostname, v[n].mac, sizeof v[n].mac);
        snprintf(v[n].host, sizeof v[n].host, "%s", hostname);
        n++;
    }
    closedir(d);
    qsort(v, (size_t)n, sizeof(Ent), ent_cmp);
    *out = v;
    return n;
}

TRAP(PBGetCatInfoSync) {
    u32 pb = ARG(0);
    s16 index = rds16(pb + 28);
    s32 dirid = (s32)rd32(pb + 48);
    if (dirid == 0) dirid = ROOT_DIRID;
    if (index > 0) {
        const char *dp = vfs_dir_path(dirid);
        if (!dp) { pb_done(cpu, pb, dirNFErr, false); return; }
        Ent *v; int n = list_dir(dp, &v);
        if (index > n) { free(v); pb_done(cpu, pb, fnfErr, false); return; }
        char host[1100];
        snprintf(host, sizeof host, "%s/%s", dp, v[index - 1].host);
        fill_catinfo(pb, host, dirid, v[index - 1].mac);
        free(v);
        pb_done(cpu, pb, noErr, false);
        return;
    }
    char name[256], host[1100], leaf[64];
    if (index < 0 || !PB_NAME(pb)) name[0] = 0; else pb_name(pb, name);
    s32 par;
    int err = vfs_resolve(PB_VREF(pb), dirid, name, host, sizeof host, &par, leaf);
    if (err) { pb_done(cpu, pb, err, false); return; }
    if (!name[0]) {
        /* info about the directory itself */
        u32 np = PB_NAME(pb);
        fill_catinfo(pb, host, vfs_dir_parent(dirid), vfs_dir_name(dirid));
        if (np && index >= 0) {} /* name already written */
    } else {
        fill_catinfo(pb, host, par, leaf);
    }
    pb_done(cpu, pb, noErr, false);
}

TRAP(PBSetCatInfoSync) {
    u32 pb = ARG(0);
    char name[256], host[1100];
    pb_name(pb, name);
    int err = vfs_resolve(PB_VREF(pb), (s32)rd32(pb + 48), name, host, sizeof host, NULL, NULL);
    if (!err && !vfs_is_dir(host)) finfo_set(host, rd32(pb + 32), rd32(pb + 36), rd16(pb + 40));
    pb_done(cpu, pb, err, false);
}

TRAP(PBHGetFInfoSync) {
    u32 pb = ARG(0);
    char name[256], host[1100], leaf[64];
    s32 dirid = (s32)rd32(pb + 48);
    s16 index = rds16(pb + 28);
    if (index > 0) {
        const char *dp = vfs_dir_path(dirid ? dirid : ROOT_DIRID);
        Ent *v; int n = dp ? list_dir(dp, &v) : 0;
        int k = 0, found = -1;
        for (int i = 0; i < n; i++) {
            char hp[1100]; snprintf(hp, sizeof hp, "%s/%s", dp, v[i].host);
            if (vfs_is_dir(hp)) continue;
            if (++k == index) { found = i; break; }
        }
        if (found < 0) { if (n) free(v); pb_done(cpu, pb, fnfErr, false); return; }
        snprintf(host, sizeof host, "%s/%s", dp, v[found].host);
        fill_catinfo(pb, host, dirid, v[found].mac);
        free(v);
        pb_done(cpu, pb, noErr, false);
        return;
    }
    pb_name(pb, name);
    s32 par;
    int err = vfs_resolve(PB_VREF(pb), dirid, name, host, sizeof host, &par, leaf);
    if (!err) fill_catinfo(pb, host, par, leaf);
    pb_done(cpu, pb, err, false);
}

TRAP(PBHSetFLockSync) { pb_done(cpu, ARG(0), noErr, false); }

TRAP(PBHGetVInfoSync) {
    u32 pb = ARG(0);
    u32 np = PB_NAME(pb);
    if (np) c_to_pstr("Cythera", np, 27);
    wr16(pb + 22, (u16)VOL_REFNUM);
    wr32(pb + 30, 0); wr32(pb + 34, mac_time_now());
    wr16(pb + 38, 0); wr16(pb + 40, 100);
    wr16(pb + 46, 0xFFFF);          /* ioVNmAlBlks */
    wr32(pb + 48, 32768);           /* ioVAlBlkSiz */
    wr32(pb + 52, 32768);
    wr16(pb + 56, 0);
    wr32(pb + 58, 100000);
    wr16(pb + 62, 0xFFFF);          /* ioVFrBlk */
    wr16(pb + 64, 0x4244);          /* 'BD' HFS */
    wr16(pb + 66, 1); wr16(pb + 68, (u16)VOL_REFNUM); wr16(pb + 70, 0);
    wr32(pb + 72, 0); wr16(pb + 76, 0); wr32(pb + 78, 0);
    wr32(pb + 82, 100); wr32(pb + 86, 10);
    gmemset(pb + 90, 0, 32);
    wr32(pb + 90, (u32)g_sysfolder_id); /* ioVFndrInfo[0]: blessed folder */
    pb_done(cpu, pb, noErr, false);
}

TRAP(PBHGetVolParmsSync) {
    u32 pb = ARG(0);
    u32 buf = rd32(pb + 32), req = rd32(pb + 36);
    u8 tmp[14] = { 0, 2, 0, 0, 0, 0 };
    if (buf) gmemcpy_to(buf, tmp, req < 14 ? req : 14);
    wr32(pb + 40, req < 14 ? req : 14);
    pb_done(cpu, pb, noErr, false);
}

TRAP(PBGetFCBInfoSync) {
    u32 pb = ARG(0);
    s16 ref = rds16(pb + 24);
    s16 idx = rds16(pb + 28);
    FCB *b = NULL;
    if (idx > 0) {
        int k = 0;
        for (int i = 0; i < FCB_MAX; i++) if (g_fcbs[i].used && ++k == idx) { b = &g_fcbs[i]; ref = (s16)(2 + 2 * i); break; }
    } else b = fcb_get(ref);
    if (!b) { pb_done(cpu, pb, idx > 0 ? fnfErr : rfNumErr, false); return; }
    wr16(pb + 24, (u16)ref);
    char leaf[64]; s32 par = ROOT_DIRID;
    const char *slash = strrchr(b->datapath, '/');
    utf8_to_macroman(slash ? slash + 1 : b->datapath, leaf, sizeof leaf);
    if (slash) {
        char dir[1024]; snprintf(dir, sizeof dir, "%.*s", (int)(slash - b->datapath), b->datapath);
        for (s32 i = 2; i < g_ndirs; i++) if (g_dirs[i].path && !strcmp(g_dirs[i].path, dir)) { par = i; break; }
    }
    if (PB_NAME(pb)) c_to_pstr(leaf, PB_NAME(pb), 63);
    wr32(pb + 32, 0);
    wr16(pb + 36, (u16)((b->perm != 1 ? 0x0100 : 0) | (b->rsrc ? 0x0200 : 0)));
    wr16(pb + 38, 0);
    wr32(pb + 40, fcb_eof(b)); wr32(pb + 44, fcb_eof(b)); wr32(pb + 48, b->mark);
    wr16(pb + 52, (u16)VOL_REFNUM); wr32(pb + 54, 0); wr32(pb + 58, (u32)par);
    pb_done(cpu, pb, noErr, false);
}

/* Desktop database, file copying and catalog search are not supported;
   callers have fallbacks. */
TRAP(PBDTGetPath) { pb_done(cpu, ARG(0), paramErr, false); }
TRAP(PBDTOpenInform) { pb_done(cpu, ARG(0), paramErr, false); }
TRAP(PBDTGetCommentSync) { pb_done(cpu, ARG(0), paramErr, false); }
TRAP(PBDTSetCommentSync) { pb_done(cpu, ARG(0), paramErr, false); }
TRAP(PBHCopyFileSync) { pb_done(cpu, ARG(0), paramErr, false); }
TRAP(PBCatSearchSync) { pb_done(cpu, ARG(0), paramErr, false); }

/* Device driver calls: no CD-ROM driver is present. */
TRAP(PBControlSync) { pb_done(cpu, ARG(0), -28 /* notOpenErr */, false); }
TRAP(PBControlAsync) { pb_done(cpu, ARG(0), -28, true); }
TRAP(PBStatusSync) { pb_done(cpu, ARG(0), -28, false); }

/* Drive queue: empty. */
static u32 g_drvq;
TRAP(GetDrvQHdr) {
    if (!g_drvq) g_drvq = sys_alloc(10);
    RET(g_drvq);
}
