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
static void put_lodtab(void);

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
    put_lodtab();  // (the field is loading: the game has its LOD table in place by now)
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
// With the Draw Distance setting: Switch 4 (the game's), High 8, Very High / Max 16 (16 x 16 needed the 800 MB field
// GPU pool, hle/pla_cmdpool.c - it crashed in the game's 300). Applies when an area loads.
static const int k_grass_n[4] = {4, 8, 16, 16};
static int grass_hook(Cpu *c, uint64_t addr) {
    static int n = -1;
    if (n < 0) n = getenv("LARECOMP_PLA_GRASS_N") ? atoi(getenv("LARECOMP_PLA_GRASS_N")) : 0;
    uint32_t game = (uint32_t)c->x[1];
    int want = n > 0 ? n : (game == 4 && g_settings.detail >= 0 && g_settings.detail <= 3 ? k_grass_n[g_settings.detail] : 0);
    if (want > 0 && (want & (want - 1)) == 0) c->x[1] = (uint32_t)want;
    LOG("[drawdist] grass block %u x %u cells (the game's %u)\n", (unsigned)c->x[1], (unsigned)c->x[1], game);
    (void)addr;
    return 0;
}

// The grass's own reach: each area's detail definition (in bin/archive/field/terrain/<area>.gfpak, next to the
// area<n>_0_0_detailmap_*.trtbf maps) has a record per grass / plant layer: {16 (cell m), 3, 1, 2.25, 2.5, 2.75, 0.9,
// 1.1, 10.5} - the three are cells (36 / 40 / 44 m): how far a plant keeps its full model (flowers lose their stems
// past it - the user's focus-camera find; x1.5 verified on the beach walk: stems further out, near density unchanged).
// The archive is Oodle-compressed: OodleLZ_Decompress (0x7000004fc0: x0 in, x1 its size, x2 out, x3 the raw size) is
// hooked and the records scaled in what it unpacks, before the game reads them. With the setting: x1 / 1.25 / 2 / 2.75
// (kept inside the grass block: 2.75 cells x k <= half of it); applies when an area loads. LARECOMP_PLA_GRASS_FADE=<k>.
static const float k_grass_fade[4] = {1.0f, 1.25f, 2.0f, 2.75f};
static float g_grass_fade = -1;
static int oodle_hook(Cpu *c, uint64_t addr) {
    uint8_t *out = (uint8_t *)(uintptr_t)c->x[2];
    uint64_t raw = c->x[3];
    rt_call_original(c, addr);
    if ((int64_t)c->x[0] <= 0 || raw < 64 || raw > (256u << 20)) return 1;
    static int find = -1;  // (research) LARECOMP_PLA_FIND_CONST=1: log the unpacked blocks holding (-36, 36, 0.25, 2)
    if (find < 0) find = getenv("LARECOMP_PLA_FIND_CONST") != NULL;
    if (find) {
        static const float pat[4] = {-36.0f, 36.0f, 0.25f, 2.0f};
        static const float pat2[2] = {-36.0f, 36.0f};
        for (uint64_t o = 0; o + 16 <= raw; o += 4)
            if (!memcmp(out + o, pat, 16) || !memcmp(out + o, pat2, 8)) {
                char nm[80] = {0};
                for (uint64_t k = 0; k + 4 < raw && k < 0x4000; k++)  // (a name nearby: the first ".tr" string in the block)
                    if (out[k] == '.' && out[k + 1] == 't' && out[k + 2] == 'r') {
                        uint64_t s = k;
                        while (s > 0 && out[s - 1] >= 0x20 && out[s - 1] < 0x7f && k - s < 60) s--;
                        snprintf(nm, sizeof nm, "%.*s", (int)(k + 6 - s), (const char *)out + s);
                        break;
                    }
                LOG("[drawdist] const %s at +%llx of a %llu-byte block %llx (%s)\n", !memcmp(out + o, pat, 16) ? "(-36,36,0.25,2)" : "(-36,36)",
                    (unsigned long long)o, (unsigned long long)raw, (unsigned long long)(uintptr_t)out, nm);
            }
    }
    float fade = g_grass_fade > 0 ? g_grass_fade : (g_settings.detail >= 0 && g_settings.detail <= 3 ? k_grass_fade[g_settings.detail] : 1.0f);
    if (fade <= 1.0f) return 1;
    static const float key[3] = {16.0f, 3.0f, 1.0f};
    int n = 0;
    for (uint64_t o = 0; o + 24 <= raw; o += 4) {
        if (memcmp(out + o, key, 12)) continue;
        float *f = (float *)(out + o + 12);
        if (!(f[0] > 1.5f && f[0] < 4.0f && f[1] >= f[0] && f[2] >= f[1] && f[2] < 5.0f)) continue;
        f[0] *= fade, f[1] *= fade, f[2] *= fade;
        n++;
    }
    if (n) LOG("[drawdist] plant detail distance x%g in %d detail layer records\n", fade, n);
    return 1;
}

// (research) LARECOMP_PLA_LODFN=1: model LOD - f_700299d91c (per object, x0) asks [[x0+0x288]+0x60](x0 = that+0x80,
// x1 = {.., current LOD}) for the LOD and stores it at [x0+0x1f0]+0x174: log each distinct selector function once
static int lodfn_hook(Cpu *c, uint64_t addr) {
    (void)addr;
    uint64_t o = c->x[0];
    if (o < 0x7200000000ull || o >= 0x7400000000ull) return 0;
    uint64_t a = *(const uint64_t *)(uintptr_t)(o + 0x288);
    if (a < 0x7200000000ull || a >= 0x7400000000ull) return 0;
    uint64_t fn = *(const uint64_t *)(uintptr_t)(a + 0x60);
    static uint64_t seen[32];
    static int n;
    for (int i = 0; i < n; i++) if (seen[i] == fn) return 0;
    if (n < 32) {
        seen[n++] = fn;
        LOG("[lodfn] selector %llx (object %llx, lod object %llx: +e8 %d)\n", (unsigned long long)fn, (unsigned long long)o,
            (unsigned long long)a, *(const int32_t *)(uintptr_t)(a + 0xe8));
    }
    return 0;
}

// Model LOD (rocks and props swapping to their _lod1/_lod2 meshes - the user's rock): the selector f_7001acd904 works out
// how much of the screen an object's bounding sphere covers (x 0.25) and takes the first level whose cutoff in the static
// table at 0x700397caf0 (3 floats per profile, the profile at object+0x234: {0.01, 0.003, 0.001}, {0.01, 0.005,
// 0.002}, {1.8, 0.5, 0.2}) is above it; the chosen level lands in [object+0x1f0]+0x174 (f_700299d91c). Coverage falls
// with the distance squared: the cutoffs / k^2 keep each level k times further out. The table is read every frame, so
// a Settings change applies at once. Found 2026-11-08 from the user's save at the rock (frame-by-frame swap, mesh
// flags diff, then the writer of the current level).
#define LODTAB 0x700397caf0ull
static const float k_lodtab[9] = {0.01f, 0.003f, 0.001f, 0.01f, 0.005f, 0.002f, 1.8f, 0.5f, 0.2f};
static void put_lodtab(void) {
    float *t = (float *)(uintptr_t)LODTAB;
    for (int i = 0; i < 9; i++)  // (not the table this was found in - or not filled in yet: left alone)
        if (!(t[i] > 1e-7f && t[i] < 2.0f)) { if (g_log) LOG("[drawdist] model LOD table not there (%g at %d)\n", t[i], i); return; }
    float k = factor();
    for (int i = 0; i < 9; i++) t[i] = k_lodtab[i] / (k * k);
    if (g_log) LOG("[drawdist] model LOD cutoffs / %g (detail x%g further)\n", k * k, k);
}

// Settings changed (the Graphics row, hle/pla_options.c): the remembered objects that still hold what was put there
void pla_set_detail(void) {
    put_lodtab();
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
    put_lodtab();
    rt_register_fnhook_at(0x700246746cull, fade_hook);
    rt_register_fnhook_at(0x700246f818ull, fade_hook);
    rt_register_fnhook_at(0x700173d4b0ull, npc_radius_hook);
    rt_register_fnhook_at(0x70018d24f8ull, anim_mgr_hook);
    rt_register_fnhook_at(0x70018d0b54ull, anim_poke_hook);
    rt_register_fnhook_at(0x7002b1d074ull, grass_hook);
    g_grass_fade = getenv("LARECOMP_PLA_GRASS_FADE") ? (float)atof(getenv("LARECOMP_PLA_GRASS_FADE")) : 0;
    rt_register_fnhook_at(0x7000004fc0ull, oodle_hook);  // (the setting decides per area load)
    if (getenv("LARECOMP_PLA_LODFN")) rt_register_fnhook_at(0x700299d91cull, lodfn_hook);
}
