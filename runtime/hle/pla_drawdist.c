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
    return 1;
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
}

void pla_drawdist_configure(void) {
    g_log = getenv("LARECOMP_PLA_DRAWDIST_LOG") && atoi(getenv("LARECOMP_PLA_DRAWDIST_LOG")) > 0;
    rt_register_fnhook_at(0x700246746cull, fade_hook);
    rt_register_fnhook_at(0x700246f818ull, fade_hook);
}
