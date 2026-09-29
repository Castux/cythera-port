#ifndef PEF_H
#define PEF_H
#include "../common.h"

typedef struct {
    char lib[64];
    char name[96];
    bool weak;
} PefImport;

typedef struct {
    u32 code_addr, code_size;
    u32 data_addr, data_size;
    u32 main_tvec;       /* guest address of main's TVector */
    int nimports;
    PefImport *imports;
} PefImage;

/* resolve(import) returns the guest address to relocate against
   (normally a TVector), or 0 for an unresolved weak import. */
typedef u32 (*PefResolver)(int index, const PefImport *imp);

bool pef_load(const u8 *buf, size_t len, u32 code_addr, u32 data_addr,
              PefResolver resolve, PefImage *out);

/* ---- symbols (from CodeWarrior traceback tables) ---- */
void sym_scan_tracebacks(u32 code_addr, u32 code_size);
void sym_add(u32 addr, u32 size, const char *name);
const char *sym_lookup(u32 addr, u32 *offset); /* NULL if unknown */
u32 sym_find(const char *name);                /* 0 if not found */
#endif
