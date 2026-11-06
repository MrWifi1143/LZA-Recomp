// Research (LARECOMP_PLA_CMDPOOL=1): the game's GPU memory sub-allocator (alloc 0x7002a919bc, free 0x7002a91f40)
// counted per pool - what is outstanding every 5 s. A command memory pool that ran dry crashed Arceus after the
// Commander scene (0x7002a8d6ec: the allocation failed, the caller reads block+0x30).
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <pthread.h>
#include "../rt.h"

void rt_register_fnhook_at(uint64_t addr, int (*fn)(Cpu *c, uint64_t addr));
void rt_call_original(Cpu *c, uint64_t addr);
uint64_t plat_now_ns(void);

#define NB 131072
static struct { uint64_t blk, owner, size, lr; } g_b[NB];
static struct { uint64_t owner, n, bytes, fails, allocs; } g_o[32];
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

static int owner_slot(uint64_t o) {
    for (int i = 0; i < 32; i++) if (g_o[i].owner == o) return i;
    for (int i = 0; i < 32; i++) if (!g_o[i].owner) {
        g_o[i].owner = o;
        const uint64_t *q = (const uint64_t *)(uintptr_t)(o - 0x68);  // (the object holding it: x0 - 0x68)
        for (int k = 0; k < 64; k += 4)
            LOG("[cmdpool] %llx+%03x: %016llx %016llx %016llx %016llx\n", (unsigned long long)(o - 0x68), k * 8,
                (unsigned long long)q[k], (unsigned long long)q[k + 1], (unsigned long long)q[k + 2], (unsigned long long)q[k + 3]);
        return i;
    }
    return -1;
}
static void report(void) {
    static uint64_t last;
    uint64_t now = plat_now_ns();
    if (now - last < 5000000000ull) return;
    last = now;
    {   // the outstanding blocks by the code that asked for them (top callers, every pool)
        static struct { uint64_t lr, owner, n; } t[64];
        int nt = 0;
        for (int i = 0; i < NB; i++) {
            if (!g_b[i].blk) continue;
            int k = 0;
            while (k < nt && (t[k].lr != g_b[i].lr || t[k].owner != g_b[i].owner)) k++;
            if (k == nt && nt < 64) t[nt].lr = g_b[i].lr, t[nt].owner = g_b[i].owner, t[nt].n = 0, nt++;
            if (k < nt) t[k].n++;
        }
        for (int k = 0; k < nt; k++)
            if (t[k].n >= 50) LOG("[cmdpool]   %llu outstanding from lr %llx (pool %llx)\n", (unsigned long long)t[k].n,
                                  (unsigned long long)t[k].lr, (unsigned long long)t[k].owner);
    }
    for (int i = 0; i < 32 && g_o[i].owner; i++)
        LOG("[cmdpool] pool %llx: %llu blocks %llu KB outstanding, %llu allocs, %llu failed\n", (unsigned long long)g_o[i].owner,
            (unsigned long long)g_o[i].n, (unsigned long long)(g_o[i].bytes >> 10), (unsigned long long)g_o[i].allocs,
            (unsigned long long)g_o[i].fails);
}
static int alloc_hook(Cpu *c, uint64_t addr) {
    uint64_t owner = c->x[0], size = c->x[1], lr = c->x[30];
    rt_call_original(c, addr);
    uint64_t blk = c->x[0];
    pthread_mutex_lock(&g_lock);
    int s = owner_slot(owner);
    if (s >= 0) {
        g_o[s].allocs++;
        if (!blk) {
            g_o[s].fails++;
            LOG("[cmdpool] pool %llx FULL: %llu bytes asked with %llu blocks %llu KB outstanding\n", (unsigned long long)owner,
                (unsigned long long)size, (unsigned long long)g_o[s].n, (unsigned long long)(g_o[s].bytes >> 10));
        } else {
            g_o[s].n++, g_o[s].bytes += size;
            for (uint64_t i = (blk >> 4) % NB, k = 0; k < NB; k++, i = (i + 1) % NB)
                if (!g_b[i].blk) { g_b[i].blk = blk, g_b[i].owner = owner, g_b[i].size = size, g_b[i].lr = lr; break; }
        }
    }
    report();
    pthread_mutex_unlock(&g_lock);
    return 1;
}
static int free_hook(Cpu *c, uint64_t addr) {
    uint64_t blk = c->x[1];
    pthread_mutex_lock(&g_lock);
    for (uint64_t i = (blk >> 4) % NB, k = 0; blk && k < NB && g_b[i].blk; k++, i = (i + 1) % NB)
        if (g_b[i].blk == blk) {
            int s = owner_slot(g_b[i].owner);
            if (s >= 0) g_o[s].n--, g_o[s].bytes -= g_b[i].size;
            // (removal from open addressing: re-insert the rest of the run)
            g_b[i].blk = 0;
            for (uint64_t j = (i + 1) % NB; g_b[j].blk; j = (j + 1) % NB) {
                typeof(g_b[0]) e = g_b[j];
                g_b[j].blk = 0;
                for (uint64_t t = (e.blk >> 4) % NB;; t = (t + 1) % NB) if (!g_b[t].blk) { g_b[t] = e; break; }
            }
            break;
        }
    pthread_mutex_unlock(&g_lock);
    rt_call_original(c, addr);
    return 1;
}
// The graphics setup (0x70029d6a64) gets the three GPU memory pools' sizes from 0x700277d798 (config +0x28: 300 MB,
// +0x30: 250 MB, +0x38: 40 MB - command memory). Our command records are several times NVN's own, so 40 MB ran dry in
// busy scenes (after the Commander / the photo: the crash above). LARECOMP_PLA_CMDMEM_MB overrides (40 = the game's).
static uint32_t g_cmdmem_mb = 280;  // (7x: what the heap window has room for - 256 MB above the 2.75 GB block)
static int gfx_setup_hook(Cpu *c, uint64_t addr) {
    (void)addr;
    uint32_t *sz = (uint32_t *)(uintptr_t)(c->x[0] + 0x38);
    if (*sz == 0x2800000u && g_cmdmem_mb != 40) {
        *sz = g_cmdmem_mb << 20;
        LOG("[pla] command memory pool: %u MB (the game's 40)\n", g_cmdmem_mb);
    }
    return 0;
}
void pla_cmdpool_configure(void) {
    {
        const char *e = getenv("LARECOMP_PLA_CMDMEM_MB");
        if (e && atoi(e) >= 40 && atoi(e) <= 296) g_cmdmem_mb = (uint32_t)atoi(e);
        extern uint64_t g_stdheap_size;  // (hle/heaps.c: the pool comes out of the game's StandardAllocator block)
        g_stdheap_size += (uint64_t)(g_cmdmem_mb - 40) << 20;
        rt_register_fnhook_at(0x70029d6a64ull, gfx_setup_hook);
    }
    if (!getenv("LARECOMP_PLA_CMDPOOL")) return;
    rt_register_fnhook_at(0x7002a919bcull, alloc_hook);
    rt_register_fnhook_at(0x7002a91f40ull, free_hook);
    LOG("[cmdpool] counting the GPU memory sub-allocator\n");
}
