// PL:A / PL:Z-A Recomp start screen (from Let's Go Recomp's): the game files (picked once, set up and recompiled here:
// runtime/gamesetup.c), the resolution, fullscreen, anti-aliasing, a mod (mods/: runtime/mods.c), the controller (what is
// connected, a live test of every button and stick, remapping) and the keyboard, Play. In English, or in Portuguese when the system's language is (LARECOMP_LANG=en|pt
// chooses). The choices go to larecomp.ini (settings.c; bind_key_* from runtime/hle/bindings.c).
// Tests: LARECOMP_LAUNCHER_SHOT=<frame> saves build/launcher.png (or LARECOMP_SHOT_PATH) and exits;
// LARECOMP_LAUNCHER_AUTOPLAY=1 plays as soon as the game files are ready; LARECOMP_SETUP_DIR=<folder> sets them up
// from that folder at once; LARECOMP_MOD_INSTALL=<file or folder> installs a translation at once;
// LARECOMP_LAUNCHER_KEYS=1 opens the keyboard section, =<slot> also waits for a new key for that input
// (runtime/input_bind.h BIND_*), and <slot>:<key name> presses that key for it.
#include <SDL3/SDL.h>
#include <float.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include "imgui.h"
#include "imgui_impl_vulkan.h"
#include "imgui_impl_sdl3.h"

extern "C" {
#include "gamesetup.h"
#include "input_bind.h"
#include "mods.h"
#include "paths.h"
#include "settings.h"
void settings_save(void);
void ui_window_init(void);
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
    if (ImGui::BeginTable("##pad", 6, ImGuiTableFlags_SizingFixedFit)) {
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
static const ImVec4 k_green(0.45f, 0.85f, 0.50f, 1), k_red(1.0f, 0.45f, 0.40f, 1);

static void game_files_section(SDL_Window *win) {
    ImGui::SeparatorText(T("Game files", "Arquivos do jogo"));
    int st = setup_state();
    if (setup_running()) {
        static const char *const en[] = {"", "Looking for the game files...", "Copying the game files...",
                                         "Building the game data image...", "Reading the game code...",
                                         "Recompiling the game code...", "Finishing..."};
        static const char *const pt[] = {"", "Procurando os arquivos do jogo...", "Copiando os arquivos do jogo...",
                                         "Montando a imagem de dados do jogo...", "Lendo o código do jogo...",
                                         "Recompilando o código do jogo...", "Finalizando..."};
        ImGui::TextUnformatted(st >= 1 && st <= 6 ? T(en[st], pt[st]) : "...");
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
        return;
    }
    if (st >= SETUP_FAIL_NOT_FOUND) {
        static const char *const en[] = {"No game files found in that folder.", "Those are packed game files (.xci / .nsp).",
                                         "That's not a supported game (Pokémon Legends: Arceus).", "The files couldn't be copied.",
                                         "The game code didn't build.", "The game code builder is missing from this copy."};
        static const char *const pt[] = {"Nenhum arquivo do jogo nessa pasta.", "Esses são arquivos do jogo empacotados (.xci / .nsp).",
                                         "Esse não é um jogo suportado (Pokémon Legends: Arceus).", "Os arquivos não puderam ser copiados.",
                                         "O código do jogo não compilou.", "O compilador do código do jogo não está nesta cópia."};
        static const char *const en_h[] = {"Choose the folder that holds exefs and romfs.", "Extract them first (exefs and romfs).",
                                           "Pick the folder of your own extracted Pokémon Legends: Arceus.", "Check there are about 6 GB free and try again.",
                                           "Check there are about 2 GB free and try again; details in lasrecomp.log.", "Download the release again."};
        static const char *const pt_h[] = {"Escolha a pasta que tem exefs e romfs.", "Extraia antes (exefs e romfs).",
                                           "Escolha a pasta do seu Pokémon Legends: Arceus extraído.", "Confira se há uns 6 GB livres e tente de novo.",
                                           "Confira se há uns 2 GB livres e tente de novo; detalhes no lasrecomp.log.", "Baixe a release de novo."};
        int k = st - SETUP_FAIL_NOT_FOUND;
        ImGui::TextColored(k_red, "%s", T(en[k], pt[k]));
        ImGui::TextDisabled("%s", T(en_h[k], pt_h[k]));
    } else {
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + 900);
        ImGui::TextUnformatted(T("Select the folder with your own extracted Pokémon Legends: Arceus (exefs and romfs).",
                                 "Selecione a pasta com o seu Pokémon Legends: Arceus extraído (exefs e romfs)."));
        ImGui::PopTextWrapPos();
    }
    if (ImGui::Button(T("Select game folder...", "Selecionar pasta do jogo..."), ImVec2(-FLT_MIN, 0)))
        SDL_ShowOpenFolderDialog(picked_game, NULL, win, NULL, false);
}

// ------------------------------------------------------------------ the start screen
extern "C" void launcher_lg_run(const char *root) {
    (void)root;
    pick_language();
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
    ImGui::StyleColorsDark();
    ImGui::GetStyle().ScaleAllSizes(1.5f);
    ImGui_ImplSDL3_InitForVulkan(win);
    vk_imgui_backend_init();

    static const char *const res_en[] = {"Automatic (window size)", "720p", "1080p", "1440p", "4K"};
    static const char *const res_pt[] = {"Automática (tamanho da janela)", "720p", "1080p", "1440p", "4K"};
    static const int res_lines[] = {0, 720, 1080, 1440, 2160};
    int res = 0;
    for (int i = 0; i < 5; i++) if (res_lines[i] == g_settings.res_height) res = i;
    bool full = g_settings.fullscreen, smaa = g_settings.antialias == AA_SMAA, wildb = g_settings.wild_battle, showfps = g_settings.show_fps;
    int fps_i = g_settings.fps == 144 ? 3 : g_settings.fps == 120 ? 2 : g_settings.fps == 60 ? 1 : 0;
    int prompts_i = g_settings.button_prompts >= 0 && g_settings.button_prompts <= 4 ? g_settings.button_prompts : 0;
    // Translation: the folders under mods/ (runtime/mods.h); 0 = none (the game's own English)
    static char mods[MODS_MAX][128];
    int nmods = mods_list(mods, MODS_MAX), mod = 0;
    for (int i = 0; i < nmods; i++) if (!strcmp(mods[i], g_settings.mod)) mod = i + 1;
    long shot_at = getenv("LARECOMP_LAUNCHER_SHOT") ? atol(getenv("LARECOMP_LAUNCHER_SHOT")) : 0, frame = 0;
    bool autoplay = getenv("LARECOMP_LAUNCHER_AUTOPLAY") != NULL, play = false;
    const char *keys_test = getenv("LARECOMP_LAUNCHER_KEYS");
    if (keys_test && atoi(keys_test) > 0) bind_capture_begin(BIND_DEV_KEY, atoi(keys_test));
    if (getenv("LARECOMP_SETUP_DIR")) setup_start(getenv("LARECOMP_SETUP_DIR"));
    if (getenv("LARECOMP_MOD_INSTALL")) mod_install_start(getenv("LARECOMP_MOD_INSTALL"));
    static const float bg[4] = {0.08f, 0.08f, 0.10f, 1};
    static const SDL_DialogFileFilter mod_filters[] = {{"Translation / mod (.zip, .rar, .7z)", "zip;rar;7z"}};

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
        int inst = mod_install_state();
        if (inst != last_install && inst == MOD_INSTALL_DONE) {  // a new translation: listed and chosen
            nmods = mods_list(mods, MODS_MAX);
            for (int i = 0; i < nmods; i++) if (!strcmp(mods[i], mod_install_name())) mod = i + 1;
        }
        last_install = inst;
        bool ready = !setup_running() && setup_ready();
        if (autoplay && ready && inst != MOD_INSTALL_RUNNING) play = true;
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
        ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x / 2, io.DisplaySize.y / 2), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
        ImGui::Begin(RT_APP_NAME, NULL, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoMove |
                                            ImGuiWindowFlags_NoSavedSettings);
        game_files_section(win);
        ImGui::SeparatorText(T("Settings", "Configurações"));
        ImGui::PushItemWidth(420);
        ImGui::Combo(T("Resolution", "Resolução"), &res, g_pt ? res_pt : res_en, 5);
        if (ImGui::Checkbox(T("Fullscreen", "Tela cheia"), &full)) SDL_SetWindowFullscreen(win, full);
        ImGui::Checkbox(T("Anti-aliasing (SMAA)", "Suavizar serrilhado (SMAA)"), &smaa);
        static const char *const fps_names[] = {"30 fps", "60 fps", "120 fps", "144 fps"};  // (above 30: experimental)
        ImGui::Combo(T("Frame rate (experimental above 30)", "Taxa de quadros (experimental acima de 30)"), &fps_i, fps_names, 4);
        ImGui::Checkbox(T("Show FPS counter", "Mostrar contador de FPS"), &showfps);
        // (runtime/nvn/glyphs.c: the game's button icons redrawn for the controller in use)
        const char *const prompt_names[] = {T("Auto (what you play with)", "Automático (o que você usa)"), "Nintendo Switch", "Xbox",
                                            "PlayStation", T("Keyboard and mouse", "Teclado e mouse")};
        ImGui::Combo(T("Button prompts", "Ícones dos botões"), &prompts_i, prompt_names, 5);
        const char *none = T("None", "Nenhum");
        if (ImGui::BeginCombo(T("Mod", "Mod"), mod ? mods[mod - 1] : none)) {
            if (ImGui::Selectable(none, mod == 0)) mod = 0;
            for (int i = 0; i < nmods; i++)
                if (ImGui::Selectable(mods[i], mod == i + 1)) mod = i + 1;
            ImGui::EndCombo();
        }
        ImGui::PopItemWidth();
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
        if (getenv("LARECOMP_LAUNCHER_PAD")) ImGui::SetNextItemOpen(true, ImGuiCond_Once);
        bool pad_open = ImGui::CollapsingHeader(T("Controller", "Controle"));
        if (pad_open) controller_section();
        // (while it is open the controller's buttons are being tested, not driving the menu)
        if (pad_open) io.ConfigFlags &= ~ImGuiConfigFlags_NavEnableGamepad;
        else io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
        if (keys_test) ImGui::SetNextItemOpen(true, ImGuiCond_Once);
        if (ImGui::CollapsingHeader(T("Keyboard", "Teclado"))) keys_section();
        ImGui::Spacing();
        ImGui::BeginDisabled(!ready);
        if (frame == 0 && ready) ImGui::SetKeyboardFocusHere();
        if (ImGui::Button(T("Play", "Jogar"), ImVec2(-FLT_MIN, 0))) play = true;
        ImGui::EndDisabled();
        ImGui::TextDisabled("v" RT_APP_VERSION);
        ImGui::End();

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
    g_settings.fps = fps_i == 3 ? 144 : fps_i == 2 ? 120 : fps_i == 1 ? 60 : 30;
    g_settings.wild_battle = wildb;
    g_settings.show_fps = showfps;
    g_settings.button_prompts = prompts_i;
    snprintf(g_settings.mod, sizeof g_settings.mod, "%s", mod ? mods[mod - 1] : "");
    settings_save();
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
    vk_imgui_backend_shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext(ctx);
    ImGui::SetCurrentContext(prev_ctx);
}
