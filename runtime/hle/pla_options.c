// Legends: Arceus's Settings screen (Menu -> Help -> Settings) with the start screen's settings in it: rows added
// after the game's twelve, drawn and driven by the game's code like its own.
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

// ------------------------------------------------------------------ the rows

enum { CHOICE, BAR10 };
typedef struct {
    const char *name, *info;
    int kind, n;
    const char *choice[5];
    int (*get)(void);
    void (*set)(int);
} Row;

static const int k_fps[] = {30, 60, 120, 144}, k_lines[] = {0, 720, 1080, 1440, 2160};
static int find(const int *v, int n, int x) {
    for (int i = 0; i < n; i++) if (v[i] == x) return i;
    return 0;
}
static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }
static int get_fps(void) { return find(k_fps, 4, g_settings.next_fps ? g_settings.next_fps : g_settings.fps); }
static void set_fps(int i) { g_settings.next_fps = k_fps[i] == g_settings.fps ? 0 : k_fps[i]; }
static int get_res(void) { return find(k_lines, 5, g_settings.next_res_height ? g_settings.next_res_height - 1 : g_settings.res_height); }
static void set_res(int i) { g_settings.next_res_height = k_lines[i] == g_settings.res_height ? 0 : k_lines[i] + 1; }
static int get_full(void) { return g_settings.fullscreen ? 1 : 0; }
static void set_full(int i) { g_settings.fullscreen = i == 1, menu_apply_fullscreen(); }
static int get_aa(void) { return g_settings.antialias == AA_SMAA ? 0 : 1; }
static void set_aa(int i) { g_settings.antialias = i == 0 ? AA_SMAA : AA_OFF; }
static int get_prompts(void) { return g_settings.button_prompts >= 0 && g_settings.button_prompts <= 4 ? g_settings.button_prompts : 0; }
static void set_prompts(int i) { g_settings.button_prompts = i; }
static int get_fpsc(void) { return g_settings.show_fps ? 0 : 1; }
static void set_fpsc(int i) { g_settings.show_fps = i == 0; }
static int get_skip(void) { return g_settings.skip_start ? 1 : 0; }
static void set_skip(int i) { g_settings.skip_start = i == 1; }
static int get_mcam(void) { return g_settings.mouse_camera ? 0 : 1; }
static void set_mcam(int i) { g_settings.mouse_camera = i == 0; }
// (bars: the nearest step to what the ini has; mouse 0.025 degrees per pixel a step, dead zone 4% of the travel a step)
static int get_msens(void) { return clampi((int)(g_settings.mouse_sens / 0.025f + 0.5f) - 1, 0, 10); }
static void set_msens(int v) { g_settings.mouse_sens = 0.025f * (float)(v + 1); }
static int get_dz(void) { return clampi((int)(g_settings.stick_deadzone / 0.04f + 0.5f), 0, 10); }
static void set_dz(int v) { g_settings.stick_deadzone = 0.04f * (float)v; }
static int get_vol(void) { return clampi((int)(g_settings.volume * 10 + 0.5f), 0, 10); }
static void set_vol(int v) { g_settings.volume = (float)v / 10; }

static const Row k_rows[] = {
    {"Frame Rate", "Choose how many frames are drawn a second. Applies the next time you start the game.", CHOICE, 4,
     {"30 fps", "60 fps", "120 fps", "144 fps"}, get_fps, set_fps},
    {"Resolution", "Choose the resolution the game is drawn at. Applies the next time you start the game.", CHOICE, 5,
     {"Auto", "720p", "1080p", "1440p", "4K"}, get_res, set_res},
    {"Screen Mode", "Choose whether the game fills the screen or plays in a window.", CHOICE, 2, {"Window", "Fullscreen"},
     get_full, set_full},
    {"Anti-Aliasing", "Choose whether to smooth jagged edges (SMAA).", CHOICE, 2, {"Enabled", "Disabled"}, get_aa, set_aa},
    {"Button Prompts", "Choose which buttons prompts show. Auto shows the ones you are playing with.", CHOICE, 5,
     {"Auto", "Switch", "Xbox", "PlayStation", "Keyboard"}, get_prompts, set_prompts},
    {"FPS Counter", "Choose whether to show the frame rate in the corner of the screen.", CHOICE, 2, {"Show", "Hide"},
     get_fpsc, set_fpsc},
    {"Start Screen", "Choose whether the start screen shows when the game starts. Holding Shift while starting shows it.",
     CHOICE, 2, {"Show", "Skip"}, get_skip, set_skip},
    {"Mouse Camera", "Choose whether a click in the game lets the mouse turn the camera. Esc lets it go.", CHOICE, 2,
     {"Enabled", "Disabled"}, get_mcam, set_mcam},
    {"Mouse Sensitivity", "Choose how far the camera turns when you move the mouse.", BAR10, 0, {0}, get_msens, set_msens},
    {"Stick Dead Zone", "Choose how far a controller's sticks must move before they count.", BAR10, 0, {0}, get_dz, set_dz},
    {"Master Volume", "Adjust the volume of all of the game's sound.", BAR10, 0, {0}, get_vol, set_vol},
};
#define NROWS (int)(sizeof k_rows / sizeof *k_rows)
static int g_first[NROWS];  // CHOICE rows of more than 3: the first of the three shown

static int row_of(uint32_t id) { return id >= FIRST_ID && id < FIRST_ID + (uint32_t)NROWS ? (int)(id - FIRST_ID) : -1; }
static void keep_shown(int r, int v) {
    if (v < g_first[r]) g_first[r] = v;
    if (v > g_first[r] + 2) g_first[r] = v - 2;
    g_first[r] = clampi(g_first[r], 0, k_rows[r].n - 3);
}
static int shown_value(int r) {
    const Row *w = &k_rows[r];
    int v = w->get();
    if (w->kind != CHOICE || w->n <= 3) return v;
    keep_shown(r, v);
    return v - g_first[r];
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
    int n = w->n > 3 ? 3 : w->n, first = w->n > 3 ? (shown_value(r), g_first[r]) : 0;
    if (texts) {
        guest(c, OPT_HEADING, (uint64_t[]){view, slot, our_label(w->name)}, 3, 0);
        for (int i = 0; i < n; i++) guest(c, OPT_CHOICE_LABEL, (uint64_t[]){view, (uint64_t)i, slot, our_label(w->choice[first + i])}, 4, 0);
        guest(c, OPT_CHOICE_LAYOUT, (uint64_t[]){view, slot}, 2, n == 3 ? 1.0f : 0.0f);
    }
    guest(c, OPT_CHOICES, (uint64_t[]){view, (uint64_t)shown_value(r), (uint64_t)n, slot, active}, 5, 0);
}

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
    uint32_t id = *(const uint32_t *)(uintptr_t)(*(const uint64_t *)(uintptr_t)(model + 0xa8) + 8 * (uint32_t)c->x[1]);
    int r = row_of(id);
    if (r < 0) return 0;
    draw_row(c, view, r, c->x[3], true, *(const uint32_t *)(uintptr_t)(view + 0x5b8) == id);
    return 1;
}

static int show_value_hook(Cpu *c, uint64_t addr) {  // (view, id, slot, active)
    (void)addr;
    int r = row_of((uint32_t)c->x[1]);
    if (r < 0) return 0;
    draw_row(c, c->x[0], r, c->x[2], k_rows[r].kind == CHOICE && k_rows[r].n > 3, c->x[3] & 1);  // (sliding: texts too)
    return 1;
}

static int change_hook(Cpu *c, uint64_t addr) {  // (model, id, op)
    (void)addr;
    int r = row_of((uint32_t)c->x[1]), op = (int)(uint32_t)c->x[2];
    if (r < 0 || (op != 0 && op != 1)) return 0;
    const Row *w = &k_rows[r];
    int v = w->get(), to = clampi(v + (op ? 1 : -1), 0, w->kind == BAR10 ? 10 : w->n - 1);
    if (to != v) w->set(to), settings_save();
    if (w->kind == CHOICE && w->n > 3) keep_shown(r, to);
    c->x[0] = to != v;
    return 1;
}

static int description_hook(Cpu *c, uint64_t addr) {  // (view, id) -> [x8] string
    int r = row_of((uint32_t)c->x[1]);
    if (r < 0) return 0;
    uint64_t out = c->x[8];
    c->x[1] = 0;
    rt_call_original(c, addr);
    uint8_t *str = (uint8_t *)(uintptr_t)*(const uint64_t *)(uintptr_t)out;
    if (str) put_text(str, k_rows[r].info, 48);
    return 1;
}

// what runtime/hle/menu_mouse.c asks (the game's own rows: not known here)
bool settings_cursor_row(Cpu *c, uint64_t *type, int *value) { (void)c, (void)type, (void)value; return false; }
int settings_tab(void) { return -1; }
uint64_t settings_screen_buttons(Cpu *c, uint64_t b) { (void)c; return b; }

// ------------------------------------------------------------------ research
static _Atomic int g_trace_on, g_trace_gen;
static struct { uint64_t addr; int gen, n; } g_traced[1024];
static int g_ntraced;

static int trace_hook(Cpu *c, uint64_t addr) {
    if (!atomic_load(&g_trace_on)) return 0;
    for (int i = 0; i < g_ntraced; i++)
        if (g_traced[i].addr == addr) {
            int gen = atomic_load(&g_trace_gen);
            if (g_traced[i].gen != gen) g_traced[i].gen = gen, g_traced[i].n = 0;
            if (g_traced[i].n++ >= 40) return 0;
            break;
        }
    LOG("[opttrace] %llx from %llx x0 %llx x1 %llx x2 %llx x3 %llx\n", (unsigned long long)addr, (unsigned long long)c->x[30],
        (unsigned long long)c->x[0], (unsigned long long)c->x[1], (unsigned long long)c->x[2], (unsigned long long)c->x[3]);
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
    const char *tf = getenv("LARECOMP_ZA_OPTTRACE");
    FILE *f = tf ? fopen(tf, "r") : NULL;
    unsigned long long a;
    while (f && g_ntraced < 1024 && fscanf(f, "%llx", &a) == 1) g_traced[g_ntraced++].addr = a, rt_register_fnhook_at(a, trace_hook);
    if (f) fclose(f), LOG("[options] tracing %d functions\n", g_ntraced);
}
