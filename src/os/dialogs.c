/* Dialog Manager (in progress). */
#include "wm.h"
#include "resources.h"

#define LM_DAStrings 0x0AA0

static u32 g_param[4];

TRAP(ParamText) {
    for (int i = 0; i < 4; i++) {
        u32 s = ARG(i);
        if (!g_param[i]) g_param[i] = mm_new_handle(256, true, ZONE_SYS);
        if (s) gmemmove(hderef(g_param[i]), s, 1u + rd8(s)); else wr8(hderef(g_param[i]), 0);
        wr32(LM_DAStrings + 4 * (u32)i, g_param[i]);
    }
}

static void log_alert(const char *kind, s16 id) {
    u32 len;
    u8 *a = res_load_raw(FOURCC('A','L','R','T'), id, &len);
    if (!a) { LOG_W("%s(%d): no ALRT", kind, id); return; }
    s16 ditl = (s16)be16(a + 8);
    free(a);
    u8 *d = res_load_raw(FOURCC('D','I','T','L'), ditl, &len);
    if (!d) return;
    int n = (s16)be16(d) + 1;
    u32 p = 2;
    for (int i = 0; i < n && p + 14 <= len; i++) {
        u8 type = d[p + 12], l = d[p + 13];
        char txt[256]; memcpy(txt, d + p + 14, l); txt[l] = 0;
        /* substitute ^0..^3 */
        char out[1024]; int o = 0;
        for (int k = 0; txt[k] && o < 1000; k++) {
            if (txt[k] == '^' && txt[k + 1] >= '0' && txt[k + 1] <= '3') {
                char ps[256]; pstr_to_c(g_param[txt[k + 1] - '0'] ? hderef(g_param[txt[k + 1] - '0']) : 0, ps, sizeof ps);
                o += snprintf(out + o, sizeof out - (size_t)o, "%s", ps); k++;
            } else out[o++] = txt[k];
        }
        out[o] = 0;
        LOG_W("%s(%d) item %d type %d: %s", kind, id, i + 1, type & 0x7F, out);
        p += 14 + l + (l & 1);
    }
    free(d);
}
TRAP(StopAlert) { log_alert("StopAlert", ARGS16(0)); RET(1); }
TRAP(Alert) { log_alert("Alert", ARGS16(0)); RET(1); }
