// PKHeX support (both games; the same file in both repos).
//
// PKHeX Link (Settings > Gameplay, ini pkhex_link: 0 off, 1 this PC, 2 the local network): the text protocol of
// sys-botbase, the Switch homebrew PKHeX's LiveHeX plugin (PKHeX-Plugins) talks to, on TCP port 6000. LiveHeX asks for
// the botbase version, the title id and the game's version, then follows its pointers from the main module (peekMain /
// peekAbsolute) to the boxes and reads / writes them (peek / poke, relative to the heap base). The recomp's guest memory
// is the game's own address space, so the same pointers work; only addresses inside the guest window are answered and
// nothing is written over the modules' code or read-only data.
//
// Pokémon Files (Settings > Gameplay): Export writes every Pokémon in the boxes as a PKHeX file (Z-A .pa9, Arceus .pa8,
// decrypted, PKHeX's own format) to <data>/pokemon/export/<date>/; Import puts the files in <data>/pokemon/import/ into
// the first empty box slots and moves each one placed to import/done/. Box 1 slot 1 is found with LiveHeX's pointer for
// that game version; slots are stored encrypted as on the Switch (PKHeX's PokeCrypto: blocks shuffled by
// (EC >> 13) & 31, then XORed with an LCRNG seeded with the EC). Z-A keeps the party format (0x158) in boxes plus a
// 0x40-byte footer whose first byte says "present".
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <pthread.h>
#include <sys/stat.h>
#include <time.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET sock_t;
#define sock_close closesocket
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
typedef int sock_t;
#define INVALID_SOCKET (-1)
#define sock_close close
#endif

#include "../hle.h"
#include "../paths.h"
#include "../rt.h"
#include "../settings.h"

uint64_t game_title_id(void);  // runtime/gamesetup.c
extern Module g_modules[4];
extern int g_nmodules;
void menu_notice(const char *text);  // runtime/ui/menu.mm

// ------------------------------------------------------------------ the two games

typedef struct {
    uint64_t title;
    const char *name, *version, *ext;
    uint64_t chain[3];  // box 1 slot 1: [[main + chain[0]] + chain[1]] ... (nchain reads), then + add
    int nchain;
    uint64_t add;
    int slot, data, stored;  // a box slot's size in memory, the part PKHeX's file holds, the stored (checksummed) part
    int nick;                // the nickname's offset (UTF-16, 13 units)
    bool shuffle8a;          // Arceus' 0x58-byte blocks (else 0x50)
} PkmGame;

static const PkmGame k_games[] = {
    // Z-A 2.0.2: LiveHeX's ZA_v202 "[[[main+610A710]+B0]+978]"; PA9 = party format, 0x40 footer (first byte: present)
    {0x0100F43008C44000ull, "Pokémon Legends: Z-A", "2.0.2", "pa9", {0x610A710, 0xB0, 0x978}, 3, 0, 0x198, 0x158, 0x148, 0x58, false},
    // Arceus 1.1.1: LA_v111 "[[main+42BA6B0]+1F0]+68"; PA8 stored format
    {0x01001F5010DFA000ull, "Pokémon Legends: Arceus", "1.1.1", "pa8", {0x42BA6B0, 0x1F0}, 2, 0x68, 0x168, 0x168, 0x168, 0x60, true},
};
enum { BOXES = 32, SLOTS = 30 };

static const PkmGame *game(void) {
    uint64_t t = game_title_id();
    for (size_t i = 0; i < sizeof k_games / sizeof *k_games; i++)
        if (k_games[i].title == t) return &k_games[i];
    return NULL;
}

// ------------------------------------------------------------------ guest memory, checked

static bool running(void) { return g_nmodules > 0; }
static bool readable(uint64_t a, uint64_t n) { return n <= GUEST_HI - GUEST_LO && a >= GUEST_LO && a <= GUEST_HI - n; }
static bool writable(uint64_t a, uint64_t n) {
    if (!readable(a, n)) return false;
    for (int i = 0; i < g_nmodules; i++) {  // (not the code or read-only data)
        uint64_t lo = g_modules[i].base + g_modules[i].text_off, hi = g_modules[i].base + g_modules[i].ro_off + g_modules[i].ro_size;
        if (a < hi && a + n > lo) return false;
    }
    return true;
}
static uint64_t rd64(uint64_t a) {
    uint64_t v = 0;
    if (readable(a, 8)) memcpy(&v, (const void *)(uintptr_t)a, 8);
    return v;
}
static bool guest_write(uint64_t a, const void *p, uint64_t n) {
    if (!writable(a, n)) return false;
    pagewatch_prewrite(a, n);
    memcpy((void *)(uintptr_t)a, p, n);
    return true;
}

static uint64_t box1(const PkmGame *g) {
    if (!running()) return 0;
    uint64_t a = MAIN_BASE + g->chain[0];
    for (int i = 0; i < g->nchain; i++) {
        a = rd64(i ? a + g->chain[i] : a);
        if (!a) return 0;
    }
    return a + g->add;
}

// ------------------------------------------------------------------ PKHeX's PokeCrypto (Gen 8 / 8a)

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
static void shuffle(uint8_t *d, uint32_t sv, int block) {  // block i <- block k_pos[sv * 4 + i]
    uint8_t tmp[4 * 0x58];
    memcpy(tmp, d, (size_t)block * 4);
    for (int i = 0; i < 4; i++) memcpy(d + i * block, tmp + k_pos[sv * 4 + i] * block, (size_t)block);
}
static void pk_decrypt(const PkmGame *g, uint8_t *d) {
    uint32_t pv = u32le(d);
    pk_crypt(d + 8, (size_t)g->stored - 8, pv);
    if (g->data > g->stored) pk_crypt(d + g->stored, (size_t)(g->data - g->stored), pv);
    shuffle(d + 8, (pv >> 13) & 31, g->shuffle8a ? 0x58 : 0x50);
}
static void pk_encrypt(const PkmGame *g, uint8_t *d) {
    uint32_t pv = u32le(d);
    shuffle(d + 8, k_inv[(pv >> 13) & 31], g->shuffle8a ? 0x58 : 0x50);
    pk_crypt(d + 8, (size_t)g->stored - 8, pv);
    if (g->data > g->stored) pk_crypt(d + g->stored, (size_t)(g->data - g->stored), pv);
}
static bool pk_checksum_ok(const PkmGame *g, const uint8_t *d) {
    uint16_t sum = 0;
    for (int i = 8; i < g->stored; i += 2) sum = (uint16_t)(sum + u16le(d + i));
    return sum == u16le(d + 6) && u16le(d + 4) == 0;  // (and Sanity 0, as PKHeX's Valid)
}
static bool pk_encrypted(const PkmGame *g, const uint8_t *d) {  // (PokeCrypto.IsEncrypted8 / 8A: string terminators set)
    return g->shuffle8a ? (u16le(d + 0x78) || u16le(d + 0x128)) : (u16le(d + 0x70) || u16le(d + 0x110));
}

// ------------------------------------------------------------------ Pokémon Files

static void mkdirs(const char *path) {
    char p[1100];
    strlcpy(p, path, sizeof p);
    for (char *s = p + 1; *s; s++)
        if (*s == '/') { *s = 0; mkdir(p, 0755); *s = '/'; }
    mkdir(p, 0755);
}

static void nickname(const PkmGame *g, const uint8_t *d, char *out, size_t n) {  // (UTF-16 -> UTF-8, file-name safe)
    size_t o = 0;
    for (int i = 0; i < 12 && o + 4 < n; i++) {
        unsigned c = u16le(d + g->nick + i * 2);
        if (!c) break;
        if (c < 0x80) out[o++] = strchr("\\/:*?\"<>|", (int)c) || c < 0x20 ? '_' : (char)c;
        else if (c < 0x800) out[o++] = (char)(0xC0 | c >> 6), out[o++] = (char)(0x80 | (c & 0x3F));
        else out[o++] = (char)(0xE0 | c >> 12), out[o++] = (char)(0x80 | ((c >> 6) & 0x3F)), out[o++] = (char)(0x80 | (c & 0x3F));
    }
    out[o] = 0;
}

static void pk_export(void) {
    const PkmGame *g = game();
    uint64_t b = g ? box1(g) : 0;
    if (!b) { menu_notice("No boxes to export yet (load your save first)"); return; }
    char dir[1100], stamp[32], path[1300];
    time_t now = time(NULL);
    strftime(stamp, sizeof stamp, "%Y-%m-%d_%H-%M-%S", localtime(&now));
    snprintf(dir, sizeof dir, "%s/pokemon/export/%s", rt_data_root(), stamp);
    int n = 0, bad = 0;
    for (int box = 0; box < BOXES; box++)
        for (int s = 0; s < SLOTS; s++) {
            uint8_t d[0x198];
            uint64_t a = b + (uint64_t)(box * SLOTS + s) * (uint64_t)g->slot;
            if (!readable(a, (uint64_t)g->data)) continue;
            memcpy(d, (const void *)(uintptr_t)a, (size_t)g->data);
            pk_decrypt(g, d);
            if (!u16le(d + 8)) continue;  // (empty)
            if (!pk_checksum_ok(g, d)) { bad++; continue; }
            if (!n) mkdirs(dir);
            char nick[64];
            nickname(g, d, nick, sizeof nick);
            snprintf(path, sizeof path, "%s/Box %02d Slot %02d - %s (%08X).%s", dir, box + 1, s + 1, nick, u32le(d), g->ext);
            FILE *f = fopen(path, "wb");
            if (f) fwrite(d, 1, (size_t)g->data, f), fclose(f), n++;
        }
    char msg[160];
    if (n) snprintf(msg, sizeof msg, "Exported %d Pokémon (pokemon/export/%s)%s", n, stamp, bad ? ", some slots unreadable" : "");
    else snprintf(msg, sizeof msg, "No Pokémon in your boxes to export");
    menu_notice(msg);
    LOG("[pkm] exported %d to %s (%d with a bad checksum)\n", n, dir, bad);
}

static int next_empty(const PkmGame *g, uint64_t b, int from) {  // (box * 30 + slot, or -1)
    for (int i = from; i < BOXES * SLOTS; i++) {
        uint8_t d[0x198];
        uint64_t a = b + (uint64_t)i * (uint64_t)g->slot;
        if (!readable(a, (uint64_t)g->data)) return -1;
        memcpy(d, (const void *)(uintptr_t)a, (size_t)g->data);
        pk_decrypt(g, d);
        if (!u16le(d + 8)) return i;
    }
    return -1;
}

// A file PKHeX wrote for this game, as the box slot holds it (encrypted): false if it isn't one
static bool pk_load(const PkmGame *g, const char *path, uint8_t *d) {
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    uint8_t buf[0x200];
    size_t n = fread(buf, 1, sizeof buf, f);
    fclose(f);
    // Z-A: the party format only (boxes keep the party stats); Arceus: stored or party (the stored part)
    bool ok = g->shuffle8a ? (n == 0x168 || n == 0x178) : n == 0x158;
    if (!ok) return false;
    memcpy(d, buf, (size_t)g->data);
    if (pk_encrypted(g, d)) pk_decrypt(g, d);
    if (!u16le(d + 8) || !pk_checksum_ok(g, d)) return false;
    pk_encrypt(g, d);
    return true;
}

static void pk_import(void) {
    const PkmGame *g = game();
    uint64_t b = g ? box1(g) : 0;
    if (!b) { menu_notice("No boxes to import into yet (load your save first)"); return; }
    char dir[1100], done[1100];
    snprintf(dir, sizeof dir, "%s/pokemon/import", rt_data_root());
    snprintf(done, sizeof done, "%s/done", dir);
    mkdirs(dir);
    int placed = 0, skipped = 0, slot = 0, first = -1;
    bool full = false;
    static char names[256][256];  // (sorted: the order they're placed in)
    int nn = 0;
    DIR *dd = opendir(dir);
    for (struct dirent *e; dd && nn < 256 && (e = readdir(dd));) {
        const char *dot = strrchr(e->d_name, '.');
        if (dot && !strcasecmp(dot + 1, g->ext)) strlcpy(names[nn++], e->d_name, sizeof names[0]);
    }
    if (dd) closedir(dd);
    qsort(names, (size_t)nn, sizeof names[0], (int (*)(const void *, const void *))strcmp);
    for (int i = 0; i < nn && !full; i++) {
        char path[1300], to[1300];
        snprintf(path, sizeof path, "%s/%s", dir, names[i]);
        uint8_t d[0x198];
        if (!pk_load(g, path, d)) { skipped++; LOG("[pkm] %s: not a valid .%s, left in import\n", names[i], g->ext); continue; }
        slot = next_empty(g, b, slot);
        if (slot < 0) { full = true; break; }
        uint64_t a = b + (uint64_t)slot * (uint64_t)g->slot;
        if (!guest_write(a, d, (uint64_t)g->data)) { full = true; break; }
        if (g->slot > g->data) { uint8_t present = 1; guest_write(a + (uint64_t)g->data, &present, 1); }  // (Z-A's footer)
        if (first < 0) first = slot;
        LOG("[pkm] %s -> box %d slot %d\n", names[i], slot / SLOTS + 1, slot % SLOTS + 1);
        mkdirs(done);
        snprintf(to, sizeof to, "%s/%s", done, names[i]);
        rename(path, to);
        placed++, slot++;
    }
    char msg[200];
    if (placed)
        snprintf(msg, sizeof msg, "Imported %d Pokémon from box %d%s%s - save in the game to keep them", placed, first / SLOTS + 1,
                 skipped ? ", some files skipped" : "", full ? ", boxes full" : "");
    else if (full) snprintf(msg, sizeof msg, "Your boxes are full");
    else snprintf(msg, sizeof msg, "No .%s files to import in pokemon/import%s", g->ext, skipped ? " (some weren't valid)" : "");
    menu_notice(msg);
}

static int g_files_choice;
int pkm_files_get(void) { return g_files_choice; }
void pkm_files_set(int v) { g_files_choice = v; }
void pkm_files_run(int v) { if (v) pk_import(); else pk_export(); }

// ------------------------------------------------------------------ PKHeX Link (sys-botbase's protocol)

typedef struct { char *p; size_t n, cap; } Out;
static void out_put(Out *o, const char *s, size_t n) {
    if (o->n + n + 1 > o->cap) {
        size_t c = o->cap ? o->cap : 256;
        while (c < o->n + n + 1) c *= 2;
        char *q = realloc(o->p, c);
        if (!q) return;
        o->p = q, o->cap = c;
    }
    memcpy(o->p + o->n, s, n), o->n += n, o->p[o->n] = 0;
}
static void out_fmt(Out *o, const char *fmt, unsigned long long v) {
    char b[40];
    int k = snprintf(b, sizeof b, fmt, v);
    out_put(o, b, (size_t)k);
}
static void out_str(Out *o, const char *s) { out_put(o, s, strlen(s)); }

#define PEEK_MAX (64u << 20)
static void out_peek(Out *o, uint64_t a, uint64_t n) {  // (hex, upper case; an unreadable range: just the newline)
    if (readable(a, n) && n <= PEEK_MAX) {
        static const char hx[] = "0123456789ABCDEF";
        const uint8_t *p = (const uint8_t *)(uintptr_t)a;
        char buf[512];
        size_t k = 0;
        for (uint64_t i = 0; i < n; i++) {
            buf[k++] = hx[p[i] >> 4], buf[k++] = hx[p[i] & 15];
            if (k == sizeof buf) out_put(o, buf, k), k = 0;
        }
        out_put(o, buf, k);
    }
    out_str(o, "\n");
}

static uint64_t num(const char *s) { return (strlen(s) > 2 && s[1] == 'x') ? strtoull(s, NULL, 16) : strtoull(s, NULL, 10); }
static int64_t snum(const char *s) {
    return (strlen(s) > 2 && (s[1] == 'x' || s[2] == 'x')) ? strtoll(s, NULL, 16) : strtoll(s, NULL, 10);
}
static uint8_t *bytes(const char *s, uint64_t *n) {  // "0x0A0B..." (an odd count: a leading 0)
    if (strlen(s) > 2 && s[1] == 'x') s += 2;
    size_t len = strlen(s), m = (len + 1) / 2;
    uint8_t *b = malloc(m ? m : 1);
    if (!b) { *n = 0; return NULL; }
    for (size_t i = 0, j = 0; i < m; i++) {
        char t[3] = {0};
        if (i == 0 && (len & 1)) t[0] = s[j++];
        else t[0] = s[j], t[1] = s[j + 1], j += 2;
        b[i] = (uint8_t)strtoul(t, NULL, 16);
    }
    *n = m;
    return b;
}
static uint64_t follow(const int64_t *j, int n) {  // (sys-botbase's followMainPointer)
    uint64_t a = rd64(MAIN_BASE + (uint64_t)j[0]);
    for (int i = 1; i < n && a; i++) a = rd64(a + (uint64_t)j[i]);
    return a;
}

static void command(int argc, char **argv, Out *o) {
    const PkmGame *g = game();
    const char *c = argv[0];
    uint64_t heap = HEAP_BASE;
    bool run = running();
    if (!strcmp(c, "getVersion")) out_str(o, "2.5\n");  // (LiveHeX wants 2.5 or newer)
    else if (!strcmp(c, "getTitleID")) run ? out_fmt(o, "%016llX\n", game_title_id()) : out_str(o, "\n");
    else if (!strcmp(c, "getTitleVersion")) run ? out_fmt(o, "%016llX\n", 0) : out_str(o, "\n");
    else if (!strcmp(c, "getMainNsoBase")) run ? out_fmt(o, "%016llX\n", MAIN_BASE) : out_str(o, "\n");
    else if (!strcmp(c, "getHeapBase")) run ? out_fmt(o, "%016llX\n", heap) : out_str(o, "\n");
    else if (!strcmp(c, "getBuildID")) {
        if (!run) { out_str(o, "\n"); return; }
        uint8_t id[8] = {0};
        char p[1100];
        snprintf(p, sizeof p, "%s/extracted/exefs/main", rt_data_root());
        FILE *f = fopen(p, "rb");
        if (f) { fseek(f, 0x40, SEEK_SET); if (fread(id, 1, 8, f) != 8) memset(id, 0, 8); fclose(f); }
        for (int i = 0; i < 8; i++) out_fmt(o, "%02llx", id[i]);
        out_str(o, "\n");
    } else if (!strcmp(c, "getSystemLanguage")) out_str(o, "1\n");  // (en-US)
    else if (!strcmp(c, "isProgramRunning")) out_str(o, "0\n");     // (no Tesla overlay, no cheat module)
    else if (!strcmp(c, "game") && argc == 2) {
        if (!strcmp(argv[1], "name")) out_str(o, g ? g->name : ""), out_str(o, "\n");
        else if (!strcmp(argv[1], "version")) out_str(o, g ? g->version : ""), out_str(o, "\n");
        else if (!strcmp(argv[1], "author")) out_str(o, "The Pokémon Company\n");
        else if (!strcmp(argv[1], "rating")) out_str(o, "0\n");
        else out_str(o, "\n");
    } else if (!strncmp(c, "peek", 4) && !strstr(c, "Multi") && argc == 3) {
        uint64_t base = !strcmp(c, "peek") ? heap : !strcmp(c, "peekMain") ? MAIN_BASE : 0;
        if (strcmp(c, "peek") && strcmp(c, "peekMain") && strcmp(c, "peekAbsolute")) return;
        if (!run && base) { out_str(o, "\n"); return; }
        out_peek(o, base + num(argv[1]), num(argv[2]));
    } else if (!strncmp(c, "peek", 4) && strstr(c, "Multi") && argc >= 3 && argc % 2) {
        uint64_t base = !strcmp(c, "peekMulti") ? heap : !strcmp(c, "peekMainMulti") ? MAIN_BASE : 0;
        Out t = {0};
        for (int i = 1; i + 1 < argc; i += 2) {
            out_peek(&t, base + num(argv[i]), num(argv[i + 1]));
            if (t.n) t.n--;  // (one line for all)
        }
        out_put(o, t.p ? t.p : "", t.n), out_str(o, "\n"), free(t.p);
    } else if ((!strcmp(c, "poke") || !strcmp(c, "pokeMain") || !strcmp(c, "pokeAbsolute")) && argc == 3) {
        uint64_t base = !strcmp(c, "poke") ? heap : !strcmp(c, "pokeMain") ? MAIN_BASE : 0, n = 0;
        uint8_t *b = bytes(argv[2], &n);
        if (b && run && !guest_write(base + num(argv[1]), b, n)) LOG("[pkhex] %s 0x%llx (%llu bytes): refused\n", c, (unsigned long long)(base + num(argv[1])), (unsigned long long)n);
        free(b);
    } else if (!strcmp(c, "pointer") && argc >= 2) {
        int64_t j[32];
        int n = argc - 1 < 32 ? argc - 1 : 32;
        for (int i = 0; i < n; i++) j[i] = snum(argv[i + 1]);
        out_fmt(o, "%016llX\n", follow(j, n));
    } else if ((!strcmp(c, "pointerAll") || !strcmp(c, "pointerRelative")) && argc >= 3) {
        int64_t j[32];
        int n = argc - 2 < 32 ? argc - 2 : 32;
        for (int i = 0; i < n; i++) j[i] = snum(argv[i + 1]);
        uint64_t a = follow(j, n);
        if (a) a += (uint64_t)snum(argv[argc - 1]) - (c[7] == 'R' ? heap : 0);
        out_fmt(o, "%016llX\n", a);
    } else if ((!strcmp(c, "pointerPeek") || !strcmp(c, "pointerPoke")) && argc >= 4) {
        int64_t j[32];
        int n = argc - 3 < 32 ? argc - 3 : 32;
        for (int i = 0; i < n; i++) j[i] = snum(argv[i + 2]);
        uint64_t a = follow(j, n) + (uint64_t)snum(argv[argc - 1]);
        if (c[7] == 'e') { out_peek(o, a, num(argv[1])); return; }
        uint64_t m = 0;
        uint8_t *b = bytes(argv[1], &m);
        if (b) guest_write(a, b, m);
        free(b);
    } else if (!strcmp(c, "configure")) {
        // (echoCommands, sleep times...: nothing to set here)
    } else if (!strcmp(c, "pixelPeek")) out_str(o, "\n");
    // (buttons, sticks and touch: not offered - the recomp has its own input)
}

static _Atomic sock_t g_listen = INVALID_SOCKET, g_client = INVALID_SOCKET;
static _Atomic int g_mode;  // 0 off, 1 this PC, 2 the network

// (shutdown first: on Linux a close alone doesn't wake a thread blocked in accept / recv, and the port stays taken)
static void sock_drop(sock_t s) {
    if (s == INVALID_SOCKET) return;
    shutdown(s, 2);
    sock_close(s);
}

static void serve(sock_t s) {
    char *line = malloc(1 << 20);
    size_t n = 0;
    if (!line) return;
    for (;;) {
        char ch;
        int r = (int)recv(s, &ch, 1, 0);
        if (r <= 0) break;
        if (ch != '\n') { if (n + 1 < (1 << 20)) line[n++] = ch; continue; }
        if (n && line[n - 1] == '\r') n--;
        line[n] = 0, n = 0;
        char *argv[96];
        int argc = 0;
        for (char *t = strtok(line, " "); t && argc < 96; t = strtok(NULL, " ")) argv[argc++] = t;
        if (!argc) continue;
        Out o = {0};
        command(argc, argv, &o);
        for (size_t k = 0; k < o.n;) {
            int w = (int)send(s, o.p + k, (int)(o.n - k), 0);
            if (w <= 0) break;
            k += (size_t)w;
        }
        free(o.p);
    }
    free(line);
}

static void *link_thread(void *arg) {
    sock_t l = (sock_t)(uintptr_t)arg;
    for (;;) {
        sock_t s = accept(l, NULL, NULL);
        if (s == INVALID_SOCKET) break;  // (closed: switched off)
        LOG("[pkhex] connected\n");
        atomic_store(&g_client, s);
        serve(s);
        if (atomic_exchange(&g_client, INVALID_SOCKET) == s) sock_close(s);  // (else switched off: dropped there)
        LOG("[pkhex] disconnected\n");
    }
    return NULL;
}

// Start, restart or stop the server for the setting (g_settings.pkhex_link)
void pkm_link_apply(void) {
    int want = g_settings.pkhex_link;
    if (want == atomic_load(&g_mode)) return;
    sock_drop(atomic_exchange(&g_listen, INVALID_SOCKET));  // (its accept fails: the thread ends)
    sock_drop(atomic_exchange(&g_client, INVALID_SOCKET));
    atomic_store(&g_mode, 0);
    if (!want) return;
#ifdef _WIN32
    static bool wsa;
    if (!wsa) { WSADATA w; wsa = WSAStartup(MAKEWORD(2, 2), &w) == 0; }
#endif
    sock_t l = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (l == INVALID_SOCKET) return;
    int yes = 1;
    setsockopt(l, SOL_SOCKET, SO_REUSEADDR, (const char *)&yes, sizeof yes);
    struct sockaddr_in a = {0};
    a.sin_family = AF_INET, a.sin_port = htons(6000);
    a.sin_addr.s_addr = htonl(want == 2 ? INADDR_ANY : INADDR_LOOPBACK);
    if (bind(l, (struct sockaddr *)&a, sizeof a) || listen(l, 1)) {
        LOG("[pkhex] port 6000 is taken: PKHeX Link off\n");
        sock_close(l);
        menu_notice("PKHeX Link: port 6000 is in use by another program");
        return;
    }
    atomic_store(&g_listen, l), atomic_store(&g_mode, want);
    pthread_t t;
    pthread_create(&t, NULL, link_thread, (void *)(uintptr_t)l);
    pthread_detach(t);
    LOG("[pkhex] PKHeX Link on port 6000 (%s)\n", want == 2 ? "local network" : "this PC");
}
