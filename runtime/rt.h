// Runtime-internal declarations.
#pragma once
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>

#include "cpu.h"
#include "platform.h"

#ifdef _WIN32
// (Windows: formatted first, then written in one go - platform/windows.c)
#ifdef __cplusplus
extern "C"
#endif
int rt_log(const char *fmt, ...);
#define LOG(...) rt_log(__VA_ARGS__)
#else
#define LOG(...) fprintf(stderr, __VA_ARGS__)
#endif
#define FATAL(...) (fprintf(stderr, "FATAL: " __VA_ARGS__), rt_fatal_trace())

// ---- address space layout (guest addresses == host addresses)
// macOS reserves 0x2_0000_0000-0x70_0000_0000, and guest addresses must fit in 39 bits, so the
// guest lives in the 64 GB window 0x70_0000_0000-0x80_0000_0000 claimed at startup (see main.c; reserved without
// backing, pages commit on first touch).
#define GUEST_LO     0x7000000000ull
#define GUEST_HI     0x8000000000ull
#define MAIN_BASE    0x7000000000ull
#define SDK_BASE     0x7008000000ull   // PLA: main is 67 MB, so it gets a 128 MB slot
#define SUBSDK0_BASE 0x700A000000ull
#define SUBSDK1_BASE 0x700C000000ull
#define HLE_BASE     0x7080000000ull   // fake addresses for host-implemented functions
#define HLE_SIZE     0x0000100000ull
#define TLS_BASE     0x7090000000ull
#define TLS_SIZE     0x0000100000ull
#define HLE_HEAP     0x7092000000ull   // guest-visible memory owned by host code
#define HLE_HEAP_SIZE 0x000E000000ull
// alias region (svcMapMemory targets, and nn::os virtual address memory: PLA's nninitStartup reserves 32 GB for
// its nn::mem::StandardAllocator there, backed page by page with svcMapPhysicalMemory)
#define ALIAS_BASE   0x7400000000ull
#define ALIAS_SIZE   0x0C00000000ull
#define HEAP_BASE    0x7240000000ull
#define HEAP_MAX     0x01B0000000ull   // 6.75 GB of address space (up to ALIAS_BASE); the game is told 3.25 GB, the
                                       // Switch's (HEAP_REGION_REPORTED); the StandardAllocator block may grow into it
#define HEAP_REGION_REPORTED 0x00D0000000ull
// The game's virtual-memory StandardAllocator (PLA: Initialize(NULL, 32 GB)) gets this fixed block of the heap window
// instead (runtime/hle/heaps.c): shader global memory (LDG/STG) reaches the heap window only (imported as a GPU buffer,
// pinned), and the allocator spreads its allocations over its whole reservation. Above the heap SetHeapSize gives.
#define STDHEAP_BASE (HEAP_BASE + 0x0010000000ull)

// Write tracking for heap pages host code keeps copies of (runtime/pagewatch.c)
bool pagewatch_supported(void);
bool pagewatch_fault(uint64_t addr);                 // crash handler: a store to a watched page (true: handled, retry)
void pagewatch_prewrite(uint64_t addr, uint64_t n);  // before host code writes guest memory without CPU stores
uint32_t pagewatch_now(void);  // the write clock: take it before pagewatch_protect, keep it with the copy
bool pagewatch_written_since(uint64_t addr, uint64_t n, uint32_t since);
void pagewatch_protect(uint64_t addr, uint64_t n);
void pagewatch_reset(void);
#define STDHEAP_SIZE 0x00B0000000ull     // 2.75 GB: the block's usual size (g_stdheap_size: hle/heaps.c)
extern uint64_t g_stdheap_size;

// ---- loader.c
typedef struct Module {
    const char *name;
    uint64_t base, size;
    uint64_t text_off, text_size, ro_off, ro_size, data_off, data_size;
    uint64_t dynamic, init, fini, init_array, init_array_sz, fini_array, fini_array_sz;
    uint64_t symtab, strtab, nsyms;
} Module;

extern Module g_modules[4];
extern int g_nmodules;
void loader_load(const char *exefs_dir);  // (again after a reboot: modules are reloaded into wiped memory)
uint64_t loader_find_symbol(const char *name);
const char *loader_symbol_at(uint64_t addr, uint64_t *offset);

// ---- dispatch.c
typedef void (*HostFn)(Cpu *c);
void dispatch_init(void);
GuestFn dispatch_lookup(uint64_t addr);
uint64_t hle_register(const char *name, HostFn fn);  // returns fake guest address
HostFn hle_lookup_impl(const char *name);
void hle_override(uint64_t guest_addr, HostFn fn, const char *name);
void rt_fatal_trace(void) __attribute__((noreturn));
void crash_handler_install(void);  // per thread (alternate signal stack)

// ---- kernel.c
typedef struct KThread KThread;
void kernel_init(void);
void kernel_stop_threads(void (*idle)(void));  // reboot: unwind every guest thread and join it (idle: called while waiting)
void kernel_reset(void);                       // reboot: empty handle table, memory map, thread list
KThread *kernel_create_main_thread(uint64_t entry, uint64_t stack_size, int prio);
void kernel_run_main(KThread *t, uint64_t x0, uint64_t x1, uint64_t x2, uint64_t x3, uint64_t x4);
// Emulated cores (kernel.c "cores"): host code that blocks a guest thread gives its core up around the wait; end
// only where no lock is held. kernel_core_reacquire: the lazy retake (dispatch).
void kernel_block_begin(void);
void kernel_block_end(void);
void kernel_core_reacquire(void);
uint32_t kernel_current_thread_handle(void);
Cpu *kernel_current_cpu(void);

// ---- ipc.c
typedef struct Service Service;
int ipc_handle_request(Cpu *c, uint32_t handle);
uint32_t ipc_connect_named_port(const char *name);

// ---- in-place reboot (main.c): switch seeds or go back to the start screen without leaving the process
enum { RT_REBOOT_NONE, RT_REBOOT_GAME, RT_REBOOT_TITLE };  // GAME: boot again at once (new seed); TITLE: start screen
void rt_request_reboot(int kind);  // any thread; the window loop (ui_run) returns it and main() carries it out
// Any thread: start this program again - at the launcher, or (to_launcher 0) straight into the game (--play) - and
// close this one (the in-game "Leave the game?" box: runtime/ui/menu.mm). A new process, not the in-place reboot:
// that one is Let's Go's and doesn't reset the Legends games' own state (heap blocks, pools, caches).
void rt_restart_process(int to_launcher);
int rt_reboot_requested(void);
extern volatile int g_rt_unwind;   // set while guest threads are being stopped (kernel.c)
void rt_unwind_point(void);        // guest threads: unwind to thread_main if a reboot is stopping them

// ---- ui (runtime/nvn/metal.m)
int ui_run(void);  // window loop; returns the reboot kind requested (the window stays open)

// ---- helpers
void guest_call(Cpu *c, uint64_t addr);   // call guest function at addr (saves/restores x30)
uint64_t guest_sym(const char *name);     // cached loader_find_symbol, fatal if missing
extern uint64_t g_program_id;  // the game's program (title) id, from main.npdm
extern uint64_t g_system_resource_size;  // main.npdm: the process's system resource (svcGetInfo 16)
void *hle_alloc(uint64_t size);           // zeroed guest-visible memory (never freed)
extern const char *g_romfs_dir;
