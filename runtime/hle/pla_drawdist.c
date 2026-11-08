// Draw distance (Settings.detail, ini detail_distance 0..3 = x1 / x2 / x3 / x5): how far away the field still draws
// what the Switch drops. Legends: Arceus 1.1.1's field fills its fade settings by name from the debug parameter table
// (bin/misc/app_config/field_common_config.bin names kTreeVisibleDistance, kTreeFadeDistanceNear/Far,
// kDetailFadeDistanceNear, kDetailFadeCoefficient...): f_700246746c and f_700246f818 (x0 the settings, filled from
// 0x70009ee408's lookups) store {0, 200} at +0x430 (trees visible to 200 m), the fade bands {-3, 3, 180, 200} at
// +0x440 and +0x450 (fade out 180..200 m) and 0.75 at +0x460. Found 2026-11-08 with Legends Lua: x0.05 made the trees
// beside the player vanish (grass unchanged: grass is drawn another way). The far distances (+0x434, +0x448, +0x44c,
// +0x458, +0x45c) are multiplied after the game's function; the game's own values are kept per object, so a change in
// the Settings applies at once (a remembered object is written only while it still holds what was written last:
// one the game has freed and reused is left alone). LARECOMP_PLA_DRAWDIST_LOG=1: each object and value.
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "../rt.h"
#include "../settings.h"

void rt_register_fnhook_at(uint64_t addr, int (*fn)(Cpu *c, uint64_t addr));
void rt_call_original(Cpu *c, uint64_t addr);

static const float k_mul[4] = {1.0f, 2.0f, 3.0f, 5.0f};
static const uint32_t k_off[5] = {0x434, 0x448, 0x44c, 0x458, 0x45c};
#define NOBJ 8
static struct { uint64_t obj; float game[5], put[5]; } g_obj[NOBJ];
static int g_log = -1;

static float factor(void) { return g_settings.detail >= 0 && g_settings.detail <= 3 ? k_mul[g_settings.detail] : 1.0f; }
static float *at(uint64_t obj, int i) { return (float *)(uintptr_t)(obj + k_off[i]); }

static void put(int s) {
    float k = factor();
    for (int i = 0; i < 5; i++) *at(g_obj[s].obj, i) = g_obj[s].put[i] = g_obj[s].game[i] * k;
    if (g_log) LOG("[drawdist] %llx x%g: trees visible %.0f m, fade %.0f..%.0f m\n", (unsigned long long)g_obj[s].obj, k,
                   *at(g_obj[s].obj, 0), *at(g_obj[s].obj, 1), *at(g_obj[s].obj, 2));
}

static int fade_hook(Cpu *c, uint64_t addr) {
    uint64_t obj = c->x[0];
    rt_call_original(c, addr);
    float vis = *at(obj, 0);
    if (!(vis > 1.0f && vis < 100000.0f)) return 1;  // (not the layout this was found in: left alone)
    int s = 0;
    while (s < NOBJ - 1 && g_obj[s].obj && g_obj[s].obj != obj) s++;
    g_obj[s].obj = obj;
    for (int i = 0; i < 5; i++) g_obj[s].game[i] = *at(obj, i);  // (the game's own, just written)
    put(s);
    static float dmul = -1;  // (research) LARECOMP_PLA_DETAIL_MUL=<k>: the detail fade pair at +0x438 (1, 2) x k
    if (dmul < 0) dmul = getenv("LARECOMP_PLA_DETAIL_MUL") ? (float)atof(getenv("LARECOMP_PLA_DETAIL_MUL")) : 0;
    if (dmul > 0) {
        float *d = (float *)(uintptr_t)(obj + 0x438);
        d[0] *= dmul, d[1] *= dmul;
        LOG("[drawdist] detail fade x%g: %g %g\n", dmul, d[0], d[1]);
    }
    static float amul = -1;  // (research) LARECOMP_PLA_FADE_ALL=<k>: every float +0x430..+0x45c x k
    if (amul < 0) amul = getenv("LARECOMP_PLA_FADE_ALL") ? (float)atof(getenv("LARECOMP_PLA_FADE_ALL")) : 0;
    if (amul > 0)
        for (uint32_t o = 0x430; o < 0x460; o += 4) *(float *)(uintptr_t)(obj + o) *= amul;
    return 1;
}

// NPC load radius: f_700173d4b0(slot, r) gives each NPC slot of the field its radius in whole metres (+0x450 = r,
// +0x454 = r - 10, +0x458 = r + 20: an NPC loads inside r and unloads past r + 20; +0x330 holds its squared distance to
// the player). The field passes app_config's kArea00NpcLoadRadius (60 in the village) and kArea00NpcPokeLoadRadius (40)
// - an NPC walking away vanished at 80 m. Found 2026-11-08 with Legends Lua (hook + per-frame diff of the slots). r is
// multiplied like the trees; the slots set so far are kept so a change in the Settings applies at once.
#define NSLOT 256
static struct { uint64_t slot; int32_t game, put; } g_slot[NSLOT];
static int g_nslot;

static void put_slot(int s) {
    int32_t r = (int32_t)(g_slot[s].game * factor() + 0.5f);
    int32_t *f = (int32_t *)(uintptr_t)(g_slot[s].slot + 0x450);
    f[0] = r, f[1] = r - 10, f[2] = r + 20;
    g_slot[s].put = r;
}

static int npc_radius_hook(Cpu *c, uint64_t addr) {
    (void)addr;
    uint64_t slot = c->x[0];
    int32_t r = (int32_t)(uint32_t)c->x[1];
    int s = 0;
    while (s < g_nslot && g_slot[s].slot != slot) s++;
    if (s == g_nslot && g_nslot < NSLOT) g_nslot++;
    if (s == NSLOT) s = NSLOT - 1;  // (full: the last one is reused)
    g_slot[s].slot = slot, g_slot[s].game = r;
    put_slot(s);  // (the game's function only stores these three)
    if (g_log) LOG("[drawdist] NPC slot %llx: load radius %d -> %d m\n", (unsigned long long)slot, r, g_slot[s].put);
    return 1;
}

// Far Pokemon animating at 1/2 .. 1/8 rate (app_config field_anime_framerate_config): the field's animation-rate
// manager, built by f_70018d24f8, keeps how many Pokemon each rate level takes - +0xc0 5 at the full rate, +0xc8 3 at
// 1/2 (+0xcc), +0xd0 8 at 1/4 (+0xd4), the rest at 1/8 (+0xdc) - and each field Pokemon's part (f_70018d0b54) the
// squared distanceBaseValue (60 m) at +0x1f4. Both are multiplied with the draw distance (the counts by the factor,
// the distance squared by its square) when they are built: a change applies from the next area load.
static int anim_mgr_hook(Cpu *c, uint64_t addr) {
    uint64_t o = c->x[0];
    rt_call_original(c, addr);
    int32_t *n = (int32_t *)(uintptr_t)(o + 0xc0);
    if (n[0] != 5 || n[2] != 3 || n[4] != 8) return 1;  // (not the layout this was found in: left alone)
    float k = factor();
    for (int i = 0; i < 5; i += 2) n[i] = (int32_t)(n[i] * k + 0.5f);
    if (g_log) LOG("[drawdist] animation rates: %d Pokemon at the full rate, %d at 1/2, %d at 1/4\n", n[0], n[2], n[4]);
    return 1;
}
static int anim_poke_hook(Cpu *c, uint64_t addr) {
    uint64_t o = c->x[0];
    rt_call_original(c, addr);
    float *d2 = (float *)(uintptr_t)(o + 0x1f4), k = factor();
    if (*d2 > 1.0f && *d2 < 1e6f) *d2 *= k * k;
    static int n;
    if (g_log && n < 4) n++, LOG("[drawdist] Pokemon %llx: full-rate animation distance %.0f m\n", (unsigned long long)o, sqrtf(*d2));
    return 1;
}

// Grass and small plants (2026-11-08, Legends Lua + page watches): the field builds them in 16 m cells, an N x N block
// around the player (f_7002b1d9d8(manager, position): cell = floor((pos - N*size/2) / size), slots wrap with & (N-1)),
// 11 layers per cell, each cell's instances uploaded to the GPU when it enters the block. The manager is built by
// f_7002b1d074(manager, N) (+0xf8 N, +0xfc 16 m), which allocates N*N of everything: N = 4 = grass to ~32 m.
// LARECOMP_PLA_GRASS_N=<n> (power of two): research override.
static int grass_hook(Cpu *c, uint64_t addr) {
    static int n = -1;
    if (n < 0) n = getenv("LARECOMP_PLA_GRASS_N") ? atoi(getenv("LARECOMP_PLA_GRASS_N")) : 0;
    uint32_t game = (uint32_t)c->x[1];
    if (n > 0 && (n & (n - 1)) == 0) c->x[1] = (uint32_t)n;
    LOG("[drawdist] grass block %u x %u cells (the game's %u)\n", (unsigned)c->x[1], (unsigned)c->x[1], game);
    (void)addr;
    return 0;
}

// Settings changed (the Graphics row, hle/pla_options.c): the remembered objects that still hold what was put there
void pla_set_detail(void) {
    for (int s = 0; s < NOBJ; s++) {
        if (!g_obj[s].obj) continue;
        bool same = true;
        for (int i = 0; i < 5; i++) same = same && *at(g_obj[s].obj, i) == g_obj[s].put[i];
        if (same) put(s);
        else g_obj[s].obj = 0;
    }
    for (int s = 0; s < g_nslot; s++)
        if (g_slot[s].slot && *(const int32_t *)(uintptr_t)(g_slot[s].slot + 0x450) == g_slot[s].put) put_slot(s);
        else g_slot[s].slot = 0;
}

void pla_drawdist_configure(void) {
    g_log = getenv("LARECOMP_PLA_DRAWDIST_LOG") && atoi(getenv("LARECOMP_PLA_DRAWDIST_LOG")) > 0;
    rt_register_fnhook_at(0x700246746cull, fade_hook);
    rt_register_fnhook_at(0x700246f818ull, fade_hook);
    rt_register_fnhook_at(0x700173d4b0ull, npc_radius_hook);
    rt_register_fnhook_at(0x70018d24f8ull, anim_mgr_hook);
    rt_register_fnhook_at(0x70018d0b54ull, anim_poke_hook);
    rt_register_fnhook_at(0x7002b1d074ull, grass_hook);
}
