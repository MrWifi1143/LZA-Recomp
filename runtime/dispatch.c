// Guest address -> recompiled function lookup, and host (HLE) function registry.
#include <pthread.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "gamelib.h"
#include "hle.h"

typedef struct { const uint64_t *addrs; const GuestFn *fns; uint64_t count, lo, hi; } FnTable;
static FnTable tables[GAMELIB_MAX_MODULES];
static int ntables;

// HLE stubs live at fake addresses HLE_BASE + 16*i
#define MAX_HLE 4096
typedef struct { const char *name; HostFn fn; } HleSlot;
static HleSlot hle_slots[MAX_HLE];
static int hle_count;

// Overrides of recompiled guest functions (by address)
#define MAX_OVERRIDE 1024
static struct { uint64_t addr; HostFn fn; const char *name; } overrides[MAX_OVERRIDE];
static int override_count;

// Host implementations registered by name (runtime/hle/*.c fill this via HLE_FUNC)
typedef struct { const char *name; HostFn fn; } HleImpl;
#ifdef __APPLE__
extern const HleImpl __start_hle_impls[] __asm__("section$start$__DATA$__hle_impls");
extern const HleImpl __stop_hle_impls[] __asm__("section$end$__DATA$__hle_impls");
#elif defined(__linux__)
extern const HleImpl __start_hle_impls[], __stop_hle_impls[];  // (ELF: defined by the linker)
#else
// PE/COFF: the linker sorts .hle$a < .hle$m (entries, hle.h) < .hle$z by the text after '$'.
__attribute__((section(".hle$a"), used)) static const HleImpl __start_hle_impls[1];
__attribute__((section(".hle$z"), used)) static const HleImpl __stop_hle_impls[1];
#endif

HostFn hle_lookup_impl(const char *name) {
    const char *skip = getenv("LARECOMP_HLE_SKIP");  // debugging: disable HLE for names containing this
    if (skip && strstr(name, skip)) return NULL;
    const HleImpl *start = __start_hle_impls, *stop = __stop_hle_impls;
    __asm__("" : "+r"(start), "+r"(stop));  // separate objects: keep the compiler from reasoning about them
    for (const HleImpl *p = start; p < stop; p++)
        if (p->name && !strcmp(p->name, name)) return p->fn;
    return NULL;
}

static void hle_unimplemented(Cpu *c);

uint64_t hle_register(const char *name, HostFn fn) {
    for (int i = 0; i < hle_count; i++)
        if (!strcmp(hle_slots[i].name, name)) return HLE_BASE + 16ull * i;
    if (hle_count == MAX_HLE) FATAL("too many HLE stubs\n");
    hle_slots[hle_count].name = strdup(name);
    hle_slots[hle_count].fn = fn;
    {   // LARECOMP_HLE_WHO=<fake address, hex>[,...]: names the host function behind an address of a thread dump
        const char *who = getenv("LARECOMP_HLE_WHO");
        char want[32];
        snprintf(want, sizeof want, "%llx", (unsigned long long)(HLE_BASE + 16ull * hle_count));
        if (who && strstr(who, want)) LOG("[hle] %s = %s%s\n", want, name, fn ? "" : " (no host implementation)");
    }
    return HLE_BASE + 16ull * hle_count++;
}

void hle_override(uint64_t addr, HostFn fn, const char *name) {
    if (override_count == MAX_OVERRIDE) FATAL("too many overrides\n");
    overrides[override_count].addr = addr;
    overrides[override_count].fn = fn;
    overrides[override_count].name = name;
    override_count++;
}

// ---- recent-HLE ring (crash debugging: what the game asked the host just before dying)
#define HHIST 128
static __thread struct { const char *name; uint64_t x0, x1, x30; } hhist[HHIST];
static __thread int hhist_n;
static void hhist_push(const char *name, Cpu *c) {
    int i = hhist_n++ & (HHIST - 1);
    hhist[i].name = name;
    hhist[i].x0 = c->x[0];
    hhist[i].x1 = c->x[1];
    hhist[i].x30 = c->x[30];
}
static void hhist_dump(void) {
    int n = hhist_n < HHIST ? hhist_n : HHIST, start = hhist_n - n;
    fprintf(stderr, "last %d host calls:\n", n);
    for (int k = 0; k < n; k++) {
        int i = (start + k) & (HHIST - 1);
        fprintf(stderr, "  %s x0=%llx x1=%llx lr=%llx\n", hhist[i].name ? hhist[i].name : "?",
                (unsigned long long)hhist[i].x0, (unsigned long long)hhist[i].x1, (unsigned long long)hhist[i].x30);
    }
}

static __thread uint64_t unimpl_pc;
static int unimpl_counts[MAX_HLE];
static void hle_unimplemented(Cpu *c) {
    int i = (int)((unimpl_pc - HLE_BASE) / 16);
    if (unimpl_counts[i]++ < 3)
        LOG("[hle] UNIMPLEMENTED %s (x0=%llx x1=%llx lr=%llx) -> returning 0\n", hle_slots[i].name, c->x[0],
            c->x[1], c->x[30]);
    c->x[0] = 0;
}

uint64_t guest_sym(const char *name) {
    // small cache: callers pass string literals, so pointer identity is a good key
    static struct { const char *name; uint64_t addr; } cache[64];
    static pthread_mutex_t m = PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_lock(&m);
    for (int i = 0; i < 64 && cache[i].name; i++)
        if (cache[i].name == name) { uint64_t a = cache[i].addr; pthread_mutex_unlock(&m); return a; }
    pthread_mutex_unlock(&m);
    uint64_t a = loader_find_symbol(name);
    if (!a) FATAL("guest symbol %s not found\n", name);
    pthread_mutex_lock(&m);
    for (int i = 0; i < 64; i++) if (!cache[i].name) { cache[i].name = name; cache[i].addr = a; break; }
    pthread_mutex_unlock(&m);
    return a;
}

static uint64_t hle_next = HLE_HEAP;
RT_ON_RESET(hle_heap_reset) { hle_next = HLE_HEAP; }  // (guest memory, wiped by the reboot)

void *hle_alloc(uint64_t size) {
    static pthread_mutex_t m = PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_lock(&m);
    uint64_t a = hle_next;
    hle_next += (size + 63) & ~63ull;
    if (hle_next > HLE_HEAP + HLE_HEAP_SIZE) FATAL("HLE heap exhausted\n");
    pthread_mutex_unlock(&m);
    plat_commit(a, size);
    memset((void *)a, 0, size);
    return (void *)a;
}

void dispatch_init(void) {  // (after gamelib_load)
    const GameLibTables *g = gamelib_tables();
    ntables = 0;
    for (uint32_t i = 0; i < g->nmods && i < GAMELIB_MAX_MODULES; i++) {
        const GameLibModule *m = &g->mods[i];
        uint64_t n = *m->count;
        if (n) tables[ntables++] = (FnTable){m->addrs, m->fns, n, m->addrs[0], m->addrs[n - 1]};
    }
}

GuestFn dispatch_lookup(uint64_t addr) {
    for (int t = 0; t < ntables; t++) {
        FnTable *ft = &tables[t];
        if (addr < ft->lo || addr > ft->hi) continue;
        uint64_t lo = 0, hi = ft->count;
        while (lo < hi) {
            uint64_t mid = (lo + hi) / 2;
            if (ft->addrs[mid] < addr) lo = mid + 1;
            else hi = mid;
        }
        if (lo < ft->count && ft->addrs[lo] == addr) return ft->fns[lo];
    }
    return NULL;
}

// ---- guest setjmp / longjmp. The generated code takes a host __builtin_setjmp at every guest setjmp call
// (tools/gen.py), keyed by the guest return address and sp. The guest's own longjmp restores the guest registers and
// branches to that return address, which is no function: dispatch_slow finds the point and longjmps the host back into
// the generated function that made it. Per thread; a point whose sp is below the caller's belongs to a frame that has
// returned and is reused first.
#ifdef _WIN32
typedef struct { uint64_t ret, sp; void *buf[30]; } JmpPoint;  // (rt_sj_set below: registers, xmm6-15, sp, return address)
// Win64 setjmp / longjmp without unwinding: the callee-saved registers (rbx rbp rdi rsi r12-r15, xmm6-xmm15), the stack
// pointer and the return address (cpu.h: LLVM's __builtin_setjmp is broken on this target).
__attribute__((naked, returns_twice)) int rt_sj_set(void **buf) {
    __asm__ volatile(
        "movq %rbx, 0(%rcx)\n\t" "movq %rbp, 8(%rcx)\n\t" "movq %rdi, 16(%rcx)\n\t" "movq %rsi, 24(%rcx)\n\t"
        "movq %r12, 32(%rcx)\n\t" "movq %r13, 40(%rcx)\n\t" "movq %r14, 48(%rcx)\n\t" "movq %r15, 56(%rcx)\n\t"
        "leaq 8(%rsp), %rdx\n\t" "movq %rdx, 64(%rcx)\n\t" "movq (%rsp), %rdx\n\t" "movq %rdx, 72(%rcx)\n\t"
        "movdqu %xmm6, 80(%rcx)\n\t" "movdqu %xmm7, 96(%rcx)\n\t" "movdqu %xmm8, 112(%rcx)\n\t" "movdqu %xmm9, 128(%rcx)\n\t"
        "movdqu %xmm10, 144(%rcx)\n\t" "movdqu %xmm11, 160(%rcx)\n\t" "movdqu %xmm12, 176(%rcx)\n\t"
        "movdqu %xmm13, 192(%rcx)\n\t" "movdqu %xmm14, 208(%rcx)\n\t" "movdqu %xmm15, 224(%rcx)\n\t"
        "xorl %eax, %eax\n\t" "ret\n\t");
}
__attribute__((naked, noreturn)) void rt_sj_long(void **buf, int v) {
    __asm__ volatile(
        "movl %edx, %eax\n\t" "testl %eax, %eax\n\t" "jnz 1f\n\t" "movl $1, %eax\n\t" "1:\n\t"
        "movq 0(%rcx), %rbx\n\t" "movq 8(%rcx), %rbp\n\t" "movq 16(%rcx), %rdi\n\t" "movq 24(%rcx), %rsi\n\t"
        "movq 32(%rcx), %r12\n\t" "movq 40(%rcx), %r13\n\t" "movq 48(%rcx), %r14\n\t" "movq 56(%rcx), %r15\n\t"
        "movdqu 80(%rcx), %xmm6\n\t" "movdqu 96(%rcx), %xmm7\n\t" "movdqu 112(%rcx), %xmm8\n\t" "movdqu 128(%rcx), %xmm9\n\t"
        "movdqu 144(%rcx), %xmm10\n\t" "movdqu 160(%rcx), %xmm11\n\t" "movdqu 176(%rcx), %xmm12\n\t"
        "movdqu 192(%rcx), %xmm13\n\t" "movdqu 208(%rcx), %xmm14\n\t" "movdqu 224(%rcx), %xmm15\n\t"
        "movq 64(%rcx), %rsp\n\t" "jmpq *72(%rcx)\n\t");
}
#elif defined(__aarch64__)
typedef struct { uint64_t ret, sp; void *buf[30]; } JmpPoint;  // (rt_sj_set below: x19-x30, sp, d8-d15)
// AArch64 setjmp / longjmp without unwinding (cpu.h: LLVM has no __builtin_longjmp on this target): the callee-saved
// registers x19-x28, the frame pointer and the return address (x29, x30), sp, and d8-d15 (AAPCS64)
__attribute__((naked, returns_twice)) int rt_sj_set(void **buf) {
    __asm__ volatile(
        "stp x19, x20, [x0, #0]\n\t" "stp x21, x22, [x0, #16]\n\t" "stp x23, x24, [x0, #32]\n\t"
        "stp x25, x26, [x0, #48]\n\t" "stp x27, x28, [x0, #64]\n\t" "stp x29, x30, [x0, #80]\n\t"
        "mov x2, sp\n\t" "str x2, [x0, #96]\n\t"
        "stp d8, d9, [x0, #104]\n\t" "stp d10, d11, [x0, #120]\n\t" "stp d12, d13, [x0, #136]\n\t"
        "stp d14, d15, [x0, #152]\n\t"
        "mov w0, #0\n\t" "ret\n\t");
}
__attribute__((naked, noreturn)) void rt_sj_long(void **buf, int v) {
    __asm__ volatile(
        "ldp x19, x20, [x0, #0]\n\t" "ldp x21, x22, [x0, #16]\n\t" "ldp x23, x24, [x0, #32]\n\t"
        "ldp x25, x26, [x0, #48]\n\t" "ldp x27, x28, [x0, #64]\n\t" "ldp x29, x30, [x0, #80]\n\t"
        "ldr x2, [x0, #96]\n\t" "mov sp, x2\n\t"
        "ldp d8, d9, [x0, #104]\n\t" "ldp d10, d11, [x0, #120]\n\t" "ldp d12, d13, [x0, #136]\n\t"
        "ldp d14, d15, [x0, #152]\n\t"
        "cmp w1, #0\n\t" "csinc w0, w1, wzr, ne\n\t"  // (longjmp(b, 0) returns 1)
        "br x30\n\t");
}
#else
typedef struct { uint64_t ret, sp; void *buf[5]; } JmpPoint;
#endif
#define NJMP 256
static __thread JmpPoint jmp_points[NJMP];
static __thread unsigned jmp_next;

void **rt_setjmp_point(Cpu *c, uint64_t ret) {
    JmpPoint *free_p = NULL;
    for (unsigned i = 0; i < NJMP; i++) {
        JmpPoint *p = &jmp_points[i];
        if (p->ret == ret && p->sp == c->sp) return p->buf;
        if (!free_p && (!p->ret || p->sp < c->sp)) free_p = p;
    }
    if (!free_p) free_p = &jmp_points[jmp_next++ % NJMP];
    free_p->ret = ret;
    free_p->sp = c->sp;
    return free_p->buf;
}

static void setjmp_resume(Cpu *c) {
    for (unsigned i = 0; i < NJMP; i++)
        if (jmp_points[i].ret == c->pc && jmp_points[i].sp == c->sp) __builtin_longjmp(jmp_points[i].buf, 1);
}

// Small direct-mapped cache in front of the binary search
#define DCACHE 4096
static __thread struct { uint64_t addr; GuestFn fn; } dcache[DCACHE];

static GuestFn dispatch_slow(Cpu *c) {
    uint64_t pc = c->pc;
    kernel_core_reacquire();  // (after host code that blocked: no lock is held here)
    if (pc >= HLE_BASE && pc < HLE_BASE + HLE_SIZE) {
        rt_unwind_point();  // (HLE calls aren't cached below, so every one passes here: a reboot's safe point)
        int i = (int)((pc - HLE_BASE) / 16);
        if (i < hle_count) {
            hhist_push(hle_slots[i].name, c);
            if (hle_slots[i].fn) return hle_slots[i].fn;
            unimpl_pc = pc;
            return hle_unimplemented;
        }
    }
    for (int i = 0; i < override_count; i++)
        if (overrides[i].addr == pc) return overrides[i].fn;
    GuestFn f = dispatch_lookup(pc);
    if (!f) {
        setjmp_resume(c);  // (returns only if no guest setjmp point matches)
        uint64_t off;
        const char *s = loader_symbol_at(c->x[30], &off);
        FATAL("no function at %llx (lr=%llx %s+%llx)\n", pc, c->x[30], s ? s : "?", off);
    }
    unsigned h = (unsigned)(pc >> 2) & (DCACHE - 1);
    dcache[h].addr = pc;
    dcache[h].fn = f;
    return f;
}

// Always tail-calls, so chains of indirect jumps don't grow the host stack.
void rt_dispatch(Cpu *c) {
    unsigned h = (unsigned)(c->pc >> 2) & (DCACHE - 1);
    GuestFn f = dcache[h].addr == c->pc ? dcache[h].fn : dispatch_slow(c);
    __attribute__((musttail)) return f(c);
}

void guest_call(Cpu *c, uint64_t addr) {
    uint64_t lr = c->x[30];
    c->x[30] = 0;
    c->pc = addr;
    rt_dispatch(c);
    c->x[30] = lr;
}

// Host pc -> recompiled guest function containing it ("f_<addr>"), for hosts without symbol lookup.
static int cmp_host(const void *a, const void *b) {
    uint64_t x = ((const uint64_t *)a)[0], y = ((const uint64_t *)b)[0];
    return x < y ? -1 : x > y;
}
const char *rt_host_fn_name(uint64_t pc, uint64_t *offset) {
    static uint64_t (*byhost)[2];  // {host address, guest address}, sorted by host address
    static uint64_t n;
    static __thread char name[32];
    if (!byhost) {
        uint64_t total = 0, k = 0;
        for (int t = 0; t < ntables; t++) total += tables[t].count;
        uint64_t (*v)[2] = malloc(total * sizeof *v);
        if (!v) return NULL;
        for (int t = 0; t < ntables; t++)
            for (uint64_t i = 0; i < tables[t].count; i++) {
                v[k][0] = (uint64_t)(uintptr_t)tables[t].fns[i];
                v[k++][1] = tables[t].addrs[i];
            }
        qsort(v, k, sizeof *v, cmp_host);
        n = k;
        byhost = v;
    }
    uint64_t lo = 0, hi = n;  // last entry with host address <= pc
    while (lo < hi) {
        uint64_t mid = (lo + hi) / 2;
        if (byhost[mid][0] <= pc) lo = mid + 1;
        else hi = mid;
    }
    if (!lo || pc - byhost[lo - 1][0] > 0x100000) return NULL;
    snprintf(name, sizeof name, "f_%llx", (unsigned long long)byhost[lo - 1][1]);
    if (offset) *offset = pc - byhost[lo - 1][0];
    return name;
}

static const char *host_sym(uint64_t pc, uint64_t *offset) {
    // (game functions first: they live in lasgame.dll, which the OS lookup can only name as a whole)
    const char *s = rt_host_fn_name(pc, offset);
    return s ? s : plat_symbol(pc, offset);
}

void rt_fatal_trace(void) {
    hhist_dump();
    fprintf(stderr, "host backtrace (guest functions are f_<addr>):\n");
    plat_print_backtrace();
    Cpu *c = kernel_current_cpu();
    if (c) {
        fprintf(stderr, "guest: sp=%llx lr=%llx\n", c->sp, c->x[30]);
        for (int i = 0; i < 31; i += 4)
            fprintf(stderr, "  x%-2d %016llx  x%-2d %016llx  x%-2d %016llx  x%-2d %016llx\n", i, c->x[i], i + 1,
                    c->x[i + 1], i + 2, c->x[i + 2], i + 3, c->x[i + 3]);
        // guest call stack: the lifted code keeps the guest's frame records (x29 -> {x29, x30}) on the guest stack
        fprintf(stderr, "guest backtrace (frame pointers):\n");
        uint64_t fp = c->x[29], lr = c->x[30];
        for (int i = 0; i < 24; i++) {
            uint64_t off = 0;
            const char *s = loader_symbol_at(lr, &off);
            fprintf(stderr, "  #%-2d %llx %s+%llx\n", i, (unsigned long long)lr, s ? s : "?", (unsigned long long)off);
            if (fp < GUEST_LO || fp >= GUEST_HI || (fp & 7)) break;
            lr = ((uint64_t *)(uintptr_t)fp)[1];
            fp = ((uint64_t *)(uintptr_t)fp)[0];
            if (!lr) break;
        }
    }
    plat_fatal_notice();
    // our crash handler also catches SIGABRT: without this abort() re-entered it forever (a 5 GB log)
    signal(SIGABRT, SIG_DFL);
    abort();
}

void rt_udf(Cpu *c, uint64_t pc) {
    (void)c;
    FATAL("undefined instruction / trap at %llx\n", pc);
}

void rt_brk(Cpu *c, uint32_t imm, uint64_t pc) {
    (void)c;
    FATAL("brk #%u at %llx\n", imm, pc);
}

// ---- crash reporting

// Debug data watchpoint: the page is protected; accesses in [lo, hi) are attributed to the host
// function (f_<guest addr>) doing them, then the page is reopened until the next re-arm.
uint64_t g_watch_lo, g_watch_hi;
static uint64_t watch_page;
static void *watch_thread(void *a) {
    (void)a;
    int r = plat_protect(watch_page, PLAT_PAGE, PLAT_PROT_NONE);
    fprintf(stderr, "[watch] page %llx protect -> %d\n", (unsigned long long)watch_page, r);
    for (;;) { plat_protect(watch_page, PLAT_PAGE, PLAT_PROT_NONE); usleep(100); }
    return NULL;
}
void watch_arm(void) {
    if (!g_watch_lo || watch_page) return;
    watch_page = g_watch_lo & ~(PLAT_PAGE - 1);
    pthread_t t;
    pthread_create(&t, NULL, watch_thread, NULL);
}

bool rt_fault_watch(uint64_t fa, uint64_t pc) {
    if (!watch_page || fa < watch_page || fa >= watch_page + PLAT_PAGE) return false;
    if (fa >= g_watch_lo && fa < g_watch_hi) {
        static char seen[64][48];
        static int nseen;
        const char *nm = host_sym(pc, NULL);
        if (!nm) nm = "?";
        char key[48];  // (each function + offset once)
        snprintf(key, sizeof key, "%.36s+%llx", nm, (unsigned long long)(fa - g_watch_lo));
        bool found = false;
        for (int i = 0; i < nseen; i++) found |= !strcmp(seen[i], key);
        if (!found && nseen < 64) {
            snprintf(seen[nseen++], 48, "%s", key);
            Cpu *gc = kernel_current_cpu();  // (and the guest's view: a memcpy's caller and source)
            if (gc)
                fprintf(stderr, "[watch] +%#llx by %s (guest lr %llx x0 %llx x1 %llx x2 %llx fp-lr %llx)\n", fa - g_watch_lo, nm,
                        (unsigned long long)gc->x[30], (unsigned long long)gc->x[0], (unsigned long long)gc->x[1],
                        (unsigned long long)gc->x[2],
                        (unsigned long long)(gc->x[29] >= GUEST_LO && gc->x[29] < GUEST_HI ? ((uint64_t *)(uintptr_t)gc->x[29])[1] : 0));
            else
                fprintf(stderr, "[watch] +%#llx by %s\n", fa - g_watch_lo, nm);
        }
    }
    plat_protect(watch_page, PLAT_PAGE, PLAT_PROT_RW);
    return true;
}

void rt_fault_report(const char *what, uint64_t fa, uint64_t pc, uint64_t lr) {
    uint64_t po = 0, lo = 0;
    const char *ps = host_sym(pc, &po), *ls = host_sym(lr, &lo);
    fprintf(stderr, "\n*** %s accessing %#llx\n    host pc %llx (%s+%llx)\n    host lr %llx (%s)\n", what,
            (unsigned long long)fa, (unsigned long long)pc, ps ? ps : "?", (unsigned long long)po,
            (unsigned long long)lr, ls ? ls : "?");
    rt_fatal_trace();
}

void crash_handler_install(void) { plat_crash_handler_install(); }
