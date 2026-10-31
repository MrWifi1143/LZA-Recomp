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
#include <arpa/inet.h>
#include <dlfcn.h>
#include <glob.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "../../runtime/plugin.h"

static const LarecompPluginApi *A;
#define LOG(...) A->log(__VA_ARGS__)

// ---- runtime/services/ldn.c's relay interface (the executable exports it)
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
static struct { int k; uint16_t port, local; int fd; } g_out[512];
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
    int fd = -1;
    pthread_mutex_lock(&g_out_lock);
    for (int i = 0; i < g_nout && fd < 0; i++)
        if (g_out[i].k == k && g_out[i].port == from_port) fd = g_out[i].fd;
    if (fd < 0) {
        fd = socket(AF_INET, SOCK_DGRAM, 0);
        struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = htons(from_port), .sin_addr.s_addr = htonl(peer_addr(k))};
        int one = 1;
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
        if (fd >= 0 && bind(fd, (struct sockaddr *)&a, sizeof a)) {
            a.sin_port = 0;
            if (bind(fd, (struct sockaddr *)&a, sizeof a)) close(fd), fd = -1;
        }
        socklen_t al = sizeof a;
        if (fd < 0 || getsockname(fd, (struct sockaddr *)&a, &al)) {
            LOG("[steamrelay] can't send from 127.0.4.%d here\n", k);
            if (fd >= 0) close(fd);
            pthread_mutex_unlock(&g_out_lock);
            return;
        }
        if (ntohs(a.sin_port) != from_port) LOG("[steamrelay] 127.0.4.%d:%u stands in for the peer's port %u\n", k, ntohs(a.sin_port), from_port);
        if (g_nout < (int)(sizeof g_out / sizeof *g_out))
            g_out[g_nout].k = k, g_out[g_nout].port = from_port, g_out[g_nout].local = ntohs(a.sin_port), g_out[g_nout++].fd = fd;
    }
    pthread_mutex_unlock(&g_out_lock);
    struct sockaddr_in to = {.sin_family = AF_INET, .sin_port = htons(to_port), .sin_addr.s_addr = htonl(ldn_relay_local())};
    sendto(fd, p, n, 0, (struct sockaddr *)&to, sizeof to);
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

static bool load_steam(void) {
    char found[1024] = "";
    const char *e = getenv("LARECOMP_STEAM_API");
    if (e) snprintf(found, sizeof found, "%s", e);
    else {  // (the Steam install's libraries: SteamVR's copy, then any game's)
        const char *home = getenv("HOME");
        char pat[1100];
        const char *const where[] = {"%s/.local/share/Steam/steamapps/common/SteamVR/bin/linux64/libsteam_api.so",
                                     "%s/.steam/steam/steamapps/common/SteamVR/bin/linux64/libsteam_api.so",
                                     "%s/.local/share/Steam/steamapps/common/*/libsteam_api.so",
                                     "%s/.local/share/Steam/steamapps/common/*/bin/libsteam_api.so",
                                     "%s/.local/share/Steam/steamapps/common/*/bin/linux64/libsteam_api.so"};
        for (size_t i = 0; i < sizeof where / sizeof *where && !found[0] && home; i++) {
            snprintf(pat, sizeof pat, where[i], home);
            glob_t g;
            if (!glob(pat, 0, NULL, &g) && g.gl_pathc) snprintf(found, sizeof found, "%s", g.gl_pathv[0]);
            globfree(&g);
        }
    }
    void *h = found[0] ? dlopen(found, RTLD_NOW | RTLD_LOCAL) : NULL;
    if (!h) { LOG("[steamrelay] no libsteam_api.so (Steam installed? or LARECOMP_STEAM_API=<path>)\n"); return false; }
#define SYM(f, name) ((*(void **)&S.f = dlsym(h, name)) || (LOG("[steamrelay] %s: no %s\n", found, name), 0))
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
    if (!ok) return false;
    setenv("SteamAppId", "480", 1), setenv("SteamGameId", "480", 1);  // (Spacewar)
    if (!S.InitSafe()) { LOG("[steamrelay] Steam isn't running (or not logged in): no relay\n"); return false; }
    S.MD_Init();  // (after the init: callbacks are taken by the pump thread)
    g_msgs = S.Messages(), g_friends = S.Friends();
    LOG("[steamrelay] Steam (%s): online play with friends on, as Spacewar - id %llu\n", found,
        (unsigned long long)S.MySteamID(S.User()));
    return g_msgs && g_friends;
}

// ---- the fake relay (LARECOMP_RELAY_FAKE=<this port>,<peer port>): UDP on 127.0.0.1, the peer id = its port
static int g_fake = -1;
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
    if (g_fake >= 0) {
        struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = htons((uint16_t)id), .sin_addr.s_addr = htonl(0x7F000001u)};
        sendto(g_fake, m, n + 4, 0, (struct sockaddr *)&a, sizeof a);
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
    if (g_fake >= 0) {
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
    if (g_fake < 0) S.SetRichPresence(g_friends, "lzarecomp", on ? "hosting" : "");
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
    int32_t pipe = g_fake < 0 ? S.GetHSteamPipe() : 0;
    for (unsigned tick = 0; !g_quit; tick++) {
        if (g_fake >= 0) {
            uint8_t m[66000];
            struct sockaddr_in a;
            socklen_t al = sizeof a;
            ssize_t n;
            while ((n = recvfrom(g_fake, m, sizeof m, MSG_DONTWAIT, (struct sockaddr *)&a, &al)) > 0) deliver(ntohs(a.sin_port), m, (size_t)n);
            usleep(500);
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
        if (!n) usleep(1000);
    }
    return NULL;
}

// At exit: the pump stopped, then Steam shut down (Steam asserts - and sends Valve a crash report - when a process
// leaves with its networking sockets open)
static pthread_t g_pump;
static void quit(void) {
    g_quit = 1;
    pthread_join(g_pump, NULL);
    if (g_fake < 0) S.Shutdown();
}

int larecomp_plugin_init(const LarecompPluginApi *api, LarecompPluginHooks *hooks) {
    (void)hooks;
    A = api;
    void (*set)(const LdnRelay *) = (void (*)(const LdnRelay *))dlsym(RTLD_DEFAULT, "ldn_relay_set");
    ldn_relay_local = (uint32_t(*)(void))dlsym(RTLD_DEFAULT, "ldn_relay_local");
    if (!set || !ldn_relay_local) { LOG("[steamrelay] this runtime has no relay interface (ldn_relay_set)\n"); return 1; }
    const char *fake = getenv("LARECOMP_RELAY_FAKE");
    unsigned mine, peer;
    if (fake && sscanf(fake, "%u,%u", &mine, &peer) == 2) {
        g_fake = socket(AF_INET, SOCK_DGRAM, 0);
        struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = htons((uint16_t)mine), .sin_addr.s_addr = htonl(0x7F000001u)};
        if (g_fake < 0 || bind(g_fake, (struct sockaddr *)&a, sizeof a)) { LOG("[steamrelay] fake relay: can't bind %u\n", mine); return 1; }
        g_fake_peer = (uint16_t)peer;
        LOG("[steamrelay] fake relay (tests): port %u, the peer at %u is 127.0.4.%d\n", mine, peer, peer_of(peer));
    } else if (!load_steam()) return 1;
    set(&k_relay);
    pthread_create(&g_pump, NULL, pump, NULL);
    atexit(quit);
    return 0;
}
