// Game files straight from the player's own dumped packages (runtime/nxpkg.c), the way the emulators read them: the
// .nsp / .xci of the base game, its update and its add-on content, opened with the player's keys (prod.keys, dumped
// from their own Switch). Only what the runtime uses is written out: the program's exefs files, the RomFS image (the
// update's patch applied over the base game's RomFS) and the add-on content's RomFS image.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    NXPKG_OK,
    NXPKG_NO_KEYS,       // no prod.keys found (or no usable header key in it)
    NXPKG_KEY_MISSING,   // keys older than the game (its master key generation) or no title key for it
    NXPKG_NO_GAME,       // packages found, but not the base game of a wanted title
    NXPKG_ONLY_UPDATE,   // a wanted title's update (or add-on content) without its base game
    NXPKG_COMPRESSED,    // .nsz / .xcz (compressed NCAs): not read
    NXPKG_UNSUPPORTED,   // a layout this reader doesn't know (sparse / compressed sections...)
    NXPKG_READ_ERROR,    // unreadable / damaged package, or the output couldn't be written
};

typedef struct NxPkg NxPkg;

// Scans `path` (a package file, or a folder searched a few levels deep) for .nsp / .xci and finds the keys (the folder
// itself, ~/.switch, the emulators' key folders). titles: the wanted base program ids, best first, 0-terminated.
// NULL = failed: *err (NXPKG_*) and why (a short message for the log).
NxPkg *nxpkg_open(const char *path, const uint64_t *titles, int *err, char *why, size_t why_size);
// Given on the command line (runtime/main.c: --keys, the --game paths after the first - a launcher's setup step): a
// prod.keys file tried before the usual places, and more packages read along with `path` (the update's .nsp, the DLC's).
void nxpkg_set_keys(const char *file);
void nxpkg_add_path(const char *path);
void nxpkg_close(NxPkg *p);
// True when `path` holds .nsp / .xci / .nsz / .xcz (a file, or a folder searched a few levels deep).
bool nxpkg_any(const char *path);
// The base game picked (its program id).
uint64_t nxpkg_title(const NxPkg *p);
// Updates found for it, newest first (index 1..count; 0 = the base game alone), and their versions (v65536 = 1.0.1...).
int nxpkg_update_count(const NxPkg *p);
uint32_t nxpkg_update_version(const NxPkg *p, int upd);
// Writes the program's exefs files (with update upd, 0 = none) into the folder dir (created).
bool nxpkg_write_exefs(NxPkg *p, int upd, const char *dir, char *why, size_t why_size);
// Writes the RomFS image (the base game's, with update upd's patch applied) to the file out. progress may be NULL.
bool nxpkg_write_romfs(NxPkg *p, int upd, const char *out, void (*progress)(uint64_t done, uint64_t total), char *why,
                       size_t why_size);
// Writes the add-on content's RomFS image (the first found for the title) to out; false = none found or failed (also
// when its file is damaged or altered: the checks below).
bool nxpkg_write_dlc(NxPkg *p, const char *out, char *why, size_t why_size);

// What nxpkg_write_romfs checked while it read and wrote (all in the same pass, logged): each NCA it used, hashed
// whole as stored (SHA-256) against its name - the content id, the hash's first 16 bytes - and against the record its
// meta has of it; and the image against its section's hash tree (IVFC). base / update: the base game's program NCA (and
// the meta naming it) / the update's; image: the RomFS image written. A damaged or altered NCA is BAD; NCAs that are
// fine but an image that isn't: the base game isn't the one the update was made for, or a packing this doesn't read.
enum {
    NXPKG_CHECK_NONE,     // not checked (not used, or no checks: out of memory)
    NXPKG_CHECK_OK,
    NXPKG_CHECK_BAD,
    NXPKG_CHECK_UNNAMED,  // hashed, but its file name isn't a content id (renamed) and no meta names it: nothing to compare
    NXPKG_CHECK_UNREAD,   // a read error while hashing
};
typedef struct {
    int base, update, image;                       // NXPKG_CHECK_*
    char base_file[256], update_file[256];         // the packages they're in (file names)
    char detail[256];                              // the image check's result
} NxCheck;
const NxCheck *nxpkg_check(const NxPkg *p);
// For a report when the files are fine but the image isn't (a packing to fix the reader for): the NCAs' headers,
// section tables, key generations, the update's patch tables, and the image's RomFS header and where its tables came
// from (romfs: the image written).
void nxpkg_log_details(NxPkg *p, int upd, const char *romfs);

#ifdef __cplusplus
}
#endif
