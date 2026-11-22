// Legends Lua: the quest library (lib/quest.lua - docs/lua-api.md "Quests"), built in and run after the rest of the
// API with the same rt table: every mod gets `quest`. #included by legendslua.c (after world.c).
static const char k_quest_lua[] = {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wc23-extensions"
#embed "lib/quest.lua"
#pragma clang diagnostic pop
    , 0};
static void quest_open(lua_State *s) {  // (the rt table at the top of the stack stays)
    // (LEGENDSLUA_QUEST_LIB=<file>: that file instead of the built-in copy - working on the library, read again at
    // every reload of the scripts)
    const char *dev = getenv("LEGENDSLUA_QUEST_LIB");
    if (dev && *dev) {
        if (luaL_loadfilex(s, dev, "t")) {
            A->log("[lua] %s: %s\n", dev, lua_tostring(s, -1));
            lua_pop(s, 1);
            return;
        }
    } else if (luaL_loadbufferx(s, k_quest_lua, strlen(k_quest_lua), "=lib/quest.lua", "t")) {
        A->log("[lua] lib/quest.lua: %s\n", lua_tostring(s, -1));
        lua_pop(s, 1);
        return;
    }
    lua_pushvalue(s, -2);
    if (pcall_traced(s, 1, 0)) A->log("[lua] lib/quest.lua: %s\n", lua_tostring(s, -1)), lua_pop(s, 1);
}
