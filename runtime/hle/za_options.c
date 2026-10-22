// Z-A's Settings screen (main menu -> Help & Settings -> Settings) with the start screen's settings in it: rows added
// after the game's own three on each tab, drawn and driven by the game's code like its own.
//
// The screen (GameSettingPage, code 0x7002c7b000..0x7002c85000; layout ui/data/option/option_gamesetting_00.arc):
// three tabs, each a scrolling list (7 rows on screen, one row layout reused) of three rows. The page P keeps its state
// at [P+0x118] (tab +0xb8, cursor row +0xbc), its text view at [P+0x128]. The list asks for its row count
// (f_7002c7f778: 3 per tab), then binds each row on screen (f_7002c818c8) from its type and value (f_7002c80230:
// x0 = the type - 0 three choices, 1 two choices, 2 a bar: 5 steps 1..5 when the high word is 0, else 10, 0..10 - and
// x1 = the value) and its texts (f_7002c83760 -> per tab: the name f_7002c83d04, the choices f_7002c83da4 /
// f_7002c83ef0, a bar's number f_7002c840bc). Left / right compute the next value from the type and store it
// (f_7002c806dc -> per tab), then redraw the row (f_7002c80708); the cursor's row shows its description
// (f_7002c83a94). Past row 2 the per-tab functions stop the game (or show nothing): those six are hooked for the rows
// added here. Text: every one of those goes through the message lookup (0x7000410a30, (manager, file, label) -> text
// object: UTF-16 at +0x30, length +0x52), asked with one of the game's labels while ours waits in a queue; the text it
// returns is replaced by ours.
//
// Five choices or more don't fit the three a row shows: those rows show three at a time, moving along as the value
// does. Frame rate and resolution are read at start: a change is kept for the next start (Settings.next_*).
// LARECOMP_ZA_OPTIONS=0: the game's screen as it is.
//
// Research: LARECOMP_ZA_ARCDUMP=<name>: every SARC archive the game decompresses (OodleLZ_Decompress, 0x7001a9c9e0)
// whose file names include <name> is written to arcdump_<n>.sarc in the working directory.
// LARECOMP_ZA_OPTTRACE=<file of hex addresses>: while LARECOMP_LIVE's dir has a file "opttrace", each entry of those
// functions is logged ("[opttrace] <fn> from <lr> x0..x3"), at most 40 times per function per "opttrace" file.
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

#define ZA_OODLE_DECOMPRESS 0x7001a9c9e0ull  // (comp, comp size, raw, raw size, ...) -> raw size
#define ZA_MSG_LOOKUP 0x7000410a30ull        // (manager, &file label, &label, language) -> [x8] text object
#define ZA_MSG_TEXT_VTABLE 0x7003bd7038ull
#define OPT_ROW_COUNT 0x7002c7f778ull   // (page state) -> rows of its tab
#define OPT_ROW_TYPE 0x7002c80230ull    // (page, row) -> x0 type, x1 value
#define OPT_ROW_SET 0x7002c806dcull     // (page, row, value)
#define OPT_ROW_SHOW 0x7002c80708ull    // (page, row, type, value, animate)
#define OPT_ROW_TEXTS 0x7002c83760ull   // (view, row item, tab, row, value)
#define OPT_ROW_INFO 0x7002c83a94ull    // (view, tab, row)
#define OPT_SET_NAME 0x7002c83d04ull    // (view, row item, &label)
#define OPT_SET_CHOICES2 0x7002c83da4ull  // (view, row item, &label, &label)
#define OPT_SET_CHOICES3 0x7002c83ef0ull  // (view, row item, &label, &label, &label)
#define OPT_SET_NUMBER 0x7002c840bcull  // (view, row item, number)
#define OPT_SET_INFO 0x7002c84210ull    // (view, &label)

// ------------------------------------------------------------------ the rows

enum { CHOICE, BAR5, BAR10 };
typedef struct {
    int tab;  // 0 Gameplay, 1 Camera, 2 Sound
    const char *name, *info;
    int kind, n;  // CHOICE: n choices
    const char *choice[5];
    int (*get)(void);
    void (*set)(int);
} Row;

static const int k_fps[] = {30, 60, 120, 144}, k_lines[] = {0, 720, 1080, 1440, 2160};
static int find(const int *v, int n, int x) {
    for (int i = 0; i < n; i++) if (v[i] == x) return i;
    return 0;
}
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
static int get_mcam(void) { return g_settings.mouse_camera ? 0 : 1; }
static void set_mcam(int i) { g_settings.mouse_camera = i == 0; }
// (bars: the nearest step to what the ini has; mouse 0.025 degrees per pixel a step, dead zone 4% of the travel a step)
static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }
static int get_msens(void) { return clampi((int)(g_settings.mouse_sens / 0.025f + 0.5f) - 1, 0, 10); }
static void set_msens(int v) { g_settings.mouse_sens = 0.025f * (float)(v + 1); }
static int get_dz(void) { return clampi((int)(g_settings.stick_deadzone / 0.04f + 0.5f), 0, 10); }
static void set_dz(int v) { g_settings.stick_deadzone = 0.04f * (float)v; }
static int get_vol(void) { return clampi((int)(g_settings.volume * 10 + 0.5f), 0, 10); }
static void set_vol(int v) { g_settings.volume = (float)v / 10; }

static const Row k_rows[] = {
    {0, "Frame Rate", "Choose how many frames are drawn a second. Applies the next time you start the game.",
     CHOICE, 4, {"30 fps", "60 fps", "120 fps", "144 fps"}, get_fps, set_fps},
    {0, "Resolution", "Choose the resolution the game is drawn at. Applies the next time you start the game.",
     CHOICE, 5, {"Auto", "720p", "1080p", "1440p", "4K"}, get_res, set_res},
    {0, "Screen Mode", "Choose whether the game fills the screen or plays in a window.", CHOICE, 2, {"Window", "Fullscreen"},
     get_full, set_full},
    {0, "Anti-Aliasing", "Choose whether to smooth jagged edges (SMAA).", CHOICE, 2, {"On", "Off"}, get_aa, set_aa},
    {0, "Button Prompts", "Choose which buttons prompts show. Auto shows the ones you are playing with.", CHOICE, 5,
     {"Auto", "Switch", "Xbox", "PlayStation", "Keyboard"}, get_prompts, set_prompts},
    {0, "FPS Counter", "Choose whether to show the frame rate in the corner of the screen.", CHOICE, 2, {"Show", "Hide"},
     get_fpsc, set_fpsc},
    {1, "Mouse Camera", "Choose whether a click in the game lets the mouse turn the camera. Esc lets it go.",
     CHOICE, 2, {"On", "Off"}, get_mcam, set_mcam},
    {1, "Mouse Sensitivity", "Choose how far the camera turns when you move the mouse.", BAR10, 0, {0}, get_msens, set_msens},
    {1, "Stick Dead Zone", "Choose how far a controller's sticks must move before they count.", BAR10, 0, {0}, get_dz, set_dz},
    {2, "Master Volume", "Adjust the volume of all of the game's sound.", BAR10, 0, {0}, get_vol, set_vol},
};
#define NROWS (int)(sizeof k_rows / sizeof *k_rows)
static int g_first[NROWS];  // CHOICE rows of more than 3: the first of the three shown

static int added(int tab) {
    int n = 0;
    for (int i = 0; i < NROWS; i++) n += k_rows[i].tab == tab;
    return n;
}
static int row_index(int tab, int row) {  // (row 3 on: ours) -> k_rows index, or -1
    if (row < 3) return -1;
    for (int i = 0, k = 3; i < NROWS; i++)
        if (k_rows[i].tab == tab && k++ == row) return i;
    return -1;
}
static int tab_of(uint64_t page) { return *(const int32_t *)(uintptr_t)(*(const uint64_t *)(uintptr_t)(page + 0x118) + 0xb8); }

static void keep_shown(int r, int v) {  // (the three choices shown include v)
    const Row *w = &k_rows[r];
    if (v < g_first[r]) g_first[r] = v;
    if (v > g_first[r] + 2) g_first[r] = v - 2;
    g_first[r] = clampi(g_first[r], 0, w->n - 3);
}
static uint64_t type_of(const Row *w) {
    if (w->kind == BAR5) return 2;
    if (w->kind == BAR10) return 0xa00000002ull;
    return w->n == 2 ? 0xa00000001ull : 0xa00000000ull;
}
static int shown_value(int r) {  // the value as the row shows it
    const Row *w = &k_rows[r];
    int v = w->get();
    if (w->kind != CHOICE || w->n <= 3) return v;
    keep_shown(r, v);
    return v - g_first[r];
}

// ------------------------------------------------------------------ text

typedef struct { uint64_t hash; const char *str; uint64_t len, zero; } Label;  // (the game's message label)
static Label make_label(const char *s) {
    uint64_t h = 0xcbf29ce484222645ull;  // (FNV-1a)
    for (const char *p = s; *p; p++) h = (h ^ (uint8_t)*p) * 0x100000001b3ull;
    return (Label){h, s, strlen(s), 0};
}
static Label g_label;  // (any label of the game's: its text is replaced)

static __thread const char *t_text[4];  // the texts the next lookups give, in order
static __thread int t_ntext, t_next;

static int msg_lookup_hook(Cpu *c, uint64_t addr) {
    if (t_next >= t_ntext) return 0;
    uint64_t res = c->x[8];
    rt_call_original(c, addr);
    const char *s = t_text[t_next++];
    uint8_t *obj = (uint8_t *)(uintptr_t)*(const uint64_t *)(uintptr_t)res;
    if (!obj || *(const uint64_t *)obj != ZA_MSG_TEXT_VTABLE) return 1;
    uint16_t *buf = *(uint16_t **)(obj + 0x30);
    uint64_t cap = *(const uint64_t *)(obj + 0x40);
    size_t n = 0, col = 0;
    for (const uint8_t *p = (const uint8_t *)s; *p && n + 1 < cap;) {  // (UTF-8 -> UTF-16, up to 3-byte sequences)
        if (*p == ' ') {  // (the text box doesn't wrap - it squeezes a long line: lines of up to 44 characters)
            size_t w = 1;
            while (p[w] && p[w] != ' ') w++;
            if (col + w > 44) { buf[n++] = '\n', col = 0, p++; continue; }
        }
        uint32_t u = *p++;
        if (u >= 0xe0 && p[0] && p[1]) u = (u & 0xf) << 12 | (p[0] & 0x3fu) << 6 | (p[1] & 0x3fu), p += 2;
        else if (u >= 0xc0 && p[0]) u = (u & 0x1f) << 6 | (p[0] & 0x3fu), p++;
        buf[n++] = (uint16_t)u, col++;
    }
    buf[n] = 0;
    *(uint16_t *)(obj + 0x52) = (uint16_t)n;
    return 1;
}

// the game's function fn(x0..x4) with the texts its lookups give
static void call_with_texts(Cpu *c, uint64_t fn, const uint64_t *args, int nargs, const char *const *texts, int ntexts) {
    uint64_t save[5];
    memcpy(save, c->x, sizeof save);
    for (int i = 0; i < nargs; i++) c->x[i] = args[i];
    for (int i = 0; i < ntexts; i++) t_text[i] = texts[i];
    t_ntext = ntexts, t_next = 0;
    guest_call(c, fn);
    t_ntext = t_next = 0;
    memcpy(c->x, save, sizeof save);
}

// ------------------------------------------------------------------ hooks

static int row_count_hook(Cpu *c, uint64_t addr) {
    (void)addr;
    int tab = *(const int32_t *)(uintptr_t)(c->x[0] + 0xb8);
    c->x[0] = tab >= 0 && tab < 3 ? 3 + (uint64_t)added(tab) : 0;
    return 1;
}

static uint64_t g_page, g_page_vt;  // (the Settings screen last drawn, its class: za_options_cursor_row)

static int row_type_hook(Cpu *c, uint64_t addr) {
    (void)addr;
    g_page = c->x[0], g_page_vt = *(const uint64_t *)(uintptr_t)c->x[0];
    int r = row_index(tab_of(c->x[0]), (int)(uint32_t)c->x[1]);
    if (r < 0) return 0;
    c->x[0] = type_of(&k_rows[r]);
    c->x[1] = (uint64_t)shown_value(r);
    return 1;
}

static int row_set_hook(Cpu *c, uint64_t addr) {
    (void)addr;
    int r = row_index(tab_of(c->x[0]), (int)(uint32_t)c->x[1]);
    if (r < 0) return 0;
    const Row *w = &k_rows[r];
    int v = (int)(uint32_t)c->x[2];
    if (w->kind == CHOICE && w->n > 3) {  // (shown 0..2, moving on past either end: +1 or -1 of the value)
        int cur = w->get(), was = cur - g_first[r];
        v = v == (was + 1) % 3 ? (cur + 1) % w->n : (cur + w->n - 1) % w->n;
        keep_shown(r, v);
    }
    w->set(v);
    settings_save();
    return 1;
}

static int row_show_hook(Cpu *c, uint64_t addr) {
    int r = row_index(tab_of(c->x[0]), (int)(uint32_t)c->x[1]);
    if (r >= 0) c->x[3] = (uint64_t)shown_value(r);  // (moved along: not the value the game worked out)
    return 0;
}

static int row_texts_hook(Cpu *c, uint64_t addr) {
    (void)addr;
    int r = row_index((int)(uint32_t)c->x[2], (int)(uint32_t)c->x[3]);
    if (r < 0) return 0;
    const Row *w = &k_rows[r];
    uint64_t view = c->x[0], item = c->x[1], lab = (uint64_t)(uintptr_t)&g_label;
    call_with_texts(c, OPT_SET_NAME, (uint64_t[]){view, item, lab}, 3, &w->name, 1);
    if (w->kind != CHOICE) {
        call_with_texts(c, OPT_SET_NUMBER, (uint64_t[]){view, item, (uint64_t)w->get()}, 3, NULL, 0);
        return 1;
    }
    int first = w->n > 3 ? (shown_value(r), g_first[r]) : 0;
    if (w->n == 2) call_with_texts(c, OPT_SET_CHOICES2, (uint64_t[]){view, item, lab, lab}, 4, w->choice, 2);
    else call_with_texts(c, OPT_SET_CHOICES3, (uint64_t[]){view, item, lab, lab, lab}, 5, w->choice + first, 3);
    return 1;
}

static int row_info_hook(Cpu *c, uint64_t addr) {
    (void)addr;
    int r = row_index((int)(uint32_t)c->x[1], (int)(uint32_t)c->x[2]);
    if (r < 0) return 0;
    call_with_texts(c, OPT_SET_INFO, (uint64_t[]){c->x[0], (uint64_t)(uintptr_t)&g_label}, 2, &k_rows[r].info, 1);
    return 1;
}

int za_options_tab(void) {  // the Settings screen's tab (runtime/hle/za_mouse.c), -1 when it isn't up
    if (!g_page || *(const uint64_t *)(uintptr_t)g_page != g_page_vt) return -1;
    return tab_of(g_page);
}

// The cursor's row on the Settings screen (runtime/hle/za_mouse.c: a click on a choice or a bar): its type and value,
// as left / right see them. (Asked while its rows are on screen: the page is the one they were bound by.)
bool za_options_cursor_row(Cpu *c, uint64_t *type, int *value) {
    if (!g_page || *(const uint64_t *)(uintptr_t)g_page != g_page_vt) return false;
    uint64_t save[31];
    memcpy(save, c->x, sizeof save);
    c->x[0] = g_page;
    c->x[1] = (uint64_t)*(const uint32_t *)(uintptr_t)(*(const uint64_t *)(uintptr_t)(g_page + 0x118) + 0xbc);
    guest_call(c, OPT_ROW_TYPE);
    *type = c->x[0], *value = (int)(uint32_t)c->x[1];
    memcpy(c->x, save, sizeof save);
    return true;
}

// ------------------------------------------------------------------ research

static bool has(const uint8_t *p, size_t n, const char *s) {
    size_t k = strlen(s);
    for (size_t i = 0; k && i + k <= n; i++)
        if (p[i] == (uint8_t)s[0] && !memcmp(p + i, s, k)) return true;
    return false;
}

// The description line sits where the list's rows 5-7 are (it was below three): moved under the list (its bottom
// at -293, the bottom line at -451), in the layout as it is decompressed. (+ research: LARECOMP_ZA_ARCDUMP)
static void move_description(uint8_t *arc, size_t n) {
    static const char name[] = "T_explanation_00";
    for (size_t i = 0x0c; i + 0x34 <= n; i++)
        if (arc[i] == 'T' && !memcmp(arc + i, name, sizeof name) && !memcmp(arc + i - 0x0c, "txt1", 4)) {
            float *y = (float *)(arc + i - 0x0c + 0x30);
            if (*y == -224.0f) *y = -372.0f;
            return;
        }
}

static int decompress_hook(Cpu *c, uint64_t addr) {
    uint64_t raw = c->x[2];
    rt_call_original(c, addr);
    int64_t n = (int64_t)c->x[0];
    if (n < 0x20 || memcmp((const void *)(uintptr_t)raw, "SARC", 4)) return 1;
    uint8_t *arc = (uint8_t *)(uintptr_t)raw;
    size_t head = (size_t)(n < 0x10000 ? n : 0x10000);  // (the file names)
    if (has(arc, head, "blyt/option_gamesetting_00.bflyt")) move_description(arc, (size_t)n);
    const char *want = getenv("LARECOMP_ZA_ARCDUMP");
    if (!want || !has(arc, head, want)) return 1;
    static int k;
    char path[64];
    snprintf(path, sizeof path, "arcdump_%d.sarc", k++);
    FILE *f = fopen(path, "wb");
    if (f) fwrite(arc, 1, (size_t)n, f), fclose(f);
    LOG("[options] %s: %lld bytes\n", path, (long long)n);
    return 1;
}

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
void za_options_present(void) {
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

void za_options_configure(void) {
    const char *e = getenv("LARECOMP_ZA_OPTIONS");
    if (!(e && atoi(e) == 0)) {
        g_label = make_label("msg_ui_option_01");
        rt_register_fnhook_at(ZA_MSG_LOOKUP, msg_lookup_hook);
        rt_register_fnhook_at(OPT_ROW_COUNT, row_count_hook);
        rt_register_fnhook_at(OPT_ROW_TYPE, row_type_hook);
        rt_register_fnhook_at(OPT_ROW_SET, row_set_hook);
        rt_register_fnhook_at(OPT_ROW_SHOW, row_show_hook);
        rt_register_fnhook_at(OPT_ROW_TEXTS, row_texts_hook);
        rt_register_fnhook_at(OPT_ROW_INFO, row_info_hook);
        rt_register_fnhook_at(ZA_OODLE_DECOMPRESS, decompress_hook);
    }
    const char *tf = getenv("LARECOMP_ZA_OPTTRACE");
    FILE *f = tf ? fopen(tf, "r") : NULL;
    unsigned long long a;
    while (f && g_ntraced < 1024 && fscanf(f, "%llx", &a) == 1) g_traced[g_ntraced++].addr = a, rt_register_fnhook_at(a, trace_hook);
    if (f) fclose(f), LOG("[options] tracing %d functions\n", g_ntraced);
    if (getenv("LARECOMP_ZA_ARCDUMP")) rt_register_fnhook_at(ZA_OODLE_DECOMPRESS, decompress_hook);
}
