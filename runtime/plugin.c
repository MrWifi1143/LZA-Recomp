// Runtime plugins (runtime/plugin.h): dlopen'd at startup, hooks called from the Vulkan backend.
#include <dirent.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "plugin.h"
#include "rt.h"

#ifdef _WIN32
#include <windows.h>
#define PLUGIN_EXT ".dll"
static void *dl_open(const char *p) { return (void *)LoadLibraryA(p); }
static void *dl_sym(void *h, const char *n) { return (void *)GetProcAddress((HMODULE)h, n); }
static void dl_close(void *h) { FreeLibrary((HMODULE)h); }
static const char *dl_error(void) { static char b[32]; snprintf(b, sizeof b, "error %lu", GetLastError()); return b; }
#else
#include <dlfcn.h>
#define PLUGIN_EXT ".so"
static void *dl_open(const char *p) { return dlopen(p, RTLD_NOW | RTLD_LOCAL); }
static void *dl_sym(void *h, const char *n) { return dlsym(h, n); }
static void dl_close(void *h) { dlclose(h); }
static const char *dl_error(void) { return dlerror(); }
#endif

typedef int (*FnHook)(struct Cpu *c, uint64_t addr);
void rt_register_fnhook_at(uint64_t addr, FnHook fn);
void rt_call_original(struct Cpu *c, uint64_t addr);
uint64_t game_title_id(void);

#define MAX_PLUGINS 8
static LarecompPluginHooks g_hooks[MAX_PLUGINS];
static int g_n;
static pthread_mutex_t g_queue = PTHREAD_MUTEX_INITIALIZER;

void vk_queue_lock(void) { pthread_mutex_lock(&g_queue); }
void vk_queue_unlock(void) { pthread_mutex_unlock(&g_queue); }

static void plugin_log(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
}

static int origin_lower_left(void) {
    bool renderer_origin_lower_left(void);  // runtime/nvn/renderer.h
    return renderer_origin_lower_left();
}

void input_plugin_pad(uint64_t buttons, int32_t lx, int32_t ly, int32_t rx, int32_t ry);  // runtime/hle/input.c
volatile uint32_t g_plugin_render_height;  // (runtime/nvn/output_scale.c)
static void set_render_height(uint32_t lines) { g_plugin_render_height = lines >= 360 && lines <= 8640 ? lines : 0; }

static const LarecompPluginApi g_api = {
    .abi = LARECOMP_PLUGIN_ABI, .log = plugin_log, .register_fnhook = rt_register_fnhook_at,
    .call_original = rt_call_original, .title_id = game_title_id, .queue_lock = vk_queue_lock, .queue_unlock = vk_queue_unlock,
    .image_origin_lower_left = origin_lower_left, .set_pad = input_plugin_pad, .set_render_height = set_render_height,
};

volatile int g_plugins_cbuf;

static void load_one(const char *path) {
    if (g_n == MAX_PLUGINS) return;
    void *h = dl_open(path);
    if (!h) { LOG("[plugin] %s: %s\n", path, dl_error()); return; }
    LarecompPluginInitFn init = (LarecompPluginInitFn)dl_sym(h, "larecomp_plugin_init");
    if (!init) { LOG("[plugin] %s: no larecomp_plugin_init\n", path); dl_close(h); return; }
    LarecompPluginHooks hk = {0};
    int r = init(&g_api, &hk);
    if (r) { LOG("[plugin] %s stays out (%d)\n", path, r); dl_close(h); return; }
    g_hooks[g_n++] = hk;
    if (hk.cbuf_bound) g_plugins_cbuf = 1;
    LOG("[plugin] loaded %s\n", path);
}

void plugins_load(void) {
    static int done;
    if (done++) return;
    const char *env = getenv("LARECOMP_PLUGINS");
    if (env) {
        char buf[4096];
        snprintf(buf, sizeof buf, "%s", env);
#ifdef _WIN32
        for (char *p = strtok(buf, ";"); p; p = strtok(NULL, ";")) load_one(p);  // (';': Windows paths have a ':')
#else
        for (char *p = strtok(buf, ":"); p; p = strtok(NULL, ":")) load_one(p);
#endif
    }
    DIR *d = opendir("plugins");  // (the working directory is the data root)
    if (d) {
        for (struct dirent *e; (e = readdir(d));) {
            size_t n = strlen(e->d_name);
            if (n > 4 && !strcmp(e->d_name + n - strlen(PLUGIN_EXT), PLUGIN_EXT)) {
                char path[1024];
                snprintf(path, sizeof path, "plugins/%s", e->d_name);
                load_one(path);
            }
        }
        closedir(d);
    }
}

int plugins_count(void) { return g_n; }

uint32_t plugins_vk_instance_extensions(const char **out, uint32_t max) {
    uint32_t n = 0;
    for (int i = 0; i < g_n; i++)
        if (g_hooks[i].vk_instance_extensions) n += g_hooks[i].vk_instance_extensions(out + n, max - n);
    return n;
}

VkPhysicalDevice plugins_vk_physical_device(VkInstance instance) {
    for (int i = 0; i < g_n; i++)
        if (g_hooks[i].vk_physical_device) {
            VkPhysicalDevice pd = g_hooks[i].vk_physical_device(instance);
            if (pd) return pd;
        }
    return VK_NULL_HANDLE;
}

uint32_t plugins_vk_device_extensions(VkPhysicalDevice pd, const char **out, uint32_t max) {
    uint32_t n = 0;
    for (int i = 0; i < g_n; i++)
        if (g_hooks[i].vk_device_extensions) n += g_hooks[i].vk_device_extensions(pd, out + n, max - n);
    return n;
}

void plugins_vk_ready(VkInstance instance, VkPhysicalDevice pd, VkDevice dev, uint32_t qfam, VkQueue queue) {
    for (int i = 0; i < g_n; i++)
        if (g_hooks[i].vk_ready) g_hooks[i].vk_ready(instance, pd, dev, qfam, queue);
}

void plugins_frame(VkCommandBuffer cb, VkImage image, VkFormat fmt, uint32_t w, uint32_t h) {
    for (int i = 0; i < g_n; i++)
        if (g_hooks[i].frame) g_hooks[i].frame(cb, image, fmt, w, h);
}

void plugins_frame_submitted(void) {
    for (int i = 0; i < g_n; i++)
        if (g_hooks[i].frame_submitted) g_hooks[i].frame_submitted();
}

void plugins_cbuf_bound(int stage, uint64_t addr, uint32_t size) {
    for (int i = 0; i < g_n; i++)
        if (g_hooks[i].cbuf_bound) g_hooks[i].cbuf_bound(stage, addr, size);
}

void plugins_shutdown(void) {
    for (int i = 0; i < g_n; i++)
        if (g_hooks[i].shutdown) g_hooks[i].shutdown();
}
