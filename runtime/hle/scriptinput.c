// Scripts' keyboard and mouse (runtime/hud.h; the mod API's input_* - Legends Lua's on("key") / on("mouse") /
// input.mouse_pos, its menus): the window's events, as the runtime's own menu doesn't take them (ui/menu.mm first), kept
// for the plugin to read each frame. A script's menu holds the game's input (input_grab): the game then sees a
// controller at rest (hle/input.c, as with the runtime's own menu), the mouse is let go (no camera turning, no clicks for
// the game), Esc goes to the script instead of opening the runtime's menu (F1 still does) - the controller still reaches
// scripts (the mod API's buttons).
#include <SDL3/SDL.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../hle.h"
#include "../hud.h"

static _Atomic int g_grab, g_want;  // (g_want: a script reads input - events kept)
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
#define NEV 256
static HudInputEvent g_ev[NEV];
static int g_head, g_n;
static float g_mx = -1, g_my = -1;
static _Atomic uint32_t g_mbuttons;

bool scriptinput_grabbed(void) { return atomic_load(&g_grab) != 0; }

static void push(const HudInputEvent *e) {
    pthread_mutex_lock(&g_lock);
    if (g_n == NEV) g_head = (g_head + 1) % NEV, g_n--;  // (nobody read them: the oldest go)
    g_ev[(g_head + g_n) % NEV] = *e;
    g_n++;
    pthread_mutex_unlock(&g_lock);
}

static float density(uint32_t window_id) {  // (window points -> the overlay's pixels)
    SDL_Window *w = SDL_GetWindowFromID(window_id);
    float d = w ? SDL_GetWindowPixelDensity(w) : 1.0f;
    return d > 0.1f ? d : 1.0f;
}

// Each window event the runtime's menu didn't take (nvn/vulkan/device.c). True: it's the script's (a held menu's):
// the game doesn't get it.
bool scriptinput_event(const SDL_Event *e) {
    bool grab = atomic_load(&g_grab) != 0;
    if (!atomic_load(&g_want) && !grab) return false;
    if (grab) {  // (a mouse the game window held - turning the camera - is let go: the pointer is the menu's)
        extern void input_mouse_let_go(void);
        input_mouse_let_go();
    }
    HudInputEvent ev;
    memset(&ev, 0, sizeof ev);
    switch (e->type) {
    case SDL_EVENT_KEY_DOWN: case SDL_EVENT_KEY_UP:
        ev.type = e->type == SDL_EVENT_KEY_DOWN ? HUD_KEY_DOWN : HUD_KEY_UP;
        ev.repeat = e->key.repeat ? 1 : 0, ev.mods = (uint16_t)e->key.mod;
        snprintf(ev.name, sizeof ev.name, "%s", SDL_GetKeyName(e->key.key));
        ev.x = g_mx, ev.y = g_my;
        push(&ev);
        return false;  // (keys still reach the game's bindings: held back from the game by hle/input.c while grabbed)
    case SDL_EVENT_TEXT_INPUT:
        ev.type = HUD_TEXT_INPUT;
        snprintf(ev.name, sizeof ev.name, "%s", e->text.text);
        push(&ev);
        return false;
    case SDL_EVENT_MOUSE_MOTION: {
        float d = density(e->motion.windowID);
        g_mx = e->motion.x * d, g_my = e->motion.y * d;
        return grab;
    }
    case SDL_EVENT_MOUSE_BUTTON_DOWN: case SDL_EVENT_MOUSE_BUTTON_UP: {
        float d = density(e->button.windowID);
        g_mx = e->button.x * d, g_my = e->button.y * d;
        bool down = e->type == SDL_EVENT_MOUSE_BUTTON_DOWN;
        ev.type = down ? HUD_MOUSE_DOWN : HUD_MOUSE_UP, ev.button = e->button.button, ev.x = g_mx, ev.y = g_my;
        uint32_t bit = e->button.button == SDL_BUTTON_LEFT ? 1 : e->button.button == SDL_BUTTON_RIGHT ? 2 : e->button.button == SDL_BUTTON_MIDDLE ? 4 : 0;
        if (down) atomic_fetch_or(&g_mbuttons, bit);
        else atomic_fetch_and(&g_mbuttons, ~bit);
        push(&ev);
        return grab;
    }
    case SDL_EVENT_MOUSE_WHEEL:
        ev.type = HUD_WHEEL, ev.wheel = e->wheel.y, ev.x = g_mx, ev.y = g_my;
        push(&ev);
        return grab;
    default: return false;
    }
}

// ---- the mod API (any thread)
void scriptinput_grab(int on) {
    int was = atomic_exchange(&g_grab, on ? 1 : 0);
    atomic_store(&g_want, 1);
    if (was != (on != 0)) {
        LOG("[input] a script's menu %s the game's input\n", on ? "holds" : "lets go of");
    }
}
int scriptinput_events(void *out, int max) {
    atomic_store(&g_want, 1);
    HudInputEvent *o = out;
    pthread_mutex_lock(&g_lock);
    int n = 0;
    while (g_n > 0 && n < max) o[n++] = g_ev[g_head], g_head = (g_head + 1) % NEV, g_n--;
    pthread_mutex_unlock(&g_lock);
    return n;
}
void scriptinput_mouse(float out[3]) {
    atomic_store(&g_want, 1);
    out[0] = g_mx, out[1] = g_my, out[2] = (float)atomic_load(&g_mbuttons);
}

// (testing) LARECOMP_LIVE=<dir>: <dir>/sdl, lines "key <SDL key name>" (pressed and let go), "down / up <key>", "type
// <text>", "move <x> <y>", "click <x> <y> [left|right|middle]", "wheel <n>" - window events made from them (overlay
// pixels, as the window's), for runs without a person at the keyboard; the file is removed once read. Game thread.
void scriptinput_live_poll(uint64_t frame) {
    static const char *dir = (const char *)1;
    if (dir == (const char *)1) dir = getenv("LARECOMP_LIVE");
    if (!dir || frame % 4) return;
    char p[1100], line[300];
    snprintf(p, sizeof p, "%s/sdl", dir);
    FILE *f = fopen(p, "r");
    if (!f) return;
    SDL_Window *w = NULL;
    {
        int n = 0;
        SDL_Window **ws = SDL_GetWindows(&n);
        if (ws && n > 0) w = ws[0];
        SDL_free(ws);
    }
    SDL_WindowID wid = w ? SDL_GetWindowID(w) : 0;
    float d = w ? SDL_GetWindowPixelDensity(w) : 1.0f;
    if (d < 0.1f) d = 1.0f;
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        char a[64] = "", b[200] = "";
        float x = 0, y = 0, n = 0;
        SDL_Event e;
        memset(&e, 0, sizeof e);
        if (sscanf(line, "type %199[^\n]", b) == 1) {
            e.type = SDL_EVENT_TEXT_INPUT, e.text.windowID = wid;
            e.text.text = SDL_strdup(b);  // (SDL frees what it's given? it copies: kept small, once)
            SDL_PushEvent(&e);
        } else if (sscanf(line, "%63s %199[^\n]", a, b) == 2 && (!strcmp(a, "key") || !strcmp(a, "down") || !strcmp(a, "up"))) {
            SDL_Scancode sc = SDL_GetScancodeFromName(b);
            if (sc == SDL_SCANCODE_UNKNOWN) { LOG("[input] live sdl: no key \"%s\"\n", b); continue; }
            for (int k = 0; k < 2; k++) {
                if ((k == 0 && !strcmp(a, "up")) || (k == 1 && !strcmp(a, "down"))) continue;
                e.type = k == 0 ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
                e.key.windowID = wid, e.key.scancode = sc, e.key.key = SDL_GetKeyFromScancode(sc, SDL_KMOD_NONE, false), e.key.down = k == 0;
                SDL_PushEvent(&e);
            }
        } else if (sscanf(line, "move %f %f", &x, &y) == 2) {
            e.type = SDL_EVENT_MOUSE_MOTION, e.motion.windowID = wid, e.motion.x = x / d, e.motion.y = y / d;
            SDL_PushEvent(&e);
        } else if (sscanf(line, "click %f %f %63s", &x, &y, a) >= 2) {
            Uint8 bt = !strcmp(a, "right") ? SDL_BUTTON_RIGHT : !strcmp(a, "middle") ? SDL_BUTTON_MIDDLE : SDL_BUTTON_LEFT;
            e.type = SDL_EVENT_MOUSE_MOTION, e.motion.windowID = wid, e.motion.x = x / d, e.motion.y = y / d;
            SDL_PushEvent(&e);
            for (int k = 0; k < 2; k++) {
                memset(&e, 0, sizeof e);
                e.type = k == 0 ? SDL_EVENT_MOUSE_BUTTON_DOWN : SDL_EVENT_MOUSE_BUTTON_UP;
                e.button.windowID = wid, e.button.button = bt, e.button.down = k == 0, e.button.clicks = 1, e.button.x = x / d, e.button.y = y / d;
                SDL_PushEvent(&e);
            }
        } else if (sscanf(line, "wheel %f", &n) == 1) {
            e.type = SDL_EVENT_MOUSE_WHEEL, e.wheel.windowID = wid, e.wheel.y = n;
            SDL_PushEvent(&e);
        }
    }
    fclose(f);
    remove(p);
}
