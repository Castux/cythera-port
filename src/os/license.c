/* Shareware registration bypass (built unless LICENSE_BYPASS=0; the name
 * shown defaults to "Cythera Port", or --registered NAME).
 *
 * Cythera links Ambrosia's registration library (code without traceback
 * names). The application asks it two things:
 *   0xb8f80  OSErr IsRegistered(Boolean *registered): checks the licence
 *            record's code; used by the title screen, the reminder on startup
 *            and when opening a game, and the scripts' registration global;
 *   0xb947c  OSErr GetRegisteredName(Str255 name): the licensee.
 * Both are redirected to native versions that report a registration to NAME.
 * The patch is applied only if the code is the expected one.
 */
#include "os.h"
#include "../config.h"

#ifdef LICENSE_BYPASS

typedef struct { u32 off; u32 words[12]; void (*fn)(CPU *); const char *name; } Patch;

static void is_registered(CPU *cpu) { wr8(ARG(0), 1); RETERR(noErr); }
static const char *name(void) { return g_cfg.registered_to ? g_cfg.registered_to : "Cythera Port"; }
static void registered_name(CPU *cpu) { c_to_pstr(name(), ARG(0), 255); RETERR(noErr); }

static const Patch g_patches[] = {
    { 0xb8f80, { 0x7c0802a6, 0x93e1fffc, 0x93c1fff8, 0x93a1fff4, 0x9381fff0, 0x90010008,
                 0x9421ffb0, 0x7c7c1b78, 0x3ba00000, 0x9bbc0000, 0x80628ecc, 0x88030000 },
      is_registered, "IsRegistered" },
    { 0xb947c, { 0x7c0802a6, 0x93e1fffc, 0x93c1fff8, 0x93a1fff4, 0x90010008, 0x9421ffb0,
                 0x7c7d1b78, 0x3be00000, 0x80628ecc, 0x88030000, 0x28000000, 0x4182007c },
      registered_name, "GetRegisteredName" },
};

void license_bypass(u32 code_addr, u32 code_size) {
    for (size_t i = 0; i < sizeof g_patches / sizeof g_patches[0]; i++) {
        const Patch *p = &g_patches[i];
        if (p->off + sizeof p->words > code_size) goto mismatch;
        for (int k = 0; k < 12; k++)
            if (rd32(code_addr + p->off + 4 * (u32)k) != p->words[k]) goto mismatch;
    }
    for (size_t i = 0; i < sizeof g_patches / sizeof g_patches[0]; i++) {
        const Patch *p = &g_patches[i];
        u32 entry = rd32(trap_native_tvector(p->name, p->fn));
        wr32(code_addr + p->off, 0x48000002 | (entry & 0x03FFFFFC)); /* ba entry */
    }
    LOG_I("registration: registered to \"%s\"", name());
    return;
mismatch:
    LOG_I("registration: not bypassed (unknown application code)");
}
#else
void license_bypass(u32 code_addr, u32 code_size) { (void)code_addr; (void)code_size; }
#endif
