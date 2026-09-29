/* Memory Manager: nonrelocatable blocks (Ptr) and relocatable blocks
 * (Handle) in guest memory.
 *
 * Every block has a 16-byte header just before its data:
 *   +0  magic   'PtrB' or 'HdlB'
 *   +4  logical size
 *   +8  capacity (usable bytes)
 *   +12 owning master pointer (handles) or 0
 * Master pointers live in a reserved area at the start of each zone.
 * Handle state flags are kept host-side, indexed by master pointer slot.
 * Blocks never move unless a handle must grow and the space after it is
 * taken.  Purgeable handles are never purged (memory is plentiful).
 */
#include "mm.h"

#define MAGIC_PTR 0x50747242u /* 'PtrB' */
#define MAGIC_HDL 0x48646C42u /* 'HdlB' */
#define HDR 16u
#define LOWMEM_MemErr 0x0220

typedef struct { u32 addr, size; } Extent;

typedef struct {
    u32 start, end;             /* allocatable range */
    u32 mp_start, mp_count;     /* master pointer area */
    u8 *mp_flags;               /* host-side state per master pointer */
    u8 *mp_used;
    u32 mp_next;                /* scan hint */
    Extent *free;               /* sorted by address */
    int nfree, capfree;
    u32 fake_zone;              /* guest THz */
} Zone;

static Zone g_zones[2];

void mm_set_memerr(s16 err) { wr16(LOWMEM_MemErr, (u16)err); }

static void zone_init(Zone *z, u32 start, u32 end, u32 nmp) {
    z->mp_start = start;
    z->mp_count = nmp;
    z->mp_flags = calloc(nmp, 1);
    z->mp_used = calloc(nmp, 1);
    z->start = (start + nmp * 4 + 15) & ~15u;
    z->end = end;
    z->capfree = 256;
    z->free = malloc(sizeof(Extent) * (size_t)z->capfree);
    z->free[0] = (Extent){ z->start, z->end - z->start };
    z->nfree = 1;
}

static void free_insert(Zone *z, u32 addr, u32 size) {
    /* binary search insertion point */
    int lo = 0, hi = z->nfree;
    while (lo < hi) { int mid = (lo + hi) / 2; if (z->free[mid].addr < addr) lo = mid + 1; else hi = mid; }
    int i = lo;
    bool merge_prev = i > 0 && z->free[i - 1].addr + z->free[i - 1].size == addr;
    bool merge_next = i < z->nfree && addr + size == z->free[i].addr;
    if (merge_prev && merge_next) {
        z->free[i - 1].size += size + z->free[i].size;
        memmove(&z->free[i], &z->free[i + 1], sizeof(Extent) * (size_t)(z->nfree - i - 1));
        z->nfree--;
    } else if (merge_prev) {
        z->free[i - 1].size += size;
    } else if (merge_next) {
        z->free[i].addr = addr; z->free[i].size += size;
    } else {
        if (z->nfree == z->capfree) {
            z->capfree *= 2;
            z->free = realloc(z->free, sizeof(Extent) * (size_t)z->capfree);
        }
        memmove(&z->free[i + 1], &z->free[i], sizeof(Extent) * (size_t)(z->nfree - i));
        z->free[i] = (Extent){ addr, size };
        z->nfree++;
    }
}

/* Allocate a raw range of `size` bytes (multiple of 16). Returns 0 if none. */
static u32 raw_alloc(Zone *z, u32 size) {
    for (int i = 0; i < z->nfree; i++) {
        if (z->free[i].size >= size) {
            u32 a = z->free[i].addr;
            z->free[i].addr += size;
            z->free[i].size -= size;
            if (z->free[i].size == 0) {
                memmove(&z->free[i], &z->free[i + 1], sizeof(Extent) * (size_t)(z->nfree - i - 1));
                z->nfree--;
            }
            return a;
        }
    }
    return 0;
}

/* Try to extend the range [addr, addr+size) in place by `extra` bytes. */
static bool raw_extend(Zone *z, u32 addr, u32 size, u32 extra) {
    u32 end = addr + size;
    int lo = 0, hi = z->nfree;
    while (lo < hi) { int mid = (lo + hi) / 2; if (z->free[mid].addr < end) lo = mid + 1; else hi = mid; }
    if (lo < z->nfree && z->free[lo].addr == end && z->free[lo].size >= extra) {
        z->free[lo].addr += extra;
        z->free[lo].size -= extra;
        if (z->free[lo].size == 0) {
            memmove(&z->free[lo], &z->free[lo + 1], sizeof(Extent) * (size_t)(z->nfree - lo - 1));
            z->nfree--;
        }
        return true;
    }
    return false;
}

static Zone *zone_of(u32 addr) {
    for (int i = 0; i < 2; i++)
        if (addr >= g_zones[i].mp_start && addr < g_zones[i].end) return &g_zones[i];
    return NULL;
}

static inline u32 round16(u32 n) { return (n + 15) & ~15u; }

/* Allocate a block with header; returns data address or 0. */
static u32 block_alloc(Zone *z, u32 size, u32 magic, u32 owner, bool clear) {
    u32 cap = round16(size ? size : 1);
    u32 a = raw_alloc(z, cap + HDR);
    if (!a) return 0;
    wr32(a, magic); wr32(a + 4, size); wr32(a + 8, cap); wr32(a + 12, owner);
    if (clear) gmemset(a + HDR, 0, cap);
    return a + HDR;
}

static bool block_valid(u32 p, u32 magic) {
    if (p < HDR + LOWMEM_END || p >= GUEST_MEM_SIZE) return false;
    return rd32(p - HDR) == magic;
}

static void block_free(u32 p) {
    Zone *z = zone_of(p);
    u32 cap = rd32(p - 8);
    wr32(p - HDR, 0xDEADBEEF);
    free_insert(z, p - HDR, cap + HDR);
}

void mm_init(void) {
    zone_init(&g_zones[ZONE_SYS], SYSZONE_START, SYSZONE_END, 16384);
    zone_init(&g_zones[ZONE_APP], APPZONE_START, APPZONE_END, 262144);
    for (int i = 0; i < 2; i++) {
        /* A small fake zone header so GetZone() returns something sensible. */
        u32 zh = block_alloc(&g_zones[ZONE_SYS], 64, MAGIC_PTR, 0, true);
        wr32(zh, g_zones[i].end);      /* bkLim */
        g_zones[i].fake_zone = zh;
    }
    wr32(0x02AA, g_zones[ZONE_APP].fake_zone); /* ApplZone */
    wr32(0x02A6, g_zones[ZONE_SYS].fake_zone); /* SysZone */
    wr32(0x0118, g_zones[ZONE_APP].fake_zone); /* TheZone */
    wr32(0x0130, APPZONE_END);                 /* ApplLimit */
    wr32(0x0114, APPZONE_END);                 /* HeapEnd */
}

/* ---- pointers ---- */
u32 mm_new_ptr(u32 size, bool clear, int zone) {
    u32 p = block_alloc(&g_zones[zone], size, MAGIC_PTR, 0, clear);
    mm_set_memerr(p ? noErr : memFullErr);
    if (!p) LOG_W("NewPtr(%u) failed", size);
    return p;
}

void mm_dispose_ptr(u32 p) {
    if (!p) return;
    if (!block_valid(p, MAGIC_PTR)) { LOG_W("DisposePtr(%08x): not a pointer block", p); mm_set_memerr(paramErr); return; }
    block_free(p);
    mm_set_memerr(noErr);
}

u32 mm_ptr_size(u32 p) {
    if (!block_valid(p, MAGIC_PTR) && !block_valid(p, MAGIC_HDL)) { mm_set_memerr(paramErr); return 0; }
    mm_set_memerr(noErr);
    return rd32(p - 12);
}

bool mm_is_block(u32 p) { return block_valid(p, MAGIC_PTR) || block_valid(p, MAGIC_HDL); }

/* ---- handles ---- */
static u32 mp_alloc(Zone *z) {
    for (u32 n = 0; n < z->mp_count; n++) {
        u32 i = (z->mp_next + n) % z->mp_count;
        if (!z->mp_used[i]) {
            z->mp_used[i] = 1;
            z->mp_flags[i] = 0;
            z->mp_next = i + 1;
            u32 h = z->mp_start + 4 * i;
            wr32(h, 0);
            return h;
        }
    }
    fatal("out of master pointers");
}

static bool mp_index(u32 h, Zone **zo, u32 *idx) {
    for (int i = 0; i < 2; i++) {
        Zone *z = &g_zones[i];
        if (h >= z->mp_start && h < z->mp_start + 4 * z->mp_count && !((h - z->mp_start) & 3)) {
            u32 k = (h - z->mp_start) / 4;
            if (!z->mp_used[k]) return false;
            *zo = z; *idx = k;
            return true;
        }
    }
    return false;
}

bool mm_is_handle(u32 h) { Zone *z; u32 k; return mp_index(h, &z, &k); }

u32 mm_new_empty_handle(int zone) {
    Zone *z = &g_zones[zone];
    u32 h = mp_alloc(z);
    mm_set_memerr(noErr);
    return h;
}

u32 mm_new_handle(u32 size, bool clear, int zone) {
    Zone *z = &g_zones[zone];
    u32 h = mp_alloc(z);
    u32 p = block_alloc(z, size, MAGIC_HDL, h, clear);
    if (!p) {
        z->mp_used[(h - z->mp_start) / 4] = 0;
        mm_set_memerr(memFullErr);
        LOG_W("NewHandle(%u) failed", size);
        return 0;
    }
    wr32(h, p);
    mm_set_memerr(noErr);
    return h;
}

u32 mm_handle_from_data(const void *data, u32 size, int zone) {
    u32 h = mm_new_handle(size, false, zone);
    if (h && size) gmemcpy_to(hderef(h), data, size);
    return h;
}

void mm_dispose_handle(u32 h) {
    Zone *z; u32 k;
    if (!h) { mm_set_memerr(nilHandleErr); return; }
    if (!mp_index(h, &z, &k)) { LOG_W("DisposeHandle(%08x): not a handle", h); mm_set_memerr(nilHandleErr); return; }
    u32 p = rd32(h);
    if (p) block_free(p);
    wr32(h, 0);
    z->mp_used[k] = 0;
    z->mp_flags[k] = 0;
    mm_set_memerr(noErr);
}

u32 mm_handle_size(u32 h) {
    Zone *z; u32 k;
    if (!h || !mp_index(h, &z, &k)) { mm_set_memerr(nilHandleErr); return 0; }
    u32 p = rd32(h);
    mm_set_memerr(noErr);
    if (!p) return 0;
    return rd32(p - 12);
}

bool mm_reallocate_handle(u32 h, u32 size) {
    Zone *z; u32 k;
    if (!mp_index(h, &z, &k)) { mm_set_memerr(nilHandleErr); return false; }
    u32 p = rd32(h);
    if (p) block_free(p);
    p = block_alloc(z, size, MAGIC_HDL, h, false);
    wr32(h, p);
    mm_set_memerr(p ? noErr : memFullErr);
    return p != 0;
}

void mm_empty_handle(u32 h) {
    Zone *z; u32 k;
    if (!mp_index(h, &z, &k)) return;
    u32 p = rd32(h);
    if (p) block_free(p);
    wr32(h, 0);
}

bool mm_set_handle_size(u32 h, u32 size) {
    Zone *z; u32 k;
    if (!h || !mp_index(h, &z, &k)) { mm_set_memerr(nilHandleErr); return false; }
    u32 p = rd32(h);
    if (!p) return mm_reallocate_handle(h, size);
    u32 cap = rd32(p - 8);
    if (size <= cap) { wr32(p - 12, size); mm_set_memerr(noErr); return true; }
    u32 ncap = round16(size);
    if (raw_extend(z, p - HDR, cap + HDR, ncap - cap)) {
        wr32(p - 12, size); wr32(p - 8, ncap);
        mm_set_memerr(noErr);
        return true;
    }
    if (z->mp_flags[k] & HS_LOCKED)
        LOG_D("SetHandleSize: moving locked handle %08x (%u -> %u)", h, rd32(p - 12), size);
    /* grow with some slack to make repeated growth cheap */
    u32 want = size + (size >> 3);
    u32 np = block_alloc(z, want, MAGIC_HDL, h, false);
    if (!np) { mm_set_memerr(memFullErr); return false; }
    wr32(np - 12, size);
    gmemmove(np, p, rd32(p - 12));
    block_free(p);
    wr32(h, np);
    mm_set_memerr(noErr);
    return true;
}

u8 mm_hgetstate(u32 h) {
    Zone *z; u32 k;
    if (!mp_index(h, &z, &k)) { mm_set_memerr(nilHandleErr); return 0; }
    mm_set_memerr(noErr);
    return z->mp_flags[k];
}

void mm_hsetstate(u32 h, u8 s) {
    Zone *z; u32 k;
    if (!mp_index(h, &z, &k)) { mm_set_memerr(nilHandleErr); return; }
    z->mp_flags[k] = s;
    mm_set_memerr(noErr);
}

u32 mm_recover_handle(u32 p) {
    if (!block_valid(p, MAGIC_HDL)) return 0;
    return rd32(p - 4);
}

u32 mm_free_bytes(int zone) {
    u32 t = 0;
    for (int i = 0; i < g_zones[zone].nfree; i++) t += g_zones[zone].free[i].size;
    return t;
}
u32 mm_max_block(int zone) {
    u32 m = 0;
    for (int i = 0; i < g_zones[zone].nfree; i++) if (g_zones[zone].free[i].size > m) m = g_zones[zone].free[i].size;
    return m > HDR ? m - HDR : 0;
}

/* ---------------------------------------------------------------------- */
/* Traps                                                                   */

static int cur_zone(void) {
    return rd32(0x0118) == g_zones[ZONE_SYS].fake_zone ? ZONE_SYS : ZONE_APP;
}

TRAP(NewPtr) { RET(mm_new_ptr(ARG(0), false, cur_zone())); }
TRAP(NewPtrClear) { RET(mm_new_ptr(ARG(0), true, cur_zone())); }
TRAP(NewPtrSys) { RET(mm_new_ptr(ARG(0), false, ZONE_SYS)); }
TRAP(NewPtrSysClear) { RET(mm_new_ptr(ARG(0), true, ZONE_SYS)); }
TRAP(DisposePtr) { mm_dispose_ptr(ARG(0)); }
TRAP(GetPtrSize) { RET(mm_ptr_size(ARG(0))); }
TRAP(SetPtrSize) {
    u32 p = ARG(0), size = ARG(1);
    if (block_valid(p, MAGIC_PTR) && size <= rd32(p - 8)) { wr32(p - 12, size); mm_set_memerr(noErr); }
    else mm_set_memerr(memFullErr);
}
TRAP(NewHandle) { RET(mm_new_handle(ARG(0), false, cur_zone())); }
TRAP(NewHandleClear) { RET(mm_new_handle(ARG(0), true, cur_zone())); }
TRAP(NewHandleSys) { RET(mm_new_handle(ARG(0), false, ZONE_SYS)); }
TRAP(NewEmptyHandle) { RET(mm_new_empty_handle(cur_zone())); }
TRAP(TempNewHandle) {
    u32 size = ARG(0), errp = ARG(1);
    u32 h = mm_new_handle(size, false, ZONE_APP);
    if (errp) wr16(errp, (u16)(h ? noErr : memFullErr));
    RET(h);
}
TRAP(DisposeHandle) { mm_dispose_handle(ARG(0)); }
TRAP(GetHandleSize) { RET(mm_handle_size(ARG(0))); }
TRAP(InlineGetHandleSize) { RET(mm_handle_size(ARG(0))); }
TRAP(SetHandleSize) { mm_set_handle_size(ARG(0), ARG(1)); }
TRAP(ReallocateHandle) { mm_reallocate_handle(ARG(0), ARG(1)); }
TRAP(EmptyHandle) { mm_empty_handle(ARG(0)); mm_set_memerr(noErr); }
TRAP(HLock) { u32 h = ARG(0); mm_hsetstate(h, mm_hgetstate(h) | HS_LOCKED); }
TRAP(HUnlock) { u32 h = ARG(0); mm_hsetstate(h, mm_hgetstate(h) & ~HS_LOCKED); }
TRAP(HLockHi) { u32 h = ARG(0); mm_hsetstate(h, mm_hgetstate(h) | HS_LOCKED); }
TRAP(MoveHHi) { mm_set_memerr(noErr); }
TRAP(HPurge) { u32 h = ARG(0); mm_hsetstate(h, mm_hgetstate(h) | HS_PURGEABLE); }
TRAP(HNoPurge) { u32 h = ARG(0); mm_hsetstate(h, mm_hgetstate(h) & ~HS_PURGEABLE); }
TRAP(HGetState) { RET(mm_hgetstate(ARG(0))); }
TRAP(HSetState) { mm_hsetstate(ARG(0), (u8)ARG(1)); }
TRAP(RecoverHandle) { RET(mm_recover_handle(ARG(0))); }
TRAP(MemError) { RETERR(rds16(LOWMEM_MemErr)); }
TRAP(FreeMem) { RET(mm_free_bytes(cur_zone())); }
TRAP(MaxMem) { u32 grow = ARG(0); if (grow) wr32(grow, 0); RET(mm_max_block(cur_zone())); }
TRAP(MaxBlock) { RET(mm_max_block(cur_zone())); }
TRAP(MaxApplZone) { mm_set_memerr(noErr); }
TRAP(MoreMasters) { mm_set_memerr(noErr); }
TRAP(SetGrowZone) { }
TRAP(InitZone) { }
TRAP(GetZone) { RET(rd32(0x0118)); }
TRAP(SetZone) { wr32(0x0118, ARG(0)); }
TRAP(SystemZone) { RET(g_zones[ZONE_SYS].fake_zone); }
TRAP(ApplicationZone) { RET(g_zones[ZONE_APP].fake_zone); }
TRAP(CompactMem) { RET(mm_max_block(cur_zone())); }
TRAP(PurgeMem) { }
TRAP(BlockMove) { gmemmove(ARG(1), ARG(0), ARG(2)); }
TRAP(BlockMoveData) { gmemmove(ARG(1), ARG(0), ARG(2)); }
TRAP(BlockZero) { gmemset(ARG(0), 0, ARG(1)); }

TRAP(HandToHand) {
    u32 hp = ARG(0);
    u32 src = rd32(hp);
    u32 n = mm_handle_size(src);
    u32 h = mm_new_handle(n, false, ZONE_APP);
    if (!h) { RETERR(memFullErr); return; }
    if (n) gmemmove(hderef(h), hderef(src), n);
    wr32(hp, h);
    RETERR(noErr);
}
TRAP(PtrToHand) {
    u32 src = ARG(0), hp = ARG(1), n = ARG(2);
    u32 h = mm_new_handle(n, false, ZONE_APP);
    if (!h) { RETERR(memFullErr); return; }
    if (n) gmemmove(hderef(h), src, n);
    wr32(hp, h);
    RETERR(noErr);
}
TRAP(PtrToXHand) {
    u32 src = ARG(0), h = ARG(1), n = ARG(2);
    if (!mm_set_handle_size(h, n)) { RETERR(memFullErr); return; }
    gmemmove(hderef(h), src, n);
    RETERR(noErr);
}
TRAP(PtrAndHand) {
    u32 src = ARG(0), h = ARG(1), n = ARG(2);
    u32 old = mm_handle_size(h);
    if (!mm_set_handle_size(h, old + n)) { RETERR(memFullErr); return; }
    gmemmove(hderef(h) + old, src, n);
    RETERR(noErr);
}
TRAP(HandAndHand) {
    u32 a = ARG(0), b = ARG(1);
    u32 na = mm_handle_size(a), nb = mm_handle_size(b);
    if (!mm_set_handle_size(b, na + nb)) { RETERR(memFullErr); return; }
    gmemmove(hderef(b) + nb, hderef(a), na);
    RETERR(noErr);
}

/* Munger: search-and-replace inside a handle. */
TRAP(Munger) {
    u32 h = ARG(0); s32 offset = (s32)ARG(1);
    u32 p1 = ARG(2); s32 len1 = (s32)ARG(3);
    u32 p2 = ARG(4); s32 len2 = (s32)ARG(5);
    s32 size = (s32)mm_handle_size(h);
    if (offset > size) { RET(-1); return; }
    s32 found;
    if (!p1) {
        found = offset;
        if (len1 < 0 || offset + len1 > size) len1 = size - offset;
    } else {
        if (len1 < 0) len1 = size - offset;
        found = -1;
        u32 base = hderef(h);
        for (s32 i = offset; i + len1 <= size; i++) {
            bool m = true;
            for (s32 j = 0; j < len1 && m; j++) m = rd8(base + (u32)(i + j)) == rd8(p1 + (u32)j);
            if (m) { found = i; break; }
        }
        if (found < 0) { RET(-1); return; }
    }
    if (!p2) { RET(found); return; }
    s32 delta = len2 - len1;
    if (delta > 0) mm_set_handle_size(h, (u32)(size + delta));
    u32 base = hderef(h);
    gmemmove(base + (u32)(found + len2), base + (u32)(found + len1), (u32)(size - found - len1));
    if (len2) gmemmove(base + (u32)found, p2, (u32)len2);
    if (delta < 0) mm_set_handle_size(h, (u32)(size + delta));
    RET(found + len2);
}
