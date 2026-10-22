// Research aid: LARECOMP_FSCAN=<present>: the game's memory (the heap, and what MapPhysicalMemory mapped - where Z-A
// keeps most of its data) at that present and the next two, compared: every 4-byte word that grows (or shrinks) by
// the same step on both frames - an integer stepping 1 or 2, a float stepping 1, 0.5, 2, 1/30 or 1/60 - logged as
// "[fscan]" (frame-counting timers: what runs too fast above 30 fps). LARECOMP_FSCAN_WATCH=<addr>[,<addr>...] then puts
// hardware watchpoints on those words (hwwatch.c) to name their writers; =auto: on what was found (frame-stepped
// floats first, then integers). LARECOMP_FSCAN_MAX=<MB>: the most memory compared (default 8192).
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "../hle.h"
#include "../kernel.h"

void hwwatch_arm(uint64_t addr, uint64_t len);  // hle/hwwatch.c
int kernel_physmem_ranges(uint64_t *lo, uint64_t *hi, int max);  // kernel.c

#define NREG 520
static uint64_t g_lo[NREG], g_n[NREG];  // regions scanned (address, words) and their copies
static uint32_t *g_a[NREG], *g_b[NREG];
static int g_nreg;
static uint64_t g_auto[4], g_auto_int[4];
static int g_nauto, g_nauto_int;

static bool step_ok(float d) {
    static const float k[] = {1.0f, 0.5f, 2.0f, 1.0f / 30, 1.0f / 60, 0.25f};
    float ad = fabsf(d);
    for (size_t i = 0; i < sizeof k / sizeof *k; i++)
        if (fabsf(ad - k[i]) <= k[i] * 1e-4f) return true;
    return false;
}

static void free_copies(void) {
    for (int r = 0; r < g_nreg; r++) free(g_a[r]), free(g_b[r]), g_a[r] = g_b[r] = NULL;
    g_nreg = 0;
}

void fscan_present(uint64_t frame) {
    static long at = -2;
    if (at == -2) at = getenv("LARECOMP_FSCAN") ? atol(getenv("LARECOMP_FSCAN")) : -1;
    static bool started;
    if (at < 0 || frame < (uint64_t)at) return;
    if (!started) started = true, at = (long)frame;  // (the first present at or after it: the count can skip one)
    if (frame > (uint64_t)at + 3) return;
    uint64_t k = frame - (uint64_t)at;
    if (k == 0) {
        uint64_t budget = (getenv("LARECOMP_FSCAN_MAX") ? strtoull(getenv("LARECOMP_FSCAN_MAX"), NULL, 0) : 8192) << 20;
        extern uint64_t g_stdheap_used;  // (heaps.c: the StandardAllocators' window above the kernel heap)
        uint64_t top = g_stdheap_used ? STDHEAP_BASE + g_stdheap_used - HEAP_BASE : 0;
        uint64_t heap = kernel_heap_size() > top ? kernel_heap_size() : top;
        static uint64_t plo[NREG], phi[NREG];
        int np = kernel_physmem_ranges(plo, phi, NREG - 1);
        g_lo[0] = HEAP_BASE, g_n[0] = heap / 4, g_nreg = 1;
        for (int i = 0; i < np && g_nreg < NREG; i++) g_lo[g_nreg] = plo[i], g_n[g_nreg] = (phi[i] - plo[i]) / 4, g_nreg++;
        uint64_t total = 0;
        for (int r = 0; r < g_nreg; r++) {
            if ((total + g_n[r] * 4) > budget) g_n[r] = total < budget ? (budget - total) / 4 : 0;
            total += g_n[r] * 4;
            g_a[r] = g_n[r] ? malloc(g_n[r] * 4) : NULL, g_b[r] = g_n[r] ? malloc(g_n[r] * 4) : NULL;
            if (g_n[r] && (!g_a[r] || !g_b[r])) { LOG("[fscan] out of memory\n"); free_copies(); at = -1; return; }
            if (g_n[r]) memcpy(g_a[r], (const void *)(uintptr_t)g_lo[r], g_n[r] * 4);
        }
        LOG("[fscan] scanning %llu MB in %d regions (heap %llu MB, %d physical ranges)\n", (unsigned long long)(total >> 20), g_nreg,
            (unsigned long long)(heap >> 20), np);
        const char *fp = getenv("LARECOMP_FSCAN_PAIR");  // (=<a>,<b>: where two 32-bit integers a, b follow each other)
        if (fp && strchr(fp, ',')) {
            uint32_t pa = (uint32_t)strtoul(fp, NULL, 0), pb = (uint32_t)strtoul(strchr(fp, ',') + 1, NULL, 0);
            int found = 0;
            for (int r = 0; r < g_nreg && found < 200; r++)
                for (uint64_t i = 0; i + 1 < g_n[r] && found < 200; i++)
                    if (g_a[r][i] == pa && g_a[r][i + 1] == pb) LOG("[fscan] %u,%u at %llx\n", pa, pb, (unsigned long long)(g_lo[r] + i * 4)), found++;
            LOG("[fscan] pair %u,%u: %d places\n", pa, pb, found);
        }
        const char *fv = getenv("LARECOMP_FSCAN_VALUE");  // (=<float>: where memory holds exactly it)
        if (fv) {
            float want = (float)atof(fv);
            uint32_t bits;
            memcpy(&bits, &want, 4);
            int found = 0;
            for (int r = 0; r < g_nreg && found < 200; r++)
                for (uint64_t i = 0; i < g_n[r] && found < 200; i++)
                    if (g_a[r][i] == bits) {
                        LOG("[fscan] %g at %llx\n", want, (unsigned long long)(g_lo[r] + i * 4)), found++;
                        // (=<float>: write this instead, at matches number LARECOMP_FSCAN_SET_FROM..TO - bisecting a value)
                        const char *sv = getenv("LARECOMP_FSCAN_SET");
                        long from = getenv("LARECOMP_FSCAN_SET_FROM") ? atol(getenv("LARECOMP_FSCAN_SET_FROM")) : 1;
                        long to = getenv("LARECOMP_FSCAN_SET_TO") ? atol(getenv("LARECOMP_FSCAN_SET_TO")) : 1000000;
                        if (sv && found >= from && found <= to) { float nv = (float)atof(sv); memcpy((void *)(uintptr_t)(g_lo[r] + i * 4), &nv, 4); }
                    }
            LOG("[fscan] %g: %d places\n", want, found);
        }
    } else if (k == 1) {
        for (int r = 0; r < g_nreg; r++) if (g_n[r]) memcpy(g_b[r], (const void *)(uintptr_t)g_lo[r], g_n[r] * 4);
    } else if (k == 2) {
        int hits = 0, ints = 0;
        for (int r = 0; r < g_nreg; r++) {
            const uint32_t *mem = (const uint32_t *)(uintptr_t)g_lo[r];
            for (uint64_t i = 0; i < g_n[r] && hits < 3000; i++) {
                uint32_t a0 = g_a[r][i], b0 = g_b[r][i];
                if (a0 == b0) continue;
                uint32_t ci = mem[i];
                uint64_t addr = g_lo[r] + i * 4;
                uint32_t d = b0 - a0;
                if ((d == 1 || d == 2) && ci - b0 == d && a0 < 10000000) {  // (an integer frame counter)
                    if (ints < 600) LOG("[fscan] %llx: int %u %u %u (step %u)\n", (unsigned long long)addr, a0, b0, ci, d);
                    ints++;
                    // (auto: the counters that started a while ago, not with the game: what a scene counts its frames with)
                    if (a0 >= 30 && a0 < (uint32_t)(frame / 2) && g_nauto_int < 4) g_auto_int[g_nauto_int++] = addr;
                    continue;
                }
                float fa, fb, fc;
                memcpy(&fa, &a0, 4), memcpy(&fb, &b0, 4), memcpy(&fc, &ci, 4);
                if (!isfinite(fa) || !isfinite(fc) || fabsf(fa) > 1e6f) continue;
                float d1 = fb - fa, d2 = fc - fb;
                if (!step_ok(d1) || fabsf(d1 - d2) > fabsf(d1) * 1e-3f) continue;
                LOG("[fscan] %llx: %g %g %g (step %g)\n", (unsigned long long)addr, fa, fb, fc, d1);
                if (fabsf(d1) >= 0.25f && g_nauto < 3) g_auto[g_nauto++] = addr;
                hits++;
            }
        }
        LOG("[fscan] present %llu: %d frame-stepped floats, %d integer counters\n", (unsigned long long)frame, hits, ints);
        free_copies();
    } else {
        const char *w = getenv("LARECOMP_FSCAN_WATCH");
        if (w && !strcmp(w, "auto")) {
            for (int j = 0; j < g_nauto_int && g_nauto < 4; j++) g_auto[g_nauto++] = g_auto_int[j];  // (floats first)
            for (int j = 0; j < g_nauto; j++) { hwwatch_arm(g_auto[j], 4); LOG("[fscan] watching %llx\n", (unsigned long long)g_auto[j]); }
            w = NULL;
        }
        for (char *s = (char *)w; s && *s;) {
            uint64_t a = strtoull(s, &s, 16);
            if (a) { hwwatch_arm(a, 4); LOG("[fscan] watching %llx\n", (unsigned long long)a); }
            while (*s == ',') s++;
            if (*s && !strchr("0123456789abcdefABCDEF", *s)) break;
        }
    }
}
