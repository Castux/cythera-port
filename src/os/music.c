/* QuickTime Music Architecture: tune player and note allocator.
 *
 * Tune headers and sequences are the standard QTMA event streams (rest,
 * note, extended note, controller, marker, general events). They are
 * sequenced in the audio thread directly from guest memory and rendered
 * with TinySoundFont using a General MIDI SoundFont when one is available
 * (the original used QuickTime's GS-compatible instruments), or with a
 * small built-in synthesizer otherwise.
 */
#include "os.h"
#include "mm.h"
#include "../config.h"
#include <SDL.h>
#include <math.h>
#define TSF_IMPLEMENTATION
#include "../../third_party/tsf.h"

#define RATE 44100
#define MAX_PARTS 64
#define MAX_NOTES 256
#define MAX_PLAYERS 4
#define QLEN 8

typedef struct { u32 data; double rate; u32 flags; } Seq;
typedef struct { int ch; int key; double end; bool used; } ActiveNote;

typedef struct {
    bool used;
    u32 inst;                 /* component instance */
    int program[MAX_PARTS];   /* GM program (0-based), -1 unset */
    bool drum[MAX_PARTS];
    int chan_base;
    double scale;             /* units per second */
    double volume;
    Seq q[QLEN]; int qn;      /* q[0] is playing when playing */
    bool playing;
    u32 pos;                  /* next event (guest address) */
    double t;                 /* current tune time (units) */
    double next;              /* time of next event */
    ActiveNote notes[MAX_NOTES];
} Player;

static Player g_pl[MAX_PLAYERS];
static tsf *g_tsf;
static SDL_mutex *g_mx;
extern SDL_AudioDeviceID music_audio_device(void);

/* ---- fallback synth (used when no SoundFont is available) ---- */
typedef struct { bool on; int ch; int key; double freq, phase; float amp, env; bool released; int prog; bool drum; u32 noise; } Voice;
static Voice g_v[48];
/* Synth channels: MAX_PARTS per tune player, then the note allocator's. */
#define NA_CHAN0 (MAX_PLAYERS * MAX_PARTS)
#define NA_NCHAN 8
#define NCHAN (NA_CHAN0 + NA_NCHAN)
static float g_chvol[NCHAN], g_chpan[NCHAN];
static float g_chctl[NCHAN];  /* volume controller of the part */
static float g_chgain[NCHAN]; /* volume of the tune player owning the channel (TuneSetVolume) */
static int g_chprog[NCHAN]; static bool g_chdrum[NCHAN];

static void fb_note_on(int ch, int key, float vel) {
    Voice *v = NULL;
    for (int i = 0; i < 48; i++) if (!g_v[i].on) { v = &g_v[i]; break; }
    if (!v) v = &g_v[0];
    *v = (Voice){ true, ch, key, 440.0 * pow(2.0, (key - 69) / 12.0), 0, vel, 1.0f, false, g_chprog[ch], g_chdrum[ch], 12345u + (u32)key };
}
static void fb_note_off(int ch, int key) {
    for (int i = 0; i < 48; i++) if (g_v[i].on && g_v[i].ch == ch && g_v[i].key == key) g_v[i].released = true;
}
static void fb_render(float *out, int n) {
    for (int i = 0; i < 48; i++) {
        Voice *v = &g_v[i];
        if (!v->on) continue;
        float vol = g_chvol[v->ch] * v->amp * 0.15f, pan = g_chpan[v->ch];
        for (int k = 0; k < n; k++) {
            float s;
            if (v->drum) { v->noise = v->noise * 1103515245u + 12345u; s = ((v->noise >> 16) & 0x7FFF) / 16384.0f - 1.0f; v->env *= 0.9990f; }
            else {
                double ph = v->phase;
                int fam = v->prog / 8;
                if (fam == 0 || fam == 1) s = (float)(sin(2 * M_PI * ph) * 0.8 + sin(4 * M_PI * ph) * 0.2);  /* piano/percussive */
                else if (fam == 9) s = (float)(ph < 0.5 ? 0.5 : -0.5);                                          /* pipe */
                else if (fam >= 5 && fam <= 7) s = (float)(2 * ph - 1) * 0.6f;                                 /* strings/brass */
                else s = (float)sin(2 * M_PI * ph);
                v->phase += v->freq / RATE;
                if (v->phase >= 1) v->phase -= 1;
                if (fam <= 1) v->env *= 0.99995f;
            }
            if (v->released) v->env *= 0.9992f;
            float e = s * v->env * vol;
            out[2 * k] += e * (1 - pan);
            out[2 * k + 1] += e * pan;
        }
        if (v->env < 0.001f) v->on = false;
    }
}

/* ---- synth abstraction ---- */
static void syn_program(int ch, int prog, bool drum) {
    g_chprog[ch] = prog; g_chdrum[ch] = drum;
    if (g_tsf) tsf_channel_set_presetnumber(g_tsf, ch, drum ? 0 : prog, drum ? 1 : 0);
}
static void syn_note_on(int ch, int key, int vel) {
    if (g_tsf) tsf_channel_note_on(g_tsf, ch, key, vel / 127.0f); else fb_note_on(ch, key, vel / 127.0f);
}
static void syn_note_off(int ch, int key) { if (g_tsf) tsf_channel_note_off(g_tsf, ch, key); else fb_note_off(ch, key); }
static void syn_apply_volume(int ch) {
    float v = g_chctl[ch] * g_chgain[ch];
    g_chvol[ch] = v;
    if (g_tsf) tsf_channel_set_volume(g_tsf, ch, v);
}
static void syn_volume(int ch, float v) { g_chctl[ch] = v; syn_apply_volume(ch); }
static void syn_gain(int ch, float g) { g_chgain[ch] = g; syn_apply_volume(ch); }
static void syn_pan(int ch, float p) { g_chpan[ch] = p; if (g_tsf) tsf_channel_set_pan(g_tsf, ch, p); }
static void syn_pitchbend(int ch, double semis) {
    if (!g_tsf) return;
    int v = 8192 + (int)(semis / 2.0 * 8192);
    if (v < 0) v = 0;
    if (v > 16383) v = 16383;
    tsf_channel_set_pitchwheel(g_tsf, ch, v);
}
static void syn_sustain(int ch, bool on) { if (g_tsf) tsf_channel_midi_control(g_tsf, ch, 64, on ? 127 : 0); }

/* ---- sequencer ---- */
static void release_all(Player *p) {
    for (int i = 0; i < MAX_NOTES; i++) if (p->notes[i].used) { syn_note_off(p->notes[i].ch, p->notes[i].key); p->notes[i].used = false; }
}

static int part_chan(Player *p, int part) { return p->chan_base + (part % MAX_PARTS); }

static void set_part_instrument(Player *p, int part, u32 gm) {
    if (part >= MAX_PARTS) return;
    bool drum = gm > 16384;
    int prog = drum ? 0 : (int)(gm ? gm - 1 : 0) & 127;
    p->program[part] = prog; p->drum[part] = drum;
    syn_program(part_chan(p, part), prog, drum);
    syn_volume(part_chan(p, part), 1.0f);
    syn_pan(part_chan(p, part), 0.5f);
}

/* Process general events in a header or sequence. `w` points at the head. */
static u32 general_event(Player *p, u32 at) {
    u32 w = rd32(at);
    int part = (int)((w >> 16) & 0xFFF);
    u32 len = w & 0xFFFF;
    if (len < 2) return at + 4;
    u32 tail = rd32(at + 4 * (len - 1));
    int sub = (int)((tail >> 16) & 0x3FFF);
    if (sub == 1 && len >= 23) set_part_instrument(p, part, rd32(at + 4 + 80)); /* NoteRequest: gmNumber */
    return at + 4 * len;
}

/* kMarkerEventEnd (subtype 0) stops only with value 0; others are ignored.
   Cythera's tunes put 0x60000001 between sections, and the game requeues
   the tune from the top as soon as TuneGetStatus reports it finished. */
static bool is_end_marker(u32 w) { return (w >> 29) == 3 && (w & 0x00FFFFFF) == 0; }

static void parse_header(Player *p, u32 h) {
    for (int i = 0; i < MAX_PARTS; i++) { p->program[i] = -1; p->drum[i] = false; }
    u32 at = h;
    for (int guard = 0; guard < 4096; guard++) {
        u32 w = rd32(at);
        if ((w >> 28) == 0xF) at = general_event(p, at);
        else if (is_end_marker(w)) break;
        else if (w >> 31) at += 8;
        else at += 4;
    }
}

static void add_note(Player *p, int ch, int key, double end) {
    for (int i = 0; i < MAX_NOTES; i++) if (!p->notes[i].used) { p->notes[i] = (ActiveNote){ ch, key, end, true }; return; }
}

static void seq_start(Player *p) {
    if (!p->qn) { p->playing = false; return; }
    p->pos = p->q[0].data;
    p->next = p->t;
    p->playing = true;
}

/* Run events due at or before p->t. */
static void seq_events(Player *p) {
    for (int guard = 0; guard < 20000 && p->playing && p->next <= p->t; guard++) {
        u32 w = rd32(p->pos);
        u32 top3 = w >> 29;
        if (!(w >> 31)) {
            switch (top3) {
            case 0: p->next += (double)(w & 0xFFFFFF); p->pos += 4; break; /* rest */
            case 1: { /* note */
                int part = (int)((w >> 24) & 0x1F), key = (int)((w >> 18) & 0x3F) + 32;
                int vel = (int)((w >> 11) & 0x7F), dur = (int)(w & 0x7FF);
                int ch = part_chan(p, part);
                if (vel) { syn_note_on(ch, key, vel); add_note(p, ch, key, p->next + dur); }
                p->pos += 4;
                break;
            }
            case 2: { /* controller */
                int part = (int)((w >> 24) & 0x1F), ctl = (int)((w >> 16) & 0xFF);
                s16 val = (s16)(w & 0xFFFF);
                int ch = part_chan(p, part);
                if (ctl == 7) syn_volume(ch, (float)(val / 256.0 / 127.0));
                else if (ctl == 10) syn_pan(ch, (float)((val - 256) / 256.0));
                else if (ctl == 32) syn_pitchbend(ch, val / 256.0);
                else if (ctl == 64) syn_sustain(ch, val != 0);
                p->pos += 4;
                break;
            }
            case 3: /* marker */
                if (is_end_marker(w)) {
                    /* end of sequence: next queued sequence or stop */
                    if (p->qn) { memmove(&p->q[0], &p->q[1], sizeof(Seq) * (size_t)(p->qn - 1)); p->qn--; }
                    seq_start(p);
                    if (!p->playing) { release_all(p); return; } /* no more note-offs once stopped */
                } else p->pos += 4;
                break;
            }
        } else {
            u32 t4 = w >> 28;
            if (t4 == 0xF) { p->pos = general_event(p, p->pos); continue; }
            u32 w2 = rd32(p->pos + 4);
            if (t4 == 0x9) { /* extended note */
                int part = (int)((w >> 16) & 0xFFF);
                int pitch = (int)(w & 0xFFFF);
                if (pitch > 127) pitch >>= 8;
                int vel = (int)((w2 >> 22) & 0x7F), dur = (int)(w2 & 0x3FFFFF);
                int ch = part_chan(p, part);
                if (vel) { syn_note_on(ch, pitch, vel); add_note(p, ch, pitch, p->next + dur); }
            }
            p->pos += 8;
        }
    }
    /* note-offs */
    for (int i = 0; i < MAX_NOTES; i++)
        if (p->notes[i].used && p->notes[i].end <= p->t) { syn_note_off(p->notes[i].ch, p->notes[i].key); p->notes[i].used = false; }
}

/* Called from the audio mixer: adds music into out (stereo float). */
void music_render(float *out, int n) {
    if (!g_mx) return;
    SDL_LockMutex(g_mx);
    const int step = 64;
    float tmp[2 * 64];
    for (int k = 0; k < n; k += step) {
        int m = n - k < step ? n - k : step;
        for (int i = 0; i < MAX_PLAYERS; i++) {
            Player *p = &g_pl[i];
            if (!p->used || !p->playing) continue;
            p->t += (double)m / RATE * p->scale * (p->qn ? p->q[0].rate : 1.0);
            seq_events(p);
        }
        memset(tmp, 0, sizeof tmp);
        if (g_tsf) tsf_render_float(g_tsf, tmp, m, 0);
        else fb_render(tmp, m);
        for (int j = 0; j < 2 * m; j++) out[2 * k + j] += tmp[j];
    }
    SDL_UnlockMutex(g_mx);
}

void music_init(void) {
    g_mx = SDL_CreateMutex();
    for (int i = 0; i < NCHAN; i++) { g_chvol[i] = g_chctl[i] = g_chgain[i] = 1; g_chpan[i] = 0.5f; }
    char path[1100];
    const char *cands[3] = { g_cfg.soundfont, NULL, NULL };
    snprintf(path, sizeof path, "%s/soundfont.sf2", g_cfg.data_dir);
    cands[1] = path;
    for (int i = 0; i < 2 && !g_tsf; i++) {
        if (!cands[i]) continue;
        g_tsf = tsf_load_filename(cands[i]);
        if (g_tsf) LOG_I("music: SoundFont %s", cands[i]);
    }
    if (!g_tsf) LOG_I("music: no SoundFont found (%s); using built-in synthesizer", path);
    else {
        tsf_set_output(g_tsf, TSF_STEREO_INTERLEAVED, RATE, 0);
        tsf_set_max_voices(g_tsf, 96);
    }
}

/* ---------------------------------------------------------------------- */
/* Tune player traps                                                       */

Player *player_of(u32 inst, bool create) {
    for (int i = 0; i < MAX_PLAYERS; i++) if (g_pl[i].used && g_pl[i].inst == inst) return &g_pl[i];
    if (!create) return NULL;
    for (int i = 0; i < MAX_PLAYERS; i++) if (!g_pl[i].used) {
        Player *p = &g_pl[i];
        memset(p, 0, sizeof *p);
        p->used = true; p->inst = inst; p->scale = 600; p->volume = 1.0;
        p->chan_base = i * MAX_PARTS;
        for (int k = 0; k < MAX_PARTS; k++) { p->program[k] = -1; syn_gain(p->chan_base + k, 1.0f); }
        return p;
    }
    return NULL;
}

void music_close_player(u32 inst) {
    SDL_LockMutex(g_mx);
    Player *p = player_of(inst, false);
    if (p) { release_all(p); p->used = false; }
    SDL_UnlockMutex(g_mx);
}

TRAP(TuneSetHeader) {
    u32 inst = ARG(0), hdr = ARG(1), lenp = ARG(2);
    SDL_LockMutex(g_mx);
    Player *p = player_of(inst, true);
    if (p) parse_header(p, hdr);
    SDL_UnlockMutex(g_mx);
    if (lenp) wr32(lenp, 0);
    RETERR(noErr);
}
TRAP(TuneSetTimeScale) {
    SDL_LockMutex(g_mx);
    Player *p = player_of(ARG(0), true);
    if (p && ARG(1)) p->scale = ARG(1);
    SDL_UnlockMutex(g_mx);
    RETERR(noErr);
}
TRAP(TuneSetVolume) {
    SDL_LockMutex(g_mx);
    Player *p = player_of(ARG(0), true);
    /* the volume of this tune's parts only: note allocator channels and
       other tune players are unaffected */
    if (p) {
        p->volume = (s32)ARG(1) / 65536.0;
        if (p->volume < 0) p->volume = 0;
        for (int k = 0; k < MAX_PARTS; k++) syn_gain(p->chan_base + k, (float)p->volume);
    }
    SDL_UnlockMutex(g_mx);
    RETERR(noErr);
}
TRAP(TuneGetVolume) { Player *p = player_of(ARG(0), false); RET(p ? (u32)(p->volume * 65536) : 0x10000); }

/* TuneQueue(tp, tune, tuneRate, tuneStartPosition, tuneStopPosition, queueFlags, callBackProc, refCon) */
TRAP(TuneQueue) {
    u32 inst = ARG(0), data = ARG(1), rate = ARG(2), flags = ARG(5);
    SDL_LockMutex(g_mx);
    Player *p = player_of(inst, true);
    int err = noErr;
    if (p) {
        if (flags & 1) { /* kTuneStartNow */
            release_all(p);
            p->qn = 0;
        }
        if (p->qn < QLEN) {
            p->q[p->qn++] = (Seq){ data, rate ? rate / 65536.0 : 1.0, flags };
            if (!p->playing) seq_start(p);
        } else err = -2003;
    }
    SDL_UnlockMutex(g_mx);
    RETERR(err);
}
TRAP(TuneStop) {
    SDL_LockMutex(g_mx);
    Player *p = player_of(ARG(0), false);
    if (p) { release_all(p); p->qn = 0; p->playing = false; }
    SDL_UnlockMutex(g_mx);
    RETERR(noErr);
}
TRAP(TunePreroll) { RETERR(noErr); }
TRAP(TuneUnroll) { RETERR(noErr); }
/* TuneStatus: tune(4) tunePtr(4) time(4) queueCount(2) queueSpots(2) queueTime(4) reserved(12) */
TRAP(TuneGetStatus) {
    u32 st = ARG(1);
    gmemset(st, 0, 32);
    SDL_LockMutex(g_mx);
    Player *p = player_of(ARG(0), false);
    if (p) {
        wr32(st, p->qn ? p->q[0].data : 0);
        wr32(st + 4, p->pos);
        wr32(st + 8, (u32)p->t);
        wr16(st + 12, (u16)(p->playing ? p->qn : 0));
        wr16(st + 14, (u16)(QLEN - p->qn));
    }
    SDL_UnlockMutex(g_mx);
    RETERR(noErr);
}

/* ---------------------------------------------------------------------- */
/* Note allocator                                                          */

typedef struct { bool used; u32 handle; int ch; } NoteChan;
static NoteChan g_nc[32];

TRAP(NAStuffToneDescription) {
    u32 gm = ARG(1), td = ARG(2);
    if (td) { gmemset(td, 0, 76); wr32(td + 72, gm); }
    RETERR(noErr);
}
/* NANewNoteChannel(na, NoteRequest*, &channel) */
TRAP(NANewNoteChannel) {
    u32 req = ARG(1), outp = ARG(2);
    for (int i = 0; i < 32; i++) if (!g_nc[i].used) {
        g_nc[i].used = true;
        g_nc[i].handle = mm_new_ptr(8, true, ZONE_SYS);
        g_nc[i].ch = NA_CHAN0 + (i % NA_NCHAN);
        u32 gm = req ? rd32(req + 8 + 72) : 1;
        SDL_LockMutex(g_mx);
        syn_program(g_nc[i].ch, gm > 16384 ? 0 : (int)((gm ? gm - 1 : 0) & 127), gm > 16384);
        syn_volume(g_nc[i].ch, 1.0f); syn_pan(g_nc[i].ch, 0.5f);
        SDL_UnlockMutex(g_mx);
        if (outp) wr32(outp, g_nc[i].handle);
        RETERR(noErr);
        return;
    }
    RETERR(-2003);
}
TRAP(NADisposeNoteChannel) {
    u32 h = ARG(1);
    for (int i = 0; i < 32; i++) if (g_nc[i].used && g_nc[i].handle == h) { g_nc[i].used = false; mm_dispose_ptr(h); }
    RETERR(noErr);
}
/* NAPlayNote(na, channel, pitch, velocity); velocity 0 = note off */
TRAP(NAPlayNote) {
    u32 h = ARG(1); s32 pitch = (s32)ARG(2), vel = (s32)ARG(3);
    if (pitch > 127) pitch >>= 8;
    for (int i = 0; i < 32; i++) if (g_nc[i].used && g_nc[i].handle == h) {
        SDL_LockMutex(g_mx);
        if (vel > 0) syn_note_on(g_nc[i].ch, pitch, vel); else syn_note_off(g_nc[i].ch, pitch);
        SDL_UnlockMutex(g_mx);
    }
    RETERR(noErr);
}
