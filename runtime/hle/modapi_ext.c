// The mod API's version 6 pieces that aren't anywhere else (runtime/modapi_ext.h): a point of the world on the overlay,
// and the save's id for scripts' storage.
#include <dirent.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "../hle.h"
#include "../modapi_ext.h"
#include "../paths.h"
#include "../platform.h"

int bubble_world_to_screen(const float p[3], float w, float h, float out[3]);  // (hle/bubble.c: the scene camera)
int modapi_hud_to_screen(const float pos[3], float out[3]) {
    float s[6];
    hud_size(s);
    return bubble_world_to_screen(pos, s[0], s[1], out);
}

// The saves' folder: LARECOMP_SAVE_DIR (a test's or Legends Studio's copy) or <data root>/save - as services/fs.c mounts it
static void save_root(char *out, size_t cap) {
    const char *d = getenv("LARECOMP_SAVE_DIR");
    if (d && *d) snprintf(out, cap, "%s%s%s", d[0] == '/' ? "" : rt_data_root(), d[0] == '/' ? "" : "/", d);
    else snprintf(out, cap, "%s/save", rt_data_root());
}

// The save the game uses: the newest save_<n> folder under the saves' folder (one per user; written when the game
// saves). "" none yet.
int modapi_save_id(char *out, int cap) {
    static char cached[256];
    static uint64_t at;
    uint64_t now = plat_now_ns();
    if (at && now - at < 2000000000ull) {  // (scripts ask often - storage.get each frame: the folder looked at every 2 s)
        snprintf(out, (size_t)cap, "%s", cached);
        return (int)strlen(out);
    }
    at = now;
    char dir[1100];
    save_root(dir, sizeof dir);
    DIR *d = opendir(dir);
    out[0] = 0;
    if (!d) return 0;
    struct dirent *e;
    time_t best = 0;
    while ((e = readdir(d))) {
        if (strncmp(e->d_name, "save_", 5)) continue;
        char p[1400];
        struct stat st;
        snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
        if (stat(p, &st) || !S_ISDIR(st.st_mode)) continue;
        if (!out[0] || st.st_mtime > best) best = st.st_mtime, snprintf(out, (size_t)cap, "%s", e->d_name);
    }
    closedir(d);
    snprintf(cached, sizeof cached, "%s", out);
    return (int)strlen(out);
}

// Version 8: the game's save commits counted (services/fs.c calls modapi_save_committed at each IFileSystem Commit) - a
// script's per-save values are written then, so they are what the save is: quit without saving, they are as they were
// when it was last saved; load an older save (its folder copied back), its values come with it.
static _Atomic uint32_t g_save_commits;
void modapi_save_committed(void) { g_save_commits++; }
uint32_t modapi_save_commits(void) { return g_save_commits; }
int modapi_save_path(char *out, int cap) {
    char id[256];
    out[0] = 0;
    if (!modapi_save_id(id, sizeof id)) return 0;
    char root[1100];
    save_root(root, sizeof root);
    snprintf(out, (size_t)cap, "%s/%s", root, id);
    return (int)strlen(out);
}
