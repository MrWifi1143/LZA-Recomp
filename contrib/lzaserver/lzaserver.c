// lzaserver: an online server for PL:A Recomp / PL:Z-A Recomp's local play (Link Trades, Private Battles) - anyone can
// run one; players put its address in the launcher (Mods > Online server) and switch the relay plugin on. Players who
// enter the same Link Code in the same game meet in a lobby here, and the server passes their games' traffic between
// them (the game's own "Nearby" play, carried over the internet). Nothing about the players is kept: a client is an
// address and a random id, forgotten 15 s after its last packet. No Nintendo service is involved or imitated.
//
//   lzaserver [--port 39200] [--max-clients 256]      (UDP; open / forward that port)
//
// Protocol (UDP, every packet starts with its type byte; ids are u64 little-endian, picked at random by each client):
//   client -> server  'H' ver(1) id game(1: Arceus, 2: Z-A)   hello / keep-alive, every 2 s
//                     'L' id code[8]                          join the lobby of that Link Code (8 ASCII digits)
//                     'P' id hosting(1)                       the client's game hosts a network / no longer
//                     'D' id to payload                       pass payload to another client of the same lobby
//                     'W' id on(1)                            in the shared world or not (opt-in, game setting)
//                     'X' id payload (up to 96 bytes)         to everyone in the shared world of the same game (where
//                                                             the player is, ~20 times a second: players see each
//                                                             other walking around)
//   server -> client  'M' n (id hosting(1)) x n               the lobby's other members, every second
//                     'D' from payload                        a lobby member's payload
//                     'X' from payload                        a shared-world player's payload
// A packet whose id isn't the one that address said hello with is dropped (no speaking for others).
// An id's hello from a new address is taken once the old address has been quiet 5 s (a home router's new port).
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET sock_t;
#define NO_SOCK INVALID_SOCKET
static uint64_t now_ms(void) { return GetTickCount64(); }
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
typedef int sock_t;
#define NO_SOCK (-1)
static uint64_t now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000 + (uint64_t)t.tv_nsec / 1000000;
}
#endif

#define VERSION 1
#define IDLE_MS 15000
#define REBIND_MS 5000  // (an id whose address has been quiet this long may come back from another address)
#define MAX_PER_SEC 3000  // (packets a client may send per second; more are dropped)

typedef struct {
    bool used;
    uint64_t id;
    struct sockaddr_in addr;
    uint8_t game, hosting;
    bool world;    // (in the shared world: 'W')
    char code[9];  // ("" not in a lobby)
    uint64_t seen, window;
    uint32_t count;
} Client;

static Client *g_c;
static int g_max = 256;
static sock_t g_s;
static uint64_t g_fwd_packets, g_fwd_bytes, g_world_packets;

static const char *who(const Client *c, char *b) {
    snprintf(b, 64, "%s:%u", inet_ntoa(c->addr.sin_addr), ntohs(c->addr.sin_port));
    return b;
}
static bool same_addr(const struct sockaddr_in *a, const struct sockaddr_in *b) {
    return a->sin_addr.s_addr == b->sin_addr.s_addr && a->sin_port == b->sin_port;
}
static Client *by_addr(const struct sockaddr_in *a) {
    for (int i = 0; i < g_max; i++)
        if (g_c[i].used && same_addr(&g_c[i].addr, a)) return &g_c[i];
    return NULL;
}
static Client *by_id(uint64_t id) {
    for (int i = 0; i < g_max; i++)
        if (g_c[i].used && g_c[i].id == id) return &g_c[i];
    return NULL;
}
static void send_to(const Client *c, const void *p, size_t n) {
    sendto(g_s, (const char *)p, (int)n, 0, (const struct sockaddr *)&c->addr, sizeof c->addr);
}

static void members(void) {  // (each lobby member: the others, with their hosting flag)
    static uint8_t b[2 + 9 * 64];
    for (int i = 0; i < g_max; i++) {
        Client *c = &g_c[i];
        if (!c->used || !c->code[0]) continue;
        int n = 0;
        b[0] = 'M';
        for (int j = 0; j < g_max && n < 64; j++) {
            Client *o = &g_c[j];
            if (j == i || !o->used || o->game != c->game || strcmp(o->code, c->code)) continue;
            memcpy(b + 2 + 9 * n, &o->id, 8), b[2 + 9 * n + 8] = o->hosting;
            n++;
        }
        b[1] = (uint8_t)n;
        send_to(c, b, 2 + 9 * (size_t)n);
    }
}

int main(int argc, char **argv) {
    int port = 39200;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--port") && i + 1 < argc) port = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--max-clients") && i + 1 < argc) g_max = atoi(argv[++i]);
        else {
            fprintf(stderr, "lzaserver [--port 39200] [--max-clients 256]\n");
            return 2;
        }
    }
    if (g_max < 2) g_max = 2;
    g_c = calloc((size_t)g_max, sizeof *g_c);
#ifdef _WIN32
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
    setvbuf(stdout, NULL, _IOLBF, 0);
    g_s = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = htons((uint16_t)port), .sin_addr.s_addr = htonl(INADDR_ANY)};
    if (g_s == NO_SOCK || bind(g_s, (struct sockaddr *)&a, sizeof a)) {
        fprintf(stderr, "lzaserver: can't use UDP port %d\n", port);
        return 1;
    }
    printf("lzaserver %d: UDP port %d, up to %d players\n", VERSION, port, g_max);
    static uint8_t buf[65536], out[65536];
    uint64_t last_list = 0, last_stats = now_ms();
    for (;;) {
        fd_set rd;
        FD_ZERO(&rd);
        FD_SET(g_s, &rd);
        struct timeval tv = {0, 200000};
        int r = select((int)g_s + 1, &rd, NULL, NULL, &tv);
        uint64_t now = now_ms();
        if (r > 0 && FD_ISSET(g_s, &rd)) {
            struct sockaddr_in from;
            socklen_t fl = sizeof from;
            int n = (int)recvfrom(g_s, (char *)buf, sizeof buf, 0, (struct sockaddr *)&from, &fl);
            if (n >= 9) {
                uint64_t id;
                memcpy(&id, buf + (buf[0] == 'H' ? 2 : 1), 8);
                Client *c = by_addr(&from);
                if (buf[0] == 'H' && n >= 11 && buf[1] == VERSION) {
                    if (!c) {
                        Client *dup = by_id(id);
                        // (an id already in use elsewhere: refused - unless that address has been quiet for REBIND_MS. A
                        // player's home router can give them a new port (a NAT mapping renewed, Wi-Fi to cable): they keep
                        // their id and lobby instead of being shut out until the old entry is forgotten 15 s later. A live
                        // client says hello every 2 s, so a quiet one is gone: taking its id over can't cut anyone off)
                        if (dup && !same_addr(&dup->addr, &from)) {
                            if (now - dup->seen < REBIND_MS) continue;
                            char w0[64], w1[64];
                            snprintf(w0, sizeof w0, "%s", who(dup, w1));
                            dup->addr = from, dup->seen = now;
                            printf("~ %016llx moved from %s to %s\n", (unsigned long long)id, w0, who(dup, w1));
                            continue;
                        }
                        for (int i = 0; i < g_max && !c; i++)
                            if (!g_c[i].used) c = &g_c[i];
                        if (!c) continue;  // (full)
                        memset(c, 0, sizeof *c);
                        c->used = true, c->id = id, c->addr = from, c->game = buf[10];
                        char w[64];
                        printf("+ %016llx %s (%s)\n", (unsigned long long)id, who(c, w), c->game == 1 ? "Arceus" : c->game == 2 ? "Z-A" : "?");
                    }
                    c->seen = now;
                    continue;
                }
                if (!c || c->id != id) continue;  // (unknown, or speaking for another)
                c->seen = now;
                if (now - c->window >= 1000) c->window = now, c->count = 0;
                if (++c->count > MAX_PER_SEC) continue;
                if (buf[0] == 'L' && n >= 17) {
                    char code[9];
                    memcpy(code, buf + 9, 8), code[8] = 0;
                    if (strspn(code, "0123456789") != 8) continue;
                    if (strcmp(code, c->code)) {
                        memcpy(c->code, code, 9);
                        printf("  %016llx in a lobby (%s)\n", (unsigned long long)id, c->game == 1 ? "Arceus" : "Z-A");
                    }
                } else if (buf[0] == 'P' && n >= 10) {
                    c->hosting = buf[9] ? 1 : 0;
                } else if (buf[0] == 'W' && n >= 10) {
                    bool on = buf[9] != 0;
                    if (on != c->world) printf("  %016llx %s the shared world (%s)\n", (unsigned long long)id, on ? "in" : "out of",
                                               c->game == 1 ? "Arceus" : "Z-A");
                    c->world = on;
                } else if (buf[0] == 'X' && n >= 10 && n <= 9 + 96) {
                    if (!c->world) continue;
                    out[0] = 'X';
                    memcpy(out + 1, &id, 8);
                    memcpy(out + 9, buf + 9, (size_t)n - 9);
                    for (int i = 0; i < g_max; i++) {
                        Client *t = &g_c[i];
                        if (t == c || !t->used || !t->world || t->game != c->game) continue;
                        send_to(t, out, (size_t)n);
                        g_world_packets++;
                    }
                } else if (buf[0] == 'D' && n >= 17) {
                    uint64_t to;
                    memcpy(&to, buf + 9, 8);
                    Client *t = by_id(to);
                    if (!t || !c->code[0] || t->game != c->game || strcmp(t->code, c->code)) continue;  // (only within a lobby)
                    out[0] = 'D';
                    memcpy(out + 1, &id, 8);
                    memcpy(out + 9, buf + 17, (size_t)n - 17);
                    send_to(t, out, (size_t)n - 8);
                    g_fwd_packets++, g_fwd_bytes += (uint64_t)n - 17;
                }
            }
        }
        if (now - last_list >= 1000) {
            last_list = now;
            for (int i = 0; i < g_max; i++)
                if (g_c[i].used && now - g_c[i].seen > IDLE_MS) {
                    printf("- %016llx (gone)\n", (unsigned long long)g_c[i].id);
                    g_c[i].used = false;
                }
            members();
        }
        if (now - last_stats >= 60000) {
            int nc = 0;
            for (int i = 0; i < g_max; i++) nc += g_c[i].used;
            if (nc || g_fwd_packets || g_world_packets)
                printf("  %d players, %llu packets (%llu bytes) passed on in lobbies, %llu in the shared world in the last minute\n",
                       nc, (unsigned long long)g_fwd_packets, (unsigned long long)g_fwd_bytes, (unsigned long long)g_world_packets);
            g_fwd_packets = g_fwd_bytes = g_world_packets = 0, last_stats = now;
        }
    }
}
