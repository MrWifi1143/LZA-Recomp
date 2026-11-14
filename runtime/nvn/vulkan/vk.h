// Vulkan backend for the NVN layer (Windows; macOS uses runtime/nvn/metal.m + renderer.m).
// Files: device.c (instance, window, swapchain, present, screenshots, ImGui hooks), textures.c (images, uploads,
// views), exec.c (command execution: passes, draws, clears, copies, compute), shaders.c (GLSL -> SPIR-V, programs,
// pipelines). Mirrors the Metal backend's behaviour; differences are noted where they happen.
//
// Synchronisation model: every image lives in VK_IMAGE_LAYOUT_GENERAL, and a full memory barrier separates
// render passes, copies and dispatches (Metal tracks these hazards itself). Submissions are numbered; a timeline
// semaphore signals each number, which is what NVN fences (renderer_fence_seq / renderer_wait_seq) wait for.
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <vulkan/vulkan.h>
#if __has_include(<vma/vk_mem_alloc.h>)
#include <vma/vk_mem_alloc.h>
#else
#include <vk_mem_alloc.h>  // (Debian / Ubuntu libvulkan-memory-allocator-dev)
#endif

#include "../nvn.h"
#include "../maxwell/shader.h"

void vk_device_fault_report(void);
void vk_pipeline_cache_save(void);  // cache/vk_pipelines.bin (any thread)
#define VK_CHECK(x) do { VkResult r_ = (x); if (r_ != VK_SUCCESS) { if (r_ == VK_ERROR_DEVICE_LOST) vk_device_fault_report(); \
    FATAL("%s failed: %d (%s:%d)\n", #x, r_, __FILE__, __LINE__); } } while (0)

typedef struct {
    VkInstance inst;
    VkPhysicalDevice phys;
    VkDevice dev;
    VkQueue queue;
    uint32_t qfam;
    VmaAllocator vma;
    VkPhysicalDeviceProperties props;
    VkPipelineCache pcache;
    VkSemaphore timeline;      // value = last completed submission number
    VkCommandPool pool;        // render thread only
    bool custom_border, divisor, host_import;
    bool mem_budget;  // VK_EXT_memory_budget (VMA's heap budgets are the driver's: texture packs keep below them)
    bool xfb;  // VK_EXT_transform_feedback (NVN transform feedback: PLA skins its characters with it)
    bool depth_clamp;  // depthClamp feature (NVN SetDepthClamp)
    bool gpl;          // VK_EXT_graphics_pipeline_library with fast linking (exec.c: pipelines from parts)
    bool gpuheap;  // the guest heap window is GPU memory (gpuheap.c: no host memory import), a sparse buffer in exec.c
    bool lazyhost;  // the host memory import made in 64 MB chunks as the game uses them, a sparse buffer (gpuheap.c, exec.c)
    VkShaderStageFlags sg32;  // stages that get a required subgroup size of 32 (the Maxwell warp: vk_sg32)
    bool oq_host_reset, oq_precise;  // occlusion queries (exec.c): slots reset from the CPU; exact sample counts
    bool minmax;                     // samplerFilterMinmax: min / max reduction samplers
    PFN_vkCmdBindTransformFeedbackBuffersEXT bind_xfb;
    PFN_vkCmdBeginTransformFeedbackEXT begin_xfb;
    PFN_vkCmdEndTransformFeedbackEXT end_xfb;
    // window
    VkSurfaceKHR surface;
    VkSwapchainKHR swapchain;
    VkFormat sc_format;
    VkExtent2D sc_extent;
    uint32_t sc_count;
    VkImage sc_images[8];
    VkImageView sc_views[8];
} VkG;
// Guest shaders assume 32-lane warps (ballots, lane ids and masks, shuffles): a stage of g_vk.sg32 is given a required
// subgroup size of 32 (GPUs whose default differs: AMD's wave64, Intel's 8 / 16); NVIDIA is always 32 (sg32 = 0).
static inline const void *vk_sg32(VkShaderStageFlagBits stage) {
    static const VkPipelineShaderStageRequiredSubgroupSizeCreateInfo r = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_REQUIRED_SUBGROUP_SIZE_CREATE_INFO, .requiredSubgroupSize = 32};
    extern VkG g_vk;
    return (g_vk.sg32 & stage) ? &r : NULL;
}
extern VkG g_vk;

// ---- submissions (exec.c)
// Work is recorded into the current command buffer; uploads go into a separate one that runs first.
VkCommandBuffer vk_cmd(void);         // current command buffer (outside any rendering unless exec.c began one)
VkCommandBuffer vk_upload_cmd(void);  // upload command buffer of the pending submission
uint64_t vk_pending_seq(void);        // number the pending submission will get
uint64_t vk_completed_seq(void);
void vk_wait_seq(uint64_t seq);
void vk_full_barrier(VkCommandBuffer cb);
void vk_end_pass(void);
void vk_submit_extra(VkSemaphore wait, VkSemaphore signal, VkCommandBuffer extra);  // submit + one more cb (present)

// Transient GPU-visible memory that lives until the submission it was allocated for completes.
typedef struct { VkBuffer buf; uint64_t off; void *ptr; } VkSpan;
VkSpan vk_transient(uint64_t size, uint64_t align);
VkSpan vk_transient_copy(const void *data, uint64_t size, uint64_t align);
VkSpan vk_readback(uint64_t size);  // host-cached GPU -> CPU staging (read right after the copy completes)
VkDescriptorSet vk_alloc_set(VkDescriptorSetLayout layout);

// ---- textures (textures.c)
typedef struct TexBackend {
    VkImage image;
    VmaAllocation mem;
    VkFormat format;
    VkImageViewType base_type;
    uint32_t width, height, depth, layers, levels, samples;
    VkImageAspectFlags aspect;
    bool gpu_owned, uploaded, dynamic;
    bool glyph;          // a button icon (runtime/nvn/glyphs.c): uploaded again when glyphs_generation() moves on
    uint32_t glyph_gen;
    uint64_t data_hash, sample_hash, checked_frame, full_frame;
    bool scalable, presented;
    uint64_t touched_frame, frame_cleared;
    uint32_t level_gen;
    float sx, sy;
    void *pending_rgba;  // host-produced pixels (video) waiting to be uploaded on the render thread
    uint32_t pending_w, pending_h, pending_stride;
    uint8_t astc_bw, astc_bh;  // ASTC without GPU support: decoded to RGBA8 on upload (runtime/nvn/astc.c)
    bool astc_srgb;
    int reg;  // slot in textures.c's live list + 1 (a reboot destroys them all)
    uint64_t dirty_seq;  // last host write (backend_pool_written) this texture was checked against
    bool storage;        // created with storage usage (an NVN image: SUST / SURED write it)
    bool texpack_seen;   // looked up in the texture pack (texpack.c) once
    bool texpack;        // replaced by the pack's image: the game's data isn't uploaded into it
    bool texpack_wait;   // its replacement is being read (texpack_key): the game's own until then
    uint64_t texpack_key;
    VkImageUsageFlags usage;
} TexBackend;

TexBackend *vk_tex(NvnTexture *t, bool for_render);  // backend image (uploads CPU contents unless for_render)
VkFormat vk_format(uint32_t nvn_format);
bool vk_format_alpha_one(uint32_t nvn_format);
// View of t (optionally through an NVN texture view) for rendering or sampling.
VkImageView vk_view(NvnTexture *t, const NvnTextureView *v, bool has_view, bool render, int want_textype);
VkImageView vk_image_view(TexBackend *b, VkImageViewType type, VkFormat fmt, uint32_t lv0, uint32_t nlv, uint32_t ly0,
                          uint32_t nly, VkComponentMapping sw, VkImageAspectFlags aspect);
VkImageView vk_dummy_view(int textype);
VkImageView vk_storage_view(NvnTexture *t, const NvnTextureView *v, bool has_view, int sudim);  // NULL handle: not a storage image
VkImageView vk_dummy_storage_view(int sudim, bool atomic);
VkSampler vk_sampler(NvnSamplerDesc *d);
VkSampler vk_sampler_for(NvnSamplerDesc *d, bool shadow);  // (textures.c: no compare for non-.DC reads)
VkSampler vk_default_sampler(bool linear);
void vk_image_init_layout(VkCommandBuffer cb, VkImage img, VkImageAspectFlags aspect);
// Destruction once every submission that may use the object has completed.
void vk_defer_image(VkImage img, VmaAllocation mem);
void vk_defer_view(VkImageView v);
void vk_views_forget(TexBackend *b);  // drop (deferred) the cached views of an image being replaced
uint64_t vk_frame_counter(void);
bool vcache_get(uint64_t addr, uint64_t len, VkBuffer *buf, VkDeviceSize *off);  // vertex stream in VRAM (vcache.c)
void vcache_reset(void);
void vcache_invalidate(uint64_t addr, uint64_t len);  // the GPU may write there from now on: no copies of it
extern uint64_t g_vcache_hits, g_vcache_uploads;

// ---- shaders / pipelines (shaders.c)
VkShaderModule vk_compile_glsl(const char *src, int stage, const char *name);  // NULL on failure (logged)
VkShaderModule vk_builtin_shader(const char *name);  // internal shaders (clear, copy, present)

// ---- device / window (device.c)
void vk_save_png(const char *path, const uint8_t *rgba, uint32_t w, uint32_t h);
double vk_gpu_frame_update(void);  // exec.c: GPU time per presented frame, averaged (-1: not yet); once per present
double vk_gpu_ms(void);            // (its last value)
