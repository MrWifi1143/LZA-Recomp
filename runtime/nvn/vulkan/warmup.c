// Shader warm-up: before the first boot (and after an update of the runtime) every shader program the game's files hold is
// translated and compiled to SPIR-V up front, behind a progress screen (runtime/ui/launcher_lg.mm), so the game doesn't
// stop to compile them - or draw without them - the first time each appears.
//
// Programs live in BNSH shader containers. In one, each stage is a record {control, code, code size, control size}; a
// program's vertex record is followed 0x40 bytes later by its fragment record (checked over every container of Legends:
// Arceus - 13760 pairs, 9 vertex-only - and the ones Z-A loads - 14453 and 96). The pointers are offsets in the files and
// absolute addresses once the game has loaded a container (it relocates them in place): both are read here.
//  - At the start: the containers in the romfs (loose .bnsh, embedded in .ptcl / .arc; PLA: 8427 programs, found in
//    1 s) and the ones captured earlier (below).
//  - While playing: the first time the game sets up a program from a container we haven't seen (objects.c set_shaders ->
//    warmup_capture), the container is found around the program's control section in guest memory, saved to
//    cache/warm/containers/ and all its programs are compiled by one low-priority thread while the game loads. Z-A keeps
//    every container in compressed archives (the start's scan finds none there), so this is how its shaders get here;
//    the next warm-up (after an update) includes them.
// Each program is translated as exec.c translate_program would with the key most draws have (float vertex attributes and
// render targets, the window origin and depth mode both Legends games use, no transform feedback) and its GLSL compiled with
// vk_glsl_to_spirv (exec.c vk_warm_program): cache/xlat and cache/spirv (keyed by the GLSL text) then have what the draws
// look up. A draw whose key differs (an integer render target, transform feedback) still compiles at first use.
// The pipelines themselves are not built (their state is the draws'): on a fresh install they remain the larger cost (NVIDIA:
// ~45 ms each with an empty driver shader cache). Building each program into a pipeline of a typical state only partly
// warms the driver for the draws' own (LARECOMP_WARMUP_PIPELINES=1: a much longer wait).
// PLA, fresh caches, walk test: GLSL -> SPIR-V during play 2.25 s -> 0.2 s, draws skipped while compiling 564 -> ~230.
// Done once per translator build and game: cache/warm/<game id>. LARECOMP_WARMUP=0: none of it; =1: also with --play.
#include <dirent.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

#include "../../rt.h"
#include "../maxwell/shader.h"
#include "shaders.h"

bool vk_warm_program(const uint8_t *vs, size_t vmax, size_t vcn, const uint8_t *fs, size_t fmax, size_t fcn, bool pipeline);  // exec.c

static uint64_t fnv(const uint8_t *p, size_t n, uint64_t h) {
    for (size_t i = 0; i < n; i++) h = (h ^ p[i]) * 0x100000001b3ull;
    return h;
}
static inline uint32_t rd32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static inline uint64_t rd64(const uint8_t *p) { uint64_t v; memcpy(&v, p, 8); return v; }
// A captured container's name: its hash with host pointers left out. The runtime keeps a few of its own pointers in a
// loaded container (8-byte words above the guest range, below 2^47 - both Linux and Windows user space), different
// every run, so hashing them saved the same container again on each start (one Z-A container 486 times: 9 GB).
static uint64_t container_hash(const uint8_t *c, size_t n) {
    uint64_t h = 0xcbf29ce484222325ull;
    size_t i = 0;
    for (; i + 8 <= n; i += 8) {
        uint64_t v = rd64(c + i);
        if (v >= GUEST_HI && v < (1ull << 47)) v = 0;
        h = (h ^ v) * 0x100000001b3ull;
    }
    return fnv(c + i, n - i, h);
}

static bool disabled(void) {
    static int off = -1;
    if (off < 0) off = getenv("LARECOMP_WARMUP") && atoi(getenv("LARECOMP_WARMUP")) == 0;
    return off;
}

// ---- stages and programs (deduplicated by content), shared by the start's warm-up and the captures
typedef struct { uint8_t *code; uint32_t len, max, cn; } WStage;  // code from its NVN header on; max = translate's max_size;
                                                               // cn: compiler constants after it (code + max)
typedef struct { int vs, fs; } WProg;                        // stage indices (fs -1: vertex-only)
typedef struct {
    WStage *stages;
    int nstages, capstages;
    uint64_t *stage_hash;  // open addressing: blob hash -> stage index + 1
    int *stage_slot;
    int stage_tab;
    WProg *progs;
    int nprogs, capprogs;
    uint64_t *prog_keys;  // open addressing: (vs, fs) -> 1
    int prog_tab;
} Set;

static void set_init(Set *s, int stage_tab, int prog_tab) {
    memset(s, 0, sizeof *s);
    s->stage_tab = stage_tab, s->prog_tab = prog_tab;
    s->stage_hash = calloc((size_t)stage_tab, sizeof *s->stage_hash);
    s->stage_slot = calloc((size_t)stage_tab, sizeof *s->stage_slot);
    s->prog_keys = calloc((size_t)prog_tab, sizeof *s->prog_keys);
}

static void set_free(Set *s) {
    for (int i = 0; i < s->nstages; i++) free(s->stages[i].code);
    free(s->stages), free(s->progs), free(s->stage_hash), free(s->stage_slot), free(s->prog_keys);
    memset(s, 0, sizeof *s);
}

static int add_stage(Set *s, const uint8_t *code, uint32_t len, uint32_t max, uint32_t cn) {
    uint64_t h = fnv(code, len, 0xcbf29ce484222325ull ^ max);
    if (!h) h = 1;
    uint32_t k = (uint32_t)(h ^ (h >> 29)) & (s->stage_tab - 1);
    while (s->stage_slot[k]) {
        if (s->stage_hash[k] == h) return s->stage_slot[k] - 1;
        k = (k + 1) & (s->stage_tab - 1);
    }
    if (s->nstages * 2 >= s->stage_tab) return -1;  // (table full: the rest is left to first use)
    if (s->nstages == s->capstages) {
        s->capstages = s->capstages ? s->capstages * 2 : 4096;
        s->stages = realloc(s->stages, (size_t)s->capstages * sizeof *s->stages);
    }
    WStage *st = &s->stages[s->nstages];
    st->code = malloc(len);
    memcpy(st->code, code, len);
    st->len = len, st->max = max, st->cn = cn;
    s->stage_hash[k] = h;
    s->stage_slot[k] = ++s->nstages;
    return s->nstages - 1;
}

static bool add_prog(Set *s, int vs, int fs) {  // false: known already
    uint64_t key = ((uint64_t)(uint32_t)vs << 32 | (uint32_t)(fs + 1)) + 1;
    uint32_t k = (uint32_t)((key * 0x9E3779B97F4A7C15ull) >> 40) & (s->prog_tab - 1);
    while (s->prog_keys[k]) {
        if (s->prog_keys[k] == key) return false;
        k = (k + 1) & (s->prog_tab - 1);
    }
    if (s->nprogs * 2 >= s->prog_tab) return false;
    s->prog_keys[k] = key;
    if (s->nprogs == s->capprogs) {
        s->capprogs = s->capprogs ? s->capprogs * 2 : 4096;
        s->progs = realloc(s->progs, (size_t)s->capprogs * sizeof *s->progs);
    }
    s->progs[s->nprogs++] = (WProg){vs, fs};
    return true;
}

// ---- containers
// A pointer field of a container at c (n bytes; base: its address in guest memory, 0 for a file's copy) as an offset into it
static inline uint64_t offset_of(uint64_t v, uint64_t base, uint32_t n) {
    if (v < n) return v;
    if (base && v >= base && v - base < n) return v - base;
    return ~0ull;
}

// The stage record whose code pointer is at c + off: its stage (1 vertex, 5 fragment) and stage index in s, or -1
static int record(Set *s, const uint8_t *c, uint32_t n, uint64_t base, uint32_t off, int *stage) {
    uint64_t code = offset_of(rd64(c + off), base, n), ctrl = offset_of(rd64(c + off - 8), base, n);
    if (code == ~0ull || ctrl == ~0ull || code & 0xFF || code + 0x40 > n || ctrl + 0x750 > n) return -1;
    if (rd32(c + code) != 0x12345678 || rd32(c + ctrl) != 0x98761234) return -1;
    uint32_t sph_type = (rd32(c + code + 0x30) >> 10) & 0xF;
    if (sph_type != 1 && sph_type != 5) return -1;
    uint32_t const_off = rd32(c + ctrl + 0x700);
    uint32_t max = const_off ? const_off : 0x10000;  // (exec.c get_program: cmax)
    uint32_t len = max, cn = const_off ? rd32(c + ctrl + 0x6FC) : 0;  // (the compiler constants follow the code)
    if (cn > 0x10000 || code + max + cn > n) cn = 0;
    len += cn;
    if (code + len > n) len = (uint32_t)(n - code);
    if (len < 0x40) return -1;
    *stage = (int)sph_type;
    return add_stage(s, c + code, len, len < max ? len : max, cn);
}

// Every program of the container at c into s; returns how many were new
static int scan_container(Set *s, const uint8_t *c, uint32_t n, uint64_t base) {
    int added = 0;
    for (uint32_t off = 8; off + 0x48 <= n; off += 8) {
        int st, st2 = 0;
        int vs = record(s, c, n, base, off, &st);
        if (vs < 0 || st != 1) continue;
        int fs = off + 0x48 <= n ? record(s, c, n, base, off + 0x40, &st2) : -1;
        added += add_prog(s, vs, fs >= 0 && st2 == 5 ? fs : -1);
    }
    return added;
}

static bool container_at(const uint8_t *p, size_t avail, uint32_t *size) {
    if (avail < 0x64 || rd64(p) != 0x48534e42ull || memcmp(p + 0x60, "grsc", 4)) return false;  // "BNSH\0\0\0\0"
    uint32_t n = rd32(p + 0x1C);
    if (n <= 0x100 || n > (64u << 20) || n > avail) return false;
    *size = n;
    return true;
}

// One program: translated and compiled (with the start screen's or the game's Vulkan device: also its shader modules and
// pipeline layout, as a draw would make them)
static void compile(const WStage *v, const WStage *f) {
    static int pipes = -1;
    if (pipes < 0) pipes = getenv("LARECOMP_WARMUP_PIPELINES") && atoi(getenv("LARECOMP_WARMUP_PIPELINES")) != 0;
    if (g_vk.dev) { vk_warm_program(v->code, v->max, v->cn, f ? f->code : NULL, f ? f->max : 0, f ? f->cn : 0, pipes); return; }
    ShaderKey vk = {0}, fk = {0};
    vk.glsl = fk.glsl = 1;  // (flip_y and depth_minus_one 0: window origin and depth mode 1 in both Legends games)
    ShaderInfo *vi = maxwell_translate_consts(v->code, v->max, v->cn ? v->code + v->max : NULL, v->cn, STAGE_VERTEX, &vk);
    if (vi && vi->msl) {
        size_t n;
        free(vk_glsl_to_spirv(vi->msl, STAGE_VERTEX, "vs", &n));
        if (f) {
            fk.vs_out_mask = vi->out_mask;
            ShaderInfo *fi = maxwell_translate_consts(f->code, f->max, f->cn ? f->code + f->max : NULL, f->cn, STAGE_FRAGMENT, &fk);
            if (fi && fi->msl) free(vk_glsl_to_spirv(fi->msl, STAGE_FRAGMENT, "fs", &n));
            if (fi) maxwell_free(fi);
        }
    }
    if (vi) maxwell_free(vi);
}

// ================================================================== at the start
static Set g_set;
static _Atomic int g_phase;  // 0 idle, 1 scanning, 2 compiling, 3 done
static _Atomic int g_done, g_total, g_next;
static _Atomic uint64_t g_scanned;  // bytes read so far
static char g_root[1024], g_marker[1100];

static bool skip_ext(const char *name) {  // (audio and video packs: large, never a shader container)
    static const char *const ext[] = {".bnk", ".pck", ".wem", ".bfstm", ".bwav", ".bwar", ".mp4", ".webm", ".usm", ".bfsar", ".opus", ".trpfs"};
    const char *dot = strrchr(name, '.');
    if (!dot) return false;
    for (size_t i = 0; i < sizeof ext / sizeof *ext; i++) {
        const char *a = dot, *b = ext[i];
        while (*a && *b && (*a | 0x20) == *b) a++, b++;
        if (!*a && !*b) return true;
    }
    return false;
}

// A romfs file: the containers in it. A captured one (cache/warm/containers): its guest address, then the container.
static void scan_file(const char *path, uint64_t size, bool captured) {
    if (size < 0x100 || size > (512ull << 20)) return;
    FILE *f = fopen(path, "rb");
    if (!f) return;
    uint8_t *d = malloc(size);
    bool ok = d && fread(d, 1, size, f) == size;
    fclose(f);
    uint32_t n;
    if (ok && captured) {
        if (container_at(d + 8, size - 8, &n)) scan_container(&g_set, d + 8, n, rd64(d));
    } else if (ok) {
        for (uint8_t *p = d; (p = memchr(p, 'B', size - (size_t)(p - d))); p++)
            if (container_at(p, size - (size_t)(p - d), &n)) scan_container(&g_set, p, n, 0);
    }
    free(d);
    atomic_fetch_add(&g_scanned, size);
}

static void scan_dir(const char *dir, bool captured) {
    DIR *dp = opendir(dir);
    if (!dp) return;
    struct dirent *e;
    while ((e = readdir(dp))) {
        if (e->d_name[0] == '.') continue;
        char path[2048];
        snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
        struct stat sb;
        if (stat(path, &sb)) continue;
        if (S_ISDIR(sb.st_mode)) scan_dir(path, captured);
        else if (S_ISREG(sb.st_mode) && !skip_ext(e->d_name)) scan_file(path, (uint64_t)sb.st_size, captured);
    }
    closedir(dp);
}

static void *worker(void *arg) {
    (void)arg;
    for (int i; (i = atomic_fetch_add(&g_next, 1)) < g_set.nprogs; atomic_fetch_add(&g_done, 1)) {
        WProg *p = &g_set.progs[i];
        compile(&g_set.stages[p->vs], p->fs >= 0 ? &g_set.stages[p->fs] : NULL);
    }
    return NULL;
}

static void *run(void *arg) {
    (void)arg;
    uint64_t t0 = plat_now_ns();
    set_init(&g_set, 1 << 17, 1 << 18);
    char dir[1100];
    snprintf(dir, sizeof dir, "%s/romfs", g_root);
    scan_dir(dir, false);
    int from_romfs = g_set.nprogs;
    scan_dir("cache/warm/containers", true);
    uint64_t t1 = plat_now_ns();
    LOG("[warmup] %d shader programs (%d distinct stages): %d in the game's files, %d more in containers captured earlier "
        "(%.1f s, %.0f MB read)\n", g_set.nprogs, g_set.nstages, from_romfs, g_set.nprogs - from_romfs, (double)(t1 - t0) / 1e9,
        (double)atomic_load(&g_scanned) / 1048576.0);
    atomic_store(&g_total, g_set.nprogs);
    atomic_store(&g_phase, 2);
    maxwell_gmem_base = HEAP_BASE;  // (as exec.c sets them before translating)
    maxwell_gmem_size = HEAP_MAX;
    int nt = plat_cpu_count() - 1;
    if (nt < 1) nt = 1;
    if (nt > 32) nt = 32;
    pthread_t th[32];
    for (int i = 0; i < nt; i++) pthread_create(&th[i], NULL, worker, NULL);
    for (int i = 0; i < nt; i++) pthread_join(th[i], NULL);
    LOG("[warmup] compiled in %.1f s on %d threads\n", (double)(plat_now_ns() - t1) / 1e9, nt);
    set_free(&g_set);
    FILE *f = fopen(g_marker, "w");
    if (f) { fprintf(f, "%s\n", maxwell_build_id()); fclose(f); }
    atomic_store(&g_phase, 3);
    return NULL;
}

// The game's id (its main module's build id) for the marker's name
static bool game_id(const char *root, char out[33]) {
    char path[1100];
    snprintf(path, sizeof path, "%s/exefs/main", root);
    FILE *f = fopen(path, "rb");
    uint8_t id[16];
    bool ok = f && !fseek(f, 0x40, SEEK_SET) && fread(id, 1, 16, f) == 16;
    if (f) fclose(f);
    for (int i = 0; ok && i < 16; i++) sprintf(out + 2 * i, "%02x", id[i]);
    return ok;
}

// Whether the shaders of the game at root still have to be warmed up (not done for this translator build)
bool warmup_needed(const char *root) {
    if (disabled()) return false;
    char id[33];
    if (!game_id(root, id)) return false;
    mkdir("cache", 0755);
    mkdir("cache/warm", 0755);
    snprintf(g_marker, sizeof g_marker, "cache/warm/%s", id);
    snprintf(g_root, sizeof g_root, "%s", root);
    FILE *f = fopen(g_marker, "r");
    char line[128] = "";
    if (f) { if (!fgets(line, sizeof line, f)) line[0] = 0; fclose(f); }
    line[strcspn(line, "\n")] = 0;
    return strcmp(line, maxwell_build_id()) != 0;
}

void warmup_start(void) {
    if (atomic_load(&g_phase)) return;
    atomic_store(&g_phase, 1);
    pthread_t t;
    pthread_create(&t, NULL, run, NULL);
    pthread_detach(t);
}

// phase 1 scanning (done: MB read so far), 2 compiling (done / total programs), 3 finished
int warmup_progress(int *done, int *total) {
    int ph = atomic_load(&g_phase);
    if (ph == 1) *done = (int)(atomic_load(&g_scanned) >> 20), *total = 0;
    else *done = atomic_load(&g_done), *total = atomic_load(&g_total);
    return ph;
}

// ================================================================== while playing: containers the game loads
static pthread_mutex_t g_cap_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_cap_cond = PTHREAD_COND_INITIALIZER;
static Set g_cap;      // programs of the captured containers (kept: their stages are compiled from it)
static int g_cap_next;  // next of g_cap.progs for the compile thread
static struct { uint64_t base; uint32_t size; } g_known[1024];
static int g_nknown;
static uint64_t g_miss[4096];  // 64 KB pages whose control sections are in no container (not searched again)

static void *cap_worker(void *arg) {
    (void)arg;
#ifdef _WIN32
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_LOWEST);
#else
    setpriority(PRIO_PROCESS, (id_t)syscall(SYS_gettid), 19);  // (behind the game's threads)
#endif
    uint64_t done = 0;
    for (;;) {
        pthread_mutex_lock(&g_cap_lock);
        while (g_cap_next >= g_cap.nprogs) pthread_cond_wait(&g_cap_cond, &g_cap_lock);
        WProg p = g_cap.progs[g_cap_next++];
        WStage v = g_cap.stages[p.vs], f = p.fs >= 0 ? g_cap.stages[p.fs] : (WStage){0};  // (copies: the array may grow)
        pthread_mutex_unlock(&g_cap_lock);
        compile(&v, p.fs >= 0 ? &f : NULL);
        done++;
        if (!(done & (done - 1)) && done >= 64) LOG("[warmup] %llu programs of loaded shader containers compiled\n", (unsigned long long)done);
    }
    return NULL;
}

// A container the game has loaded at base: saved for the next warm-up, its programs queued for the compile thread
static void capture(uint64_t base, uint32_t size) {
    const uint8_t *c = (const uint8_t *)(uintptr_t)base;
    uint64_t h = container_hash(c, size);
    char path[96];
    mkdir("cache", 0755);
    mkdir("cache/warm", 0755);
    mkdir("cache/warm/containers", 0755);
    snprintf(path, sizeof path, "cache/warm/containers/%016llx.bnsh", (unsigned long long)h);
    struct stat sb;
    if (stat(path, &sb)) {
        char tmp[112];
        snprintf(tmp, sizeof tmp, "%s.tmp", path);
        FILE *f = fopen(tmp, "wb");
        if (f) {
            bool ok = fwrite(&base, 8, 1, f) == 1 && fwrite(c, 1, size, f) == size;
            ok &= fclose(f) == 0;
            if (ok) rename(tmp, path); else remove(tmp);
        }
    }
    static bool started;
    if (!g_cap.stage_tab) set_init(&g_cap, 1 << 18, 1 << 19);
    int added = scan_container(&g_cap, c, size, base);
    static int nlog;
    if (added && nlog++ < 16) LOG("[warmup] shader container at %llx (%u KB): %d new programs to compile\n", (unsigned long long)base, size >> 10, added);
    if (added && !started) {
        started = true;
        pthread_t t;
        pthread_create(&t, NULL, cap_worker, NULL);
        pthread_detach(t);
    }
    if (added) pthread_cond_signal(&g_cap_cond);
}

// objects.c set_shaders: a program's stage with its control section at control (guest memory) was set up
void warmup_capture(uint64_t control) {
    if (disabled() || control < GUEST_LO || control >= GUEST_HI) return;
    pthread_mutex_lock(&g_cap_lock);
    for (int i = 0; i < g_nknown; i++)
        if (control - g_known[i].base < g_known[i].size) { pthread_mutex_unlock(&g_cap_lock); return; }
    uint64_t page = control >> 16;
    uint32_t ms = (uint32_t)(page * 0x9E3779B1u) & 4095;
    if (g_miss[ms] == page) { pthread_mutex_unlock(&g_cap_lock); return; }
    // the container's header: before the control section, within 16 MB (8-byte aligned, as the game allocates it)
    uint64_t found = 0;
    uint32_t size = 0;
    for (uint64_t a = control & ~7ull, lo = control > GUEST_LO + (16u << 20) ? control - (16u << 20) : GUEST_LO; a >= lo; a -= 8) {
        if (*(const uint64_t *)(uintptr_t)a != 0x48534e42ull) continue;
        if (container_at((const uint8_t *)(uintptr_t)a, (size_t)(GUEST_HI - a), &size) && control - a < size) found = a;
        break;
    }
    if (!found) g_miss[ms] = page;
    else {
        if (g_nknown < 1024) g_known[g_nknown].base = found, g_known[g_nknown].size = size, g_nknown++;
        capture(found, size);
    }
    pthread_mutex_unlock(&g_cap_lock);
}
