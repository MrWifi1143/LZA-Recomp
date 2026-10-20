// Button prompts for the controller in use (runtime/nvn/glyphs.c)
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// A texture's level 0 as uploaded (deswizzled blocks, `hash` = its content hash | 1): 0 not a button icon, 1 a button
// icon left as it is, 2 a button icon whose symbol was redrawn in place.
int glyphs_patch(uint64_t hash, uint32_t format, uint32_t w, uint32_t h, uint8_t *lin);
// Changes whenever the icons would be drawn differently: textures that are button icons are uploaded again then.
uint32_t glyphs_generation(void);

#ifdef __cplusplus
}
#endif
