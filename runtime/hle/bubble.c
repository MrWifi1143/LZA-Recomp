// Speech bubbles over characters' heads (runtime/bubble.h): kept here, placed each drawn frame with the scene camera
// the game's camera update gave (bubble_camera), drawn by the overlay (ui/menu.mm: white box, tail, the text wrapped).
// The same file in both games; what differs - the camera hook, where a target stands - is in each game's code.
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>

#include "../bubble.h"
#include "../hle.h"
#include "../platform.h"

#define FADE_IN 0.15
#define FADE_OUT 0.35
#define FAR_FADE 35.0f  // (metres: fading out from here...)
#define FAR_GONE 45.0f  // (...to here)

typedef struct {
    int id;
    BubbleTarget t;
    char text[4 * BUBBLE_MAX_CHARS + 8];
    double t0, t1;  // (shown at, ends at: seconds, plat_now_ns)
} Bubble;
static Bubble g_b[BUBBLE_MAX];
static int g_next_id = 1, g_n;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

static struct {
    float eye[3], X[3], Y[3], Z[3], tan_half, aspect;
    int ok;
} g_cam;
static pthread_mutex_t g_cam_lock = PTHREAD_MUTEX_INITIALIZER;

// (the camera is followed while something places itself with it: a bubble, a positional sound, a script's to_screen -
// each game's camera hook feeds bubble_camera then; scene_camera_on (hle/modapi.c) switches a game's hook on)
volatile int g_scene_cam_wanted;
void scene_camera_on(void);
void scene_camera_want(void) {
    if (g_scene_cam_wanted) return;
    g_scene_cam_wanted = 1;
    scene_camera_on();
}
static double now_s(void) { return (double)plat_now_ns() / 1e9; }

static float default_height(int kind) {
    switch (kind) {
    case BUBBLE_POS: return 0.0f;
    case BUBBLE_POKEMON: return 1.0f;
    case BUBBLE_PLAYER: return 1.95f;
    default: return 2.0f;
    }
}

// the text kept: UTF-8 cut at BUBBLE_MAX_CHARS characters ("..." then), control characters but "\n" made spaces,
// broken sequences dropped
static int clean_text(char *out, int cap, const char *in) {
    int n = 0, chars = 0, total = 0;
    for (const unsigned char *p = (const unsigned char *)in; *p;) {  // (how many characters it has)
        int k = *p < 0x80 ? 1 : (*p >> 5) == 6 ? 2 : (*p >> 4) == 14 ? 3 : (*p >> 3) == 30 ? 4 : 1;
        total++;
        for (int i = 0; i < k && *p; i++) p++;
    }
    int keep = total > BUBBLE_MAX_CHARS ? BUBBLE_MAX_CHARS - 3 : total;
    for (const unsigned char *p = (const unsigned char *)in; *p && chars < keep;) {
        int k = *p < 0x80 ? 1 : (*p >> 5) == 6 ? 2 : (*p >> 4) == 14 ? 3 : (*p >> 3) == 30 ? 4 : 0;
        if (k == 0) { p++; continue; }
        bool ok = true;
        for (int i = 1; i < k; i++) ok = ok && (p[i] & 0xc0) == 0x80;
        if (!ok) { p++; continue; }
        if (n + k + 4 >= cap) break;
        if (k == 1 && (*p < 0x20 || *p == 0x7f)) out[n++] = *p == '\n' ? '\n' : ' ';
        else memcpy(out + n, p, (size_t)k), n += k;
        p += k, chars++;
    }
    while (n > 0 && (out[n - 1] == ' ' || out[n - 1] == '\n')) n--;
    if (total > keep) memcpy(out + n, "...", 3), n += 3;
    out[n] = 0;
    return total > keep ? keep : total;
}

static bool same_target(const BubbleTarget *a, const BubbleTarget *b) {
    if (a->kind != b->kind || a->owner != b->owner) return false;
    switch (a->kind) {
    case BUBBLE_PLAYER: return true;
    case BUBBLE_NPC: case BUBBLE_POKEMON: return a->handle == b->handle;
    case BUBBLE_OBJECT: return !strcmp(a->object, b->object);
    case BUBBLE_FN: return a->where == b->where && a->ctx == b->ctx;
    default: return false;  // (a spot: every one its own)
    }
}

int bubble_show(const BubbleTarget *t, const char *utf8, float seconds) {
    if (!t || !utf8 || (t->kind == BUBBLE_FN && !t->where)) return 0;
    scene_camera_want();
    char text[sizeof g_b[0].text];
    int chars = clean_text(text, sizeof text, utf8);
    if (!text[0]) return 0;
    if (!(seconds > 0)) seconds = fminf(12.0f, fmaxf(3.0f, 2.0f + 0.08f * (float)chars));
    double now = now_s();
    pthread_mutex_lock(&g_lock);
    Bubble *b = NULL;
    for (int i = 0; i < BUBBLE_MAX && !b; i++)
        if (g_b[i].id && same_target(&g_b[i].t, t)) b = &g_b[i];
    bool replaced = b != NULL;
    for (int i = 0; i < BUBBLE_MAX && !b; i++)
        if (!g_b[i].id) b = &g_b[i];
    if (!b) {  // (all taken: the oldest's place)
        b = &g_b[0];
        for (int i = 1; i < BUBBLE_MAX; i++) if (g_b[i].t0 < b->t0) b = &g_b[i];
    }
    if (!replaced) {
        if (!b->id) g_n++;
        b->id = g_next_id++;
        if (g_next_id <= 0) g_next_id = 1;
        b->t0 = now;
    } else if (now - b->t0 > FADE_IN) b->t0 = now - FADE_IN;  // (a new line over it: no fade in again)
    b->t = *t;
    b->t.object[sizeof b->t.object - 1] = 0;
    if (!(b->t.height > 0) && b->t.kind != BUBBLE_POS) {
        float bubble_game_height(int kind);  // (each game's people: hle/modapi.c; 0 the default)
        float gh = bubble_game_height(t->kind);
        b->t.height = gh > 0 ? gh : default_height(t->kind);
    }
    memcpy(b->text, text, sizeof text);
    b->t1 = now + seconds;
    int id = b->id;
    pthread_mutex_unlock(&g_lock);
    return id;
}

static void drop(Bubble *b) { if (b->id) g_n--; memset(b, 0, sizeof *b); }

int bubble_hide(int id) {
    int ok = 0;
    pthread_mutex_lock(&g_lock);
    double now = now_s();
    for (int i = 0; i < BUBBLE_MAX; i++)
        if (g_b[i].id == id && id > 0) {  // (it fades out)
            if (g_b[i].t1 > now + FADE_OUT) g_b[i].t1 = now + FADE_OUT;
            ok = 1;
        }
    pthread_mutex_unlock(&g_lock);
    return ok;
}
void bubble_hide_target(const BubbleTarget *t) {
    pthread_mutex_lock(&g_lock);
    double now = now_s();
    for (int i = 0; i < BUBBLE_MAX; i++)
        if (g_b[i].id && same_target(&g_b[i].t, t) && g_b[i].t1 > now + FADE_OUT) g_b[i].t1 = now + FADE_OUT;
    pthread_mutex_unlock(&g_lock);
}
void bubble_clear(int owner) {
    pthread_mutex_lock(&g_lock);
    for (int i = 0; i < BUBBLE_MAX; i++)
        if (g_b[i].id && (owner < 0 || g_b[i].t.owner == owner)) drop(&g_b[i]);
    pthread_mutex_unlock(&g_lock);
}
int bubble_count(void) { return g_n; }

void bubble_camera(const float *e) {
    float fov = e[-9], aspect = e[-8];
    if (!(fov > 0.05f && fov < 3.0f) || !(aspect > 0.2f && aspect < 10.0f)) return;
    pthread_mutex_lock(&g_cam_lock);
    for (int i = 0; i < 3; i++) g_cam.eye[i] = e[i], g_cam.X[i] = e[4 + 4 * i], g_cam.Y[i] = e[5 + 4 * i], g_cam.Z[i] = e[6 + 4 * i];
    g_cam.tan_half = tanf(fov * 0.5f), g_cam.aspect = aspect, g_cam.ok = 1;
    pthread_mutex_unlock(&g_cam_lock);
}

static float dot3(const float *a, const float *b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

// the scene camera's eye and right (positional sounds: hle/modsound.c); 0 none yet
int bubble_camera_get(float eye[3], float right[3]) {
    pthread_mutex_lock(&g_cam_lock);
    int ok = g_cam.ok;
    if (ok) memcpy(eye, g_cam.eye, 12), memcpy(right, g_cam.X, 12);
    pthread_mutex_unlock(&g_cam_lock);
    return ok;
}
// a point of the world on a w x h overlay (the mod API's hud_to_screen): 1 in front of the camera (out: x, y, and the
// metres away), 0 behind it / no camera yet
int bubble_world_to_screen(const float p[3], float w, float h, float out[3]) {
    scene_camera_want();
    pthread_mutex_lock(&g_cam_lock);
    __typeof__(g_cam) cam = g_cam;
    pthread_mutex_unlock(&g_cam_lock);
    if (!cam.ok) return 0;
    float d[3] = {p[0] - cam.eye[0], p[1] - cam.eye[1], p[2] - cam.eye[2]}, zc = dot3(d, cam.Z);
    if (zc > -0.05f) return 0;
    float vw = fminf(w, h * cam.aspect), vh = vw / cam.aspect, vx = (w - vw) * 0.5f, vy = (h - vh) * 0.5f;
    float nx = dot3(d, cam.X) / (-zc * cam.tan_half * cam.aspect), ny = dot3(d, cam.Y) / (-zc * cam.tan_half);
    out[0] = vx + (nx * 0.5f + 0.5f) * vw, out[1] = vy + (0.5f - ny * 0.5f) * vh, out[2] = sqrtf(dot3(d, d));
    return 1;
}

int bubble_draw_list(BubbleDraw *out, int max, float w, float h) {
    if (!g_n) return 0;
    Bubble b[BUBBLE_MAX];
    int nb = 0;
    double now = now_s();
    pthread_mutex_lock(&g_lock);
    for (int i = 0; i < BUBBLE_MAX; i++) {
        if (!g_b[i].id) continue;
        if (now >= g_b[i].t1) { drop(&g_b[i]); continue; }
        b[nb++] = g_b[i];
    }
    pthread_mutex_unlock(&g_lock);
    if (!nb) return 0;
    pthread_mutex_lock(&g_cam_lock);
    __typeof__(g_cam) cam = g_cam;
    pthread_mutex_unlock(&g_cam_lock);
    if (!cam.ok) return 0;
    // the game's image in the window: its camera's shape, fitted and centred (nvn/output_scale.c)
    float vw = fminf(w, h * cam.aspect), vh = vw / cam.aspect, vx = (w - vw) * 0.5f, vy = (h - vh) * 0.5f;
    int n = 0;
    int gone[BUBBLE_MAX], ngone = 0;
    for (int i = 0; i < nb && n < max; i++) {
        float feet[3];
        int st = b[i].t.kind == BUBBLE_POS ? (memcpy(feet, b[i].t.pos, 12), 1) : bubble_target_where(&b[i].t, feet);
        if (st < 0) { gone[ngone++] = b[i].id; continue; }
        if (st == 0) continue;
        float p[3] = {feet[0], feet[1] + b[i].t.height, feet[2]}, d[3] = {p[0] - cam.eye[0], p[1] - cam.eye[1], p[2] - cam.eye[2]};
        float zc = dot3(d, cam.Z), dist = sqrtf(dot3(d, d));
        if (zc > -0.3f || dist > FAR_GONE) continue;  // (behind the camera, or far)
        float nx = dot3(d, cam.X) / (-zc * cam.tan_half * cam.aspect), ny = dot3(d, cam.Y) / (-zc * cam.tan_half);
        if (fabsf(nx) > 1.3f || fabsf(ny) > 1.6f) continue;  // (well off the screen)
        double a = fmin(1.0, (now - b[i].t0) / FADE_IN);
        a = fmin(a, (b[i].t1 - now) / FADE_OUT);
        if (dist > FAR_FADE) a *= (FAR_GONE - dist) / (FAR_GONE - FAR_FADE);
        if (a <= 0.01) continue;
        out[n].x = vx + (nx * 0.5f + 0.5f) * vw, out[n].y = vy + (0.5f - ny * 0.5f) * vh;
        out[n].alpha = (float)a, out[n].dist = dist;
        memcpy(out[n].text, b[i].text, sizeof out[n].text);
        n++;
    }
    if (ngone) {  // (their target is gone for good: so are they)
        pthread_mutex_lock(&g_lock);
        for (int k = 0; k < ngone; k++)
            for (int i = 0; i < BUBBLE_MAX; i++)
                if (g_b[i].id == gone[k]) drop(&g_b[i]);
        pthread_mutex_unlock(&g_lock);
    }
    for (int i = 1; i < n; i++)  // (farthest first: the nearer drawn over it)
        for (int j = i; j > 0 && out[j].dist > out[j - 1].dist; j--) {
            BubbleDraw t = out[j];
            out[j] = out[j - 1], out[j - 1] = t;
        }
    return n;
}
