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
#include "../platform.h"
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
static void pk_crypt(uint8_t *d, size_t n, uint32_t seed) {
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
    pk_crypt(d + 8, (size_t)s->stored - 8, ec);
    if (s->data > s->stored) pk_crypt(d + s->stored, (size_t)(s->data - s->stored), ec);
    shuffle(d + 8, (ec >> 13) & 31, s->block);
}
static void encrypt(const PkmSlot *s, uint8_t *d) {
    uint32_t ec = u32le(d);
    shuffle(d + 8, k_inv[(ec >> 13) & 31], s->block);
    pk_crypt(d + 8, (size_t)s->stored - 8, ec);
    if (s->data > s->stored) pk_crypt(d + s->stored, (size_t)(s->data - s->stored), ec);
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

// ---- battles: a function of the game's battle that runs once a frame for as long as a battle is on - its menus too
// (each game's hle/worldapi.c names it: found by profiling the game's functions in and out of a battle, fnprof live
// windows) - hooked when a script first asks; the battle is on while it ran in the last half second, and each time it
// stops, one more battle has ended
void rt_register_fnhook_at(uint64_t addr, int (*fn)(Cpu *c, uint64_t addr));  // runtime/fnhook.c
static _Atomic uint64_t g_btl_last;
static uint32_t g_btl_ends;
static int g_btl_on;
static int battle_frame_hook(Cpu *c, uint64_t addr) { (void)c, (void)addr; g_btl_last = plat_now_ns(); return 0; }
int world_battle_state_at(uint64_t fn, int *kind) {
    static int hooked;
    if (!hooked && fn) hooked = 1, rt_register_fnhook_at(fn, battle_frame_hook), LOG("[world-api] battles followed (%llx)\n", (unsigned long long)fn);
    uint64_t last = g_btl_last, now = plat_now_ns();
    int on = last && now - last < 500000000ull;
    if (on != g_btl_on) {
        if (!on) g_btl_ends++;
        LOG("[world-api] battle %s\n", on ? "started" : "ended");
        g_btl_on = on;
    }
    if (kind) *kind = 0;
    return on;
}
int world_battle_ends(uint32_t *count) { if (count) *count = g_btl_ends; return 0; }

// ---- a species' growth rate (0 medium fast, 1 erratic, 2 fluctuating, 3 medium slow, 4 fast, 5 slow) from the game's
// own personal table in its romfs (Arceus: bin/pml/personal/personal_data_total.perbin, a FlatBuffer: the root's field
// 0 a vector of tables {0 species u16, 1 form u16, ..., 15 growth rate u8} - Legends Studio's arceus_tables.cpp), read
// once when first asked. -1: not known (no such file - Z-A packs its romfs - or no such species)
#include <stdio.h>
#include <stdlib.h>
#ifndef _WIN32
#include <unistd.h>
#endif
bool romfs_image_file(const char *path, int *fd, uint64_t *off, uint64_t *size);  // runtime/services/fs.c
static uint8_t *romfs_whole(const char *path, size_t *n) {
    char loose[1200];
    snprintf(loose, sizeof loose, "%s/%s", g_romfs_dir, path);
    FILE *f = fopen(loose, "rb");
    if (f) {
        fseek(f, 0, SEEK_END);
        long sz = ftell(f);
        fseek(f, 0, SEEK_SET);
        uint8_t *d = sz > 0 ? malloc((size_t)sz) : NULL;
        bool ok = d && fread(d, 1, (size_t)sz, f) == (size_t)sz;
        fclose(f);
        if (!ok) { free(d); return NULL; }
        *n = (size_t)sz;
        return d;
    }
    int fd;
    uint64_t off, size;
    if (!romfs_image_file(path, &fd, &off, &size) || !size) return NULL;
    uint8_t *d = malloc(size);
#ifdef _WIN32
    bool ok = d && _lseeki64(fd, (__int64)off, SEEK_SET) >= 0 && _read(fd, d, (unsigned)size) == (int)size;
#else
    bool ok = d && pread(fd, d, (size_t)size, (off_t)off) == (ssize_t)size;
#endif
    if (!ok) { free(d); return NULL; }
    *n = (size_t)size;
    return d;
}
// (a FlatBuffer table's field: its position in the buffer, 0 when absent)
static size_t fb_field(const uint8_t *b, size_t n, size_t tbl, int field) {
    if (tbl + 4 > n) return 0;
    int32_t so;
    memcpy(&so, b + tbl, 4);
    size_t vt = (size_t)((int64_t)tbl - so);
    if (vt + 4 > n) return 0;
    uint16_t vsz, fo;
    memcpy(&vsz, b + vt, 2);
    if ((size_t)(4 + field * 2 + 2) > vsz || vt + 4 + (size_t)field * 2 + 2 > n) return 0;
    memcpy(&fo, b + vt + 4 + field * 2, 2);
    return fo ? tbl + fo : 0;
}
static uint8_t g_growth[1100][32];  // (species, form -> growth + 1; 0 unknown)
static int g_growth_state;          // 0 not read, 1 read, -1 not there
int world_growth(const char *path, int species, int form) {
    if (!g_growth_state) {
        g_growth_state = -1;
        size_t n = 0;
        uint8_t *b = path ? romfs_whole(path, &n) : NULL;
        uint32_t root = 0, cnt = 0;
        if (b && n > 8) memcpy(&root, b, 4);
        size_t vf = b && root < n ? fb_field(b, n, root, 0) : 0;
        if (vf && vf + 4 <= n) {
            uint32_t vo;
            memcpy(&vo, b + vf, 4);
            size_t v = vf + vo;
            if (v + 4 <= n) memcpy(&cnt, b + v, 4);
            int got = 0;
            for (uint32_t i = 0; i < cnt && v + 4 + (size_t)i * 4 + 4 <= n; i++) {
                uint32_t eo;
                memcpy(&eo, b + v + 4 + (size_t)i * 4, 4);
                size_t e = v + 4 + (size_t)i * 4 + eo;
                size_t fs = fb_field(b, n, e, 0), ff = fb_field(b, n, e, 1), fg = fb_field(b, n, e, 15);
                uint16_t sp = 0, fm = 0;
                if (fs && fs + 2 <= n) memcpy(&sp, b + fs, 2);
                if (ff && ff + 2 <= n) memcpy(&fm, b + ff, 2);
                uint8_t g = fg && fg < n ? b[fg] : 0;  // (absent: 0, the default - medium fast)
                if (sp < 1100 && fm < 32 && g < 6) g_growth[sp][fm] = (uint8_t)(g + 1), got++;
            }
            if (got) g_growth_state = 1;
            LOG("[world-api] growth rates of %d species / forms read from %s\n", got, path);
        }
        free(b);
    }
    if (g_growth_state < 0 || species < 0 || species >= 1100) return -1;
    int f = form >= 0 && form < 32 && g_growth[species][form] ? form : 0;
    return g_growth[species][f] ? g_growth[species][f] - 1 : -1;
}
