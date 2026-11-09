// Legends: Arceus's Settings screen (Menu -> Help -> Settings) with the start screen's settings in it: rows added
// after the game's twelve, drawn and driven by the game's code like its own; its Controls row opens the key bindings
// in the list's place (below: "the Controls list").
//
// The screen (code 0x700112c000..0x7001133400): one scrolling list, its rows a pool of parts ("L_settings_list_%02d")
// that show the settings by id. The view V keeps the model at [V+0x5b0] - its +0x88 vector lists the rows' setting ids
// (8 bytes each: id, flag; built by f_700112eaa4 for ids 0..11, the list sized from it after) - the cursor's id at
// +0x5b8 (12: none), its row +0x5bc. A row is drawn into a part (slot) by f_700112f108 (V, row, -, slot): the texts
// for the id (heading and choices or a slider's heading, by message label hash), then its value through
// f_70011302f4 (V, id, slot, active) -> per id: f_700113116c (V, value, choices, slot, active) or f_7001131270 (V, value,
// min, max, slot, active). Left / right go to f_700112c700 (model, id, op: 0 left, 1 right; 2 cursor moved) - per id -
// then the row's value is drawn again; the description is built by f_7001130c58 (V, id) -> [x8] string. Ids from 16
// are ours (12 is "none"): those six are hooked for them.
//
// Text: every text goes through f_7001130458 (V, label hash, pane) -> the message, expanded into a string object
// (UTF-16 at +0x58, capacity +0x68, length +0x7a) -> 0x700273ce7c (formatter, pane, string, flag). Our rows pass
// labels of their own (0x5ec0... + an index); the hook asks the game for one of its labels instead and the string's
// text is replaced with ours just before it goes on the pane. LARECOMP_PLA_OPTIONS=0: the game's screen as it is.
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../hle.h"
#include "../input_bind.h"
#include "../settings.h"

void rt_register_fnhook_at(uint64_t addr, int (*fn)(Cpu *c, uint64_t addr));
void rt_call_original(Cpu *c, uint64_t addr);
void guest_call(Cpu *c, uint64_t addr);
void menu_apply_fullscreen(void);  // runtime/ui/menu.mm

#define OPT_BUILD_ROWS 0x700112eaa4ull  // (view): the rows' ids
#define OPT_VEC_PUSH 0x70011329e4ull    // (vector, &entry)
#define OPT_BIND 0x700112f108ull        // (view, row, -, slot)
#define OPT_SHOW_VALUE 0x70011302f4ull  // (view, id, slot, active)
#define OPT_CHANGE 0x700112c700ull      // (model, id, op)
#define OPT_DESCRIPTION 0x7001130c58ull // (view, id) -> [x8] string
#define OPT_TEXT 0x7001130458ull        // (view, label hash, pane)
#define OPT_TEXT_APPLY 0x700273ce7cull  // (formatter, pane, string, flag)
#define OPT_HEADING 0x70011305ecull     // (view, slot, hash)
#define OPT_CHOICE_LABEL 0x7001130708ull  // (view, choice, slot, hash)
#define OPT_CHOICE_LAYOUT 0x7001130a04ull // (view, slot, s0: 0 two choices, 1 three)
#define OPT_SLIDER_HEADING 0x700112fa4cull  // (view, slot, hash)
#define OPT_CHOICES 0x700113116cull     // (view, value, choices, slot, active)
#define OPT_SLIDER 0x7001131270ull      // (view, value, min, max, slot, active)
#define GAME_LABEL 0xf78b9ed1507ad6dfull  // (a label of the game's: its text is replaced)
#define OUR_LABEL 0x5ec0000000000000ull
#define FIRST_ID 16
#define BIND_ID 64                       // (the Controls list's rows)
#define OPT_SLOT_OF 0x700112f5e4ull      // (view, row; -2 the cursor's) -> its part (slot), -1 off screen
#define UI_FIND 0x7000c2ee24ull          // (view + 0x180, &label) -> the part of that name
#define UI_LIST_REBUILD 0x700271857cull  // (list): its rows from the model again

// ------------------------------------------------------------------ the rows

enum { CHOICE, BAR10 };
typedef struct {
    const char *name, *info;
    int kind, n;
    const char *choice[6];
    int (*get)(void);
    void (*set)(int);
    void (*act)(int);  // (A on the row: runs it for the choice shown)
} Row;

static const int k_fps[] = {30, 60, 72, 120, 144, FPS_UNLIMITED},  // (72: half of 144 Hz, every frame 2 refreshes)
                 k_lines[] = {0, 540, 720, 1080, 1440, 2160};  // (540p: weak GPUs, ~half 1080p's GPU time)
static int find(const int *v, int n, int x) {
    for (int i = 0; i < n; i++) if (v[i] == x) return i;
    return 0;
}
static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }
static int get_fps(void) { return find(k_fps, 6, g_settings.next_fps ? g_settings.next_fps : g_settings.fps); }
// (120 / 144: the game's own logic was made for 30 and some of it counts frames; the row's description says so while
// one of them is chosen - drawn again right after the change, settings_screen_buttons)
static const char *const k_fps_warning = "Experimental: scripted scenes can stall at this rate. If you get stuck, set 60 or 30 fps.";
static bool g_info_pending;
bool legends_set_fps(int fps);  // runtime/hle/fps60.c: at once
static void set_fps(int i) {
    if (legends_set_fps(k_fps[i])) g_settings.fps = k_fps[i], g_settings.next_fps = 0;
    else g_settings.next_fps = k_fps[i] == g_settings.fps ? 0 : k_fps[i];
}
static int get_res(void) { return find(k_lines, 6, g_settings.next_res_height ? g_settings.next_res_height - 1 : g_settings.res_height); }
static void set_res(int i) { g_settings.next_res_height = k_lines[i] == g_settings.res_height ? 0 : k_lines[i] + 1; }
static int get_full(void) { return g_settings.fullscreen ? 1 : 0; }
static void set_full(int i) { g_settings.fullscreen = i == 1, menu_apply_fullscreen(); }
static int get_aa(void) { return g_settings.antialias == AA_SMAA ? 0 : 1; }
static void set_aa(int i) { g_settings.antialias = i == 0 ? AA_SMAA : AA_OFF; }
static int get_prompts(void) { return g_settings.button_prompts >= 0 && g_settings.button_prompts <= 4 ? g_settings.button_prompts : 0; }
static void set_prompts(int i) { g_settings.button_prompts = i; }
static int get_steps(void) { return g_settings.fixed_steps ? 1 : 0; }
static void set_steps(int i) { g_settings.fixed_steps = i == 1; }
static int get_fpsc(void) { return g_settings.show_fps ? 0 : 1; }
static void set_fpsc(int i) { g_settings.show_fps = i == 0; }
static int get_skip(void) { return g_settings.skip_start ? 1 : 0; }
static void set_skip(int i) { g_settings.skip_start = i == 1; }
int input_last_was_kbm(void);  // runtime/hle/input.c
static int ctl_col(void) { return input_last_was_kbm() ? 0 : 1; }  // (the Controls list: the column A changes)
static void ctl_open(int i);
static int get_mcam(void) { return g_settings.mouse_camera ? 0 : 1; }
static void set_mcam(int i) { g_settings.mouse_camera = i == 0; }
// (bars: the nearest step to what the ini has; mouse 0.025 degrees per pixel a step, dead zone 4% of the travel a step)
static int get_msens(void) { return clampi((int)(g_settings.mouse_sens / 0.025f + 0.5f) - 1, 0, 10); }
static void set_msens(int v) { g_settings.mouse_sens = 0.025f * (float)(v + 1); }
static int get_dz(void) { return clampi((int)(g_settings.stick_deadzone / 0.04f + 0.5f), 0, 10); }
static void set_dz(int v) { g_settings.stick_deadzone = 0.04f * (float)v; }
static int get_minvy(void) { return g_settings.mouse_invert_y ? 1 : 0; }
static void set_minvy(int i) { g_settings.mouse_invert_y = i == 1; }
static int get_minvx(void) { return g_settings.mouse_invert_x ? 1 : 0; }
static void set_minvx(int i) { g_settings.mouse_invert_x = i == 1; }
static int get_smooth(void) { return g_settings.shadow_smooth ? 0 : 1; }
static void set_smooth(int i) { g_settings.shadow_smooth = i == 0; }
static int get_sharp(void) { return g_settings.sharpen >= 0 && g_settings.sharpen <= 3 ? g_settings.sharpen : 0; }
static void set_sharp(int i) { g_settings.sharpen = i; }
static int get_shadows(void) { return g_settings.shadows >= 0 && g_settings.shadows <= 2 ? g_settings.shadows : 0; }
static void set_shadows(int i) { g_settings.shadows = i; }
void pla_set_detail(void);  // (hle/pla_drawdist.c)
static int get_detail(void) { return g_settings.detail >= 0 && g_settings.detail <= 3 ? g_settings.detail : 0; }
static void set_detail(int i) { g_settings.detail = i; pla_set_detail(); }
static int get_shl(void) { return g_settings.shader_nowait ? 1 : 0; }
static void set_shl(int i) { g_settings.shader_nowait = i; }
static int get_bg(void) { return g_settings.background >= BG_KEEP && g_settings.background <= BG_PAUSE ? g_settings.background : BG_KEEP; }
static void set_bg(int i) { g_settings.background = i; }
void pkm_link_apply(void);  // (hle/pkm_link.c)
int pkm_files_get(void);
void pkm_files_set(int v), pkm_files_run(int v);
static int get_pklink(void) { return g_settings.pkhex_link >= 0 && g_settings.pkhex_link <= 2 ? g_settings.pkhex_link : 0; }
static void set_pklink(int i) { g_settings.pkhex_link = i; pkm_link_apply(); }
static int get_vol(void) { return clampi((int)(g_settings.volume * 10 + 0.5f), 0, 10); }
static void set_vol(int v) { g_settings.volume = (float)v / 10; }

static const Row k_rows[] = {
    {"Controls", "Press A to see and change the key and the controller button for each action.", CHOICE, 2,
     {"Keyboard", "Controller"}, ctl_col, ctl_open},
    {"Frame Rate", "Choose how many frames are drawn a second.", CHOICE, 6,
     {"30 fps", "60 fps", "72 fps", "120 fps", "144 fps", "Unlimited"}, get_fps, set_fps},
    {"Frame Timing", "Choose whether the game keeps full speed when the frame rate dips (Real Time) or slows down with it.",
     CHOICE, 2, {"Real Time", "Fixed"}, get_steps, set_steps},
    {"Resolution (Next Start)", "Choose the resolution the game is drawn at. It applies the next time you start the game.", CHOICE, 6,
     {"Auto", "540p", "720p", "1080p", "1440p", "4K"}, get_res, set_res},
    {"Screen Mode", "Choose whether the game fills the screen or plays in a window.", CHOICE, 2, {"Window", "Fullscreen"},
     get_full, set_full},
    {"Anti-Aliasing", "Choose whether to smooth jagged edges (SMAA).", CHOICE, 2, {"Enabled", "Disabled"}, get_aa, set_aa},
    {"Sharpening", "Choose how much the image is sharpened at your screen's resolution (AMD's RCAS).", CHOICE, 4,
     {"Off", "Low", "Medium", "High"}, get_sharp, set_sharp},
    {"Shadows (Next Start)", "Choose how sharp shadows are. Higher shows the shadows of thin things, like flower stems, further away.",
     CHOICE, 3, {"Switch", "High", "Ultra"}, get_shadows, set_shadows},
    {"Shadow Smoothing (Next Start)", "Choose whether shadow edges are smoothed or show the shadow map's steps.", CHOICE, 2,
     {"Enabled", "Disabled"}, get_smooth, set_smooth},
    {"Draw Distance", "Choose how far away trees and the rest of the field are still drawn. Farther costs performance.",
     CHOICE, 4, {"Switch", "High", "Very High", "Max"}, get_detail, set_detail},
    {"Shader Loading", "Wait for new things to load (may stutter), or show them a moment later.", CHOICE, 2,
     {"Wait", "Don't Wait"}, get_shl, set_shl},
    {"Button Prompts", "Choose which button prompts show. Auto shows the ones you are playing with.", CHOICE, 5,
     {"Auto", "Switch", "Xbox", "PlayStation", "Keyboard"}, get_prompts, set_prompts},
    {"FPS Counter", "Choose whether to show the frame rate in the corner of the screen.", CHOICE, 2, {"Show", "Hide"},
     get_fpsc, set_fpsc},
    {"Start Screen", "Choose whether the start screen shows when the game starts. Holding Shift while starting shows it.",
     CHOICE, 2, {"Show", "Skip"}, get_skip, set_skip},
    {"In Background", "Choose whether the game keeps playing, mutes or pauses while it is in the background.",
     CHOICE, 3, {"Keep Playing", "Mute", "Pause"}, get_bg, set_bg},
    {"Mouse Camera", "Choose whether a click in the game lets the mouse turn the camera. Esc lets it go.", CHOICE, 2,
     {"Enabled", "Disabled"}, get_mcam, set_mcam},
    {"Mouse Sensitivity", "Choose how far the camera turns when you move the mouse.", BAR10, 0, {0}, get_msens, set_msens},
    {"Mouse Vertical", "Choose which way the camera turns when you move the mouse up and down.", CHOICE, 2,
     {"Regular", "Inverted"}, get_minvy, set_minvy},
    {"Mouse Horizontal", "Choose which way the camera turns when you move the mouse left and right.", CHOICE, 2,
     {"Regular", "Inverted"}, get_minvx, set_minvx},
    {"Stick Dead Zone", "Choose how far a controller's sticks must move before they count.", BAR10, 0, {0}, get_dz, set_dz},
    {"Master Volume", "Adjust the volume of all of the game's sound.", BAR10, 0, {0}, get_vol, set_vol},
    {"PKHeX Link", "Let PKHeX's LiveHeX plugin edit your boxes as you play, from this PC or your network.",
     CHOICE, 3, {"Off", "This PC", "Network"}, get_pklink, set_pklink},
    {"Pokémon Files", "Press A to save your boxed Pokémon as PKHeX files, or add the ones in pokemon/import.",
     CHOICE, 2, {"Export", "Import"}, pkm_files_get, pkm_files_set, pkm_files_run},
};
#define NROWS (int)(sizeof k_rows / sizeof *k_rows)
// A row's choices as offered: the frame rate's above 60 only with the start screen's Experimental settings on
static int nchoice(const Row *w) { return w->get == get_fps && !g_settings.experimental ? 2 : w->n; }
static int g_first[NROWS];  // CHOICE rows of more than 3: the first of the three shown

static int row_of(uint32_t id) { return id >= FIRST_ID && id < FIRST_ID + (uint32_t)NROWS ? (int)(id - FIRST_ID) : -1; }
static void keep_shown(int r, int v) {
    if (v < g_first[r]) g_first[r] = v;
    if (v > g_first[r] + 2) g_first[r] = v - 2;
    g_first[r] = clampi(g_first[r], 0, nchoice(&k_rows[r]) - 3);
}
static int shown_value(int r) {
    const Row *w = &k_rows[r];
    int v = w->get();
    if (w->kind != CHOICE || nchoice(w) <= 3) return v;
    keep_shown(r, v);
    return v - g_first[r];
}

// ------------------------------------------------------------------ the Controls list
// A, or a click, on the Controls row puts the bindings (runtime/hle/bindings.c) in the list's place - its row table
// holds their ids then, the list rebuilt: a row an action (the Switch input the game reads, named as its Game Controls
// screen does), the key and the controller's button its two choices. A waits for the new one from what pressed it -
// the keyboard (or a click) for the key, the controller for its button; a click on one changes that one (Esc cancels;
// one in use swaps over). X puts the defaults back (the keyboard's or the controller's, by the same rule), B goes back.
// (Its A is "Save and Exit" there: kept from the game on these rows.)

static const struct { int slot; const char *name; } k_binds[] = {
    {BIND_LS_UP, "Move Forward"}, {BIND_LS_DOWN, "Move Back"}, {BIND_LS_LEFT, "Move Left"}, {BIND_LS_RIGHT, "Move Right"},
    {BIND_WALK, "Walk (Hold)"}, {BIND_RS_UP, "Camera Up"}, {BIND_RS_DOWN, "Camera Down"}, {BIND_RS_LEFT, "Camera Left"},
    {BIND_RS_RIGHT, "Camera Right"}, {BIND_A, "Investigate / Talk"}, {BIND_B, "Crouch / Rise"},
    {BIND_X, "Ready Item / Pokémon"}, {BIND_Y, "Dodge"}, {BIND_ZL, "Focus on Target"}, {BIND_ZR, "Aim and Throw"},
    {BIND_L, "Previous Item"}, {BIND_R, "Next Item"}, {BIND_LSTICK, "Dash"}, {BIND_RSTICK, "Right Stick Press"},
    {BIND_UP, "Open Menu"}, {BIND_DOWN, "Pokédex"}, {BIND_LEFT, "Previous Ride Pokémon"}, {BIND_RIGHT, "Next Ride Pokémon"},
    {BIND_PLUS, "Ride Pokémon"}, {BIND_MINUS, "Arc Phone"},
};
#define NBINDS (int)(sizeof k_binds / sizeof *k_binds)
static bool g_ctl;               // the list holds the bindings
static int g_ctl_pending = -1;   // (a click: 100 + the Controls row's choice opens it; 0 / 1 on a binding: that column waits)
bool menu_mouse_value_press(void);  // runtime/hle/menu_mouse.c
static void ctl_open(int i) { if (menu_mouse_value_press()) g_ctl_pending = 100 + i; }
static int bind_of(uint32_t id) { return g_ctl && id >= BIND_ID && id < BIND_ID + (uint32_t)NBINDS ? (int)(id - BIND_ID) : -1; }

static const char *ctl_cell(int b, int col, char *buf, size_t n) {
    int slot = k_binds[b].slot, dev, cslot, ms;
    if (bind_capture_active(&dev, &cslot, &ms) && cslot == slot && dev == (col ? BIND_DEV_PAD : BIND_DEV_KEY))
        return col ? "Press a button..." : "Press a key...";
    if (!col) snprintf(buf, n, "%s", g_bind_key[slot] ? bind_key_label(g_bind_key[slot]) : "-");
    else if (slot == BIND_WALK) snprintf(buf, n, "-");
    else if (slot >= BIND_PAD_COUNT) snprintf(buf, n, "%s", slot < BIND_RS_UP ? "Left Stick" : "Right Stick");
    else snprintf(buf, n, "%s", bind_pad_label(g_bind_pad[slot], bind_label_family()));
    return buf;
}

// ------------------------------------------------------------------ text

static const char *g_texts[64];  // (our labels: OUR_LABEL + index)
static _Atomic int g_ntexts;
static uint64_t our_label(const char *s) {
    int i = atomic_fetch_add(&g_ntexts, 1) & 63;
    g_texts[i] = s;
    return OUR_LABEL | (uint64_t)i;
}
static __thread const char *t_text;  // (the text the next string put on a pane gets)

static void put_text(uint8_t *str, const char *s, int wrap) {  // (a string object: UTF-8 -> UTF-16, lines of up to wrap)
    uint16_t *buf = *(uint16_t **)(str + 0x58);
    uint64_t cap = *(const uint64_t *)(str + 0x68);
    if (!buf || cap < 2) return;
    size_t n = 0, col = 0;
    for (const uint8_t *p = (const uint8_t *)s; *p && n + 1 < cap;) {
        if (*p == ' ' && wrap) {
            size_t w = 1;
            while (p[w] && p[w] != ' ') w++;
            if (col + w > (size_t)wrap) { buf[n++] = '\n', col = 0, p++; continue; }
        }
        uint32_t u = *p++;
        if (u >= 0xe0 && p[0] && p[1]) u = (u & 0xf) << 12 | (p[0] & 0x3fu) << 6 | (p[1] & 0x3fu), p += 2;
        else if (u >= 0xc0 && p[0]) u = (u & 0x1f) << 6 | (p[0] & 0x3fu), p++;
        buf[n++] = (uint16_t)u, col++;
    }
    buf[n] = 0;
    *(uint16_t *)(str + 0x7a) = (uint16_t)n;
}

static int text_hook(Cpu *c, uint64_t addr) {  // (view, hash, pane): ours -> the game's label, the text kept for later
    (void)addr;
    if ((c->x[1] & 0xffff000000000000ull) != OUR_LABEL) return 0;
    t_text = g_texts[c->x[1] & 63];
    c->x[1] = GAME_LABEL;
    return 0;
}
static int text_apply_hook(Cpu *c, uint64_t addr) {  // (formatter, pane, string, flag)
    (void)addr;
    if (!t_text) return 0;
    put_text((uint8_t *)(uintptr_t)c->x[2], t_text, 0);
    t_text = NULL;
    return 0;
}

static void guest(Cpu *c, uint64_t fn, const uint64_t *a, int n, float s0) {
    uint64_t save[31];
    __uint128_t sv = c->v[0];
    uint32_t bits;
    memcpy(&bits, &s0, 4);
    memcpy(save, c->x, sizeof save);
    for (int i = 0; i < n; i++) c->x[i] = a[i];
    c->v[0] = bits;  // (s0: the low 32 bits of v0)
    guest_call(c, fn);
    memcpy(c->x, save, sizeof save);
    c->v[0] = sv;
}

// ------------------------------------------------------------------ hooks

static void draw_row(Cpu *c, uint64_t view, int r, uint64_t slot, bool texts, uint64_t active) {
    const Row *w = &k_rows[r];
    if (w->kind == BAR10) {
        if (texts) guest(c, OPT_SLIDER_HEADING, (uint64_t[]){view, slot, our_label(w->name)}, 3, 0);
        guest(c, OPT_SLIDER, (uint64_t[]){view, (uint64_t)w->get(), 0, 10, slot, active}, 6, 0);
        return;
    }
    int n = nchoice(w) > 3 ? 3 : nchoice(w), first = nchoice(w) > 3 ? (shown_value(r), g_first[r]) : 0;
    if (texts) {
        guest(c, OPT_HEADING, (uint64_t[]){view, slot, our_label(w->name)}, 3, 0);
        for (int i = 0; i < n; i++) guest(c, OPT_CHOICE_LABEL, (uint64_t[]){view, (uint64_t)i, slot, our_label(w->choice[first + i])}, 4, 0);
        guest(c, OPT_CHOICE_LAYOUT, (uint64_t[]){view, slot}, 2, n == 3 ? 1.0f : 0.0f);
    }
    guest(c, OPT_CHOICES, (uint64_t[]){view, (uint64_t)shown_value(r), (uint64_t)n, slot, active}, 5, 0);
}

static void draw_bind(Cpu *c, uint64_t view, int b, uint64_t slot, uint64_t active) {
    static char k[4][64], p[4][64];
    static int ring;
    int i = ring++ & 3;
    guest(c, OPT_HEADING, (uint64_t[]){view, slot, our_label(k_binds[b].name)}, 3, 0);
    guest(c, OPT_CHOICE_LABEL, (uint64_t[]){view, 0, slot, our_label(ctl_cell(b, 0, k[i], sizeof k[i]))}, 4, 0);
    guest(c, OPT_CHOICE_LABEL, (uint64_t[]){view, 1, slot, our_label(ctl_cell(b, 1, p[i], sizeof p[i]))}, 4, 0);
    guest(c, OPT_CHOICE_LAYOUT, (uint64_t[]){view, slot}, 2, 0.0f);
    guest(c, OPT_CHOICES, (uint64_t[]){view, (uint64_t)ctl_col(), 2, slot, active}, 5, 0);
}

static uint64_t g_view, g_view_vt, g_page;  // (the Settings screen's view, its class; the page left / right go to)

static int build_rows_hook(Cpu *c, uint64_t addr) {
    uint64_t view = c->x[0];
    rt_call_original(c, addr);
    uint64_t model = *(const uint64_t *)(uintptr_t)(view + 0x5b0);
    static uint32_t entry[2];
    for (int i = 0; i < NROWS && model; i++) {
        entry[0] = FIRST_ID + (uint32_t)i, entry[1] = 0;
        guest(c, OPT_VEC_PUSH, (uint64_t[]){model + 0x88, (uint64_t)(uintptr_t)entry}, 2, 0);
    }
    return 1;
}

static int bind_hook(Cpu *c, uint64_t addr) {  // (view, row, -, slot)
    (void)addr;
    uint64_t view = c->x[0], model = *(const uint64_t *)(uintptr_t)(view + 0x5b0);
    g_view = view, g_view_vt = *(const uint64_t *)(uintptr_t)view;
    uint32_t id = *(const uint32_t *)(uintptr_t)(*(const uint64_t *)(uintptr_t)(model + 0xa8) + 8 * (uint32_t)c->x[1]);
    if (bind_of(id) >= 0) {
        draw_bind(c, view, bind_of(id), c->x[3], *(const uint32_t *)(uintptr_t)(view + 0x5b8) == id);
        return 1;
    }
    int r = row_of(id);
    if (r < 0) return 0;
    draw_row(c, view, r, c->x[3], true, *(const uint32_t *)(uintptr_t)(view + 0x5b8) == id);
    return 1;
}

static int show_value_hook(Cpu *c, uint64_t addr) {  // (view, id, slot, active)
    (void)addr;
    if (bind_of((uint32_t)c->x[1]) >= 0) { draw_bind(c, c->x[0], bind_of((uint32_t)c->x[1]), c->x[2], c->x[3] & 1); return 1; }
    int r = row_of((uint32_t)c->x[1]);
    if (r < 0) return 0;
    draw_row(c, c->x[0], r, c->x[2], k_rows[r].kind == CHOICE && nchoice(&k_rows[r]) > 3, c->x[3] & 1);  // (sliding: texts too)
    return 1;
}

static int change_hook(Cpu *c, uint64_t addr) {  // (model, id, op)
    (void)addr;
    g_page = c->x[0];
    if (bind_of((uint32_t)c->x[1]) >= 0 && ((uint32_t)c->x[2] == 0 || (uint32_t)c->x[2] == 1)) {  // (left / right: nothing;
        if (menu_mouse_value_press()) g_ctl_pending = (int)(uint32_t)c->x[2];                     // a click: that column)
        c->x[0] = 0;
        return 1;
    }
    int r = row_of((uint32_t)c->x[1]), op = (int)(uint32_t)c->x[2];
    if (r < 0 || (op != 0 && op != 1)) return 0;
    const Row *w = &k_rows[r];
    int v = w->get(), to = clampi(v + (op ? 1 : -1), 0, w->kind == BAR10 ? 10 : nchoice(w) - 1);
    if (to != v) w->set(to), settings_save();
    if (to != v && w->get == get_fps) g_info_pending = true;
    if (w->kind == CHOICE && nchoice(w) > 3) keep_shown(r, to);
    c->x[0] = to != v;
    return 1;
}

static int description_hook(Cpu *c, uint64_t addr) {  // (view, id) -> [x8] string
    int r = row_of((uint32_t)c->x[1]);
    if (bind_of((uint32_t)c->x[1]) >= 0) {
        int dev, slot, ms;
        bool wait = bind_capture_active(&dev, &slot, &ms);
        const char *t = wait ? (dev == BIND_DEV_KEY ? "Press the new key, mouse button or wheel. Esc cancels; one already in use swaps over."
                                                    : "Press the new controller button. Esc cancels; one already in use swaps over.")
                             : "A changes it: a key from the keyboard, a button from the controller. X: defaults. B: back.";
        uint64_t out = c->x[8];
        c->x[1] = 0;
        rt_call_original(c, addr);
        uint8_t *str = (uint8_t *)(uintptr_t)*(const uint64_t *)(uintptr_t)out;
        if (str) put_text(str, t, 48);
        return 1;
    }
    if (r < 0) return 0;
    uint64_t out = c->x[8];
    c->x[1] = 0;
    rt_call_original(c, addr);
    uint8_t *str = (uint8_t *)(uintptr_t)*(const uint64_t *)(uintptr_t)out;
    if (str) put_text(str, k_rows[r].get == get_fps && get_fps() >= 2 ? k_fps_warning : k_rows[r].info, 48);
    return 1;
}

// what runtime/hle/menu_mouse.c asks (the game's own rows: not known here)
bool settings_cursor_row(Cpu *c, uint64_t *type, int *value) { (void)c, (void)type, (void)value; return false; }
int settings_tab(void) { return -1; }
static void set_rows(Cpu *c, bool ctl) {  // (the row table: the settings, or the bindings; the list rebuilt)
    uint64_t model = *(const uint64_t *)(uintptr_t)(g_view + 0x5b0);
    if (!model) return;
    g_ctl = ctl;
    *(uint64_t *)(uintptr_t)(model + 0xb0) = 0;  // (its vector's size)
    static uint32_t entry[2];
    int n = ctl ? NBINDS : 12 + NROWS;
    for (int i = 0; i < n; i++) {
        entry[0] = ctl ? BIND_ID + (uint32_t)i : i < 12 ? (uint32_t)i : FIRST_ID + (uint32_t)(i - 12), entry[1] = 0;
        guest(c, OPT_VEC_PUSH, (uint64_t[]){model + 0x88, (uint64_t)(uintptr_t)entry}, 2, 0);
    }
    static const struct { uint64_t hash; const char *str; uint64_t len, zero; } panel = {0x801e693dd9658f72ull, "scroll_panel_00", 15, 0};
    uint64_t save[31];
    memcpy(save, c->x, sizeof save);
    c->x[0] = g_view + 0x180, c->x[1] = (uint64_t)(uintptr_t)&panel;
    guest_call(c, UI_FIND);
    uint64_t list = c->x[0];
    memcpy(c->x, save, sizeof save);
    if (list) guest(c, UI_LIST_REBUILD, (uint64_t[]){list}, 1, 0);
}
static void redraw_cursor_row(Cpu *c) {  // (and the description)
    uint32_t id = *(const uint32_t *)(uintptr_t)(g_view + 0x5b8);
    uint64_t save[31];
    memcpy(save, c->x, sizeof save);
    c->x[0] = g_view, c->x[1] = (uint64_t)(int64_t)-2;
    guest_call(c, OPT_SLOT_OF);
    int64_t slot = (int32_t)c->x[0];
    memcpy(c->x, save, sizeof save);
    if (slot >= 0) guest(c, OPT_SHOW_VALUE, (uint64_t[]){g_view, id, (uint64_t)slot, 1}, 4, 0);
    if (g_page) guest(c, OPT_CHANGE, (uint64_t[]){g_page, id, 2}, 3, 0);  // (as when the cursor moves to it)
}

// Each read of the controller (runtime/hle/input.c, the game's thread): A on the Controls row opens the list; in it
// A / X / B do its things - those presses are kept from the game (until let go).
uint64_t settings_screen_buttons(Cpu *c, uint64_t b) {
    enum { A = 1 << 0, B = 1 << 1, X = 1 << 2 };
    static uint64_t prev, kept;
    static int serial0 = -1;
    uint64_t edge = b & ~prev;
    prev = b;
    kept &= b;
    extern bool menu_item_part(const char *prefix);  // runtime/hle/menu_mouse.c
    static int gone;  // (frames the screen hasn't been seen: gone after 30 - not a frame's gap in what's drawn)
    bool live = g_view && *(const uint64_t *)(uintptr_t)g_view == g_view_vt;
    gone = live && menu_item_part("L_settings_list_") ? 0 : gone + 1;
    if (!live || gone > 30) {
        g_ctl = false, g_ctl_pending = -1;
        return b & ~kept;
    }
    if (gone) return b & ~kept;
    uint32_t id = *(const uint32_t *)(uintptr_t)(g_view + 0x5b8);
    int dev, slot, ms, serial, pending = g_ctl_pending;
    g_ctl_pending = -1;
    bind_last_result(&dev, &slot, &ms, &serial);
    if (serial0 < 0) serial0 = serial;
    if (g_ctl && bind_capture_active(&dev, &slot, &ms)) { kept |= b; return 0; }  // (the next press is the binding's)
    if (serial != serial0) {  // (set, swapped or cancelled)
        serial0 = serial;
        settings_save();
        if (g_ctl) redraw_cursor_row(c);
    }
    if (g_info_pending && !g_ctl) redraw_cursor_row(c), g_info_pending = false;  // (the frame rate changed: its description again)
    int bi = bind_of(id);
    if (!g_ctl) {
        if (id == FIRST_ID && ((edge & A) || pending >= 100)) kept |= edge & A, set_rows(c, true);  // (the Controls row)
        else if ((edge & A) && row_of(id) >= 0 && k_rows[row_of(id)].act)  // (Pokémon Files)
            kept |= A, k_rows[row_of(id)].act(k_rows[row_of(id)].get());
    } else if (pending == 0 || pending == 1) {
        if (bi >= 0 && !(pending && k_binds[bi].slot >= BIND_PAD_COUNT))
            bind_capture_begin(pending ? BIND_DEV_PAD : BIND_DEV_KEY, k_binds[bi].slot), redraw_cursor_row(c);
    } else if (edge & B) {
        kept |= B, set_rows(c, false);
    } else if (edge & X) {
        kept |= X, bind_reset(ctl_col() ? BIND_DEV_PAD : BIND_DEV_KEY), settings_save(), set_rows(c, true);
    } else if ((edge & A) && bi >= 0) {
        kept |= A;
        int col = ctl_col();
        if (!(col && k_binds[bi].slot >= BIND_PAD_COUNT))  // (a controller's sticks stay its sticks)
            bind_capture_begin(col ? BIND_DEV_PAD : BIND_DEV_KEY, k_binds[bi].slot), redraw_cursor_row(c);
    }
    return b & ~kept;
}

// ------------------------------------------------------------------ research
static _Atomic int g_trace_on, g_trace_gen;
static struct { uint64_t addr; int gen, n; } g_traced[4096];
static int g_ntraced;

static int trace_hook(Cpu *c, uint64_t addr) {
    if (!atomic_load(&g_trace_on)) return 0;
    for (int i = 0; i < g_ntraced; i++)
        if (g_traced[i].addr == addr) {
            int gen = atomic_load(&g_trace_gen);
            if (g_traced[i].gen != gen) g_traced[i].gen = gen, g_traced[i].n = 0;
            if (g_traced[i].n++ >= (getenv("LARECOMP_OPTTRACE_MAX") ? atoi(getenv("LARECOMP_OPTTRACE_MAX")) : 40)) return 0;
            break;
        }
    char bt[200] = "";  // (the guest frame pointers' chain: the callers above)
    uint64_t fp = c->x[29];
    for (int k = 0, n = 0; k < 6 && fp >= 0x7000000000ull && fp < 0x7800000000ull && !(fp & 7); k++) {
        uint64_t lr = *(const uint64_t *)(uintptr_t)(fp + 8);
        n += snprintf(bt + n, sizeof bt - n, " %llx", (unsigned long long)lr);
        fp = *(const uint64_t *)(uintptr_t)fp;
    }
    float s0;
    memcpy(&s0, &c->v[0], 4);
    extern uint64_t renderer_present_count(void);
    LOG("[opttrace] %llx from %llx x0 %llx x1 %llx x2 %llx x3 %llx s0 %g @%llu bt%s\n", (unsigned long long)addr, (unsigned long long)c->x[30],
        (unsigned long long)c->x[0], (unsigned long long)c->x[1], (unsigned long long)c->x[2], (unsigned long long)c->x[3], s0,
        (unsigned long long)renderer_present_count(), bt);
    return 0;
}

// (each present: the live dir's "opttrace" file turns tracing on; a new one - removed and made again - restarts the counts)
void pla_options_present(void) {
    static const char *dir;
    static bool init;
    if (!init) init = true, dir = getenv("LARECOMP_ZA_OPTTRACE") ? getenv("LARECOMP_LIVE") : NULL;
    if (!dir) return;
    char path[512];
    snprintf(path, sizeof path, "%s/opttrace", dir);
    FILE *f = fopen(path, "r");
    bool on = f != NULL;
    if (f) fclose(f);
    if (on && !atomic_load(&g_trace_on)) atomic_fetch_add(&g_trace_gen, 1);
    atomic_store(&g_trace_on, on);
}

// ------------------------------------------------------------------ the game's own settings, from the launcher
// The game's twelve settings live in one u32 at [cfg+0x68] (cfg = [[0x70042a6ee0]+0x230]); the save keeps each part
// as a block of its own. PLA_CFG_LOAD reads them from the save and pushes the volumes to the sound engine,
// PLA_CFG_DEFAULTS sets a new game's (also the Settings screen's Restore Defaults), PLA_CFG_SAVE writes them back.
// Launcher: the values the save had (Settings.game_opts, kept at each load and save) and new ones it chose
// (game_opts_next), written into the field each time the game loads the save - or starts a new one - until it saves
// them. Bits (PLA_CFG_BITS; 2-5 the text language and 18 are the game's own, never touched): 0-1 text speed 0..2;
// 6-9 / 10-13 / 14-17 music / sound effects / cries 0..10; 19 autosave off; 20 / 21 vertical / horizontal camera NOT
// inverted; 22-23 motion sensitivity 0..3; 24-26 camera sensitivity 0..4 (shown 1..5); 27 HUD toggling off; 28 ZR
// confirmation off; 29 dynamic range narrow.
#define PLA_CFG_LOAD 0x7000fea798ull      // (cfg)
#define PLA_CFG_DEFAULTS 0x7000fea060ull  // (cfg)
#define PLA_CFG_SAVE 0x7000fea5f4ull      // (cfg)
#define PLA_CFG_RESTORE_RET 0x700112c804ull  // (the Settings screen's Restore Defaults calling PLA_CFG_DEFAULTS)
#define PLA_SET_RTPC 0x7002813898ull      // (engine, &name, -1, 0, s0 value)
#define PLA_SOUND_ENGINE 0x70042a84b8ull  // (the engine, once the sound is up)
#define PLA_SET_DYNAMIC_RANGE 0x700112cc5cull  // (w0: 1 wide)
#define PLA_CFG_BITS 0x3ffbffc3u

static void cfg_seen(uint64_t cfg, bool saving) {  // (the save's values, for the launcher)
    int v = (int)(*(const uint32_t *)(uintptr_t)(cfg + 0x68) & PLA_CFG_BITS);
    bool done = saving && g_settings.game_opts_next >= 0 && v == (g_settings.game_opts_next & (int)PLA_CFG_BITS);
    if (v == g_settings.game_opts && !done) return;
    g_settings.game_opts = v;
    if (done) g_settings.game_opts_next = -1;
    settings_save();
}
static void cfg_sound(Cpu *c, uint32_t f) {  // (the volumes and the dynamic range as the field has them)
    uint64_t engine = *(const uint64_t *)(uintptr_t)PLA_SOUND_ENGINE;
    if (!engine) return;
    static const struct { uint64_t hash, name, len, zero; } rtpc[3] = {  // (the game's own RTPC names: BGM, SE, PMVOICE)
        {0x1c56beb784e02b07ull, 0x70037c1cbaull, 13, 0}, {0xe0cb2cb772a51543ull, 0x7003856ebaull, 12, 0},
        {0x12405c2019b3865aull, 0x70037dedfdull, 17, 0}};
    for (int i = 0; i < 3; i++) {
        float v = (float)((f >> (6 + 4 * i)) & 15);
        uint32_t b;
        memcpy(&b, &v, 4);
        c->x[0] = engine, c->x[1] = (uint64_t)(uintptr_t)&rtpc[i], c->x[2] = ~0ull, c->x[3] = 0, c->v[0] = b;
        guest_call(c, PLA_SET_RTPC);
    }
    c->x[0] = !(f >> 29 & 1);
    guest_call(c, PLA_SET_DYNAMIC_RANGE);
}
static int cfg_loaded_hook(Cpu *c, uint64_t addr) {  // (PLA_CFG_LOAD and PLA_CFG_DEFAULTS, after the game's own)
    uint64_t cfg = c->x[0], ret = c->x[30];
    rt_call_original(c, addr);
    uint64_t r0 = c->x[0], r1 = c->x[1];
    if (cfg && addr == PLA_CFG_LOAD) cfg_seen(cfg, false);
    int next = g_settings.game_opts_next;
    if (cfg && next >= 0 && ret != PLA_CFG_RESTORE_RET) {  // (the player's Restore Defaults in the game: theirs)
        uint32_t *f = (uint32_t *)(uintptr_t)(cfg + 0x68);
        LOG("[options] the game's settings from the launcher: 0x%08x (the save had 0x%08x)\n", next, *f & PLA_CFG_BITS);
        *f = (*f & ~PLA_CFG_BITS) | ((uint32_t)next & PLA_CFG_BITS);
        if (addr == PLA_CFG_LOAD) cfg_sound(c, *f);  // (a new game's: the sound engine may not be up yet)
    }
    c->x[0] = r0, c->x[1] = r1;
    return 1;
}
static int cfg_save_hook(Cpu *c, uint64_t addr) {
    if (c->x[0]) cfg_seen(c->x[0], true);
    (void)addr;
    return 0;  // (the game's own runs)
}

void pla_options_configure(void) {
    const char *e = getenv("LARECOMP_PLA_OPTIONS");
    if (!(e && atoi(e) == 0)) {
        rt_register_fnhook_at(OPT_BUILD_ROWS, build_rows_hook);
        rt_register_fnhook_at(OPT_BIND, bind_hook);
        rt_register_fnhook_at(OPT_SHOW_VALUE, show_value_hook);
        rt_register_fnhook_at(OPT_CHANGE, change_hook);
        rt_register_fnhook_at(OPT_DESCRIPTION, description_hook);
        rt_register_fnhook_at(OPT_TEXT, text_hook);
        rt_register_fnhook_at(OPT_TEXT_APPLY, text_apply_hook);
    }
    rt_register_fnhook_at(PLA_CFG_LOAD, cfg_loaded_hook);
    rt_register_fnhook_at(PLA_CFG_DEFAULTS, cfg_loaded_hook);
    rt_register_fnhook_at(PLA_CFG_SAVE, cfg_save_hook);
    const char *tf = getenv("LARECOMP_ZA_OPTTRACE");
    FILE *f = tf ? fopen(tf, "r") : NULL;
    unsigned long long a;
    while (f && g_ntraced < 4096 && fscanf(f, "%llx", &a) == 1) g_traced[g_ntraced++].addr = a, rt_register_fnhook_at(a, trace_hook);
    if (f) fclose(f), LOG("[options] tracing %d functions\n", g_ntraced);
}
