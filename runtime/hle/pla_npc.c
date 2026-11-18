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
    uint64_t level;
    float dest[3];      // the newest order
    int run, order;     // (order: a new one to give)
    int run_set;        // (the move state given last: 0 none, 1 run, 2 walk)
    int shrink;         // (a let-go person's walk-off orders refused so far: each retry half as far)
    uint64_t asked;     // (frame asked)
    uint64_t outfit[12];  // (their dress-up items; none heard: the senior's look)
    int has_outfit, dressed, redress;
    int modeled;        // (its model made: the NPC model job came for it - dressed or not)
    uint64_t worn[12];  // (the items put on it so far)
    uint64_t comp;      // (its dress-up component, once dressed)
    uint64_t ent;       // (its scene object (vtable 0x700417dc80): the dress-up goes on it)
    uint64_t ctx;       // (its model's context, from the model job)
    uint64_t modeled_frame;
    int dress_steps, seen_taken;
    uint64_t model;     // (its model handle, from the maker)  // (dressed in the camera update a few frames after its model was made)
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
static uint64_t g_navq[32];
static volatile int g_navn;
static int nav_pos_hook(Cpu *c, uint64_t addr) {
    (void)addr;
    if (c->x[30] != NAV_POS_LR || !in_heap(c->x[0])) return 0;
    pthread_mutex_lock(&g_lock);
    if (g_navn < 32) g_navq[g_navn++] = c->x[0];
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
            LOG("[npc] mpplayer_%d is up: navigator %llx at %.2f %.2f %.2f (%llu frames after asking), controller %llx\n", o->k, (unsigned long long)nav, p[0], p[1],
                p[2], (unsigned long long)(g_frame - o->made_frame), (unsigned long long)o->ctrl);
            unfix(o);
            break;
        }
    }
    g_navn = 0;
    pthread_mutex_unlock(&g_lock);
}

// ---- the other player's own look (their clothes, hair, eyes - the dress-up), worn by ours
// How the game dresses its player (Lua-traced 2026-11-18): the field player's model is made from a desc (0x700125eb8c:
// the shared body bin/chara/data/pc/p2/model/base/p2_base0001_00_default.trmdl, the player's motion pc0002_00.tracn,
// its locators) by the model-object maker 0x700142b4a4(out, ctx, x2, desc) - the same maker the NPC model job
// 0x7001ae417c calls with the character's desc (lr 0x7001ae48bc). Then its dress-up component is made by
// 0x7001253354(model object, ctx, params 0x68 B: three resource handles at +0/+0x10/+0x40, +0x50 the save's dress
// slot 11, +0x58 the body (0 p1, 1 p2)) and each worn item loaded onto it by 0x7001a2b778(component, &{u64 item hash,
// ..}, 0) - the item = FNV-1a of its dress-up table name ("p1_hrs0001_00_default_black", bin/chara/table/
// dressup_table_p1.bin / _p2.bin); only the hash is read. The save keeps the 12 worn items as hashes at +0x68 of its
// dress-up data (the empty hash = none). The cutscene player (0x700142c940) does the same.
// Each player sends their 12 items (hle/pla_world.c 'O1'); ours keep the outfit of whom they stand for.
// UNFINISHED (research LARECOMP_NPC_DRESS=1, off): when the NPC model job makes one of ours that has an outfit (its
// desc's model = a senior's), the desc is swapped for the player's and, once its scene object has a model, a dress-up
// component is put on it and the items loaded (its update - vfunc 17 0x7001a26068, state +0x27b8, 1 = settled - run
// from the camera update). Found 2026-11-18: (1) the player's desc on an NPC gives an invisible person that walks (the
// shared body has no meshes; the parts never come); (2) the component's state 0 needs a model component (vtable
// 0x70040d4ac0, looked up by 0x7000a12644(owner+0xf0)) on its owner - the scene object at the person's spot
// (vtable 0x700417dc80, translation +0xa0) never gets one, and no new scene object with one shows up for ours; (3) the
// player's params handles shared with ours turned the player's own face and hair white (they're written to). Next:
// the cutscene player (0x700142c0b4 / 0x700142c940: desc 0x700125efcc, maker, then the component) makes a dressed
// player-like person from scratch - follow how its owner gets that model component.
// Without an outfit (or with this off) ours are the senior of their body: the default player look.
#define CREATE_MODEL 0x700142b4a4ull
#define NPC_MODEL_LR 0x7001ae48bcull
#define PLAYER_DESC 0x700125eb8cull
#define DRESS_MAKE 0x7001253354ull
#define DRESS_UPDATE 0x7001a26068ull
#define MODEL_OBJ_VTABLE 0x700417dc80ull  // (the object a model handle stands for: what components are put on)
#define DRESS_MAKE_PLAYER_LR 0x7001c5a284ull
#define SAVE_DRESS_SLOT 0x700101b7c0ull  // (x0 the save's dress-up data, w1 a slot: the field player asks for 11)
#define SAVE_DRESS_SLOT_LR 0x7001c5a268ull
#define EMPTY_HASH 0xcbf29ce484222645ull
static uint8_t g_pparams[0x68];
static volatile int g_have_pparams;
static volatile uint64_t g_player_comp, g_save_dress;

static int dressmake_hook(Cpu *c, uint64_t addr) {
    if (c->x[30] != DRESS_MAKE_PLAYER_LR || !guest_ptr(c->x[2])) return 0;
    memcpy(g_pparams, (const void *)(uintptr_t)c->x[2], sizeof g_pparams), g_have_pparams = 1;
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
int pla_npc_my_outfit(uint64_t out[12]) {  // (the 12 items we wear: 0 when not known yet)
    uint64_t s = g_save_dress;
    if (!s) return 0;
    memcpy(out, (const void *)(uintptr_t)(s + 0x68), 96);
    return 1;
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

// ours waiting for their model (made by the game in the order asked): the oldest made one with an outfit
static int dress_next(void) {
    int best = -1;
    for (int i = 0; i < NOWN; i++) {
        Own *o = &g_own[i];
        if (o->state >= 2 && o->has_outfit && !o->dressed && !o->modeled && (best < 0 || o->made_frame < g_own[best].made_frame)) best = i;
    }
    return best;
}

static int g_dress = -1;  // (research LARECOMP_NPC_DRESS=1: ours dressed as the player - unfinished, see below)
static int create_hook(Cpu *c, uint64_t addr) {
    if (c->x[30] != NPC_MODEL_LR || !g_dress || !g_have_pparams) return 0;
    uint64_t d = c->x[3];
    if (!guest_ptr(d)) return 0;
    uint64_t v0 = *(const uint64_t *)(uintptr_t)d, v1 = *(const uint64_t *)(uintptr_t)(d + 8);
    if (!guest_ptr(v0) || v1 <= v0) return 0;
    uint64_t sp = *(const uint64_t *)(uintptr_t)(v0 + 0x10);
    const char *path = guest_ptr(sp) ? (const char *)(uintptr_t)sp : "";
    static int nlog;
    if (nlog < 60 && getenv("LARECOMP_NPC_DRESSLOG")) nlog++, LOG("[npc] model job: %s\n", path);
    if (!strstr(path, "tr0036_00_senpai_m.trmdl") && !strstr(path, "tr0037_00_senpai_f.trmdl")) return 0;
    pthread_mutex_lock(&g_lock);
    int k = dress_next();
    Own *o = k >= 0 ? &g_own[k] : NULL;
    int gender = 0;
    if (o) gender = o->gender, o->dressed = 1, o->modeled = 1;
    pthread_mutex_unlock(&g_lock);
    if (!o) return 0;
    uint64_t ctx = c->x[1];
    uint8_t *desc = hle_alloc(0x200);  // (the player's desc: the shared body, the player's motion and locators)
    gcall(c, PLAYER_DESC, ctx, (uint64_t)gender, 0, (uint64_t)(uintptr_t)desc);
    static int keepmo = -1;  // (research LARECOMP_NPC_DRESSMO=1: the senior's motion kept - the player's swapped out)
    if (keepmo < 0) keepmo = getenv("LARECOMP_NPC_DRESSMO") ? atoi(getenv("LARECOMP_NPC_DRESSMO")) : 0;
    if (keepmo) {
        uint8_t t[0x38];
        memcpy(t, desc + 0x38, 0x38), memcpy(desc + 0x38, (const void *)(uintptr_t)(d + 0x38), 0x38), memcpy((void *)(uintptr_t)(d + 0x38), t, 0x38);
    }
    c->x[3] = (uint64_t)(uintptr_t)desc;
    rt_call_original(c, addr);
    pthread_mutex_lock(&g_lock);
    o->ctx = ctx, o->modeled_frame = g_frame, o->model = c->x[0];
    pthread_mutex_unlock(&g_lock);
    LOG("[npc] mpplayer_%d made as a player: model %llx (%s body)\n", o->k, (unsigned long long)c->x[0], gender ? "p2" : "p1");
    return 1;
}

// its dress-up component on its game object, their items loaded onto it (the camera update: the game's thread)
// the dress-up's first update looks up its owner's model component (0x7000a12644(owner+0xf0, &ref): vtable
// 0x70040d4ac0) - our person's scene object gets it a while after its model is made (once loaded): asked until then
#define FIND_MODEL_COMP 0x7000a12644ull
static bool has_model_comp(Cpu *c, uint64_t ent) {
    static uint64_t *ref;
    if (!ref) ref = hle_alloc(0x20);
    ref[0] = ref[1] = 0;
    return in_heap(gcall(c, FIND_MODEL_COMP, ent + 0xf0, (uint64_t)(uintptr_t)ref, 0, 0));
}

// the scene objects of a context with a model component: our person's is the one new since its model was asked for
#define NSEEN 256
static int with_model(Cpu *c, uint64_t ctx, uint64_t *out, int max) {
    extern uint64_t kernel_heap_size(void), g_stdheap_used;
    uint64_t top = g_stdheap_used ? STDHEAP_BASE + g_stdheap_used : 0, heap = HEAP_BASE + kernel_heap_size();
    uint64_t hi = top > heap ? top : heap;
    int n = 0;
    for (const uint64_t *q = (const uint64_t *)(uintptr_t)HEAP_BASE, *e = (const uint64_t *)(uintptr_t)(hi - 0x400); q < e && n < max; q += 2)
        if (*q == MODEL_OBJ_VTABLE && q[1] == ctx && q[2] == (uint64_t)(uintptr_t)q && has_model_comp(c, (uint64_t)(uintptr_t)q)) out[n++] = (uint64_t)(uintptr_t)q;
    return n;
}
static uint64_t g_seen[NSEEN];
static int g_nseen;

static void dress_up(Cpu *c, Own *o) {
    uint64_t obj = o->ent;
    if (!in_heap(obj) || *(const uint64_t *)(uintptr_t)obj != MODEL_OBJ_VTABLE) {
        LOG("[npc] mpplayer_%d: its game object %llx is gone (vtable %llx) - not dressed\n", o->k, (unsigned long long)obj,
            (unsigned long long)(in_heap(obj) ? *(const uint64_t *)(uintptr_t)obj : 0));
        return;
    }
    uint8_t *pr = hle_alloc(0x70);
    static int share = -1;  // (research LARECOMP_NPC_DRESSSHARE=1: the player's resource handles shared)
    if (share < 0) share = getenv("LARECOMP_NPC_DRESSSHARE") && atoi(getenv("LARECOMP_NPC_DRESSSHARE"));
    if (share) memcpy(pr, g_pparams, 0x68);
    else memcpy(pr + 0x50, g_pparams + 0x50, 0x18);
    static const int hoff[3] = {0, 0x10, 0x40};
    for (int j = 0; j < 3 && share; j++) {  // (the resource handles: one more reference each, ours lets go of them)
        uint64_t h;
        memcpy(&h, pr + hoff[j], 8);
        if (in_heap(h)) __atomic_fetch_add((uint32_t *)(uintptr_t)(h + 8), 1, __ATOMIC_SEQ_CST);
    }
    memcpy(pr + 0x50, &o->outfit[11], 8);
    pr[0x58] = (uint8_t)o->gender;
    uint64_t comp = gcall(c, DRESS_MAKE, obj, o->ctx, (uint64_t)(uintptr_t)pr, 0);
    o->comp = in_heap(comp) ? comp : 0;
    int n = 0;
    for (int i = 0; i < 12 && o->comp; i++) {
        if (!o->outfit[i] || o->outfit[i] == EMPTY_HASH) continue;
        uint64_t *it = hle_alloc(0x20);
        it[0] = o->outfit[i];
        gcall(c, MODEL_BUILD, comp, (uint64_t)(uintptr_t)it, 0, 0);
        n++;
    }
    memcpy(o->worn, o->outfit, sizeof o->worn);
    LOG("[npc] mpplayer_%d: dress-up component %llx on its game object, %d items\n", o->k, (unsigned long long)comp, n);
}

// orders need a vec3 in guest memory: one per person, written just before the call
static float *g_vec[NOWN];
void pla_npc_cam(Cpu *c) {  // (the camera update, the game's thread: hle/warp.c)
    if (!g_mgr) return;
    nav_match();
    int made = 0;
    for (int i = 0; i < NOWN; i++) {
        Own *o = &g_own[i];
        if (o->state == 1 && !made) { make_one(c, o), made = 1; continue; }  // (one a frame)
        if (o->modeled_frame && !o->comp && o->dressed == 1 && o->nav && (g_frame - o->modeled_frame) % 15 == 2 && o->state != 6) {
            // (its scene object - where its navigator has it - once it has its model: then dressed; given up after 20 s)
            uint64_t now[NSEEN], fresh = 0;
            int n = with_model(c, o->ctx, now, NSEEN), nf = 0;
            if (!o->seen_taken) memcpy(g_seen, now, n * 8), g_nseen = n, o->seen_taken = 1;  // (those there before)
            else
                for (int a = 0; a < n; a++) {
                    bool old = false;
                    for (int b = 0; b < g_nseen && !old; b++) old = g_seen[b] == now[a];
                    if (!old) fresh = now[a], nf++;
                }
            if (nf) {
                LOG("[npc] mpplayer_%d: %d scene objects with a model, %d new: %llx\n", o->k, n, nf, (unsigned long long)fresh);
                const float *t = (const float *)(uintptr_t)(fresh + 0x40);
                LOG("[npc]   at %.2f %.2f %.2f (+0xa0), its navigator at %.2f %.2f\n", t[24], t[25], t[26], ((const float *)(uintptr_t)(o->nav + 0x3e0))[0], ((const float *)(uintptr_t)(o->nav + 0x3e0))[2]);
                o->ent = fresh, o->dressed = 2;
                if (!getenv("LARECOMP_NPC_DRESSDRY")) dress_up(c, o);
            }
            else if (g_frame - o->modeled_frame > 600) {
                LOG("[npc] mpplayer_%d: its scene object %llx has no model 20 s on - not dressed\n", o->k, (unsigned long long)o->ent);
                o->dressed = 2;
            }
        }
        if ((o->state == 2 || o->state == 5) && g_frame - o->made_frame > 900) {  // (the game makes people far from us slowly)
            // (given up - but the game may still make it later: then it is kept as a free one, not left standing
            // where nobody is; a level change lets it go for good)
            LOG("[npc] mpplayer_%d: no navigator 30 s after making it - given up (kept as a free one if it comes)\n", o->k);
            o->state = 6;
            continue;
        }
        if (o->comp && o->state >= 3 && o->state <= 4 && in_heap(o->comp)) {
            // (its loads: the component's update (vfunc 17, 0x7001a26068 - a state machine over +0x27b8, 1 = idle with
            // nothing to load) isn't run by the game for a component put on afterwards: run here until it settles)
            uint32_t st = *(const uint32_t *)(uintptr_t)(o->comp + 0x27b8);
            uint64_t q0 = *(const uint64_t *)(uintptr_t)(o->comp + 0x27c0), q1 = *(const uint64_t *)(uintptr_t)(o->comp + 0x27c8);
            if (st != 1 || q0 != q1) {
                gcall(c, DRESS_UPDATE, o->comp, 0, 0, 0);
                if (++o->dress_steps == 1 || *(const uint32_t *)(uintptr_t)(o->comp + 0x27b8) == 1)
                    LOG("[npc] mpplayer_%d's dress-up: state %u (%d updates)\n", o->k, *(const uint32_t *)(uintptr_t)(o->comp + 0x27b8), o->dress_steps);
            }
        }
        if ((o->state == 3 || o->state == 4) && o->redress && in_heap(o->comp)) {  // (their changed items put on)
            o->redress = 0;
            int n = 0;
            for (int j = 0; j < 12; j++) {
                if (!o->outfit[j] || o->outfit[j] == EMPTY_HASH || o->outfit[j] == o->worn[j]) continue;
                uint64_t *it = hle_alloc(0x20);
                it[0] = o->outfit[j], o->worn[j] = o->outfit[j], n++;
                gcall(c, MODEL_BUILD, o->comp, (uint64_t)(uintptr_t)it, 0, 0);
            }
            LOG("[npc] mpplayer_%d re-dressed: %d items changed\n", o->k, n);
        }
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
void pla_npc_order(int i, const float to[3], int run) {
    if (i < 0 || i >= NOWN || (g_own[i].state != 3 && g_own[i].state != 4)) return;
    Own *o = &g_own[i];
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
int pla_npc_take(int gender, const uint64_t *outfit) {  // (a free one of that look already up, or -1: the same
    // outfit first, else one dressed the same way (worn by a player / the senior) - re-dressed with theirs)
    pthread_mutex_lock(&g_lock);
    int r = -1, want = g_dress > 0 && outfit_any(outfit);
    for (int pass = 0; pass < 2 && r < 0; pass++)
        for (int i = 0; i < NOWN && r < 0; i++) {
            Own *o = &g_own[i];
            if (o->state != 4 || o->gender != gender || (o->dressed != 0) != want) continue;
            if (pass == 0 && want && memcmp(o->outfit, outfit, sizeof o->outfit)) continue;
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
        if (memcmp(o->outfit, outfit, sizeof o->outfit)) memcpy(o->outfit, outfit, sizeof o->outfit), o->redress = 1;
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
        if (sscanf(l, "make %f %f %f %c", &at[0], &at[1], &at[2], &g) >= 3) pla_npc_new(at, g == 'f', NULL);
        else if (sscanf(l, "go %d %f %f %f %d", &k, &at[0], &at[1], &at[2], &run) >= 4) pla_npc_order(k, at, run);
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
    g_dress = getenv("LARECOMP_NPC_DRESS") && atoi(getenv("LARECOMP_NPC_DRESS"));  // (research: not working yet)
    rt_register_fnhook_at(DRESS_MAKE, dressmake_hook);  // (the player's own: our outfit is read from the save's dress-up data)
    rt_register_fnhook_at(SAVE_DRESS_SLOT, saveslot_hook);
    if (g_dress) rt_register_fnhook_at(CREATE_MODEL, create_hook);
}
