// Steam relay: PL:Z-A Recomp's local play (Link Trades, Private Battles - "Nearby" in Link Play) with friends anywhere,
// over Steam's relay network. A runtime plugin (runtime/plugin.h): `make steamrelay` builds build/plugins/libsteamrelay.so;
// put it in <data root>/plugins/ (or LARECOMP_PLUGINS=<path>), with Steam running and logged in. The start screen's Mods
// page switches it on and off. It runs as Spacewar (app 480, Valve's test app every
// Steam account has): Steam shows "Playing Spacewar" while the game runs.
//
// The Steam API runs in a helper program beside the plugin (plugins/steamrelay-helper[.exe], this file built with
// RELAY_HELPER), not in the game: a process registered as Spacewar gets Spacewar's Steam Input controller setup, which
// sends the game nothing - every controller going through Steam Input (a Steam Controller, an Xbox pad with Steam Input
// on) lost its buttons while online play was on. The plugin and the helper talk over its stdin / stdout: frames
// [type][u32 length][data] - plugin -> helper 'S' send (u64 to + message), 'P' hosting (u8); helper -> plugin 'R' ready
// (u64 own id), 'F' failed (text), 'M' a friend's message (u64 from + message), 'H' the friends hosting (u64 id, u8 name
// length, name)... The helper ends when its stdin closes (the game quit).
//
// How: runtime/services/ldn.c treats a relay peer as a player at 127.0.4.<k> (ldn_relay_set). What the game sends there
// comes here and goes to that friend as a Steam networking message (port numbers first: [from port][to port][data]);
// what a friend sends is replayed over loopback from 127.0.4.<k>:<their port> to this copy, where ldn.c receives it as
// from any LAN player. A host advertises itself in its Steam rich presence (key "lzarecomp" = "hosting" - Z-A; Arceus:
// "plarecomp", so the two games never see each other's rooms - LARECOMP_RELAY_KEY, set for the helper); a scan asks
// every friend showing it. Only friends: messages from anyone else are refused (their sessions not accepted).
//
// The Steam API is not shipped: libsteam_api.so is loaded from the Steam install (Steam's own copy - SteamVR's - or any
// game's), or LARECOMP_STEAM_API=<path>. LARECOMP_RELAY_FAKE=<this port>,<peer port> (tests): no Steam - the relay's
// messages go over UDP on 127.0.0.1 between two copies of the game, the peer at 127.0.4.1.
#define _GNU_SOURCE
#include <pthread.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <io.h>
#include <fcntl.h>
typedef SOCKET sock_t;
#define NO_SOCK INVALID_SOCKET
#define sock_close closesocket
#define strcasecmp _stricmp
static __attribute__((unused)) void *lib_open(const char *p) { return (void *)LoadLibraryA(p); }
static __attribute__((unused)) void *lib_sym(void *h, const char *n) { return (void *)GetProcAddress((HMODULE)h, n); }
static __attribute__((unused)) void lib_close(void *h) { FreeLibrary((HMODULE)h); }
static void nap_ms(unsigned ms) { Sleep(ms); }
static __attribute__((unused)) void env_set(const char *k, const char *v) { SetEnvironmentVariableA(k, v), _putenv_s(k, v); }
static __attribute__((unused)) void nonblocking(sock_t s) { u_long on = 1; ioctlsocket(s, FIONBIO, &on); }
#define STEAM_API "steam_api64.dll"
#else
#include <arpa/inet.h>
#include <dirent.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <strings.h>
#include <sys/socket.h>
#include <unistd.h>
#include <spawn.h>
#include <sys/wait.h>
extern char **environ;
typedef int sock_t;
#define NO_SOCK (-1)
#define sock_close close
static __attribute__((unused)) void *lib_open(const char *p) { return dlopen(p, RTLD_NOW | RTLD_LOCAL); }
static __attribute__((unused)) void *lib_sym(void *h, const char *n) { return dlsym(h, n); }
static __attribute__((unused)) void lib_close(void *h) { dlclose(h); }
static void nap_ms(unsigned ms) { usleep(ms * 1000); }
static __attribute__((unused)) void env_set(const char *k, const char *v) { setenv(k, v, 1); }
static __attribute__((unused)) void nonblocking(sock_t s) { fcntl(s, F_SETFL, fcntl(s, F_GETFL) | O_NONBLOCK); }
#define STEAM_API "libsteam_api.so"
#endif
// the rich presence key hosts advertise under: one per game (the plugin sets it for the helper from the title id)
static __attribute__((unused)) const char *relay_key(void) {
    const char *k = getenv("LARECOMP_RELAY_KEY");
    return k && *k ? k : "lzarecomp";
}


#ifdef RELAY_HELPER
#define LOG(...) (fprintf(stderr, __VA_ARGS__), fflush(stderr))
#ifdef _WIN32
#define fd_read _read
#define fd_write _write
#else
#define fd_read read
#define fd_write write
#endif
#else
#include "../../runtime/plugin.h"

static const LarecompPluginApi *A;
#define LOG(...) A->log(__VA_ARGS__)

// ---- runtime/services/ldn.c's relay interface (plugin API 3: ldn_relay_set / ldn_relay_local)
typedef struct {
    void (*send)(uint32_t to, uint16_t from_port, uint16_t to_port, const void *p, size_t n);
    int (*targets)(uint32_t *out, int max);
    void (*hosting)(int on);
    uint64_t (*id_of)(uint32_t addr);
    uint32_t (*addr_of)(uint64_t id);
} LdnRelay;
static uint32_t (*ldn_relay_local)(void);

// ---- the peers: 127.0.4.<k> <-> a Steam id
#define PEERS 250
static uint64_t g_peer[PEERS + 1];  // (k 1..PEERS)
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static uint32_t peer_addr(int k) { return 0x7F000400u | (uint32_t)k; }
static int peer_k(uint32_t addr) { return (addr & 0xFFFFFF00u) == 0x7F000400u && (addr & 0xFF) >= 1 && (addr & 0xFF) <= PEERS ? (int)(addr & 0xFF) : 0; }
static int peer_of(uint64_t id) {  // (registered on first sight)
    if (!id) return 0;
    pthread_mutex_lock(&g_lock);
    int k = 0;
    for (int i = 1; i <= PEERS && !k; i++)
        if (g_peer[i] == id) k = i;
    for (int i = 1; i <= PEERS && !k; i++)
        if (!g_peer[i]) g_peer[i] = id, k = i;
    pthread_mutex_unlock(&g_lock);
    return k;
}

// ---- replaying what a peer sent: from 127.0.4.<k>:<their port> to this copy. When that port is taken on this PC
// (a socket of this PC on 0.0.0.0 holds it: two copies on one PC), any free port stands in for it (local) and
// what this copy sends back to that port goes to the peer's real one (real_port).
static struct { int k; uint16_t port, local; sock_t fd; } g_out[512];
static int g_nout;
static pthread_mutex_t g_out_lock = PTHREAD_MUTEX_INITIALIZER;
static uint16_t real_port(int k, uint16_t local) {
    uint16_t port = local;
    pthread_mutex_lock(&g_out_lock);
    for (int i = 0; i < g_nout; i++)
        if (g_out[i].k == k && g_out[i].local == local) port = g_out[i].port;
    pthread_mutex_unlock(&g_out_lock);
    return port;
}
static void replay(int k, uint16_t from_port, uint16_t to_port, const void *p, size_t n) {
    sock_t fd = NO_SOCK;
    pthread_mutex_lock(&g_out_lock);
    for (int i = 0; i < g_nout && fd == NO_SOCK; i++)
        if (g_out[i].k == k && g_out[i].port == from_port) fd = g_out[i].fd;
    if (fd == NO_SOCK) {
        fd = socket(AF_INET, SOCK_DGRAM, 0);
        struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = htons(from_port), .sin_addr.s_addr = htonl(peer_addr(k))};
        int one = 1;  // (the peer's own port even where this PC's other copy holds it on every address: tests on one PC)
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (const char *)&one, sizeof one);
        if (fd != NO_SOCK && bind(fd, (struct sockaddr *)&a, sizeof a)) {
            a.sin_port = 0;
            if (bind(fd, (struct sockaddr *)&a, sizeof a)) sock_close(fd), fd = NO_SOCK;
        }
        socklen_t al = sizeof a;
        if (fd == NO_SOCK || getsockname(fd, (struct sockaddr *)&a, &al)) {
            LOG("[steamrelay] can't send from 127.0.4.%d here\n", k);
            if (fd != NO_SOCK) sock_close(fd);
            pthread_mutex_unlock(&g_out_lock);
            return;
        }
        if (ntohs(a.sin_port) != from_port) LOG("[steamrelay] 127.0.4.%d:%u stands in for the peer's port %u\n", k, ntohs(a.sin_port), from_port);
        if (g_nout < (int)(sizeof g_out / sizeof *g_out))
            g_out[g_nout].k = k, g_out[g_nout].port = from_port, g_out[g_nout].local = ntohs(a.sin_port), g_out[g_nout++].fd = fd;
    }
    pthread_mutex_unlock(&g_out_lock);
    struct sockaddr_in to = {.sin_family = AF_INET, .sin_port = htons(to_port), .sin_addr.s_addr = htonl(ldn_relay_local())};
    sendto(fd, (const char *)p, (int)n, 0, (struct sockaddr *)&to, sizeof to);
}

#endif

#ifdef RELAY_HELPER
// ---- the Steam API (its flat C interface, loaded at run time)
typedef struct { int32_t type, size; union { uint64_t steamid; uint8_t raw[128]; }; } Identity;  // SteamNetworkingIdentity
typedef struct {  // SteamNetworkingMessage_t (the fields used)
    void *data;
    int32_t size;
    uint32_t conn;
    Identity peer;
} Message;
typedef struct { int32_t user, id; uint8_t *param; int32_t size; } CallbackMsg;  // CallbackMsg_t
static struct {
    bool (*InitSafe)(void);
    int32_t (*GetHSteamPipe)(void);
    void (*MD_Init)(void), (*MD_RunFrame)(int32_t), (*MD_FreeLast)(int32_t);
    bool (*MD_Next)(int32_t, CallbackMsg *);
    void *(*Messages)(void), *(*Friends)(void), *(*User)(void);
    int32_t (*Send)(void *, const Identity *, const void *, uint32_t, int32_t, int32_t);
    int32_t (*Receive)(void *, int32_t, Message **, int32_t);
    bool (*Accept)(void *, const Identity *);
    void (*Release)(Message *);
    void (*SetSteamID64)(Identity *, uint64_t);
    uint64_t (*GetSteamID64)(const Identity *);
    bool (*SetRichPresence)(void *, const char *, const char *);
    int32_t (*FriendCount)(void *, int32_t);
    uint64_t (*FriendByIndex)(void *, int32_t, int32_t);
    const char *(*FriendRichPresence)(void *, uint64_t, const char *);
    void (*RequestRichPresence)(void *, uint64_t);
    void (*Shutdown)(void);
    bool (*HasFriend)(void *, uint64_t, int32_t);
    const char *(*FriendName)(void *, uint64_t);
    uint64_t (*MySteamID)(void *);
} S;
static void *g_msgs, *g_friends;
static volatile int g_quit;
#define FRIEND_IMMEDIATE 4
#define SEND_UNRELIABLE_NO_NAGLE_AUTORESTART (1 | 32)

// Where Steam keeps games: its own folder and every library folder (steamapps/libraryfolders.vdf "path" lines)
static int steam_libraries(char (*out)[512], int max) {
    char root[512] = "";
#ifdef _WIN32
    DWORD n = sizeof root;
    if (RegGetValueA(HKEY_CURRENT_USER, "Software\\Valve\\Steam", "SteamPath", RRF_RT_REG_SZ, NULL, root, &n)) root[0] = 0;
#else
    const char *home = getenv("HOME");
    const char *const homes[] = {"%s/.local/share/Steam", "%s/.steam/steam", "%s/.var/app/com.valvesoftware.Steam/data/Steam"};
    for (int i = 0; i < 3 && home && !root[0]; i++) {
        snprintf(root, sizeof root, homes[i], home);
        char t[600];
        snprintf(t, sizeof t, "%s/steamapps", root);
        DIR *d = opendir(t);
        if (d) closedir(d);
        else root[0] = 0;
    }
#endif
    if (!root[0]) return 0;
    int k = 0;
    snprintf(out[k++], 512, "%s", root);
    char vdf[600], line[1024];
    snprintf(vdf, sizeof vdf, "%s/steamapps/libraryfolders.vdf", root);
    FILE *f = fopen(vdf, "r");
    while (f && k < max && fgets(line, sizeof line, f)) {
        const char *q = strstr(line, "\"path\"");
        if (!q || !(q = strchr(q + 6, '"'))) continue;
        char path[512];
        int n = 0;
        for (q++; *q && *q != '"' && n < 511; q++) path[n++] = *q == '\\' && q[1] == '\\' ? *++q : *q;  // ("D:\\SteamLibrary")
        path[n] = 0;
        bool dup = false;
        for (int i = 0; i < k; i++) dup |= !strcasecmp(out[i], path);
        if (n && !dup) snprintf(out[k++], 512, "%s", path);
    }
    if (f) fclose(f);
    return k;
}

static bool exists(const char *p) {
    FILE *f = fopen(p, "rb");
    if (f) fclose(f);
    return f != NULL;
}

// The Steam API libraries installed: SteamVR's copy first, then any game's (games ship it next to their files)
static int steam_api_copies(char (*out)[1024], int max) {
    int n = 0;
    char lib[16][512];
    int nl = steam_libraries(lib, 16);
#ifdef _WIN32
    const char *const vr = "steamapps/common/SteamVR/bin/win64/" STEAM_API, *const sub[] = {"", "bin/", "bin/win64/", "x64/"};
#else
    const char *const vr = "steamapps/common/SteamVR/bin/linux64/" STEAM_API, *const sub[] = {"", "bin/", "bin/linux64/", "lib/"};
#endif
    for (int i = 0; i < nl && n < max; i++) {
        snprintf(out[n], 1024, "%s/%s", lib[i], vr);
        if (exists(out[n])) n++;
    }
    for (int i = 0; i < nl && n < max; i++) {
        char common[600];
        snprintf(common, sizeof common, "%s/steamapps/common", lib[i]);
#ifdef _WIN32
        char pat[640];
        snprintf(pat, sizeof pat, "%s/*", common);
        WIN32_FIND_DATAA fd;
        HANDLE h = FindFirstFileA(pat, &fd);
        for (bool more = h != INVALID_HANDLE_VALUE; more && n < max; more = FindNextFileA(h, &fd)) {
            const char *game = fd.cFileName;
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || game[0] == '.' || !strcmp(game, "SteamVR")) continue;
#else
        DIR *d = opendir(common);
        for (struct dirent *e; d && n < max && (e = readdir(d));) {
            const char *game = e->d_name;
            if (game[0] == '.' || !strcmp(game, "SteamVR")) continue;
#endif
            for (int j = 0; j < 4 && n < max; j++) {
                snprintf(out[n], 1024, "%s/%s/%s" STEAM_API, common, game, sub[j]);
                if (exists(out[n])) { n++; break; }
            }
        }
#ifdef _WIN32
        if (h != INVALID_HANDLE_VALUE) FindClose(h);
#else
        if (d) closedir(d);
#endif
    }
    return n;
}

// A Steam API library with everything used here (an old one lacks the networking messages): resolved into S
static bool resolve(const char *path) {
    void *h = lib_open(path);
    if (!h) return false;
#define SYM(f, name) ((*(void **)&S.f = lib_sym(h, name)) != NULL)
    bool ok = SYM(InitSafe, "SteamAPI_InitSafe") && SYM(GetHSteamPipe, "SteamAPI_GetHSteamPipe") &&
              SYM(MD_Init, "SteamAPI_ManualDispatch_Init") && SYM(MD_RunFrame, "SteamAPI_ManualDispatch_RunFrame") &&
              SYM(MD_FreeLast, "SteamAPI_ManualDispatch_FreeLastCallback") && SYM(MD_Next, "SteamAPI_ManualDispatch_GetNextCallback") &&
              SYM(Messages, "SteamAPI_SteamNetworkingMessages_SteamAPI_v002") && SYM(Friends, "SteamAPI_SteamFriends_v017") &&
              SYM(User, "SteamAPI_SteamUser_v023") && SYM(Send, "SteamAPI_ISteamNetworkingMessages_SendMessageToUser") &&
              SYM(Receive, "SteamAPI_ISteamNetworkingMessages_ReceiveMessagesOnChannel") &&
              SYM(Accept, "SteamAPI_ISteamNetworkingMessages_AcceptSessionWithUser") &&
              SYM(Release, "SteamAPI_SteamNetworkingMessage_t_Release") && SYM(SetSteamID64, "SteamAPI_SteamNetworkingIdentity_SetSteamID64") &&
              SYM(GetSteamID64, "SteamAPI_SteamNetworkingIdentity_GetSteamID64") &&
              SYM(SetRichPresence, "SteamAPI_ISteamFriends_SetRichPresence") && SYM(FriendCount, "SteamAPI_ISteamFriends_GetFriendCount") &&
              SYM(FriendByIndex, "SteamAPI_ISteamFriends_GetFriendByIndex") &&
              SYM(FriendRichPresence, "SteamAPI_ISteamFriends_GetFriendRichPresence") &&
              SYM(RequestRichPresence, "SteamAPI_ISteamFriends_RequestFriendRichPresence") && SYM(HasFriend, "SteamAPI_ISteamFriends_HasFriend") &&
              SYM(FriendName, "SteamAPI_ISteamFriends_GetFriendPersonaName") && SYM(MySteamID, "SteamAPI_ISteamUser_GetSteamID") &&
              SYM(Shutdown, "SteamAPI_Shutdown");
#undef SYM
    if (!ok) lib_close(h);
    return ok;
}

static bool load_steam(void) {
    static char cand[24][1024];
    int n = 0;
    const char *e = getenv("LARECOMP_STEAM_API");
    if (e) snprintf(cand[n++], 1024, "%s", e);
    else n = steam_api_copies(cand, 24);
    int i = 0;
    while (i < n && !resolve(cand[i])) i++;
    if (i == n) {
        LOG("[steamrelay] no usable " STEAM_API " (%d found; Steam installed? or LARECOMP_STEAM_API=<path>)\n", n);
        return false;
    }
    env_set("SteamAppId", "480"), env_set("SteamGameId", "480");  // (Spacewar - this helper's own process only)
    if (!S.InitSafe()) { LOG("[steamrelay] Steam isn't running (or not logged in): no relay\n"); return false; }
    S.MD_Init();  // (after the init: callbacks are taken by the pump thread)
    g_msgs = S.Messages(), g_friends = S.Friends();
    LOG("[steamrelay] Steam (%s): online play with friends on, as Spacewar - id %llu\n", cand[i],
        (unsigned long long)S.MySteamID(S.User()));
    return g_msgs && g_friends;
}


// ---- the helper's side of the pipe
static pthread_mutex_t g_out_lock = PTHREAD_MUTEX_INITIALIZER, g_steam_lock = PTHREAD_MUTEX_INITIALIZER;
static void out_frame(char type, const void *a, size_t an, const void *b, size_t bn) {
    uint8_t h[5] = {(uint8_t)type};
    uint32_t n = (uint32_t)(an + bn);
    memcpy(h + 1, &n, 4);
    pthread_mutex_lock(&g_out_lock);
    const void *part[3] = {h, a, b};
    size_t len[3] = {5, an, bn};
    for (int i = 0; i < 3; i++)
        for (size_t done = 0; done < len[i];) {
            int w = (int)fd_write(1, (const char *)part[i] + done, (unsigned)(len[i] - done));
            if (w <= 0) { g_quit = 1; break; }
            done += (size_t)w;
        }
    pthread_mutex_unlock(&g_out_lock);
}
static bool in_full(void *p, size_t n) {
    for (size_t done = 0; done < n;) {
        int r = (int)fd_read(0, (char *)p + done, (unsigned)(n - done));
        if (r <= 0) return false;
        done += (size_t)r;
    }
    return true;
}
static void *from_game(void *arg) {  // (the plugin's frames: sends and the hosting flag; its pipe closing ends the helper)
    (void)arg;
    static uint8_t buf[70000];
    for (;;) {
        uint8_t h[5];
        uint32_t n;
        if (!in_full(h, 5)) break;
        memcpy(&n, h + 1, 4);
        if (n > sizeof buf || !in_full(buf, n)) break;
        pthread_mutex_lock(&g_steam_lock);
        if (h[0] == 'S' && n > 8) {
            uint64_t id;
            memcpy(&id, buf, 8);
            Identity who;
            memset(&who, 0, sizeof who);
            S.SetSteamID64(&who, id);
            S.Send(g_msgs, &who, buf + 8, n - 8, SEND_UNRELIABLE_NO_NAGLE_AUTORESTART, 0);
        } else if (h[0] == 'P' && n >= 1) {
            S.SetRichPresence(g_friends, relay_key(), buf[0] ? "hosting" : "");
        }
        pthread_mutex_unlock(&g_steam_lock);
    }
    g_quit = 1;
    return NULL;
}

int main(void) {
#ifdef _WIN32
    _setmode(0, _O_BINARY), _setmode(1, _O_BINARY);
#endif
    if (!load_steam()) {
        static const char why[] = "Steam isn't running (or not logged in)";
        out_frame('F', why, sizeof why - 1, NULL, 0);
        return 1;
    }
    uint64_t me = S.MySteamID(S.User());
    out_frame('R', &me, 8, NULL, 0);
    pthread_t t;
    pthread_create(&t, NULL, from_game, NULL);
    int32_t pipe = S.GetHSteamPipe();
    for (unsigned tick = 0; !g_quit; tick++) {
        pthread_mutex_lock(&g_steam_lock);
        S.MD_RunFrame(pipe);
        CallbackMsg cb;
        while (S.MD_Next(pipe, &cb)) {
            if (cb.id == 1251 && cb.param) {  // SteamNetworkingMessagesSessionRequest_t: a friend's first message
                const Identity *who = (const Identity *)cb.param;
                uint64_t id = S.GetSteamID64(who);
                if (S.HasFriend(g_friends, id, FRIEND_IMMEDIATE)) S.Accept(g_msgs, who);
                else LOG("[steamrelay] refused a session from %llu (not a friend)\n", (unsigned long long)id);
            }
            S.MD_FreeLast(pipe);
        }
        Message *msg[64];
        int32_t n = S.Receive(g_msgs, 0, msg, 64);
        for (int i = 0; i < n; i++) {
            uint64_t id = S.GetSteamID64(&msg[i]->peer);
            if (S.HasFriend(g_friends, id, FRIEND_IMMEDIATE)) out_frame('M', &id, 8, msg[i]->data, (size_t)msg[i]->size);
            S.Release(msg[i]);
        }
        if (tick % 4000 == 0) {  // (~every 4 s: friends' rich presence kept fresh)
            int nf = S.FriendCount(g_friends, FRIEND_IMMEDIATE);
            for (int i = 0; i < nf; i++) S.RequestRichPresence(g_friends, S.FriendByIndex(g_friends, i, FRIEND_IMMEDIATE));
        }
        if (tick % 500 == 0) {  // (~every 0.5 s: who is hosting, for the game's scans)
            static uint8_t list[64 * 300];
            size_t k = 0;
            int nf = S.FriendCount(g_friends, FRIEND_IMMEDIATE);
            for (int i = 0; i < nf && k + 300 <= sizeof list; i++) {
                uint64_t f = S.FriendByIndex(g_friends, i, FRIEND_IMMEDIATE);
                const char *rp = S.FriendRichPresence(g_friends, f, relay_key());
                if (!rp || strcmp(rp, "hosting")) continue;
                const char *name = S.FriendName(g_friends, f);
                size_t nl = name ? strlen(name) : 0;
                if (nl > 255) nl = 255;
                memcpy(list + k, &f, 8), list[k + 8] = (uint8_t)nl, memcpy(list + k + 9, name, nl);
                k += 9 + nl;
            }
            out_frame('H', list, k, NULL, 0);
        }
        pthread_mutex_unlock(&g_steam_lock);
        if (!n) nap_ms(1);
    }
    // (Steam asserts - and sends Valve a crash report - when a process leaves with its networking sockets open)
    S.Shutdown();
    return 0;
}

#else
// ---- the fake relay (LARECOMP_RELAY_FAKE=<this port>,<peer port>): UDP on 127.0.0.1, the peer id = its port
static sock_t g_fake = NO_SOCK;
static uint16_t g_fake_peer;

// ---- the helper (plugins/steamrelay-helper[.exe]): started with pipes for its stdin / stdout, its stderr the game's log
#ifdef _WIN32
static HANDLE g_to_helper = INVALID_HANDLE_VALUE, g_from_helper = INVALID_HANDLE_VALUE, g_helper_proc;
static bool to_helper_raw(const void *p, size_t n) {
    DWORD w;
    return WriteFile(g_to_helper, p, (DWORD)n, &w, NULL) && w == n;
}
static bool from_helper(void *p, size_t n) {
    for (size_t done = 0; done < n;) {
        DWORD r = 0;
        if (!ReadFile(g_from_helper, (char *)p + done, (DWORD)(n - done), &r, NULL) || !r) return false;
        done += r;
    }
    return true;
}
static bool helper_start(void) {
    HMODULE m = NULL;
    char path[MAX_PATH];
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            (LPCSTR)(void *)helper_start, &m) || !GetModuleFileNameA(m, path, sizeof path)) return false;
    char *slash = strrchr(path, '\\');
    if (!slash || (size_t)(slash - path) + 32 > sizeof path) return false;
    strcpy(slash + 1, "steamrelay-helper.exe");
    SECURITY_ATTRIBUTES sa = {sizeof sa, NULL, TRUE};
    HANDLE in_r, out_w, err = NULL;
    if (!CreatePipe(&in_r, &g_to_helper, &sa, 0)) return false;
    if (!CreatePipe(&g_from_helper, &out_w, &sa, 0)) return false;
    SetHandleInformation(g_to_helper, HANDLE_FLAG_INHERIT, 0), SetHandleInformation(g_from_helper, HANDLE_FLAG_INHERIT, 0);
    HANDLE log = (HANDLE)_get_osfhandle(_fileno(stderr));
    if (log != INVALID_HANDLE_VALUE) DuplicateHandle(GetCurrentProcess(), log, GetCurrentProcess(), &err, 0, TRUE, DUPLICATE_SAME_ACCESS);
    STARTUPINFOA si = {.cb = sizeof si, .dwFlags = STARTF_USESTDHANDLES, .hStdInput = in_r, .hStdOutput = out_w, .hStdError = err};
    PROCESS_INFORMATION pi;
    char cmd[MAX_PATH + 4];
    snprintf(cmd, sizeof cmd, "\"%s\"", path);
    BOOL ok = CreateProcessA(path, cmd, NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi);
    CloseHandle(in_r), CloseHandle(out_w);
    if (err) CloseHandle(err);
    if (!ok) { LOG("[steamrelay] can't start %s (error %lu)\n", path, GetLastError()); return false; }
    CloseHandle(pi.hThread);
    g_helper_proc = pi.hProcess;
    return true;
}
static void helper_stop(void) {
    if (g_to_helper != INVALID_HANDLE_VALUE) CloseHandle(g_to_helper), g_to_helper = INVALID_HANDLE_VALUE;
    if (g_helper_proc) WaitForSingleObject(g_helper_proc, 3000), CloseHandle(g_helper_proc), g_helper_proc = NULL;
}
#else
static int g_to_helper = -1, g_from_helper = -1;
static pid_t g_helper_pid;
static bool to_helper_raw(const void *p, size_t n) {
    for (size_t done = 0; done < n;) {
        ssize_t w = write(g_to_helper, (const char *)p + done, n - done);
        if (w < 0 && errno == EINTR) continue;
        if (w <= 0) return false;
        done += (size_t)w;
    }
    return true;
}
static bool from_helper(void *p, size_t n) {
    for (size_t done = 0; done < n;) {
        ssize_t r = read(g_from_helper, (char *)p + done, n - done);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) return false;
        done += (size_t)r;
    }
    return true;
}
static bool helper_start(void) {
    Dl_info info;
    if (!dladdr((void *)helper_start, &info) || !info.dli_fname) return false;
    char path[4096];
    snprintf(path, sizeof path, "%s", info.dli_fname);
    char *slash = strrchr(path, '/');
    if (slash) snprintf(slash + 1, sizeof path - (size_t)(slash + 1 - path), "steamrelay-helper");
    else snprintf(path, sizeof path, "./steamrelay-helper");
    int in[2], out[2];
    if (pipe2(in, O_CLOEXEC) || pipe2(out, O_CLOEXEC)) return false;
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, in[0], 0);
    posix_spawn_file_actions_adddup2(&fa, out[1], 1);
    char *argv[] = {path, NULL};
    int e = posix_spawn(&g_helper_pid, path, &fa, NULL, argv, environ);
    posix_spawn_file_actions_destroy(&fa);
    close(in[0]), close(out[1]);
    if (e) { LOG("[steamrelay] can't start %s: %s\n", path, strerror(e)); close(in[1]), close(out[0]); return false; }
    g_to_helper = in[1], g_from_helper = out[0];
    return true;
}
static void helper_stop(void) {
    if (g_to_helper >= 0) close(g_to_helper), g_to_helper = -1;
    for (int i = 0; i < 300 && g_helper_pid; i++) {  // (up to 3 s for it to shut Steam down)
        if (waitpid(g_helper_pid, NULL, WNOHANG) == g_helper_pid) g_helper_pid = 0;
        else nap_ms(10);
    }
}
#endif
static pthread_mutex_t g_to_lock = PTHREAD_MUTEX_INITIALIZER;
static void to_helper(char type, const void *a, size_t an, const void *b, size_t bn) {
    uint8_t h[5] = {(uint8_t)type};
    uint32_t n = (uint32_t)(an + bn);
    memcpy(h + 1, &n, 4);
    pthread_mutex_lock(&g_to_lock);
    if (to_helper_raw(h, 5) && an) to_helper_raw(a, an);
    if (bn) to_helper_raw(b, bn);
    pthread_mutex_unlock(&g_to_lock);
}

// ---- the friends hosting, as the helper last listed them
static struct { uint64_t id; char name[64]; } g_hosts[64];
static int g_nhosts;
static pthread_mutex_t g_hosts_lock = PTHREAD_MUTEX_INITIALIZER;

// ---- the relay
static void relay_send(uint32_t to, uint16_t from_port, uint16_t to_port, const void *p, size_t n) {
    int k = peer_k(to);
    uint64_t id = k ? g_peer[k] : 0;
    if (!id || n > 65000) return;
    to_port = real_port(k, to_port);
    uint8_t *m = malloc(n + 4);
    if (!m) return;
    m[0] = (uint8_t)(from_port >> 8), m[1] = (uint8_t)from_port, m[2] = (uint8_t)(to_port >> 8), m[3] = (uint8_t)to_port;
    memcpy(m + 4, p, n);
    if (g_fake != NO_SOCK) {
        struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = htons((uint16_t)id), .sin_addr.s_addr = htonl(0x7F000001u)};
        sendto(g_fake, (const char *)m, (int)(n + 4), 0, (struct sockaddr *)&a, sizeof a);
    } else {
        to_helper('S', &id, 8, m, n + 4);
    }
    free(m);
}
static int relay_targets(uint32_t *out, int max) {
    int n = 0;
    if (g_fake != NO_SOCK) {
        if (max > 0) out[n++] = peer_addr(peer_of(g_fake_peer));
        return n;
    }
    pthread_mutex_lock(&g_hosts_lock);
    for (int i = 0; i < g_nhosts && n < max; i++) {
        out[n++] = peer_addr(peer_of(g_hosts[i].id));
        LOG("[steamrelay] %s is hosting\n", g_hosts[i].name);
    }
    pthread_mutex_unlock(&g_hosts_lock);
    return n;
}
static void relay_hosting(int on) {
    if (g_fake == NO_SOCK) {
        uint8_t b = on ? 1 : 0;
        to_helper('P', &b, 1, NULL, 0);
    }
    LOG("[steamrelay] %s\n", on ? "hosting: friends see this network when they look for one" : "not hosting");
}
static uint64_t relay_id_of(uint32_t addr) { int k = peer_k(addr); return k ? g_peer[k] : 0; }
static uint32_t relay_addr_of(uint64_t id) { int k = peer_of(id); return k ? peer_addr(k) : 0; }
static const LdnRelay k_relay = {relay_send, relay_targets, relay_hosting, relay_id_of, relay_addr_of};

static void deliver(uint64_t from, const uint8_t *m, size_t n) {
    if (n < 4) return;
    int k = peer_of(from);
    if (k) replay(k, (uint16_t)(m[0] << 8 | m[1]), (uint16_t)(m[2] << 8 | m[3]), m + 4, n - 4);
}

static volatile int g_quit;
static void *pump(void *arg) {
    (void)arg;
    static uint8_t m[70000];
    while (!g_quit) {
        if (g_fake != NO_SOCK) {
            struct sockaddr_in a;
            socklen_t al = sizeof a;
            int n;
            while ((n = (int)recvfrom(g_fake, (char *)m, sizeof m, 0, (struct sockaddr *)&a, &al)) > 0) deliver(ntohs(a.sin_port), m, (size_t)n);
            nap_ms(1);
            continue;
        }
        uint8_t h[5];
        uint32_t n;
        if (!from_helper(h, 5)) break;  // (the helper ended)
        memcpy(&n, h + 1, 4);
        if (n > sizeof m || !from_helper(m, n)) break;
        if (h[0] == 'M' && n >= 8) {
            uint64_t from;
            memcpy(&from, m, 8);
            deliver(from, m + 8, n - 8);
        } else if (h[0] == 'H') {
            pthread_mutex_lock(&g_hosts_lock);
            g_nhosts = 0;
            for (uint32_t k = 0; k + 9 <= n && g_nhosts < 64;) {
                size_t nl = m[k + 8];
                if (k + 9 + nl > n) break;
                memcpy(&g_hosts[g_nhosts].id, m + k, 8);
                size_t c = nl < 63 ? nl : 63;
                memcpy(g_hosts[g_nhosts].name, m + k + 9, c), g_hosts[g_nhosts].name[c] = 0;
                g_nhosts++, k += 9 + (uint32_t)nl;
            }
            pthread_mutex_unlock(&g_hosts_lock);
        }
    }
    if (g_fake == NO_SOCK && !g_quit) LOG("[steamrelay] the Steam helper ended: online play off until the game starts again\n");
    return NULL;
}

// At exit: the helper told to stop (its stdin closed: it shuts Steam down and ends), then the pump
static pthread_t g_pump;
static void quit(void) {  // (hooks.shutdown: from exit())
    g_quit = 1;
    if (g_fake == NO_SOCK) helper_stop();
    pthread_join(g_pump, NULL);
}

int larecomp_plugin_init(const LarecompPluginApi *api, LarecompPluginHooks *hooks) {
    A = api;
    if (api->abi < 3 || !api->ldn_relay_set || !api->ldn_relay_local) {
        LOG("[steamrelay] this game build has no relay interface (plugin API 3): update the game\n");
        return 1;
    }
    ldn_relay_local = api->ldn_relay_local;
#ifdef _WIN32
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
    const char *fake = getenv("LARECOMP_RELAY_FAKE");
    unsigned mine, peer;
    if (fake && sscanf(fake, "%u,%u", &mine, &peer) == 2) {
        g_fake = socket(AF_INET, SOCK_DGRAM, 0);
        struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = htons((uint16_t)mine), .sin_addr.s_addr = htonl(0x7F000001u)};
        if (g_fake == NO_SOCK || bind(g_fake, (struct sockaddr *)&a, sizeof a)) { LOG("[steamrelay] fake relay: can't bind %u\n", mine); return 1; }
        nonblocking(g_fake);
        g_fake_peer = (uint16_t)peer;
        LOG("[steamrelay] fake relay (tests): port %u, the peer at %u is 127.0.4.%d\n", mine, peer, peer_of(peer));
    } else {
        env_set("LARECOMP_RELAY_KEY", api->title_id && api->title_id() == 0x01001F5010DFA000ull ? "plarecomp" : "lzarecomp");
        if (!helper_start()) { LOG("[steamrelay] no Steam helper (plugins/steamrelay-helper): no relay\n"); return 1; }
        uint8_t h[5], b[256];
        uint32_t n = 0;
        bool got = from_helper(h, 5);
        if (got) memcpy(&n, h + 1, 4);
        if (!got || n > sizeof b - 1 || !from_helper(b, n) || h[0] != 'R' || n < 8) {
            b[got && n < sizeof b ? n : 0] = 0;
            LOG("[steamrelay] %s: no relay\n", got && h[0] == 'F' ? (const char *)b : "the Steam helper didn't start");
            helper_stop();
            return 1;
        }
        uint64_t me;
        memcpy(&me, b, 8);
        LOG("[steamrelay] online play with friends on, as Spacewar (in the helper: the game keeps its own Steam Input "
            "controller setup) - id %llu\n", (unsigned long long)me);
    }
    api->ldn_relay_set(&k_relay);
    hooks->shutdown = quit;
    pthread_create(&g_pump, NULL, pump, NULL);
    return 0;
}
#endif
