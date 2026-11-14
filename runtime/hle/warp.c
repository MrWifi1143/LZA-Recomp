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

static int ghost_step(Cpu *c, uint64_t addr, uint64_t o);  // (below: LARECOMP_GHOST)
static int g_ghost_on;
static int body_hook(Cpu *c, uint64_t addr) {
    uint64_t o = c->x[0];
    if ((g_state < 0 || g_state == 3) && !g_log_every && !g_ghost_on) return 0;  // (off, or finished)
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
    if (g_ghost_on) { int r = ghost_step(c, addr, o); if (r) return r; }
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
    extern void ghost_init(void);
    ghost_init();
    if (g_log_every || g_state == 0 || g_ghost_on) {
        rt_register_fnhook_at(STEP_A, body_hook);
        rt_register_fnhook_at(STEP_B, body_hook);
        LOG("[warp] %s%s\n", g_state == 0 ? "the player goes to " : "", g_state == 0 ? w : "logging the player's position");
    }
}

static void ghost_frame(uint64_t frame);
void warp_frame(uint64_t frame) {  // (each game frame: runtime/hle/input.c)
    g_frame = frame;
    if (g_ghost_on) ghost_frame(frame);
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

// ---- (research) LARECOMP_GHOST=<my port>,<peer port>[,<peer host>]: a second player in the field, as a stand-in.
// Each copy sends its player's position and facing over UDP every frame; the other shows them with a character of its
// own level standing in. The stand-in is a story/station NPC near our player (a character body 3-40 m away that isn't
// stepped - the crowd's models follow their own animation, not their bodies, 2026-11-13), moved through its transform
// node: a node (vtable 0x7003be0d28) keeps its local scale at +0xc0, rotation at +0xd0 (quaternion xyzw) and
// translation at +0xe0, and 0x7001ccf27c (x0 the node; called many times a frame) makes its world matrices (+0x40,
// +0x80) from them - writing the translation moves the model (found 2026-11-14 by shifting every copy of a station
// NPC's x and halving: the world matrices themselves are overwritten). The character's own node is the one whose
// translation is its body's position. The stand-in keeps its own model, animation and collision (where it stood):
// a test that another player can be shown in Lumiose outside a battle, not the game's own remote player.
#ifndef _WIN32  // (sockets and process_vm_readv: Linux research only)
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/uio.h>
#define _GNU_SOURCE_UNUSED
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
static uint64_t ghost_now_ns(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec; }
ssize_t process_vm_readv(pid_t, const struct iovec *, unsigned long, const struct iovec *, unsigned long, unsigned long);
#define NODE_VTABLE 0x7003be0d28ull
#define NODE_UPDATE 0x7001ccf27cull
static int g_ghost_fd = -1;
static struct sockaddr_in g_ghost_peer;
static float g_peer[4];              // (the peer's player: x y z, yaw)
static volatile uint64_t g_peer_at;  // (frame of the last packet)
static float g_off;                  // (LARECOMP_GHOST_OFFSET=<m>: research - beside the peer's spot, a loopback test on one copy)
static volatile uint64_t g_stand_body, g_stand_node;  // (the stand-in's body, then its node)
static volatile int g_scanning;
static volatile uint64_t g_me_body, g_me_node;  // (our player's body and node: the node has the model's facing)

static int node_hook(Cpu *c, uint64_t addr) {
    (void)addr;
    uint64_t n = c->x[0];
    uint64_t mb = g_me_body;
    if (mb && !g_me_node && n >= HEAP_BASE && n < HEAP_BASE + HEAP_MAX && *(const uint64_t *)(uintptr_t)n == NODE_VTABLE &&
        !memcmp((const void *)(uintptr_t)(n + 0xe0), (const void *)(uintptr_t)(mb + 0x90), 4) &&
        !memcmp((const void *)(uintptr_t)(n + 0xe8), (const void *)(uintptr_t)(mb + 0x98), 4)) {
        g_me_node = n;
        LOG("[ghost] our player's node %llx\n", (unsigned long long)n);
    }
    if (!g_stand_body) return 0;
    if (n == g_stand_node) {
        if (g_frame - g_peer_at > 30 || *(const uint64_t *)(uintptr_t)n != NODE_VTABLE) return 0;
        float *q = (float *)(uintptr_t)(n + 0xd0), *t = (float *)(uintptr_t)(n + 0xe0);
        q[0] = 0, q[1] = sinf(g_peer[3] * 0.5f), q[2] = 0, q[3] = cosf(g_peer[3] * 0.5f);
        t[0] = g_peer[0] + g_off, t[1] = g_peer[1], t[2] = g_peer[2];
        return 0;
    }
    if (!g_stand_node && n >= HEAP_BASE && n < HEAP_BASE + HEAP_MAX && *(const uint64_t *)(uintptr_t)n == NODE_VTABLE &&
        !memcmp((const void *)(uintptr_t)(n + 0xe0), (const void *)(uintptr_t)(g_stand_body + 0x90), 4) &&
        !memcmp((const void *)(uintptr_t)(n + 0xe8), (const void *)(uintptr_t)(g_stand_body + 0x98), 4)) {
        g_stand_node = n;
        LOG("[ghost] the stand-in's node %llx\n", (unsigned long long)n);
    }
    return 0;
}

// every character body in the heap (a background thread, once): the nearest one 3-40 m from `me` not stepped lately
static uint64_t g_scan_me;
static void *scan_bodies(void *arg) {
    (void)arg;
    uint64_t me = g_scan_me, best_o = 0;
    float px = ((const float *)(uintptr_t)(me + 0x90))[0], pz = ((const float *)(uintptr_t)(me + 0x90))[2], best = 1e9f;
    const size_t chunk = 1u << 20;
    uint64_t *buf = malloc(chunk);
    int count = 0;
    for (uint64_t a = HEAP_BASE; buf && a < HEAP_BASE + HEAP_MAX; a += chunk) {
        struct iovec l = {buf, chunk}, r = {(void *)(uintptr_t)a, chunk};
        if (process_vm_readv(getpid(), &l, 1, &r, 1, 0) != (ssize_t)chunk) continue;
        for (size_t k = 0; k < chunk / 8; k++) {
            if (buf[k] != BODY_VTABLE) continue;
            uint64_t o = a + k * 8;
            if (o == me || chunk - k * 8 < 0xa0) continue;
            count++;
            const float *q = (const float *)((const char *)buf + k * 8 + 0x90);
            float d = sqrtf((q[0] - px) * (q[0] - px) + (q[2] - pz) * (q[2] - pz));
            bool stepped = false;
            pthread_mutex_lock(&g_body_lock);
            for (int i = 0; i < NBODY; i++) if (g_bodies[i].o == o && g_frame - g_bodies[i].last <= 30) stepped = true;
            pthread_mutex_unlock(&g_body_lock);
            if (!stepped && d > 3 && d < 40 && d < best) best = d, best_o = o;
        }
    }
    free(buf);
    LOG("[ghost] %d character bodies; the stand-in: %llx, %.1f m from us\n", count, (unsigned long long)best_o, best);
    g_stand_body = best_o;
    g_scanning = 0;
    return NULL;
}

// The walk-up trade: A pressed by the stand-in (the game's own Talk is kept from it) asks the other player; them
// doing the same answers. Both games then go to Link Play -> Link Trade -> Nearby Players with the same Link Code
// (pressed through runtime/hle/input.c's queue; the code typed by menu.mm's name entry): the game's own trade from there.
#define TRADE_NEAR 2.5f
#define BTN_A_BIT 1ull
void input_macro(const char *lines);
bool input_macro_busy(void);
uint64_t input_buttons_now(void);
extern volatile uint64_t g_input_swallow;
void menu_notice_for(const char *text, unsigned ms);
extern char g_kbd_auto_once[32];
static int g_trade;            // 0 nothing, 1 we asked, 2 they asked
static uint32_t g_trade_code;  // (8 digits, below 2^24: exact in the packet's float)
static uint64_t g_trade_at;
static void trade_send(int accept, uint32_t code) {
    float out[5] = {accept ? 4.0f : 3.0f, (float)code, 0, 0, 0};
    for (int i = 0; i < 3; i++) sendto(g_ghost_fd, out, sizeof out, 0, (struct sockaddr *)&g_ghost_peer, sizeof g_ghost_peer);
}
static void trade_start(uint32_t code) {
    snprintf(g_kbd_auto_once, sizeof g_kbd_auto_once, "%08u", code);
    // X (menu), up to Link Play, A; right twice to Link Trade, A; Nearby Players, A; the Link Code entry answers itself
    input_macro("200ms -\n170ms X\n2500ms -\n170ms UP\n500ms -\n170ms A\n4000ms -\n170ms RIGHT\n500ms -\n170ms RIGHT\n"
                "500ms -\n170ms A\n3000ms -\n170ms A\n1000ms -\n");
    char t[96];
    snprintf(t, sizeof t, "Trading with the other player (Link Code %04u %04u)", code / 10000, code % 10000);
    menu_notice_for(t, 8000);
    LOG("[ghost] trade: Link Code %08u\n", code);
    g_trade = 0;
}
static void trade_message(bool accept, uint32_t code, uint64_t frame) {
    if (accept) {
        if (g_trade == 1 && code == g_trade_code) trade_start(code);
        return;
    }
    if (g_trade == 2 && code == g_trade_code) return;  // (a repeat)
    if (g_trade == 1 && code > g_trade_code) return;   // (both asked at once: the lower code stands)
    g_trade = 2, g_trade_code = code, g_trade_at = frame;
    menu_notice_for("The other player wants to trade! Walk up to them and press A.", 8000);
    LOG("[ghost] trade asked by the other player (code %08u)\n", code);
}
static void trade_frame(uint64_t me, uint64_t frame) {
    uint64_t n = g_stand_node;
    bool near = false;
    if (me && n && frame - g_peer_at < 30) {
        const float *p = (const float *)(uintptr_t)(me + 0x90), *t = (const float *)(uintptr_t)(n + 0xe0);
        near = (p[0] - t[0]) * (p[0] - t[0]) + (p[2] - t[2]) * (p[2] - t[2]) < TRADE_NEAR * TRADE_NEAR && fabsf(p[1] - t[1]) < 2;
    }
    bool busy = input_macro_busy();
    g_input_swallow = near && !busy ? BTN_A_BIT : 0;  // (the stand-in's own Talk never starts)
    static bool was;
    bool a = (input_buttons_now() & BTN_A_BIT) != 0, press = a && !was;
    was = a;
    if (g_trade && frame - g_trade_at > 60 * 120) {  // (two minutes: forgotten)
        if (g_trade == 1) menu_notice_for("No answer to the trade request.", 4000);
        g_trade = 0;
    }
    if (!near || busy || !press) return;
    if (g_trade == 2) {
        trade_send(1, g_trade_code);
        trade_start(g_trade_code);
    } else if (g_trade == 0) {
        g_trade_code = 10000000u + (uint32_t)(ghost_now_ns() / 1000 % 6777216u);
        g_trade = 1, g_trade_at = frame;
        trade_send(0, g_trade_code);
        menu_notice_for("Trade request sent. Waiting for the other player to walk up to you and press A.", 6000);
        LOG("[ghost] trade asked (code %08u)\n", g_trade_code);
    } else {
        trade_send(0, g_trade_code);  // (again, in case it was lost)
    }
}

void ghost_init(void) {
    const char *e = getenv("LARECOMP_GHOST");
    int mine = 0, peer = 0;
    char host[64] = "127.0.0.1";
    if (!e || sscanf(e, "%d,%d,%63s", &mine, &peer, host) < 2) return;
    g_ghost_fd = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = htons((uint16_t)mine), .sin_addr.s_addr = htonl(INADDR_ANY)};
    if (g_ghost_fd < 0 || bind(g_ghost_fd, (struct sockaddr *)&a, sizeof a)) { LOG("[ghost] can't bind %d\n", mine); return; }
    fcntl(g_ghost_fd, F_SETFL, O_NONBLOCK);
    g_ghost_peer.sin_family = AF_INET, g_ghost_peer.sin_port = htons((uint16_t)peer);
    inet_pton(AF_INET, host, &g_ghost_peer.sin_addr);
    g_off = getenv("LARECOMP_GHOST_OFFSET") ? (float)atof(getenv("LARECOMP_GHOST_OFFSET")) : 0;
    g_ghost_on = 1;
    rt_register_fnhook_at(NODE_UPDATE, node_hook);
    LOG("[ghost] on: port %d, peer %s:%d\n", mine, host, peer);
}
static void ghost_frame(uint64_t frame) {
    uint64_t me = player_body(frame);
    if (me != g_me_body) g_me_body = me, g_me_node = 0;
    uint64_t mn = g_me_node;
    if (mn && *(const uint64_t *)(uintptr_t)mn != NODE_VTABLE) g_me_node = mn = 0;
    if (me && mn) {  // (send ours: the body's position, the node's facing - a turn about y, quaternion xyzw at +0xd0)
        const float *b = (const float *)(uintptr_t)(me + 0x90), *q = (const float *)(uintptr_t)(mn + 0xd0);
        float out[5] = {2.0f, b[0], b[1], b[2], 2.0f * atan2f(q[1], q[3])};
        sendto(g_ghost_fd, out, sizeof out, 0, (struct sockaddr *)&g_ghost_peer, sizeof g_ghost_peer);
    }
    float in[5];
    while (recv(g_ghost_fd, in, sizeof in, 0) == (ssize_t)sizeof in) {
        if (in[0] == 2.0f) memcpy(g_peer, in + 1, 16), g_peer_at = frame;
        else if (in[0] == 3.0f || in[0] == 4.0f) trade_message(in[0] == 4.0f, (uint32_t)in[1], frame);
    }
    trade_frame(me, frame);
    if (g_stand_body && !is_body(g_stand_body)) {  // (gone with its level: another one)
        LOG("[ghost] the stand-in %llx is gone\n", (unsigned long long)g_stand_body);
        g_stand_body = g_stand_node = 0;
    }
    static uint64_t scanned_at;
    if (!g_stand_body && !g_scanning && me && frame - g_peer_at < 30 && (!scanned_at || frame - scanned_at > 600)) {
        pthread_t t;
        g_scanning = 1, g_scan_me = me, scanned_at = frame;
        if (pthread_create(&t, NULL, scan_bodies, NULL)) g_scanning = 0; else pthread_detach(t);
    }
    static uint64_t logged;
    if (!g_me_node && me && frame - logged >= 300) {
        logged = frame;
        const float *b = (const float *)(uintptr_t)(me + 0x90);
        LOG("[ghost] our body %llx at %.2f %.2f %.2f: its node not found yet\n", (unsigned long long)me, b[0], b[1], b[2]);
    }
    if (g_stand_node && frame - logged >= 300) {
        logged = frame;
        LOG("[ghost] peer at %.2f %.2f %.2f yaw %.2f\n", g_peer[0], g_peer[1], g_peer[2], g_peer[3]);
    }
}
static int ghost_step(Cpu *c, uint64_t addr, uint64_t o) { (void)c, (void)addr, (void)o; return 0; }
#else
void ghost_init(void) {}
static void ghost_frame(uint64_t frame) { (void)frame; }
static int ghost_step(Cpu *c, uint64_t addr, uint64_t o) { (void)c, (void)addr, (void)o; return 0; }
#endif
