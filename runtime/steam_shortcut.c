// "Add to Steam" (start screen, Options): this game as a non-Steam shortcut in the running Steam client, with its
// library artwork. Steam keeps its shortcuts in userdata/<account>/config/shortcuts.vdf, reads that file only when it
// starts and writes its own copy back when it exits, so editing it while Steam runs is lost. Instead the running client
// is asked to add the game (steam://addnonsteamgame/<path, every byte but A-Z a-z 0-9 -_.~ percent-encoded>, with the
// file /tmp/addnonsteamgamefile present - what SteamOS's steamos-add-to-steam does): it shows in the
// library at once. Steam picks the shortcut's app id itself (not the crc of exe + name), so the new entry is read back
// from shortcuts.vdf once Steam has saved it, and the artwork is copied to userdata/<account>/config/grid/ under that
// id: <id>p.png (capsule), <id>.png (wide), <id>_hero.png, <id>_logo.png. Steam shows new artwork after a restart.
//
// What gets added: Linux, the AppImage ($APPIMAGE), else play.sh in the data root (a source checkout), else this program
// - through a .desktop file in ~/.local/share/applications (its Name gives the shortcut its name; it also puts the game
// in the desktop's app menu). Windows: this program. The artwork: steam/*.png next to the program (the releases ship
// it), or docs/icon/steam/ in a source checkout.
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include <SDL3/SDL_misc.h>
#include <SDL3/SDL_thread.h>
#include <SDL3/SDL_timer.h>

#include "paths.h"
#include "rt.h"

#ifdef _WIN32
#include <windows.h>
#define SEP "\\"
#else
#include <dirent.h>
#include <unistd.h>
#define SEP "/"
#endif

static char g_status[256];
static SDL_AtomicInt g_busy;
const char *steam_shortcut_status(void) { return g_status; }
bool steam_shortcut_busy(void) { return SDL_GetAtomicInt(&g_busy) != 0; }

// ---- shortcuts.vdf (binary KeyValues: 0x00 map, 0x01 string, 0x02 int32, 0x08 end of map; keys are C strings)
typedef struct { uint32_t appid; char exe[1024]; } Shortcut;

// Every shortcut's appid and Exe (as stored: quoted). Returns how many (max n).
static int vdf_shortcuts(const uint8_t *d, size_t len, Shortcut *out, int n) {
    const uint8_t *p = d, *e = d + len;
    int count = 0, depth = 0;
    Shortcut cur = {0};
    while (p < e) {
        uint8_t t = *p++;
        if (t == 8) {  // end of a map
            if (depth == 2 && count < n && (cur.appid || cur.exe[0])) out[count++] = cur;
            if (--depth < 0) break;
            continue;
        }
        const uint8_t *k = p;
        while (p < e && *p) p++;
        if (p >= e) break;
        size_t klen = (size_t)(p - k);
        p++;
        if (t == 0) {
            depth++;
            if (depth == 2) memset(&cur, 0, sizeof cur);  // (root -> "shortcuts" -> "<n>")
        } else if (t == 1) {
            const uint8_t *v = p;
            while (p < e && *p) p++;
            if (p >= e) break;
            if (depth == 2 && klen == 3 && (!memcmp(k, "Exe", 3) || !memcmp(k, "exe", 3))) {
                size_t vl = (size_t)(p - v) < sizeof cur.exe - 1 ? (size_t)(p - v) : sizeof cur.exe - 1;
                memcpy(cur.exe, v, vl), cur.exe[vl] = 0;
            }
            p++;
        } else if (t == 2) {
            if (p + 4 > e) break;
            if (depth == 2 && klen == 5 && !memcmp(k, "appid", 5)) memcpy(&cur.appid, p, 4);
            p += 4;
        } else if (t == 7) {  // (uint64: not used in shortcuts, skipped)
            p += 8;
        } else {
            break;  // unknown type: stop rather than misread
        }
    }
    return count;
}

static uint8_t *read_file(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *b = n > 0 && n < (64 << 20) ? malloc((size_t)n) : NULL;
    if (b && fread(b, 1, (size_t)n, f) != (size_t)n) { free(b); b = NULL; }
    fclose(f);
    if (b) *len = (size_t)n;
    return b;
}

static bool copy_file(const char *from, const char *to) {
    size_t n;
    uint8_t *b = read_file(from, &n);
    if (!b) return false;
    FILE *f = fopen(to, "wb");
    bool ok = f && fwrite(b, 1, n, f) == n;
    if (f) ok = fclose(f) == 0 && ok;
    free(b);
    return ok;
}

static bool is_dir(const char *p) {
    struct stat st;
    return !stat(p, &st) && S_ISDIR(st.st_mode);
}

// ---- where Steam is
static bool steam_root(char *out, size_t n) {
#ifdef _WIN32
    char v[MAX_PATH];
    DWORD sz = sizeof v;
    if (RegGetValueA(HKEY_CURRENT_USER, "Software\\Valve\\Steam", "SteamPath", RRF_RT_REG_SZ, NULL, v, &sz) == ERROR_SUCCESS) {
        for (char *c = v; *c; c++) if (*c == '/') *c = '\\';
        snprintf(out, n, "%s", v);
        char u[1100];
        snprintf(u, sizeof u, "%s\\userdata", out);
        if (is_dir(u)) return true;
    }
    return false;
#else
    const char *home = getenv("HOME");
    if (!home) return false;
    static const char *const rel[] = {"/.local/share/Steam", "/.steam/steam", "/.steam/root",
                                      "/.var/app/com.valvesoftware.Steam/.local/share/Steam"};  // (Flatpak)
    for (size_t i = 0; i < sizeof rel / sizeof *rel; i++) {
        char u[1100];
        snprintf(out, n, "%s%s", home, rel[i]);
        snprintf(u, sizeof u, "%s/userdata", out);
        if (is_dir(u)) return true;
    }
    return false;
#endif
}

// Every account's shortcuts.vdf path (userdata/<id>/config/shortcuts.vdf; the file may not exist yet). Returns count.
static int account_dirs(const char *root, char dirs[][1100], int max) {
    int n = 0;
    char ud[1100];
    snprintf(ud, sizeof ud, "%s" SEP "userdata", root);
#ifdef _WIN32
    char pat[1100];
    snprintf(pat, sizeof pat, "%s\\*", ud);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    do {
        if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && fd.cFileName[0] >= '1' && fd.cFileName[0] <= '9' && n < max)
            snprintf(dirs[n++], 1100, "%s\\%s\\config", ud, fd.cFileName);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
#else
    DIR *d = opendir(ud);
    if (!d) return 0;
    for (struct dirent *de; (de = readdir(d)) && n < max;)
        if (de->d_name[0] >= '1' && de->d_name[0] <= '9') snprintf(dirs[n++], 1100, "%s/%s/config", ud, de->d_name);
    closedir(d);
#endif
    return n;
}

// The shortcut for `target` in that account (its stored Exe holds the path), or 0
static uint32_t find_shortcut(const char *config_dir, const char *target) {
    char p[1200];
    snprintf(p, sizeof p, "%s" SEP "shortcuts.vdf", config_dir);
    size_t len;
    uint8_t *b = read_file(p, &len);
    if (!b) return 0;
    static Shortcut sc[512];
    int n = vdf_shortcuts(b, len, sc, 512);
    free(b);
    for (int i = 0; i < n; i++)
        if (strstr(sc[i].exe, target)) return sc[i].appid;
    return 0;
}

// ---- what gets added
static bool add_target(char *exe, size_t n) {
#ifdef _WIN32
    return GetModuleFileNameA(NULL, exe, (DWORD)n) > 0;
#else
    const char *ai = getenv("APPIMAGE");
    if (ai && *ai) return (size_t)snprintf(exe, n, "%s", ai) < n;
    char sh[1100];
    snprintf(sh, sizeof sh, "%s/play.sh", rt_data_root());
    if (!access(sh, X_OK)) return (size_t)snprintf(exe, n, "%s", sh) < n;
    ssize_t l = readlink("/proc/self/exe", exe, n - 1);
    if (l <= 0) return false;
    exe[l] = 0;
    return true;
#endif
}

#ifndef _WIN32
// ~/.local/share/applications/<id>.desktop (and its icon in ~/.local/share/icons): Steam takes the name, command and
// icon from it. Returns its path in `out`.
static bool write_desktop(const char *exe, char *out, size_t n) {
    const char *home = getenv("HOME");
    if (!home) return false;
    char dir[1100], icon[1100], src[1100];
    const char *id = strstr(RT_APP_NAME, "Z-A") ? "lza-recomp-za" : "lza-recomp-pla";
    snprintf(dir, sizeof dir, "%s/.local/share/applications", home);
    char d2[1100];
    snprintf(d2, sizeof d2, "%s/.local/share", home), mkdir(d2, 0755);
    mkdir(dir, 0755);
    snprintf(d2, sizeof d2, "%s/.local/share/icons", home), mkdir(d2, 0755);
    snprintf(icon, sizeof icon, "%s/%s.png", d2, id);
    if (rt_resource("icon.png", "docs/icon/icon_1024.png", src, sizeof src)) copy_file(src, icon);  // (an AppImage's own is gone after it exits)
    char wd[1100];
    snprintf(wd, sizeof wd, "%s", exe);
    char *slash = strrchr(wd, '/');
    if (slash) *slash = 0;
    snprintf(out, n, "%s/%s.desktop", dir, id);
    FILE *f = fopen(out, "w");
    if (!f) return false;
    fprintf(f, "[Desktop Entry]\nType=Application\nName=%s\nComment=%s\nExec=\"%s\"\nPath=%s\nIcon=%s\nTerminal=false\n"
               "Categories=Game;\n",
            RT_APP_NAME, strstr(RT_APP_NAME, "Z-A") ? "Pokemon Legends: Z-A, recompiled for PC" : "Pokemon Legends: Arceus, recompiled for PC",
            exe, wd, icon);
    return fclose(f) == 0;
}
#endif

// ---- the artwork into each account that has the shortcut. Returns how many images were copied.
static int copy_art(const char *config_dir, uint32_t appid) {
    static const char *const art[4][2] = {{"capsule.png", "p.png"}, {"wide.png", ".png"}, {"hero.png", "_hero.png"}, {"logo.png", "_logo.png"}};
    char grid[1200];
    snprintf(grid, sizeof grid, "%s" SEP "grid", config_dir);
#ifdef _WIN32
    CreateDirectoryA(grid, NULL);
#else
    mkdir(grid, 0755);
#endif
    int copied = 0;
    for (int i = 0; i < 4; i++) {
        char name[64], dev[96], src[1100], dst[1300];
        snprintf(name, sizeof name, "steam" SEP "%s", art[i][0]);
        snprintf(dev, sizeof dev, "docs/icon/steam/%s", art[i][0]);
        if (!rt_resource(name, dev, src, sizeof src)) continue;
        snprintf(dst, sizeof dst, "%s" SEP "%u%s", grid, appid, art[i][1]);
        copied += copy_file(src, dst);
    }
    return copied;
}

static int run(void *unused) {
    (void)unused;
    char root[1100], exe[1100], dirs[16][1100];
    if (!steam_root(root, sizeof root)) {
        snprintf(g_status, sizeof g_status, "Steam isn't installed (no Steam folder found).");
        goto done;
    }
    if (!add_target(exe, sizeof exe)) {
        snprintf(g_status, sizeof g_status, "Couldn't find this game's program to add.");
        goto done;
    }
    int na = account_dirs(root, dirs, 16);
    if (!na) {
        snprintf(g_status, sizeof g_status, "No Steam account found: sign in to Steam once, then try again.");
        goto done;
    }
    LOG("[steam] adding %s (Steam at %s, %d account(s))\n", exe, root, na);
    // Already there: only the artwork again
    int had = 0;
    for (int i = 0; i < na; i++) {
        uint32_t id = find_shortcut(dirs[i], exe);
        if (id) had++, copy_art(dirs[i], id);
    }
    if (had) {
        snprintf(g_status, sizeof g_status, "Already in Steam - artwork refreshed. Restart Steam if it doesn't show yet.");
        goto done;
    }
    char add[1100];
#ifdef _WIN32
    snprintf(add, sizeof add, "%s", exe);
#else
    if (!write_desktop(exe, add, sizeof add)) {
        snprintf(g_status, sizeof g_status, "Couldn't write the desktop entry in ~/.local/share/applications.");
        goto done;
    }
#endif
    {   // steam://addnonsteamgame/<path, percent-encoded>
        char url[3500];
        int k = snprintf(url, sizeof url, "steam://addnonsteamgame/");
        for (const unsigned char *c = (const unsigned char *)add; *c && k < (int)sizeof url - 4; c++)
            k += (*c >= 'A' && *c <= 'Z') || (*c >= 'a' && *c <= 'z') || (*c >= '0' && *c <= '9') || strchr("-_.~", *c)  // (slashes too)
                     ? snprintf(url + k, sizeof url - k, "%c", *c)
                     : snprintf(url + k, sizeof url - k, "%%%02X", *c);
        LOG("[steam] %s\n", url);
#ifndef _WIN32
        {   // Steam only takes the request while this file exists (what SteamOS's steamos-add-to-steam does first)
            FILE *g = fopen("/tmp/addnonsteamgamefile", "a");
            if (g) fclose(g);
        }
#endif
        if (!SDL_OpenURL(url)) {
            snprintf(g_status, sizeof g_status, "Couldn't open Steam (%s).", SDL_GetError());
            goto done;
        }
    }
    snprintf(g_status, sizeof g_status, "Adding to Steam... (Steam starts if it isn't running)");
    // Steam saves shortcuts.vdf a moment after adding (longer when the link had to start it)
    for (int t = 0; t < 120; t++) {
        SDL_Delay(500);
        int found = 0, art = 0;
        for (int i = 0; i < na; i++) {
            uint32_t id = find_shortcut(dirs[i], exe);
            if (id) found++, art += copy_art(dirs[i], id);
        }
        if (found) {
            LOG("[steam] added (%d account(s)), %d image(s) copied\n", found, art);
            snprintf(g_status, sizeof g_status, art ? "Added to Steam. Restart Steam to see the artwork."
                                                    : "Added to Steam (no artwork found to copy).");
            goto done;
        }
    }
    snprintf(g_status, sizeof g_status, "Steam didn't add it within a minute - is Steam signed in? Try again with Steam open.");
done:
    LOG("[steam] %s\n", g_status);
    SDL_SetAtomicInt(&g_busy, 0);
    return 0;
}

// --add-to-steam: the same, waiting for it; the result on stdout. 0 = added or already there.
int steam_shortcut_add_now(void) {
    SDL_SetAtomicInt(&g_busy, 1);
    run(NULL);
    printf("%s\n", g_status);
    return strncmp(g_status, "Added", 5) && strncmp(g_status, "Already", 7);
}

// Start it (a thread: Steam can take a while). steam_shortcut_status() has what happened.
void steam_shortcut_add(void) {
    if (!SDL_CompareAndSwapAtomicInt(&g_busy, 0, 1)) return;
    snprintf(g_status, sizeof g_status, "Looking for Steam...");
    SDL_Thread *t = SDL_CreateThread(run, "steam-shortcut", NULL);
    if (t) SDL_DetachThread(t);
    else SDL_SetAtomicInt(&g_busy, 0);
}
