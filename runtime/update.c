// Updates from the release repository (RT_UPDATE_REPO, paths.h): at start a background look at its latest release; the
// launcher shows one line next to the version when there's a newer one, and on a click downloads it, puts it in place
// and offers a restart. Nothing happens without that click, and the look is skipped with "Check for updates" off
// (larecomp.ini check_updates = 0).
//   Windows: the release zip, unpacked over this folder - each file that changes renamed to <name>.old first (an exe or
//   a DLL in use can be renamed, not overwritten; the .old files go at the next start), files the player may have
//   changed in mods/ kept.
//   Linux: the AppImage ($APPIMAGE) replaced by the new one (the running copy stays mounted until it exits). A folder
//   install (a source checkout, the PS5 / Steam Frame builds) gets the line and a link to the release page only.
// The game code is built again by the start screen when the new version's differs (gamelib id), as after a manual update.
// LARECOMP_UPDATE_AS=<version>: pretend to be that version; LARECOMP_UPDATE_TEST_INSTALL=1: install without the click (tests).
#include "update.h"

#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "paths.h"
#include "rt.h"
#include "settings.h"

#define MINIZ_NO_TIME
#include "miniz/miniz.h"  // (compiled once, in nvn/vulkan/texpack.c)

#ifdef _WIN32
#include <windows.h>
#include <winhttp.h>
#include <shellapi.h>
#include <direct.h>
#else
#include <errno.h>
#include <pthread.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#define UA "LZA-Recomp-updater"

static _Atomic int g_state = UPD_IDLE;
static char g_version[32], g_page[512], g_url[1024], g_asset[256], g_err[256];
static uint64_t g_size;
static _Atomic uint64_t g_got;
static bool g_can_install;

int update_state(void) { return atomic_load(&g_state); }
const char *update_version(void) { return g_version; }
const char *update_error(void) { return g_err; }
bool update_can_install(void) { return g_can_install; }
float update_progress(void) { return g_size ? (float)((double)atomic_load(&g_got) / (double)g_size) : 0.0f; }

static void fail(const char *fmt, const char *a) {
    snprintf(g_err, sizeof g_err, fmt, a ? a : "");
    LOG("[update] %s\n", g_err);
    atomic_store(&g_state, UPD_FAILED);
}

// ---- the program's folder (Windows: the exe's; Linux: the AppImage's)
static void exe_dir(char *out, size_t n) {
#ifdef _WIN32
    char p[1024];
    DWORD l = GetModuleFileNameA(NULL, p, sizeof p);
    if (!l || l >= sizeof p) { out[0] = 0; return; }
    char *s = strrchr(p, '\\');
    if (s) *s = 0;
    snprintf(out, n, "%s", p);
#else
    const char *ai = getenv("APPIMAGE");
    snprintf(out, n, "%s", ai ? ai : "");
    char *s = strrchr(out, '/');
    if (s) *s = 0;
#endif
}

// ---- HTTPS GET into a file (counting the bytes in g_got)
#ifdef _WIN32
static bool http_get(const char *url, const char *path) {
    wchar_t wurl[1100];
    MultiByteToWideChar(CP_UTF8, 0, url, -1, wurl, 1100);
    URL_COMPONENTS uc = {.dwStructSize = sizeof uc};
    wchar_t host[256], upath[1024];
    uc.lpszHostName = host, uc.dwHostNameLength = 256, uc.lpszUrlPath = upath, uc.dwUrlPathLength = 1024;
    if (!WinHttpCrackUrl(wurl, 0, 0, &uc)) return false;
    HINTERNET s = WinHttpOpen(L"" UA, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!s) s = WinHttpOpen(L"" UA, WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!s) return false;
    WinHttpSetTimeouts(s, 15000, 15000, 30000, 60000);
    HINTERNET c = WinHttpConnect(s, host, uc.nPort, 0);
    HINTERNET r = c ? WinHttpOpenRequest(c, L"GET", upath, NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                         uc.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0) : NULL;
    bool ok = false;
    FILE *f = NULL;
    if (r && WinHttpSendRequest(r, L"Accept: application/vnd.github+json, application/octet-stream\r\n", (DWORD)-1L,
                                WINHTTP_NO_REQUEST_DATA, 0, 0, 0) && WinHttpReceiveResponse(r, NULL)) {
        DWORD code = 0, cl = sizeof code;
        WinHttpQueryHeaders(r, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &code, &cl,
                            WINHTTP_NO_HEADER_INDEX);
        if (code == 200 && (f = fopen(path, "wb"))) {
            ok = true;
            for (;;) {
                DWORD avail = 0, got = 0;
                if (!WinHttpQueryDataAvailable(r, &avail)) { ok = false; break; }
                if (!avail) break;
                static char buf[1 << 16];
                if (avail > sizeof buf) avail = sizeof buf;
                if (!WinHttpReadData(r, buf, avail, &got) || fwrite(buf, 1, got, f) != got) { ok = false; break; }
                atomic_fetch_add(&g_got, got);
            }
            fclose(f);
        } else LOG("[update] %s: HTTP %lu\n", url, (unsigned long)code);
    }
    if (r) WinHttpCloseHandle(r);
    if (c) WinHttpCloseHandle(c);
    WinHttpCloseHandle(s);
    return ok;
}
#else
// (Linux: the system's curl - every desktop distro and SteamOS has it; the runtime has no TLS of its own)
static bool http_get(const char *url, const char *path) {
    pid_t pid = fork();
    if (pid == 0) {
        execlp("curl", "curl", "-fsSL", "--max-time", "3600", "-A", UA, "-H", "Accept: application/vnd.github+json",
               "-o", path, url, (char *)NULL);
        _exit(127);
    }
    if (pid < 0) return false;
    int st = 0;
    for (;;) {
        pid_t w = waitpid(pid, &st, WNOHANG);
        if (w == pid) break;
        if (w < 0 && errno != EINTR) return false;
        struct stat sb;
        if (!stat(path, &sb)) atomic_store(&g_got, (uint64_t)sb.st_size);
        usleep(100000);
    }
    if (!WIFEXITED(st) || WEXITSTATUS(st) != 0) {
        LOG("[update] curl %s: %s %d\n", url, WIFEXITED(st) && WEXITSTATUS(st) == 127 ? "not found, exit" : "exit",
            WIFEXITED(st) ? WEXITSTATUS(st) : -1);
        return false;
    }
    return true;
}
#endif

// ---- the release (a few fields from the GitHub API's JSON)
static char *read_file(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *s = n > 0 && n < (64 << 20) ? malloc((size_t)n + 1) : NULL;
    if (s && fread(s, 1, (size_t)n, f) != (size_t)n) free(s), s = NULL;
    fclose(f);
    if (s) s[n] = 0, *len = (size_t)n;
    return s;
}
// the string value of "key" at or after p (escapes aren't expected in the fields read)
static const char *json_str(const char *p, const char *key, char *out, size_t n) {
    char k[64];
    snprintf(k, sizeof k, "\"%s\"", key);
    const char *q = p ? strstr(p, k) : NULL;
    if (!q) return NULL;
    q = strchr(q + strlen(k), ':');
    if (!q) return NULL;
    q++;
    while (*q == ' ') q++;
    if (*q != '"') return NULL;
    q++;
    size_t i = 0;
    while (*q && *q != '"' && i + 1 < n) out[i++] = *q++;
    out[i] = 0;
    return q;
}
// "1.2.10" > "1.2.9"; "1.2.0" > "1.2.0-rc5"
static int ver_cmp(const char *a, const char *b) {
    if (*a == 'v') a++;
    if (*b == 'v') b++;
    for (;;) {
        char *ea, *eb;
        long x = strtol(a, &ea, 10), y = strtol(b, &eb, 10);
        if (x != y) return x < y ? -1 : 1;
        a = ea, b = eb;
        if (*a == '.' && *b == '.') { a++, b++; continue; }
        bool pa = *a == '-', pb = *b == '-';  // (a pre-release before its release)
        if (pa != pb) return pa ? -1 : 1;
        return strcmp(a, b) < 0 ? -1 : strcmp(a, b) > 0;
    }
}

const char *update_current_version(void);
static const char *current_version(void) {
    const char *as = getenv("LARECOMP_UPDATE_AS");
    return as && *as ? as : RT_APP_VERSION;
}

const char *update_current_version(void) { return current_version(); }  // (the launcher's version text: LARECOMP_UPDATE_AS too)

static void tmp_dir(char *out, size_t n) {
    snprintf(out, n, "%s/cache", rt_data_root());
#ifdef _WIN32
    _mkdir(out);
#else
    mkdir(out, 0755);
#endif
    snprintf(out, n, "%s/cache/update", rt_data_root());
#ifdef _WIN32
    _mkdir(out);
#else
    mkdir(out, 0755);
#endif
}

static void *check_thread(void *arg) {
    (void)arg;
    char dir[1100], json[1200];
    tmp_dir(dir, sizeof dir);
    snprintf(json, sizeof json, "%s/latest.json", dir);
    char url[256];
    snprintf(url, sizeof url, "https://api.github.com/repos/%s/releases/latest", RT_UPDATE_REPO);
    size_t len = 0;
    char *s = http_get(url, json) ? read_file(json, &len) : NULL;
    if (!s) { LOG("[update] no answer from GitHub (offline?)\n"); atomic_store(&g_state, UPD_NONE); return NULL; }
    char tag[64] = "";
    json_str(s, "tag_name", tag, sizeof tag);
    json_str(s, "html_url", g_page, sizeof g_page);
    snprintf(g_version, sizeof g_version, "%s", tag[0] == 'v' ? tag + 1 : tag);
    if (!tag[0] || ver_cmp(g_version, current_version()) <= 0) {
        LOG("[update] up to date (%s, the latest release %s)\n", current_version(), tag[0] ? tag : "?");
        free(s);
        atomic_store(&g_state, UPD_NONE);
        return NULL;
    }
    // the asset for this platform: Windows the zip, Linux the AppImage (only when running from one)
#ifdef _WIN32
    const char *want = "windows-x64", *ext = ".zip";
    bool installable = true;
#else
    const char *want = "linux-x86_64", *ext = ".AppImage";
    bool installable = getenv("APPIMAGE") != NULL;
#if !defined(__x86_64__)
    installable = false;
#endif
#endif
    const char *p = s;
    char name[256], dl[1024];
    while ((p = strstr(p, "\"browser_download_url\""))) {
        // (the asset's "name" and "size" come before its download URL - the last ones before it: its "uploader" object,
        // also before it, has neither)
        const char *nk = NULL, *sk = NULL;
        for (const char *q = s; q < p; q++) {
            if (!strncmp(q, "\"name\"", 6)) nk = q;
            else if (!strncmp(q, "\"size\"", 6)) sk = q;
        }
        if (nk && json_str(nk, "name", name, sizeof name) && json_str(p, "browser_download_url", dl, sizeof dl) &&
            strstr(name, want) && strlen(name) > strlen(ext) && !strcmp(name + strlen(name) - strlen(ext), ext)) {
            g_size = sk ? strtoull(strchr(sk, ':') + 1, NULL, 10) : 0;
            snprintf(g_url, sizeof g_url, "%s", dl);
            snprintf(g_asset, sizeof g_asset, "%s", name);
            break;
        }
        p += 20;
    }
    g_can_install = installable && g_url[0];
    LOG("[update] %s available (running %s)%s\n", tag, current_version(),
        g_can_install ? "" : ": this install updates by hand (the release page)");
    free(s);
    atomic_store(&g_state, UPD_AVAILABLE);
    if (getenv("LARECOMP_UPDATE_TEST_INSTALL")) update_install_start();  // (tests: as if "Update" was clicked)
    return NULL;
}

static void start_thread(void *(*fn)(void *)) {
#ifdef _WIN32
    CreateThread(NULL, 0, (LPTHREAD_START_ROUTINE)(void *)fn, NULL, 0, NULL);
#else
    pthread_t t;
    if (!pthread_create(&t, NULL, fn, NULL)) pthread_detach(t);
#endif
}

void update_check_start(void) {
    static bool started;
    if (started) return;
    started = true;
    if (!g_settings.check_updates && !getenv("LARECOMP_UPDATE_AS")) return;
    atomic_store(&g_state, UPD_CHECKING);
    start_thread(check_thread);
}

// ---- installing
#ifdef _WIN32
static void mkdirs(const char *path) {  // (the folders of a file's path)
    char p[1100];
    snprintf(p, sizeof p, "%s", path);
    for (char *q = p + 3; *q; q++)
        if (*q == '/' || *q == '\\') { char c = *q; *q = 0; _mkdir(p); *q = c; }
}
static bool file_same(const char *path, uint64_t size, mz_uint32 crc) {
    size_t n = 0;
    char *s = read_file(path, &n);
    bool same = s && n == size && mz_crc32(MZ_CRC32_INIT, (const unsigned char *)s, n) == crc;
    free(s);
    return same;
}
#endif

static void *install_thread(void *arg) {
    (void)arg;
    char dir[1100], dl[1400], app[1100];
    tmp_dir(dir, sizeof dir);
    exe_dir(app, sizeof app);
    if (!app[0]) { fail("can't find this program's folder", NULL); return NULL; }
#ifdef _WIN32
    snprintf(dl, sizeof dl, "%s/%s", dir, g_asset);
#else
    snprintf(dl, sizeof dl, "%s.new", getenv("APPIMAGE"));  // (beside it: the rename stays on one file system)
#endif
    atomic_store(&g_got, 0);
    LOG("[update] downloading %s (%llu MB)\n", g_url, (unsigned long long)(g_size >> 20));
    if (!http_get(g_url, dl)) { fail("the download failed - try again, or get it from the release page%s", NULL); return NULL; }
    atomic_store(&g_state, UPD_INSTALLING);
#ifdef _WIN32
    mz_zip_archive z;
    memset(&z, 0, sizeof z);
    if (!mz_zip_reader_init_file(&z, dl, 0)) { fail("the download isn't a zip%s", NULL); return NULL; }
    char list[1200];
    snprintf(list, sizeof list, "%s/old-files.txt", dir);
    FILE *old = fopen(list, "a");
    unsigned n = mz_zip_reader_get_num_files(&z), moved = 0, written = 0, kept = 0;
    bool ok = true;
    for (unsigned i = 0; i < n && ok; i++) {
        mz_zip_archive_file_stat st;
        if (!mz_zip_reader_file_stat(&z, i, &st) || st.m_is_directory) continue;
        const char *rel = strchr(st.m_filename, '/');  // (past the zip's top folder, LZA-Recomp-PLA/)
        if (!rel || !rel[1]) continue;
        rel++;
        char to[1400];
        snprintf(to, sizeof to, "%s\\%s", app, rel);
        for (char *q = to; *q; q++) if (*q == '/') *q = '\\';
        DWORD at = GetFileAttributesA(to);
        bool exists = at != INVALID_FILE_ATTRIBUTES;
        if (exists && !strncmp(rel, "mods/", 5)) { kept++; continue; }  // (a mod's files the player may have changed)
        if (exists && file_same(to, st.m_uncomp_size, st.m_crc32)) continue;
        mkdirs(to);
        if (exists) {
            char bak[1410];
            snprintf(bak, sizeof bak, "%s.old", to);
            DeleteFileA(bak);
            if (!MoveFileExA(to, bak, MOVEFILE_REPLACE_EXISTING)) { LOG("[update] can't move %s aside (%lu)\n", to, GetLastError()); ok = false; break; }
            if (old) fprintf(old, "%s\n", bak);
            moved++;
        }
        if (!mz_zip_reader_extract_to_file(&z, i, to, 0)) { LOG("[update] can't write %s\n", to); ok = false; break; }
        written++;
    }
    if (old) fclose(old);
    mz_zip_reader_end(&z);
    DeleteFileA(dl);
    LOG("[update] %u files written (%u replaced, kept aside as .old), %u of the player's mod files kept\n", written, moved, kept);
    if (!ok) { fail("the update couldn't be put in place (details in lasrecomp.log)%s", NULL); return NULL; }
#else
    const char *ai = getenv("APPIMAGE");
    struct stat sb;
    if (stat(dl, &sb) || (g_size && (uint64_t)sb.st_size != g_size)) { unlink(dl); fail("the download is incomplete%s", NULL); return NULL; }
    chmod(dl, 0755);
    if (rename(dl, ai)) { unlink(dl); fail("can't replace the AppImage (%s)", strerror(errno)); return NULL; }
    LOG("[update] %s replaced by %s\n", ai, g_asset);
#endif
    atomic_store(&g_state, UPD_DONE);
    return NULL;
}

void update_install_start(void) {
    if (atomic_load(&g_state) != UPD_AVAILABLE && atomic_load(&g_state) != UPD_FAILED) return;
    if (!g_can_install) return;
    atomic_store(&g_state, UPD_DOWNLOADING);
    start_thread(install_thread);
}

void update_open_page(void) {
    const char *u = g_page[0] ? g_page : "https://github.com/" RT_UPDATE_REPO "/releases/latest";
#ifdef _WIN32
    ShellExecuteA(NULL, "open", u, NULL, NULL, SW_SHOWNORMAL);
#else
    if (fork() == 0) { execlp("xdg-open", "xdg-open", u, (char *)NULL); _exit(127); }
#endif
}

// at start: the files the last update set aside (no longer in use now)
void update_cleanup(void) {
#ifdef _WIN32
    char list[1200];
    snprintf(list, sizeof list, "%s/cache/update/old-files.txt", rt_data_root());
    FILE *f = fopen(list, "r");
    if (!f) return;
    char line[1500];
    int n = 0;
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        if (line[0] && strlen(line) > 4 && !strcmp(line + strlen(line) - 4, ".old") && DeleteFileA(line)) n++;
    }
    fclose(f);
    DeleteFileA(list);
    if (n) LOG("[update] %d old files from the last update removed\n", n);
#endif
}
