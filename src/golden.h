/* Golden traces for regression tests (golden.c). */
#ifndef GOLDEN_H
#define GOLDEN_H
#include "common.h"

struct CPU;
void golden_open(const char *path);
bool golden_on(void);
void golden_trap_enter(const struct CPU *c);
void golden_trap_leave(const struct CPU *c, u32 index);
void golden_mark(int line, const char *cmd, const char *arg);
void golden_exit(void);
#endif
