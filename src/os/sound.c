/* Sound Manager.
 *
 * Sound channels have a guest SndChannel record plus a host-side command
 * queue. Command processing and callbacks (callBackCmd, double-buffer
 * refills) happen on the emulator thread at safe points; the SDL audio
 * thread only mixes the currently playing buffer of each channel.
 */
#include "os.h"
#include "mm.h"
#include "misc.h"
#include "resources.h"
#include "../host/host.h"
#include <SDL.h>
void ev_idle_frame(void);
#include <math.h>

enum {
    nullCmd = 0, quietCmd = 3, flushCmd = 4, reInitCmd = 5, waitCmd = 10, pauseCmd = 11, resumeCmd = 12,
    callBackCmd = 13, syncCmd = 14, availableCmd = 24, versionCmd = 25, freqDurationCmd = 40, restCmd = 41,
    freqCmd = 42, ampCmd = 43, timbreCmd = 44, getAmpCmd = 45, volumeCmd = 46, getVolumeCmd = 47,
    soundCmd = 80, bufferCmd = 81, rateCmd = 82, continueCmd = 83, doubleBufferCmd = 84, getRateCmd = 85,
    rateMultiplierCmd = 86, getRateMultiplierCmd = 87
};
#define dataPointerFlag 0x8000

#define OUT_RATE 44100
#define SNDCHAN_SIZE (36 + 128 * 8)
#define SC_CALLBACK 8
#define SC_USERINFO 12

typedef struct { u16 cmd; s16 p1; u32 p2; } Cmd;

typedef struct {
    bool used;
    u32 chan;               /* guest SndChannel */
    Cmd q[128]; int qh, qt;
    /* current sample buffer */
    bool playing;
    u32 data;               /* guest address of samples */
    u32 frames;
    int channels, bits;
    bool is_signed;
    double rate;            /* source frames per second */
    double pos;
    u32 loop_start, loop_end;
    bool looping;
    /* double buffering */
    bool dbl;
    u32 dbhdr;              /* SndDoubleBufferHeader */
    int dbcur;              /* buffer being played */
    bool db_need[2];        /* buffer consumed, needs refill callback */
    bool db_last;
    /* tones */
    double tone_freq, tone_phase; int tone_samples;
    /* volume */
    int vol_l, vol_r;       /* 0..256 */
    bool paused;
    bool waiting;           /* waitCmd in progress */
    u32 wait_until;
    bool done_flag;         /* buffer finished (set by audio thread) */
} Chan;

#define MAX_CHAN 16
static Chan g_ch[MAX_CHAN];
static SDL_AudioDeviceID g_dev;
static int g_master = 256;
static bool g_audio_ok;

static Chan *chan_of(u32 c) { for (int i = 0; i < MAX_CHAN; i++) if (g_ch[i].used && g_ch[i].chan == c) return &g_ch[i]; return NULL; }

static void lock(void) { if (g_dev) SDL_LockAudioDevice(g_dev); }
static void unlock(void) { if (g_dev) SDL_UnlockAudioDevice(g_dev); }

static inline float sample_at(Chan *c, u32 frame, int ch) {
    if (c->channels == 1) ch = 0;
    u32 idx = frame * (u32)c->channels + (u32)ch;
    if (c->bits == 16) {
        u32 a = c->data + idx * 2;
        if (a + 2 > GUEST_MEM_SIZE) return 0;
        s16 v = (s16)(g_mem[a] << 8 | g_mem[a + 1]);
        return v / 32768.0f;
    }
    u32 a = c->data + idx;
    if (a >= GUEST_MEM_SIZE) return 0;
    return c->is_signed ? (s8)g_mem[a] / 128.0f : ((int)g_mem[a] - 128) / 128.0f;
}

static void audio_cb(void *ud, Uint8 *stream, int len) {
    (void)ud;
    float *out = (float *)stream;
    int n = len / (int)(2 * sizeof(float));
    memset(stream, 0, (size_t)len);
    for (int i = 0; i < MAX_CHAN; i++) {
        Chan *c = &g_ch[i];
        if (!c->used || c->paused) continue;
        float vl = c->vol_l / 256.0f * g_master / 256.0f, vr = c->vol_r / 256.0f * g_master / 256.0f;
        if (c->tone_samples > 0) {
            for (int k = 0; k < n && c->tone_samples > 0; k++, c->tone_samples--) {
                float s = c->tone_phase < 0.5 ? 0.25f : -0.25f;
                c->tone_phase += c->tone_freq / OUT_RATE;
                if (c->tone_phase >= 1) c->tone_phase -= 1;
                out[2 * k] += s * vl; out[2 * k + 1] += s * vr;
            }
            if (c->tone_samples <= 0) c->done_flag = true;
            continue;
        }
        if (!c->playing) continue;
        double step = c->rate / OUT_RATE;
        for (int k = 0; k < n; k++) {
            u32 f = (u32)c->pos;
            if (f >= c->frames) {
                if (c->looping && c->loop_end > c->loop_start) { c->pos = c->loop_start + (c->pos - c->loop_end); f = (u32)c->pos; }
                else if (c->dbl) {
                    /* switch to the other buffer if it's ready */
                    u32 hdr = c->dbhdr;
                    c->db_need[c->dbcur] = true;
                    if (c->db_last) { c->playing = false; c->done_flag = true; break; }
                    int nb = c->dbcur ^ 1;
                    u32 buf = rd32(hdr + 12 + 4 * (u32)nb);
                    u32 flags = rd32(buf + 4);
                    if (!(flags & 1)) { c->playing = false; break; } /* starved */
                    c->dbcur = nb;
                    c->data = buf + 16;
                    c->frames = rd32(buf);
                    c->pos -= (double)f;
                    if (c->pos < 0) c->pos = 0;
                    c->db_last = (flags & 4) != 0;
                    f = (u32)c->pos;
                    if (c->frames == 0) continue;
                } else { c->playing = false; c->done_flag = true; break; }
            }
            float l = sample_at(c, f, 0), r = sample_at(c, f, 1);
            out[2 * k] += l * vl;
            out[2 * k + 1] += r * vr;
            c->pos += step;
        }
    }
    extern void music_render(float *out, int n);
    music_render(out, n);
    for (int k = 0; k < 2 * n; k++) { if (out[k] > 1) out[k] = 1; else if (out[k] < -1) out[k] = -1; }
}

/* Optional capture of the mixed output to a WAV file (tests). When no
   audio device is open, the mixer is driven from sound_service(). */
static FILE *g_wav;
static u64 g_wav_frames;
static u64 g_mix_start_us;
static void wav_header(void) {
    u32 data = (u32)(g_wav_frames * 4);
    u8 h[44] = "RIFF\0\0\0\0WAVEfmt \x10\0\0\0\1\0\2\0";
    put_be32(h + 4, 0);
    u32 v;
#define LE32(o, x) do { v = (x); h[o] = (u8)v; h[o + 1] = (u8)(v >> 8); h[o + 2] = (u8)(v >> 16); h[o + 3] = (u8)(v >> 24); } while (0)
    LE32(4, 36 + data);
    LE32(24, OUT_RATE); LE32(28, OUT_RATE * 4);
    h[32] = 4; h[33] = 0; h[34] = 16; h[35] = 0;
    memcpy(h + 36, "data", 4);
    LE32(40, data);
#undef LE32
    fseek(g_wav, 0, SEEK_SET);
    fwrite(h, 1, 44, g_wav);
    fseek(g_wav, 0, SEEK_END);
}
void sound_wav_close(void) { if (g_wav) { wav_header(); fclose(g_wav); g_wav = NULL; } }
void sound_wav_open(const char *path) {
    g_wav = fopen(path, "wb");
    if (!g_wav) return;
    wav_header();
    atexit(sound_wav_close);
}
static void software_mix(void) {
    if (g_dev) return;
    u64 now = host_now_us();
    if (!g_mix_start_us) g_mix_start_us = now;
    extern int g_turbo;
    u64 due = (now - g_mix_start_us) * OUT_RATE / 1000000u / (u64)(g_turbo > 0 ? g_turbo : 1);
    static u64 done;
    while (done + 512 <= due) {
        float buf[1024];
        audio_cb(NULL, (Uint8 *)buf, sizeof buf);
        done += 512;
        if (g_wav) {
            s16 pcm[1024];
            for (int i = 0; i < 1024; i++) pcm[i] = (s16)(buf[i] * 32767);
            fwrite(pcm, 2, 1024, g_wav); /* host little-endian */
            g_wav_frames += 512;
        }
    }
}

void sound_init(void) {
    extern void music_init(void);
    music_init();
    if (host_is_headless()) return;
    SDL_AudioSpec want = { 0 }, have;
    want.freq = OUT_RATE; want.format = AUDIO_F32SYS; want.channels = 2; want.samples = 1024;
    want.callback = audio_cb;
    g_dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    if (!g_dev) { LOG_W("audio: %s", SDL_GetError()); return; }
    g_audio_ok = true;
    SDL_PauseAudioDevice(g_dev, 0);
}

/* ---- sound headers ---- */
static bool parse_header(Chan *c, u32 h) {
    u8 enc = rd8(h + 20);
    c->data = rd32(h);
    u32 rate = rd32(h + 8);
    c->rate = (rate >> 16) + (rate & 0xFFFF) / 65536.0;
    c->loop_start = rd32(h + 12); c->loop_end = rd32(h + 16);
    c->is_signed = false;
    if (enc == 0) {
        c->frames = rd32(h + 4);
        c->channels = 1; c->bits = 8;
        if (!c->data) c->data = h + 22;
    } else if (enc == 0xFF) {
        c->channels = (int)rd32(h + 4);
        c->frames = rd32(h + 22);
        c->bits = rd16(h + 48);
        if (!c->data) c->data = h + 64;
        c->is_signed = c->bits == 16;
    } else {
        LOG_W("sound: compressed sound header (encode %d) not supported", enc);
        return false;
    }
    if (c->channels < 1) c->channels = 1;
    c->looping = false;
    return true;
}

static void start_buffer(Chan *c, u32 hdr) {
    lock();
    if (parse_header(c, hdr)) { c->pos = 0; c->playing = true; c->done_flag = false; c->dbl = false; }
    unlock();
}

static void do_callback(Chan *c, const Cmd *cmd) {
    u32 cb = rd32(c->chan + SC_CALLBACK);
    if (!cb) return;
    u32 cp = sys_alloc(8);
    wr16(cp, cmd->cmd); wr16(cp + 2, (u16)cmd->p1); wr32(cp + 4, cmd->p2);
    u32 a[2] = { c->chan, cp };
    guest_call_async(cb, 2, a);
    mm_dispose_ptr(cp);
}

/* Execute a command immediately. Returns false if the command must wait
   (it stays at the head of the queue). */
static bool exec_cmd(Chan *c, Cmd *cmd, bool immediate) {
    switch (cmd->cmd & 0x7FFF) {
    case nullCmd: break;
    case quietCmd: lock(); c->playing = false; c->tone_samples = 0; c->dbl = false; unlock(); break;
    case flushCmd: c->qh = c->qt = 0; break;
    case reInitCmd: break;
    case waitCmd: c->waiting = true; c->wait_until = tick_count() + (u32)(cmd->p1 / 2000.0 * 60); break;
    case pauseCmd: c->paused = true; break;
    case resumeCmd: c->paused = false; break;
    case callBackCmd: do_callback(c, cmd); break;
    case volumeCmd: {
        u32 v = cmd->p2;
        lock(); c->vol_r = (int)((v >> 16) & 0xFFFF); c->vol_l = (int)(v & 0xFFFF); unlock();
        break;
    }
    case getVolumeCmd: if (cmd->p2) wr32(cmd->p2, (u32)c->vol_r << 16 | (u32)c->vol_l); break;
    case ampCmd: lock(); c->vol_l = c->vol_r = cmd->p1; unlock(); break;
    case getAmpCmd: if (cmd->p2) wr16(cmd->p2, (u16)c->vol_l); break;
    case soundCmd: case bufferCmd: {
        if (!immediate && (c->playing || c->tone_samples > 0)) return false;
        start_buffer(c, cmd->p2);
        if ((cmd->cmd & 0x7FFF) == soundCmd) { lock(); c->playing = false; unlock(); }
        break;
    }
    case rateCmd: { u32 r = cmd->p2; lock(); c->rate = (r >> 16) + (r & 0xFFFF) / 65536.0; unlock(); break; }
    case getRateCmd: if (cmd->p2) wr32(cmd->p2, (u32)(c->rate * 65536)); break;
    case freqDurationCmd: case freqCmd: {
        if (!immediate && (c->playing || c->tone_samples > 0)) return false;
        int note = cmd->p2 & 0xFF;
        double f = 440.0 * pow(2.0, (note - 69) / 12.0);
        lock();
        c->tone_freq = f;
        c->tone_samples = (cmd->cmd & 0x7FFF) == freqDurationCmd ? (int)(cmd->p1 / 2000.0 * OUT_RATE) : OUT_RATE;
        unlock();
        break;
    }
    case restCmd: c->waiting = true; c->wait_until = tick_count() + (u32)(cmd->p1 / 2000.0 * 60); break;
    case availableCmd: if (cmd->p2) wr16(cmd->p2 + 2, 1); break;
    case versionCmd: if (cmd->p2) wr32(cmd->p2, 0x00030000); break;
    default: LOG_D("sound: command %d ignored", cmd->cmd & 0x7FFF); break;
    }
    return true;
}

/* Run queued commands; called at safe points. */
void sound_service(void) {
    software_mix();
    for (int i = 0; i < MAX_CHAN; i++) {
        Chan *c = &g_ch[i];
        if (!c->used) continue;
        /* double-buffer refills */
        if (c->dbl) {
            for (int b = 0; b < 2; b++) {
                if (c->db_need[b]) {
                    c->db_need[b] = false;
                    u32 hdr = c->dbhdr;
                    u32 buf = rd32(hdr + 12 + 4 * (u32)b);
                    u32 upp = rd32(hdr + 20);
                    wr32(buf + 4, rd32(buf + 4) & ~1u); /* clear dbBufferReady */
                    if (upp && !c->db_last) { u32 a[2] = { c->chan, buf }; guest_call_async(upp, 2, a); }
                }
            }
            if (!c->playing && !c->db_last) {
                /* restart after starvation if the next buffer became ready */
                u32 hdr = c->dbhdr;
                int nb = c->dbcur ^ 1;
                u32 buf = rd32(hdr + 12 + 4 * (u32)nb);
                if (rd32(buf + 4) & 1) {
                    lock();
                    c->dbcur = nb; c->data = buf + 16; c->frames = rd32(buf); c->pos = 0; c->playing = true;
                    c->db_last = (rd32(buf + 4) & 4) != 0;
                    unlock();
                }
            }
        }
        if (c->waiting) {
            if ((s32)(tick_count() - c->wait_until) < 0) continue;
            c->waiting = false;
        }
        while (c->qh != c->qt && !c->waiting) {
            Cmd *cmd = &c->q[c->qh];
            if (!exec_cmd(c, cmd, false)) break;
            c->qh = (c->qh + 1) % 128;
        }
    }
}

static int enqueue(Chan *c, u32 cmdp, bool nowait) {
    int n = (c->qt + 1) % 128;
    if (n == c->qh) {
        if (nowait) return -203; /* queueFull */
        while (n == c->qh) { sound_service(); ev_idle_frame(); }
    }
    c->q[c->qt] = (Cmd){ rd16(cmdp), rds16(cmdp + 2), rd32(cmdp + 4) };
    c->qt = n;
    sound_service();
    return noErr;
}

/* ---------------------------------------------------------------------- */
/* Traps                                                                   */

TRAP(SndNewChannel) {
    u32 cp = ARG(0); s16 synth = ARGS16(1); u32 init = ARG(2); u32 user = ARG(3);
    (void)synth; (void)init;
    u32 chan = rd32(cp);
    Chan *c = NULL;
    for (int i = 0; i < MAX_CHAN; i++) if (!g_ch[i].used) { c = &g_ch[i]; break; }
    if (!c) { RETERR(-201); return; } /* notEnoughHardwareErr */
    if (!chan) chan = mm_new_ptr(SNDCHAN_SIZE, true, ZONE_APP);
    lock();
    memset(c, 0, sizeof *c);
    c->used = true; c->chan = chan;
    c->vol_l = c->vol_r = 256;
    unlock();
    wr32(chan + SC_CALLBACK, user);
    wr16(chan + 32, 128); /* qLength */
    wr32(cp, chan);
    RETERR(noErr);
}

TRAP(SndDisposeChannel) {
    u32 chan = ARG(0);
    Chan *c = chan_of(chan);
    if (!c) { RETERR(-205); return; } /* badChannel */
    lock(); c->used = false; unlock();
    RETERR(noErr);
}

TRAP(SndDoCommand) {
    Chan *c = chan_of(ARG(0));
    if (!c) { RETERR(-205); return; }
    RETERR(enqueue(c, ARG(1), ARGB(2)));
}

TRAP(SndDoImmediate) {
    Chan *c = chan_of(ARG(0));
    if (!c) { RETERR(-205); return; }
    u32 cp = ARG(1);
    Cmd cmd = { rd16(cp), rds16(cp + 2), rd32(cp + 4) };
    exec_cmd(c, &cmd, true);
    RETERR(noErr);
}

TRAP(SndPlayDoubleBuffer) {
    u32 chan = ARG(0), hdr = ARG(1);
    Chan *c = chan_of(chan);
    if (!c) { RETERR(-205); return; }
    int chans = rd16(hdr), bits = rd16(hdr + 2);
    if (rd16(hdr + 4) != 0) { LOG_W("SndPlayDoubleBuffer: compressed data not supported"); RETERR(-206); return; }
    u32 rate = rd32(hdr + 8);
    lock();
    c->dbl = true; c->dbhdr = hdr;
    c->channels = chans; c->bits = bits; c->is_signed = bits == 16;
    c->rate = (rate >> 16) + (rate & 0xFFFF) / 65536.0;
    c->db_last = false;
    unlock();
    /* the app fills both buffers first by calling the doubleback proc */
    u32 upp = rd32(hdr + 20);
    for (int b = 0; b < 2; b++) {
        u32 buf = rd32(hdr + 12 + 4 * (u32)b);
        if (!(rd32(buf + 4) & 1) && upp) { u32 a[2] = { chan, buf }; call_upp(upp, 2, a); }
    }
    u32 buf0 = rd32(hdr + 12);
    lock();
    c->dbcur = 0; c->data = buf0 + 16; c->frames = rd32(buf0); c->pos = 0;
    c->db_last = (rd32(buf0 + 4) & 4) != 0;
    c->playing = true;
    unlock();
    LOG_D("SndPlayDoubleBuffer: %d ch, %d bit, %.0f Hz, %u frames", chans, bits, c->rate, c->frames);
    RETERR(noErr);
}

/* Parse a 'snd ' resource: returns the sound header address or 0. */
static u32 snd_header(u32 h) {
    u32 p = hderef(h);
    u16 fmt = rd16(p);
    u32 q;
    if (fmt == 1) {
        u16 nmods = rd16(p + 2);
        q = p + 4 + 6 * (u32)nmods;
    } else if (fmt == 2) q = p + 4;
    else return 0;
    u16 ncmds = rd16(q);
    q += 2;
    for (int i = 0; i < ncmds; i++, q += 8) {
        u16 cmd = rd16(q);
        if ((cmd & 0x7FFF) == bufferCmd || (cmd & 0x7FFF) == soundCmd)
            return (cmd & dataPointerFlag) ? p + rd32(q + 4) : rd32(q + 4);
    }
    return 0;
}

static u32 g_sysbeep_chan;
TRAP(SndPlay) {
    u32 chan = ARG(0), h = ARG(1); bool async = ARGB(2);
    if (!h || !hderef(h)) { RETERR(-201); return; }
    u32 hdr = snd_header(h);
    if (!hdr) { RETERR(-206); return; }
    if (!chan) {
        if (!g_sysbeep_chan) {
            u32 cp = sys_alloc(4);
            CPU f; memset(&f, 0, sizeof f); f.r[3] = cp; f.r[4] = 5;
            trap_SndNewChannel(&f);
            g_sysbeep_chan = rd32(cp);
        }
        chan = g_sysbeep_chan;
    }
    Chan *c = chan_of(chan);
    if (!c) { RETERR(-205); return; }
    start_buffer(c, hdr);
    if (!async) while (c->playing && g_audio_ok) { sound_service(); ev_idle_frame(); }
    RETERR(noErr);
}

TRAP(SysBeep) {
    if (!g_sysbeep_chan) {
        u32 cp = sys_alloc(4);
        CPU f; memset(&f, 0, sizeof f); f.r[3] = cp; f.r[4] = 5;
        trap_SndNewChannel(&f);
        g_sysbeep_chan = rd32(cp);
    }
    Chan *c = chan_of(g_sysbeep_chan);
    lock(); c->tone_freq = 1000; c->tone_samples = OUT_RATE / 8; unlock();
}

TRAP(SndSoundManagerVersion) { wr32(ARG(0), 0x03208000); } /* NumVersion returned through a hidden pointer */
TRAP(SndGetInfo) { RETERR(-2201); /* siUnknownInfoType */ }
TRAP(SndChannelStatus) {
    Chan *c = chan_of(ARG(0)); u32 st = ARG(2);
    gmemset(st, 0, 24);
    if (c) wr8(st + 16, c->playing);
    RETERR(noErr);
}
TRAP(GetDefaultOutputVolume) { u32 p = ARG(0); if (p) wr32(p, (u32)g_master << 16 | (u32)g_master); RETERR(noErr); }
TRAP(SetDefaultOutputVolume) { u32 v = ARG(0); g_master = (int)(v & 0xFFFF); if (g_master > 256) g_master = 256; RETERR(noErr); }
TRAP(GetSoundVol) { wr16(ARG(0), (u16)(g_master * 7 / 256)); }
TRAP(SetSoundVol) { g_master = ARGS16(0) * 256 / 7; }
