// DirectSound for D2sound.dll: COM objects in guest memory whose vtables point at native thunks, and a
// software mixer on SDL audio (44.1 kHz stereo) over the buffers' guest memory. Also the video codec
// stubs (Bink/Smack: no cinematics) and the JPEG library stub.
#include "native.h"
#include <SDL.h>
#include <pthread.h>
#include <math.h>

#define DS "dsound.dll"
#define E_NOINTERFACE 0x80004002u
#define DSERR_INVALIDPARAM 0x80070057u

typedef struct {
    int used, primary, playing, looping;
    u32 data, size;            // guest buffer
    u32 rate, channels, bits;
    u32 freq;                  // playback frequency (SetFrequency)
    s32 volume, pan;           // hundredths of dB
    u64 pos;                   // play position, bytes << 16
    u32 obj;                   // guest COM object
} Buf;
#define MAXBUF 256
static Buf g_buf[MAXBUF];
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static SDL_AudioDeviceID g_dev;
static u32 g_vt_ds, g_vt_buf, g_ds_obj;

static float db_to_lin(s32 v) { return v <= -10000 ? 0.f : powf(10.f, v / 2000.f); }

static void mix(void* ud, Uint8* stream, int len) {
    (void)ud;
    s16* out = (s16*)stream;
    int frames = len / 4;
    static float acc[8192];
    if (frames > 4096) frames = 4096;
    memset(acc, 0, sizeof(float) * frames * 2);
    pthread_mutex_lock(&g_mu);
    for (int b = 0; b < MAXBUF; ++b) {
        Buf* k = &g_buf[b];
        if (!k->used || k->primary || !k->playing || !k->size) continue;
        float vol = db_to_lin(k->volume);
        float lv = vol * (k->pan > 0 ? db_to_lin(-k->pan) : 1.f), rv = vol * (k->pan < 0 ? db_to_lin(k->pan) : 1.f);
        u32 bpf = k->channels * (k->bits / 8);
        if (!bpf) continue;
        u64 step = ((u64)(k->freq ? k->freq : k->rate) << 16) / 44100;  // frames per output frame, 16.16
        u64 fpos = k->pos / bpf;                                         // frame position << 16
        u64 total = (u64)(k->size / bpf) << 16;
        for (int i = 0; i < frames; ++i) {
            if (fpos >= total) {
                if (k->looping) fpos -= total;
                else { k->playing = 0; break; }
            }
            u32 at = k->data + (u32)(fpos >> 16) * bpf;
            float l, r;
            if (k->bits == 16) {
                l = (s16)rd16(at) / 32768.f;
                r = k->channels == 2 ? (s16)rd16(at + 2) / 32768.f : l;
            } else {
                l = ((int)rd8(at) - 128) / 128.f;
                r = k->channels == 2 ? ((int)rd8(at + 1) - 128) / 128.f : l;
            }
            acc[2 * i] += l * lv;
            acc[2 * i + 1] += r * rv;
            fpos += step;
        }
        k->pos = fpos * bpf;
    }
    pthread_mutex_unlock(&g_mu);
    for (int i = 0; i < frames * 2; ++i) {
        float v = acc[i] * 32767.f;
        out[i] = (s16)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
    }
    if (len > frames * 4) memset(stream + frames * 4, 0, len - frames * 4);
}

static void audio_open(void) {
    if (g_dev) return;
    SDL_InitSubSystem(SDL_INIT_AUDIO);
    SDL_AudioSpec want = {0}, got;
    want.freq = 44100; want.format = AUDIO_S16SYS; want.channels = 2; want.samples = 1024; want.callback = mix;
    g_dev = SDL_OpenAudioDevice(NULL, 0, &want, &got, 0);
    if (!g_dev) LOG("audio: %s", SDL_GetError());
    else SDL_PauseAudioDevice(g_dev, 0);
}

static Buf* buf_of(u32 obj) { u32 i = rd32(obj + 4); return i < MAXBUF && g_buf[i].used ? &g_buf[i] : NULL; }

static u32 make_buffer(u32 desc) {
    u32 flags = rd32(desc + 4), bytes = rd32(desc + 8), wfx = rd32(desc + 16);
    pthread_mutex_lock(&g_mu);
    int i;
    for (i = 0; i < MAXBUF && g_buf[i].used; ++i) {}
    if (i == MAXBUF) { pthread_mutex_unlock(&g_mu); return 0; }
    Buf* k = &g_buf[i];
    memset(k, 0, sizeof *k);
    k->used = 1;
    k->primary = flags & 1;
    k->rate = 22050; k->channels = 2; k->bits = 16;
    if (wfx) { k->channels = rd16(wfx + 2); k->rate = rd32(wfx + 4); k->bits = rd16(wfx + 14); }
    k->freq = k->rate;
    if (!k->primary && bytes) { k->size = bytes; k->data = gcalloc(bytes); }
    k->obj = gcalloc(8);
    wr32(k->obj, g_vt_buf);
    wr32(k->obj + 4, (u32)i);
    pthread_mutex_unlock(&g_mu);
    return k->obj;
}

// ---- IDirectSound
static void ds_QueryInterface(Cpu* c) { if (ARG(2)) wr32(ARG(2), 0); RET(E_NOINTERFACE, 3); }
static void ds_AddRef(Cpu* c) { RET(1, 1); }
static void ds_Release(Cpu* c) { RET(0, 1); }
static void ds_CreateSoundBuffer(Cpu* c) {
    u32 o = make_buffer(ARG(1));
    if (!o) RET(DSERR_INVALIDPARAM, 4);
    wr32(ARG(2), o);
    RET(0, 4);
}
static void ds_GetCaps(Cpu* c) {
    u32 p = ARG(1);
    u32 sz = rd32(p);
    memset(H(p + 4), 0, sz > 4 ? sz - 4 : 0);
    wr32(p + 4, 0x0F5F);  // continuous rates, prim/sec 8/16 mono/stereo, certified
    wr32(p + 8, 100); wr32(p + 12, 100000);
    wr32(p + 20, 64); wr32(p + 24, 64);
    RET(0, 2);
}
static void ds_DuplicateSoundBuffer(Cpu* c) {
    Buf* s = buf_of(ARG(1));
    if (!s) RET(DSERR_INVALIDPARAM, 3);
    u32 desc = gcalloc(36), wfx = gcalloc(20);
    wr16(wfx, 1); wr16(wfx + 2, (u16)s->channels); wr32(wfx + 4, s->rate); wr16(wfx + 14, (u16)s->bits);
    wr32(desc, 36); wr32(desc + 4, 0xE0); wr32(desc + 8, 0); wr32(desc + 16, wfx);
    u32 o = make_buffer(desc);
    gfree(desc); gfree(wfx);
    Buf* d = buf_of(o);
    pthread_mutex_lock(&g_mu);
    d->data = s->data; d->size = s->size;  // shares the sound data
    pthread_mutex_unlock(&g_mu);
    wr32(ARG(2), o);
    RET(0, 3);
}
static void ds_SetCooperativeLevel(Cpu* c) { RET(0, 3); }
static void ds_Compact(Cpu* c) { RET(0, 1); }
static void ds_GetSpeakerConfig(Cpu* c) { if (ARG(1)) wr32(ARG(1), 4); RET(0, 2); }  // stereo
static void ds_SetSpeakerConfig(Cpu* c) { RET(0, 2); }
static void ds_Initialize(Cpu* c) { RET(0, 2); }

// ---- IDirectSoundBuffer
static void b_Release(Cpu* c) {
    pthread_mutex_lock(&g_mu);
    Buf* k = buf_of(ARG(0));
    if (k) { k->used = 0; k->playing = 0; }
    pthread_mutex_unlock(&g_mu);
    RET(0, 1);
}
static void b_GetCaps(Cpu* c) {
    Buf* k = buf_of(ARG(0));
    u32 p = ARG(1);
    wr32(p + 4, 0xE0 | (k && k->primary));
    wr32(p + 8, k ? k->size : 0);
    RET(0, 2);
}
static void b_GetCurrentPosition(Cpu* c) {
    Buf* k = buf_of(ARG(0));
    u32 play = 0;
    if (k && k->size) { pthread_mutex_lock(&g_mu); play = (u32)(k->pos >> 16) % k->size; pthread_mutex_unlock(&g_mu); }
    if (ARG(1)) wr32(ARG(1), play);
    if (ARG(2)) wr32(ARG(2), k && k->size ? (play + 4410 * k->channels * k->bits / 8 / 10) % k->size : 0);
    RET(0, 3);
}
static void b_GetFormat(Cpu* c) {
    Buf* k = buf_of(ARG(0));
    u32 w = ARG(1);
    if (w && k) {
        wr16(w, 1); wr16(w + 2, (u16)k->channels); wr32(w + 4, k->rate);
        wr32(w + 8, k->rate * k->channels * k->bits / 8); wr16(w + 12, (u16)(k->channels * k->bits / 8)); wr16(w + 14, (u16)k->bits);
    }
    if (ARG(3)) wr32(ARG(3), 16);
    RET(0, 4);
}
static void b_GetVolume(Cpu* c) { Buf* k = buf_of(ARG(0)); if (ARG(1)) wr32(ARG(1), k ? (u32)k->volume : 0); RET(0, 2); }
static void b_GetPan(Cpu* c) { Buf* k = buf_of(ARG(0)); if (ARG(1)) wr32(ARG(1), k ? (u32)k->pan : 0); RET(0, 2); }
static void b_GetFrequency(Cpu* c) { Buf* k = buf_of(ARG(0)); if (ARG(1)) wr32(ARG(1), k ? k->freq : 0); RET(0, 2); }
static void b_GetStatus(Cpu* c) {
    Buf* k = buf_of(ARG(0));
    u32 s = k && k->playing ? (1 | (k->looping ? 4 : 0)) : 0;
    if (ARG(1)) wr32(ARG(1), s);
    RET(0, 2);
}
static void b_Initialize(Cpu* c) { RET(0, 3); }
static void b_Lock(Cpu* c) {
    Buf* k = buf_of(ARG(0));
    u32 off = ARG(1), n = ARG(2), flags = ARG(7);
    if (!k || !k->size) {
        if (ARG(3)) wr32(ARG(3), 0);
        if (ARG(4)) wr32(ARG(4), 0);
        if (ARG(5)) wr32(ARG(5), 0);
        if (ARG(6)) wr32(ARG(6), 0);
        RET(k ? 0 : DSERR_INVALIDPARAM, 8);
    }
    if (flags & 2) n = k->size;                       // DSBLOCK_ENTIREBUFFER
    if (flags & 1) off = (u32)(k->pos >> 16) % k->size;  // DSBLOCK_FROMWRITECURSOR
    off %= k->size;
    if (n > k->size) n = k->size;
    u32 n1 = off + n <= k->size ? n : k->size - off;
    wr32(ARG(3), k->data + off);
    wr32(ARG(4), n1);
    if (ARG(5)) wr32(ARG(5), n1 < n ? k->data : 0);
    if (ARG(6)) wr32(ARG(6), n - n1);
    RET(0, 8);
}
static void b_Play(Cpu* c) {
    Buf* k = buf_of(ARG(0));
    if (k) { pthread_mutex_lock(&g_mu); k->playing = 1; k->looping = ARG(3) & 1; pthread_mutex_unlock(&g_mu); }
    RET(0, 4);
}
static void b_SetCurrentPosition(Cpu* c) {
    Buf* k = buf_of(ARG(0));
    if (k) { pthread_mutex_lock(&g_mu); k->pos = (u64)ARG(1) << 16; pthread_mutex_unlock(&g_mu); }
    RET(0, 2);
}
static void b_SetFormat(Cpu* c) {
    Buf* k = buf_of(ARG(0));
    u32 w = ARG(1);
    if (k && w) { k->channels = rd16(w + 2); k->rate = rd32(w + 4); k->bits = rd16(w + 14); k->freq = k->rate; }
    RET(0, 2);
}
static void b_SetVolume(Cpu* c) { Buf* k = buf_of(ARG(0)); if (k) k->volume = (s32)ARG(1); RET(0, 2); }
static void b_SetPan(Cpu* c) { Buf* k = buf_of(ARG(0)); if (k) k->pan = (s32)ARG(1); RET(0, 2); }
static void b_SetFrequency(Cpu* c) { Buf* k = buf_of(ARG(0)); if (k) k->freq = ARG(1) ? ARG(1) : k->rate; RET(0, 2); }
static void b_Stop(Cpu* c) { Buf* k = buf_of(ARG(0)); if (k) { pthread_mutex_lock(&g_mu); k->playing = 0; pthread_mutex_unlock(&g_mu); } RET(0, 1); }
static void b_Unlock(Cpu* c) { RET(0, 5); }
static void b_Restore(Cpu* c) { RET(0, 1); }

static u32 vtable(const char* const* names, int n) {
    u32 vt = gcalloc((u32)n * 4);
    for (int i = 0; i < n; ++i) wr32(vt + 4 * i, native_thunk("dsound.com", names[i], 0));
    return vt;
}

static const char* const DS_M[] = {"ds_QueryInterface", "ds_AddRef", "ds_Release", "ds_CreateSoundBuffer", "ds_GetCaps",
    "ds_DuplicateSoundBuffer", "ds_SetCooperativeLevel", "ds_Compact", "ds_GetSpeakerConfig", "ds_SetSpeakerConfig", "ds_Initialize"};
static const char* const B_M[] = {"ds_QueryInterface", "ds_AddRef", "b_Release", "b_GetCaps", "b_GetCurrentPosition", "b_GetFormat",
    "b_GetVolume", "b_GetPan", "b_GetFrequency", "b_GetStatus", "b_Initialize", "b_Lock", "b_Play", "b_SetCurrentPosition",
    "b_SetFormat", "b_SetVolume", "b_SetPan", "b_SetFrequency", "b_Stop", "b_Unlock", "b_Restore"};

// DirectSoundCreate(guid, LPDIRECTSOUND*, outer)
static void nt_DirectSoundCreate(Cpu* c) {
    audio_open();
    if (!g_vt_ds) { g_vt_ds = vtable(DS_M, 11); g_vt_buf = vtable(B_M, 21); }
    if (!g_ds_obj) { g_ds_obj = gcalloc(8); wr32(g_ds_obj, g_vt_ds); }
    wr32(ARG(1), g_ds_obj);
    LOG("DirectSoundCreate -> %08X", g_ds_obj);
    RET(0, 3);
}
// DirectSoundEnumerateA(callback, context): one device
static void nt_DirectSoundEnumerateA(Cpu* c) {
    u32 cb = ARG(0), ctx = ARG(1);
    u32 desc = gstrdup("Primary Sound Driver"), mod = gstrdup("");
    u32 a[4] = {0, desc, mod, ctx};
    call_guest(c, cb, 4, a);
    RET(0, 2);
}

// ---- Bink / Smack / IJL stubs: no videos (the game skips them), no JPEG
static void nt_r0_1(Cpu* c) { RET(0, 1); }
static void nt_r0_2(Cpu* c) { RET(0, 2); }
static void nt_r0_3(Cpu* c) { RET(0, 3); }

static void nt_r0_7(Cpu* c) { RET(0, 7); }
static void nt_ijl_err1(Cpu* c) { RET(0xFFFFFFFFu, 1); }
static void nt_ijl_err2(Cpu* c) { RET(0xFFFFFFFFu, 2); }

#define M(n) {"dsound.com", #n, 0, n}
NATIVE_TABLE(dsound,
    {DS, NULL, 1, nt_DirectSoundCreate}, {DS, NULL, 2, nt_DirectSoundEnumerateA},
    {DS, "DirectSoundCreate", 0, nt_DirectSoundCreate}, {DS, "DirectSoundEnumerateA", 0, nt_DirectSoundEnumerateA},
    M(ds_QueryInterface), M(ds_AddRef), M(ds_Release), M(ds_CreateSoundBuffer), M(ds_GetCaps), M(ds_DuplicateSoundBuffer),
    M(ds_SetCooperativeLevel), M(ds_Compact), M(ds_GetSpeakerConfig), M(ds_SetSpeakerConfig), M(ds_Initialize),
    M(b_Release), M(b_GetCaps), M(b_GetCurrentPosition), M(b_GetFormat), M(b_GetVolume), M(b_GetPan), M(b_GetFrequency),
    M(b_GetStatus), M(b_Initialize), M(b_Lock), M(b_Play), M(b_SetCurrentPosition), M(b_SetFormat), M(b_SetVolume),
    M(b_SetPan), M(b_SetFrequency), M(b_Stop), M(b_Unlock), M(b_Restore),
    {"binkw32.dll", "_BinkOpen@8", 0, nt_r0_2}, {"binkw32.dll", "_BinkClose@4", 0, nt_r0_1},
    {"binkw32.dll", "_BinkCopyToBuffer@28", 0, nt_r0_7}, {"binkw32.dll", "_BinkDoFrame@4", 0, nt_r0_1},
    {"binkw32.dll", "_BinkNextFrame@4", 0, nt_r0_1}, {"binkw32.dll", "_BinkOpenDirectSound@4", 0, nt_r0_1},
    {"binkw32.dll", "_BinkSetSoundSystem@8", 0, nt_r0_2}, {"binkw32.dll", "_BinkWait@4", 0, nt_r0_1},
    {"smackw32.dll", "_SmackOpen@12", 0, nt_r0_3}, {"smackw32.dll", "_SmackClose@4", 0, nt_r0_1},
    {"smackw32.dll", "_SmackDoFrame@4", 0, nt_r0_1}, {"smackw32.dll", "_SmackNextFrame@4", 0, nt_r0_1},
    {"smackw32.dll", "_SmackToBuffer@28", 0, nt_r0_7}, {"smackw32.dll", "_SmackWait@4", 0, nt_r0_1},
    {"ijl11.dll", NULL, 2, nt_ijl_err1}, {"ijl11.dll", NULL, 3, nt_ijl_err1}, {"ijl11.dll", NULL, 5, nt_ijl_err2},
)
