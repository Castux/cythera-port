/* SDL2 host: window, framebuffer presentation, input translation,
 * screenshots and scripted input for automated tests. */
#include "host.h"
#include <SDL.h>
#include "../os/misc.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBIW_ASSERT(x) ((void)0)
#include "../../third_party/stb_image_write.h"

static SDL_Window *g_win;
static SDL_Renderer *g_ren;
static SDL_Texture *g_tex;
static int g_w, g_h;
static bool g_headless;
static u32 *g_rgb;           /* last presented frame (ARGB) */
static SDL_Cursor *g_cursor;

#define EVQ 256
static HostEvent g_evq[EVQ];
static int g_evh, g_evt;
static int g_mx, g_my;
static bool g_mdown;
static u8 g_keymap[16];
static u16 g_mods;

bool host_is_headless(void) { return g_headless; }

static char *g_clip; /* headless clipboard */
char *host_clipboard_get(void) {
    if (g_headless) return g_clip ? strdup(g_clip) : NULL;
    if (!SDL_HasClipboardText()) return NULL;
    char *t = SDL_GetClipboardText(), *r = (t && *t) ? strdup(t) : NULL;
    SDL_free(t);
    return r;
}
void host_clipboard_set(const char *utf8) {
    if (g_headless) { free(g_clip); g_clip = strdup(utf8); return; }
    SDL_SetClipboardText(utf8);
}

static void push_event(HostEvent e) {
    int n = (g_evt + 1) % EVQ;
    if (n == g_evh) return;
    g_evq[g_evt] = e;
    g_evt = n;
}

bool host_next_event(HostEvent *ev) {
    if (g_evh == g_evt) return false;
    *ev = g_evq[g_evh];
    g_evh = (g_evh + 1) % EVQ;
    return true;
}

void host_init(int w, int h, bool headless, int scale) {
    g_w = w; g_h = h; g_headless = headless;
    g_rgb = calloc((size_t)w * (size_t)h, 4);
    g_mx = w / 2; g_my = h / 2;
    if (headless) return;
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_EVENTS) < 0) fatal("SDL_Init: %s", SDL_GetError());
    if (scale <= 0) {
        SDL_DisplayMode dm;
        scale = 1;
        if (!SDL_GetDesktopDisplayMode(0, &dm)) {
            while ((scale + 1) * w <= dm.w * 9 / 10 && (scale + 1) * h <= dm.h * 9 / 10) scale++;
        }
    }
    g_win = SDL_CreateWindow("Cythera", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, w * scale, h * scale,
                             SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
    if (!g_win) fatal("SDL_CreateWindow: %s", SDL_GetError());
    g_ren = SDL_CreateRenderer(g_win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!g_ren) g_ren = SDL_CreateRenderer(g_win, -1, 0);
    SDL_RenderSetLogicalSize(g_ren, w, h);
    SDL_RenderSetIntegerScale(g_ren, SDL_TRUE);
    g_tex = SDL_CreateTexture(g_ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, w, h);
    SDL_ShowCursor(SDL_ENABLE);
}

void host_shutdown(void) {
    if (g_ren) SDL_DestroyRenderer(g_ren);
    if (g_win) SDL_DestroyWindow(g_win);
    if (!g_headless) SDL_Quit();
}

/* ---- key translation ---- */
static int mac_keycode(SDL_Keycode k, SDL_Scancode sc) {
    switch (sc) {
    case SDL_SCANCODE_A: return 0x00; case SDL_SCANCODE_S: return 0x01; case SDL_SCANCODE_D: return 0x02;
    case SDL_SCANCODE_F: return 0x03; case SDL_SCANCODE_H: return 0x04; case SDL_SCANCODE_G: return 0x05;
    case SDL_SCANCODE_Z: return 0x06; case SDL_SCANCODE_X: return 0x07; case SDL_SCANCODE_C: return 0x08;
    case SDL_SCANCODE_V: return 0x09; case SDL_SCANCODE_B: return 0x0B; case SDL_SCANCODE_Q: return 0x0C;
    case SDL_SCANCODE_W: return 0x0D; case SDL_SCANCODE_E: return 0x0E; case SDL_SCANCODE_R: return 0x0F;
    case SDL_SCANCODE_Y: return 0x10; case SDL_SCANCODE_T: return 0x11; case SDL_SCANCODE_1: return 0x12;
    case SDL_SCANCODE_2: return 0x13; case SDL_SCANCODE_3: return 0x14; case SDL_SCANCODE_4: return 0x15;
    case SDL_SCANCODE_6: return 0x16; case SDL_SCANCODE_5: return 0x17; case SDL_SCANCODE_EQUALS: return 0x18;
    case SDL_SCANCODE_9: return 0x19; case SDL_SCANCODE_7: return 0x1A; case SDL_SCANCODE_MINUS: return 0x1B;
    case SDL_SCANCODE_8: return 0x1C; case SDL_SCANCODE_0: return 0x1D; case SDL_SCANCODE_RIGHTBRACKET: return 0x1E;
    case SDL_SCANCODE_O: return 0x1F; case SDL_SCANCODE_U: return 0x20; case SDL_SCANCODE_LEFTBRACKET: return 0x21;
    case SDL_SCANCODE_I: return 0x22; case SDL_SCANCODE_P: return 0x23; case SDL_SCANCODE_RETURN: return 0x24;
    case SDL_SCANCODE_L: return 0x25; case SDL_SCANCODE_J: return 0x26; case SDL_SCANCODE_APOSTROPHE: return 0x27;
    case SDL_SCANCODE_K: return 0x28; case SDL_SCANCODE_SEMICOLON: return 0x29; case SDL_SCANCODE_BACKSLASH: return 0x2A;
    case SDL_SCANCODE_COMMA: return 0x2B; case SDL_SCANCODE_SLASH: return 0x2C; case SDL_SCANCODE_N: return 0x2D;
    case SDL_SCANCODE_M: return 0x2E; case SDL_SCANCODE_PERIOD: return 0x2F; case SDL_SCANCODE_TAB: return 0x30;
    case SDL_SCANCODE_SPACE: return 0x31; case SDL_SCANCODE_GRAVE: return 0x32; case SDL_SCANCODE_BACKSPACE: return 0x33;
    case SDL_SCANCODE_ESCAPE: return 0x35;
    case SDL_SCANCODE_KP_PERIOD: return 0x41; case SDL_SCANCODE_KP_MULTIPLY: return 0x43; case SDL_SCANCODE_KP_PLUS: return 0x45;
    case SDL_SCANCODE_NUMLOCKCLEAR: return 0x47; case SDL_SCANCODE_KP_DIVIDE: return 0x4B; case SDL_SCANCODE_KP_ENTER: return 0x4C;
    case SDL_SCANCODE_KP_MINUS: return 0x4E; case SDL_SCANCODE_KP_EQUALS: return 0x51; case SDL_SCANCODE_KP_0: return 0x52;
    case SDL_SCANCODE_KP_1: return 0x53; case SDL_SCANCODE_KP_2: return 0x54; case SDL_SCANCODE_KP_3: return 0x55;
    case SDL_SCANCODE_KP_4: return 0x56; case SDL_SCANCODE_KP_5: return 0x57; case SDL_SCANCODE_KP_6: return 0x58;
    case SDL_SCANCODE_KP_7: return 0x59; case SDL_SCANCODE_KP_8: return 0x5B; case SDL_SCANCODE_KP_9: return 0x5C;
    case SDL_SCANCODE_F5: return 0x60; case SDL_SCANCODE_F6: return 0x61; case SDL_SCANCODE_F7: return 0x62;
    case SDL_SCANCODE_F3: return 0x63; case SDL_SCANCODE_F8: return 0x64; case SDL_SCANCODE_F9: return 0x65;
    case SDL_SCANCODE_F11: return 0x67; case SDL_SCANCODE_F13: return 0x69; case SDL_SCANCODE_F14: return 0x6B;
    case SDL_SCANCODE_F10: return 0x6D; case SDL_SCANCODE_F12: return 0x6F; case SDL_SCANCODE_F15: return 0x71;
    case SDL_SCANCODE_INSERT: return 0x72; case SDL_SCANCODE_HOME: return 0x73; case SDL_SCANCODE_PAGEUP: return 0x74;
    case SDL_SCANCODE_DELETE: return 0x75; case SDL_SCANCODE_F4: return 0x76; case SDL_SCANCODE_END: return 0x77;
    case SDL_SCANCODE_F2: return 0x78; case SDL_SCANCODE_PAGEDOWN: return 0x79; case SDL_SCANCODE_F1: return 0x7A;
    case SDL_SCANCODE_LEFT: return 0x7B; case SDL_SCANCODE_RIGHT: return 0x7C; case SDL_SCANCODE_DOWN: return 0x7D;
    case SDL_SCANCODE_UP: return 0x7E;
    case SDL_SCANCODE_LGUI: case SDL_SCANCODE_RGUI: return 0x37;
    case SDL_SCANCODE_LSHIFT: case SDL_SCANCODE_RSHIFT: return 0x38;
    case SDL_SCANCODE_CAPSLOCK: return 0x39;
    case SDL_SCANCODE_LALT: case SDL_SCANCODE_RALT: return 0x3A;
    case SDL_SCANCODE_LCTRL: case SDL_SCANCODE_RCTRL: return 0x3B;
    default: (void)k; return -1;
    }
}

/* Option layer of the US Mac keyboard (Mac Roman). Dead keys yield the
   accent itself. Indexed by the unshifted ASCII key. */
static u8 mac_option_char(int c, bool shift) {
    static const char *keys = "`1234567890-=qwertyuiop[]\\asdfghjkl;'zxcvbnm,./";
    static const u8 plain[] = { 0x60, 0xC1, 0xAA, 0xA3, 0xA2, 0xB0, 0xA4, 0xA6, 0xA5, 0xBB, 0xBC, 0xD0, 0xAD,
        0xCF, 0xB7, 0xAB, 0xA8, 0xA0, 0xB4, 0xAC, 0xF6, 0xBF, 0xB9, 0xD2, 0xD4, 0xC7,
        0x8C, 0xA7, 0xB6, 0xC4, 0xA9, 0xFA, 0xC6, 0xFB, 0xC2, 0xC9, 0xBE,
        0xBD, 0xC5, 0x8D, 0xC3, 0xBA, 0xF7, 0xB5, 0xB2, 0xB3, 0xD6 };
    static const u8 shifted[] = { 0x60, 0xDA, 0xDB, 0xDC, 0xDD, 0xDE, 0xDF, 0xE0, 0xA1, 0xE1, 0xE2, 0xD1, 0xB1,
        0xCE, 0xE3, 0xAB, 0xE4, 0xFF, 0xE5, 0xAC, 0xF6, 0xAF, 0xB8, 0xD3, 0xD5, 0xC8,
        0x81, 0xEA, 0xEB, 0xEC, 0xED, 0xEE, 0xEF, 0xF0, 0xF1, 0xF2, 0xAE,
        0xF3, 0xF4, 0x82, 0xD7, 0xF5, 0xF7, 0xF8, 0xF9, 0xFA, 0xC0 };
    if (c >= 'A' && c <= 'Z') c += 32;
    const char *p = c ? strchr(keys, c) : NULL;
    if (!p) return 0;
    return shift ? shifted[p - keys] : plain[p - keys];
}

static u8 mac_char(int kc, SDL_Keycode k, bool shift, bool option) {
    switch (kc) {
    case 0x24: return 0x0D; case 0x4C: return 0x03; case 0x30: return 0x09; case 0x33: return 0x08;
    case 0x35: return 0x1B; case 0x75: return 0x7F; case 0x73: return 0x01; case 0x77: return 0x04;
    case 0x74: return 0x0B; case 0x79: return 0x0C; case 0x72: return 0x05; case 0x47: return 0x1B;
    case 0x7B: return 0x1C; case 0x7C: return 0x1D; case 0x7E: return 0x1E; case 0x7D: return 0x1F;
    case 0x7A: case 0x78: case 0x63: case 0x76: case 0x60: case 0x61: case 0x62: case 0x64:
    case 0x65: case 0x6D: case 0x67: case 0x6F: case 0x69: case 0x6B: case 0x71: return 0x10;
    case 0x41: return '.'; case 0x43: return '*'; case 0x45: return '+'; case 0x4B: return '/';
    case 0x4E: return '-'; case 0x51: return '=';
    case 0x52: return '0'; case 0x53: return '1'; case 0x54: return '2'; case 0x55: return '3';
    case 0x56: return '4'; case 0x57: return '5'; case 0x58: return '6'; case 0x59: return '7';
    case 0x5B: return '8'; case 0x5C: return '9';
    }
    if (option && k > 32 && k < 127) { u8 o = mac_option_char((int)k, shift); if (o) return o; }
    if (k >= 'a' && k <= 'z') return (u8)(shift ? k - 32 : k);
    if (k >= 32 && k < 127) {
        if (!shift) return (u8)k;
        static const char *from = "1234567890-=[]\\;',./`", *to = "!@#$%^&*()_+{}|:\"<>?~";
        const char *p = strchr(from, (int)k);
        return p ? (u8)to[p - from] : (u8)k;
    }
    return 0;
}

static void set_key(int kc, bool down) {
    if (kc < 0 || kc > 127) return;
    if (down) g_keymap[kc >> 3] |= (u8)(1 << (kc & 7));
    else g_keymap[kc >> 3] &= (u8)~(1 << (kc & 7));
}

static u16 mods_from_sdl(SDL_Keymod m) {
    u16 r = 0;
#ifdef __APPLE__
    if (m & KMOD_GUI) r |= 0x0100;
    if (m & KMOD_CTRL) r |= 0x1000;
#else
    if (m & (KMOD_GUI | KMOD_CTRL)) r |= 0x0100;
#endif
    if (m & KMOD_SHIFT) r |= 0x0200;
    if (m & KMOD_CAPS) r |= 0x0400;
    if (m & KMOD_ALT) r |= 0x0800;
    return r;
}

void host_mouse(int *x, int *y, bool *down) { *x = g_mx; *y = g_my; *down = g_mdown; }
u16 host_modifiers(void) { return g_mods; }
void host_keymap(u8 out[16]) {
    memcpy(out, g_keymap, 16);
    /* modifier keys from state */
    if (g_mods & 0x0100) out[0x37 >> 3] |= 1 << (0x37 & 7);
    if (g_mods & 0x0200) out[0x38 >> 3] |= 1 << (0x38 & 7);
    if (g_mods & 0x0800) out[0x3A >> 3] |= 1 << (0x3A & 7);
    if (g_mods & 0x1000) out[0x3B >> 3] |= 1 << (0x3B & 7);
}

void host_pump(bool wait) {
    script_tick();
    if (g_headless) {
        extern bool g_deterministic;
        extern void vclock_idle(void);
        if (wait) { if (g_deterministic) vclock_idle(); else SDL_Delay(1); }
        return;
    }
    SDL_Event e;
    bool any = false;
    while (SDL_PollEvent(&e)) {
        any = true;
        switch (e.type) {
        case SDL_QUIT: push_event((HostEvent){ .type = HEV_QUIT }); break;
        case SDL_MOUSEMOTION: g_mx = e.motion.x; g_my = e.motion.y; break;
        case SDL_MOUSEBUTTONDOWN:
        case SDL_MOUSEBUTTONUP: {
            g_mx = e.button.x; g_my = e.button.y;
            bool down = e.type == SDL_MOUSEBUTTONDOWN;
            u16 m = mods_from_sdl(SDL_GetModState());
            if (e.button.button == SDL_BUTTON_RIGHT) m |= 0x1000; /* control-click */
            g_mdown = down;
            push_event((HostEvent){ .type = down ? HEV_MOUSE_DOWN : HEV_MOUSE_UP, .x = g_mx, .y = g_my, .mods = m });
            break;
        }
        case SDL_KEYDOWN:
        case SDL_KEYUP: {
            bool down = e.type == SDL_KEYDOWN;
            int kc = mac_keycode(e.key.keysym.sym, e.key.keysym.scancode);
            g_mods = mods_from_sdl((SDL_Keymod)e.key.keysym.mod);
            set_key(kc, down);
            if (kc < 0 || kc == 0x37 || kc == 0x38 || kc == 0x39 || kc == 0x3A || kc == 0x3B) break;
            u8 ch = mac_char(kc, e.key.keysym.sym, (e.key.keysym.mod & KMOD_SHIFT) != 0,
                             (e.key.keysym.mod & KMOD_ALT) != 0);
            push_event((HostEvent){ .type = down ? HEV_KEY_DOWN : HEV_KEY_UP, .mac_key = (u8)kc, .ch = ch,
                                    .mods = g_mods, .repeat = e.key.repeat != 0, .x = g_mx, .y = g_my });
            break;
        }
        case SDL_WINDOWEVENT:
            if (e.window.event == SDL_WINDOWEVENT_FOCUS_GAINED) push_event((HostEvent){ .type = HEV_FOCUS_IN });
            if (e.window.event == SDL_WINDOWEVENT_FOCUS_LOST) push_event((HostEvent){ .type = HEV_FOCUS_OUT });
            break;
        }
    }
    extern bool g_deterministic;
    extern void vclock_idle(void);
    if (wait && !any) { if (g_deterministic) vclock_idle(); else SDL_Delay(2); }
}

void host_present(const u8 *pixels, int pitch, int w, int h, const u32 *pal) {
    if (w > g_w) w = g_w;
    if (h > g_h) h = g_h;
    for (int y = 0; y < h; y++) {
        const u8 *s = pixels + (size_t)y * (size_t)pitch;
        u32 *d = g_rgb + (size_t)y * (size_t)g_w;
        for (int x = 0; x < w; x++) d[x] = 0xFF000000u | pal[s[x]];
    }
    if (g_headless) return;
    SDL_UpdateTexture(g_tex, NULL, g_rgb, g_w * 4);
    SDL_SetRenderDrawColor(g_ren, 0, 0, 0, 255);
    SDL_RenderClear(g_ren);
    SDL_RenderCopy(g_ren, g_tex, NULL, NULL);
    SDL_RenderPresent(g_ren);
}

bool host_screenshot(const char *path) {
    u8 *rgb = malloc((size_t)g_w * (size_t)g_h * 3);
    for (int i = 0; i < g_w * g_h; i++) {
        rgb[i * 3] = (u8)(g_rgb[i] >> 16);
        rgb[i * 3 + 1] = (u8)(g_rgb[i] >> 8);
        rgb[i * 3 + 2] = (u8)g_rgb[i];
    }
    int ok = stbi_write_png(path, g_w, g_h, 3, rgb, g_w * 3);
    free(rgb);
    LOG_I("screenshot %s %s", path, ok ? "saved" : "FAILED");
    return ok != 0;
}

void host_set_cursor(const u8 *rgba, int hotx, int hoty, bool visible) {
    if (g_headless) return;
    if (!visible) { SDL_ShowCursor(SDL_DISABLE); return; }
    SDL_Surface *s = SDL_CreateRGBSurfaceWithFormatFrom((void *)rgba, 16, 16, 32, 64, SDL_PIXELFORMAT_RGBA32);
    if (!s) return;
    int sc = 1;
    int ww, wh;
    SDL_GetWindowSize(g_win, &ww, &wh);
    sc = ww / g_w; if (sc < 1) sc = 1;
    SDL_Surface *big = SDL_CreateRGBSurfaceWithFormat(0, 16 * sc, 16 * sc, 32, SDL_PIXELFORMAT_RGBA32);
    SDL_BlitScaled(s, NULL, big, NULL);
    SDL_Cursor *c = SDL_CreateColorCursor(big, hotx * sc, hoty * sc);
    SDL_FreeSurface(s);
    SDL_FreeSurface(big);
    if (c) {
        SDL_SetCursor(c);
        if (g_cursor) SDL_FreeCursor(g_cursor);
        g_cursor = c;
    }
    SDL_ShowCursor(SDL_ENABLE);
}

/* ---------------------------------------------------------------------- */
/* Scripted input                                                          */

typedef struct { char cmd[16]; char arg[256]; } ScriptLine;
static ScriptLine *g_script;
static int g_script_n, g_script_pc;
static u32 g_script_wait_until;

void script_load(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) fatal("cannot open script %s", path);
    char line[512];
    int cap = 64;
    g_script = malloc(sizeof(ScriptLine) * (size_t)cap);
    while (fgets(line, sizeof line, f)) {
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '#' || *p == '\n' || !*p) continue;
        p[strcspn(p, "\r\n")] = 0;
        if (g_script_n == cap) { cap *= 2; g_script = realloc(g_script, sizeof(ScriptLine) * (size_t)cap); }
        ScriptLine *s = &g_script[g_script_n++];
        memset(s, 0, sizeof *s);
        sscanf(p, "%15s", s->cmd);
        char *a = p + strlen(s->cmd);
        while (*a == ' ') a++;
        snprintf(s->arg, sizeof s->arg, "%s", a);
    }
    fclose(f);
    LOG_I("script %s: %d commands", path, g_script_n);
}

static int key_by_name(const char *n, u8 *ch) {
    static const struct { const char *name; int kc; u8 ch; } keys[] = {
        { "return", 0x24, 0x0D }, { "enter", 0x4C, 0x03 }, { "tab", 0x30, 0x09 }, { "space", 0x31, ' ' },
        { "backspace", 0x33, 0x08 }, { "escape", 0x35, 0x1B }, { "left", 0x7B, 0x1C }, { "right", 0x7C, 0x1D },
        { "up", 0x7E, 0x1E }, { "down", 0x7D, 0x1F }, { "f1", 0x7A, 0x10 }, { "f2", 0x78, 0x10 },
        { "kp1", 0x53, '1' }, { "kp2", 0x54, '2' }, { "kp3", 0x55, '3' }, { "kp4", 0x56, '4' },
        { "kp5", 0x57, '5' }, { "kp6", 0x58, '6' }, { "kp7", 0x59, '7' }, { "kp8", 0x5B, '8' }, { "kp9", 0x5C, '9' },
        { NULL, 0, 0 }
    };
    for (int i = 0; keys[i].name; i++) if (!strcmp(keys[i].name, n)) { *ch = keys[i].ch; return keys[i].kc; }
    if (strlen(n) == 1) {
        static const char *qwerty = "asdfhgzxcv\0bqweryt123465=97-8]ou[ip\0lj'k;\\,/nm.";
        char c = n[0];
        char lc = (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
        for (int i = 0; i < 48; i++) if (qwerty[i] == lc && lc) { *ch = (u8)c; return i; }
        if (c == '0') { *ch = '0'; return 0x1D; }
        *ch = (u8)c;
        return 0x31;
    }
    return -1;
}

static int g_hold_kc = -1;
static u32 g_hold_until;
static bool g_click_pending; static int g_click_x, g_click_y; static u32 g_click_up_at;
static void script_key(const char *name, u16 mods) {
    u8 ch = 0;
    int kc = key_by_name(name, &ch);
    if (kc < 0) { LOG_W("script: unknown key %s", name); return; }
    if ((mods & 0x800) && strlen(name) == 1) { u8 o = mac_option_char(name[0], (mods & 0x200) != 0); if (o) ch = o; }
    push_event((HostEvent){ .type = HEV_KEY_DOWN, .mac_key = (u8)kc, .ch = ch, .mods = mods, .x = g_mx, .y = g_my });
    push_event((HostEvent){ .type = HEV_KEY_UP, .mac_key = (u8)kc, .ch = ch, .mods = mods, .x = g_mx, .y = g_my });
}

void script_tick(void) {
    if (!g_script) return;
    u32 now = tick_count();
    if (g_click_pending && (s32)(now - g_click_up_at) >= 0) {
        g_click_pending = false; g_mdown = false;
        push_event((HostEvent){ .type = HEV_MOUSE_UP, .x = g_click_x, .y = g_click_y });
    }
    if (g_hold_kc >= 0 && (s32)(now - g_hold_until) >= 0) {
        set_key(g_hold_kc, false);
        push_event((HostEvent){ .type = HEV_KEY_UP, .mac_key = (u8)g_hold_kc, .x = g_mx, .y = g_my });
        g_hold_kc = -1;
    }
    while (g_script_pc < g_script_n) {
        if ((s32)(g_script_wait_until - now) > 0) return;
        /* don't run ahead of the app: wait for the event queue to drain */
        if (g_evh != g_evt) return;
        ScriptLine *s = &g_script[g_script_pc++];
        int x, y;
        LOG_I("script: %s %s", s->cmd, s->arg);
        if (!strcmp(s->cmd, "wait")) { g_script_wait_until = now + (u32)atoi(s->arg); return; }
        else if (!strcmp(s->cmd, "move") && sscanf(s->arg, "%d %d", &x, &y) == 2) { g_mx = x; g_my = y; }
        else if (!strcmp(s->cmd, "trace")) { extern bool g_trace_traps; g_trace_traps = !strcmp(s->arg, "on"); }
        else if (!strcmp(s->cmd, "click") && sscanf(s->arg, "%d %d", &x, &y) == 2) {
            /* like a human click: the button stays down for a few ticks */
            g_mx = x; g_my = y; g_mdown = true;
            push_event((HostEvent){ .type = HEV_MOUSE_DOWN, .x = x, .y = y });
            g_click_pending = true; g_click_x = x; g_click_y = y; g_click_up_at = now + 6;
            g_script_wait_until = now + 12;
            return;
        } else if (!strcmp(s->cmd, "dclick") && sscanf(s->arg, "%d %d", &x, &y) == 2) {
            g_mx = x; g_my = y;
            for (int k = 0; k < 2; k++) {
                push_event((HostEvent){ .type = HEV_MOUSE_DOWN, .x = x, .y = y });
                push_event((HostEvent){ .type = HEV_MOUSE_UP, .x = x, .y = y });
            }
            g_script_wait_until = now + 10;
            return;
        } else if (!strcmp(s->cmd, "mousedown") && sscanf(s->arg, "%d %d", &x, &y) == 2) {
            g_mx = x; g_my = y; g_mdown = true;
            push_event((HostEvent){ .type = HEV_MOUSE_DOWN, .x = x, .y = y });
        } else if (!strcmp(s->cmd, "mouseup") && sscanf(s->arg, "%d %d", &x, &y) == 2) {
            g_mx = x; g_my = y; g_mdown = false;
            push_event((HostEvent){ .type = HEV_MOUSE_UP, .x = x, .y = y });
        } else if (!strcmp(s->cmd, "key")) {
            char name[64] = {0}, mod[64] = {0};
            sscanf(s->arg, "%63s %63s", name, mod);
            u16 m = 0;
            if (strstr(mod, "cmd")) m |= 0x100;
            if (strstr(mod, "shift")) m |= 0x200;
            if (strstr(mod, "opt")) m |= 0x800;
            if (strstr(mod, "ctrl")) m |= 0x1000;
            script_key(name, m);
            g_script_wait_until = now + 2;
            return;
        } else if (!strcmp(s->cmd, "hold")) {
            char name[64] = {0}; int ticks = 30;
            sscanf(s->arg, "%63s %d", name, &ticks);
            u8 ch = 0;
            int kc = key_by_name(name, &ch);
            if (kc >= 0) {
                set_key(kc, true);
                push_event((HostEvent){ .type = HEV_KEY_DOWN, .mac_key = (u8)kc, .ch = ch, .x = g_mx, .y = g_my });
                g_hold_kc = kc; g_hold_until = now + (u32)ticks;
                g_script_wait_until = now + (u32)ticks + 2;
            }
            return;
        } else if (!strcmp(s->cmd, "type")) {
            for (const char *p = s->arg; *p; p++) { char n[2] = { *p, 0 }; script_key(n, (*p >= 'A' && *p <= 'Z') ? 0x200 : 0); }
            g_script_wait_until = now + 2;
            return;
        } else if (!strcmp(s->cmd, "bt")) {
            extern _Thread_local struct CPU *g_cpu;
            extern void cpu_backtrace(struct CPU *c, FILE *f);
            if (g_cpu) cpu_backtrace(g_cpu, stderr);
            extern void threads_debug_dump(void);
            threads_debug_dump();
        } else if (!strcmp(s->cmd, "peek")) {
            unsigned a = 0, cnt = 16;
            sscanf(s->arg, "%x %u", &a, &cnt);
            extern u8 *g_mem;
            char buf[512]; int o = 0;
            for (unsigned i = 0; i < cnt && i < 128; i++) o += snprintf(buf + o, sizeof buf - (size_t)o, "%02x%s", g_mem[a + i], (i & 1) ? " " : "");
            LOG_I("peek %08x: %s", a, buf);
        } else if (!strcmp(s->cmd, "dumpwin")) {
            extern void wm_debug_dump(void);
            wm_debug_dump();
        } else if (!strcmp(s->cmd, "shot")) {
            host_screenshot(s->arg);
        } else if (!strcmp(s->cmd, "quit")) {
            LOG_I("script: quit");
            host_shutdown();
            exit(0);
        } else LOG_W("script: bad command %s", s->cmd);
    }
}
