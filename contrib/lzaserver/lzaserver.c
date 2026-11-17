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
//   server -> client  'C' cookie[8]                           the answer to every hello: this address's cookie
//   client -> server  'H' ver(1) id game(1) cookie[8]         a hello with the cookie last heard (19 bytes)
// A packet whose id isn't the one that address said hello with is dropped (no speaking for others).
// An id's hello from a new address is taken once the old address has been quiet 5 s (a home router's new port) - with
// that address's cookie if the id's old address had one.
//
// Return routability (2026-11-17): a UDP sender's address can be forged, so a hello "from" someone else's address would
// have the server stream lobby lists and shared-world traffic (up to players x 20 packets a second) at them - a
// reflection / amplification attack. Every hello is answered with a cookie (9 bytes: never more than the hello's 11):
// a keyed hash (SipHash-2-4, a secret made at start) of the address, the id and the minute. Only whoever really
// receives at that address can echo it: a client whose hello carries its address's cookie (this minute's or the last
// one's) is VERIFIED and gets everything. Until then - and for good for an older client, which never echoes one - the
// server sends an address at most as many bytes as it has received from it (a byte budget, capped at 64 KB): no packet
// it sends there is ever amplified. Older clients keep working (their hellos and game traffic earn their budget; a
// busy shared world may be thinned for them) and are logged once as such. Older servers ignore the 8 extra bytes of a
// new client's hello and never send 'C', so a new client works with them as before.
#ifdef _WIN32
#define _CRT_RAND_S  // (rand_s: the cookie key)
#endif
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
#define BUDGET_MAX 65536  // (bytes an unverified address may have in hand: what it sent, not yet answered)
#define OLD_CLIENT_MS 10000  // (no cookie echoed this long after its first hello: logged once as an older client)

typedef struct {
    bool used;
    uint64_t id;
    struct sockaddr_in addr;
    uint8_t game, hosting;
    bool world;    // (in the shared world: 'W')
    char code[9];  // ("" not in a lobby)
    uint64_t seen, window, since;
    uint32_t count;
    bool verified, old_logged;  // (its hello echoed its address's cookie: see above)
    uint32_t budget;            // (unverified: bytes it may still be sent - what it sent, less what it was sent)
    uint64_t trimmed;           // (bytes not sent to it for want of budget, for the log)
} Client;

static Client *g_c;
static int g_max = 256;
static sock_t g_s;
static uint64_t g_fwd_packets, g_fwd_bytes, g_world_packets, g_trimmed;

// ---- cookies: SipHash-2-4 (keyed hash) of {address, port, id, minute} with a secret made at start
static uint64_t g_key[2];
#define ROTL(x, b) (uint64_t)(((x) << (b)) | ((x) >> (64 - (b))))
#define SIPROUND do { v0 += v1; v1 = ROTL(v1, 13); v1 ^= v0; v0 = ROTL(v0, 32); v2 += v3; v3 = ROTL(v3, 16); v3 ^= v2; \
    v0 += v3; v3 = ROTL(v3, 21); v3 ^= v0; v2 += v1; v1 = ROTL(v1, 17); v1 ^= v2; v2 = ROTL(v2, 32); } while (0)
static uint64_t siphash(const uint8_t *m, size_t n) {
    uint64_t v0 = 0x736f6d6570736575ull ^ g_key[0], v1 = 0x646f72616e646f6dull ^ g_key[1];
    uint64_t v2 = 0x6c7967656e657261ull ^ g_key[0], v3 = 0x7465646279746573ull ^ g_key[1];
    uint64_t b = (uint64_t)n << 56, w;
    size_t i = 0;
    for (; i + 8 <= n; i += 8) {
        memcpy(&w, m + i, 8);
        v3 ^= w;
        SIPROUND;
        SIPROUND;
        v0 ^= w;
    }
    for (size_t k = 0; i + k < n; k++) b |= (uint64_t)m[i + k] << (8 * k);
    v3 ^= b;
    SIPROUND;
    SIPROUND;
    v0 ^= b, v2 ^= 0xff;
    for (int r = 0; r < 4; r++) SIPROUND;
    return v0 ^ v1 ^ v2 ^ v3;
}
static uint64_t cookie(const struct sockaddr_in *a, uint64_t id, uint64_t minute) {
    uint8_t m[22];
    memcpy(m, &a->sin_addr.s_addr, 4), memcpy(m + 4, &a->sin_port, 2), memcpy(m + 6, &id, 8), memcpy(m + 14, &minute, 8);
    return siphash(m, sizeof m);
}
static bool cookie_ok(const struct sockaddr_in *a, uint64_t id, const uint8_t *c, uint64_t now) {
    uint64_t v, minute = now / 60000;
    memcpy(&v, c, 8);
    return v == cookie(a, id, minute) || v == cookie(a, id, minute - 1);
}
static void make_key(void) {
    bool ok = false;
#ifdef _WIN32
    unsigned int r;
    ok = true;
    for (int i = 0; i < 4; i++) {
        if (rand_s(&r)) { ok = false; break; }
        memcpy((uint8_t *)g_key + 4 * i, &r, 4);
    }
#else
    FILE *f = fopen("/dev/urandom", "rb");
    ok = f && fread(g_key, 1, sizeof g_key, f) == sizeof g_key;
    if (f) fclose(f);
#endif
    if (!ok) {  // (no system randomness: the clock and this process's addresses - weaker, said so)
        uint64_t x = now_ms() ^ (uint64_t)(uintptr_t)&x ^ (uint64_t)time(NULL) << 20;
        for (int i = 0; i < 2; i++) {
            x += 0x9E3779B97F4A7C15ull;
            uint64_t z = x;
            z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull, z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
            g_key[i] = z ^ (z >> 31);
        }
        fprintf(stderr, "lzaserver: no system random source - cookies keyed from the clock\n");
    }
}

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
static void send_to(Client *c, const void *p, size_t n) {
    if (!c->verified) {  // (an address that hasn't shown it is really there: never more than it sent)
        if (c->budget < n) { c->trimmed += n, g_trimmed += n; return; }
        c->budget -= (uint32_t)n;
    }
    sendto(g_s, (const char *)p, (int)n, 0, (const struct sockaddr *)&c->addr, sizeof c->addr);
}
static void earn(Client *c, int n) {  // (an unverified address sent n bytes: that much may go back)
    if (!c->verified) c->budget = c->budget + (uint32_t)n > BUDGET_MAX ? BUDGET_MAX : c->budget + (uint32_t)n;
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
    make_key();
    setvbuf(stdout, NULL, _IOLBF, 0);
    g_s = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = htons((uint16_t)port), .sin_addr.s_addr = htonl(INADDR_ANY)};
    if (g_s == NO_SOCK || bind(g_s, (struct sockaddr *)&a, sizeof a)) {
        fprintf(stderr, "lzaserver: can't use UDP port %d\n", port);
        return 1;
    }
    printf("lzaserver %d: UDP port %d, up to %d players (cookies: forged addresses get nothing amplified)\n", VERSION, port, g_max);
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
                    // (the answer: this address's cookie - 9 bytes for a hello of 11 or more, so a forged one gets its
                    // "sender" less than it cost; it is also how a client knows the server is there)
                    uint8_t ck[9] = {'C'};
                    uint64_t v = cookie(&from, id, now / 60000);
                    memcpy(ck + 1, &v, 8);
                    sendto(g_s, (const char *)ck, sizeof ck, 0, (const struct sockaddr *)&from, sizeof from);
                    bool echoed = n >= 19 && cookie_ok(&from, id, buf + 11, now);
                    if (!c) {
                        Client *dup = by_id(id);
                        // (an id already in use elsewhere: refused - unless that address has been quiet for REBIND_MS. A
                        // player's home router can give them a new port (a NAT mapping renewed, Wi-Fi to cable): they keep
                        // their id and lobby instead of being shut out until the old entry is forgotten 15 s later. A live
                        // client says hello every 2 s, so a quiet one is gone: taking its id over can't cut anyone off.
                        // An id whose address was verified moves only to an address that is too - its cookie echoed)
                        if (dup && !same_addr(&dup->addr, &from)) {
                            if (now - dup->seen < REBIND_MS || (dup->verified && !echoed)) continue;
                            char w0[64], w1[64];
                            snprintf(w0, sizeof w0, "%s", who(dup, w1));
                            dup->addr = from, dup->seen = now, dup->verified = echoed, dup->budget = 0, dup->since = now;
                            earn(dup, n - (int)sizeof ck);
                            printf("~ %016llx moved from %s to %s%s\n", (unsigned long long)id, w0, who(dup, w1), echoed ? " (verified)" : "");
                            continue;
                        }
                        for (int i = 0; i < g_max && !c; i++)
                            if (!g_c[i].used) c = &g_c[i];
                        if (!c) continue;  // (full)
                        memset(c, 0, sizeof *c);
                        c->used = true, c->id = id, c->addr = from, c->game = buf[10], c->since = now;
                        char w[64];
                        printf("+ %016llx %s (%s)%s\n", (unsigned long long)id, who(c, w), c->game == 1 ? "Arceus" : c->game == 2 ? "Z-A" : "?",
                               echoed ? " verified" : "");
                    }
                    if (c->id == id && echoed && !c->verified) {
                        c->verified = true;
                        if (now - c->since > 1000 || c->old_logged)
                            printf("  %016llx verified (its cookie echoed)\n", (unsigned long long)id);
                    }
                    earn(c, n - (int)sizeof ck);  // (the cookie answer has already spent that much of it)
                    c->seen = now;
                    continue;
                }
                if (!c || c->id != id) continue;  // (unknown, or speaking for another)
                c->seen = now;
                earn(c, n);
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
            for (int i = 0; i < g_max; i++) {
                Client *c = &g_c[i];
                if (c->used && now - c->seen > IDLE_MS) {
                    printf("- %016llx (gone)%s\n", (unsigned long long)c->id, c->verified ? "" : " - never verified");
                    c->used = false;
                } else if (c->used && !c->verified && !c->old_logged && now - c->since > OLD_CLIENT_MS) {
                    c->old_logged = true;
                    char w[64];
                    printf("  %016llx %s: no cookie echoed - an older client (or a forged address): sent at most what it sends\n",
                           (unsigned long long)c->id, who(c, w));
                }
            }
            members();
        }
        if (now - last_stats >= 60000) {
            int nc = 0;
            for (int i = 0; i < g_max; i++) nc += g_c[i].used;
            if (nc || g_fwd_packets || g_world_packets)
                printf("  %d players, %llu packets (%llu bytes) passed on in lobbies, %llu in the shared world in the last minute%s\n",
                       nc, (unsigned long long)g_fwd_packets, (unsigned long long)g_fwd_bytes, (unsigned long long)g_world_packets,
                       g_trimmed ? " (some kept from unverified addresses)" : "");
            if (g_trimmed) printf("  %llu bytes not sent to unverified addresses (over what they sent)\n", (unsigned long long)g_trimmed);
            g_fwd_packets = g_fwd_bytes = g_world_packets = g_trimmed = 0, last_stats = now;
        }
    }
}
