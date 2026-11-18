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
#define NOWN 16
typedef struct {
    int state;  // 0 free, 1 asked for, 2 made (its navigator not seen yet), 3 walking orders taken, 4 up and nobody's,
                // 5 made but let go before it came up (it becomes 4 when it does: kept, not left standing there),
                // 6 given up waiting for it (as 5: a free one if the game makes it after all)
    int gender, k;
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

static int place_hook(Cpu *c, uint64_t addr) {
    if (c->x[30] != PLACE_LOAD_LR) return 0;
    uint64_t e = c->x[2];
    pthread_mutex_lock(&g_lock);
    if (g_mgr != c->x[0]) {
        g_nent = 0, g_level++;
        for (int i = 0; i < NOWN; i++)
            if (g_own[i].state) LOG("[npc] mpplayer_%d went with the level\n", g_own[i].k), memset(&g_own[i], 0, sizeof g_own[i]);
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
            if ((o->state != 2 && o->state != 5 && o->state != 6) || fabsf(p[0] - o->at[0]) > 1.0f || fabsf(p[2] - o->at[2]) > 1.0f) continue;
            if (o->state == 6) LOG("[npc] mpplayer_%d came up after all: a free one now\n", o->k);
            o->nav = nav, o->state = o->state == 2 ? 3 : 4, o->order = 0, o->up_frame = g_frame;  // (no order before one is given: an order to where it stands left it unable to walk)
            o->ctrl = ctrl_of(nav);
            { uint64_t e = *(const uint64_t *)(uintptr_t)(nav + 0x1f0), q2 = in_heap(e) ? *(const uint64_t *)(uintptr_t)(e + 0x18) : 0;
              o->cc = in_heap(q2 - 0x50) && *(const uint64_t *)(uintptr_t)(q2 - 0x50) == CC_VTABLE ? q2 - 0x50 : 0; }
            o->actl = in_heap(g_navactl[q]) && *(const uint64_t *)(uintptr_t)g_navactl[q] == ANIM_VTABLE ? g_navactl[q] : 0, o->mf = -1;
            o->chr = in_heap(g_navchr[q]) ? g_navchr[q] : 0;
            LOG("[npc] mpplayer_%d is up: navigator %llx at %.2f %.2f %.2f (%llu frames after asking), controller %llx, animation %llx\n", o->k, (unsigned long long)nav,
                p[0], p[1], p[2], (unsigned long long)(g_frame - o->made_frame), (unsigned long long)o->ctrl, (unsigned long long)o->actl);
            unfix(o);
            break;
        }
        for (int i = 0; i < NMNT; i++) {  // (a mount of ours)
            Mnt *m = &g_mnt[i];
            if (m->state != 2 || fabsf(p[0] - m->at[0]) > 1.0f || fabsf(p[2] - m->at[2]) > 1.0f) continue;
            m->nav = nav, m->state = 3, m->up_frame = g_frame, m->ctrl = ctrl_of(nav);
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

static void make_one(Cpu *c, Own *o) {
    char name[32];
    snprintf(name, sizeof name, "mpplayer_%d", o->k);
    uint64_t chr = o->gender ? CHR_SENPAI_F : CHR_SENPAI_M, ce = 0;
    const char *like = "?";
    pthread_mutex_lock(&g_lock);
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
    LOG("[npc] %s asked for at %.2f %.2f %.2f (a copy of %s, the %s senior): %s\n", name, o->at[0], o->at[1], o->at[2], like, o->gender ? "female" : "male",
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

static int g_dress = -1;  // (ours wear their player's outfit; research LARECOMP_NPC_DRESS=0: never)
static int create_hook(Cpu *c, uint64_t addr) {
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

// The animation component's parameters, set by name hash (FNV-1a of the name in the control net's .tracp): the
// setters look the hash up in the component's map of that type (float +0x730, int +0x6c8, bool +0x798, trigger +0x800;
// a name the net doesn't have is ignored). A dressed person runs the player's net (pc0002_00.tracn): it walks and runs
// by move_front (0..1, the stick's push), its root motion moving it forward, as the player's does.
#define ANIM_SET_FLOAT 0x7001950188ull
#define ANIM_SET_INT 0x7001951334ull
#define ANIM_SET_BOOL 0x700195bd74ull
#define ANIM_SET_TRIGGER 0x700194f4d4ull
static uint64_t *g_hash_slot;  // (guest memory for the hash argument)
static void anim_set(Cpu *c, uint64_t actl, uint64_t fn, uint64_t hash, float f, uint32_t i) {
    if (!in_heap(actl) || *(const uint64_t *)(uintptr_t)actl != ANIM_VTABLE) return;
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
static void my_set(Cpu *c, char type) {
    uint64_t a = c->x[0];
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
int pla_npc_my_riding(void) {  // (we ride now: our set_ride_bool)
    int k = mirror_index(fnv("set_ride_bool"));
    return k >= 0 && g_my_type[k] == 'B' && g_my_state[k];
}
// theirs, on their person i (a trigger: fired; the rest: set)
void pla_npc_mirror(int i, int k, char type, uint32_t val) {
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
const char *pla_npc_mirror_name(int k) { return k >= 0 && k < NMIRROR ? k_mirror[k] : "?"; }
int pla_npc_mirror_count(void) { return NMIRROR; }

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
static float *g_fwd_vec, *g_vel_vec, *g_warp_vec;
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
static struct { int mons; float seat_y, seat_z, turn_x, mount_y; } k_ride[8] = {  // (research: live "seat <type> <mons> <y> <z> <turn> [mount y]")
    {0, 0, 0, 0, 0}, {899, 1.20f, -0.15f, 1.5707963f, 0}, {902, 0, 0, 0, 0}, {0, 0, 0, 0, 0}, {0, 0, 0, 0, 0}, {903, 0, 0, 0, 0}, {0, 0, 0, 0, 0}, {0, 0, 0, 0, 0},
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
// a riding person's frame: its mount where they are, the person on it. 1 = done (the walking steps skipped)
static int ride_step(Cpu *c, Own *o, float dt) {
    int t = o->ride_type & 7;
    bool want = o->riding && t && k_ride[t].mons;
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
                m->k = ++g_poke_n, m->state = 1, m->level = g_level, m->rider = (int)(o - g_own) + 1, m->attached = 0;
                snprintf(m->name, sizeof m->name, "mpride_%d", m->k);
                o->mnt[t] = i + 1;
                LOG("[npc] mpplayer_%d rides (type %d): %s asked for\n", o->k, t, m->name);
                break;
            }
        pthread_mutex_unlock(&g_lock);
        m = NULL;
    }
    bool on = want && m && m->state == 3 && m->cc && m->owner;
    float *loc = (float *)(uintptr_t)(o->owner + 0xd0), *lq = (float *)(uintptr_t)(o->owner + 0xc0);
    if (!on) {
        if (o->rode) {  // (off: back on its feet where its mount stands, the mount put away)
            o->rode = 0;
            anim_set(c, o->actl, ANIM_SET_BOOL, fnv("set_ride_bool"), 0, 0);
            loc[0] = loc[1] = loc[2] = 0, lq[0] = lq[1] = lq[2] = 0, lq[3] = 1, o->lyaw = 0, o->fa_set = 0;
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
    ml[0] = ml[1] = ml[2] = 0, mq[0] = mq[1] = mq[2] = 0, mq[3] = 1;
    float mat[3] = {o->fo[0], o->fo[1] + k_ride[t].mount_y, o->fo[2]};
    cc_warp(c, m->cc, mat, yaw);
    if (!g_fwd_vec) g_fwd_vec = hle_alloc(16);  // (its facing: the warp's rotation isn't taken - its forward direction is)
    g_fwd_vec[0] = sinf(yaw), g_fwd_vec[1] = 0, g_fwd_vec[2] = cosf(yaw), g_fwd_vec[3] = 0;
    guest_call_saved(c, CC_SET_FORWARD, m->cc, (uint64_t)(uintptr_t)g_fwd_vec, 0, 0, 0);
    if (m->actl && *(const uint64_t *)(uintptr_t)m->actl == POKE_ANIM_VTABLE) {
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
    loc[0] = 0, loc[1] = park + k_ride[t].mount_y + k_ride[t].seat_y + g_knob[2], loc[2] = k_ride[t].seat_z;
    float qa = (k_ride[t].turn_x + g_knob[4]) * 0.5f;  // (into the joint's frame: Wyrdeer's about x +90 degrees, y forward, z down)
    lq[0] = sinf(qa), lq[1] = 0, lq[2] = 0, lq[3] = cosf(qa);
    // (their speed to the rider's net: the riding pose moves with it)
    float mf = mf_for(o->fo[4]);
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
static uint64_t poke_make(Cpu *c, int mons, const float at[3], float yaw_deg, const char *name) {
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
    st[0] = 0, st[1] = 0, st[2] = 0, st[3] = 128, st[4] = 0, st[5] = 1, st[6] = 1;  // (form, sex, rare, height, alpha, p11, p12)
    c->sp = sp;
    c->x[0] = fo, c->x[1] = (uint64_t)(uintptr_t)pos, c->x[2] = (uint64_t)(uintptr_t)rot, c->x[3] = (uint64_t)(uintptr_t)scale;
    c->x[4] = (uint64_t)(uintptr_t)(m + 0x30), c->x[5] = (uint64_t)(uintptr_t)(m + 0x40), c->x[6] = (uint64_t)(uintptr_t)(m + 0x38), c->x[7] = (uint64_t)mons;
    guest_call(c, FO_POKE_DIRECT);
    *c = saved;
    LOG("[npc] Pokemon %d asked for as %s at %.2f %.2f %.2f (FieldObjectForHaxe %llx)\n", mons, name, at[0], at[1], at[2], (unsigned long long)fo);
    return 1;
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
void pla_npc_cam(Cpu *c) {  // (the camera update, the game's thread: hle/warp.c)
    if (!g_mgr) return;
    nav_match();
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
        if (m->state == 1) { m->state = poke_make(c, m->mons, m->at, m->yaw, m->name) ? 2 : 0, m->asked = g_frame; continue; }
        if (m->state == 2 && g_frame - m->asked > 900) { LOG("[npc] %s: not up 30 s after asking - given up\n", m->name); m->state = 0; continue; }
        if (m->state != 3) continue;
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
            nlog++, LOG("[npc] mpplayer_%d sent to %.2f %.2f %.2f (%s, %.1f m from it): %s\n", o->k, v[0], v[1], v[2], o->run ? "run" : "walk", sqrtf(dx * dx + dz * dz),
                        ok ? "on its way" : "not taken (there already, or no navmesh)");
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
        else if (sscanf(l, "poke %d %f %f %f %f", &k, &at[0], &at[1], &at[2], &yw) >= 4) {  // (research: a Pokémon of ours there)
            pthread_mutex_lock(&g_lock);
            for (int i = 0; i < NMNT; i++)
                if (!g_mnt[i].state) {
                    Mnt *m = &g_mnt[i];
                    memset(m, 0, sizeof *m);
                    m->mons = k, memcpy(m->at, at, 12), m->yaw = yw, m->k = ++g_poke_n, m->state = 1, m->level = g_level;
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

__attribute__((constructor)) static void pla_npc_init(void) {
    if (!pla_npc_enabled()) return;
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
    rt_register_fnhook_at(ANIM_SET_FLOAT, myset_f), rt_register_fnhook_at(ANIM_SET_INT, myset_i);
    rt_register_fnhook_at(ANIM_SET_BOOL, myset_b), rt_register_fnhook_at(ANIM_SET_TRIGGER, myset_t);
    rt_register_fnhook_at(PLAYER_CC_CROUCH, crouch_hook), rt_register_fnhook_at(PLAYER_CC_SWIM, swim_hook);
    rt_register_fnhook_at(SAVE_DRESS_SLOT, saveslot_hook);
    try_parse();  // (our outfit is read from the save's dress-up data)
    rt_register_fnhook_at(CHAR_BUILD, build_hook), rt_register_fnhook_at(ANIM_CTOR, actor_hook), rt_register_fnhook_at(NPC_MODEL_CTOR, chr_ctor_hook);
    rt_register_fnhook_at(POKE_ANIM_CTOR, actor_hook), rt_register_fnhook_at(POKE_MODEL_CTOR, chr_ctor_hook);  // (our mounts' parts)
    rt_register_fnhook_at(NPC_ENTITY_CB, entity_hook);
    if (g_dress) rt_register_fnhook_at(CREATE_MODEL, create_hook), rt_register_fnhook_at(NPC_MODEL_JOB, job_hook);
}
