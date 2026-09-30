/* Resource Manager.
 *
 * Resource files are parsed into host-side tables when opened. A resource's
 * data becomes a guest handle when first requested. Writable files are
 * rewritten in full on UpdateResFile/CloseResFile. Refnum 0 is a built-in
 * "System" file with resources the runtime provides.
 */
#include "resources.h"
#include "files.h"
#include "mm.h"

#define LOWMEM_ResErr   0x0A60
#define LOWMEM_CurMap   0x0A5A
#define LOWMEM_ResLoad  0x0A5E
#define LOWMEM_CurApRefNum 0x0900

typedef struct {
    u32 type;
    s16 id;
    u8 attrs;
    char *name;       /* NULL if unnamed */
    u8 *data;         /* original data (host) */
    u32 len;
    u32 handle;       /* guest handle when loaded */
    bool removed;
} Res;

typedef struct {
    bool used;
    s16 refnum;
    char path[1100];  /* host path of the fork file ("" for System) */
    bool writable;
    bool dirty;
    Res *res;
    int nres, capres;
    u16 map_attrs;
    int order;        /* open order */
} ResFile;

#define MAX_RESFILES 32
static ResFile g_rf[MAX_RESFILES];
static int g_open_counter;
static s16 g_cur = 0;
static bool g_resload = true;

void res_set_err(s16 e) { wr16(LOWMEM_ResErr, (u16)e); }

static ResFile *rf_by_ref(s16 ref) {
    for (int i = 0; i < MAX_RESFILES; i++) if (g_rf[i].used && g_rf[i].refnum == ref) return &g_rf[i];
    return NULL;
}

static Res *rf_add(ResFile *f) {
    if (f->nres == f->capres) {
        f->capres = f->capres ? f->capres * 2 : 64;
        f->res = realloc(f->res, sizeof(Res) * (size_t)f->capres);
    }
    Res *r = &f->res[f->nres++];
    memset(r, 0, sizeof *r);
    return r;
}

static bool parse_fork(ResFile *f, const u8 *d, u32 n) {
    if (n == 0) return true;
    if (n < 16) return false;
    u32 doff = be32(d), moff = be32(d + 4), dlen = be32(d + 8), mlen = be32(d + 12);
    if ((u64)moff + mlen > n || (u64)doff + dlen > n || mlen < 30) return false;
    const u8 *m = d + moff;
    f->map_attrs = be16(m + 22);
    u16 tl = be16(m + 24), nl = be16(m + 26);
    int ntypes = (s16)be16(m + tl) + 1;
    for (int i = 0; i < ntypes; i++) {
        const u8 *te = m + tl + 2 + i * 8;
        u32 type = be32(te);
        int cnt = be16(te + 4) + 1;
        u16 roff = be16(te + 6);
        for (int j = 0; j < cnt; j++) {
            const u8 *re = m + tl + roff + j * 12;
            Res *r = rf_add(f);
            r->type = type;
            r->id = (s16)be16(re);
            u16 noff = be16(re + 2);
            r->attrs = re[4];
            u32 off = (u32)re[5] << 16 | (u32)re[6] << 8 | re[7];
            if (noff != 0xFFFF) {
                const u8 *np = m + nl + noff;
                r->name = malloc(np[0] + 1u);
                memcpy(r->name, np + 1, np[0]);
                r->name[np[0]] = 0;
            }
            u32 len = be32(d + doff + off);
            r->len = len;
            r->data = malloc(len ? len : 1);
            memcpy(r->data, d + doff + off + 4, len);
        }
    }
    return true;
}

static ResFile *rf_new(s16 ref) {
    for (int i = 0; i < MAX_RESFILES; i++) {
        if (!g_rf[i].used) {
            memset(&g_rf[i], 0, sizeof g_rf[i]);
            g_rf[i].used = true;
            g_rf[i].refnum = ref;
            g_rf[i].order = g_open_counter++;
            return &g_rf[i];
        }
    }
    return NULL;
}

static int open_res_file(const char *hostpath, int perm, s16 *out) {
    char rp[1100];
    rsrc_path(hostpath, rp, sizeof rp);
    /* already open? return same refnum */
    for (int i = 0; i < MAX_RESFILES; i++)
        if (g_rf[i].used && !strcmp(g_rf[i].path, rp)) { *out = g_rf[i].refnum; return noErr; }
    if (!vfs_exists(hostpath) && !vfs_exists(rp)) return fnfErr;
    FILE *fp = fopen(rp, "rb");
    if (!fp) return -39; /* eofErr: no resource fork */
    fseek(fp, 0, SEEK_END);
    long n = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    u8 *buf = malloc((size_t)n + 1);
    if (fread(buf, 1, (size_t)n, fp) != (size_t)n) { fclose(fp); free(buf); return ioErr; }
    fclose(fp);
    if (n == 0) { free(buf); return eofErr; }
    s16 ref = fcb_alloc_refnum();
    ResFile *f = rf_new(ref);
    if (!f) { free(buf); return tmfoErr; }
    snprintf(f->path, sizeof f->path, "%s", rp);
    f->writable = perm != 1 && !vfs_read_only(rp);
    if (!parse_fork(f, buf, (u32)n)) {
        free(buf);
        f->used = false;
        fcb_free_refnum(ref);
        return mapReadErr;
    }
    free(buf);
    *out = ref;
    LOG_D("opened resource file %s as %d (%d resources)", rp, ref, f->nres);
    return noErr;
}

/* The QuickTime preview of a file, as AddFilePreview leaves it: a 'pnot'
   resource naming the preview resource (normally a thumbnail PICT). Read
   straight from the file on disk; returns a malloc'd copy of the preview
   data (and its type) or NULL. */
u8 *res_file_preview(const char *hostpath, u32 *type, u32 *len) {
    char rp[1100];
    rsrc_path(hostpath, rp, sizeof rp);
    FILE *fp = fopen(rp, "rb");
    if (!fp) return NULL;
    fseek(fp, 0, SEEK_END);
    long n = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    u8 *buf = n > 0 ? malloc((size_t)n) : NULL;
    if (!buf || fread(buf, 1, (size_t)n, fp) != (size_t)n) { fclose(fp); free(buf); return NULL; }
    fclose(fp);
    ResFile f = {0};
    u8 *out = NULL;
    if (parse_fork(&f, buf, (u32)n)) {
        for (int i = 0; i < f.nres && !out; i++) {
            Res *pn = &f.res[i];
            if (pn->type != FOURCC('p','n','o','t') || pn->len < 12) continue;
            u32 ptype = be32(pn->data + 6); s16 pid = (s16)be16(pn->data + 10);
            for (int j = 0; j < f.nres; j++)
                if (f.res[j].type == ptype && f.res[j].id == pid && f.res[j].len) {
                    out = malloc(f.res[j].len);
                    memcpy(out, f.res[j].data, f.res[j].len);
                    *len = f.res[j].len; *type = ptype;
                    break;
                }
        }
    }
    for (int i = 0; i < f.nres; i++) { free(f.res[i].data); free(f.res[i].name); }
    free(f.res);
    free(buf);
    return out;
}

/* ---- writing ---- */
static void wbe16(u8 *p, u32 v) { p[0] = (u8)(v >> 8); p[1] = (u8)v; }
static void wbe32(u8 *p, u32 v) { p[0] = (u8)(v >> 24); p[1] = (u8)(v >> 16); p[2] = (u8)(v >> 8); p[3] = (u8)v; }

static void res_current_data(Res *r, const u8 **data, u32 *len, u8 **tmp) {
    *tmp = NULL;
    if (r->handle && hderef(r->handle)) {
        *len = mm_handle_size(r->handle);
        *tmp = malloc(*len ? *len : 1);
        gmemcpy_from(*tmp, hderef(r->handle), *len);
        *data = *tmp;
    } else { *data = r->data; *len = r->len; }
}

static int write_res_file(ResFile *f) {
    if (!f->path[0] || !f->writable) return noErr;
    /* collect live resources grouped by type */
    int n = 0;
    for (int i = 0; i < f->nres; i++) if (!f->res[i].removed) n++;
    u32 *types = malloc(sizeof(u32) * (size_t)(n + 1));
    int nt = 0;
    for (int i = 0; i < f->nres; i++) {
        if (f->res[i].removed) continue;
        bool seen = false;
        for (int k = 0; k < nt; k++) if (types[k] == f->res[i].type) seen = true;
        if (!seen) types[nt++] = f->res[i].type;
    }
    /* data section */
    size_t dcap = 4096, dlen = 0;
    u8 *dbuf = malloc(dcap);
    u32 *offs = calloc((size_t)f->nres + 1, sizeof(u32));
    for (int i = 0; i < f->nres; i++) {
        Res *r = &f->res[i];
        if (r->removed) continue;
        const u8 *d; u32 l; u8 *tmp;
        res_current_data(r, &d, &l, &tmp);
        while (dlen + l + 4 > dcap) { dcap *= 2; dbuf = realloc(dbuf, dcap); }
        offs[i] = (u32)dlen;
        wbe32(dbuf + dlen, l); memcpy(dbuf + dlen + 4, d, l);
        dlen += 4 + l;
        free(tmp);
    }
    /* map */
    size_t ncap = 1024, nlen = 0;
    u8 *nbuf = malloc(ncap);
    size_t tl_size = 2 + 8 * (size_t)nt;
    size_t ref_size = 12 * (size_t)n;
    size_t mlen0 = 28 + tl_size + ref_size;
    u8 *mbuf = calloc(1, mlen0);
    wbe16(mbuf + 22, f->map_attrs);
    wbe16(mbuf + 24, 28);
    wbe16(mbuf + 28, (u32)(nt - 1));
    size_t refpos = 28 + tl_size;
    for (int k = 0; k < nt; k++) {
        int cnt = 0;
        u8 *te = mbuf + 28 + 2 + 8 * k;
        wbe32(te, types[k]);
        wbe16(te + 6, (u32)(refpos - 28));
        for (int i = 0; i < f->nres; i++) {
            Res *r = &f->res[i];
            if (r->removed || r->type != types[k]) continue;
            u8 *re = mbuf + refpos;
            wbe16(re, (u16)r->id);
            if (r->name) {
                size_t l = strlen(r->name);
                while (nlen + l + 1 > ncap) { ncap *= 2; nbuf = realloc(nbuf, ncap); }
                wbe16(re + 2, (u32)nlen);
                nbuf[nlen] = (u8)l; memcpy(nbuf + nlen + 1, r->name, l);
                nlen += l + 1;
            } else wbe16(re + 2, 0xFFFF);
            re[4] = r->attrs & ~0x02; /* clear resChanged */
            re[5] = (u8)(offs[i] >> 16); re[6] = (u8)(offs[i] >> 8); re[7] = (u8)offs[i];
            refpos += 12;
            cnt++;
        }
        wbe16(te + 4, (u32)(cnt - 1));
    }
    wbe16(mbuf + 26, (u32)mlen0);
    size_t mlen = mlen0 + nlen;
    u32 doff = 256, moff = (u32)(256 + dlen);
    u8 hdr[256] = {0};
    wbe32(hdr, doff); wbe32(hdr + 4, moff); wbe32(hdr + 8, (u32)dlen); wbe32(hdr + 12, (u32)mlen);
    memcpy(mbuf, hdr, 16);
    FILE *fp = fopen(f->path, "wb");
    int err = noErr;
    if (!fp) err = ioErr;
    else {
        fwrite(hdr, 1, 256, fp);
        fwrite(dbuf, 1, dlen, fp);
        fwrite(mbuf, 1, mlen0, fp);
        fwrite(nbuf, 1, nlen, fp);
        fclose(fp);
    }
    free(types); free(dbuf); free(offs); free(nbuf); free(mbuf);
    f->dirty = false;
    for (int i = 0; i < f->nres; i++) f->res[i].attrs &= ~0x02;
    return err;
}

/* ---- lookup ---- */
static int rf_sorted(ResFile **list) {
    int n = 0;
    for (int i = 0; i < MAX_RESFILES; i++) if (g_rf[i].used) list[n++] = &g_rf[i];
    /* newest first */
    for (int i = 0; i < n; i++)
        for (int j = i + 1; j < n; j++)
            if (list[j]->order > list[i]->order) { ResFile *t = list[i]; list[i] = list[j]; list[j] = t; }
    return n;
}

static Res *find_in(ResFile *f, u32 type, s16 id, const char *name) {
    for (int i = 0; i < f->nres; i++) {
        Res *r = &f->res[i];
        if (r->removed || r->type != type) continue;
        if (name ? (r->name && mac_names_equal(r->name, name)) : r->id == id) return r;
    }
    return NULL;
}

static Res *search(u32 type, s16 id, const char *name, bool one, ResFile **owner) {
    ResFile *list[MAX_RESFILES];
    int n = rf_sorted(list);
    int start = 0;
    for (int i = 0; i < n; i++) if (list[i]->refnum == g_cur) { start = i; break; }
    for (int i = start; i < n; i++) {
        Res *r = find_in(list[i], type, id, name);
        if (r) { if (owner) *owner = list[i]; return r; }
        if (one) break;
    }
    return NULL;
}

static u32 load_handle(Res *r, ResFile *f) {
    if (r->handle) {
        if (!hderef(r->handle) && g_resload) {
            mm_reallocate_handle(r->handle, r->len);
            gmemcpy_to(hderef(r->handle), r->data, r->len);
        }
        return r->handle;
    }
    int zone = (r->attrs & 0x40) ? ZONE_SYS : ZONE_APP;
    u32 h;
    if (g_resload) {
        h = mm_handle_from_data(r->data, r->len, zone);
    } else {
        h = mm_new_empty_handle(zone);
    }
    if (!h) { res_set_err(memFullErr); return 0; }
    u8 st = HS_RESOURCE;
    if (r->attrs & 0x20) st |= HS_PURGEABLE;
    if (r->attrs & 0x10) st |= HS_LOCKED;
    mm_hsetstate(h, st);
    r->handle = h;
    (void)f;
    return h;
}

static Res *res_by_handle(u32 h, ResFile **owner) {
    if (!h) return NULL;
    for (int i = 0; i < MAX_RESFILES; i++) {
        if (!g_rf[i].used) continue;
        for (int j = 0; j < g_rf[i].nres; j++)
            if (g_rf[i].res[j].handle == h && !g_rf[i].res[j].removed) { if (owner) *owner = &g_rf[i]; return &g_rf[i].res[j]; }
    }
    return NULL;
}

u32 res_get(u32 type, s16 id) {
    ResFile *f;
    Res *r = search(type, id, NULL, false, &f);
    if (!r) { res_set_err(resNotFound); LOG_D("GetResource('%s',%d) not found", fourcc_str(type), id); return 0; }
    res_set_err(noErr);
    return load_handle(r, f);
}
u32 res_get1(u32 type, s16 id) {
    ResFile *f;
    Res *r = search(type, id, NULL, true, &f);
    if (!r) { res_set_err(resNotFound); return 0; }
    res_set_err(noErr);
    return load_handle(r, f);
}
u32 res_get_named(u32 type, const char *name) {
    ResFile *f;
    Res *r = search(type, 0, name, false, &f);
    if (!r) { res_set_err(resNotFound); return 0; }
    res_set_err(noErr);
    return load_handle(r, f);
}

u8 *res_load_raw(u32 type, s16 id, u32 *len) {
    ResFile *f;
    Res *r = search(type, id, NULL, false, &f);
    if (!r) return NULL;
    const u8 *d; u8 *tmp;
    res_current_data(r, &d, len, &tmp);
    if (tmp) return tmp;
    u8 *c = malloc(*len ? *len : 1);
    memcpy(c, d, *len);
    return c;
}

void res_release(u32 h) {
    Res *r = res_by_handle(h, NULL);
    if (!r) { res_set_err(resNotFound); return; }
    if (r->attrs & 0x02) { res_set_err(noErr); return; } /* changed: keep */
    mm_dispose_handle(h);
    r->handle = 0;
    res_set_err(noErr);
}

void res_detach(u32 h) {
    Res *r = res_by_handle(h, NULL);
    if (!r) { res_set_err(resNotFound); return; }
    r->handle = 0;
    mm_hsetstate(h, mm_hgetstate(h) & ~HS_RESOURCE);
    res_set_err(noErr);
}

bool res_info(u32 h, u32 *type, s16 *id, char *name) {
    Res *r = res_by_handle(h, NULL);
    if (!r) return false;
    if (type) *type = r->type;
    if (id) *id = r->id;
    if (name) snprintf(name, 256, "%s", r->name ? r->name : "");
    return true;
}

s16 res_cur_file(void) { return g_cur; }

void res_add_system(u32 type, s16 id, const char *name, const void *data, u32 len) {
    ResFile *f = rf_by_ref(0);
    Res *r = rf_add(f);
    r->type = type; r->id = id;
    r->name = name ? strdup(name) : NULL;
    r->data = malloc(len ? len : 1);
    memcpy(r->data, data, len);
    r->len = len;
    r->attrs = 0x40;
}

void res_init(void) {
    ResFile *f = rf_new(0);
    f->path[0] = 0;
    f->order = -1000;
    g_cur = 0;
    wr16(LOWMEM_CurMap, 0);
}

s16 res_open_app(const char *hostpath) {
    s16 ref;
    int err = open_res_file(hostpath, 1, &ref);
    if (err) fatal("cannot open application resource fork for %s (%d)", hostpath, err);
    g_cur = ref;
    wr16(LOWMEM_CurApRefNum, (u16)ref);
    wr16(LOWMEM_CurMap, (u16)ref);
    return ref;
}

/* ---------------------------------------------------------------------- */
/* Traps                                                                   */

TRAP(GetResource) { RET(res_get(ARG(0), ARGS16(1))); }
TRAP(Get1Resource) { RET(res_get1(ARG(0), ARGS16(1))); }
TRAP(GetNamedResource) { char n[256]; pstr_to_c(ARG(1), n, sizeof n); RET(res_get_named(ARG(0), n)); }
TRAP(Get1NamedResource) {
    char n[256]; pstr_to_c(ARG(1), n, sizeof n);
    ResFile *f;
    Res *r = search(ARG(0), 0, n, true, &f);
    if (!r) { res_set_err(resNotFound); RET(0); return; }
    res_set_err(noErr);
    RET(load_handle(r, f));
}
TRAP(ReleaseResource) { res_release(ARG(0)); }
TRAP(DetachResource) { res_detach(ARG(0)); }
TRAP(ResError) { RETERR(rds16(LOWMEM_ResErr)); }
TRAP(CurResFile) { RETERR(g_cur); }
TRAP(SetResLoad) { g_resload = ARGB(0); wr8(LOWMEM_ResLoad, g_resload); }
TRAP(LoadResource) {
    u32 h = ARG(0);
    ResFile *f; Res *r = res_by_handle(h, &f);
    if (!r) { res_set_err(resNotFound); return; }
    bool save = g_resload; g_resload = true;
    load_handle(r, f);
    g_resload = save;
    res_set_err(noErr);
}
TRAP(HomeResFile) {
    ResFile *f; Res *r = res_by_handle(ARG(0), &f);
    if (!r) { res_set_err(resNotFound); RETERR(-1); return; }
    res_set_err(noErr); RETERR(f->refnum);
}
TRAP(UseResFile) {
    s16 ref = ARGS16(0);
    if (!rf_by_ref(ref)) { res_set_err(resFNotFound); return; }
    g_cur = ref;
    wr16(LOWMEM_CurMap, (u16)ref);
    res_set_err(noErr);
}

static s16 do_open_spec_res(u32 spec, s8 perm) {
    char host[1100];
    int err = fsspec_hostpath(spec, host, sizeof host);
    s16 ref = -1;
    if (!err) err = open_res_file(host, perm, &ref);
    if (err) { res_set_err((s16)err); return -1; }
    g_cur = ref;
    wr16(LOWMEM_CurMap, (u16)ref);
    res_set_err(noErr);
    return ref;
}

TRAP(FSpOpenResFile) { RETERR(do_open_spec_res(ARG(0), (s8)ARG(1))); }

TRAP(HOpenResFile) {
    s16 v = ARGS16(0); s32 d = (s32)ARG(1); u32 np = ARG(2); s8 perm = (s8)ARG(3);
    char name[256], host[1100];
    pstr_to_c(np, name, sizeof name);
    int err = vfs_resolve(v, d, name, host, sizeof host, NULL, NULL);
    s16 ref = -1;
    if (!err) err = open_res_file(host, perm, &ref);
    if (err) { res_set_err((s16)err); RETERR(-1); return; }
    g_cur = ref;
    wr16(LOWMEM_CurMap, (u16)ref);
    res_set_err(noErr);
    RETERR(ref);
}

static void create_res_file(const char *host, u32 creator, u32 type, bool set_type) {
    if (vfs_read_only(host)) return;
    if (!vfs_exists(host)) {
        FILE *fp = fopen(host, "wb");
        if (fp) fclose(fp);
        if (set_type) finfo_set(host, type, creator, 0);
    }
    char rp[1100];
    rsrc_path(host, rp, sizeof rp);
    FILE *fp = fopen(rp, "rb");
    if (fp) {
        fseek(fp, 0, SEEK_END);
        long n = ftell(fp);
        fclose(fp);
        if (n > 0) { res_set_err(dupFNErr); return; }
    }
    ResFile tmp = {0};
    snprintf(tmp.path, sizeof tmp.path, "%s", rp);
    tmp.writable = true;
    res_set_err((s16)write_res_file(&tmp));
}

TRAP(FSpCreateResFile) {
    u32 spec = ARG(0), creator = ARG(1), type = ARG(2);
    char host[1100];
    int err = fsspec_hostpath(spec, host, sizeof host);
    if (err && err != fnfErr) { res_set_err((s16)err); return; }
    create_res_file(host, creator, type, err == fnfErr);
}

TRAP(HCreateResFile) {
    s16 v = ARGS16(0); s32 d = (s32)ARG(1); u32 np = ARG(2);
    char name[256], host[1100];
    pstr_to_c(np, name, sizeof name);
    int err = vfs_resolve(v, d, name, host, sizeof host, NULL, NULL);
    if (err && err != fnfErr) { res_set_err((s16)err); return; }
    create_res_file(host, FOURCC('?','?','?','?'), FOURCC('?','?','?','?'), err == fnfErr);
}

TRAP(CloseResFile) {
    s16 ref = ARGS16(0);
    ResFile *f = rf_by_ref(ref);
    if (!f || ref == 0) { res_set_err(ref ? resFNotFound : noErr); return; }
    if (f->dirty) write_res_file(f);
    for (int i = 0; i < f->nres; i++) {
        Res *r = &f->res[i];
        if (r->handle) mm_dispose_handle(r->handle);
        free(r->data); free(r->name);
    }
    free(f->res);
    f->used = false;
    fcb_free_refnum(ref);
    if (g_cur == ref) {
        /* next newest file becomes current */
        ResFile *list[MAX_RESFILES];
        int n = rf_sorted(list);
        g_cur = n ? list[0]->refnum : 0;
        wr16(LOWMEM_CurMap, (u16)g_cur);
    }
    res_set_err(noErr);
}

TRAP(UpdateResFile) {
    ResFile *f = rf_by_ref(ARGS16(0));
    if (!f) { res_set_err(resFNotFound); return; }
    res_set_err((s16)write_res_file(f));
}

TRAP(AddResource) {
    u32 h = ARG(0), type = ARG(1); s16 id = ARGS16(2); u32 np = ARG(3);
    ResFile *f = rf_by_ref(g_cur);
    if (!f || !h || res_by_handle(h, NULL)) { res_set_err(addResFailed); return; }
    Res *r = rf_add(f);
    r->type = type; r->id = id;
    char name[256];
    pstr_to_c(np, name, sizeof name);
    r->name = (np && name[0]) ? strdup(name) : NULL;
    r->handle = h;
    r->attrs = 0x02;
    r->len = 0; r->data = malloc(1);
    mm_hsetstate(h, mm_hgetstate(h) | HS_RESOURCE);
    f->dirty = true;
    res_set_err(noErr);
}

TRAP(ChangedResource) {
    ResFile *f; Res *r = res_by_handle(ARG(0), &f);
    if (!r) { res_set_err(resNotFound); return; }
    r->attrs |= 0x02;
    f->dirty = true;
    res_set_err(noErr);
}

TRAP(WriteResource) {
    ResFile *f; Res *r = res_by_handle(ARG(0), &f);
    if (!r) { res_set_err(resNotFound); return; }
    if (r->attrs & 0x02) {
        /* snapshot current data */
        u32 l = mm_handle_size(r->handle);
        free(r->data);
        r->data = malloc(l ? l : 1);
        gmemcpy_from(r->data, hderef(r->handle), l);
        r->len = l;
        f->dirty = true;
    }
    res_set_err(noErr);
}

TRAP(RemoveResource) {
    ResFile *f; Res *r = res_by_handle(ARG(0), &f);
    if (!r) { res_set_err(rmvResFailed); return; }
    r->removed = true;
    mm_hsetstate(r->handle, mm_hgetstate(r->handle) & ~HS_RESOURCE);
    r->handle = 0;
    f->dirty = true;
    res_set_err(noErr);
}

TRAP(UniqueID) {
    u32 type = ARG(0);
    for (s16 id = 128; id < 32767; id++) {
        if (!search(type, id, NULL, false, NULL)) { RETERR(id); return; }
    }
    RETERR(-1);
}

TRAP(GetResInfo) {
    u32 h = ARG(0), idp = ARG(1), tp = ARG(2), np = ARG(3);
    Res *r = res_by_handle(h, NULL);
    if (!r) { res_set_err(resNotFound); return; }
    if (idp) wr16(idp, (u16)r->id);
    if (tp) wr32(tp, r->type);
    if (np) c_to_pstr(r->name ? r->name : "", np, 255);
    res_set_err(noErr);
}

TRAP(GetResAttrs) {
    Res *r = res_by_handle(ARG(0), NULL);
    if (!r) { res_set_err(resNotFound); RET(0); return; }
    res_set_err(noErr);
    RET(r->attrs);
}

TRAP(SizeResource) {
    Res *r = res_by_handle(ARG(0), NULL);
    if (!r) { res_set_err(resNotFound); RET(-1); return; }
    res_set_err(noErr);
    RET(r->handle && hderef(r->handle) ? mm_handle_size(r->handle) : r->len);
}

/* ---- string resources ---- */
TRAP(GetString) { RET(res_get(FOURCC('S','T','R',' '), ARGS16(0))); }

TRAP(GetIndString) {
    u32 out = ARG(0); s16 id = ARGS16(1); s16 index = ARGS16(2);
    wr8(out, 0);
    u32 h = res_get(FOURCC('S','T','R','#'), id);
    if (!h || index < 1) return;
    u32 p = hderef(h);
    u16 n = rd16(p);
    if (index > n) return;
    p += 2;
    for (int i = 1; i < index; i++) p += 1 + rd8(p);
    u8 len = rd8(p);
    gmemmove(out, p, 1u + len);
}
