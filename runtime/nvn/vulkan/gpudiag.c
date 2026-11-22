// GPU crash diagnostics. Players with a GPU we don't have (NVIDIA RTX 50 on new drivers: "device lost") send a log
// that said only that the GPU stopped. Now, always on and nearly free:
// - a ring of the last 32768 draws / dispatches recorded (the program, the pass, the submission) - gpudiag.h gd_op;
// - on NVIDIA, a checkpoint (VK_NV_device_diagnostic_checkpoints) at each pass, dispatch and command buffer start:
//   after a device loss the driver says the last one each pipeline stage reached, so the report names the passes and
//   the programs that were on the GPU when it stopped. Other GPUs: the work submitted after the last finished
//   submission (less exact);
// - the crash note gpucrash.json in the data folder (next to the log and the settings): the programs in flight, the
//   driver, the game, the time. The next start reads it: a fresh note turns on the deeper mode for that session - a
//   checkpoint before every draw, the programs in flight at the crash (not the common ones) each drawn in a pass of
//   its own between full barriers (the GPU's front end otherwise runs ~20 draws ahead of the one that faults), and
//   the GLSL of the programs in flight in the log if it crashes again. ~10% slower; a session without a crash (30
//   minutes, or a normal exit after 2) turns it off again;
// - a program on the GPU at two crashes on the same driver (one of them exact) goes on a skip list: its draws are
//   left out from then on, "[vk] program ... crashed the GPU twice on driver ...: skipped from now on". Never a
//   program with more than 2% of the draws (scenery, characters, terrain) or a full-screen pass - and none at all when
//   such a program was there both times (the crash may be in it); at most 3 from one pair of crashes, 8 in all; a
//   skipped program that turns out to be common (> 2% of a frame's draws for 30 frames) is drawn again and leaves
//   the list. Entries belong to one GPU + driver version: a driver update clears the list.
// Switches: LARECOMP_GPUCRASH_RESET=1 deletes the note (deleting gpucrash.json does the same), LARECOMP_GPU_AUTOSKIP=0
// keeps the list but draws everything, LARECOMP_GPUDIAG=0 turns it all off, LARECOMP_GPUDIAG_DRAWMARKS=0/1 forces the
// per-draw checkpoints, LARECOMP_GPUDIAG_ISOLATE=0 no one-at-a-time drawing, LARECOMP_GPUDIAG_CLEAN_MIN=<minutes> the
// clean time that turns the deeper mode off. Tests: LARECOMP_TEST_GPUCRASH=<fs hash> (that program's vertex shader
// faults the GPU), LARECOMP_TEST_GPUDIAG_LIST=<frame>,... (the programs of a frame), LARECOMP_TEST_GPUDIAG_COST=1.
// Logs and the note name programs, passes, the GPU and its driver - nothing about the player or their files.
#include <math.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <SDL3/SDL_timer.h>
#include "../../kernel.h"
#include "../../paths.h"
#include "vk.h"
#include "gpudiag.h"

GdOp g_gd_ops[GD_OPS];
GdMark g_gd_marks[GD_MARKS];
uint64_t g_gd_nops, g_gd_nmarks;
bool g_gd_cp, g_gd_drawmarks;
uint64_t g_gd_seq, g_gd_done;
static bool gd_off;  // LARECOMP_GPUDIAG=0
static PFN_vkCmdSetCheckpointNV set_cp;
static PFN_vkGetQueueCheckpointDataNV get_cp;
static VkDevice gd_dev;
static VkQueue gd_queue;
static uint64_t frame_no;
static uint16_t pass_in_frame;
#define GD_TAG (1ull << 62)  // (our checkpoint markers: a research trace's own small numbers are told apart)
#define GD_PASS_TAG (1ull << 61)

uint64_t gd_mark(VkCommandBuffer cb, int kind, uint32_t w, uint32_t h, uint32_t fmt, int ncolor) {
    uint64_t m = g_gd_nmarks++;
    GdMark *k = &g_gd_marks[m & (GD_MARKS - 1)];
    k->op = g_gd_nops, k->seq = (uint32_t)g_gd_seq, k->frame = (uint32_t)frame_no;
    k->pass = kind == GD_M_PASS ? ++pass_in_frame : pass_in_frame;
    k->w = (uint16_t)(w > 65535 ? 65535 : w), k->h = (uint16_t)(h > 65535 ? 65535 : h), k->kind = (uint8_t)kind;
    k->ncolor = (uint8_t)ncolor, k->fmt = fmt;
    if (set_cp && cb) set_cp(cb, (void *)(uintptr_t)(GD_TAG | (m & (GD_PASS_TAG - 1))));
    return m;
}

// ---------------------------------------------------------------- programs by key (names for the report)
typedef struct { uint64_t key, vs_hash, fs_hash, vs_addr, fs_addr; const char *vs_glsl, *fs_glsl; int kind; } GdProg;
#define GD_PROGS 16384
static GdProg progs[GD_PROGS];
static _Atomic unsigned nprogs;
void gd_register(uint64_t key, int kind, uint64_t vs_hash, uint64_t fs_hash, uint64_t vs_addr, uint64_t fs_addr,
                 const char *vs_glsl, const char *fs_glsl) {
    if (atomic_load(&nprogs) >= GD_PROGS * 3 / 4) return;
    unsigned s = (unsigned)(key * 0x9E3779B97F4A7C15ull >> 50) & (GD_PROGS - 1);
    for (int i = 0; i < 64; i++, s = (s + 1) & (GD_PROGS - 1)) {
        if (progs[s].key == key) return;  // (the same program under another state key: the first one names it)
        if (progs[s].key) continue;
        progs[s] = (GdProg){key, vs_hash, fs_hash, vs_addr, fs_addr, vs_glsl, fs_glsl, kind};
        atomic_fetch_add(&nprogs, 1);
        return;
    }
}
static const GdProg *prog_of(uint64_t key) {
    unsigned s = (unsigned)(key * 0x9E3779B97F4A7C15ull >> 50) & (GD_PROGS - 1);
    for (int i = 0; i < 64; i++, s = (s + 1) & (GD_PROGS - 1)) {
        if (progs[s].key == key) return &progs[s];
        if (!progs[s].key) return NULL;
    }
    return NULL;
}
// "fs <hash> vs <hash>" / "cs <hash>": how the log and the note name a program (and how the note is read back)
static void prog_name(uint64_t key, char *out, size_t n) {
    const GdProg *p = prog_of(key);
    if (!p) snprintf(out, n, "key %016llx", (unsigned long long)key);
    else if (p->kind == 2) snprintf(out, n, "cs %016llx", (unsigned long long)p->fs_hash);
    else snprintf(out, n, "fs %016llx vs %016llx", (unsigned long long)p->fs_hash, (unsigned long long)p->vs_hash);
}
static uint64_t parse_name(const char *s) {  // (the inverse; 0 if it isn't one)
    if (!strncmp(s, "cs ", 3)) return gd_key_cs(strtoull(s + 3, NULL, 16));
    if (!strncmp(s, "fs ", 3)) {
        char *e;
        uint64_t fs = strtoull(s + 3, &e, 16);
        if (strncmp(e, " vs ", 4)) return 0;
        return gd_key_draw(fs, strtoull(e + 4, NULL, 16));
    }
    if (!strncmp(s, "key ", 4)) return strtoull(s + 4, NULL, 16);
    return 0;
}

// ---------------------------------------------------------------- the crash note (gpucrash.json)
#define NOTE "gpucrash.json"
#define MAX_CRASH 4
#define MAX_SKIP 8
#define CRASH_KEYS 64
// (common: more than 2% of the draws or full-screen at that crash - never skipped, not drawn one at a time either)
typedef struct { int64_t time; int precise, n; uint64_t keys[CRASH_KEYS]; char names[CRASH_KEYS][48]; uint8_t common[CRASH_KEYS]; } GdCrash;
static struct {
    bool loaded;
    uint32_t vendor, device, driver;
    int armed;
    int64_t armed_at;
    GdCrash crash[MAX_CRASH];
    int ncrash;
    struct { uint64_t key; char name[48]; } skip[MAX_SKIP];
    int nskip;
} note;
static uint32_t cur_vendor, cur_device, cur_driver;
static char cur_drv[32], cur_gpu[128];
static bool deep;             // the deeper mode (a fresh note)
static bool skip_on = true;   // LARECOMP_GPU_AUTOSKIP
static bool skip_active[MAX_SKIP];
static bool skip_removed[MAX_SKIP];  // (too common after all: left out of the note, indexes kept for this session)
static uint32_t skip_tries[MAX_SKIP], skip_common_frames[MAX_SKIP];
static uint64_t deep_since_ns;
// The deeper mode draws the programs that were on the GPU at a crash (not the common ones) one at a time: each in a pass
// of its own between full barriers, so the next crash's checkpoints bracket exactly one of them (with a checkpoint at
// every draw alone, the GPU's front end runs ~20 draws ahead and a crash names everything around the culprit too)
#define MAX_ISO 64
static uint64_t iso[MAX_ISO];
static int niso;

static const char *field(const char *s, const char *name) {  // the value after "name": on this line (or NULL)
    char pat[48];
    snprintf(pat, sizeof pat, "\"%s\":", name);
    const char *p = strstr(s, pat);
    if (!p) return NULL;
    p += strlen(pat);
    while (*p == ' ') p++;
    return p;
}
static void note_read(void) {
    memset(&note, 0, sizeof note);
    FILE *f = fopen(NOTE, "rb");
    if (!f) return;
    static char line[16384];
    while (fgets(line, sizeof line, f)) {
        const char *v;
        if (strstr(line, "\"time\":") && note.ncrash < MAX_CRASH) {  // a crash: one line
            GdCrash *c = &note.crash[note.ncrash++];
            memset(c, 0, sizeof *c);
            if ((v = field(line, "time"))) c->time = strtoll(v, NULL, 10);
            if ((v = field(line, "precise"))) c->precise = atoi(v);
            const char *p = field(line, "programs");
            while (p && (p = strchr(p, '"')) && c->n < CRASH_KEYS) {
                p++;
                const char *e = strchr(p, '"');
                if (!e) break;
                uint64_t k = parse_name(p);
                if (k) {
                    c->keys[c->n] = k;
                    const char *cm = strstr(p, " common");
                    c->common[c->n] = cm && cm < e;
                    int len = (int)(e - p) - (c->common[c->n] ? 7 : 0);
                    snprintf(c->names[c->n], sizeof c->names[0], "%.*s", len, p);
                    c->n++;
                }
                p = e + 1;
            }
            continue;
        }
        if ((v = field(line, "skip_program")) && note.nskip < MAX_SKIP) {
            const char *p = strchr(v, '"'), *e = p ? strchr(p + 1, '"') : NULL;
            uint64_t k = p && e ? parse_name(p + 1) : 0;
            if (k) {
                note.skip[note.nskip].key = k;
                snprintf(note.skip[note.nskip].name, sizeof note.skip[0].name, "%.*s", (int)(e - p - 1), p + 1);
                note.nskip++;
            }
            continue;
        }
        if ((v = field(line, "vendor"))) note.vendor = (uint32_t)strtoul(v, NULL, 10);
        else if ((v = field(line, "device"))) note.device = (uint32_t)strtoul(v, NULL, 10);
        else if ((v = field(line, "driver_raw"))) note.driver = (uint32_t)strtoul(v, NULL, 10);
        else if ((v = field(line, "armed_at"))) note.armed_at = strtoll(v, NULL, 10);
        else if ((v = field(line, "armed"))) note.armed = atoi(v);
    }
    fclose(f);
    note.loaded = true;
}
static void note_write(void) {
    FILE *f = fopen(NOTE ".tmp", "wb");
    if (!f) { LOG("[vk] GPU crash note: can't write it\n"); return; }
    fprintf(f, "{\n  \"about\": \"GPU crash note: the GPU stopped (device lost) - read at the next start. Delete this file to reset.\",\n");
    fprintf(f, "  \"game\": \"%s %s\",\n  \"gpu\": \"%s\",\n  \"vendor\": %u,\n  \"device\": %u,\n  \"driver\": \"%s\",\n  \"driver_raw\": %u,\n",
            RT_APP_NAME, RT_APP_VERSION, cur_gpu, cur_vendor, cur_device, cur_drv, cur_driver);
    fprintf(f, "  \"armed\": %d,\n  \"armed_at\": %lld,\n  \"crashes\": [\n", note.armed, (long long)note.armed_at);
    for (int i = 0; i < note.ncrash; i++) {
        const GdCrash *c = &note.crash[i];
        fprintf(f, "    {\"time\": %lld, \"precise\": %d, \"programs\": [", (long long)c->time, c->precise);
        for (int k = 0; k < c->n; k++) fprintf(f, "%s\"%s%s\"", k ? ", " : "", c->names[k], c->common[k] ? " common" : "");
        fprintf(f, "]}%s\n", i + 1 < note.ncrash ? "," : "");
    }
    fprintf(f, "  ],\n  \"skip\": [\n");
    for (int i = 0, first = 1; i < note.nskip; i++)
        if (!skip_removed[i]) fprintf(f, "%s    {\"skip_program\": \"%s\"}", first ? "" : ",\n", note.skip[i].name), first = 0;
    fprintf(f, "\n");
    fprintf(f, "  ]\n}\n");
    fclose(f);
    remove(NOTE);
    if (rename(NOTE ".tmp", NOTE) != 0) LOG("[vk] GPU crash note: can't write it\n");
}

static double clean_min(void) {
    const char *e = getenv("LARECOMP_GPUDIAG_CLEAN_MIN");
    return e && atof(e) > 0 ? atof(e) : 30;
}
static void deep_off(const char *why) {
    if (!deep || !note.armed) return;
    note.armed = 0;
    note_write();
    LOG("[vk] GPU crash diagnostics: %s without a GPU crash - the extra checks are off from the next start\n", why);
}
static void at_exit(void) {  // a normal exit after a while: a clean session
    if (deep && note.armed && SDL_GetTicksNS() - deep_since_ns > 120ull * 1000000000ull) deep_off("a session");
}

bool gd_setup(const VkPhysicalDeviceProperties *props, const char *drv, bool has_cp, bool trace_owns_checkpoints) {
    const char *e = getenv("LARECOMP_GPUDIAG");
    gd_off = e && !atoi(e);
    if (gd_off) { LOG("[vk] GPU crash diagnostics off (LARECOMP_GPUDIAG=0)\n"); return false; }
    cur_vendor = props->vendorID, cur_device = props->deviceID, cur_driver = props->driverVersion;
    snprintf(cur_drv, sizeof cur_drv, "%s", drv);
    snprintf(cur_gpu, sizeof cur_gpu, "%s", props->deviceName);
    for (char *p = cur_gpu; *p; p++) if (*p == '"' || *p == '\\' || (unsigned char)*p < 32) *p = ' ';
    e = getenv("LARECOMP_GPU_AUTOSKIP");
    skip_on = !e || atoi(e);
    if ((e = getenv("LARECOMP_GPUCRASH_RESET")) && atoi(e)) {
        if (remove(NOTE) == 0) LOG("[vk] GPU crash note deleted (LARECOMP_GPUCRASH_RESET=1): skip list and crash history cleared\n");
    }
    note_read();
    if (note.loaded && (note.vendor != cur_vendor || note.device != cur_device || note.driver != cur_driver)) {
        if (note.nskip || note.ncrash)
            LOG("[vk] GPU or driver changed since the last GPU crash (now driver %s): its crash history and skip list cleared\n", cur_drv);
        int armed = note.armed;
        int64_t at = note.armed_at;
        memset(&note, 0, sizeof note);
        note.loaded = true, note.armed = armed, note.armed_at = at;  // (still checks once more: the crash may come back)
        note_write();
    }
    int64_t now = (int64_t)time(NULL);
    deep = note.loaded && note.armed && now - note.armed_at < 14 * 86400;
    if (deep) {
        LOG("[vk] GPU crash diagnostics: the last session ended in a GPU crash (device lost, %lld min ago) - extra checks on for this "
            "session (a checkpoint at every draw, the code of the programs in flight if it happens again)\n",
            (long long)((now - note.armed_at) / 60));
        deep_since_ns = SDL_GetTicksNS();
        atexit(at_exit);
    } else if (note.loaded && note.armed) {  // (too old: not armed any more)
        note.armed = 0;
        note_write();
    }
    if (deep && has_cp && !trace_owns_checkpoints && !(getenv("LARECOMP_GPUDIAG_ISOLATE") && !atoi(getenv("LARECOMP_GPUDIAG_ISOLATE")))) {
        for (int k = note.ncrash; k-- > 0;)  // (the latest crash's first)
            for (int j = 0; j < note.crash[k].n && niso < MAX_ISO; j++) {
                bool dup = note.crash[k].common[j];
                for (int i = 0; i < niso; i++) dup |= iso[i] == note.crash[k].keys[j];
                if (!dup) iso[niso++] = note.crash[k].keys[j];
            }
        if (niso) LOG("[vk] GPU crash diagnostics: the %d programs that were on the GPU then are drawn one at a time (to tell which one it is)\n", niso);
    }
    for (int i = 0; i < note.nskip; i++) {
        skip_active[i] = skip_on;
        LOG("[vk] program %s crashed the GPU twice on driver %s: skipped%s\n", note.skip[i].name, cur_drv,
            skip_on ? " from now on" : " - not now (LARECOMP_GPU_AUTOSKIP=0)");
    }
    e = getenv("LARECOMP_GPUDIAG_DRAWMARKS");
    bool want_cp = has_cp && !trace_owns_checkpoints;
    g_gd_cp = want_cp;
    g_gd_drawmarks = e ? atoi(e) != 0 : deep;
    if (!has_cp) g_gd_drawmarks = false;
    LOG("[vk] GPU crash diagnostics: %s%s\n", trace_owns_checkpoints ? "ring only (the device-lost trace owns the checkpoints)"
                                                : has_cp ? "NVIDIA checkpoints" : "ring of recent work (no checkpoints on this driver)",
        g_gd_drawmarks ? ", at every draw" : "");
    return want_cp;
}

// (test: LARECOMP_TEST_GPUDIAG_COST=1 - what recording costs on this CPU and driver, into a scratch command buffer)
static void cost_test(void) {
    VkCommandPoolCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .queueFamilyIndex = g_vk.qfam};
    VkCommandPool pool;
    if (vkCreateCommandPool(gd_dev, &pi, NULL, &pool) != VK_SUCCESS) return;
    VkCommandBufferAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = pool,
                                      .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
    VkCommandBuffer cb;
    VkCommandBufferBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    if (vkAllocateCommandBuffers(gd_dev, &ai, &cb) != VK_SUCCESS) { vkDestroyCommandPool(gd_dev, pool, NULL); return; }
    bool dm = g_gd_drawmarks;
    double ns[3];
    for (int t = 0; t < 3; t++) {  // 0: op alone, 1: mark (+ checkpoint), 2: op with a per-draw mark
        vkBeginCommandBuffer(cb, &bi);
        enum { N = 20000 };
        g_gd_drawmarks = t == 2 && set_cp;
        uint64_t t0 = SDL_GetTicksNS();
        for (int r = 0; r < 10; r++)
            for (int i = 0; i < N; i++) {
                if (t == 1) gd_mark(cb, GD_M_PASS, 1920, 1080, 0, 1);
                else gd_op(cb, (uint64_t)i * 0x9E3779B97F4A7C15ull, GD_I_DRAW);
            }
        ns[t] = (double)(SDL_GetTicksNS() - t0) / (10.0 * N);
        vkEndCommandBuffer(cb);
        vkResetCommandPool(gd_dev, pool, 0);
    }
    g_gd_drawmarks = dm;
    g_gd_nops = g_gd_nmarks = 0;
    memset(g_gd_ops, 0, sizeof g_gd_ops);
    memset(g_gd_marks, 0, sizeof g_gd_marks);
    vkDestroyCommandPool(gd_dev, pool, NULL);
    LOG("[gpudiag] cost on this machine: %.1f ns a draw / dispatch noted, %.1f ns a mark%s, %.1f ns a draw with its own mark\n", ns[0], ns[1],
        set_cp ? " (with its NVIDIA checkpoint)" : "", ns[2]);
}

void gd_device_ready(VkDevice dev, VkQueue queue) {
    gd_dev = dev, gd_queue = queue;
    if (g_gd_cp) {
        set_cp = (PFN_vkCmdSetCheckpointNV)vkGetDeviceProcAddr(dev, "vkCmdSetCheckpointNV");
        get_cp = (PFN_vkGetQueueCheckpointDataNV)vkGetDeviceProcAddr(dev, "vkGetQueueCheckpointDataNV");
        if (!set_cp || !get_cp) set_cp = NULL, get_cp = NULL, g_gd_cp = g_gd_drawmarks = false;
    }
    const char *e = getenv("LARECOMP_TEST_GPUDIAG_COST");
    if (e && atoi(e) && !gd_off) cost_test();
}

bool gd_isolate_lookup(uint64_t key) {
    for (int i = 0; i < niso; i++) if (iso[i] == key) return !gd_skip_lookup(key);
    return false;
}
int gd_skip_lookup(uint64_t key) {
    for (int i = 0; i < note.nskip; i++) if (note.skip[i].key == key) return i + 1;
    return 0;
}
bool gd_skip_hit(int idx) {
    if (idx < 0 || idx >= MAX_SKIP) return false;
    skip_tries[idx]++;
    return skip_active[idx];
}

static void gd_list_programs(void) {
    static struct { uint64_t key; unsigned n; uint32_t info; } t[512];
    int nt = 0;
    uint64_t nops = g_gd_nops, lo = nops > GD_OPS ? nops - GD_OPS : 0, frame0 = 0;
    // (the last frame's ops: from the last mark of the frame before)
    for (uint64_t m = g_gd_nmarks; m-- > 0 && g_gd_nmarks - m < GD_MARKS;)
        if (g_gd_marks[m & (GD_MARKS - 1)].frame + 1 < frame_no) { frame0 = g_gd_marks[m & (GD_MARKS - 1)].op; break; }
    if (frame0 < lo) frame0 = lo;
    for (uint64_t o = frame0; o < nops; o++) {
        const GdOp *op = &g_gd_ops[o & (GD_OPS - 1)];
        int i = 0;
        while (i < nt && t[i].key != op->key) i++;
        if (i == nt) { if (nt == 512) continue; t[nt].key = op->key, t[nt].n = 0, t[nt].info = 0, nt++; }
        t[i].n++, t[i].info |= op->info & 0xffff;
    }
    for (int i = 0; i < nt; i++)
        for (int j = i + 1; j < nt; j++)
            if (t[j].n > t[i].n) { __typeof__(t[0]) x = t[i]; t[i] = t[j]; t[j] = x; }
    LOG("[gpudiag] frame %llu: %u draws / dispatches, %d programs\n", (unsigned long long)frame_no, (unsigned)(nops - frame0), nt);
    for (int i = 0; i < nt; i++) {
        char nm[64];
        prog_name(t[i].key, nm, sizeof nm);
        LOG("[gpudiag]   %5u %5.2f%% %s%s%s\n", t[i].n, 100.0 * t[i].n / (nops - frame0 ? nops - frame0 : 1), nm,
            t[i].info & GD_I_FULL ? " full-screen" : "", t[i].info & GD_I_DISPATCH ? " compute" : "");
    }
}

void gd_frame(uint64_t draws_total) {
    static uint64_t last_draws;
    uint64_t drawn = draws_total - last_draws;
    last_draws = draws_total;
    frame_no++;
    pass_in_frame = 0;
    if (note.nskip) {  // a skipped program that is more than 2% of a frame's draws (30 frames running): drawn again
        uint64_t tries = 0;
        for (int i = 0; i < note.nskip; i++) tries += skip_tries[i];
        uint64_t all = drawn + tries;
        for (int i = 0; i < note.nskip; i++) {
            if (skip_active[i] && all >= 200 && skip_tries[i] * 50 > all) {
                if (++skip_common_frames[i] == 30) {
                    skip_active[i] = false, skip_removed[i] = true;
                    LOG("[vk] program %s is %.1f%% of the draws - too common to skip: drawn again (and off the skip list)\n",
                        note.skip[i].name, 100.0 * skip_tries[i] / all);
                    note_write();
                }
            } else skip_common_frames[i] = 0;
            skip_tries[i] = 0;
        }
    }
    {   // (test: LARECOMP_TEST_GPUDIAG_LIST=<frame>[,<frame>...]: the programs of the ring then, most common first - to pick
        // one for LARECOMP_TEST_GPUCRASH)
        static const char *e;
        static int init;
        if (!init) init = 1, e = getenv("LARECOMP_TEST_GPUDIAG_LIST");
        for (const char *p = e; p && *p;) {
            char *q;
            unsigned long long f = strtoull(p, &q, 10);
            if (q == p) break;
            if (f == frame_no) gd_list_programs();
            p = *q == ',' ? q + 1 : q;
        }
    }
    if (deep && note.armed && !(frame_no & 255)) {
        double m = clean_min();
        if (SDL_GetTicksNS() - deep_since_ns > (uint64_t)(m * 60e9)) {
            char why[48];
            snprintf(why, sizeof why, "%g minutes", m);
            deep_off(why);
        }
    }
}

// ---------------------------------------------------------------- the report
typedef struct { uint64_t key; unsigned n, ring_n, frame_n; uint32_t info; } Hit;
static int hit_add(Hit *h, int nh, int max, uint64_t key, uint32_t info) {
    for (int i = 0; i < nh; i++) if (h[i].key == key) { h[i].n++; h[i].info |= info; return nh; }
    if (nh >= max) return nh;
    h[nh] = (Hit){key, 1, 0, 0, info};
    return nh + 1;
}
static const char *mark_kind(int k) {
    return k == GD_M_PASS ? "pass" : k == GD_M_DISPATCH ? "dispatch" : k == GD_M_CB ? "command buffer start" : k == GD_M_DRAW ? "draw" : "?";
}
static void print_glsl(const GdProg *p) {
    if (!p) return;
    char nm[64];
    prog_name(p->key, nm, sizeof nm);
    if (p->kind == 2) LOG("[vk] ---- program %s: compute GLSL ----\n%s\n[vk] ---- end of program ----\n", nm, p->fs_glsl ? p->fs_glsl : "(none)");
    else LOG("[vk] ---- program %s: vertex GLSL ----\n%s\n[vk] ---- fragment GLSL ----\n%s\n[vk] ---- end of program ----\n", nm,
             p->vs_glsl ? p->vs_glsl : "(none)", p->fs_glsl ? p->fs_glsl : "(none)");
}

void gd_device_lost(const char *fault) {
    static atomic_int once;
    if (gd_off || atomic_exchange(&once, 1)) return;
    // (a snapshot: the recording thread may still be adding to the rings)
    uint64_t nops = g_gd_nops, nmarks = g_gd_nmarks;
    static GdOp ops[GD_OPS];
    static GdMark marks[GD_MARKS];
    memcpy(ops, g_gd_ops, sizeof ops);
    memcpy(marks, g_gd_marks, sizeof marks);
    uint64_t op_lo = nops > GD_OPS ? nops - GD_OPS : 0, mk_lo = nmarks > GD_MARKS ? nmarks - GD_MARKS : 0;
#define MK(m) (&marks[(m) & (GD_MARKS - 1)])
#define OP(o) (&ops[(o) & (GD_OPS - 1)])
    LOG("[vk] ==== GPU crash report (device lost): %s, driver %s, %s %s ====\n", cur_gpu, cur_drv, RT_APP_NAME, RT_APP_VERSION);
    if (fault) LOG("[vk] the driver says: %s\n", fault);
    uint64_t done = g_gd_done, v = 0;
    if (gd_dev && g_vk.timeline && vkGetSemaphoreCounterValue(gd_dev, g_vk.timeline, &v) == VK_SUCCESS && v > done) done = v;
    // the work in flight: [from, to) in op serials
    uint64_t from = op_lo, to = nops, m_bot = UINT64_MAX, m_top = 0;
    bool exact = false;
    if (get_cp && gd_queue) {
        VkCheckpointDataNV cd[32];
        uint32_t nc = 0;
        get_cp(gd_queue, &nc, NULL);
        if (nc > 32) nc = 32;
        for (uint32_t i = 0; i < nc; i++) cd[i] = (VkCheckpointDataNV){.sType = VK_STRUCTURE_TYPE_CHECKPOINT_DATA_NV};
        get_cp(gd_queue, &nc, cd);
        for (uint32_t i = 0; i < nc; i++) {
            uint64_t mv = (uint64_t)(uintptr_t)cd[i].pCheckpointMarker;
            if (!(mv & GD_TAG)) { LOG("[vk] checkpoint at stage %#x: not ours (%#llx)\n", (unsigned)cd[i].stage, (unsigned long long)mv); continue; }
            uint64_t m = mv & (GD_PASS_TAG - 1);  // (the mark's serial: its low 61 bits)
            bool known = m < nmarks && m >= mk_lo;
            const GdMark *k = known ? MK(m) : NULL;
            LOG("[vk] checkpoint: %s reached mark #%llu%s", (cd[i].stage & VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT) ? "top of pipe" :
                (cd[i].stage & VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT) ? "bottom of pipe" : "a stage", (unsigned long long)m, known ? "" : " (no longer in the ring)\n");
            if (k) LOG(" = %s, frame %u pass %u, submission %u\n", mark_kind(k->kind), k->frame, k->pass, k->seq);
            if (!known) continue;
            if (m < m_bot) m_bot = m;
            if (m > m_top) m_top = m;
        }
        if (m_bot <= m_top) {
            from = MK(m_bot)->op;
            to = m_top + 1 < nmarks ? MK(m_top + 1)->op : nops;  // (past the top mark: up to the next one)
            if (to < from + 1 && MK(m_top)->kind != GD_M_PASS) to = from + 1;
            exact = true;
        }
    }
    if (!exact) {  // the work submitted after the last finished submission
        uint64_t m = nmarks;
        while (m > mk_lo && MK(m - 1)->seq > (uint32_t)done) m--;
        if (m < nmarks) from = MK(m)->op;
        else from = nops > 64 ? nops - 64 : op_lo;  // (nothing known: the last 64)
        if (nops - from > 512) from = nops - 512;  // (keep it short: the most recent)
        LOG("[vk] no checkpoints: the work after the last finished submission (%llu): %u draws / dispatches\n", (unsigned long long)done, (unsigned)(nops - from));
    }
    if (from < op_lo) from = op_lo;
    if (to > nops) to = nops;
    // the passes in that span: from the pass (or dispatch) its first mark belongs to, up to its last mark
    uint64_t pass_of_first = UINT64_MAX;
    {
        uint64_t m1 = exact ? m_bot : nmarks;  // (no checkpoints: the first mark at or after the first op)
        if (!exact)
            for (uint64_t m = nmarks; m-- > mk_lo;) { if (MK(m)->op < from) break; m1 = m; }
        uint64_t m_end = exact ? m_top : nmarks ? nmarks - 1 : 0;
        uint64_t m0 = m1;
        while (m0 > mk_lo && m1 - m0 < 4096 && MK(m0)->kind != GD_M_PASS && MK(m0)->kind != GD_M_DISPATCH) m0--;
        int shown = 0;
        for (uint64_t m = m0; m <= m_end && m < nmarks && shown < 12; m++) {
            const GdMark *k = MK(m);
            if (k->kind != GD_M_PASS && k->kind != GD_M_DISPATCH) continue;
            if (pass_of_first == UINT64_MAX) pass_of_first = m;
            if (k->kind == GD_M_PASS)
                LOG("[vk] in flight: pass %u of frame %u - %d color target(s) %ux%u format %#x, submission %u\n", k->pass, k->frame,
                    k->ncolor, k->w, k->h, k->fmt, k->seq);
            else LOG("[vk] in flight: a compute dispatch (%ux%u groups) after pass %u of frame %u, submission %u\n", k->w, k->h, k->pass,
                     k->frame, k->seq);
            shown++;
        }
    }
    // the programs in that span (newest first), with their share of the whole ring (how common they are)
    Hit hit[64];
    int nh = 0;
    for (uint64_t o = to; o-- > from;) nh = hit_add(hit, nh, 64, OP(o)->key, OP(o)->info);
    unsigned ring_ops = nops - op_lo;
    for (uint64_t o = op_lo; o < nops; o++)
        for (int i = 0; i < nh; i++) if (hit[i].key == OP(o)->key) { hit[i].ring_n++; hit[i].info |= OP(o)->info & GD_I_FULL; break; }
    // (and of the frame the work in flight belongs to: a program common in the new scene is told even when the ring
    // still holds mostly the scene before - the first frame of an area)
    uint64_t f_lo = nops, f_hi = nops;
    uint32_t fr = from < nops ? MK(OP(from)->mark)->frame : 0;
    for (uint64_t m = mk_lo; m < nmarks; m++) {
        if (MK(m)->frame == fr && MK(m)->op < f_lo) f_lo = MK(m)->op;
        if (MK(m)->frame > fr && f_lo < nops) { f_hi = MK(m)->op; break; }
    }
    if (f_lo < op_lo) f_lo = op_lo;
    for (uint64_t o = f_lo; o < f_hi; o++)
        for (int i = 0; i < nh; i++) if (hit[i].key == OP(o)->key) { hit[i].frame_n++; break; }
    unsigned frame_ops = f_hi > f_lo ? f_hi - f_lo : 0;
#define SHARE(h) (fmax(ring_ops ? 100.0 * (h)->ring_n / ring_ops : 0, frame_ops >= 200 ? 100.0 * (h)->frame_n / frame_ops : 0))
    bool precise = exact && g_gd_drawmarks && nh <= 8;
    LOG("[vk] %s: %u draws / dispatches, %d program(s)%s\n", exact ? "on the GPU when it stopped (checkpoints)" : "submitted last", (unsigned)(to - from), nh,
        precise ? " - exact (a checkpoint at every draw)" : "");
    for (int i = 0; i < nh; i++) {
        const GdProg *p = prog_of(hit[i].key);
        char nm[64];
        prog_name(hit[i].key, nm, sizeof nm);
        double share = SHARE(&hit[i]);
        if (p && p->kind == 2) LOG("[vk]   program %s (game shader at %#llx): %u dispatch(es)\n", nm, (unsigned long long)p->fs_addr, hit[i].n);
        else LOG("[vk]   program %s (game shaders vs %#llx fs %#llx): %u draw(s)%s%s, %.2f%% of the draws\n", nm,
                 (unsigned long long)(p ? p->vs_addr : 0), (unsigned long long)(p ? p->fs_addr : 0), hit[i].n,
                 hit[i].info & GD_I_FULL ? ", full-screen" : "", hit[i].info & GD_I_INDIRECT ? ", indirect" : "", share);
    }
    if (nh == 1) {
        char nm[64];
        prog_name(hit[0].key, nm, sizeof nm);
        const GdMark *k = pass_of_first != UINT64_MAX ? MK(pass_of_first) : NULL;
        if (k && k->kind == GD_M_PASS)
            LOG("[vk] the GPU stopped in program %s (pass %u of frame %u, %ux%u)\n", nm, k->pass, k->frame, k->w, k->h);
        else LOG("[vk] the GPU stopped in program %s\n", nm);
    }
    // ---- the crash note: this crash, then the skip list
    int64_t now = (int64_t)time(NULL);
    bool had_crash = note.ncrash > 0;
    if (note.ncrash == MAX_CRASH) { memmove(&note.crash[0], &note.crash[1], sizeof note.crash[0] * (MAX_CRASH - 1)); note.ncrash--; }
    GdCrash *c = &note.crash[note.ncrash];
    memset(c, 0, sizeof *c);
    c->time = now, c->precise = precise;
    for (int i = 0; i < nh && c->n < CRASH_KEYS; i++) {
        c->keys[c->n] = hit[i].key;
        prog_name(hit[i].key, c->names[c->n], sizeof c->names[0]);
        c->common[c->n] = (hit[i].info & GD_I_FULL) || SHARE(&hit[i]) > 2.0;
        c->n++;
    }
    // On the GPU at an earlier crash too (one of the two exact: a checkpoint at every draw): skipped from now on. Not when
    // a common (> 2% of the draws) or full-screen program was there both times - the crash may be in that one, and the
    // others only drawn next to it (the same pass, the same order); not when more than 3 are left (not sure which).
    uint64_t cand[CRASH_KEYS];
    int ncand = 0;
    for (int k = 0; k < note.ncrash; k++) {
        const GdCrash *e = &note.crash[k];
        if (!precise && !e->precise) continue;
        uint64_t both[CRASH_KEYS];
        int nb = 0;
        bool guarded = false;
        for (int i = 0; i < c->n; i++) {
            bool in = false;
            for (int j = 0; j < e->n; j++) in |= e->keys[j] == c->keys[i];
            if (!in) continue;
            const Hit *h = NULL;
            for (int j = 0; j < nh; j++) if (hit[j].key == c->keys[i]) h = &hit[j];
            if (h && (h->info & GD_I_FULL)) {
                LOG("[vk] program %s was on the GPU at two crashes, but it is a full-screen pass: not skipped\n", c->names[i]);
                guarded = true;
            } else if (h && SHARE(h) > 2.0) {
                LOG("[vk] program %s was on the GPU at two crashes, but it is %.1f%% of the draws: not skipped\n", c->names[i], SHARE(h));
                guarded = true;
            } else both[nb++] = c->keys[i];
        }
        if (guarded && nb) {
            LOG("[vk] %d other program(s) were on the GPU at both crashes beside it: not skipped either (the crash may be in it)\n", nb);
            continue;
        }
        if (nb > 3) { LOG("[vk] %d programs were on the GPU at both crashes: none skipped (not sure which)\n", nb); continue; }
        for (int i = 0; i < nb; i++) {
            bool dup = gd_skip_lookup(both[i]) != 0;
            for (int j = 0; j < ncand; j++) dup |= cand[j] == both[i];
            if (!dup && ncand < CRASH_KEYS) cand[ncand++] = both[i];
        }
    }
    note.ncrash++;
    for (int i = 0; i < ncand && note.nskip < MAX_SKIP; i++) {
        note.skip[note.nskip].key = cand[i];
        prog_name(cand[i], note.skip[note.nskip].name, sizeof note.skip[0].name);
        LOG("[vk] program %s crashed the GPU twice on driver %s: skipped from now on\n", note.skip[note.nskip].name, cur_drv);
        note.nskip++;
    }
    note.armed = 1, note.armed_at = now;
    note.vendor = cur_vendor, note.device = cur_device, note.driver = cur_driver;
    note_write();
    LOG("[vk] crash note written (%s): the next start checks more closely%s\n", NOTE, note.nskip ? " and skips the programs above" : "");
    // a repeat crash with the deeper mode on: the code of the programs that were on the GPU
    if (deep && had_crash) {
        int shown = 0;
        for (int i = 0; i < nh && shown < 4; i++) {
            if (!precise && i >= 4) break;
            const GdProg *p = prog_of(hit[i].key);
            if (p) print_glsl(p), shown++;
        }
    }
#undef MK
#undef OP
#undef SHARE
}

// ---------------------------------------------------------------- test only
char *gd_test_inject(const char *vs, uint64_t fs_hash) {
    const char *e = getenv("LARECOMP_TEST_GPUCRASH");
    if (!e || !vs || !fs_hash) return NULL;
    bool want = false;
    for (const char *p = e; *p;) {  // (hex hashes, comma separated)
        char *q;
        uint64_t h = strtoull(p, &q, 16);
        if (q == p) break;
        want |= h == fs_hash;
        p = q;
        while (*p == ',' || *p == ' ') p++;
    }
    const char *m = want ? strstr(vs, "void main() {") : NULL;
    if (!m) return NULL;
    static const char decl[] = "layout(buffer_reference, std430, buffer_reference_align = 4) buffer GdCrashW { uint v; };\n";
    static const char body[] = "void main() {\n  GdCrashW(0x7ead00000000ul + uint64_t(gl_VertexIndex) * 256ul).v = 1u;  // (LARECOMP_TEST_GPUCRASH)\n";
    size_t pre = (size_t)(m - vs), len = strlen(vs), ml = strlen("void main() {\n");
    if (strncmp(m, "void main() {\n", ml)) ml = strlen("void main() {");
    char *s = malloc(len + sizeof decl + sizeof body);
    memcpy(s, vs, pre);
    memcpy(s + pre, decl, sizeof decl - 1);
    memcpy(s + pre + sizeof decl - 1, body, sizeof body - 1);
    memcpy(s + pre + sizeof decl - 1 + sizeof body - 1, m + ml, len - pre - ml + 1);
    LOG("[test] program fs %016llx: its vertex shader writes where nothing is mapped (LARECOMP_TEST_GPUCRASH): a GPU crash\n",
        (unsigned long long)fs_hash);
    return s;
}
