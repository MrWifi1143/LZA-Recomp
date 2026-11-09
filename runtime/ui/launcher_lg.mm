// PL:A / PL:Z-A Recomp launcher, an old-PC-game launcher (Fallout: New Vegas style): a fixed window with the game's
// banner (drawn here, no game art), Play, and pages for Options, Controls, Game Files and Mods; the game opens at its
// own size / fullscreen after Play. The classic start screen (one centered window with everything, made for a TV and
// a controller) is still here for console ports: LARECOMP_START_SCREEN=classic, or built with
// -DRT_CLASSIC_START_SCREEN=1 (=launcher picks the launcher there). Both show the same settings (settings_* below).
// What they hold (from Let's Go Recomp's start screen): the game files (picked once, set up and recompiled here:
// runtime/gamesetup.c), the resolution, fullscreen, anti-aliasing, a mod (mods/: runtime/mods.c), the controller (what is
// connected, a live test of every button and stick, remapping) and the keyboard, Play. In English, or in Portuguese when the system's language is (LARECOMP_LANG=en|pt
// chooses). The choices go to larecomp.ini (settings.c; bind_key_* from runtime/hle/bindings.c).
// Tests: LARECOMP_LAUNCHER_SHOT=<frame> saves build/launcher.png (or LARECOMP_SHOT_PATH) and exits;
// LARECOMP_LAUNCHER_AUTOPLAY=1 plays as soon as the game files are ready; LARECOMP_SETUP_DIR=<folder> sets them up
// from that folder at once; LARECOMP_MOD_INSTALL=<file or folder> installs a translation at once;
// LARECOMP_LAUNCHER_KEYS=1 opens the keyboard section, =<slot> also waits for a new key for that input
// (runtime/input_bind.h BIND_*), and <slot>:<key name> presses that key for it; LARECOMP_LAUNCHER_PAGE=options|
// controls|files|mods opens that launcher page; LARECOMP_LAUNCHER_SIZE=<w>x<h> makes the launcher's window that size.
// Any window size: drawn for 1280x720 and scaled to the window (ui_scale_*: a window the desktop stretched, F11 /
// Alt+Enter fullscreen, a TV). A controller drives all of it: A selects, B goes back, L / R change tabs (the game's
// own A and B: bindings.c), and the Controller tab's button test is a mode of its own (pad_nav_update).
#include <SDL3/SDL.h>
#include <float.h>
#include <math.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include <string>
#include <vector>

#include "imgui.h"
#include "imgui_internal.h"  // (the input queue: ui_scale_apply)
#include "imgui_impl_vulkan.h"
#include "imgui_impl_sdl3.h"

extern "C" void vk_describe_gpu(char *name, size_t n, int *vram_gb, bool *integrated);  // nvn/vulkan/device.c
extern "C" {
#include "gamesetup.h"
#include "input_bind.h"
#include "mods.h"
#include "platform.h"
#include "plugin.h"
#include "paths.h"
#include "settings.h"
void settings_save(void);
void ui_window_init(void);
void ui_window_size_hint(int w, int h);  // (nvn/vulkan/device.c)
void settings_load(void);
SDL_Window *ui_window(void);
void menu_apply_window_requests(void *sdl_window);
bool vk_frame_begin(const float clear[4]);
void vk_frame_end(const char *shot_path);
void *vk_frame_cmd(void);
bool vk_imgui_backend_init(void);
void vk_imgui_backend_shutdown(void);
void vk_imgui_fonts_changed(void);
bool warmup_needed(const char *root);  // runtime/nvn/vulkan/warmup.c
void warmup_start(void);
int warmup_progress(int *done, int *total);
}

// ------------------------------------------------------------------ language
static bool g_pt;
static void pick_language(void) {
    const char *e = getenv("LARECOMP_LANG");
    if (e && *e) { g_pt = !strncmp(e, "pt", 2); return; }
    int n = 0;
    SDL_Locale **l = SDL_GetPreferredLocales(&n);
    g_pt = l && n > 0 && l[0]->language && !strcmp(l[0]->language, "pt");
    SDL_free(l);
}
#define T(en, pt) (g_pt ? (pt) : (en))

// ------------------------------------------------------------------ keyboard
// The game's actions with the key bound to each. Click one, then press the new key (Esc or 5 s cancels; a key already
// in use swaps over). Controllers keep their buttons: nothing to set for them.
static const struct { int slot; const char *en, *pt; } k_actions[] = {
    {BIND_LS_UP, "Left stick up", "Analógico esquerdo cima"},       {BIND_A, "A (or Enter)", "A (ou Enter)"},
    {BIND_LS_DOWN, "Left stick down", "Analógico esquerdo baixo"},   {BIND_B, "B (or Backspace)", "B (ou Backspace)"},
    {BIND_LS_LEFT, "Left stick left", "Analógico esquerdo esq."},    {BIND_X, "X", "X"},
    {BIND_LS_RIGHT, "Left stick right", "Analógico esquerdo dir."},  {BIND_Y, "Y", "Y"},
    {BIND_RS_UP, "Camera up (or the mouse)", "Câmera cima (ou o mouse)"}, {BIND_L, "L", "L"},
    {BIND_RS_DOWN, "Camera down", "Câmera baixo"},                   {BIND_R, "R", "R"},
    {BIND_RS_LEFT, "Camera left", "Câmera esquerda"},                {BIND_ZL, "ZL (or 1)", "ZL (ou 1)"},
    {BIND_RS_RIGHT, "Camera right", "Câmera direita"},               {BIND_ZR, "ZR (or 3)", "ZR (ou 3)"},
    {BIND_UP, "D-pad up", "Direcional cima"},                        {BIND_PLUS, "+", "+"},
    {BIND_DOWN, "D-pad down", "Direcional baixo"},                   {BIND_MINUS, "-", "-"},
    {BIND_LEFT, "D-pad left", "Direcional esquerda"},                {BIND_LSTICK, "Left stick press", "Clique analógico esq."},
    {BIND_RIGHT, "D-pad right", "Direcional direita"},               {BIND_RSTICK, "Right stick press", "Clique analógico dir."},
    {BIND_WALK, "Walk (hold)", "Andar (segurar)"},
};

static const char *key_name(int sc) {
    switch (sc) {
    case SDL_SCANCODE_SPACE: return T("Space", "Espaço");
    case SDL_SCANCODE_RETURN: return "Enter";
    case SDL_SCANCODE_UP: return T("Up arrow", "Seta cima");
    case SDL_SCANCODE_DOWN: return T("Down arrow", "Seta baixo");
    case SDL_SCANCODE_LEFT: return T("Left arrow", "Seta esquerda");
    case SDL_SCANCODE_RIGHT: return T("Right arrow", "Seta direita");
    case 0: return T("(none)", "(nenhuma)");
    default: return bind_key_label(sc);
    }
}

static void focus_first(void);
static void keys_section(void) {
    int cap_dev, cap_slot, ms_left;
    bind_capture_active(&cap_dev, &cap_slot, &ms_left);
    focus_first();
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(ImGui::GetStyle().ItemSpacing.x, 2));
    ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(ImGui::GetStyle().CellPadding.x, 1));
    if (ImGui::BeginTable("##keys", 4, ImGuiTableFlags_SizingFixedFit)) {
        for (size_t i = 0; i < sizeof k_actions / sizeof k_actions[0]; i++) {
            int slot = k_actions[i].slot;
            ImGui::TableNextColumn();
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(T(k_actions[i].en, k_actions[i].pt));
            ImGui::TableNextColumn();
            bool waiting = cap_dev == BIND_DEV_KEY && cap_slot == slot;
            char lbl[96];
            if (waiting) snprintf(lbl, sizeof lbl, "%s %d##k%d", T("Press a key or mouse button...", "Aperte uma tecla ou botão do mouse..."), (ms_left + 999) / 1000, slot);
            else snprintf(lbl, sizeof lbl, "%s##k%d", key_name(g_bind_key[slot]), slot);
            int pushed = 0;
            if (waiting) { ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.55f, 0.42f, 0.12f, 1)); pushed++; }
            if (!bind_is_default_slot(BIND_DEV_KEY, slot)) { ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.98f, 0.82f, 0.38f, 1)); pushed++; }
            if (ImGui::Button(lbl, ImVec2(230, 0))) {
                if (waiting) bind_capture_cancel();
                else bind_capture_begin(BIND_DEV_KEY, slot);
            }
            ImGui::PopStyleColor(pushed);
        }
        ImGui::EndTable();
    }
    ImGui::PopStyleVar(2);
    if (cap_dev == BIND_DEV_KEY)
        ImGui::TextDisabled("%s", T("Press the new key, mouse button or wheel. Esc cancels; one already in use swaps over.",
                                    "Aperte a nova tecla, botão ou roda do mouse. Esc cancela; uma já usada troca de lugar."));
    if (!bind_is_default(BIND_DEV_KEY) && ImGui::Button(T("Restore default keys", "Restaurar teclas padrão"))) bind_reset(BIND_DEV_KEY);
    ImGui::PushItemWidth(420);
    ImGui::Checkbox(T("Mouse camera (click the game to take the mouse, Esc lets it go)",
                      "Câmera no mouse (clique no jogo para prender o mouse, Esc solta)"), &g_settings.mouse_camera);
    ImGui::SliderFloat(T("Mouse sensitivity", "Sensibilidade do mouse"), &g_settings.mouse_sens, 0.02f, 0.30f, "%.2f");
    ImGui::Checkbox(T("Invert mouse vertical", "Inverter mouse na vertical"), &g_settings.mouse_invert_y);
    ImGui::Checkbox(T("Invert mouse horizontal", "Inverter mouse na horizontal"), &g_settings.mouse_invert_x);
    ImGui::PopItemWidth();
}


// ------------------------------------------------------------------ controller
// What is connected, a live test (each Switch input lights up while the controller button bound to it is held; the
// sticks and triggers as the game reads them) and remapping: click an input, then press the controller button for it
// (Esc or 5 s cancels; a button already in use swaps over). Saved in the .ini with the rest (runtime/hle/bindings.c).
static SDL_Gamepad *g_pads[8];
static int g_npads;
static void refresh_pads(void) {
    int n = 0;
    SDL_JoystickID *ids = SDL_GetGamepads(&n);
    g_npads = 0;
    for (int i = 0; ids && i < n && g_npads < 8; i++) {
        SDL_Gamepad *gp = SDL_GetGamepadFromID(ids[i]);
        if (!gp) gp = SDL_OpenGamepad(ids[i]);
        if (gp) g_pads[g_npads++] = gp;
    }
    SDL_free(ids);
}

static bool host_down(int host) {  // on any connected controller
    for (int i = 0; i < g_npads; i++) {
        if (host == PADB_LTRIGGER || host == PADB_RTRIGGER) {
            SDL_GamepadAxis ax = host == PADB_LTRIGGER ? SDL_GAMEPAD_AXIS_LEFT_TRIGGER : SDL_GAMEPAD_AXIS_RIGHT_TRIGGER;
            if (SDL_GetGamepadAxis(g_pads[i], ax) > 16000) return true;
        } else if (host >= 0 && host < SDL_GAMEPAD_BUTTON_COUNT && SDL_GetGamepadButton(g_pads[i], (SDL_GamepadButton)host)) {
            return true;
        }
    }
    return false;
}

static const ImVec4 k_test_color(1.0f, 0.82f, 0.35f, 1);
static void lamp(bool on) {  // a round indicator on the current line
    ImVec2 p = ImGui::GetCursorScreenPos();
    float r = ImGui::GetTextLineHeight() * 0.42f;
    ImVec2 c(p.x + r + 2, p.y + ImGui::GetFrameHeight() * 0.5f);
    ImDrawList *dl = ImGui::GetWindowDrawList();
    dl->AddCircleFilled(c, r, on ? IM_COL32(110, 230, 120, 255) : IM_COL32(60, 60, 68, 255));
    dl->AddCircle(c, r, IM_COL32(150, 150, 160, 255));
    ImGui::Dummy(ImVec2(2 * r + 4, ImGui::GetFrameHeight()));
}

static void stick_view(const char *label, float x, float y, float dz) {
    float size = 110;
    ImGui::BeginGroup();
    ImGui::TextUnformatted(label);
    ImVec2 p = ImGui::GetCursorScreenPos(), c(p.x + size / 2, p.y + size / 2);
    ImDrawList *dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p, ImVec2(p.x + size, p.y + size), IM_COL32(30, 30, 36, 255), 6);
    dl->AddCircle(c, size / 2 - 4, IM_COL32(120, 120, 130, 255), 48);
    dl->AddCircle(c, (size / 2 - 4) * dz, IM_COL32(90, 70, 40, 255), 32);  // the dead zone
    dl->AddCircleFilled(ImVec2(c.x + x * (size / 2 - 4), c.y + y * (size / 2 - 4)), 7, IM_COL32(110, 230, 120, 255));
    ImGui::Dummy(ImVec2(size, size));
    ImGui::Text("%+.2f %+.2f", x, y);
    ImGui::EndGroup();
}

static void trigger_view(const char *label, float v) {
    ImGui::BeginGroup();
    ImGui::TextUnformatted(label);
    ImGui::ProgressBar(v, ImVec2(110, 0), "");
    ImGui::EndGroup();
}

// The menu with a controller. ImGui moves the focus with the d-pad / stick and selects / goes back with the game's own A
// and B (where the A and B inputs are bound: by position, the right face button A, as in the game and its menus). The
// Controller tab's button test is a mode: while it's on the buttons light their lamps and move nothing (held + and -
// end it); after it, and after a new binding, the menu ignores the controller until every button is let go.
static bool g_pad_test, g_pad_hold;
static bool pad_any_down(void) {
    for (int i = 0; i < g_npads; i++) {
        for (int b = 0; b < SDL_GAMEPAD_BUTTON_COUNT; b++)
            if (SDL_GetGamepadButton(g_pads[i], (SDL_GamepadButton)b)) return true;
        if (SDL_GetGamepadAxis(g_pads[i], SDL_GAMEPAD_AXIS_LEFT_TRIGGER) > 16000 || SDL_GetGamepadAxis(g_pads[i], SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) > 16000)
            return true;
    }
    return false;
}
static void pad_nav_update(ImGuiIO &io, bool test_shown) {  // (each frame, after drawing; test_shown: the Controller section)
    refresh_pads();
    if (!test_shown || ImGui::IsKeyPressed(ImGuiKey_Escape, false)) g_pad_test = false;
    static uint64_t both_since;  // (+ and - held together: the test ends)
    if (g_pad_test && host_down(SDL_GAMEPAD_BUTTON_START) && host_down(SDL_GAMEPAD_BUTTON_BACK)) {
        if (!both_since) both_since = SDL_GetTicks();
        else if (SDL_GetTicks() - both_since >= 800) g_pad_test = false;
    } else both_since = 0;
    int dev = -1;
    bool capture = bind_capture_active(&dev, NULL, NULL) && dev == BIND_DEV_PAD;
    if (g_pad_test || capture) g_pad_hold = true;
    else if (g_pad_hold && !pad_any_down()) g_pad_hold = false;
    if (g_pad_hold) io.ConfigFlags &= ~ImGuiConfigFlags_NavEnableGamepad;
    else io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
    io.ConfigNavSwapGamepadButtons = g_bind_pad[BIND_A] == SDL_GAMEPAD_BUTTON_EAST;  // (ImGui's "activate" is the south button)
}
// A controller button pressed this frame, while the controller drives the menu (ImGui's names: FaceDown the south one)
static bool pad_pressed(ImGuiKey k) {
    return (ImGui::GetIO().ConfigFlags & ImGuiConfigFlags_NavEnableGamepad) && ImGui::IsKeyPressed(k, false);
}
static ImGuiKey pad_cancel_key(void) { return ImGui::GetIO().ConfigNavSwapGamepadButtons ? ImGuiKey_GamepadFaceDown : ImGuiKey_GamepadFaceRight; }
static bool cancel_pressed(void) { return ImGui::IsKeyPressed(ImGuiKey_Escape, false) || pad_pressed(pad_cancel_key()); }
// A page's first control focused when it was opened from the keyboard or a controller (not with the mouse)
static bool g_focus_first, g_focus_wait;  // (wait: a tab chosen with L / R shows from the next frame - not this one's)
static void focus_first(void) {
    if (g_focus_first && !g_focus_wait) ImGui::SetKeyboardFocusHere(), g_focus_first = false;
}
// L / R between a page's tabs: tab_begin(n) before the tab bar, tab_flags(i) for each tab, tab_shown(i) in its body
static int g_tab_cur, g_tab_want = -1, g_tab_to = -1;
static void tab_begin(int n) {
    g_tab_want = -1;
    if (g_tab_cur >= n) g_tab_cur = 0;
    if (pad_pressed(ImGuiKey_GamepadL1)) g_tab_want = (g_tab_cur + n - 1) % n;
    else if (pad_pressed(ImGuiKey_GamepadR1)) g_tab_want = (g_tab_cur + 1) % n;
    if (g_tab_want >= 0) g_tab_to = g_tab_want, g_focus_first = true;
}
static ImGuiTabItemFlags tab_flags(int i) { return g_tab_want == i ? ImGuiTabItemFlags_SetSelected : 0; }
static void tab_shown(int i) {
    g_tab_cur = i;
    g_focus_wait = g_tab_to >= 0 && i != g_tab_to;
    if (i == g_tab_to) g_tab_to = -1;
}

static void controller_section(void) {
    refresh_pads();
    {  // the button test
        int fam = bind_label_family();
        if (!g_pad_test) {
            focus_first();
            if (ImGui::Button(T("Test the buttons", "Testar os botões"))) g_pad_test = true;
            ImGui::SameLine();
            ImGui::TextDisabled("%s", T("(the lamps light up as you press)", "(as luzes acendem ao apertar)"));
        } else {
            if (ImGui::Button(T("Stop testing", "Parar o teste"))) g_pad_test = false;
            ImGui::SameLine();
            ImGui::TextColored(k_test_color, g_pt ? "Testando: segure %s e %s juntos para parar." : "Testing: hold %s and %s together to stop.",
                               bind_pad_label(SDL_GAMEPAD_BUTTON_START, fam), bind_pad_label(SDL_GAMEPAD_BUTTON_BACK, fam));
        }
    }
    if (!g_npads) {
        ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.40f, 1), "%s", T("No controller detected.", "Nenhum controle detectado."));
        ImGui::TextDisabled("%s", T("Connect one (USB or Bluetooth); it shows up here at once.",
                                    "Conecte um (USB ou Bluetooth); ele aparece aqui na hora."));
    } else {
        for (int i = 0; i < g_npads; i++) {
            const char *name = SDL_GetGamepadName(g_pads[i]);
            ImGui::TextColored(ImVec4(0.45f, 0.85f, 0.50f, 1), "%s %d: %s", T("Controller", "Controle"), i + 1, name ? name : "?");
        }
    }
    int fam = bind_label_family();
    int cap_dev, cap_slot, ms_left;
    bind_capture_active(&cap_dev, &cap_slot, &ms_left);
    static const int order[] = {BIND_A, BIND_B, BIND_X, BIND_Y, BIND_L, BIND_R, BIND_ZL, BIND_ZR, BIND_PLUS, BIND_MINUS,
                                BIND_LSTICK, BIND_RSTICK, BIND_UP, BIND_DOWN, BIND_LEFT, BIND_RIGHT};
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(ImGui::GetStyle().ItemSpacing.x, 2));
    int pairs = ImGui::GetContentRegionAvail().x >= 1250 ? 3 : 2;  // (input + button pairs a row: 2 in the launcher's window)
    if (ImGui::BeginTable("##pad", 2 * pairs, ImGuiTableFlags_SizingFixedFit)) {
        for (size_t i = 0; i < sizeof order / sizeof order[0]; i++) {
            int slot = order[i];
            ImGui::TableNextColumn();
            lamp(g_npads && host_down(g_bind_pad[slot]));
            ImGui::SameLine();
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(bind_input_name(slot));
            ImGui::TableNextColumn();
            bool waiting = cap_dev == BIND_DEV_PAD && cap_slot == slot;
            char lbl[96];
            if (waiting) snprintf(lbl, sizeof lbl, "%s %d##p%d", T("Press a button...", "Aperte o botão..."), (ms_left + 999) / 1000, slot);
            else snprintf(lbl, sizeof lbl, "%s##p%d", bind_pad_label(g_bind_pad[slot], fam), slot);
            int pushed = 0;
            if (waiting) { ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.55f, 0.42f, 0.12f, 1)); pushed++; }
            if (!bind_is_default_slot(BIND_DEV_PAD, slot)) { ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.98f, 0.82f, 0.38f, 1)); pushed++; }
            if (ImGui::Button(lbl, ImVec2(200, 0))) {
                if (waiting) bind_capture_cancel();
                else bind_capture_begin(BIND_DEV_PAD, slot);
            }
            ImGui::PopStyleColor(pushed);
        }
        ImGui::EndTable();
    }
    ImGui::PopStyleVar();
    if (cap_dev == BIND_DEV_PAD)
        ImGui::TextDisabled("%s", T("Press the controller button for it. Esc cancels; a button already in use swaps over.",
                                    "Aperte o botão do controle. Esc cancela; um botão já usado troca de lugar."));
    if (g_npads) {
        SDL_Gamepad *gp = g_pads[0];
        float dz = g_settings.stick_deadzone;
        auto ax = [&](SDL_GamepadAxis a) { return SDL_GetGamepadAxis(gp, a) / 32767.0f; };
        stick_view(T("Left stick", "Analógico esquerdo"), ax(SDL_GAMEPAD_AXIS_LEFTX), ax(SDL_GAMEPAD_AXIS_LEFTY), dz);
        ImGui::SameLine(0, 30);
        stick_view(T("Right stick", "Analógico direito"), ax(SDL_GAMEPAD_AXIS_RIGHTX), ax(SDL_GAMEPAD_AXIS_RIGHTY), dz);
        ImGui::SameLine(0, 30);
        ImGui::BeginGroup();
        trigger_view(T("Left trigger", "Gatilho esquerdo"), ax(SDL_GAMEPAD_AXIS_LEFT_TRIGGER));
        trigger_view(T("Right trigger", "Gatilho direito"), ax(SDL_GAMEPAD_AXIS_RIGHT_TRIGGER));
        ImGui::EndGroup();
    }
    ImGui::PushItemWidth(420);
    ImGui::SliderFloat(T("Stick dead zone", "Zona morta do analógico"), &g_settings.stick_deadzone, 0.0f, 0.40f, "%.2f");
    ImGui::PopItemWidth();
    if (!bind_is_default(BIND_DEV_PAD) && ImGui::Button(T("Restore default buttons", "Restaurar botões padrão"))) bind_reset(BIND_DEV_PAD);
    ImGui::TextDisabled("%s", T("Buttons work by position, like a Switch controller: the right face button is A.",
                                "Os botões valem pela posição, como num controle do Switch: o botão da direita é o A."));
}

// ------------------------------------------------------------------ file dialogs (SDL calls back on any thread)
static pthread_mutex_t g_pick_lock = PTHREAD_MUTEX_INITIALIZER;
static char g_pick_game[1024], g_pick_mod[1024];
static void SDLCALL picked_game(void *, const char *const *files, int) {
    if (!files || !files[0]) return;
    pthread_mutex_lock(&g_pick_lock);
    snprintf(g_pick_game, sizeof g_pick_game, "%s", files[0]);
    pthread_mutex_unlock(&g_pick_lock);
}
static void SDLCALL picked_mod(void *, const char *const *files, int) {
    if (!files || !files[0]) return;
    pthread_mutex_lock(&g_pick_lock);
    snprintf(g_pick_mod, sizeof g_pick_mod, "%s", files[0]);
    pthread_mutex_unlock(&g_pick_lock);
}
// The Mods page's helpers: what a plugin is, a folder deleted, a folder opened, a plugin file added
static const char *plugin_title(const char *file) {
    return strstr(file, "steamrelay") ? T("Steam relay", "Relay Steam") : strstr(file, "vrmod") ? "LegendsVR"
         : strstr(file, "legendslua") ? "Legends Lua" : file;
}
static const char *plugin_about(const char *file) {
    return strstr(file, "steamrelay") ? T("Link Trades and Private Battles with your Steam friends, anywhere (Steam shows \"Playing Spacewar\" "
                                          "while the game runs).",
                                          "Trocas e batalhas privadas com seus amigos da Steam, de qualquer lugar (a Steam mostra \"Jogando "
                                          "Spacewar\" enquanto o jogo roda).")
         : strstr(file, "vrmod") ? T("Play in a VR headset.", "Jogue com um headset de RV.")
         : strstr(file, "legendslua") ? T("Lua scripting: runs the .lua scripts in the game folder's lua/ folder (for mod makers; "
                                          "scripts can change anything the game does - only use ones you trust).",
                                          "Scripts em Lua: roda os scripts .lua da pasta lua/ do jogo (para quem faz mods; scripts "
                                          "podem mudar tudo no jogo - use só os que você confia).")
                                 : T("A plugin (code that runs with the game).", "Um plugin (código que roda com o jogo).");
}
static SDL_EnumerationResult SDLCALL remove_entry(void *, const char *dir, const char *name) {
    char p[1024];
    snprintf(p, sizeof p, "%s%s", dir, name);
    SDL_PathInfo info;
    if (SDL_GetPathInfo(p, &info) && info.type == SDL_PATHTYPE_DIRECTORY) SDL_EnumerateDirectory(p, remove_entry, NULL);
    SDL_RemovePath(p);
    return SDL_ENUM_CONTINUE;
}
static void remove_tree(const char *dir) {
    SDL_EnumerateDirectory(dir, remove_entry, NULL);
    SDL_RemovePath(dir);
}
static void open_folder(const char *sub) {  // (in the data root, the working directory)
    SDL_CreateDirectory(sub);
    char *cwd = SDL_GetCurrentDirectory(), url[1200];
    if (!cwd) return;
    snprintf(url, sizeof url, "file://%s%s%s", cwd[0] == '/' ? "" : "/", cwd, sub);
    SDL_free(cwd);
    SDL_OpenURL(url);
}
static bool add_plugin(const char *src, char *msg, size_t cap) {  // src a plugin file: copied into plugins/
#ifdef _WIN32
    const char *ext = ".dll";
#else
    const char *ext = ".so";
#endif
    size_t n = strlen(src), k = strlen(ext);
    if (n <= k || SDL_strcasecmp(src + n - k, ext)) return false;
    const char *base = src + n;
    while (base > src && base[-1] != '/' && base[-1] != '\\') base--;
    char dst[300];
    snprintf(dst, sizeof dst, "plugins/%s", base);
    SDL_CreateDirectory("plugins");
    if (SDL_CopyFile(src, dst)) snprintf(msg, cap, "%s %s", T("Added", "Adicionado"), dst);
    else snprintf(msg, cap, "%s %s", T("Couldn't copy", "Não foi possível copiar"), base);
    return true;
}

static bool take(char *slot, char *out, size_t n) {
    pthread_mutex_lock(&g_pick_lock);
    bool got = slot[0] != 0;
    if (got) snprintf(out, n, "%s", slot), slot[0] = 0;
    pthread_mutex_unlock(&g_pick_lock);
    return got;
}

// ------------------------------------------------------------------ the game files section
static const ImVec4 k_green(0.45f, 0.85f, 0.50f, 1), k_red(1.0f, 0.45f, 0.40f, 1), k_yellow(1.0f, 0.82f, 0.35f, 1);

static void game_files_section(SDL_Window *win) {
    ImGui::SeparatorText(T("Game files", "Arquivos do jogo"));
    int st = setup_state();
    if (setup_running()) {
        static const char *const en[] = {"", "Looking for the game files...", "Reading the game files out of the packages...", "Copying the game files...",
                                         "Building the game data image...", "Reading the game code...",
                                         "Recompiling the game code...", "Finishing..."};
        static const char *const pt[] = {"", "Procurando os arquivos do jogo...", "Lendo os arquivos do jogo dos pacotes...", "Copiando os arquivos do jogo...",
                                         "Montando a imagem de dados do jogo...", "Lendo o código do jogo...",
                                         "Recompilando o código do jogo...", "Finalizando..."};
        ImGui::TextUnformatted(st >= 1 && st <= 7 ? T(en[st], pt[st]) : "...");
        ImGui::ProgressBar(setup_progress(), ImVec2(-FLT_MIN, 0));
        ImGui::TextDisabled("%s", T("This takes a while (10-30 minutes), only once. The game can be played when it's done.",
                                    "Isso demora (de 10 a 30 minutos), só uma vez. O jogo fica liberado quando terminar."));
        return;
    }
    bool ready = setup_ready();
    if (ready) {
        const char *name = game_name("extracted/exefs");
        ImGui::TextColored(k_green, g_pt ? "Pokémon %s - pronto para jogar" : "Pokémon %s - ready to play",
                           name ? name : "Legends: Arceus");
        ImGui::SameLine();
        if (ImGui::SmallButton(T("Change...", "Trocar..."))) SDL_ShowOpenFolderDialog(picked_game, NULL, win, NULL, false);
        if (st < SETUP_FAIL_NOT_FOUND) return;  // (a failed change: why, below - the game files set up before still play)
    }
    if (st >= SETUP_FAIL_NOT_FOUND) {
        static const char *const en[] = {"No game files found in that folder.", "Those are compressed game files (.nsz / .xcz).",
                                         "That's not a supported game (Pokémon Legends: Arceus).", "The files couldn't be copied.",
                                         "The game code didn't build.", "The game code builder is missing from this copy.",
                                         "No keys found for those game files (prod.keys).", "Your keys can't open those game files.",
                                         "Only the update was found, not the base game.", "Those game files couldn't be read."};
        static const char *const pt[] = {"Nenhum arquivo do jogo nessa pasta.", "Esses são arquivos do jogo comprimidos (.nsz / .xcz).",
                                         "Esse não é um jogo suportado (Pokémon Legends: Arceus).", "Os arquivos não puderam ser copiados.",
                                         "O código do jogo não compilou.", "O compilador do código do jogo não está nesta cópia.",
                                         "Nenhuma chave encontrada para esses arquivos (prod.keys).", "As suas chaves não abrem esses arquivos.",
                                         "Só a atualização foi encontrada, não o jogo base.", "Esses arquivos do jogo não puderam ser lidos."};
        static const char *const en_h[] = {"Choose the folder with your .nsp / .xci files, or the one that holds exefs and romfs.",
                                           "Decompress them first (nsz -D), or dump them as .nsp / .xci.",
                                           "Pick the folder of your own Pokémon Legends: Arceus.", "Check there are about 6 GB free and try again.",
                                           "Check there are about 2 GB free and try again; details in lasrecomp.log.", "Download the release again.",
                                           "Dump prod.keys from your own Switch and put it next to the game files (or in .switch in your home folder).",
                                           "Dump prod.keys again from your Switch on its current system version; details in lasrecomp.log.",
                                           "Put the base game's .nsp / .xci in the same folder as the update.",
                                           "The dump may be damaged; details in lasrecomp.log."};
        static const char *const pt_h[] = {"Escolha a pasta com os seus arquivos .nsp / .xci, ou a que tem exefs e romfs.",
                                           "Descomprima antes (nsz -D), ou faça o dump como .nsp / .xci.",
                                           "Escolha a pasta do seu Pokémon Legends: Arceus.", "Confira se há uns 6 GB livres e tente de novo.",
                                           "Confira se há uns 2 GB livres e tente de novo; detalhes no lasrecomp.log.", "Baixe a release de novo.",
                                           "Faça o dump do prod.keys do seu próprio Switch e coloque junto dos arquivos do jogo (ou em .switch na sua pasta pessoal).",
                                           "Faça o dump do prod.keys de novo no Switch com a versão atual do sistema; detalhes no lasrecomp.log.",
                                           "Coloque o .nsp / .xci do jogo base na mesma pasta da atualização.",
                                           "O dump pode estar danificado; detalhes no lasrecomp.log."};
        int k = st - SETUP_FAIL_NOT_FOUND;
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + 900);
        ImGui::TextColored(k_red, "%s", T(en[k], pt[k]));
        ImGui::TextDisabled("%s", T(en_h[k], pt_h[k]));
        ImGui::PopTextWrapPos();
    } else {
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + 900);
        ImGui::TextUnformatted(T("Select the folder with your own Pokémon Legends: Arceus: the dumped base game and update "
                                 "(.nsp / .xci, with your prod.keys), or the extracted exefs and romfs.",
                                 "Selecione a pasta com o seu Pokémon Legends: Arceus: o dump do jogo base e da atualização "
                                 "(.nsp / .xci, com o seu prod.keys), ou o exefs e o romfs extraídos."));
        ImGui::PopTextWrapPos();
    }
    if (!ready && ImGui::Button(T("Select game folder...", "Selecionar pasta do jogo..."), ImVec2(-FLT_MIN, 0)))
        SDL_ShowOpenFolderDialog(picked_game, NULL, win, NULL, false);
}

// ------------------------------------------------------------------ the launcher's look
#ifndef RT_CLASSIC_START_SCREEN
#define RT_CLASSIC_START_SCREEN 0  // (console ports: 1 - a TV, a controller, no separate launcher window)
#endif
enum { PAGE_HOME, PAGE_OPTIONS, PAGE_CONTROLS, PAGE_FILES, PAGE_MODS };
static const int k_launcher_w = 1280, k_launcher_h = 720;  // (the game's own window size: some Wayland compositors
                                                              // never resize a window for its program)
static ImFont *g_title_font, *g_button_font;  // (Baloo 2, the font of the game's button icons: runtime/nvn/glyphs.c)
static const ImU32 k_accent = IM_COL32(240, 196, 92, 255), k_accent_hi = IM_COL32(252, 216, 128, 255),
                   k_panel = IM_COL32(18, 16, 22, 255);

// The fonts, made at <density> times their size so the text stays sharp when the launcher is drawn bigger (ui_scale)
static char g_font_path[1100], g_baloo_path[1100];
static void load_fonts(float density) {
    ImGuiIO &io = ImGui::GetIO();
    static const ImWchar ranges[] = {0x20, 0xFF, 0};        // (Latin-1: Portuguese accents)
    static const ImWchar title_ranges[] = {0x20, 0x7E, 0};  // (the banner's title: ASCII; it's big)
    io.Fonts->Clear();
    ImFontConfig cfg;
    cfg.RasterizerDensity = density;
    if (!g_font_path[0] || !io.Fonts->AddFontFromFileTTF(g_font_path, 26, &cfg, ranges)) io.Fonts->AddFontDefault(&cfg);
    g_button_font = g_title_font = NULL;
    if (g_baloo_path[0]) {
        g_button_font = io.Fonts->AddFontFromFileTTF(g_baloo_path, 44, &cfg, ranges);
        g_title_font = io.Fonts->AddFontFromFileTTF(g_baloo_path, 120, &cfg, title_ranges);
    }
}

// The launcher is laid out for a 1280x720 window (the classic screen for 1080p). Another size - the desktop or the
// compositor stretched the window to the screen, F11, a TV - draws all of it bigger (or smaller) to fit: ImGui works
// in units of the layout's size (io.DisplaySize divided by the scale; a wider or taller window gets the extra room) and
// draws at the window's real pixels (io.DisplayFramebufferScale times it); the mouse is scaled the same way. The fonts
// are made again at the new size. ui_scale_begin before the frame's NewFrame calls, ui_scale_apply after the SDL one.
static float g_ui_scale = 1, g_font_density = 1;
static void ui_scale_begin(SDL_Window *win, float base_w, float base_h, bool grow_only) {
    int w = 0, h = 0;
    SDL_GetWindowSize(win, &w, &h);
    float s = w > 0 && h > 0 ? fminf((float)w / base_w, (float)h / base_h) : 1;
    if (grow_only && s < 1) s = 1;
    s = fminf(fmaxf(s, 0.5f), 4.0f);
    if (fabsf(s - 1) < 0.03f) s = 1;  // (a few pixels off: as made)
    g_ui_scale = s;
    float px = s * SDL_GetWindowPixelDensity(win), q = fminf(fmaxf(roundf(px * 4) / 4, 1.0f), 2.5f);  // (steps of 1/4)
    if (q != g_font_density) {
        g_font_density = q;
        load_fonts(q);
        vk_imgui_fonts_changed();  // (made again by the next ImGui_ImplVulkan_NewFrame)
        fprintf(stderr, "[launcher] window %dx%d: drawn at %.2fx, fonts at %.2fx\n", w, h, s, q);
    }
}
static void ui_scale_apply(ImGuiIO &io) {
    float s = g_ui_scale;
    if (s == 1) return;
    io.DisplaySize = ImVec2(io.DisplaySize.x / s, io.DisplaySize.y / s);
    io.DisplayFramebufferScale = ImVec2(io.DisplayFramebufferScale.x * s, io.DisplayFramebufferScale.y * s);
    for (ImGuiInputEvent &e : ImGui::GetCurrentContext()->InputEventsQueue)  // (all of this frame's: the queue doesn't trickle)
        if (e.Type == ImGuiInputEventType_MousePos && e.MousePos.PosX != -FLT_MAX)
            e.MousePos.PosX /= s, e.MousePos.PosY /= s;
}

// The controller's buttons for the page: the button's name in a pill, what it does (right-aligned, ending at x)
static void pad_hints(ImDrawList *d, float x, float y, bool back, bool tabs) {
    if (!g_npads) return;
    int fam = bind_label_family();
    bool swap = ImGui::GetIO().ConfigNavSwapGamepadButtons;
    char lr[48];
    snprintf(lr, sizeof lr, "%s %s", bind_pad_label(SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, fam), bind_pad_label(SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, fam));
    struct { const char *btn, *what; } h[4];
    int n = 0;
    if (g_pad_test) {
        snprintf(lr, sizeof lr, "%s %s", bind_pad_label(SDL_GAMEPAD_BUTTON_START, fam), bind_pad_label(SDL_GAMEPAD_BUTTON_BACK, fam));
        h[n++] = {lr, T("hold: stop the test", "segure: parar o teste")};
    } else {
        h[n++] = {bind_pad_label(swap ? SDL_GAMEPAD_BUTTON_EAST : SDL_GAMEPAD_BUTTON_SOUTH, fam), T("Select", "Selecionar")};
        if (back) h[n++] = {bind_pad_label(swap ? SDL_GAMEPAD_BUTTON_SOUTH : SDL_GAMEPAD_BUTTON_EAST, fam), T("Back", "Voltar")};
        if (tabs) h[n++] = {lr, T("Tabs", "Abas")};
    }
    ImFont *f = ImGui::GetFont();
    float fs = ImGui::GetFontSize() * 0.85f, pad = 7, gap = 22;
    for (int i = n - 1; i >= 0; i--) {
        ImVec2 tw = f->CalcTextSizeA(fs, FLT_MAX, 0, h[i].what), bw = f->CalcTextSizeA(fs, FLT_MAX, 0, h[i].btn);
        x -= tw.x;
        d->AddText(f, fs, ImVec2(x, y), IM_COL32(200, 206, 222, 255), h[i].what);
        x -= 8 + bw.x + 2 * pad;
        d->AddRectFilled(ImVec2(x, y - 2), ImVec2(x + bw.x + 2 * pad, y + fs + 2), k_accent, fs * 0.5f + 2);
        d->AddText(f, fs, ImVec2(x + pad, y), IM_COL32(16, 18, 24, 255), h[i].btn);
        x -= gap;
    }
}

static uint32_t rnd(uint32_t *s) { return *s = *s * 1664525u + 1013904223u, *s >> 8; }

// Dusk sky and a faint dot grid, shared by the banner and the page headers
static void banner_sky(ImDrawList *d, ImVec2 a, ImVec2 b) {
    d->AddRectFilledMultiColor(a, b, IM_COL32(22, 20, 52, 255), IM_COL32(30, 22, 58, 255), IM_COL32(120, 64, 54, 255),
                               IM_COL32(96, 58, 60, 255));
    float h = b.y - a.y;
    for (float y = a.y + 12; y < b.y; y += 24)
        for (float x = a.x + 12; x < b.x; x += 24)
            d->AddCircleFilled(ImVec2(x, y), 1.3f, IM_COL32(255, 226, 170, (int)(26 * (1 - (y - a.y) / h) + 4)), 6);
}

// The space-time rift over Mount Coronet's summit, as the game shows it: a flattened dark disc with a torn, ragged
// rim, grey streaks turning in it, shards breaking off around its edge and drifting, a white starburst in the middle.
// (cx, cy): its middle, rx: its half width.
static void rift(ImDrawList *d, float cx, float cy, float rx, float t) {
    const float ry = rx * 0.30f;
    uint32_t s = 21;
    for (int i = 18; i > 0; i--)  // (a faint pale light around it: the dusk sky is dark, as the rift is)
        d->AddEllipseFilled(ImVec2(cx, cy), ImVec2(rx * (0.95f + 0.03f * i), ry * (0.95f + 0.07f * i)), IM_COL32(170, 175, 215, 3));
    // the body: an ellipse whose rim is torn (its radius pulled in and out), filled as a fan of triangles from the middle
    ImVec2 rim[120];
    for (int i = 0; i < 120; i++) {
        float th = i * 6.2831853f / 120, r = 1 + 0.05f * sinf(7 * th + 0.4f) + 0.035f * sinf(17 * th + 1.3f) + 0.02f * sinf(29 * th + t * 0.4f);
        if (rnd(&s) % 7 == 0) r -= 0.06f + 0.06f * (float)(rnd(&s) % 100) / 100;  // (a bite out of the edge)
        rim[i] = ImVec2(cx + cosf(th) * rx * r, cy + sinf(th) * ry * r);
    }
    ImDrawListFlags fl = d->Flags;
    d->Flags &= ~ImDrawListFlags_AntiAliasedFill;  // (the fan's triangles meet without seams)
    for (int i = 0; i < 120; i++) d->AddTriangleFilled(ImVec2(cx, cy), rim[i], rim[(i + 1) % 120], IM_COL32(44, 46, 56, 240));
    d->Flags = fl;
    d->AddPolyline(rim, 120, IM_COL32(150, 155, 175, 110), ImDrawFlags_Closed, 1.2f);  // (the torn edge catching light)
    for (int i = 0; i < 6; i++)  // (darker toward the middle)
        d->AddEllipseFilled(ImVec2(cx, cy), ImVec2(rx * (0.85f - 0.12f * i), ry * (0.85f - 0.12f * i)), IM_COL32(20, 20, 28, 40));
    for (int k = 0; k < 28; k++) {  // grey wisps swirling in it: short, wobbling, at all depths
        float a0 = k * 2.39996f + t * (0.05f + 0.015f * (k % 4)), r0 = 0.22f + 0.70f * (float)((k * 53) % 100) / 100;
        int m = 6 + (k * 7) % 8;
        ImVec2 pts[14];
        for (int i = 0; i < m; i++) {
            float th = a0 + i * 0.06f, rr = r0 * (1 - 0.12f * i / 13.0f) + 0.010f * sinf(i * 0.6f + k);
            pts[i] = ImVec2(cx + cosf(th) * rx * rr, cy + sinf(th) * ry * rr);
        }
        d->AddPolyline(pts, m, IM_COL32(160, 164, 180, (int)(40 + 70 * (1 - r0))), 0, 1.1f + 0.6f * (k % 2));
    }
    for (int k = 0; k < 34; k++) {  // shards breaking off the rim, drifting round it (more at the sides)
        float th = (float)(rnd(&s) % 1000) / 1000 * 6.2831853f + t * (0.015f + 0.01f * (k % 3)), out = 1.02f + 0.16f * (float)(rnd(&s) % 100) / 100;
        out += 0.10f * fabsf(cosf(th));
        ImVec2 p(cx + cosf(th) * rx * out, cy + sinf(th) * ry * out);
        float sz = 2.5f + 6.0f * (float)(rnd(&s) % 100) / 100, an = (float)(rnd(&s) % 628) / 100 + t * 0.5f;
        ImVec2 q0(p.x + cosf(an) * sz, p.y + sinf(an) * sz * 0.6f), q1(p.x + cosf(an + 2.3f) * sz * 0.7f, p.y + sinf(an + 2.3f) * sz * 0.5f),
            q2(p.x + cosf(an + 4.1f) * sz * 0.8f, p.y + sinf(an + 4.1f) * sz * 0.5f);
        d->AddTriangleFilled(q0, q1, q2, IM_COL32(78, 82, 98, 235));
    }
    // the starburst: a soft glow, thin rays (longest across), a bright point, iridescent specks round it
    float g = 0.7f + 0.3f * sinf(t * 2.4f);
    for (int i = 10; i > 0; i--) d->AddEllipseFilled(ImVec2(cx, cy), ImVec2(rx * 0.022f * i, ry * 0.05f * i), IM_COL32(220, 230, 255, (int)(7 * g)));
    for (int i = 0; i < 12; i++) {
        float an = i * 0.5235988f + 0.15f * sinf(t * 0.5f + i), l = rx * (i % 3 == 0 ? 0.22f : 0.11f) * (0.85f + 0.3f * g) * (0.6f + 0.4f * fabsf(cosf(an)));
        d->AddLine(ImVec2(cx, cy), ImVec2(cx + cosf(an) * l, cy + sinf(an) * l * 0.55f), IM_COL32(255, 255, 255, (int)(210 * g)), i % 3 == 0 ? 1.6f : 1.0f);
    }
    d->AddCircleFilled(ImVec2(cx, cy), 3.0f + 1.5f * g, IM_COL32(255, 255, 255, 255), 12);
    static const ImU32 spk[3] = {IM_COL32(170, 220, 255, 200), IM_COL32(255, 190, 230, 200), IM_COL32(220, 255, 210, 200)};
    for (int k = 0; k < 9; k++) {
        float an = k * 0.7f + t * 0.2f, r = 0.10f + 0.12f * (float)((k * 41) % 100) / 100;
        float tw = 0.5f + 0.5f * sinf(t * 3.1f + k * 1.9f);
        d->AddCircleFilled(ImVec2(cx + cosf(an) * rx * r, cy + sinf(an) * ry * r), 1.3f, (spk[k % 3] & 0x00FFFFFF) | ((ImU32)(200 * tw) << 24), 6);
    }
}

// The launcher's banner: Mount Coronet at dusk under the rift, drawn as the Prism Tower is in Z-A's (a dark shape,
// glowing outlines): the cone with its broken summit, the snowcap's ragged edge and the gullies, foothills in front
static void banner(ImDrawList *d, ImVec2 a, ImVec2 b, float t) {
    d->PushClipRect(a, b, true);
    banner_sky(d, a, b);
    const ImU32 fill = IM_COL32(26, 20, 40, 255), c = IM_COL32(240, 196, 92, 235), dim = IM_COL32(200, 160, 80, 110),
                snow = IM_COL32(236, 228, 255, 210);
    float w = b.x - a.x, h = b.y - a.y, cx = a.x + w * 0.66f, peak = a.y + h * 0.32f, foot = b.y - h * 0.10f;
    rift(d, cx, a.y + h * 0.19f, w * 0.17f, t);  // (just above the summit, clear of the title)
    // the cone: near-straight flanks easing out low (half width = summit + spread * u^1.25), the summit broad, broken
    float sw = w * 0.05f, spread = w * 0.63f;
    // the summit: broad and a little crumbled - small uneven bumps on the rim (in sw / 2, up is negative)
    static const float jag[9] = {0.0f, -0.12f, 0.06f, -0.22f, -0.08f, -0.18f, 0.04f, -0.10f, 0.0f};
    auto rim_y = [&](int i) { return peak + jag[i] * sw * 0.5f; };
    auto hw = [&](float y) { float u = (y - peak) / (foot - peak); return sw + spread * powf(u < 0 ? 0 : u, 1.25f); };
    ImVec2 P[160];
    int n = 0;
    for (int i = 0; i <= 50; i++) {  // left flank, from the foot up
        float y = foot - (foot - peak) * i / 50.0f;
        P[n++] = ImVec2(cx - hw(y), y);
    }
    for (int i = 1; i < 8; i++) P[n++] = ImVec2(cx - sw + 2 * sw * i / 8.0f, rim_y(i));
    for (int i = 50; i >= 0; i--) {  // right flank, down to the foot
        float y = foot - (foot - peak) * i / 50.0f;
        P[n++] = ImVec2(cx + hw(y), y);
    }
    ImDrawListFlags fl = d->Flags;
    d->Flags &= ~ImDrawListFlags_AntiAliasedFill;
    d->AddConcavePolyFilled(P, n, fill);
    d->Flags = fl;
    // the snowcap: broad snowfields with tongues of snow reaching down the gullies (a few, of different lengths and
    // widths), lighter above the line; streaks on down from the longest tongues
    uint32_t s = 5;
    const int ns = 72;
    float tpos[9], td[9], tw[9];
    for (int k = 0; k < 9; k++)
        tpos[k] = (k + 0.5f) / 9 + 0.03f * ((float)(rnd(&s) % 100) / 50 - 1), td[k] = 0.04f + 0.16f * (float)(rnd(&s) % 100) / 100,
        tw[k] = 0.025f + 0.03f * (float)(rnd(&s) % 100) / 100;
    float yl = peak + (foot - peak) * 0.28f;
    ImVec2 S[ns + 2];
    for (int i = 0; i < ns; i++) {
        float f = i / (float)(ns - 1), dep = 0.012f * sinf(f * 23.0f);
        for (int k = 0; k < 9; k++) {
            float q = 1 - fabsf(f - tpos[k]) / tw[k];
            if (q > 0) dep = fmaxf(dep, td[k] * q * q);
        }
        if (i == 0 || i == ns - 1) dep = 0;
        S[i] = ImVec2(cx + (2 * f - 1) * hw(yl), yl + (foot - peak) * dep);  // (even steps across: never crosses itself)
    }
    {  // (the snow above the line: strips from the cone's outline down to the line)
        auto top = [&](float x) {  // (the outline's height above x: hw(y) = |x - cx| solved for y)
            float dx = fabsf(x - cx);
            if (dx > sw) return peak + (foot - peak) * powf((dx - sw) / spread, 1 / 1.25f);
            float f = (x - (cx - sw)) / (2 * sw) * 8;  // (on the summit: the rim, between its points)
            int i = f < 0 ? 0 : f >= 8 ? 7 : (int)f;
            return rim_y(i) + (rim_y(i + 1) - rim_y(i)) * (f - i);
        };
        d->Flags &= ~ImDrawListFlags_AntiAliasedFill;
        for (int i = 0; i + 1 < ns; i++)
            d->AddQuadFilled(ImVec2(S[i].x, top(S[i].x)), ImVec2(S[i + 1].x, top(S[i + 1].x)), S[i + 1], S[i], IM_COL32(150, 140, 205, 90));
        d->Flags = fl;
    }
    d->AddPolyline(S, ns, snow, 0, 1.5f);
    for (int k = 0; k < 9; k++) {  // streaks below the longer tongues
        if (td[k] < 0.11f) continue;
        float f = tpos[k], y = yl + (foot - peak) * td[k], x = cx + (2 * f - 1) * hw(yl), len = (foot - peak) * 0.08f;
        d->AddLine(ImVec2(x, y + 4), ImVec2(x + (2 * f - 1) * len * 0.35f, y + len), IM_COL32(236, 228, 255, 110), 1.2f);
    }
    for (int i = -3; i <= 3; i++) {  // the ridges: from the rim down the flanks
        if (!i) continue;
        float f = i / 3.5f;
        ImVec2 R[12];
        for (int j = 0; j < 12; j++) {
            float y = peak + (foot - peak) * (0.03f + 0.80f * j / 11.0f);
            R[j] = ImVec2(cx + f * hw(y) * (0.92f - 0.10f * fabsf(f)), y);
        }
        d->AddPolyline(R, 12, dim, 0, 1.2f);
    }
    d->AddPolyline(P, n, c, 0, 2.5f);  // the cone's outline, over the rest
    // the foothills in front: two ranges, the near one darkest, outlined
    static const ImU32 col[2] = {IM_COL32(34, 26, 46, 255), IM_COL32(20, 16, 30, 255)};
    d->Flags &= ~ImDrawListFlags_AntiAliasedFill;
    for (int layer = 0; layer < 2; layer++) {
        float base = b.y - h * (0.22f - 0.08f * layer), amp = h * (0.13f - 0.03f * layer);
        ImVec2 pts[64];
        int m = 0;
        for (int i = 0; i <= 60; i++) {
            float x = a.x + w * i / 60.0f;
            pts[m++] = ImVec2(x, base - amp * (0.35f + 0.65f * (float)(rnd(&s) % 100) / 100.0f * (0.6f + 0.4f * sinf(i * 0.7f + layer))));
        }
        for (int i = 0; i + 1 < m; i++)
            d->AddQuadFilled(pts[i], pts[i + 1], ImVec2(pts[i + 1].x, b.y), ImVec2(pts[i].x, b.y), col[layer]);
        d->Flags = fl;
        d->AddPolyline(pts, m, layer ? dim : IM_COL32(220, 176, 86, 160), 0, 1.5f);
        d->Flags &= ~ImDrawListFlags_AntiAliasedFill;
    }
    d->Flags = fl;
    d->AddRectFilledMultiColor(ImVec2(a.x, b.y - h * 0.2f), b, IM_COL32(12, 10, 18, 0), IM_COL32(12, 10, 18, 0), IM_COL32(12, 10, 18, 255),
                               IM_COL32(12, 10, 18, 255));  // (the valley fades into the status strip)
    ImFont *tf = g_title_font ? g_title_font : ImGui::GetFont(), *bf = g_button_font ? g_button_font : ImGui::GetFont();
    d->AddText(tf, 120, ImVec2(a.x + 40, a.y + 28), IM_COL32(255, 248, 236, 255), "PL:A");
    d->AddText(bf, 34, ImVec2(a.x + 46, a.y + 128), k_accent, "R  E  C  O  M  P");
    d->AddText(ImGui::GetFont(), ImGui::GetFontSize(), ImVec2(a.x + 46, a.y + 174), IM_COL32(220, 200, 190, 255),
               "Pokémon Legends: Arceus, recompiled for PC");
    d->PopClipRect();
}

// A launcher button: a wide flat bar with its label in Baloo 2; Play is the filled one
static bool big_button(const char *label, float w, float h, bool primary, bool enabled) {
    ImGui::BeginDisabled(!enabled);
    ImVec2 p = ImGui::GetCursorScreenPos();
    bool hit = ImGui::InvisibleButton(label, ImVec2(w, h), ImGuiButtonFlags_EnableNav);  // (keyboard / controller too:
                                                                                         // invisible buttons leave nav by default)
    bool hot = enabled && (ImGui::IsItemHovered() || ImGui::IsItemFocused()), held = hot && ImGui::IsItemActive();
    ImDrawList *d = ImGui::GetWindowDrawList();
    ImU32 bg = primary ? (enabled ? (hot ? k_accent_hi : k_accent) : IM_COL32(84, 72, 56, 255))
                       : (held ? IM_COL32(255, 255, 255, 56) : hot ? IM_COL32(255, 255, 255, 34) : IM_COL32(255, 255, 255, 12));
    d->AddRectFilled(p, ImVec2(p.x + w, p.y + h), bg, 6);
    if (hot && !primary) d->AddRectFilled(p, ImVec2(p.x + 6, p.y + h), k_accent, 3);
    ImFont *f = g_button_font ? g_button_font : ImGui::GetFont();
    float fs = primary ? 44.0f : 32.0f;
    const char *end = strstr(label, "##");
    ImVec2 ts = f->CalcTextSizeA(fs, FLT_MAX, 0, label, end);
    ImU32 tc = primary ? IM_COL32(34, 22, 8, enabled ? 255 : 160) : enabled ? IM_COL32(236, 240, 250, 255) : IM_COL32(120, 124, 136, 255);
    d->AddText(f, fs, ImVec2(p.x + (primary ? (w - ts.x) / 2 : 28), p.y + (h - ts.y) / 2 + 2), tc, label, end);
    ImGui::EndDisabled();
    return hit && enabled;
}

// ------------------------------------------------------------------ the start screen
extern "C" void launcher_lg_run(const char *root) {
    (void)root;
    pick_language();
    // the launcher's window is made at its own size (a window resized later isn't always given the size: Wayland)
    settings_load();
    const char *mode = getenv("LARECOMP_START_SCREEN");
    bool classic = mode && *mode ? !strcmp(mode, "classic") : RT_CLASSIC_START_SCREEN;
    bool skip_now = g_settings.skip_start && !getenv("LARECOMP_LAUNCHER_SHOT");
    bool sized = !classic && !skip_now;
    int lw = k_launcher_w, lh = k_launcher_h;
    if (getenv("LARECOMP_LAUNCHER_SIZE")) sscanf(getenv("LARECOMP_LAUNCHER_SIZE"), "%dx%d", &lw, &lh);
    if (sized) ui_window_size_hint(lw, lh);
    ui_window_init();
    SDL_Window *win = ui_window();
    ImGuiContext *prev_ctx = ImGui::GetCurrentContext();  // (the in-game menu's, when back here after a reboot)
    ImGuiContext *ctx = ImGui::CreateContext();
    ImGui::SetCurrentContext(ctx);
    ImGuiIO &io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    io.IniFilename = NULL;
    io.ConfigInputTrickleEventQueue = false;  // (every input event in its frame: ui_scale_apply scales the mouse's)
    static const char *const fonts[] = {"C:/Windows/Fonts/segoeui.ttf", "/usr/share/fonts/noto/NotoSans-Regular.ttf",
                                        "/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf", "/usr/share/fonts/TTF/DejaVuSans.ttf",
                                        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", "/usr/share/fonts/dejavu-sans-fonts/DejaVuSans.ttf",
                                        "/usr/share/fonts/google-noto-vf/NotoSans[wght].ttf",  // (Fedora: Bazzite, the PS5's)
                                        "/usr/share/fonts/liberation-sans-fonts/LiberationSans-Regular.ttf"};
    for (const char *p : fonts) {
        FILE *f = fopen(p, "rb");  // (a missing file is an assertion in ImGui, not a failure it returns)
        if (!f) continue;
        fclose(f);
        snprintf(g_font_path, sizeof g_font_path, "%s", p);
        break;
    }
    if (!rt_resource("Baloo2-SemiBold.ttf", "third_party/fonts/Baloo2/Baloo2-SemiBold.ttf", g_baloo_path, sizeof g_baloo_path))
        g_baloo_path[0] = 0;
    g_font_density = 1, g_ui_scale = 1;
    load_fonts(1);
    ImGui::StyleColorsDark();
    ImGui::GetStyle().ScaleAllSizes(1.5f);
    if (!classic) {  // (the launcher's colors: the banner's dusk, the accent for what is on)
        ImGuiStyle &st = ImGui::GetStyle();
        ImVec4 *c = st.Colors;
        c[ImGuiCol_WindowBg] = ImVec4(0.07f, 0.06f, 0.09f, 1);
        c[ImGuiCol_ChildBg] = ImVec4(0, 0, 0, 0);
        c[ImGuiCol_PopupBg] = ImVec4(0.10f, 0.08f, 0.12f, 0.98f);
        c[ImGuiCol_FrameBg] = ImVec4(0.18f, 0.14f, 0.20f, 1);
        c[ImGuiCol_FrameBgHovered] = ImVec4(0.26f, 0.20f, 0.27f, 1);
        c[ImGuiCol_FrameBgActive] = ImVec4(0.32f, 0.24f, 0.32f, 1);
        c[ImGuiCol_Button] = ImVec4(0.22f, 0.17f, 0.24f, 1);
        c[ImGuiCol_ButtonHovered] = ImVec4(0.34f, 0.26f, 0.34f, 1);
        c[ImGuiCol_ButtonActive] = ImVec4(0.42f, 0.32f, 0.40f, 1);
        c[ImGuiCol_Header] = ImVec4(0.22f, 0.17f, 0.24f, 1);
        c[ImGuiCol_HeaderHovered] = ImVec4(0.34f, 0.26f, 0.34f, 1);
        c[ImGuiCol_HeaderActive] = ImVec4(0.42f, 0.32f, 0.40f, 1);
        c[ImGuiCol_CheckMark] = ImVec4(0.94f, 0.77f, 0.36f, 1);
        c[ImGuiCol_SliderGrab] = ImVec4(0.94f, 0.77f, 0.36f, 1);
        c[ImGuiCol_Tab] = ImVec4(0.18f, 0.14f, 0.20f, 1);
        c[ImGuiCol_TabHovered] = ImVec4(0.34f, 0.26f, 0.34f, 1);
        c[ImGuiCol_TabSelected] = ImVec4(0.34f, 0.26f, 0.34f, 1);
        c[ImGuiCol_TabSelectedOverline] = ImVec4(0.94f, 0.77f, 0.36f, 1);
        c[ImGuiCol_PlotHistogram] = ImVec4(0.94f, 0.77f, 0.36f, 1);
        c[ImGuiCol_Separator] = ImVec4(0.36f, 0.28f, 0.34f, 1);
        c[ImGuiCol_TitleBg] = c[ImGuiCol_TitleBgActive] = c[ImGuiCol_TitleBgCollapsed] = ImVec4(0.18f, 0.14f, 0.20f, 1);  // (the shader warm-up window)
        st.WindowRounding = 8;
        st.FrameRounding = st.GrabRounding = st.TabRounding = 6;
    }
    ImGui_ImplSDL3_InitForVulkan(win);
    ImGui_ImplSDL3_SetGamepadMode(ImGui_ImplSDL3_GamepadMode_AutoAll);  // (any controller moves the focus, not only the first one listed)
    vk_imgui_backend_init();

    static const char *const res_en[] = {"Automatic (window size)", "540p", "720p", "1080p", "1440p", "4K"};
    static const char *const res_pt[] = {"Automática (tamanho da janela)", "540p", "720p", "1080p", "1440p", "4K"};
    static const int res_lines[] = {0, 540, 720, 1080, 1440, 2160};
    int res = 0;
    for (int i = 0; i < 6; i++) if (res_lines[i] == g_settings.res_height) res = i;
    bool full = g_settings.fullscreen, smaa = g_settings.antialias == AA_SMAA, wildb = g_settings.wild_battle, showfps = g_settings.show_fps, realtime = !g_settings.fixed_steps;
    // (72: half a 144 Hz screen's refresh - every frame shown for exactly 2 refreshes, where 60 alternates 2 and 3)
    static const int fps_vals[] = {30, 60, 72, 120, 144, FPS_UNLIMITED};
    auto fps_index = [](int f) { for (int i = 0; i < 6; i++) if (fps_vals[i] == f) return i; return 0; };
    int fps_i = fps_index(g_settings.fps);
    bool experimental = g_settings.experimental;
    // Recommended settings for this PC (from Z-A's start screen; by the GPU, the CPU threads and the RAM): shown with an
    // Apply button, and applied at once on a first start (no larecomp.ini yet)
    struct Rec { int res, fps, nowait; char text[120]; } rec;
    char pc[200];
    {
        char gpu[128];
        int vram_gb;
        bool igpu;
        vk_describe_gpu(gpu, sizeof gpu, &vram_gb, &igpu);
        int threads = SDL_GetNumLogicalCPUCores(), ram_gb = (SDL_GetSystemRAM() + 512) / 1024;  // (not plat_cpu_count: cpupin narrows it)
        // resolution index into res_lines: 0 Auto, 1 540p, 2 720p, 3 1080p, 4 1440p, 5 4K; fps index: 0 30, 1 60
        if (igpu || vram_gb < 4) rec = {2, 0, threads <= 8, ""};
        else if (vram_gb < 12) rec = {3, 1, threads <= 8, ""};
        else rec = {4, 1, threads <= 8, ""};
        static const char *const rn[] = {"Auto", "540p", "720p", "1080p", "1440p", "4K"};
        snprintf(rec.text, sizeof rec.text, "%s, %s fps%s", rn[rec.res], rec.fps ? "60" : "30", rec.nowait ? ", shader loading: don't wait" : "");
        snprintf(pc, sizeof pc, "%s (%s%d GB), %d CPU threads, %d GB RAM", gpu, igpu ? "integrated, " : "", vram_gb, threads, ram_gb);
        fprintf(stderr, "[launcher] this PC: %s; recommended: %s%s\n", pc, rec.text, g_settings_fresh ? " (first start: applied)" : "");
    }
    int shl_i = g_settings.shader_nowait ? 1 : 0;
    bool smooth_sh = g_settings.shadow_smooth;
    int detail_i = g_settings.detail >= 0 && g_settings.detail <= 3 ? g_settings.detail : 0;
    int shadows_i = g_settings.shadows >= 0 && g_settings.shadows <= 2 ? g_settings.shadows : 0;
    int sharp_i = g_settings.sharpen >= 0 && g_settings.sharpen <= 3 ? g_settings.sharpen : 0;
    auto apply_rec = [&]() { res = rec.res, fps_i = rec.fps, shl_i = rec.nowait; };
    if (g_settings_fresh) apply_rec();
    int bg_i = g_settings.background >= 0 && g_settings.background <= 2 ? g_settings.background : 0;
    int pk_i = g_settings.pkhex_link >= 0 && g_settings.pkhex_link <= 2 ? g_settings.pkhex_link : 0;
    int prompts_i = g_settings.button_prompts >= 0 && g_settings.button_prompts <= 4 ? g_settings.button_prompts : 0;
    // The game's own settings (its Settings menu's: text speed, camera, volumes...), one bit field (hle/pla_options.c
    // has the bits): shown as the save last had them, put in the game at the next load when changed
    const int k_gopt_default = 0x2a31ddc2;  // (a new game's, without the text language: the game's own)
    int gopt = g_settings.game_opts_next >= 0 ? g_settings.game_opts_next : g_settings.game_opts >= 0 ? g_settings.game_opts : k_gopt_default;
    const int gopt_was = gopt;
    auto restore_defaults = [&]() {  // (the Options' settings as a first start has them; Controls has its own Restore)
        Settings d;
        settings_defaults(&d);
        res = 0;
        for (int i = 0; i < 6; i++) if (res_lines[i] == d.res_height) res = i;
        full = d.fullscreen, smaa = d.antialias == AA_SMAA, showfps = d.show_fps, realtime = !d.fixed_steps, experimental = d.experimental;
        fps_i = fps_index(d.fps);
        wildb = d.wild_battle, prompts_i = d.button_prompts, smooth_sh = d.shadow_smooth, bg_i = d.background, pk_i = d.pkhex_link;
        detail_i = d.detail, shadows_i = d.shadows, sharp_i = d.sharpen;
        shl_i = d.shader_nowait;
        gopt = k_gopt_default;
    };
    // Translation: the folders under mods/ (runtime/mods.h); 0 = none (the game's own English)
    static char mods[MODS_MAX][128];
    int nmods = mods_list(mods, MODS_MAX);
    // The mods that are on, in load order (g_settings.mod: names separated by commas; the first wins where two change
    // the same file - a texture pack and a translation both on, say), those still in mods/
    std::vector<std::string> on_mods;
    {
        char act[MODS_MAX][128];
        int na = mods_active(act, MODS_MAX);
        for (int k = 0; k < na; k++)
            for (int i = 0; i < nmods; i++) if (!strcmp(mods[i], act[k])) { on_mods.push_back(act[k]); break; }
    }
    auto mod_on = [&](const char *nm) { for (auto &s : on_mods) if (s == nm) return true; return false; };
    auto mod_set = [&](const char *nm, bool on) {
        for (size_t k = 0; k < on_mods.size(); k++) if (on_mods[k] == nm) { on_mods.erase(on_mods.begin() + (long)k); break; }
        if (on) on_mods.push_back(nm);
    };
    auto mod_prune = [&]() {  // (the list after mods/ changed: those gone dropped)
        for (size_t k = 0; k < on_mods.size();) {
            bool there = false;
            for (int i = 0; i < nmods; i++) there |= on_mods[k] == mods[i];
            if (there) k++; else on_mods.erase(on_mods.begin() + (long)k);
        }
    };
    // Plugins (plugins/: code mods - the VR mod, the Steam relay): each on or off. They load before this screen (they
    // may extend the Vulkan device it draws with), so a change restarts the game once when Play is pressed.
    static char plugs[16][64];
    static bool plug_on[16];
    int nplugs = 0;
    auto plugins_refresh = [&]() {  // (the list again - a plugin added or removed: the choices made kept)
        static char was[16][64];
        static bool was_on[16];
        int nwas = nplugs;
        memcpy(was, plugs, sizeof plugs), memcpy(was_on, plug_on, sizeof plug_on);
        nplugs = plugins_files(plugs, 16);
        for (int i = 0; i < nplugs; i++) {
            plug_on[i] = plugin_enabled(plugs[i]);
            for (int j = 0; j < nwas; j++) if (!strcmp(was[j], plugs[i])) plug_on[i] = was_on[j];
        }
    };
    auto plugins_on_now = [&](char *out, size_t cap) {  // (the ones on, as one string)
        out[0] = 0;
        for (int i = 0; i < nplugs; i++)
            if (plug_on[i] && strlen(out) + strlen(plugs[i]) + 2 < cap) strcat(out, ","), strcat(out, plugs[i]);
    };
    plugins_refresh();
    char plugins_at_start[1100];  // (the ones loaded)
    plugins_on_now(plugins_at_start, sizeof plugins_at_start);
    if (const char *t = getenv("LARECOMP_TEST_PLUGINS_TOGGLE"))  // (tests: these switched over, as if clicked)
        for (int i = 0; i < nplugs; i++) if (plugin_listed(t, plugs[i])) plug_on[i] = !plug_on[i];
    setenv("LARECOMP_TEST_PLUGINS_TOGGLE", "", 1);  // (not again after the restart)
    int mod_sel = -1;  // (the Mods page's selected row: translations first, then plugins)
    char mod_msg[256] = "";
    long shot_at = getenv("LARECOMP_LAUNCHER_SHOT") ? atol(getenv("LARECOMP_LAUNCHER_SHOT")) : 0, frame = 0;
    bool autoplay = getenv("LARECOMP_LAUNCHER_AUTOPLAY") != NULL, play = false;
    // "Skip this screen": straight to the game once it's ready - unless Shift is held in the first moments
    bool skipstart = g_settings.skip_start, shift_seen = false;
    uint64_t opened = SDL_GetTicks();
    int page = PAGE_HOME;
    if (const char *pg = getenv("LARECOMP_LAUNCHER_PAGE"))
        page = !strcmp(pg, "options") ? PAGE_OPTIONS : !strcmp(pg, "controls") ? PAGE_CONTROLS : !strcmp(pg, "files") ? PAGE_FILES
             : !strcmp(pg, "mods") ? PAGE_MODS : PAGE_HOME;
    if (getenv("LARECOMP_LAUNCHER_PAD") || getenv("LARECOMP_LAUNCHER_KEYS")) page = PAGE_CONTROLS;
    // The launcher is a small fixed window; the game gets its own size (or fullscreen) after Play (below). (Made at
    // that size; resized here when the window was there already - back from the game)
    if (sized) {
        SDL_SetWindowFullscreen(win, false);
        SDL_SetWindowSize(win, lw, lh);
        SDL_SetWindowResizable(win, false);  // (after the size: on Wayland it pins min = max = the current size)
        SDL_SetWindowPosition(win, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
    }
    const char *keys_test = getenv("LARECOMP_LAUNCHER_KEYS");
    if (keys_test && atoi(keys_test) > 0) bind_capture_begin(BIND_DEV_KEY, atoi(keys_test));
    if (getenv("LARECOMP_SETUP_DIR")) setup_start(getenv("LARECOMP_SETUP_DIR"));
    if (getenv("LARECOMP_MOD_INSTALL")) mod_install_start(getenv("LARECOMP_MOD_INSTALL"));
    static const float bg[4] = {0.08f, 0.08f, 0.10f, 1};
#ifdef _WIN32
    static const SDL_DialogFileFilter mod_filters[] = {{"Mod (.zip, .rar, .7z) or plugin (.dll)", "zip;rar;7z;dll"}};
#else
    static const SDL_DialogFileFilter mod_filters[] = {{"Mod (.zip, .rar, .7z) or plugin (.so)", "zip;rar;7z;so"}};
#endif
    int inst = 0;
    bool ready = false;

    // The settings, shown by both screens: the launcher's Options tabs, or the classic screen's one list
    auto settings_display = [&]() {
        ImGui::Combo(T("Resolution", "Resolução"), &res, g_pt ? res_pt : res_en, 6);
        if (ImGui::Checkbox(T("Fullscreen", "Tela cheia"), &full) && classic) SDL_SetWindowFullscreen(win, full);  // (launcher: at Play)
        ImGui::Checkbox(T("Anti-aliasing (SMAA)", "Suavizar serrilhado (SMAA)"), &smaa);
        static const char *const fps_names_en[] = {"30 fps", "60 fps", "72 fps", "120 fps", "144 fps", "Unlimited"};  // (above 60: Experimental settings)
        static const char *const fps_names_pt[] = {"30 fps", "60 fps", "72 fps", "120 fps", "144 fps", "Ilimitada"};
        if (!experimental && fps_i > 1) fps_i = 1;
        ImGui::Combo(T("Frame rate", "Taxa de quadros"), &fps_i, g_pt ? fps_names_pt : fps_names_en, experimental ? 6 : 2);
        if (fps_i >= 1 && fps_i <= 4)  // (Unlimited: always real time, hle/fps60.c)
            ImGui::Checkbox(T("Keep full speed when the frame rate dips", "Manter a velocidade total quando a taxa de quadros cai"), &realtime);
        if (fps_i >= 2)  // (72 / 120 / 144 / Unlimited: the game's own logic was made for 30; some of it counts frames)
            ImGui::TextColored(k_yellow, "%s",
                               T("Above 60 fps is experimental. Some scripted scenes can stall at these rates\n"
                                              "(a cutscene that walks you somewhere may never let go of you). If that happens,\n"
                                              "set 60 or 30 fps in the game's Settings and step into the scene again.",
                                              "Acima de 60 fps é experimental. Algumas cenas com script podem travar nessas taxas\n"
                                              "(uma cena que leva você a algum lugar pode nunca soltar o controle). Se acontecer,\n"
                                              "escolha 60 ou 30 fps nas configurações do jogo e entre na cena de novo."));
        ImGui::Checkbox(T("Show FPS counter", "Mostrar contador de FPS"), &showfps);
        ImGui::Spacing();
        ImGui::Checkbox(T("Experimental settings", "Configurações experimentais"), &experimental);
        ImGui::Indent(ImGui::GetFrameHeight() + ImGui::GetStyle().ItemInnerSpacing.x);
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        ImGui::TextWrapped("%s", T("Settings that aren't finished yet: frame rates above 60 (72, 120, 144, Unlimited). Shown here and in "
                                   "the game's Settings only while this is on.",
                                   "Configurações ainda não terminadas: taxas de quadros acima de 60 (72, 120, 144, Ilimitada). Aparecem "
                                   "aqui e nas configurações do jogo só com isto ligado."));
        ImGui::PopStyleColor();
        ImGui::Unindent(ImGui::GetFrameHeight() + ImGui::GetStyle().ItemInnerSpacing.x);
    };
    auto settings_graphics = [&]() {  // (the game's Settings rows of the same names: runtime/hle/pla_options.c)
        const char *const sharp_names[] = {T("Off", "Desligado"), T("Low", "Baixo"), T("Medium", "Médio"), T("High", "Alto")};
        ImGui::Combo(T("Sharpening", "Nitidez"), &sharp_i, sharp_names, 4);
        const char *const shadow_names[] = {"Switch (1024)", T("High (2048)", "Alta (2048)"), T("Ultra (4096)", "Ultra (4096)")};
        ImGui::Combo(T("Shadows", "Sombras"), &shadows_i, shadow_names, 3);
        ImGui::Checkbox(T("Smooth shadow edges", "Suavizar as bordas das sombras"), &smooth_sh);
        const char *const detail_names[] = {T("Switch", "Switch"), T("High", "Alta"), T("Very high", "Muito alta"), T("Max", "Máxima")};
        ImGui::Combo(T("Draw distance", "Distância de desenho"), &detail_i, detail_names, 4);
        const char *const shl_names[] = {T("Wait (may stutter the first time)", "Esperar (pode travar na primeira vez)"),
                                         T("Don't wait (may show late the first time)", "Não esperar (pode aparecer atrasado na primeira vez)")};
        ImGui::Combo(T("Shader loading", "Carregamento de shaders"), &shl_i, shl_names, 2);
    };
    auto game_own_settings = [&]() {
        auto field = [&](int at, int bits) { return (gopt >> at) & ((1 << bits) - 1); };
        auto set_field = [&](int at, int bits, int v) { gopt = (gopt & ~(((1 << bits) - 1) << at)) | (v << at); };
        auto on_off = [&](const char *label, int bit, bool on_is_set) {  // (a checkbox for one bit)
            bool b = field(bit, 1) == (on_is_set ? 1 : 0);
            if (ImGui::Checkbox(label, &b)) set_field(bit, 1, b == on_is_set);
        };
        ImGui::Spacing();
        ImGui::SeparatorText(T("The game's own settings", "Configurações do próprio jogo"));
        const char *const speed_names[] = {T("Slow", "Lenta"), T("Normal", "Normal"), T("Fast", "Rápida")};
        int v = field(0, 2) <= 2 ? field(0, 2) : 2;
        if (ImGui::Combo(T("Text speed", "Velocidade do texto"), &v, speed_names, 3)) set_field(0, 2, v);
        on_off(T("Autosave", "Salvamento automático"), 19, false);
        on_off(T("HUD toggling", "Alternar a interface (HUD)"), 27, false);
        on_off(T("ZR button confirmation", "Confirmação com o botão ZR"), 28, false);
        on_off(T("Invert camera vertically", "Inverter a câmera na vertical"), 20, false);
        on_off(T("Invert camera horizontally", "Inverter a câmera na horizontal"), 21, false);
        v = (field(24, 3) <= 4 ? field(24, 3) : 2) + 1;
        if (ImGui::SliderInt(T("Camera sensitivity", "Sensibilidade da câmera"), &v, 1, 5)) set_field(24, 3, v - 1);
        v = field(22, 2);
        if (ImGui::SliderInt(T("Motion sensitivity", "Sensibilidade de movimento"), &v, 0, 3)) set_field(22, 2, v);
        const struct { const char *en, *pt; int at; } vols[] = {{"Music", "Música", 6}, {"Sound effects", "Efeitos sonoros", 10},
                                                               {"Pokémon cries", "Gritos dos Pokémon", 14}};
        for (const auto &o : vols) {
            v = field(o.at, 4) <= 10 ? field(o.at, 4) : 10;
            if (ImGui::SliderInt(T(o.en, o.pt), &v, 0, 10)) set_field(o.at, 4, v);
        }
        const char *const range_names[] = {T("Wide", "Ampla"), T("Narrow", "Estreita")};
        v = field(29, 1);
        if (ImGui::Combo(T("Dynamic range", "Faixa dinâmica"), &v, range_names, 2)) set_field(29, 1, v);
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + 900);
        ImGui::TextWrapped("%s", g_settings.game_opts < 0
                                     ? T("These live in your save. Shown as a new game has them until you've played once; a change here goes "
                                         "into the game when it loads the save.",
                                         "Estas ficam no seu save. Aparecem como num jogo novo até você jogar uma vez; uma mudança aqui vai "
                                         "para o jogo quando ele carregar o save.")
                                     : T("These live in your save (the game's Settings menu changes them too). A change here goes into the "
                                         "game when it loads the save.",
                                         "Estas ficam no seu save (o menu de configurações do jogo também muda). Uma mudança aqui vai para o "
                                         "jogo quando ele carregar o save."));
        ImGui::PopTextWrapPos();
        ImGui::PopStyleColor();
    };
    auto settings_game = [&]() {
        const char *const bg_names[] = {T("Keep playing", "Continuar jogando"), T("Mute", "Silenciar"), T("Pause", "Pausar")};
        ImGui::Combo(T("In background", "Em segundo plano"), &bg_i, bg_names, 3);
        const char *const pk_names[] = {T("Off", "Desligado"), T("This PC", "Este PC"), T("Network", "Rede")};
        ImGui::Combo(T("PKHeX Link (LiveHeX, port 6000)", "PKHeX Link (LiveHeX, porta 6000)"), &pk_i, pk_names, 3);
        // (runtime/nvn/glyphs.c: the game's button icons redrawn for the controller in use)
        const char *const prompt_names[] = {T("Auto (what you play with)", "Automático (o que você usa)"), "Nintendo Switch", "Xbox",
                                            "PlayStation", T("Keyboard and mouse", "Teclado e mouse")};
        ImGui::Combo(T("Button prompts", "Ícones dos botões"), &prompts_i, prompt_names, 5);
        game_own_settings();
        // Delete save data: the save/ folder emptied (rt_remove_saves), after a warning; Cancel is the default choice
        static char save_msg[160];
        ImGui::Spacing();
        ImGui::SeparatorText(T("Save data", "Dados salvos"));
        if (ImGui::Button(T("Delete save data...", "Apagar dados salvos..."))) ImGui::OpenPopup("##delsave");
        ImGui::SameLine();
        if (ImGui::Button(T("Open the save folder", "Abrir a pasta de saves"))) open_folder("save");
        if (save_msg[0]) ImGui::TextDisabled("%s", save_msg);
        if (ImGui::BeginPopupModal("##delsave", NULL, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar)) {
            ImGui::Text("%s", T("Delete all save data?", "Apagar todos os dados salvos?"));
            ImGui::Spacing();
            ImGui::TextColored(k_red, "%s", T("This can't be undone: your progress in this game is gone for good.",
                                              "Isto não pode ser desfeito: seu progresso neste jogo some para sempre."));
            ImGui::TextDisabled("%s", T("To keep a copy, cancel and copy the save folder somewhere else first.",
                                        "Para guardar uma cópia, cancele e copie a pasta de saves para outro lugar antes."));
            ImGui::Spacing();
            if (ImGui::Button(T("Delete", "Apagar"), ImVec2(140, 0))) {
                snprintf(save_msg, sizeof save_msg, "%s", rt_remove_saves() ? T("Save data deleted.", "Dados salvos apagados.")
                                                                             : T("Couldn't delete all of it (see lasrecomp.log).",
                                                                                 "Não foi possível apagar tudo (veja o lasrecomp.log)."));
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button(T("Cancel", "Cancelar"), ImVec2(140, 0)) || cancel_pressed()) ImGui::CloseCurrentPopup();
            ImGui::SetItemDefaultFocus();  // (a stray Enter / A press cancels)
            ImGui::EndPopup();
        }
    };
    auto settings_mods = [&]() {
        std::string shown = on_mods.empty() ? std::string(T("None", "Nenhum"))
                          : on_mods[0] + (on_mods.size() > 1 ? " +" + std::to_string(on_mods.size() - 1) : std::string());
        if (ImGui::BeginCombo(T("Mods", "Mods"), shown.c_str())) {  // (each on or off; the order: the Mods page)
            for (int i = 0; i < nmods; i++) {
                bool on = mod_on(mods[i]);
                if (ImGui::Checkbox(mods[i], &on)) mod_set(mods[i], on);
            }
            ImGui::EndCombo();
        }
    };
    // The launcher's Mods page: Add / Remove / the folder at the top, then every mod - translations (mods/, one at a
    // time) and plugins (plugins/) - with a tick box each
    auto mods_page = [&]() {
        bool busy = inst == MOD_INSTALL_RUNNING;
        ImGui::BeginDisabled(busy);
        focus_first();
        if (ImGui::Button(T("Add a mod...", "Adicionar mod..."))) SDL_ShowOpenFileDialog(picked_mod, NULL, win, mod_filters, 1, NULL, false);
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginDisabled(mod_sel < 0 || busy);
        if (ImGui::Button(T("Remove", "Remover"))) ImGui::OpenPopup("##remove");
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button(T("Open the mods folder", "Abrir a pasta de mods"))) open_folder("mods");
        if (busy) ImGui::TextDisabled("%s", T("Installing the mod...", "Instalando o mod..."));
        else if (inst == MOD_INSTALL_FAIL_UNPACK || inst == MOD_INSTALL_FAIL_NOT_MOD)
            ImGui::TextColored(k_red, "%s", inst == MOD_INSTALL_FAIL_UNPACK ? T("Couldn't unpack that file.", "Não foi possível abrir esse arquivo.")
                                                                            : T("No mod for this game in that file.", "Esse arquivo não tem mod para este jogo."));
        else if (mod_msg[0]) ImGui::TextDisabled("%s", mod_msg);
        ImGui::Spacing();
        if (!nmods && !nplugs)
            ImGui::TextDisabled("%s", T("No mods yet. Add a mod... takes a translation (.zip / .rar / .7z) or a plugin.",
                                        "Nenhum mod ainda. Adicionar mod... aceita uma tradução (.zip / .rar / .7z) ou um plugin."));
        else if (ImGui::BeginTable("##mods", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_PadOuterX)) {
            ImGui::TableSetupColumn("on", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFrameHeight());
            ImGui::TableSetupColumn("mod", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("kind", ImGuiTableColumnFlags_WidthFixed, 130);
            std::vector<int> ord;  // (the mods that are on first, in their load order, then the rest)
            for (auto &s : on_mods)
                for (int i = 0; i < nmods; i++) if (s == mods[i]) ord.push_back(i);
            for (int i = 0; i < nmods; i++) if (!mod_on(mods[i])) ord.push_back(i);
            int move_from = -1, move_to = -1;
            for (int r = 0; r < nmods + nplugs; r++) {
                bool tr = r < nmods;
                int i = tr ? ord[r] : r - nmods;
                ImGui::PushID(r);
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                bool on = tr ? mod_on(mods[i]) : plug_on[i];
                if (ImGui::Checkbox("##on", &on)) {
                    if (tr) mod_set(mods[i], on);
                    else plug_on[i] = on;
                }
                ImGui::TableSetColumnIndex(1);
                int sel_id = tr ? i : r;  // (a mod's own index in mods[], a plugin's nmods + its index: rows are reordered)
                if (ImGui::Selectable(tr ? mods[i] : plugin_title(plugs[i]), mod_sel == sel_id,
                                      ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap))
                    mod_sel = sel_id;
                char about[300];
                if (tr) {
                    const char *lang = mod_language(mods[i]);
                    char tp[200];
                    struct stat tsb;
                    snprintf(tp, sizeof tp, "mods/%s/texpack", mods[i]);
                    bool pack = !stat(tp, &tsb) && S_ISDIR(tsb.st_mode);
                    snprintf(about, sizeof about, "%s%s%s", pack ? T("Texture pack", "Pacote de texturas") : T("Replaces game files", "Substitui arquivos do jogo"),
                             lang ? T(" - texts: ", " - textos: ") : "", lang ? lang : "");
                } else snprintf(about, sizeof about, "%s", plugin_about(plugs[i]));
                ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
                ImGui::TextWrapped("%s", about);
                ImGui::PopStyleColor();
                ImGui::TableSetColumnIndex(2);
                if (tr && on && on_mods.size() > 1) {  // (its place in the load order: up / down)
                    int k = r;  // (the mods that are on come first, in order)
                    ImGui::BeginDisabled(k == 0);
                    if (ImGui::ArrowButton("##up", ImGuiDir_Up)) move_from = k, move_to = k - 1;
                    ImGui::EndDisabled();
                    ImGui::SameLine();
                    ImGui::BeginDisabled(k + 1 >= (int)on_mods.size());
                    if (ImGui::ArrowButton("##down", ImGuiDir_Down)) move_from = k, move_to = k + 1;
                    ImGui::EndDisabled();
                    ImGui::SameLine();
                    ImGui::TextDisabled("%d", k + 1);
                } else ImGui::TextDisabled("%s", tr ? T("Mod", "Mod") : T("Plugin", "Plugin"));
                ImGui::PopID();
            }
            if (move_from >= 0) std::swap(on_mods[(size_t)move_from], on_mods[(size_t)move_to]);
            ImGui::EndTable();
        }
        ImGui::Spacing();
        ImGui::TextDisabled("%s", T("Mods that are on load in their order (the arrows): where two change the same file or texture, the higher one wins. "
                                    "Plugins load as the game starts: a change restarts it once when you press Play.",
                                    "Mods ligados carregam na ordem (as setas): onde dois mudam o mesmo arquivo ou textura, o de cima vence. "
                                    "Plugins carregam ao iniciar: uma mudança reinicia o jogo uma vez ao apertar Jogar."));
        if (ImGui::BeginPopupModal("##remove", NULL, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar)) {
            bool tr = mod_sel < nmods;
            int i = tr ? mod_sel : mod_sel - nmods;
            if (mod_sel < 0 || i >= (tr ? nmods : nplugs)) ImGui::CloseCurrentPopup();
            else {
                char path[300];
                snprintf(path, sizeof path, tr ? "mods/%s" : "plugins/%s", tr ? mods[i] : plugs[i]);
                ImGui::Text("%s %s?", T("Remove", "Remover"), tr ? mods[i] : plugin_title(plugs[i]));
                ImGui::TextDisabled("%s %s", T("This deletes", "Isto apaga"), path);
                ImGui::Spacing();
                if (ImGui::Button(T("Remove", "Remover"), ImVec2(140, 0))) {
                    if (tr) {
                        mod_set(mods[i], false);
                        remove_tree(path);
                        nmods = mods_list(mods, MODS_MAX);
                        mod_prune();
                    } else if (remove(path)) {  // (a plugin loaded now: Windows keeps its file - gone at the next start)
                        char later[320];
                        snprintf(later, sizeof later, "%s.removed", path);
                        rename(path, later);
                    }
                    if (!tr) plugins_refresh();
                    snprintf(mod_msg, sizeof mod_msg, "%s %s", T("Removed", "Removido"), path);
                    mod_sel = -1;
                    ImGui::CloseCurrentPopup();
                }
                ImGui::SameLine();
                if (ImGui::Button(T("Cancel", "Cancelar"), ImVec2(140, 0)) || cancel_pressed()) ImGui::CloseCurrentPopup();
                ImGui::SetItemDefaultFocus();
            }
            ImGui::EndPopup();
        }
    };
    auto mod_install_row = [&]() {
        if (inst == MOD_INSTALL_RUNNING) ImGui::TextDisabled("%s", T("Installing the translation...", "Instalando a tradução..."));
        else {
            if (ImGui::SmallButton(T("Install a mod...", "Instalar mod...")))
                SDL_ShowOpenFileDialog(picked_mod, NULL, win, mod_filters, 1, NULL, false);
            if (inst == MOD_INSTALL_FAIL_UNPACK || inst == MOD_INSTALL_FAIL_NOT_MOD) {
                ImGui::SameLine();
                ImGui::TextColored(k_red, "%s", inst == MOD_INSTALL_FAIL_UNPACK ? T("Couldn't unpack that file.", "Não foi possível abrir esse arquivo.")
                                                                                : T("No translation for this game in that file.", "Esse arquivo não tem tradução para este jogo."));
            }
        }
    };
    auto recommended_row = [&]() {  // (this PC, and the settings recommended for it)
        ImGui::TextDisabled("%s", pc);
        ImGui::TextDisabled("%s %s", T("Recommended:", "Recomendado:"), rec.text);
        ImGui::SameLine();
        if (ImGui::SmallButton(T("Apply", "Aplicar"))) apply_rec();
        if (ImGui::SmallButton(T("Restore defaults", "Restaurar padrões"))) restore_defaults();
        ImGui::SameLine();
        ImGui::TextDisabled("%s", T("(the settings a new install starts with)", "(as configurações de uma instalação nova)"));
    };

    // The classic start screen: everything in one centered window (console ports)
    auto classic_frame = [&]() {
        ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x / 2, io.DisplaySize.y / 2), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
        ImGui::Begin(RT_APP_NAME, NULL, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoMove |
                                            ImGuiWindowFlags_NoSavedSettings);
        game_files_section(win);
        ImGui::SeparatorText(T("Settings", "Configurações"));
        ImGui::PushItemWidth(420);
        settings_display();
        settings_graphics();
        settings_game();
        settings_mods();
        ImGui::PopItemWidth();
        mod_install_row();
        if (getenv("LARECOMP_LAUNCHER_PAD")) ImGui::SetNextItemOpen(true, ImGuiCond_Once);
        bool pad_open = ImGui::CollapsingHeader(T("Controller", "Controle"));
        if (pad_open) controller_section();
        if (keys_test) ImGui::SetNextItemOpen(true, ImGuiCond_Once);
        if (ImGui::CollapsingHeader(T("Keyboard", "Teclado"))) keys_section();
        ImGui::Spacing();
        recommended_row();  // (above, where the Play button keeps the view)
        ImGui::BeginDisabled(!ready);
        if (frame == 0 && ready) ImGui::SetKeyboardFocusHere();
        if (ImGui::Button(T("Play", "Jogar"), ImVec2(-FLT_MIN, 0))) play = true;
        ImGui::EndDisabled();
        ImGui::Checkbox(T("Don't show this screen at start", "Não mostrar esta tela ao iniciar"), &skipstart);
        if (skipstart)  // (the settings are in the game too: runtime/hle/pla_options.c)
            ImGui::TextDisabled("%s", T("These settings are also in the game: Menu > Help > Settings.\n"
                                        "Hold Shift while starting the game to see this screen again.",
                                        "Estas configurações também estão no jogo: Menu > Help > Settings.\n"
                                        "Segure Shift ao iniciar o jogo para ver esta tela de novo."));
        ImGui::TextDisabled("v" RT_APP_VERSION);
        ImGui::End();
        pad_nav_update(io, pad_open);
    };

    // The launcher: the banner and the buttons (home), or a page with a header bar and Back
    auto launcher_frame = [&]() {
        float W = io.DisplaySize.x, H = io.DisplaySize.y;
        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(ImVec2(W, H));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0);
        ImGui::Begin("##launcher", NULL, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                                             ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoScrollWithMouse);
        ImGui::PopStyleVar(2);
        ImDrawList *d = ImGui::GetWindowDrawList();
        int st = setup_state();
        bool running = setup_running();
        bool pad_page = false;
        static bool busy_before;  // (a popup open or a control in use last frame: Esc / B closed that, not the page)
        static int last_page = PAGE_HOME;  // (back on the home page: the keyboard / controller focus on the button left from)
        int came_from = page == PAGE_HOME && last_page != PAGE_HOME ? last_page : -1;
        last_page = page;
        if (page == PAGE_HOME) {
            float split = floorf(W * 0.64f), strip = 92;
            banner(d, ImVec2(0, 0), ImVec2(split, H), (float)ImGui::GetTime());
            d->AddRectFilled(ImVec2(split, 0), ImVec2(W, H), k_panel);
            d->AddLine(ImVec2(split, 0), ImVec2(split, H), IM_COL32(240, 196, 92, 90), 2);
            // status strip under the banner: the game files
            d->AddRectFilled(ImVec2(0, H - strip), ImVec2(split, H), IM_COL32(12, 10, 18, 230));
            ImGui::SetCursorScreenPos(ImVec2(28, H - strip + 14));
            ImGui::BeginGroup();
            ImGui::PushTextWrapPos(split - 28);
            if (running) {
                static const char *const en[] = {"", "Looking for the game files...", "Reading the game files out of the packages...",
                                                 "Copying the game files...", "Building the game data image...", "Reading the game code...",
                                                 "Recompiling the game code...", "Finishing..."};
                static const char *const pt[] = {"", "Procurando os arquivos do jogo...", "Lendo os arquivos do jogo dos pacotes...",
                                                 "Copiando os arquivos do jogo...", "Montando a imagem de dados do jogo...",
                                                 "Lendo o código do jogo...", "Recompilando o código do jogo...", "Finalizando..."};
                ImGui::TextUnformatted(st >= 1 && st <= 7 ? T(en[st], pt[st]) : "...");
                ImGui::ProgressBar(setup_progress(), ImVec2(split - 56, 0));
            } else if (ready) {
                const char *name = game_name("extracted/exefs");
                ImGui::TextColored(k_green, g_pt ? "Pokémon %s - pronto para jogar" : "Pokémon %s - ready to play", name ? name : "Legends: Arceus");
                ImGui::TextDisabled("%s", pc);
            } else if (st >= SETUP_FAIL_NOT_FOUND) {
                ImGui::TextColored(k_red, "%s", T("The game files couldn't be set up.", "Os arquivos do jogo não puderam ser preparados."));
                ImGui::TextDisabled("%s", T("Open Game Files for why.", "Abra Arquivos do jogo para ver o motivo."));
            } else {
                ImGui::TextColored(k_yellow, "%s", T("Game files needed", "Faltam os arquivos do jogo"));
                ImGui::TextDisabled("%s", T("Your own dumped base game and update (.nsp / .xci) and prod.keys.",
                                            "O dump do seu jogo base e da atualização (.nsp / .xci) e o prod.keys."));
            }
            ImGui::PopTextWrapPos();
            ImGui::EndGroup();
            // the buttons
            float bx = split + 34, bw = W - split - 68, y = 52;
            ImGui::SetCursorScreenPos(ImVec2(bx, y));
            if (frame == 0) ImGui::SetKeyboardFocusHere();
            if (ready) { if (big_button(T("PLAY", "JOGAR"), bw, 88, true, inst != MOD_INSTALL_RUNNING)) play = true; }
            else if (big_button(running ? T("SETTING UP...", "PREPARANDO...") : T("SET UP GAME", "PREPARAR O JOGO"), bw, 88, true, !running))
                page = PAGE_FILES;
            y += 88 + 28;
            const struct { const char *en, *pt; int to; } rows[] = {
                {"OPTIONS", "OPÇÕES", PAGE_OPTIONS}, {"CONTROLS", "CONTROLES", PAGE_CONTROLS},
                {"GAME FILES", "ARQUIVOS DO JOGO", PAGE_FILES}, {"MODS", "MODS", PAGE_MODS}, {"EXIT", "SAIR", -1}};
            for (const auto &r : rows) {
                ImGui::SetCursorScreenPos(ImVec2(bx, y));
                if (came_from >= 0 && r.to == came_from) ImGui::SetKeyboardFocusHere();  // (Exit's -1 isn't a page)
                if (big_button(T(r.en, r.pt), bw, 58, false, true)) {
                    if (r.to < 0) exit(0);
                    page = r.to, g_tab_cur = 0;
                    g_focus_first = io.NavVisible;  // (opened from the keyboard / a controller: the focus goes along)
                }
                y += 58 + 10;
            }
            ImGui::SetCursorScreenPos(ImVec2(bx, H - 86));
            ImGui::Checkbox(T("Skip the launcher", "Pular o launcher"), &skipstart);
            if (skipstart && ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", T("Hold Shift while starting the game to see it again.", "Segure Shift ao iniciar o jogo para ver de novo."));
            ImGui::SetCursorScreenPos(ImVec2(bx, H - 40));
            ImGui::TextDisabled("v" RT_APP_VERSION);
            pad_hints(d, W - 34, H - 38, false, false);
        } else {
            static const char *const titles_en[] = {"", "Options", "Controls", "Game Files", "Mods"};
            static const char *const titles_pt[] = {"", "Opções", "Controles", "Arquivos do jogo", "Mods"};
            float bar = 96;
            banner_sky(d, ImVec2(0, 0), ImVec2(W, bar));
            d->AddLine(ImVec2(0, bar), ImVec2(W, bar), IM_COL32(240, 196, 92, 120), 2);
            ImFont *bf = g_button_font ? g_button_font : ImGui::GetFont();
            d->AddText(bf, 30, ImVec2(36, 14), k_accent, "PL:A RECOMP");
            d->AddText(bf, 44, ImVec2(36, 40), IM_COL32(240, 244, 255, 255), T(titles_en[page], titles_pt[page]));
            ImGui::SetCursorScreenPos(ImVec2(W - 36 - 180, 26));
            if (ImGui::Button(T("Back", "Voltar"), ImVec2(180, 0))) page = PAGE_HOME;
            pad_hints(d, W - 36 - 180 - 30, 36, true, page == PAGE_OPTIONS || page == PAGE_CONTROLS);
            ImGui::SetCursorScreenPos(ImVec2(36, bar + 18));
            ImGui::BeginChild("##page", ImVec2(W - 72, H - bar - 36));
            ImGui::PushItemWidth(420);
            if (page == PAGE_OPTIONS) {
                tab_begin(3);
                if (ImGui::BeginTabBar("##opt")) {
                    if (ImGui::BeginTabItem(T("Display", "Tela"), NULL, tab_flags(0))) { tab_shown(0); ImGui::Spacing(); focus_first(); settings_display(); ImGui::EndTabItem(); }
                    if (ImGui::BeginTabItem(T("Graphics", "Gráficos"), NULL, tab_flags(1))) { tab_shown(1); ImGui::Spacing(); focus_first(); settings_graphics(); ImGui::EndTabItem(); }
                    if (ImGui::BeginTabItem(T("Game", "Jogo"), NULL, tab_flags(2))) { tab_shown(2); ImGui::Spacing(); focus_first(); settings_game(); ImGui::EndTabItem(); }
                    ImGui::EndTabBar();
                }
                ImGui::Spacing();
                ImGui::Separator();
                ImGui::Spacing();
                recommended_row();
                ImGui::TextDisabled("%s", T("These settings are also in the game: Menu > Help > Settings.",
                                            "Estas configurações também estão no jogo: Menu > Help > Settings."));
            } else if (page == PAGE_CONTROLS) {
                tab_begin(2);
                if (ImGui::BeginTabBar("##ctl")) {
                    ImGuiTabItemFlags kf = keys_test && frame < 2 ? ImGuiTabItemFlags_SetSelected : 0;
                    if (ImGui::BeginTabItem(T("Controller", "Controle"), NULL, tab_flags(0))) { tab_shown(0); pad_page = true; controller_section(); ImGui::EndTabItem(); }
                    if (ImGui::BeginTabItem(T("Keyboard", "Teclado"), NULL, kf | tab_flags(1))) { tab_shown(1); keys_section(); ImGui::EndTabItem(); }
                    ImGui::EndTabBar();
                }
            } else if (page == PAGE_FILES) {
                ImGui::TextWrapped("%s", T("Your own Pokémon Legends: Arceus 1.1.1: pick the folder with the dumped base game and update "
                                           "(.nsp / .xci, with your prod.keys) or the extracted exefs and romfs. They are set up once "
                                           "and the game code is recompiled for this PC.",
                                           "O seu Pokémon Legends: Arceus 1.1.1: escolha a pasta com o dump do jogo base e da atualização "
                                           "(.nsp / .xci, com o seu prod.keys) ou o exefs e o romfs extraídos. Eles são preparados uma vez "
                                           "e o código do jogo é recompilado para este PC."));
                ImGui::Spacing();
                focus_first();
                game_files_section(win);
            } else if (page == PAGE_MODS) {
                mods_page();
            }
            ImGui::PopItemWidth();
            ImGui::EndChild();
            if (cancel_pressed() && !busy_before && !g_pad_test && !bind_capture_active(NULL, NULL, NULL)) {
                g_focus_first = false;
                page = PAGE_HOME;
            }
        }
        busy_before = ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel) || ImGui::IsAnyItemActive();
        ImGui::End();
        pad_nav_update(io, pad_page);
    };

    // (~60 redraws a second: the frames aren't paced by the display here, and spun at ~3000 a second)
    auto pace = []() {
        static uint64_t last;
        uint64_t now = SDL_GetTicksNS();
        if (last && now - last < 16000000) SDL_DelayNS(16000000 - (now - last));
        last = SDL_GetTicksNS();
    };
    while (!play) {
        pace();
        menu_apply_window_requests(win);
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (bind_capture_event(&e)) continue;  // (waiting for a new key: the press is the binding's)
            if (e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat && !classic &&
                (e.key.scancode == SDL_SCANCODE_F11 || (e.key.scancode == SDL_SCANCODE_RETURN && (e.key.mod & SDL_KMOD_ALT)))) {
                SDL_SetWindowFullscreen(win, !(SDL_GetWindowFlags(win) & SDL_WINDOW_FULLSCREEN));  // (drawn to fit: ui_scale)
                continue;
            }
            ImGui_ImplSDL3_ProcessEvent(&e);
            if (e.type == SDL_EVENT_QUIT) exit(0);
        }
        char path[1024];
        if (take(g_pick_game, path, sizeof path)) setup_start(path);
        if (take(g_pick_mod, path, sizeof path)) {
            if (add_plugin(path, mod_msg, sizeof mod_msg)) {  // (a plugin: into plugins/, ticked)
                plugins_refresh();
                const char *base = strrchr(mod_msg, '/') ? strrchr(mod_msg, '/') + 1 : mod_msg;
                for (int i = 0; i < nplugs; i++) if (strstr(base, plugs[i])) plug_on[i] = true;
            } else mod_install_start(path);
        }
        static int last_install;
        inst = mod_install_state();
        if (inst != last_install && inst == MOD_INSTALL_DONE) {  // a new mod: listed and on, at the top of the order
            nmods = mods_list(mods, MODS_MAX);
            for (int i = 0; i < nmods; i++)
                if (!strcmp(mods[i], mod_install_name())) mod_set(mods[i], false), on_mods.insert(on_mods.begin(), mods[i]);
        }
        last_install = inst;
        ready = !setup_running() && setup_ready();
        if (autoplay && ready && inst != MOD_INSTALL_RUNNING) play = true;
        // Shift is only seen once the window has the keyboard (a new window can take a while to get it, or never: a
        // compositor may leave focus where it was): decided a moment after the focus comes, or after 1.5 s without it
        static uint64_t focused_at;
        if (!focused_at && (SDL_GetWindowFlags(win) & SDL_WINDOW_INPUT_FOCUS)) focused_at = SDL_GetTicks();
        const bool *keys = SDL_GetKeyboardState(NULL);
        if ((SDL_GetModState() & SDL_KMOD_SHIFT) || keys[SDL_SCANCODE_LSHIFT] || keys[SDL_SCANCODE_RSHIFT]) shift_seen = true;
        bool decide = focused_at ? SDL_GetTicks() - focused_at >= 250 : SDL_GetTicks() - opened >= 1500;
        if (skip_now && !shift_seen && decide && ready && inst != MOD_INSTALL_RUNNING) {
            fprintf(stderr, "[launcher] start screen skipped (skip_start_screen; Shift at start shows it)\n");
            play = true;
        }
        if (!vk_frame_begin(bg)) { SDL_Delay(16); continue; }
        if (keys_test && strchr(keys_test, ':') && frame == 5) {  // "<slot>:<key name>": the key pressed for the test
            SDL_Event k = {};
            k.type = SDL_EVENT_KEY_DOWN;
            k.key.scancode = SDL_GetScancodeFromName(strchr(keys_test, ':') + 1);
            k.key.down = true;
            SDL_PushEvent(&k);
        }
        ui_scale_begin(win, classic ? 1920.0f : (float)k_launcher_w, classic ? 1080.0f : (float)k_launcher_h, classic);
        ImGui_ImplVulkan_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ui_scale_apply(io);
        ImGui::NewFrame();
        if (classic) classic_frame();
        else launcher_frame();

        ImGui::Render();
        ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), (VkCommandBuffer)vk_frame_cmd());
        bool shot = shot_at && ++frame == shot_at;
        if (!shot_at) frame++;
        vk_frame_end(shot ? (getenv("LARECOMP_SHOT_PATH") ? getenv("LARECOMP_SHOT_PATH") : "build/launcher.png") : NULL);
        if (shot) exit(0);
    }
    g_settings.res_height = res_lines[res];
    g_settings.fullscreen = full;
    g_settings.antialias = smaa ? AA_SMAA : AA_OFF;
    if (!experimental && fps_i > 1) fps_i = 1;
    g_settings.experimental = experimental;
    g_settings.fps = fps_vals[fps_i >= 0 && fps_i < 6 ? fps_i : 0];
    g_settings.wild_battle = wildb;
    g_settings.show_fps = showfps;
    g_settings.fixed_steps = !realtime;
    g_settings.skip_start = skipstart;
    g_settings.button_prompts = prompts_i;
    g_settings.shadow_smooth = smooth_sh;
    g_settings.detail = detail_i;
    g_settings.shadows = shadows_i;
    g_settings.sharpen = sharp_i;
    g_settings.background = bg_i;
    g_settings.pkhex_link = pk_i;
    g_settings.shader_nowait = shl_i;
    if (gopt != gopt_was) g_settings.game_opts_next = gopt == g_settings.game_opts ? -1 : gopt;  // (back to the save's: nothing to do)
    {   // the mods that are on, in their order
        std::string list;
        for (auto &s : on_mods) list += (list.empty() ? "" : ",") + s;
        snprintf(g_settings.mod, sizeof g_settings.mod, "%s", list.c_str());
    }
    plugins_choose(plugs, plug_on, nplugs);
    settings_save();
    char plugins_now[1100];
    plugins_on_now(plugins_now, sizeof plugins_now);
    const char *again = getenv("LARECOMP_PLUGINS_RESTARTED");
    if (strcmp(plugins_at_start, plugins_now) && !(again && *again)) {  // the plugins loaded aren't the ones chosen: once more, straight in (once)
        fprintf(stderr, "[launcher] plugins changed (on: \"%s\"): restarting\n", plugins_now + (plugins_now[0] == ','));
        setenv("LARECOMP_LAUNCHER_AUTOPLAY", "1", 1), setenv("LARECOMP_PLUGINS_RESTARTED", "1", 1);
        plat_exec(NULL);
    }
    { extern void pkm_link_apply(void); pkm_link_apply(); }  // (PKHeX Link as just chosen: hle/pkm_link.c)
    if (warmup_needed(root)) {  // first start, or the first after an update: every shader of the game, once (warmup.c)
        warmup_start();
        float shown = 0;
        for (int done = 0, total = 0, ph; (ph = warmup_progress(&done, &total)) != 3;) {
            pace();  // (the warm-up's compile threads get the CPU)
            SDL_Event e;
            while (SDL_PollEvent(&e)) {
                ImGui_ImplSDL3_ProcessEvent(&e);
                if (e.type == SDL_EVENT_QUIT) exit(0);
            }
            if (!vk_frame_begin(bg)) { SDL_Delay(16); continue; }
            ui_scale_begin(win, classic ? 1920.0f : (float)k_launcher_w, classic ? 1080.0f : (float)k_launcher_h, classic);
            ImGui_ImplVulkan_NewFrame();
            ImGui_ImplSDL3_NewFrame();
            ui_scale_apply(io);
            ImGui::NewFrame();
            ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x / 2, io.DisplaySize.y / 2), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
            ImGui::SetNextWindowSize(ImVec2(760, 0));
            ImGui::Begin(RT_APP_NAME "##warmup", NULL, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
                                                         ImGuiWindowFlags_NoSavedSettings);
            ImGui::TextWrapped("%s", T("Preparing the game's shaders. This happens on the first start and after an update, "
                                       "so the game doesn't stutter or pop in later.",
                                       "Preparando os shaders do jogo. Isso acontece na primeira vez e depois de uma atualização, "
                                       "para o jogo não travar nem ter objetos surgindo depois."));
            ImGui::Spacing();
            char label[96];
            float frac = 0;
            if (ph == 1) snprintf(label, sizeof label, T("Looking through the game files... %d MB", "Procurando nos arquivos do jogo... %d MB"), done);
            else {
                frac = total ? (float)done / (float)total : 1.0f;
                snprintf(label, sizeof label, "%d / %d", done, total);
            }
            shown += (frac - shown) * 0.25f;  // (eased: the counter moves in bursts)
            ImGui::ProgressBar(ph == 1 ? -1.0f * (float)ImGui::GetTime() : shown, ImVec2(-FLT_MIN, 0), label);
            ImGui::End();
            ImGui::Render();
            ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), (VkCommandBuffer)vk_frame_cmd());
            vk_frame_end(NULL);
        }
    }
    if (sized) {  // the game's window: its usual size, or fullscreen (as runtime/nvn/vulkan/device.c opens it)
        int w = 1280, h = 720;
        if (getenv("LARECOMP_WINDOW")) sscanf(getenv("LARECOMP_WINDOW"), "%dx%d", &w, &h);
        SDL_SetWindowFullscreen(win, false);  // (the launcher's F11)
        SDL_SetWindowResizable(win, true);
        SDL_SetWindowSize(win, w, h);
        SDL_SetWindowPosition(win, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
        if (g_settings.fullscreen && !getenv("LARECOMP_WINDOW") && !getenv("LARECOMP_HIDDEN")) SDL_SetWindowFullscreen(win, true);
    }
    vk_imgui_backend_shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext(ctx);
    ImGui::SetCurrentContext(prev_ctx);
}
