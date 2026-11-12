// Write tracking for guest heap pages that host code keeps copies of (runtime/nvn/vulkan/vcache.c: vertex data in VRAM).
// pagewatch_protect makes a range read-only; the game's next CPU store to one of its pages faults once (pagewatch_fault,
// from the crash handler: the page gets the write clock's next value and is writable again, the store retries). A copy
// keeps the clock it was made at (pagewatch_now, read before its pagewatch_protect) and pagewatch_written_since says
// whether its pages were written after. (Per copy, not a shared dirty bit: ranges of different sizes overlap the same
// pages as the game reuses memory - a dirty bit cleared by one range's refresh left the others drawing old vertices,
// Z-A's vertex explosions that grew with play time.) Host code that writes guest memory without a CPU store (read(2) straight into it, page
// decommits) calls pagewatch_prewrite first: the kernel would fail those with EFAULT instead of faulting. Bitmaps and
// page protection change together under one lock (a fault racing pagewatch_protect could otherwise leave a read-only
// page marked writable); it is held for a few bit operations and one mprotect, never by code that writes guest memory.
// Linux and Windows (Windows: VirtualProtect, the faults from the guest window's vectored handler, platform/windows.c);
// macOS has no Vulkan renderer.
#include "rt.h"

#if !defined(__linux__) && !defined(_WIN32)
bool pagewatch_supported(void) { return false; }
bool pagewatch_fault(uint64_t addr) { (void)addr; return false; }
void pagewatch_prewrite(uint64_t addr, uint64_t n) { (void)addr, (void)n; }
uint32_t pagewatch_now(void) { return 0; }
bool pagewatch_written_since(uint64_t addr, uint64_t n, uint32_t since) { (void)addr, (void)n, (void)since; return true; }
void pagewatch_protect(uint64_t addr, uint64_t n) { (void)addr, (void)n; }
void pagewatch_reset(void) {}
#else
#include <stdatomic.h>
#ifdef _WIN32
#include <windows.h>
// (a range the game hasn't touched all of yet is partly reserved, not committed: VirtualProtect fails for all of it)
static bool prot_ro(uint64_t lo, uint64_t n) { DWORD old; return VirtualProtect((void *)(uintptr_t)lo, n, PAGE_READONLY, &old) != 0; }
static void prot_rw(uint64_t lo, uint64_t n) { DWORD old; VirtualProtect((void *)(uintptr_t)lo, n, PAGE_READWRITE, &old); }
#else
#include <unistd.h>
#include <sys/mman.h>
static bool prot_ro(uint64_t lo, uint64_t n) { return mprotect((void *)(uintptr_t)lo, n, PROT_READ) == 0; }
static void prot_rw(uint64_t lo, uint64_t n) { mprotect((void *)(uintptr_t)lo, n, PROT_READ | PROT_WRITE); }
#endif

#define PAGE 4096ull
#define NPAGES (HEAP_MAX / PAGE)
static _Atomic uint64_t g_dirty[NPAGES / 64];  // ever tracked (a fault there retries)
static _Atomic uint32_t g_stamp[NPAGES];       // the write clock at its last tracked write
static _Atomic uint32_t g_clock = 1;
static _Atomic uint64_t g_prot[NPAGES / 64];   // read-only now (ours)
static atomic_flag g_lock = ATOMIC_FLAG_INIT;

static inline void cpu_relax(void) {
#if defined(__x86_64__)
    __builtin_ia32_pause();
#elif defined(__aarch64__)
    __asm__ volatile("yield");
#endif
}
static void lock(void) { while (atomic_flag_test_and_set_explicit(&g_lock, memory_order_acquire)) cpu_relax(); }
static void unlock(void) { atomic_flag_clear_explicit(&g_lock, memory_order_release); }
static inline bool in_heap(uint64_t a) { return a >= HEAP_BASE && a < HEAP_BASE + HEAP_MAX; }
static inline uint64_t pidx(uint64_t a) { return (a - HEAP_BASE) / PAGE; }

#ifdef _WIN32
bool pagewatch_supported(void) {
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    const char *v = getenv("LARECOMP_PAGEWATCH");  // (=0: off - every vertex range copied at each draw, as before)
    return si.dwPageSize == PAGE && !(v && atoi(v) == 0);
}
#else
bool pagewatch_supported(void) { return sysconf(_SC_PAGESIZE) == (long)PAGE; }
#endif

// A store to one of our read-only pages (signal handler; async-signal-safe): dirty, writable again. A thread that faulted
// on the same page while another thread unprotected it finds it dirty and no longer ours: it just retries.
bool pagewatch_fault(uint64_t addr) {
    if (!in_heap(addr)) return false;
    uint64_t p = pidx(addr), bit = 1ull << (p & 63);
    if (!((atomic_load_explicit(&g_prot[p >> 6], memory_order_acquire) | atomic_load_explicit(&g_dirty[p >> 6], memory_order_acquire)) & bit))
        return false;
    lock();
    bool ours = atomic_load_explicit(&g_prot[p >> 6], memory_order_relaxed) & bit;
    if (ours) {
        atomic_fetch_or(&g_dirty[p >> 6], bit);
        atomic_store_explicit(&g_stamp[p], atomic_fetch_add(&g_clock, 1) + 1, memory_order_release);
        atomic_fetch_and(&g_prot[p >> 6], ~bit);
        prot_rw(addr & ~(PAGE - 1), PAGE);
    }
    bool retry = ours || (atomic_load_explicit(&g_dirty[p >> 6], memory_order_relaxed) & bit);
    unlock();
    return retry;
}

void gpuheap_touch(uint64_t addr, uint64_t n);
void pagewatch_prewrite(uint64_t addr, uint64_t n) {
    if (!n) return;
#ifdef __linux__
    gpuheap_touch(addr, n);  // (an unbacked GPU-memory chunk: a syscall would fail with EFAULT there, not fault)
#endif
    for (uint64_t a = addr & ~(PAGE - 1); a < addr + n; a += PAGE) {
        if (!in_heap(a)) continue;
        uint64_t p = pidx(a);
        if (atomic_load_explicit(&g_prot[p >> 6], memory_order_acquire) & (1ull << (p & 63))) pagewatch_fault(a);
    }
}

uint32_t pagewatch_now(void) { return atomic_load_explicit(&g_clock, memory_order_acquire); }

bool pagewatch_written_since(uint64_t a, uint64_t n, uint32_t since) {
    for (uint64_t p = pidx(a), e = pidx(a + n - 1); p <= e; p++)
        if ((int32_t)(atomic_load_explicit(&g_stamp[p], memory_order_acquire) - since) > 0) return true;
    return false;
}

// Read-only (before the caller copies the range: a store after this faults, so the copy can't miss it)
void pagewatch_protect(uint64_t a, uint64_t n) {
    uint64_t lo = a & ~(PAGE - 1), hi = (a + n + PAGE - 1) & ~(PAGE - 1);
#ifdef __linux__
    gpuheap_touch(lo, hi - lo);  // (an unbacked GPU-memory chunk backed first: backing it maps it read-write, which would
    // drop the protection unseen, and gpuheap_fault leaves faults in backed chunks to pagewatch_fault)
#endif
    lock();
    for (uint64_t pa = lo; pa < hi; pa += PAGE) {
        uint64_t p = pidx(pa), bit = 1ull << (p & 63);
        atomic_fetch_or(&g_dirty[p >> 6], bit);
        atomic_fetch_or(&g_prot[p >> 6], bit);
    }
    if (!prot_ro(lo, hi - lo)) {
        // Not protected (part of it not committed yet): untracked, and counted as written now - the copy that follows
        // is never trusted, so the next draw copies again rather than keep vertices the game may have changed
        uint32_t now = atomic_fetch_add(&g_clock, 1) + 1;
        for (uint64_t pa = lo; pa < hi; pa += PAGE) {
            uint64_t p = pidx(pa), bit = 1ull << (p & 63);
            atomic_fetch_and(&g_prot[p >> 6], ~bit);
            atomic_store_explicit(&g_stamp[p], now, memory_order_release);
        }
    }
    unlock();
}

// Everything writable again, nothing tracked (reboot: guest memory is about to be replaced)
void pagewatch_reset(void) {
    lock();
    for (uint64_t i = 0; i < NPAGES / 64; i++) {
        uint64_t w = atomic_exchange(&g_prot[i], 0);
        for (int k = 0; w && k < 64; k++)
            if (w >> k & 1) prot_rw(HEAP_BASE + (i * 64 + k) * PAGE, PAGE);
        atomic_store(&g_dirty[i], 0);
    }
    for (uint64_t i = 0; i < NPAGES; i++) atomic_store_explicit(&g_stamp[i], 0, memory_order_relaxed);
    atomic_store(&g_clock, 1);
    unlock();
}
#endif
