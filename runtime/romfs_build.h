// RomFS image from a folder of game files (runtime/romfs_build.c). The game reads its data through one raw RomFS
// image (romfs.bin, runtime/services/fs.c); players bring their own game files, and the start screen builds the image
// from their romfs/ folder.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Writes the RomFS image of the folder `dir` to `out`. progress (may be NULL): bytes of file data written so far and
// the total, now and then. false = failed (why: a short message).
bool romfs_build(const char *dir, const char *out, void (*progress)(uint64_t done, uint64_t total), char *why,
                 size_t why_size);

// Checks a RomFS image's tables the way the game's RomFS code uses them: every file in the file table found again by
// its path, through the directory and file hash tables. false = the game wouldn't find (some of) its own files (why:
// how many, the first one). An update's patch takes parts of the tables from the base game, so a base game whose
// RomFS isn't the one the update was made for gives an image like that: it boots, and the game stops at its first
// file ("path not found" - a reporter's 1.1.1 crash at the shader loading, 2026-11-17).
bool romfs_check(const char *image, char *why, size_t why_size);

// Bytes of file data under dir (the image is about this plus its tables).
uint64_t romfs_dir_bytes(const char *dir);

// Copies the folder tree src to dst (hard links where the file system allows: no second copy of 6 GB). progress as
// above. false = failed.
bool romfs_copy_tree(const char *src, const char *dst, void (*progress)(uint64_t done, uint64_t total));
// One file the same way (a hard link where the file system allows).
bool romfs_copy_file(const char *src, const char *dst, void (*progress)(uint64_t done, uint64_t total));

#ifdef __cplusplus
}
#endif
