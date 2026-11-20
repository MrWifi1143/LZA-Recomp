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
#define CHAR_OBJ_VTABLE 0x7003be7358ull  // (a made character's object: below, the dressed townspeople)
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

// dressed characters let go (a player who left or isn't shown any more, an outfit changed): put away until the game
// removes them - gone_* below (owner: the character's object; body, node once known; col_body: the body whose collision
// was put away)
#define MAXGONE 16
#define GONE_Y (-500.0f)  // (where: under the street - the crowd lets a walker that far off its route go, 2026-11-14)
static struct { volatile uint64_t owner, node, body; uint64_t since, peer, col_body, lost_at; } g_gone[MAXGONE];
static volatile int g_ngone;  // (slots in use: the hooks look only while there are any)
static pthread_mutex_t g_gone_lock = PTHREAD_MUTEX_INITIALIZER;
static bool gone_node(uint64_t n) {
    if (!g_ngone) return false;
    for (int i = 0; i < MAXGONE; i++) if (g_gone[i].node == n && g_gone[i].owner) return true;
    return false;
}
static bool gone_body(uint64_t b) {
    if (!g_ngone || !b) return false;
    for (int i = 0; i < MAXGONE; i++) if (g_gone[i].body == b && g_gone[i].owner) return true;
    return false;
}
static volatile int g_world;   // (the shared world on: sending and showing - below)
static bool g_body_hooks;      // (body_hook registered)
static volatile int g_follow;  // (the mod API follows the player: warp_follow_player)
void newnpc_body(uint64_t body);  // (hle/newnpc.c: the mod API's characters' bodies)
extern volatile int g_newnpc_mods;
static int body_hook(Cpu *c, uint64_t addr) {
    uint64_t o = c->x[0];
    if (g_newnpc_mods) newnpc_body(o);
    if ((g_state < 0 || g_state == 3) && !g_log_every && !g_world && !g_ngone && !g_follow) return 0;  // (off, or finished)
    if (!is_body(o)) return 0;
    if (g_ngone && gone_body(o)) {  // (a let-go dressed walker: kept under the street until the crowd lets it go)
        float *pos = (float *)(uintptr_t)(o + 0x90);
        if (pos[1] > GONE_Y + 100.0f) pos[1] = GONE_Y;
    }
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

// the player's body now (0 none): the oldest one stepped in the last second on the Player collision layer (the proxy's
// +0x40 filter info, low 5 bits: Player 3, NPC 4, SymbolPokemon 5 - below, pres_frame); none such: the oldest of any. (A
// copy put back in a Pokemon Center - a warp off the loaded level, or a black-out - gets a new body; the Pokemon made with
// it right after were then older than it and one of them was taken for the player: the world was sent a Pokemon standing
// in the Center, 2026-11-16)
static uint64_t player_body(uint64_t frame) {
    uint64_t best = 0, best_first = ~0ull, any = 0, any_first = ~0ull;
    pthread_mutex_lock(&g_body_lock);
    for (int i = 0; i < NBODY; i++) {
        if (!g_bodies[i].o || frame - g_bodies[i].last > 30 || !is_body(g_bodies[i].o)) continue;
        if (g_bodies[i].first < any_first) any = g_bodies[i].o, any_first = g_bodies[i].first;
        if ((*(const uint32_t *)(uintptr_t)(g_bodies[i].o + 0x40) & 0x1f) == 3 && g_bodies[i].first < best_first)
            best = g_bodies[i].o, best_first = g_bodies[i].first;
    }
    if (!best) best = any;
    pthread_mutex_unlock(&g_body_lock);
    static int dbg = -1;  // (research LARECOMP_WORLD_LOG_BODIES=1: every 5 s the stepped bodies - the player's candidates)
    if (dbg < 0) dbg = getenv("LARECOMP_WORLD_LOG_BODIES") != NULL;
    if (dbg && frame % 300 == 0)
        for (int i = 0; i < NBODY; i++) {
            if (!g_bodies[i].o || frame - g_bodies[i].last > 30 || !is_body(g_bodies[i].o)) continue;
            const float *q = (const float *)(uintptr_t)(g_bodies[i].o + 0x90);
            LOG("[bodies] frame %llu %llx first %llu last %llu via %u at %.2f %.2f %.2f +40 %08x%s\n", (unsigned long long)frame,
                (unsigned long long)g_bodies[i].o, (unsigned long long)g_bodies[i].first, (unsigned long long)g_bodies[i].last, g_bodies[i].via,
                q[0], q[1], q[2], *(const uint32_t *)(uintptr_t)(g_bodies[i].o + 0x40), g_bodies[i].o == best ? " <- ours" : "");
        }
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

// ---- the mod API's player (runtime/hle/modapi.c): where it is, facing, and put somewhere (its body written for a few
// frames, as the warp above does - the game settles it there)
void warp_follow_player(void) {
    if (g_follow) return;
    g_follow = 1;
    if (!g_body_hooks) g_body_hooks = true, rt_register_fnhook_at(STEP_A, body_hook), rt_register_fnhook_at(STEP_B, body_hook);
}
static float g_tp[3];
static volatile int g_tp_n;
static bool heap_read(uint64_t a, void *out, size_t n);
bool warp_player_now(float pos[3], float *yaw) {  // (yaw: 0 - the body doesn't turn; the facing is the node's, hle/newnpc.c)
    uint64_t b = player_body(g_frame);
    if (!b || !heap_read(b + 0x90, pos, 12)) return false;
    if (yaw) *yaw = 0;
    return true;
}
void warp_teleport(const float p[3]) {
    memcpy(g_tp, p, 12);
    g_tp_n = 10;
}
static void ghost_frame(uint64_t frame);
void warp_frame(uint64_t frame) {  // (each game frame: runtime/hle/input.c)
    g_frame = frame;
    ghost_frame(frame);
    if (g_tp_n > 0) {  // (the mod API's teleport)
        uint64_t b = player_body(frame);
        if (!b) g_tp_n = 0, LOG("[warp] teleport: the player has no body yet\n");
        else if (memcpy((void *)(uintptr_t)(b + 0x90), g_tp, 12), --g_tp_n == 0) LOG("[warp] the player put at %.2f %.2f %.2f\n", g_tp[0], g_tp[1], g_tp[2]);
    }
    if (g_state < 0 && !g_log_every) return;
    uint64_t b = player_body(frame);
    if (g_log_every > 0 && frame % (uint64_t)g_log_every == 0 && b) {
        const float *p = (const float *)(uintptr_t)(b + 0x90);
        LOG("[player] frame %llu %.2f %.2f %.2f (body %llx) t %.1f\n", (unsigned long long)frame, p[0], p[1], p[2], (unsigned long long)b,
            (double)plat_now_ns() / 1e6);  // (t: the monotonic clock in ms - two copies' logs line up)
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

// the mod API's game.work: a work by name, read or written in place in the work manager's maps (above: four unordered_maps
// of {next, hash, key, i32 value +0x18} keyed by FNV-1a 64 of the name) - 0 when the manager isn't up or has no such work
int warp_work(const char *name, int set, int64_t value, int64_t *out) {
    uint64_t mgr = *(const uint64_t *)(uintptr_t)WORK_MANAGER_PTR, h = 0xcbf29ce484222645ull;
    if (mgr - 0x7000000000ull >= 0x617b000ull || !*(const volatile uint32_t *)(uintptr_t)(mgr + 0x200)) return 0;
    for (const char *q = name; *q; q++) h = (h ^ (uint8_t)*q) * 0x100000001b3ull;
    static const uint32_t maps[4] = {0xf0, 0x128, 0x160, 0x198};
    for (int m = 0; m < 4; m++) {
        uint64_t buckets = 0, nb = 0, n = 0, nh = 0, key = 0;
        if (!heap_read(mgr + maps[m], &buckets, 8) || !heap_read(mgr + maps[m] + 8, &nb, 8) || !buckets || !nb || nb > (1u << 24)) continue;
        uint64_t i = (nb & (nb - 1)) == 0 ? h & (nb - 1) : h % nb;
        if (!heap_read(buckets + i * 8, &n, 8) || !n || !heap_read(n, &n, 8)) continue;
        for (int guard = 0; n && guard < 4096; guard++) {
            if (!heap_read(n + 8, &nh, 8) || !heap_read(n + 16, &key, 8)) break;
            if (key == h) {
                int32_t v = 0;
                heap_read(n + 0x18, &v, 4);
                if (out) *out = v;
                if (set) *(int32_t *)(uintptr_t)(n + 0x18) = (int32_t)value, LOG("[warp] script: work %s = %lld (was %d)\n", name, (long long)value, v);
                return 1;
            }
            uint64_t bi = (nb & (nb - 1)) == 0 ? nh & (nb - 1) : nh % nb;
            if (bi != i || !heap_read(n, &n, 8)) break;
        }
    }
    return 0;
}

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
// Options > Online tab, with an online server - contrib/lzaserver, signed in - joined through the relay plugin; or LARECOMP_GHOST=<my port>,
// <peer port>[,<peer host>] for two copies over UDP, a test). Each copy sends its player's position, speed and facing
// ~20 times a second; the others show each player - the nearest few, world_choose - with a character of their own level
// standing in: a townsperson the game makes for them where they are, in their outfit and look (world_own_npc, below; on
// unless set to 0), or - that off, or no station of a loaded level to make one from - one borrowed: a station NPC near
// our player (a character body 3-40 m away that isn't stepped), until a townsperson made in their outfit - a station NPC
// or a walker of the crowd, below - takes over; moved through its transform node. A node (vtable 0x7003be0d28) keeps its local scale at
// +0xc0, rotation at +0xd0 (quaternion xyzw) and translation at +0xe0, and 0x7001ccf27c (x0 the node; called many times
// a frame) makes its world matrices (+0x40, +0x80) from them - writing the translation moves the model (found 2026-11-14
// by shifting every copy of a station NPC's x and halving: the world matrices themselves are overwritten). A
// character's own node: from its object (every one made is kept), else the one whose translation is its body's position. The
// stand-in keeps its own model, turns toward their facing (not snapped to each heading heard), walks, runs and stands as
// they do (stand_in_walk, their animation's locomotion value); its collision is put away while it stands in (pres_frame:
// a station NPC's stays where it stood, a walker's would follow it - neither blocks);
// its Talk prompt is hidden - and A there asks that player to trade (below).
//   (The game's own remote player - Private Battles' - is an ordinary character whose movement comes from the network,
// 2026-11-14: a stand-in until that can be made in the field.)
#define NODE_VTABLE 0x7003be0d28ull
#define NODE_UPDATE 0x7001ccf27cull
#define MAXPEER 32  // (players heard at once: where they are, their outfits and looks kept)
#define MAXSHOW 8   // (players shown at once - the nearest ones, world_choose: LARECOMP_WORLD_SHOW, 4 unless set)
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
    // (their packets' order and timing - type 2's sequence and send time: an older one arriving after a newer one is
    // dropped, not drawn; late = how much longer than their quickest packet lately this one took on the way, seconds -
    // where they are now is the heard position carried on by that too, so uneven delays don't make the stand-in surge
    // and drag)
    uint16_t seq;
    int seq_ok;
    uint32_t base, base_win[2];      // (the quickest arrival-minus-sent seen: of the last two 2 s windows)
    uint64_t base_win_at;
    float late;
    uint32_t stale, nseq;            // (packets dropped as out of date / taken, for the log)
    // (with send times: the stand-in is drawn a little in their past, between the two packets around that moment - it
    // never runs on past where they stopped or turned, as carrying the newest one on by its speed did. snap: the newest
    // NSNAP, oldest first; behind: how far behind their clock it is drawn (ms, past the way's quickest time) - eased
    // toward behind_want so a change never makes it jump back; base0: the first base, the origin of those ms.
    // need: for each packet taken, how old the newest one before it was by then (ms past the way's quickest time) - how
    // far behind the stand-in must be drawn not to run out of packets; behind_want = the 90th percentile of the last
    // NNEED of them (+10 ms), so a lost packet now and then (two gaps) or a stall doesn't keep it far behind for long, as
    // the old slowly forgotten maximum of the lateness did. Run out all the same: it waits where the newest one is (its
    // clock stretched - behind grows), never carried on past it - so it never runs past where they stopped)
#define NSNAP 36  // (1.2 s at 30 a second - world_rate -, 1.8 s at 20: behind is at most 400 ms + 600 ms stretched)
#define NNEED 160 // (~8 s of packets)
    struct { uint32_t t; float p[3], v[3], yaw, move; } snap[NSNAP];
    int nsnap;
    uint32_t base0;
    float behind, behind_want, gap;
    uint16_t need[NNEED];
    int nneed, need_at;
    uint32_t stretched;              // (frames drawn waiting for a packet - for the log: late packets only; not while they are
                                     //  away - a menu, a loading screen: "still here" heard - nor once the wait is past its
                                     //  stretch, the stand-in simply standing there)
    int away;                        // (a "still here" heard since their last position: in a menu or loading)
    uint32_t waiting;                // (frames waited since their last position: taken back off when "still here" comes)
    float target_was[3];             // (the last frame's target: shown follows it exactly, a jump in it is eased out)
    float shown[3];                  // (where the stand-in is drawn: the target, a jump in it eased out)
    float shown_yaw;                 // (its facing: turned toward theirs - not snapped to each heading heard)
    uint64_t heard_ns;
    volatile uint64_t body, node;    // (the stand-in's body, then its node)
    // their outfit (type 5 packets, part k of n): once every part is heard, the next townsperson made is dressed in
    // it and stands in for them from then on (dressed_*: that character, the outfit it wears)
    DressPart parts[MAXPARTS];
    int nparts, body_type;
    uint32_t have, hash, dressed_hash, armed_hash;
    // (with tags - each part carries their whole outfit's hash: the parts as they come, kept here until all of them carry
    // the same one; then they are the outfit - a part lost while they change clothes never makes a half-new one)
    DressPart parts_in[MAXPARTS];
    uint32_t parts_tag[MAXPARTS], have_in, outfit_tag, look_took;  // (outfit_tag / look_took: the tags of what we have)
    int hash_logged;
    // (acknowledgements, type 8: ack_* = what they say they have of ours (acked: they have said so at least once - a copy
    // that does; an older one never does and gets ours resent as before); told_* = what we last told them we have of
    // theirs, when; poked: a part or a look piece of theirs came since - they're still sending, so may not have heard it)
    uint32_t ack_o, ack_l, told_o, told_l;
    int acked, poked;
    uint64_t told_ns;
    uint64_t dressed_owner, dressed_body, dressed_at;
    uint64_t saved_node;             // (the stand-in's node as it was - put back when another one takes over / they leave)
    float saved_q[4], saved_t[3];
    Look look;                       // (their real look: type 6 packets, 2 pieces; worn by their dressed character once whole)
    uint32_t look_have, look_hash;
    uint32_t look_tag[2];            // (each piece's whole-look hash as sent: both the same, and it matches, or not taken)
    Look look_in;                    // (the pieces as they come - copied to look once they belong together)
    int look_state;                  // (0 not checked yet, 1 the game has everything in it, 2 refused: their outfit alone)
    uint64_t via;                    // (who the transport says sent their packets: the server's id - their id can't be borrowed)
    uint64_t pres_body, pres_rec, pres_id;  // (the stand-in's presence body in the physics world: its filter changed -
    uint32_t pres_was;                      //  pres_frame; pres_was: its own, put back on let-go)
    uint64_t armed_at;               // (when their last dressing was asked for: the next one not within 10 s - unless the last
                                     //  one went away, then at once)
    int refused;                     // (outfit parts refused, logged once)
    uint64_t look_refused_at;        // (a refused look logged at most every 30 s: a changing bad look mustn't flood the log)
    // their walk (below, stand_in_walk): the locomotion value their own player's animation has (0 standing, 0.5 walking,
    // 1 running, 1.5 dashing; -1 not sent - an older copy: from their speed) and what their stand-in was last given
    float move, move_given;
    uint64_t move_node, move_bb, move_at, move_given_at, move_scratch, move_tried;
    uint32_t move_idx;
    int move_ok;
    // (a stand-in that sat, talked or stood in a loop of its own when picked - its own AI's pose parameters, POSE_N of
    // them: the ones changed to stand it up, and what they were - given back when it is let go, stand_in_stop)
    int32_t pose_was[10];
    uint16_t pose_cleared;
    uint64_t pose_at;
    // (shown: one of the nearest players - only they get a stand-in, world_choose; since when; how far they were)
    int show, noticed;
    uint64_t shown_at;
    float dist;
    // (their own townsperson - world_own_npc, below: newnpc_<own_k> asked for them and not found yet (0 none), when)
    int own_k;
    uint64_t own_at;
    uint64_t here_ns, key_ns;        // (when they were first heard; when what they wear last changed - the log's timings)
    uint32_t key_seen;
    uint64_t own_none_at;            // (since when no station of a loaded level was there to make theirs from - 5 s: borrowed)
    int own_far;                     // (too far from us for theirs to be made: logged once until nearer)
} Peer;
static bool look_ours(Look *l);
// the shared world's dressing: the next townsperson made wears `want` (its player then waits for it); each one made
// is kept as {player, owner (the request's +0x30: the character's object), outfit} until its body is found
static pthread_mutex_t g_dress_lock = PTHREAD_MUTEX_INITIALIZER;
static struct { uint64_t peer; DressPart parts[MAXPARTS]; int n, body; uint32_t hash; Look look; bool look_ok; } g_dress_want;
static int g_dressings;  // (townspeople dressed for other players so far: each takes 16 KB of HLE memory - capped)
static struct { uint64_t peer, owner; uint32_t hash; } g_dressed[8];
static int g_ndressed;
static DressPart g_pparts[24];   // (our player's parts, as its character was made; g_npparts of them)
static int g_npparts, g_pbody = -1;
// ---- Their own townsperson (always; LARECOMP_WORLD_OWN_NPC=0 for research only): a shown player
// is a NEW townsperson the game makes for them where they are (hle/newnpc.c: the character placement spawn called again
// with a kept station spawner's arguments, our name newnpc_<k> and their spot), made in their outfit and look from the
// start (dress_hook knows it by its node's name: g_own[k % NOWN] says whose, in what) - not one taken from the street and
// dressed when the next one happens to be made (that waited for the crowd: 3 s to over 45 s, and needed the crowd and
// station bookkeeping). It goes with its level (its station's area: a trip away, a loading screen) and is made again; on
// an outfit change a new one is made at once and the old one put away (gone_add). No station spawn of a loaded area kept
// (none seen yet - indoors, a level just loading): the old way, a townsperson borrowed (scan_bodies, g_dress_want).
static int own_npc(void) {
    static int e = -2;
    if (e == -2) e = getenv("LARECOMP_WORLD_OWN_NPC") ? atoi(getenv("LARECOMP_WORLD_OWN_NPC")) != 0 : -1;
    return e >= 0 ? e : 1;
}
void newnpc_on(void);
bool newnpc_ready(void);
int newnpc_spawn(const float at[3], float yaw);
int newnpc_number(uint64_t node);
void newnpc_bad(int k);
void newnpc_missed(int k);
// (how far from us theirs is made: the game realises people within ~58-60 m of the player and keeps them to ~75 m
// (hle/za_fixes.c); one asked for 90 m off was never made - 2026-11-17, fake players at 5..100 m: 5-60 m made at once.
// LARECOMP_WORLD_OWN_RANGE=<m>)
static float own_range(void) {
    static float r = -1;
    if (r < 0) r = getenv("LARECOMP_WORLD_OWN_RANGE") ? (float)atof(getenv("LARECOMP_WORLD_OWN_RANGE")) : 55;
    return r;
}
#define NOWN 64
// (k: newnpc_<k>; n 0: plain - their outfit not heard in time; failed: made, but its station's NPC data is no townsperson)
static struct { int k, failed, made; uint64_t peer; uint32_t key; DressPart parts[MAXPARTS]; int n, body; Look look; bool look_ok; } g_own[NOWN];
static volatile int g_own_used;  // (one asked for: dress_hook looks at node names from then on)
static uint32_t outfit_hash(const DressPart *p, int n) {
    uint32_t h = 2166136261u;
    for (int k = 0; k < n; k++)
        for (const char *s[3] = {p[k].name, p[k].path, p[k].var}, **q = s; q < s + 3; q++)
            for (const char *c = *q; ; c++) { h = (h ^ (uint8_t)*c) * 16777619u; if (!*c) break; }
    return h;
}
static Peer g_peers[MAXPEER];
// the shown players' slots (g_peers indices; the guest threads' hooks look only at these - world_choose rebuilds it)
static volatile uint8_t g_show_idx[MAXSHOW];
static volatile int g_nshow;
static uint64_t g_self;               // (this copy's id in the world)
static volatile int g_scanning;
static volatile uint64_t g_me_body, g_me_node;  // (our player's body and node: the node has the model's facing)
static uint64_t g_me_since;                      // (the frame our player's body became this one: a level settled)
static bool g_hooked;

// ---- packets (both ways the same, little-endian): {u8 type, u8 version 1, u8[6], u64 from, ...}
//   type 2 where:  f32 x y z, f32 yaw, f32 vx vy vz, f32 move (their animation's locomotion value; 48 bytes - copies
//          before it sent 44). Its header's u8[6] = u16 sequence, u32 the sender's clock in ms when sent (0: an older
//          copy - none): over the internet packets come late, unevenly and out of order (WorldWhere below)
//   type 3 trade asked, 4 trade answered: u64 to, u32 code
//   type 9 trade link: u64 to, u32 code, u8 state - during a walk-up trade, how the Link Trade goes: 1 our room is up
//          (the answerer, who hosts: on a change and every 0.5 s while up), 0 it closed, 3 we gave up / stopped (unmatched)
//   type 5 outfit: u8 part k, u8 parts n, u8 body type, then three strings (NUL-ended): the part's name, its .ccdatam
//          ("ik_chara/<dir>/<last>.ccdatam" sent as "<dir>"), its variant (the item and colour); 96 bytes at most.
//          Its header's u8[6] begins with a u32 hash of the whole outfit (outfit_hash; 0: an older copy): taken once all
//          parts carry the same one
//   type 8 have: u64 to, u32 their outfit's hash, u32 their look's hash - what we have of that player's (0: not all of
//          it). Sent when it changes, every 2 s while we lack some of it, and when they send again what we have. A player
//          whose outfit and look everyone has acknowledged sends them only when they change, someone new comes, or once
//          every 15 s for safety - they were a quarter of all traffic, resent every 2 s for good (2026-11-17)
//   type 7 still here: nothing more - sent once a second while we have no player in the world (a menu, Outfit & Looks,
//          a Link Trade, a loading screen): they keep our stand-in where it was instead of taking us for gone after 5 s
//   type 6 look: u8 piece k of n (2), u8[6], 64 bytes of a Look (the save's dress-up: part hashes, options). The u8[6]
//          begins with a u32 FNV-1a of the whole Look (0: an older copy): a look is taken only once both pieces carry
//          the same one and it matches - a piece lost or late while they change clothes never makes a half-new look
typedef struct { uint8_t type, ver, pad[6]; uint64_t from; union { struct { float p[3], yaw, v[3], move; } w; struct { uint64_t to; uint32_t code; uint8_t state; } t;
    struct { uint64_t to; uint32_t o, l; } a;
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
static float g_off_ahead;  // (LARECOMP_GHOST_AHEAD=<degrees>: the offset that far from their facing - in the camera's view)
static float g_off_turn;   // (LARECOMP_GHOST_TURN=<degrees>: the stand-in turned that much from their facing - its profile in view)
static int g_self_test;  // (LARECOMP_GHOST_SELF=1: our own packets count as another player's - one copy, a loopback test)
static void udp_send(const void *p, size_t n) { sendto(g_udp, p, n, 0, (struct sockaddr *)&g_udp_peer, sizeof g_udp_peer); }
static int udp_recv(uint64_t *from, void *p, size_t cap) {
    ssize_t n = recv(g_udp, p, cap, 0);
    *from = 0;  // (a test: no one vouches for the sender)
    return n > 0 ? (int)n : 0;
}
ssize_t process_vm_readv(pid_t, const struct iovec *, unsigned long, const struct iovec *, unsigned long, unsigned long);
#else
static const float g_off = 0;
static const float g_off_ahead = 0;
static const float g_off_turn = 0;
static const int g_self_test = 0;
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
    // (region by region: the page-write tracker's read-only ranges split the heap into many regions - a 1 MB chunk of the
    // heap scans spans several, and was skipped whole when only the first one counted)
    for (uint64_t p = a, end = a + n; p < end;) {
        MEMORY_BASIC_INFORMATION mb;
        if (!VirtualQuery((const void *)(uintptr_t)p, &mb, sizeof mb) || mb.State != MEM_COMMIT || !mb.Protect ||
            (mb.Protect & (PAGE_NOACCESS | PAGE_GUARD))) return false;
        uint64_t re = (uint64_t)(uintptr_t)mb.BaseAddress + mb.RegionSize, e = re < end ? re : end;
        memcpy((uint8_t *)out + (p - a), (const void *)(uintptr_t)p, e - p);
        p = e;
    }
    return true;
#else
    struct iovec l = {out, n}, r = {(void *)(uintptr_t)a, n};
    return process_vm_readv(getpid(), &l, 1, &r, 1, 0) == (ssize_t)n;
#endif
}

bool warp_heap_read(uint64_t a, void *out, size_t n) { return heap_read(a, out, n); }  // (hle/newnpc.c)
static Peer *peer_of_node(uint64_t n) {
    for (int k = 0, m = g_nshow; k < m; k++) { Peer *p = &g_peers[g_show_idx[k]]; if (p->id && p->node == n) return p; }
    return NULL;
}

// ---- Their walk. A character's animation reads named parameters from its blackboard (vtable 0x7003c2b7a8: tables of
// 0x40-byte entries {FNV-1a 64 of the name, the name, its length, ..., +0x20/+0x28 the listeners} - +0x68 ints, +0x88
// floats, +0xa8 bools, +0xc8 triggers); 0x700010a6e0 (x0 the blackboard, x1 -> {u8 1, u32 the index at +4}, s0) sets a
// float and tells the listeners (the animation graphs). The float "move" is the locomotion: the player's controller
// (0x7000107e80: a parameter set from data per movement state) gives 0.5 walking, 1 running, 1.5 dashing, 0 standing;
// townspeople get theirs from their AI the same way (found 2026-11-16 by Lua-hooking the setters; the sound update
// 0x70029a1644 reads "move_front" / "move" too). A character node's blackboard: the node's component vector (+0x148 ..
// +0x150) holds one with vtable 0x7003c2b008, its +0x130 the blackboard. Giving "move" = 1 to the player's own made it
// run on the spot of its own accord (root motion); a stand-in gets its player's value - its node is put where they are
// anyway (node_hook), so the root motion goes nowhere. The game sets it only on a change of state, so ours stays.
#define PARAMS_COMP_VTABLE 0x7003c2b008ull
#define BB_VTABLE 0x7003c2b7a8ull
#define BB_SET_FLOAT 0x700010a6e0ull
#define HASH_MOVE 0xd5eb2f99669eefd4ull  // (the game's FNV-1a 64 of "move")
void *hle_alloc(uint64_t size);
static uint64_t anim_bb_of_node(uint64_t node) {
    uint64_t v = 0, b = 0, e = 0, comps[256];
    if (!heap_read(node, &v, 8) || v != NODE_VTABLE || !heap_read(node + 0x148, &b, 8) || !heap_read(node + 0x150, &e, 8) ||
        e <= b || e - b > sizeof comps || (e - b) % 8 || !heap_read(b, comps, e - b)) return 0;
    for (size_t i = 0; i < (e - b) / 8; i++) {
        uint64_t cv = 0, bb = 0, bv = 0;
        if (heap_read(comps[i], &cv, 8) && cv == PARAMS_COMP_VTABLE && heap_read(comps[i] + 0x130, &bb, 8) && heap_read(bb, &bv, 8) &&
            bv == BB_VTABLE) return bb;
    }
    return 0;
}
// a blackboard's float by its name's hash: its index (false: none, or nothing listens to it)
static bool anim_float_index(uint64_t bb, uint64_t hash, uint32_t *idx) {
    uint64_t b = 0, e = 0;
    if (!heap_read(bb + 0x88, &b, 8) || !heap_read(bb + 0x90, &e, 8) || e <= b || e - b > 0x40 * 512) return false;
    for (uint64_t p = b; p < e; p += 0x40) {
        uint64_t ent[6];
        if (!heap_read(p, ent, sizeof ent)) return false;
        if (ent[0] != hash) continue;
        if (ent[4] == ent[5]) return false;
        *idx = (uint32_t)((p - b) / 0x40);
        return true;
    }
    return false;
}
// (the call, from a guest thread: x0.., v0..: its state, kept; scratch: 8 bytes of guest memory; fn: one of the
// blackboard's setters - x2 the int / bool, s0 the float. t_ours: the setters' hooks below let our own sets through)
static __thread int t_ours;
static void anim_set(Cpu *c, uint64_t fn, uint64_t bb, uint32_t idx, int32_t ival, float val, uint64_t scratch) {
    uint64_t sx[32], snzcv = c->nzcv;
    __uint128_t sv[32];
    memcpy(sx, c->x, sizeof sx), memcpy(sv, c->v, sizeof sv);
    uint8_t *s = (uint8_t *)(uintptr_t)scratch;
    memset(s, 0, 8), s[0] = 1, memcpy(s + 4, &idx, 4);
    c->x[0] = bb, c->x[1] = scratch, c->x[2] = (uint64_t)(uint32_t)ival, c->v[0] = 0;
    memcpy(&c->v[0], &val, 4);
    t_ours = 1;
    guest_call(c, fn);
    t_ours = 0;
    memcpy(c->x, sx, sizeof sx), memcpy(c->v, sv, sizeof sv), c->nzcv = snzcv;
}
// ---- A stand-in does what its player does, nothing of its own: the AI of the townsperson it is (a station NPC's - its
// own one is made from a station's data too) sets its blackboard as it goes - sit / talk / wait / loop poses, "move" for
// a stroll - and those sets of a stand-in's are overridden as they happen (the hooked setters change the value before the
// game's setter runs: "move" their player's, a pose parameter its standing value), so its animation only ever has what
// its player does. Its triggers too (0x700010ba8c, x0 the blackboard, x1 the key, w2 1 fire / 0 reset): a station NPC's AI
// fires once_action_trigger (a gesture at whoever stands by - with once_action_int its kind) and move_to_bump (its stumble
// when walked into) - reset instead; its eyes still blink (2026-11-17, LARECOMP_WORLD_LOG_AI on our own townsperson: blinks,
// once_action, move_to_bump / bump_to_move, wait_to_squat - nothing else in 30 s). Research LARECOMP_WORLD_LOG_AI=1: every
// set of a stand-in's blackboard not ours, logged (name, value, the caller)
static const Peer *standin_of_bb(uint64_t bb) {
    for (int k = 0, m = g_nshow; k < m; k++) {
        const Peer *p = &g_peers[g_show_idx[k]];
        if (p->id && p->move_ok && p->move_bb == bb) return p;
    }
    return NULL;
}
static int ai_log(void) {
    static int l = -1;
    if (l < 0) l = getenv("LARECOMP_WORLD_LOG_AI") != NULL;
    return l;
}
static void bb_name(Cpu *c, uint64_t bb, uint64_t tab, char *nm, size_t n) {  // (the parameter a setter's key names)
    uint32_t idx = 0;
    uint64_t b = 0, e = 0, ent[3] = {0};
    snprintf(nm, n, "?");
    if (heap_read(c->x[1] + 4, &idx, 4) && heap_read(bb + tab, &b, 8) && heap_read(bb + tab + 8, &e, 8) && e > b && idx < (e - b) / 0x40 &&
        heap_read(b + (uint64_t)idx * 0x40, ent, sizeof ent) && ent[2] && ent[2] < n && heap_read(ent[1], nm, ent[2])) nm[ent[2]] = 0;
}
static void ai_set_log(Cpu *c, uint64_t bb, uint64_t tab, char kind, const char *what) {
    char nm[48];
    bb_name(c, bb, tab, nm, sizeof nm);
    float f;
    memcpy(&f, &c->v[0], 4);
    if (kind == 'f') LOG("[world] AI set of a stand-in's %s (float) %.3f from %llx%s\n", nm, f, (unsigned long long)c->x[30], what);
    else LOG("[world] AI set of a stand-in's %s (%c) %d from %llx%s\n", nm, kind, (int)(uint32_t)c->x[2], (unsigned long long)c->x[30], what);
}
#define BB_SET_TRIGGER 0x700010ba8cull
static int ai_trigger_hook(Cpu *c, uint64_t addr) {
    (void)addr;
    if (t_ours || !g_nshow || !(c->x[2] & 0xff) || !standin_of_bb(c->x[0])) return 0;
    char nm[48];
    bb_name(c, c->x[0], 0xc8, nm, sizeof nm);
    bool keep = !strncmp(nm, "eye_", 4);  // (blinking)
    if (ai_log()) ai_set_log(c, c->x[0], 0xc8, 't', keep ? "" : " - reset instead");
    if (!keep) c->x[2] &= ~0xffull;
    return 0;
}
static void anim_set_float(Cpu *c, uint64_t bb, uint32_t idx, float val, uint64_t scratch) { anim_set(c, BB_SET_FLOAT, bb, idx, 0, val, scratch); }
// (hle/newnpc.c: the mod API's characters walk with the same "move" value)
uint64_t warp_anim_bb(uint64_t node) { return anim_bb_of_node(node); }
bool warp_anim_move_index(uint64_t bb, uint32_t *idx) { return anim_float_index(bb, HASH_MOVE, idx); }
void warp_anim_set_move(Cpu *c, uint64_t bb, uint32_t idx, float val, uint64_t scratch) {
    uint64_t v = 0;
    if (heap_read(bb, &v, 8) && v == BB_VTABLE) anim_set_float(c, bb, idx, val, scratch);
}
static bool anim_index(uint64_t bb, uint64_t tab, uint64_t hash, uint32_t *idx);
#define BB_SET_INT 0x700010aad4ull
#define BB_SET_BOOL 0x700010e8b8ull
// (the mod API's npc:anim) one of a character's animation parameters by its name's hash: kind 'f' float, 'i' int, 'b'
// bool, 't' trigger (fired) - false: its blackboard has no such parameter
bool warp_anim_param(Cpu *c, uint64_t node, uint64_t hash, char kind, float val, uint64_t scratch) {
    uint64_t bb = anim_bb_of_node(node), tab = kind == 'i' ? 0x68 : kind == 'f' ? 0x88 : kind == 'b' ? 0xa8 : 0xc8;
    uint32_t idx;
    if (!bb || !anim_index(bb, tab, hash, &idx)) return false;
    uint64_t fn = kind == 'i' ? BB_SET_INT : kind == 'f' ? BB_SET_FLOAT : kind == 'b' ? BB_SET_BOOL : BB_SET_TRIGGER;
    anim_set(c, fn, bb, idx, kind == 't' ? 1 : (int32_t)val, val, scratch);
    return true;
}

// ---- Sitting, talking, standing in a loop of its own (2026-11-17, Lua: testout/za-mp8/tools/sit.lua, stand.lua). A
// townsperson's pose is its blackboard too, set once from its placement as it is made (0x700010e6ac / 0x700010a814 -> the
// bool / int setters below): a bench sitter has sit_bool 1 and sit_int 1 or 2 (tr0064 tourist on the Vert Pokemon
// Center's ledge, women on the benches 40 m on), a group chatting speak_bool 1 + wait_int 2 (or wait_int 2 alone), some
// loop_action_bool 1; everyone else 0 / 1. In such a pose "move" does nothing - the stand-in that was the tourist stayed
// seated in mid-air and glided after its player (za-mp7). Setting those back to 0 / 1 (the game's own setters) stands
// it up at once and it then walks, runs and stops like any other (checked by hand on the tourist, a talking woman and a
// looping child). So: every set of these parameters is kept per blackboard (the setters hooked from the start - the
// level's people are made at its load); a plain stand-in is never one in such a pose (scan_bodies passes them over); a
// dressed one, made from whoever came next, is stood up (stand_in_walk; again if its AI puts it back) and a stand-in let
// go gets its own values back (stand_in_stop: sits down again where it sat).
//   LARECOMP_WORLD_POSE=0: as before (neither) - an A/B.
#define POSE_N 10
static const struct { const char *name; char kind; int32_t neutral; } k_pose[POSE_N] = {
    {"sit_bool", 'b', 0}, {"sit_int", 'i', 0}, {"speak_bool", 'b', 0}, {"wait_int", 'i', 1}, {"idle_int", 'i', 1},
    {"loop_action_bool", 'b', 0}, {"ev_loop_action_bool", 'b', 0}, {"ev_ex_loop_action_bool", 'b', 0},
    {"ev_ex_loop_action_act_bool", 'b', 0}, {"ua07_speak_bool", 'b', 0}};
static uint64_t g_pose_hash[POSE_N];
#define POSE_BUCKETS 1024  // (x4: blackboards with a pose set - a new character at an old one's address sets its own)
static struct { uint64_t bb; int32_t v[POSE_N]; } g_pose[POSE_BUCKETS][4];
static pthread_mutex_t g_pose_lock = PTHREAD_MUTEX_INITIALIZER;
static int pose_mode(void) {
    static int m = -1;
    if (m < 0) m = getenv("LARECOMP_WORLD_POSE") ? atoi(getenv("LARECOMP_WORLD_POSE")) : 1;
    return m;
}
static unsigned pose_bucket(uint64_t bb) { return (unsigned)(((bb >> 9) * 0x9E3779B97F4A7C15ull) >> 54) & (POSE_BUCKETS - 1); }
static bool pose_neutral(const int32_t *v) {
    for (int k = 0; k < POSE_N; k++) if (v[k] != k_pose[k].neutral) return false;
    return true;
}
static int pose_hook(Cpu *c, uint64_t addr) {  // (the int and bool setters: x0 the blackboard, x1 -> {u8 1, u32 index at +4}, w2)
    // (read in place, no copies: called for every int / bool any character's AI sets - the game reads the same right after)
    bool is_int = addr == BB_SET_INT;
    uint64_t bb = c->x[0], key = c->x[1];
    if (bb < HEAP_BASE || bb >= HEAP_BASE + HEAP_MAX || key < 0x7000000000ull || key >= HEAP_BASE + HEAP_MAX) return 0;
    uint32_t idx;
    memcpy(&idx, (const void *)(uintptr_t)(key + 4), 4);
    const uint64_t *tab = (const uint64_t *)(uintptr_t)(bb + (is_int ? 0x68 : 0xa8));
    uint64_t b = tab[0], e = tab[1];
    if (e <= b || b < HEAP_BASE || e > HEAP_BASE + HEAP_MAX || idx >= (e - b) / 0x40) return 0;
    uint64_t h = *(const uint64_t *)(uintptr_t)(b + (uint64_t)idx * 0x40);
    int k = 0;
    while (k < POSE_N && (g_pose_hash[k] != h || (k_pose[k].kind == 'i') != is_int)) k++;
    bool standin = !t_ours && g_nshow && standin_of_bb(bb);
    if (standin && k == POSE_N && !is_int && (c->x[2] & 0xff)) {  // (its stumble when walked into: not started)
        char nm[48];
        bb_name(c, bb, 0xa8, nm, sizeof nm);
        bool bump = !strcmp(nm, "move_to_bump");
        if (ai_log()) ai_set_log(c, bb, 0xa8, 'b', bump ? " - 0 instead" : "");
        if (bump) c->x[2] &= ~0xffull;
        return 0;
    }
    if (standin && ai_log()) ai_set_log(c, bb, is_int ? 0x68 : 0xa8, is_int ? 'i' : 'b', k < POSE_N ? " - standing instead" : "");
    if (k == POSE_N) return 0;
    if (standin) {  // (a stand-in's AI posing it: it stays standing - what it set is kept as its own, for its let-go)
        int32_t v = is_int ? (int32_t)(uint32_t)c->x[2] : (int32_t)(c->x[2] & 0xff);
        Peer *p = (Peer *)standin_of_bb(bb);
        if (p && v != k_pose[k].neutral) p->pose_was[k] = v, p->pose_cleared |= (uint16_t)(1u << k);
        c->x[2] = (c->x[2] & ~(is_int ? 0xffffffffull : 0xffull)) | (uint32_t)k_pose[k].neutral;
        return 0;
    }
    int32_t v = is_int ? (int32_t)(uint32_t)c->x[2] : (int32_t)(c->x[2] & 0xff);
    unsigned bk = pose_bucket(bb);
    pthread_mutex_lock(&g_pose_lock);
    int s = -1;
    for (int j = 0; j < 4 && s < 0; j++) if (g_pose[bk][j].bb == bb) s = j;
    if (s < 0 && v != k_pose[k].neutral) {  // (a new one: in a free place, else one with nothing set, else the oldest)
        for (int j = 0; j < 4 && s < 0; j++) if (!g_pose[bk][j].bb || pose_neutral(g_pose[bk][j].v)) s = j;
        if (s < 0) s = (int)(bb >> 9) & 3;
        g_pose[bk][s].bb = bb;
        for (int j = 0; j < POSE_N; j++) g_pose[bk][s].v[j] = k_pose[j].neutral;
    }
    if (s >= 0) g_pose[bk][s].v[k] = v;
    pthread_mutex_unlock(&g_pose_lock);
    return 0;
}
// a blackboard's pose (false: none - standing - or nothing known; v: its values)
static bool pose_of(uint64_t bb, int32_t *v) {
    unsigned bk = pose_bucket(bb);
    bool posed = false;
    pthread_mutex_lock(&g_pose_lock);
    for (int j = 0; j < 4; j++)
        if (g_pose[bk][j].bb == bb) { memcpy(v, g_pose[bk][j].v, sizeof g_pose[bk][j].v); posed = !pose_neutral(v); break; }
    pthread_mutex_unlock(&g_pose_lock);
    return posed;
}
static void pose_names(const int32_t *v, char *out, size_t n) {
    size_t w = 0;
    out[0] = 0;
    for (int k = 0; k < POSE_N && w + 1 < n; k++)
        if (v[k] != k_pose[k].neutral) w += (size_t)snprintf(out + w, n - w, "%s%s %d", w ? ", " : "", k_pose[k].name, v[k]);
}
static uint64_t gf_hash(const char *s);
static bool g_pose_hooked;
static void pose_hooks_on(void) {
    if (g_pose_hooked || !pose_mode()) return;
    g_pose_hooked = true;
    for (int k = 0; k < POSE_N; k++) g_pose_hash[k] = gf_hash(k_pose[k].name);
    rt_register_fnhook_at(BB_SET_INT, pose_hook);
    rt_register_fnhook_at(BB_SET_BOOL, pose_hook);
}
// a blackboard parameter's index by its name's hash in one of its tables (+0x68 ints, +0x88 floats, +0xa8 bools)
static bool anim_index(uint64_t bb, uint64_t tab, uint64_t hash, uint32_t *idx) {
    uint64_t b = 0, e = 0;
    if (!heap_read(bb + tab, &b, 8) || !heap_read(bb + tab + 8, &e, 8) || e <= b || e - b > 0x40 * 512) return false;
    for (uint64_t p = b; p < e; p += 0x40) {
        uint64_t h = 0;
        if (!heap_read(p, &h, 8)) return false;
        if (h == hash) { *idx = (uint32_t)((p - b) / 0x40); return true; }
    }
    return false;
}
// (stand_in_walk, a guest thread) the stand-in in a pose: each parameter back to its standing value - what it was kept
// the first time (for its let-go). True: something was changed
static bool pose_stand_up(Cpu *c, Peer *p, uint64_t bb, uint64_t scratch) {
    int32_t v[POSE_N];
    if (!pose_of(bb, v)) return false;
    char names[200];
    pose_names(v, names, sizeof names);
    bool any = false;
    for (int k = 0; k < POSE_N; k++) {
        uint32_t idx;
        if (v[k] == k_pose[k].neutral || !anim_index(bb, k_pose[k].kind == 'i' ? 0x68 : 0xa8, g_pose_hash[k], &idx)) continue;
        if (!(p->pose_cleared & (1u << k))) p->pose_was[k] = v[k], p->pose_cleared |= (uint16_t)(1u << k);
        anim_set(c, k_pose[k].kind == 'i' ? BB_SET_INT : BB_SET_BOOL, bb, idx, k_pose[k].neutral, 0, scratch);
        any = true;
    }
    if (any)
        LOG("[world] player %016llx's stand-in was in a pose of its own (%s): stood up\n", (unsigned long long)p->id, names);
    return any;
}
// our player's value, as its controller sets it (0x700010a6e0 hooked: our blackboard, the "move" index)
static volatile uint64_t g_me_bb, g_me_bb_node;
static volatile uint32_t g_me_move_idx = ~0u;
static volatile float g_me_move;
static int bb_set_hook(Cpu *c, uint64_t addr) {
    (void)addr;
    uint64_t bb = g_me_bb;
    if (!t_ours && g_nshow && c->x[0] != bb) {  // (a stand-in's: its "move" stays their player's)
        const Peer *p = standin_of_bb(c->x[0]);
        uint32_t idx = 0;
        if (p) {
            bool mv = heap_read(c->x[1] + 4, &idx, 4) && idx == p->move_idx && p->move_given >= 0;
            if (ai_log()) ai_set_log(c, c->x[0], 0x88, 'f', mv ? " - theirs instead" : "");
            if (mv) memcpy(&c->v[0], &p->move_given, 4);
        }
        return 0;
    }
    if (!bb || c->x[0] != bb) return 0;
    uint32_t idx = 0;
    if (!heap_read(c->x[1] + 4, &idx, 4) || idx != g_me_move_idx) return 0;
    float v;
    memcpy(&v, &c->v[0], 4);
    if (isfinite(v)) g_me_move = v;
    return 0;
}
// ---- No Talk prompt by a stand-in (its Talk is kept from the game anyway - A there asks for a trade). A townsperson's
// InteractNPC component (vtable 0x7003ce9540) offers "Talk" from its per-frame 0x7000d44050 (x0 the component); its +0x40
// is a handle to the character's animation component (+0x28 of an object with vtable 0x7003d2a8e8), whose blackboard
// ([[E +0x20] + 0x128] for h = [[comp+0x2038]+0x80], E = [0x7003ec21c8] + (h & 0xfffff) * 32, [E+0x10] == h) is the
// one its node's component holds - the stand-in's (found 2026-11-16). That update is skipped for a stand-in's.
#define INTERACT_NPC_UPDATE 0x7000d44050ull
#define HANDLE_TABLE_PTR 0x7003ec21c8ull
static uint64_t handle_obj(uint64_t h) {  // (a handle's object, 0: none / stale)
    uint64_t tab = 0, id = 0, o = 0;
    if (!h || !heap_read(HANDLE_TABLE_PTR, &tab, 8) || !tab) return 0;
    uint64_t e = tab + (h & 0xfffff) * 32;
    if (!heap_read(e + 0x10, &id, 8) || id != h || !heap_read(e + 0x20, &o, 8)) return 0;
    return o;
}
static uint64_t interact_bb(uint64_t comp) {  // (an InteractNPC component's character's blackboard, 0: not known)
    uint64_t h = 0, ac = 0, x = 0, h2 = 0, o = 0, bb = 0, v = 0;
    if (!heap_read(comp + 0x40, &h, 8) || !(ac = handle_obj(h)) || !heap_read(ac - 0x28 + 0x2038, &x, 8) || !heap_read(x + 0x80, &h2, 8) ||
        !(o = handle_obj(h2)) || !heap_read(o + 0x128, &bb, 8) || !heap_read(bb, &v, 8) || v != BB_VTABLE) return 0;
    return bb;
}
static int interact_hook(Cpu *c, uint64_t addr) {
    (void)addr;
    if (!g_world) return 0;
    bool any = false;
    for (int k = 0, m = g_nshow; k < m; k++) any |= g_peers[g_show_idx[k]].id && g_peers[g_show_idx[k]].move_ok;
    if (!any) return 0;
    static struct { uint64_t comp, bb, at; } cache[64];
    static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
    uint64_t comp = c->x[0], f = g_frame, bb = 0;
    unsigned slot = (unsigned)(comp >> 5) & 63;
    pthread_mutex_lock(&lock);
    bool have = cache[slot].comp == comp && f - cache[slot].at < 120;
    if (have) bb = cache[slot].bb;
    pthread_mutex_unlock(&lock);
    if (!have) {
        bb = interact_bb(comp);
        pthread_mutex_lock(&lock);
        cache[slot].comp = comp, cache[slot].bb = bb, cache[slot].at = f;
        pthread_mutex_unlock(&lock);
    }
    if (!bb) return 0;
    for (int k = 0, m = g_nshow; k < m; k++) {
        const Peer *p = &g_peers[g_show_idx[k]];
        if (p->id && p->move_ok && p->move_bb == bb) return 1;  // (no Talk)
    }
    return 0;
}
// a let-go stand-in back to standing ("move" 0) and to its own pose (sitting again, if it sat) - from the next update of
// its node (a guest thread)
static struct { uint64_t node, bb, scratch, at; uint32_t idx; int move; uint16_t pose_mask; int32_t pose[POSE_N]; } g_move_reset[8];
static pthread_mutex_t g_move_lock = PTHREAD_MUTEX_INITIALIZER;
static void stand_in_stop(Peer *p) {
    bool move = p->move_given > 0, pose = p->pose_cleared != 0;
    if (!p->move_ok || !p->move_bb || !p->move_scratch || (!move && !pose)) { p->move_node = 0, p->move_ok = 0, p->pose_cleared = 0; return; }
    pthread_mutex_lock(&g_move_lock);
    for (int i = 0; i < 8; i++)
        if (!g_move_reset[i].node) {
            g_move_reset[i].node = p->move_node, g_move_reset[i].bb = p->move_bb, g_move_reset[i].idx = p->move_idx,
            g_move_reset[i].scratch = p->move_scratch, g_move_reset[i].at = g_frame, g_move_reset[i].move = move;
            g_move_reset[i].pose_mask = p->pose_cleared, memcpy(g_move_reset[i].pose, p->pose_was, sizeof p->pose_was);
            break;
        }
    pthread_mutex_unlock(&g_move_lock);
    p->move_node = 0, p->move_ok = 0, p->move_given = 0, p->move_scratch = 0, p->pose_cleared = 0;  // (the scratch goes with
}                                                                                                      //  the reset: a new one
                                                                                                       //  for the next stand-in)
static void move_reset_check(Cpu *c, uint64_t node) {
    for (int i = 0; i < 8; i++) {
        if (g_move_reset[i].node != node) continue;
        pthread_mutex_lock(&g_move_lock);
        uint64_t bb = g_move_reset[i].bb, s = g_move_reset[i].scratch;
        uint32_t idx = g_move_reset[i].idx;
        int move = g_move_reset[i].move;
        uint16_t mask = g_move_reset[i].pose_mask;
        int32_t pose[POSE_N];
        memcpy(pose, g_move_reset[i].pose, sizeof pose);
        bool mine = g_move_reset[i].node == node;
        if (mine) g_move_reset[i].node = 0;
        pthread_mutex_unlock(&g_move_lock);
        uint64_t v = 0;
        if (!mine || !s || !heap_read(bb, &v, 8) || v != BB_VTABLE) continue;
        if (move) anim_set_float(c, bb, idx, 0.0f, s);
        for (int k = 0; k < POSE_N; k++) {
            uint32_t pi;
            if ((mask & (1u << k)) && anim_index(bb, k_pose[k].kind == 'i' ? 0x68 : 0xa8, g_pose_hash[k], &pi))
                anim_set(c, k_pose[k].kind == 'i' ? BB_SET_INT : BB_SET_BOOL, bb, pi, pose[k], 0, s);
        }
        if (mask) LOG("[world] a let-go stand-in back in its own pose\n");
    }
}
static void move_reset_expire(uint64_t frame) {  // (a node that went with its level isn't updated again)
    pthread_mutex_lock(&g_move_lock);
    for (int i = 0; i < 8; i++) if (g_move_reset[i].node && frame - g_move_reset[i].at > 600) g_move_reset[i].node = 0;
    pthread_mutex_unlock(&g_move_lock);
}
static volatile int g_scan_soon;  // (a stand-in went with its level: the next one looked for at once, not within 10 s)
static void stand_in_gone(Peer *p) {  // (with its level - its collision too)
    p->pres_body = p->pres_rec = p->pres_id = 0;
    p->body = p->node = 0, p->move_node = 0, p->move_ok = 0, p->move_scratch = 0, p->pose_cleared = 0, g_scan_soon = 1;
}
// what a stand-in is given: their own value; from an older copy (no value) their speed: walking below ~4 m/s (research
// LARECOMP_WORLD_MOVE=<v>: always v - a test)
static float stand_in_move(const Peer *p) {
    static float forced = -2;
    if (forced == -2) forced = getenv("LARECOMP_WORLD_MOVE") ? (float)atof(getenv("LARECOMP_WORLD_MOVE")) : -1;
    if (forced >= 0) return forced;
    if (p->move >= 0) return p->move;
    float s = sqrtf(p->vel[0] * p->vel[0] + p->vel[2] * p->vel[2]);
    return s < 0.4f ? 0.0f : s < 4.0f ? 0.5f : 1.0f;
}
// (node_hook, the stand-in's node, a guest thread: once a frame - the value when it changes, and again every second: its
// own AI changing state sets its own)
static void stand_in_walk(Cpu *c, Peer *p, uint64_t node) {
    uint64_t f = g_frame;
    if (__atomic_exchange_n(&p->move_at, f, __ATOMIC_ACQ_REL) == f) return;
    bool fresh = p->move_node != node;
    if (fresh || (!p->move_ok && f - p->move_tried >= 30 && p->move_tried)) {  // (a character just made gets its animation
        if (fresh && p->move_node) stand_in_stop(p);                            //  a little later: looked for again)
        p->move_node = node, p->move_bb = anim_bb_of_node(node), p->move_given = -1, p->move_tried = f;
        p->move_ok = p->move_bb && anim_float_index(p->move_bb, HASH_MOVE, &p->move_idx);
        if (p->move_ok && !p->move_scratch) p->move_scratch = (uint64_t)(uintptr_t)hle_alloc(16);
        if (fresh || p->move_ok)
            LOG("[world] player %016llx's stand-in (node %llx): blackboard %llx, %s\n", (unsigned long long)p->id, (unsigned long long)node,
                (unsigned long long)p->move_bb, p->move_ok ? "walks with them" : "no \"move\" yet");
    }
    if (!p->move_ok || !p->move_scratch) return;
    if (pose_mode() && f - p->pose_at >= 10) {  // (in a pose of its own - sitting, talking: stood up; looked at a few times a
        p->pose_at = f;                         //  second - its AI may put it back)
        uint64_t v = 0;
        if (heap_read(p->move_bb, &v, 8) && v == BB_VTABLE && pose_stand_up(c, p, p->move_bb, p->move_scratch)) p->move_given = -2;
    }                                                                                         // (-2: its "move" given again)
    float want = stand_in_move(p);
    if (want == p->move_given && f - p->move_given_at < 60) return;
    uint64_t v = 0;
    if (!heap_read(p->move_bb, &v, 8) || v != BB_VTABLE) { p->move_ok = 0; return; }
    anim_set_float(c, p->move_bb, p->move_idx, want, p->move_scratch);
    p->move_given = want, p->move_given_at = f;
}
void newnpc_node(Cpu *c, uint64_t node);  // (hle/newnpc.c: the mod API's characters, walked / put away)
static int node_hook(Cpu *c, uint64_t addr) {
    (void)addr;
    uint64_t n = c->x[0];
    if (g_newnpc_mods) newnpc_node(c, n);
    if ((!g_world && !g_ngone) || n < HEAP_BASE || n >= HEAP_BASE + HEAP_MAX) return 0;
    if (g_ngone && gone_node(n) && *(const uint64_t *)(uintptr_t)n == NODE_VTABLE) {  // (a let-go dressed character: away)
        float *t = (float *)(uintptr_t)(n + 0xe0);
        if (t[1] > GONE_Y + 100.0f) t[1] = GONE_Y;
        return 0;
    }
    if (!g_world) return 0;
    Peer *p = peer_of_node(n);
    if (p) {
        if (*(const uint64_t *)(uintptr_t)n != NODE_VTABLE) return 0;
        float *q = (float *)(uintptr_t)(n + 0xd0), *t = (float *)(uintptr_t)(n + 0xe0);
        if (p->saved_node != n) memcpy(p->saved_q, q, 16), memcpy(p->saved_t, t, 12), p->saved_node = n;
        q[0] = 0, q[1] = sinf((p->shown_yaw + g_off_turn) * 0.5f), q[2] = 0, q[3] = cosf((p->shown_yaw + g_off_turn) * 0.5f);
        float oa = p->yaw + g_off_ahead * 0.0174533f;
        t[0] = p->shown[0] + (g_off_ahead ? sinf(oa) * g_off : g_off), t[1] = p->shown[1], t[2] = p->shown[2] + (g_off_ahead ? cosf(oa) * g_off : 0);
        stand_in_walk(c, p, n);  // (their walk, run, standing: the stand-in's animation)
        return 0;
    }
    if (*(const uint64_t *)(uintptr_t)n != NODE_VTABLE) return 0;
    if (g_move_reset[0].node | g_move_reset[1].node | g_move_reset[2].node | g_move_reset[3].node | g_move_reset[4].node |
        g_move_reset[5].node | g_move_reset[6].node | g_move_reset[7].node) move_reset_check(c, n);  // (a let-go stand-in)
    const void *nt = (const void *)(uintptr_t)(n + 0xe0);
    uint64_t mb = g_me_body;
    if (mb && !g_me_node && !memcmp(nt, (const void *)(uintptr_t)(mb + 0x90), 4) &&
        !memcmp((const void *)(uintptr_t)(n + 0xe8), (const void *)(uintptr_t)(mb + 0x98), 4)) {
        g_me_node = n;
        LOG("[world] our player's node %llx\n", (unsigned long long)n);
    }
    for (int k = 0, m = g_nshow; k < m; k++) {  // (a stand-in's node: the one at its body's spot)
        Peer *s = &g_peers[g_show_idx[k]];
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
// from `me` that isn't stepped and isn't standing in already. Its node: the character's own, through its object (every
// character made is kept - g_made, from the make's hook - and an object leads to its body, dressed_body_of); a walker of
// the crowd's body and its own node part between its animation's step and its body's, so the node at its body's spot
// (node_hook's way, kept for a character made before the hook) can be another one of its (2026-11-16)
#define NMADE 1024
static uint64_t g_made[NMADE];  // (the newest NMADE characters made: their objects - the make request's +0x30)
static volatile unsigned g_nmade;
static uint64_t dressed_body_of(uint64_t owner);
static uint64_t node_of_owner(uint64_t owner);
static uint64_t node_of_body(uint64_t body) {
    unsigned n = g_nmade;
    for (unsigned k = 0; k < NMADE && k < n; k++) {
        uint64_t ow = g_made[(n - 1 - k) % NMADE];
        if (ow && dressed_body_of(ow) == body) return node_of_owner(ow);
    }
    return 0;
}
static uint64_t g_scan_me;
#define LAYER_NPC 4u
// (a body passed over as no person - logged once each: its layer by the names of system/physics/CollisionLayer/
// collision_layer_main.trhcl)
static void not_person_log(uint64_t o, uint32_t layer, float d, const float *at) {
    static const char *const names[32] = {"0", "Ground", "Water", "Player", "NPC", "SymbolPokemon", "FieldGimmick", "Event",
        "ViolentPokemon", "Sound", "Area", "HighHit", "HidingPlace", "EventWall", "InlandOnlySymbolPokemon", "Unused15", "Unused16",
        "BattleWall", "Unstable", "Unused19", "Unused20", "Unused21", "Spawner", "Attack", "Defence", "PlayerNpcWall",
        "ClimbableWall", "FieldTriggerGimmick", "NonPhysicalAttack", "Unused29", "Unused30", "None"};
    static uint64_t seen[64];
    static unsigned nseen;
    for (unsigned i = 0; i < 64 && i < nseen; i++) if (seen[i] == o) return;
    seen[nseen++ % 64] = o;
    LOG("[world] (not %llx, %.1f m from us at %.2f %.2f %.2f: physics layer %u %s - no townsperson)\n", (unsigned long long)o, d,
        at[0], at[1], at[2], layer, names[layer & 31]);
}
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
            bool skip = false;  // (stepped: a walker of the crowd - it follows its route, not its node)
            pthread_mutex_lock(&g_body_lock);
            for (int i = 0; i < NBODY; i++) if (g_bodies[i].o == o && g_frame - g_bodies[i].last <= 30) skip = true;
            pthread_mutex_unlock(&g_body_lock);
            for (int i = 0; i < MAXPEER; i++) if (g_peers[i].body == o) skip = true;
            if (skip || gone_body(o)) continue;  // (a let-go dressed character isn't anyone's stand-in again)
            // (a person only: its proxy's collision layer NPC (4). Not stepped is not enough - by the Vert Pokemon Center a
            // body on layer 14, InlandOnlySymbolPokemon (a Pokemon of the town, not made as a character), was taken and
            // walked about as a player, 2026-11-17)
            uint32_t layer = *(const uint32_t *)((const char *)buf + k * 8 + 0x40) & 0x1f;
            if (layer != LAYER_NPC) {
                not_person_log(o, layer, d, q);
                continue;
            }
            if (nc == NC && d >= cd[NC - 1]) continue;  // (kept sorted, nearest first)
            int j = nc < NC ? nc++ : NC - 1;
            while (j > 0 && cd[j - 1] > d) cand[j] = cand[j - 1], cd[j] = cd[j - 1], j--;
            cand[j] = o, cd[j] = d;
        }
    }
    free(buf);
    // (each one's node, and whether it sits, talks or loops in a pose of its own - those aren't taken: LARECOMP_WORLD_POSE
    // above; a body whose node isn't known yet can't be told - stood up once it stands in, if it must be)
    uint64_t cnode[NC];
    bool cposed[NC];
    int mode = pose_mode();
    char pnames[NC][96];
    for (int j = 0; j < nc; j++) {
        cnode[j] = node_of_body(cand[j]), cposed[j] = false, pnames[j][0] = 0;
        uint64_t bb = mode && cnode[j] ? anim_bb_of_node(cnode[j]) : 0;
        int32_t v[POSE_N];
        if (bb && pose_of(bb, v)) cposed[j] = true, pose_names(v, pnames[j], sizeof pnames[j]);
    }
    int used = 0;
    for (int i = 0; i < MAXPEER && used < nc; i++)
        if (g_peers[i].id && g_peers[i].show && !g_peers[i].body) {
            for (; used < nc; used++) {
                if (cnode[used] && newnpc_number(cnode[used])) continue;  // (one made for a player - ours, or let go)
                if (cposed[used]) {
                    LOG("[world] (not %llx, %.1f m from us: it sits or talks - %s)\n", (unsigned long long)cand[used], cd[used], pnames[used]);
                    continue;
                }
                break;
            }
            if (used == nc) break;
            uint64_t node = cnode[used];
            g_peers[i].node = node, g_peers[i].body = cand[used];  // (no node yet: node_hook finds it at the body's spot)
            LOG("[world] player %016llx: stand-in %llx, %.1f m from us (%d characters; its node %s)%s%s\n", (unsigned long long)g_peers[i].id,
                (unsigned long long)cand[used], cd[used], count, node ? "from its object" : "at its spot", cposed[used] ? " - in a pose: " : "",
                cposed[used] ? pnames[used] : "");
            used++;
        }
    g_scanning = 0;
    return NULL;
}

// ---- A stand-in's collision. Every character's physics presence is its character proxy's own body in the hknp world
// (2026-11-16, read-only Lua research, testout/za-mp5/tools/colf*.lua): a character proxy (BODY_VTABLE, as made at
// 0x700119d2c4 -> 0x700062e0b0 from its description) keeps its body's id at +0x38 (0x8000000001xxxxxx; created there with
// a body description whose filter is the proxy's +0x40) and the world wrapper at +0x50; [wrapper+0x10] = the hknp world,
// whose [+0x38] is the body array, 0xc0 each, indexed by id & 0xffffff (the make writes the record's +0xb8). A record:
// transform (translation +0x30 = the proxy's spot), +0x50 / +0x84 the id's low half, +0x8c the collision filter info
// (= the proxy's +0x40: the player 3, people 4, Pokemon 5 - the layers of system/physics/CollisionLayer/
// collision_layer_main.trhcl: Player 3, NPC 4, SymbolPokemon 5 ... None 0x1f), +0xb0 the whole id. Only the layer pairs
// listed there collide (Player-NPC is, NPC-NPC isn't); None is in no pair. So a stand-in gets its own presence body's
// filter set to None while it stands in - checked to be its own record (id at +0xb0, +0x50, +0x84) before each write, and
// only while the filter is still the one it was made with or ours; its proxy's +0x40 (what its own steps cast with - the
// ground) is left alone; on let-go the record, if still its own, gets its filter back.
//   A walker of the crowd's body follows its node - to the stand-in's spot - so it blocked AT the stand-in like a person
// (walked into: stopped 0.57 m short); a station NPC's body isn't stepped and stays where the NPC stood - an invisible
// wall there (stopped 0.58 m short of its old spot). Both walked through with the filter None (walker 2026-11-16 cw1,
// station 2026-11-16 st1/st0). (Until then a station stand-in's blockers were found by a background heap scan for the bodies
// at its spot - transforms after a header 0x8000000001xxxxxx: its record and copies of it - and put 1 km under the street;
// this is exact and needs no scan.)
// LARECOMP_WORLD_WALKER_COL=1: left as it was (an A/B).
#define PRES_STRIDE 0xc0
#define LAYER_NONE 0x1fu
static uint64_t presence_of(uint64_t proxy, uint64_t *idp) {
    uint64_t id = 0, w = 0, hw = 0, arr = 0, rid = 0;
    uint32_t i1 = 0, i2 = 0;
    if (!heap_read(proxy + 0x38, &id, 8) || !(id >> 63) || !heap_read(proxy + 0x50, &w, 8) || !heap_read(w + 0x10, &hw, 8) ||
        !heap_read(hw + 0x38, &arr, 8) || arr < HEAP_BASE || arr >= HEAP_BASE + HEAP_MAX) return 0;
    uint64_t r = arr + (id & 0xffffff) * PRES_STRIDE;
    if (!heap_read(r + 0xb0, &rid, 8) || rid != id || !heap_read(r + 0x50, &i1, 4) || i1 != (uint32_t)id || !heap_read(r + 0x84, &i2, 4) ||
        i2 != (uint32_t)id) return 0;
    *idp = id;
    return r;
}
static void pres_restore(Peer *p) {
    uint64_t rid = 0;
    if (p->pres_rec && heap_read(p->pres_rec + 0xb0, &rid, 8) && rid == p->pres_id) {
        volatile uint32_t *f = (volatile uint32_t *)(uintptr_t)(p->pres_rec + 0x8c);
        if (*f == LAYER_NONE) {
            *f = p->pres_was;
            LOG("[world] player %016llx's stand-in let go: it blocks again (physics filter %u back)\n", (unsigned long long)p->id, p->pres_was);
        }
    }
    p->pres_body = p->pres_rec = p->pres_id = 0;
}
static bool body_stepped(uint64_t b, uint64_t frame);
static bool pres_put_away(uint64_t body, uint64_t *rec, uint64_t *id, uint32_t *was) {  // (a character's presence: None)
    uint64_t i = 0, r = presence_of(body, &i);
    if (!r) return false;
    volatile uint32_t *f = (volatile uint32_t *)(uintptr_t)(r + 0x8c);
    uint32_t made_with = *(const volatile uint32_t *)(uintptr_t)(body + 0x40);
    if (*f != made_with || made_with == LAYER_NONE) return false;  // (not the filter its proxy made it with: not touched)
    *f = LAYER_NONE, *rec = r, *id = i, *was = made_with;
    return true;
}
static void pres_frame(Peer *p, uint64_t frame) {  // (each frame, a shown player's stand-in)
    (void)frame;
    static int off = -1;
    if (off < 0) off = getenv("LARECOMP_WORLD_WALKER_COL") != NULL;
    bool in = p->body && p->node && is_body(p->body);
    if (p->pres_rec && (!in || p->pres_body != p->body)) pres_restore(p);
    if (!in || off) return;
    if (!p->pres_rec) {
        uint64_t r, id;
        uint32_t was;
        if (!pres_put_away(p->body, &r, &id, &was)) return;
        p->pres_body = p->body, p->pres_rec = r, p->pres_id = id, p->pres_was = was;
        LOG("[world] player %016llx's stand-in: its physics presence (body %llx) no longer blocks (filter %u -> none)\n",
            (unsigned long long)p->id, (unsigned long long)id, was);
        return;
    }
    uint64_t rid = 0;  // (still its own record: again if the game set its own filter back)
    if (!heap_read(p->pres_rec + 0xb0, &rid, 8) || rid != p->pres_id) { p->pres_body = p->pres_rec = p->pres_id = 0; return; }
    volatile uint32_t *f = (volatile uint32_t *)(uintptr_t)(p->pres_rec + 0x8c);
    if (*f == p->pres_was) *f = LAYER_NONE;
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
static uint64_t g_trade_via;   // (that player as the server knows them - see the world's receive loop)
static uint32_t g_trade_code;
static uint64_t g_trade_at;
// (an ask or an answer goes out three times: at once, 0.5 s and 1.5 s later - not three copies in the same instant, which a
// stall or a burst of loss on the way takes together: 2026-11-18, 1 lossy trade in 32 the asker never heard the answer and
// never started. The Link Code is the trade's id - a copy of one already taken is a repeat. Besides: the asker asks again
// every 2 s until the trade starts, the answerer answers such a repeat again (it didn't get there), the answerer's word that
// its room is up (type 9) starts the asker too, and two players who asked each other at once trade on the lower Link Code
// - its asker joins, the other hosts. Research LARECOMP_TRADE_ASK_OLD=1: three copies at once and none of the rest)
static int trade_old(void) {
    static int o = -1;
    if (o < 0) o = getenv("LARECOMP_TRADE_ASK_OLD") && atoi(getenv("LARECOMP_TRADE_ASK_OLD")) > 0;
    return o;
}
static struct { WorldPkt k; uint64_t due; } g_tq[8];  // (the later copies waiting: trade_send_due, each frame)
static int g_tqn;
static uint64_t g_trade_ask_ns;  // (when our ask last went out)
static void trade_send_one(int accept, uint64_t to, uint32_t code) {
    WorldPkt k = {.type = (uint8_t)(accept ? 4 : 3), .ver = 1, .from = g_self};
    k.t.to = to, k.t.code = code;
    world_send(&k, 16 + 12);
    if (!accept) g_trade_ask_ns = world_now_ns();
}
static void trade_send(int accept, uint64_t to, uint32_t code) {
    if (trade_old()) {
        for (int i = 0; i < 3; i++) trade_send_one(accept, to, code);
        return;
    }
    trade_send_one(accept, to, code);
    static const unsigned later_ms[2] = {500, 1500};
    uint64_t now = world_now_ns();
    for (int i = 0; i < 2; i++) {
        if (g_tqn == 8) memmove(g_tq, g_tq + 1, 7 * sizeof g_tq[0]), g_tqn = 7;  // (full: the oldest goes)
        WorldPkt k = {.type = (uint8_t)(accept ? 4 : 3), .ver = 1, .from = g_self};
        k.t.to = to, k.t.code = code;
        g_tq[g_tqn].k = k, g_tq[g_tqn++].due = now + later_ms[i] * 1000000ull;
    }
}
static void trade_send_due(uint64_t now, int drop_asks) {  // (drop_asks: we aren't asking any more - its copies stay home)
    int j = 0;
    for (int i = 0; i < g_tqn; i++) {
        if (drop_asks && g_tq[i].k.type == 3) continue;
        if (g_tq[i].due <= now) world_send(&g_tq[i].k, 16 + 12);
        else g_tq[j++] = g_tq[i];
    }
    g_tqn = j;
}
static struct { uint64_t from; uint32_t code; } g_asks_seen[8];  // (asks we had - their repeats aren't new asks)
static int g_asks_seen_pos;
static bool ask_seen(uint64_t from, uint32_t code) {
    for (int i = 0; i < 8; i++) if (g_asks_seen[i].from == from && g_asks_seen[i].code == code) return true;
    return false;
}
static void ask_note(uint64_t from, uint32_t code) {
    if (ask_seen(from, code)) return;
    g_asks_seen[g_asks_seen_pos].from = from, g_asks_seen[g_asks_seen_pos].code = code, g_asks_seen_pos = (g_asks_seen_pos + 1) % 8;
}
// ---- the link of a walk-up trade. Each game, left alone, looks for a room a few times and then opens one for a few
// seconds, over and over; two copies going in at once both opened rooms and waited for each other (2026-11-16), and the
// fix then - the asker going in 6 s later - still paired only about half the time through a lagged server (2026-11-17:
// the answerer's rooms were up 2.6-3.9 s at a time, the asker's looks waited 250 ms for answers that took 160-600 ms).
// Now the roles are fixed in runtime/services/ldn.c (ldn_trade_role): the answerer hosts, the asker only joins - its
// looks wait until the answerer's room is up. The answerer says so here (type 9 state 1), the asker's looks then find it
// at once (they ask the lobby every second too: an older answerer doesn't say). Not linked by the end of TRADE_LINK_S:
// both are told (state 3), a notice says so, and the game is backed out of its search to the field.
int ldn_trade_state(void);  // (1 our room open, 2 someone joined it, 4 we joined a room, 8 the game is in Link Play)
void ldn_trade_role(int role, unsigned secs);
void ldn_trade_partner(int up);
#define TRADE_LINK_S 75
static struct {
    int role;              // 0 none, 1 we host (we answered), 2 we join (we asked)
    uint64_t with;
    uint32_t code;
    uint64_t start_ns, linked_ns, up_sent_ns;
    int up_sent, matched, active, dropped, retries, again_presses;
    uint64_t inactive_ns, again_ns;
} g_tl;
// (backing the game out of a search that didn't link: stage 1 "Do you want to stop searching?" - B, then A for Yes - as
// long as the game is still searching (it can be slow to open that question while it is busy: the A then went nowhere,
// 2026-11-17 - the next try is A alone, the one after B and A again), up to 6 times; stage 2 once it has stopped: B past "You canceled your search.", then B through Link
// Trade, Link Play and the menu to the field. "Still searching" = an ldn command in the last second, 3 s after the last
// try ended: judged by the 3 s Link Play window instead, a second try came right after the search had stopped - its B
// took the message away and its A opened Nearby Players' Link Code keyboard, which then waited for typing)
uint64_t ldn_trade_active_ns(void);
static struct { int stage, tries; uint64_t tried_ns; } g_bo;
static void trade_link_send(uint8_t state) {
    WorldPkt k = {.type = 9, .ver = 1, .from = g_self};
    k.t.to = g_tl.with, k.t.code = g_tl.code, k.t.state = state;
    world_send(&k, 16 + 13);
}
static void trade_link_end(const char *why, bool tell, bool back_out) {
    LOG("[world] trade: %s (%s, %.1f s after it began)\n", why, g_tl.role == 1 ? "we host" : "we join",
        (double)(world_now_ns() - g_tl.start_ns) / 1e9);
    if (tell)
        for (int i = 0; i < 3; i++) trade_link_send(3);
    ldn_trade_role(0, 0);
    g_tl.role = 0;
    if (!g_tl.matched) g_kbd_auto_once[0] = 0;  // (a Link Code still waiting for a keyboard: not for the next one the player opens)
    if (back_out) g_bo.stage = 1, g_bo.tries = 0, g_bo.tried_ns = world_now_ns() - 3000000000ull;
}
static void back_out_frame(int st, uint64_t now) {
    (void)st;
    if (!g_bo.stage || input_macro_busy()) return;
    if (!g_bo.tried_ns) g_bo.tried_ns = now;  // (a try just ended - or none yet: watch the next 3 s)
    if (now - g_bo.tried_ns < 3000000000ull) return;
    uint64_t a = ldn_trade_active_ns();
    bool searching = a && now - a < 1000000000ull && a > g_bo.tried_ns;
    if (searching && g_bo.tries < 6) {  // (odd tries B then A; even ones A alone - the question came up late and is open)
        g_bo.tries++, g_bo.tried_ns = 0;
        input_macro(g_bo.tries & 1 ? "300ms -\n170ms B\n2000ms -\n170ms A\n2500ms -\n" : "300ms -\n170ms A\n2500ms -\n");
    } else if (searching) {
        g_bo.stage = 0;
        LOG("[world] trade: couldn't stop the game's search - left to the player\n");
    } else {
        g_bo.stage = 0;
        if (g_bo.tries) {
            input_macro("170ms B\n2500ms -\n170ms B\n2500ms -\n170ms B\n2500ms -\n170ms B\n1000ms -\n");
            LOG("[world] trade: the game's search stopped (%d tr%s) - back to the field\n", g_bo.tries, g_bo.tries == 1 ? "y" : "ies");
        }
    }
}
static void trade_link_frame(uint64_t frame) {
    (void)frame;
    uint64_t now = world_now_ns();
    int st = ldn_trade_state();
    back_out_frame(st, now);
    if (!g_tl.role) return;
    if (st & 8) g_tl.active = 1, g_tl.inactive_ns = 0;
    else if (g_tl.active && !g_tl.inactive_ns) g_tl.inactive_ns = now;
    if (g_tl.role == 1) {  // (our room: the other is told when it is up, again every 0.5 s - a lost one - and when it closes;
        int up = st & 1;    //  closed: again every second until linked - it is also our answer, should that be lost)
        bool again = up ? now - g_tl.up_sent_ns >= 500000000ull : !trade_old() && !g_tl.matched && now - g_tl.up_sent_ns >= 1000000000ull;
        if (up != g_tl.up_sent || again) {
            trade_link_send((uint8_t)up);
            if (up != g_tl.up_sent) LOG("[world] trade: our room is %s t %llu\n", up ? "up" : "closed", (unsigned long long)(now / 1000000));
            g_tl.up_sent = up, g_tl.up_sent_ns = now;
        }
    }
    bool linked = (st & 6) != 0;
    if (!linked && g_tl.linked_ns && !g_tl.matched) {
        g_tl.dropped = 1;
        LOG("[world] trade: the link dropped %.1f s after it was made (the game's own session over it failed)\n",
            (double)(now - g_tl.linked_ns) / 1e9);
    }
    if (!linked) g_tl.linked_ns = 0;
    else if (!g_tl.linked_ns) g_tl.linked_ns = now;
    if (!g_tl.matched && g_tl.linked_ns && now - g_tl.linked_ns >= 12000000000ull) {  // (a join the game gives up on - its own
        // session over the link failing - leaves within ~5 s: 1.4 s through the moderate lag, 4.5-4.9 s through the harsh)
        g_tl.matched = 1;
        LOG("[world] trade: linked with %016llx (%s) %.1f s after it began\n", (unsigned long long)g_tl.with,
            g_tl.role == 1 ? "they joined our room" : "we joined their room", (double)(g_tl.linked_ns - g_tl.start_ns) / 1e9);
    }
    // (we joined, then the game's own session over the link failed - through a lagged server it can give up a few seconds
    // in: "No one could be found!", and back to Nearby / Faraway Players once that is dismissed. The one who joins goes
    // again - A past the message, A for Nearby Players, the Link Code answers itself - while the other keeps its room up.
    // One A every 3 s until the game is looking again: the message can take a while to come up, and an A before it went
    // nowhere - a fixed A, A then left the game on Nearby / Faraway, 2026-11-17. Why it failed: lost packets - Pia's mesh
    // join resends each lost step only after 0.5-1 s, the joining game leaves a session that took ~3.5 s or more to come
    // up; the relay plugin now sends every game packet twice over a server, so this is a rare fallback)
    if (g_tl.again_ns) {
        if (st & 8) {
            g_tl.again_ns = 0, g_tl.active = 1, g_tl.inactive_ns = 0;
            LOG("[world] trade: the game is looking again (%d press%s)\n", g_tl.again_presses, g_tl.again_presses == 1 ? "" : "es");
        } else if (now - g_tl.again_ns >= 3000000000ull && !input_macro_busy()) {
            if (g_tl.again_presses >= 4) {
                g_tl.again_ns = 0;  // (not looking again: given up - the search-ended path below tells the other)
                LOG("[world] trade: the game didn't start looking again\n");
            } else {
                g_tl.again_ns = now, g_tl.again_presses++;
                snprintf(g_kbd_auto_once, sizeof g_kbd_auto_once, "%08u", g_tl.code);
                input_macro("170ms A\n300ms -\n");
            }
            return;
        } else return;
    }
    if (g_tl.role == 2 && g_tl.dropped && !g_tl.matched && g_tl.inactive_ns && now - g_tl.inactive_ns >= 1000000000ull &&
        g_tl.retries < 3 && now - g_tl.start_ns < (uint64_t)(TRADE_LINK_S - 15) * 1000000000ull && !input_macro_busy()) {
        g_tl.retries++, g_tl.dropped = 0, g_tl.again_presses = 1, g_tl.again_ns = now;
        snprintf(g_kbd_auto_once, sizeof g_kbd_auto_once, "%08u", g_tl.code);
        input_macro("170ms A\n300ms -\n");
        menu_notice_for("The link dropped - linking up again...", 5000);
        LOG("[world] trade: linking up again (try %d)\n", g_tl.retries + 1);
        return;
    }
    if (g_tl.inactive_ns && now - g_tl.inactive_ns >= 4000000000ull) {  // (out of Link Play: the trade is over, or it was left)
        if (g_tl.matched) trade_link_end("over", false, false);
        else menu_notice_for("The trade search ended before you linked up.", 6000), trade_link_end("search ended before it linked", true, false);
        return;
    }
    if (!g_tl.matched && now - g_tl.start_ns >= (uint64_t)TRADE_LINK_S * 1000000000ull) {
        menu_notice_for("Couldn't link up with the other player for the trade. Back to the field - try again.", 8000);
        trade_link_end("not linked in time", true, (st & 8) != 0);
    }
}
static void trade_start(uint32_t code, unsigned wait_ms, int role);
static void trade_link_message(const WorldPkt *k) {  // (type 9)
    if (!trade_old() && g_trade == 1 && k->from == g_trade_with && k->t.to == g_self && k->t.code == g_trade_code) {
        if (k->t.state <= 1) {  // (they host our trade - their answer itself didn't get here)
            LOG("[world] trade: their room's word came, their answer didn't - we join\n");
            trade_start(k->t.code, 0, 2);
        } else if (k->t.state == 3) {
            g_trade = 0;
            menu_notice_for("The other player couldn't link up or stopped. Try again.", 6000);
            LOG("[world] trade: they gave up before we heard their answer\n");
            return;
        }
    }
    if (!g_tl.role || k->from != g_tl.with || k->t.to != g_self || k->t.code != g_tl.code) return;
    if (k->t.state <= 1 && g_tl.role == 2) {
        static int was;
        ldn_trade_partner(k->t.state);
        if (k->t.state != was) LOG("[world] trade: their room is %s t %llu\n", k->t.state ? "up" : "closed", (unsigned long long)(world_now_ns() / 1000000));
        was = k->t.state;
    } else if (k->t.state == 3 && !g_tl.matched) {
        menu_notice_for("The other player couldn't link up or stopped. Back to the field - try again.", 8000);
        trade_link_end("they gave up", false, (ldn_trade_state() & 8) != 0);
    }
}
// (role: 1 we host - we answered: we go in at once; 2 we join - we asked: we go in at once too, our looks wait for their
// room. Before 2026-11-17 the asker went in 6 s later instead)
static void trade_start(uint32_t code, unsigned wait_ms, int role) {
    snprintf(g_kbd_auto_once, sizeof g_kbd_auto_once, "%08u", code);
    // X (menu), up to Link Play, A; right twice to Link Trade, A; Nearby Players, A; the Link Code entry answers itself
    char m[256];
    snprintf(m, sizeof m, "%ums -\n170ms X\n2500ms -\n170ms UP\n500ms -\n170ms A\n4000ms -\n170ms RIGHT\n500ms -\n170ms RIGHT\n"
             "500ms -\n170ms A\n3000ms -\n170ms A\n1000ms -\n", 200 + wait_ms);
    input_macro(m);
    char t[96];
    snprintf(t, sizeof t, "Trading with the other player (Link Code %04u %04u)", code / 10000, code % 10000);
    menu_notice_for(t, 8000);
    LOG("[world] trade: Link Code %08u (%s) t %llu\n", code, role == 1 ? "we host" : "we join", (unsigned long long)(world_now_ns() / 1000000));
    memset(&g_tl, 0, sizeof g_tl);
    g_tl.role = role, g_tl.with = g_trade_with, g_tl.code = code, g_tl.start_ns = world_now_ns();
    ldn_trade_role(role, TRADE_LINK_S + 30 + wait_ms / 1000);
    g_trade = 0;
    trade_send_due(0, 1);  // (our ask's later copies: not needed now)
}
static void trade_message(const WorldPkt *k, uint64_t frame, uint64_t via) {
    if (k->t.to != g_self || k->t.code >= 100000000u) return;  // (a Link Code has 8 digits)
    if (k->type == 4) {
        if (g_trade == 1 && k->from == g_trade_with && k->t.code == g_trade_code) trade_start(k->t.code, 0, 2);
        return;
    }
    if (!trade_old()) {
        if (g_tl.role && k->from == g_tl.with && k->t.code == g_tl.code) {  // (the trade we're in: they ask again - our answer
            static uint64_t at;                                                //  didn't get there; once a second)
            uint64_t now = world_now_ns();
            if (g_tl.role == 1 && now - at >= 1000000000ull) {
                at = now;
                trade_send_one(1, k->from, k->t.code);
                LOG("[world] trade: they asked again - our answer again\n");
            }
            return;
        }
        if (g_trade == 1 && k->from == g_trade_with && k->t.code != g_trade_code) {  // (we asked each other at once: the
            ask_note(k->from, k->t.code);                                            //  lower Link Code - its asker joins)
            if (k->t.code < g_trade_code) {
                LOG("[world] trade: we asked each other - theirs (code %08u) stands, we answer it\n", k->t.code);
                g_trade_code = k->t.code, g_trade_via = via;
                trade_send(1, g_trade_with, g_trade_code);
                trade_start(g_trade_code, 0, 1);
            } else if (g_tqn == 0) {
                LOG("[world] trade: we asked each other - ours (code %08u) stands, asked again\n", g_trade_code);
                trade_send(0, g_trade_with, g_trade_code);
            }
            return;
        }
        if (g_trade == 2 && k->from == g_trade_with && k->t.code == g_trade_code) return;  // (a repeat)
        if (ask_seen(k->from, k->t.code)) return;  // (an ask we had and moved on from: a repeat, not a new ask)
    }
    if (g_trade == 2 && k->from == g_trade_with && k->t.code == g_trade_code) return;  // (a repeat)
    if (g_trade == 2 && frame - g_trade_at < 600) return;  // (a newer ask within 10 s: the first one stands - no notice spam)
    if (g_trade == 1 && k->from == g_trade_with && k->t.code > g_trade_code) return;  // (both asked at once: the lower code)
    g_trade = 2, g_trade_with = k->from, g_trade_via = via, g_trade_code = k->t.code, g_trade_at = frame;
    ask_note(k->from, k->t.code);
    menu_notice_for("Another player wants to trade! Walk up to them and press A.", 8000);
    LOG("[world] trade asked by %016llx (code %08u)\n", (unsigned long long)k->from, k->t.code);
}
static void trade_frame(uint64_t me, uint64_t frame) {
    // (by: the player whose stand-in we are by - the nearest one within reach; the one who asked us, if within reach, before
    // anyone nearer: with several players standing together, A answers them - 2026-11-16, four players: the last one in
    // the table won, whoever it was)
    Peer *by = NULL;
    float best = TRADE_NEAR * TRADE_NEAR;
    bool by_asker = false;
    if (me)
        for (int i = 0; i < MAXPEER; i++) {
            Peer *p = &g_peers[i];
            if (!p->id || !p->node) continue;
            const float *a = (const float *)(uintptr_t)(me + 0x90);
            float dx = a[0] - p->shown[0] - g_off, dz = a[2] - p->shown[2], d = dx * dx + dz * dz;
            if (d >= TRADE_NEAR * TRADE_NEAR || fabsf(a[1] - p->shown[1]) >= 2) continue;
            bool asker = g_trade == 2 && p->id == g_trade_with;
            if (by_asker && !asker) continue;
            if (asker && !by_asker) by = p, best = d, by_asker = true;
            else if (d < best) by = p, best = d;
        }
    bool busy = input_macro_busy();
    static bool was, eat;  // (eat: the A that asked or answered is kept from the game until it is let go - at 30 fps a
    bool a = (input_buttons_now() & BTN_A_BIT) != 0, press = a && !was;  // 10-frame press outlasted the trade macro's first
    was = a;                                                              // 200 ms, the game took it as Talk to someone near
    if (!a) eat = false;                                                  // and the menu never opened, 2026-11-16)
    g_input_swallow = (by && !busy) || eat ? BTN_A_BIT : 0;  // (the stand-in's own Talk never starts)
    if (g_trade && frame - g_trade_at > 60 * 120) {  // (two minutes: forgotten)
        if (g_trade == 1) menu_notice_for("No answer to the trade request.", 4000);
        g_trade = 0;
    }
    uint64_t now = world_now_ns();
    trade_send_due(now, g_trade != 1);
    if (g_trade == 1 && !trade_old() && now - g_trade_ask_ns >= 2000000000ull) trade_send_one(0, g_trade_with, g_trade_code);  // (again
    // every 2 s until they answer: a lost ask, or their answer lost - see trade_send)
    bool theirs = by && g_trade && by->id == g_trade_with;
    static uint64_t hint_at;  // (by a stand-in: what A does there, in the corner while by)
    if (by && !busy && frame - hint_at >= 30) {
        hint_at = frame;
        menu_notice_for(theirs && g_trade == 2 ? "A: accept this player's trade" : theirs && g_trade == 1 ? "Trade asked: waiting for them" :
                        "A: ask this player to trade", 700);
    }
    if (!by || busy || !press) return;
    eat = true;
    if (theirs && g_trade == 2) {
        trade_send(1, g_trade_with, g_trade_code);
        trade_start(g_trade_code, 0, 1);
    } else if (!theirs || g_trade == 0) {
        g_trade_code = 10000000u + (uint32_t)(world_now_ns() / 1000 % 89999999u);
        g_trade = 1, g_trade_with = by->id, g_trade_via = by->via, g_trade_at = frame;
        trade_send(0, g_trade_with, g_trade_code);
        menu_notice_for("Trade request sent. Waiting for them to walk up to you and press A.", 6000);
        LOG("[world] trade asked of %016llx (code %08u)\n", (unsigned long long)g_trade_with, g_trade_code);
    } else {
        trade_send(0, g_trade_with, g_trade_code);  // (again, in case it was lost)
    }
}

// a stand-in let go: its node back where it stood (a station NPC's isn't written again by the game: it stayed where
// the player was last seen, 2026-11-16) - unless it went with its level
static void pres_restore(Peer *p);
static void standin_release(Peer *p) {
    stand_in_stop(p);  // (back to standing, from its node's next update)
    pres_restore(p);   // (its physics presence: blocking again)
    uint64_t n = p->saved_node, b = p->body;
    p->node = 0, p->saved_node = 0;
    if (!n || !b || !is_body(b) || *(const uint64_t *)(uintptr_t)n != NODE_VTABLE) return;
    memcpy((void *)(uintptr_t)(n + 0xd0), p->saved_q, 16), memcpy((void *)(uintptr_t)(n + 0xe0), p->saved_t, 12);
}

// ---- What another player may wear: only what the game's own data has. A look naming a variant the game has no file
// for in that part (2026-11-16: the other body's p2_ hoodie in the upper-body slot of a p1 body - the slot's p1_ name
// is the one the game has) kept the level from ever finishing loading, so a peer's look and outfit are checked first.
// The names: every variant the game knows is a FlatBuffers string {u32 length, the name, NUL} in its loaded data - the
// parts' character-creation files (ik_chara/savm_pc/<part>/<part>.ccdatam: each variant's full name, e.g.
// p1_upb0000_00_blousonshirt_normal_r00_c00, p0_hrs0002_00_medium, p1_drs9999_00_none, p0_base; an item's own folder
// name is only inside its .ccdata path, never a string of its own) and the dress-up table (world/exl/dress_up_data:
// every wearable item, p2_ names). One scan of the heap (a thread, once we are in the world; again, rarely, when a look
// names something unknown) keeps each such name's hash (the game's FNV-1a 64), its part and its body (p0 any, p1 / p2:
// the body-shaped parts drs / lwb / upb - also kept under the other body's prefix, both bodies' files having the same
// items). Our own memory (HLE: descriptions made from peers' strings) is outside the heap: it can't vouch for itself.
// The look's 13 slots are base, acc, bag, eyw, ftw, glv, hdw, hrs, lgw, drs, face, lwb, upb (checked: a save's 13 hashes
// were those parts' names, in that order); the body-shaped three must share one body. Options aren't checked (all 18 at
// 250 made odd hair, nothing worse) beyond keeping them under 64. The blob's third field is the BODY (2026-11-16, a new
// game's other body: the player's dress-up component +0x2f4 = 2, a p1 player's 1 - the deserialiser 0x7000479480 reads
// only fields 0 and 2), so it comes from the clothes' own p1_/p2_ names; its second (+0x2e8) is 1 for both. A p2 look sent
// with body 1 (as before) kept the other copy's level from ever finishing loading, as any body mismatch does.
#define VSET_N 65536u
static struct VsetEnt { uint64_t h; uint8_t slot, body; } *g_vset;
static volatile int g_vset_state;  // 0 never scanned, 1 scanning, 2 ready
static uint64_t g_vset_at;         // (the frame of the last scan)
static int g_vset_scans, g_vset_count;
static volatile int g_vset_want;   // (a look named something unknown: scan again, rarely)
static const char *const k_slot_type[13] = {"base", "acc", "bag", "eyw", "ftw", "glv", "hdw", "hrs", "lgw", "drs", "face", "lwb", "upb"};
static void vset_add(struct VsetEnt *t, uint64_t h, int slot, int body) {
    for (uint32_t i = (uint32_t)(h ^ h >> 32) & (VSET_N - 1), n = 0; n < VSET_N; i = (i + 1) & (VSET_N - 1), n++) {
        if (t[i].h == h) return;
        if (!t[i].h) { t[i].h = h, t[i].slot = (uint8_t)slot, t[i].body = (uint8_t)body; g_vset_count++; return; }
    }
}
static int vset_slot(uint64_t h, int *body) {  // (-1: not a variant the game has)
    if (g_vset_state != 2 || !h) return -1;
    for (uint32_t i = (uint32_t)(h ^ h >> 32) & (VSET_N - 1), n = 0; n < VSET_N; i = (i + 1) & (VSET_N - 1), n++) {
        if (!g_vset[i].h) return -1;
        if (g_vset[i].h == h) { if (body) *body = g_vset[i].body; return g_vset[i].slot; }
    }
    return -1;
}
// a name's part (slot) and body; -1 not a variant name: "p<0-2>_<part><4 digits>_<2 digits>_..." or "p0_base"
static int variant_slot(const char *s, size_t n, int *body) {
    if (n < 7 || s[0] != 'p' || s[1] < '0' || s[1] > '2' || s[2] != '_') return -1;
    *body = s[1] - '0';
    if (n == 7 && !memcmp(s, "p0_base", 7)) return 0;
    size_t t = 3;
    while (t < n && s[t] >= 'a' && s[t] <= 'z') t++;
    if (t + 8 > n || t == 3) return -1;
    for (int k = 0; k < 4; k++) if (s[t + k] < '0' || s[t + k] > '9') return -1;
    if (s[t + 4] != '_' || s[t + 5] < '0' || s[t + 5] > '9' || s[t + 6] < '0' || s[t + 6] > '9') return -1;
    for (int k = 1; k < 13; k++)
        if (strlen(k_slot_type[k]) == t - 3 && !memcmp(s + 3, k_slot_type[k], t - 3)) {
            bool shaped = k == 9 || k == 11 || k == 12;
            return shaped == (*body != 0) ? k : -1;
        }
    return -1;
}
static uint64_t gf_hash(const char *s);
static void vset_chunk(struct VsetEnt *t, const uint8_t *b, size_t len) {
    for (const uint8_t *p = b + 4; p + 8 < b + len && (p = memchr(p, 'p', (size_t)(b + len - 8 - p)));) {
        uint32_t sl;
        memcpy(&sl, p - 4, 4);
        if (sl < 7 || sl > 90 || p + sl >= b + len || p[sl] || p[1] < '0' || p[1] > '2' || p[2] != '_') { p++; continue; }
        bool ok = true;
        for (uint32_t k = 0; k < sl && ok; k++)
            ok = (p[k] >= 'a' && p[k] <= 'z') || (p[k] >= '0' && p[k] <= '9') || p[k] == '_' || (p[k] >= 'A' && p[k] <= 'Z');
        int body, slot = ok ? variant_slot((const char *)p, sl, &body) : -1;
        if (slot >= 0) {
            char name[96];
            memcpy(name, p, sl), name[sl] = 0;
            vset_add(t, gf_hash(name), slot, body);
            if (body) name[1] = body == 1 ? '2' : '1', vset_add(t, gf_hash(name), slot, 3 - body);
        }
        p += ok && slot >= 0 ? sl : 1;
    }
}
static void *vset_scan(void *arg) {
    (void)arg;
    struct VsetEnt *t = calloc(VSET_N, sizeof *t);
    const size_t chunk = 1u << 20, over = 128;
    uint8_t *buf = malloc(chunk + over);
    int count0 = g_vset_count;
    g_vset_count = 0;
    for (uint64_t a = HEAP_BASE; t && buf && a < HEAP_BASE + HEAP_MAX; a += chunk) {
        size_t len = chunk + over;
        if (!heap_read(a, buf, len)) { len = chunk; if (!heap_read(a, buf, len)) continue; }
        vset_chunk(t, buf, len);
    }
    free(buf);
    pthread_mutex_lock(&g_dress_lock);  // (and our own template's - our own game's - whatever the scan saw)
    for (int k = 0; t && k < g_npparts && k < 24; k++) {
        int body, slot = variant_slot(g_pparts[k].var, strlen(g_pparts[k].var), &body);
        if (slot >= 0) vset_add(t, gf_hash(g_pparts[k].var), slot, body);
    }
    pthread_mutex_unlock(&g_dress_lock);
    if (t && g_vset_count >= 1000) {  // (the game's data in: kept; the old table stays readable a while - not freed)
        g_vset = t;
        __atomic_store_n(&g_vset_state, 2, __ATOMIC_RELEASE);
        LOG("[world] %d variants the game has (parts' files, the dress-up table): other players' looks are checked against them\n", g_vset_count);
    } else {
        LOG("[world] only %d variant names in memory: checked again later\n", g_vset_count);
        free(t);
        g_vset_count = count0;
        __atomic_store_n(&g_vset_state, g_vset ? 2 : 0, __ATOMIC_RELEASE);
    }
    return NULL;
}
static void vset_frame(uint64_t frame, bool in_world) {  // (ghost_frame: the scan started once we are in the world)
    if (!in_world || g_vset_state == 1) return;
    bool again = g_vset_state == 2 && g_vset_want && g_vset_scans < 4 && frame - g_vset_at > 3600;
    bool first = g_vset_state == 0 && (!g_vset_at || frame - g_vset_at > 600);
    if (!first && !again) return;
    g_vset_want = 0, g_vset_at = frame, g_vset_scans++, g_vset_state = 1;
    pthread_t th;
    if (pthread_create(&th, NULL, vset_scan, NULL)) g_vset_state = g_vset ? 2 : 0; else pthread_detach(th);
}
// a peer's look as it may be worn (options kept under 64, the flags 1); false: refused (`why` says which part), or
// not known yet (*pending: the names not scanned yet)
static bool look_check(Look *l, char *why, size_t wn, bool *pending) {
    *pending = g_vset_state != 2;
    if (*pending) { snprintf(why, wn, "not checked yet"); return false; }
    int shaped = -1;
    for (int k = 0; k < 13; k++) {
        int body = 0, slot = vset_slot(l->part[k], &body);
        if (slot != k) {
            snprintf(why, wn, "its %s (%016llx) isn't a %s the game has", k_slot_type[k], (unsigned long long)l->part[k], k_slot_type[k]);
            g_vset_want = 1;
            return false;
        }
        if (k == 9 || k == 11 || k == 12) {
            if (shaped >= 0 && body != shaped) { snprintf(why, wn, "its clothes are for two different bodies"); return false; }
            shaped = body;
        }
    }
    for (int k = 0; k < 18; k++) if (l->opt[k] > 63) l->opt[k] = 63;
    l->f1 = 1, l->f2 = (uint8_t)shaped;  // (shaped: 1 or 2 - the body-shaped parts' body, checked above)
    return true;
}
// a peer's outfit part (type 5) as it may go into a description: one of the 13 parts (by name), the part's own file
// (ours: every player's character is made from the same template) and a variant of that part the game has, for the
// template's body; false: refused
static bool outfit_part_check(DressPart *p) {
    int slot = -1;
    for (int k = 0; k < 13; k++) if (!strcmp(p->name, k_slot_type[k])) slot = k;
    if (slot < 0) return false;
    bool ok = false;
    pthread_mutex_lock(&g_dress_lock);
    for (int k = 0; k < g_npparts; k++)
        if (!strcmp(g_pparts[k].name, p->name) && !strcmp(g_pparts[k].path, p->path) && g_pparts[k].var[1] == p->var[1]) ok = true;
    pthread_mutex_unlock(&g_dress_lock);
    int body = 0;
    return ok && vset_slot(gf_hash(p->var), &body) == slot;
}

// ---- outfits: ours sent part by part (type 5), theirs kept per player; a player whose whole outfit is heard gets the
// next townsperson made dressed in it (below, dress_hook), which then stands in for them
static uint64_t dressed_body_of(uint64_t owner);
static uint64_t node_of_owner(uint64_t owner);
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
static bool outfit_part_check(DressPart *p);
static void outfit_message(Peer *p, const WorldPkt *k, size_t n) {
    // (13 parts, as every player's template has; each one a part of the template with a variant the game has for it -
    // anything else refused, see outfit_part_check; their body type: the template's)
    if (!p || g_npparts != 13 || k->o.n != 13 || k->o.k >= k->o.n) return;  // (not heard where they are yet: resent)
    DressPart part;
    if (!outfit_get(k, n, &part)) return;
    if (g_vset_state != 2) return;  // (the game's names not known yet: it comes again)
    if (!outfit_part_check(&part)) {
        if (!p->refused++) LOG("[world] player %016llx's outfit refused: its %.31s \"%.95s\" isn't one the game has\n", (unsigned long long)p->id,
                               part.name, part.var);
        return;
    }
    if (p->nparts != k->o.n) p->nparts = k->o.n, p->have = 0, p->have_in = 0, p->outfit_tag = 0;
    p->body_type = g_pbody;
    uint32_t tag, all = (1u << p->nparts) - 1;
    memcpy(&tag, k->pad, 4);
    p->poked = 1;
    if (tag) {  // (tagged: kept aside until every part carries the same whole-outfit hash)
        p->parts_in[k->o.k] = part, p->parts_tag[k->o.k] = tag, p->have_in |= 1u << k->o.k;
        if (p->have_in != all) return;
        for (int i = 0; i < p->nparts; i++) if (p->parts_tag[i] != tag) return;
        if (p->outfit_tag == tag && p->have == all) return;  // (the same again)
        memcpy(p->parts, p->parts_in, sizeof p->parts);
        p->have = all, p->outfit_tag = tag;
    } else {  // (an older copy: part by part)
        p->parts[k->o.k] = part;
        p->have |= 1u << k->o.k;
        if (p->have != all) return;
    }
    uint32_t h = outfit_hash(p->parts, p->nparts);
    if (tag && h != tag && !p->hash_logged++)  // (their hash of it and ours differ: a part didn't come over exactly - logged once)
        LOG("[world] player %016llx's outfit hashes %08x here, %08x as sent\n", (unsigned long long)p->id, h, tag);
    if (h == p->hash) return;
    p->hash = h;
    LOG("[world] player %016llx's outfit: %d parts (%08x), hat %s, top %s\n", (unsigned long long)p->id, p->nparts, h,
        p->nparts > 6 ? p->parts[6].var : "?", p->parts[p->nparts - 1].var);
}
// (LARECOMP_GHOST_SELF, one copy: we are the other player too - our first townsperson is dressed for "them" at once)
void world_self_outfit(void) {
    // (a changed look: checked, as another player's would be; their own townsperson: made from the packets as it is asked for)
    if (!g_self_test || getenv("LARECOMP_WORLD_OUTFIT") || getenv("LARECOMP_WORLD_LOOK") || own_npc()) return;
    DressPart o[MAXPARTS];
    int body, n = outfit_ours(o, &body);
    pthread_mutex_lock(&g_dress_lock);
    Look l;
    if (!look_ours(&l)) { pthread_mutex_unlock(&g_dress_lock); return; }  // (the save's look isn't there yet: as packets)
    // (its body, as look_check would find it: our upper body's p1_/p2_ name, once the names are known - at our player's
    // make they aren't yet: LARECOMP_GHOST_SELF_BODY=2 for a save of the other body, whose look made with body 1 kept the
    // level from ever finishing loading, 2026-11-17 testroot-mpP2)
    int lbody = 0, given = getenv("LARECOMP_GHOST_SELF_BODY") ? atoi(getenv("LARECOMP_GHOST_SELF_BODY")) : 1;
    if (vset_slot(l.part[12], &lbody) != 12 || (lbody != 1 && lbody != 2)) lbody = given;
    l.f1 = 1, l.f2 = (uint8_t)(lbody == 2 ? 2 : 1);
    g_dress_want.peer = g_self ^ 2, g_dress_want.n = n, g_dress_want.body = body, g_dress_want.look = l, g_dress_want.look_ok = true;
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
static void gone_add(uint64_t owner, uint64_t peer, uint64_t col_body);
static void dressed_add(uint64_t peer, uint64_t owner, uint32_t hash) {  // (a character made for them: kept until it stands in)
    pthread_mutex_lock(&g_dress_lock);
    bool full = g_ndressed == 8;
    int i = full ? 7 : g_ndressed++;
    uint64_t lost = full ? g_dressed[7].owner : 0, lost_peer = g_dressed[7].peer;
    g_dressed[i].peer = peer, g_dressed[i].owner = owner, g_dressed[i].hash = hash;
    pthread_mutex_unlock(&g_dress_lock);
    if (lost) gone_add(lost, lost_peer, 0);  // (no room to keep it: put away, not left in their clothes)
}
static void dressed_drop(uint64_t owner) {
    pthread_mutex_lock(&g_dress_lock);
    for (int i = 0; i < g_ndressed; i++) if (g_dressed[i].owner == owner) g_dressed[i] = g_dressed[--g_ndressed], i--;
    pthread_mutex_unlock(&g_dress_lock);
}
// ---- A dressed character let go. Its look is fixed when it is made (the description and the DressUp are read by the
// make only), so it can't be given its own clothes back; left alone it went on as a townsperson in that player's
// clothes and face - back where it first stood (a station NPC) or walking with the crowd (2026-11-16). So it is put away
// for good: its node 500 m under the street at every update (a station NPC's node isn't written by the game, so it stays
// there - drawn nowhere, its Talk out of reach; a walker's body follows its node, and is put there before each of its
// steps too: the crowd lets a walker that far off its route go - removed, as 2026-11-14's stray walkers were) and its
// collision (its physics presence's filter None, as pres_frame puts a stand-in's away: a station NPC's stays where it
// stood) kept away - until the game removes the character (its level goes, the crowd lets it go: its object or its body gone).
static void gone_add(uint64_t owner, uint64_t peer, uint64_t col_body) {
    if (!owner) return;
    pthread_mutex_lock(&g_gone_lock);
    int f = -1;
    for (int i = 0; i < MAXGONE; i++) {
        if (g_gone[i].owner == owner) { f = -2; break; }
        if (f == -1 && !g_gone[i].owner) f = i;
    }
    if (f == -1) {  // (full: the oldest one goes back to the game as it is)
        f = 0;
        for (int i = 1; i < MAXGONE; i++) if (g_gone[i].since < g_gone[f].since) f = i;
        g_gone[f].owner = 0, g_gone[f].node = 0, g_gone[f].body = 0;
    }
    if (f >= 0) {
        g_gone[f].body = 0, g_gone[f].node = 0, g_gone[f].since = g_frame, g_gone[f].peer = peer, g_gone[f].col_body = col_body;
        g_gone[f].lost_at = 0;
        __atomic_store_n(&g_gone[f].owner, owner, __ATOMIC_RELEASE);
        int n = 0;
        for (int i = 0; i < MAXGONE; i++) n += g_gone[i].owner != 0;
        g_ngone = n;
    }
    pthread_mutex_unlock(&g_gone_lock);
    if (f >= 0)
        LOG("[world] player %016llx's dressed character %llx let go: put away until the game removes it\n", (unsigned long long)peer,
            (unsigned long long)owner);
}
static void gone_drop(int i) {
    pthread_mutex_lock(&g_gone_lock);
    g_gone[i].owner = 0, g_gone[i].node = 0, g_gone[i].body = 0;
    int n = 0;
    for (int k = 0; k < MAXGONE; k++) n += g_gone[k].owner != 0;
    g_ngone = n;
    pthread_mutex_unlock(&g_gone_lock);
}
// (each frame, also with the shared world off: its node and body found once it has them - the node is the one it had
// then, for good: an object of the same address later is another character. Forgotten once the game removed it: its
// object no longer a character, or its node no longer its own (2026-11-16: 1.1 s under the street, a let-go station NPC
// and walkers alike lost their bodies - the object stays a while without one: still kept away, by its node), or, never
// placed, after 20 s)
static void gone_frame(uint64_t frame) {
    if (!g_ngone) return;
    for (int i = 0; i < MAXGONE; i++) {
        uint64_t ow = g_gone[i].owner, v = 0;
        if (!ow) continue;
        bool obj = heap_read(ow, &v, 8) && v == CHAR_OBJ_VTABLE;
        uint64_t n = obj ? node_of_owner(ow) : 0, b = obj ? dressed_body_of(ow) : 0;
        double secs = (double)(frame - g_gone[i].since) / 60.0;
        if (!g_gone[i].node) {
            if (!n || !b) {
                if (!obj || frame - g_gone[i].since > 1200) LOG("[world] a let-go dressed character (%llx) never placed: forgotten\n", (unsigned long long)ow), gone_drop(i);
                continue;
            }
            g_gone[i].node = n, g_gone[i].body = b;
            continue;
        }
        if (!obj || n != g_gone[i].node) {
            LOG("[world] a let-go dressed character (%llx) is gone: the game removed it (%.1f s after)\n", (unsigned long long)ow, secs);
            gone_drop(i);
            continue;
        }
        if (b != g_gone[i].body) {
            if (b) g_gone[i].body = b;  // (another body: kept away too)
            else if (!g_gone[i].lost_at) {
                g_gone[i].lost_at = frame, g_gone[i].body = 0;
                LOG("[world] a let-go dressed character (%llx) lost its body (%.1f s after): its object stays, kept away by its node\n", (unsigned long long)ow, secs);
            }
        }
        if (b && b != g_gone[i].col_body) {  // (its collision: away for good - never put back)
            uint64_t r, id;
            uint32_t was;
            if (pres_put_away(b, &r, &id, &was))
                LOG("[world] a let-go dressed character (%llx): its physics presence (body %llx) no longer blocks\n", (unsigned long long)ow,
                    (unsigned long long)id);
            g_gone[i].col_body = b;
        }
    }
}
// (a player's stand-in let go: if it is their dressed character it is put away, its collision with it - never put back)
static void standin_let_go(Peer *p, uint64_t owner) {
    bool dressed = owner && p->body && p->body == dressed_body_of(owner);
    if (!dressed) { standin_release(p); return; }
    uint64_t col = p->pres_rec && p->pres_body == p->body ? p->body : 0;
    p->pres_body = p->pres_rec = p->pres_id = 0, p->saved_node = 0;  // (no collision or node back where it stood: it goes)
    standin_release(p);
    p->body = 0;
    gone_add(owner, p->id, col);
}
// (once a second: a character dressed for a player who isn't shown any more - made just as they left, the dressing
// already under way - is put away too)
static void dressed_sweep(void) {
    uint64_t gone[8], peer[8];
    int ng = 0;
    pthread_mutex_lock(&g_dress_lock);
    for (int i = 0; i < g_ndressed; i++) {
        bool shown = false;
        for (int k = 0; k < MAXPEER; k++) shown |= g_peers[k].id && g_peers[k].id == g_dressed[i].peer && g_peers[k].show;
        if (shown) continue;
        if (ng < 8) gone[ng] = g_dressed[i].owner, peer[ng++] = g_dressed[i].peer;
        g_dressed[i] = g_dressed[--g_ndressed], i--;
    }
    pthread_mutex_unlock(&g_dress_lock);
    for (int i = 0; i < ng; i++) gone_add(gone[i], peer[i], 0);
}
// a player no longer shown (or gone): their stand-in goes back, their dressed characters (the one standing in, ones made
// and not placed yet) are put away (above) and a dressing asked for them isn't made
static void standin_hide(Peer *p) {
    uint64_t gone[8];
    int ng = 0;
    if (p->dressed_owner) gone[ng++] = p->dressed_owner;
    standin_let_go(p, p->dressed_owner);
    pthread_mutex_lock(&g_dress_lock);
    for (int i = 0; i < g_ndressed; i++)
        if (g_dressed[i].peer == p->id) {
            if (ng < 8 && g_dressed[i].owner != p->dressed_owner) gone[ng++] = g_dressed[i].owner;
            g_dressed[i] = g_dressed[--g_ndressed], i--;
        }
    if (g_dress_want.peer == p->id) g_dress_want.peer = 0;
    pthread_mutex_unlock(&g_dress_lock);
    for (int i = 0; i < ng; i++) gone_add(gone[i], p->id, 0);
    p->body = p->node = 0;
    p->dressed_body = p->dressed_owner = 0, p->dressed_hash = p->armed_hash = 0, p->armed_at = p->dressed_at = 0;
    p->show = 0;
}
// ---- Who is shown: the nearest players. Everyone on the server in the shared world is heard (up to MAXPEER), but only
// the N nearest to our player (LARECOMP_WORLD_SHOW=<n>[,<metres>]: 4 within 150 m unless set, n at most MAXSHOW) get a
// stand-in - each one is a townsperson taken from the street and, once dressed, a crowd character of their own. Chosen
// again every second and whenever someone comes or goes; without flicker: a shown player keeps their place for 5 s at
// least and is counted 8 m nearer than they are (one not shown must be clearly nearer to take it), and the range is
// 20 m longer for them. Who loses their place is let go like a player who left (standin_hide).
static int g_show_n = -1;
static float g_show_range;
static volatile int g_choose_soon;
static void world_choose(uint64_t me, uint64_t frame) {
    if (g_show_n < 0) {
        const char *e = getenv("LARECOMP_WORLD_SHOW");
        int n = 4;
        float r = 150;
        if (e) sscanf(e, "%d,%f", &n, &r);
        g_show_n = n < 0 ? 0 : n > MAXSHOW ? MAXSHOW : n, g_show_range = r > 10 ? r : 10;
        if (e) LOG("[world] the %d nearest players within %.0f m are shown\n", g_show_n, g_show_range);
    }
    if (!me) return;  // (no player: as it was - a level loading)
    const float *a = (const float *)(uintptr_t)(me + 0x90);
    int order[MAXPEER], n = 0;
    float score[MAXPEER];
    for (int i = 0; i < MAXPEER; i++) {
        Peer *p = &g_peers[i];
        if (!p->id) continue;
        float dx = p->pos[0] - a[0], dy = p->pos[1] - a[1], dz = p->pos[2] - a[2];
        p->dist = sqrtf(dx * dx + dy * dy + dz * dz);
        if (p->dist > g_show_range + (p->show ? 20 : 0) && !(p->show && frame - p->shown_at < 300)) continue;
        float sc = p->show ? (frame - p->shown_at < 300 ? -1e9f + p->dist : p->dist - 8) : p->dist;
        int j = n++;
        while (j > 0 && score[j - 1] > sc) order[j] = order[j - 1], score[j] = score[j - 1], j--;
        order[j] = i, score[j] = sc;
    }
    bool want[MAXPEER] = {0};
    for (int k = 0; k < n && k < g_show_n; k++) want[order[k]] = true;
    int shown = 0, heard = 0;
    for (int i = 0; i < MAXPEER; i++) heard += g_peers[i].id != 0;
    for (int i = 0; i < MAXPEER; i++) {  // (first the ones losing their place: a stand-in of theirs may be the next one's)
        Peer *p = &g_peers[i];
        if (p->id && p->show && !want[i]) {
            LOG("[world] player %016llx no longer shown (%.0f m away): nearer players take their place\n", (unsigned long long)p->id, p->dist);
            standin_hide(p);
        }
    }
    uint8_t idx[MAXSHOW];
    for (int i = 0; i < MAXPEER; i++) {
        Peer *p = &g_peers[i];
        if (!p->id || !want[i]) continue;
        if (!p->show) {
            p->show = 1, p->shown_at = frame;
            LOG("[world] player %016llx shown (%.0f m away; %d heard)\n", (unsigned long long)p->id, p->dist, heard);
            if (!p->noticed) p->noticed = 1, menu_notice_for("Another player is nearby.", 3000);
        }
        if (shown < MAXSHOW) idx[shown++] = (uint8_t)i;
    }
    // (the guest threads' hooks read the list while it changes: a slot seen twice or missed for one frame is harmless)
    for (int k = 0; k < shown; k++) g_show_idx[k] = idx[k];
    g_nshow = shown;
}
static void outfit_frame(Peer *p, uint64_t frame) {
    if (p->dressed_body && (!is_body(p->dressed_body) || dressed_body_of(p->dressed_owner) != p->dressed_body)) {
        LOG("[world] player %016llx's dressed character is gone: the next one made wears their outfit\n", (unsigned long long)p->id);
        if (p->body == p->dressed_body) stand_in_gone(p);
        dressed_drop(p->dressed_owner);
        p->dressed_body = p->dressed_owner = 0, p->dressed_hash = p->armed_hash = 0, p->armed_at = 0;  // (asked for again at once)
    }
    // (their outfit and their look - the save's dress-up, worn as the character is made - both heard: the key of what
    // their character must wear)
    if (p->look_have == 3 && !p->look_state) {  // (their look checked once the game's names are known)
        char why[160];
        bool pending;
        if (look_check(&p->look, why, sizeof why, &pending)) {
            uint32_t h = 2166136261u;
            for (size_t b = 0; b < sizeof p->look; b++) h = (h ^ ((const uint8_t *)&p->look)[b]) * 16777619u;
            p->look_hash = h, p->look_state = 1;
            LOG("[world] player %016llx's look heard (skin %u, eyes %u, hair colour %u)\n", (unsigned long long)p->id, p->look.opt[0],
                p->look.opt[3], p->look.opt[11]);
            char t[13 * 17 + 1];  // (all of it: line for line comparable with their own "our look" lines)
            size_t w = 0;
            for (int k = 0; k < 13; k++) w += (size_t)snprintf(t + w, sizeof t - w, " %016llx", (unsigned long long)p->look.part[k]);
            LOG("[world]   its parts%s\n", t);
            w = 0;
            for (int k = 0; k < 18; k++) w += (size_t)snprintf(t + w, sizeof t - w, " %u", p->look.opt[k]);
            LOG("[world]   its options%s\n", t);
        } else if (!pending) {
            p->look_state = 2;
            if (!p->look_refused_at || frame - p->look_refused_at > 1800)
                p->look_refused_at = frame, LOG("[world] player %016llx's look refused: %s - shown in their outfit alone\n", (unsigned long long)p->id, why);
        }
    }
    if (own_npc()) {  // (their own can be made here: a station of a loaded level kept - none for 5 s, one borrowed: below, ghost_frame)
        if (newnpc_ready()) p->own_none_at = 0;
        else if (!p->own_none_at) p->own_none_at = frame;
    }
    uint32_t key = p->hash && p->look_state ? p->hash ^ (p->look_state == 1 ? p->look_hash : 0x5eed) : 0;
    bool settled = g_me_body && g_me_node && frame - g_me_since > 90;
    if (own_npc() && !key && !p->dressed_owner && p->shown_at && frame - p->shown_at > 480) key = 1;  // (their outfit not heard 8 s
    if (key != p->key_seen) p->key_seen = key, p->key_ns = world_now_ns();                             //  after they were shown: one
    if (!key || p->dressed_hash == key) return;                                                         //  as it is)
    bool own = own_npc() && !p->own_none_at, borrow = !own_npc() || frame - p->own_none_at > 300;
    float reach = own_range(), d2 = 0;
    if (g_me_body) {
        const float *m = (const float *)(uintptr_t)(g_me_body + 0x90);
        d2 = (p->shown[0] - m[0]) * (p->shown[0] - m[0]) + (p->shown[1] - m[1]) * (p->shown[1] - m[1]) +
             (p->shown[2] - m[2]) * (p->shown[2] - m[2]);  // (up and down too: one falling off the level is far)
    }
    if (own && d2 > reach * reach) {  // (the game realises characters near the player only: one made farther off isn't, until we come by)
        if (!p->own_far) p->own_far = 1, LOG("[world] player %016llx is %.0f m from us: their own townsperson once they are within %.0f m\n",
                                             (unsigned long long)p->id, sqrtf(d2), reach);
        return;
    }
    p->own_far = 0;
    uint64_t owner = 0;
    pthread_mutex_lock(&g_dress_lock);
    for (int i = 0; i < g_ndressed; i++) if (g_dressed[i].peer == p->id && g_dressed[i].hash == key) owner = g_dressed[i].owner;
    if (!owner && own) {  // (their own, made now where they are: asked for again if it isn't made within 5 s or wasn't one)
        if (g_dress_want.peer == p->id) g_dress_want.peer = 0;
        int i = p->own_k % NOWN;
        // (made 0.0-0.1 s after it is asked for; not made within 2 s - its spawner's area was just going or coming, after a
        // put-back: never made, 2026-11-17 - another station's then)
        bool waiting = p->own_k && g_own[i].k == p->own_k && g_own[i].key == key && !g_own[i].failed && frame - p->own_at < (g_own[i].made ? 1200u : 120u);
        if (!waiting && p->own_k && g_own[i].k == p->own_k && !g_own[i].made && frame - p->own_at >= 120) newnpc_missed(p->own_k), p->own_k = 0;
        int k = !waiting && settled && (!p->own_at || frame - p->own_at >= 60) && g_dressings < 1000 ? newnpc_spawn(p->shown, p->shown_yaw) : 0;
        if (k) {
            i = k % NOWN;
            g_own[i].k = k, g_own[i].failed = 0, g_own[i].made = 0, g_own[i].peer = p->id, g_own[i].key = key;
            g_own[i].n = key == 1 ? 0 : p->nparts, g_own[i].body = p->body_type, g_own[i].look = p->look, g_own[i].look_ok = key != 1 && p->look_state == 1;
            memcpy(g_own[i].parts, p->parts, sizeof p->parts);
            g_own_used = 1;
            p->own_k = k, p->own_at = frame, p->armed_hash = key, p->dressed_at = 0;
            LOG("[world] player %016llx: their own townsperson newnpc_%03d asked for at %.1f %.1f %.1f (%s)\n", (unsigned long long)p->id, k, p->shown[0],
                p->shown[1], p->shown[2], key == 1 ? "as it is: their outfit not heard yet" : "in their outfit");
        }
    } else if (!owner && !own && borrow && p->armed_hash != key && !g_dress_want.peer && (!p->armed_at || frame - p->armed_at > 600) &&
               g_dressings < 400) {
        g_dress_want.peer = p->id, g_dress_want.n = p->nparts, g_dress_want.body = p->body_type, g_dress_want.hash = key;  // (the next
        g_dress_want.look = p->look, g_dress_want.look_ok = p->look_state == 1;                                         // made: theirs)
        p->armed_at = frame;
        memcpy(g_dress_want.parts, p->parts, sizeof p->parts);
        p->armed_hash = key, p->dressed_at = 0;  // (the wait for its body starts once it is made)
        LOG("[world] player %016llx: the next townsperson made wears their outfit\n", (unsigned long long)p->id);
    }
    pthread_mutex_unlock(&g_dress_lock);
    if (!owner) return;
    uint64_t b = dressed_body_of(owner);
    if (!b) {
        if (!p->dressed_at) p->dressed_at = frame;
        if (frame - p->dressed_at > (own ? 600u : 1200u))  // (never placed)
            dressed_drop(owner), gone_add(owner, p->id, 0), p->armed_hash = 0, p->dressed_at = 0, p->own_k = 0, p->own_at = 0;
        return;
    }
    // (a station NPC or a walker of the crowd - in a busy square nearly every one made is a walker: either stands in. Its
    // node is its own, from its object - a walker's body and node part between its animation's step and its body's, and
    // the first node at its body's spot, node_hook's way, was some other one of its: that walker never moved, 2026-11-16)
    uint64_t node = node_of_owner(owner), old = p->dressed_owner;
    if (p->body != b) standin_let_go(p, old);  // (the plain stand-in goes back; one dressed in their old outfit is put away)
    if (old && old != owner) gone_add(old, p->id, 0);
    if (p->own_k) LOG("[world] player %016llx: newnpc_%03d placed %.1f s after it was asked for\n", (unsigned long long)p->id, p->own_k,
                      (double)(frame - p->own_at) / 60.0);
    p->dressed_hash = key, p->dressed_owner = owner, p->dressed_body = b, p->dressed_at = 0, p->own_k = 0, p->own_none_at = 0;
    p->body = b, p->node = node;  // (0: found by node_hook at the body's spot)
    uint64_t others[8];
    int no = 0;
    pthread_mutex_lock(&g_dress_lock);
    for (int i = 0; i < g_ndressed; i++)
        if (g_dressed[i].peer == p->id && g_dressed[i].owner != owner) {
            if (no < 8) others[no++] = g_dressed[i].owner;
            g_dressed[i] = g_dressed[--g_ndressed], i--;
        }
    pthread_mutex_unlock(&g_dress_lock);
    for (int i = 0; i < no; i++) gone_add(others[i], p->id, 0);  // (made for them earlier, never placed: put away too)
    uint64_t now = world_now_ns();
    LOG("[world] player %016llx is shown by a townsperson in their outfit (body %llx, node %llx, %s) - %.1f s after they were first heard, "
        "%.1f s after their outfit and look were\n", (unsigned long long)p->id, (unsigned long long)b, (unsigned long long)node,
        body_stepped(b, frame) ? "a walker of the crowd" : "standing", (double)(now - p->here_ns) / 1e9, (double)(now - p->key_ns) / 1e9);
}

// (the player a packet is from - known by their id and, through the server, by the sender the server names; NULL else)
static Peer *peer_from(const WorldPkt *k, uint64_t via) {
    for (int i = 0; i < MAXPEER; i++)
        if (g_peers[i].id && g_peers[i].id == k->from) return !via || g_peers[i].via == via ? &g_peers[i] : NULL;
    return NULL;
}
static bool g_node_hooked;
void warp_node_hook_on(void) {  // (the mod API's characters: hle/newnpc.c)
    if (!g_node_hooked) g_node_hooked = true, rt_register_fnhook_at(NODE_UPDATE, node_hook);
}
static void hooks_on(void) {
    if (g_hooked) return;
    g_hooked = true;
    warp_node_hook_on();
    rt_register_fnhook_at(BB_SET_FLOAT, bb_set_hook);
    rt_register_fnhook_at(INTERACT_NPC_UPDATE, interact_hook);
    rt_register_fnhook_at(BB_SET_TRIGGER, ai_trigger_hook);
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
    g_off_ahead = getenv("LARECOMP_GHOST_AHEAD") ? (float)atof(getenv("LARECOMP_GHOST_AHEAD")) : 0;
    g_off_turn = getenv("LARECOMP_GHOST_TURN") ? (float)atof(getenv("LARECOMP_GHOST_TURN")) * 0.0174533f : 0;
    g_self_test = getenv("LARECOMP_GHOST_SELF") != NULL;
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
        g_nshow = 0;
        for (int i = 0; i < MAXPEER; i++) if (g_peers[i].id) standin_hide(&g_peers[i]);
        memset(g_peers, 0, sizeof g_peers);
    }
}

// ---- Is the online server there? (the relay plugin's larecomp_server_state: every hello it sends is answered - lzaserver
// 2026-11-17 - so it knows within ~8 s). The player is told when it can't be reached, when it drops and when it's back
// (again every 2 minutes while it stays away) - before, online play just silently did nothing (za-mp12 review).
void menu_notice_for(const char *text, unsigned ms);
static void server_notice_frame(uint64_t frame) {
    static int (*state)(char *, size_t);
    static uint64_t at, told_at;
    static int was = -1, tries;
    if (frame - at < 60) return;
    at = frame;
    if (!state) {
        if (tries > 30) return;  // (no relay plugin with it - an older one, or none: looked for 30 s, then not again)
        tries++;
        void *plugins_symbol(const char *name);
        if (!(state = (int (*)(char *, size_t))plugins_symbol("larecomp_server_state"))) return;
    }
    char name[256] = "";
    int st = state(name, sizeof name);
    bool away = st == 3 || st == 4 || st == 5;
    if (st == was && !(away && frame - told_at >= 7200)) return;
    char t[400];
    const char *lt = "";  // (the log's words: never the server's address)
    t[0] = 0;
    if (st == 3) snprintf(t, sizeof t, "Can't reach the online server (%s) - online play waits for it. Is the address right, and the server running?", name), lt = "can't reach the server";
    else if (st == 4) snprintf(t, sizeof t, "Lost the connection to the online server (%s) - trying again...", name), lt = "lost the server";
    else if (st == 5) snprintf(t, sizeof t, "The online server's name (%s) can't be found - check the address (launcher > Options > Online) and your connection.", name), lt = "the server's name doesn't resolve";
    else if (st == 6) snprintf(t, sizeof t, "Sign in to %s to play online there: launcher > Options > Online. (Playing offline needs no account.)", name), lt = "sign in to play on that server";
    else if (st == 2 && was == 6) snprintf(t, sizeof t, "Signed in on %s: online play is on.", name), lt = "signed in: online play on";
    // (back after it couldn't be reached: online play just carries on - nothing to tell, the notice said it waits for it)
    if (t[0]) menu_notice_for(t, st == 2 ? 5000 : st == 6 ? 12000 : 10000), told_at = frame, LOG("[online] told: %s\n", lt);
    else if (st != was) LOG("[online] the server: %s\n", st == 1 ? "asked, no answer yet" : st != 2 ? "not in use" : was == 4 ? "answering again (not told)" : was == 3 || was == 5 ? "reached (not told)" : "answering");
    was = st;
}

static void ghost_frame(uint64_t frame) {
    server_notice_frame(frame);
    world_transport(frame);
    gone_frame(frame);
    if (!g_world) {
        return;
    }
    hooks_on();
    uint64_t now = world_now_ns();
    uint64_t me = player_body(frame);
    if (me != g_me_body) g_me_body = me, g_me_node = 0, g_me_since = frame;
    uint64_t mn = g_me_node;
    if (mn && *(const uint64_t *)(uintptr_t)mn != NODE_VTABLE) g_me_node = mn = 0;
    if (mn != g_me_bb_node) {  // (our blackboard: our "move" as our controller sets it - bb_set_hook)
        uint64_t bb = mn ? anim_bb_of_node(mn) : 0;
        uint32_t idx = ~0u;
        if (bb && !anim_float_index(bb, HASH_MOVE, &idx)) bb = 0;
        if (bb || !mn) {
            g_me_bb = 0, g_me_move_idx = idx, g_me_move = 0, g_me_bb = bb, g_me_bb_node = mn;
            if (bb) LOG("[world] our player's animation blackboard %llx (\"move\" #%u)\n", (unsigned long long)bb, idx);
        }
    }
    move_reset_expire(frame);
    vset_frame(frame, me && mn);  // (the names of everything the game has to wear: once in the world)
    static uint64_t sent_ns, alive_ns, due_ns;
    static float last[3];
    static uint64_t beat_ns;  // (ours 20 a second - the setting world_rate: 30; research LARECOMP_WORLD_RATE=<n>, 5..60)
    if (!beat_ns) {
        int r = getenv("LARECOMP_WORLD_RATE") ? atoi(getenv("LARECOMP_WORLD_RATE")) : g_settings.world_rate == 30 ? 30 : 20;
        if (r < 5 || r > 60) r = 20;
        beat_ns = 1000000000ull / (uint64_t)r;
        LOG("[world] our position sent %d times a second\n", r);
    }
    if (me && mn && now >= due_ns) {  // (ours: the body's position, the node's facing - on a beat (50 ms at 20 a second),
        due_ns = now - due_ns < 2 * beat_ns ? due_ns + beat_ns : now + beat_ns;  //  not 50 ms after the last frame sent: at
                                                       //  30 fps that was every 67 ms, and the others wait for each one)
        const float *b = (const float *)(uintptr_t)(me + 0x90), *q = (const float *)(uintptr_t)(mn + 0xd0);
        float dt = sent_ns ? (float)(now - sent_ns) / 1e9f : 1;
        WorldPkt k = {.type = 2, .ver = 1, .from = g_self};
        static uint16_t seq;
        uint32_t ms = (uint32_t)(now / 1000000ull) | 1;  // (never 0: "none")
        seq++, memcpy(k.pad, &seq, 2), memcpy(k.pad + 2, &ms, 4);
        for (int i = 0; i < 3; i++) k.w.p[i] = b[i], k.w.v[i] = dt < 0.5f ? (b[i] - last[i]) / dt : 0, last[i] = b[i];
        k.w.yaw = 2.0f * atan2f(q[1], q[3]);
        k.w.move = g_me_move;
        world_send(&k, 16 + 32);
        sent_ns = now;
    } else if ((!me || !mn) && now - sent_ns >= 1000000000ull && now - alive_ns >= 1000000000ull) {  // (no player of ours in the
        WorldPkt k = {.type = 7, .ver = 1, .from = g_self};                                            //  world: still here, once
        world_send(&k, 16);                                                                            //  a second)
        alive_ns = now;
    }
    // (our outfit and our look: sent while someone we hear hasn't acknowledged the ones we have now - they change, a
    // player comes, an older copy never acknowledges -, else once every 15 s for safety: a whole round of the parts and
    // the look once. LARECOMP_WORLD_RESEND=1: always, the old way - an A/B)
    static int resend_always = -1;
    if (resend_always < 0) resend_always = getenv("LARECOMP_WORLD_RESEND") && atoi(getenv("LARECOMP_WORLD_RESEND")) == 1;
    static uint32_t our_o, our_l;
    static uint64_t safety_ns;
    static int round_o, round_l, need_was = -1;
    int need_o = resend_always, need_l = resend_always;
    for (int i = 0; i < MAXPEER; i++)
        if (g_peers[i].id) need_o |= g_peers[i].ack_o != our_o || !our_o, need_l |= g_peers[i].ack_l != our_l || !our_l;
    if (me && now - safety_ns >= 15000000000ull) safety_ns = now, round_o = 13, round_l = 1;
    if ((need_o || need_l) != need_was) {
        if (need_was >= 0)
            LOG("[world] our outfit and look %s\n", need_o || need_l ? "sent: someone hasn't got them" : "acknowledged by everyone: sent again on a change, "
                "a newcomer or every 15 s");
        need_was = need_o || need_l;
    }
    Look ours;
    if (me && frame % 60 == 30 && (need_l || round_l) && look_ours(&ours)) {  // (our real look, every second)
        uint32_t lh = 2166136261u;
        for (size_t b = 0; b < sizeof ours; b++) lh = (lh ^ ((const uint8_t *)&ours)[b]) * 16777619u;
        our_l = lh;
        if (round_l) round_l--;
        for (int k = 0; k < 2; k++) {
            WorldPkt w = {.type = 6, .ver = 1, .from = g_self};
            w.l.k = (uint8_t)k, w.l.n = 2;
            memcpy(w.l.pad, &lh, 4);
            size_t off = (size_t)k * 64, len = sizeof ours - off < 64 ? sizeof ours - off : 64;
            memcpy(w.l.d, (const uint8_t *)&ours + off, len);
            world_send(&w, 16 + 8 + 64);
        }
    } else if (me && frame % 60 == 30 && look_ours(&ours)) {  // (not sent: still looked at - a change is sent at once)
        uint32_t lh = 2166136261u;
        for (size_t b = 0; b < sizeof ours; b++) lh = (lh ^ ((const uint8_t *)&ours)[b]) * 16777619u;
        our_l = lh;
    }
    if (me && frame % 8 == 0) {  // (our outfit, a part every 8 frames: the whole of it every ~2 s while it's sent)
        static int part;
        DressPart o[MAXPARTS];
        int body, n = outfit_ours(o, &body);
        if (n) {
            our_o = outfit_hash(o, n);
            if (need_o || round_o > 0) {
                if (round_o > 0) round_o--;
                part = (part + 1) % n;
                WorldPkt k = {.type = 5, .ver = 1, .from = g_self};
                memcpy(k.pad, &our_o, 4);
                k.o.k = (uint8_t)part, k.o.n = (uint8_t)n, k.o.body = (uint8_t)(body < 0 ? 0xff : body);
                size_t len = outfit_put(&k, &o[part]);
                if (len) world_send(&k, len);
            }
        }
    }
    WorldPkt in;
    uint64_t via;
    int n;
    // (everything in a packet is checked before use: its size per type, numbers finite and in range, strings bounded and
    // known to the game - outfit_part_check / look_check; a player is known by their id AND, through the server, by who
    // the server says sent it - peer_from: an id can't be borrowed)
    while (w_recv && (via = 0, n = w_recv(&via, &in, sizeof in)) >= 16) {
        if (g_self_test && in.from == g_self) in.from = g_self ^ 2;  // (ourselves, as someone else)
        if (in.ver != 1 || in.from == g_self || !in.from) continue;
        Peer *p = peer_from(&in, via);
        if (in.type == 10) {  // (a script message's piece / a chat line's: hle/luanet.c)
            void luanet_heard(uint64_t from, const uint8_t *b, int n);
            if (n > 16) luanet_heard(in.from, (const uint8_t *)&in + 16, n - 16);
            continue;
        }
        // (a walk-up trade's messages: from a player we hear - or from the one we trade with, known by the server's word,
        // while we have no stand-in for them: back from a menu or a lag stall they are heard again only with their next
        // position - the answer to our ask came first, was dropped, and the trade never started, 2026-11-17)
        bool partner = p || (via && in.from == g_trade_with && via == g_trade_via);
        if (in.type == 3 || in.type == 4) { if (n >= 28 && partner) trade_message(&in, frame, via); continue; }
        if (in.type == 9) { if (n >= 29 && partner) trade_link_message(&in); continue; }
        if (in.type == 5) { outfit_message(p, &in, (size_t)n); continue; }
        if (in.type == 8) {  // (what they have of ours: ours stop being resent once everyone has them)
            if (p && n >= 32 && (in.a.to == g_self || (g_self_test && in.a.to == (g_self ^ 2))))
                p->ack_o = in.a.o, p->ack_l = in.a.l, p->acked = 1;
            continue;
        }
        if (in.type == 7) {  // (in a menu or loading: still here - the wait since their last position was that, not lateness)
            if (p) p->heard_ns = now, p->away = 1, p->stretched -= p->waiting < p->stretched ? p->waiting : p->stretched, p->waiting = 0;
            continue;
        }
        if (in.type == 6) {
            if (!p || n < 16 + 8 + 64 || in.l.n != 2 || in.l.k >= 2) continue;
            size_t off = (size_t)in.l.k * 64, len = sizeof p->look - off < 64 ? sizeof p->look - off : 64;
            uint32_t tag;
            memcpy(&tag, in.l.pad, 4);
            memcpy((uint8_t *)&p->look_in + off, in.l.d, len), p->look_tag[in.l.k] = tag;
            if (tag) {  // (both pieces of the same look - the hash of what they make: theirs)
                uint32_t h = 2166136261u;
                for (size_t b = 0; b < sizeof p->look_in; b++) h = (h ^ ((const uint8_t *)&p->look_in)[b]) * 16777619u;
                p->poked = 1;
                if (p->look_tag[0] != tag || p->look_tag[1] != tag || h != tag) continue;
                p->look_took = tag;  // (what we have of it: acknowledged, type 8)
                if (p->look_have == 3 && !memcmp(&p->look, &p->look_in, offsetof(Look, f1))) continue;  // (the same again - the
                // flags aside: look_check sets them, the body, from the parts; compared whole, every resend was a new look)
                p->look = p->look_in, p->look_have = 3;
            } else {  // (an older copy: piece by piece)
                size_t same = off + len > offsetof(Look, f1) ? offsetof(Look, f1) - off : len;  // (the flags aside, as above)
                if (!memcmp((uint8_t *)&p->look + off, in.l.d, same) && (p->look_have & 1u << in.l.k)) continue;  // (the same again)
                memcpy((uint8_t *)&p->look + off, in.l.d, len);
                p->look_have |= 1u << in.l.k;
            }
            p->look_state = 0;  // (changed: checked again - outfit_frame)
            continue;
        }
        if (in.type != 2 || n < 16 + 28) continue;
        bool sane = isfinite(in.w.yaw);
        for (int k = 0; k < 3; k++) sane = sane && isfinite(in.w.p[k]) && fabsf(in.w.p[k]) < 100000.0f && isfinite(in.w.v[k]);
        if (!sane) continue;
        if (!p) {
            bool taken = false;  // (a free slot; someone the server already knows under another id: not a second player)
            Peer *free_p = NULL;
            for (int i = 0; i < MAXPEER; i++) {
                if (g_peers[i].id == in.from || (via && g_peers[i].id && g_peers[i].via == via)) taken = true;
                else if (!g_peers[i].id && !free_p) free_p = &g_peers[i];
            }
            if (!taken && !free_p && me) {  // (more players than we keep: one clearly nearer than the farthest of those not shown
                const float *m = (const float *)(uintptr_t)(me + 0x90);  //  takes their place - far ones mustn't keep
                float dx = in.w.p[0] - m[0], dy = in.w.p[1] - m[1], dz = in.w.p[2] - m[2];  //  a near one out)
                float d = sqrtf(dx * dx + dy * dy + dz * dz), farthest = 0;
                for (int i = 0; i < MAXPEER; i++)
                    if (!g_peers[i].show && g_peers[i].dist > farthest) farthest = g_peers[i].dist, free_p = &g_peers[i];
                if (free_p && d < farthest - 10) {
                    static uint64_t logged_at;
                    if (!logged_at || frame - logged_at > 600)
                        logged_at = frame, LOG("[world] player %016llx (%.0f m away) heard in place of %016llx (%.0f m): %d kept at most\n",
                                               (unsigned long long)in.from, d, (unsigned long long)free_p->id, farthest, MAXPEER);
                    standin_hide(free_p);
                } else free_p = NULL;
            }
            if (taken || !free_p) continue;  // (more players than we keep, none nearer: they wait)
            p = free_p;
            memset(p, 0, sizeof *p);
            p->id = in.from, p->via = via, p->here_ns = now;
            memcpy(p->shown, in.w.p, 12), memcpy(p->target_was, in.w.p, 12), p->shown_yaw = remainderf(in.w.yaw, 6.2831853f);
            LOG("[world] player %016llx is here\n", (unsigned long long)in.from);
            g_choose_soon = 1;
        }
        uint16_t sq;
        uint32_t sent_ms;
        memcpy(&sq, in.pad, 2), memcpy(&sent_ms, in.pad + 2, 4);
        p->late = 0;
        if (sent_ms) {  // (their sequence and send time: an older copy sends neither)
            if (p->seq_ok && (int16_t)(sq - p->seq) <= 0) { p->stale++; continue; }  // (overtaken by a newer one: out of date)
            p->seq = sq, p->seq_ok = 1, p->nseq++;
            // (arrival minus sending, in our ms - the two clocks' difference plus the time on the way: only differences of it
            // count. The quickest of the last 2-4 s is the way's own time; anything more is this packet's lateness)
            uint32_t off = (uint32_t)(now / 1000000ull) - sent_ms;
            if (!p->base_win_at || now - p->base_win_at > 2000000000ull)
                p->base_win[1] = p->base_win_at ? p->base_win[0] : off, p->base_win[0] = off, p->base_win_at = now;
            else if ((int32_t)(off - p->base_win[0]) < 0) p->base_win[0] = off;
            p->base = (int32_t)(p->base_win[0] - p->base_win[1]) < 0 ? p->base_win[0] : p->base_win[1];
            int32_t late = (int32_t)(off - p->base);
            p->late = late <= 0 ? 0 : late > 400 ? 0.4f : (float)late / 1000.0f;
            if (p->nsnap && (int32_t)(sent_ms - p->snap[p->nsnap - 1].t) > 1000) p->nsnap = 0;  // (back from a menu or a load:
            if (p->nsnap) {  // (the send gap, for the log; how long we'd have waited for this one)   //  drawn afresh)
                float g = (float)(int32_t)(sent_ms - p->snap[p->nsnap - 1].t);
                if (g > 0 && g < 500) p->gap = p->gap ? p->gap + (g - p->gap) * 0.1f : g;
                int32_t nd = (int32_t)((uint32_t)(now / 1000000ull) - p->base - p->snap[p->nsnap - 1].t);
                p->need[p->need_at] = (uint16_t)(nd < 0 ? 0 : nd > 1000 ? 1000 : nd), p->need_at = (p->need_at + 1) % NNEED;
                if (p->nneed < NNEED) p->nneed++;
            } else p->base0 = p->base;
            if (p->nneed) {  // (the 90th percentile of the waits: counted, 10 ms buckets)
                uint16_t hist[101] = {0};
                for (int k = 0; k < p->nneed; k++) hist[p->need[k] / 10]++;
                int want = p->nneed - p->nneed / 10, c = 0, b = 0;
                while (b < 100 && (c += hist[b]) < want) b++;
                p->behind_want = fminf(fmaxf((float)(b * 10 + 10) + 10, 30), 400);
            } else p->behind_want = 100;
            if (!p->nsnap) p->behind = p->behind_want;
            if (p->nsnap == NSNAP) memmove(&p->snap[0], &p->snap[1], sizeof p->snap[0] * (NSNAP - 1)), p->nsnap = NSNAP - 1;
            int s = p->nsnap++;
            p->snap[s].t = sent_ms, memcpy(p->snap[s].p, in.w.p, 12), p->snap[s].yaw = remainderf(in.w.yaw, 6.2831853f);
            for (int k = 0; k < 3; k++) p->snap[s].v[k] = fminf(fmaxf(in.w.v[k], -60.0f), 60.0f);
            p->snap[s].move = n >= 16 + 32 && isfinite(in.w.move) ? fminf(fmaxf(in.w.move, 0.0f), 2.0f) : -1;
        }
        memcpy(p->pos, in.w.p, 12), p->heard_ns = now, p->away = 0, p->waiting = 0;
        for (int k = 0; k < 3; k++) p->vel[k] = fminf(fmaxf(in.w.v[k], -60.0f), 60.0f);
        p->yaw = remainderf(in.w.yaw, 6.2831853f);  // (with send times: the moment drawn's, below)
        if (!p->nsnap) {  // (an older copy: no send times - drawn from the newest packet, below)
            p->move = n >= 16 + 32 && isfinite(in.w.move) ? fminf(fmaxf(in.w.move, 0.0f), 2.0f) : -1;
        }
    }
    // (what we have of each player's outfit and look, told them - type 8: when it changes (at most 4 times a second),
    // every 2 s while we lack some of it, and - at most every 2 s - when they sent some again: they may not have heard us)
    for (int i = 0; i < MAXPEER; i++) {
        Peer *p = &g_peers[i];
        if (!p->id) continue;
        uint32_t ho = p->nparts && p->have == (1u << p->nparts) - 1 ? p->outfit_tag : 0, hl = p->look_have == 3 ? p->look_took : 0;
        uint64_t since = now - p->told_ns;
        if (((ho != p->told_o || hl != p->told_l) && since >= 250000000ull) || (since >= 2000000000ull && (!ho || !hl || p->poked))) {
            WorldPkt k = {.type = 8, .ver = 1, .from = g_self};
            k.a.to = p->id, k.a.o = ho, k.a.l = hl;
            world_send(&k, 32);
            p->told_o = ho, p->told_l = hl, p->told_ns = now, p->poked = 0;
        }
    }
    static uint64_t chosen_at;
    if (g_choose_soon || frame - chosen_at >= 60) g_choose_soon = 0, chosen_at = frame, world_choose(me, frame), dressed_sweep();
    bool want_scan = false;
    static uint64_t prev_ns;  // (the time since the last frame: the turn below is the same at any frame rate)
    float dt = prev_ns && now > prev_ns ? (float)(now - prev_ns) / 1e9f : 1.0f / 60;
    prev_ns = now;
    if (dt > 0.1f) dt = 0.1f;
    for (int i = 0; i < MAXPEER; i++) {
        Peer *p = &g_peers[i];
        if (!p->id) continue;
        if (now - p->heard_ns > PEER_TIMEOUT_NS) {  // (gone quiet: their stand-in goes back)
            LOG("[world] player %016llx left\n", (unsigned long long)p->id);
            bool was_shown = p->show;
            standin_hide(p);
            memset(p, 0, sizeof *p);
            if (was_shown) world_choose(me, frame);  // (out of the shown list now - and someone else may take the place)
            continue;
        }
        if (!p->show) {  // (heard, not shown: kept up to date, nothing more)
            memcpy(p->shown, p->pos, 12), memcpy(p->target_was, p->pos, 12), p->shown_yaw = p->yaw;
            continue;
        }
        outfit_frame(p, frame);
        if (p->body && !is_body(p->body)) {  // (the stand-in went with its level)
            LOG("[world] player %016llx's stand-in went with its level: another one at once\n", (unsigned long long)p->id);
            stand_in_gone(p);
        }
        // (none: one borrowed from the street - with world_own_npc only where their own can't be made, 5 s after they were
        // shown: a level loads its stations a moment after our player is in it)
        if (!p->body && (!own_npc() || (p->own_none_at && frame - p->own_none_at > 300))) want_scan = true;
        pres_frame(p, frame);
        float target[3], pull = 1.0f - expf(-dt * 25.8f);  // (pull: 0.35 of the way a frame at 60 fps - the same at any rate)
        if (p->nsnap) {
            // (drawn `behind` ms behind their clock: between the two packets around that moment. Past the newest - lost or
            // very late packets - carried on by its speed, up to 0.25 s. behind eases toward what's wanted at a tenth of
            // the time passing: the stand-in moves at 0.9-1.1x their speed meanwhile, never back)
            float step = dt * 100.0f, w = p->behind_want - (float)(int32_t)(p->base0 - p->base);  // (base0..base: the way's own
            p->behind = p->behind < w ? fminf(p->behind + step, w) : fmaxf(p->behind - step, w);  //  time changing - eased too)
            int nw = p->nsnap - 1;
            float age = (float)(int32_t)((uint32_t)(now / 1000000ull) - p->base0 - p->snap[nw].t);
            float r = age - p->behind;  // (ms from the newest)
            if (r > 0) {  // (run out: it waits at the newest - its clock held, up to 600 ms more than wanted; then held anyway)
                if (p->behind < w + 600) {
                    p->behind = fminf(p->behind + r, w + 600);
                    if (!p->away) p->stretched++, p->waiting++;  // (a late packet waited for - not their menu time)
                }
                r = 0;
            }
            int a = -1;
            for (int k = nw; k >= 0; k--) if ((float)(int32_t)(p->snap[k].t - p->snap[nw].t) <= r) { a = k; break; }
            if (a == nw || a < 0) {
                int k = a < 0 ? 0 : nw;
                for (int i = 0; i < 3; i++) target[i] = p->snap[k].p[i];
                p->yaw = p->snap[k].yaw, p->move = p->snap[k].move;
                if (age - w > 250.0f && p->move > 0) p->move = 0;  // (nothing new for a while - a menu, a loading screen: standing)
            } else {
                float ra = (float)(int32_t)(p->snap[a].t - p->snap[nw].t), rb = (float)(int32_t)(p->snap[a + 1].t - p->snap[nw].t);
                float f = rb > ra ? (r - ra) / (rb - ra) : 1;
                for (int i = 0; i < 3; i++) target[i] = p->snap[a].p[i] + (p->snap[a + 1].p[i] - p->snap[a].p[i]) * f;
                p->yaw = remainderf(p->snap[a].yaw + remainderf(p->snap[a + 1].yaw - p->snap[a].yaw, 6.2831853f) * f, 6.2831853f);
                p->move = p->snap[f < 0.5f ? a : a + 1].move;
            }
        } else {  // (an older copy: the newest packet carried on by their speed for the time since it came, up to 0.25 s)
            float age = (float)(now - p->heard_ns) / 1e9f;
            if (age > 0.25f) age = 0.25f;
            for (int k = 0; k < 3; k++) target[k] = p->pos[k] + p->vel[k] * age;
        }
        if (p->nsnap) {  // (the target moves smoothly by itself: shown is it exactly - not a step behind, as following it was;
            float j = 0, jw = 0;  //  a jump in it - faster than anyone runs: drawn afresh after a menu - is eased out; a jump
            for (int k = 0; k < 3; k++) j += (target[k] - p->target_was[k]) * (target[k] - p->target_was[k]),  //  of 8 m - a warp:
                                        jw += (target[k] - p->shown[k]) * (target[k] - p->shown[k]);           //  at once)
            bool jump = sqrtf(j) > 20.0f * dt + 0.05f;
            for (int k = 0; k < 3; k++) {
                float e = p->shown[k] - (jump ? target[k] : p->target_was[k]);
                p->shown[k] = sqrtf(jw) > 8 ? target[k] : target[k] + e * (1.0f - pull);
                p->target_was[k] = target[k];
            }
        } else
            for (int k = 0; k < 3; k++) {
                float d = target[k] - p->shown[k];
                p->shown[k] = fabsf(d) > 8 ? target[k] : p->shown[k] + d * pull;  // (a jump of 8 m - a warp: at once)
                p->target_was[k] = p->shown[k];
            }
        static int net_log = -1;  // (research LARECOMP_WORLD_LOG_NET=1: every 10 s, their packets taken / dropped as out of date)
        if (net_log < 0) net_log = getenv("LARECOMP_WORLD_LOG_NET") != NULL;
        if (net_log && frame % 600 == 0)
            LOG("[world] player %016llx: %u packets in order, %u out of date dropped, this one %.0f ms late; drawn %.0f ms behind (wanted %.0f, gap %.0f; %u frames waited)\n",
                (unsigned long long)p->id, p->nseq, p->stale, p->late * 1000, p->behind, p->behind_want, p->gap, p->stretched);
        // (their facing comes 20 times a second, and their own turn is quick: their stand-in turns toward it - most of the
        // way in a tenth of a second, the short way round - rather than snapping to each heading)
        float dy = remainderf(p->yaw - p->shown_yaw, 6.2831853f);
        p->shown_yaw = remainderf(p->shown_yaw + dy * (1.0f - expf(-dt * 14.0f)), 6.2831853f);
        static int turn_log = -1;  // (research LARECOMP_WORLD_LOG_TURN=1: each frame of a turn, heard vs shown)
        if (turn_log < 0) turn_log = getenv("LARECOMP_WORLD_LOG_TURN") != NULL;
        if (turn_log && fabsf(dy) > 0.02f) LOG("[world] turn: frame %llu heard %.3f shown %.3f\n", (unsigned long long)frame, p->yaw, p->shown_yaw);
        static int where_log = -1;  // (research LARECOMP_WORLD_LOG_STANDIN=<n>: every n frames where the stand-in is drawn, its
        if (where_log < 0) where_log = getenv("LARECOMP_WORLD_LOG_STANDIN") ? atoi(getenv("LARECOMP_WORLD_LOG_STANDIN")) : 0;  // body, us)
        if (where_log > 0 && frame % (uint64_t)where_log == 0 && p->body && is_body(p->body) && me) {
            const float *b = (const float *)(uintptr_t)(p->body + 0x90), *m = (const float *)(uintptr_t)(me + 0x90);
            LOG("[world] standin frame %llu shown %.2f %.2f %.2f body %.2f %.2f %.2f us %.2f %.2f %.2f%s t %.1f\n", (unsigned long long)frame,
                p->shown[0], p->shown[1], p->shown[2], b[0], b[1], b[2], m[0], m[1], m[2], body_stepped(p->body, frame) ? " (stepped)" : "",
                (double)now / 1e6);
        }
    }
    trade_frame(me, frame);
    trade_link_frame(frame);
    static uint64_t scanned_at;
    if (want_scan && !g_scanning && me && mn && (!scanned_at || frame - scanned_at > 600 || (g_scan_soon && frame - scanned_at > 60))) {
        pthread_t t;
        g_scanning = 1, g_scan_me = me, scanned_at = frame, g_scan_soon = 0;
        if (pthread_create(&t, NULL, scan_bodies, NULL)) g_scanning = 0; else pthread_detach(t);
    }
}

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
    g_made[__atomic_fetch_add(&g_nmade, 1, __ATOMIC_RELAXED) % NMADE] = ((const uint64_t *)(uintptr_t)req)[6];  // (scan_bodies)
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
    DressPart own = {{0}};
    if (n == 1) part_of(b, vec + 4 + fb_u32(vec + 4, 0), &own);
    bool towns = n == 1 && strstr(own.path, "savm_cc_vr");  // (townspeople only)
    uint64_t owner = ((const uint64_t *)(uintptr_t)req)[6];
    int ownk = g_own_used ? newnpc_number(node_of_owner(owner)) : 0;  // (one made for a player: world_own_npc)
    if (!towns && !ownk) return 0;
    {   // (a script's character the script dressed - the mod API's npc_outfit "player": our player character's parts, as its
        // make had them. Not its look on top (look_wear_at_make, as the shared world's stand-ins get theirs): one made so
        // wasn't drawn at all - 2026-11-19; the save's own outfit is in that look, so some pieces differ)
        extern int newnpc_mod_outfit_of_node(uint64_t node);
        int lua = towns && !ownk ? newnpc_mod_outfit_of_node(node_of_owner(owner)) : 0;
        if (lua == 1 && g_npparts) {
            DressPart lp[25];
            int ln = 0;
            pthread_mutex_lock(&g_dress_lock);
            for (int k = 0; k < g_npparts && ln < 25; k++) lp[ln++] = g_pparts[k];
            int lbody = g_pbody;
            pthread_mutex_unlock(&g_dress_lock);
            for (int k = 0; k < ln; k++) if (!strcmp(lp[k].name, "base")) snprintf(lp[k].name, sizeof lp[k].name, "default");
            const uint32_t lbase = 8192;
            uint8_t *d = hle_alloc(16384);
            memcpy(d, b, lbase);
            uint32_t vpos = fb_put_parts(d, lbase, lp, ln);
            uint64_t f15o = (uint64_t)(f15 - b);
            memcpy(d + f15o, &(uint32_t){vpos - (uint32_t)f15o}, 4);
            if (f14 && lbody >= 0) d[f14 - b] = (uint8_t)lbody;
            *(uint64_t *)(uintptr_t)req = (uint64_t)(uintptr_t)d;
            LOG("[dress] a script's character (%s) dressed in our player's %d parts\n", own.path, ln);
            rt_call_original(c, addr);
            return 1;
        }
    }
    int body = f14 ? f14[0] : -1;
    DressPart parts[25];
    int np = 0, to_body = -1;
    uint64_t for_peer = 0;
    uint32_t hash = 0;
    // (any townsperson: station NPCs - made with their level or streamed in as the player walks - and walkers of the
    // crowd - made alone, now and then, while the player stands: in a busy square nearly all of them - both stand in.
    // A walker's model follows its node too once that is its own node, from its object, 2026-11-16 - the node at its
    // body's spot was another one, so a walker had seemed to follow the crowd only)
    pthread_mutex_lock(&g_dress_lock);
    Look want_look;
    bool want_look_ok = false;
    int own_state = 0;  // (one made for a player: 1 theirs, 2 no townsperson, 3 nobody's any more)
    if (ownk) {
        int i = ownk % NOWN;
        if (g_own[i].k != ownk) own_state = 3;
        else if (!towns) own_state = 2, g_own[i].failed = 1, for_peer = g_own[i].peer;
        else {
            own_state = 1, g_own[i].made = 1;
            for_peer = g_own[i].peer, hash = g_own[i].key, to_body = g_own[i].body, want_look = g_own[i].look, want_look_ok = g_own[i].look_ok;
            for (int k = 0; k < g_own[i].n; k++) parts[np++] = g_own[i].parts[k];
            if (np) g_dressings++;
        }
    } else if (g_dress_want.peer) {  // (a player of the shared world waiting for a look)
        for_peer = g_dress_want.peer, hash = g_dress_want.hash, to_body = g_dress_want.body, want_look = g_dress_want.look;
        want_look_ok = g_dress_want.look_ok, g_dressings++;
        for (int k = 0; k < g_dress_want.n; k++) parts[np++] = g_dress_want.parts[k];
        g_dress_want.peer = 0;
    } else if (g_dress_left > 0 && g_npparts && (g_dress_body < 0 || body == g_dress_body)) {  // (research)
        if (g_dress_mode == 1) parts[np++] = own;
        for (int k = 0; k < g_npparts && np < 25; k++) parts[np++] = g_pparts[k];
        g_dress_left--;
    }
    pthread_mutex_unlock(&g_dress_lock);
    if (own_state >= 2) {  // (not one to stand in: put away as soon as it is placed - the game then removes it)
        if (own_state == 2) newnpc_bad(ownk);
        LOG("[dress] newnpc_%03d (%s) %s: put away\n", ownk, n == 1 ? own.path : "a character of many parts",
            own_state == 2 ? "is no townsperson" : "was asked for by no one shown now");
        gone_add(owner, for_peer, 0);
        return 0;
    }
    if (!np) {
        if (own_state == 1) {  // (a plain one of their own - their outfit not heard in time: stands in as it is)
            dressed_add(for_peer, owner, hash);
            LOG("[dress] newnpc_%03d %s made for another player as it is (character %llx)\n", ownk, own.path, (unsigned long long)owner);
        }
        return 0;
    }
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
    if (for_peer) dressed_add(for_peer, owner, hash);
    LOG("[dress] %s%s (body type %d) dressed in %s's %d parts (character %llx, description %p)\n", ownk ? "their own townsperson, " : "townsperson ",
        own.path, body, for_peer ? "another player" : "our player", np, (unsigned long long)owner, (void *)d);
    if (for_peer && want_look_ok) { extern bool look_wear_at_make(Cpu *c, uint64_t owner, const Look *l); look_wear_at_make(c, owner, &want_look); }
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
// a character's own node (the transform its model is drawn at; 0: none): its object +0xc8 -> +0x38
static uint64_t node_of_owner(uint64_t owner) {
    uint64_t v = 0, comp = 0, node = 0;
    if (!heap_read(owner, &v, 8) || v != CHAR_OBJ_VTABLE || !heap_read(owner + 0xc8, &comp, 8) || !heap_read(comp + 0x38, &node, 8) ||
        !heap_read(node, &v, 8) || v != NODE_VTABLE) return 0;
    return node;
}
// A dressed character's animation asked for a part it hasn't (2026-11-16, a dressed walker of the crowd, a minute after it
// was made: abort, std::vector::at): 0x70003feb14 (x0 -> [+8] the animation component, w1 a part's index, x8 out: its
// handle) indexes the parts the character was made with (+0x2038 -> +0x150, 0x68-byte entries) - the index comes from the
// townsperson's own data, its parts are a player's. Out of range it now answers "none" (0), as for a part not loaded yet;
// its callers check for that.
#define PART_BY_INDEX 0x70003feb14ull
static int part_index_hook(Cpu *c, uint64_t addr) {
    (void)addr;
    uint64_t comp = 0, a = 0, b = 0, e = 0;
    uint32_t idx = (uint32_t)(c->x[1] & 0xff);
    if (!heap_read(c->x[0] + 8, &comp, 8) || !heap_read(comp + 0x2038, &a, 8) || !heap_read(a + 0x150, &b, 8) || !heap_read(a + 0x158, &e, 8) ||
        e < b || idx < (e - b) / 0x68) return 0;
    static int logged;
    if (logged++ < 8)
        LOG("[dress] a character's animation asked for its part #%u of %llu (from %llx): none - a dressed one has other parts\n", idx,
            (unsigned long long)((e - b) / 0x68), (unsigned long long)c->x[30]);
    uint64_t z = 0;
    memcpy((void *)(uintptr_t)c->x[8], &z, 8);
    return 1;
}
static bool g_dress_hooked;
// The CharacterCreationMaster make: the shared world's dressing (dress_hook) and the mod API's characters (hle/newnpc.c
// newnpc_made: which one was made) - one hook on it, either or both
void newnpc_made(Cpu *c);
static volatile int g_make_api;
static int cc_make_hook(Cpu *c, uint64_t addr) {
    if (g_make_api) newnpc_made(c);
    return g_dress_hooked ? dress_hook(c, addr) : 0;
}
static bool g_make_hooked;
void warp_make_hook_on(void) {
    g_make_api = 1;
    if (!g_make_hooked) g_make_hooked = true, rt_register_fnhook_at(CC_MAKE, cc_make_hook);
}
// the mod API (hle/newnpc.c): scripts' characters dressed as they are made - the make hook with the dressing, and the
// parts fix (no pose hooks: those are the shared world's stand-ins')
void warp_dress_scripts_on(void) {
    static bool on;
    if (on) return;
    on = true;
    g_dress_hooked = true;
    if (!g_make_hooked) g_make_hooked = true, rt_register_fnhook_at(CC_MAKE, cc_make_hook);
    rt_register_fnhook_at(PART_BY_INDEX, part_index_hook);
}
static void dress_hook_on(void) {
    static bool done;  // (g_dress_hooked may be on already: scripts' dressing, warp_dress_scripts_on)
    if (done) return;
    done = true;
    g_dress_hooked = true;
    if (!g_make_hooked) g_make_hooked = true, rt_register_fnhook_at(CC_MAKE, cc_make_hook);
    rt_register_fnhook_at(PART_BY_INDEX, part_index_hook);
    pose_hooks_on();  // (every character's pose, as it is made - the level's people at its load)
    if (own_npc()) newnpc_on();  // (the station spawns kept as the levels load: their own townspeople made from them)
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
    l->f1 = l->f2 = 0;  // (the blob's flags - the body: not sent, each copy works them out from the parts, look_check -
                        //  so our look's bytes never change when our names scan finishes)
    static uint64_t logged;
    if (logged != o && (logged = o)) {
        LOG("[world] our look: object %llx, hair %016llx, skin %u eyes %u lashes %u hair colour %u\n", (unsigned long long)o,
            (unsigned long long)l->part[7], l->opt[0], l->opt[3], l->opt[6], l->opt[11]);
        LOG("[world]   its parts %016llx %016llx %016llx %016llx %016llx %016llx %016llx %016llx %016llx %016llx %016llx %016llx %016llx\n",
            (unsigned long long)l->part[0], (unsigned long long)l->part[1], (unsigned long long)l->part[2], (unsigned long long)l->part[3],
            (unsigned long long)l->part[4], (unsigned long long)l->part[5], (unsigned long long)l->part[6], (unsigned long long)l->part[7],
            (unsigned long long)l->part[8], (unsigned long long)l->part[9], (unsigned long long)l->part[10], (unsigned long long)l->part[11],
            (unsigned long long)l->part[12]);
        char t[18 * 4 + 1];
        size_t w = 0;
        for (int k = 0; k < 18; k++) w += (size_t)snprintf(t + w, sizeof t - w, " %u", l->opt[k]);
        LOG("[world]   its options%s\n", t);
    }
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

// ---- For scripts (hle/luanet.c: the world API's net / players / chat): a script message's piece travels as type 10,
// {type 10, ver 1, u8[6], u64 from} + up to 80 bytes (luanet's piece), through the same transport as the rest.
#include "../worldapi.h"
void luanet_heard(uint64_t from, const uint8_t *b, int n);
int world_net_up(void) { return g_world && w_send != NULL; }
uint64_t world_self_id(void) { return g_self; }
void world_net_send(const uint8_t *body, int n) {
    if (n <= 0 || n > 80) return;
    WorldPkt k = {.type = 10, .ver = 1, .from = g_self};
    memcpy((uint8_t *)&k + 16, body, (size_t)n);
    world_send(&k, 16 + (size_t)n);
}
// the other players heard (positions as last heard; standin 1: theirs is drawn here)
int world_players_list(LarecompPlayer *out, int max) {
    int n = 0;
    float me[3];
    bool have_me = warp_player_now(me, NULL);
    uint64_t now = world_now_ns();
    for (int i = 0; i < MAXPEER && n < max; i++) {
        const Peer *p = &g_peers[i];
        if (!p->id || now - p->heard_ns > PEER_TIMEOUT_NS * 2) continue;
        LarecompPlayer *o = &out[n++];
        o->id = p->id, o->yaw = p->yaw * 57.29578f;
        memcpy(o->pos, p->pos, 12);
        o->standin = p->show && p->node;
        if (o->standin) memcpy(o->pos, p->shown, 12);
        o->dist = have_me ? sqrtf((o->pos[0] - me[0]) * (o->pos[0] - me[0]) + (o->pos[2] - me[2]) * (o->pos[2] - me[2])) : -1;
    }
    return n;
}
// where that player's stand-in stands: 1 (feet), 0 not drawn now, -1 no such player
int world_player_feet(uint64_t id, float feet[3]) {
    for (int i = 0; i < MAXPEER; i++) {
        const Peer *p = &g_peers[i];
        if (p->id != id) continue;
        if (!(p->show && p->node)) return 0;
        memcpy(feet, p->shown, 12);
        return 1;
    }
    return -1;
}
