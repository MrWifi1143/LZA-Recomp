// The mod API's version 6 pieces that aren't anywhere else (runtime/modapi_ext.h): a point of the world on the overlay,
// and the save's id for scripts' storage.
#include <dirent.h>
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

// The save the game uses: the newest save_<n> folder under <data root>/save (one per user; written when the game
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
    snprintf(dir, sizeof dir, "%s/save", rt_data_root());
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
