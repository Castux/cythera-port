#ifndef MISC_H
#define MISC_H
#include "os.h"
void misc_init(void);
void misc_poll(void);
u32 tick_count(void);
u64 host_now_us(void);
void irq_post(u32 upp, u32 arg);
void irq_service(void);
void guest_call_async(u32 upp, int n, const u32 *args);
extern bool g_deterministic;
void vclock_advance_ns(u64 ns);
void vclock_idle(void);
void wait_vbl(void);
#endif
