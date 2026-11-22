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
#define LODTAB 0x700397caf0ull

static float factor(void) { return g_settings.detail >= 0 && g_settings.detail <= 3 ? k_mul[g_settings.detail] : 1.0f; }
static float *at(uint64_t obj, int i) { return (float *)(uintptr_t)(obj + k_off[i]); }

static void put(int s) {
    float k = factor();
    float r = 1.0f;  // (the view distance's raise: the fades move with it, or far trees stand there without leaves)
    for (int i = 0; i < 5; i++) {
        float base = g_obj[s].game[i] * (i == 1 || i == 2 ? r : 1.0f);
        // Max: the tree view distance from 500 m (the game's kTreeVisibleDistance is 200), as the Nexus "Max Draw
        // Distance" mod sets it - x5 = 2500 m: the far hills keep their trees (player report, 2026-11-11)
        static int tbase = -1;  // (LARECOMP_PLA_TREE_BASE=0: the game's 200 m, research A/B)
        if (tbase < 0) tbase = !(getenv("LARECOMP_PLA_TREE_BASE") && atoi(getenv("LARECOMP_PLA_TREE_BASE")) == 0);
        if (i == 0 && g_settings.detail == 3 && tbase && base > 1.0f && base < 500.0f) r = 500.0f / base, base = 500.0f;
        *at(g_obj[s].obj, i) = g_obj[s].put[i] = base * k;
    }
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
    static float rk = -1;  // (LARECOMP_PLA_NPC_RADIUS=<k>: research - the NPC slots' factor alone)
    if (rk < 0) rk = getenv("LARECOMP_PLA_NPC_RADIUS") ? (float)atof(getenv("LARECOMP_PLA_NPC_RADIUS")) : 0;
    int32_t r = (int32_t)(g_slot[s].game * (rk > 0 ? rk : factor()) + 0.5f);
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

// (research) LARECOMP_PLA_SLOTWATCH=1: each frame, the words +0x300..+0x460 of every NPC slot that changed (not +0x330,
// the distance) - what flips when an NPC is hidden (hle/warp.c calls this each game frame)
void pla_slot_watch(uint64_t frame) {
    // The model LOD table is applied when the field's trees are set up (fade_hook), but in Jubilife it is still empty then
    // ("model LOD table not there") and the game fills it in later: village models - its people too - kept the game's
    // cut-offs at every setting (player report: far NPCs only appear walking closer, same at Switch and Max). Once a
    // second: a table back at the game's values while the setting wants it further gets it now.
    if (frame % 60 == 0 && factor() > 1.0f) {
        const float *t = (const float *)(uintptr_t)LODTAB;
        if (t[6] > 1.79f && t[6] < 1.81f) put_lodtab();  // (1.8: the game's own, not scaled yet)
    }
    static int on = -1;
    if (on < 0) on = getenv("LARECOMP_PLA_SLOTWATCH") != NULL;
    if (!on) return;
    static int armed;  // LARECOMP_PLA_SLOTWATCH=<frame> (> 1): page-watch +0x354..+0x35c of 6 slots from that frame
    if (!armed && atoi(getenv("LARECOMP_PLA_SLOTWATCH")) > 1 && frame >= (uint64_t)atoi(getenv("LARECOMP_PLA_SLOTWATCH"))) {
        void watch_add_page(uint64_t addr, uint64_t len);
        armed = 1;
        for (int s = 0, k = 0; s < g_nslot && k < 6; s++)
            if (g_slot[s].slot) watch_add_page(g_slot[s].slot + 0x354, 8), k++;
    }
    static uint32_t prev[NSLOT][0x58];
    static uint64_t prev_slot[NSLOT];
    for (int s = 0; s < g_nslot; s++) {
        if (!g_slot[s].slot) continue;
        const uint32_t *w = (const uint32_t *)(uintptr_t)(g_slot[s].slot + 0x300);
        bool fresh = prev_slot[s] != g_slot[s].slot;
        for (int i = 0; i < 0x58; i++) {
            if (!fresh && w[i] != prev[s][i] && i != (0x330 - 0x300) / 4)
                LOG("[slotw] frame %llu slot %llx +%03x: %08x -> %08x (dist %.1f)\n", (unsigned long long)frame,
                    (unsigned long long)g_slot[s].slot, 0x300 + i * 4, prev[s][i], w[i], sqrtf(*(const float *)(uintptr_t)(g_slot[s].slot + 0x330)));
            prev[s][i] = w[i];
        }
        prev_slot[s] = g_slot[s].slot;
    }
}

// (research, with LARECOMP_PLA_SLOTWATCH=1) f_7001738c14(slot, state, x2) sets a slot's state (+0x354/+0x358, 0x80
// flips while the camera pans): each distinct caller + state with its call stack
static int slotstate_hook(Cpu *c, uint64_t addr) {
    (void)addr;
    static struct { uint64_t lr; uint32_t st; } seen[64];
    static int n;
    uint64_t lr = c->x[30];
    uint32_t st = (uint32_t)c->x[1];
    for (int i = 0; i < n; i++) if (seen[i].lr == lr && seen[i].st == st) return 0;
    if (n < 64) seen[n].lr = lr, seen[n].st = st, n++;
    char line[256];
    int o = 0;
    uint64_t fp = c->x[29];
    for (int i = 0; i < 8 && fp >= 0x7000000000ull && fp < 0x8000000000ull && !(fp & 7) && o < (int)sizeof line - 24; i++) {
        uint64_t l = ((uint64_t *)(uintptr_t)fp)[1];
        fp = ((uint64_t *)(uintptr_t)fp)[0];
        if (!l) break;
        o += snprintf(line + o, sizeof line - o, " %llx", (unsigned long long)l);
    }
    line[o] = 0;
    LOG("[slotstate] state %#x x2 %#llx from lr %llx:%s\n", st, (unsigned long long)c->x[2], (unsigned long long)lr, line);
    return 0;
}

// NPCs popping out and in while the camera turns (the user's village shots, 2026-11-08): the two per-frame NPC
// updates, f_700173740c(slot, frustum) (people, states 2..7) and f_7001736bfc(slot, frustum) (field objects, states 9 /
// 10), test the NPC's bounding sphere against the camera and, when they decide it isn't seen, set slot state 0x80
// (hidden: not animated, not drawn) through f_70014916d0(slot, 1, 0x80, 4) (lr 70017377c4 / 7001736d38; x3 = fade
// frames, 0 = at once). 09b425b refused both hide requests: every NPC inside its load radius stayed on - animated on the
// game's threads and drawn into the shadow passes even off screen. That was the cost of Very High in Jubilife (x3 radius
// = the whole village, ~130 slots, ~90 hide requests a frame; player report 2026-11-17, RX 6600 XT / 12 threads: 72 fps
// not held): village walk + pan 165 M instructions and ~1870 draws a frame vs ~103 M / ~1300 with the game's hides.
// The game's hides aren't usable as they are: people well inside the view (3-50 m inside the planes) get hide requests
// too, and the people's sphere (~1 m) is smaller than the body, so one at the screen edge went while part of it was on
// screen and faded back in after its centre got in (LARECOMP_PLA_NPC_POPS: ~2700 in-view pop-ins and ~600 vanishes per
// 100 s of the village pan; the dithered fade-in at the edge is in the screenshots).
// Now each hide request is decided here, on the camera the update tests against (its x1: +0x00 the camera position,
// +0x20..+0x70 six planes n.p + w > 0 inside - 4 sides, near, far): hidden only when the sphere + 1 m is outside a side or
// the near plane by more than a margin - each plane turned out by 4 deg + 2 frames of the camera's current turn (stick
// turns 1 - 3 deg a frame at ~40-72 fps, peaks ~10) - and over 6 m from the camera. Any other request becomes a clear
// with no fade, so one hidden earlier is back, whole, before it reaches the screen edge. Out-of-range slots: below.
// Result (Jubilife walk + pan, Very High, 72 fps cap): 0 in-view pop-ins / vanishes (09b425b 0, the game's ~3300);
// ~120 M instructions and ~1500 draws a frame.
// LARECOMP_PLA_NPC_KEEP=0: the game's hides; =1: 09b425b's (none). Research: LARECOMP_PLA_NPC_MARGIN=<deg> (a fixed
// margin), _BASE / _LEAD / _NEAR / _PAD (the numbers above), LARECOMP_PLA_NPC_STATS=1 (counts; 2: each slot shown
// against the game's request, once), LARECOMP_PLA_NPC_POPS=1 (below).
uint64_t renderer_game_frame(void);
static int g_npc_keep = -1;
static float g_npc_near = 6.0f, g_npc_pad = 1.0f;
static int g_npc_stats;
static volatile long g_npc_n[4];  // (stats: hidden as the game asked, shown instead, no sphere / camera (shown), clears)

static bool npc_ptr_ok(uint64_t p) { return p >= 0x7000000000ull && p < 0x8000000000ull && !(p & 7); }

// the bounding sphere the updates test (centre, radius)
static bool npc_sphere(uint64_t slot, float out[4]) {
    uint64_t o = *(const uint64_t *)(uintptr_t)(slot + 0x268);
    if (!npc_ptr_ok(o)) return false;
    uint64_t b = *(const uint64_t *)(uintptr_t)(o + 0x18);
    if (!npc_ptr_ok(b)) return false;
    uint64_t k = *(const uint64_t *)(uintptr_t)(b + 0x1a0);
    if (!npc_ptr_ok(k)) return false;
    uint64_t m = *(const uint64_t *)(uintptr_t)(k + 0x18);
    if (!npc_ptr_ok(m)) return false;
    memcpy(out, (const void *)(uintptr_t)(m + 0x240), 16);
    return out[3] >= 0.0f && out[3] < 1000.0f && isfinite(out[0] + out[1] + out[2]);
}

// 1: the sphere is more than sin_m x distance outside one of the side / near planes and further than near metres from
// the camera; 0: not; -1: no sphere or camera
static volatile long g_npc_why[4];  // (stats: no sphere, camera not finite, a plane not unit)
static int npc_outside(uint64_t slot, uint64_t fr, float sin_m, float near, float pad) {
    float sp[4];
    if (!npc_sphere(slot, sp)) { g_npc_why[0]++; return -1; }
    const float *cam = (const float *)(uintptr_t)fr, *pl = (const float *)(uintptr_t)(fr + 0x20);
    if (!isfinite(cam[0] + cam[1] + cam[2])) { g_npc_why[1]++; return -1; }
    float r = sp[3] + pad, dx = sp[0] - cam[0], dy = sp[1] - cam[1], dz = sp[2] - cam[2];
    float d = sqrtf(dx * dx + dy * dy + dz * dz);
    if (d - r < near) return 0;
    float slack = d * sin_m;
    for (int i = 0; i < 5; i++) {  // (the 4 sides and the near plane, each turned out by the margin)
        const float *n = pl + i * 4;
        float l = n[0] * n[0] + n[1] * n[1] + n[2] * n[2];
        if (!(l > 0.9f && l < 1.1f)) { g_npc_why[2]++; return -1; }  // (not the layout this was found in)
        if (n[0] * sp[0] + n[1] * sp[1] + n[2] * sp[2] + n[3] + r < -slack) return 1;
    }
    return 0;
}

// (research) LARECOMP_PLA_NPC_POPS=1: each NPC whose applied state (+0x354) turns hidden -> shown ("pop in") or shown ->
// hidden ("vanish") while its sphere is inside the camera's real frustum (no margin): "[npcpop]" lines and counts - the
// pop-in A/B without pictures (occluded ones count too). Frames from the first clear asked to the applied change.
static struct { uint64_t slot; uint32_t hidden; uint64_t asked, last; } g_pop[1024];
static volatile long g_pop_n[2];
static void npc_pop_check(uint64_t slot, uint64_t fr, bool set_asked) {
    uint64_t h = (slot >> 4) * 0x9e3779b97f4a7c15ull >> 54;
    int i = (int)h, tries = 0;
    while (g_pop[i].slot && g_pop[i].slot != slot && tries++ < 1024) i = (i + 1) & 1023;
    if (tries >= 1024) return;
    uint32_t hid = *(const uint32_t *)(uintptr_t)(slot + 0x354) & 0x80;
    uint64_t now = renderer_game_frame();
    if (g_pop[i].slot != slot) { g_pop[i].slot = slot, g_pop[i].hidden = hid, g_pop[i].asked = 0, g_pop[i].last = now; return; }
    uint64_t gap = now - g_pop[i].last;
    g_pop[i].last = now;
    if (hid && !set_asked && !g_pop[i].asked) g_pop[i].asked = now;
    if (hid != g_pop[i].hidden) {
        int in = npc_outside(slot, fr, 0.0f, 0.0f, 1.0f) == 0;  // (+1 m: a person's sphere is ~0.6 m, the body taller)
        float sp[4] = {0}, d = 0, u = -1, v = -1;
        if (npc_sphere(slot, sp)) {
            const float *cam = (const float *)(uintptr_t)fr, *pl = (const float *)(uintptr_t)(fr + 0x20);
            d = sqrtf((sp[0] - cam[0]) * (sp[0] - cam[0]) + (sp[1] - cam[1]) * (sp[1] - cam[1]) + (sp[2] - cam[2]) * (sp[2] - cam[2]));
            float e[4];  // (where on screen: the side planes' distances, 0..1 across - +0x20/+0x30 and +0x40/+0x50 pairs)
            for (int j = 0; j < 4; j++) e[j] = pl[j * 4] * sp[0] + pl[j * 4 + 1] * sp[1] + pl[j * 4 + 2] * sp[2] + pl[j * 4 + 3];
            if (e[0] + e[1] != 0) u = e[0] / (e[0] + e[1]);
            if (e[2] + e[3] != 0) v = e[2] / (e[2] + e[3]);
        }
        if (in) {
            long t = __atomic_add_fetch(&g_pop_n[hid ? 1 : 0], 1, __ATOMIC_RELAXED);
            LOG("[npcpop] frame %llu slot %llx %s in view, %.1f m r %.1f at %.2f %.2f st %u gap %llu (asked %lld frames before); pop-ins %ld vanishes %ld\n",
                (unsigned long long)now, (unsigned long long)slot, hid ? "VANISH" : "POP-IN", d, sp[3], u, v,
                *(const uint32_t *)(uintptr_t)(slot + 0x220), (unsigned long long)gap,
                hid || !g_pop[i].asked ? -1ll : (long long)(now - g_pop[i].asked), hid ? g_pop_n[0] : t, hid ? t : g_pop_n[1]);
        }
        g_pop[i].hidden = hid, g_pop[i].asked = 0;
    }
}

// the camera of the NPC update running on this thread (its x1; a copy on that thread's stack, valid only inside it)
static __thread uint64_t t_npc_fr;
static int npc_update_hook(Cpu *c, uint64_t addr) {
    uint64_t was = t_npc_fr;
    t_npc_fr = c->x[1];
    rt_call_original(c, addr);
    t_npc_fr = was;
    return 1;
}

// How fast the camera turns, for the margin: each new camera the updates see (its forward, the near plane's normal at
// +0x60, changed) gives the angle from the last one; the peak decays by 0.85 a frame. The margin is g_npc_base degrees +
// g_npc_lead frames of that turn (an NPC a turn brings into view within that many frames is shown already). A cut (over
// 90 deg) isn't a turn: the update after it tests against the new camera and shows what is in view at once.
static float g_npc_base = 4.0f, g_npc_lead = 2.0f, g_npc_fixed = -1;
static float g_npc_fwd[3], g_npc_peak;
static uint64_t g_npc_peak_frame;
static float npc_margin_sin(uint64_t fr) {
    if (g_npc_fixed >= 0) return g_npc_fixed;
    const float *n = (const float *)(uintptr_t)(fr + 0x60);
    uint64_t f = renderer_game_frame();
    float dot = n[0] * g_npc_fwd[0] + n[1] * g_npc_fwd[1] + n[2] * g_npc_fwd[2];
    if (dot < 0.9999999f) {  // (a new camera: this frame's)
        float deg = acosf(fmaxf(-1.0f, fminf(1.0f, dot))) * 57.29578f;
        if (deg > 90.0f) deg = 0.0f;  // (a cut, or the first: no turn to follow)
        float now = g_npc_peak * powf(0.85f, (float)(f - g_npc_peak_frame));
        if (deg >= now) g_npc_peak = deg, g_npc_peak_frame = f;
        memcpy(g_npc_fwd, n, 12);
    }
    float turn = g_npc_peak * powf(0.85f, (float)(f - g_npc_peak_frame));
    return sinf(fminf(g_npc_base + g_npc_lead * turn, 60.0f) * 0.01745329f);
}

// The field's NPC loop (around 0x7001d84e08) updates a slot only near the player: f_7001736bfc within +0x3c0 + r and
// f_700173740c within +0x3c0 + r' by the horizontal distance it keeps squared at +0x330 (slot position +0x340, the
// player's at its context +0x110). r, r' = the slot's +0x450 / +0x454 (what npc_radius_hook sets: r, r - 10) or, for a
// slot without (0: the field objects of state 9 / 10), its context's +0x68 / +0x70 (100 / 60 m). Only f_700173740c
// (people, states 2..7) and f_7001736bfc for states 9 / 10 ask for 0x80. Past its range a slot isn't tested at all and
// keeps its state: one hidden there stayed hidden while the camera turned to it and came back only when the player
// walked in again (far NPCs ~105 m away popping in at Very High, 2026-11-17). Those are looked after here instead: the
// loop reads every slot's +0x3c0 (f_700173d12c from lr 7001d84e50) each frame, in range or not; for one out of range
// the same test decides and the game's own setter is called (f_70014916d0(slot, 1 / 0, 0x80, 0): at once), on that
// thread, at the point the update would have run.
static volatile uint64_t g_npc_ctx;
static int npc_ctx_hook(Cpu *c, uint64_t addr) {
    (void)addr;
    if (c->x[30] == 0x7001d84da8ull) g_npc_ctx = *(const uint64_t *)(uintptr_t)c->sp;
    return 0;
}
// true: the slot's hide test runs this frame (it is inside the range of the update that asks for 0x80)
static bool npc_in_range(uint64_t slot) {
    const int32_t *r = (const int32_t *)(uintptr_t)(slot + 0x450);
    uint32_t st = *(const uint32_t *)(uintptr_t)(slot + 0x220);
    bool objs = st == 9 || st == 10;
    float r0 = *(const float *)(uintptr_t)(slot + 0x3c0), d2 = *(const float *)(uintptr_t)(slot + 0x330), range;
    if (!(r0 >= 0.0f && r0 < 1000.0f)) return true;  // (not the layout this was found in: left to the game)
    if (r[0] > 0) range = (float)(objs ? r[0] : r[1]);
    else {
        uint64_t ctx = g_npc_ctx;
        if (!npc_ptr_ok(ctx)) return true;
        range = (float)(int32_t)*(const float *)(uintptr_t)(ctx + (objs ? 0x68 : 0x70));
    }
    float edge = r0 + range;
    return edge > 0.0f && d2 <= edge * edge;
}
static float npc_margin_sin(uint64_t fr);
static void npc_pop_check(uint64_t slot, uint64_t fr, bool set_asked);
static int g_npc_pops;
static volatile long g_npc_far[2];  // (stats: out-of-range slots hidden / shown by npc_far_hook)
static int npc_far_hook(Cpu *c, uint64_t addr) {
    (void)addr;
    if (c->x[30] != 0x7001d84e50ull) return 0;
    uint64_t slot = c->x[0], hold = *(const uint64_t *)(uintptr_t)(c->sp + 0x30);
    if (!npc_ptr_ok(slot) || !npc_ptr_ok(hold)) return 0;
    uint64_t fr = *(const uint64_t *)(uintptr_t)(hold + 8);
    uint32_t st = *(const uint32_t *)(uintptr_t)(slot + 0x220);
    if (!npc_ptr_ok(fr) || !((st >= 2 && st <= 7) || st == 9 || st == 10) || npc_in_range(slot)) return 0;
    if (*(const uint8_t *)(uintptr_t)(fr + 0x141)) return 0;  // (the loop's own "no frustum test" flag)
    if (g_npc_pops) npc_pop_check(slot, fr, false);
    int o = npc_outside(slot, fr, npc_margin_sin(fr), g_npc_near, g_npc_pad);
    if (o < 0) return 0;
    bool hidden = (*(const uint32_t *)(uintptr_t)(slot + 0x358) & 0x80) != 0;
    if (hidden == (o == 1)) return 0;
    Cpu saved = *c;
    c->x[0] = slot, c->x[1] = o == 1, c->x[2] = 0x80, c->x[3] = 0;  // (0: at once, no fade)
    guest_call(c, 0x70014916d0ull);
    *c = saved;
    if (g_npc_stats) __atomic_add_fetch(&g_npc_far[o == 1 ? 0 : 1], 1, __ATOMIC_RELAXED);
    return 0;
}

static int npc_hide_hook(Cpu *c, uint64_t addr) {
    (void)addr;
    if ((uint32_t)c->x[2] != 0x80) return 0;
    static int research = -1;  // (LARECOMP_PLA_SLOTWATCH=1: each distinct caller setting / clearing 0x80, logged once)
    if (research < 0) research = getenv("LARECOMP_PLA_SLOTWATCH") != NULL;
    if (research) {
        static uint64_t seen[64];
        static int n;
        uint64_t key = c->x[30] ^ ((c->x[1] & 1) << 63);
        bool dup = false;
        for (int i = 0; i < n && !dup; i++) dup = seen[i] == key;
        if (!dup && n < 64) {
            seen[n++] = key;
            LOG("[slotbit] %s 0x80 from lr %llx slot %llx\n", c->x[1] & 1 ? "SET" : "clear", (unsigned long long)c->x[30],
                (unsigned long long)c->x[0]);
        }
    }
    uint64_t lr = c->x[30];
    int who = lr == 0x70017377c4ull ? 1 : lr == 0x7001736d38ull ? 2 : 0;
    uint64_t fr = t_npc_fr;
    if (g_npc_pops) {
        if (npc_ptr_ok(fr) && npc_ptr_ok(c->x[0])) npc_pop_check(c->x[0], fr, c->x[1] & 1);
        if (c->x[1] & 1 && !who) {  // (other callers hiding: each logged once)
            static uint64_t seen[32];
            static int n;
            bool dup = false;
            for (int i = 0; i < n && !dup; i++) dup = seen[i] == lr;
            if (!dup && n < 32) seen[n++] = lr, LOG("[npcpop] SET 0x80 from lr %llx (in an update: %d)\n", (unsigned long long)lr, fr != 0);
        }
    }
    if (!g_npc_keep || !who) return 0;
    if (!(c->x[1] & 1)) {
        if (g_npc_stats) __atomic_add_fetch(&g_npc_n[3], 1, __ATOMIC_RELAXED);
        return 0;
    }
    if (g_npc_keep == 1) return 1;  // (09b425b: refused)
    int k;
    if (!npc_ptr_ok(fr) || !npc_ptr_ok(c->x[0])) k = 2;
    else {
        int o = npc_outside(c->x[0], fr, npc_margin_sin(fr), g_npc_near, g_npc_pad);
        k = o == 1 ? 0 : o == 0 ? 1 : 2;
    }
    if (g_npc_stats) {
        long t = __atomic_add_fetch(&g_npc_n[k], 1, __ATOMIC_RELAXED);
        if (k == 0 && t % 20000 == 0)
            LOG("[npckeep] hidden %ld, shown instead %ld, no sphere / camera %ld (sphere %ld, camera %ld, planes %ld), clears %ld; "
                "out of range: hidden %ld, shown %ld\n", g_npc_n[0], g_npc_n[1], g_npc_n[2], g_npc_why[0], g_npc_why[1], g_npc_why[2],
                g_npc_n[3], g_npc_far[0], g_npc_far[1]);
    }
    if (g_npc_stats > 1 && k == 1) {  // (LARECOMP_PLA_NPC_STATS=2: each slot shown instead, once, with its numbers)
        static uint64_t seen[64];
        static int n;
        bool dup = false;
        for (int i = 0; i < n && !dup; i++) dup = seen[i] == c->x[0];
        float sp[4];
        if (!dup && n < 64 && npc_sphere(c->x[0], sp)) {
            seen[n++] = c->x[0];
            const float *cam = (const float *)(uintptr_t)fr, *pl = (const float *)(uintptr_t)(fr + 0x20);
            float v[5];
            for (int i = 0; i < 5; i++) v[i] = pl[i * 4] * sp[0] + pl[i * 4 + 1] * sp[1] + pl[i * 4 + 2] * sp[2] + pl[i * 4 + 3] + sp[3];
            LOG("[npckeep] shown instead: slot %llx who %d st %u r %.1f cam dist %.1f planes %.1f %.1f %.1f %.1f %.1f 45c %u\n",
                (unsigned long long)c->x[0], who, *(const uint32_t *)(uintptr_t)(c->x[0] + 0x220), sp[3],
                sqrtf((sp[0] - cam[0]) * (sp[0] - cam[0]) + (sp[1] - cam[1]) * (sp[1] - cam[1]) + (sp[2] - cam[2]) * (sp[2] - cam[2])),
                v[0], v[1], v[2], v[3], v[4], *(const uint8_t *)(uintptr_t)(c->x[0] + 0x45c));
        }
    }
    if (k) c->x[1] &= ~1ull, c->x[3] = 0;  // (the request becomes a clear, x3 0 = at once, no fade-in)
    return 0;
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
        // (the whole record, size range and density too: every block the game unpacks is scanned - a mesh or an animation
        // holding the first six numbers by chance must not be changed)
        if (o + 36 > raw || !(f[3] > 0.3f && f[3] <= 1.0f && f[4] >= 1.0f && f[4] < 3.0f && f[5] > 0.5f && f[5] < 100.0f)) continue;
        f[0] *= fade, f[1] *= fade, f[2] *= fade;
        // (the record's rest, 0.9 / 1.1 / 10.5: the size range and the density - x3 on 10.5 thinned the flowers)
        if (g_log && n == 0) LOG("[drawdist] detail record: 16 3 1 %g %g %g %g %g %g\n", f[0], f[1], f[2], f[3], f[4], f[5]);
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
static const float k_lodtab[9] = {0.01f, 0.003f, 0.001f, 0.01f, 0.005f, 0.002f, 1.8f, 0.5f, 0.2f};
static void put_lodtab(void) {
    float *t = (float *)(uintptr_t)LODTAB;
    for (int i = 0; i < 9; i++)  // (not the table this was found in - or not filled in yet: left alone)
        if (!(t[i] > 1e-7f && t[i] < 2.0f)) { if (g_log) LOG("[drawdist] model LOD table not there (%g at %d)\n", t[i], i); return; }
    float k = factor();
    if (getenv("LARECOMP_PLA_LODTAB")) k = (float)atof(getenv("LARECOMP_PLA_LODTAB"));  // (research: this table's factor alone; 1 = the game's)
    if (!(k > 0)) k = 1;
    for (int i = 0; i < 9; i++) t[i] = k_lodtab[i] / (k * k);
    if (g_log) LOG("[drawdist] model LOD cutoffs / %g (detail x%g further)\n", k * k, k);
}

// (research, with LARECOMP_PLA_TERRFN=1) terrain: f_7002b17bd4(manager, node) asks [manager+0x60](manager+0x80, node +
// camera data) whether to split the quadtree node (1) - log each distinct criterion function and the manager's floats
static int terrfn_hook(Cpu *c, uint64_t addr) {
    (void)addr;
    uint64_t o = c->x[0];
    if (o < 0x7200000000ull || o >= 0x7400000000ull) return 0;
    uint64_t fn = *(const uint64_t *)(uintptr_t)(o + 0x60);
    static uint64_t seen[8];
    static int n;
    for (int i = 0; i < n; i++) if (seen[i] == fn) return 0;
    if (n < 8) {
        seen[n++] = fn;
        uint64_t a = *(const uint64_t *)(uintptr_t)(o + 0x80), b = a ? *(const uint64_t *)(uintptr_t)(a + 0x18) : 0;
        const float *q = b ? (const float *)(uintptr_t)(b - 0x50 + 0x200) : 0;
        LOG("[terrfn] criterion %llx (manager %llx; obj %llx; +200..: %g %g %g %g %g %g %g %g)\n", (unsigned long long)fn,
            (unsigned long long)o, (unsigned long long)(b ? b - 0x50 : 0), q ? q[0] : 0, q ? q[1] : 0, q ? q[2] : 0, q ? q[3] : 0,
            q ? q[4] : 0, q ? q[5] : 0, q ? q[6] : 0, q ? q[7] : 0);
    }
    return 0;
}

// Shadows: the shadow renderer's setup (f_7002a076e0) gets its map size from the config (key d11dc1e0c2a5f6a3 -> renderer
// +0x3fc: 1024; +0x3f8: 2 cascades), copies it into the texture builder on its stack (sp+0xb0/+0xb8) and creates a
// 1024x1024x2 depth array + an R8 id array. Far flower stems fell between the far cascade's texels (no shadows past a
// few metres - the user's 2026-11-08 shots). f_7001a34908 is called between the copy and the creation (lr 7002a07a54):
// there the size goes up in both places.
static int g_shadow_res = -1;
int pla_shadow_res(void) {  // (also hle/pla_cmdpool.c: memory for the bigger maps, set up before this file's hooks)
    if (g_shadow_res < 0) {
        static const int k_size[3] = {0, 2048, 4096};  // (Settings.shadows: Switch / High / Ultra; env LARECOMP_PLA_SHADOWRES=<n>)
        g_shadow_res = g_settings.shadows >= 0 && g_settings.shadows <= 2 ? k_size[g_settings.shadows] : 0;
        if (getenv("LARECOMP_PLA_SHADOWRES")) g_shadow_res = atoi(getenv("LARECOMP_PLA_SHADOWRES"));
        if (!(g_shadow_res > 1024 && g_shadow_res <= 8192)) g_shadow_res = 0;
    }
    return g_shadow_res;
}
static int shadow_size_hook(Cpu *c, uint64_t addr) {
    (void)addr;
    if (c->x[30] != 0x7002a07a54ull || g_shadow_res <= 0) return 0;
    int32_t *res = (int32_t *)(uintptr_t)(c->x[20] + 0x3fc);
    uint64_t *bw = (uint64_t *)(uintptr_t)(c->sp + 0xb0);
    if (*res != 1024 || bw[0] != 1024 || bw[1] != 1024) {
        LOG("[pla] shadow maps: not the expected setup (%d, %llu x %llu) - left alone\n", *res, (unsigned long long)bw[0], (unsigned long long)bw[1]);
        return 0;
    }
    *res = g_shadow_res, bw[0] = bw[1] = (uint64_t)g_shadow_res;
    LOG("[pla] shadow maps %d (the game's 1024)\n", g_shadow_res);
    return 0;
}

// (research) tiles emitted (f_7002b1b6ec) per walk (f_7002b1b248)
uint32_t g_terr_tiles_max;
static _Thread_local uint32_t t_tiles, t_depth;
static int terrwalk_hook(Cpu *c, uint64_t addr) {  // (recursive: count from the outermost call)
    if (!t_depth++) t_tiles = 0;
    rt_call_original(c, addr);
    if (!--t_depth && t_tiles > g_terr_tiles_max) g_terr_tiles_max = t_tiles;
    return 1;
}
static int terremit_hook(Cpu *c, uint64_t addr) { (void)c, (void)addr; t_tiles++; return 0; }

// (research, LARECOMP_PLA_TERRHIST=1) the criterion's answers by quadtree level (0 keep, 1 split, 2 skip)
static int terrhist_hook(Cpu *c, uint64_t addr) {
    uint32_t level = *(const uint32_t *)(uintptr_t)(c->x[1] + 8);
    rt_call_original(c, addr);
    static uint32_t h[16][3], calls;
    uint32_t r = (uint32_t)c->x[0];
    if (level < 16 && r < 3) h[level][r]++;
    if (++calls % 50000 == 0) {
        extern uint32_t g_terr_tiles_max;
        LOG("[terrhist] most tiles in one walk: %u\n", g_terr_tiles_max);
        g_terr_tiles_max = 0;
        for (int l = 0; l < 16; l++)
            if (h[l][0] + h[l][1] + h[l][2]) LOG("[terrhist] level %d: keep %u split %u skip %u\n", l, h[l][0], h[l][1], h[l][2]);
        memset(h, 0, sizeof h);
    }
    return 1;
}

// Terrain mesh LOD: f_7002b17bd4 (quadtree node) -> criterion f_7002471048 splits a node when the LOD point is within
// [obj+0x208] (40 m) of the node's edge - one distance for every level, so it sets how far each finer ring reaches.
// Scaled per Draw Distance (env LARECOMP_PLA_TERRAIN = factor overrides); re-put whenever the game writes it back.
// Tiles per walk on the user's ridge save: 133 at x1, 302 at x3, 650 at x5 (the staging holds 4096); GPU cost in the noise.
static const float k_terrain[4] = {1.0f, 1.5f, 2.5f, 4.0f};
static float g_terrain = -1;
static uint64_t g_terr_obj;
static float g_terr_game, g_terr_put;
static int terrain_hook(Cpu *c, uint64_t addr) {
    (void)addr;
    uint64_t o = c->x[0];
    if (o < 0x7200000000ull || o >= 0x7400000000ull) return 0;
    uint64_t a = *(const uint64_t *)(uintptr_t)(o + 0x80);
    if (a < 0x7200000000ull || a >= 0x7400000000ull) return 0;
    uint64_t b = *(const uint64_t *)(uintptr_t)(a + 0x18);
    if (b < 0x7200000000ull + 0x50 || b >= 0x7400000000ull) return 0;
    float *t = (float *)(uintptr_t)(b - 0x50 + 0x208);
    float k = g_terrain > 0 ? g_terrain : k_terrain[g_settings.detail >= 0 && g_settings.detail <= 3 ? g_settings.detail : 0];
    if (b - 0x50 != g_terr_obj || *t != g_terr_put) {
        if (!(*t > 1.0f && *t < 1000.0f)) return 0;
        if (b - 0x50 != g_terr_obj || *t != g_terr_game * k) g_terr_game = *t;  // (the game's own value)
        g_terr_obj = b - 0x50;
        if (g_log) LOG("[drawdist] terrain detail distance %g m (the game's %g)\n", g_terr_game * k, g_terr_game);
    }
    *t = g_terr_put = g_terr_game * k;
    return 0;
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

// Distance LOD (the field's trees: the far ones are flat stand-in cards, full trees only near the player): the LOD
// selector f_7001acd904's distance mode ([model+0x1f0] = 1: the jump table's 0x7001acdb48) takes level floor(d / 100 m)
// (100.0 built in code, 0x7001acdc08), capped at the model's level count, d from the object (x19 + 0x10) to the camera
// that f_7002a11f80 returns there (its +0x40). That one call (lr 0x7001acdbb0) gets a copy of the camera moved toward
// the object to d / k, so each level reaches k times further. Found 2026-11-08 on the user's save by the beach (birches
// 100+ m away swapping as they walked): forcing the selector to 0 made every far tree full. The coverage mode (rocks)
// is put_lodtab's.
static const float k_distlod[4] = {1.0f, 1.5f, 2.0f, 3.0f};
static float g_distlod = -1;  // (LARECOMP_PLA_DISTLOD=<k>: k instead of the setting's)
static _Thread_local float t_cam[8] __attribute__((aligned(16)));
static int lodcam_hook(Cpu *c, uint64_t addr) {
    if (c->x[30] != 0x7001acdbb0ull) return 0;
    float k = g_distlod > 0 ? g_distlod : g_settings.detail >= 0 && g_settings.detail <= 3 ? k_distlod[g_settings.detail] : 1.0f;
    uint64_t obj = c->x[19];  // (the selector's object, set before this call)
    rt_call_original(c, addr);
    uint64_t cam = c->x[0];
    if (k == 1.0f || cam < 0x7200000000ull || cam >= 0x7400000000ull || obj < 0x7200000000ull || obj >= 0x7400000000ull) return 1;
    const float *cp = (const float *)(uintptr_t)(cam + 0x40), *op = (const float *)(uintptr_t)(obj + 0x10);
    for (int i = 0; i < 3; i++) t_cam[i + 4] = op[i] + (cp[i] - op[i]) / k;
    t_cam[7] = cp[3];
    c->x[0] = (uint64_t)(uintptr_t)t_cam - 0x30;  // (the selector reads only +0x40 of it)
    return 1;
}

// (research) LARECOMP_PLA_GRASSWATCH=1: the render code copies each grass cell's instances (16 B: x, z, scale, 0;
// 36 x 36) from a page-aligned staging slot (memcpy 0x70032a23b0 from lr 0x7002aac5a0); after 30 cells, page-watch the
// 64 pages above the highest slot so far - the builder's first write into a new slot there logs its function
static int grasscopy_hook(Cpu *c, uint64_t addr) {
    (void)addr;
    if (c->x[30] != 0x7002aac5a0ull) return 0;
    uint64_t src = c->x[1], len = c->x[2];
    if (len < 0x400 || len > 0x5a40 || len % 16) return 0;
    static uint64_t hi;
    static int n, armed;
    if (src + len > hi) hi = src + len;
    if (++n == 30 && !armed) {
        void watch_add_page(uint64_t addr, uint64_t len);
        armed = 1;
        uint64_t p = (hi + 0x3fffull) & ~0x3fffull;
        LOG("[grasswatch] slots up to %llx: watching %llx..%llx\n", (unsigned long long)hi, (unsigned long long)p, (unsigned long long)(p + 64 * 0x4000ull));
        for (int i = 0; i < 64; i++) watch_add_page(p + (uint64_t)i * 0x4000ull, 0x4000);
    }
    return 0;
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
    g_distlod = getenv("LARECOMP_PLA_DISTLOD") ? (float)atof(getenv("LARECOMP_PLA_DISTLOD")) : 0;
    rt_register_fnhook_at(0x7002a11f80ull, lodcam_hook);
    g_grass_fade = getenv("LARECOMP_PLA_GRASS_FADE") ? (float)atof(getenv("LARECOMP_PLA_GRASS_FADE")) : 0;
    rt_register_fnhook_at(0x7000004fc0ull, oodle_hook);  // (the setting decides per area load)
    if (getenv("LARECOMP_PLA_GRASSWATCH")) rt_register_fnhook_at(0x70032a23b0ull, grasscopy_hook);
    if (getenv("LARECOMP_PLA_LODFN")) rt_register_fnhook_at(0x700299d91cull, lodfn_hook);
    g_terrain = getenv("LARECOMP_PLA_TERRAIN") ? (float)atof(getenv("LARECOMP_PLA_TERRAIN")) : 0;
    rt_register_fnhook_at(0x7002b17bd4ull, getenv("LARECOMP_PLA_TERRFN") ? terrfn_hook : terrain_hook);
    if (pla_shadow_res()) rt_register_fnhook_at(0x7001a34908ull, shadow_size_hook);
    if (getenv("LARECOMP_PLA_SLOTWATCH")) rt_register_fnhook_at(0x7001738c14ull, slotstate_hook);
    g_npc_keep = getenv("LARECOMP_PLA_NPC_KEEP") ? atoi(getenv("LARECOMP_PLA_NPC_KEEP")) : 2;
    if (getenv("LARECOMP_PLA_NPC_MARGIN")) g_npc_fixed = sinf((float)atof(getenv("LARECOMP_PLA_NPC_MARGIN")) * 3.14159265f / 180.0f);
    if (getenv("LARECOMP_PLA_NPC_BASE")) g_npc_base = (float)atof(getenv("LARECOMP_PLA_NPC_BASE"));
    if (getenv("LARECOMP_PLA_NPC_LEAD")) g_npc_lead = (float)atof(getenv("LARECOMP_PLA_NPC_LEAD"));
    if (getenv("LARECOMP_PLA_NPC_NEAR")) g_npc_near = (float)atof(getenv("LARECOMP_PLA_NPC_NEAR"));
    if (getenv("LARECOMP_PLA_NPC_PAD")) g_npc_pad = (float)atof(getenv("LARECOMP_PLA_NPC_PAD"));
    g_npc_stats = getenv("LARECOMP_PLA_NPC_STATS") ? atoi(getenv("LARECOMP_PLA_NPC_STATS")) : 0;
    g_npc_pops = getenv("LARECOMP_PLA_NPC_POPS") != NULL;
    if (g_npc_keep || g_npc_pops || getenv("LARECOMP_PLA_SLOTWATCH")) rt_register_fnhook_at(0x70014916d0ull, npc_hide_hook);
    if (g_npc_keep >= 2 || g_npc_pops)
        rt_register_fnhook_at(0x700173740cull, npc_update_hook), rt_register_fnhook_at(0x7001736bfcull, npc_update_hook),
            rt_register_fnhook_at(0x700173d4a8ull, npc_ctx_hook);
    if (g_npc_keep >= 2) rt_register_fnhook_at(0x700173d12cull, npc_far_hook);
    if (getenv("LARECOMP_PLA_TERRHIST"))
        rt_register_fnhook_at(0x7002471048ull, terrhist_hook), rt_register_fnhook_at(0x7002b1b248ull, terrwalk_hook),
            rt_register_fnhook_at(0x7002b1b6ecull, terremit_hook);
}

// Wild Pokemon despawn distance: app_config event_culling_config's npc_distance (20 m; the file allows up to 100) is
// how far away field Pokemon are culled - not only in events (the Nexus "Improved Pokemon De-Spawn Radius" mod sets it
// to 999 and Pokemon stay out on the plains; player report 2026-11-11, with Max Draw Distance trees reaching 2500 m).
// Set as the image opens (services/fs.c): High 60, Max 99 (the file allows up to 100). Takes effect at the next
// start. LARECOMP_PLA_DESPAWN=0: the game's.
void pla_rom_edits(void) {
    if (getenv("LARECOMP_PLA_DESPAWN") && atoi(getenv("LARECOMP_PLA_DESPAWN")) == 0) return;
    // (the value as stored: length 2, "20", NUL - its length stays 2 as the Nexus mod leaves it: "999" reads as 99)
    static const uint8_t was[7] = {2, 0, 0, 0, '2', '0', 0}, high[7] = {2, 0, 0, 0, '6', '0', 0},
                         max[7] = {2, 0, 0, 0, '9', '9', 0};
    const uint8_t *p = g_settings.detail >= 3 ? max : g_settings.detail == 2 ? high : NULL;
    if (!p) return;
    const char *put = p == max ? "99" : "60";
    extern int fs_rom_edit(const char *path, const void *expect, const void *put, size_t n);
    int ok = fs_rom_edit("bin/misc/app_config/event_culling_config.bin", was, p, sizeof was);
    LOG("[drawdist] Pokemon despawn distance (event_culling npc_distance) 20 -> %s%s\n", put, ok ? "" : ": not this file, left alone");
}
