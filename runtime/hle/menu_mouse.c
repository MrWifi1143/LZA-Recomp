// The mouse in the game's menus (the Legends games: this file is the same in both repositories, configured with the
// game's address). Pointing at a menu's button moves the game's cursor to it, a click presses it (A), the right
// button goes back (B), the wheel moves up and down - by pressing the d-pad and buttons for the player, so every menu
// reacts the way it does to a controller.
//
// The game's UI is Nintendo's ui2d. Every pane drawn has its screen matrix worked out each frame (menu_mouse_configure's
// function, (pane, draw info), from the virtual CalculateMtx; Z-A 0x700264424c): those of the last 3 frames are
// the screen (Arceus doesn't work out every layout on every frame). Pane: +0x08 sibling links, +0x18 parent, +0x20 its children's list, +0x30 translate, +0x50 size, +0x58 flags
// (bit 0 visible), +0x5a alpha with its parents', +0x70 global matrix (3x4 rows; +0x7c x, +0x8c y: the layout's
// 1920 x 1080 with 0,0 in the middle, y up), +0xb0 name.
//
// The menus share their parts: a selectable button's layout has an "N_cursor" pane where the game puts its cursor
// (part "L_cursor_00", cmn_cursor_00; "L_cursor_box_00"...) when that button is selected. So a menu is up while an
// L_cursor pane is drawn; its buttons are the N_cursor panes drawn in the same layout (the same RootPane: a window
// over a screen has its own; clipped by a scroll list's "S_scissor" pane), each the size of its part (the first
// pane up from the N_cursor with a size; where parts overlap, the nearest centre); the selected one is the button
// whose N_cursor the cursor is on. Moving there is one d-pad press at a time (the longer way across the screen: up /
// down or left / right), each after the cursor has moved for the last; a press that doesn't move it ends the move
// (the menu's edge). On the Settings screen a click on a choice, a point of a bar or a tab presses left / right or
// L / R to it (its rows' values: the game's options file - settings_cursor_row, settings_tab). Where the player's
// camera isn't turning (g_field_camera_frame, set by the game's camera hook: a menu without buttons, a dialogue, a
// cutscene) the pointer is free too: a click is A. LARECOMP_MENU_MOUSE=0: off; LARECOMP_MENU_MOUSE_LOG=1: what it
// sees, logged ("[menumouse]").
//
// Research: LARECOMP_LIVE's dir, file "panes": the panes of the last frame -> panes.out; file "mouse": pointer
// moves, clicks and wheel turns (runtime/hle/input.c input_live_mouse).
// THE NEW MENU POINTER (experimental, off by default): our own pointer drawn in the game's menus, the window keeps the
// mouse, menu keys of their own (the rework from the lowend branch, 2026-11-15/16).
// runtime/hle/menu_mouse_pick.c picks one of the two at start: the launcher's experimental "Menu mouse pointer"
// (larecomp.ini menu_pointer = 1, with experimental = 1) - this file's names carry a prefix, the picker has the plain ones.
#define menu_mouse_active mmp_menu_mouse_active
#define menu_mouse_buttons_up mmp_menu_mouse_buttons_up
#define menu_mouse_game_cursor mmp_menu_mouse_game_cursor
#define menu_cursor_items mmp_menu_cursor_items
#define menu_settings_up mmp_menu_settings_up
#define menu_item_part mmp_menu_item_part
#define menu_mouse_value_press mmp_menu_mouse_value_press
#define menu_mouse_buttons mmp_menu_mouse_buttons
#define menu_mouse_present mmp_menu_mouse_present
#define menu_mouse_configure mmp_menu_mouse_configure
#include <math.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../hle.h"

void rt_register_fnhook_at(uint64_t addr, int (*fn)(Cpu *c, uint64_t addr));
void rt_call_original(Cpu *c, uint64_t addr);
uint64_t renderer_game_frame(void);
// runtime/hle/input.c: the free pointer (1920 x 1080), how often it has moved, the clicks and wheel notches not yet taken
bool input_menu_pointer(int32_t *x, int32_t *y, uint32_t *moves, int *clicks, int *rclicks, int *wheel);


enum { BTN_A = 0, BTN_B = 1, BTN_L = 6, BTN_R = 7, BTN_ZL = 8, BTN_ZR = 9, BTN_LEFT = 12, BTN_UP = 13, BTN_RIGHT = 14, BTN_DOWN = 15 };

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

// The panes worked out lately (Arceus doesn't do every layout on every frame): a table by pane, with the frame and
// the order it was last done in; g_list (rebuilt once a frame, scan_menu) has those of the last 3 frames, in order.
#define NSEEN 32768
#define NPANE 16384
static struct { _Atomic uint64_t pane; uint64_t frame, seq; } g_seen[NSEEN];
static _Atomic uint64_t g_seq;
static uint64_t g_list[NPANE];
static int g_nlist;

// (a game's own look at each pane as it is worked out - Z-A: za_options.c hides the title screen's eShop banner)
void (*g_menu_pane_cb)(uint64_t pane);

static int pane_calc_hook(Cpu *c, uint64_t addr) {
    (void)addr;
    uint64_t p = c->x[0], f = renderer_game_frame();
    if (g_menu_pane_cb) g_menu_pane_cb(p);
    uint32_t h = (uint32_t)((p >> 4) * 0x9E3779B1u) & (NSEEN - 1);
    for (int k = 0; k < 64; k++, h = (h + 1) & (NSEEN - 1)) {
        uint64_t cur = atomic_load(&g_seen[h].pane);
        if (cur != p && !(cur == 0 || f - g_seen[h].frame > 30)) continue;  // (another pane's; an old one's is reused)
        if (cur != p) atomic_store(&g_seen[h].pane, p);
        g_seen[h].frame = f, g_seen[h].seq = atomic_fetch_add(&g_seq, 1);
        break;
    }
    return 0;
}

static int by_seq(const void *a, const void *b) {
    uint64_t x = ((const uint64_t *)a)[1], y = ((const uint64_t *)b)[1];
    return x < y ? -1 : x > y;
}
static void gather(void) {  // (g_list: the panes done in the last 3 frames, in the order they were done)
    static uint64_t tmp[NPANE][2];
    uint64_t f = renderer_game_frame();
    int n = 0;
    for (int i = 0; i < NSEEN && n < NPANE; i++) {
        uint64_t p = atomic_load(&g_seen[i].pane);
        if (p && f - g_seen[i].frame <= 3) tmp[n][0] = p, tmp[n][1] = g_seen[i].seq, n++;
    }
    qsort(tmp, (size_t)n, sizeof tmp[0], by_seq);
    for (int i = 0; i < n; i++) g_list[i] = tmp[i][0];
    g_nlist = n;
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

typedef struct { const Pane *anchor, *part; float ax, ay, cx, cy, hw, hh; bool listed; int idx; float sx, sy, sw, sh; } Item;  // (s*: a list's clip)  // anchor, the part: centre,
                                                                                               // half size; in a scroll list
#define NITEM 256
static Item g_items[NITEM];
static int g_nitems;
static float g_cur_x, g_cur_y;  // the cursor
static bool g_cur_free;  // (it's between the buttons: on a place of the menu that isn't one - see scan_menu)
static bool g_settings_up;      // the Settings screen is the one with the cursor (its description line drawn)
static _Atomic bool g_menu_up;
static _Atomic bool g_buttons_up;  // a menu with buttons (N_cursor panes) is on screen: one the pointer clicks
static uint64_t g_menu_seen;

static bool is_cursor(const Pane *p) {  // (Z-A: the part "L_cursor_00" / "L_cursor_box_00"...; Arceus: its own small
    // layout, moved onto the button - picture "P_cursor_00" in "N_select_00")
    if (!strncmp(p->name, "L_cursor", 8)) return strncmp(p->name, "L_cursor_l_", 11) && strncmp(p->name, "L_cursor_r_", 11);
    const Pane *up = parent_of(p);
    return !strcmp(p->name, "P_cursor_00") && up && !strcmp(up->name, "N_select_00");
}

// a button part's number, as the menu counts its buttons: "L_menu_button_03" -> 3 ("..._Copy2": a list's pooled row: -1)
static int name_index(const char *nm) {
    size_t n = strnlen(nm, 24);
    if (n < 3 || nm[n - 3] != '_' || nm[n - 2] < '0' || nm[n - 2] > '9' || nm[n - 1] < '0' || nm[n - 1] > '9') return -1;
    return (nm[n - 2] - '0') * 10 + (nm[n - 1] - '0');
}

// Hover selects at once: most of the games' menus move their cursor with a navigator (one class in both games; its
// core: Z-A 0x7000fe5720, Arceus 0x700272b6d0 - (nav, &direction, &status) -> the new cell; nav +8 the index, +0xc /
// +0x10 columns / rows, +0x1c column-major). A press towards the hovered button is still made - the menu's own input
// path, animations and sounds - and the core's answer is the hovered button instead of the next cell: by its part's
// number when the parts are numbered as the navigator counts (the selected one's number is its index), else by the
// rows and columns between the two on screen (a list whose parts are reused as it scrolls).
static struct { _Atomic int idx; uint64_t frame; int dcol, drow, from; bool grid, horiz; } g_jump = {-1, 0, 0, 0, -1, false, false};
// The d-pad press going on is the pointer's (a hover's step): the menus wrap around as for a controller - up on the top
// row goes to the bottom - and a step of the pointer's at an edge sent the selection to the far end. A move of more
// than one cell that isn't the pointer's own jump is a wrap: the selection stays.
static _Atomic int g_ptr_dpad;  // (its button + 1)
// (a step from one part of the screen to another - a click on an item while the cursor is on the Pokémon: the move
// leaves the grid the edges are known for, so it isn't a wrap - the Satchel's row down, 5 slots, went back as one
// from the party's 1-column list, again and again)
static _Atomic bool g_crossing;
static int nav_move_hook(Cpu *c, uint64_t addr) {  // (the navigator's core: (nav, &direction, &status) -> x0 the new
    uint64_t nav = c->x[0];                          // cell, low / high 32 bits as the flag at +0x1c says)
    const int32_t *n = (const int32_t *)(uintptr_t)nav;
    int want = atomic_load(&g_jump.idx), from = n[2];
    rt_call_original(c, addr);
    uint64_t f = renderer_game_frame();
    static int log = -1;
    if (log < 0) log = getenv("LARECOMP_MENU_MOUSE_LOG") != NULL;
    int cols = n[3], rows = n[4];
    if (atomic_load(&g_ptr_dpad) && !atomic_load(&g_crossing) && cols > 0 && rows > 0) {  // (the pointer's step: no wrapping around)
        uint32_t glo = (uint32_t)c->x[0], ghi = (uint32_t)(c->x[0] >> 32);
        int g = n[7] ? (int)(rows * glo + ghi) : (int)(cols * ghi + glo);
        int fc = n[7] ? from / rows : from % cols, fr = n[7] ? from % rows : from / cols;
        int gc = n[7] ? g / rows : g % cols, gr = n[7] ? g % rows : g / cols;
        bool jumping = want >= 0 && f - g_jump.frame <= 30;
        if (!jumping && (abs(gc - fc) > 1 || abs(gr - fr) > 1)) {
            c->x[0] = n[7] ? ((uint64_t)(uint32_t)fr << 32 | (uint32_t)fc) : ((uint64_t)(uint32_t)fr << 32 | (uint32_t)fc);
            return 1;
        }
    }
    if (getenv("LARECOMP_MENU_NAV_TRACE")) LOG("[menumouse] TRACE nav %llx from %d grid %dx%d want %d\n", (unsigned long long)nav, from, cols, rows, want);  // (LARECOMP_MENU_NAV_TRACE=1: every move asked, research)
    if (want < 0 || f - g_jump.frame > 30 || cols <= 0 || rows <= 0) return 1;
    uint32_t lo = (uint32_t)c->x[0], hi = (uint32_t)(c->x[0] >> 32);
    int got = n[7] ? (int)(rows * lo + hi) : (int)(cols * hi + lo);
    if (got == from) return 1;  // (not a move: a query, or the menu's edge)
    bool by_name = !g_jump.grid && g_jump.from == from;
    if (!by_name) {
        int col = n[7] ? from / rows : from % cols, row = n[7] ? from % rows : from / cols;
        col += g_jump.dcol, row += g_jump.drow;
        if (col < 0 || row < 0 || col >= cols || row >= rows) {  // (off the grid: no move - not the game's wrap)
            if (atomic_load(&g_ptr_dpad)) {
                int fc = n[7] ? from / rows : from % cols, fr = n[7] ? from % rows : from / cols;
                c->x[0] = (uint64_t)(uint32_t)fr << 32 | (uint32_t)fc;
            }
            return 1;
        }
        want = n[7] ? col * rows + row : row * cols + col;
    } else if (want >= cols * rows) return 1;
    if (log) LOG("[menumouse] nav %llx %d -> %d, made %d (grid %dx%d)\n", (unsigned long long)nav, from, got, want, cols, rows);
    lo = n[7] ? (uint32_t)(want / rows) : (uint32_t)(want % cols);
    hi = n[7] ? (uint32_t)(want % rows) : (uint32_t)(want / cols);
    c->x[0] = (uint64_t)hi << 32 | lo;
    return 1;  // (the jump stays for the frame: a menu may ask the core twice for one press)
}

// Scroll lists (the Settings screens, Arceus's Satchel) move their cursor with their own "select cell" (Z-A
// 0x7000fc2630, Arceus 0x7002719878: (list, a, b), the current a / b in the list at the configured offsets): a the slot
// in reading order (the Satchel: 5 a row - down is +5), b how far it's scrolled. A one-step move asked while a hover jump
// waits becomes a move to the hovered slot (the rows and columns between the selected and hovered slots on screen).
static uint64_t g_list_fn;
static uint32_t g_list_off[2];
static _Atomic int g_list_cols = 1;  // (the most buttons side by side in a row of the menu on screen: a row's slots)
// A scroll list wraps round past its ends - and the list scrolls to the far end outside its select cell (refusing the
// move there left the view at the top and the cursor at the bottom). The pointer's press wrapping round goes, and the
// press back is made at once (g_unwrap_bit); the list's last row is remembered, and its first is a = 0, b = 0: the
// wheel stops there.
static _Atomic uint64_t g_list_obj, g_list_hit;  // (the list whose cell was selected last, the frame)
// A list that keeps its cursor in the middle (Arceus's Settings: slot 4 of 9, the rows scroll under it): a hover there
// selected a row and the list scrolled it to the middle - away from the pointer, onto the next row's... The rows slid
// under a resting pointer. Learned when the cursor stays and the rows scroll with rows still beyond it; a hover doesn't
// select in it (a click does, the wheel steps).
static _Atomic uint64_t g_center_obj;
static int selected_item(void);
static uint64_t g_end_obj;
static int g_end_a, g_end_b;
static _Atomic int g_unwrap_bit = -1;
static _Atomic bool g_unwrapping;
static int list_select_hook(Cpu *c, uint64_t addr) {
    int want = atomic_load(&g_jump.idx);
    atomic_store(&g_list_obj, c->x[0]), atomic_store(&g_list_hit, renderer_game_frame());
    if (getenv("LARECOMP_MENU_NAV_TRACE")) LOG("[menumouse] TRACE list %llx a %d b %d want %d\n", (unsigned long long)c->x[0], (int)c->x[1], (int)c->x[2], want);  // (LARECOMP_MENU_NAV_TRACE=1: every move asked, research)
    bool jumping = want >= 0 && renderer_game_frame() - g_jump.frame <= 30;
    const int32_t *l = (const int32_t *)(uintptr_t)c->x[0];
    int ca = l[g_list_off[0] / 4], cb = l[g_list_off[1] / 4], a = (int32_t)c->x[1], b = (int32_t)c->x[2];
    int da = a - ca, db = b - cb, cols = atomic_load(&g_list_cols);
    if (!da && abs(db) == 1 && ca > 0) {  // (scrolled with the cursor staying: rows beyond it on the side it scrolls to?)
        int sel = selected_item(), beyond = 0;
        for (int i = 0; sel >= 0 && i < g_nitems; i++)
            beyond += root_of(g_items[i].anchor) == root_of(g_items[sel].anchor) && (db > 0 ? g_items[i].ay < g_items[sel].ay - 4 : g_items[i].ay > g_items[sel].ay + 4);
        if (beyond >= 2) atomic_store(&g_center_obj, c->x[0]);
    }
    if (cols < 1) cols = 1;
    // (one step: a slot along the row or a row down / up - or the list scrolled a row)
    bool step = (db == 0 && (abs(da) == 1 || abs(da) == cols)) || (abs(db) == 1 && abs(da) <= cols);
    int dpad = atomic_load(&g_ptr_dpad);
    if (dpad && !jumping && (da || db) && !step && !atomic_load(&g_crossing)) {  // (the pointer's step wrapping round: back at once)
        if (!atomic_exchange(&g_unwrapping, false)) {
            int bit = dpad - 1;
            if (bit == BTN_DOWN || bit == BTN_RIGHT) g_end_obj = c->x[0], g_end_a = ca, g_end_b = cb;
            atomic_store(&g_unwrap_bit, bit == BTN_DOWN ? BTN_UP : bit == BTN_UP ? BTN_DOWN : bit == BTN_LEFT ? BTN_RIGHT : BTN_LEFT);
            if (getenv("LARECOMP_MENU_MOUSE_LOG")) LOG("[menumouse] list %d,%d wrapped round to %d,%d: back (%d a row)\n", ca, cb, a, b, cols);
        }
        return 0;
    }
    if (jumping && step) {  // (the hovered slot: as many rows and columns from the selected one as on screen; the
        // list doesn't scroll - the slot is on screen)
        int na = ca + g_jump.drow * cols + g_jump.dcol;
        if (na >= 0 && (g_jump.dcol || g_jump.drow)) {
            c->x[1] = (uint64_t)(uint32_t)na, c->x[2] = (uint64_t)(uint32_t)cb;
            atomic_store(&g_jump.idx, -1);
            if (getenv("LARECOMP_MENU_MOUSE_LOG")) LOG("[menumouse] list %d,%d -> %d,%d made %d,%d (%d a row)\n", ca, cb, a, b, na, cb, cols);
        }
    }
    (void)addr;
    return 0;
}

// A choice window over a menu (Arceus's "Favorite / Discard / Cancel" on an item): its rows "N_cursor_00".."_06" with
// their texts "T_text_0n" beside them (only the rows with a text shown are choices), the selected one's marker
// "P_marker_00"; its arrow "N_cursor_arrow_00" is up while it is. It is modal: its rows are the only buttons. Its
// navigator counts the rows as they are numbered (a 1 x n grid): a hover jumps there by number.
// A window of rows that are each a "key_select_00" (Arceus's Grit Dust: which stat - the rows L_btn_glv_00.. and Quit),
// the marker "P_marker_00" and the arrow under the selected one's: its rows are the buttons, in order down the window.
// It is modal too - left to the general scan, the buttons were the Pokémon rows under it: a hover's presses for them
// moved its cursor (to Quit), and a click on it was a click on the row behind.
static bool scan_key_select_window(const Pane *marker) {
    const Pane *selp = parent_of(marker), *ks = selp ? parent_of(selp) : NULL;
    if (!ks || strcmp(ks->name, "key_select_00")) return false;
    const uint64_t *list = g_list;
    int n = g_nlist, nr = 0;
    const Pane *root = root_of(marker), *rows[NITEM];
    for (int i = 0; i < n && nr < NITEM; i++) {
        if (!pane_ok(list[i])) continue;
        const Pane *p = (const Pane *)(uintptr_t)list[i];
        if (strcmp(p->name, "key_select_00") || !shown(p) || root_of(p) != root || fabsf(p->m[3] - ks->m[3]) > 4) continue;
        bool dup = false;
        for (int k = 0; k < nr && !dup; k++) dup = rows[k] == p;
        if (!dup) rows[nr++] = p;
    }
    for (int i = 0; i < nr; i++)  // (top to bottom: the navigator's order)
        for (int j = i + 1; j < nr; j++)
            if (rows[j]->m[7] > rows[i]->m[7]) { const Pane *t = rows[i]; rows[i] = rows[j], rows[j] = t; }
    float mx, my, mw, mh;
    pane_rect(marker, &mx, &my, &mw, &mh);
    float hh = nr > 1 ? fabsf(rows[0]->m[7] - rows[1]->m[7]) / 2 : mh;
    g_nitems = 0, g_settings_up = false;
    for (int k = 0; k < nr && g_nitems < NITEM; k++) {
        Item *it = &g_items[g_nitems++];
        it->anchor = rows[k], it->part = rows[k], it->idx = k, it->listed = false;
        it->ax = rows[k]->m[3], it->ay = rows[k]->m[7];
        it->cx = mx, it->hw = mw, it->cy = rows[k]->m[7], it->hh = hh;
    }
    g_cur_x = ks->m[3], g_cur_y = ks->m[7];  // (the selected row: the marker's)
    return g_nitems > 0;
}

static bool scan_choice_window(void) {
    const uint64_t *list = g_list;
    int n = g_nlist;
    const Pane *arrow = NULL, *marker = NULL, *rows[10] = {0}, *texts[10] = {0};
    for (int i = 0; i < n; i++) {
        if (!pane_ok(list[i])) continue;
        const Pane *p = (const Pane *)(uintptr_t)list[i];
        if (!strcmp(p->name, "N_cursor_arrow_00") && shown(p)) arrow = p;
        else if (!strcmp(p->name, "P_marker_00") && shown(p)) marker = p;
        else if (!strncmp(p->name, "N_cursor_0", 10) && p->name[10] >= '0' && p->name[10] <= '9' && !p->name[11] && shown(p)) rows[p->name[10] - '0'] = p;
    }
    if (arrow && marker && !rows[0]) return scan_key_select_window(marker);
    if (!arrow || !marker || !rows[0]) return false;
    // (each row's text: the "T_text_0n" of its number beside it - other layouts on screen have their own T_text_00)
    for (int i = 0; i < n; i++) {
        if (!pane_ok(list[i])) continue;
        const Pane *p = (const Pane *)(uintptr_t)list[i];
        if (strncmp(p->name, "T_text_0", 8) || p->name[8] < '0' || p->name[8] > '9' || p->name[9] || !shown(p)) continue;
        int k = p->name[8] - '0';
        if (rows[k] && fabsf(p->m[7] - rows[k]->m[7]) <= 12 && p->m[3] > rows[k]->m[3]) texts[k] = p;
    }
    g_nitems = 0, g_settings_up = false;
    for (int k = 0; k < 10 && g_nitems < NITEM; k++) {
        if (!rows[k] || !texts[k] || fabsf(texts[k]->m[7] - rows[k]->m[7]) > 12) continue;
        Item *it = &g_items[g_nitems++];
        float tx, ty, tw, th;
        pane_rect(texts[k], &tx, &ty, &tw, &th);
        float left = rows[k]->m[3] - 23, right = tx + tw;
        it->anchor = rows[k], it->part = rows[k], it->idx = k, it->listed = false;
        it->ax = rows[k]->m[3], it->ay = rows[k]->m[7];
        it->cx = (left + right) / 2, it->hw = (right - left) / 2, it->cy = rows[k]->m[7], it->hh = 23;
    }
    g_cur_x = rows[0]->m[3], g_cur_y = marker->m[7];  // (the selected row: where the marker is)
    return g_nitems > 0;
}

// A button's anchor: "N_cursor" - or "N_cursor_00", Z-A's list rows (the Pokémon menu's "L_item_00_Copy<n>", the
// green arrow L_cursor on the selected one's)
static bool is_anchor(const Pane *p) { return !strcmp(p->name, "N_cursor") || !strcmp(p->name, "N_cursor_00"); }

static void scan_menu(void) {
    gather();
    const uint64_t *list = g_list;
    int n = g_nlist;
    if (scan_choice_window()) return;
    // the cursor: the last drawn (on top) of those on a button's N_cursor - else the last drawn
    const Pane *cursor = NULL, *on_button = NULL, *lined_up = NULL;  // (lined up: in a button's row or column - the
    for (int i = 0; i < n; i++) {                                     // Satchel's on an empty slot; not the party's
        const Pane *p = (const Pane *)(uintptr_t)list[i];             // parked off screen, drawn after it)
        if (!pane_ok(list[i]) || !is_cursor(p) || !shown(p)) continue;
        cursor = p;
        for (int j = 0; j < n; j++) {
            const Pane *a = (const Pane *)(uintptr_t)list[j];
            if (!pane_ok(list[j]) || !is_anchor(a) || !shown(a)) continue;
            float dx = a->m[3] - p->m[3], dy = a->m[7] - p->m[7];
            if (dx * dx + dy * dy < 40 * 40) { on_button = p; break; }
            if ((fabsf(dx) < 8 || fabsf(dy) < 8) && fabsf(p->m[3]) < 960 && fabsf(p->m[7]) < 540 && root_of(a) == root_of(p)) lined_up = p;
        }
    }
    if (!on_button && lined_up) cursor = lined_up;
    // (a cursor that left the buttons for a place without one - Arceus's Satchel: its empty slots - is still the
    // cursor, and its menu's buttons still the buttons: the pointer went dead there, a hover did nothing)
    static const Pane *held, *held_box;
    bool held_on = false;
    if (!on_button && held)
        for (int i = 0; i < n && !held_on; i++) held_on = list[i] == (uint64_t)(uintptr_t)held && pane_ok(list[i]) && shown(held);
    g_cur_free = false;
    bool opened_free = false;  // (a menu opened with its cursor between the buttons - the Satchel on the slot an item
                               // used up left empty: no button seen yet, the pointer was dead there)
    if (on_button) cursor = on_button;
    else if (held_on) cursor = held, g_cur_free = true;
    else held = NULL, g_cur_free = opened_free = cursor != NULL;
    g_nitems = 0, g_settings_up = false;
    if (!cursor) return;
    g_cur_x = cursor->m[3], g_cur_y = cursor->m[7];
    // the buttons: the N_cursor panes in the layout of the one the cursor is on (else in the cursor's own)
    const Pane *box = g_cur_free && !opened_free ? held_box : root_of(cursor);
    float bd = 40 * 40;
    for (int i = 0; i < n; i++) {
        const Pane *p = (const Pane *)(uintptr_t)list[i];
        if (!pane_ok(list[i]) || !is_anchor(p) || !shown(p)) continue;
        float dx = p->m[3] - g_cur_x, dy = p->m[7] - g_cur_y;
        if (dx * dx + dy * dy < bd) bd = dx * dx + dy * dy, box = root_of(p);
    }
    if (on_button) held = on_button, held_box = box;
    for (int i = 0; i < n && !g_settings_up; i++) {
        const Pane *p = (const Pane *)(uintptr_t)list[i];
        g_settings_up = pane_ok(list[i]) && !strcmp(p->name, "T_explanation_00") && root_of(p) == box && shown(p);
    }
    for (int i = 0; i < n && g_nitems < NITEM; i++) {
        const Pane *p = (const Pane *)(uintptr_t)list[i];
        if (!pane_ok(list[i]) || !is_anchor(p) || root_of(p) != box || !shown(p)) continue;
        Item *it = &g_items[g_nitems];
        it->anchor = p, it->ax = p->m[3], it->ay = p->m[7];
        const Pane *part = parent_of(p);  // (the button: the first pane up with a size)
        for (int k = 0; part && k < 6 && (part->size[0] <= 0 || part->size[1] <= 0); k++) part = parent_of(part);
        if (!part || part->size[0] <= 0 || part->size[1] <= 0) continue;
        // (not the item the cursor carries - Arceus's Satchel giving an item, Grit Dust: its icon "L_btn_catch_00"
        // rides above the selected Pokémon with an N_cursor of its own; as a button, a hover by the selected row went
        // for it - a press up, round to the bottom row and back: the cursor flip-flopped)
        bool carried = false;
        // (nor one inside the cursor itself - Z-A's green arrow L_cursor has an N_cursor_00 of its own)
        for (const Pane *q = part; q && q != box && !carried; q = parent_of(q)) carried = !strncmp(q->name, "L_btn_catch_", 12) || is_cursor(q);
        if (carried) continue;
        it->part = part;
        it->idx = name_index(part->name);
        pane_rect(part, &it->cx, &it->cy, &it->hw, &it->hh);
        bool clipped = false;  // (a scrolling list's rows outside it)
        it->listed = false;
        for (const Pane *q = part; q && q != box; q = parent_of(q))
            if (!strncmp(q->name, "S_scissor", 9)) {
                it->listed = true;
                float sx, sy, sw, sh;
                pane_rect(q, &sx, &sy, &sw, &sh);
                it->sx = sx, it->sy = sy, it->sw = sw, it->sh = sh;
                if (it->cy < sy - sh || it->cy > sy + sh) clipped = true;
                float top = fminf(it->cy + it->hh, sy + sh), bot = fmaxf(it->cy - it->hh, sy - sh);
                it->cy = (top + bot) / 2, it->hh = (top - bot) / 2;
                float rt = fminf(it->cx + it->hw, sx + sw), lt = fmaxf(it->cx - it->hw, sx - sw);
                it->cx = (rt + lt) / 2, it->hw = (rt - lt) / 2;
            }
        if (!clipped && it->hh > 4) g_nitems++;
    }
    // Each button's area: the biggest pane drawn around it that holds no other button and is about as tall as it -
    // a row of Arceus's Pokémon list is its whole row (L_pokelist_0n, a layout apart from the cursor's targets), not
    // the icon at its left its anchor's first sized parent is (the name, HP and level beside it were outside every
    // button). Not for a scroll list's rows (clipped to the list).
    for (int i = 0; i < g_nitems; i++) {
        Item *it = &g_items[i];
        float best = it->hw * it->hh, maxh = fmaxf(it->hh * 1.6f, 100);
        for (int k = 0; k < n; k++) {
            if (!pane_ok(list[k])) continue;
            const Pane *q = (const Pane *)(uintptr_t)list[k];
            if (q->size[0] <= 0 || q->size[1] <= 0 || !shown(q)) continue;
            float cx, cy, hw, hh;
            pane_rect(q, &cx, &cy, &hw, &hh);
            if (hw >= 900 || hh > maxh || hw * hh <= best || fabsf(it->ax - cx) > hw || fabsf(it->ay - cy) > hh) continue;
            bool alone = true;
            for (int j = 0; j < g_nitems && alone; j++)
                alone = j == i || fabsf(g_items[j].ax - cx) > hw || fabsf(g_items[j].ay - cy) > hh;
            if (alone) best = hw * hh, it->cx = cx, it->cy = cy, it->hw = hw, it->hh = hh;
        }
        if (it->listed) {  // (a list's row: within the list's clip, up and down - lists scroll that way; the clip may be a
            // narrow column of icons at the row's left, Arceus's Pokémon list)
            float top = fminf(it->cy + it->hh, it->sy + it->sh), bot = fmaxf(it->cy - it->hh, it->sy - it->sh);
            if (top > bot) it->cy = (top + bot) / 2, it->hh = (top - bot) / 2;
        }
    }
    // (opened between the buttons: only a place in their grid - a button in its row or column; not some cursor drawn
    // elsewhere, which would steer presses from where it isn't)
    bool lined = false;
    for (int i = 0; opened_free && i < g_nitems && !lined; i++)
        lined = fabsf(g_items[i].ax - g_cur_x) < 8 || fabsf(g_items[i].ay - g_cur_y) < 8;
    if (opened_free && !lined) g_cur_free = false;
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

bool menu_mouse_active(void) { return atomic_load(&g_menu_up); }
// A menu with buttons is up (not only the player's camera stopped: aiming, a lock-on, a dialogue, a cutscene) - a held
// mouse is let go for it (runtime/hle/input.c)
bool menu_mouse_buttons_up(void) { return atomic_load(&g_buttons_up); }
// Where the game's own cursor is, in the touch screen's 1920 x 1080 (our pointer starts there: runtime/hle/input.c)
// (the selected button's middle; no button known: the middle of the screen - the cursor sprite itself may be elsewhere,
// Arceus's menu tabs: at the top edge)
void menu_mouse_game_cursor(float *x, float *y) {
    int s = selected_item();
    *x = 960 + (s >= 0 ? g_items[s].cx : 0), *y = 540 - (s >= 0 ? g_items[s].cy : 0);
}
int menu_cursor_items(void) { return g_nitems; }  // (the buttons of the cursor menu on screen, last frame; 0: none - hle/fps60.c)
bool menu_settings_up(void) { return g_settings_up && g_nitems; }  // (runtime/hle/za_options.c, the game's thread)
bool menu_item_part(const char *prefix) {  // a button on screen is a part of that name (the game's options file)
    for (int i = 0; i < g_nitems; i++)
        if (g_items[i].part && !strncmp(g_items[i].part->name, prefix, strlen(prefix))) return true;
    return false;
}

// A click on the Settings screen's row (runtime/hle/za_options.c): on one of its choices, or along its bar - the
// left / right presses that set that (+ right, - left), 0 for anything else.
bool settings_cursor_row(Cpu *c, uint64_t *type, int *value);
int settings_tab(void);  // (-1: the screen isn't up)

// A click on the Settings screen's tabs: the L / R presses to that one (+ R, - L), 0 elsewhere. (Its tab bar: three
// "L_switchitem_0n" parts in "N_switch_00", outside the rows.)
static int tab_presses(float x, float y) {
    const uint64_t *list = g_list;
    int n = g_nlist, tab = settings_tab();
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
// ------------------------------------------------------------------ tab bars
// A menu's tabs, clicked: the presses that switch them (L / R or ZL / ZR) one at a time towards the clicked tab, each
// after the last has landed, until it is the selected one (a locked tab the game skips doesn't throw a count off).
// Each bar by its panes: its tabs (name prefix, their parent's name), the pane shown on the selected one, the presses,
// and the arrows at its ends (a click there: one press).
typedef struct { const char *tab, *parent, *selected; int left, right; const char *larrow, *rarrow; } TabBar;
static const TabBar k_bars[] = {
    // Arceus's menu: Satchel / Trainer card / Online / Settings..., ZL / ZR (each "L_menu_0n" in "N_menu_0n": its
    // "N_select_01" is shown on the selected one)
    {"L_menu_0", "N_menu_0", "N_select_01", BTN_ZL, BTN_ZR, NULL, NULL},
    // Arceus's pages within one (Satchel: Everyday items / Key items), L / R ("L_tab_disp_0n": "N_selected_00")
    {"L_tab_disp_0", "N_tab_scale_00", "N_selected_00", BTN_L, BTN_R, "L_cursor_l_00", "L_cursor_r_00"},
    // Z-A's Satchel pockets (Medicines / Key Items...), L / R ("L_icon_0n" in "A_alignment_00": the selected one's title
    // balloon "N_title_00")
    {"L_icon_0", "A_alignment_00", "N_title_00", BTN_L, BTN_R, NULL, NULL},
};
#define NBARS (int)(sizeof k_bars / sizeof k_bars[0])
static bool lit(const Pane *p) { return p && (p->flag & 1) && p->galpha; }  // (itself drawn, not faded)
static bool under(const Pane *p, const Pane *top) {
    for (int i = 0; p && i < 12; i++, p = parent_of(p)) if (p == top) return true;
    return false;
}
// The bar's tabs on screen, left to right (up to 12), and which is selected (-1: none seen)
static int bar_tabs(const TabBar *b, const Pane **t, int *selected) {
    const uint64_t *list = g_list;
    int n = g_nlist, nt = 0;
    *selected = -1;
    for (int i = 0; i < n && nt < 12; i++) {
        if (!pane_ok(list[i])) continue;
        const Pane *p = (const Pane *)(uintptr_t)list[i], *up = parent_of(p);
        if (strncmp(p->name, b->tab, strlen(b->tab)) || !up || strncmp(up->name, b->parent, strlen(b->parent)) || !shown(p)) continue;
        bool dup = false;
        for (int k = 0; k < nt; k++) dup |= t[k] == p;
        if (!dup) t[nt++] = p;
    }
    for (int i = 0; i < nt; i++)
        for (int j = i + 1; j < nt; j++)
            if (t[j]->m[3] < t[i]->m[3]) { const Pane *k = t[i]; t[i] = t[j], t[j] = k; }
    for (int i = 0; i < n && *selected < 0; i++) {
        if (!pane_ok(list[i])) continue;
        const Pane *p = (const Pane *)(uintptr_t)list[i];
        if (strcmp(p->name, b->selected) || !lit(p)) continue;
        for (int k = 0; k < nt; k++) if (under(p, t[k])) { *selected = k; break; }
    }
    return nt;
}
static struct { int bar, target, tries; } g_tabgo = {-1, 0, 0};
static int g_tab_step;  // (an arrow clicked: one press, + right / - left, on the bar g_tab_step_bar)
static int g_tab_step_bar;
// A click on a tab (or a bar's arrow): true when it was one
static bool tab_click(float x, float y) {
    for (int b = 0; b < NBARS; b++) {
        const Pane *t[12];
        int sel, nt = bar_tabs(&k_bars[b], t, &sel);
        if (nt < 2) continue;
        // (the nearest of the tabs under it: Z-A's pocket icons are parts far wider than their spacing - each click was
        // on the first)
        int hit = -1;
        float hd = 1e30f;
        for (int i = 0; i < nt; i++) {
            float cx, cy, hw, hh;
            pane_rect(t[i], &cx, &cy, &hw, &hh);
            if (fabsf(x - cx) > hw || fabsf(y - cy) > hh) continue;
            float d = (x - cx) * (x - cx) + (y - cy) * (y - cy);
            if (d < hd) hd = d, hit = i;
        }
        if (hit >= 0) {
            if (hit != sel) g_tabgo.bar = b, g_tabgo.target = hit, g_tabgo.tries = 0;
            if (getenv("LARECOMP_MENU_MOUSE_LOG")) LOG("[menumouse] tab %d of %d clicked (bar %d, selected %d)\n", hit, nt, b, sel);
            return true;
        }
        for (int a = 0; a < 2; a++) {  // (the arrows at its ends)
            const char *nm = a ? k_bars[b].rarrow : k_bars[b].larrow;
            for (int i = 0; nm && i < g_nlist; i++) {
                if (!pane_ok(g_list[i])) continue;
                const Pane *p = (const Pane *)(uintptr_t)g_list[i];
                if (strcmp(p->name, nm) || !shown(p)) continue;
                float cx, cy, hw, hh;
                pane_rect(p, &cx, &cy, &hw, &hh);
                if (fabsf(x - cx) <= hw && fabsf(y - cy) <= hh) { g_tab_step = a ? 1 : -1, g_tab_step_bar = b; return true; }
            }
        }
    }
    return false;
}

// (Arceus: its rows by their panes alone - choices "N_switch_3_0n", the chosen one with a "W_base_select_" shown;
// a slider's track "W_slider_gauge_back_00" and fill "W_slider_gauge_00", stepped to where it was clicked)
#define VALUE_NONE 1000000  // (a value's row clicked off its values: nothing - A there may leave the screen)
#define VALUE_SEEK 2000000  // (a slider: stepped by g_seek)
static struct { const Pane *part; float to, step, last; int n; } g_seek;
static int under_part(const Pane *part, const char *prefix, const Pane **out, int max) {
    const uint64_t *list = g_list;
    int n = g_nlist, k = 0;
    size_t pl = strlen(prefix);
    for (int i = 0; i < n && k < max; i++) {
        const Pane *p = (const Pane *)(uintptr_t)list[i];
        if (!pane_ok(list[i]) || strncmp(p->name, prefix, pl)) continue;
        const Pane *q = p;
        for (int d = 0; q && q != part && d < 8; d++) q = parent_of(q);
        if (q == part && shown(p)) out[k++] = p;
    }
    return k;
}
static float slider_at(const Pane *part) {  // (0..1, or -1)
    const Pane *b, *g;
    if (under_part(part, "W_slider_gauge_back_00", &b, 1) < 1 || under_part(part, "W_slider_gauge_00", &g, 1) < 1) return -1;
    float bw = b->size[0] * fabsf(b->m[0]);
    return bw > 0 ? g->size[0] * fabsf(g->m[0]) / bw : -1;
}
static int arceus_value(const Item *row, float x) {
    const Pane *ch[4], *sel[4], *back;
    int nch = under_part(row->part, "N_switch_3_0", ch, 4);
    if (nch) {
        for (int i = 0; i < nch; i++)
            for (int j = i + 1; j < nch; j++)
                if (ch[j]->m[3] < ch[i]->m[3]) { const Pane *t = ch[i]; ch[i] = ch[j], ch[j] = t; }
        int nsel = under_part(row->part, "W_base_select_", sel, 4), to = 0, from = -1;  // (_0n, or _1n on the cursor's row)
        for (int i = 1; i < nch; i++) if (fabsf(x - ch[i]->m[3]) < fabsf(x - ch[to]->m[3])) to = i;
        for (int k = 0; k < nsel; k++)
            for (int i = 0; i < nch; i++)
                if (fabsf(sel[k]->m[3] - ch[i]->m[3]) < 40) from = i;
        if (getenv("LARECOMP_MENU_MOUSE_LOG")) LOG("[menumouse] choices %d chosen %d (of %d marks) clicked %d\n", nch, from, nsel, to);
        return from < 0 || to == from ? VALUE_NONE : to - from;
    }
    if (under_part(row->part, "W_slider_gauge_back_00", &back, 1) == 1) {
        float cx, cy, hw, hh;
        pane_rect(back, &cx, &cy, &hw, &hh);
        if (x < cx - hw - 40 || x > cx + hw + 40) return VALUE_NONE;
        float t = (x - (cx - hw)) / (2 * hw);
        g_seek.part = row->part, g_seek.to = t < 0 ? 0 : t > 1 ? 1 : t, g_seek.step = 0, g_seek.last = -1, g_seek.n = 0;
        return VALUE_SEEK;
    }
    return 0;
}

static int value_presses(Cpu *c, const Item *row, float x, float y) {
    int a = arceus_value(row, x);
    if (a) return a;
    const uint64_t *list = g_list;
    int n = g_nlist;
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
    if (!settings_cursor_row(c, &type, &value)) return 0;
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
static float g_target_y;
// In a scroll list the rows' parts are reused as it scrolls (Arceus): a row there is reached by counting - the rows
// between the cursor and it, one press each once the cursor has settled - then the press is for the cursor's row.
static struct { bool on, a, landed; int left; float x, y, pitch; uint32_t moves; } g_walk;  // (y: the pointer's, the rows
// counted from; landed: walked to the row under a pointer that hasn't moved since - the list may have scrolled under it)
static bool g_have_nav;  // (menu_mouse_configure: the navigator's move is hooked - same-family buttons are jumped to)
static bool same_family(int i, int j) {  // ("L_icon_box_03" and "L_icon_box_17": numbered parts of one menu)
    const char *a = g_items[i].part->name, *b = g_items[j].part->name;
    size_t la = strnlen(a, 24), lb = strnlen(b, 24);
    return g_items[i].idx >= 0 && g_items[j].idx >= 0 && la == lb && !strncmp(a, b, la - 2);
}
static bool walk_to(int sel, int t, bool a, float x, float y) {  // (false: not a list's rows - the button is the target)
    if (sel < 0 || t < 0 || !g_items[sel].listed || !g_items[t].listed) return false;
    if (g_have_nav && same_family(sel, t)) return false;  // (a numbered grid - Boxes: its navigator jumps)
    if (g_list_fn) return false;  // (a scroll list: its select cell jumps)
    float pitch = 1e9f;
    for (int i = 0; i < g_nitems; i++)
        for (int j = 0; j < g_nitems; j++) {
            float d = fabsf(g_items[i].ay - g_items[j].ay);
            if (g_items[i].listed && g_items[j].listed && d > 10 && d < pitch) pitch = d;
        }
    if (pitch > 1e8f) pitch = 1;
    g_walk.on = true, g_walk.a = a, g_walk.x = x, g_walk.y = y, g_walk.pitch = pitch;
    g_walk.left = (int)lroundf((g_items[sel].ay - g_items[t].ay) / pitch);  // (> 0: down)
    if (getenv("LARECOMP_MENU_MOUSE_LOG")) LOG("[menumouse] walk %d rows (pitch %.0f, from %.0f to %.0f)%s\n", g_walk.left, pitch,
                                               g_items[sel].ay, g_items[t].ay, a ? ", then the click" : "");
    g_target = NULL;
    return true;
}
static int g_steps;
static uint64_t g_press_bit, g_press_until, g_wait_until;  // a press held until, then let go and the cursor watched
static float g_press_from_x, g_press_from_y;
static int g_queue_a, g_queue_b, g_queue_wheel, g_queue_value, g_queue_tab;  // (value: right presses, or left when
                                                                            // < 0; tab: R / L)
static bool g_value_press;
bool menu_mouse_value_press(void) { return g_value_press; }  // (a click's left / right press is going on)
static float g_click_x, g_click_y;

// A press: the d-pad for a hover held one frame (the game reads it once: a press), A / B three; then the cursor is
// watched for a move (10 frames for the d-pad: a hover's press the game dropped is made again, not waited out)
static int g_redo;  // (hover presses the game dropped in a row - its cursor still sliding)
static const Pane *g_expect;  // (a jump's button: no other hover press until the selection is there, or g_expect_until)
static uint64_t g_expect_until;
static void press(int bit, uint64_t f) {
    bool dir = bit == BTN_UP || bit == BTN_DOWN || bit == BTN_LEFT || bit == BTN_RIGHT;
    atomic_store(&g_ptr_dpad, dir ? bit + 1 : 0);  // (the button + 1)
    atomic_store(&g_crossing, false);
    g_press_bit = 1ull << bit, g_press_until = f + (dir ? 1 : 3), g_wait_until = f + (dir ? 1 + 10 : 3 + 24);
    g_press_from_x = g_cur_x, g_press_from_y = g_cur_y;
}

// The buttons of one grid with button `from`: same-sized parts lined up - each in a row or a column with another of them
// (the Satchel's slots and its Pokémon rows both hang off a 32 x 32 pane, but don't line up)
static void grid_of(int from, bool *in) {
    int stack[NITEM], ns = 0;
    memset(in, 0, sizeof(bool) * NITEM);
    in[from] = true, stack[ns++] = from;
    while (ns) {
        int i = stack[--ns];
        for (int j = 0; j < g_nitems; j++) {
            if (in[j] || g_items[j].part->size[0] != g_items[from].part->size[0] || g_items[j].part->size[1] != g_items[from].part->size[1]) continue;
            if (fabsf(g_items[j].ax - g_items[i].ax) > 4 && fabsf(g_items[j].ay - g_items[i].ay) > 4) continue;
            in[j] = true, stack[ns++] = j;
        }
    }
}

// The scroll list on screen at its first / last row for an up / down press (the press would wrap round)
static const Pane *g_list_box;  // (the list's layout: the list known is the one on screen)
static bool list_edge(int bit) {
    uint64_t obj = atomic_load(&g_list_obj);
    if (!obj || !g_nitems || !g_list_box || root_of(g_items[0].anchor) != g_list_box) return false;
    const int32_t *l = (const int32_t *)(uintptr_t)obj;
    int a = l[g_list_off[0] / 4], b = l[g_list_off[1] / 4], cols = atomic_load(&g_list_cols);
    if (cols < 1) cols = 1;
    if (bit == BTN_UP) return a < cols && b == 0;
    if (bit == BTN_DOWN) return obj == g_end_obj && a / cols == g_end_a / cols && b == g_end_b;
    return false;
}

// The cursor between the buttons (g_cur_free): the button of its grid it was on last - else the one nearest it, of its
// grid (not the nearest of any: at the Satchel's last empty slots a Pokémon row is nearer than a slot)
static const Pane *g_last_on;  // (the button the cursor was on last)
static int free_from(void) {
    if (!g_cur_free || !g_nitems) return -1;
    int last = -1;
    for (int i = 0; i < g_nitems; i++) if (g_items[i].anchor == g_last_on) last = i;
    if (last < 0) {  // (none yet in this menu: the nearest lined up with the cursor - its row or column - else the nearest)
        float bd = 1e30f, ba = 1e30f;
        int any = -1;
        for (int i = 0; i < g_nitems; i++) {
            float ex = g_items[i].ax - g_cur_x, ey = g_items[i].ay - g_cur_y, d = ex * ex + ey * ey;
            if (d < bd) bd = d, any = i;
            if ((fabsf(ex) < 8 || fabsf(ey) < 8) && d < ba) ba = d, last = i;
        }
        if (last < 0) last = any;
        if (last < 0) return -1;
    }
    static bool grid[NITEM];
    grid_of(last, grid);
    int best = last;
    float bd = 1e30f;
    for (int i = 0; i < g_nitems; i++) {
        float ex = g_items[i].ax - g_cur_x, ey = g_items[i].ay - g_cur_y;
        if (grid[i] && ex * ex + ey * ey < bd) bd = ex * ex + ey * ey, best = i;
    }
    return best;
}

// Each read of the controller: the buttons the mouse presses (worked out once a frame)
static bool g_configured;  // (menu_mouse_configure: the game's address known)
uint64_t menu_mouse_buttons(Cpu *c) {
    if (!g_configured) return 0;
    static uint64_t last_f = ~0ull, out;
    uint64_t f = renderer_game_frame();
    if (f == last_f) return out;
    last_f = f;
    scan_menu();  // (also for the Settings screen's Controls list: menu_settings_up)
    if (!g_on) return out = 0;
    extern uint64_t g_field_camera_frame;
    if (g_nitems || f - g_field_camera_frame > 8) g_menu_seen = f;  // (or the player's camera stopped: a menu without
    bool up = f - g_menu_seen < 10;                                    // buttons, a dialogue, a cutscene)
    atomic_store(&g_menu_up, up);
    static uint64_t buttons_seen;
    // (a page without buttons of its own in a menu with tabs - Arceus's Save / Trainer card page - is the menu too: the
    // pointer stays, for its tabs, and so do the menu keys)
    bool tabs = false;
    for (int b = 0; b < NBARS && !tabs && !g_nitems; b++) {
        const Pane *t[12];
        int sel;
        tabs = bar_tabs(&k_bars[b], t, &sel) >= 2;
    }
    // (the title screen and what comes before the world: the player's camera hasn't turned yet - the pointer's, a click
    // is A, so the mouse starts the game too)
    if (!g_field_camera_frame && up) tabs = true;
    if (g_nitems || tabs) buttons_seen = f;
    atomic_store(&g_buttons_up, g_nitems || tabs || f - buttons_seen < 10);
    extern void input_set_menu_context(bool on);  // (runtime/hle/input.c: the keyboard's menu keys while one is up)
    input_set_menu_context(atomic_load(&g_buttons_up));
    int32_t px, py;
    uint32_t moves;
    int clicks, rclicks, wheel;
    bool have = input_menu_pointer(&px, &py, &moves, &clicks, &rclicks, &wheel);
    static bool dbg = false, dbg_init;
    if (!dbg_init) dbg_init = true, dbg = getenv("LARECOMP_MENU_MOUSE_LOG") != NULL;
    if (dbg && (clicks || rclicks || wheel || (f % 60) == 0 || g_target || g_press_bit))
        LOG("[menumouse] up %d have %d at %d,%d moves %u clicks %d/%d wheel %d items %d sel %d target %p press %llx\n", up, have,
            px, py, moves, clicks, rclicks, wheel, g_nitems, selected_item(), (const void *)g_target, (unsigned long long)g_press_bit);
    if (!have || !up) {
        g_target = NULL, g_press_bit = 0, g_queue_a = g_queue_b = g_queue_wheel = g_queue_value = g_queue_tab = 0;
        atomic_store(&g_ptr_dpad, 0), atomic_store(&g_unwrapping, false);
        g_tabgo.bar = -1, g_tab_step = 0;
        g_seek.part = NULL, g_walk.on = false;
        return out = 0;
    }
    float x = (float)px - 960, y = 540 - (float)py;
    static uint32_t last_moves;
    bool moved = moves != last_moves;
    last_moves = moves;
    int sel = selected_item(), hov = g_nitems ? item_at(x, y) : -1;
    if (sel >= 0) g_last_on = g_items[sel].anchor;
    if (g_nitems && f - atomic_load(&g_list_hit) <= 3) g_list_box = root_of(g_items[0].anchor);
    {  // (a row's slots, for the scroll lists: the most buttons side by side in a row of the selected one's grid)
        int ref = sel >= 0 ? sel : free_from(), cols = 1;
        if (ref < 0) ref = hov;
        static bool grid[NITEM];
        if (ref >= 0) grid_of(ref, grid);
        for (int i = 0; ref >= 0 && i < g_nitems; i++) {
            int k = 0;
            for (int j = 0; grid[i] && j < g_nitems; j++) k += grid[j] && fabsf(g_items[j].ay - g_items[i].ay) < 4;
            if (k > cols) cols = k;
        }
        if (ref >= 0) atomic_store(&g_list_cols, cols);
    }
    static const Pane *still_on;  // (frames the cursor has been on the same button: Arceus ignores a press while it
    static int still;             // slides - and its cursor bobs, so not its pixels)
    const Pane *on = sel >= 0 ? g_items[sel].anchor : NULL;
    static float still_x, still_y;  // (between the buttons: by where it is)
    bool free_cur = sel < 0 && g_cur_free && g_nitems;
    if (free_cur) still = still_on == NULL && fabsf(g_cur_x - still_x) < 8 && fabsf(g_cur_y - still_y) < 8 ? still + 1 : 0;
    else still = on && on == still_on ? still + 1 : 0;
    still_on = on, still_x = g_cur_x, still_y = g_cur_y;
    if (moved && hov >= 0 && hov == sel && !g_target_a) g_target = NULL, g_walk.on = g_walk.on && g_walk.a;  // (back on it)
    else if (moved && hov >= 0 && g_items[hov].anchor != g_target &&
             !(g_list_box && root_of(g_items[hov].anchor) == g_list_box && atomic_load(&g_center_obj) == atomic_load(&g_list_obj)))
        g_target = g_items[hov].anchor, g_target_a = false, g_steps = 0, g_target_y = g_items[hov].ay;
    if (moved) g_walk.landed = false;
    if (!g_walk.on && g_walk.landed && clicks > 0 && sel >= 0)  // (a click where the walk took it: that row)
        g_walk.on = true, g_walk.a = true, g_walk.left = 0, g_walk.x = x, g_walk.y = y, clicks = 0;
    if (g_walk.on && (moved || clicks > 0)) {  // (walking a list: the pointer's own move in rows, not what scrolled under it)
        int rows = (int)lroundf((g_walk.y - y) / g_walk.pitch);
        g_walk.left += rows, g_walk.y -= (float)rows * g_walk.pitch, g_walk.x = x;
        if (clicks > 0) g_walk.a = true, clicks = 0;
        if (getenv("LARECOMP_MENU_MOUSE_LOG")) LOG("[menumouse] walk now %d rows%s\n", g_walk.left, g_walk.a ? ", then the click" : "");
    } else if (moved && g_target && !g_target_a && walk_to(sel, hov, false, x, y)) {}
    if (clicks > 0 && hov >= 0 && !walk_to(sel, hov, true, x, y))
        g_target = g_items[hov].anchor, g_target_a = true, g_steps = 0, g_click_x = x, g_click_y = y, g_target_y = g_items[hov].ay;
    if (clicks > 0 && !g_nitems && !tab_click(x, y)) g_queue_a += clicks;  // (no buttons: a click is A - not on a tab)
    if (clicks > 0 && g_nitems && hov < 0 && !tab_click(x, y)) g_queue_tab += tab_presses(x, y);
    g_queue_b += rclicks, g_queue_wheel += wheel;

    if (g_press_bit && f < g_press_until) return out = g_press_bit;  // (held)
    if (g_press_bit && g_value_press) {  // (a value's left / right: the cursor stays)
        if (f < g_wait_until) return out = 0;
        g_press_bit = 0, g_value_press = false;
        atomic_store(&g_ptr_dpad, 0), atomic_store(&g_unwrapping, false);
    }
    if (g_press_bit) {  // (let go: until the cursor moves)
        bool went = fabsf(g_cur_x - g_press_from_x) > 2 || fabsf(g_cur_y - g_press_from_y) > 2;
        if (!went && f < g_wait_until) return out = 0;
        bool dir = g_press_bit & (1ull << BTN_UP | 1ull << BTN_DOWN | 1ull << BTN_LEFT | 1ull << BTN_RIGHT);
        // (not moved: the menu's edge, it doesn't go there - or the game dropped the press while its cursor slid: made
        // again, up to 3 times)
        if (dir && !went && g_target && ++g_redo > 3) g_target = NULL, g_redo = 0;
        if (went) g_redo = 0;
        g_press_bit = 0;
        atomic_store(&g_ptr_dpad, 0), atomic_store(&g_unwrapping, false);
        if (went) return out = 0;  // (one more frame: it may still be sliding)
    }
    int unwrap = atomic_exchange(&g_unwrap_bit, -1);
    if (unwrap >= 0) { atomic_store(&g_unwrapping, true), press(unwrap, f); return out = g_press_bit; }
    if (g_queue_b > 0) { g_queue_b--, g_target = NULL, press(BTN_B, f); return out = g_press_bit; }
    if (g_queue_wheel && list_edge(g_queue_wheel < 0 ? BTN_DOWN : BTN_UP)) g_queue_wheel = 0;  // (the end: no further)
    if (g_queue_wheel) {
        bool down = g_queue_wheel < 0;
        g_queue_wheel += down ? 1 : -1;
        g_target = NULL, atomic_store(&g_jump.idx, -1), press(down ? BTN_DOWN : BTN_UP, f);
        return out = g_press_bit;
    }
    if (g_tab_step) {  // (a bar's arrow: one press)
        int bit = g_tab_step > 0 ? k_bars[g_tab_step_bar].right : k_bars[g_tab_step_bar].left;
        g_tab_step = 0, g_target = NULL;
        g_press_bit = 1ull << bit, g_press_until = f + 3, g_wait_until = f + 24, g_value_press = true;
        return out = g_press_bit;
    }
    if (g_tabgo.bar >= 0) {  // (a tab clicked: one press towards it at a time, until it's the selected one)
        const Pane *t[12];
        int sel, nt = bar_tabs(&k_bars[g_tabgo.bar], t, &sel);
        if (nt < 2 || sel < 0 || sel == g_tabgo.target || g_tabgo.tries++ >= 8) {
            g_tabgo.bar = -1;
        } else {
            int bit = g_tabgo.target > sel ? k_bars[g_tabgo.bar].right : k_bars[g_tabgo.bar].left;
            g_target = NULL;
            g_press_bit = 1ull << bit, g_press_until = f + 3, g_wait_until = f + 24, g_value_press = true;
            return out = g_press_bit;
        }
    }
    if ((g_queue_value || g_queue_tab || g_seek.part) && still < 6) return out = 0;  // (a value: once the cursor settled)
    if (g_queue_value || g_queue_tab) {
        int *q = g_queue_value ? &g_queue_value : &g_queue_tab;
        int bit = q == &g_queue_tab ? (*q > 0 ? BTN_R : BTN_L) : (*q > 0 ? BTN_RIGHT : BTN_LEFT);
        *q += *q > 0 ? -1 : 1;
        g_press_bit = 1ull << bit, g_press_until = f + 3, g_wait_until = f + (q == &g_queue_tab ? 24 : 7), g_value_press = true;
        return out = g_press_bit;
    }
    if (g_seek.part) {  // (a slider stepped to where it was clicked: until it's nearest, or it stops moving)
        float cur = slider_at(g_seek.part);
        if (g_seek.last >= 0 && fabsf(cur - g_seek.last) > 0.001f) g_seek.step = fabsf(cur - g_seek.last);
        bool stuck = g_seek.last >= 0 && fabsf(cur - g_seek.last) <= 0.001f;
        float d = g_seek.to - cur;
        if (cur < 0 || stuck || g_seek.n++ > 40 || (g_seek.step > 0 && fabsf(d) <= g_seek.step / 2) || fabsf(d) < 0.01f) {
            g_seek.part = NULL;
        } else {
            g_seek.last = cur;
            g_press_bit = 1ull << (d > 0 ? BTN_RIGHT : BTN_LEFT), g_press_until = f + 3, g_wait_until = f + 9, g_value_press = true;
            return out = g_press_bit;
        }
    }
    if (g_queue_a > 0) { g_queue_a--, g_target = NULL, press(BTN_A, f); return out = g_press_bit; }
    if (g_walk.on) {  // (a list's row, by counting)
        if (sel < 0 || still < 4) return out = 0;
        if (g_walk.left) {
            bool down = g_walk.left > 0;
            g_walk.left += down ? -1 : 1;
            if (getenv("LARECOMP_MENU_MOUSE_LOG")) LOG("[menumouse] walk press, %d left (cursor on %.0f)\n", g_walk.left, g_items[sel].ay);
            g_press_bit = 1ull << (down ? BTN_DOWN : BTN_UP), g_press_until = f + 3, g_wait_until = f + 10, g_value_press = true;
            return out = g_press_bit;
        }
        if (still < 6) return out = 0;
        g_walk.on = false, g_walk.landed = !g_walk.a;
        int v = g_walk.a ? value_presses(c, &g_items[sel], g_walk.x, g_walk.y) : 0;
        if (v && v != VALUE_NONE && v != VALUE_SEEK) g_queue_value = v;
        else if (!v && g_walk.a) press(BTN_A, f);
        return out = g_press_bit;
    }
    if (!g_target) return out = 0;
    int t = -1;
    for (int i = 0; i < g_nitems; i++) if (g_items[i].anchor == g_target) t = i;
    static int missing;  // (frames the target hasn't been there: gone after 10 - not one frame's gap)
    missing = t < 0 ? missing + 1 : 0;
    if (t < 0 || (sel < 0 && !free_cur)) { if (missing > 10) g_target = NULL; return out = 0; }  // (gone; or no cursor)
    if (t == sel) {
        if (g_target_a && still < 6) return out = 0;
        int v = g_target_a ? value_presses(c, &g_items[t], g_click_x, g_click_y) : 0;
        if (v && v != VALUE_NONE && v != VALUE_SEEK) g_queue_value = v;
        else if (!v && g_target_a) press(BTN_A, f);
        g_target = NULL;
        return out = g_press_bit;
    }
    // (after a jump: until the selection is where it jumped to - its cursor slides there over a few frames, and a step
    // worked out from the cursor on the way overshot: the next row's jump stacked on the last)
    if (g_expect) {
        if ((sel >= 0 && g_items[sel].anchor == g_expect) || f >= g_expect_until) g_expect = NULL;
        else return out = 0;
    }
    if (still < 2) return out = 0;  // (a press while the cursor slides can be dropped: made again if it is)
    if (++g_steps > 40) { g_target = NULL; return out = 0; }
    // (between the buttons: from where the cursor is, a cell of the buttons nearest it - the Satchel's empty slot)
    int from = sel >= 0 ? sel : free_from();
    if (from < 0) { g_target = NULL; return out = 0; }
    float fx = sel >= 0 ? g_items[sel].ax : g_cur_x, fy = sel >= 0 ? g_items[sel].ay : g_cur_y;
    float dx = g_items[t].ax - fx, dy = g_items[t].ay - fy;
    bool family = sel >= 0 && same_family(t, sel);
    static bool grid[NITEM];  // (one grid: the cursor's and the hovered button)
    grid_of(from, grid);
    bool sized = grid[t];
    atomic_store(&g_jump.idx, -1);  // (a jump from before not taken: not this press's)
    // (a hover over another part of the screen than the cursor's - the Satchel's Pokémon while its items are chosen -
    // doesn't walk the cursor there: it may not go (it went down the items' rows towards them, to the empty slots);
    // a click does)
    if (!family && !sized && !g_target_a) { g_target = NULL; return out = 0; }
    // (the same part family as the selected one - one menu: its navigator jumps there; else a step towards it)
    if (family || sized) {
        g_jump.grid = !family, g_jump.from = sel >= 0 ? g_items[sel].idx : -1;
        // (same-sized buttons: a grid - the steps in cells, by their pitch)
        float pw = 2 * g_items[t].hw, ph = 2 * g_items[t].hh;
        for (int i = 0; i < g_nitems; i++)  // (the pitch: the nearest neighbours' distance - of any two of them: the
            for (int j = 0; j < i; j++) {   // cursor's row may have one)
                if (g_items[i].part->size[0] != g_items[t].part->size[0] || g_items[j].part->size[0] != g_items[t].part->size[0]) continue;
                float ex = fabsf(g_items[i].ax - g_items[j].ax), ey = fabsf(g_items[i].ay - g_items[j].ay);
                if (ey < 4 && ex > 4 && ex < pw * 1.5f) pw = ex;
                if (ex < 4 && ey > 4 && ey < ph * 1.5f) ph = ey;
            }
        g_jump.dcol = (int)lroundf(dx / pw), g_jump.drow = (int)lroundf(-dy / ph), g_jump.horiz = fabsf(dy) < fabsf(dx);
        atomic_store(&g_jump.idx, g_jump.grid ? 0 : g_items[t].idx), g_jump.frame = f;
        g_expect = g_items[t].anchor, g_expect_until = f + 12;
        // (one press: the navigator lands on it - a list may scroll under the pointer after, so the walk ends here; a
        // click is done on the row the cursor settles on)
        if (g_have_nav) {
            if (g_target_a) g_walk.on = true, g_walk.a = true, g_walk.left = 0, g_walk.x = g_click_x, g_walk.y = g_click_y,
                            g_walk.pitch = 2 * g_items[t].hh;
            g_target = NULL;
        }
    }
    press(fabsf(dy) >= fabsf(dx) ? (dy > 0 ? BTN_UP : BTN_DOWN) : (dx > 0 ? BTN_RIGHT : BTN_LEFT), f);
    atomic_store(&g_crossing, !family && !sized);
    return out = g_press_bit;
}

// ------------------------------------------------------------------ research

static void dump_panes(const char *path) {
    int n = g_nlist;
    FILE *o = fopen(path, "w");
    if (!o) return;
    for (int i = 0; i < n; i++) {
        const Pane *p = (const Pane *)(uintptr_t)g_list[i];
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

void menu_mouse_present(void) {
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

uint64_t g_field_camera_frame;  // the last frame the player's camera turned (the game's camera hook): not in a menu

// calc_mtx: the game's (pane, draw info) function that works out a pane's global matrix; nav_move: its menu cursor
// navigator's core (0: none - the cursor is walked there); list_fn, list_a, list_b: its scroll lists' select cell and
// where a list keeps the two values it takes (0: none)
void menu_mouse_configure(uint64_t calc_mtx, uint64_t nav_move, uint64_t list_fn, uint32_t list_a, uint32_t list_b) {
    const char *e = getenv("LARECOMP_MENU_MOUSE");
    g_on = !(e && atoi(e) == 0);
    g_configured = true;
    rt_register_fnhook_at(calc_mtx, pane_calc_hook);
    if (nav_move && g_on) g_have_nav = true, rt_register_fnhook_at(nav_move, nav_move_hook);
    if (list_fn && g_on) g_list_fn = list_fn, g_list_off[0] = list_a, g_list_off[1] = list_b, rt_register_fnhook_at(list_fn, list_select_hook);
}
