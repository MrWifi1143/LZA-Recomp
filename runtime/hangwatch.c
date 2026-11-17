// Hang watch: a thread of ours that notices when no frame has been shown for a while although the process lives on (a
// freeze with the music still playing, nothing in the log), and logs once what everything waits on - "[hang]" lines:
// the render thread's job, the GPU timeline and who waits on it, the swapchain's acquire / present, every guest thread
// (the svc it is in, the mutex / condition variable / address / handles it waits on, its guest call stack, where host
// code blocked it, its last host call) and the presenting thread's last host calls. Again 60 s later if still stuck;
// a stall that ends (a long load) says so. Read-only and lock-free: it never takes a lock the game or the render thread
// holds, so it can't add to a hang. Armed at each boot's first present (the start screen, the setup and a reboot don't
// present game frames), quiet while the game is paused in the background. Last, the host call stacks of the render
// thread and the shader workers (stacks_report: each stopped for a moment - the one step that touches other threads).
// LARECOMP_HANG_WATCH=0: off; =<seconds>: after how long (default 10). LARECOMP_TEST_HANG=<frame>[,render] (research,
// renderthread.c): the game / render thread stops for good at that frame - the watch's test.
#define _GNU_SOURCE 1  // (dladdr)
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#include <tlhelp32.h>
#else
#include <dlfcn.h>
#include <execinfo.h>
#include <signal.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

#include "hle.h"
#include "kernel.h"
#include "paths.h"

uint64_t renderer_present_count(void);         // nvn/vulkan/device.c: frames shown
uint64_t renderer_game_frame(void);            // nvn/vulkan/renderthread.c: presents the game made
uint64_t vk_last_present_ns(void);             // device.c: when the last frame was shown
void vk_swapchain_hang_report(const char *tag);  // device.c
void vk_hang_report(const char *tag);          // nvn/vulkan/exec.c
void rt_hang_report(const char *tag);          // renderthread.c
void *rt_present_thread(void);                 // renderthread.c: the KThread that presents
void kernel_thread_dump(const char *tag, bool detail);  // kernel.c

volatile int g_hang_paused;  // (nvn/objects.c: the game waits in the background on purpose - Settings.background)

#define TAG "[hang] "
static uint64_t g_limit_ns = 10000000000ull;
static volatile int g_armed;            // a frame of this boot was shown
static volatile uint64_t g_boot_frames;  // frames shown when this boot started (armed once the count moves)

static uintptr_t exe_base(void) {
#ifdef _WIN32
    return (uintptr_t)GetModuleHandleA(NULL);
#else
    Dl_info di;
    return dladdr((void *)exe_base, &di) ? (uintptr_t)di.dli_fbase : 0;
#endif
}

// ---- host call stacks of our own threads (the render thread, the shader workers): which function a stuck one is in.
// A thread registers itself (hangwatch_thread); the report then stops each for a moment and walks its stack - Windows:
// SuspendThread + GetThreadContext, unwound with the modules' own unwind tables (.pdata, looked up here: no loader
// lock while a thread is stopped); Linux: a signal, the thread's backtrace() in its handler. Frames: module+offset
// (ZARecomp.exe+<offset> / lasrecomp+<offset>: the build's map file or addr2line names it), the exported name where
// one starts there, the game code as f_<guest address>.
#define HW_MAXTHREADS 48
#define HW_FRAMES 40
typedef struct {
    char name[32];
#ifdef _WIN32
    HANDLE h;
    DWORD tid;
#else
    pthread_t th;
    long tid;
#endif
} HwThread;
static HwThread g_threads[HW_MAXTHREADS];
static int g_nthreads;
static pthread_mutex_t g_threads_lock = PTHREAD_MUTEX_INITIALIZER;  // (registration only: the report reads g_nthreads)

void hangwatch_thread(const char *name, int index) {
    pthread_mutex_lock(&g_threads_lock);
    if (g_nthreads < HW_MAXTHREADS) {
        HwThread *t = &g_threads[g_nthreads];
        if (index >= 0) snprintf(t->name, sizeof t->name, "%s %d", name, index);
        else snprintf(t->name, sizeof t->name, "%s", name);
#ifdef _WIN32
        t->tid = GetCurrentThreadId();
        if (DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &t->h,
                            THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, 0))
            __atomic_store_n(&g_nthreads, g_nthreads + 1, __ATOMIC_RELEASE);
#else
        t->th = pthread_self();
        t->tid = (long)syscall(SYS_gettid);
        __atomic_store_n(&g_nthreads, g_nthreads + 1, __ATOMIC_RELEASE);
#endif
    }
    pthread_mutex_unlock(&g_threads_lock);
}

const char *rt_host_fn_name(uint64_t pc, uint64_t *offset);  // runtime/dispatch.c: the game code's functions

#ifdef _WIN32
typedef struct { uint64_t base, end; char name[48]; } HwMod;
static HwMod g_mods[512];
static int g_nmods;
static void mods_scan(void) {  // (before any thread is stopped: the snapshot takes the loader lock)
    g_nmods = 0;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId());
    if (snap == INVALID_HANDLE_VALUE) return;
    MODULEENTRY32 me = {.dwSize = sizeof me};
    for (BOOL ok = Module32First(snap, &me); ok && g_nmods < 512; ok = Module32Next(snap, &me)) {
        HwMod *m = &g_mods[g_nmods++];
        m->base = (uint64_t)(uintptr_t)me.modBaseAddr, m->end = m->base + me.modBaseSize;
        snprintf(m->name, sizeof m->name, "%s", me.szModule);
    }
    CloseHandle(snap);
}
static const HwMod *mod_of(uint64_t pc) {
    for (int i = 0; i < g_nmods; i++) if (pc - g_mods[i].base < g_mods[i].end - g_mods[i].base) return &g_mods[i];
    return NULL;
}
static const IMAGE_DATA_DIRECTORY *pe_dir(uint64_t base, int which) {
    const IMAGE_DOS_HEADER *d = (const IMAGE_DOS_HEADER *)(uintptr_t)base;
    if (d->e_magic != IMAGE_DOS_SIGNATURE) return NULL;
    const IMAGE_NT_HEADERS64 *nt = (const IMAGE_NT_HEADERS64 *)(uintptr_t)(base + (uint64_t)d->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE || nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) return NULL;
    if (nt->OptionalHeader.NumberOfRvaAndSizes <= (DWORD)which) return NULL;
    const IMAGE_DATA_DIRECTORY *dd = &nt->OptionalHeader.DataDirectory[which];
    return dd->VirtualAddress && dd->Size ? dd : NULL;
}
// The function containing pc, from its module's .pdata (sorted by address): what RtlLookupFunctionEntry finds, without
// its locks
static RUNTIME_FUNCTION *func_entry(uint64_t pc, uint64_t *base) {
    const HwMod *m = mod_of(pc);
    if (!m) return NULL;
    const IMAGE_DATA_DIRECTORY *dd = pe_dir(m->base, IMAGE_DIRECTORY_ENTRY_EXCEPTION);
    if (!dd) return NULL;
    RUNTIME_FUNCTION *rf = (RUNTIME_FUNCTION *)(uintptr_t)(m->base + dd->VirtualAddress);
    uint32_t lo = 0, hi = dd->Size / sizeof *rf, rva = (uint32_t)(pc - m->base);
    while (lo < hi) {
        uint32_t mid = (lo + hi) / 2;
        if (rva < rf[mid].BeginAddress) hi = mid;
        else if (rva >= rf[mid].EndAddress) lo = mid + 1;
        else {
            RUNTIME_FUNCTION *f = &rf[mid];
            if (f->UnwindData & 1) f = (RUNTIME_FUNCTION *)(uintptr_t)(m->base + (f->UnwindData & ~3u));  // (indirect entry)
            *base = m->base;
            return f;
        }
    }
    return NULL;
}
static bool readable(uint64_t a) {
    MEMORY_BASIC_INFORMATION mi;
    return VirtualQuery((void *)(uintptr_t)a, &mi, sizeof mi) && mi.State == MEM_COMMIT &&
           !(mi.Protect & (PAGE_NOACCESS | PAGE_GUARD));
}
// pcs[i] and the start of each one's function (0: none known). -1: the thread could not be stopped.
static int thread_stack(HwThread *t, uint64_t *pcs, uint64_t *starts, int max) {
    if (SuspendThread(t->h) == (DWORD)-1) return -1;
    CONTEXT ctx;
    memset(&ctx, 0, sizeof ctx);
    ctx.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
    int n = 0;
    if (GetThreadContext(t->h, &ctx)) {
        while (n < max && ctx.Rip) {
            uint64_t base = 0, rsp = ctx.Rsp;
            RUNTIME_FUNCTION *f = func_entry(ctx.Rip, &base);
            pcs[n] = ctx.Rip, starts[n] = f ? base + f->BeginAddress : 0, n++;
            if (f) {
                PVOID hd = NULL;
                DWORD64 ef = 0;
                RtlVirtualUnwind(UNW_FLAG_NHANDLER, base, ctx.Rip, f, &ctx, &hd, &ef, NULL);
            } else {  // a leaf function (no unwind entry): the return address is on top of the stack
                if (!readable(ctx.Rsp)) break;
                ctx.Rip = *(DWORD64 *)(uintptr_t)ctx.Rsp;
                ctx.Rsp += 8;
            }
            if (ctx.Rsp <= rsp) break;  // (each caller sits higher up the stack: else a bad unwind)
        }
    }
    ResumeThread(t->h);
    return n;
}
// A module's exported name for the function starting at start (NULL: none - a static function: the offset says which)
static const char *export_at(uint64_t base, uint64_t start) {
    const IMAGE_DATA_DIRECTORY *dd = pe_dir(base, IMAGE_DIRECTORY_ENTRY_EXPORT);
    if (!dd) return NULL;
    const IMAGE_EXPORT_DIRECTORY *ed = (const IMAGE_EXPORT_DIRECTORY *)(uintptr_t)(base + dd->VirtualAddress);
    const DWORD *fn = (const DWORD *)(uintptr_t)(base + ed->AddressOfFunctions), *nm = (const DWORD *)(uintptr_t)(base + ed->AddressOfNames);
    const WORD *ord = (const WORD *)(uintptr_t)(base + ed->AddressOfNameOrdinals);
    for (DWORD i = 0; i < ed->NumberOfNames; i++)
        if (ord[i] < ed->NumberOfFunctions && base + fn[ord[i]] == start) return (const char *)(uintptr_t)(base + nm[i]);
    return NULL;
}
static void frame_text(uint64_t pc, uint64_t start, char *out, size_t n) {
    uint64_t off = 0;
    const char *g = rt_host_fn_name(pc, &off);
    const HwMod *m = mod_of(pc);
    if (g && m && m->base != (uint64_t)(uintptr_t)GetModuleHandleA(NULL)) { snprintf(out, n, "%s+%llx (game code)", g, (unsigned long long)off); return; }
    if (!m) { snprintf(out, n, "%llx (no module)", (unsigned long long)pc); return; }
    const char *e = start ? export_at(m->base, start) : NULL;
    if (e) snprintf(out, n, "%s+%llx %s+%llx", m->name, (unsigned long long)(pc - m->base), e, (unsigned long long)(pc - start));
    else if (start) snprintf(out, n, "%s+%llx (function at +%llx)", m->name, (unsigned long long)(pc - m->base), (unsigned long long)(start - m->base));
    else snprintf(out, n, "%s+%llx", m->name, (unsigned long long)(pc - m->base));
}
static void stacks_prepare(void) { mods_scan(); }
static bool is_self(const HwThread *t) { return t->tid == GetCurrentThreadId(); }
#else
static void *volatile g_bt[HW_FRAMES + 2];
static volatile int g_bt_n, g_bt_done;
static void bt_handler(int sig) {
    (void)sig;
    int n = backtrace((void **)g_bt, HW_FRAMES + 2);
    g_bt_n = n;
    __atomic_store_n(&g_bt_done, 1, __ATOMIC_RELEASE);
}
#define HW_SIG (SIGRTMIN + 6)  // (hle/hwwatch.c has +5)
static void stacks_prepare(void) {
    static bool done;
    if (done) return;
    done = true;
    void *tmp[4];
    backtrace(tmp, 4);  // (loads the unwinder now, not inside the handler)
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = bt_handler;
    sa.sa_flags = SA_RESTART;
    sigemptyset(&sa.sa_mask);
    sigaction(HW_SIG, &sa, NULL);
}
static int thread_stack(HwThread *t, uint64_t *pcs, uint64_t *starts, int max) {
    __atomic_store_n(&g_bt_done, 0, __ATOMIC_RELEASE);
    if (pthread_kill(t->th, HW_SIG)) return -1;
    uint64_t until = plat_now_ns() + 500000000ull;
    while (!__atomic_load_n(&g_bt_done, __ATOMIC_ACQUIRE) && plat_now_ns() < until) plat_sleep_until_ns(plat_now_ns() + 1000000);
    if (!__atomic_load_n(&g_bt_done, __ATOMIC_ACQUIRE)) return -1;
    int n = 0;
    for (int i = 2; i < g_bt_n && n < max; i++) pcs[n] = (uint64_t)(uintptr_t)g_bt[i], starts[n++] = 0;  // (from the interrupted function)
    return n;
}
static void frame_text(uint64_t pc, uint64_t start, char *out, size_t n) {
    (void)start;
    Dl_info di = {0}, self = {0};
    dladdr((void *)frame_text, &self);
    if (!dladdr((void *)(uintptr_t)pc, &di)) { snprintf(out, n, "%llx", (unsigned long long)pc); return; }
    const char *mod = di.dli_fname ? (strrchr(di.dli_fname, '/') ? strrchr(di.dli_fname, '/') + 1 : di.dli_fname) : "?";
    uint64_t moff = pc - (uint64_t)(uintptr_t)di.dli_fbase, off = 0;
    bool ours = di.dli_fbase == self.dli_fbase;
    const char *g = !ours ? rt_host_fn_name(pc, &off) : NULL;
    if (g) snprintf(out, n, "%s+%llx (game code)", g, (unsigned long long)off);
    else if (di.dli_sname) snprintf(out, n, "%s+%llx %s+%llx", mod, (unsigned long long)moff, di.dli_sname,
                                    (unsigned long long)(pc - (uint64_t)(uintptr_t)di.dli_saddr));
    else snprintf(out, n, "%s+%llx", mod, (unsigned long long)moff);
}
static bool is_self(const HwThread *t) { return pthread_equal(t->th, pthread_self()); }
#endif

// Each registered thread's host call stack; threads whose stack is the same as the one before (idle workers) in one line
static void stacks_report(void) {
    int n = __atomic_load_n(&g_nthreads, __ATOMIC_ACQUIRE);
    if (!n) return;
    stacks_prepare();
    static uint64_t pcs[HW_FRAMES], starts[HW_FRAMES], prev[HW_FRAMES];
    int nprev = -2;
    const char *prev_name = NULL;
    for (int i = 0; i < n; i++) {
        HwThread *t = &g_threads[i];
        if (is_self(t)) continue;
        int k = thread_stack(t, pcs, starts, HW_FRAMES);  // (formatted after: the thread runs again first)
        if (k < 0) { LOG(TAG "host stack of %s: could not be read\n", t->name); continue; }
        if (k == nprev && !memcmp(pcs, prev, (size_t)k * sizeof *pcs)) { LOG(TAG "host stack of %s: as %s\n", t->name, prev_name); continue; }
        LOG(TAG "host stack of %s (thread %ld), innermost first:\n", t->name, (long)t->tid);
        for (int j = 0; j < k; j++) {
            char line[256];
            frame_text(pcs[j], starts[j], line, sizeof line);
            LOG(TAG "  #%-2d %s\n", j, line);
        }
        memcpy(prev, pcs, (size_t)k * sizeof *pcs), nprev = k, prev_name = t->name;
    }
}

static void report(uint64_t stall_ns, int nth) {
    uint64_t shown = renderer_present_count(), game = renderer_game_frame();
    LOG(TAG "---- no frame shown for %.1f s (frames shown %llu, game frames %llu) - %s\n", stall_ns / 1e9, (unsigned long long)shown,
        (unsigned long long)game, nth == 1 ? "the game may be stuck (a long load ends with an \"over\" line)" : "still stuck");
    LOG(TAG "version %s, exe base %llx\n", RT_APP_VERSION, (unsigned long long)exe_base());
    rt_hang_report(TAG);
    vk_swapchain_hang_report(TAG);
    vk_hang_report(TAG);
    kernel_thread_dump(TAG, false);
    KThread *pt = rt_present_thread();
    if (pt && pt->hcr) {
        LOG(TAG "the presenting thread %llx (the game's frame loop): its last host calls, oldest first:\n", (unsigned long long)pt->id);
        hostcalls_log(pt->hcr, HOSTCALL_RING, TAG "  ");
    }
    stacks_report();  // (last: stopping threads is the one step here that could itself get stuck)
    LOG(TAG "---- end\n");
}

static void *watch_main(void *arg) {
    (void)arg;
    crash_handler_install();
    uint64_t prev = plat_now_ns(), base = 0, frames_at_report = 0, next_ns = 0;
    int reports = 0;
    for (;;) {
        plat_sleep_until_ns(plat_now_ns() + 500000000ull);
        uint64_t now = plat_now_ns();
        bool jumped = now - prev > 5000000000ull;  // (this thread didn't run for 5 s: the PC slept, a debugger)
        prev = now;
        if (!g_armed) {
            if (g_rt_unwind || renderer_present_count() == g_boot_frames) continue;
            g_armed = 1, base = now, reports = 0;
        }
        uint64_t last = vk_last_present_ns(), shown = renderer_present_count();
        if (reports && shown != frames_at_report) {
            LOG(TAG "over: frames are shown again, %.1f s after the last one (a stall, not a hang)\n",
                (last > base ? last - base : now - base) / 1e9);
            reports = 0;
        }
        if (last > base) base = last;
        if (g_rt_unwind) { g_armed = 0, g_boot_frames = shown; continue; }  // (a reboot: armed again at its first frame)
        if (jumped || g_hang_paused) { base = now; continue; }
        uint64_t stall = now - base;
        if (!reports && stall >= g_limit_ns) {
            report(stall, 1);
            reports = 1, frames_at_report = shown, next_ns = stall + 60000000000ull;
        } else if (reports == 1 && stall >= next_ns) {
            report(stall, 2);
            reports = 2;
        }
    }
    return NULL;
}

// kernel_init (each boot)
void hangwatch_start(void) {
    static int started, off;
    if (!started) {
        started = 1;
        const char *e = ENV("LARECOMP_HANG_WATCH");
        if (e && atof(e) <= 0) { off = 1; LOG(TAG "watch off (LARECOMP_HANG_WATCH=0)\n"); return; }
        if (e) g_limit_ns = (uint64_t)(atof(e) * 1e9);
        pthread_t th;
        if (pthread_create(&th, NULL, watch_main, NULL) == 0) pthread_detach(th);
    }
    if (off) return;
    g_boot_frames = renderer_present_count();
    g_armed = 0;
}

RT_ON_RESET(hangwatch_reset) { g_armed = 0, g_boot_frames = renderer_present_count(); }
