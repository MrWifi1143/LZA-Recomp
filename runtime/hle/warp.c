// LARECOMP_WARP="<x> <y> <z>" (Legends Studio's "Play here"): the player put at that spot of the level once it has
// loaded. Z-A 1.0's player character body - the one object of its class, vtable 0x7003be6218 - keeps the player's
// position at +0x90 (its rotation matrix before it, at +0x60) and the game follows it: the character, its transform and
// the camera move there and stay. The functions that step it each frame (0x7000081ce0, 0x700004c3e8) get it as x0,
// which is how it is found. Written once it has existed for two seconds and stood still for one (the save's own
// position applied, the fade-in over), then for a few presents more; once.
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../hle.h"

void rt_register_fnhook_at(uint64_t addr, int (*fn)(Cpu *c, uint64_t addr));

#define BODY_VTABLE 0x7003be6218ull
static float g_to[3];
static volatile uint64_t g_body;
static int g_state = -1;  // -1 off, 0 waiting, 1 writing, 2 done

static int body_hook(Cpu *c, uint64_t addr) {
    (void)addr;
    uint64_t o = c->x[0];
    if (g_state == 0 && o >= HEAP_BASE && o < HEAP_BASE + HEAP_MAX && *(const uint64_t *)(uintptr_t)o == BODY_VTABLE) g_body = o;
    return 0;
}

__attribute__((constructor)) static void warp_init(void) {
    const char *w = getenv("LARECOMP_WARP");
    if (!w || sscanf(w, "%f %f %f", &g_to[0], &g_to[1], &g_to[2]) != 3) return;
    g_state = 0;
    rt_register_fnhook_at(0x7000081ce0ull, body_hook);
    rt_register_fnhook_at(0x700004c3e8ull, body_hook);
}

void warp_frame(uint64_t frame) {  // (each game frame: runtime/hle/input.c)
    if (g_state < 0 || g_state == 2) return;
    static uint64_t body, since, still_since, writes;
    static float last[3];
    uint64_t b = g_body;
    if (!b) return;
    float *pos = (float *)(uintptr_t)(b + 0x90);
    if (b != body || *(const uint64_t *)(uintptr_t)b != BODY_VTABLE) {  // (a new body: the level loaded again)
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
