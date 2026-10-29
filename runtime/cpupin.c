// Host CPUs for the emulated cores and the render thread (Linux). The game runs its frame on three emulated cores
// (kernel.c "cores"), the render thread translates it; left to the OS scheduler those four busy threads shared physical
// cores with each other's hyper-threads, the GPU driver's threads and the shader compiles, and on hybrid CPUs (Intel
// 12th gen and later) moved to the slower efficiency cores. Lumiose, 144 fps target: game threads busy ~7.4 ms of a
// frame unpinned, ~6.6 ms pinned; ~4 fps more. Each of them gets a physical core of its own - the fastest ones, by
// their maximum clock - and every other thread (started after cpupin_init) the CPUs left over. Only with 8 or more
// fast physical cores, so the rest keep enough room. LARECOMP_PIN=0: off; LARECOMP_PIN=<core 0 cpu>,<core 1>,<core 2>,
// <core 3>[,<render thread>]: those CPUs (the others not restricted). Emulated cores 4+ (Z-A's extra job workers) go on
// the fast cores after those.
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hle.h"

static int g_core_cpu[8] = {-1, -1, -1, -1, -1, -1, -1, -1}, g_render_cpu = -1;
int cpupin_core(int c) { return c >= 0 && c < 8 ? g_core_cpu[c] : -1; }
int cpupin_render(void) { return g_render_cpu; }
static int g_fast;  // fast physical cores (cpupin_init)
int cpupin_fast_cores(void) { return g_fast; }

#if defined(_WIN32)
#include <windows.h>
// (no pinning: the count only, for hle/za_jobs.c's job_workers = auto - physical cores of the highest efficiency class,
// the performance cores of a hybrid CPU; all of them otherwise)
void cpupin_init(void) {
    DWORD len = 0;
    GetLogicalProcessorInformationEx(RelationProcessorCore, NULL, &len);
    uint8_t *buf = len ? malloc(len) : NULL;
    if (!buf || !GetLogicalProcessorInformationEx(RelationProcessorCore, (PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX)buf, &len)) {
        free(buf);
        return;
    }
    int best = -1, n = 0;
    for (DWORD o = 0; o < len;) {
        PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX e = (PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX)(buf + o);
        int c = e->Processor.EfficiencyClass;
        if (c > best) best = c, n = 0;
        if (c == best) n++;
        o += e->Size;
    }
    free(buf);
    g_fast = n;
    LOG("[cpu] %d fast physical cores\n", n);
}
void cpupin_self(int cpu) { (void)cpu; }
void cpupin_self_core(int c) { (void)c; }
unsigned cpupin_epoch(void) { return 0; }
void cpupin_render_refresh(void) {}
void cpupin_rest_push(void) {}
void cpupin_rest_pop(void) {}
#elif !defined(__linux__)
void cpupin_init(void) {}
void cpupin_self(int cpu) { (void)cpu; }
void cpupin_self_core(int c) { (void)c; }
unsigned cpupin_epoch(void) { return 0; }
void cpupin_render_refresh(void) {}
void cpupin_rest_push(void) {}
void cpupin_rest_pop(void) {}
#else
#include <sched.h>
#include <pthread.h>
#include <unistd.h>

void cpupin_self(int cpu);
static cpu_set_t g_core_sib[8];
// Adaptive pinning: pinned threads can't move off a CPU another program's thread is busy on, so under load (a game, a
// build, a world generating beside us) the pinning that helps on a quiet PC costs frames. Lumiose walk uncapped beside
// a steady 12-thread load (3 runs each): pinned 110 fps avg / 1% low 65, adaptive 116 / 72, not pinned 118 / 72; on
// a quiet PC (2 each): pinned 177, adaptive 189 (frames over 6.94 ms ~85), not pinned 172-184 (155-323). cpupin_monitor
// watches the CPU time other programs use (all CPUs' busy time minus ours, /proc/stat and /proc/self/stat); above
// LARECOMP_PIN_ADAPT cores (default 4) for 3 s the cores' and the render thread's threads may run on any CPU
// (g_loose), back to their own cores once it stays below half that for 5 s. LARECOMP_PIN_ADAPT=0: always pinned.
static cpu_set_t g_all, g_rest;
#include <dirent.h>
// Every thread of ours on the given CPUs (the cores' and render thread's own pinning follows by the epoch)
static void all_threads(const cpu_set_t *cs) {
    DIR *d = opendir("/proc/self/task");
    if (!d) return;
    for (struct dirent *e; (e = readdir(d));)
        if (e->d_name[0] != '.') sched_setaffinity((pid_t)atoi(e->d_name), sizeof *cs, cs);
    closedir(d);
}
static volatile int g_loose;
static volatile unsigned g_epoch = 1;
unsigned cpupin_epoch(void) { return g_epoch; }  // (each emulated core's physical core: its hyperthreads)
// This thread on emulated core c's physical core, both of its hyperthreads (its first CPU only without SMT). Each
// emulated core's guest threads share it - the game's main loop or a job worker with that core's RenderWorker, audio
// and helper threads - and on a single CPU a thread woken there (a lock it waited for free, a job done) preempted the
// core's owner or waited for its slice: the game's frame stalled behind it (the Lumiose walk's critical path: wake-ups
// ~0.5 ms instead of ~0.1 in the frames that missed 144 Hz). On the core's otherwise idle sibling it runs at once; only
// one of them runs guest code at a time (kernel.c "cores") anyway. Lumiose walk capped at 144 (3 runs each, Max people,
// Ultra shadows, 1440p, i9-12900K): presents over 7.6 ms 2.3% -> 0.8%, over 10.4 ms ~26 -> ~10 per 7400, 1% low
// ~117 -> ~134 fps. Uncapped the average drops a little (~175 -> ~170: two busy threads on one physical core) while
// frames over 10 ms fall ~58 -> ~21. LARECOMP_PIN_SMT=0: one CPU per core, as before.
void cpupin_self_core(int c) {
    static int smt = -1;
    if (smt < 0) smt = !(getenv("LARECOMP_PIN_SMT") && atoi(getenv("LARECOMP_PIN_SMT")) == 0);
    if (c < 0 || c >= 8 || g_core_cpu[c] < 0) return;
    if (g_loose) { sched_setaffinity(0, sizeof g_all, &g_all); return; }
    if (smt && CPU_COUNT(&g_core_sib[c]) > 1) sched_setaffinity(0, sizeof g_core_sib[c], &g_core_sib[c]);
    else cpupin_self(g_core_cpu[c]);
}
void cpupin_self(int cpu) {
    if (cpu < 0) return;
    cpu_set_t cs;
    CPU_ZERO(&cs);
    CPU_SET(cpu, &cs);
    sched_setaffinity(0, sizeof cs, &cs);
}

// A thread that isn't a core's or the render thread's - or one that starts those (a pinned thread's new threads inherit
// its CPU: the render thread's pipeline compilers all landed on its one CPU, a game thread's audio threads on that
// core's) - on the CPUs left over until cpupin_rest_pop
static __thread cpu_set_t t_saved;
static __thread int t_pushed;
void cpupin_rest_push(void) {
    if (!CPU_COUNT(&g_rest)) return;
    t_pushed = sched_getaffinity(0, sizeof t_saved, &t_saved) == 0;
    sched_setaffinity(0, sizeof g_rest, g_loose ? &g_all : &g_rest);
}
void cpupin_rest_pop(void) {
    if (t_pushed) sched_setaffinity(0, sizeof t_saved, &t_saved), t_pushed = 0;
}

// The render thread, once a frame: its CPU again after the mode changed
void cpupin_render_refresh(void) {
    static __thread unsigned seen;
    if (seen == g_epoch || g_render_cpu < 0) return;
    seen = g_epoch;
    if (g_loose) sched_setaffinity(0, sizeof g_all, &g_all);
    else cpupin_self(g_render_cpu);
}

static long read_long(const char *path) {
    FILE *f = fopen(path, "r");
    long v = -1;
    if (f) {
        if (fscanf(f, "%ld", &v) != 1) v = -1;
        fclose(f);
    }
    return v;
}

static bool read_list(const char *path, cpu_set_t *out) {  // "0-1", "4,12", ...
    FILE *f = fopen(path, "r");
    if (!f) return false;
    char line[256];
    bool ok = fgets(line, sizeof line, f) != NULL;
    fclose(f);
    if (!ok) return false;
    CPU_ZERO(out);
    for (char *p = line; *p && *p != '\n';) {
        long a = strtol(p, &p, 10), b = a;
        if (*p == '-') b = strtol(p + 1, &p, 10);
        for (long i = a; i <= b && i < CPU_SETSIZE; i++) CPU_SET((int)i, out);
        if (*p == ',') p++;
        else break;
    }
    return CPU_COUNT(out) > 0;
}

static bool read_cpu_ticks(unsigned long long *busy, unsigned long long *self) {
    FILE *f = fopen("/proc/stat", "r");
    if (!f) return false;
    unsigned long long v[10] = {0};
    int n = fscanf(f, "cpu %llu %llu %llu %llu %llu %llu %llu %llu", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &v[7]);
    fclose(f);
    if (n < 5) return false;
    *busy = v[0] + v[1] + v[2] + v[5] + v[6] + v[7];  // (user nice system irq softirq steal: not idle / iowait)
    f = fopen("/proc/self/stat", "r");
    if (!f) return false;
    char buf[1024];
    size_t len = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[len] = 0;
    char *p = strrchr(buf, ')');
    unsigned long long ut = 0, st = 0;
    if (!p || sscanf(p + 2, "%*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %llu %llu", &ut, &st) != 2) return false;
    *self = ut + st;
    return true;
}
static void *monitor(void *arg) {
    double on = *(double *)arg, off = on / 2;
    long hz = sysconf(_SC_CLK_TCK);
    unsigned long long b0 = 0, s0 = 0;
    read_cpu_ticks(&b0, &s0);
    int above = 0, below = 0;
    for (;;) {
        usleep(500000);
        unsigned long long b1, s1;
        if (!read_cpu_ticks(&b1, &s1)) continue;
        double others = (double)((long long)(b1 - b0) - (long long)(s1 - s0)) / (double)hz / 0.5;  // (CPUs busy with other programs)
        b0 = b1, s0 = s1;
        above = others > on ? above + 1 : 0;
        below = others < off ? below + 1 : 0;
        if (!g_loose && above >= 6) {  // (3 s: a world saving or a burst of chunk work beside us flipped it every few seconds)
            g_loose = 1, g_epoch++, all_threads(&g_all);
            LOG("[cpu] other programs busy (%.1f CPUs): threads not pinned for now\n", others);
        } else if (g_loose && below >= 10) {
            g_loose = 0, all_threads(&g_rest), g_epoch++;  // (then each core's threads back on their own core, cpupin_self_core)
            LOG("[cpu] quiet again (%.1f CPUs): threads pinned\n", others);
        }
    }
    return NULL;
}

typedef struct { int first; long freq; cpu_set_t sib; } PhysCore;
static int by_speed(const void *a, const void *b) {
    const PhysCore *x = a, *y = b;
    if (x->freq != y->freq) return x->freq > y->freq ? -1 : 1;
    return x->first - y->first;
}

void cpupin_init(void) {
    const char *e = getenv("LARECOMP_PIN");
    if (e && !strcmp(e, "0")) return;
    if (e && *e) {
        int v[5] = {-1, -1, -1, -1, -1};
        for (int i = 0; i < 5 && *e; i++) {
            v[i] = (int)strtol(e, (char **)&e, 10);
            while (*e == ',') e++;
        }
        for (int i = 0; i < 4; i++) g_core_cpu[i] = v[i];
        g_render_cpu = v[4];
        LOG("[cpu] cores on CPUs %d %d %d %d, render thread %d (LARECOMP_PIN)\n", v[0], v[1], v[2], v[3], v[4]);
        return;
    }
    cpu_set_t allowed;
    if (sched_getaffinity(0, sizeof allowed, &allowed)) return;
    static PhysCore pc[256];
    int n = 0;
    for (int cpu = 0; cpu < CPU_SETSIZE && n < 256; cpu++) {
        if (!CPU_ISSET(cpu, &allowed)) continue;
        char path[128];
        cpu_set_t sib;
        snprintf(path, sizeof path, "/sys/devices/system/cpu/cpu%d/topology/core_cpus_list", cpu);
        if (!read_list(path, &sib)) {
            snprintf(path, sizeof path, "/sys/devices/system/cpu/cpu%d/topology/thread_siblings_list", cpu);
            if (!read_list(path, &sib)) { CPU_ZERO(&sib); CPU_SET(cpu, &sib); }
        }
        int first = 0;
        while (first < CPU_SETSIZE && !CPU_ISSET(first, &sib)) first++;
        if (first != cpu) continue;  // (each physical core once, by its first CPU)
        snprintf(path, sizeof path, "/sys/devices/system/cpu/cpu%d/cpufreq/cpuinfo_max_freq", cpu);
        pc[n].first = cpu, pc[n].freq = read_long(path), pc[n].sib = sib;
        n++;
    }
    if (!n) return;
    qsort(pc, (size_t)n, sizeof *pc, by_speed);
    int fast = 0;
    while (fast < n && (pc[0].freq <= 0 || pc[fast].freq >= pc[0].freq * 95 / 100)) fast++;
    g_fast = fast;
    if (fast < 8) {
        LOG("[cpu] %d fast physical cores (of %d): threads not pinned\n", fast, n);
        return;
    }
    cpu_set_t rest = allowed;
    for (int i = 0; i < 5; i++)
        for (int k = 0; k < CPU_SETSIZE; k++)
            if (CPU_ISSET(k, &pc[i].sib)) CPU_CLR(k, &rest);
    for (int i = 0; i < 4; i++) g_core_cpu[i] = pc[i].first, g_core_sib[i] = pc[i].sib;
    g_render_cpu = pc[4].first;
    for (int i = 4; i < 8 && i + 1 < fast; i++) g_core_cpu[i] = pc[i + 1].first, g_core_sib[i] = pc[i + 1].sib;  // (cores 4+, when a game has them -
    // hle/za_jobs.c's extra job workers - on the next fast cores; not kept free of other threads)
    g_all = allowed, g_rest = rest;
    sched_setaffinity(0, sizeof rest, &rest);  // (this thread and every thread it starts from now on)
    {
        const char *a = getenv("LARECOMP_PIN_ADAPT");
        double on = a ? atof(a) : 4.0;
        if (on > 0) {
            static double s_on;
            s_on = on;
            pthread_t t;
            if (pthread_create(&t, NULL, monitor, &s_on) == 0) pthread_detach(t);
        }
    }
    LOG("[cpu] cores on CPUs %d %d %d %d, render thread %d; other threads on the %d CPUs left\n", g_core_cpu[0], g_core_cpu[1],
        g_core_cpu[2], g_core_cpu[3], g_render_cpu, CPU_COUNT(&rest));
}
#endif
