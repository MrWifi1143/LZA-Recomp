// Research aid (LARECOMP_LIVE: <dir>/pscan appears, "<period>" inside, default 60): guest heap memory that changes on
// exactly every <period>th present for the next 2 periods + 10 presents - data updated by a frame-counting scheduler
// (Z-A: the animation of NPCs standing around changes once per 60 frames at any frame rate). Each 64-byte line is
// folded to a 64-bit value per present (the game runs ~3 fps meanwhile); lines that changed 2 or 3 times, <period>
// presents apart, are grouped into runs and logged as "[pscan]" with their phase (present % period). LARECOMP_LIVE's
// <dir>/watch ("<addr>[,<addr>...]") then puts hardware watchpoints on them (hle/hwwatch.c) to name the writer.
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "../hle.h"

#define LINES (HEAP_MAX / 64)
static uint64_t *g_fold;
static uint8_t *g_cnt, *g_first, *g_last;
static long g_at = -1, g_period = 60;

static inline uint64_t fold(const uint64_t *p) {
    uint64_t h = 0;
    for (int i = 0; i < 8; i++) h = (h << 7 | h >> 57) ^ p[i];
    return h;
}

void pscan_start(long period) {
    if (g_at >= 0) return;
    g_period = period > 1 && period < 120 ? period : 60;
    if (!g_fold) g_fold = malloc(LINES * 8), g_cnt = malloc(LINES), g_first = malloc(LINES), g_last = malloc(LINES);
    if (!g_fold || !g_cnt || !g_first || !g_last) { LOG("[pscan] out of memory\n"); return; }
    memset(g_cnt, 0, LINES);
    g_at = 0;
    LOG("[pscan] scanning the heap for %ld presents (period %ld)\n", 2 * g_period + 10, g_period);
}

void pscan_present(uint64_t frame) {
    if (g_at < 0) return;
    const uint64_t *heap = (const uint64_t *)(uintptr_t)HEAP_BASE;
    long k = g_at++;
    for (uint64_t i = 0; i < LINES; i++) {
        uint64_t h = fold(heap + i * 8);
        if (k && h != g_fold[i]) {
            if (g_cnt[i] < 255) g_cnt[i]++;
            if (g_cnt[i] == 1) g_first[i] = (uint8_t)k;
            g_last[i] = (uint8_t)k;
        }
        g_fold[i] = h;
    }
    if (g_at <= 2 * g_period + 10) return;
    g_at = -1;
    // runs of neighbouring lines with the same phase
    struct Run { uint64_t a, n; int phase, cnt; } top[48];
    int ntop = 0;
    uint64_t total = 0, gpu = 0;
    for (uint64_t i = 0; i < LINES;) {
        int c = g_cnt[i];
        bool hit = (c == 2 && g_last[i] - g_first[i] == g_period) || (c == 3 && g_last[i] - g_first[i] == 2 * g_period);
        if (!hit) { i++; continue; }
        int phase = g_first[i] % (int)g_period;
        uint64_t j = i + 1;
        while (j < LINES) {
            int cj = g_cnt[j];
            bool hj = ((cj == 2 && g_last[j] - g_first[j] == g_period) || (cj == 3 && g_last[j] - g_first[j] == 2 * g_period)) &&
                      g_first[j] % (int)g_period == phase;
            if (!hj) break;
            j++;
        }
        extern bool vk_gpu_may_write(uint64_t a);
        if (vk_gpu_may_write(HEAP_BASE + i * 64)) { gpu += j - i; i = j; continue; }  // (the GPU's output: no CPU writer)
        total += j - i;
        struct Run r = {HEAP_BASE + i * 64, j - i, phase, c};
        int s = ntop < 48 ? ntop++ : 47;
        if (s == 47 && ntop == 48 && top[47].n >= r.n) { i = j; continue; }
        top[s] = r;
        while (s > 0 && top[s - 1].n < top[s].n) { struct Run t = top[s]; top[s] = top[s - 1]; top[s - 1] = t; s--; }
        i = j;
    }
    LOG("[pscan] present %llu: %llu lines change every %ld presents (and %llu the GPU writes); largest runs:\n",
        (unsigned long long)frame, (unsigned long long)total, g_period, (unsigned long long)gpu);
    for (int i = 0; i < ntop; i++)
        LOG("[pscan]   %llx +%llx (phase %d, %d changes)\n", (unsigned long long)top[i].a, (unsigned long long)(top[i].n * 64), top[i].phase, top[i].cnt);
}

// LARECOMP_LIVE <dir>/bt ("<function> [n] [x0]": only calls with that x0, each one): the guest call chain (frame records through x29) of the next calls of that
// function, each distinct chain logged once as "[bt]", up to n (default 12) of them.
void rt_register_fnhook_at(uint64_t addr, int (*fn)(Cpu *c, uint64_t addr));
static uint64_t g_bt_fn, g_bt_x0;
static int g_bt_left;
static uint64_t g_bt_seen[64];
static int g_bt_nseen;
static int bt_hook(Cpu *c, uint64_t addr) {
    if (addr != g_bt_fn || g_bt_left <= 0 || (g_bt_x0 && c->x[0] != g_bt_x0)) return 0;
    uint64_t chain[16], h = 0;
    int n = 0;
    chain[n++] = c->x[30];
    uint64_t fp = c->x[29];
    for (; n < 16 && fp >= 0x7000000000ull && fp < 0x8000000000ull && (fp & 7) == 0; n++) {
        chain[n] = *(const uint64_t *)(uintptr_t)(fp + 8);
        fp = *(const uint64_t *)(uintptr_t)fp;
    }
    for (int i = 0; i < n; i++) h = h * 0x100000001b3ull ^ chain[i];
    for (int i = 0; i < g_bt_nseen && !g_bt_x0; i++) if (g_bt_seen[i] == h) return 0;  // (one object: every call, its frames)
    if (g_bt_nseen < 64) g_bt_seen[g_bt_nseen++] = h;
    g_bt_left--;
    char line[600];
    extern uint64_t renderer_game_frame(void);
    int len = snprintf(line, sizeof line, "[bt] f%llu %llx x0 %llx:", (unsigned long long)renderer_game_frame(), (unsigned long long)addr,
                       (unsigned long long)c->x[0]);
    for (int i = 0; i < n && len < (int)sizeof line - 20; i++) len += snprintf(line + len, sizeof line - (size_t)len, " %llx", (unsigned long long)chain[i]);
    LOG("%s\n", line);
    return 0;
}
void bt_start(uint64_t fn, int n, uint64_t x0) {
    g_bt_fn = fn, g_bt_left = n > 0 ? n : 12, g_bt_nseen = 0, g_bt_x0 = x0;
    rt_register_fnhook_at(fn, bt_hook);
    LOG("[bt] tracing %llx\n", (unsigned long long)fn);
}
