// The mod API (runtime/modapi.h) for Legends: Arceus: what scripts (contrib/legendslua, docs/lua-api.md) get of the game.
// Characters: hle/pla_npc.c (people of our own the game makes - copies of a placement the level made - walked by their
// navigator); the player: hle/warp.c (its body - where it is, its axes, written to put it somewhere); the screen's
// notice: ui/menu.mm.
#include <stdio.h>
#include <string.h>

#include "../hle.h"
#include "../modapi.h"
#include "../mods.h"
#include "../platform.h"

void pla_npc_api_on(void);
int pla_npc_mod_spawn(const char *model, const float pos[3], float yaw_deg, const char *label);
void pla_npc_mod_remove(int h);
void pla_npc_mod_remove_all(void);
void pla_npc_mod_walk(int h, const float p[3], int run);
void pla_npc_mod_stop(int h);
void pla_npc_mod_set(int h, const float p[3], float yaw_deg);
int pla_npc_mod_state(int h, float p[3], float *yaw);
int pla_npc_mod_models(char *out, int cap);
uint64_t pla_npc_area(char *name, int cap);
bool warp_player_now(float pos[3], float *yaw);
void warp_teleport(const float p[3]);
uint64_t input_buttons_now(void);
void menu_notice_for(const char *text, unsigned ms);

static void enable(void) { pla_npc_api_on(); }
static int player_pos(float pos[3], float *yaw) { return warp_player_now(pos, yaw); }
static int lua_dirs(char (*dirs)[256], int max) { return mod_lua_dirs(dirs, max); }
static double now(void) { return (double)plat_now_ns() / 1e9; }

static const LarecompModApi g_modapi = {
    .version = LARECOMP_MODAPI_VERSION, .game = "pla", .enable = enable,
    .npc_spawn = pla_npc_mod_spawn, .npc_remove = pla_npc_mod_remove, .npc_remove_all = pla_npc_mod_remove_all,
    .npc_walk_to = pla_npc_mod_walk, .npc_stop = pla_npc_mod_stop, .npc_set_pos = pla_npc_mod_set, .npc_state = pla_npc_mod_state,
    .npc_models = pla_npc_mod_models,
    .player_pos = player_pos, .player_teleport = warp_teleport,
    .area = pla_npc_area, .buttons = input_buttons_now, .notify = menu_notice_for, .mod_lua_dirs = lua_dirs, .now = now,
};
const LarecompModApi *modapi_get(void) { return &g_modapi; }
