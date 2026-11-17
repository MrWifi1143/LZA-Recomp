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

uint64_t g_stdheap_used;  // bytes of the heap window given to StandardAllocators from STDHEAP_BASE (exec.c: GPU import)
// The block's size: the Switch-like 2.75 GB, more where a game's settings need it (hle/za_fixes.c: Z-A above 1440p); at
// most what the heap window has above STDHEAP_BASE
uint64_t g_stdheap_size = STDHEAP_SIZE;
static uint64_t g_std_obj, g_std_size;  // the allocator given the block (heap_std_init*) and the block's size

// Initialize(NULL, size): the allocator reserves virtual address space and backs it on demand (nn::os virtual address
// memory). Shaders read the game's data through the imported heap window only, so it gets a fixed block there instead.
static void std_virtual_to_block(Cpu *c) {
    if (c->x[1] || ENV("LARECOMP_VIRTUAL_HEAP")) return;
    uint64_t cap = HEAP_BASE + HEAP_MAX - STDHEAP_BASE, total = g_stdheap_size < cap ? g_stdheap_size : cap;
    uint64_t want = c->x[2], left = total - g_stdheap_used;
    uint64_t size = want < left ? want : left;
    if (size < 0x100000) return;  // (no room: the virtual way)
    c->x[1] = STDHEAP_BASE + g_stdheap_used;
    c->x[2] = size;
    g_stdheap_used += (size + 0xFFFF) & ~0xFFFFull;
    extern void gpuheap_back(uint64_t addr, uint64_t size);
    gpuheap_back(c->x[1], size);  // (the heap window as GPU memory, where the driver can't import it: gpuheap.c)
    LOG("[heaps] standard allocator: %#llx bytes of virtual memory asked, a %#llx-byte block at %llx given\n",
        (unsigned long long)want, (unsigned long long)size, (unsigned long long)c->x[1]);
    g_std_obj = c->x[0], g_std_size = size;
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

// The SDK's own StandardAllocator queries, called on the allocator object (x0..x18 the callee's; x30 kept)
#define SYM_TOTALFREE "_ZNK2nn3mem17StandardAllocator16GetTotalFreeSizeEv"
#define SYM_ALLOCATABLE "_ZNK2nn3mem17StandardAllocator18GetAllocatableSizeEv"
#define SYM_FREE "_ZN2nn3mem17StandardAllocator4FreeEPv"
static uint64_t sdk_call(Cpu *c, const char *sym, uint64_t *cache, uint64_t obj) {
    if (!*cache) *cache = loader_find_symbol(sym);
    if (!*cache) return 0;
    uint64_t lr = c->x[30];
    c->x[0] = obj;
    guest_call(c, *cache);
    c->x[30] = lr;
    return c->x[0];
}
static uint64_t std_free_size(Cpu *c, uint64_t obj) { static uint64_t a; return sdk_call(c, SYM_TOTALFREE, &a, obj); }
static uint64_t std_largest(Cpu *c, uint64_t obj) { static uint64_t a; return sdk_call(c, SYM_ALLOCATABLE, &a, obj); }

// ---- How full the block is (research, as Arceus's): LARECOMP_LOG_HEAPUSE=1 logs the block's use every 5 s, with the
// largest piece left (a fragmented block fails big allocations with room to spare); LARECOMP_HEAP_BYLR=1 adds the
// outstanding bytes by the code that asked (two return addresses above the game's allocation wrapper f_70017957f0;
// slow). A failed allocation always logs the block's state.
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
        if (r == lr || (r >= 0x70017957f0ull && r < 0x7001795a00ull)) continue;  // (the allocation wrapper)
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
        uint64_t key = g_bylr[i].key, h = (key * 0x9E3779B97F4A7C15ull) >> 50;
        for (int k = 0; k < NT; k++, h = (h + 1) % NT)
            if (!t[h].n || t[h].key == key) { t[h].key = key, t[h].bytes += g_bylr[i].size, t[h].n++; break; }
    }
    pthread_mutex_unlock(&g_bylr_lock);
    for (int r = 0; r < 25; r++) {
        int b = -1;
        for (int k = 0; k < NT; k++) if (t[k].bytes && (b < 0 || t[k].bytes > t[b].bytes)) b = k;
        if (b < 0) break;
        LOG("[heaps]   %6.1f MB in %llu blocks from %llx <- %llx\n", t[b].bytes / 1048576.0, (unsigned long long)t[b].n,
            (unsigned long long)((t[b].key & 0xffffffffull) + GUEST_LO), (unsigned long long)((t[b].key >> 32) + GUEST_LO));
        t[b].bytes = 0;
    }
}
void rt_call_original(Cpu *c, uint64_t addr);
static int std_hook_free(Cpu *c, uint64_t addr) {
    (void)addr;
    if (g_bylr && c->x[0] == g_std_obj) {
        pthread_mutex_lock(&g_bylr_lock);
        bylr_del(c->x[1]);
        pthread_mutex_unlock(&g_bylr_lock);
    }
    return 0;
}
static void std_note(Cpu *c, uint64_t obj, uint64_t p, uint64_t size, uint64_t lr) {
    static int on = -1;
    if (on < 0) {
        on = ENV("LARECOMP_LOG_HEAPUSE") != NULL;
        if (ENV("LARECOMP_HEAP_BYLR") && (g_bylr = calloc(BYLR_N, sizeof *g_bylr))) {
            void rt_register_fnhook_at(uint64_t addr, int (*fn)(Cpu *c, uint64_t addr));
            on = 1;
            rt_register_fnhook_at(loader_find_symbol(SYM_FREE), std_hook_free);
        }
    }
    if (!on || obj != g_std_obj || !g_std_obj) return;
    if (g_bylr && p) {
        uint64_t key = caller_key(c, lr);
        pthread_mutex_lock(&g_bylr_lock);
        bylr_add(p, size, key);
        pthread_mutex_unlock(&g_bylr_lock);
    }
    static uint64_t last;
    uint64_t now = plat_now_ns(), l = __atomic_load_n(&last, __ATOMIC_RELAXED);
    if (now - l < 5000000000ull || !__atomic_compare_exchange_n(&last, &l, now, false, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) return;
    uint64_t x0 = c->x[0], used = g_std_size - std_free_size(c, obj), big = std_largest(c, obj);
    c->x[0] = x0;
    if (used > g_std_peak && used < g_std_size - 0x1000000) g_std_peak = used;  // (not the game's first probe: all of it)
    LOG("[heaps] standard allocator block: %llu MB used of %llu (peak %llu), largest free piece %llu KB\n",
        (unsigned long long)(used >> 20), (unsigned long long)(g_std_size >> 20), (unsigned long long)(g_std_peak >> 20),
        (unsigned long long)(big >> 10));
    if (g_bylr) bylr_report();
}

static void std_failed(Cpu *c, uint64_t obj, const char *what, uint64_t lr, uint64_t size) {
    static uint64_t seen[64];
    static int n;
    for (int i = 0; i < n; i++) if (seen[i] == lr) return;
    if (n < 64) seen[n++] = lr;
    LOG("[heaps] %s FAILED (size %#llx) from lr %llx\n", what, (unsigned long long)size, (unsigned long long)lr);
    if (obj != g_std_obj || !g_std_obj) return;
    uint64_t fr = std_free_size(c, obj), al = std_largest(c, obj);
    c->x[0] = 0;
    LOG("[heaps] the block: %llu MB of %llu in use, %llu MB free, %llu KB the largest piece\n",
        (unsigned long long)((g_std_size - fr) >> 20), (unsigned long long)(g_std_size >> 20), (unsigned long long)(fr >> 20),
        (unsigned long long)(al >> 10));
    if (g_bylr) bylr_report();
}

HLE_FUNC("_ZN2nn3mem17StandardAllocator8AllocateEm", heap_std_alloc) {
    uint64_t lr = c->x[30], size = c->x[1], obj = c->x[0];
    CALL("_ZN2nn3mem17StandardAllocator8AllocateEm");
    if (!c->x[0] && size) std_failed(c, obj, "standard allocation", lr, size);
    std_note(c, obj, c->x[0], size, lr);
}

HLE_FUNC("_ZN2nn3mem17StandardAllocator8AllocateEmm", heap_std_alloc_a) {
    uint64_t lr = c->x[30], size = c->x[1], obj = c->x[0];
    CALL("_ZN2nn3mem17StandardAllocator8AllocateEmm");
    if (!c->x[0] && size) std_failed(c, obj, "standard allocation (aligned)", lr, size);
    std_note(c, obj, c->x[0], size, lr);
}
