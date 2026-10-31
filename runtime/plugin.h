// Runtime plugins: optional shared libraries loaded at startup, for mods that need code rather than files (the VR mod,
// https://github.com/MrWifi1143/LegendsVR). Loaded from LARECOMP_PLUGINS (paths separated by ':') and from
// <data root>/plugins/*.so (minus the ones switched off on the start screen's Mods page: larecomp.ini plugins_off);
// without either nothing changes. A plugin exports
//     int larecomp_plugin_init(const LarecompPluginApi *api, LarecompPluginHooks *hooks);
// which fills in the hooks it wants (the rest stay NULL) and returns 0, or non-zero to stay out.
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <vulkan/vulkan.h>

#define LARECOMP_PLUGIN_ABI 3  // (2: set_pad, set_render_height; 3: ldn_relay_* - a plugin checks abi before using them)

struct Cpu;  // runtime/cpu.h: the guest registers (x[31], sp, pc, v[32]...)

typedef struct {
    uint32_t abi;  // LARECOMP_PLUGIN_ABI
    void (*log)(const char *fmt, ...);
    // A guest function hook: fn runs when the guest calls addr; returning 1 skips the function (fn set x0 itself),
    // 0 runs it as usual. Addresses as the recompiled game has them (main at 0x7000000000).
    void (*register_fnhook)(uint64_t addr, int (*fn)(struct Cpu *c, uint64_t addr));
    // ... and running the original from inside such a hook (the arguments as in c)
    void (*call_original)(struct Cpu *c, uint64_t addr);
    uint64_t (*title_id)(void);  // the running game's program id (0x01001F5010DFA000 Legends: Arceus, ...)
    // The Vulkan queue is not thread-safe: take this around any queue use from a plugin thread (xrEndFrame...)
    void (*queue_lock)(void);
    void (*queue_unlock)(void);
    // The game's final image (hooks.frame) is stored bottom row first (the window draw flips it): 1, top row first: 0
    int (*image_origin_lower_left)(void);
    // ---- ABI 2
    // The plugin's controller (a VR mod's motion controllers): the Switch buttons held (bit = the BTN_* order: A B X Y
    // LSTICK RSTICK L R ZL ZR PLUS MINUS LEFT UP RIGHT DOWN) and both sticks (-32767..32767, y up), added to the player's
    // own controller and keyboard until the next call. Any thread.
    void (*set_pad)(uint64_t buttons, int32_t lx, int32_t ly, int32_t rx, int32_t ry);
    // The render height wanted (lines; the width follows the window's shape), over the Internal resolution setting;
    // 0: the setting again. Takes effect at the next presented frame (the render targets are reallocated).
    void (*set_render_height)(uint32_t lines);
    // ---- ABI 3
    // Local play (Link Trades...) over another network: the relay's functions (runtime/services/ldn.c LdnRelay,
    // contrib/steamrelay), and where a relay replays what a peer sent - this copy's address (127.0.1.N, host order)
    void (*ldn_relay_set)(const void *relay);
    uint32_t (*ldn_relay_local)(void);
} LarecompPluginApi;

typedef struct {
    // Before the instance / device are created: extensions to enable too (returns how many it wrote to out)
    uint32_t (*vk_instance_extensions)(const char **out, uint32_t max);
    // After the instance: the physical device to use (VK_NULL_HANDLE: the runtime's own pick)
    VkPhysicalDevice (*vk_physical_device)(VkInstance instance);
    uint32_t (*vk_device_extensions)(VkPhysicalDevice pd, const char **out, uint32_t max);
    // Everything created (the one queue the runtime uses, family qfam index 0)
    void (*vk_ready)(VkInstance instance, VkPhysicalDevice pd, VkDevice dev, uint32_t qfam, VkQueue queue);
    // Each presented frame: the game's final image (GENERAL layout, w x h, fmt) about to be shown, and a command buffer
    // recording into this frame's submission (a copy out of image belongs here)
    void (*frame)(VkCommandBuffer cb, VkImage image, VkFormat fmt, uint32_t w, uint32_t h);
    // That frame's commands are on the queue (images written by them may be handed on now)
    void (*frame_submitted)(void);
    void (*shutdown)(void);  // (the process is exiting: from exit(), other threads still running)
    // A vertex / fragment stage's constant buffer (game memory at addr, size bytes) read for a draw now: what the frame
    // being recorded draws with (the next hooks.frame shows it). Called for every bind - keep it cheap.
    void (*cbuf_bound)(int stage, uint64_t addr, uint32_t size);
} LarecompPluginHooks;

typedef int (*LarecompPluginInitFn)(const LarecompPluginApi *api, LarecompPluginHooks *hooks);

// ---- runtime side (runtime/plugin.c)
void plugins_load(void);
int plugins_files(char (*names)[64], int max);       // plugins/'s files, on or off (the start screen's list)
bool plugin_opt_in(const char *name);                 // off until switched on (ini plugins_on): the Steam relay
bool plugin_enabled(const char *name);                // as the ini has it (plugins_on / plugins_off)
void plugins_choose(char (*names)[64], const bool *on, int n);  // the start screen's choice -> the ini lists
bool plugin_listed(const char *list, const char *name);  // name in a comma-separated list (ini plugins_off)
int plugins_count(void);
uint32_t plugins_vk_instance_extensions(const char **out, uint32_t max);
VkPhysicalDevice plugins_vk_physical_device(VkInstance instance);
uint32_t plugins_vk_device_extensions(VkPhysicalDevice pd, const char **out, uint32_t max);
void plugins_vk_ready(VkInstance instance, VkPhysicalDevice pd, VkDevice dev, uint32_t qfam, VkQueue queue);
void plugins_frame(VkCommandBuffer cb, VkImage image, VkFormat fmt, uint32_t w, uint32_t h);
void plugins_frame_submitted(void);
void plugins_shutdown(void);
extern volatile int g_plugins_cbuf;  // some plugin wants cbuf_bound
void plugins_cbuf_bound(int stage, uint64_t addr, uint32_t size);
void vk_queue_lock(void);
void vk_queue_unlock(void);
