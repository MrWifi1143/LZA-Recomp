// Scripts' overlay: what a plugin (Legends Lua's draw API) hands the runtime each frame to draw over the game, and the
// input it reads back (runtime/ui/hud.mm, runtime/hle/scriptinput.c; the mod API's hud_* / input_* - modapi.h v6).
// Shared by the runtime and plugins: plain C, fixed sizes.
#pragma once
#include <stdint.h>

// One drawing command. Coordinates are the overlay's pixels (the window's: hud_size), y down.
enum {
    HUD_RECT_FILL = 1,  // x, y, w, h; r rounding
    HUD_RECT = 2,       // outline: x, y, w, h; r rounding; t thickness
    HUD_LINE = 3,       // x, y -> w, h (the second point); t thickness
    HUD_CIRCLE_FILL = 4,// x, y centre; r radius
    HUD_CIRCLE = 5,     // outline; t thickness
    HUD_TRI_FILL = 6,   // x, y / w, h / u0, v0: the three points
    HUD_TEXT = 7,       // x, y; r font size; w wrap width (0: none); align (HUD_ALIGN_*); text = text[a .. a + n]
    HUD_IMAGE = 8,      // x, y, w, h; a the image (hud_image); u0 v0 u1 v1 its part; r rounding; color the tint
    HUD_CLIP = 9,       // x, y, w, h: later commands clipped to it (inside the current clip)
    HUD_UNCLIP = 10,    // back to the clip before the last HUD_CLIP
    HUD_GRAD = 11,      // x, y, w, h filled top colour (color) to bottom (a, as 0xAABBGGRR)
};
enum {
    HUD_ALIGN_LEFT = 0, HUD_ALIGN_CENTER = 1, HUD_ALIGN_RIGHT = 2,  // (x is that edge / the middle)
    HUD_ALIGN_MIDDLE = 4,  // (y is the text's middle, not its top)
    HUD_ALIGN_BOTTOM = 8,
    HUD_TEXT_SHADOW = 16,  // a dark copy behind, 1/16 of the size down-right
};
typedef struct {
    uint8_t op, align;
    uint16_t pad;
    uint32_t color;  // 0xAABBGGRR (ImGui's IM_COL32)
    float x, y, w, h;
    float r, t;
    float u0, v0, u1, v1;
    int32_t a, n;
} HudCmd;

// Script input (keyboard and mouse) since the last call (input_events)
enum { HUD_KEY_DOWN = 1, HUD_KEY_UP = 2, HUD_MOUSE_DOWN = 3, HUD_MOUSE_UP = 4, HUD_WHEEL = 5, HUD_TEXT_INPUT = 6, HUD_MOUSE_MOVE = 7 };
typedef struct {
    uint8_t type, button, repeat, pad;  // (button: 1 left, 2 middle, 3 right, 4 / 5 the side ones)
    uint16_t mods;                      // (SDL_Keymod: shift 0x3, ctrl 0xc0, alt 0x300)
    uint16_t pad2;
    float x, y, wheel;
    char name[32];                      // key: SDL's name ("A", "Return", "Left Shift", "F5"); text input: the UTF-8 typed
} HudInputEvent;
