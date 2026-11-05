// Runtime plugins (runtime/plugin.h): dlopen'd at startup, hooks called from the Vulkan backend.
#include <dirent.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "paths.h"
#include "plugin.h"
#include "rt.h"
#include "settings.h"

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


// A plugin's hook where the runtime has one already (a fix): the plugin's first, then - when it doesn't skip the
// function - the runtime's, so a script never switches a fix off by hooking the same function
FnHook rt_fnhook_get(uint64_t addr);
// Several plugins on one function (the VR mod and a Lua script): each in the order they hooked it, and a plugin's
// call_original from inside its hook runs the next plugin's hook first - the game's function runs once, after all of
// them (it used to be the last plugin's hook only: a Lua script on the VR mod's camera turn switched snap turns off).
#define NCHAIN 256
#define NPLUG 8
static struct { uint64_t addr; FnHook plugin[NPLUG], runtime; int n; } g_chain[NCHAIN];
static int g_nchain;
static __thread int t_entry = -1, t_pos;  // (the chain and the plugin hook running on this thread)
static int chain_from(struct Cpu *c, uint64_t addr, int e, int k) {
    int pe = t_entry, pp = t_pos, r = 0;
    for (int i = k; i < g_chain[e].n && !r; i++) t_entry = e, t_pos = i, r = g_chain[e].plugin[i](c, addr);
    t_entry = pe, t_pos = pp;
    return r ? 1 : g_chain[e].runtime ? g_chain[e].runtime(c, addr) : 0;
}
static int chained(struct Cpu *c, uint64_t addr) {
    for (int i = 0; i < __atomic_load_n(&g_nchain, __ATOMIC_ACQUIRE); i++)
        if (g_chain[i].addr == addr) return chain_from(c, addr, i, 0);
    return 0;
}
// (runtime/fnhook.c: the runtime's own hook on a chained address - registered before or after the plugins')
static void chain_runtime(uint64_t addr, FnHook fn) {
    for (int i = 0; i < g_nchain; i++)
        if (g_chain[i].addr == addr) { g_chain[i].runtime = fn; return; }
}
extern FnHook g_plugin_chain_fn;
extern void (*g_plugin_chain_runtime)(uint64_t addr, FnHook fn);
static void plugin_call_original(struct Cpu *c, uint64_t addr) {
    if (t_entry >= 0 && g_chain[t_entry].addr == addr && chain_from(c, addr, t_entry, t_pos + 1)) return;
    rt_call_original(c, addr);
}
static void plugin_register_fnhook(uint64_t addr, FnHook fn) {
    for (int i = 0; i < g_nchain; i++)
        if (g_chain[i].addr == addr) {
            for (int k = 0; k < g_chain[i].n; k++) if (g_chain[i].plugin[k] == fn) return;
            if (g_chain[i].n < NPLUG) g_chain[i].plugin[g_chain[i].n++] = fn;
            return;
        }
    if (g_nchain == NCHAIN) return;
    g_plugin_chain_fn = chained, g_plugin_chain_runtime = chain_runtime;
    g_chain[g_nchain].addr = addr, g_chain[g_nchain].plugin[0] = fn, g_chain[g_nchain].n = 1, g_chain[g_nchain].runtime = rt_fnhook_get(addr);
    __atomic_store_n(&g_nchain, g_nchain + 1, __ATOMIC_RELEASE);
    rt_register_fnhook_at(addr, chained);
}

uint64_t renderer_present_count(void);
void hwwatch_arm(uint64_t addr, uint64_t len);  // (runtime/hle/hwwatch.c)
void hwwatch_reset(void);
static const LarecompPluginApi g_api = {
    .abi = LARECOMP_PLUGIN_ABI, .log = plugin_log, .register_fnhook = plugin_register_fnhook,
    .call_original = plugin_call_original, .title_id = game_title_id, .queue_lock = vk_queue_lock, .queue_unlock = vk_queue_unlock,
    .image_origin_lower_left = origin_lower_left, .set_pad = input_plugin_pad, .set_render_height = set_render_height,
    .ldn_relay_set = NULL, .ldn_relay_local = NULL,  // (Arceus: no local play service - no relay)
    .guest_call = guest_call, .data_root = rt_data_root, .present_count = renderer_present_count,
    .watch = hwwatch_arm, .watch_reset = hwwatch_reset,
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

// name (a file in plugins/) is in the comma-separated list
bool plugin_listed(const char *list, const char *name) {
    size_t n = strlen(name);
    for (const char *p = list; *p; p += strcspn(p, ",") + (p[strcspn(p, ",")] == ',')) {
        while (*p == ' ') p++;
        if (!strncmp(p, name, n) && (p[n] == ',' || p[n] == 0 || p[n] == ' ')) return true;
    }
    return false;
}

// Plugins that are off until the player switches them on (the start screen's Mods page): ones that change what the
// game does with other people or the PC - the Steam relay (Steam shows "Playing Spacewar"), Legends Lua (runs scripts)
bool plugin_opt_in(const char *name) { return strstr(name, "steamrelay") != NULL || strstr(name, "legendslua") != NULL; }
bool plugin_enabled(const char *name) {
    return plugin_opt_in(name) ? plugin_listed(g_settings.plugins_on, name) : !plugin_listed(g_settings.plugins_off, name);
}

// The plugin files in plugins/ (names only, sorted), on or off: what the start screen lists
int plugins_files(char (*names)[64], int max) {
    int n = 0;
    DIR *d = opendir("plugins");
    if (!d) return 0;
    for (struct dirent *e; (e = readdir(d)) && n < max;) {
        size_t k = strlen(e->d_name);
        if (k > strlen(PLUGIN_EXT) && k < 64 && !strcmp(e->d_name + k - strlen(PLUGIN_EXT), PLUGIN_EXT)) snprintf(names[n++], 64, "%s", e->d_name);
    }
    closedir(d);
    qsort(names, (size_t)n, 64, (int (*)(const void *, const void *))strcmp);
    return n;
}

// The start screen's choice: plugins/ files names[i] on or off -> ini plugins_on / plugins_off (an opt-in one listed in
// plugins_on when on, any other in plugins_off when off; names of files not there kept, should they come back)
void plugins_choose(char (*names)[64], const bool *on, int n) {
    char lists[2][sizeof g_settings.plugins_on], *was[2] = {g_settings.plugins_on, g_settings.plugins_off};
    for (int l = 0; l < 2; l++) {
        lists[l][0] = 0;
        for (const char *p = was[l]; *p; p += strcspn(p, ",") + (p[strcspn(p, ",")] == ',')) {  // (the absent ones)
            char nm[64];
            snprintf(nm, sizeof nm, "%.*s", (int)strcspn(p, ","), p);
            bool here = !nm[0];
            for (int i = 0; i < n && !here; i++) here = !strcmp(names[i], nm);
            if (!here && strlen(lists[l]) + strlen(nm) + 2 < sizeof lists[l]) strcat(lists[l], lists[l][0] ? "," : ""), strcat(lists[l], nm);
        }
    }
    for (int i = 0; i < n; i++) {
        int l = plugin_opt_in(names[i]) ? 0 : 1;
        if ((l == 0) == on[i] && strlen(lists[l]) + strlen(names[i]) + 2 < sizeof lists[l])
            strcat(lists[l], lists[l][0] ? "," : ""), strcat(lists[l], names[i]);
    }
    memcpy(g_settings.plugins_on, lists[0], sizeof lists[0]), memcpy(g_settings.plugins_off, lists[1], sizeof lists[1]);
}

void plugins_shutdown(void);
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
    settings_load();  // (plugins_off; the working directory is the data root)
    DIR *d = opendir("plugins");
    for (struct dirent *e; d && (e = readdir(d));) {  // (ones removed on the start screen while loaded: gone now)
        size_t n = strlen(e->d_name);
        char path[1024];
        snprintf(path, sizeof path, "plugins/%s", e->d_name);
        if (n > 8 && !strcmp(e->d_name + n - 8, ".removed")) remove(path);
    }
    if (d) closedir(d), d = opendir("plugins");
    if (d) {
        for (struct dirent *e; (e = readdir(d));) {
            size_t n = strlen(e->d_name);
            if (n > 4 && !strcmp(e->d_name + n - strlen(PLUGIN_EXT), PLUGIN_EXT)) {
                if (!plugin_enabled(e->d_name)) {  // (switched off on the start screen, or not switched on)
                    LOG("[plugin] plugins/%s is switched off\n", e->d_name);
                    continue;
                }
                char path[1024];
                snprintf(path, sizeof path, "plugins/%s", e->d_name);
                load_one(path);
            }
        }
        closedir(d);
    }
    if (g_n) atexit(plugins_shutdown);  // (hooks.shutdown: Steam must close its connections before the process goes)
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

void plugins_game_start(void) {
    for (int i = 0; i < g_n; i++)
        if (g_hooks[i].game_start) g_hooks[i].game_start();
}

void plugins_game_frame(uint64_t frame) {
    for (int i = 0; i < g_n; i++)
        if (g_hooks[i].game_frame) g_hooks[i].game_frame(frame);
}

void plugins_shutdown(void) {
    for (int i = 0; i < g_n; i++)
        if (g_hooks[i].shutdown) g_hooks[i].shutdown();
}
