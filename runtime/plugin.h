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

#define LARECOMP_PLUGIN_ABI 6  // (2: set_pad, set_render_height; 3: ldn_relay_*; 4: guest_call, data_root, present_count,
                                // hooks.game_frame; 5: watch, watch_reset, hooks.game_start; 6: set_stereo, hooks.frame_eye,
                                // hooks.stereo_cbuf - a plugin checks abi first)

struct Cpu;  // runtime/cpu.h: the guest registers - x[0..30] at byte 0 (8 each), sp at 256, v[0..31] at 512 (16 each)

typedef struct {
    uint32_t abi;  // LARECOMP_PLUGIN_ABI
    void (*log)(const char *fmt, ...);
    // A guest function hook: fn runs when the guest calls addr; returning 1 skips the function (fn set x0 itself),
    // 0 runs it as usual. Addresses as the recompiled game has them (main at 0x7000000000). Where the runtime hooks
    // the function itself (a fix), fn runs first and the runtime's hook after it (unless fn skipped the function).
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
    // ---- ABI 4
    // Inside a hook only (the guest thread's registers): run the game's function at addr with c's x0.. as arguments,
    // as a call from there (x30 kept); its results are in c afterwards
    void (*guest_call)(struct Cpu *c, uint64_t addr);
    const char *(*data_root)(void);   // the game folder (absolute): settings, saves, mods, plugins
    uint64_t (*present_count)(void);  // frames presented so far
    // ---- ABI 5
    // Research: a hardware watchpoint on len (1, 2, 4, 8) bytes at addr, on every thread: each writer (LARECOMP_HWWATCH_RW=1:
    // reader too) is logged once, "[hwwatch]" with the guest function and registers. Up to 4; watch_reset frees them.
    // Linux only (elsewhere: nothing).
    void (*watch)(uint64_t addr, uint64_t len);
    void (*watch_reset)(void);
    // ---- ABI 6
    // Stereo (a VR mod's both eyes of one moment): on, each presented frame is drawn twice - as the game drew it (eye 0),
    // then its command lists run again from the state the frame started with (eye 1), with the constant buffers the
    // draws read then (copies, so the game's later writes don't leak in) and whatever hooks.stereo_cbuf puts in their
    // place. Work that writes game memory (compute, buffer copies, counters, syncs) isn't done again: eye 1 has eye 0's
    // results of it. hooks.frame_eye gets each eye's image. A frame that can't be run again (its lists overwritten,
    // an object destroyed) has eye 0 only. Takes effect at the next frame; costs the renderer's work twice.
    void (*set_stereo)(int on);
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
    // ---- ABI 4
    // Each game frame, before its input is read (the render thread; game memory can be read, not called into)
    void (*game_frame)(uint64_t frame);
    // ---- ABI 5
    // The game is about to start (again, after a reboot to the start screen): the runtime's own hooks are registered,
    // none of the game's code has run yet - where to set hooks (a plugin's chain in front of the runtime's)
    void (*game_start)(void);
    // ---- ABI 6 (stereo: api.set_stereo)
    // Each eye's image of a presented frame, as hooks.frame (which isn't called while stereo is on); eye 1 may not come
    void (*frame_eye)(VkCommandBuffer cb, VkImage image, VkFormat fmt, uint32_t w, uint32_t h, int eye);
    // Running a frame again for eye 1: a vertex / fragment stage's constant buffer (game memory addr, size bytes) as the
    // draw read it the first time (in); write what eye 1's draw reads instead to out (size bytes) and return 1, or 0
    int (*stereo_cbuf)(int stage, uint64_t addr, uint32_t size, const void *in, void *out);
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
void plugins_frame_eye(VkCommandBuffer cb, VkImage image, VkFormat fmt, uint32_t w, uint32_t h, int eye);
int plugins_stereo_cbuf(int stage, uint64_t addr, uint32_t size, const void *in, void *out);
extern volatile int g_plugin_stereo;  // api.set_stereo (the renderer: nvn/vulkan/exec.c vk_stereo_render)
void plugins_game_frame(uint64_t frame);
void plugins_game_start(void);
void plugins_shutdown(void);
extern volatile int g_plugins_cbuf;  // some plugin wants cbuf_bound
void plugins_cbuf_bound(int stage, uint64_t addr, uint32_t size);
void vk_queue_lock(void);
void vk_queue_unlock(void);
