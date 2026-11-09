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
// Lazy on RADV: there every chunk is GTT memory and counts against the kernel's GTT limit from the moment it exists -
// the PS5 under Linux has 512 MB VRAM and 7.2 GB GTT, and Z-A's StandardAllocator block at max settings is 5.75 GB
// (hle/za_fixes.c: headroom for 4K targets, People and Detail): backed whole, GTT sat at its limit and anything more
// (a texture pack) made gamescope fault and the session restart. The game takes nearly all of the block for its own
// heap at once (one Allocate), but touches ~1.9 GB of it in the field (41 of its 92 chunks: PS5, max settings). So the
// block's chunks are backed as they're first used, not up front: an unbacked chunk is PROT_NONE, and
//  - a CPU access there (the game's or ours) faults: gpuheap_fault, from the crash handler (platform/linux.c), backs
//    the chunk and the access retries;
//  - an NVN buffer (nvn/objects.c: what the GPU reads and writes by address - compute output, transform feedback,
//    queries - maybe never touched by the CPU) and a host syscall's write (pagewatch_prewrite) back their range first
//    (gpuheap_touch).
// Shaders read zeros in an unbacked chunk (the sparse buffer's unbound range). LARECOMP_GPUHEAP_EAGER=1: all up front.
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "../../hle.h"
#include "../../rt.h"
#include "vk.h"

#if defined(__linux__)
#include <pthread.h>
#include <sys/mman.h>
#include <unistd.h>

#define CHUNK (64ull << 20)
#define NCHUNK (HEAP_MAX / CHUNK)

static struct {
    pthread_mutex_t lock;
    int state;  // 0 undecided, 1 on, -1 off
    VkInstance inst;
    VkPhysicalDevice phys;
    VkDevice dev;
    uint32_t mtype;
    PFN_vkGetMemoryFdKHR get_fd;
    int fd[NCHUNK];       // the chunk's dma-buf (-1: not backed)
    bool handed[NCHUNK];  // given to the game's device (gpuheap_take)
    uint64_t end;         // window bytes from HEAP_BASE that are (or may be, lazily) backed
    bool lazy;            // (RADV: see the top)
    bool lazy_chunk[NCHUNK];  // (backed when the allocator hands out memory in it)
    unsigned backed;      // chunks backed
} H = {.lock = PTHREAD_MUTEX_INITIALIZER};
uint64_t g_gpuheap_end;  // (H.end, read without the lock: exec.c's per-draw check for new chunks)
uint64_t g_gpuheap_gen;  // (chunks backed so far: a lazy chunk backed below H.end changes this, not H.end)

static bool has_ext(VkExtensionProperties *e, uint32_t n, const char *name) {
    for (uint32_t i = 0; i < n; i++) if (!strcmp(e[i].extensionName, name)) return true;
    return false;
}

// (H.lock held) the device device.c will pick (pick_device's order: LARECOMP_GPU, discrete, integrated, other; Vulkan
// 1.3), and whether this way is needed and possible there
static void decide(void) {
    H.state = -1;
    for (unsigned i = 0; i < NCHUNK; i++) H.fd[i] = -1;
    const char *e = ENV("LARECOMP_DMABUF_HEAP");
    if (e && *e == '0') return;
    VkApplicationInfo ai = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_3};
    VkInstanceCreateInfo ici = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &ai};
    if (vkCreateInstance(&ici, NULL, &H.inst) != VK_SUCCESS) return;
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
    // RADV (AMD on Linux) too, though it imports host memory: the kernel driver revalidates an imported (userptr) buffer's
    // pages on every queue submit, all 3+ GB of the heap - the render thread spent ~20 of its 24 ms per frame in the
    // amdgpu submit ioctl (PS5 Linux: main menu 41 -> 60 fps this way)
    VkPhysicalDeviceDriverProperties drv = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
    VkPhysicalDeviceProperties2 p2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &drv};
    vkGetPhysicalDeviceProperties2(H.phys, &p2);
    bool radv = drv.driverID == VK_DRIVER_ID_MESA_RADV;
    if ((host && !radv && !(e && *e == '1')) || !dmabuf || !f.sparseBinding || !f.sparseResidencyBuffer) goto off;  // (the import: exec.c)
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(H.phys, &mp);
    const uint32_t VIS = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    const uint32_t pref[] = {VIS | VK_MEMORY_PROPERTY_HOST_CACHED_BIT | VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                             VIS | VK_MEMORY_PROPERTY_HOST_CACHED_BIT, VIS};  // (cached: the game's CPU code lives in it)
    H.mtype = UINT32_MAX;
    for (int w = 0; w < 3 && H.mtype == UINT32_MAX; w++)
        for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
            if ((mp.memoryTypes[i].propertyFlags & pref[w]) == pref[w]) { H.mtype = i; break; }
    if (H.mtype == UINT32_MAX) goto off;
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qi = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueCount = 1, .pQueuePriorities = &prio};
    const char *ext[] = {VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME, VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME};
    VkDeviceCreateInfo dci = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1, .pQueueCreateInfos = &qi,
                              .enabledExtensionCount = 2, .ppEnabledExtensionNames = ext};
    if (vkCreateDevice(H.phys, &dci, NULL, &H.dev) != VK_SUCCESS) goto off;
    H.get_fd = (PFN_vkGetMemoryFdKHR)vkGetDeviceProcAddr(H.dev, "vkGetMemoryFdKHR");
    if (!H.get_fd) goto off;
    H.state = 1;
    H.lazy = radv && !(ENV("LARECOMP_GPUHEAP_EAGER") && *ENV("LARECOMP_GPUHEAP_EAGER") == '1');
    LOG("[vk] no host memory import: the guest heap is made of GPU memory (dma-buf, memory type %u, %#x)\n", H.mtype,
        mp.memoryTypes[H.mtype].propertyFlags);
    return;
off:
    if (H.dev) vkDestroyDevice(H.dev, NULL), H.dev = VK_NULL_HANDLE;
    if (H.inst) vkDestroyInstance(H.inst, NULL), H.inst = VK_NULL_HANDLE;
}

bool gpuheap_on(void) {
    pthread_mutex_lock(&H.lock);
    if (!H.state) decide();
    bool on = H.state > 0;
    pthread_mutex_unlock(&H.lock);
    return on;
}

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
static void back_chunk(unsigned i) {
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
        H.fd[i] = fd;  // (the allocation stays: the dma-buf and its mapping hold the memory)
        H.lazy_chunk[i] = false;
        H.backed++;
        if ((i + 1) * CHUNK > H.end) H.end = (i + 1) * CHUNK;
    }
}

// The game is handed [addr, addr + size) of the heap window: back every chunk from HEAP_BASE up to its end
// (kernel.c SetHeapSize, hle/heaps.c: the StandardAllocator block - lazily on RADV: its first and last chunk now, the
// rest as the allocator hands them out)
void gpuheap_back(uint64_t addr, uint64_t size) {
    if (addr < HEAP_BASE || !size || !gpuheap_on()) return;
    uint64_t end = addr + size - HEAP_BASE;
    if (end > HEAP_MAX) end = HEAP_MAX;
    bool lazy = H.lazy && addr >= STDHEAP_BASE;
    pthread_mutex_lock(&H.lock);
    unsigned first = (unsigned)((addr - HEAP_BASE) / CHUNK), last = (unsigned)((end - 1) / CHUNK);
    for (unsigned i = 0; i <= last; i++) {
        if (lazy && i > first && i < last && H.fd[i] < 0) {
            if (!H.lazy_chunk[i]) mprotect((void *)(uintptr_t)(HEAP_BASE + i * CHUNK), CHUNK, PROT_NONE);
            H.lazy_chunk[i] = true;
            continue;
        }
        back_chunk(i);
    }
    if (end > H.end) H.end = (end + CHUNK - 1) / CHUNK * CHUNK;
    __atomic_store_n(&g_gpuheap_gen, H.backed, __ATOMIC_RELEASE);
    __atomic_store_n(&g_gpuheap_end, H.end, __ATOMIC_RELEASE);
    pthread_mutex_unlock(&H.lock);
    LOG("[vk] guest heap: %llu MB of GPU memory from %llx (%llu MB of the window%s)\n", (unsigned long long)H.backed * (CHUNK >> 20),
        (unsigned long long)HEAP_BASE, (unsigned long long)(H.end >> 20), lazy ? ": the rest backed as it's used" : "");
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

// (the crash handler, SIGSEGV at addr) an unbacked chunk's first access: backed, true (the access retries)
bool gpuheap_fault(uint64_t addr) {
    if (!H.lazy || addr < HEAP_BASE || addr - HEAP_BASE >= HEAP_MAX) return false;
    unsigned i = (unsigned)((addr - HEAP_BASE) / CHUNK);
    if (!__atomic_load_n(&H.lazy_chunk[i], __ATOMIC_ACQUIRE) && H.fd[i] < 0) return false;
    pthread_mutex_lock(&H.lock);
    bool ok = true;
    if (H.lazy_chunk[i]) back_chunk(i);
    else ok = H.fd[i] >= 0;  // (backed meanwhile by another thread)
    __atomic_store_n(&g_gpuheap_gen, H.backed, __ATOMIC_RELEASE);
    pthread_mutex_unlock(&H.lock);
    return ok;
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
    pthread_mutex_lock(&H.lock);
    for (unsigned i = 0; i < NCHUNK && n < max; i++)
        if (H.fd[i] >= 0 && !H.handed[i]) {
            int d = dup(H.fd[i]);
            if (d < 0) break;
            H.handed[i] = true, off[n] = i * CHUNK, fds[n++] = d;
        }
    pthread_mutex_unlock(&H.lock);
    return n;
}
uint64_t gpuheap_chunk_size(void) { return CHUNK; }

// After a reboot reset the guest range (fresh anonymous memory over it): the chunks mapped there again, zeroed
// (the game's device keeps its imports: the same memory)
void gpuheap_guest_reset(void) {
    pthread_mutex_lock(&H.lock);
    for (unsigned i = 0; i < NCHUNK; i++)
        if (H.fd[i] >= 0) {
            void *at = (void *)(uintptr_t)(HEAP_BASE + i * CHUNK);
            if (mmap(at, CHUNK, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, H.fd[i], 0) != at) FATAL("guest heap: remap failed\n");
            memset(at, 0, CHUNK);
            H.handed[i] = false;  // (exec.c dropped its imports at the reset: imported again)
        } else if (H.lazy_chunk[i])
            mprotect((void *)(uintptr_t)(HEAP_BASE + i * CHUNK), CHUNK, PROT_NONE);  // (the fresh range: unbacked again)
    pthread_mutex_unlock(&H.lock);
}

#else  // (Windows / macOS: the host memory import, exec.c)
uint64_t g_gpuheap_end, g_gpuheap_gen;
void gpuheap_touch(uint64_t addr, uint64_t n) { (void)addr, (void)n; }
bool gpuheap_fault(uint64_t addr) { (void)addr; return false; }
unsigned gpuheap_backed_mb(void) { return 0; }
uint64_t gpuheap_unbacked(void) { return 0; }
bool gpuheap_on(void) { return false; }
void gpuheap_back(uint64_t addr, uint64_t size) { (void)addr, (void)size; }
uint64_t gpuheap_end(void) { return 0; }
int gpuheap_take(uint64_t *off, int *fds, int max) { (void)off, (void)fds, (void)max; return 0; }
uint64_t gpuheap_chunk_size(void) { return 0; }
void gpuheap_guest_reset(void) {}
#endif
