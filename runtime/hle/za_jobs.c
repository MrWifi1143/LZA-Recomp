// Z-A: more job workers than the Switch's three, on PCs with the cores for them - EXPERIMENTAL, off by default.
// larecomp.ini job_workers = 0 (the Switch's three), auto (one per fast physical core left after the emulated cores'
// and the render thread's, up to 3: none on CPUs with fewer than 8) or 1-4; LARECOMP_ZA_WORKERS=<n> overrides it.
// Lumiose at 144 fps: ~121 -> ~126 fps with 3 (more than 3 was slower on an i9-12900K: the fourth ran on an
// efficiency core); the render thread is the limit then. Known issue: one route run in five crashed in Lumiose (a job
// on the main thread found a null object, [x+0x2068] at 0x7000d9afa0) - some per-worker state still assumes three.
//
// The game runs each frame as jobs on a work-stealing scheduler (built by 0x7001cf44c0): worker 0 is the main thread,
// JobThread01 / 02 run workers 1 and 2 on cores 1 and 2, each with three priority deques (Chase-Lev: buffer, bottom
// +0x20, top +0x28, at +0x2e0 / +0x310 / +0x340 of its 0xc98-byte context). The scheduler keeps them in a fixed array
// of three (+0x18; the count at +0x16c). In Lumiose, half the frame had all three busy with ~14 more jobs waiting in
// the deques (LARECOMP_ZA_JOBQ) - work for more workers.
//
// The extra workers are made the game's own way - its worker constructor (0x7001cf4bc0, the arguments its setup
// passed for worker 2), the same thread function and start (0x7000a8a54c, "JobThreadNN") - as workers 3, 4... on
// emulated cores 4+ (kernel_set_cores; core 3 keeps the game's system and audio threads). They aren't in the
// scheduler's array; instead its steal (0x7001cf6400: from the other workers in the array) is replaced by one that
// takes from all of them, extra ones included, so the jobs their own jobs spawn are shared too. They sleep and are
// woken on the scheduler's condition variable like the others, and get the frame's values (+0xc08 / +0xc10, set for
// the three by 0x7001cf5680) when their loop (0x7001cf76f0) starts. Where the scheduler finds a worker by its index
// (its thread function; scheduling a job on a given worker) the extra ones are found too.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../hle.h"
#include "../settings.h"

void rt_register_fnhook_at(uint64_t addr, int (*fn)(Cpu *c, uint64_t addr));
void rt_call_original(Cpu *c, uint64_t addr);
void guest_call(Cpu *c, uint64_t addr);
void kernel_set_cores(int n);

enum {
    SCHED_SETUP = 0x7001cf44c0ull,  // (sched, config): builds the scheduler and its workers
    WORKER_CTOR = 0x7001cf4bc0ull,  // (allocator, &index, &sched, &[sched+0xf0], &[sched], &[config+0x10]) -> x8: context
    THREAD_START = 0x7000a8a54cull, // (context, name, core, priority)
    STEAL = 0x7001cf6400ull,        // (sched, own index) -> a job from another worker, or 0
    STEAL_FROM = 0x7001cf7640ull,   // (context) -> a job from its deques, or 0
    WORKER_LOOP = 0x7001cf76f0ull,  // (context): runs jobs, sleeps when there are none
    PUSH_BY_INFO = 0x7001cf63dcull, // (sched, job, &worker info): job onto workers[info index] (0x7001cf73b0)
    PUSH_BY_INDEX = 0x7001cf63ecull,// (sched, job, index): job onto workers[index]
    PUSH_TO = 0x7001cf73b0ull,      // (context, job)
    CONFIG_READ = 0x7001b2fc30ull,  // (config table, engine info, ...): info+0x58 = the worker core count (3 if unset)
    SCHED_FACTORY = 0x7001b24e24ull,// (engine): the job scheduler, with [[engine+0x40]+0x58] workers
    ALLOC_INIT = 0x700261e2e0ull,   // (allocator, ?, config): a heap with per-core caches
    TLS_SET_WORKER = 0x7001b503b8ull,// (thread's game TLS block, worker index): +0xb0 = it
    RQ_INIT = 0x7002685dc0ull,      // (render queue, ?, ?, desc): its per-core command lists, desc+0x48 of them
    THREAD_FN = 0x7001cf68b0ull, THREAD_FN_DESTROY = 0x7001cf6908ull,      // (the worker thread's function object,
    THREAD_FN_COPY = 0x7001cf690cull, THREAD_FN_MOVE = 0x7001cf6918ull,    //  as the setup fills it in)
};

static int g_want;                // extra workers asked for
static uint64_t g_sched;          // the scheduler they joined
static uint64_t g_extra[4];       // their contexts
static int g_nextra;
static uint64_t g_ctor_args[5];   // what the setup passed the constructor of worker 2: allocator, then the values
static int g_ctor_seen;

static int ctor_hook(Cpu *c, uint64_t addr) {
    (void)addr;
    if (g_ctor_seen < 0) return 0;  // (our own calls)
    g_ctor_args[0] = c->x[0];
    g_ctor_args[1] = *(uint64_t *)(uintptr_t)c->x[2];
    g_ctor_args[2] = *(uint64_t *)(uintptr_t)c->x[3];
    g_ctor_args[3] = *(uint64_t *)(uintptr_t)c->x[4];
    g_ctor_args[4] = *(uint64_t *)(uintptr_t)c->x[5];
    g_ctor_seen = 1;
    return 0;
}

static uint64_t worker_ctx(int i) {  // (the scheduler's workers first, then the extra ones)
    if (i < 3) return *(uint64_t *)(uintptr_t)(g_sched + 0x18 + 8 * i);
    return i - 3 < g_nextra ? g_extra[i - 3] : 0;
}

static int setup_hook(Cpu *c, uint64_t addr) {
    uint64_t sched = c->x[0];
    rt_call_original(c, addr);
    if (g_nextra && sched == g_sched) return 1;
    g_nextra = 0;  // (a scheduler of a new boot: the old one's workers are gone with it)
    if (!g_ctor_seen || *(int32_t *)(uintptr_t)(sched + 0x16c) != 3) {
        LOG("[za] job workers: the scheduler isn't the one expected - no extra workers\n");
        return 1;
    }
    g_sched = sched;
    uint64_t sp0 = c->sp, x0 = c->x[0];
    for (int k = 0; k < g_want; k++) {
        uint32_t index = 3 + (uint32_t)k;
        c->sp = sp0 - 0x80;
        uint64_t *s = (uint64_t *)(uintptr_t)c->sp;
        memset(s, 0, 0x80);
        *(uint32_t *)&s[0] = index;
        s[1] = g_ctor_args[1], s[2] = g_ctor_args[2], s[3] = g_ctor_args[3], s[4] = g_ctor_args[4];
        c->x[0] = g_ctor_args[0];
        c->x[1] = c->sp, c->x[2] = c->sp + 8, c->x[3] = c->sp + 16, c->x[4] = c->sp + 24, c->x[5] = c->sp + 32;
        c->x[8] = c->sp + 40;
        g_ctor_seen = -1;
        guest_call(c, WORKER_CTOR);
        g_ctor_seen = 1;
        uint64_t ctx = s[5];
        if (!ctx) { LOG("[za] job workers: worker %u wasn't made\n", index); break; }
        // its thread's function object, as the setup fills in a new worker's (0x7001cf4734)
        uint8_t *w = (uint8_t *)(uintptr_t)ctx;
        *(uint64_t *)(w + 0x250) = 0, *(uint32_t *)(w + 0x260) = 0;
        // (it runs the worker found at [base + 0x18 + 8 * index], base = what the setup stores here: the scheduler,
        // whose array has three - here a spare word of the function object's own storage, holding this context)
        *(uint64_t *)(w + 0x258) = ctx;
        *(uint64_t *)(w + 0x248) = ctx + 0x258 - 0x18 - 8 * (uint64_t)index;
        *(uint64_t *)(w + 0x268) = THREAD_FN, *(uint64_t *)(w + 0x270) = THREAD_FN_DESTROY;
        *(uint32_t *)(w + 0x250) = index;
        *(uint64_t *)(w + 0x278) = THREAD_FN_COPY, *(uint64_t *)(w + 0x280) = THREAD_FN_MOVE;
        __atomic_store_n((uint32_t *)(w + 0x23c), 1, __ATOMIC_RELEASE);
        g_extra[g_nextra++] = ctx;
        char *name = (char *)&s[8];
        snprintf(name, 32, "JobThread%02u", index);
        c->x[0] = ctx, c->x[1] = (uint64_t)(uintptr_t)name, c->x[2] = 4 + (uint64_t)k, c->x[3] = 0x10;
        guest_call(c, THREAD_START);
        LOG("[za] job worker %u (%s) on core %d\n", index, name, 4 + k);
    }
    c->sp = sp0, c->x[0] = x0;
    return 1;
}

// The scheduler's steal, over every worker (from the one after the thief, round the ring)
static int steal_hook(Cpu *c, uint64_t addr) {
    (void)addr;
    if (!g_nextra || c->x[0] != g_sched) return 0;
    int self = (int)(int32_t)c->x[1], n = 3 + g_nextra;
    uint64_t sched = c->x[0];
    for (int k = 1; k < n; k++) {
        uint64_t w = worker_ctx((self + k) % n);
        if (!w) continue;
        c->x[0] = w;
        guest_call(c, STEAL_FROM);
        if (c->x[0]) return 1;
    }
    c->x[0] = 0;
    (void)sched;
    return 1;
}

// The scheduler's two lookups of a worker by index (a job finishing schedules the jobs waiting on it on its worker):
// the extra workers' indexes past its array of three
static int push_hook(Cpu *c, uint64_t addr) {
    if (!g_nextra || c->x[0] != g_sched) return 0;
    int64_t index = addr == PUSH_BY_INFO ? *(int32_t *)(uintptr_t)(c->x[2] + 0xc) : (int64_t)c->x[2];
    if (index < 3) return 0;
    uint64_t w = worker_ctx((int)index);
    if (!w) return 0;
    c->x[0] = w;
    guest_call(c, PUSH_TO);
    return 1;
}

// The engine's worker core count (a field of its system config, 3 when the data doesn't set it) sizes its per-core
// tables (the render queue's per-core command lists, found by GetCurrentProcessorNumber; a thread on a core past them
// shares one list that isn't locked - the extra workers' render jobs then corrupted it). Raised to cover the extra
// workers' cores; the job scheduler, whose worker array holds three, still gets 3 (the extra workers join it here).
static int config_hook(Cpu *c, uint64_t addr) {
    uint64_t info = c->x[1];
    rt_call_original(c, addr);
    int32_t *n = (int32_t *)(uintptr_t)(info + 0x58);
    if (*n == 3) {
        *n = 4 + g_want;
        LOG("[za] worker core count 3 -> %d (cores 0-%d)\n", *n, *n - 1);
    }
    return 1;
}
static int factory_count_hook(Cpu *c, uint64_t addr) {
    int32_t *n = (int32_t *)(uintptr_t)(*(uint64_t *)(uintptr_t)(c->x[0] + 0x40) + 0x58);
    int32_t keep = *n;
    *n = 3;
    rt_call_original(c, addr);
    *n = keep;
    return 1;
}

// The render queue's per-core command lists (0x7002685dc0's desc+0x48 of them, 3; found by GetCurrentProcessorNumber):
// a thread on a core past them shares one list that isn't locked - the extra workers' render jobs corrupted it.
static int rq_init_hook(Cpu *c, uint64_t addr) {
    (void)addr;
    int32_t *n = (int32_t *)(uintptr_t)(c->x[3] + 0x48);
    if (*n == 3) *n = 4 + g_want;
    return 0;
}

// The game's heap allocators (made by 0x700261e2e0 (allocator, ?, config)) keep a cache per core, found by
// GetCurrentProcessorNumber: config+0x16a18+0x8a0 = on, +0x8a4 = how many (3; room for 8). Past them the extra
// workers' allocations corrupted the heap - one per core they run on.
static int alloc_init_hook(Cpu *c, uint64_t addr) {
    (void)addr;
    uint8_t *k = (uint8_t *)(uintptr_t)(c->x[2] + 0x16a18);
    int32_t *n = (int32_t *)(k + 0x8a4);
    if (k[0x8a0] && *n == 3) *n = 4 + g_want;
    return 0;
}

// A worker thread's index also goes to its TLS block (+0xb0), where 51 places in the game look it up to use their
// per-worker slot (arrays of three: the extra workers' indexes overwrote what follows them - a lock and a vector, in
// one) - each also handles -1, "not a job worker", the way other threads use them. The extra workers are that there.
static int tls_worker_hook(Cpu *c, uint64_t addr) {
    (void)addr;
    if ((int32_t)c->x[1] >= 3) c->x[1] = 0xFFFFFFFFu;
    return 0;
}

static int loop_hook(Cpu *c, uint64_t addr) {
    (void)addr;
    for (int i = 0; i < g_nextra; i++)
        if (c->x[0] == g_extra[i]) {
            uint64_t w0 = worker_ctx(0);
            uint8_t *w = (uint8_t *)(uintptr_t)g_extra[i];
            *(uint64_t *)(w + 0xc08) = *(uint64_t *)(uintptr_t)(w0 + 0xc08);
            *(uint32_t *)(w + 0xc10) = *(uint32_t *)(uintptr_t)(g_sched + 0x174);
        }
    return 0;
}

void za_jobs_configure(void);
void za_jobs_configure(void) {
    extern void settings_load(void);
    extern int cpupin_fast_cores(void);
    settings_load();  // (before boot: the start screen may have been skipped)
    const char *e = getenv("LARECOMP_ZA_WORKERS");
    g_want = e ? atoi(e) : g_settings.job_workers;
    if (g_want < 0) g_want = cpupin_fast_cores() - 5 < 3 ? cpupin_fast_cores() - 5 : 3;
    if (g_want <= 0) { g_want = 0; LOG("[za] job workers: the game's three\n"); return; }
    if (g_want > 4) g_want = 4;
    kernel_set_cores(4 + g_want);
    rt_register_fnhook_at(WORKER_CTOR, ctor_hook);
    rt_register_fnhook_at(SCHED_SETUP, setup_hook);
    rt_register_fnhook_at(STEAL, steal_hook);
    rt_register_fnhook_at(WORKER_LOOP, loop_hook);
    rt_register_fnhook_at(CONFIG_READ, config_hook);
    rt_register_fnhook_at(RQ_INIT, rq_init_hook);
    rt_register_fnhook_at(ALLOC_INIT, alloc_init_hook);
    rt_register_fnhook_at(TLS_SET_WORKER, tls_worker_hook);
    rt_register_fnhook_at(SCHED_FACTORY, factory_count_hook);
    rt_register_fnhook_at(PUSH_BY_INFO, push_hook);
    rt_register_fnhook_at(PUSH_BY_INDEX, push_hook);
    LOG("[za] job workers: the game's three and %d more\n", g_want);
}
