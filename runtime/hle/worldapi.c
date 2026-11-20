// The world API (runtime/worldapi.h) for Legends: Z-A: what scripts (contrib/legendslua world.lua, docs/lua-api.md from
// "The world") get of the game beyond the mod API. Networking: hle/luanet.c over the shared world (hle/warp.c); the
// player held: hle/worldcommon.c (input); the Pokémon: hle/worldcommon.c with the slots found here.
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../hle.h"
#include "../worldapi.h"

int luanet_up(void);
uint64_t luanet_self(void);
int luanet_send(const void *msg, int len, uint64_t to);
int luanet_recv(uint64_t *from, void *msg, int cap);
int luanet_players(LarecompPlayer *out, int max);
int luanet_chat_send(const char *text);
int luanet_chat_recv(uint64_t *from, char *text, int cap);
int luanet_bubble_over(uint64_t id, const char *text, float sec);
void world_input_hold(uint64_t buttons, int sticks);
void world_input_stick(int on, float x, float y);
int world_pkm_read(int box, int slot, uint8_t *out, int cap);
int world_pkm_write(int box, int slot, const uint8_t *in, int len);

// ---- where the Pokémon are. Boxes: LiveHeX's ZA_v202 pointer [[[main+610A710]+B0]+978] = box 1 slot 1; a slot is
// 0x198 bytes: the party format (0x158, PKHeX's .pa9 - the boxes keep the party stats) + a 0x40 footer, its first byte
// "present". The party: below
typedef struct { uint64_t addr; int data, stored, block, footer; } PkmSlot;
static uint64_t rd64(uint64_t a) {
    uint64_t v = 0;
    if (a >= GUEST_LO && a + 8 <= GUEST_HI) memcpy(&v, (const void *)(uintptr_t)a, 8);
    return v;
}
static uint64_t box1(void) {
    uint64_t a = rd64(MAIN_BASE + 0x610A710);
    a = a ? rd64(a + 0xB0) : 0;
    return a ? rd64(a + 0x978) : 0;
}
// The party: each member's data is its own (0x180 apart as it happens): found through the game's own party objects
// (the framework's Player -> party -> member userdata: [ud] -> [+8] -> [+0x50] -> [+0x30] = the member's PA9 data,
// 2026-11-19), which world.lua asks the game's Lua for and hands here (pkm_party_at) - checked before it is taken
static uint64_t g_party[6];
static int g_party_n;
static bool pkm_ok_at(uint64_t a);
static int party_at(int slot, uint64_t addr) {
    if (slot == -1) { g_party_n = addr <= 6 ? (int)addr : 0; return 1; }
    if (slot < 0 || slot >= 6) return 0;
    if (addr && !pkm_ok_at(addr)) return 0;
    g_party[slot] = addr;
    return 1;
}
bool world_pkm_slot(int box, int slot, PkmSlot *out) {
    out->data = 0x158, out->stored = 0x148, out->block = 0x50, out->footer = 1;
    if (box < 0) {
        if (slot < 0 || slot >= g_party_n || !g_party[slot]) return false;
        out->addr = g_party[slot], out->footer = 0, out->data = 0x148;  // (the stored part only - level and stats are the
        return true;                                                    //  game's own; no "present" byte: the count says)
    }
    if (box >= 32 || slot < 0 || slot >= 30) return false;
    uint64_t b = box1();
    if (!b) return false;
    out->addr = b + (uint64_t)(box * 30 + slot) * 0x198;
    return true;
}
static int pkm_info(int *stored, int *party, int *count) {
    if (stored) *stored = 0x148;
    if (party) *party = 0x158;
    if (count) *count = g_party_n;
    return box1() != 0;
}

void za_bubble_cam_on(void);  // (hle/za_fixes.c)
void world_bubbles_on(void) { za_bubble_cam_on(); }  // (a chat line's bubble: the scene camera followed)

int world_pkm_read(int box, int slot, uint8_t *out, int cap);
static bool pkm_ok_at(uint64_t a) {  // (a PA9 there: its checksum checks out - world_pkm_read through a slot of its own)
    uint64_t was = g_party[5];
    int wn = g_party_n;
    g_party[5] = a, g_party_n = 6;
    uint8_t d[0x200];
    bool ok = world_pkm_read(-1, 5, d, (int)sizeof d) > 0;
    g_party[5] = was, g_party_n = wn;
    return ok;
}

// ---- not in this game yet (0)
static int speed_none(float k) { (void)k; return 0; }
static int battle_none(int *kind) { if (kind) *kind = 0; return 0; }
static int result_none(uint32_t *count) { if (count) *count = 0; return 0; }
static int weather_set_none(int id) { (void)id; return 0; }
static int weather_get_none(void) { return -1; }
static int weather_name_none(int id, char *out, int cap) { (void)id; if (cap > 0) out[0] = 0; return 0; }

static const LarecompWorldApi g_api = {
    .version = LARECOMP_WORLDAPI_VERSION, .game = "za",
    .net_up = luanet_up, .net_self = luanet_self, .net_send = luanet_send, .net_recv = luanet_recv, .players = luanet_players,
    .chat_send = luanet_chat_send, .chat_recv = luanet_chat_recv, .bubble_over = luanet_bubble_over,
    .input_hold = world_input_hold, .input_stick = world_input_stick, .player_speed = speed_none,
    .pkm_read = world_pkm_read, .pkm_write = world_pkm_write, .pkm_info = pkm_info, .pkm_party_at = party_at,
    .battle_state = battle_none, .battle_result = result_none,
    .weather_set = weather_set_none, .weather_get = weather_get_none, .weather_name = weather_name_none,
};
const LarecompWorldApi *worldapi_get(void) { return &g_api; }
