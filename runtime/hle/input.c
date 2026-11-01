// Controller input: SDL keyboard/gamepad -> nn::hid Npad state.
//
// Both go through the player's bindings (runtime/hle/bindings.c, defaults there: keyboard WASD left stick (Left Ctrl
// held: walk), arrows d-pad, Space (or Enter) = A, Q (or Backspace) = B, Tab = X, R = Y, Z / C = L / R, mouse right /
// left (or 1 / 3) = ZL / ZR, = / - = Plus / Minus, numpad 8/4/5/6 right stick; controller by position, Switch A = east).
// Mouse: a click in the game window takes it (relative mode; the mouse camera setting): moving it turns the camera
// (the right stick, by its speed), or the virtual gyro while the game aims with the motion sensor; its buttons and
// wheel are keyboard bindings. The menu (Esc) or leaving the window lets it go. With the mouse camera off it is free
// and is a finger on the touch screen (left button held), held only while the game aims.
#include <SDL3/SDL.h>
#include <ctype.h>
#include <math.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#include "../hle.h"
#include "../nvn/nvn.h"
#include "../nvn/renderer.h"
#include "../settings.h"
#include "../input_bind.h"

enum {
    BTN_A = 0, BTN_B, BTN_X, BTN_Y, BTN_LSTICK, BTN_RSTICK, BTN_L, BTN_R, BTN_ZL, BTN_ZR, BTN_PLUS, BTN_MINUS,
    BTN_LEFT, BTN_UP, BTN_RIGHT, BTN_DOWN
};

_Static_assert(BTN_DOWN == BIND_DOWN && BTN_ZL == BIND_ZL, "bindings follow the button bits");

static _Atomic uint64_t key_buttons, pad_buttons;
static _Atomic int32_t key_lx, key_ly, key_rx, key_ry, pad_lx, pad_ly, pad_rx, pad_ry;
static uint8_t keys_down[BIND_KEYCODE_COUNT];  // (keys, then the mouse buttons: BIND_MOUSE_*)
static SDL_Gamepad *pad;
static _Atomic int64_t sampling;

// Host keys / buttons held by a test script (LARECOMP_HOST_INPUT, input_frame): they go through the bindings
static uint8_t test_keys[BIND_KEYCODE_COUNT];
static _Atomic uint32_t test_pad;
static _Atomic int32_t test_lx, test_ly;  // (a resting stick's drift: "lstick" in LARECOMP_HOST_INPUT)
static bool host_test;  // (then a real controller is left out: the player's own pad must not spoil the test)

// The wheel has no "held": a notch holds its binding for WHEEL_MS
#define WHEEL_MS 90
static _Atomic uint64_t wheel_until[2];  // (SDL_GetTicks: up, down)
static int held_key(int sc) {
    if (sc == BIND_WHEEL_UP || sc == BIND_WHEEL_DOWN) return SDL_GetTicks() < wheel_until[sc - BIND_WHEEL_UP];
    return sc > 0 && sc < BIND_KEYCODE_COUNT && (keys_down[sc] || test_keys[sc]);
}

// Keyboard and mouse or controller: the one used last (the button prompts' Auto, runtime/nvn/glyphs.c). Without a
// controller it is always the keyboard.
static _Atomic int last_kbm = 1;
int input_last_was_kbm(void) { return last_kbm || !pad; }

// Through the bindings (runtime/hle/bindings.c: g_bind_key, and the fixed second keys), read on every key event, so
// a rebind applies at once.
static void recompute_keyboard(void) {
    uint64_t b = 0;
    for (int i = 0; i < BIND_PAD_COUNT; i++)
        if (held_key(g_bind_key[i]) || held_key(g_bind_key2[i])) b |= 1ull << i;
    key_buttons = b;
    // diagonals at the same speed (not sqrt 2 faster); Walk (held) at half tilt
    int dx = held_key(g_bind_key[BIND_LS_RIGHT]) - held_key(g_bind_key[BIND_LS_LEFT]);
    int dy = held_key(g_bind_key[BIND_LS_UP]) - held_key(g_bind_key[BIND_LS_DOWN]);
    int32_t m = dx && dy ? 23170 : 32767;
    if (held_key(g_bind_key[BIND_WALK])) m /= 2;
    key_lx = dx * m, key_ly = dy * m;
    key_rx = (held_key(g_bind_key[BIND_RS_RIGHT]) - held_key(g_bind_key[BIND_RS_LEFT])) * 32767;
    key_ry = (held_key(g_bind_key[BIND_RS_UP]) - held_key(g_bind_key[BIND_RS_DOWN])) * 32767;
}

// A controller's stick never rests at exactly 0, and the game reads any value as a push: the Poké Ball aim crept with
// the stick let go. Radial dead zone (g_settings.stick_deadzone, the launcher's "stick dead zone"): inside it the stick
// reads 0; outside, the travel left is stretched back to the full range, so there is no jump at its edge.
static void stick_deadzone(int32_t *x, int32_t *y) {
    double dz = g_settings.stick_deadzone, m = sqrt((double)*x * *x + (double)*y * *y) / 32767.0;
    if (dz <= 0) return;
    if (m <= dz) { *x = *y = 0; return; }
    double k = (m > 1 ? 1 : (m - dz) / (1 - dz)) / m;
    *x = (int32_t)lround(*x * k), *y = (int32_t)lround(*y * k);
}

// Controller buttons (and the triggers as buttons) -> Switch buttons through g_bind_pad; the sticks stay the sticks.
static void recompute_pad(void) {
    uint32_t host = test_pad;
    // Scripted runs ignore the real controller: its state is polled here, not only through events, so the event
    // filter alone (input_user_blocked) let the player's controller drive a test instance (non-deterministic runs).
    static int user_off = -1;
    if (user_off < 0) {
        const char *v = getenv("LARECOMP_NO_USER_INPUT");
        user_off = v ? atoi(v) != 0 : getenv("LARECOMP_INPUT") != NULL || getenv("LARECOMP_REPLAY") != NULL;
    }
    if (pad && !host_test && !user_off) {
        for (int i = 0; i < SDL_GAMEPAD_BUTTON_COUNT; i++)
            if (SDL_GetGamepadButton(pad, (SDL_GamepadButton)i)) host |= 1u << i;
        if (SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) > 16000) host |= 1u << PADB_LTRIGGER;
        if (SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) > 16000) host |= 1u << PADB_RTRIGGER;
    }
    uint64_t b = 0;
    for (int i = 0; i < BIND_PAD_COUNT; i++)
        if (g_bind_pad[i] >= 0 && g_bind_pad[i] < PADB_HOST_COUNT && (host >> g_bind_pad[i] & 1)) b |= 1ull << i;
    pad_buttons = b;
    if (!pad || host_test || user_off) {  // (a test's simulated stick drift goes through the dead zone too)
        int32_t tx = test_lx, ty = test_ly;
        stick_deadzone(&tx, &ty);
        pad_lx = tx, pad_ly = ty, pad_rx = pad_ry = 0;
        return;
    }
    int32_t lx = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTX), ly = -SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTY);
    int32_t rx = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHTX), ry = -SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHTY);
    stick_deadzone(&lx, &ly);
    stick_deadzone(&rx, &ry);
    pad_lx = lx, pad_ly = ly, pad_rx = rx, pad_ry = ry;
}

// The mouse: held by the game window (relative mode) or free. Held, it turns the camera (the right stick, by its
// speed: mouse_stick) or, while the game aims with the motion sensor, the virtual gyro; its buttons and wheel are
// keyboard bindings (BIND_MOUSE_*). With the mouse camera on (setting), a click in the window takes the mouse;
// losing focus or opening the menu (Esc) lets it go.
static SDL_Window *mouse_win;
static _Atomic int gyro_aiming;  // the game has the motion sensor started (aiming): the mouse turns the gyro
static _Atomic int64_t mouse_acc_x, mouse_acc_y;  // motion not yet applied to the virtual gyro, in 1/1000 pixel
static _Atomic int64_t cam_acc_x, cam_acc_y;      // motion not yet read as the camera stick, in 1/1000 pixel
static _Atomic int touch_down;
static void mouse_release(void) {
    if (mouse_win) SDL_SetWindowRelativeMouseMode(mouse_win, false);
    mouse_win = NULL;
    for (int b = BIND_MOUSE_LEFT; b <= BIND_MOUSE_X2; b++) keys_down[b] = 0;
    cam_acc_x = cam_acc_y = 0;
    touch_down = 0;
    recompute_keyboard();
}

// The game window's focus (its events here), for Settings.background: muted (services/audio.c) or paused at the next
// present (nvn/objects.c) while another window has it. LARECOMP_LIVE's dir file "unfocused" (0 / 1) stands in (tests).
static _Atomic int g_unfocused;
static bool background_now(void) {
    static const char *live = (const char *)1;
    if (live == (const char *)1) live = getenv("LARECOMP_LIVE");
    if (live) {
        static uint64_t last;
        uint64_t now = SDL_GetTicks();
        if (now - last > 200) {
            last = now;
            char p[1100];
            snprintf(p, sizeof p, "%s/unfocused", live);
            FILE *f = fopen(p, "r");
            int v = 0;
            if (f) { if (fscanf(f, "%d", &v) != 1) v = 0; fclose(f); remove(p); atomic_store(&g_unfocused, v); }
        }
    }
    return atomic_load(&g_unfocused) != 0;
}
bool input_background_muted(void) { return g_settings.background >= BG_MUTE && background_now(); }
bool input_background_paused(void) { return g_settings.background == BG_PAUSE && background_now(); }

// Mouse look (input_mouse_camera_take): a game hook asks each frame for the held mouse's motion as camera angles and
// adds them to the camera's own (the game's camera hook: hle/za_fixes.c / hle/fps60.c) - while it does, the mouse isn't the
// camera stick (mouse_stick).
static _Atomic uint64_t mouse_look_ns;  // when it last asked
bool input_mouse_camera_take(float *dyaw, float *dpitch) {
    if (!mouse_win || gyro_aiming) return false;
    double ax = (double)atomic_exchange(&cam_acc_x, 0) / 1000, ay = (double)atomic_exchange(&cam_acc_y, 0) / 1000;
    if (g_settings.mouse_invert_x) ax = -ax;
    if (g_settings.mouse_invert_y) ay = -ay;
    double k = g_settings.mouse_sens * M_PI / 180;  // (the sensitivity: degrees per pixel, as the gyro's)
    *dyaw = (float)(-ax * k), *dpitch = (float)(ay * k);  // (right: the camera turns right; up: it looks up, as the stick)
    atomic_store(&mouse_look_ns, SDL_GetTicksNS());
    return true;
}

// The camera stick from the held mouse: its speed since the last read (smoothed over ~35 ms: the game reads the
// controller several times a frame), full tilt at 600 px/s at the default sensitivity 0.10. Not while the game takes
// the mouse as angles (mouse look, above).
static void mouse_stick(int32_t *rx, int32_t *ry) {
    static uint64_t last;
    static double vx, vy;
    uint64_t now = SDL_GetTicksNS();
    double dt = last ? (double)(now - last) / 1e9 : 0;
    last = now;
    if (now - atomic_load(&mouse_look_ns) < 100000000ull) { vx = vy = 0; return; }
    double ax = (double)atomic_exchange(&cam_acc_x, 0) / 1000, ay = (double)atomic_exchange(&cam_acc_y, 0) / 1000;
    if (g_settings.mouse_invert_x) ax = -ax;
    if (g_settings.mouse_invert_y) ay = -ay;
    if (dt <= 0 || dt > 0.25 || !mouse_win) { vx = vy = 0; return; }
    double a = dt / 0.035 > 1 ? 1 : dt / 0.035;
    vx += (ax / dt - vx) * a, vy += (ay / dt - vy) * a;
    double k = g_settings.mouse_sens / 60.0 * 32767;
    double sx = vx * k, sy = -vy * k;  // (the mouse moved up: the stick up)
    *rx += (int32_t)(sx > 32767 ? 32767 : sx < -32767 ? -32767 : sx);
    *ry += (int32_t)(sy > 32767 ? 32767 : sy < -32767 ? -32767 : sy);
}

// Touch screen: the free mouse over the game image. The game is told it is docked (runtime/services/am.c: a 1920 x
// 1080 display, wider with LARECOMP_DISPLAY_W) and takes touches in that display's pixels - not the handheld panel's
// 1280 x 720: in those, the berry button sat 2/3 of the way to where it was drawn and never opened.
// g_present_rect: where the image is drawn in the window, in pixels (runtime/nvn/vulkan/device.c).
double g_present_rect[4] = {0, 0, 1280, 720};
static double touch_w(void) {
    static double w = 0;
    if (!w) w = getenv("LARECOMP_DISPLAY_W") ? atoi(getenv("LARECOMP_DISPLAY_W")) : 1920;
    return w > 0 ? w : 1920;
}
#define TOUCH_H 1080.0
static _Atomic int32_t touch_x = 960, touch_y = 540;
// The free pointer in a menu (runtime/hle/menu_mouse.c): how often it moved, the clicks and wheel notches not yet taken
static _Atomic uint32_t ptr_moves;
static _Atomic int ptr_clicks, ptr_rclicks, ptr_wheel;
bool menu_mouse_active(void);
static void touch_at(SDL_Window *w, float mx, float my) {
    float d = w ? SDL_GetWindowPixelDensity(w) : 1;
    if (d <= 0) d = 1;
    double r0 = g_present_rect[0], r1 = g_present_rect[1], r2 = g_present_rect[2], r3 = g_present_rect[3];
    if (r2 <= 0 || r3 <= 0) return;
    double x = (mx * d - r0) / r2 * touch_w(), y = (my * d - r1) / r3 * TOUCH_H;
    touch_x = (int32_t)(x < 0 ? 0 : x > touch_w() - 1 ? touch_w() - 1 : x);
    touch_y = (int32_t)(y < 0 ? 0 : y > TOUCH_H - 1 ? TOUCH_H - 1 : y);
}

bool input_menu_pointer(int32_t *x, int32_t *y, uint32_t *moves, int *clicks, int *rclicks, int *wheel) {
    *x = touch_x, *y = touch_y, *moves = ptr_moves;
    *clicks = atomic_exchange(&ptr_clicks, 0), *rclicks = atomic_exchange(&ptr_rclicks, 0), *wheel = atomic_exchange(&ptr_wheel, 0);
    return !mouse_win;
}

// Forget held keys (the menu took focus) and let the mouse go; the gamepad is re-read on its next event.
void input_reset(void) {
    memset(keys_down, 0, sizeof keys_down);
    recompute_keyboard();
    mouse_release();
}

volatile int g_debug_frame_request;  // F9: dump the next frame's passes (build/pass_*.png) + draw log
// F12, or a controller's capture / share button (SDL's MISC1): a screenshot of the game image at the next present
// (runtime/nvn/vulkan/device.c), into <data folder>/screenshots/
volatile int g_screenshot_request;

// Events a test pushes (LARECOMP_HOST_INPUT type: / sdlkey: / sdlpad:) carry this device id: they go the whole way, menu first.
#define TEST_DEVICE 0x7E57u

// Research (LARECOMP_LIVE's "mouse" file, runtime/hle/menu_mouse.c): "at <x> <y>" (the game image, 1920 x 1080) moves
// the pointer there, "move <dx> <dy>" moves it by that, "key <name>" presses a key, "click" / "rclick" press and let go, "wheelup" /
// "wheeldown" turn the wheel - as SDL events.
void input_live_mouse(const char *cmd) {
    int nw = 0;
    SDL_Window **ws = SDL_GetWindows(&nw);
    SDL_Window *w = ws && nw > 0 ? ws[0] : NULL;
    SDL_free(ws);
    if (!w) return;
    SDL_Event e;
    memset(&e, 0, sizeof e);
    float gx, gy, d = SDL_GetWindowPixelDensity(w);
    if (d <= 0) d = 1;
    char kname[64];
    if (sscanf(cmd, "key %63[^\n]", kname) == 1) {  // ("key <SDL key name>": pressed and let go)
        for (int down = 1; down >= 0; down--) {
            e.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP, e.key.which = TEST_DEVICE, e.key.windowID = SDL_GetWindowID(w);
            e.key.scancode = SDL_GetScancodeFromName(kname), e.key.down = down;
            SDL_PushEvent(&e);
        }
    } else if (sscanf(cmd, "move %f %f", &gx, &gy) == 2) {  // ("move <dx> <dy>": relative, as a held mouse turns)
        e.type = SDL_EVENT_MOUSE_MOTION, e.motion.which = TEST_DEVICE, e.motion.windowID = SDL_GetWindowID(w);
        SDL_GetMouseState(&e.motion.x, &e.motion.y);
        e.motion.xrel = gx, e.motion.yrel = gy;
        SDL_PushEvent(&e);
    } else if (sscanf(cmd, "at %f %f", &gx, &gy) == 2) {
        e.type = SDL_EVENT_MOUSE_MOTION, e.motion.which = TEST_DEVICE, e.motion.windowID = SDL_GetWindowID(w);
        e.motion.x = (float)((gx / touch_w() * g_present_rect[2] + g_present_rect[0]) / d);
        e.motion.y = (float)((gy / TOUCH_H * g_present_rect[3] + g_present_rect[1]) / d);
        e.motion.xrel = 1;
        SDL_PushEvent(&e);
    } else if (!strncmp(cmd, "click", 5) || !strncmp(cmd, "rclick", 6)) {
        float mx, my;
        SDL_GetMouseState(&mx, &my);
        for (int down = 1; down >= 0; down--) {
            e.type = down ? SDL_EVENT_MOUSE_BUTTON_DOWN : SDL_EVENT_MOUSE_BUTTON_UP;
            e.button.which = TEST_DEVICE, e.button.windowID = SDL_GetWindowID(w), e.button.down = down;
            e.button.button = cmd[0] == 'r' ? SDL_BUTTON_RIGHT : SDL_BUTTON_LEFT;
            e.button.x = (float)((touch_x / touch_w() * g_present_rect[2] + g_present_rect[0]) / d);
            e.button.y = (float)((touch_y / TOUCH_H * g_present_rect[3] + g_present_rect[1]) / d);
            SDL_PushEvent(&e);
        }
    } else if (!strncmp(cmd, "wheel", 5)) {
        e.type = SDL_EVENT_MOUSE_WHEEL, e.wheel.which = TEST_DEVICE, e.wheel.windowID = SDL_GetWindowID(w);
        e.wheel.y = strstr(cmd, "up") ? 1 : -1;
        SDL_PushEvent(&e);
    }
}
static bool test_event(const SDL_Event *e) {
    switch (e->type) {
    case SDL_EVENT_KEY_DOWN: case SDL_EVENT_KEY_UP: return e->key.which == TEST_DEVICE;
    case SDL_EVENT_TEXT_INPUT: return e->text.windowID == TEST_DEVICE;
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN: case SDL_EVENT_GAMEPAD_BUTTON_UP: return e->gbutton.which == TEST_DEVICE;
    case SDL_EVENT_MOUSE_BUTTON_DOWN: case SDL_EVENT_MOUSE_BUTTON_UP: return e->button.which == TEST_DEVICE;
    case SDL_EVENT_MOUSE_MOTION: return e->motion.which == TEST_DEVICE;
    case SDL_EVENT_MOUSE_WHEEL: return e->wheel.which == TEST_DEVICE;
    default: return false;
    }
}

// A controller SDL has no mapping for is only a joystick to it, and the game reads gamepads only. gamecontrollerdb.txt
// in the data root holds the player's own mappings, in SDL's format (lines for other platforms are skipped, so
// SDL_GameControllerDB's file works as is; LAS Recomp 8f28a0e). Called right after SDL_Init: a pad already connected
// becomes a gamepad then (SDL sends GAMEPAD_ADDED).
void input_load_user_mappings(void) {
    if (!SDL_GetPathInfo("gamecontrollerdb.txt", NULL)) return;
    int n = SDL_AddGamepadMappingsFromFile("gamecontrollerdb.txt");
    if (n < 0) LOG("[input] gamecontrollerdb.txt: %s\n", SDL_GetError());
    else LOG("[input] gamecontrollerdb.txt: %d controller mapping(s) added\n", n);
}

// Scripted test runs ignore the user's own input (a key or stick touched while a test drives Link spoils it):
// LARECOMP_NO_USER_INPUT=1, on by default whenever LARECOMP_INPUT / LARECOMP_DOORJUMP_TEST / LARECOMP_PLACE is set
// (or LARECOMP_HOST_INPUT / LARECOMP_BIND_TEST; LARECOMP_NO_USER_INPUT=0 lets it through again).
bool input_user_blocked(const SDL_Event *e) {
    static int on = -1;
    if (on < 0) {
        const char *v = getenv("LARECOMP_NO_USER_INPUT");
        on = v ? atoi(v) != 0 : (getenv("LARECOMP_INPUT") || getenv("LARECOMP_REPLAY") || getenv("LARECOMP_DOORJUMP_TEST") || getenv("LARECOMP_PLACE") ||
                                 getenv("LARECOMP_HOST_INPUT") || getenv("LARECOMP_BIND_TEST"));
        if (on) LOG("[input] scripted run: keyboard, mouse and controllers ignored\n");
    }
    if (!on) return false;
    if (test_event(e)) return false;  // (LARECOMP_HOST_INPUT type: / sdlkey: / sdlpad: - the test's own presses)
    switch (e->type) {
    case SDL_EVENT_KEY_DOWN: case SDL_EVENT_KEY_UP: case SDL_EVENT_TEXT_INPUT: case SDL_EVENT_TEXT_EDITING:
    case SDL_EVENT_MOUSE_MOTION: case SDL_EVENT_MOUSE_BUTTON_DOWN: case SDL_EVENT_MOUSE_BUTTON_UP:
    case SDL_EVENT_MOUSE_WHEEL:
    case SDL_EVENT_GAMEPAD_ADDED: case SDL_EVENT_GAMEPAD_AXIS_MOTION: case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
    case SDL_EVENT_GAMEPAD_BUTTON_UP: case SDL_EVENT_JOYSTICK_ADDED: case SDL_EVENT_JOYSTICK_AXIS_MOTION:
    case SDL_EVENT_JOYSTICK_BUTTON_DOWN: case SDL_EVENT_JOYSTICK_BUTTON_UP:
        return true;
    default:
        return false;
    }
}

// Controller family for the button prompts (runtime/nvn/glyphs.c): 0 Nintendo / none, 1 Xbox, 2 PlayStation.
// By what the controller's bottom face button says (SDL's label for it): A = an Xbox layout (Xbox pads, the Steam
// Controller / Deck, 8BitDo and other XInput-style pads), Cross = PlayStation, B = Nintendo. A pad SDL has no label
// for is laid out as SDL's own standard gamepad, the Xbox one. LARECOMP_PAD_TYPE=xbox|playstation|nintendo pretends
// one is attached (tests).
static _Atomic int pad_family;
int input_family_of_label(int south_label) {
    return south_label == SDL_GAMEPAD_BUTTON_LABEL_CROSS ? 2 : south_label == SDL_GAMEPAD_BUTTON_LABEL_B ? 0 : 1;
}
static void note_family(void) {
    int f = pad ? input_family_of_label(SDL_GetGamepadButtonLabel(pad, SDL_GAMEPAD_BUTTON_SOUTH)) : 0;
    static SDL_Gamepad *seen;
    if (pad && pad != seen) last_kbm = 0;  // (a controller connected: the prompts are its own until a key is pressed)
    seen = pad;
    if (f != pad_family) LOG("[input] button prompts: %s controller\n", f == 1 ? "Xbox layout" : f == 2 ? "PlayStation" : "Nintendo");
    pad_family = f;
}
int input_pad_family(void) {
    static int env = -2;
    if (env == -2) {
        const char *e = getenv("LARECOMP_PAD_TYPE");
        env = !e ? -1 : !strcmp(e, "xbox") ? 1 : !strcmp(e, "playstation") || !strcmp(e, "ps") ? 2 : 0;
    }
    return env >= 0 ? env : pad_family;
}

// Every connected controller is opened (SDL reports only an open one's buttons) and the game reads the one used last:
// the first one SDL lists isn't always the player's - a virtual pad (Steam Input, DS4Windows / ViGEm, vJoy), a wheel or
// a second receiver can come first, and an Xbox controller then did nothing in the game.
static SDL_Gamepad *pads[8];
static void pad_use(SDL_JoystickID id) {
    SDL_Gamepad *g = SDL_GetGamepadFromID(id);
    if (!g || g == pad) return;
    pad = g;
    LOG("[input] gamepad: %s\n", SDL_GetGamepadName(pad));
    note_family();
}
static int pad_slot(SDL_JoystickID id) {
    for (int i = 0; i < 8; i++) if (pads[i] && SDL_GetGamepadID(pads[i]) == id) return i;
    return -1;
}
static void pad_open(SDL_JoystickID id) {
    if (pad_slot(id) >= 0) return;
    SDL_Gamepad *g = SDL_OpenGamepad(id);  // (opened again when the start screen has it open too: SDL counts the opens)
    if (!g) return;
    for (int i = 0; i < 8; i++) if (!pads[i]) { pads[i] = g; break; }
    if (!pad) pad_use(id);
    else LOG("[input] gamepad connected: %s (used once a button is pressed on it)\n", SDL_GetGamepadName(g));
}
static void pad_close(SDL_JoystickID id) {
    int s = pad_slot(id);
    if (s < 0) return;
    SDL_Gamepad *g = pads[s];
    pads[s] = NULL;
    SDL_CloseGamepad(g);
    if (pad != g) return;
    pad = NULL;
    for (int i = 0; i < 8 && !pad; i++) if (pads[i]) pad_use(SDL_GetGamepadID(pads[i]));
    note_family();
}

void input_handle_event(const SDL_Event *e) {
    static bool scanned;  // pads present at startup: their ADDED event may have gone to the start screen's loop
    if (!scanned) {
        scanned = true;
        int n = 0;
        SDL_JoystickID *ids = SDL_GetGamepads(&n);
        for (int i = 0; ids && i < n; i++) pad_open(ids[i]);
        SDL_free(ids);
    }
    switch (e->type) {
    case SDL_EVENT_KEY_DOWN: case SDL_EVENT_KEY_UP:
        if (e->type == SDL_EVENT_KEY_DOWN && e->key.scancode == SDL_SCANCODE_F9 && !e->key.repeat) g_debug_frame_request = 1;
        if (e->type == SDL_EVENT_KEY_DOWN && e->key.scancode == SDL_SCANCODE_F12 && !e->key.repeat) g_screenshot_request = 1;
        // research (broken map after a capture): F5 marks all guest memory written (every texture re-checks its data), F6 reads
        // all vertex and index data in place from now on
        if (e->type == SDL_EVENT_KEY_DOWN && !e->key.repeat && getenv("LARECOMP_HEAL_KEYS")) {
            extern volatile int g_heal_tex, g_heal_vtx;
            if (e->key.scancode == SDL_SCANCODE_F5) { g_heal_tex = 1; LOG("[heal] F5: textures re-checked\n"); }
            if (e->key.scancode == SDL_SCANCODE_F6) { g_heal_vtx = 1; LOG("[heal] F6: vertex data in place\n"); }
        }
        if (e->key.scancode < SDL_SCANCODE_COUNT) keys_down[e->key.scancode] = e->type == SDL_EVENT_KEY_DOWN;
        if (e->type == SDL_EVENT_KEY_DOWN && !e->key.repeat) last_kbm = 1;
        recompute_keyboard();
        break;
    case SDL_EVENT_MOUSE_BUTTON_DOWN: case SDL_EVENT_MOUSE_BUTTON_UP: case SDL_EVENT_MOUSE_MOTION: case SDL_EVENT_MOUSE_WHEEL: {
        bool motion = e->type == SDL_EVENT_MOUSE_MOTION, wheel = e->type == SDL_EVENT_MOUSE_WHEEL;
        SDL_Window *w = SDL_GetWindowFromID(motion ? e->motion.windowID : wheel ? e->wheel.windowID : e->button.windowID);
        bool cam = g_settings.mouse_camera, down = e->type == SDL_EVENT_MOUSE_BUTTON_DOWN;
        bool menu = menu_mouse_active();  // (a menu is up: the pointer is for it - a held mouse is let go)
        // aiming takes the mouse (the gyro); with the mouse camera a click does too (and is only that: no press)
        if (!mouse_win && w && !menu && (gyro_aiming || (cam && down))) {
            mouse_win = w;
            SDL_SetWindowRelativeMouseMode(w, true);
            touch_down = 0;
            last_kbm = 1;
            if (down && !gyro_aiming) break;
        } else if (mouse_win && (menu || (!gyro_aiming && !cam))) {
            mouse_release();
        }
        if (motion) {
            if (mouse_win) {
                if (gyro_aiming) mouse_acc_x += (int64_t)(e->motion.xrel * 1000), mouse_acc_y += (int64_t)(e->motion.yrel * 1000);
                else cam_acc_x += (int64_t)(e->motion.xrel * 1000), cam_acc_y += (int64_t)(e->motion.yrel * 1000);
                if (e->motion.xrel || e->motion.yrel) last_kbm = 1;
            } else {
                touch_at(w, e->motion.x, e->motion.y);
                ptr_moves++;
            }
            break;
        }
        if (wheel && !mouse_win && menu) {  // (a menu's list: up / down)
            if (e->wheel.y) ptr_wheel += e->wheel.y > 0 ? 1 : -1;
            break;
        }
        if (wheel) {
            if (e->wheel.y) wheel_until[e->wheel.y > 0 ? 0 : 1] = SDL_GetTicks() + WHEEL_MS, last_kbm = 1;
            recompute_keyboard();
            break;
        }
        // buttons: keyboard bindings while the window holds the mouse; free, the left one is a finger on the touch screen
        uint8_t b = e->button.button;
        if (!mouse_win && menu && (b == SDL_BUTTON_LEFT || b == SDL_BUTTON_RIGHT)) {  // (a menu's button / back)
            touch_at(w, e->button.x, e->button.y);
            if (down) b == SDL_BUTTON_LEFT ? ptr_clicks++ : ptr_rclicks++;
            break;
        }
        if (!mouse_win && b == SDL_BUTTON_LEFT) { touch_at(w, e->button.x, e->button.y); touch_down = down; break; }
        if (b >= 1 && b <= 5) keys_down[BIND_MOUSE_BASE + b] = down;
        if (down) last_kbm = 1;
        recompute_keyboard();
        break;
    }
    case SDL_EVENT_WINDOW_FOCUS_LOST:
        mouse_release();
        atomic_store(&g_unfocused, 1);
        break;
    case SDL_EVENT_WINDOW_FOCUS_GAINED: atomic_store(&g_unfocused, 0); break;
    case SDL_EVENT_GAMEPAD_ADDED:
        pad_open(e->gdevice.which);
        break;
    case SDL_EVENT_GAMEPAD_REMOVED:
        pad_close(e->gdevice.which);
        recompute_pad();
        break;
    case SDL_EVENT_GAMEPAD_AXIS_MOTION: case SDL_EVENT_GAMEPAD_BUTTON_DOWN: case SDL_EVENT_GAMEPAD_BUTTON_UP: {
        bool used = e->type == SDL_EVENT_GAMEPAD_BUTTON_DOWN || (e->type == SDL_EVENT_GAMEPAD_AXIS_MOTION && abs(e->gaxis.value) > 20000);
        SDL_JoystickID from = e->type == SDL_EVENT_GAMEPAD_AXIS_MOTION ? e->gaxis.which : e->gbutton.which;
        if (!pad || SDL_GetGamepadID(pad) != from) {
            if (pad && !used) break;  // (another controller's drift or release: not the one in use)
            pad_open(from);
            pad_use(from);
        }
        if (used) last_kbm = 0;
        if (e->type == SDL_EVENT_GAMEPAD_BUTTON_DOWN && e->gbutton.button == SDL_GAMEPAD_BUTTON_MISC1) g_screenshot_request = 1;
        recompute_pad();
        break;
    }
    }
}

// Scripted input for testing: LARECOMP_INPUT="frame:BUTTON:frames,..." (BUTTON = A,B,X,Y,L,R,ZL,ZR,PLUS,
// MINUS,LEFT,UP,RIGHT,DOWN, or STICK_UP/DOWN/LEFT/RIGHT, RSTICK_UP/DOWN/LEFT/RIGHT, STICK_<degrees>: full tilt at
// that angle, 0 = right, 90 = up).
static char g_replay_text[8][96];
static int g_nreplay_text, g_replay_text_next;
static _Atomic uint64_t script_buttons;
static _Atomic int32_t script_lx, script_ly, script_rx, script_ry;
// A plugin's controller (plugin.h set_pad: a VR mod's motion controllers), added to the player's own
static _Atomic uint64_t plugin_buttons;
static _Atomic int32_t plugin_lx, plugin_ly, plugin_rx, plugin_ry;
void input_plugin_pad(uint64_t buttons, int32_t lx, int32_t ly, int32_t rx, int32_t ry) {
    plugin_buttons = buttons, plugin_lx = lx, plugin_ly = ly, plugin_rx = rx, plugin_ry = ry;
}

// Tests of the bindings, through them like a real player:
// LARECOMP_HOST_INPUT="frame:key:<SDL key name>:frames,frame:pad:<button>:frames,..." holds a host key / controller
//   button (names as in larecomp.ini's bind_* lines: "Z", "Keypad 0", "south", "ltrigger"...);
//   "frame:lstick:<x>/<y>:frames" holds the controller's left stick there (raw SDL units, y up: a drifting stick).
// "frame:type:<text>" types <text> as real key events (down, text, up per character), through the overlay menu / name
//   entry first like a player's typing; "{Escape}" (any SDL key name) presses that key. No ',' or ':' in <text>.
// "frame:sdlpad:<button>:frames" / "frame:sdlkey:<key>:frames" hold a controller button / key as real events (down
//   at frame, up `frames` later): they reach menus and the on-screen keyboard first, as a player's would.
// "frame:sdlmouse:left|middle|right|x1|x2|wheelup|wheeldown:frames" presses a mouse button (the wheel: one notch),
// "frame:sdlmove:<dx>/<dy>:frames" moves the mouse by that much every frame, as real events in the game window.
// "frame:touch:<x>/<y>:frames" puts a finger on the touch screen there (1920 x 1080) for that long.
// "frame:shot:<name>" saves the window (game + menus) to build/<name>.png.
// LARECOMP_BIND_TEST="frame:pad|key:<input>:<host>,..." starts a rebind capture of <input> (bind_* name: a, zl,
//   ls_up...) at that frame (the Controls tab shows it waiting) and presses <host> 90 frames later ("esc" cancels,
//   "wait" lets it time out); "frame:reset:pad|key" resets to the defaults.
static void push_key(SDL_Scancode sc, SDL_Keymod mod, bool down) {
    SDL_Event e;
    memset(&e, 0, sizeof e);
    e.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
    e.key.which = TEST_DEVICE, e.key.scancode = sc, e.key.mod = mod, e.key.down = down;
    e.key.key = SDL_GetKeyFromScancode(sc, mod, false);
    SDL_PushEvent(&e);
}
static void test_push(uint64_t frame, unsigned long start, unsigned long len, const char *dev, const char *arg) {
    SDL_Event e;
    memset(&e, 0, sizeof e);
    if (!strcmp(dev, "sdlkey")) {
        if (frame == start || frame == start + len) push_key(SDL_GetScancodeFromName(arg), SDL_KMOD_NONE, frame == start);
        return;
    }
    if (!strcmp(dev, "touch")) {  // ("frame:touch:<x>/<y>:frames": a finger on the touch screen there, 1920 x 1080)
        float tx = 0, ty = 0;
        if (sscanf(arg, "%f/%f", &tx, &ty) != 2) return;
        if (frame == start) touch_x = (int32_t)tx, touch_y = (int32_t)ty, touch_down = 1;
        if (frame == start + len) touch_down = 0;
        return;
    }
    if (!strcmp(dev, "sdlmouse") || !strcmp(dev, "sdlmove")) {
        int nw = 0;
        SDL_Window **ws = SDL_GetWindows(&nw);
        SDL_WindowID wid = ws && nw > 0 ? SDL_GetWindowID(ws[0]) : 0;
        SDL_free(ws);
        if (!strcmp(dev, "sdlmove")) {
            float dx = 0, dy = 0;
            if (frame < start || frame >= start + len || sscanf(arg, "%f/%f", &dx, &dy) != 2) return;
            e.type = SDL_EVENT_MOUSE_MOTION, e.motion.which = TEST_DEVICE, e.motion.windowID = wid;
            e.motion.xrel = dx, e.motion.yrel = dy;
            SDL_PushEvent(&e);
            return;
        }
        static const char *const names[] = {"left", "middle", "right", "x1", "x2"};
        if (!strcmp(arg, "wheelup") || !strcmp(arg, "wheeldown")) {
            if (frame != start) return;
            e.type = SDL_EVENT_MOUSE_WHEEL, e.wheel.which = TEST_DEVICE, e.wheel.windowID = wid;
            e.wheel.y = !strcmp(arg, "wheelup") ? 1 : -1;
            SDL_PushEvent(&e);
            return;
        }
        for (int i = 0; i < 5; i++) {
            if (strcmp(arg, names[i]) || (frame != start && frame != start + len)) continue;
            e.type = frame == start ? SDL_EVENT_MOUSE_BUTTON_DOWN : SDL_EVENT_MOUSE_BUTTON_UP;
            e.button.which = TEST_DEVICE, e.button.windowID = wid, e.button.button = (Uint8)(i + 1), e.button.down = frame == start;
            SDL_PushEvent(&e);
        }
        return;
    }
    if (!strcmp(dev, "sdlpad")) {
        int b = bind_pad_from_name(arg);
        if (b < 0 || b >= SDL_GAMEPAD_BUTTON_COUNT || (frame != start && frame != start + len)) return;
        e.type = frame == start ? SDL_EVENT_GAMEPAD_BUTTON_DOWN : SDL_EVENT_GAMEPAD_BUTTON_UP;
        e.gbutton.which = TEST_DEVICE, e.gbutton.button = (Uint8)b, e.gbutton.down = frame == start;
        SDL_PushEvent(&e);
        return;
    }
    if (frame != start) return;
    LOG("[input] test: typing \"%s\"\n", arg);
    static char ascii[128][2];  // (a text event keeps a pointer to its text: these live on)
    for (const char *p = arg; *p; p++) {
        if (*p == '{' && strchr(p, '}')) {  // a named key
            char nm[32];
            size_t n = (size_t)(strchr(p, '}') - p - 1);
            snprintf(nm, sizeof nm, "%.*s", (int)(n < sizeof nm ? n : sizeof nm - 1), p + 1);
            SDL_Scancode sc = SDL_GetScancodeFromName(nm);
            push_key(sc, SDL_KMOD_NONE, true), push_key(sc, SDL_KMOD_NONE, false);
            p += n + 1;
            continue;
        }
        SDL_Keymod mod = SDL_KMOD_NONE;
        SDL_Scancode sc = SDL_GetScancodeFromKey((SDL_Keycode)tolower((unsigned char)*p), &mod);
        if (isupper((unsigned char)*p)) mod |= SDL_KMOD_LSHIFT;
        push_key(sc, mod, true);
        int c = *p & 0x7f;
        ascii[c][0] = (char)c;
        memset(&e, 0, sizeof e);
        e.type = SDL_EVENT_TEXT_INPUT, e.text.windowID = TEST_DEVICE, e.text.text = ascii[c];
        SDL_PushEvent(&e);
        push_key(sc, mod, false);
    }
}

static void host_test_frame(uint64_t frame) {
    static const char *hspec, *bspec;
    static int init;
    if (!init) { init = 1; hspec = getenv("LARECOMP_HOST_INPUT"); bspec = getenv("LARECOMP_BIND_TEST"); host_test = hspec; }
    char buf[2048];
    if (hspec) {
        uint8_t keys[SDL_SCANCODE_COUNT] = {0};
        uint32_t padm = 0;
        int32_t sx = 0, sy = 0;
        snprintf(buf, sizeof buf, "%s", hspec);
        char *save = NULL;
        for (char *tok = strtok_r(buf, ",", &save); tok; tok = strtok_r(NULL, ",", &save)) {
            unsigned long start = 0, len = 10;
            char dev[12] = "", name[400] = "";
            if (sscanf(tok, "%lu:%11[a-z]:%399[^:]:%lu", &start, dev, name, &len) < 3) continue;
            if (!strcmp(dev, "type") || !strcmp(dev, "sdlpad") || !strcmp(dev, "sdlkey") || !strcmp(dev, "shot") ||
                !strcmp(dev, "sdlmouse") || !strcmp(dev, "sdlmove") || !strcmp(dev, "touch")) {
                test_push(frame, start, len, dev, name);
                continue;
            }
            if (frame < start || frame >= start + len) continue;
            if (!strcmp(dev, "key")) { SDL_Scancode sc = SDL_GetScancodeFromName(name); if (sc) keys[sc] = 1; }
            else if (!strcmp(dev, "lstick")) sscanf(name, "%d/%d", &sx, &sy);
            else { int h = bind_pad_from_name(name); if (h >= 0) padm |= 1u << h; }
        }
        bool kch = memcmp(keys, test_keys, sizeof keys) != 0, pch = padm != test_pad || sx != test_lx || sy != test_ly;
        memcpy(test_keys, keys, sizeof keys);
        test_pad = padm;
        test_lx = sx, test_ly = sy;
        if (kch) recompute_keyboard();
        if (pch) recompute_pad();
    }
    if (bspec) {
        snprintf(buf, sizeof buf, "%s", bspec);
        char *save = NULL;
        for (char *tok = strtok_r(buf, ",", &save); tok; tok = strtok_r(NULL, ",", &save)) {
            unsigned long start = 0;
            char dev[8] = "", in[16] = "", host[40] = "";
            if (sscanf(tok, "%lu:%7[a-z]:%15[a-z_]:%39[^:]", &start, dev, in, host) < 3) continue;
            if (!strcmp(dev, "reset")) { if (frame == start) bind_reset(!strcmp(in, "key") ? BIND_DEV_KEY : BIND_DEV_PAD); continue; }
            int d = !strcmp(dev, "key") ? BIND_DEV_KEY : BIND_DEV_PAD;
            if (frame == start) {
                LOG("[input] test: capture %s %s\n", dev, in);
                bind_capture_begin(d, bind_slot_from_name(in));
            }
            if (frame == start + 90 && strcmp(host, "wait")) {
                SDL_Event e;
                memset(&e, 0, sizeof e);
                if (!strcmp(host, "esc") || d == BIND_DEV_KEY) {
                    e.type = SDL_EVENT_KEY_DOWN;
                    e.key.scancode = !strcmp(host, "esc") ? SDL_SCANCODE_ESCAPE : SDL_GetScancodeFromName(host);
                } else {
                    int h = bind_pad_from_name(host);
                    if (h >= PADB_LTRIGGER) {
                        e.type = SDL_EVENT_GAMEPAD_AXIS_MOTION;
                        e.gaxis.axis = h == PADB_LTRIGGER ? SDL_GAMEPAD_AXIS_LEFT_TRIGGER : SDL_GAMEPAD_AXIS_RIGHT_TRIGGER;
                        e.gaxis.value = 32767;
                    } else {
                        e.type = SDL_EVENT_GAMEPAD_BUTTON_DOWN;
                        e.gbutton.button = (Uint8)h;
                    }
                }
                bind_capture_event(&e);
            }
        }
    }
}

static uint64_t g_pad_obj;
volatile int g_inject_plus;  // frames to hold Plus for the game (movie.c: A / B skip the opening movie too)
RT_ON_RESET(input_reboot_reset) { g_pad_obj = 0; g_inject_plus = 0; }  // (reboot: the controller object was guest memory)
void input_frame(uint64_t frame) {
    if (g_pad_obj && getenv("LARECOMP_DUMP_PAD") && frame % 600 == 0) {
        const uint8_t *m = (const uint8_t *)(uintptr_t)(g_pad_obj - 0x98);
        LOG("[pad] mgr %llx 4e0=%u 4e4=%u 4e8=%u 548=%#x 54c..=%02x %02x %02x 56c=%02x\n", g_pad_obj - 0x98,
            *(uint32_t *)(m + 0x4e0), *(uint32_t *)(m + 0x4e4), *(uint32_t *)(m + 0x4e8), *(uint32_t *)(m + 0x548),
            m[0x54c], m[0x54d], m[0x54e], m[0x56c]);
        for (int i = 0; i < 9; i++) {
            const uint8_t *o = m + 0x98 + 0x48 * i;
            uint64_t player = *(uint64_t *)(m + 0x10 * i);
            LOG("[pad]  ctl%d id %#x style %#x btn %#x conn %d %d | player %llx [8]=%u\n", i, *(uint32_t *)(o + 8),
                *(uint32_t *)(o + 0xc), *(uint32_t *)(o + 0x14), o[0x44], o[0x45], player,
                player ? *(uint32_t *)(uintptr_t)(player + 8) : 0);
        }
    }
    extern void watch_arm(void);
    watch_arm();
    host_test_frame(frame);
    extern void warp_frame(uint64_t frame);  // (LARECOMP_WARP: hle/warp.c)
    warp_frame(frame);
    {   // LARECOMP_LIVE=<dir> (testing): steered while it runs - lines appended to <dir>/cmd, "<frames> <keys>" with keys
        // LARECOMP_INPUT's names joined by '+' (A, STICK_UP, STICK_45 = left stick at 45 degrees, RSTICK_LEFT...; "-": none),
        // run one after another; <dir>/state says the frame and what's left (screenshots: <dir>/shot, nvn/vulkan/device.c)
        static const char *dir = (const char *)1;
        if (dir == (const char *)1) dir = getenv("LARECOMP_LIVE");
        if (dir) {
            static long off;
            static struct { long len; char keys[120]; } q[256];
            static int qh, qt;
            static uint64_t until;
            static bool active;
            char path[1100], line[160];
            snprintf(path, sizeof path, "%s/cmd", dir);
            FILE *f = fopen(path, "r");
            if (f) {
                fseek(f, off, SEEK_SET);
                while (fgets(line, sizeof line, f) && strchr(line, '\n')) {
                    off = ftell(f);
                    long len;
                    char keys[120];
                    if (sscanf(line, "%ld %119s", &len, keys) == 2 && (qt + 1) % 256 != qh) q[qt].len = len, snprintf(q[qt].keys, 120, "%s", keys), qt = (qt + 1) % 256;
                }
                fclose(f);
            }
            if (active && frame >= until) active = false, qh = (qh + 1) % 256;
            if (!active && qh != qt) active = true, until = frame + (uint64_t)q[qh].len;
            uint64_t b = 0;
            int32_t lx = 0, ly = 0, rx = 0, ry = 0;
            static const char *const nm[] = {"A", "B", "X", "Y", "LSTICK", "RSTICK", "L", "R", "ZL", "ZR", "PLUS",
                                             "MINUS", "LEFT", "UP", "RIGHT", "DOWN"};
            if (active) {
                char k[120];
                snprintf(k, sizeof k, "%s", q[qh].keys);
                for (char *t = strtok(k, "+"); t; t = strtok(NULL, "+")) {
                    if (!strcmp(t, "STICK_UP")) ly = 32767;
                    else if (!strcmp(t, "STICK_DOWN")) ly = -32767;
                    else if (!strcmp(t, "STICK_LEFT")) lx = -32767;
                    else if (!strcmp(t, "STICK_RIGHT")) lx = 32767;
                    else if (!strcmp(t, "RSTICK_UP")) ry = 32767;
                    else if (!strcmp(t, "RSTICK_DOWN")) ry = -32767;
                    else if (!strcmp(t, "RSTICK_LEFT")) rx = -32767;
                    else if (!strcmp(t, "RSTICK_RIGHT")) rx = 32767;
                    else if (!strncmp(t, "STICK_", 6) && isdigit((unsigned char)t[6])) {
                        double r = atoi(t + 6) * 3.14159265358979 / 180;
                        lx = (int32_t)lround(cos(r) * 32767), ly = (int32_t)lround(sin(r) * 32767);
                    } else if (!strncmp(t, "WALK_", 5) && isdigit((unsigned char)t[5])) {  // (half tilt: walk, not run)
                        double r = atoi(t + 5) * 3.14159265358979 / 180;
                        lx = (int32_t)lround(cos(r) * 14000), ly = (int32_t)lround(sin(r) * 14000);
                    }
                    for (int i = 0; i < 16; i++) if (!strcmp(t, nm[i])) b |= 1ull << i;
                }
            }
            static uint64_t last_state;
            if (frame - last_state >= 5) {  // (often: test scripts steer by the player position in it)
                last_state = frame;
                snprintf(path, sizeof path, "%s/state", dir);
                FILE *o = fopen(path, "w");
                extern bool warp_player_pos(float out[3]);  // (hle/warp.c)
                float pp[3];
                bool hp = warp_player_pos(pp);
                if (o) {
                    fprintf(o, "frame %llu queued %d%s%s\n", (unsigned long long)frame, (qt - qh + 256) % 256, active ? " now " : "", active ? q[qh].keys : "");
                    if (hp) fprintf(o, "player %.2f %.2f %.2f\n", pp[0], pp[1], pp[2]);
                    fclose(o);
                }
            }
            script_buttons = b, script_lx = lx, script_ly = ly, script_rx = rx, script_ry = ry;
            return;
        }
    }
    {   // LARECOMP_REPLAY=<file>: a LARECOMP_REC recording, by game frame
        static struct RecLine { uint64_t f, b; int32_t a[4]; } *rl;
        static size_t nrl, pos;
        static int init;
        if (!init) {
            init = 1;
            FILE *f = getenv("LARECOMP_REPLAY") ? fopen(getenv("LARECOMP_REPLAY"), "r") : NULL;
            size_t cap = 0;
            unsigned long long fr, bb;
            int a0, a1, a2, a3;
            char line[256];
            while (f && fgets(line, sizeof line, f)) {
                char text[96];
                if (line[0] == 'K' && sscanf(line, "K %llu %95[^\n]", &fr, text) == 2) {
                    if (g_nreplay_text < 8) snprintf(g_replay_text[g_nreplay_text++], sizeof g_replay_text[0], "%s", text);
                    continue;
                }
                if (sscanf(line, "%llu %llx %d %d %d %d", &fr, &bb, &a0, &a1, &a2, &a3) != 6) continue;
                if (nrl == cap) cap = cap ? cap * 2 : 4096, rl = realloc(rl, cap * sizeof *rl);
                rl[nrl++] = (struct RecLine){fr, bb, {a0, a1, a2, a3}};
            }
            if (f) { fclose(f); LOG("[input] replaying %zu controller changes from %s\n", nrl, getenv("LARECOMP_REPLAY")); }
        }
        // LARECOMP_REPLAY_AT=<frame>:<recorded frame>: LARECOMP_INPUT drives the game until <frame>, the recording from
        // <recorded frame> on after it (a scripted way into the game, then the player's session from where they could move)
        static long at = -2, from;
        if (at == -2) { at = -1; if (getenv("LARECOMP_REPLAY_AT")) sscanf(getenv("LARECOMP_REPLAY_AT"), "%ld:%ld", &at, &from); }
        uint64_t rf = frame;
        if (nrl && at >= 0) {
            if ((long)frame < at) goto scripted;
            rf = frame - (uint64_t)at + (uint64_t)from;
            static bool said;
            if (!said) { said = true; LOG("[input] replay takes over at frame %llu (recorded frame %ld)\n", (unsigned long long)frame, from); }
        }
        if (nrl) {
            while (pos + 1 < nrl && rl[pos + 1].f <= rf) pos++;
            bool on = rl[pos].f <= rf;
            script_buttons = on ? rl[pos].b : 0;
            script_lx = on ? rl[pos].a[0] : 0, script_ly = on ? rl[pos].a[1] : 0;
            script_rx = on ? rl[pos].a[2] : 0, script_ry = on ? rl[pos].a[3] : 0;
            return;
        }
    }
scripted:;
    const char *spec = getenv("LARECOMP_INPUT");
    if (!spec) return;
    static const char *const names[] = {"A", "B", "X", "Y", "LSTICK", "RSTICK", "L", "R", "ZL", "ZR", "PLUS",
                                        "MINUS", "LEFT", "UP", "RIGHT", "DOWN"};
    uint64_t b = 0;
    int32_t lx = 0, ly = 0, rx = 0, ry = 0;
    static char buf[16384];  // (long scripted sessions: the perf walk is ~4 KB)
    snprintf(buf, sizeof buf, "%s", spec);
    // LARECOMP_INPUT_LOOP=<start>:<period>: from frame <start> on, the script's entries in [start, start + period) repeat
    // every period frames (long unattended sessions: letsgo-recomp's tools/capture_loop.py walks through the grass and captures for hours)
    static long loop_start = -1, loop_period;
    if (loop_start < 0) { loop_start = 0; const char *e = getenv("LARECOMP_INPUT_LOOP"); if (e) sscanf(e, "%ld:%ld", &loop_start, &loop_period); }
    uint64_t real_frame = frame;
    if (loop_period > 0 && frame >= (uint64_t)loop_start) frame = (uint64_t)loop_start + (frame - (uint64_t)loop_start) % (uint64_t)loop_period;
    for (char *tok = strtok(buf, ","); tok; tok = strtok(NULL, ",")) {
        unsigned long start = 0, len = 10;
        char name[32] = "";
        if (sscanf(tok, "%lu:%31[A-Z_0-9]:%lu", &start, name, &len) < 2) continue;
        if (frame < start || frame >= start + len) continue;
        if (!strcmp(name, "STICK_UP")) ly = 32767;
        else if (!strcmp(name, "STICK_DOWN")) ly = -32767;
        else if (!strcmp(name, "STICK_LEFT")) lx = -32767;
        else if (!strcmp(name, "STICK_RIGHT")) lx = 32767;
        else if (!strcmp(name, "RSTICK_UP")) ry = 32767;
        else if (!strcmp(name, "RSTICK_DOWN")) ry = -32767;
        else if (!strcmp(name, "RSTICK_LEFT")) rx = -32767;
        else if (!strcmp(name, "RSTICK_RIGHT")) rx = 32767;
        else if (!strncmp(name, "STICK_", 6) && name[6] >= '0' && name[6] <= '9') {
            double r = atoi(name + 6) * 3.14159265358979 / 180;
            lx = (int32_t)lround(cos(r) * 32767), ly = (int32_t)lround(sin(r) * 32767);
        }
        for (int i = 0; i < 16; i++) if (!strcmp(name, names[i])) b |= 1ull << i;
    }
    if (loop_period > 0) {  // (the loop holds still for 300 frames once the world is back after a battle / capture: the
        // capture loop compares that view with the one before the encounter)
        extern long fps60_world_back(void);
        long wb = fps60_world_back();
        if (wb >= 0 && (long)real_frame >= wb && (long)real_frame - wb < 300) b = 0, lx = ly = rx = ry = 0;
        extern bool fps60_in_scene(void);  // (inside the encounter: run away)
        static int catch_ = -1;  // LARECOMP_INPUT_LOOP_CATCH=1: A every 40 frames instead (get ready, throw, the screens
        if (catch_ < 0) catch_ = getenv("LARECOMP_INPUT_LOOP_CATCH") ? atoi(getenv("LARECOMP_INPUT_LOOP_CATCH")) : 0;
        // after a catch: experience, Pokédex): what a player does, more textures replaced than running away
        if (fps60_in_scene() && catch_) {
            lx = ly = rx = ry = 0;
            b = real_frame % 40 < 6 ? 1ull : 0;
        } else if (fps60_in_scene()) {  // the encounter menu (Get ready / Bag / Help / Run away): B back to it, down three, A;
            // "give up catching?" may start on No: up, A
            uint64_t ph = real_frame % 80;
            lx = ly = rx = ry = 0;
            b = ph < 6 ? 1ull << 1 : (ph >= 12 && ph < 18) || (ph >= 22 && ph < 28) || (ph >= 32 && ph < 38) ? 1ull << 15 :
                (ph >= 42 && ph < 48) || (ph >= 68 && ph < 74) ? 1ull : ph >= 56 && ph < 62 ? 1ull << 13 : 0;
        }
    }
    script_buttons = b;
    script_lx = lx;
    script_ly = ly;
    script_rx = rx;
    script_ry = ry;
}

extern bool menu_input_captured(void);  // runtime/ui/menu.mm: overlay menu or name entry up (it has the keys / pad)

// Buttons held right now (keyboard, gamepad, test script), independent of whether the game polls (movies).
uint64_t input_buttons_now(void) { return (menu_input_captured() ? 0 : key_buttons | pad_buttons | plugin_buttons) | script_buttons; }

static int32_t clamp_axis(int32_t v) { return v > 32767 ? 32767 : v < -32767 ? -32767 : v; }

// LARECOMP_REC=<file>: what the game reads from the controller (buttons, both sticks), a line "<game frame> <buttons hex>
// <lx> <ly> <rx> <ry>" each time it changes - a player's session, for LARECOMP_REPLAY=<file> to drive the game the same
// way from the same save (copied to <file>.save/ when the recording starts).
static FILE *g_rec;
static void input_rec_sample(uint64_t b, int32_t lx, int32_t ly, int32_t rx, int32_t ry) {
    static int init;
    if (!init) {
        init = 1;
        const char *path = getenv("LARECOMP_REC");
        if (path && *path) {
            g_rec = fopen(path, "w");
            if (g_rec) {
                char cmd[2200];
                snprintf(cmd, sizeof cmd, "rm -rf '%s.save' && cp -a save '%s.save'", path, path);
                if (system(cmd)) {}
                LOG("[input] recording the controller to %s (save copied to %s.save)\n", path, path);
            }
        }
    }
    if (!g_rec) return;
    static uint64_t lb = ~0ull;
    static int32_t l[4];
    b &= ~0xFF0000ull;  // (the stick directions as buttons: derived from the sticks again on replay)
    if (b == lb && lx == l[0] && ly == l[1] && rx == l[2] && ry == l[3]) return;
    lb = b, l[0] = lx, l[1] = ly, l[2] = rx, l[3] = ry;
    extern uint64_t renderer_game_frame(void);
    fprintf(g_rec, "%llu %llx %d %d %d %d\n", (unsigned long long)renderer_game_frame(), (unsigned long long)b, lx, ly, rx, ry);
    fflush(g_rec);
}

// The name typed into the game's keyboard dialog (runtime/ui/menu.mm): a line "K <game frame> <text>" in the recording,
// and the next one of the replay's answers the dialog at once (no game frames pass while it is open: the game waits)
void input_rec_text(const char *text) {
    if (!g_rec) return;
    extern uint64_t renderer_game_frame(void);
    fprintf(g_rec, "K %llu %s\n", (unsigned long long)renderer_game_frame(), text);
    fflush(g_rec);
}
bool input_replay_text(char *out, int cap) {
    if (g_replay_text_next >= g_nreplay_text) return false;
    snprintf(out, (size_t)cap, "%s", g_replay_text[g_replay_text_next++]);
    return true;
}

// NpadFullKeyState / NpadHandheldState / NpadJoy*State share this layout (0x28 bytes):
// {int64 sampling, u64 buttons, AnalogStick L {x,y}, AnalogStick R {x,y}, u32 attributes}
static uint32_t npad_id_used(uint32_t *style);  // (below: which controller the player has)

static void fill_state(Cpu *c) {
    uint8_t *s = (uint8_t *)(uintptr_t)c->x[0];
    uint32_t id = *(uint32_t *)(uintptr_t)c->x[1];
    memset(s, 0, 0x28);
    *(int64_t *)s = ++sampling;
    static int nlog;
    if (nlog < 6 && (sampling % 600) == 1) { nlog++; LOG("[input] GetNpadState id %#x lr %llx\n", id, c->x[30]); }
    if (id != npad_id_used(NULL)) return;  // only the one controller (npad_id_used) is connected
    extern uint64_t g_watch_lo, g_watch_hi;
    if (!g_pad_obj) g_pad_obj = c->x[1] - 8;
    if (!g_watch_lo && getenv("LARECOMP_WATCH_PAD")) {
        unsigned lo = 0x98 + 0x10, hi = 0x98 + 0x48;  // offsets from the controller manager
        sscanf(getenv("LARECOMP_WATCH_PAD"), "%x,%x", &lo, &hi);
        g_watch_lo = c->x[1] - 8 - 0x98 + lo;
        g_watch_hi = c->x[1] - 8 - 0x98 + hi;
        LOG("[input] watching controller object %llx\n", c->x[1] - 8);
    }
    bool menu = menu_input_captured();  // the overlay menu / name entry has the controls (typing there isn't for the game)
    bool input_pad_finger_on(void);
    const uint64_t zr = input_pad_finger_on() ? 1ull << BTN_ZR : 0;  // (with the finger on, the controller's ZR touches)
    if (wheel_until[0] || wheel_until[1]) recompute_keyboard();  // (a wheel notch's binding lets go by time)
    uint64_t b = menu ? script_buttons & ~zr : key_buttons | plugin_buttons | ((pad_buttons | script_buttons) & ~zr);
    {
        uint64_t menu_mouse_buttons(Cpu *c);  // (runtime/hle/menu_mouse.c: the mouse in the game's menus)
        uint64_t m = menu_mouse_buttons(c);
        if (!menu) b |= m;
        uint64_t settings_screen_buttons(Cpu *c, uint64_t b);  // (runtime/hle/za_options.c: the Settings screen's Controls list)
        if (!menu) b = settings_screen_buttons(c, b);
    }
    {   // injected Plus: held for ~10 presented frames (the game polls several times per frame)
        extern uint64_t renderer_game_frame(void);
        static uint64_t until;
        if (g_inject_plus > 0) { until = renderer_game_frame() + 10; g_inject_plus = 0; }
        if (renderer_game_frame() < until) b |= 1ull << BTN_PLUS;
    }
    int32_t lx = clamp_axis((menu ? 0 : key_lx + pad_lx + plugin_lx) + script_lx), ly = clamp_axis((menu ? 0 : key_ly + pad_ly + plugin_ly) + script_ly);
    int32_t mrx = 0, mry = 0;
    mouse_stick(&mrx, &mry);
    int32_t rx = clamp_axis((menu ? 0 : key_rx + pad_rx + plugin_rx + mrx) + script_rx), ry = clamp_axis((menu ? 0 : key_ry + pad_ry + plugin_ry + mry) + script_ry);
    // d-pad / arrow keys walk (setting) while the stick is at rest. "At rest" has a dead zone: a controller's stick
    // never reads exactly 0 (a DualSense rests at a few hundred), which used to turn the d-pad off altogether.
    if (!menu && g_settings.dpad_move && (int64_t)lx * lx + (int64_t)ly * ly < 8000ll * 8000) {
        uint64_t d = key_buttons | pad_buttons | plugin_buttons | script_buttons;
        int dx = (int)((d >> BTN_RIGHT) & 1) - (int)((d >> BTN_LEFT) & 1);
        int dy = (int)((d >> BTN_UP) & 1) - (int)((d >> BTN_DOWN) & 1);
        int32_t m = dx && dy ? 23170 : 32767;  // diagonals: same speed (32767 / sqrt 2)
        if (dx || dy) lx = dx * m, ly = dy * m;
    }
    {   // (Arceus above 30 fps: the buttons change only on the updates its scripts step on - hle/fps60.c pla_input_tick)
        extern int pla_input_tick(void);
        static uint64_t held;
        if (pla_input_tick()) held = b;
        else b = held;
    }
    // stick directions also appear as buttons (bits 16-23)
    if (lx < -16000) b |= 1ull << 16;
    if (ly > 16000) b |= 1ull << 17;
    if (lx > 16000) b |= 1ull << 18;
    if (ly < -16000) b |= 1ull << 19;
    if (rx < -16000) b |= 1ull << 20;
    if (ry > 16000) b |= 1ull << 21;
    if (rx > 16000) b |= 1ull << 22;
    if (ry < -16000) b |= 1ull << 23;
    *(uint64_t *)(s + 8) = b;
    input_rec_sample(b, lx, ly, rx, ry);
    static uint64_t last_b;
    if (b != last_b && getenv("LARECOMP_INPUT")) LOG("[input] id %#x buttons %#llx (sampling %lld, lr %llx)\n", id, (unsigned long long)b, (long long)sampling, c->x[30]);
    last_b = b;
    *(int32_t *)(s + 16) = lx;
    *(int32_t *)(s + 20) = ly;
    *(int32_t *)(s + 24) = rx;
    *(int32_t *)(s + 28) = ry;
    *(uint32_t *)(s + 32) = 1;  // IsConnected
}

HLE_FUNC("_ZN2nn3hid12GetNpadStateEPNS0_16NpadFullKeyStateERKj", hid_state_fullkey) { fill_state(c); }
HLE_FUNC("_ZN2nn3hid12GetNpadStateEPNS0_16NpadJoyDualStateERKj", hid_state_dual) { fill_state(c); }
HLE_FUNC("_ZN2nn3hid12GetNpadStateEPNS0_16NpadJoyLeftStateERKj", hid_state_left) { fill_state(c); }
HLE_FUNC("_ZN2nn3hid12GetNpadStateEPNS0_17NpadHandheldStateERKj", hid_state_handheld) { fill_state(c); }
HLE_FUNC("_ZN2nn3hid12GetNpadStateEPNS0_17NpadJoyRightStateERKj", hid_state_right) { fill_state(c); }

// The one controller the PC gamepad / keyboard appears as. Let's Go accepts no Pro Controller (FullKey): only a
// single Joy-Con, the Poke Ball Plus and the console in handheld mode - handheld maps every button and both sticks.
// LARECOMP_NPAD=handheld (default) | fullkey | dual | left | right.
static uint32_t npad_id_used(uint32_t *style) {
    static uint32_t id = ~0u, st;
    if (id == ~0u) {
        const char *e = getenv("LARECOMP_NPAD");
        if (!e || !strcmp(e, "handheld")) id = 0x20, st = 1u << 1;
        else if (!strcmp(e, "fullkey")) id = 0, st = 1u << 0;
        else if (!strcmp(e, "dual")) id = 0, st = 1u << 2;
        else if (!strcmp(e, "left")) id = 0, st = 1u << 3;
        else if (!strcmp(e, "right")) id = 0, st = 1u << 4;
        else id = 0x20, st = 1u << 1;
        LOG("[input] controller: npad id %#x, style %#x\n", id, st);
    }
    if (style) *style = st;
    return id;
}

HLE_FUNC("_ZN2nn3hid15GetNpadStyleSetERKj", hid_style_set) {
    uint32_t id = *(uint32_t *)(uintptr_t)c->x[0], style;
    static int nlog;
    if (nlog++ < 4) LOG("[input] GetNpadStyleSet id %#x\n", id);
    c->x[0] = id == npad_id_used(&style) ? style : 0;
}

// BindNpadStyleSetUpdateEvent(id, SystemEventType*, clear mode): signaled once so the game sees a
// controller connected.
#include "../kernel.h"
HLE_FUNC("_ZN2nn3hid27BindNpadStyleSetUpdateEventERKjPNS_2os15SystemEventTypeENS3_14EventClearModeE", hid_bind_style_event) {
    uint32_t w, r;
    KEvent *ev = kernel_create_event_pair(&w, &r);
    pthread_mutex_lock(&g_klock);
    kernel_signal(ev);
    pthread_mutex_unlock(&g_klock);
    uint64_t clear_mode = c->x[2];
    c->x[0] = c->x[1];
    c->x[1] = r;
    c->x[2] = 0;
    c->x[3] = clear_mode;
    guest_call(c, guest_sym("_ZN2nn2os33AttachReadableHandleToSystemEventEPNS0_15SystemEventTypeEjbNS0_14EventClearModeE"));
    LOG("[input] style set update event bound\n");
}

// Touch screen: TouchScreenState<16> {int64 sampling; int32 count; pad; TouchState[16]}, TouchState (0x28) {int64
// delta time; u32 attributes (bit 0 Start, bit 1 End); u32 finger id; u32 x, y; u32 diameter x, y; u32 rotation;
// pad}. One finger: the free mouse (touch_at), the controller's finger (pad_finger), or
// LARECOMP_TOUCH="<from>-<to>[@x,y[,width]]" (frames: a back-and-forth petting stroke `width` wide - default 120,
// 0 = a press in place - around x,y, default the middle of the screen; display pixels, 1920 x 1080) or "<from>-<to>@x,y>x2,y2" (a drag from x,y
// to x2,y2); several segments separated by ';' (consecutive frames: one touch going on); for tests.
static bool pad_finger(int32_t *x, int32_t *y);
static bool touch_now(int32_t *x, int32_t *y) {
    static struct { int from, to, cx, cy, amp, ex, ey; } seg[8];
    static int nseg = -1;
    if (nseg < 0) {
        nseg = 0;
        for (const char *e = getenv("LARECOMP_TOUCH"); e && *e && nseg < 8; e = strchr(e, ';') ? strchr(e, ';') + 1 : NULL) {
            seg[nseg] = (typeof(seg[0])){0, 0, 960, 495, 180, -1, -1};
            sscanf(e, "%d-%d@%d,%d,%d", &seg[nseg].from, &seg[nseg].to, &seg[nseg].cx, &seg[nseg].cy, &seg[nseg].amp);
            const char *gt = strchr(e, '>'), *semi = strchr(e, ';');
            if (gt && (!semi || gt < semi)) sscanf(gt + 1, "%d,%d", &seg[nseg].ex, &seg[nseg].ey);
            if (seg[nseg].from > 0) nseg++;
        }
    }
    extern uint64_t renderer_game_frame(void);
    uint64_t f = renderer_game_frame();
    for (int i = 0; i < nseg; i++) {
        if (f < (uint64_t)seg[i].from || f > (uint64_t)seg[i].to) continue;
        double t = (double)(f - seg[i].from), k = seg[i].to > seg[i].from ? t / (seg[i].to - seg[i].from) : 1;
        *x = (int32_t)(seg[i].ex >= 0 ? seg[i].cx + (seg[i].ex - seg[i].cx) * k : seg[i].cx + seg[i].amp * sin(t * 0.25));
        *y = (int32_t)(seg[i].ex >= 0 ? seg[i].cy + (seg[i].ey - seg[i].cy) * k : seg[i].cy + seg[i].amp / 6 * sin(t * 0.5));
        return true;
    }
    if (pad_finger(x, y)) return true;
    *x = touch_x, *y = touch_y;
    return touch_down && !menu_input_captured();
}
// The controller as a finger (a controller has no touch screen, and Play with Eevee pets with one): the right stick
// moves it over the screen - the overlay draws it (runtime/ui/menu.mm) - and ZR held touches there. ZR then doesn't
// reach the game (fill_state; in Let's Go it only repeats A). Off by default: PLA uses ZR (throwing) and the right
// stick (camera). LARECOMP_PAD_FINGER=1 turns it on.
bool input_pad_finger_on(void) {
    static int on = -1;
    if (on < 0) on = getenv("LARECOMP_PAD_FINGER") && atoi(getenv("LARECOMP_PAD_FINGER")) != 0;
    return on;
}
static double finger_x = 960, finger_y = 540;
static _Atomic uint64_t finger_seen_ns;
static _Atomic int finger_down;
static bool pad_finger(int32_t *x, int32_t *y) {
    if (!input_pad_finger_on()) return false;
    static uint64_t last;
    uint64_t now = SDL_GetTicksNS();
    double dt = last ? (now - last) / 1e9 : 0;
    if (dt > 0.1) dt = 0.1;
    last = now;
    int32_t rx = pad_rx + script_rx, ry = pad_ry + script_ry;
    double ax = abs(rx) > 6000 ? rx / 32767.0 : 0, ay = abs(ry) > 6000 ? ry / 32767.0 : 0;
    if (ax != 0 || ay != 0) {
        finger_x += ax * 1000 * dt;  // (display pixels per second at full tilt: about 2 s across)
        finger_y -= ay * 1000 * dt;
        finger_x = finger_x < 0 ? 0 : finger_x > touch_w() - 1 ? touch_w() - 1 : finger_x;
        finger_y = finger_y < 0 ? 0 : finger_y > TOUCH_H - 1 ? TOUCH_H - 1 : finger_y;
        finger_seen_ns = now;
    }
    bool down = ((pad_buttons | script_buttons) >> BTN_ZR & 1) && !menu_input_captured();
    if (down) finger_seen_ns = now;
    finger_down = down;
    *x = (int32_t)finger_x, *y = (int32_t)finger_y;
    return down;
}
// Overlay (runtime/ui/menu.mm): where to draw the controller's finger, in window pixels, for 3 s after it last moved.
bool input_finger_cursor(float *x, float *y, bool *down) {
    uint64_t seen = finger_seen_ns;
    if (!seen || SDL_GetTicksNS() - seen > 3000000000ull) return false;
    *x = (float)(g_present_rect[0] + finger_x / touch_w() * g_present_rect[2]);
    *y = (float)(g_present_rect[1] + finger_y / TOUCH_H * g_present_rect[3]);
    *down = finger_down;
    return true;
}

static void touch_caller_log(const char *what, uint64_t lr) {  // (LARECOMP_TOUCH runs: who reads touches / gestures)
    static uint64_t seen[32];
    static int n, on = -1;
    if (on < 0) on = getenv("LARECOMP_TOUCH") != NULL;
    if (!on) return;
    for (int i = 0; i < n; i++) if (seen[i] == lr) return;
    if (n < 32) seen[n++] = lr;
    extern uint64_t renderer_game_frame(void);
    LOG("[touch] %s read from lr %llx (frame %llu)\n", what, (unsigned long long)lr, (unsigned long long)renderer_game_frame());
}
HLE_FUNC("_ZN2nn3hid20GetTouchScreenStatesILm16EEEiPNS0_16TouchScreenStateIXT_EEEi", hid_touch_states) {
    touch_caller_log("touch screen", c->x[30]);
    uint8_t *s = (uint8_t *)(uintptr_t)c->x[0];
    int count = (int)c->x[1];
    static int64_t sampling;
    static bool was_down;
    static int32_t last_x, last_y;
    static int nlog;
    if (count <= 0) { c->x[0] = 0; return; }
    int32_t x, y;
    bool down = touch_now(&x, &y);
    // The release sample keeps the finger where it was: the game checks every sample's position (finger id -> x, y),
    // and a release reported elsewhere (the test's default, the mouse while the controller's finger let go) took the
    // touch off an on-screen button before it ended - the berry button never opened.
    if (down) last_x = x, last_y = y;
    else x = last_x, y = last_y;
    memset(s, 0, 0x290);
    *(int64_t *)s = ++sampling;
    if (down || was_down) {  // (the release is one more sample, flagged End)
        uint8_t *t = s + 0x10;
        *(int32_t *)(s + 8) = 1;
        *(int64_t *)t = 16666667;
        *(uint32_t *)(t + 8) = down && !was_down ? 1 : !down ? 2 : 0;
        *(uint32_t *)(t + 0x10) = (uint32_t)x;
        *(uint32_t *)(t + 0x14) = (uint32_t)y;
        *(uint32_t *)(t + 0x18) = *(uint32_t *)(t + 0x1c) = 15;
        if (nlog < 12 && down != was_down) {
            nlog++;
            LOG("[touch] %s at %d,%d (lr %llx)\n", down ? "down" : "up", x, y, (unsigned long long)c->x[30]);
        }
    }
    was_down = down;
    c->x[0] = 1;
}
// Gestures (Let's Go reads them at start-up only - its screens go by the raw touches above - but they are cheap to
// keep right), recognized from the same finger as the system does: Touch (new touch) -> Press (held still 0.5 s) / Pan (moved, per frame) -> Tap
// (short, still) or Swipe (fast) -> Complete. GestureState (0x60): int64 event number, int64 context number, u32 type
// (0 idle, 1 complete, 2 cancel, 3 touch, 4 press, 5 tap, 6 pan, 7 swipe), u32 direction (0 none, 1 left, 2 up,
// 3 right, 4 down), u32 x, y, s32 delta x, y, f32 velocity x, y, u32 attributes (bit 4 new touch), f32 scale,
// f32 rotation, s32 point count, points[4] {u32 x, y}. The game reads the newest first and keeps the event number.
typedef struct { int64_t num, ctx; uint32_t type, dir, x, y; int32_t dx, dy; float vx, vy; uint32_t attr; float scale, rot;
                 int32_t npoints; uint32_t pts[8]; } Gesture;
_Static_assert(sizeof(Gesture) == 0x60, "nn::hid::GestureState");
static Gesture g_gestures[16];
static int64_t g_gesture_num, g_gesture_ctx;
static void gesture_emit(uint32_t type, uint32_t dir, int32_t x, int32_t y, int32_t dx, int32_t dy, float vx, float vy,
                         uint32_t attr, bool point) {
    if (getenv("LARECOMP_TOUCH")) LOG("[touch] gesture %u at %d,%d\n", type, x, y);
    memmove(g_gestures + 1, g_gestures, sizeof g_gestures - sizeof g_gestures[0]);
    Gesture *g = &g_gestures[0];
    memset(g, 0, sizeof *g);
    g->num = ++g_gesture_num;
    g->ctx = g_gesture_ctx;
    g->type = type, g->dir = dir, g->x = (uint32_t)x, g->y = (uint32_t)y, g->dx = dx, g->dy = dy, g->vx = vx, g->vy = vy;
    g->attr = attr, g->scale = 1;
    if (point) { g->npoints = 1; g->pts[0] = (uint32_t)x; g->pts[1] = (uint32_t)y; }
}
// Once per presented frame (the game may poll several times): the finger's new sample -> gesture events.
static void gesture_update(void) {
    extern uint64_t renderer_game_frame(void);
    static uint64_t frame = ~0ull, start_ns, last_ns;
    static bool was, moved, pressed;
    static int32_t sx, sy, lx, ly;
    uint64_t f = renderer_game_frame();
    if (f == frame) return;
    frame = f;
    int32_t x, y;
    bool down = touch_now(&x, &y);
    uint64_t now = SDL_GetTicksNS();
    double dt = last_ns ? (now - last_ns) / 1e9 : 1 / 30.0;
    if (dt <= 0) dt = 1 / 30.0;
    last_ns = now;
    if (down && !was) {
        g_gesture_ctx++;
        sx = lx = x, sy = ly = y, start_ns = now, moved = pressed = false;
        gesture_emit(3, 0, x, y, 0, 0, 0, 0, 1u << 4, true);  // Touch, new touch
    } else if (down) {
        if (!moved && (abs(x - sx) > 16 || abs(y - sy) > 16)) moved = true;
        if (moved && (x != lx || y != ly)) {
            gesture_emit(6, 0, x, y, x - lx, y - ly, (float)((x - lx) / dt), (float)((y - ly) / dt), 0, true);  // Pan
        } else if (!moved && !pressed && now - start_ns > 500000000ull) {
            pressed = true;
            gesture_emit(4, 0, x, y, 0, 0, 0, 0, 0, true);  // Press
        }
        lx = x, ly = y;
    } else if (was) {
        double t = (now - start_ns) / 1e9, vx = (lx - sx) / (t > 0 ? t : 1), vy = (ly - sy) / (t > 0 ? t : 1);
        if (!moved && !pressed) gesture_emit(5, 0, sx, sy, 0, 0, 0, 0, 0, true);  // Tap
        else if (moved && t < 0.4 && fabs(vx) + fabs(vy) > 800) {  // Swipe
            uint32_t dir = fabs(vx) > fabs(vy) ? (vx < 0 ? 1 : 3) : (vy < 0 ? 2 : 4);
            gesture_emit(7, dir, lx, ly, lx - sx, ly - sy, (float)vx, (float)vy, 0, true);
        }
        gesture_emit(1, 0, lx, ly, 0, 0, 0, 0, 0, false);  // Complete
    }
    was = down;
}
HLE_FUNC("_ZN2nn3hid16GetGestureStatesEPNS0_12GestureStateEi", hid_gesture_states) {
    touch_caller_log("gestures", c->x[30]);
    gesture_update();
    int count = (int)c->x[1], n = 0;
    Gesture *out = (Gesture *)(uintptr_t)c->x[0];
    for (; n < count && n < 16 && g_gestures[n].num; n++) out[n] = g_gestures[n];
    c->x[0] = (uint64_t)n;
}

// Vibration devices. The game's values (nn::hid::SendVibrationValue: a motor's {low band amplitude, frequency, high
// band amplitude, frequency}; the device handle's bit 0: left / right Joy-Con) reach the controller's motors through
// SDL_RumbleGamepad, the stronger of the two sides for each band: low -> the big (low frequency) motor, high -> the
// small one. The game's own "Controller Rumble" option decides whether it sends any. A value lasts 250 ms unless the
// next one replaces it (the game keeps sending while it vibrates, and zeros when it stops). LARECOMP_RUMBLE=0: off.
HLE_FUNC("_ZN2nn3hid25InitializeVibrationDeviceERKNS0_21VibrationDeviceHandleE", hid_init_vibration) { (void)c; }
static float g_vib[2][2];  // [left / right][low, high band amplitude]
// (also the hid service's SendVibrationValue(s), runtime/services/hid_set.c: the SDK's vibration player sends that way)
void input_rumble(uint32_t h, const float *v) {
    static int off = -1;
    if (off < 0) off = getenv("LARECOMP_RUMBLE") && atoi(getenv("LARECOMP_RUMBLE")) == 0;
    if (off || !v) return;
    static int nall;
    if (getenv("LARECOMP_RUMBLE_LOG") && nall < 40) nall++, LOG("[input] vibration %x: %g %g %g %g\n", h, v[0], v[1], v[2], v[3]);
    int m = (int)(h & 1);
    g_vib[m][0] = v[0] > 0 ? (v[0] < 1 ? v[0] : 1) : 0, g_vib[m][1] = v[2] > 0 ? (v[2] < 1 ? v[2] : 1) : 0;
    float lo = g_vib[0][0] > g_vib[1][0] ? g_vib[0][0] : g_vib[1][0], hi = g_vib[0][1] > g_vib[1][1] ? g_vib[0][1] : g_vib[1][1];
    uint16_t l16 = (uint16_t)(lo * 65535.0f), h16 = (uint16_t)(hi * 65535.0f);
    static uint16_t last_l = 1, last_h = 1;
    static uint64_t last_t;
    uint64_t now = SDL_GetTicks();
    if (l16 == last_l && h16 == last_h && now - last_t < 150) return;  // (the same again: refreshed now and then)
    last_l = l16, last_h = h16, last_t = now;
    static int logged;
    if ((l16 || h16) && logged < 5) logged++, LOG("[input] rumble low %.2f high %.2f\n", lo, hi);
    if (pad) SDL_RumbleGamepad(pad, l16, h16, 250);
}
HLE_FUNC("_ZN2nn3hid18SendVibrationValueERKNS0_21VibrationDeviceHandleERKNS0_14VibrationValueE", hid_send_vibration) {
    if (c->x[0] && c->x[1]) input_rumble(*(const uint32_t *)(uintptr_t)c->x[0], (const float *)(uintptr_t)c->x[1]);
}
// Six-axis (motion) sensor. Let's Go aims Poke Ball throws with the gyro in handheld mode; with no sensor behind it
// the SDK read garbage and the aim went into the sky. The rest pose is a controller lying level (identity orientation,
// gravity -z), which keeps the aim centred. The mouse turns this virtual controller (yaw / pitch, input_handle_event
// accumulates the motion), with every field consistent: orientation, angle, angular velocity and gravity.
// SixAxisSensorState (0x60): +0 delta time ns, +8 sampling number, +0x10 acceleration (G), +0x1c angular velocity
// (turns / s), +0x28 angle (turns), +0x34 direction (3x3: x, y, z axes), +0x58 attributes (bit 0 IsConnected).
// LARECOMP_GYRO_TEST=x|y|z sweeps the yaw axis ±15 degrees (2 s period) to see which way the aim follows.
static double gyro_yaw, gyro_pitch;               // radians
static _Atomic uint64_t gyro_start_ns;            // when the game last started the sensor (aiming begins)
static int gyro_yaw_axis = 2, gyro_pitch_axis = 0;
static void mat_rot(double m[3][3], int axis, double a) {
    int i = (axis + 1) % 3, j = (axis + 2) % 3;
    memset(m, 0, 9 * sizeof(double));
    m[axis][axis] = 1;
    m[i][i] = cos(a); m[i][j] = -sin(a);
    m[j][i] = sin(a); m[j][j] = cos(a);
}
static void mat_mul(double o[3][3], double a[3][3], double b[3][3]) {
    for (int r = 0; r < 3; r++)
        for (int k = 0; k < 3; k++) o[r][k] = a[r][0] * b[0][k] + a[r][1] * b[1][k] + a[r][2] * b[2][k];
}
static void six_axis_fill(uint8_t *s, int64_t sampling) {
    static int test = -1, test_fixed;
    if (test < 0) {
        const char *e = getenv("LARECOMP_GYRO_TEST");
        test = e && *e >= 'x' && *e <= 'z' ? 1 + (*e - 'x') : 0;
        if (test && (e[1] == '+' || e[1] == '-')) test_fixed = e[1] == '+' ? 1 : -1;
        const char *ax = getenv("LARECOMP_GYRO_AXES");  // "<yaw><pitch>", e.g. "zx" (the default)
        if (ax && ax[0] >= 'x' && ax[0] <= 'z' && ax[1] >= 'x' && ax[1] <= 'z') gyro_yaw_axis = ax[0] - 'x', gyro_pitch_axis = ax[1] - 'x';
    }
    static uint64_t last_ns;
    uint64_t now = SDL_GetTicksNS();
    double dt = last_ns ? (now - last_ns) / 1e9 : 1 / 60.0;
    if (dt < 1e-4) dt = 1e-4;
    if (dt > 0.1) dt = 0.1;
    last_ns = now;
    double py = gyro_yaw, pp = gyro_pitch;
    int yaw_axis = gyro_yaw_axis;
    if (test) {
        yaw_axis = test - 1;
        // x+ / z-...: 1 s into the aim, a turn of 10 degrees at 20 degrees/s (a sudden jump is ignored as noise)
        double ts = gyro_start_ns ? (now - gyro_start_ns) / 1e9 - 1 : 0, ramp = ts < 0 ? 0 : ts > 0.5 ? 0.5 : ts;
        gyro_yaw = test_fixed ? test_fixed * 20 * ramp * M_PI / 180 : 15 * M_PI / 180 * sin(now / 1e9 * M_PI);
        gyro_pitch = 0;
    } else {
        double k = g_settings.mouse_sens * M_PI / 180 / 1000;  // degrees per pixel
        gyro_yaw -= atomic_exchange(&mouse_acc_x, 0) * k;
        gyro_pitch -= atomic_exchange(&mouse_acc_y, 0) * k;
        double lim = 80 * M_PI / 180;
        if (gyro_pitch > lim) gyro_pitch = lim;
        if (gyro_pitch < -lim) gyro_pitch = -lim;
    }
    double ry[3][3], rp[3][3], r[3][3];
    mat_rot(ry, yaw_axis, gyro_yaw);
    mat_rot(rp, gyro_pitch_axis, gyro_pitch);
    mat_mul(r, ry, rp);
    memset(s, 0, 0x60);
    *(int64_t *)(s + 0x00) = (int64_t)(dt * 1e9);
    *(int64_t *)(s + 0x08) = sampling;
    float *acc = (float *)(s + 0x10), *gyr = (float *)(s + 0x1c), *ang = (float *)(s + 0x28), *dir = (float *)(s + 0x34);
    for (int i = 0; i < 3; i++) acc[i] = (float)-r[2][i];  // gravity (world -z) in the controller's frame: R^T (0,0,-1)
    gyr[yaw_axis] += (float)((gyro_yaw - py) / dt / (2 * M_PI));
    gyr[gyro_pitch_axis] += (float)((gyro_pitch - pp) / dt / (2 * M_PI));
    ang[yaw_axis] += (float)(gyro_yaw / (2 * M_PI));
    ang[gyro_pitch_axis] += (float)(gyro_pitch / (2 * M_PI));
    for (int a = 0; a < 3; a++)  // direction: the controller's x, y, z axes in world coordinates (columns of R)
        for (int i = 0; i < 3; i++) dir[a * 3 + i] = (float)r[i][a];
    *(uint32_t *)(s + 0x58) = 1;  // IsConnected
}
static int64_t six_axis_sampling;
HLE_FUNC("_ZN2nn3hid23GetSixAxisSensorHandlesEPNS0_19SixAxisSensorHandleEiRKjNS_4util10BitFlagSetILi32ENS0_12NpadStyleTagEEE", hid_six_axis_handles) {
    uint32_t *out = (uint32_t *)(uintptr_t)c->x[0];
    int n = (int)c->x[1] < 2 ? (int)c->x[1] : 2;
    uint32_t id = *(uint32_t *)(uintptr_t)c->x[2];
    for (int i = 0; i < n; i++) out[i] = (id & 0xFF) << 8 | (uint32_t)i | 0x04000000u;  // (opaque to the game)
    c->x[0] = (uint64_t)n;
}
HLE_FUNC("_ZN2nn3hid18StartSixAxisSensorERKNS0_19SixAxisSensorHandleE", hid_six_axis_start) {
    // aiming begins (the game measures the turn from here): the virtual controller starts level again, and the
    // mouse becomes the gyro at its next move (input_handle_event)
    gyro_aiming = 1;
    gyro_start_ns = SDL_GetTicksNS();
    gyro_yaw = gyro_pitch = 0;
    atomic_store(&mouse_acc_x, 0);
    atomic_store(&mouse_acc_y, 0);
    extern uint64_t renderer_game_frame(void);
    LOG("[input] six-axis sensor started (frame %llu)\n", (unsigned long long)renderer_game_frame());
}
HLE_FUNC("_ZN2nn3hid17StopSixAxisSensorERKNS0_19SixAxisSensorHandleE", hid_six_axis_stop) {
    gyro_aiming = 0;
    extern uint64_t renderer_game_frame(void);
    if (getenv("LARECOMP_GYRO_TEST")) LOG("[input] six-axis sensor stopped (frame %llu)\n", (unsigned long long)renderer_game_frame());
}
static void six_axis_log(Cpu *c) {  // (LARECOMP_GYRO_TEST: when the game reads it, and from where)
    static int on = -1;
    if (on < 0) on = getenv("LARECOMP_GYRO_TEST") != NULL;
    extern uint64_t renderer_game_frame(void);
    static uint64_t last, n;
    uint64_t f = renderer_game_frame();
    n++;
    if (on && (n <= 8 || f / 30 != last / 30))
        LOG("[gyro] read #%llu at frame %llu (lr %llx)\n", (unsigned long long)n, (unsigned long long)f, (unsigned long long)c->x[30]);
    last = f;
}
HLE_FUNC("_ZN2nn3hid21GetSixAxisSensorStateEPNS0_18SixAxisSensorStateERKNS0_19SixAxisSensorHandleE", hid_six_axis_state) {
    six_axis_log(c);
    six_axis_fill((uint8_t *)(uintptr_t)c->x[0], ++six_axis_sampling);
}
HLE_FUNC("_ZN2nn3hid22GetSixAxisSensorStatesEPNS0_18SixAxisSensorStateEiRKNS0_19SixAxisSensorHandleE", hid_six_axis_states) {
    six_axis_log(c);
    int n = (int)c->x[1];
    if (n > 0) six_axis_fill((uint8_t *)(uintptr_t)c->x[0], ++six_axis_sampling);  // the latest sample only
    c->x[0] = n > 0 ? 1 : 0;
}
HLE_FUNC("_ZN2nn3hid21IsSixAxisSensorAtRestERKNS0_19SixAxisSensorHandleE", hid_six_axis_at_rest) {
    static int n;
    if (getenv("LARECOMP_GYRO_TEST") && n++ < 8) LOG("[gyro] IsSixAxisSensorAtRest (lr %llx)\n", (unsigned long long)c->x[30]);
    c->x[0] = 0;  // never at rest: at rest, the game stopped reading the sensor (a hand-held console never is)
}
HLE_FUNC("_ZN2nn3hid25EnableSixAxisSensorFusionERKNS0_19SixAxisSensorHandleEb", hid_six_axis_fusion) { (void)c; }
HLE_FUNC("_ZN2nn3hid28IsSixAxisSensorFusionEnabledERKNS0_19SixAxisSensorHandleE", hid_six_axis_fusion_on) { c->x[0] = 1; }
HLE_FUNC("_ZN2nn3hid32SetSixAxisSensorFusionParametersERKNS0_19SixAxisSensorHandleEff", hid_six_axis_set_params) { (void)c; }
HLE_FUNC("_ZN2nn3hid32GetSixAxisSensorFusionParametersEPfS1_RKNS0_19SixAxisSensorHandleE", hid_six_axis_get_params) {
    if (c->x[0]) *(float *)(uintptr_t)c->x[0] = 0.03f;
    if (c->x[1]) *(float *)(uintptr_t)c->x[1] = 0.4f;
}
HLE_FUNC("_ZN2nn3hid25SetGyroscopeZeroDriftModeERKNS0_19SixAxisSensorHandleERKNS0_22GyroscopeZeroDriftModeE", hid_gyro_drift_set) { (void)c; }
HLE_FUNC("_ZN2nn3hid25GetGyroscopeZeroDriftModeERKNS0_19SixAxisSensorHandleE", hid_gyro_drift_get) { c->x[0] = 1; }

HLE_FUNC("_ZN2nn3hid25GetVibrationDeviceHandlesEPNS0_21VibrationDeviceHandleEiRKjNS_4util10BitFlagSetILi32ENS0_12NpadStyleTagEEE", hid_vibration_handles) {
    uint32_t *out = (uint32_t *)(uintptr_t)c->x[0];
    int n = (int)c->x[1] < 2 ? (int)c->x[1] : 2;
    uint32_t id = *(uint32_t *)(uintptr_t)c->x[2];
    for (int i = 0; i < n; i++) out[i] = (id & 0xFF) << 8 | (uint32_t)i | 0x03000000u;  // type, player, position
    c->x[0] = (uint64_t)n;
    if (getenv("LARECOMP_RUMBLE_LOG")) LOG("[input] vibration handles for %x: %d\n", id, n);
}
HLE_FUNC("_ZN2nn3hid22GetVibrationDeviceInfoEPNS0_19VibrationDeviceInfoERKNS0_21VibrationDeviceHandleE", hid_vibration_info) {
    uint32_t *info = (uint32_t *)(uintptr_t)c->x[0];
    uint32_t h = *(uint32_t *)(uintptr_t)c->x[1];
    info[0] = 1;        // VibrationDeviceType_LinearResonantActuator
    info[1] = h & 1;    // position: left / right
}
