/* mss32.dll: the Miles Sound System 6.5 API the game imports, on the host audio
 * device (SDL).
 *
 * One software mixer plays three kinds of voices:
 *  - 2D samples (AIL_allocate_sample_handle): PCM in guest memory set with
 *    AIL_set_sample_address, or queued with the double-buffer calls
 *    (AIL_load_sample_buffer) for streamed raw audio;
 *  - 3D samples (AIL_allocate_3D_sample_handle): PCM described by an AILSOUNDINFO,
 *    attenuated by distance and panned from their position relative to the
 *    listener at the origin (the game positions them relative to the listener);
 *  - streams (AIL_open_stream): WAV or MP3 files read whole through the game's
 *    file callbacks when opened, decoded on the fly (MP3: dr_mp3).
 *
 * Volumes are linear 0..1, pans 0 (left) .. 0.5 .. 1 (right), as in MSS 6.5.
 * Handles given to the game are guest addresses (opaque to it). */
#include "shim.h"

#include <SDL.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <strings.h>

#define DR_MP3_IMPLEMENTATION
#define DR_MP3_NO_STDIO
#include "../../third_party/dr_libs/dr_mp3.h"

#define SMP_FREE 0x0001u
#define SMP_DONE 0x0002u
#define SMP_PLAYING 0x0004u
#define SMP_STOPPED 0x0008u

#define DIG_F_16BITS_MASK 1u
#define DIG_F_STEREO_MASK 2u
#define DIG_F_ADPCM_MASK 4u

#define WAVE_FORMAT_PCM 1u
#define WAVE_FORMAT_IMA_ADPCM 0x11u

enum { V_2D, V_3D, V_STREAM };

#define MAX_VOICES 256

typedef struct Voice {
    int used, kind;
    uint32_t handle;              /* guest address the game holds */
    uint32_t status;
    /* source: guest PCM (data/len), or a decoded host buffer (pcm, streams) */
    uint32_t data, len;
    int bits, channels, adpcm;
    uint32_t block_size;
    uint32_t rate;
    double pos;                   /* in frames */
    int loops;                    /* 1: once, 0: forever, n: n times */
    float volume, pan;
    /* 3D */
    float x, y, z, min_dist, max_dist;
    /* double buffering (2D): copies of the guest buffers */
    int16_t *qbuf[2];
    uint32_t qframes[2];
    int qfull[2], qcur, qended;
    /* streams: the whole file in host memory */
    uint8_t *file;
    uint32_t file_len;
    int is_mp3;
    drmp3 mp3;
    int16_t *dec;                 /* decoded block (stereo or mono frames) */
    uint32_t dec_frames, dec_pos;
    uint32_t total_frames;
    int paused;
} Voice;

static Voice voices[MAX_VOICES];
static SDL_mutex *lock;
static SDL_AudioDeviceID dev;
static int out_rate = 44100;
static uint32_t driver_handle;
static uint32_t file_cb[4];       /* open, close, seek, read (guest, stdcall) */
static float listener_scale = 1.0f;

static Voice *voice_of(uint32_t h)
{
    for (int i = 0; i < MAX_VOICES; i++)
        if (voices[i].used && voices[i].handle == h)
            return &voices[i];
    return NULL;
}

static Voice *voice_new(int kind)
{
    for (int i = 0; i < MAX_VOICES; i++) {
        Voice *v = &voices[i];
        if (!v->used) {
            uint32_t h = v->handle ? v->handle : scratch_alloc(16, 16);
            memset(v, 0, sizeof(*v));
            v->used = 1;
            v->kind = kind;
            v->handle = h;
            v->status = SMP_DONE;
            v->volume = 1.0f;
            v->pan = 0.5f;
            v->loops = 1;
            v->bits = 16;
            v->channels = 1;
            v->rate = 22050;
            v->min_dist = 1.0f;
            v->max_dist = 1000.0f;
            return v;
        }
    }
    return NULL;
}

/* ---- decoding ------------------------------------------------------------- */

/* IMA ADPCM (Microsoft layout): one block -> 16-bit frames. */
static const int ima_index[16] = { -1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8 };
static const int ima_step[89] = {
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97,
    107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796,
    876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871,
    5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623,
    27086, 29794, 32767 };

static int ima_nibble(int *pred, int *idx, int n)
{
    int step = ima_step[*idx];
    int diff = step >> 3;
    if (n & 1)
        diff += step >> 2;
    if (n & 2)
        diff += step >> 1;
    if (n & 4)
        diff += step;
    *pred += (n & 8) ? -diff : diff;
    if (*pred > 32767)
        *pred = 32767;
    if (*pred < -32768)
        *pred = -32768;
    *idx += ima_index[n];
    if (*idx < 0)
        *idx = 0;
    if (*idx > 88)
        *idx = 88;
    return *pred;
}

/* Decodes the IMA ADPCM data at src (len bytes, blocks of block_size) to 16-bit. */
static int16_t *ima_decode(const uint8_t *src, uint32_t len, uint32_t block_size, int channels, uint32_t *frames_out)
{
    if (block_size < 4u * (uint32_t)channels)
        return NULL;
    uint32_t per_block = (block_size - 4u * (uint32_t)channels) * 2u / (uint32_t)channels + 1u;
    uint32_t blocks = (len + block_size - 1u) / block_size;
    int16_t *out = malloc((size_t)blocks * per_block * (size_t)channels * 2u);
    if (!out)
        return NULL;
    uint32_t f = 0;
    for (uint32_t b = 0; b < blocks; b++) {
        const uint8_t *p = src + b * block_size;
        uint32_t avail = len - b * block_size < block_size ? len - b * block_size : block_size;
        if (avail < 4u * (uint32_t)channels)
            break;
        int pred[2], idx[2];
        for (int ch = 0; ch < channels; ch++) {
            pred[ch] = (int16_t)(p[ch * 4] | p[ch * 4 + 1] << 8);
            idx[ch] = p[ch * 4 + 2] > 88 ? 88 : p[ch * 4 + 2];
            out[(size_t)f * channels + ch] = (int16_t)pred[ch];
        }
        f++;
        const uint8_t *d = p + 4 * channels, *end = p + avail;
        if (channels == 1) {
            for (; d < end; d++) {
                out[f++] = (int16_t)ima_nibble(&pred[0], &idx[0], *d & 15);
                out[f++] = (int16_t)ima_nibble(&pred[0], &idx[0], *d >> 4);
            }
        } else {
            /* 4 bytes (8 samples) of the left channel, then 4 of the right. */
            for (; d + 8 <= end; d += 8) {
                for (int ch = 0; ch < 2; ch++)
                    for (int k = 0; k < 4; k++) {
                        uint8_t byte = d[ch * 4 + k];
                        out[(size_t)(f + k * 2) * 2 + ch] = (int16_t)ima_nibble(&pred[ch], &idx[ch], byte & 15);
                        out[(size_t)(f + k * 2 + 1) * 2 + ch] = (int16_t)ima_nibble(&pred[ch], &idx[ch], byte >> 4);
                    }
                f += 8;
            }
        }
    }
    *frames_out = f;
    return out;
}

/* Fields of a RIFF WAVE file in memory. */
typedef struct WavInfo {
    uint32_t format, rate, channels, bits, block_size;
    uint32_t data_off, data_len;
} WavInfo;

static int wav_parse(const uint8_t *p, uint32_t len, WavInfo *w)
{
    memset(w, 0, sizeof(*w));
    if (len < 12 || memcmp(p, "RIFF", 4) || memcmp(p + 8, "WAVE", 4))
        return 0;
    uint32_t off = 12;
    int have_fmt = 0;
    while (off + 8 <= len) {
        uint32_t n;
        memcpy(&n, p + off + 4, 4);
        const uint8_t *ck = p + off + 8;
        if (!memcmp(p + off, "fmt ", 4) && n >= 16 && off + 8 + 16 <= len) {
            w->format = ck[0] | ck[1] << 8;
            w->channels = ck[2] | ck[3] << 8;
            memcpy(&w->rate, ck + 4, 4);
            w->block_size = ck[12] | ck[13] << 8;
            w->bits = ck[14] | ck[15] << 8;
            have_fmt = 1;
        } else if (!memcmp(p + off, "data", 4)) {
            w->data_off = off + 8;
            w->data_len = n > len - w->data_off ? len - w->data_off : n;
            return have_fmt;
        }
        off += 8 + n + (n & 1);
    }
    return 0;
}

/* ---- mixer ---------------------------------------------------------------- */

/* Frame `i` of a guest PCM buffer as two floats (-1..1). */
static inline void pcm_frame(const uint8_t *d, int bits, int channels, uint32_t i, float *l, float *r)
{
    if (bits == 16) {
        int16_t a, b;
        memcpy(&a, d + (size_t)i * channels * 2, 2);
        b = a;
        if (channels == 2)
            memcpy(&b, d + (size_t)i * 4 + 2, 2);
        *l = a * (1.0f / 32768.0f);
        *r = b * (1.0f / 32768.0f);
    } else {
        int a = d[(size_t)i * channels] - 128, b = channels == 2 ? d[(size_t)i * 2 + 1] - 128 : a;
        *l = a * (1.0f / 128.0f);
        *r = b * (1.0f / 128.0f);
    }
}

/* Gains of a voice now (3D: distance and direction from the listener). */
static void voice_gains(const Voice *v, float *gl, float *gr)
{
    float vol = v->volume, pan = v->pan;
    if (v->kind == V_3D) {
        float d = sqrtf(v->x * v->x + v->y * v->y + v->z * v->z) * listener_scale;
        if (d > v->max_dist) {
            vol = 0.0f;
        } else if (d > v->min_dist && d > 0.0f) {
            vol *= v->min_dist / d;
        }
        pan = d > 0.001f ? 0.5f + 0.5f * (v->x / d) : 0.5f;
    }
    *gl = vol * (pan < 0.5f ? 1.0f : 2.0f * (1.0f - pan));
    *gr = vol * (pan > 0.5f ? 1.0f : 2.0f * pan);
}

/* Next decoded block of a stream; 0 at the end of the file. */
static int stream_refill(Voice *v)
{
    v->dec_pos = 0;
    v->dec_frames = 0;
    if (v->is_mp3) {
        v->dec_frames = (uint32_t)drmp3_read_pcm_frames_s16(&v->mp3, 4096, v->dec);
    }
    return v->dec_frames > 0;
}

static void stream_rewind(Voice *v, uint32_t frame)
{
    if (v->is_mp3) {
        drmp3_seek_to_pcm_frame(&v->mp3, frame);
        v->dec_frames = v->dec_pos = 0;
        v->pos = frame;
    } else {
        v->pos = frame;
    }
}

static void mix_voice(Voice *v, float *out, int frames)
{
    float gl, gr;
    voice_gains(v, &gl, &gr);
    double step = (double)v->rate / out_rate;
    for (int i = 0; i < frames; i++) {
        float l = 0, r = 0;
        if (v->kind == V_STREAM && v->is_mp3) {
            if (v->dec_pos >= v->dec_frames) {
                if (!stream_refill(v)) {
                    if (v->loops != 1) {
                        if (v->loops > 1)
                            v->loops--;
                        stream_rewind(v, 0);
                        if (!stream_refill(v))
                            break;
                    } else {
                        v->status = SMP_DONE;
                        break;
                    }
                }
            }
            /* MP3 streams are resampled by whole frames (step accumulates in pos). */
            const int16_t *f = v->dec + (size_t)v->dec_pos * v->channels;
            l = f[0] * (1.0f / 32768.0f);
            r = v->channels == 2 ? f[1] * (1.0f / 32768.0f) : l;
            v->pos += step;
            while (v->pos >= 1.0) {
                v->pos -= 1.0;
                v->dec_pos++;
            }
        } else if (v->qbuf[0] || v->qbuf[1]) {
            /* double-buffered 2D sample */
            int b = v->qcur;
            if (!v->qfull[b]) {
                if (v->qended)
                    v->status = SMP_DONE;
                break;
            }
            uint32_t fi = (uint32_t)v->pos;
            const int16_t *f = v->qbuf[b] + (size_t)fi * v->channels;
            l = f[0] * (1.0f / 32768.0f);
            r = v->channels == 2 ? f[1] * (1.0f / 32768.0f) : l;
            v->pos += step;
            if ((uint32_t)v->pos >= v->qframes[b]) {
                v->pos = 0;
                v->qfull[b] = 0;
                v->qcur = b ^ 1;
            }
        } else {
            const uint8_t *d = v->dec ? (const uint8_t *)v->dec : g_mem + v->data;
            int bits = v->dec ? 16 : v->bits;
            uint32_t n = v->dec ? v->dec_frames : v->len / (uint32_t)(v->channels * bits / 8);
            uint32_t fi = (uint32_t)v->pos;
            if (fi >= n) {
                if (v->loops != 1 && n) {
                    if (v->loops > 1)
                        v->loops--;
                    v->pos -= n;
                    fi = (uint32_t)v->pos;
                    if (fi >= n)
                        fi = 0, v->pos = 0;
                } else {
                    v->status = SMP_DONE;
                    break;
                }
            }
            float l0, r0, l1, r1;
            pcm_frame(d, bits, v->channels, fi, &l0, &r0);
            if (fi + 1 < n) {
                pcm_frame(d, bits, v->channels, fi + 1, &l1, &r1);
                float t = (float)(v->pos - fi);
                l = l0 + (l1 - l0) * t;
                r = r0 + (r1 - r0) * t;
            } else {
                l = l0;
                r = r0;
            }
            v->pos += step;
        }
        out[i * 2] += l * gl;
        out[i * 2 + 1] += r * gr;
    }
}

static void SDLCALL mix(void *ud, Uint8 *stream, int bytes)
{
    (void)ud;
    float *out = (float *)stream;
    int frames = bytes / 8;
    memset(out, 0, (size_t)bytes);
    SDL_LockMutex(lock);
    for (int i = 0; i < MAX_VOICES; i++) {
        Voice *v = &voices[i];
        if (v->used && v->status == SMP_PLAYING && !v->paused)
            mix_voice(v, out, frames);
    }
    SDL_UnlockMutex(lock);
    for (int i = 0; i < frames * 2; i++)
        out[i] = out[i] > 1.0f ? 1.0f : out[i] < -1.0f ? -1.0f : out[i];
}

/* ---- startup and the digital driver ------------------------------------- */

SHIM(mss32, _AIL_startup_0)
{
    if (!lock)
        lock = SDL_CreateMutex();
    stdcall_ret(c, 1, 0);
}

SHIM(mss32, _AIL_shutdown_0)
{
    if (dev)
        SDL_CloseAudioDevice(dev);
    dev = 0;
    stdcall_ret(c, 0, 0);
}

SHIM(mss32, _AIL_set_redist_directory_4) { stdcall_ret(c, ARG(0), 1); }
SHIM(mss32, _AIL_set_preference_8) { stdcall_ret(c, 0, 2); }

SHIM(mss32, _AIL_last_error_0)
{
    static uint32_t msg;
    if (!msg)
        msg = scratch_strdup("no error");
    stdcall_ret(c, msg, 0);
}

/* AIL_open_digital_driver(frequency, bits, channels, flags) */
SHIM(mss32, _AIL_open_digital_driver_16)
{
    uint32_t freq = ARG(0);
    if (!lock)
        lock = SDL_CreateMutex();
    if (!dev) {
        if (!SDL_WasInit(SDL_INIT_AUDIO))
            SDL_InitSubSystem(SDL_INIT_AUDIO);
        SDL_AudioSpec want, have;
        memset(&want, 0, sizeof(want));
        want.freq = freq >= 8000u && freq <= 48000u ? (int)freq : 44100;
        want.format = AUDIO_F32SYS;
        want.channels = 2;
        want.samples = 1024;
        want.callback = mix;
        dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, SDL_AUDIO_ALLOW_FREQUENCY_CHANGE);
        if (!dev) {
            rt_log("SDL_OpenAudioDevice: %s", SDL_GetError());
            stdcall_ret(c, 0, 4);
            return;
        }
        out_rate = have.freq;
        SDL_PauseAudioDevice(dev, 0);
        driver_handle = scratch_alloc(16, 16);
    }
    rt_log("AIL_open_digital_driver(%u Hz, %u, %u): device at %d Hz", freq, ARG(1), ARG(2), out_rate);
    stdcall_ret(c, driver_handle, 4);
}

SHIM(mss32, _AIL_set_DirectSound_HWND_8) { stdcall_ret(c, 0, 2); }
SHIM(mss32, _AIL_digital_CPU_percent_4) { stdcall_ret(c, 0, 1); }
SHIM(mss32, _AIL_set_digital_master_room_type_8) { stdcall_ret(c, 0, 2); }

SHIM(mss32, _AIL_set_file_callbacks_16)
{
    for (int i = 0; i < 4; i++)
        file_cb[i] = ARG(i);
    stdcall_ret(c, 0, 4);
}

/* ---- 2D samples ------------------------------------------------------------ */

static void free_buffers(Voice *v)
{
    for (int b = 0; b < 2; b++) {
        free(v->qbuf[b]);
        v->qbuf[b] = NULL;
        v->qfull[b] = 0;
    }
    free(v->dec);
    v->dec = NULL;
    v->dec_frames = 0;
}

SHIM(mss32, _AIL_allocate_sample_handle_4)
{
    SDL_LockMutex(lock);
    Voice *v = voice_new(V_2D);
    SDL_UnlockMutex(lock);
    stdcall_ret(c, v ? v->handle : 0u, 1);
}

SHIM(mss32, _AIL_release_sample_handle_4)
{
    SDL_LockMutex(lock);
    Voice *v = voice_of(ARG(0));
    if (v) {
        free_buffers(v);
        v->used = 0;
    }
    SDL_UnlockMutex(lock);
    stdcall_ret(c, 0, 1);
}

SHIM(mss32, _AIL_init_sample_4)
{
    SDL_LockMutex(lock);
    Voice *v = voice_of(ARG(0));
    if (v) {
        free_buffers(v);
        uint32_t h = v->handle;
        int kind = v->kind;
        memset(v, 0, sizeof(*v));
        v->used = 1;
        v->kind = kind;
        v->handle = h;
        v->status = SMP_DONE;
        v->volume = 1.0f;
        v->pan = 0.5f;
        v->loops = 1;
        v->bits = 8;
        v->channels = 1;
        v->rate = 11025;
    }
    SDL_UnlockMutex(lock);
    stdcall_ret(c, 0, 1);
}

/* AIL_set_sample_type(s, format, flags) */
SHIM(mss32, _AIL_set_sample_type_12)
{
    SDL_LockMutex(lock);
    Voice *v = voice_of(ARG(0));
    if (v) {
        uint32_t f = ARG(1);
        v->bits = (f & DIG_F_16BITS_MASK) || (f & DIG_F_ADPCM_MASK) ? 16 : 8;
        v->channels = f & DIG_F_STEREO_MASK ? 2 : 1;
        v->adpcm = (f & DIG_F_ADPCM_MASK) != 0;
    }
    SDL_UnlockMutex(lock);
    stdcall_ret(c, 0, 3);
}

SHIM(mss32, _AIL_set_sample_adpcm_block_size_8)
{
    Voice *v = voice_of(ARG(0));
    if (v)
        v->block_size = ARG(1);
    stdcall_ret(c, 0, 2);
}

/* PCM to play in place; ADPCM is decoded now. */
static void set_source(Voice *v, uint32_t data, uint32_t len)
{
    free_buffers(v);
    v->data = data;
    v->len = len;
    v->pos = 0;
    if (v->adpcm && data && len) {
        v->dec = ima_decode(g_mem + data, len, v->block_size ? v->block_size : 512u * (uint32_t)v->channels,
                            v->channels, &v->dec_frames);
    }
}

SHIM(mss32, _AIL_set_sample_address_12)
{
    SDL_LockMutex(lock);
    Voice *v = voice_of(ARG(0));
    if (v)
        set_source(v, ARG(1), ARG(2));
    SDL_UnlockMutex(lock);
    stdcall_ret(c, 0, 3);
}

SHIM(mss32, _AIL_set_sample_playback_rate_8)
{
    Voice *v = voice_of(ARG(0));
    if (v)
        v->rate = ARG(1);
    stdcall_ret(c, 0, 2);
}

SHIM(mss32, _AIL_sample_playback_rate_4)
{
    Voice *v = voice_of(ARG(0));
    stdcall_ret(c, v ? v->rate : 0u, 1);
}

static float argfl(Ctx *c, int n)
{
    uint32_t b = ARG(n);
    float f;
    memcpy(&f, &b, 4);
    return f;
}

static void put_float(uint32_t p, float f)
{
    if (p) {
        uint32_t b;
        memcpy(&b, &f, 4);
        WR32(p, b);
    }
}

SHIM(mss32, _AIL_set_sample_volume_pan_12)
{
    Voice *v = voice_of(ARG(0));
    if (v) {
        v->volume = argfl(c, 1);
        v->pan = argfl(c, 2);
    }
    stdcall_ret(c, 0, 3);
}

SHIM(mss32, _AIL_sample_volume_pan_12)
{
    Voice *v = voice_of(ARG(0));
    put_float(ARG(1), v ? v->volume : 0.0f);
    put_float(ARG(2), v ? v->pan : 0.5f);
    stdcall_ret(c, 0, 3);
}

SHIM(mss32, _AIL_set_sample_loop_count_8)
{
    Voice *v = voice_of(ARG(0));
    if (v)
        v->loops = (int)ARG(1);
    stdcall_ret(c, 0, 2);
}

SHIM(mss32, _AIL_set_sample_reverb_levels_12) { stdcall_ret(c, 0, 3); }

static void start(Voice *v)
{
    v->pos = 0;
    v->qcur = 0;
    v->paused = 0;
    v->status = SMP_PLAYING;
}

SHIM(mss32, _AIL_start_sample_4)
{
    SDL_LockMutex(lock);
    Voice *v = voice_of(ARG(0));
    if (v)
        start(v);
    SDL_UnlockMutex(lock);
    stdcall_ret(c, 0, 1);
}

static void set_status(uint32_t h, uint32_t from_mask, uint32_t to)
{
    SDL_LockMutex(lock);
    Voice *v = voice_of(h);
    if (v && (v->status & from_mask))
        v->status = to;
    SDL_UnlockMutex(lock);
}

SHIM(mss32, _AIL_stop_sample_4) { set_status(ARG(0), SMP_PLAYING, SMP_STOPPED); stdcall_ret(c, 0, 1); }
SHIM(mss32, _AIL_resume_sample_4) { set_status(ARG(0), SMP_STOPPED, SMP_PLAYING); stdcall_ret(c, 0, 1); }
SHIM(mss32, _AIL_end_sample_4) { set_status(ARG(0), SMP_PLAYING | SMP_STOPPED, SMP_DONE); stdcall_ret(c, 0, 1); }

SHIM(mss32, _AIL_sample_status_4)
{
    Voice *v = voice_of(ARG(0));
    stdcall_ret(c, v ? v->status : SMP_FREE, 1);
}

static uint32_t frame_bytes(const Voice *v) { return (uint32_t)(v->channels * v->bits / 8); }

static uint32_t voice_frames(const Voice *v)
{
    if (v->dec)
        return v->dec_frames;
    return frame_bytes(v) ? v->len / frame_bytes(v) : 0u;
}

SHIM(mss32, _AIL_sample_position_4)
{
    Voice *v = voice_of(ARG(0));
    stdcall_ret(c, v ? (uint32_t)v->pos * frame_bytes(v) : 0u, 1);
}

/* AIL_sample_ms_position(s, S32 *total_ms, S32 *current_ms) */
SHIM(mss32, _AIL_sample_ms_position_12)
{
    Voice *v = voice_of(ARG(0));
    uint32_t total = 0, cur = 0;
    if (v && v->rate) {
        total = (uint32_t)((uint64_t)voice_frames(v) * 1000u / v->rate);
        cur = (uint32_t)(v->pos * 1000.0 / v->rate);
    }
    if (ARG(1))
        WR32(ARG(1), total);
    if (ARG(2))
        WR32(ARG(2), cur);
    stdcall_ret(c, 0, 3);
}

SHIM(mss32, _AIL_set_sample_ms_position_8)
{
    SDL_LockMutex(lock);
    Voice *v = voice_of(ARG(0));
    if (v)
        v->pos = (double)ARG(1) * v->rate / 1000.0;
    SDL_UnlockMutex(lock);
    stdcall_ret(c, 0, 2);
}

/* Double buffering: AIL_minimum_sample_buffer_size(dig, rate, format) */
SHIM(mss32, _AIL_minimum_sample_buffer_size_12)
{
    uint32_t rate = ARG(1), fmt = ARG(2);
    uint32_t bpf = ((fmt & DIG_F_16BITS_MASK) ? 2u : 1u) * ((fmt & DIG_F_STEREO_MASK) ? 2u : 1u);
    uint32_t n = rate / 8u * bpf;   /* 125 ms */
    stdcall_ret(c, (n + 1023u) & ~1023u, 3);
}

/* Which buffer wants data (0 or 1), -1 when both are queued. */
SHIM(mss32, _AIL_sample_buffer_ready_4)
{
    SDL_LockMutex(lock);
    Voice *v = voice_of(ARG(0));
    int32_t r = -1;
    if (v) {
        int next = v->qfull[v->qcur] ? v->qcur ^ 1 : v->qcur;
        if (!v->qfull[next])
            r = next;
    }
    SDL_UnlockMutex(lock);
    stdcall_ret(c, (uint32_t)r, 1);
}

/* AIL_load_sample_buffer(s, buffer_num, buffer, len): a copy is queued (len 0 ends). */
SHIM(mss32, _AIL_load_sample_buffer_16)
{
    uint32_t num = ARG(1) & 1u, buf = ARG(2), len = ARG(3);
    SDL_LockMutex(lock);
    Voice *v = voice_of(ARG(0));
    if (v) {
        if (!len) {
            v->qended = 1;
        } else {
            uint32_t frames = len / frame_bytes(v);
            int16_t *p = realloc(v->qbuf[num], (size_t)frames * v->channels * 2u);
            if (p) {
                v->qbuf[num] = p;
                for (uint32_t i = 0; i < frames * (uint32_t)v->channels; i++)
                    p[i] = v->bits == 16 ? (int16_t)RD16(buf + i * 2u) : (int16_t)((RD8(buf + i) - 128) << 8);
                v->qframes[num] = frames;
                v->qfull[num] = 1;
                v->qended = 0;
                if (v->status != SMP_PLAYING && v->status != SMP_STOPPED)
                    v->status = SMP_PLAYING;
            }
        }
    }
    SDL_UnlockMutex(lock);
    stdcall_ret(c, 0, 4);
}

/* ---- WAV info --------------------------------------------------------------- */

/* AILSOUNDINFO: format, data_ptr, data_len, rate, bits, channels, samples, block_size, initial_ptr */
static int fill_info(uint32_t data, uint32_t len, uint32_t info)
{
    WavInfo w;
    if (!wav_parse(g_mem + data, len, &w))
        return 0;
    WR32(info + 0u, w.format);
    WR32(info + 4u, data + w.data_off);
    WR32(info + 8u, w.data_len);
    WR32(info + 12u, w.rate);
    WR32(info + 16u, w.bits);
    WR32(info + 20u, w.channels);
    uint32_t samples = w.format == WAVE_FORMAT_PCM && w.bits && w.channels
                       ? w.data_len / (w.bits / 8u * w.channels) : 0u;
    WR32(info + 24u, samples);
    WR32(info + 28u, w.block_size);
    WR32(info + 32u, data + w.data_off);
    return 1;
}

/* AIL_WAV_info(data, info): the file size is not given; bounded by the guest space. */
SHIM(mss32, _AIL_WAV_info_8)
{
    uint32_t data = ARG(0);
    uint32_t riff = RD32(data + 4u) + 8u;
    stdcall_ret(c, (uint32_t)fill_info(data, riff, ARG(1)), 2);
}

/* ---- 3D --------------------------------------------------------------------- */

#define PROVIDER_HANDLE 0x00000301u
static uint32_t provider_name;

/* AIL_enumerate_3D_providers(HPROENUM *next, HPROVIDER *dest, char **name) */
SHIM(mss32, _AIL_enumerate_3D_providers_12)
{
    uint32_t next = ARG(0);
    if (!provider_name)
        provider_name = scratch_strdup("Miles Fast 2D Positional Audio");
    if (RD32(next) != 0) {
        stdcall_ret(c, 0, 3);
        return;
    }
    WR32(next, 1);
    WR32(ARG(1), PROVIDER_HANDLE);
    WR32(ARG(2), provider_name);
    stdcall_ret(c, 1, 3);
}

SHIM(mss32, _AIL_open_3D_provider_4) { stdcall_ret(c, 0, 1); }   /* M3D_NOERR */
SHIM(mss32, _AIL_close_3D_provider_4) { stdcall_ret(c, 0, 1); }
SHIM(mss32, _AIL_set_3D_provider_preference_12) { stdcall_ret(c, 0, 3); }

/* AIL_3D_provider_attribute(lib, name, void *value) */
SHIM(mss32, _AIL_3D_provider_attribute_12)
{
    const char *name = gstr(ARG(1));
    if (ARG(2))
        WR32(ARG(2), name && strstr(name, "samples") ? 64u : 0u);
    stdcall_ret(c, 0, 3);
}

SHIM(mss32, _AIL_set_3D_room_type_8) { stdcall_ret(c, 0, 2); }

SHIM(mss32, _AIL_set_3D_distance_factor_8)
{
    float f = argfl(c, 1);
    listener_scale = f > 0.0f ? 1.0f : 1.0f;   /* distances are in the game's units already */
    stdcall_ret(c, 0, 2);
}

SHIM(mss32, _AIL_allocate_3D_sample_handle_4)
{
    SDL_LockMutex(lock);
    Voice *v = voice_new(V_3D);
    SDL_UnlockMutex(lock);
    stdcall_ret(c, v ? v->handle : 0u, 1);
}

/* AIL_set_3D_sample_info(s, AILSOUNDINFO *info) */
SHIM(mss32, _AIL_set_3D_sample_info_8)
{
    uint32_t info = ARG(1);
    SDL_LockMutex(lock);
    Voice *v = voice_of(ARG(0));
    uint32_t ok = 0;
    if (v) {
        uint32_t fmt = RD32(info);
        v->bits = (int)RD32(info + 16u);
        v->channels = (int)RD32(info + 20u);
        v->rate = RD32(info + 12u);
        v->block_size = RD32(info + 28u);
        v->adpcm = fmt == WAVE_FORMAT_IMA_ADPCM;
        if (v->adpcm)
            v->bits = 16;
        if (v->channels < 1 || v->channels > 2)
            v->channels = 1;
        set_source(v, RD32(info + 4u), RD32(info + 8u));
        v->status = SMP_DONE;
        ok = 1;
    }
    SDL_UnlockMutex(lock);
    stdcall_ret(c, ok, 2);
}

SHIM(mss32, _AIL_set_3D_sample_offset_8)
{
    SDL_LockMutex(lock);
    Voice *v = voice_of(ARG(0));
    if (v && frame_bytes(v))
        v->pos = ARG(1) / frame_bytes(v);
    SDL_UnlockMutex(lock);
    stdcall_ret(c, 0, 2);
}

SHIM(mss32, _AIL_3D_sample_offset_4)
{
    Voice *v = voice_of(ARG(0));
    stdcall_ret(c, v ? (uint32_t)v->pos * frame_bytes(v) : 0u, 1);
}

SHIM(mss32, _AIL_3D_sample_length_4)
{
    Voice *v = voice_of(ARG(0));
    stdcall_ret(c, v ? voice_frames(v) * frame_bytes(v) : 0u, 1);
}

SHIM(mss32, _AIL_set_3D_sample_loop_count_8)
{
    Voice *v = voice_of(ARG(0));
    if (v)
        v->loops = (int)ARG(1);
    stdcall_ret(c, 0, 2);
}

SHIM(mss32, _AIL_set_3D_sample_playback_rate_8)
{
    Voice *v = voice_of(ARG(0));
    if (v)
        v->rate = ARG(1);
    stdcall_ret(c, 0, 2);
}

SHIM(mss32, _AIL_3D_sample_playback_rate_4)
{
    Voice *v = voice_of(ARG(0));
    stdcall_ret(c, v ? v->rate : 0u, 1);
}

SHIM(mss32, _AIL_set_3D_sample_volume_8)
{
    Voice *v = voice_of(ARG(0));
    if (v)
        v->volume = argfl(c, 1);
    stdcall_ret(c, 0, 2);
}

/* Returns an F32 on the x87 stack (IMPORT_X87 in tools/recomp/gen.py). */
SHIM(mss32, _AIL_3D_sample_volume_4)
{
    Voice *v = voice_of(ARG(0));
    fpush(c, v ? v->volume : 0.0);
    stdcall_ret(c, c->r[0], 1);
}

/* AIL_set_3D_sample_distances(s, max_dist, min_dist) */
SHIM(mss32, _AIL_set_3D_sample_distances_12)
{
    Voice *v = voice_of(ARG(0));
    if (v) {
        v->max_dist = argfl(c, 1);
        v->min_dist = argfl(c, 2);
    }
    stdcall_ret(c, 0, 3);
}

SHIM(mss32, _AIL_set_3D_sample_effects_level_8) { stdcall_ret(c, 0, 2); }
SHIM(mss32, _AIL_set_3D_sample_preference_12) { stdcall_ret(c, 0, 3); }

SHIM(mss32, _AIL_set_3D_position_16)
{
    Voice *v = voice_of(ARG(0));
    if (v) {
        v->x = argfl(c, 1);
        v->y = argfl(c, 2);
        v->z = argfl(c, 3);
    }
    stdcall_ret(c, 0, 4);
}

SHIM(mss32, _AIL_3D_position_16)
{
    Voice *v = voice_of(ARG(0));
    put_float(ARG(1), v ? v->x : 0.0f);
    put_float(ARG(2), v ? v->y : 0.0f);
    put_float(ARG(3), v ? v->z : 0.0f);
    stdcall_ret(c, 0, 4);
}

SHIM(mss32, _AIL_start_3D_sample_4)
{
    SDL_LockMutex(lock);
    Voice *v = voice_of(ARG(0));
    if (v) {
        double keep = v->pos;   /* AIL_set_3D_sample_offset may have chosen the start */
        start(v);
        v->pos = keep;
    }
    SDL_UnlockMutex(lock);
    stdcall_ret(c, 0, 1);
}

SHIM(mss32, _AIL_stop_3D_sample_4) { set_status(ARG(0), SMP_PLAYING, SMP_STOPPED); stdcall_ret(c, 0, 1); }
SHIM(mss32, _AIL_resume_3D_sample_4) { set_status(ARG(0), SMP_STOPPED, SMP_PLAYING); stdcall_ret(c, 0, 1); }

SHIM(mss32, _AIL_end_3D_sample_4)
{
    SDL_LockMutex(lock);
    Voice *v = voice_of(ARG(0));
    if (v) {
        v->status = SMP_DONE;
        v->pos = 0;
    }
    SDL_UnlockMutex(lock);
    stdcall_ret(c, 0, 1);
}

SHIM(mss32, _AIL_3D_sample_status_4)
{
    Voice *v = voice_of(ARG(0));
    stdcall_ret(c, v ? v->status : SMP_FREE, 1);
}

/* ---- streams ------------------------------------------------------------- */

/* Reads a whole file through the game's file callbacks (on the game thread). */
static uint8_t *read_through_callbacks(Ctx *c, const char *name, uint32_t *len_out)
{
    if (!file_cb[0])
        return NULL;
    uint32_t namebuf = scratch_strdup(name);   /* small, kept: stream opens are rare */
    uint32_t hslot = heap_alloc(4);
    uint32_t args[3] = { namebuf, hslot, 0 };
    uint8_t *data = NULL;
    if (guest_call(c, file_cb[0], 2, args)) {
        uint32_t fh = RD32(hslot);
        uint32_t sargs[3] = { fh, 0, 2 };     /* seek to the end: the size */
        uint32_t size = guest_call(c, file_cb[2], 3, sargs);
        sargs[2] = 0;
        guest_call(c, file_cb[2], 3, sargs);
        uint32_t gbuf_ = heap_alloc(size ? size : 1u);
        uint32_t rargs[3] = { fh, gbuf_, size };
        uint32_t got = size ? guest_call(c, file_cb[3], 3, rargs) : 0u;
        guest_call(c, file_cb[1], 1, &fh);
        if (got == size && (data = malloc(size ? size : 1u)) != NULL) {
            memcpy(data, g_mem + gbuf_, size);
            *len_out = size;
        }
        heap_free(gbuf_);
    }
    heap_free(hslot);
    return data;
}

/* AIL_open_stream(dig, filename, stream_mem) */
SHIM(mss32, _AIL_open_stream_12)
{
    const char *name = gstr(ARG(1));
    uint32_t len = 0;
    uint8_t *file = read_through_callbacks(c, name, &len);
    if (!file) {
        rt_log("AIL_open_stream(%s): cannot read", name);
        stdcall_ret(c, 0, 3);
        return;
    }
    SDL_LockMutex(lock);
    Voice *v = voice_new(V_STREAM);
    if (v) {
        v->file = file;
        v->file_len = len;
        WavInfo w;
        if (wav_parse(file, len, &w)) {
            v->bits = (int)w.bits;
            v->channels = (int)w.channels;
            v->rate = w.rate;
            if (w.format == WAVE_FORMAT_IMA_ADPCM) {
                v->dec = ima_decode(file + w.data_off, w.data_len, w.block_size, v->channels, &v->dec_frames);
            } else {
                /* PCM: keep a 16-bit host copy */
                uint32_t frames = w.data_len / (w.bits / 8u * w.channels);
                v->dec = malloc((size_t)frames * w.channels * 2u);
                if (v->dec) {
                    for (uint32_t i = 0; i < frames * w.channels; i++)
                        v->dec[i] = w.bits == 16 ? (int16_t)(file[w.data_off + i * 2] | file[w.data_off + i * 2 + 1] << 8)
                                                 : (int16_t)((file[w.data_off + i] - 128) << 8);
                    v->dec_frames = frames;
                }
            }
            v->total_frames = v->dec_frames;
        } else if (drmp3_init_memory(&v->mp3, file, len, NULL)) {
            v->is_mp3 = 1;
            v->channels = (int)v->mp3.channels;
            v->rate = v->mp3.sampleRate;
            v->dec = malloc(4096u * 2u * sizeof(int16_t));
            v->total_frames = (uint32_t)drmp3_get_pcm_frame_count(&v->mp3);
            drmp3_seek_to_pcm_frame(&v->mp3, 0);
        } else {
            rt_log("AIL_open_stream(%s): unknown format", name);
            free(file);
            v->used = 0;
            v = NULL;
        }
    }
    SDL_UnlockMutex(lock);
    if (v)
        rt_log("AIL_open_stream(%s): %s %u Hz %d ch, %u frames", name, v->is_mp3 ? "mp3" : "wav", v->rate,
               v->channels, v->total_frames);
    stdcall_ret(c, v ? v->handle : 0u, 3);
}

SHIM(mss32, _AIL_close_stream_4)
{
    SDL_LockMutex(lock);
    Voice *v = voice_of(ARG(0));
    if (v) {
        if (v->is_mp3)
            drmp3_uninit(&v->mp3);
        free(v->file);
        free(v->dec);
        v->file = NULL;
        v->dec = NULL;
        v->used = 0;
    }
    SDL_UnlockMutex(lock);
    stdcall_ret(c, 0, 1);
}

SHIM(mss32, _AIL_start_stream_4)
{
    SDL_LockMutex(lock);
    Voice *v = voice_of(ARG(0));
    if (v) {
        stream_rewind(v, 0);
        start(v);
    }
    SDL_UnlockMutex(lock);
    stdcall_ret(c, 0, 1);
}

SHIM(mss32, _AIL_pause_stream_8)
{
    SDL_LockMutex(lock);
    Voice *v = voice_of(ARG(0));
    if (v)
        v->paused = ARG(1) != 0;
    SDL_UnlockMutex(lock);
    stdcall_ret(c, 0, 2);
}

SHIM(mss32, _AIL_stream_status_4)
{
    Voice *v = voice_of(ARG(0));
    uint32_t s = v ? v->status : SMP_FREE;
    if (v && v->paused && s == SMP_PLAYING)
        s = SMP_STOPPED;
    stdcall_ret(c, s, 1);
}

SHIM(mss32, _AIL_set_stream_loop_count_8)
{
    Voice *v = voice_of(ARG(0));
    if (v)
        v->loops = (int)ARG(1);
    stdcall_ret(c, 0, 2);
}

static uint32_t stream_frame(const Voice *v)
{
    if (v->is_mp3)
        return (uint32_t)v->mp3.currentPCMFrame - (v->dec_frames - v->dec_pos);
    return (uint32_t)v->pos;
}

SHIM(mss32, _AIL_set_stream_ms_position_8)
{
    SDL_LockMutex(lock);
    Voice *v = voice_of(ARG(0));
    if (v && v->rate)
        stream_rewind(v, (uint32_t)((uint64_t)ARG(1) * v->rate / 1000u));
    SDL_UnlockMutex(lock);
    stdcall_ret(c, 0, 2);
}

/* AIL_stream_ms_position(s, S32 *total_ms, S32 *current_ms) */
SHIM(mss32, _AIL_stream_ms_position_12)
{
    SDL_LockMutex(lock);
    Voice *v = voice_of(ARG(0));
    uint32_t total = 0, cur = 0;
    if (v && v->rate) {
        total = (uint32_t)((uint64_t)v->total_frames * 1000u / v->rate);
        cur = (uint32_t)((uint64_t)stream_frame(v) * 1000u / v->rate);
    }
    SDL_UnlockMutex(lock);
    if (ARG(1))
        WR32(ARG(1), total);
    if (ARG(2))
        WR32(ARG(2), cur);
    stdcall_ret(c, 0, 3);
}

SHIM(mss32, _AIL_set_stream_playback_rate_8)
{
    Voice *v = voice_of(ARG(0));
    if (v)
        v->rate = ARG(1);
    stdcall_ret(c, 0, 2);
}

SHIM(mss32, _AIL_stream_playback_rate_4)
{
    Voice *v = voice_of(ARG(0));
    stdcall_ret(c, v ? v->rate : 0u, 1);
}

SHIM(mss32, _AIL_set_stream_volume_pan_12)
{
    Voice *v = voice_of(ARG(0));
    if (v) {
        v->volume = argfl(c, 1);
        v->pan = argfl(c, 2);
    }
    stdcall_ret(c, 0, 3);
}

SHIM(mss32, _AIL_stream_volume_pan_12)
{
    Voice *v = voice_of(ARG(0));
    put_float(ARG(1), v ? v->volume : 0.0f);
    put_float(ARG(2), v ? v->pan : 0.5f);
    stdcall_ret(c, 0, 3);
}

SHIM(mss32, _AIL_set_stream_reverb_levels_12) { stdcall_ret(c, 0, 3); }

/* ---- format conversion -----------------------------------------------------
 * AILMIXINFO (MSS 6.5): AILSOUNDINFO Info (36 bytes), ADPCMDATA (72 bytes),
 * U32 src_fract, S32 left_val, S32 right_val. The game converts its sounds to
 * the output format with these: each source resampled (linear) to dest_rate and
 * dest_format, several sources summed. */
#define MIXINFO_SIZE 120u

typedef struct SrcPcm {
    int16_t *pcm;        /* 16-bit frames (owned) */
    uint32_t frames;
    int channels;
    uint32_t rate;
} SrcPcm;

static int load_src(uint32_t mi, SrcPcm *o)
{
    uint32_t fmt = RD32(mi), data = RD32(mi + 4u), len = RD32(mi + 8u), rate = RD32(mi + 12u);
    int bits = (int)RD32(mi + 16u), ch = (int)RD32(mi + 20u);
    uint32_t block = RD32(mi + 28u);
    memset(o, 0, sizeof(*o));
    if (ch < 1 || ch > 2 || !rate || !data)
        return 0;
    o->channels = ch;
    o->rate = rate;
    if (fmt == WAVE_FORMAT_IMA_ADPCM) {
        o->pcm = ima_decode(g_mem + data, len, block ? block : 512u * (uint32_t)ch, ch, &o->frames);
        return o->pcm != NULL;
    }
    uint32_t bpf = (uint32_t)(bits / 8 * ch);
    if (!bpf)
        return 0;
    o->frames = len / bpf;
    o->pcm = malloc((size_t)o->frames * ch * 2u + 2u);
    if (!o->pcm)
        return 0;
    for (uint32_t i = 0; i < o->frames * (uint32_t)ch; i++)
        o->pcm[i] = bits == 16 ? (int16_t)RD16(data + i * 2u) : (int16_t)((RD8(data + i) - 128) << 8);
    return 1;
}

static uint32_t out_frames(const SrcPcm *s, uint32_t dest_rate)
{
    return (uint32_t)(((uint64_t)s->frames * dest_rate + s->rate - 1u) / s->rate);
}

/* AIL_size_processed_digital_audio(dest_rate, dest_format, num_srcs, AILMIXINFO const *src) */
SHIM(mss32, _AIL_size_processed_digital_audio_16)
{
    uint32_t rate = ARG(0), fmt = ARG(1), n = ARG(2), src = ARG(3);
    uint32_t bpf = ((fmt & DIG_F_16BITS_MASK) ? 2u : 1u) * ((fmt & DIG_F_STEREO_MASK) ? 2u : 1u);
    uint32_t most = 0;
    for (uint32_t k = 0; k < n; k++) {
        uint32_t mi = src + k * MIXINFO_SIZE;
        uint32_t fmt_s = RD32(mi), len = RD32(mi + 8u), srate = RD32(mi + 12u);
        int bits = (int)RD32(mi + 16u), ch = (int)RD32(mi + 20u);
        uint32_t frames;
        if (fmt_s == WAVE_FORMAT_IMA_ADPCM) {
            uint32_t block = RD32(mi + 28u) ? RD32(mi + 28u) : 512u * (uint32_t)ch;
            uint32_t per = block >= 4u * (uint32_t)ch ? (block - 4u * (uint32_t)ch) * 2u / (uint32_t)ch + 1u : 1u;
            frames = (len + block - 1u) / block * per;
        } else {
            frames = bits && ch ? len / (uint32_t)(bits / 8 * ch) : 0u;
        }
        uint32_t f = srate ? (uint32_t)(((uint64_t)frames * rate + srate - 1u) / srate) : 0u;
        if (f > most)
            most = f;
    }
    stdcall_ret(c, most * bpf, 4);
}

/* AIL_process_digital_audio(dest, dest_size, dest_rate, dest_format, num_srcs, AILMIXINFO *src) */
SHIM(mss32, _AIL_process_digital_audio_24)
{
    uint32_t dest = ARG(0), size = ARG(1), rate = ARG(2), fmt = ARG(3), n = ARG(4), src = ARG(5);
    int dch = (fmt & DIG_F_STEREO_MASK) ? 2 : 1, d16 = (fmt & DIG_F_16BITS_MASK) != 0;
    uint32_t bpf = (uint32_t)(dch * (d16 ? 2 : 1));
    uint32_t cap = bpf ? size / bpf : 0u, written = 0;
    float *acc = calloc((size_t)cap * dch + 1u, sizeof(float));
    for (uint32_t k = 0; acc && k < n; k++) {
        SrcPcm s;
        if (!load_src(src + k * MIXINFO_SIZE, &s))
            continue;
        uint32_t of = out_frames(&s, rate);
        if (of > cap)
            of = cap;
        double step = (double)s.rate / rate;
        for (uint32_t i = 0; i < of; i++) {
            double p = i * step;
            uint32_t j = (uint32_t)p;
            float t = (float)(p - j);
            uint32_t j1 = j + 1 < s.frames ? j + 1 : j;
            for (int ch = 0; ch < dch; ch++) {
                int sc = ch < s.channels ? ch : 0;
                float a = s.pcm[(size_t)j * s.channels + sc], b = s.pcm[(size_t)j1 * s.channels + sc];
                float v = a + (b - a) * t;
                if (dch == 1 && s.channels == 2)
                    v = 0.5f * (v + s.pcm[(size_t)j * 2 + 1]);
                acc[(size_t)i * dch + ch] += v;
            }
        }
        if (of > written)
            written = of;
        free(s.pcm);
    }
    for (uint32_t i = 0; acc && i < written * (uint32_t)dch; i++) {
        float v = acc[i];
        int32_t x = (int32_t)(v > 32767.0f ? 32767.0f : v < -32768.0f ? -32768.0f : v);
        if (d16)
            WR16(dest + i * 2u, (uint32_t)(uint16_t)(int16_t)x);
        else
            WR8(dest + i, (uint32_t)((x >> 8) + 128));
    }
    free(acc);
    stdcall_ret(c, written * bpf, 6);
}
