// Legends Z-A workarounds (main build id B1F12FD919EAE86A), until their causes are found.
//
// Stretched legs: running, the player's feet stay where the run began - the legs stretch back toward that spot, and
// stay so after stopping. The game's biped foot IK (its BipedIK* weights, named float parameters looked up through
// 0x700134ad10(owner, key {FNV-1a 64 hash, name}) -> s0) locks the feet and never lets go. Its ground raycasts
// (0x7003014970) hit the ground as they should, and every FP/SIMD, integer and load/store instruction form the game
// uses matches a reference ARM emulator (Unicorn), so the cause is elsewhere. With those weights looked up as 0 the
// legs follow the animation: correct on flat ground, the feet just don't adapt to slopes and steps.
// LARECOMP_ZA_FOOT_IK=1 keeps the game's foot IK.
// Where the cause is (2026-10-19): the lookup returns (owner+0xe0 map [key], default 1.0) x (owner+0xa8 map [key]).
// The +0xa8 map holds the BipedIK keys at a constant 1.0; the +0xe0 map, the per-frame values that should make the
// feet let go, stays empty - nothing ever calls its setters (vtable slots 0x700134a100 / 0x700134a2d4) or the
// command handlers that also fill it (0x7001e70638, and 0x7001e707a0 for +0xa8), for any object, from boot on.
// A whole animation parameter feed never runs: what drives those handlers is the next thing to find.
// Further (2026-10-19): the setters are reached only from two script-VM natives, 0x700133b140 (script name
// "f26E9E53B": object, mode 0..2 -> one of three BipedIK keys, value) and 0x700133b390 ("fD081658F"), listed in the
// natives table at main+0x3BEF6F0 ({function, hashed name} pairs). Neither is ever called while other natives are:
// the game script that should set the per-frame weights never runs (or takes another branch). Next: find which
// script (romfs bytecode) references those names and why it doesn't run. (Not the lane-id or skipped-compute bugs
// fixed the same day: the legs still stretch with LARECOMP_ZA_FOOT_IK=1 after them.)
#include <stdlib.h>
#include <string.h>

#include "../hle.h"

void rt_register_fnhook_at(uint64_t addr, int (*fn)(Cpu *c, uint64_t addr));
void rt_call_original(Cpu *c, uint64_t addr);

#define ZA_PARAM_LOOKUP 0x700134ad10ull

static uint64_t fnv1a(const char *s) {
    uint64_t h = 0xcbf29ce484222645ull;
    for (; *s; s++) h = (h ^ (uint8_t)*s) * 0x100000001b3ull;
    return h;
}

static uint64_t g_biped[8];

static int param_lookup(Cpu *c, uint64_t addr) {
    uint64_t hash = *(const uint64_t *)(uintptr_t)c->x[1];
    rt_call_original(c, addr);
    for (int i = 0; i < 8; i++)
        if (hash == g_biped[i]) { c->v[0] = 0; break; }  // (the weight, s0)
    return 1;
}

// Choppy NPCs: an NPC's animation advances every frame, but its pose is only applied to the skeleton when a per-frame LOD
// decision allows it: a callback each animation component holds (component + 0x180, called from 0x7001e73abc) answers
// whether to evaluate this frame, kept at component + 0x1f4, and 0x7001dcdf40 (animation item -> [+0x48] component)
// applies the pose only then. NPCs standing or walking around get it once every 60 frames - 2 s at 30 fps, 0.4 s at 144 -
// while a conversation puts everyone nearby on every frame. Here it is always set: every NPC's pose follows its animation
// each frame (about 2% of the frame rate in Lumiose). LARECOMP_ZA_NPC_LOD=1 keeps the game's choice.
// Missing station wall (Lumiose Station platform: posters float in front of the hall): the wall's mesh faces the hall
// - its winding agrees with its own normals, like every other mesh - and its command list (recorded once, at load) binds
// back-face culling, so seen from the platform it is culled; nothing else in the frame draws that side. Every shader of
// the game's static meshes reads gl_FrontFacing (back faces are shaded right), and with culling off nothing else in the
// station or in Lumiose changes, at the same frame rate. Culling stays off for this game; LARECOMP_ZA_CULL=1 keeps it.
// (Why the console shows the wall is still open: the polygon state, matrices, viewport and vertex data are all ordinary.)

#define ZA_POSE_APPLY 0x7001dcdf40ull  // (x0 = animation item; [x0 + 0x48] -> its component, + 0x1f4: evaluate this frame)
volatile int g_za_npc_full = 1;  // (research: LARECOMP_LIVE <dir>/npcfull "0" / "1" switches it while the game runs)
static int pose_apply(Cpu *c, uint64_t addr) {
    if (!g_za_npc_full) return 0;
    uint64_t item = c->x[0];
    uint64_t comp = item ? *(uint64_t *)(uintptr_t)(item + 0x48) : 0;
    if (comp >= 0x7000000000ull && comp < 0x8000000000ull) *(uint8_t *)(uintptr_t)(comp + 0x1f4) = 1;
    (void)addr;
    return 0;
}

void za_fixes_configure(const char *exefs) {
    static const uint8_t ZA_MAIN[8] = {0xb1, 0xf1, 0x2f, 0xd9, 0x19, 0xea, 0xe8, 0x6a};
    static bool done;
    char p[1100];
    snprintf(p, sizeof p, "%s/main", exefs);
    FILE *f = fopen(p, "rb");
    uint8_t id[8] = {0};
    if (f) {
        fseek(f, 0x40, SEEK_SET);
        if (fread(id, 1, 8, f) != 8) memset(id, 0, 8);
        fclose(f);
    }
    if (done || memcmp(id, ZA_MAIN, 8)) return;
    done = true;
    const char *cull = getenv("LARECOMP_ZA_CULL");
    if (!(cull && atoi(cull))) {
        extern volatile int g_live_nocull;  // (nvn/vulkan/exec.c)
        g_live_nocull = 1;
        LOG("[za] face culling off (missing station wall; LARECOMP_ZA_CULL=1 keeps it)\n");
    }
    const char *lod = getenv("LARECOMP_ZA_NPC_LOD");
    if (!(lod && atoi(lod))) {
        rt_register_fnhook_at(ZA_POSE_APPLY, pose_apply);
        LOG("[za] NPC animation at the full frame rate (LARECOMP_ZA_NPC_LOD=1 keeps the game's animation LOD)\n");
    }
    const char *e = getenv("LARECOMP_ZA_FOOT_IK");
    if (e && atoi(e)) return;
    static const char *names[8] = {"BipedIKWeight", "BipedIKOffsetWeight", "BipedIKControl0Weight", "BipedIKControl1Weight",
                                   "BipedIKControl0FootWeight", "BipedIKControl1FootWeight", "BipedIKControl0ToeWeight",
                                   "BipedIKControl1ToeWeight"};
    for (int i = 0; i < 8; i++) g_biped[i] = fnv1a(names[i]);
    rt_register_fnhook_at(ZA_PARAM_LOOKUP, param_lookup);
    LOG("[za] foot IK off (stretched legs workaround; LARECOMP_ZA_FOOT_IK=1 keeps it)\n");
}
