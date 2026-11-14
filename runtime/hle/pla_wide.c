// Ultrawide (Settings.ultrawide, Aspect ratio "Fill the window"): Legends: Arceus' scene camera made as wide as the
// image. With a window wider than 16:9 the render targets take the window's shape (nvn/output_scale.c: g_wide_aspect,
// the image's aspect, 0 while it is 16:9) and the interface stays in a centred 16:9 box (nvn/vulkan/exec.c draw_);
// the camera keeps its vertical field of view and sees as much more to the sides (Hor+).
//
// The render camera's update 0x7002994b70 (x0 = the camera; twice a frame; the one LegendsVR drives) keeps the vertical
// field of view at +0x23c (radians) and the aspect at +0x240; the projection is made from them later in the frame (the
// matrix it holds at +0x2b0 is rebuilt, writing it does nothing). The game's own aspect is 16:9: it is replaced before
// the update and again after it (the update may set it again) - on a camera whose aspect is 16:9 or the one put there
// before (a window resized: the new shape; ultrawide off: 16:9 again); any other shape is the game's and stays. What
// is drawn is chosen from the projection too: nothing at the new sides is left out (looked at turning in the field).
// A VR plugin's shape (plugin.h set_render_shape): the plugin sets the camera itself.
// The update's hook is this one: hle/warp.c's (LARECOMP_WARP / LARECOMP_LIVE) runs from it.
#include <math.h>

#include "../hle.h"
#include "../nvn/output_scale.h"

void rt_register_fnhook_at(uint64_t addr, int (*fn)(Cpu *c, uint64_t addr));
void rt_call_original(Cpu *c, uint64_t addr);
int warp_cam_hook(Cpu *c, uint64_t addr);  // (hle/warp.c)

#define PLA_CAM_UPDATE 0x7002994b70ull
#define CAM_FOV 0x23c
#define CAM_ASPECT 0x240

static float g_put;  // the aspect last put in a camera (0: none)

static bool widen(float *a, float want) {  // (want 0: the game's 16:9)
    bool ours = fabsf(*a - 16.0f / 9.0f) < 0.002f || (g_put > 0 && fabsf(*a - g_put) < 0.002f);
    if (!ours) return false;
    *a = want > 0 ? want : 16.0f / 9.0f;
    return true;
}

static int cam_hook(Cpu *c, uint64_t addr) {
    warp_cam_hook(c, addr);
    extern volatile float g_plugin_render_shape;  // (runtime/plugin.c)
    float want = g_wide_aspect;
    if (g_plugin_render_shape > 0 || (!want && !g_put)) return 0;
    float *cam = (float *)(uintptr_t)c->x[0], *a = cam + CAM_ASPECT / 4;
    if (!widen(a, want)) return 0;
    rt_call_original(c, addr);
    widen(a, want);
    static float logged;
    if (want != logged) {
        if (want) LOG("[wide] scene camera aspect %.4f (16:9 x %.3f; field of view %.1f degrees high)\n", want, want / (16.0f / 9.0f),
                      cam[CAM_FOV / 4] * 57.29578f);
        else LOG("[wide] scene camera aspect 16:9 again\n");
        logged = want;
    }
    if (want) g_put = want;  // (kept after ultrawide ends: every camera that had it gets 16:9 back)
    return 1;
}

void pla_wide_configure(void) { rt_register_fnhook_at(PLA_CAM_UPDATE, cam_hook); }
