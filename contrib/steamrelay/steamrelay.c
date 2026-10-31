// Steam relay: PL:Z-A Recomp's local play (Link Trades, Private Battles - "Nearby" in Link Play) with friends anywhere,
// over Steam's relay network. A runtime plugin (runtime/plugin.h): `make steamrelay` builds build/plugins/libsteamrelay.so;
// put it in <data root>/plugins/ (or LARECOMP_PLUGINS=<path>), with Steam running and logged in. The start screen's Mods
// page switches it on and off. It runs as Spacewar (app 480, Valve's test app every
// Steam account has): Steam shows "Playing Spacewar" while the game runs.
//
// How: runtime/services/ldn.c treats a relay peer as a player at 127.0.4.<k> (ldn_relay_set). What the game sends there
// comes here and goes to that friend as a Steam networking message (port numbers first: [from port][to port][data]);
// what a friend sends is replayed over loopback from 127.0.4.<k>:<their port> to this copy, where ldn.c receives it as
// from any LAN player. A host advertises itself in its Steam rich presence (key "lzarecomp" = "hosting"); a scan asks
// every friend showing it. Only friends: messages from anyone else are refused (their sessions not accepted).
//
// The Steam API is not shipped: libsteam_api.so is loaded from the Steam install (Steam's own copy - SteamVR's - or any
// game's), or LARECOMP_STEAM_API=<path>. LARECOMP_RELAY_FAKE=<this port>,<peer port> (tests): no Steam - the relay's
// messages go over UDP on 127.0.0.1 between two copies of the game, the peer at 127.0.4.1.
#define _GNU_SOURCE
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
typedef SOCKET sock_t;
#define NO_SOCK INVALID_SOCKET
#define sock_close closesocket
#define strcasecmp _stricmp
static void *lib_open(const char *p) { return (void *)LoadLibraryA(p); }
static void *lib_sym(void *h, const char *n) { return (void *)GetProcAddress((HMODULE)h, n); }
static void lib_close(void *h) { FreeLibrary((HMODULE)h); }
static void nap_ms(unsigned ms) { Sleep(ms); }
static void env_set(const char *k, const char *v) { SetEnvironmentVariableA(k, v), _putenv_s(k, v); }
static void nonblocking(sock_t s) { u_long on = 1; ioctlsocket(s, FIONBIO, &on); }
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
typedef int sock_t;
#define NO_SOCK (-1)
#define sock_close close
static void *lib_open(const char *p) { return dlopen(p, RTLD_NOW | RTLD_LOCAL); }
static void *lib_sym(void *h, const char *n) { return dlsym(h, n); }
static void lib_close(void *h) { dlclose(h); }
static void nap_ms(unsigned ms) { usleep(ms * 1000); }
static void env_set(const char *k, const char *v) { setenv(k, v, 1); }
static void nonblocking(sock_t s) { fcntl(s, F_SETFL, fcntl(s, F_GETFL) | O_NONBLOCK); }
#define STEAM_API "libsteam_api.so"
#endif

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
    env_set("SteamAppId", "480"), env_set("SteamGameId", "480");  // (Spacewar)
    if (!S.InitSafe()) { LOG("[steamrelay] Steam isn't running (or not logged in): no relay\n"); return false; }
    S.MD_Init();  // (after the init: callbacks are taken by the pump thread)
    g_msgs = S.Messages(), g_friends = S.Friends();
    LOG("[steamrelay] Steam (%s): online play with friends on, as Spacewar - id %llu\n", cand[i],
        (unsigned long long)S.MySteamID(S.User()));
    return g_msgs && g_friends;
}

// ---- the fake relay (LARECOMP_RELAY_FAKE=<this port>,<peer port>): UDP on 127.0.0.1, the peer id = its port
static sock_t g_fake = NO_SOCK;
static uint16_t g_fake_peer;

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
        Identity who;
        memset(&who, 0, sizeof who);
        S.SetSteamID64(&who, id);
        S.Send(g_msgs, &who, m, (uint32_t)(n + 4), SEND_UNRELIABLE_NO_NAGLE_AUTORESTART, 0);
    }
    free(m);
}
static int relay_targets(uint32_t *out, int max) {
    int n = 0;
    if (g_fake != NO_SOCK) {
        if (max > 0) out[n++] = peer_addr(peer_of(g_fake_peer));
        return n;
    }
    int nf = S.FriendCount(g_friends, FRIEND_IMMEDIATE);
    for (int i = 0; i < nf && n < max; i++) {
        uint64_t f = S.FriendByIndex(g_friends, i, FRIEND_IMMEDIATE);
        const char *rp = S.FriendRichPresence(g_friends, f, "lzarecomp");
        if (rp && !strcmp(rp, "hosting")) {
            out[n++] = peer_addr(peer_of(f));
            LOG("[steamrelay] %s is hosting\n", S.FriendName(g_friends, f));
        }
    }
    return n;
}
static void relay_hosting(int on) {
    if (g_fake == NO_SOCK) S.SetRichPresence(g_friends, "lzarecomp", on ? "hosting" : "");
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
    int32_t pipe = g_fake == NO_SOCK ? S.GetHSteamPipe() : 0;
    for (unsigned tick = 0; !g_quit; tick++) {
        if (g_fake != NO_SOCK) {
            uint8_t m[66000];
            struct sockaddr_in a;
            socklen_t al = sizeof a;
            int n;
            while ((n = (int)recvfrom(g_fake, (char *)m, sizeof m, 0, (struct sockaddr *)&a, &al)) > 0) deliver(ntohs(a.sin_port), m, (size_t)n);
            nap_ms(1);
            continue;
        }
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
            if (S.HasFriend(g_friends, id, FRIEND_IMMEDIATE)) deliver(id, msg[i]->data, (size_t)msg[i]->size);
            S.Release(msg[i]);
        }
        if (tick % 4000 == 0) {  // (~every 4 s: friends' rich presence kept fresh for the next scan)
            int nf = S.FriendCount(g_friends, FRIEND_IMMEDIATE);
            for (int i = 0; i < nf; i++) S.RequestRichPresence(g_friends, S.FriendByIndex(g_friends, i, FRIEND_IMMEDIATE));
        }
        if (!n) nap_ms(1);
    }
    return NULL;
}

// At exit: the pump stopped, then Steam shut down (Steam asserts - and sends Valve a crash report - when a process
// leaves with its networking sockets open)
static pthread_t g_pump;
static void quit(void) {  // (hooks.shutdown: from exit())
    g_quit = 1;
    pthread_join(g_pump, NULL);
    if (g_fake == NO_SOCK) S.Shutdown();
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
    } else if (!load_steam()) return 1;
    api->ldn_relay_set(&k_relay);
    hooks->shutdown = quit;
    pthread_create(&g_pump, NULL, pump, NULL);
    return 0;
}
