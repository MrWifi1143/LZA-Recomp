// The guest heap window as GPU memory where the driver can't import host memory (no VK_EXT_external_memory_host:
// Mesa's Turnip on Adreno - the Steam Frame). exec.c's shader global memory, compute output read back as vertex data,
// transform feedback and query results all need the GPU to see the game's heap in place; without it Z-A's world drew as
// huge stretched slabs (the vertices its compute shaders write never reached guest memory).
//
// Here the heap window is made of Vulkan memory instead: 64 MB chunks allocated as exportable dma-bufs (on a small
// device of our own, at boot: the game's device comes later) and mmapped at their guest addresses (MAP_FIXED over the
// reserved guest range), as the game is handed each part of the window - SetHeapSize, the StandardAllocator block
// (hle/heaps.c) - before it writes there. Only those parts: Turnip commits an allocation's pages at once.
// exec.c imports the chunks into the game's device and binds them into one sparse buffer over the whole window, so
// shaders keep one base address (gmem_ref_info). Also on RADV, where importing costs every submit (see decide).
// LARECOMP_DMABUF_HEAP=0 turns it off; =1 tries it on any driver.
//
// Lazy (port of Z-A's; here only the host import below uses it - the dma-buf heap is backed whole, as before, unless
// LARECOMP_GPUHEAP_LAZY=1): a chunk of the StandardAllocator block is backed as it's first used, not up front - an
// unbacked chunk is PROT_NONE, and
//  - a CPU access there (the game's or ours) faults: gpuheap_fault, from the crash handler (platform/linux.c), backs
//    the chunk and the access retries;
//  - an NVN buffer (nvn/objects.c: what the GPU reads and writes by address - compute output, transform feedback,
//    queries - maybe never touched by the CPU) and a host syscall's write (pagewatch_prewrite) back their range first
//    (gpuheap_touch).
// Shaders read zeros in an unbacked chunk (the sparse buffer's unbound range).
//
// Lazy host import (any driver with VK_EXT_external_memory_host and sparse buffers; Windows too): the same chunks and
// faults over ordinary guest memory, for the host memory import (exec.c). The import pins what it covers, and on an
// integrated GPU that counts against the GPU's own budget: Intel on Windows has one heap of half the RAM (~4 GB with
// 8 GB), the whole window imported was 3 GB of it, and Z-A ran out of graphics memory after the opening train (720p,
// Switch settings). So a chunk is imported only once it's used - an unbacked chunk is PROT_NONE (Linux) or left
// uncommitted (Windows, where the vectored handler sees its first access), backing it makes it read-write, and exec.c
// imports each backed chunk (64 MB, VkImportMemoryHostPointerInfoEXT) and binds it into one sparse buffer over the
// window. On by default for integrated GPUs and small heaps (host_lazy_wanted); LARECOMP_LAZY_IMPORT=0 / 1 forces it
// off / on. Without sparse buffers (or if the device can't take the chunks: gpuheap_host_cancel) the whole import as before.
// Arceus: Intel UHD on Windows (8 GB) failed the whole import outright (vkAllocateMemory -2 for 3812 MB): shader global
// memory read zeros and every dialog and button text was blank (its text shaders read the font data through it).
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "../../hle.h"
#include "../../rt.h"
#include "vk.h"

#if defined(__linux__) || defined(_WIN32)
#include <pthread.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#include <unistd.h>
#endif

#define CHUNK (64ull << 20)
#define NCHUNK (HEAP_MAX / CHUNK)

enum { OFF = -1, UNDECIDED = 0, DMABUF = 1, HOSTIMPORT = 2 };
static struct {
    pthread_mutex_t lock;
    int state;  // UNDECIDED, DMABUF (the heap window is GPU memory), HOSTIMPORT (lazy host import), OFF
    VkInstance inst;
    VkPhysicalDevice phys;
    VkDevice dev;
    uint32_t mtype;
    PFN_vkGetMemoryFdKHR get_fd;
    int fd[NCHUNK];       // the chunk's dma-buf (-1: not backed)
    bool host_backed[NCHUNK];  // (HOSTIMPORT) read-write / committed: exec.c imports it
    bool handed[NCHUNK];  // given to the game's device (gpuheap_take / gpuheap_take_host)
    uint64_t end;         // window bytes from HEAP_BASE that are (or may be, lazily) backed
    bool lazy;            // (RADV, lazy host import: see the top)
    bool table;           // (HOSTIMPORT) no sparse buffer: each chunk its own import and buffer, a table (g_maxwell_gmem_table)
    bool lazy_chunk[NCHUNK];  // (backed when the allocator hands out memory in it)
    unsigned backed;      // chunks backed
} H = {.lock = PTHREAD_MUTEX_INITIALIZER};
uint64_t g_gpuheap_end;  // (H.end, read without the lock: exec.c's per-draw check for new chunks)
uint64_t g_gpuheap_gen;  // (chunks backed so far: a lazy chunk backed below H.end changes this, not H.end)

static inline void *chunk_at(unsigned i) { return (void *)(uintptr_t)(HEAP_BASE + (uint64_t)i * CHUNK); }
static inline bool is_backed(unsigned i) {
    return H.state == HOSTIMPORT ? __atomic_load_n(&H.host_backed[i], __ATOMIC_ACQUIRE) : __atomic_load_n(&H.fd[i], __ATOMIC_ACQUIRE) >= 0;
}
#ifdef _WIN32
// The reserved (never committed) parts of [p, p + n) committed read-write; committed pages keep their protection
// (pagewatch's read-only pages: committing them again would make them writable unseen)
static bool commit_reserved(void *p, uint64_t n) {
    uint8_t *a = p, *e = a + n;
    while (a < e) {
        MEMORY_BASIC_INFORMATION mi;
        if (!VirtualQuery(a, &mi, sizeof mi)) return false;
        uint8_t *re = (uint8_t *)mi.BaseAddress + mi.RegionSize;
        if (re > e) re = e;
        if (mi.State == MEM_RESERVE && !VirtualAlloc(a, (SIZE_T)(re - a), MEM_COMMIT, PAGE_READWRITE)) return false;
        a = re;
    }
    return true;
}
static bool any_committed(void *p, uint64_t n) {
    uint8_t *a = p, *e = a + n;
    while (a < e) {
        MEMORY_BASIC_INFORMATION mi;
        if (!VirtualQuery(a, &mi, sizeof mi)) return true;
        if (mi.State == MEM_COMMIT) return true;
        a = (uint8_t *)mi.BaseAddress + mi.RegionSize;
    }
    return false;
}
#endif

static bool has_ext(VkExtensionProperties *e, uint32_t n, const char *name) {
    for (uint32_t i = 0; i < n; i++) if (!strcmp(e[i].extensionName, name)) return true;
    return false;
}

// The usage exec.c gives the heap window's sparse buffer (gmem_sparse_bind)
#define GMEM_USAGE (VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | \
                    VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | \
                    VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT)

// Whether the host import should be lazy on this device (it has VK_EXT_external_memory_host): where the import's pinned
// memory is scarce - an integrated GPU (its memory is the system's, the driver's budget for it a part), or a heap the
// import may land in under 6 GB - and only if the device can bind imported host memory into a sparse buffer.
// LARECOMP_LAZY_IMPORT=1: wherever it can.
// Where it can't (the "why" cases: AMD's Windows driver can't back a sparse buffer with host memory, and its whole 3.8 GB
// import failed on 8 GB of RAM - blank text, missing models): the chunk table instead (H.table, g_maxwell_gmem_table) -
// each chunk imported as a buffer of its own and shaders find a word's chunk through a table of their GPU addresses
// (exec.c gmem_table_bind, gmem_ref_info). LARECOMP_CHUNK_TABLE=1: the chunk table wherever the host import is (a test,
// sparse or not); =0: never (the whole import where sparse can't).
static bool host_lazy_wanted(const VkPhysicalDeviceFeatures *f) {
    const char *lz = ENV("LARECOMP_LAZY_IMPORT"), *ct = ENV("LARECOMP_CHUNK_TABLE");
    bool force = lz && *lz == '1', table_on = ct && *ct == '1', table_off = ct && *ct == '0';
    VkPhysicalDeviceProperties p;
    vkGetPhysicalDeviceProperties(H.phys, &p);
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(H.phys, &mp);
    uint64_t host_heap = 0, any_heap = 0;  // (the biggest system-memory heap; the biggest heap)
    for (uint32_t i = 0; i < mp.memoryHeapCount; i++) {
        uint64_t sz = mp.memoryHeaps[i].size;
        if (!(mp.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) && sz > host_heap) host_heap = sz;
        if (sz > any_heap) any_heap = sz;
    }
    uint64_t heap = host_heap ? host_heap : any_heap;
    bool integrated = p.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU, small = heap < (6ull << 30);
    if (!force && !table_on && !integrated && !small) return false;
    const char *why = NULL;
    uint32_t nq = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(H.phys, &nq, NULL);
    VkQueueFamilyProperties qp[16];
    if (nq > 16) nq = 16;
    vkGetPhysicalDeviceQueueFamilyProperties(H.phys, &nq, qp);
    uint32_t q = UINT32_MAX;  // (device.c pick_device: the first graphics + compute family that can present)
    for (uint32_t i = 0; i < nq && q == UINT32_MAX; i++)
        if ((qp[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && (qp[i].queueFlags & VK_QUEUE_COMPUTE_BIT)) q = i;
    VkPhysicalDeviceExternalBufferInfo xi = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_BUFFER_INFO,
                                             .flags = VK_BUFFER_CREATE_SPARSE_BINDING_BIT | VK_BUFFER_CREATE_SPARSE_RESIDENCY_BIT,
                                             .usage = GMEM_USAGE, .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT};
    VkExternalBufferProperties xp = {.sType = VK_STRUCTURE_TYPE_EXTERNAL_BUFFER_PROPERTIES};
    if (!f->sparseBinding || !f->sparseResidencyBuffer) why = "no sparse buffers";
    else if (q == UINT32_MAX || !(qp[q].queueFlags & VK_QUEUE_SPARSE_BINDING_BIT)) why = "the graphics queue can't bind sparse memory";
    else {
        vkGetPhysicalDeviceExternalBufferProperties(H.phys, &xi, &xp);
        if (!(xp.externalMemoryProperties.externalMemoryFeatures & VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT))
            why = "host memory can't back a sparse buffer";
    }
    const char *who = table_on ? "LARECOMP_CHUNK_TABLE=1" : force ? "LARECOMP_LAZY_IMPORT=1" : integrated ? "integrated GPU" : "small heap";
    if (table_on || (why && !table_off)) {
        H.table = true;
        LOG("[vk] guest heap: imported in %llu MB chunks as they're first used (chunk table: %s; %s, %llu MB heap)\n",
            CHUNK >> 20, why ? why : "sparse binding not used", who, (unsigned long long)(heap >> 20));
        return true;
    }
    if (why) {
        LOG("[vk] guest heap: imported whole (%s: no lazy import; %s, %llu MB heap)\n", why,
            integrated ? "integrated GPU" : "small heap", (unsigned long long)(heap >> 20));
        return false;
    }
    LOG("[vk] guest heap: imported in %llu MB chunks as they're first used (%s, %llu MB heap)\n", CHUNK >> 20, who,
        (unsigned long long)(heap >> 20));
    return true;
}

#ifdef __linux__
// (H.lock held) the dma-buf heap on H.phys: its memory type and the small device the chunks are allocated on
static bool dmabuf_setup(void) {
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(H.phys, &mp);
    const uint32_t VIS = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    const uint32_t pref[] = {VIS | VK_MEMORY_PROPERTY_HOST_CACHED_BIT | VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                             VIS | VK_MEMORY_PROPERTY_HOST_CACHED_BIT, VIS};  // (cached: the game's CPU code lives in it)
    H.mtype = UINT32_MAX;
    for (int w = 0; w < 3 && H.mtype == UINT32_MAX; w++)
        for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
            if ((mp.memoryTypes[i].propertyFlags & pref[w]) == pref[w]) { H.mtype = i; break; }
    if (H.mtype == UINT32_MAX) return false;
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qi = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueCount = 1, .pQueuePriorities = &prio};
    const char *ext[] = {VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME, VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME};
    VkDeviceCreateInfo dci = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1, .pQueueCreateInfos = &qi,
                              .enabledExtensionCount = 2, .ppEnabledExtensionNames = ext};
    if (vkCreateDevice(H.phys, &dci, NULL, &H.dev) != VK_SUCCESS) { H.dev = VK_NULL_HANDLE; return false; }
    H.get_fd = (PFN_vkGetMemoryFdKHR)vkGetDeviceProcAddr(H.dev, "vkGetMemoryFdKHR");
    if (!H.get_fd) { vkDestroyDevice(H.dev, NULL), H.dev = VK_NULL_HANDLE; return false; }
    H.state = DMABUF;
    // (backed whole on Arceus, as before; LARECOMP_GPUHEAP_LAZY=1: as its chunks are used, Z-A's way)
    H.lazy = ENV("LARECOMP_GPUHEAP_LAZY") && *ENV("LARECOMP_GPUHEAP_LAZY") == '1';
    LOG("[vk] no host memory import: the guest heap is made of GPU memory (dma-buf, memory type %u, %#x)\n", H.mtype,
        mp.memoryTypes[H.mtype].propertyFlags);
    return true;
}
#endif

// (H.lock held) the device device.c will pick (pick_device's order: LARECOMP_GPU, discrete, integrated, other; Vulkan
// 1.3), and which way the heap window reaches it: GPU memory (dma-buf, Linux), the host import in chunks, or (OFF)
// the whole host import (exec.c gmem_import)
static void decide(void) {
    H.state = OFF;
    for (unsigned i = 0; i < NCHUNK; i++) H.fd[i] = -1;
    const char *e = ENV("LARECOMP_DMABUF_HEAP"), *lz = ENV("LARECOMP_LAZY_IMPORT");
#ifdef __linux__
    bool dma_ok = !(e && *e == '0');
#else
    bool dma_ok = false;
    (void)e;
#endif
    bool lazy_ok = !(lz && *lz == '0');
    if (!dma_ok && !lazy_ok) return;
    VkApplicationInfo ai = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_3};
    VkInstanceCreateInfo ici = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &ai};
    if (vkCreateInstance(&ici, NULL, &H.inst) != VK_SUCCESS) { H.inst = VK_NULL_HANDLE; return; }
    uint32_t n = 0;
    vkEnumeratePhysicalDevices(H.inst, &n, NULL);
    VkPhysicalDevice *pd = calloc(n ? n : 1, sizeof *pd);
    vkEnumeratePhysicalDevices(H.inst, &n, pd);
    int want = ENV("LARECOMP_GPU") ? atoi(ENV("LARECOMP_GPU")) : -1, best = -1, best_score = -1;
    for (uint32_t i = 0; i < n; i++) {
        VkPhysicalDeviceProperties p;
        vkGetPhysicalDeviceProperties(pd[i], &p);
        if (p.apiVersion < VK_API_VERSION_1_3) continue;
        int score = p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 3 : p.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? 2 : 1;
        if ((int)i == want) score = 100;
        if (score > best_score) best_score = score, best = (int)i;
    }
    if (best >= 0) H.phys = pd[best];
    free(pd);
    if (!H.phys) goto off;
    uint32_t ne = 0;
    vkEnumerateDeviceExtensionProperties(H.phys, NULL, &ne, NULL);
    VkExtensionProperties *ep = calloc(ne ? ne : 1, sizeof *ep);
    vkEnumerateDeviceExtensionProperties(H.phys, NULL, &ne, ep);
    bool host = has_ext(ep, ne, VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME);
    bool dmabuf = has_ext(ep, ne, VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME) && has_ext(ep, ne, VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME);
    free(ep);
    VkPhysicalDeviceFeatures f;
    vkGetPhysicalDeviceFeatures(H.phys, &f);
#ifdef __linux__
    // RADV (AMD on Linux) too, though it imports host memory: the kernel driver revalidates an imported (userptr) buffer's
    // pages on every queue submit, all 3+ GB of the heap - the render thread spent ~20 of its 24 ms per frame in the
    // amdgpu submit ioctl (PS5 Linux: main menu 41 -> 60 fps this way)
    VkPhysicalDeviceDriverProperties drv = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
    VkPhysicalDeviceProperties2 p2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &drv};
    vkGetPhysicalDeviceProperties2(H.phys, &p2);
    bool radv = drv.driverID == VK_DRIVER_ID_MESA_RADV;
    if (dma_ok && !(host && !radv && !(e && *e == '1')) && dmabuf && f.sparseBinding && f.sparseResidencyBuffer && dmabuf_setup())
        return;  // (the instance and device stay: the chunks are allocated on it)
#else
    (void)dmabuf;
#endif
    if (lazy_ok && host && host_lazy_wanted(&f)) {
        H.state = HOSTIMPORT;
        H.lazy = true;
        extern bool g_maxwell_gmem_table;  // (the translator's GLSL for it: decided here, before the first translation -
        g_maxwell_gmem_table = H.table;    // warmup.c asks first - and kept, whatever happens to the import later)
    }
off:
    if (H.dev) vkDestroyDevice(H.dev, NULL), H.dev = VK_NULL_HANDLE;
    if (H.inst) vkDestroyInstance(H.inst, NULL), H.inst = VK_NULL_HANDLE;
}

static int heap_state(void) {
    pthread_mutex_lock(&H.lock);
    if (H.state == UNDECIDED) decide();
    int st = H.state;
    pthread_mutex_unlock(&H.lock);
    return st;
}
bool gpuheap_on(void) { return heap_state() == DMABUF; }      // the heap window is GPU memory (device.c, exec.c)
bool gpuheap_lazy_host(void) { return heap_state() == HOSTIMPORT; }  // the host import in chunks as they're used
bool gpuheap_chunk_table(void) { return heap_state() == HOSTIMPORT && H.table; }  // ... each its own buffer (no sparse)

// (H.lock held) HOSTIMPORT: chunk i made read-write (Linux) / committed (Windows); exec.c imports it next
static void host_back(unsigned i) {
    if (H.host_backed[i]) return;
#ifdef _WIN32
    if (!commit_reserved(chunk_at(i), CHUNK))
        FATAL("guest heap: committing %llx..%llx failed (%lu): out of memory?\n", (unsigned long long)(uintptr_t)chunk_at(i),
              (unsigned long long)(uintptr_t)chunk_at(i) + CHUNK, GetLastError());
#else
    if (H.lazy_chunk[i] && mprotect(chunk_at(i), CHUNK, PROT_READ | PROT_WRITE))
        FATAL("guest heap: mprotect %llx failed\n", (unsigned long long)(uintptr_t)chunk_at(i));
#endif
    __atomic_store_n(&H.host_backed[i], true, __ATOMIC_RELEASE);
    __atomic_store_n(&H.lazy_chunk[i], false, __ATOMIC_RELEASE);
    H.backed++;
    if ((i + 1) * CHUNK > H.end) H.end = (i + 1) * CHUNK;
}
// (H.lock held) HOSTIMPORT: chunk i left for later - its first access faults
static void host_unback(unsigned i) {
#ifdef _WIN32
    (void)i;  // (reserved, not committed: the vectored handler sees the first access)
#else
    mprotect(chunk_at(i), CHUNK, PROT_NONE);
#endif
}

#ifdef __linux__
// (H.lock held) chunk i's dma-buf mapped at its guest address. Whatever the reserved range held there first is kept
// (a chunk is normally backed before anything is written in it: only pages already in memory are copied)
static bool map_chunk(unsigned i, int fd) {
    void *at = (void *)(uintptr_t)(HEAP_BASE + i * CHUNK);
    unsigned char vec[CHUNK / 4096];
    bool used = false;
    if (!mincore(at, CHUNK, vec))
        for (size_t k = 0; k < sizeof vec && !used; k++) used = vec[k] & 1;
    if (used) {
        void *tmp = mmap(NULL, CHUNK, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        if (tmp == MAP_FAILED) return false;
        memcpy(tmp, at, CHUNK);
        munmap(tmp, CHUNK);
    }
    return mmap(at, CHUNK, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, fd, 0) == at;
}

// (H.lock held) chunk i backed now. A lazy chunk is PROT_NONE until then: made read-only for map_chunk's copy (a
// store there faults and waits for H.lock in gpuheap_fault), so nothing is written while it's copied
static void dma_back_chunk(unsigned i) {
    if (H.fd[i] >= 0) return;
    if (H.lazy_chunk[i]) mprotect((void *)(uintptr_t)(HEAP_BASE + i * CHUNK), CHUNK, PROT_READ);
    {
        VkExportMemoryAllocateInfo ex = {.sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO,
                                         .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT};
        VkMemoryAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .pNext = &ex, .allocationSize = CHUNK,
                                   .memoryTypeIndex = H.mtype};
        VkDeviceMemory m;
        VkResult r = vkAllocateMemory(H.dev, &ai, NULL, &m);
        VkMemoryGetFdInfoKHR gi = {.sType = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR, .memory = m,
                                   .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT};
        int fd = -1;
        if (r == VK_SUCCESS) r = H.get_fd(H.dev, &gi, &fd);
        if (r != VK_SUCCESS || !map_chunk(i, fd))
            FATAL("guest heap: GPU memory for %llx..%llx failed (%d): out of memory?\n", (unsigned long long)(HEAP_BASE + i * CHUNK),
                  (unsigned long long)(HEAP_BASE + (i + 1) * CHUNK), (int)r);
        __atomic_store_n(&H.fd[i], fd, __ATOMIC_RELEASE);  // (the allocation stays: the dma-buf and its mapping hold the memory)
        H.lazy_chunk[i] = false;
        H.backed++;
        if ((i + 1) * CHUNK > H.end) H.end = (i + 1) * CHUNK;
    }
}
#endif
static void back_chunk(unsigned i) {
#ifdef __linux__
    if (H.state == DMABUF) { dma_back_chunk(i); return; }
#endif
    host_back(i);
}

// The game is handed [addr, addr + size) of the heap window: back every chunk from HEAP_BASE up to its end
// (kernel.c SetHeapSize, hle/heaps.c: the StandardAllocator block - lazily on RADV: its first and last chunk now, the
// rest as the allocator hands them out)
void gpuheap_back(uint64_t addr, uint64_t size) {
    if (addr < HEAP_BASE || !size || heap_state() <= 0) return;
    uint64_t end = addr + size - HEAP_BASE;
    if (end > HEAP_MAX) end = HEAP_MAX;
    bool lazy = H.lazy && addr >= STDHEAP_BASE;
    pthread_mutex_lock(&H.lock);
    unsigned first = (unsigned)((addr - HEAP_BASE) / CHUNK), last = (unsigned)((end - 1) / CHUNK);
    for (unsigned i = 0; i <= last; i++) {
        if (lazy && i > first && i < last && !is_backed(i)) {
#ifdef _WIN32
            if (!H.lazy_chunk[i] && any_committed(chunk_at(i), CHUNK)) { back_chunk(i); continue; }  // (in use already)
#endif
            if (!H.lazy_chunk[i]) {
#ifdef __linux__
                if (H.state == DMABUF) mprotect(chunk_at(i), CHUNK, PROT_NONE); else
#endif
                host_unback(i);
            }
            H.lazy_chunk[i] = true;
            continue;
        }
        back_chunk(i);
    }
    if (end > H.end) H.end = (end + CHUNK - 1) / CHUNK * CHUNK;
    __atomic_store_n(&g_gpuheap_gen, H.backed, __ATOMIC_RELEASE);
    __atomic_store_n(&g_gpuheap_end, H.end, __ATOMIC_RELEASE);
    pthread_mutex_unlock(&H.lock);
    LOG("[vk] guest heap: %llu MB of %s from %llx (%llu MB of the window%s)\n", (unsigned long long)H.backed * (CHUNK >> 20),
        H.state == DMABUF ? "GPU memory" : "it in use", (unsigned long long)HEAP_BASE, (unsigned long long)(H.end >> 20),
        lazy ? H.state == DMABUF ? ": the rest backed as it's used" : ": the rest imported as it's used" : "");
}

// [addr, addr + n) about to be used where a fault won't do (the GPU, a syscall): its unbacked chunks backed
void gpuheap_touch(uint64_t addr, uint64_t n) {
    if (!H.lazy || !n || addr < HEAP_BASE || addr - HEAP_BASE >= HEAP_MAX) return;
    uint64_t o = addr - HEAP_BASE, e = o + n > HEAP_MAX ? HEAP_MAX : o + n;
    unsigned a = (unsigned)(o / CHUNK), b = (unsigned)((e - 1) / CHUNK);
    bool need = false;
    for (unsigned i = a; i <= b && !need; i++) need = __atomic_load_n(&H.lazy_chunk[i], __ATOMIC_ACQUIRE);
    if (!need) return;
    pthread_mutex_lock(&H.lock);
    for (unsigned i = a; i <= b; i++) if (H.lazy_chunk[i]) back_chunk(i);
    __atomic_store_n(&g_gpuheap_gen, H.backed, __ATOMIC_RELEASE);
    pthread_mutex_unlock(&H.lock);
}

// (the crash handler, SIGSEGV at addr) an unbacked chunk's first access: backed, true (the access retries). A fault in
// a chunk that is backed isn't this one's: it is pagewatch's (a page the renderer keeps a copy of, read-only until the
// game writes it) - claimed here, the page stayed read-only and the store faulted forever: Z-A on a Radeon (vertex data
// kept in VRAM, so pages watched) hung at the next load after the first (fast travel; a new game after the name).
bool gpuheap_fault(uint64_t addr) {
    if (!H.lazy || addr < HEAP_BASE || addr - HEAP_BASE >= HEAP_MAX) return false;
    unsigned i = (unsigned)((addr - HEAP_BASE) / CHUNK);
    if (!__atomic_load_n(&H.lazy_chunk[i], __ATOMIC_ACQUIRE)) return false;
    pthread_mutex_lock(&H.lock);
    if (H.lazy_chunk[i]) back_chunk(i);
    bool ok = is_backed(i);
    __atomic_store_n(&g_gpuheap_gen, H.backed, __ATOMIC_RELEASE);
    pthread_mutex_unlock(&H.lock);
    return ok;
}
// (the crash handler, after pagewatch_fault declined too) a fault in a chunk another thread backed between this
// thread's access and gpuheap_fault: true (the access retries; the chunk is read-write now)
bool gpuheap_backed_meanwhile(uint64_t addr) {
    if (!H.lazy || addr < HEAP_BASE || addr - HEAP_BASE >= HEAP_MAX) return false;
    return is_backed((unsigned)((addr - HEAP_BASE) / CHUNK));
}
unsigned gpuheap_backed_mb(void) { return H.backed * (unsigned)(CHUNK >> 20); }
uint64_t gpuheap_unbacked(void) {  // (lazy chunks not backed yet: room the game may still take - texpack_room)
    uint64_t n = 0;
    for (unsigned i = 0; i < NCHUNK; i++) n += __atomic_load_n(&H.lazy_chunk[i], __ATOMIC_RELAXED) ? CHUNK : 0;
    return n;
}

uint64_t gpuheap_end(void) {  // window bytes backed
    pthread_mutex_lock(&H.lock);
    uint64_t e = H.end;
    pthread_mutex_unlock(&H.lock);
    return e;
}

// Chunks the game's device hasn't imported yet: their offsets in the window and a dma-buf fd each (the caller's to
// import, which takes it). Returns how many.
int gpuheap_take(uint64_t *off, int *fds, int max) {
    int n = 0;
#ifdef __linux__
    pthread_mutex_lock(&H.lock);
    for (unsigned i = 0; i < NCHUNK && n < max; i++)
        if (H.fd[i] >= 0 && !H.handed[i]) {
            int d = dup(H.fd[i]);
            if (d < 0) break;
            H.handed[i] = true, off[n] = i * CHUNK, fds[n++] = d;
        }
    pthread_mutex_unlock(&H.lock);
#else
    (void)off, (void)fds, (void)max;
#endif
    return n;
}
// (HOSTIMPORT) Backed chunks the game's device hasn't imported yet: their offsets in the window. Returns how many.
int gpuheap_take_host(uint64_t *off, int max) {
    int n = 0;
    pthread_mutex_lock(&H.lock);
    for (unsigned i = 0; i < NCHUNK && n < max; i++)
        if (H.host_backed[i] && !H.handed[i]) H.handed[i] = true, off[n++] = i * CHUNK;
    pthread_mutex_unlock(&H.lock);
    return n;
}
// (HOSTIMPORT) The device can't import in chunks after all (device.c, exec.c): every chunk left for later backed now,
// nothing lazy from here - exec.c imports the window whole, as before
void gpuheap_host_cancel(void) {
    pthread_mutex_lock(&H.lock);
    if (H.state == HOSTIMPORT) {
        for (unsigned i = 0; i < NCHUNK; i++) if (H.lazy_chunk[i]) host_back(i);
        H.lazy = false;
        H.state = OFF;
        __atomic_store_n(&g_gpuheap_gen, H.backed, __ATOMIC_RELEASE);
        LOG("[vk] guest heap: lazy import off, imported whole\n");
    }
    pthread_mutex_unlock(&H.lock);
}
uint64_t gpuheap_chunk_size(void) { return CHUNK; }

// After a reboot reset the guest range (fresh anonymous memory over it / decommitted on Windows): the dma-buf chunks
// mapped there again, zeroed (the game's device keeps its imports: the same memory); the host import's backed chunks
// imported again (exec.c dropped its imports at the reset), its lazy ones unbacked again
void gpuheap_guest_reset(void) {
    pthread_mutex_lock(&H.lock);
    for (unsigned i = 0; i < NCHUNK; i++)
        if (H.state == HOSTIMPORT && H.host_backed[i]) {
#ifdef _WIN32
            commit_reserved(chunk_at(i), CHUNK);
#endif
            H.handed[i] = false;
        } else if (H.state == HOSTIMPORT && H.lazy_chunk[i]) {
            host_unback(i);
#ifdef __linux__
        } else if (H.fd[i] >= 0) {
            void *at = chunk_at(i);
            if (mmap(at, CHUNK, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, H.fd[i], 0) != at) FATAL("guest heap: remap failed\n");
            memset(at, 0, CHUNK);
            H.handed[i] = false;  // (exec.c dropped its imports at the reset: imported again)
        } else if (H.lazy_chunk[i]) {
            mprotect(chunk_at(i), CHUNK, PROT_NONE);  // (the fresh range: unbacked again)
#endif
        }
    pthread_mutex_unlock(&H.lock);
}

#else  // (macOS: no Vulkan renderer)
uint64_t g_gpuheap_end, g_gpuheap_gen;
bool gpuheap_lazy_host(void) { return false; }
bool gpuheap_chunk_table(void) { return false; }
int gpuheap_take_host(uint64_t *off, int max) { (void)off, (void)max; return 0; }
void gpuheap_host_cancel(void) {}
void gpuheap_touch(uint64_t addr, uint64_t n) { (void)addr, (void)n; }
bool gpuheap_fault(uint64_t addr) { (void)addr; return false; }
bool gpuheap_backed_meanwhile(uint64_t addr) { (void)addr; return false; }
unsigned gpuheap_backed_mb(void) { return 0; }
uint64_t gpuheap_unbacked(void) { return 0; }
bool gpuheap_on(void) { return false; }
void gpuheap_back(uint64_t addr, uint64_t size) { (void)addr, (void)size; }
uint64_t gpuheap_end(void) { return 0; }
int gpuheap_take(uint64_t *off, int *fds, int max) { (void)off, (void)fds, (void)max; return 0; }
uint64_t gpuheap_chunk_size(void) { return 0; }
void gpuheap_guest_reset(void) {}
#endif
