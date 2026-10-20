// Research aid: LARECOMP_FDIFF=<present>: float words of the guest heap that advance by the same step (1, 2 or 4) from
// that present to the next and again to the one after - animation frame counters (layout animations add a fixed step per
// update: what to scale when the game runs faster than 30). Logged as "[fdiff]" with their step; LARECOMP_HWWATCH then names
// their writers. Reads the heap window while the game runs (a debug snapshot).
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "../hle.h"

static uint32_t *g_idx;  // word index from HEAP_BASE
static float *g_val, *g_step;
static size_t g_n;

void fdiff_present(uint64_t frame) {
    static long at = -2;
    if (at == -2) at = getenv("LARECOMP_FDIFF") ? atol(getenv("LARECOMP_FDIFF")) : -1;
    if (at < 0 || (long)frame < at || (long)frame > at + 3) return;
    const float *heap = (const float *)(uintptr_t)HEAP_BASE;
    uint64_t words = HEAP_MAX / 4;
    if ((long)frame == at) {
        size_t cap = 1 << 22;
        g_idx = malloc(cap * 4), g_val = malloc(cap * 4), g_n = 0;
        for (uint64_t i = 0; i < words; i++) {
            float v = heap[i];
            if (!(v >= 0.001f && v <= 100000.0f)) continue;
            if (g_n == cap) { cap *= 2; g_idx = realloc(g_idx, cap * 4), g_val = realloc(g_val, cap * 4); }
            g_idx[g_n] = (uint32_t)i, g_val[g_n] = v, g_n++;
        }
        LOG("[fdiff] present %llu: %zu candidate words\n", (unsigned long long)frame, g_n);
        return;
    }
    if ((long)frame == at + 1) {  // (the scan above took a while: the game moved on meanwhile, so this is the baseline)
        for (size_t j = 0; j < g_n; j++) g_val[j] = heap[g_idx[j]];
        return;
    }
    frame--;  // (the two steps compared: at + 1 -> at + 2 -> at + 3)
    size_t k = 0;
    if ((long)frame == at + 1) g_step = malloc((g_n ? g_n : 1) * 4);
    for (size_t j = 0; j < g_n; j++) {
        float v = heap[g_idx[j]], d = v - g_val[j];
        bool keep = (long)frame == at + 1 ? (d > 0.0f && d <= 8.0f) : fabsf(d - g_step[j]) <= 1e-5f * fmaxf(1.0f, fabsf(v));
        if (!keep) continue;
        g_idx[k] = g_idx[j], g_val[k] = v, g_step[k] = d, k++;
    }
    g_n = k;
    LOG("[fdiff] present %llu: %zu words still advancing\n", (unsigned long long)frame, g_n);
    if ((long)frame == at + 2) {
        // steps, most common first (with a sample address each)
        float steps[64]; int cnt[64], ns = 0; uint32_t sample[64];
        for (size_t j = 0; j < g_n; j++) {
            int s_ = 0;
            while (s_ < ns && fabsf(steps[s_] - g_step[j]) > 1e-5f) s_++;
            if (s_ == ns && ns < 64) steps[ns] = g_step[j], cnt[ns] = 0, sample[ns] = g_idx[j], ns++;
            if (s_ < 64) cnt[s_]++;
        }
        for (int a = 0; a < ns; a++)
            LOG("[fdiff]   step %.6g: %d words (e.g. %llx = %g)\n", steps[a], cnt[a], (unsigned long long)(HEAP_BASE + 4ull * sample[a]), heap[sample[a]]);
        for (size_t j = 0; j < g_n && j < 48; j++)
            LOG("[fdiff]   %llx = %g (step %g)\n", (unsigned long long)(HEAP_BASE + 4ull * g_idx[j]), g_val[j], g_step[j]);
    }
}
