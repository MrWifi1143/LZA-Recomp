// The world API (runtime/worldapi.h) for Legends: Arceus: what scripts (contrib/legendslua world.lua, docs/lua-api.md from
// "The world") get of the game beyond the mod API. Networking: hle/luanet.c over the shared world (hle/pla_world.c); the
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

// ---- where the Pokémon are. Boxes: LiveHeX's LA_v111 pointer [[main+42BA6B0]+1F0]+68 = box 1 slot 1; a slot is 0x168
// bytes: the stored format (PKHeX's .pa8 without its party part). The party: (below, party_base)
typedef struct { uint64_t addr; int data, stored, block, footer; } PkmSlot;
static uint64_t rd64(uint64_t a) {
    uint64_t v = 0;
    if (a >= GUEST_LO && a + 8 <= GUEST_HI) memcpy(&v, (const void *)(uintptr_t)a, 8);
    return v;
}
static uint64_t box1(void) {
    uint64_t a = rd64(MAIN_BASE + 0x42BA6B0);
    a = a ? rd64(a + 0x1F0) : 0;
    return a ? a + 0x68 : 0;
}
// The party: [[main+42BA6B0]+200]+68 (the box's neighbour block: found 2026-11-19 by decrypting what the save object's
// blocks point at), 6 slots of 0x178 - the party format (PKHeX's .pa8: level and stats at +0x168). Its count: the slots
// in a row that hold one.
int world_pkm_read(int box, int slot, uint8_t *out, int cap);
static uint64_t party_base(int *count) {
    uint64_t a = rd64(MAIN_BASE + 0x42BA6B0);
    a = a ? rd64(a + 0x200) : 0;
    if (!a) return 0;
    if (count) {
        static int busy;
        *count = 0;
        if (!busy) {
            busy = 1;
            uint8_t d[0x200];
            for (int i = 0; i < 6 && world_pkm_read(-1, i, d, (int)sizeof d) > 0; i++) (*count)++;
            busy = 0;
        }
    }
    return a + 0x68;
}
static int party_at(int slot, uint64_t addr) { (void)slot, (void)addr; return 0; }  // (Arceus: its party is found here, not by scripts)
bool world_pkm_slot(int box, int slot, PkmSlot *out) {
    out->data = 0x168, out->stored = 0x168, out->block = 0x58, out->footer = 0;
    if (box < 0) {
        int n = 0;
        uint64_t p = party_base(&n);
        if (!p || slot < 0 || slot >= 6) return false;
        out->addr = p + (uint64_t)slot * 0x178, out->data = 0x178;
        return true;
    }
    if (box >= 32 || slot < 0 || slot >= 30) return false;
    uint64_t b = box1();
    if (!b) return false;
    out->addr = b + (uint64_t)(box * 30 + slot) * 0x168;
    return true;
}
static int pkm_info(int *stored, int *party, int *count) {
    int n = 0;
    party_base(&n);
    if (stored) *stored = 0x168;
    if (party) *party = 0x178;
    if (count) *count = n;
    return box1() != 0;
}

void world_bubbles_on(void) {}  // (the render camera's hook is on with the shared world: hle/warp.c cam_hook)

// ---- not in this game yet (0)
static int speed_none(float k) { (void)k; return 0; }
static int battle_none(int *kind) { if (kind) *kind = 0; return 0; }
static int result_none(uint32_t *count) { if (count) *count = 0; return 0; }
static int weather_set_none(int id) { (void)id; return 0; }
static int weather_get_none(void) { return -1; }
static int weather_name_none(int id, char *out, int cap) { (void)id; if (cap > 0) out[0] = 0; return 0; }

static const LarecompWorldApi g_api = {
    .version = LARECOMP_WORLDAPI_VERSION, .game = "pla",
    .net_up = luanet_up, .net_self = luanet_self, .net_send = luanet_send, .net_recv = luanet_recv, .players = luanet_players,
    .chat_send = luanet_chat_send, .chat_recv = luanet_chat_recv, .bubble_over = luanet_bubble_over,
    .input_hold = world_input_hold, .input_stick = world_input_stick, .player_speed = speed_none,
    .pkm_read = world_pkm_read, .pkm_write = world_pkm_write, .pkm_info = pkm_info, .pkm_party_at = party_at,
    .battle_state = battle_none, .battle_result = result_none,
    .weather_set = weather_set_none, .weather_get = weather_get_none, .weather_name = weather_name_none,
};
const LarecompWorldApi *worldapi_get(void) { return &g_api; }
