// Discord Rich Presence: "Playing PL:A Recomp / PL:Z-A Recomp" on the player's Discord profile, with where they are
// (Arceus: the area, from the terrain the game loads), the frame rate and resolution, and the time played. Talks to
// the Discord app on this PC over its local IPC (a Unix socket discord-ipc-N on Linux, the named pipe
// \\.\pipe\discord-ipc-N on Windows); no Discord library, nothing sent anywhere else. No Discord running: nothing
// happens (tried again every 20 s). Off with larecomp.ini discord_presence = 0 (the launcher's Options).
// The same file in both games (runtime/discord.c in LegendsA and LegendsZA): keep them in step.
// LARECOMP_DISCORD_APP_ID=<id> uses another Discord application (tests); LARECOMP_DISCORD_LOG=1 logs the traffic.
//
// Invites (with the Steam relay plugin on: contrib/steamrelay): while this copy hosts a Link Trade / Private Battle
// network, the status is joinable - Discord friends get Join on it and the chat's + > Invite card. The join secret
// is "<host Steam id>.<token>.<Link Code>": the relay lets in whoever brings the token (larecomp_relay_invite /
// larecomp_relay_join), and the joining copy fills the code into its Link Code box (runtime/hle/swkbd.c). Joining
// doesn't move anyone in the game: the player still goes to the trade (Simona / Link Play) and confirms the code.
// Asking to join isn't accepted by the game (anyone in a shared server sees the status): the host sends a friend the
// chat's invite card, and that friend's Join takes it.
// LARECOMP_DISCORD_JOIN=<secret> (tests): as if Discord had sent that join.
#include <SDL3/SDL.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#endif

#include "paths.h"  // (after SDL and windows.h: rt.h's cpuid.h and SDL's intrin.h clash the other way round)
#include "rt.h"
#include "settings.h"

#ifndef RT_DISCORD_APP_ID
#define RT_DISCORD_APP_ID ""
#endif

enum { OP_HANDSHAKE = 0, OP_FRAME = 1, OP_CLOSE = 2, OP_PING = 3, OP_PONG = 4 };

static SDL_Mutex *g_lock;
static int g_screen;                // 0: the launcher, 1: the game
static char g_area[64];             // where the player is ("" unknown)
static int64_t g_start;             // when the game (or the launcher) was started (unix seconds)
static volatile bool g_running;
static bool g_log;
static char g_code[16];       // the Link Code this copy last entered (8 digits): its invites carry it
static char g_join_code[16];  // the code of an invite taken, for the next Link Code box
static uint64_t g_join_at;    // (when: an invite's code is offered for 30 minutes)
void *plugins_symbol(const char *name);  // runtime/plugin.c
void menu_notice_for(const char *text, unsigned ms);  // runtime/ui/menu.mm

#ifdef _WIN32
static HANDLE g_pipe = INVALID_HANDLE_VALUE;
#define CONNECTED (g_pipe != INVALID_HANDLE_VALUE)
#else
static int g_fd = -1;
#define CONNECTED (g_fd >= 0)
#endif

static void dc_close(void) {
#ifdef _WIN32
    if (g_pipe != INVALID_HANDLE_VALUE) CloseHandle(g_pipe);
    g_pipe = INVALID_HANDLE_VALUE;
#else
    if (g_fd >= 0) close(g_fd);
    g_fd = -1;
#endif
}

static bool dc_write(const void *p, size_t n) {
#ifdef _WIN32
    DWORD w;
    return WriteFile(g_pipe, p, (DWORD)n, &w, NULL) && w == n;
#else
    const char *c = p;
    while (n) {
        ssize_t w = send(g_fd, c, n, MSG_NOSIGNAL);
        if (w < 0 && errno == EINTR) continue;
        if (w <= 0) return false;
        c += w, n -= (size_t)w;
    }
    return true;
#endif
}

// How many bytes can be read now (-1: the connection is gone)
static int dc_available(int wait_ms) {
#ifdef _WIN32
    for (int t = 0;; t += 10) {
        DWORD avail = 0;
        if (!PeekNamedPipe(g_pipe, NULL, 0, NULL, &avail, NULL)) return -1;
        if (avail || t >= wait_ms) return (int)avail;
        Sleep(10);
    }
#else
    struct pollfd p = {g_fd, POLLIN, 0};
    int r = poll(&p, 1, wait_ms);
    if (r < 0) return errno == EINTR ? 0 : -1;
    if (r == 0) return 0;
    if (p.revents & (POLLERR | POLLHUP | POLLNVAL)) return -1;
    return 1;
#endif
}

static bool dc_read(void *p, size_t n) {
    char *c = p;
    while (n) {
#ifdef _WIN32
        DWORD r;
        if (!ReadFile(g_pipe, c, (DWORD)n, &r, NULL) || !r) return false;
#else
        if (dc_available(2000) <= 0) return false;
        ssize_t r = recv(g_fd, c, n, 0);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) return false;
#endif
        c += r, n -= (size_t)r;
    }
    return true;
}

static bool dc_send(uint32_t op, const char *json) {
    uint32_t len = (uint32_t)strlen(json), hdr[2] = {op, len};
    if (g_log) LOG("[discord] -> %u %s\n", op, json);
    return dc_write(hdr, sizeof hdr) && dc_write(json, len);
}

// One message from Discord: its opcode, the JSON in buf (cut to its size). False: the connection is gone.
static bool dc_recv(uint32_t *op, char *buf, size_t size) {
    uint32_t hdr[2];
    if (!dc_read(hdr, sizeof hdr)) return false;
    *op = hdr[0];
    uint32_t len = hdr[1];
    if (len > (1u << 20)) return false;
    char tmp[512];
    size_t got = 0;
    while (got < len) {  // (all of it is read; what doesn't fit in buf is dropped)
        size_t k = len - got < sizeof tmp ? len - got : sizeof tmp;
        if (!dc_read(tmp, k)) return false;
        if (got < size - 1) memcpy(buf + got, tmp, got + k < size - 1 ? k : size - 1 - got);
        got += k;
    }
    buf[len < size - 1 ? len : size - 1] = 0;
    if (g_log) LOG("[discord] <- %u %.300s\n", *op, buf);
    return true;
}

static bool dc_open(void) {
#ifdef _WIN32
    for (int i = 0; i < 10; i++) {
        char name[64];
        snprintf(name, sizeof name, "\\\\.\\pipe\\discord-ipc-%d", i);
        g_pipe = CreateFileA(name, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
        if (g_pipe != INVALID_HANDLE_VALUE) return true;
    }
    return false;
#else
    // Discord's own build, and the Flatpak / Snap ones and Vesktop (their sockets are in a folder of their own)
    const char *bases[] = {getenv("XDG_RUNTIME_DIR"), getenv("TMPDIR"), getenv("TMP"), getenv("TEMP"), "/tmp"};
    const char *subs[] = {"", "app/com.discordapp.Discord/", "snap.discord/", ".flatpak/com.discordapp.Discord/xdg-run/",
                          ".flatpak/dev.vencord.Vesktop/xdg-run/", "app/dev.vencord.Vesktop/"};
    for (size_t b = 0; b < sizeof bases / sizeof *bases; b++) {
        if (!bases[b] || !bases[b][0]) continue;
        for (size_t s = 0; s < sizeof subs / sizeof *subs; s++)
            for (int i = 0; i < 10; i++) {
                struct sockaddr_un a = {.sun_family = AF_UNIX};
                if (snprintf(a.sun_path, sizeof a.sun_path, "%s/%sdiscord-ipc-%d", bases[b], subs[s], i) >= (int)sizeof a.sun_path) continue;
                if (access(a.sun_path, F_OK)) continue;
                int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
                if (fd < 0) return false;
                if (!connect(fd, (struct sockaddr *)&a, sizeof a)) {
                    g_fd = fd;
                    if (g_log) LOG("[discord] connected: %s\n", a.sun_path);
                    return true;
                }
                close(fd);
            }
    }
    return false;
#endif
}

static const char *app_id(void) {
    const char *e = getenv("LARECOMP_DISCORD_APP_ID");
    return e && e[0] ? e : RT_DISCORD_APP_ID;
}

static bool dc_connect(void) {
    if (!dc_open()) return false;
    char msg[160], buf[2048];
    snprintf(msg, sizeof msg, "{\"v\":1,\"client_id\":\"%s\"}", app_id());
    uint32_t op = 0;
    buf[0] = 0;
    if (!dc_send(OP_HANDSHAKE, msg) || dc_available(3000) <= 0 || !dc_recv(&op, buf, sizeof buf) || op != OP_FRAME ||
        !strstr(buf, "\"READY\"")) {
        LOG("[discord] Discord didn't accept the connection%s%.200s\n", op == OP_CLOSE ? ": " : "", op == OP_CLOSE ? buf : "");
        dc_close();
        return false;
    }
    LOG("[discord] Rich Presence on (Discord found)\n");
    return true;
}

// JSON string contents (quotes and backslashes escaped, control characters dropped)
static void jesc(char *out, size_t size, const char *s) {
    size_t o = 0;
    for (; *s && o + 3 < size; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') out[o++] = '\\', out[o++] = (char)c;
        else if (c >= 0x20) out[o++] = (char)c;
    }
    out[o] = 0;
}

#ifndef RT_DISCORD_INVITE  // (the community invite: from the build, Makefile DISCORD_INVITE_FILE; none: no button)
#define RT_DISCORD_INVITE ""
#endif
static bool is_pla(void) { return RT_SHOT_PREFIX[0] == 'P'; }

// The Steam relay plugin's invite functions (NULL: no relay)
static int relay_invite(char *out, size_t size) {
    int (*f)(char *, size_t) = (int (*)(char *, size_t))plugins_symbol("larecomp_relay_invite");
    return f ? f(out, size) : 0;
}
static int relay_join(const char *secret) {
    int (*f)(const char *) = (int (*)(const char *))plugins_symbol("larecomp_relay_join");
    return f ? f(secret) : -1;
}

// The activity as JSON (the SET_ACTIVITY args), from the settings and where the player is
static void build_activity(char *out, size_t size, uint64_t nonce) {
    char details[96], state[96], large[128], de[200], se[200], le[260];
    SDL_LockMutex(g_lock);
    int screen = g_screen;
    snprintf(details, sizeof details, "%s", g_area[0] ? g_area : is_pla() ? "In the Hisui region" : "In Lumiose City");
    int64_t start = g_start;
    SDL_UnlockMutex(g_lock);
    if (screen == 0) snprintf(details, sizeof details, "In the launcher");
    // state: "4K · 60 fps" (the render height chosen; Automatic: the frame rate alone)
    int h = g_settings.res_height, fps = g_settings.fps > 0 ? g_settings.fps : 30;
    const char *res = h >= 2160 ? "4K" : h >= 1440 ? "1440p" : h >= 1080 ? "1080p" : h >= 720 ? "720p" : h > 0 ? "540p" : NULL;
    char rate[32];
    if (fps >= FPS_UNLIMITED) snprintf(rate, sizeof rate, "unlimited fps");
    else snprintf(rate, sizeof rate, "%d fps", fps);
    if (screen == 0) snprintf(state, sizeof state, "v%s", RT_APP_VERSION);
    else if (res) snprintf(state, sizeof state, "%s \xC2\xB7 %s", res, rate);
    else snprintf(state, sizeof state, "%s", rate);
    snprintf(large, sizeof large, "%s v%s", RT_APP_NAME, RT_APP_VERSION);
    // hosting with the relay: joinable (party + join secret; Discord takes no buttons with a secret)
    char secret[96] = "", party[64] = "", extra[400];
    if (screen == 1 && relay_invite(secret, sizeof secret - 20)) {
        SDL_LockMutex(g_lock);
        size_t k = strlen(secret);
        snprintf(secret + k, sizeof secret - k, ".%s", g_code);
        SDL_UnlockMutex(g_lock);
        snprintf(party, sizeof party, "lza-%.*s", (int)strcspn(secret, "."), secret);
        snprintf(details, sizeof details, "%s", is_pla() ? "Waiting for a Link Trade partner" : "Waiting for a Link Play partner");
    }
    char dbtn[160] = "";  // (the Discord button: only with the build's invite, as the launcher's)
    if (RT_DISCORD_INVITE[0]) snprintf(dbtn, sizeof dbtn, ",{\"label\":\"Discord\",\"url\":\"%s\"}", RT_DISCORD_INVITE);
    if (secret[0])
        snprintf(extra, sizeof extra, "\"party\":{\"id\":\"%s\",\"size\":[1,2]},\"secrets\":{\"join\":\"%s\"}", party, secret);
    else
        snprintf(extra, sizeof extra, "\"buttons\":[{\"label\":\"Get the recomp\",\"url\":\"https://github.com/MrWifi1143/LZA-Recomp\"}%s]",
                 dbtn);
    jesc(de, sizeof de, details), jesc(se, sizeof se, state), jesc(le, sizeof le, large);
    snprintf(out, size,
             "{\"cmd\":\"SET_ACTIVITY\",\"nonce\":\"%llu\",\"args\":{\"pid\":%d,\"activity\":{"
             "\"details\":\"%s\",\"state\":\"%s\",\"timestamps\":{\"start\":%lld},"
             "\"assets\":{\"large_image\":\"https://raw.githubusercontent.com/MrWifi1143/%s/main/icon.png\",\"large_text\":\"%s\"},"
             "%s}}}",
             (unsigned long long)nonce,
#ifdef _WIN32
             (int)GetCurrentProcessId(),
#else
             (int)getpid(),
#endif
             de, se, (long long)start, is_pla() ? "PLA-Recomp" : "ZA-Recomp", le, extra);
}

// The string value of "key":"..." after the first anchor in json (no escapes kept); false: not there
static bool jstr(const char *json, const char *anchor, const char *key, char *out, size_t size) {
    const char *p = anchor ? strstr(json, anchor) : json;
    if (!p) return false;
    char k[64];
    snprintf(k, sizeof k, "\"%s\":\"", key);
    p = strstr(p, k);
    if (!p) return false;
    p += strlen(k);
    size_t n = 0;
    while (*p && *p != '"' && n + 1 < size) {
        if (*p == '\\' && p[1]) p++;
        out[n++] = *p++;
    }
    out[n] = 0;
    return true;
}

// An invite taken (Discord's ACTIVITY_JOIN, or LARECOMP_DISCORD_JOIN): the relay lets the host in, the code waits
// for the Link Code box, and the player is told where to go
static void take_invite(const char *secret) {
    char relay[64] = "", code[16] = "";
    const char *last = strrchr(secret, '.');
    if (!last || last == secret) return;
    snprintf(relay, sizeof relay, "%.*s", (int)(last - secret) < 63 ? (int)(last - secret) : 63, secret);
    snprintf(code, sizeof code, "%s", last + 1);
    int r = relay_join(relay);
    LOG("[discord] invite taken: relay %s, Link Code %s\n", r > 0 ? "joining the host" : r == 0 ? "refused the invite" : "not on",
        code[0] ? "known" : "not set");
    if (r < 0) {
        menu_notice_for("Discord invite: switch on the Steam relay first\n(launcher > Mods > Steam relay, then start the game again).", 15000);
        return;
    }
    if (r == 0) return;
    SDL_LockMutex(g_lock);
    snprintf(g_join_code, sizeof g_join_code, "%s", code);
    g_join_at = SDL_GetTicks();
    SDL_UnlockMutex(g_lock);
    char msg[320];
    if (is_pla())
        snprintf(msg, sizeof msg, "Joined a Discord friend's Link Trade.\nGo to Simona in the Galaxy Hall and pick a Link Trade (nearby)%s.",
                 code[0] ? ": their Link Code is filled in" : "");
    else
        snprintf(msg, sizeof msg, "Joined a Discord friend's Link Play.\nOpen Link Play (Link Trade or Private Battle, nearby)%s.",
                 code[0] ? ": their Link Code is filled in" : "");
    menu_notice_for(msg, 20000);
}

static int dc_thread(void *arg) {
    (void)arg;
    char want[2048], sent[2048] = "", buf[4096];
    uint64_t next_try = 0, last_send = 0, nonce = 1;
    bool cleared = true;
    while (g_running) {
        uint64_t now = SDL_GetTicks();
        bool on = g_settings.discord_presence;
        if (!CONNECTED && on && now >= next_try) {
            if (!dc_connect()) next_try = now + 20000;
            else {  // (invites: a join, and friends asking to join)
                char sub[160];
                snprintf(sub, sizeof sub, "{\"cmd\":\"SUBSCRIBE\",\"evt\":\"ACTIVITY_JOIN\",\"nonce\":\"%llu\",\"args\":{}}", (unsigned long long)nonce++);
                dc_send(OP_FRAME, sub);
                snprintf(sub, sizeof sub, "{\"cmd\":\"SUBSCRIBE\",\"evt\":\"ACTIVITY_JOIN_REQUEST\",\"nonce\":\"%llu\",\"args\":{}}", (unsigned long long)nonce++);
                dc_send(OP_FRAME, sub);
            }
            sent[0] = 0;
        }
        {   // (tests: a join as if from Discord)
            static bool test_done;
            const char *t = getenv("LARECOMP_DISCORD_JOIN");
            if (t && !test_done && g_screen == 1 && now > 15000) test_done = true, take_invite(t);
        }
        if (CONNECTED) {
            for (int k = 0; k < 8; k++) {  // (whatever Discord sent: answers, pings)
                int a = dc_available(0);
                if (a < 0) { dc_close(); break; }
                if (a == 0) break;
                uint32_t op;
                if (!dc_recv(&op, buf, sizeof buf)) { dc_close(); break; }
                if (op == OP_PING) dc_send(OP_PONG, buf);
                else if (op == OP_FRAME && strstr(buf, "\"evt\":\"ACTIVITY_JOIN\"")) {
                    char secret[128];
                    if (jstr(buf, "\"data\"", "secret", secret, sizeof secret)) take_invite(secret);
                } else if (op == OP_FRAME && strstr(buf, "\"evt\":\"ACTIVITY_JOIN_REQUEST\"")) {
                    // (someone asks to join: not let in from here - the status is seen in every shared server. The
                    // player lets a friend in by sending them Discord's invite card: chat + > Invite)
                    char name[64] = "", msg[200];
                    jstr(buf, "\"user\"", "username", name, sizeof name);
                    snprintf(msg, sizeof msg, "%s asked to join on Discord.\nTo let them in, invite them from a chat: + > Invite to %s.",
                             name[0] ? name : "Someone", RT_APP_NAME);
                    menu_notice_for(msg, 10000);
                    LOG("[discord] %s asked to join (not accepted here: an invite from the chat lets them in)\n", name[0] ? name : "someone");
                } else if (op == OP_CLOSE) { LOG("[discord] Discord closed the connection: %.200s\n", buf); dc_close(); break; }
            }
        }
        if (CONNECTED) {
            if (!on) {  // (switched off in the launcher: the status goes, the connection too)
                if (!cleared) {
                    char clr[200];
                    snprintf(clr, sizeof clr, "{\"cmd\":\"SET_ACTIVITY\",\"nonce\":\"%llu\",\"args\":{\"pid\":%d}}", (unsigned long long)nonce++,
#ifdef _WIN32
                             (int)GetCurrentProcessId());
#else
                             (int)getpid());
#endif
                    dc_send(OP_FRAME, clr);
                }
                cleared = true;
                dc_close();
            } else {
                build_activity(want, sizeof want, 0);
                // (Discord takes 5 updates per 20 s: one every 4 s at most; the nonce isn't part of the comparison)
                if (strcmp(want, sent) && now - last_send >= 4000) {
                    char msg[2100];
                    build_activity(msg, sizeof msg, nonce++);
                    if (dc_send(OP_FRAME, msg)) {
                        strcpy(sent, want);
                        last_send = now;
                        cleared = false;
                    } else dc_close();
                }
            }
        }
        SDL_Delay(500);
    }
    if (CONNECTED) dc_close();
    return 0;
}

void discord_start(void) {
    if (g_running) return;
    g_log = getenv("LARECOMP_DISCORD_LOG") != NULL;
    if (!app_id()[0]) {
        if (g_log) LOG("[discord] no Discord application id in this build: Rich Presence off\n");
        return;
    }
    g_lock = SDL_CreateMutex();
    g_start = (int64_t)time(NULL);
    g_running = true;
    SDL_Thread *t = SDL_CreateThread(dc_thread, "discord", NULL);
    if (t) SDL_DetachThread(t);
    else g_running = false;
}

// 0: the launcher (or the setup), 1: the game (the time played starts again)
void discord_set_screen(int screen) {
    if (!g_lock) return;
    SDL_LockMutex(g_lock);
    if (screen != g_screen) {
        g_screen = screen;
        g_start = (int64_t)time(NULL);
        if (screen == 1) g_area[0] = 0;
    }
    SDL_UnlockMutex(g_lock);
}

void discord_set_area(const char *name) {
    if (!g_lock) return;
    SDL_LockMutex(g_lock);
    if (strcmp(g_area, name ? name : "")) {
        snprintf(g_area, sizeof g_area, "%s", name ? name : "");
        if (g_log) LOG("[discord] area: %s\n", g_area);
    }
    SDL_UnlockMutex(g_lock);
}

// A game file read (runtime/services/fs.c, each file once in a row): where the player is.
// Arceus: each area's terrain (bin/archive/field/terrain/ha_areaNN[_sNN].gfpak) is read when it is entered.
void discord_file_read(const char *path) {
    if (!g_lock || !path) return;
    if (is_pla()) {
        const char *p = strstr(path, "field/terrain/ha_area");
        if (!p) return;
        int n = atoi(p + 21);
        static const char *names[] = {"Jubilife Village", "Obsidian Fieldlands", "Crimson Mirelands", "Cobalt Coastlands",
                                      "Coronet Highlands", "Alabaster Icelands", "Ancient Retreat"};
        if (n >= 0 && n < (int)(sizeof names / sizeof *names)) discord_set_area(names[n]);
        else discord_set_area("");
    }
}

// A Link Code box answered (runtime/hle/swkbd.c): an 8-digit code is this copy's room - its invites carry it
void discord_link_code_entered(const char *text, int max_len) {
    if (!g_lock || max_len != 8 || strlen(text) != 8 || strspn(text, "0123456789") != 8) return;
    SDL_LockMutex(g_lock);
    snprintf(g_code, sizeof g_code, "%s", text);
    g_join_code[0] = 0;  // (an invite's code used, or another typed in its place)
    SDL_UnlockMutex(g_lock);
}

// A Link Code box opening: the code of an invite taken in the last 30 minutes, to fill in (1)
int discord_link_code_prefill(char *out, size_t size, int max_len) {
    if (!g_lock || max_len != 8) return 0;
    SDL_LockMutex(g_lock);
    int ok = g_join_code[0] && SDL_GetTicks() - g_join_at < 30 * 60 * 1000;
    if (ok) snprintf(out, size, "%s", g_join_code);
    SDL_UnlockMutex(g_lock);
    if (ok) LOG("[discord] Link Code box: the invite's code filled in\n");
    return ok;
}
