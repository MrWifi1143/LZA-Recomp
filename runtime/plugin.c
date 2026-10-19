// Runtime plugins (runtime/plugin.h): dlopen'd at startup, hooks called from the Vulkan backend.
#include <dirent.h>
#include <dlfcn.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "plugin.h"
#include "rt.h"

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

static const LarecompPluginApi g_api = {
    .abi = LARECOMP_PLUGIN_ABI, .log = plugin_log, .register_fnhook = rt_register_fnhook_at,
    .call_original = rt_call_original, .title_id = game_title_id, .queue_lock = vk_queue_lock, .queue_unlock = vk_queue_unlock,
};

static void load_one(const char *path) {
    if (g_n == MAX_PLUGINS) return;
    void *h = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!h) { LOG("[plugin] %s: %s\n", path, dlerror()); return; }
    LarecompPluginInitFn init = (LarecompPluginInitFn)dlsym(h, "larecomp_plugin_init");
    if (!init) { LOG("[plugin] %s: no larecomp_plugin_init\n", path); dlclose(h); return; }
    LarecompPluginHooks hk = {0};
    int r = init(&g_api, &hk);
    if (r) { LOG("[plugin] %s stays out (%d)\n", path, r); dlclose(h); return; }
    g_hooks[g_n++] = hk;
    LOG("[plugin] loaded %s\n", path);
}

void plugins_load(void) {
    static int done;
    if (done++) return;
    const char *env = getenv("LARECOMP_PLUGINS");
    if (env) {
        char buf[4096];
        snprintf(buf, sizeof buf, "%s", env);
        for (char *p = strtok(buf, ":"); p; p = strtok(NULL, ":")) load_one(p);
    }
    DIR *d = opendir("plugins");  // (the working directory is the data root)
    if (d) {
        for (struct dirent *e; (e = readdir(d));) {
            size_t n = strlen(e->d_name);
            if (n > 3 && !strcmp(e->d_name + n - 3, ".so")) {
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

void plugins_shutdown(void) {
    for (int i = 0; i < g_n; i++)
        if (g_hooks[i].shutdown) g_hooks[i].shutdown();
}
