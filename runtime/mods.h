// Mods: game files replaced or added from a folder, as Atmosphere's LayeredFS (fan translations come this way), and
// the game's language. runtime/mods.c; the start screen (runtime/ui/launcher_lg.mm) picks both.
//
//   mods/<name>/romfs/...                                  the game's RomFS paths (bin/message/English/...)
//   mods/<name>/texpack/<key>.png                          a texture pack (runtime/nvn/vulkan/texpack.c)
//   mods/<name>/atmosphere/contents/<title id>/romfs/...   (or any folder holding a romfs/ up to 4 levels down:
//                                                           a download unpacked as it came)
#pragma once
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MODS_MAX 32
// The folders under mods/ that hold a romfs/: names[i] (up to MODS_MAX). Returns how many.
int mods_list(char names[][128], int max);
// The mods that are on, in load order (LARECOMP_MOD, else g_settings.mod: names separated by commas; the first wins
// where two change the same file): names[i]. Returns how many.
int mods_active(char names[][128], int max);
// Their romfs/ folders, in that order (mods without one - a texture pack alone - left out). Returns how many.
int mod_romfs_dirs(const char *dirs[], int max);
const char *mod_romfs_dir(void);  // (the first of them, or NULL)
// Their texture packs, mods/<name>/texpack/ (<key>.dds / .png replacements: Legends Studio's texexport, tools/
// texpack_upscale.py, texpackbuild; runtime/nvn/vulkan/texpack.c loads them), in that order. Returns how many.
int mod_texpack_dirs(char dirs[][160], int max);
// path: a RomFS path ("bin/message/English/..."). True with the host file when a mod replaces it (the first that has it).
bool mod_romfs_override(const char *path, char *out, size_t cap);
// The language whose texts a mod replaces (its romfs/bin/message/<language>/), as a language code ("en-US"), or NULL
// (none, or several).
const char *mod_language(const char *name);
// Installs a mod from a file the player downloaded (.zip / .rar / .7z, unpacked with Windows' tar) or a folder, on a
// background thread: its romfs/ (Let's Go Eevee's first) is copied to mods/<file name>/romfs.
enum { MOD_INSTALL_IDLE, MOD_INSTALL_RUNNING, MOD_INSTALL_DONE, MOD_INSTALL_FAIL_UNPACK, MOD_INSTALL_FAIL_NOT_MOD };
void mod_install_start(const char *src);
int mod_install_state(void);
const char *mod_install_name(void);  // after MOD_INSTALL_DONE: the mods/ folder name
// The language the game is told (nn::settings / applet "desired language"): the active mod's, else g_settings.game_lang.
const char *game_language(void);

#ifdef __cplusplus
}
#endif
