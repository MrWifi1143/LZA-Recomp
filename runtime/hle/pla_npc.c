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
#define NAV_SET_STEERING 0x700171e30cull  // (SetIsSteering(bool): FObjMove sets false after each destination)
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
} Own;
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

static int dressmake_hook(Cpu *c, uint64_t addr) {
    if (c->x[30] != DRESS_MAKE_PLAYER_LR) return 0;
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
#define NPAR 8
static struct { int slot; char type; uint64_t hash; float f; uint32_t i; } g_par[NPAR];
static volatile int g_npar;
static void par_run(Cpu *c) {
    pthread_mutex_lock(&g_lock);
    for (int k = 0; k < g_npar; k++) {
        if (g_par[k].slot < 0 || g_par[k].slot >= NOWN) continue;
        uint64_t a = g_own[g_par[k].slot].actl;
        uint64_t fn = g_par[k].type == 'F' ? ANIM_SET_FLOAT : g_par[k].type == 'I' ? ANIM_SET_INT : g_par[k].type == 'B' ? ANIM_SET_BOOL : ANIM_SET_TRIGGER;
        anim_set(c, a, fn, g_par[k].hash, g_par[k].f, g_par[k].i);
        LOG("[npc] mpplayer_%d: parameter %c %016llx = %g / %u (animation %llx)\n", g_own[g_par[k].slot].k, g_par[k].type, (unsigned long long)g_par[k].hash,
            g_par[k].f, g_par[k].i, (unsigned long long)a);
    }
    g_npar = 0;
    pthread_mutex_unlock(&g_lock);
}

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
static void puppet_step(Cpu *c, Own *o) {
    if (o->step_frame == g_frame) return;  // (once a frame: the camera update can come twice)
    o->step_frame = g_frame;
    unfix(o);  // (its controller kept from standing at its spot: else no root motion moves it)
    uint64_t now = plat_now_ns();
    float dt = o->step_ns ? (float)(now - o->step_ns) * 1e-9f : 0.033f;
    o->step_ns = now;
    if (dt > 0.2f) dt = 0.2f;
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
    // (standing: none - the idle's own drift would walk it off its spot; up or down to them (stairs, a ledge, a
    // slope): lifted up to 2.5 m/s, lowered up to 4 - given flat, the physics character ran into the first step)
    float up = o->fo[1] - pos[1], vy = o->moving && fabsf(up) > 0.15f ? fmaxf(fminf(up * 4.0f, 2.5f), -4.0f) : 0;
    if (!o->moving) rx = rz = 0;
    g_vel_vec[0] = (rx * ax[0] + rz * az[0]) * inv, g_vel_vec[1] = vy, g_vel_vec[2] = (rx * ax[2] + rz * az[2]) * inv, g_vel_vec[3] = 0;
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
    if (s >= 0 && d > 10.0f) { if (!o->far_frame) o->far_frame = g_frame; }
    else o->far_frame = 0;
    bool stuck = o->stuck_frame && g_frame - o->stuck_frame > 45 && d > 2.0f;
    if (s >= 0 && (d > 25.0f || (o->far_frame && g_frame - o->far_frame > 60) || stuck || o->warp_now)) {
        o->stuck_frame = 0;
        o->warp_now = 0;
        if (!g_warp_vec) g_warp_vec = hle_alloc(32);
        g_warp_vec[0] = o->fo[0], g_warp_vec[1] = o->fo[1] + 0.1f, g_warp_vec[2] = o->fo[2], g_warp_vec[3] = 0;
        g_warp_vec[4] = 0, g_warp_vec[5] = sinf(o->fo[3] * 0.5f), g_warp_vec[6] = 0, g_warp_vec[7] = cosf(o->fo[3] * 0.5f);
        guest_call_saved(c, CC_WARP, o->cc, (uint64_t)(uintptr_t)g_warp_vec, (uint64_t)(uintptr_t)(g_warp_vec + 4), 0, 0);
        LOG("[npc] mpplayer_%d was %.1f m behind%s: put where they are (%.2f %.2f %.2f)\n", o->k, d, stuck ? " (stuck)" : "", o->fo[0], o->fo[1], o->fo[2]);
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
    o->moving = v > 0;
    // which way: toward the spot when off it, else their way (moving) / their facing (standing) - turned at most 12 rad/s
    float wx, wz;
    if (o->moving && d > 0.4f) wx = ex / d, wz = ez / d;
    else wx = yx, wz = yz;
    // (from the way it faces now - its owner's z axis: the net's own turns rotate it too)
    const float *zx = (const float *)(uintptr_t)(o->owner + 0x90);
    float a = atan2f(zx[0], zx[2]), b = atan2f(wx, wz), da = remainderf(b - a, 6.2831853f), lim = 12.0f * dt;
    float off = fabsf(da);
    if (da > lim) da = lim; else if (da < -lim) da = -lim;
    a += da;
    if (off > 0.03f) {
        if (!g_fwd_vec) g_fwd_vec = hle_alloc(16);
        g_fwd_vec[0] = sinf(a), g_fwd_vec[1] = 0, g_fwd_vec[2] = cosf(a), g_fwd_vec[3] = 0;
        guest_call_saved(c, CC_SET_FORWARD, o->cc, (uint64_t)(uintptr_t)g_fwd_vec, 0, 0, 0);
        o->face[0] = g_fwd_vec[0], o->face[1] = g_fwd_vec[2];
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
    if (wlog < 0) wlog = getenv("LARECOMP_NPC_WALKLOG") && atoi(getenv("LARECOMP_NPC_WALKLOG"));
    static int nlog;
    if (wlog && nlog < 3000 && (g_frame % 30 == 0))
        nlog++, LOG("[npc] mpplayer_%d walked (frame %llu): at %.2f %.2f, %.2f m off, their speed %.1f -> %.2f m/s (move_front %.2f), facing %.0f want %.0f\n", o->k,
                    (unsigned long long)g_frame, pos[0], pos[2], d, s, v, mf, atan2f(zx[0], zx[2]) * 57.3f, b * 57.3f);
}

// orders need a vec3 in guest memory: one per person, written just before the call
static float *g_vec[NOWN];
void pla_npc_cam(Cpu *c) {  // (the camera update, the game's thread: hle/warp.c)
    if (!g_mgr) return;
    nav_match();
    if (g_npar) par_run(c);
    if (!g_try_done) try_wear(c);
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
            g_par[g_npar].slot = k, g_par[g_npar].type = o, g_par[g_npar].hash = fnv(item_name), g_par[g_npar].f = at[0], g_par[g_npar].i = (uint32_t)at[0];
            g_npar++;
            pthread_mutex_unlock(&g_lock);
        }
        else if (sscanf(l, "pwarp %d", &k) == 1 && k >= 0 && k < NOWN) g_own[k].warp_now = 1;
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
    rt_register_fnhook_at(SAVE_DRESS_SLOT, saveslot_hook);
    try_parse();  // (our outfit is read from the save's dress-up data)
    rt_register_fnhook_at(CHAR_BUILD, build_hook), rt_register_fnhook_at(ANIM_CTOR, actor_hook), rt_register_fnhook_at(NPC_MODEL_CTOR, chr_ctor_hook);
    if (g_dress) rt_register_fnhook_at(CREATE_MODEL, create_hook), rt_register_fnhook_at(NPC_ENTITY_CB, entity_hook), rt_register_fnhook_at(NPC_MODEL_JOB, job_hook);
}
