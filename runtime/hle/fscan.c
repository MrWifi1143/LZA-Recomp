// Research aid: LARECOMP_FSCAN=<present>: the game's heap at that present and the next two, compared as floats - every
// 4-byte word that grows (or shrinks) by the same step on both frames, a step of 1, 0.5, 2, 1/30 or 1/60, logged as
// "[fscan]" (frame-counting timers: what runs too fast above 30 fps). LARECOMP_FSCAN_WATCH=<addr>[,<addr>...] then
// puts hardware watchpoints on those words (hwwatch.c) to name their writers.
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "../hle.h"
#include "../kernel.h"

void hwwatch_arm(uint64_t addr, uint64_t len);  // hle/hwwatch.c

static uint32_t *g_a, *g_b;
static uint64_t g_words;
static uint64_t g_auto[4], g_auto_int[4];  // LARECOMP_FSCAN_WATCH=auto: what was found, watched in the same run
static int g_nauto, g_nauto_int;

static bool step_ok(float d) {
    static const float k[] = {1.0f, 0.5f, 2.0f, 1.0f / 30, 1.0f / 60, 0.25f};
    float ad = fabsf(d);
    for (size_t i = 0; i < sizeof k / sizeof *k; i++)
        if (fabsf(ad - k[i]) <= k[i] * 1e-4f) return true;
    return false;
}

void fscan_present(uint64_t frame) {
    static long at = -2;
    if (at == -2) at = getenv("LARECOMP_FSCAN") ? atol(getenv("LARECOMP_FSCAN")) : -1;
    if (at < 0 || frame < (uint64_t)at || frame > (uint64_t)at + 3) return;
    const uint32_t *heap = (const uint32_t *)(uintptr_t)HEAP_BASE;
    uint64_t k = frame - (uint64_t)at;
    if (k == 0) {
        extern uint64_t g_stdheap_used;  // (heaps.c: the StandardAllocators' window above the kernel heap)
        uint64_t top = g_stdheap_used ? STDHEAP_BASE + g_stdheap_used - HEAP_BASE : 0;
        g_words = (kernel_heap_size() > top ? kernel_heap_size() : top) / 4;
        LOG("[fscan] scanning %llu MB\n", (unsigned long long)(g_words >> 18));
        g_a = malloc(g_words * 4), g_b = malloc(g_words * 4);
        if (!g_a || !g_b) { LOG("[fscan] out of memory\n"); at = -1; return; }
        memcpy(g_a, heap, g_words * 4);
    } else if (k == 1) {
        memcpy(g_b, heap, g_words * 4);
    } else if (k == 2) {
        int hits = 0, ints = 0;
        for (uint64_t i = 0; i < g_words && hits < 3000; i++) {
            if (g_a[i] == g_b[i]) continue;
            float a, b, c;
            uint32_t ci = heap[i];
            if (g_b[i] - g_a[i] == 1 && ci - g_b[i] == 1 && g_a[i] < 10000000 && ints < 400) {  // (an integer frame counter)
                LOG("[fscan] %llx: int %u %u %u\n", (unsigned long long)(HEAP_BASE + i * 4), g_a[i], g_b[i], ci);
                ints++;
                // (auto: the counters that started a while ago, not with the game: what a scene counts its frames with)
                if (g_a[i] >= 30 && g_a[i] < (uint32_t)(frame / 2) && g_nauto_int < 4) g_auto_int[g_nauto_int++] = HEAP_BASE + i * 4;
                continue;
            }
            memcpy(&a, &g_a[i], 4), memcpy(&b, &g_b[i], 4), memcpy(&c, &ci, 4);
            if (!isfinite(a) || !isfinite(c) || fabsf(a) > 1e6f) continue;
            float d1 = b - a, d2 = c - b;
            if (!step_ok(d1) || fabsf(d1 - d2) > fabsf(d1) * 1e-3f) continue;
            LOG("[fscan] %llx: %g %g %g (step %g)\n", (unsigned long long)(HEAP_BASE + i * 4), a, b, c, d1);
            if (getenv("LARECOMP_FSCAN_DUMP") && hits < 12) {  // (research: the words around it, as hex and float)
                char line[1024];
                int n = 0;
                for (int j = -12; j < 12 && n < 1000; j++) {
                    uint32_t v = heap[i + j];
                    float fv;
                    memcpy(&fv, &v, 4);
                    n += snprintf(line + n, sizeof line - n, " %s%08x(%.4g)", j ? "" : "*", v, fv);
                }
                LOG("[fscan]   %s\n", line);
            }
            if (fabsf(d1) >= 0.25f && g_nauto < 3) g_auto[g_nauto++] = HEAP_BASE + i * 4;  // (auto: frame-stepped floats first)
            { extern void trace_watch_add(uint64_t addr); if (getenv("LARECOMP_FSCAN_WATCH")) trace_watch_add(HEAP_BASE + i * 4); }  // (storetrace.c, a trace build)
            hits++;
        }
        LOG("[fscan] present %llu: %d frame-stepped floats\n", (unsigned long long)frame, hits);
        free(g_a), free(g_b);
        g_a = g_b = NULL;
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
