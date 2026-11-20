// The world API's parts that are the same in both games (runtime/worldapi.h; the same file in both repos): the player
// held still or walked by a script (input_hold / input_stick: hle/input.c's npad state goes through world_input_filter),
// and the Pokémon in the party and boxes read and written in PKHeX's format (each game's hle/worldapi.c says where a
// slot is: world_pkm_slot). Encryption as PKHeX's PokeCrypto (as hle/pkm_link.c: blocks shuffled by (EC >> 13) & 31,
// then XORed with an LCRNG seeded with the EC; Arceus' blocks are 0x58 bytes, Z-A's 0x50).
#include <math.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "../hle.h"
#include "../rt.h"

// ---- the player held / walked
static _Atomic uint64_t g_hold_buttons;
static _Atomic int g_hold_sticks;
static _Atomic int g_stick_on;
static _Atomic int32_t g_stick_x, g_stick_y;
void world_input_hold(uint64_t buttons, int sticks) {
    g_hold_buttons = buttons & 0xffffull;
    g_hold_sticks = sticks & 3;
    LOG("[world-api] input held: buttons %04llx, sticks %d\n", (unsigned long long)(buttons & 0xffffull), sticks & 3);
}
void world_input_stick(int on, float x, float y) {
    x = x < -1 ? -1 : x > 1 ? 1 : x, y = y < -1 ? -1 : y > 1 ? 1 : y;
    g_stick_x = (int32_t)lrintf(x * 32767.0f), g_stick_y = (int32_t)lrintf(y * 32767.0f);
    g_stick_on = on != 0;
}
void world_input_filter(uint64_t *b, int32_t *lx, int32_t *ly, int32_t *rx, int32_t *ry) {
    uint64_t hb = g_hold_buttons;
    int hs = g_hold_sticks;
    if (hb) *b &= ~hb;
    if (hs & 1) *lx = *ly = 0;
    if (hs & 2) *rx = *ry = 0;
    if (g_stick_on) *lx = g_stick_x, *ly = g_stick_y;
}

// ---- Pokémon (PKHeX's PokeCrypto)
// each game: where box (-1 the party) slot's data is in guest memory, and its format: data (bytes the format has),
// stored (the checksummed part), block (0x50 / 0x58), footer (Z-A's "present" byte after the data: 1, else 0)
typedef struct { uint64_t addr; int data, stored, block, footer; } PkmSlot;
bool world_pkm_slot(int box, int slot, PkmSlot *out);

static const uint8_t k_pos[32 * 4] = {
    0, 1, 2, 3, 0, 1, 3, 2, 0, 2, 1, 3, 0, 3, 1, 2, 0, 2, 3, 1, 0, 3, 2, 1, 1, 0, 2, 3, 1, 0, 3, 2,
    2, 0, 1, 3, 3, 0, 1, 2, 2, 0, 3, 1, 3, 0, 2, 1, 1, 2, 0, 3, 1, 3, 0, 2, 2, 1, 0, 3, 3, 1, 0, 2,
    2, 3, 0, 1, 3, 2, 0, 1, 1, 2, 3, 0, 1, 3, 2, 0, 2, 1, 3, 0, 3, 1, 2, 0, 2, 3, 1, 0, 3, 2, 1, 0,
    0, 1, 2, 3, 0, 1, 3, 2, 0, 2, 1, 3, 0, 3, 1, 2, 0, 2, 3, 1, 0, 3, 2, 1, 1, 0, 2, 3, 1, 0, 3, 2,
};
static const uint8_t k_inv[32] = {0, 1, 2, 4, 3, 5, 6, 7, 12, 18, 13, 19, 8, 10, 14, 20, 16, 22, 9, 11, 15, 21, 17, 23,
                                  0, 1, 2, 4, 3, 5, 6, 7};
static uint32_t u32le(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint16_t u16le(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static void crypt(uint8_t *d, size_t n, uint32_t seed) {
    for (size_t i = 0; i + 1 < n; i += 2) {
        seed = 0x41C64E6Du * seed + 0x6073u;
        d[i] ^= (uint8_t)(seed >> 16), d[i + 1] ^= (uint8_t)(seed >> 24);
    }
}
static void shuffle(uint8_t *d, uint32_t sv, int block) {
    uint8_t tmp[4 * 0x58];
    memcpy(tmp, d, (size_t)block * 4);
    for (int i = 0; i < 4; i++) memcpy(d + i * block, tmp + k_pos[sv * 4 + i] * block, (size_t)block);
}
static void decrypt(const PkmSlot *s, uint8_t *d) {
    uint32_t ec = u32le(d);
    crypt(d + 8, (size_t)s->stored - 8, ec);
    if (s->data > s->stored) crypt(d + s->stored, (size_t)(s->data - s->stored), ec);
    shuffle(d + 8, (ec >> 13) & 31, s->block);
}
static void encrypt(const PkmSlot *s, uint8_t *d) {
    uint32_t ec = u32le(d);
    shuffle(d + 8, k_inv[(ec >> 13) & 31], s->block);
    crypt(d + 8, (size_t)s->stored - 8, ec);
    if (s->data > s->stored) crypt(d + s->stored, (size_t)(s->data - s->stored), ec);
}
static uint16_t checksum(const PkmSlot *s, const uint8_t *d) {
    uint16_t sum = 0;
    for (int i = 8; i < s->stored; i += 2) sum = (uint16_t)(sum + u16le(d + i));
    return sum;
}
static bool readable(uint64_t a, uint64_t n) { return a >= GUEST_LO && n <= GUEST_HI - GUEST_LO && a <= GUEST_HI - n; }

// box -1 the party; the decrypted data (its length), 0: empty, bad, or not there
int world_pkm_read(int box, int slot, uint8_t *out, int cap) {
    PkmSlot s;
    if (!world_pkm_slot(box, slot, &s) || cap < s.data || !readable(s.addr, (uint64_t)s.data + (uint64_t)s.footer)) return 0;
    uint8_t d[0x200];
    memcpy(d, (const void *)(uintptr_t)s.addr, (size_t)s.data);
    if (s.footer && !*(const uint8_t *)(uintptr_t)(s.addr + (uint64_t)s.data)) return 0;  // (Z-A: not present)
    decrypt(&s, d);
    if (!u16le(d + 8) || checksum(&s, d) != u16le(d + 6)) return 0;
    memcpy(out, d, (size_t)s.data);
    return s.data;
}
// the data put there (its checksum set, encrypted); len: the format's data (stored-only for a box slot of Arceus too)
int world_pkm_write(int box, int slot, const uint8_t *in, int len) {
    PkmSlot s;
    if (!world_pkm_slot(box, slot, &s) || len < s.stored || len > s.data || !readable(s.addr, (uint64_t)s.data + (uint64_t)s.footer)) return 0;
    uint8_t d[0x200];
    memcpy(d, (const void *)(uintptr_t)s.addr, (size_t)s.data);  // (what isn't given stays as it was: the party part)
    decrypt(&s, d);
    memcpy(d, in, (size_t)len);
    if (!u16le(d + 8)) return 0;
    uint16_t c = checksum(&s, d);
    d[6] = (uint8_t)c, d[7] = (uint8_t)(c >> 8), d[4] = d[5] = 0;
    encrypt(&s, d);
    pagewatch_prewrite(s.addr, (uint64_t)s.data + (uint64_t)s.footer);
    memcpy((void *)(uintptr_t)s.addr, d, (size_t)s.data);
    if (s.footer) *(uint8_t *)(uintptr_t)(s.addr + (uint64_t)s.data) = 1;
    LOG("[world-api] Pokemon written: %s %d slot %d (species %u)\n", box < 0 ? "party" : "box", box < 0 ? 0 : box + 1, slot + 1, u16le(in + 8));
    return 1;
}
