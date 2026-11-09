// Texture packs (texpack.c): replacements for the game's textures, read on a loader thread.
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <vulkan/vulkan.h>

#define TEXPACK_MAX_LEVELS 16

typedef struct TexLoaded {
    uint32_t w, h, levels;
    VkFormat fmt;      // a .dds's BC7 / BC5 / BC4, or R8G8B8A8 (a PNG: level 0 only, the rest blitted down)
    bool rgba;         // (from a PNG: sRGB or not follows the game's texture)
    uint8_t *data;     // the levels back to back
    uint64_t level_off[TEXPACK_MAX_LEVELS], level_size[TEXPACK_MAX_LEVELS];
} TexLoaded;

bool texpack_active(void);             // a pack is on (LARECOMP_TEXPACK, or the active mod's texpack/)
bool texpack_has(uint64_t key);        // it has a replacement for that key
void texpack_request(uint64_t key);    // queued for the loader thread
bool texpack_take(uint64_t key, TexLoaded **out);  // the loader is done with it: *out the file, NULL if unreadable
void texpack_free(TexLoaded *r);
