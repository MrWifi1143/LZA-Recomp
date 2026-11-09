// Texture packs: the game's textures replaced, at any size, by PNG files named by a key of their content.
//
//   mods/<name>/texpack/<key>.png   (or .../texpack/textures/<key>.png) - the active mod's pack (runtime/mods.h);
//   LARECOMP_TEXPACK=<folder>       a pack folder for this run instead (tests, Legends Studio).
//
// <key> (16 hex digits) is textures.c's hash_full of the texture's level 0, layer 0 as deswizzled block rows, | 1 - the
// bytes the game uploads. Legends Studio's texexport writes every texture of the game under that name (formats/
// texture.cpp tex_pack_key, the same function) and tools/texpack_upscale.py makes the pack from the export (AI for
// colour, filters for normal maps and masks). When the game first uploads a plain 2D texture (not a render target, not
// one the CPU changes, one layer) whose key is in the pack, its image becomes an RGBA8 one of the PNG's size - sRGB when
// the original was - with its mips generated, and the game's data isn't uploaded into it again. The texture's views
// follow (textures.c: a backend whose format isn't the game's keeps its own), so shaders sample the new image; a shader
// reading the texture's size (textureSize / texelFetch) would see the new one - material textures don't.
// Framework: PNGs are decoded on the render thread when first used (a hitch per texture), and kept as RGBA8 (4 bytes a
// pixel where BC7 had 1). Next: background loading + BC7 encoding (or a pre-encoded cache next to the pack).
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL.h>

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

static struct { uint64_t key; char *path; } *g_ent;
static int g_n = -1;
static uint64_t g_hits;

static int cmp_ent(const void *a, const void *b) {
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return x < y ? -1 : x > y;
}

static void index_dir(const char *dir) {
    int count = 0;
    char **names = SDL_GlobDirectory(dir, "*.png", 0, &count);
    if (!names) return;
    for (int i = 0; i < count; i++) {
        const char *nm = names[i];
        if (strlen(nm) < 20) continue;
        char hex[17];
        memcpy(hex, nm, 16);
        hex[16] = 0;
        char *end;
        uint64_t key = strtoull(hex, &end, 16);
        if (*end || !key) continue;
        g_ent = realloc(g_ent, (size_t)(g_n + 1) * sizeof *g_ent);
        size_t n = strlen(dir) + strlen(nm) + 2;
        g_ent[g_n].key = key;
        g_ent[g_n].path = malloc(n);
        snprintf(g_ent[g_n].path, n, "%s/%s", dir, nm);
        g_n++;
    }
    SDL_free(names);
}

static void texpack_init(void) {
    g_n = 0;
    const char *e = getenv("LARECOMP_TEXPACK");
    const char *dir = e && *e ? e : mod_texpack_dir();
    if (!dir) return;
    char sub[1100];
    index_dir(dir);
    snprintf(sub, sizeof sub, "%s/textures", dir);
    index_dir(sub);
    if (g_n) qsort(g_ent, (size_t)g_n, sizeof *g_ent, cmp_ent);
    LOG("[texpack] %s: %d replacement textures\n", dir, g_n);
}

bool texpack_active(void) {
    if (g_n < 0) texpack_init();
    return g_n > 0;
}

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

// The replacement of the texture with that key: RGBA8 pixels (malloc'd, the caller frees) and their size, or NULL
uint8_t *texpack_load(uint64_t key, uint32_t *w, uint32_t *h) {
    if (!texpack_active()) return NULL;
    int lo = 0, hi = g_n - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (g_ent[mid].key == key) {
            size_t size = 0;
            uint8_t *f = SDL_LoadFile(g_ent[mid].path, &size);
            uint8_t *px = f ? png_decode(f, size, w, h) : NULL;
            SDL_free(f);
            if (!px) LOG("[texpack] %s: not a PNG this reads (8 bits a channel, not interlaced)\n", g_ent[mid].path);
            else if (g_hits++ < 20 || g_hits % 500 == 0) LOG("[texpack] %016llx replaced: %ux%u (%llu so far)\n", (unsigned long long)key, *w, *h, (unsigned long long)g_hits);
            return px;
        }
        if (g_ent[mid].key < key) lo = mid + 1;
        else hi = mid - 1;
    }
    return NULL;
}
