/* PEF container loader: sections, pattern-initialised data, relocations,
 * import binding. Also recovers function names from traceback tables. */
#include "pef.h"

static u32 unpack_arg(const u8 *raw, size_t *p) {
    u32 v = 0;
    for (;;) {
        u8 b = raw[(*p)++];
        v = (v << 7) | (b & 0x7F);
        if (!(b & 0x80)) return v;
    }
}

static void unpack_pidata(const u8 *raw, size_t rawlen, u8 *out, size_t outlen) {
    size_t p = 0, o = 0;
#define EMIT(src, n) do { if (o + (n) > outlen) fatal("pidata overflow"); memcpy(out + o, src, n); o += (n); } while (0)
#define ZERO(n) do { if (o + (n) > outlen) fatal("pidata overflow"); memset(out + o, 0, n); o += (n); } while (0)
    while (p < rawlen) {
        u8 b = raw[p++];
        int opc = b >> 5;
        u32 cnt = b & 0x1F;
        if (cnt == 0) cnt = unpack_arg(raw, &p);
        switch (opc) {
        case 0: ZERO(cnt); break;
        case 1: EMIT(raw + p, cnt); p += cnt; break;
        case 2: {
            u32 rc = unpack_arg(raw, &p);
            for (u32 i = 0; i <= rc; i++) EMIT(raw + p, cnt);
            p += cnt;
            break;
        }
        case 3: {
            u32 cc = cnt, nc = unpack_arg(raw, &p), rc = unpack_arg(raw, &p);
            const u8 *common = raw + p; p += cc;
            for (u32 i = 0; i < rc; i++) { EMIT(common, cc); EMIT(raw + p, nc); p += nc; }
            EMIT(common, cc);
            break;
        }
        case 4: {
            u32 cc = cnt, nc = unpack_arg(raw, &p), rc = unpack_arg(raw, &p);
            for (u32 i = 0; i < rc; i++) { ZERO(cc); EMIT(raw + p, nc); p += nc; }
            ZERO(cc);
            break;
        }
        default: fatal("bad pidata opcode %d", opc);
        }
    }
#undef EMIT
#undef ZERO
}

typedef struct {
    u32 raddr, imp, sectC, sectD;
    u32 base;           /* guest address of section being relocated */
    const u32 *bases;
    const u32 *impaddr;
    int nimp;
} RelocState;

static void radd(RelocState *s, u32 v) {
    u32 a = s->base + s->raddr;
    wr32(a, rd32(a) + v);
    s->raddr += 4;
}

static int reloc_exec(RelocState *s, const u16 *hw, int i) {
    u16 h = hw[i];
    if ((h >> 14) == 0) {
        u32 skip = (h >> 6) & 0xFF, rc = h & 0x3F;
        s->raddr += skip * 4;
        for (u32 k = 0; k < rc; k++) radd(s, s->sectD);
        return 1;
    }
    if ((h >> 13) == 2) {
        int sub = (h >> 9) & 0xF; u32 n = (h & 0x1FF) + 1;
        for (u32 k = 0; k < n; k++) {
            switch (sub) {
            case 0: radd(s, s->sectC); break;
            case 1: radd(s, s->sectD); break;
            case 2: radd(s, s->sectC); radd(s, s->sectD); s->raddr += 4; break;
            case 3: radd(s, s->sectC); radd(s, s->sectD); break;
            case 4: radd(s, s->sectD); s->raddr += 4; break;
            case 5:
                if ((int)s->imp >= s->nimp) fatal("reloc import index");
                radd(s, s->impaddr[s->imp++]); break;
            default: fatal("reloc run subop %d", sub);
            }
        }
        return 1;
    }
    if ((h >> 13) == 3) {
        int sub = (h >> 9) & 0xF; u32 idx = h & 0x1FF;
        switch (sub) {
        case 0: radd(s, s->impaddr[idx]); s->imp = idx + 1; break;
        case 1: s->sectC = s->bases[idx]; break;
        case 2: s->sectD = s->bases[idx]; break;
        case 3: radd(s, s->bases[idx]); break;
        default: fatal("reloc sm subop %d", sub);
        }
        return 1;
    }
    if ((h >> 12) == 8) { s->raddr += (h & 0xFFF) + 1; return 1; }
    if ((h >> 10) == 0x28) { s->raddr = ((u32)(h & 0x3FF) << 16) | hw[i + 1]; return 2; }
    if ((h >> 10) == 0x29) {
        u32 idx = ((u32)(h & 0x3FF) << 16) | hw[i + 1];
        radd(s, s->impaddr[idx]); s->imp = idx + 1; return 2;
    }
    if ((h >> 10) == 0x2D) {
        int sub = (h >> 6) & 0xF; u32 idx = ((u32)(h & 0x3F) << 16) | hw[i + 1];
        if (sub == 0) radd(s, s->bases[idx]);
        else if (sub == 1) s->sectC = s->bases[idx];
        else if (sub == 2) s->sectD = s->bases[idx];
        return 2;
    }
    fatal("unknown relocation opcode %04x", h);
}

static void run_relocs(RelocState *s, const u16 *hw, int count) {
    int *starts = malloc(sizeof(int) * (size_t)(count + 1));
    int nstarts = 0;
    int i = 0;
    while (i < count) {
        u16 h = hw[i];
        if ((h >> 12) == 9 || (h >> 10) == 0x2C) {
            int blk, rep, len;
            if ((h >> 12) == 9) { blk = ((h >> 8) & 0xF) + 1; rep = (h & 0xFF) + 1; len = 1; }
            else { blk = ((h >> 6) & 0xF) + 1; rep = (int)(((u32)(h & 0x3F) << 16) | hw[i + 1]); len = 2; }
            for (int r = 0; r < rep; r++)
                for (int b = nstarts - blk; b < nstarts; b++) reloc_exec(s, hw, starts[b]);
            starts[nstarts++] = i;
            i += len;
            continue;
        }
        starts[nstarts++] = i;
        i += reloc_exec(s, hw, i);
    }
    free(starts);
}

bool pef_load(const u8 *buf, size_t len, u32 code_addr, u32 data_addr,
              PefResolver resolve, PefImage *out) {
    if (len < 40 || memcmp(buf, "Joy!peffpwpc", 12)) return false;
    int nsec = be16(buf + 32);
    u32 bases[8] = {0};
    const u8 *loader = NULL;
    memset(out, 0, sizeof *out);
    for (int i = 0; i < nsec && i < 8; i++) {
        const u8 *h = buf + 40 + i * 28;
        u32 total = be32(h + 8), unpacked = be32(h + 12), packed = be32(h + 16), off = be32(h + 20);
        u8 kind = h[24];
        if (kind == 0 || kind == 1 || kind == 2) {
            u32 addr = (kind == 0) ? code_addr : data_addr;
            bases[i] = addr;
            gmemset(addr, 0, total);
            if (kind == 2) {
                u8 *tmp = calloc(1, unpacked);
                unpack_pidata(buf + off, packed, tmp, unpacked);
                gmemcpy_to(addr, tmp, unpacked);
                free(tmp);
            } else {
                gmemcpy_to(addr, buf + off, packed);
            }
            if (kind == 0) { out->code_addr = addr; out->code_size = total; }
            else { out->data_addr = addr; out->data_size = total; }
        } else if (kind == 4) {
            loader = buf + off;
        }
    }
    if (!loader) fatal("PEF: no loader section");
    s32 main_sec = (s32)be32(loader + 0);
    u32 main_off = be32(loader + 4);
    u32 nlib = be32(loader + 24), nimp = be32(loader + 28), nrelsec = be32(loader + 32);
    u32 reloc_off = be32(loader + 36), strtab = be32(loader + 40);
    out->main_tvec = main_sec >= 0 ? bases[main_sec] + main_off : 0;

    const u8 *libs = loader + 56;
    const u8 *syms = libs + nlib * 24;
    out->nimports = (int)nimp;
    out->imports = calloc(nimp ? nimp : 1, sizeof(PefImport));
    for (u32 l = 0; l < nlib; l++) {
        const u8 *lh = libs + l * 24;
        const char *lname = (const char *)loader + strtab + be32(lh);
        u32 nsym = be32(lh + 12), first = be32(lh + 16);
        u8 opts = lh[20];
        for (u32 k = first; k < first + nsym; k++) {
            u32 wsym = be32(syms + k * 4);
            PefImport *im = &out->imports[k];
            snprintf(im->lib, sizeof im->lib, "%s", lname);
            snprintf(im->name, sizeof im->name, "%s", (const char *)loader + strtab + (wsym & 0xFFFFFF));
            im->weak = (opts & 0x40) || ((wsym >> 24) & 0x80);
        }
    }
    u32 *impaddr = calloc(nimp ? nimp : 1, sizeof(u32));
    for (u32 k = 0; k < nimp; k++) impaddr[k] = resolve((int)k, &out->imports[k]);

    const u8 *relhdr = syms + nimp * 4;
    for (u32 r = 0; r < nrelsec; r++) {
        const u8 *rh = relhdr + r * 12;
        u16 sidx = be16(rh);
        u32 cnt = be32(rh + 4), first = be32(rh + 8);
        u16 *hw = malloc(sizeof(u16) * cnt);
        for (u32 k = 0; k < cnt; k++) hw[k] = be16(loader + reloc_off + (first + k) * 2);
        RelocState s = { .raddr = 0, .imp = 0, .sectC = bases[0], .sectD = bases[1],
                         .base = bases[sidx], .bases = bases, .impaddr = impaddr, .nimp = (int)nimp };
        run_relocs(&s, hw, (int)cnt);
        free(hw);
    }
    free(impaddr);
    return true;
}

/* ---------------------------------------------------------------------- */
/* Symbol table                                                            */

typedef struct { u32 addr, size; char *name; } Sym;
static Sym *g_syms;
static int g_nsyms, g_capsyms;
static bool g_sorted;

void sym_add(u32 addr, u32 size, const char *name) {
    if (g_nsyms == g_capsyms) {
        g_capsyms = g_capsyms ? g_capsyms * 2 : 1024;
        g_syms = realloc(g_syms, sizeof(Sym) * (size_t)g_capsyms);
    }
    g_syms[g_nsyms++] = (Sym){ addr, size, strdup(name) };
    g_sorted = false;
}

static int sym_cmp(const void *a, const void *b) {
    const Sym *x = a, *y = b;
    return x->addr < y->addr ? -1 : x->addr > y->addr;
}

const char *sym_lookup(u32 addr, u32 *offset) {
    if (!g_sorted) { qsort(g_syms, (size_t)g_nsyms, sizeof(Sym), sym_cmp); g_sorted = true; }
    int lo = 0, hi = g_nsyms - 1, best = -1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (g_syms[mid].addr <= addr) { best = mid; lo = mid + 1; } else hi = mid - 1;
    }
    if (best < 0) return NULL;
    Sym *s = &g_syms[best];
    if (addr >= s->addr + s->size + 64) return NULL;
    if (offset) *offset = addr - s->addr;
    return s->name;
}

u32 sym_find(const char *name) {
    for (int i = 0; i < g_nsyms; i++)
        if (!strcmp(g_syms[i].name, name)) return g_syms[i].addr;
    return 0;
}

/* Demangle "Func__5ClassFargs" into "Class::Func". */
static void demangle(const char *in, char *out, size_t outsz) {
    if (*in == '.') in++;
    const char *p = strstr(in, "__");
    while (p) {
        const char *q = p + 2;
        if (*q >= '0' && *q <= '9') {
            int n = 0;
            while (*q >= '0' && *q <= '9') n = n * 10 + (*q++ - '0');
            if ((int)strlen(q) >= n && p > in) {
                snprintf(out, outsz, "%.*s::%.*s", n, q, (int)(p - in), in);
                return;
            }
        }
        p = strstr(p + 1, "__");
    }
    snprintf(out, outsz, "%s", in);
}

void sym_scan_tracebacks(u32 code_addr, u32 code_size) {
    u32 end = code_addr + code_size;
    u32 o = code_addr;
    int count = 0;
    while (o + 16 < end) {
        if (rd32(o) == 0 && rd8(o + 4) == 0) {
            u8 lang = rd8(o + 5);
            u8 f0 = rd8(o + 6), f1 = rd8(o + 7), fixedp = rd8(o + 10), floatp = rd8(o + 11) >> 1;
            (void)lang;
            u32 p = o + 12;
            if (fixedp || floatp) p += 4;
            if ((f0 & 0x20) && (f1 & 0x40)) {
                u32 tboff = rd32(p); p += 4;
                if (f1 & 0x80) p += 4;
                if (f0 & 0x08) { u32 n = rd32(p); p += 4 + 4 * n; }
                if (p + 2 < end && tboff > 0 && tboff <= o - code_addr) {
                    u16 nl = rd16(p);
                    if (nl > 0 && nl < 256 && p + 2 + nl <= end) {
                        char raw[260], dem[300];
                        memcpy(raw, g_mem + p + 2, nl); raw[nl] = 0;
                        demangle(raw, dem, sizeof dem);
                        sym_add(o - tboff, tboff, dem);
                        count++;
                        o = (p + 2 + nl + 3) & ~3u;
                        continue;
                    }
                }
            }
        }
        o += 4;
    }
    LOG_I("symbols: %d functions from traceback tables", count);
}
