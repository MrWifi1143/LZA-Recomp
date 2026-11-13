// Output resolution (runtime/nvn/output_scale.c), shared by the Metal and Vulkan backends.
#pragma once
#include <stdbool.h>

typedef struct {
    double sx, sy;             // render-target scale (relative to the game's 1920x1080 sizes)
    double shape;              // window aspect / 16:9 when the camera is widened, else 1
    double vx, vy, vw, vh;     // where the image is drawn in the window (pixels)
} OutScale;

// dw x dh: the window's drawable size in pixels; now: seconds; gpu_ms: GPU time of the last frame (< 0: unknown)
// reshape_allowed: the render targets may take the window's shape (Legends: Arceus with Aspect ratio "Fill the window",
// Settings.ultrawide, and a window wider than 16:9) - the image then fills the window
void output_scale(double dw, double dh, bool reshape_allowed, double now, double gpu_ms, OutScale *o);
double output_scale_dynamic_factor(void);
// Ultrawide ("Fill the window"): the aspect the game's scene camera is given (its own 16:9 widened to the reshaped
// image's: Legends: Arceus, hle/pla_wide.c), 0 while the image is 16:9
extern volatile float g_wide_aspect;
// The game image's own aspect when the game makes it in another shape than 16:9 itself (Legends Z-A's screen size: its
// ultrawide), 0 none: the image is fitted to the window at that shape
extern volatile float g_game_image_aspect;
