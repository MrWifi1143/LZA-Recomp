// Game file setup (runtime/gamesetup.h). The picked folder is searched a few levels deep (dump tools nest the files):
// exefs/ is the folder with main and main.npdm, the RomFS either a romfs.bin image or the folder with bin/message.
// exefs/ is copied into <data root>/extracted (hard links where the file system allows), the RomFS image is copied
// or built (runtime/romfs_build.c), then the game code is recompiled from exefs/ (runtime/gamebuild.c).
// No extracted files but .nsp / .xci: read out of those instead (runtime/nxpkg.c), the newest update that makes a
// supported build (1.1.1 for Arceus), the exefs checked before the old game files are replaced.
#include "gamesetup.h"

#include <SDL3/SDL.h>
#include <ctype.h>
#include <dirent.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "gamebuild.h"
#include "gamelib.h"
#include "hle.h"
#include "nxpkg.h"
#include "paths.h"
#include "platform.h"
#include "romfs_build.h"

#define ROOT "extracted"  // (in the data root, the working directory)
// The two games (program ids, main.npdm's ACI0), any version. The 60 fps and wild battles options and the generator's
// instruction patches are addresses in the main: only the builds with address tables get them (game_build).
#define TITLE_EEVEE 0x0100187003A36000ull
#define TITLE_PIKACHU 0x010003F003A34000ull
#define TITLE_PLA 0x01001F5010DFA000ull  // Pokemon Legends: Arceus
static const uint8_t EEVEE_100[8] = {0x0a, 0x3f, 0x0d, 0x21, 0xe6, 0x7d, 0xad, 0x80};    // Let's Go, Eevee! v1.0.0 main
static const uint8_t PIKACHU_100[8] = {0x1c, 0x37, 0x56, 0x92, 0xde, 0x4f, 0x4b, 0x87};  // Let's Go, Pikachu! v1.0.0
// Legends: Arceus 1.1.1 (base game 1.0.0 + the 1.1.1 update, v262144 - its control.nacp's display version 1.1.1):
// the build the runtime's Arceus fixes are written for (their hooks are at its addresses). Any other (the base game
// alone, 1.0.0: main 7FCAD279539DE183, size 0x430c000; an older update...) isn't played - the hooks would land in
// other code: the start screen asks for 1.1.1 (game_pla_unsupported).
static const uint8_t PLA_111[8] = {0xae, 0xe8, 0xf1, 0x50, 0xdd, 0xa1, 0xb5, 0xa8};

static _Atomic int g_state;
static _Atomic float g_progress;
static _Atomic bool g_running;
static char g_folder[1024];
static char g_fail_file[256];  // (SETUP_FAIL_DAMAGED_*: which file)

int setup_state(void) { return g_state; }
const char *setup_fail_file(void) { return g_fail_file; }
float setup_progress(void) { return g_progress; }
bool setup_running(void) { return g_running; }

static bool is_dir(const char *p) {
    struct stat st;
    return stat(p, &st) == 0 && S_ISDIR(st.st_mode);
}
static bool is_file(const char *p) {
    struct stat st;
    return stat(p, &st) == 0 && S_ISREG(st.st_mode);
}

static uint64_t program_id(const char *exefs) {
    char p[1100];
    snprintf(p, sizeof p, "%s/main.npdm", exefs);
    FILE *f = fopen(p, "rb");
    if (!f) return 0;
    uint64_t id = 0;
    uint8_t meta[0x80], aci[0x18];  // ACI0 (offset at META+0x70): the program id at +0x10
    if (fread(meta, 1, sizeof meta, f) == sizeof meta && !memcmp(meta, "META", 4) &&
        !fseek(f, *(uint32_t *)(meta + 0x70), SEEK_SET) && fread(aci, 1, sizeof aci, f) == sizeof aci &&
        !memcmp(aci, "ACI0", 4))
        id = *(uint64_t *)(aci + 0x10);
    fclose(f);
    return id;
}

static bool game_ok(const char *exefs) {
    uint64_t id = program_id(exefs);
    return id == TITLE_EEVEE || id == TITLE_PIKACHU || id == TITLE_PLA;
}

int game_build(const char *exefs) {
    char p[1100];
    snprintf(p, sizeof p, "%s/main", exefs);
    FILE *f = fopen(p, "rb");
    if (!f) return GAME_BUILD_OTHER;
    uint8_t id[8] = {0};
    fseek(f, 0x40, SEEK_SET);  // (NSO header: the module id)
    size_t n = fread(id, 1, 8, f);
    fclose(f);
    uint64_t pid = program_id(exefs);
    if (n == 8 && !memcmp(id, EEVEE_100, 8) && pid == TITLE_EEVEE) return GAME_BUILD_EEVEE_100;
    if (n == 8 && !memcmp(id, PIKACHU_100, 8) && pid == TITLE_PIKACHU) return GAME_BUILD_PIKACHU_100;
    if (n == 8 && !memcmp(id, PLA_111, 8) && pid == TITLE_PLA) return GAME_BUILD_PLA_111;
    return GAME_BUILD_OTHER;
}

bool game_pla_unsupported(const char *exefs) {
    return program_id(exefs) == TITLE_PLA && game_build(exefs) != GAME_BUILD_PLA_111;
}

bool game_module_id(const char *exefs, char out[17]) {
    char p[1100];
    snprintf(p, sizeof p, "%s/main", exefs);
    uint8_t id[8];
    FILE *f = fopen(p, "rb");
    bool ok = f && !fseek(f, 0x40, SEEK_SET) && fread(id, 1, 8, f) == 8;
    if (f) fclose(f);
    if (!ok) { snprintf(out, 17, "?"); return false; }
    for (int i = 0; i < 8; i++) snprintf(out + 2 * i, 3, "%02X", id[i]);
    return true;
}

const char *game_version(const char *exefs) {
    static const struct { uint8_t id[8]; const char *v; } k[] = {
        {{0x0a, 0x3f, 0x0d, 0x21, 0xe6, 0x7d, 0xad, 0x80}, "1.0.0"},  // Eevee
        {{0x1c, 0x37, 0x56, 0x92, 0xde, 0x4f, 0x4b, 0x87}, "1.0.0"},  // Pikachu
        {{0xc2, 0x08, 0xdb, 0x6a, 0x4e, 0xf4, 0x36, 0x1f}, "1.0.2"},  // Pikachu (issue #1's log)
    };
    char p[1100];
    snprintf(p, sizeof p, "%s/main", exefs);
    uint8_t id[8] = {0};
    FILE *f = fopen(p, "rb");
    if (f) {
        fseek(f, 0x40, SEEK_SET);
        if (fread(id, 1, 8, f) != 8) memset(id, 0, 8);
        fclose(f);
    }
    for (size_t i = 0; i < sizeof k / sizeof *k; i++)
        if (!memcmp(id, k[i].id, 8)) return k[i].v;
    return "1.0.0";  // (a build not listed here)
}

uint64_t game_title_id(void) { return program_id(ROOT "/exefs"); }  // (the extracted game's program id)

const char *game_name(const char *exefs) {
    uint64_t id = program_id(exefs);
    return id == TITLE_EEVEE ? "Eevee" : id == TITLE_PIKACHU ? "Pikachu" : id == TITLE_PLA ? "Legends: Arceus" : NULL;
}

bool game_romfs_broken(void) {
    static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
    static uint64_t seen_size;
    static int64_t seen_time;
    static bool broken;
    struct stat sb;
    if (stat(ROOT "/romfs.bin", &sb) || !S_ISREG(sb.st_mode)) return false;  // (no image: not set up, said elsewhere)
    pthread_mutex_lock(&lock);
    if ((uint64_t)sb.st_size != seen_size || (int64_t)sb.st_mtime != seen_time) {  // (checked once per image)
        char why[700] = "";
        broken = !romfs_check(ROOT "/romfs.bin", why, sizeof why);
        if (broken) LOG("[setup] " ROOT "/romfs.bin: %s - the base game doesn't fit the update (or the image is damaged)\n", why);
        seen_size = (uint64_t)sb.st_size, seen_time = (int64_t)sb.st_mtime;
    }
    bool b = broken;
    pthread_mutex_unlock(&lock);
    return b;
}

bool setup_ready(void) {
    return game_ok(ROOT "/exefs") && !game_pla_unsupported(ROOT "/exefs") && is_file(ROOT "/romfs.bin") && gamelib_present(ROOT) &&
           !game_romfs_broken();
}

typedef struct { char exefs[1024], romfs[1024], romfs_bin[1024]; bool package; } Found;

static void find_game(const char *dir, int depth, Found *f) {
    if (depth > 5 || (f->exefs[0] && (f->romfs[0] || f->romfs_bin[0]))) return;
    char p[1100];
    snprintf(p, sizeof p, "%s/main.npdm", dir);
    if (!f->exefs[0] && is_file(p)) {
        snprintf(p, sizeof p, "%s/main", dir);
        if (is_file(p)) snprintf(f->exefs, sizeof f->exefs, "%s", dir);
    }
    snprintf(p, sizeof p, "%s/bin/message", dir);  // (Let's Go; the Legends games: bin/archive (Arceus), arc/data.trpfs (Z-A))
    if (!f->romfs[0] && is_dir(p)) snprintf(f->romfs, sizeof f->romfs, "%s", dir);
    snprintf(p, sizeof p, "%s/bin/archive", dir);
    if (!f->romfs[0] && is_dir(p)) snprintf(f->romfs, sizeof f->romfs, "%s", dir);
    snprintf(p, sizeof p, "%s/arc/data.trpfs", dir);
    if (!f->romfs[0] && is_file(p)) snprintf(f->romfs, sizeof f->romfs, "%s", dir);
    snprintf(p, sizeof p, "%s/romfs.bin", dir);
    if (!f->romfs_bin[0] && is_file(p)) snprintf(f->romfs_bin, sizeof f->romfs_bin, "%s", p);
    DIR *d = opendir(dir);
    if (!d) return;
    char sub[64][256];
    int nsub = 0;
    for (struct dirent *e; (e = readdir(d));) {
        const char *n = e->d_name;
        if (n[0] == '.') continue;
        size_t len = strlen(n);
        if (len > 4) {
            char ext[5];
            for (int i = 0; i < 4; i++) ext[i] = (char)tolower((unsigned char)n[len - 4 + i]);
            ext[4] = 0;
            if (!strcmp(ext, ".xci") || !strcmp(ext, ".nsp") || !strcmp(ext, ".nca") || !strcmp(ext, ".nsz") ||
                !strcmp(ext, ".xcz"))
                f->package = true;
        }
        snprintf(p, sizeof p, "%s/%s", dir, n);
        if (nsub < 64 && strcmp(n, "bin") && is_dir(p)) snprintf(sub[nsub++], sizeof sub[0], "%s", n);
    }
    closedir(d);
    for (int i = 0; i < nsub; i++) {
        snprintf(p, sizeof p, "%s/%s", dir, sub[i]);
        find_game(p, depth + 1, f);
    }
}

// progress: copying + the image are the first 15%, the game code the rest
static float g_phase0, g_phase1;
static void copy_progress(uint64_t done, uint64_t total) {
    if (total) g_progress = g_phase0 + (g_phase1 - g_phase0) * (float)((double)done / (double)total);
}
static _Atomic int g_minutes_left = -1;
int setup_minutes_left(void) { return g_minutes_left; }
static void build_progress(double f, const char *step) {
    g_progress = 0.15f + 0.85f * (float)f;
    const char *a = strstr(step, "about ");  // (gamebuild.c's estimate: "about N minutes left" / "about a minute left")
    g_minutes_left = !a ? -1 : !strncmp(a, "about a minute", 14) ? 1 : atoi(a + 6);
    g_state = !strncmp(step, "Reading", 7) ? SETUP_READING : !strncmp(step, "Finishing", 9) ? SETUP_FINISHING
                                                                                           : SETUP_COMPILING;
}

// From the player's packages: the exefs of each update found (newest first, then the base game alone) is unpacked
// into a scratch folder until one is a supported build; then the old game files go and that exefs, the RomFS image
// (base + that update) and any add-on content are written into extracted/.
#define UNPACK_TMP "extracted.unpack"
static bool from_packages(void) {
    static const uint64_t titles[] = {TITLE_PLA, TITLE_EEVEE, TITLE_PIKACHU, 0};
    int err;
    char why[512] = "";
    g_state = SETUP_UNPACKING;
    NxPkg *p = nxpkg_open(g_folder, titles, &err, why, sizeof why);
    bool pla = p && nxpkg_title(p) == TITLE_PLA;
    if (!p) {
        LOG("[setup] packages in %s: %s\n", g_folder, why);
        g_state = err == NXPKG_NO_KEYS ? SETUP_FAIL_KEYS : err == NXPKG_KEY_MISSING ? SETUP_FAIL_KEYS_OLD
                : err == NXPKG_ONLY_UPDATE ? SETUP_FAIL_NO_BASE : err == NXPKG_COMPRESSED ? SETUP_FAIL_PACKAGE
                : err == NXPKG_NO_GAME ? SETUP_FAIL_VERSION : SETUP_FAIL_UNPACK;
        return false;
    }
    int pick = -1, n = nxpkg_update_count(p), order[17], no = 0;
    for (int u = 1; u <= n && no < 16; u++) order[no++] = u;
    order[no++] = 0;
    for (int i = 0; pick < 0 && i < no; i++) {
        int u = order[i];
        plat_remove_tree(UNPACK_TMP);
        mkdir(UNPACK_TMP, 0755);
        if (!nxpkg_write_exefs(p, u, UNPACK_TMP "/exefs", why, sizeof why)) {
            LOG("[setup] exefs: %s\n", why);
            g_state = SETUP_FAIL_UNPACK;
            break;
        }
        bool good = game_ok(UNPACK_TMP "/exefs") && !game_pla_unsupported(UNPACK_TMP "/exefs");
        char mid[17];
        game_module_id(UNPACK_TMP "/exefs", mid);
        LOG("[setup] %s v%u (main %s): %s\n", u ? "update" : "base game", nxpkg_update_version(p, u), mid,
            good ? "supported" : game_ok(UNPACK_TMP "/exefs") ? "not this build (Legends: Arceus needs 1.1.1)" : "not this game");
        if (good) pick = u;
    }
    if (pick < 0) {
        if (g_state == SETUP_UNPACKING) g_state = pla ? SETUP_FAIL_PLA_VERSION : SETUP_FAIL_VERSION;
        plat_remove_tree(UNPACK_TMP);
        nxpkg_close(p);
        return false;
    }
    rt_remove_game_files(ROOT);  // (an earlier or interrupted setup's)
    mkdir(ROOT, 0755);
    bool ok = rename(UNPACK_TMP "/exefs", ROOT "/exefs") == 0;
    plat_remove_tree(UNPACK_TMP);
    g_phase0 = 0, g_phase1 = 0.15f;
    why[0] = 0;
    bool read_err = false;
    Uint64 t0 = SDL_GetTicks();
    if (ok && !nxpkg_write_romfs(p, pick, ROOT "/romfs.bin", copy_progress, why, sizeof why)) {
        LOG("[setup] romfs image: %s\n", why);
        ok = false, read_err = !strstr(why, "can't");  // (else: the image couldn't be written - disk space)
    }
    // The checks nxpkg ran on the way (each NCA used against its content id, the image against its hash tree), then
    // the image's tables (romfs_check): the update's patch takes parts of the RomFS tables from the base game, so a
    // base game dump other than the one the update was made for gives an image whose tables don't find the game's
    // files, and the game stops at its first. Which it is decides what the player is told: a file damaged or altered
    // (dump that one again) / files fine but not fitting together - or a packing the reader gets wrong (send the log).
    const NxCheck *c = nxpkg_check(p);
    bool tables_bad = ok && !romfs_check(ROOT "/romfs.bin", why, sizeof why);
    if (ok) LOG("[setup] romfs image written and checked in %.1f s\n", (double)(SDL_GetTicks() - t0) / 1000);
    int fail = 0;
    g_fail_file[0] = 0;
    if (ok && (c->base == NXPKG_CHECK_BAD || c->update == NXPKG_CHECK_BAD)) {
        bool b = c->base == NXPKG_CHECK_BAD;
        snprintf(g_fail_file, sizeof g_fail_file, "%s", b ? c->base_file : c->update_file);
        LOG("[setup] the %s's file (%s) is damaged or altered: what's in it doesn't match its own content id - dump it again\n",
            b ? "base game" : "update", g_fail_file);
        fail = b ? SETUP_FAIL_DAMAGED_BASE : SETUP_FAIL_DAMAGED_UPDATE;
    } else if (ok && (tables_bad || c->image == NXPKG_CHECK_BAD)) {
        bool fine = c->base == NXPKG_CHECK_OK && (c->update == NXPKG_CHECK_OK || pick == 0);
        if (tables_bad) LOG("[setup] romfs image: %s - the game wouldn't find its own files\n", why);
        if (c->image == NXPKG_CHECK_BAD) LOG("[setup] romfs image: %s\n", c->detail);
        LOG("[setup] %s - details for a report:\n",
            fine ? "the files themselves are fine (their hashes match): the base game and the update don't fit together, or they're "
                   "packed in a way the setup doesn't read right"
                 : "the base game doesn't fit the update (or the image is damaged)");
        nxpkg_log_details(p, pick, ROOT "/romfs.bin");
        fail = fine ? SETUP_FAIL_MISMATCH : SETUP_FAIL_ROMFS;
    }
    if (fail) ok = false;
    // (an image that's wrong but whose tables look fine would play into a later crash: not kept for a code rebuild -
    // one whose tables don't find the files is, game_romfs_broken says why on the next start)
    if (fail && !(fail == SETUP_FAIL_ROMFS && tables_bad)) remove(ROOT "/romfs.bin");
    if (ok && nxpkg_write_dlc(p, ROOT "/dlc.bin", why, sizeof why)) LOG("[setup] add-on content written\n");
    nxpkg_close(p);
    if (!ok) g_state = fail ? fail : read_err ? SETUP_FAIL_UNPACK : SETUP_FAIL_COPY;
    return ok;
}

static bool build_code(void) {
    if (!gamebuild_available()) { g_state = SETUP_FAIL_TOOLS; return false; }
    // One compiler process per thread, as far as the memory goes: the generator and Windows take ~4 GB, each compiler
    // up to ~1.5 GB on the big files. 8 GB PCs (Windows reports ~7) ran 4 at once and swapped for hours at "55%"
    // (a player's i5-6500, 2026-11-11); now 2.
    int cores = SDL_GetNumLogicalCPUCores(), ram_mb = SDL_GetSystemRAM();
    int by_ram = (ram_mb - 4096) / 1536;
    if (by_ram < 1) by_ram = 1;
    GameBuildOpts o = {build_progress, cores < by_ram ? cores : by_ram};
    LOG("[setup] game code: %d compiler processes (%d threads, %d MB memory)\n", o.jobs, cores, ram_mb);
    char why[256] = "";
    g_state = SETUP_READING;
    if (!gamebuild_run(ROOT, &o, why, sizeof why)) {
        LOG("[setup] game code: %s\n", why);
        g_state = SETUP_FAIL_BUILD;
        return false;
    }
    return true;
}

static void *setup_thread(void *arg) {
    (void)arg;
    g_progress = 0;
    if (!g_folder[0]) {  // only the game code (files set up already, e.g. a new version of the app)
        if (game_pla_unsupported(ROOT "/exefs")) g_state = SETUP_FAIL_PLA_VERSION;  // (not built for nothing)
        else if (game_romfs_broken()) g_state = SETUP_FAIL_ROMFS;
        else if (build_code()) g_state = setup_ready() ? SETUP_DONE : SETUP_FAIL_BUILD;
        g_running = false;
        return NULL;
    }
    g_state = SETUP_LOOKING;
    Found f = {0};
    find_game(g_folder, 0, &f);
    LOG("[setup] %s: exefs %s, romfs %s, romfs.bin %s\n", g_folder, f.exefs[0] ? f.exefs : "-", f.romfs[0] ? f.romfs : "-",
        f.romfs_bin[0] ? f.romfs_bin : "-");
    if (!f.exefs[0] || (!f.romfs[0] && !f.romfs_bin[0])) {
        if (f.package || nxpkg_any(g_folder)) {
            if (from_packages() && build_code()) g_state = setup_ready() ? SETUP_DONE : SETUP_FAIL_BUILD;
            LOG("[setup] %s\n", g_state == SETUP_DONE ? "ready" : "failed");
            g_running = false;
            return NULL;
        }
        g_state = SETUP_FAIL_NOT_FOUND;
    } else if (!game_ok(f.exefs)) g_state = SETUP_FAIL_VERSION;
    else if (game_pla_unsupported(f.exefs)) {  // (the old game files stay: a setup before still plays)
        char mid[17];
        game_module_id(f.exefs, mid);
        LOG("[setup] %s: Legends: Arceus, but not 1.1.1 (main %s) - this build needs Arceus 1.1.1\n", f.exefs, mid);
        g_state = SETUP_FAIL_PLA_VERSION;
    } else {
        // (the picked folder can't be our own copy: setting up again deletes that first)
        char a[4096], b[4096];
        mkdir(ROOT, 0755);
        if (realpath(f.exefs, a) && realpath(ROOT, b) && !strncmp(a, b, strlen(b)))
            snprintf(f.exefs, sizeof f.exefs, "%s", ""), g_state = SETUP_FAIL_NOT_FOUND;
    }
    if (g_state >= SETUP_FAIL_NOT_FOUND) { g_running = false; return NULL; }

    rt_remove_game_files(ROOT);  // (an earlier or interrupted setup's)
    mkdir(ROOT, 0755);
    g_state = SETUP_COPYING;
    g_phase0 = 0, g_phase1 = 0.01f;
    bool ok = true;
    if (!strcmp(f.exefs, f.romfs)) {  // (a flat dump, main next to bin/: only the six code files, not the whole RomFS)
        static const char *const code[] = {"main", "main.npdm", "rtld", "sdk", "subsdk0", "subsdk1"};
        mkdir(ROOT "/exefs", 0755);
        for (size_t i = 0; ok && i < sizeof code / sizeof *code; i++) {
            char src[1100], dst[1100];
            snprintf(src, sizeof src, "%s/%s", f.exefs, code[i]);
            snprintf(dst, sizeof dst, ROOT "/exefs/%s", code[i]);
            ok = is_file(src) && romfs_copy_file(src, dst, copy_progress);
        }
    } else
        ok = romfs_copy_tree(f.exefs, ROOT "/exefs", copy_progress);
    if (ok && f.romfs_bin[0]) {
        g_phase0 = 0.01f, g_phase1 = 0.15f;
        ok = romfs_copy_file(f.romfs_bin, ROOT "/romfs.bin", copy_progress);
    } else if (ok) {
        g_state = SETUP_IMAGE;
        g_phase0 = 0.01f, g_phase1 = 0.15f;
        char why[256] = "";
        ok = romfs_build(f.romfs, ROOT "/romfs.bin", copy_progress, why, sizeof why);
        if (!ok) LOG("[setup] romfs image: %s\n", why);
    }
    char why[700] = "";
    if (ok && !romfs_check(ROOT "/romfs.bin", why, sizeof why)) {  // (a romfs.bin or romfs/ picked: the same check)
        LOG("[setup] romfs image: %s\n", why);
        g_state = SETUP_FAIL_ROMFS;
    } else if (!ok) g_state = SETUP_FAIL_COPY;
    else if (build_code()) g_state = setup_ready() ? SETUP_DONE : SETUP_FAIL_BUILD;
    LOG("[setup] %s\n", g_state == SETUP_DONE ? "ready" : "failed");
    g_running = false;
    return NULL;
}

void setup_start(const char *folder) {
    if (g_running) return;
    snprintf(g_folder, sizeof g_folder, "%s", folder ? folder : "");
    for (char *c = g_folder; *c; c++) if (*c == '\\') *c = '/';
    g_running = true;
    g_fail_file[0] = 0;
    g_state = SETUP_LOOKING;
    pthread_t t;
    pthread_create(&t, NULL, setup_thread, NULL);
    pthread_detach(t);
}

int setup_prepare(const char *game, FILE *out) {
    static const char *const step[] = {"", "Looking for the game files", "Reading the game files out of the packages",
                                       "Copying the game files", "Building the game data image", "Reading the game code",
                                       "Recompiling the game code", "Finishing"};
    static const char *const fail[] = {"No game files found there", "Compressed game files (.nsz / .xcz): decompress them first",
                                       "Not Pokemon Legends: Arceus", "The files couldn't be copied (disk space?)",
                                       "The game code didn't build (lasrecomp.log)", "The game code builder is missing from this copy",
                                       "No keys found for the game files (prod.keys)", "Your keys can't open the game files",
                                       "Only the update was found, not the base game", "The game files couldn't be read",
                                       "This needs Pokemon Legends: Arceus version 1.1.1 (the base game and the 1.1.1 update); those files are another version",
                                       "The game can't find its own files in those (the base game doesn't fit the update): dump the base game again from your Switch",
                                       "Your base game file is damaged or altered (what's in it doesn't match its own checksum): dump the base game again from your Switch",
                                       "Your update file is damaged or altered (what's in it doesn't match its own checksum): dump the 1.1.1 update again from your Switch",
                                       "Your game files are intact, but the base game and the update don't fit together (or are packed in a way the setup doesn't "
                                       "expect): please send lasrecomp.log in a bug report on Discord; a fresh dump of the base game may also help"};
    if (!game || !*game) { fprintf(out, "progress 0 No game given (--game <folder or .nsp / .xci>)\n"); return 2; }
    LOG("[setup] --prepare %s\n", game);
    setup_start(game);
    int last_st = -1, last_pct = -1, last_min = -2;
    float p = 0;
    while (setup_running()) {
        int st = setup_state(), pct = (int)(setup_progress() * 100), m = setup_minutes_left();
        p = setup_progress();
        if (st >= 1 && st <= 7 && (st != last_st || pct != last_pct || m != last_min)) {
            if (st == SETUP_COMPILING && m > 0) fprintf(out, "progress %.2f %s (about %d minute%s left)\n", p, step[st], m, m == 1 ? "" : "s");
            else fprintf(out, "progress %.2f %s\n", p, step[st]);
            fflush(out);
            last_st = st, last_pct = pct, last_min = m;
        }
        SDL_Delay(200);
    }
    int st = setup_state();
    if (st == SETUP_DONE) { fprintf(out, "progress 1 Ready to play\n"); fflush(out); return 0; }
    int k = st - SETUP_FAIL_NOT_FOUND;
    bool known = k >= 0 && k < (int)(sizeof fail / sizeof *fail);
    bool damaged = st == SETUP_FAIL_DAMAGED_BASE || st == SETUP_FAIL_DAMAGED_UPDATE;
    fprintf(out, "progress %.2f %s%s%s%s\n", p, known ? fail[k] : "The setup failed (lasrecomp.log)", damaged ? " (" : "", damaged ? g_fail_file : "",
            damaged ? ")" : "");
    fflush(out);
    return known ? 20 + k : 1;
}
