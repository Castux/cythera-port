#ifndef CONFIG_H
#define CONFIG_H
#include "common.h"

typedef struct {
    const char *data_dir;
    const char *script;
    const char *sysdir;
    const char *soundfont;
    const char *render_pict, *render_out;
    const char *app;
    const char *registered_to; /* --registered: licensee shown by the registration bypass */
    bool fullscreen;  /* application file name in data_dir */
    bool headless;
    bool data_readonly; /* never write into data_dir (packaged builds: it may be a signed app bundle) */
    int screen_w, screen_h;
    int scale;
    int timeout_s;
} Config;

extern Config g_cfg;
#endif
