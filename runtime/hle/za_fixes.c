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
#include <pthread.h>
#include <time.h>
#include <stdatomic.h>

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
// 0x700102d380 gets the new time (s0) = old time ([player+0x90]) + the advance; the advance is scaled by our frame time /
// (1/30 s). LARECOMP_ZA_CUTSCENE_SPEED=0: off; LARECOMP_ZA_CUTSCENE_LOG=1: the first steps.
#define ZA_TIMELINE_SET_TIME 0x700102d380ull  // (player time object, s0 new time, s1 seconds, s2 rate)
#define ZA_TIMELINE_SET_RET 0x70010275ecull   // (the call from the player's update, x19 = the player)
static int timeline_step(Cpu *c, uint64_t addr) {
    (void)addr;
    extern bool pla60_enabled(void);
    extern uint64_t pla60_period_ns(void);
    if (c->x[30] != ZA_TIMELINE_SET_RET || !pla60_enabled()) return 0;
    float t, old;
    uint32_t w = (uint32_t)c->v[0];
    memcpy(&t, &w, 4);
    memcpy(&old, (const void *)(uintptr_t)(c->x[19] + 0x90), 4);
    float scale = (float)((double)pla60_period_ns() / 33333333.3), nt = old + (t - old) * scale;
    static int seen;
    if (seen < 6 && getenv("LARECOMP_ZA_CUTSCENE_LOG")) seen++, LOG("[za] timeline %g -> %g (was %g)\n", old, nt, t);
    memcpy(&w, &nt, 4);
    c->v[0] = w;
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
    { extern void za_qstats_configure(void); za_qstats_configure(); }
    { extern void za_passq_configure(void); za_passq_configure(); }
    { extern void za_jobq_configure(void); za_jobq_configure(); }
    {   // Unscaled: the internal resolution option sizes the game's 1920x1080 targets for the window (1440p: x1.33), but
        // the rscale fix-ups only cover what shaders read by frag coord, and Z-A addresses many pixels as integers
        // (texelFetch, image stores, compute: its upscaler, depth pyramid, lighting) - scaled, those passes read a
        // stretched lattice: a softer image with artifacts. Its own 1920x1080 output (the game's upscaler from its
        // dynamic resolution) is drawn to the window instead, as LZA Recomp (FerSaiyan) found. LARECOMP_FORCE_SCALE=1:
        // the scaling back.
        extern bool g_tex_no_scale;
        g_tex_no_scale = !getenv("LARECOMP_FORCE_SCALE");
    }
    { extern void za_jobs_configure(void); za_jobs_configure(); }
    {
        const char *cs = getenv("LARECOMP_ZA_CUTSCENE_SPEED");
        if (!(cs && atoi(cs) == 0)) rt_register_fnhook_at(ZA_TIMELINE_SET_TIME, timeline_step);
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
    if (e && atoi(e)) {
        if (getenv("LARECOMP_ZA_PARAMS")) rt_register_fnhook_at(ZA_PARAM_LOOKUP, param_lookup);  // (g_biped empty: no change)
        return;
    }
    static const char *names[8] = {"BipedIKWeight", "BipedIKOffsetWeight", "BipedIKControl0Weight", "BipedIKControl1Weight",
                                   "BipedIKControl0FootWeight", "BipedIKControl1FootWeight", "BipedIKControl0ToeWeight",
                                   "BipedIKControl1ToeWeight"};
    for (int i = 0; i < 8; i++) g_biped[i] = fnv1a(names[i]);
    rt_register_fnhook_at(ZA_PARAM_LOOKUP, param_lookup);
    LOG("[za] foot IK off (stretched legs workaround; LARECOMP_ZA_FOOT_IK=1 keeps it)\n");
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
