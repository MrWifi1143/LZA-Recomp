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
// Done once per translator version and game: cache/warm/<game id>. With the start screen skipped (its setting, --play) it
// runs in the background instead, behind the game's threads, while the game starts (warmup_start_background).
// LARECOMP_WARMUP=0: none of it; =1: with --play, before the game, as on the start screen.
// The caches' clean-up (warmup_cleanup_start, every start): other translator versions' folders of cache/xlat and
// cache/spirv (and their files from before the folders) are deleted, and captured containers saved more than once under
// old names (the host-pointer hash, below) are merged - Z-A's cache had grown to 38 GB.
#include <dirent.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/resource.h>
#include <sys/syscall.h>
#endif

#include "../../rt.h"
#include "../maxwell/shader.h"
#include "shaders.h"

bool vk_ready(void);  // device.c: the device fully set up
bool rt_resource(const char *name, const char *dev_rel, char *out, size_t n);  // paths.c
bool vk_warm_program_keyed(const uint8_t *vs, size_t vmax, size_t vcn, const uint8_t *fs, size_t fmax, size_t fcn, size_t flen,
                           bool pipeline, const ShaderKey *vkey, const ShaderKey *fkey);  // exec.c

static uint64_t fnv(const uint8_t *p, size_t n, uint64_t h) {
    for (size_t i = 0; i < n; i++) h = (h ^ p[i]) * 0x100000001b3ull;
    return h;
}
static inline uint32_t rd32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static inline uint64_t rd64(const uint8_t *p) { uint64_t v; memcpy(&v, p, 8); return v; }
// A captured container's name: its hash with host pointers left out. The runtime keeps a few of its own pointers in a
// loaded container (8-byte words above the guest range, below 2^48 - user space: x86-64 Linux and Windows stop at 2^47,
// AArch64 Linux goes to 2^48), different every run, so hashing them saved the same container again on each start (one Z-A
// container 486 times: 9 GB). Below 2^47 only, the Steam Frame's pointers (0xf7..) stayed in: 7225 containers, 5.4 GB,
// and every start compiled their programs again - the Frame at 90 C even at 30 fps.
static uint64_t container_hash(const uint8_t *c, size_t n) {
    uint64_t h = 0xcbf29ce484222325ull;
    size_t i = 0;
    for (; i + 8 <= n; i += 8) {
        uint64_t v = rd64(c + i);
        if (v >= GUEST_HI && v < (1ull << 48)) v = 0;
        h = (h ^ v) * 0x100000001b3ull;
    }
    return fnv(c + i, n - i, h);
}

// Off by default on AArch64 hosts (the Steam Frame): there it never caught up - each start compiled thousands of programs
// again (the cache grew by ~2500 SPIR-V files a run) on a CPU that has no headroom, the SoC at 90-96 C and paused even at
// 30 fps; without it the draws' own programs (~450 a run, all from the cache after the first) cost 0.04 s and the Frame
// ran 40 s of Lumiose with no pause, CPU 42 vs 113-136 ms per frame. LARECOMP_WARMUP=1 turns it on there.
static bool disabled(void) {
    static int off = -1;
    if (off < 0) {
        const char *e = getenv("LARECOMP_WARMUP");
#if defined(__aarch64__)
        off = !(e && atoi(e) != 0);
#else
        off = e && atoi(e) == 0;
#endif
    }
    return off;
}

// ---- stages and programs (deduplicated by content), shared by the start's warm-up and the captures
typedef struct { uint8_t *code; uint32_t len, max, cn; uint8_t sph; } WStage;  // sph: the stage type (1 vertex, 5 fragment)  // code from its NVN header on; max = translate's max_size;
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

static int add_stage(Set *s, const uint8_t *code, uint32_t len, uint32_t max, uint32_t cn, int sph) {
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
    st->len = len, st->max = max, st->cn = cn, st->sph = (uint8_t)sph;
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
    return add_stage(s, c + code, len, len < max ? len : max, cn, (int)sph_type);
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

// ---- the shader manifest
// The programs the game draws with, in the order it first did, each with the key (shader.h ShaderKey: attribute and
// target kinds, transform feedback, ...) of its draws: one line each, "<vertex> <fragment or 0> <vertex key> <fragment
// key>" - maxwell_program_hash names and the keys' bytes, in hex (hashes and render state, nothing of the game's). The
// warm-up builds these first and in that order - translated with their own keys and their pipeline shader parts compiled
// (the ~50-100 ms per part a fresh driver cache costs, the first-play hitches) - then every other program as before. Two
// lists: data/shader_manifest.txt, shipped (recorded from play-throughs: a first start has the intro's and the city's
// ready before they are drawn), then cache/warm/played.txt, what this install's own sessions drew (appended as each
// program first appears: the warm-up after an update starts with what this player has seen).
// LARECOMP_SHADER_MANIFEST=<file>: that list instead of the shipped one (recording a new one: copy played.txt).
typedef struct { uint64_t vh, fh; ShaderKey vk, fk; } MEntry;
// Also the pipelines' state parts the game's draws asked for (exec.c vk_warm_state): "vi <hex>" a vertex input (formats,
// strides, topology), "fo <hex>" a fragment output (target formats, blending) - built first of all.
typedef struct { int kind; uint32_t n; uint8_t *bytes; } MState;
bool vk_warm_state(int kind, const void *state, size_t n);  // exec.c

static uint64_t mentry_hash(const MEntry *e) {
    uint64_t h = fnv((const uint8_t *)&e->vk, sizeof e->vk, (e->vh * 31) ^ e->fh);
    return fnv((const uint8_t *)&e->fk, sizeof e->fk, h) | 1;
}

static void hex_out(FILE *f, const void *p, size_t n) {
    for (size_t i = 0; i < n; i++) fprintf(f, "%02x", ((const uint8_t *)p)[i]);
}
static bool hex_in(const char *s, void *p, size_t n) {
    for (size_t i = 0; i < n; i++) {
        unsigned v;
        if (sscanf(s + 2 * i, "%2x", &v) != 1) return false;
        ((uint8_t *)p)[i] = (uint8_t)v;
    }
    return s[2 * n] == 0 || s[2 * n] == ' ' || s[2 * n] == '\n' || s[2 * n] == '\r';
}

// Appends a manifest file's entries to *v (those whose line is well formed)
// st (NULL: skipped): its state lines too
typedef struct { MState *v; int n, cap; } MStates;
static void manifest_read(const char *path, MEntry **v, int *n, int *cap, MStates *st) {
    FILE *f = fopen(path, "r");
    if (!f) return;
    static char line[8192];
    while (fgets(line, sizeof line, f)) {
        if ((line[0] == 'v' && line[1] == 'i' && line[2] == ' ') || (line[0] == 'f' && line[1] == 'o' && line[2] == ' ')) {
            size_t hl = strcspn(line + 3, " \r\n");
            if (!st || !hl || hl & 1 || hl / 2 > 4096) continue;
            MState e = {line[0] == 'v' ? 1 : 4, (uint32_t)(hl / 2), malloc(hl / 2)};
            if (!hex_in(line + 3, e.bytes, e.n)) { free(e.bytes); continue; }
            if (st->n == st->cap) st->v = realloc(st->v, (size_t)(st->cap = st->cap ? st->cap * 2 : 256) * sizeof *st->v);
            st->v[st->n++] = e;
            continue;
        }
        MEntry e;
        memset(&e, 0, sizeof e);
        char vk[256], fk[256];
        unsigned long long vh, fh;
        if (sscanf(line, "%llx %llx %255s %255s", &vh, &fh, vk, fk) != 4) continue;
        e.vh = vh, e.fh = fh;
        if (!hex_in(vk, &e.vk, sizeof e.vk) || !hex_in(fk, &e.fk, sizeof e.fk)) continue;
        if (*n == *cap) *v = realloc(*v, (size_t)(*cap = *cap ? *cap * 2 : 4096) * sizeof **v);
        (*v)[(*n)++] = e;
    }
    fclose(f);
}

// exec.c translate_program: a program the game draws with, translated with these keys (vertex-only: fh 0)
static pthread_mutex_t g_played_lock = PTHREAD_MUTEX_INITIALIZER;
void warmup_note_program(uint64_t vh, uint64_t fh, const ShaderKey *vk, const ShaderKey *fk) {
    if (disabled()) return;
    static uint64_t *seen;  // open addressing, mentry_hash
    static int nseen, cap;
    static FILE *out;
    MEntry e;
    memset(&e, 0, sizeof e);
    e.vh = vh, e.fh = fh, e.vk = *vk;
    if (fh) e.fk = *fk;
    pthread_mutex_lock(&g_played_lock);
    if (!seen) {  // (what earlier sessions noted: not again)
        cap = 1 << 16;
        seen = calloc((size_t)cap, sizeof *seen);
        MEntry *old = NULL;
        int n = 0, c = 0;
        manifest_read("cache/warm/played.txt", &old, &n, &c, NULL);
        for (int i = 0; i < n && nseen < cap / 2; i++) {
            uint64_t h = mentry_hash(&old[i]);
            uint32_t k = (uint32_t)(h ^ (h >> 31)) & (uint32_t)(cap - 1);
            while (seen[k] && seen[k] != h) k = (k + 1) & (uint32_t)(cap - 1);
            if (!seen[k]) seen[k] = h, nseen++;
        }
        free(old);
    }
    uint64_t h = mentry_hash(&e);
    uint32_t k = (uint32_t)(h ^ (h >> 31)) & (uint32_t)(cap - 1);
    while (seen[k] && seen[k] != h) k = (k + 1) & (uint32_t)(cap - 1);
    if (!seen[k] && nseen < cap / 2) {
        seen[k] = h, nseen++;
        if (!out) {
            mkdir("cache", 0755);
            mkdir("cache/warm", 0755);
            out = fopen("cache/warm/played.txt", "a");
        }
        if (out) {
            fprintf(out, "%016llx %016llx ", (unsigned long long)vh, (unsigned long long)fh);
            hex_out(out, &e.vk, sizeof e.vk);
            fputc(' ', out);
            hex_out(out, &e.fk, sizeof e.fk);
            fputc('\n', out);
            fflush(out);
        }
    }
    pthread_mutex_unlock(&g_played_lock);
}

// exec.c lib_get: a state part the game's draws asked for (kind 1 vertex input, 4 fragment output), noted once
void warmup_note_state(int kind, const void *state, size_t n) {
    if (disabled()) return;
    static uint64_t seen[8192];
    static int nseen;
    static FILE *out;
    uint64_t h = fnv(state, n, 0xcbf29ce484222325ull ^ (uint64_t)kind) | 1;
    pthread_mutex_lock(&g_played_lock);
    static bool loaded;
    if (!loaded) {  // (what earlier sessions noted)
        loaded = true;
        MEntry *m = NULL;
        int nm = 0, cm = 0;
        MStates old = {0};
        manifest_read("cache/warm/played.txt", &m, &nm, &cm, &old);
        free(m);
        for (int i = 0; i < old.n; i++) {
            uint64_t sh = fnv(old.v[i].bytes, old.v[i].n, 0xcbf29ce484222325ull ^ (uint64_t)old.v[i].kind) | 1;
            uint32_t k = (uint32_t)(sh ^ (sh >> 31)) & 8191;
            while (seen[k] && seen[k] != sh) k = (k + 1) & 8191;
            if (!seen[k] && nseen < 4096) seen[k] = sh, nseen++;
            free(old.v[i].bytes);
        }
        free(old.v);
    }
    uint32_t k = (uint32_t)(h ^ (h >> 31)) & 8191;
    while (seen[k] && seen[k] != h) k = (k + 1) & 8191;
    if (!seen[k] && nseen < 4096) {
        seen[k] = h, nseen++;
        if (!out) {
            mkdir("cache", 0755);
            mkdir("cache/warm", 0755);
            out = fopen("cache/warm/played.txt", "a");
        }
        if (out) {
            fputs(kind == 1 ? "vi " : "fo ", out);
            hex_out(out, state, n);
            fputc('\n', out);
            fflush(out);
        }
    }
    pthread_mutex_unlock(&g_played_lock);
}

// One program: translated and compiled (with the start screen's or the game's Vulkan device: also its shader modules and
// pipeline layout, as a draw would make them). keys (vertex, fragment): a manifest entry's, its pipeline shader parts
// built too; NULL: the key most draws have.
static void compile_keyed(const WStage *v, const WStage *f, const ShaderKey *keys) {
    static int pipes = -1;
    if (pipes < 0) pipes = getenv("LARECOMP_WARMUP_PIPELINES") && atoi(getenv("LARECOMP_WARMUP_PIPELINES")) != 0;
    if (vk_ready()) {
        vk_warm_program_keyed(v->code, v->max, v->cn, f ? f->code : NULL, f ? f->max : 0, f ? f->cn : 0, f ? f->len : 0, pipes,
                              keys, keys ? keys + 1 : NULL);
        return;
    }
    ShaderKey vk = {0}, fk = {0};
    vk.glsl = fk.glsl = 1;  // (flip_y and depth_minus_one 0: window origin and depth mode 1 in both Legends games)
    if (keys) vk = keys[0], fk = keys[1];
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
static void compile(const WStage *v, const WStage *f) { compile_keyed(v, f, NULL); }

// ================================================================== at the start
static Set g_set;
static _Atomic int g_phase;  // 0 idle, 1 scanning, 2 compiling, 3 done
static _Atomic int g_done, g_total, g_next;
typedef struct { int prog; int key; } Job;  // key: index into g_keys (a manifest entry's two keys), -1 the usual key;
                                            // prog -1: a state part, key its index in g_states
static Job *g_jobs;
static int g_njobs;
static ShaderKey (*g_keys)[2];
static MStates g_st;  // (the manifests' state parts, for the jobs)
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

static bool g_background;  // (warmup_start_background: behind the game's threads)

// The warm-up's order: the manifests' programs first (their keys, as listed), then the rest of g_set with the usual key.
// A listed program's stages are looked up one by one (by maxwell_program_hash): the game pairs vertex and fragment
// stages its own way, not always as they lie in a container - a listed pair is added to g_set as listed.
static int stage_find(const uint64_t *hk, const int *hv, int tab, uint64_t h) {
    uint32_t k = (uint32_t)(h ^ (h >> 29)) & (uint32_t)(tab - 1);
    while (hk[k] && hk[k] != h) k = (k + 1) & (uint32_t)(tab - 1);
    return hk[k] ? hv[k] - 1 : -1;
}
static void stage_put(uint64_t *hk, int *hv, int tab, uint64_t h, int i) {
    uint32_t k = (uint32_t)(h ^ (h >> 29)) & (uint32_t)(tab - 1);
    while (hk[k] && hk[k] != h) k = (k + 1) & (uint32_t)(tab - 1);
    if (!hk[k]) hk[k] = h, hv[k] = i + 1;
}
static void plan_jobs(void) {
    int np = g_set.nprogs, ns = g_set.nstages, tab = 1;
    while (tab < ns * 4) tab <<= 1;
    // stage hashes: as a vertex stage (the programs' first) and as a fragment stage (their second)
    uint64_t *vk_ = calloc((size_t)tab, sizeof *vk_), *fk_ = calloc((size_t)tab, sizeof *fk_);
    int *vv = calloc((size_t)tab, sizeof *vv), *fv = calloc((size_t)tab, sizeof *fv);
    for (int i = 0; i < ns; i++) {  // (every stage, by its type: the game pairs them its own way)
        const WStage *st = &g_set.stages[i];
        if (st->sph != 1 && st->sph != 5) continue;
        int stage = st->sph == 1 ? STAGE_VERTEX : STAGE_FRAGMENT;
        uint64_t h = maxwell_program_hash(st->code, st->max, st->cn ? st->code + st->max : NULL, st->cn, stage);
        if (stage == STAGE_VERTEX) stage_put(vk_, vv, tab, h, i);
        else stage_put(fk_, fv, tab, h, i);
    }
    MEntry *m = NULL;
    int nm = 0, cm = 0;
    char shipped[1100];
    const char *e = getenv("LARECOMP_SHADER_MANIFEST");
    if (e) manifest_read(e, &m, &nm, &cm, &g_st);
    else if (rt_resource("shader_manifest.txt", "data/shader_manifest.txt", shipped, sizeof shipped)) manifest_read(shipped, &m, &nm, &cm, &g_st);
    int from_shipped = nm;
    manifest_read("cache/warm/played.txt", &m, &nm, &cm, &g_st);
    g_jobs = malloc((size_t)(nm + np + g_st.n) * sizeof *g_jobs);
    for (int i = 0; i < g_st.n; i++) g_jobs[g_njobs++] = (Job){-1, i};  // (the state parts first: fast, and every scene's)
    g_keys = malloc((size_t)(nm ? nm : 1) * sizeof *g_keys);
    int found = 0, nov = 0, nof = 0;
    uint64_t *done = calloc((size_t)(nm ? nm * 2 : 2), sizeof *done);  // (entries already queued: by mentry_hash)
    for (int i = 0; i < nm; i++) {
        int v = stage_find(vk_, vv, tab, m[i].vh), f = m[i].fh ? stage_find(fk_, fv, tab, m[i].fh) : -1;
        if (v < 0) { nov++; continue; }  // (a program this install doesn't have: another game version's)
        if (m[i].fh && f < 0) { nof++; continue; }
        uint64_t eh = mentry_hash(&m[i]);
        uint32_t d = (uint32_t)(eh ^ (eh >> 31)) % (uint32_t)(nm * 2);
        while (done[d] && done[d] != eh) d = (d + 1) % (uint32_t)(nm * 2);
        if (done[d]) continue;
        done[d] = eh;
        if (g_set.nprogs == g_set.capprogs) {
            g_set.capprogs = g_set.capprogs ? g_set.capprogs * 2 : 4096;
            g_set.progs = realloc(g_set.progs, (size_t)g_set.capprogs * sizeof *g_set.progs);
        }
        g_set.progs[g_set.nprogs] = (WProg){v, f};
        g_keys[found][0] = m[i].vk, g_keys[found][1] = m[i].fk;
        g_jobs[g_njobs++] = (Job){g_set.nprogs++, found++};
    }
    if (!getenv("LARECOMP_WARMUP_MANIFEST_ONLY"))  // (research: the manifest's programs only)
        for (int i = 0; i < np; i++) g_jobs[g_njobs++] = (Job){i, -1};
    LOG("[warmup] shader manifest: %d of %d programs listed (%d shipped, %d from this install's play) found here, built first"
        " (%d vertex, %d fragment stages not here), after %d pipeline state parts\n", found, nm, from_shipped, nm - from_shipped, nov, nof,
        g_st.n);
    free(m), free(vk_), free(fk_), free(vv), free(fv), free(done);
}
static void lower_priority(void) {
#ifdef _WIN32
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_LOWEST);
#else
    setpriority(PRIO_PROCESS, (id_t)syscall(SYS_gettid), 19);
#endif
}

static void *worker(void *arg) {
    (void)arg;
    if (g_background) lower_priority();
    // The manifest's programs and state parts are built into pipeline parts with the game's device: in the background
    // it comes up a few seconds after the start - waited for, or the intro's would be translated without their parts.
    // (Before the game - the start screen has its device, `--play` with LARECOMP_WARMUP=1 none: not waited for.)
    for (int w = 0; g_background && !vk_ready() && w < 300; w++) usleep(100000);
    for (int i; (i = atomic_fetch_add(&g_next, 1)) < g_njobs; atomic_fetch_add(&g_done, 1)) {
        if (g_jobs[i].prog < 0) {  // (a state part: with the game's device only)
            const MState *st = &g_st.v[g_jobs[i].key];
            if (vk_ready()) vk_warm_state(st->kind, st->bytes, st->n);
            continue;
        }
        WProg *p = &g_set.progs[g_jobs[i].prog];
        compile_keyed(&g_set.stages[p->vs], p->fs >= 0 ? &g_set.stages[p->fs] : NULL, g_jobs[i].key >= 0 ? g_keys[g_jobs[i].key] : NULL);
    }
    return NULL;
}

// Z-A: its shaders are in data.trpfs, which the romfs scan can't read: once, `lasrecomp <root> --extract-shaders`
// (runtime/oodle_server.c) decodes the shader packs with the game's own decoder into cache/warm/romfs/ (a first start
// then has them to compile up front, not only what a session has captured)
static void extract_packed_shaders(void) {
    char fd[1100], img[1100], exe[1100];
    struct stat sb;
    snprintf(fd, sizeof fd, "%s/romfs/arc/data.trpfd", g_root);
    snprintf(img, sizeof img, "%s/romfs.bin", g_root);
    // (a romfs/ folder - a source checkout - or, on an install, the romfs.bin image the start screen built: the
    // extractor reads the packs from there; checking only the folder skipped every install, 2026-10-31)
    bool packs = !stat(fd, &sb) || !stat(img, &sb);
    if (!packs || !stat("cache/warm/romfs/.done-all", &sb) || !plat_exe_path(exe, sizeof exe)) return;
    uint64_t t0 = plat_now_ns();
    const char *argv[] = {exe, g_root, "--extract-shaders", NULL};
    PlatProc p = plat_spawn(argv, "cache/warm/extract.log", false);
    int rc = p ? plat_proc_wait(p, true) : -1;
    LOG("[warmup] shaders in data.trpfs: %s (%.1f s, cache/warm/extract.log)\n", rc == 0 ? "extracted" : "not extracted", (double)(plat_now_ns() - t0) / 1e9);
    if (rc == 0) {
        FILE *f = fopen("cache/warm/romfs/.done-all", "w");
        if (f) fclose(f);
    }
}

static void *run(void *arg) {
    (void)arg;
    if (g_background) lower_priority();
    uint64_t t0 = plat_now_ns();
    set_init(&g_set, 1 << 17, 1 << 18);
    char dir[1100];
    snprintf(dir, sizeof dir, "%s/romfs", g_root);
    scan_dir(dir, false);
    extract_packed_shaders();
    scan_dir("cache/warm/romfs", false);
    int from_romfs = g_set.nprogs;
    scan_dir("cache/warm/containers", true);
    uint64_t t1 = plat_now_ns();
    LOG("[warmup] %d shader programs (%d distinct stages): %d in the game's files, %d more in containers captured earlier "
        "(%.1f s, %.0f MB read)\n", g_set.nprogs, g_set.nstages, from_romfs, g_set.nprogs - from_romfs, (double)(t1 - t0) / 1e9,
        (double)atomic_load(&g_scanned) / 1048576.0);
    maxwell_gmem_base = HEAP_BASE;  // (as exec.c sets them before translating)
    maxwell_gmem_size = HEAP_MAX;
    plan_jobs();
    atomic_store(&g_total, g_njobs);
    atomic_store(&g_phase, 2);
    int nt = plat_cpu_count() - 1;
    if (g_background) nt = plat_cpu_count() / 2;  // (the game runs beside it)
    if (nt < 1) nt = 1;
    if (nt > 32) nt = 32;
    pthread_t th[32];
    for (int i = 0; i < nt; i++) pthread_create(&th[i], NULL, worker, NULL);
    for (int i = 0; i < nt; i++) pthread_join(th[i], NULL);
    LOG("[warmup] compiled in %.1f s on %d threads%s\n", (double)(plat_now_ns() - t1) / 1e9, nt, g_background ? " (in the background)" : "");
    set_free(&g_set);
    free(g_jobs), free(g_keys);
    g_jobs = NULL, g_keys = NULL, g_njobs = 0;
    for (int i = 0; i < g_st.n; i++) free(g_st.v[i].bytes);
    free(g_st.v), memset(&g_st, 0, sizeof g_st);
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

void warmup_start_background(void) {
    if (atomic_load(&g_phase)) return;
    g_background = true;
    LOG("[warmup] start screen skipped: the shaders are compiled in the background while the game runs\n");
    warmup_start();
}

// ---- the caches' clean-up
static void remove_tree(const char *path) {
    struct stat sb;
    if (stat(path, &sb)) return;
    if (S_ISDIR(sb.st_mode)) {
        DIR *dp = opendir(path);
        struct dirent *e;
        while (dp && (e = readdir(dp))) {
            if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
            char p[2048];
            snprintf(p, sizeof p, "%s/%s", path, e->d_name);
            remove_tree(p);
        }
        if (dp) closedir(dp);
        rmdir(path);
    } else remove(path);
}

// everything in dir but the folder keep
static uint64_t prune_dir(const char *dir, const char *keep) {
    DIR *dp = opendir(dir);
    if (!dp) return 0;
    char **names = NULL;
    size_t n = 0, cap = 0;
    struct dirent *e;
    while ((e = readdir(dp))) {  // (names first: removing while reading a folder can skip entries)
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..") || !strcmp(e->d_name, keep)) continue;
        if (n == cap) cap = cap ? cap * 2 : 4096, names = realloc(names, cap * sizeof *names);
        names[n++] = strdup(e->d_name);
    }
    closedir(dp);
    for (size_t i = 0; i < n; i++) {
        char p[2048];
        snprintf(p, sizeof p, "%s/%s", dir, names[i]);
        remove_tree(p);
        free(names[i]);
    }
    free(names);
    return n;
}

// captured containers named by the old hash (with host pointers in it): renamed to their name now, or removed if that
// one exists (the same container); once, then cache/warm/containers/.merged2 (2: again with the 2^48 bound)
static void merge_containers(void) {
    const char *dir = "cache/warm/containers", *mark = "cache/warm/containers/.merged2";
    struct stat sb;
    if (!stat(mark, &sb) || stat(dir, &sb)) return;
    DIR *dp = opendir(dir);
    if (!dp) return;
    char **names = NULL;
    size_t n = 0, cap = 0;
    struct dirent *e;
    while ((e = readdir(dp))) {
        size_t l = strlen(e->d_name);
        if (l != 21 || strcmp(e->d_name + 16, ".bnsh")) continue;
        if (n == cap) cap = cap ? cap * 2 : 4096, names = realloc(names, cap * sizeof *names);
        names[n++] = strdup(e->d_name);
    }
    closedir(dp);
    uint64_t removed = 0, renamed = 0, freed = 0;
    for (size_t i = 0; i < n; i++) {
        char p[1100], q[1100];
        snprintf(p, sizeof p, "%s/%s", dir, names[i]);
        FILE *f = fopen(p, "rb");
        uint8_t *d = NULL;
        long sz = 0;
        if (f && !fseek(f, 0, SEEK_END) && (sz = ftell(f)) > 8 && sz < (64l << 20) && !fseek(f, 0, SEEK_SET) && (d = malloc((size_t)sz)) &&
            fread(d, 1, (size_t)sz, f) != (size_t)sz) {
            free(d);
            d = NULL;
        }
        if (f) fclose(f);
        if (d) {
            snprintf(q, sizeof q, "%s/%016llx.bnsh", dir, (unsigned long long)container_hash(d + 8, (size_t)sz - 8));
            if (strcmp(p, q)) {
                if (!stat(q, &sb)) remove(p), removed++, freed += (uint64_t)sz;
                else if (!rename(p, q)) renamed++;
            }
            free(d);
        }
        free(names[i]);
    }
    free(names);
    FILE *m = fopen(mark, "w");
    if (m) fclose(m);
    LOG("[warmup] captured shader containers merged: %llu duplicates removed (%.0f MB), %llu renamed, of %zu\n", (unsigned long long)removed,
        (double)freed / 1048576.0, (unsigned long long)renamed, n);
}

static void *cleanup(void *arg) {
    (void)arg;
    lower_priority();
    uint64_t t0 = plat_now_ns();
    merge_containers();
    // The cache folder in use is only known once the device is set up (device.c may turn g_maxwell_warp_slices on,
    // which picks the tag): pruning before that deleted the Steam Frame's own cache, under it, at every start.
    for (int i = 0; !vk_ready() && i < 600; i++) usleep(100000);
    if (!vk_ready()) return NULL;
    uint64_t a = prune_dir("cache/xlat", maxwell_cache_tag()), b = prune_dir("cache/spirv", maxwell_cache_tag());
    if (a || b)
        LOG("[warmup] old shader caches deleted: %llu entries of cache/xlat, %llu of cache/spirv (%.1f s)\n", (unsigned long long)a,
            (unsigned long long)b, (double)(plat_now_ns() - t0) / 1e9);
    return NULL;
}

void warmup_cleanup_start(void) {
    static bool started;
    if (started || disabled()) return;
    started = true;
    pthread_t t;
    pthread_create(&t, NULL, cleanup, NULL);
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
// The shader manifest's programs, in captured containers: built first (their keys, their pipeline shader parts), as soon
// as a loaded container brings their stages - while the game is still loading, before it draws with them. Z-A's
// vertex stages in memory are often not the extracted packs' (206 of the manifest's 227 are found only in the loaded
// containers), so the start's warm-up can't build them. CAP_TAB: stage hash maps (vertex / fragment) over g_cap's stages.
#define CAP_TAB (1 << 19)
static MEntry *g_cm;
static int g_ncm;
static char *g_cm_queued;
static uint64_t *g_cvk, *g_cfk;
static int *g_cvv, *g_cfv;
static char *g_cap_hashed;  // per g_cap stage: bit 0 hashed as vertex, bit 1 as fragment
static int g_cap_hashed_n;
typedef struct { int vs, fs, e; } CapJob;
static CapJob *g_pq;
static int g_npq, g_pq_next, g_pq_cap;

// (g_cap_lock held) the manifest's entries whose stages the containers captured so far have, queued
static void cap_manifest(int from_prog) {
    if (!g_cm) {
        int cm = 0;
        char shipped[1100];
        const char *e = getenv("LARECOMP_SHADER_MANIFEST");
        if (e) manifest_read(e, &g_cm, &g_ncm, &cm, NULL);
        else if (rt_resource("shader_manifest.txt", "data/shader_manifest.txt", shipped, sizeof shipped)) manifest_read(shipped, &g_cm, &g_ncm, &cm, NULL);
        manifest_read("cache/warm/played.txt", &g_cm, &g_ncm, &cm, NULL);
        if (!g_cm) g_cm = calloc(1, sizeof *g_cm);
        g_cm_queued = calloc((size_t)g_ncm + 1, 1);
        g_cvk = calloc(CAP_TAB, sizeof *g_cvk), g_cfk = calloc(CAP_TAB, sizeof *g_cfk);
        g_cvv = calloc(CAP_TAB, sizeof *g_cvv), g_cfv = calloc(CAP_TAB, sizeof *g_cfv);
    }
    if (!g_ncm) return;
    if (g_cap_hashed_n < g_cap.nstages) {
        g_cap_hashed = realloc(g_cap_hashed, (size_t)g_cap.capstages);
        memset(g_cap_hashed + g_cap_hashed_n, 0, (size_t)(g_cap.nstages - g_cap_hashed_n));
        g_cap_hashed_n = g_cap.nstages;
    }
    for (int i = from_prog; i < g_cap.nprogs; i++) {
        int v = g_cap.progs[i].vs, f = g_cap.progs[i].fs;
        if (!(g_cap_hashed[v] & 1)) {
            const WStage *st = &g_cap.stages[v];
            stage_put(g_cvk, g_cvv, CAP_TAB, maxwell_program_hash(st->code, st->max, st->cn ? st->code + st->max : NULL, st->cn, STAGE_VERTEX), v);
            g_cap_hashed[v] |= 1;
        }
        if (f >= 0 && !(g_cap_hashed[f] & 2)) {
            const WStage *st = &g_cap.stages[f];
            stage_put(g_cfk, g_cfv, CAP_TAB, maxwell_program_hash(st->code, st->max, st->cn ? st->code + st->max : NULL, st->cn, STAGE_FRAGMENT), f);
            g_cap_hashed[f] |= 2;
        }
    }
    for (int i = 0; i < g_ncm; i++) {
        if (g_cm_queued[i]) continue;
        int v = stage_find(g_cvk, g_cvv, CAP_TAB, g_cm[i].vh), f = g_cm[i].fh ? stage_find(g_cfk, g_cfv, CAP_TAB, g_cm[i].fh) : -1;
        if (v < 0 || (g_cm[i].fh && f < 0)) continue;
        g_cm_queued[i] = 1;
        if (g_npq == g_pq_cap) g_pq = realloc(g_pq, (size_t)(g_pq_cap = g_pq_cap ? g_pq_cap * 2 : 1024) * sizeof *g_pq);
        g_pq[g_npq++] = (CapJob){v, f, i};
    }
}

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
        while (g_pq_next >= g_npq && g_cap_next >= g_cap.nprogs) pthread_cond_wait(&g_cap_cond, &g_cap_lock);
        if (g_pq_next < g_npq) {  // (the manifest's first, the one the game drew first - a scene's containers come in a burst)
            int best = g_pq_next;
            for (int i = g_pq_next + 1; i < g_npq; i++)
                if (g_pq[i].e < g_pq[best].e) best = i;
            CapJob j = g_pq[best];
            g_pq[best] = g_pq[g_pq_next++];
            WStage v = g_cap.stages[j.vs], f = j.fs >= 0 ? g_cap.stages[j.fs] : (WStage){0};  // (copies: the array may grow)
            ShaderKey keys[2] = {g_cm[j.e].vk, g_cm[j.e].fk};
            pthread_mutex_unlock(&g_cap_lock);
            compile_keyed(&v, j.fs >= 0 ? &f : NULL, keys);
            continue;
        }
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
    int before = g_cap.nprogs, pq = g_npq;
    int added = scan_container(&g_cap, c, size, base);
    cap_manifest(before);
    static int nlog;
    if (added && nlog++ < 16) LOG("[warmup] shader container at %llx (%u KB): %d new programs to compile\n", (unsigned long long)base, size >> 10, added);
    if (added && !started) {  // (a quarter of the CPUs: the manifest's parts are ~25-80 ms each, wanted before the draws)
        started = true;
        int nt = plat_cpu_count() / 4;
        if (nt < 1) nt = 1;
        if (nt > 8) nt = 8;
        for (int i = 0; i < nt; i++) {
            pthread_t t;
            pthread_create(&t, NULL, cap_worker, NULL);
            pthread_detach(t);
        }
    }
    static int wlog = -1;  // LARECOMP_WARMUP_LOG=1: each container's manifest programs and the queue ahead of them
    if (wlog < 0) wlog = getenv("LARECOMP_WARMUP_LOG") != NULL;
    if (wlog && g_npq > pq)
        LOG("[warmup] shader container at %llx: %d programs of the shader manifest queued first (%d before them)\n", (unsigned long long)base,
            g_npq - pq, pq - g_pq_next);
    if (added) pthread_cond_broadcast(&g_cap_cond);
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
