// The mod API (runtime/modapi.h) for Legends: Z-A: what scripts (contrib/legendslua, docs/lua-api.md) get of the game.
// Characters: hle/newnpc.c (new townspeople the game makes from a kept station spawn, moved by their node); the player:
// hle/warp.c (its body - where it is, its rotation, written to put it somewhere); the screen's notice: ui/menu.mm.
#include <stdio.h>
#include <string.h>

#include "../hle.h"
#include "../modapi.h"
#include "../mods.h"
#include "../platform.h"

void newnpc_api_on(void);
int newnpc_mod_spawn(const char *model, const float pos[3], float yaw_deg, const char *label);
void newnpc_mod_remove(int h);
void newnpc_mod_remove_all(void);
void newnpc_mod_walk(int h, const float p[3], int run);
void newnpc_mod_stop(int h);
void newnpc_mod_set(int h, const float p[3], float yaw_deg);
int newnpc_mod_state(int h, float p[3], float *yaw);
int newnpc_mod_models(char *out, int cap);
uint64_t newnpc_area(char *name, int cap);
bool warp_player_now(float pos[3], float *yaw);
void warp_teleport(const float p[3]);
uint64_t input_buttons_now(void);
void menu_notice_for(const char *text, unsigned ms);

static void enable(void) { newnpc_api_on(); }
bool newnpc_player_yaw(float *yaw);
uint64_t renderer_present_count(void);
// (once a frame at most: scripts ask for it many times)
static int player_pos(float pos[3], float *yaw) {
    static uint64_t at = ~0ull;
    static int ok;
    static float p[3], y;
    uint64_t f = renderer_present_count();
    if (f != at) {
        at = f, ok = warp_player_now(p, NULL), y = 0;
        if (ok) newnpc_player_yaw(&y);
    }
    if (!ok) return 0;
    memcpy(pos, p, 12);
    if (yaw) *yaw = y;
    return 1;
}
static int lua_dirs(char (*dirs)[256], int max) { return mod_lua_dirs(dirs, max); }
static double now(void) { return (double)plat_now_ns() / 1e9; }

static const LarecompModApi g_modapi = {
    .version = LARECOMP_MODAPI_VERSION, .game = "za", .enable = enable,
    .npc_spawn = newnpc_mod_spawn, .npc_remove = newnpc_mod_remove, .npc_remove_all = newnpc_mod_remove_all,
    .npc_walk_to = newnpc_mod_walk, .npc_stop = newnpc_mod_stop, .npc_set_pos = newnpc_mod_set, .npc_state = newnpc_mod_state,
    .npc_models = newnpc_mod_models,
    .player_pos = player_pos, .player_teleport = warp_teleport,
    .area = newnpc_area, .buttons = input_buttons_now, .notify = menu_notice_for, .mod_lua_dirs = lua_dirs, .now = now,
};
const LarecompModApi *modapi_get(void) { return &g_modapi; }
