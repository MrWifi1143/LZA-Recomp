// Research aid: LARECOMP_FSCAN=<present>: the game's heap at that present and the next two, compared as floats - every
// 4-byte word that grows (or shrinks) by the same step on both frames, a step of 1, 0.5, 2, 1/30 or 1/60, logged as
// "[fscan]" (frame-counting timers: what runs too fast above 30 fps); with LARECOMP_FSCAN_LIVE=1, when <LARECOMP_LIVE>/scan_now
// appears (then deleted). LARECOMP_FSCAN_WATCH=<addr>[,<addr>...] then
// puts hardware watchpoints on those words (hwwatch.c) to name their writers.
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../hle.h"
#include "../kernel.h"

void hwwatch_arm(uint64_t addr, uint64_t len);  // hle/hwwatch.c

static uint32_t *g_a, *g_b;
static uint64_t g_words;
static uint64_t g_auto[4], g_auto_int[4];  // LARECOMP_FSCAN_WATCH=auto: what was found, watched in the same run
static int g_nauto, g_nauto_int;

static float g_step_lo = -1, g_step_hi;  // (LARECOMP_FSCAN_STEP=<lo>,<hi>: any constant step of that size, research)
static bool step_ok(float d) {
    if (g_step_lo < 0) {
        g_step_lo = 0;
        if (getenv("LARECOMP_FSCAN_STEP")) sscanf(getenv("LARECOMP_FSCAN_STEP"), "%f,%f", &g_step_lo, &g_step_hi);
    }
    if (g_step_hi > 0) return fabsf(d) >= g_step_lo && fabsf(d) <= g_step_hi;
    static const float k[] = {1.0f, 0.5f, 2.0f, 1.0f / 30, 1.0f / 60, 0.25f};
    float ad = fabsf(d);
    for (size_t i = 0; i < sizeof k / sizeof *k; i++)
        if (fabsf(ad - k[i]) <= k[i] * 1e-4f) return true;
    return false;
}

// (research: <live>/find64 holding a hex value: every aligned 8-byte word of the heap equal to it, logged with the
// 16 words before it, "[find64]")
static void find64_present(void) {
    if (!getenv("LARECOMP_LIVE")) return;
    char p[600];
    snprintf(p, sizeof p, "%s/find64", getenv("LARECOMP_LIVE"));
    FILE *f = fopen(p, "r");
    if (!f) return;
    unsigned long long want = 0;
    int ok = fscanf(f, "%llx", &want) == 1;
    fclose(f), remove(p);
    if (!ok) return;
    extern uint64_t g_stdheap_used;
    uint64_t top = g_stdheap_used ? STDHEAP_BASE + g_stdheap_used - HEAP_BASE : 0;
    uint64_t words = (kernel_heap_size() > top ? kernel_heap_size() : top) / 8;
    const uint64_t *heap = (const uint64_t *)(uintptr_t)HEAP_BASE;
    int hits = 0;
    for (uint64_t i = 56; i + 24 < words && hits < 200; i++) {
        if (heap[i] != want) continue;
        char line[2048];
        int n = 0;
        for (int j = -56; j < 24 && n < 2000; j++) n += snprintf(line + n, sizeof line - n, " %s%llx", j ? "" : "*", (unsigned long long)heap[i + j]);
        LOG("[find64] %llx:%s\n", (unsigned long long)(HEAP_BASE + i * 8), line);
        hits++;
    }
    LOG("[find64] %llx: %d hits\n", want, hits);
}

void fscan_present(uint64_t frame) {
    find64_present();
    static long at = -2;
    if (at == -2) at = getenv("LARECOMP_FSCAN") ? atol(getenv("LARECOMP_FSCAN")) : -1;
    if (at < 0 && getenv("LARECOMP_FSCAN_LIVE") && getenv("LARECOMP_LIVE") && frame % 4 == 0) {  // (or when <live>/scan_now appears)
        char p[600];
        snprintf(p, sizeof p, "%s/scan_now", getenv("LARECOMP_LIVE"));
        long later = 1;  // (the file may hold how many presents later to scan)
        FILE *lf = fopen(p, "r");
        if (lf) { if (fscanf(lf, "%ld", &later) != 1 || later < 1) later = 1; fclose(lf); }
        if (!remove(p)) at = (long)frame + later, g_nauto = g_nauto_int = 0, LOG("[fscan] asked at present %llu for %ld\n", (unsigned long long)frame, at);
    }
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
        for (uint64_t i = 0; i < g_words && hits < 6000; i++) {
            if (g_a[i] == g_b[i]) continue;
            float a, b, c;
            uint32_t ci = heap[i];
            if (g_a[i] - g_b[i] == 1 && g_b[i] - ci == 1 && g_a[i] < 2000 && ints < 400) {  // (an integer counting down: a wait)
                LOG("[fscan] %llx: int down %u %u %u\n", (unsigned long long)(HEAP_BASE + i * 4), g_a[i], g_b[i], ci);
                ints++;
                static int down = -1;
                if (down < 0) down = getenv("LARECOMP_FSCAN_WATCH") && !strcmp(getenv("LARECOMP_FSCAN_WATCH"), "down");
                static uint64_t dfloor = 1;
                if (dfloor == 1) dfloor = getenv("LARECOMP_FSCAN_MIN") ? strtoull(getenv("LARECOMP_FSCAN_MIN"), NULL, 16) : 0;
                static unsigned dlo = 1, dhi;  // (LARECOMP_FSCAN_DOWN=<lo>,<hi>: only countdowns at those values)
                if (dlo == 1) { dlo = 0, dhi = ~0u; if (getenv("LARECOMP_FSCAN_DOWN")) sscanf(getenv("LARECOMP_FSCAN_DOWN"), "%u,%u", &dlo, &dhi); }
                if (down && HEAP_BASE + i * 4 >= dfloor && g_a[i] >= dlo && g_a[i] <= dhi && g_nauto_int < 4) g_auto_int[g_nauto_int++] = HEAP_BASE + i * 4;
                continue;
            }
            if (g_b[i] - g_a[i] == 1 && ci - g_b[i] == 1 && g_a[i] < 10000000 && ints < 400) {  // (an integer frame counter)
                LOG("[fscan] %llx: int %u %u %u\n", (unsigned long long)(HEAP_BASE + i * 4), g_a[i], g_b[i], ci);
                ints++;
                // (auto: the counters that started a while ago, not with the game: what a scene counts its frames with)
                // (=ints: the ones that started with the scene - from 0 a moment ago - and only those)
                static int young = -1;
                static int skip;  // (=ints:<n>: the first n of those left out)
                if (young < 0) {
                    const char *w = getenv("LARECOMP_FSCAN_WATCH");
                    young = w && !strncmp(w, "ints", 4);
                    skip = young && w[4] == ':' ? atoi(w + 5) : 0;
                }
                static uint64_t floor_ = 1;  // (LARECOMP_FSCAN_MIN=<hex address>: only words from there up are watched)
                if (floor_ == 1) floor_ = getenv("LARECOMP_FSCAN_MIN") ? strtoull(getenv("LARECOMP_FSCAN_MIN"), NULL, 16) : 0;
                if (getenv("LARECOMP_FSCAN_WATCH") && !strcmp(getenv("LARECOMP_FSCAN_WATCH"), "down")) continue;
                if ((young ? g_a[i] < 60 && HEAP_BASE + i * 4 >= floor_ && skip-- <= 0 : g_a[i] >= 30 && g_a[i] < (uint32_t)(frame / 2)) && g_nauto_int < 4) g_auto_int[g_nauto_int++] = HEAP_BASE + i * 4;
                continue;
            }
            memcpy(&a, &g_a[i], 4), memcpy(&b, &g_b[i], 4), memcpy(&c, &ci, 4);
            if (!isfinite(a) || !isfinite(c) || fabsf(a) > 1e6f) continue;
            float d1 = b - a, d2 = c - b;
            {   // (LARECOMP_FSCAN_RANGE=<lo>,<hi>,<min step>: any value in [lo, hi] moving that much per frame, same direction)
                static float rlo = 1, rhi = 0, rmin = 0;
                static int rinit;
                if (!rinit) { rinit = 1; if (getenv("LARECOMP_FSCAN_RANGE")) sscanf(getenv("LARECOMP_FSCAN_RANGE"), "%f,%f,%f", &rlo, &rhi, &rmin); }
                if (rlo <= rhi) {
                    if (a >= rlo && a <= rhi && fabsf(d1) >= rmin && fabsf(d2) >= rmin && (d1 > 0) == (d2 > 0) && hits < 3000) {
                        LOG("[fscan] %llx: %g %g %g (moving %g %g)\n", (unsigned long long)(HEAP_BASE + i * 4), a, b, c, d1, d2);
                        hits++;
                        static float rauto = -1;  // (LARECOMP_FSCAN_RANGE_AUTO=<step>: with WATCH=auto, the first going up that fast)
                        if (rauto < 0) rauto = getenv("LARECOMP_FSCAN_RANGE_AUTO") ? (float)atof(getenv("LARECOMP_FSCAN_RANGE_AUTO")) : 0;
                        if (rauto > 0 && d1 >= rauto && d2 >= rauto * 0.3f && g_nauto < 4) g_auto[g_nauto++] = HEAP_BASE + i * 4;
                    }
                    continue;
                }
            }
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
            {   // (=step2: only the values stepping by 2 - 60 Hz frames counted 2 an update)
                static int step2 = -1;
                if (step2 < 0) {  // (=young1: floats stepping by 1 that started a moment ago - from under 10)
                    const char *w = getenv("LARECOMP_FSCAN_WATCH");
                    step2 = w && !strcmp(w, "step2") ? 1 : w && !strcmp(w, "young1") ? 2 : w && !strcmp(w, "fstep") ? 3 : 0;
                }
                bool pick = step2 == 3 ? true : step2 == 1 ? fabsf(fabsf(d1) - 2) < 1e-3f : step2 == 2 ? fabsf(fabsf(d1) - 1) < 1e-3f && fabsf(a) < 10 : fabsf(d1) >= 0.25f;
                if (pick && g_nauto < (step2 ? 4 : 3)) g_auto[g_nauto++] = HEAP_BASE + i * 4;
            }  // (auto: frame-stepped floats first)
            { extern void trace_watch_add(uint64_t addr); if (getenv("LARECOMP_FSCAN_WATCH")) trace_watch_add(HEAP_BASE + i * 4); }  // (storetrace.c, a trace build)
            hits++;
        }
        LOG("[fscan] present %llu: %d frame-stepped floats\n", (unsigned long long)frame, hits);
        free(g_a), free(g_b);
        g_a = g_b = NULL;
    } else {
        if (getenv("LARECOMP_FSCAN_LIVE")) at = -1;  // (live: ready for the next <live>/scan_now)
        const char *w = getenv("LARECOMP_FSCAN_WATCH");
        if (w && (!strncmp(w, "ints", 4) || !strcmp(w, "down"))) g_nauto = 0, w = "auto";  // (the floats left out)
        if (w && (!strcmp(w, "step2") || !strcmp(w, "young1") || !strcmp(w, "fstep"))) g_nauto_int = 0, w = "auto";
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
