// LARECOMP_WARP="<x> <y> <z>" (Legends Studio's "Play here"): the player put at that spot of the area once it has
// loaded. Legends: Arceus 1.1.1's character bodies (vtable 0x7004187048: the player's, the NPCs', their parts) keep
// their position at +0x90 (a rotation matrix before it, at +0x60) and the game follows it: writing the player's moves
// the player there to stay. Which body is the player's: the one standing exactly where the camera looks - the camera
// update (0x7002994b70, the camera as x0) keeps its target, the player's position, at +0x320 - among the bodies the
// physics steps (0x7002fee4e0, the body as x0). Written once it has existed for two seconds and stood still for one
// (the save's own position applied, the fade-in over), then for a few presents more; once.
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../hle.h"

void rt_register_fnhook_at(uint64_t addr, int (*fn)(Cpu *c, uint64_t addr));

#define BODY_VTABLE 0x7004187048ull
static float g_to[3];
static volatile uint64_t g_body, g_cam;
static int g_state = -1;  // -1 off, 0 waiting, 1 writing, 2 done

static bool in_heap(uint64_t a) { return a >= HEAP_BASE && a < HEAP_BASE + HEAP_MAX; }

static volatile uint64_t g_seen_body;  // (the player's body as last found, warp or not: LARECOMP_LIVE's state shows it)

static int cam_hook(Cpu *c, uint64_t addr) {
    (void)addr;
    if ((g_state == 0 || g_state == 3) && in_heap(c->x[0])) g_cam = c->x[0];
    return 0;
}

static int body_hook(Cpu *c, uint64_t addr) {
    (void)addr;
    uint64_t o = c->x[0], cam = g_cam;
    if ((g_state != 0 && g_state != 3) || !cam || !in_heap(o) || *(const uint64_t *)(uintptr_t)o != BODY_VTABLE) return 0;
    const float *p = (const float *)(uintptr_t)(o + 0x90), *t = (const float *)(uintptr_t)(cam + 0x320);
    if (fabsf(p[0] - t[0]) + fabsf(p[1] - t[1]) + fabsf(p[2] - t[2]) < 1e-3f) g_seen_body = o, g_body = g_state == 0 ? o : g_body;
    return 0;
}

__attribute__((constructor)) static void warp_init(void) {
    const char *w = getenv("LARECOMP_WARP");
    if (!w || sscanf(w, "%f %f %f", &g_to[0], &g_to[1], &g_to[2]) != 3) {
        if (!getenv("LARECOMP_LIVE")) return;
        g_state = 3;  // (no warp: only following the player, for LARECOMP_LIVE)
    } else g_state = 0;
    rt_register_fnhook_at(0x7002994b70ull, cam_hook);
    rt_register_fnhook_at(0x7002fee4e0ull, body_hook);
}

// where the player is (LARECOMP_LIVE's state file): false before the body is found
bool warp_player_pos(float out[3]) {
    uint64_t b = g_seen_body;
    if (!b || *(const uint64_t *)(uintptr_t)b != BODY_VTABLE) return false;
    memcpy(out, (const float *)(uintptr_t)(b + 0x90), 12);
    return true;
}

void warp_frame(uint64_t frame) {  // (each game frame: runtime/hle/input.c)
    if (g_state < 0 || g_state >= 2) return;
    static uint64_t body, since, still_since, writes;
    static float last[3];
    uint64_t b = g_body;
    if (!b) return;
    float *pos = (float *)(uintptr_t)(b + 0x90);
    if (b != body || *(const uint64_t *)(uintptr_t)b != BODY_VTABLE) {  // (a new body: the area loaded again)
        body = b, since = still_since = frame, memcpy(last, pos, 12), g_state = 0;
        LOG("[warp] the player's body %llx, at %.2f %.2f %.2f\n", (unsigned long long)b, pos[0], pos[1], pos[2]);
        return;
    }
    if (g_state == 0) {
        if (fabsf(pos[0] - last[0]) + fabsf(pos[1] - last[1]) + fabsf(pos[2] - last[2]) > 1e-3f) still_since = frame, memcpy(last, pos, 12);
        if (frame - since < 120 || frame - still_since < 60) return;
        g_state = 1, writes = 0;
        LOG("[warp] the player moved from %.2f %.2f %.2f to %.2f %.2f %.2f\n", pos[0], pos[1], pos[2], g_to[0], g_to[1], g_to[2]);
    }
    memcpy(pos, g_to, 12);
    if (++writes >= 10) g_state = 2;
}
