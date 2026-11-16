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
// a character's part (its look, below: the townspeople dressed in a player's outfit): {name, .ccdatam, variant}
typedef struct { char name[32], path[96], var[96]; } DressPart;
// a player's real look (below, the save's dress-up): its 13 parts' variants (hashes) and 18 customisation options
typedef struct { uint64_t part[13]; uint8_t opt[18], f1, f2; } Look;
#define MAXPARTS 16
typedef struct {
    uint64_t id;                     // (the player's own random id; 0 a free slot)
    float pos[3], vel[3], yaw;       // (as last heard)
    float shown[3];                  // (where the stand-in is drawn: smoothed toward pos + vel * age)
    uint64_t heard_ns;
    volatile uint64_t body, node;    // (the stand-in's body, then its node)
    // their outfit (type 5 packets, part k of n): once every part is heard, the next townsperson made is dressed in
    // it and stands in for them from then on (dressed_*: that character, the outfit it wears)
    DressPart parts[MAXPARTS];
    int nparts, body_type;
    uint32_t have, hash, dressed_hash, armed_hash;
    uint64_t dressed_owner, dressed_body, dressed_at;
    uint64_t saved_node;             // (the stand-in's node as it was - put back when another one takes over / they leave)
    float saved_q[4], saved_t[3];
    Look look;                       // (their real look: type 6 packets, 2 pieces; worn by their dressed character once whole)
    uint32_t look_have, look_hash;
} Peer;
static bool look_ours(Look *l);
// the shared world's dressing: the next townsperson made wears `want` (its player then waits for it); each one made
// is kept as {player, owner (the request's +0x30: the character's object), outfit} until its body is found
static pthread_mutex_t g_dress_lock = PTHREAD_MUTEX_INITIALIZER;
static struct { uint64_t peer; DressPart parts[MAXPARTS]; int n, body; uint32_t hash; Look look; } g_dress_want;
static struct { uint64_t peer, owner; uint32_t hash; } g_dressed[8];
static int g_ndressed;
static DressPart g_pparts[24];   // (our player's parts, as its character was made; g_npparts of them)
static int g_npparts, g_pbody = -1;
static uint32_t outfit_hash(const DressPart *p, int n) {
    uint32_t h = 2166136261u;
    for (int k = 0; k < n; k++)
        for (const char *s[3] = {p[k].name, p[k].path, p[k].var}, **q = s; q < s + 3; q++)
            for (const char *c = *q; ; c++) { h = (h ^ (uint8_t)*c) * 16777619u; if (!*c) break; }
    return h;
}
static Peer g_peers[MAXPEER];
static uint64_t g_self;               // (this copy's id in the world)
static volatile int g_scanning;
static volatile uint64_t g_me_body, g_me_node;  // (our player's body and node: the node has the model's facing)
static bool g_hooked;

// ---- packets (both ways the same, little-endian): {u8 type, u8 version 1, u8[6], u64 from, ...}
//   type 2 where:  f32 x y z, f32 yaw, f32 vx vy vz
//   type 3 trade asked, 4 trade answered: u64 to, u32 code
//   type 5 outfit: u8 part k, u8 parts n, u8 body type, then three strings (NUL-ended): the part's name, its .ccdatam
//          ("ik_chara/<dir>/<last>.ccdatam" sent as "<dir>"), its variant (the item and colour); 96 bytes at most
//   type 6 look: u8 piece k of n (2), u8[6], 64 bytes of a Look (the save's dress-up: part hashes, options)
typedef struct { uint8_t type, ver, pad[6]; uint64_t from; union { struct { float p[3], yaw, v[3]; } w; struct { uint64_t to; uint32_t code; } t;
    struct { uint8_t k, n, body; char s[77]; } o; struct { uint8_t k, n, pad[6]; uint8_t d[64]; } l; }; } WorldPkt;

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
static int g_self_test;  // (LARECOMP_GHOST_SELF=1: our own packets count as another player's - one copy, a loopback test)
static int g_crowd_ok;   // (LARECOMP_GHOST_CROWD=1: a stepped body - the crowd's - may stand in too; 2: only those, research)
static void udp_send(const void *p, size_t n) { sendto(g_udp, p, n, 0, (struct sockaddr *)&g_udp_peer, sizeof g_udp_peer); }
static int udp_recv(uint64_t *from, void *p, size_t cap) {
    ssize_t n = recv(g_udp, p, cap, 0);
    (void)from;
    return n > 0 ? (int)n : 0;
}
ssize_t process_vm_readv(pid_t, const struct iovec *, unsigned long, const struct iovec *, unsigned long, unsigned long);
#else
static const float g_off = 0;
static const int g_self_test = 0, g_crowd_ok = 0;
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
        if (p->saved_node != n) memcpy(p->saved_q, q, 16), memcpy(p->saved_t, t, 12), p->saved_node = n;
        q[0] = 0, q[1] = sinf(p->yaw * 0.5f), q[2] = 0, q[3] = cosf(p->yaw * 0.5f);
        t[0] = p->shown[0] + g_off, t[1] = p->shown[1], t[2] = p->shown[2];
        static int animvel = -1;  // (research LARECOMP_WORLD_ANIMVEL=1: their speed written into the stand-in's body)
        if (animvel < 0) animvel = getenv("LARECOMP_WORLD_ANIMVEL") ? atoi(getenv("LARECOMP_WORLD_ANIMVEL")) : 0;
        uint64_t b = p->body;
        if (animvel && b && is_body(b)) {
            float *v = (float *)(uintptr_t)(b + 0xc0), *d = (float *)(uintptr_t)(b + 0xd0), *v2 = (float *)(uintptr_t)(b + 0xe0);
            for (int k = 0; k < 3; k++) v[k] = v2[k] = k == 1 ? 0 : p->vel[k], d[k] = v[k] / 60.0f;
        }
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
            bool stepped = false;
            for (int i = 0; i < NBODY; i++) if (g_bodies[i].o == o && g_frame - g_bodies[i].last <= 30) stepped = true;
            if (g_crowd_ok == 2 ? !stepped : stepped && !g_crowd_ok) skip = true;
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

// a stand-in let go: its node back where it stood (a station NPC's isn't written again by the game: it stayed where
// the player was last seen, 2026-11-16) - unless it went with its level
static void standin_release(Peer *p) {
    uint64_t n = p->saved_node, b = p->body;
    p->node = 0, p->saved_node = 0;
    if (!n || !b || !is_body(b) || *(const uint64_t *)(uintptr_t)n != NODE_VTABLE) return;
    memcpy((void *)(uintptr_t)(n + 0xd0), p->saved_q, 16), memcpy((void *)(uintptr_t)(n + 0xe0), p->saved_t, 12);
}

// ---- outfits: ours sent part by part (type 5), theirs kept per player; a player whose whole outfit is heard gets the
// next townsperson made dressed in it (below, dress_hook), which then stands in for them
static uint64_t dressed_body_of(uint64_t owner);
static void dress_hook_on(void);
// our outfit as sent: our player's parts; research LARECOMP_WORLD_OUTFIT="<part>=<variant>[;...]" changes some
// (a test: two copies of one save then look different to each other)
static int outfit_ours(DressPart *out, int *body) {
    pthread_mutex_lock(&g_dress_lock);
    int n = g_npparts < MAXPARTS ? g_npparts : MAXPARTS;
    memcpy(out, g_pparts, sizeof *out * (size_t)n);
    *body = g_pbody;
    pthread_mutex_unlock(&g_dress_lock);
    const char *e = getenv("LARECOMP_WORLD_OUTFIT");
    for (const char *q = e; q && *q;) {
        char name[32] = "", var[96] = "";
        if (sscanf(q, "%31[^=;]=%95[^;]", name, var) == 2)
            for (int k = 0; k < n; k++) if (!strcmp(out[k].name, name)) snprintf(out[k].var, sizeof out[k].var, "%s", var);
        q = strchr(q, ';');
        if (q) q++;
    }
    return n;
}
// a part as a type 5 packet's strings: "ik_chara/<dir>/<last>.ccdatam" goes as "<dir>" (0: too long)
static size_t outfit_put(WorldPkt *k, const DressPart *p) {
    char dir[96];
    const char *path = p->path, *slash = strrchr(path, '/');
    size_t pl = strlen(path);
    snprintf(dir, sizeof dir, "%s", path);
    if (!strncmp(path, "ik_chara/", 9) && slash && pl > 8 && !strcmp(path + pl - 8, ".ccdatam")) {
        size_t dl = (size_t)(slash - path) - 9, ll = (size_t)(path + pl - 8 - (slash + 1));
        const char *dirlast = path + 9;
        for (const char *c = path + 9; c < path + 9 + dl; c++) if (*c == '/') dirlast = c + 1;
        if ((size_t)(slash - dirlast) == ll && !memcmp(dirlast, slash + 1, ll)) snprintf(dir, sizeof dir, "%.*s", (int)dl, path + 9);
    }
    size_t a = strlen(p->name) + 1, b = strlen(dir) + 1, c = strlen(p->var) + 1;
    if (a + b + c > sizeof k->o.s) return 0;
    memcpy(k->o.s, p->name, a), memcpy(k->o.s + a, dir, b), memcpy(k->o.s + a + b, p->var, c);
    return 16 + 3 + a + b + c;
}
static bool outfit_get(const WorldPkt *k, size_t n, DressPart *p) {
    if (n <= 16 + 3) return false;
    const char *s = k->o.s, *end = (const char *)k + n, *f[3];
    for (int i = 0; i < 3; i++) {
        f[i] = s;
        while (s < end && *s) s++;
        if (s >= end) return false;
        s++;
    }
    snprintf(p->name, sizeof p->name, "%s", f[0]), snprintf(p->var, sizeof p->var, "%s", f[2]);
    if (strstr(f[1], ".ccdatam")) snprintf(p->path, sizeof p->path, "%s", f[1]);
    else {
        const char *last = strrchr(f[1], '/');
        snprintf(p->path, sizeof p->path, "ik_chara/%s/%s.ccdatam", f[1], last ? last + 1 : f[1]);
    }
    return true;
}
static void outfit_message(const WorldPkt *k, size_t n) {
    Peer *p = NULL;
    for (int i = 0; i < MAXPEER; i++) if (g_peers[i].id == k->from) p = &g_peers[i];
    if (!p || k->o.n == 0 || k->o.n > MAXPARTS || k->o.k >= k->o.n) return;  // (not heard where they are yet: resent)
    if (p->nparts != k->o.n) p->nparts = k->o.n, p->have = 0;
    if (!outfit_get(k, n, &p->parts[k->o.k])) return;
    p->body_type = k->o.body == 0xff ? -1 : k->o.body;
    p->have |= 1u << k->o.k;
    if (p->have != (1u << p->nparts) - 1) return;
    uint32_t h = outfit_hash(p->parts, p->nparts);
    if (h == p->hash) return;
    p->hash = h;
    LOG("[world] player %016llx's outfit: %d parts (%08x), hat %s, top %s\n", (unsigned long long)p->id, p->nparts, h,
        p->nparts > 6 ? p->parts[6].var : "?", p->parts[p->nparts - 1].var);
}
// (LARECOMP_GHOST_SELF, one copy: we are the other player too - our first townsperson is dressed for "them" at once)
void world_self_outfit(void) {
    if (!g_self_test) return;
    DressPart o[MAXPARTS];
    int body, n = outfit_ours(o, &body);
    pthread_mutex_lock(&g_dress_lock);
    Look l;
    if (!look_ours(&l)) { pthread_mutex_unlock(&g_dress_lock); return; }  // (the save's look isn't there yet: as packets)
    g_dress_want.peer = g_self ^ 2, g_dress_want.n = n, g_dress_want.body = body, g_dress_want.look = l;
    uint32_t lh = 2166136261u;
    for (size_t b = 0; b < sizeof l; b++) lh = (lh ^ ((const uint8_t *)&l)[b]) * 16777619u;
    g_dress_want.hash = outfit_hash(o, n) ^ lh;
    memcpy(g_dress_want.parts, o, sizeof o);
    pthread_mutex_unlock(&g_dress_lock);
}
// a player's dressed character: asked for once their outfit is whole, then found (its body) - it stands in from then on;
// gone (its level, the crowd took it back) - asked for again
static bool body_stepped(uint64_t b, uint64_t frame) {  // (the crowd's and the player's bodies are stepped)
    bool st = false;
    pthread_mutex_lock(&g_body_lock);
    for (int i = 0; i < NBODY; i++) if (g_bodies[i].o == b && frame - g_bodies[i].last <= 30) st = true;
    pthread_mutex_unlock(&g_body_lock);
    return st;
}
static void dressed_drop(uint64_t owner) {
    pthread_mutex_lock(&g_dress_lock);
    for (int i = 0; i < g_ndressed; i++) if (g_dressed[i].owner == owner) g_dressed[i] = g_dressed[--g_ndressed], i--;
    pthread_mutex_unlock(&g_dress_lock);
}
static void outfit_frame(Peer *p, uint64_t frame) {
    if (g_crowd_ok == 2) return;  // (research: the crowd's stand-ins only)
    if (p->dressed_body && body_stepped(p->dressed_body, frame)) {  // (one of the crowd: its model follows the crowd, not
        LOG("[world] player %016llx's dressed character walks with the crowd: another one asked for\n", (unsigned long long)p->id);
        dressed_drop(p->dressed_owner);                             // its node - it can't stand in, 2026-11-16)
        if (p->body == p->dressed_body) standin_release(p), p->body = 0;
        p->dressed_body = p->dressed_owner = 0, p->dressed_hash = p->armed_hash = 0;
    }
    if (p->dressed_body && (!is_body(p->dressed_body) || dressed_body_of(p->dressed_owner) != p->dressed_body)) {
        LOG("[world] player %016llx's dressed character is gone\n", (unsigned long long)p->id);
        if (p->body == p->dressed_body) p->body = p->node = 0;
        p->dressed_body = p->dressed_owner = 0, p->dressed_hash = p->armed_hash = 0;
    }
    // (their outfit and their look - the save's dress-up, worn as the character is made - both heard: the key of what
    // their character must wear)
    uint32_t key = p->hash && p->look_hash ? p->hash ^ p->look_hash : 0;
    if (!key || p->dressed_hash == key) return;
    uint64_t owner = 0;
    pthread_mutex_lock(&g_dress_lock);
    for (int i = 0; i < g_ndressed; i++) if (g_dressed[i].peer == p->id && g_dressed[i].hash == key) owner = g_dressed[i].owner;
    if (!owner && p->armed_hash != key && !g_dress_want.peer) {  // (the next townsperson made: theirs)
        g_dress_want.peer = p->id, g_dress_want.n = p->nparts, g_dress_want.body = p->body_type, g_dress_want.hash = key;
        g_dress_want.look = p->look;
        memcpy(g_dress_want.parts, p->parts, sizeof p->parts);
        p->armed_hash = key, p->dressed_at = 0;  // (the wait for its body starts once it is made)
        LOG("[world] player %016llx: the next townsperson made wears their outfit\n", (unsigned long long)p->id);
    }
    pthread_mutex_unlock(&g_dress_lock);
    if (!owner) return;
    uint64_t b = dressed_body_of(owner);
    if (!b) {
        if (!p->dressed_at) p->dressed_at = frame;
        if (frame - p->dressed_at > 1200) dressed_drop(owner), p->armed_hash = 0, p->dressed_at = 0;  // (never placed in 20 s)
        return;
    }
    if (body_stepped(b, frame)) {  // (one of the crowd: see above)
        LOG("[world] player %016llx's dressed character walks with the crowd: another one asked for\n", (unsigned long long)p->id);
        dressed_drop(owner), p->armed_hash = 0, p->dressed_at = 0;
        return;
    }
    p->dressed_hash = key, p->dressed_owner = owner, p->dressed_body = b, p->dressed_at = 0;
    if (p->body != b) standin_release(p);  // (the plain stand-in goes back)
    p->body = b, p->node = 0;  // (its node: found by node_hook at the body's spot)
    pthread_mutex_lock(&g_dress_lock);
    for (int i = 0; i < g_ndressed; i++) if (g_dressed[i].peer == p->id && g_dressed[i].owner != owner) g_dressed[i] = g_dressed[--g_ndressed], i--;
    pthread_mutex_unlock(&g_dress_lock);
    LOG("[world] player %016llx is shown by a townsperson in their outfit (body %llx)\n", (unsigned long long)p->id, (unsigned long long)b);
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
    g_self_test = getenv("LARECOMP_GHOST_SELF") != NULL, g_crowd_ok = getenv("LARECOMP_GHOST_CROWD") ? atoi(getenv("LARECOMP_GHOST_CROWD")) : 0;
    w_send = udp_send, w_recv = udp_recv;
    g_world = 1;
    dress_hook_on();  // (our player's outfit is kept as its character is made: before the world is up)
    LOG("[world] test transport: UDP %d -> %s:%d\n", mine, host, peer);
#endif
}

// the online server's shared world, when the setting is on and the relay plugin has a server (checked once a second)
static void world_transport(uint64_t frame) {
    static uint64_t checked;
    if (g_udp_test() || frame - checked < 60) return;
    checked = frame;
    if (g_settings.world_players) dress_hook_on();  // (our player's outfit: kept as its character is made)
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
    if (!want) {  // (their stand-ins go back where they stood)
        for (int i = 0; i < MAXPEER; i++) if (g_peers[i].id) standin_release(&g_peers[i]);
        memset(g_peers, 0, sizeof g_peers);
    }
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
    Look ours;
    if (me && frame % 60 == 30 && look_ours(&ours))  // (our real look, every second)
        for (int k = 0; k < 2; k++) {
            WorldPkt w = {.type = 6, .ver = 1, .from = g_self};
            w.l.k = (uint8_t)k, w.l.n = 2;
            size_t off = (size_t)k * 64, len = sizeof ours - off < 64 ? sizeof ours - off : 64;
            memcpy(w.l.d, (const uint8_t *)&ours + off, len);
            world_send(&w, 16 + 8 + 64);
        }
    if (me && frame % 8 == 0) {  // (our outfit, a part every 8 frames: the whole of it every ~2 s)
        static int part;
        DressPart o[MAXPARTS];
        int body, n = outfit_ours(o, &body);
        if (n) {
            part = (part + 1) % n;
            WorldPkt k = {.type = 5, .ver = 1, .from = g_self};
            k.o.k = (uint8_t)part, k.o.n = (uint8_t)n, k.o.body = (uint8_t)(body < 0 ? 0xff : body);
            size_t len = outfit_put(&k, &o[part]);
            if (len) world_send(&k, len);
        }
    }
    WorldPkt in;
    uint64_t via;
    int n;
    while (w_recv && (n = w_recv(&via, &in, sizeof in)) >= 16) {
        if (g_self_test && in.from == g_self) in.from = g_self ^ 2;  // (ourselves, as someone else)
        if (in.ver != 1 || in.from == g_self) continue;
        if (in.type == 3 || in.type == 4) { if (n >= 28) trade_message(&in, frame); continue; }
        if (in.type == 5) { outfit_message(&in, (size_t)n); continue; }
        if (in.type == 6) {
            if (n < 16 + 8 + 64 || in.l.n != 2 || in.l.k >= 2) continue;
            for (int i = 0; i < MAXPEER; i++) {
                Peer *p = &g_peers[i];
                if (p->id != in.from) continue;
                size_t off = (size_t)in.l.k * 64, len = sizeof p->look - off < 64 ? sizeof p->look - off : 64;
                memcpy((uint8_t *)&p->look + off, in.l.d, len);
                p->look_have |= 1u << in.l.k;
                if (p->look_have == 3) {
                    uint32_t h = 2166136261u;
                    for (size_t b = 0; b < sizeof p->look; b++) h = (h ^ ((const uint8_t *)&p->look)[b]) * 16777619u;
                    if (h != p->look_hash) p->look_hash = h, LOG("[world] player %016llx's look heard (skin %u, eyes %u, hair colour %u)\n",
                        (unsigned long long)p->id, p->look.opt[0], p->look.opt[3], p->look.opt[11]);
                }
            }
            continue;
        }
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
            standin_release(p);
            memset(p, 0, sizeof *p);
            continue;
        }
        outfit_frame(p, frame);
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


// ---- Dressed townspeople: the other players' look. A character's look is its template's
// CharacterCreationMasterComponent (FlatBuffers), given to the CharacterCreationMaster make (0x70011164d0, x2 a request
// {u64 description, ..., +0x18 its name's length (6: "Player"), +0x30 the character's object}): 0 its name, 2-12
// distances (20, 40, 60...), 14 a body type (the player 1 - so are the teenage boys, schoolkids_m / hope_m / child_a_m;
// the women 6, ...), 15 its parts - tables {0 name, 1 .ccdatam path, 2 variant} - and 16 (4). A townsperson has one part
// ("default", savm_cc_vr/trNNNN.ccdatam: one .ccdata of a whole body), the player 13 (base = savm_pc_base, then acc, bag,
// eyw, ftw, glv, hdw, hrs, lgw, drs, face, lwb, upb from savm_pc; the variant is the item and its colour, e.g.
// "p0_hdw0000_00_default_normal_08"); everything else is the same. A townsperson's description copied into guest memory
// with field 15 pointed at a player's parts - the player's "base" part named "default", the part a townsperson's other
// components wait for (named "base", the level never finished loading) - is made, drawn and animated like any
// townsperson (2026-11-16: 38 in Lumiose, standing and walking): a player-looking character. Its object (the request's
// +0x30, vtable 0x7003be7358) leads to its body: +0x1a0 -> +0x8 -> +0x138.
//   The shared world (above) asks for one per player (g_dress_want: their outfit, heard as type 5 packets); research:
// LARECOMP_DRESS_NPC=<n>[,<mode>[,<body>]] - the next n townspeople wear our own player's outfit: mode 0 the parts as
// the player's (never loads), 1 the townsperson's own part first, 2 "base" named "default"; <body>: only that body type.
#define CC_MAKE 0x70011164d0ull
#define CHAR_OBJ_VTABLE 0x7003be7358ull
static int g_dress_left = -1, g_dress_mode, g_dress_body = -1;
static uint32_t fb_u32(const uint8_t *b, uint64_t o) { uint32_t v; memcpy(&v, b + o, 4); return v; }
static const uint8_t *fb_field(const uint8_t *b, uint64_t tab, int f) {  // (the field's address in the table, NULL absent)
    int32_t so; memcpy(&so, b + tab, 4);
    const uint8_t *vt = b + tab - so;
    uint16_t vl; memcpy(&vl, vt, 2);
    if (4 + 2 * f >= vl) return NULL;
    uint16_t fo; memcpy(&fo, vt + 4 + 2 * f, 2);
    return fo ? b + tab + fo : NULL;
}
static const char *fb_str(const uint8_t *fieldp) { uint32_t o; memcpy(&o, fieldp, 4); return (const char *)(fieldp + o + 4); }
static void part_of(const uint8_t *b, const uint8_t *t, DressPart *p) {
    const uint8_t *p0 = fb_field(b, (uint64_t)(t - b), 0), *p1 = fb_field(b, (uint64_t)(t - b), 1), *p2 = fb_field(b, (uint64_t)(t - b), 2);
    snprintf(p->name, sizeof p->name, "%s", p0 ? fb_str(p0) : ""), snprintf(p->path, sizeof p->path, "%s", p1 ? fb_str(p1) : "");
    snprintf(p->var, sizeof p->var, "%s", p2 ? fb_str(p2) : "");
}
// a parts list written at d + w: [vector][vtable + table per part][strings] (FlatBuffers offsets point forward); the
// vector's position
static uint32_t fb_put_parts(uint8_t *d, uint32_t w, const DressPart *parts, int np) {
    uint32_t vbase = w = (w + 3) & ~3u, tabs[32];
    memcpy(d + w, &(uint32_t){(uint32_t)np}, 4);
    w += 4 + 4 * np;
    for (int k = 0; k < np; k++) {
        w = (w + 3) & ~3u;
        uint16_t vt[5] = {10, 16, 4, 8, 12};  // (vtable: its size, the table's, fields 0..2 at +4 +8 +12)
        uint32_t vto = w;
        memcpy(d + w, vt, 10), w += 12;
        tabs[k] = w;
        memcpy(d + w, &(int32_t){(int32_t)(tabs[k] - vto)}, 4), w += 16;
        memcpy(d + vbase + 4 + 4 * k, &(uint32_t){tabs[k] - (vbase + 4 + 4 * k)}, 4);
    }
    for (int k = 0; k < np; k++) {
        const char *s[3] = {parts[k].name, parts[k].path, parts[k].var};
        for (int j = 0; j < 3; j++) {
            w = (w + 3) & ~3u;
            uint32_t len = (uint32_t)strlen(s[j]);
            memcpy(d + w, &len, 4), memcpy(d + w + 4, s[j], len + 1);
            memcpy(d + tabs[k] + 4 + 4 * j, &(uint32_t){w - (tabs[k] + 4 + 4 * j)}, 4);
            w += 4 + len + 1;
        }
    }
    return vbase;
}
void *hle_alloc(uint64_t size);
static int dress_hook(Cpu *c, uint64_t addr) {
    uint64_t req = c->x[2], fbp = *(const uint64_t *)(uintptr_t)req;
    if (fbp < HEAP_BASE || fbp >= HEAP_BASE + HEAP_MAX) return 0;
    const uint8_t *b = (const uint8_t *)(uintptr_t)fbp;
    uint64_t root = fb_u32(b, 0);
    const uint8_t *f15 = fb_field(b, root, 15), *f14 = fb_field(b, root, 14);
    if (!f15) return 0;
    const uint8_t *vec = f15 + fb_u32(f15, 0);
    uint32_t n = fb_u32(vec, 0);
    uint64_t name_len = ((const uint64_t *)(uintptr_t)req)[3];
    if (n >= 8 && (!g_npparts || name_len == 6)) {  // (the player - "Player" - or the first of its kind: its parts kept)
        pthread_mutex_lock(&g_dress_lock);
        g_npparts = 0;
        for (uint32_t k = 0; k < n && k < 24; k++) {
            const uint8_t *e = vec + 4 + 4 * k;
            part_of(b, e + fb_u32(e, 0), &g_pparts[g_npparts++]);
        }
        g_pbody = f14 ? f14[0] : -1;
        pthread_mutex_unlock(&g_dress_lock);
        extern void world_self_outfit(void);
        world_self_outfit();
        uint64_t out = c->x[8];
        rt_call_original(c, addr);
        LOG("[dress] our player's %d parts kept (body type %d; its instance %llx)\n", g_npparts, g_pbody,
            (unsigned long long)(out ? *(const uint64_t *)(uintptr_t)(out + 8) : 0));
        return 1;
    }
    if (n != 1) return 0;
    DressPart own;
    part_of(b, vec + 4 + fb_u32(vec + 4, 0), &own);
    if (!strstr(own.path, "savm_cc_vr")) return 0;  // (townspeople only)
    int body = f14 ? f14[0] : -1;
    DressPart parts[25];
    int np = 0, to_body = -1;
    uint64_t for_peer = 0;
    uint32_t hash = 0;
    // (any townsperson: station NPCs - made with their level or streamed in as the player walks - can stand in; one
    // of the crowd (made alone while the player stands, its body stepped) can't - its model follows the crowd, not its
    // node - and is let go once its body is seen stepped, 2026-11-16: the dressed walker then keeps their look)
    pthread_mutex_lock(&g_dress_lock);
    Look want_look;
    if (g_dress_want.peer) {  // (a player of the shared world waiting for a look)
        for_peer = g_dress_want.peer, hash = g_dress_want.hash, to_body = g_dress_want.body, want_look = g_dress_want.look;
        for (int k = 0; k < g_dress_want.n; k++) parts[np++] = g_dress_want.parts[k];
        g_dress_want.peer = 0;
    } else if (g_dress_left > 0 && g_npparts && (g_dress_body < 0 || body == g_dress_body)) {  // (research)
        if (g_dress_mode == 1) parts[np++] = own;
        for (int k = 0; k < g_npparts && np < 25; k++) parts[np++] = g_pparts[k];
        g_dress_left--;
    }
    pthread_mutex_unlock(&g_dress_lock);
    if (!np) return 0;
    if (for_peer || g_dress_mode == 2)
        for (int k = 0; k < np; k++) if (!strcmp(parts[k].name, "base")) snprintf(parts[k].name, sizeof parts[k].name, "default");
    // copy the description (8 KB: it and what follows it in its file) into guest memory and add the new parts list
    const uint32_t base = 8192;
    uint8_t *d = hle_alloc(16384);
    memcpy(d, b, base);
    uint32_t vpos = fb_put_parts(d, base, parts, np);
    uint64_t f15o = (uint64_t)(f15 - b);
    memcpy(d + f15o, &(uint32_t){vpos - (uint32_t)f15o}, 4);
    if (f14 && to_body >= 0) d[f14 - b] = (uint8_t)to_body;  // (the player's body type: theirs)
    *(uint64_t *)(uintptr_t)req = (uint64_t)(uintptr_t)d;
    uint64_t owner = ((const uint64_t *)(uintptr_t)req)[6];
    if (for_peer) {
        pthread_mutex_lock(&g_dress_lock);
        int i = g_ndressed < 8 ? g_ndressed++ : 7;
        g_dressed[i].peer = for_peer, g_dressed[i].owner = owner, g_dressed[i].hash = hash;
        pthread_mutex_unlock(&g_dress_lock);
    }
    LOG("[dress] townsperson %s (body type %d) dressed in %s's %d parts (character %llx, description %p)\n", own.path, body,
        for_peer ? "another player" : "our player", np, (unsigned long long)owner, (void *)d);
    if (for_peer) { extern bool look_wear_at_make(Cpu *c, uint64_t owner, const Look *l); look_wear_at_make(c, owner, &want_look); }
    uint64_t out = c->x[8];
    rt_call_original(c, addr);
    LOG("[dress]   its instance %llx\n", (unsigned long long)(out ? *(const uint64_t *)(uintptr_t)(out + 8) : 0));
    return 1;
}
// a dressed character's body, once it has one (0: not yet / gone): its object +0x1a0 -> +0x8 -> +0x138
static uint64_t dressed_body_of(uint64_t owner) {
    uint64_t v = 0, a = 0, b = 0, body = 0;
    if (!heap_read(owner, &v, 8) || v != CHAR_OBJ_VTABLE || !heap_read(owner + 0x1a0, &a, 8) || !heap_read(a + 8, &b, 8) ||
        !heap_read(b + 0x138, &body, 8)) return 0;
    return heap_read(body, &v, 8) && v == BODY_VTABLE ? body : 0;
}
static bool g_dress_hooked;
static void dress_hook_on(void) {
    if (g_dress_hooked) return;
    g_dress_hooked = true;
    rt_register_fnhook_at(CC_MAKE, dress_hook);
}
__attribute__((constructor)) static void dress_init(void) {
    const char *e = getenv("LARECOMP_DRESS_NPC");
    if (!e) return;
    sscanf(e, "%d,%d,%d", &g_dress_left, &g_dress_mode, &g_dress_body);
    dress_hook_on();
    LOG("[dress] the next %d townspeople (body type %d) get the player's outfit, mode %d\n", g_dress_left, g_dress_body, g_dress_mode);
}

// ---- A player's real look. Their character is made from a template (the field player's description is the same for
// everyone: halfup hair, round face, sockssneakers, cap colour 08...); their real outfit and customisation are the
// save's dress-up: a static registration (e.g. 0x70047d7950, its type 0x7003d5a888) whose object (at slot +0x10) holds
// at +0x18 the 13 parts' variants - FNV-1a 64 hashes (basis 0xcbf29ce484222645) of the variant names, e.g.
// p0_hdw0000_00_default_normal_07 - and at +0x80 18 u32 customisation options in the order of its reflection table
// (0x7003d5a6d8: color_skin, color_lip, color_darkcircle, color_eye, color_eyebrow, volume_eyebrow, color_eyelash,
// volume_eyelash, mole, mole_01, freckles, color_hrs_00..03, shape_face, shape_fhr, style_hrs). Link Play sends it as
// CommandSyncDressUp (2026-11-16, a lobby's two copies): 0x7000b69118 -> 0x7000b6b370 serialises it into a 384-byte
// size-prefixed FlatBuffer {1: 1, 2: 1, 3: [13 u64 part hashes], 4: [18 u64 option-name hashes], 5: [18 u32 values]};
// the other side's characters wear it through 0x7000478578 (x0 the character's scene object, x1 its allocator = +8, x2
// -> a pointer to the blob, x8 out): 0x70004788cc deserialises it (0x7000479480) into a DressUp component (vtable
// 0x7003d99358, 0x178 bytes) that is added to the character (the room cards 0x700047786c, CharacterSetup 0x7000387ddc)
// and read by its make. Here: ours is read from the save and sent (type 6); theirs is built into such a blob and put on
// their dressed townsperson just before its make - it then wears their exact items, colours, skin and hair.
//   A part variant no player can wear (LARECOMP_WORLD_OUTFIT upb=p2_upb0002_00_downhoodie_normal_r03_c18 - drawn fine
// without the DressUp) in the DressUp kept the level from ever finishing loading: test values must be real items (a
// real save's never aren't).
#define DRESS_SAVE_TYPE 0x7003d5a888ull
#define DRESS_WEAR 0x7000478578ull
static const char *const k_look_opts[18] = {"color_skin", "color_lip", "color_darkcircle", "color_eye", "color_eyebrow",
    "volume_eyebrow", "color_eyelash", "volume_eyelash", "mole", "mole_01", "freckles", "color_hrs_00", "color_hrs_01",
    "color_hrs_02", "color_hrs_03", "shape_face", "shape_fhr", "style_hrs"};
static uint64_t gf_hash(const char *s) {  // (the game's FNV-1a 64: its own offset basis)
    uint64_t h = 0xcbf29ce484222645ull;
    for (; *s; s++) h = (h ^ (uint8_t)*s) * 0x100000001b3ull;
    return h;
}
// (the registration's slot: {u64 id, its type 0x7003d5a888 - next to the options' reflection table -, the object, ...};
// slots are handed out in the order things register: 0x70047d7950 in one run, 0x70040fe910 in another - looked up by
// type in main's data)
static uint64_t dress_slot(void) {
    static uint64_t slot;
    if (slot && *(const volatile uint64_t *)(uintptr_t)(slot + 8) == DRESS_SAVE_TYPE) return slot;
    slot = 0;
    static uint64_t buf[1 << 14];  // (main's data: each place holding the type, whose object points back at its slot)
    for (uint64_t a = 0x7003e00000ull; a < 0x700617b000ull && !slot; a += sizeof buf) {
        if (!heap_read(a, buf, sizeof buf)) continue;
        for (size_t k = 1; k + 1 < sizeof buf / 8 && !slot; k++) {
            if (buf[k] != DRESS_SAVE_TYPE) continue;
            uint64_t back = 0;
            if (heap_read(buf[k + 1] + 0x10, &back, 8) && back == a + (k - 1) * 8) slot = back;
        }
    }
    return slot;
}
static bool look_ours(Look *l) {
    memset(l, 0, sizeof *l);
    uint64_t slot = dress_slot();
    if (!slot) return false;
    uint64_t o = *(const volatile uint64_t *)(uintptr_t)(slot + 0x10), back = 0;
    if (!heap_read(o + 0x10, &back, 8) || back != slot ||
        !heap_read(o + 0x18, l->part, sizeof l->part) || l->part[0] != gf_hash("p0_base")) return false;  // (one body: p0_base)
    uint32_t v[18];
    if (!heap_read(o + 0x80, v, sizeof v)) return false;
    for (int k = 0; k < 18; k++) l->opt[k] = (uint8_t)(v[k] > 255 ? 255 : v[k]);
    l->f1 = l->f2 = 1;
    static uint64_t logged;
    if (logged != o && (logged = o)) LOG("[world] our look: object %llx, hair %016llx, skin %u eyes %u lashes %u hair colour %u\n", (unsigned long long)o,
        (unsigned long long)l->part[7], l->opt[0], l->opt[3], l->opt[6], l->opt[11]);
    static const char *lk = (const char *)1;  // (research LARECOMP_WORLD_LOOK="<option>=<n>;...": options changed - a test)
    if (lk == (const char *)1) lk = getenv("LARECOMP_WORLD_LOOK");
    for (const char *q = lk; q && *q;) {
        char name[32] = "";
        unsigned val = 0;
        if (sscanf(q, "%31[^=;]=%u", name, &val) == 2)
            for (int k = 0; k < 18; k++) if (!strcmp(k_look_opts[k], name)) l->opt[k] = (uint8_t)val;
        q = strchr(q, ';');
        if (q) q++;
    }
    static const char *e = (const char *)1;  // (LARECOMP_WORLD_OUTFIT: those parts' variants changed here too)
    if (e == (const char *)1) e = getenv("LARECOMP_WORLD_OUTFIT");
    for (const char *q = e; q && *q;) {
        char name[32] = "", var[96] = "";
        if (sscanf(q, "%31[^=;]=%95[^;]", name, var) == 2)
            for (int k = 0; k < g_npparts && k < 13; k++) if (!strcmp(g_pparts[k].name, name)) l->part[k] = gf_hash(var);
        q = strchr(q, ';');
        if (q) q++;
    }
    return true;
}
// the blob CommandSyncDressUp carries, for a Look (the layout the game's serialiser makes)
static void look_blob(const Look *l, uint8_t *b) {
    memset(b, 0, 384);
    uint32_t w32;
#define PUT32(o, v) (w32 = (uint32_t)(v), memcpy(b + (o), &w32, 4))
    PUT32(0, 380), PUT32(4, 0x14);
    static const uint16_t vt[8] = {0x10, 0x18, 0, 4, 8, 0xc, 0x10, 0x14};
    memcpy(b + 8, vt, 16);
    PUT32(0x18, 0x10), PUT32(0x1c, l->f1), PUT32(0x20, l->f2), PUT32(0x24, 0xf0), PUT32(0x28, 0x54), PUT32(0x2c, 4);
    PUT32(0x30, 18);
    for (int k = 0; k < 18; k++) PUT32(0x34 + 4 * k, l->opt[k]);
    PUT32(0x7c, 18);
    for (int k = 0; k < 18; k++) { uint64_t h = gf_hash(k_look_opts[k]); memcpy(b + 0x80 + 8 * k, &h, 8); }
    PUT32(0x114, 13);
    memcpy(b + 0x118, l->part, sizeof l->part);
#undef PUT32
}
// (the call: 0x7000478578 with the blob; x0..: this thread's guest state, kept)
static uint64_t look_put_on(Cpu *c, uint64_t node, uint64_t alloc, const Look *l) {
    uint8_t *m = hle_alloc(512);  // (guest memory, kept: a pointer to the blob, the call's out handle, the blob)
    look_blob(l, m + 128);
    uint64_t bp = (uint64_t)(uintptr_t)(m + 128);
    memcpy(m, &bp, 8);
    uint64_t sx[32], snzcv = c->nzcv;
    __uint128_t sv[32];
    memcpy(sx, c->x, sizeof sx), memcpy(sv, c->v, sizeof sv);
    c->x[0] = node, c->x[1] = alloc, c->x[2] = (uint64_t)(uintptr_t)m, c->x[8] = (uint64_t)(uintptr_t)(m + 64);
    guest_call(c, DRESS_WEAR);
    memcpy(c->x, sx, sizeof sx), memcpy(c->v, sv, sizeof sv), c->nzcv = snzcv;
    uint64_t comp = 0;
    memcpy(&comp, m + 64, 8);
    return comp;
}
// Their look goes on the character's scene object - its transform node (vtable 0x7003be0d28) - BEFORE its make: the
// make reads it (2026-11-16: put on after the make, from the game's main thread, nothing changed; before, the dressed
// townsperson wore the player's exact cap colour, hair, skin, trousers and shoes). The object the make's request names
// (+0x30) is one of the node's components (seen from its +0x20 base) - its +0xc8 holds the node's reference block (node
// +0x110), and that block the node at +0x38. The node's +8 is its allocator (vtable 0x7003cd4a38); its component list
// at +0x118 has an owner byte at +0xa (0x5f free, else the guest thread holding it: then not put on - not waited for).
bool look_wear_at_make(Cpu *c, uint64_t owner, const Look *l) {
    uint64_t v = 0, cb = 0, o = 0, back = 0, alloc = 0, av = 0;
    if (!heap_read(owner, &v, 8) || v != CHAR_OBJ_VTABLE || !heap_read(owner + 0xc8, &cb, 8) || !heap_read(cb + 0x38, &o, 8) ||
        !heap_read(o + 0x110, &back, 8) || back != cb || !heap_read(o + 8, &alloc, 8) || !heap_read(alloc, &av, 8) || av != 0x7003cd4a38ull) {
        LOG("[world] look at make: %llx has no node yet\n", (unsigned long long)owner);
        return false;
    }
    uint8_t lk = *(const volatile uint8_t *)(uintptr_t)(o + 0x118 + 0xa);
    if (lk != 0x5f && lk != (uint8_t)((c->tpidr >> 13) & 0x7f)) { LOG("[world] look at make: node %llx's components held by %02x\n", (unsigned long long)o, lk); return false; }
    uint64_t comp = look_put_on(c, o, alloc, l);
    LOG("[world] look put on node %llx before its make (%llx)\n", (unsigned long long)o, (unsigned long long)comp);
    return true;
}
