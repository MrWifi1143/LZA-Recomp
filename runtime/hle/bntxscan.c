// Research aid: LARECOMP_BNTX_SCAN="<present>[,<present>...]": at those presents, every texture file (BNTX) the game
// holds in its heap is read in place - its textures' names, sizes, formats and the hash the renderer gives their
// level 0 (runtime/nvn/vulkan/textures.c vk_tex_content_hash of the deswizzled block rows), logged as "[bntx]". Names
// textures that only ever reach the GPU as hashes (the button icons of runtime/nvn/vulkan/glyphs.c: the game's
// archives are Oodle-compressed, so they can't be read from the RomFS). LARECOMP_BNTX_DUMP=<name part> also writes
// the level 0 of the textures whose name has it to build/tex/bntx/<name>_<w>x<h>_f<format>.bin (deswizzled blocks).
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "../hle.h"
#include "../nvn/tegra.h"

uint64_t vk_tex_content_hash(const uint8_t *p, uint64_t n);  // runtime/nvn/vulkan/textures.c

static const uint8_t *g_heap;
static uint64_t g_lo, g_hi;  // the blob being read: pointers in it are absolute (relocated) or offsets from its start

static const uint8_t *at(uint64_t p, uint64_t n) {
    if (p >= HEAP_BASE && p + n <= HEAP_BASE + HEAP_MAX) return g_heap + (p - HEAP_BASE);
    if (p + n <= g_hi - g_lo) return g_heap + (g_lo - HEAP_BASE) + p;
    return NULL;
}
static uint64_t u64(const uint8_t *p) { uint64_t v; memcpy(&v, p, 8); return v; }
static uint32_t u32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static uint16_t u16(const uint8_t *p) { uint16_t v; memcpy(&v, p, 2); return v; }
static bool has(const uint8_t *s, size_t n, const char *part) {  // (memmem: not on Windows)
    size_t k = strlen(part);
    for (size_t i = 0; k && i + k <= n; i++) if (!memcmp(s + i, part, k)) return true;
    return false;
}

// BNTX format (high byte: kind, low byte: type) -> bytes per 4x4 block (or per texel for the plain ones), block width
static bool blocks_of(uint32_t fmt, uint32_t *bpb, uint32_t *bw) {
    switch (fmt >> 8) {
    case 0x1a: case 0x1d: *bpb = 8, *bw = 4; return true;                         // BC1, BC4
    case 0x1b: case 0x1c: case 0x1e: case 0x1f: case 0x20: *bpb = 16, *bw = 4; return true;  // BC2, BC3, BC5, BC6, BC7
    case 0x0b: *bpb = 4, *bw = 1; return true;                                    // RGBA8
    case 0x02: *bpb = 1, *bw = 1; return true;                                    // R8
    case 0x07: *bpb = 2, *bw = 1; return true;                                    // RG8
    default: return false;
    }
}

static void read_bntx(uint64_t base, int *ntex) {
    const uint8_t *h = g_heap + (base - HEAP_BASE);
    uint32_t size = u32(h + 0x1C);  // file size
    if (size < 0x40 || size > 0x10000000 || base + size > HEAP_BASE + HEAP_MAX || memcmp(h + 0x20, "NX  ", 4)) return;
    g_lo = base, g_hi = base + size;
    uint32_t cnt = u32(h + 0x24);
    const uint8_t *arr = at(u64(h + 0x28), (uint64_t)cnt * 8);
    if (!arr || cnt > 4096) return;
    for (uint32_t i = 0; i < cnt; i++) {
        const uint8_t *t = at(u64(arr + 8 * i), 0x80);
        if (!t || memcmp(t, "BRTI", 4)) continue;
        uint16_t tile = u16(t + 0x12), mips = u16(t + 0x16);
        uint32_t fmt = u32(t + 0x1C), w = u32(t + 0x24), hh = u32(t + 0x28), dep = u32(t + 0x2C), blk = u32(t + 0x34);
        const uint8_t *np = at(u64(t + 0x60), 2);
        const uint8_t *pp = at(u64(t + 0x70), 8);
        if (!np || !pp) continue;
        uint16_t nl = u16(np);
        const uint8_t *name = at(u64(t + 0x60) + 2, nl);
        if (!name) continue;
        uint32_t bpb, bw;
        uint64_t hash = 0;
        if (w && hh && w <= 256 && hh <= 256 && dep <= 1 && tile == 0 && blocks_of(fmt, &bpb, &bw)) {
            uint32_t wb = (w + bw - 1) / bw, hb = (hh + bw - 1) / bw;
            uint64_t gh = 8ull << blk;  // block linear: 64-byte x 8-row GOBs, blocks of 2^blk GOBs high
            uint64_t need = (wb * bpb + 63) / 64 * ((hb + gh - 1) / gh) * 512 * (1ull << blk);
            const uint8_t *src = at(u64(pp), need);
            if (src) {
                uint8_t *lin = malloc((uint64_t)wb * hb * bpb);
                tegra_deswizzle(src, lin, wb, hb, 1, bpb, blk, 0);
                hash = vk_tex_content_hash(lin, (uint64_t)wb * hb * bpb) | 1;
                const char *want = getenv("LARECOMP_BNTX_DUMP");
                if (want && has(name, nl, want)) {
                    char path[300];
                    mkdir("build/tex", 0755);
                    mkdir("build/tex/bntx", 0755);
                    snprintf(path, sizeof path, "build/tex/bntx/%.*s_%ux%u_f%x.bin", (int)(nl < 120 ? nl : 120), (const char *)name, w,
                             hh, fmt);
                    for (char *c = path + 15; *c; c++) if (*c == '^') *c = '-';
                    FILE *f = fopen(path, "wb");
                    if (f) { fwrite(lin, 1, (uint64_t)wb * hb * bpb, f); fclose(f); }
                }
                free(lin);
            }
        }
        LOG("[bntx] %016llx %ux%u fmt %#x mips %u %.*s (file %#llx)\n", (unsigned long long)hash, w, hh, fmt, mips, (int)nl,
            (const char *)name, (unsigned long long)base);
        (*ntex)++;
    }
}

void bntxscan_present(uint64_t frame) {
    static char *spec;
    static int init;
    if (!init) {
        init = 1;
        const char *e = getenv("LARECOMP_BNTX_SCAN");
        if (e) spec = strdup(e);
    }
    if (!spec) return;
    bool now = false;
    for (char *s = spec; *s;) {
        if ((uint64_t)strtoull(s, &s, 10) == frame) now = true;
        while (*s == ',') s++;
        if (*s && (*s < '0' || *s > '9')) break;
    }
    if (!now) return;
    g_heap = (const uint8_t *)(uintptr_t)HEAP_BASE;
    int files = 0, ntex = 0;
    static const uint64_t magic = 0x58544E42ull;  // "BNTX\0\0\0\0"
    for (uint64_t off = 0; off + 0x40 <= HEAP_MAX; off += 8) {
        if (u64(g_heap + off) != magic) continue;
        files++;
        read_bntx(HEAP_BASE + off, &ntex);
    }
    LOG("[bntx] present %llu: %d texture files, %d textures\n", (unsigned long long)frame, files, ntex);
}
