/* Host platform layer (SDL2). */
#ifndef HOST_H
#define HOST_H
#include "../common.h"

typedef enum {
    HEV_NONE, HEV_MOUSE_DOWN, HEV_MOUSE_UP, HEV_KEY_DOWN, HEV_KEY_UP, HEV_QUIT,
    HEV_FOCUS_IN, HEV_FOCUS_OUT
} HostEventType;

typedef struct {
    HostEventType type;
    int x, y;          /* screen coordinates (guest pixels) */
    u8 mac_key;        /* Mac virtual key code */
    u8 ch;             /* Mac Roman character */
    u16 mods;          /* Mac modifier bits (cmdKey etc.) */
    bool repeat;
} HostEvent;

void host_init(int w, int h, bool headless, int scale);
void host_shutdown(void);
/* Process pending host events; if `wait`, sleep briefly when idle. */
void host_pump(bool wait);
bool host_next_event(HostEvent *ev);
void host_mouse(int *x, int *y, bool *down);
u16 host_modifiers(void);
/* KeyMap (16 bytes, Mac layout) */
void host_keymap(u8 out[16]);

/* Present an 8-bit indexed framebuffer with a 256-entry RGB palette. */
void host_present(const u8 *pixels, int pitch, int w, int h, const u32 *palette_rgb);
/* Save the current frame as a PNG (for scripted tests). */
bool host_screenshot(const char *path);

void host_set_cursor(const u8 *rgba32x32, int hotx, int hoty, bool visible);
bool host_is_headless(void);

/* Clipboard text (UTF-8). get returns a malloc'd string, or NULL if empty. */
char *host_clipboard_get(void);
void host_clipboard_set(const char *utf8);

/* Scripted input (tests) */
void script_load(const char *path);
void script_tick(void);
#endif
