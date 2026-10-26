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
    {{0xb1, 0xf1, 0x2f, 0xd9, 0x19, 0xea, 0xe8, 0x6a}, 0x7001a9c9e0ull},  // Legends Z-A 1.x (caller 0x7001a9b718)
    {{0x8a, 0x1c, 0x86, 0xc4, 0x37, 0x39, 0x4b, 0x69}, 0x7001a9d000ull},  // Legends Z-A 2.0.0 (caller 0x7001a9bd38)
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

// ---- --extract-shaders <romfs> (the shader warm-up's first step, runtime/nvn/vulkan/warmup.c): Z-A keeps its shader
// containers in data.trpfs, mostly Oodle-compressed, where the warm-up's scan of the romfs can't see them - so a first
// start had nothing to compile up front. This mode decodes the packs whose names say they hold shaders
// ("...gfx2shader..." in data.trpfd's pack names: every .bnsh the game has), with the game's own decoder as above, and
// writes each file that holds a BNSH container to cache/warm/romfs/<hash>.bin for the warm-up to scan; then exits.
//   data.trpfd {0 [u64 file hash], 1 [pack name], 2 [{0 u64 pack index}]}; data.trpfs "ONEPACK\0", u64 offset of
//   {0 [u64 FNV-1a of a pack name], 1 [u64 offset]}; a pack (TRPAK) {0 [u64 hash], 1 [{1 i8 compression (-1 stored,
//   3..7 Oodle), 3 u64 size, 4 [u8 data]}]}
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#define xmkdir(p) _mkdir(p)
#define xseek(f, o) _fseeki64(f, (long long)(o), SEEK_SET)
#define xsize(f) (_fseeki64(f, 0, SEEK_END), (uint64_t)_ftelli64(f))
#else
#define xmkdir(p) mkdir(p, 0755)
#define xseek(f, o) fseeko(f, (off_t)(o), SEEK_SET)
#define xsize(f) (fseeko(f, 0, SEEK_END), (uint64_t)ftello(f))
#endif
static bool has_bytes(const uint8_t *h, size_t n, const char *k, size_t kn) {  // (memmem: not in every C library)
    for (size_t i = 0; i + kn <= n; i++) if (h[i] == (uint8_t)k[0] && !memcmp(h + i, k, kn)) return true;
    return false;
}
typedef struct { const uint8_t *p; size_t n; } FbBuf;
static uint32_t fb32(FbBuf b, size_t o) { uint32_t v = 0; if (o + 4 <= b.n) memcpy(&v, b.p + o, 4); return v; }
static uint64_t fb64(FbBuf b, size_t o) { uint64_t v = 0; if (o + 8 <= b.n) memcpy(&v, b.p + o, 8); return v; }
static size_t fb_root(FbBuf b) { return fb32(b, 0); }
static size_t fb_field(FbBuf b, size_t t, int id) {  // its offset, 0 when absent
    if (t + 4 > b.n) return 0;
    int32_t so;
    memcpy(&so, b.p + t, 4);
    size_t vt = (size_t)((int64_t)t - so);
    if (vt + 4 > b.n) return 0;
    uint16_t vs, fo = 0;
    memcpy(&vs, b.p + vt, 2);
    if (4 + 2 * (size_t)id + 2 > vs || vt + 4 + 2 * (size_t)id + 2 > b.n) return 0;
    memcpy(&fo, b.p + vt + 4 + 2 * id, 2);
    return fo ? t + fo : 0;
}
static size_t fb_deref(FbBuf b, size_t at) { return at ? at + fb32(b, at) : 0; }  // (a uoffset)
static size_t fb_vec(FbBuf b, size_t t, int id, uint32_t *count) {  // the first element's offset
    size_t v = fb_deref(b, fb_field(b, t, id));
    *count = v ? fb32(b, v) : 0;
    return v + 4;
}
static uint64_t fnv64(const char *s, size_t n) {
    uint64_t h = 0xcbf29ce484222645ull;
    for (size_t i = 0; i < n; i++) h = (h ^ (uint8_t)s[i]) * 0x100000001b3ull;
    return h;
}
static uint8_t *read_range(const char *path, uint64_t off, uint64_t n) {
    FILE *f = fopen(path, "rb");
    uint8_t *d = f ? malloc(n ? n : 1) : NULL;
    bool ok = d && !xseek(f, off) && fread(d, 1, n, f) == n;
    if (f) fclose(f);
    if (!ok) { free(d); return NULL; }
    return d;
}
static const char *g_extract_romfs;

static int64_t oodle_decode(Cpu *c, uint8_t *dec_mem, const uint8_t *in, uint64_t n, uint8_t *out, uint64_t raw) {
    uint64_t sp = c->sp;
    c->sp = (sp - 0x40) & ~15ull;
    uint64_t *st = (uint64_t *)(uintptr_t)c->sp;
    st[0] = 0, st[1] = 0, st[2] = 0, st[3] = (uint64_t)(uintptr_t)dec_mem, st[4] = 0x80000, st[5] = 3;
    c->x[0] = (uint64_t)(uintptr_t)in, c->x[1] = n, c->x[2] = (uint64_t)(uintptr_t)out, c->x[3] = raw;
    c->x[4] = 1, c->x[5] = 0, c->x[6] = 0, c->x[7] = 0;
    guest_call(c, g_fn);
    int64_t r = (int64_t)c->x[0];
    c->sp = sp;
    return r;
}

static void extract(Cpu *c) {
    char fdp[1100], fsp[1100];
    snprintf(fdp, sizeof fdp, "%s/arc/data.trpfd", g_extract_romfs);
    snprintf(fsp, sizeof fsp, "%s/arc/data.trpfs", g_extract_romfs);
    FILE *f = fopen(fdp, "rb");
    long fdn = 0;
    if (f) { fseek(f, 0, SEEK_END); fdn = ftell(f); fclose(f); }
    uint8_t *fd = fdn > 0 ? read_range(fdp, 0, (uint64_t)fdn) : NULL, *head = read_range(fsp, 0, 16);
    if (!g_fn || !fd || !head || memcmp(head, "ONEPACK", 7)) { fprintf(stderr, "[extract] no decoder or no data.trpfd / data.trpfs\n"); _exit(2); }
    FbBuf d = {fd, (size_t)fdn};
    size_t root = fb_root(d);
    uint32_t nnames;
    size_t names = fb_vec(d, root, 1, &nnames);
    // the shader packs: their name hashes
    uint64_t want[64];
    int nwant = 0;
    for (uint32_t i = 0; i < nnames && nwant < 64; i++) {
        size_t s = fb_deref(d, names + 4 * (size_t)i);
        uint32_t len = fb32(d, s);
        if (!s || s + 4 + len > d.n) continue;
        const char *nm = (const char *)fd + s + 4;
        if (has_bytes((const uint8_t *)nm, len, "gfx2shader", 10)) want[nwant++] = fnv64(nm, len);
    }
    // where they are in data.trpfs (a pack ends where the next one starts, or at the table)
    FILE *fs = fopen(fsp, "rb");
    if (!fs) _exit(2);
    uint64_t total = xsize(fs), toff;
    fclose(fs);
    memcpy(&toff, head + 8, 8);
    uint8_t *tab = toff < total ? read_range(fsp, toff, total - toff) : NULL;
    if (!tab) _exit(2);
    FbBuf t = {tab, (size_t)(total - toff)};
    uint32_t nh, no;
    size_t th = fb_vec(t, fb_root(t), 0, &nh), to = fb_vec(t, fb_root(t), 1, &no);
    xmkdir("cache"), xmkdir("cache/warm"), xmkdir("cache/warm/romfs");
    uint8_t *dec_mem = calloc(1, 0x80000);
    int files = 0, written = 0;
    for (int w = 0; w < nwant; w++) {
        uint64_t off = 0, end = toff;
        bool found = false;
        for (uint32_t i = 0; i < nh && i < no; i++) if (fb64(t, th + 8 * (size_t)i) == want[w]) off = fb64(t, to + 8 * (size_t)i), found = true;
        if (!found) continue;
        for (uint32_t i = 0; i < no; i++) { uint64_t o = fb64(t, to + 8 * (size_t)i); if (o > off && o < end) end = o; }
        uint8_t *pk = read_range(fsp, off, end - off);
        if (!pk) continue;
        FbBuf p = {pk, (size_t)(end - off)};
        size_t pr = fb_root(p);
        uint32_t nph, npf;
        size_t ph = fb_vec(p, pr, 0, &nph), pf = fb_vec(p, pr, 1, &npf);
        for (uint32_t i = 0; i < nph && i < npf; i++) {
            size_t e = fb_deref(p, pf + 4 * (size_t)i), co = fb_field(p, e, 1), so = fb_field(p, e, 3);
            uint32_t dn;
            size_t dd = fb_vec(p, e, 4, &dn);
            if (!e || dd + dn > p.n) continue;
            int8_t comp = co ? (int8_t)pk[co] : -1;
            uint64_t raw = so ? fb64(p, so) : dn;
            uint8_t *out = NULL;
            int64_t r = -1;
            if (comp == -1) out = malloc(dn ? dn : 1), memcpy(out, pk + dd, dn), r = dn;
            else if (comp >= 3 && comp <= 7 && raw < (256u << 20) && (out = malloc(raw ? raw : 1))) r = oodle_decode(c, dec_mem, pk + dd, dn, out, raw);
            files++;
            if (r > 0x60 && has_bytes(out, (size_t)r, "BNSH\0\0\0\0", 8)) {
                char path[96];
                snprintf(path, sizeof path, "cache/warm/romfs/%016llx.bin", (unsigned long long)fb64(p, ph + 8 * (size_t)i));
                FILE *o = fopen(path, "wb");
                if (o) { written += fwrite(out, 1, (size_t)r, o) == (size_t)r; fclose(o); }
            }
            free(out);
        }
        free(pk);
    }
    fprintf(stderr, "[extract] %d shader packs, %d files decoded, %d with shader containers -> cache/warm/romfs\n", nwant, files, written);
    _exit(written ? 0 : 3);
}

// main.c, --oodle-server: called by boot() once the modules are loaded. The SDK starts as usual (nn::init::Start: its
// heap, nn::os's thread objects - Oodle takes nn::os locks), and the game's nnMain is replaced by the server.
void extract_shaders_install(const char *exefs, const char *romfs) {
    g_extract_romfs = romfs;
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
    if (!nn_main || !g_fn) { fprintf(stderr, "[extract] this game build has no known decoder\n"); _exit(2); }
    hle_override(nn_main, extract, "<extract_shaders>");
}

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
