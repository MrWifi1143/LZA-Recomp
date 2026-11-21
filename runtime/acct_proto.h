// Online server accounts and save sync: the protocol between the game (runtime/acct.c, PC and PS5) and the server
// (contrib/lzaserver/lzaserver_accounts.c). Shared by both; plain C, no runtime headers.
//
// TCP over TLS 1.3, on the server's port number (the same number as its UDP Link Play port: 39200 by default). The
// server's certificate is its own (made at its first start, self-signed) or the operator's (from a certificate
// authority). The game pins it on first use (the SHA-256 of its public key, per server address): a different key later
// is refused unless a certificate authority vouches for it (runtime/acct.c).
//
// Every message, both ways: u32 header length, u32 body length (little-endian), the header (text, one line, fields
// separated by single spaces), the body (bytes; empty for most). Requests may follow each other on one connection.
//   CREATE <name> <password hex> <device>        -> OK <token>                  (a new account, signed in)
//   LOGIN <name> <password hex> <device>         -> OK <token>
//   LOGOUT <token>                               -> OK                          (that session ends on the server)
//   WHOAMI <token>                               -> OK <name>
//   INFO <token> <game>                          -> OK <save>  |  OK 0          (the latest save of that game, or none)
//   GET <token> <game> <version>                 -> OK <save>, body = the save's blob
//   PUT <token> <game> <base> <counter> <saved> <device> <content hash> <blob sha256>, body = blob
//                                                -> OK <save>  |  CONFLICT <save>   (base: the version this upload
//                                                   replaces - not the latest: refused, nothing changes)
//   LIST <token> <game>                          -> OK <n>, body = n lines "<save>" (newest first: the versions kept)
//   JOIN <token>                                 -> OK <ticket> <key> <seconds>   (a play ticket: Link Play / the
//                                                   shared world through this server need one - see below)
// <save> = <version> <counter> <saved> <device> <content hash> <blob sha256> <size> <uploaded>
// Errors: ERR <code> <text...>; codes: bad, wrong (name or password), taken, closed (no new accounts), wait (too many
// tries: <text> starts with the seconds), signin (the token isn't valid: sign in again), big, quota, full, corrupt, server.
// <name>: 3-24 of a-z 0-9 _ . -; password: 8-256 bytes; <device>: 1-24 of A-Z a-z 0-9 _ -; <game>: the program id,
// 16 lowercase hex digits; <saved>, <uploaded>: unix seconds (the save's own time on the device that made it /
// when the server got it); <counter>: the uploading device's own count of its uploads; hashes: 64 lowercase hex digits.
// <token>: "<name>:<64 hex>" - the server keeps only its SHA-256.
//
// Play tickets (2026-11-17): Link Play and the shared world through an lzaserver need an account there (unless it runs
// with --no-accounts / --open-play). The game asks for a ticket over TLS (JOIN): <ticket> = 16 hex digits (an id, not
// secret), <key> = 32 hex digits (secret: it never goes over UDP). Each UDP hello then carries the ticket and a MAC:
//   'H' ver(1) id(8) game(1) cookie(8) ticket(8) mac(8)          (35 bytes; mac = acct_siphash(key, the first 27 bytes))
// The cookie is the address's (lzaserver.c: per address, id and minute), so a MAC overheard can't be replayed from
// elsewhere or later. The server answers a hello that echoed its cookie with 'A' status(1): 0 in (signed in as the
// ticket's account, or the server needs no account), 1 sign-in needed (no ticket), 2 the ticket isn't known (expired,
// the server restarted, the session signed out: the game asks for a new one). A client not "in" gets no lobby or shared-
// world traffic and its own is dropped. Tickets live in the server's memory for ACCT_TICKET_S after they're made.
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define ACCT_PROTO_VERSION 1
#define ACCT_DEFAULT_PORT 39200
#define ACCT_MAX_HEADER 2048
#define ACCT_NAME_MIN 3
#define ACCT_NAME_MAX 24
#define ACCT_PASS_MIN 8
#define ACCT_PASS_MAX 256
#define ACCT_DEVICE_MAX 24
#define ACCT_TOKEN_MAX (ACCT_NAME_MAX + 1 + 64)
#define ACCT_BLOB_MAGIC "LZSV"  // (a save blob: runtime/acct.c pack_save)
#define ACCT_TICKET_S (24 * 3600)
#define ACCT_HELLO_AUTH_LEN 35

// SipHash-2-4 with a 16-byte key (the hello's MAC; shared by the game's relay plugin and the server)
static inline uint64_t acct_siphash(const uint8_t key[16], const void *msg, size_t n) {
#define ACCT_ROTL(x, b) (uint64_t)(((x) << (b)) | ((x) >> (64 - (b))))
#define ACCT_SIPROUND do { v0 += v1; v1 = ACCT_ROTL(v1, 13); v1 ^= v0; v0 = ACCT_ROTL(v0, 32); v2 += v3; v3 = ACCT_ROTL(v3, 16); \
    v3 ^= v2; v0 += v3; v3 = ACCT_ROTL(v3, 21); v3 ^= v0; v2 += v1; v1 = ACCT_ROTL(v1, 17); v1 ^= v2; v2 = ACCT_ROTL(v2, 32); } while (0)
    const uint8_t *m = (const uint8_t *)msg;
    uint64_t k0, k1, w;
    memcpy(&k0, key, 8), memcpy(&k1, key + 8, 8);
    uint64_t v0 = 0x736f6d6570736575ull ^ k0, v1 = 0x646f72616e646f6dull ^ k1;
    uint64_t v2 = 0x6c7967656e657261ull ^ k0, v3 = 0x7465646279746573ull ^ k1;
    uint64_t b = (uint64_t)n << 56;
    size_t i = 0;
    for (; i + 8 <= n; i += 8) {
        memcpy(&w, m + i, 8);
        v3 ^= w;
        ACCT_SIPROUND;
        ACCT_SIPROUND;
        v0 ^= w;
    }
    for (size_t k = 0; i + k < n; k++) b |= (uint64_t)m[i + k] << (8 * k);
    v3 ^= b;
    ACCT_SIPROUND;
    ACCT_SIPROUND;
    v0 ^= b, v2 ^= 0xff;
    for (int r = 0; r < 4; r++) ACCT_SIPROUND;
    return v0 ^ v1 ^ v2 ^ v3;
#undef ACCT_SIPROUND
#undef ACCT_ROTL
}
