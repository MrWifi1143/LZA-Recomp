// Maxwell (GM20B) shader translation to Metal Shading Language (macOS) or Vulkan GLSL (Windows; compiled to
// SPIR-V at runtime with shaderc). ShaderKey.glsl selects the dialect; the MSL output does not depend on it.
//
// GLSL contract (what the Vulkan renderer binds):
//  - `#version 460`, entry point `main`. Descriptor set: vertex 0, fragment 1, compute 0.
//  - hardware cbuf k -> binding k: `layout(std140, set=S, binding=k) uniform CB<k> { uvec4 cb<k>[N]; };`
//    (anonymous block). N = max(cbuf_size[k] / 16, highest constant-index vec4 + 1), and at least 4096 when
//    cbuf_dynamic has bit k (the renderer binds up to 64 KB). Register-indexed reads are clamped to N - 1.
//  - global memory (LDG/STG) -> binding GMEM_BUFFER_INDEX (17):
//    `layout(std430, set=S, binding=17) [readonly] buffer GMem { uint w[]; } gmem_;`, 32-bit words relative to
//    maxwell_gmem_base. `readonly` unless the shader stores (needs vertexPipeline/fragmentStoresAndAtomics then);
//    byte / short stores are atomicAnd + atomicOr on the containing word.
//  - rscale table -> binding 18: `uniform RScale { vec4 rscale[65]; };` (std140, the MSL buffer 18 contents).
//  - per-draw tint -> binding 19: `uniform Tint { vec4 ltint[2]; };` (fragment, when uses_tint).
//  - texture slot i (ShaderInfo.tex[i]) -> binding 32 + i, a combined image sampler: sampler1D/2D/3D/Cube/
//    1DArray/2DArray/CubeArray, or its *Shadow variant when tex[i].shadow.
//  - image i (ShaderInfo.img[i]: SUST / SURED, NVN image slot img[i].slot) -> binding 64 + i, a storage image:
//    `writeonly [i|u]image<dim>` (stores, needs shaderStorageImageWriteWithoutFormat; the type from key->img_kind) or
//    `r32ui uimage<dim>` (atomics).
//  - vertex inputs `layout(location=i) in vec4|ivec4|uvec4 a<i>;` (key->attr_kind); varyings: location i =
//    generic attribute i, `vec4 v<i>` (fragment side flat / noperspective as in MSL); `invariant gl_Position`;
//    gl_PointSize when writes_point_size. Fragment outputs `layout(location=rt) out vec4|ivec4|uvec4 c<rt>;`,
//    gl_FragDepth when writes_depth.
//  - clip space exactly as the MSL output (key->flip_y / depth_minus_one epilogue unchanged): the renderer uses
//    a negative-height viewport so Vulkan matches Metal's y-up NDC, clip z in [0, 1].
//  - compute: `layout(local_size_x_id = 0, local_size_y_id = 1, local_size_z_id = 2) in;` (spec constants 0..2).
//  - extensions: GL_EXT_shader_explicit_arithmetic_types_int64 (64-bit carry / address math -> shaderInt64),
//    GL_KHR_shader_subgroup_basic/_shuffle/_shuffle_relative (SHFL, FSWZADD lane: fragment/compute subgroup
//    support), GL_EXT_texture_shadow_lod (explicit-LOD array / cube shadow lookups).
//  - not in GLSL: the maxwell_dbg_vs position dump (buffer 29) is MSL only.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum { STAGE_VERTEX = 0, STAGE_FRAGMENT = 1, STAGE_GEOMETRY = 2, STAGE_TESS_CTRL = 3, STAGE_TESS_EVAL = 4,
       STAGE_COMPUTE = 5 };

enum { TEXTYPE_1D, TEXTYPE_2D, TEXTYPE_3D, TEXTYPE_CUBE, TEXTYPE_2D_ARRAY, TEXTYPE_CUBE_ARRAY, TEXTYPE_1D_ARRAY,
       TEXTYPE_BUFFER, TEXTYPE_2D_MS };

typedef struct {
    uint8_t attr_kind[32];  // vertex inputs: 0 float, 1 signed int, 2 unsigned int
    uint8_t rt_kind[8];     // fragment outputs: 0 float, 1 signed int, 2 unsigned int
    bool depth_minus_one;   // NVN depth mode [-1,1]: remap clip z
    bool flip_y;            // lower-left window origin
    uint32_t vs_out_mask;   // fragment: varyings the vertex shader provides
    uint8_t glsl;           // emit Vulkan GLSL (see the contract above) instead of MSL
    uint8_t xfb_count[4];   // vertex: transform feedback capture (exec.c): components per buffer, 0 = none
    uint16_t xfb_stride[4]; //   and the buffers' strides in bytes
    uint8_t img_kind[8];    // images bound per NVN slot: 0 float, 1 signed int, 2 unsigned int (the GLSL image type)
    uint32_t tex_stencil;   // NVN texture slots (bit i: handle at 0x20 + 8 * i) bound to a depth-stencil texture in
                            // stencil mode: read as unsigned integers (usampler)
} ShaderKey;

typedef struct {
    uint32_t handle_offset;  // byte offset of the texture handle in the bound texture cbuf
    uint8_t type;            // TEXTYPE_*
    bool shadow, is_array, is_image, bindless;
    bool stencil;            // a depth-stencil texture's stencil (key->tex_stencil): usampler, integer results
    uint32_t bindless_cbuf, bindless_offset;
} ShaderTexture;

enum { SUDIM_1D, SUDIM_1D_BUFFER, SUDIM_1D_ARRAY, SUDIM_2D, SUDIM_2D_ARRAY, SUDIM_3D };  // SUST / SURED .dim
typedef struct {
    uint8_t slot;  // NVN image binding (its handle at driver cbuf 2, 0x120 + 8 * slot)
    uint8_t dim;   // SUDIM_*
    bool atomic;   // r32ui, written with atomics (SURED); otherwise stores (SUST.P)
    uint8_t kind;  // ShaderKey.img_kind of the slot when translated: image / iimage / uimage
} ShaderImage;

typedef struct {
    int stage;
    char *msl;              // complete MSL or GLSL source (MSL entry "main0", GLSL "main"); NULL on failure
    uint32_t cbuf_mask;     // hardware constant buffers read
    uint32_t cbuf_size[18];
    uint32_t cbuf_dynamic;  // cbufs indexed with a register: bind the whole buffer
    int ntex;
    ShaderTexture tex[64];
    uint32_t attr_mask;     // vertex: input attributes; fragment: input varyings
    uint32_t out_mask;      // vertex: output varyings; fragment: render targets written
    bool writes_depth, uses_discard, writes_point_size;
    bool uses_gmem;         // global memory access: bind the guest heap at GMEM_BUFFER_INDEX
    bool gmem_stores;       // ... and it stores: the shader writes guest memory (through its storage buffers, or any pointer)
    bool uses_tint;         // fragment: reads the per-draw tint (buffer 19, float4[2]) applied to color target 0
    bool uses_rscale;       // reads the resolution-scale table (fragment/compute buffer 18): float4 [0] = render
                            // target scale, [1 + i] = allocation scale of texture i
    uint32_t local_mem;     // bytes of local memory
    uint32_t num_insts;
    int nimg;
    ShaderImage img[8];
    char error[256];        // set when translation fails
} ShaderInfo;

// Global memory (LDG/STG) is addressed through one buffer that wraps the guest heap.
#define GMEM_BUFFER_INDEX 17
extern uint64_t maxwell_gmem_base, maxwell_gmem_size;
// The GPU's subgroups are wider than 32 and can't be made 32 (Adreno: 64 / 128): warp operations work on each 32-lane
// slice as a warp (lane id, masks, votes, shuffles). Set before the first translation (vulkan/device.c).
extern bool g_maxwell_warp_slices;
// Debug: when translating the shader whose code starts at maxwell_dbg_code, output register
// maxwell_dbg_reg as captured after the instruction at maxwell_dbg_pc (greyscale, all targets).
extern const uint8_t *maxwell_dbg_code;
extern uint32_t maxwell_dbg_pc;
extern int maxwell_dbg_reg;
// Debug: the vertex shader at maxwell_dbg_vs writes each vertex's position to buffer 18 (slot iid*8+vid).
extern const uint8_t *maxwell_dbg_vs;
#define DBG_VS_BUFFER_INDEX 29

// code: shader binary (with the 0x50-byte SPH header for graphics stages)
ShaderInfo *maxwell_translate(const uint8_t *code, size_t max_size, int stage, const ShaderKey *key);
// consts: the program's compiler constants (c1, nconsts bytes; NULL: unknown), whose jump tables name BRX's targets
ShaderInfo *maxwell_translate_consts(const uint8_t *code, size_t max_size, const uint8_t *consts, size_t nconsts, int stage,
                                     const ShaderKey *key);
void maxwell_free(ShaderInfo *s);
const char *maxwell_build_id(void);  // the translator's version: a hash of its source (its caches key on it)
const char *maxwell_cache_tag(void); // the same as 16 hex digits: the caches' folder (cache/xlat/<tag>, cache/spirv/<tag>)
// Disassemble for debugging (writes text to f).
void maxwell_disassemble(const uint8_t *code, size_t max_size, int stage, void *file);
