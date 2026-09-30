/* Common definitions for the Delver runtime (Cythera port). */
#ifndef COMMON_H
#define COMMON_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int8_t s8;
typedef int16_t s16;
typedef int32_t s32;
typedef int64_t s64;

/* ---- Guest address space layout -------------------------------------- */
#define GUEST_MEM_SIZE   0x10000000u   /* 256 MB */
#define LOWMEM_END       0x00004000u
#define TRAP_BASE        0x00010000u   /* import trap entry points (4 bytes each) */
#define TRAP_COUNT       0x00002000u   /* max trap slots */
#define TRAP_END         (TRAP_BASE + TRAP_COUNT * 4)
#define RET_SENTINEL     (TRAP_END - 4) /* return address for host->guest calls */
#define TVEC_BASE        0x00020000u   /* TVectors for imports (8 bytes each) */
#define CODE_ADDR        0x00100000u
#define DATA_ADDR        0x00200000u
#define SYSZONE_START    0x00400000u   /* OS-owned structures */
#define SYSZONE_END      0x01000000u
#define APPZONE_START    0x01000000u   /* application heap */
#define APPZONE_END      0x0BF00000u
#define FAKEROM_START    0x0BF00000u   /* zero-filled, what low-memory vectors point to */
#define FAKEROM_END      0x0C000000u
#define STACK_AREA_START 0x0C000000u
#define STACK_AREA_END   0x10000000u

extern u8 *g_mem;

/* ---- Logging ----------------------------------------------------------- */
extern int g_log_level; /* 0 error, 1 warn, 2 info, 3 debug, 4 trace */
void log_msg(int level, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
#define LOG_E(...) log_msg(0, __VA_ARGS__)
#define LOG_W(...) log_msg(1, __VA_ARGS__)
#define LOG_I(...) log_msg(2, __VA_ARGS__)
#define LOG_D(...) do { if (g_log_level >= 3) log_msg(3, __VA_ARGS__); } while (0)
#define LOG_T(...) do { if (g_log_level >= 4) log_msg(4, __VA_ARGS__); } while (0)

_Noreturn void fatal(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
extern void (*g_fatal_hook)(const char *msg); /* also report fatal errors, e.g. in a dialog */

/* ---- Guest memory access (big-endian) ---------------------------------- */
_Noreturn void mem_fault(u32 addr, u32 size, bool write);

static inline void mchk(u32 a, u32 n, bool w) {
    if (__builtin_expect((u64)a + n > GUEST_MEM_SIZE, 0)) mem_fault(a, n, w);
}
static inline u8 rd8(u32 a) { mchk(a, 1, false); return g_mem[a]; }
static inline u16 rd16(u32 a) { mchk(a, 2, false); return (u16)(g_mem[a] << 8 | g_mem[a + 1]); }
static inline u32 rd32(u32 a) {
    mchk(a, 4, false);
    u32 v; memcpy(&v, g_mem + a, 4); return __builtin_bswap32(v);
}
static inline u64 rd64(u32 a) {
    mchk(a, 8, false);
    u64 v; memcpy(&v, g_mem + a, 8); return __builtin_bswap64(v);
}
static inline void wr8(u32 a, u8 v) { mchk(a, 1, true); g_mem[a] = v; }
static inline void wr16(u32 a, u16 v) { mchk(a, 2, true); g_mem[a] = (u8)(v >> 8); g_mem[a + 1] = (u8)v; }
static inline void wr32(u32 a, u32 v) { mchk(a, 4, true); v = __builtin_bswap32(v); memcpy(g_mem + a, &v, 4); }
static inline void wr64(u32 a, u64 v) { mchk(a, 8, true); v = __builtin_bswap64(v); memcpy(g_mem + a, &v, 8); }
static inline s16 rds16(u32 a) { return (s16)rd16(a); }
static inline s8 rds8(u32 a) { return (s8)rd8(a); }

/* Host pointer into guest memory (bounds-checked for n bytes). */
static inline void *gptr(u32 a, u32 n) { mchk(a, n, false); return g_mem + a; }

void gmemcpy_to(u32 dst, const void *src, u32 n);   /* host -> guest */
void gmemcpy_from(void *dst, u32 src, u32 n);       /* guest -> host */
void gmemset(u32 dst, u8 v, u32 n);
void gmemmove(u32 dst, u32 src, u32 n);

/* Pascal / C strings in guest memory */
void pstr_to_c(u32 pstr, char *out, size_t outsz);
void c_to_pstr(const char *s, u32 pstr, int maxlen);
size_t gstrlen(u32 a);

/* Big-endian helpers for host buffers */
static inline u16 be16(const u8 *p) { return (u16)(p[0] << 8 | p[1]); }
static inline u32 be32(const u8 *p) { return (u32)p[0] << 24 | (u32)p[1] << 16 | (u32)p[2] << 8 | p[3]; }
static inline void put_be16(u8 *p, u16 v) { p[0] = (u8)(v >> 8); p[1] = (u8)v; }
static inline void put_be32(u8 *p, u32 v) { p[0] = (u8)(v >> 24); p[1] = (u8)(v >> 16); p[2] = (u8)(v >> 8); p[3] = (u8)v; }

#define FOURCC(a, b, c, d) ((u32)(a) << 24 | (u32)(b) << 16 | (u32)(c) << 8 | (u32)(d))
const char *fourcc_str(u32 t); /* returns static buffer (rotating) */

/* Mac OSErr codes used throughout */
enum {
    noErr = 0, paramErr = -50, memFullErr = -108, nilHandleErr = -109,
    memWZErr = -111, resNotFound = -192, resFNotFound = -193, fnfErr = -43,
    eofErr = -39, dupFNErr = -48, fnOpnErr = -38, ioErr = -36, bdNamErr = -37,
    permErr = -54, dirNFErr = -120, nsvErr = -35, wrPermErr = -61, opWrErr = -49,
    posErr = -40, tmfoErr = -42, dskFulErr = -34, fLckdErr = -45, notOpenErr = -28,
    gestaltUndefSelectorErr = -5551, unimpErr = -4, userCanceledErr = -128,
    errAEEventNotHandled = -1708, rfNumErr = -51, mapReadErr = -199,
    addResFailed = -194, rmvResFailed = -196, resAttrErr = -198
};

#endif
