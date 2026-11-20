// Scripts' own sounds (the mod API's sound_* - Legends Lua's sound.load / sound.play): a mod's .wav / .ogg read once
// (stb_vorbis for Ogg Vorbis; WAV PCM 8 / 16 / 24 / 32-bit or float, mono or stereo), made 48 kHz stereo float, and
// mixed into the game's own audio output (services/audio.c, the first audio renderer: after the game's mix, before the
// output's volume - the Volume setting and background muting apply to them too). 2D, or positional: farther is
// quieter (silent at its range), and it is panned by where it is from the scene camera (the speech bubbles' camera,
// hle/bubble.c). LARECOMP_AUDIO_WAV records them with the rest - how they're checked without sound.
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../hle.h"

#define STB_VORBIS_NO_PUSHDATA_API
#include "../../third_party/stb/stb_vorbis.c"

#define RATE 48000
#define NSND 128
#define NVOICE 64
#define BYTES_MAX (96ull << 20)

typedef struct {
    char path[512];
    float *pcm;  // (frames x 2)
    uint32_t frames;
    int failed;
} Sound;
typedef struct {
    int id, sound;       // (id 0: free)
    double pos;          // (in the sound's frames)
    float volume, pitch, range;
    int positional, loop;
    float at[3];
    float gl, gr;        // (the gains last used: ramped toward the new ones each block)
} Voice;
static Sound g_snd[NSND];
static int g_nsnd;
static uint64_t g_bytes;
static Voice g_v[NVOICE];
static int g_next_voice = 1;
static _Atomic int g_playing, g_positional;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

// ---- reading
static float *to_stereo48(const float *in, uint32_t frames, int ch, int rate, uint32_t *out_frames) {
    if (rate <= 0 || ch <= 0 || !frames) return NULL;
    double step = (double)rate / RATE;
    uint32_t n = (uint32_t)((double)frames / step);
    if (!n) return NULL;
    float *o = malloc((size_t)n * 2 * sizeof(float));
    if (!o) return NULL;
    for (uint32_t i = 0; i < n; i++) {
        double p = i * step;
        uint32_t k = (uint32_t)p;
        float f = (float)(p - k);
        uint32_t k2 = k + 1 < frames ? k + 1 : k;
        float l = in[(size_t)k * ch] * (1 - f) + in[(size_t)k2 * ch] * f;
        float r = ch > 1 ? in[(size_t)k * ch + 1] * (1 - f) + in[(size_t)k2 * ch + 1] * f : l;
        o[i * 2] = l, o[i * 2 + 1] = r;
    }
    *out_frames = n;
    return o;
}
static float *read_wav(const char *path, uint32_t *frames) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size < 44 || size > (64l << 20)) { fclose(f); return NULL; }
    uint8_t *b = malloc((size_t)size);
    if (!b || fread(b, 1, (size_t)size, f) != (size_t)size) { fclose(f); free(b); return NULL; }
    fclose(f);
    float *out = NULL;
    if (memcmp(b, "RIFF", 4) || memcmp(b + 8, "WAVE", 4)) goto done;
    int fmt = 0, ch = 0, rate = 0, bits = 0;
    for (long p = 12; p + 8 <= size;) {
        uint32_t len;
        memcpy(&len, b + p + 4, 4);
        if (!memcmp(b + p, "fmt ", 4) && len >= 16) {
            uint16_t v16;
            uint32_t v32;
            memcpy(&v16, b + p + 8, 2), fmt = v16;
            memcpy(&v16, b + p + 10, 2), ch = v16;
            memcpy(&v32, b + p + 12, 4), rate = (int)v32;
            memcpy(&v16, b + p + 22, 2), bits = v16;
            if (fmt == 0xFFFE && len >= 40) memcpy(&v16, b + p + 32, 2), fmt = v16;  // (WAVE_FORMAT_EXTENSIBLE: its sub-format)
        } else if (!memcmp(b + p, "data", 4) && ch > 0 && bits > 0) {
            if ((long)len > size - p - 8) len = (uint32_t)(size - p - 8);
            int bps = bits / 8;
            uint32_t n = len / (uint32_t)(bps * ch);
            float *tmp = malloc((size_t)n * ch * sizeof(float));
            if (!tmp) goto done;
            const uint8_t *d = b + p + 8;
            for (uint32_t i = 0; i < n * (uint32_t)ch; i++) {
                const uint8_t *s = d + (size_t)i * bps;
                float v = 0;
                if (fmt == 3 && bits == 32) memcpy(&v, s, 4);
                else if (bits == 8) v = (s[0] - 128) / 128.0f;
                else if (bits == 16) v = (int16_t)(s[0] | s[1] << 8) / 32768.0f;
                else if (bits == 24) v = (float)((int32_t)((uint32_t)s[0] << 8 | (uint32_t)s[1] << 16 | (uint32_t)s[2] << 24) >> 8) / 8388608.0f;
                else if (bits == 32) { int32_t x; memcpy(&x, s, 4); v = (float)x / 2147483648.0f; }
                tmp[i] = v;
            }
            out = to_stereo48(tmp, n, ch, rate, frames);
            free(tmp);
            break;
        }
        p += 8 + len + (len & 1);
    }
done:
    free(b);
    return out;
}
static float *read_ogg(const char *path, uint32_t *frames) {
    int ch = 0, rate = 0;
    short *s = NULL;
    int n = stb_vorbis_decode_filename(path, &ch, &rate, &s);
    if (n <= 0 || !s || ch <= 0) { free(s); return NULL; }
    float *tmp = malloc((size_t)n * ch * sizeof(float));
    float *out = NULL;
    if (tmp) {
        for (size_t i = 0; i < (size_t)n * ch; i++) tmp[i] = s[i] / 32768.0f;
        out = to_stereo48(tmp, (uint32_t)n, ch, rate, frames);
    }
    free(tmp), free(s);
    return out;
}

// a sound by path: its id (> 0; the same file again: the same id), 0 not read
int modsound_load(const char *path) {
    if (!path || !*path) return 0;
    pthread_mutex_lock(&g_lock);
    for (int i = 0; i < g_nsnd; i++)
        if (!strcmp(g_snd[i].path, path)) { int id = g_snd[i].failed ? 0 : i + 1; pthread_mutex_unlock(&g_lock); return id; }
    pthread_mutex_unlock(&g_lock);
    uint32_t frames = 0;
    const char *ext = strrchr(path, '.');
    float *pcm = ext && (!strcmp(ext, ".ogg") || !strcmp(ext, ".OGG")) ? read_ogg(path, &frames) : read_wav(path, &frames);
    pthread_mutex_lock(&g_lock);
    int id = 0;
    if (g_nsnd < NSND) {
        Sound *s = &g_snd[g_nsnd++];
        snprintf(s->path, sizeof s->path, "%s", path);
        if (!pcm || g_bytes + (uint64_t)frames * 8 > BYTES_MAX) {
            free(pcm);
            s->failed = 1;
        } else {
            s->pcm = pcm, s->frames = frames, g_bytes += (uint64_t)frames * 8;
            id = g_nsnd;
        }
    } else free(pcm);
    pthread_mutex_unlock(&g_lock);
    if (id) LOG("[sound] %s: %.2f s (id %d)\n", path, frames / (double)RATE, id);
    else LOG("[sound] %s: not read (a .wav or .ogg of the mod's)\n", path);
    return id;
}

// plays a sound: volume 0..4, pitch 0.25..4 (its speed), pos (NULL: everywhere - 2D) with range (metres: silent past
// it), loop. Its voice (> 0; stop / set it with that), 0 none free.
int modsound_play(int id, float volume, float pitch, const float pos[3], float range, int loop) {
    pthread_mutex_lock(&g_lock);
    int v = 0;
    if (id > 0 && id <= g_nsnd && !g_snd[id - 1].failed) {
        Voice *f = NULL;
        for (int i = 0; i < NVOICE && !f; i++) if (!g_v[i].id) f = &g_v[i];
        if (f) {
            memset(f, 0, sizeof *f);
            f->id = v = g_next_voice++;
            if (g_next_voice <= 0) g_next_voice = 1;
            f->sound = id, f->volume = fminf(fmaxf(volume, 0), 4), f->pitch = fminf(fmaxf(pitch > 0 ? pitch : 1, 0.25f), 4);
            f->loop = loop != 0, f->range = range > 1 ? range : 30;
            if (pos) {
                void scene_camera_want(void);  // (hle/bubble.c: the camera it's heard from)
                f->positional = 1, memcpy(f->at, pos, 12);
                scene_camera_want();
            }
            f->gl = f->gr = -1;  // (first block: no ramp)
            atomic_fetch_add(&g_playing, 1);
            if (pos) atomic_fetch_add(&g_positional, 1);
        }
    }
    pthread_mutex_unlock(&g_lock);
    return v;
}
static void voice_end(Voice *v) {
    if (!v->id) return;
    atomic_fetch_sub(&g_playing, 1);
    if (v->positional) atomic_fetch_sub(&g_positional, 1);
    v->id = 0;
}
void modsound_stop(int voice) {
    pthread_mutex_lock(&g_lock);
    for (int i = 0; i < NVOICE; i++)
        if (g_v[i].id && (voice <= 0 || g_v[i].id == voice)) voice_end(&g_v[i]);  // (0: every one)
    pthread_mutex_unlock(&g_lock);
}
void modsound_set(int voice, float volume, const float pos[3]) {
    pthread_mutex_lock(&g_lock);
    for (int i = 0; i < NVOICE; i++)
        if (g_v[i].id == voice) {
            if (volume >= 0) g_v[i].volume = fminf(volume, 4);
            if (pos && g_v[i].positional) memcpy(g_v[i].at, pos, 12);
        }
    pthread_mutex_unlock(&g_lock);
}
int modsound_playing(int voice) {
    pthread_mutex_lock(&g_lock);
    int on = 0;
    for (int i = 0; i < NVOICE && !on; i++) on = g_v[i].id == voice && voice > 0;
    pthread_mutex_unlock(&g_lock);
    return on;
}
bool modsound_positional(void) { return atomic_load(&g_positional) > 0; }

static int recording(void) {
    static int r = -1;
    if (r < 0) r = getenv("LARECOMP_MODSOUND_WAV") && *getenv("LARECOMP_MODSOUND_WAV");
    return r;
}
bool modsound_active(void) { return atomic_load(&g_playing) > 0 || recording(); }  // (recording: the silences too)

// (testing) LARECOMP_MODSOUND_WAV=<file>: what the scripts' sounds added, alone (48 kHz stereo float) - checked without
// listening (a run's LARECOMP_NO_AUDIO=1 too: the game's output still runs, in real time)
static void record(const float *add, int frames) {
    static FILE *f;
    static int tried;
    static uint32_t bytes;
    if (!tried) {
        tried = 1;
        const char *p = getenv("LARECOMP_MODSOUND_WAV");
        if (p && *p) f = fopen(p, "wb");
    }
    if (!f) return;
    uint32_t h[11] = {0x46464952, 36 + bytes, 0x45564157, 0x20746d66, 16, 0x00020003, RATE, RATE * 8, 0x00200008, 0x61746164, bytes};
    if (!bytes) fwrite(h, 4, 11, f);
    fwrite(add, sizeof(float), (size_t)frames * 2, f);
    bytes += (uint32_t)frames * 8;
    h[1] = 36 + bytes, h[10] = bytes;
    long at = ftell(f);
    fseek(f, 0, SEEK_SET), fwrite(h, 4, 11, f), fseek(f, at, SEEK_SET);
    fflush(f);
}

// ---- mixing (the audio renderer's thread, each block: out = frames x 2 floats at rate)
int bubble_camera_get(float eye[3], float right[3]);  // (hle/bubble.c: the scene camera)
void modsound_mix(float *out, int frames, int rate) {
    if (!atomic_load(&g_playing) || rate <= 0) return;

    float eye[3] = {0, 0, 0}, right[3] = {1, 0, 0};
    int cam = atomic_load(&g_positional) ? bubble_camera_get(eye, right) : 0;
    double rstep = (double)RATE / rate;
    pthread_mutex_lock(&g_lock);
    for (int i = 0; i < NVOICE; i++) {
        Voice *v = &g_v[i];
        if (!v->id) continue;
        const Sound *s = &g_snd[v->sound - 1];
        float gl = v->volume, gr = v->volume;
        if (v->positional) {
            if (!cam) gl = gr = 0;
            else {
                float d[3] = {v->at[0] - eye[0], v->at[1] - eye[1], v->at[2] - eye[2]};
                float dist = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
                float g = dist <= 1.0f ? 1.0f : fmaxf(0.0f, 1.0f - (dist - 1.0f) / (v->range - 1.0f));
                g *= g;  // (quieter sooner)
                float pan = dist > 0.01f ? (d[0] * right[0] + d[1] * right[1] + d[2] * right[2]) / dist : 0;  // (-1 left .. 1 right)
                float a = (pan + 1.0f) * 0.78539816f;  // (equal power)
                gl = v->volume * g * cosf(a) * 1.41421356f, gr = v->volume * g * sinf(a) * 1.41421356f;
            }
        }
        float l0 = v->gl < 0 ? gl : v->gl, r0 = v->gr < 0 ? gr : v->gr;
        double step = rstep * v->pitch;
        for (int k = 0; k < frames; k++) {
            if (v->pos >= s->frames) {
                if (v->loop) v->pos -= s->frames;
                else { voice_end(v); break; }
            }
            uint32_t p = (uint32_t)v->pos;
            float t = (float)k / (float)frames, ml = l0 + (gl - l0) * t, mr = r0 + (gr - r0) * t;
            out[k * 2] += s->pcm[p * 2] * ml;
            out[k * 2 + 1] += s->pcm[p * 2 + 1] * mr;
            v->pos += step;
        }
        v->gl = gl, v->gr = gr;
    }
    pthread_mutex_unlock(&g_lock);
    for (int k = 0; k < frames * 2; k++) out[k] = fminf(1.0f, fmaxf(-1.0f, out[k]));
}
// into the float stereo output of the audio renderer (audren)
void modsound_mix_f32(float *out, int frames, int rate) {
    static float *tmp;
    static int cap;
    if (frames <= 0 || !modsound_active()) return;
    if (frames > cap) { free(tmp); tmp = malloc((size_t)frames * 2 * sizeof(float)); cap = tmp ? frames : 0; }
    if (!tmp) return;
    memset(tmp, 0, (size_t)frames * 2 * sizeof(float));
    modsound_mix(tmp, frames, rate);
    if (rate == RATE) record(tmp, frames);
    for (int k = 0; k < frames * 2; k++) out[k] = fminf(1.0f, fmaxf(-1.0f, out[k] + tmp[k]));
}
// the same into 16-bit output of ch channels (audout - Legends: Arceus mixes into it itself): the first two get them
void modsound_mix_s16(int16_t *buf, int frames, int ch, int rate) {
    static float *tmp;
    static int cap;
    if (frames <= 0 || ch < 1) return;
    if (frames > cap) { free(tmp); tmp = malloc((size_t)frames * 2 * sizeof(float)); cap = tmp ? frames : 0; }
    if (!tmp) return;
    memset(tmp, 0, (size_t)frames * 2 * sizeof(float));
    modsound_mix(tmp, frames, rate);
    if (rate == RATE) record(tmp, frames);
    for (int k = 0; k < frames; k++) {
        for (int c = 0; c < (ch > 1 ? 2 : 1); c++) {
            float v = buf[k * ch + c] / 32768.0f + (ch > 1 ? tmp[k * 2 + c] : 0.5f * (tmp[k * 2] + tmp[k * 2 + 1]));
            v = fminf(1.0f, fmaxf(-1.0f, v));
            buf[k * ch + c] = (int16_t)(v * 32767.0f);
        }
    }
}
