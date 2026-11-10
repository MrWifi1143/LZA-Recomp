// Texture packs: the game's textures replaced, at any size, by files named by a key of their content.
//
//   mods/<name>/texpack/<key>.dds|.png   (or .../texpack/textures/) - the active mod's pack (runtime/mods.h);
//   LARECOMP_TEXPACK=<folder>            a pack folder for this run instead (tests, Legends Studio).
//
// <key> (16 hex digits) is textures.c's hash_full of the texture's level 0, layer 0 as deswizzled block rows, | 1 - the
// bytes the game uploads. Legends Studio's texexport writes every texture of the game under that name (formats/
// texture.cpp tex_pack_key, the same function), tools/texpack_upscale.py makes the pack from the export (AI for colour,
// filters for normal maps and masks) and the CLI's texpackbuild compresses it: <key>.dds, BC7 / BC5 / BC4 with all mips,
// which this uploads as it is (a .png - RGBA8, mips blitted - is the fallback, for hand-made replacements).
// When the game first uploads a plain 2D texture (not a render target, not one the CPU changes, one layer) whose key is in
// the pack, a loader thread reads the file; the game's own texture shows until it's ready, then the texture's image is
// the pack's (textures.c texpack_apply) and the game's data isn't uploaded into it again. The views follow (a backend
// whose format isn't the game's keeps its own), so shaders sample the new image; a shader reading the texture's size
// (textureSize / texelFetch) would see the new one - material textures don't.
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL.h>

#include "vk.h"
#include "texpack.h"
#include "../../rt.h"
#include "../../mods.h"

#define MINIZ_NO_STDIO
#define MINIZ_NO_ARCHIVE_APIS
#define MINIZ_NO_TIME
#if defined(__clang__) || defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wall"
#pragma GCC diagnostic ignored "-Wextra"
#endif
#include "miniz/miniz.c"
#if defined(__clang__) || defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

typedef struct { uint64_t key; char *path; bool dds; int prio; } Ent;  // (prio: the mod's place in the load order)
static Ent *g_ent;
static int g_n = -1;
static uint64_t g_hits;

static int cmp_ent(const void *a, const void *b) {
    const Ent *x = a, *y = b;
    if (x->key != y->key) return x->key < y->key ? -1 : 1;
    if (x->prio != y->prio) return x->prio - y->prio;  // (the first mod's)
    return (int)y->dds - (int)x->dds;  // (its .dds before its .png)
}

static void index_dir(const char *dir, int prio) {
    int count = 0;
    char **names = SDL_GlobDirectory(dir, "*", 0, &count);
    if (!names) return;
    for (int i = 0; i < count; i++) {
        const char *nm = names[i];
        size_t len = strlen(nm);
        bool dds = len == 20 && !SDL_strcasecmp(nm + 16, ".dds"), png = len == 20 && !SDL_strcasecmp(nm + 16, ".png");
        if (!dds && !png) continue;
        char hex[17];
        memcpy(hex, nm, 16);
        hex[16] = 0;
        char *end;
        uint64_t key = strtoull(hex, &end, 16);
        if (*end || !key) continue;
        g_ent = realloc(g_ent, (size_t)(g_n + 1) * sizeof *g_ent);
        size_t n = strlen(dir) + len + 2;
        g_ent[g_n] = (Ent){key, malloc(n), dds, prio};
        snprintf(g_ent[g_n].path, n, "%s/%s", dir, nm);
        g_n++;
    }
    SDL_free(names);
}

static void loader_start(void);
static void texpack_init(void) {
    g_n = 0;
    const char *e = ENV("LARECOMP_TEXPACK");  // (a folder for this run, in place of the mods' packs)
    char dirs[MODS_MAX][160];
    int nd = 0;
    if (e && *e) snprintf(dirs[nd++], sizeof dirs[0], "%s", e);
    else nd = mod_texpack_dirs(dirs, MODS_MAX);  // (every mod that's on with a texpack/, in load order)
    for (int d = 0; d < nd; d++) {
        char sub[200];
        int before = g_n;
        index_dir(dirs[d], d);
        snprintf(sub, sizeof sub, "%s/textures", dirs[d]);
        index_dir(sub, d);
        LOG("[texpack] %s: %d files\n", dirs[d], g_n - before);
    }
    if (g_n) qsort(g_ent, (size_t)g_n, sizeof *g_ent, cmp_ent);
    int dds = 0, keys = 0;
    for (int i = 0; i < g_n; i++) dds += g_ent[i].dds, keys += !i || g_ent[i].key != g_ent[i - 1].key;
    if (nd) LOG("[texpack] %d replacement textures from %d pack%s (%d .dds files)\n", keys, nd, nd == 1 ? "" : "s", dds);
    if (g_n) loader_start();
}

bool texpack_active(void) {
    if (g_n < 0) texpack_init();
    return g_n > 0;
}

static const Ent *find(uint64_t key) {  // (the first mod's, its .dds when it has one: sorted first)
    int lo = 0, hi = g_n - 1, at = -1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (g_ent[mid].key < key) lo = mid + 1;
        else { if (g_ent[mid].key == key) at = mid; hi = mid - 1; }
    }
    return at >= 0 ? &g_ent[at] : NULL;
}

bool texpack_has(uint64_t key) { return texpack_active() && find(key); }

// The PNG (8-bit gray / gray-alpha / RGB / RGBA / palette, not interlaced: what the pack tools write) as RGBA8
static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
static uint8_t *png_decode(const uint8_t *f, size_t size, uint32_t *ow, uint32_t *oh) {
    static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', 13, 10, 26, 10};
    if (size < 33 || memcmp(f, sig, 8)) return NULL;
    uint32_t w = 0, h = 0;
    int depth = 0, ctype = 0, interlace = 0;
    uint8_t *idat = NULL, plte[768];
    size_t nidat = 0, nplte = 0;
    for (size_t at = 8; at + 12 <= size;) {
        uint32_t len = be32(f + at);
        if (at + 12 + (uint64_t)len > size) break;
        const uint8_t *d = f + at + 8;
        if (!memcmp(f + at + 4, "IHDR", 4) && len >= 13) w = be32(d), h = be32(d + 4), depth = d[8], ctype = d[9], interlace = d[12];
        else if (!memcmp(f + at + 4, "PLTE", 4)) nplte = len < sizeof plte ? len : sizeof plte, memcpy(plte, d, nplte);
        else if (!memcmp(f + at + 4, "IDAT", 4)) {
            idat = realloc(idat, nidat + len);
            memcpy(idat + nidat, d, len);
            nidat += len;
        } else if (!memcmp(f + at + 4, "IEND", 4)) break;
        at += 12 + (size_t)len;
    }
    int ch = ctype == 0 ? 1 : ctype == 2 ? 3 : ctype == 3 ? 1 : ctype == 4 ? 2 : ctype == 6 ? 4 : 0;
    if (depth != 8 || interlace || !ch || !w || !h || w > 16384 || h > 16384) { free(idat); return NULL; }
    size_t stride = (size_t)w * ch, raw_n = (stride + 1) * h;
    uint8_t *raw = malloc(raw_n), *img = malloc(stride * h), *out = malloc((size_t)w * h * 4);
    mz_ulong n = (mz_ulong)raw_n;
    bool ok = raw && img && out && mz_uncompress(raw, &n, idat, (mz_ulong)nidat) == MZ_OK && n == raw_n;
    free(idat);
    if (!ok) { free(raw); free(img); free(out); return NULL; }
    for (uint32_t y = 0; y < h; y++) {
        const uint8_t *s = raw + y * (stride + 1) + 1, *prev = y ? img + (y - 1) * stride : NULL;
        uint8_t ft = s[-1], *o = img + y * stride;
        for (size_t x = 0; x < stride; x++) {
            int a = x >= (size_t)ch ? o[x - ch] : 0, b = prev ? prev[x] : 0, c = prev && x >= (size_t)ch ? prev[x - ch] : 0, v = s[x];
            if (ft == 1) v += a;
            else if (ft == 2) v += b;
            else if (ft == 3) v += (a + b) / 2;
            else if (ft == 4) {
                int p = a + b - c, pa = abs(p - a), pb = abs(p - b), pc = abs(p - c);
                v += pa <= pb && pa <= pc ? a : pb <= pc ? b : c;
            }
            o[x] = (uint8_t)v;
        }
    }
    free(raw);
    for (size_t i = 0; i < (size_t)w * h; i++) {
        const uint8_t *p = img + i * ch;
        uint8_t *o = out + i * 4;
        switch (ctype) {
        case 0: o[0] = o[1] = o[2] = p[0], o[3] = 255; break;
        case 4: o[0] = o[1] = o[2] = p[0], o[3] = p[1]; break;
        case 2: o[0] = p[0], o[1] = p[1], o[2] = p[2], o[3] = 255; break;
        case 3: {
            size_t k = (size_t)p[0] * 3;
            o[0] = k + 2 < nplte ? plte[k] : 0, o[1] = k + 2 < nplte ? plte[k + 1] : 0, o[2] = k + 2 < nplte ? plte[k + 2] : 0, o[3] = 255;
            break;
        }
        default: memcpy(o, p, 4); break;
        }
    }
    free(img);
    *ow = w, *oh = h;
    return out;
}


// A .dds with a DX10 header (texpackbuild writes them): BC7 98 / 99 sRGB, BC5 83, BC4 80; all levels back to back
static TexLoaded *dds_read(uint8_t *f, size_t size) {
    if (size < 148 || memcmp(f, "DDS ", 4) || memcmp(f + 84, "DX10", 4)) return NULL;
    uint32_t h, w, mips, dxgi, dim, arr;
    memcpy(&h, f + 12, 4), memcpy(&w, f + 16, 4), memcpy(&mips, f + 28, 4);
    memcpy(&dxgi, f + 128, 4), memcpy(&dim, f + 132, 4), memcpy(&arr, f + 140, 4);
    VkFormat vf = dxgi == 98 ? VK_FORMAT_BC7_UNORM_BLOCK : dxgi == 99 ? VK_FORMAT_BC7_SRGB_BLOCK : dxgi == 83 ? VK_FORMAT_BC5_UNORM_BLOCK
                : dxgi == 80 ? VK_FORMAT_BC4_UNORM_BLOCK : VK_FORMAT_UNDEFINED;
    uint32_t bpb = dxgi == 80 ? 8 : 16;
    if (vf == VK_FORMAT_UNDEFINED || dim != 3 || arr > 1 || !w || !h || w > 16384 || h > 16384) return NULL;
    if (!mips) mips = 1;
    if (mips > TEXPACK_MAX_LEVELS) mips = TEXPACK_MAX_LEVELS;
    TexLoaded *r = calloc(1, sizeof *r);
    r->w = w, r->h = h, r->fmt = vf;
    uint64_t off = 148;
    for (uint32_t lv = 0; lv < mips; lv++) {
        uint32_t lw = w >> lv ? w >> lv : 1, lh = h >> lv ? h >> lv : 1;
        uint64_t n = (uint64_t)((lw + 3) / 4) * ((lh + 3) / 4) * bpb;
        if (off + n > size) break;
        r->level_off[lv] = off - 148, r->level_size[lv] = n;
        r->levels = lv + 1;
        off += n;
    }
    if (!r->levels) { free(r); return NULL; }
    r->data = malloc(off - 148);
    memcpy(r->data, f + 148, off - 148);
    return r;
}

static TexLoaded *load_file(const Ent *e) {
    size_t size = 0;
    uint8_t *f = SDL_LoadFile(e->path, &size);
    if (!f) return NULL;
    TexLoaded *r = NULL;
    if (e->dds) r = dds_read(f, size);
    else {
        uint32_t w, h;
        uint8_t *px = png_decode(f, size, &w, &h);
        if (px) {
            r = calloc(1, sizeof *r);
            r->w = w, r->h = h, r->fmt = VK_FORMAT_R8G8B8A8_UNORM, r->rgba = true, r->data = px;
            r->levels = 1, r->level_size[0] = (uint64_t)w * h * 4;
        }
    }
    SDL_free(f);
    if (!r) LOG("[texpack] %s: not a file this reads (DX10 .dds BC7 / BC5 / BC4, or an 8-bit PNG)\n", e->path);
    return r;
}

// ---- the loader thread: keys asked for (texpack_request) in, loaded files out (texpack_take)
#define QMAX 4096
static SDL_Mutex *g_lock;
static SDL_Condition *g_cond;
static uint64_t g_queue[QMAX];
static int g_qhead, g_qtail;
typedef struct Done { uint64_t key; TexLoaded *r; struct Done *next; } Done;
static Done *g_done;

static int loader(void *arg) {
    (void)arg;
    for (;;) {
        SDL_LockMutex(g_lock);
        while (g_qhead == g_qtail) SDL_WaitCondition(g_cond, g_lock);
        uint64_t key = g_queue[g_qhead];
        g_qhead = (g_qhead + 1) % QMAX;
        SDL_UnlockMutex(g_lock);
        const Ent *e = find(key);
        TexLoaded *r = e ? load_file(e) : NULL;
        Done *d = calloc(1, sizeof *d);
        d->key = key, d->r = r;
        SDL_LockMutex(g_lock);
        d->next = g_done, g_done = d;
        SDL_UnlockMutex(g_lock);
    }
    return 0;
}

static void loader_start(void) {
    g_lock = SDL_CreateMutex();
    g_cond = SDL_CreateCondition();
    for (int i = 0; i < 2; i++) SDL_DetachThread(SDL_CreateThread(loader, "texpack", NULL));
}

void texpack_request(uint64_t key) {
    SDL_LockMutex(g_lock);
    if ((g_qtail + 1) % QMAX != g_qhead) g_queue[g_qtail] = key, g_qtail = (g_qtail + 1) % QMAX;
    SDL_SignalCondition(g_cond);
    SDL_UnlockMutex(g_lock);
}

// done: true when the loader is finished with that key (*out: the file, or NULL when it didn't read)
bool texpack_take(uint64_t key, TexLoaded **out) {
    bool done = false;
    SDL_LockMutex(g_lock);
    for (Done **p = &g_done; *p; p = &(*p)->next)
        if ((*p)->key == key) {
            Done *d = *p;
            *p = d->next;
            *out = d->r, done = true;
            free(d);
            break;
        }
    SDL_UnlockMutex(g_lock);
    if (done && *out && (g_hits++ < 20 || g_hits % 500 == 0))
        LOG("[texpack] %016llx replaced: %ux%u, %u levels%s (%llu so far)\n", (unsigned long long)key, (*out)->w, (*out)->h,
            (*out)->levels, (*out)->rgba ? ", from a PNG" : "", (unsigned long long)g_hits);
    return done;
}

void texpack_free(TexLoaded *r) {
    if (!r) return;
    free(r->data);
    free(r);
}
