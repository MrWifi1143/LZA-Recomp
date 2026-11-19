// The mod API: what the game itself offers scripts (Legends Lua's npc / player / ui / events - contrib/legendslua), the
// same in both games. The runtime implements it per game (runtime/hle/modapi.c) and hands it to a plugin that exports
//     void larecomp_plugin_modapi(const LarecompModApi *api);
// right after that plugin's larecomp_plugin_init (runtime/plugin.c). Nothing here runs until a plugin calls enable():
// a game with no scripts pays nothing.
//
// Threads: every function may be called from any thread. What needs the game's own thread (making a character, moving
// one, the player put somewhere) is queued and done there within a frame or two.
// Positions are the game's world coordinates (metres, y up); yaw is in degrees, 0 facing +z, 90 facing +x.
#pragma once
#include <stdint.h>

#define LARECOMP_MODAPI_VERSION 1

// npc_state answers
enum {
    MODNPC_NONE = 0,     // no such handle (removed, or never made)
    MODNPC_WAITING = 1,  // not in the world: its area isn't loaded or the player is far (made again once both are right)
    MODNPC_COMING = 2,   // asked for: the game is making it
    MODNPC_UP = 3,       // in the world, standing
    MODNPC_WALKING = 4,  // in the world, on its way somewhere (npc_walk_to)
};

typedef struct {
    uint32_t version;  // LARECOMP_MODAPI_VERSION
    const char *game;  // "za" (Legends: Z-A) or "pla" (Legends: Arceus)
    // The hooks the API needs switched on (the game's characters and the player followed). Call once, before the game's
    // first frame if possible (the level's own people are noted as it loads - what new ones are made like).
    void (*enable)(void);

    // ---- characters of a script's own, made by the game
    // A new one wanted at pos, facing yaw. model: what it looks like - Z-A: part of an NPC data name the level has
    // ("npc_A04_z02_02_0002", "A04"; NULL / "" the nearest townsperson's); Arceus: a character id ("tr0036_00_senpai_m"),
    // a hash ("0x2d992cf818c93910") or a placement name of the level ("area00_npc_g_002": that one's look). label: the
    // script's name for it (logs). Answers its handle (> 0), 0 none free. It is made when its spot is near the player
    // and its area is loaded, and made again after a trip away (MODNPC_WAITING meanwhile).
    int (*npc_spawn)(const char *model, const float pos[3], float yaw, const char *label);
    void (*npc_remove)(int h);                                 // gone for good (the game lets it go)
    void (*npc_remove_all)(void);                              // every one (a script reload)
    void (*npc_walk_to)(int h, const float pos[3], int run);   // walks (run: runs) there, on the ground
    void (*npc_stop)(int h);                                   // stands where it is
    void (*npc_set_pos)(int h, const float pos[3], float yaw); // put there at once
    // state (MODNPC_*); pos / yaw (may be NULL): where it is now (or where it will be made)
    int (*npc_state)(int h, float pos[3], float *yaw);
    // the looks one can be made with now (one per line, "name - what"), into out; answers the length
    int (*npc_models)(char *out, int cap);

    // ---- the player
    int (*player_pos)(float pos[3], float *yaw);  // 0: not known yet (no level)
    void (*player_teleport)(const float pos[3]);

    // ---- the world
    // A number that changes when an area (Arceus: a level's people; Z-A: a part of the city, a building) has loaded,
    // and its name when known ("" not): scripts re-check what they made then.
    uint64_t (*area)(char *name, int cap);
    uint64_t (*buttons)(void);  // the Switch buttons held (bit = A B X Y LSTICK RSTICK L R ZL ZR PLUS MINUS LEFT UP RIGHT DOWN)
    void (*notify)(const char *text, unsigned ms);  // a notice on screen
    // the folders (relative to the game folder) of the mods that are on and carry scripts: mods/<name>/lua
    int (*mod_lua_dirs)(char (*dirs)[256], int max);
    double (*now)(void);  // seconds, a steady clock
} LarecompModApi;

// ---- runtime side
const LarecompModApi *modapi_get(void);  // (runtime/hle/modapi.c)
