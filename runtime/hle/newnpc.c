// Research (LARECOMP_NEWNPC=1): NEW townspeople made by the game at a spot we choose (not dressed ones it made itself).
//
// How Z-A makes its people (2026-11-16/17, Legends Lua traces - contrib/legendslua/research/za_newnpc.lua):
//   station NPCs  - a placement NPC spawner component (vtable 0x7003d7da80; its update 0x7000653a8c, call at +0x11a0;
//                   +0x714 for the NPC's Pokemon)
//   crowd walkers - a traffic NPC spawner component (vtable 0x7003d7e438; 0x70009cdbcc +0x618), pairs 0x700049e55c,
//                   others 0x70005e5a38 (+0x788, outzone effects), 0x700060df8c
// all call the CHARACTER PLACEMENT SPAWN 0x70005e8644(x0 the spawner component, x1 the NPC data - {name struct, ...}:
// "npc_A04_z02_02_0002" for a station NPC, "id01" for a walker, "npc_pokemon_136" -, x2 the new object's name struct
// {u64 FNV-1a hash, char *, u64 len, interned record or 0}, x3 / x4 / x5 one block S on the caller's stack: x5 = S
// (+0 a u64 handle), x3 = S+8, x4 = S+0x40 = a transform {+0x10 scale xyz, +0x20 quaternion xyzw, +0x30 position xyz},
// S+0x1a0 a vector {begin, end, capacity, allocator} of 0xc0-byte parameter entries; x6 a hash; x8 out). It makes the
// character's placement scene object (a node, vtable 0x7003be0d28, its name at +0x2b0 {hash, char *, len, record}) and
// its components (the character component, vtable 0x7003d2a8e8, 0x2280 bytes); the character manager (0x70000af150,
// once a frame) realises it when admitted (0x70002ffeb0 -> 0x70003015e0 -> 0x7000302c90: the live node made from the
// placement one, "active_<name>_old" then destroyed; the model's sub-template job 0x70010b6210; CharacterCreationMaster
// make 0x70011164d0). The node's name is ours, the look the NPC data's, the place S's.
//   Here: every station spawn's arguments are kept (by NPC data; blocks copied into guest memory; refreshed as areas
// load), and a command makes the same call again with a new name and position from the character manager's update
// (the game thread that realises characters). A kept spawner whose component is gone (its area unloaded) isn't used
// (2026-11-17: a spawn with one kept from before a trip to Vert crashed in the spawn).
//   Commands: <LARECOMP_LIVE or game folder>/newnpc, one line, read every 15 frames, then removed:
//     spawn <x> <y> <z> [<yaw degrees> [<NPC data name part>]]    near <dx> <dz> [<yaw> [<data>]]  (beside the player)
//     remove <k>     (newnpc_<k>: put under the street - the game lets it go itself)
//     list           (the NPC data kept)        tele <dx> <dz> / goto <x> <y> <z>  (test: the player moved)
//   The shared world (hle/warp.c, setting world_own_npc) makes its other players' stand-ins this way: newnpc_on /
// newnpc_ready / newnpc_spawn / newnpc_number / newnpc_bad below; warp.c dresses them (by their node's name) as they are made.
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../hle.h"
#include "../platform.h"

void rt_register_fnhook_at(uint64_t addr, int (*fn)(Cpu *c, uint64_t addr));
typedef int (*FnHook)(Cpu *c, uint64_t addr);
FnHook rt_fnhook_get(uint64_t addr);
void *hle_alloc(uint64_t size);

#define PLACE_SPAWN 0x70005e8644ull
#define STATION_LR 0x7000654c30ull       // (the placement NPC spawner's call)
#define STATION_SPAWNER_VT 0x7003d7da80ull
#define CHAR_MGR 0x70000af150ull         // (the character manager's update: once a frame, a game thread)
#define CC_MAKE 0x70011164d0ull
#define STEP_A 0x7000081ce0ull           // (a character body stepped)
#define BODY_VTABLE 0x7003be6218ull
#define NODE_VTABLE 0x7003be0d28ull
#define NODE_UPDATE 0x7001ccf27cull      // (a node's world matrices from its local transform: x0 the node, many a frame)
#define AWAY_Y (-500.0f)                 // (where a removed one is put: under the street)
#define MAXNEW 256
#define MAXDATA 64

typedef struct {
    uint64_t x0, x1, x6, x1_hash;
    uint8_t *name, *blk, *ents;  // (guest copies: the name struct 0x40, S 0x300, the entries)
    int nent;
    char data[64];               // (the NPC data's name)
    float at[3];                 // (where its own station NPC stands: the shared world's own townspeople take the nearest)
    int bad;                     // (what it makes isn't a townsperson: not used for the shared world again)
    int miss;                    // (one asked from it wasn't made - newnpc_missed: others first, until its area loads again)
} Snap;
static Snap g_snaps[MAXDATA];
static int g_nsnaps;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

static int g_on = -1;
static int g_count;              // (the last k given: newnpc_<k>)
static volatile uint64_t g_player_owner;
static volatile uint64_t g_node[MAXNEW], g_owner[MAXNEW], g_body[MAXNEW], g_away_at[MAXNEW];
static volatile int g_away[MAXNEW], g_naway;
static float g_tele[3];
static volatile int g_tele_n;
static volatile uint64_t g_pbody;
static FnHook g_prev_make, g_prev_step, g_prev_node;

static uint64_t fnv(const char *s) {
    uint64_t h = 0xcbf29ce484222645ull;
    for (; *s; s++) h = (h ^ (uint8_t)*s) * 0x100000001b3ull;
    return h;
}
bool warp_heap_read(uint64_t a, void *out, size_t n);  // (hle/warp.c: an unmapped or uncommitted range - false, no fault)
static bool rd(uint64_t a, void *o, size_t n) {
    if (a < 0x7000000000ull || a >= 0x7400000000ull) return false;
#ifndef _WIN32
    if (a >= HEAP_BASE && a + n <= HEAP_BASE + HEAP_MAX) { memcpy(o, (const void *)(uintptr_t)a, n); return true; }  // (the guest heap:
#endif                                                                                                                //  all mapped, readable)
    return warp_heap_read(a, o, n);
}
static void name_of(uint64_t ns, char *o, size_t n) {  // (a name struct {hash, char *, len, ...})
    uint64_t p = 0, len = 0;
    o[0] = 0;
    if (rd(ns + 8, &p, 8) && rd(ns + 0x10, &len, 8) && p && len < n) memcpy(o, (const void *)(uintptr_t)p, len), o[len] = 0;
}

// ---- keeping station spawns
static void area_seen(const char *data);  // (the mod API's area_loaded: a batch of station people made - below)
static int spawn_hook(Cpu *c, uint64_t addr) {
    (void)addr;
    static int keep = -1;  // (test LARECOMP_NEWNPC_KEEP=0: none kept - the shared world's fallback, as where no station is loaded)
    if (keep < 0) keep = !getenv("LARECOMP_NEWNPC_KEEP") || atoi(getenv("LARECOMP_NEWNPC_KEEP")) != 0;
    if (c->x[30] != STATION_LR || !keep) return 0;
    char data[64];
    name_of(c->x[1], data, sizeof data);
    area_seen(data);
    pthread_mutex_lock(&g_lock);
    int i = 0;
    for (; i < g_nsnaps; i++) if (!strcmp(g_snaps[i].data, data)) break;
    if (i == g_nsnaps) {
        if (g_nsnaps == MAXDATA) i = (int)(fnv(data) % MAXDATA); else g_nsnaps++;
        g_snaps[i].name = hle_alloc(0x40), g_snaps[i].blk = hle_alloc(0x300), g_snaps[i].ents = hle_alloc(0xc0 * 16);
    }
    Snap *s = &g_snaps[i];
    memcpy(s->name, (const void *)(uintptr_t)c->x[2], 0x40);
    memcpy(s->blk, (const void *)(uintptr_t)c->x[5], 0x300);
    // (S+0x1a0: the parameter entries - {u64 hash, value name struct, ...}, in the caller's heap, freed after the call:
    // copied too. An entry's optional values - flags +0x68 / +0xa0 / +0xb0 - hold callables and heap blocks of the
    // caller: a copy made later called a dead one (2026-11-16). Dropped: the entries keep only their names)
    uint64_t vb = 0, ve = 0;
    memcpy(&vb, s->blk + 0x1a0, 8), memcpy(&ve, s->blk + 0x1a8, 8);
    s->nent = ve > vb && ve - vb <= 0xc0 * 16 ? (int)((ve - vb) / 0xc0) : 0;
    if (s->nent) memcpy(s->ents, (const void *)(uintptr_t)vb, 0xc0 * (size_t)s->nent);
    for (int k = 0; k < s->nent; k++) s->ents[0xc0 * k + 0x68] = 0, s->ents[0xc0 * k + 0xa0] = 0, s->ents[0xc0 * k + 0xb0] = 0;
    s->x0 = c->x[0], s->x1 = c->x[1], s->x6 = c->x[6];
    rd(c->x[1], &s->x1_hash, 8);
    if (strcmp(s->data, data)) s->bad = 0;
    s->miss = 0;
    snprintf(s->data, sizeof s->data, "%s", data);
    memcpy(s->at, (const void *)(uintptr_t)(c->x[4] + 0x30), 12);
    pthread_mutex_unlock(&g_lock);
    static int logged;
    if (logged++ < 3) {
        const float *t = (const float *)(uintptr_t)(c->x[4] + 0x30);
        LOG("[newnpc] kept a station spawn: spawner %llx, NPC data %s, at %.2f %.2f %.2f, %d entries\n", (unsigned long long)c->x[0], data,
            t[0], t[1], t[2], s->nent);
    }
    return 0;
}
static bool snap_alive(const Snap *s) {  // (its spawner and NPC data still there: their area loaded)
    uint64_t v = 0, h = 0;
    return s->x0 && rd(s->x0, &v, 8) && v == STATION_SPAWNER_VT && rd(s->x1, &h, 8) && h == s->x1_hash;
}

// ---- the player (its character's node: the CharacterCreationMaster make named "Player") and our characters
static void modnpc_up(int k, uint64_t node, uint64_t owner);
void newnpc_made(Cpu *c) {  // (the make of any character: hle/warp.c's hook on it, or make_hook below)
    uint64_t req = c->x[2], len = 0, owner = 0;
    if (rd(req + 0x18, &len, 8) && len == 6 && rd(req + 0x30, &owner, 8)) g_player_owner = owner;  // ("Player")
    else if (rd(req + 0x30, &owner, 8)) {  // (ours: its live node kept by number)
        uint64_t cb = 0, n = 0, np = 0;
        char nm[16] = "";
        int k;
        if (!rd(owner + 0xc8, &cb, 8) || !rd(cb + 0x38, &n, 8) || !rd(n + 0x2b8, &np, 8) || !rd(np, nm, 15)) return;
        nm[15] = 0;
        if (sscanf(nm, "newnpc_%d", &k) == 1 && k > 0 && k < MAXNEW) {
            g_node[k] = n, g_owner[k] = owner;
            LOG("[newnpc] newnpc_%03d made: its node %llx, character object %llx\n", k, (unsigned long long)n, (unsigned long long)owner);
        } else if (sscanf(nm, "luanpc_%d", &k) == 1 && k > 0) modnpc_up(k, n, owner);
    }
}
static int make_hook(Cpu *c, uint64_t addr) {
    newnpc_made(c);
    return g_prev_make ? g_prev_make(c, addr) : 0;
}
static bool player_pos(float p[3]) {
    uint64_t o = g_player_owner, v = 0, cb = 0, n = 0;
    if (!o || !rd(o + 0xc8, &cb, 8) || !rd(cb + 0x38, &n, 8) || !rd(n, &v, 8) || v != NODE_VTABLE) return false;
    return rd(n + 0xe0, p, 12);
}
// which way the player faces (the mod API): its node's rotation (+0xd0, a quaternion about y) - its body's matrix doesn't
// turn (2026-11-19: always facing +z); false before its character is made
bool newnpc_player_yaw(float *yaw) {
    uint64_t o = g_player_owner, v = 0, cb = 0, n = 0;
    float q[4];
    if (!o || !rd(o + 0xc8, &cb, 8) || !rd(cb + 0x38, &n, 8) || !rd(n, &v, 8) || v != NODE_VTABLE || !rd(n + 0xd0, q, 16)) return false;
    *yaw = 2.0f * atan2f(q[1], q[3]) * 57.29578f;
    return true;
}
// bodies (vtable 0x7003be6218, position +0x90): the player's (for tele: the one stepped within 1.5 m of its node) and
// put-away ones of ours (kept under the street)
static int step_hook(Cpu *c, uint64_t addr) {
    uint64_t o = c->x[0], v = 0;
    if (g_naway)
        for (int k = 1; k < MAXNEW; k++)
            if (g_away[k] && g_body[k] == o) {
                float *t = (float *)(uintptr_t)(o + 0x90);
                if (t[1] > AWAY_Y + 100.0f) t[1] = AWAY_Y;
                break;
            }
    if (g_tele_n > 0 && o != g_pbody && rd(o, &v, 8) && v == BODY_VTABLE) {
        float p[3], q[3];
        if (player_pos(p) && rd(o + 0x90, q, 12) && (p[0] - q[0]) * (p[0] - q[0]) + (p[2] - q[2]) * (p[2] - q[2]) < 2.25f) g_pbody = o;
    }
    return g_prev_step ? g_prev_step(c, addr) : 0;
}
// Removing one: put away under the street (its node y -500 at each of its updates, its body before each step) - the
// game then lets it go itself 1.2 s later (its node destroyed by the world's deferred destroy list), as with the shared
// world's let-go dressed characters (warp.c gone_*). Destroying its node directly crashed or aborted (2026-11-17: the
// release 0x7001ccef68 and the destroy 0x7001146f10 from the character manager's update - something still held it; from
// the deferred destroy run 0x7001ce2e90; the node's virtual destroy 0x70011476cc aborts unless node +0x5d9 is set).
static int node_hook(Cpu *c, uint64_t addr) {
    uint64_t n = c->x[0];
    if (g_naway)
        for (int k = 1; k < MAXNEW; k++)
            if (g_away[k] && g_node[k] == n) {
                float *t = (float *)(uintptr_t)(n + 0xe0);
                if (t[1] > AWAY_Y + 100.0f) t[1] = AWAY_Y;
                break;
            }
    return g_prev_node ? g_prev_node(c, addr) : 0;
}

// ---- the spawn: from the character manager's update (the game thread that realises characters). A request: where,
// facing (radians), the NPC data wanted (part of its name, "" any; research) or the nearest live station to `at` (the
// shared world: its area is the one most likely loaded as long as that spot is), and the number k of its name newnpc_<k>
typedef struct { int k; float at[3], yaw; char want[64]; int nearest; int mod; } Req;  // (mod: a script's character + 1 - luanpc_<k>)
#define NREQ 16
static Req g_req[NREQ];
static volatile int g_nreq;
static int g_req_snap[MAXNEW];  // (k % MAXNEW: the kept spawn it was made from, +1 - newnpc_bad)
#define NEWNPC_DROP_DEFAULT ""
// The spawn's parameter entries ({u64 key hash, value name struct {hash, char *, len, ...}, ...}: a station NPC's are its
// talk script "TalkNpcEmpty" (key ""), "Armor_NPCGeneral" and the tags PlacementNpcTeam, HumanCharacter, InteractNPC,
// UiDependency - 2026-11-17). Dropped from ours: LARECOMP_NEWNPC_DROP=<name>[,<name>...] (research), else the default list
static uint64_t g_drop[8];
static int g_ndrop = -1;
static bool newnpc_dropped(uint64_t h) {
    if (g_ndrop < 0) {
        const char *e = getenv("LARECOMP_NEWNPC_DROP");
        char buf[256];
        snprintf(buf, sizeof buf, "%s", e ? e : NEWNPC_DROP_DEFAULT);
        g_ndrop = 0;
        for (char *t = strtok(buf, ","); t && g_ndrop < 8; t = strtok(NULL, ","))
            if (*t) g_drop[g_ndrop++] = fnv(t), LOG("[newnpc] ours made without the spawn entry \"%s\"\n", t);
    }
    for (int i = 0; i < g_ndrop; i++) if (h == g_drop[i]) return true;
    return false;
}

static void modnpc_made_from(int i, int snap);
static void do_spawn(Cpu *c, const Req *r) {
    pthread_mutex_lock(&g_lock);
    Snap s;
    int found = -1;
    float best = 1e30f;
    for (int i = g_nsnaps - 1; i >= 0; i--) {  // (the newest kept that is still there and fits; or the nearest - a townsperson
        if (!snap_alive(&g_snaps[i]) || (r->nearest && g_snaps[i].bad) || (r->want[0] && !strstr(g_snaps[i].data, r->want))) continue;
        float dx = g_snaps[i].at[0] - r->at[0], dz = g_snaps[i].at[2] - r->at[2], d = dx * dx + dz * dz;
        if (!r->nearest) { found = i; break; }
        // (of an area's own people - NPC data npc_A<area>_..., not a quest giver's or the Pokemon Center's (npc_lostball_01,
        // npc_t1veil01_pclady: theirs may carry more than a look) - and not one whose last wasn't made, first)
        const char *dn = g_snaps[i].data;
        bool plain = !strncmp(dn, "npc_A", 5) && dn[5] >= '0' && dn[5] <= '9';
        d += (plain ? 0 : 1e6f) + (g_snaps[i].miss ? 1e7f : 0);
        if (d < best) best = d, found = i;
    }
    if (found >= 0) s = g_snaps[found];
    pthread_mutex_unlock(&g_lock);
    if (found < 0) {
        LOG("[newnpc] no station spawn kept%s%s whose area is loaded: %s_%03d not made\n", r->want[0] ? " for " : "", r->want, r->mod ? "luanpc" : "newnpc", r->k);
        if (r->mod) modnpc_made_from(r->mod - 1, -1);
        return;
    }
    if (r->mod) modnpc_made_from(r->mod - 1, found);
    else g_req_snap[r->k % MAXNEW] = found + 1;
    char *name = hle_alloc(32);
    snprintf(name, 32, "%s_%03d", r->mod ? "luanpc" : "newnpc", r->k);
    uint8_t *nm = hle_alloc(0x40), *blk = hle_alloc(0x300), *out = hle_alloc(32);
    memcpy(nm, s.name, 0x40);
    uint64_t h = fnv(name), p = (uint64_t)(uintptr_t)name, len = strlen(name), z = 0;
    memcpy(nm, &h, 8), memcpy(nm + 8, &p, 8), memcpy(nm + 0x10, &len, 8), memcpy(nm + 0x18, &z, 8);
    memcpy(blk, s.blk, 0x300);
    uint64_t b = 0, e = 0;
    if (s.nent) {  // (its own copy of the entries: the game may take them over - less the ones dropped, newnpc_drop)
        uint8_t *ents = hle_alloc(0xc0 * (uint64_t)s.nent);
        int n = 0;
        for (int k = 0; k < s.nent; k++) {
            uint64_t kh, vh;
            memcpy(&kh, s.ents + 0xc0 * k, 8), memcpy(&vh, s.ents + 0xc0 * k + 8, 8);
            if (newnpc_dropped(kh) || newnpc_dropped(vh)) continue;
            memcpy(ents + 0xc0 * n++, s.ents + 0xc0 * k, 0xc0);
        }
        b = (uint64_t)(uintptr_t)ents, e = b + 0xc0 * (uint64_t)n;
    }
    if (r->mod && s.nent && getenv("LARECOMP_MODNPC_TALK") && atoi(getenv("LARECOMP_MODNPC_TALK"))) {  // (research: talkable)
        static char *talk;
        if (!talk) talk = hle_alloc(64), strcpy(talk, "StandTalkNpcLookAtOnInteractTurn");
        for (uint64_t q = b; q < e; q += 0xc0) {
            uint64_t vp = 0, vl = 0;
            memcpy(&vp, (const void *)(uintptr_t)(q + 0x10), 8), memcpy(&vl, (const void *)(uintptr_t)(q + 0x18), 8);
            if (vl == 12 && vp && !memcmp((const void *)(uintptr_t)vp, "TalkNpcEmpty", 12)) {
                uint64_t h = fnv(talk), tp = (uint64_t)(uintptr_t)talk, tl = strlen(talk), z = 0;
                memcpy((void *)(uintptr_t)(q + 8), &h, 8), memcpy((void *)(uintptr_t)(q + 0x10), &tp, 8);
                memcpy((void *)(uintptr_t)(q + 0x18), &tl, 8), memcpy((void *)(uintptr_t)(q + 0x20), &z, 8);
                LOG("[newnpc] %s_%03d: talk script %s (was TalkNpcEmpty)\n", r->mod ? "luanpc" : "newnpc", r->k, talk);
            }
        }
    }
    memcpy(blk + 0x1a0, &b, 8), memcpy(blk + 0x1a8, &e, 8), memcpy(blk + 0x1b0, &e, 8);
    float *t = (float *)(blk + 0x40 + 0x30), *q = (float *)(blk + 0x40 + 0x20);
    t[0] = r->at[0], t[1] = r->at[1], t[2] = r->at[2];
    q[0] = 0, q[1] = sinf(r->yaw / 2), q[2] = 0, q[3] = cosf(r->yaw / 2);
    uint64_t sx[32], snzcv = c->nzcv;
    __uint128_t sv[32];
    memcpy(sx, c->x, sizeof sx), memcpy(sv, c->v, sizeof sv);
    c->x[0] = s.x0, c->x[1] = s.x1, c->x[2] = (uint64_t)(uintptr_t)nm, c->x[3] = (uint64_t)(uintptr_t)(blk + 8);
    c->x[4] = (uint64_t)(uintptr_t)(blk + 0x40), c->x[5] = (uint64_t)(uintptr_t)blk, c->x[6] = s.x6;
    c->x[8] = (uint64_t)(uintptr_t)out;
    guest_call(c, PLACE_SPAWN);
    memcpy(c->x, sx, sizeof sx), memcpy(c->v, sv, sizeof sv), c->nzcv = snzcv;
    LOG("[newnpc] spawned %s (NPC data %s, its station %.0f m away) at %.2f %.2f %.2f yaw %.0f\n", name, s.data,
        sqrtf((s.at[0] - r->at[0]) * (s.at[0] - r->at[0]) + (s.at[2] - r->at[2]) * (s.at[2] - r->at[2])), r->at[0], r->at[1], r->at[2],
        r->yaw * 57.29578f);
}
static volatile int g_anim_pending;
static void modnpc_anims(Cpu *c);
static int mgr_hook(Cpu *c, uint64_t addr) {
    (void)addr;
    if (g_anim_pending) modnpc_anims(c);
    if (!g_nreq) return 0;
    Req r[NREQ];
    pthread_mutex_lock(&g_lock);
    int n = g_nreq;
    memcpy(r, g_req, sizeof *r * (size_t)n);
    g_nreq = 0;
    pthread_mutex_unlock(&g_lock);
    for (int i = 0; i < n; i++) do_spawn(c, &r[i]);
    return 0;
}
static int queue_as(const float at[3], float yaw, const char *want, int nearest, int mod, int k) {
    pthread_mutex_lock(&g_lock);
    if (g_nreq < NREQ) {
        Req *r = &g_req[g_nreq];
        if (!k) k = ++g_count;
        r->k = k, r->mod = mod;
        memcpy(r->at, at, 12), r->yaw = yaw, r->nearest = nearest;
        snprintf(r->want, sizeof r->want, "%s", want ? want : "");
        g_nreq++;
    } else k = 0;
    pthread_mutex_unlock(&g_lock);
    return k;
}
static int queue(const float at[3], float yaw, const char *want, int nearest) { return queue_as(at, yaw, want, nearest, 0, 0); }

// ---- for the shared world (hle/warp.c, setting world_own_npc): its other players' stand-ins made like this
static bool g_api_on;
void newnpc_on(void) {
    if (g_api_on || g_on > 0) return;  // (research LARECOMP_NEWNPC: already)
    g_api_on = true;
    rt_register_fnhook_at(PLACE_SPAWN, spawn_hook);
    rt_register_fnhook_at(CHAR_MGR, mgr_hook);
}
// a station spawn kept whose area is loaded (its spawner still there): one can be made now
bool newnpc_ready(void) {
    static uint64_t at;  // (looked at again at most every 0.2 s: asked each frame for each player waiting)
    static bool any;
    uint64_t now = plat_now_ns();
    pthread_mutex_lock(&g_lock);
    if (!at || now - at > 200000000ull) {
        at = now, any = false;
        for (int i = 0; i < g_nsnaps && !any; i++) any = !g_snaps[i].bad && snap_alive(&g_snaps[i]);
    }
    bool r = any;
    pthread_mutex_unlock(&g_lock);
    return r;
}
// one made at `at` facing yaw (radians) at the character manager's next update: its number k (its node newnpc_<k>), 0
// none (too many asked at once)
int newnpc_spawn(const float at[3], float yaw) { return queue(at, yaw, NULL, 1); }
// a node's k (newnpc_<k>), 0 another's
int newnpc_number(uint64_t node) {
    uint64_t v = 0, np = 0;
    char nm[16] = "";
    int k;
    if (!rd(node, &v, 8) || v != NODE_VTABLE || !rd(node + 0x2b8, &np, 8) || !rd(np, nm, 15)) return 0;
    nm[15] = 0;
    return sscanf(nm, "newnpc_%d", &k) == 1 && k > 0 ? k : 0;
}
// newnpc_<k> wasn't made (asked from a spawner whose area was just going or coming): its kept spawn after others
void newnpc_missed(int k) {
    int i = g_req_snap[k % MAXNEW] - 1;
    pthread_mutex_lock(&g_lock);
    if (i >= 0 && i < g_nsnaps) g_snaps[i].miss++;
    pthread_mutex_unlock(&g_lock);
}
// newnpc_<k> isn't a townsperson (a story character's station): its kept spawn not used for these again
void newnpc_bad(int k) {
    int i = g_req_snap[k % MAXNEW] - 1;
    pthread_mutex_lock(&g_lock);
    if (i >= 0 && i < g_nsnaps && !g_snaps[i].bad) g_snaps[i].bad = 1, LOG("[newnpc] NPC data %s makes no townsperson: not used again\n", g_snaps[i].data);
    pthread_mutex_unlock(&g_lock);
}

// (hooks registered before plugins load: theirs chain in front of these - runtime/plugin.c)
__attribute__((constructor)) static void newnpc_init(void) {
    g_on = getenv("LARECOMP_NEWNPC") != NULL;
    if (!g_on) return;
    rt_register_fnhook_at(PLACE_SPAWN, spawn_hook);
    rt_register_fnhook_at(CHAR_MGR, mgr_hook);
    g_prev_make = rt_fnhook_get(CC_MAKE);
    rt_register_fnhook_at(CC_MAKE, make_hook);
    g_prev_step = rt_fnhook_get(STEP_A);
    rt_register_fnhook_at(STEP_A, step_hook);
    g_prev_node = rt_fnhook_get(NODE_UPDATE);
    rt_register_fnhook_at(NODE_UPDATE, node_hook);
}

static void modnpc_frame(uint64_t frame);
static int g_api;
void newnpc_frame(uint64_t frame) {  // (each frame: runtime/hle/input.c; the render thread - no game calls here)
    if (g_api) modnpc_frame(frame);
    if (g_on <= 0) return;
    if (g_tele_n > 0) {  // (test: the player put somewhere - written for 20 frames, as the game settles it)
        uint64_t b = g_pbody, v = 0;
        if (b && rd(b, &v, 8) && v == BODY_VTABLE) memcpy((void *)(uintptr_t)(b + 0x90), g_tele, 12);
        g_tele_n--;
    }
    if (g_naway && frame % 10 == 0) {  // (put-away ones: their bodies, and whether the game let them go)
        int left = 0;
        for (int k = 1; k < MAXNEW; k++) {
            if (!g_away[k]) continue;
            uint64_t n = g_node[k], v = 0, np = 0, a = 0, b = 0, body = 0;
            char nm[16] = "", want[16];
            snprintf(want, sizeof want, "newnpc_%03d", k);
            if (!rd(n, &v, 8) || v != NODE_VTABLE || !rd(n + 0x2b8, &np, 8) || !rd(np, nm, 15) || strncmp(nm, want, 10)) {
                LOG("[newnpc] %s is gone: the game removed it %.1f s after it was put away\n", want, (double)(frame - g_away_at[k]) / 60.0);
                g_away[k] = 0, g_node[k] = 0, g_body[k] = 0;
                continue;
            }
            uint64_t o = g_owner[k];
            if (rd(o + 0x1a0, &a, 8) && rd(a + 8, &b, 8) && rd(b + 0x138, &body, 8) && rd(body, &v, 8) && v == BODY_VTABLE) g_body[k] = body;
            left++;
        }
        g_naway = left > 0;
    }
    if (frame % 15) return;
    static char path[512];
    if (!path[0]) {
        const char *d = getenv("LARECOMP_LIVE");
        snprintf(path, sizeof path, "%s/newnpc", d ? d : ".");
    }
    FILE *f = fopen(path, "r");
    if (!f) return;
    char line[256] = "", want[64] = "";
    if (!fgets(line, sizeof line, f)) line[0] = 0;
    fclose(f);
    unlink(path);
    float x, y, zz, yaw = 0, dx, dz, p[3];
    int rk;
    int asked = 0;
    if (sscanf(line, "spawn %f %f %f %f %63s", &x, &y, &zz, &yaw, want) >= 3) {
        float at[3] = {x, y, zz};
        asked = queue(at, yaw * (float)M_PI / 180.0f, want, 0);
    } else if (sscanf(line, "near %f %f %f %63s", &dx, &dz, &yaw, want) >= 2) {
        if (!player_pos(p)) { LOG("[newnpc] near: no player position yet\n"); return; }
        float at[3] = {p[0] + dx, p[1], p[2] + dz};
        asked = queue(at, yaw * (float)M_PI / 180.0f, want, 0);
    } else if (sscanf(line, "remove %d", &rk) == 1 && rk > 0 && rk < MAXNEW) {
        if (!g_node[rk]) { LOG("[newnpc] newnpc_%03d: none made\n", rk); return; }
        g_away_at[rk] = frame, g_away[rk] = 1, g_naway = 1;
        LOG("[newnpc] newnpc_%03d put away (node %llx)\n", rk, (unsigned long long)g_node[rk]);
    } else if (!strncmp(line, "list", 4)) {
        pthread_mutex_lock(&g_lock);
        for (int i = 0; i < g_nsnaps; i++) LOG("[newnpc] NPC data %s%s\n", g_snaps[i].data, snap_alive(&g_snaps[i]) ? "" : " (its area gone)");
        pthread_mutex_unlock(&g_lock);
    } else if (sscanf(line, "tele %f %f", &dx, &dz) == 2) {
        if (!player_pos(p)) { LOG("[newnpc] tele: no player position yet\n"); return; }
        g_tele[0] = p[0] + dx, g_tele[1] = p[1] + 2, g_tele[2] = p[2] + dz, g_tele_n = 20;
        LOG("[newnpc] the player goes to %.1f %.1f %.1f\n", g_tele[0], g_tele[1], g_tele[2]);
    } else if (sscanf(line, "goto %f %f %f", &x, &y, &zz) == 3) {
        g_tele[0] = x, g_tele[1] = y, g_tele[2] = zz, g_tele_n = 20;
        LOG("[newnpc] the player goes to %.1f %.1f %.1f\n", x, y, zz);
    } else LOG("[newnpc] unknown command: %s", line);
    if (asked) LOG("[newnpc] asked (newnpc_%03d): %s", asked, line);
}

// ---- Characters of the mod API's scripts (runtime/modapi.h, hle/modapi.c): new townspeople made the same way (the
// placement spawn again, from a kept station spawn - the model asked for, else the nearest townsperson's), named
// luanpc_<k>. A script's character is a slot that lives on while the script keeps it: it is made when its spot is near
// the player (the game realises people within ~60 m, 2026-11-17) and a station of a loaded area is kept to make it from;
// when it goes (its area unloaded, a trip away, a loading screen) it waits and is made again at its last spot, its walk
// carried on. Moved by its node (as the shared world's stand-ins: node_hook in hle/warp.c calls newnpc_node, at each
// update of a node - its local transform written before the game's matrices are made from it), walking with its own
// animation ("move" 0.5 / 1 on its blackboard - the player's own locomotion values); y between the two points of a walk
// (no ground probe). Removed = put away under the street, the game lets it go itself (as above).
#include "../modapi.h"
#define MAXMOD 64
#define MOD_RANGE 50.0f          // (made when the player is this near its spot, 2D)
#define MOD_WALK 1.3f            // (m/s: a townsperson's stroll, "move" 0.5)
#define MOD_RUN 4.2f             // ("move" 1)
#define MOD_TURN 7.0f            // (rad/s)
typedef struct {
    int used, removing;
    uint32_t gen;
    char model[64], label[32];
    float at[3], yaw;            // (where it is - its node's while up - or is to be made; radians)
    int state;                   // (MODNPC_WAITING / COMING / UP; walking: below)
    int k, snap;                 // (luanpc_<k>; the kept spawn it was made from)
    uint64_t node, owner, body, asked_ns, up_ns;
    int misses, told_none;
    uint64_t want_ns;            // (near, waiting for a station of its look since: after 10 s any townsperson's - any_look)
    int any_look;
    int outfit;                  // (1: our player character's clothes - npc_outfit "player"; hle/warp.c dress_hook)
    struct { uint64_t hash; char kind; float v; } anim[8];  // (animation parameters to set - npc_anim, the char manager's update)
    volatile int nanim;
    // held where scripts put it (walked / set): its node's transform written at each update
    volatile int over, walking, run, drawn_ok;
    float goal[3], from[3], drawn[3], drawn_yaw, want_yaw;
    // a walk the game's own navigation does (FObj_Move_To_PosXYZ_Navi from the script - hle/za_script.c): not moved here,
    // only followed - where it has got to, the walk over within 1 m of the goal; 1.5 s without getting on: walked here
    volatile int navi;
    float navi_last[3];
    uint64_t navi_ns;
    uint64_t step_ns;
    // its walk animation
    uint64_t bb, scratch, move_ns, bb_ns;
    uint32_t move_idx;
    int move_ok;
    float move_given;
} ModNpc;
static ModNpc g_mod[MAXMOD];
volatile int g_newnpc_mods;  // (one in use: hle/warp.c's hooks call newnpc_node / newnpc_body)
static pthread_mutex_t g_mlock = PTHREAD_MUTEX_INITIALIZER;
static int g_mk;
static uint64_t g_over_nodes[MAXMOD], g_away_nodes[MAXMOD];  // (the nodes the node hook looks for: kept short)
static uint64_t g_over_bodies[MAXMOD];  // (their bodies: their collision kept where they are drawn)
static int g_over_slot[MAXMOD];
static volatile int g_nover, g_naway_m, g_nbody;
static struct { uint64_t node, owner, body, since_ns; int k; } g_maway[MAXMOD];
static volatile uint64_t g_area_id, g_area_seen_ns;
static volatile int g_area_burst;
static char g_area_name[64], g_area_next[64];
static void area_seen(const char *data) {  // (spawn_hook: a station's person made - its NPC data "npc_A04_z02_02_0002")
    g_area_seen_ns = plat_now_ns();
    if (g_area_burst) return;
    g_area_burst = 1;
    const char *u = data;  // (the area: "A04" of npc_A04_..., else the data's name)
    if (!strncmp(u, "npc_A", 5)) snprintf(g_area_next, sizeof g_area_next, "%.3s", u + 4);
    else snprintf(g_area_next, sizeof g_area_next, "%.40s", u);
}

// (a quick filter: bit (address >> 4) & 63 of each node / body the hooks look for - the node hook runs for every node
// of the world, ~2500 times a frame in Lumiose: most are turned away by one test)
static volatile uint64_t g_node_mask, g_body_mask;
static uint64_t mask_bit(uint64_t a) { return 1ull << ((a >> 4) & 63); }
static void mod_nodes_rebuild(void) {  // (g_mlock held)
    int n = 0, a = 0, b = 0;
    for (int i = 0; i < MAXMOD; i++)
        if (g_mod[i].used && g_mod[i].node && g_mod[i].over) {
            g_over_nodes[n++] = g_mod[i].node;
            if (g_mod[i].body) g_over_slot[b] = i, g_over_bodies[b++] = g_mod[i].body;
        }
    for (int i = 0; i < MAXMOD; i++)
        if (g_maway[i].node) g_away_nodes[a++] = g_maway[i].node;
    uint64_t nm = 0, bm = 0;
    for (int i = 0; i < n; i++) nm |= mask_bit(g_over_nodes[i]);
    for (int i = 0; i < a; i++) nm |= mask_bit(g_away_nodes[i]);
    for (int i = 0; i < b; i++) bm |= mask_bit(g_over_bodies[i]);
    for (int i = 0; i < MAXMOD; i++) if (g_maway[i].body) bm |= mask_bit(g_maway[i].body);
    g_nover = n, g_naway_m = a, g_nbody = b, g_node_mask = nm, g_body_mask = bm;
}
static ModNpc *mod_of(int h) {
    int i = (h & 0xff) - 1;
    if (h <= 0 || i < 0 || i >= MAXMOD) return NULL;
    ModNpc *m = &g_mod[i];
    return m->used && !m->removing && (m->gen & 0x3fffff) == ((uint32_t)h >> 8) ? m : NULL;
}
// the name the game knows it by (its object luanpc_<k>: the mod API's npc_object, for the game's own FObj_* calls)
int newnpc_mod_object(int h, char *out, int cap) {
    pthread_mutex_lock(&g_mlock);
    ModNpc *m = mod_of(h);
    int ok = m && m->state == MODNPC_UP && m->k > 0;
    if (ok) snprintf(out, (size_t)cap, "luanpc_%03d", m->k);
    pthread_mutex_unlock(&g_mlock);
    return ok;
}
static void modnpc_made_from(int i, int snap) {  // (do_spawn, the game thread: which kept spawn it was made from; -1 none)
    if (i < 0 || i >= MAXMOD) return;
    pthread_mutex_lock(&g_mlock);
    ModNpc *m = &g_mod[i];
    if (m->used && snap < 0 && m->state == MODNPC_COMING) m->state = MODNPC_WAITING;
    else if (m->used) m->snap = snap;
    pthread_mutex_unlock(&g_mlock);
}
static void put_away(uint64_t node, uint64_t owner, uint64_t body, int k) {  // (g_mlock held)
    for (int j = 0; j < MAXMOD; j++)
        if (!g_maway[j].node) {
            g_maway[j].node = node, g_maway[j].owner = owner, g_maway[j].body = body, g_maway[j].since_ns = plat_now_ns(), g_maway[j].k = k;
            break;
        }
    mod_nodes_rebuild();
    void warp_node_hook_on(void);
    warp_node_hook_on();
}
static void modnpc_up(int k, uint64_t node, uint64_t owner) {  // (newnpc_made, the game thread)
    pthread_mutex_lock(&g_mlock);
    for (int i = 0; i < MAXMOD; i++) {
        ModNpc *m = &g_mod[i];
        if (!m->used || m->k != k) continue;
        if (m->removing) {  // (removed while the game made it: away at once)
            put_away(node, owner, 0, k);
            memset(m, 0, sizeof *m);
            LOG("[modnpc] luanpc_%03d made after its script removed it: put away\n", k);
            break;
        }
        m->node = node, m->owner = owner, m->body = 0, m->state = MODNPC_UP, m->up_ns = plat_now_ns(), m->misses = 0;
        m->drawn_ok = 0, m->move_ok = 0, m->bb = 0, m->move_given = -1;
        if (m->walking || m->over) {  // (a walk or a place carried on from before it went: held from the start, at its spot)
            m->over = 1;
            memcpy(m->drawn, m->at, 12), m->drawn_yaw = m->yaw, m->drawn_ok = 1, m->step_ns = 0;
            memcpy(m->from, m->at, 12);
        }
        mod_nodes_rebuild();
        LOG("[modnpc] luanpc_%03d (%s) is up: node %llx, character %llx\n", k, m->label, (unsigned long long)node, (unsigned long long)owner);
        break;
    }
    pthread_mutex_unlock(&g_mlock);
}

// (the node hook, the game thread, every node's every update - fast when it isn't one of ours)
static void modnpc_move(Cpu *c, ModNpc *m, uint64_t n) {
    float *q = (float *)(uintptr_t)(n + 0xd0), *t = (float *)(uintptr_t)(n + 0xe0);
    uint64_t now = plat_now_ns();
    float dt = m->step_ns ? (float)((double)(now - m->step_ns) / 1e9) : 0;
    if (dt > 0.1f) dt = 0.1f;
    if (dt < 0) dt = 0;
    m->step_ns = now;
    if (!m->drawn_ok) {  // (where the game has it now)
        memcpy(m->drawn, t, 12);
        m->drawn_yaw = 2.0f * atan2f(q[1], q[3]), m->drawn_ok = 1;
        memcpy(m->from, t, 12);
    }
    if (m->navi) {  // (the game walks it: followed, not moved - its node, its body and its walk animation are the game's)
        memcpy(m->drawn, t, 12);
        m->drawn_yaw = m->want_yaw = 2.0f * atan2f(q[1], q[3]);
        memcpy(m->at, m->drawn, 12), m->yaw = m->drawn_yaw;
        float dx = m->goal[0] - t[0], dz = m->goal[2] - t[2];
        float mx = t[0] - m->navi_last[0], mz = t[2] - m->navi_last[2];
        if (mx * mx + mz * mz > 0.09f) memcpy(m->navi_last, t, 12), m->navi_ns = now;
        if (m->walking && dx * dx + dz * dz < 1.0f) m->walking = 0, m->navi = 0, m->state = MODNPC_UP;
        else if (m->walking && now - m->navi_ns > 1500000000ull) {
            // (the game's walk not getting it anywhere for 1.5 s: walked here instead, straight on - a Town Walk walker
            // took one leg of the game's walk and then no more orders, 2026-11-19, the game's own move calls answering)
            static int nlog;
            if (nlog++ < 8) LOG("[modnpc] %s: the game's walk doesn't move it - walked straight on\n", m->label);
            m->navi = 0, memcpy(m->from, m->drawn, 12);
        } else {
            m->move_given = -1;
            return;
        }
    }
    if (m->walking) {
        float dx = m->goal[0] - m->drawn[0], dz = m->goal[2] - m->drawn[2], d = sqrtf(dx * dx + dz * dz);
        float step = (m->run ? MOD_RUN : MOD_WALK) * dt;
        if (d <= step || d < 0.02f) {
            memcpy(m->drawn, m->goal, 12);
            m->walking = 0, m->state = MODNPC_UP;
        } else {
            m->drawn[0] += dx / d * step, m->drawn[2] += dz / d * step;
            float fx = m->goal[0] - m->from[0], fz = m->goal[2] - m->from[2], all = sqrtf(fx * fx + fz * fz);
            float done = all > 0.01f ? 1.0f - (d - step) / all : 1.0f;
            m->drawn[1] = m->from[1] + (m->goal[1] - m->from[1]) * (done < 0 ? 0 : done > 1 ? 1 : done);
            m->want_yaw = atan2f(dx, dz);
        }
    }
    float dy = m->want_yaw - m->drawn_yaw;  // (turned toward where it goes / was told to face)
    while (dy > (float)M_PI) dy -= 2 * (float)M_PI;
    while (dy < -(float)M_PI) dy += 2 * (float)M_PI;
    float tmax = MOD_TURN * dt;
    m->drawn_yaw += dy > tmax ? tmax : dy < -tmax ? -tmax : dy;
    memcpy(t, m->drawn, 12);
    q[0] = 0, q[1] = sinf(m->drawn_yaw * 0.5f), q[2] = 0, q[3] = cosf(m->drawn_yaw * 0.5f);
    memcpy(m->at, m->drawn, 12), m->yaw = m->drawn_yaw;
    // its walk: "move" 0.5 / 1 while walking, 0 standing - given on a change and again every second (its own AI sets its
    // own when it changes state)
    if (!m->move_ok && now - m->bb_ns > 500000000ull) {
        uint64_t warp_anim_bb(uint64_t node);
        bool warp_anim_move_index(uint64_t bb, uint32_t *idx);
        m->bb_ns = now, m->bb = warp_anim_bb(n);
        m->move_ok = m->bb && warp_anim_move_index(m->bb, &m->move_idx);
        if (m->move_ok && !m->scratch) m->scratch = (uint64_t)(uintptr_t)hle_alloc(16);
    }
    float want = m->walking ? (m->run ? 1.0f : 0.5f) : 0.0f;
    if (m->move_ok && (want != m->move_given || now - m->move_ns > 1000000000ull)) {
        void warp_anim_set_move(Cpu *c, uint64_t bb, uint32_t idx, float val, uint64_t scratch);
        warp_anim_set_move(c, m->bb, m->move_idx, want, m->scratch);
        m->move_given = want, m->move_ns = now;
    }
}
// (research LARECOMP_MODNPC_STATS=1: what the node hook and the frame's bookkeeping cost, logged every 10 s)
static int g_mstats = -1;
static uint64_t g_ms_calls, g_ms_ns, g_ms_frame_ns, g_ms_frames, g_ms_all;
static void modnpc_node_(Cpu *c, uint64_t n);
// ---- An object's animation parameters by its name (the mod API's object_anim): what a script made through the game's
// own framework - pokemon.spawn's luapoke_<n> - has no character slot here. Its node is found by its name (+0x2b0 {hash,
// char *, len}) at its next update (this hook sees every node) and the parameter set there through its blackboard, as a
// character's (warp_anim_param). 2026-11-19: once_action_int + once_action_trigger play a Pokemon's once-action motion -
// a new one too (Studio rig: motionadd). Asked ones not found in 2 s are dropped.
#define NOANIM 16
static struct { char name[48]; uint64_t hash; char kind; float v; uint64_t asked; volatile int on; } g_oanim[NOANIM];
static volatile int g_noanim;
static uint64_t g_oanim_scratch;
static pthread_mutex_t g_olock = PTHREAD_MUTEX_INITIALIZER;
int newnpc_object_anim(const char *object, const char *param, int kind, float value) {
    if (!object || !param || !*object || strlen(object) >= 48) return 0;
    pthread_mutex_lock(&g_olock);
    int ok = 0;
    for (int i = 0; i < NOANIM && !ok; i++)
        if (!g_oanim[i].on) {
            snprintf(g_oanim[i].name, sizeof g_oanim[i].name, "%s", object);
            g_oanim[i].hash = fnv(param), g_oanim[i].kind = (char)kind, g_oanim[i].v = value, g_oanim[i].asked = plat_now_ns();
            g_oanim[i].on = 1, g_noanim++, ok = 1;
        }
    pthread_mutex_unlock(&g_olock);
    if (ok) {
        if (!g_oanim_scratch) g_oanim_scratch = (uint64_t)(uintptr_t)hle_alloc(16);
        extern volatile int g_newnpc_mods;
        void warp_node_hook_on(void);
        g_newnpc_mods = 1, warp_node_hook_on();
    }
    return ok;
}
static void oanim_check(Cpu *c, uint64_t n) {
    uint64_t v = 0, p = 0, len = 0;
    if (!rd(n, &v, 8) || v != NODE_VTABLE || !rd(n + 0x2b8, &p, 8) || !rd(n + 0x2c0, &len, 8) || !p || len == 0 || len >= 48) return;
    char nm[48];
    if (!rd(p, nm, len)) return;
    nm[len] = 0;
    uint64_t now = plat_now_ns();
    pthread_mutex_lock(&g_olock);
    for (int i = 0; i < NOANIM; i++) {  // (in the order asked: an int before the trigger that reads it)
        if (!g_oanim[i].on) continue;
        if (now - g_oanim[i].asked > 2000000000ull) {
            LOG("[modnpc] %s: no such object (its animation parameter not set)\n", g_oanim[i].name);
            g_oanim[i].on = 0, g_noanim--;
            continue;
        }
        if (strcmp(nm, g_oanim[i].name)) continue;
        bool warp_anim_param(Cpu *c, uint64_t node, uint64_t hash, char kind, float val, uint64_t scratch);
        if (!warp_anim_param(c, n, g_oanim[i].hash, g_oanim[i].kind, g_oanim[i].v, g_oanim_scratch))
            LOG("[modnpc] %s: its animation has no such parameter (%c %016llx)\n", nm, g_oanim[i].kind, (unsigned long long)g_oanim[i].hash);
        g_oanim[i].on = 0, g_noanim--;
    }
    pthread_mutex_unlock(&g_olock);
}
// ---- Game objects followed by their name (speech bubbles over them: runtime/bubble.h's BUBBLE_OBJECT): a name asked
// for is looked for among the nodes this hook sees (+0x2b8 its name, +0xe0 its position, as the player's) and its
// position kept; one not asked for in 5 s is let go.
#define NOPOS 16
static struct { char name[48]; float p[3]; uint64_t seen, asked; volatile int on; } g_opos[NOPOS];
static volatile int g_nopos;
static pthread_mutex_t g_plock = PTHREAD_MUTEX_INITIALIZER;
// 1 its position (seen in the last second), 0 not seen yet / lately (asked for: followed from now on)
int newnpc_object_pos(const char *name, float out[3]) {
    if (!name || !*name || strlen(name) >= 48) return -1;
    uint64_t now = plat_now_ns();
    int r = 0, i, free_i = -1;
    pthread_mutex_lock(&g_plock);
    for (i = 0; i < NOPOS; i++) {
        if (!g_opos[i].on) { if (free_i < 0) free_i = i; continue; }
        if (strcmp(g_opos[i].name, name)) continue;
        g_opos[i].asked = now;
        if (g_opos[i].seen && now - g_opos[i].seen < 1000000000ull) memcpy(out, g_opos[i].p, 12), r = 1;
        break;
    }
    if (i == NOPOS && free_i >= 0) {
        snprintf(g_opos[free_i].name, sizeof g_opos[free_i].name, "%s", name);
        g_opos[free_i].seen = 0, g_opos[free_i].asked = now, g_opos[free_i].on = 1, g_nopos++;
    }
    pthread_mutex_unlock(&g_plock);
    if (i == NOPOS && free_i >= 0) {
        extern volatile int g_newnpc_mods;
        void warp_node_hook_on(void);
        g_newnpc_mods = 1, warp_node_hook_on();
    }
    return r;
}
static void opos_check(uint64_t n) {  // (every node, every frame while names are asked for: no lock unless it's one)
    static unsigned calls;
    if ((++calls & 4095) == 0) {  // (now and then: names nobody asked for in 5 s let go)
        uint64_t now = plat_now_ns();
        pthread_mutex_lock(&g_plock);
        for (int i = 0; i < NOPOS; i++)
            if (g_opos[i].on && now - g_opos[i].asked > 5000000000ull) g_opos[i].on = 0, g_nopos--;
        pthread_mutex_unlock(&g_plock);
    }
    uint64_t v = 0, p = 0, len = 0;
    if (!rd(n, &v, 8) || v != NODE_VTABLE || !rd(n + 0x2b8, &p, 8) || !rd(n + 0x2c0, &len, 8) || !p || len == 0 || len >= 48) return;
    char nm[48];
    if (!rd(p, nm, len)) return;
    nm[len] = 0;
    for (int i = 0; i < NOPOS; i++) {
        if (!g_opos[i].on || g_opos[i].name[0] != nm[0] || strcmp(nm, g_opos[i].name)) continue;
        float q[3];
        if (!rd(n + 0xe0, q, 12)) continue;
        pthread_mutex_lock(&g_plock);
        memcpy(g_opos[i].p, q, 12), g_opos[i].seen = plat_now_ns();
        pthread_mutex_unlock(&g_plock);
    }
}
void newnpc_node(Cpu *c, uint64_t n) {
    if (g_mstats > 0) __atomic_add_fetch(&g_ms_all, 1, __ATOMIC_RELAXED);
    if (g_noanim) oanim_check(c, n);
    if (g_nopos) opos_check(n);
    if (!(g_node_mask & mask_bit(n))) return;
    if (g_mstats <= 0) { modnpc_node_(c, n); return; }
    uint64_t k = __atomic_add_fetch(&g_ms_calls, 1, __ATOMIC_RELAXED);
    if (k & 63) { modnpc_node_(c, n); return; }  // (one call in 64 timed: the clock costs more than the call)
    uint64_t t0 = plat_now_ns();
    modnpc_node_(c, n);
    __atomic_add_fetch(&g_ms_ns, (plat_now_ns() - t0) * 64, __ATOMIC_RELAXED);
}
static void modnpc_node_(Cpu *c, uint64_t n) {
    int no = g_nover, na = g_naway_m;
    for (int j = 0; j < na; j++)
        if (g_away_nodes[j] == n) {
            uint64_t v = 0;
            if (rd(n, &v, 8) && v == NODE_VTABLE) {
                float *t = (float *)(uintptr_t)(n + 0xe0);
                if (t[1] > AWAY_Y + 100.0f) t[1] = AWAY_Y;
            }
            return;
        }
    for (int j = 0; j < no; j++)
        if (g_over_nodes[j] == n) {
            uint64_t v = 0;
            if (!rd(n, &v, 8) || v != NODE_VTABLE) return;
            for (int i = 0; i < MAXMOD; i++)
                if (g_mod[i].used && g_mod[i].node == n && g_mod[i].over) { modnpc_move(c, &g_mod[i], n); break; }
            return;
        }
}
// (the body step hook, the game thread): a walked one's body (its collision) where it is drawn - LARECOMP_MODNPC_BODY=0
// leaves it where the game has it (research); a put-away one's body stays under the street too
void newnpc_body(uint64_t b) {
    if (!(g_body_mask & mask_bit(b))) return;
    static int follow = -1;
    if (follow < 0) follow = !getenv("LARECOMP_MODNPC_BODY") || atoi(getenv("LARECOMP_MODNPC_BODY")) != 0;
    int nb = g_nbody;
    for (int j = 0; j < nb && follow; j++)
        if (g_over_bodies[j] == b) {
            ModNpc *m = &g_mod[g_over_slot[j]];
            uint64_t v = 0;
            // (its body - its collision - where it is drawn. Its height isn't the ground's: a townsperson's body follows its
            // node, no physics under it - tried 2026-11-19, a walk to a spot 2.5 m up rose with it)
            if (m->used && m->body == b && m->drawn_ok && !m->navi && rd(b, &v, 8) && v == BODY_VTABLE) memcpy((void *)(uintptr_t)(b + 0x90), m->drawn, 12);
            return;
        }
    if (!g_naway_m) return;
    for (int j = 0; j < MAXMOD; j++)
        if (g_maway[j].body == b && b) {
            float *t = (float *)(uintptr_t)(b + 0x90);
            if (t[1] > AWAY_Y + 100.0f) t[1] = AWAY_Y;
            return;
        }
}

static uint64_t body_of_owner(uint64_t o) {
    uint64_t a = 0, b = 0, body = 0, v = 0;
    if (o && rd(o + 0x1a0, &a, 8) && rd(a + 8, &b, 8) && rd(b + 0x138, &body, 8) && rd(body, &v, 8) && v == BODY_VTABLE) return body;
    return 0;
}
static bool node_is(uint64_t n, int k) {  // (still our luanpc_<k>)
    uint64_t v = 0, np = 0;
    char nm[16] = "", want[16];
    if (!n || !rd(n, &v, 8) || v != NODE_VTABLE || !rd(n + 0x2b8, &np, 8) || !rd(np, nm, 15)) return false;
    nm[15] = 0;
    snprintf(want, sizeof want, "luanpc_%03d", k);
    return !strcmp(nm, want);
}
// a kept station spawn alive that fits (model: part of its NPC data's name, "" any townsperson)
static bool snap_fits(const char *model) {
    bool ok = false;
    pthread_mutex_lock(&g_lock);
    for (int i = 0; i < g_nsnaps && !ok; i++)
        ok = snap_alive(&g_snaps[i]) && (model[0] ? strstr(g_snaps[i].data, model) != NULL : !g_snaps[i].bad);
    pthread_mutex_unlock(&g_lock);
    return ok;
}
bool warp_player_now(float pos[3], float *yaw);
// (each frame, the render thread: newnpc_frame) made when near and makeable, waiting again when gone
static void modnpc_frame_(uint64_t frame);
static void modnpc_frame(uint64_t frame) {
    if (g_mstats < 0) g_mstats = getenv("LARECOMP_MODNPC_STATS") != NULL;
    if (g_mstats <= 0) { modnpc_frame_(frame); return; }
    uint64_t t0 = plat_now_ns();
    modnpc_frame_(frame);
    g_ms_frame_ns += plat_now_ns() - t0, g_ms_frames++;
    if (g_ms_frames % 600 == 0)
        LOG("[modnpc] cost: node hook %.1f calls a frame (%.1f past the filter), %.3f us a frame there (%.0f ns a call); bookkeeping %.2f us a frame\n",
            (double)g_ms_all / 600, (double)g_ms_calls / 600, (double)g_ms_ns / 600 / 1000.0, g_ms_calls ? (double)g_ms_ns / g_ms_calls : 0.0,
            (double)g_ms_frame_ns / 600 / 1000.0), g_ms_calls = g_ms_ns = g_ms_frame_ns = g_ms_all = 0;
}
static void modnpc_frame_(uint64_t frame) {
    uint64_t now = plat_now_ns();
    if (g_area_burst && now - g_area_seen_ns > 1500000000ull) {  // (a batch of station people made: an area loaded)
        g_area_burst = 0, g_area_id++;
        snprintf(g_area_name, sizeof g_area_name, "%s", g_area_next);
        LOG("[modnpc] area loaded (%llu): %s\n", (unsigned long long)g_area_id, g_area_name);
    }
    if (!g_newnpc_mods || frame % 5) return;
    float me[3];
    bool have_me = warp_player_now(me, NULL);
    pthread_mutex_lock(&g_mlock);
    bool changed = false;
    for (int i = 0; i < MAXMOD; i++) {
        ModNpc *m = &g_mod[i];
        if (!m->used) continue;
        if (m->removing) {
            if (now - m->asked_ns > 8000000000ull) memset(m, 0, sizeof *m), changed = true;  // (never made: forgotten)
            continue;
        }
        if (m->state == MODNPC_UP) {
            if (!node_is(m->node, m->k)) {  // (gone with its area, or far: made again when it can be)
                LOG("[modnpc] luanpc_%03d (%s) went (its area unloaded or the player far): waits at %.1f %.1f %.1f\n", m->k, m->label, m->at[0],
                    m->at[1], m->at[2]);
                m->state = MODNPC_WAITING, m->node = m->owner = m->body = 0, m->drawn_ok = 0, changed = true;
                continue;
            }
            if (!m->over) {  // (where the game has it)
                const float *t = (const float *)(uintptr_t)(m->node + 0xe0), *q = (const float *)(uintptr_t)(m->node + 0xd0);
                memcpy(m->at, t, 12), m->yaw = 2.0f * atan2f(q[1], q[3]);
            }
            if (!m->body && (m->body = body_of_owner(m->owner))) changed = true;
            continue;
        }
        if (m->state == MODNPC_COMING) {
            if (now - m->asked_ns > 6000000000ull) {
                LOG("[modnpc] luanpc_%03d (%s) wasn't made within 6 s: asked again later\n", m->k, m->label);
                m->state = MODNPC_WAITING, m->misses++;
                extern void newnpc_mod_missed(int snap);
                if (m->snap >= 0) newnpc_mod_missed(m->snap);
            }
            continue;
        }
        // waiting: near enough, a station to make it from?
        if (frame % 15 || !have_me) continue;
        float dx = m->at[0] - me[0], dz = m->at[2] - me[2];
        if (dx * dx + dz * dz > MOD_RANGE * MOD_RANGE) continue;
        if (m->misses && now - m->asked_ns < (uint64_t)(m->misses < 5 ? m->misses : 5) * 2000000000ull) continue;  // (after a miss: a while)
        const char *look = m->any_look ? "" : m->model;
        pthread_mutex_unlock(&g_mlock);
        bool fits = snap_fits(look);
        pthread_mutex_lock(&g_mlock);
        if (!m->used || m->state != MODNPC_WAITING) continue;
        if (!fits) {
            if (!m->told_none++) LOG("[modnpc] %s waits: no station %s%s kept in a loaded area to make it from\n", m->label, look[0] ? "with " : "of a townsperson", look);
            if (!m->want_ns) m->want_ns = now;
            else if (look[0] && now - m->want_ns > 10000000000ull) {  // (its look isn't here - not loaded at this point of the story,
                m->any_look = 1, m->told_none = 0;                      //  say: the nearest townsperson's rather than none)
                LOG("[modnpc] %s: no station of %s in 10 s near it - made with the nearest townsperson's look\n", m->label, m->model);
            }
            continue;
        }
        m->k = ++g_mk, m->state = MODNPC_COMING, m->asked_ns = now, m->snap = -1;
        if (!queue_as(m->at, m->yaw, look, 1, i + 1, m->k)) m->state = MODNPC_WAITING;
        else LOG("[modnpc] luanpc_%03d (%s) asked for at %.2f %.2f %.2f\n", m->k, m->label, m->at[0], m->at[1], m->at[2]);
    }
    for (int j = 0; j < MAXMOD; j++) {  // (put-away ones: gone? their bodies)
        if (!g_maway[j].node) continue;
        int k = g_maway[j].k;
        if (!node_is(g_maway[j].node, k) || now - g_maway[j].since_ns > 30000000000ull) {
            LOG("[modnpc] luanpc_%03d gone: the game let it go %.1f s after it was put away\n", k, (double)(now - g_maway[j].since_ns) / 1e9);
            memset(&g_maway[j], 0, sizeof g_maway[j]), changed = true;
            continue;
        }
        if (!g_maway[j].body && (g_maway[j].body = body_of_owner(g_maway[j].owner))) changed = true;
    }
    if (changed) mod_nodes_rebuild();
    pthread_mutex_unlock(&g_mlock);
}
void newnpc_mod_missed(int snap) {
    pthread_mutex_lock(&g_lock);
    if (snap >= 0 && snap < g_nsnaps) g_snaps[snap].miss++;
    pthread_mutex_unlock(&g_lock);
}

// ---- the API (hle/modapi.c)
void newnpc_api_on(void) {
    if (g_api) return;
    g_api = 1;
    newnpc_on();
    void warp_make_hook_on(void), warp_follow_player(void);
    warp_make_hook_on();
    warp_follow_player();
    void warp_dress_scripts_on(void);
    warp_dress_scripts_on();  // (npc_outfit: our player's parts kept as it is made, scripts' characters dressed as made)
    LOG("[modnpc] on: townspeople of scripts' own\n");
}
int newnpc_mod_spawn(const char *model, const float pos[3], float yaw_deg, const char *label) {
    pthread_mutex_lock(&g_mlock);
    int h = 0;
    for (int i = 0; i < MAXMOD && !h; i++) {
        ModNpc *m = &g_mod[i];
        if (m->used) continue;
        uint32_t gen = m->gen + 1;
        memset(m, 0, sizeof *m);
        m->used = 1, m->gen = gen, m->state = MODNPC_WAITING, m->snap = -1;
        snprintf(m->model, sizeof m->model, "%s", model ? model : "");
        snprintf(m->label, sizeof m->label, "%s", label && *label ? label : "npc");
        memcpy(m->at, pos, 12), m->yaw = m->want_yaw = yaw_deg * (float)M_PI / 180.0f;
        h = (int)((gen & 0x3fffff) << 8 | (uint32_t)(i + 1));
        g_newnpc_mods = 1;
    }
    pthread_mutex_unlock(&g_mlock);
    if (!h) LOG("[modnpc] no free slot (%d in use)\n", MAXMOD);
    return h;
}
void newnpc_mod_remove(int h) {
    pthread_mutex_lock(&g_mlock);
    ModNpc *m = mod_of(h);
    if (m) {
        LOG("[modnpc] %s removed (luanpc_%03d)\n", m->label, m->k);
        if (m->state == MODNPC_UP && m->node) put_away(m->node, m->owner, m->body, m->k), memset(m, 0, sizeof *m) , m->gen = (uint32_t)h >> 8;
        else if (m->state == MODNPC_COMING) m->removing = 1;
        else memset(m, 0, sizeof *m), m->gen = (uint32_t)h >> 8;
        mod_nodes_rebuild();
    }
    pthread_mutex_unlock(&g_mlock);
}
void newnpc_mod_remove_all(void) {
    for (int i = 0; i < MAXMOD; i++)
        if (g_mod[i].used && !g_mod[i].removing) newnpc_mod_remove((int)((g_mod[i].gen & 0x3fffff) << 8 | (uint32_t)(i + 1)));
}
void newnpc_mod_walk(int h, const float p[3], int run) {
    pthread_mutex_lock(&g_mlock);
    ModNpc *m = mod_of(h);
    if (m) {
        memcpy(m->goal, p, 12), memcpy(m->from, m->drawn_ok ? m->drawn : m->at, 12);
        m->run = run & 1, m->walking = 1, m->over = 1;
        m->navi = (run & 2) != 0, memcpy(m->navi_last, m->at, 12), m->navi_ns = plat_now_ns();  // (run | 2: the game's walk)
        mod_nodes_rebuild();
        void warp_node_hook_on(void);
        warp_node_hook_on();
    }
    pthread_mutex_unlock(&g_mlock);
}
void newnpc_mod_stop(int h) {
    pthread_mutex_lock(&g_mlock);
    ModNpc *m = mod_of(h);
    if (m) m->walking = 0, m->navi = 0;
    pthread_mutex_unlock(&g_mlock);
}
void newnpc_mod_set(int h, const float p[3], float yaw_deg) {
    pthread_mutex_lock(&g_mlock);
    ModNpc *m = mod_of(h);
    if (m) {
        m->walking = 0, m->navi = 0, m->over = 1;
        memcpy(m->at, p, 12), memcpy(m->drawn, p, 12), memcpy(m->from, p, 12);
        if (m->node) m->drawn_ok = 1;
        if (yaw_deg < 1e8f) m->want_yaw = yaw_deg * (float)M_PI / 180.0f, m->yaw = m->want_yaw;
        if (!m->node) m->drawn_yaw = m->want_yaw;
        mod_nodes_rebuild();
        void warp_node_hook_on(void);
        warp_node_hook_on();
    }
    pthread_mutex_unlock(&g_mlock);
}
int newnpc_mod_state(int h, float p[3], float *yaw) {
    pthread_mutex_lock(&g_mlock);
    ModNpc *m = mod_of(h);
    int st = 0;
    if (m) {
        st = m->state == MODNPC_UP && m->walking ? MODNPC_WALKING : m->state;
        if (p) memcpy(p, m->at, 12);
        if (yaw) *yaw = m->yaw * 57.29578f;
    }
    pthread_mutex_unlock(&g_mlock);
    return st;
}
int newnpc_mod_models(char *out, int cap) {
    float me[3];
    bool have_me = warp_player_now(me, NULL);
    int n = 0;
    out[0] = 0;
    pthread_mutex_lock(&g_lock);
    for (int i = 0; i < g_nsnaps; i++) {
        char line[200];
        bool alive = snap_alive(&g_snaps[i]);
        float dx = g_snaps[i].at[0] - me[0], dz = g_snaps[i].at[2] - me[2];
        if (!alive) snprintf(line, sizeof line, "%s - its area isn't loaded\n", g_snaps[i].data);
        else if (have_me) snprintf(line, sizeof line, "%s - %s, its station %.0f m away\n", g_snaps[i].data, g_snaps[i].bad ? "not a townsperson" : "a townsperson",
                                   sqrtf(dx * dx + dz * dz));
        else snprintf(line, sizeof line, "%s - %s\n", g_snaps[i].data, g_snaps[i].bad ? "not a townsperson" : "a townsperson");
        int l = (int)strlen(line);
        if (n + l >= cap) break;
        memcpy(out + n, line, (size_t)l + 1), n += l;
    }
    pthread_mutex_unlock(&g_lock);
    return n;
}
uint64_t newnpc_area(char *name, int cap) {
    if (name && cap > 0) snprintf(name, (size_t)cap, "%s", g_area_name);
    return g_area_id;
}

// ---- version 2: outfits, animation parameters
int newnpc_mod_outfit(int h, const char *spec) {
    pthread_mutex_lock(&g_mlock);
    ModNpc *m = mod_of(h);
    int ok = m && m->state == MODNPC_WAITING && spec && !strcmp(spec, "player");
    if (ok) m->outfit = 1;
    else if (m && spec) LOG("[modnpc] %s: outfit \"%s\" - Z-A has \"player\" (our player character's clothes) only\n", m->label, spec);
    pthread_mutex_unlock(&g_mlock);
    return ok;
}
int newnpc_mod_outfit_of_node(uint64_t node) {  // (hle/warp.c dress_hook, as a character is made)
    uint64_t np = 0;
    char nm[16] = "";
    int k;
    if (!rd(node + 0x2b8, &np, 8) || !rd(np, nm, 15)) return 0;
    nm[15] = 0;
    if (sscanf(nm, "luanpc_%d", &k) != 1) return 0;
    int r = 0;
    pthread_mutex_lock(&g_mlock);
    for (int i = 0; i < MAXMOD; i++)
        if (g_mod[i].used && g_mod[i].k == k) r = g_mod[i].outfit;
    pthread_mutex_unlock(&g_mlock);
    return r;
}
int newnpc_mod_anim(int h, const char *name, int kind, float value) {
    pthread_mutex_lock(&g_mlock);
    ModNpc *m = mod_of(h);
    int ok = m && m->nanim < 8;
    if (ok) m->anim[m->nanim].hash = fnv(name), m->anim[m->nanim].kind = (char)kind, m->anim[m->nanim].v = value, m->nanim++, g_anim_pending = 1;
    pthread_mutex_unlock(&g_mlock);
    return ok;
}
static void modnpc_anims(Cpu *c) {  // (the character manager's update: the game thread)
    bool warp_anim_param(Cpu *c, uint64_t node, uint64_t hash, char kind, float val, uint64_t scratch);
    pthread_mutex_lock(&g_mlock);
    int left = 0;
    for (int i = 0; i < MAXMOD; i++) {
        ModNpc *m = &g_mod[i];
        if (!m->used || !m->nanim) continue;
        if (m->state != MODNPC_UP || !node_is(m->node, m->k)) { left = 1; continue; }  // (set once it is up)
        if (!m->scratch) m->scratch = (uint64_t)(uintptr_t)hle_alloc(16);
        for (int k = 0; k < m->nanim; k++)
            if (!warp_anim_param(c, m->node, m->anim[k].hash, m->anim[k].kind, m->anim[k].v, m->scratch))
                LOG("[modnpc] %s: its animation has no such parameter (%c %016llx)\n", m->label, m->anim[k].kind, (unsigned long long)m->anim[k].hash);
        m->nanim = 0;
    }
    g_anim_pending = left;
    pthread_mutex_unlock(&g_mlock);
}
