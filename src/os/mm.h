/* Memory Manager internal API. */
#ifndef MM_H
#define MM_H
#include "os.h"

enum { ZONE_APP = 0, ZONE_SYS = 1 };

/* Handle state flags (HGetState) */
#define HS_LOCKED    0x80
#define HS_PURGEABLE 0x40
#define HS_RESOURCE  0x20

void mm_init(void);
u32 mm_new_ptr(u32 size, bool clear, int zone);
void mm_dispose_ptr(u32 p);
u32 mm_ptr_size(u32 p);
bool mm_is_block(u32 p);

u32 mm_new_handle(u32 size, bool clear, int zone);
u32 mm_new_empty_handle(int zone);
void mm_dispose_handle(u32 h);
u32 mm_handle_size(u32 h);
bool mm_set_handle_size(u32 h, u32 size);   /* false on failure */
bool mm_reallocate_handle(u32 h, u32 size); /* for empty handles */
void mm_empty_handle(u32 h);
u8 mm_hgetstate(u32 h);
void mm_hsetstate(u32 h, u8 s);
u32 mm_recover_handle(u32 p);
bool mm_is_handle(u32 h);
u32 mm_handle_from_data(const void *data, u32 size, int zone);

static inline u32 hderef(u32 h) { return rd32(h); }
void mm_set_memerr(s16 err);

/* Scratch allocation for OS use (never freed while referenced). */
static inline u32 sys_alloc(u32 size) { return mm_new_ptr(size, true, ZONE_SYS); }

u32 mm_free_bytes(int zone);
u32 mm_max_block(int zone);
#endif
