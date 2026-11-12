// LARECOMP_WARP="<x> <y> <z>" (Legends Studio's "Play here"): the player put at that spot of the level once it has
// loaded. Z-A's character bodies (vtable 0x7003be6218) keep the position at +0x90 (the rotation matrix before it, at
// +0x60) and the game follows it: writing the player's moves the character, its transform and the camera for good. The
// functions that step a body each frame (0x7000081ce0, 0x700004c3e8) get it as x0, which is how they are found.
//   The player's body isn't the only one of its class: the townspeople's bodies (Lumiose's crowd) are stepped through
// 0x7000081ce0 as well (contrib/legendslua/research/za_npc_bodies.lua), so "the last body hooked" was any of them - the
// warp kept seeing "a new body", never waited out its two seconds and never wrote (and LARECOMP_LOG_PLAYER logged
// whoever came last; both functions step every body - in Lumiose dozens, 2026-11-11). The bodies are kept in a table
// now; the player's is the oldest one still stepped: it exists from the title screen on, at the save's position, before
// the level's people (checked in Lumiose: the first body seen was the player's, the warp put it at the Pokemon Center). Written once it has existed for two seconds and stood still for one
// (the save's own position applied, the fade-in over), then for a few presents more; again (up to 3 times, within a
// minute) when the game puts the player somewhere else at once - a save loaded after the title, where it was written.
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../hle.h"

void rt_register_fnhook_at(uint64_t addr, int (*fn)(Cpu *c, uint64_t addr));

#define BODY_VTABLE 0x7003be6218ull
#define STEP_A 0x7000081ce0ull  // (both step every character body)
#define STEP_B 0x700004c3e8ull
static float g_to[3];
static int g_state = -1;  // -1 off, 0 waiting, 1 writing, 2 written (watching for the save putting it back), 3 finished
static long g_log_every;  // (research) LARECOMP_LOG_PLAYER=<n>: the player's position every n frames ("[player] frame f x y z")

#define NBODY 128
static struct { uint64_t o, first, last; unsigned via; } g_bodies[NBODY];  // (via: the hooks that saw it, research)
static pthread_mutex_t g_body_lock = PTHREAD_MUTEX_INITIALIZER;
static volatile uint64_t g_frame;  // (the presents so far: warp_frame)

static bool is_body(uint64_t o) { return o >= HEAP_BASE && o < HEAP_BASE + HEAP_MAX && *(const uint64_t *)(uintptr_t)o == BODY_VTABLE; }

static int body_hook(Cpu *c, uint64_t addr) {
    uint64_t o = c->x[0];
    if ((g_state < 0 || g_state == 3) && !g_log_every) return 0;  // (off, or finished)
    if (!is_body(o)) return 0;
    uint64_t f = g_frame;
    unsigned via = addr == STEP_B ? 2 : 1;
    pthread_mutex_lock(&g_body_lock);
    int i = 0, free_slot = -1;
    for (; i < NBODY; i++) {
        if (g_bodies[i].o == o) break;
        if (free_slot < 0 && (!g_bodies[i].o || f - g_bodies[i].last > 300)) free_slot = i;  // (gone for 10 s: reused)
    }
    bool fresh = i == NBODY;
    if (fresh && free_slot >= 0) i = free_slot, g_bodies[i].o = o, g_bodies[i].first = f, g_bodies[i].via = 0;
    unsigned was = i < NBODY ? g_bodies[i].via : 0;
    if (i < NBODY) g_bodies[i].last = f, g_bodies[i].via |= via;
    pthread_mutex_unlock(&g_body_lock);
    if (g_log_every && i < NBODY && (fresh || !(was & via))) {  // (research: each body as each hook first sees it)
        const float *p = (const float *)(uintptr_t)(o + 0x90);
        LOG("[player] body %llx via %llx lr %llx frame %llu at %.2f %.2f %.2f\n", (unsigned long long)o, (unsigned long long)addr,
            (unsigned long long)c->x[30], (unsigned long long)f, p[0], p[1], p[2]);
    }
    return 0;
}

// the player's body now (0 none): the oldest one stepped in the last second
static uint64_t player_body(uint64_t frame) {
    uint64_t best = 0, best_first = ~0ull;
    pthread_mutex_lock(&g_body_lock);
    for (int i = 0; i < NBODY; i++) {
        if (!g_bodies[i].o || frame - g_bodies[i].last > 30 || !is_body(g_bodies[i].o)) continue;
        if (g_bodies[i].first < best_first) best = g_bodies[i].o, best_first = g_bodies[i].first;
    }
    pthread_mutex_unlock(&g_body_lock);
    return best;
}

__attribute__((constructor)) static void warp_init(void) {
    g_log_every = getenv("LARECOMP_LOG_PLAYER") ? atol(getenv("LARECOMP_LOG_PLAYER")) : 0;
    const char *w = getenv("LARECOMP_WARP");
    if (w && sscanf(w, "%f %f %f", &g_to[0], &g_to[1], &g_to[2]) == 3) g_state = 0;
    if (g_log_every || g_state == 0) {
        rt_register_fnhook_at(STEP_A, body_hook);
        rt_register_fnhook_at(STEP_B, body_hook);
        LOG("[warp] %s%s\n", g_state == 0 ? "the player goes to " : "", g_state == 0 ? w : "logging the player's position");
    }
}

void warp_frame(uint64_t frame) {  // (each game frame: runtime/hle/input.c)
    g_frame = frame;
    if (g_state < 0 && !g_log_every) return;
    uint64_t b = player_body(frame);
    if (g_log_every > 0 && frame % (uint64_t)g_log_every == 0 && b) {
        const float *p = (const float *)(uintptr_t)(b + 0x90);
        LOG("[player] frame %llu %.2f %.2f %.2f (body %llx)\n", (unsigned long long)frame, p[0], p[1], p[2], (unsigned long long)b);
    }
    if (g_state < 0 || g_state == 3) return;
    static uint64_t body, since, still_since, writes, done_at, redo;
    static float last[3];
    if (!b) return;
    float *pos = (float *)(uintptr_t)(b + 0x90);
    if (g_state == 2) {  // (written; the body exists from the title screen on: a save loaded after it puts the player back)
        bool jump = fabsf(pos[0] - last[0]) + fabsf(pos[2] - last[2]) > 4.0f;
        memcpy(last, pos, 12);
        if (frame - done_at >= 1800 || redo >= 3) { g_state = 3; return; }
        if (jump && b == body) {
            redo++, g_state = 0, since = still_since = frame;
            LOG("[warp] the player was put back at %.2f %.2f %.2f: again once it stands still\n", pos[0], pos[1], pos[2]);
        }
        return;
    }
    if (b != body) {  // (a new body: the level loaded again)
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
    memcpy(last, g_to, 12);
    if (++writes >= 10) g_state = 2, done_at = frame;
}

// LARECOMP_STORY="<n>" (Legends Studio's "Play here at a story point"): the level's spawn conditions see the story at
// phase n (10000 per chapter, 100000 the end). Z-A keeps the phase as the work SYS_WORK_PHASE, one entry of the
// save's works: the work manager (its pointer at 0x7003ec3328, up once the word at +0x200 is set) holds four libc++
// unordered_maps - buckets at +0xf0 (the array) and +0xf8 (their count), then +0x128, +0x160, +0x198 - of nodes
// {next, hash, key, i32 value at +0x18} keyed by the FNV-1a 64 of the work's name, read by 0x700048b3b0 (x0 the
// manager, x1 a key {hash, name, length, shared string}, w0 the value) and written by 0x700092dca0 (x0, x1, w2 the
// value; never inserting). The phase has a getter of its own, 0x7000066dc0 (w0 the phase), and the placement data's
// phase_condition (appearingConditionList) is evaluated by 0x7000066930, which calls it from 0x7000066bac and compares
// with the condition's op. The story's own advance (0x7002ace9cc) reads it the same way, finds the next phase in its
// table and writes it. Hooked: the getter answers n to the evaluator - the save's value is never written, the story
// stays where the save has it. "<n> all": n to every reader (both getters), and writes of the phase swallowed.
#define WORK_MANAGER_PTR 0x7003ec3328ull
#define PHASE_HASH 0x992fe2a6dec12926ull  // (FNV-1a 64 of SYS_WORK_PHASE)
#define PHASE_GET 0x7000066dc0ull
#define PHASE_COND_CALLER 0x7000066bacull  // (the return into the phase_condition evaluator)
#define WORK_GET 0x700048b3b0ull
#define WORK_SET 0x700092dca0ull
static int g_story = -1, g_story_all;

void rt_call_original(Cpu *c, uint64_t addr);  // (runtime/fnhook.c: the game's own function, from inside its hook)

// the save's own phase, by the game's getter: false before the work manager is up (its instance pointer 0x7003ec3328,
// set and flagged at +0x200 once constructed - the getter reads through it unguarded)
static bool phase_value(Cpu *c, int *out) {
    uint64_t mgr = *(const uint64_t *)(uintptr_t)WORK_MANAGER_PTR;
    if (mgr - 0x7000000000ull >= 0x617b000ull ||  // (in main's image, 0x617b000 bytes)
        !*(const volatile uint32_t *)(uintptr_t)(mgr + 0x200)) return false;
    rt_call_original(c, PHASE_GET);
    *out = (int)c->x[0];
    return true;
}

static int phase_get_hook(Cpu *c, uint64_t addr) {
    (void)addr;
    if (!g_story_all && c->x[30] != PHASE_COND_CALLER) return 0;
    static int last = -1;  // (the save's own value, as last logged: 0 at the title, the save's once it has loaded)
    int v;
    if (phase_value(c, &v) && v != last) last = v, LOG("[warp] story phase %d read as %d%s\n", v, g_story, g_story_all ? "" : " by the spawn conditions");
    c->x[0] = (uint32_t)g_story;
    return 1;
}

static bool phase_key(const Cpu *c) { return c->x[1] && *(const uint64_t *)(uintptr_t)c->x[1] == PHASE_HASH; }

static int work_get_hook(Cpu *c, uint64_t addr) {  // ("all": the generic getter asked for the phase)
    (void)addr;
    if (!phase_key(c)) return 0;
    c->x[0] = (uint32_t)g_story;
    return 1;
}

static int work_set_hook(Cpu *c, uint64_t addr) {  // ("all": a write of the phase - the story's advance - swallowed)
    (void)addr;
    if (!phase_key(c)) return 0;
    static int logged;
    if (logged < 4) logged++, LOG("[warp] story phase write of %d swallowed\n", (int)c->x[2]);
    c->x[0] = 1;
    return 1;
}

__attribute__((constructor)) static void story_init(void) {
    const char *s = getenv("LARECOMP_STORY");
    char all[8] = "";
    if (!s || sscanf(s, "%d %7s", &g_story, all) < 1 || g_story < 0) return;
    g_story_all = !strcmp(all, "all");
    rt_register_fnhook_at(PHASE_GET, phase_get_hook);
    if (g_story_all) rt_register_fnhook_at(WORK_GET, work_get_hook), rt_register_fnhook_at(WORK_SET, work_set_hook);
    LOG("[warp] story at phase %d%s\n", g_story, g_story_all ? " for everything" : " for the spawn conditions");
}
