/* QuickTime (subset): Component Manager instances for the tune player and
 * note allocator, file previews.  Music synthesis lives in music.c (M6);
 * until then tunes are accepted and play silently. */
#include "os.h"
#include "mm.h"

#define COMP_TUNE FOURCC('t','u','n','e')
#define COMP_NOTA FOURCC('n','o','t','a')

typedef struct { u32 inst; u32 type; } Comp;
static Comp g_comps[32];

TRAP(EnterMovies) { RETERR(noErr); }
TRAP(ExitMovies) { }

TRAP(OpenDefaultComponent) {
    u32 type = ARG(0);
    if (type != COMP_TUNE && type != COMP_NOTA) { LOG_D("OpenDefaultComponent('%s') -> none", fourcc_str(type)); RET(0); return; }
    for (int i = 0; i < 32; i++) if (!g_comps[i].inst) {
        g_comps[i].inst = mm_new_ptr(16, true, ZONE_SYS);
        g_comps[i].type = type;
        wr32(g_comps[i].inst, type);
        RET(g_comps[i].inst);
        return;
    }
    RET(0);
}
TRAP(CloseComponent) {
    u32 c = ARG(0);
    extern void music_close_player(u32 inst);
    for (int i = 0; i < 32; i++) if (g_comps[i].inst == c) { music_close_player(c); mm_dispose_ptr(c); g_comps[i].inst = 0; }
    RETERR(noErr);
}

/* File previews */
TRAP(MakeFilePreview) { RETERR(noErr); }
TRAP(AddFilePreview) { RETERR(noErr); }
TRAP(MakeThumbnailFromPixMap) { RETERR(-2000); }
