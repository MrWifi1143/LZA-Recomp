// Legends Z-A workarounds (2.0.2, main build id B1F12FD919EAE86A), until their causes are found.
//
// Stretched legs: running, the player's feet stay where the run began - the legs stretch back toward that spot, and
// stay so after stopping. The game's biped foot IK (its BipedIK* weights, named float parameters looked up through
// 0x700134ad10(owner, key {FNV-1a 64 hash, name}) -> s0) locks the feet and never lets go. Its ground raycasts
// (0x7003014970) hit the ground as they should, and every FP/SIMD, integer and load/store instruction form the game
// uses matches a reference ARM emulator (Unicorn), so the cause is elsewhere. With those weights looked up as 0 the
// legs follow the animation: correct on flat ground, the feet just don't adapt to slopes and steps.
// 2026-10-29: gone - with the game's foot IK the run, the stop and standing are right at 30, 60 and 144 fps (the
// cemetery, the Lumiose walk with its kerbs), so the game's foot IK is on again; one of the fixes since (the lifter,
// the kernel, HLE) cured it, which one wasn't bisected. LARECOMP_ZA_FOOT_IK=0 brings the workaround back.
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
#include <SDL3/SDL.h>  // (first: on Windows its intrinsics header must come before others)
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <time.h>
#include <stdatomic.h>

#include "../hle.h"
#include "../settings.h"
#include "../platform.h"

void rt_register_fnhook_at(uint64_t addr, int (*fn)(Cpu *c, uint64_t addr));
void rt_call_original(Cpu *c, uint64_t addr);

#define ZA_PARAM_LOOKUP 0x700134ad10ull

static uint64_t fnv1a(const char *s) {
    uint64_t h = 0xcbf29ce484222645ull;
    for (; *s; s++) h = (h ^ (uint8_t)*s) * 0x100000001b3ull;
    return h;
}

static uint64_t g_biped[8];

// (research, LARECOMP_ZA_PARAMS=1: every parameter read through the lookup - its hash, how often, its values - logged
// every 600 frames as "[za] param"; names: FNV-1a 64 of the game's strings)
static struct { uint64_t hash, n, changes; float last, lo, hi; } g_params[512];
static int g_nparams;
static void param_note(uint64_t hash, float v) {
    int i = 0;
    while (i < g_nparams && g_params[i].hash != hash) i++;
    if (i == g_nparams) {
        if (g_nparams == 512) return;
        g_params[g_nparams++] = (typeof(g_params[0])){hash, 0, 0, v, v, v};
    }
    if (v != g_params[i].last) g_params[i].changes++;
    g_params[i].n++, g_params[i].last = v;
    if (v < g_params[i].lo) g_params[i].lo = v;
    if (v > g_params[i].hi) g_params[i].hi = v;
    extern uint64_t renderer_game_frame(void);
    static uint64_t dumped;
    uint64_t f = renderer_game_frame();
    if (f / 600 != dumped / 600) {
        dumped = f;
        for (int k = 0; k < g_nparams; k++)
            LOG("[za] param %016llx reads %llu changes %llu last %g range %g..%g\n", (unsigned long long)g_params[k].hash,
                (unsigned long long)g_params[k].n, (unsigned long long)g_params[k].changes, g_params[k].last, g_params[k].lo, g_params[k].hi);
    }
}

static int param_lookup(Cpu *c, uint64_t addr) {
    static int stats = -1;
    if (stats < 0) stats = getenv("LARECOMP_ZA_PARAMS") != NULL;
    uint64_t hash = *(const uint64_t *)(uintptr_t)c->x[1];
    rt_call_original(c, addr);
    for (int i = 0; i < 8; i++)
        if (hash == g_biped[i] && g_biped[i]) { c->v[0] = 0; break; }  // (the weight, s0)
    if (stats) { float v; memcpy(&v, &c->v[0], 4); param_note(hash, v); }
    return 1;
}

// Choppy NPCs: an NPC's animation advances every frame, but its pose is only applied to the skeleton when a per-frame LOD
// decision allows it: a callback each animation component holds (component + 0x180, called from 0x7001e73abc) answers
// whether to evaluate this frame, kept at component + 0x1f4, and 0x7001dcdf40 (animation item -> [+0x48] component)
// applies the pose only then. NPCs standing or walking around get it once every 60 frames - 2 s at 30 fps, 0.4 s at 144 -
// while a conversation puts everyone nearby on every frame. Here it is always set: every NPC's pose follows its animation
// each frame. (2026-10-20: forcing that flag only re-applied the same pose - the gate below is the real fix.)
// LARECOMP_ZA_NPC_LOD=1 keeps the game's choice.

#define ZA_POSE_APPLY 0x7001dcdf40ull  // (x0 = animation item; [x0 + 0x48] -> its component, + 0x1f4: evaluate this frame)
volatile int g_za_npc_full = 0;  // (research: LARECOMP_LIVE <dir>/npcfull "0" / "1" switches it while the game runs)
static int pose_apply(Cpu *c, uint64_t addr) {
    uint64_t item = c->x[0];
    uint64_t comp = item ? *(uint64_t *)(uintptr_t)(item + 0x48) : 0;
    if (getenv("LARECOMP_ZA_NPC_STATS") && comp >= 0x7000000000ull && comp < 0x8000000000ull) {  // (research: the LOD callback)
        static uint64_t seen[24];
        static int n;
        uint64_t fn = *(uint64_t *)(uintptr_t)(comp + 0x180);
        int i = 0;
        while (i < n && seen[i] != fn) i++;
        if (i == n && n < 24) {
            seen[n++] = fn;
            const uint64_t *q = (const uint64_t *)(uintptr_t)(comp + 0x160);
            LOG("[za] anim component %llx: +0x160.. %llx %llx %llx %llx %llx %llx %llx %llx flag %d\n", (unsigned long long)comp,
                (unsigned long long)q[0], (unsigned long long)q[1], (unsigned long long)q[2], (unsigned long long)q[3], (unsigned long long)q[4],
                (unsigned long long)q[5], (unsigned long long)q[6], (unsigned long long)q[7], *(uint8_t *)(uintptr_t)(comp + 0x1f4));
        }
    }
    if (!g_za_npc_full) return 0;
    if (comp >= 0x7000000000ull && comp < 0x8000000000ull) *(uint8_t *)(uintptr_t)(comp + 0x1f4) = 1;
    (void)addr;
    return 0;
}

// The real gate (2026-10-20): the component's callback (component + 0x180, called from 0x7001e73abc with x0 = state,
// x1 = &dt, x2 = &skip, x3 = &threshold, x4 = &accumulated time, x5 = &&counter) adds dt to the accumulated time and
// evaluates only once the counter has reached the threshold - then with all the time since the last evaluation.
// Forcing the "apply" flag alone (pose_apply) re-applied the old pose. These hooks make the threshold always met:
// 0x7001e75b20 / 0x7001e75b78 (w1 skip, w2 threshold, x3 accumulated, x4 counter) and 0x70029019f8 (handled here:
// the accumulated time + dt, as the other two compute it).
#define ZA_ANIM_GATE_A 0x7001e75b20ull
#define ZA_ANIM_GATE_A2 0x7001e75b78ull
#define ZA_ANIM_GATE_B 0x70029019f8ull
volatile int g_za_gate_on = 1;  // (research: LARECOMP_LIVE <dir>/npcfull "0" / "1")
static int anim_gate_a(Cpu *c, uint64_t addr) {
    if (!g_za_gate_on) return 0;
    if (!(c->x[1] & 1)) c->x[2] = 0x80000000u;  // (w2: INT32_MIN, the counter is always >= it)
    (void)addr;
    return 0;
}
static int anim_gate_b(Cpu *c, uint64_t addr) {
    if (!g_za_gate_on) return 0;
    float *acc = *(float **)(uintptr_t)c->x[4];
    float dt = *(const float *)(uintptr_t)c->x[1];
    float t = *acc + dt;
    *acc = 0.0f;
    if (t > 1.0f) t = 1.0f;
    uint32_t bits;
    memcpy(&bits, &t, 4);
    c->x[0] = (uint64_t)bits | 0x100000000ull;
    (void)addr;
    return 1;
}

// Cutscenes (and the other timelines the same player runs) at the right speed above 30 fps. The player's update
// (0x70010273f0) advances its time by the frame time divided by the engine's frame time for the present interval (the
// table entry runtime/hle/fps60.c patches to our frame time, so the world steps by it): one 30-fps frame a game frame
// at any frame rate - the opening ran 4.8 times too fast at 144 fps (over in ~25 s instead of ~2 min). Its call to
// 0x700102d380 gets the new time (s0) = old time ([player+0x90]) + the advance; the advance is scaled by the real time
// since the player's last update / (1/30 s), so a late frame doesn't leave it behind its audio. LARECOMP_ZA_CUTSCENE_SPEED=0: off; LARECOMP_ZA_CUTSCENE_LOG=1: the first steps,
// 2: each player's updates, real time and advance once a second. (s1 is the frame time, s2 the player's rate: 0.5 for
// the cutscenes, which step 1.0 an update, 1 for the battles' players, which step s1 x 60 x s2 - by time already.)
#define ZA_TIMELINE_SET_TIME 0x700102d380ull  // (player time object, s0 new time, s1 seconds, s2 rate)
#define ZA_TIMELINE_SET_RET 0x70010275ecull   // (the call from the player's update, x19 = the player)
static int timeline_step(Cpu *c, uint64_t addr) {
    (void)addr;
    extern bool pla60_enabled(void);
    extern uint64_t pla60_period_ns(void);
    if (c->x[30] != ZA_TIMELINE_SET_RET || !pla60_enabled()) return 0;
    float t, old, s1, s2;
    uint32_t w = (uint32_t)c->v[0], w1 = (uint32_t)c->v[1], w2 = (uint32_t)c->v[2];
    memcpy(&t, &w, 4);
    memcpy(&s1, &w1, 4), memcpy(&s2, &w2, 4);
    memcpy(&old, (const void *)(uintptr_t)(c->x[19] + 0x90), 4);
    // Only the players the game steps a whole frame per update: some (the battles' - their intro, the text's wait for A)
    // it already steps by time, frame time (s1) x 60 x rate (s2), and scaling those again ran them at a quarter speed
    // at 144 fps (the trainer's ball behind her arm; "You are challenged by ...!" not taking A for seconds)
    float by_time = s1 * 60.0f * s2;
    if (by_time > 0 && fabsf((t - old) - by_time) <= 0.1f * by_time) return 0;
    // the real time since this player's last update (the audio plays in real time: a frame late - a stutter, a shader
    // compiled - is made up on the next one), our frame time on its first, 100 ms at most
    static struct { uint64_t player, at; } last[32];
    uint64_t now = plat_now_ns(), dt = pla60_period_ns();
    int k = 0;
    while (k < 31 && last[k].player && last[k].player != c->x[19]) k++;
    if (last[k].player == c->x[19] && now - last[k].at < 100000000ull) dt = now - last[k].at;
    else if (now - last[k].at >= 100000000ull && last[k].player == c->x[19]) dt = 100000000ull;
    last[k].player = c->x[19], last[k].at = now;
    float scale = (float)((double)dt / 33333333.3), nt = old + (t - old) * scale;
    static int seen;
    if (seen < 6 && getenv("LARECOMP_ZA_CUTSCENE_LOG")) seen++, LOG("[za] timeline %g -> %g (was %g)\n", old, nt, t);
    {   // (LARECOMP_ZA_CUTSCENE_LOG=2: each player's calls, real time and advance, once a second)
        static int lv = -1;
        if (lv < 0) lv = getenv("LARECOMP_ZA_CUTSCENE_LOG") ? atoi(getenv("LARECOMP_ZA_CUTSCENE_LOG")) : 0;
        static struct { uint64_t player, calls; double dt, adv, game; } st[32];
        static uint64_t since;
        if (lv == 2) {
            st[k].player = c->x[19], st[k].calls++, st[k].dt += (double)dt / 1e9, st[k].adv += nt - old, st[k].game += t - old;
            if (!since) since = now;
            if (now - since > 1000000000ull) {
                for (int j = 0; j < 32; j++)
                    if (st[j].calls)
                        LOG("[za] timeline player %llx: %llu updates, %.3f s real, advanced %.2f frames (the game's own advance %.2f)\n",
                            (unsigned long long)st[j].player, (unsigned long long)st[j].calls, st[j].dt, st[j].adv, st[j].game),
                        st[j].calls = 0, st[j].dt = st[j].adv = st[j].game = 0;
                since = now;
            }
        }
    }
    memcpy(&w, &nt, 4);
    c->v[0] = w;
    return 0;
}

// Higher resolution, the game's own way. Z-A draws its scene at 0.65 of its screen size inside render targets made at the
// largest screen size, then upscales to the screen with its own upscaler: docked 1920x1080 (scene 1248x696), handheld
// 1280x720. Scaling its targets from outside (output_scale.c, as for Legends: Arceus) breaks it - many of its passes
// address pixels as integers - so the screen size itself is changed: the output size in its system config (below),
// the docked entry of the render pipeline's screen sizes and the size its targets are made at (two constants), all
// before the renderer is made. Every pass then agrees on it, as on the Switch 2. The internal resolution option picks
// it: render_height lines (1440: 2560x1440, scene 1664x936), 1080 x the multiplier, or the display's height (Match
// display; the 75 / 50 / 25 % choices of it); 16:9, multiples of 8. LARECOMP_ZA_RES=<w>x<h> sets it directly.
static void za_screen_size(uint64_t *w, uint64_t *h) {
    const char *r = getenv("LARECOMP_ZA_RES");
    if (r && strchr(r, 'x') && atoi(r) >= 640 && atoi(strchr(r, 'x') + 1) >= 360) {
        *w = (uint64_t)atoi(r) & ~7ull, *h = (uint64_t)atoi(strchr(r, 'x') + 1) & ~7ull;
        return;
    }
    double lines = 1080;
    int mode = g_settings.res_mode;
    if (g_settings.res_height > 0) lines = g_settings.res_height;
    else if (mode >= 1 && mode <= 8) lines = 1080.0 * mode;
    else {
        int dh = 0;
        if (SDL_WasInit(SDL_INIT_VIDEO)) {
            const SDL_DisplayMode *dm = SDL_GetCurrentDisplayMode(SDL_GetPrimaryDisplay());
            if (dm) dh = dm->h;
        }
        lines = dh >= 360 ? dh : 1080;
        if (mode == RES_Q75) lines *= 0.75;
        else if (mode == RES_Q50) lines *= 0.5;
        else if (mode == RES_Q25) lines *= 0.25;
    }
    if (lines < 720) lines = 720;
    if (lines > 4320) lines = 4320;
    *h = (uint64_t)lines & ~7ull;
    *w = ((uint64_t)(lines * 16.0 / 9.0 + 0.5)) & ~7ull;
}

// The scene's resolution scale: 0x7000f59970 ([x0] = the scene renderer) reads the engine values ResolutionWidth /
// ResolutionHeight and stores them over the size its targets are made at, a constant 1920 x 1080 (0x7003308150) -
// handheld 1280 x 720 draws into part of those targets: 0.667. Effects that read the scene's depth by screen position
// scale their coordinates by it (the city's hologram walls: the depth they fade against, cbuf 15). With the targets
// made at the screen size (config_read, below) that constant is wrong: 1440p gave 1.333 - the walls read the depth a
// third further down and right, lost their lower part where it found the floor and showed the player's outline. The
// constant itself also serves the UI's 1920 x 1080 layout space, so only this result is corrected: by 1920 / width.
#define ZA_RES_SCALE 0x7000f59970ull
static uint64_t g_res_sw = 1920, g_res_sh = 1080;
static int res_scale_hook(Cpu *c, uint64_t addr) {
    uint64_t scene = *(const uint64_t *)(uintptr_t)c->x[0];
    rt_call_original(c, addr);
    float *k = (float *)(uintptr_t)(scene + 0x24c0);
    static int logged;
    if (!logged++) LOG("[za] resolution scale %g %g -> %g %g\n", k[0], k[1], k[0] * 1920.0f / (float)g_res_sw, k[1] * 1080.0f / (float)g_res_sh);
    k[0] *= 1920.0f / (float)g_res_sw, k[1] *= 1080.0f / (float)g_res_sh;
    return 1;
}

// The engine's system config (read by 0x7001b2fc30 (config table, engine info, ...) into the info struct): +0x10 / +0x18
// the output width / height (1920 x 1080 unless the data sets them), +0x58 the worker core count (hle/za_jobs.c).
// (the output size: set by the screen size, below)
#define ZA_CONFIG_READ 0x7001b2fc30ull
#define ZA_SCREEN_SIZES 0x70031a14e0ull  // (u32 handheld w, docked w, handheld h, docked h: 1280, 1920, 720, 1080)
static int config_read(Cpu *c, uint64_t addr) {
    uint64_t info = c->x[1];
    rt_call_original(c, addr);
    extern void za_jobs_config_read(uint64_t info);
    za_jobs_config_read(info);
    uint64_t drs = *(uint64_t *)(uintptr_t)(info + 0x28);  // (dynamic resolution: u8 on, then four floats)
    if (drs) {
        const float *f = (const float *)(uintptr_t)(drs + 4);
        LOG("[za] dynamic resolution: on %d, %g %g %g %g\n", *(uint8_t *)(uintptr_t)drs, f[0], f[1], f[2], f[3]);
        const char *d = getenv("LARECOMP_ZA_DRS");
        if (d && !strcmp(d, "off")) *(uint8_t *)(uintptr_t)drs = 0, LOG("[za] dynamic resolution off (LARECOMP_ZA_DRS)\n");
    }
    uint64_t sw, sh;
    za_screen_size(&sw, &sh);
    if (sw != 1920 || sh != 1080) {
        g_res_sw = sw, g_res_sh = sh;
        static bool hooked;
        if (!hooked) hooked = true, rt_register_fnhook_at(ZA_RES_SCALE, res_scale_hook);
        uint64_t *w = (uint64_t *)(uintptr_t)(info + 0x10), *h = (uint64_t *)(uintptr_t)(info + 0x18);
        LOG("[za] screen %llux%llu (the game draws its scene at 0.65 of it and upscales to it)\n", (unsigned long long)sw, (unsigned long long)sh);
        *w = sw, *h = sh;
        // the render pipeline's screen sizes by performance mode {handheld w, docked w, handheld h, docked h} (read-only data,
        // copied into each pipeline when it's made: 0x7000ecf788)
        uint32_t *t = (uint32_t *)(uintptr_t)ZA_SCREEN_SIZES;
        if (t[1] == 1920 && t[3] == 1080) {
            plat_protect(ZA_SCREEN_SIZES & ~(PLAT_PAGE - 1), PLAT_PAGE, PLAT_PROT_RW);
            t[1] = (uint32_t)*w, t[3] = (uint32_t)*h;
            LOG("[za] docked screen %ux%u\n", t[1], t[3]);
        }
        // the render pipeline's target sizes (read-only data, read when it's made): full, half and quarter of 1920x1080
        static const struct { uint64_t addr; int u64, f32, div; } k[] = {
            {0x70031a48b0ull, 1, 0, 1},  // u64 1920, 1080: the targets (0x7000ededa0 and four more setups)
            {0x70031a3478ull, 0, 0, 1},  // u32 1920, 1080 (0x7000e6d058)
            {0x70031a0ec0ull, 0, 1, 1},  // f32 1920, 1080 (0x7000e6c7bc)
            {0x70031a3a70ull, 1, 0, 2},  // u64 960, 540: half-size targets (five setups)
            {0x70031a1f80ull, 1, 0, 4},  // u64 480, 270 (0x7000ee026c)
            {0x70031a48d0ull, 0, 0, 4},  // u32 480, 270 (0x7000eeff3c)
        };
        for (size_t i = 0; i < sizeof k / sizeof *k; i++) {
            uint64_t ow = 1920 / k[i].div, oh = 1080 / k[i].div, nw = *w / k[i].div, nh = *h / k[i].div;
            void *a = (void *)(uintptr_t)k[i].addr;
            plat_protect(k[i].addr & ~(PLAT_PAGE - 1), PLAT_PAGE, PLAT_PROT_RW);
            if (k[i].u64) { uint64_t *v = a; if (v[0] == ow && v[1] == oh) v[0] = nw, v[1] = nh; }
            else if (k[i].f32) { float *v = a; if (v[0] == (float)ow && v[1] == (float)oh) v[0] = (float)nw, v[1] = (float)nh; }
            else { uint32_t *v = a; if (v[0] == ow && v[1] == oh) v[0] = (uint32_t)nw, v[1] = (uint32_t)nh; }
        }
    }
    return 1;
}

// (research, LARECOMP_ZA_VPWATCH=<width>: the render state's viewport flush (0x70018b7390: viewports at x0+0xc4) - the
// first time one is that wide, a hardware watchpoint on its width names who writes it)
static int vp_flush_watch(Cpu *c, uint64_t addr) {
    (void)addr;
    static int armed;
    static float want = -1;
    if (want < 0) want = (float)atof(getenv("LARECOMP_ZA_VPWATCH"));
    float w;
    memcpy(&w, (const void *)(uintptr_t)(c->x[0] + 0xc4 + 8), 4);
    if (!armed && w == want) {
        armed = 1;
        extern void hwwatch_arm(uint64_t addr, uint64_t len);
        LOG("[za] viewport %g in state %llx: watching\n", w, (unsigned long long)c->x[0]);
        hwwatch_arm(c->x[0] + 0xc4 + 8, 4);
    }
    return 0;
}

// (research, LARECOMP_ZA_RTWATCH=<width>: the render target lookup 0x70018a4da4 (its object: +0x68 / +0x70 the size the
// viewport is set to) - the first object that wide gets a hardware watchpoint on its width, and is logged)
static int rt_lookup_watch(Cpu *c, uint64_t addr) {
    static int armed;
    static uint64_t want;
    if (!want) want = strtoull(getenv("LARECOMP_ZA_RTWATCH"), NULL, 0);
    // (0x70018b7088 (state, slot, target handle, ..): the handle {u8 valid, u64 object} -> the object, as 0x70018a4da4)
    uint64_t hnd = addr == 0x70018b7088ull ? c->x[2] : c->x[1];
    uint64_t o = hnd >= GUEST_LO && hnd < GUEST_HI && *(uint8_t *)(uintptr_t)hnd ? *(uint64_t *)(uintptr_t)(hnd + 8) : 0;
    static int nlog;
    if (nlog < 30 && o >= GUEST_LO && o < GUEST_HI)
        nlog++, LOG("[za] bind %llx: target %llx size %llu x %llu\n", (unsigned long long)addr, (unsigned long long)o,
                    (unsigned long long)*(uint64_t *)(uintptr_t)(o + 0x68), (unsigned long long)*(uint64_t *)(uintptr_t)(o + 0x70));
    if (!armed && o >= GUEST_LO && o < GUEST_HI && *(uint64_t *)(uintptr_t)(o + 0x68) == want) {
        armed = 1;
        const uint64_t *q = (const uint64_t *)(uintptr_t)o;
        LOG("[za] render target %llx: size %llu x %llu; words %llx %llx %llx %llx %llx %llx %llx %llx\n", (unsigned long long)o,
            (unsigned long long)q[13], (unsigned long long)q[14], (unsigned long long)q[8], (unsigned long long)q[9], (unsigned long long)q[10],
            (unsigned long long)q[11], (unsigned long long)q[12], (unsigned long long)q[15], (unsigned long long)q[16], (unsigned long long)q[17]);
        extern void hwwatch_arm(uint64_t addr, uint64_t len);
        hwwatch_arm(o + 0x68, 8);
    }
    return 0;
}

// The renderer's setup (0x70017faa10 (renderer, desc)): desc+0x48 / +0x50 = its screen size (u64), what its render
// targets and the scene's 0.65 viewport are sized from. LARECOMP_ZA_RES=<w>x<h> sets it too.
#define ZA_RENDERER_SETUP 0x70017faa10ull
static int renderer_setup(Cpu *c, uint64_t addr) {
    (void)addr;
    uint64_t *d = (uint64_t *)(uintptr_t)(c->x[1] + 0x48);
    const char *r = getenv("LARECOMP_ZA_RES");
    LOG("[za] renderer setup: screen %llu x %llu\n", (unsigned long long)d[0], (unsigned long long)d[1]);
    if (r && strchr(r, 'x')) d[0] = strtoull(r, NULL, 10), d[1] = strtoull(strchr(r, 'x') + 1, NULL, 10);
    return 0;
}

// (research, LARECOMP_ZA_PERFLOG=1: who asks nn::oe::GetPerformanceMode (sdk 0x70083f70e0), each caller once)
static int perfmode_log(Cpu *c, uint64_t addr) {
    (void)addr;
    static uint64_t seen[32];
    static int n;
    for (int i = 0; i < n; i++) if (seen[i] == c->x[30]) return 0;
    if (n < 32) seen[n++] = c->x[30];
    uint64_t fp = c->x[29], up1 = fp >= GUEST_LO && fp < GUEST_HI ? ((uint64_t *)(uintptr_t)fp)[1] : 0;
    LOG("[za] GetPerformanceMode from %llx (its caller %llx)\n", (unsigned long long)c->x[30], (unsigned long long)up1);
    return 0;
}

// (research: the render pipeline's per-mode sizes - 0x7000ed2dc0 (pipeline): +0x288 + mode * 4 width, +0x290 + mode * 4
// height, mode at +0x2b4 (0 normal / handheld, 1 boost / docked))
static int pipeline_sizes(Cpu *c, uint64_t addr) {
    (void)addr;
    const uint32_t *w = (const uint32_t *)(uintptr_t)(c->x[0] + 0x288), *h = (const uint32_t *)(uintptr_t)(c->x[0] + 0x290);
    static int n;
    if (n++ < 4)
        LOG("[za] pipeline %llx sizes: mode 0 %ux%u, mode 1 %ux%u (mode %d)\n", (unsigned long long)c->x[0], w[0], h[0], w[1], h[1],
            *(int32_t *)(uintptr_t)(c->x[0] + 0x2b4));
    return 0;
}

// (research, LARECOMP_ZA_RTLOG=1: render target creation 0x700044e390 (?, desc, ...) - the descriptions holding 1920)
static int rt_create_log(Cpu *c, uint64_t addr) {
    (void)addr;
    const uint32_t *d = (const uint32_t *)(uintptr_t)c->x[1];
    int at = -1;
    for (int i = 0; i < 24; i++) if (d[i] == 1920) { at = i; break; }
    static int n;
    if (at >= 0 && n < 30) {
        n++;
        LOG("[za] rt create from %llx: desc %llx, 1920 at +0x%x: %x %x %x %x %x %x %x %x %x %x %x %x\n", (unsigned long long)c->x[30],
            (unsigned long long)c->x[1], at * 4, d[0], d[1], d[2], d[3], d[4], d[5], d[6], d[7], d[8], d[9], d[10], d[11]);
    }
    return 0;
}

// Mouse look. The gameplay camera's update (the function around 0x7000371068) turns it by the right stick at its turn
// speed, from the angles it keeps (yaw in radians, pitch 0..1: +0x4a0 / +0x4a4 of its state), plus two angles of its
// own (+0x158 yaw, +0x15c pitch, radians: added once, then cleared - direct camera input), and hands them to 0x7000371af0
// (camera, yaw, pitch). The held mouse's motion goes to those two: the camera turns by as much as the mouse moved,
// with no stick curve, acceleration or top speed, from the next frame; a controller's stick still turns it as before.
// LARECOMP_ZA_MOUSE_LOOK=0: the mouse is the camera stick instead (as on other cameras: menus, cutscenes, aiming).
#define ZA_CAM_ANGLES 0x7000371af0ull      // (camera, s0 yaw, s1 pitch)
#define ZA_CAM_ANGLES_RET 0x700037106cull  // (the gameplay camera's call)
static bool g_mouse_look;
extern uint64_t g_field_camera_frame;  // (runtime/hle/menu_mouse.c: the player's camera turned - not in a menu)
static int mouse_look_hook(Cpu *c, uint64_t addr) {
    (void)addr;
    if (c->x[30] != ZA_CAM_ANGLES_RET) return 0;
    extern uint64_t renderer_game_frame(void);
    g_field_camera_frame = renderer_game_frame();
    if (!g_mouse_look) return 0;
    extern bool input_mouse_camera_take(float *dyaw, float *dpitch);
    float dy, dp;
    if (input_mouse_camera_take(&dy, &dp)) {
        float *add = (float *)(uintptr_t)(c->x[0] + 0x158);  // (cleared just before this call: read next frame)
        add[0] += dy, add[1] += dp;
    }
    return 0;
}

// Hotel Z room 202 lock above 60 fps: at 144 fps (sometimes 120) Ev_m01_0210 - the player teleported into room 202,
// then walked in by FObj_Move_To_PosXYZ, which the event waits for - leaves the player turning on the spot, the walk
// never done and the event never over; at 60 it works. What goes wrong happens at the event's start (a cap from the
// walk's own call is too late; from the event's first native call it works), the cause itself isn't found. So scripted
// scenes run at 60 fps when the frame rate is above it: the game's script natives are hardly ever called in free roam
// (3 in a minute's walk), and the object lookup 0x7001517e30 (fB51E7F78) opens almost every scene that moves someone.
// From its first call (or a scripted walk's, 0x7000ae3cb0) to 4 s after the last. LARECOMP_ZA_SCENE60=0: off.
#define ZA_SCRIPT_OBJ 0x7001517e30ull
#define ZA_MOVE_TO 0x7000ae3cb0ull
bool legends_set_fps(int fps);  // runtime/hle/fps60.c
static int g_move_fps;          // the frame rate to come back to (0: not lowered)
static _Atomic uint64_t g_move_until;  // (plat_now_ns)
static int scene60_hook(Cpu *c, uint64_t addr) {
    (void)c, (void)addr;
    if (g_settings.fps <= 60) return 0;
    static uint64_t hold = 0;  // (LARECOMP_ZA_SCENE60_S=<seconds>: research)
    if (!hold) hold = getenv("LARECOMP_ZA_SCENE60_S") ? (uint64_t)atoi(getenv("LARECOMP_ZA_SCENE60_S")) * 1000000000ull : 4000000000ull;
    atomic_store(&g_move_until, plat_now_ns() + hold);
    if (!g_move_fps && legends_set_fps(60)) g_move_fps = g_settings.fps, LOG("[za] scripted scene: 60 fps for now\n");
    return 0;
}
void za_fixes_present(void) {  // (each present: za_options_present)
    if (g_move_fps && plat_now_ns() > atomic_load(&g_move_until)) {
        int f = g_move_fps;
        g_move_fps = 0;
        if (g_settings.fps == f) legends_set_fps(f);  // (unless the player changed it meanwhile)
    }
}

// Draw distance for people. The game draws at most 20 characters (people and their Pokemon) in full at once: the
// pass 0x70002ffeb0 (x0 -> {.., +8 &config, +0x10 &count}) walks them nearest first and lets through as many as the
// config's +0xf8 says; the rest wait for a free place (people near the station's railing appear, or swap with others,
// on the way). Found 2026-10-27 from the model swap (the timed behaviour 0x7000d58b20 its result creates). The pass
// also only takes characters on a ready list (config +0xd8 -> object, sorted handles at +0x258, count +0x260), which
// 0x70000b27f0 rebuilds each frame with at most the object's +0x310 (25) nearest: the station's kid with a Staryu is
// 26th from the start spot. Settings.crowd multiplies both of the game's own numbers: x1 (the Switch), x2, x4, x10.
// Shadows: the shadow renderer's setup (0x7000f11dd0) reads the config value "ShadowResolution" (1024: three cascades
// of 1024x1024 depth plus an R8 id map, which the shadow mask shader reads by their size) through the config object's
// getter (vtable +0x58: key {FNV-1a hash, name}, out). Settings.shadows asks for 2048 or 4096 instead and the game sizes
// the maps, their viewports and its filter offsets by it - people's and Pokemon's shadows show their shapes instead of
// blobs. Read once, at the start (the setup's first run looks the getter up the way the setup does).
void guest_call(Cpu *c, uint64_t addr);
static int shadow_res_getter(Cpu *c, uint64_t addr) {
    static uint64_t key;
    if (!key) key = fnv1a("ShadowResolution");
    uint64_t k = c->x[1], out = c->x[2];
    if (k < 0x7000000000ull || k >= 0x8000000000ull || *(uint64_t *)(uintptr_t)k != key) return 0;
    rt_call_original(c, addr);
    static const int k_size[3] = {0, 2048, 4096};
    int n = g_settings.shadows >= 1 && g_settings.shadows <= 2 ? k_size[g_settings.shadows] : 0;
    int32_t *v = (int32_t *)(uintptr_t)out;
    if (n && *v > 0 && *v < n) LOG("[za] shadow maps %d (the game's %d)\n", n, *v), *v = n;
    return 1;
}
static int shadow_setup_hook(Cpu *c, uint64_t addr) {
    (void)addr;
    static bool hooked;
    if (hooked) return 0;
    uint64_t sx[32], snzcv = c->nzcv;
    __uint128_t sv[32];
    memcpy(sx, c->x, sizeof sx), memcpy(sv, c->v, sizeof sv);
    guest_call(c, 0x70001134e4ull);  // (as the setup does: the config object)
    uint64_t o = c->x[0] ? *(uint64_t *)(uintptr_t)c->x[0] : 0;
    if (o) {
        c->x[0] = o + 0x30;
        guest_call(c, *(uint64_t *)(uintptr_t)(*(uint64_t *)(uintptr_t)(o + 0x30) + 0x40));
        uint64_t cfg = c->x[0];
        if (cfg) {
            uint64_t get = *(uint64_t *)(uintptr_t)(*(uint64_t *)(uintptr_t)cfg + 0x58);
            rt_register_fnhook_at(get, shadow_res_getter);
            hooked = true;
        }
    }
    memcpy(c->x, sx, sizeof sx), memcpy(c->v, sv, sizeof sv), c->nzcv = snzcv;
    return 0;
}

// Draw distance (detail), Settings.detail: the scenes' trinity_ModelLodSetting entries ({0 name, 1 path regex,
// 2 [distances], 3, 4}: "Field" "^field/model/.*?$" - trees, props, buildings - 50 / 100 / 150 m, "chara" 8 / 15 / 30,
// "pokemon" 50 / 100 / 150) say how far away a model keeps each of its levels of detail (a tree's farther ones have
// fewer leaves: they fill in as you walk up). Their create callback 0x70010f10f0 hands each to 0x7001091700 (x3 the
// distances), which copies them into a registry entry (made by 0x7001096050; the distances at +0x58) that the renderer
// reads every frame: scaled as they are copied, and again in place when the setting changes (za_set_detail).
// LARECOMP_ZA_LODSCALE=<k>: x k instead.
#define ZA_LOD_SETTING_ADD 0x7001091700ull
#define ZA_LOD_ENTRY_MAKE 0x7001096050ull
static struct { float *v; float orig[3]; uint32_t n; } g_lod[64];
static float g_lod_k = -1;  // (the factor g_lod's entries hold)
static uint64_t g_lod_entry;
static int g_nlod;
static float detail_k(void) {
    static const float k_mul[4] = {1, 2, 4, 10};
    const char *e = getenv("LARECOMP_ZA_LODSCALE");
    if (e && atof(e) > 0) return (float)atof(e);
    return g_settings.detail >= 0 && g_settings.detail <= 3 ? k_mul[g_settings.detail] : 1;
}
// Rescales the entries that still hold what was written into them (else their scene is gone and the memory reused:
// dropped, left alone).
static void lod_rescale(float k) {
    int m = 0;
    for (int j = 0; j < g_nlod; j++) {
        bool ours = true;
        for (uint32_t i = 0; i < g_lod[j].n; i++) ours = ours && g_lod[j].v[i] == g_lod[j].orig[i] * g_lod_k;
        if (!ours) continue;
        for (uint32_t i = 0; i < g_lod[j].n; i++) g_lod[j].v[i] = g_lod[j].orig[i] * k;
        g_lod[m++] = g_lod[j];
    }
    g_nlod = m, g_lod_k = k;
}
void za_set_detail(void) {
    float k = detail_k();
    if (g_lod_k >= 0 && k != g_lod_k) LOG("[za] model detail distances x%g (%d entries)\n", k, g_nlod), lod_rescale(k);
}
static int lod_setting_add(Cpu *c, uint64_t addr) {
    uint64_t d = c->x[3];
    if (d < 0x7000000000ull || d >= 0x8000000000ull) return 0;
    uint32_t n = *(const uint32_t *)(uintptr_t)(d - 4);
    if (n == 0 || n > 3) return 0;
    if (g_lod_k < 0) g_lod_k = detail_k();
    float *v = (float *)(uintptr_t)d, orig[3];
    memcpy(orig, v, n * 4);
    for (uint32_t i = 0; i < n; i++) v[i] = orig[i] * g_lod_k;
    g_lod_entry = 0;
    rt_call_original(c, addr);
    memcpy(v, orig, n * 4);  // (the scene's own data as it was, should it be read again)
    float *e = g_lod_entry ? (float *)(uintptr_t)(g_lod_entry + 0x58) : NULL;
    for (uint32_t i = 0; e && i < n; i++) if (e[i] != orig[i] * g_lod_k) e = NULL;
    if (e && g_nlod == 64) lod_rescale(g_lod_k);
    if (e && g_nlod < 64) g_lod[g_nlod].v = e, g_lod[g_nlod].n = n, memcpy(g_lod[g_nlod].orig, orig, n * 4), g_nlod++;
    LOG("[za] model detail distances %g %g %g m x%g%s\n", orig[0], n > 1 ? orig[1] : 0, n > 2 ? orig[2] : 0, g_lod_k, e ? "" : " (not found to change later)");
    return 1;
}
static int lod_entry_make(Cpu *c, uint64_t addr) {
    rt_call_original(c, addr);
    g_lod_entry = c->x[0];
    return 1;
}

#define ZA_CHAR_DETAIL_PASS 0x70002ffeb0ull
static int crowd_hook(Cpu *c, uint64_t addr) {
    (void)addr;
    static const int k_mul[4] = {1, 2, 4, 10};
    uint64_t a = c->x[0];
    if (a < 0x7000000000ull || a >= 0x8000000000ull) return 0;
    uint64_t pc = *(uint64_t *)(uintptr_t)(a + 8);
    if (pc < 0x7000000000ull || pc >= 0x8000000000ull) return 0;
    uint64_t cfg = *(uint64_t *)(uintptr_t)pc;
    if (cfg < 0x7000000000ull || cfg >= 0x8000000000ull) return 0;
    int32_t *budget = (int32_t *)(uintptr_t)(cfg + 0xf8);
    uint64_t ready = *(uint64_t *)(uintptr_t)(cfg + 0xd8);
    int32_t *rcap = ready >= 0x7000000000ull && ready < 0x8000000000ull ? (int32_t *)(uintptr_t)(ready + 0x310) : NULL;
    static struct { uint64_t at; int32_t own; } seen[8], rseen[8];  // (each config's / ready list's own number, before ours)
    int k = 0, r = 0;
    while (k < 8 && seen[k].at && seen[k].at != cfg) k++;
    if (k == 8) return 0;
    if (!seen[k].at) seen[k].at = cfg, seen[k].own = *budget, LOG("[za] characters in full: %d at once (the game's)\n", *budget);
    while (rcap && r < 8 && rseen[r].at && rseen[r].at != ready) r++;
    if (r == 8) rcap = NULL;
    if (rcap && !rseen[r].at) rseen[r].at = ready, rseen[r].own = *rcap, LOG("[za] characters ready: %d at once (the game's)\n", *rcap);
    int m = g_settings.crowd >= 0 && g_settings.crowd <= 3 ? k_mul[g_settings.crowd] : 1;
    *budget = seen[k].own * m;
    if (rcap && rseen[r].own > 0 && rseen[r].own < 1000) *rcap = rseen[r].own * m;
    return 0;
}

// (research, LARECOMP_ZA_DITHER=<k>: models dissolve between 75 m and 90 m from the camera on the Switch -
// world/ik_data/gfx/model_dither_fade/model_dither_fade_default_param/default.bin, a 44-byte FlatBuffer {0 FarBegin 75,
// 1 FarEnd 90, 2 NearBegin 5, 3 NearEnd 2, 4 FadeTime 0.2}: both far distances times k wherever it is read)
static const uint8_t k_dither_param[44] = {
    0x14, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0e, 0x00, 0x18, 0x00, 0x04, 0x00, 0x08, 0x00, 0x0c, 0x00,
    0x10, 0x00, 0x14, 0x00, 0x0e, 0x00, 0x00, 0x00, 0x00, 0x00, 0x96, 0x42, 0x00, 0x00, 0xb4, 0x42,
    0x00, 0x00, 0xa0, 0x40, 0x00, 0x00, 0x00, 0x40, 0xcd, 0xcc, 0x4c, 0x3e};
void za_game_data_loaded(uint8_t *data, uint64_t size) {
    static float k = -1;
    if (k < 0) k = getenv("LARECOMP_ZA_DITHER") ? (float)atof(getenv("LARECOMP_ZA_DITHER")) : 0;
    if (k <= 0) return;
    for (uint8_t *p = data; size >= sizeof k_dither_param;) {
        uint8_t *hit = memmem(p, size, k_dither_param, sizeof k_dither_param);
        if (!hit) break;
        float *far = (float *)(hit + 0x18);
        far[0] = 75.0f * k, far[1] = 90.0f * k;
        LOG("[za] model distance fade: %g - %g m\n", far[0], far[1]);
        size -= (uint64_t)(hit + sizeof k_dither_param - p), p = hit + sizeof k_dither_param;
    }
}

// (research: the collision component factory 0x7001108570 (.., x2 -> its FlatBuffer: field 1 shape {0 type, 1 params
// {1 radius, 2 height}, 9 name}). LARECOMP_ZA_COLLOG=1 logs each shape with the frame; LARECOMP_ZA_TRIG=<name>=<k>
// scales that trigger's radius (its own scene value remembered, so the factor never compounds))
static uint64_t fb_field(uint64_t t, int idx) {
    if (!t) return 0;
    uint64_t vt = t - (uint64_t)(int64_t)*(const int32_t *)(uintptr_t)t;
    uint16_t vsize = *(const uint16_t *)(uintptr_t)vt;
    if (4 + 2 * idx + 2 > vsize) return 0;
    uint16_t off = *(const uint16_t *)(uintptr_t)(vt + 4 + 2 * idx);
    return off ? t + off : 0;
}
static uint64_t fb_ref(uint64_t f) { return f ? f + *(const uint32_t *)(uintptr_t)f : 0; }
static int collision_make(Cpu *c, uint64_t addr) {
    (void)addr;
    uint64_t p = *(uint64_t *)(uintptr_t)c->x[2];
    if (!p) return 0;
    uint64_t t = fb_ref(p), shape = fb_ref(fb_field(t, 1)), nm = fb_ref(fb_field(shape, 9));
    const char *name = nm ? (const char *)(uintptr_t)(nm + 4) : "-";
    uint64_t pr = fb_ref(fb_field(shape, 1));
    if (getenv("LARECOMP_ZA_COLLOG")) {
        uint64_t ty = fb_field(shape, 0);
        float v[4] = {0};
        for (int i = 0; i < 4; i++) { uint64_t f = fb_field(pr, i); if (f) v[i] = *(float *)(uintptr_t)f; }
        extern uint64_t renderer_present_count(void);
        LOG("[za] col %s type %u params %g %g %g %g (frame %llu)\n", name, ty ? *(uint8_t *)(uintptr_t)ty : 255, v[0], v[1], v[2], v[3],
            (unsigned long long)renderer_present_count());
        static long from = -2;  // (LARECOMP_ZA_COLBT=<frame>: the guest call stack of the first 12 made from that frame on)
        static int nbt;
        if (from == -2) from = getenv("LARECOMP_ZA_COLBT") ? atol(getenv("LARECOMP_ZA_COLBT")) : -1;
        if (from >= 0 && renderer_present_count() >= (uint64_t)from && nbt < 12) {
            char line[400];
            int k = snprintf(line, sizeof line, "lr %llx:", (unsigned long long)c->x[30]);
            uint64_t fp = c->x[29];
            for (int d = 0; d < 16 && fp >= 0x7200000000ull && fp < 0x7400000000ull && !(fp & 7) && k < 380; d++) {
                k += snprintf(line + k, sizeof line - k, " %llx", (unsigned long long)((const uint64_t *)(uintptr_t)fp)[1]);
                fp = ((const uint64_t *)(uintptr_t)fp)[0];
            }
            nbt++, LOG("[za] col bt %s\n", line);
        }
    }
    const char *tr = getenv("LARECOMP_ZA_TRIG");
    const char *eq = tr ? strchr(tr, '=') : NULL;
    if (!eq || strlen(name) != (size_t)(eq - tr) || strncmp(name, tr, (size_t)(eq - tr))) return 0;
    float *r = (float *)(uintptr_t)fb_field(pr, 1);
    if (!r) return 0;
    static struct { float *at; float v; } orig[1024];
    unsigned h = (unsigned)(((uintptr_t)r >> 2) * 2654435761u) & 1023u;
    for (int i = 0; i < 1024 && orig[h].at && orig[h].at != r; i++) h = (h + 1) & 1023u;
    if (!orig[h].at) orig[h].at = r, orig[h].v = *r;
    if (orig[h].at == r) *r = orig[h].v * (float)atof(eq + 1);
    return 0;
}

// (research: 0x70008f0680 reads an area's FieldAreaInfo; its streamingConfig lands at +0x108..+0x11c: grid realize /
// unrealize, override-sensor realize / unrealize, conditional realize / unrealize offsets. LARECOMP_ZA_AREALOG=1 logs
// them, LARECOMP_ZA_REALIZE=<f> adds f to all six)
static int area_info_read(Cpu *c, uint64_t addr) {
    uint64_t a = c->x[0];
    rt_call_original(c, addr);
    float *o = (float *)(uintptr_t)(a + 0x108);
    static int n;
    if (n++ < 64) LOG("[za] area %llx streaming offsets: grid %g / %g, sensor %g / %g, conditional %g / %g\n", (unsigned long long)a,
                      o[0], o[1], o[2], o[3], o[4], o[5]);
    const char *f = getenv("LARECOMP_ZA_REALIZE");
    if (f) for (int i = 0; i < 6; i++) o[i] += (float)atof(f);
    return 1;
}
// (research: a scene's GridStreamingGroup realizes what it holds within a distance of the player; its factory
// 0x70010e4ae0 (result via x8: u8 made, the group at +8) sets +0x240 realize, +0x244 unrealize. LARECOMP_ZA_SGDIST=<k>)
static int stream_group_make(Cpu *c, uint64_t addr) {
    uint64_t res = c->x[8];
    rt_call_original(c, addr);
    if (!res || !*(uint8_t *)(uintptr_t)res) return 1;
    uint64_t g = *(uint64_t *)(uintptr_t)(res + 8);
    if (!g) return 1;
    float *d = (float *)(uintptr_t)(g + 0x240);
    float k = getenv("LARECOMP_ZA_SGDIST") ? (float)atof(getenv("LARECOMP_ZA_SGDIST")) : 1.0f;
    static int n;
    if (n++ < 40) LOG("[za] streaming group %llx: realize %g m, unrealize %g m (x%g)\n", (unsigned long long)g, d[0], d[1], k);
    d[0] *= k, d[1] *= k;
    return 1;
}

void za_fixes_configure(const char *exefs) {
    if (getenv("LARECOMP_ZA_AREALOG") || getenv("LARECOMP_ZA_REALIZE")) rt_register_fnhook_at(0x70008f0680ull, area_info_read);
    if (getenv("LARECOMP_ZA_AREALOG") || getenv("LARECOMP_ZA_SGDIST")) rt_register_fnhook_at(0x70010e4ae0ull, stream_group_make);
    if (getenv("LARECOMP_ZA_COLLOG") || getenv("LARECOMP_ZA_TRIG")) rt_register_fnhook_at(0x7001108570ull, collision_make);
    if (getenv("LARECOMP_ZA_DITHER")) { extern void (*g_romfs_read_hook)(uint8_t *, uint64_t); g_romfs_read_hook = za_game_data_loaded; }
    static const uint8_t ZA_MAIN[8] = {0xb1, 0xf1, 0x2f, 0xd9, 0x19, 0xea, 0xe8, 0x6a};  // 2.0.2 (any other build: none of this)
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
    g_mouse_look = !getenv("LARECOMP_ZA_MOUSE_LOOK") || atoi(getenv("LARECOMP_ZA_MOUSE_LOOK")) != 0;
    rt_register_fnhook_at(ZA_CAM_ANGLES, mouse_look_hook);
    { extern void za_options_configure(void); za_options_configure(); }
    {   // (runtime/hle/menu_mouse.c: ui2d pane matrices, the menu navigator's core, the scroll lists' select cell)
        extern void menu_mouse_configure(uint64_t, uint64_t, uint64_t, uint32_t, uint32_t);
        menu_mouse_configure(0x700264424cull, 0x7000fe5720ull, 0x7000fc2630ull, 0x690, 0x694);
    }
    { extern void za_qstats_configure(void); za_qstats_configure(); }
    { extern void za_passq_configure(void); za_passq_configure(); }
    { extern void za_jobq_configure(void); za_jobq_configure(); }
    {   // Unscaled: the internal resolution option sizes the game's 1920x1080 targets for the window (1440p: x1.33), but
        // the rscale fix-ups only cover what shaders read by frag coord, and Z-A addresses many pixels as integers
        // (texelFetch, image stores, compute: its upscaler, depth pyramid, lighting) - scaled, those passes read a
        // stretched lattice: a softer image with artifacts. Its own output is drawn to the window instead, as LZA Recomp
        // (FerSaiyan) found; the internal resolution option sets the game's screen size instead (za_screen_size, above:
        // every pass then agrees). LARECOMP_FORCE_SCALE=1: the scaling back.
        extern bool g_tex_no_scale;
        g_tex_no_scale = !getenv("LARECOMP_FORCE_SCALE");
    }
    { extern void za_jobs_configure(void); za_jobs_configure(); }
    { extern void za_press_configure(void); za_press_configure(); }  // (hle/za_press.c: presses the battle text misses)
    rt_register_fnhook_at(ZA_CONFIG_READ, config_read);
    {   // a screen above 1440p needs more than the Switch's memory for its targets (4K: the StandardAllocator ran out)
        uint64_t sw, sh;
        za_screen_size(&sw, &sh);
        const char *e = getenv("LARECOMP_ZA_HEAP_MB");
        if (e) g_stdheap_size = strtoull(e, NULL, 0) << 20;
        else if (sh > 2160) g_stdheap_size = HEAP_MAX;  // (capped to the window by heaps.c)
        else if (sh > 1440) g_stdheap_size = 0x120000000ull;  // 4.5 GB (4K's route ran in 4 GB, not in 2.75)
        if (!e && g_settings.shadows > 0)  // (bigger shadow maps come out of it too: 2048 +60 MB, 4096 +240 MB)
            g_stdheap_size += g_settings.shadows == 2 ? 0x10000000ull : 0x4000000ull;
        // (and the characters Draw Distance lets in: Max at 1440p with Ultra shadows ran out opening the menu)
        static const uint64_t k_crowd_mb[4] = {0, 256, 512, 1280};
        if (!e && g_settings.crowd > 0 && g_settings.crowd <= 3) g_stdheap_size += k_crowd_mb[g_settings.crowd] << 20;
        // (Shaders reach the game's memory through its GPU address since 2026-10-28 - exec.c gmem_ref_info: no 4 GB
        // limit any more. As a storage buffer it was capped at 4 GB, maxStorageBufferRange: past that every text was gone.)
        if (g_stdheap_size != STDHEAP_SIZE) LOG("[za] screen %llux%llu: %llu MB for the game's allocator\n", (unsigned long long)sw,
                                                (unsigned long long)sh, (unsigned long long)(g_stdheap_size >> 20));
    }
    rt_register_fnhook_at(ZA_RENDERER_SETUP, renderer_setup);
    if (getenv("LARECOMP_ZA_PERFLOG")) rt_register_fnhook_at(0x7000ed2dc0ull, pipeline_sizes);
    if (getenv("LARECOMP_ZA_RTLOG")) rt_register_fnhook_at(0x700044e390ull, rt_create_log);
    if (getenv("LARECOMP_ZA_PERFLOG")) rt_register_fnhook_at(0x70083f70e0ull, perfmode_log);
    if (getenv("LARECOMP_ZA_RTWATCH")) rt_register_fnhook_at(0x70018b7088ull, rt_lookup_watch), rt_register_fnhook_at(0x70018b71a4ull, rt_lookup_watch);
    if (getenv("LARECOMP_ZA_VPWATCH")) rt_register_fnhook_at(0x70018b7390ull, vp_flush_watch);
    {
        const char *cs = getenv("LARECOMP_ZA_CUTSCENE_SPEED");
        if (!(cs && atoi(cs) == 0)) rt_register_fnhook_at(ZA_TIMELINE_SET_TIME, timeline_step);
    }
    rt_register_fnhook_at(ZA_CHAR_DETAIL_PASS, crowd_hook);
    rt_register_fnhook_at(ZA_LOD_SETTING_ADD, lod_setting_add);
    rt_register_fnhook_at(ZA_LOD_ENTRY_MAKE, lod_entry_make);
    if (g_settings.shadows > 0) rt_register_fnhook_at(0x7000f11dd0ull, shadow_setup_hook);
    {
        const char *m = getenv("LARECOMP_ZA_SCENE60");
        if (!(m && atoi(m) == 0)) rt_register_fnhook_at(ZA_SCRIPT_OBJ, scene60_hook), rt_register_fnhook_at(ZA_MOVE_TO, scene60_hook);
    }
    const char *lod = getenv("LARECOMP_ZA_NPC_LOD");
    if (!(lod && atoi(lod))) {
        rt_register_fnhook_at(ZA_ANIM_GATE_A, anim_gate_a);
        rt_register_fnhook_at(ZA_ANIM_GATE_A2, anim_gate_a);
        rt_register_fnhook_at(ZA_ANIM_GATE_B, anim_gate_b);
        if (getenv("LARECOMP_ZA_NPC_STATS")) rt_register_fnhook_at(ZA_POSE_APPLY, pose_apply);  // (research)
        LOG("[za] NPC animation evaluated every frame (LARECOMP_ZA_NPC_LOD=1 keeps the game's animation LOD)\n");
    }
    const char *e = getenv("LARECOMP_ZA_FOOT_IK");
    if (!e || atoi(e)) {  // (the game's foot IK: the default since the stretched legs are gone)
        if (getenv("LARECOMP_ZA_PARAMS")) rt_register_fnhook_at(ZA_PARAM_LOOKUP, param_lookup);  // (g_biped empty: no change)
        return;
    }
    static const char *names[8] = {"BipedIKWeight", "BipedIKOffsetWeight", "BipedIKControl0Weight", "BipedIKControl1Weight",
                                   "BipedIKControl0FootWeight", "BipedIKControl1FootWeight", "BipedIKControl0ToeWeight",
                                   "BipedIKControl1ToeWeight"};
    for (int i = 0; i < 8; i++) g_biped[i] = fnv1a(names[i]);
    rt_register_fnhook_at(ZA_PARAM_LOOKUP, param_lookup);
    LOG("[za] foot IK off (LARECOMP_ZA_FOOT_IK=0: the old stretched-legs workaround)\n");
}

// (research, LARECOMP_ZA_QSTATS=1: the render queue lookup 0x7001802c90 called from the queue walk 0x700180ee10 -
// how often it finds no renderable for a queued id)
static uint64_t g_q_calls, g_q_null;
static int queue_lookup_hook(Cpu *c, uint64_t addr) {
    uint64_t lr = c->x[30], out = c->x[8];
    rt_call_original(c, addr);
    if (lr == 0x700180ee74ull) {
        g_q_calls++;
        if (out >= 0x7000000000ull && !*(uint64_t *)(uintptr_t)out) g_q_null++;
        if ((g_q_calls & 0x3FFFF) == 0) LOG("[za] render queue lookups %llu, not found %llu\n", (unsigned long long)g_q_calls, (unsigned long long)g_q_null);
    }
    return 1;
}
void za_qstats_configure(void);
void za_qstats_configure(void) {
    if (getenv("LARECOMP_ZA_QSTATS")) rt_register_fnhook_at(0x7001802c90ull, queue_lookup_hook);
}

// (research, LARECOMP_ZA_PASSQ=1: each render pass's queue walk 0x700180ee10 - the pass object and its queue length)
static int passq_hook(Cpu *c, uint64_t addr) {
    static uint64_t seen[64], last_frame;
    static int nseen;
    extern uint64_t renderer_game_frame(void);
    uint64_t f = renderer_game_frame();
    uint64_t p = c->x[0];
    uint64_t b = *(uint64_t *)(uintptr_t)(p + 0x168), e = *(uint64_t *)(uintptr_t)(p + 0x170);
    if (f % 600 == 0 && f != last_frame) { last_frame = f; nseen = 0; }
    int i = 0;
    while (i < nseen && seen[i] != p) i++;
    if (i == nseen && nseen < 64) {
        seen[nseen++] = p;
        uint64_t nm = *(uint64_t *)(uintptr_t)(p + 0x40);
        const char *name = nm >= 0x7000000000ull && nm < 0x8000000000ull ? (const char *)(uintptr_t)nm : "?";
        LOG("[za] f%llu pass %llx %.40s queue %llu items (lr %llx)\n", (unsigned long long)f, (unsigned long long)p, name,
            (unsigned long long)((e - b) / 16), (unsigned long long)c->x[30]);
    }
    (void)addr;
    return 0;
}
void za_passq_configure(void);
void za_passq_configure(void) {
    if (getenv("LARECOMP_ZA_PASSQ")) rt_register_fnhook_at(0x700180ec2cull, passq_hook);
}

// (research, LARECOMP_ZA_JOBQ=1: would more job workers help? The job system (scheduler at 0x7001cf5680's x0: workers
// at +0x18, 3 of them) - a sampler every 200 us: how many jobs run (the job runner 0x7001cf3900, wrapped) and how
// many wait in the workers' queues (each worker: three work-stealing deques at +0x2e0 / +0x310 / +0x340).)
static _Atomic uint64_t g_jobq_sched;
static _Atomic int g_jobq_running;
static int jobq_kick_hook(Cpu *c, uint64_t addr) { (void)addr; g_jobq_sched = c->x[0]; return 0; }
static int jobq_run_hook(Cpu *c, uint64_t addr) {
    g_jobq_running++;
    rt_call_original(c, addr);
    g_jobq_running--;
    return 1;
}
static void *jobq_sampler(void *arg) {
    (void)arg;
    uint64_t n = 0, by_run[5] = {0}, wait_when_full = 0, full_with_wait = 0, pend_sum = 0;
    for (;;) {
        struct timespec ts = {0, 200000};
        nanosleep(&ts, NULL);
        uint64_t s = g_jobq_sched;
        if (!s) continue;
        int run = g_jobq_running;
        if (run > 4) run = 4;
        int64_t pend = 0;
        for (int i = 0; i < 3; i++) {
            uint64_t w = *(uint64_t *)(uintptr_t)(s + 0x18 + 8 * i);
            if (w < 0x7000000000ull || w >= 0x8000000000ull) continue;
            for (int q = 0; q < 3; q++) {  // (its three work-stealing deques, by priority: bottom - top)
                int64_t d = *(int64_t *)(uintptr_t)(w + 0x300 + 0x30 * q) - *(int64_t *)(uintptr_t)(w + 0x308 + 0x30 * q);
                if (d > 0 && d < 4096) pend += d;
            }
        }
        n++, by_run[run]++;
        if (run >= 3) { pend_sum += (uint64_t)pend; if (pend > 0) full_with_wait++; }
        (void)wait_when_full;
        if (n % 10000 == 0) {
            LOG("[jobq] %llu samples: running 0/1/2/3+ jobs %.0f%% %.0f%% %.0f%% %.0f%% | all busy and jobs waiting %.0f%% of the time, %.1f waiting on average then\n",
                (unsigned long long)n, 100.0 * by_run[0] / n, 100.0 * by_run[1] / n, 100.0 * by_run[2] / n, 100.0 * (by_run[3] + by_run[4]) / n,
                100.0 * full_with_wait / n, by_run[3] + by_run[4] ? (double)pend_sum / (by_run[3] + by_run[4]) : 0.0);
            n = 0, memset(by_run, 0, sizeof by_run), full_with_wait = pend_sum = 0;
        }
    }
    return NULL;
}
void za_jobq_configure(void);
void za_jobq_configure(void) {
    if (!getenv("LARECOMP_ZA_JOBQ")) return;
    rt_register_fnhook_at(0x7001cf5680ull, jobq_kick_hook);
    rt_register_fnhook_at(0x7001cf3900ull, jobq_run_hook);
    pthread_t th;
    pthread_create(&th, NULL, jobq_sampler, NULL);
    pthread_detach(th);
}
