// The mouse in the game's menus (the Legends games: this file is the same in both repositories, configured with the
// game's address). Pointing at a menu's button moves the game's cursor to it, a click presses it (A), the right
// button goes back (B), the wheel moves up and down - by pressing the d-pad and buttons for the player, so every menu
// reacts the way it does to a controller.
//
// The game's UI is Nintendo's ui2d. Every pane drawn has its screen matrix worked out each frame (menu_mouse_configure's
// function, (pane, draw info), from the virtual CalculateMtx; Z-A 0x700264486c): those of the last 3 frames are
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

// The panes worked out lately (Arceus doesn't do every layout on every frame): a table by pane, with the frame and
// the order it was last done in; g_list (rebuilt once a frame, scan_menu) has those of the last 3 frames, in order.
#define NSEEN 32768
#define NPANE 16384
static struct { _Atomic uint64_t pane; uint64_t frame, seq; } g_seen[NSEEN];
static _Atomic uint64_t g_seq;
static uint64_t g_list[NPANE];
static int g_nlist;

static int pane_calc_hook(Cpu *c, uint64_t addr) {
    (void)addr;
    uint64_t p = c->x[0], f = renderer_game_frame();
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

typedef struct { const Pane *anchor, *part; float ax, ay, cx, cy, hw, hh; bool listed; int idx; } Item;  // anchor, the part: centre,
                                                                                               // half size; in a scroll list
#define NITEM 256
static Item g_items[NITEM];
static int g_nitems;
static float g_cur_x, g_cur_y;  // the cursor
static bool g_settings_up;      // the Settings screen is the one with the cursor (its description line drawn)
static _Atomic bool g_menu_up;
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
// core: Z-A 0x7000fe5d40, Arceus 0x700272b6d0 - (nav, &direction, &status) -> the new cell; nav +8 the index, +0xc /
// +0x10 columns / rows, +0x1c column-major). A press towards the hovered button is still made - the menu's own input
// path, animations and sounds - and the core's answer is the hovered button instead of the next cell: by its part's
// number when the parts are numbered as the navigator counts (the selected one's number is its index), else by the
// rows and columns between the two on screen (a list whose parts are reused as it scrolls).
static struct { _Atomic int idx; uint64_t frame; int dcol, drow, from; bool grid; } g_jump = {-1, 0, 0, 0, -1, false};
static int nav_move_hook(Cpu *c, uint64_t addr) {  // (the navigator's core: (nav, &direction, &status) -> x0 the new
    uint64_t nav = c->x[0];                          // cell, low / high 32 bits as the flag at +0x1c says)
    const int32_t *n = (const int32_t *)(uintptr_t)nav;
    int want = atomic_load(&g_jump.idx), from = n[2];
    rt_call_original(c, addr);
    uint64_t f = renderer_game_frame();
    static int log = -1;
    if (log < 0) log = getenv("LARECOMP_MENU_MOUSE_LOG") != NULL;
    int cols = n[3], rows = n[4];
    if (want < 0 || f - g_jump.frame > 30 || cols <= 0 || rows <= 0) return 1;
    uint32_t lo = (uint32_t)c->x[0], hi = (uint32_t)(c->x[0] >> 32);
    int got = n[7] ? (int)(rows * lo + hi) : (int)(cols * hi + lo);
    if (got == from) return 1;  // (not a move: a query, or the menu's edge)
    bool by_name = !g_jump.grid && g_jump.from == from;
    if (!by_name) {
        int col = n[7] ? from / rows : from % cols, row = n[7] ? from % rows : from / cols;
        col += g_jump.dcol, row += g_jump.drow;
        if (col < 0 || row < 0 || col >= cols || row >= rows) return 1;
        want = n[7] ? col * rows + row : row * cols + col;
    } else if (want >= cols * rows) return 1;
    if (log) LOG("[menumouse] nav %llx %d -> %d, made %d (grid %dx%d)\n", (unsigned long long)nav, from, got, want, cols, rows);
    lo = n[7] ? (uint32_t)(want / rows) : (uint32_t)(want % cols);
    hi = n[7] ? (uint32_t)(want % rows) : (uint32_t)(want / cols);
    c->x[0] = (uint64_t)hi << 32 | lo;
    return 1;  // (the jump stays for the frame: a menu may ask the core twice for one press)
}

// Scroll lists (the Settings screens) move their cursor with their own "select cell" (Z-A 0x7000fc2c50, Arceus
// 0x7002719878: (list, a, b), the current a / b in the list at the configured offsets): a one-step move asked while a
// hover jump waits becomes a move of the whole distance (the rows between the selected and hovered rows on screen).
static uint64_t g_list_fn;
static uint32_t g_list_off[2];
static int list_select_hook(Cpu *c, uint64_t addr) {
    int want = atomic_load(&g_jump.idx);
    if (want >= 0 && renderer_game_frame() - g_jump.frame <= 30) {
        const int32_t *l = (const int32_t *)(uintptr_t)c->x[0];
        int ca = l[g_list_off[0] / 4], cb = l[g_list_off[1] / 4], a = (int32_t)c->x[1], b = (int32_t)c->x[2];
        int da = a - ca, db = b - cb, steps = abs(g_jump.drow) >= abs(g_jump.dcol) ? abs(g_jump.drow) : abs(g_jump.dcol);
        if (abs(da) + abs(db) == 1 && steps > 1) {
            c->x[1] = (uint64_t)(uint32_t)(ca + da * steps), c->x[2] = (uint64_t)(uint32_t)(cb + db * steps);
            atomic_store(&g_jump.idx, -1);
            if (getenv("LARECOMP_MENU_MOUSE_LOG")) LOG("[menumouse] list %d,%d -> %d,%d made %d,%d\n", ca, cb, a, b, ca + da * steps, cb + db * steps);
        }
    }
    (void)addr;
    return 0;
}

static void scan_menu(void) {
    gather();
    const uint64_t *list = g_list;
    int n = g_nlist;
    // the cursor: the last drawn (on top) of those on a button's N_cursor - else the last drawn
    const Pane *cursor = NULL, *on_button = NULL;
    for (int i = 0; i < n; i++) {
        const Pane *p = (const Pane *)(uintptr_t)list[i];
        if (!pane_ok(list[i]) || !is_cursor(p) || !shown(p)) continue;
        cursor = p;
        for (int j = 0; j < n; j++) {
            const Pane *a = (const Pane *)(uintptr_t)list[j];
            if (!pane_ok(list[j]) || strcmp(a->name, "N_cursor") || !shown(a)) continue;
            float dx = a->m[3] - p->m[3], dy = a->m[7] - p->m[7];
            if (dx * dx + dy * dy < 40 * 40) { on_button = p; break; }
        }
    }
    if (on_button) cursor = on_button;
    g_nitems = 0, g_settings_up = false;
    if (!cursor) return;
    g_cur_x = cursor->m[3], g_cur_y = cursor->m[7];
    // the buttons: the N_cursor panes in the layout of the one the cursor is on (else in the cursor's own)
    const Pane *box = root_of(cursor);
    float bd = 40 * 40;
    for (int i = 0; i < n; i++) {
        const Pane *p = (const Pane *)(uintptr_t)list[i];
        if (!pane_ok(list[i]) || strcmp(p->name, "N_cursor") || !shown(p)) continue;
        float dx = p->m[3] - g_cur_x, dy = p->m[7] - g_cur_y;
        if (dx * dx + dy * dy < bd) bd = dx * dx + dy * dy, box = root_of(p);
    }
    for (int i = 0; i < n && !g_settings_up; i++) {
        const Pane *p = (const Pane *)(uintptr_t)list[i];
        g_settings_up = pane_ok(list[i]) && !strcmp(p->name, "T_explanation_00") && root_of(p) == box && shown(p);
    }
    for (int i = 0; i < n && g_nitems < NITEM; i++) {
        const Pane *p = (const Pane *)(uintptr_t)list[i];
        if (!pane_ok(list[i]) || strcmp(p->name, "N_cursor") || root_of(p) != box || !shown(p)) continue;
        Item *it = &g_items[g_nitems];
        it->anchor = p, it->ax = p->m[3], it->ay = p->m[7];
        const Pane *part = parent_of(p);  // (the button: the first pane up with a size)
        for (int k = 0; part && k < 6 && (part->size[0] <= 0 || part->size[1] <= 0); k++) part = parent_of(part);
        if (!part || part->size[0] <= 0 || part->size[1] <= 0) continue;
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

bool menu_mouse_active(void) { return atomic_load(&g_menu_up); }
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

static void press(int bit, uint64_t f) {
    g_press_bit = 1ull << bit, g_press_until = f + 3, g_wait_until = f + 3 + 24;
    g_press_from_x = g_cur_x, g_press_from_y = g_cur_y;
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
        g_seek.part = NULL, g_walk.on = false;
        return out = 0;
    }
    float x = (float)px - 960, y = 540 - (float)py;
    static uint32_t last_moves;
    bool moved = moves != last_moves;
    last_moves = moves;
    int sel = selected_item(), hov = g_nitems ? item_at(x, y) : -1;
    static const Pane *still_on;  // (frames the cursor has been on the same button: Arceus ignores a press while it
    static int still;             // slides - and its cursor bobs, so not its pixels)
    const Pane *on = sel >= 0 ? g_items[sel].anchor : NULL;
    still = on && on == still_on ? still + 1 : 0;
    still_on = on;
    if (moved && hov >= 0 && hov == sel && !g_target_a) g_target = NULL, g_walk.on = g_walk.on && g_walk.a;  // (back on it)
    else if (moved && hov >= 0 && g_items[hov].anchor != g_target)
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
    if (t < 0 || sel < 0) { if (missing > 10) g_target = NULL; return out = 0; }  // (gone; or the cursor between buttons)
    if (t == sel) {
        if (g_target_a && still < 6) return out = 0;
        int v = g_target_a ? value_presses(c, &g_items[t], g_click_x, g_click_y) : 0;
        if (v && v != VALUE_NONE && v != VALUE_SEEK) g_queue_value = v;
        else if (!v && g_target_a) press(BTN_A, f);
        g_target = NULL;
        return out = g_press_bit;
    }
    if (still < 4) return out = 0;  // (a press while the cursor slides can be dropped)
    if (++g_steps > 40) { g_target = NULL; return out = 0; }
    float dx = g_items[t].ax - g_items[sel].ax, dy = g_items[t].ay - g_items[sel].ay;
    // (the same part family as the selected one - one menu: its navigator jumps there; else a step towards it)
    if (same_family(t, sel) || (g_items[t].part->size[0] == g_items[sel].part->size[0] && g_items[t].part->size[1] == g_items[sel].part->size[1])) {
        g_jump.grid = !same_family(t, sel), g_jump.from = g_items[sel].idx;
        // (same-sized buttons: a grid - the steps in cells, by their pitch)
        float pw = 2 * g_items[t].hw, ph = 2 * g_items[t].hh;
        for (int i = 0; i < g_nitems; i++) {  // (the pitch: the nearest neighbours' distance)
            if (g_items[i].part->size[0] != g_items[t].part->size[0]) continue;
            float ex = fabsf(g_items[i].ax - g_items[sel].ax), ey = fabsf(g_items[i].ay - g_items[sel].ay);
            if (ey < 4 && ex > 4 && ex < pw * 1.5f) pw = ex;
            if (ex < 4 && ey > 4 && ey < ph * 1.5f) ph = ey;
        }
        g_jump.dcol = (int)lroundf(dx / pw), g_jump.drow = (int)lroundf(-dy / ph);
        atomic_store(&g_jump.idx, g_jump.grid ? 0 : g_items[t].idx), g_jump.frame = f;
        // (one press: the navigator lands on it - a list may scroll under the pointer after, so the walk ends here; a
        // click is done on the row the cursor settles on)
        if (g_have_nav) {
            if (g_target_a) g_walk.on = true, g_walk.a = true, g_walk.left = 0, g_walk.x = g_click_x, g_walk.y = g_click_y,
                            g_walk.pitch = 2 * g_items[t].hh;
            g_target = NULL;
        }
    }
    press(fabsf(dy) >= fabsf(dx) ? (dy > 0 ? BTN_UP : BTN_DOWN) : (dx > 0 ? BTN_RIGHT : BTN_LEFT), f);
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
