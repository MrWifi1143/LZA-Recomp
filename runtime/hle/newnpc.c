// Research (LARECOMP_NEWNPC=1): a NEW townsperson made by the game at a spot we choose, not a dressed one it made.
//
// How Z-A makes its people (2026-11-16, Legends Lua traces - contrib/legendslua/research/za_newnpc.lua):
//   station NPCs  - a placement NPC spawner component (vtable 0x7003d7da80; its update 0x7000653a8c, +0x11a0)
//   crowd walkers - a traffic NPC spawner component (vtable 0x7003d7e438; 0x70009cdbcc, +0x618)
//   pairs         - 0x700049e55c; others 0x70005e5a38, 0x700060df8c
// all call the CHARACTER PLACEMENT SPAWN 0x70005e8644(x0 spawner component, x1 NPC data {name struct "npc_A04_z02_02_0002"
// / "id01" ...}, x2 the new object's name struct {u64 FNV-1a hash, char *, u64 len, interned record or 0}, x3 / x4 / x5
// one block S on the caller's stack: x5 = S, x3 = S+8, x4 = S+0x40 = a transform {+0x10 scale xyz, +0x20 quaternion xyzw,
// +0x30 position xyz}, x6 a hash; x8 out). It makes the character's placement scene object (a node, vtable 0x7003be0d28,
// named by x2 at +0x2b8) with its components (the character component, vtable 0x7003d2a8e8, size 0x2280); the
// character manager (0x70000af150, once a frame) realises it when admitted (0x70002ffeb0 -> 0x70003015e0 -> 0x7000302c90:
// the live node "_new", the model sub-template job 0x70010b6210, CharacterCreationMaster make 0x70011164d0).
//   Here: the first station spawn's arguments are kept (blocks copied into guest memory), and a command asks for the
// same call again with another name and position, made from the character manager's update (the game thread that
// realises characters). Commands: <LARECOMP_LIVE or game folder>/newnpc, one line, read every 15 frames, then removed:
//   "spawn <x> <y> <z> [<yaw degrees>]"   "near <dx> <dz>" (beside the player: the "Player" character's node)
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../hle.h"

void rt_register_fnhook_at(uint64_t addr, int (*fn)(Cpu *c, uint64_t addr));
typedef int (*FnHook)(Cpu *c, uint64_t addr);
FnHook rt_fnhook_get(uint64_t addr);
void rt_call_original(Cpu *c, uint64_t addr);
void *hle_alloc(uint64_t size);

#define PLACE_SPAWN 0x70005e8644ull
#define STATION_LR 0x7000654c30ull     // (the placement NPC spawner's call)
#define CHAR_MGR 0x70000af150ull       // (the character manager's update: once a frame, a game thread)
#define CC_MAKE 0x70011164d0ull
#define NODE_VTABLE 0x7003be0d28ull
#define CHAR_OBJ_VTABLE 0x7003be7358ull

static int g_on = -1;
static struct {
    volatile int ok;
    uint64_t x0, x1, x6;
    uint8_t *name;   // (guest copy of the name struct, 0x40)
    uint8_t *blk;    // (guest copy of S, 0x300)
    char orig[96];
    int nent;
    uint8_t *ents;
} g_snap;
static volatile int g_pending;          // (a spawn asked for)
static float g_at[3], g_yaw;
static int g_count;
static volatile uint64_t g_player_owner;
static FnHook g_prev_make;

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

static int spawn_hook(Cpu *c, uint64_t addr) {
    if (!g_snap.ok && c->x[30] == STATION_LR) {
        uint8_t *nm = hle_alloc(0x40), *blk = hle_alloc(0x300);
        memcpy(nm, (const void *)(uintptr_t)c->x[2], 0x40);
        memcpy(blk, (const void *)(uintptr_t)c->x[5], 0x300);
        // (S+0x1a0: a vector {begin, end, capacity, allocator} of 0xc0-byte entries - {u64 hash, value, ...} - in the
        // caller's heap, freed after the call: the entries copied too, the copy's vector pointed at them)
        uint64_t vb = 0, ve = 0;
        memcpy(&vb, blk + 0x1a0, 8), memcpy(&ve, blk + 0x1a8, 8);
        int ne = ve > vb && ve - vb <= 0xc0 * 32 ? (int)((ve - vb) / 0xc0) : 0;
        g_snap.nent = ne;
        if (ne) {
            uint8_t *ents = hle_alloc(0xc0 * (uint64_t)ne);
            memcpy(ents, (const void *)(uintptr_t)vb, 0xc0 * (size_t)ne);
            g_snap.ents = ents;
            // (an entry's optional values - flags +0x68 / +0xa0 / +0xb0 - hold callables and heap blocks of the caller,
            // gone after the call: a copy made later called a dead one, 2026-11-16. Dropped: the entries keep only
            // their names)
            for (int k = 0; k < ne; k++) ents[0xc0 * k + 0x68] = 0, ents[0xc0 * k + 0xa0] = 0, ents[0xc0 * k + 0xb0] = 0;
            LOG("[newnpc]   entries at %llx (S %llx)\n", (unsigned long long)vb, (unsigned long long)c->x[5]);
            for (int k = 0; k < ne; k++) {
                char t[24 * 18 + 1];
                size_t w = 0;
                for (int j = 0; j < 24; j++) {
                    uint64_t v;
                    memcpy(&v, ents + 0xc0 * k + 8 * j, 8);
                    w += (size_t)snprintf(t + w, sizeof t - w, " %llx", (unsigned long long)v);
                }
                LOG("[newnpc]   its entry %d:%s\n", k, t);
            }
        }
        g_snap.x0 = c->x[0], g_snap.x1 = c->x[1], g_snap.x6 = c->x[6], g_snap.name = nm, g_snap.blk = blk;
        uint64_t p = 0, len = 0;
        rd(c->x[2] + 8, &p, 8), rd(c->x[2] + 0x10, &len, 8);
        snprintf(g_snap.orig, sizeof g_snap.orig, "%.*s", (int)(len < 90 ? len : 90), p ? (const char *)(uintptr_t)p : "?");
        const float *t = (const float *)(uintptr_t)(c->x[4] + 0x30);
        LOG("[newnpc] kept a station spawn: spawner %llx, NPC data %llx, name %s at %.2f %.2f %.2f (x3-x5 %+lld %+lld)\n",
            (unsigned long long)c->x[0], (unsigned long long)c->x[1], g_snap.orig, t[0], t[1], t[2],
            (long long)(c->x[3] - c->x[5]), (long long)(c->x[4] - c->x[5]));
        __atomic_store_n(&g_snap.ok, 1, __ATOMIC_RELEASE);
    }
    return 0;
}

static int make_hook(Cpu *c, uint64_t addr) {
    uint64_t req = c->x[2], len = 0, owner = 0;
    static int n;
    if (n++ < 3) LOG("[newnpc] make: request +0x18 %llx\n", (unsigned long long)(rd(req + 0x18, &len, 8) ? len : 0));
    if (rd(req + 0x18, &len, 8) && len == 6 && rd(req + 0x30, &owner, 8)) g_player_owner = owner, LOG("[newnpc] the player: %llx\n", (unsigned long long)owner);  // ("Player")
    return g_prev_make ? g_prev_make(c, addr) : 0;
}
static bool player_pos(float p[3]) {
    uint64_t o = g_player_owner, v = 0, cb = 0, n = 0;
    if (!o || !rd(o + 0xc8, &cb, 8) || !rd(cb + 0x38, &n, 8) || !rd(n, &v, 8) ||
        v != NODE_VTABLE) return false;
    return rd(n + 0xe0, p, 12);
}

// the player's body (vtable 0x7003be6218, position +0x90): its object +0x1a0 -> +0x8 -> +0x138 (warp.c dressed_body_of)
static uint64_t player_body(void) {
    uint64_t o = g_player_owner, a = 0, b = 0, body = 0, v = 0;
    if (!o || !rd(o + 0x1a0, &a, 8) || !rd(a + 8, &b, 8) || !rd(b + 0x138, &body, 8) || !rd(body, &v, 8) || v != 0x7003be6218ull) return 0;
    return body;
}
static float g_tele[3];
static int g_tele_n;

static void do_spawn(Cpu *c) {
    int k = ++g_count;
    char *name = hle_alloc(64);
    snprintf(name, 64, "newnpc_%03d", k);
    uint8_t *nm = hle_alloc(0x40), *blk = hle_alloc(0x300), *out = hle_alloc(32);
    memcpy(nm, g_snap.name, 0x40);
    uint64_t h = fnv(name), p = (uint64_t)(uintptr_t)name, len = strlen(name), z = 0;
    memcpy(nm, &h, 8), memcpy(nm + 8, &p, 8), memcpy(nm + 0x10, &len, 8), memcpy(nm + 0x18, &z, 8);
    memcpy(blk, g_snap.blk, 0x300);
    if (g_snap.nent) {  // (its own copy of the entries: the game may take them over)
        uint8_t *ents = hle_alloc(0xc0 * (uint64_t)(g_snap.nent + 1));
        memcpy(ents, g_snap.ents, 0xc0 * (size_t)g_snap.nent);
        uint64_t b = (uint64_t)(uintptr_t)ents, e = b + 0xc0 * (uint64_t)g_snap.nent;
        memcpy(blk + 0x1a0, &b, 8), memcpy(blk + 0x1a8, &e, 8), memcpy(blk + 0x1b0, &e, 8);
    } else {
        uint64_t z0 = 0;
        memcpy(blk + 0x1a0, &z0, 8), memcpy(blk + 0x1a8, &z0, 8), memcpy(blk + 0x1b0, &z0, 8);
    }
    float *t = (float *)(blk + 0x40 + 0x30), *q = (float *)(blk + 0x40 + 0x20);
    t[0] = g_at[0], t[1] = g_at[1], t[2] = g_at[2];
    float a = g_yaw * (float)M_PI / 180.0f;
    q[0] = 0, q[1] = sinf(a / 2), q[2] = 0, q[3] = cosf(a / 2);
    uint64_t sx[32], snzcv = c->nzcv;
    __uint128_t sv[32];
    memcpy(sx, c->x, sizeof sx), memcpy(sv, c->v, sizeof sv);
    c->x[0] = g_snap.x0, c->x[1] = g_snap.x1, c->x[2] = (uint64_t)(uintptr_t)nm, c->x[3] = (uint64_t)(uintptr_t)(blk + 8);
    c->x[4] = (uint64_t)(uintptr_t)(blk + 0x40), c->x[5] = (uint64_t)(uintptr_t)blk, c->x[6] = g_snap.x6;
    c->x[8] = (uint64_t)(uintptr_t)out;
    guest_call(c, PLACE_SPAWN);
    memcpy(c->x, sx, sizeof sx), memcpy(c->v, sv, sizeof sv), c->nzcv = snzcv;
    uint64_t r = 0;
    memcpy(&r, out, 8);
    LOG("[newnpc] spawned %s (NPC data of %s) at %.2f %.2f %.2f yaw %.0f: out %llx\n", name, g_snap.orig, g_at[0], g_at[1], g_at[2],
        g_yaw, (unsigned long long)r);
}
static int mgr_hook(Cpu *c, uint64_t addr) {
    if (g_pending && g_snap.ok) {
        g_pending = 0;
        do_spawn(c);
    }
    return 0;
}

// (hooks registered before plugins load: theirs chain in front of these - runtime/plugin.c)
__attribute__((constructor)) static void newnpc_init(void) {
    g_on = getenv("LARECOMP_NEWNPC") != NULL;
    if (!g_on) return;
    rt_register_fnhook_at(PLACE_SPAWN, spawn_hook);
    rt_register_fnhook_at(CHAR_MGR, mgr_hook);
    g_prev_make = rt_fnhook_get(CC_MAKE);
    rt_register_fnhook_at(CC_MAKE, make_hook);
    LOG("[newnpc] on (commands in <live>/newnpc)\n");
}
void newnpc_frame(uint64_t frame) {  // (each frame: runtime/hle/input.c; the render thread - no game calls here)
    if (g_on <= 0) return;
    if (g_tele_n > 0) {  // (test: the player put somewhere - written for 20 frames, as the game settles it)
        uint64_t b = player_body();
        if (b) memcpy((void *)(uintptr_t)(b + 0x90), g_tele, 12);
        g_tele_n--;
    }
    if (frame % 15) return;
    static char path[512];
    if (!path[0]) {
        const char *d = getenv("LARECOMP_LIVE");
        snprintf(path, sizeof path, "%s/newnpc", d ? d : ".");
    }
    FILE *f = fopen(path, "r");
    if (!f) return;
    char line[256] = "";
    if (!fgets(line, sizeof line, f)) line[0] = 0;
    fclose(f);
    unlink(path);
    float x, y, zz, yaw = 0, dx, dz;
    if (sscanf(line, "spawn %f %f %f %f", &x, &y, &zz, &yaw) >= 3) {
        g_at[0] = x, g_at[1] = y, g_at[2] = zz, g_yaw = yaw, g_pending = 1;
    } else if (sscanf(line, "near %f %f %f", &dx, &dz, &yaw) >= 2) {
        float p[3];
        if (!player_pos(p)) { LOG("[newnpc] near: no player position yet\n"); return; }
        g_at[0] = p[0] + dx, g_at[1] = p[1], g_at[2] = p[2] + dz, g_yaw = yaw, g_pending = 1;
    } else if (sscanf(line, "tele %f %f", &dx, &dz) == 2) {  // (test: the player moved by dx, dz)
        float p[3];
        if (!player_pos(p)) { LOG("[newnpc] tele: no player position yet\n"); return; }
        g_tele[0] = p[0] + dx, g_tele[1] = p[1] + 2, g_tele[2] = p[2] + dz, g_tele_n = 20;
        LOG("[newnpc] the player goes to %.1f %.1f %.1f\n", g_tele[0], g_tele[1], g_tele[2]);
    } else if (sscanf(line, "goto %f %f %f", &x, &y, &zz) == 3) {
        g_tele[0] = x, g_tele[1] = y, g_tele[2] = zz, g_tele_n = 20;
        LOG("[newnpc] the player goes to %.1f %.1f %.1f\n", x, y, zz);
    } else LOG("[newnpc] unknown command: %s", line);
    if (g_pending) LOG("[newnpc] asked: %s", line);
}
