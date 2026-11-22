// Research (2026-11-19, the Arceus clock): <live>/heapsnap holding a file path - the guest's writable memory (the heap
// window, mapped physical memory, the modules' data; the ranges hle/warp.c's story scan walks) written to that file from a thread of its own: per range {u64 lo, u64
// size} and its bytes. Diffed offline (snapshots across the game's own changes - a rest at camp - find what moved).
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../hle.h"

int kernel_physmem_ranges(uint64_t *lo, uint64_t *hi, int max);
int kernel_data_regions(uint64_t *addr, uint64_t *size, int max);
uint64_t kernel_heap_size(void);
extern uint64_t g_stdheap_used;

static char g_path[512];
static volatile int g_busy;

static void *snap_thread(void *arg) {
    (void)arg;
    static uint64_t lo[600], hi[600];
    int n = kernel_physmem_ranges(lo + 1, hi + 1, 80) + 1;
    uint64_t top = g_stdheap_used ? STDHEAP_BASE + g_stdheap_used : 0, heap = HEAP_BASE + kernel_heap_size();
    lo[0] = HEAP_BASE, hi[0] = top > heap ? top : heap;  // (the heap window: the kernel heap + the StandardAllocators' block)
    uint64_t ra[512], rs[512];
    int m = kernel_data_regions(ra, rs, 512);
    for (int i = 0; i < m && n < 600; i++)  // (the modules' data)
        if (ra[i] < HEAP_BASE) lo[n] = ra[i], hi[n] = ra[i] + rs[i], n++;
    FILE *f = fopen(g_path, "wb");
    if (!f) { LOG("[heapsnap] can't write %s\n", g_path); g_busy = 0; return NULL; }
    uint64_t total = 0;
    for (int i = 0; i < n; i++) {
        uint64_t sz = hi[i] - lo[i];
        fwrite(&lo[i], 8, 1, f), fwrite(&sz, 8, 1, f);
        fwrite((const void *)(uintptr_t)lo[i], 1, sz, f);
        total += sz;
    }
    fclose(f);
    LOG("[heapsnap] %s: %d ranges, %llu MB\n", g_path, n, (unsigned long long)(total >> 20));
    g_busy = 0;
    return NULL;
}

// each frame (the render thread): a request?
void heapsnap_frame(void) {
    static const char *live = (const char *)1;
    if (live == (const char *)1) live = getenv("LARECOMP_LIVE");
    if (!live || g_busy) return;
    static int tick;
    if (++tick % 15) return;
    char req[600];
    snprintf(req, sizeof req, "%s/heapsnap", live);
    FILE *f = fopen(req, "r");
    if (!f) return;
    char line[512] = {0};
    if (!fgets(line, sizeof line, f)) line[0] = 0;
    fclose(f);
    remove(req);
    line[strcspn(line, "\r\n")] = 0;
    if (!line[0]) return;
    snprintf(g_path, sizeof g_path, "%s", line);
    g_busy = 1;
    pthread_t t;
    if (pthread_create(&t, NULL, snap_thread, NULL)) { g_busy = 0; return; }
    pthread_detach(t);
}
