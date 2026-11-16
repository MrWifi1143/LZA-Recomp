// Hang watch: a thread of ours that notices when no frame has been shown for a while although the process lives on (a
// freeze with the music still playing, nothing in the log), and logs once what everything waits on - "[hang]" lines:
// the render thread's job, the GPU timeline and who waits on it, the swapchain's acquire / present, every guest thread
// (the svc it is in, the mutex / condition variable / address / handles it waits on, its guest call stack, where host
// code blocked it, its last host call) and the presenting thread's last host calls. Again 60 s later if still stuck;
// a stall that ends (a long load) says so. Read-only and lock-free: it never takes a lock the game or the render thread
// holds, so it can't add to a hang. Armed at each boot's first present (the start screen, the setup and a reboot don't
// present game frames), quiet while the game is paused in the background.
// LARECOMP_HANG_WATCH=0: off; =<seconds>: after how long (default 10). LARECOMP_TEST_HANG=<frame>[,render] (research,
// renderthread.c): the game / render thread stops for good at that frame - the watch's test.
#define _GNU_SOURCE 1  // (dladdr)
#include <pthread.h>
#include <stdlib.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
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
