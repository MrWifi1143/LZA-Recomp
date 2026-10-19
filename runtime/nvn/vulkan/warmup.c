// Shader warm-up: before the first boot (and after an update of the runtime) every shader program in the game's files is
// translated and compiled to SPIR-V up front, behind a progress screen (runtime/ui/launcher_lg.mm), so the game doesn't
// stop to compile them - or draw without them - the first time each appears.
//
// The programs are found in the romfs's BNSH shader containers (loose .bnsh files and the ones embedded in .ptcl / .arc;
// containers inside compressed archives stay out of reach). In a container each stage is a record {control offset, code
// offset, code size, control size}; a program's vertex record is followed 0x40 bytes later by its fragment record
// (checked over every container of Legends: Arceus: 13760 pairs, 9 vertex-only). Each program is translated as
// runtime/nvn/vulkan/exec.c translate_program would with the key most draws have (float vertex attributes and render
// targets, the window origin and depth mode both Legends games use, no transform feedback), and its GLSL compiled with
// vk_glsl_to_spirv: what lands in cache/spirv (keyed by the GLSL text) and cache/xlat is what the draws later look up.
// The pipelines themselves are not built (their state is the draws'): on a fresh install they remain the larger cost (NVIDIA:
// ~45 ms each with an empty driver shader cache). Building each program into a pipeline of a typical state only partly
// warms the driver for the draws' own (exec.c vk_warm_program: LARECOMP_WARMUP_PIPELINES=1, a much longer wait).
// PLA, fresh caches, walk test: GLSL -> SPIR-V during play 2.25 s -> 0.2 s, draws skipped while compiling 564 -> ~230.
// A draw whose key differs (an integer render target, transform feedback) still compiles at first use, as before.
// Done once per translator build and game: cache/warm/<game id>. LARECOMP_WARMUP=0: never; =1: also with --play.
#include <dirent.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "../../rt.h"
#include "../maxwell/shader.h"
#include "shaders.h"

typedef struct { uint8_t *code; uint32_t len, max; } WStage;  // code from its NVN header on; max = translate's max_size
typedef struct { int vs, fs; } WProg;                        // stage indices (fs -1: vertex-only)

static WStage *g_stages;
static int g_nstages, g_capstages;
static uint64_t *g_stage_hash;  // open addressing: blob hash -> stage index + 1
static int g_stage_tab = 1 << 16;
static int *g_stage_slot;
static WProg *g_progs;
static int g_nprogs, g_capprogs;
static uint64_t *g_prog_keys;  // open addressing: (vs, fs) pair -> 1
static int g_prog_tab = 1 << 17;

static _Atomic int g_phase;  // 0 idle, 1 scanning, 2 compiling, 3 done
static _Atomic int g_done, g_total;
static _Atomic uint64_t g_scanned;  // bytes read so far
static char g_root[1024], g_marker[1100];

static uint64_t fnv(const uint8_t *p, size_t n, uint64_t h) {
    for (size_t i = 0; i < n; i++) h = (h ^ p[i]) * 0x100000001b3ull;
    return h;
}

static int add_stage(const uint8_t *code, uint32_t len, uint32_t max) {
    uint64_t h = fnv(code, len, 0xcbf29ce484222325ull ^ max);
    if (!h) h = 1;
    uint32_t s = (uint32_t)(h ^ (h >> 29)) & (g_stage_tab - 1);
    while (g_stage_slot[s]) {
        if (g_stage_hash[s] == h) return g_stage_slot[s] - 1;
        s = (s + 1) & (g_stage_tab - 1);
    }
    if (g_nstages * 2 >= g_stage_tab) return -1;  // (table full: the rest is left to first use)
    if (g_nstages == g_capstages) {
        g_capstages = g_capstages ? g_capstages * 2 : 4096;
        g_stages = realloc(g_stages, (size_t)g_capstages * sizeof *g_stages);
    }
    WStage *st = &g_stages[g_nstages];
    st->code = malloc(len);
    memcpy(st->code, code, len);
    st->len = len, st->max = max;
    g_stage_hash[s] = h;
    g_stage_slot[s] = ++g_nstages;
    return g_nstages - 1;
}

static void add_prog(int vs, int fs) {
    uint64_t k = ((uint64_t)(uint32_t)vs << 32 | (uint32_t)(fs + 1)) + 1;
    uint32_t s = (uint32_t)((k * 0x9E3779B97F4A7C15ull) >> 40) & (g_prog_tab - 1);
    while (g_prog_keys[s]) {
        if (g_prog_keys[s] == k) return;
        s = (s + 1) & (g_prog_tab - 1);
    }
    if (g_nprogs * 2 >= g_prog_tab) return;
    g_prog_keys[s] = k;
    if (g_nprogs == g_capprogs) {
        g_capprogs = g_capprogs ? g_capprogs * 2 : 4096;
        g_progs = realloc(g_progs, (size_t)g_capprogs * sizeof *g_progs);
    }
    g_progs[g_nprogs++] = (WProg){vs, fs};
}

static inline uint32_t rd32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static inline uint64_t rd64(const uint8_t *p) { uint64_t v; memcpy(&v, p, 8); return v; }

// One stage record at c + off of a container of size n: its stage (1 vertex, 5 fragment, else -1) and stage index
static int record(const uint8_t *c, uint32_t n, uint32_t off, int *stage) {
    uint64_t code = rd64(c + off), ctrl = rd64(c + off - 8);
    if (code & 0xFF || code + 0x40 > n || ctrl + 0x750 > n || rd32(c + code) != 0x12345678 || rd32(c + ctrl) != 0x98761234) return -1;
    uint32_t sph_type = (rd32(c + code + 0x30) >> 10) & 0xF;
    if (sph_type != 1 && sph_type != 5) return -1;
    uint32_t const_off = rd32(c + ctrl + 0x700);
    uint32_t max = const_off ? const_off : 0x10000;  // (exec.c get_program: cmax)
    uint32_t len = max;
    if (code + len > n) len = (uint32_t)(n - code);
    if (len < 0x40) return -1;
    *stage = (int)sph_type;
    return add_stage(c + code, len, len < max ? len : max);
}

static void scan_container(const uint8_t *c, uint32_t n) {
    for (uint32_t off = 8; off + 0x48 <= n; off += 8) {
        int st, st2;
        int vs = record(c, n, off, &st);
        if (vs < 0 || st != 1) continue;
        int fs = off + 0x40 + 8 <= n ? record(c, n, off + 0x40, &st2) : -1;
        add_prog(vs, fs >= 0 && st2 == 5 ? fs : -1);
    }
}

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

static void scan_file(const char *path, uint64_t size) {
    if (size < 0x100 || size > (512ull << 20)) return;
    FILE *f = fopen(path, "rb");
    if (!f) return;
    uint8_t *d = malloc(size);
    bool ok = d && fread(d, 1, size, f) == size;
    fclose(f);
    if (ok) {
        static const uint8_t sig[8] = {'B', 'N', 'S', 'H', 0, 0, 0, 0};
        for (uint8_t *p = d; (p = memchr(p, 'B', size - (size_t)(p - d))); p++) {
            size_t i = (size_t)(p - d);
            if (i + 8 > size || memcmp(p, sig, 8)) continue;
            if (i + 0x64 > size) break;
            uint32_t n = rd32(p + 0x1C);
            if (n <= 0x100 || n > (64u << 20) || i + n > size || memcmp(p + 0x60, "grsc", 4)) continue;
            scan_container(p, n);
        }
    }
    free(d);
    atomic_fetch_add(&g_scanned, size);
}

static void scan_dir(const char *dir) {
    DIR *dp = opendir(dir);
    if (!dp) return;
    struct dirent *e;
    while ((e = readdir(dp))) {
        if (e->d_name[0] == '.') continue;
        char path[2048];
        snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
        struct stat sb;
        if (stat(path, &sb)) continue;
        if (S_ISDIR(sb.st_mode)) scan_dir(path);
        else if (S_ISREG(sb.st_mode) && !skip_ext(e->d_name)) scan_file(path, (uint64_t)sb.st_size);
    }
    closedir(dp);
}

static _Atomic int g_next;
bool vk_warm_program(const uint8_t *vs, size_t vmax, const uint8_t *fs, size_t fmax, bool pipeline);  // exec.c

static void *worker(void *arg) {
    (void)arg;
    for (int i; (i = atomic_fetch_add(&g_next, 1)) < g_nprogs; atomic_fetch_add(&g_done, 1)) {
        WProg *p = &g_progs[i];
        if (g_vk.dev) {  // (the start screen's device: translation and SPIR-V as a draw would make them, and the pipeline layout)
            static int pipes = -1;
            if (pipes < 0) pipes = getenv("LARECOMP_WARMUP_PIPELINES") && atoi(getenv("LARECOMP_WARMUP_PIPELINES")) != 0;
            WStage *v = &g_stages[p->vs], *f = p->fs >= 0 ? &g_stages[p->fs] : NULL;
            vk_warm_program(v->code, v->max, f ? f->code : NULL, f ? f->max : 0, pipes);
            continue;
        }
        ShaderKey vk = {0}, fk = {0};
        vk.glsl = fk.glsl = 1;  // (flip_y and depth_minus_one 0: window origin and depth mode 1 in both Legends games)
        WStage *v = &g_stages[p->vs];
        ShaderInfo *vi = maxwell_translate(v->code, v->max, STAGE_VERTEX, &vk);
        if (!vi || !vi->msl) { if (vi) maxwell_free(vi); continue; }
        size_t n;
        free(vk_glsl_to_spirv(vi->msl, STAGE_VERTEX, "vs", &n));
        if (p->fs >= 0) {
            fk.vs_out_mask = vi->out_mask;
            WStage *f = &g_stages[p->fs];
            ShaderInfo *fi = maxwell_translate(f->code, f->max, STAGE_FRAGMENT, &fk);
            if (fi && fi->msl) free(vk_glsl_to_spirv(fi->msl, STAGE_FRAGMENT, "fs", &n));
            if (fi) maxwell_free(fi);
        }
        maxwell_free(vi);
    }
    return NULL;
}

static void *run(void *arg) {
    (void)arg;
    uint64_t t0 = plat_now_ns();
    g_stage_hash = calloc((size_t)g_stage_tab, sizeof *g_stage_hash);
    g_stage_slot = calloc((size_t)g_stage_tab, sizeof *g_stage_slot);
    g_prog_keys = calloc((size_t)g_prog_tab, sizeof *g_prog_keys);
    char romfs[1100];
    snprintf(romfs, sizeof romfs, "%s/romfs", g_root);
    scan_dir(romfs);
    uint64_t t1 = plat_now_ns();
    LOG("[warmup] %d shader programs (%d distinct stages) in the game's files (%.1f s, %.0f MB read)\n", g_nprogs, g_nstages,
        (double)(t1 - t0) / 1e9, (double)atomic_load(&g_scanned) / 1048576.0);
    atomic_store(&g_total, g_nprogs);
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
    for (int i = 0; i < g_nstages; i++) free(g_stages[i].code);
    free(g_stages), free(g_progs), free(g_stage_hash), free(g_stage_slot), free(g_prog_keys);
    g_stages = NULL, g_progs = NULL, g_nstages = g_nprogs = g_capstages = g_capprogs = 0;
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
    const char *e = getenv("LARECOMP_WARMUP");
    if (e && atoi(e) == 0) return false;
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
