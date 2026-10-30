// Local wireless play (ldn:u) and the sockets it carries (bsd:u / bsd:s) between copies of the game - on one PC or
// on the LAN: Link Play's "Nearby Players" (link trades, private battles). The game's own nn::ldn calls (create / scan /
// connect / the network's info) are answered here, and Pia's UDP sockets carry its data between the copies.
//
// Each copy is a console of its own. The first on a PC (slot 1) listens on all of the PC's addresses: copies on other
// PCs find it by broadcast (or LARECOMP_LDN_PEERS); more copies on the same PC (tests) are reached on 127.0.1.N.
// In a network the game sees the Switch-like addresses 169.254.1.(node + 1); its sockets bind where this copy listens
// and send to the nodes' real addresses. A socket can only be made while the copy is in a network - otherwise it
// still fails with ENETDOWN: nothing ever goes to the internet.
// Control messages, UDP port LDN_CTRL_PORT ("LDN1" + type ...): 'S' scan -> 's' + NetworkInfo from a host; 'C' join
// (user name, version) -> 'A' node + NetworkInfo + the nodes' addresses / 'R' refused; 'I' + NetworkInfo + addresses
// (the host to its stations, on a change); 'L' node (a station leaves); 'D' (the host closed the network).
// Structures (NetworkInfo 0x480 ...) as switchbrew / Ryujinx's Ldn types document them. Windows: the same, on Winsock.
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../hle.h"
#include "../ipc.h"
#include "../platform.h"
#include "../rt.h"

#include <errno.h>
#include <pthread.h>
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

void kernel_block_begin(void);
Cpu *kernel_current_cpu(void);
void kernel_block_end(void);

enum { ST_NONE, ST_INIT, ST_AP, ST_AP_CREATED, ST_STATION, ST_STATION_CONNECTED, ST_ERROR };
#define LDN_CTRL_PORT 39100
#define NODES 8

#pragma pack(push, 1)
typedef struct { uint64_t local_id; uint16_t r1, scene_id; uint32_t r2; } IntentId;            // 0x10
typedef struct { IntentId intent; uint8_t session[16]; } NetworkId;                            // 0x20
typedef struct { uint8_t len; char name[33]; } Ssid;                                           // 0x22
typedef struct { uint8_t mac[6]; Ssid ssid; uint16_t channel; uint8_t link, type; uint32_t r; } CommonInfo;  // 0x30
typedef struct {
    uint32_t ip;  // (host order: 169.254.1.1 = 0xA9FE0101)
    uint8_t mac[6], node_id, connected;
    char name[33];
    uint8_t r1;
    uint16_t version;
    uint8_t r2[16];
} NodeInfo;  // 0x40
typedef struct {
    uint8_t sec_param[16];
    uint16_t sec_mode;
    uint8_t accept_policy, unk1;
    uint16_t r1;
    uint8_t max_nodes, nodes_n;
    NodeInfo nodes[NODES];
    uint16_t r2, adv_size;
    uint8_t adv[0x180], unk2[0x8C];
    uint64_t auth_id;
} LdnInfo;  // 0x430
typedef struct { NetworkId id; CommonInfo common; LdnInfo ldn; } NetworkInfo;  // 0x480
typedef struct { uint16_t sec_mode, pass_len; uint8_t pass[0x40]; } SecurityConfig;  // 0x44
typedef struct { char name[33]; uint8_t r[15]; } UserConfig;                        // 0x30
typedef struct { IntentId intent; uint16_t channel; uint8_t max_nodes, r1; uint16_t version; uint8_t r2[10]; } NetworkConfig;  // 0x20
typedef struct { NetworkId id; uint32_t type; uint8_t bssid[6]; Ssid ssid; uint8_t r[16]; uint32_t flags; } ScanFilter;  // 0x60
#pragma pack(pop)
_Static_assert(sizeof(NetworkInfo) == 0x480, "NetworkInfo");
_Static_assert(sizeof(NodeInfo) == 0x40, "NodeInfo");
_Static_assert(sizeof(ScanFilter) == 0x60, "ScanFilter");

static uint32_t signalled_event(void) {  // (as network.c's: a wait on it ends at once)
    uint32_t w, r;
    KEvent *ev = kernel_create_event_pair(&w, &r);
    pthread_mutex_lock(&g_klock);
    kernel_signal(ev);
    pthread_mutex_unlock(&g_klock);
    return r;
}

// IClientProcessMonitor (ldn:u 1, 18.0.0+; nn::ldn::Initialize crashed without it): RegisterClient (0), nothing to do
static void client_monitor(Service *s, IpcReq *req, IpcResp *resp) {
    (void)s, (void)resp;
    if (req->cmd) LOG("[ldn] IClientProcessMonitor cmd %u (ok)\n", req->cmd);
}

// ------------------------------------------------------------------ sockets: POSIX / Winsock
// (Windows: socket handles kept as ints - small numbers in practice, Wine's too; the service reports Linux errno
// values, so Winsock's errors are turned into them; no MSG_DONTWAIT there: a poll first)
#ifdef _WIN32
#ifndef SIO_UDP_CONNRESET
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
#endif
#define close_sock(h) closesocket((SOCKET)(h))
static int sock_poll(struct pollfd *p, unsigned n, int ms) { return WSAPoll(p, n, ms); }
static int sock_errno(void) {
    switch (WSAGetLastError()) {
    case WSAEWOULDBLOCK: return 11;  // EAGAIN
    case WSAEBADF: case WSAENOTSOCK: return 9;
    case WSAEINVAL: return 22;
    case WSAEMFILE: return 24;
    case WSAEMSGSIZE: return 90;
    case WSAEADDRINUSE: return 98;
    case WSAEADDRNOTAVAIL: return 99;
    case WSAENETDOWN: return 100;
    case WSAENETUNREACH: return 101;
    case WSAECONNRESET: return 104;
    case WSAENOTCONN: return 107;
    case WSAETIMEDOUT: return 110;
    case WSAECONNREFUSED: return 111;
    case WSAEHOSTUNREACH: return 113;
    default: return 5;  // EIO
    }
}
static void net_init(void) {
    static bool done;
    if (done) return;
    WSADATA w;
    done = WSAStartup(MAKEWORD(2, 2), &w) == 0;
}
static int udp_socket(void) {  // (a UDP socket that doesn't fail its receives over a peer's ICMP "port unreachable")
    net_init();
    SOCKET h = socket(AF_INET, SOCK_DGRAM, 0);
    if (h == INVALID_SOCKET) return -1;
    BOOL off = FALSE;
    DWORD got;
    WSAIoctl(h, SIO_UDP_CONNRESET, &off, sizeof off, NULL, 0, &got, NULL, NULL);
    return (int)h;
}
#else
#define close_sock(h) close(h)
static int sock_poll(struct pollfd *p, unsigned n, int ms) { return poll(p, (nfds_t)n, ms); }
static int sock_errno(void) { return errno; }
static int udp_socket(void) { return socket(AF_INET, SOCK_DGRAM, 0); }
#endif


static bool ldn_log(void) {  // (LARECOMP_LDN_LOG=1: every ldn:u / bsd:u command)
    static int on = -1;
    if (on < 0) on = getenv("LARECOMP_LDN_LOG") != NULL;
    return on;
}

// ------------------------------------------------------------------ the network (one per copy of the game)
// A copy's place on its PC: slot N (1..LDN_PEERS), held by a lock socket on 127.0.2.N. Slot 1 listens on every address
// (the control port and the game's ports): other PCs reach it - one copy per PC plays over the LAN. Slots 2.. listen on
// 127.0.1.N only (more copies on one PC: tests). Here a copy is 127.0.1.N either way.
// In a network the host numbers the nodes (address 169.254.1.(node + 1)) and sends everyone the nodes' real addresses
// as it reached them (0: the host itself - each station knows where it found the host).
#define LDN_PEERS 32
#define LOOPBACK(ip) (((ip) >> 24) == 127)
static struct {
    pthread_mutex_t lock;
    int state;
    int me;                 // this copy's slot, 0 none yet
    int node;               // this copy's node in the network (0 the host), -1 none
    NetworkInfo info;       // the network we host or joined
    uint32_t real[NODES];   // the nodes' real addresses (host order; 0: out of reach)
    uint32_t host_addr;     // (a station) where it found its host
    uint16_t disconnect_reason;
    KEvent *event;          // the game's state change event (AttachStateChangeEvent)
    int ctrl;               // control socket, -1 none
    uint8_t changes[NODES]; // NodeLatestUpdate: 1 joined, 2 left - since the game last read them
    struct { uint8_t session[16]; uint32_t addr; } found[NODES];  // the last scan's hosts
    int nfound;
} L = {.lock = PTHREAD_MUTEX_INITIALIZER, .state = ST_NONE, .node = -1, .ctrl = -1};

static uint32_t my_addr(void) { return 0x7F000100u | (uint32_t)L.me; }   // 127.0.1.N
static uint32_t bind_addr(void) { return L.me == 1 ? 0 : my_addr(); }    // (slot 1: every address)
static uint32_t canon(uint32_t ip) { return ip == 0x7F000001u ? 0x7F000101u : ip; }  // (slot 1 sends from 127.0.0.1)
static uint32_t node_vip(int k) { return 0xA9FE0101u + (uint32_t)k; }   // 169.254.1.(k + 1)
static uint32_t my_vip(void) { return L.node >= 0 ? node_vip(L.node) : 0; }
static bool in_subnet(uint32_t ip) { return (ip & 0xFFFFFF00u) == 0xA9FE0100u; }
static int vip_node(uint32_t ip) { return in_subnet(ip) && (ip & 0xFF) >= 1 && (ip & 0xFF) <= NODES ? (int)(ip & 0xFF) - 1 : -1; }
static uint64_t local_id(uint64_t id) { return id == ~0ull ? g_program_id : id; }  // (-1: the game's own)
static const char *ip_str(uint32_t ip, char *b) {
    snprintf(b, 16, "%u.%u.%u.%u", ip >> 24, ip >> 16 & 255, ip >> 8 & 255, ip & 255);
    return b;
}

static void signal_change(void) {  // (caller may hold L.lock)
    if (!L.event) return;
    pthread_mutex_lock(&g_klock);
    kernel_signal(L.event);
    pthread_mutex_unlock(&g_klock);
}

// (reuse: slot 1's wildcard sockets and the other slots' 127.0.1.N ones share ports - each must allow it)
static int udp_bound(uint32_t ip, uint16_t port, bool reuse) {
    int fd = udp_socket();
    if (fd < 0) return -1;
    int one = 1;
    if (reuse) setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (const void *)&one, sizeof one);
    setsockopt(fd, SOL_SOCKET, SO_BROADCAST, (const void *)&one, sizeof one);
    struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = htons(port), .sin_addr.s_addr = htonl(ip)};
    if (bind(fd, (struct sockaddr *)&a, sizeof a)) { close_sock(fd); return -1; }
    return fd;
}
static void ctrl_send(int fd, uint32_t ip, char type, const void *body, size_t n) {
    uint8_t m[8 + sizeof(NetworkInfo) + 64];
    memcpy(m, "LDN1", 4), m[4] = (uint8_t)type;
    if (n > sizeof m - 5) n = sizeof m - 5;
    if (n) memcpy(m + 5, body, n);
    struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = htons(LDN_CTRL_PORT), .sin_addr.s_addr = htonl(ip)};
    sendto(fd, (const void *)m, (int)(5 + n), 0, (struct sockaddr *)&a, sizeof a);
}
// (L.lock held) the network's info and its nodes' addresses, as the host sends them: 0 for itself
static size_t host_packet(uint8_t *out) {
    memcpy(out, &L.info, sizeof L.info);
    uint32_t t[NODES];
    for (int k = 0; k < NODES; k++) t[k] = k ? L.real[k] : 0;
    memcpy(out + sizeof L.info, t, sizeof t);
    return sizeof L.info + sizeof t;
}
static void host_broadcast_info(void) {  // (L.lock held) to every station
    uint8_t b[sizeof(NetworkInfo) + 4 * NODES];
    size_t n = host_packet(b);
    for (int k = 1; k < NODES; k++)
        if (L.info.ldn.nodes[k].connected && L.real[k]) ctrl_send(L.ctrl, L.real[k], 'I', b, n);
}
// (L.lock held, a station) the nodes' addresses from the host's table
static void station_table(const uint8_t *t) {
    for (int k = 0; k < NODES; k++) {
        uint32_t r;
        memcpy(&r, t + 4 * k, 4);
        if (k == 0) r = L.host_addr;
        else if (k == L.node) r = my_addr();
        else if (LOOPBACK(r) && !LOOPBACK(L.host_addr)) r = 0;  // (a second copy on the host's PC: loopback only)
        L.real[k] = r;
    }
}

static void *ctrl_thread(void *arg) {
    (void)arg;
    int fd = L.ctrl;
    for (;;) {
        uint8_t m[8 + sizeof(NetworkInfo) + 64];
        struct sockaddr_in from;
        socklen_t fl = sizeof from;
        ssize_t n = recvfrom(fd, (void *)m, sizeof m, 0, (struct sockaddr *)&from, &fl);
        if (n < 5 || memcmp(m, "LDN1", 4)) continue;
        uint32_t src = canon(ntohl(from.sin_addr.s_addr));
        char sb[16];
        pthread_mutex_lock(&L.lock);
        char t = (char)m[4];
        if (L.state == ST_AP_CREATED && t == 'S') {  // a scan: our network
            uint8_t r[5 + sizeof(NetworkInfo)];
            memcpy(r, "LDN1s", 5), memcpy(r + 5, &L.info, sizeof L.info);
            sendto(fd, (const void *)r, sizeof r, 0, (struct sockaddr *)&from, sizeof from);
        } else if (L.state == ST_AP_CREATED && t == 'C' && n >= 5 + 33 + 2) {  // a join
            int k = 1;
            while (k < L.info.ldn.max_nodes && L.info.ldn.nodes[k].connected) k++;
            uint8_t r[6 + sizeof(NetworkInfo) + 4 * NODES];
            if (k >= L.info.ldn.max_nodes || L.info.ldn.accept_policy == 1) {  // (full / AcceptPolicy RejectAll)
                memcpy(r, "LDN1R", 5);
                sendto(fd, (const void *)r, 5, 0, (struct sockaddr *)&from, sizeof from);
            } else {
                NodeInfo *nd = &L.info.ldn.nodes[k];
                memset(nd, 0, sizeof *nd);
                nd->ip = node_vip(k), nd->node_id = (uint8_t)k, nd->connected = 1;
                nd->mac[0] = 2, nd->mac[4] = (uint8_t)(src >> 8), nd->mac[5] = (uint8_t)src;
                memcpy(nd->name, m + 5, 32), memcpy(&nd->version, m + 5 + 33, 2);
                L.info.ldn.nodes_n++;
                L.real[k] = src;
                L.changes[k] = 1;
                memcpy(r, "LDN1A", 5), r[5] = (uint8_t)k;
                size_t pn = host_packet(r + 6);
                sendto(fd, (const void *)r, (int)(6 + pn), 0, (struct sockaddr *)&from, sizeof from);
                host_broadcast_info();
                LOG("[ldn] \"%s\" joined as node %d (%s)\n", nd->name, k, ip_str(src, sb));
                signal_change();
            }
        } else if (L.state == ST_AP_CREATED && t == 'L' && n >= 6) {  // a station leaves
            int k = m[5];
            if (k > 0 && k < NODES && L.info.ldn.nodes[k].connected && L.real[k] == src) {
                memset(&L.info.ldn.nodes[k], 0, sizeof(NodeInfo));
                L.info.ldn.nodes_n--, L.changes[k] = 2, L.real[k] = 0;
                host_broadcast_info();
                LOG("[ldn] node %d left\n", k);
                signal_change();
            }
        } else if (L.state == ST_STATION_CONNECTED && src == L.host_addr) {  // from our host:
            if (t == 'I' && n >= 5 + (ssize_t)(sizeof(NetworkInfo) + 4 * NODES)) {  // the network changed
                NetworkInfo ni;
                memcpy(&ni, m + 5, sizeof ni);
                for (int k = 0; k < NODES; k++)
                    if (ni.ldn.nodes[k].connected != L.info.ldn.nodes[k].connected) L.changes[k] = ni.ldn.nodes[k].connected ? 1 : 2;
                L.info = ni;
                station_table(m + 5 + sizeof ni);
                signal_change();
            } else if (t == 'D') {  // it closed the network
                L.state = ST_STATION, L.disconnect_reason = 3, L.node = -1;  // (DestroyedByAccessPoint)
                LOG("[ldn] the host closed the network\n");
                signal_change();
            }
        }
        pthread_mutex_unlock(&L.lock);
    }
    return NULL;
}

// (L.lock held) this copy's slot and control socket, once
static bool attach(void) {
    if (L.me) return true;
    for (int k = 1; k <= LDN_PEERS && !L.me; k++)  // (the lock socket stays open for good)
        if (udp_bound(0x7F000200u | (uint32_t)k, LDN_CTRL_PORT + 1, false) >= 0) L.me = k;
    if (!L.me) { LOG("[ldn] no free local slot (1-%d)\n", LDN_PEERS); return false; }
    L.ctrl = udp_bound(bind_addr(), LDN_CTRL_PORT, true);
    if (L.ctrl < 0 && L.me == 1) {
        LOG("[ldn] can't listen on port %d of this PC's addresses (errno %d): this PC only\n", LDN_CTRL_PORT, sock_errno());
        L.me = 0, L.ctrl = udp_bound(0x7F000101u, LDN_CTRL_PORT, true), L.me = 1;
    }
    if (L.ctrl < 0) { LOG("[ldn] no control socket (errno %d)\n", sock_errno()); return false; }
    pthread_t th;
    pthread_create(&th, NULL, ctrl_thread, NULL);
    pthread_detach(th);
    LOG("[ldn] this copy is local player %d (%s)\n", L.me, L.me == 1 ? "LAN and this PC" : "this PC only");
    return true;
}

static void host_create(const NetworkConfig *nc, const SecurityConfig *sc, const UserConfig *uc) {
    uint16_t adv_size = L.info.ldn.adv_size;  // (the advertise data set before the network is made stays: Pia reads
    uint8_t adv[sizeof L.info.ldn.adv];        // it back with GetNetworkInfo and sends that copy again later)
    memcpy(adv, L.info.ldn.adv, sizeof adv);
    memset(&L.info, 0, sizeof L.info);
    memset(L.real, 0, sizeof L.real);
    L.info.ldn.adv_size = adv_size, memcpy(L.info.ldn.adv, adv, sizeof adv);
    L.info.id.intent = nc->intent;
    L.info.id.intent.local_id = local_id(nc->intent.local_id);
    for (int i = 0; i < 16; i++) L.info.id.session[i] = (uint8_t)rand();
    L.info.common.mac[0] = 2, L.info.common.mac[4] = (uint8_t)rand(), L.info.common.mac[5] = (uint8_t)L.me;
    L.info.common.ssid.len = 32;
    for (int i = 0; i < 32; i++) L.info.common.ssid.name[i] = "0123456789abcdef"[rand() & 15];
    L.info.common.channel = nc->channel ? nc->channel : 1, L.info.common.link = 3, L.info.common.type = 2;
    L.info.ldn.sec_mode = sc->sec_mode;
    memcpy(L.info.ldn.sec_param, L.info.id.session, 16);
    L.info.ldn.max_nodes = nc->max_nodes ? nc->max_nodes : NODES;
    if (L.info.ldn.max_nodes > NODES) L.info.ldn.max_nodes = NODES;
    NodeInfo *me = &L.info.ldn.nodes[0];
    me->ip = node_vip(0), me->node_id = 0, me->connected = 1, me->version = nc->version;
    memcpy(me->mac, L.info.common.mac, 6), memcpy(me->name, uc->name, 32);
    L.info.ldn.nodes_n = 1;
    L.node = 0, L.real[0] = my_addr();
    LOG("[ldn] hosting (local id %016llx, scene %u, up to %d players)\n", (unsigned long long)L.info.id.intent.local_id,
        nc->intent.scene_id, L.info.ldn.max_nodes);
}

// where a scan asks besides this PC's other copies (slot 1 only): the LAN's broadcast addresses, and
// LARECOMP_LDN_PEERS=<ip>[,<ip>...] (other PCs by address: networks that drop broadcasts)
static int lan_targets(uint32_t *out, int max) {
    int n = 0;
    out[n++] = 0xFFFFFFFFu;
#ifndef _WIN32
    struct ifaddrs *ifs = NULL;
    if (!getifaddrs(&ifs)) {
        for (struct ifaddrs *i = ifs; i && n < max; i = i->ifa_next)
            if (i->ifa_addr && i->ifa_addr->sa_family == AF_INET && (i->ifa_flags & IFF_BROADCAST) && i->ifa_broadaddr &&
                !(i->ifa_flags & IFF_LOOPBACK))
                out[n++] = ntohl(((struct sockaddr_in *)i->ifa_broadaddr)->sin_addr.s_addr);
        freeifaddrs(ifs);
    }
#endif
    const char *e = getenv("LARECOMP_LDN_PEERS");
    for (char buf[256], *tok = e ? (snprintf(buf, sizeof buf, "%s", e), strtok(buf, ", ")) : NULL; tok && n < max; tok = strtok(NULL, ", ")) {
        struct in_addr a;
        if (inet_pton(AF_INET, tok, &a) == 1) out[n++] = ntohl(a.s_addr);
    }
    return n;
}

// a scan: ask the other copies (this PC's, the LAN's); the networks that match the filter
static int scan(const ScanFilter *f, NetworkInfo *out, int max) {
    if (max < 1 || !L.me) return 0;
    int fd = udp_bound(bind_addr(), 0, false);
    if (fd < 0) return 0;
    for (int k = 1; k <= LDN_PEERS; k++)
        if (k != L.me) ctrl_send(fd, 0x7F000100u | (uint32_t)k, 'S', NULL, 0);
    if (L.me == 1) {
        uint32_t t[24];
        int nt = lan_targets(t, 24);
        for (int i = 0; i < nt; i++) ctrl_send(fd, t[i], 'S', NULL, 0);
    }
    int n = 0;
    pthread_mutex_lock(&L.lock);
    L.nfound = 0;
    pthread_mutex_unlock(&L.lock);
    uint64_t end = plat_now_ns() + 250000000ull;
    kernel_block_begin();
    for (uint64_t now; n < max && (now = plat_now_ns()) < end;) {
        struct pollfd p = {fd, POLLIN, 0};
        if (sock_poll(&p, 1, (int)((end - now) / 1000000) + 1) <= 0) break;
        uint8_t m[8 + sizeof(NetworkInfo)];
        struct sockaddr_in from;
        socklen_t fl = sizeof from;
        ssize_t got = recvfrom(fd, (void *)m, sizeof m, 0, (struct sockaddr *)&from, &fl);
        if (got < 5 + (ssize_t)sizeof(NetworkInfo) || memcmp(m, "LDN1s", 5)) continue;
        NetworkInfo ni;
        memcpy(&ni, m + 5, sizeof ni);
        uint32_t src = canon(ntohl(from.sin_addr.s_addr));
        int dup = -1;
        pthread_mutex_lock(&L.lock);
        for (int i = 0; i < L.nfound; i++)
            if (!memcmp(L.found[i].session, ni.id.session, 16)) dup = i;
        if (dup >= 0) {  // (the same host twice: its loopback address is the better one)
            if (LOOPBACK(src)) L.found[dup].addr = src;
            pthread_mutex_unlock(&L.lock);
            continue;
        }
        if (L.nfound < NODES) memcpy(L.found[L.nfound].session, ni.id.session, 16), L.found[L.nfound++].addr = src;
        pthread_mutex_unlock(&L.lock);
        bool ok = true;
        if (f->flags & 1) ok &= ni.id.intent.local_id == local_id(f->id.intent.local_id);  // LocalCommunicationId
        if (f->flags & 2) ok &= !memcmp(ni.id.session, f->id.session, 16);                // SessionId
        if (f->flags & 4) ok &= ni.common.type == f->type;                                 // NetworkType
        if (f->flags & 0x20) ok &= ni.id.intent.scene_id == f->id.intent.scene_id;         // SceneId
        char sb[16];
        if (ldn_log()) LOG("[ldn]   found a network at %s: adv %u bytes, %u/%u players\n", ip_str(src, sb), ni.ldn.adv_size,
                           ni.ldn.nodes_n, ni.ldn.max_nodes);
        if (ok) out[n++] = ni;
    }
    kernel_block_end();
    close_sock(fd);
    return n;
}

// joining a network found by the last scan: ask its host; true with our node, its info and its nodes' addresses
static bool station_connect(const NetworkInfo *want, const UserConfig *uc, uint16_t version) {
    uint32_t host = 0;
    pthread_mutex_lock(&L.lock);
    for (int i = 0; i < L.nfound; i++)
        if (!memcmp(L.found[i].session, want->id.session, 16)) host = L.found[i].addr;
    pthread_mutex_unlock(&L.lock);
    if (!L.me || !host) return false;
    int fd = udp_bound(bind_addr(), 0, false);
    if (fd < 0) return false;
    uint8_t body[33 + 2];
    memcpy(body, uc->name, 33), memcpy(body + 33, &version, 2);
    ctrl_send(fd, host, 'C', body, sizeof body);
    struct pollfd p = {fd, POLLIN, 0};
    bool ok = false;
    kernel_block_begin();
    if (sock_poll(&p, 1, 1500) > 0) {
        uint8_t m[8 + sizeof(NetworkInfo) + 4 * NODES];
        ssize_t got = recv(fd, (void *)m, sizeof m, 0);
        if (got >= 6 + (ssize_t)(sizeof(NetworkInfo) + 4 * NODES) && !memcmp(m, "LDN1A", 5)) {
            pthread_mutex_lock(&L.lock);
            L.node = m[5], L.host_addr = host;
            memcpy(&L.info, m + 6, sizeof L.info);
            station_table(m + 6 + sizeof L.info);
            pthread_mutex_unlock(&L.lock);
            ok = true;
        }
    }
    kernel_block_end();
    close_sock(fd);
    char sb[16];
    if (ok) LOG("[ldn] joined \"%s\"'s network at %s as node %d\n", L.info.ldn.nodes[0].name, ip_str(host, sb), L.node);
    return ok;
}
static void leave(void) {  // (L.lock held) a station out / the host's network closed
    if (L.state == ST_STATION_CONNECTED && L.node > 0) {
        uint8_t k = (uint8_t)L.node;
        ctrl_send(L.ctrl, L.host_addr, 'L', &k, 1);
    }
    if (L.state == ST_AP_CREATED)
        for (int k = 1; k < NODES; k++)
            if (L.info.ldn.nodes[k].connected && L.real[k]) ctrl_send(L.ctrl, L.real[k], 'D', NULL, 0);
    L.node = -1, L.host_addr = 0;
    memset(&L.info, 0, sizeof L.info);
    memset(L.real, 0, sizeof L.real);
}

static void log_guest_stack(void) {  // (the guest calls that led here: debugging)
    Cpu *cc = kernel_current_cpu();
    uint64_t fp = cc ? cc->x[29] : 0;
    LOG("[ldn]   from %llx:", cc ? (unsigned long long)cc->x[30] : 0ull);
    for (int i = 0; i < 12 && fp >= GUEST_LO && fp < GUEST_HI && !(fp & 7); i++) {
        LOG(" %llx", (unsigned long long)((uint64_t *)(uintptr_t)fp)[1]);
        fp = ((uint64_t *)(uintptr_t)fp)[0];
    }
    LOG("\n");
}

static void write_buf(IpcBuf *b, const void *p, size_t n) {
    if (b && b->addr) memcpy((void *)(uintptr_t)b->addr, p, n < b->size ? n : (size_t)b->size);
}

// ------------------------------------------------------------------ ldn:u
static void ldn_user(Service *s, IpcReq *req, IpcResp *resp) {
    (void)s;
    if (ldn_log()) LOG("[ldn] cmd %u (state %d)\n", req->cmd, L.state);
    pthread_mutex_lock(&L.lock);
    switch (req->cmd) {
    case 0: OUT(uint32_t, 0) = (uint32_t)L.state, resp_size(resp, 4); break;  // GetState
    case 1: write_buf(out_buf(req, 0), &L.info, sizeof L.info); break;         // GetNetworkInfo
    case 2:                                                                     // GetIpv4Address: address, netmask
        OUT(uint32_t, 0) = L.node >= 0 ? my_vip() : 0, OUT(uint32_t, 4) = 0xFFFFFF00u, resp_size(resp, 8);
        break;
    case 3: OUT(uint16_t, 0) = L.disconnect_reason, resp_size(resp, 4); break;  // GetDisconnectReason
    case 4:                                                                       // GetSecurityParameter
        memcpy(resp->data, L.info.ldn.sec_param, 16), memcpy(resp->data + 16, L.info.id.session, 16), resp_size(resp, 0x20);
        break;
    case 5: {  // GetNetworkConfig
        NetworkConfig nc = {0};
        nc.intent = L.info.id.intent, nc.channel = L.info.common.channel, nc.max_nodes = L.info.ldn.max_nodes;
        nc.version = L.info.ldn.nodes[0].version;
        memcpy(resp->data, &nc, sizeof nc), resp_size(resp, sizeof nc);
        break;
    }
    case 100: {  // AttachStateChangeEvent
        uint32_t w, r;
        L.event = kernel_create_event_pair(&w, &r);
        resp->copy_handles[resp->ncopy++] = r;
        break;
    }
    case 101: {  // GetNetworkInfoLatestUpdate: the info, then what changed for each node (cleared)
        write_buf(out_buf(req, 0), &L.info, sizeof L.info);
        IpcBuf *u = out_buf(req, 1);
        if (u && u->addr) {
            uint8_t up[NODES][16] = {{0}};
            for (int k = 0; k < NODES; k++) up[k][0] = L.changes[k];
            write_buf(u, up, sizeof up);
        }
        memset(L.changes, 0, sizeof L.changes);
        break;
    }
    case 102: case 103: {  // Scan / ScanPrivate: u16 channel, pad, ScanFilter -> the networks found
        ScanFilter f;
        memcpy(&f, req->data + 8, sizeof f);
        IpcBuf *b = out_buf(req, 0);
        NetworkInfo found[NODES];
        int n = 0, max = b ? (int)(b->size / sizeof(NetworkInfo)) : 0;
        if (max > 0) {
            pthread_mutex_unlock(&L.lock);  // (the scan waits for answers)
            n = scan(&f, found, max < NODES ? max : NODES);
            pthread_mutex_lock(&L.lock);
            write_buf(b, found, (size_t)n * sizeof(NetworkInfo));
        }
        OUT(uint16_t, 0) = (uint16_t)n, resp_size(resp, 4);
        LOG("[ldn] scan: %d network%s\n", n, n == 1 ? "" : "s");
        if (ldn_log()) {
            LOG("[ldn]   filter flags %x local %016llx scene %u type %u; data %u bytes; b %d c %d x %d a %d (size %llu)\n", f.flags,
                (unsigned long long)f.id.intent.local_id, f.id.intent.scene_id, f.type, req->data_size, req->nb, req->nc, req->nx, req->na,
                b ? (unsigned long long)b->size : 0ull);
            log_guest_stack();
        }
        break;
    }
    case 200: L.state = ST_AP, signal_change(); break;                            // OpenAccessPoint
    case 201: leave(), L.state = ST_INIT, signal_change(); break;                 // CloseAccessPoint
    case 202: case 203: {  // CreateNetwork(SecurityConfig, UserConfig, pad, NetworkConfig) / ...Private (+ a security parameter)
        SecurityConfig sc;
        UserConfig uc;
        NetworkConfig nc;
        size_t o = req->cmd == 202 ? 0x44 : 0x64;
        memcpy(&sc, req->data, sizeof sc), memcpy(&uc, req->data + o, sizeof uc), memcpy(&nc, req->data + o + 0x30 + 4, sizeof nc);
        host_create(&nc, &sc, &uc);
        L.state = ST_AP_CREATED;
        signal_change();
        break;
    }
    case 204: leave(), L.state = ST_AP, signal_change(); break;  // DestroyNetwork
    case 206: {                                                  // SetAdvertiseData
        IpcBuf *b = in_buf(req, 0);
        size_t n = b ? (size_t)b->size : 0;
        if (n > sizeof L.info.ldn.adv) n = sizeof L.info.ldn.adv;
        if (n && b->addr) memcpy(L.info.ldn.adv, (const void *)(uintptr_t)b->addr, n);
        L.info.ldn.adv_size = (uint16_t)n;
        if (ldn_log()) {
            LOG("[ldn] advertise data (%zu bytes; a %d x %d, a0 %llu x0 %llu, data %u):", n, req->na, req->nx,
                req->na ? (unsigned long long)req->a[0].size : 0ull, req->nx ? (unsigned long long)req->x[0].size : 0ull, req->data_size);
            for (size_t i = 0; i < n; i++) LOG("%s%02x", i % 32 ? "" : "\n[ldn]   ", L.info.ldn.adv[i]);
            LOG("\n");
            log_guest_stack();
        }
        if (L.state == ST_AP_CREATED) host_broadcast_info();
        break;
    }
    case 207: L.info.ldn.accept_policy = IN(uint8_t, 0); break;  // SetStationAcceptPolicy
    case 300: L.state = ST_STATION, signal_change(); break;      // OpenStation
    case 301: leave(), L.state = ST_INIT, signal_change(); break;  // CloseStation
    case 302: case 303: {  // Connect(SecurityConfig, UserConfig, version, option; NetworkInfo) / ConnectPrivate
        UserConfig uc;
        memcpy(&uc, req->data + 0x44, sizeof uc);
        uint16_t version = (uint16_t)IN(uint32_t, 0x74);
        IpcBuf *b = in_buf(req, 0);
        NetworkInfo want = {0};
        if (b && b->addr && b->size >= sizeof want) memcpy(&want, (const void *)(uintptr_t)b->addr, sizeof want);
        pthread_mutex_unlock(&L.lock);
        bool ok = station_connect(&want, &uc, version);
        pthread_mutex_lock(&L.lock);
        if (ok) L.state = ST_STATION_CONNECTED, L.disconnect_reason = 0, signal_change();
        else resp->result = MAKE_RESULT(203, 64), LOG("[ldn] couldn't join the network\n");  // (ConnectFailure)
        break;
    }
    case 304: leave(), L.state = ST_STATION, signal_change(); break;  // Disconnect
    case 400: case 402: if (attach()) L.state = ST_INIT; break;        // Initialize / InitializeWithVersion
    case 401: leave(), L.state = ST_NONE; break;                      // Finalize
    default: LOG("[ldn] IUserLocalCommunicationService cmd %u (ok)\n", req->cmd);
    }
    pthread_mutex_unlock(&L.lock);
}

SERVICE("ldn:u", ldn_u) {
    (void)s;
    if (req->cmd == 0) ipc_return_object(resp, service_new("IUserLocalCommunicationService", ldn_user, NULL)), LOG("[ldn] service opened\n");
    else if (req->cmd == 1) ipc_return_object(resp, service_new("IClientProcessMonitor", client_monitor, NULL));
    else LOG("[ldn] ldn:u cmd %u (ok)\n", req->cmd);
}

// ------------------------------------------------------------------ bsd:u: real UDP within the network
// The game's descriptors are its own numbers (from 3) mapped to host sockets. A socket can be made only while this
// copy is in a network; the network's virtual addresses are turned into this PC's loopback ones both ways.
#define SOCKS 64
static int g_sock[SOCKS];  // host fd + 1 (0: free)

static int sock_host(int fd) { return fd >= 0 && fd < SOCKS && g_sock[fd] ? g_sock[fd] - 1 : -1; }
static bool g_nonblock[SOCKS];  // (O_NONBLOCK as the game set it: Winsock can't be asked)

// guest sockaddr (BSD layout: len, family, port BE, addr BE) <-> host. The network's addresses (169.254.1.x) are the
// nodes' real ones; any / our own is where this copy binds. false: out of reach (a node we have no address for)
static bool to_host(const uint8_t *g, size_t n, struct sockaddr_in *h) {
    memset(h, 0, sizeof *h);
    if (n < 8) return false;
    h->sin_family = AF_INET;
    memcpy(&h->sin_port, g + 2, 2);
    uint32_t ip;
    memcpy(&ip, g + 4, 4);
    ip = ntohl(ip);
    pthread_mutex_lock(&L.lock);
    int k = vip_node(ip);
    if (ip == 0 || (k >= 0 && k == L.node)) ip = bind_addr();
    else if (k >= 0) ip = L.real[k];
    pthread_mutex_unlock(&L.lock);
    h->sin_addr.s_addr = htonl(ip);
    return ip != 0 || k < 0;
}
static void to_guest(const struct sockaddr_in *h, uint8_t *g) {
    uint32_t ip = canon(ntohl(h->sin_addr.s_addr));
    pthread_mutex_lock(&L.lock);
    if (ip == 0 || ip == my_addr()) ip = my_vip();
    else
        for (int k = 0; k < NODES; k++)
            if (L.real[k] == ip) { ip = node_vip(k); break; }
    pthread_mutex_unlock(&L.lock);
    memset(g, 0, 16);
    g[0] = 16, g[1] = 2;
    memcpy(g + 2, &h->sin_port, 2);
    uint32_t be = htonl(ip);
    memcpy(g + 4, &be, 4);
}
static int host_flags(int f) {  // (BSD MSG_ values -> Linux)
    static int seen;
    if (ldn_log() && (f & ~seen)) seen |= f, LOG("[bsd] message flags %#x\n", f);
    return (f & 0x1 ? MSG_OOB : 0) | (f & 0x2 ? MSG_PEEK : 0) | (f & 0x4 ? MSG_DONTROUTE : 0) | (f & 0x40 ? MSG_WAITALL : 0)
#ifndef _WIN32
           | (f & 0x80 ? MSG_DONTWAIT : 0)
#endif
        ;
}
static bool broadcast(const struct sockaddr_in *h) {
    uint32_t ip = ntohl(h->sin_addr.s_addr);
    return ip == 0xFFFFFFFFu || ip == 0xA9FE01FFu;
}

static uint64_t g_sent[2], g_recvd[2];  // packets, bytes (LARECOMP_LDN_LOG: every 5 s)
static void traffic_log(void) {
    static uint64_t last;
    uint64_t now = plat_now_ns();
    if (now - last < 5000000000ull) return;
    last = now;
    LOG("[bsd] traffic: sent %llu packets (%llu bytes), received %llu (%llu bytes)\n", (unsigned long long)g_sent[0],
        (unsigned long long)g_sent[1], (unsigned long long)g_recvd[0], (unsigned long long)g_recvd[1]);
}

SERVICE("bsd:u", bsd_u) {
    (void)s;
    if (ldn_log() && req->cmd != 9 && req->cmd != 11 && req->cmd != 6) LOG("[bsd] cmd %u (%d bytes in)\n", req->cmd, (int)req->data_size);
    if (ldn_log()) traffic_log();
#define RET(v, e) (OUT(int32_t, 0) = (int32_t)(v), OUT(int32_t, 4) = (int32_t)(e), resp_size(resp, 8), \
                   ldn_log() && req->cmd != 9 && req->cmd != 11 && req->cmd != 6 ? LOG("[bsd]   -> %d (errno %d)\n", (int)(v), (int)(e)) : (void)0)
    switch (req->cmd) {
    case 0: OUT(uint64_t, 0) = 0, resp_size(resp, 8); break;  // RegisterClient
    case 2: case 3: {  // Socket(domain, type, protocol) / SocketExempt
        int dom = IN(int32_t, 0), type = IN(int32_t, 4);
        pthread_mutex_lock(&L.lock);
        bool in = L.state == ST_AP_CREATED || L.state == ST_STATION_CONNECTED;
        pthread_mutex_unlock(&L.lock);
        if (!in || dom != 2 || (type & 0xF) != 2) { RET(-1, 100); break; }  // (only UDP, only in a network: else ENETDOWN)
        int fd = 3;
        while (fd < SOCKS && g_sock[fd]) fd++;
        int h = fd < SOCKS ? udp_socket() : -1;
        if (h < 0) { RET(-1, 24); break; }  // EMFILE
        g_sock[fd] = h + 1, g_nonblock[fd] = false;
        RET(fd, 0);
        break;
    }
    case 6: {  // Poll(nfds, timeout; pollfd[] in, pollfd[] out)
        int nfds = IN(int32_t, 0), timeout = IN(int32_t, 4);
        IpcBuf *ib = in_buf(req, 0), *ob = out_buf(req, 0);
        if (nfds < 0 || nfds > 32 || !ib || !ib->addr) { RET(-1, 22); break; }
        struct { int32_t fd; int16_t ev, rev; } g[32];
        memcpy(g, (const void *)(uintptr_t)ib->addr, (size_t)nfds * 8);
        struct pollfd p[32];
        for (int i = 0; i < nfds; i++) p[i].fd = sock_host(g[i].fd), p[i].events = g[i].ev, p[i].revents = 0;
        kernel_block_begin();
        int r = sock_poll(p, (unsigned)nfds, timeout);
        kernel_block_end();
        for (int i = 0; i < nfds; i++) g[i].rev = p[i].revents;
        if (ob && ob->addr) write_buf(ob, g, (size_t)nfds * 8);
        RET(r, r < 0 ? sock_errno() : 0);
        break;
    }
    case 8: case 9: {  // Recv(fd, flags; data) / RecvFrom(fd, flags; data, addr)
        int h = sock_host(IN(int32_t, 0)), fl = IN(int32_t, 4);
        IpcBuf *b = out_buf(req, 0), *ab = req->cmd == 9 ? out_buf(req, 1) : NULL;
        if (h < 0 || !b) { RET(-1, 9); break; }  // EBADF
        int hf = host_flags(fl);
        struct sockaddr_in from;
        socklen_t fromlen = sizeof from;
        ssize_t n;
#ifdef _WIN32
        struct pollfd pr = {(SOCKET)h, POLLIN, 0};
        if ((fl & 0x80) && !g_nonblock[IN(int32_t, 0)] && sock_poll(&pr, 1, 0) == 0) {  // (MSG_DONTWAIT, nothing there)
            RET(-1, 11);
            break;
        }
#endif
        kernel_block_begin();
        n = recvfrom(h, (void *)(uintptr_t)b->addr, (int)b->size, hf, (struct sockaddr *)&from, &fromlen);
        kernel_block_end();
        RET(n, n < 0 ? sock_errno() : 0);
        if (n > 0) g_recvd[0]++, g_recvd[1] += (uint64_t)n;
        if (req->cmd == 9) {
            uint8_t ga[16];
            to_guest(&from, ga);
            if (n >= 0 && ab && ab->addr) write_buf(ab, ga, 16);
            OUT(uint32_t, 8) = n >= 0 ? 16 : 0, resp_size(resp, 12);
        }
        break;
    }
    case 10: case 11: {  // Send(fd, flags; data) / SendTo(fd, flags; data, addr)
        int h = sock_host(IN(int32_t, 0)), fl = IN(int32_t, 4);
        IpcBuf *b = in_buf(req, 0), *ab = req->cmd == 11 ? in_buf(req, 1) : NULL;
        if (h < 0 || !b) { RET(-1, 9); break; }
        int hf = host_flags(fl);
        struct sockaddr_in to;
        ssize_t n;
        if (ab && ab->addr) {
            if (!to_host((const uint8_t *)(uintptr_t)ab->addr, (size_t)ab->size, &to)) n = (ssize_t)b->size;  // (no way there: lost)
            else if (broadcast(&to)) {  // (the network's broadcast: to every other node, the same port)
                n = (ssize_t)b->size;
                pthread_mutex_lock(&L.lock);
                for (int k = 0; k < NODES; k++)
                    if (k != L.node && L.info.ldn.nodes[k].connected && L.real[k]) {
                        struct sockaddr_in t = to;
                        t.sin_addr.s_addr = htonl(L.real[k]);
                        sendto(h, (const void *)(uintptr_t)b->addr, (int)b->size, hf, (struct sockaddr *)&t, sizeof t);
                    }
                pthread_mutex_unlock(&L.lock);
            } else n = sendto(h, (const void *)(uintptr_t)b->addr, (int)b->size, hf, (struct sockaddr *)&to, sizeof to);
        } else n = send(h, (const void *)(uintptr_t)b->addr, (int)b->size, hf);
        if (n < 0) LOG("[bsd] send failed: errno %d\n", sock_errno());
        else g_sent[0]++, g_sent[1] += (uint64_t)n;
        RET(n, n < 0 ? sock_errno() : 0);
        break;
    }
    case 13: {  // Bind(fd; addr): where this copy binds (slot 1: every address; else 127.0.1.N), the game's port
        int h = sock_host(IN(int32_t, 0));
        IpcBuf *ab = in_buf(req, 0);
        struct sockaddr_in a;
        if (h < 0 || !ab || !ab->addr || ab->size < 8) { RET(-1, 22); break; }
        to_host((const uint8_t *)(uintptr_t)ab->addr, (size_t)ab->size, &a);
        pthread_mutex_lock(&L.lock);
        a.sin_addr.s_addr = htonl(bind_addr());
        pthread_mutex_unlock(&L.lock);
        int one = 1;
        setsockopt(h, SOL_SOCKET, SO_REUSEADDR, (const void *)&one, sizeof one);  // (slot 1's port is the others' too)
        setsockopt(h, SOL_SOCKET, SO_BROADCAST, (const void *)&one, sizeof one);
        int r = bind(h, (struct sockaddr *)&a, sizeof a);
        if (ldn_log()) LOG("[bsd] bind %s:%u\n", inet_ntoa(a.sin_addr), ntohs(a.sin_port));
        if (r) LOG("[bsd] bind %s:%u: errno %d\n", inet_ntoa(a.sin_addr), ntohs(a.sin_port), sock_errno());
        RET(r, r ? sock_errno() : 0);
        break;
    }
    case 14: {  // Connect(fd; addr)
        int h = sock_host(IN(int32_t, 0));
        IpcBuf *ab = in_buf(req, 0);
        struct sockaddr_in a;
        if (h < 0 || !ab || !ab->addr || !to_host((const uint8_t *)(uintptr_t)ab->addr, (size_t)ab->size, &a)) { RET(-1, 22); break; }
        int r = connect(h, (struct sockaddr *)&a, sizeof a);
        RET(r, r ? sock_errno() : 0);
        break;
    }
    case 15: case 16: {  // GetPeerName / GetSockName(fd; addr)
        int h = sock_host(IN(int32_t, 0));
        struct sockaddr_in a;
        socklen_t al = sizeof a;
        int r = h < 0 ? -1 : req->cmd == 15 ? getpeername(h, (struct sockaddr *)&a, &al) : getsockname(h, (struct sockaddr *)&a, &al);
        uint8_t ga[16];
        if (!r) to_guest(&a, ga), write_buf(out_buf(req, 0), ga, 16);
        RET(r, r ? (h < 0 ? 9 : sock_errno()) : 0);
        OUT(uint32_t, 8) = r ? 0 : 16, resp_size(resp, 12);
        break;
    }
    case 17: {  // GetSockOpt: zeros
        IpcBuf *b = out_buf(req, 0);
        if (b && b->addr) memset((void *)(uintptr_t)b->addr, 0, (size_t)b->size);
        RET(0, 0);
        OUT(uint32_t, 8) = b ? (uint32_t)b->size : 0, resp_size(resp, 12);
        break;
    }
    case 20: {  // Fcntl(fd, cmd, arg): F_GETFL 3 / F_SETFL 4, O_NONBLOCK 0x800
        int h = sock_host(IN(int32_t, 0)), c = IN(int32_t, 4), arg = IN(int32_t, 8);
        if (h < 0) { RET(-1, 9); break; }
        int gfd = IN(int32_t, 0);
        if (c == 3) RET(g_nonblock[gfd] ? 0x800 : 0, 0);
        else if (c == 4) {
            bool nb = (arg & 0x800) != 0;
#ifdef _WIN32
            u_long v = nb;
            int r = ioctlsocket((SOCKET)h, FIONBIO, &v);
#else
            int f = fcntl(h, F_GETFL, 0);
            int r = fcntl(h, F_SETFL, nb ? (f | O_NONBLOCK) : (f & ~O_NONBLOCK));
#endif
            if (!r) g_nonblock[gfd] = nb;
            RET(r ? -1 : 0, r ? sock_errno() : 0);
        }
        else RET(0, 0);
        break;
    }
    case 21: {  // SetSockOpt(fd, level, name; value): the ones that matter for UDP, the rest accepted
        int h = sock_host(IN(int32_t, 0)), level = IN(int32_t, 4), name = IN(int32_t, 8);
        IpcBuf *b = in_buf(req, 0);
        int v = b && b->addr && b->size >= 4 ? *(const int32_t *)(uintptr_t)b->addr : 0;
        if (h >= 0 && level == 0xFFFF) {
            if (name == 0x20) setsockopt(h, SOL_SOCKET, SO_BROADCAST, (const void *)&v, sizeof v);
            else if (name == 0x4) setsockopt(h, SOL_SOCKET, SO_REUSEADDR, (const void *)&v, sizeof v);
            else if (name == 0x1001) setsockopt(h, SOL_SOCKET, SO_SNDBUF, (const void *)&v, sizeof v);
            else if (name == 0x1002) setsockopt(h, SOL_SOCKET, SO_RCVBUF, (const void *)&v, sizeof v);
        }
        RET(h < 0 ? -1 : 0, h < 0 ? 9 : 0);
        break;
    }
    case 22: RET(0, 0); break;  // Shutdown
    case 26: {                  // Close(fd)
        int fd = IN(int32_t, 0), h = sock_host(fd);
        if (h >= 0) close_sock(h), g_sock[fd] = 0;
        RET(h < 0 ? -1 : 0, h < 0 ? 9 : 0);
        break;
    }
#ifndef _WIN32
    case 31: {  // EventFd(initval, flags)
        int fd = 3;
        while (fd < SOCKS && g_sock[fd]) fd++;
        int h = fd < SOCKS ? eventfd((unsigned)IN(uint64_t, 0), 0) : -1;
        if (h < 0) { RET(-1, 24); break; }
        g_sock[fd] = h + 1;
        RET(fd, 0);
        break;
    }
    case 24: case 25: {  // Write / Read (an eventfd)
        int h = sock_host(IN(int32_t, 0));
        IpcBuf *b = req->cmd == 24 ? in_buf(req, 0) : out_buf(req, 0);
        if (h < 0 || !b || !b->addr) { RET(-1, 9); break; }
        kernel_block_begin();
        ssize_t n = req->cmd == 24 ? write(h, (const void *)(uintptr_t)b->addr, (size_t)b->size) : read(h, (void *)(uintptr_t)b->addr, (size_t)b->size);
        kernel_block_end();
        RET(n, n < 0 ? sock_errno() : 0);
        break;
    }
#else
    case 24: case 25: case 31: RET(-1, 38); break;  // (no event fds: ENOSYS)
#endif
    default: LOG("[bsd] bsd:u cmd %u (ok)\n", req->cmd); RET(0, 0);
    }
#undef RET
}

// (bsd:s: the same service for system modules - Pia's sockets come through it)
SERVICE("bsd:s", bsd_s) { bsd_u(s, req, resp); }
