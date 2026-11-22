// GPU crash diagnostics (gpudiag.c): what the GPU was running when the driver reports a lost device, a crash note
// (gpucrash.json) that arms deeper diagnostics for the next start, and a skip list for a program that crashed the GPU
// twice on the same driver. Shared by both recomps (same file in each).
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <vulkan/vulkan.h>

// The ring of recorded GPU work: every draw / dispatch (op) and every point a checkpoint can name (mark: a pass, a
// dispatch, a command buffer's start, and with per-draw marks each draw). Written by the thread that records (one at
// a time), read after a device loss. Serials only grow; slot = serial % size.
#define GD_OPS 32768
#define GD_MARKS 32768
typedef struct { uint64_t key, mark; uint32_t info; } GdOp;  // key: gd_key_draw / gd_key_cs; mark: the last mark before it
typedef struct { uint64_t op; uint32_t seq, frame; uint16_t pass, w, h; uint8_t kind, ncolor; uint32_t fmt; } GdMark;
enum { GD_M_PASS = 1, GD_M_DISPATCH, GD_M_CB, GD_M_DRAW };
// op info: kind (bits 0-1: 1 draw, 2 dispatch), GD_I_FULL a full-screen-style draw (non-indexed, <= 4 vertices, one
// instance: never skipped), GD_I_INDIRECT, instances << 16 (clamped)
#define GD_I_DRAW 1u
#define GD_I_DISPATCH 2u
#define GD_I_FULL 4u
#define GD_I_INDIRECT 8u
extern GdOp g_gd_ops[GD_OPS];
extern GdMark g_gd_marks[GD_MARKS];
extern uint64_t g_gd_nops, g_gd_nmarks;  // (64-bit: ~200k draws a second wrap 32 bits in 6 hours)
extern bool g_gd_cp;        // NVIDIA checkpoints set (VK_NV_device_diagnostic_checkpoints)
extern bool g_gd_drawmarks; // a mark before every draw (the deeper mode, or LARECOMP_GPUDIAG_DRAWMARKS=1)
extern uint64_t g_gd_seq;   // the submission being recorded (exec.c)
extern uint64_t g_gd_done;  // the last submission seen finished (exec.c vk_wait_seq)

uint64_t gd_mark(VkCommandBuffer cb, int kind, uint32_t w, uint32_t h, uint32_t fmt, int ncolor);
static inline void gd_op(VkCommandBuffer cb, uint64_t key, uint32_t info) {
    if (g_gd_drawmarks) gd_mark(cb, GD_M_DRAW, 0, 0, 0, 0);
    GdOp *o = &g_gd_ops[g_gd_nops & (GD_OPS - 1)];
    o->key = key, o->info = info, o->mark = g_gd_nmarks - 1;
    g_gd_nops++;
}

// A program's identity, stable between runs: its fragment and vertex shaders' code hashes (vk_fs_hash's), or a
// compute shader's. Never 0.
static inline uint64_t gd_key_draw(uint64_t fs_hash, uint64_t vs_hash) {
    uint64_t k = fs_hash ^ (vs_hash * 0x9E3779B97F4A7C15ull) ^ 0x5bd1e995ull;
    return k ? k : 1;
}
static inline uint64_t gd_key_cs(uint64_t cs_hash) {
    uint64_t k = (cs_hash * 0xC2B2AE3D27D4EB4Full) ^ 0x27d4eb2full;
    return k ? k : 1;
}
// Names a program for the report (once per program, when its key is made): its hashes, the game's shader addresses,
// its GLSL (kept as long as the program). kind: 1 draw, 2 compute (vs_* then unused, fs_* = the compute shader).
void gd_register(uint64_t key, int kind, uint64_t vs_hash, uint64_t fs_hash, uint64_t vs_addr, uint64_t fs_addr,
                 const char *vs_glsl, const char *fs_glsl);

// Device setup (device.c, before vkCreateDevice): reads the crash note, decides the mode. Returns whether to enable
// VK_NV_device_diagnostic_checkpoints (has_cp: the device offers it). drv: the driver version as its vendor writes it.
bool gd_setup(const VkPhysicalDeviceProperties *props, const char *drv, bool has_cp, bool trace_owns_checkpoints);
void gd_device_ready(VkDevice dev, VkQueue queue);  // after vkCreateDevice
// After VK_ERROR_DEVICE_LOST (any thread, once): the report, the crash note. fault: the driver's description (or NULL).
void gd_device_lost(const char *fault);
void gd_frame(uint64_t draws_total);  // each present (the skip list's share guard, the deeper mode's clean timer)
// The skip list: index + 1 when this program is on it (0: not). gd_skip_hit counts a try and says whether to skip.
int gd_skip_lookup(uint64_t key);
// The deeper mode: this program was on the GPU at the last crash - drawn in a pass of its own between full barriers
bool gd_isolate_lookup(uint64_t key);
bool gd_skip_hit(int idx);
// Test only (LARECOMP_TEST_GPUCRASH=<fs hash>[,<fs hash>...]): that program's vertex shader writes to an address
// nothing maps - a GPU page fault, a real device loss. Returns the new GLSL, NULL when not asked for this program.
char *gd_test_inject(const char *vs_glsl, uint64_t fs_hash);
