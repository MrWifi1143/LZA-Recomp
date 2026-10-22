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
#elif !defined(__linux__)
void cpupin_init(void) {}
void cpupin_self(int cpu) { (void)cpu; }
#else
#include <sched.h>

void cpupin_self(int cpu) {
    if (cpu < 0) return;
    cpu_set_t cs;
    CPU_ZERO(&cs);
    CPU_SET(cpu, &cs);
    sched_setaffinity(0, sizeof cs, &cs);
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
    for (int i = 0; i < 4; i++) g_core_cpu[i] = pc[i].first;
    g_render_cpu = pc[4].first;
    for (int i = 4; i < 8 && i + 1 < fast; i++) g_core_cpu[i] = pc[i + 1].first;  // (cores 4+, when a game has them -
    // hle/za_jobs.c's extra job workers - on the next fast cores; not kept free of other threads)
    sched_setaffinity(0, sizeof rest, &rest);  // (this thread and every thread it starts from now on)
    LOG("[cpu] cores on CPUs %d %d %d %d, render thread %d; other threads on the %d CPUs left\n", g_core_cpu[0], g_core_cpu[1],
        g_core_cpu[2], g_core_cpu[3], g_render_cpu, CPU_COUNT(&rest));
}
#endif
