// Sampling profiler (Linux). LARECOMP_SAMPLE=<from>:<to> (presented frames): every thread running then is sampled
// every millisecond of its own CPU time (a per-thread CPU-clock timer raising SIGPROF on that thread); each sample is
// the thread and its host pc. At <to>: build/sample/samples.bin (u32 tid, u32 pad, u64 pc per sample), threads.txt
// (tid name), maps.txt (/proc/self/maps) and frames.txt (frames, wall ns) - tools/sample_report.py makes the report:
// CPU ms per frame for each thread, and the hottest functions (game functions as f_<guest address>).
#ifdef __linux__
#define _GNU_SOURCE
#include <dirent.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <time.h>
#include <ucontext.h>
#include <unistd.h>

#include "../rt.h"

#define CAP (1u << 23)
typedef struct { uint32_t tid, pad; uint64_t pc; } Sample;
static Sample *g_buf;
static _Atomic uint32_t g_n;
static timer_t g_timers[512];
static int g_ntimers;
static long g_from = -1, g_to = -1;
static uint64_t g_t0;

static void on_prof(int sig, siginfo_t *si, void *ctx) {
    (void)sig, (void)si;
    uint32_t i = atomic_fetch_add_explicit(&g_n, 1, memory_order_relaxed);
    if (i >= CAP) return;
    g_buf[i].tid = (uint32_t)syscall(SYS_gettid);
    g_buf[i].pc = (uint64_t)((ucontext_t *)ctx)->uc_mcontext.gregs[REG_RIP];
}

static uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static void start(void) {
    g_buf = calloc(CAP, sizeof *g_buf);
    if (!g_buf) return;
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = on_prof;
    sa.sa_flags = SA_SIGINFO | SA_RESTART;
    sigaction(SIGPROF, &sa, NULL);
    DIR *d = opendir("/proc/self/task");
    for (struct dirent *e; d && (e = readdir(d)) && g_ntimers < 512;) {
        if (e->d_name[0] < '0' || e->d_name[0] > '9') continue;
        pid_t tid = (pid_t)atoi(e->d_name);
        clockid_t clk = (clockid_t)((~(unsigned)tid << 3) | 6);  // the thread's CPU clock (CPUCLOCK_SCHED | PERTHREAD)
        struct sigevent sev;
        memset(&sev, 0, sizeof sev);
        sev.sigev_notify = SIGEV_THREAD_ID;
        sev.sigev_signo = SIGPROF;
        sev._sigev_un._tid = tid;
        timer_t t;
        if (timer_create(clk, &sev, &t) != 0) continue;
        struct itimerspec it = {{0, 1000000}, {0, 1000000}};
        timer_settime(t, 0, &it, NULL);
        g_timers[g_ntimers++] = t;
    }
    if (d) closedir(d);
    g_t0 = now_ns();
    LOG("[sample] sampling %d threads\n", g_ntimers);
}

static void copy_file(const char *from, const char *to) {
    FILE *a = fopen(from, "r"), *b = fopen(to, "w");
    char buf[4096];
    size_t n;
    while (a && b && (n = fread(buf, 1, sizeof buf, a)) > 0) fwrite(buf, 1, n, b);
    if (a) fclose(a);
    if (b) fclose(b);
}

static void stop(void) {
    for (int i = 0; i < g_ntimers; i++) timer_delete(g_timers[i]);
    uint64_t wall = now_ns() - g_t0;
    mkdir("build/sample", 0755);
    uint32_t n = atomic_load(&g_n);
    if (n > CAP) n = CAP;
    FILE *f = fopen("build/sample/samples.bin", "wb");
    if (f) fwrite(g_buf, sizeof *g_buf, n, f), fclose(f);
    f = fopen("build/sample/threads.txt", "w");
    DIR *d = opendir("/proc/self/task");
    for (struct dirent *e; f && d && (e = readdir(d));) {
        if (e->d_name[0] < '0' || e->d_name[0] > '9') continue;
        char p[64], name[64] = "?";
        snprintf(p, sizeof p, "/proc/self/task/%s/comm", e->d_name);
        FILE *c = fopen(p, "r");
        if (c) { if (fgets(name, sizeof name, c)) name[strcspn(name, "\n")] = 0; fclose(c); }
        fprintf(f, "%s %s\n", e->d_name, name);
    }
    if (d) closedir(d);
    if (f) fclose(f);
    copy_file("/proc/self/maps", "build/sample/maps.txt");
    if ((f = fopen("build/sample/frames.txt", "w"))) fprintf(f, "%ld %llu\n", g_to - g_from, (unsigned long long)wall), fclose(f);
    LOG("[sample] %u samples over %ld frames (%.1f s): build/sample/, tools/sample_report.py\n", n, g_to - g_from, wall / 1e9);
}

void sampler_frame(uint64_t frame) {
    if (g_from < 0) {
        const char *e = getenv("LARECOMP_SAMPLE");
        g_from = g_to = 0;
        if (e) sscanf(e, "%ld:%ld", &g_from, &g_to);
    }
    if (!g_to) return;
    if ((long)frame == g_from) start();
    if ((long)frame == g_to) stop();
}
#else
#include <stdint.h>
void sampler_frame(uint64_t frame) { (void)frame; }
#endif
