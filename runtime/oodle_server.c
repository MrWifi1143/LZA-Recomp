// Oodle decoding for Legends Studio (the mod editor): `lasrecomp <game root> --oodle-server` loads the game's code but,
// instead of starting the game, decodes data with the game's own OodleLZ_Decompress. Most of Z-A's files (and Arceus's
// Pokemon packs) are Oodle-compressed, and Oodle can't be shipped with the editor: the player's own game does it.
//
// Protocol (stdin -> stdout, binary, one request at a time): u64 compressed size, u64 decompressed size, the
// compressed bytes; reply: i64 result (the decompressed size on success, <= 0 on failure), then that many bytes.
// The first reply is sent before any request: i64 1 = ready, 0 = this build has no known OodleLZ_Decompress.
// EOF on stdin ends the process. Everything else the runtime prints goes to stderr.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

#include "gamelib.h"
#include "hle.h"
#include "rt.h"

// OodleLZ_Decompress in the builds the runtime knows, by main's build id (the function passes its own name to a profiler
// hook; its only caller gives it decoder memory and thread phase 3, as below)
static const struct { uint8_t id[8]; uint64_t fn; } k_builds[] = {
    {{0xb1, 0xf1, 0x2f, 0xd9, 0x19, 0xea, 0xe8, 0x6a}, 0x7001a9c9e0ull},  // Legends Z-A 0.6.0 (caller 0x7001a9b718)
    {{0xae, 0xe8, 0xf1, 0x50, 0xdd, 0xa1, 0xb5, 0xa8}, 0x7000004fc0ull},  // Legends: Arceus (caller 0x7002975eac)
};
static uint64_t g_fn;
static FILE *g_out;
int g_oodle_out_fd = -1;  // (main.c: stdout as the process started)

static bool read_all(void *p, size_t n) { return fread(p, 1, n, stdin) == n; }

static void reply(int64_t r, const void *data) {
    fwrite(&r, 8, 1, g_out);
    if (r > 0 && data) fwrite(data, 1, (size_t)r, g_out);
    fflush(g_out);
}

static void serve(Cpu *c) {
    reply(g_fn ? 1 : 0, NULL);
    if (!g_fn) _exit(3);
    uint8_t *dec_mem = calloc(1, 0x80000);
    for (;;) {
        uint64_t hdr[2];
        if (!read_all(hdr, sizeof hdr)) _exit(0);
        uint64_t n = hdr[0], raw = hdr[1];
        if (n > (1ull << 31) || raw > (1ull << 31)) _exit(4);
        uint8_t *in = malloc(n ? n : 1), *out = malloc(raw ? raw : 1);
        if (!in || !out || !read_all(in, n)) _exit(5);
        // OodleLZ_Decompress(comp, compSize, raw, rawLen, fuzzSafe 1, checkCRC 0, verbosity 0, decBufBase 0 | on the
        // stack: decBufSize 0, callback 0, callback data 0, decoder memory, its size, thread phase 3)
        uint64_t sp = c->sp;
        c->sp = (sp - 0x40) & ~15ull;
        uint64_t *st = (uint64_t *)(uintptr_t)c->sp;
        st[0] = 0, st[1] = 0, st[2] = 0, st[3] = (uint64_t)(uintptr_t)dec_mem, st[4] = 0x80000, st[5] = 3;
        c->x[0] = (uint64_t)(uintptr_t)in, c->x[1] = n, c->x[2] = (uint64_t)(uintptr_t)out, c->x[3] = raw;
        c->x[4] = 1, c->x[5] = 0, c->x[6] = 0, c->x[7] = 0;
        guest_call(c, g_fn);
        int64_t r = (int64_t)c->x[0];
        c->sp = sp;
        reply(r, out);
        free(in);
        free(out);
    }
}

// main.c, --oodle-server: called by boot() once the modules are loaded. The SDK starts as usual (nn::init::Start: its
// heap, nn::os's thread objects - Oodle takes nn::os locks), and the game's nnMain is replaced by the server.
void oodle_server_install(const char *exefs) {
    int fd = g_oodle_out_fd >= 0 ? g_oodle_out_fd : dup(1);
#ifdef _WIN32
    _setmode(fd, _O_BINARY);
    _setmode(_fileno(stdin), _O_BINARY);
#endif
    g_out = fdopen(fd, "wb");
    uint8_t id[8] = {0};
    char mp[1100];
    snprintf(mp, sizeof mp, "%s/main", exefs);
    FILE *f = fopen(mp, "rb");
    if (f) {
        if (fseek(f, 0x40, SEEK_SET) || fread(id, 1, 8, f) != 8) memset(id, 0, 8);
        fclose(f);
    }
    for (size_t i = 0; i < sizeof k_builds / sizeof *k_builds; i++)
        if (!memcmp(id, k_builds[i].id, 8)) g_fn = k_builds[i].fn;
    uint64_t nn_main = loader_find_symbol("nnMain");
    if (!nn_main) { fprintf(stderr, "[oodle] nnMain not found\n"); reply(0, NULL); _exit(2); }
    hle_override(nn_main, serve, "<oodle_server>");
}
