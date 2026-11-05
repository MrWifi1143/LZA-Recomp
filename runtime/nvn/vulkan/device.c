// Vulkan device, window, swapchain and presentation (counterpart of the window/present half of metal.m).
#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../../paths.h"
#include "../../settings.h"
#include "../../ui/menu.h"
#include "vk.h"
void rt_trace(char ev, uint64_t arg);  // (renderthread.c: research timeline)
#include "../../plugin.h"
#include "../output_scale.h"
#include "../renderer.h"

VkG g_vk;
static SDL_Window *window;
static atomic_bool ui_ready;
static VkDebugUtilsMessengerEXT messenger;

// ================================================================== instance / device
static VKAPI_ATTR VkBool32 VKAPI_CALL debug_cb(VkDebugUtilsMessageSeverityFlagBitsEXT sev, VkDebugUtilsMessageTypeFlagsEXT type,
                                               const VkDebugUtilsMessengerCallbackDataEXT *d, void *user) {
    (void)type, (void)user;
    static int n;
    if (sev >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT && n++ < 500) LOG("[vk] %s\n", d->pMessage);
    return VK_FALSE;
}

// LARECOMP_VK_VALIDATION=1: Khronos validation layer (MSYS2 package vulkan-validation-layers)
static void create_instance(void) {
    Uint32 n_sdl = 0;
    const char *const *sdl_ext = SDL_Vulkan_GetInstanceExtensions(&n_sdl);
    const char *ext[48];
    uint32_t n_ext = 0;
    for (Uint32 i = 0; i < n_sdl && n_ext < 14; i++) ext[n_ext++] = sdl_ext[i];
    {   // plugins' extensions (the VR mod: OpenXR's), once each
        const char *pe[24];
        uint32_t np = plugins_vk_instance_extensions(pe, 24);
        for (uint32_t i = 0; i < np && n_ext < 46; i++) {
            bool dup = false;
            for (uint32_t k = 0; k < n_ext; k++) dup |= !strcmp(ext[k], pe[i]);
            if (!dup) ext[n_ext++] = pe[i];
        }
    }
    bool validate = ENV("LARECOMP_VK_VALIDATION") != NULL;
    if (validate) ext[n_ext++] = VK_EXT_DEBUG_UTILS_EXTENSION_NAME;
    const char *layers[] = {"VK_LAYER_KHRONOS_validation"};
    VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = "LARecomp",
                             .pEngineName = "LARecomp", .apiVersion = VK_API_VERSION_1_3};
    VkInstanceCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app,
                               .enabledExtensionCount = n_ext, .ppEnabledExtensionNames = ext,
                               .enabledLayerCount = validate ? 1 : 0, .ppEnabledLayerNames = layers};
    VkResult r = vkCreateInstance(&ci, NULL, &g_vk.inst);
    if (r != VK_SUCCESS && validate) {
        LOG("[vk] validation layer unavailable (%d); continuing without\n", r);
        ci.enabledLayerCount = 0;
        ci.enabledExtensionCount--;
        validate = false;
        r = vkCreateInstance(&ci, NULL, &g_vk.inst);
    }
    if (r != VK_SUCCESS) FATAL("vkCreateInstance: %d (is a Vulkan 1.3 driver installed?)\n", r);
    if (validate) {
        PFN_vkCreateDebugUtilsMessengerEXT f =
            (PFN_vkCreateDebugUtilsMessengerEXT)vkGetInstanceProcAddr(g_vk.inst, "vkCreateDebugUtilsMessengerEXT");
        VkDebugUtilsMessengerCreateInfoEXT mi = {
            .sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,
            .messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT,
            .messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                           VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT,
            .pfnUserCallback = debug_cb};
        if (f) f(g_vk.inst, &mi, NULL, &messenger);
    }
}

bool g_pipe_exec_stats;  // LARECOMP_PIPE_STATS and the driver has VK_KHR_pipeline_executable_properties

static bool has_ext(VkExtensionProperties *e, uint32_t n, const char *name) {
    for (uint32_t i = 0; i < n; i++) if (!strcmp(e[i].extensionName, name)) return true;
    return false;
}

// Discrete GPU first (LARECOMP_GPU=<index> picks one), needs Vulkan 1.3 and a queue that can present.
static void pick_device(void) {
    uint32_t n = 0;
    vkEnumeratePhysicalDevices(g_vk.inst, &n, NULL);
    if (!n) FATAL("no Vulkan device\n");
    VkPhysicalDevice *pd = calloc(n, sizeof *pd);
    vkEnumeratePhysicalDevices(g_vk.inst, &n, pd);
    int want = ENV("LARECOMP_GPU") ? atoi(ENV("LARECOMP_GPU")) : -1, best = -1, best_score = -1;
    for (uint32_t i = 0; i < n; i++) {
        VkPhysicalDeviceProperties p;
        vkGetPhysicalDeviceProperties(pd[i], &p);
        LOG("[vk] gpu %u: %s (api %u.%u.%u)\n", i, p.deviceName, VK_API_VERSION_MAJOR(p.apiVersion),
            VK_API_VERSION_MINOR(p.apiVersion), VK_API_VERSION_PATCH(p.apiVersion));
        if (p.apiVersion < VK_API_VERSION_1_3) continue;
        int score = p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 3 : p.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? 2 : 1;
        if ((int)i == want) score = 100;
        if (score > best_score) { best_score = score; best = (int)i; }
    }
    if (best < 0) FATAL("no Vulkan 1.3 device\n");
    g_vk.phys = pd[best];
    VkPhysicalDevice want_pd = plugins_vk_physical_device(g_vk.inst);  // (a plugin's: OpenXR names the HMD's GPU)
    if (want_pd) g_vk.phys = want_pd;
    free(pd);
    vkGetPhysicalDeviceProperties(g_vk.phys, &g_vk.props);
    uint32_t nq = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(g_vk.phys, &nq, NULL);
    VkQueueFamilyProperties *qp = calloc(nq, sizeof *qp);
    vkGetPhysicalDeviceQueueFamilyProperties(g_vk.phys, &nq, qp);
    g_vk.qfam = UINT32_MAX;
    for (uint32_t i = 0; i < nq; i++) {
        VkBool32 present = VK_FALSE;
        vkGetPhysicalDeviceSurfaceSupportKHR(g_vk.phys, i, g_vk.surface, &present);
        if ((qp[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && (qp[i].queueFlags & VK_QUEUE_COMPUTE_BIT) && present) { g_vk.qfam = i; break; }
    }
    free(qp);
    if (g_vk.qfam == UINT32_MAX) FATAL("no graphics+present queue\n");
    LOG("[vk] using %s\n", g_vk.props.deviceName);
}

static bool g_fault_ext;
// After VK_ERROR_DEVICE_LOST: what the driver says faulted (VK_EXT_device_fault), into the log.
void vk_device_fault_report(void) {
    static int once;
    if (once++ || !g_fault_ext) return;
    PFN_vkGetDeviceFaultInfoEXT get = (PFN_vkGetDeviceFaultInfoEXT)vkGetDeviceProcAddr(g_vk.dev, "vkGetDeviceFaultInfoEXT");
    if (!get) return;
    VkDeviceFaultCountsEXT c = {.sType = VK_STRUCTURE_TYPE_DEVICE_FAULT_COUNTS_EXT};
    if (get(g_vk.dev, &c, NULL) != VK_SUCCESS) { LOG("[vk] device fault: no info\n"); return; }
    VkDeviceFaultAddressInfoEXT addr[16];
    VkDeviceFaultVendorInfoEXT vend[16];
    if (c.addressInfoCount > 16) c.addressInfoCount = 16;
    if (c.vendorInfoCount > 16) c.vendorInfoCount = 16;
    c.vendorBinarySize = 0;
    VkDeviceFaultInfoEXT info = {.sType = VK_STRUCTURE_TYPE_DEVICE_FAULT_INFO_EXT, .pAddressInfos = addr, .pVendorInfos = vend};
    if (get(g_vk.dev, &c, &info) != VK_SUCCESS && 0) return;
    LOG("[vk] device fault: %s\n", info.description);
    static const char *const kind[] = {"none", "read invalid", "write invalid", "execute invalid", "ip unknown", "ip invalid", "ip valid"};
    for (uint32_t i = 0; i < c.addressInfoCount; i++)
        LOG("[vk]   address %s %#llx (precision %#llx)\n", addr[i].addressType < 7 ? kind[addr[i].addressType] : "?",
            (unsigned long long)addr[i].reportedAddress, (unsigned long long)addr[i].addressPrecision);
    for (uint32_t i = 0; i < c.vendorInfoCount; i++)
        LOG("[vk]   vendor: %s (code %#llx data %#llx)\n", vend[i].description, (unsigned long long)vend[i].vendorFaultCode,
            (unsigned long long)vend[i].vendorFaultData);
}

static bool g_display_csv;
static void create_device(void) {
    uint32_t ne = 0;
    vkEnumerateDeviceExtensionProperties(g_vk.phys, NULL, &ne, NULL);
    VkExtensionProperties *ep = calloc(ne, sizeof *ep);
    vkEnumerateDeviceExtensionProperties(g_vk.phys, NULL, &ne, ep);
    const char *ext[48];
    uint32_t n_ext = 0;
    ext[n_ext++] = VK_KHR_SWAPCHAIN_EXTENSION_NAME;
    g_vk.custom_border = has_ext(ep, ne, VK_EXT_CUSTOM_BORDER_COLOR_EXTENSION_NAME);
    if (g_vk.custom_border) ext[n_ext++] = VK_EXT_CUSTOM_BORDER_COLOR_EXTENSION_NAME;
    g_vk.divisor = has_ext(ep, ne, VK_EXT_VERTEX_ATTRIBUTE_DIVISOR_EXTENSION_NAME);
    if (g_vk.divisor) ext[n_ext++] = VK_EXT_VERTEX_ATTRIBUTE_DIVISOR_EXTENSION_NAME;
    g_vk.host_import = has_ext(ep, ne, VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME);  // guest heap as a GPU buffer (exec.c)
    if (g_vk.host_import) ext[n_ext++] = VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME;
    {   // without it: the heap window made of GPU memory (gpuheap.c), imported as dma-bufs into a sparse buffer (exec.c)
        extern bool gpuheap_on(void);
        uint32_t nq = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(g_vk.phys, &nq, NULL);
        VkQueueFamilyProperties qp[16];
        if (nq > 16) nq = 16;
        vkGetPhysicalDeviceQueueFamilyProperties(g_vk.phys, &nq, qp);
        g_vk.gpuheap = gpuheap_on() && g_vk.qfam < nq && (qp[g_vk.qfam].queueFlags & VK_QUEUE_SPARSE_BINDING_BIT);
        if (g_vk.gpuheap) {
            g_vk.host_import = false;  // (LARECOMP_DMABUF_HEAP=1 where both exist: this way)
            if (has_ext(ep, ne, VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME)) n_ext--;
            ext[n_ext++] = VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME;
            ext[n_ext++] = VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME;
        } else if (gpuheap_on())
            LOG("[vk] warning: the guest heap is GPU memory but the queue can't bind it: shader global memory reads return zero\n");
    }
    g_vk.xfb = has_ext(ep, ne, VK_EXT_TRANSFORM_FEEDBACK_EXTENSION_NAME);
    if (g_vk.xfb) ext[n_ext++] = VK_EXT_TRANSFORM_FEEDBACK_EXTENSION_NAME;
    // pipelines linked from separately built parts (exec.c gpl_link); LARECOMP_VK_GPL=0: whole pipelines as before
    bool gpl = has_ext(ep, ne, VK_KHR_PIPELINE_LIBRARY_EXTENSION_NAME) && has_ext(ep, ne, VK_EXT_GRAPHICS_PIPELINE_LIBRARY_EXTENSION_NAME) &&
               !(ENV("LARECOMP_VK_GPL") && !atoi(ENV("LARECOMP_VK_GPL")));
    if (gpl) {
        VkPhysicalDeviceGraphicsPipelineLibraryFeaturesEXT gf = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_GRAPHICS_PIPELINE_LIBRARY_FEATURES_EXT};
        VkPhysicalDeviceFeatures2 hf = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &gf};
        vkGetPhysicalDeviceFeatures2(g_vk.phys, &hf);
        VkPhysicalDeviceGraphicsPipelineLibraryPropertiesEXT gp = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_GRAPHICS_PIPELINE_LIBRARY_PROPERTIES_EXT};
        VkPhysicalDeviceProperties2 hp = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &gp};
        vkGetPhysicalDeviceProperties2(g_vk.phys, &hp);
        gpl = gf.graphicsPipelineLibrary && gp.graphicsPipelineLibraryFastLinking;
    }
    if (gpl) ext[n_ext++] = VK_KHR_PIPELINE_LIBRARY_EXTENSION_NAME, ext[n_ext++] = VK_EXT_GRAPHICS_PIPELINE_LIBRARY_EXTENSION_NAME;
    LOG("[vk] pipeline libraries: %s\n", gpl ? "yes (pipelines linked from parts)" : "no (whole pipelines)");
    bool fault = has_ext(ep, ne, VK_EXT_DEVICE_FAULT_EXTENSION_NAME);  // device-lost diagnostics (vk_device_fault_report)
    if (fault) ext[n_ext++] = VK_EXT_DEVICE_FAULT_EXTENSION_NAME;
    // LARECOMP_PIPE_STATS: what the driver says about each compiled shader (registers, spills, ...), exec.c pipe_exec_report
    g_pipe_exec_stats = ENV("LARECOMP_PIPE_STATS") && has_ext(ep, ne, VK_KHR_PIPELINE_EXECUTABLE_PROPERTIES_EXTENSION_NAME);
    if (g_pipe_exec_stats) ext[n_ext++] = VK_KHR_PIPELINE_EXECUTABLE_PROPERTIES_EXTENSION_NAME;
    else if (ENV("LARECOMP_PIPE_STATS")) LOG("[vk] LARECOMP_PIPE_STATS: the driver has no VK_KHR_pipeline_executable_properties\n");
    g_fault_ext = fault;
    // LARECOMP_DISPLAY_CSV=<file> (research): when each frame reached the screen (present ids + a thread waiting on them)
    g_display_csv = ENV("LARECOMP_DISPLAY_CSV") && has_ext(ep, ne, VK_KHR_PRESENT_ID_EXTENSION_NAME) &&
                    has_ext(ep, ne, VK_KHR_PRESENT_WAIT_EXTENSION_NAME);
    if (g_display_csv) ext[n_ext++] = VK_KHR_PRESENT_ID_EXTENSION_NAME, ext[n_ext++] = VK_KHR_PRESENT_WAIT_EXTENSION_NAME;
    else if (ENV("LARECOMP_DISPLAY_CSV")) LOG("[vk] LARECOMP_DISPLAY_CSV: no VK_KHR_present_id / present_wait\n");
    {   // plugins' device extensions, once each and only those the device has
        const char *pe[24];
        uint32_t np = plugins_vk_device_extensions(g_vk.phys, pe, 24);
        for (uint32_t i = 0; i < np && n_ext < 46; i++) {
            bool dup = false;
            for (uint32_t k = 0; k < n_ext; k++) dup |= !strcmp(ext[k], pe[i]);
            if (!dup && has_ext(ep, ne, pe[i])) ext[n_ext++] = pe[i];
            else if (!dup) LOG("[plugin] device extension %s: not available\n", pe[i]);
        }
    }
    free(ep);

    // features the backend relies on (all present on current desktop drivers)
    VkPhysicalDeviceVulkan12Features have12 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    VkPhysicalDeviceFeatures2 have = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &have12};
    vkGetPhysicalDeviceFeatures2(g_vk.phys, &have);
    g_vk.oq_host_reset = have12.hostQueryReset;  // (occlusion queries, exec.c: slots reset from the CPU)
    g_vk.minmax = have12.samplerFilterMinmax;    // (samplers with a min / max reduction: textures.c sampler_create)
    VkPhysicalDeviceVertexAttributeDivisorFeaturesEXT div = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_ATTRIBUTE_DIVISOR_FEATURES_EXT,
        .vertexAttributeInstanceRateDivisor = VK_TRUE, .vertexAttributeInstanceRateZeroDivisor = VK_FALSE};
    VkPhysicalDeviceCustomBorderColorFeaturesEXT cbc = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CUSTOM_BORDER_COLOR_FEATURES_EXT, .customBorderColors = VK_TRUE,
        .customBorderColorWithoutFormat = VK_TRUE, .pNext = g_vk.divisor ? &div : NULL};
    VkPhysicalDeviceVulkan13Features f13 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
                                            .dynamicRendering = VK_TRUE, .synchronization2 = VK_TRUE,
                                            .shaderDemoteToHelperInvocation = VK_TRUE, .shaderTerminateInvocation = VK_TRUE,
                                            .maintenance4 = VK_TRUE,
                                            .pNext = g_vk.custom_border ? (void *)&cbc : g_vk.divisor ? (void *)&div : NULL};
    VkPhysicalDeviceVulkan12Features f12 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
                                            .timelineSemaphore = VK_TRUE, .shaderInt8 = VK_TRUE, .samplerMirrorClampToEdge = VK_TRUE,
                                            .uniformAndStorageBuffer8BitAccess = have12.uniformAndStorageBuffer8BitAccess,  // (not on Adreno / Turnip)
                                            .storageBuffer8BitAccess = VK_TRUE,
                                            .scalarBlockLayout = VK_TRUE, .shaderOutputLayer = VK_TRUE,
                                            .hostQueryReset = have12.hostQueryReset, .samplerFilterMinmax = have12.samplerFilterMinmax,
                                            .bufferDeviceAddress = VK_TRUE,  // (Vulkan 1.3 requires it: the guest heap's address, exec.c gmem_ref_info)
                                            .pNext = &f13};
    VkPhysicalDeviceVulkan11Features f11 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES,
                                            .storageBuffer16BitAccess = VK_TRUE, .shaderDrawParameters = VK_TRUE, .pNext = &f12};
    VkPhysicalDeviceFaultFeaturesEXT ff = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FAULT_FEATURES_EXT, .deviceFault = VK_TRUE,
                                           .pNext = &f11};
    VkPhysicalDeviceFeatures2 f = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = fault ? (void *)&ff : (void *)&f11};
    VkPhysicalDeviceTransformFeedbackFeaturesEXT xff = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TRANSFORM_FEEDBACK_FEATURES_EXT,
                                                        .transformFeedback = VK_TRUE, .pNext = f.pNext};
    if (g_vk.xfb) f.pNext = &xff;
    VkPhysicalDevicePipelineExecutablePropertiesFeaturesKHR pe = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_EXECUTABLE_PROPERTIES_FEATURES_KHR, .pipelineExecutableInfo = VK_TRUE,
        .pNext = f.pNext};
    if (g_pipe_exec_stats) f.pNext = &pe;
    VkPhysicalDeviceGraphicsPipelineLibraryFeaturesEXT gplf = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_GRAPHICS_PIPELINE_LIBRARY_FEATURES_EXT, .graphicsPipelineLibrary = VK_TRUE, .pNext = f.pNext};
    if (gpl) f.pNext = &gplf;
    g_vk.gpl = gpl;
    VkPhysicalDevicePresentIdFeaturesKHR pidf = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_ID_FEATURES_KHR, .presentId = VK_TRUE,
                                                 .pNext = f.pNext};
    VkPhysicalDevicePresentWaitFeaturesKHR pwf = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_WAIT_FEATURES_KHR,
                                                  .presentWait = VK_TRUE, .pNext = &pidf};
    if (g_display_csv) f.pNext = &pwf;
    {   // subgroup size control (vk_sg32): only where 32 is possible but not the only size
        extern bool g_maxwell_warp_slices;  // (LARECOMP_WARP_SLICES=1: the sliced warp operations anywhere - a 32-wide
        g_maxwell_warp_slices = ENV("LARECOMP_WARP_SLICES") != NULL;  // subgroup is one slice: the same results, a test)
        VkPhysicalDeviceVulkan13Features h13 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
        VkPhysicalDeviceFeatures2 hf = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &h13};
        vkGetPhysicalDeviceFeatures2(g_vk.phys, &hf);
        VkPhysicalDeviceVulkan13Properties p13 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_PROPERTIES};
        VkPhysicalDeviceProperties2 hp = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &p13};
        vkGetPhysicalDeviceProperties2(g_vk.phys, &hp);
        LOG("[vk] subgroup size %u..%u\n", p13.minSubgroupSize, p13.maxSubgroupSize);
        const VkShaderStageFlags vfc = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT;
        if (h13.subgroupSizeControl && p13.minSubgroupSize <= 32 && p13.maxSubgroupSize >= 32 && p13.minSubgroupSize != p13.maxSubgroupSize) {
            f13.subgroupSizeControl = VK_TRUE;
            g_vk.sg32 = p13.requiredSubgroupSizeStages & vfc;
            LOG("[vk] guest shaders run in 32-wide subgroups (stages %#x of %#x the driver can fix at 32)\n", g_vk.sg32,
                p13.requiredSubgroupSizeStages);
            // A stage the driver can't fix at 32 (AMD's Windows driver: compute only) runs at its own width - wave64 on a
            // Radeon - and the guest's warp operations (lane ids, ballots, votes, shuffles) there saw 64 lanes: wrong
            // results in vertex / fragment shaders (a Radeon owner's trees out of the intro's train window didn't draw).
            // Such stages get the 32-lane slices; on a stage fixed at 32 a slice is the whole subgroup (the same results).
            if (p13.maxSubgroupSize > 32 && (g_vk.sg32 & vfc) != vfc && !ENV("LARECOMP_NO_WARP_SLICES")) {
                g_maxwell_warp_slices = true;
                LOG("[vk] subgroups up to %u lanes in stages not fixed at 32: guest shaders' warp operations work on 32-lane "
                    "slices\n", p13.maxSubgroupSize);
            }
        } else if (p13.minSubgroupSize > 32 && !ENV("LARECOMP_NO_WARP_SLICES")) {
            g_maxwell_warp_slices = true;
            LOG("[vk] subgroups of %u..%u lanes: guest shaders' warp operations work on 32-lane slices\n", p13.minSubgroupSize,
                p13.maxSubgroupSize);
        } else if (p13.minSubgroupSize != 32 || p13.maxSubgroupSize != 32)
            LOG("[vk] warning: no 32-wide subgroups: guest shaders' warp operations may misbehave\n");
    }
    VkPhysicalDeviceFeatures *b = &f.features, *h = &have.features;
    b->independentBlend = h->independentBlend;
    b->occlusionQueryPrecise = h->occlusionQueryPrecise;  // (the game's SAMPLES_PASSED counters: exact counts)
    g_vk.oq_precise = h->occlusionQueryPrecise;
    b->dualSrcBlend = h->dualSrcBlend;
    b->fillModeNonSolid = h->fillModeNonSolid;
    b->samplerAnisotropy = h->samplerAnisotropy;
    b->textureCompressionBC = h->textureCompressionBC;
    b->shaderInt64 = h->shaderInt64;
    b->depthBiasClamp = h->depthBiasClamp;
    b->depthClamp = h->depthClamp;
    g_vk.depth_clamp = h->depthClamp;
    b->largePoints = h->largePoints;
    b->wideLines = h->wideLines;
    b->imageCubeArray = h->imageCubeArray;
    b->sampleRateShading = h->sampleRateShading;
    b->fragmentStoresAndAtomics = h->fragmentStoresAndAtomics;
    b->shaderStorageImageWriteWithoutFormat = h->shaderStorageImageWriteWithoutFormat;  // (SUST: images of any format)
    b->geometryShader = h->geometryShader;  // (gl_Layer read by fragment shaders: the Geometry capability)
    b->vertexPipelineStoresAndAtomics = h->vertexPipelineStoresAndAtomics;
    b->pipelineStatisticsQuery = h->pipelineStatisticsQuery;  // (LARECOMP_GPUPROF_STATS: invocations per draw)
    b->robustBufferAccess = h->robustBufferAccess;  // guest shaders index cbufs freely: keep OOB reads harmless
    if (ENV("LARECOMP_NO_ROBUST")) b->robustBufferAccess = VK_FALSE;  // (research: what the bounds checks cost)
    b->shaderClipDistance = h->shaderClipDistance;
    b->sparseBinding = b->sparseResidencyBuffer = g_vk.gpuheap;  // (exec.c: the heap window's sparse buffer)
    if (!h->shaderInt64) LOG("[vk] warning: no shaderInt64 (64-bit address math in shaders)\n");
    if (!have12.uniformAndStorageBuffer8BitAccess) LOG("[vk] warning: no 8-bit uniform buffer access\n");
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qi = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = g_vk.qfam,
                                  .queueCount = 1, .pQueuePriorities = &prio};
    VkDeviceCreateInfo di = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .pNext = &f, .queueCreateInfoCount = 1,
                             .pQueueCreateInfos = &qi, .enabledExtensionCount = n_ext, .ppEnabledExtensionNames = ext};
    VK_CHECK(vkCreateDevice(g_vk.phys, &di, NULL, &g_vk.dev));
    vkGetDeviceQueue(g_vk.dev, g_vk.qfam, 0, &g_vk.queue);
    plugins_vk_ready(g_vk.inst, g_vk.phys, g_vk.dev, g_vk.qfam, g_vk.queue);
    if (g_vk.xfb) {
        g_vk.bind_xfb = (PFN_vkCmdBindTransformFeedbackBuffersEXT)vkGetDeviceProcAddr(g_vk.dev, "vkCmdBindTransformFeedbackBuffersEXT");
        g_vk.begin_xfb = (PFN_vkCmdBeginTransformFeedbackEXT)vkGetDeviceProcAddr(g_vk.dev, "vkCmdBeginTransformFeedbackEXT");
        g_vk.end_xfb = (PFN_vkCmdEndTransformFeedbackEXT)vkGetDeviceProcAddr(g_vk.dev, "vkCmdEndTransformFeedbackEXT");
        if (!g_vk.bind_xfb || !g_vk.begin_xfb || !g_vk.end_xfb) g_vk.xfb = false;
    }
    LOG("[vk] transform feedback: %s\n", g_vk.xfb ? "yes" : "no");

    VmaAllocatorCreateInfo ai = {.physicalDevice = g_vk.phys, .device = g_vk.dev, .instance = g_vk.inst,
                                 .vulkanApiVersion = VK_API_VERSION_1_3};
    VK_CHECK(vmaCreateAllocator(&ai, &g_vk.vma));

    VkSemaphoreTypeCreateInfo st = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
                                    .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE, .initialValue = 0};
    VkSemaphoreCreateInfo si = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, .pNext = &st};
    VK_CHECK(vkCreateSemaphore(g_vk.dev, &si, NULL, &g_vk.timeline));
    VkCommandPoolCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                  .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, .queueFamilyIndex = g_vk.qfam};
    VK_CHECK(vkCreateCommandPool(g_vk.dev, &pi, NULL, &g_vk.pool));

    // pipeline cache: cache/vk_pipelines.bin (written by vk_pipeline_cache_save)
    size_t sz = 0;
    void *data = NULL;
    FILE *fp = fopen("cache/vk_pipelines.bin", "rb");
    if (fp) {
        fseek(fp, 0, SEEK_END);
        sz = (size_t)ftell(fp);
        fseek(fp, 0, SEEK_SET);
        data = malloc(sz);
        if (fread(data, 1, sz, fp) != sz) sz = 0;
        fclose(fp);
    }
    VkPipelineCacheCreateInfo pc = {.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO, .initialDataSize = sz,
                                    .pInitialData = data};
    if (vkCreatePipelineCache(g_vk.dev, &pc, NULL, &g_vk.pcache) != VK_SUCCESS) {
        pc.initialDataSize = 0;
        VK_CHECK(vkCreatePipelineCache(g_vk.dev, &pc, NULL, &g_vk.pcache));
    }
    free(data);
}

void vk_pipeline_cache_save(void) {  // (exit and exec.c's periodic save: one writer of the .tmp at a time)
    static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_lock(&lock);
    size_t sz = 0;
    if (vkGetPipelineCacheData(g_vk.dev, g_vk.pcache, &sz, NULL) != VK_SUCCESS || !sz) { pthread_mutex_unlock(&lock); return; }
    void *data = malloc(sz);
    if (vkGetPipelineCacheData(g_vk.dev, g_vk.pcache, &sz, data) == VK_SUCCESS) {
        mkdir("cache", 0755);
        FILE *f = fopen("cache/vk_pipelines.bin.tmp", "wb");
        if (f) {
            bool ok = fwrite(data, 1, sz, f) == sz;
            fclose(f);
            if (ok) { remove("cache/vk_pipelines.bin"); rename("cache/vk_pipelines.bin.tmp", "cache/vk_pipelines.bin"); }
        }
    }
    free(data);    pthread_mutex_unlock(&lock);
}

// ================================================================== swapchain
static VkPresentModeKHR present_mode(void) {
    // MAILBOX by default: the game paces itself (vi.c's emulated 60 Hz vsync), and with FIFO every submission
    // queued behind the one waiting for a window image, so the game's fence waits followed the presentation engine:
    // a throttled / captured / busy compositor slowed the whole game (4-21 fps with the GPU mostly idle; mailbox:
    // 60). No tearing. LARECOMP_VK_PRESENT=fifo|immediate|mailbox to choose.
    const char *e = ENV("LARECOMP_VK_PRESENT");
    VkPresentModeKHR want = !e || !strcmp(e, "mailbox") ? VK_PRESENT_MODE_MAILBOX_KHR
                          : !strcmp(e, "immediate") ? VK_PRESENT_MODE_IMMEDIATE_KHR : VK_PRESENT_MODE_FIFO_KHR;
    uint32_t n = 0;
    vkGetPhysicalDeviceSurfacePresentModesKHR(g_vk.phys, g_vk.surface, &n, NULL);
    VkPresentModeKHR m[16];
    if (n > 16) n = 16;
    vkGetPhysicalDeviceSurfacePresentModesKHR(g_vk.phys, g_vk.surface, &n, m);
    VkPresentModeKHR got = VK_PRESENT_MODE_FIFO_KHR;
    for (uint32_t i = 0; i < n; i++) if (m[i] == want) got = want;
    static const char *const names[] = {"immediate", "mailbox", "fifo", "fifo relaxed"};
    static int logged;
    if (!logged++) {
        LOG("[vk] present mode %s (wanted %s; the window offers", got < 4 ? names[got] : "?", want < 4 ? names[want] : "?");
        for (uint32_t i = 0; i < n; i++) LOG(" %s", m[i] < 4 ? names[m[i]] : "other");
        LOG(")\n");
    }
    return got;
}

static void create_swapchain(void) {
    VkSurfaceCapabilitiesKHR caps;
    VK_CHECK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(g_vk.phys, g_vk.surface, &caps));
    int w, h;
    SDL_GetWindowSizeInPixels(window, &w, &h);
    VkExtent2D ext = caps.currentExtent.width != UINT32_MAX ? caps.currentExtent : (VkExtent2D){(uint32_t)w, (uint32_t)h};
    if (!ext.width || !ext.height) return;  // minimised
    uint32_t nf = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(g_vk.phys, g_vk.surface, &nf, NULL);
    VkSurfaceFormatKHR fmts[64];
    if (nf > 64) nf = 64;
    vkGetPhysicalDeviceSurfaceFormatsKHR(g_vk.phys, g_vk.surface, &nf, fmts);
    // scanout shows the bytes the game wrote (see the raw views in the present pass): a UNORM swapchain like Metal's
    VkSurfaceFormatKHR sf = fmts[0];
    for (uint32_t i = 0; i < nf; i++)
        if (fmts[i].format == VK_FORMAT_B8G8R8A8_UNORM && fmts[i].colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) { sf = fmts[i]; break; }
    uint32_t count = caps.minImageCount + 1;
    if (caps.maxImageCount && count > caps.maxImageCount) count = caps.maxImageCount;
    if (count > 8) count = 8;
    VkSwapchainKHR old = g_vk.swapchain;
    VkSwapchainCreateInfoKHR ci = {
        .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR, .surface = g_vk.surface, .minImageCount = count,
        .imageFormat = sf.format, .imageColorSpace = sf.colorSpace, .imageExtent = ext, .imageArrayLayers = 1,
        .imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE, .preTransform = caps.currentTransform,
        .compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR, .presentMode = present_mode(), .clipped = VK_TRUE,
        .oldSwapchain = old};
    VK_CHECK(vkCreateSwapchainKHR(g_vk.dev, &ci, NULL, &g_vk.swapchain));
    for (uint32_t i = 0; i < g_vk.sc_count; i++) vkDestroyImageView(g_vk.dev, g_vk.sc_views[i], NULL);
    if (old) vkDestroySwapchainKHR(g_vk.dev, old, NULL);
    g_vk.sc_format = sf.format;
    g_vk.sc_extent = ext;
    g_vk.sc_count = 8;
    vkGetSwapchainImagesKHR(g_vk.dev, g_vk.swapchain, &g_vk.sc_count, g_vk.sc_images);
    for (uint32_t i = 0; i < g_vk.sc_count; i++) {
        VkImageViewCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = g_vk.sc_images[i],
                                    .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = sf.format,
                                    .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
        VK_CHECK(vkCreateImageView(g_vk.dev, &vi, NULL, &g_vk.sc_views[i]));
    }
}

static void recreate_swapchain(void) {
    vkDeviceWaitIdle(g_vk.dev);
    create_swapchain();
}

// ================================================================== frames (start screen and game)
// Acquire semaphores rotate per frame in flight; a slot is reused once the submission that waited on it is done.
#define FRAMES 3
static VkSemaphore acquire_sem[FRAMES], done_sem[8];
static uint64_t slot_seq[FRAMES];
static int slot;

static VkSemaphore binary_semaphore(void) {
    VkSemaphoreCreateInfo si = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    VkSemaphore s;
    VK_CHECK(vkCreateSemaphore(g_vk.dev, &si, NULL, &s));
    return s;
}

static void image_barrier(VkCommandBuffer cb, VkImage img, VkImageLayout from, VkImageLayout to) {
    VkImageMemoryBarrier2 b = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                               .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, .srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT,
                               .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                               .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
                               .oldLayout = from, .newLayout = to, .image = img,
                               .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
                               .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED};
    VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &b};
    vkCmdPipelineBarrier2(cb, &di);
}

typedef struct { uint32_t image; VkSemaphore acquired; bool ok; } Frame;
static Frame cur_frame;

// Acquire the next window image and begin rendering into it (cleared to `clear`). Returns false when there is
// nothing to draw into (minimised window).
bool vk_frame_begin(const float clear[4]) {
    cur_frame.ok = false;
    // The frame's batched lists (exec.c backend_execute) go now: in the present's submission they would wait for the
    // window image (its acquire semaphore), and the GPU can run them while the acquire blocks
    extern void renderer_submit(void);
    renderer_submit();
    int w, h;
    SDL_GetWindowSizeInPixels(window, &w, &h);
    if (!w || !h) return false;  // minimised
    if (!g_vk.swapchain || (uint32_t)w != g_vk.sc_extent.width || (uint32_t)h != g_vk.sc_extent.height) recreate_swapchain();
    if (!g_vk.swapchain) return false;
    static int trace = -1;
    if (trace < 0) trace = ENV("LARECOMP_VK_TRACE") != NULL;
    if (trace) LOG("[vk] frame begin %dx%d (swapchain %ux%u)\n", w, h, g_vk.sc_extent.width, g_vk.sc_extent.height);
    slot = (slot + 1) % FRAMES;
    if (!acquire_sem[slot]) acquire_sem[slot] = binary_semaphore();
    vk_wait_seq(slot_seq[slot]);
    rt_trace('A', 0);
    VkResult r = vkAcquireNextImageKHR(g_vk.dev, g_vk.swapchain, UINT64_MAX, acquire_sem[slot], VK_NULL_HANDLE, &cur_frame.image);
    rt_trace('a', 0);
    if (r == VK_ERROR_OUT_OF_DATE_KHR) {
        recreate_swapchain();
        r = vkAcquireNextImageKHR(g_vk.dev, g_vk.swapchain, UINT64_MAX, acquire_sem[slot], VK_NULL_HANDLE, &cur_frame.image);
    }
    static unsigned acquire_fails;  // (a window the driver keeps reporting out of date: logged once per 600, not every frame)
    if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) {
        if (acquire_fails++ % 600 == 0) LOG("[vk] acquire failed: %d (%u times)\n", r, acquire_fails);
        return false;
    }
    acquire_fails = 0;
    cur_frame.acquired = acquire_sem[slot];
    slot_seq[slot] = vk_pending_seq();
    VkCommandBuffer cb = vk_cmd();
    image_barrier(cb, g_vk.sc_images[cur_frame.image], VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    VkRenderingAttachmentInfo ca = {.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = g_vk.sc_views[cur_frame.image],
                                    .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                    .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
                                    .clearValue.color.float32 = {clear[0], clear[1], clear[2], clear[3]}};
    VkRenderingInfo ri = {.sType = VK_STRUCTURE_TYPE_RENDERING_INFO, .renderArea = {{0, 0}, g_vk.sc_extent}, .layerCount = 1,
                          .colorAttachmentCount = 1, .pColorAttachments = &ca};
    vkCmdBeginRendering(cb, &ri);
    VkViewport vp = {0, 0, (float)g_vk.sc_extent.width, (float)g_vk.sc_extent.height, 0, 1};
    VkRect2D sc = {{0, 0}, g_vk.sc_extent};
    vkCmdSetViewport(cb, 0, 1, &vp);
    vkCmdSetScissor(cb, 0, 1, &sc);
    cur_frame.ok = true;
    return true;
}

// Passes of their own between vk_frame_begin and vk_frame_end (exec.c post passes): end the window image's
// rendering, and begin it again keeping what was drawn.
void vk_frame_suspend(void) {
    if (cur_frame.ok) vkCmdEndRendering(vk_cmd());
}
void vk_frame_resume(void) {
    if (!cur_frame.ok) return;
    VkCommandBuffer cb = vk_cmd();
    VkRenderingAttachmentInfo ca = {.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = g_vk.sc_views[cur_frame.image],
                                    .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                    .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD, .storeOp = VK_ATTACHMENT_STORE_OP_STORE};
    VkRenderingInfo ri = {.sType = VK_STRUCTURE_TYPE_RENDERING_INFO, .renderArea = {{0, 0}, g_vk.sc_extent}, .layerCount = 1,
                          .colorAttachmentCount = 1, .pColorAttachments = &ca};
    vkCmdBeginRendering(cb, &ri);
    VkViewport vp = {0, 0, (float)g_vk.sc_extent.width, (float)g_vk.sc_extent.height, 0, 1};
    VkRect2D sc = {{0, 0}, g_vk.sc_extent};
    vkCmdSetViewport(cb, 0, 1, &vp);
    vkCmdSetScissor(cb, 0, 1, &sc);
}

VkExtent2D vk_frame_extent(void) { return g_vk.sc_extent; }
void *vk_frame_cmd(void) { return (void *)vk_cmd(); }

static void save_window_image(VkCommandBuffer cb, VkSpan *out) {
    uint32_t w = g_vk.sc_extent.width, h = g_vk.sc_extent.height;
    *out = vk_readback((uint64_t)w * h * 4);
    image_barrier(cb, g_vk.sc_images[cur_frame.image], VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    VkBufferImageCopy rg = {.bufferOffset = out->off, .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                            .imageExtent = {w, h, 1}};
    vkCmdCopyImageToBuffer(cb, g_vk.sc_images[cur_frame.image], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, out->buf, 1, &rg);
    image_barrier(cb, g_vk.sc_images[cur_frame.image], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
}

// LARECOMP_DISPLAY_CSV=<file> (research): "<present id>,<ns presented by us>,<ns shown>,<game frame>" per present. A thread
// waits for each id (vkWaitForPresentKHR returns once that present, or a later one, is on the screen): ids shown at the
// same time as the next one were replaced before a refresh showed them (mailbox), and the gaps between shown times are
// what the screen did - whole refreshes.
static uint64_t g_pid_next, g_pid_presented;
static uint64_t g_pid_t[4096], g_pid_frame[4096];
static uint64_t display_log_id(void) { return ++g_pid_next; }
static void *display_log_thread(void *arg) {
    (void)arg;
    FILE *f = fopen(ENV("LARECOMP_DISPLAY_CSV"), "w");
    if (!f) return NULL;
    fprintf(f, "id,presented_ns,shown_ns,game_frame\n");
    for (uint64_t id = 1;; id++) {
        while (__atomic_load_n(&g_pid_presented, __ATOMIC_ACQUIRE) < id) plat_sleep_until_ns(plat_now_ns() + 200000);
        VkSwapchainKHR sc = g_vk.swapchain;
        static PFN_vkWaitForPresentKHR wait;
        if (!wait) wait = (PFN_vkWaitForPresentKHR)vkGetDeviceProcAddr(g_vk.dev, "vkWaitForPresentKHR");
        VkResult r = wait(g_vk.dev, sc, id, 1000000000ull);
        uint64_t t = plat_now_ns();
        fprintf(f, "%llu,%llu,%llu,%llu%s\n", (unsigned long long)id, (unsigned long long)g_pid_t[id % 4096],
                r == VK_SUCCESS || r == VK_SUBOPTIMAL_KHR ? (unsigned long long)t : 0ull, (unsigned long long)g_pid_frame[id % 4096],
                r == VK_SUCCESS || r == VK_SUBOPTIMAL_KHR ? "" : ",fail");
        if (id % 64 == 0) fflush(f);
    }
    return NULL;
}
static void display_log_presented(uint64_t id) {
    extern uint64_t renderer_game_frame(void);
    g_pid_t[id % 4096] = plat_now_ns(), g_pid_frame[id % 4096] = renderer_game_frame();
    __atomic_store_n(&g_pid_presented, id, __ATOMIC_RELEASE);
    static bool started;
    if (!started) {
        started = true;
        pthread_t th;
        pthread_create(&th, NULL, display_log_thread, NULL);
        pthread_detach(th);
    }
}

// End rendering into the window image and present it. shot_path: also save the window image as PNG.
void vk_frame_end(const char *shot_path) {
    if (!cur_frame.ok) return;
    VkCommandBuffer cb = vk_cmd();
    vkCmdEndRendering(cb);
    VkSpan shot = {0};
    if (shot_path) save_window_image(cb, &shot);
    image_barrier(cb, g_vk.sc_images[cur_frame.image], VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
    uint32_t img = cur_frame.image;
    if (!done_sem[img]) done_sem[img] = binary_semaphore();
    uint64_t seq = vk_pending_seq();
    vk_submit_extra(cur_frame.acquired, done_sem[img], VK_NULL_HANDLE);
    VkPresentInfoKHR pi = {.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR, .waitSemaphoreCount = 1, .pWaitSemaphores = &done_sem[img],
                           .swapchainCount = 1, .pSwapchains = &g_vk.swapchain, .pImageIndices = &img};
    uint64_t pid = 0;
    VkPresentIdKHR pidi = {.sType = VK_STRUCTURE_TYPE_PRESENT_ID_KHR, .swapchainCount = 1, .pPresentIds = &pid};
    if (g_display_csv) pid = display_log_id(), pi.pNext = &pidi;
    vk_queue_lock();
    rt_trace('Q', 0);
    VkResult r = vkQueuePresentKHR(g_vk.queue, &pi);
    rt_trace('q', 0);
    vk_queue_unlock();
    if (g_display_csv) display_log_presented(pid);
    if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) recreate_swapchain();
    cur_frame.ok = false;
    if (shot_path) {
        vk_wait_seq(seq);
        uint32_t w = g_vk.sc_extent.width, h = g_vk.sc_extent.height;
        uint8_t *px = shot.ptr;
        bool bgr = g_vk.sc_format == VK_FORMAT_B8G8R8A8_UNORM || g_vk.sc_format == VK_FORMAT_B8G8R8A8_SRGB;
        for (uint64_t i = 0; bgr && i < (uint64_t)w * h; i++) { uint8_t t = px[i * 4]; px[i * 4] = px[i * 4 + 2]; px[i * 4 + 2] = t; }
        for (uint64_t i = 0; i < (uint64_t)w * h; i++) px[i * 4 + 3] = 255;
        vk_save_png(shot_path, px, w, h);
    }
}

// LARECOMP_SHOT_RAW=<file>: the numbered series (".../frame_NNNNN.png") goes to one raw file instead, no PNG encoding:
// per frame a record of u32 frame number + 480x270 RGB (box-filtered), read by letsgo-recomp's tools/bateria_scan.py. Several times faster
// than PNGs (and ffmpeg) for a 900-frame route.
#define RAW_W 480
#define RAW_H 270
static bool shot_raw(const char *path, const uint8_t *rgba, uint32_t w, uint32_t h) {
    static FILE *f; static int init;
    if (!init) {
        init = 1;
        const char *e = ENV("LARECOMP_SHOT_RAW");
        if (e && *e) { f = fopen(e, "wb"); if (f) setvbuf(f, NULL, _IOFBF, 1 << 20); else LOG("[vk] cannot open %s\n", e); }
    }
    const char *n = strstr(path, "frame_");
    if (!f || !n) return false;
    static uint8_t out[RAW_W * RAW_H * 3];
    for (uint32_t y = 0; y < RAW_H; y++) {
        uint32_t y0 = (uint64_t)y * h / RAW_H, y1 = (uint64_t)(y + 1) * h / RAW_H; if (y1 <= y0) y1 = y0 + 1;
        for (uint32_t x = 0; x < RAW_W; x++) {
            uint32_t x0 = (uint64_t)x * w / RAW_W, x1 = (uint64_t)(x + 1) * w / RAW_W; if (x1 <= x0) x1 = x0 + 1;
            uint32_t s[3] = {0, 0, 0};
            for (uint32_t yy = y0; yy < y1; yy++)
                for (uint32_t xx = x0; xx < x1; xx++) { const uint8_t *p = rgba + ((uint64_t)yy * w + xx) * 4; s[0] += p[0]; s[1] += p[1]; s[2] += p[2]; }
            uint32_t c = (y1 - y0) * (x1 - x0);
            uint8_t *o = out + ((uint64_t)y * RAW_W + x) * 3;
            o[0] = (uint8_t)(s[0] / c); o[1] = (uint8_t)(s[1] / c); o[2] = (uint8_t)(s[2] / c);
        }
    }
    uint32_t fn = (uint32_t)atol(n + 6);
    fwrite(&fn, 4, 1, f); fwrite(out, sizeof out, 1, f); fflush(f);  // (the game leaves with _exit)
    if (ENV("LARECOMP_SHOT_RAW_LAST") && (long)fn >= atol(ENV("LARECOMP_SHOT_RAW_LAST"))) { LOG("[vk] raw frames done at %u\n", fn); fflush(stderr); _exit(0); }
    return true;
}

// The PNG is encoded and written on a thread of its own (a copy of the pixels): encoding 1080p took ~250 ms on the
// render thread - every F12 screenshot (and each of the test routes' shots) was a quarter-second hitch. Written to
// <path>.tmp and renamed, so no one reads a half-written file.
typedef struct { char path[1024]; uint8_t *px; uint32_t w, h; } PngJob;
static _Atomic int png_pending;
static void png_wait(void) {  // (atexit: a screenshot just asked for is written before the process ends)
    for (int i = 0; i < 400 && atomic_load(&png_pending) > 0; i++) SDL_Delay(5);
}
static void *png_thread(void *arg) {
    PngJob *j = arg;
    char tmp[1100];
    snprintf(tmp, sizeof tmp, "%s.tmp", j->path);
    SDL_Surface *s = SDL_CreateSurfaceFrom((int)j->w, (int)j->h, SDL_PIXELFORMAT_ABGR8888, j->px, (int)j->w * 4);
    if (!s || !SDL_SavePNG(s, tmp) || rename(tmp, j->path) != 0) LOG("[vk] saving %s failed: %s\n", j->path, SDL_GetError()), remove(tmp);
    else LOG("[vk] saved %s (%ux%u)\n", j->path, j->w, j->h);
    SDL_DestroySurface(s);
    free(j->px);
    free(j);
    atomic_fetch_sub(&png_pending, 1);
    return NULL;
}

void vk_save_png(const char *path, const uint8_t *rgba, uint32_t w, uint32_t h) {
    if (shot_raw(path, rgba, w, h)) return;
    PngJob *j = malloc(sizeof *j);
    j->px = j ? malloc((size_t)w * h * 4) : NULL;
    if (!j || !j->px) { free(j); LOG("[vk] saving %s failed: out of memory\n", path); return; }
    snprintf(j->path, sizeof j->path, "%s", path);
    memcpy(j->px, rgba, (size_t)w * h * 4);
    j->w = w, j->h = h;
    static bool hooked;
    if (!hooked) hooked = true, atexit(png_wait);
    atomic_fetch_add(&png_pending, 1);
    pthread_t t;
    if (pthread_create(&t, NULL, png_thread, j) == 0) pthread_detach(t);
    else png_thread(j);
}

// ================================================================== game presentation (metal.m backend_present)
// Draws the game's image into the window with the widescreen crop, then the overlay menu.
extern void vk_present_draw(VkCommandBuffer cb, TexBackend *b, float vx, float vy, float vw, float vh, const float pv[8]);
void renderer_submit(void);
void renderer_set_scale(float sx, float sy);
uint64_t renderer_present_count(void);
void renderer_save_texture_tex(TexBackend *b, const char *path);

static uint64_t frames, total_frames, g_frame_counter;
uint64_t renderer_present_count(void) { return total_frames; }
uint64_t vk_frame_counter(void) { return g_frame_counter; }
static float g_shot_crop[4] = {0, 0, 1, 1};
static double last_fps_time;
static NvnTexture *g_last_presented;

const float *vk_shot_crop(void) { return g_shot_crop; }

void backend_present(NvnTexture *t);
void backend_represent(void);
static void represent_job(void *unused) { (void)unused; backend_represent(); }
void backend_represent(void) {
    if (rt_active() && !rt_on_render_thread()) { rt_call(represent_job, NULL); return; }  // (the keyboard dialog: game thread)
    if (g_last_presented) backend_present(g_last_presented);
    else SDL_Delay(16);
}

void backend_present(NvnTexture *t) {
    {   // (LARECOMP_HEAL_KEYS research key F5: every texture re-checks its memory)
        extern volatile int g_heal_tex;
        extern void backend_pool_written(uint64_t addr, uint64_t size);
        if (g_heal_tex) { g_heal_tex = 0; backend_pool_written(0x7000000000ull, 0x1000000000ull); }
    }
    g_last_presented = t;
    TexBackend *b = vk_tex(t, true);
    b->presented = true;
    vk_end_pass();
    static const float black[4] = {0, 0, 0, 1};
    uint64_t t0 = SDL_GetTicksNS();
    // Frame interpolation (60 fps, exec.c vk_interp_render): the in-between image now, this frame's own half a frame
    // later (the period: the game's present rhythm, smoothed).
    TexBackend *hold = NULL;
    extern bool vk_interp_render(TexBackend *b, TexBackend **hold);
    extern bool vk_stereo_render(TexBackend *b);
    // Stereo (plugin.h set_stereo, a VR mod): the plugins take this frame's image as eye 0, the frame is drawn again
    // for eye 1 (exec.c vk_stereo_render) and they take that too; the window shows eye 1
    // (research: LARECOMP_STEREO=1 without a plugin; LARECOMP_STEREO_SHOT=<present>: both eyes of that one saved,
    // build/frames/eye{0,1}_<present>.png; <present>:<every>:<how many>: a series, eye 0 only)
    static int st_env = -1;
    static long st_shot, st_step = 1, st_count = 1;  // (LARECOMP_STEREO_SHOT=<present>[:<every>:<how many>])
    if (st_env < 0) {
        st_env = getenv("LARECOMP_STEREO") && atoi(getenv("LARECOMP_STEREO")) != 0;
        const char *sh = getenv("LARECOMP_STEREO_SHOT");
        if (sh) sscanf(sh, "%ld:%ld:%ld", &st_shot, &st_step, &st_count);
        if (st_step < 1) st_step = 1;
        if (st_env) g_plugin_stereo = 1;
    }
    extern bool vk_interp_enabled(void);
    bool stereo = g_plugin_stereo && !vk_interp_enabled();  // (frame interpolation - Let's Go's 60 fps menus - shares the recorder)
    if (stereo) {
        extern void renderer_save_texture_tex(TexBackend *b, const char *path);
        char sp[96];
        long fr = (long)total_frames + 1;
        bool shot = st_shot && fr >= st_shot && (fr - st_shot) % st_step == 0 && (fr - st_shot) / st_step < st_count;
        if (shot) mkdir("build/frames", 0755), snprintf(sp, sizeof sp, "build/frames/eye0_%ld.png", fr), renderer_save_texture_tex(b, sp);
        plugins_frame_eye(vk_cmd(), b->image, b->format, b->width, b->height, 0);
        extern void vk_stereo_present_size(uint32_t w, uint32_t h);
        vk_stereo_present_size(t->width, t->height);
        if (vk_stereo_render(b)) {
            plugins_frame_eye(vk_cmd(), b->image, b->format, b->width, b->height, 1);
            if (shot && st_count == 1) snprintf(sp, sizeof sp, "build/frames/eye1_%ld.png", fr), renderer_save_texture_tex(b, sp);
        }
    }
    bool two = !stereo && vk_interp_render(b, &hold);
    static uint64_t last_present, period = 33333333;
    if (last_present && t0 - last_present > 5000000 && t0 - last_present < 100000000) period = (period * 7 + (t0 - last_present)) / 8;
    last_present = t0;
    {   // 60 fps mode: a menu's frames come every 33.3 ms, but the smoothed period still holds the world's 16.7 ms for its
        // first frames - the second image would come too early
        extern bool fps60_menu(void);
        if (fps60_menu()) period = 33333333;
    }
    for (int image = two ? 0 : 1; image < 2; image++) {
    bool final = image == 1;
    if (final && two) {
        uint64_t due = t0 + period / 2, now = SDL_GetTicksNS();
        if (due > now) SDL_DelayPrecise(due - now);
        b = hold;
    }
    if (final && plugins_count() && !stereo) plugins_frame(vk_cmd(), b->image, b->format, b->width, b->height);
    bool have = vk_frame_begin(black);
    uint64_t nd = SDL_GetTicksNS() - t0;
    if (nd > 30000000) {
        static int warned;
        if (warned++ < 20) LOG("[vk] acquire blocked %llums\n", (unsigned long long)(nd / 1000000));
    }
    if (have) {
        double dw = g_vk.sc_extent.width, dh = g_vk.sc_extent.height;
        // render scale + placement in the window (the game's 16:9 image, fitted): runtime/nvn/output_scale.c;
        // with the GPU time per frame (exec.c, submission timestamps): dynamic resolution raises it again by that
        OutScale os;
        output_scale(dw, dh, false, SDL_GetTicksNS() / 1e9, vk_gpu_frame_update(), &os);
        double sx = os.sx, sy = os.sy, vx = os.vx, vy = os.vy, vw = os.vw, vh = os.vh;
        float crop[4] = {0, 0, 1, 1};  // uv origin and size: the whole image
        {   // where the game image is in the window (pixels): the mouse as a finger on the touch screen (hle/input.c)
            extern double g_present_rect[4];
            g_present_rect[0] = vx, g_present_rect[1] = vy, g_present_rect[2] = vw, g_present_rect[3] = vh;
        }
        renderer_set_scale((float)sx, (float)sy);
        VkCommandBuffer cb = vk_cmd();
        float pv[8] = {renderer_origin_lower_left() ? 1.0f : 0.0f, 0, 0, 0, crop[0], crop[1], crop[2], crop[3]};
        memcpy(g_shot_crop, crop, sizeof crop);
        // The game's first two presents are a blue full-screen 2D quad drawn while it loads (then black, then the
        // controller screen): a flash on every start. Keep the window black for them. LARECOMP_HIDE_FIRST_FRAMES=<n>
        // (0: show everything).
        { extern void bntxscan_present(uint64_t frame); bntxscan_present(total_frames); }  // (research: hle/bntxscan.c)
        static long hide = -1;
        if (hide < 0) hide = ENV("LARECOMP_HIDE_FIRST_FRAMES") ? atol(ENV("LARECOMP_HIDE_FIRST_FRAMES")) : 2;
        if ((long)total_frames >= hide) vk_present_draw(cb, b, (float)vx, (float)vy, (float)vw, (float)vh, pv);
        static long menu_at = -1;
        if (menu_at < 0) menu_at = ENV("LARECOMP_MENU_AT") ? atol(ENV("LARECOMP_MENU_AT")) : 0;
        if (final && menu_at && (long)total_frames + 1 == menu_at) { extern void menu_test_open(void); menu_test_open(); }
        menu_render((void *)cb, g_vk.sc_extent.width, g_vk.sc_extent.height);
        { extern uint32_t g_dyn_epoch; g_dyn_epoch++; }  // (the menu set its own state: exec.c draw_'s cache)
        static long win_shot = -1;
        if (win_shot < 0) win_shot = ENV("LARECOMP_WINDOW_SHOT") ? atol(ENV("LARECOMP_WINDOW_SHOT")) : 0;
        // (the in-between image: build/window_mid.png, with interpolation on)
        bool shot = win_shot && (long)total_frames + 1 == win_shot;
        // LARECOMP_WINDOW_SHOTS=<n>,<n>,...: the in-between image and the frame of each, build/frames/{mid,fin}_<n>.png
        static long list[512]; static int nlist = -1;
        if (nlist < 0) {
            nlist = 0;
            const char *e = ENV("LARECOMP_WINDOW_SHOTS");
            for (; e && *e && nlist < 512; nlist++) { list[nlist] = atol(e); e = strchr(e, ','); if (!e) { nlist++; break; } e++; }
        }
        static char lpath[96];
        extern long fps60_scene_start(void);
        static bool rel_list = false;
        { static bool init; if (!init) { init = true; rel_list = ENV("LARECOMP_WINDOW_SHOTS_REL") != NULL; } }
        const char *path = shot ? (final ? "build/window.png" : "build/window_mid.png") : NULL;
        for (int i = 0; i < nlist; i++)
            if (rel_list ? (fps60_scene_start() >= 0 && (long)total_frames + 1 - fps60_scene_start() == list[i])
                         : (long)total_frames + 1 == list[i]) {
                mkdir("build/frames", 0755);
                snprintf(lpath, sizeof lpath, "build/frames/%s_%ld.png", final ? "fin" : "mid", list[i]);
                path = lpath;
            }
        vk_frame_end(path);
        if (final && shot && ENV("LARECOMP_EXIT_AFTER_SHOT")) { fflush(stderr); _exit(0); }
    } else {
        renderer_submit();
    }
    if (final && plugins_count()) plugins_frame_submitted();
    }
    extern void vk_prof_frame(uint64_t frame, uint64_t present_ns);
    vk_prof_frame(total_frames, SDL_GetTicksNS() - t0);
    frames++;
    total_frames++;
    g_frame_counter++;
    {   // Opt-in game-present intervals (not the extra menu image shown by the 60 fps mode).
        static FILE *csv;
        static uint64_t previous;
        static bool initialized;
        if (!initialized) {
            initialized = true;
            const char *path = ENV("LARECOMP_FRAMETIME_CSV");
            if (path && *path) {
                csv = fopen(path, "w");
                if (csv) fputs("present,dt_ms\n", csv);
                else LOG("[vk] cannot open frametime CSV: %s\n", path);
            }
        }
        if (csv) {
            uint64_t now = SDL_GetTicksNS();
            if (previous) fprintf(csv, "%llu,%.6f\n", (unsigned long long)total_frames, (now - previous) / 1e6);
            previous = now;
            if (total_frames % 60 == 0) fflush(csv);
            static long exit_at = -1;
            if (exit_at < 0) exit_at = ENV("LARECOMP_EXIT_AT_PRESENT") ? atol(ENV("LARECOMP_EXIT_AT_PRESENT")) : 0;
            if (exit_at && (long)total_frames >= exit_at) { LOG("[vk] exit at present %llu\n", (unsigned long long)total_frames); fflush(csv); fflush(stderr); _exit(0); }
        }
    }
    {   // LARECOMP_RENDERDOC_AT=<frame>: under RenderDoc (renderdoccmd capture), capture the frame after that one
        extern void renderdoc_frame(uint64_t frame);
        renderdoc_frame(total_frames);
    }
    // (with the render thread the game thread runs these three when it queues the present: rt_present)
    extern void input_frame(uint64_t frame);
    if (!rt_on_render_thread()) input_frame(total_frames);
    static long slow_ms = -1;
    if (slow_ms < 0) slow_ms = ENV("LARECOMP_SLOW_FRAME_MS") ? atol(ENV("LARECOMP_SLOW_FRAME_MS")) : 0;
    if (slow_ms > 0 && total_frames > 2600) SDL_Delay((Uint32)slow_ms);
    extern void fnprof_frame(uint64_t frame);
    if (!rt_on_render_thread()) fnprof_frame(total_frames);
    extern void sampler_frame(uint64_t frame);  // (runtime/hle/sampler.c: LARECOMP_SAMPLE)
    sampler_frame(total_frames);
    extern void reboot_test_frame(uint64_t frame);  // runtime/main.c (LARECOMP_REBOOT_AT)
    if (!rt_on_render_thread()) reboot_test_frame(total_frames);
    static long shot_every = -1;
    if (shot_every < 0) { const char *e = ENV("LARECOMP_SCREENSHOT_EVERY"); shot_every = e ? atol(e) : 0; }
    extern bool g_debug_frame;
    static long debug_at = -1;
    if (debug_at < 0) { const char *e = ENV("LARECOMP_DEBUG_FRAME"); debug_at = e ? atol(e) : 0; }
    extern volatile int g_debug_frame_request;
    if (g_debug_frame_request) { g_debug_frame_request = 0; debug_at = (long)total_frames + 1; }
    // LARECOMP_DEBUG_FRAMES=<n> (with LARECOMP_DEBUG_FRAME / F9): dump n consecutive frames (build/pass_<frame>_NNN.png)
    static long debug_n = -1;
    if (debug_n < 0) { const char *e = ENV("LARECOMP_DEBUG_FRAMES"); debug_n = e && atol(e) > 1 ? atol(e) : 1; }
    {   // LARECOMP_LIVE=<dir>: <dir>/debug appears ("<n>", default 1) -> the next n frames dumped as LARECOMP_DEBUG_FRAME does
        static const char *ldir = (const char *)1;
        if (ldir == (const char *)1) ldir = ENV("LARECOMP_LIVE");
        char dpath[1100];
        if (ldir && (snprintf(dpath, sizeof dpath, "%s/debug", ldir), access(dpath, F_OK) == 0)) {
            FILE *f = fopen(dpath, "r");
            long n = 1;
            if (f) { if (fscanf(f, "%ld", &n) != 1) n = 1; fclose(f); }
            remove(dpath);
            debug_n = n > 0 ? n : 1, debug_at = (long)total_frames + 1;
            LOG("[debug] live request: frames %ld..%ld dumped\n", debug_at, debug_at + debug_n - 1);
        }
    }
    g_debug_frame = debug_at && total_frames >= (uint64_t)debug_at && total_frames < (uint64_t)debug_at + (uint64_t)debug_n;
    {   // LARECOMP_GPUPROF_AT=<frame>[,<frame>...]: GPU time per pass of those frames ("[gpuprof]" in the log)
        extern bool g_gpuprof_frame;
        extern void vk_gpuprof_report(void);
        if (g_gpuprof_frame) { g_gpuprof_frame = false; vk_gpuprof_report(); }
        const char *e = ENV("LARECOMP_GPUPROF_AT");
        for (const char *p = e; p && *p;) {
            long at = strtol(p, (char **)&p, 10);
            if (at == (long)total_frames) { g_gpuprof_frame = true; break; }
            if (*p == ',') p++; else break;
        }
    }
    {   // LARECOMP_WATCH_RANGE=<lo>,<hi>@<frame> (hex guest addresses): from that frame on, the functions accessing [lo, hi)
        // ("[watch]" in the log, dispatch.c watch_arm: one page, re-protected every 100 us)
        static long at = -1;
        static uint64_t lo, hi;
        if (at < 0) {
            const char *e = ENV("LARECOMP_WATCH_RANGE");
            at = 0;
            if (e) sscanf(e, "%llx,%llx@%ld", (unsigned long long *)&lo, (unsigned long long *)&hi, &at);
        }
        if (at && (long)total_frames == at) {
            extern uint64_t g_watch_lo, g_watch_hi;
            extern void watch_arm(void);
            g_watch_lo = lo, g_watch_hi = hi;
            watch_arm();
        }
    }
    {   // LARECOMP_HWWATCH=<addr>[,<addr>...]@<frame> (hex, up to 4): hardware write watchpoints, 8 bytes each (hle/hwwatch.c)
        static long at = -1;
        static uint64_t wa[4];
        static int nw;
        if (at < 0) {
            const char *e = ENV("LARECOMP_HWWATCH");
            at = 0;
            for (const char *p = e; p && *p && nw < 4;) {
                wa[nw++] = strtoull(p, (char **)&p, 16);
                if (*p == ',') p++;
                else { if (*p == '@') at = atol(p + 1); break; }
            }
        }
        extern void hwwatch_arm(uint64_t addr, uint64_t len);
        if (at && (long)total_frames == at)
            for (int i = 0; i < nw; i++) hwwatch_arm(wa[i] & ~7ull, 8);
        extern void hwwatch_src_arm(void);  // (LARECOMP_HWWATCH_SRC, from the same frame)
        if (at && (long)total_frames == at) hwwatch_src_arm();
    }
    { extern void fdiff_present(uint64_t frame); fdiff_present(total_frames); }  // (LARECOMP_FDIFF research: hle/fdiff.c)
    {   // LARECOMP_CAMSCAN=<cbuf>@<frame>: exec.c camscan
        static long at = -1;
        static uint64_t cb;
        if (at < 0) {
            const char *e = ENV("LARECOMP_CAMSCAN");
            at = 0;
            if (e) sscanf(e, "%llx@%ld", (unsigned long long *)&cb, &at);
        }
        extern void camscan(uint64_t cbuf);
        if (at && (long)total_frames == at) camscan(cb);
    }
    {   // LARECOMP_FIND_VIEW_AT=<frame>[,<frame>...]: camera research, the constant buffers holding view-like matrices
        extern uint64_t g_findview_frame;  // exec.c ("[findview]" in the log)
        g_findview_frame = 0;
        for (const char *p = ENV("LARECOMP_FIND_VIEW_AT"); p && *p;) {
            long at = strtol(p, (char **)&p, 10);
            if (at == (long)total_frames) { g_findview_frame = total_frames; break; }
            if (*p == ',') p++; else break;
        }
    }
    if (g_debug_frame) LOG("===== debug frame %llu =====\n", (unsigned long long)total_frames);
    {   // LARECOMP_SHOT_AT=<frame>,<frame>,... (ascending): the game image at those frames -> build/frames/frame_N.png
        static const char *next;
        static int init;
        if (!init) { init = 1; next = ENV("LARECOMP_SHOT_AT"); if (next) mkdir("build/frames", 0755); }
        while (next && *next) {
            long at = strtol(next, (char **)&next, 10);
            if ((long)total_frames < at) { while (next > ENV("LARECOMP_SHOT_AT") && next[-1] != ',') next--; break; }
            if ((long)total_frames == at) {
                char path[256];
                snprintf(path, sizeof path, "build/frames/frame_%05llu.png", (unsigned long long)total_frames);
                renderer_save_texture_tex(b, path);
            }
            if (*next == ',') next++;
        }
    }
    {   // LARECOMP_LIVE=<dir> (testing, hle/input.c): <dir>/shot appears ("<n> <every>", default 1 1) -> the next n game
        // images <every> presents apart as <dir>/live_<frame>.png; the request file is removed once taken
        static const char *dir = (const char *)1;
        static long left, every = 1, next_at;
        if (dir == (const char *)1) dir = ENV("LARECOMP_LIVE");
        if (dir) {
            char path[1100];
            snprintf(path, sizeof path, "%s/shot", dir);
            if (!left && access(path, F_OK) == 0) {
                FILE *f = fopen(path, "r");
                left = 1, every = 1;
                if (f) { if (fscanf(f, "%ld %ld", &left, &every) < 1) left = 1; fclose(f); }
                if (left < 1) left = 1;
                if (every < 1) every = 1;
                next_at = (long)total_frames;
                remove(path);
            }
            for (int k = 0; k < 2; k++) {  // (testing: Settings > Pokémon Files' Export / Import without the menu, hle/pkm_link.c)
                snprintf(path, sizeof path, "%s/%s", dir, k ? "pkimport" : "pkexport");
                if (access(path, F_OK) == 0) { remove(path); extern void pkm_files_run(int v); pkm_files_run(k); }
            }
            snprintf(path, sizeof path, "%s/pscan", dir);  // (research: hle/pscan.c)
            if (access(path, F_OK) == 0) {
                FILE *f = fopen(path, "r");
                long per = 60;
                if (f) { if (fscanf(f, "%ld", &per) != 1) per = 60; fclose(f); }
                remove(path);
                extern void pscan_start(long period);
                pscan_start(per);
            }
            snprintf(path, sizeof path, "%s/find", dir);  // (research: "<text>" -> <dir>/find.out, where game memory has it)
            if (access(path, F_OK) == 0) {
                FILE *f = fopen(path, "r");
                char want[256] = "";
                if (f) { if (!fgets(want, sizeof want, f)) want[0] = 0; fclose(f); }
                remove(path);
                want[strcspn(want, "\r\n")] = 0;
                if (!strncmp(want, "hex:", 4)) {  // ("hex:<bytes>": those bytes)
                    size_t m = 0;
                    for (const char *h = want + 4; h[0] && h[1] && m < sizeof want - 1; h += 2) {
                        unsigned v;
                        if (sscanf(h, "%2x", &v) != 1) break;
                        want[m++] = (char)v;
                    }
                    want[m] = 0, want[255] = (char)m;  // (its length: it may hold zeros)
                }
                extern int kernel_physmem_ranges(uint64_t *lo, uint64_t *hi, int max);
                extern int kernel_data_regions(uint64_t *addr, uint64_t *size, int max);
                extern uint64_t kernel_heap_size(void), g_stdheap_used;
                static uint64_t lo[1100], hi[1100];
                int n = kernel_physmem_ranges(lo + 1, hi + 1, 519) + 1;
                uint64_t top = g_stdheap_used ? STDHEAP_BASE + g_stdheap_used : 0, heap = HEAP_BASE + kernel_heap_size();
                lo[0] = HEAP_BASE, hi[0] = top > heap ? top : heap;
                {   // (and the game's own writable data below the heap: module .data / .bss, where its globals are)
                    uint64_t ra[512], rs[512];
                    int m = kernel_data_regions(ra, rs, 512);
                    for (int i = 0; i < m && n < 1100; i++)
                        if (ra[i] < HEAP_BASE) lo[n] = ra[i], hi[n] = ra[i] + rs[i], n++;
                }
                snprintf(path, sizeof path, "%s/find.tmp", dir);  // (renamed when complete)
                FILE *o = fopen(path, "w");
                size_t k = want[255] ? (size_t)(uint8_t)want[255] : strlen(want);
                int found = 0;
                bool aligned = want[255] == 8;  // (an 8-byte value, a pointer: only 8-aligned places, and quicker)
                for (int r = 0; o && k && r < n && found < 256; r++)
                    for (const uint8_t *p = (const uint8_t *)(uintptr_t)lo[r], *e = (const uint8_t *)(uintptr_t)hi[r]; p + k <= e && found < 256; p += aligned ? 8 : 1)
                        if (aligned ? *(const uint64_t *)p == *(const uint64_t *)want : *p == (uint8_t)want[0] && !memcmp(p, want, k))
                            fprintf(o, "%llx\n", (unsigned long long)(uintptr_t)p), found++;
                if (o) {
                    fclose(o);
                    char done[512];
                    snprintf(done, sizeof done, "%s/find.out", dir);
                    rename(path, done);
                }
            }
            snprintf(path, sizeof path, "%s/vscan", dir);  // (research: "<u32>" - the 4-byte-aligned words equal to it; the
            if (access(path, F_OK) == 0) {               // next "<u32>" keeps those of them equal to that; "reset" starts
                FILE *f = fopen(path, "r");              // over. <dir>/vscan.out: how many, and up to 400 of them)
                char line[64] = "";
                if (f) { if (!fgets(line, sizeof line, f)) line[0] = 0; fclose(f); }
                remove(path);
                static uint64_t *cand;
                static size_t ncand, capcand;
                static bool have;
                if (!strncmp(line, "reset", 5)) have = false, ncand = 0;
                else {
                    uint32_t v = (uint32_t)strtoul(line, NULL, 0);
                    if (!have) {
                        extern int kernel_physmem_ranges(uint64_t *lo, uint64_t *hi, int max);
                        extern uint64_t kernel_heap_size(void), g_stdheap_used;
                        static uint64_t lo[520], hi[520];
                        int n = kernel_physmem_ranges(lo + 1, hi + 1, 519) + 1;
                        uint64_t top = g_stdheap_used ? STDHEAP_BASE + g_stdheap_used : 0, heap = HEAP_BASE + kernel_heap_size();
                        lo[0] = HEAP_BASE, hi[0] = top > heap ? top : heap;
                        ncand = 0;
                        for (int r = 0; r < n; r++)
                            for (const uint32_t *q = (const uint32_t *)(uintptr_t)lo[r], *e = (const uint32_t *)(uintptr_t)hi[r]; q < e; q++)
                                if (*q == v) {
                                    if (ncand == capcand) capcand = capcand ? capcand * 2 : 1 << 20, cand = realloc(cand, capcand * 8);
                                    cand[ncand++] = (uint64_t)(uintptr_t)q;
                                }
                        have = true;
                    } else {
                        size_t k = 0;
                        for (size_t i = 0; i < ncand; i++) if (*(const uint32_t *)(uintptr_t)cand[i] == v) cand[k++] = cand[i];
                        ncand = k;
                    }
                }
                snprintf(path, sizeof path, "%s/vscan.tmp", dir);
                FILE *o = fopen(path, "w");
                if (o) {
                    fprintf(o, "%zu\n", ncand);
                    for (size_t i = 0; i < ncand && i < 400; i++) fprintf(o, "%llx\n", (unsigned long long)cand[i]);
                    fclose(o);
                    char done[512];
                    snprintf(done, sizeof done, "%s/vscan.out", dir);
                    rename(path, done);
                }
            }
            snprintf(path, sizeof path, "%s/fscan", dir);  // (research: positions. "<x> <y> <z> <r>": the 4-byte-aligned float
            if (access(path, F_OK) == 0) {                // triplets within r of that point; then "moved" keeps those that
                FILE *f = fopen(path, "r");               // changed since the last fscan, "still" those that didn't.
                char line[128] = "";                      // <dir>/fscan.out: how many, then up to 400 "addr x y z")
                if (f) { if (!fgets(line, sizeof line, f)) line[0] = 0; fclose(f); }
                remove(path);
                static uint64_t *cand;
                static float (*was)[3];
                static size_t ncand, capcand;
                float x, y, z, r;
                bool moved = !strncmp(line, "moved", 5), still = !strncmp(line, "still", 5);
                if (!moved && !still && sscanf(line, "%f %f %f %f", &x, &y, &z, &r) == 4) {
                    extern int kernel_physmem_ranges(uint64_t *lo, uint64_t *hi, int max);
                    extern uint64_t kernel_heap_size(void), g_stdheap_used;
                    static uint64_t lo[520], hi[520];
                    int n = kernel_physmem_ranges(lo + 1, hi + 1, 519) + 1;
                    uint64_t top = g_stdheap_used ? STDHEAP_BASE + g_stdheap_used : 0, heap = HEAP_BASE + kernel_heap_size();
                    lo[0] = HEAP_BASE, hi[0] = top > heap ? top : heap;
                    ncand = 0;
                    for (int k = 0; k < n; k++)
                        for (const float *q = (const float *)(uintptr_t)lo[k], *e = (const float *)(uintptr_t)hi[k] - 3; q < e; q++) {
                            float dx = q[0] - x, dy = q[1] - y, dz = q[2] - z;
                            if (!(dx * dx + dy * dy + dz * dz <= r * r)) continue;  // (NaNs fail too)
                            if (q[0] == 0 && q[1] == 0 && q[2] == 0) continue;
                            if (ncand == capcand) capcand = capcand ? capcand * 2 : 1 << 16, cand = realloc(cand, capcand * 8), was = realloc(was, capcand * 12);
                            cand[ncand] = (uint64_t)(uintptr_t)q, memcpy(was[ncand], q, 12), ncand++;
                        }
                } else if (moved || still) {
                    size_t k = 0;
                    for (size_t i = 0; i < ncand; i++) {
                        const float *q = (const float *)(uintptr_t)cand[i];
                        float d = fabsf(q[0] - was[i][0]) + fabsf(q[1] - was[i][1]) + fabsf(q[2] - was[i][2]);
                        if (moved ? d > 0.3f && d < 100 : d == 0) cand[k] = cand[i], memcpy(was[k], q, 12), k++;
                    }
                    ncand = k;
                }
                snprintf(path, sizeof path, "%s/fscan.tmp", dir);
                FILE *o = fopen(path, "w");
                if (o) {
                    fprintf(o, "%zu\n", ncand);
                    for (size_t i = 0; i < ncand && i < 400; i++) fprintf(o, "%llx %.3f %.3f %.3f\n", (unsigned long long)cand[i], was[i][0], was[i][1], was[i][2]);
                    fclose(o);
                    char done[512];
                    snprintf(done, sizeof done, "%s/fscan.out", dir);
                    rename(path, done);
                }
            }
            {   // (research: "<addr> <x> <y> <z> [presents]" in <dir>/poke - those floats written there each present)
                static uint64_t pa;
                static float pv[3];
                static long pleft;
                snprintf(path, sizeof path, "%s/poke", dir);
                if (access(path, F_OK) == 0) {
                    FILE *f = fopen(path, "r");
                    unsigned long long a = 0;
                    pleft = 1;
                    if (f) { if (fscanf(f, "%llx %f %f %f %ld", &a, &pv[0], &pv[1], &pv[2], &pleft) < 4) a = 0; fclose(f); }
                    remove(path);
                    pa = a >= HEAP_BASE && a < HEAP_BASE + HEAP_MAX ? a : 0;
                }
                if (pa && pleft-- > 0) memcpy((void *)(uintptr_t)pa, pv, 12);
            }
            snprintf(path, sizeof path, "%s/dump", dir);  // (research: "<addr> <len>" -> <dir>/dump.bin)
            if (access(path, F_OK) == 0) {
                FILE *f = fopen(path, "r");
                unsigned long long a = 0, n = 0;
                if (f) { if (fscanf(f, "%llx %llx", &a, &n) != 2) a = 0; fclose(f); }
                remove(path);
                snprintf(path, sizeof path, "%s/dump.tmp", dir);  // (renamed when complete)
                FILE *o = a && n && n < (64u << 20) ? fopen(path, "wb") : NULL;
                if (o) {
                    fwrite((const void *)(uintptr_t)a, 1, n, o), fclose(o);
                    char done[512];
                    snprintf(done, sizeof done, "%s/dump.bin", dir);
                    rename(path, done);
                }
            }
            snprintf(path, sizeof path, "%s/watch", dir);  // (research: hardware watchpoints, hle/hwwatch.c)
            if (access(path, F_OK) == 0) {
                FILE *f = fopen(path, "r");
                char line[512] = "";
                if (f) { if (!fgets(line, sizeof line, f)) line[0] = 0; fclose(f); }
                remove(path);
                extern void hwwatch_arm(uint64_t addr, uint64_t len);
                extern void hwwatch_reset(void);
                hwwatch_reset();
                for (char *p = line; *p;) {
                    uint64_t a = strtoull(p, &p, 16);
                    if (a) hwwatch_arm(a & ~7ull, 8);
                    if (*p != ',') break;
                    p++;
                }
            }
            snprintf(path, sizeof path, "%s/bt", dir);  // (research: call chains, hle/pscan.c)
            if (access(path, F_OK) == 0) {
                FILE *f = fopen(path, "r");
                unsigned long long fn = 0, x0 = 0;
                int n = 12;
                if (f) { if (fscanf(f, "%llx %d %llx", &fn, &n, &x0) < 1) fn = 0; fclose(f); }
                remove(path);
                extern void bt_start(uint64_t fn, int n, uint64_t x0);
                if (fn) bt_start(fn, n, x0);
            }
            snprintf(path, sizeof path, "%s/peek", dir);  // (research: "<addr> <len>" -> <dir>/peek.out, hex, 16 bytes a line)
            if (access(path, F_OK) == 0) {
                FILE *f = fopen(path, "r");
                unsigned long long a = 0, n = 64;
                if (f) { if (fscanf(f, "%llx %llx", &a, &n) < 1) a = 0; fclose(f); }
                remove(path);
                char out[1100];
                snprintf(out, sizeof out, "%s/peek.out", dir);
                FILE *o = fopen(out, "w");
                if (o && a >= 0x7000000000ull && a + n < 0x8000000000ull && n <= 0x10000) {
                    for (unsigned long long i = 0; i < n; i += 16) {
                        fprintf(o, "%llx:", a + i);
                        for (int k = 0; k < 16; k += 8) fprintf(o, " %016llx", (unsigned long long)*(const uint64_t *)(uintptr_t)(a + i + k));
                        fprintf(o, "\n");
                    }
                }
                if (o) fclose(o);
            }
            snprintf(path, sizeof path, "%s/debugframe", dir);  // (research: the next frame's draws logged, LARECOMP_DEBUG_FRAME)
            if (access(path, F_OK) == 0) {
                remove(path);
                extern volatile int g_debug_frame_request;
                g_debug_frame_request = 1;
            }
            snprintf(path, sizeof path, "%s/nocullbits", dir);  // (research: exec.c LARECOMP_EXP_NOCULL_BITS from the frame after next)
            if (access(path, F_OK) == 0) {
                remove(path);
                extern volatile uint64_t g_exp_nocull_start;
                extern uint64_t vk_frame_counter(void);
                g_exp_nocull_start = vk_frame_counter() + 2;
                LOG("[exp] nocull bits from frame %llu (vk frame)\n", (unsigned long long)g_exp_nocull_start);
            }
            snprintf(path, sizeof path, "%s/trace", dir);  // (research: renderthread.c rt_trace, ~300 presents)
            {
                static long trace_end = -1, trace_at = -2;  // (or LARECOMP_TRACE_AT=<present>)
                if (trace_at == -2) trace_at = ENV("LARECOMP_TRACE_AT") ? atol(ENV("LARECOMP_TRACE_AT")) : -1;
                bool at = trace_at >= 0 && (long)total_frames >= trace_at;
                if (at) trace_at = -1;
                if (at || access(path, F_OK) == 0) {
                    if (!at) remove(path);
                    extern void rt_trace_start(void);
                    rt_trace_start();
                    trace_end = (long)total_frames + 300;
                }
                if (trace_end >= 0 && (long)total_frames >= trace_end) {
                    trace_end = -1;
                    extern void rt_trace_stop(void);
                    rt_trace_stop();
                }
            }
            snprintf(path, sizeof path, "%s/sample", dir);  // (profiling: hle/sampler.c for "<frames>" presents)
            if (access(path, F_OK) == 0) {
                FILE *f = fopen(path, "r");
                long n = 600;
                if (f) { if (fscanf(f, "%ld", &n) != 1) n = 600; fclose(f); }
                remove(path);
                extern void sampler_live(long frames, uint64_t now);
                sampler_live(n, total_frames);
            }
            snprintf(path, sizeof path, "%s/skipcs", dir);  // (research: exec.c dispatch_compute)
            if (access(path, F_OK) == 0) {
                FILE *f = fopen(path, "r");
                char line[300] = "";
                if (f) { if (!fgets(line, sizeof line, f)) line[0] = 0; fclose(f); }
                remove(path);
                extern volatile uint64_t g_skip_cs[8];
                int k = 0;
                for (char *p = line; *p && k < 8;) {
                    uint64_t a = strtoull(p, &p, 16);
                    if (a) g_skip_cs[k++] = a;
                    if (*p != ',') break;
                    p++;
                }
                while (k < 8) g_skip_cs[k++] = 0;
                LOG("[exp] skipping compute: %s", line);
            }
            snprintf(path, sizeof path, "%s/nocull", dir);  // (research: exec.c, no culling at all on / off)
            if (access(path, F_OK) == 0) {
                FILE *f = fopen(path, "r");
                int v = 1;
                if (f) { if (fscanf(f, "%d", &v) != 1) v = 1; fclose(f); }
                remove(path);
                extern volatile int g_live_nocull;
                g_live_nocull = v;
                LOG("[exp] culling %s\n", v ? "off" : "on");
            }
            snprintf(path, sizeof path, "%s/nocullone", dir);  // (research: exec.c; "0 0" ends it)
            if (access(path, F_OK) == 0) {
                FILE *f = fopen(path, "r");
                long lo = 0, hi = 0;
                if (f) { if (fscanf(f, "%ld %ld", &lo, &hi) < 1) lo = 0; fclose(f); }
                remove(path);
                if (hi < lo) hi = lo;
                extern volatile long g_nocull_lo, g_nocull_hi;
                g_nocull_lo = lo, g_nocull_hi = hi;
                LOG("[exp] no culling for draws %ld-%ld\n", lo, hi);
            }
            snprintf(path, sizeof path, "%s/drawprobe", dir);  // (research: exec.c [probe])
            if (access(path, F_OK) == 0) {
                FILE *f = fopen(path, "r");
                long lo = 0, hi = 0;
                if (f) { if (fscanf(f, "%ld %ld", &lo, &hi) < 1) lo = 0; fclose(f); }
                remove(path);
                if (hi < lo) hi = lo;
                extern volatile long g_probe_lo, g_probe_hi;
                extern volatile uint64_t g_probe_frame;
                extern uint64_t vk_frame_counter(void);
                g_probe_lo = lo, g_probe_hi = hi, g_probe_frame = vk_frame_counter() + 2;
            }
            snprintf(path, sizeof path, "%s/f12", dir);  // (tests: as if F12 was pressed - the screenshot below)
            if (access(path, F_OK) == 0) { remove(path); extern volatile int g_screenshot_request; g_screenshot_request = 1; }
            snprintf(path, sizeof path, "%s/npcfull", dir);  // (research: hle/za_fixes.c, the NPC animation fix on / off)
            if (access(path, F_OK) == 0) {
                FILE *f = fopen(path, "r");
                int v = 1;
                if (f) { if (fscanf(f, "%d", &v) != 1) v = 1; fclose(f); }
                remove(path);
                extern volatile int g_za_gate_on;
                g_za_gate_on = v;
                LOG("[za] NPC animation fix %s\n", v ? "on" : "off");
            }
            { extern void pscan_present(uint64_t frame); pscan_present(total_frames); }
            if (left && (long)total_frames >= next_at) {
                snprintf(path, sizeof path, "%s/live_%06llu.png", dir, (unsigned long long)total_frames);
                renderer_save_texture_tex(b, path);
                left--, next_at = (long)total_frames + every;
            }
        }
    }
    {   // LARECOMP_SHOT_REL=<n>,<n>,...: the game image n presents after a capture began (fps60.c marks it)
        extern long fps60_scene_start(void);
        static const char *rel = (const char *)1;
        if (rel == (const char *)1) { rel = ENV("LARECOMP_SHOT_REL"); if (rel) mkdir("build/frames", 0755); }
        long st = rel ? fps60_scene_start() : -1;
        for (const char *p = rel; st >= 0 && p && *p;) {
            long n = strtol(p, (char **)&p, 10);
            if ((long)total_frames == st + n) {
                char path[256];
                snprintf(path, sizeof path, "build/frames/rel_%04ld.png", n);
                renderer_save_texture_tex(b, path);
            }
            if (*p == ',') p++;
        }
    }
    {   // F12 / a controller's capture button (hle/input.c): the game image, without the overlay, as a PNG
        extern volatile int g_screenshot_request;
        if (g_screenshot_request) {
            g_screenshot_request = 0;
            char dir[1100], path[1200];
            snprintf(dir, sizeof dir, "%s/screenshots", rt_data_root());
            mkdir(dir, 0755);
            SDL_Time now = 0;
            SDL_DateTime dt = {0};
            SDL_GetCurrentTime(&now);
            SDL_TimeToDateTime(now, &dt, true);
            snprintf(path, sizeof path, "%s/%s_%04d-%02d-%02d_%02d-%02d-%02d.png", dir, RT_SHOT_PREFIX, dt.year, dt.month, dt.day,
                     dt.hour, dt.minute, dt.second);
            for (int k = 2; access(path, F_OK) == 0 && k < 100; k++)  // (two in one second)
                snprintf(path + strlen(path) - 4, 16, "_%d.png", k);
            renderer_save_texture_tex(b, path);
            LOG("[shot] %s\n", path);
            extern void menu_notice(const char *text);
            menu_notice("Screenshot saved (screenshots folder)");
        }
    }
    static long shot_from = -1;
    if (shot_from < 0) { const char *e = ENV("LARECOMP_SCREENSHOT_FROM"); shot_from = e ? atol(e) : 0; }
    if (shot_every && total_frames % shot_every == 0 && (long)total_frames >= shot_from) {
        char path[256];
        if (shot_from) mkdir("build/frames", 0755);
        snprintf(path, sizeof path, shot_from ? "build/frames/frame_%05llu.png" : "build/frame_%05llu.png",
                 (unsigned long long)total_frames);
        renderer_save_texture_tex(b, path);
        // a series (SCREENSHOT_FROM) runs on until LARECOMP_WINDOW_SHOT exits
        if (ENV("LARECOMP_EXIT_AFTER_SHOT") && !shot_from) { fflush(stderr); _exit(0); }
    }
    double now = SDL_GetTicks() / 1000.0;
    static double fps_iv = -1, prev_present, max_gap;
    if (fps_iv < 0) fps_iv = ENV("LARECOMP_FPS_INTERVAL") ? atof(ENV("LARECOMP_FPS_INTERVAL")) : 2.0;
    if (prev_present > 0 && now - prev_present > max_gap) max_gap = now - prev_present;
    prev_present = now;
    if (now - last_fps_time >= fps_iv) {
        if (last_fps_time > 0)
            LOG("[vk] t=%.1f frame %llu %.1f fps max %.0fms (%s)\n", now, (unsigned long long)total_frames,
                frames / (now - last_fps_time), max_gap * 1000.0, renderer_stats());
        max_gap = 0;
        frames = 0;
        last_fps_time = now;
    }
}

// ================================================================== UI thread
void vk_exec_init(void);
void vk_shaders_init(void);

// exit() from any thread: let the GPU finish, keep the pipeline cache, then end the process without the DLL
// teardown (driver / validation-layer threads can stall it for minutes while the game's threads still run).
static void quick_exit_handler(void) {
    static int trace = -1;
    if (trace < 0) trace = ENV("LARECOMP_VK_TRACE") != NULL;
    if (trace) LOG("[vk] exit: waiting for the GPU\n");
    if (g_vk.dev) {
        vkDeviceWaitIdle(g_vk.dev);
        if (trace) LOG("[vk] exit: saving the pipeline cache\n");
        vk_pipeline_cache_save();
    }
    if (trace) LOG("[vk] exit: terminating\n");
    fflush(stdout);
    fflush(stderr);
#if defined(_WIN32) || defined(__linux__)
    plat_terminate();
#endif
}

static _Atomic bool g_vk_ready;
// The first size of the window, from the launcher (runtime/ui/launcher_lg.mm: its own fixed size, the game's after
// Play). A size asked for later isn't always given: Wayland compositors may keep a window at the size they chose.
static int g_hint_w, g_hint_h;
void ui_window_size_hint(int w, int h) { g_hint_w = w, g_hint_h = h; }

void ui_window_init(void) {
    if (window) return;
    atexit(quick_exit_handler);
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    settings_load();
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) FATAL("SDL_Init: %s\n", SDL_GetError());
    input_load_user_mappings();
    int win_w = 1280, win_h = 720;
    if (ENV("LARECOMP_WINDOW")) sscanf(ENV("LARECOMP_WINDOW"), "%dx%d", &win_w, &win_h);
    else if (g_hint_w > 0) win_w = g_hint_w, win_h = g_hint_h;
    SDL_WindowFlags wflags = SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY;
    if (ENV("LARECOMP_HIDDEN")) wflags |= SDL_WINDOW_HIDDEN;
    window = SDL_CreateWindow(RT_APP_NAME, win_w, win_h, wflags);
    if (!window) FATAL("SDL_CreateWindow: %s\n", SDL_GetError());
    rt_window_icon(window);
    // the Fullscreen setting applies from the start screen on (not only once the game runs); test windows stay windows
    if (g_settings.fullscreen && !g_hint_w && !ENV("LARECOMP_WINDOW") && !ENV("LARECOMP_HIDDEN")) SDL_SetWindowFullscreen(window, true);
    create_instance();
    if (!SDL_Vulkan_CreateSurface(window, g_vk.inst, NULL, &g_vk.surface)) FATAL("SDL_Vulkan_CreateSurface: %s\n", SDL_GetError());
    pick_device();
    create_device();
    create_swapchain();
    vk_exec_init();
    vk_shaders_init();
    atomic_store(&g_vk_ready, true);
}

// The device and everything made with it are set up (ui_window_init's end): g_vk.dev alone is set by vkCreateDevice,
// before the allocator, pipeline cache and exec.c's objects - the shader warm-up's threads (warmup.c), started before
// the window, compiled into a device still being made (Arceus under Wine: a crash in its first vkCreateShaderModule)
bool vk_ready(void) { return atomic_load(&g_vk_ready); }

SDL_Window *ui_window(void) { return window; }

void backend_init(void) {
    while (!atomic_load(&ui_ready)) SDL_Delay(1);
    LOG("[vk] %s, swapchain %ux%u\n", g_vk.props.deviceName, g_vk.sc_extent.width, g_vk.sc_extent.height);
}

// ---- in-place reboot (runtime/main.c teardown; main thread, guest threads stopped)
void backend_reset(void) {
    rt_reset();  // (the queued work belongs to the game that is going away)
    if (!g_vk.dev) return;
    vkDeviceWaitIdle(g_vk.dev);
    extern void vk_textures_reset(void);
    extern void vk_exec_reset(void);
    vk_textures_reset();
    vk_exec_reset();
    g_last_presented = NULL;
}
void backend_guest_memory_reset(void) {  // (exec.c re-imports the heap on first use)
    extern void gpuheap_guest_reset(void);
    gpuheap_guest_reset();  // (the GPU memory heap window: mapped again over the fresh guest range)
}
void backend_clear_window(void) {
    static const float black[4] = {0, 0, 0, 1};
    if (g_vk.dev && vk_frame_begin(black)) vk_frame_end(NULL);
}

int ui_run(void) {
    ui_window_init();
    atomic_store(&ui_ready, true);
    for (;;) {
        if (rt_reboot_requested()) return rt_reboot_requested();  // (runtime/main.c tears the game down)
        int drained = 0;
        // what's queued, then the window's own requests (fullscreen, text input) - every time: a controller sends
        // its sticks and motion sensor about a thousand times a second, so the queue may never be empty for long
        SDL_Event e;
        for (bool got = SDL_WaitEventTimeout(&e, 16); got; got = ++drained < 512 && SDL_PollEvent(&e)) {
            if (e.type == SDL_EVENT_QUIT) {
                LOG("window closed\n");
                exit(0);
            }
            if (input_user_blocked(&e)) continue;  // scripted test runs: the real keyboard / mouse / pads do nothing
            {   // (the game's Settings screen waiting for a new key / button: runtime/hle/za_options.c)
                extern bool bind_capture_event(const SDL_Event *e);
                if (bind_capture_event(&e)) continue;
            }
            if (menu_handle_event(&e)) continue;
            input_handle_event(&e);
        }
        menu_apply_window_requests(window);
    }
}

// The GPU for the start screen's recommended settings (runtime/ui/launcher_lg.mm): its name, its VRAM (device-local
// heaps, GB, rounded) and whether it is integrated (shares the system's memory)
void vk_describe_gpu(char *name, size_t n, int *vram_gb, bool *integrated) {
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(g_vk.phys, &mp);
    uint64_t vram = 0;
    for (uint32_t i = 0; i < mp.memoryHeapCount; i++)
        if (mp.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) vram += mp.memoryHeaps[i].size;
    snprintf(name, n, "%s", g_vk.props.deviceName);
    *vram_gb = (int)((vram + (512ull << 20)) >> 30);
    *integrated = g_vk.props.deviceType != VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU;
}
