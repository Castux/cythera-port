#ifndef CONFIG_H
#define CONFIG_H
#include "common.h"

typedef struct {
    const char *data_dir;
    const char *script;
    const char *sysdir;
    bool headless;
    int screen_w, screen_h;
    int scale;
    int timeout_s;
} Config;

extern Config g_cfg;
#endif
