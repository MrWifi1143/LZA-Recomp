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
// controls|files|mods opens that launcher page.
#include <SDL3/SDL.h>
#include <float.h>
#include <math.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include "imgui.h"
#include "imgui_impl_vulkan.h"
#include "imgui_impl_sdl3.h"

extern "C" void vk_describe_gpu(char *name, size_t n, int *vram_gb, bool *integrated);  // nvn/vulkan/device.c
extern "C" {
#include "gamesetup.h"
#include "input_bind.h"
#include "mods.h"
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

static void keys_section(void) {
    int cap_dev, cap_slot, ms_left;
    bind_capture_active(&cap_dev, &cap_slot, &ms_left);
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

static void controller_section(void) {
    refresh_pads();
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
                           name ? name : "Legends: Z-A");
        ImGui::SameLine();
        if (ImGui::SmallButton(T("Change...", "Trocar..."))) SDL_ShowOpenFolderDialog(picked_game, NULL, win, NULL, false);
        if (st < SETUP_FAIL_NOT_FOUND) return;  // (a failed change: why, below - the game files set up before still play)
    }
    // (game files set up by an earlier release from another Z-A version: asked for again, as a failed setup would)
    if (!ready && st < SETUP_FAIL_NOT_FOUND && game_za_unsupported("extracted/exefs")) st = SETUP_FAIL_ZA_VERSION;
    if (st >= SETUP_FAIL_NOT_FOUND) {
        static const char *const en[] = {"No game files found in that folder.", "Those are compressed game files (.nsz / .xcz).",
                                         "That's not a supported game (Pokémon Legends: Z-A).", "The files couldn't be copied.",
                                         "The game code didn't build.", "The game code builder is missing from this copy.",
                                         "This needs Pokémon Legends: Z-A version 2.0.2; those files are another version.",
                                         "No keys found for those game files (prod.keys).", "Your keys can't open those game files.",
                                         "Only the update was found, not the base game.", "Those game files couldn't be read."};
        static const char *const pt[] = {"Nenhum arquivo do jogo nessa pasta.", "Esses são arquivos do jogo comprimidos (.nsz / .xcz).",
                                         "Esse não é um jogo suportado (Pokémon Legends: Z-A).", "Os arquivos não puderam ser copiados.",
                                         "O código do jogo não compilou.", "O compilador do código do jogo não está nesta cópia.",
                                         "É preciso o Pokémon Legends: Z-A versão 2.0.2; esses arquivos são de outra versão.",
                                         "Nenhuma chave encontrada para esses arquivos (prod.keys).", "As suas chaves não abrem esses arquivos.",
                                         "Só a atualização foi encontrada, não o jogo base.", "Esses arquivos do jogo não puderam ser lidos."};
        static const char *const en_h[] = {"Choose the folder with your .nsp / .xci files, or the one that holds exefs and romfs.",
                                           "Decompress them first (nsz -D), or dump them as .nsp / .xci.",
                                           "Pick the folder of your own Pokémon Legends: Z-A.", "Check there are about 6 GB free and try again.",
                                           "Check there are about 2 GB free and try again; details in lasrecomp.log.", "Download the release again.",
                                           "Update the game to 2.0.2 on your Switch, dump the base game and the update, and put both in one folder (README).",
                                           "Dump prod.keys from your own Switch and put it next to the game files (or in .switch in your home folder).",
                                           "Dump prod.keys again from your Switch on its current system version; details in lasrecomp.log.",
                                           "Put the base game's .nsp / .xci in the same folder as the update.",
                                           "The dump may be damaged; details in lasrecomp.log."};
        static const char *const pt_h[] = {"Escolha a pasta com os seus arquivos .nsp / .xci, ou a que tem exefs e romfs.",
                                           "Descomprima antes (nsz -D), ou faça o dump como .nsp / .xci.",
                                           "Escolha a pasta do seu Pokémon Legends: Z-A.", "Confira se há uns 6 GB livres e tente de novo.",
                                           "Confira se há uns 2 GB livres e tente de novo; detalhes no lasrecomp.log.", "Baixe a release de novo.",
                                           "Atualize o jogo para a 2.0.2 no Switch, faça o dump do jogo base e da atualização e coloque os dois numa pasta (README).",
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
        ImGui::TextUnformatted(T("Select the folder with your own Pokémon Legends: Z-A: the dumped base game and 2.0.2 update "
                                 "(.nsp / .xci, with your prod.keys), or the extracted exefs and romfs.",
                                 "Selecione a pasta com o seu Pokémon Legends: Z-A: o dump do jogo base e da atualização 2.0.2 "
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
static const ImU32 k_accent = IM_COL32(200, 236, 72, 255), k_accent_hi = IM_COL32(222, 250, 110, 255),
                   k_panel = IM_COL32(15, 16, 24, 255);

static uint32_t rnd(uint32_t *s) { return *s = *s * 1664525u + 1013904223u, *s >> 8; }

// Night sky and the UI's dot grid (the game's menus have one), shared by the banner and the page headers
static void banner_sky(ImDrawList *d, ImVec2 a, ImVec2 b) {
    d->AddRectFilledMultiColor(a, b, IM_COL32(10, 14, 34, 255), IM_COL32(16, 12, 40, 255), IM_COL32(34, 18, 52, 255),
                               IM_COL32(14, 20, 46, 255));
    float h = b.y - a.y;
    for (float y = a.y + 12; y < b.y; y += 24)
        for (float x = a.x + 12; x < b.x; x += 24)
            d->AddCircleFilled(ImVec2(x, y), 1.3f, IM_COL32(150, 190, 255, (int)(34 * (1 - (y - a.y) / h) + 6)), 6);
}

// Lumiose City's Prism Tower as glowing wireframe: a dark body outlined in the accent - the tiered base with window
// bands and the arched door, curved legs crossed by beams up to the deck, the hourglass shaft with its long X-lattice
// window, the crown's cup with two horns curving out and the spire with its antenna; red lights on the horn tips.
// H: its height to the antenna's tip, from the ground.
static void prism_tower(ImDrawList *d, float cx, float ground, float H, float t) {
    const ImU32 fill = IM_COL32(12, 18, 30, 255), c = IM_COL32(160, 220, 90, 235), dim = IM_COL32(120, 180, 70, 130);
    auto Y = [&](float f) { return ground - f * H; };
    auto legw = [&](float f) { float u = (f - 0.075f) / 0.165f; return H * (0.070f + 0.085f * (1 - u) * (1 - u)); };
    auto shw = [&](float f) { float u = f - 0.52f; return H * (0.036f + 0.52f * u * u); };
    float cup0 = shw(0.80f);
    ImVec2 tips[2], back_lights[2];  // (the fins' red lights)
    // a body part between heights f0..f1 with half width hw(f): filled dark, its two sides stroked
    auto part = [&](float f0, float f1, int n, auto hw, float thick) {
        ImVec2 L[96], R[96];
        for (int i = 0; i <= n; i++) {
            float f = f0 + (f1 - f0) * i / n;
            L[i] = ImVec2(cx - hw(f), Y(f)), R[i] = ImVec2(cx + hw(f), Y(f));
        }
        ImDrawListFlags fl = d->Flags;
        d->Flags &= ~ImDrawListFlags_AntiAliasedFill;  // (slices meet without seams)
        for (int i = 0; i < n; i++) d->AddQuadFilled(L[i], R[i], R[i + 1], L[i + 1], fill);
        d->Flags = fl;
        d->AddPolyline(L, n + 1, c, 0, thick), d->AddPolyline(R, n + 1, c, 0, thick);
    };
    // the base: two tiers with window bands, the arched door
    for (int k = 0; k < 2; k++) {
        float hw = (k ? 0.165f : 0.215f) * H, y0 = Y(k ? 0.035f : 0.0f), y1 = Y(k ? 0.075f : 0.035f);
        d->AddRectFilled(ImVec2(cx - hw, y1), ImVec2(cx + hw, y0), fill);
        d->AddRect(ImVec2(cx - hw, y1), ImVec2(cx + hw, y0), c, 0, 0, 2.0f);
        float yb = Y(k ? 0.056f : 0.019f);
        for (float x = cx - hw + 8; x < cx + hw - 8; x += 9)  // (the window band: a row of lit panes)
            d->AddLine(ImVec2(x, yb - 3), ImVec2(x, yb + 3), dim, 1.2f);
    }
    d->PathArcTo(ImVec2(cx, Y(0.0f)), 0.045f * H, 3.14159265f, 6.2831853f, 24);  // the door
    d->PathFillConvex(fill);
    d->PathArcTo(ImVec2(cx, Y(0.0f)), 0.045f * H, 3.14159265f, 6.2831853f, 24);
    d->PathStroke(c, 0, 2.0f);
    // the legs, curving in up to the deck, beams crossed between them, the arch under them
    part(0.075f, 0.240f, 40, legw, 2.5f);
    for (int i = 0; i < 5; i++) {
        float f0 = 0.10f + 0.028f * i, f1 = f0 + 0.028f;
        float l0 = cx - legw(f0), r0 = cx + legw(f0), l1 = cx - legw(f1), r1 = cx + legw(f1);
        d->AddLine(ImVec2(l0, Y(f0)), ImVec2(r1, Y(f1)), dim, 1.2f), d->AddLine(ImVec2(r0, Y(f0)), ImVec2(l1, Y(f1)), dim, 1.2f);
        d->AddLine(ImVec2(l1, Y(f1)), ImVec2(r1, Y(f1)), c, 1.3f);
    }
    float arch = 0.045f * H;
    d->PathLineTo(ImVec2(cx - arch, Y(0.075f)));
    d->PathArcTo(ImVec2(cx, Y(0.120f)), arch, 3.14159265f, 6.2831853f, 20);
    d->PathLineTo(ImVec2(cx + arch, Y(0.075f)));
    d->PathFillConvex(IM_COL32(20, 34, 40, 255));
    d->PathLineTo(ImVec2(cx - arch, Y(0.075f)));
    d->PathArcTo(ImVec2(cx, Y(0.120f)), arch, 3.14159265f, 6.2831853f, 20);
    d->PathLineTo(ImVec2(cx + arch, Y(0.075f)));
    d->PathStroke(c, 0, 2.0f);
    d->AddRectFilled(ImVec2(cx - 0.095f * H, Y(0.258f)), ImVec2(cx + 0.095f * H, Y(0.240f)), fill);  // the deck
    d->AddRect(ImVec2(cx - 0.095f * H, Y(0.258f)), ImVec2(cx + 0.095f * H, Y(0.240f)), c, 0, 0, 2.0f);
    // the shaft and the crown, one outline: the shaft's sides flare out and run on up into the horns; each horn's inner
    // edge comes down into a rounded notch that rises into the spire
    {
        ImVec2 P[320];
        int n = 0;
        auto bez = [&](ImVec2 p0, ImVec2 p1, ImVec2 p2, int k, bool first) {
            for (int i = first ? 0 : 1; i <= k; i++) {
                float u = (float)i / k, v = 1 - u;
                P[n++] = ImVec2(v * v * p0.x + 2 * v * u * p1.x + u * u * p2.x, v * v * p0.y + 2 * v * u * p1.y + u * u * p2.y);
            }
        };
        // the crown, as on the Prism Tower card: the shaft flares into flat fins with slanted tops (the red lights on
        // their outer corners), a rounded notch between each fin and the stepped block in the middle that carries the
        // needle; a second pair of fins stands behind (drawn first, dimmer)
        float o = cup0, fin = o + 0.040f * H;  // (the shaft's half width where the crown starts; the fins' outer corners)
        for (int k = -1; k <= 1; k += 2) {  // (from under the notch's floor, behind the middle block and the front fin: only
                                            // the notch shows them, and their tops below the front fins')
            ImVec2 q[4] = {ImVec2(cx + k * 0.020f * H, Y(0.84f)), ImVec2(cx + k * 0.088f * H, Y(0.84f)),
                           ImVec2(cx + k * 0.088f * H, Y(0.958f)), ImVec2(cx + k * 0.030f * H, Y(0.946f))};
            d->AddQuadFilled(q[0], q[1], q[2], q[3], fill);
            d->AddPolyline(q, 4, IM_COL32(150, 210, 85, 190), ImDrawFlags_Closed, 1.8f);
            d->AddLine(ImVec2(cx + k * 0.046f * H, Y(0.86f)), ImVec2(cx + k * 0.048f * H, Y(0.948f)), dim, 1.0f);  // (its face's edge)
            back_lights[k > 0] = ImVec2(cx + k * 0.062f * H, Y(0.952f));  // (on the top edge, where the front fin doesn't hide it)
        }
        // left half: up the shaft, out along the fin's outer edge, across its slanted top, down its inner edge, round
        // the notch, up the middle block's step and shoulder to the needle
        for (int i = 0; i < 80; i++) {  // the shaft (an hourglass), from the deck up
            float f = 0.258f + 0.542f * i / 80;
            P[n++] = ImVec2(cx - shw(f), Y(f));
        }
        bez(ImVec2(cx - o, Y(0.80f)), ImVec2(cx - o - 0.020f * H, Y(0.875f)), ImVec2(cx - fin, Y(0.970f)), 24, true);  // (keeps the shaft's slope)
        P[n++] = ImVec2(cx - o + 0.004f * H, Y(0.958f));                                  // the slanted top's inner corner
        P[n++] = ImVec2(cx - o + 0.006f * H, Y(0.885f));                                  // the fin's inner edge
        bez(ImVec2(cx - o + 0.006f * H, Y(0.885f)), ImVec2(cx - o + 0.006f * H, Y(0.852f)), ImVec2(cx - 0.048f * H, Y(0.852f)), 8, false);
        bez(ImVec2(cx - 0.048f * H, Y(0.852f)), ImVec2(cx - 0.024f * H, Y(0.852f)), ImVec2(cx - 0.024f * H, Y(0.885f)), 8, false);  // the notch
        P[n++] = ImVec2(cx - 0.024f * H, Y(0.958f));                                      // the middle block
        P[n++] = ImVec2(cx - 0.013f * H, Y(0.968f));
        P[n++] = ImVec2(cx - 0.013f * H, Y(0.990f));                                      // its upper step
        P[n++] = ImVec2(cx - 0.005f * H, Y(0.997f));
        P[n++] = ImVec2(cx - 0.004f * H, Y(1.004f));                                      // the needle's foot
        int half = n;
        for (int i = half - 1; i >= 0; i--) P[n++] = ImVec2(2 * cx - P[i].x, P[i].y);  // (the right half, mirrored)
        ImDrawListFlags fl = d->Flags;
        d->Flags &= ~ImDrawListFlags_AntiAliasedFill;
        d->AddConcavePolyFilled(P, n, fill);
        d->Flags = fl;
        d->AddPolyline(P, n, c, 0, 2.5f);  // (open at the deck)
        for (int k = -1; k <= 1; k += 2)  // (the fins' faces: a fold line up each)
            d->AddLine(ImVec2(cx + k * (o + 0.004f * H), Y(0.83f)), ImVec2(cx + k * (fin - 0.014f * H), Y(0.962f)), dim, 1.2f);
        tips[0] = ImVec2(cx - fin, Y(0.970f)), tips[1] = ImVec2(cx + fin, Y(0.970f));
    }
    for (float f = 0.29f; f < 0.80f; f += 0.06f) d->AddLine(ImVec2(cx - shw(f), Y(f)), ImVec2(cx + shw(f), Y(f)), dim, 1.0f);  // rings
    d->AddLine(ImVec2(cx, Y(1.004f)), ImVec2(cx, Y(1.05f)), c, 2.0f);  // antenna
    d->AddLine(ImVec2(cx - 0.007f * H, Y(1.016f)), ImVec2(cx + 0.007f * H, Y(1.016f)), c, 1.3f);
    d->AddLine(ImVec2(cx - 0.004f * H, Y(1.030f)), ImVec2(cx + 0.004f * H, Y(1.030f)), c, 1.3f);
    // the long window up the shaft: its outline and its X lattice
    const float w0 = 0.30f, w1 = 0.78f;
    auto whw = [&](float f) { float u = (f - w0) / (w1 - w0); return shw(f) * 0.70f * sqrtf(fmaxf(0.0f, sinf(u * 3.14159265f))); };
    ImVec2 L[61], R[61];
    for (int i = 0; i <= 60; i++) {
        float f = w0 + (w1 - w0) * i / 60;
        L[i] = ImVec2(cx - whw(f), Y(f)), R[i] = ImVec2(cx + whw(f), Y(f));
    }
    d->AddPolyline(L, 61, c, 0, 1.8f), d->AddPolyline(R, 61, c, 0, 1.8f);
    for (int i = 0; i < 9; i++) {
        float f0 = w0 + (w1 - w0) * (i + 0.5f) / 9.5f, f1 = w0 + (w1 - w0) * (i + 1.5f) / 9.5f;
        d->AddLine(ImVec2(cx - whw(f0), Y(f0)), ImVec2(cx + whw(f1), Y(f1)), dim, 1.2f);
        d->AddLine(ImVec2(cx + whw(f0), Y(f0)), ImVec2(cx - whw(f1), Y(f1)), dim, 1.2f);
        d->AddLine(ImVec2(cx - whw(f1), Y(f1)), ImVec2(cx + whw(f1), Y(f1)), c, 1.3f);
    }
    // red lights on the horn tips (blinking), the beacon on the antenna
    float blink = sinf(t * 3.0f) > 0 ? 1.0f : 0.35f;
    for (ImVec2 p : tips) {
        d->AddCircleFilled(p, 7, IM_COL32(255, 60, 60, (int)(70 * blink)), 16);
        d->AddCircleFilled(p, 3, IM_COL32(255, 90, 80, (int)(255 * blink)), 12);
    }
    float blink2 = sinf(t * 3.0f + 1.6f) > 0 ? 0.8f : 0.25f;  // (the back fins' lights, out of step)
    for (ImVec2 p : back_lights) {
        d->AddCircleFilled(p, 6, IM_COL32(255, 60, 60, (int)(55 * blink2)), 16);
        d->AddCircleFilled(p, 2.5f, IM_COL32(255, 90, 80, (int)(230 * blink2)), 12);
    }
    float pulse = 0.5f + 0.5f * sinf(t * 2.2f);
    d->AddCircleFilled(ImVec2(cx, Y(1.05f)), 5 + 3 * pulse, IM_COL32(220, 255, 140, (int)(120 + 120 * pulse)), 16);
    d->AddCircleFilled(ImVec2(cx, Y(1.05f)), 2.5f, IM_COL32(255, 255, 230, 255), 12);
}

// The launcher's banner: a city at night around Prism Tower, its lights twinkling, and the title
static void banner(ImDrawList *d, ImVec2 a, ImVec2 b, float t) {
    d->PushClipRect(a, b, true);
    banner_sky(d, a, b);
    float w = b.x - a.x, h = b.y - a.y, cx = a.x + w * 0.66f, base = b.y - h * 0.16f;
    for (int i = 48; i > 0; i--)  // (the glow behind the tower)
        d->AddCircleFilled(ImVec2(cx, base - h * 0.40f), h * 0.011f * i, IM_COL32(150, 230, 90, 2), 64);
    uint32_t s = 7;
    for (int layer = 0; layer < 2; layer++) {  // back row (taller, darker), then the tower, then the front row
        if (layer == 1) prism_tower(d, cx, base, h * 0.74f, t);
        float x = a.x - 10;
        while (x < b.x) {
            float bw = 26.0f + (float)(rnd(&s) % 48), bh = h * (layer ? 0.06f + (float)(rnd(&s) % 100) * 0.0011f
                                                                       : 0.14f + (float)(rnd(&s) % 100) * 0.0022f);
            if (layer == 1 && fabsf(x + bw / 2 - cx) < w * 0.09f) { x += bw; continue; }  // (the tower's foot stays clear)
            ImVec2 p(x, base - bh), q(x + bw - 3, b.y);
            d->AddRectFilled(p, q, layer ? IM_COL32(8, 9, 20, 255) : IM_COL32(20, 22, 44, 255));
            for (float wy = p.y + 8; wy < base - 6; wy += 11)
                for (float wx = p.x + 5; wx < q.x - 6; wx += 9) {
                    uint32_t r = rnd(&s);
                    if (r % 5) continue;
                    float tw = 0.55f + 0.45f * sinf(t * (0.6f + (float)(r % 7) * 0.2f) + (float)(r % 31));
                    d->AddRectFilled(ImVec2(wx, wy), ImVec2(wx + 4, wy + 5), IM_COL32(255, 214, 120, (int)(60 + 150 * tw)));
                }
            x += bw;
        }
    }
    d->AddRectFilledMultiColor(ImVec2(a.x, base), b, IM_COL32(8, 9, 20, 0), IM_COL32(8, 9, 20, 0), IM_COL32(8, 9, 20, 255),
                               IM_COL32(8, 9, 20, 255));  // (the street fades into the status strip)
    ImFont *tf = g_title_font ? g_title_font : ImGui::GetFont(), *bf = g_button_font ? g_button_font : ImGui::GetFont();
    d->AddText(tf, 120, ImVec2(a.x + 40, a.y + 28), IM_COL32(245, 248, 255, 255), "PL:Z-A");
    const char *sub = "R  E  C  O  M  P";
    d->AddText(bf, 34, ImVec2(a.x + 46, a.y + 128), k_accent, sub);
    d->AddText(ImGui::GetFont(), ImGui::GetFontSize(), ImVec2(a.x + 46, a.y + 174), IM_COL32(170, 180, 205, 255),
               "Pokémon Legends: Z-A, recompiled for PC");
    d->PopClipRect();
}

// A launcher button: a wide flat bar with its label in Baloo 2; Play is the filled one
static bool big_button(const char *label, float w, float h, bool primary, bool enabled) {
    ImGui::BeginDisabled(!enabled);
    ImVec2 p = ImGui::GetCursorScreenPos();
    bool hit = ImGui::InvisibleButton(label, ImVec2(w, h));
    bool hot = enabled && (ImGui::IsItemHovered() || ImGui::IsItemFocused()), held = hot && ImGui::IsItemActive();
    ImDrawList *d = ImGui::GetWindowDrawList();
    ImU32 bg = primary ? (enabled ? (hot ? k_accent_hi : k_accent) : IM_COL32(70, 76, 60, 255))
                       : (held ? IM_COL32(255, 255, 255, 56) : hot ? IM_COL32(255, 255, 255, 34) : IM_COL32(255, 255, 255, 12));
    d->AddRectFilled(p, ImVec2(p.x + w, p.y + h), bg, 6);
    if (hot && !primary) d->AddRectFilled(p, ImVec2(p.x + 6, p.y + h), k_accent, 3);
    ImFont *f = g_button_font ? g_button_font : ImGui::GetFont();
    float fs = primary ? 44.0f : 32.0f;
    const char *end = strstr(label, "##");
    ImVec2 ts = f->CalcTextSizeA(fs, FLT_MAX, 0, label, end);
    ImU32 tc = primary ? IM_COL32(18, 22, 10, enabled ? 255 : 160) : enabled ? IM_COL32(236, 240, 250, 255) : IM_COL32(120, 124, 136, 255);
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
    if (sized) ui_window_size_hint(k_launcher_w, k_launcher_h);
    ui_window_init();
    SDL_Window *win = ui_window();
    ImGuiContext *prev_ctx = ImGui::GetCurrentContext();  // (the in-game menu's, when back here after a reboot)
    ImGuiContext *ctx = ImGui::CreateContext();
    ImGui::SetCurrentContext(ctx);
    ImGuiIO &io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    io.IniFilename = NULL;
    static const ImWchar ranges[] = {0x20, 0xFF, 0};  // (Latin-1: Portuguese accents)
    static const char *const fonts[] = {"C:/Windows/Fonts/segoeui.ttf", "/usr/share/fonts/noto/NotoSans-Regular.ttf",
                                        "/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf", "/usr/share/fonts/TTF/DejaVuSans.ttf",
                                        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", "/usr/share/fonts/dejavu-sans-fonts/DejaVuSans.ttf"};
    bool font = false;
    for (const char *p : fonts) {
        FILE *f = fopen(p, "rb");  // (a missing file is an assertion in ImGui, not a failure it returns)
        if (!f) continue;
        fclose(f);
        font = io.Fonts->AddFontFromFileTTF(p, 26, NULL, ranges) != NULL;
        if (font) break;
    }
    if (!font) io.Fonts->AddFontDefault();
    char baloo[1100];
    if (rt_resource("Baloo2-SemiBold.ttf", "third_party/fonts/Baloo2/Baloo2-SemiBold.ttf", baloo, sizeof baloo)) {
        g_button_font = io.Fonts->AddFontFromFileTTF(baloo, 44, NULL, ranges);
        g_title_font = io.Fonts->AddFontFromFileTTF(baloo, 120, NULL, ranges);
    }
    ImGui::StyleColorsDark();
    ImGui::GetStyle().ScaleAllSizes(1.5f);
    if (!classic) {  // (the launcher's colors: the banner's night blues, the accent for what is on)
        ImGuiStyle &st = ImGui::GetStyle();
        ImVec4 *c = st.Colors;
        c[ImGuiCol_WindowBg] = ImVec4(0.06f, 0.06f, 0.09f, 1);
        c[ImGuiCol_ChildBg] = ImVec4(0, 0, 0, 0);
        c[ImGuiCol_PopupBg] = ImVec4(0.08f, 0.09f, 0.13f, 0.98f);
        c[ImGuiCol_FrameBg] = ImVec4(0.12f, 0.14f, 0.22f, 1);
        c[ImGuiCol_FrameBgHovered] = ImVec4(0.18f, 0.21f, 0.32f, 1);
        c[ImGuiCol_FrameBgActive] = ImVec4(0.22f, 0.26f, 0.38f, 1);
        c[ImGuiCol_Button] = ImVec4(0.16f, 0.19f, 0.29f, 1);
        c[ImGuiCol_ButtonHovered] = ImVec4(0.24f, 0.29f, 0.43f, 1);
        c[ImGuiCol_ButtonActive] = ImVec4(0.30f, 0.36f, 0.52f, 1);
        c[ImGuiCol_Header] = ImVec4(0.16f, 0.19f, 0.29f, 1);
        c[ImGuiCol_HeaderHovered] = ImVec4(0.24f, 0.29f, 0.43f, 1);
        c[ImGuiCol_HeaderActive] = ImVec4(0.30f, 0.36f, 0.52f, 1);
        c[ImGuiCol_CheckMark] = ImVec4(0.78f, 0.93f, 0.28f, 1);
        c[ImGuiCol_SliderGrab] = ImVec4(0.78f, 0.93f, 0.28f, 1);
        c[ImGuiCol_Tab] = ImVec4(0.12f, 0.14f, 0.22f, 1);
        c[ImGuiCol_TabHovered] = ImVec4(0.24f, 0.29f, 0.43f, 1);
        c[ImGuiCol_TabSelected] = ImVec4(0.22f, 0.26f, 0.40f, 1);
        c[ImGuiCol_TabSelectedOverline] = ImVec4(0.78f, 0.93f, 0.28f, 1);
        c[ImGuiCol_PlotHistogram] = ImVec4(0.78f, 0.93f, 0.28f, 1);
        c[ImGuiCol_Separator] = ImVec4(0.25f, 0.28f, 0.40f, 1);
        st.FrameRounding = st.GrabRounding = st.TabRounding = 6;
    }
    ImGui_ImplSDL3_InitForVulkan(win);
    vk_imgui_backend_init();

    static const char *const res_en[] = {"Automatic (window size)", "540p", "720p", "1080p", "1440p", "4K"};
    static const char *const res_pt[] = {"Automática (tamanho da janela)", "540p", "720p", "1080p", "1440p", "4K"};
    static const int res_lines[] = {0, 540, 720, 1080, 1440, 2160};
    int res = 0;
    for (int i = 0; i < 6; i++) if (res_lines[i] == g_settings.res_height) res = i;
    bool full = g_settings.fullscreen, smaa = g_settings.antialias == AA_SMAA, wildb = g_settings.wild_battle, showfps = g_settings.show_fps, realtime = !g_settings.fixed_steps;
    int fps_i = g_settings.fps == FPS_UNLIMITED ? 4 : g_settings.fps == 144 ? 3 : g_settings.fps == 120 ? 2 : g_settings.fps == 60 ? 1 : 0;
    // Recommended settings for this PC, from the GPU (integrated or not, its VRAM), the CPU threads and the RAM: shown
    // with an Apply button, and applied at once on a first start (no larecomp.ini yet). Measured 2026-10-28: Z-A's CPU
    // side holds 60 fps on four efficiency cores of an i9-12900K (Steam Deck class); its GPU side is ~2.4 ms a frame at
    // 1080p on an RTX 4090 and roughly halves at 540p - weak GPUs are what limit it, so the tiers go by the GPU.
    struct Rec { int res, fps, shadows, crowd, nowait; char text[160]; } rec;
    char pc[200];
    {
        char gpu[128];
        int vram_gb;
        bool igpu;
        vk_describe_gpu(gpu, sizeof gpu, &vram_gb, &igpu);
        int threads = SDL_GetNumLogicalCPUCores(), ram_gb = (SDL_GetSystemRAM() + 512) / 1024;  // (not plat_cpu_count: cpupin_init narrowed this thread already)
        // resolution index into res_lines below: 0 Auto, 1 540p, 2 720p, 3 1080p, 4 1440p, 5 4K; fps index: 0 30, 1 60
        if (igpu || vram_gb < 4) rec = {2, 0, 0, 0, threads <= 8, ""};
        else if (vram_gb < 8) rec = {3, 1, 0, 0, threads <= 8, ""};
        else if (vram_gb < 12) rec = {3, 1, 1, 0, threads <= 8, ""};
        else rec = {4, 1, 1, ram_gb >= 16 && threads >= 12 ? 1 : 0, threads <= 8, ""};
        if (ram_gb < 12) rec.shadows = 0, rec.crowd = 0;  // (both take game memory: hle/za_fixes.c)
        static const char *const rn[] = {"Auto", "540p", "720p", "1080p", "1440p", "4K"}, *const sn[] = {"Switch", "High", "Ultra"},
                                 *const cn[] = {"Switch", "2x", "4x", "Max"};
        snprintf(rec.text, sizeof rec.text, "%s, %s fps, shadows %s, draw distance %s%s", rn[rec.res], rec.fps ? "60" : "30",
                 sn[rec.shadows], cn[rec.crowd], rec.nowait ? ", shader loading: don't wait" : "");
        snprintf(pc, sizeof pc, "%s (%s%d GB), %d CPU threads, %d GB RAM", gpu, igpu ? "integrated, " : "", vram_gb, threads, ram_gb);
        fprintf(stderr, "[launcher] this PC: %s; recommended: %s%s\n", pc, rec.text, g_settings_fresh ? " (first start: applied)" : "");
    }
    int prompts_i = g_settings.button_prompts >= 0 && g_settings.button_prompts <= 4 ? g_settings.button_prompts : 0;
    int crowd_i = g_settings.crowd >= 0 && g_settings.crowd <= 3 ? g_settings.crowd : 0;
    int detail_i = g_settings.detail >= 0 && g_settings.detail <= 3 ? g_settings.detail : 0;
    int shadows_i = g_settings.shadows >= 0 && g_settings.shadows <= 2 ? g_settings.shadows : 0;
    bool smooth_sh = g_settings.shadow_smooth;
    int bg_i = g_settings.background >= 0 && g_settings.background <= 2 ? g_settings.background : 0;
    int pk_i = g_settings.pkhex_link >= 0 && g_settings.pkhex_link <= 2 ? g_settings.pkhex_link : 0;
    int shl_i = g_settings.shader_nowait ? 1 : 0;
    auto apply_rec = [&]() { res = rec.res, fps_i = rec.fps, shadows_i = rec.shadows, crowd_i = detail_i = rec.crowd, shl_i = rec.nowait; };
    if (g_settings_fresh) apply_rec();
    // Translation: the folders under mods/ (runtime/mods.h); 0 = none (the game's own English)
    static char mods[MODS_MAX][128];
    int nmods = mods_list(mods, MODS_MAX), mod = 0;
    for (int i = 0; i < nmods; i++) if (!strcmp(mods[i], g_settings.mod)) mod = i + 1;
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
        SDL_SetWindowSize(win, k_launcher_w, k_launcher_h);
        SDL_SetWindowResizable(win, false);  // (after the size: on Wayland it pins min = max = the current size)
        SDL_SetWindowPosition(win, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
    }
    const char *keys_test = getenv("LARECOMP_LAUNCHER_KEYS");
    if (keys_test && atoi(keys_test) > 0) bind_capture_begin(BIND_DEV_KEY, atoi(keys_test));
    if (getenv("LARECOMP_SETUP_DIR")) setup_start(getenv("LARECOMP_SETUP_DIR"));
    if (getenv("LARECOMP_MOD_INSTALL")) mod_install_start(getenv("LARECOMP_MOD_INSTALL"));
    static const float bg[4] = {0.08f, 0.08f, 0.10f, 1};
    static const SDL_DialogFileFilter mod_filters[] = {{"Translation / mod (.zip, .rar, .7z)", "zip;rar;7z"}};
    int inst = 0;
    bool ready = false;

    // The settings, shown by both screens: the launcher's Options tabs, or the classic screen's one list
    auto settings_display = [&]() {
        ImGui::Combo(T("Resolution", "Resolução"), &res, g_pt ? res_pt : res_en, 6);
        if (ImGui::Checkbox(T("Fullscreen", "Tela cheia"), &full) && classic) SDL_SetWindowFullscreen(win, full);  // (launcher: at Play)
        ImGui::Checkbox(T("Anti-aliasing (SMAA)", "Suavizar serrilhado (SMAA)"), &smaa);
        static const char *const fps_names_en[] = {"30 fps", "60 fps", "120 fps", "144 fps", "Unlimited"};  // (above 30: experimental)
        static const char *const fps_names_pt[] = {"30 fps", "60 fps", "120 fps", "144 fps", "Ilimitada"};
        ImGui::Combo(T("Frame rate (experimental above 30)", "Taxa de quadros (experimental acima de 30)"), &fps_i, g_pt ? fps_names_pt : fps_names_en, 4);  // (Unlimited hidden for now: battles)
        if (fps_i >= 1 && fps_i <= 3)  // (Unlimited: always real time, hle/fps60.c)
            ImGui::Checkbox(T("Keep full speed when the frame rate dips", "Manter a velocidade total quando a taxa de quadros cai"), &realtime);
        if (fps_i >= 2)  // (120 / 144 / Unlimited: the game's own logic was made for 30; some of it counts frames)
            ImGui::TextColored(k_yellow, "%s",
                               T("120 fps and above are experimental. Some scripted scenes can stall at these rates\n"
                                 "(a cutscene that walks you somewhere may never let go of you). If that happens,\n"
                                 "set 60 or 30 fps in the game's Settings and step into the scene again.",
                                 "120 fps ou mais são experimentais. Algumas cenas com script podem travar nessas taxas\n"
                                 "(uma cena que leva você a algum lugar pode nunca soltar o controle). Se acontecer,\n"
                                 "escolha 60 ou 30 fps nas configurações do jogo e entre na cena de novo."));
        ImGui::Checkbox(T("Show FPS counter", "Mostrar contador de FPS"), &showfps);
    };
    auto settings_graphics = [&]() {  // (the game's Settings rows of the same names: runtime/hle/za_options.c)
        const char *const crowd_names[] = {"Switch", "2x", "4x", "Max"};
        ImGui::Combo(T("Draw distance (people)", "Distância de desenho (pessoas)"), &crowd_i, crowd_names, 4);
        ImGui::Combo(T("Draw distance (detail)", "Distância de desenho (detalhes)"), &detail_i, crowd_names, 4);
        const char *const shadow_names[] = {"Switch (1024)", T("High (2048)", "Alta (2048)"), T("Ultra (4096)", "Ultra (4096)")};
        ImGui::Combo(T("Shadows", "Sombras"), &shadows_i, shadow_names, 3);
        ImGui::Checkbox(T("Smooth shadow edges", "Suavizar as bordas das sombras"), &smooth_sh);
        const char *const shl_names[] = {T("Wait (may stutter the first time)", "Esperar (pode travar na primeira vez)"),
                                         T("Don't wait (may show late the first time)", "Não esperar (pode aparecer atrasado na primeira vez)")};
        ImGui::Combo(T("Shader loading", "Carregamento de shaders"), &shl_i, shl_names, 2);
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
    };
    auto settings_mods = [&]() {
        const char *none = T("None", "Nenhum");
        if (ImGui::BeginCombo(T("Mod", "Mod"), mod ? mods[mod - 1] : none)) {
            if (ImGui::Selectable(none, mod == 0)) mod = 0;
            for (int i = 0; i < nmods; i++)
                if (ImGui::Selectable(mods[i], mod == i + 1)) mod = i + 1;
            ImGui::EndCombo();
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
        // (while it is open the controller's buttons are being tested, not driving the menu)
        if (pad_open) io.ConfigFlags &= ~ImGuiConfigFlags_NavEnableGamepad;
        else io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
        if (keys_test) ImGui::SetNextItemOpen(true, ImGuiCond_Once);
        if (ImGui::CollapsingHeader(T("Keyboard", "Teclado"))) keys_section();
        ImGui::Spacing();
        recommended_row();  // (above, where the Play button keeps the view)
        ImGui::BeginDisabled(!ready);
        if (frame == 0 && ready) ImGui::SetKeyboardFocusHere();
        if (ImGui::Button(T("Play", "Jogar"), ImVec2(-FLT_MIN, 0))) play = true;
        ImGui::EndDisabled();
        ImGui::Checkbox(T("Don't show this screen at start", "Não mostrar esta tela ao iniciar"), &skipstart);
        if (skipstart)  // (the settings are in the game too: runtime/hle/za_options.c)
            ImGui::TextDisabled("%s", T("These settings are also in the game: Menu > Help & Settings > Settings.\n"
                                        "Hold Shift while starting the game to see this screen again.",
                                        "Estas configurações também estão no jogo: Menu > Help & Settings > Settings.\n"
                                        "Segure Shift ao iniciar o jogo para ver esta tela de novo."));
        ImGui::TextDisabled("v" RT_APP_VERSION);
        ImGui::End();
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
        if (page == PAGE_HOME) {
            float split = floorf(W * 0.64f), strip = 92;
            banner(d, ImVec2(0, 0), ImVec2(split, H), (float)ImGui::GetTime());
            d->AddRectFilled(ImVec2(split, 0), ImVec2(W, H), k_panel);
            d->AddLine(ImVec2(split, 0), ImVec2(split, H), IM_COL32(200, 236, 72, 90), 2);
            // status strip under the banner: the game files
            d->AddRectFilled(ImVec2(0, H - strip), ImVec2(split, H), IM_COL32(6, 7, 14, 230));
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
                ImGui::TextColored(k_green, g_pt ? "Pokémon %s - pronto para jogar" : "Pokémon %s - ready to play", name ? name : "Legends: Z-A");
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
                if (big_button(T(r.en, r.pt), bw, 58, false, true)) {
                    if (r.to < 0) exit(0);
                    page = r.to;
                }
                y += 58 + 10;
            }
            ImGui::SetCursorScreenPos(ImVec2(bx, H - 86));
            ImGui::Checkbox(T("Skip the launcher", "Pular o launcher"), &skipstart);
            if (skipstart && ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", T("Hold Shift while starting the game to see it again.", "Segure Shift ao iniciar o jogo para ver de novo."));
            ImGui::SetCursorScreenPos(ImVec2(bx, H - 40));
            ImGui::TextDisabled("v" RT_APP_VERSION);
        } else {
            static const char *const titles_en[] = {"", "Options", "Controls", "Game Files", "Mods"};
            static const char *const titles_pt[] = {"", "Opções", "Controles", "Arquivos do jogo", "Mods"};
            float bar = 96;
            banner_sky(d, ImVec2(0, 0), ImVec2(W, bar));
            d->AddLine(ImVec2(0, bar), ImVec2(W, bar), IM_COL32(200, 236, 72, 120), 2);
            ImFont *bf = g_button_font ? g_button_font : ImGui::GetFont();
            d->AddText(bf, 30, ImVec2(36, 14), k_accent, "PL:Z-A RECOMP");
            d->AddText(bf, 44, ImVec2(36, 40), IM_COL32(240, 244, 255, 255), T(titles_en[page], titles_pt[page]));
            ImGui::SetCursorScreenPos(ImVec2(W - 36 - 180, 26));
            if (ImGui::Button(T("Back", "Voltar"), ImVec2(180, 0))) page = PAGE_HOME;
            ImGui::SetCursorScreenPos(ImVec2(36, bar + 18));
            ImGui::BeginChild("##page", ImVec2(W - 72, H - bar - 36));
            ImGui::PushItemWidth(420);
            if (page == PAGE_OPTIONS) {
                if (ImGui::BeginTabBar("##opt")) {
                    if (ImGui::BeginTabItem(T("Display", "Tela"))) { ImGui::Spacing(); settings_display(); ImGui::EndTabItem(); }
                    if (ImGui::BeginTabItem(T("Graphics", "Gráficos"))) { ImGui::Spacing(); settings_graphics(); ImGui::EndTabItem(); }
                    if (ImGui::BeginTabItem(T("Game", "Jogo"))) { ImGui::Spacing(); settings_game(); ImGui::EndTabItem(); }
                    ImGui::EndTabBar();
                }
                ImGui::Spacing();
                ImGui::Separator();
                ImGui::Spacing();
                recommended_row();
                ImGui::TextDisabled("%s", T("These settings are also in the game: Menu > Help & Settings > Settings.",
                                            "Estas configurações também estão no jogo: Menu > Help & Settings > Settings."));
            } else if (page == PAGE_CONTROLS) {
                if (ImGui::BeginTabBar("##ctl")) {
                    ImGuiTabItemFlags kf = keys_test && frame < 2 ? ImGuiTabItemFlags_SetSelected : 0;
                    if (ImGui::BeginTabItem(T("Controller", "Controle"))) { pad_page = true; controller_section(); ImGui::EndTabItem(); }
                    if (ImGui::BeginTabItem(T("Keyboard", "Teclado"), NULL, kf)) { keys_section(); ImGui::EndTabItem(); }
                    ImGui::EndTabBar();
                }
            } else if (page == PAGE_FILES) {
                ImGui::TextWrapped("%s", T("Your own Pokémon Legends: Z-A 2.0.2: pick the folder with the dumped base game and update "
                                           "(.nsp / .xci, with your prod.keys) or the extracted exefs and romfs. They are set up once "
                                           "and the game code is recompiled for this PC.",
                                           "O seu Pokémon Legends: Z-A 2.0.2: escolha a pasta com o dump do jogo base e da atualização "
                                           "(.nsp / .xci, com o seu prod.keys) ou o exefs e o romfs extraídos. Eles são preparados uma vez "
                                           "e o código do jogo é recompilado para este PC."));
                ImGui::Spacing();
                game_files_section(win);
            } else if (page == PAGE_MODS) {
                ImGui::TextWrapped("%s", T("Translations and other mods for the game (mods/ in the game's folder). Pick one to play with, "
                                           "or install one from a .zip / .rar / .7z.",
                                           "Traduções e outros mods do jogo (mods/ na pasta do jogo). Escolha um para jogar, "
                                           "ou instale um de um .zip / .rar / .7z."));
                ImGui::Spacing();
                settings_mods();
                mod_install_row();
            }
            ImGui::PopItemWidth();
            ImGui::EndChild();
            if (ImGui::IsKeyPressed(ImGuiKey_Escape) && !bind_capture_active(NULL, NULL, NULL)) page = PAGE_HOME;
        }
        // (while the controller tab is open its buttons are being tested, not driving the launcher)
        if (pad_page) io.ConfigFlags &= ~ImGuiConfigFlags_NavEnableGamepad;
        else io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
        ImGui::End();
    };

    while (!play) {
        menu_apply_window_requests(win);
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (bind_capture_event(&e)) continue;  // (waiting for a new key: the press is the binding's)
            ImGui_ImplSDL3_ProcessEvent(&e);
            if (e.type == SDL_EVENT_QUIT) exit(0);
        }
        char path[1024];
        if (take(g_pick_game, path, sizeof path)) setup_start(path);
        if (take(g_pick_mod, path, sizeof path)) mod_install_start(path);
        static int last_install;
        inst = mod_install_state();
        if (inst != last_install && inst == MOD_INSTALL_DONE) {  // a new translation: listed and chosen
            nmods = mods_list(mods, MODS_MAX);
            for (int i = 0; i < nmods; i++) if (!strcmp(mods[i], mod_install_name())) mod = i + 1;
        }
        last_install = inst;
        ready = !setup_running() && setup_ready();
        if (autoplay && ready && inst != MOD_INSTALL_RUNNING) play = true;
        if (SDL_GetModState() & SDL_KMOD_SHIFT) shift_seen = true;
        if (skip_now && !shift_seen && SDL_GetTicks() - opened >= 400 && ready && inst != MOD_INSTALL_RUNNING) {
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
        ImGui_ImplVulkan_NewFrame();
        ImGui_ImplSDL3_NewFrame();
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
    g_settings.fps = fps_i == 4 ? FPS_UNLIMITED : fps_i == 3 ? 144 : fps_i == 2 ? 120 : fps_i == 1 ? 60 : 30;
    g_settings.wild_battle = wildb;
    g_settings.show_fps = showfps;
    g_settings.fixed_steps = !realtime;
    g_settings.skip_start = skipstart;
    g_settings.button_prompts = prompts_i;
    g_settings.crowd = crowd_i;
    g_settings.detail = detail_i;
    g_settings.shadows = shadows_i;
    g_settings.shadow_smooth = smooth_sh;
    g_settings.background = bg_i;
    g_settings.pkhex_link = pk_i;
    g_settings.shader_nowait = shl_i;
    snprintf(g_settings.mod, sizeof g_settings.mod, "%s", mod ? mods[mod - 1] : "");
    settings_save();
    { extern void pkm_link_apply(void); pkm_link_apply(); }  // (PKHeX Link as just chosen: hle/pkm_link.c)
    if (warmup_needed(root)) {  // first start, or the first after an update: every shader of the game, once (warmup.c)
        warmup_start();
        float shown = 0;
        for (int done = 0, total = 0, ph; (ph = warmup_progress(&done, &total)) != 3;) {
            SDL_Event e;
            while (SDL_PollEvent(&e)) {
                ImGui_ImplSDL3_ProcessEvent(&e);
                if (e.type == SDL_EVENT_QUIT) exit(0);
            }
            if (!vk_frame_begin(bg)) { SDL_Delay(16); continue; }
            ImGui_ImplVulkan_NewFrame();
            ImGui_ImplSDL3_NewFrame();
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
