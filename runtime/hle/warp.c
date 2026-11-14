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
#include "../platform.h"
#include "../settings.h"

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

static int ghost_step(Cpu *c, uint64_t addr, uint64_t o);  // (below: the shared world)
static volatile int g_world;   // (the shared world on: sending and showing - below)
static bool g_body_hooks;      // (body_hook registered)
static int body_hook(Cpu *c, uint64_t addr) {
    uint64_t o = c->x[0];
    if ((g_state < 0 || g_state == 3) && !g_log_every && !g_world) return 0;  // (off, or finished)
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
    if (g_world) { int r = ghost_step(c, addr, o); if (r) return r; }
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
    if (g_log_every || g_state == 0 || g_world) {
        g_body_hooks = true;
        rt_register_fnhook_at(STEP_A, body_hook);
        rt_register_fnhook_at(STEP_B, body_hook);
        LOG("[warp] %s%s\n", g_state == 0 ? "the player goes to " : "", g_state == 0 ? w : "logging the player's position");
    }
}

static void ghost_frame(uint64_t frame);
void warp_frame(uint64_t frame) {  // (each game frame: runtime/hle/input.c)
    g_frame = frame;
    ghost_frame(frame);
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

// ---- The shared world: other players walking around Lumiose (opt-in: larecomp.ini world_players = 1, the launcher's
// Mods page, with an online server - contrib/lzaserver - joined through the relay plugin; or LARECOMP_GHOST=<my port>,
// <peer port>[,<peer host>] for two copies over UDP, a test). Each copy sends its player's position, speed and facing
// ~20 times a second; the others show each player with a character of their own level standing in: a story/station NPC
// near our player (a character body 3-40 m away that isn't stepped - the crowd's models follow their own animation, not
// their bodies, 2026-11-13), moved through its transform node. A node (vtable 0x7003be0d28) keeps its local scale at
// +0xc0, rotation at +0xd0 (quaternion xyzw) and translation at +0xe0, and 0x7001ccf27c (x0 the node; called many times
// a frame) makes its world matrices (+0x40, +0x80) from them - writing the translation moves the model (found 2026-11-14
// by shifting every copy of a station NPC's x and halving: the world matrices themselves are overwritten). A
// character's own node is the one whose translation is its body's position. The stand-in keeps its own model, animation
// and collision (where it stood); its Talk prompt follows it - and A there asks that player to trade (below).
//   (The game's own remote player - Private Battles' - is an ordinary character whose movement comes from the network,
// 2026-11-14: a stand-in until that can be made in the field.)
#define NODE_VTABLE 0x7003be0d28ull
#define NODE_UPDATE 0x7001ccf27cull
#define MAXPEER 4
#define PEER_TIMEOUT_NS 5000000000ull
#ifdef _WIN32
#include <windows.h>
#endif
static uint64_t world_now_ns(void) { return plat_now_ns(); }
typedef struct {
    uint64_t id;                     // (the player's own random id; 0 a free slot)
    float pos[3], vel[3], yaw;       // (as last heard)
    float shown[3];                  // (where the stand-in is drawn: smoothed toward pos + vel * age)
    uint64_t heard_ns;
    volatile uint64_t body, node;    // (the stand-in's body, then its node)
} Peer;
static Peer g_peers[MAXPEER];
static uint64_t g_self;               // (this copy's id in the world)
static volatile int g_scanning;
static volatile uint64_t g_me_body, g_me_node;  // (our player's body and node: the node has the model's facing)
static bool g_hooked;

// ---- packets (both ways the same, little-endian): {u8 type, u8 version 1, u8[6], u64 from, ...}
//   type 2 where:  f32 x y z, f32 yaw, f32 vx vy vz
//   type 3 trade asked, 4 trade answered: u64 to, u32 code
typedef struct { uint8_t type, ver, pad[6]; uint64_t from; union { struct { float p[3], yaw, v[3]; } w; struct { uint64_t to; uint32_t code; } t; }; } WorldPkt;

// transport: the online server through the relay plugin, else (tests) UDP
static void (*w_send)(const void *, size_t);
static int (*w_recv)(uint64_t *, void *, size_t);
static void (*w_enable)(int);
static int (*w_avail)(void);
#ifndef _WIN32
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>
#include <fcntl.h>
static int g_udp = -1;
static struct sockaddr_in g_udp_peer;
static float g_off;  // (LARECOMP_GHOST_OFFSET=<m>: one copy shown beside itself, a loopback test)
static void udp_send(const void *p, size_t n) { sendto(g_udp, p, n, 0, (struct sockaddr *)&g_udp_peer, sizeof g_udp_peer); }
static int udp_recv(uint64_t *from, void *p, size_t cap) {
    ssize_t n = recv(g_udp, p, cap, 0);
    (void)from;
    return n > 0 ? (int)n : 0;
}
ssize_t process_vm_readv(pid_t, const struct iovec *, unsigned long, const struct iovec *, unsigned long, unsigned long);
#else
static const float g_off = 0;
#endif
static void world_send(const WorldPkt *k, size_t n) { if (w_send) w_send(k, n); }
#ifndef _WIN32
static bool g_udp_test(void) { return g_udp >= 0; }
#else
static bool g_udp_test(void) { return false; }
#endif

// guest memory read for the heap scan, an unmapped or uncommitted range: false
static bool heap_read(uint64_t a, void *out, size_t n) {
#ifdef _WIN32
    MEMORY_BASIC_INFORMATION mb;
    if (!VirtualQuery((const void *)(uintptr_t)a, &mb, sizeof mb) || mb.State != MEM_COMMIT ||
        (mb.Protect & (PAGE_NOACCESS | PAGE_GUARD)) || (uint64_t)(uintptr_t)mb.BaseAddress + mb.RegionSize < a + n) return false;
    memcpy(out, (const void *)(uintptr_t)a, n);
    return true;
#else
    struct iovec l = {out, n}, r = {(void *)(uintptr_t)a, n};
    return process_vm_readv(getpid(), &l, 1, &r, 1, 0) == (ssize_t)n;
#endif
}

static Peer *peer_of_node(uint64_t n) {
    for (int i = 0; i < MAXPEER; i++) if (g_peers[i].id && g_peers[i].node == n) return &g_peers[i];
    return NULL;
}
static int node_hook(Cpu *c, uint64_t addr) {
    (void)addr;
    uint64_t n = c->x[0];
    if (!g_world || n < HEAP_BASE || n >= HEAP_BASE + HEAP_MAX) return 0;
    Peer *p = peer_of_node(n);
    if (p) {
        if (*(const uint64_t *)(uintptr_t)n != NODE_VTABLE) return 0;
        float *q = (float *)(uintptr_t)(n + 0xd0), *t = (float *)(uintptr_t)(n + 0xe0);
        q[0] = 0, q[1] = sinf(p->yaw * 0.5f), q[2] = 0, q[3] = cosf(p->yaw * 0.5f);
        t[0] = p->shown[0] + g_off, t[1] = p->shown[1], t[2] = p->shown[2];
        return 0;
    }
    if (*(const uint64_t *)(uintptr_t)n != NODE_VTABLE) return 0;
    const void *nt = (const void *)(uintptr_t)(n + 0xe0);
    uint64_t mb = g_me_body;
    if (mb && !g_me_node && !memcmp(nt, (const void *)(uintptr_t)(mb + 0x90), 4) &&
        !memcmp((const void *)(uintptr_t)(n + 0xe8), (const void *)(uintptr_t)(mb + 0x98), 4)) {
        g_me_node = n;
        LOG("[world] our player's node %llx\n", (unsigned long long)n);
    }
    for (int i = 0; i < MAXPEER; i++) {  // (a stand-in's node: the one at its body's spot)
        Peer *s = &g_peers[i];
        uint64_t b = s->body;
        if (!s->id || !b || s->node) continue;
        if (!memcmp(nt, (const void *)(uintptr_t)(b + 0x90), 4) && !memcmp((const void *)(uintptr_t)(n + 0xe8), (const void *)(uintptr_t)(b + 0x98), 4)) {
            s->node = n;
            LOG("[world] player %016llx shown by the character at node %llx\n", (unsigned long long)s->id, (unsigned long long)n);
        }
    }
    return 0;
}

// every character body in the heap (a background thread): for each player without a stand-in, the nearest body 3-40 m
// from `me` that isn't stepped and isn't standing in already
static uint64_t g_scan_me;
static void *scan_bodies(void *arg) {
    (void)arg;
    uint64_t me = g_scan_me;
    float px = ((const float *)(uintptr_t)(me + 0x90))[0], pz = ((const float *)(uintptr_t)(me + 0x90))[2];
    enum { NC = 16 };
    uint64_t cand[NC];
    float cd[NC];
    int nc = 0, count = 0;
    const size_t chunk = 1u << 20;
    uint64_t *buf = malloc(chunk);
    for (uint64_t a = HEAP_BASE; buf && a < HEAP_BASE + HEAP_MAX; a += chunk) {
        if (!heap_read(a, buf, chunk)) continue;
        for (size_t k = 0; k + 0xa0 / 8 < chunk / 8; k++) {
            if (buf[k] != BODY_VTABLE) continue;
            uint64_t o = a + k * 8;
            if (o == me) continue;
            count++;
            const float *q = (const float *)((const char *)buf + k * 8 + 0x90);
            float d = sqrtf((q[0] - px) * (q[0] - px) + (q[2] - pz) * (q[2] - pz));
            if (!(d > 3 && d < 40)) continue;
            bool skip = false;
            pthread_mutex_lock(&g_body_lock);
            for (int i = 0; i < NBODY; i++) if (g_bodies[i].o == o && g_frame - g_bodies[i].last <= 30) skip = true;
            pthread_mutex_unlock(&g_body_lock);
            for (int i = 0; i < MAXPEER; i++) if (g_peers[i].body == o) skip = true;
            if (skip) continue;
            if (nc == NC && d >= cd[NC - 1]) continue;  // (kept sorted, nearest first)
            int j = nc < NC ? nc++ : NC - 1;
            while (j > 0 && cd[j - 1] > d) cand[j] = cand[j - 1], cd[j] = cd[j - 1], j--;
            cand[j] = o, cd[j] = d;
        }
    }
    free(buf);
    int used = 0;
    for (int i = 0; i < MAXPEER && used < nc; i++)
        if (g_peers[i].id && !g_peers[i].body) {
            g_peers[i].node = 0, g_peers[i].body = cand[used];
            LOG("[world] player %016llx: stand-in %llx, %.1f m from us (%d characters)\n", (unsigned long long)g_peers[i].id,
                (unsigned long long)cand[used], cd[used], count);
            used++;
        }
    g_scanning = 0;
    return NULL;
}

// ---- the walk-up trade: A by a player's stand-in (the stand-in's own Talk is kept from the game) asks that player; them
// doing the same answers. Both games then go to Link Play -> Link Trade -> Nearby Players with the same Link Code
// (pressed through runtime/hle/input.c's queue; the code typed by menu.mm's name entry; with an online server the relay
// plugin takes the code as the lobby, so the trade goes through the server too): the game's own trade from there.
#define TRADE_NEAR 2.5f
#define BTN_A_BIT 1ull
void input_macro(const char *lines);
bool input_macro_busy(void);
uint64_t input_buttons_now(void);
extern volatile uint64_t g_input_swallow;
void menu_notice_for(const char *text, unsigned ms);
extern char g_kbd_auto_once[32];
static int g_trade;            // 0 nothing, 1 we asked, 2 they asked
static uint64_t g_trade_with;  // (that player)
static uint32_t g_trade_code;
static uint64_t g_trade_at;
static void trade_send(int accept, uint64_t to, uint32_t code) {
    WorldPkt k = {.type = (uint8_t)(accept ? 4 : 3), .ver = 1, .from = g_self};
    k.t.to = to, k.t.code = code;
    for (int i = 0; i < 3; i++) world_send(&k, 16 + 12);
}
static void trade_start(uint32_t code) {
    snprintf(g_kbd_auto_once, sizeof g_kbd_auto_once, "%08u", code);
    // X (menu), up to Link Play, A; right twice to Link Trade, A; Nearby Players, A; the Link Code entry answers itself
    input_macro("200ms -\n170ms X\n2500ms -\n170ms UP\n500ms -\n170ms A\n4000ms -\n170ms RIGHT\n500ms -\n170ms RIGHT\n"
                "500ms -\n170ms A\n3000ms -\n170ms A\n1000ms -\n");
    char t[96];
    snprintf(t, sizeof t, "Trading with the other player (Link Code %04u %04u)", code / 10000, code % 10000);
    menu_notice_for(t, 8000);
    LOG("[world] trade: Link Code %08u\n", code);
    g_trade = 0;
}
static void trade_message(const WorldPkt *k, uint64_t frame) {
    if (k->t.to != g_self) return;
    if (k->type == 4) {
        if (g_trade == 1 && k->from == g_trade_with && k->t.code == g_trade_code) trade_start(k->t.code);
        return;
    }
    if (g_trade == 2 && k->from == g_trade_with && k->t.code == g_trade_code) return;  // (a repeat)
    if (g_trade == 1 && k->from == g_trade_with && k->t.code > g_trade_code) return;  // (both asked at once: the lower code)
    g_trade = 2, g_trade_with = k->from, g_trade_code = k->t.code, g_trade_at = frame;
    menu_notice_for("Another player wants to trade! Walk up to them and press A.", 8000);
    LOG("[world] trade asked by %016llx (code %08u)\n", (unsigned long long)k->from, k->t.code);
}
static void trade_frame(uint64_t me, uint64_t frame) {
    Peer *by = NULL;
    if (me)
        for (int i = 0; i < MAXPEER; i++) {
            Peer *p = &g_peers[i];
            if (!p->id || !p->node) continue;
            const float *a = (const float *)(uintptr_t)(me + 0x90);
            float dx = a[0] - p->shown[0] - g_off, dz = a[2] - p->shown[2];
            if (dx * dx + dz * dz < TRADE_NEAR * TRADE_NEAR && fabsf(a[1] - p->shown[1]) < 2) by = p;
        }
    bool busy = input_macro_busy();
    g_input_swallow = by && !busy ? BTN_A_BIT : 0;  // (the stand-in's own Talk never starts)
    static bool was;
    bool a = (input_buttons_now() & BTN_A_BIT) != 0, press = a && !was;
    was = a;
    if (g_trade && frame - g_trade_at > 60 * 120) {  // (two minutes: forgotten)
        if (g_trade == 1) menu_notice_for("No answer to the trade request.", 4000);
        g_trade = 0;
    }
    bool theirs = by && g_trade && by->id == g_trade_with;
    static uint64_t hint_at;  // (by a stand-in: what A does there, in the corner while by)
    if (by && !busy && frame - hint_at >= 30) {
        hint_at = frame;
        menu_notice_for(theirs && g_trade == 2 ? "A: accept this player's trade" : theirs && g_trade == 1 ? "Trade asked: waiting for them" :
                        "A: ask this player to trade", 700);
    }
    if (!by || busy || !press) return;
    if (theirs && g_trade == 2) {
        trade_send(1, g_trade_with, g_trade_code);
        trade_start(g_trade_code);
    } else if (!theirs || g_trade == 0) {
        g_trade_code = 10000000u + (uint32_t)(world_now_ns() / 1000 % 89999999u);
        g_trade = 1, g_trade_with = by->id, g_trade_at = frame;
        trade_send(0, g_trade_with, g_trade_code);
        menu_notice_for("Trade request sent. Waiting for them to walk up to you and press A.", 6000);
        LOG("[world] trade asked of %016llx (code %08u)\n", (unsigned long long)g_trade_with, g_trade_code);
    } else {
        trade_send(0, g_trade_with, g_trade_code);  // (again, in case it was lost)
    }
}

static void hooks_on(void) {
    if (g_hooked) return;
    g_hooked = true;
    rt_register_fnhook_at(NODE_UPDATE, node_hook);
    if (!g_body_hooks) g_body_hooks = true, rt_register_fnhook_at(STEP_A, body_hook), rt_register_fnhook_at(STEP_B, body_hook);
}
void ghost_init(void) {  // (the test transport: LARECOMP_GHOST=<my port>,<peer port>[,<peer host>], Linux)
    if (!g_self) {  // (random: the clock, this copy's stack address and its process, mixed - splitmix64)
#ifdef _WIN32
        uint64_t pid = GetCurrentProcessId();
#else
        uint64_t pid = (uint64_t)getpid();
#endif
        uint64_t x = plat_now_ns() ^ (uint64_t)(uintptr_t)&pid * 0x9E3779B97F4A7C15ull ^ pid << 32;
        x += 0x9E3779B97F4A7C15ull, x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull, x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
        g_self = (x ^ (x >> 31)) | 1;
    }
#ifndef _WIN32
    const char *e = getenv("LARECOMP_GHOST");
    int mine = 0, peer = 0;
    char host[64] = "127.0.0.1";
    if (!e || sscanf(e, "%d,%d,%63s", &mine, &peer, host) < 2) return;
    g_udp = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = htons((uint16_t)mine), .sin_addr.s_addr = htonl(INADDR_ANY)};
    if (g_udp < 0 || bind(g_udp, (struct sockaddr *)&a, sizeof a)) { LOG("[world] can't bind UDP %d\n", mine); return; }
    fcntl(g_udp, F_SETFL, O_NONBLOCK);
    g_udp_peer.sin_family = AF_INET, g_udp_peer.sin_port = htons((uint16_t)peer);
    inet_pton(AF_INET, host, &g_udp_peer.sin_addr);
    g_off = getenv("LARECOMP_GHOST_OFFSET") ? (float)atof(getenv("LARECOMP_GHOST_OFFSET")) : 0;
    w_send = udp_send, w_recv = udp_recv;
    g_world = 1;
    LOG("[world] test transport: UDP %d -> %s:%d\n", mine, host, peer);
#endif
}

// the online server's shared world, when the setting is on and the relay plugin has a server (checked once a second)
static void world_transport(uint64_t frame) {
    static uint64_t checked;
    if (g_udp_test() || frame - checked < 60) return;
    checked = frame;
    void *plugins_symbol(const char *name);
    if (!w_avail) {
        w_avail = (int (*)(void))plugins_symbol("larecomp_world_available");
        w_enable = (void (*)(int))plugins_symbol("larecomp_world_enable");
        w_send = (void (*)(const void *, size_t))plugins_symbol("larecomp_world_send");
        w_recv = (int (*)(uint64_t *, void *, size_t))plugins_symbol("larecomp_world_recv");
        if (!w_avail || !w_enable || !w_send || !w_recv) { w_avail = NULL, w_send = NULL, w_recv = NULL; return; }
    }
    int want = g_settings.world_players && w_avail();
    if (want == g_world) return;
    w_enable(want);
    g_world = want;
    LOG("[world] %s\n", want ? "on: other players on the online server are shown" : "off");
    if (!want) memset(g_peers, 0, sizeof g_peers);  // (their stand-ins go back where they stood)
}

static void ghost_frame(uint64_t frame) {
    world_transport(frame);
    if (!g_world) return;
    hooks_on();
    uint64_t now = world_now_ns();
    uint64_t me = player_body(frame);
    if (me != g_me_body) g_me_body = me, g_me_node = 0;
    uint64_t mn = g_me_node;
    if (mn && *(const uint64_t *)(uintptr_t)mn != NODE_VTABLE) g_me_node = mn = 0;
    static uint64_t sent_ns;
    static float last[3];
    if (me && mn && now - sent_ns >= 50000000ull) {  // (ours, 20 a second: the body's position, the node's facing)
        const float *b = (const float *)(uintptr_t)(me + 0x90), *q = (const float *)(uintptr_t)(mn + 0xd0);
        float dt = sent_ns ? (float)(now - sent_ns) / 1e9f : 1;
        WorldPkt k = {.type = 2, .ver = 1, .from = g_self};
        for (int i = 0; i < 3; i++) k.w.p[i] = b[i], k.w.v[i] = dt < 0.5f ? (b[i] - last[i]) / dt : 0, last[i] = b[i];
        k.w.yaw = 2.0f * atan2f(q[1], q[3]);
        world_send(&k, 16 + 28);
        sent_ns = now;
    }
    WorldPkt in;
    uint64_t via;
    int n;
    while (w_recv && (n = w_recv(&via, &in, sizeof in)) >= 16) {
        if (in.ver != 1 || in.from == g_self) continue;
        if (in.type == 3 || in.type == 4) { if (n >= 28) trade_message(&in, frame); continue; }
        if (in.type != 2 || n < 44) continue;
        Peer *p = NULL, *free_p = NULL;
        for (int i = 0; i < MAXPEER; i++) {
            if (g_peers[i].id == in.from) p = &g_peers[i];
            else if (!g_peers[i].id && !free_p) free_p = &g_peers[i];
        }
        if (!p && free_p) {
            p = free_p;
            memset(p, 0, sizeof *p);
            p->id = in.from;
            memcpy(p->shown, in.w.p, 12);
            LOG("[world] player %016llx is here\n", (unsigned long long)in.from);
            menu_notice_for("Another player is nearby.", 3000);
        }
        if (!p) continue;  // (more players than stand-ins)
        memcpy(p->pos, in.w.p, 12), memcpy(p->vel, in.w.v, 12), p->yaw = in.w.yaw, p->heard_ns = now;
    }
    bool want_scan = false;
    for (int i = 0; i < MAXPEER; i++) {
        Peer *p = &g_peers[i];
        if (!p->id) continue;
        if (now - p->heard_ns > PEER_TIMEOUT_NS) {  // (gone quiet: their stand-in goes back)
            LOG("[world] player %016llx left\n", (unsigned long long)p->id);
            memset(p, 0, sizeof *p);
            continue;
        }
        if (p->body && !is_body(p->body)) p->body = p->node = 0;  // (the stand-in went with its level)
        if (!p->body) want_scan = true;
        float age = (float)(now - p->heard_ns) / 1e9f;  // (shown: toward where they should be now, smoothly)
        if (age > 0.25f) age = 0.25f;
        for (int k = 0; k < 3; k++) {
            float target = p->pos[k] + p->vel[k] * age, d = target - p->shown[k];
            p->shown[k] = fabsf(d) > 8 ? target : p->shown[k] + d * 0.35f;  // (a jump of 8 m - a warp: at once)
        }
    }
    trade_frame(me, frame);
    static uint64_t scanned_at;
    if (want_scan && !g_scanning && me && mn && (!scanned_at || frame - scanned_at > 600)) {
        pthread_t t;
        g_scanning = 1, g_scan_me = me, scanned_at = frame;
        if (pthread_create(&t, NULL, scan_bodies, NULL)) g_scanning = 0; else pthread_detach(t);
    }
}
static int ghost_step(Cpu *c, uint64_t addr, uint64_t o) { (void)c, (void)addr, (void)o; return 0; }
