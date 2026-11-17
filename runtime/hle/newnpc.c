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
//   LARECOMP_NEWNPC_DRESS=1: each new one wears our player's outfit (warp.c's dressing, mode 2).
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../hle.h"

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
} Snap;
static Snap g_snaps[MAXDATA];
static int g_nsnaps;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

static int g_on = -1, g_dress;
static volatile int g_pending;   // (a spawn asked for)
static float g_at[3], g_yaw;
static char g_want[64];          // (the NPC data asked for: part of its name, "" any)
static int g_count;
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
static bool rd(uint64_t a, void *o, size_t n) {
    if (a < 0x7000000000ull || a >= 0x7400000000ull) return false;
    memcpy(o, (const void *)(uintptr_t)a, n);
    return true;
}
static void name_of(uint64_t ns, char *o, size_t n) {  // (a name struct {hash, char *, len, ...})
    uint64_t p = 0, len = 0;
    o[0] = 0;
    if (rd(ns + 8, &p, 8) && rd(ns + 0x10, &len, 8) && p && len < n) memcpy(o, (const void *)(uintptr_t)p, len), o[len] = 0;
}

// ---- keeping station spawns
static int spawn_hook(Cpu *c, uint64_t addr) {
    (void)addr;
    if (c->x[30] != STATION_LR) return 0;
    char data[64];
    name_of(c->x[1], data, sizeof data);
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
    snprintf(s->data, sizeof s->data, "%s", data);
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
static int make_hook(Cpu *c, uint64_t addr) {
    uint64_t req = c->x[2], len = 0, owner = 0;
    if (rd(req + 0x18, &len, 8) && len == 6 && rd(req + 0x30, &owner, 8)) g_player_owner = owner;  // ("Player")
    else if (rd(req + 0x30, &owner, 8)) {  // (ours: its live node kept by number)
        uint64_t cb = 0, n = 0, np = 0;
        char nm[16] = "";
        int k;
        if (rd(owner + 0xc8, &cb, 8) && rd(cb + 0x38, &n, 8) && rd(n + 0x2b8, &np, 8) && rd(np, nm, 15) && sscanf(nm, "newnpc_%d", &k) == 1 &&
            k > 0 && k < MAXNEW) {
            g_node[k] = n, g_owner[k] = owner;
            LOG("[newnpc] newnpc_%03d made: its node %llx, character object %llx\n", k, (unsigned long long)n, (unsigned long long)owner);
        }
    }
    return g_prev_make ? g_prev_make(c, addr) : 0;
}
static bool player_pos(float p[3]) {
    uint64_t o = g_player_owner, v = 0, cb = 0, n = 0;
    if (!o || !rd(o + 0xc8, &cb, 8) || !rd(cb + 0x38, &n, 8) || !rd(n, &v, 8) || v != NODE_VTABLE) return false;
    return rd(n + 0xe0, p, 12);
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

// ---- the spawn
static void do_spawn(Cpu *c) {
    pthread_mutex_lock(&g_lock);
    Snap s;
    int found = -1;
    for (int i = g_nsnaps - 1; i >= 0 && found < 0; i--)  // (the newest kept that is still there and fits)
        if (snap_alive(&g_snaps[i]) && (!g_want[0] || strstr(g_snaps[i].data, g_want))) found = i;
    if (found >= 0) s = g_snaps[found];
    pthread_mutex_unlock(&g_lock);
    if (found < 0) {
        LOG("[newnpc] no station spawn kept%s%s whose area is loaded: walk near townspeople first\n", g_want[0] ? " for " : "", g_want);
        return;
    }
    int k = ++g_count;
    char *name = hle_alloc(32);
    snprintf(name, 32, "newnpc_%03d", k);
    uint8_t *nm = hle_alloc(0x40), *blk = hle_alloc(0x300), *out = hle_alloc(32);
    memcpy(nm, s.name, 0x40);
    uint64_t h = fnv(name), p = (uint64_t)(uintptr_t)name, len = strlen(name), z = 0;
    memcpy(nm, &h, 8), memcpy(nm + 8, &p, 8), memcpy(nm + 0x10, &len, 8), memcpy(nm + 0x18, &z, 8);
    memcpy(blk, s.blk, 0x300);
    uint64_t b = 0, e = 0;
    if (s.nent) {  // (its own copy of the entries: the game may take them over)
        uint8_t *ents = hle_alloc(0xc0 * (uint64_t)s.nent);
        memcpy(ents, s.ents, 0xc0 * (size_t)s.nent);
        b = (uint64_t)(uintptr_t)ents, e = b + 0xc0 * (uint64_t)s.nent;
    }
    memcpy(blk + 0x1a0, &b, 8), memcpy(blk + 0x1a8, &e, 8), memcpy(blk + 0x1b0, &e, 8);
    float *t = (float *)(blk + 0x40 + 0x30), *q = (float *)(blk + 0x40 + 0x20);
    t[0] = g_at[0], t[1] = g_at[1], t[2] = g_at[2];
    float a = g_yaw * (float)M_PI / 180.0f;
    q[0] = 0, q[1] = sinf(a / 2), q[2] = 0, q[3] = cosf(a / 2);
    if (g_dress) { extern void dress_newnpc_next(void); dress_newnpc_next(); }
    uint64_t sx[32], snzcv = c->nzcv;
    __uint128_t sv[32];
    memcpy(sx, c->x, sizeof sx), memcpy(sv, c->v, sizeof sv);
    c->x[0] = s.x0, c->x[1] = s.x1, c->x[2] = (uint64_t)(uintptr_t)nm, c->x[3] = (uint64_t)(uintptr_t)(blk + 8);
    c->x[4] = (uint64_t)(uintptr_t)(blk + 0x40), c->x[5] = (uint64_t)(uintptr_t)blk, c->x[6] = s.x6;
    c->x[8] = (uint64_t)(uintptr_t)out;
    guest_call(c, PLACE_SPAWN);
    memcpy(c->x, sx, sizeof sx), memcpy(c->v, sv, sizeof sv), c->nzcv = snzcv;
    LOG("[newnpc] spawned %s (NPC data %s) at %.2f %.2f %.2f yaw %.0f\n", name, s.data, g_at[0], g_at[1], g_at[2], g_yaw);
}
static int mgr_hook(Cpu *c, uint64_t addr) {
    (void)addr;
    if (g_pending) {
        g_pending = 0;
        do_spawn(c);
    }
    return 0;
}

// (hooks registered before plugins load: theirs chain in front of these - runtime/plugin.c)
__attribute__((constructor)) static void newnpc_init(void) {
    g_on = getenv("LARECOMP_NEWNPC") != NULL;
    if (!g_on) return;
    g_dress = getenv("LARECOMP_NEWNPC_DRESS") != NULL;
    if (g_dress) { extern void dress_newnpc_init(void); dress_newnpc_init(); }  // (before ours: chained behind it)
    rt_register_fnhook_at(PLACE_SPAWN, spawn_hook);
    rt_register_fnhook_at(CHAR_MGR, mgr_hook);
    g_prev_make = rt_fnhook_get(CC_MAKE);
    rt_register_fnhook_at(CC_MAKE, make_hook);
    g_prev_step = rt_fnhook_get(STEP_A);
    rt_register_fnhook_at(STEP_A, step_hook);
    g_prev_node = rt_fnhook_get(NODE_UPDATE);
    rt_register_fnhook_at(NODE_UPDATE, node_hook);
}

void newnpc_frame(uint64_t frame) {  // (each frame: runtime/hle/input.c; the render thread - no game calls here)
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
    if (sscanf(line, "spawn %f %f %f %f %63s", &x, &y, &zz, &yaw, want) >= 3) {
        g_at[0] = x, g_at[1] = y, g_at[2] = zz, g_yaw = yaw;
        snprintf(g_want, sizeof g_want, "%s", want), g_pending = 1;
    } else if (sscanf(line, "near %f %f %f %63s", &dx, &dz, &yaw, want) >= 2) {
        if (!player_pos(p)) { LOG("[newnpc] near: no player position yet\n"); return; }
        g_at[0] = p[0] + dx, g_at[1] = p[1], g_at[2] = p[2] + dz, g_yaw = yaw;
        snprintf(g_want, sizeof g_want, "%s", want), g_pending = 1;
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
    if (g_pending) LOG("[newnpc] asked: %s", line);
}
