// Host versions of the SDK's memory and string functions. The lifted ones copy a few bytes per C statement and are
// among the hottest functions of Legends: Arceus (memcpy alone ~0.12 ms a frame); these hooks (runtime/fnhook.c, at
// the addresses the SDK exports, so its own direct calls are caught too) run libc's instead. Guest pointers are host
// pointers. LARECOMP_LIBC_FAST=0: off.
#include <string.h>

#include "../hle.h"

void rt_register_fnhook_at(uint64_t addr, int (*fn)(Cpu *c, uint64_t addr));

static int h_memcpy(Cpu *c, uint64_t a) {  // (memmove: a game that overlaps them got away with it on the console)
    (void)a;
    if (c->x[2]) memmove((void *)(uintptr_t)c->x[0], (const void *)(uintptr_t)c->x[1], c->x[2]);
    return 1;
}
static int h_memset(Cpu *c, uint64_t a) {
    (void)a;
    if (c->x[2]) memset((void *)(uintptr_t)c->x[0], (int)(uint8_t)c->x[1], c->x[2]);
    return 1;
}
static int h_memcmp(Cpu *c, uint64_t a) {
    (void)a;
    const uint8_t *p = (const uint8_t *)(uintptr_t)c->x[0], *q = (const uint8_t *)(uintptr_t)c->x[1];
    int r = 0;
    for (uint64_t i = 0, n = c->x[2]; i < n; i++)  // (the sign of the first differing byte, as the SDK's: libc's may differ in magnitude)
        if (p[i] != q[i]) { r = (int)p[i] - (int)q[i]; break; }
    c->x[0] = (uint64_t)(uint32_t)r;
    return 1;
}
static int h_strlen(Cpu *c, uint64_t a) {
    (void)a;
    c->x[0] = strlen((const char *)(uintptr_t)c->x[0]);
    return 1;
}
static int h_strcmp(Cpu *c, uint64_t a) {
    (void)a;
    const uint8_t *p = (const uint8_t *)(uintptr_t)c->x[0], *q = (const uint8_t *)(uintptr_t)c->x[1];
    while (*p && *p == *q) p++, q++;
    c->x[0] = (uint64_t)(uint32_t)((int)*p - (int)*q);
    return 1;
}
static int h_strncmp(Cpu *c, uint64_t a) {
    (void)a;
    const uint8_t *p = (const uint8_t *)(uintptr_t)c->x[0], *q = (const uint8_t *)(uintptr_t)c->x[1];
    uint64_t n = c->x[2];
    int r = 0;
    for (; n; n--, p++, q++)
        if (*p != *q || !*p) { r = (int)*p - (int)*q; break; }
    c->x[0] = (uint64_t)(uint32_t)r;
    return 1;
}

void libcfast_configure(void) {
    const char *e = getenv("LARECOMP_LIBC_FAST");
    if (e && atoi(e) == 0) return;
    static const struct { const char *name; int (*fn)(Cpu *, uint64_t); } k[] = {
        {"memcpy", h_memcpy}, {"memmove", h_memcpy}, {"memset", h_memset}, {"memcmp", h_memcmp},
        {"strlen", h_strlen}, {"strcmp", h_strcmp},  {"strncmp", h_strncmp},
    };
    int n = 0;
    for (size_t i = 0; i < sizeof k / sizeof *k; i++) {
        uint64_t addr = loader_find_symbol(k[i].name);
        if (!addr) continue;
        rt_register_fnhook_at(addr, k[i].fn);
        n++;
    }
    LOG("[libc] %d SDK memory / string functions run on the host's libc\n", n);
}
