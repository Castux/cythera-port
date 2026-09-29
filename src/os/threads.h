#ifndef THREADS_H
#define THREADS_H
#include "os.h"
void threads_init(CPU *main_cpu);
void thread_yield(void);
u32 thread_current_id(void);
#endif
