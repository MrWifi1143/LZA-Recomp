// Research aid (LARECOMP_LIVE <dir>/x0s, "<function> [n]"): the distinct x0 (this) values the next calls of a guest
// function get, up to n, each logged once as "[x0s]" with x1 and the caller (lr) - the objects a method runs on.
#include "../hle.h"

void rt_register_fnhook_at(uint64_t addr, int (*fn)(Cpu *c, uint64_t addr));
static uint64_t g_fn, g_seen[256];
static int g_left, g_n;

static int x0s_hook(Cpu *c, uint64_t addr) {
    if (addr != g_fn || g_left <= 0) return 0;
    for (int i = 0; i < g_n; i++) if (g_seen[i] == c->x[0]) return 0;
    if (g_n < 256) g_seen[g_n++] = c->x[0];
    g_left--;
    LOG("[x0s] %llx x0 %llx x1 %llx lr %llx\n", (unsigned long long)addr, (unsigned long long)c->x[0], (unsigned long long)c->x[1],
        (unsigned long long)c->x[30]);
    return 0;
}

void x0s_start(uint64_t fn, int n) {
    g_fn = fn, g_left = n > 0 ? n : 16, g_n = 0;
    rt_register_fnhook_at(fn, x0s_hook);
    LOG("[x0s] watching %llx\n", (unsigned long long)fn);
}
