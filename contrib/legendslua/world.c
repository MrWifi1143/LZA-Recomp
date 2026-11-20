// Legends Lua: the world side of the mod API (docs/lua-api.md from "The world"; world.lua builds it on these): the
// game's runtime/worldapi.h - networking between players and chat, the player held and walked, the party and boxes,
// battles, objects, traces, weather. Included by legendslua.c (it uses its A / M / helpers); world.lua runs right after
// api.lua in each fresh state, given these primitives and api.lua's own parts (__world_parts).
#include "worldapi.h"

static const LarecompWorldApi *W;  // (NULL: a game build without the world API)

#ifdef _WIN32
__declspec(dllexport)
#else
__attribute__((visibility("default")))
#endif
void larecomp_plugin_worldapi(const LarecompWorldApi *api) {
    if (api && api->version >= 1) W = api;
}

static void need_w(lua_State *s) {
    if (!W) luaL_error(s, "this game build has no world API (net, party, battle...): update the game");
}
// ---- networking
static int w_net_up(lua_State *s) { lua_pushboolean(s, W && W->net_up()); return 1; }
static int w_net_self(lua_State *s) {
    char t[24];
    snprintf(t, sizeof t, "%016" PRIx64, W ? W->net_self() : (uint64_t)0);
    lua_pushstring(s, t);
    return 1;
}
static uint64_t id_of(lua_State *s, int i) {  // (a player's id: its 16 hex digits, or nil / "" for everyone)
    if (lua_isnoneornil(s, i)) return 0;
    const char *t = luaL_checkstring(s, i);
    return t[0] ? strtoull(t, NULL, 16) : 0;
}
static int w_net_send(lua_State *s) {  // (bytes, to) -> sent
    need_w(s);
    size_t n;
    const char *d = luaL_checklstring(s, 1, &n);
    lua_pushboolean(s, n <= WORLD_NET_MAX && W->net_send(d, (int)n, id_of(s, 2)));
    return 1;
}
static int w_net_recv(lua_State *s) {  // -> bytes, from (nil: none)
    if (!W) return 0;
    static char b[WORLD_NET_MAX];
    uint64_t from = 0;
    int n = W->net_recv(&from, b, (int)sizeof b);
    if (n <= 0) return 0;
    char t[24];
    snprintf(t, sizeof t, "%016" PRIx64, from);
    lua_pushlstring(s, b, (size_t)n);
    lua_pushstring(s, t);
    return 2;
}
static int w_players(lua_State *s) {  // -> {{id, x, y, z, yaw, standin, dist}, ...}
    lua_newtable(s);
    if (!W) return 1;
    LarecompPlayer p[32];
    int n = W->players(p, 32);
    for (int i = 0; i < n; i++) {
        char t[24];
        snprintf(t, sizeof t, "%016" PRIx64, p[i].id);
        lua_createtable(s, 0, 7);
        lua_pushstring(s, t), lua_setfield(s, -2, "id");
        lua_pushnumber(s, p[i].pos[0]), lua_setfield(s, -2, "x");
        lua_pushnumber(s, p[i].pos[1]), lua_setfield(s, -2, "y");
        lua_pushnumber(s, p[i].pos[2]), lua_setfield(s, -2, "z");
        lua_pushnumber(s, p[i].yaw), lua_setfield(s, -2, "yaw");
        lua_pushboolean(s, p[i].standin), lua_setfield(s, -2, "standin");
        lua_pushnumber(s, p[i].dist), lua_setfield(s, -2, "dist");
        lua_rawseti(s, -2, i + 1);
    }
    return 1;
}
static int w_chat_send(lua_State *s) { need_w(s); lua_pushboolean(s, W->chat_send(luaL_checkstring(s, 1))); return 1; }
static int w_chat_recv(lua_State *s) {
    if (!W) return 0;
    char text[512], t[24];
    uint64_t from = 0;
    if (W->chat_recv(&from, text, (int)sizeof text) <= 0) return 0;
    snprintf(t, sizeof t, "%016" PRIx64, from);
    lua_pushstring(s, text), lua_pushstring(s, t);
    return 2;
}
static int w_bubble_over(lua_State *s) {
    need_w(s);
    lua_pushboolean(s, W->bubble_over(id_of(s, 1), luaL_checkstring(s, 2), (float)luaL_optnumber(s, 3, 0)));
    return 1;
}
// ---- the player
static int w_input_hold(lua_State *s) {
    need_w(s);
    W->input_hold((uint64_t)luaL_checkinteger(s, 1), (int)luaL_optinteger(s, 2, 0));
    return 0;
}
static int w_input_stick(lua_State *s) {
    need_w(s);
    W->input_stick(lua_toboolean(s, 1), (float)luaL_optnumber(s, 2, 0), (float)luaL_optnumber(s, 3, 0));
    return 0;
}
static int w_speed(lua_State *s) { need_w(s); lua_pushboolean(s, W->player_speed((float)luaL_checknumber(s, 1))); return 1; }
// ---- Pokémon: (box, slot) -> the decrypted bytes (nil: empty / none); box -1 the party
static int w_pkm_read(lua_State *s) {
    need_w(s);
    uint8_t b[0x200];
    int n = W->pkm_read((int)luaL_checkinteger(s, 1), (int)luaL_checkinteger(s, 2), b, (int)sizeof b);
    if (n <= 0) return 0;
    lua_pushlstring(s, (const char *)b, (size_t)n);
    return 1;
}
static int w_pkm_write(lua_State *s) {
    need_w(s);
    size_t n;
    const char *d = luaL_checklstring(s, 3, &n);
    lua_pushboolean(s, n <= 0x200 && W->pkm_write((int)luaL_checkinteger(s, 1), (int)luaL_checkinteger(s, 2), (const uint8_t *)d, (int)n));
    return 1;
}
static int w_pkm_info(lua_State *s) {  // -> ok, stored size, party size, party count
    int st = 0, pa = 0, c = 0;
    int ok = W ? W->pkm_info(&st, &pa, &c) : 0;
    lua_pushboolean(s, ok), lua_pushinteger(s, st), lua_pushinteger(s, pa), lua_pushinteger(s, c);
    return 4;
}
static int w_party_at(lua_State *s) {  // (slot 0-5 | -1 count, address)
    need_w(s);
    lua_pushboolean(s, W->pkm_party_at((int)luaL_checkinteger(s, 1), (uint64_t)luaL_checkinteger(s, 2)));
    return 1;
}
// ---- battles
static int w_battle(lua_State *s) {
    int kind = 0, on = W ? W->battle_state(&kind) : 0;
    uint32_t count = 0;
    int res = W ? W->battle_result(&count) : 0;
    lua_pushinteger(s, on), lua_pushinteger(s, kind), lua_pushinteger(s, res), lua_pushinteger(s, (lua_Integer)count);
    return 4;
}
// ---- the weather
static int w_weather(lua_State *s) {  // weather([id]) -> current id (set: done)
    need_w(s);
    if (!lua_isnoneornil(s, 1)) { lua_pushboolean(s, W->weather_set((int)luaL_checkinteger(s, 1))); return 1; }
    lua_pushinteger(s, W->weather_get());
    return 1;
}
static int w_weather_name(lua_State *s) {
    need_w(s);
    char n[64] = "";
    W->weather_name((int)luaL_checkinteger(s, 1), n, (int)sizeof n);
    lua_pushstring(s, n);
    return 1;
}

static const char k_world_lua[] = {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wc23-extensions"
#embed "world.lua"
#pragma clang diagnostic pop
    , 0};

// world.lua run in the fresh state (after api.lua): given the primitives (wt) and api.lua's own parts (G.__world_parts)
static void world_open(lua_State *s) {
    static const luaL_Reg wm[] = {{"net_up", w_net_up}, {"net_self", w_net_self}, {"net_send", w_net_send}, {"net_recv", w_net_recv},
                                  {"players", w_players}, {"chat_send", w_chat_send}, {"chat_recv", w_chat_recv},
                                  {"bubble_over", w_bubble_over}, {"input_hold", w_input_hold}, {"input_stick", w_input_stick},
                                  {"speed", w_speed}, {"pkm_read", w_pkm_read}, {"pkm_write", w_pkm_write}, {"pkm_info", w_pkm_info}, {"party_at", w_party_at},
                                  {"battle", w_battle}, {"weather", w_weather},
                                  {"weather_name", w_weather_name}, {NULL, NULL}};
    if (luaL_loadbufferx(s, k_world_lua, strlen(k_world_lua), "=world.lua", "t")) {
        A->log("[lua] world.lua: %s\n", lua_tostring(s, -1));
        lua_pop(s, 1);
        return;
    }
    luaL_newlib(s, wm);
    lua_pushboolean(s, W != NULL), lua_setfield(s, -2, "have");
    lua_pushinteger(s, W ? (lua_Integer)W->version : 0), lua_setfield(s, -2, "version");
    if (pcall_traced(s, 1, 0)) A->log("[lua] world.lua: %s\n", lua_tostring(s, -1)), lua_pop(s, 1);
}
