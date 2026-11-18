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
#define MODEL_BUILD 0x7001a2b778ull  // (the player's model from its dress-up: [x0+0x248] != 0 = the p2 body)
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
    int state;  // 0 free, 1 asked for, 2 made (its navigator not seen yet), 3 walking orders taken
    int gender, k;
    float at[3];        // where it was made
    uint64_t nav, made_frame;
    uint64_t level;
    float dest[3];      // the newest order
    int run, order;     // (order: a new one to give)
    uint64_t asked;     // (frame asked)
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

// a navigator set up: ours when it stands where one of ours was asked for
static int nav_init_hook(Cpu *c, uint64_t addr) {
    uint64_t nav = c->x[0];
    rt_call_original(c, addr);
    if (!in_heap(nav)) return 1;
    const float *p = (const float *)(uintptr_t)(nav + 0x3e0);
    extern bool warp_player_pos(float out[3]);
    float me[3];
    if (warp_player_pos(me) && fabsf(p[0] - me[0]) < 1.5f && fabsf(p[2] - me[2]) < 1.5f) return 1;  // (the player's own)
    pthread_mutex_lock(&g_lock);
    for (int i = 0; i < NOWN; i++) {
        Own *o = &g_own[i];
        if (o->state != 2 || fabsf(p[0] - o->at[0]) > 1.0f || fabsf(p[2] - o->at[2]) > 1.0f) continue;
        o->nav = nav, o->state = 3, o->order = 1;
        LOG("[npc] mpplayer_%d is up: navigator %llx at %.2f %.2f %.2f (%llu frames after asking)\n", o->k, (unsigned long long)nav, p[0], p[1], p[2],
            (unsigned long long)(g_frame - o->asked));
        break;
    }
    pthread_mutex_unlock(&g_lock);
    return 1;
}

static int model_hook(Cpu *c, uint64_t addr) {
    (void)addr;
    if (in_heap(c->x[0])) {
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

// orders need a vec3 in guest memory: one per person, written just before the call
static float *g_vec[NOWN];
void pla_npc_cam(Cpu *c) {  // (the camera update, the game's thread: hle/warp.c)
    if (!g_mgr) return;
    int made = 0;
    for (int i = 0; i < NOWN; i++) {
        Own *o = &g_own[i];
        if (o->state == 1 && !made) { make_one(c, o), made = 1; continue; }  // (one a frame)
        if (o->state == 2 && g_frame - o->made_frame > 300) {
            LOG("[npc] mpplayer_%d: no navigator 10 s after making it - asking again\n", o->k);
            o->state = 1, o->k = ++g_count;
            continue;
        }
        if ((o->state != 3 && o->state != 4) || !o->order) continue;
        if (*(const uint64_t *)(uintptr_t)o->nav != NAV_VTABLE) {
            LOG("[npc] mpplayer_%d: its navigator is gone\n", o->k);
            o->state = 0;
            continue;
        }
        if (!g_vec[i]) g_vec[i] = hle_alloc(16);
        float *v = g_vec[i];
        v[0] = o->dest[0], v[1] = o->dest[1], v[2] = o->dest[2], v[3] = 0;
        o->order = 0;
        guest_call_saved(c, NAV_SET_MOVESTATE, o->nav, o->run ? 1 : 2, 0, 0, 0);
        uint64_t sx[32], snzcv = c->nzcv;  // (SetDestination's answer: 0 = no navmesh there)
        __uint128_t sv[32];
        memcpy(sx, c->x, sizeof sx), memcpy(sv, c->v, sizeof sv);
        c->x[0] = o->nav, c->x[1] = (uint64_t)(uintptr_t)v;
        guest_call(c, NAV_SET_DEST);
        uint32_t ok = (uint32_t)c->x[0] & 1;
        memcpy(c->x, sx, sizeof sx), memcpy(c->v, sv, sizeof sv), c->nzcv = snzcv;
        static int nlog;
        if (nlog < 12 || !ok) nlog++, LOG("[npc] mpplayer_%d sent to %.2f %.2f %.2f (%s): %s\n", o->k, v[0], v[1], v[2], o->run ? "run" : "walk", ok ? "on its way" : "no navmesh there");
    }
}

// pla_world.c's side (its own thread): ask for one, give it orders, let it go
int pla_npc_new(const float at[3], int gender) {
    pthread_mutex_lock(&g_lock);
    int r = -1;
    for (int i = 0; i < NOWN && r < 0; i++)
        if (!g_own[i].state) {
            Own *o = &g_own[i];
            memset(o, 0, sizeof *o);
            o->state = 1, o->gender = gender, o->k = ++g_count, o->level = g_level, o->asked = g_frame;
            memcpy(o->at, at, 12), memcpy(o->dest, at, 12);
            r = i;
        }
    pthread_mutex_unlock(&g_lock);
    return r;
}
int pla_npc_state(int i) { return i >= 0 && i < NOWN ? g_own[i].state : 0; }
int pla_npc_gender_of(int i) { return i >= 0 && i < NOWN ? g_own[i].gender : -1; }
void pla_npc_order(int i, const float to[3], int run) {
    if (i < 0 || i >= NOWN || (g_own[i].state != 3 && g_own[i].state != 4)) return;
    Own *o = &g_own[i];
    memcpy(o->dest, to, 12), o->run = run;
    o->order = 1;
}
void pla_npc_free(int i) {  // (it walks off where it was sent last: the caller sends it away first)
    if (i < 0 || i >= NOWN) return;
    pthread_mutex_lock(&g_lock);
    if (g_own[i].state && g_own[i].state != 3) g_own[i].state = 0;  // (not up yet: forgotten - the game may still make it)
    else if (g_own[i].state == 3) g_own[i].state = 4;  // (4: up, nobody's)
    pthread_mutex_unlock(&g_lock);
}
int pla_npc_take(int gender) {  // (a free one of that look already up, or -1)
    pthread_mutex_lock(&g_lock);
    int r = -1;
    for (int i = 0; i < NOWN && r < 0; i++)
        if (g_own[i].state == 4 && g_own[i].gender == gender) g_own[i].state = 3, r = i;
    pthread_mutex_unlock(&g_lock);
    return r;
}
uint64_t pla_npc_level(void) { return g_level; }

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
        if (sscanf(l, "make %f %f %f %c", &at[0], &at[1], &at[2], &g) >= 3) pla_npc_new(at, g == 'f');
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
    rt_register_fnhook_at(FACTORY_CREATE, fcreate_hook);
    rt_register_fnhook_at(NAV_INIT, nav_init_hook);
    rt_register_fnhook_at(MODEL_BUILD, model_hook);
}
