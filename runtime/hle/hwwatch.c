// Research aid: hardware data watchpoints (Linux perf_event_open breakpoints, the CPU's debug registers) - who writes a few
// bytes of guest memory, without protecting pages (memwatch.c / dispatch.c watch_arm protect a whole page: on a page that
// also holds lock words the game then stops). One breakpoint per thread and address, created for the threads running
// when armed; each hit raises a real-time signal on the writing thread, whose handler names the host function (the guest
// function, f_<address>) with the guest's registers. Each writer is logged once ("[hwwatch]").
//  hwwatch_arm(addr, len): len 1, 2, 4 or 8, addr aligned to it; up to 4 per thread (the debug registers)
#ifdef __linux__
#define _GNU_SOURCE
#include <dirent.h>
#include <fcntl.h>
#include <linux/hw_breakpoint.h>
#include <linux/perf_event.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <ucontext.h>
#include <unistd.h>

#include "../rt.h"

const char *rt_host_fn_name(uint64_t pc, uint64_t *offset);  // runtime/dispatch.c

#define HW_SIG (SIGRTMIN + 5)
static uint64_t g_lo[4], g_len[4];
static int g_nw;

static void on_hit(int sig, siginfo_t *si, void *ctx) {
    (void)sig;
    (void)si;
    ucontext_t *uc = ctx;
    uint64_t pc = (uint64_t)uc->uc_mcontext.gregs[REG_RIP], off = 0;
    const char *nm = rt_host_fn_name(pc, &off);
    static char seen[96][64];
    static int nseen;
    char key[64];
    snprintf(key, sizeof key, "%.40s+%llx", nm ? nm : "?", (unsigned long long)off);
    for (int i = 0; i < nseen; i++) if (!strcmp(seen[i], key)) return;
    if (nseen < 96) snprintf(seen[nseen++], 64, "%s", key);
    Cpu *c = kernel_current_cpu();
    fprintf(stderr, "[hwwatch] write by %s (host pc %llx)", key, (unsigned long long)pc);
    if (c)
        fprintf(stderr, " guest lr %llx x0 %llx x1 %llx x8 %llx x19 %llx x20 %llx", (unsigned long long)c->x[30], (unsigned long long)c->x[0],
                (unsigned long long)c->x[1], (unsigned long long)c->x[8], (unsigned long long)c->x[19], (unsigned long long)c->x[20]);
    fprintf(stderr, "\n");
}

static int bp_open(pid_t tid, uint64_t addr, uint64_t len) {
    struct perf_event_attr a;
    memset(&a, 0, sizeof a);
    a.type = PERF_TYPE_BREAKPOINT;
    a.size = sizeof a;
    a.bp_type = HW_BREAKPOINT_W;
    a.bp_addr = addr;
    a.bp_len = len;
    a.sample_period = 1;
    a.wakeup_events = 1;
    a.exclude_kernel = 1;
    a.exclude_hv = 1;
    a.disabled = 1;
    int fd = (int)syscall(SYS_perf_event_open, &a, tid, -1, -1, 0);
    if (fd < 0) return -1;
    struct f_owner_ex own = {F_OWNER_TID, tid};
    fcntl(fd, F_SETFL, O_ASYNC);
    fcntl(fd, F_SETSIG, HW_SIG);
    fcntl(fd, F_SETOWN_EX, &own);
    ioctl(fd, PERF_EVENT_IOC_ENABLE, 0);
    return fd;
}

static int g_fds[4][256], g_nfd[4];
void hwwatch_reset(void) {  // (the watchpoints armed so far closed: room for 4 new ones)
    for (int w = 0; w < g_nw; w++)
        for (int i = 0; i < g_nfd[w]; i++) close(g_fds[w][i]);
    memset(g_nfd, 0, sizeof g_nfd);
    g_nw = 0;
}
void hwwatch_arm(uint64_t addr, uint64_t len) {
    if (g_nw >= 4) return;
    static int installed;
    if (!installed) {
        installed = 1;
        struct sigaction sa;
        memset(&sa, 0, sizeof sa);
        sa.sa_sigaction = on_hit;
        sa.sa_flags = SA_SIGINFO | SA_RESTART;
        sigaction(HW_SIG, &sa, NULL);
    }
    g_lo[g_nw] = addr, g_len[g_nw] = len, g_nw++;
    DIR *d = opendir("/proc/self/task");
    int n = 0, fail = 0;
    struct dirent *e;
    while (d && (e = readdir(d))) {
        if (e->d_name[0] < '0' || e->d_name[0] > '9') continue;
        int fd = bp_open((pid_t)atoi(e->d_name), addr, len);
        if (fd >= 0) { n++; if (g_nfd[g_nw - 1] < 256) g_fds[g_nw - 1][g_nfd[g_nw - 1]++] = fd; }
        else fail++;
    }
    if (d) closedir(d);
    fprintf(stderr, "[hwwatch] %llx+%llu on %d thread(s) (%d failed)\n", (unsigned long long)addr, (unsigned long long)len, n, fail);
}

// The watchpoints armed so far, on the calling thread too (one started after they were armed: kernel.c thread_main)
void hwwatch_arm_self(void) {
    for (int w = 0; w < g_nw; w++) {
        int fd = bp_open((pid_t)syscall(SYS_gettid), g_lo[w], g_len[w]);
        if (fd >= 0 && g_nfd[w] < 256) g_fds[w][g_nfd[w]++] = fd;
    }
}

// LARECOMP_HWWATCH_SRC=<memcpy>,<dst>,<off>: the first time the guest's memcpy (its address) copies to dst, watch what
// it copies from (+off, 3 words of 8 bytes): data that a buffer on its way to dst got from (a per-frame ring is reused)
static uint64_t src_memcpy, src_dst, src_off;
static int src_hook(Cpu *c, uint64_t addr) {
    (void)addr;
    if (c->x[0] == src_dst && src_dst) {
        uint64_t s = c->x[1] + src_off;
        fprintf(stderr, "[hwwatch] memcpy to %llx from %llx (lr %llx), watching the source\n", (unsigned long long)src_dst,
                (unsigned long long)c->x[1], (unsigned long long)c->x[30]);
        src_dst = 0;
        hwwatch_arm(s & ~7ull, 8), hwwatch_arm((s & ~7ull) + 8, 8), hwwatch_arm((s & ~7ull) + 16, 8);
    }
    return 0;
}
void hwwatch_src_arm(void) {
    const char *e = getenv("LARECOMP_HWWATCH_SRC");
    if (!e || src_memcpy) return;
    sscanf(e, "%llx,%llx,%llx", (unsigned long long *)&src_memcpy, (unsigned long long *)&src_dst, (unsigned long long *)&src_off);
    void rt_register_fnhook_at(uint64_t addr, int (*fn)(Cpu *, uint64_t));
    rt_register_fnhook_at(src_memcpy, src_hook);
}
#else
#include <stdint.h>
void hwwatch_arm(uint64_t addr, uint64_t len) { (void)addr, (void)len; }
void hwwatch_reset(void) {}
void hwwatch_arm_self(void) {}
void hwwatch_src_arm(void) {}
#endif
