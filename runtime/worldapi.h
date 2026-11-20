// The world API: the second table of what the game offers scripts (runtime/modapi.h is the first) - networking between
// players, the player held and steered, the party and boxes, battles, objects, traces, weather (Legends Lua's world.lua,
// docs/lua-api.md from "The world"). The same in both games; the runtime implements it per game (runtime/hle/worldapi.c,
// hle/luanet.c the same in both) and hands it to a plugin that exports
//     void larecomp_plugin_worldapi(const LarecompWorldApi *api);
// right after its larecomp_plugin_modapi (runtime/plugin.c). A function a game lacks answers 0 / does nothing.
// Threads: any (what needs the game's thread is queued and done there within a frame or two).
// Positions are the game's world coordinates (metres, y up); yaw is in degrees, 0 facing +z, 90 facing +x.
#pragma once
#include <stdint.h>

#define LARECOMP_WORLDAPI_VERSION 1
#define WORLD_NET_MAX 1024  // (a script message's bytes at most)

typedef struct {
    uint64_t id;        // their id in the shared world
    float pos[3], yaw;  // where they are, as last heard
    int standin;        // 1: their stand-in (the character showing them) is in the world here
    float dist;         // metres from our player (-1: not known)
} LarecompPlayer;

typedef struct {
    uint32_t version;  // LARECOMP_WORLDAPI_VERSION
    const char *game;  // "za" / "pla"

    // ---- networking over the shared world (the online server's relay; tests: LARECOMP_GHOST UDP, Z-A) - hle/luanet.c
    int (*net_up)(void);                                     // 1: the shared world is on (messages go out)
    uint64_t (*net_self)(void);                              // our id there (0 not known)
    int (*net_send)(const void *msg, int len, uint64_t to);  // to 0 = everyone; 1 sent, 0 not (world off, too big, over the cap)
    int (*net_recv)(uint64_t *from, void *msg, int cap);     // the oldest message heard: its length, 0 none
    int (*players)(LarecompPlayer *out, int max);            // the other players heard now
    int (*chat_send)(const char *text);                      // a chat line: shown over our player here, over our stand-in there
    int (*chat_recv)(uint64_t *from, char *text, int cap);   // the oldest chat line heard (already shown): its length, 0 none
    int (*bubble_over)(uint64_t id, const char *text, float seconds);  // a speech bubble over that player's stand-in

    // ---- the player
    // buttons kept from the game (bits as the mod API's buttons: A B X Y LSTICK RSTICK L R ZL ZR PLUS MINUS LEFT UP RIGHT
    // DOWN) and sticks (1 the left, 2 the right) held at rest; scripts still see them (modapi buttons)
    void (*input_hold)(uint64_t buttons, int sticks);
    // the left stick given to the game from now on (x, y -1..1; on 0: the player's own again) - a script walking the player
    void (*input_stick)(int on, float x, float y);
    int (*player_speed)(float k);  // how fast the player moves, times k (1 normal); 0 not in this game

    // ---- Pokémon: the party and the boxes as the game keeps them, in PKHeX's format (Z-A .pa9, Arceus .pa8), decrypted
    // box -1: the party (slot 0-5); 0-31: a box (slot 0-29). pkm_read: its length (0 empty / none); pkm_write: put back
    // encrypted with its checksum set (1 done). pkm_info: the format's sizes (stored, party) and the party's count.
    int (*pkm_read)(int box, int slot, uint8_t *out, int cap);
    int (*pkm_write)(int box, int slot, const uint8_t *in, int len);
    int (*pkm_info)(int *stored, int *party, int *party_count);
    // where party slot's data is, as found through the game's own party (Z-A: its script objects - world.lua); slot -1:
    // addr is the party's count. 1 taken (the data there checks out), 0 not
    int (*pkm_party_at)(int slot, uint64_t addr);
    // a species' growth rate (0 medium fast, 1 erratic, 2 fluctuating, 3 medium slow, 4 fast, 5 slow), read from the
    // game's own personal table (Arceus; Z-A packs its romfs: -1), -1 not known
    int (*pkm_growth)(int species, int form);

    // ---- battles: 0 none, 1 a battle is on; kind: 1 wild, 2 trainer, 0 not known. battle_result: the last one's end
    // (0 none yet, 1 won, 2 lost, 3 ran / caught / other) and a count that changes with each battle that ends
    int (*battle_state)(int *kind);
    int (*battle_result)(uint32_t *count);

    // ---- the weather: set (-1: back to the game's own); get: the game's current one (-1 not known); name: id's name, "" none
    // (traces, objects, the camera: the game's own script commands - world.lua through game_script)
    int (*weather_set)(int id);
    int (*weather_get)(void);
    int (*weather_name)(int id, char *out, int cap);
} LarecompWorldApi;

// ---- runtime side
const LarecompWorldApi *worldapi_get(void);  // (runtime/hle/worldapi.c)
