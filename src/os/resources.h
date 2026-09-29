#ifndef RESOURCES_H
#define RESOURCES_H
#include "os.h"

void res_init(void);
/* Open the application's resource fork (host path of data file). */
s16 res_open_app(const char *hostpath);
/* Look up a resource through the current chain; returns handle or 0. */
u32 res_get(u32 type, s16 id);
u32 res_get1(u32 type, s16 id);
u32 res_get_named(u32 type, const char *name);
void res_release(u32 h);
void res_detach(u32 h);
bool res_info(u32 h, u32 *type, s16 *id, char *name);
s16 res_cur_file(void);
void res_set_err(s16 e);
/* Add a built-in "System file" resource (bottom of the chain). */
void res_add_system(u32 type, s16 id, const char *name, const void *data, u32 len);
/* Host-side copy of resource data (no handle); returns malloc'd buffer. */
u8 *res_load_raw(u32 type, s16 id, u32 *len);
#endif
