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
// Linux only for now (Windows: the vectored handler only runs with crash reports on; macOS has no Vulkan renderer).
#include "rt.h"

#if !defined(__linux__)
bool pagewatch_supported(void) { return false; }
bool pagewatch_fault(uint64_t addr) { (void)addr; return false; }
void pagewatch_prewrite(uint64_t addr, uint64_t n) { (void)addr, (void)n; }
uint32_t pagewatch_now(void) { return 0; }
bool pagewatch_written_since(uint64_t addr, uint64_t n, uint32_t since) { (void)addr, (void)n, (void)since; return true; }
void pagewatch_protect(uint64_t addr, uint64_t n) { (void)addr, (void)n; }
void pagewatch_reset(void) {}
#else
#include <stdatomic.h>
#include <unistd.h>
#include <sys/mman.h>

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

bool pagewatch_supported(void) { return sysconf(_SC_PAGESIZE) == (long)PAGE; }

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
        mprotect((void *)(uintptr_t)(addr & ~(PAGE - 1)), PAGE, PROT_READ | PROT_WRITE);
    }
    bool retry = ours || (atomic_load_explicit(&g_dirty[p >> 6], memory_order_relaxed) & bit);
    unlock();
    return retry;
}

void pagewatch_prewrite(uint64_t addr, uint64_t n) {
    if (!n) return;
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
    lock();
    for (uint64_t pa = lo; pa < hi; pa += PAGE) {
        uint64_t p = pidx(pa), bit = 1ull << (p & 63);
        atomic_fetch_or(&g_dirty[p >> 6], bit);
        atomic_fetch_or(&g_prot[p >> 6], bit);
    }
    mprotect((void *)(uintptr_t)lo, hi - lo, PROT_READ);
    unlock();
}

// Everything writable again, nothing tracked (reboot: guest memory is about to be replaced)
void pagewatch_reset(void) {
    lock();
    for (uint64_t i = 0; i < NPAGES / 64; i++) {
        uint64_t w = atomic_exchange(&g_prot[i], 0);
        for (int k = 0; w && k < 64; k++)
            if (w >> k & 1) mprotect((void *)(uintptr_t)(HEAP_BASE + (i * 64 + k) * PAGE), PAGE, PROT_READ | PROT_WRITE);
        atomic_store(&g_dirty[i], 0);
    }
    for (uint64_t i = 0; i < NPAGES; i++) atomic_store_explicit(&g_stamp[i], 0, memory_order_relaxed);
    atomic_store(&g_clock, 1);
    unlock();
}
#endif
