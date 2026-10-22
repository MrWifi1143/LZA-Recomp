// Z-A: the mouse in the game's menus. Pointing at a menu's button moves the game's cursor to it, a click presses it
// (A), the right button goes back (B), the wheel moves up and down - by pressing the d-pad and buttons for the player,
// so every menu reacts the way it does to a controller.
//
// The game's UI is Nintendo's ui2d. Every pane drawn has its screen matrix worked out each frame (0x700264424c,
// (pane, draw info), from the virtual CalculateMtx): those panes are kept, frame by frame. Pane: +0x08 sibling links,
// +0x18 parent, +0x20 its children's list, +0x30 translate, +0x50 size, +0x58 flags (bit 0 visible), +0x5a alpha
// with its parents', +0x70 global matrix (3x4 rows; +0x7c x, +0x8c y: the layout's 1920 x 1080 with 0,0 in the
// middle, y up), +0xb0 name.
//
// The menus share their parts: a selectable button's layout has an "N_cursor" pane where the game puts its cursor
// (part "L_cursor_00", cmn_cursor_00; "L_cursor_box_00"...) when that button is selected. So a menu is up while an
// L_cursor pane is drawn; its buttons are the N_cursor panes drawn in the same layout (the same RootPane: a window
// over a screen has its own; clipped by a scroll list's "S_scissor" pane), each the size of its part (the first
// pane up from the N_cursor with a size; where parts overlap, the nearest centre); the selected one is the button
// whose N_cursor the cursor is on. Moving there is one d-pad press at a time (the
// longer way across the screen: up / down or left / right), each after the cursor has moved for the last; a press
// that doesn't move it ends the move (the menu's edge). On the Settings screen a click on a choice, a point of a
// bar or a tab presses left / right or L / R to it (its rows' values: runtime/hle/za_options.c). Where the player's
// camera isn't turning (a menu without buttons, a dialogue, a cutscene) the pointer is free too: a click is A.
// LARECOMP_ZA_MENU_MOUSE=0: off;
// LARECOMP_ZA_MENU_MOUSE_LOG=1: what it sees, logged ("[menumouse]").
//
// Research: LARECOMP_LIVE's dir, file "panes": the panes of the last frame -> panes.out; file "mouse": pointer
// moves, clicks and wheel turns (runtime/hle/input.c input_live_mouse).
#include <math.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../hle.h"

void rt_register_fnhook_at(uint64_t addr, int (*fn)(Cpu *c, uint64_t addr));
uint64_t renderer_game_frame(void);
// runtime/hle/input.c: the free pointer (1920 x 1080), how often it has moved, the clicks and wheel notches not yet taken
bool input_menu_pointer(int32_t *x, int32_t *y, uint32_t *moves, int *clicks, int *rclicks, int *wheel);

#define ZA_PANE_CALC_MTX 0x700264424cull  // (pane, draw info): the pane's global matrix

enum { BTN_A = 0, BTN_B = 1, BTN_L = 6, BTN_R = 7, BTN_LEFT = 12, BTN_UP = 13, BTN_RIGHT = 14, BTN_DOWN = 15 };

typedef struct {
    uint64_t vt, link[2], parent, children[2];
    float t[3], r[3], s[2], size[2];
    uint8_t flag, alpha, galpha, base;
    uint8_t pad[0x14];
    float m[12];
    uint8_t pad2[0x10];
    char name[24];
} Pane;
_Static_assert(__builtin_offsetof(Pane, m) == 0x70 && __builtin_offsetof(Pane, name) == 0xb0, "pane layout");

#define NPANE 16384
static uint64_t g_panes[2][NPANE];
static _Atomic int g_npanes[2];
static _Atomic uint64_t g_pane_frame;

static int pane_calc_hook(Cpu *c, uint64_t addr) {
    (void)addr;
    uint64_t f = renderer_game_frame(), was = atomic_load(&g_pane_frame);
    if (f != was && atomic_compare_exchange_strong(&g_pane_frame, &was, f)) atomic_store(&g_npanes[f & 1], 0);
    int i = atomic_fetch_add(&g_npanes[f & 1], 1);
    if (i < NPANE) g_panes[f & 1][i] = c->x[0];
    return 0;
}

static bool pane_ok(uint64_t p) { return p >= 0x7000000000ull && p < 0x7800000000ull && !(p & 7); }
static const Pane *parent_of(const Pane *p) { return pane_ok(p->parent) ? (const Pane *)(uintptr_t)p->parent : NULL; }
static bool shown(const Pane *p) {  // (it and its parents visible, not faded out)
    for (int i = 0; p && i < 24; i++, p = parent_of(p))
        if (!(p->flag & 1) || !p->galpha) return false;
    return true;
}
// its rectangle on screen: centre, half size (+0x5b: where its position is - bits 0-1 centre / left / right, 2-3
// centre / top / bottom)
static void pane_rect(const Pane *p, float *cx, float *cy, float *hw, float *hh) {
    *hw = p->size[0] * fabsf(p->m[0]) / 2, *hh = p->size[1] * fabsf(p->m[5]) / 2;
    int h = p->base & 3, v = p->base >> 2 & 3;
    *cx = p->m[3] + (h == 1 ? *hw : h == 2 ? -*hw : 0);
    *cy = p->m[7] + (v == 1 ? -*hh : v == 2 ? *hh : 0);
}
static const Pane *root_of(const Pane *p) {  // (its layout's RootPane: a screen, a window over it...)
    for (int i = 0; p && parent_of(p) && i < 32; i++) p = parent_of(p);
    return p;
}

// ------------------------------------------------------------------ the menu on screen

typedef struct { const Pane *anchor, *part; float ax, ay, cx, cy, hw, hh; } Item;  // anchor, the part: centre, half size
#define NITEM 256
static Item g_items[NITEM];
static int g_nitems;
static float g_cur_x, g_cur_y;  // the cursor
static _Atomic bool g_menu_up;
static uint64_t g_menu_seen;

static void scan_menu(void) {
    uint64_t f = atomic_load(&g_pane_frame);
    const uint64_t *list = g_panes[(f - 1) & 1];
    int n = atomic_load(&g_npanes[(f - 1) & 1]);
    if (n > NPANE) n = NPANE;
    const Pane *cursor = NULL;
    for (int i = 0; i < n; i++) {
        const Pane *p = (const Pane *)(uintptr_t)list[i];
        if (pane_ok(list[i]) && !strncmp(p->name, "L_cursor", 8) && shown(p)) cursor = p;  // (the last drawn: on top)
    }
    g_nitems = 0;
    if (!cursor) return;
    const Pane *box = root_of(cursor);
    g_cur_x = cursor->m[3], g_cur_y = cursor->m[7];
    for (int i = 0; i < n && g_nitems < NITEM; i++) {
        const Pane *p = (const Pane *)(uintptr_t)list[i];
        if (!pane_ok(list[i]) || strcmp(p->name, "N_cursor") || root_of(p) != box || !shown(p)) continue;
        Item *it = &g_items[g_nitems];
        it->anchor = p, it->ax = p->m[3], it->ay = p->m[7];
        const Pane *part = parent_of(p);  // (the button: the first pane up with a size)
        for (int k = 0; part && k < 6 && (part->size[0] <= 0 || part->size[1] <= 0); k++) part = parent_of(part);
        if (!part || part->size[0] <= 0 || part->size[1] <= 0) continue;
        it->part = part;
        pane_rect(part, &it->cx, &it->cy, &it->hw, &it->hh);
        bool clipped = false;  // (a scrolling list's rows outside it)
        for (const Pane *q = part; q && q != box; q = parent_of(q))
            if (!strncmp(q->name, "S_scissor", 9)) {
                float sx, sy, sw, sh;
                pane_rect(q, &sx, &sy, &sw, &sh);
                if (it->cy < sy - sh || it->cy > sy + sh) clipped = true;
                float top = fminf(it->cy + it->hh, sy + sh), bot = fmaxf(it->cy - it->hh, sy - sh);
                it->cy = (top + bot) / 2, it->hh = (top - bot) / 2;
                float rt = fminf(it->cx + it->hw, sx + sw), lt = fmaxf(it->cx - it->hw, sx - sw);
                it->cx = (rt + lt) / 2, it->hw = (rt - lt) / 2;
            }
        if (!clipped && it->hh > 4) g_nitems++;
    }
}

static int selected_item(void) {  // the button the cursor is on, or -1
    int best = -1;
    float bd = 40 * 40;
    for (int i = 0; i < g_nitems; i++) {
        float dx = g_items[i].ax - g_cur_x, dy = g_items[i].ay - g_cur_y, d = dx * dx + dy * dy;
        if (d < bd) bd = d, best = i;
    }
    return best;
}
static int item_at(float x, float y) {  // the button under the pointer, or -1
    int best = -1;
    float bd = 1e30f;
    for (int i = 0; i < g_nitems; i++) {
        const Item *it = &g_items[i];
        if (fabsf(x - it->cx) > it->hw || fabsf(y - it->cy) > it->hh) continue;
        float dx = x - it->cx, dy = y - it->cy, d = dx * dx + dy * dy;
        if (d < bd) bd = d, best = i;
    }
    return best;
}

bool za_menu_mouse_active(void) { return atomic_load(&g_menu_up); }

// A click on the Settings screen's row (runtime/hle/za_options.c): on one of its choices, or along its bar - the
// left / right presses that set that (+ right, - left), 0 for anything else.
bool za_options_cursor_row(Cpu *c, uint64_t *type, int *value);
int za_options_tab(void);  // (-1: the screen isn't up)

// A click on the Settings screen's tabs: the L / R presses to that one (+ R, - L), 0 elsewhere. (Its tab bar: three
// "L_switchitem_0n" parts in "N_switch_00", outside the rows.)
static int tab_presses(float x, float y) {
    uint64_t f = atomic_load(&g_pane_frame);
    const uint64_t *list = g_panes[(f - 1) & 1];
    int n = atomic_load(&g_npanes[(f - 1) & 1]), tab = za_options_tab();
    if (n > NPANE) n = NPANE;
    if (tab < 0) return 0;
    const Pane *t[3];
    int nt = 0;
    for (int i = 0; i < n && nt < 3; i++) {
        const Pane *p = (const Pane *)(uintptr_t)list[i], *up = pane_ok(list[i]) ? parent_of(p) : NULL;
        if (up && !strncmp(p->name, "L_switchitem_0", 14) && !strcmp(up->name, "N_switch_00") && parent_of(up) &&
            !strncmp(parent_of(up)->name, "N_inout", 7) && shown(p))
            t[nt++] = p;
    }
    if (nt != 3) return 0;
    for (int i = 0; i < 3; i++)
        for (int j = i + 1; j < 3; j++)
            if (t[j]->m[3] < t[i]->m[3]) { const Pane *k = t[i]; t[i] = t[j], t[j] = k; }
    for (int i = 0; i < 3; i++) {
        float cx, cy, hw, hh;
        pane_rect(t[i], &cx, &cy, &hw, &hh);
        if (fabsf(x - cx) <= hw && fabsf(y - cy) <= hh) return i - tab;
    }
    return 0;
}
static int value_presses(Cpu *c, const Item *row, float x, float y) {
    uint64_t f = atomic_load(&g_pane_frame);
    const uint64_t *list = g_panes[(f - 1) & 1];
    int n = atomic_load(&g_npanes[(f - 1) & 1]);
    if (n > NPANE) n = NPANE;
    const Pane *seg[8], *bar = NULL;
    int nseg = 0;
    for (int i = 0; i < n; i++) {
        const Pane *p = (const Pane *)(uintptr_t)list[i];
        if (!pane_ok(list[i]) || (strncmp(p->name, "L_switchitem_0", 14) && strncmp(p->name, "P_gauge_bg_0", 12))) continue;
        const Pane *q = p;
        for (int k = 0; q && q != row->part && k < 8; k++) q = parent_of(q);
        if (q != row->part || !shown(p)) continue;
        if (p->name[0] == 'P') bar = p;
        else if (nseg < 8) seg[nseg++] = p;
    }
    if (!bar && !nseg) return 0;
    uint64_t type;
    int value;
    if (!za_options_cursor_row(c, &type, &value)) return 0;
    if ((uint32_t)type == 2 && bar) {  // (a bar: 1..5, or 0..10)
        float bx, by, bw, bh;
        pane_rect(bar, &bx, &by, &bw, &bh);
        float fr = (x - (bx - bw)) / (2 * bw);
        if (fabsf(y - by) > 40 || fr < -0.1f || fr > 1.1f) return 0;
        fr = fr < 0 ? 0 : fr > 1 ? 1 : fr;
        int to = type >> 32 ? (int)lroundf(fr * 10) : 1 + (int)lroundf(fr * 4);
        return to - value;
    }
    if ((uint32_t)type > 1) return 0;
    for (int i = 0; i < nseg; i++)  // (left to right: the values in order)
        for (int j = i + 1; j < nseg; j++)
            if (seg[j]->m[3] < seg[i]->m[3]) { const Pane *t = seg[i]; seg[i] = seg[j], seg[j] = t; }
    for (int i = 0; i < nseg; i++) {
        float sx, sy, hw, hh;
        pane_rect(seg[i], &sx, &sy, &hw, &hh);
        if (fabsf(x - sx) <= hw && fabsf(y - sy) <= hh) return i - value;
    }
    return 0;
}

// ------------------------------------------------------------------ pressing

static bool g_on;
static const Pane *g_target;  // the button to move to (its N_cursor)
static bool g_target_a;       // and press there
static int g_steps;
static uint64_t g_press_bit, g_press_until, g_wait_until;  // a press held until, then let go and the cursor watched
static float g_press_from_x, g_press_from_y;
static int g_queue_a, g_queue_b, g_queue_wheel, g_queue_value, g_queue_tab;  // (value: right presses, or left when
                                                                            // < 0; tab: R / L)
static bool g_value_press;
static float g_click_x, g_click_y;

static void press(int bit, uint64_t f) {
    g_press_bit = 1ull << bit, g_press_until = f + 3, g_wait_until = f + 3 + 24;
    g_press_from_x = g_cur_x, g_press_from_y = g_cur_y;
}

// Each read of the controller: the buttons the mouse presses (worked out once a frame)
uint64_t za_menu_mouse_buttons(Cpu *c) {
    if (!g_on) return 0;
    static uint64_t last_f = ~0ull, out;
    uint64_t f = renderer_game_frame();
    if (f == last_f) return out;
    last_f = f;
    scan_menu();
    extern uint64_t g_za_field_camera_frame;  // (runtime/hle/za_fixes.c)
    if (g_nitems || f - g_za_field_camera_frame > 8) g_menu_seen = f;  // (or the player's camera stopped: a menu without
    bool up = f - g_menu_seen < 10;                                    // buttons, a dialogue, a cutscene)
    atomic_store(&g_menu_up, up);
    int32_t px, py;
    uint32_t moves;
    int clicks, rclicks, wheel;
    bool have = input_menu_pointer(&px, &py, &moves, &clicks, &rclicks, &wheel);
    static bool dbg = false, dbg_init;
    if (!dbg_init) dbg_init = true, dbg = getenv("LARECOMP_ZA_MENU_MOUSE_LOG") != NULL;
    if (dbg && (clicks || rclicks || wheel || (f % 60) == 0))
        LOG("[menumouse] up %d have %d at %d,%d moves %u clicks %d/%d wheel %d items %d sel %d target %p press %llx\n", up, have,
            px, py, moves, clicks, rclicks, wheel, g_nitems, selected_item(), (const void *)g_target, (unsigned long long)g_press_bit);
    if (!have || !up) {
        g_target = NULL, g_press_bit = 0, g_queue_a = g_queue_b = g_queue_wheel = g_queue_value = g_queue_tab = 0;
        return out = 0;
    }
    float x = (float)px - 960, y = 540 - (float)py;
    static uint32_t last_moves;
    bool moved = moves != last_moves;
    last_moves = moves;
    int sel = selected_item(), hov = g_nitems ? item_at(x, y) : -1;
    if (moved && hov >= 0 && hov == sel && !g_target_a) g_target = NULL;  // (back on the selected one)
    else if (moved && hov >= 0 && g_items[hov].anchor != g_target) g_target = g_items[hov].anchor, g_target_a = false, g_steps = 0;
    if (clicks > 0 && hov >= 0) g_target = g_items[hov].anchor, g_target_a = true, g_steps = 0, g_click_x = x, g_click_y = y;
    if (clicks > 0 && !g_nitems) g_queue_a += clicks;  // (no buttons: a click is A)
    if (clicks > 0 && g_nitems && hov < 0) g_queue_tab += tab_presses(x, y);
    g_queue_b += rclicks, g_queue_wheel += wheel;

    if (g_press_bit && f < g_press_until) return out = g_press_bit;  // (held)
    if (g_press_bit && g_value_press) {  // (a value's left / right: the cursor stays)
        if (f < g_wait_until) return out = 0;
        g_press_bit = 0, g_value_press = false;
    }
    if (g_press_bit) {  // (let go: until the cursor moves)
        bool went = fabsf(g_cur_x - g_press_from_x) > 2 || fabsf(g_cur_y - g_press_from_y) > 2;
        if (!went && f < g_wait_until) return out = 0;
        bool dir = g_press_bit & (1ull << BTN_UP | 1ull << BTN_DOWN | 1ull << BTN_LEFT | 1ull << BTN_RIGHT);
        if (dir && !went && g_target) g_target = NULL;  // (the menu's edge: it doesn't go there)
        g_press_bit = 0;
        if (went) return out = 0;  // (one more frame: it may still be sliding)
    }
    if (g_queue_b > 0) { g_queue_b--, g_target = NULL, press(BTN_B, f); return out = g_press_bit; }
    if (g_queue_wheel) {
        bool down = g_queue_wheel < 0;
        g_queue_wheel += down ? 1 : -1;
        g_target = NULL, press(down ? BTN_DOWN : BTN_UP, f);
        return out = g_press_bit;
    }
    if (g_queue_value || g_queue_tab) {
        int *q = g_queue_value ? &g_queue_value : &g_queue_tab;
        int bit = q == &g_queue_tab ? (*q > 0 ? BTN_R : BTN_L) : (*q > 0 ? BTN_RIGHT : BTN_LEFT);
        *q += *q > 0 ? -1 : 1;
        g_press_bit = 1ull << bit, g_press_until = f + 3, g_wait_until = f + (q == &g_queue_tab ? 24 : 7), g_value_press = true;
        return out = g_press_bit;
    }
    if (g_queue_a > 0) { g_queue_a--, g_target = NULL, press(BTN_A, f); return out = g_press_bit; }
    if (!g_target) return out = 0;
    int t = -1;
    for (int i = 0; i < g_nitems; i++) if (g_items[i].anchor == g_target) t = i;
    if (t < 0 || sel < 0) { if (t < 0) g_target = NULL; return out = 0; }  // (gone; or the cursor between buttons)
    if (t == sel) {
        int v = g_target_a ? value_presses(c, &g_items[t], g_click_x, g_click_y) : 0;
        if (v) g_queue_value = v;
        else if (g_target_a) press(BTN_A, f);
        g_target = NULL;
        return out = g_press_bit;
    }
    if (++g_steps > 40) { g_target = NULL; return out = 0; }
    float dx = g_items[t].ax - g_items[sel].ax, dy = g_items[t].ay - g_items[sel].ay;
    press(fabsf(dy) >= fabsf(dx) ? (dy > 0 ? BTN_UP : BTN_DOWN) : (dx > 0 ? BTN_RIGHT : BTN_LEFT), f);
    return out = g_press_bit;
}

// ------------------------------------------------------------------ research

static void dump_panes(const char *path) {
    uint64_t f = atomic_load(&g_pane_frame);
    int n = atomic_load(&g_npanes[(f - 1) & 1]);
    if (n > NPANE) n = NPANE;
    FILE *o = fopen(path, "w");
    if (!o) return;
    for (int i = 0; i < n; i++) {
        const Pane *p = (const Pane *)(uintptr_t)g_panes[(f - 1) & 1][i];
        fprintf(o, "%llx parent %llx %-24.24s flag %02x a %3d base %02x size %7.1f %7.1f at %8.1f %8.1f scale %.3f %.3f\n",
                (unsigned long long)(uintptr_t)p, (unsigned long long)p->parent, p->name, p->flag, p->galpha, p->base,
                p->size[0], p->size[1], p->m[3], p->m[7], p->m[0], p->m[5]);
        if (strncmp(p->name, "N_cursor", 8) && strncmp(p->name, "L_cursor", 8)) continue;
        for (const Pane *q = parent_of(p); q; q = parent_of(q))
            fprintf(o, "    ^ %llx %-24.24s flag %02x a %3d base %02x size %7.1f %7.1f at %8.1f %8.1f\n", (unsigned long long)(uintptr_t)q,
                    q->name, q->flag, q->galpha, q->base, q->size[0], q->size[1], q->m[3], q->m[7]);
    }
    for (int i = 0; i < g_nitems; i++)
        fprintf(o, "item %d anchor %.0f %.0f rect %.0f %.0f +- %.0f %.0f\n", i, g_items[i].ax, g_items[i].ay, g_items[i].cx, g_items[i].cy,
                g_items[i].hw, g_items[i].hh);
    fprintf(o, "cursor %.0f %.0f selected %d\n", g_cur_x, g_cur_y, selected_item());
    fclose(o);
}

void za_mouse_present(void) {
    static const char *dir;
    static bool init;
    if (!init) init = true, dir = getenv("LARECOMP_LIVE");
    if (!dir) return;
    char path[512], out[512];
    snprintf(path, sizeof path, "%s/mouse", dir);  // (runtime/hle/input.c input_live_mouse: a line each)
    FILE *f = fopen(path, "r");
    if (f) {
        extern void input_live_mouse(const char *cmd);
        char line[128];
        while (fgets(line, sizeof line, f)) input_live_mouse(line);
        fclose(f);
        remove(path);
    }
    snprintf(path, sizeof path, "%s/panes", dir);
    f = fopen(path, "r");
    if (!f) return;
    fclose(f);
    remove(path);
    snprintf(path, sizeof path, "%s/panes.tmp", dir);
    snprintf(out, sizeof out, "%s/panes.out", dir);
    dump_panes(path);
    rename(path, out);
}

void za_mouse_configure(void) {
    const char *e = getenv("LARECOMP_ZA_MENU_MOUSE");
    if (e && atoi(e) == 0) return;
    g_on = true;
    rt_register_fnhook_at(ZA_PANE_CALC_MTX, pane_calc_hook);
}
