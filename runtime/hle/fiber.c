// Horizon fibers (nn::os::SwitchToFiber; nn::async's fiber tasks: Legends Z-A's "Ready!" after character creation).
// The SDK switches with nn::os::detail::DispatchFiberContext(save, load): it pushes the callee-saved registers on the
// guest stack, stores that sp in *save, takes the sp in *load and returns there. Recompiled code also keeps its frames
// on the host stack, so here every guest context gets a host context (ucontext) and a Cpu of its own: a fiber may
// resume on another thread (the job system's workers), whose own Cpu is busy. The context word (*save) is the sp the
// SDK would have used: the key of the suspended host context. A key we don't know is a new fiber: its frame is on the
// guest stack (FiberImplByHorizon::InitializeImpl: x0 = the fiber at +0, the entry trampoline as x30 at +0x60).
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../hle.h"

#ifndef _WIN32
#include <sys/mman.h>
#include <ucontext.h>

uint64_t loader_find_symbol(const char *name);
void guest_call(Cpu *c, uint64_t addr);
extern __thread Cpu *g_fiber_cpu;  // kernel.c: kernel_current_cpu() while a fiber runs

#define HOST_STACK (8ull << 20)  // reserved, touched on use (guest recursion runs on the host stack)
#define FRAME_X30 0x60           // offsets in DispatchFiberContext's 0xf0-byte frame
#define FRAME_Q8 0x70

typedef struct HFiber {
    ucontext_t uc;
    void *stack;          // NULL: a thread's own context (its pthread stack)
    Cpu *cpu;
    uint64_t key;         // context word while suspended
    struct HFiber *next;  // suspended list / free list
} HFiber;

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static HFiber *g_suspended, *g_free;
static __thread HFiber *t_running;  // NULL until this thread first switches (it runs its own context)
static __thread HFiber *t_left;     // the context just switched away from, published by whoever runs next
static __thread uint64_t t_left_save;

static HFiber *take_suspended(uint64_t key) {  // (g_lock held)
    for (HFiber **p = &g_suspended; *p; p = &(*p)->next)
        if ((*p)->key == key) {
            HFiber *f = *p;
            *p = f->next;
            return f;
        }
    return NULL;
}

// After a switch, on the new context: the old one is complete now (its registers are saved), so it may be resumed.
// The context word goes to the guest last, as DispatchFiberContext's own store of sp does.
static void publish_left(void) {
    HFiber *f = t_left;
    if (!f) return;
    t_left = NULL;
    pthread_mutex_lock(&g_lock);
    f->next = g_suspended;
    g_suspended = f;
    pthread_mutex_unlock(&g_lock);
    __atomic_store_n((uint64_t *)(uintptr_t)t_left_save, f->key, __ATOMIC_RELEASE);
}

static void copy_thread_state(Cpu *to, const Cpu *from) {  // what belongs to the thread, not the context
    to->tpidr = from->tpidr;
    to->tpidrro = from->tpidrro;
    to->thread = from->thread;
    to->fpcr = from->fpcr;
    to->fpsr = from->fpsr;
    to->excl_addr = ~0ull;
    to->host_fpcr = from->host_fpcr;  // (the host FP state is the thread's too)
}

static void fiber_entry(void) {
    publish_left();
    Cpu *c = t_running->cpu;
    c->host_fpcr = 1;  // (swapcontext loaded the host FP state saved with this context: simd.h syncs it)
    rt_dispatch(c);  // the SDK's trampoline: runs the fiber function, then switches away for good
    FATAL("fiber: a fiber's entry returned (key %llx)\n", (unsigned long long)t_running->key);
}

static HFiber *new_fiber(uint64_t frame, const Cpu *from) {
    HFiber *f;
    pthread_mutex_lock(&g_lock);
    f = g_free;
    if (f) g_free = f->next;
    pthread_mutex_unlock(&g_lock);
    if (!f) {
        f = calloc(1, sizeof *f);
        f->stack = mmap(NULL, HOST_STACK, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
        if (f->stack == MAP_FAILED) FATAL("fiber: no host stack\n");
        mprotect(f->stack, 0x1000, PROT_NONE);  // guard
        f->cpu = aligned_alloc(16, sizeof(Cpu));
    }
    Cpu *c = f->cpu;
    memset(c, 0, sizeof *c);
    copy_thread_state(c, from);
    const uint64_t *fr = (const uint64_t *)(uintptr_t)frame;
    c->x[0] = fr[0];
    for (int i = 0; i < 11; i++) c->x[19 + i] = fr[1 + i];  // x19..x29
    for (int i = 0; i < 8; i++) memcpy(&c->v[8 + i], (const void *)(uintptr_t)(frame + FRAME_Q8 + 16 * i), 16);
    c->pc = fr[FRAME_X30 / 8];
    c->x[30] = 0;
    c->sp = frame + 0xf0;
    getcontext(&f->uc);
    f->uc.uc_stack.ss_sp = f->stack;
    f->uc.uc_stack.ss_size = HOST_STACK;
    f->uc.uc_link = NULL;
    makecontext(&f->uc, fiber_entry, 0);
    return f;
}

// A finished fiber (nn::os::FinalizeFiber, called through the import: the game's): its last suspended context (the trampoline's final switch) is never resumed.
static void drop_fiber_of(uint64_t fiber) {
    if (!fiber) return;
    uint64_t key = *(uint64_t *)(uintptr_t)(fiber + 0x50);
    pthread_mutex_lock(&g_lock);
    HFiber *f = key ? take_suspended(key) : NULL;
    if (f && f->stack) { f->next = g_free; g_free = f; }  // (host stack and Cpu reused by the next new fiber)
    else if (f) { f->next = g_suspended; g_suspended = f; }  // (a thread's own context: not ours to drop)
    pthread_mutex_unlock(&g_lock);
}

// nn::os::detail::DispatchFiberContext(FiberContextForHorizon **save, FiberContextForHorizon **load)
HLE_FUNC("_ZN2nn2os6detail20DispatchFiberContextEPPNS1_22FiberContextForHorizonES4_", fiber_dispatch) {
    uint64_t save = c->x[0], load = c->x[1];
    HFiber *me = t_running;
    if (!me) {  // the thread's own context, switching for the first time
        me = calloc(1, sizeof *me);
        me->cpu = c;
    }
    me->key = c->sp - 0xf0;  // the frame DispatchFiberContext would have pushed
    *(uint64_t *)(uintptr_t)(me->key + FRAME_X30) = 0;  // (never taken for a new fiber's frame, see below)

    uint64_t key = __atomic_load_n((uint64_t *)(uintptr_t)load, __ATOMIC_ACQUIRE);
    HFiber *to = NULL;
    for (int spins = 0;; spins++) {
        pthread_mutex_lock(&g_lock);
        to = take_suspended(key);
        pthread_mutex_unlock(&g_lock);
        if (to) break;
        if (*(uint64_t *)(uintptr_t)(key + FRAME_X30)) { to = new_fiber(key, c); break; }  // a new fiber's frame
        // A context still being switched away from on another thread (its word was read before it was published).
        if (spins > 2000000) FATAL("fiber: context %llx never became resumable\n", (unsigned long long)key);
        sched_yield();
        key = __atomic_load_n((uint64_t *)(uintptr_t)load, __ATOMIC_ACQUIRE);
    }
    copy_thread_state(to->cpu, c);  // (it runs on this thread now)

    t_left = me;
    t_left_save = save;
    t_running = to;
    g_fiber_cpu = to->stack ? to->cpu : NULL;
    swapcontext(&me->uc, &to->uc);
    // Resumed (maybe on another thread): this context runs again with its own Cpu, as DispatchFiberContext returns.
    publish_left();
    t_running = me;
    g_fiber_cpu = me->stack ? me->cpu : NULL;
    me->cpu->host_fpcr = 1;  // (swapcontext restored the host FP state saved on whichever thread this context left)
    c->x[0] = 0;  // (DispatchFiberContext's saved x0 slot)
}

static uint64_t orig(const char *name, uint64_t *cache) {
    if (!*cache) *cache = loader_find_symbol(name);
    if (!*cache) FATAL("fiber: no SDK implementation of %s\n", name);
    return *cache;
}

#define CALL(sym)                             \
    do {                                      \
        static uint64_t a_;                   \
        uint64_t lr_ = c->x[30];              \
        guest_call(c, orig(sym, &a_));        \
        c->x[30] = lr_;                       \
    } while (0)

HLE_FUNC("_ZN2nn2os13FinalizeFiberEPNS0_9FiberTypeE", fiber_finalize) {
    drop_fiber_of(c->x[0]);
    CALL("_ZN2nn2os13FinalizeFiberEPNS0_9FiberTypeE");
}

RT_ON_RESET(fiber_reset) {
    g_suspended = NULL;  // (the old boot's contexts are gone with its threads; their memory stays allocated)
    t_running = NULL;
    t_left = NULL;
}
#endif
