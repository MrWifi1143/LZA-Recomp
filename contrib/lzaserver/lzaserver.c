// lzaserver: an online server for PL:A Recomp / PL:Z-A Recomp's local play (Link Trades, Private Battles) - anyone can
// run one; players put its address in the launcher (Options > Online) and switch online play on. Players who
// enter the same Link Code in the same game meet in a lobby here, and the server passes their games' traffic between
// them (the game's own "Nearby" play, carried over the internet). Nothing about the players is kept: a client is an
// address and a random id, forgotten 15 s after its last packet. No Nintendo service is involved or imitated.
//
//   lzaserver [--port 39200] [--max-clients 256] [--bind ADDR]      (UDP; open / forward that port)
//             [--world-crowd 16] [--world-near 120]                 (shared world: interest management, below)
//
// Accounts and save sync (lzaserver_accounts.c, protocol in runtime/acct_proto.h): TCP + TLS on the same port number -
// open / forward TCP too. Playing HERE needs an account here (Link Play by Link Code, the shared world): a client whose
// hello doesn't carry a valid play ticket (acct_proto.h) is told 'A' 1 "sign in" and gets nothing passed on - the game
// says "Sign in to <server> to join". Nobody needs an account to play the game itself, offline or nearby. Signed-in
// players' saves are kept here and picked up by another copy of the game (another PC, the PS5 port). Options:
//   --data DIR            accounts, saves and the server's own certificate (default ./lzaserver-data)
//   --cert FILE --key FILE  your own certificate (PEM, e.g. from Let's Encrypt) instead of the server's own
//   --signup open|closed  new accounts from the game (default open); closed: --add-user NAME (password on stdin)
//   --save-max-mb 32 --save-versions 10 --quota-mb 256 --max-accounts 1000
//   --open-play           accounts (and save sync) kept, but anyone may play here - no sign-in needed
//   --no-accounts         no accounts at all: anyone may play here, the old open server - e.g. at home on a LAN (also:
//                         build with -DLZA_NO_ACCOUNTS, then this file alone is the server)
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
//                     ... ticket[8] mac[8]                    signed in: a play ticket + MAC (35 bytes, acct_proto.h)
//   server -> client  'A' status(1)                           (accounts required) the answer to a hello with the
//                                                             cookie: 0 in, 1 sign in to play here, 2 ticket unknown
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
#include <math.h>
#include <time.h>
#ifndef LZA_NO_ACCOUNTS
#include "acct_proto.h"
#include "lzaserver_accounts.h"
#endif
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
// Interest management (2026-11-17): every shared-world packet went to every other player of the same game - fine for a
// handful, O(N^2) for a public server (50 players: ~49k packets a second out, ~1000 a second into each player, who keeps
// 32 heard and shows the 8 nearest within 55 m). With more than --world-crowd players (default 16) in one game's world, a
// player's position packets (~20 a second) go at full rate only to players within --world-near metres (default 120) of
// them; players farther away get one every 2 s (they still know where everyone is, and someone coming closer is at full
// rate well before the 55 m where the game shows them). Everything else (outfits, looks, acknowledgements, trade asks)
// goes to everyone as before. Up to the crowd size nothing changes. A player whose position isn't known (none sent in
// 10 s: in a menu, a loading screen) gets everything.
#define WORLD_CROWD 16
#define WORLD_NEAR_M 120.0f
#define WORLD_FAR_MS 2000
#define WORLD_POS_MS 10000

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
    bool in;                    // (signed in: its hello carried a play ticket's MAC - or the server needs no account)
    uint8_t auth_said;          // (the last 'A' status sent, + 1: logged on change)
    char user[32];              // (the ticket's account)
    float pos[3];               // (shared world: where its player was, from its last position packet - interest management)
    uint64_t pos_at, far_next;  // (when that came; when its next position packet may go to players far from it)
} Client;

static Client *g_c;
static int g_max = 256;
static sock_t g_s;
static uint64_t g_fwd_packets, g_fwd_bytes, g_world_packets, g_trimmed, g_world_held;
static int g_world_crowd = WORLD_CROWD, g_world_n[4];  // (players in each game's shared world, counted every second)
static float g_world_near = WORLD_NEAR_M;
static bool world_far(const Client *a, const Client *b, uint64_t now) {  // (both placed lately, and farther apart than near)
    if (!a->pos_at || !b->pos_at || now - a->pos_at > WORLD_POS_MS || now - b->pos_at > WORLD_POS_MS) return false;
    float dx = a->pos[0] - b->pos[0], dy = a->pos[1] - b->pos[1], dz = a->pos[2] - b->pos[2];
    return dx * dx + dy * dy + dz * dz > g_world_near * g_world_near;
}
// Accounts required to play here (the default with accounts; --no-accounts / --open-play: anyone) - acct_proto.h
static bool g_need_auth;
#define PLAYS(c) (!g_need_auth || (c)->in)

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

// A hello that echoed its address's cookie (so 'A' goes to whoever really is there): signed in? Its play ticket's MAC
// over the hello (acct_proto.h) - each hello again: a ticket signed out or expired takes the player out within 2 s
static void hello_auth(Client *c, const uint8_t *b, int n) {
    if (!g_need_auth) return;
    uint8_t st = 1;
    char user[32] = "";
#ifndef LZA_NO_ACCOUNTS
    if (n >= ACCT_HELLO_AUTH_LEN) {
        uint8_t key[16];
        uint64_t mac;
        memcpy(&mac, b + 27, 8);
        st = 2;
        if (accounts_ticket(b + 19, key, user, sizeof user) && acct_siphash(key, b, 27) == mac) st = 0;
        memset(key, 0, sizeof key);
    }
#else
    (void)b, (void)n;
#endif
    bool was = c->in;
    c->in = st == 0;
    if (c->in) snprintf(c->user, sizeof c->user, "%s", user);
    uint8_t a[2] = {'A', st};
    send_to(c, a, sizeof a);
    if (c->auth_said != st + 1 || was != c->in) {
        printf("  %016llx %s%s\n", (unsigned long long)c->id, st == 0 ? "signed in as " : st == 1 ? "not signed in: told to sign in (can't play here)"
               : "its play ticket isn't known (expired / signed out / a restart): told", st == 0 ? c->user : "");
        c->auth_said = st + 1;
    }
    if (was && !c->in) c->code[0] = 0, c->world = false;  // (out: its lobby and the shared world too)
}

static void members(void) {  // (each lobby member: the others, with their hosting flag)
    static uint8_t b[2 + 9 * 64];
    for (int i = 0; i < g_max; i++) {
        Client *c = &g_c[i];
        if (!c->used || !c->code[0] || !PLAYS(c)) continue;
        int n = 0;
        b[0] = 'M';
        for (int j = 0; j < g_max && n < 64; j++) {
            Client *o = &g_c[j];
            if (j == i || !o->used || !PLAYS(o) || o->game != c->game || strcmp(o->code, c->code)) continue;
            memcpy(b + 2 + 9 * n, &o->id, 8), b[2 + 9 * n + 8] = o->hosting;
            n++;
        }
        b[1] = (uint8_t)n;
        send_to(c, b, 2 + 9 * (size_t)n);
    }
}

int main(int argc, char **argv) {
    int port = 39200;
    const char *bind_addr = "";
#ifndef LZA_NO_ACCOUNTS
    AccountsOptions ao = {0};
    bool accounts = true;
    const char *add_user = NULL;
    bool open_play = false;
    snprintf(ao.data, sizeof ao.data, "lzaserver-data");
    ao.signup = true, ao.save_max_mb = 32, ao.versions = 10, ao.quota_mb = 256, ao.max_accounts = 1000;
#define ARG(name) (!strcmp(argv[i], name) && i + 1 < argc)
#endif
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--port") && i + 1 < argc) port = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--max-clients") && i + 1 < argc) g_max = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--bind") && i + 1 < argc) bind_addr = argv[++i];
        else if (!strcmp(argv[i], "--world-crowd") && i + 1 < argc) g_world_crowd = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--world-near") && i + 1 < argc) g_world_near = (float)atof(argv[++i]);
#ifndef LZA_NO_ACCOUNTS
        else if (ARG("--data")) snprintf(ao.data, sizeof ao.data, "%s", argv[++i]);
        else if (ARG("--cert")) snprintf(ao.cert, sizeof ao.cert, "%s", argv[++i]);
        else if (ARG("--key")) snprintf(ao.key, sizeof ao.key, "%s", argv[++i]);
        else if (ARG("--signup")) ao.signup = strcmp(argv[++i], "closed") != 0;
        else if (ARG("--save-max-mb")) ao.save_max_mb = atoi(argv[++i]);
        else if (ARG("--save-versions")) ao.versions = atoi(argv[++i]);
        else if (ARG("--quota-mb")) ao.quota_mb = atoi(argv[++i]);
        else if (ARG("--max-accounts")) ao.max_accounts = atoi(argv[++i]);
        else if (ARG("--add-user")) add_user = argv[++i];
        else if (!strcmp(argv[i], "--no-accounts")) accounts = false;
        else if (!strcmp(argv[i], "--open-play")) open_play = true;
#endif
        else {
            fprintf(stderr, "lzaserver [--port 39200] [--max-clients 256] [--bind ADDR] [--world-crowd 16] [--world-near 120]\n"
#ifndef LZA_NO_ACCOUNTS
                            "          [--data DIR] [--cert FILE --key FILE] [--signup open|closed] [--save-max-mb 32]\n"
                            "          [--save-versions 10] [--quota-mb 256] [--max-accounts 1000] [--no-accounts | --open-play]\n"
                            "lzaserver [--data DIR] --add-user NAME     (the password: one line on stdin)\n"
#endif
            );
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
#ifndef LZA_NO_ACCOUNTS
    if (add_user) return accounts_add_user(&ao, add_user);
#endif
    g_s = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = htons((uint16_t)port), .sin_addr.s_addr = htonl(INADDR_ANY)};
    if (bind_addr[0]) a.sin_addr.s_addr = inet_addr(bind_addr);
    if (g_s == NO_SOCK || bind(g_s, (struct sockaddr *)&a, sizeof a)) {
        fprintf(stderr, "lzaserver: can't use UDP port %d\n", port);
        return 1;
    }
    printf("lzaserver %d: UDP port %d, up to %d players (cookies: forged addresses get nothing amplified)\n", VERSION, port, g_max);
#ifndef LZA_NO_ACCOUNTS
    if (accounts) {
        ao.port = port;
        snprintf(ao.bind, sizeof ao.bind, "%s", bind_addr);
        if (ao.save_max_mb < 1) ao.save_max_mb = 1;
        if (ao.versions < 1) ao.versions = 1;
        if (accounts_start(&ao)) return 1;
        g_need_auth = !open_play;
        printf(g_need_auth ? "lzaserver: playing here needs an account on this server (--open-play / --no-accounts: anyone)\n"
                           : "lzaserver: anyone may play here (--open-play); accounts keep saves only\n");
    } else printf("lzaserver: no accounts (--no-accounts): anyone may play here\n");
#endif
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
                            // (signed in again from there: ids are public - the lobby lists - so a moved id is "in" only
                            // with its play ticket's MAC over the new address's cookie)
                            bool was_in = dup->in;
                            dup->in = false;
                            if (echoed) hello_auth(dup, buf, n);
                            if (g_need_auth && was_in && !dup->in) dup->code[0] = 0, dup->world = false;
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
                    if (c->id == id && echoed) hello_auth(c, buf, n);
                    continue;
                }
                if (!c || c->id != id) continue;  // (unknown, or speaking for another)
                c->seen = now;
                earn(c, n);
                if (!PLAYS(c)) continue;  // (not signed in: nothing it sends goes anywhere)
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
                    // (a position packet: type 2, its x y z at payload +16 - runtime/hle/warp.c WorldPkt)
                    bool is_pos = n - 9 >= 28 && buf[9] == 2;
                    if (is_pos) {
                        float p[3];
                        memcpy(p, buf + 9 + 16, sizeof p);
                        if (p[0] == p[0] && p[1] == p[1] && p[2] == p[2] && fabsf(p[0]) < 1e6f && fabsf(p[1]) < 1e6f && fabsf(p[2]) < 1e6f)
                            memcpy(c->pos, p, sizeof p), c->pos_at = now ? now : 1;
                    }
                    bool crowd = is_pos && c->game < 4 && g_world_n[c->game] > g_world_crowd, far_due = now >= c->far_next, far_sent = false;
                    for (int i = 0; i < g_max; i++) {
                        Client *t = &g_c[i];
                        if (t == c || !t->used || !PLAYS(t) || !t->world || t->game != c->game) continue;
                        if (crowd && world_far(c, t, now)) {
                            if (!far_due) { g_world_held++; continue; }
                            far_sent = true;
                        }
                        send_to(t, out, (size_t)n);
                        g_world_packets++;
                    }
                    if (far_sent) c->far_next = now + WORLD_FAR_MS;
                } else if (buf[0] == 'D' && n >= 17) {
                    uint64_t to;
                    memcpy(&to, buf + 9, 8);
                    Client *t = by_id(to);
                    if (!t || !PLAYS(t) || !c->code[0] || t->game != c->game || strcmp(t->code, c->code)) continue;  // (only within a lobby)
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
            memset(g_world_n, 0, sizeof g_world_n);
            for (int i = 0; i < g_max; i++)
                if (g_c[i].used && g_c[i].world && PLAYS(&g_c[i]) && g_c[i].game < 4) g_world_n[g_c[i].game]++;
        }
        if (now - last_stats >= 60000) {
            int nc = 0;
            for (int i = 0; i < g_max; i++) nc += g_c[i].used;
            if (nc || g_fwd_packets || g_world_packets)
                printf("  %d players, %llu packets (%llu bytes) passed on in lobbies, %llu in the shared world in the last minute%s\n",
                       nc, (unsigned long long)g_fwd_packets, (unsigned long long)g_fwd_bytes, (unsigned long long)g_world_packets,
                       g_trimmed ? " (some kept from unverified addresses)" : "");
            if (g_trimmed) printf("  %llu bytes not sent to unverified addresses (over what they sent)\n", (unsigned long long)g_trimmed);
            if (g_world_held) printf("  %llu shared-world position packets not sent to players far away (a crowd: --world-crowd)\n",
                                     (unsigned long long)g_world_held);
            g_fwd_packets = g_fwd_bytes = g_world_packets = g_trimmed = g_world_held = 0, last_stats = now;
        }
    }
}
