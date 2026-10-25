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

// (LARECOMP_LIVE <dir>/calls, "<function> [n]": its next n distinct calls by x0, x1 and caller, logged as "[calls]" with
// x2, x3 and two callers up the frame chain - who asks a getter for what)
static uint64_t c_fn, c_seen[1024][3];
static int c_left, c_n;

static int calls_hook(Cpu *c, uint64_t addr) {
    if (addr != c_fn || c_left <= 0) return 0;
    for (int i = 0; i < c_n; i++)
        if (c_seen[i][0] == c->x[0] && c_seen[i][1] == c->x[1] && c_seen[i][2] == c->x[30]) return 0;
    if (c_n < 1024) c_seen[c_n][0] = c->x[0], c_seen[c_n][1] = c->x[1], c_seen[c_n][2] = c->x[30], c_n++;
    c_left--;
    uint64_t fp = c->x[29], l1 = 0, l2 = 0;
    if (fp >= 0x1000) {
        l1 = ((uint64_t *)(uintptr_t)fp)[1];
        uint64_t fp2 = ((uint64_t *)(uintptr_t)fp)[0];
        if (fp2 >= 0x1000) l2 = ((uint64_t *)(uintptr_t)fp2)[1];
    }
    LOG("[calls] %llx x0 %llx x1 %llx x2 %llx x3 %llx lr %llx %llx %llx\n", (unsigned long long)addr, (unsigned long long)c->x[0],
        (unsigned long long)c->x[1], (unsigned long long)c->x[2], (unsigned long long)c->x[3], (unsigned long long)c->x[30],
        (unsigned long long)l1, (unsigned long long)l2);
    return 0;
}

void calls_start(uint64_t fn, int n) {
    c_fn = fn, c_left = n > 0 ? n : 64, c_n = 0;
    rt_register_fnhook_at(fn, calls_hook);
    LOG("[calls] watching %llx\n", (unsigned long long)fn);
}
