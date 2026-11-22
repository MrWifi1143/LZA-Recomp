// The mouse in the game's menus: which of the two (the same in both repositories). The default is the menu mouse as
// released (runtime/hle/menu_mouse_classic.c: the system pointer in menus, the mouse let go for a menu with buttons).
// The new pointer (runtime/hle/menu_mouse.c: our own pointer drawn in the game's menus, the window keeps the mouse,
// menu keys of their own) is EXPERIMENTAL: the launcher's "Menu mouse pointer" under Experimental settings
// (larecomp.ini menu_pointer = 1, only with experimental = 1), off by default. Picked once, when the game's code says
// where its menus are (menu_mouse_configure, at the game's start): the two never run together (they hook the same
// functions). LARECOMP_MENU_POINTER=0 / 1 overrides the setting (tests).
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

#include "../hle.h"
#include "../settings.h"

#define BOTH(ret, name, params, args)                       \
    ret mmp_##name params;                                  \
    ret mmc_##name params;                                  \
    ret name params { return g_ptr ? mmp_##name args : mmc_##name args; }

static _Atomic int g_ptr;  // (1: the experimental pointer)

bool menu_pointer_on(void) { return g_ptr; }  // (runtime/hle/input.c: our pointer drawn, the held mouse kept)

BOTH(bool, menu_mouse_active, (void), ())
BOTH(bool, menu_mouse_buttons_up, (void), ())
BOTH(int, menu_cursor_items, (void), ())
BOTH(bool, menu_settings_up, (void), ())
BOTH(bool, menu_item_part, (const char *prefix), (prefix))
BOTH(bool, menu_mouse_value_press, (void), ())
BOTH(uint64_t, menu_mouse_buttons, (Cpu *c), (c))

void mmp_menu_mouse_present(void), mmc_menu_mouse_present(void);
void menu_mouse_present(void) { g_ptr ? mmp_menu_mouse_present() : mmc_menu_mouse_present(); }

// (only the experimental pointer starts on the game's cursor; the released one never asks)
void mmp_menu_mouse_game_cursor(float *x, float *y);
void menu_mouse_game_cursor(float *x, float *y) {
    if (g_ptr) mmp_menu_mouse_game_cursor(x, y);
    else *x = 960, *y = 540;
}

void mmp_menu_mouse_configure(uint64_t, uint64_t, uint64_t, uint32_t, uint32_t);
void mmc_menu_mouse_configure(uint64_t, uint64_t, uint64_t, uint32_t, uint32_t);
void menu_mouse_configure(uint64_t calc_mtx, uint64_t nav_move, uint64_t list_fn, uint32_t list_a, uint32_t list_b) {
    const char *e = getenv("LARECOMP_MENU_POINTER");
    int on = e ? atoi(e) != 0 : g_settings.experimental && g_settings.menu_pointer;
    atomic_store(&g_ptr, on);
    LOG("[menumouse] %s\n", on ? "the experimental menu pointer (our own pointer in menus)" : "the menu mouse as released");
    if (on) mmp_menu_mouse_configure(calc_mtx, nav_move, list_fn, list_a, list_b);
    else mmc_menu_mouse_configure(calc_mtx, nav_move, list_fn, list_a, list_b);
}
