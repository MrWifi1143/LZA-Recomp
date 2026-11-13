// Game heaps (engine limits). The game carves its memory into SDK heaps of fixed sizes: nn::mem::StandardAllocator
// (general), nn::lmem unit heaps (fixed-size blocks: object pools) and frame heaps. A heap that runs out returns
// NULL and the game crashes further on (bushsanity: ~450 actors around Link -> null weak_ptr in f_70010d0f00).
// These wrappers call the SDK's own implementation (loader_find_symbol), log heap creation (LARECOMP_LOG_HEAPS=1)
// and every allocation that fails (once per caller), and can grow heaps (heap_scale) - a recomp has memory to spare.
#include <pthread.h>
#ifdef _WIN32
#include <malloc.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../hle.h"
#include "../platform.h"

uint64_t loader_find_symbol(const char *name);
void guest_call(Cpu *c, uint64_t addr);

static uint64_t orig(const char *name, uint64_t *cache) {
    if (!*cache) *cache = loader_find_symbol(name);
    if (!*cache) FATAL("heaps: no SDK implementation of %s\n", name);
    return *cache;
}

static void fail_once(const char *what, uint64_t lr, uint64_t size) {
    static uint64_t seen[64];
    static int n;
    for (int i = 0; i < n; i++) if (seen[i] == lr) return;
    if (n < 64) seen[n++] = lr;
    LOG("[heaps] %s FAILED (size %#llx) from lr %llx\n", what, (unsigned long long)size, (unsigned long long)lr);
}

#define CALL(sym)                                    \
    do {                                             \
        static uint64_t a_;                          \
        uint64_t lr_ = c->x[30];                     \
        guest_call(c, orig(sym, &a_));               \
        c->x[30] = lr_;                              \
    } while (0)

// Unit heaps are fixed pools (e.g. 256 x 0x50-byte objects per loaded area: bushsanity's extra actors ran out at
// ~450 actors around Link). Every unit heap gets UNIT_SCALE times the units in a buffer of ours (the game's own
// buffer stays unused); freed again in DestroyUnitHeap. LARECOMP_UNIT_HEAP_SCALE overrides (1 = the game's sizes).
typedef struct { uint64_t head; void *mem; } UnitBuf;
static UnitBuf g_unit[256];
static pthread_mutex_t g_unit_lock = PTHREAD_MUTEX_INITIALIZER;

// Reboot: our unit-heap buffers (host memory, not in the guest window) go with the game that made them.
RT_ON_RESET(heaps_reset) {
    for (int i = 0; i < 256; i++)
        if (g_unit[i].mem) {
#ifdef _WIN32
            _aligned_free(g_unit[i].mem);
#else
            free(g_unit[i].mem);
#endif
            g_unit[i].mem = NULL;
        }
}

static int unit_scale(void) {
    static int s = -1;
    if (s < 0) { const char *e = ENV("LARECOMP_UNIT_HEAP_SCALE"); s = e ? atoi(e) : 4; if (s < 1) s = 1; }
    return s;
}

// nn::lmem::CreateUnitHeap(void *addr, size_t size, size_t unit, int align, int opt, HeapHead *head)
HLE_FUNC("_ZN2nn4lmem14CreateUnitHeapEPvmmiiPNS0_6detail8HeapHeadE", heap_create_unit) {
    uint64_t lr = c->x[30], size = c->x[1], unit = c->x[2];
    int k = unit_scale();
    void *mem = NULL;
    if (k > 1 && unit) {
        uint64_t big = size * (uint64_t)k;
#ifdef _WIN32
        mem = _aligned_malloc(big, 0x1000);
        if (mem) {
#else
        if (posix_memalign(&mem, 0x1000, big) == 0) {
#endif
            memset(mem, 0, big);
            c->x[0] = (uint64_t)(uintptr_t)mem;
            c->x[1] = big;
        } else {
            mem = NULL;
        }
    }
    uint64_t head = c->x[5];
    CALL("_ZN2nn4lmem14CreateUnitHeapEPvmmiiPNS0_6detail8HeapHeadE");
    if (mem) {
        pthread_mutex_lock(&g_unit_lock);
        for (int i = 0; i < 256; i++)
            if (!g_unit[i].mem) { g_unit[i].head = c->x[0] ? c->x[0] : head; g_unit[i].mem = mem; break; }
        pthread_mutex_unlock(&g_unit_lock);
    }
    if (ENV("LARECOMP_LOG_HEAPS"))
        LOG("[heaps] unit heap %#llx bytes, unit %#llx (%llu units, x%d) from lr %llx -> %llx\n", (unsigned long long)size,
            (unsigned long long)unit, unit ? (unsigned long long)(size / unit) : 0ull, mem ? k : 1, (unsigned long long)lr,
            (unsigned long long)c->x[0]);
}

HLE_FUNC("_ZN2nn4lmem15DestroyUnitHeapEPNS0_6detail8HeapHeadE", heap_destroy_unit) {
    uint64_t head = c->x[0];
    CALL("_ZN2nn4lmem15DestroyUnitHeapEPNS0_6detail8HeapHeadE");
    pthread_mutex_lock(&g_unit_lock);
    for (int i = 0; i < 256; i++)
        if (g_unit[i].mem && g_unit[i].head == head) {
#ifdef _WIN32
            _aligned_free(g_unit[i].mem);
#else
            free(g_unit[i].mem);
#endif
            g_unit[i].mem = NULL;
            break;
        }
    pthread_mutex_unlock(&g_unit_lock);
}

HLE_FUNC("_ZN2nn4lmem20AllocateFromUnitHeapEPNS0_6detail8HeapHeadE", heap_alloc_unit) {
    uint64_t lr = c->x[30], head = c->x[0];
    CALL("_ZN2nn4lmem20AllocateFromUnitHeapEPNS0_6detail8HeapHeadE");
    if (!c->x[0]) fail_once("unit heap allocation", lr, head);
}

// nn::lmem::CreateFrameHeap(void *addr, size_t size, int opt)
HLE_FUNC("_ZN2nn4lmem15CreateFrameHeapEPvmi", heap_create_frame) {
    uint64_t lr = c->x[30], size = c->x[1];
    CALL("_ZN2nn4lmem15CreateFrameHeapEPvmi");
    if (ENV("LARECOMP_LOG_HEAPS"))
        LOG("[heaps] frame heap %#llx bytes from lr %llx\n", (unsigned long long)size, (unsigned long long)lr);
}

HLE_FUNC("_ZN2nn4lmem21AllocateFromFrameHeapEPNS0_6detail8HeapHeadEmi", heap_alloc_frame) {
    uint64_t lr = c->x[30], size = c->x[1];
    CALL("_ZN2nn4lmem21AllocateFromFrameHeapEPNS0_6detail8HeapHeadEmi");
    if (!c->x[0]) fail_once("frame heap allocation", lr, size);
}

// ---- The game's StandardAllocator block
uint64_t g_stdheap_used;  // bytes of the heap window given to StandardAllocators from STDHEAP_BASE (exec.c: GPU import)
// The block's size: what the heap window has above STDHEAP_BASE (rt.h; hle/pla_cmdpool.c adds what its bigger GPU
// memory pools take, so the sum never falls short of it)
uint64_t g_stdheap_size = STDHEAP_SIZE;
static uint64_t g_std_obj, g_std_size;  // the allocator given the block (heap_std_init*) and the block's size

// Initialize(NULL, size): the allocator reserves virtual address space and backs it on demand (nn::os virtual address
// memory). Shaders read the game's data through the imported heap window only, so it gets a fixed block there instead.
static bool std_virtual_to_block(Cpu *c) {
    if (c->x[1] || ENV("LARECOMP_VIRTUAL_HEAP")) return false;
    uint64_t cap = HEAP_BASE + HEAP_MAX - STDHEAP_BASE, total = g_stdheap_size < cap ? g_stdheap_size : cap;
    const char *lim = ENV("LARECOMP_STDHEAP_MB");  // (test: a smaller block - a full one sooner, as with heavy mods)
    if (lim && (uint64_t)atoll(lim) << 20 < total) total = (uint64_t)atoll(lim) << 20;
    uint64_t want = c->x[2], left = total - g_stdheap_used;
    uint64_t size = want < left ? want : left;
    if (size < 0x100000) return false;  // (no room: the virtual way)
    c->x[1] = STDHEAP_BASE + g_stdheap_used;
    c->x[2] = size;
    g_stdheap_used += (size + 0xFFFF) & ~0xFFFFull;
    extern void gpuheap_back(uint64_t addr, uint64_t size);
    gpuheap_back(c->x[1], size);  // (the heap window as GPU memory, where the driver can't import it: gpuheap.c)
    LOG("[heaps] standard allocator: %#llx bytes of virtual memory asked, a %#llx-byte block at %llx given\n",
        (unsigned long long)want, (unsigned long long)size, (unsigned long long)c->x[1]);
    g_std_obj = c->x[0], g_std_size = size;
    return true;
}

// nn::mem::StandardAllocator::Initialize(void *addr, size_t size[, bool])
HLE_FUNC("_ZN2nn3mem17StandardAllocator10InitializeEPvm", heap_std_init) {
    std_virtual_to_block(c);
    uint64_t lr = c->x[30], size = c->x[2];
    CALL("_ZN2nn3mem17StandardAllocator10InitializeEPvm");
    if (ENV("LARECOMP_LOG_HEAPS"))
        LOG("[heaps] standard allocator %#llx bytes from lr %llx\n", (unsigned long long)size, (unsigned long long)lr);
}

HLE_FUNC("_ZN2nn3mem17StandardAllocator10InitializeEPvmb", heap_std_init_b) {
    std_virtual_to_block(c);
    uint64_t lr = c->x[30], size = c->x[2];
    if (ENV("LARECOMP_LOG_HEAPS"))
        LOG("[heaps] standard allocator init addr %#llx size %#llx cache %d from lr %llx\n", (unsigned long long)c->x[1],
            (unsigned long long)size, (int)(c->x[3] & 1), (unsigned long long)lr);
    CALL("_ZN2nn3mem17StandardAllocator10InitializeEPvmb");
    if (ENV("LARECOMP_LOG_HEAPS"))
        LOG("[heaps] standard allocator %#llx bytes (b) from lr %llx\n", (unsigned long long)size, (unsigned long long)lr);
}

// The SDK's own StandardAllocator functions (not the imports bound above), called on an allocator object
#define SYM_ALLOC  "_ZN2nn3mem17StandardAllocator8AllocateEmm"
#define SYM_FREE   "_ZN2nn3mem17StandardAllocator4FreeEPv"
#define SYM_REALLOC "_ZN2nn3mem17StandardAllocator10ReallocateEPvm"
#define SYM_SIZEOF "_ZNK2nn3mem17StandardAllocator9GetSizeOfEPKv"
#define SYM_TOTALFREE "_ZNK2nn3mem17StandardAllocator16GetTotalFreeSizeEv"
#define SYM_ALLOCATABLE "_ZNK2nn3mem17StandardAllocator18GetAllocatableSizeEv"
#define SYM_CTOR   "_ZN2nn3mem17StandardAllocatorC1Ev"
#define SYM_INIT   "_ZN2nn3mem17StandardAllocator10InitializeEPvm"
static uint64_t sdk_fn(const char *sym) {
    static struct { const char *s; uint64_t a; } t[8];
    static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
    for (int i = 0; i < 8; i++) {
        const char *s = __atomic_load_n(&t[i].s, __ATOMIC_ACQUIRE);
        if (s == sym) return t[i].a;
        if (!s) break;
    }
    pthread_mutex_lock(&lock);
    uint64_t a = loader_find_symbol(sym);
    for (int i = 0; i < 8; i++)
        if (!t[i].s || t[i].s == sym) { t[i].a = a; __atomic_store_n(&t[i].s, sym, __ATOMIC_RELEASE); break; }
    pthread_mutex_unlock(&lock);
    return a;
}
// f(obj, a1, a2) on this thread's guest state, as a call from where the hook is: x0..x18 are the callee's to clobber
// (the hook's own results are set after); x30 kept
static uint64_t sdk_call(Cpu *c, const char *sym, uint64_t obj, uint64_t a1, uint64_t a2) {
    uint64_t a = sdk_fn(sym), lr = c->x[30];
    if (!a) return 0;
    c->x[0] = obj, c->x[1] = a1, c->x[2] = a2;
    guest_call(c, a);
    c->x[30] = lr;
    return c->x[0];
}

// ---- Spill: the block full, allocations go on beyond the heap window instead of failing
// The game asks for 32 GB and lives with what it gets: a full block returns NULL and the caller crashes (draw distance
// mods + HD texture romfs mods + more wild Pokemon: StandardAllocator FAILED (0x1000) from lr 700084c60c, then null+0x30
// in f_70031bd5d8 at a field load). With the block full, a second StandardAllocator of ours over STDHEAP_SPILL (rt.h:
// guest address space past the heap window, nothing else there) takes the allocation; Free / Reallocate / GetSizeOf of
// its blocks are routed to it (hooks on the SDK's functions). The window keeps everything it held, and the main block
// takes allocations again as soon as it has room. Spilled memory is CPU memory: vertex / index / uniform data there is
// copied per draw (exec.c guest_direct), and shader global memory (LDG/STG, gmem) can't reach it (reads 0, as text
// glyphs did in the alias region before the block) - a glitch at worst, where the game crashed. Pages are committed per
// allocation (Windows: the OS's own file reads into them must not fault). LARECOMP_HEAP_SPILL=0: off (the game's NULL).
static uint64_t g_spill_obj;  // guest StandardAllocator object (HLE heap), 0 until the block first runs out
static pthread_mutex_t g_spill_lock = PTHREAD_MUTEX_INITIALIZER;
static uint64_t g_spill_n, g_spill_bytes;  // allocations spilled, bytes asked (log)
static inline bool in_spill(uint64_t p) { return p >= STDHEAP_SPILL_BASE && p < STDHEAP_SPILL_BASE + STDHEAP_SPILL_SIZE; }

void rt_call_original(Cpu *c, uint64_t addr);
static int std_hook_free(Cpu *c, uint64_t addr);
static int std_hook_realloc(Cpu *c, uint64_t addr);
static int std_hook_sizeof(Cpu *c, uint64_t addr);
static void std_hooks_on(void) {  // (once: the SDK functions that take a block from the game, routed per pointer)
    static int on;
    if (__atomic_load_n(&on, __ATOMIC_ACQUIRE)) return;
    void rt_register_fnhook_at(uint64_t addr, int (*fn)(Cpu *c, uint64_t addr));
    rt_register_fnhook_at(sdk_fn(SYM_FREE), std_hook_free);
    rt_register_fnhook_at(sdk_fn(SYM_REALLOC), std_hook_realloc);
    rt_register_fnhook_at(sdk_fn(SYM_SIZEOF), std_hook_sizeof);
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    __atomic_store_n(&on, 1, __ATOMIC_RELEASE);
}

static uint64_t spill_alloc(Cpu *c, uint64_t size, uint64_t align) {
    static int off = -1;
    if (off < 0) { const char *e = ENV("LARECOMP_HEAP_SPILL"); off = e && *e == '0'; }
    if (off || !g_std_obj) return 0;
    if (!__atomic_load_n(&g_spill_obj, __ATOMIC_ACQUIRE)) {
        pthread_mutex_lock(&g_spill_lock);
        if (!g_spill_obj) {
            uint64_t obj = (uint64_t)(uintptr_t)hle_alloc(0x1000);  // (the object: well over the SDK's size)
            std_hooks_on();  // (before any spilled block exists)
            sdk_call(c, SYM_CTOR, obj, 0, 0);
            sdk_call(c, SYM_INIT, obj, STDHEAP_SPILL_BASE, STDHEAP_SPILL_SIZE);
            LOG("[heaps] the game's memory block (%llu MB) is full: allocations go on in %llu MB of CPU memory past it "
                "(at %llx; a mod-heavy setup - draw distance / HD texture / more Pokemon mods)\n",
                (unsigned long long)(g_std_size >> 20), (unsigned long long)(STDHEAP_SPILL_SIZE >> 20),
                (unsigned long long)STDHEAP_SPILL_BASE);
            __atomic_store_n(&g_spill_obj, obj, __ATOMIC_RELEASE);
        }
        pthread_mutex_unlock(&g_spill_lock);
    }
    uint64_t p = sdk_call(c, SYM_ALLOC, g_spill_obj, size, align < 16 ? 16 : align);
    if (!p) return 0;
    plat_commit(p, size);
    uint64_t n = __atomic_add_fetch(&g_spill_n, 1, __ATOMIC_RELAXED), b = __atomic_add_fetch(&g_spill_bytes, size, __ATOMIC_RELAXED);
    if ((n & (n - 1)) == 0 && n >= 1024) LOG("[heaps] spilled: %llu allocations so far, %llu MB asked\n", (unsigned long long)n, (unsigned long long)(b >> 20));
    return p;
}

// ---- How full the block is (research: LARECOMP_LOG_HEAPUSE=1 every 5 s; LARECOMP_HEAP_BYLR=1 adds the outstanding
// bytes by the code that asked: the two callers above the game's allocator wrappers f_700084c5b8 / ..c94c; slow - the
// field load takes a minute longer). Used =
// block - GetTotalFreeSize. 2026-11-12 (a reporter's save, Lush + Max Draw Distance mod, Max): ~3.1 of 3.5 GB - the
// GPU memory pools 800 + 288 + 280 MB (hle/pla_cmdpool.c), then 377 MB from 7002973c18, 314 MB 700083a570, ...
uint64_t plat_now_ns(void);
static uint64_t g_std_peak;
#define BYLR_N (1u << 21)
static struct BylrEnt { uint64_t p, size, key; } *g_bylr;
static pthread_mutex_t g_bylr_lock = PTHREAD_MUTEX_INITIALIZER;
static uint64_t caller_key(Cpu *c, uint64_t lr) {
    uint64_t fp = c->x[29], k[2] = {0, 0};
    int n = 0;
    for (int d = 0; d < 8 && n < 2; d++) {
        if (fp < GUEST_LO || fp >= GUEST_HI || (fp & 7)) break;
        uint64_t r = *(const uint64_t *)(uintptr_t)(fp + 8);
        fp = *(const uint64_t *)(uintptr_t)fp;
        if (r == lr || (r >= 0x700084c5b8ull && r < 0x700084c9a0ull)) continue;  // (the allocator wrappers)
        k[n++] = r;
    }
    return ((k[0] - GUEST_LO) & 0xffffffffull) | ((k[1] - GUEST_LO) & 0xffffffffull) << 32;
}
static void bylr_add(uint64_t p, uint64_t size, uint64_t key) {
    for (uint64_t i = (p >> 4) % BYLR_N, k = 0; k < BYLR_N; k++, i = (i + 1) % BYLR_N)
        if (!g_bylr[i].p) { g_bylr[i] = (struct BylrEnt){p, size, key}; return; }
}
static void bylr_del(uint64_t p) {
    for (uint64_t i = (p >> 4) % BYLR_N, k = 0; p && k < BYLR_N && g_bylr[i].p; k++, i = (i + 1) % BYLR_N)
        if (g_bylr[i].p == p) {
            g_bylr[i].p = 0;
            for (uint64_t j = (i + 1) % BYLR_N; g_bylr[j].p; j = (j + 1) % BYLR_N) {
                struct BylrEnt e = g_bylr[j];
                g_bylr[j].p = 0;
                for (uint64_t t = (e.p >> 4) % BYLR_N;; t = (t + 1) % BYLR_N) if (!g_bylr[t].p) { g_bylr[t] = e; break; }
            }
            return;
        }
}
static void bylr_report(void) {
    enum { NT = 16384 };
    static struct { uint64_t key, bytes, n; } t[NT];
    memset(t, 0, sizeof t);
    pthread_mutex_lock(&g_bylr_lock);
    for (uint64_t i = 0; i < BYLR_N; i++) {
        if (!g_bylr[i].p) continue;
        uint64_t key = g_bylr[i].key, h = (key * 0x9E3779B97F4A7C15ull) >> 50;  // (14 bits)
        for (int k = 0; k < NT; k++, h = (h + 1) % NT)
            if (!t[h].n || t[h].key == key) { t[h].key = key, t[h].bytes += g_bylr[i].size, t[h].n++; break; }
    }
    pthread_mutex_unlock(&g_bylr_lock);
    for (int r = 0; r < 25; r++) {  // (the biggest 25)
        int b = -1;
        for (int k = 0; k < NT; k++) if (t[k].bytes && (b < 0 || t[k].bytes > t[b].bytes)) b = k;
        if (b < 0) break;
        LOG("[heaps]   %6.1f MB in %llu blocks from %llx <- %llx\n", t[b].bytes / 1048576.0, (unsigned long long)t[b].n,
            (unsigned long long)((t[b].key & 0xffffffffull) + GUEST_LO), (unsigned long long)((t[b].key >> 32) + GUEST_LO));
        t[b].bytes = 0;
    }
}

static void std_note(Cpu *c, uint64_t obj, uint64_t p, uint64_t size, uint64_t lr) {
    static int on = -1;
    if (on < 0) {
        on = ENV("LARECOMP_LOG_HEAPUSE") != NULL;
        if (ENV("LARECOMP_HEAP_BYLR") && (g_bylr = calloc(BYLR_N, sizeof *g_bylr))) on = 1, std_hooks_on();
    }
    if (!on || obj != g_std_obj) return;
    if (g_bylr && p) {
        uint64_t key = caller_key(c, lr);
        pthread_mutex_lock(&g_bylr_lock);
        bylr_add(p, size, key);
        pthread_mutex_unlock(&g_bylr_lock);
    }
    static uint64_t last;
    uint64_t now = plat_now_ns(), l = __atomic_load_n(&last, __ATOMIC_RELAXED);
    if (now - l < 5000000000ull || !__atomic_compare_exchange_n(&last, &l, now, false, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) return;
    uint64_t x0 = c->x[0], used = g_std_size - sdk_call(c, SYM_TOTALFREE, obj, 0, 0), spill = 0;
    uint64_t so = __atomic_load_n(&g_spill_obj, __ATOMIC_ACQUIRE);
    if (so) spill = STDHEAP_SPILL_SIZE - sdk_call(c, SYM_TOTALFREE, so, 0, 0);
    c->x[0] = x0;
    if (used > g_std_peak && used < g_std_size - 0x1000000) g_std_peak = used;  // (not the game's first probe: all of it)
    LOG("[heaps] standard allocator block: %llu MB used of %llu (peak %llu); spilled past it: %llu MB in use\n",
        (unsigned long long)(used >> 20), (unsigned long long)(g_std_size >> 20), (unsigned long long)(g_std_peak >> 20),
        (unsigned long long)(spill >> 20));
    if (g_bylr) bylr_report();
}

// Free / Reallocate / GetSizeOf (the SDK's: every caller): a spilled block goes to the spill allocator
static int std_hook_free(Cpu *c, uint64_t addr) {
    uint64_t p = c->x[1];
    if (g_bylr && c->x[0] == g_std_obj) {
        pthread_mutex_lock(&g_bylr_lock);
        bylr_del(p);
        pthread_mutex_unlock(&g_bylr_lock);
    }
    if (!in_spill(p) || !g_spill_obj || c->x[0] == g_spill_obj) return 0;  // (the game's own: as it is)
    c->x[0] = g_spill_obj;
    rt_call_original(c, addr);
    return 1;
}
static int std_hook_sizeof(Cpu *c, uint64_t addr) {
    if (!in_spill(c->x[1]) || !g_spill_obj || c->x[0] == g_spill_obj) return 0;
    c->x[0] = g_spill_obj;
    rt_call_original(c, addr);
    return 1;
}
static int std_hook_realloc(Cpu *c, uint64_t addr) {
    uint64_t obj = c->x[0], old = c->x[1], size = c->x[2], lr = c->x[30];
    if (obj == g_spill_obj && g_spill_obj) return 0;
    if (in_spill(old) && g_spill_obj) {  // (a spilled block grows / shrinks where it is)
        c->x[0] = g_spill_obj;
        rt_call_original(c, addr);
        if (c->x[0]) plat_commit(c->x[0], size);
        return 1;
    }
    rt_call_original(c, addr);
    uint64_t r = c->x[0];
    if (!r && size && obj == g_std_obj) {  // the block is full: a new block past it, the old one copied and freed
        uint64_t old_size = old ? sdk_call(c, SYM_SIZEOF, obj, old, 0) : 0;
        r = spill_alloc(c, size, 16);
        if (r && old) {
            memcpy((void *)(uintptr_t)r, (const void *)(uintptr_t)old, old_size < size ? old_size : size);
            sdk_call(c, SYM_FREE, obj, old, 0);
        }
        if (!r) fail_once("standard reallocation", lr, size);
    }
    if (g_bylr && r && obj == g_std_obj) {
        uint64_t key = caller_key(c, lr);
        pthread_mutex_lock(&g_bylr_lock);
        if (old) bylr_del(old);
        if (!in_spill(r)) bylr_add(r, size, key);
        pthread_mutex_unlock(&g_bylr_lock);
    }
    c->x[0] = r;
    return 1;
}

// An allocation the block couldn't serve: spilled, else logged (once per caller) with how full the block is
static uint64_t std_failed(Cpu *c, uint64_t obj, const char *what, uint64_t lr, uint64_t size, uint64_t align) {
    if (obj != g_std_obj) { fail_once(what, lr, size); return 0; }
    uint64_t p = spill_alloc(c, size, align);
    if (p) return p;
    static uint64_t seen[64];
    static int n;
    for (int i = 0; i < n; i++) if (seen[i] == lr) return 0;
    if (n < 64) seen[n++] = lr;
    fail_once(what, lr, size);
    uint64_t fr = sdk_call(c, SYM_TOTALFREE, obj, 0, 0), al = sdk_call(c, SYM_ALLOCATABLE, obj, 0, 0);
    LOG("[heaps] the block: %llu MB of %llu in use, %llu MB free, %llu KB the largest piece; %llu MB spilled\n",
        (unsigned long long)((g_std_size - fr) >> 20), (unsigned long long)(g_std_size >> 20), (unsigned long long)(fr >> 20),
        (unsigned long long)(al >> 10), (unsigned long long)(g_spill_bytes >> 20));
    if (g_bylr) bylr_report();
    return 0;
}

HLE_FUNC("_ZN2nn3mem17StandardAllocator8AllocateEm", heap_std_alloc) {
    uint64_t lr = c->x[30], size = c->x[1], obj = c->x[0];
    CALL("_ZN2nn3mem17StandardAllocator8AllocateEm");
    if (!c->x[0] && size) c->x[0] = std_failed(c, obj, "standard allocation", lr, size, 16);
    std_note(c, obj, c->x[0], size, lr);
}

HLE_FUNC("_ZN2nn3mem17StandardAllocator8AllocateEmm", heap_std_alloc_a) {
    uint64_t lr = c->x[30], size = c->x[1], obj = c->x[0], align = c->x[2];
    CALL("_ZN2nn3mem17StandardAllocator8AllocateEmm");
    if (!c->x[0] && size) c->x[0] = std_failed(c, obj, "standard allocation (aligned)", lr, size, align);
    std_note(c, obj, c->x[0], size, lr);
}

// Reboot: the spill allocator's object was HLE heap (wiped), its memory guest memory (wiped)
RT_ON_RESET(heaps_std_reset) {
    g_spill_obj = 0, g_spill_n = g_spill_bytes = 0;
    g_std_obj = 0, g_std_size = 0, g_std_peak = 0, g_stdheap_used = 0;
}
