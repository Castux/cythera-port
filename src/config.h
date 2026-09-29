#ifndef CONFIG_H
#define CONFIG_H
#include "common.h"

typedef struct {
    const char *data_dir;
    const char *script;
    const char *sysdir;
    const char *soundfont;
    const char *render_pict, *render_out;
    const char *app;  /* application file name in data_dir */
    bool headless;
    int screen_w, screen_h;
    int scale;
    int timeout_s;
} Config;

extern Config g_cfg;
#endif
