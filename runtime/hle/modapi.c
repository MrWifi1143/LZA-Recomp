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
int pla_npc_mod_outfit(int h, const char *spec);
int pla_npc_mod_anim(int h, const char *name, int kind, float value);
int pla_npc_poke_spawn(int species, const float pos[3], float yaw_deg, int form, int shiny, const char *label);
void pla_npc_poke_remove(int h);
int pla_npc_poke_state(int h, float pos[3], float *yaw);
int warp_work(const char *name, int set, int64_t value, int64_t *out);
bool warp_player_now(float pos[3], float *yaw);
void warp_teleport(const float p[3]);
uint64_t input_buttons_now(void);
void menu_notice_for(const char *text, unsigned ms);

static void enable(void) { pla_npc_api_on(); }
static int player_pos(float pos[3], float *yaw) { return warp_player_now(pos, yaw); }
static int lua_dirs(char (*dirs)[256], int max) { return mod_lua_dirs(dirs, max); }
static double now(void) { return (double)plat_now_ns() / 1e9; }

int pla_item_give(int item, int n);  // (hle/pla_npc.c: the bag's own add)
int pla_script(const char *code);  // (hle/pla_script.c: the game's own Lua)
int pla_script_result(int id, char *out, int cap);
static int object_none(int h, char *out, int cap) { (void)h, (void)out, (void)cap; return 0; }
int pla_time_set(float hour);  // (hle/pla_npc.c: the clock, set as resting at camp does)
static int message_none(int slot, const char *text) { (void)slot, (void)text; return 0; }
int pla_npc_object_anim(const char *object, const char *param, int kind, float value);  // (hle/pla_npc.c)
int pla_npc_mod_talk(int h, int on);  // (hle/pla_npc.c: the game's Talk prompt over ours)
int pla_npc_mod_talked(void);
// ---- speech bubbles (runtime/bubble.h, hle/bubble.c): where a target stands, a script's bubble made and hidden (the
// scene camera: hle/warp.c's camera update hook)
#include "../bubble.h"
int bubble_target_where(const BubbleTarget *t, float feet[3]) {
    int st;
    switch (t->kind) {
    case BUBBLE_PLAYER: return warp_player_now(feet, NULL) ? 1 : 0;
    case BUBBLE_NPC: st = pla_npc_mod_state(t->handle, feet, NULL); return st == 0 ? -1 : st >= 3 ? 1 : 0;
    case BUBBLE_POKEMON: st = pla_npc_poke_state(t->handle, feet, NULL); return st == 0 ? -1 : st >= 3 ? 1 : 0;
    case BUBBLE_FN: return t->where(t->ctx, feet);
    default: return -1;  // (Arceus: no objects by name)
    }
}
static int bubble_api(int kind, int handle, const char *object, const float pos[3], float height, const char *text, float seconds) {
    BubbleTarget t = {.kind = kind, .handle = handle, .height = height, .owner = BUBBLE_OWNER_SCRIPT};
    if (kind < BUBBLE_POS || kind > BUBBLE_POKEMON) return 0;
    if (object) snprintf(t.object, sizeof t.object, "%s", object);
    if (pos) memcpy(t.pos, pos, 12);
    return bubble_show(&t, text, seconds);
}
static int bubble_hide_api(int id) {
    if (id == 0) { bubble_clear(BUBBLE_OWNER_SCRIPT); return 1; }
    return bubble_hide(id);
}
// the game's interface (runtime/gameui.h; hle/gameui.c - the same in both games)
int gameui_api_set(const char *layout, const char *pane, const char *key, const char *value);
void gameui_api_unset(const char *layout, const char *pane);
int gameui_api_clone(const char *layout, const char *src, const char *name);
#include "../gameui.h"
static const LarecompModApi g_modapi = {
    .version = LARECOMP_MODAPI_VERSION, .game = "pla", .enable = enable,
    .npc_spawn = pla_npc_mod_spawn, .npc_remove = pla_npc_mod_remove, .npc_remove_all = pla_npc_mod_remove_all,
    .npc_walk_to = pla_npc_mod_walk, .npc_stop = pla_npc_mod_stop, .npc_set_pos = pla_npc_mod_set, .npc_state = pla_npc_mod_state,
    .npc_models = pla_npc_mod_models,
    .player_pos = player_pos, .player_teleport = warp_teleport,
    .area = pla_npc_area, .buttons = input_buttons_now, .notify = menu_notice_for, .mod_lua_dirs = lua_dirs, .now = now,
    .npc_outfit = pla_npc_mod_outfit, .npc_anim = pla_npc_mod_anim,
    .pokemon_spawn = pla_npc_poke_spawn, .pokemon_remove = pla_npc_poke_remove, .pokemon_state = pla_npc_poke_state,
    .game_work = warp_work,
    .game_script = pla_script, .script_result = pla_script_result, .item_give = pla_item_give, .npc_object = object_none,
    .time_set = pla_time_set, .message_text = message_none, .object_anim = pla_npc_object_anim,
    .npc_talk = pla_npc_mod_talk, .npc_talked = pla_npc_mod_talked,
    .bubble = bubble_api, .bubble_hide = bubble_hide_api,
    .ui_set = gameui_api_set, .ui_unset = gameui_api_unset, .ui_layouts = gameui_layouts, .ui_panes = gameui_panes,
    .ui_text = gameui_text, .ui_clone = gameui_api_clone,
};
const LarecompModApi *modapi_get(void) { return &g_modapi; }
