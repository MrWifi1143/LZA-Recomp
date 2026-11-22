// Characters of our own, made by the game (Legends: Arceus 1.1.1): the shared world's other players (hle/pla_world.c)
// each get a person the game makes for us, walked by the game's own navigation - not a townsperson borrowed.
//
// How Arceus makes its people: a level's placement list bin/field/param/placement/<area>/npc/npc.bin (a FlatBuffer,
// loaded with the level: {0 [entry {0 name, 1 FNV-1a of the name, 2 character hash, 4 [variant {2 work, 3 from, 4 to,
// 7 position {0 x, 1 y, 2 z}, 8 rotation, 9 scale}] ...}]}) is walked at load by 0x7001e677a4, and each entry is made by
// PLACE_NPC 0x7001e6d130(x0 the level's NPC manager, x1 [[0x70042a6f80]+0x100], x2 the entry's table, x3, x4): a name
// already made (0x7001745504 asks the manager's set of name hashes, entry field 1) gives request state 1, else the NPC
// factory (id 1) makes it through 0x70008ad7f0. Events make one by name later the same way (FObjCreatePlacement ->
// 0x7001e78120 -> job 0x7001e7da14: x3 = &out (0), x4 = a request {u32 0, u32 0x26, u64 the empty hash, vtable
// 0x7004128f60, .., +0x40 16 bytes from 0x7003984030, u32 0}).
// Ours: a copy of an entry the level made at load (the window of the loaded file around it - every offset in it is
// relative), its name "mpplayer_<k>" (string and hash) and every variant's position changed, its character the
// survey corps senior of the other player's gender (the default player look: tr0036 / tr0037 senpai), made by the same
// call from the game's thread (the camera update 0x7002994b70, hle/warp.c).
//
// Walking: each person has a CharacterNavigatorController (vtable 0x7004127458; its init 0x70017187d0 stores the
// character's spot at +0x3e0) - the one event scripts move people with (FObjMove: SetPreferredMoveState 1 run / 2 walk,
// SetDestination(&vec3): navmesh-projected goal at +0x250, the game walks it there with its own animation). Ours is the
// navigator whose init happens at our spawn spot after our request. Orders (destination + walk/run) are given from the
// camera update too. A level change takes ours with it (the manager changes): everything here starts over.
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../hle.h"
#include "../platform.h"

void rt_register_fnhook_at(uint64_t addr, int (*fn)(Cpu *c, uint64_t addr));
void rt_call_original(Cpu *c, uint64_t addr);

#define PLACE_NPC 0x7001e6d130ull
#define PLACE_LOAD_LR 0x7001e678a8ull  // (the level load's call)
#define FACTORY_CREATE 0x70008ad7f0ull
#define SINGLETON_VAR 0x70042a6f80ull
#define REQ_VTABLE 0x7004128f60ull
#define REQ_CONST 0x7003984030ull
#define NAV_INIT 0x70017187d0ull
#define NAV_VTABLE 0x7004127458ull
#define NAV_SET_DEST 0x700171bb88ull
#define NAV_SET_MOVESTATE 0x700171d458ull
#define NAV_SET_STEERING 0x700171e30cull
#define NAV_STOP 0x700171b198ull  // (StopPathfinding)  // (SetIsSteering(bool): FObjMove sets false after each destination)
#define MODEL_BUILD 0x7001a2b778ull  // (the player's model from its dress-up: [x0+0x248] != 0 = the p2 body)
// The character controller (vtable 0x700413c888) of each character gets its navigator at setup: SetNavigator
// 0x700197344c(ctrl, &weak navigator) keeps it at ctrl+0x258 ({table entry, handle}: navigator = [entry+0x18]-0x50) and
// installs the navigator's root-motion step (0x7001979fe8) - the step that walks a character toward its destination
// with its own walk/run animation. That step only runs while the byte ctrl+0x240 is 0 (the character's transform
// provider 0x7001978570 takes the plain animation path 0x70019748d4 while it is set; ctrl vfunc +0xf0 0x70017d46b8 sets
// it). The level's standing people (guards, townspeople at their spot: 12 of the 41 in Jubilife's first area) have it
// set - and ours are copies of them. With it set, the navigator takes the order (+0x240 moving, its AI character added),
// but the AI character is given the person's own spot as its goal (cmd 0x47 = the animation's next position), finds a
// one-point path, reports "goal reached" (AI event 6 -> the completion callback 0x7001725010 -> +0x418 -> the next
// update's StopPathfinding clears +0x240): the person stays where it was made. Cleared, it walks (Lua-traced 2026-11-18).
#define SET_NAVIGATOR 0x700197344cull
#define CTRL_VTABLE 0x700413c888ull
#define CC_VTABLE 0x700413cad8ull  // (the physics character: SetForwardDirection, SetVelocity, Warp are its)
#define CTRL_FIXED 0x240
#define CHR_SENPAI_M 0x2d992cf818c93910ull  // tr0036_00_senpai_m
#define CHR_SENPAI_F 0xfca68432c5c9b79eull  // tr0037_00_senpai_f

static bool in_heap(uint64_t a) { return a >= HEAP_BASE && a < HEAP_BASE + HEAP_MAX; }
static bool guest_ptr(uint64_t a) { return a >= 0x7000000000ull && a < 0x8000000000ull; }  // (our copies live in hle_alloc memory)

// FlatBuffer table helpers (guest memory)
static uint32_t fb_field(uint64_t t, int f) {  // (offset of field f in table t, 0 when absent)
    int32_t so;
    memcpy(&so, (const void *)(uintptr_t)t, 4);
    uint64_t vt = t - (int64_t)so;
    if (!guest_ptr(vt)) return 0;
    uint16_t vlen = *(const uint16_t *)(uintptr_t)vt;
    if (4 + 2 * f >= vlen) return 0;
    return *(const uint16_t *)(uintptr_t)(vt + 4 + 2 * f);
}
static uint64_t fb_ref(uint64_t t, int f) {
    uint32_t o = fb_field(t, f);
    if (!o) return 0;
    uint64_t p = t + o;
    return p + *(const uint32_t *)(uintptr_t)p;
}
static const char *fb_str(uint64_t t, int f, uint32_t *len) {
    uint64_t p = fb_ref(t, f);
    if (!p || !guest_ptr(p)) return NULL;
    if (len) *len = *(const uint32_t *)(uintptr_t)p;
    return (const char *)(uintptr_t)(p + 4);
}
static uint64_t fnv(const char *s) {
    uint64_t h = 0xcbf29ce484222645ull;
    for (; *s; s++) h = (h ^ (uint8_t)*s) * 0x100000001b3ull;
    return h;
}

// the level's entries, as its load makes them (made = the factory made one for it)
#define MAXENT 512
static struct { uint64_t e, chr; char name[48]; int made; } g_ent[MAXENT];
static int g_nent;
static volatile uint64_t g_mgr;
static volatile uint64_t g_level;  // (counts level loads: ours go with a level)
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static __thread int t_load_ent = -1;  // (the entry the load is making on this thread)
static volatile int g_ours;
static volatile int g_gender = -1;  // (ours: 0 the p1 body, 1 p2; -1 not seen yet)

// our people
#define NOWN 32  // (the shared world's players' people and scripts' characters)
typedef struct {
    int state;  // 0 free, 1 asked for, 2 made (its navigator not seen yet), 3 walking orders taken, 4 up and nobody's,
                // 5 made but let go before it came up (it becomes 4 when it does: kept, not left standing there),
                // 6 given up waiting for it (as 5: a free one if the game makes it after all)
    int gender, k;
    int script;         // (a script's character - the mod API, below: named luanpc_<k>, looks like schr / a copy of sent)
    int refused;        // (its last order wasn't taken: no navigation mesh there - the mod API stops its walk)
    float cur[3];       // (a script's character: where it is drawn - its skeleton, below - for its goal tolerance)
    int cur_ok;
    uint64_t schr;      // (its character hash; 0: the copied placement's own)
    int sent;           // (the placement to copy, + 1; 0: any townsperson's)
    float at[3];        // where it was made
    uint64_t nav, made_frame, up_frame;
    uint64_t ctrl;      // (its character controller, once seen: ctrl+0x240 kept 0 so the navigator walks it)
    uint64_t cc;        // (its physics character, vtable 0x700413cad8: the navigator's +0x1f0)
    uint64_t level;
    float dest[3];      // the newest order
    int run, order;     // (order: a new one to give)
    int run_set;        // (the move state given last: 0 none, 1 run, 2 walk)
    int shrink;         // (a let-go person's walk-off orders refused so far: each retry half as far)
    uint64_t asked;     // (frame asked)
    uint64_t outfit[12];  // (their dress-up items; none heard: the senior's look)
    int has_outfit;
    int dressed;        // (1 made with the player's body - its entity not ready yet, 2 its entity seen (dressed when comp))
    int redress;        // (their changed items to put on)
    int modeled;        // (its model made: the NPC model job came for it - dressed or not)
    uint64_t worn[12];  // (the items put on it so far)
    uint64_t comp;      // (its dress-up component, once dressed)
    uint64_t owner;     // (its entity's owner: the dress-up and its own components are on it)
    uint64_t alloc;     // (the allocator its model was made with)
    uint64_t model;     // (its model, from the maker: its entity's +0x90)
    int settled;        // (its dress-up seen settled - logged once)
    uint64_t chr;       // (its NPC character: the model job's, the entity callback's x0)
    int lod_set;
    uint64_t actl;      // (its animation component, vtable 0x7004146210: its parameters set by hash)
    float mf;           // (move_front given last; -1 none yet)
    // walked like a player (dressed: the player's net) - see puppet_step
    float fo[5];        // (where to be: x y z, their yaw, their speed)
    int fo_set, moving;
    float face[2];      // (the forward direction given last)
    uint64_t fo_ns, step_ns, mf_frame, face_frame, step_frame;
    int nudge;
    uint64_t far_frame, stuck_frame;
    float stuck_at[3];
    int warp_now;       // (research: <live>/npc "pwarp <slot>")
    float want_v;       // (the speed it should go: steered last frame)
    int turn_sign;      // (the way it is turning round: 1 left, -1 right, 0 not)
    float lyaw;         // (its model turned about y on top of its character's facing: owner local rotation)
    float fa;           // (the way it was drawn facing last frame)
    int fa_set;
    // riding (their set_ride_bool / ride_type_int, mirrored): a Pokémon of ours carries it - see ride_step
    int riding, ride_type, rode;  // (rode: the ride type it sat on last frame, 0 none)
    int mnt[8];         // (its mounts by ride type: index + 1 into g_mnt, 0 none)
    int swimming, swam; // (their swim_bool; swam: swimming mode last frame)
    int mnt_fail;       // (mounts asked for that never came up)
    uint64_t mnt_retry;
    // their ride Pokémon's net parameters (k_rmirror), heard: set on our mount of that ride model (ride_step)
    char rp_type[64];
    uint32_t rp_val[64];
    uint64_t rp_dirty, rp_fire;
    float ax[6];        // (their ride's up and forward axes, riding a climber / swimmer: ax_ok)
    int ax_ok;
    float rlq[4];       // (the rider's local rotation written last frame)
    float rlt[3];       // (and its local translation)
    int rl_set;         // (rlq / rlt hold what is in its owner)
    int rhidden;        // (not drawn while it rides in Sneasler's basket: its meshes' LOD masks kept in rmask)
    uint64_t rmask[24];
} Own;
// research knobs, <live>/npc "knob <i> <value>" (puppet_step): 0 up/down (1 down only, 0 also lifted as before, 2 none),
// 1 a mount's "move" per m/s (ride_step), 2 the rider's seat higher by, 4 the rider's turn about x (radians, added), 5 the
// rider's character under its mount by (m),
// 3 the run's step stretch at most (1.12), 6 bits: 1 turning round without keeping its way, 2 no their-way blend close
// behind, 4 no model turn on top (facing stalls back), 7 the walk log (1 every second,
// 2 every frame - LARECOMP_NPC_WALKLOG the same)
static float g_knob[8] = {1, 0.25f, 0, 1.12f, 0, 0, 0, 0};
static Own g_own[NOWN];
static volatile uint64_t g_frame;
static int g_count;

static volatile uint64_t g_place_seen_ns;  // (the mod API's area_loaded: the last placement the level's load made)
static int place_hook(Cpu *c, uint64_t addr) {
    if (c->x[30] != PLACE_LOAD_LR) return 0;
    uint64_t e = c->x[2];
    g_place_seen_ns = plat_now_ns();
    pthread_mutex_lock(&g_lock);
    if (g_mgr != c->x[0]) {
        g_nent = 0, g_level++;
        for (int i = 0; i < NOWN; i++)
            if (g_own[i].state) LOG("[npc] %s_%d went with the level\n", g_own[i].script ? "luanpc" : "mpplayer", g_own[i].k), memset(&g_own[i], 0, sizeof g_own[i]);
        LOG("[npc] the level's NPC manager %llx\n", (unsigned long long)c->x[0]);
    }
    g_mgr = c->x[0];
    int i = -1;
    if (g_nent < MAXENT && in_heap(e)) {
        uint32_t n = 0;
        const char *s = fb_str(e, 0, &n);
        i = g_nent++;
        g_ent[i].e = e, g_ent[i].made = 0;
        snprintf(g_ent[i].name, sizeof g_ent[i].name, "%.*s", s && n < 48 ? (int)n : 1, s && n < 48 ? s : "?");
        uint32_t o = fb_field(e, 2);
        g_ent[i].chr = o ? *(const uint64_t *)(uintptr_t)(e + o) : 0;
    }
    pthread_mutex_unlock(&g_lock);
    static int nowrap = -1;
    if (nowrap < 0) nowrap = getenv("LARECOMP_NPC_NOHOOK") && (atoi(getenv("LARECOMP_NPC_NOHOOK")) & 1);
    if (nowrap) { if (i >= 0) g_ent[i].made = 1; return 0; }
    t_load_ent = i;
    rt_call_original(c, addr);
    t_load_ent = -1;
    return 1;
}

static int fcreate_hook(Cpu *c, uint64_t addr) {
    (void)c, (void)addr;
    if ((unsigned)c->x[1] == 1 && t_load_ent >= 0 && t_load_ent < g_nent) g_ent[t_load_ent].made = 1;
    return 0;
}

// a navigator set up: its init (0x70017187d0) asks GetLocalTranslation 0x70017199f4 for the character's spot (lr
// 0x7001718908) and keeps it at +0x3e0. That call is watched without touching the init itself (a hook calling the init
// through left its person standing for good); the navigators seen are matched on the camera update, by that spot.
#define NAV_POS_FN 0x70017199f4ull
#define NAV_POS_LR 0x7001718908ull
static uint64_t g_navq[32], g_navactl[32], g_navchr[32];
static volatile int g_navn;
// A character's components are made by 0x7001c271f4 on one thread: its animation component (constructor 0x7001ade124,
// vtable 0x7004146210) first, then its navigator's init - so the navigator seen comes with the animation component made
// last on that thread (the builder's start forgets the one before).
#define CHAR_BUILD 0x7001c271f4ull
#define ANIM_CTOR 0x7001ade124ull
#define ANIM_VTABLE 0x7004146210ull
// Before the builder its model component (vtable 0x7004146710, constructor 0x7001ae21a8; the NPC model job 0x7001ae417c
// is its vfunc +0x138: x0 = it) is made, on the same thread - so ours are known by it when their model is made.
#define NPC_MODEL_CTOR 0x7001ae21a8ull
static __thread uint64_t t_actl, t_chr;
static int build_hook(Cpu *c, uint64_t addr) { (void)c, (void)addr; t_actl = 0; return 0; }
static int actor_hook(Cpu *c, uint64_t addr) { (void)addr; t_actl = c->x[0]; return 0; }
static int chr_ctor_hook(Cpu *c, uint64_t addr) { (void)addr; t_chr = c->x[0]; return 0; }
static int nav_pos_hook(Cpu *c, uint64_t addr) {
    (void)addr;
    if (c->x[30] != NAV_POS_LR || !in_heap(c->x[0])) return 0;
    pthread_mutex_lock(&g_lock);
    if (g_navn < 32) g_navactl[g_navn] = t_actl, g_navchr[g_navn] = t_chr, g_navq[g_navn++] = c->x[0];
    t_actl = t_chr = 0;
    pthread_mutex_unlock(&g_lock);
    return 0;
}
// controllers given a navigator lately (SetNavigator), by navigator: ours is looked up when its navigator is matched
#define NCTL 64
static struct { uint64_t nav, ctrl; } g_ctl[NCTL];
static unsigned g_nctl;
static int setnav_hook(Cpu *c, uint64_t addr) {
    (void)addr;
    uint64_t ctrl = c->x[0], w = c->x[1];
    if (!in_heap(ctrl) || !guest_ptr(w)) return 0;  // (the weak reference is on the stack)
    uint64_t entry = *(const uint64_t *)(uintptr_t)w;
    if (!in_heap(entry)) return 0;
    uint64_t p = *(const uint64_t *)(uintptr_t)(entry + 0x18), nav = p - 0x50;
    if (!in_heap(nav) || *(const uint64_t *)(uintptr_t)nav != NAV_VTABLE) return 0;
    pthread_mutex_lock(&g_lock);
    g_ctl[g_nctl++ % NCTL].nav = nav, g_ctl[(g_nctl - 1) % NCTL].ctrl = ctrl;
    pthread_mutex_unlock(&g_lock);
    return 0;
}
static uint64_t ctrl_of(uint64_t nav) {  // (g_lock held)
    for (unsigned i = 0; i < NCTL; i++)
        if (g_ctl[i].nav == nav && in_heap(g_ctl[i].ctrl) && *(const uint64_t *)(uintptr_t)g_ctl[i].ctrl == CTRL_VTABLE) return g_ctl[i].ctrl;
    return 0;
}
static void unfix(Own *o) {  // (its controller's "stands at its spot" byte cleared: the navigator's root motion steps it)
    if (!o->ctrl || *(const uint64_t *)(uintptr_t)o->ctrl != CTRL_VTABLE) return;
    uint8_t *f = (uint8_t *)(uintptr_t)(o->ctrl + CTRL_FIXED);
    static int keep = -1;  // (research LARECOMP_NPC_KEEPFIXED=1: left as the copied entry had it - the person stands)
    if (keep < 0) keep = getenv("LARECOMP_NPC_KEEPFIXED") && atoi(getenv("LARECOMP_NPC_KEEPFIXED"));
    if (*f && !keep) {
        static int nlog;
        if (nlog++ < 8) LOG("[npc] mpplayer_%d: its controller %llx stood at its spot - let go so its navigator walks it\n", o->k, (unsigned long long)o->ctrl);
        *f = 0;
    }
}

// IsOutOfGoalTolerance 0x700171b79c(nav, &goal): SetDestination takes an order only when the goal is out of the
// character's goal tolerance (its AI component's radius, ~1.5 m for these people): a player walking slowly moves less
// than that between orders, so every order was refused and the person never walked. Ours answer with 0.35 m (1 m up/down).
#define NAV_OUT_OF_TOL 0x700171b79cull
static int tol_hook(Cpu *c, uint64_t addr) {
    (void)addr;
    uint64_t nav = c->x[0];
    bool ours = false;
    for (int i = 0; i < NOWN && !ours; i++) ours = g_own[i].nav == nav && g_own[i].state >= 3;
    if (!ours || !guest_ptr(c->x[1])) return 0;
    const float *g = (const float *)(uintptr_t)c->x[1], *p = (const float *)(uintptr_t)(nav + 0x3e0);
    for (int i = 0; i < NOWN; i++)  // (a script's character: from where it is - +0x3e0 is its last goal by then)
        if (g_own[i].nav == nav && g_own[i].script && g_own[i].cur_ok) p = g_own[i].cur;
    float dx = g[0] - p[0], dz = g[2] - p[2];
    c->x[0] = dx * dx + dz * dz > 0.35f * 0.35f || fabsf(g[1] - p[1]) > 1.0f;
    return 1;
}

// ---- mounts: Pokémon of our own a person rides (see poke_make / mount_attach below)
#define NMNT 16
#define POKE_MODEL_VTABLE 0x70041495d0ull  // (a Pokémon's model component: the NPC model job's counterpart 0x7001b20ed8 is its vfunc +0x138)
#define POKE_MODEL_CTOR 0x7001b1ff2cull
#define POKE_ANIM_CTOR 0x7001afe6e4ull     // (its animation component, vtable 0x7004147c18: "move", "turn", ... of pm<n>_base.tracp)
#define POKE_ANIM_VTABLE 0x7004147c18ull
#define POKE_CC_VTABLE 0x700413ef88ull     // (its physics character)
typedef struct {
    int state;          // 0 free, 1 to ask for, 2 asked (its navigator not seen yet), 3 up
    int mons, k, rider; // (rider: the person on it + 1, 0 none)
    float at[3], yaw;
    char name[32];
    uint64_t nav, cc, chr, actl, owner, asked, up_frame, level, ctrl;
    int attached;       // (its rider attached to its joint)
    int attach_req;     // (research "mount": attach_to tried)
    char joint[16];
    int hidden;         // (put away: its meshes' LOD masks kept in lods)
    uint64_t lods[16];
    int t;              // (the ride type it is made for: its model is that ride Pokémon's - see ride_model)
    int remodel;        // (1: made with the ride model, saddle and riding net)
    uint64_t chr_pre;   // (its model component, seen while poke_make asked for it)
    uint64_t used_ns;   // (last ridden: one unused a while is removed - mount_term)
    int term;           // (to be removed: its rider let go)
    int research;       // (made by the live "poke" line: no rider rules)
    float lq[4];        // (its local rotation written last frame: tilted onto a wall / the water)
    uint64_t jsk;       // (its skeleton, where its seat joint was looked up)
    int jidx;           // (the seat joint's matrix index + 1 in jsk; -1 not found)
    int form, rare;     // (a script's Pokémon - the mod API's pokemon.spawn: its form, shiny)
    int script;
    struct { uint64_t hash; char kind; float v; } anim[8];  // (its animation's parameters asked for: object_anim)
    volatile int nanim;
} Mnt;
static Mnt g_mnt[NMNT];
static int g_poke_n;
static struct { uint64_t chr, owner; } g_pent[32];  // (Pokémon entities seen lately: model component -> owner)
static unsigned g_npent;
static uint64_t poke_owner(uint64_t chr) {  // (g_lock held)
    for (unsigned i = 0; i < 32; i++)
        if (g_pent[i].chr == chr && in_heap(g_pent[i].owner) && *(const uint64_t *)(uintptr_t)g_pent[i].owner == 0x700417dc80ull) return g_pent[i].owner;
    return 0;
}

static void nav_match(void) {
    if (!g_navn) return;
    extern bool warp_player_pos(float out[3]);
    float me[3];
    bool have_me = warp_player_pos(me);
    pthread_mutex_lock(&g_lock);
    for (int q = 0; q < g_navn; q++) {
        uint64_t nav = g_navq[q];
        if (*(const uint64_t *)(uintptr_t)nav != NAV_VTABLE) continue;
        const float *p = (const float *)(uintptr_t)(nav + 0x3e0);
        if (have_me && fabsf(p[0] - me[0]) < 1.5f && fabsf(p[2] - me[2]) < 1.5f) continue;  // (the player's own)
        for (int i = 0; i < NOWN; i++) {
            Own *o = &g_own[i];
            float tol = o->script ? 3.0f : 1.0f;  // (a script's spot may be off the navigation mesh: the game puts it on the nearest)
            if ((o->state != 2 && o->state != 5 && o->state != 6) || fabsf(p[0] - o->at[0]) > tol || fabsf(p[2] - o->at[2]) > tol) continue;
            if (o->state == 6) LOG("[npc] mpplayer_%d came up after all: a free one now\n", o->k);
            o->nav = nav, o->state = o->state == 2 ? 3 : 4, o->order = 0, o->up_frame = g_frame;  // (no order before one is given: an order to where it stands left it unable to walk)
            o->ctrl = ctrl_of(nav);
            { uint64_t e = *(const uint64_t *)(uintptr_t)(nav + 0x1f0), q2 = in_heap(e) ? *(const uint64_t *)(uintptr_t)(e + 0x18) : 0;
              o->cc = in_heap(q2 - 0x50) && *(const uint64_t *)(uintptr_t)(q2 - 0x50) == CC_VTABLE ? q2 - 0x50 : 0; }
            o->actl = in_heap(g_navactl[q]) && *(const uint64_t *)(uintptr_t)g_navactl[q] == ANIM_VTABLE ? g_navactl[q] : 0, o->mf = -1;
            o->chr = in_heap(g_navchr[q]) ? g_navchr[q] : 0;
            LOG("[npc] %s_%d is up: navigator %llx at %.2f %.2f %.2f (%llu frames after asking), controller %llx, animation %llx\n", o->script ? "luanpc" : "mpplayer", o->k, (unsigned long long)nav,
                p[0], p[1], p[2], (unsigned long long)(g_frame - o->made_frame), (unsigned long long)o->ctrl, (unsigned long long)o->actl);
            unfix(o);
            break;
        }
        for (int i = 0; i < NMNT; i++) {  // (a mount of ours)
            Mnt *m = &g_mnt[i];
            // (within 4 m: the game puts one asked for off the navigation mesh - on a rock, in water - on the nearest walkable spot)
            if (m->state != 2 || fabsf(p[0] - m->at[0]) > 4.0f || fabsf(p[2] - m->at[2]) > 4.0f) continue;
            m->nav = nav, m->state = 3, m->up_frame = g_frame, m->ctrl = ctrl_of(nav), m->used_ns = plat_now_ns();
            { uint64_t e = *(const uint64_t *)(uintptr_t)(nav + 0x1f0), q2 = in_heap(e) ? *(const uint64_t *)(uintptr_t)(e + 0x18) : 0;
              m->cc = in_heap(q2 - 0x50) ? q2 - 0x50 : 0; }
            m->actl = in_heap(g_navactl[q]) && *(const uint64_t *)(uintptr_t)g_navactl[q] == POKE_ANIM_VTABLE ? g_navactl[q] : 0;
            m->chr = in_heap(g_navchr[q]) ? g_navchr[q] : 0;
            LOG("[npc] %s is up: navigator %llx at %.2f %.2f %.2f (%llu frames after asking), physics %llx (vt %llx), animation %llx, model %llx\n", m->name, (unsigned long long)nav,
                p[0], p[1], p[2], (unsigned long long)(g_frame - m->asked), (unsigned long long)m->cc, (unsigned long long)(m->cc ? *(const uint64_t *)(uintptr_t)m->cc : 0),
                (unsigned long long)m->actl, (unsigned long long)m->chr);
            break;
        }
    }
    g_navn = 0;
    pthread_mutex_unlock(&g_lock);
}

// ---- the other player's own look (their clothes, hair, eyes, skin - the dress-up), worn by ours
// How the game dresses a player-like person (Lua-traced + read 2026-11-18; the field player 0x7001c682f0/0x7001c862fc/
// 0x7001c56008 and the cutscene player 0x700142c0b4/0x700142ea9c/0x700142c940 do the same):
// (1) its model is made from the player desc 0x700125eb8c(alloc, body) (the shared body bin/chara/data/pc/p<n>/model/
//     base/p<n>_base0001_00_default.trmdl - no meshes of its own - the player's motion and locators) by the model maker
//     0x700142b4a4(out, alloc, x2, desc); 0x7001252900(world, 0, &model, callbacks, 0) then makes an entity for it
//     (entity +0x90 the model, +0x68 its owner - a scene object, vtable 0x700417dc80 - +0xb0 the dress-up context) and
//     calls back once it is set up (the owner then has its model component, vtable 0x70040d4ac0). An NPC goes the same
//     way: its model job 0x7001ae417c (vfunc +0x138 of the NPC character) calls the maker, 0x70018fd3dc makes the
//     entity, and its callback 0x70018fd9ec(character, &entity) runs when it is ready.
// (2) in the callback three components are made on the owner - 0x700125be64(owner, alloc) (vtable 0x7004145c70),
//     0x700125c050(owner, alloc), 0x700142f590(owner, alloc) - and the first two linked to the entity's resources
//     (entity +0x70 / +0x78 handles -> 0x70007802f0 -> 0x70009b3c84 / 0x70009a03f0 -> comp+0x1f0 setters, vfunc +0xf0,
//     then 0x700125c614 / 0x700125cd08 on comp+0x200). They are the person's own (face, eye, skin materials: written to -
//     sharing the player's turned the player's face white).
// (3) the dress-up component 0x7001253354(owner, context, params 0x68: +0 / +0x10 / +0x40 handles taken from those three
//     components, +0x50 worn item 11, +0x58 the body (0 p1, 1 p2)) and each worn item put on by 0x7001a2b778(component,
//     &{u64 item, ..}, 0) - the item = FNV-1a of its name in bin/chara/table/dressup_table_p1.bin / _p2.bin (e.g.
//     "p1_hrs0001_00_default_black"); the save keeps the 12 worn items at +0x68 of its dress-up data (none = the empty
//     hash). The owner updates the component itself (its state +0x27b8: 1 = settled).
// (4) its parts are drawn only while their LOD controller has a LOD (lod_show below).
// Ours (default; research LARECOMP_NPC_DRESS=0: all the senior): an NPC whose player has sent an outfit ('O1',
// hle/pla_world.c) has its model job's desc swapped for the player desc (create_hook: the job is known by its model
// component, nav_match), and in its entity callback (entity_hook) gets (2) and (3) with that outfit; (4) is put right
// in the camera update. It then runs the player's control net (pc0002_00.tracn) and is walked like a player: see
// puppet_step (2026-11-18: two copies through lzaserver, testout/pla-mp6 m2-*). Without an outfit heard ours are the
// senior of their body (the default player look), walked by their navigator. (The senior's net on the player skeleton
// crashes - std::out_of_range: its IK/spring bones index another skeleton.)
// A changed outfit is put on item by item; one with an item taken off (the game has no call for that) or another item 11
// (it's in the params) is made again from scratch (pla_npc_outfit answers 1: hle/pla_world.c lets the person go).
#define CREATE_MODEL 0x700142b4a4ull
#define NPC_MODEL_LR 0x7001ae48bcull
#define PLAYER_DESC 0x700125eb8cull
#define NPC_ENTITY_CB 0x70018fd9ecull
#define DRESS_MAKE 0x7001253354ull
#define DRESS_MAKE_PLAYER_LR 0x7001c5a284ull
#define SAVE_DRESS_SLOT 0x700101b7c0ull  // (x0 the save's dress-up data, w1 a slot: the field player asks for 11)
#define SAVE_DRESS_SLOT_LR 0x7001c5a268ull
#define EMPTY_HASH 0xcbf29ce484222645ull
#define MODEL_OBJ_VTABLE 0x700417dc80ull  // (an entity's owner: the scene object components are put on)
static volatile uint64_t g_player_comp, g_save_dress;

static volatile uint64_t g_player_owner;  // (the field player's model owner, scene object vtable 0x700417dc80)
static int dressmake_hook(Cpu *c, uint64_t addr) {
    if (c->x[30] != DRESS_MAKE_PLAYER_LR) return 0;
    g_player_owner = c->x[0];
    rt_call_original(c, addr);
    g_player_comp = c->x[0];
    LOG("[npc] the player's dress-up component %llx\n", (unsigned long long)c->x[0]);
    return 1;
}
static int saveslot_hook(Cpu *c, uint64_t addr) {
    (void)addr;
    if (c->x[30] == SAVE_DRESS_SLOT_LR && in_heap(c->x[0]) && g_save_dress != c->x[0]) {
        g_save_dress = c->x[0];
        LOG("[npc] the save's dress-up data %llx\n", (unsigned long long)c->x[0]);
    }
    return 0;
}
// research LARECOMP_WORLD_TRY_OUTFIT="<slot>=<item name>,...": we wear those (slots 0-10, put on our player - not saved)
// and send them - two test copies of one save then look different to each other
static int g_try_n, g_try_slot[12], g_try_done;
static uint64_t g_try_hash[12];
static void try_parse(void) {
    const char *e = getenv("LARECOMP_WORLD_TRY_OUTFIT");
    static char buf[1024];
    snprintf(buf, sizeof buf, "%s", e ? e : "");
    for (char *q = buf; *q; q++) if (*q == ',') *q = ' ';  // (comma or space separated)
    char item[96];
    int slot, n;
    for (const char *q = buf; g_try_n < 12 && sscanf(q, " %d=%95s%n", &slot, item, &n) == 2; q += n)
        if (slot >= 0 && slot < 11) g_try_slot[g_try_n] = slot, g_try_hash[g_try_n++] = fnv(item);
}
int pla_npc_my_outfit(uint64_t out[12]) {  // (the 12 items we wear: 0 when not known yet)
    uint64_t s = g_save_dress;
    if (!s) return 0;
    memcpy(out, (const void *)(uintptr_t)(s + 0x68), 96);
    for (int i = 0; i < g_try_n; i++) out[g_try_slot[i]] = g_try_hash[i];
    return 1;
}
static void try_wear(Cpu *c) {  // (the camera update: the player's dress-up component known)
    if (g_try_done || !g_player_comp || !g_save_dress) return;
    g_try_done = 1;
    uint64_t *w = (uint64_t *)(uintptr_t)(g_save_dress + 0x68);
    LOG("[npc] our outfit: %016llx %016llx %016llx %016llx %016llx %016llx %016llx %016llx %016llx %016llx %016llx %016llx\n", (unsigned long long)w[0],
        (unsigned long long)w[1], (unsigned long long)w[2], (unsigned long long)w[3], (unsigned long long)w[4], (unsigned long long)w[5], (unsigned long long)w[6],
        (unsigned long long)w[7], (unsigned long long)w[8], (unsigned long long)w[9], (unsigned long long)w[10], (unsigned long long)w[11]);
    static uint64_t *it;
    if (!it) it = hle_alloc(0x20);
    for (int i = 0; i < g_try_n; i++) {
        it[0] = g_try_hash[i];
        uint64_t sx[32], snzcv = c->nzcv;
        __uint128_t sv[32];
        memcpy(sx, c->x, sizeof sx), memcpy(sv, c->v, sizeof sv);
        c->x[0] = g_player_comp, c->x[1] = (uint64_t)(uintptr_t)it, c->x[2] = 0;
        guest_call(c, MODEL_BUILD);
        memcpy(c->x, sx, sizeof sx), memcpy(c->v, sv, sizeof sv), c->nzcv = snzcv;
        LOG("[npc] trying on slot %d: %016llx\n", g_try_slot[i], (unsigned long long)g_try_hash[i]);
    }
}
static bool outfit_any(const uint64_t *h) {
    for (int i = 0; h && i < 12; i++)
        if (h[i] && h[i] != EMPTY_HASH) return true;
    return false;
}

static int model_hook(Cpu *c, uint64_t addr) {
    (void)addr;
    if (in_heap(c->x[0]) && (!g_player_comp || c->x[0] == g_player_comp)) {  // (the player's only: ours get items too)
        int g = *(const uint8_t *)(uintptr_t)(c->x[0] + 0x248) != 0;
        if (g != g_gender) LOG("[npc] our player's body: %s\n", g ? "p2" : "p1");
        g_gender = g;
    }
    return 0;
}

int pla_npc_gender(void) { return g_gender; }

// the copy of an entry: a window of the loaded file around it (all its offsets are relative); name, its hash, every
// variant's position (where it has one) and the character changed. 0 when the entry has no position to change.
#define WIN 0x30000ull
static uint64_t copy_entry(uint64_t e, const char *name, const float at[3], uint64_t chr) {
    uint64_t lo = e - WIN < HEAP_BASE ? HEAP_BASE : e - WIN, hi = e + WIN > HEAP_BASE + HEAP_MAX ? HEAP_BASE + HEAP_MAX : e + WIN;
    lo &= ~15ull;
    uint8_t *w = hle_alloc(hi - lo + 16);
    if (!w) return 0;
    memcpy(w, (const void *)(uintptr_t)lo, hi - lo);
    uint64_t base = (uint64_t)(uintptr_t)w, end = base + (hi - lo), ce = base + (e - lo);
    uint64_t sp = fb_ref(ce, 0);
    if (!sp || sp < base || sp + 4 >= end) return 0;
    uint32_t n = *(uint32_t *)(uintptr_t)sp, k = (uint32_t)strlen(name);
    if (k > n) return 0;
    memcpy((void *)(uintptr_t)(sp + 4), name, k + 1), *(uint32_t *)(uintptr_t)sp = k;
    uint32_t ho = fb_field(ce, 1), co = fb_field(ce, 2);
    uint64_t h = fnv(name);
    if (!ho) return 0;
    memcpy((void *)(uintptr_t)(ce + ho), &h, 8);
    if (co && chr) memcpy((void *)(uintptr_t)(ce + co), &chr, 8);
    uint64_t vv = fb_ref(ce, 4);
    if (!vv || vv < base || vv + 4 >= end) return 0;
    uint32_t nv = *(const uint32_t *)(uintptr_t)vv, set = 0;
    for (uint32_t i = 0; i < nv && i < 64; i++) {
        uint64_t vp = vv + 4 + 4 * i, v = vp + *(const uint32_t *)(uintptr_t)vp;
        if (v < base || v + 8 >= end) continue;
        uint64_t pt = fb_ref(v, 7);
        if (!pt || pt < base || pt + 8 >= end) continue;
        uint32_t fx = fb_field(pt, 0), fy = fb_field(pt, 1), fz = fb_field(pt, 2);
        if (!fx || !fy || !fz) continue;
        memcpy((void *)(uintptr_t)(pt + fx), &at[0], 4), memcpy((void *)(uintptr_t)(pt + fy), &at[1], 4), memcpy((void *)(uintptr_t)(pt + fz), &at[2], 4);
        set++;
    }
    return set ? ce : 0;
}

static void guest_call_saved(Cpu *c, uint64_t fn, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
    uint64_t sx[32], snzcv = c->nzcv;
    __uint128_t sv[32];
    memcpy(sx, c->x, sizeof sx), memcpy(sv, c->v, sizeof sv);
    c->x[0] = a0, c->x[1] = a1, c->x[2] = a2, c->x[3] = a3, c->x[4] = a4;
    guest_call(c, fn);
    memcpy(c->x, sx, sizeof sx), memcpy(c->v, sv, sizeof sv), c->nzcv = snzcv;
}

static int g_mod_copies;  // (copies made for scripts in this level: each takes ~400 KB of HLE memory - capped)
static uint64_t g_mod_copies_level;
static void make_one(Cpu *c, Own *o) {
    char name[32];
    snprintf(name, sizeof name, "%s_%d", o->script ? "luanpc" : "mpplayer", o->k);
    uint64_t chr = o->script ? o->schr : o->gender ? CHR_SENPAI_F : CHR_SENPAI_M, ce = 0;
    const char *like = "?";
    if (o->script) {
        if (g_mod_copies_level != g_level) g_mod_copies_level = g_level, g_mod_copies = 0;
        if (g_mod_copies >= 160) {
            LOG("[npc] %s: scripts made 160 characters in this level already - no more until the next level\n", name);
            o->state = 0;
            return;
        }
        g_mod_copies++;
    }
    pthread_mutex_lock(&g_lock);
    if (o->script && o->sent > 0 && o->sent <= g_nent)  // (a script asked for that placement's look)
        ce = copy_entry(g_ent[o->sent - 1].e, name, o->at, chr), like = g_ent[o->sent - 1].name;
    // (a townsperson the level made itself first - "_npc_g" names are the plain ones - then any it made, then any; never
    // a senior's entry: theirs belongs to the story)
    static const char *force = (const char *)1;  // (research LARECOMP_NPC_LIKE=<entry name>: that one)
    if (force == (const char *)1) force = getenv("LARECOMP_NPC_LIKE");
    for (int i = 0; force && i < g_nent && !ce; i++)
        if (!strcmp(g_ent[i].name, force)) ce = copy_entry(g_ent[i].e, name, o->at, chr), like = g_ent[i].name;
    for (int pass = 0; pass < 3 && !ce; pass++)
        for (int i = 0; i < g_nent && !ce; i++) {
            if (strstr(g_ent[i].name, "senior") || g_ent[i].chr == CHR_SENPAI_M || g_ent[i].chr == CHR_SENPAI_F || g_ent[i].chr == 0x5facc1d35b48a02dull) continue;
            if (pass == 0 && (!g_ent[i].made || !strstr(g_ent[i].name, "_npc_g"))) continue;
            if (pass == 1 && !g_ent[i].made) continue;
            ce = copy_entry(g_ent[i].e, name, o->at, chr), like = g_ent[i].name;
        }
    pthread_mutex_unlock(&g_lock);
    if (!ce) {
        LOG("[npc] %s: no placement of this level to copy (%d kept)\n", name, g_nent);
        o->state = 0;
        return;
    }
    uint64_t single = *(const uint64_t *)(uintptr_t)SINGLETON_VAR, x1 = single ? *(const uint64_t *)(uintptr_t)(single + 0x100) : 0;
    uint8_t *req = hle_alloc(0x60), *out = hle_alloc(16);
    uint64_t empty = 0xcbf29ce484222645ull, vt = REQ_VTABLE;
    uint32_t k26 = 0x26;
    memcpy(req + 4, &k26, 4), memcpy(req + 8, &empty, 8), memcpy(req + 0x10, &vt, 8);
    memcpy(req + 0x40, (const void *)(uintptr_t)REQ_CONST, 16);
    g_ours = 1;
    guest_call_saved(c, PLACE_NPC, g_mgr, x1, ce, (uint64_t)(uintptr_t)out, (uint64_t)(uintptr_t)req);
    g_ours = 0;
    uint32_t st;
    memcpy(&st, req, 4);
    o->state = st == 0 ? 2 : 0, o->made_frame = g_frame;
    if (o->script) LOG("[npc] %s asked for at %.2f %.2f %.2f (a copy of %s, character %016llx): %s\n", name, o->at[0], o->at[1], o->at[2], like,
                       (unsigned long long)chr, st == 0 ? "made" : "refused");
    else LOG("[npc] %s asked for at %.2f %.2f %.2f (a copy of %s, the %s senior): %s\n", name, o->at[0], o->at[1], o->at[2], like, o->gender ? "female" : "male",
        st == 0 ? "made" : "refused");
}

// a guest call's x0, every register put back (x8 = the result slot for functions returning a struct)
static uint64_t gcall(Cpu *c, uint64_t fn, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t x8) {
    uint64_t sx[32], snzcv = c->nzcv, r;
    __uint128_t sv[32];
    memcpy(sx, c->x, sizeof sx), memcpy(sv, c->v, sizeof sv);
    c->x[0] = a0, c->x[1] = a1, c->x[2] = a2, c->x[8] = x8;
    guest_call(c, fn);
    r = c->x[0];
    memcpy(c->x, sx, sizeof sx), memcpy(c->v, sv, sizeof sv), c->nzcv = snzcv;
    return r;
}

static uint64_t gcall_n(Cpu *c, uint64_t fn, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3) {
    uint64_t sx[32], snzcv = c->nzcv, r;
    __uint128_t sv[32];
    memcpy(sx, c->x, sizeof sx), memcpy(sv, c->v, sizeof sv);
    c->x[0] = a0, c->x[1] = a1, c->x[2] = a2, c->x[3] = a3;
    guest_call(c, fn);
    r = c->x[0];
    memcpy(c->x, sx, sizeof sx), memcpy(c->v, sv, sizeof sv), c->nzcv = snzcv;
    return r;
}

// the NPC model job (vfunc +0x138 of an NPC character: x0 the character) - its character noted for the maker's call
#define NPC_MODEL_JOB 0x7001ae417cull
static __thread uint64_t t_job_chr;
static int job_hook(Cpu *c, uint64_t addr) {
    uint64_t prev = t_job_chr;
    t_job_chr = c->x[0];
    rt_call_original(c, addr);
    t_job_chr = prev;
    return 1;
}

// the field player's model maker call (lr 0x7001c68634): its x1 (the loader its files are found with) kept
#define PLAYER_MAKER_LR 0x7001c68634ull
static volatile uint64_t g_player_loader;

// ---- our mounts wear the game's ride gear: the player's ride Pokémon are their own models (bin/archive/field/
// ride_pokemon.gfpak, bin/chara/data/pm/<m>/: the Galaxy saddle is part of the mesh) with their own control nets
// (<m>_rd.tracn: walking / running, swimming, climbing, gliding), made by the ride object's loader (0x70025d4248, its
// ride index at [+0x128]+0xd0: 0 Wyrdeer, 1 Sneasler, 2 Basculegion, 3 Braviary, 4 Ursaluna) through the model maker
// with a desc of kind 3 (+0xe0): explicit files - model (+0 list), net (+0x38), locator skeletons (+0x70) - each made
// from its path by 0x70008f1b84(&char *, x8 = 0x30 B handle) and added by 0x7002601ad8 (model / net lists) /
// 0x7002601a84 (skeletons); its pack object (+0x1e8) +0x68 = 2 (Lua-traced 2026-11-18, testout/pla-mp9). The game makes
// all opened rides at load and shows the one ridden. Ours: a Pokémon of ours (poke_make) whose model job's desc (a
// Pokémon's: kind 0, its monsno) is turned into that one before the maker runs - ride_model below.
#define POKE_MODEL_JOB 0x7001b20ed8ull  // (vfunc +0x138 of a Pokémon's model component: x0 that component)
#define POKE_MAKER_LR 0x7001b210c0ull
#define RES_PATH 0x70008f1b84ull
#define DESC_ADD_FILE 0x7002601ad8ull
#define DESC_ADD_SKEL 0x7002601a84ull
#define DESC_CTOR 0x7001252fe4ull  // (desc, allocator)
#define DESC_DTOR 0x7000a17558ull
#define EMPTY_STR 0x7003896322ull  // (the game's "")
static const char *const k_ride_mdl[8] = {NULL, "pm1001_00_00", "pm1006_11_41", "pm1007_00_00", "pm0628_00_41", "pm1003_00_41", NULL, NULL};
static __thread uint64_t t_pjob;
static Mnt *volatile g_making;  // (the mount poke_make is asking for: its model component noted if made at once)
static int pjob_hook(Cpu *c, uint64_t addr) {
    uint64_t prev = t_pjob;
    t_pjob = c->x[0];
    rt_call_original(c, addr);
    t_pjob = prev;
    return 1;
}
static int poke_ctor_hook(Cpu *c, uint64_t addr) {
    t_chr = c->x[0];
    Mnt *m = g_making;
    if (m && !m->chr_pre) m->chr_pre = c->x[0];
    (void)addr;
    return 0;
}
static void res_release(Cpu *c, uint64_t obj) {
    if (in_heap(obj)) gcall(c, *(const uint64_t *)(uintptr_t)(*(const uint64_t *)(uintptr_t)obj + 0x20), obj, 0, 0, 0);
}
static int ride_model(Cpu *c, uint64_t addr) {  // (the maker's call from a Pokémon's model job: ours made a ride model)
    if (!t_pjob) return 0;
    Mnt *m = NULL;
    pthread_mutex_lock(&g_lock);
    for (int i = 0; i < NMNT && !m; i++)
        if (g_mnt[i].state >= 2 && !g_mnt[i].remodel && g_mnt[i].t > 0 && g_mnt[i].t < 8 && k_ride_mdl[g_mnt[i].t] && (g_mnt[i].chr == t_pjob || g_mnt[i].chr_pre == t_pjob))
            m = &g_mnt[i], m->remodel = 1;
    pthread_mutex_unlock(&g_lock);
    if (!m) return 0;
    static __thread char *str;
    static __thread uint64_t *h;
    static __thread uint8_t *desc;  // (a fresh desc, as the ride object's loader makes one: the maker copies it)
    if (!str) str = hle_alloc(0x200), h = hle_alloc(0x100), desc = hle_alloc(0x300);
    memset(desc, 0, 0x300);
    uint64_t d = (uint64_t)(uintptr_t)desc;
    gcall(c, DESC_CTOR, d, c->x[1], 0, 0);
    const char *b = k_ride_mdl[m->t];
    struct { const char *fmt; uint32_t off; uint64_t add; } f[4] = {
        {"bin/chara/data/pm/%s/mdl/%s.trmdl", 0, DESC_ADD_FILE}, {"bin/chara/data/pm/%s/anm/%s_rd.tracn", 0x38, DESC_ADD_FILE},
        {"bin/chara/data/pm/%s/locators/%s_loc.trskl", 0x70, DESC_ADD_SKEL}, {"bin/chara/data/pm/%s/locators/%s_eff.trskl", 0x70, DESC_ADD_SKEL}};
    // (a model / net entry is 0x70 B: the file's handle (0x30 B, from RES_PATH) then two more unnamed - the empty FNV
    // and an empty string each - as the loader sets them up)
    for (int i = 0; i < 4; i++) {
        if (i == 2 && strcmp(b, "pm1003_00_41")) continue;  // (Sneasler's has a second locator skeleton)
        snprintf(str, 0x100, f[i].fmt, b, b);
        h[0] = (uint64_t)(uintptr_t)str;
        uint64_t *e = &h[2];
        memset(e, 0, 0x70);
        e[1] = EMPTY_HASH, e[2] = EMPTY_STR, e[6] = EMPTY_HASH, e[7] = EMPTY_STR, e[10] = EMPTY_HASH, e[11] = EMPTY_STR;
        gcall(c, RES_PATH, (uint64_t)(uintptr_t)&h[0], 0, 0, (uint64_t)(uintptr_t)&h[0x10]);
        memcpy(e, &h[0x10], 0x30);
        gcall(c, f[i].add, d + f[i].off, (uint64_t)(uintptr_t)e, 0, 0);
        res_release(c, e[4]), res_release(c, e[0]);
    }
    *(uint32_t *)(uintptr_t)(d + 0xe0) = 3;  // (kind: the files listed)
    uint64_t pk = *(const uint64_t *)(uintptr_t)(d + 0x1e8);
    if (in_heap(pk)) *(uint32_t *)(uintptr_t)(pk + 0x68) = 2;
    c->x[3] = d;
    rt_call_original(c, addr);
    uint64_t r = c->x[0];
    gcall(c, DESC_DTOR, d, 0, 0, 0);
    c->x[0] = r;
    LOG("[npc] %s made as the ride Pokemon %s (saddle, riding net): model %llx\n", m->name, b, (unsigned long long)r);
    return 1;
}

static int g_dress = -1;  // (ours wear their player's outfit; research LARECOMP_NPC_DRESS=0: never)
static int create_hook(Cpu *c, uint64_t addr) {
    if (c->x[30] == POKE_MAKER_LR) return ride_model(c, addr);
    if (c->x[30] == PLAYER_MAKER_LR) {
        if (g_player_loader != c->x[1]) LOG("[npc] the player's model loader %llx (desc %llx)\n", (unsigned long long)c->x[1], (unsigned long long)c->x[3]);
        g_player_loader = c->x[1];
        return 0;
    }
    if (c->x[30] != NPC_MODEL_LR || g_dress <= 0) return 0;
    uint64_t d = c->x[3];
    if (!guest_ptr(d)) return 0;
    uint64_t v0 = *(const uint64_t *)(uintptr_t)d, v1 = *(const uint64_t *)(uintptr_t)(d + 8);
    if (!guest_ptr(v0) || v1 <= v0) return 0;
    uint64_t sp = *(const uint64_t *)(uintptr_t)(v0 + 0x10);
    const char *path = guest_ptr(sp) ? (const char *)(uintptr_t)sp : "";
    if (!strstr(path, "tr0036_00_senpai_m.trmdl") && !strstr(path, "tr0037_00_senpai_f.trmdl")) return 0;
    pthread_mutex_lock(&g_lock);
    Own *o = NULL;  // (ours whose model component this job is - known from its making, nav_match)
    for (int i = 0; i < NOWN && !o; i++)
        if (g_own[i].state >= 2 && g_own[i].chr == t_job_chr && t_job_chr && g_own[i].has_outfit && !g_own[i].dressed && !g_own[i].modeled) o = &g_own[i];
    int gender = 0;
    if (o) gender = o->gender, o->dressed = 1, o->modeled = 1;
    pthread_mutex_unlock(&g_lock);
    if (!o) return 0;
    uint64_t alloc = c->x[1];
    static int ldr = -1;  // (research LARECOMP_NPC_DRESSLDR=1: made with the player's loader)
    if (ldr < 0) ldr = getenv("LARECOMP_NPC_DRESSLDR") && atoi(getenv("LARECOMP_NPC_DRESSLDR"));
    if (ldr && g_player_loader) alloc = g_player_loader, c->x[1] = alloc;
    static __thread uint8_t *desc;  // (the player desc: the shared body, the player's motion and locators)
    if (!desc) desc = hle_alloc(0x200);
    memset(desc, 0, 0x200);
    gcall(c, PLAYER_DESC, alloc, (uint64_t)gender, 0, (uint64_t)(uintptr_t)desc);
    static int keep = -1;  // (research LARECOMP_NPC_DRESSKEEP=1: the senior's own model kept, dressed over)
    if (keep < 0) keep = getenv("LARECOMP_NPC_DRESSKEEP") && atoi(getenv("LARECOMP_NPC_DRESSKEEP"));
    if (!keep) c->x[3] = (uint64_t)(uintptr_t)desc;
    rt_call_original(c, addr);
    pthread_mutex_lock(&g_lock);
    o->alloc = alloc, o->model = c->x[0], o->chr = t_job_chr;
    pthread_mutex_unlock(&g_lock);
    LOG("[npc] mpplayer_%d made with the player's body: model %llx (%s)\n", o->k, (unsigned long long)c->x[0], gender ? "p2" : "p1");
    return 1;
}

// (2) + (3) on our person's owner, in its entity callback (the game's own order: the components are made there)
static void dress_owner(Cpu *c, Own *o, uint64_t ent) {
    static __thread uint64_t *t;  // (scratch: result slots, smart pointers; the game keeps its own references)
    if (!t) t = hle_alloc(0x400);
    memset(t, 0, 0x400);
    uint64_t owner = *(const uint64_t *)(uintptr_t)(ent + 0x68), dctx = *(const uint64_t *)(uintptr_t)(ent + 0xb0), al = o->alloc;
    if (!in_heap(owner) || *(const uint64_t *)(uintptr_t)owner != MODEL_OBJ_VTABLE) {
        LOG("[npc] mpplayer_%d: its entity %llx has no owner (%llx) - not dressed\n", o->k, (unsigned long long)ent, (unsigned long long)owner);
        return;
    }
#define T(i) ((uint64_t)(uintptr_t)&t[(i) * 2])
    uint64_t h0 = gcall(c, 0x700125be64ull, owner, al, 0, 0);
    if (in_heap(h0)) {
        gcall(c, 0x7000a17fa0ull, ent, 0, 0, T(0));  // (the entity's +0x70 resource)
        if (in_heap(t[0])) {
            gcall(c, 0x70007802f0ull, T(1), t[0] + 0x58, 0, 0);
            gcall(c, 0x70009b3c84ull, T(2), T(1), 0, 0);
            gcall(c, 0x7000a1b5bcull, h0 + 0x1f0, T(2), 0, 0);
        }
        gcall(c, *(const uint64_t *)(uintptr_t)(*(const uint64_t *)(uintptr_t)h0 + 0xf0), h0, 0, 0, 0);
        gcall(c, 0x70009ae1c0ull, h0, 0, 0, T(3));
        gcall(c, 0x700125c614ull, *(const uint64_t *)(uintptr_t)(h0 + 0x200), T(3), 0, 0);
    }
    uint64_t h1 = gcall(c, 0x700125c050ull, owner, al, 0, 0);
    if (in_heap(h1)) {
        gcall(c, 0x7000a18014ull, ent, 0, 0, T(4));  // (the entity's +0x78 resource)
        if (in_heap(t[8])) {
            gcall(c, 0x70007802f0ull, T(5), t[8] + 0x58, 0, 0);
            gcall(c, 0x70009a03f0ull, T(6), T(5), 0, 0);
            gcall(c, 0x70009eaa8cull, h1 + 0x1f0, T(6), 0, 0);
        }
        gcall(c, *(const uint64_t *)(uintptr_t)(*(const uint64_t *)(uintptr_t)h1 + 0xf0), h1, 0, 0, 0);
        gcall(c, 0x700099ecd4ull, h1, 0, 0, T(7));
        gcall(c, 0x700125cd08ull, *(const uint64_t *)(uintptr_t)(h1 + 0x200), T(7), 0, 0);
    }
    uint64_t h2 = gcall(c, 0x700142f590ull, owner, al, 0, 0);
    if (in_heap(h2)) {
        gcall(c, 0x7000a18098ull, ent, 0, 0, T(11));  // (the entity's third resource: a provider asked through its vfunc +0x10)
        if (in_heap(t[22])) {
            gcall(c, *(const uint64_t *)(uintptr_t)(*(const uint64_t *)(uintptr_t)t[22] + 0x10), t[22], 0, 0, T(12));
            gcall(c, 0x70014300dcull, T(13), T(12), 0, 0);
            gcall(c, 0x700142fad0ull, h2 + 0x1f0, T(13), 0, 0);
        }
        gcall(c, *(const uint64_t *)(uintptr_t)(*(const uint64_t *)(uintptr_t)h2 + 0xf0), h2, 0, 0, 0);
        gcall(c, 0x700142d920ull, h2, 0, 0, T(14));
        gcall(c, 0x700142f8a0ull, *(const uint64_t *)(uintptr_t)(h2 + 0x200), T(14), 0, 0);
    }
    uint8_t *pr = (uint8_t *)&t[0x40];  // (the params, 0x68 B)
    if (in_heap(h0)) gcall(c, 0x70009ae1c0ull, h0, 0, 0, T(8)), gcall(c, 0x70009b196cull, (uint64_t)(uintptr_t)pr, T(8), 0, 0);
    if (in_heap(h1)) gcall(c, 0x700099ecd4ull, h1, 0, 0, T(9)), gcall(c, 0x70009b166cull, (uint64_t)(uintptr_t)(pr + 0x10), T(9), 0, 0);
    if (in_heap(h2)) gcall(c, 0x700142d920ull, h2, 0, 0, T(15)), gcall(c, 0x700142def0ull, (uint64_t)(uintptr_t)(pr + 0x40), T(15), 0, 0);
#undef T
    memcpy(pr + 0x50, &o->outfit[11], 8);
    pr[0x58] = (uint8_t)o->gender;
    uint64_t comp = gcall(c, DRESS_MAKE, owner, dctx, (uint64_t)(uintptr_t)pr, 0);
    o->comp = in_heap(comp) ? comp : 0, o->owner = owner;
    int n = 0;
    for (int i = 0; i < 11 && o->comp; i++) {  // (as the game: items 0-10 put on; item 11 goes in the params)
        if (!o->outfit[i] || o->outfit[i] == EMPTY_HASH) continue;
        t[0x60] = o->outfit[i];
        gcall(c, MODEL_BUILD, comp, (uint64_t)(uintptr_t)&t[0x60], 0, 0);
        n++;
    }
    memcpy(o->worn, o->outfit, sizeof o->worn);
    LOG("[npc] mpplayer_%d dressed: owner %llx, components %llx %llx %llx, dress-up %llx, %d items\n", o->k, (unsigned long long)owner,
        (unsigned long long)h0, (unsigned long long)h1, (unsigned long long)h2, (unsigned long long)comp, n);
}

static int entity_hook(Cpu *c, uint64_t addr) {
    // (ours: x0 the character whose model job (job_hook) made it with the player's body)
    uint64_t ent = guest_ptr(c->x[1]) ? *(const uint64_t *)(uintptr_t)c->x[1] : 0;
    if (in_heap(c->x[0]) && *(const uint64_t *)(uintptr_t)c->x[0] == POKE_MODEL_VTABLE && in_heap(ent)) {  // (a Pokémon's: its owner noted for our mounts)
        bool ours = false;
        for (int i = 0; i < NMNT && !ours; i++) ours = g_mnt[i].state >= 2;
        if (!ours) return 0;
        uint64_t chr = c->x[0];
        rt_call_original(c, addr);
        pthread_mutex_lock(&g_lock);
        g_pent[g_npent % 32].chr = chr, g_pent[g_npent % 32].owner = *(const uint64_t *)(uintptr_t)(ent + 0x68), g_npent++;
        pthread_mutex_unlock(&g_lock);
        return 1;
    }
    Own *o = NULL;
    pthread_mutex_lock(&g_lock);
    for (int i = 0; i < NOWN && !o && in_heap(ent); i++)
        if (g_own[i].dressed == 1 && !g_own[i].comp && g_own[i].chr == c->x[0]) o = &g_own[i];
    if (o) o->dressed = 2;
    pthread_mutex_unlock(&g_lock);
    if (!o) return 0;
    rt_call_original(c, addr);  // (the person's own setup first: then its owner is ready)
    dress_owner(c, o, ent);
    return 1;
}

// Its parts drawn: each mesh component (vtable 0x70041801a0) of a model is drawn only while its LOD controller
// ([comp+0x1f0]: +0x180 on, +0x174 the LOD shown, +0x178 the mask of LODs the model has) has its LOD (comp+0x208) in the
// mask (the update 0x7002997c94 sets each instance's +0x22a from it). The mask comes from the model's own meshes - the
// shared player body has none, so ours stayed 0 and nothing was drawn (the field player's is 1). LOD 0 is put in.
static void lod_show(Own *o) {
    if (o->rhidden) return;  // (put away on purpose: rider_show)
    const uint64_t *b = (const uint64_t *)(uintptr_t)*(const uint64_t *)(uintptr_t)(o->owner + 0xf0 + 0xe8);
    const uint64_t *e = (const uint64_t *)(uintptr_t)*(const uint64_t *)(uintptr_t)(o->owner + 0xf0 + 0xf0);
    if (!in_heap((uint64_t)(uintptr_t)b) || e < b || e - b > 256) return;
    for (const uint64_t *q = b; q < e; q++) {
        uint64_t cp = *q - 0x48;
        if (!in_heap(cp) || *(const uint64_t *)(uintptr_t)cp != 0x70041801a0ull) continue;
        uint64_t l = *(const uint64_t *)(uintptr_t)(cp + 0x1f0);
        if (!in_heap(l)) continue;
        uint64_t *mask = (uint64_t *)(uintptr_t)(l + 0x178);
        if (!*mask) {
            *mask = 1;
            if (!o->lod_set) o->lod_set = 1, LOG("[npc] mpplayer_%d: its LOD controller %llx had no LOD - LOD 0 shown\n", o->k, (unsigned long long)l);
        }
    }
}

// Kept moving on screen. A model owner's byte +0x33c freezes its model: while set, the mesh update 0x7002997c94 leaves
// every instance's world transform (+0xf0 scale, +0x100 rotation, +0x110 position) - and so its bounds (+0x1d0) - as
// they were. The player-model controller our dressed persons carry (vt 0x70041284b8, the field player's kind) sets it
// from 0x7001d1054c each frame its model was not seen in any view the frame before (0x700299d20c; unless its +0x360
// bit 0), and clears it only on its own state changes (0x700173740c) - getting on or off a ride. The field player is
// never off screen; ours are: one that left the view froze there, moved on, and its bounds stayed behind where nobody
// looks - never seen again, never unfrozen (rider invisible until its player got off and on, 2026-11-18). Cleared each
// frame from the camera update (before the mesh update), for persons and mounts.
static void unfreeze(uint64_t owner) {
    if (!in_heap(owner) || *(const uint64_t *)(uintptr_t)owner != MODEL_OBJ_VTABLE) return;
    uint8_t *f = (uint8_t *)(uintptr_t)(owner + 0x33c);
    if (*f) *f = 0;
}

// The animation component's parameters, set by name hash (FNV-1a of the name in the control net's .tracp): the
// setters look the hash up in the component's map of that type (float +0x730, int +0x6c8, bool +0x798, trigger +0x800;
// a name the net doesn't have is ignored). A dressed person runs the player's net (pc0002_00.tracn): it walks and runs
// by move_front (0..1, the stick's push), its root motion moving it forward, as the player's does.
#define ANIM_SET_FLOAT 0x7001950188ull
#define ANIM_SET_INT 0x7001951334ull
#define ANIM_SET_BOOL 0x700195bd74ull
#define ANIM_SET_TRIGGER 0x700194f4d4ull
static uint64_t *g_hash_slot;  // (guest memory for the hash argument)
#define POKE_ANIM_VTABLE 0x7004147c18ull  // (a Pokémon's animation component: the same setters)
static void anim_set(Cpu *c, uint64_t actl, uint64_t fn, uint64_t hash, float f, uint32_t i) {
    if (!in_heap(actl)) return;
    uint64_t av = *(const uint64_t *)(uintptr_t)actl;
    if (av != ANIM_VTABLE && av != POKE_ANIM_VTABLE) return;
    if (!g_hash_slot) g_hash_slot = hle_alloc(16);
    g_hash_slot[0] = hash;
    uint64_t sx[32], snzcv = c->nzcv;
    __uint128_t sv[32];
    memcpy(sx, c->x, sizeof sx), memcpy(sv, c->v, sizeof sv);
    c->x[0] = actl, c->x[1] = (uint64_t)(uintptr_t)g_hash_slot, c->x[2] = i;
    memset(&c->v[0], 0, 16), memcpy(&c->v[0], &f, 4);
    guest_call(c, fn);
    memcpy(c->x, sx, sizeof sx), memcpy(c->v, sv, sizeof sv), c->nzcv = snzcv;
}
// research: <live>/npc "par <slot> <F|I|B|T> <name> <value>" - a parameter of that person's animation set once
// (also the other player's own state, mirrored: pla_npc_mirror below)
#define NPAR 32
static struct { int slot; char type; uint64_t hash; float f; uint32_t i; int quiet; } g_par[NPAR];
static volatile int g_npar;
static void par_run(Cpu *c) {
    pthread_mutex_lock(&g_lock);
    for (int k = 0; k < g_npar; k++) {
        if (g_par[k].slot < 0 || g_par[k].slot >= NOWN) continue;
        uint64_t a = g_own[g_par[k].slot].actl;
        uint64_t fn = g_par[k].type == 'F' ? ANIM_SET_FLOAT : g_par[k].type == 'I' ? ANIM_SET_INT : g_par[k].type == 'B' ? ANIM_SET_BOOL : ANIM_SET_TRIGGER;
        anim_set(c, a, fn, g_par[k].hash, g_par[k].f, g_par[k].i);
        static int nl;
        if (!g_par[k].quiet || nl++ < 300)
            LOG("[npc] mpplayer_%d: parameter %c %016llx = %g / %u (animation %llx)\n", g_own[g_par[k].slot].k, g_par[k].type, (unsigned long long)g_par[k].hash,
                g_par[k].f, g_par[k].i, (unsigned long long)a);
    }
    g_npar = 0;
    pthread_mutex_unlock(&g_lock);
}

// ---- the other player's state on theirs: a roll (dodge - the field player has no jump of its own), crouching,
// swimming, riding, falling and landing. Those are parameters of the field player's animation (its component, vtable
// 0x7004147468, set through the same setters as the people's: Lua-traced 2026-11-18 - a dodge sets rolling_direction_int,
// then rolling_triger and upperbody_one_action_trigger), and the person runs the same net (pc0002_00.tracn): ours are
// watched as the game sets them (k_mirror) and sent ('S1', hle/pla_world.c); theirs are set on their person, at the
// moment they are drawn. A trigger travels as a count (one more each time it fires). Verified 2026-11-18 (single copy,
// LARECOMP_GHOST_DELAY): a dodge roll (testout/pla-mp7 st4-roll-strip.png), crouching (fl-crouch*).
#define PLAYER_ANIM_VTABLE 0x7004147468ull
static const char *const k_mirror[] = {
    "rolling_direction_int", "set_squat_bool", "swim_bool", "swim_action_int", "dive_bool", "set_ride_bool", "ride_type_int",
    "ride_action_bool", "ride_action_int", "ride_dive_bool", "aerial", "aerial_type_int", "landing_type_int", "down_bool",
    "slipdown_bool", "slipdown_int", "rolling_triger", "upperbody_one_action_trigger", "double_jump_trigger",
};
#define NMIRROR ((int)(sizeof k_mirror / sizeof k_mirror[0]))
static uint64_t g_mirror_hash[NMIRROR];
static volatile uint32_t g_my_state[NMIRROR];
static volatile char g_my_type[NMIRROR];  // (0: never set)
static volatile uint32_t g_my_state_seq;   // (one more on each change)
static int mirror_index(uint64_t h) {
    for (int k = 0; k < NMIRROR; k++)
        if (g_mirror_hash[k] == h) return k;
    return -1;
}
// ... and our ride Pokémon's: its net (bin/chara/data/pm/<m>/anm/<m>_rd.tracn) runs on an animation component of its own
// (vtable 0x7004165430), set by the game next to the player's - move_front for its gait, swim_bool, the climbing
// parameters (Sneasler), aerial / ride_fly_away_bool / ride_dive_bool (Braviary), ride_found_* (Ursaluna)... Sent in
// 'S1' packets too (indices from NMIRROR on), set on our mount of the same model (ride_step). Lua-traced 2026-11-18.
// (each ride Pokémon's component is a class of its own: Wyrdeer 0x7004165430, Ursaluna 0x7004166848, Braviary 0x70041679c8,
// Sneasler 0x7004163af0; Basculegion one of the two other siblings sharing their slots)
static bool ride_anim_vt(uint64_t vt) {
    return vt == 0x7004165430ull || vt == 0x7004166848ull || vt == 0x70041679c8ull || vt == 0x7004163af0ull || vt == 0x7004161cd8ull || vt == 0x70041621f0ull;
}
static const char *const k_rmirror[] = {
    "move_front", "move_left", "set_ride_bool", "ride_type_int", "swim_bool", "set_climb_bool", "is_clime_bool",
    "is_clime_jump_bool", "climb_move_speed", "movec_climb_left", "movec_climb_up", "climb_aerial", "climb_aerial_type",
    "climb_start_end_int", "aerial", "aerial_type_int", "landing_type_int", "ride_dash_bool", "ride_fly_away_bool",
    "ride_dive_bool", "ride_found_bool", "ride_found_int", "ride_action_bool", "ride_action_int", "ride_halfway_down_bool",
    "ride_move_stop_int", "slipdown_bool", "slipdown_int", "down_bool", "set_squat_bool", "slope", "direction",
    "move_stop_trigger", "ride_one_action_trigger", "double_jump_trigger", "turn_trigger", "rolling_triger",
    "ride_addturn_trigger", "ride_addturn_int",
};
#define NRMIRROR ((int)(sizeof k_rmirror / sizeof k_rmirror[0]))
static uint64_t g_rmirror_hash[NRMIRROR];
static volatile uint32_t g_my_rstate[NRMIRROR];
static volatile char g_my_rtype[NRMIRROR];
static volatile uint64_t g_my_rdirty;  // (changed since sent: bit per entry)
static int rmirror_index(uint64_t h) {
    for (int k = 0; k < NRMIRROR; k++)
        if (g_rmirror_hash[k] == h) return k;
    return -1;
}
static void my_rset(Cpu *c, char type) {
    int k = rmirror_index(*(const uint64_t *)(uintptr_t)c->x[1]);
    if (k < 0) return;
    uint32_t v;
    if (type == 'T') v = g_my_rstate[k] + 1;
    else if (type == 'F') memcpy(&v, &c->v[0], 4);
    else v = (uint32_t)c->x[2];
    if (type == 'B') v &= 1;
    if (g_my_rtype[k] == type && g_my_rstate[k] == v) return;
    g_my_rstate[k] = v, g_my_rtype[k] = type, g_my_rdirty |= 1ull << k;
}
// ours, for 'S1' packets: the entries changed since last asked (then, with full, the rest in turn) - indices + NMIRROR
int pla_npc_my_rstate(uint8_t *idx, char *type, uint32_t *val, int max, int full);
static void my_set(Cpu *c, char type) {
    uint64_t a = c->x[0];
    if (in_heap(a) && ride_anim_vt(*(const uint64_t *)(uintptr_t)a) && in_heap(c->x[1])) { my_rset(c, type); return; }
    if (!in_heap(a) || *(const uint64_t *)(uintptr_t)a != PLAYER_ANIM_VTABLE || !in_heap(c->x[1])) return;
    int k = mirror_index(*(const uint64_t *)(uintptr_t)c->x[1]);
    if (k < 0) return;
    uint32_t v;
    if (type == 'T') v = g_my_state[k] + 1;
    else if (type == 'F') memcpy(&v, &c->v[0], 4);
    else v = (uint32_t)c->x[2];
    if (type == 'B') v &= 1;
    if (g_my_type[k] == type && g_my_state[k] == v) return;
    g_my_state[k] = v, g_my_type[k] = type, g_my_state_seq++;
}
// (crouching and swimming don't go through those parameters: the field player's character controller (vtable
// 0x700413db58 - the people's base class 0x7004139ab0) is told Crouch(cc, on) (vfunc +0x1c8, its own 0x7001a0d110: kept
// at +0x910 unless refused) and Swim(cc, on) (vfunc +0x210, its own 0x7001a16cf4: +0x918 1 swimming, 2 drowning) - what
// they leave is sent as the net's set_squat_bool / swim_bool, which crouch / swim a person running it)
#define PLAYER_CC_VTABLE 0x700413db58ull
#define PLAYER_CC_CROUCH 0x7001a0d110ull
#define PLAYER_CC_SWIM 0x7001a16cf4ull
static void my_flag(const char *name, uint32_t v) {
    int k = mirror_index(fnv(name));
    if (k < 0 || (g_my_type[k] == 'B' && g_my_state[k] == v)) return;
    g_my_state[k] = v, g_my_type[k] = 'B', g_my_state_seq++;
}
static int crouch_hook(Cpu *c, uint64_t addr) {
    uint64_t cc = c->x[0];
    rt_call_original(c, addr);
    if (in_heap(cc) && *(const uint64_t *)(uintptr_t)cc == PLAYER_CC_VTABLE) my_flag("set_squat_bool", *(const uint8_t *)(uintptr_t)(cc + 0x910) != 0);
    return 1;
}
static int swim_hook(Cpu *c, uint64_t addr) {
    uint64_t cc = c->x[0];
    rt_call_original(c, addr);
    if (in_heap(cc) && *(const uint64_t *)(uintptr_t)cc == PLAYER_CC_VTABLE) my_flag("swim_bool", *(const uint32_t *)(uintptr_t)(cc + 0x918) == 1);
    return 1;
}
static int myset_f(Cpu *c, uint64_t addr) { (void)addr; my_set(c, 'F'); return 0; }
static int myset_i(Cpu *c, uint64_t addr) { (void)addr; my_set(c, 'I'); return 0; }
static int myset_b(Cpu *c, uint64_t addr) { (void)addr; my_set(c, 'B'); return 0; }
static int myset_t(Cpu *c, uint64_t addr) { (void)addr; my_set(c, 'T'); return 0; }
// ours, for the packet: the entries set so far (index, type, value); answers the count, *seq the change count
int pla_npc_my_state(uint8_t *idx, char *type, uint32_t *val, int max, uint32_t *seq) {
    int n = 0;
    for (int k = 0; k < NMIRROR && n < max; k++)
        if (g_my_type[k]) idx[n] = (uint8_t)k, type[n] = g_my_type[k], val[n] = g_my_state[k], n++;  // (once set: a value back to 0 is sent too)
    if (seq) *seq = g_my_state_seq;
    return n;
}
int pla_npc_my_rstate(uint8_t *idx, char *type, uint32_t *val, int max, int full) {
    static int next;
    int n = 0;
    uint64_t d = g_my_rdirty, took = 0;
    for (int k = 0; k < NRMIRROR && n < max; k++)
        if ((d >> k & 1) && g_my_rtype[k]) idx[n] = (uint8_t)(NMIRROR + k), type[n] = g_my_rtype[k], val[n] = g_my_rstate[k], n++, took |= 1ull << k;
    __atomic_and_fetch(&g_my_rdirty, ~took, __ATOMIC_RELAXED);
    for (int j = 0; full && j < NRMIRROR && n < max; j++) {
        int k = (next + j) % NRMIRROR;
        if (!g_my_rtype[k] || (took >> k & 1)) continue;
        idx[n] = (uint8_t)(NMIRROR + k), type[n] = g_my_rtype[k], val[n] = g_my_rstate[k], n++, next = k + 1;
    }
    return n;
}
// riding Sneasler (5) or Basculegion (2) - attached at their "origin": our model owner's world axes are theirs - our
// ride's up (owner +0x80) and forward (+0x90), for the packet (on a wall Sneasler isn't upright)
int pla_npc_my_axes(float ax[6]) {
    int kr = mirror_index(fnv("set_ride_bool")), kt = mirror_index(fnv("ride_type_int"));
    if (kr < 0 || kt < 0 || !g_my_state[kr] || (g_my_state[kt] != 2 && g_my_state[kt] != 5)) return 0;
    uint64_t ow = g_player_owner;
    if (!in_heap(ow) || *(const uint64_t *)(uintptr_t)ow != MODEL_OBJ_VTABLE) return 0;
    memcpy(ax, (const float *)(uintptr_t)(ow + 0x80), 12), memcpy(ax + 3, (const float *)(uintptr_t)(ow + 0x90), 12);
    for (int j = 0; j < 6; j++)
        if (!isfinite(ax[j]) || fabsf(ax[j]) > 1.01f) return 0;
    return 1;
}
void pla_npc_follow_axes(int i, const float ax[6], int ok) {
    if (i < 0 || i >= NOWN) return;
    pthread_mutex_lock(&g_lock);
    g_own[i].ax_ok = ok;
    if (ok) memcpy(g_own[i].ax, ax, 24);
    pthread_mutex_unlock(&g_lock);
}
int pla_npc_my_riding(void) {  // (we ride now: our set_ride_bool)
    int k = mirror_index(fnv("set_ride_bool"));
    return k >= 0 && g_my_type[k] == 'B' && g_my_state[k];
}
// theirs, on their person i (a trigger: fired; the rest: set)
void pla_npc_mirror(int i, int k, char type, uint32_t val) {
    if (i >= 0 && i < NOWN && k >= NMIRROR && k < NMIRROR + NRMIRROR) {  // (their ride Pokémon's: for our mount - ride_step)
        Own *o = &g_own[i];
        int r = k - NMIRROR;
        pthread_mutex_lock(&g_lock);
        if (type == 'T') o->rp_fire |= 1ull << r;
        o->rp_type[r] = type, o->rp_val[r] = val, o->rp_dirty |= 1ull << r;
        pthread_mutex_unlock(&g_lock);
        return;
    }
    if (i < 0 || i >= NOWN || k < 0 || k >= NMIRROR || !g_own[i].has_outfit) return;  // (the senior's net has none of these)
    // (riding: noted, set on the person by ride_step once its mount is there - without one, the riding pose has it lying
    // on the ground)
    if (!strcmp(k_mirror[k], "set_ride_bool")) { g_own[i].riding = val != 0; return; }
    if (!strcmp(k_mirror[k], "ride_type_int")) { g_own[i].ride_type = (int)val; return; }
    if (!strcmp(k_mirror[k], "swim_bool")) g_own[i].swimming = val != 0;  // (and set: the net's swimming)
    pthread_mutex_lock(&g_lock);
    if (g_npar < NPAR) {
        g_par[g_npar].slot = i, g_par[g_npar].type = type, g_par[g_npar].hash = g_mirror_hash[k], g_par[g_npar].i = val, g_par[g_npar].quiet = 1;
        if (type == 'F') memcpy(&g_par[g_npar].f, &val, 4);
        else g_par[g_npar].f = (float)val;
        g_npar++;
    }
    pthread_mutex_unlock(&g_lock);
}
const char *pla_npc_mirror_name(int k) { return k >= 0 && k < NMIRROR ? k_mirror[k] : k >= NMIRROR && k < NMIRROR + NRMIRROR ? k_rmirror[k - NMIRROR] : "?"; }
int pla_npc_mirror_count(void) { return NMIRROR + NRMIRROR; }

// ---- a dressed person walked like a player: its forward direction and its stick (move_front) set every frame
// It is steered as the player is: its forward direction and how far the stick is pushed (move_front 0..1 - the net
// walks, then runs). The net's root motion lands in its model's local translation (owner +0xd0: the field player's own
// code takes it from there) - so each frame that is taken (zeroed) and given to its physics character (vtable
// 0x700413cad8, the navigator's +0x1f0) as its velocity (SetVelocity, vfunc +0x100 0x7001982398: it moves as far as its
// feet say, against the world's collision); left alone, the model ran off from a physics character standing still.
// The same character's SetForwardDirection 0x70019853a8(cc, &vec) turns it (x, z taken; at once - eased here) and its
// Warp 0x7001983770(cc, &pos, &quat) puts it somewhere. These three are the game's Haxe-bound CharacterController
// methods (table built in 0x7001fda8ec; Lua-traced 2026-11-18). The speed move_front gives (30 fps, flat ground): 0.1
// 0.25 m/s, 0.2 0.52, 0.3 0.82, 0.4 1.12, 0.5 1.45, 0.6 1.83, 0.7 2.30, 0.8 2.90, 0.9 3.71, 1.0 4.84. Where it is: its
// owner's world translation (+0xa0); which way it faces: the owner's z axis (+0x90).
#define CC_SET_FORWARD 0x70019853a8ull
#define CC_WARP 0x7001983770ull
#define MOVE_FRONT 0x8416e8de8ea9a1ecull  // (FNV-1a "move_front")
#define CC_SET_VELOCITY 0x7001982398ull  // (vfunc +0x100: x1 = &vec4, kept at +0x6b0, +0x6a0 = 1)
static const float k_mf_speed[11] = {0, 0.25f, 0.52f, 0.82f, 1.12f, 1.45f, 1.83f, 2.30f, 2.90f, 3.71f, 4.84f};
static float mf_for(float v) {  // (the move_front that walks / runs at v m/s)
    if (v <= 0) return 0;
    for (int i = 1; i < 11; i++)
        if (v <= k_mf_speed[i]) return (i - 1 + (v - k_mf_speed[i - 1]) / (k_mf_speed[i] - k_mf_speed[i - 1])) * 0.1f;
    return 1.0f;
}
static bool puppet_ready(const Own *o) {
    return o->comp && in_heap(o->owner) && *(const uint64_t *)(uintptr_t)o->owner == MODEL_OBJ_VTABLE && o->actl &&
           *(const uint64_t *)(uintptr_t)o->actl == ANIM_VTABLE && o->ctrl && *(const uint64_t *)(uintptr_t)o->ctrl == CTRL_VTABLE && o->cc &&
           *(const uint64_t *)(uintptr_t)o->cc == CC_VTABLE;
}
static float *g_fwd_vec, *g_vel_vec, *g_warp_vec, *g_vel2;
static void cc_warp(Cpu *c, uint64_t cc, const float at[3], float yaw) {
    static float *w;
    if (!w) w = hle_alloc(32);
    w[0] = at[0], w[1] = at[1], w[2] = at[2], w[3] = 0, w[4] = 0, w[5] = sinf(yaw * 0.5f), w[6] = 0, w[7] = cosf(yaw * 0.5f);
    guest_call_saved(c, CC_WARP, cc, (uint64_t)(uintptr_t)w, (uint64_t)(uintptr_t)(w + 4), 0, 0);
}

// ---- riding: their ride Pokémon is a Pokémon of ours (made as event scripts make one: poke_make) walked where they are,
// the person sat on it. The game mounts its player by attaching the player's model owner to a joint of the ride
// Pokémon's model (see attach_to - doing that to ours aborted: their owner already hangs off their character, and the
// Pokémon of ours has no attach component of its own); ours is placed the same way by hand instead: the person's model
// owner hangs off its character (owner world = character x owner local, +0xc0 rotation, +0xd0 translation), so its
// character stands where the mount stands and its local transform lifts it onto the saddle - turned as the joint is
// (the riding pose of the player's net is made for the joint's frame: y forward, z down). The mount is warped where
// they are each frame, its own navigator kept still, its root motion taken out, its "move" set from their speed so
// its legs walk / run. Verified 2026-11-18, two copies through lzaserver: testout/pla-mp8 x4b-*.
// ride_type_int: 1 Wyrdeer (joint "parts": its frame y forward, z down; the player's owner 1.2 m over the ground, 0.15 m
// back), 2 Basculegion (joint "origin", its body at the water's surface - ours stands there in its on-land idle, not
// swimming), 5 Sneasler (joint "origin" = its model's root: upright, at its feet - the player rides on its back);
// measured riding (LARECOMP_NPC_RIDELOG, Lua plpar, "near") 2026-11-18. 3 / 4 (Ursaluna "parts_01", Braviary
// "parts_03") not seen yet: no mount - the person runs where they are.
// quaternions (x y z w) for the tilt of a climber / swimmer: from a frame's axes (world x, y, z of its local axes)
static void q_from_axes(float q[4], const float *x, const float *y, const float *z) {
    float tr = x[0] + y[1] + z[2];
    if (tr > 0) { float s = sqrtf(tr + 1) * 2; q[3] = 0.25f * s, q[0] = (y[2] - z[1]) / s, q[1] = (z[0] - x[2]) / s, q[2] = (x[1] - y[0]) / s; }
    else if (x[0] > y[1] && x[0] > z[2]) { float s = sqrtf(1 + x[0] - y[1] - z[2]) * 2; q[3] = (y[2] - z[1]) / s, q[0] = 0.25f * s, q[1] = (y[0] + x[1]) / s, q[2] = (z[0] + x[2]) / s; }
    else if (y[1] > z[2]) { float s = sqrtf(1 + y[1] - x[0] - z[2]) * 2; q[3] = (z[0] - x[2]) / s, q[0] = (y[0] + x[1]) / s, q[1] = 0.25f * s, q[2] = (z[1] + y[2]) / s; }
    else { float s = sqrtf(1 + z[2] - x[0] - y[1]) * 2; q[3] = (x[1] - y[0]) / s, q[0] = (z[0] + x[2]) / s, q[1] = (z[1] + y[2]) / s, q[2] = 0.25f * s; }
    float n = sqrtf(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
    for (int i = 0; i < 4; i++) q[i] = n > 1e-6f ? q[i] / n : (i == 3);
}
static void q_mul(float r[4], const float a[4], const float b[4]) {
    float t[4] = {a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1], a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0],
                  a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3], a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2]};
    memcpy(r, t, 16);
}
static void q_conj(float r[4], const float a[4]) { r[0] = -a[0], r[1] = -a[1], r[2] = -a[2], r[3] = a[3]; }
static void q_rot(float r[3], const float q[4], const float v[3]) {  // (v turned by q)
    float p[4] = {v[0], v[1], v[2], 0}, c[4], t[4];
    q_conj(c, q), q_mul(t, q, p), q_mul(t, t, c);
    r[0] = t[0], r[1] = t[1], r[2] = t[2];
}
// a frame from an up and a forward (forward made square to up)
static void q_from_upfwd(float q[4], const float up[3], const float fw[3]) {
    float y[3] = {up[0], up[1], up[2]}, z[3], x[3];
    float n = sqrtf(y[0] * y[0] + y[1] * y[1] + y[2] * y[2]);
    for (int i = 0; i < 3; i++) y[i] /= n > 1e-6f ? n : 1;
    x[0] = y[1] * fw[2] - y[2] * fw[1], x[1] = y[2] * fw[0] - y[0] * fw[2], x[2] = y[0] * fw[1] - y[1] * fw[0];
    n = sqrtf(x[0] * x[0] + x[1] * x[1] + x[2] * x[2]);
    if (n < 1e-6f) { q[0] = q[1] = q[2] = 0, q[3] = 1; return; }
    for (int i = 0; i < 3; i++) x[i] /= n;
    z[0] = x[1] * y[2] - x[2] * y[1], z[1] = x[2] * y[0] - x[0] * y[2], z[2] = x[0] * y[1] - x[1] * y[0];
    q_from_axes(q, x, y, z);
}
// the character's own rotation under an owner: its world rotation less the local one written last frame
static void q_char(float r[4], uint64_t owner, const float last[4]) {
    float w[4], c[4], l[4] = {last[0], last[1], last[2], last[3]};
    if (l[0] == 0 && l[1] == 0 && l[2] == 0 && l[3] == 0) l[3] = 1;
    q_from_axes(w, (const float *)(uintptr_t)(owner + 0x70), (const float *)(uintptr_t)(owner + 0x80), (const float *)(uintptr_t)(owner + 0x90));
    q_conj(c, l), q_mul(r, w, c);
}

// the joint of the ride model the game hangs its player on (app_config ride_*_config.bin "player_attach_joint_name")
static const char *const k_ride_joint[8] = {NULL, "parts", "origin", "parts_01", "parts_03", "origin", NULL, NULL};
static struct { int mons; float seat_y, seat_z, turn_x, mount_y; } k_ride[8] = {  // (research: live "seat <type> <mons> <y> <z> <turn> [mount y]")
    {0, 0, 0, 0, 0}, {899, 1.20f, -0.15f, 1.5707963f, 0}, {902, 0, 0, 0, 0}, {901, 1.20f, 0, 1.5707963f, 0}, {628, 1.20f, 0, 1.5707963f, 0}, {903, 0, 0, 0, 0}, {0, 0, 0, 0, 0}, {0, 0, 0, 0, 0},
};
#define POKE_MOVE 0xd5eb2f99669eefd4ull  // (FNV-1a "move": a Pokémon's walk / run blend, 0 .. ~2)
static Mnt *ride_mount(Own *o) {
    int t = o->ride_type & 7, i = o->mnt[t] - 1;
    return i >= 0 && i < NMNT && g_mnt[i].state && g_mnt[i].level == g_level ? &g_mnt[i] : NULL;
}
// a mount shown / hidden: every mesh's LOD mask (see lod_show) kept aside and zeroed - nothing drawn - or put back
static void mount_show(Mnt *m, int on) {
    if (!m->owner || on == !m->hidden) return;
    const uint64_t *b = (const uint64_t *)(uintptr_t)*(const uint64_t *)(uintptr_t)(m->owner + 0xf0 + 0xe8);
    const uint64_t *e = (const uint64_t *)(uintptr_t)*(const uint64_t *)(uintptr_t)(m->owner + 0xf0 + 0xf0);
    if (!in_heap((uint64_t)(uintptr_t)b) || e < b || e - b > 256) return;
    int n = 0;
    for (const uint64_t *q = b; q < e && n < 16; q++) {
        uint64_t cp = *q - 0x48;
        if (!in_heap(cp) || *(const uint64_t *)(uintptr_t)cp != 0x70041801a0ull) continue;
        uint64_t l = *(const uint64_t *)(uintptr_t)(cp + 0x1f0);
        if (!in_heap(l)) continue;
        uint64_t *mask = (uint64_t *)(uintptr_t)(l + 0x178);
        if (on) *mask = m->lods[n];
        else m->lods[n] = *mask, *mask = 0;
        n++;
    }
    m->hidden = !on;
}
static void mount_park(Cpu *c, Mnt *m, const float at[3]) {  // (not ridden: hidden, out of the way under the ground)
    if (!m->cc || m->state != 3) return;
    float p[3] = {at[0], at[1] - 30.0f, at[2]};
    cc_warp(c, m->cc, p, 0);
    mount_show(m, 0);
}
// The seat, as the game seats its own player (2026-11-18, Lua: the riding player's model owner world = the ride
// Pokémon's owner world x this joint's matrix, exactly, its local 0): the ride model's skeleton (vt 0x7004185640) is
// [instance +0xe8] of its first mesh; joint names (FNV-1a, the game's basis) are in the unordered maps of its parts
// (vector +0x58 of {map, _, u16 first joint +0xa}; map +0x60 buckets, +0x68 count; node +0x10 key, +0x30 index); a
// joint's matrix (model space, columns x y z position, 64 B) = [sk+0x2a8] + 64 x [sk+0xc8][index] (as 0x7002a6962c
// reads it - kept posed: its dirty bit in [sk+0x138] not set for these).
#define SKEL_VTABLE 0x7004185640ull
static bool mount_joint(Mnt *m, int t, float out_q[4], float out_p[3]) {
    if (!m->owner || !k_ride_joint[t & 7]) return false;
    const uint64_t *b = (const uint64_t *)(uintptr_t)*(const uint64_t *)(uintptr_t)(m->owner + 0xf0 + 0xe8);
    const uint64_t *e = (const uint64_t *)(uintptr_t)*(const uint64_t *)(uintptr_t)(m->owner + 0xf0 + 0xf0);
    if (!in_heap((uint64_t)(uintptr_t)b) || e < b || e - b > 256) return false;
    uint64_t sk = 0;
    for (const uint64_t *q = b; q < e && !sk; q++) {
        uint64_t cp = *q - 0x48;
        if (!in_heap(cp) || *(const uint64_t *)(uintptr_t)cp != 0x70041801a0ull) continue;
        uint64_t md = *(const uint64_t *)(uintptr_t)(cp + 0x200);
        if (!in_heap(md)) continue;
        uint64_t ib = *(const uint64_t *)(uintptr_t)(md + 0x90), ie = *(const uint64_t *)(uintptr_t)(md + 0x98);
        if (!in_heap(ib) || ie <= ib) continue;
        uint64_t ins = *(const uint64_t *)(uintptr_t)ib;
        if (in_heap(ins)) sk = *(const uint64_t *)(uintptr_t)(ins + 0xe8);
    }
    if (!in_heap(sk) || *(const uint64_t *)(uintptr_t)sk != SKEL_VTABLE) return false;
    if (m->jsk != sk) {  // (looked up once per skeleton)
        m->jsk = sk, m->jidx = -1;
        uint64_t h = fnv(k_ride_joint[t & 7]);
        uint64_t vb = *(const uint64_t *)(uintptr_t)(sk + 0x58), ve = *(const uint64_t *)(uintptr_t)(sk + 0x60);
        for (uint64_t v = vb; in_heap(vb) && v + 16 <= ve && v - vb < 16 * 16 && m->jidx < 0; v += 16) {
            uint64_t map = *(const uint64_t *)(uintptr_t)v;
            if (!in_heap(map)) continue;
            uint64_t bk = *(const uint64_t *)(uintptr_t)(map + 0x60), nb = *(const uint64_t *)(uintptr_t)(map + 0x68);
            if (!in_heap(bk) || nb > 4096) continue;
            for (uint64_t i = 0; i < nb && m->jidx < 0; i++) {
                uint64_t n = *(const uint64_t *)(uintptr_t)(bk + 8 * i);
                if (!in_heap(n)) continue;
                n = *(const uint64_t *)(uintptr_t)n;
                for (int k = 0; in_heap(n) && k < 256; k++, n = *(const uint64_t *)(uintptr_t)n)
                    if (*(const uint64_t *)(uintptr_t)(n + 0x10) == h) {
                        int g = *(const uint16_t *)(uintptr_t)(n + 0x30) + *(const uint16_t *)(uintptr_t)(v + 0xa);
                        uint64_t remap = *(const uint64_t *)(uintptr_t)(sk + 0xc8);
                        if (in_heap(remap)) m->jidx = *(const uint16_t *)(uintptr_t)(remap + 2 * (uint64_t)g) + 1;
                        break;
                    }
            }
        }
        LOG("[npc] %s: seat joint %s %s (skeleton %llx)\n", m->name, k_ride_joint[t & 7], m->jidx > 0 ? "found" : "NOT found", (unsigned long long)sk);
    }
    if (m->jidx <= 0) return false;
    uint64_t mt = *(const uint64_t *)(uintptr_t)(sk + 0x2a8);
    if (!in_heap(mt)) return false;
    const float *j = (const float *)(uintptr_t)(mt + 64 * (uint64_t)(m->jidx - 1));
    float jq[4], wq[4], jp[3];
    q_from_axes(jq, j, j + 4, j + 8);
    q_from_axes(wq, (const float *)(uintptr_t)(m->owner + 0x70), (const float *)(uintptr_t)(m->owner + 0x80), (const float *)(uintptr_t)(m->owner + 0x90));
    q_mul(out_q, wq, jq);
    q_rot(jp, wq, j + 12);
    const float *mp = (const float *)(uintptr_t)(m->owner + 0xa0);
    for (int i = 0; i < 3; i++) out_p[i] = mp[i] + jp[i];
    return isfinite(out_p[0]) && isfinite(out_p[1]) && isfinite(out_p[2]) && isfinite(out_q[3]);
}

// The rider's world, as drawn. Writing its owner's local each frame left it trailing its mount by up to ~0.5 m at a
// gallop (measured 2026-11-18 from the drawn instances: the net's root motion lands in that local after we write it,
// and the owner's world is computed again mid-frame). So its owner's world matrix is answered directly: the owner
// world getter (vt 0x700417dc80 +0x40 = 0x7002952bd8, x0 owner, returns the matrix in q0..q3 - columns x y z axes and
// position, cached at +0x70..+0xa0) gives, for a seated rider, its mount's world (that getter, called now) x the seat
// joint - what the mesh update draws, the same moment its mount is drawn.
#define OWNER_WORLD_FN 0x7002952bd8ull
typedef struct { volatile uint64_t owner; uint64_t mowner, sk; int jidx; } Seat;
static Seat g_seat[NOWN];
static void seat_set(int i, uint64_t owner, const Mnt *m) {
    if (owner && m && m->owner && m->jsk && m->jidx > 0) {
        if (g_seat[i].owner == owner && g_seat[i].mowner == m->owner && g_seat[i].sk == m->jsk && g_seat[i].jidx == m->jidx) return;
        __atomic_store_n(&g_seat[i].owner, 0, __ATOMIC_RELEASE);
        g_seat[i].mowner = m->owner, g_seat[i].sk = m->jsk, g_seat[i].jidx = m->jidx;
        __atomic_store_n(&g_seat[i].owner, owner, __ATOMIC_RELEASE);
    } else if (g_seat[i].owner) __atomic_store_n(&g_seat[i].owner, 0, __ATOMIC_RELEASE);
}
static int world_hook(Cpu *c, uint64_t addr) {
    uint64_t ow = c->x[0];
    int i = 0;
    for (; i < NOWN; i++)
        if (__atomic_load_n(&g_seat[i].owner, __ATOMIC_ACQUIRE) == ow) break;
    if (i == NOWN || !ow) return 0;
    uint64_t mo = g_seat[i].mowner, sk = g_seat[i].sk;
    int ji = g_seat[i].jidx;
    rt_call_original(c, addr);  // (its own, as the game has it: dirty flags cleared, its cache filled)
    if (!in_heap(mo) || *(const uint64_t *)(uintptr_t)mo != MODEL_OBJ_VTABLE || !in_heap(sk) || *(const uint64_t *)(uintptr_t)sk != SKEL_VTABLE || ji <= 0) return 1;
    uint64_t jm = *(const uint64_t *)(uintptr_t)(sk + 0x2a8);
    if (!in_heap(jm)) return 1;
    uint64_t sx[32], snzcv = c->nzcv;
    __uint128_t sv[32];
    memcpy(sx, c->x, sizeof sx), memcpy(sv, c->v, sizeof sv);
    c->x[0] = mo;
    guest_call(c, addr);  // (its mount's world now)
    float m[4][4], j[4][4], r[4][4];
    for (int k = 0; k < 4; k++) memcpy(m[k], &c->v[k], 16);
    memcpy(c->x, sx, sizeof sx), memcpy(c->v, sv, sizeof sv), c->nzcv = snzcv;
    memcpy(j, (const void *)(uintptr_t)(jm + 64 * (uint64_t)(ji - 1)), 64);
    for (int k = 0; k < 4; k++) {
        memcpy(r[k], &c->v[k], 16);  // (w lanes as its own)
        for (int a = 0; a < 3; a++) r[k][a] = m[0][a] * j[k][0] + m[1][a] * j[k][1] + m[2][a] * j[k][2] + (k == 3 ? m[3][a] : 0);
    }
    if (!isfinite(r[3][0]) || !isfinite(r[3][1]) || !isfinite(r[3][2])) return 1;
    for (int k = 0; k < 4; k++) memcpy(&c->v[k], r[k], 16), memcpy((void *)(uintptr_t)(ow + 0x70 + 16 * (uint64_t)k), r[k], 16);
    return 1;
}

// The rider drawn or not: the game hides its own player while it rides Sneasler (in the basket - its meshes' LOD
// controller +0x180 off from getting on till off, measured 2026-11-18; Wyrdeer / Braviary / Ursaluna on). Ours the
// same, by its meshes' LOD masks (see lod_show / mount_show).
static void rider_show(Own *o, int on) {
    if (!o->owner || on == !o->rhidden) return;
    const uint64_t *b = (const uint64_t *)(uintptr_t)*(const uint64_t *)(uintptr_t)(o->owner + 0xf0 + 0xe8);
    const uint64_t *e = (const uint64_t *)(uintptr_t)*(const uint64_t *)(uintptr_t)(o->owner + 0xf0 + 0xf0);
    if (!in_heap((uint64_t)(uintptr_t)b) || e < b || e - b > 256) return;
    int n = 0;
    for (const uint64_t *q = b; q < e && n < 24; q++) {
        uint64_t cp = *q - 0x48;
        if (!in_heap(cp) || *(const uint64_t *)(uintptr_t)cp != 0x70041801a0ull) continue;
        uint64_t l = *(const uint64_t *)(uintptr_t)(cp + 0x1f0);
        if (!in_heap(l)) continue;
        uint64_t *mask = (uint64_t *)(uintptr_t)(l + 0x178);
        if (on) *mask = o->rmask[n] ? o->rmask[n] : 1;
        else o->rmask[n] = *mask, *mask = 0;
        n++;
    }
    o->rhidden = !on;
}

// a riding person's frame: its mount where they are, the person on it. 1 = done (the walking steps skipped)
static int ride_step(Cpu *c, Own *o, float dt) {
    int t = o->ride_type & 7;
    bool want = o->riding && t && k_ride[t].mons && o->state == 3;  // (theirs only: one let go asks for none)
    Mnt *m = want ? ride_mount(o) : NULL;
    if (want && !m && o->mnt[t] && !g_mnt[o->mnt[t] - 1].state) o->mnt[t] = 0, o->mnt_fail++;  // (never came up: again)
    if (want && !m && !o->mnt[t] && g_frame >= o->mnt_retry) {  // (its mount asked for, where it is - or, after one that
        // never came up (asked in water, say), near us: it is put where they are once it is up)
        pthread_mutex_lock(&g_lock);
        for (int i = 0; i < NMNT; i++)
            if (!g_mnt[i].state) {
                m = &g_mnt[i];
                memset(m, 0, sizeof *m);
                m->mons = k_ride[t].mons, memcpy(m->at, (const float *)(uintptr_t)(o->owner + 0xa0), 12), m->yaw = o->fo[3] * 57.29578f;
                extern bool warp_player_pos(float out[3]);
                float me[3];
                if (o->mnt_fail && warp_player_pos(me)) {
                    float dx = m->at[0] - me[0], dz = m->at[2] - me[2], dd = sqrtf(dx * dx + dz * dz), k = dd > 3.0f ? 3.0f / dd : 1.0f;
                    m->at[0] = me[0] + dx * k + (dd <= 3.0f ? 2.0f : 0), m->at[1] = me[1], m->at[2] = me[2] + dz * k;
                }
                o->mnt_retry = g_frame + 300;
                m->k = ++g_poke_n, m->state = 1, m->level = g_level, m->rider = (int)(o - g_own) + 1, m->attached = 0, m->t = t;
                snprintf(m->name, sizeof m->name, "mpride_%d", m->k);
                o->mnt[t] = i + 1;
                LOG("[npc] mpplayer_%d rides (type %d): %s asked for\n", o->k, t, m->name);
                break;
            }
        pthread_mutex_unlock(&g_lock);
        m = NULL;
    }
    bool on = want && m && m->state == 3 && m->cc && m->owner;
    if (!on && want && o->rode && o->rode != t) {  // (another ride, not up yet: kept on the one before till then - getting
        // off and on again in a moment left the rider's net in a pose that draws nothing, 2026-11-18 two copies)
        int pi = o->mnt[o->rode & 7] - 1;
        Mnt *pm = pi >= 0 && pi < NMNT && g_mnt[pi].state == 3 && g_mnt[pi].cc && g_mnt[pi].owner ? &g_mnt[pi] : NULL;
        if (pm) m = pm, t = o->rode, on = true;
    }
    float *loc = (float *)(uintptr_t)(o->owner + 0xd0), *lq = (float *)(uintptr_t)(o->owner + 0xc0);
    if (!on) {
        seat_set((int)(o - g_own), 0, NULL);
        if (o->rode) {  // (off: back on its feet where its mount stands, the mount put away)
            o->rode = 0;
            anim_set(c, o->actl, ANIM_SET_BOOL, fnv("set_ride_bool"), 0, 0);
            loc[0] = loc[1] = loc[2] = 0, lq[0] = lq[1] = lq[2] = 0, lq[3] = 1, o->lyaw = 0, o->fa_set = 0;
            memset(o->rlq, 0, sizeof o->rlq), memset(o->rlt, 0, sizeof o->rlt), o->rl_set = 0;
            rider_show(o, 1);
            for (int j = 0; j < 8; j++) {
                int i = o->mnt[j] - 1;
                if (i >= 0 && i < NMNT && g_mnt[i].state == 3) mount_park(c, &g_mnt[i], o->fo);
            }
            cc_warp(c, o->cc, o->fo, o->fo[3]);
            LOG("[npc] mpplayer_%d got off\n", o->k);
        }
        return 0;
    }
    if (o->rode != t) {  // (on it now - or another ride: the one before put away)
        o->mnt_fail = 0;
        int was = o->rode, i = was ? o->mnt[was & 7] - 1 : -1;
        if (i >= 0 && i < NMNT && g_mnt[i].state == 3) mount_park(c, &g_mnt[i], o->fo);
        o->rode = t;
        mount_show(m, 1);
        pthread_mutex_lock(&g_lock);  // (the ride's net parameters heard so far, all set on this one)
        for (int r = 0; r < NRMIRROR; r++)
            if (o->rp_type[r] && o->rp_type[r] != 'T') o->rp_dirty |= 1ull << r;
        pthread_mutex_unlock(&g_lock);
        // (as the game switches its player's ride: in one frame set_ride_bool 0, the new ride_type_int, set_ride_bool 1 -
        // the net enters the riding pose of the new ride afresh; only the type changed left the rider drawn as nothing)
        anim_set(c, o->actl, ANIM_SET_BOOL, fnv("set_ride_bool"), 0, 0);
        anim_set(c, o->actl, ANIM_SET_INT, fnv("ride_type_int"), 0, (uint32_t)t);
        anim_set(c, o->actl, ANIM_SET_BOOL, fnv("set_ride_bool"), 0, 1);
        LOG("[npc] mpplayer_%d on %s\n", o->k, m->name);
    }
    for (int j = 0; j < 8; j++) {  // (its other mounts kept away)
        int i = o->mnt[j] - 1;
        if (j != t && i >= 0 && i < NMNT && g_mnt[i].state == 3 && g_frame % 30 == 0) mount_park(c, &g_mnt[i], o->fo);
    }
    // the mount: where they are, their way
    float yaw = o->fo[3];
    // (its own navigator kept from walking it: its controller "stands at its spot" (+0x240, see unfix) and any path it
    // took up stopped - left alone, it walked off on its own and the rider sat on air)
    if (!m->ctrl) { pthread_mutex_lock(&g_lock); m->ctrl = ctrl_of(m->nav); pthread_mutex_unlock(&g_lock); }
    if (m->ctrl && *(const uint64_t *)(uintptr_t)m->ctrl == CTRL_VTABLE) *(uint8_t *)(uintptr_t)(m->ctrl + CTRL_FIXED) = 1;
    if (*(const uint64_t *)(uintptr_t)m->nav == NAV_VTABLE && *(const uint8_t *)(uintptr_t)(m->nav + 0x240)) guest_call_saved(c, NAV_STOP, m->nav, 0, 0, 0, 0);
    // (its net's root motion lands in its model owner's local transform, as a dressed person's does: taken out - it is
    // put where they are; left there, its model walked off on its own and the rider sat on air)
    float *ml = (float *)(uintptr_t)(m->owner + 0xd0), *mq = (float *)(uintptr_t)(m->owner + 0xc0);
    ml[0] = ml[1] = ml[2] = 0;
    // (Sneasler on a wall, Basculegion in the water: turned as theirs is - their ride's axes, sent - on top of its
    // character's own way (only ever upright): owner local rotation)
    bool tilt = o->ax_ok && (t == 2 || t == 5);
    float qt[4] = {0, 0, 0, 1}, tq[4] = {0, 0, 0, 1};
    if (tilt) {
        float qc[4], ic[4];
        q_char(qc, m->owner, m->lq);
        q_from_upfwd(qt, o->ax, o->ax + 3);
        q_conj(ic, qc), q_mul(tq, ic, qt);
    }
    memcpy(mq, tq, 16), memcpy(m->lq, tq, 16);
    float mat[3] = {o->fo[0], o->fo[1] + k_ride[t].mount_y, o->fo[2]};
    cc_warp(c, m->cc, mat, yaw);
    if (!g_fwd_vec) g_fwd_vec = hle_alloc(16);  // (its facing: the warp's rotation isn't taken - its forward direction is)
    g_fwd_vec[0] = sinf(yaw), g_fwd_vec[1] = 0, g_fwd_vec[2] = cosf(yaw), g_fwd_vec[3] = 0;
    guest_call_saved(c, CC_SET_FORWARD, m->cc, (uint64_t)(uintptr_t)g_fwd_vec, 0, 0, 0);
    if (m->actl && *(const uint64_t *)(uintptr_t)m->actl == POKE_ANIM_VTABLE && m->remodel) {
        // (a ride model: their ride Pokémon's net parameters as heard - its gait, swimming, climbing, gliding; from an
        // older copy that sends none, the gait from their speed)
        uint64_t d, f;
        char ty[NRMIRROR];
        uint32_t va[NRMIRROR];
        pthread_mutex_lock(&g_lock);
        d = o->rp_dirty, f = o->rp_fire, o->rp_dirty = o->rp_fire = 0;
        memcpy(ty, o->rp_type, sizeof ty), memcpy(va, o->rp_val, sizeof va);
        pthread_mutex_unlock(&g_lock);
        for (int r = 0; r < NRMIRROR; r++) {
            if (!(d >> r & 1) || !ty[r]) continue;
            if (ty[r] == 'T') { if (f >> r & 1) anim_set(c, m->actl, ANIM_SET_TRIGGER, g_rmirror_hash[r], 0, 0); continue; }
            float fv;
            memcpy(&fv, &va[r], 4);
            anim_set(c, m->actl, ty[r] == 'F' ? ANIM_SET_FLOAT : ty[r] == 'I' ? ANIM_SET_INT : ANIM_SET_BOOL, g_rmirror_hash[r], fv, va[r]);
        }
        if (ty[0] != 'F') {
            float mv = o->fo[4] / 8.0f;
            anim_set(c, m->actl, ANIM_SET_FLOAT, g_rmirror_hash[0], mv > 1 ? 1 : mv < 0.05f ? 0 : mv, 0);
        }
    } else if (m->actl && *(const uint64_t *)(uintptr_t)m->actl == POKE_ANIM_VTABLE) {
        float mv = o->fo[4] * g_knob[1];
        mv = mv > 2.0f ? 2.0f : mv < 0.05f ? 0 : mv;
        anim_set(c, m->actl, ANIM_SET_FLOAT, POKE_MOVE, mv, 0);
    }
    // the rider: its character under the mount, its model on the saddle (turned into the joint's frame)
    // (its character where the mount stands: parked under the ground (knob 5 = depth) the game stopped posing the rider
    // - its pose froze in the air while the mount rode on)
    float park = g_knob[5];
    float under[3] = {o->fo[0], o->fo[1] - park, o->fo[2]};
    cc_warp(c, o->cc, under, yaw);
    guest_call_saved(c, CC_SET_FORWARD, o->cc, (uint64_t)(uintptr_t)g_fwd_vec, 0, 0, 0);  // (its own way too: the seat is put
    // on top of its character's turn - one left facing elsewhere sat its model on the ride sideways a frame)
    if (!o->rl_set) memcpy(o->rlq, lq, 16), memcpy(o->rlt, loc, 12), o->rl_set = 1;  // (what its owner has now)
    float sq[4], sp[3];
    bool seated = !((int)g_knob[6] & 8) && mount_joint(m, m->t ? m->t : t, sq, sp);
    seat_set((int)(o - g_own), seated && !((int)g_knob[6] & 16) ? o->owner : 0, m);  // (knob 6 bit 16: its local only)
    if (seated) {
        // (on the seat joint, as the game's own rider: its world = the joint's; its character (under it) taken out - its
        // turn and place measured from its world less the local written last frame)
        float qc[4], ic[4], lt[3], d[3];
        const float *wp = (const float *)(uintptr_t)(o->owner + 0xa0);
        q_char(qc, o->owner, o->rlq);
        q_rot(lt, qc, o->rlt);
        for (int i = 0; i < 3; i++) d[i] = sp[i] - (wp[i] - lt[i]);
        q_conj(ic, qc);
        q_mul(lq, ic, sq), q_rot(loc, ic, d);
    } else {  // (no joint found: the seat guessed per ride - research knob 6 bit 8 too)
        loc[0] = 0, loc[1] = park + k_ride[t].mount_y + k_ride[t].seat_y + g_knob[2], loc[2] = k_ride[t].seat_z;
        float qa = (k_ride[t].turn_x + g_knob[4]) * 0.5f;  // (into the joint's frame: Wyrdeer's about x +90 degrees, y forward, z down)
        lq[0] = sinf(qa), lq[1] = 0, lq[2] = 0, lq[3] = cosf(qa);
        if (tilt) {  // (turned with its mount: the seat in the mount's tilted frame)
            float qc[4], ic[4], rel[4], seat[4] = {lq[0], lq[1], lq[2], lq[3]}, sl[3] = {loc[0], loc[1], loc[2]};
            q_char(qc, o->owner, o->rlq);
            q_conj(ic, qc), q_mul(rel, ic, qt);
            q_mul(lq, rel, seat), q_rot(loc, rel, sl);
        }
    }
    memcpy(o->rlq, lq, 16), memcpy(o->rlt, loc, 12);
    rider_show(o, (m->t ? m->t : t) != 5);  // (in Sneasler's basket: not drawn, as the game's own rider)
    // (their speed to the rider's net: the riding pose moves with it)
    float mf = mf_for(o->fo[4]);
    if (o->rp_type[0] == 'F') memcpy(&mf, &o->rp_val[0], 4);  // (the ride's own move_front, as the game sets both)
    if (fabsf(mf - o->mf) > 0.01f || g_frame - o->mf_frame >= 30) anim_set(c, o->actl, ANIM_SET_FLOAT, MOVE_FRONT, mf, 0), o->mf = mf, o->mf_frame = g_frame;
    (void)dt;
    static int wlog = -1;
    if (wlog < 0) wlog = getenv("LARECOMP_NPC_WALKLOG") ? atoi(getenv("LARECOMP_NPC_WALKLOG")) : 0;
    int wl = wlog > (int)g_knob[7] ? wlog : (int)g_knob[7];
    if (wl && (wl > 1 || g_frame % 30 == 0)) {
        const float *mp = (const float *)(uintptr_t)(m->owner + 0xa0), *mz = (const float *)(uintptr_t)(m->owner + 0x90), *rp = (const float *)(uintptr_t)(o->owner + 0xa0);
        LOG("[npc] mpplayer_%d rode (frame %llu): them %.2f %.2f %.2f yaw %.0f speed %.1f | %s at %.2f %.2f %.2f facing %.0f | rider at %.2f %.2f %.2f\n", o->k, (unsigned long long)g_frame,
            o->fo[0], o->fo[1], o->fo[2], yaw * 57.3f, o->fo[4], m->name, mp[0], mp[1], mp[2], atan2f(mz[0], mz[2]) * 57.3f, rp[0], rp[1], rp[2]);
    }
    return 1;
}

static void puppet_step(Cpu *c, Own *o) {
    if (o->step_frame == g_frame) return;  // (once a frame: the camera update can come twice)
    o->step_frame = g_frame;
    unfix(o);  // (its controller kept from standing at its spot: else no root motion moves it)
    uint64_t now = plat_now_ns();
    float dt = o->step_ns ? (float)(now - o->step_ns) * 1e-9f : 0.033f;
    o->step_ns = now;
    if (dt > 0.2f) dt = 0.2f;
    if (ride_step(c, o, dt)) return;
    // swimming: a person's character has no swimming of its own (the base Swim is nothing) - it walked the riverbed.
    // Held where they are instead (at the water, their way), the net's swimming stroke going by their speed
    if (o->swimming || o->swam) {
        float *loc = (float *)(uintptr_t)(o->owner + 0xd0);
        loc[0] = loc[1] = loc[2] = 0;
        if (!o->swimming) { o->swam = 0; LOG("[npc] mpplayer_%d out of the water\n", o->k); return; }
        if (!o->swam) {
            float *lq = (float *)(uintptr_t)(o->owner + 0xc0);
            lq[0] = lq[1] = lq[2] = 0, lq[3] = 1, o->lyaw = 0, o->fa_set = 0, o->swam = 1;
            LOG("[npc] mpplayer_%d swims\n", o->k);
        }
        cc_warp(c, o->cc, o->fo, o->fo[3]);
        if (!g_fwd_vec) g_fwd_vec = hle_alloc(16);
        g_fwd_vec[0] = sinf(o->fo[3]), g_fwd_vec[1] = 0, g_fwd_vec[2] = cosf(o->fo[3]), g_fwd_vec[3] = 0;
        guest_call_saved(c, CC_SET_FORWARD, o->cc, (uint64_t)(uintptr_t)g_fwd_vec, 0, 0, 0);
        float mf = mf_for(o->fo[4]);
        if (fabsf(mf - o->mf) > 0.01f || g_frame - o->mf_frame >= 30) anim_set(c, o->actl, ANIM_SET_FLOAT, MOVE_FRONT, mf, 0), o->mf = mf, o->mf_frame = g_frame;
        return;
    }
    // (the root motion: the player's net leaves it in its model's local translation (owner +0xd0) - where the field
    // player's own code takes it. Taken here: zeroed, and its physics character given that velocity - so it walks as
    // far as its feet say, against the world's collision)
    float *loc = (float *)(uintptr_t)(o->owner + 0xd0);
    const float *ax = (const float *)(uintptr_t)(o->owner + 0x70), *az = (const float *)(uintptr_t)(o->owner + 0x90);
    float rx = loc[0], rz = loc[2];
    loc[0] = loc[1] = loc[2] = 0;
    if (!g_vel_vec) g_vel_vec = hle_alloc(16);
    float inv = dt > 0.005f ? 1.0f / dt : 30.0f;
    const float *pos = (const float *)(uintptr_t)(o->owner + 0xa0);
    // (standing: none - the idle's own drift would walk it off its spot; down to them: lowered up to 4 m/s. Never
    // lifted (knob 0 = 0: up to 2.5 m/s as before): lifted, it left the ground - in the air its facing froze and it
    // drifted off the stairs it was on, then got stuck and was put there; on the ground the physics character climbs
    // steps itself, as the player's does (Jubilife Galaxy hall steps 485.4 446 -> 487.5 443.6, 1.6 m up, 2026-11-18))
    float up = o->fo[1] - pos[1], vy = 0;
    int vym = (int)g_knob[0];
    if (o->moving && fabsf(up) > 0.15f && vym != 2 && !(vym == 1 && up > 0)) vy = fmaxf(fminf(up * 4.0f, 2.5f), -4.0f);
    if (!o->moving) rx = rz = 0;
    // (running flat out and still behind: the net's fastest run is 4.84 m/s, a player runs 5.0 - its steps taken a
    // little longer, up to knob 3 (1.12x: no sliding to be seen), so it keeps up)
    float kspd = 1;
    if (o->mf >= 0.999f && o->want_v > 4.84f) kspd = fminf(o->want_v / 4.84f, g_knob[3]);
    g_vel_vec[0] = (rx * ax[0] + rz * az[0]) * inv * kspd, g_vel_vec[1] = vy, g_vel_vec[2] = (rx * ax[2] + rz * az[2]) * inv * kspd, g_vel_vec[3] = 0;
    guest_call_saved(c, CC_SET_VELOCITY, o->cc, (uint64_t)(uintptr_t)g_vel_vec, 0, 0, 0);
    // (walking but not getting anywhere for 1.5 s while over 2 m off: stuck - put where they are, below)
    if (o->moving && o->mf > 0.2f) {
        float mx = pos[0] - o->stuck_at[0], mz = pos[2] - o->stuck_at[2];
        if (!o->stuck_frame || mx * mx + mz * mz > 0.09f) o->stuck_frame = g_frame, o->stuck_at[0] = pos[0], o->stuck_at[2] = pos[2];
    } else o->stuck_frame = 0;
    float ex = o->fo[0] - pos[0], ez = o->fo[2] - pos[2], d = sqrtf(ex * ex + ez * ez), s = o->fo[4];
    float yx = sinf(o->fo[3]), yz = cosf(o->fo[3]);
    // (left far behind - stuck on something, or they jumped down / turned a corner it can't follow: over 10 m for 2 s
    // or over 25 m at once - it is put where they are: the physics character's Warp 0x7001983770(cc, &pos, &rotation))
    // (also over 4 m above or below them - made where they were mid-fall or under the ground, or fallen through it:
    // it never walks there)
    if (s >= 0 && (d > 10.0f || fabsf(o->fo[1] - pos[1]) > 4.0f)) { if (!o->far_frame) o->far_frame = g_frame; }
    else o->far_frame = 0;
    bool stuck = o->stuck_frame && g_frame - o->stuck_frame > 45 && d > 2.0f;
    if (s >= 0 && (d > 25.0f || (o->far_frame && g_frame - o->far_frame > 60) || stuck || o->warp_now)) {
        o->stuck_frame = 0;
        o->warp_now = 0;
        if (!g_warp_vec) g_warp_vec = hle_alloc(32);
        g_warp_vec[0] = o->fo[0], g_warp_vec[1] = o->fo[1] + 0.1f, g_warp_vec[2] = o->fo[2], g_warp_vec[3] = 0;
        g_warp_vec[4] = 0, g_warp_vec[5] = sinf(o->fo[3] * 0.5f), g_warp_vec[6] = 0, g_warp_vec[7] = cosf(o->fo[3] * 0.5f);
        guest_call_saved(c, CC_WARP, o->cc, (uint64_t)(uintptr_t)g_warp_vec, (uint64_t)(uintptr_t)(g_warp_vec + 4), 0, 0);
        LOG("[npc] mpplayer_%d was %.1f m behind (%.1f m below them)%s: put where they are (%.2f %.2f %.2f)\n", o->k, d, o->fo[1] - pos[1], stuck ? " (stuck)" : "", o->fo[0], o->fo[1], o->fo[2]);
        o->far_frame = 0;
        return;
    }
    // how fast: their speed, plus closing what it is behind (0.8/s); to a standing spot at most 0.4 + 0.9/s of the
    // way, stopping within 0.5 m (walking again only past 1 m: no stepping about on the spot)
    // (a negative speed: walk there at most that fast and stand - a person let go walking off)
    float v = 0;
    if (s < 0) v = d > (o->moving ? 0.3f : 0.5f) ? fminf(-s, 0.3f + 1.0f * d) : 0, s = 0;
    else if (s > 0.2f) v = fminf(s + 0.8f * d, 6.0f);
    else if (d > (o->moving ? 0.5f : 1.0f)) v = fminf(0.4f + 0.9f * d, 4.8f);
    o->moving = v > 0, o->want_v = v;
    // which way: toward the spot when off it, else their way (moving) / their facing (standing) - turned at most 12 rad/s
    float wx, wz;
    if (o->moving && d > 0.4f) {
        wx = ex / d, wz = ez / d;
        // (close behind someone walking: mostly their own way - the way to a spot under a metre off swung 10 degrees
        // from frame to frame as their drawn spot moved on in steps)
        if (s > 0.2f && d < 1.5f && !((int)g_knob[6] & 2)) {
            float k = d / 1.5f, bx = yx * (1 - k) + wx * k, bz = yz * (1 - k) + wz * k, n = sqrtf(bx * bx + bz * bz);
            if (n > 0.1f) wx = bx / n, wz = bz / n;
        }
    } else wx = yx, wz = yz;
    // (from the way it faces now - its owner's z axis: the net's own turns rotate it too; less our own turn of its model
    // (lyaw, below) - the way its character faces)
    const float *zx = (const float *)(uintptr_t)(o->owner + 0x90);
    float cy = atan2f(zx[0], zx[2]) - o->lyaw;  // (its character's)
    // (turned on from the way it was drawn last frame (fa), not from its character's: that one stands still every 2nd
    // frame and the turn with it)
    if (!o->fa_set || ((int)g_knob[6] & 4)) o->fa = cy, o->fa_set = 1;
    float a = o->fa, b = atan2f(wx, wz), da = remainderf(b - a, 6.2831853f), lim = 12.0f * dt;
    // (turning round: the way it began, kept - near 180 degrees off, the nearer way flips from frame to frame and it
    // turned back and forth)
    if (!((int)g_knob[6] & 1)) {
        if (fabsf(da) > 2.4f && o->turn_sign && (da > 0 ? 1 : -1) != o->turn_sign) da += o->turn_sign * 6.2831853f;
        o->turn_sign = fabsf(da) > 0.3f ? (da > 0 ? 1 : -1) : fabsf(da) < 0.1f ? 0 : o->turn_sign;
    }
    float off = fabsf(da);
    if (da > lim) da = lim; else if (da < -lim) da = -lim;
    a += da;
    if (off > 0.03f) {
        if (!g_fwd_vec) g_fwd_vec = hle_alloc(16);
        g_fwd_vec[0] = sinf(a), g_fwd_vec[1] = 0, g_fwd_vec[2] = cosf(a), g_fwd_vec[3] = 0;
        guest_call_saved(c, CC_SET_FORWARD, o->cc, (uint64_t)(uintptr_t)g_fwd_vec, 0, 0, 0);
        o->face[0] = g_fwd_vec[0], o->face[1] = g_fwd_vec[2];
    }
    // (its character takes a new forward direction only on its own update - every 2nd frame for people, a call that
    // came in between overwritten: its turn stood still for a frame, then caught up. Its model is turned the rest of
    // the way itself - its owner's local rotation about y - so it is drawn facing the way given every frame)
    {
        float *lq = (float *)(uintptr_t)(o->owner + 0xc0), l = remainderf(a - cy, 6.2831853f);
        if ((int)g_knob[6] & 4) l = 0;
        if (l > 0.6f) l = 0.6f, a = cy + l; else if (l < -0.6f) l = -0.6f, a = cy + l;  // (its character far behind: no further)
        o->fa = a, o->lyaw = l, lq[0] = 0, lq[1] = sinf(l * 0.5f), lq[2] = 0, lq[3] = cosf(l * 0.5f);
    }
    // (the spot behind it and near: it turns on the spot first, as a player does - no circling round it; turning
    // hard: slower)
    float mf = mf_for(v);
    if (off > 1.75f && d < 2.5f) mf = 0;
    else if (off > 1.0f) mf *= 0.5f;
    // (given again every second, a hair off: should the component pass on changes only, or its net be set up again
    // once the dress-up has loaded, a value given early still arrives)
    if (fabsf(mf - o->mf) > 0.01f || g_frame - o->mf_frame >= 30) {
        o->nudge ^= 1;
        anim_set(c, o->actl, ANIM_SET_FLOAT, MOVE_FRONT, mf + (o->nudge ? 1e-4f : 0), 0);
        o->mf = mf, o->mf_frame = g_frame;
    }
    static int wlog = -1;  // (research LARECOMP_NPC_WALKLOG=1: each one's steering logged every second)
    if (wlog < 0) wlog = getenv("LARECOMP_NPC_WALKLOG") ? atoi(getenv("LARECOMP_NPC_WALKLOG")) : 0;
    static int nlog;
    int wl = wlog > (int)g_knob[7] ? wlog : (int)g_knob[7];  // (knob 7: the same, live)
    if (wl && nlog < 20000 && (wl > 1 || g_frame % 30 == 0)) {
        const float *lq = (const float *)(uintptr_t)(o->owner + 0xc0);
        nlog++, LOG("[npc] mpplayer_%d walked (frame %llu): at %.2f %.2f %.2f, %.2f m off (dy %.2f), their speed %.1f -> %.2f m/s (move_front %.2f, root %.3f %.3f), facing %.0f want %.0f set %.0f | local q %.2f %.2f %.2f %.2f\n", o->k,
                    (unsigned long long)g_frame, pos[0], pos[1], pos[2], d, o->fo[1] - pos[1], s, v, mf, rx, rz, atan2f(zx[0], zx[2]) * 57.3f, b * 57.3f, a * 57.3f,
                    lq[0], lq[1], lq[2], lq[3]);
    }
}

// ---- Pokémon of our own (research first: <live>/npc "poke <monsno> <x> <y> <z> [yaw deg]"): made the way event scripts
// make a story Pokémon - FieldObjectCommand.FObjCreateNPCPokemonDirectly(x, y, z, rot, name, monsno, form, sex, rare,
// height, alpha, p11, p12) -> Global.GetFieldObjectForHaxe() (0x7000d91c8c) :__haxe_export_CreateNpcPokemonObjectDirectly
// (the Lua binding's function 0x70020bf6fc(fo, &pos, &rot (radians about y), &scale, &FNV(name), name, &FNV(""),
// w7 monsno, stack: u16 form, u32 sex, u8 rare, u32 height, u8 alpha, u8 p11, u8 p12 - 8-byte slots) -> 0x7001e9b53c)
#define FO_GET 0x7000d91c8cull
#define FO_POKE_DIRECT 0x70020bf6fcull
static uint64_t poke_make(Cpu *c, int mons, int form, const float at[3], float yaw_deg, const char *name, int rare) {
    static uint8_t *m;
    if (!m) m = hle_alloc(0x100);
    memset(m, 0, 0x100);
    float *pos = (float *)m, *rot = (float *)(m + 0x10), *scale = (float *)(m + 0x20);
    pos[0] = at[0], pos[1] = at[1], pos[2] = at[2], rot[1] = yaw_deg * 3.14159265f / 180.0f, scale[0] = scale[1] = scale[2] = 1;
    uint64_t h = fnv(name), e = EMPTY_HASH;
    memcpy(m + 0x30, &h, 8), memcpy(m + 0x38, &e, 8);
    snprintf((char *)(m + 0x40), 0x40, "%s", name);
    uint64_t fo = gcall(c, FO_GET, 0, 0, 0, 0);
    if (!in_heap(fo)) { LOG("[npc] no FieldObjectForHaxe (%llx)\n", (unsigned long long)fo); return 0; }
    Cpu saved = *c;
    uint64_t sp = (c->sp - 0x800) & ~0xfull;
    uint64_t *st = (uint64_t *)(uintptr_t)sp;
    memset(st, 0, 0x40);
    st[0] = (uint64_t)form, st[1] = 0, st[2] = (uint64_t)(rare != 0), st[3] = 128, st[4] = 0, st[5] = 1, st[6] = 1;  // (form, sex, rare, height, alpha, p11, p12)
    c->sp = sp;
    c->x[0] = fo, c->x[1] = (uint64_t)(uintptr_t)pos, c->x[2] = (uint64_t)(uintptr_t)rot, c->x[3] = (uint64_t)(uintptr_t)scale;
    c->x[4] = (uint64_t)(uintptr_t)(m + 0x30), c->x[5] = (uint64_t)(uintptr_t)(m + 0x40), c->x[6] = (uint64_t)(uintptr_t)(m + 0x38), c->x[7] = (uint64_t)mons;
    guest_call(c, FO_POKE_DIRECT);
    *c = saved;
    LOG("[npc] Pokemon %d asked for as %s at %.2f %.2f %.2f (FieldObjectForHaxe %llx)\n", mons, name, at[0], at[1], at[2], (unsigned long long)fo);
    return 1;
}

// A Pokémon of ours removed as event scripts remove theirs: FieldObjectCommand.TerminateObject(name) -> the binding's
// 0x7001e9e274(FieldObjectForHaxe, &FNV-1a(name)) (finds the object by name, ends its character / Pokémon components).
// Ours go when unused for MOUNT_IDLE_S, when their rider is let go, or when given up before they came up.
#define FO_TERMINATE 0x7001e9e274ull
#define MOUNT_IDLE_S 45
static void mount_term(Cpu *c, Mnt *m, const char *why) {
    static uint64_t *h;
    if (!h) h = hle_alloc(16);
    uint64_t fo = gcall(c, FO_GET, 0, 0, 0, 0);
    if (in_heap(fo)) h[0] = fnv(m->name), gcall(c, FO_TERMINATE, fo, (uint64_t)(uintptr_t)h, 0, 0);
    int idx = (int)(m - g_mnt);
    for (int i = 0; i < NOWN; i++)
        if (g_seat[i].owner && g_seat[i].mowner == m->owner) seat_set(i, 0, NULL);
    pthread_mutex_lock(&g_lock);
    if (m->rider > 0 && m->rider <= NOWN) {
        for (int j = 0; j < 8; j++)
            if (g_own[m->rider - 1].mnt[j] == idx + 1) g_own[m->rider - 1].mnt[j] = 0;
        if (m->state == 2) g_own[m->rider - 1].mnt_fail++;  // (never came up: the next one asked for near us)
    }
    LOG("[npc] %s removed (%s)\n", m->name, why);
    memset(m, 0, sizeof *m);
    pthread_mutex_unlock(&g_lock);
}

// A rider put on its mount the way the game mounts the player (traced 2026-11-18: on mounting, 0x7002570a8c gets the
// ride config's "player_attach_joint_name" - Wyrdeer "parts", Basculegion / Sneasler "origin", Braviary "parts_03",
// Ursaluna "parts_01" - and calls 0x7000c81760(the player's attach component, &weak reference to the ride Pokémon's model
// owner, &the joint name, 0) -> 0x7000c81464(component, &ref, &{FNV-1a, char *, length, 0}, flag)): the player's model
// owner is then drawn at that joint (its local translation 0) and plays its net's riding pose (set_ride_bool,
// ride_type_int). The component: vtable 0x70040ec370, one of the owner's components (list owner+0x1d8..+0x1e0, each
// entry the component + 0x48). A weak reference to an owner (scene object 0x700417dc80): {[owner+0x58], [owner+0x60]}.
#define ATTACH_FN 0x7000c81464ull
#define ATTACH_COMP_VTABLE 0x70040ec370ull
static uint64_t comp_of(uint64_t owner, uint64_t vt) {
    const uint64_t *b = (const uint64_t *)(uintptr_t)*(const uint64_t *)(uintptr_t)(owner + 0xf0 + 0xe8);
    const uint64_t *e = (const uint64_t *)(uintptr_t)*(const uint64_t *)(uintptr_t)(owner + 0xf0 + 0xf0);
    if (!in_heap((uint64_t)(uintptr_t)b) || e < b || e - b > 256) return 0;
    for (const uint64_t *q = b; q < e; q++) {
        uint64_t cp = *q - 0x48;
        if (in_heap(cp) && *(const uint64_t *)(uintptr_t)cp == vt) return cp;
    }
    return 0;
}
static int attach_to(Cpu *c, uint64_t owner, uint64_t parent, const char *joint) {  // (parent 0: let go)
    uint64_t comp = comp_of(owner, ATTACH_COMP_VTABLE);
    if (!comp) return 0;
    static uint64_t *t;
    if (!t) t = hle_alloc(0x80);
    memset(t, 0, 0x80);
    if (parent) t[0] = *(const uint64_t *)(uintptr_t)(parent + 0x58), t[1] = *(const uint64_t *)(uintptr_t)(parent + 0x60);
    char *js = (char *)&t[8];
    snprintf(js, 32, "%s", joint ? joint : "");
    t[4] = fnv(js), t[5] = (uint64_t)(uintptr_t)js, t[6] = strlen(js), t[7] = 0;
    gcall_n(c, ATTACH_FN, comp, (uint64_t)(uintptr_t)&t[0], (uint64_t)(uintptr_t)&t[4], 0);
    return 1;
}

// orders need a vec3 in guest memory: one per person, written just before the call
static float *g_vec[NOWN];
static void mod_cam(Cpu *c);
// ---- items (the mod API's item_give): the bag's own add, Bag::AddItem 0x7000fdb1d8(bag, item, n, slot - -1: where it
// fits, its stack or the first free) - the script binding "Bag.AddItem" (registered with GetItemNum 0x7000fda868,
// HaveNeverAdded, IsAddItem, AddItemToLimit, SubItem by 0x7000f431e0). The bag is [holder + 0x280], the holder the
// global behind GOT 0x7004279560 (as 0x7000cab054 reads it for GetItemNum). Found 2026-11-19 (Studio exestrings + a
// flat image of main: AddItem's name -> its registration -> the function stored beside it).
#define BAG_HOLDER_GOT 0x7004279560ull
#define BAG_ADD 0x7000fdb1d8ull
#define BAG_NUM 0x7000fda868ull
static struct { int item, n; } g_items[16];
static volatile int g_nitems;
int pla_item_give(int item, int n) {
    if (item <= 0 || item > 0xffff || n <= 0 || n > 999) return 0;
    pthread_mutex_lock(&g_lock);
    int ok = g_nitems < 16;
    if (ok) g_items[g_nitems].item = item, g_items[g_nitems].n = n, g_nitems++;
    pthread_mutex_unlock(&g_lock);
    return ok;
}
static void items_run(Cpu *c) {
    if (!g_nitems) return;
    uint64_t got = *(const uint64_t *)(uintptr_t)BAG_HOLDER_GOT, holder = 0, bag = 0;
    if (got >= 0x7000000000ull && got < 0x7008000000ull) holder = *(const uint64_t *)(uintptr_t)got;
    if (in_heap(holder)) bag = *(const uint64_t *)(uintptr_t)(holder + 0x280);
    if (!in_heap(bag)) return;  // (no save loaded yet: at a later frame)
    pthread_mutex_lock(&g_lock);
    int n = g_nitems;
    struct { int item, n; } todo[16];
    memcpy(todo, g_items, sizeof todo);
    g_nitems = 0;
    pthread_mutex_unlock(&g_lock);
    uint64_t sx[32], snzcv = c->nzcv;
    __uint128_t sv[32];
    memcpy(sx, c->x, sizeof sx), memcpy(sv, c->v, sizeof sv);
    for (int i = 0; i < n; i++) {
        c->x[0] = bag, c->x[1] = (uint32_t)todo[i].item;
        guest_call(c, BAG_NUM);
        uint32_t had = (uint32_t)c->x[0];
        c->x[0] = bag, c->x[1] = (uint32_t)todo[i].item, c->x[2] = (uint32_t)todo[i].n, c->x[3] = 0xffffffffull;
        guest_call(c, BAG_ADD);
        int ok = (int)(c->x[0] & 0xff);
        c->x[0] = bag, c->x[1] = (uint32_t)todo[i].item;
        guest_call(c, BAG_NUM);
        LOG("[npc] item %d x%d %s (had %u, now %u)\n", todo[i].item, todo[i].n, ok ? "given" : "not given (full, or no such item)", had, (uint32_t)c->x[0]);
    }
    memcpy(c->x, sx, sizeof sx), memcpy(c->v, sv, sizeof sv), c->nzcv = snzcv;
}

// ---- The time of day for scripts (time_set: game.set_time; traced 2026-11-19). Arceus' clock is an object (vtable
// 0x700415e328) whose +0x70 is the minute of the day (a double: hour * 60 + minute + second / 60) and +0x78 the hour -
// found by heap snapshots across rests at camp (the double 486.7 -> 1263.3 -> 422.9 for 8:06 -> nightfall -> morning).
// Resting sets it through the event system: the camp's triggers (bin/event/event_progress/trigger, after CampEvent's
// result 0-3) carry the action time_change (kind 0x48d69d8788b2fe61: "sets the time to 7 / 12 / 19 / 21"), whose
// handler (vtable 0x7004123e40, execute 0x70015917b8) finds the clock (0x700155a260 -> 0x70014196f8 -> 0x70018cb7ec:
// [+0x1d0]), sets it (CLOCK_SET(clock, hour, minute, second, 1)) and has the sky follow at once (0x700155a7a0's object:
// its +0xf0 (0x700189c708) told the old minute (0x7002499244), its +0xe0 (0x700189c694) 0x7001e20578(0.01, 0)) - the
// clock alone set leaves the sky as it was. Done here as that handler does, references taken and given back the same.
#define CLOCK_ROOT 0x700155a260ull
#define CLOCK_CAST 0x70014196f8ull
#define CLOCK_OF 0x70018cb7ecull
#define CLOCK_GET 0x70024acf84ull
#define CLOCK_SET 0x70024accdcull
#define SKY_ROOT 0x700155a7a0ull
#define SKY_TYPE_GOT 0x70042796e8ull
#define SKY_A 0x700189c708ull
#define SKY_A_TELL 0x7002499244ull
#define SKY_B 0x700189c694ull
#define SKY_B_SNAP 0x7001e20578ull
#define REF_WEAK_DROP 0x7002943934ull
#define REF_DROP 0x7002943a5cull
static volatile float g_time_want = -1;
int pla_time_set(float hour) {
    if (!(hour >= 0 && hour < 24.0f)) return 0;
    g_time_want = hour;
    return 1;
}
static uint64_t tcall(Cpu *c, uint64_t fn, uint64_t x0, uint64_t x8) {  // (x0 in, x8 the result's slot: x0 out)
    c->x[0] = x0, c->x[8] = x8;
    guest_call(c, fn);
    return c->x[0];
}
static void tdrop(Cpu *c, uint64_t obj) {  // (a strong reference given back: its control block +0x58, deleted at 0)
    if (!in_heap(obj)) return;
    c->x[0] = *(const uint64_t *)(uintptr_t)(obj + 0x58);
    guest_call(c, REF_DROP);
    if (c->x[0] & 1) c->x[0] = obj, guest_call(c, *(const uint64_t *)(uintptr_t)(*(const uint64_t *)(uintptr_t)obj + 0x20));
}
static void time_run(Cpu *c) {
    float want = g_time_want;
    if (want < 0) return;
    g_time_want = -1;
    static uint64_t *slot;
    if (!slot) slot = hle_alloc(0x40);
    uint64_t s0 = (uint64_t)(uintptr_t)slot, s1 = s0 + 0x20;
    int h = (int)want, m = (int)((want - (float)h) * 60.0f + 0.5f);
    if (m >= 60) m = 59;
    uint64_t sx[32], snzcv = c->nzcv;
    __uint128_t sv[32];
    memcpy(sx, c->x, sizeof sx), memcpy(sv, c->v, sizeof sv);
    double was = 0;
    uint64_t a, b = 0, clock = 0;
    memset(slot, 0, 0x40);
    tcall(c, CLOCK_ROOT, 0, s0);
    if ((a = slot[0])) {
        tcall(c, CLOCK_CAST, a, s1), b = slot[4];
        c->x[0] = a, guest_call(c, REF_WEAK_DROP);
    }
    if (b) {
        slot[0] = 0;
        tcall(c, CLOCK_OF, b, s0), clock = slot[0];
        if (clock) {
            c->x[0] = clock, guest_call(c, CLOCK_GET);
            memcpy(&was, &c->v[0], 8);
            c->x[0] = clock, c->x[1] = (uint32_t)h, c->x[2] = (uint32_t)m, c->x[3] = 0, c->x[4] = 1;
            guest_call(c, CLOCK_SET);
            tdrop(c, clock);
        }
        tdrop(c, b);
    }
    int sky = 0;
    if (clock) {  // (the sky told: as the handler does)
        memset(slot, 0, 0x40);
        tcall(c, SKY_ROOT, 0, s0);
        uint64_t w = slot[0], e = 0, want_t = *(const uint64_t *)(uintptr_t)SKY_TYPE_GOT;
        if (in_heap(w) && *(const uint32_t *)(uintptr_t)(w + 0xc)) {  // (the weak reference locked if its type is the sky's)
            uint64_t t = *(const uint64_t *)(uintptr_t)(w + 0x10);
            for (int i = 0; i < 16 && t && t != want_t && guest_ptr(t); i++) t = *(const uint64_t *)(uintptr_t)t;
            uint64_t p = *(const uint64_t *)(uintptr_t)(w + 0x18);
            if (t == want_t && p) __atomic_add_fetch((uint32_t *)(uintptr_t)(w + 0xc), 1, __ATOMIC_ACQ_REL), e = p - 0x50;
        }
        if (w) c->x[0] = w, guest_call(c, REF_WEAK_DROP);
        if (e) {
            slot[0] = 0;
            uint64_t f = (tcall(c, SKY_A, e, s0), slot[0]);
            if (f) {
                c->x[0] = f, memcpy(&c->v[0], &was, 8);
                guest_call(c, SKY_A_TELL);
                tdrop(c, f), sky |= 1;
            }
            slot[0] = 0;
            uint64_t g = (tcall(c, SKY_B, e, s0), slot[0]);
            if (g) {
                double snap = 0.01;
                c->x[0] = g, c->x[1] = 0, memcpy(&c->v[0], &snap, 8);
                guest_call(c, SKY_B_SNAP);
                tdrop(c, g), sky |= 2;
            }
            tdrop(c, e);
        }
    }
    memcpy(c->x, sx, sizeof sx), memcpy(c->v, sv, sizeof sv), c->nzcv = snzcv;
    if (!clock) LOG("[npc] time %.2f: the game's clock isn't there (no level yet?)\n", want);
    else LOG("[npc] time set to %02d:%02d (was %02d:%02d; the sky %s)\n", h, m, (int)(was / 60), (int)was % 60, sky == 3 ? "told" : "not found");
}

void pla_npc_cam(Cpu *c) {  // (the camera update, the game's thread: hle/warp.c)
    items_run(c);
    time_run(c);
    if (!g_mgr) return;
    nav_match();
    mod_cam(c);
    if (g_npar) par_run(c);
    if (!g_try_done) try_wear(c);
    static int ridelog = -1;  // (research LARECOMP_NPC_RIDELOG=<n>: the player's model owner every n frames)
    if (ridelog < 0) ridelog = getenv("LARECOMP_NPC_RIDELOG") ? atoi(getenv("LARECOMP_NPC_RIDELOG")) : 0;
    if (ridelog > 0 && g_frame % (uint64_t)ridelog == 0 && in_heap(g_player_owner) && *(const uint64_t *)(uintptr_t)g_player_owner == MODEL_OBJ_VTABLE) {
        extern bool warp_player_pos(float out[3]);
        float me[3] = {0};
        warp_player_pos(me);
        const float *w = (const float *)(uintptr_t)(g_player_owner + 0xa0), *l = (const float *)(uintptr_t)(g_player_owner + 0xd0), *z = (const float *)(uintptr_t)(g_player_owner + 0x90);
        const float *y = (const float *)(uintptr_t)(g_player_owner + 0x80);
        LOG("[npc] ride log: owner %llx world %.2f %.2f %.2f local %.3f %.3f %.3f z %.2f %.2f %.2f y %.2f %.2f %.2f | body %.2f %.2f %.2f\n", (unsigned long long)g_player_owner, w[0], w[1], w[2], l[0], l[1], l[2], z[0], z[1], z[2], y[0], y[1], y[2], me[0], me[1], me[2]);
    }
    for (int i = 0; i < NMNT; i++) {  // (mounts: asked for, their owner, their rider)
        Mnt *m = &g_mnt[i];
        if (m->state == 1) {
            g_making = m;
            m->state = poke_make(c, m->mons, m->form ? m->form : m->t == 4 ? 1 : 0, m->at, m->yaw, m->name, m->rare) ? 2 : 0, m->asked = g_frame;
            g_making = NULL;
            if (m->chr_pre) LOG("[npc] %s: its model component %llx made at once\n", m->name, (unsigned long long)m->chr_pre);
            continue;
        }
        if (m->state && m->level != g_level) {  // (went with its level)
            for (int k = 0; k < NOWN; k++)
                if (g_seat[k].owner && g_seat[k].mowner == m->owner) seat_set(k, 0, NULL);
            memset(m, 0, sizeof *m);
            continue;
        }
        if (m->state == 2 && g_frame - m->asked > 900) { mount_term(c, m, "not up 30 s after asking"); continue; }
        if (m->state != 3) continue;
        if (m->owner) unfreeze(m->owner);
        if (m->nanim) {  // (a script's parameters for its animation: one_emotion_int + one_emotion_trigger ... - in the order asked)
            pthread_mutex_lock(&g_lock);
            int na = m->nanim;
            struct { uint64_t hash; char kind; float v; } an[8];
            memcpy(an, m->anim, sizeof an);
            m->nanim = 0;
            pthread_mutex_unlock(&g_lock);
            for (int k = 0; k < na; k++) {
                uint64_t fn = an[k].kind == 'f' ? ANIM_SET_FLOAT : an[k].kind == 'i' ? ANIM_SET_INT : an[k].kind == 'b' ? ANIM_SET_BOOL : ANIM_SET_TRIGGER;
                anim_set(c, m->actl, fn, an[k].hash, an[k].v, (uint32_t)(int32_t)an[k].v);
            }
        }
        if (m->research && m->term) { mount_term(c, m, "asked"); continue; }
        if (m->t && !m->research) {   // (in use: ridden now by its rider - else removed after a while, or at once when its rider was let go)
            Own *ro = m->rider > 0 && m->rider <= NOWN ? &g_own[m->rider - 1] : NULL;
            bool ridden = ro && ro->rode && ro->mnt[ro->rode & 7] == i + 1;
            uint64_t now = plat_now_ns();
            if (ridden) m->used_ns = now;
            if (m->term || !ro || (ro->state != 3 && ro->state != 2)) { mount_term(c, m, m->term ? "its rider let go" : "no rider"); continue; }
            if (now - m->used_ns > (uint64_t)MOUNT_IDLE_S * 1000000000ull) { mount_term(c, m, "unused"); continue; }
        }
        if (!m->owner) {
            pthread_mutex_lock(&g_lock);
            m->owner = poke_owner(m->chr);
            pthread_mutex_unlock(&g_lock);
            if (m->owner) LOG("[npc] %s: its model owner %llx\n", m->name, (unsigned long long)m->owner);
        }
        if (m->attach_req && m->rider && !m->attached && m->owner && m->rider - 1 < NOWN && puppet_ready(&g_own[m->rider - 1])) {  // (research: aborts - see ride_step)
            Own *o = &g_own[m->rider - 1];
            int ok = attach_to(c, o->owner, m->owner, m->joint);
            m->attached = ok ? 1 : -1;
            LOG("[npc] mpplayer_%d put on %s (joint %s): %s\n", o->k, m->name, m->joint, ok ? "attached" : "no attach component");
        }
    }
    int made = 0;
    for (int i = 0; i < NOWN; i++) {
        Own *o = &g_own[i];
        if (o->state == 1 && !made) { make_one(c, o), made = 1; continue; }  // (one a frame)
        if ((o->state == 2 || o->state == 5) && g_frame - o->made_frame > 900) {  // (the game makes people far from us slowly)
            // (given up - but the game may still make it later: then it is kept as a free one, not left standing
            // where nobody is; a level change lets it go for good)
            LOG("[npc] mpplayer_%d: no navigator 30 s after making it - given up (kept as a free one if it comes)\n", o->k);
            o->state = 6;
            continue;
        }
        if (o->comp && in_heap(o->owner) && *(const uint64_t *)(uintptr_t)o->owner == MODEL_OBJ_VTABLE && g_frame % 10 == 0) lod_show(o);
        if (o->owner && (o->state == 3 || o->state == 4)) unfreeze(o->owner);
        if (g_seat[i].owner && (!(o->state == 3 || o->state == 4) || !o->rode || g_seat[i].owner != o->owner)) seat_set(i, 0, NULL);
        if (o->comp && o->settled < 2 && in_heap(o->comp) && g_frame % 15 == 0) {  // (log: its items loaded and worn)
            uint32_t st = *(const uint32_t *)(uintptr_t)(o->comp + 0x27b8);
            if ((st == 1) != o->settled) o->settled = st == 1 ? 2 : 0, LOG("[npc] mpplayer_%d's dress-up: state %u\n", o->k, st);
        }
        if ((o->state == 3 || o->state == 4) && o->redress && in_heap(o->comp)) {  // (their changed items put on)
            o->redress = 0;
            int n = 0;
            for (int j = 0; j < 11; j++) {
                if (!o->outfit[j] || o->outfit[j] == EMPTY_HASH || o->outfit[j] == o->worn[j]) continue;
                static uint64_t *it;
                if (!it) it = hle_alloc(0x20);
                it[0] = o->outfit[j], o->worn[j] = o->outfit[j], n++;
                gcall(c, MODEL_BUILD, o->comp, (uint64_t)(uintptr_t)it, 0, 0);
            }
            LOG("[npc] mpplayer_%d re-dressed: %d items changed\n", o->k, n);
        }
        if ((o->state == 3 || o->state == 4) && o->has_outfit && !o->dressed && g_frame - o->up_frame > 300) {
            // (its model was made before its outfit came, or not by the job we know: the senior's look, walked by its
            // navigator)
            LOG("[npc] mpplayer_%d: not dressed 10 s after it came up - the senior's look\n", o->k);
            o->has_outfit = 0;
        }
        if ((o->state == 3 || o->state == 4) && o->has_outfit && o->fo_set && puppet_ready(o)) { puppet_step(c, o); continue; }
        if ((o->state != 3 && o->state != 4) || !o->order) continue;
        static int hold = -1;  // (research LARECOMP_NPC_HOLD=<frames>: no orders that long after it is up)
        if (hold < 0) hold = getenv("LARECOMP_NPC_HOLD") ? atoi(getenv("LARECOMP_NPC_HOLD")) : 0;
        if (g_frame - o->up_frame < (uint64_t)hold) continue;
        if (*(const uint64_t *)(uintptr_t)o->nav != NAV_VTABLE) {
            LOG("[npc] mpplayer_%d: its navigator is gone\n", o->k);
            o->state = 0;
            continue;
        }
        if (!o->ctrl) {  // (its controller's setup seen after its navigator's)
            pthread_mutex_lock(&g_lock);
            o->ctrl = ctrl_of(o->nav);
            pthread_mutex_unlock(&g_lock);
        }
        unfix(o);
        if (!g_vec[i]) g_vec[i] = hle_alloc(16);
        float *v = g_vec[i];
        v[0] = o->dest[0], v[1] = o->dest[1], v[2] = o->dest[2], v[3] = 0;
        o->order = 0;
        static int st1 = -1;  // (research LARECOMP_NPC_RUNSTATE=0: never state 1)
        if (st1 < 0) st1 = !(getenv("LARECOMP_NPC_RUNSTATE") && atoi(getenv("LARECOMP_NPC_RUNSTATE")) == 0);
        int want = o->run && st1 ? 1 : 2;
        static int mode = -1;  // (research LARECOMP_NPC_ORDERMODE: bit 0 move state every order, bit 1 no SetIsSteering)
        if (mode < 0) mode = getenv("LARECOMP_NPC_ORDERMODE") ? atoi(getenv("LARECOMP_NPC_ORDERMODE")) : 0;
        if (o->run_set != want || (mode & 1)) o->run_set = want, guest_call_saved(c, NAV_SET_MOVESTATE, o->nav, want, 0, 0, 0);
        uint64_t sx[32], snzcv = c->nzcv;  // (SetDestination's answer: 0 = not taken - no navmesh there, or within its goal tolerance)
        __uint128_t sv[32];
        memcpy(sx, c->x, sizeof sx), memcpy(sv, c->v, sizeof sv);
        c->x[0] = o->nav, c->x[1] = (uint64_t)(uintptr_t)v;
        guest_call(c, NAV_SET_DEST);
        uint32_t ok = (uint32_t)c->x[0] & 1;
        memcpy(c->x, sx, sizeof sx), memcpy(c->v, sv, sizeof sv), c->nzcv = snzcv;
        if (!(mode & 2)) guest_call_saved(c, NAV_SET_STEERING, o->nav, 0, 0, 0, 0);
        static int nlog;
        const float *np = (const float *)(uintptr_t)(o->nav + 0x3e0);  // (where its navigator has it now)
        float dx = v[0] - np[0], dz = v[2] - np[2];
        if (nlog < 12 || !ok)
            nlog++, LOG("[npc] %s_%d sent to %.2f %.2f %.2f (%s, %.1f m from it): %s\n", o->script ? "luanpc" : "mpplayer", o->k, v[0], v[1], v[2], o->run ? "run" : "walk",
                        sqrtf(dx * dx + dz * dz), ok ? "on its way" : "not taken (there already, or no navmesh)");
        if (o->script && !ok) o->refused = 1;
        if (o->script && ok) {  // (the goal as the navigation mesh has it, +0x250: one inside a building was put on its roof -
            const float *pg = (const float *)(uintptr_t)(o->nav + 0x250);  //  and the person climbed up there, 2026-11-19)
            if (fabsf(pg[1] - v[1]) > 2.0f) {
                LOG("[npc] luanpc_%d: the navigation mesh has %.1f %.1f %.1f %.1f m %s the goal - not walked there\n", o->k, pg[0], pg[1], pg[2],
                    fabsf(pg[1] - v[1]), pg[1] > v[1] ? "above" : "below");
                guest_call_saved(c, NAV_STOP, o->nav, 0, 0, 0, 0);
                o->refused = 1;
            }
        }
        if (o->state == 4 && !ok && o->shrink < 3) {  // (walking off: no navmesh that far - a nearer spot the same way)
            o->shrink++, o->order = 1;
            for (int j = 0; j < 3; j += 2) o->dest[j] = np[j] + (o->dest[j] - np[j]) * 0.5f;
        }
    }
}

// pla_world.c's side (its own thread): ask for one, give it orders, let it go
int pla_npc_new(const float at[3], int gender, const uint64_t *outfit) {
    pthread_mutex_lock(&g_lock);
    int r = -1;
    for (int i = 0, pass = 0; pass < 2 && r < 0; i = (i + 1) % NOWN, pass += !i)  // (a free slot; else one given up)
        if (!g_own[i].state || (pass && g_own[i].state == 6)) {
            Own *o = &g_own[i];
            memset(o, 0, sizeof *o);
            o->state = 1, o->gender = gender, o->k = ++g_count, o->level = g_level, o->asked = g_frame;
            if (g_dress > 0 && outfit_any(outfit)) memcpy(o->outfit, outfit, sizeof o->outfit), o->has_outfit = 1;
            memcpy(o->at, at, 12), memcpy(o->dest, at, 12);
            r = i;
        }
    pthread_mutex_unlock(&g_lock);
    return r;
}
void pla_npc_at(int i, float out[3]) { if (i >= 0 && i < NOWN) memcpy(out, g_own[i].at, 12); }
int pla_npc_state(int i) { return i >= 0 && i < NOWN && g_own[i].state != 6 ? g_own[i].state : 0; }
int pla_npc_gender_of(int i) { return i >= 0 && i < NOWN ? g_own[i].gender : -1; }
int pla_npc_puppet(int i) { return i >= 0 && i < NOWN && g_own[i].has_outfit; }  // (dressed, or to be)  // (walked like a player: pla_npc_follow)
int pla_npc_pos(int i, float out[3]) {  // (where a dressed one is: 0 not known)
    if (i < 0 || i >= NOWN || !puppet_ready(&g_own[i])) return 0;
    memcpy(out, (const float *)(uintptr_t)(g_own[i].owner + 0xa0), 12);
    return 1;
}
void pla_npc_follow(int i, const float tgt[5]) {  // (x y z yaw speed: where its player is drawn now)
    if (i < 0 || i >= NOWN) return;
    pthread_mutex_lock(&g_lock);
    memcpy(g_own[i].fo, tgt, 20), g_own[i].fo_set = 1, g_own[i].fo_ns = plat_now_ns();
    pthread_mutex_unlock(&g_lock);
}
void pla_npc_order(int i, const float to[3], int run) {
    if (i < 0 || i >= NOWN || (g_own[i].state != 3 && g_own[i].state != 4)) return;
    Own *o = &g_own[i];
    if (o->has_outfit) {  // (walked like a player: there, walking, facing that way)
        float dx = to[0] - o->fo[0], dz = to[2] - o->fo[2], t[5] = {to[0], to[1], to[2], atan2f(dx, dz), run ? -4.0f : -1.4f};
        pla_npc_follow(i, t);
        return;
    }
    memcpy(o->dest, to, 12), o->run = run;
    o->order = 1, o->shrink = 0;
}
void pla_npc_free(int i) {  // (it walks off where it was sent last: the caller sends it away first)
    if (i < 0 || i >= NOWN) return;
    pthread_mutex_lock(&g_lock);
    g_own[i].riding = g_own[i].swimming = 0;  // (nobody's now: off its mount, out of the water)
    for (int j = 0; j < 8; j++)  // (its mounts removed)
        if (g_own[i].mnt[j] > 0 && g_own[i].mnt[j] <= NMNT) g_mnt[g_own[i].mnt[j] - 1].term = 1;
    if (g_own[i].state == 1) g_own[i].state = 0;  // (not asked yet: forgotten)
    else if (g_own[i].state == 2) g_own[i].state = 5;  // (made, not up yet: kept for later once up)
    else if (g_own[i].state == 3) g_own[i].state = 4;  // (4: up, nobody's)
    pthread_mutex_unlock(&g_lock);
}
// a dressed person can change into this outfit item by item (0: it has to be made again - an item taken off, which the
// game has no call for, or item 11, which is in the dress-up's params)
static bool needs_remake(const uint64_t *worn, const uint64_t *outfit) {
    for (int j = 0; j < 12; j++)
        if (worn[j] && worn[j] != EMPTY_HASH && (!outfit[j] || outfit[j] == EMPTY_HASH)) return true;
    return worn[11] != outfit[11];
}
int pla_npc_take(int gender, const uint64_t *outfit, const float *near) {  // (a free one of that look already up, or -1: the
    // same outfit first, else one dressed the same way (worn by a player / the senior) - re-dressed with theirs; a dressed
    // one only within 10 m of near: it runs there itself)
    pthread_mutex_lock(&g_lock);
    int r = -1, want = g_dress > 0 && outfit_any(outfit);
    for (int pass = 0; pass < 2 && r < 0; pass++)
        for (int i = 0; i < NOWN && r < 0; i++) {
            Own *o = &g_own[i];
            if (o->state != 4 || o->gender != gender || (o->dressed != 0) != want) continue;
            if (pass == 0 && want && memcmp(o->outfit, outfit, sizeof o->outfit)) continue;
            if (want && o->comp && needs_remake(o->worn, outfit)) continue;
            if (want && near && puppet_ready(o)) {
                const float *q = (const float *)(uintptr_t)(o->owner + 0xa0);
                if ((q[0] - near[0]) * (q[0] - near[0]) + (q[2] - near[2]) * (q[2] - near[2]) > 10.0f * 10.0f) continue;
            }
            o->state = 3, r = i;
            if (want && memcmp(o->outfit, outfit, sizeof o->outfit)) memcpy(o->outfit, outfit, sizeof o->outfit), o->redress = 1;
            if (o->nav && *(const uint64_t *)(uintptr_t)o->nav == NAV_VTABLE)  // (where it stands now: its skeleton is looked for there)
                memcpy(o->at, (const void *)(uintptr_t)(o->nav + 0x3e0), 12);
        }
    pthread_mutex_unlock(&g_lock);
    return r;
}
// their outfit heard (again): a dressed one is re-dressed (the game swaps each changed item); 1 when it can't show it
// (made as the senior before their outfit was heard)
int pla_npc_outfit(int i, const uint64_t *outfit) {
    if (i < 0 || i >= NOWN || g_dress <= 0 || !outfit_any(outfit)) return 0;
    pthread_mutex_lock(&g_lock);
    Own *o = &g_own[i];
    int r = 0;
    if (o->state == 1 || ((o->state == 2 || o->state == 5) && !o->modeled) || (o->dressed && !o->comp)) memcpy(o->outfit, outfit, sizeof o->outfit), o->has_outfit = 1;
    else if (o->comp) {
        if (needs_remake(o->worn, outfit)) r = 1;
        else if (memcmp(o->outfit, outfit, sizeof o->outfit)) memcpy(o->outfit, outfit, sizeof o->outfit), o->redress = 1;
    } else if (o->state == 3 || o->state == 4) r = 1;
    pthread_mutex_unlock(&g_lock);
    return r;
}
void pla_npc_unstick(int i, const float at[3]) {  // (drawn from its body a while, its navigator is to walk it again)
    if (i < 0 || i >= NOWN) return;
    pthread_mutex_lock(&g_lock);
    Own *o = &g_own[i];
    if (o->state == 3 || o->state == 4) memcpy(o->dest, at, 12), o->order = 1, o->run_set = 0, o->shrink = 0;
    pthread_mutex_unlock(&g_lock);
}
uint64_t pla_npc_level(void) { return g_level; }
// which area we are in, for the world packets (players in another area aren't shown): the "areaNN" most of the level's
// placements are named after (area00_npc_guard_005, ha_area01_...), FNV-1a 32; 0 not known (no placements yet)
uint32_t pla_npc_area_tag(void) {
    static uint64_t at_level = ~0ull;
    static uint32_t tag;
    static int at_n;
    pthread_mutex_lock(&g_lock);
    if (at_level != g_level || at_n != g_nent) {
        at_level = g_level, at_n = g_nent, tag = 0;
        int best = 0;
        for (int i = 0; i < g_nent && i < 64; i++) {
            const char *a = strstr(g_ent[i].name, "area");
            if (!a || a[4] < '0' || a[4] > '9' || a[5] < '0' || a[5] > '9') continue;
            int cnt = 0;
            for (int j = 0; j < g_nent && j < 64; j++) { const char *b = strstr(g_ent[j].name, "area"); cnt += b && !strncmp(a, b, 6); }
            if (cnt > best) {
                best = cnt, tag = 2166136261u;
                for (int q = 0; q < 6; q++) tag = (tag ^ (uint8_t)a[q]) * 16777619u;
            }
        }
    }
    uint32_t r = tag;
    pthread_mutex_unlock(&g_lock);
    return r;
}
int pla_npc_can_make(void) { return g_mgr && g_nent > 0; }  // (the level has placements to copy)

// research: <live>/npc "make <x> <y> <z> [f]" (one of ours there), "list" (the level's entries)
static void live_poll(void) {
    static const char *live = (const char *)1;
    if (live == (const char *)1) live = getenv("LARECOMP_LIVE");
    if (!live) return;
    char p[1024], l[256];
    snprintf(p, sizeof p, "%s/npc", live);
    FILE *f = fopen(p, "r");
    if (!f) return;
    while (fgets(l, sizeof l, f)) {
        float at[3];
        char g = 0;
        int k = 0, run = 0;
        float yw = 0;
        char o = 0;
        uint64_t mine[12];
        int used = 0;
        char item_name[96];
        if (sscanf(l, "make %f %f %f %c %c%n", &at[0], &at[1], &at[2], &g, &o, &used) >= 3) {  // ("o": our outfit; then <slot>=<item name>...)
            bool fit = o == 'o' && pla_npc_my_outfit(mine);
            char item[96];
            int slot = 0, n = 0;
            for (const char *q = used ? l + used : ""; fit && sscanf(q, " %d=%95s%n", &slot, item, &n) == 2; q += n)
                if (slot >= 0 && slot < 12) mine[slot] = fnv(item);
            pla_npc_new(at, g == 'f', fit ? mine : NULL);
        }
        else if (sscanf(l, "go %d %f %f %f %d", &k, &at[0], &at[1], &at[2], &run) >= 4) pla_npc_order(k, at, run);
        else if (sscanf(l, "par %d %c %95s %f", &k, &o, item_name, &at[0]) == 4 && g_npar < NPAR) {
            pthread_mutex_lock(&g_lock);
            g_par[g_npar].slot = k, g_par[g_npar].type = o, g_par[g_npar].hash = fnv(item_name), g_par[g_npar].f = at[0], g_par[g_npar].i = (uint32_t)at[0], g_par[g_npar].quiet = 0;
            g_npar++;
            pthread_mutex_unlock(&g_lock);
        }
        else if (sscanf(l, "pwarp %d", &k) == 1 && k >= 0 && k < NOWN) g_own[k].warp_now = 1;
        else if (sscanf(l, "seat %d %d %f %f %f %f", &k, &run, &at[0], &at[1], &at[2], &yw) >= 5 && k > 0 && k < 8)
            k_ride[k].mons = run, k_ride[k].seat_y = at[0], k_ride[k].seat_z = at[1], k_ride[k].turn_x = at[2], k_ride[k].mount_y = yw,
            LOG("[npc] ride type %d: Pokemon %d seat %g %g turn %g, mount %g\n", k, run, at[0], at[1], at[2], yw);
        else if (sscanf(l, "ride %d %d %d", &k, &run, &used) == 3 && k >= 0 && k < NOWN) g_own[k].ride_type = run, g_own[k].riding = used;  // (research: riding as type t)
        else if (sscanf(l, "pterm %d", &k) == 1 && k >= 0 && k < NMNT) g_mnt[k].term = 1;  // (research: mount / Pokémon k removed)
        else if (sscanf(l, "poke %d %f %f %f %f", &k, &at[0], &at[1], &at[2], &yw) >= 4) {  // (research: a Pokémon of ours there; [ride type]: its ride model)
            int rt = 0;
            sscanf(l, "poke %*d %*f %*f %*f %*f %d", &rt);
            pthread_mutex_lock(&g_lock);
            for (int i = 0; i < NMNT; i++)
                if (!g_mnt[i].state) {
                    Mnt *m = &g_mnt[i];
                    memset(m, 0, sizeof *m);
                    m->mons = k, memcpy(m->at, at, 12), m->yaw = yw, m->k = ++g_poke_n, m->state = 1, m->level = g_level, m->research = 1, m->t = rt & 7;
                    snprintf(m->name, sizeof m->name, "mppoke_%d", m->k), snprintf(m->joint, sizeof m->joint, "parts");
                    break;
                }
            pthread_mutex_unlock(&g_lock);
        }
        else if (sscanf(l, "mount %d %d %15s", &k, &run, item_name) >= 2 && k >= 0 && k < NOWN && run >= 0 && run < NMNT) {  // (research: person k on mount m)
            g_mnt[run].rider = k + 1, g_mnt[run].attached = 0, g_mnt[run].attach_req = 1;
            if (sscanf(l, "mount %d %d %15s", &k, &run, item_name) == 3) snprintf(g_mnt[run].joint, sizeof g_mnt[run].joint, "%s", item_name);
        }
        else if (sscanf(l, "knob %d %f", &k, &at[0]) == 2 && k >= 0 && k < 8) g_knob[k] = at[0], LOG("[npc] knob %d = %g\n", k, at[0]);
        else if (!strncmp(l, "near", 4)) { extern void pla_world_list_near(float); pla_world_list_near(25); }
        else if (!strncmp(l, "list", 4)) {
            pthread_mutex_lock(&g_lock);
            for (int i = 0; i < g_nent; i++) LOG("[npc]   %s character %016llx%s\n", g_ent[i].name, (unsigned long long)g_ent[i].chr, g_ent[i].made ? " (made)" : "");
            pthread_mutex_unlock(&g_lock);
        }
    }
    fclose(f);
    remove(p);
}

void pla_npc_frame(uint64_t frame) {  // (each game frame: hle/pla_world.c)
    g_frame = frame;
    if (frame % 15 == 0) live_poll();
}

static int g_on = -1;
int pla_npc_enabled(void) {
    if (g_on < 0) {
        const char *e = getenv("LARECOMP_WORLD"), *s = getenv("LARECOMP_GHOST_SELF");
        g_on = (e && atoi(e) > 0) || (s && atof(s) != 0);
    }
    return g_on;
}

static void npc_hooks(void);
__attribute__((constructor)) static void pla_npc_init(void) {
    if (pla_npc_enabled()) npc_hooks();
}
void pla_npc_start(void) {  // (the shared world switched on by the setting: hle/pla_world.c pla_world_settings)
    if (pla_npc_enabled()) return;
    g_on = 1;
    npc_hooks();
}
static void npc_hooks(void) {
    rt_register_fnhook_at(PLACE_NPC, place_hook);
    int no = getenv("LARECOMP_NPC_NOHOOK") ? atoi(getenv("LARECOMP_NPC_NOHOOK")) : 0;
    if (!(no & 2)) rt_register_fnhook_at(FACTORY_CREATE, fcreate_hook);
    rt_register_fnhook_at(NAV_POS_FN, nav_pos_hook);
    rt_register_fnhook_at(SET_NAVIGATOR, setnav_hook);
    if (!(no & 16)) rt_register_fnhook_at(NAV_OUT_OF_TOL, tol_hook);
    if (!(no & 4)) rt_register_fnhook_at(MODEL_BUILD, model_hook);
    g_dress = !getenv("LARECOMP_NPC_DRESS") || atoi(getenv("LARECOMP_NPC_DRESS"));  // (research LARECOMP_NPC_DRESS=0: everyone the senior)
    rt_register_fnhook_at(DRESS_MAKE, dressmake_hook);  // (the player's own, for the log)
    for (int k = 0; k < NMIRROR; k++) g_mirror_hash[k] = fnv(k_mirror[k]);  // (our own state, sent: pla_npc_my_state)
    for (int k = 0; k < NRMIRROR; k++) g_rmirror_hash[k] = fnv(k_rmirror[k]);
    rt_register_fnhook_at(ANIM_SET_FLOAT, myset_f), rt_register_fnhook_at(ANIM_SET_INT, myset_i);
    rt_register_fnhook_at(ANIM_SET_BOOL, myset_b), rt_register_fnhook_at(ANIM_SET_TRIGGER, myset_t);
    rt_register_fnhook_at(PLAYER_CC_CROUCH, crouch_hook), rt_register_fnhook_at(PLAYER_CC_SWIM, swim_hook);
    rt_register_fnhook_at(SAVE_DRESS_SLOT, saveslot_hook);
    try_parse();  // (our outfit is read from the save's dress-up data)
    rt_register_fnhook_at(CHAR_BUILD, build_hook), rt_register_fnhook_at(ANIM_CTOR, actor_hook), rt_register_fnhook_at(NPC_MODEL_CTOR, chr_ctor_hook);
    rt_register_fnhook_at(POKE_ANIM_CTOR, actor_hook), rt_register_fnhook_at(POKE_MODEL_CTOR, poke_ctor_hook);  // (our mounts' parts)
    rt_register_fnhook_at(NPC_ENTITY_CB, entity_hook);
    rt_register_fnhook_at(OWNER_WORLD_FN, world_hook);  // (seated riders drawn on their mount's joint)
    rt_register_fnhook_at(CREATE_MODEL, create_hook), rt_register_fnhook_at(POKE_MODEL_JOB, pjob_hook);
    if (g_dress) rt_register_fnhook_at(NPC_MODEL_JOB, job_hook);
}

// ---- Characters of the mod API's scripts (runtime/modapi.h, hle/modapi.c): people of our own as above (a copy of a
// placement the level made, its name luanpc_<k>, its look the character asked for), walked by their navigator. A
// script's character is a slot that outlives its person: the person is made when the player is in its area (the level
// it was first made in - Arceus' areas each have their own coordinates) within MOD_RANGE of its spot; a level change
// takes the person with it (place_hook) and the slot waits to be made again at its last spot, its walk carried on.
#include "../modapi.h"
#define MAXMOD 64
#define MOD_RANGE 60.0f
typedef struct {
    int used;
    uint32_t gen;
    char model[64], label[32];
    float at[3], yaw;        // (where it is / is to be made; radians)
    uint32_t area;           // (its level's area tag - pla_npc_area_tag; 0: the first one it is made in)
    int own, k, state;       // (its person: g_own index, -1 none; MODNPC_WAITING / COMING / UP)
    uint64_t asked_frame, up_frame;
    int walking, run, order; // (on its way to goal; order: a walk to give its navigator)
    float goal[3];
    int set, face_n;         // (set: put at set_at at once; face_n: its facing given a few more times - see below)
    float set_at[3];
    float last[3];
    uint64_t moved_frame;
    int misses, told;
    uint64_t sk;             // (its skeleton: where it is drawn - hle/pla_world.c)
    uint64_t asked_sk_frame; // (pla_world.c's frame when it was asked for: its skeleton is first posed after that)
    int stop;                // (its walk to stop: its navigator stopped, where it stands)
    int retried;             // (its walk's order given again after getting nowhere)
    int steer;               // (its navigator's root motion stopped giving it any way: steered here instead - below)
    uint64_t order_frame;    // (its last order)
    char outfit[256];        // (what it wears - npc_outfit's spec; "" its look's own)
    int told_outfit;
    struct { uint64_t hash; char kind; float v; } anim[8];  // (animation parameters to set - npc_anim)
    int nanim;
    int talk;                // (the game's Talk prompt over it - npc_talk, below)
} ModNpc;
static ModNpc g_mod[MAXMOD];
static volatile int g_mods_any;
static pthread_mutex_t g_mlock = PTHREAD_MUTEX_INITIALIZER;
static struct { char name[24]; int own, k; uint64_t level; } g_term[16];  // (removed: terminated from the camera update)
static int g_nterm;
static volatile uint64_t g_area_id;
static volatile int g_area_burst;

static ModNpc *mod_of(int h) {
    int i = (h & 0xff) - 1;
    if (h <= 0 || i < 0 || i >= MAXMOD) return NULL;
    ModNpc *m = &g_mod[i];
    return m->used && (m->gen & 0x3fffff) == ((uint32_t)h >> 8) ? m : NULL;
}
static uint64_t mod_chr(const char *model, int *sent) {  // (what it looks like: a hash, a placement's look, a character id)
    *sent = 0;
    if (!model || !*model) return 0;
    if (model[0] == '0' && (model[1] == 'x' || model[1] == 'X')) return strtoull(model, NULL, 16);
    for (int i = 0; i < g_nent; i++)
        if (!strcmp(g_ent[i].name, model)) { *sent = i + 1; return g_ent[i].chr; }
    return fnv(model);
}
// what it wears: "player" (our outfit), "<slot>=<item>" over ours, "body=p1|p2" - false while ours isn't known
static bool mod_outfit_items(const char *spec, uint64_t items[12], int *body) {
    if (!pla_npc_my_outfit(items)) return false;
    char buf[256];
    snprintf(buf, sizeof buf, "%s", spec);
    for (char *t = strtok(buf, ", "); t; t = strtok(NULL, ", ")) {
        int slot;
        char item[96];
        if (!strcmp(t, "body=p1")) *body = 0;
        else if (!strcmp(t, "body=p2")) *body = 1;
        else if (sscanf(t, "%d=%95s", &slot, item) == 2 && slot >= 0 && slot < 12) items[slot] = fnv(item);
    }
    return true;
}
// its animation's parameters asked for (npc_anim), set now (the game thread)
static void mod_anims(Cpu *c, ModNpc *m, Own *o) {
    for (int k = 0; k < m->nanim; k++) {
        uint64_t fn = m->anim[k].kind == 'f' ? ANIM_SET_FLOAT : m->anim[k].kind == 'i' ? ANIM_SET_INT : m->anim[k].kind == 'b' ? ANIM_SET_BOOL : ANIM_SET_TRIGGER;
        anim_set(c, o->actl, fn, m->anim[k].hash, m->anim[k].v, (uint32_t)(int32_t)m->anim[k].v);
    }
    m->nanim = 0;
}
// a dressed one (an outfit): walked as the shared world's players' people are (puppet_step - pla_npc_cam right after
// this): its target where the script sends it, a negative speed = walk there and stand; where it is: its model owner
static void mod_puppet(Cpu *c, ModNpc *m, Own *o) {
    if (!puppet_ready(o)) return;
    const float *wp = (const float *)(uintptr_t)(o->owner + 0xa0), *wz = (const float *)(uintptr_t)(o->owner + 0x90);
    memcpy(m->at, wp, 12), memcpy(o->cur, wp, 12), o->cur_ok = 1;
    float asked_yaw = m->yaw;  // (until it stands: the way it was asked to face)
    if (o->fo_set && wz[0] * wz[0] + wz[2] * wz[2] > 0.01f) m->yaw = atan2f(wz[0], wz[2]);  // (as drawn)
    mod_anims(c, m, o);
    if (!o->fo_set) {  // (up: standing where it was made, facing its way)
        float t[5] = {m->at[0], m->at[1], m->at[2], asked_yaw, -0.01f};
        memcpy(o->fo, t, 20), o->fo_set = 1, o->fo_ns = plat_now_ns();
    }
    if (m->set) {
        m->set = 0, m->walking = 0, m->order = 0;
        cc_warp(c, o->cc, m->set_at, m->yaw);
        float t[5] = {m->set_at[0], m->set_at[1], m->set_at[2], m->yaw, -0.01f};
        memcpy(o->fo, t, 20), memcpy(m->at, m->set_at, 12);
    }
    if (m->stop) {
        m->stop = 0, m->walking = 0;
        float t[5] = {m->at[0], m->at[1], m->at[2], m->yaw, -0.01f};
        memcpy(o->fo, t, 20);
    }
    if (m->order) {
        m->order = 0, m->walking = 1, m->order_frame = g_frame;
        float dx = m->goal[0] - m->at[0], dz = m->goal[2] - m->at[2];
        float t[5] = {m->goal[0], m->goal[1], m->goal[2], atan2f(dx, dz), m->run ? -4.0f : -1.4f};
        memcpy(o->fo, t, 20), o->fo_ns = plat_now_ns();
    } else if (m->walking) {
        float gx = m->goal[0] - m->at[0], gz = m->goal[2] - m->at[2];
        if (gx * gx + gz * gz < 0.6f * 0.6f) m->walking = 0;  // (there: it stands, facing the way it came)
        else if (g_frame - m->order_frame > 30 * 60) {  // (a minute and not there: stops)
            LOG("[npc] luanpc_%d (%s) didn't get to %.1f %.1f %.1f in a minute: stops\n", m->k, m->label, m->goal[0], m->goal[1], m->goal[2]);
            m->walking = 0;
            float t[5] = {m->at[0], m->at[1], m->at[2], m->yaw, -0.01f};
            memcpy(o->fo, t, 20);
        }
    } else if (m->face_n > 0) {  // (turned: its target's way)
        m->face_n = 0, o->fo[3] = m->yaw;
    }
}

// (the camera update, the game thread, twice a frame; research LARECOMP_MODNPC_STATS=1: its cost logged every 20 s)
static void mod_cam_(Cpu *c);
static void mod_cam(Cpu *c) {
    static int stats = -1;
    static uint64_t ns, calls, at;
    if (stats < 0) stats = getenv("LARECOMP_MODNPC_STATS") != NULL;
    if (!stats) { mod_cam_(c); return; }
    uint64_t t0 = plat_now_ns();
    mod_cam_(c);
    ns += plat_now_ns() - t0, calls++;
    if (!at) at = g_frame;
    if (g_frame - at >= 600) LOG("[npc] cost: scripts' characters %.2f us a frame (%llu calls in %llu frames)\n", (double)ns / (double)(g_frame - at) / 1000.0,
                                (unsigned long long)calls, (unsigned long long)(g_frame - at)), ns = calls = 0, at = g_frame;
}
static void mod_cam_(Cpu *c) {
    if (g_nterm) {  // (removed ones: TerminateObject by name - as the mounts above)
        static uint64_t *h;
        if (!h) h = hle_alloc(16);
        pthread_mutex_lock(&g_mlock);
        int n = g_nterm;
        g_nterm = 0;
        for (int i = 0; i < n; i++) {
            if (g_term[i].level != g_level) continue;  // (went with its level already)
            uint64_t fo = gcall(c, FO_GET, 0, 0, 0, 0);
            if (in_heap(fo)) h[0] = fnv(g_term[i].name), gcall(c, FO_TERMINATE, fo, (uint64_t)(uintptr_t)h, 0, 0);
            pthread_mutex_lock(&g_lock);
            Own *o = &g_own[g_term[i].own];
            if (o->script && o->k == g_term[i].k) memset(o, 0, sizeof *o);
            pthread_mutex_unlock(&g_lock);
            LOG("[npc] %s removed (its script)\n", g_term[i].name);
        }
        pthread_mutex_unlock(&g_mlock);
    }
    if (!g_mods_any) return;
    extern bool warp_player_now(float pos[3], float *yaw);
    float me[3];
    bool have_me = warp_player_now(me, NULL);
    uint32_t tag = pla_npc_area_tag();
    pthread_mutex_lock(&g_mlock);
    for (int i = 0; i < MAXMOD; i++) {
        ModNpc *m = &g_mod[i];
        if (!m->used) continue;
        Own *o = m->own >= 0 ? &g_own[m->own] : NULL;
        if (o && (!o->script || o->k != m->k || !o->state)) {  // (its person went with its level)
            if (m->state == MODNPC_COMING) LOG("[npc] luanpc_%d (%s) wasn't made (refused): asked again later\n", m->k, m->label), m->misses++;
            else LOG("[npc] luanpc_%d (%s) went with its level: waits at %.1f %.1f %.1f\n", m->k, m->label, m->at[0], m->at[1], m->at[2]);
            m->own = -1, o = NULL, m->state = MODNPC_WAITING, m->order = m->walking, m->sk = 0;  // (its walk given again once it is back)
        }
        if (m->state == MODNPC_WAITING) {
            if (g_frame % 15 || !have_me || !tag || (m->area && m->area != tag) || !pla_npc_can_make()) continue;
            float dx = m->at[0] - me[0], dz = m->at[2] - me[2];
            if (dx * dx + dz * dz > MOD_RANGE * MOD_RANGE) continue;
            if (m->misses && g_frame - m->asked_frame < (uint64_t)(m->misses < 5 ? m->misses : 5) * 120) continue;
            uint64_t items[12];
            int body = -1;
            if (m->outfit[0] && !mod_outfit_items(m->outfit, items, &body)) {
                if (!m->told_outfit++) LOG("[npc] %s waits: the player's outfit isn't known yet (no save loaded)\n", m->label);
                continue;
            }
            pthread_mutex_lock(&g_lock);
            int sent = 0, slot = -1;
            uint64_t chr = mod_chr(m->model, &sent);
            for (int j = NOWN - 1; j >= 0 && slot < 0; j--) if (!g_own[j].state) slot = j;  // (from the top: the shared world's
                                                                                           //  players take theirs from the bottom)
            if (slot >= 0) {
                Own *n = &g_own[slot];
                memset(n, 0, sizeof *n);
                n->state = 1, n->script = 1, n->schr = chr, n->sent = sent, n->k = ++g_count, n->level = g_level, n->asked = g_frame;
                if (m->outfit[0]) {  // (dressed: made as the senior of that body - the model job swaps in the player's - see create_hook)
                    n->gender = body >= 0 ? body : g_gender > 0, n->schr = n->gender ? CHR_SENPAI_F : CHR_SENPAI_M, n->sent = 0;
                    memcpy(n->outfit, items, sizeof n->outfit), n->has_outfit = 1;
                }
                memcpy(n->at, m->at, 12), memcpy(n->dest, m->at, 12);
                extern uint64_t pla_world_frame_now(void);
                m->own = slot, m->k = n->k, m->state = MODNPC_COMING, m->asked_frame = g_frame, m->asked_sk_frame = pla_world_frame_now();
                if (!m->area) m->area = tag;
            }
            pthread_mutex_unlock(&g_lock);
            if (slot < 0 && !m->told++) LOG("[npc] %s waits: every person slot is in use\n", m->label);
            continue;
        }
        if (m->state == MODNPC_COMING) {
            if (o && o->state == 3) {
                m->state = MODNPC_UP, m->up_frame = g_frame, m->face_n = 20, m->misses = 0;
                if (o->nav && *(const uint64_t *)(uintptr_t)o->nav == NAV_VTABLE) memcpy(m->at, (const float *)(uintptr_t)(o->nav + 0x3e0), 12);
                memcpy(m->last, m->at, 12), m->moved_frame = g_frame;
                LOG("[npc] luanpc_%d (%s) is up (physics character %llx)\n", m->k, m->label, (unsigned long long)o->cc);
            } else if (!o || o->state == 6 || g_frame - m->asked_frame > 1200) {
                LOG("[npc] luanpc_%d (%s) wasn't made: asked again later\n", m->k, m->label);
                if (o) { pthread_mutex_lock(&g_lock); memset(o, 0, sizeof *o); pthread_mutex_unlock(&g_lock); }
                m->own = -1, m->state = MODNPC_WAITING, m->misses++;
            }
            continue;
        }
        if (o && o->has_outfit) { mod_puppet(c, m, o); continue; }  // (dressed: walked as a player - below)
        // up: where it is drawn - its skeleton as posed (hle/pla_world.c: the person-sized one first posed at its spot after
        // it was asked for); its facing: the way it moved. (The navigator's +0x3e0 is its goal; its model owner's world
        // translation a cache nothing keeps up for people the navigator walks; the body objects at its spot other things' -
        // all 2026-11-19. Its owner's world getter answers the same as the skeleton, but catching the owner through the
        // entity callback left new people undrawn.)
        if (!o || !o->nav || *(const uint64_t *)(uintptr_t)o->nav != NAV_VTABLE) continue;
        extern uint64_t pla_world_skel_near(const float p[3], float r, unsigned minbones, uint64_t since, const uint64_t *skip, int nskip);
        extern int pla_world_skel_pos(uint64_t sk, float out[3]);
        float sp[3];
        if (m->sk && !pla_world_skel_pos(m->sk, sp)) m->sk = 0;
        if (!m->sk) {
            uint64_t taken[MAXMOD];
            int nt = 0;
            for (int j = 0; j < MAXMOD; j++) if (g_mod[j].used && g_mod[j].sk) taken[nt++] = g_mod[j].sk;
            if ((m->sk = pla_world_skel_near(m->at, 2.0f, 100, m->asked_sk_frame, taken, nt)))
                LOG("[npc] luanpc_%d (%s): its skeleton %llx\n", m->k, m->label, (unsigned long long)m->sk);
        }
        if (m->sk && pla_world_skel_pos(m->sk, sp)) {
            float mx = sp[0] - m->at[0], mz = sp[2] - m->at[2];
            if (mx * mx + mz * mz > 0.0025f) m->yaw = atan2f(mx, mz);
            memcpy(m->at, sp, 12), memcpy(o->cur, sp, 12), o->cur_ok = 1;
        }
        mod_anims(c, m, o);
        if (o->refused) {  // (its walk's goal has no navigation mesh: it stands)
            o->refused = 0;
            if (m->walking) LOG("[npc] luanpc_%d (%s): no way to %.1f %.1f %.1f on the navigation mesh - stops\n", m->k, m->label, m->goal[0], m->goal[1], m->goal[2]);
            m->walking = 0;
        }
        if (m->set) {  // (put somewhere: its physics character warped there, its navigator stopped)
            m->set = 0, m->walking = 0, m->order = 0;
            if (o->cc && *(const uint64_t *)(uintptr_t)o->cc == CC_VTABLE) cc_warp(c, o->cc, m->set_at, m->yaw);
            guest_call_saved(c, NAV_STOP, o->nav, 0, 0, 0, 0);
            memcpy(m->at, m->set_at, 12), m->face_n = 20;
        }
        bool settling = g_frame - m->up_frame < 30;  // (an order given as it comes up comes to nothing: a second later)
        if (m->order && !settling) {  // (a walk: its navigator's order - given by pla_npc_cam right after this)
            if (!m->walking) m->moved_frame = g_frame, memcpy(m->last, m->at, 12), m->steer = 0;  // (a new walk: progress measured from here)
            m->order = 0, m->walking = 1, m->order_frame = g_frame;
            memcpy(o->dest, m->goal, 12), o->run = m->run, o->order = 1, o->shrink = 0;
            o->run_set = 0;  // (its move state given with every order: after a walk's end it stood and took no more - 2026-11-19)
        } else if (m->stop) {  // (stopped: its navigator, where it stands)
            m->stop = 0, m->walking = 0;
            guest_call_saved(c, NAV_STOP, o->nav, 0, 0, 0, 0);
        } else if (m->walking && m->sk && m->steer && o->cc && *(const uint64_t *)(uintptr_t)o->cc == CC_VTABLE) {
            // (steered: its physics character given the way to its goal each frame - its navigator still plays its walk.
            // After a walk had ended, a villager's next legs played the walk on the spot: the navigator's mover
            // 0x700172b964 found no root motion in its source (+0x170 of 0x700172ccc4's, zero on every later leg - a
            // fresh one's first leg had 1.6 m/s) and set the velocity to zero (0x700172bdf0, vs 0x700172bebc moving).
            // Lua-traced 2026-11-19. A straight line, against the world's collision; the goal on the navigation mesh)
            float gx = m->goal[0] - m->at[0], gz = m->goal[2] - m->at[2], gd = sqrtf(gx * gx + gz * gz);
            if (gd < 0.6f || g_frame - m->moved_frame > 90) {
                m->walking = 0, m->steer = 0;
                guest_call_saved(c, NAV_STOP, o->nav, 0, 0, 0, 0);
                if (!g_vel2) g_vel2 = hle_alloc(16);
                g_vel2[0] = g_vel2[1] = g_vel2[2] = g_vel2[3] = 0;
                guest_call_saved(c, CC_SET_VELOCITY, o->cc, (uint64_t)(uintptr_t)g_vel2, 0, 0, 0);
                if (gd >= 0.6f) LOG("[npc] luanpc_%d (%s) can't get to %.1f %.1f %.1f (steered, stuck %.1f m off): stops\n", m->k, m->label, m->goal[0], m->goal[1], m->goal[2], gd);
            } else {
                float lx = m->at[0] - m->last[0], lz = m->at[2] - m->last[2];
                if (lx * lx + lz * lz > 0.3f * 0.3f) memcpy(m->last, m->at, 12), m->moved_frame = g_frame;
                float v = m->run ? 4.2f : 1.4f;
                if (!g_vel2) g_vel2 = hle_alloc(16);
                g_vel2[0] = gx / gd * v, g_vel2[1] = 0, g_vel2[2] = gz / gd * v, g_vel2[3] = 0;
                guest_call_saved(c, CC_SET_VELOCITY, o->cc, (uint64_t)(uintptr_t)g_vel2, 0, 0, 0);
                if (!g_fwd_vec) g_fwd_vec = hle_alloc(16);
                g_fwd_vec[0] = gx / gd, g_fwd_vec[1] = 0, g_fwd_vec[2] = gz / gd, g_fwd_vec[3] = 0;
                guest_call_saved(c, CC_SET_FORWARD, o->cc, (uint64_t)(uintptr_t)g_fwd_vec, 0, 0, 0);
                if (g_frame - m->order_frame >= 15) m->order = 1;  // (its walk kept playing)
            }
        } else if (m->walking && m->sk) {
            float gx = m->goal[0] - m->at[0], gz = m->goal[2] - m->at[2];
            float lx = m->at[0] - m->last[0], lz = m->at[2] - m->last[2];
            if (gx * gx + gz * gz < 0.8f * 0.8f) m->walking = 0;  // (there)
            else if (g_frame - m->moved_frame > 20 && g_frame - m->order_frame < 15 && lx * lx + lz * lz < 0.25f * 0.25f && !m->steer) {
                m->steer = 1, m->moved_frame = g_frame;  // (ordered, walking, not getting on: steered from here - above)
                static int nlog;
                if (nlog++ < 8) LOG("[npc] luanpc_%d (%s): its walk doesn't move it - steered\n", m->k, m->label);
            }
            else if (g_frame - m->order_frame >= 15) m->order = 1;  // (its order again twice a second, as the shared world's
                                                                    //  persons get theirs: one given once was walked a leg, the
                                                                    //  next ones taken but not walked - 2026-11-19)
            else if (lx * lx + lz * lz > 0.3f * 0.3f) memcpy(m->last, m->at, 12), m->moved_frame = g_frame, m->retried = 0;
            else if (g_frame - m->moved_frame > 45 && m->retried < 4) {  // (not getting anywhere for 1.5 s: its order again - the
                m->retried++, m->order = 1;                                 //  navigator drops one now and then)
            } else if (g_frame - m->moved_frame > 45) {  // (after 4 more: no way there on the navigation mesh)
                LOG("[npc] luanpc_%d (%s) can't get to %.1f %.1f %.1f (stuck %.1f m off): stops\n", m->k, m->label, m->goal[0], m->goal[1], m->goal[2],
                    sqrtf(gx * gx + gz * gz));
                m->walking = 0;
                guest_call_saved(c, NAV_STOP, o->nav, 0, 0, 0, 0);
            }
        } else if (m->walking && !m->sk && g_frame - m->order_frame > 240) {
            m->walking = 0;  // (its skeleton not found - where it is isn't known: its walk taken as done after 8 s)
        } else if (m->face_n > 0 && o->cc && *(const uint64_t *)(uintptr_t)o->cc == CC_VTABLE) {
            // (its facing: given for a few frames - its controller updates every 2nd frame and a turn given between two
            // is overwritten - see puppet_step)
            m->face_n--;
            if (!g_fwd_vec) g_fwd_vec = hle_alloc(16);
            g_fwd_vec[0] = sinf(m->yaw), g_fwd_vec[1] = 0, g_fwd_vec[2] = cosf(m->yaw), g_fwd_vec[3] = 0;
            guest_call_saved(c, CC_SET_FORWARD, o->cc, (uint64_t)(uintptr_t)g_fwd_vec, 0, 0, 0);
        }
    }
    pthread_mutex_unlock(&g_mlock);
}
// (each frame, the render thread: warp.c) a level's people all made -> an area loaded
void pla_npc_api_frame(uint64_t frame) {
    g_frame = frame;
    extern void pla_world_api_frame(uint64_t);
    pla_world_api_frame(frame);
    if (g_place_seen_ns && !g_area_burst && plat_now_ns() - g_place_seen_ns < 1000000000ull) g_area_burst = 1;
    if (g_area_burst && plat_now_ns() - g_place_seen_ns > 1500000000ull) g_area_burst = 0, g_area_id++, LOG("[npc] area loaded (%llu)\n", (unsigned long long)g_area_id);
}

// ---- the API (hle/modapi.c)
static int g_api;
// ---- The game's own Talk prompt on a script's character (npc_talk; traced 2026-11-19). The player's interaction
// update 0x70018648bc follows the object it targets ([[ctrl+0xc8]+0x1f0], a weak ref; the object's kind at +0x220: 2 a
// person, 14 a tent...) and for a person asks the event system whether a talk trigger (bin/event/event_progress/
// trigger: where {kind npc_talk, name}) holds for it: TRIG_QUERY(&kind, the person's name hash [obj+0x1f8], 3 x the
// empty hash, a callback set) - true puts "A <its character's name>" over it. A then fires that trigger: TRIG_FIRE
// (&kind, name, ...) (its answer unused). Ours (luanpc_<k>) have no trigger of the game's: a talkable one answers true
// here, and its fire is taken - the game does nothing more (no event; the player stays free) - and handed to the script
// (npc_talked). The game's own triggers and people are untouched.
#define TRIG_QUERY 0x70015698d0ull
#define TRIG_FIRE 0x70015693c0ull
#define KIND_NPC_TALK 0xab00e1a6f5d27687ull
static int g_talked[16], g_ntalked;
static int talk_handle(uint64_t name) {  // (the talkable script character with that name hash; 0 none - g_mlock held)
    char nm[24];
    for (int i = 0; i < MAXMOD; i++) {
        ModNpc *m = &g_mod[i];
        if (!m->used || !m->talk || m->state < MODNPC_UP) continue;
        snprintf(nm, sizeof nm, "luanpc_%d", m->k);
        if (fnv(nm) == name) return (int)((m->gen & 0x3fffff) << 8 | (uint32_t)(i + 1));
    }
    return 0;
}
static int talkq_hook(Cpu *c, uint64_t addr) {
    (void)addr;
    if (!guest_ptr(c->x[0]) || *(const uint64_t *)(uintptr_t)c->x[0] != KIND_NPC_TALK || !g_mods_any) return 0;
    pthread_mutex_lock(&g_mlock);
    int h = talk_handle(c->x[1]);
    pthread_mutex_unlock(&g_mlock);
    if (!h) return 0;
    c->x[0] = 1;
    return 1;
}
static int talkf_hook(Cpu *c, uint64_t addr) {
    (void)addr;
    if (!guest_ptr(c->x[0]) || *(const uint64_t *)(uintptr_t)c->x[0] != KIND_NPC_TALK || !g_mods_any) return 0;
    pthread_mutex_lock(&g_mlock);
    int h = talk_handle(c->x[1]);
    if (h && g_ntalked < 16) g_talked[g_ntalked++] = h;
    pthread_mutex_unlock(&g_mlock);
    if (!h) return 0;
    LOG("[npc] the player talked to script character %d (the game's Talk prompt)\n", h);
    c->x[0] = 0;
    return 1;
}
// h > 0: its prompt on (on 1) or off; h 0: 1 when this game has the prompt
int pla_npc_mod_talk(int h, int on) {
    if (h == 0) return 1;
    pthread_mutex_lock(&g_mlock);
    ModNpc *m = mod_of(h);
    if (m) m->talk = on != 0;
    pthread_mutex_unlock(&g_mlock);
    return m != NULL;
}
int pla_npc_mod_talked(void) {
    pthread_mutex_lock(&g_mlock);
    int h = 0;
    if (g_ntalked > 0) {
        h = g_talked[0];
        memmove(g_talked, g_talked + 1, (size_t)--g_ntalked * sizeof g_talked[0]);
    }
    pthread_mutex_unlock(&g_mlock);
    return h;
}

void pla_npc_api_on(void) {
    if (g_api) return;
    g_api = 1;
    rt_register_fnhook_at(TRIG_QUERY, talkq_hook), rt_register_fnhook_at(TRIG_FIRE, talkf_hook);
    rt_register_fnhook_at(PLACE_NPC, place_hook);
    rt_register_fnhook_at(FACTORY_CREATE, fcreate_hook);
    rt_register_fnhook_at(NAV_POS_FN, nav_pos_hook);
    rt_register_fnhook_at(SET_NAVIGATOR, setnav_hook);
    rt_register_fnhook_at(NAV_OUT_OF_TOL, tol_hook);
    rt_register_fnhook_at(CHAR_BUILD, build_hook), rt_register_fnhook_at(ANIM_CTOR, actor_hook), rt_register_fnhook_at(NPC_MODEL_CTOR, chr_ctor_hook);
    // (dressed ones - npc_outfit: the shared world's dressing, as made - and Pokémon of scripts' own: their parts)
    if (g_dress < 0) g_dress = !getenv("LARECOMP_NPC_DRESS") || atoi(getenv("LARECOMP_NPC_DRESS"));
    rt_register_fnhook_at(MODEL_BUILD, model_hook), rt_register_fnhook_at(DRESS_MAKE, dressmake_hook);
    rt_register_fnhook_at(SAVE_DRESS_SLOT, saveslot_hook), rt_register_fnhook_at(NPC_ENTITY_CB, entity_hook);
    rt_register_fnhook_at(CREATE_MODEL, create_hook);
    if (g_dress) rt_register_fnhook_at(NPC_MODEL_JOB, job_hook);
    rt_register_fnhook_at(POKE_ANIM_CTOR, actor_hook), rt_register_fnhook_at(POKE_MODEL_CTOR, poke_ctor_hook);
    extern void warp_api_start(void), pla_world_track_skeletons(void);
    pla_world_track_skeletons();  // (where each is drawn: its posed skeleton)
    warp_api_start();
    LOG("[npc] on: people of scripts' own\n");
}
int pla_npc_mod_spawn(const char *model, const float pos[3], float yaw_deg, const char *label) {
    pthread_mutex_lock(&g_mlock);
    int h = 0;
    for (int i = 0; i < MAXMOD && !h; i++) {
        ModNpc *m = &g_mod[i];
        if (m->used) continue;
        uint32_t gen = m->gen + 1;
        memset(m, 0, sizeof *m);
        m->used = 1, m->gen = gen, m->own = -1, m->state = MODNPC_WAITING;
        snprintf(m->model, sizeof m->model, "%s", model ? model : "");
        snprintf(m->label, sizeof m->label, "%s", label && *label ? label : "npc");
        memcpy(m->at, pos, 12), m->yaw = yaw_deg * (float)M_PI / 180.0f;
        h = (int)((gen & 0x3fffff) << 8 | (uint32_t)(i + 1));
        g_mods_any = 1;
    }
    pthread_mutex_unlock(&g_mlock);
    if (!h) LOG("[npc] scripts: no free slot (%d in use)\n", MAXMOD);
    return h;
}
void pla_npc_mod_remove(int h) {
    pthread_mutex_lock(&g_mlock);
    ModNpc *m = mod_of(h);
    if (m) {
        if (m->own >= 0 && g_nterm < 16) {
            snprintf(g_term[g_nterm].name, sizeof g_term[g_nterm].name, "luanpc_%d", m->k);
            g_term[g_nterm].own = m->own, g_term[g_nterm].k = m->k, g_term[g_nterm].level = g_level, g_nterm++;
        }
        uint32_t gen = m->gen;
        memset(m, 0, sizeof *m);
        m->gen = gen;
    }
    pthread_mutex_unlock(&g_mlock);
}
void pla_npc_mod_remove_all(void) {
    for (int i = 0; i < MAXMOD; i++)
        if (g_mod[i].used) pla_npc_mod_remove((int)((g_mod[i].gen & 0x3fffff) << 8 | (uint32_t)(i + 1)));
}
void pla_npc_mod_walk(int h, const float p[3], int run) {
    pthread_mutex_lock(&g_mlock);
    ModNpc *m = mod_of(h);
    if (m) memcpy(m->goal, p, 12), m->run = run, m->order = 1, m->walking = 0, m->retried = 0;  // (walking once its order is given)
    pthread_mutex_unlock(&g_mlock);
}
void pla_npc_mod_stop(int h) {
    pthread_mutex_lock(&g_mlock);
    ModNpc *m = mod_of(h);
    if (m && (m->walking || m->order)) m->stop = m->state == MODNPC_UP, m->walking = 0, m->order = 0;
    pthread_mutex_unlock(&g_mlock);
}
void pla_npc_mod_set(int h, const float p[3], float yaw_deg) {
    pthread_mutex_lock(&g_mlock);
    ModNpc *m = mod_of(h);
    if (m) {
        if (yaw_deg < 1e8f) m->yaw = yaw_deg * (float)M_PI / 180.0f;
        memcpy(m->set_at, p, 12), m->walking = 0, m->order = 0;
        if (m->state == MODNPC_UP) m->set = 1;
        else memcpy(m->at, p, 12);  // (not made yet: made there)
        m->face_n = 20;
    }
    pthread_mutex_unlock(&g_mlock);
}
int pla_npc_mod_state(int h, float p[3], float *yaw) {
    pthread_mutex_lock(&g_mlock);
    ModNpc *m = mod_of(h);
    int st = 0;
    if (m) {
        st = m->state == MODNPC_UP && (m->walking || m->order) ? MODNPC_WALKING : m->state;  // (an order not given yet: walking already)
        if (p) memcpy(p, m->at, 12);
        if (yaw) *yaw = m->yaw * 57.29578f;
    }
    pthread_mutex_unlock(&g_mlock);
    return st;
}
int pla_npc_mod_models(char *out, int cap) {
    int n = snprintf(out, (size_t)cap, "any character id (\"tr0036_00_senpai_m\") or hash (\"0x...\"), or a placement of this level for its look:\n");
    pthread_mutex_lock(&g_lock);
    for (int i = 0; i < g_nent && n < cap; i++) {
        char line[160];
        int l = snprintf(line, sizeof line, "%s - character %016llx%s\n", g_ent[i].name, (unsigned long long)g_ent[i].chr, g_ent[i].made ? "" : " (not made here now)");
        if (n + l >= cap) break;
        memcpy(out + n, line, (size_t)l + 1), n += l;
    }
    pthread_mutex_unlock(&g_lock);
    return n < cap ? n : cap - 1;
}
uint64_t pla_npc_area(char *name, int cap) {  // (the "areaNN" most of the level's placements are named after)
    if (name && cap > 0) {
        name[0] = 0;
        pthread_mutex_lock(&g_lock);
        int best = 0;
        for (int i = 0; i < g_nent && i < 64; i++) {
            const char *a = strstr(g_ent[i].name, "area");
            if (!a || a[4] < '0' || a[4] > '9' || a[5] < '0' || a[5] > '9') continue;
            int cnt = 0;
            for (int j = 0; j < g_nent && j < 64; j++) { const char *b = strstr(g_ent[j].name, "area"); cnt += b && !strncmp(a, b, 6); }
            if (cnt > best) best = cnt, snprintf(name, (size_t)cap, "%.6s", a);
        }
        pthread_mutex_unlock(&g_lock);
    }
    return g_area_id;
}

int pla_npc_mod_outfit(int h, const char *spec) {
    pthread_mutex_lock(&g_mlock);
    ModNpc *m = mod_of(h);
    int ok = m && m->state == MODNPC_WAITING && spec && *spec && g_dress != 0;
    if (ok) snprintf(m->outfit, sizeof m->outfit, "%s", spec);
    pthread_mutex_unlock(&g_mlock);
    return ok;
}
int pla_npc_mod_anim(int h, const char *name, int kind, float value) {
    pthread_mutex_lock(&g_mlock);
    ModNpc *m = mod_of(h);
    int ok = m && m->nanim < 8;
    if (ok) m->anim[m->nanim].hash = fnv(name), m->anim[m->nanim].kind = (char)kind, m->anim[m->nanim].v = value, m->nanim++;
    pthread_mutex_unlock(&g_mlock);
    return ok;
}
// Pokémon of scripts' own: made as the mounts above are (poke_make from the camera update, its navigator matched within
// 4 m), named luapoke_<k>; removed by TerminateObject; gone with its level
int pla_npc_poke_spawn(int species, const float pos[3], float yaw_deg, int form, int shiny, const char *label) {
    (void)label;
    if (species <= 0 || species > 1010) return 0;
    pthread_mutex_lock(&g_lock);
    int h = 0;
    for (int i = 0; i < NMNT && !h; i++)
        if (!g_mnt[i].state) {
            Mnt *m = &g_mnt[i];
            memset(m, 0, sizeof *m);
            m->mons = species, memcpy(m->at, pos, 12), m->yaw = yaw_deg, m->k = ++g_poke_n, m->state = 1, m->level = g_level;
            m->research = 1, m->script = 1, m->form = form, m->rare = shiny;
            snprintf(m->name, sizeof m->name, "luapoke_%d", m->k), snprintf(m->joint, sizeof m->joint, "parts");
            h = (m->k << 8) | (i + 1);
        }
    pthread_mutex_unlock(&g_lock);
    if (!h) LOG("[npc] scripts: no free Pokemon slot (%d in use)\n", NMNT);
    return h;
}
static Mnt *poke_of(int h) {
    int i = (h & 0xff) - 1;
    if (h <= 0 || i < 0 || i >= NMNT || !g_mnt[i].state || !g_mnt[i].script || g_mnt[i].k != (h >> 8)) return NULL;
    return &g_mnt[i];
}
void pla_npc_poke_remove(int h) {
    pthread_mutex_lock(&g_lock);
    Mnt *m = poke_of(h);
    if (m) m->term = 1;
    pthread_mutex_unlock(&g_lock);
}
// an animation parameter of a script's Pokémon by its name (luapoke_<k>: the mod API's object_anim), set at the next
// camera update through its animation component (vtable 0x7004147c18 - pm<n>_base.tracp's parameters: Pokémon play a
// one-shot with one_emotion_int + one_emotion_trigger, idle_emotion_int + idle_emotion_trigger - Studio motionstates)
int pla_npc_object_anim(const char *object, const char *param, int kind, float value) {
    if (!object || !param) return 0;
    pthread_mutex_lock(&g_lock);
    int ok = 0;
    for (int i = 0; i < NMNT && !ok; i++) {
        Mnt *m = &g_mnt[i];
        if (!m->state || !m->script || strcmp(m->name, object) || m->nanim >= 8) continue;
        m->anim[m->nanim].hash = fnv(param), m->anim[m->nanim].kind = (char)kind, m->anim[m->nanim].v = value, m->nanim++;
        ok = 1;
    }
    pthread_mutex_unlock(&g_lock);
    return ok;
}
int pla_npc_poke_state(int h, float pos[3], float *yaw) {
    pthread_mutex_lock(&g_lock);
    Mnt *m = poke_of(h);
    int st = 0;
    if (m && m->level != g_level) st = 0;
    else if (m) {
        st = m->state == 3 ? MODNPC_UP : MODNPC_COMING;
        if (pos) {
            if (m->owner && in_heap(m->owner) && *(const uint64_t *)(uintptr_t)m->owner == MODEL_OBJ_VTABLE) memcpy(pos, (const float *)(uintptr_t)(m->owner + 0xa0), 12);
            else memcpy(pos, m->at, 12);
        }
        if (yaw) *yaw = m->yaw;
    }
    pthread_mutex_unlock(&g_lock);
    return st;
}
