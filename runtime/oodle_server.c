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

// ---- --build-trees (Lush Foliage's trees, runtime/mods.h mod.ini tree_patch): a mod can carry trees as a patch of the
// game's files - only the trees' matrices and file names, no game data (Legends Studio's `foliage ... <patch>`,
// formats/foliage.h: "LFT1", u32 ops, each {u8 kind, str pack, str path, str tmpl, str model, str trtrr, u32 trees,
// f32[16] each}, str = u16 length + bytes). This mode builds the mod's files from the player's own game: each area pack
// (bin/archive/field/terrain/<area>.gfpak, a GFLXPACK) read from the RomFS, the entries the patch names decoded (the
// game's own Oodle, as above), the trees added - kind 0 appended to an existing .trins ({0 "SrtInstanceBuffer", 2
// count, 4 {0 [ubyte] 64-byte matrices}}: a new matrix vector appended, the table pointed at it), kind 1 a new .trins
// made from tmpl (a file of the same tree) and listed in its tree's group of the chunk's .trtrr ({4 [{0 model, 1
// [names]}]}) - and the pack written to mods/<mod>/romfs/<pack> (changed and new entries as stored LZ4; the tables
// rebuilt). trees_prepare runs it before the game when the mod's build is missing or stale.
typedef struct { uint8_t *p; size_t n, cap; } Bytes;
static void by_put(Bytes *b, const void *d, size_t n) {
    if (b->n + n > b->cap) { b->cap = (b->n + n) * 2 + 64; b->p = realloc(b->p, b->cap); }
    if (n) memcpy(b->p + b->n, d, n);
    b->n += n;
}
static void by_zero(Bytes *b, size_t n) { while (n--) { uint8_t z = 0; by_put(b, &z, 1); } }
static void by_align(Bytes *b, size_t a, size_t plus) { while ((b->n + plus) % a) by_zero(b, 1); }
static void put32(Bytes *b, size_t at, uint32_t v) { memcpy(b->p + at, &v, 4); }

typedef struct { uint8_t kind; char pack[256], path[256], tmpl[256], model[128], trtrr[256]; uint32_t n; const float *mats; } TreeOp;

static bool rd_str(const uint8_t *d, size_t n, size_t *o, char *out, size_t cap) {
    if (*o + 2 > n) return false;
    uint16_t l = (uint16_t)(d[*o] | d[*o + 1] << 8);
    *o += 2;
    if (*o + l > n || l >= cap) return false;
    memcpy(out, d + *o, l), out[l] = 0;
    *o += l;
    return true;
}

// a stored LZ4 block (literals only: what a decoder reads back as the bytes)
static void lz4_stored(Bytes *out, const uint8_t *d, size_t n) {
    uint8_t tok = (uint8_t)((n >= 15 ? 15 : n) << 4);
    by_put(out, &tok, 1);
    if (n >= 15) {
        size_t r = n - 15;
        while (r >= 255) { uint8_t f = 255; by_put(out, &f, 1); r -= 255; }
        uint8_t l = (uint8_t)r;
        by_put(out, &l, 1);
    }
    by_put(out, d, n);
}
static bool lz4_dec(const uint8_t *in, size_t n, uint8_t *out, size_t outsz) {
    size_t i = 0, o = 0;
    while (i < n) {
        uint8_t t = in[i++];
        size_t lit = t >> 4;
        if (lit == 15) { uint8_t b; do { if (i >= n) return false; b = in[i++]; lit += b; } while (b == 255); }
        if (i + lit > n || o + lit > outsz) return false;
        memcpy(out + o, in + i, lit), i += lit, o += lit;
        if (i >= n) break;
        if (i + 2 > n) return false;
        size_t off = in[i] | ((size_t)in[i + 1] << 8), ml = t & 15;
        i += 2;
        if (ml == 15) { uint8_t b; do { if (i >= n) return false; b = in[i++]; ml += b; } while (b == 255); }
        ml += 4;
        if (!off || off > o || o + ml > outsz) return false;
        for (size_t k = 0; k < ml; k++, o++) out[o] = out[o - off];
    }
    return o == outsz;
}

typedef struct { uint8_t *d; size_t n; int32_t nf, nd; uint64_t ftab, hashes; } Pak;
static int pak_find(const Pak *p, uint64_t h) {
    for (int i = 0; i < p->nf; i++) { uint64_t v; memcpy(&v, p->d + p->hashes + 8 * (size_t)i, 8); if (v == h) return i; }
    return -1;
}
static uint8_t *pak_entry(Cpu *c, uint8_t *dec_mem, const Pak *p, int i, size_t *outn) {
    const uint8_t *r = p->d + p->ftab + 0x18 * (size_t)i;
    uint16_t codec;
    uint32_t size, packed;
    uint64_t off;
    memcpy(&codec, r + 2, 2), memcpy(&size, r + 4, 4), memcpy(&packed, r + 8, 4), memcpy(&off, r + 0x10, 8);
    if (off + packed > p->n || size > (256u << 20)) return NULL;
    uint8_t *out = malloc(size ? size : 1);
    bool ok = false;
    if (codec == 0) ok = packed == size, memcpy(out, p->d + off, size);
    else if (codec == 2) ok = lz4_dec(p->d + off, packed, out, size);
    else if (codec >= 3 && codec <= 7) ok = oodle_decode(c, dec_mem, p->d + off, packed, out, size) == (int64_t)size;
    if (!ok) { free(out); return NULL; }
    *outn = size;
    return out;
}

// the matrices put in a .trins: tmpl's tables, its matrices (kept or not) and these after them
static bool trins_with(const uint8_t *d, size_t n, bool keep, const float *mats, uint32_t count, Bytes *out) {
    FbBuf b = {d, n};
    size_t root = fb_root(b), body = fb_deref(b, fb_field(b, root, 4)), slot = body ? fb_field(b, body, 0) : 0, cslot = fb_field(b, root, 2);
    if (!slot || !cslot) return false;
    size_t v = fb_deref(b, slot);
    uint32_t vn = fb32(b, v);
    if (v + 4 + vn > n || vn % 64) return false;
    out->n = 0;
    by_put(out, d, n);
    by_align(out, 16, 4);
    size_t at = out->n;
    uint32_t total = (keep ? vn : 0) + count * 64;
    by_put(out, &total, 4);
    if (keep) by_put(out, d + v + 4, vn);
    by_put(out, mats, (size_t)count * 64);
    put32(out, slot, (uint32_t)(at - slot));
    put32(out, cslot, total / 64);
    return true;
}

// trtrr with name added to model's group (its names vector rebuilt after the end: the old names and the new one)
static bool trtrr_add(Bytes *t, const char *model, const char *name) {
    FbBuf b = {t->p, t->n};
    uint32_t ng;
    size_t g0 = fb_vec(b, fb_root(b), 4, &ng), slot = 0;
    char **names = NULL;
    uint32_t nn = 0;
    for (uint32_t gi = 0; gi < ng && !slot; gi++) {
        size_t gt = fb_deref(b, g0 + 4 * (size_t)gi), ms = fb_deref(b, fb_field(b, gt, 0));
        uint32_t ml = fb32(b, ms);
        if (!ms || ml != strlen(model) || memcmp(t->p + ms + 4, model, ml) || !fb_field(b, gt, 1)) continue;
        uint32_t cnt;
        size_t v0 = fb_vec(b, gt, 1, &cnt);
        names = calloc(cnt + 1, sizeof *names);
        for (uint32_t k = 0; k < cnt; k++) {
            size_t s = fb_deref(b, v0 + 4 * (size_t)k);
            uint32_t l = fb32(b, s);
            names[nn] = malloc(l + 1), memcpy(names[nn], t->p + s + 4, l), names[nn][l] = 0, nn++;
        }
        slot = fb_field(b, gt, 1);
    }
    if (!slot) return false;
    names[nn++] = strdup(name);
    by_align(t, 4, 0);
    size_t vp = t->n;
    by_put(t, &nn, 4);
    by_zero(t, 4 * (size_t)nn);
    for (uint32_t k = 0; k < nn; k++) {
        by_align(t, 4, 0);
        size_t sp = t->n;
        uint32_t l = (uint32_t)strlen(names[k]);
        by_put(t, &l, 4), by_put(t, names[k], l), by_zero(t, 1);
        put32(t, vp + 4 + 4 * k, (uint32_t)(sp - (vp + 4 + 4 * k)));
        free(names[k]);
    }
    free(names);
    put32(t, slot, (uint32_t)(vp - slot));
    return true;
}

// the pack with entries replaced (repl[i]: new bytes or NULL) and added (path hash, name hash, bytes) to the folder
// entry `sibling` is in: tables rebuilt (header, folder offsets, path hashes, folders, file table), the old payloads
// after them, the new ones (stored LZ4) at the end
typedef struct { uint64_t ph, nh; Bytes data; } NewEnt;
static bool pak_build(const Pak *p, Bytes *const *repl, const NewEnt *add, int nadd, int sibling, Bytes *out) {
    int nf2 = p->nf + nadd, nd = p->nd;
    uint8_t *recs = malloc(0x18 * (size_t)nf2);
    uint64_t *ph = malloc(8 * (size_t)nf2), data0 = ~0ull;
    for (int i = 0; i < p->nf; i++) {
        memcpy(recs + 0x18 * (size_t)i, p->d + p->ftab + 0x18 * (size_t)i, 0x18);
        memcpy(&ph[i], p->d + p->hashes + 8 * (size_t)i, 8);
        uint64_t off;
        memcpy(&off, recs + 0x18 * (size_t)i + 0x10, 8);
        if (off < data0) data0 = off;
    }
    for (int k = 0; k < nadd; k++) memcpy(recs + 0x18 * (size_t)(p->nf + k), recs + 0x18 * (size_t)sibling, 0x18), ph[p->nf + k] = add[k].ph;
    // the folders
    uint64_t *fh = calloc((size_t)nd, 8);
    int *fcnt = calloc((size_t)nd, sizeof(int)), sib_f = -1;
    uint64_t **fname = calloc((size_t)nd, sizeof *fname);
    int32_t **fidx = calloc((size_t)nd, sizeof *fidx);
    for (int f = 0; f < nd; f++) {
        uint64_t fo;
        int32_t cnt;
        memcpy(&fo, p->d + 0x28 + 8 * (size_t)f, 8);
        memcpy(&fh[f], p->d + fo, 8), memcpy(&cnt, p->d + fo + 8, 4);
        fname[f] = malloc(8 * (size_t)(cnt + nadd)), fidx[f] = malloc(4 * (size_t)(cnt + nadd));
        for (int k = 0; k < cnt; k++) {
            memcpy(&fname[f][k], p->d + fo + 0x10 + 0x10 * (size_t)k, 8), memcpy(&fidx[f][k], p->d + fo + 0x10 + 0x10 * (size_t)k + 8, 4);
            if (fidx[f][k] == sibling) sib_f = f;
        }
        fcnt[f] = cnt;
    }
    bool ok = sib_f >= 0 && data0 <= p->n;
    for (int k = 0; ok && k < nadd; k++) fname[sib_f][fcnt[sib_f]] = add[k].nh, fidx[sib_f][fcnt[sib_f]++] = p->nf + k;
    if (ok) {
        out->n = 0;
        by_zero(out, 0x28 + 8 * (size_t)nd);
        uint64_t h2 = out->n;
        by_put(out, ph, 8 * (size_t)nf2);
        uint64_t *fo2 = malloc(8 * (size_t)nd);
        for (int f = 0; f < nd; f++) {
            fo2[f] = out->n;
            by_put(out, &fh[f], 8), by_put(out, &fcnt[f], 4), by_zero(out, 4);
            for (int k = 0; k < fcnt[f]; k++) by_put(out, &fname[f][k], 8), by_put(out, &fidx[f][k], 4), by_zero(out, 4);
        }
        uint64_t ft2 = out->n;
        by_zero(out, 0x18 * (size_t)nf2);
        by_align(out, 16, 0);
        int64_t delta = (int64_t)out->n - (int64_t)data0;
        by_put(out, p->d + data0, p->n - data0);
        for (int i = 0; i < nf2; i++) {
            const Bytes *nb = i < p->nf ? repl[i] : &add[i - p->nf].data;
            uint8_t *r = recs + 0x18 * (size_t)i;
            if (!nb) { uint64_t off; memcpy(&off, r + 0x10, 8); off = (uint64_t)((int64_t)off + delta); memcpy(r + 0x10, &off, 8); continue; }
            by_align(out, 16, 0);
            uint64_t at = out->n;
            lz4_stored(out, nb->p, nb->n);
            uint16_t codec = 2;
            uint32_t size = (uint32_t)nb->n, packed = (uint32_t)(out->n - at);
            memcpy(r + 2, &codec, 2), memcpy(r + 4, &size, 4), memcpy(r + 8, &packed, 4), memcpy(r + 0x10, &at, 8);
        }
        memcpy(out->p, p->d, 0x10);
        memcpy(out->p + 0x10, &nf2, 4), memcpy(out->p + 0x14, &nd, 4), memcpy(out->p + 0x18, &ft2, 8), memcpy(out->p + 0x20, &h2, 8);
        for (int f = 0; f < nd; f++) memcpy(out->p + 0x28 + 8 * (size_t)f, &fo2[f], 8);
        memcpy(out->p + ft2, recs, 0x18 * (size_t)nf2);
        free(fo2);
    }
    for (int f = 0; f < nd; f++) free(fname[f]), free(fidx[f]);
    free(fname), free(fidx), free(fh), free(fcnt), free(recs), free(ph);
    return ok;
}

static bool write_whole(const char *path, const uint8_t *d, size_t n) {
    char dir[1100];
    snprintf(dir, sizeof dir, "%s", path);
    for (char *q = dir + 1; *q; q++) if (*q == '/') { *q = 0; xmkdir(dir); *q = '/'; }
    char tmp[1110];
    snprintf(tmp, sizeof tmp, "%s.part", path);
    FILE *f = fopen(tmp, "wb");
    bool ok = f && fwrite(d, 1, n, f) == n;
    if (f) ok &= fclose(f) == 0;
    remove(path);
    return ok && rename(tmp, path) == 0;
}

bool romfs_image_file(const char *path, int *fd, uint64_t *off, uint64_t *size);  // runtime/services/fs.c
static uint8_t *romfs_read(const char *path, size_t *n) {  // (the extracted folder of a source checkout, else romfs.bin)
    char loose[1100];
    snprintf(loose, sizeof loose, "%s/%s", g_romfs_dir, path);
    FILE *f = getenv("LARECOMP_TREES_IMAGE") ? NULL : fopen(loose, "rb");  // (tests: =1 reads romfs.bin as an install does)
    if (f) {
        uint64_t sz = xsize(f);
        fclose(f);
        *n = (size_t)sz;
        return read_range(loose, 0, sz);
    }
    int fd;
    uint64_t off, size;
    if (!romfs_image_file(path, &fd, &off, &size)) return NULL;
    uint8_t *d = malloc(size ? size : 1);
    if (!d || pread(fd, d, (size_t)size, (off_t)off) != (ssize_t)size) { free(d); return NULL; }
    *n = (size_t)size;
    return d;
}

static const char *g_trees_mod;  // (--build-trees: the mod; main.c)
#include "mods.h"
static void build_trees(Cpu *c) {
    char pf[64], path[1100];
    if (!mod_ini_get(g_trees_mod, "tree_patch", pf, sizeof pf)) { fprintf(stderr, "[trees] %s: no tree_patch\n", g_trees_mod); _exit(2); }
    snprintf(path, sizeof path, "mods/%s/%s", g_trees_mod, pf);
    FILE *f = fopen(path, "rb");
    uint64_t pn = f ? xsize(f) : 0;
    if (f) fclose(f);
    uint8_t *pd = pn ? read_range(path, 0, pn) : NULL;
    if (!pd || pn < 8 || memcmp(pd, "LFT1", 4)) { fprintf(stderr, "[trees] %s: not a tree patch\n", path); _exit(2); }
    uint32_t nops;
    memcpy(&nops, pd + 4, 4);
    TreeOp *ops = calloc(nops ? nops : 1, sizeof *ops);
    size_t o = 8;
    for (uint32_t i = 0; i < nops; i++) {
        TreeOp *op = &ops[i];
        if (o + 1 > pn) _exit(2);
        op->kind = pd[o++];
        if (!rd_str(pd, pn, &o, op->pack, sizeof op->pack) || !rd_str(pd, pn, &o, op->path, sizeof op->path) ||
            !rd_str(pd, pn, &o, op->tmpl, sizeof op->tmpl) || !rd_str(pd, pn, &o, op->model, sizeof op->model) ||
            !rd_str(pd, pn, &o, op->trtrr, sizeof op->trtrr) || o + 4 > pn) { fprintf(stderr, "[trees] patch damaged\n"); _exit(2); }
        memcpy(&op->n, pd + o, 4), o += 4;
        if (o + (uint64_t)op->n * 64 > pn) { fprintf(stderr, "[trees] patch damaged\n"); _exit(2); }
        op->mats = (const float *)(pd + o), o += (size_t)op->n * 64;
    }
    uint8_t *dec_mem = calloc(1, 0x80000);
    int packs = 0, trees = 0, newfiles = 0, failed = 0;
    bool *done = calloc(nops ? nops : 1, 1);
    for (uint32_t s = 0; s < nops; s++) {  // pack by pack
        if (done[s]) continue;
        const char *pack = ops[s].pack;
        size_t pkn;
        uint8_t *pk = romfs_read(pack, &pkn);
        Pak p = {pk, pkn, 0, 0, 0, 0};
        if (pk && pkn >= 0x28 && !memcmp(pk, "GFLXPACK", 8)) {
            memcpy(&p.nf, pk + 0x10, 4), memcpy(&p.nd, pk + 0x14, 4), memcpy(&p.ftab, pk + 0x18, 8), memcpy(&p.hashes, pk + 0x20, 8);
            if (p.nf < 0 || p.nd <= 0 || p.ftab + 0x18 * (uint64_t)p.nf > pkn || p.hashes + 8 * (uint64_t)p.nf > pkn) p.nf = -1;
        } else p.nf = -1;
        if (p.nf < 0) { fprintf(stderr, "[trees] %s: not readable\n", pack); failed++; for (uint32_t i = s; i < nops; i++) if (!strcmp(ops[i].pack, pack)) done[i] = 1; free(pk); continue; }
        Bytes **repl = calloc((size_t)p.nf, sizeof *repl);
        NewEnt *add = calloc(nops, sizeof *add);
        int nadd = 0, sibling = -1;
        for (uint32_t i = s; i < nops; i++) {
            TreeOp *op = &ops[i];
            if (done[i] || strcmp(op->pack, pack)) continue;
            done[i] = 1;
            if (op->kind == 0) {
                int e = pak_find(&p, fnv64(op->path, strlen(op->path)));
                if (e < 0) { failed++; continue; }
                Bytes *cur = repl[e];
                size_t n0;
                uint8_t *d0 = cur ? NULL : pak_entry(c, dec_mem, &p, e, &n0);
                Bytes nb = {0};
                if ((cur || d0) && trins_with(cur ? cur->p : d0, cur ? cur->n : n0, true, op->mats, op->n, &nb)) {
                    if (cur) free(cur->p); else repl[e] = calloc(1, sizeof(Bytes));
                    *repl[e] = nb, trees += (int)op->n;
                } else failed++, free(nb.p);
                free(d0);
            } else {
                int te = pak_find(&p, fnv64(op->tmpl, strlen(op->tmpl))), re = pak_find(&p, fnv64(op->trtrr, strlen(op->trtrr)));
                size_t tn, rn;
                uint8_t *td = te >= 0 ? pak_entry(c, dec_mem, &p, te, &tn) : NULL;
                if (re >= 0 && !repl[re]) {
                    uint8_t *rd = pak_entry(c, dec_mem, &p, re, &rn);
                    if (rd) { repl[re] = calloc(1, sizeof(Bytes)); by_put(repl[re], rd, rn); free(rd); }
                }
                const char *nm = strrchr(op->path, '/');
                nm = nm ? nm + 1 : op->path;
                Bytes nb = {0};
                if (td && re >= 0 && repl[re] && trins_with(td, tn, false, op->mats, op->n, &nb) && trtrr_add(repl[re], op->model, nm)) {
                    add[nadd].ph = fnv64(op->path, strlen(op->path)), add[nadd].nh = fnv64(nm, strlen(nm)), add[nadd].data = nb, nadd++;
                    if (sibling < 0) sibling = te;
                    trees += (int)op->n, newfiles++;
                } else failed++, free(nb.p);
                free(td);
            }
        }
        Bytes out = {0};
        if (sibling < 0) for (int i = 0; i < p.nf && sibling < 0; i++) if (repl[i]) sibling = i;
        char dst[1100];
        snprintf(dst, sizeof dst, "mods/%s/romfs/%s", g_trees_mod, pack);
        if (sibling >= 0 && pak_build(&p, repl, add, nadd, sibling, &out) && write_whole(dst, out.p, out.n)) packs++;
        else if (sibling >= 0) { fprintf(stderr, "[trees] %s: not written\n", dst); failed++; }
        free(out.p);
        for (int i = 0; i < p.nf; i++) if (repl[i]) free(repl[i]->p), free(repl[i]);
        for (int k = 0; k < nadd; k++) free(add[k].data.p);
        free(repl), free(add), free(pk);
    }
    fprintf(stderr, "[trees] %s: %d packs written, %d trees (%d new files), %d operations failed\n", g_trees_mod, packs, trees, newfiles, failed);
    _exit(packs && !failed ? 0 : 3);
}

// main.c, --build-trees <mod>: as extract_shaders_install
void build_trees_install(const char *exefs, const char *mod) {
    g_trees_mod = mod;
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
    if (!nn_main || !g_fn) { fprintf(stderr, "[trees] this game build has no known decoder\n"); _exit(2); }
    hle_override(nn_main, build_trees, "<build_trees>");
}

// Before the game (main.c): each active mod with a tree_patch gets its trees built from the player's files once - again
// when the patch or the game's RomFS changes (mods/<mod>/romfs/.trees: what it was built from)
#include "platform.h"
void trees_prepare(const char *root) {
    char names[MODS_MAX][128];
    int n = mods_active(names, MODS_MAX);
    for (int i = 0; i < n; i++) {
        char pf[64], pp[1100], img[1100], stamp_path[1100], want[256], have[256] = "";
        if (!mod_ini_get(names[i], "tree_patch", pf, sizeof pf)) continue;
        snprintf(pp, sizeof pp, "mods/%s/%s", names[i], pf);
        snprintf(img, sizeof img, "%s/romfs.bin", root);
        struct stat sp, si;
        if (stat(pp, &sp)) continue;
        if (stat(img, &si)) memset(&si, 0, sizeof si);
        snprintf(want, sizeof want, "1 %lld %lld %lld", (long long)sp.st_size, (long long)sp.st_mtime, (long long)si.st_size);
        snprintf(stamp_path, sizeof stamp_path, "mods/%s/romfs/.trees", names[i]);
        FILE *f = fopen(stamp_path, "r");
        if (f) { if (!fgets(have, sizeof have, f)) have[0] = 0; fclose(f); have[strcspn(have, "\n")] = 0; }
        if (!strcmp(want, have)) continue;
        char exe[1100], logp[1100];
        if (!plat_exe_path(exe, sizeof exe)) continue;
        snprintf(logp, sizeof logp, "mods/%s/build-trees.log", names[i]);
        uint64_t t0 = plat_now_ns();
        const char *argv[] = {exe, root, "--build-trees", names[i], NULL};
        PlatProc pr = plat_spawn(argv, logp, false);
        int rc = pr ? plat_proc_wait(pr, true) : -1;
        LOG("[trees] %s: %s (%.1f s, %s)\n", names[i], rc == 0 ? "built from the game's files" : "not built", (double)(plat_now_ns() - t0) / 1e9, logp);
        if (rc == 0 && (f = fopen(stamp_path, "w"))) { fprintf(f, "%s\n", want); fclose(f); }
        mods_forget();
    }
}
